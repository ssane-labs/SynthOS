#include <synth/codepage.h>
#include <synth/font.h>
#include <synth/gui.h>
#include <synth/heap.h>
#include <synth/string.h>

struct rect rect_make(int x, int y, int w, int h)
{
    return (struct rect){x, y, w, h};
}

bool rect_empty(struct rect r)
{
    return r.w <= 0 || r.h <= 0;
}

bool rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    return x1 > x0 && y1 > y0 ? (struct rect){x0, y0, x1 - x0, y1 - y0} : (struct rect){0, 0, 0, 0};
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

struct rect rect_inset(struct rect r, int d)
{
    return (struct rect){r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d};
}

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
    a += a >> 7;
    uint32_t rb = ((src & 0xff00ffu) * a + (dst & 0xff00ffu) * (256 - a)) >> 8 & 0xff00ffu;
    uint32_t g = ((src & 0x00ff00u) * a + (dst & 0x00ff00u) * (256 - a)) >> 8 & 0x00ff00u;
    return 0xff000000u | rb | g;
}

uint32_t color_mix(uint32_t a, uint32_t b, int t)
{
    return blend(a, b, (unsigned)CLAMP(t, 0, 255));
}

uint32_t color_shade(uint32_t c, int amount)
{
    return amount >= 0 ? blend(c, 0xffffffffu, (unsigned)MIN(amount, 255))
                       : blend(c, 0xff000000u, (unsigned)MIN(-amount, 255));
}

struct canvas canvas_make(uint32_t *px, int w, int h)
{
    return (struct canvas){px, w, h, w, {0, 0, w, h}};
}

bool canvas_alloc(struct canvas *c, int w, int h)
{
    uint32_t *px = kmalloc((size_t)MAX(w, 1) * (size_t)MAX(h, 1) * 4);
    if (!px)
        return false;
    *c = canvas_make(px, w, h);
    return true;
}

void canvas_free(struct canvas *c)
{
    kfree(c->px);
    c->px = NULL;
}

void canvas_clip(struct canvas *c, struct rect r)
{
    c->clip = rect_intersect(r, (struct rect){0, 0, c->w, c->h});
}

static inline void put(struct canvas *c, int x, int y, uint32_t color)
{
    uint32_t a = color >> 24;
    uint32_t *p = &c->px[y * c->stride + x];
    *p = a == 255 ? color : blend(*p, color, a);
}

void gfx_pixel(struct canvas *c, int x, int y, uint32_t color)
{
    if (rect_contains(c->clip, x, y))
        put(c, x, y, color);
}

void gfx_fill(struct canvas *c, struct rect r, uint32_t color)
{
    r = rect_intersect(r, c->clip);
    if (rect_empty(r) || !(color >> 24))
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->px + y * c->stride + r.x;
        if (color >> 24 == 255) {
            for (int x = 0; x < r.w; x++)
                row[x] = color;
        } else {
            for (int x = 0; x < r.w; x++)
                row[x] = blend(row[x], color, color >> 24);
        }
    }
}

void gfx_frame(struct canvas *c, struct rect r, uint32_t color)
{
    gfx_fill(c, (struct rect){r.x, r.y, r.w, 1}, color);
    gfx_fill(c, (struct rect){r.x, r.y + r.h - 1, r.w, 1}, color);
    gfx_fill(c, (struct rect){r.x, r.y + 1, 1, r.h - 2}, color);
    gfx_fill(c, (struct rect){r.x + r.w - 1, r.y + 1, 1, r.h - 2}, color);
}

static uint32_t lerp_color(uint32_t a, uint32_t b, int num, int den)
{
    if (den <= 0)
        return a;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        int ca = (int)(a >> shift & 0xff), cb = (int)(b >> shift & 0xff);
        out |= (uint32_t)(ca + (cb - ca) * num / den) << shift;
    }
    return out;
}

void gfx_gradient(struct canvas *c, struct rect r, uint32_t top, uint32_t bottom)
{
    for (int y = 0; y < r.h; y++)
        gfx_fill(c, (struct rect){r.x, r.y + y, r.w, 1}, lerp_color(top, bottom, y, r.h - 1));
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
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

static int corner_coverage(struct rect r, int radius, int x, int y)
{
    int cx, cy;
    if (x < r.x + radius)
        cx = r.x + radius;
    else if (x >= r.x + r.w - radius)
        cx = r.x + r.w - radius;
    else
        return 256;
    if (y < r.y + radius)
        cy = r.y + radius;
    else if (y >= r.y + r.h - radius)
        cy = r.y + r.h - radius;
    else
        return 256;
    int dx = x * 16 + 8 - cx * 16, dy = y * 16 + 8 - cy * 16;
    int d = (int)isqrt((uint32_t)(dx * dx + dy * dy));
    return CLAMP(radius * 16 - d + 8, 0, 16) * 16;
}

static void round_rows(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom, bool outline)
{
    radius = MIN(radius, MIN(r.w, r.h) / 2);
    struct rect area = rect_intersect(r, c->clip);
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t color = top == bottom ? top : lerp_color(top, bottom, y - r.y, r.h - 1);
        unsigned alpha = color >> 24;
        bool in_corner_rows = y < r.y + radius || y >= r.y + r.h - radius;
        for (int x = area.x; x < area.x + area.w; x++) {
            if (outline) {
                bool edge = y == r.y || y == r.y + r.h - 1 || x == r.x || x == r.x + r.w - 1;
                if (!edge && !in_corner_rows && x >= r.x + radius && x < r.x + r.w - radius)
                    continue;
                int outer = corner_coverage(r, radius, x, y);
                int inner = edge ? 0 : corner_coverage(rect_inset(r, 1), MAX(radius - 1, 0), x, y);
                int cov = outer - inner;
                if (cov > 0)
                    put(c, x, y, ALPHA(color, alpha * (unsigned)MIN(cov, 256) >> 8));
                continue;
            }
            int cov = in_corner_rows || x < r.x + radius || x >= r.x + r.w - radius ? corner_coverage(r, radius, x, y)
                                                                                     : 256;
            if (cov >= 256)
                put(c, x, y, color);
            else if (cov > 0)
                put(c, x, y, ALPHA(color, alpha * (unsigned)cov >> 8));
        }
    }
}

void gfx_round(struct canvas *c, struct rect r, int radius, uint32_t color)
{
    if (radius <= 0) {
        gfx_fill(c, r, color);
        return;
    }
    round_rows(c, r, radius, color, color, false);
}

void gfx_round_gradient(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom)
{
    round_rows(c, r, radius, top, bottom, false);
}

void gfx_round_frame(struct canvas *c, struct rect r, int radius, uint32_t color)
{
    if (radius <= 0) {
        gfx_frame(c, r, color);
        return;
    }
    round_rows(c, r, radius, color, color, true);
}

void gfx_shadow(struct canvas *c, struct rect r, int radius, int size, int strength)
{
    struct rect outer = {r.x - size, r.y - size + size / 3, r.w + 2 * size, r.h + 2 * size};
    struct rect area = rect_intersect(outer, c->clip);
    for (int y = area.y; y < area.y + area.h; y++)
        for (int x = area.x; x < area.x + area.w; x++) {
            int sy = y - size / 3;
            int dx = x < r.x + radius ? r.x + radius - x : x >= r.x + r.w - radius ? x - (r.x + r.w - radius - 1) : 0;
            int dy = sy < r.y + radius ? r.y + radius - sy : sy >= r.y + r.h - radius ? sy - (r.y + r.h - radius - 1) : 0;
            int d = (int)isqrt((uint32_t)(dx * dx + dy * dy)) - radius;
            if (d <= 0) {
                if (rect_contains(r, x, y))
                    continue;
                d = 0;
            }
            if (d >= size)
                continue;
            int t = size - d;
            put(c, x, y, ALPHA(0, (unsigned)(strength * t * t / (size * size))));
        }
}

void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
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
}

void gfx_circle(struct canvas *c, int cx, int cy, int r, uint32_t color)
{
    if (r <= 0) {
        gfx_pixel(c, cx, cy, color);
        return;
    }
    round_rows(c, (struct rect){cx - r, cy - r, 2 * r + 1, 2 * r + 1}, r, color, color, false);
}

void gfx_thick_line(struct canvas *c, int x0, int y0, int x1, int y1, int width, uint32_t color)
{
    if (width <= 1) {
        gfx_line(c, x0, y0, x1, y1, color);
        return;
    }
    int dx = x1 - x0, dy = y1 - y0;
    int steps = MAX(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
    for (int i = 0; i <= steps; i++) {
        int x = steps ? x0 + dx * i / steps : x0, y = steps ? y0 + dy * i / steps : y0;
        gfx_fill(c, (struct rect){x - width / 2, y - width / 2, width, width}, color);
    }
}

void gfx_blit(struct canvas *c, int x, int y, const struct canvas *src, struct rect from)
{
    struct rect dst = rect_intersect((struct rect){x, y, from.w, from.h}, c->clip);
    for (int row = 0; row < dst.h; row++) {
        const uint32_t *s = src->px + (from.y + dst.y - y + row) * src->stride + from.x + dst.x - x;
        memcpy(c->px + (dst.y + row) * c->stride + dst.x, s, (size_t)dst.w * 4);
    }
}

void gfx_image(struct canvas *c, int x, int y, const uint32_t *argb, int w, int h)
{
    struct rect dst = rect_intersect((struct rect){x, y, w, h}, c->clip);
    for (int row = dst.y; row < dst.y + dst.h; row++)
        for (int col = dst.x; col < dst.x + dst.w; col++) {
            uint32_t p = argb[(row - y) * w + (col - x)];
            if (p >> 24)
                put(c, col, row, p);
        }
}

void gfx_icon(struct canvas *c, int x, int y, enum icon_id id, int size)
{
    if ((unsigned)id >= ICON_COUNT)
        return;
    if (size <= 16)
        gfx_image(c, x, y, icons[id].small, 16, 16);
    else
        gfx_image(c, x, y, icons[id].large, 32, 32);
}

uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p;
    if (c < 0x80) {
        *s += 1;
        return c;
    }
    int extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
    uint32_t cp = c & (0x3fu >> extra);
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            extra = 0;
            break;
        }
        cp = cp << 6 | (p[i] & 0x3f);
    }
    if (!extra) {
        *s += 1;
        return codepage_to_unicode((uint8_t)c);
    }
    *s += extra + 1;
    return cp;
}

static const struct ui_glyph *find_glyph(const struct ui_font *f, uint32_t code)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code)
            return &f->glyphs[mid];
        if (f->glyphs[mid].code < code)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return code == '?' ? NULL : find_glyph(f, '?');
}

static void draw_glyph(struct canvas *c, int x, int y, const struct ui_font *f, const struct ui_glyph *g,
                       uint32_t color)
{
    const uint8_t *data = f->alpha + g->offset;
    int stride = (g->w + 1) / 2;
    unsigned base = color >> 24;
    for (int row = 0; row < g->h; row++) {
        int py = y + g->y + row;
        if (py < c->clip.y || py >= c->clip.y + c->clip.h)
            continue;
        for (int col = 0; col < g->w; col++) {
            int px = x + g->x + col;
            if (px < c->clip.x || px >= c->clip.x + c->clip.w)
                continue;
            uint8_t nib = data[row * stride + col / 2];
            unsigned a = col & 1 ? nib & 0x0f : nib >> 4;
            if (a)
                put(c, px, py, ALPHA(color, base * a * 17 / 255));
        }
    }
}

int gfx_text_n(struct canvas *c, int x, int y, const struct ui_font *f, const char *s, size_t n, uint32_t color)
{
    int start = x;
    const char *end = s + n;
    while (s < end && *s) {
        const struct ui_glyph *g = find_glyph(f, utf8_next(&s));
        if (!g)
            continue;
        if (c)
            draw_glyph(c, x, y, f, g, color);
        x += g->advance;
    }
    return x - start;
}

int gfx_text(struct canvas *c, int x, int y, const struct ui_font *f, const char *s, uint32_t color)
{
    return gfx_text_n(c, x, y, f, s, strlen(s), color);
}

int gfx_text_width_n(const struct ui_font *f, const char *s, size_t n)
{
    return gfx_text_n(NULL, 0, 0, f, s, n, 0);
}

int gfx_text_width(const struct ui_font *f, const char *s)
{
    return gfx_text_width_n(f, s, strlen(s));
}

void gfx_text_box(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color,
                  enum text_align align)
{
    size_t n = strlen(s);
    int width = gfx_text_width_n(f, s, n);
    int dots = gfx_text_width(f, "…");
    bool cut = width > r.w;
    if (cut) {
        while (n > 0 && gfx_text_width_n(f, s, n) + dots > r.w)
            n--;
        while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80)
            n--;
        width = gfx_text_width_n(f, s, n) + dots;
    }
    int x = align == ALIGN_CENTER ? r.x + (r.w - width) / 2 : align == ALIGN_RIGHT ? r.x + r.w - width : r.x;
    int y = r.y + (r.h - f->height) / 2;
    struct rect saved = c->clip;
    c->clip = rect_intersect(c->clip, r);
    x += gfx_text_n(c, x, y, f, s, n, color);
    if (cut)
        gfx_text(c, x, y, f, "…", color);
    c->clip = saved;
}

void gfx_cell(struct canvas *c, int x, int y, uint8_t ch, uint32_t fg, uint32_t bg)
{
    const uint8_t(*font)[16] = font_for_codepage(codepage_current());
    if (x < c->clip.x || y < c->clip.y || x + FONT_WIDTH > c->clip.x + c->clip.w ||
        y + FONT_HEIGHT > c->clip.y + c->clip.h) {
        for (int row = 0; row < FONT_HEIGHT; row++)
            for (int col = 0; col < FONT_WIDTH; col++)
                gfx_pixel(c, x + col, y + row, font[ch][row] & (0x80 >> col) ? fg : bg);
        return;
    }
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint32_t *line = c->px + (y + row) * c->stride + x;
        uint8_t bits = font[ch][row];
        for (int col = 0; col < FONT_WIDTH; col++)
            line[col] = bits & (0x80 >> col) ? fg : bg;
    }
}
