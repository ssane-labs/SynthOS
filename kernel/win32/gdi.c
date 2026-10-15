#include <synth/config.h>
#if CONFIG_GUI
#include <synth/codepage.h>
#include <synth/string.h>

#include "w32gui.h"

enum {
    OBJ_PEN = 1,
    OBJ_BRUSH = 2,
    OBJ_DC = 3,
    OBJ_FONT = 6,
    OBJ_BITMAP = 7,
    OBJ_REGION = 8,
    OBJ_MEMDC = 10,
};

enum {
    PS_NULL = 5,
    BS_NULL = 1,
    TRANSPARENT = 1,
    OPAQUE = 2,
    TA_UPDATECP = 1,
    TA_RIGHT = 2,
    TA_CENTER = 6,
    TA_BOTTOM = 8,
    TA_BASELINE = 24,
};

enum {
    SRCCOPY = 0x00cc0020u,
    SRCPAINT = 0x00ee0086u,
    SRCAND = 0x008800c6u,
    SRCINVERT = 0x00660046u,
    NOTSRCCOPY = 0x00330008u,
    PATCOPY = 0x00f00021u,
    PATINVERT = 0x005a0049u,
    DSTINVERT = 0x00550009u,
    BLACKNESS = 0x00000042u,
    WHITENESS = 0x00ff0062u,
};

struct gdi_obj {
    uint32_t magic;
    int kind;
    bool stock;
    COLORREF color;
    int style;
    int width;
    const struct ui_font *font;
    int height, weight;
    bool italic, underline;
    char face[32];
    struct canvas canvas;
    int bpp;
    void *bits;
    struct rect region;
};

struct dc_state {
    struct gdi_obj *pen, *brush, *font, *bitmap;
    COLORREF text, bk, dc_brush, dc_pen;
    int bk_mode, rop2, org_x, org_y;
    UINT align;
    struct rect clip;
    bool clipped;
};

struct w32_dc {
    uint32_t magic;
    struct w32_window *win;
    bool memory;
    struct rect visible;
    struct dc_state s;
    int x, y;
    struct dc_state saved[8];
    int depth;
};

static W32_STATE struct gdi_obj *stock[20];
static W32_STATE struct gdi_obj *sys_brushes[31];
static W32_STATE struct gdi_obj default_bitmap_obj;

static const uint32_t sys_colors[31] = {
    0xc8c8c8, 0x3a6ea5, 0x000000, 0xa0a0a0, 0xf0f0f0, 0xffffff, 0x646464, 0x000000, 0x000000, 0x000000,
    0xb4b4b4, 0xf4f7fc, 0xababab, 0xff7a0a, 0xffffff, 0xececec, 0xa0a0a0, 0x6d6d6d, 0x1d1d1d, 0x6d6d6d,
    0xffffff, 0x696969, 0xe3e3e3, 0x000000, 0xe1ffff, 0xb5b5b5, 0xcc6600, 0xead1b9, 0xf2e4d7, 0xff7a0a,
    0xececec,
};

uint32_t w32_rgb(COLORREF c)
{
    return RGB(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
}

static COLORREF to_colorref(uint32_t px)
{
    return ((px >> 16) & 0xff) | (px & 0xff00) | (px & 0xff) << 16;
}

COLORREF w32_syscolor(int index)
{
    if (index < 0 || index >= 31)
        return 0;
    uint32_t c;
    switch (index) {
    case 5: c = style.content; break;
    case 8:
    case 18: c = style.text; break;
    case 15:
    case 4: c = style.window; break;
    case 13:
    case 29: c = style.accent; break;
    case 14: c = style.accent_text; break;
    case 17: c = style.secondary; break;
    default: return sys_colors[index] & 0xffffff;
    }
    return to_colorref(c);
}

static struct gdi_obj *obj(HANDLE h)
{
    struct gdi_obj *o = h;
    if ((uintptr_t)h < 0x10000 || !o || o->magic != W32_GDI_MAGIC)
        return NULL;
    return o;
}

static struct gdi_obj *new_obj(int kind)
{
    struct gdi_obj *o = w32_zalloc(sizeof(*o));
    if (o) {
        o->magic = W32_GDI_MAGIC;
        o->kind = kind;
    }
    return o;
}

static struct gdi_obj *make_stock(int kind, COLORREF color, int style)
{
    struct gdi_obj *o = new_obj(kind);
    if (o) {
        o->stock = true;
        o->color = color;
        o->style = style;
        o->width = 1;
    }
    return o;
}

static const struct ui_font *pick_font(int height, int weight, const char *face, int pitch)
{
    int h = height < 0 ? -height : height ? height * 3 / 4 + 1 : 13;
    bool mono = (pitch & 3) == 1;
    static const char *const mono_faces[] = {"courier", "consolas", "lucida console", "terminal", "fixedsys",
                                             "courier new", "monospace"};
    for (size_t i = 0; i < ARRAY_SIZE(mono_faces) && face; i++)
        if (!str_casecmp(face, mono_faces[i]))
            mono = true;
    if (mono)
        return &font_mono;
    if (h <= 11)
        return &font_small;
    if (h <= 16)
        return weight >= 600 ? &font_bold : &font_sans;
    if (h <= 23)
        return &font_title;
    return &font_large;
}

HANDLE w32_stock(int index)
{
    if (index < 0 || index >= 20)
        return NULL;
    if (!stock[index]) {
        switch (index) {
        case 0: stock[index] = make_stock(OBJ_BRUSH, 0xffffff, 0); break;
        case 1: stock[index] = make_stock(OBJ_BRUSH, 0xc0c0c0, 0); break;
        case 2: stock[index] = make_stock(OBJ_BRUSH, 0x808080, 0); break;
        case 3: stock[index] = make_stock(OBJ_BRUSH, 0x404040, 0); break;
        case 4: stock[index] = make_stock(OBJ_BRUSH, 0x000000, 0); break;
        case 5: stock[index] = make_stock(OBJ_BRUSH, 0, BS_NULL); break;
        case 6: stock[index] = make_stock(OBJ_PEN, 0xffffff, 0); break;
        case 7: stock[index] = make_stock(OBJ_PEN, 0x000000, 0); break;
        case 8: stock[index] = make_stock(OBJ_PEN, 0, PS_NULL); break;
        case 18: stock[index] = make_stock(OBJ_BRUSH, 0xffffff, 0); break;
        case 19: stock[index] = make_stock(OBJ_PEN, 0x000000, 0); break;
        case 15: stock[index] = make_stock(9, 0, 0); break;
        default:
            stock[index] = make_stock(OBJ_FONT, 0, 0);
            if (stock[index])
                stock[index]->font = index == 10 || index == 11 || index == 16 ? &font_mono : &font_sans;
            break;
        }
    }
    return stock[index];
}

HANDLE w32_syscolor_brush(int index)
{
    if (index < 0 || index >= 31)
        return NULL;
    if (!sys_brushes[index])
        sys_brushes[index] = make_stock(OBJ_BRUSH, 0, 0);
    if (sys_brushes[index])
        sys_brushes[index]->color = w32_syscolor(index);
    return sys_brushes[index];
}

static struct gdi_obj *brush_of(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (v && v <= 31)
        return w32_syscolor_brush((int)v - 1);
    struct gdi_obj *o = obj(h);
    return o && o->kind == OBJ_BRUSH ? o : NULL;
}

const struct ui_font *w32_font_of(HANDLE font)
{
    struct gdi_obj *o = obj(font);
    return o && o->kind == OBJ_FONT && o->font ? o->font : &font_sans;
}

static struct w32_dc *dc_of(HANDLE h)
{
    struct w32_dc *d = h;
    if ((uintptr_t)h < 0x10000 || !d || d->magic != W32_DC_MAGIC)
        return NULL;
    return d;
}

static void dc_defaults(struct w32_dc *d)
{
    d->magic = W32_DC_MAGIC;
    d->s.pen = w32_stock(7);
    d->s.brush = w32_stock(0);
    d->s.font = w32_stock(13);
    d->s.text = 0;
    d->s.bk = 0xffffff;
    d->s.dc_brush = 0xffffff;
    d->s.dc_pen = 0;
    d->s.bk_mode = OPAQUE;
    d->s.rop2 = 13;
}

HANDLE w32_window_dc(struct w32_window *w, struct rect visible)
{
    struct w32_dc *d = w32_zalloc(sizeof(*d));
    if (!d)
        return NULL;
    dc_defaults(d);
    d->win = w;
    struct rect a = w32_area(w);
    d->visible = (struct rect){visible.x - a.x, visible.y - a.y, visible.w, visible.h};
    if (w->font)
        d->s.font = obj(w->font) ? obj(w->font) : d->s.font;
    if (w->control)
        d->s.font = NULL;
    return d;
}

HANDLE w32_screen_dc(void)
{
    struct w32_dc *d = w32_zalloc(sizeof(*d));
    if (d)
        dc_defaults(d);
    return d;
}

void w32_free_dc(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    if (d && !d->memory) {
        d->magic = 0;
        w32_free(d);
    }
}

struct target {
    struct canvas *c;
    int ox, oy;
    struct rect clip;
};

static bool target_of(struct w32_dc *d, struct target *t)
{
    if (d->memory) {
        struct gdi_obj *b = d->s.bitmap;
        if (!b || !b->canvas.px || b->bpp != 32)
            return false;
        t->c = &b->canvas;
        t->ox = 0;
        t->oy = 0;
        t->clip = (struct rect){0, 0, b->canvas.w, b->canvas.h};
    } else {
        if (!d->win || !w32_win(d->win))
            return false;
        struct w32_window *top = w32_top(d->win);
        if (!top->wm)
            return false;
        t->c = &top->wm->surface;
        struct rect a = w32_area(d->win);
        t->ox = a.x;
        t->oy = a.y;
        t->clip = rect_intersect((struct rect){a.x + d->visible.x, a.y + d->visible.y, d->visible.w, d->visible.h},
                                 (struct rect){0, 0, t->c->w, t->c->h});
    }
    if (d->s.clipped)
        t->clip = rect_intersect(t->clip, (struct rect){t->ox + d->s.clip.x, t->oy + d->s.clip.y, d->s.clip.w,
                                                         d->s.clip.h});
    t->ox -= d->s.org_x;
    t->oy -= d->s.org_y;
    t->c->clip = t->clip;
    return !rect_empty(t->clip);
}

static void damage(struct w32_dc *d, const struct target *t, struct rect r)
{
    if (!d->memory && d->win)
        w32_damage(d->win, rect_intersect(r, t->clip));
}

struct canvas *w32_dc_canvas(HANDLE h, int *ox, int *oy)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t))
        return NULL;
    *ox = t.ox;
    *oy = t.oy;
    return t.c;
}

void w32_dc_damage(HANDLE h, struct rect r)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (d && target_of(d, &t))
        damage(d, &t, r);
}

HANDLE w32_dc_font(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    return d ? d->s.font : NULL;
}

COLORREF w32_dc_text_color(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    return d ? d->s.text : 0;
}

static uint32_t brush_color(struct w32_dc *d, struct gdi_obj *b, bool *none)
{
    *none = !b || b->style == BS_NULL;
    if (b == stock[18])
        return w32_rgb(d->s.dc_brush);
    return b ? w32_rgb(b->color) : 0;
}

static uint32_t pen_color(struct w32_dc *d, bool *none)
{
    struct gdi_obj *p = d->s.pen;
    *none = !p || p->style == PS_NULL;
    if (p == stock[19])
        return w32_rgb(d->s.dc_pen);
    return p ? w32_rgb(p->color) : 0;
}

static int pen_width(struct w32_dc *d)
{
    return d->s.pen ? MAX(d->s.pen->width, 1) : 1;
}

static void fill(struct target *t, struct rect r, uint32_t color)
{
    gfx_clear(t->c, rect_intersect(r, t->clip), color);
}

void w32_fill_brush(HANDLE h, const RECT *r, HANDLE brush)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    struct gdi_obj *b = brush_of(brush);
    if (!d || !b || !target_of(d, &t))
        return;
    bool none;
    uint32_t color = brush_color(d, b, &none);
    if (none)
        return;
    struct rect area = {t.ox + r->left, t.oy + r->top, r->right - r->left, r->bottom - r->top};
    fill(&t, area, color);
    damage(d, &t, area);
}

static int WINAPI FillRect(HANDLE dc, const RECT *r, HANDLE brush)
{
    w32_fill_brush(dc, r, brush);
    return 1;
}

static int WINAPI FrameRect(HANDLE h, const RECT *r, HANDLE brush)
{
    RECT edges[4] = {{r->left, r->top, r->right, r->top + 1},
                     {r->left, r->bottom - 1, r->right, r->bottom},
                     {r->left, r->top, r->left + 1, r->bottom},
                     {r->right - 1, r->top, r->right, r->bottom}};
    for (int i = 0; i < 4; i++)
        w32_fill_brush(h, &edges[i], brush);
    return 1;
}

static void invert(struct target *t, struct rect r)
{
    r = rect_intersect(r, t->clip);
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t *p = t->c->px + y * t->c->stride + x;
            *p = (*p ^ 0x00ffffffu) | 0xff000000u;
        }
}

static BOOL WINAPI InvertRect(HANDLE h, const RECT *r)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t))
        return FALSE;
    struct rect area = {t.ox + r->left, t.oy + r->top, r->right - r->left, r->bottom - r->top};
    invert(&t, area);
    damage(d, &t, area);
    return TRUE;
}

static DWORD WINAPI GetSysColor(int index)
{
    return w32_syscolor(index);
}

static HANDLE WINAPI GetSysColorBrush(int index)
{
    return w32_syscolor_brush(index);
}

static HANDLE WINAPI GetStockObject(int index)
{
    return w32_stock(index);
}

static HANDLE WINAPI CreateSolidBrush(COLORREF color)
{
    struct gdi_obj *o = new_obj(OBJ_BRUSH);
    if (o)
        o->color = color & 0xffffff;
    return o;
}

static HANDLE WINAPI CreateHatchBrush(int hatch, COLORREF color)
{
    UNUSED(hatch);
    return CreateSolidBrush(color);
}

static HANDLE WINAPI CreateBrushIndirect(const uint32_t *lb)
{
    struct gdi_obj *o = new_obj(OBJ_BRUSH);
    if (o) {
        o->style = (int)lb[0];
        o->color = lb[1] & 0xffffff;
    }
    return o;
}

static HANDLE WINAPI CreatePen(int style, int width, COLORREF color)
{
    struct gdi_obj *o = new_obj(OBJ_PEN);
    if (o) {
        o->style = style & 0xf;
        o->width = MAX(width, 1);
        o->color = color & 0xffffff;
    }
    return o;
}

static HANDLE WINAPI CreatePenIndirect(const uint32_t *lp)
{
    return CreatePen((int)lp[0], (int)lp[1], lp[3]);
}

static HANDLE WINAPI ExtCreatePen(DWORD style, DWORD width, const uint32_t *lb, DWORD count, const DWORD *dashes)
{
    UNUSED(count);
    UNUSED(dashes);
    return CreatePen((int)(style & 0xf), (int)width, lb ? lb[1] : 0);
}

struct logfont {
    LONG height, width, escapement, orientation, weight;
    BYTE italic, underline, strike, charset, out_precision, clip_precision, quality, pitch;
};

static HANDLE make_font(const struct logfont *lf, const char *face)
{
    struct gdi_obj *o = new_obj(OBJ_FONT);
    if (!o)
        return NULL;
    o->height = lf->height;
    o->weight = lf->weight;
    o->italic = lf->italic;
    o->underline = lf->underline;
    str_copy(o->face, face ? face : "", sizeof(o->face));
    o->font = pick_font(lf->height, lf->weight, face, lf->pitch);
    return o;
}

static HANDLE WINAPI CreateFontIndirectA(const uint8_t *lf)
{
    char face[33];
    memcpy(face, lf + 28, 32);
    face[32] = 0;
    return make_font((const struct logfont *)lf, face);
}

static HANDLE WINAPI CreateFontIndirectW(const uint8_t *lf)
{
    WCHAR wide[33];
    memcpy(wide, lf + 28, 64);
    wide[32] = 0;
    char *face = w32_narrow(wide);
    HANDLE h = make_font((const struct logfont *)lf, face);
    w32_free(face);
    return h;
}

static HANDLE WINAPI CreateFontA(int height, int width, int esc, int orient, int weight, DWORD italic,
                                 DWORD underline, DWORD strike, DWORD charset, DWORD out, DWORD clip, DWORD quality,
                                 DWORD pitch, LPCSTR face)
{
    UNUSED(width);
    UNUSED(esc);
    UNUSED(orient);
    UNUSED(strike);
    UNUSED(charset);
    UNUSED(out);
    UNUSED(clip);
    UNUSED(quality);
    struct logfont lf = {height, 0, 0, 0, weight, (BYTE)italic, (BYTE)underline, 0, 0, 0, 0, 0, (BYTE)pitch};
    return make_font(&lf, face);
}

static HANDLE WINAPI CreateFontW(int height, int width, int esc, int orient, int weight, DWORD italic,
                                 DWORD underline, DWORD strike, DWORD charset, DWORD out, DWORD clip, DWORD quality,
                                 DWORD pitch, LPCWSTR face)
{
    char *f = face ? w32_narrow(face) : NULL;
    HANDLE h = CreateFontA(height, width, esc, orient, weight, italic, underline, strike, charset, out, clip,
                           quality, pitch, f);
    w32_free(f);
    return h;
}

static HANDLE WINAPI SelectObject(HANDLE h, HANDLE object)
{
    struct w32_dc *d = dc_of(h);
    struct gdi_obj *o = obj(object);
    if (!d || !o)
        return NULL;
    struct gdi_obj *old = NULL;
    switch (o->kind) {
    case OBJ_PEN: old = d->s.pen; d->s.pen = o; break;
    case OBJ_BRUSH: old = d->s.brush; d->s.brush = o; break;
    case OBJ_FONT:
        old = d->s.font ? d->s.font : w32_stock(13);
        d->s.font = o;
        break;
    case OBJ_BITMAP:
        if (!d->memory)
            return NULL;
        old = d->s.bitmap ? d->s.bitmap : &default_bitmap_obj;
        d->s.bitmap = o;
        break;
    case OBJ_REGION:
        d->s.clip = o->region;
        d->s.clipped = true;
        return (HANDLE)2;
    default: return NULL;
    }
    return old;
}

static BOOL WINAPI DeleteObject(HANDLE h)
{
    struct gdi_obj *o = obj(h);
    if (!o)
        return FALSE;
    if (o->stock)
        return TRUE;
    if (o->kind == OBJ_BITMAP && o->bits)
        w32_free(o->bits);
    o->magic = 0;
    w32_free(o);
    return TRUE;
}

static DWORD WINAPI GetObjectType(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    if (d)
        return d->memory ? OBJ_MEMDC : OBJ_DC;
    struct gdi_obj *o = obj(h);
    return o ? (DWORD)o->kind : 0;
}

void w32_logfont(void *out, bool wide, LONG height, LONG weight, BYTE italic, BYTE underline, const char *face)
{
    uint8_t *lf = out;
    memset(lf, 0, wide ? 92 : 60);
    memcpy(lf, &height, 4);
    memcpy(lf + 16, &weight, 4);
    lf[20] = italic;
    lf[21] = underline;
    lf[23] = 1;
    lf[26] = 5;
    if (!face)
        face = "";
    for (int i = 0; i < 31 && face[i]; i++) {
        if (wide) {
            WCHAR c = (uint8_t)face[i];
            memcpy(lf + 28 + i * 2, &c, 2);
        } else {
            lf[28 + i] = (uint8_t)face[i];
        }
    }
}

static int get_object(HANDLE h, int size, void *out, bool wide)
{
    struct gdi_obj *o = obj(h);
    if (!o)
        return 0;
    if (o->kind == OBJ_BITMAP) {
        if (!out)
            return 24;
        uint32_t bm[6] = {0, (uint32_t)o->canvas.w, (uint32_t)o->canvas.h, (uint32_t)(o->canvas.w * 4),
                          1u | 32u << 16, (uint32_t)o->bits};
        memcpy(out, bm, (size_t)MIN(size, 24));
        return MIN(size, 24);
    }
    if (o->kind == OBJ_FONT) {
        uint8_t lf[92];
        int n = wide ? 92 : 60;
        w32_logfont(lf, wide, o->height ? o->height : -o->font->height, o->weight, o->italic, o->underline, o->face);
        if (out)
            memcpy(out, lf, (size_t)MIN(size, n));
        return out ? MIN(size, n) : n;
    }
    if (o->kind == OBJ_PEN) {
        uint32_t lp[4] = {(uint32_t)o->style, (uint32_t)o->width, 0, o->color};
        if (out)
            memcpy(out, lp, (size_t)MIN(size, 16));
        return MIN(size, 16);
    }
    if (o->kind == OBJ_BRUSH) {
        uint32_t lb[3] = {(uint32_t)o->style, o->color, 0};
        if (out)
            memcpy(out, lb, (size_t)MIN(size, 12));
        return MIN(size, 12);
    }
    return 0;
}

static int WINAPI GetObjectA(HANDLE h, int size, void *out)
{
    return get_object(h, size, out, false);
}

static int WINAPI GetObjectW(HANDLE h, int size, void *out)
{
    return get_object(h, size, out, true);
}

static COLORREF WINAPI SetTextColor(HANDLE h, COLORREF c)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0xffffffffu;
    COLORREF old = d->s.text;
    d->s.text = c & 0xffffff;
    return old;
}

static COLORREF WINAPI GetTextColor(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    return d ? d->s.text : 0;
}

static COLORREF WINAPI SetBkColor(HANDLE h, COLORREF c)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0xffffffffu;
    COLORREF old = d->s.bk;
    d->s.bk = c & 0xffffff;
    return old;
}

static COLORREF WINAPI GetBkColor(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    return d ? d->s.bk : 0;
}

static int WINAPI SetBkMode(HANDLE h, int mode)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0;
    int old = d->s.bk_mode;
    d->s.bk_mode = mode;
    return old;
}

static int WINAPI GetBkMode(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    return d ? d->s.bk_mode : 0;
}

static COLORREF WINAPI SetDCBrushColor(HANDLE h, COLORREF c)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0xffffffffu;
    COLORREF old = d->s.dc_brush;
    d->s.dc_brush = c & 0xffffff;
    return old;
}

static COLORREF WINAPI SetDCPenColor(HANDLE h, COLORREF c)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0xffffffffu;
    COLORREF old = d->s.dc_pen;
    d->s.dc_pen = c & 0xffffff;
    return old;
}

static int WINAPI SetROP2(HANDLE h, int rop)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0;
    int old = d->s.rop2;
    d->s.rop2 = rop;
    return old;
}

static UINT WINAPI SetTextAlign(HANDLE h, UINT align)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0xffffffffu;
    UINT old = d->s.align;
    d->s.align = align;
    return old;
}

static int WINAPI SetMapMode(HANDLE h, int mode)
{
    UNUSED(h);
    UNUSED(mode);
    return 1;
}

static int WINAPI SetStretchBltMode(HANDLE h, int mode)
{
    UNUSED(h);
    UNUSED(mode);
    return 3;
}

static int WINAPI SetGraphicsMode(HANDLE h, int mode)
{
    UNUSED(h);
    UNUSED(mode);
    return 1;
}

static BOOL WINAPI SetViewportOrgEx(HANDLE h, int x, int y, POINT *old)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    if (old)
        *old = (POINT){-d->s.org_x, -d->s.org_y};
    d->s.org_x = -x;
    d->s.org_y = -y;
    return TRUE;
}

static BOOL WINAPI SetWindowOrgEx(HANDLE h, int x, int y, POINT *old)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    if (old)
        *old = (POINT){d->s.org_x, d->s.org_y};
    d->s.org_x = x;
    d->s.org_y = y;
    return TRUE;
}

static BOOL WINAPI OffsetViewportOrgEx(HANDLE h, int x, int y, POINT *old)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    if (old)
        *old = (POINT){-d->s.org_x, -d->s.org_y};
    d->s.org_x -= x;
    d->s.org_y -= y;
    return TRUE;
}

static int WINAPI SaveDC(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    if (!d || d->depth >= 8)
        return 0;
    d->saved[d->depth++] = d->s;
    return d->depth;
}

static BOOL WINAPI RestoreDC(HANDLE h, int level)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !d->depth)
        return FALSE;
    int target = level < 0 ? d->depth + level : level - 1;
    if (target < 0 || target >= d->depth)
        return FALSE;
    d->s = d->saved[target];
    d->depth = target;
    return TRUE;
}

static HANDLE WINAPI CreateRectRgn(int l, int t, int r, int b)
{
    struct gdi_obj *o = new_obj(OBJ_REGION);
    if (o)
        o->region = (struct rect){l, t, r - l, b - t};
    return o;
}

static HANDLE WINAPI CreateRectRgnIndirect(const RECT *r)
{
    return CreateRectRgn(r->left, r->top, r->right, r->bottom);
}

static int WINAPI SelectClipRgn(HANDLE h, HANDLE rgn)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0;
    struct gdi_obj *o = obj(rgn);
    d->s.clipped = o != NULL;
    if (o)
        d->s.clip = o->region;
    return o ? 2 : 1;
}

static int WINAPI IntersectClipRect(HANDLE h, int l, int t, int r, int b)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0;
    struct rect area = {l, t, r - l, b - t};
    d->s.clip = d->s.clipped ? rect_intersect(d->s.clip, area) : area;
    d->s.clipped = true;
    return 2;
}

static int WINAPI GetClipBox(HANDLE h, RECT *out)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t)) {
        *out = (RECT){0, 0, 0, 0};
        return 1;
    }
    *out = (RECT){t.clip.x - t.ox, t.clip.y - t.oy, t.clip.x - t.ox + t.clip.w, t.clip.y - t.oy + t.clip.h};
    return 2;
}

static const struct ui_font *dc_font(struct w32_dc *d);

static int region_kind(struct rect r)
{
    return r.w > 0 && r.h > 0 ? 2 : 1;
}

static BOOL WINAPI SetRectRgn(HANDLE rgn, int l, int t, int r, int b)
{
    struct gdi_obj *o = obj(rgn);
    if (!o || o->kind != OBJ_REGION)
        return FALSE;
    o->region = (struct rect){l, t, r - l, b - t};
    return TRUE;
}

static int WINAPI CombineRgn(HANDLE dst, HANDLE a, HANDLE b, int mode)
{
    struct gdi_obj *d = obj(dst), *x = obj(a), *y = obj(b);
    if (!d || !x || d->kind != OBJ_REGION)
        return 0;
    struct rect r = x->region, o = y ? y->region : r;
    if (mode == 1)
        r = rect_intersect(r, o);
    else if ((mode == 2 || mode == 3) && region_kind(o) == 2)
        r = region_kind(r) == 2 ? (struct rect){MIN(r.x, o.x), MIN(r.y, o.y),
                                                MAX(r.x + r.w, o.x + o.w) - MIN(r.x, o.x),
                                                MAX(r.y + r.h, o.y + o.h) - MIN(r.y, o.y)}
                                : o;
    else if (mode == 4 && o.x <= r.x && o.y <= r.y && o.x + o.w >= r.x + r.w && o.y + o.h >= r.y + r.h)
        r = (struct rect){0, 0, 0, 0};
    d->region = r;
    return region_kind(r);
}

static int WINAPI GetRgnBox(HANDLE rgn, RECT *out)
{
    struct gdi_obj *o = obj(rgn);
    if (!o || o->kind != OBJ_REGION)
        return 0;
    struct rect r = o->region;
    *out = (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
    return region_kind(r);
}

static BOOL WINAPI EqualRgn(HANDLE a, HANDLE b)
{
    struct gdi_obj *x = obj(a), *y = obj(b);
    return x && y && !memcmp(&x->region, &y->region, sizeof(x->region));
}

static DWORD WINAPI GetRegionData(HANDLE rgn, DWORD size, uint32_t *out)
{
    struct gdi_obj *o = obj(rgn);
    if (!o || o->kind != OBJ_REGION)
        return 0;
    struct rect r = o->region;
    bool any = region_kind(r) == 2;
    DWORD need = 32 + (any ? 16 : 0);
    if (!out)
        return need;
    if (size < need)
        return 0;
    int32_t box[4] = {r.x, r.y, r.x + r.w, r.y + r.h};
    out[0] = 32;
    out[1] = 1;
    out[2] = any;
    out[3] = any ? 16 : 0;
    memcpy(out + 4, box, 16);
    if (any)
        memcpy(out + 8, box, 16);
    return need;
}

static int WINAPI GetClipRgn(HANDLE h, HANDLE rgn)
{
    struct w32_dc *d = dc_of(h);
    struct gdi_obj *o = obj(rgn);
    if (!d || !o)
        return -1;
    if (!d->s.clipped)
        return 0;
    o->region = d->s.clip;
    return 1;
}

static HANDLE WINAPI GetCurrentObject(HANDLE h, UINT type)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return NULL;
    switch (type) {
    case OBJ_PEN: return d->s.pen;
    case OBJ_BRUSH: return d->s.brush;
    case OBJ_FONT: return d->s.font;
    case OBJ_BITMAP: return d->s.bitmap;
    default: return NULL;
    }
}

static BOOL WINAPI GetWorldTransform(HANDLE h, uint32_t *xform)
{
    static const uint32_t identity[6] = {0x3f800000u, 0, 0, 0x3f800000u, 0, 0};
    if (!dc_of(h) || !xform)
        return FALSE;
    memcpy(xform, identity, sizeof(identity));
    return TRUE;
}

static BOOL WINAPI SetBrushOrgEx(HANDLE h, int x, int y, POINT *old)
{
    UNUSED(x);
    UNUSED(y);
    if (old)
        *old = (POINT){0, 0};
    return dc_of(h) != NULL;
}

static DWORD WINAPI GetFontData(HANDLE h, DWORD table, DWORD offset, void *buf, DWORD size)
{
    UNUSED(h);
    UNUSED(table);
    UNUSED(offset);
    UNUSED(buf);
    UNUSED(size);
    return 0xffffffffu;
}

static BOOL WINAPI GetCharWidthW(HANDLE h, UINT first, UINT last, int *out)
{
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    for (UINT c = first; c <= last; c++) {
        char u[8];
        u[utf8_encode(c, u)] = 0;
        out[c - first] = gfx_text_width(f, u);
    }
    return TRUE;
}

static BOOL WINAPI MoveToEx(HANDLE h, int x, int y, POINT *old)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    if (old)
        *old = (POINT){d->x, d->y};
    d->x = x;
    d->y = y;
    return TRUE;
}

static void line(struct w32_dc *d, struct target *t, int x0, int y0, int x1, int y1)
{
    bool none;
    uint32_t color = pen_color(d, &none);
    if (none)
        return;
    int w = pen_width(d);
    x0 += t->ox;
    y0 += t->oy;
    x1 += t->ox;
    y1 += t->oy;
    if (w <= 1)
        gfx_line(t->c, x0, y0, x1, y1, color);
    else
        gfx_thick_line(t->c, x0, y0, x1, y1, w, color);
    struct rect r = {MIN(x0, x1) - w, MIN(y0, y1) - w, ABS(x1 - x0) + 2 * w + 1, ABS(y1 - y0) + 2 * w + 1};
    damage(d, t, r);
}

static BOOL WINAPI LineTo(HANDLE h, int x, int y)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d)
        return FALSE;
    if (target_of(d, &t))
        line(d, &t, d->x, d->y, x, y);
    d->x = x;
    d->y = y;
    return TRUE;
}

static BOOL WINAPI Polyline(HANDLE h, const POINT *p, int n)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || n < 2)
        return FALSE;
    if (target_of(d, &t))
        for (int i = 1; i < n; i++)
            line(d, &t, p[i - 1].x, p[i - 1].y, p[i].x, p[i].y);
    return TRUE;
}

static BOOL WINAPI PolylineTo(HANDLE h, const POINT *p, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        LineTo(h, p[i].x, p[i].y);
    return TRUE;
}

static void outline_rect(struct w32_dc *d, struct target *t, struct rect r)
{
    bool none;
    uint32_t color = pen_color(d, &none);
    if (none)
        return;
    int w = MIN(pen_width(d), MAX(1, MIN(r.w, r.h) / 2));
    fill(t, (struct rect){r.x, r.y, r.w, w}, color);
    fill(t, (struct rect){r.x, r.y + r.h - w, r.w, w}, color);
    fill(t, (struct rect){r.x, r.y, w, r.h}, color);
    fill(t, (struct rect){r.x + r.w - w, r.y, w, r.h}, color);
}

static BOOL WINAPI Rectangle(HANDLE h, int l, int top, int r, int b)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d)
        return FALSE;
    if (!target_of(d, &t))
        return TRUE;
    struct rect area = {t.ox + MIN(l, r), t.oy + MIN(top, b), ABS(r - l), ABS(b - top)};
    bool none;
    uint32_t color = brush_color(d, d->s.brush, &none);
    if (!none)
        fill(&t, area, color);
    outline_rect(d, &t, area);
    damage(d, &t, area);
    return TRUE;
}

static unsigned isqrt(unsigned v)
{
    unsigned r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int span(int radius_x, int radius_y, int dy)
{
    if (radius_y <= 0 || ABS(dy) > radius_y)
        return -1;
    int64_t num = (int64_t)radius_x * radius_x * ((int64_t)radius_y * radius_y - (int64_t)dy * dy);
    return (int)isqrt((unsigned)(num / ((int64_t)radius_y * radius_y)));
}

static BOOL WINAPI Ellipse(HANDLE h, int l, int top, int r, int b)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d)
        return FALSE;
    if (!target_of(d, &t))
        return TRUE;
    int x0 = t.ox + MIN(l, r), y0 = t.oy + MIN(top, b), w = ABS(r - l), hgt = ABS(b - top);
    int rx = w / 2, ry = hgt / 2, cx = x0 + rx, cy = y0 + ry;
    bool no_brush, no_pen;
    uint32_t bc = brush_color(d, d->s.brush, &no_brush), pc = pen_color(d, &no_pen);
    int pw = no_pen ? 0 : pen_width(d);
    for (int dy = -ry; dy <= ry; dy++) {
        int outer = span(rx, ry, dy), inner = span(rx - pw, ry - pw, dy);
        if (outer < 0)
            continue;
        int y = cy + dy;
        if (inner >= 0) {
            if (!no_brush)
                fill(&t, (struct rect){cx - inner, y, 2 * inner + 1, 1}, bc);
            if (pw) {
                fill(&t, (struct rect){cx - outer, y, outer - inner, 1}, pc);
                fill(&t, (struct rect){cx + inner + 1, y, outer - inner, 1}, pc);
            }
        } else if (pw) {
            fill(&t, (struct rect){cx - outer, y, 2 * outer + 1, 1}, pc);
        } else if (!no_brush) {
            fill(&t, (struct rect){cx - outer, y, 2 * outer + 1, 1}, bc);
        }
    }
    damage(d, &t, (struct rect){x0, y0, w + 1, hgt + 1});
    return TRUE;
}

static BOOL WINAPI RoundRect(HANDLE h, int l, int top, int r, int b, int ew, int eh)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d)
        return FALSE;
    if (!target_of(d, &t))
        return TRUE;
    struct rect area = {t.ox + MIN(l, r), t.oy + MIN(top, b), ABS(r - l), ABS(b - top)};
    int radius = MIN(ew, eh) / 2;
    bool no_brush, no_pen;
    uint32_t bc = brush_color(d, d->s.brush, &no_brush), pc = pen_color(d, &no_pen);
    if (!no_pen)
        gfx_round(t.c, area, radius, pc);
    int pw = no_pen ? 0 : pen_width(d);
    struct rect inner = {area.x + pw, area.y + pw, area.w - 2 * pw, area.h - 2 * pw};
    if (!no_brush && !rect_empty(inner))
        gfx_round(t.c, inner, MAX(radius - pw, 0), bc);
    damage(d, &t, area);
    return TRUE;
}

static BOOL WINAPI Polygon(HANDLE h, const POINT *p, int n)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || n < 2)
        return FALSE;
    if (!target_of(d, &t))
        return TRUE;
    int top = p[0].y, bottom = p[0].y, left = p[0].x, right = p[0].x;
    for (int i = 1; i < n; i++) {
        top = MIN(top, (int)p[i].y);
        bottom = MAX(bottom, (int)p[i].y);
        left = MIN(left, (int)p[i].x);
        right = MAX(right, (int)p[i].x);
    }
    bool none;
    uint32_t color = brush_color(d, d->s.brush, &none);
    if (!none) {
        int xs[64];
        for (int y = top; y <= bottom; y++) {
            int count = 0;
            for (int i = 0, j = n - 1; i < n && count < 64; j = i++) {
                int yi = p[i].y, yj = p[j].y;
                if ((yi <= y && yj > y) || (yj <= y && yi > y))
                    xs[count++] = p[i].x + (int)((int64_t)(y - yi) * (p[j].x - p[i].x) / (yj - yi));
            }
            for (int a = 1; a < count; a++)
                for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
                    int tmp = xs[b];
                    xs[b] = xs[b - 1];
                    xs[b - 1] = tmp;
                }
            for (int k = 0; k + 1 < count; k += 2)
                fill(&t, (struct rect){t.ox + xs[k], t.oy + y, xs[k + 1] - xs[k] + 1, 1}, color);
        }
    }
    for (int i = 0; i < n; i++)
        line(d, &t, p[i].x, p[i].y, p[(i + 1) % n].x, p[(i + 1) % n].y);
    damage(d, &t, (struct rect){t.ox + left, t.oy + top, right - left + 1, bottom - top + 1});
    return TRUE;
}

static BOOL WINAPI Arc(HANDLE h, int l, int t, int r, int b, int x1, int y1, int x2, int y2)
{
    UNUSED(x1);
    UNUSED(y1);
    UNUSED(x2);
    UNUSED(y2);
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    struct gdi_obj *saved = d->s.brush;
    d->s.brush = w32_stock(5);
    Ellipse(h, l, t, r, b);
    d->s.brush = saved;
    return TRUE;
}

static BOOL WINAPI Pie(HANDLE h, int l, int t, int r, int b, int x1, int y1, int x2, int y2)
{
    UNUSED(x1);
    UNUSED(y1);
    UNUSED(x2);
    UNUSED(y2);
    return Ellipse(h, l, t, r, b);
}

static COLORREF WINAPI SetPixel(HANDLE h, int x, int y, COLORREF c)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t))
        return 0xffffffffu;
    x += t.ox;
    y += t.oy;
    if (!rect_contains(t.clip, x, y))
        return 0xffffffffu;
    t.c->px[y * t.c->stride + x] = w32_rgb(c);
    damage(d, &t, (struct rect){x, y, 1, 1});
    return c & 0xffffff;
}

static BOOL WINAPI SetPixelV(HANDLE h, int x, int y, COLORREF c)
{
    return SetPixel(h, x, y, c) != 0xffffffffu;
}

static COLORREF WINAPI GetPixel(HANDLE h, int x, int y)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t))
        return 0xffffffffu;
    x += t.ox;
    y += t.oy;
    if (x < 0 || y < 0 || x >= t.c->w || y >= t.c->h)
        return 0xffffffffu;
    return to_colorref(t.c->px[y * t.c->stride + x]);
}

static char *ansi_to_utf8(const char *s, int n)
{
    if (n < 0)
        n = (int)strlen(s);
    WCHAR *wide = w32_alloc(((size_t)n + 1) * 2);
    if (!wide)
        return NULL;
    size_t wn = w32_cp_to_wide(s, n, wide, (size_t)n, w32_acp());
    char *out = w32_alloc(wn * 3 + 1);
    if (out) {
        size_t k = w32_wide_to_cp(wide, (int)wn, out, wn * 3, 65001);
        out[MIN(k, wn * 3)] = 0;
    }
    w32_free(wide);
    return out;
}

static char *wide_to_utf8(const WCHAR *s, int n)
{
    if (n < 0)
        n = (int)w32_wcslen(s);
    char *out = w32_alloc((size_t)n * 3 + 1);
    if (out) {
        size_t k = w32_wide_to_cp(s, n, out, (size_t)n * 3, 65001);
        out[MIN(k, (size_t)n * 3)] = 0;
    }
    return out;
}

static const struct ui_font *dc_font(struct w32_dc *d)
{
    return d->s.font && d->s.font->font ? d->s.font->font : &font_sans;
}

static void draw_text(struct w32_dc *d, int x, int y, const char *utf8, const RECT *opaque_rect, bool update_cp)
{
    const struct ui_font *f = dc_font(d);
    struct target t;
    int width = gfx_text_width(f, utf8);
    if (update_cp || (d->s.align & TA_UPDATECP)) {
        x = d->x;
        y = d->y;
    }
    if ((d->s.align & TA_CENTER) == TA_CENTER)
        x -= width / 2;
    else if (d->s.align & TA_RIGHT)
        x -= width;
    if ((d->s.align & TA_BASELINE) == TA_BASELINE)
        y -= f->ascent;
    else if (d->s.align & TA_BOTTOM)
        y -= f->height;
    if (d->s.align & TA_UPDATECP)
        d->x += width;
    if (!target_of(d, &t))
        return;
    struct rect box = {t.ox + x, t.oy + y, width, f->height};
    if (opaque_rect)
        fill(&t, (struct rect){t.ox + opaque_rect->left, t.oy + opaque_rect->top,
                               opaque_rect->right - opaque_rect->left, opaque_rect->bottom - opaque_rect->top},
             w32_rgb(d->s.bk));
    else if (d->s.bk_mode == OPAQUE)
        fill(&t, box, w32_rgb(d->s.bk));
    gfx_text(t.c, box.x, box.y, f, utf8, w32_rgb(d->s.text));
    if (d->s.font && d->s.font->underline)
        fill(&t, (struct rect){box.x, box.y + f->ascent + 1, width, 1}, w32_rgb(d->s.text));
    damage(d, &t, opaque_rect ? (struct rect){t.ox + opaque_rect->left, t.oy + opaque_rect->top,
                                              opaque_rect->right - opaque_rect->left,
                                              opaque_rect->bottom - opaque_rect->top}
                              : box);
}

static BOOL WINAPI TextOutA(HANDLE h, int x, int y, LPCSTR s, int n)
{
    struct w32_dc *d = dc_of(h);
    char *u = d ? ansi_to_utf8(s, n) : NULL;
    if (!u)
        return FALSE;
    draw_text(d, x, y, u, NULL, false);
    w32_free(u);
    return TRUE;
}

static BOOL WINAPI TextOutW(HANDLE h, int x, int y, LPCWSTR s, int n)
{
    struct w32_dc *d = dc_of(h);
    char *u = d ? wide_to_utf8(s, n) : NULL;
    if (!u)
        return FALSE;
    draw_text(d, x, y, u, NULL, false);
    w32_free(u);
    return TRUE;
}

static BOOL WINAPI ExtTextOutA(HANDLE h, int x, int y, UINT options, const RECT *r, LPCSTR s, UINT n,
                               const int *dx)
{
    UNUSED(dx);
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    char *u = s ? ansi_to_utf8(s, (int)n) : w32_strdup("");
    if (!u)
        return FALSE;
    draw_text(d, x, y, u, (options & 2) && r ? r : NULL, false);
    w32_free(u);
    return TRUE;
}

static BOOL WINAPI ExtTextOutW(HANDLE h, int x, int y, UINT options, const RECT *r, LPCWSTR s, UINT n,
                               const int *dx)
{
    UNUSED(dx);
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    char *u = s ? wide_to_utf8(s, (int)n) : w32_strdup("");
    if (!u)
        return FALSE;
    draw_text(d, x, y, u, (options & 2) && r ? r : NULL, false);
    w32_free(u);
    return TRUE;
}

static BOOL extent(HANDLE h, const char *u, SIZE *out)
{
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    out->cx = gfx_text_width(f, u);
    out->cy = f->height;
    return TRUE;
}

static BOOL WINAPI GetTextExtentPoint32A(HANDLE h, LPCSTR s, int n, SIZE *out)
{
    char *u = ansi_to_utf8(s, n);
    BOOL ok = u && extent(h, u, out);
    w32_free(u);
    return ok;
}

static BOOL WINAPI GetTextExtentPoint32W(HANDLE h, LPCWSTR s, int n, SIZE *out)
{
    char *u = wide_to_utf8(s, n);
    BOOL ok = u && extent(h, u, out);
    w32_free(u);
    return ok;
}

static void text_metrics(HANDLE h, uint8_t *tm, bool wide)
{
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    LONG v[11] = {f->height,
                  f->ascent,
                  f->height - f->ascent,
                  0,
                  0,
                  gfx_text_width(f, "x"),
                  gfx_text_width(f, "W"),
                  d && d->s.font ? MAX(d->s.font->weight, 400) : 400,
                  0,
                  96,
                  96};
    memcpy(tm, v, sizeof(v));
    uint8_t *p = tm + 44;
    if (wide) {
        WCHAR chars[4] = {32, 0xfffc, 31, 32};
        memcpy(p, chars, 8);
        p += 8;
    } else {
        uint8_t chars[4] = {32, 255, 31, 32};
        memcpy(p, chars, 4);
        p += 4;
    }
    p[0] = d && d->s.font ? d->s.font->italic : 0;
    p[1] = d && d->s.font ? d->s.font->underline : 0;
    p[2] = 0;
    p[3] = f == &font_mono ? 0x30 : 0x26;
    p[4] = 0;
}

static BOOL WINAPI GetTextMetricsA(HANDLE h, uint8_t *tm)
{
    text_metrics(h, tm, false);
    return TRUE;
}

static BOOL WINAPI GetTextMetricsW(HANDLE h, uint8_t *tm)
{
    text_metrics(h, tm, true);
    return TRUE;
}

static int WINAPI GetTextFaceA(HANDLE h, int cap, LPSTR out)
{
    struct w32_dc *d = dc_of(h);
    const char *face = d && d->s.font && d->s.font->face[0] ? d->s.font->face : "Inter";
    if (out && cap > 0)
        str_copy(out, face, (size_t)cap);
    return (int)strlen(face) + 1;
}

enum {
    DT_CENTER = 0x1,
    DT_RIGHT = 0x2,
    DT_VCENTER = 0x4,
    DT_BOTTOM = 0x8,
    DT_WORDBREAK = 0x10,
    DT_SINGLELINE = 0x20,
    DT_CALCRECT = 0x400,
    DT_NOPREFIX = 0x800,
};

static int layout_lines(const struct ui_font *f, const char *s, int width, UINT format, int *starts, int *lens,
                        int max)
{
    int count = 0;
    const char *p = s;
    while (*p && count < max) {
        const char *line_end = p;
        while (*line_end && *line_end != '\n' && *line_end != '\r')
            line_end++;
        if (format & DT_SINGLELINE)
            line_end = p + strlen(p);
        const char *end = line_end;
        if ((format & DT_WORDBREAK) && width > 0) {
            while (end > p && gfx_text_width_n(f, p, (size_t)(end - p)) > width) {
                const char *cut = end - 1;
                while (cut > p && *cut != ' ')
                    cut--;
                if (cut == p) {
                    end--;
                    continue;
                }
                end = cut;
            }
            if (end == p)
                end = line_end > p ? p + 1 : p;
        }
        starts[count] = (int)(p - s);
        lens[count] = (int)(end - p);
        count++;
        p = end;
        while (*p == ' ' && end != line_end)
            p++;
        if (p == line_end) {
            if (*p == '\r')
                p++;
            if (*p == '\n')
                p++;
            if (!*p)
                break;
        }
        if (format & DT_SINGLELINE)
            break;
    }
    return count;
}

static void strip_prefix(char *s)
{
    char *out = s;
    for (char *p = s; *p; p++) {
        if (*p == '&') {
            if (p[1] == '&')
                *out++ = *++p;
            continue;
        }
        *out++ = *p;
    }
    *out = 0;
}

static int draw_text_box(HANDLE h, char *u, RECT *r, UINT format)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0;
    if (!(format & DT_NOPREFIX))
        strip_prefix(u);
    const struct ui_font *f = dc_font(d);
    int width = r->right - r->left;
    static int starts[128], lens[128];
    int lines = layout_lines(f, u, width, format, starts, lens, 128);
    int total = lines * f->height;
    if (format & DT_CALCRECT) {
        int widest = 0;
        for (int i = 0; i < lines; i++)
            widest = MAX(widest, gfx_text_width_n(f, u + starts[i], (size_t)lens[i]));
        r->right = r->left + widest;
        r->bottom = r->top + total;
        return total;
    }
    int y = r->top;
    if (format & DT_SINGLELINE) {
        if (format & DT_VCENTER)
            y = r->top + (r->bottom - r->top - f->height) / 2;
        else if (format & DT_BOTTOM)
            y = r->bottom - f->height;
    }
    struct target t;
    if (!target_of(d, &t))
        return total;
    struct rect box = {t.ox + r->left, t.oy + r->top, width, r->bottom - r->top};
    t.c->clip = rect_intersect(t.clip, box);
    for (int i = 0; i < lines; i++, y += f->height) {
        int w = gfx_text_width_n(f, u + starts[i], (size_t)lens[i]);
        int x = format & DT_CENTER ? r->left + (width - w) / 2 : format & DT_RIGHT ? r->right - w : r->left;
        if (d->s.bk_mode == OPAQUE)
            gfx_clear(t.c, rect_intersect((struct rect){t.ox + x, t.oy + y, w, f->height}, t.c->clip),
                      w32_rgb(d->s.bk));
        gfx_text_n(t.c, t.ox + x, t.oy + y, f, u + starts[i], (size_t)lens[i], w32_rgb(d->s.text));
    }
    t.c->clip = t.clip;
    damage(d, &t, box);
    return total;
}

static int WINAPI DrawTextA(HANDLE h, LPCSTR s, int n, RECT *r, UINT format)
{
    char *u = ansi_to_utf8(s, n);
    int result = u ? draw_text_box(h, u, r, format) : 0;
    w32_free(u);
    return result;
}

static int WINAPI DrawTextW(HANDLE h, LPCWSTR s, int n, RECT *r, UINT format)
{
    char *u = wide_to_utf8(s, n);
    int result = u ? draw_text_box(h, u, r, format) : 0;
    w32_free(u);
    return result;
}

static int WINAPI DrawTextExW(HANDLE h, LPWSTR s, int n, RECT *r, UINT format, void *params)
{
    UNUSED(params);
    return DrawTextW(h, s, n, r, format);
}

static BOOL WINAPI DrawFocusRect(HANDLE h, const RECT *r)
{
    UNUSED(h);
    UNUSED(r);
    return TRUE;
}

static BOOL WINAPI DrawEdge(HANDLE h, RECT *r, UINT edge, UINT flags)
{
    UNUSED(edge);
    UNUSED(flags);
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t))
        return FALSE;
    struct rect area = {t.ox + r->left, t.oy + r->top, r->right - r->left, r->bottom - r->top};
    gfx_frame(t.c, area, style.control_border);
    damage(d, &t, area);
    return TRUE;
}

static HANDLE WINAPI CreateCompatibleDC(HANDLE h)
{
    UNUSED(h);
    struct w32_dc *d = w32_zalloc(sizeof(*d));
    if (!d)
        return NULL;
    dc_defaults(d);
    d->memory = true;
    return d;
}

static HANDLE WINAPI CreateDCW(const void *driver, const void *device, const void *output, const void *mode)
{
    UNUSED(driver);
    UNUSED(device);
    UNUSED(output);
    UNUSED(mode);
    return CreateCompatibleDC(NULL);
}

static BOOL WINAPI DeleteDC(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    d->magic = 0;
    w32_free(d);
    return TRUE;
}

static struct gdi_obj *new_bitmap(int w, int h, void *bits, int bpp)
{
    struct gdi_obj *o = new_obj(OBJ_BITMAP);
    if (!o)
        return NULL;
    w = MAX(w, 1);
    int height = h < 0 ? -h : MAX(h, 1);
    o->bpp = bpp;
    size_t stride = (size_t)(((w * bpp + 31) / 32) * 4);
    if (!bits) {
        bits = w32_zalloc(stride * (size_t)height);
        if (!bits) {
            w32_free(o);
            return NULL;
        }
    }
    o->bits = bits;
    if (bpp == 32) {
        if (h > 0)
            o->canvas = (struct canvas){(uint32_t *)bits + (size_t)(height - 1) * (size_t)w, w, height, -w,
                                        {0, 0, w, height}};
        else
            o->canvas = canvas_make(bits, w, height);
    } else {
        o->canvas = (struct canvas){NULL, w, height, 0, {0, 0, w, height}};
    }
    o->height = h;
    return o;
}

static HANDLE WINAPI CreateCompatibleBitmap(HANDLE h, int w, int height)
{
    UNUSED(h);
    return new_bitmap(w, -height, NULL, 32);
}

static HANDLE WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits)
{
    UNUSED(planes);
    struct gdi_obj *o = new_bitmap(w, -h, NULL, 32);
    if (o && bits && bpp == 32)
        memcpy(o->bits, bits, (size_t)w * (size_t)h * 4);
    return o;
}

struct bitmap_info {
    DWORD size;
    LONG width, height;
    WORD planes, bpp;
    DWORD compression, image_size;
    LONG xppm, yppm;
    DWORD used, important;
};

static HANDLE WINAPI CreateDIBSection(HANDLE h, const struct bitmap_info *bmi, UINT usage, void **bits,
                                      HANDLE section, DWORD offset)
{
    UNUSED(h);
    UNUSED(usage);
    UNUSED(section);
    UNUSED(offset);
    if (!bmi || bmi->width <= 0 || !bmi->height)
        return NULL;
    struct gdi_obj *o = new_bitmap(bmi->width, bmi->height, NULL, bmi->bpp);
    if (o && bits)
        *bits = o->bits;
    return o;
}

static uint32_t dib_pixel(const struct bitmap_info *bmi, const uint8_t *bits, int x, int y)
{
    int w = bmi->width, h = bmi->height < 0 ? -bmi->height : bmi->height;
    int row = bmi->height < 0 ? y : h - 1 - y;
    int stride = ((w * bmi->bpp + 31) / 32) * 4;
    const uint8_t *p = bits + (size_t)row * (size_t)stride;
    const uint32_t *palette = (const uint32_t *)((const uint8_t *)bmi + bmi->size);
    switch (bmi->bpp) {
    case 32: return ((const uint32_t *)p)[x] | 0xff000000u;
    case 24: return RGB(p[x * 3 + 2], p[x * 3 + 1], p[x * 3]);
    case 16: {
        uint16_t v = (uint16_t)(p[x * 2] | p[x * 2 + 1] << 8);
        if (bmi->compression == 3)
            return RGB((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3);
        return RGB(((v >> 10) & 31) << 3, ((v >> 5) & 31) << 3, (v & 31) << 3);
    }
    case 8: return palette[p[x]] | 0xff000000u;
    case 4: return palette[(p[x / 2] >> (x & 1 ? 0 : 4)) & 15] | 0xff000000u;
    case 1: return palette[(p[x / 8] >> (7 - x % 8)) & 1] | 0xff000000u;
    default: return 0xff000000u;
    }
}

static void blit_dib(struct target *t, int dx, int dy, int dw, int dh, const struct bitmap_info *bmi,
                     const void *bits, int sx, int sy, int sw, int sh)
{
    int h = bmi->height < 0 ? -bmi->height : bmi->height;
    if (!dw || !dh || !sw || !sh)
        return;
    for (int y = 0; y < ABS(dh); y++) {
        int ty = dy + (dh < 0 ? -y : y);
        if (ty < t->clip.y || ty >= t->clip.y + t->clip.h)
            continue;
        int src_y = sy + (int)((int64_t)y * sh / dh);
        if (src_y < 0 || src_y >= h)
            continue;
        for (int x = 0; x < ABS(dw); x++) {
            int tx = dx + (dw < 0 ? -x : x);
            if (tx < t->clip.x || tx >= t->clip.x + t->clip.w)
                continue;
            int src_x = sx + (int)((int64_t)x * sw / dw);
            if (src_x < 0 || src_x >= bmi->width)
                continue;
            t->c->px[ty * t->c->stride + tx] = dib_pixel(bmi, bits, src_x, src_y);
        }
    }
}

static int WINAPI StretchDIBits(HANDLE h, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh,
                                const void *bits, const struct bitmap_info *bmi, UINT usage, DWORD rop)
{
    UNUSED(usage);
    UNUSED(rop);
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !bmi || !bits)
        return 0;
    if (!target_of(d, &t))
        return ABS(sh);
    int height = bmi->height < 0 ? -bmi->height : bmi->height;
    if (bmi->height > 0)
        sy = height - sy - sh;
    blit_dib(&t, t.ox + dx, t.oy + dy, dw, dh, bmi, bits, sx, sy, sw, sh);
    damage(d, &t, (struct rect){t.ox + MIN(dx, dx + dw), t.oy + MIN(dy, dy + dh), ABS(dw) + 1, ABS(dh) + 1});
    return ABS(sh);
}

static int WINAPI SetDIBitsToDevice(HANDLE h, int dx, int dy, DWORD w, DWORD hgt, int sx, int sy, UINT start,
                                    UINT lines, const void *bits, const struct bitmap_info *bmi, UINT usage)
{
    UNUSED(start);
    UNUSED(lines);
    int height = bmi->height < 0 ? -bmi->height : bmi->height;
    int top = bmi->height > 0 ? height - sy - (int)hgt : sy;
    return StretchDIBits(h, dx, dy, (int)w, (int)hgt, sx, bmi->height > 0 ? height - top - (int)hgt : top, (int)w,
                         (int)hgt, bits, bmi, usage, SRCCOPY);
}

static int WINAPI GetDIBits(HANDLE h, HANDLE bitmap, UINT start, UINT lines, void *bits, struct bitmap_info *bmi,
                            UINT usage)
{
    UNUSED(h);
    UNUSED(usage);
    struct gdi_obj *b = obj(bitmap);
    if (!b || b->kind != OBJ_BITMAP || !bmi)
        return 0;
    if (!bits) {
        bmi->width = b->canvas.w;
        bmi->height = b->canvas.h;
        bmi->planes = 1;
        bmi->bpp = 32;
        bmi->compression = 0;
        bmi->image_size = (DWORD)(b->canvas.w * b->canvas.h * 4);
        return 1;
    }
    if (bmi->bpp != 32 || !b->canvas.px)
        return 0;
    int w = b->canvas.w;
    UINT n = 0;
    for (UINT i = start; i < start + lines && (int)i < b->canvas.h; i++, n++) {
        int row = bmi->height > 0 ? b->canvas.h - 1 - (int)i : (int)i;
        memcpy((uint32_t *)bits + (size_t)(i - start) * (size_t)w, b->canvas.px + row * b->canvas.stride,
               (size_t)w * 4);
    }
    return (int)n;
}

static uint32_t raster(uint32_t src, uint32_t dst, uint32_t pat, DWORD rop)
{
    switch (rop) {
    case SRCCOPY: return src;
    case SRCPAINT: return src | dst;
    case SRCAND: return src & dst;
    case SRCINVERT: return src ^ dst;
    case NOTSRCCOPY: return ~src;
    case PATCOPY: return pat;
    case PATINVERT: return pat ^ dst;
    case DSTINVERT: return ~dst;
    case BLACKNESS: return 0;
    case WHITENESS: return 0xffffff;
    default: return src;
    }
}

#define ROP_ALPHA 0xffffffffu

static uint32_t blend_func;

static uint32_t alpha_blend(uint32_t src, uint32_t dst)
{
    unsigned k = (blend_func >> 16) & 0xff, a = blend_func & 0x01000000u ? (src >> 24) * k / 255 : k;
    unsigned sk = blend_func & 0x01000000u ? k : a;
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        unsigned sc = (src >> sh) & 0xff, dc = (dst >> sh) & 0xff;
        unsigned v = sc * sk / 255 + dc * (255 - a) / 255;
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}

static BOOL stretch(HANDLE hd, int dx, int dy, int dw, int dh, HANDLE hs, int sx, int sy, int sw, int sh, DWORD rop)
{
    struct w32_dc *d = dc_of(hd), *s = dc_of(hs);
    struct target dt, st;
    if (!d || !target_of(d, &dt))
        return d != NULL;
    bool uses_src = rop != PATCOPY && rop != PATINVERT && rop != DSTINVERT && rop != BLACKNESS && rop != WHITENESS;
    struct gdi_obj *sb = s && s->memory ? s->s.bitmap : NULL;
    if (uses_src && (!s || (!target_of(s, &st) && !(sb && sb->bits))))
        return FALSE;
    bool none;
    uint32_t pat = brush_color(d, d->s.brush, &none) & 0xffffff;
    if (!dw || !dh)
        return TRUE;
    struct bitmap_info bmi = {40, sb ? sb->canvas.w : 0, sb ? sb->height : 0, 1, (WORD)(sb ? sb->bpp : 32), 0, 0, 0, 0, 0, 0};
    for (int y = 0; y < ABS(dh); y++) {
        int ty = dt.oy + dy + (dh < 0 ? -y : y);
        if (ty < dt.clip.y || ty >= dt.clip.y + dt.clip.h)
            continue;
        int yy = sy + (int)((int64_t)y * sh / dh);
        for (int x = 0; x < ABS(dw); x++) {
            int tx = dt.ox + dx + (dw < 0 ? -x : x);
            if (tx < dt.clip.x || tx >= dt.clip.x + dt.clip.w)
                continue;
            uint32_t *out = dt.c->px + ty * dt.c->stride + tx;
            uint32_t src = 0;
            if (uses_src) {
                int xx = sx + (int)((int64_t)x * sw / dw);
                if (sb && sb->bpp != 32) {
                    if (xx < 0 || yy < 0 || xx >= sb->canvas.w || yy >= sb->canvas.h)
                        continue;
                    src = dib_pixel(&bmi, sb->bits, xx, yy);
                } else {
                    int px = st.ox + xx, py = st.oy + yy;
                    if (px < 0 || py < 0 || px >= st.c->w || py >= st.c->h)
                        continue;
                    src = st.c->px[py * st.c->stride + px];
                }
            }
            uint32_t v = rop == ROP_ALPHA ? alpha_blend(src, *out) : raster(src & 0xffffff, *out & 0xffffff, pat, rop);
            *out = (v & 0xffffff) | 0xff000000u;
        }
    }
    dt.c->clip = dt.clip;
    damage(d, &dt, (struct rect){dt.ox + MIN(dx, dx + dw), dt.oy + MIN(dy, dy + dh), ABS(dw) + 1, ABS(dh) + 1});
    return TRUE;
}

static BOOL WINAPI GdiAlphaBlend(HANDLE hd, int dx, int dy, int dw, int dh, HANDLE hs, int sx, int sy, int sw, int sh,
                                 uint32_t func)
{
    blend_func = func;
    return stretch(hd, dx, dy, dw, dh, hs, sx, sy, sw, sh, ROP_ALPHA);
}

static int WINAPI SetDIBits(HANDLE h, HANDLE bitmap, UINT start, UINT lines, const void *bits,
                            const struct bitmap_info *bmi, UINT usage)
{
    UNUSED(h);
    UNUSED(usage);
    struct gdi_obj *b = obj(bitmap);
    if (!b || b->kind != OBJ_BITMAP || !bmi || !bits || !b->canvas.px)
        return 0;
    int height = bmi->height < 0 ? -bmi->height : bmi->height, n = 0;
    for (UINT i = start; i < start + lines && (int)i < height && (int)i < b->canvas.h; i++, n++) {
        int row = bmi->height > 0 ? b->canvas.h - 1 - (int)i : (int)i;
        for (int x = 0; x < b->canvas.w && x < bmi->width; x++)
            b->canvas.px[row * b->canvas.stride + x] = dib_pixel(bmi, bits, x, bmi->height > 0 ? height - 1 - (int)i : (int)i);
    }
    return n;
}

static BOOL WINAPI BitBlt(HANDLE hd, int dx, int dy, int w, int h, HANDLE hs, int sx, int sy, DWORD rop)
{
    return stretch(hd, dx, dy, w, h, hs, sx, sy, w, h, rop);
}

static BOOL WINAPI StretchBlt(HANDLE hd, int dx, int dy, int dw, int dh, HANDLE hs, int sx, int sy, int sw, int sh,
                              DWORD rop)
{
    return stretch(hd, dx, dy, dw, dh, hs, sx, sy, sw, sh, rop);
}

static BOOL WINAPI PatBlt(HANDLE h, int x, int y, int w, int hgt, DWORD rop)
{
    return stretch(h, x, y, w, hgt, NULL, 0, 0, w, hgt, rop);
}

static BOOL WINAPI TransparentBlt(HANDLE hd, int dx, int dy, int dw, int dh, HANDLE hs, int sx, int sy, int sw,
                                  int sh, UINT key)
{
    struct w32_dc *d = dc_of(hd), *s = dc_of(hs);
    struct target dt, st;
    if (!d || !s || !target_of(d, &dt) || !target_of(s, &st) || !dw || !dh)
        return FALSE;
    uint32_t transparent = w32_rgb(key) & 0xffffff;
    for (int y = 0; y < dh; y++) {
        int ty = dt.oy + dy + y, py = st.oy + sy + (int)((int64_t)y * sh / dh);
        if (ty < dt.clip.y || ty >= dt.clip.y + dt.clip.h || py < 0 || py >= st.c->h)
            continue;
        for (int x = 0; x < dw; x++) {
            int tx = dt.ox + dx + x, px = st.ox + sx + (int)((int64_t)x * sw / dw);
            if (tx < dt.clip.x || tx >= dt.clip.x + dt.clip.w || px < 0 || px >= st.c->w)
                continue;
            uint32_t src = st.c->px[py * st.c->stride + px] & 0xffffff;
            if (src != transparent)
                dt.c->px[ty * dt.c->stride + tx] = src | 0xff000000u;
        }
    }
    damage(d, &dt, (struct rect){dt.ox + dx, dt.oy + dy, dw, dh});
    return TRUE;
}

static int WINAPI GetDeviceCaps(HANDLE h, int index)
{
    UNUSED(h);
    int sw = 1024, sh = 768;
    if (gui_active()) {
        struct rect s = wm_screen_rect(0);
        sw = s.w;
        sh = s.h;
    }
    switch (index) {
    case 2: return 1;
    case 4: return sw * 254 / 960;
    case 6: return sh * 254 / 960;
    case 8: return sw;
    case 10: return sh;
    case 12: return 32;
    case 14: return 1;
    case 24: return -1;
    case 38: return 0x7e99;
    case 88:
    case 90: return 96;
    case 116: return 60;
    default: return 0;
    }
}

static BOOL WINAPI GdiFlush(void)
{
    return TRUE;
}

static int WINAPI EnumFontFamiliesExA(HANDLE h, void *lf, void *fn, LPARAM lp, DWORD flags)
{
    UNUSED(h);
    UNUSED(lf);
    UNUSED(fn);
    UNUSED(lp);
    UNUSED(flags);
    return 1;
}

void w32_gdi_reset(void)
{
    memset(stock, 0, sizeof(stock));
    memset(sys_brushes, 0, sizeof(sys_brushes));
    memset(&default_bitmap_obj, 0, sizeof(default_bitmap_obj));
    default_bitmap_obj.magic = W32_GDI_MAGIC;
    default_bitmap_obj.kind = OBJ_BITMAP;
    default_bitmap_obj.stock = true;
}

static const struct w32_export gdi_exports[] = {
    W32_FN(GetStockObject), W32_FN(CreateSolidBrush), W32_FN(CreateHatchBrush), W32_FN(CreateBrushIndirect),
    W32_FN(CreatePen), W32_FN(CreatePenIndirect), W32_FN(ExtCreatePen), W32_FN(CreateFontIndirectA),
    W32_FN(CreateFontIndirectW), W32_FN(CreateFontA), W32_FN(CreateFontW), W32_FN(SelectObject), W32_FN(DeleteObject),
    W32_FN(GetObjectType), W32_FN(GetObjectA), W32_FN(GetObjectW), W32_FN(SetTextColor),
    W32_FN(GetTextColor), W32_FN(SetBkColor), W32_FN(GetBkColor), W32_FN(SetBkMode), W32_FN(GetBkMode),
    W32_FN(SetDCBrushColor), W32_FN(SetDCPenColor), W32_FN(SetROP2), W32_FN(SetTextAlign), W32_FN(SetMapMode),
    W32_FN(SetStretchBltMode), W32_FN(SetGraphicsMode), W32_FN(SetViewportOrgEx), W32_FN(SetWindowOrgEx),
    W32_FN(OffsetViewportOrgEx), W32_FN(SaveDC), W32_FN(RestoreDC), W32_FN(CreateRectRgn),
    W32_FN(CreateRectRgnIndirect), W32_FN(SelectClipRgn), W32_FN(SetRectRgn), W32_FN(CombineRgn), W32_FN(GetRgnBox),
    W32_FN(EqualRgn), W32_FN(GetRegionData), W32_FN(GetClipRgn), W32_FN(GetCurrentObject), W32_FN(GetWorldTransform),
    W32_AS("SetWorldTransform", w32_one2), W32_AS("ModifyWorldTransform", w32_one3), W32_FN(SetBrushOrgEx),
    W32_FN(GetCharWidthW), W32_AS("GetCharWidth32W", GetCharWidthW), W32_FN(GdiAlphaBlend), W32_FN(SetDIBits),
    W32_AS("SetPolyFillMode", w32_one2), W32_AS("SetArcDirection", w32_one2), W32_AS("SetMiterLimit", w32_one3),
    W32_FN(GetFontData), W32_AS("GetOutlineTextMetricsW", w32_zero3), W32_AS("BeginPath", w32_zero1),
    W32_AS("EndPath", w32_zero1), W32_AS("CloseFigure", w32_zero1), W32_AS("FillPath", w32_zero1),
    W32_AS("StrokePath", w32_zero1), W32_AS("StrokeAndFillPath", w32_zero1), W32_AS("WidenPath", w32_zero1),
    W32_AS("SelectClipPath", w32_zero2), W32_AS("PolyBezierTo", w32_zero3), W32_AS("StartDocW", w32_zero2),
    W32_AS("StartPage", w32_zero1), W32_AS("EndPage", w32_zero1), W32_AS("EndDoc", w32_zero1),
    W32_AS("SetAbortProc", w32_zero2), W32_AS("CancelDC", w32_one1), W32_AS("CreateEnhMetaFileW", w32_zero4),
    W32_AS("CloseEnhMetaFile", w32_zero1), W32_AS("DeleteEnhMetaFile", w32_zero1), W32_AS("EnumEnhMetaFile", w32_zero5),
    W32_AS("GetEnhMetaFileBits", w32_zero3), W32_AS("GetEnhMetaFileHeader", w32_zero3), W32_AS("PlayEnhMetaFile", w32_zero3),
    W32_AS("PlayEnhMetaFileRecord", w32_zero4), W32_AS("SetEnhMetaFileBits", w32_zero2), W32_AS("GdiComment", w32_zero3),
    W32_AS("ChoosePixelFormat", w32_zero2), W32_AS("DescribePixelFormat", w32_zero4), W32_AS("GetPixelFormat", w32_zero1),
    W32_AS("SetPixelFormat", w32_zero3), W32_AS("SwapBuffers", w32_zero1), W32_AS("ExtEscape", w32_zero6),
    W32_AS("GetICMProfileW", w32_zero3), W32_FN(IntersectClipRect), W32_FN(GetClipBox),
    W32_FN(MoveToEx), W32_FN(LineTo), W32_FN(Polyline), W32_FN(PolylineTo), W32_FN(Rectangle), W32_FN(Ellipse),
    W32_FN(RoundRect), W32_FN(Polygon), W32_FN(Arc), W32_FN(Pie), W32_AS("Chord", Pie), W32_FN(SetPixel),
    W32_FN(SetPixelV), W32_FN(GetPixel), W32_FN(TextOutA), W32_FN(TextOutW), W32_FN(ExtTextOutA), W32_FN(ExtTextOutW),
    W32_FN(GetTextExtentPoint32A), W32_FN(GetTextExtentPoint32W), W32_AS("GetTextExtentPointA", GetTextExtentPoint32A),
    W32_AS("GetTextExtentPointW", GetTextExtentPoint32W), W32_FN(GetTextMetricsA), W32_FN(GetTextMetricsW),
    W32_FN(GetTextFaceA), W32_FN(CreateCompatibleDC), W32_FN(DeleteDC), W32_FN(CreateDCW), W32_AS("CreateDCA", CreateDCW),
    W32_AS("CreateICW", CreateDCW), W32_AS("CreateICA", CreateDCW), W32_FN(CreateCompatibleBitmap),
    W32_FN(CreateBitmap), W32_FN(CreateDIBSection), W32_FN(StretchDIBits), W32_FN(SetDIBitsToDevice),
    W32_FN(GetDIBits), W32_FN(BitBlt), W32_FN(StretchBlt), W32_FN(PatBlt), W32_FN(GetDeviceCaps), W32_FN(GdiFlush),
    W32_FN(EnumFontFamiliesExA), W32_AS("EnumFontFamiliesExW", EnumFontFamiliesExA),
};

static const struct w32_export gdi_user_exports[] = {
    W32_FN(FillRect),    W32_FN(FrameRect), W32_FN(InvertRect),  W32_FN(GetSysColor),   W32_FN(GetSysColorBrush),
    W32_FN(DrawTextA),   W32_FN(DrawTextW), W32_FN(DrawTextExW), W32_FN(DrawFocusRect), W32_FN(DrawEdge),
};

static const struct w32_export msimg_exports[] = {
    W32_FN(TransparentBlt),
    W32_AS("AlphaBlend", GdiAlphaBlend),
};

W32_EXPORTS(gdi_module, "gdi32.dll", gdi_exports, .attach = w32_gdi_reset);
W32_EXPORTS(gdi_user_module, "user32.dll", gdi_user_exports);
W32_EXPORTS(msimg_module, "msimg32.dll", msimg_exports);
#endif
