
#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>
#include <synth/string.h>

#include "w32gui.h"

/* ListBox and ComboBox: one item list; a combo box shows its pick in a field and opens a desktop menu */
enum {
    LB_ADDSTRING = 0x180,
    LB_INSERTSTRING = 0x181,
    LB_DELETESTRING = 0x182,
    LB_SELITEMRANGEEX = 0x183,
    LB_RESETCONTENT = 0x184,
    LB_SETSEL = 0x185,
    LB_SETCURSEL = 0x186,
    LB_GETSEL = 0x187,
    LB_GETCURSEL = 0x188,
    LB_GETTEXT = 0x189,
    LB_GETTEXTLEN = 0x18a,
    LB_GETCOUNT = 0x18b,
    LB_SELECTSTRING = 0x18c,
    LB_DIR = 0x18d,
    LB_GETTOPINDEX = 0x18e,
    LB_FINDSTRING = 0x18f,
    LB_GETSELCOUNT = 0x190,
    LB_GETSELITEMS = 0x191,
    LB_SETTABSTOPS = 0x192,
    LB_GETHORIZONTALEXTENT = 0x193,
    LB_SETHORIZONTALEXTENT = 0x194,
    LB_SETCOLUMNWIDTH = 0x195,
    LB_ADDFILE = 0x196,
    LB_SETTOPINDEX = 0x197,
    LB_GETITEMRECT = 0x198,
    LB_GETITEMDATA = 0x199,
    LB_SETITEMDATA = 0x19a,
    LB_SELITEMRANGE = 0x19b,
    LB_SETANCHORINDEX = 0x19c,
    LB_GETANCHORINDEX = 0x19d,
    LB_SETCARETINDEX = 0x19e,
    LB_GETCARETINDEX = 0x19f,
    LB_SETITEMHEIGHT = 0x1a0,
    LB_GETITEMHEIGHT = 0x1a1,
    LB_FINDSTRINGEXACT = 0x1a2,
    LB_SETLOCALE = 0x1a5,
    LB_GETLOCALE = 0x1a6,
    LB_SETCOUNT = 0x1a7,
    LB_INITSTORAGE = 0x1a8,
    LB_ITEMFROMPOINT = 0x1a9,
    LB_GETLISTBOXINFO = 0x1b2,
    LBS_NOTIFY = 0x1,
    LBS_SORT = 0x2,
    LBS_MULTIPLESEL = 0x8,
    LBS_OWNERDRAWFIXED = 0x10,
    LBS_OWNERDRAWVARIABLE = 0x20,
    LBS_HASSTRINGS = 0x40,
    LBS_EXTENDEDSEL = 0x800,
    LBS_NOSEL = 0x4000,
    LBN_SELCHANGE = 1,
    LBN_DBLCLK = 2,
    LBN_SETFOCUS = 4,
    LBN_KILLFOCUS = 5,
    CB_GETEDITSEL = 0x140,
    CB_LIMITTEXT = 0x141,
    CB_SETEDITSEL = 0x142,
    CB_ADDSTRING = 0x143,
    CB_DELETESTRING = 0x144,
    CB_DIR = 0x145,
    CB_GETCOUNT = 0x146,
    CB_GETCURSEL = 0x147,
    CB_GETLBTEXT = 0x148,
    CB_GETLBTEXTLEN = 0x149,
    CB_INSERTSTRING = 0x14a,
    CB_RESETCONTENT = 0x14b,
    CB_FINDSTRING = 0x14c,
    CB_SELECTSTRING = 0x14d,
    CB_SETCURSEL = 0x14e,
    CB_SHOWDROPDOWN = 0x14f,
    CB_GETITEMDATA = 0x150,
    CB_SETITEMDATA = 0x151,
    CB_GETDROPPEDCONTROLRECT = 0x152,
    CB_SETITEMHEIGHT = 0x153,
    CB_GETITEMHEIGHT = 0x154,
    CB_SETEXTENDEDUI = 0x155,
    CB_GETEXTENDEDUI = 0x156,
    CB_GETDROPPEDSTATE = 0x157,
    CB_FINDSTRINGEXACT = 0x158,
    CB_SETLOCALE = 0x159,
    CB_GETLOCALE = 0x15a,
    CB_GETTOPINDEX = 0x15b,
    CB_SETTOPINDEX = 0x15c,
    CB_GETHORIZONTALEXTENT = 0x15d,
    CB_SETHORIZONTALEXTENT = 0x15e,
    CB_GETDROPPEDWIDTH = 0x15f,
    CB_SETDROPPEDWIDTH = 0x160,
    CB_INITSTORAGE = 0x161,
    CB_GETCOMBOBOXINFO = 0x164,
    CBS_SIMPLE = 0x1,
    CBS_DROPDOWN = 0x2,
    CBS_DROPDOWNLIST = 0x3,
    CBS_OWNERDRAWFIXED = 0x10,
    CBS_OWNERDRAWVARIABLE = 0x20,
    CBS_SORT = 0x100,
    CBS_HASSTRINGS = 0x200,
    CBN_SELCHANGE = 1,
    CBN_SETFOCUS = 3,
    CBN_KILLFOCUS = 4,
    CBN_EDITCHANGE = 5,
    CBN_EDITUPDATE = 6,
    CBN_DROPDOWN = 7,
    CBN_CLOSEUP = 8,
    CBN_SELENDOK = 9,
    WM_DRAWITEM = 0x2b,
    WM_MEASUREITEM = 0x2c,
    WM_DELETEITEM = 0x2d,
    WM_NCCREATE_ = 0x81,
    WM_NCDESTROY_ = 0x82,
    ODT_LISTBOX = 2,
    ODT_COMBOBOX = 3,
    ODA_DRAWENTIRE = 1,
    ODS_SELECTED = 0x1,
    ODS_DISABLED = 0x4,
    ODS_FOCUS = 0x10,
    ODS_COMBOBOXEDIT = 0x1000,
    LB_ERR = -1,
    POPUP_ITEMS = 48,
};

typedef struct {
    UINT type, id, item, action, state;
    HANDLE hwnd, dc;
    RECT rect;
    uintptr_t data;
} DRAWITEMSTRUCT;

typedef struct {
    UINT type, id, item, width, height;
    uintptr_t data;
} MEASUREITEMSTRUCT;

struct lb_item {
    char *text;
    uintptr_t data;
    int height;
    bool sel;
};

struct item_list {
    struct lb_item *items;
    int count, cap;
    int cur, top, anchor;
    int item_h;
    bool fixed_measured;
    size_t limit;
};

static struct item_list *list_of(struct w32_window *w)
{
    if (!w->list)
        w->list = w32_zalloc(sizeof(struct item_list));
    struct item_list *l = w->list;
    if (l && !l->cap && !l->count)
        l->cur = -1;
    return l;
}

static bool is_combo(struct w32_window *w)
{
    return w->control == CK_COMBOBOX;
}

static bool owner_draw(struct w32_window *w)
{
    return w->style & (is_combo(w) ? (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)
                                   : (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE));
}

static bool has_strings(struct w32_window *w)
{
    return !owner_draw(w) || (w->style & (is_combo(w) ? CBS_HASSTRINGS : LBS_HASSTRINGS));
}

static bool variable(struct w32_window *w)
{
    return w->style & (is_combo(w) ? CBS_OWNERDRAWVARIABLE : LBS_OWNERDRAWVARIABLE);
}

/* 1: lParam is an input string, 2: an output buffer for an item's text, 0: no string */
int w32_string_message(struct w32_window *w, UINT msg)
{
    if ((w->control == CK_EDIT || w->control == CK_COMBOBOX) && msg == 0xc2)
        return 1;
    if (w->control != CK_LISTBOX && w->control != CK_COMBOBOX)
        return 0;
    if (!has_strings(w))
        return 0;
    switch (msg) {
    case LB_ADDSTRING:
    case LB_INSERTSTRING:
    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT:
    case LB_SELECTSTRING:
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case CB_FINDSTRING:
    case CB_FINDSTRINGEXACT:
    case CB_SELECTSTRING: return 1;
    case LB_GETTEXT:
    case CB_GETLBTEXT: return 2;
    default: return 0;
    }
}

static char *text_in(struct w32_window *w, LPARAM lp)
{
    if (!lp)
        return w32_strdup("");
    return w->unicode ? w32_narrow((const WCHAR *)lp) : w32_strdup((const char *)lp);
}

static LRESULT text_out(struct w32_window *w, const char *s, LPARAM lp)
{
    if (!lp)
        return LB_ERR;
    if (w->unicode) {
        size_t n = w32_cp_to_wide(s, (int)strlen(s), (WCHAR *)lp, 0x7fffffff, 65001);
        ((WCHAR *)lp)[n] = 0;
        return (LRESULT)n;
    }
    size_t n = strlen(s);
    memcpy((char *)lp, s, n + 1);
    return (LRESULT)n;
}

static LRESULT text_len(struct w32_window *w, const char *s)
{
    if (!w->unicode)
        return (LRESULT)strlen(s);
    size_t n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++)
        n += (*p & 0xc0) != 0x80 ? (*p >= 0xf0 ? 2 : 1) : 0;
    return (LRESULT)n;
}

static int base_height(struct w32_window *w)
{
    return w32_ctl_font(w)->height + 4;
}

static void measure(struct w32_window *w, struct item_list *l, int index)
{
    if (!owner_draw(w) || !w->parent) {
        l->item_h = l->item_h ? l->item_h : base_height(w);
        return;
    }
    if (!variable(w) && l->fixed_measured)
        return;
    MEASUREITEMSTRUCT *m = w32_zalloc(sizeof(*m));
    if (!m)
        return;
    m->type = is_combo(w) ? ODT_COMBOBOX : ODT_LISTBOX;
    m->id = (UINT)w->id;
    m->item = (UINT)(variable(w) ? index : 0);
    m->width = (UINT)w->rect.w;
    m->height = (UINT)base_height(w);
    m->data = index >= 0 && index < l->count ? l->items[index].data : 0;
    w32_send(w->parent, WM_MEASUREITEM, (WPARAM)w->id, (LPARAM)m);
    int h = MAX(1, (int)m->height);
    if (variable(w) && index >= 0 && index < l->count)
        l->items[index].height = h;
    else {
        l->item_h = h;
        l->fixed_measured = true;
    }
    w32_free(m);
}

static int item_height(struct w32_window *w, struct item_list *l, int i)
{
    if (variable(w) && i >= 0 && i < l->count && l->items[i].height)
        return l->items[i].height;
    if (!l->item_h)
        measure(w, l, -1);
    return l->item_h ? l->item_h : base_height(w);
}

static int compare_items(struct w32_window *w, const char *a, const char *b)
{
    UNUSED(w);
    return str_casecmp(a, b);
}

static LRESULT insert(struct w32_window *w, int at, LPARAM lp, bool sorted)
{
    struct item_list *l = list_of(w);
    if (!l)
        return -2;
    if (l->count == l->cap) {
        int cap = MAX(16, l->cap * 2);
        struct lb_item *items = w32_realloc(l->items, sizeof(*items) * (size_t)cap);
        if (!items)
            return -2;
        l->items = items;
        l->cap = cap;
    }
    struct lb_item it = {0};
    if (has_strings(w)) {
        it.text = text_in(w, lp);
        if (!it.text)
            return -2;
    } else {
        it.data = (uintptr_t)lp;
    }
    if (sorted && it.text) {
        at = 0;
        while (at < l->count && compare_items(w, l->items[at].text ? l->items[at].text : "", it.text) <= 0)
            at++;
    }
    if (at < 0 || at > l->count)
        at = l->count;
    memmove(&l->items[at + 1], &l->items[at], sizeof(*l->items) * (size_t)(l->count - at));
    l->items[at] = it;
    l->count++;
    if (l->cur >= at)
        l->cur++;
    measure(w, l, at);
    w32_invalidate(w, NULL, true);
    return at;
}

static void free_item(struct w32_window *w, struct lb_item *it, int index)
{
    if (owner_draw(w) && w->parent && !w->destroyed) {
        struct {
            UINT type, id, item;
            HANDLE hwnd;
            uintptr_t data;
        } *d = w32_zalloc(sizeof(*d));
        if (d) {
            d->type = is_combo(w) ? ODT_COMBOBOX : ODT_LISTBOX;
            d->id = (UINT)w->id;
            d->item = (UINT)index;
            d->hwnd = w;
            d->data = it->data;
            w32_send(w->parent, WM_DELETEITEM, (WPARAM)w->id, (LPARAM)d);
            w32_free(d);
        }
    }
    w32_free(it->text);
}

static LRESULT remove_item(struct w32_window *w, int i)
{
    struct item_list *l = list_of(w);
    if (!l || i < 0 || i >= l->count)
        return LB_ERR;
    free_item(w, &l->items[i], i);
    memmove(&l->items[i], &l->items[i + 1], sizeof(*l->items) * (size_t)(l->count - i - 1));
    l->count--;
    if (l->cur == i)
        l->cur = -1;
    else if (l->cur > i)
        l->cur--;
    l->top = MIN(l->top, MAX(0, l->count - 1));
    w32_invalidate(w, NULL, true);
    return l->count;
}

static void reset(struct w32_window *w)
{
    struct item_list *l = w->list;
    if (!l)
        return;
    for (int i = 0; i < l->count; i++)
        free_item(w, &l->items[i], i);
    l->count = 0;
    l->cur = -1;
    l->top = 0;
    w32_invalidate(w, NULL, true);
}

static const char *item_text(struct lb_item *it)
{
    return it->text ? it->text : "";
}

static LRESULT find(struct w32_window *w, int start, LPARAM lp, bool exact)
{
    struct item_list *l = list_of(w);
    if (!l || !l->count)
        return LB_ERR;
    if (!has_strings(w)) {
        for (int k = 1; k <= l->count; k++) {
            int i = (start + k) % l->count;
            if (l->items[i].data == (uintptr_t)lp)
                return i;
        }
        return LB_ERR;
    }
    char *s = text_in(w, lp);
    if (!s)
        return LB_ERR;
    size_t n = strlen(s);
    LRESULT found = LB_ERR;
    for (int k = 1; k <= l->count && found == LB_ERR; k++) {
        int i = ((start < 0 ? -1 : start) + k) % l->count;
        const char *t = item_text(&l->items[i]);
        if (exact ? !str_casecmp(t, s) : !str_ncasecmp(t, s, n))
            found = i;
    }
    w32_free(s);
    return found;
}

static int visible_rows(struct w32_window *w, struct item_list *l)
{
    int h = 0, n = 0;
    for (int i = l->top; i < l->count && h + item_height(w, l, i) <= w->rect.h; i++, n++)
        h += item_height(w, l, i);
    return MAX(1, n);
}

static void ensure_visible(struct w32_window *w, struct item_list *l, int i)
{
    if (i < 0)
        return;
    if (i < l->top)
        l->top = i;
    while (i >= l->top + visible_rows(w, l) && l->top < i)
        l->top++;
}

static int item_y(struct w32_window *w, struct item_list *l, int i)
{
    int y = 0;
    for (int k = l->top; k < i; k++)
        y += item_height(w, l, k);
    for (int k = i; k < l->top; k++)
        y -= item_height(w, l, k);
    return y;
}

static int item_at(struct w32_window *w, struct item_list *l, int y)
{
    int at = 0;
    for (int i = l->top; i < l->count; i++) {
        int h = item_height(w, l, i);
        if (y >= at && y < at + h)
            return i;
        at += h;
    }
    return -1;
}

static void draw_owner(struct w32_window *w, HANDLE dc, int index, RECT r, UINT state, uintptr_t data)
{
    DRAWITEMSTRUCT *d = w32_zalloc(sizeof(*d));
    if (!d)
        return;
    d->type = is_combo(w) ? ODT_COMBOBOX : ODT_LISTBOX;
    d->id = (UINT)w->id;
    d->item = (UINT)index;
    d->action = ODA_DRAWENTIRE;
    d->state = state | (w->enabled ? 0 : ODS_DISABLED);
    d->hwnd = w;
    d->dc = dc;
    d->rect = r;
    d->data = data;
    w32_send(w->parent, WM_DRAWITEM, (WPARAM)w->id, (LPARAM)d);
    w32_free(d);
}

static uint32_t text_color(struct w32_window *w, struct w32_paint *p)
{
    if (!w->enabled)
        return style.tertiary;
    return p->custom_text ? w32_rgb(p->text) : style.text;
}

static bool multi(struct w32_window *w)
{
    return !is_combo(w) && (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL));
}

static bool selected(struct w32_window *w, struct item_list *l, int i)
{
    return multi(w) ? l->items[i].sel : i == l->cur;
}

static void paint_list(struct w32_window *w)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, WM_CTLCOLORLISTBOX))
        return;
    struct item_list *l = list_of(w);
    const struct ui_font *f = w32_ctl_font(w);
    bool framed = (w->style & WS_BORDER) || (w->exstyle & 0x200);
    if (framed) {
        gfx_round(p.c, p.r, 5, style.field);
        gfx_round_frame(p.c, p.r, 5, w32_focus() == w ? style.accent : style.field_border);
    } else {
        gfx_fill(p.c, p.r, style.field);
    }
    if (l) {
        struct rect inner = framed ? (struct rect){p.r.x + 1, p.r.y + 1, p.r.w - 2, p.r.h - 2} : p.r;
        struct rect clip = p.c->clip;
        p.c->clip = rect_intersect(clip, inner);
        int y = 0;
        for (int i = l->top; i < l->count && y < w->rect.h; i++) {
            int h = item_height(w, l, i);
            bool sel = selected(w, l, i);
            if (owner_draw(w)) {
                RECT r = {0, y, w->rect.w, y + h};
                draw_owner(w, p.dc, i, r, (sel ? ODS_SELECTED : 0) | (w32_focus() == w && i == l->cur ? ODS_FOCUS : 0),
                           l->items[i].data);
                p.c->clip = rect_intersect(clip, inner);
            } else {
                struct rect row = {p.r.x + 2, p.r.y + y, p.r.w - 4, h};
                if (sel)
                    gfx_round(p.c, row, 4, style.accent);
                gfx_text_box(p.c, (struct rect){row.x + 4, row.y, row.w - 8, h}, f, item_text(&l->items[i]),
                             sel ? style.accent_text : text_color(w, &p), ALIGN_LEFT);
            }
            y += h;
        }
        int rows = visible_rows(w, l);
        if (l->count > rows) {
            int track = inner.h, knob = MAX(12, track * rows / l->count);
            int at = (track - knob) * l->top / MAX(1, l->count - rows);
            gfx_round(p.c, (struct rect){inner.x + inner.w - 5, inner.y + at, 3, knob}, 1, style.tertiary);
        }
        p.c->clip = clip;
    }
    w32_ctl_end(w, &p);
}

int w32_combo_field_height(struct w32_window *w)
{
    if ((w->style & 3) == CBS_SIMPLE)
        return w->rect.h;
    struct item_list *l = w->list;
    int h = l && owner_draw(w) && l->item_h ? l->item_h + 6 : w32_ctl_font(w)->height + 10;
    return MAX(h, 20);
}

static void paint_combo(struct w32_window *w)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, WM_CTLCOLOREDIT))
        return;
    struct item_list *l = list_of(w);
    const struct ui_font *f = w32_ctl_font(w);
    struct rect r = p.r;
    bool focused = w32_focus() == w;
    gfx_round(p.c, r, 5, w->enabled ? style.field : style.window);
    gfx_round_frame(p.c, r, 5, focused ? style.accent : style.field_border);
    int arrow = 18;
    struct rect field = {r.x + 6, r.y, r.w - 12 - arrow, r.h};
    struct rect clip = p.c->clip;
    if (owner_draw(w) && l && l->cur >= 0 && l->cur < l->count) {
        RECT ir = {3, 3, w->rect.w - arrow - 3, w->rect.h - 3};
        draw_owner(w, p.dc, l->cur, ir, ODS_COMBOBOXEDIT | (focused ? ODS_FOCUS | ODS_SELECTED : 0),
                   l->items[l->cur].data);
        p.c->clip = clip;
    } else {
        const char *text = (w->style & 3) == CBS_DROPDOWNLIST && l && l->cur >= 0 && l->cur < l->count
                               ? item_text(&l->items[l->cur])
                               : w->text ? w->text : "";
        p.c->clip = rect_intersect(clip, field);
        gfx_text_box(p.c, field, f, text, text_color(w, &p), ALIGN_LEFT);
        if (focused && (w->style & 3) == CBS_DROPDOWN) {
            int cx = field.x + gfx_text_width(f, text);
            gfx_fill(p.c, (struct rect){cx, r.y + (r.h - f->height) / 2, 1, f->height}, style.accent);
        }
        p.c->clip = clip;
    }
    int ax = r.x + r.w - arrow + 2, ay = r.y + r.h / 2 - 2;
    for (int k = 0; k < 4; k++)
        gfx_fill(p.c, (struct rect){ax + k, ay + k, 9 - 2 * k, 1}, w->enabled ? style.secondary : style.tertiary);
    w32_ctl_end(w, &p);
}

static void select_index(struct w32_window *w, int i, bool notify)
{
    struct item_list *l = list_of(w);
    if (!l || i < -1 || i >= l->count)
        return;
    if (multi(w)) {
        if (!(w->style & LBS_MULTIPLESEL))
            for (int k = 0; k < l->count; k++)
                l->items[k].sel = false;
        if (i >= 0)
            l->items[i].sel = (w->style & LBS_MULTIPLESEL) ? !l->items[i].sel : true;
    }
    bool changed = l->cur != i || multi(w);
    l->cur = i;
    ensure_visible(w, l, i);
    if (is_combo(w) && i >= 0 && has_strings(w)) {
        char *t = w32_strdup(item_text(&l->items[i]));
        if (t) {
            w32_free(w->text);
            w->text = t;
        }
    }
    w32_invalidate(w, NULL, true);
    if (notify && changed && (is_combo(w) || (w->style & LBS_NOTIFY)))
        w32_ctl_notify(w, LBN_SELCHANGE);
}

static void drop_down(struct w32_window *w)
{
    struct item_list *l = list_of(w);
    if (!l)
        return;
    w32_ctl_notify(w, CBN_DROPDOWN);
    int n = MIN(l->count, POPUP_ITEMS);
    struct menu_item *items = w32_zalloc(sizeof(*items) * (size_t)MAX(n, 1));
    if (!items)
        return;
    for (int i = 0; i < n; i++) {
        items[i].label = has_strings(w) ? item_text(&l->items[i]) : "";
        items[i].key = i;
    }
    int x, y;
    w32_client_origin(w, &x, &y);
    int pick = n ? w32_popup_choose(items, n, x, y + w->rect.h) : -1;
    w32_free(items);
    w32_ctl_notify(w, CBN_CLOSEUP);
    if (pick >= 0 && pick < l->count) {
        select_index(w, pick, true);
        w32_ctl_notify(w, CBN_SELENDOK);
    }
}

static LRESULT get_text(struct w32_window *w, int i, LPARAM lp)
{
    struct item_list *l = list_of(w);
    if (!l || i < 0 || i >= l->count)
        return LB_ERR;
    if (!has_strings(w)) {
        if (lp)
            *(uintptr_t *)lp = l->items[i].data;
        return sizeof(uintptr_t);
    }
    return text_out(w, item_text(&l->items[i]), lp);
}

static LRESULT list_key(struct w32_window *w, struct item_list *l, WPARAM vk)
{
    int i = l->cur, rows = visible_rows(w, l);
    switch (vk) {
    case 0x26: i = MAX(0, i - 1); break;
    case 0x28: i = MIN(l->count - 1, i + 1); break;
    case 0x21: i = MAX(0, i - rows); break;
    case 0x22: i = MIN(l->count - 1, i + rows); break;
    case 0x24: i = 0; break;
    case 0x23: i = l->count - 1; break;
    case 0x20:
        if (multi(w) && l->cur >= 0)
            select_index(w, l->cur, true);
        return 0;
    default: return 0;
    }
    if (l->count && i != l->cur) {
        if (multi(w)) {
            l->cur = i;
            ensure_visible(w, l, i);
            w32_invalidate(w, NULL, true);
        } else {
            select_index(w, i, true);
        }
    }
    return 0;
}

/* messages both kinds share; *handled is false for the rest */
static LRESULT common(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp, bool *handled)
{
    struct item_list *l = list_of(w);
    bool combo = is_combo(w);
    *handled = true;
    if (!l)
        return LB_ERR;
    if (combo && msg >= CB_ADDSTRING && msg <= CB_INITSTORAGE) {
        static const uint16_t map[] = {
            [CB_ADDSTRING - CB_ADDSTRING] = LB_ADDSTRING,         [CB_DELETESTRING - CB_ADDSTRING] = LB_DELETESTRING,
            [CB_DIR - CB_ADDSTRING] = LB_DIR,                     [CB_GETCOUNT - CB_ADDSTRING] = LB_GETCOUNT,
            [CB_GETCURSEL - CB_ADDSTRING] = LB_GETCURSEL,         [CB_GETLBTEXT - CB_ADDSTRING] = LB_GETTEXT,
            [CB_GETLBTEXTLEN - CB_ADDSTRING] = LB_GETTEXTLEN,     [CB_INSERTSTRING - CB_ADDSTRING] = LB_INSERTSTRING,
            [CB_RESETCONTENT - CB_ADDSTRING] = LB_RESETCONTENT,   [CB_FINDSTRING - CB_ADDSTRING] = LB_FINDSTRING,
            [CB_SELECTSTRING - CB_ADDSTRING] = LB_SELECTSTRING,   [CB_SETCURSEL - CB_ADDSTRING] = LB_SETCURSEL,
            [CB_GETITEMDATA - CB_ADDSTRING] = LB_GETITEMDATA,     [CB_SETITEMDATA - CB_ADDSTRING] = LB_SETITEMDATA,
            [CB_SETITEMHEIGHT - CB_ADDSTRING] = LB_SETITEMHEIGHT, [CB_GETITEMHEIGHT - CB_ADDSTRING] = LB_GETITEMHEIGHT,
            [CB_FINDSTRINGEXACT - CB_ADDSTRING] = LB_FINDSTRINGEXACT,
            [CB_GETTOPINDEX - CB_ADDSTRING] = LB_GETTOPINDEX,     [CB_SETTOPINDEX - CB_ADDSTRING] = LB_SETTOPINDEX,
            [CB_INITSTORAGE - CB_ADDSTRING] = LB_INITSTORAGE,
        };
        UINT m = map[msg - CB_ADDSTRING];
        if (m) {
            if (m == LB_GETITEMHEIGHT && (int)wp == -1)
                return w32_combo_field_height(w);
            if (m == LB_SETITEMHEIGHT && (int)wp == -1)
                return 0;
            msg = m;
        }
    }
    bool sort = combo ? (w->style & CBS_SORT) : (w->style & LBS_SORT);
    switch (msg) {
    case LB_ADDSTRING: return insert(w, -1, lp, sort && has_strings(w));
    case LB_INSERTSTRING: return insert(w, (int)wp, lp, false);
    case LB_DELETESTRING: return remove_item(w, (int)wp);
    case LB_RESETCONTENT: reset(w); return 0;
    case LB_GETCOUNT: return l->count;
    case LB_GETCURSEL: return multi(w) ? l->cur : l->cur;
    case LB_SETCURSEL:
        if ((int)wp < -1 || (int)wp >= l->count) {
            select_index(w, -1, false);
            return LB_ERR;
        }
        select_index(w, (int)wp, false);
        return (int)wp;
    case LB_GETTEXT: return get_text(w, (int)wp, lp);
    case LB_GETTEXTLEN:
        if ((int)wp < 0 || (int)wp >= l->count)
            return LB_ERR;
        return has_strings(w) ? text_len(w, item_text(&l->items[wp])) : (LRESULT)sizeof(uintptr_t);
    case LB_GETITEMDATA: return (int)wp >= 0 && (int)wp < l->count ? (LRESULT)l->items[wp].data : LB_ERR;
    case LB_SETITEMDATA:
        if ((int)wp < 0 || (int)wp >= l->count)
            return LB_ERR;
        l->items[wp].data = (uintptr_t)lp;
        return TRUE;
    case LB_FINDSTRING: return find(w, (int)wp, lp, false);
    case LB_FINDSTRINGEXACT: return find(w, (int)wp, lp, true);
    case LB_SELECTSTRING: {
        LRESULT i = find(w, (int)wp, lp, false);
        if (i >= 0)
            select_index(w, (int)i, false);
        return i;
    }
    case LB_GETTOPINDEX: return l->top;
    case LB_SETTOPINDEX:
        if ((int)wp < 0 || (int)wp >= MAX(l->count, 1))
            return LB_ERR;
        l->top = (int)wp;
        w32_invalidate(w, NULL, true);
        return 0;
    case LB_SETITEMHEIGHT:
        if (variable(w) && (int)wp >= 0 && (int)wp < l->count)
            l->items[wp].height = (int)(lp & 0xffff);
        else
            l->item_h = (int)(lp & 0xffff);
        w32_invalidate(w, NULL, true);
        return 0;
    case LB_GETITEMHEIGHT: return item_height(w, l, (int)wp);
    case LB_INITSTORAGE: return l->cap;
    case LB_DIR: return LB_ERR;
    case WM_SETFONT:
        w->font = (HANDLE)wp;
        if (!owner_draw(w))
            l->item_h = 0;
        if (combo)
            w->rect.h = w32_combo_field_height(w);
        if (lp)
            w32_invalidate(w, NULL, true);
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_NCDESTROY_:
        reset(w);
        w32_free(l->items);
        w32_free(l);
        w->list = NULL;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ENABLE: w32_invalidate(w, NULL, true); return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        w32_invalidate(w, NULL, true);
        if (combo || (w->style & LBS_NOTIFY))
            w32_ctl_notify(w, msg == WM_SETFOCUS ? (combo ? CBN_SETFOCUS : LBN_SETFOCUS)
                                                 : (combo ? CBN_KILLFOCUS : LBN_KILLFOCUS));
        return 0;
    default: *handled = false; return 0;
    }
}

static void init_kind(struct w32_window *w, enum control_kind kind)
{
    if (w->control != CK_NONE)
        return;
    w->control = kind;
    if (kind == CK_COMBOBOX)
        w->rect.h = w32_combo_field_height(w);
}

LRESULT WINAPI w32_listbox_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    extern LRESULT WINAPI w32_def_window_proc(HANDLE, UINT, WPARAM, LPARAM);
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    init_kind(w, CK_LISTBOX);
    bool handled;
    LRESULT r = common(w, msg, wp, lp, &handled);
    if (handled)
        return r;
    struct item_list *l = list_of(w);
    if (!l)
        return w32_def_window_proc(h, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: paint_list(w); return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        w32_set_focus(w);
        int i = item_at(w, l, (int16_t)(lp >> 16));
        if (i >= 0 && !(w->style & LBS_NOSEL)) {
            if (msg == WM_LBUTTONDBLCLK && i == l->cur) {
                if (w->style & LBS_NOTIFY)
                    w32_ctl_notify(w, LBN_DBLCLK);
            } else {
                select_index(w, i, true);
            }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int steps = -(int16_t)(wp >> 16) / 40;
        l->top = MAX(0, MIN(l->top + steps, l->count - visible_rows(w, l)));
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case WM_VSCROLL: {
        int rows = visible_rows(w, l), code = (int)(wp & 0xffff);
        int top = code == 0 ? l->top - 1 : code == 1 ? l->top + 1 : code == 2 ? l->top - rows
                  : code == 3 ? l->top + rows : code == 6 ? 0 : code == 7 ? l->count : l->top;
        l->top = MAX(0, MIN(top, l->count - rows));
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case WM_KEYDOWN: return list_key(w, l, wp);
    case WM_GETDLGCODE: return 0x81;
    case LB_GETSEL:
        if ((int)wp < 0 || (int)wp >= l->count)
            return LB_ERR;
        return selected(w, l, (int)wp);
    case LB_SETSEL: {
        if (!multi(w))
            return LB_ERR;
        int from = (int)lp == -1 ? 0 : (int)lp, to = (int)lp == -1 ? l->count - 1 : (int)lp;
        for (int i = MAX(0, from); i <= to && i < l->count; i++)
            l->items[i].sel = wp != 0;
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case LB_SELITEMRANGE:
    case LB_SELITEMRANGEEX: {
        if (!multi(w))
            return LB_ERR;
        int a = msg == LB_SELITEMRANGE ? (int)(lp & 0xffff) : (int)wp;
        int b = msg == LB_SELITEMRANGE ? (int)((uint32_t)lp >> 16) : (int)lp;
        bool on = msg == LB_SELITEMRANGE ? wp != 0 : a <= b;
        for (int i = MAX(0, MIN(a, b)); i <= MAX(a, b) && i < l->count; i++)
            l->items[i].sel = on;
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case LB_GETSELCOUNT: {
        if (!multi(w))
            return LB_ERR;
        int n = 0;
        for (int i = 0; i < l->count; i++)
            n += l->items[i].sel;
        return n;
    }
    case LB_GETSELITEMS: {
        if (!multi(w))
            return LB_ERR;
        int n = 0;
        for (int i = 0; i < l->count && n < (int)wp; i++)
            if (l->items[i].sel)
                ((int *)lp)[n++] = i;
        return n;
    }
    case LB_GETITEMRECT: {
        if ((int)wp < 0 || (int)wp >= l->count || !lp)
            return LB_ERR;
        int y = item_y(w, l, (int)wp);
        *(RECT *)lp = (RECT){0, y, w->rect.w, y + item_height(w, l, (int)wp)};
        return 1;
    }
    case LB_ITEMFROMPOINT: {
        int y = (int16_t)(lp >> 16), i = item_at(w, l, y);
        return i >= 0 ? i : (LRESULT)((uint32_t)MAX(0, l->count - 1) | 0x10000u);
    }
    case LB_SETCARETINDEX:
        if ((int)wp < 0 || (int)wp >= l->count)
            return LB_ERR;
        l->cur = (int)wp;
        ensure_visible(w, l, l->cur);
        w32_invalidate(w, NULL, true);
        return 0;
    case LB_GETCARETINDEX: return l->cur;
    case LB_SETANCHORINDEX: l->anchor = (int)wp; return 0;
    case LB_GETANCHORINDEX: return l->anchor;
    case LB_SETCOUNT: return LB_ERR;
    case LB_SETTABSTOPS:
    case LB_SETCOLUMNWIDTH:
    case LB_SETHORIZONTALEXTENT: return TRUE;
    case LB_GETHORIZONTALEXTENT:
    case LB_GETLOCALE:
    case LB_SETLOCALE: return 0;
    case LB_ADDFILE: return LB_ERR;
    case LB_GETLISTBOXINFO: return visible_rows(w, l);
    default: return w32_def_window_proc(h, msg, wp, lp);
    }
}

static void combo_edit_char(struct w32_window *w, WPARAM ch)
{
    size_t len = w->text ? strlen(w->text) : 0;
    struct item_list *l = list_of(w);
    if (ch == 8) {
        if (!len)
            return;
        while (len && ((uint8_t)w->text[len - 1] & 0xc0) == 0x80)
            len--;
        if (len)
            len--;
        w->text[len] = 0;
    } else if (ch >= 32) {
        if (l && l->limit && len >= l->limit)
            return;
        char u[4] = {0};
        if (ch < 0x80) {
            u[0] = (char)ch;
        } else if (ch < 0x800) {
            u[0] = (char)(0xc0 | ch >> 6);
            u[1] = (char)(0x80 | (ch & 0x3f));
        } else {
            u[0] = (char)(0xe0 | ch >> 12);
            u[1] = (char)(0x80 | (ch >> 6 & 0x3f));
            u[2] = (char)(0x80 | (ch & 0x3f));
        }
        char *t = w32_alloc(len + strlen(u) + 1);
        if (!t)
            return;
        memcpy(t, w->text ? w->text : "", len);
        memcpy(t + len, u, strlen(u) + 1);
        w32_free(w->text);
        w->text = t;
    } else {
        return;
    }
    if (l)
        l->cur = -1;
    w32_invalidate(w, NULL, true);
    w32_ctl_notify(w, CBN_EDITUPDATE);
    w32_ctl_notify(w, CBN_EDITCHANGE);
}

LRESULT WINAPI w32_combobox_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    extern LRESULT WINAPI w32_def_window_proc(HANDLE, UINT, WPARAM, LPARAM);
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    init_kind(w, CK_COMBOBOX);
    bool handled;
    LRESULT r = common(w, msg, wp, lp, &handled);
    if (handled)
        return r;
    struct item_list *l = list_of(w);
    if (!l)
        return w32_def_window_proc(h, msg, wp, lp);
    bool list_only = (w->style & 3) == CBS_DROPDOWNLIST;
    switch (msg) {
    case WM_PAINT: paint_combo(w); return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        w32_set_focus(w);
        if ((w->style & 3) != CBS_SIMPLE && (list_only || (int16_t)(lp & 0xffff) > w->rect.w - 24))
            drop_down(w);
        return 0;
    case WM_MOUSEWHEEL:
        if (w32_focus() == w && l->count)
            select_index(w, MAX(0, MIN(l->count - 1, l->cur + ((int16_t)(wp >> 16) > 0 ? -1 : 1))), true);
        return 0;
    case WM_KEYDOWN:
        if ((wp == 0x0d || wp == 0x1b) && w32_comboex_key(w, wp))
            return 0;
        if (wp == 0x73 || (wp == 0x28 && (w32_get_key_state(0x12) & 0x8000))) {
            drop_down(w);
            return 0;
        }
        if (wp == 0x26 || wp == 0x28 || wp == 0x21 || wp == 0x22 || (list_only && (wp == 0x24 || wp == 0x23))) {
            int i = l->cur;
            i = wp == 0x26 ? i - 1 : wp == 0x28 ? i + 1 : wp == 0x21 || wp == 0x24 ? 0 : l->count - 1;
            if (l->count)
                select_index(w, MAX(0, MIN(l->count - 1, i)), true);
        }
        return 0;
    case WM_CHAR:
        if ((w->style & 3) != CBS_DROPDOWNLIST)
            combo_edit_char(w, wp);
        else if (wp >= 32 && l->count) {
            char key[2] = {(char)wp, 0};
            for (int k = 1; k <= l->count; k++) {
                int i = (l->cur + k) % l->count;
                if (!str_ncasecmp(item_text(&l->items[i]), key, 1)) {
                    select_index(w, i, true);
                    break;
                }
            }
        }
        return 0;
    case WM_GETDLGCODE: return 0x81;
    case WM_SETTEXT: {
        LRESULT res = w32_def_window_proc(h, msg, wp, lp);
        if (list_only) {
            LRESULT i = find(w, -1, lp, true);
            l->cur = (int)i;
        }
        w32_invalidate(w, NULL, true);
        return res;
    }
    case CB_SHOWDROPDOWN:
        if (wp)
            drop_down(w);
        return TRUE;
    case CB_GETDROPPEDSTATE: return FALSE;
    case CB_GETDROPPEDCONTROLRECT:
        if (lp) {
            int x, y;
            w32_client_origin(w, &x, &y);
            *(RECT *)lp = (RECT){x, y, x + w->rect.w, y + w->rect.h + 200};
        }
        return TRUE;
    case CB_LIMITTEXT: l->limit = wp; return TRUE;
    case CB_GETEDITSEL: {
        DWORD n = w->text ? (DWORD)strlen(w->text) : 0;
        if (wp)
            *(DWORD *)wp = n;
        if (lp)
            *(DWORD *)lp = n;
        return (LRESULT)(n | n << 16);
    }
    case CB_SETEDITSEL: return list_only ? LB_ERR : TRUE;
    case CB_SETEXTENDEDUI:
    case CB_SETHORIZONTALEXTENT:
    case CB_SETLOCALE: return 0;
    case CB_GETEXTENDEDUI:
    case CB_GETHORIZONTALEXTENT:
    case CB_GETLOCALE: return 0;
    case CB_GETDROPPEDWIDTH: return w->rect.w;
    case CB_SETDROPPEDWIDTH: return w->rect.w;
    case CB_GETCOMBOBOXINFO: {
        struct {
            DWORD size;
            RECT item, button;
            DWORD state;
            HANDLE combo, item_wnd, list;
        } *info = (void *)lp;
        if (!info)
            return FALSE;
        info->item = (RECT){3, 3, w->rect.w - 21, w->rect.h - 3};
        info->button = (RECT){w->rect.w - 18, 0, w->rect.w, w->rect.h};
        info->state = 0;
        info->combo = w;
        info->item_wnd = list_only ? NULL : w;
        info->list = NULL;
        return TRUE;
    }
    default: return w32_def_window_proc(h, msg, wp, lp);
    }
}
#endif
