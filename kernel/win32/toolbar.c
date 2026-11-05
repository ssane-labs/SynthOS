#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>
#include <synth/string.h>

#include "w32gui.h"

/* ToolbarWindow32 and msctls_statusbar32; TBBUTTON/TBBUTTONINFO/NMHDR come in the caller's 32- or 64-bit layout */
enum {
    TB_ENABLEBUTTON = 0x401,
    TB_CHECKBUTTON = 0x402,
    TB_PRESSBUTTON = 0x403,
    TB_HIDEBUTTON = 0x404,
    TB_INDETERMINATE = 0x405,
    TB_ISBUTTONENABLED = 0x409,
    TB_ISBUTTONCHECKED = 0x40a,
    TB_ISBUTTONPRESSED = 0x40b,
    TB_ISBUTTONHIDDEN = 0x40c,
    TB_SETSTATE = 0x411,
    TB_GETSTATE = 0x412,
    TB_ADDBITMAP = 0x413,
    TB_ADDBUTTONSA = 0x414,
    TB_INSERTBUTTONA = 0x415,
    TB_DELETEBUTTON = 0x416,
    TB_GETBUTTON = 0x417,
    TB_BUTTONCOUNT = 0x418,
    TB_COMMANDTOINDEX = 0x419,
    TB_ADDSTRINGA = 0x41c,
    TB_GETITEMRECT = 0x41d,
    TB_BUTTONSTRUCTSIZE = 0x41e,
    TB_SETBUTTONSIZE = 0x41f,
    TB_SETBITMAPSIZE = 0x420,
    TB_AUTOSIZE = 0x421,
    TB_SETPARENT = 0x425,
    TB_GETROWS = 0x428,
    TB_SETCMDID = 0x42a,
    TB_GETBITMAP = 0x42c,
    TB_SETIMAGELIST = 0x430,
    TB_GETIMAGELIST = 0x431,
    TB_GETRECT = 0x433,
    TB_SETSTYLE = 0x438,
    TB_GETSTYLE = 0x439,
    TB_GETBUTTONSIZE = 0x43a,
    TB_SETBUTTONWIDTH = 0x43b,
    TB_GETBUTTONINFOW = 0x43f,
    TB_SETBUTTONINFOW = 0x440,
    TB_GETBUTTONINFOA = 0x441,
    TB_SETBUTTONINFOA = 0x442,
    TB_INSERTBUTTONW = 0x443,
    TB_ADDBUTTONSW = 0x444,
    TB_HITTEST = 0x445,
    TB_GETHOTITEM = 0x447,
    TB_ADDSTRINGW = 0x44d,
    TB_GETMAXSIZE = 0x453,
    TB_SETEXTENDEDSTYLE = 0x454,
    TB_GETEXTENDEDSTYLE = 0x455,
    TB_GETPADDING = 0x456,
    TB_SETPADDING = 0x457,
    TB_GETIDEALSIZE = 0x463,
    TBSTATE_CHECKED = 1,
    TBSTATE_PRESSED = 2,
    TBSTATE_ENABLED = 4,
    TBSTATE_HIDDEN = 8,
    TBSTYLE_SEP = 1,
    TBSTYLE_CHECK = 2,
    TBSTYLE_GROUP = 4,
    TBSTYLE_DROPDOWN = 8,
    TBSTYLE_LIST = 0x1000,
    TBIF_IMAGE = 1,
    TBIF_TEXT = 2,
    TBIF_STATE = 4,
    TBIF_STYLE = 8,
    TBIF_LPARAM = 0x10,
    TBIF_COMMAND = 0x20,
    TBIF_SIZE = 0x40,
    TBIF_BYINDEX = (int)0x80000000,
    TBN_DROPDOWN = -710,
    CCS_TOP = 1,
    CCS_BOTTOM = 3,
    CCS_NORESIZE = 4,
    CCS_NOPARENTALIGN = 8,
    CCS_NODIVIDER = 0x40,
    WM_NCDESTROY_ = 0x82,
    WM_LBUTTONUP_ = 0x202,
    WM_SETTEXT_ = 0x0c,
    WM_GETTEXT_ = 0x0d,
    WM_GETTEXTLENGTH_ = 0x0e,
    SB_SETTEXTA = 0x401,
    SB_GETTEXTA = 0x402,
    SB_GETTEXTLENGTHA = 0x403,
    SB_SETPARTS = 0x404,
    SB_GETPARTS = 0x406,
    SB_GETBORDERS = 0x407,
    SB_SETMINHEIGHT = 0x408,
    SB_SIMPLE = 0x409,
    SB_GETRECT = 0x40a,
    SB_SETTEXTW = 0x40b,
    SB_GETTEXTLENGTHW = 0x40c,
    SB_GETTEXTW = 0x40d,
    SB_ISSIMPLE = 0x40e,
    SB_SETICON = 0x40f,
    SB_GETICON = 0x414,
    MAX_PARTS = 32,
};

static int ptr_size(void)
{
    return w32->win64 ? 8 : 4;
}

static uintptr_t get_ptr(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int get32(const uint8_t *p)
{
    int v;
    memcpy(&v, p, 4);
    return v;
}

static void put_text(void *out, int cch, const char *t, bool wide)
{
    if (!out || cch <= 0)
        return;
    if (wide) {
        size_t n = w32_cp_to_wide(t, (int)strlen(t), (WCHAR *)out, (size_t)cch - 1, 65001);
        ((WCHAR *)out)[n] = 0;
    } else {
        str_copy(out, t, (size_t)cch);
    }
}

static char *take_text(uintptr_t p, bool wide)
{
    return !p ? w32_strdup("") : wide ? w32_narrow((const WCHAR *)p) : w32_strdup((const char *)p);
}

/* places a bar along the top or bottom of its parent, as comctl32 does on TB_AUTOSIZE and WM_SIZE */
static void align_to_parent(struct w32_window *w, int height, bool bottom)
{
    if (!w->parent || (w->style & (CCS_NORESIZE | CCS_NOPARENTALIGN)))
        return;
    struct rect pa = w32_area(w->parent);
    if (w->rect.x == 0 && w->rect.y == (bottom ? pa.h - height : 0) && w->rect.w == pa.w && w->rect.h == height)
        return;
    BOOL(WINAPI * move)(HANDLE, int, int, int, int, BOOL) =
        (BOOL(WINAPI *)(HANDLE, int, int, int, int, BOOL))w32_builtin_lookup("user32.dll", "MoveWindow");
    if (move)
        move(w, 0, bottom ? pa.h - height : 0, pa.w, height, TRUE);
}

struct tb_button {
    int image, id;
    uint8_t state, style;
    uint64_t data;
    char *text;
    int width;
};

struct tb {
    struct tb_button *b;
    int count, cap;
    char **strings;
    int nstrings;
    int bw, bh, iw, ih, pad_x, pad_y;
    int struct_size, pressed, images_used;
    HANDLE il, own_il;
    struct w32_window *target;
    uint32_t ex;
};

static struct tb *tb_of(struct w32_window *w)
{
    if (!w->list) {
        struct tb *s = w32_zalloc(sizeof(*s));
        if (!s)
            return NULL;
        s->iw = 16;
        s->ih = 15;
        s->pad_x = 7;
        s->pad_y = 6;
        s->pressed = -1;
        s->struct_size = w32->win64 ? 32 : 20;
        w->list = s;
    }
    return w->list;
}

static bool any_text(struct tb *s)
{
    for (int i = 0; i < s->count; i++)
        if (s->b[i].text && s->b[i].text[0] && !(s->b[i].style & TBSTYLE_SEP))
            return true;
    return false;
}

static int button_h(struct w32_window *w, struct tb *s)
{
    int h = s->ih + s->pad_y;
    if (any_text(s) && !(w->style & TBSTYLE_LIST))
        h += w32_ctl_font(w)->height + 2;
    return MAX(h, s->bh);
}

static int button_w(struct w32_window *w, struct tb *s, int i)
{
    struct tb_button *b = &s->b[i];
    if (b->state & TBSTATE_HIDDEN)
        return 0;
    if (b->style & TBSTYLE_SEP)
        return b->image > 0 ? b->image : 8;
    if (b->width)
        return b->width;
    int width = s->iw + s->pad_x, text = b->text && b->text[0] ? gfx_text_width(w32_ctl_font(w), b->text) + 8 : 0;
    width = (w->style & TBSTYLE_LIST) ? width + text : MAX(width, text);
    if (b->style & TBSTYLE_DROPDOWN)
        width += 12;
    return MAX(width, s->bw);
}

static struct rect button_rect(struct w32_window *w, struct tb *s, int i)
{
    int x = 2;
    for (int k = 0; k < i && k < s->count; k++)
        x += button_w(w, s, k);
    return (struct rect){x, 2, button_w(w, s, i), button_h(w, s)};
}

static int bar_h(struct w32_window *w, struct tb *s)
{
    return button_h(w, s) + 4 + ((w->style & CCS_NODIVIDER) ? 0 : 2);
}

static int index_of(struct tb *s, int id)
{
    for (int i = 0; i < s->count; i++)
        if (s->b[i].id == id)
            return i;
    return -1;
}

static int hit(struct w32_window *w, struct tb *s, int x, int y)
{
    for (int i = 0; i < s->count; i++) {
        struct rect r = button_rect(w, s, i);
        if (r.w && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

static const char *button_text(struct tb *s, uintptr_t string, bool wide, char **own)
{
    *own = NULL;
    if ((intptr_t)string >= 0 && string < 0x10000)
        return (int)string < s->nstrings ? s->strings[string] : NULL;
    if (string == 0xffffffffu)
        return NULL;
    return *own = take_text(string, wide);
}

static bool add_buttons(struct w32_window *w, struct tb *s, int at, int n, const uint8_t *p, bool wide)
{
    if (n <= 0 || !p)
        return false;
    if (s->count + n > s->cap) {
        int cap = MAX(s->count + n, s->cap * 2);
        struct tb_button *b = w32_zalloc(sizeof(*b) * (size_t)cap);
        if (!b)
            return false;
        if (s->b)
            memcpy(b, s->b, sizeof(*b) * (size_t)s->count);
        w32_free(s->b);
        s->b = b;
        s->cap = cap;
    }
    at = at < 0 || at > s->count ? s->count : at;
    memmove(s->b + at + n, s->b + at, sizeof(*s->b) * (size_t)(s->count - at));
    for (int k = 0; k < n; k++, p += s->struct_size) {
        struct tb_button *b = &s->b[at + k];
        *b = (struct tb_button){get32(p), get32(p + 4), p[8], p[9], 0, NULL, 0};
        b->data = get_ptr(p + (w32->win64 ? 16 : 12));
        if (s->struct_size >= (w32->win64 ? 32 : 20)) {
            char *own;
            const char *t = button_text(s, get_ptr(p + (w32->win64 ? 24 : 16)), wide, &own);
            b->text = own ? own : t ? w32_strdup(t) : NULL;
        }
    }
    s->count += n;
    w32_invalidate(w, NULL, true);
    return true;
}

static void free_button(struct tb_button *b)
{
    w32_free(b->text);
    b->text = NULL;
}

static LRESULT add_bitmap(struct w32_window *w, struct tb *s, int n, const uint8_t *p)
{
    if (!p)
        return -1;
    uintptr_t inst = get_ptr(p), id = get_ptr(p + ptr_size());
    int first = s->images_used;
    s->images_used += MAX(n, 0);
    if (inst == 0xffffffffu)
        return first;
    HANDLE bitmap = inst ? ((HANDLE(WINAPI *)(HANDLE, uintptr_t))w32_builtin_lookup("user32.dll", "LoadBitmapW"))(
                               (HANDLE)inst, id)
                         : (HANDLE)id;
    if (!bitmap)
        return first;
    if (!s->own_il) {
        void *(WINAPI * create)(int, int, UINT, int, int) =
            (void *(WINAPI *)(int, int, UINT, int, int))w32_builtin_lookup("comctl32.dll", "ImageList_Create");
        s->own_il = create ? create(s->iw, s->ih, 0x21, 16, 16) : NULL;
        if (!s->il)
            s->il = s->own_il;
    }
    int(WINAPI * add)(HANDLE, HANDLE, COLORREF) =
        (int(WINAPI *)(HANDLE, HANDLE, COLORREF))w32_builtin_lookup("comctl32.dll", "ImageList_AddMasked");
    if (s->own_il && add)
        add(s->own_il, bitmap, 0xffffffffu);
    if (inst)
        ((BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "DeleteObject"))(bitmap);
    w32_invalidate(w, NULL, true);
    return first;
}

static LRESULT add_strings(struct tb *s, uintptr_t inst, uintptr_t p, bool wide)
{
    if (inst || !p)
        return -1;
    int first = s->nstrings;
    for (;;) {
        char *t = take_text(p, wide);
        if (!t || !t[0]) {
            w32_free(t);
            break;
        }
        char **list = w32_zalloc(sizeof(char *) * (size_t)(s->nstrings + 1));
        if (!list) {
            w32_free(t);
            break;
        }
        if (s->strings)
            memcpy(list, s->strings, sizeof(char *) * (size_t)s->nstrings);
        w32_free(s->strings);
        s->strings = list;
        s->strings[s->nstrings++] = t;
        p += wide ? (w32_wcslen((const WCHAR *)p) + 1) * 2 : strlen((const char *)p) + 1;
    }
    return first;
}

static void paint_toolbar(struct w32_window *w, struct tb *s)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    const struct ui_font *f = w32_ctl_font(w);
    gfx_fill(p.c, p.r, style.window);
    if (!(w->style & CCS_NODIVIDER))
        gfx_fill(p.c, (struct rect){p.r.x, p.r.y + p.r.h - 1, p.r.w, 1}, style.separator);
    typedef BOOL WINAPI (*draw_fn)(HANDLE, int, HANDLE, int, int, UINT);
    draw_fn draw = (draw_fn)w32_builtin_lookup("comctl32.dll", "ImageList_Draw");
    bool list = w->style & TBSTYLE_LIST;
    for (int i = 0; i < s->count; i++) {
        struct tb_button *b = &s->b[i];
        struct rect r = button_rect(w, s, i);
        if (!r.w)
            continue;
        r.x += p.r.x;
        r.y += p.r.y;
        if (b->style & TBSTYLE_SEP) {
            gfx_fill(p.c, (struct rect){r.x + r.w / 2, r.y + 3, 1, r.h - 6}, style.separator);
            continue;
        }
        bool enabled = b->state & TBSTATE_ENABLED, down = (b->state & (TBSTATE_PRESSED | TBSTATE_CHECKED)) != 0;
        if (down)
            gfx_round(p.c, r, 5, style.control_pressed);
        int ix = list ? r.x + 4 : r.x + (r.w - ((b->style & TBSTYLE_DROPDOWN) ? 12 : 0) - s->iw) / 2;
        int iy = list ? r.y + (r.h - s->ih) / 2 : r.y + 3;
        if (s->il && b->image >= 0 && draw)
            draw(s->il, b->image, p.dc, ix - p.r.x, iy - p.r.y, 1);
        uint32_t color = enabled ? style.text : style.tertiary;
        if (b->text && b->text[0]) {
            struct rect tr = list ? (struct rect){ix + s->iw + 4, r.y, r.x + r.w - ix - s->iw - 6, r.h}
                                  : (struct rect){r.x + 2, iy + s->ih + 1, r.w - 4, f->height + 2};
            gfx_text_box(p.c, tr, f, b->text, color, list ? ALIGN_LEFT : ALIGN_CENTER);
        }
        if (b->style & TBSTYLE_DROPDOWN)
            gfx_text_box(p.c, (struct rect){r.x + r.w - 13, r.y, 12, r.h}, f, "v", color, ALIGN_CENTER);
        if (!enabled && s->il && b->image >= 0)
            for (int y = 0; y < s->ih; y += 2)
                gfx_fill(p.c, (struct rect){ix, iy + y, s->iw, 1}, style.window);
    }
    w32_ctl_end(w, &p);
}

static void press(struct w32_window *w, struct tb *s, int i, bool on)
{
    if (i < 0 || i >= s->count)
        return;
    s->b[i].state = on ? s->b[i].state | TBSTATE_PRESSED : s->b[i].state & ~TBSTATE_PRESSED;
    w32_invalidate(w, NULL, false);
}

static void click(struct w32_window *w, struct tb *s, int i)
{
    struct tb_button *b = &s->b[i];
    if (b->style & TBSTYLE_CHECK) {
        if ((b->style & TBSTYLE_GROUP) && !(b->state & TBSTATE_CHECKED)) {
            for (int k = i - 1; k >= 0 && (s->b[k].style & TBSTYLE_GROUP); k--)
                s->b[k].state &= ~TBSTATE_CHECKED;
            for (int k = i + 1; k < s->count && (s->b[k].style & TBSTYLE_GROUP); k++)
                s->b[k].state &= ~TBSTATE_CHECKED;
            b->state |= TBSTATE_CHECKED;
        } else if (!(b->style & TBSTYLE_GROUP)) {
            b->state ^= TBSTATE_CHECKED;
        }
    }
    struct w32_window *to = s->target ? s->target : w->parent;
    if (to)
        w32_send(to, WM_COMMAND, (WPARAM)(uint16_t)b->id, (LPARAM)(uintptr_t)w);
}

static void dropdown(struct w32_window *w, struct tb *s, int i)
{
    struct w32_window *to = s->target ? s->target : w->parent;
    int hs = w32->win64 ? 24 : 12, code = TBN_DROPDOWN;
    uint8_t *nm = to ? w32_zalloc((size_t)hs + 96) : NULL;
    if (!nm)
        return;
    uint64_t hwnd = (uintptr_t)w, id = (uint64_t)w->id;
    memcpy(nm, &hwnd, (size_t)ptr_size());
    memcpy(nm + ptr_size(), &id, (size_t)ptr_size());
    memcpy(nm + 2 * ptr_size(), &code, 4);
    memcpy(nm + hs, &s->b[i].id, 4);
    struct rect r = button_rect(w, s, i);
    RECT rc = {r.x, r.y, r.x + r.w, r.y + r.h};
    memcpy(nm + hs + (w32->win64 ? 72 : 44), &rc, sizeof(rc));
    w32_send(to, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
    w32_free(nm);
}

static void button_info(struct tb *s, int i, uint8_t *p, bool wide, bool set)
{
    struct tb_button *b = &s->b[i];
    uint32_t mask = (uint32_t)get32(p + 4);
    int lp_off = w32->win64 ? 24 : 20, text_off = w32->win64 ? 32 : 24, cch_off = w32->win64 ? 40 : 28;
    if (set) {
        if (mask & TBIF_COMMAND)
            b->id = get32(p + 8);
        if (mask & TBIF_IMAGE)
            b->image = get32(p + 12);
        if (mask & TBIF_STATE)
            b->state = p[16];
        if (mask & TBIF_STYLE)
            b->style = p[17];
        if (mask & TBIF_SIZE)
            b->width = *(const uint16_t *)(p + 18);
        if (mask & TBIF_LPARAM)
            b->data = get_ptr(p + lp_off);
        if (mask & TBIF_TEXT) {
            free_button(b);
            b->text = take_text(get_ptr(p + text_off), wide);
        }
        return;
    }
    if (mask & TBIF_COMMAND)
        memcpy(p + 8, &b->id, 4);
    if (mask & TBIF_IMAGE)
        memcpy(p + 12, &b->image, 4);
    if (mask & TBIF_STATE)
        p[16] = b->state;
    if (mask & TBIF_STYLE)
        p[17] = b->style;
    if (mask & TBIF_SIZE)
        *(uint16_t *)(p + 18) = (uint16_t)b->width;
    if (mask & TBIF_LPARAM)
        memcpy(p + lp_off, &b->data, (size_t)ptr_size());
    if (mask & TBIF_TEXT)
        put_text((void *)get_ptr(p + text_off), get32(p + cch_off), b->text ? b->text : "", wide);
}

static LRESULT set_flag(struct w32_window *w, struct tb *s, int id, uint8_t flag, bool on)
{
    int i = index_of(s, id);
    if (i < 0)
        return FALSE;
    s->b[i].state = on ? s->b[i].state | flag : s->b[i].state & ~flag;
    w32_invalidate(w, NULL, true);
    return TRUE;
}

LRESULT WINAPI w32_toolbar_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct tb *s = tb_of(w);
    if (!s)
        return w32_def_window_proc(h, msg, wp, lp);
    uint8_t *ptr = (uint8_t *)(uintptr_t)(uint32_t)lp;
    int i = (int)wp, at = index_of(s, (int)wp);
    switch (msg) {
    case TB_BUTTONSTRUCTSIZE: s->struct_size = MAX(16, (int)wp); return 0;
    case TB_ADDBUTTONSA:
    case TB_ADDBUTTONSW: return add_buttons(w, s, -1, (int)wp, ptr, msg == TB_ADDBUTTONSW);
    case TB_INSERTBUTTONA:
    case TB_INSERTBUTTONW: return add_buttons(w, s, (int)wp, 1, ptr, msg == TB_INSERTBUTTONW);
    case TB_DELETEBUTTON:
        if (i < 0 || i >= s->count)
            return FALSE;
        free_button(&s->b[i]);
        memmove(s->b + i, s->b + i + 1, sizeof(*s->b) * (size_t)(s->count - i - 1));
        s->count--;
        w32_invalidate(w, NULL, true);
        return TRUE;
    case TB_BUTTONCOUNT: return s->count;
    case TB_COMMANDTOINDEX: return at;
    case TB_GETBUTTON: {
        if (i < 0 || i >= s->count || !ptr)
            return FALSE;
        struct tb_button *b = &s->b[i];
        memset(ptr, 0, (size_t)s->struct_size);
        memcpy(ptr, &b->image, 4);
        memcpy(ptr + 4, &b->id, 4);
        ptr[8] = b->state;
        ptr[9] = b->style;
        memcpy(ptr + (w32->win64 ? 16 : 12), &b->data, (size_t)ptr_size());
        return TRUE;
    }
    case TB_ENABLEBUTTON: return set_flag(w, s, (int)wp, TBSTATE_ENABLED, (int16_t)lp != 0);
    case TB_CHECKBUTTON: return set_flag(w, s, (int)wp, TBSTATE_CHECKED, (int16_t)lp != 0);
    case TB_PRESSBUTTON: return set_flag(w, s, (int)wp, TBSTATE_PRESSED, (int16_t)lp != 0);
    case TB_HIDEBUTTON: return set_flag(w, s, (int)wp, TBSTATE_HIDDEN, (int16_t)lp != 0);
    case TB_INDETERMINATE: return set_flag(w, s, (int)wp, 0x10, (int16_t)lp != 0);
    case TB_ISBUTTONENABLED: return at >= 0 && (s->b[at].state & TBSTATE_ENABLED);
    case TB_ISBUTTONCHECKED: return at >= 0 && (s->b[at].state & TBSTATE_CHECKED);
    case TB_ISBUTTONPRESSED: return at >= 0 && (s->b[at].state & TBSTATE_PRESSED);
    case TB_ISBUTTONHIDDEN: return at >= 0 && (s->b[at].state & TBSTATE_HIDDEN);
    case TB_GETSTATE: return at >= 0 ? s->b[at].state : -1;
    case TB_SETSTATE:
        if (at < 0)
            return FALSE;
        s->b[at].state = (uint8_t)lp;
        w32_invalidate(w, NULL, true);
        return TRUE;
    case TB_SETCMDID:
        if (i < 0 || i >= s->count)
            return FALSE;
        s->b[i].id = (int)lp;
        return TRUE;
    case TB_GETBITMAP: return at >= 0 ? s->b[at].image : -1;
    case TB_ADDBITMAP: return add_bitmap(w, s, (int)wp, ptr);
    case TB_ADDSTRINGA:
    case TB_ADDSTRINGW: return add_strings(s, wp, (uintptr_t)ptr, msg == TB_ADDSTRINGW);
    case TB_SETIMAGELIST: {
        HANDLE old = s->il;
        s->il = (HANDLE)(uintptr_t)(uint32_t)lp;
        typedef BOOL WINAPI (*size_fn)(HANDLE, int *, int *);
        size_fn size = (size_fn)w32_builtin_lookup("comctl32.dll", "ImageList_GetIconSize");
        if (s->il && size)
            size(s->il, &s->iw, &s->ih);
        w32_invalidate(w, NULL, true);
        return (LRESULT)(uintptr_t)old;
    }
    case TB_GETIMAGELIST: return (LRESULT)(uintptr_t)s->il;
    case TB_SETBUTTONSIZE:
        s->bw = (int16_t)lp;
        s->bh = (int16_t)(lp >> 16);
        return TRUE;
    case TB_SETBITMAPSIZE:
        s->iw = MAX(1, (int)(int16_t)lp);
        s->ih = MAX(1, (int)(int16_t)(lp >> 16));
        return TRUE;
    case TB_SETBUTTONWIDTH: return TRUE;
    case TB_GETBUTTONSIZE: {
        int bw = s->count ? button_w(w, s, 0) : s->iw + s->pad_x;
        return (LRESULT)((uint32_t)(uint16_t)bw | (uint32_t)(uint16_t)button_h(w, s) << 16);
    }
    case TB_GETITEMRECT:
    case TB_GETRECT: {
        int k = msg == TB_GETRECT ? at : i;
        if (k < 0 || k >= s->count || !ptr)
            return FALSE;
        struct rect r = button_rect(w, s, k);
        *(RECT *)ptr = (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
        return TRUE;
    }
    case TB_HITTEST: {
        if (!ptr)
            return -1;
        int k = hit(w, s, get32(ptr), get32(ptr + 4));
        return k >= 0 ? k : -1;
    }
    case TB_GETMAXSIZE:
    case TB_GETIDEALSIZE:
        if (ptr) {
            struct rect r = s->count ? button_rect(w, s, s->count - 1) : (struct rect){0, 0, 0, 0};
            *(SIZE *)ptr = (SIZE){r.x + r.w + 2, bar_h(w, s)};
        }
        return TRUE;
    case TB_AUTOSIZE: align_to_parent(w, bar_h(w, s), (w->style & 3) == CCS_BOTTOM); return 0;
    case TB_GETROWS: return 1;
    case TB_SETPARENT: {
        struct w32_window *old = s->target ? s->target : w->parent;
        s->target = w32_win((HANDLE)wp);
        return (LRESULT)(uintptr_t)old;
    }
    case TB_GETBUTTONINFOA:
    case TB_GETBUTTONINFOW:
    case TB_SETBUTTONINFOA:
    case TB_SETBUTTONINFOW: {
        if (!ptr)
            return msg == TB_GETBUTTONINFOA || msg == TB_GETBUTTONINFOW ? -1 : FALSE;
        int k = (get32(ptr + 4) & TBIF_BYINDEX) ? i : at;
        if (k < 0 || k >= s->count)
            return msg == TB_GETBUTTONINFOA || msg == TB_GETBUTTONINFOW ? -1 : FALSE;
        bool set = msg == TB_SETBUTTONINFOA || msg == TB_SETBUTTONINFOW;
        button_info(s, k, ptr, msg == TB_GETBUTTONINFOW || msg == TB_SETBUTTONINFOW, set);
        if (set)
            w32_invalidate(w, NULL, true);
        return set ? TRUE : k;
    }
    case TB_GETHOTITEM: return -1;
    case TB_SETEXTENDEDSTYLE: {
        uint32_t old = s->ex;
        s->ex = (uint32_t)lp;
        return (LRESULT)old;
    }
    case TB_GETEXTENDEDSTYLE: return (LRESULT)s->ex;
    case TB_GETPADDING: return (LRESULT)((uint32_t)(uint16_t)s->pad_x | (uint32_t)(uint16_t)s->pad_y << 16);
    case TB_SETPADDING: {
        LRESULT old = (LRESULT)((uint32_t)(uint16_t)s->pad_x | (uint32_t)(uint16_t)s->pad_y << 16);
        s->pad_x = (int16_t)lp;
        s->pad_y = (int16_t)(lp >> 16);
        return old;
    }
    case TB_SETSTYLE: w->style = (uint32_t)lp; w32_invalidate(w, NULL, true); return 0;
    case TB_GETSTYLE: return (LRESULT)w->style;
    case WM_PAINT: paint_toolbar(w, s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT: w->font = (HANDLE)wp; return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int k = hit(w, s, (int16_t)lp, (int16_t)(lp >> 16));
        if (k < 0 || (s->b[k].style & TBSTYLE_SEP) || !(s->b[k].state & TBSTATE_ENABLED))
            return 0;
        struct rect r = button_rect(w, s, k);
        if ((s->b[k].style & TBSTYLE_DROPDOWN) && (int16_t)lp >= r.x + r.w - 13) {
            dropdown(w, s, k);
            return 0;
        }
        s->pressed = k;
        press(w, s, k, true);
        w32_set_capture_internal(w);
        return 0;
    }
    case WM_LBUTTONUP_: {
        int k = s->pressed;
        if (k < 0)
            return 0;
        s->pressed = -1;
        w32_set_capture_internal(NULL);
        if (k < s->count) {
            press(w, s, k, false);
            if (hit(w, s, (int16_t)lp, (int16_t)(lp >> 16)) == k)
                click(w, s, k);
        }
        return 0;
    }
    case WM_NCDESTROY_:
        for (int k = 0; k < s->count; k++)
            free_button(&s->b[k]);
        for (int k = 0; k < s->nstrings; k++)
            w32_free(s->strings[k]);
        if (s->own_il)
            ((BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("comctl32.dll", "ImageList_Destroy"))(s->own_il);
        w32_free(s->strings);
        w32_free(s->b);
        w32_free(s);
        w->list = NULL;
        break;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}

struct sb {
    char *text[MAX_PARTS + 1];
    int edge[MAX_PARTS];
    int parts, min_h;
    bool simple;
};

static struct sb *sb_of(struct w32_window *w)
{
    if (!w->list) {
        struct sb *s = w32_zalloc(sizeof(*s));
        if (!s)
            return NULL;
        s->parts = 1;
        s->edge[0] = -1;
        w->list = s;
    }
    return w->list;
}

static int sb_h(struct w32_window *w, struct sb *s)
{
    return MAX(w32_ctl_font(w)->height + 8, s->min_h);
}

static struct rect part_rect(struct w32_window *w, struct sb *s, int i)
{
    if (s->simple)
        return (struct rect){0, 0, w->rect.w, w->rect.h};
    int x = i ? s->edge[i - 1] : 0, right = s->edge[i] < 0 ? w->rect.w : MIN(s->edge[i], w->rect.w);
    return (struct rect){x, 0, MAX(0, right - x), w->rect.h};
}

static void paint_status(struct w32_window *w, struct sb *s)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    const struct ui_font *f = w32_ctl_font(w);
    gfx_fill(p.c, p.r, style.window);
    gfx_fill(p.c, (struct rect){p.r.x, p.r.y, p.r.w, 1}, style.separator);
    int n = s->simple ? 1 : s->parts;
    for (int i = 0; i < n; i++) {
        struct rect r = part_rect(w, s, i);
        r.x += p.r.x;
        r.y += p.r.y;
        if (i)
            gfx_fill(p.c, (struct rect){r.x, r.y + 4, 1, r.h - 8}, style.separator);
        const char *t = s->text[s->simple ? MAX_PARTS : i];
        if (!t)
            continue;
        enum text_align align = ALIGN_LEFT;
        for (int tabs = 0; *t == '\t' && tabs < 2; tabs++, t++)
            align = tabs ? ALIGN_RIGHT : ALIGN_CENTER;
        gfx_text_box(p.c, (struct rect){r.x + 6, r.y, r.w - 12, r.h}, f, t, style.secondary, align);
    }
    w32_ctl_end(w, &p);
}

static bool sb_set(struct w32_window *w, struct sb *s, WPARAM wp, uintptr_t text, bool wide)
{
    int part = (int)(wp & 0xff);
    part = part == 255 ? MAX_PARTS : part;
    if (part > MAX_PARTS || (part >= s->parts && part != MAX_PARTS))
        return false;
    char *t = take_text(text, wide);
    if (!t)
        return false;
    w32_free(s->text[part]);
    s->text[part] = t;
    w32_invalidate(w, NULL, true);
    return true;
}

static LRESULT sb_get(struct sb *s, int part, void *out, bool wide, bool length_only)
{
    part = part == 255 ? MAX_PARTS : part;
    if (part < 0 || part > MAX_PARTS)
        return 0;
    const char *t = s->text[part] ? s->text[part] : "";
    if (!length_only && out) {
        if (wide) {
            size_t n = w32_cp_to_wide(t, (int)strlen(t), (WCHAR *)out, 1024, 65001);
            ((WCHAR *)out)[n] = 0;
        } else {
            memcpy(out, t, strlen(t) + 1);
        }
    }
    return (LRESULT)strlen(t);
}

LRESULT WINAPI w32_status_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct sb *s = sb_of(w);
    if (!s)
        return w32_def_window_proc(h, msg, wp, lp);
    uint8_t *ptr = (uint8_t *)(uintptr_t)(uint32_t)lp;
    switch (msg) {
    case WM_CREATE:
        if (w->text)
            s->text[0] = w32_strdup(w->text);
        align_to_parent(w, sb_h(w, s), (w->style & 3) != CCS_TOP);
        break;
    case SB_SETTEXTA:
    case SB_SETTEXTW: return sb_set(w, s, wp, (uintptr_t)ptr, msg == SB_SETTEXTW);
    case WM_SETTEXT_:
        w32_def_window_proc(h, msg, wp, lp);
        return sb_set(w, s, 0, (uintptr_t)ptr, w->unicode);
    case SB_GETTEXTA:
    case SB_GETTEXTW: return sb_get(s, (int)(wp & 0xff), ptr, msg == SB_GETTEXTW, false);
    case SB_GETTEXTLENGTHA:
    case SB_GETTEXTLENGTHW: return sb_get(s, (int)(wp & 0xff), NULL, false, true);
    case SB_SETPARTS: {
        int n = (int)wp;
        if (n < 1 || n > MAX_PARTS || !ptr)
            return FALSE;
        for (int i = 0; i < n; i++)
            s->edge[i] = get32(ptr + i * 4);
        for (int i = n; i < s->parts; i++) {
            w32_free(s->text[i]);
            s->text[i] = NULL;
        }
        s->parts = n;
        w32_invalidate(w, NULL, true);
        return TRUE;
    }
    case SB_GETPARTS:
        for (int i = 0; ptr && i < (int)wp && i < s->parts; i++)
            memcpy(ptr + i * 4, &s->edge[i], 4);
        return s->parts;
    case SB_GETBORDERS:
        if (ptr) {
            int b[3] = {0, 2, 2};
            memcpy(ptr, b, sizeof(b));
        }
        return TRUE;
    case SB_SETMINHEIGHT: s->min_h = (int)wp; return 0;
    case SB_SIMPLE: s->simple = wp != 0; w32_invalidate(w, NULL, true); return 0;
    case SB_ISSIMPLE: return s->simple;
    case SB_GETRECT: {
        int i = (int)wp;
        if (i < 0 || i >= s->parts || !ptr)
            return FALSE;
        struct rect r = part_rect(w, s, i);
        *(RECT *)ptr = (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
        return TRUE;
    }
    case SB_SETICON: return TRUE;
    case SB_GETICON: return 0;
    case WM_SIZE:
        align_to_parent(w, sb_h(w, s), (w->style & 3) != CCS_TOP);
        return 0;
    case WM_PAINT: paint_status(w, s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFONT: w->font = (HANDLE)wp; return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_NCDESTROY_:
        for (int i = 0; i <= MAX_PARTS; i++)
            w32_free(s->text[i]);
        w32_free(s);
        w->list = NULL;
        break;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}

typedef HANDLE(WINAPI *create_fn)(DWORD, const char *, const char *, DWORD, int, int, int, int, HANDLE, HANDLE, HANDLE,
                                  void *);

static HANDLE create(const char *cls, const char *text, DWORD style, HANDLE parent, UINT id)
{
    create_fn make = (create_fn)w32_builtin_lookup("user32.dll", "CreateWindowExA");
    return make ? make(0, cls, text, style | WS_CHILD, 0, 0, 0, 0, parent, (HANDLE)(uintptr_t)id, NULL, NULL) : NULL;
}

static HANDLE WINAPI CreateToolbarEx(HANDLE parent, DWORD style, UINT id, int bitmaps, HANDLE inst, uintptr_t bitmap,
                                     const void *buttons, int count, int bw, int bh, int iw, int ih, UINT size)
{
    HANDLE h = create("ToolbarWindow32", "", style, parent, id);
    struct w32_window *w = w32_win(h);
    if (!w)
        return NULL;
    w32_send(w, TB_BUTTONSTRUCTSIZE, size, 0);
    if (iw > 0 && ih > 0)
        w32_send(w, TB_SETBITMAPSIZE, 0, (LPARAM)((uint32_t)iw | (uint32_t)ih << 16));
    if (bw > 0 && bh > 0)
        w32_send(w, TB_SETBUTTONSIZE, 0, (LPARAM)((uint32_t)bw | (uint32_t)bh << 16));
    uint64_t add[2] = {(uintptr_t)inst, bitmap};
    uint32_t add32[2] = {(uint32_t)(uintptr_t)inst, (uint32_t)bitmap};
    if (bitmaps > 0)
        w32_send(w, TB_ADDBITMAP, (WPARAM)bitmaps, (LPARAM)(uintptr_t)(w32->win64 ? (void *)add : (void *)add32));
    if (buttons && count > 0)
        w32_send(w, TB_ADDBUTTONSA, (WPARAM)count, (LPARAM)(uintptr_t)buttons);
    w32_send(w, TB_AUTOSIZE, 0, 0);
    return h;
}

static HANDLE WINAPI CreateStatusWindowA(DWORD style, const char *text, HANDLE parent, UINT id)
{
    return create("msctls_statusbar32", text, style, parent, id);
}

static HANDLE WINAPI CreateStatusWindowW(DWORD style, LPCWSTR text, HANDLE parent, UINT id)
{
    char *t = text ? w32_narrow(text) : NULL;
    HANDLE h = create("msctls_statusbar32", t, style, parent, id);
    w32_free(t);
    return h;
}

static const struct w32_export toolbar_exports[] = {
    W32_FN(CreateToolbarEx), W32_FN(CreateStatusWindowA), W32_FN(CreateStatusWindowW),
};

W32_EXPORTS(toolbar_comctl_module, "comctl32.dll", toolbar_exports);
LRESULT WINAPI w32_tooltip_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case 0x404: case 0x432: return TRUE;
    case 0x418: return -1;
    case 0x84: return -1;
    default:
        if (msg >= 0x400 && msg < 0x500)
            return 0;
        return w32_def_window_proc(h, msg, wp, lp);
    }
}
#endif
