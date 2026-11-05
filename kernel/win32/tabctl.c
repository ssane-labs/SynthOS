#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>
#include <synth/string.h>

#include "w32gui.h"

/* SysTabControl32: one row of tabs over a framed page area; TCITEM comes in the caller's own (32- or 64-bit) layout */
enum {
    TCM_GETIMAGELIST = 0x1302,
    TCM_SETIMAGELIST = 0x1303,
    TCM_GETITEMCOUNT = 0x1304,
    TCM_GETITEMA = 0x1305,
    TCM_SETITEMA = 0x1306,
    TCM_INSERTITEMA = 0x1307,
    TCM_DELETEITEM = 0x1308,
    TCM_DELETEALLITEMS = 0x1309,
    TCM_GETITEMRECT = 0x130a,
    TCM_GETCURSEL = 0x130b,
    TCM_SETCURSEL = 0x130c,
    TCM_HITTEST = 0x130d,
    TCM_SETITEMEXTRA = 0x130e,
    TCM_ADJUSTRECT = 0x1328,
    TCM_SETITEMSIZE = 0x1329,
    TCM_REMOVEIMAGE = 0x132a,
    TCM_SETPADDING = 0x132b,
    TCM_GETROWCOUNT = 0x132c,
    TCM_GETTOOLTIPS = 0x132d,
    TCM_SETTOOLTIPS = 0x132e,
    TCM_GETCURFOCUS = 0x132f,
    TCM_SETCURFOCUS = 0x1330,
    TCM_SETMINTABWIDTH = 0x1331,
    TCM_DESELECTALL = 0x1332,
    TCM_HIGHLIGHTITEM = 0x1333,
    TCM_SETEXTENDEDSTYLE = 0x1334,
    TCM_GETEXTENDEDSTYLE = 0x1335,
    TCM_GETITEMW = 0x133c,
    TCM_SETITEMW = 0x133d,
    TCM_INSERTITEMW = 0x133e,
    CCM_SETUNICODEFORMAT = 0x2005,
    CCM_GETUNICODEFORMAT = 0x2006,
    TCIF_TEXT = 1,
    TCIF_IMAGE = 2,
    TCIF_PARAM = 8,
    TCIF_STATE = 0x10,
    TCIS_BUTTONPRESSED = 1,
    TCIS_HIGHLIGHTED = 2,
    TCS_BUTTONS = 0x100,
    TCS_FIXEDWIDTH = 0x400,
    TCHT_NOWHERE = 1,
    TCHT_ONITEM = 6,
    TCN_KEYDOWN = -550,
    TCN_SELCHANGE = -551,
    TCN_SELCHANGING = -552,
    NM_CLICK = -2,
    NM_RCLICK = -5,
    WM_NCDESTROY_ = 0x82,
    VK_LEFT_ = 0x25,
    VK_RIGHT_ = 0x27,
    ARROWS_W = 34,
};

struct tab {
    char *text;
    int image;
    uint64_t param;
    uint32_t state;
};

struct tabs {
    struct tab *t;
    int count, cap, cur, focus, first;
    int fixed_w, fixed_h, min_w, pad_x, pad_y;
    HANDLE images;
    uint32_t ex;
    bool unicode;
};

static struct tabs *tabs_of(struct w32_window *w)
{
    if (!w->list) {
        struct tabs *s = w32_zalloc(sizeof(*s));
        if (!s)
            return NULL;
        s->cur = s->focus = -1;
        s->pad_x = 8;
        s->pad_y = 3;
        s->unicode = w->unicode;
        w->list = s;
    }
    return w->list;
}

/* the TCITEM fields at their offsets in the caller's layout */
struct item_view {
    uint32_t mask, state, state_mask;
    uintptr_t text;
    int cch, image;
    uint64_t param;
};

static void item_read(const uint8_t *p, struct item_view *v)
{
    memcpy(&v->mask, p, 4);
    memcpy(&v->state, p + 4, 4);
    memcpy(&v->state_mask, p + 8, 4);
    uint32_t text;
    if (w32->win64) {
        memcpy(&text, p + 16, 4);
        memcpy(&v->cch, p + 24, 4);
        memcpy(&v->image, p + 28, 4);
        memcpy(&v->param, p + 32, 8);
    } else {
        uint32_t param;
        memcpy(&text, p + 12, 4);
        memcpy(&v->cch, p + 16, 4);
        memcpy(&v->image, p + 20, 4);
        memcpy(&param, p + 24, 4);
        v->param = param;
    }
    v->text = text;
}

static void item_write(uint8_t *p, const struct tab *t, bool wide)
{
    struct item_view v;
    item_read(p, &v);
    if (v.mask & TCIF_STATE) {
        uint32_t st = t->state & (v.state_mask ? v.state_mask : ~0u);
        memcpy(p + 4, &st, 4);
    }
    if (v.mask & TCIF_IMAGE)
        memcpy(p + (w32->win64 ? 28 : 20), &t->image, 4);
    if (v.mask & TCIF_PARAM)
        memcpy(p + (w32->win64 ? 32 : 24), &t->param, w32->win64 ? 8 : 4);
    if ((v.mask & TCIF_TEXT) && v.text && v.cch > 0) {
        const char *s = t->text ? t->text : "";
        if (wide) {
            size_t n = w32_cp_to_wide(s, (int)strlen(s), (WCHAR *)v.text, (size_t)v.cch - 1, 65001);
            ((WCHAR *)v.text)[n] = 0;
        } else {
            size_t n = MIN(strlen(s), (size_t)v.cch - 1);
            memcpy((char *)v.text, s, n);
            ((char *)v.text)[n] = 0;
        }
    }
}

static void item_apply(struct tab *t, const struct item_view *v, bool wide)
{
    if (v->mask & TCIF_TEXT) {
        char *s = !v->text ? w32_strdup("") : wide ? w32_narrow((const WCHAR *)v->text) : w32_strdup((const char *)v->text);
        if (s) {
            w32_free(t->text);
            t->text = s;
        }
    }
    if (v->mask & TCIF_IMAGE)
        t->image = v->image;
    if (v->mask & TCIF_PARAM)
        t->param = v->param;
    if (v->mask & TCIF_STATE)
        t->state = (t->state & ~v->state_mask) | (v->state & v->state_mask);
}

static int tab_h(struct w32_window *w, struct tabs *s)
{
    return s->fixed_h ? s->fixed_h : w32_ctl_font(w)->height + 2 * s->pad_y + 4;
}

static int tab_w(struct w32_window *w, struct tabs *s, int i)
{
    if ((w->style & TCS_FIXEDWIDTH) && s->fixed_w)
        return s->fixed_w;
    int width = gfx_text_width(w32_ctl_font(w), s->t[i].text ? s->t[i].text : "") + 2 * s->pad_x;
    if (s->images && s->t[i].image >= 0)
        width += 20;
    return MAX(width, s->min_w > 0 ? s->min_w : 40);
}

static bool overflow(struct w32_window *w, struct tabs *s)
{
    int x = 0;
    for (int i = 0; i < s->count; i++)
        x += tab_w(w, s, i);
    return x > w->rect.w;
}

static struct rect item_rect(struct w32_window *w, struct tabs *s, int i)
{
    int x = 2;
    for (int k = s->first; k < i && k < s->count; k++)
        x += tab_w(w, s, k);
    if (i < s->first)
        return (struct rect){-1000, 0, 0, 0};
    return (struct rect){x, 0, tab_w(w, s, i), tab_h(w, s)};
}

static void make_visible(struct w32_window *w, struct tabs *s, int i)
{
    if (i < 0)
        return;
    if (i < s->first)
        s->first = i;
    int room = w->rect.w - (overflow(w, s) ? ARROWS_W : 0);
    while (s->first < i) {
        struct rect r = item_rect(w, s, i);
        if (r.x + r.w <= room)
            break;
        s->first++;
    }
}

static LRESULT notify(struct w32_window *w, int code)
{
    if (!w->parent)
        return 0;
    uint8_t *nm = w32_zalloc(24);
    if (!nm)
        return 0;
    uint32_t hwnd = (uint32_t)(uintptr_t)w, id = (uint32_t)w->id;
    memcpy(nm, &hwnd, 4);
    memcpy(nm + (w32->win64 ? 8 : 4), &id, 4);
    memcpy(nm + (w32->win64 ? 16 : 8), &code, 4);
    LRESULT r = w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
    w32_free(nm);
    return r;
}

static void select_tab(struct w32_window *w, struct tabs *s, int i, bool user)
{
    if (i < 0 || i >= s->count || i == s->cur)
        return;
    if (user && notify(w, TCN_SELCHANGING))
        return;
    s->cur = s->focus = i;
    make_visible(w, s, i);
    w32_invalidate(w, NULL, true);
    if (user)
        notify(w, TCN_SELCHANGE);
}

static LRESULT insert(struct w32_window *w, struct tabs *s, int at, const void *item, bool wide)
{
    if (!item)
        return -1;
    if (s->count == s->cap) {
        int cap = s->cap ? s->cap * 2 : 8;
        struct tab *t = w32_zalloc(sizeof(*t) * (size_t)cap);
        if (!t)
            return -1;
        if (s->t)
            memcpy(t, s->t, sizeof(*t) * (size_t)s->count);
        w32_free(s->t);
        s->t = t;
        s->cap = cap;
    }
    at = at < 0 || at > s->count ? s->count : at;
    memmove(s->t + at + 1, s->t + at, sizeof(*s->t) * (size_t)(s->count - at));
    struct tab *t = &s->t[at];
    *t = (struct tab){NULL, -1, 0, 0};
    struct item_view v;
    item_read(item, &v);
    item_apply(t, &v, wide);
    if (!t->text)
        t->text = w32_strdup("");
    s->count++;
    if (s->cur >= at)
        s->cur++;
    if (s->cur < 0)
        s->cur = s->focus = 0;
    w32_invalidate(w, NULL, true);
    return at;
}

static bool remove_tab(struct w32_window *w, struct tabs *s, int i)
{
    if (i < 0 || i >= s->count)
        return false;
    w32_free(s->t[i].text);
    memmove(s->t + i, s->t + i + 1, sizeof(*s->t) * (size_t)(s->count - i - 1));
    s->count--;
    if (s->cur > i || s->cur >= s->count)
        s->cur--;
    if (s->count && s->cur < 0)
        s->cur = 0;
    s->focus = s->cur;
    s->first = MAX(0, MIN(s->first, s->count - 1));
    w32_invalidate(w, NULL, true);
    return true;
}

static void clear(struct tabs *s)
{
    for (int i = 0; i < s->count; i++)
        w32_free(s->t[i].text);
    s->count = 0;
    s->first = 0;
    s->cur = s->focus = -1;
}

static int hit(struct w32_window *w, struct tabs *s, int x, int y)
{
    if (y < 0 || y >= tab_h(w, s))
        return -1;
    for (int i = s->first; i < s->count; i++) {
        struct rect r = item_rect(w, s, i);
        if (x >= r.x && x < r.x + r.w)
            return i;
    }
    return -1;
}

/* TCS_OWNERDRAWFIXED: the parent draws each tab from a DRAWITEMSTRUCT in the program's 32- or 64-bit layout */
static void draw_owner(struct w32_window *w, struct tabs *s, HANDLE dc, int i, struct rect r, bool sel)
{
    uint8_t *d = w32_zalloc(64);
    if (!d || !w->parent) {
        w32_free(d);
        return;
    }
    uint32_t head[5] = {101, (uint32_t)w->id, (uint32_t)i, 1, sel ? 1u : 0u};
    memcpy(d, head, sizeof(head));
    int ps = w32->win64 ? 8 : 4, at = w32->win64 ? 24 : 20;
    uint64_t hwnd = (uintptr_t)w, hdc = (uintptr_t)dc;
    memcpy(d + at, &hwnd, (size_t)ps);
    memcpy(d + at + ps, &hdc, (size_t)ps);
    RECT rc = {r.x, r.y, r.x + r.w, r.y + r.h};
    memcpy(d + at + 2 * ps, &rc, sizeof(rc));
    memcpy(d + at + 2 * ps + 16, &s->t[i].param, (size_t)ps);
    w32_send(w->parent, 0x2b, (WPARAM)w->id, (LPARAM)(uintptr_t)d);
    w32_free(d);
}

static void paint(struct w32_window *w, struct tabs *s)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    const struct ui_font *f = w32_ctl_font(w);
    int h = tab_h(w, s);
    gfx_fill(p.c, p.r, style.window);
    bool buttons = w->style & TCS_BUTTONS;
    if (!buttons) {
        struct rect body = {p.r.x, p.r.y + h - 1, p.r.w, p.r.h - h + 1};
        gfx_round(p.c, body, 4, style.content);
        gfx_round_frame(p.c, body, 4, style.separator);
    }
    bool arrows = overflow(w, s);
    struct rect clip = p.c->clip;
    p.c->clip = rect_intersect(clip, (struct rect){p.r.x, p.r.y, p.r.w - (arrows ? ARROWS_W : 0), p.r.h});
    for (int i = s->first; i < s->count; i++) {
        struct rect r = item_rect(w, s, i);
        if (r.x > w->rect.w)
            break;
        struct rect t = {p.r.x + r.x, p.r.y + 1, r.w - 2, h - 2};
        bool sel = i == s->cur || (s->t[i].state & (TCIS_BUTTONPRESSED | TCIS_HIGHLIGHTED));
        if (sel) {
            struct rect face = buttons ? t : (struct rect){t.x, t.y, t.w, t.h + 3};
            gfx_round(p.c, face, 5, buttons ? style.control_pressed : style.content);
            gfx_round_frame(p.c, face, 5, style.separator);
            if (!buttons)
                gfx_fill(p.c, (struct rect){t.x + 1, t.y + t.h, t.w - 2, 3}, style.content);
        }
        if (w->style & 0x2000) {
            draw_owner(w, s, p.dc, i, (struct rect){r.x, 1, r.w - 2, h - 2}, i == s->cur);
            continue;
        }
        int tx = t.x + s->pad_x;
        if (s->images && s->t[i].image >= 0) {
            typedef BOOL WINAPI (*draw_fn)(HANDLE, int, HANDLE, int, int, UINT);
            draw_fn draw = (draw_fn)w32_builtin_lookup("comctl32.dll", "ImageList_Draw");
            if (draw)
                draw(s->images, s->t[i].image, p.dc, tx - p.r.x, t.y - p.r.y + (t.h - 16) / 2, 1);
            tx += 20;
        }
        uint32_t color = !w->enabled ? style.tertiary : sel ? style.text : style.secondary;
        gfx_text_box(p.c, (struct rect){tx, t.y, t.x + t.w - tx - s->pad_x / 2, t.h}, f, s->t[i].text, color,
                     ALIGN_LEFT);
        if (i == s->focus && w32_focus() == w)
            gfx_round_frame(p.c, (struct rect){t.x + 2, t.y + 2, t.w - 4, t.h - 4}, 4, style.accent);
    }
    p.c->clip = clip;
    if (arrows) {
        int x = p.r.x + p.r.w - ARROWS_W, y = p.r.y + (h - 14) / 2;
        for (int k = 0; k < 2; k++) {
            struct rect b = {x + k * 17, y, 16, 14};
            gfx_round(p.c, b, 3, style.control);
            gfx_round_frame(p.c, b, 3, style.control_border);
            gfx_text_box(p.c, b, &font_sans, k ? ">" : "<", style.text, ALIGN_CENTER);
        }
    }
    w32_ctl_end(w, &p);
}

LRESULT WINAPI w32_tab_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct tabs *s = tabs_of(w);
    if (!s)
        return w32_def_window_proc(h, msg, wp, lp);
    void *ptr = (void *)(uintptr_t)(uint32_t)lp;
    switch (msg) {
    case TCM_GETITEMCOUNT: return s->count;
    case TCM_INSERTITEMA:
    case TCM_INSERTITEMW: return insert(w, s, (int)wp, ptr, msg == TCM_INSERTITEMW);
    case TCM_GETITEMA:
    case TCM_GETITEMW:
        if ((int)wp < 0 || (int)wp >= s->count || !ptr)
            return FALSE;
        item_write(ptr, &s->t[wp], msg == TCM_GETITEMW);
        return TRUE;
    case TCM_SETITEMA:
    case TCM_SETITEMW: {
        if ((int)wp < 0 || (int)wp >= s->count || !ptr)
            return FALSE;
        struct item_view v;
        item_read(ptr, &v);
        item_apply(&s->t[wp], &v, msg == TCM_SETITEMW);
        w32_invalidate(w, NULL, true);
        return TRUE;
    }
    case TCM_DELETEITEM: return remove_tab(w, s, (int)wp);
    case TCM_DELETEALLITEMS:
        clear(s);
        w32_invalidate(w, NULL, true);
        return TRUE;
    case TCM_GETCURSEL: return s->cur;
    case TCM_SETCURSEL: {
        int old = s->cur;
        if ((int)wp < 0 || (int)wp >= s->count)
            return -1;
        select_tab(w, s, (int)wp, false);
        return old;
    }
    case TCM_GETCURFOCUS: return s->focus;
    case TCM_SETCURFOCUS: select_tab(w, s, (int)wp, true); return 0;
    case TCM_GETITEMRECT: {
        if ((int)wp < 0 || (int)wp >= s->count || !ptr)
            return FALSE;
        struct rect r = item_rect(w, s, (int)wp);
        *(RECT *)ptr = (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
        return TRUE;
    }
    case TCM_HITTEST: {
        if (!ptr)
            return -1;
        POINT *pt = ptr;
        int i = hit(w, s, pt->x, pt->y);
        ((UINT *)ptr)[2] = i < 0 ? TCHT_NOWHERE : TCHT_ONITEM;
        return i;
    }
    case TCM_ADJUSTRECT: {
        RECT *r = ptr;
        if (!r)
            return 0;
        int th = tab_h(w, s), d = (w->style & TCS_BUTTONS) ? 0 : 2;
        if (wp) {
            r->left -= d;
            r->right += d;
            r->bottom += d;
            r->top -= th + d;
        } else {
            r->left += d;
            r->right -= d;
            r->bottom -= d;
            r->top += th + d;
        }
        return 0;
    }
    case TCM_SETITEMSIZE: {
        LRESULT old = (LRESULT)((uint32_t)(s->fixed_w ? s->fixed_w : 40) | (uint32_t)tab_h(w, s) << 16);
        s->fixed_w = (int16_t)lp;
        s->fixed_h = (int16_t)(lp >> 16);
        w32_invalidate(w, NULL, true);
        return old;
    }
    case TCM_SETPADDING:
        s->pad_x = MAX(0, (int16_t)lp);
        s->pad_y = MAX(0, (int16_t)(lp >> 16));
        w32_invalidate(w, NULL, true);
        return 0;
    case TCM_SETMINTABWIDTH: {
        int old = s->min_w > 0 ? s->min_w : 40;
        s->min_w = (int)lp;
        return old;
    }
    case TCM_GETROWCOUNT: return 1;
    case TCM_GETIMAGELIST: return (LRESULT)(uintptr_t)s->images;
    case TCM_SETIMAGELIST: {
        HANDLE old = s->images;
        s->images = (HANDLE)(uintptr_t)(uint32_t)lp;
        w32_invalidate(w, NULL, true);
        return (LRESULT)(uintptr_t)old;
    }
    case TCM_REMOVEIMAGE:
        for (int i = 0; i < s->count; i++)
            if (s->t[i].image == (int)wp)
                s->t[i].image = -1;
            else if (s->t[i].image > (int)wp)
                s->t[i].image--;
        return 0;
    case TCM_DESELECTALL:
        for (int i = 0; i < s->count; i++)
            if (!wp || i != s->cur)
                s->t[i].state &= ~(uint32_t)TCIS_BUTTONPRESSED;
        w32_invalidate(w, NULL, true);
        return 0;
    case TCM_HIGHLIGHTITEM:
        if ((int)wp < 0 || (int)wp >= s->count)
            return FALSE;
        s->t[wp].state = (s->t[wp].state & ~(uint32_t)TCIS_HIGHLIGHTED) | ((int16_t)lp ? TCIS_HIGHLIGHTED : 0);
        w32_invalidate(w, NULL, true);
        return TRUE;
    case TCM_SETEXTENDEDSTYLE: {
        uint32_t old = s->ex;
        s->ex = wp ? (s->ex & ~(uint32_t)wp) | ((uint32_t)lp & (uint32_t)wp) : (uint32_t)lp;
        return (LRESULT)old;
    }
    case TCM_GETEXTENDEDSTYLE: return (LRESULT)s->ex;
    case TCM_SETITEMEXTRA: return TRUE;
    case TCM_GETTOOLTIPS:
    case TCM_SETTOOLTIPS: return 0;
    case CCM_SETUNICODEFORMAT: {
        bool old = s->unicode;
        s->unicode = wp != 0;
        return old;
    }
    case CCM_GETUNICODEFORMAT: return s->unicode;
    case WM_PAINT: paint(w, s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: make_visible(w, s, s->cur); return 0;
    case WM_SETFONT:
        w->font = (HANDLE)wp;
        if (lp)
            w32_invalidate(w, NULL, true);
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_GETDLGCODE: return 1;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int x = (int16_t)lp, y = (int16_t)(lp >> 16);
        if (overflow(w, s) && x >= w->rect.w - ARROWS_W && y < tab_h(w, s)) {
            if (msg == WM_LBUTTONDOWN) {
                s->first = x >= w->rect.w - ARROWS_W / 2 ? MIN(s->first + 1, s->count - 1) : MAX(s->first - 1, 0);
                w32_invalidate(w, NULL, true);
            }
            return 0;
        }
        int i = hit(w, s, x, y);
        if (msg == WM_LBUTTONDOWN) {
            if (i >= 0)
                select_tab(w, s, i, true);
            notify(w, NM_CLICK);
        } else {
            notify(w, NM_RCLICK);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_LEFT_ || wp == VK_RIGHT_) {
            select_tab(w, s, s->cur + (wp == VK_LEFT_ ? -1 : 1), true);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: w32_invalidate(w, NULL, false); return 0;
    case WM_ENABLE: w32_invalidate(w, NULL, true); return 0;
    case WM_NCDESTROY_:
        clear(s);
        w32_free(s->t);
        w32_free(s);
        w->list = NULL;
        break;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}
#endif
