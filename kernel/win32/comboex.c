#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>
#include <synth/string.h>

#include "w32gui.h"

/* ComboBoxEx32: a frame around an ordinary combo box child; items carry text and lParam, Enter/Escape report
   CBEN_ENDEDIT */
enum {
    CB_FIRST = 0x140,
    CB_DELETESTRING = 0x144,
    CB_GETCURSEL = 0x147,
    CB_GETLBTEXT = 0x148,
    CB_GETLBTEXTLEN = 0x149,
    CB_INSERTSTRING = 0x14a,
    CB_SETCURSEL = 0x14e,
    CB_GETITEMDATA = 0x150,
    CB_SETITEMDATA = 0x151,
    CB_LAST = 0x164,
    CBEM_INSERTITEMA = 0x401,
    CBEM_SETIMAGELIST = 0x402,
    CBEM_GETIMAGELIST = 0x403,
    CBEM_GETITEMA = 0x404,
    CBEM_SETITEMA = 0x405,
    CBEM_GETCOMBOCONTROL = 0x406,
    CBEM_GETEDITCONTROL = 0x407,
    CBEM_SETEXSTYLE = 0x408,
    CBEM_GETEXSTYLE = 0x409,
    CBEM_HASEDITCHANGED = 0x40a,
    CBEM_INSERTITEMW = 0x40b,
    CBEM_SETITEMW = 0x40c,
    CBEM_GETITEMW = 0x40d,
    CBEM_SETEXTENDEDSTYLE = 0x40e,
    CBEIF_TEXT = 0x1,
    CBEIF_LPARAM = 0x20,
    CCM_SETUNICODEFORMAT = 0x2005,
    CCM_GETUNICODEFORMAT = 0x2006,
    CBEN_ENDEDITA = -805,
    CBEN_ENDEDITW = -806,
    CBENF_RETURN = 2,
    CBENF_ESCAPE = 3,
    WM_NCDESTROY_ = 0x82,
    CBN_EDITCHANGE = 5,
};

struct cbex {
    struct w32_window *combo;
    HANDLE images;
    uint32_t exstyle;
    bool changed, wide_notify, format_set;
};

static struct cbex *cbex_of(struct w32_window *w)
{
    if (!w->list)
        w->list = w32_zalloc(sizeof(struct cbex));
    return w->list;
}

/* COMBOBOXEXITEM field offsets: 32-bit layout to the 64-bit one */
static int item_off(int off32)
{
    if (!w32->win64)
        return off32;
    return off32 <= 8 ? off32 * 2 : off32 < 32 ? off32 + 12 : 48;
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static LRESULT send_text(struct w32_window *c, UINT msg, WPARAM wp, const void *text, bool wide)
{
    if (!text || (uintptr_t)text == (uintptr_t)-1)
        text = wide ? (const void *)L"" : "";
    if (wide == c->unicode)
        return w32_send(c, msg, wp, (LPARAM)(uintptr_t)text);
    void *conv = wide ? (void *)w32_narrow(text) : (void *)w32_widen(text);
    LRESULT r = w32_send(c, msg, wp, (LPARAM)(uintptr_t)conv);
    w32_free(conv);
    return r;
}

/* copies text in the child's encoding into a caller buffer in the requested one */
static void copy_out(void *dst, int cch, const void *src, bool src_wide, bool dst_wide)
{
    if (!dst || cch <= 0)
        return;
    void *conv = NULL;
    if (src_wide != dst_wide)
        src = conv = src_wide ? (void *)w32_narrow(src) : (void *)w32_widen(src);
    if (dst_wide) {
        const WCHAR *s = src ? src : L"";
        int n = 0;
        while (n < cch - 1 && s[n])
            n++;
        memcpy(dst, s, (size_t)n * 2);
        ((WCHAR *)dst)[n] = 0;
    } else {
        str_copy(dst, src ? src : "", (size_t)cch);
    }
    w32_free(conv);
}

static LRESULT insert_item(struct cbex *s, uint8_t *it, bool wide)
{
    uint32_t mask = get32(it);
    int at = (int)get32(it + item_off(4));
    const void *text = (const void *)(uintptr_t)get32(it + item_off(8));
    LRESULT i = send_text(s->combo, CB_INSERTSTRING, (WPARAM)at, mask & CBEIF_TEXT ? text : NULL, wide);
    if (i >= 0 && (mask & CBEIF_LPARAM))
        w32_send(s->combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)get32(it + item_off(32)));
    return i;
}

static LRESULT set_item(struct w32_window *w, struct cbex *s, uint8_t *it, bool wide)
{
    uint32_t mask = get32(it);
    int at = (int)get32(it + item_off(4));
    const void *text = (const void *)(uintptr_t)get32(it + item_off(8));
    if (at < 0) {
        if (mask & CBEIF_TEXT)
            send_text(s->combo, WM_SETTEXT, 0, text, wide);
        w32_invalidate(w, NULL, true);
        return TRUE;
    }
    if (mask & CBEIF_TEXT) {
        LRESULT data = w32_send(s->combo, CB_GETITEMDATA, (WPARAM)at, 0);
        bool cur = w32_send(s->combo, CB_GETCURSEL, 0, 0) == at;
        w32_send(s->combo, CB_DELETESTRING, (WPARAM)at, 0);
        send_text(s->combo, CB_INSERTSTRING, (WPARAM)at, text, wide);
        w32_send(s->combo, CB_SETITEMDATA, (WPARAM)at, data);
        if (cur)
            w32_send(s->combo, CB_SETCURSEL, (WPARAM)at, 0);
    }
    if (mask & CBEIF_LPARAM)
        w32_send(s->combo, CB_SETITEMDATA, (WPARAM)at, (LPARAM)get32(it + item_off(32)));
    return TRUE;
}

static LRESULT get_item(struct cbex *s, uint8_t *it, bool wide)
{
    uint32_t mask = get32(it);
    int at = (int)get32(it + item_off(4));
    void *out = (void *)(uintptr_t)get32(it + item_off(8));
    int cch = (int)get32(it + item_off(12));
    bool cw = s->combo->unicode;
    if (mask & CBEIF_TEXT) {
        LRESULT len = at < 0 ? w32_send(s->combo, WM_GETTEXTLENGTH, 0, 0)
                             : w32_send(s->combo, CB_GETLBTEXTLEN, (WPARAM)at, 0);
        if (len < 0)
            return FALSE;
        void *tmp = w32_zalloc(((size_t)len + 1) * 4);
        if (!tmp)
            return FALSE;
        if (at < 0)
            w32_send(s->combo, WM_GETTEXT, (WPARAM)len + 1, (LPARAM)(uintptr_t)tmp);
        else
            w32_send(s->combo, CB_GETLBTEXT, (WPARAM)at, (LPARAM)(uintptr_t)tmp);
        copy_out(out, cch, tmp, cw, wide);
        w32_free(tmp);
    }
    if ((mask & CBEIF_LPARAM) && at >= 0) {
        uint32_t data = (uint32_t)w32_send(s->combo, CB_GETITEMDATA, (WPARAM)at, 0);
        memcpy(it + item_off(32), &data, 4);
        if (w32->win64)
            memset(it + item_off(32) + 4, 0, 4);
    }
    return TRUE;
}

/* the combo box child reports Enter/Escape; the parent hears CBEN_ENDEDIT with the field's text */
bool w32_comboex_key(struct w32_window *combo, WPARAM vk)
{
    struct w32_window *w = combo->parent;
    if (!w || !w->cls || w->cls->proc != w32_comboex_proc || !w->parent || (vk != 0x0d && vk != 0x1b))
        return false;
    struct cbex *s = cbex_of(w);
    if (!s)
        return false;
    bool wide = s->format_set ? s->wide_notify : w->unicode;
    int hs = w32->win64 ? 24 : 12, ps = w32->win64 ? 8 : 4, cs = wide ? 2 : 1;
    size_t size = (size_t)hs + 8 + 260 * (size_t)cs + 4;
    uint8_t *nm = w32_zalloc(size + 4);
    if (!nm)
        return false;
    uint64_t hwnd = (uintptr_t)w, id = (uint64_t)w->id;
    int code = wide ? CBEN_ENDEDITW : CBEN_ENDEDITA, why = vk == 0x0d ? CBENF_RETURN : CBENF_ESCAPE;
    int changed = s->changed, sel = (int)w32_send(combo, CB_GETCURSEL, 0, 0);
    memcpy(nm, &hwnd, (size_t)ps);
    memcpy(nm + ps, &id, (size_t)ps);
    memcpy(nm + 2 * ps, &code, 4);
    memcpy(nm + hs, &changed, 4);
    memcpy(nm + hs + 4, &sel, 4);
    void *tmp = w32_zalloc(260 * 4);
    if (tmp) {
        w32_send(combo, WM_GETTEXT, 260, (LPARAM)(uintptr_t)tmp);
        copy_out(nm + hs + 8, 260, tmp, combo->unicode, wide);
        w32_free(tmp);
    }
    size_t why_at = (size_t)hs + 8 + 260 * (size_t)cs;
    memcpy(nm + ((why_at + 3) & ~(size_t)3), &why, 4);
    LRESULT r = w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
    w32_free(nm);
    s->changed = false;
    return r != 0 || vk == 0x0d;
}

struct w32_window *w32_text_owner(struct w32_window *w)
{
    struct cbex *s = w->cls && w->cls->proc == w32_comboex_proc ? w->list : NULL;
    return s && s->combo ? s->combo : w;
}

int w32_comboex_height(struct w32_window *w)
{
    struct cbex *s = w->list;
    return s && s->combo ? s->combo->rect.h : -1;
}

LRESULT WINAPI w32_comboex_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct cbex *s = cbex_of(w);
    if (!s)
        return w32_def_window_proc(h, msg, wp, lp);
    uint8_t *ptr = (uint8_t *)(uintptr_t)(uint32_t)lp;
    if (msg == WM_CREATE) {
        uint32_t kind = w->style & 3 ? w->style & 3 : 2;
        HANDLE c = w32_create_window(0, "ComboBox", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | 0x40 | kind, 0, 0,
                                     w->rect.w, MAX(w->rect.h, 200), w, (HANDLE)w->id, NULL, NULL);
        s->combo = w32_win(c);
        if (!s->combo)
            return -1;
        s->combo->unicode = w->unicode;
        s->combo->font = w->font;
        w->rect.h = s->combo->rect.h;
        return 0;
    }
    if (!s->combo)
        return w32_def_window_proc(h, msg, wp, lp);
    if (msg >= CB_FIRST && msg <= CB_LAST)
        return w32_send_as(s->combo, msg, wp, lp, w->unicode);
    switch (msg) {
    case CBEM_INSERTITEMA:
    case CBEM_INSERTITEMW: return ptr ? insert_item(s, ptr, msg == CBEM_INSERTITEMW) : -1;
    case CBEM_SETITEMA:
    case CBEM_SETITEMW: return ptr ? set_item(w, s, ptr, msg == CBEM_SETITEMW) : FALSE;
    case CBEM_GETITEMA:
    case CBEM_GETITEMW: return ptr ? get_item(s, ptr, msg == CBEM_GETITEMW) : FALSE;
    case CBEM_SETIMAGELIST: {
        HANDLE old = s->images;
        s->images = (HANDLE)lp;
        return (LRESULT)old;
    }
    case CBEM_GETIMAGELIST: return (LRESULT)s->images;
    case CBEM_GETCOMBOCONTROL:
    case CBEM_GETEDITCONTROL: return (LRESULT)s->combo;
    case CBEM_SETEXSTYLE:
    case CBEM_SETEXTENDEDSTYLE: {
        uint32_t old = s->exstyle, mask = msg == CBEM_SETEXTENDEDSTYLE && wp ? (uint32_t)wp : ~0u;
        s->exstyle = (s->exstyle & ~mask) | ((uint32_t)lp & mask);
        return old;
    }
    case CBEM_GETEXSTYLE: return s->exstyle;
    case CBEM_HASEDITCHANGED: return s->changed;
    case CCM_SETUNICODEFORMAT: {
        bool old = s->format_set ? s->wide_notify : w->unicode;
        s->wide_notify = wp != 0;
        s->format_set = true;
        return old;
    }
    case CCM_GETUNICODEFORMAT: return s->format_set ? s->wide_notify : w->unicode;
    case WM_SETTEXT:
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH: {
        LRESULT r = w32_send_as(s->combo, msg, wp, lp, w->unicode);
        if (msg == WM_SETTEXT)
            s->changed = false;
        return r;
    }
    case WM_SETFONT:
        w->font = (HANDLE)wp;
        w32_send(s->combo, msg, wp, lp);
        w->rect.h = s->combo->rect.h;
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_SETFOCUS: w32_set_focus(s->combo); return 0;
    case WM_ENABLE: s->combo->enabled = wp != 0; return w32_send(s->combo, msg, wp, lp);
    case WM_SIZE: {
        typedef BOOL(WINAPI * move_fn)(HANDLE, int, int, int, int, BOOL);
        move_fn move = (move_fn)w32_builtin_lookup("user32.dll", "MoveWindow");
        if (move)
            move(s->combo, 0, 0, w->rect.w, s->combo->rect.h, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if ((struct w32_window *)(uintptr_t)lp == s->combo) {
            if ((wp >> 16) == CBN_EDITCHANGE)
                s->changed = true;
            if (w->parent)
                return w32_send(w->parent, WM_COMMAND, (WPARAM)((uint32_t)(uint16_t)w->id | (wp & 0xffff0000u)),
                                (LPARAM)(uintptr_t)w);
        }
        return 0;
    case WM_NOTIFY:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: return w->parent ? w32_send(w->parent, msg, wp, lp) : 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        struct w32_paint p;
        if (w32_ctl_begin(w, &p, WM_CTLCOLOREDIT))
            w32_ctl_end(w, &p);
        return 0;
    }
    case WM_NCDESTROY_:
        w32_free(s);
        w->list = NULL;
        return w32_def_window_proc(h, msg, wp, lp);
    default: return w32_def_window_proc(h, msg, wp, lp);
    }
}
#endif
