#include <synth/config.h>
#if CONFIG_GUI
#include <synth/gui.h>
#include <synth/string.h>

#include "w32gui.h"
#include "win64.h"

/* SysListView32: report view with a header row (other views draw as a one-column list), owner data and text
   callbacks through LVN_GETDISPINFO; LVITEM/LVCOLUMN/NMHDR come in the caller's own 32- or 64-bit layout */
enum {
    LVM_SETBKCOLOR = 0x1001,
    LVM_GETIMAGELIST = 0x1002,
    LVM_SETIMAGELIST = 0x1003,
    LVM_GETITEMCOUNT = 0x1004,
    LVM_GETITEMA = 0x1005,
    LVM_SETITEMA = 0x1006,
    LVM_INSERTITEMA = 0x1007,
    LVM_DELETEITEM = 0x1008,
    LVM_DELETEALLITEMS = 0x1009,
    LVM_GETCALLBACKMASK = 0x100a,
    LVM_SETCALLBACKMASK = 0x100b,
    LVM_GETNEXTITEM = 0x100c,
    LVM_FINDITEMA = 0x100d,
    LVM_GETITEMRECT = 0x100e,
    LVM_GETSTRINGWIDTHA = 0x1011,
    LVM_HITTEST = 0x1012,
    LVM_ENSUREVISIBLE = 0x1013,
    LVM_SCROLL = 0x1014,
    LVM_GETCOLUMNA = 0x1019,
    LVM_SETCOLUMNA = 0x101a,
    LVM_INSERTCOLUMNA = 0x101b,
    LVM_DELETECOLUMN = 0x101c,
    LVM_GETCOLUMNWIDTH = 0x101d,
    LVM_SETCOLUMNWIDTH = 0x101e,
    LVM_GETVIEWRECT = 0x1022,
    LVM_GETTEXTCOLOR = 0x1023,
    LVM_SETTEXTCOLOR = 0x1024,
    LVM_GETTEXTBKCOLOR = 0x1025,
    LVM_SETTEXTBKCOLOR = 0x1026,
    LVM_GETTOPINDEX = 0x1027,
    LVM_GETCOUNTPERPAGE = 0x1028,
    LVM_GETORIGIN = 0x1029,
    LVM_SETITEMSTATE = 0x102b,
    LVM_GETITEMSTATE = 0x102c,
    LVM_GETITEMTEXTA = 0x102d,
    LVM_SETITEMTEXTA = 0x102e,
    LVM_SETITEMCOUNT = 0x102f,
    LVM_SORTITEMS = 0x1030,
    LVM_GETSELECTEDCOUNT = 0x1032,
    LVM_SETEXTENDEDSTYLE = 0x1036,
    LVM_GETEXTENDEDSTYLE = 0x1037,
    LVM_GETSUBITEMRECT = 0x1038,
    LVM_SUBITEMHITTEST = 0x1039,
    LVM_SETCOLUMNORDERARRAY = 0x103a,
    LVM_GETCOLUMNORDERARRAY = 0x103b,
    LVM_GETSELECTIONMARK = 0x1042,
    LVM_SETSELECTIONMARK = 0x1043,
    LVM_GETITEMW = 0x104b,
    LVM_SETITEMW = 0x104c,
    LVM_INSERTITEMW = 0x104d,
    LVM_SORTITEMSEX = 0x1051,
    LVM_FINDITEMW = 0x1053,
    LVM_GETSTRINGWIDTHW = 0x1057,
    LVM_GETCOLUMNW = 0x105f,
    LVM_SETCOLUMNW = 0x1060,
    LVM_INSERTCOLUMNW = 0x1061,
    LVM_GETITEMTEXTW = 0x1073,
    LVM_SETITEMTEXTW = 0x1074,
    LVM_SETVIEW = 0x108e,
    LVM_GETVIEW = 0x108f,
    LVM_ISITEMVISIBLE = 0x10b6,
    LVIF_TEXT = 1,
    LVIF_IMAGE = 2,
    LVIF_PARAM = 4,
    LVIF_STATE = 8,
    LVIF_INDENT = 0x10,
    LVIS_FOCUSED = 1,
    LVIS_SELECTED = 2,
    LVCF_FMT = 1,
    LVCF_WIDTH = 2,
    LVCF_TEXT = 4,
    LVCF_SUBITEM = 8,
    LVS_REPORT = 1,
    LVS_SINGLESEL = 4,
    LVS_NOCOLUMNHEADER = 0x4000,
    LVS_OWNERDATA = 0x1000,
    LVS_EX_GRIDLINES = 1,
    LVS_EX_CHECKBOXES = 4,
    LVN_ITEMCHANGING = -100,
    LVN_ITEMCHANGED = -101,
    LVN_INSERTITEM = -102,
    LVN_DELETEITEM = -103,
    LVN_DELETEALLITEMS = -104,
    LVN_COLUMNCLICK = -108,
    LVN_ITEMACTIVATE = -114,
    LVN_GETDISPINFOA = -150,
    LVN_KEYDOWN = -155,
    LVN_GETDISPINFOW = -177,
    NM_CLICK = -2,
    NM_DBLCLK = -3,
    NM_RETURN = -4,
    NM_RCLICK = -5,
    NM_SETFOCUS = -7,
    NM_KILLFOCUS = -8,
    WM_NCDESTROY_ = 0x82,
    WM_LBUTTONUP_ = 0x202,
    WM_HSCROLL_ = 0x114,
    MK_SHIFT_ = 4,
    MK_CONTROL_ = 8,
};

#define CALLBACK_TEXT ((char *)1)

struct lv_col {
    char *text;
    int width, fmt, image;
};

struct lv_item {
    char **sub;
    int image, indent;
    uint64_t param;
    uint32_t state;
};

struct lv {
    struct lv_item *items;
    int count, cap;
    struct lv_col *cols;
    int ncols;
    int top, xoff, mark, view, resize, resize_x;
    HANDLE images[3];
    uint32_t ex, callback_mask;
    COLORREF bk, text, text_bk;
    bool unicode;
    int8_t notify_wide;
};

static struct lv *lv_of(struct w32_window *w)
{
    if (!w->list) {
        struct lv *s = w32_zalloc(sizeof(*s));
        if (!s)
            return NULL;
        s->mark = s->resize = -1;
        s->view = w->style & 3;
        s->bk = s->text_bk = 0xffffffffu;
        s->unicode = w->unicode;
        w->list = s;
    }
    return w->list;
}

static bool report(struct lv *s)
{
    return s->view == LVS_REPORT;
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

/* LVITEM field offsets: text, cch, image, param, indent */
static int off_text(void)
{
    return w32->win64 ? 24 : 20;
}

static int off_cch(void)
{
    return w32->win64 ? 32 : 24;
}

static int off_image(void)
{
    return w32->win64 ? 36 : 28;
}

static int off_param(void)
{
    return w32->win64 ? 40 : 32;
}

static int off_indent(void)
{
    return w32->win64 ? 48 : 36;
}

static int hdr_size(void)
{
    return w32->win64 ? 24 : 12;
}

static uint8_t *nm_new(struct w32_window *w, int code, size_t body)
{
    uint8_t *nm = w32_zalloc((size_t)hdr_size() + body + 16);
    if (!nm)
        return NULL;
    put_ptr(nm, (uintptr_t)w);
    put_ptr(nm + ptr_size(), (uint64_t)w->id);
    memcpy(nm + 2 * ptr_size(), &code, 4);
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

/* NMLISTVIEW / NMITEMACTIVATE */
static LRESULT notify_item(struct w32_window *w, struct lv *s, int code, int item, int sub, uint32_t now, uint32_t was,
                           uint32_t changed, int x, int y)
{
    uint8_t *nm = nm_new(w, code, 56);
    if (!nm)
        return 0;
    uint8_t *b = nm + hdr_size();
    int v[8] = {item, sub, (int)now, (int)was, (int)changed, x, y, 0};
    memcpy(b, v, 28);
    if (item >= 0 && item < s->count)
        memcpy(b + (w32->win64 ? 32 : 28), &s->items[item].param, (size_t)ptr_size());
    return nm_send(w, nm);
}

static void free_item(struct lv *s, struct lv_item *it)
{
    if (it->sub)
        for (int k = 0; k < MAX(s->ncols, 1); k++)
            if (it->sub[k] != CALLBACK_TEXT)
                w32_free(it->sub[k]);
    w32_free(it->sub);
    it->sub = NULL;
}

static bool grow_subs(struct lv *s, struct lv_item *it, int ncols)
{
    char **sub = w32_zalloc(sizeof(char *) * (size_t)MAX(ncols, 1));
    if (!sub)
        return false;
    if (it->sub)
        memcpy(sub, it->sub, sizeof(char *) * (size_t)MAX(s->ncols, 1));
    w32_free(it->sub);
    it->sub = sub;
    return true;
}

static char *dup_text(uintptr_t text, bool wide)
{
    if (text == (uintptr_t)-1 || text == 0xffffffffu)
        return CALLBACK_TEXT;
    if (!text)
        return w32_strdup("");
    return wide ? w32_narrow((const WCHAR *)text) : w32_strdup((const char *)text);
}

static void set_sub(struct lv *s, struct lv_item *it, int sub, uintptr_t text, bool wide)
{
    if (sub < 0 || sub >= MAX(s->ncols, 1) || (!it->sub && !grow_subs(s, it, s->ncols)))
        return;
    if (it->sub[sub] != CALLBACK_TEXT)
        w32_free(it->sub[sub]);
    it->sub[sub] = dup_text(text, wide);
}

static bool owner_data(struct w32_window *w)
{
    return w->style & LVS_OWNERDATA;
}

/* the text of one cell, asking the parent when it is a callback */
static const char *cell_text(struct w32_window *w, struct lv *s, int i, int sub, char *buf, size_t cap, int *image)
{
    struct lv_item *it = &s->items[i];
    const char *t = it->sub && sub < MAX(s->ncols, 1) ? it->sub[sub] : NULL;
    bool ask_text = owner_data(w) || t == CALLBACK_TEXT || (sub > 0 && !t), ask_image = image && (owner_data(w) || it->image == -1);
    if (image)
        *image = it->image;
    if (!ask_text && !ask_image)
        return t ? t : "";
    if (!s->notify_wide && w->parent) {
        LRESULT f = w32_send(w->parent, 0x55, (WPARAM)(uintptr_t)w, 3);
        s->notify_wide = f == 2 || (f != 1 && w->parent->unicode) ? 1 : -1;
    }
    bool wide = s->notify_wide > 0;
    uint8_t *nm = nm_new(w, wide ? LVN_GETDISPINFOW : LVN_GETDISPINFOA, 64 + cap * 2);
    if (!nm)
        return "";
    uint8_t *item = nm + hdr_size(), *text = item + 64;
    uint32_t mask = (ask_text ? LVIF_TEXT : 0) | (ask_image ? LVIF_IMAGE : 0);
    memcpy(item, &mask, 4);
    memcpy(item + 4, &i, 4);
    memcpy(item + 8, &sub, 4);
    put_ptr(item + off_text(), (uintptr_t)text);
    int cch = (int)cap;
    memcpy(item + off_cch(), &cch, 4);
    memcpy(item + off_param(), &it->param, (size_t)ptr_size());
    if (w->parent)
        w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
    if (ask_image)
        *image = get32(item + off_image());
    const char *out = t && t != CALLBACK_TEXT ? t : "";
    if (ask_text) {
        uintptr_t p = get_ptr(item + off_text());
        buf[0] = 0;
        if (p && p != 0xffffffffu) {
            if (wide) {
                char *n = w32_narrow((const WCHAR *)p);
                if (n) {
                    str_copy(buf, n, cap);
                    w32_free(n);
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

static int row_h(struct w32_window *w)
{
    return MAX(w32_ctl_font(w)->height + 6, 20);
}

static int head_h(struct w32_window *w, struct lv *s)
{
    return report(s) && !(w->style & LVS_NOCOLUMNHEADER) ? w32_ctl_font(w)->height + 8 : 0;
}

static int page_rows(struct w32_window *w, struct lv *s)
{
    return MAX(1, (w->rect.h - head_h(w, s)) / row_h(w));
}

static int col_x(struct lv *s, int c)
{
    int x = -s->xoff;
    for (int k = 0; k < c && k < s->ncols; k++)
        x += s->cols[k].width;
    return x;
}

static int total_w(struct w32_window *w, struct lv *s)
{
    return report(s) && s->ncols ? col_x(s, s->ncols) + s->xoff : w->rect.w;
}

static void clamp_scroll(struct w32_window *w, struct lv *s)
{
    s->top = MAX(0, MIN(s->top, s->count - page_rows(w, s)));
    s->xoff = MAX(0, MIN(s->xoff, total_w(w, s) - w->rect.w));
}

static void ensure_visible(struct w32_window *w, struct lv *s, int i)
{
    if (i < 0 || i >= s->count)
        return;
    int rows = page_rows(w, s);
    if (i < s->top)
        s->top = i;
    else if (i >= s->top + rows)
        s->top = i - rows + 1;
    clamp_scroll(w, s);
}

static struct rect item_rect(struct w32_window *w, struct lv *s, int i, int sub)
{
    int y = head_h(w, s) + (i - s->top) * row_h(w);
    if (!report(s) || !s->ncols)
        return (struct rect){0, y, w->rect.w, row_h(w)};
    if (sub < 0)
        return (struct rect){-s->xoff, y, total_w(w, s), row_h(w)};
    sub = MIN(sub, s->ncols - 1);
    return (struct rect){col_x(s, sub), y, s->cols[sub].width, row_h(w)};
}

static int hit_item(struct w32_window *w, struct lv *s, int x, int y, int *sub)
{
    int hh = head_h(w, s);
    if (sub)
        *sub = 0;
    if (y < hh || x < 0 || x >= w->rect.w)
        return -1;
    int i = s->top + (y - hh) / row_h(w);
    if (i >= s->count)
        return -1;
    if (sub && report(s))
        for (int c = 0; c < s->ncols; c++)
            if (x >= col_x(s, c) && x < col_x(s, c + 1))
                *sub = c;
    return i;
}

static void set_state(struct w32_window *w, struct lv *s, int i, uint32_t state, uint32_t mask)
{
    if (i < 0 || i >= s->count)
        return;
    struct lv_item *it = &s->items[i];
    uint32_t now = (it->state & ~mask) | (state & mask);
    if (now == it->state)
        return;
    uint32_t was = it->state;
    if (notify_item(w, s, LVN_ITEMCHANGING, i, 0, now, was, LVIF_STATE, 0, 0))
        return;
    if ((now & LVIS_FOCUSED) && !(was & LVIS_FOCUSED))
        for (int k = 0; k < s->count; k++)
            if (k != i)
                s->items[k].state &= ~(uint32_t)LVIS_FOCUSED;
    it->state = now;
    w32_invalidate(w, NULL, false);
    notify_item(w, s, LVN_ITEMCHANGED, i, 0, now, was, LVIF_STATE, 0, 0);
}

static void select_only(struct w32_window *w, struct lv *s, int i)
{
    for (int k = 0; k < s->count; k++)
        if (k != i && (s->items[k].state & LVIS_SELECTED))
            set_state(w, s, k, 0, LVIS_SELECTED);
    set_state(w, s, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static int focused(struct lv *s)
{
    for (int k = 0; k < s->count; k++)
        if (s->items[k].state & LVIS_FOCUSED)
            return k;
    return -1;
}

static void click_select(struct w32_window *w, struct lv *s, int i, WPARAM keys)
{
    bool single = w->style & LVS_SINGLESEL;
    if (!single && (keys & MK_SHIFT_) && s->mark >= 0) {
        int a = MIN(s->mark, i), b = MAX(s->mark, i);
        for (int k = 0; k < s->count; k++)
            set_state(w, s, k, k >= a && k <= b ? LVIS_SELECTED : 0, LVIS_SELECTED);
        set_state(w, s, i, LVIS_FOCUSED, LVIS_FOCUSED);
    } else if (!single && (keys & MK_CONTROL_)) {
        set_state(w, s, i, (s->items[i].state ^ LVIS_SELECTED) | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        s->mark = i;
    } else {
        select_only(w, s, i);
        s->mark = i;
    }
    ensure_visible(w, s, i);
}

static bool resize_items(struct lv *s, int count)
{
    if (count > s->cap) {
        int cap = MAX(count, s->cap ? s->cap * 2 : 16);
        struct lv_item *t = w32_zalloc(sizeof(*t) * (size_t)cap);
        if (!t)
            return false;
        if (s->items)
            memcpy(t, s->items, sizeof(*t) * (size_t)s->count);
        w32_free(s->items);
        s->items = t;
        s->cap = cap;
    }
    for (int i = s->count; i < count; i++)
        s->items[i] = (struct lv_item){NULL, -1, 0, 0, 0};
    return true;
}

static void apply_item(struct lv *s, struct lv_item *it, const uint8_t *p, bool wide)
{
    uint32_t mask;
    memcpy(&mask, p, 4);
    int sub = get32(p + 8);
    if (mask & LVIF_TEXT)
        set_sub(s, it, sub, get_ptr(p + off_text()), wide);
    if (sub)
        return;
    if (mask & LVIF_IMAGE)
        it->image = get32(p + off_image());
    if (mask & LVIF_PARAM)
        memcpy(&it->param, p + off_param(), (size_t)ptr_size());
    if (mask & LVIF_INDENT)
        it->indent = get32(p + off_indent());
    if (mask & LVIF_STATE) {
        uint32_t st = (uint32_t)get32(p + 12), sm = (uint32_t)get32(p + 16);
        it->state = (it->state & ~sm) | (st & sm);
    }
}

static LRESULT insert_item(struct w32_window *w, struct lv *s, const uint8_t *p, bool wide)
{
    if (!p || get32(p + 8) != 0 || !resize_items(s, s->count + 1))
        return -1;
    int at = get32(p + 4);
    at = at < 0 || at > s->count ? s->count : at;
    memmove(s->items + at + 1, s->items + at, sizeof(*s->items) * (size_t)(s->count - at));
    s->items[at] = (struct lv_item){NULL, -1, 0, 0, 0};
    s->count++;
    apply_item(s, &s->items[at], p, wide);
    if (s->mark >= at)
        s->mark++;
    w32_invalidate(w, NULL, true);
    notify_item(w, s, LVN_INSERTITEM, at, 0, 0, 0, 0, 0, 0);
    return at;
}

static bool delete_item(struct w32_window *w, struct lv *s, int i)
{
    if (i < 0 || i >= s->count)
        return false;
    notify_item(w, s, LVN_DELETEITEM, i, 0, 0, 0, 0, 0, 0);
    free_item(s, &s->items[i]);
    memmove(s->items + i, s->items + i + 1, sizeof(*s->items) * (size_t)(s->count - i - 1));
    s->count--;
    if (s->mark >= s->count)
        s->mark = s->count - 1;
    clamp_scroll(w, s);
    w32_invalidate(w, NULL, true);
    return true;
}

static void delete_all(struct w32_window *w, struct lv *s, bool tell)
{
    if (tell && s->count)
        notify_item(w, s, LVN_DELETEALLITEMS, -1, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < s->count; i++)
        free_item(s, &s->items[i]);
    s->count = 0;
    s->top = 0;
    s->mark = -1;
    w32_invalidate(w, NULL, true);
}

static void get_item(struct w32_window *w, struct lv *s, int i, uint8_t *p, bool wide)
{
    uint32_t mask;
    memcpy(&mask, p, 4);
    int sub = get32(p + 8), image, cch = get32(p + off_cch());
    char buf[260];
    const char *t = cell_text(w, s, i, sub, buf, sizeof(buf), &image);
    struct lv_item *it = &s->items[i];
    if ((mask & LVIF_TEXT) && cch > 0) {
        uintptr_t out = get_ptr(p + off_text());
        if (out && out != 0xffffffffu) {
            if (wide) {
                size_t n = w32_cp_to_wide(t, (int)strlen(t), (WCHAR *)out, (size_t)cch - 1, 65001);
                ((WCHAR *)out)[n] = 0;
            } else {
                str_copy((char *)out, t, (size_t)cch);
            }
        }
    }
    if (mask & LVIF_IMAGE)
        memcpy(p + off_image(), &image, 4);
    if (mask & LVIF_PARAM)
        memcpy(p + off_param(), &it->param, (size_t)ptr_size());
    if (mask & LVIF_INDENT)
        memcpy(p + off_indent(), &it->indent, 4);
    if (mask & LVIF_STATE) {
        uint32_t st = it->state & (uint32_t)get32(p + 16);
        memcpy(p + 12, &st, 4);
    }
}

/* LVCOLUMN: mask, fmt, cx, text, cch, subitem */
static void apply_col(struct lv_col *c, const uint8_t *p, bool wide)
{
    uint32_t mask;
    memcpy(&mask, p, 4);
    if (mask & LVCF_FMT)
        c->fmt = get32(p + 4);
    if (mask & LVCF_WIDTH)
        c->width = MAX(0, get32(p + 8));
    if (mask & LVCF_TEXT) {
        char *t = dup_text(get_ptr(p + (w32->win64 ? 16 : 12)), wide);
        if (t != CALLBACK_TEXT) {
            w32_free(c->text);
            c->text = t;
        }
    }
}

static LRESULT insert_col(struct w32_window *w, struct lv *s, int at, const uint8_t *p, bool wide)
{
    if (!p)
        return -1;
    struct lv_col *cols = w32_zalloc(sizeof(*cols) * (size_t)(s->ncols + 1));
    if (!cols)
        return -1;
    for (int i = 0; i < s->count; i++)
        if (s->items[i].sub && !grow_subs(s, &s->items[i], s->ncols + 1)) {
            w32_free(cols);
            return -1;
        }
    at = at < 0 || at > s->ncols ? s->ncols : at;
    if (s->cols)
        memcpy(cols, s->cols, sizeof(*cols) * (size_t)s->ncols);
    memmove(cols + at + 1, cols + at, sizeof(*cols) * (size_t)(s->ncols - at));
    cols[at] = (struct lv_col){NULL, 80, 0, -1};
    apply_col(&cols[at], p, wide);
    w32_free(s->cols);
    s->cols = cols;
    if (s->ncols)
        for (int i = 0; i < s->count; i++)
            if (s->items[i].sub && at < s->ncols) {
                char **sub = s->items[i].sub;
                memmove(sub + at + 1, sub + at, sizeof(char *) * (size_t)(s->ncols - at));
                sub[at] = NULL;
            }
    s->ncols++;
    w32_invalidate(w, NULL, true);
    return at;
}

static bool delete_col(struct w32_window *w, struct lv *s, int c)
{
    if (c < 0 || c >= s->ncols)
        return false;
    for (int i = 0; i < s->count; i++) {
        char **sub = s->items[i].sub;
        if (!sub)
            continue;
        if (sub[c] != CALLBACK_TEXT)
            w32_free(sub[c]);
        memmove(sub + c, sub + c + 1, sizeof(char *) * (size_t)(s->ncols - c - 1));
        sub[s->ncols - 1] = NULL;
    }
    w32_free(s->cols[c].text);
    memmove(s->cols + c, s->cols + c + 1, sizeof(*s->cols) * (size_t)(s->ncols - c - 1));
    s->ncols--;
    w32_invalidate(w, NULL, true);
    return true;
}

static int auto_width(struct w32_window *w, struct lv *s, int c, bool header)
{
    const struct ui_font *f = w32_ctl_font(w);
    int best = header && c < s->ncols && s->cols[c].text ? gfx_text_width(f, s->cols[c].text) + 16 : 20;
    char buf[260];
    for (int i = 0; i < s->count && i < 2000; i++)
        best = MAX(best, gfx_text_width(f, cell_text(w, s, i, c, buf, sizeof(buf), NULL)) + (c ? 12 : 32));
    return best;
}

static void draw_image(HANDLE list, int index, HANDLE dc, int x, int y)
{
    typedef BOOL WINAPI (*draw_fn)(HANDLE, int, HANDLE, int, int, UINT);
    draw_fn draw = list && index >= 0 ? (draw_fn)w32_builtin_lookup("comctl32.dll", "ImageList_Draw") : NULL;
    if (draw)
        draw(list, index, dc, x, y, 1);
}

static void paint(struct w32_window *w, struct lv *s)
{
    struct w32_paint p;
    if (!w32_ctl_begin(w, &p, 0))
        return;
    const struct ui_font *f = w32_ctl_font(w);
    int hh = head_h(w, s), rh = row_h(w);
    bool active = w32_focus() == w, checks = s->ex & LVS_EX_CHECKBOXES;
    gfx_fill(p.c, p.r, style.field);
    struct rect clip = p.c->clip;
    char buf[260];
    int cols = report(s) && s->ncols ? s->ncols : 1;
    for (int i = s->top; i < s->count; i++) {
        struct rect row = item_rect(w, s, i, -1);
        if (row.y >= w->rect.h)
            break;
        row = (struct rect){p.r.x + 2, p.r.y + row.y, MAX(row.w, w->rect.w) - 4, rh};
        bool sel = s->items[i].state & LVIS_SELECTED;
        if (sel)
            gfx_round(p.c, row, 4, active ? style.accent : style.selection_inactive);
        uint32_t color = sel && active ? style.accent_text : !w->enabled ? style.tertiary : style.text;
        for (int c = 0; c < cols; c++) {
            struct rect cell = report(s) && s->ncols ? item_rect(w, s, i, c) : item_rect(w, s, i, 0);
            cell.x += p.r.x;
            cell.y += p.r.y;
            p.c->clip = rect_intersect(clip, (struct rect){cell.x, MAX(cell.y, p.r.y + hh), cell.w, cell.h});
            int image = -1, tx = cell.x + 6;
            const char *t = cell_text(w, s, i, c, buf, sizeof(buf), c ? NULL : &image);
            if (!c) {
                tx += s->items[i].indent * 16;
                if (checks) {
                    struct rect box = {tx, cell.y + (rh - 13) / 2, 13, 13};
                    gfx_round(p.c, box, 3, style.control);
                    gfx_round_frame(p.c, box, 3, style.control_border);
                    if ((s->items[i].state >> 12 & 15) == 2)
                        gfx_text_box(p.c, box, &font_bold, "v", style.text, ALIGN_CENTER);
                    tx += 18;
                }
                HANDLE il = s->images[s->view == 0 ? 0 : 1];
                if (il && image >= 0) {
                    draw_image(il, image, p.dc, tx - p.r.x, cell.y - p.r.y + (rh - 16) / 2);
                    tx += 20;
                }
            }
            int fmt = report(s) && c < s->ncols ? s->cols[c].fmt & 3 : 0;
            gfx_text_box(p.c, (struct rect){tx, cell.y, cell.x + cell.w - tx - 6, rh}, f, t, color,
                         fmt == 1 ? ALIGN_RIGHT : fmt == 2 ? ALIGN_CENTER : ALIGN_LEFT);
        }
        p.c->clip = clip;
        if (s->ex & LVS_EX_GRIDLINES)
            gfx_fill(p.c, (struct rect){p.r.x, row.y + rh - 1, p.r.w, 1}, style.separator);
    }
    p.c->clip = clip;
    if (hh) {
        struct rect head = {p.r.x, p.r.y, p.r.w, hh};
        gfx_fill(p.c, head, style.window);
        gfx_fill(p.c, (struct rect){head.x, head.y + hh - 1, head.w, 1}, style.separator);
        for (int c = 0; c < s->ncols; c++) {
            struct rect cell = {p.r.x + col_x(s, c), p.r.y, s->cols[c].width, hh};
            p.c->clip = rect_intersect(clip, cell);
            int fmt = s->cols[c].fmt & 3;
            gfx_text_box(p.c, (struct rect){cell.x + 6, cell.y, cell.w - 12, hh}, f, s->cols[c].text ? s->cols[c].text : "",
                         style.secondary, fmt == 1 ? ALIGN_RIGHT : fmt == 2 ? ALIGN_CENTER : ALIGN_LEFT);
            p.c->clip = clip;
            gfx_fill(p.c, (struct rect){cell.x + cell.w - 1, cell.y + 4, 1, hh - 8}, style.separator);
        }
    }
    int rows = page_rows(w, s);
    if (s->count > rows) {
        int track = w->rect.h - hh, knob = MAX(12, track * rows / s->count);
        int at = (track - knob) * s->top / MAX(1, s->count - rows);
        gfx_round(p.c, (struct rect){p.r.x + p.r.w - 5, p.r.y + hh + at, 3, knob}, 1, style.tertiary);
    }
    int tw = total_w(w, s);
    if (tw > w->rect.w) {
        int knob = MAX(12, w->rect.w * w->rect.w / tw), at = (w->rect.w - knob) * s->xoff / MAX(1, tw - w->rect.w);
        gfx_round(p.c, (struct rect){p.r.x + at, p.r.y + p.r.h - 5, knob, 3}, 1, style.tertiary);
    }
    w32_ctl_end(w, &p);
}

static int next_item(struct lv *s, int start, uint32_t flags)
{
    uint32_t want = flags & 3;
    if (flags & 0x100)
        return start > 0 ? start - 1 : -1;
    if (flags & 0x200)
        return start + 1 < s->count ? start + 1 : -1;
    for (int i = start < 0 ? 0 : start + 1; i < s->count; i++)
        if ((s->items[i].state & want) == want)
            return i;
    return -1;
}

static LRESULT find_item(struct w32_window *w, struct lv *s, int start, const uint8_t *p, bool wide)
{
    if (!p)
        return -1;
    uint32_t flags = (uint32_t)get32(p);
    uint64_t param = 0;
    memcpy(&param, p + (w32->win64 ? 16 : 8), (size_t)ptr_size());
    char *want = (flags & 0x2a) ? dup_text(get_ptr(p + ptr_size()), wide) : NULL;
    char buf[260];
    LRESULT found = -1;
    for (int i = start + 1; i < s->count && found < 0; i++) {
        if (flags & 1) {
            if (s->items[i].param == param)
                found = i;
            continue;
        }
        if (!want || want == CALLBACK_TEXT)
            break;
        const char *t = cell_text(w, s, i, 0, buf, sizeof(buf), NULL);
        if ((flags & 8) ? !str_ncasecmp(t, want, strlen(want)) : !str_casecmp(t, want))
            found = i;
    }
    if (want != CALLBACK_TEXT)
        w32_free(want);
    return found;
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

static void sort_items(struct w32_window *w, struct lv *s, void *fn, uint64_t param, bool by_index)
{
    if (!fn || owner_data(w))
        return;
    for (int i = 1; i < s->count; i++) {
        struct lv_item cur = s->items[i];
        int k = i;
        for (; k > 0; k--) {
            uint64_t a = by_index ? (uint64_t)(k - 1) : s->items[k - 1].param, b = by_index ? (uint64_t)i : cur.param;
            if (call_compare(fn, a, b, param) <= 0)
                break;
            s->items[k] = s->items[k - 1];
        }
        s->items[k] = cur;
    }
    w32_invalidate(w, NULL, true);
}

static void key_down(struct w32_window *w, struct lv *s, WPARAM vk)
{
    uint8_t *nm = nm_new(w, LVN_KEYDOWN, 8);
    if (nm) {
        uint16_t key = (uint16_t)vk;
        memcpy(nm + hdr_size(), &key, 2);
        nm_send(w, nm);
    }
    int cur = focused(s), rows = page_rows(w, s), to = cur;
    switch (vk) {
    case 0x26: to = cur - 1; break;
    case 0x28: to = cur + 1; break;
    case 0x21: to = cur - rows; break;
    case 0x22: to = cur + rows; break;
    case 0x24: to = 0; break;
    case 0x23: to = s->count - 1; break;
    case 0x0d:
        notify(w, NM_RETURN);
        notify_item(w, s, LVN_ITEMACTIVATE, cur, 0, 0, 0, 0, 0, 0);
        return;
    case 0x20:
        if (cur >= 0 && (s->ex & LVS_EX_CHECKBOXES))
            set_state(w, s, cur, ((s->items[cur].state >> 12 & 15) == 2 ? 1u : 2u) << 12, 0xf000);
        return;
    case 'A':
        if (!(w->style & LVS_SINGLESEL) && (w32_get_key_state(0x11) & 0x8000))
            for (int k = 0; k < s->count; k++)
                set_state(w, s, k, LVIS_SELECTED, LVIS_SELECTED);
        return;
    default: return;
    }
    if (!s->count)
        return;
    to = MAX(0, MIN(to, s->count - 1));
    click_select(w, s, to, (w32_get_key_state(0x10) & 0x8000) ? MK_SHIFT_ : 0);
}

static LRESULT mouse(struct w32_window *w, struct lv *s, UINT msg, WPARAM wp, LPARAM lp)
{
    int x = (int16_t)lp, y = (int16_t)(lp >> 16), hh = head_h(w, s);
    if (msg == WM_MOUSEMOVE) {
        if (s->resize >= 0) {
            s->cols[s->resize].width = MAX(8, x - s->resize_x);
            w32_invalidate(w, NULL, true);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP_) {
        if (s->resize >= 0) {
            s->resize = -1;
            w32_set_capture_internal(NULL);
        }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN)
        w32_set_focus(w);
    if (y < hh) {
        for (int c = 0; c < s->ncols && msg == WM_LBUTTONDOWN; c++) {
            int edge = col_x(s, c + 1);
            if (x >= edge - 4 && x <= edge + 4) {
                s->resize = c;
                s->resize_x = col_x(s, c);
                w32_set_capture_internal(w);
                return 0;
            }
        }
        for (int c = 0; c < s->ncols && msg == WM_LBUTTONDOWN; c++)
            if (x >= col_x(s, c) && x < col_x(s, c + 1))
                notify_item(w, s, LVN_COLUMNCLICK, -1, c, 0, 0, 0, x, y);
        return 0;
    }
    int sub, i = hit_item(w, s, x, y, &sub);
    if (i >= 0 && msg != WM_LBUTTONDBLCLK) {
        bool on_check = (s->ex & LVS_EX_CHECKBOXES) && msg == WM_LBUTTONDOWN &&
                        x - item_rect(w, s, i, 0).x - 6 - s->items[i].indent * 16 < 18;
        if (on_check)
            set_state(w, s, i, ((s->items[i].state >> 12 & 15) == 2 ? 1u : 2u) << 12, 0xf000);
        else if (msg == WM_LBUTTONDOWN || !(s->items[i].state & LVIS_SELECTED))
            click_select(w, s, i, wp);
    } else if (i < 0 && msg == WM_LBUTTONDOWN && !(wp & (MK_SHIFT_ | MK_CONTROL_))) {
        for (int k = 0; k < s->count; k++)
            set_state(w, s, k, 0, LVIS_SELECTED);
    }
    int code = msg == WM_LBUTTONDOWN ? NM_CLICK : msg == WM_LBUTTONDBLCLK ? NM_DBLCLK : NM_RCLICK;
    notify_item(w, s, code, i, sub, 0, 0, 0, x, y);
    if (msg == WM_LBUTTONDBLCLK && i >= 0)
        notify_item(w, s, LVN_ITEMACTIVATE, i, sub, 0, 0, 0, x, y);
    return 0;
}

static LRESULT text_width(struct w32_window *w, uintptr_t text, bool wide)
{
    char *t = text ? dup_text(text, wide) : NULL;
    if (!t || t == CALLBACK_TEXT)
        return 0;
    int width = gfx_text_width(w32_ctl_font(w), t);
    w32_free(t);
    return width;
}

LRESULT WINAPI w32_listview_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    struct lv *s = lv_of(w);
    if (!s)
        return w32_def_window_proc(h, msg, wp, lp);
    uint8_t *ptr = (uint8_t *)(uintptr_t)(uint32_t)lp;
    int i = (int)wp;
    bool valid = i >= 0 && i < s->count;
    switch (msg) {
    case LVM_GETITEMCOUNT: return s->count;
    case LVM_SETITEMCOUNT:
        if (!owner_data(w) || i < 0)
            return TRUE;
        for (int k = i; k < s->count; k++)
            free_item(s, &s->items[k]);
        if (!resize_items(s, i))
            return FALSE;
        s->count = i;
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return TRUE;
    case LVM_INSERTITEMA:
    case LVM_INSERTITEMW: return insert_item(w, s, ptr, msg == LVM_INSERTITEMW);
    case LVM_SETITEMA:
    case LVM_SETITEMW: {
        int at = ptr ? get32(ptr + 4) : -1;
        if (at < 0 || at >= s->count)
            return FALSE;
        apply_item(s, &s->items[at], ptr, msg == LVM_SETITEMW);
        w32_invalidate(w, NULL, false);
        return TRUE;
    }
    case LVM_GETITEMA:
    case LVM_GETITEMW: {
        int at = ptr ? get32(ptr + 4) : -1;
        if (at < 0 || at >= s->count)
            return FALSE;
        get_item(w, s, at, ptr, msg == LVM_GETITEMW);
        return TRUE;
    }
    case LVM_GETITEMTEXTA:
    case LVM_GETITEMTEXTW: {
        if (!valid || !ptr)
            return 0;
        uint32_t mask = LVIF_TEXT;
        memcpy(ptr, &mask, 4);
        get_item(w, s, i, ptr, msg == LVM_GETITEMTEXTW);
        uintptr_t out = get_ptr(ptr + off_text());
        return !out ? 0 : msg == LVM_GETITEMTEXTW ? (LRESULT)w32_wcslen((const WCHAR *)out) : (LRESULT)strlen((char *)out);
    }
    case LVM_SETITEMTEXTA:
    case LVM_SETITEMTEXTW:
        if (!valid || !ptr)
            return FALSE;
        set_sub(s, &s->items[i], get32(ptr + 8), get_ptr(ptr + off_text()), msg == LVM_SETITEMTEXTW);
        w32_invalidate(w, NULL, false);
        return TRUE;
    case LVM_DELETEITEM: return delete_item(w, s, i);
    case LVM_DELETEALLITEMS: delete_all(w, s, true); return TRUE;
    case LVM_SETITEMSTATE: {
        if (!ptr)
            return FALSE;
        uint32_t st = (uint32_t)get32(ptr + 12), sm = (uint32_t)get32(ptr + 16);
        if (i == -1)
            for (int k = 0; k < s->count; k++)
                set_state(w, s, k, st, sm);
        else if (valid)
            set_state(w, s, i, st, sm);
        else
            return FALSE;
        return TRUE;
    }
    case LVM_GETITEMSTATE: return valid ? (LRESULT)(s->items[i].state & (uint32_t)lp) : 0;
    case LVM_GETNEXTITEM: return next_item(s, i, (uint32_t)lp);
    case LVM_GETSELECTEDCOUNT: {
        int n = 0;
        for (int k = 0; k < s->count; k++)
            n += (s->items[k].state & LVIS_SELECTED) != 0;
        return n;
    }
    case LVM_GETSELECTIONMARK: return s->mark;
    case LVM_SETSELECTIONMARK: {
        int old = s->mark;
        s->mark = (int)lp;
        return old;
    }
    case LVM_FINDITEMA:
    case LVM_FINDITEMW: return find_item(w, s, i, ptr, msg == LVM_FINDITEMW);
    case LVM_GETITEMRECT:
    case LVM_GETSUBITEMRECT: {
        if (!valid || !ptr)
            return FALSE;
        RECT *r = (RECT *)ptr;
        int sub = msg == LVM_GETSUBITEMRECT ? r->top : -1;
        struct rect a = item_rect(w, s, i, sub > 0 ? sub : -1);
        if ((msg == LVM_GETITEMRECT || sub == 0) && r->left == 2 && report(s) && s->ncols)
            a = item_rect(w, s, i, 0);
        *r = (RECT){a.x, a.y, a.x + a.w, a.y + a.h};
        return TRUE;
    }
    case LVM_HITTEST:
    case LVM_SUBITEMHITTEST: {
        if (!ptr)
            return -1;
        int sub, at = hit_item(w, s, get32(ptr), get32(ptr + 4), &sub);
        uint32_t flags = at >= 0 ? 0xe : 1;
        memcpy(ptr + 8, &flags, 4);
        memcpy(ptr + 12, &at, 4);
        if (msg == LVM_SUBITEMHITTEST)
            memcpy(ptr + 16, &sub, 4);
        return at;
    }
    case LVM_ENSUREVISIBLE: ensure_visible(w, s, i); w32_invalidate(w, NULL, true); return TRUE;
    case LVM_ISITEMVISIBLE: return valid && i >= s->top && i < s->top + page_rows(w, s);
    case LVM_SCROLL:
        s->xoff += (int)wp;
        s->top += (int)lp / row_h(w);
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return TRUE;
    case LVM_GETTOPINDEX: return s->top;
    case LVM_GETCOUNTPERPAGE: return page_rows(w, s);
    case LVM_GETORIGIN:
        if (ptr)
            *(POINT *)ptr = (POINT){report(s) ? 0 : s->xoff, 0};
        return !report(s);
    case LVM_GETVIEWRECT:
        if (ptr)
            *(RECT *)ptr = (RECT){0, 0, total_w(w, s), head_h(w, s) + s->count * row_h(w)};
        return TRUE;
    case LVM_INSERTCOLUMNA:
    case LVM_INSERTCOLUMNW: return insert_col(w, s, i, ptr, msg == LVM_INSERTCOLUMNW);
    case LVM_DELETECOLUMN: return delete_col(w, s, i);
    case LVM_SETCOLUMNA:
    case LVM_SETCOLUMNW:
        if (i < 0 || i >= s->ncols || !ptr)
            return FALSE;
        apply_col(&s->cols[i], ptr, msg == LVM_SETCOLUMNW);
        w32_invalidate(w, NULL, true);
        return TRUE;
    case LVM_GETCOLUMNA:
    case LVM_GETCOLUMNW: {
        if (i < 0 || i >= s->ncols || !ptr)
            return FALSE;
        uint32_t mask;
        memcpy(&mask, ptr, 4);
        struct lv_col *c = &s->cols[i];
        if (mask & LVCF_FMT)
            memcpy(ptr + 4, &c->fmt, 4);
        if (mask & LVCF_WIDTH)
            memcpy(ptr + 8, &c->width, 4);
        if (mask & LVCF_SUBITEM)
            memcpy(ptr + (w32->win64 ? 28 : 20), &i, 4);
        uintptr_t out = get_ptr(ptr + (w32->win64 ? 16 : 12));
        int cch = get32(ptr + (w32->win64 ? 24 : 16));
        if ((mask & LVCF_TEXT) && out && cch > 0) {
            const char *t = c->text ? c->text : "";
            if (msg == LVM_GETCOLUMNW) {
                size_t n = w32_cp_to_wide(t, (int)strlen(t), (WCHAR *)out, (size_t)cch - 1, 65001);
                ((WCHAR *)out)[n] = 0;
            } else {
                str_copy((char *)out, t, (size_t)cch);
            }
        }
        return TRUE;
    }
    case LVM_GETCOLUMNWIDTH: return i >= 0 && i < s->ncols ? s->cols[i].width : report(s) ? 0 : w->rect.w;
    case LVM_SETCOLUMNWIDTH:
        if (i < 0 || i >= s->ncols)
            return FALSE;
        s->cols[i].width = (int)lp == -1 ? auto_width(w, s, i, false) : (int)lp == -2 ? auto_width(w, s, i, true)
                                                                                     : MAX(0, (int)(int16_t)lp);
        if ((int)lp == -2 && i == s->ncols - 1)
            s->cols[i].width = MAX(s->cols[i].width, w->rect.w - col_x(s, i) - s->xoff);
        w32_invalidate(w, NULL, true);
        return TRUE;
    case LVM_GETCOLUMNORDERARRAY:
        for (int k = 0; ptr && k < i && k < s->ncols; k++)
            memcpy(ptr + k * 4, &k, 4);
        return ptr != NULL;
    case LVM_SETCOLUMNORDERARRAY: return TRUE;
    case LVM_GETSTRINGWIDTHA:
    case LVM_GETSTRINGWIDTHW: return text_width(w, (uintptr_t)ptr, msg == LVM_GETSTRINGWIDTHW);
    case LVM_SETIMAGELIST: {
        if (i < 0 || i > 2)
            return 0;
        HANDLE old = s->images[i];
        s->images[i] = (HANDLE)(uintptr_t)(uint32_t)lp;
        w32_invalidate(w, NULL, true);
        return (LRESULT)(uintptr_t)old;
    }
    case LVM_GETIMAGELIST: return i >= 0 && i <= 2 ? (LRESULT)(uintptr_t)s->images[i] : 0;
    case LVM_SETEXTENDEDSTYLE: {
        uint32_t old = s->ex;
        s->ex = wp ? (s->ex & ~(uint32_t)wp) | ((uint32_t)lp & (uint32_t)wp) : (uint32_t)lp;
        w32_invalidate(w, NULL, true);
        return (LRESULT)old;
    }
    case LVM_GETEXTENDEDSTYLE: return (LRESULT)s->ex;
    case LVM_SETCALLBACKMASK: s->callback_mask = (uint32_t)wp; return TRUE;
    case LVM_GETCALLBACKMASK: return (LRESULT)s->callback_mask;
    case LVM_SETBKCOLOR: s->bk = (COLORREF)lp; return TRUE;
    case LVM_SETTEXTCOLOR: s->text = (COLORREF)lp; return TRUE;
    case LVM_SETTEXTBKCOLOR: s->text_bk = (COLORREF)lp; return TRUE;
    case LVM_GETTEXTCOLOR: return (LRESULT)s->text;
    case LVM_GETTEXTBKCOLOR: return (LRESULT)s->text_bk;
    case LVM_SETVIEW:
        if (i < 0 || i > 4)
            return -1;
        s->view = i == 4 ? 3 : i;
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return 1;
    case LVM_GETVIEW: return s->view;
    case LVM_SORTITEMS:
    case LVM_SORTITEMSEX:
        sort_items(w, s, ptr, (uint64_t)wp, msg == LVM_SORTITEMSEX);
        return TRUE;
    case 0x1015: /* LVM_REDRAWITEMS */
    case 0x102a: /* LVM_UPDATE */
    case 0x1016: /* LVM_ARRANGE */ w32_invalidate(w, NULL, false); return TRUE;
    case 0x101f: /* LVM_GETHEADER */
    case 0x1018: /* LVM_GETEDITCONTROL */
    case 0x1017: /* LVM_EDITLABELA */
    case 0x1076: /* LVM_EDITLABELW */ return 0;
    case 0x103d: /* LVM_GETHOTITEM */ return -1;
    case 0x2005: { /* CCM_SETUNICODEFORMAT */
        bool old = s->unicode;
        s->unicode = wp != 0;
        return old;
    }
    case 0x2006: return s->unicode;
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
    case WM_RBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP_: return mouse(w, s, msg, wp, lp);
    case WM_MOUSEWHEEL: {
        int steps = -(int16_t)(wp >> 16) / 40;
        if (wp & MK_SHIFT_)
            s->xoff += steps * 16;
        else
            s->top += steps;
        clamp_scroll(w, s);
        w32_invalidate(w, NULL, true);
        return 0;
    }
    case WM_VSCROLL:
    case WM_HSCROLL_: {
        int code = (int)(wp & 0xffff), page = msg == WM_VSCROLL ? page_rows(w, s) : w->rect.w / 2;
        int *v = msg == WM_VSCROLL ? &s->top : &s->xoff, step = msg == WM_VSCROLL ? 1 : 16;
        *v = code == 0 ? *v - step : code == 1 ? *v + step : code == 2 ? *v - page : code == 3 ? *v + page
             : code == 6 ? 0 : code == 7 ? 1 << 24 : *v;
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
        delete_all(w, s, false);
        for (int c = 0; c < s->ncols; c++)
            w32_free(s->cols[c].text);
        w32_free(s->cols);
        w32_free(s->items);
        w32_free(s);
        w->list = NULL;
        break;
    }
    return w32_def_window_proc(h, msg, wp, lp);
}
#endif
