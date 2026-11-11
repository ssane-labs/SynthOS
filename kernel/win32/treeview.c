#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>
#include <synth/string.h>

#include "w32gui.h"
#include "win64.h"

/* SysTreeView32: items with buttons, lines of indent, images, check boxes, text callbacks and the selection/expand
   notifications; TVITEM/TVINSERTSTRUCT/NMTREEVIEW come in the caller's own 32- or 64-bit layout */
enum {
    TVM_INSERTITEMA = 0x1100,
    TVM_DELETEITEM = 0x1101,
    TVM_EXPAND = 0x1102,
    TVM_GETITEMRECT = 0x1104,
    TVM_GETCOUNT = 0x1105,
    TVM_GETINDENT = 0x1106,
    TVM_SETINDENT = 0x1107,
    TVM_GETIMAGELIST = 0x1108,
    TVM_SETIMAGELIST = 0x1109,
    TVM_GETNEXTITEM = 0x110a,
    TVM_SELECTITEM = 0x110b,
    TVM_GETITEMA = 0x110c,
    TVM_SETITEMA = 0x110d,
    TVM_GETVISIBLECOUNT = 0x1110,
    TVM_HITTEST = 0x1111,
    TVM_SORTCHILDREN = 0x1113,
    TVM_ENSUREVISIBLE = 0x1114,
    TVM_SORTCHILDRENCB = 0x1115,
    TVM_SETITEMHEIGHT = 0x111b,
    TVM_GETITEMHEIGHT = 0x111c,
    TVM_SETBKCOLOR = 0x111d,
    TVM_SETTEXTCOLOR = 0x111e,
    TVM_GETBKCOLOR = 0x111f,
    TVM_GETTEXTCOLOR = 0x1120,
    TVM_GETITEMSTATE = 0x1127,
    TVM_SETEXTENDEDSTYLE = 0x112c,
    TVM_GETEXTENDEDSTYLE = 0x112d,
    TVM_INSERTITEMW = 0x1132,
    TVM_GETITEMW = 0x113e,
    TVM_SETITEMW = 0x113f,
    TVIF_TEXT = 1,
    TVIF_IMAGE = 2,
    TVIF_PARAM = 4,
    TVIF_STATE = 8,
    TVIF_HANDLE = 0x10,
    TVIF_SELECTEDIMAGE = 0x20,
    TVIF_CHILDREN = 0x40,
    TVIS_SELECTED = 2,
    TVIS_BOLD = 0x10,
    TVIS_EXPANDED = 0x20,
    TVIS_EXPANDEDONCE = 0x40,
    TVS_HASBUTTONS = 1,
    TVS_HASLINES = 2,
    TVS_LINESATROOT = 4,
    TVS_CHECKBOXES = 0x100,
    TVN_SELCHANGINGA = -401,
    TVN_SELCHANGEDA = -402,
    TVN_GETDISPINFOA = -403,
    TVN_ITEMEXPANDINGA = -405,
    TVN_ITEMEXPANDEDA = -406,
    TVN_DELETEITEMA = -409,
    TVN_KEYDOWN = -412,
    TVN_SELCHANGINGW = -450,
    TVN_SELCHANGEDW = -451,
    TVN_GETDISPINFOW = -452,
    TVN_ITEMEXPANDINGW = -454,
    TVN_ITEMEXPANDEDW = -455,
    TVN_DELETEITEMW = -458,
    NM_CLICK = -2,
    NM_DBLCLK = -3,
    NM_RETURN = -4,
    NM_RCLICK = -5,
    NM_SETFOCUS = -7,
    NM_KILLFOCUS = -8,
    TVI_ROOT = 0xffff0000u,
    TVI_FIRST = 0xffff0001u,
    TVI_LAST = 0xffff0002u,
    TVI_SORT = 0xffff0003u,
    WM_NCDESTROY_ = 0x82,
    WM_LBUTTONUP_ = 0x202,
};

#define CALLBACK_TEXT ((char *)1)

struct tv_node {
    uint32_t magic;
    struct tv_node *parent, *child, *next, *prev;
    char *text;
    int image, sel_image, children;
    uint32_t state;
    uint64_t param;
};

struct tv {
    struct tv_node *first, *sel;
    int count, top, indent, item_h;
    HANDLE images[3];
    COLORREF bk, text;
    uint32_t ex;
    int8_t notify_wide;
};

#define NODE_MAGIC 0x54564e44u

static struct tv *tv_of(struct w32_window *w)
{
    if (!w->list) {
        struct tv *s = w32_zalloc(sizeof(*s));
        if (!s)
            return NULL;
        s->indent = 19;
        s->bk = s->text = 0xffffffffu;
        w->list = s;
    }
    return w->list;
}

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

static void put_ptr(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, (size_t)ptr_size());
}

static int get32(const uint8_t *p)
{
    int v;
    memcpy(&v, p, 4);
    return v;
}

static void put32(uint8_t *p, int v)
{
    memcpy(p, &v, 4);
}

/* TVITEM offsets: hItem, state, stateMask, pszText, cchTextMax, iImage, iSelectedImage, cChildren, lParam */
enum { F_ITEM, F_STATE, F_SMASK, F_TEXT, F_CCH, F_IMAGE, F_SEL, F_CHILDREN, F_PARAM, F_SIZE };

static int off(int f)
{
    static const uint8_t o32[] = {4, 8, 12, 16, 20, 24, 28, 32, 36, 40}, o64[] = {8, 16, 20, 24, 32, 36, 40, 44, 48, 56};
    return w32->win64 ? o64[f] : o32[f];
}

static struct tv_node *node(uintptr_t h)
{
    struct tv_node *n = (struct tv_node *)h;
    return h && (uint32_t)h < 0xffff0000u && n->magic == NODE_MAGIC ? n : NULL;
}

static int hdr_size(void)
{
    return w32->win64 ? 24 : 12;
}

static bool wide_notify(struct w32_window *w, struct tv *s)
{
    if (!s->notify_wide && w->parent) {
        LRESULT f = w32_send(w->parent, 0x55, (WPARAM)(uintptr_t)w, 3);
        s->notify_wide = f == 2 || (f != 1 && w->parent->unicode) ? 1 : -1;
    }
    return s->notify_wide > 0;
}

static uint8_t *nm_new(struct w32_window *w, int code, size_t body)
{
    uint8_t *nm = w32_zalloc((size_t)hdr_size() + body + 16);
    if (!nm)
        return NULL;
    put_ptr(nm, (uintptr_t)w);
    put_ptr(nm + ptr_size(), (uint64_t)w->id);
    put32(nm + 2 * ptr_size(), code);
    return nm;
}

static LRESULT nm_send(struct w32_window *w, uint8_t *nm)
{
    LRESULT r = w->parent ? w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm) : 0;
    w32_free(nm);
    return r;
}

static LRESULT notify(struct w32_window *w, int code)
{
    uint8_t *nm = nm_new(w, code, 0);
    return nm ? nm_send(w, nm) : 0;
}

static void fill_item(uint8_t *it, struct tv_node *n)
{
    put32(it, TVIF_HANDLE | TVIF_STATE | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN);
    if (!n)
        return;
    put_ptr(it + off(F_ITEM), (uintptr_t)n);
    put32(it + off(F_STATE), (int)n->state);
    put32(it + off(F_SMASK), -1);
    put32(it + off(F_IMAGE), n->image);
    put32(it + off(F_SEL), n->sel_image);
    put32(it + off(F_CHILDREN), n->children);
    memcpy(it + off(F_PARAM), &n->param, (size_t)ptr_size());
}

/* NMTREEVIEW: action, itemOld, itemNew, ptDrag */
static LRESULT notify_tree(struct w32_window *w, struct tv *s, int code_a, int code_w, int action, struct tv_node *old,
                           struct tv_node *now)
{
    int item = off(F_SIZE), first = w32->win64 ? 8 : 4;
    uint8_t *nm = nm_new(w, wide_notify(w, s) ? code_w : code_a, (size_t)(first + 2 * item + 8));
    if (!nm)
        return 0;
    uint8_t *b = nm + hdr_size();
    put32(b, action);
    fill_item(b + first, old);
    fill_item(b + first + item, now);
    return nm_send(w, nm);
}

static char *dup_text(uintptr_t text, bool wide)
{
    if (text == (uintptr_t)-1 || text == 0xffffffffu)
        return CALLBACK_TEXT;
    if (!text)
        return w32_strdup("");
    return wide ? w32_narrow((const WCHAR *)text) : w32_strdup((const char *)text);
}

static void free_text(struct tv_node *n)
{
    if (n->text != CALLBACK_TEXT)
        w32_free(n->text);
    n->text = NULL;
}

/* text (and image) of an item, asking the parent for callbacks */
static const char *item_text(struct w32_window *w, struct tv *s, struct tv_node *n, char *buf, size_t cap, int *image)
{
    bool sel = n == s->sel;
    if (image)
        *image = sel ? n->sel_image : n->image;
    bool ask_image = image && *image == -1, ask_text = n->text == CALLBACK_TEXT;
    if (!ask_text && !ask_image)
        return n->text ? n->text : "";
    bool wide = wide_notify(w, s);
    uint8_t *nm = nm_new(w, wide ? TVN_GETDISPINFOW : TVN_GETDISPINFOA, (size_t)off(F_SIZE) + cap * 2);
    if (!nm)
        return "";
    uint8_t *it = nm + hdr_size(), *text = it + off(F_SIZE);
    fill_item(it, n);
    put32(it, (ask_text ? TVIF_TEXT : 0) | (ask_image ? (sel ? TVIF_SELECTEDIMAGE : TVIF_IMAGE) : 0) | TVIF_HANDLE |
                  TVIF_PARAM | TVIF_STATE);
    put_ptr(it + off(F_TEXT), (uintptr_t)text);
    put32(it + off(F_CCH), (int)cap);
    if (w->parent)
        w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
    if (ask_image)
        *image = get32(it + off(sel ? F_SEL : F_IMAGE));
    const char *out = n->text && n->text != CALLBACK_TEXT ? n->text : "";
    if (ask_text) {
        uintptr_t p = get_ptr(it + off(F_TEXT));
        buf[0] = 0;
        if (p && p != 0xffffffffu) {
            if (wide) {
                char *t = w32_narrow((const WCHAR *)p);
                if (t) {
                    str_copy(buf, t, cap);
                    w32_free(t);
                }
            } else {
                str_copy(buf, (const char *)p, cap);
            }
        }
        out = buf;
    }
    w32_free(nm);
    return out;
}

static bool expanded(struct tv_node *n)
{
    return (n->state & TVIS_EXPANDED) && n->child;
}

static bool has_children(struct tv_node *n)
{
    return n->child || n->children > 0 || n->children == -1;
}

static struct tv_node *next_visible(struct tv_node *n)
{
    if (expanded(n))
        return n->child;
    for (; n; n = n->parent)
        if (n->next)
            return n->next;
    return NULL;
}

static struct tv_node *last_visible_in(struct tv_node *n)
{
    while (expanded(n)) {
        n = n->child;
        while (n->next)
            n = n->next;
    }
    return n;
}

static struct tv_node *prev_visible(struct tv_node *n)
{
    return n->prev ? last_visible_in(n->prev) : n->parent;
}

static int depth(struct tv_node *n)
{
    int d = 0;
    for (n = n->parent; n; n = n->parent)
        d++;
    return d;
}

static bool shown(struct tv_node *n)
{
    for (n = n->parent; n; n = n->parent)
        if (!(n->state & TVIS_EXPANDED))
            return false;
    return true;
}

static int row_of(struct tv *s, struct tv_node *want)
{
    int r = 0;
    for (struct tv_node *n = s->first; n; n = next_visible(n), r++)
        if (n == want)
            return r;
    return -1;
}

static int rows(struct tv *s)
{
    int r = 0;
    for (struct tv_node *n = s->first; n; n = next_visible(n))
        r++;
    return r;
}

static struct tv_node *at_row(struct tv *s, int row)
{
    struct tv_node *n = s->first;
    for (; n && row > 0; row--)
        n = next_visible(n);
    return row < 0 ? NULL : n;
}

static int row_h(struct w32_window *w, struct tv *s)
{
    return s->item_h > 0 ? s->item_h : MAX(w32_ctl_font(w)->height + 4, 18);
}

static int page(struct w32_window *w, struct tv *s)
{
    return MAX(1, w->rect.h / row_h(w, s));
}

static void clamp_scroll(struct w32_window *w, struct tv *s)
{
    s->top = MAX(0, MIN(s->top, rows(s) - page(w, s)));
}

static void ensure_visible(struct w32_window *w, struct tv *s, struct tv_node *n)
{
    for (struct tv_node *p = n->parent; p; p = p->parent)
        p->state |= TVIS_EXPANDED;
    int r = row_of(s, n), pg = page(w, s);
    if (r < s->top)
        s->top = r;
    else if (r >= s->top + pg)
        s->top = r - pg + 1;
    clamp_scroll(w, s);
}

static int level_x(struct w32_window *w, struct tv *s, struct tv_node *n)
{
    return 2 + (depth(n) + ((w->style & TVS_LINESATROOT) ? 1 : 0)) * s->indent;
}

static int label_x(struct w32_window *w, struct tv *s, struct tv_node *n)
{
    int x = level_x(w, s, n);
    if (w->style & TVS_CHECKBOXES)
        x += 18;
    if (s->images[0])
        x += 20;
    return x;
}

static struct rect node_rect(struct w32_window *w, struct tv *s, struct tv_node *n, bool text_only)
{
    int y = (row_of(s, n) - s->top) * row_h(w, s);
    if (!text_only)
        return (struct rect){0, y, w->rect.w, row_h(w, s)};
    char buf[260];
    int x = label_x(w, s, n);
    return (struct rect){x, y, gfx_text_width(w32_ctl_font(w), item_text(w, s, n, buf, sizeof(buf), NULL)) + 8, row_h(w, s)};
}

static bool select(struct w32_window *w, struct tv *s, struct tv_node *n, int action)
{
    if (n == s->sel)
        return true;
    if (notify_tree(w, s, TVN_SELCHANGINGA, TVN_SELCHANGINGW, action, s->sel, n))
        return false;
    struct tv_node *old = s->sel;
    if (old)
        old->state &= ~(uint32_t)TVIS_SELECTED;
    s->sel = n;
    if (n) {
        n->state |= TVIS_SELECTED;
        ensure_visible(w, s, n);
    }
    w32_invalidate(w, NULL, true);
    notify_tree(w, s, TVN_SELCHANGEDA, TVN_SELCHANGEDW, action, old, n);
    return true;
}

static bool expand(struct w32_window *w, struct tv *s, struct tv_node *n, int how)
{
    if (!n)
        return false;
    bool open = n->state & TVIS_EXPANDED;
    int action = how & 3;
    if (action == 3)
        action = open ? 1 : 2;
    if ((action == 2) == open && !(how & 0x8000))
        return false;
    if (action == 2 && !(n->state & TVIS_EXPANDEDONCE) &&
        notify_tree(w, s, TVN_ITEMEXPANDINGA, TVN_ITEMEXPANDINGW, action, NULL, n))
        return false;
    if (action == 2)
        n->state |= TVIS_EXPANDED | TVIS_EXPANDEDONCE;
    else
        n->state &= ~(uint32_t)TVIS_EXPANDED;
    if (action == 1 && (how & 0x8000))
        n->state &= ~(uint32_t)TVIS_EXPANDEDONCE;
    if (action == 1 && s->sel && s->sel != n) {
        for (struct tv_node *p = s->sel->parent; p; p = p->parent)
            if (p == n) {
                select(w, s, n, 0);
                break;
            }
    }
    notify_tree(w, s, TVN_ITEMEXPANDEDA, TVN_ITEMEXPANDEDW, action, NULL, n);
    clamp_scroll(w, s);
    w32_invalidate(w, NULL, true);
    return true;
}

static void apply_item(struct tv_node *n, const uint8_t *it, bool wide)
{
    uint32_t mask = (uint32_t)get32(it);
    if (mask & TVIF_TEXT) {
        free_text(n);
        n->text = dup_text(get_ptr(it + off(F_TEXT)), wide);
    }
    if (mask & TVIF_IMAGE)
        n->image = get32(it + off(F_IMAGE));
    if (mask & TVIF_SELECTEDIMAGE)
        n->sel_image = get32(it + off(F_SEL));
    if (mask & TVIF_CHILDREN)
        n->children = get32(it + off(F_CHILDREN));
    if (mask & TVIF_PARAM) {
        n->param = 0;
        memcpy(&n->param, it + off(F_PARAM), (size_t)ptr_size());
    }
    if (mask & TVIF_STATE) {
        uint32_t m = (uint32_t)get32(it + off(F_SMASK)) & ~(uint32_t)TVIS_SELECTED;
        n->state = (n->state & ~m) | ((uint32_t)get32(it + off(F_STATE)) & m);
    }
}

static struct tv_node **head_of(struct tv *s, struct tv_node *parent)
{
    return parent ? &parent->child : &s->first;
}

static void link_after(struct tv *s, struct tv_node *parent, struct tv_node *after, struct tv_node *n)
{
    struct tv_node **head = head_of(s, parent);
    n->parent = parent;
    if (!after) {
        n->next = *head;
        if (*head)
            (*head)->prev = n;
        *head = n;
        return;
    }
    n->prev = after;
    n->next = after->next;
    if (after->next)
        after->next->prev = n;
    after->next = n;
}

static LRESULT insert(struct w32_window *w, struct tv *s, const uint8_t *p, bool wide)
{
    if (!p)
        return 0;
    uintptr_t hp = get_ptr(p), ha = get_ptr(p + ptr_size());
    struct tv_node *parent = (uint32_t)hp == TVI_ROOT ? NULL : node(hp);
    if (hp && (uint32_t)hp != TVI_ROOT && !parent)
        return 0;
    struct tv_node *n = w32_zalloc(sizeof(*n));
    if (!n)
        return 0;
    n->magic = NODE_MAGIC;
    apply_item(n, p + 2 * ptr_size(), wide);
    if (!(get32(p + 2 * ptr_size()) & TVIF_SELECTEDIMAGE))
        n->sel_image = n->image;
    struct tv_node *after = NULL, *k = *head_of(s, parent);
    if ((uint32_t)ha == TVI_FIRST) {
        after = NULL;
    } else if ((uint32_t)ha == TVI_SORT) {
        char a[260], b[260];
        const char *t = item_text(w, s, n, a, sizeof(a), NULL);
        for (; k && str_casecmp(item_text(w, s, k, b, sizeof(b), NULL), t) <= 0; k = k->next)
            after = k;
    } else if (node(ha) && node(ha)->parent == parent) {
        after = node(ha);
    } else {
        for (; k; k = k->next)
            after = k;
    }
    link_after(s, parent, after, n);
    s->count++;
    clamp_scroll(w, s);
    w32_invalidate(w, NULL, true);
    return (LRESULT)(uintptr_t)n;
}

static void delete_node(struct w32_window *w, struct tv *s, struct tv_node *n, bool tell)
{
    while (n->child)
        delete_node(w, s, n->child, tell);
    if (tell)
        notify_tree(w, s, TVN_DELETEITEMA, TVN_DELETEITEMW, 0, n, NULL);
    if (n->prev)
        n->prev->next = n->next;
    else
        *head_of(s, n->parent) = n->next;
    if (n->next)
        n->next->prev = n->prev;
    if (s->sel == n)
        s->sel = NULL;
    free_text(n);
    n->magic = 0;
    w32_free(n);
    s->count--;
}

static void get_item(struct w32_window *w, struct tv *s, struct tv_node *n, uint8_t *it, bool wide)
{
    uint32_t mask = (uint32_t)get32(it);
    if (mask & TVIF_STATE)
        put32(it + off(F_STATE), (int)(n->state & (uint32_t)get32(it + off(F_SMASK))));
    if (mask & TVIF_IMAGE)
        put32(it + off(F_IMAGE), n->image);
    if (mask & TVIF_SELECTEDIMAGE)
        put32(it + off(F_SEL), n->sel_image);
    if (mask & TVIF_CHILDREN)
        put32(it + off(F_CHILDREN), n->child ? 1 : n->children);
    if (mask & TVIF_PARAM)
        memcpy(it + off(F_PARAM), &n->param, (size_t)ptr_size());
    uintptr_t out = get_ptr(it + off(F_TEXT));
    int cch = get32(it + off(F_CCH));
    if ((mask & TVIF_TEXT) && out && cch > 0) {
        char buf[260];
        const char *t = n->text == CALLBACK_TEXT ? item_text(w, s, n, buf, sizeof(buf), NULL) : n->text ? n->text : "";
        if (wide) {
            size_t k = w32_cp_to_wide(t, (int)strlen(t), (WCHAR *)out, (size_t)cch - 1, 65001);
            ((WCHAR *)out)[k] = 0;
        } else {
            str_copy((char *)out, t, (size_t)cch);
        }
    }
}

static int call_compare(void *fn, uint64_t a, uint64_t b, uint64_t param)
{
#ifdef __x86_64__
    if (w32->win64) {
        uint64_t args[3] = {a, b, param};
        return (int)w64_call((uintptr_t)fn, 3, args);
    }
#endif
    return W32_CALL(int(WINAPI *)(uint32_t, uint32_t, uint32_t), fn, (uint32_t)a, (uint32_t)b, (uint32_t)param);
}

static int compare(struct w32_window *w, struct tv *s, struct tv_node *a, struct tv_node *b, void *fn, uint64_t param)
{
    if (fn)
        return call_compare(fn, a->param, b->param, param);
    char x[260], y[260];
    return str_casecmp(item_text(w, s, a, x, sizeof(x), NULL), item_text(w, s, b, y, sizeof(y), NULL));
}

static void sort_children(struct w32_window *w, struct tv *s, struct tv_node *parent, bool recurse, void *fn,
                          uint64_t param)
{
    struct tv_node **head = head_of(s, parent), *sorted = NULL;
    while (*head) {
        struct tv_node *n = *head;
        *head = n->next;
        struct tv_node **at = &sorted, *prev = NULL;
        while (*at && compare(w, s, *at, n, fn, param) <= 0) {
            prev = *at;
            at = &(*at)->next;
        }
        n->next = *at;
        n->prev = prev;
        if (*at)
            (*at)->prev = n;
        *at = n;
    }
    *head = sorted;
    if (recurse)
        for (struct tv_node *n = sorted; n; n = n->next)
            sort_children(w, s, n, true, fn, param);
    w32_invalidate(w, NULL, true);
}

static struct tv_node *next_item(struct w32_window *w, struct tv *s, int code, struct tv_node *n)
{
    switch (code) {
    case 0: return s->first;
    case 1: return n ? n->next : NULL;
    case 2: return n ? n->prev : NULL;
    case 3: return n ? n->parent : NULL;
    case 4: return n ? n->child : s->first;
    case 5: return at_row(s, s->top);
    case 6: return n && shown(n) ? next_visible(n) : NULL;
    case 7: return n && shown(n) ? prev_visible(n) : NULL;
    case 9: return s->sel;
    case 10: {
        struct tv_node *last = s->first;
        for (struct tv_node *k = s->first; k; k = next_visible(k))
            last = k;
        UNUSED(w);
        return last;
    }
    default: return NULL;
    }
}

static void draw_image(HANDLE list, int index, HANDLE dc, int x, int y)
{
    typedef BOOL WINAPI (*draw_fn)(HANDLE, int, HANDLE, int, int, UINT);
    draw_fn draw = list && index >= 0 ? (draw_fn)w32_builtin_lookup("comctl32.dll", "ImageList_Draw") : NULL;
    if (draw)
        draw(list, index, dc, x, y, 1);
}

static void draw_button(struct canvas *c, int cx, int cy, bool open, uint32_t color)
{
    for (int k = 0; k < 4; k++) {
        if (open)
            gfx_fill(c, (struct rect){cx - 4 + k, cy - 2 + k, 9 - 2 * k, 1}, color);
        else
            gfx_fill(c, (struct rect){cx - 2 + k, cy - 4 + k, 1, 9 - 2 * k}, color);
    }
}

static void paint(struct w32_window *w, struct tv *s)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    const struct ui_font *f = w32_ctl_font(w);
    int rh = row_h(w, s);
    bool active = w32_focus() == w;
    gfx_fill(p.c, p.r, style.field);
    char buf[260];
    int y = 0;
    for (struct tv_node *n = at_row(s, s->top); n && y < w->rect.h; n = next_visible(n), y += rh) {
        int x = level_x(w, s, n), ry = p.r.y + y;
        if ((w->style & TVS_HASBUTTONS) && has_children(n) && (depth(n) || (w->style & TVS_LINESATROOT)))
            draw_button(p.c, p.r.x + x - s->indent / 2, ry + rh / 2, n->state & TVIS_EXPANDED, style.secondary);
        if (w->style & TVS_CHECKBOXES) {
            struct rect box = {p.r.x + x, ry + (rh - 13) / 2, 13, 13};
            gfx_round(p.c, box, 3, style.control);
            gfx_round_frame(p.c, box, 3, style.control_border);
            if ((n->state >> 12 & 15) == 2)
                gfx_text_box(p.c, box, &font_bold, "v", style.text, ALIGN_CENTER);
            x += 18;
        }
        int image = -1;
        const char *t = item_text(w, s, n, buf, sizeof(buf), &image);
        if (s->images[0]) {
            draw_image(s->images[0], image, p.dc, x, y + (rh - 16) / 2);
            x += 20;
        }
        bool sel = n == s->sel;
        struct rect label = {p.r.x + x, ry, gfx_text_width(f, t) + 8, rh};
        if (sel)
            gfx_round(p.c, label, 4, active ? style.accent : style.selection_inactive);
        gfx_text_box(p.c, (struct rect){label.x + 4, ry, label.w - 4, rh}, (n->state & TVIS_BOLD) ? &font_bold : f, t,
                     sel && active ? style.accent_text : !w->enabled ? style.tertiary : style.text, ALIGN_LEFT);
    }
    int total = rows(s), pg = page(w, s);
    if (total > pg) {
        int knob = MAX(12, w->rect.h * pg / total), at = (w->rect.h - knob) * s->top / MAX(1, total - pg);
        gfx_round(p.c, (struct rect){p.r.x + p.r.w - 5, p.r.y + at, 3, knob}, 1, style.tertiary);
    }
    w32_ctl_end(w, &p);
}

/* TVHT_ flags for a point */
static struct tv_node *hit(struct w32_window *w, struct tv *s, int x, int y, uint32_t *flags)
{
    struct tv_node *n = y >= 0 && x >= 0 && x < w->rect.w ? at_row(s, s->top + y / row_h(w, s)) : NULL;
    *flags = 1;
    if (!n)
        return NULL;
    int lx = level_x(w, s, n), tx = label_x(w, s, n);
    char buf[260];
    if (x < lx)
        *flags = x >= lx - s->indent && has_children(n) ? 0x10 : 8;
    else if (x < lx + ((w->style & TVS_CHECKBOXES) ? 18 : 0))
        *flags = 0x40;
    else if (x < tx)
        *flags = 2;
    else if (x < tx + gfx_text_width(w32_ctl_font(w), item_text(w, s, n, buf, sizeof(buf), NULL)) + 8)
        *flags = 4;
    else
        *flags = 0x20;
    return n;
}

static LRESULT mouse(struct w32_window *w, struct tv *s, UINT msg, LPARAM lp)
{
    int x = (int16_t)lp, y = (int16_t)(lp >> 16);
    uint32_t flags;
    struct tv_node *n = hit(w, s, x, y, &flags);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
        w32_set_focus(w);
    if (msg == WM_RBUTTONDOWN) {
        notify(w, NM_RCLICK);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN && n) {
        if (flags == 0x10) {
            expand(w, s, n, 3);
            return 0;
        }
        if (flags == 0x40) {
            n->state = (n->state & ~0xf000u) | (((n->state >> 12 & 15) == 2 ? 1u : 2u) << 12);
            w32_invalidate(w, NULL, false);
        }
    }
    if (notify(w, msg == WM_LBUTTONDBLCLK ? NM_DBLCLK : NM_CLICK))
        return 0;
    if (n && msg == WM_LBUTTONDOWN)
        select(w, s, n, 1);
    if (n && msg == WM_LBUTTONDBLCLK && flags != 0x40)
        expand(w, s, n, 3);
    return 0;
}

static void key_down(struct w32_window *w, struct tv *s, WPARAM vk)
{
    uint8_t *nm = nm_new(w, TVN_KEYDOWN, 8);
    if (nm) {
        uint16_t key = (uint16_t)vk;
        memcpy(nm + hdr_size(), &key, 2);
        nm_send(w, nm);
    }
    struct tv_node *n = s->sel, *to = NULL;
    if (!n) {
        if (s->first && vk >= 0x21 && vk <= 0x28)
            select(w, s, s->first, 2);
        return;
    }
    switch (vk) {
    case 0x26: to = prev_visible(n); break;
    case 0x28: to = next_visible(n); break;
    case 0x24: to = s->first; break;
    case 0x23: to = next_item(w, s, 10, NULL); break;
    case 0x21:
    case 0x22:
        to = n;
        for (int k = page(w, s) - 1; k > 0; k--) {
            struct tv_node *m = vk == 0x21 ? prev_visible(to) : next_visible(to);
            if (!m)
                break;
            to = m;
        }
        break;
    case 0x25:
        if (expanded(n))
            expand(w, s, n, 1);
        else
            to = n->parent;
        break;
    case 0x27:
        if (has_children(n) && !(n->state & TVIS_EXPANDED))
            expand(w, s, n, 2);
        else
            to = n->child;
        break;
    case 0x6b: expand(w, s, n, 2); break;
    case 0x6d: expand(w, s, n, 1); break;
    case 0x0d: notify(w, NM_RETURN); break;
    case 0x20:
        if (w->style & TVS_CHECKBOXES) {
            n->state = (n->state & ~0xf000u) | (((n->state >> 12 & 15) == 2 ? 1u : 2u) << 12);
            w32_invalidate(w, NULL, false);
        }
        break;
    }
    if (to)
        select(w, s, to, 2);
}

LRESULT WINAPI w32_treeview_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct tv *s = tv_of(w);
    if (!s)
        return w32_def_window_proc(h, msg, wp, lp);
    uint8_t *ptr = (uint8_t *)(uintptr_t)(uint32_t)lp;
    switch (msg) {
    case TVM_INSERTITEMA:
    case TVM_INSERTITEMW: return insert(w, s, ptr, msg == TVM_INSERTITEMW);
    case TVM_DELETEITEM: {
        struct tv_node *n = node((uintptr_t)lp);
        if (!lp || (uint32_t)lp == TVI_ROOT) {
            while (s->first)
                delete_node(w, s, s->first, true);
            s->top = 0;
        } else if (n) {
            delete_node(w, s, n, true);
        } else {
            return FALSE;
        }
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return TRUE;
    }
    case TVM_EXPAND: return expand(w, s, node((uintptr_t)lp), (int)wp);
    case TVM_GETITEMRECT: {
        struct tv_node *n = ptr ? node(get_ptr(ptr)) : NULL;
        if (!n || !shown(n))
            return FALSE;
        struct rect r = node_rect(w, s, n, wp != 0);
        *(RECT *)ptr = (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
        return TRUE;
    }
    case TVM_GETCOUNT: return s->count;
    case TVM_GETINDENT: return s->indent;
    case TVM_SETINDENT: s->indent = MAX(8, (int)wp); w32_invalidate(w, NULL, true); return 0;
    case TVM_GETIMAGELIST: return wp <= 2 ? (LRESULT)(uintptr_t)s->images[wp] : 0;
    case TVM_SETIMAGELIST: {
        if (wp > 2)
            return 0;
        HANDLE old = s->images[wp];
        s->images[wp] = (HANDLE)(uintptr_t)(uint32_t)lp;
        w32_invalidate(w, NULL, true);
        return (LRESULT)(uintptr_t)old;
    }
    case TVM_GETNEXTITEM: return (LRESULT)(uintptr_t)next_item(w, s, (int)(wp & 0xff), node((uintptr_t)lp));
    case TVM_SELECTITEM: {
        struct tv_node *n = node((uintptr_t)lp);
        if ((wp & 0xff) == 5) {
            if (n) {
                int r = row_of(s, n);
                s->top = r < 0 ? s->top : r;
                clamp_scroll(w, s);
                w32_invalidate(w, NULL, true);
            }
            return n != NULL;
        }
        if ((wp & 0xff) == 8)
            return TRUE;
        return lp && !n ? FALSE : select(w, s, n, 0);
    }
    case TVM_GETITEMA:
    case TVM_GETITEMW: {
        struct tv_node *n = ptr ? node(get_ptr(ptr + off(F_ITEM))) : NULL;
        if (!n)
            return FALSE;
        get_item(w, s, n, ptr, msg == TVM_GETITEMW);
        return TRUE;
    }
    case TVM_SETITEMA:
    case TVM_SETITEMW: {
        struct tv_node *n = ptr ? node(get_ptr(ptr + off(F_ITEM))) : NULL;
        if (!n)
            return FALSE;
        apply_item(n, ptr, msg == TVM_SETITEMW);
        w32_invalidate(w, NULL, true);
        return TRUE;
    }
    case TVM_GETITEMSTATE: {
        struct tv_node *n = node((uintptr_t)wp);
        return n ? (LRESULT)(n->state & (uint32_t)lp) : 0;
    }
    case TVM_GETVISIBLECOUNT: return page(w, s);
    case TVM_HITTEST: {
        if (!ptr)
            return 0;
        uint32_t flags;
        struct tv_node *n = hit(w, s, get32(ptr), get32(ptr + 4), &flags);
        memcpy(ptr + 8, &flags, 4);
        put_ptr(ptr + (w32->win64 ? 16 : 12), (uintptr_t)n);
        return (LRESULT)(uintptr_t)n;
    }
    case TVM_SORTCHILDREN: {
        struct tv_node *n = node((uintptr_t)lp);
        sort_children(w, s, n, wp != 0, NULL, 0);
        return TRUE;
    }
    case TVM_SORTCHILDRENCB: {
        if (!ptr)
            return FALSE;
        uint64_t param = 0;
        memcpy(&param, ptr + 2 * ptr_size(), (size_t)ptr_size());
        sort_children(w, s, node(get_ptr(ptr)), wp != 0, (void *)get_ptr(ptr + ptr_size()), param);
        return TRUE;
    }
    case TVM_ENSUREVISIBLE: {
        struct tv_node *n = node((uintptr_t)lp);
        if (n) {
            ensure_visible(w, s, n);
            w32_invalidate(w, NULL, true);
        }
        return n != NULL;
    }
    case TVM_SETITEMHEIGHT: {
        int old = row_h(w, s);
        s->item_h = (int)wp == -1 ? 0 : (int)wp;
        w32_invalidate(w, NULL, true);
        return old;
    }
    case TVM_GETITEMHEIGHT: return row_h(w, s);
    case TVM_SETBKCOLOR: {
        COLORREF old = s->bk;
        s->bk = (COLORREF)lp;
        return (LRESULT)old;
    }
    case TVM_SETTEXTCOLOR: {
        COLORREF old = s->text;
        s->text = (COLORREF)lp;
        return (LRESULT)old;
    }
    case TVM_GETBKCOLOR: return (LRESULT)s->bk;
    case TVM_GETTEXTCOLOR: return (LRESULT)s->text;
    case TVM_SETEXTENDEDSTYLE: {
        uint32_t old = s->ex;
        s->ex = wp ? (s->ex & ~(uint32_t)wp) | ((uint32_t)lp & (uint32_t)wp) : (uint32_t)lp;
        return (LRESULT)old;
    }
    case TVM_GETEXTENDEDSTYLE: return (LRESULT)s->ex;
    case 0x110e: /* TVM_EDITLABELA */
    case 0x1141: /* TVM_EDITLABELW */
    case 0x110f: /* TVM_GETEDITCONTROL */
    case 0x1112: /* TVM_CREATEDRAGIMAGE */
    case 0x1119: /* TVM_GETTOOLTIPS */ return 0;
    case 0x1116: /* TVM_ENDEDITLABELNOW */
    case 0x111a: /* TVM_SETINSERTMARK */ return TRUE;
    case 0x2005: /* CCM_SETUNICODEFORMAT */
    case 0x2006: return TRUE;
    case WM_PAINT: paint(w, s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: clamp_scroll(w, s); return 0;
    case WM_SETFONT:
        w->font = (HANDLE)wp;
        if (lp)
            w32_invalidate(w, NULL, true);
        return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_GETDLGCODE: return 1;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: return mouse(w, s, msg, lp);
    case WM_MOUSEWHEEL:
        s->top -= (int16_t)(wp >> 16) / 40;
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return 0;
    case WM_VSCROLL: {
        int code = (int)(wp & 0xffff), pg = page(w, s);
        s->top = code == 0 ? s->top - 1 : code == 1 ? s->top + 1 : code == 2 ? s->top - pg : code == 3 ? s->top + pg
                 : code == 5 || code == 4 ? (int)(wp >> 16) : code == 6 ? 0 : code == 7 ? 1 << 24 : s->top;
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case WM_KEYDOWN: key_down(w, s, wp); return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        notify(w, msg == WM_SETFOCUS ? NM_SETFOCUS : NM_KILLFOCUS);
        w32_invalidate(w, NULL, false);
        return 0;
    case WM_ENABLE: w32_invalidate(w, NULL, true); return 0;
    case WM_NCDESTROY_:
        while (s->first)
            delete_node(w, s, s->first, false);
        w32_free(s);
        w->list = NULL;
        break;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}
#endif
