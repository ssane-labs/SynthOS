#include <synth/config.h>
#if CONFIG_GUI
#include <synth/codepage.h>
#include <synth/string.h>

#include "w32gui.h"

enum {
    OBJ_PEN = 1,
    OBJ_BRUSH = 2,
    OBJ_DC = 3,
    OBJ_PAL = 5,
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
    bool mapped;
    struct rect region;
    uint32_t *colors;
    int ncolors;
};

struct dc_state {
    struct gdi_obj *pen, *brush, *font, *bitmap, *palette;
    COLORREF text, bk, dc_brush, dc_pen;
    int bk_mode, rop2, org_x, org_y, brush_x, brush_y;
    UINT align;
    struct rect clip;
    bool clipped;
};

struct w32_dc {
    uint32_t magic;
    struct w32_window *win;
    bool memory, live;
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

static const uint32_t system_colors[20] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xc0c0c0, 0xc0dcc0, 0xf0caa6,
    0xf0fbff, 0xa4a0a0, 0x808080, 0x0000ff, 0x00ff00, 0x00ffff, 0xff0000, 0xff00ff, 0xffff00, 0xffffff,
};

static struct gdi_obj *palette_new(int n)
{
    struct gdi_obj *o = new_obj(OBJ_PAL);
    if (o && !(o->colors = w32_zalloc(sizeof(uint32_t) * (size_t)MAX(n, 1)))) {
        w32_free(o);
        return NULL;
    }
    if (o)
        o->ncolors = n;
    return o;
}

static struct gdi_obj *default_palette(void)
{
    struct gdi_obj *o = palette_new(20);
    if (o) {
        o->stock = true;
        memcpy(o->colors, system_colors, sizeof(system_colors));
    }
    return o;
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
        case 15: stock[index] = default_palette(); break;
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
    d->s.palette = w32_stock(15);
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

/* GetDC: the window's whole visible area at the time of each drawing call, as on Windows, so a DC kept from when
   the window was small still reaches all of it after a resize */
HANDLE w32_window_dc_live(struct w32_window *w)
{
    struct w32_dc *d = w32_window_dc(w, w32_visible_area(w));
    if (d)
        d->live = true;
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
        if (d->live) {
            struct rect v = w32_visible_area(d->win);
            d->visible = (struct rect){v.x - a.x, v.y - a.y, v.w, v.h};
        }
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

/* a pattern brush paints with the pattern's first pixel */
static HANDLE WINAPI CreatePatternBrush(HANDLE bitmap)
{
    struct gdi_obj *b = obj(bitmap);
    if (!b || b->kind != OBJ_BITMAP)
        return NULL;
    uint32_t px = b->bits && b->canvas.w > 0 ? ((const uint32_t *)b->bits)[0] : 0;
    return CreateSolidBrush((px >> 16 & 0xff) | (px & 0xff00) | (px & 0xff) << 16);
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
    if (o->kind == OBJ_BITMAP && o->bits && o->mapped)
        w32_unmap_section(o->bits);
    else if (o->kind == OBJ_BITMAP && o->bits)
        w32_free(o->bits);
    w32_free(o->colors);
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
    if (o->kind == OBJ_PAL) {
        WORD n = (WORD)o->ncolors;
        if (out && size >= 2)
            memcpy(out, &n, 2);
        return 2;
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

static int WINAPI GetMapMode(HANDLE h)
{
    return dc_of(h) ? 1 : 0;
}

static BOOL WINAPI LPtoDP(HANDLE h, POINT *p, int n)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !p)
        return FALSE;
    for (int i = 0; i < n; i++)
        p[i] = (POINT){p[i].x - d->s.org_x, p[i].y - d->s.org_y};
    return TRUE;
}

static BOOL WINAPI DPtoLP(HANDLE h, POINT *p, int n)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !p)
        return FALSE;
    for (int i = 0; i < n; i++)
        p[i] = (POINT){p[i].x + d->s.org_x, p[i].y + d->s.org_y};
    return TRUE;
}

static COLORREF WINAPI GetNearestColor(HANDLE h, COLORREF color)
{
    return dc_of(h) ? color & 0xffffff : 0xffffffffu;
}

static int WINAPI SetStretchBltMode(HANDLE h, int mode)
{
    UNUSED(h);
    UNUSED(mode);
    return 3;
}

static int WINAPI GetStretchBltMode(HANDLE h)
{
    return dc_of(h) ? 3 : 0;
}

/* only MM_TEXT: the extents stay one to one */
static BOOL WINAPI SetWindowExtEx(HANDLE h, int x, int y, SIZE *old)
{
    UNUSED(x);
    UNUSED(y);
    if (old)
        *old = (SIZE){1, 1};
    return dc_of(h) != NULL;
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

static HANDLE WINAPI CreateRoundRectRgn(int l, int t, int r, int b, int w, int h)
{
    UNUSED(w);
    UNUSED(h);
    return CreateRectRgn(l, t, r, b);
}

static int WINAPI OffsetRgn(HANDLE rgn, int x, int y)
{
    struct gdi_obj *o = obj(rgn);
    if (!o || o->kind != OBJ_REGION)
        return 0;
    o->region.x += x;
    o->region.y += y;
    return region_kind(o->region);
}

static int WINAPI OffsetClipRgn(HANDLE h, int x, int y)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return 0;
    if (!d->s.clipped)
        return 2;
    d->s.clip.x += x;
    d->s.clip.y += y;
    return region_kind(d->s.clip);
}

static BOOL WINAPI FillRgn(HANDLE dc, HANDLE rgn, HANDLE brush)
{
    struct gdi_obj *o = obj(rgn);
    if (!o || o->kind != OBJ_REGION)
        return FALSE;
    struct rect g = o->region;
    RECT r = {g.x, g.y, g.x + g.w, g.y + g.h};
    return FillRect(dc, &r, brush);
}

static BOOL WINAPI FrameRgn(HANDLE dc, HANDLE rgn, HANDLE brush, int w, int h)
{
    UNUSED(w);
    UNUSED(h);
    struct gdi_obj *o = obj(rgn);
    if (!o || o->kind != OBJ_REGION)
        return FALSE;
    struct rect g = o->region;
    RECT r = {g.x, g.y, g.x + g.w, g.y + g.h};
    return FrameRect(dc, &r, brush);
}

static BOOL WINAPI OffsetWindowOrgEx(HANDLE h, int x, int y, POINT *old)
{
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    if (old)
        *old = (POINT){d->s.org_x, d->s.org_y};
    d->s.org_x += x;
    d->s.org_y += y;
    return TRUE;
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
    struct w32_dc *d = dc_of(h);
    if (!d)
        return FALSE;
    if (old)
        *old = (POINT){d->s.brush_x, d->s.brush_y};
    d->s.brush_x = x;
    d->s.brush_y = y;
    return TRUE;
}

static BOOL WINAPI GetBrushOrgEx(HANDLE h, POINT *out)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !out)
        return FALSE;
    *out = (POINT){d->s.brush_x, d->s.brush_y};
    return TRUE;
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

static BOOL WINAPI PolyPolyline(HANDLE h, const POINT *p, const DWORD *counts, DWORD n)
{
    if (!p || !counts)
        return FALSE;
    for (DWORD i = 0; i < n; p += counts[i++])
        if (counts[i] >= 2)
            Polyline(h, p, (int)counts[i]);
    return TRUE;
}

/* cubic Bezier segments flattened into 16 lines each (fixed point, no floating point here) */
static BOOL WINAPI PolyBezier(HANDLE h, const POINT *p, DWORD n)
{
    if (!p || n < 4 || (n - 1) % 3)
        return FALSE;
    DWORD segments = (n - 1) / 3, count = segments * 16 + 1;
    POINT *out = w32_alloc(count * sizeof(POINT));
    if (!out)
        return FALSE;
    out[0] = p[0];
    for (DWORD s = 0; s < segments; s++) {
        const POINT *c = p + s * 3;
        for (int i = 1; i <= 16; i++) {
            int64_t t = i, u = 16 - i;
            int64_t x = u * u * u * c[0].x + 3 * u * u * t * c[1].x + 3 * u * t * t * c[2].x + t * t * t * c[3].x;
            int64_t y = u * u * u * c[0].y + 3 * u * u * t * c[1].y + 3 * u * t * t * c[2].y + t * t * t * c[3].y;
            out[s * 16 + (DWORD)i] = (POINT){(int)((x + 2048) / 4096), (int)((y + 2048) / 4096)};
        }
    }
    BOOL ok = Polyline(h, out, (int)count);
    w32_free(out);
    return ok;
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

static int char_width(const struct ui_font *f, uint32_t c)
{
    char u[8];
    u[utf8_encode(c, u)] = 0;
    return gfx_text_width(f, u);
}

static uint32_t ansi_char(UINT c)
{
    char a = (char)c;
    char *u = ansi_to_utf8(&a, 1);
    uint32_t r = c;
    if (u) {
        const char *q = u;
        r = utf8_next(&q);
        w32_free(u);
    }
    return r;
}

static BOOL WINAPI GetCharWidthA(HANDLE h, UINT first, UINT last, int *out)
{
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    for (UINT c = first; c <= last; c++)
        out[c - first] = char_width(f, ansi_char(c));
    return TRUE;
}

static uint32_t float_bits(int v)
{
    if (!v)
        return 0;
    uint32_t sign = v < 0 ? 0x80000000u : 0, m = v < 0 ? (uint32_t)-v : (uint32_t)v;
    int e = 31;
    while (!(m >> e))
        e--;
    uint32_t frac = e > 23 ? m >> (e - 23) : m << (23 - e);
    return sign | (uint32_t)(e + 127) << 23 | (frac & 0x7fffff);
}

static BOOL abc_widths(HANDLE h, UINT first, UINT last, uint32_t *out, bool fl, bool ansi)
{
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    for (UINT c = first; c <= last; c++, out += 3) {
        int w = char_width(f, ansi ? ansi_char(c) : c);
        out[0] = out[2] = 0;
        out[1] = fl ? float_bits(w) : (uint32_t)w;
    }
    return TRUE;
}

static BOOL WINAPI GetCharABCWidthsW(HANDLE h, UINT first, UINT last, uint32_t *out)
{
    return abc_widths(h, first, last, out, false, false);
}

static BOOL WINAPI GetCharABCWidthsA(HANDLE h, UINT first, UINT last, uint32_t *out)
{
    return abc_widths(h, first, last, out, false, true);
}

static BOOL WINAPI GetCharABCWidthsFloatW(HANDLE h, UINT first, UINT last, uint32_t *out)
{
    return abc_widths(h, first, last, out, true, false);
}

static BOOL WINAPI GetCharABCWidthsFloatA(HANDLE h, UINT first, UINT last, uint32_t *out)
{
    return abc_widths(h, first, last, out, true, true);
}

static int extent_ex(HANDLE h, const uint32_t *cp, int n, int max, int *fit, int *dx)
{
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    int x = 0, fits = 0;
    for (int i = 0; i < n; i++) {
        x += char_width(f, cp[i]);
        if (dx)
            dx[i] = x;
        if (max < 0 || x <= max)
            fits = i + 1;
    }
    if (fit)
        *fit = fits;
    return x;
}

static uint32_t *wide_codepoints(LPCWSTR s, int n)
{
    uint32_t *cp = w32_alloc((size_t)(n ? n : 1) * 4);
    for (int i = 0; cp && i < n; i++)
        cp[i] = s[i];
    return cp;
}

static BOOL WINAPI GetTextExtentExPointW(HANDLE h, LPCWSTR s, int n, int max, int *fit, int *dx, SIZE *size)
{
    if (n < 0 || (!s && n))
        return FALSE;
    uint32_t *cp = wide_codepoints(s, n);
    if (!cp)
        return FALSE;
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    int w = extent_ex(h, cp, n, fit ? max : -1, fit, dx);
    w32_free(cp);
    if (size)
        *size = (SIZE){w, f->height};
    return TRUE;
}

static BOOL WINAPI GetTextExtentExPointA(HANDLE h, LPCSTR s, int n, int max, int *fit, int *dx, SIZE *size)
{
    if (n < 0 || (!s && n))
        return FALSE;
    uint32_t *cp = w32_alloc((size_t)(n ? n : 1) * 4);
    if (!cp)
        return FALSE;
    for (int i = 0; i < n; i++)
        cp[i] = ansi_char((uint8_t)s[i]);
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    int w = extent_ex(h, cp, n, fit ? max : -1, fit, dx);
    w32_free(cp);
    if (size)
        *size = (SIZE){w, f->height};
    return TRUE;
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static DWORD WINAPI GetCharacterPlacementW(HANDLE h, LPCWSTR s, int n, int max, uint8_t *res, DWORD flags)
{
    UNUSED(flags);
    if (n < 0 || (!s && n))
        return 0;
    uint32_t *cp = wide_codepoints(s, n);
    if (!cp)
        return 0;
    int ps = w32->win64 ? 8 : 4, fit = n;
    int *dx = w32_alloc((size_t)(n ? n : 1) * sizeof(int));
    int w = dx ? extent_ex(h, cp, n, max > 0 ? max : -1, &fit, dx) : 0;
    struct w32_dc *d = dc_of(h);
    const struct ui_font *f = d ? dc_font(d) : &font_sans;
    if (res && dx) {
        int base = w32->win64 ? 8 : 4;
        uint8_t *field = res + base + 6 * ps;
        int glyphs = (int)get_u32(field);
        int count = glyphs && glyphs < n ? glyphs : n;
        WCHAR *outs = (WCHAR *)(uintptr_t)get_u32(res + base);
        UINT *order = (UINT *)(uintptr_t)get_u32(res + base + ps);
        int *adv = (int *)(uintptr_t)get_u32(res + base + 2 * ps);
        int *caret = (int *)(uintptr_t)get_u32(res + base + 3 * ps);
        char *cls = (char *)(uintptr_t)get_u32(res + base + 4 * ps);
        WCHAR *gl = (WCHAR *)(uintptr_t)get_u32(res + base + 5 * ps);
        for (int i = 0; i < count; i++) {
            if (outs)
                outs[i] = s[i];
            if (order)
                order[i] = (UINT)i;
            if (adv)
                adv[i] = dx[i] - (i ? dx[i - 1] : 0);
            if (caret)
                caret[i] = i ? dx[i - 1] : 0;
            if (cls)
                cls[i] = 0;
            if (gl)
                gl[i] = s[i];
        }
        uint32_t v = (uint32_t)count;
        memcpy(field, &v, 4);
        v = (uint32_t)fit;
        memcpy(field + 4, &v, 4);
    }
    w32_free(dx);
    w32_free(cp);
    return (DWORD)(w & 0xffff) | (DWORD)f->height << 16;
}

static UINT WINAPI SetLayout(HANDLE h, DWORD layout)
{
    UNUSED(h);
    UNUSED(layout);
    return 0;
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

/* DrawFrameControl: check boxes, radio buttons, push buttons, scroll arrows and caption buttons in the desktop's look */
static BOOL WINAPI DrawFrameControl(HANDLE h, RECT *r, UINT type, UINT state)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !r || !target_of(d, &t))
        return FALSE;
    struct rect box = {t.ox + r->left, t.oy + r->top, r->right - r->left, r->bottom - r->top};
    if (box.w <= 0 || box.h <= 0)
        return TRUE;
    t.c->clip = rect_intersect(t.clip, box);
    bool inactive = state & 0x100, pushed = state & 0x200, checked = state & 0x400;
    unsigned kind = state & 0xff;
    int size = MIN(box.w, box.h);
    struct rect sq = {box.x + (box.w - size) / 2, box.y + (box.h - size) / 2, size, size};
    if (type == 4 && (kind & 0x1f) != 0x10) {
        bool radio = (kind & 0xf) == 4 || (kind & 0xf) == 8;
        int radius = radio ? size / 2 : MIN(4, size / 3);
        uint32_t fill = checked && !inactive ? style.accent : pushed ? style.control_pressed : style.field;
        gfx_round(t.c, sq, radius, fill);
        if (!checked || inactive)
            gfx_round_frame(t.c, sq, radius, inactive ? style.separator : style.control_border);
        uint32_t mark = inactive ? style.tertiary : style.accent_text;
        if (checked && radio)
            gfx_circle(t.c, sq.x + size / 2, sq.y + size / 2, MAX(2, size / 5), mark);
        else if (checked)
            gfx_symbol(t.c, sq.x, sq.y, SYM_CHECK, size, mark);
    } else if (type == 4 || type == 1 || type == 3) {
        gfx_round(t.c, box, 4, pushed ? style.control_pressed : style.control);
        gfx_round_frame(t.c, box, 4, style.control_border);
        static const uint8_t scroll_syms[] = {SYM_UP, SYM_DOWN, SYM_LEFT, SYM_RIGHT, 0, SYM_DOWN};
        static const uint8_t caption_syms[] = {SYM_CLOSE, SYM_MINUS, SYM_PLUS, SYM_WINDOW, 0};
        int sym = type == 3 && kind < sizeof(scroll_syms) ? scroll_syms[kind]
                  : type == 1 && kind < sizeof(caption_syms) ? caption_syms[kind] : 0;
        if (sym) {
            int s = MAX(8, size - 4);
            gfx_symbol(t.c, sq.x + (size - s) / 2, sq.y + (size - s) / 2, sym, s, inactive ? style.tertiary : style.text);
        }
    } else if (type == 2) {
        uint32_t ink = w32_rgb(d->s.text);
        if (kind == 1)
            gfx_symbol(t.c, sq.x, sq.y, SYM_CHECK, size, ink);
        else if (kind == 2)
            gfx_circle(t.c, sq.x + size / 2, sq.y + size / 2, MAX(2, size / 6), ink);
        else
            gfx_symbol(t.c, sq.x, sq.y, SYM_RIGHT, size, ink);
    }
    t.c->clip = t.clip;
    damage(d, &t, box);
    return TRUE;
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

/* OBM_CHECKBOXES: a 4 x 3 sheet of 13 x 13 check boxes, radio buttons and 3-state boxes */
HANDLE w32_check_sheet(void)
{
    HANDLE bmp = new_bitmap(52, -39, NULL, 32), dc = CreateCompatibleDC(NULL);
    if (!bmp || !dc)
        return bmp;
    HANDLE old = SelectObject(dc, bmp);
    RECT all = {0, 0, 52, 39};
    FillRect(dc, &all, w32_syscolor_brush(5));
    static const UINT kinds[3] = {0, 4, 8};
    static const UINT states[4] = {0, 0x400, 0x200, 0x600};
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++) {
            RECT r = {col * 13, row * 13, col * 13 + 13, row * 13 + 13};
            DrawFrameControl(dc, &r, 4, kinds[row] | states[col]);
        }
    SelectObject(dc, old);
    DeleteDC(dc);
    return bmp;
}

static HANDLE WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits)
{
    UNUSED(planes);
    struct gdi_obj *o = new_bitmap(w, -h, NULL, 32);
    if (!o || !bits || w <= 0 || h <= 0)
        return o;
    if (bpp == 32) {
        memcpy(o->bits, bits, (size_t)w * (size_t)h * 4);
        return o;
    }
    /* device-dependent rows are padded to 16 bits; 1 bit per pixel is a mask (set = white) */
    size_t stride = (size_t)(((w * (int)bpp + 15) / 16) * 2);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)bits + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            switch (bpp) {
            case 1: c = row[x / 8] >> (7 - x % 8) & 1 ? 0xffffff : 0; break;
            case 24: c = (uint32_t)row[x * 3] | (uint32_t)row[x * 3 + 1] << 8 | (uint32_t)row[x * 3 + 2] << 16; break;
            case 16: {
                uint16_t v = (uint16_t)(row[x * 2] | row[x * 2 + 1] << 8);
                c = RGB((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3);
                break;
            }
            case 8: c = w32_rgb(system_colors[row[x] % 20]); break;
            default: c = 0;
            }
            o->canvas.px[y * o->canvas.stride + x] = c | 0xff000000u;
        }
    }
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
    if (!bmi || bmi->width <= 0 || !bmi->height)
        return NULL;
    /* With a section the pixels live in shared memory another process draws into (Chromium's compositor). */
    void *shared = NULL;
    if (section) {
        uint32_t rows = (uint32_t)(bmi->height < 0 ? -bmi->height : bmi->height);
        uint32_t stride = ((uint32_t)bmi->width * bmi->bpp + 31) / 32 * 4;
        if (!(shared = w32_map_section(section, offset, stride * rows)))
            return NULL;
    }
    struct gdi_obj *o = new_bitmap(bmi->width, bmi->height, shared, bmi->bpp);
    if (!o) {
        if (shared)
            w32_unmap_section(shared);
        return NULL;
    }
    o->mapped = shared != NULL;
    if (bmi->bpp <= 8) {
        int n = bmi->used && bmi->used <= (1u << bmi->bpp) ? (int)bmi->used : 1 << bmi->bpp;
        if ((o->colors = w32_zalloc(sizeof(uint32_t) * (size_t)n))) {
            o->ncolors = n;
            if (usage == 0)
                memcpy(o->colors, (const uint8_t *)bmi + bmi->size, sizeof(uint32_t) * (size_t)n);
        }
    }
    if (bits)
        *bits = o->bits;
    return o;
}

static uint32_t dib_pixel_in(const struct bitmap_info *bmi, const uint32_t *palette, const uint8_t *bits, int x, int y)
{
    int w = bmi->width, h = bmi->height < 0 ? -bmi->height : bmi->height;
    int row = bmi->height < 0 ? y : h - 1 - y;
    int stride = ((w * bmi->bpp + 31) / 32) * 4;
    const uint8_t *p = bits + (size_t)row * (size_t)stride;
    static const uint32_t gray[2] = {0, 0xffffff};
    if (!palette)
        palette = gray;
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

static uint32_t dib_pixel(const struct bitmap_info *bmi, const uint8_t *bits, int x, int y)
{
    return dib_pixel_in(bmi, (const uint32_t *)((const uint8_t *)bmi + bmi->size), bits, x, y);
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
        int src_y = sy + (int)((int64_t)y * sh / ABS(dh));
        if (src_y < 0 || src_y >= h)
            continue;
        if (bmi->bpp == 32 && dw == sw) {
            /* unscaled 32-bit rows (how programs present frames) are copied as they are */
            int row = bmi->height < 0 ? src_y : h - 1 - src_y;
            const uint32_t *in = (const uint32_t *)bits + (size_t)row * (size_t)bmi->width + sx;
            uint32_t *out = t->c->px + ty * t->c->stride + dx;
            int x0 = MAX(MAX(t->clip.x - dx, -sx), 0), x1 = MIN(MIN(t->clip.x + t->clip.w - dx, bmi->width - sx), dw);
            for (int x = x0; x < x1; x++)
                out[x] = in[x] | 0xff000000u;
            continue;
        }
        for (int x = 0; x < ABS(dw); x++) {
            int tx = dx + (dw < 0 ? -x : x);
            if (tx < t->clip.x || tx >= t->clip.x + t->clip.w)
                continue;
            int src_x = sx + (int)((int64_t)x * sw / ABS(dw));
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
    default: {
        /* any other ternary operation: bit i of the code is the result for pattern/source/destination bits i */
        uint8_t code = (uint8_t)(rop >> 16);
        uint32_t r = 0;
        for (int i = 0; i < 8; i++)
            if (code >> i & 1)
                r |= (i & 4 ? pat : ~pat) & (i & 2 ? src : ~src) & (i & 1 ? dst : ~dst);
        return r & 0xffffff;
    }
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
    if (rop == SRCCOPY && uses_src && !(sb && sb->bpp != 32) && dw == sw && dh == sh && dw > 0 && dh > 0) {
        /* BitBlt: rows of a 32-bit source copied as they are (how programs present their frames) */
        int x0 = MAX(dt.clip.x - (dt.ox + dx), MAX(-(st.ox + sx), 0));
        int x1 = MIN(dt.clip.x + dt.clip.w - (dt.ox + dx), MIN(st.c->w - (st.ox + sx), dw));
        for (int y = 0; y < dh && x0 < x1; y++) {
            int ty = dt.oy + dy + y, py = st.oy + sy + y;
            if (ty < dt.clip.y || ty >= dt.clip.y + dt.clip.h || py < 0 || py >= st.c->h)
                continue;
            uint32_t *out = dt.c->px + ty * dt.c->stride + dt.ox + dx + x0;
            const uint32_t *in = st.c->px + py * st.c->stride + st.ox + sx + x0;
            for (int x = 0; x < x1 - x0; x++)
                out[x] = in[x] | 0xff000000u;
        }
        dt.c->clip = dt.clip;
        damage(d, &dt, (struct rect){dt.ox + dx, dt.oy + dy, dw + 1, dh + 1});
        return TRUE;
    }
    int adw = ABS(dw);
    int *xmap = uses_src ? kmalloc(sizeof(int) * (size_t)adw) : NULL;
    if (uses_src && !xmap)
        return FALSE;
    for (int x = 0; uses_src && x < adw; x++)
        xmap[x] = sx + (int)((int64_t)x * sw / adw);
    for (int y = 0; y < ABS(dh); y++) {
        int ty = dt.oy + dy + (dh < 0 ? -y : y);
        if (ty < dt.clip.y || ty >= dt.clip.y + dt.clip.h)
            continue;
        int yy = sy + (int)((int64_t)y * sh / ABS(dh));
        for (int x = 0; x < adw; x++) {
            int tx = dt.ox + dx + (dw < 0 ? -x : x);
            if (tx < dt.clip.x || tx >= dt.clip.x + dt.clip.w)
                continue;
            uint32_t *out = dt.c->px + ty * dt.c->stride + tx;
            uint32_t src = 0;
            if (uses_src) {
                int xx = xmap[x];
                if (sb && sb->bpp != 32) {
                    if (xx < 0 || yy < 0 || xx >= sb->canvas.w || yy >= sb->canvas.h)
                        continue;
                    src = dib_pixel_in(&bmi, sb->colors, sb->bits, xx, yy);
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
    kfree(xmap);
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

/* A packed DIB from a program's resources (RT_BITMAP) as a bitmap object. */
HANDLE w32_bitmap_from_dib(const void *dib, uint32_t size)
{
    const struct bitmap_info *bmi = dib;
    if (!bmi || size < sizeof(*bmi) || bmi->size < sizeof(*bmi) || bmi->size > size || bmi->width <= 0 ||
        bmi->width > 8192 || !bmi->height || bmi->height > 8192 || bmi->height < -8192)
        return NULL;
    uint32_t colors = bmi->used ? bmi->used : bmi->bpp <= 8 ? 1u << bmi->bpp : 0;
    uint32_t offset = bmi->size + colors * 4 + (bmi->compression == 3 ? 12 : 0);
    int height = bmi->height < 0 ? -bmi->height : bmi->height;
    uint32_t stride = ((uint32_t)bmi->width * bmi->bpp + 31) / 32 * 4;
    if (offset + stride * (uint32_t)height > size || (bmi->compression != 0 && bmi->compression != 3))
        return NULL;
    struct gdi_obj *o = new_bitmap(bmi->width, height, NULL, 32);
    if (o)
        SetDIBits(NULL, o, 0, (UINT)height, (const uint8_t *)dib + offset, bmi, 0);
    return o;
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

void w32_gdi_reset(void)
{
    memset(stock, 0, sizeof(stock));
    memset(sys_brushes, 0, sizeof(sys_brushes));
    memset(&default_bitmap_obj, 0, sizeof(default_bitmap_obj));
    default_bitmap_obj.magic = W32_GDI_MAGIC;
    default_bitmap_obj.kind = OBJ_BITMAP;
    default_bitmap_obj.stock = true;
}

/* Fonts a program carries: accepted, and text asking for them is drawn with the closest built-in face. */
static HANDLE WINAPI AddFontMemResourceEx(void *data, DWORD size, void *reserved, DWORD *count)
{
    UNUSED(reserved);
    if (!data || !size || !count) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    *count = 1;
    return (HANDLE)(uintptr_t)0x5f000000u;
}

static HANDLE WINAPI CreatePalette(const WORD *lp)
{
    if (!lp || !lp[1])
        return NULL;
    struct gdi_obj *o = palette_new(lp[1]);
    if (o)
        memcpy(o->colors, lp + 2, sizeof(uint32_t) * lp[1]);
    return o;
}

static HANDLE WINAPI CreateHalftonePalette(HANDLE h)
{
    UNUSED(h);
    struct gdi_obj *o = palette_new(256);
    if (!o)
        return NULL;
    memcpy(o->colors, system_colors, 10 * sizeof(uint32_t));
    memcpy(o->colors + 246, system_colors + 10, 10 * sizeof(uint32_t));
    int i = 10;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                o->colors[i++] = (uint32_t)(r * 51) | (uint32_t)(g * 51) << 8 | (uint32_t)(b * 51) << 16;
    for (int k = 0; i < 246; i++, k++) {
        uint32_t v = (uint32_t)(8 + k * 12) & 0xff;
        o->colors[i] = v | v << 8 | v << 16;
    }
    return o;
}

static struct gdi_obj *palette_of(HANDLE h)
{
    struct gdi_obj *o = obj(h);
    return o && o->kind == OBJ_PAL ? o : NULL;
}

static HANDLE WINAPI SelectPalette(HANDLE h, HANDLE pal, BOOL background)
{
    UNUSED(background);
    struct w32_dc *d = dc_of(h);
    struct gdi_obj *p = palette_of(pal);
    if (!d || !p)
        return NULL;
    struct gdi_obj *old = d->s.palette ? d->s.palette : w32_stock(15);
    d->s.palette = p;
    return old;
}

/* a true-colour display maps every entry without changing the system palette */
static UINT WINAPI RealizePalette(HANDLE h)
{
    return dc_of(h) ? 0 : 0xffffffffu;
}

static UINT WINAPI GetPaletteEntries(HANDLE pal, UINT start, UINT n, uint32_t *out)
{
    struct gdi_obj *p = palette_of(pal);
    if (!p)
        return 0;
    if (!out)
        return (UINT)p->ncolors;
    if (start >= (UINT)p->ncolors)
        return 0;
    n = MIN(n, (UINT)p->ncolors - start);
    memcpy(out, p->colors + start, sizeof(uint32_t) * n);
    return n;
}

static UINT WINAPI SetPaletteEntries(HANDLE pal, UINT start, UINT n, const uint32_t *in)
{
    struct gdi_obj *p = palette_of(pal);
    if (!p || p->stock || !in || start >= (UINT)p->ncolors)
        return 0;
    n = MIN(n, (UINT)p->ncolors - start);
    memcpy(p->colors + start, in, sizeof(uint32_t) * n);
    return n;
}

static BOOL WINAPI ResizePalette(HANDLE pal, UINT n)
{
    struct gdi_obj *p = palette_of(pal);
    uint32_t *c = p && !p->stock && n ? w32_zalloc(sizeof(uint32_t) * n) : NULL;
    if (!c)
        return FALSE;
    memcpy(c, p->colors, sizeof(uint32_t) * MIN(n, (UINT)p->ncolors));
    w32_free(p->colors);
    p->colors = c;
    p->ncolors = (int)n;
    return TRUE;
}

static UINT WINAPI GetNearestPaletteIndex(HANDLE pal, COLORREF c)
{
    struct gdi_obj *p = palette_of(pal);
    if (!p)
        return 0xffffffffu;
    UINT best = 0;
    int dist = 1 << 30;
    for (int i = 0; i < p->ncolors; i++) {
        int dr = (int)(p->colors[i] & 0xff) - (int)(c & 0xff), dg = (int)(p->colors[i] >> 8 & 0xff) - (int)(c >> 8 & 0xff),
            db = (int)(p->colors[i] >> 16 & 0xff) - (int)(c >> 16 & 0xff);
        if (dr * dr + dg * dg + db * db < dist) {
            dist = dr * dr + dg * dg + db * db;
            best = (UINT)i;
        }
    }
    return best;
}

static UINT WINAPI GetSystemPaletteEntries(HANDLE h, UINT start, UINT n, uint32_t *out)
{
    UNUSED(h);
    struct gdi_obj *p = CreateHalftonePalette(NULL);
    UINT got = p ? GetPaletteEntries(p, start, n, out) : 0;
    if (p)
        DeleteObject(p);
    return got;
}

static BOOL WINAPI UnrealizeObject(HANDLE h)
{
    struct gdi_obj *o = obj(h);
    return o && (o->kind == OBJ_PAL || o->kind == OBJ_BRUSH);
}

static struct gdi_obj *selected_dib(HANDLE h)
{
    struct w32_dc *d = dc_of(h);
    struct gdi_obj *b = d && d->memory ? d->s.bitmap : NULL;
    return b && b->colors ? b : NULL;
}

static UINT WINAPI GetDIBColorTable(HANDLE h, UINT start, UINT n, uint32_t *out)
{
    struct gdi_obj *b = selected_dib(h);
    if (!b || !out || start >= (UINT)b->ncolors)
        return 0;
    n = MIN(n, (UINT)b->ncolors - start);
    memcpy(out, b->colors + start, sizeof(uint32_t) * n);
    return n;
}

static UINT WINAPI SetDIBColorTable(HANDLE h, UINT start, UINT n, const uint32_t *in)
{
    struct gdi_obj *b = selected_dib(h);
    if (!b || !in || start >= (UINT)b->ncolors)
        return 0;
    n = MIN(n, (UINT)b->ncolors - start);
    memcpy(b->colors + start, in, sizeof(uint32_t) * n);
    return n;
}

static LONG WINAPI GetBitmapBits(HANDLE bitmap, LONG size, void *out)
{
    struct gdi_obj *b = obj(bitmap);
    if (!b || b->kind != OBJ_BITMAP || !b->canvas.px)
        return 0;
    LONG total = b->canvas.w * b->canvas.h * 4;
    if (!out)
        return total;
    LONG n = MIN(size, total);
    for (LONG done = 0, y = 0; done < n; y++) {
        LONG k = MIN(n - done, (LONG)b->canvas.w * 4);
        memcpy((uint8_t *)out + done, b->canvas.px + y * b->canvas.stride, (size_t)k);
        done += k;
    }
    return n;
}

static LONG WINAPI SetBitmapBits(HANDLE bitmap, DWORD size, const void *in)
{
    struct gdi_obj *b = obj(bitmap);
    if (!b || b->kind != OBJ_BITMAP || !b->canvas.px || !in)
        return 0;
    LONG n = MIN((LONG)size, b->canvas.w * b->canvas.h * 4);
    for (LONG done = 0, y = 0; done < n; y++) {
        LONG k = MIN(n - done, (LONG)b->canvas.w * 4);
        memcpy(b->canvas.px + y * b->canvas.stride, (const uint8_t *)in + done, (size_t)k);
        done += k;
    }
    return n;
}

static BOOL WINAPI GetCurrentPositionEx(HANDLE h, POINT *out)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !out)
        return FALSE;
    *out = (POINT){d->x, d->y};
    return TRUE;
}

static BOOL WINAPI GetWindowOrgEx(HANDLE h, POINT *out)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !out)
        return FALSE;
    *out = (POINT){d->s.org_x, d->s.org_y};
    return TRUE;
}

static BOOL WINAPI GetViewportOrgEx(HANDLE h, POINT *out)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !out)
        return FALSE;
    *out = (POINT){-d->s.org_x, -d->s.org_y};
    return TRUE;
}

static BOOL WINAPI GetDCOrgEx(HANDLE h, POINT *out)
{
    struct w32_dc *d = dc_of(h);
    if (!d || !out)
        return FALSE;
    *out = (POINT){0, 0};
    if (!d->memory && d->win && w32_win(d->win)) {
        struct w32_window *top = w32_top(d->win);
        struct rect a = w32_area(d->win), cr = top->wm ? wm_client_rect(top->wm) : (struct rect){0, 0, 0, 0};
        *out = (POINT){cr.x + a.x, cr.y + a.y};
    }
    return TRUE;
}

/* clipping is one rectangle: an excluded band along an edge shrinks it, a hole inside keeps the whole rectangle */
static int WINAPI ExcludeClipRect(HANDLE h, int l, int t, int r, int b)
{
    struct w32_dc *d = dc_of(h);
    struct target tg;
    if (!d)
        return 0;
    struct rect c = d->s.clip;
    if (!d->s.clipped) {
        if (!target_of(d, &tg))
            return 1;
        c = (struct rect){tg.clip.x - tg.ox - d->s.org_x, tg.clip.y - tg.oy - d->s.org_y, tg.clip.w, tg.clip.h};
    }
    int cr = c.x + c.w, cb = c.y + c.h;
    if (r <= c.x || l >= cr || b <= c.y || t >= cb)
        return region_kind(c);
    if (t <= c.y && b >= cb) {
        if (l <= c.x)
            c = (struct rect){r, c.y, cr - r, c.h};
        else if (r >= cr)
            c.w = l - c.x;
    } else if (l <= c.x && r >= cr) {
        if (t <= c.y)
            c = (struct rect){c.x, b, c.w, cb - b};
        else if (b >= cb)
            c.h = t - c.y;
    }
    if (c.w < 0)
        c.w = 0;
    if (c.h < 0)
        c.h = 0;
    d->s.clip = c;
    d->s.clipped = true;
    return c.w && c.h ? 3 : 1;
}

static BOOL WINAPI RectVisible(HANDLE h, const RECT *r)
{
    RECT box;
    if (!r || GetClipBox(h, &box) <= 1)
        return FALSE;
    return MAX(r->left, box.left) < MIN(r->right, box.right) && MAX(r->top, box.top) < MIN(r->bottom, box.bottom);
}

static BOOL WINAPI PtVisible(HANDLE h, int x, int y)
{
    RECT r = {x, y, x + 1, y + 1};
    return RectVisible(h, &r);
}

static BOOL WINAPI ExtFloodFill(HANDLE h, int x, int y, COLORREF color, UINT type)
{
    struct w32_dc *d = dc_of(h);
    struct target t;
    if (!d || !target_of(d, &t))
        return FALSE;
    int sx = t.ox + x, sy = t.oy + y;
    struct rect c = t.clip;
    if (sx < c.x || sy < c.y || sx >= c.x + c.w || sy >= c.y + c.h)
        return FALSE;
    bool none;
    uint32_t fillc = (brush_color(d, d->s.brush, &none) & 0xffffff) | 0xff000000u;
    uint32_t ref = w32_rgb(color) & 0xffffff;
    uint32_t *px = t.c->px;
    int stride = t.c->stride;
#define INSIDE(X, Y) (type ? (px[(Y) * stride + (X)] & 0xffffff) == ref : (px[(Y) * stride + (X)] & 0xffffff) != ref)
    if (none || !INSIDE(sx, sy) || (px[sy * stride + sx] | 0xff000000u) == fillc)
        return !none;
    int cap = 4096, n = 0;
    int *stack = kmalloc(sizeof(int) * 2 * (size_t)cap);
    if (!stack)
        return FALSE;
    stack[n++] = sx;
    stack[n++] = sy;
    struct rect dirty = {sx, sy, 1, 1};
    while (n) {
        int py = stack[--n], pxx = stack[--n];
        if (!INSIDE(pxx, py) || (px[py * stride + pxx] | 0xff000000u) == fillc)
            continue;
        int l = pxx, r = pxx;
        while (l > c.x && INSIDE(l - 1, py) && (px[py * stride + l - 1] | 0xff000000u) != fillc)
            l--;
        while (r + 1 < c.x + c.w && INSIDE(r + 1, py) && (px[py * stride + r + 1] | 0xff000000u) != fillc)
            r++;
        for (int i = l; i <= r; i++)
            px[py * stride + i] = fillc;
        dirty = rect_union(dirty, (struct rect){l, py, r - l + 1, 1});
        for (int ny = py - 1; ny <= py + 1; ny += 2) {
            if (ny < c.y || ny >= c.y + c.h)
                continue;
            for (int i = l; i <= r; i++) {
                if (!INSIDE(i, ny) || (px[ny * stride + i] | 0xff000000u) == fillc)
                    continue;
                if (n + 2 > cap * 2) {
                    int *grown = kmalloc(sizeof(int) * 4 * (size_t)cap);
                    if (!grown)
                        break;
                    memcpy(grown, stack, sizeof(int) * (size_t)n);
                    kfree(stack);
                    stack = grown;
                    cap *= 2;
                }
                stack[n++] = i;
                stack[n++] = ny;
                while (i + 1 <= r && INSIDE(i + 1, ny))
                    i++;
            }
        }
    }
#undef INSIDE
    kfree(stack);
    damage(d, &t, dirty);
    return TRUE;
}

static BOOL WINAPI FloodFill(HANDLE h, int x, int y, COLORREF color)
{
    return ExtFloodFill(h, x, y, color, 0);
}

static BOOL WINAPI LineDDA(int x0, int y0, int x1, int y1, void *fn, LPARAM lp)
{
    if (!fn)
        return FALSE;
    int dx = ABS(x1 - x0), dy = -ABS(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    while (x0 != x1 || y0 != y1) {
        W32_CALL(void(WINAPI *)(int, int, LPARAM), fn, x0, y0, lp);
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    return TRUE;
}

/* MAKEROP4(fore, back): where the mask is set the foreground operation draws, elsewhere the background one */
static BOOL WINAPI MaskBlt(HANDLE hd, int dx, int dy, int w, int h, HANDLE hs, int sx, int sy, HANDLE mask, int mx,
                           int my, DWORD rop)
{
    DWORD fore = rop & 0x00ffffffu, back = (rop >> 8 & 0x00ff0000u) | 0x0000ffffu;
    struct gdi_obj *m = obj(mask);
    if (!m || m->kind != OBJ_BITMAP || !m->canvas.px)
        return stretch(hd, dx, dy, w, h, hs, sx, sy, w, h, fore);
    struct w32_dc *d = dc_of(hd), *s = dc_of(hs);
    struct target dt, st;
    if (!d || !target_of(d, &dt))
        return d != NULL;
    bool has_src = s && target_of(s, &st);
    bool none;
    uint32_t pat = brush_color(d, d->s.brush, &none) & 0xffffff;
    for (int y = 0; y < h; y++) {
        int ty = dt.oy + dy + y, py = has_src ? st.oy + sy + y : 0, my2 = my + y;
        if (ty < dt.clip.y || ty >= dt.clip.y + dt.clip.h || my2 < 0 || my2 >= m->canvas.h)
            continue;
        for (int x = 0; x < w; x++) {
            int tx = dt.ox + dx + x, mx2 = mx + x;
            if (tx < dt.clip.x || tx >= dt.clip.x + dt.clip.w || mx2 < 0 || mx2 >= m->canvas.w)
                continue;
            uint32_t src = 0;
            if (has_src) {
                int px = st.ox + sx + x;
                if (px < 0 || py < 0 || px >= st.c->w || py >= st.c->h)
                    continue;
                src = st.c->px[py * st.c->stride + px] & 0xffffff;
            }
            uint32_t *out = dt.c->px + ty * dt.c->stride + tx;
            bool set = (m->canvas.px[my2 * m->canvas.stride + mx2] & 0xffffff) != 0;
            *out = (raster(src, *out & 0xffffff, pat, set ? fore : back) & 0xffffff) | 0xff000000u;
        }
    }
    dt.c->clip = dt.clip;
    damage(d, &dt, (struct rect){dt.ox + dx, dt.oy + dy, w + 1, h + 1});
    return TRUE;
}

/* Fonts SynthOS can draw, under the names programs ask for */
static const struct {
    const char *face;
    const struct ui_font *font;
    uint8_t pitch;
} font_faces[] = {
    {"Segoe UI", &font_sans, 0x22},       {"Tahoma", &font_sans, 0x22},       {"Arial", &font_sans, 0x22},
    {"Microsoft Sans Serif", &font_sans, 0x22}, {"MS Sans Serif", &font_sans, 0x22}, {"Verdana", &font_sans, 0x22},
    {"Times New Roman", &font_sans, 0x12}, {"Courier New", &font_mono, 0x31}, {"Consolas", &font_mono, 0x31},
    {"Lucida Console", &font_mono, 0x31},
};

static int enum_fonts(HANDLE h, const char *only, uint8_t charset, void *fn, LPARAM lp, bool wide, bool ex)
{
    uint8_t *lf = w32_zalloc(512), *tm = lf ? w32_zalloc(512) : NULL;
    int ret = 1;
    if (!tm) {
        w32_free(lf);
        return 0;
    }
    HANDLE dc = h ? h : w32_screen_dc();
    for (size_t i = 0; i < ARRAY_SIZE(font_faces) && ret; i++) {
        if (only && *only && str_casecmp(only, font_faces[i].face))
            continue;
        if (charset != 0 && charset != 1)
            continue;
        const struct ui_font *f = font_faces[i].font;
        memset(lf, 0, 512);
        memset(tm, 0, 512);
        w32_logfont(lf, wide, -f->height, 400, 0, 0, font_faces[i].face);
        lf[23] = 0;
        lf[27] = font_faces[i].pitch;
        if (ex) {
            size_t full = wide ? 92 : 60, step = wide ? 2 : 1;
            for (size_t k = 0; font_faces[i].face[k] && k < 63; k++)
                lf[full + k * step] = (uint8_t)font_faces[i].face[k];
            const char *style = "Regular", *script = "Western";
            for (size_t k = 0; style[k]; k++)
                lf[full + 64 * step + k * step] = (uint8_t)style[k];
            for (size_t k = 0; script[k]; k++)
                lf[full + 96 * step + k * step] = (uint8_t)script[k];
        }
        struct w32_dc *d = dc_of(dc);
        struct gdi_obj *saved = d ? d->s.font : NULL, probe = {0};
        if (d) {
            probe.magic = W32_GDI_MAGIC;
            probe.kind = OBJ_FONT;
            probe.font = f;
            probe.weight = 400;
            d->s.font = &probe;
        }
        text_metrics(dc, tm, wide);
        if (d)
            d->s.font = saved;
        tm[wide ? 56 : 52] = 0;
        ret = W32_CALL(int(WINAPI *)(void *, void *, DWORD, LPARAM), fn, lf, tm, 4u, lp);
    }
    if (!h)
        w32_free_dc(dc);
    w32_free(tm);
    w32_free(lf);
    return ret;
}

static int WINAPI EnumFontFamiliesExA(HANDLE h, const uint8_t *lf, void *fn, LPARAM lp, DWORD flags)
{
    UNUSED(flags);
    return fn ? enum_fonts(h, lf ? (const char *)lf + 28 : NULL, lf ? lf[23] : 1, fn, lp, false, true) : 0;
}

static int WINAPI EnumFontFamiliesExW(HANDLE h, const uint8_t *lf, void *fn, LPARAM lp, DWORD flags)
{
    UNUSED(flags);
    char *face = lf ? w32_narrow((LPCWSTR)(lf + 28)) : NULL;
    int ret = fn ? enum_fonts(h, face, lf ? lf[23] : 1, fn, lp, true, true) : 0;
    w32_free(face);
    return ret;
}

static int WINAPI EnumFontsA(HANDLE h, const char *face, void *fn, LPARAM lp)
{
    return fn ? enum_fonts(h, face, 1, fn, lp, false, false) : 0;
}

static int WINAPI EnumFontsW(HANDLE h, LPCWSTR face, void *fn, LPARAM lp)
{
    char *f = face ? w32_narrow(face) : NULL;
    int ret = fn ? enum_fonts(h, f, 1, fn, lp, true, false) : 0;
    w32_free(f);
    return ret;
}

static int WINAPI EnumFontFamiliesA(HANDLE h, const char *face, void *fn, LPARAM lp)
{
    return fn ? enum_fonts(h, face, 1, fn, lp, false, true) : 0;
}

static int WINAPI EnumFontFamiliesW(HANDLE h, LPCWSTR face, void *fn, LPARAM lp)
{
    char *f = face ? w32_narrow(face) : NULL;
    int ret = fn ? enum_fonts(h, f, 1, fn, lp, true, true) : 0;
    w32_free(f);
    return ret;
}

/* font files a program installs for itself: accepted, drawn with the closest built-in face */
static int WINAPI AddFontResourceW(LPCWSTR file)
{
    return file ? 1 : 0;
}

static int WINAPI AddFontResourceA(const char *file)
{
    return file ? 1 : 0;
}

static int WINAPI AddFontResourceExW(LPCWSTR file, DWORD flags, void *reserved)
{
    UNUSED(flags);
    UNUSED(reserved);
    return file ? 1 : 0;
}

static BOOL WINAPI RemoveFontResourceW(LPCWSTR file)
{
    return file != NULL;
}

/* Image lists: one ARGB strip, an image per cy rows; alpha 0 is transparent */
#define IL_MAGIC 0x4c494d47u
#define CLR_NONE 0xffffffffu

struct image_list {
    uint32_t magic;
    int cx, cy, count, cap, grow;
    UINT flags;
    COLORREF bk;
    uint32_t *px;
    struct gdi_obj *image;
    int overlay[16];
};

static W32_STATE struct image_list *drag_list;

static struct image_list *il_of(void *h)
{
    struct image_list *il = h;
    return il && (uintptr_t)il >= 0x10000 && il->magic == IL_MAGIC ? il : NULL;
}

static bool il_reserve(struct image_list *il, int n)
{
    if (n <= il->cap)
        return true;
    int cap = MAX(n, il->cap + MAX(il->grow, 4));
    uint32_t *px = w32_zalloc((size_t)il->cx * (size_t)il->cy * (size_t)cap * 4);
    if (!px)
        return false;
    if (il->px)
        memcpy(px, il->px, (size_t)il->cx * (size_t)il->cy * (size_t)il->count * 4);
    w32_free(il->px);
    il->px = px;
    il->cap = cap;
    if (il->image) {
        il->image->bits = NULL;
        il->image->magic = 0;
        w32_free(il->image);
        il->image = NULL;
    }
    return true;
}

static uint32_t *il_cell(struct image_list *il, int i)
{
    return il->px + (size_t)i * (size_t)il->cx * (size_t)il->cy;
}

static void *WINAPI ImageList_Create(int cx, int cy, UINT flags, int initial, int grow)
{
    struct image_list *il = w32_zalloc(sizeof(*il));
    if (!il)
        return NULL;
    *il = (struct image_list){IL_MAGIC, MAX(cx, 1), MAX(cy, 1), 0, 0, MAX(grow, 4), flags, CLR_NONE, NULL, NULL, {0}};
    if (!il_reserve(il, MAX(initial, 1))) {
        w32_free(il);
        return NULL;
    }
    return il;
}

static BOOL WINAPI ImageList_Destroy(void *h)
{
    struct image_list *il = il_of(h);
    if (!il)
        return FALSE;
    if (il->image) {
        il->image->bits = NULL;
        il->image->magic = 0;
        w32_free(il->image);
    }
    if (drag_list == il)
        drag_list = NULL;
    il->magic = 0;
    w32_free(il->px);
    w32_free(il);
    return TRUE;
}

static int WINAPI ImageList_GetImageCount(void *h)
{
    struct image_list *il = il_of(h);
    return il ? il->count : 0;
}

static BOOL WINAPI ImageList_SetImageCount(void *h, UINT n)
{
    struct image_list *il = il_of(h);
    if (!il || !il_reserve(il, (int)n))
        return FALSE;
    if ((int)n > il->count)
        memset(il_cell(il, il->count), 0, (size_t)il->cx * (size_t)il->cy * (size_t)((int)n - il->count) * 4);
    il->count = (int)n;
    return TRUE;
}

static BOOL WINAPI ImageList_GetIconSize(void *h, int *cx, int *cy)
{
    struct image_list *il = il_of(h);
    if (!il)
        return FALSE;
    if (cx)
        *cx = il->cx;
    if (cy)
        *cy = il->cy;
    return TRUE;
}

static BOOL WINAPI ImageList_SetIconSize(void *h, int cx, int cy)
{
    struct image_list *il = il_of(h);
    if (!il || cx <= 0 || cy <= 0)
        return FALSE;
    w32_free(il->px);
    il->px = NULL;
    il->cap = il->count = 0;
    il->cx = cx;
    il->cy = cy;
    return il_reserve(il, 4);
}

static COLORREF WINAPI ImageList_SetBkColor(void *h, COLORREF c)
{
    struct image_list *il = il_of(h);
    if (!il)
        return CLR_NONE;
    COLORREF old = il->bk;
    il->bk = c;
    return old;
}

static COLORREF WINAPI ImageList_GetBkColor(void *h)
{
    struct image_list *il = il_of(h);
    return il ? il->bk : CLR_NONE;
}

/* source pixel of a bitmap in screen order, whatever its depth */
static bool bitmap_pixel(struct gdi_obj *b, int x, int y, uint32_t *out)
{
    if (!b || x < 0 || y < 0 || x >= b->canvas.w || y >= b->canvas.h)
        return false;
    if (b->canvas.px) {
        *out = b->canvas.px[y * b->canvas.stride + x];
        return true;
    }
    if (!b->bits)
        return false;
    struct bitmap_info bmi = {40, b->canvas.w, b->height, 1, (WORD)b->bpp, 0, 0, 0, 0, 0, 0};
    *out = dib_pixel_in(&bmi, b->colors, b->bits, x, y);
    return true;
}

/* images from a bitmap strip: a mask (set = transparent) or the bitmap's own alpha decide what shows */
static void il_store(struct image_list *il, int index, struct gdi_obj *b, struct gdi_obj *mask, bool keyed,
                     uint32_t key, int n)
{
    bool alpha = false;
    if (!mask && !keyed && b->bpp == 32 && (il->flags & 0xfe) >= 0x20)
        for (int y = 0; y < b->canvas.h && !alpha; y++)
            for (int x = 0; x < b->canvas.w && !alpha; x++) {
                uint32_t p;
                alpha = bitmap_pixel(b, x, y, &p) && (p >> 24) != 0 && (p >> 24) != 0xff;
            }
    for (int i = 0; i < n; i++) {
        uint32_t *cell = il_cell(il, index + i);
        for (int y = 0; y < il->cy; y++)
            for (int x = 0; x < il->cx; x++) {
                uint32_t p = 0, m = 0;
                int sx = (index + i - index) * il->cx + x;
                if (!bitmap_pixel(b, sx, y, &p)) {
                    cell[y * il->cx + x] = 0;
                    continue;
                }
                bool clear = keyed ? (p & 0xffffff) == key : mask && bitmap_pixel(mask, sx, y, &m) && (m & 0xffffff);
                cell[y * il->cx + x] = clear ? 0 : alpha ? p : (p & 0xffffff) | 0xff000000u;
            }
    }
}

static int il_add(void *h, HANDLE bitmap, HANDLE mask, bool keyed, COLORREF key)
{
    struct image_list *il = il_of(h);
    struct gdi_obj *b = obj(bitmap);
    if (!il || !b || b->kind != OBJ_BITMAP)
        return -1;
    int n = MAX(b->canvas.w / il->cx, 1);
    if (!il_reserve(il, il->count + n))
        return -1;
    struct gdi_obj *m = obj(mask);
    il_store(il, il->count, b, m && m->kind == OBJ_BITMAP ? m : NULL, keyed, w32_rgb(key) & 0xffffff, n);
    il->count += n;
    return il->count - n;
}

static int WINAPI ImageList_Add(void *h, HANDLE bitmap, HANDLE mask)
{
    return il_add(h, bitmap, mask, false, 0);
}

static int WINAPI ImageList_AddMasked(void *h, HANDLE bitmap, COLORREF key)
{
    struct gdi_obj *b = obj(bitmap);
    if (key == CLR_NONE && b && b->canvas.px)
        key = to_colorref(b->canvas.px[0]);
    return il_add(h, bitmap, NULL, key != CLR_NONE, key);
}

static BOOL WINAPI ImageList_Replace(void *h, int i, HANDLE bitmap, HANDLE mask)
{
    struct image_list *il = il_of(h);
    struct gdi_obj *b = obj(bitmap), *m = obj(mask);
    if (!il || !b || i < 0 || i >= il->count)
        return FALSE;
    il_store(il, i, b, m && m->kind == OBJ_BITMAP ? m : NULL, false, 0, 1);
    return TRUE;
}

/* icons carry no pixels here: the slot is kept, empty */
static int WINAPI ImageList_ReplaceIcon(void *h, int i, HANDLE icon)
{
    UNUSED(icon);
    struct image_list *il = il_of(h);
    if (!il || i >= il->count)
        return -1;
    if (i < 0) {
        if (!il_reserve(il, il->count + 1))
            return -1;
        i = il->count++;
    }
    memset(il_cell(il, i), 0, (size_t)il->cx * (size_t)il->cy * 4);
    return i;
}

static BOOL WINAPI ImageList_Remove(void *h, int i)
{
    struct image_list *il = il_of(h);
    if (!il || i >= il->count)
        return FALSE;
    if (i < 0) {
        il->count = 0;
        return TRUE;
    }
    size_t cell = (size_t)il->cx * (size_t)il->cy;
    memmove(il_cell(il, i), il_cell(il, i + 1), cell * (size_t)(il->count - i - 1) * 4);
    il->count--;
    return TRUE;
}

static BOOL WINAPI ImageList_Copy(void *dst, int di, void *src, int si, UINT flags)
{
    struct image_list *d = il_of(dst), *s = il_of(src);
    if (!d || !s || d->cx != s->cx || d->cy != s->cy || di < 0 || si < 0 || di >= d->count || si >= s->count)
        return FALSE;
    size_t cell = (size_t)d->cx * (size_t)d->cy * 4;
    if (flags & 1) {
        uint32_t *tmp = w32_alloc(cell);
        if (!tmp)
            return FALSE;
        memcpy(tmp, il_cell(d, di), cell);
        memcpy(il_cell(d, di), il_cell(s, si), cell);
        memcpy(il_cell(s, si), tmp, cell);
        w32_free(tmp);
    } else {
        memcpy(il_cell(d, di), il_cell(s, si), cell);
    }
    return TRUE;
}

static void *WINAPI ImageList_Duplicate(void *h)
{
    struct image_list *il = il_of(h), *n = il ? ImageList_Create(il->cx, il->cy, il->flags, il->count, il->grow) : NULL;
    if (!n)
        return NULL;
    memcpy(n->px, il->px, (size_t)il->cx * (size_t)il->cy * (size_t)il->count * 4);
    n->count = il->count;
    n->bk = il->bk;
    memcpy(n->overlay, il->overlay, sizeof(n->overlay));
    return n;
}

static BOOL WINAPI ImageList_SetOverlayImage(void *h, int image, int overlay)
{
    struct image_list *il = il_of(h);
    if (!il || overlay < 1 || overlay > 15)
        return FALSE;
    il->overlay[overlay] = image + 1;
    return TRUE;
}

static void il_paint(struct image_list *il, int i, struct w32_dc *d, struct target *t, int x, int y, int w, int h,
                     COLORREF bk, COLORREF fg, UINT how)
{
    const uint32_t *cell = il_cell(il, i);
    bool mask_only = how & 0x10, blend = how & 6;
    if (bk == 0xff000000u)
        bk = il->bk;
    bool fill_bk = !(how & 1) && bk != CLR_NONE && !mask_only;
    uint32_t bkc = w32_rgb(bk) & 0xffffff, fgc = fg == 0xff000000u || fg == CLR_NONE ? style.accent : w32_rgb(fg);
    for (int yy = 0; yy < h && yy < il->cy; yy++) {
        int ty = t->oy + y + yy;
        if (ty < t->clip.y || ty >= t->clip.y + t->clip.h)
            continue;
        for (int xx = 0; xx < w && xx < il->cx; xx++) {
            int tx = t->ox + x + xx;
            if (tx < t->clip.x || tx >= t->clip.x + t->clip.w)
                continue;
            uint32_t *out = t->c->px + ty * t->c->stride + tx, p = cell[yy * il->cx + xx];
            unsigned a = p >> 24;
            if (mask_only) {
                *out = a ? 0xff000000u : 0xffffffffu;
                continue;
            }
            uint32_t under = fill_bk ? bkc : *out & 0xffffff;
            if (blend && a)
                p = (p & 0xff000000u) | (((p & 0xfefefe) >> 1) + ((fgc & 0xfefefe) >> 1));
            uint32_t r = ((p >> 16 & 0xff) * a + (under >> 16 & 0xff) * (255 - a)) / 255,
                     g = ((p >> 8 & 0xff) * a + (under >> 8 & 0xff) * (255 - a)) / 255,
                     b = ((p & 0xff) * a + (under & 0xff) * (255 - a)) / 255;
            *out = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
    UNUSED(d);
}

static BOOL WINAPI ImageList_DrawEx(void *h, int i, HANDLE dc, int x, int y, int w, int hgt, COLORREF bk,
                                    COLORREF fg, UINT style)
{
    struct image_list *il = il_of(h);
    struct w32_dc *d = dc_of(dc);
    struct target t;
    if (!il || !d || i < 0 || i >= il->count)
        return FALSE;
    if (!target_of(d, &t))
        return TRUE;
    if (w <= 0)
        w = il->cx;
    if (hgt <= 0)
        hgt = il->cy;
    il_paint(il, i, d, &t, x, y, w, hgt, bk, fg, style);
    int ov = (int)(style >> 8 & 0xf);
    if (ov && il->overlay[ov] > 0 && il->overlay[ov] - 1 < il->count)
        il_paint(il, il->overlay[ov] - 1, d, &t, x, y, w, hgt, CLR_NONE, 0, 1);
    t.c->clip = t.clip;
    damage(d, &t, (struct rect){t.ox + x, t.oy + y, w, hgt});
    return TRUE;
}

static BOOL WINAPI ImageList_Draw(void *h, int i, HANDLE dc, int x, int y, UINT style)
{
    return ImageList_DrawEx(h, i, dc, x, y, 0, 0, 0xff000000u, 0xff000000u, style);
}

/* IMAGELISTDRAWPARAMS: cbSize, himl, i, hdcDst, x, y, cx, cy, xBitmap, yBitmap, rgbBk, rgbFg, fStyle, ... */
static BOOL WINAPI ImageList_DrawIndirect(const uint32_t *p)
{
    if (!p)
        return FALSE;
    return ImageList_DrawEx((void *)(uintptr_t)p[1], (int)p[2], (HANDLE)(uintptr_t)p[3], (int)p[4], (int)p[5],
                            (int)p[6], (int)p[7], p[10], p[11], p[12]);
}

static struct gdi_obj *il_bitmap(struct image_list *il)
{
    if (!il->image) {
        struct gdi_obj *o = new_obj(OBJ_BITMAP);
        if (!o)
            return NULL;
        o->stock = true;
        o->bpp = 32;
        o->bits = il->px;
        o->canvas = canvas_make(il->px, il->cx, il->cy * il->cap);
        o->height = -il->cy * il->cap;
        il->image = o;
    }
    return il->image;
}

/* IMAGEINFO: hbmImage, hbmMask, Unused1, Unused2, rcImage */
static BOOL WINAPI ImageList_GetImageInfo(void *h, int i, uint32_t *info)
{
    struct image_list *il = il_of(h);
    struct gdi_obj *b = il ? il_bitmap(il) : NULL;
    if (!b || !info || i < 0 || i >= il->count)
        return FALSE;
    info[0] = (uint32_t)(uintptr_t)b;
    info[1] = 0;
    info[2] = info[3] = 0;
    info[4] = 0;
    info[5] = (uint32_t)(i * il->cy);
    info[6] = (uint32_t)il->cx;
    info[7] = (uint32_t)((i + 1) * il->cy);
    return TRUE;
}

static HANDLE WINAPI ImageList_GetIcon(void *h, int i, UINT flags)
{
    UNUSED(flags);
    struct image_list *il = il_of(h);
    return il && i >= 0 && i < il->count ? (HANDLE)(uintptr_t)(0x30000u | 32512u) : NULL;
}

static void *WINAPI ImageList_Merge(void *a, int ia, void *b, int ib, int dx, int dy)
{
    struct image_list *la = il_of(a), *lb = il_of(b);
    if (!la || !lb || ia < 0 || ib < 0 || ia >= la->count || ib >= lb->count)
        return NULL;
    struct image_list *n = ImageList_Create(MAX(la->cx, lb->cx + dx), MAX(la->cy, lb->cy + dy), la->flags, 1, 1);
    if (!n)
        return NULL;
    n->count = 1;
    uint32_t *out = il_cell(n, 0);
    for (int y = 0; y < la->cy; y++)
        memcpy(out + y * n->cx, il_cell(la, ia) + y * la->cx, (size_t)la->cx * 4);
    for (int y = 0; y < lb->cy; y++)
        for (int x = 0; x < lb->cx; x++) {
            uint32_t p = il_cell(lb, ib)[y * lb->cx + x];
            int tx = x + dx, ty = y + dy;
            if (p >> 24 && tx >= 0 && ty >= 0 && tx < n->cx && ty < n->cy)
                out[ty * n->cx + tx] = p;
        }
    return n;
}

/* the drag image follows nothing on screen; the calls keep their state for the program */
static BOOL WINAPI ImageList_BeginDrag(void *h, int i, int x, int y)
{
    UNUSED(x);
    UNUSED(y);
    struct image_list *il = il_of(h);
    if (!il || i < 0 || i >= il->count)
        return FALSE;
    drag_list = il;
    return TRUE;
}

static void WINAPI ImageList_EndDrag(void)
{
    drag_list = NULL;
}

static void *WINAPI ImageList_GetDragImage(POINT *pt, POINT *hot)
{
    if (pt)
        *pt = (POINT){0, 0};
    if (hot)
        *hot = (POINT){0, 0};
    return drag_list;
}

/* ImageList_Read/Write: the ILHEAD record and BMP images that Windows stores in resources and forms */
/* IStream::Read/Write on the caller's object: a 64-bit program's has a table of 64-bit pointers */
static uint32_t stream_call(void *stream, int slot, const void *buf, uint32_t n, uint32_t *done)
{
#ifdef __x86_64__
    if (w32->win64) {
        extern uint64_t w64_call(uint64_t fn, int argc, const uint64_t *args);
        const uint64_t *vt = *(const uint64_t **)stream;
        uint64_t args[4] = {(uintptr_t)stream, (uintptr_t)buf, n, (uintptr_t)done};
        return (uint32_t)w64_call(vt[slot], 4, args);
    }
#endif
    void **vt = *(void ***)stream;
    uint32_t args[4] = {(uint32_t)(uintptr_t)stream, (uint32_t)(uintptr_t)buf, n, (uint32_t)(uintptr_t)done};
    return w32_call(vt[slot], 4, args);
}

static bool istream_read(void *stream, void *buf, uint32_t n)
{
    uint32_t *got = w32_alloc(4);
    if (!got)
        return false;
    *got = 0;
    bool ok = stream_call(stream, 3, buf, n, got) == 0 && *got == n;
    w32_free(got);
    return ok;
}

static bool istream_write(void *stream, const void *buf, uint32_t n)
{
    return stream_call(stream, 4, buf, n, NULL) == 0;
}

static uint8_t *read_bmp(void *stream, struct bitmap_info **info)
{
    uint8_t *head = w32_alloc(14 + 40);
    if (!head || !istream_read(stream, head, 14 + 40) || head[0] != 'B' || head[1] != 'M') {
        w32_free(head);
        return NULL;
    }
    struct bitmap_info bi;
    memcpy(&bi, head + 14, sizeof(bi));
    w32_free(head);
    if (bi.size < 40 || bi.width <= 0 || !bi.height || bi.bpp == 0 || bi.bpp > 32 || bi.compression)
        return NULL;
    uint32_t colors = bi.bpp <= 8 ? (bi.used ? bi.used : 1u << bi.bpp) : 0;
    uint32_t extra = bi.size - 40, rows = (uint32_t)(bi.height < 0 ? -bi.height : bi.height);
    uint32_t stride = ((uint32_t)bi.width * bi.bpp + 31) / 32 * 4, bytes = stride * rows;
    uint8_t *blk = w32_alloc(40 + 256 * 4 + bytes + extra);
    if (!blk)
        return NULL;
    memcpy(blk, &bi, sizeof(bi));
    ((struct bitmap_info *)blk)->size = 40;
    if ((extra && !istream_read(stream, blk + 40 + 256 * 4, extra)) ||
        (colors && !istream_read(stream, blk + 40, colors * 4)) || !istream_read(stream, blk + 40 + 256 * 4, bytes)) {
        w32_free(blk);
        return NULL;
    }
    if (colors)
        memmove(blk + 40 + colors * 4, blk + 40 + 256 * 4, bytes);
    *info = (struct bitmap_info *)blk;
    return blk + 40 + colors * 4;
}

static void *WINAPI ImageList_Read(void *stream)
{
    if (!stream)
        return NULL;
    uint8_t *head = w32_alloc(28);
    if (!head || !istream_read(stream, head, 28) || head[0] != 'I' || head[1] != 'L') {
        w32_free(head);
        return NULL;
    }
    WORD cur, max, grow, cx, cy, flags;
    COLORREF bk;
    memcpy(&cur, head + 4, 2);
    memcpy(&max, head + 6, 2);
    memcpy(&grow, head + 8, 2);
    memcpy(&cx, head + 10, 2);
    memcpy(&cy, head + 12, 2);
    memcpy(&bk, head + 14, 4);
    memcpy(&flags, head + 18, 2);
    short ovls[4];
    memcpy(ovls, head + 20, 8);
    w32_free(head);
    struct image_list *il = ImageList_Create(cx, cy, flags, MAX(cur, max), grow);
    if (!il)
        return NULL;
    il->bk = bk;
    struct bitmap_info *ii = NULL, *mi = NULL;
    uint8_t *ib = read_bmp(stream, &ii), *mb = ib && (flags & 1) ? read_bmp(stream, &mi) : NULL;
    if (!ib || !il_reserve(il, cur)) {
        w32_free(ii);
        ImageList_Destroy(il);
        return NULL;
    }
    bool alpha = false;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < cur; i++) {
            uint32_t *cell = il_cell(il, i);
            int ox = (i % 4) * cx, oy = (i / 4) * cy;
            for (int y = 0; y < cy; y++)
                for (int x = 0; x < cx; x++) {
                    int h = ii->height < 0 ? -ii->height : ii->height;
                    if (ox + x >= ii->width || oy + y >= h)
                        continue;
                    uint32_t p = dib_pixel(ii, ib, ox + x, oy + y);
                    if (ii->bpp == 32) {
                        int row = ii->height < 0 ? oy + y : h - 1 - (oy + y);
                        p = ((const uint32_t *)ib)[(size_t)row * (size_t)ii->width + (size_t)(ox + x)];
                    }
                    if (pass == 0) {
                        alpha |= ii->bpp == 32 && (p >> 24) != 0;
                        continue;
                    }
                    bool clear = mb && (dib_pixel(mi, mb, ox + x, oy + y) & 0xffffff);
                    cell[y * cx + x] = clear ? 0 : alpha ? p : (p & 0xffffff) | 0xff000000u;
                }
        }
    il->count = cur;
    for (int i = 0; i < 4; i++)
        if (ovls[i] >= 0)
            il->overlay[i + 1] = ovls[i] + 1;
    w32_free(ii);
    w32_free(mi);
    return il;
}

static BOOL WINAPI ImageList_Write(void *h, void *stream)
{
    struct image_list *il = il_of(h);
    if (!il || !stream)
        return FALSE;
    int rows = MAX((il->count + 3) / 4, 1), w = il->cx * 4, hgt = il->cy * rows;
    uint32_t bytes = (uint32_t)w * (uint32_t)hgt * 4;
    uint8_t *buf = w32_zalloc(28 + 54 + bytes);
    if (!buf)
        return FALSE;
    WORD head[7] = {0x4c49, 0x101, (WORD)il->count, (WORD)il->cap, (WORD)il->grow, (WORD)il->cx, (WORD)il->cy};
    memcpy(buf, head, 14);
    memcpy(buf + 14, &il->bk, 4);
    WORD flags = (WORD)((il->flags & ~1u) | 0x20);
    memcpy(buf + 18, &flags, 2);
    for (int i = 0; i < 4; i++) {
        short v = (short)(il->overlay[i + 1] - 1);
        memcpy(buf + 20 + i * 2, &v, 2);
    }
    uint8_t *bmp = buf + 28;
    uint32_t file = 54 + bytes, off = 54;
    bmp[0] = 'B';
    bmp[1] = 'M';
    memcpy(bmp + 2, &file, 4);
    memcpy(bmp + 10, &off, 4);
    struct bitmap_info bi = {40, w, hgt, 1, 32, 0, bytes, 0, 0, 0, 0};
    memcpy(bmp + 14, &bi, 40);
    uint32_t *px = (uint32_t *)(bmp + 54);
    for (int i = 0; i < il->count; i++)
        for (int y = 0; y < il->cy; y++)
            for (int x = 0; x < il->cx; x++) {
                int ty = (i / 4) * il->cy + y, tx = (i % 4) * il->cx + x;
                px[(size_t)(hgt - 1 - ty) * (size_t)w + (size_t)tx] = il_cell(il, i)[y * il->cx + x];
            }
    BOOL ok = istream_write(stream, buf, 28 + 54 + bytes);
    w32_free(buf);
    return ok;
}

static void *WINAPI ImageList_LoadImageW(HANDLE inst, const void *name, int cx, int grow, COLORREF mask, UINT type,
                                         UINT flags)
{
    UNUSED(flags);
    if (type != 0)
        return NULL;
    uint32_t size = 0;
    const void *dib = w32_find_resource(inst, (const void *)2, name, &size);
    struct gdi_obj *b = dib ? w32_bitmap_from_dib(dib, size) : NULL;
    if (!b)
        return NULL;
    struct image_list *il = ImageList_Create(cx > 0 ? cx : b->canvas.h, b->canvas.h, 0x21, 4, grow);
    if (il && (mask == CLR_NONE ? ImageList_Add(il, b, NULL) : ImageList_AddMasked(il, b, mask)) < 0) {
        ImageList_Destroy(il);
        il = NULL;
    }
    DeleteObject(b);
    return il;
}

static const struct w32_export gdi_exports[] = {
    W32_FN(AddFontMemResourceEx), W32_AS("RemoveFontMemResourceEx", w32_one1),
    W32_FN(GetStockObject), W32_FN(CreateSolidBrush), W32_FN(CreateHatchBrush), W32_FN(CreateBrushIndirect),
    W32_FN(CreatePen), W32_FN(CreatePenIndirect), W32_FN(ExtCreatePen), W32_FN(CreateFontIndirectA),
    W32_FN(CreateFontIndirectW), W32_FN(CreateFontA), W32_FN(CreateFontW), W32_FN(SelectObject), W32_FN(DeleteObject),
    W32_FN(GetObjectType), W32_FN(GetObjectA), W32_FN(GetObjectW), W32_FN(SetTextColor),
    W32_FN(GetTextColor), W32_FN(SetBkColor), W32_FN(GetBkColor), W32_FN(SetBkMode), W32_FN(GetBkMode),
    W32_FN(SetDCBrushColor), W32_FN(SetDCPenColor), W32_FN(SetROP2), W32_FN(SetTextAlign), W32_FN(SetMapMode), W32_FN(GetMapMode), W32_FN(GetStretchBltMode),
    W32_FN(SetWindowExtEx), W32_AS("SetViewportExtEx", SetWindowExtEx), W32_FN(PolyPolyline), W32_FN(LPtoDP), W32_FN(DPtoLP), W32_FN(GetNearestColor),
    W32_FN(OffsetClipRgn), W32_FN(CreatePatternBrush), W32_FN(PolyBezier), W32_AS("AngleArc", w32_one6),
    W32_AS("ArcTo", w32_one9), W32_AS("PathToRegion", w32_zero1), W32_AS("CopyEnhMetaFileW", w32_zero2),
    W32_AS("GetEnhMetaFileDescriptionW", w32_zero3), W32_AS("GetEnhMetaFilePaletteEntries", w32_zero3),
    W32_AS("GetWinMetaFileBits", w32_zero5), W32_AS("SetWinMetaFileBits", w32_zero4),
    W32_FN(SetStretchBltMode), W32_FN(SetGraphicsMode), W32_FN(SetViewportOrgEx), W32_FN(SetWindowOrgEx), W32_FN(OffsetWindowOrgEx), W32_FN(CreateRoundRectRgn), W32_FN(OffsetRgn),
    W32_FN(FillRgn), W32_FN(FrameRgn),
    W32_FN(OffsetViewportOrgEx), W32_FN(SaveDC), W32_FN(RestoreDC), W32_FN(CreateRectRgn),
    W32_FN(CreateRectRgnIndirect), W32_FN(SelectClipRgn), W32_FN(SetRectRgn), W32_FN(CombineRgn), W32_FN(GetRgnBox),
    W32_FN(EqualRgn), W32_FN(GetRegionData), W32_FN(GetClipRgn), W32_FN(GetCurrentObject), W32_FN(GetWorldTransform),
    W32_AS("SetWorldTransform", w32_one2), W32_AS("ModifyWorldTransform", w32_one3), W32_FN(SetBrushOrgEx),
    W32_FN(GetCharWidthW), W32_AS("GetCharWidth32W", GetCharWidthW), W32_FN(GetCharWidthA), W32_AS("GetCharWidth32A", GetCharWidthA),
    W32_FN(GetCharABCWidthsW), W32_FN(GetCharABCWidthsA), W32_FN(GetCharABCWidthsFloatW), W32_FN(GetCharABCWidthsFloatA),
    W32_FN(GetTextExtentExPointW), W32_FN(GetTextExtentExPointA), W32_FN(GetCharacterPlacementW), W32_FN(SetLayout),
    W32_AS("GetLayout", w32_zero1), W32_AS("UpdateColors", w32_one1), W32_FN(GdiAlphaBlend), W32_FN(SetDIBits),
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
    W32_FN(EnumFontFamiliesExA), W32_FN(EnumFontFamiliesExW), W32_FN(EnumFontsA), W32_FN(EnumFontsW),
    W32_FN(EnumFontFamiliesA), W32_FN(EnumFontFamiliesW), W32_FN(AddFontResourceW), W32_FN(AddFontResourceA),
    W32_FN(AddFontResourceExW), W32_AS("AddFontResourceExA", AddFontResourceExW), W32_FN(RemoveFontResourceW),
    W32_AS("RemoveFontResourceA", RemoveFontResourceW), W32_AS("RemoveFontResourceExW", w32_one3),
    W32_FN(CreatePalette), W32_FN(CreateHalftonePalette), W32_FN(SelectPalette), W32_FN(RealizePalette),
    W32_FN(GetPaletteEntries), W32_FN(SetPaletteEntries), W32_FN(ResizePalette), W32_FN(GetNearestPaletteIndex),
    W32_AS("AnimatePalette", SetPaletteEntries), W32_FN(GetSystemPaletteEntries), W32_FN(UnrealizeObject),
    W32_FN(GetDIBColorTable), W32_FN(SetDIBColorTable), W32_FN(GetBitmapBits), W32_FN(SetBitmapBits),
    W32_FN(GetCurrentPositionEx), W32_FN(GetWindowOrgEx), W32_FN(GetViewportOrgEx), W32_FN(GetDCOrgEx),
    W32_FN(ExcludeClipRect), W32_FN(RectVisible), W32_FN(PtVisible), W32_FN(ExtFloodFill), W32_FN(FloodFill),
    W32_FN(LineDDA), W32_FN(MaskBlt), W32_FN(GetBrushOrgEx),
};

static const struct w32_export gdi_user_exports[] = {
    W32_FN(FillRect),    W32_FN(FrameRect), W32_FN(InvertRect),  W32_FN(GetSysColor),   W32_FN(GetSysColorBrush),
    W32_FN(DrawTextA),   W32_FN(DrawTextW), W32_FN(DrawTextExW), W32_FN(DrawFocusRect), W32_FN(DrawFrameControl), W32_FN(DrawEdge),
};

static const struct w32_export msimg_exports[] = {
    W32_FN(TransparentBlt),
    W32_AS("AlphaBlend", GdiAlphaBlend),
};

W32_EXPORTS(gdi_module, "gdi32.dll", gdi_exports, .attach = w32_gdi_reset);
W32_EXPORTS(gdi_user_module, "user32.dll", gdi_user_exports);
W32_EXPORTS(msimg_module, "msimg32.dll", msimg_exports);

static const struct w32_export imagelist_exports[] = {
    W32_FN(ImageList_Create), W32_FN(ImageList_Destroy), W32_FN(ImageList_GetImageCount),
    W32_FN(ImageList_SetImageCount), W32_FN(ImageList_GetIconSize), W32_FN(ImageList_SetIconSize),
    W32_FN(ImageList_SetBkColor), W32_FN(ImageList_GetBkColor), W32_FN(ImageList_Add), W32_FN(ImageList_AddMasked),
    W32_FN(ImageList_Replace), W32_FN(ImageList_ReplaceIcon), W32_FN(ImageList_Remove), W32_FN(ImageList_Copy),
    W32_FN(ImageList_Duplicate), W32_FN(ImageList_SetOverlayImage), W32_FN(ImageList_Draw), W32_FN(ImageList_DrawEx),
    W32_FN(ImageList_DrawIndirect), W32_FN(ImageList_GetImageInfo), W32_FN(ImageList_GetIcon), W32_FN(ImageList_Merge),
    W32_FN(ImageList_BeginDrag), W32_FN(ImageList_EndDrag), W32_FN(ImageList_GetDragImage),
    W32_AS("ImageList_DragEnter", w32_one3), W32_AS("ImageList_DragLeave", w32_one1),
    W32_AS("ImageList_DragMove", w32_one2), W32_AS("ImageList_DragShowNolock", w32_one1),
    W32_AS("ImageList_SetDragCursorImage", w32_one4), W32_FN(ImageList_Read), W32_FN(ImageList_Write),
    W32_FN(ImageList_LoadImageW),
};

W32_EXPORTS(imagelist_module, "comctl32.dll", imagelist_exports);
#endif
