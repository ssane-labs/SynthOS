#include <synth/codepage.h>
#include <synth/font.h>
#include <synth/gui.h>
#include <synth/heap.h>
#include <synth/lz4.h>
#include <synth/string.h>

#include "internal.h"

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

bool rect_inside(struct rect inner, struct rect outer)
{
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
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

static inline uint32_t over(uint32_t dst, uint32_t src, unsigned a)
{
    unsigned da = dst >> 24;
    if (da == 255 || a == 255)
        return blend(dst, src, a);
    if (!a)
        return dst;
    unsigned wd = da * (255 - a) / 255;
    unsigned oa = a + wd;
    if (!oa)
        return 0;
    uint32_t out = oa << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned s = src >> shift & 0xff, d = dst >> shift & 0xff;
        out |= ((s * a + d * wd) / oa) << shift;
    }
    return out;
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

int color_luma(uint32_t c)
{
    return (int)((c >> 16 & 0xff) * 3 + (c >> 8 & 0xff) * 6 + (c & 0xff)) / 10;
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
    *p = a == 255 ? color : over(*p, color, a);
}

static inline void put_a(struct canvas *c, int x, int y, uint32_t color, unsigned a)
{
    if (!a)
        return;
    uint32_t *p = &c->px[y * c->stride + x];
    *p = a >= 255 ? (color | 0xff000000u) : over(*p, color, a);
}

void gfx_pixel(struct canvas *c, int x, int y, uint32_t color)
{
    if (rect_contains(c->clip, x, y))
        put(c, x, y, color);
}

void gfx_fill(struct canvas *c, struct rect r, uint32_t color)
{
    r = rect_intersect(r, c->clip);
    unsigned a = color >> 24;
    if (rect_empty(r) || !a)
        return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->px + y * c->stride + r.x;
        if (a == 255) {
            for (int x = 0; x < r.w; x++)
                row[x] = color;
        } else {
            for (int x = 0; x < r.w; x++)
                row[x] = (row[x] >> 24) == 255 ? blend(row[x], color, a) : over(row[x], color, a);
        }
    }
}

void gfx_clear(struct canvas *c, struct rect r, uint32_t color)
{
    r = rect_intersect(r, c->clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->px + y * c->stride + r.x;
        for (int x = 0; x < r.w; x++)
            row[x] = color;
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

int gfx_corner_coverage(struct rect r, int radius, int x, int y)
{
    return corner_coverage(r, radius, x, y);
}

enum round_mode {
    ROUND_FILL,
    ROUND_OUTLINE,
    ROUND_CLEAR,
};

static void round_rows(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom, enum round_mode mode)
{
    radius = MAX(0, MIN(radius, MIN(r.w, r.h) / 2));
    struct rect area = rect_intersect(r, c->clip);
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t color = top == bottom ? top : lerp_color(top, bottom, y - r.y, r.h - 1);
        unsigned alpha = color >> 24;
        bool in_corner_rows = y < r.y + radius || y >= r.y + r.h - radius;
        uint32_t *row = c->px + y * c->stride;
        for (int x = area.x; x < area.x + area.w; x++) {
            if (mode == ROUND_OUTLINE) {
                bool edge = y == r.y || y == r.y + r.h - 1 || x == r.x || x == r.x + r.w - 1;
                if (!edge && !in_corner_rows) {
                    if (x < r.x + r.w - 1)
                        x = r.x + r.w - 2;
                    continue;
                }
                if (!edge && x >= r.x + radius && x < r.x + r.w - radius)
                    continue;
                int outer = corner_coverage(r, radius, x, y);
                int inner = edge ? 0 : corner_coverage(rect_inset(r, 1), MAX(radius - 1, 0), x, y);
                int cov = outer - inner;
                if (cov > 0)
                    put_a(c, x, y, color, alpha * (unsigned)MIN(cov, 256) >> 8);
                continue;
            }
            int cov = in_corner_rows || x < r.x + radius || x >= r.x + r.w - radius ? corner_coverage(r, radius, x, y)
                                                                                     : 256;
            if (mode == ROUND_CLEAR) {
                if (cov >= 256)
                    row[x] = color;
                else if (cov > 0)
                    row[x] = over(row[x] & 0x00ffffffu, color, alpha * (unsigned)cov >> 8);
                else
                    row[x] = 0;
                continue;
            }
            if (cov >= 256)
                put(c, x, y, color);
            else if (cov > 0)
                put_a(c, x, y, color, alpha * (unsigned)cov >> 8);
        }
    }
}

void gfx_round(struct canvas *c, struct rect r, int radius, uint32_t color)
{
    if (radius <= 0) {
        gfx_fill(c, r, color);
        return;
    }
    round_rows(c, r, radius, color, color, ROUND_FILL);
}

void gfx_round_clear(struct canvas *c, struct rect r, int radius, uint32_t color)
{
    round_rows(c, r, radius, color, color, ROUND_CLEAR);
}

void gfx_round_gradient(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom)
{
    round_rows(c, r, radius, top, bottom, ROUND_FILL);
}

void gfx_round_frame(struct canvas *c, struct rect r, int radius, uint32_t color)
{
    if (radius <= 0) {
        gfx_frame(c, r, color);
        return;
    }
    round_rows(c, r, radius, color, color, ROUND_OUTLINE);
}

void gfx_round_frame_gradient(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom)
{
    round_rows(c, r, radius, top, bottom, ROUND_OUTLINE);
}

static uint8_t shadow_falloff[257];

static void shadow_init(void)
{
    if (shadow_falloff[0])
        return;
    for (int i = 0; i <= 256; i++) {
        int t = 256 - i;
        shadow_falloff[i] = (uint8_t)(t * t / 256 * (256 + t) / 512);
    }
    shadow_falloff[0] = 255;
}

struct shadow_table {
    int radius, size, strength, n;
    uint8_t *alpha;
};

static struct shadow_table shadow_tables[6];
static int shadow_next;

static const struct shadow_table *shadow_lookup(int radius, int size, int strength)
{
    for (int i = 0; i < (int)ARRAY_SIZE(shadow_tables); i++) {
        struct shadow_table *t = &shadow_tables[i];
        if (t->alpha && t->radius == radius && t->size == size && t->strength == strength)
            return t;
    }
    struct shadow_table *t = &shadow_tables[shadow_next++ % (int)ARRAY_SIZE(shadow_tables)];
    kfree(t->alpha);
    int n = radius + size + 1;
    t->alpha = kmalloc((size_t)n * (size_t)n);
    if (!t->alpha)
        return NULL;
    t->radius = radius;
    t->size = size;
    t->strength = strength;
    t->n = n;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int d16;
            if (!x && !y)
                d16 = 0;
            else if (!x)
                d16 = (y - radius) * 16;
            else if (!y)
                d16 = (x - radius) * 16;
            else
                d16 = (int)isqrt((uint32_t)((x * x + y * y) * 256)) - radius * 16;
            if (d16 < 0)
                d16 = 0;
            t->alpha[y * n + x] =
                d16 >= size * 16 ? 0 : (uint8_t)((unsigned)strength * shadow_falloff[d16 * 256 / (size * 16)] / 255);
        }
    return t;
}

static void shadow_impl(struct canvas *c, struct rect r, int radius, int size, int strength, int dy, uint32_t color);

void gfx_drop_shadow(struct canvas *c, struct rect r, int radius, int size, int strength, int dy)
{
    shadow_impl(c, r, radius, size, strength, dy, 0);
}

void gfx_glow(struct canvas *c, struct rect r, int radius, int size, int strength, uint32_t color)
{
    shadow_impl(c, r, radius, size, strength, 0, (color & 0xffffffu) | 0x01000000u);
}

static int isqrt_i(uint32_t v)
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
    return (int)r;
}

int gfx_dist16(int dx16, int dy16)
{
    return isqrt_i((uint32_t)(dx16 * dx16 + dy16 * dy16));
}

int gfx_cover(int d16)
{
    return CLAMP((d16 + 8) * 16, 0, 256);
}

void gfx_shape(struct canvas *c, struct rect bounds, gfx_cover_fn cover, const void *ctx, uint32_t color)
{
    struct rect area = rect_intersect(bounds, c->clip);
    unsigned alpha = color >> 24;
    for (int y = area.y; y < area.y + area.h; y++)
        for (int x = area.x; x < area.x + area.w; x++) {
            int cov = cover(x, y, ctx);
            if (cov <= 0)
                continue;
            uint32_t *p = &c->px[y * c->stride + x];
            *p = blend(*p, color, alpha * (unsigned)MIN(cov, 256) >> 8);
        }
}

static void shadow_impl(struct canvas *c, struct rect r, int radius, int size, int strength, int dy, uint32_t color)
{
    if (size <= 0 || strength <= 0)
        return;
    bool fill = color >> 24;
    color &= 0xffffffu;
    shadow_init();
    struct rect s = {r.x, r.y + dy, r.w, r.h};
    radius = MAX(0, MIN(radius, MIN(r.w, r.h) / 2));
    struct rect outer = {s.x - size, s.y - size, s.w + 2 * size, s.h + 2 * size};
    struct rect area = rect_intersect(outer, c->clip);
    if (rect_empty(area))
        return;
    const struct shadow_table *t = shadow_lookup(radius, size, MIN(strength, 255));
    if (!t)
        return;
    struct rect skip = rect_inset(r, radius);
    struct rect body = {r.x, r.y + radius, r.w, r.h - 2 * radius};
    int n = t->n;
    for (int y = area.y; y < area.y + area.h; y++) {
        int dy0 = y < s.y + radius ? s.y + radius - y : y >= s.y + s.h - radius ? y - (s.y + s.h - radius - 1) : 0;
        if (dy0 >= n)
            continue;
        const uint8_t *trow = t->alpha + dy0 * n;
        bool body_row = y >= body.y && y < body.y + body.h;
        int sx0 = body_row ? r.x : skip.x, sx1 = body_row ? r.x + r.w : skip.x + skip.w;
        bool skip_row = !fill && (body_row || (y >= r.y && y < r.y + r.h));
        uint32_t *row = c->px + y * c->stride;
        for (int x = area.x; x < area.x + area.w; x++) {
            if (skip_row && x >= sx0 && x < sx1) {
                x = sx1 - 1;
                continue;
            }
            int dx0 = x < s.x + radius ? s.x + radius - x : x >= s.x + s.w - radius ? x - (s.x + s.w - radius - 1) : 0;
            if (dx0 >= n)
                continue;
            unsigned a = trow[dx0];
            if (a)
                row[x] = over(row[x], color, a);
        }
    }
}

void gfx_shadow(struct canvas *c, struct rect r, int radius, int size, int strength)
{
    gfx_drop_shadow(c, r, radius, size, strength, size / 3);
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
    round_rows(c, (struct rect){cx - r, cy - r, 2 * r + 1, 2 * r + 1}, r, color, color, ROUND_FILL);
}

void gfx_ring(struct canvas *c, int cx, int cy, int r, uint32_t color)
{
    round_rows(c, (struct rect){cx - r, cy - r, 2 * r + 1, 2 * r + 1}, r, color, color, ROUND_OUTLINE);
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

void gfx_blit_alpha(struct canvas *c, int x, int y, const struct canvas *src, struct rect from, int alpha)
{
    struct rect dst = rect_intersect((struct rect){x, y, from.w, from.h}, c->clip);
    for (int row = 0; row < dst.h; row++) {
        const uint32_t *s = src->px + (from.y + dst.y - y + row) * src->stride + from.x + dst.x - x;
        uint32_t *d = c->px + (dst.y + row) * c->stride + dst.x;
        for (int i = 0; i < dst.w; i++) {
            unsigned a = (s[i] >> 24) * (unsigned)alpha / 255;
            if (a >= 255)
                d[i] = s[i];
            else if (a)
                d[i] = over(d[i], s[i], a);
        }
    }
}

void gfx_blit_round(struct canvas *c, struct rect dst, const struct canvas *src, int radius, bool blend_alpha)
{
    struct rect area = rect_intersect(dst, c->clip);
    radius = MIN(radius, MIN(dst.w, dst.h) / 2);
    for (int y = area.y; y < area.y + area.h; y++) {
        int sy = y - dst.y;
        if (sy >= src->h)
            break;
        const uint32_t *s = src->px + sy * src->stride - dst.x;
        uint32_t *d = c->px + y * c->stride;
        bool corner_row = y < dst.y + radius || y >= dst.y + dst.h - radius;
        int x0 = area.x, x1 = MIN(area.x + area.w, dst.x + src->w);
        for (int x = x0; x < x1; x++) {
            int cov = 256;
            if (corner_row && (x < dst.x + radius || x >= dst.x + dst.w - radius))
                cov = corner_coverage(dst, radius, x, y);
            else if (!blend_alpha && !corner_row) {
                int end = MIN(x1, dst.x + dst.w - radius);
                if (end > x) {
                    if (x >= dst.x + radius) {
                        memcpy(d + x, s + x, (size_t)(end - x) * 4);
                        x = end - 1;
                        continue;
                    }
                }
            }
            if (!cov)
                continue;
            uint32_t p = s[x];
            unsigned a = blend_alpha ? (p >> 24) : 255;
            a = a * (unsigned)cov >> 8;
            if (a >= 255)
                d[x] = p | 0xff000000u;
            else if (a)
                d[x] = over(d[x], p, a);
        }
    }
}

static uint32_t sample_bilinear(const uint32_t *px, int w, int h, int stride, int fx, int fy)
{
    int x0 = fx >> 8, y0 = fy >> 8, tx = fx & 255, ty = fy & 255;
    x0 = CLAMP(x0, 0, w - 1);
    y0 = CLAMP(y0, 0, h - 1);
    int x1 = MIN(x0 + 1, w - 1), y1 = MIN(y0 + 1, h - 1);
    uint32_t p[4] = {px[y0 * stride + x0], px[y0 * stride + x1], px[y1 * stride + x0], px[y1 * stride + x1]};
    int wgt[4] = {(256 - tx) * (256 - ty), tx * (256 - ty), (256 - tx) * ty, tx * ty};
    unsigned a = 0, r = 0, g = 0, b = 0;
    for (int i = 0; i < 4; i++) {
        unsigned pa = p[i] >> 24, wa = pa * (unsigned)wgt[i];
        a += wa;
        r += (p[i] >> 16 & 0xff) * wa / 256;
        g += (p[i] >> 8 & 0xff) * wa / 256;
        b += (p[i] & 0xff) * wa / 256;
    }
    if (!a)
        return 0;
    unsigned oa = a >> 16;
    unsigned div = MAX(a / 256, 1u);
    return oa << 24 | MIN(r / div, 255u) << 16 | MIN(g / div, 255u) << 8 | MIN(b / div, 255u);
}

static uint32_t sample_box(const uint32_t *px, int stride, int x0, int y0, int x1, int y1)
{
    unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t p = px[y * stride + x];
            unsigned pa = p >> 24;
            a += pa;
            r += (p >> 16 & 0xff) * pa;
            g += (p >> 8 & 0xff) * pa;
            b += (p & 0xff) * pa;
            n++;
        }
    if (!a || !n)
        return 0;
    return (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a);
}

static void draw_scaled(struct canvas *c, struct rect dst, const uint32_t *px, int w, int h, int stride, int alpha)
{
    if (dst.w <= 0 || dst.h <= 0)
        return;
    struct rect area = rect_intersect(dst, c->clip);
    bool shrink = dst.w * 2 <= w || dst.h * 2 <= h;
    for (int y = area.y; y < area.y + area.h; y++)
        for (int x = area.x; x < area.x + area.w; x++) {
            uint32_t p;
            if (shrink) {
                int sx0 = (x - dst.x) * w / dst.w, sx1 = MAX(sx0 + 1, (x - dst.x + 1) * w / dst.w);
                int sy0 = (y - dst.y) * h / dst.h, sy1 = MAX(sy0 + 1, (y - dst.y + 1) * h / dst.h);
                p = sample_box(px, stride, sx0, sy0, sx1, sy1);
            } else {
                int fx = ((x - dst.x) * 2 + 1) * w * 128 / dst.w - 128;
                int fy = ((y - dst.y) * 2 + 1) * h * 128 / dst.h - 128;
                p = sample_bilinear(px, w, h, stride, MAX(fx, 0), MAX(fy, 0));
            }
            unsigned a = (p >> 24) * (unsigned)alpha / 255;
            if (a)
                put_a(c, x, y, p, a);
        }
}

void gfx_scale(struct canvas *c, struct rect dst, const struct canvas *src, int alpha)
{
    draw_scaled(c, dst, src->px, src->w, src->h, src->stride, alpha);
}

void gfx_row_scaled(struct canvas *c, int y, int x0, int x1, const struct canvas *src, int sy, int alpha)
{
    if (y < c->clip.y || y >= c->clip.y + c->clip.h || x1 <= x0 || sy < 0 || sy >= src->h || alpha <= 0)
        return;
    int a0 = MAX(x0, c->clip.x), a1 = MIN(x1, c->clip.x + c->clip.w);
    if (a1 <= a0)
        return;
    uint32_t step = (uint32_t)(((uint64_t)src->w << 16) / (uint32_t)(x1 - x0));
    uint32_t fx = step * (uint32_t)(a0 - x0) + step / 2;
    const uint32_t *s = src->px + sy * src->stride;
    uint32_t *d = c->px + y * c->stride;
    for (int x = a0; x < a1; x++, fx += step) {
        uint32_t p = s[MIN((int)(fx >> 16), src->w - 1)];
        unsigned a = (p >> 24) * (unsigned)alpha / 255;
        if (a >= 255)
            d[x] = p;
        else if (a)
            d[x] = over(d[x], p, a);
    }
}

void gfx_scale_fast(struct canvas *c, struct rect dst, const struct canvas *src, int alpha)
{
    if (dst.w <= 0 || dst.h <= 0)
        return;
    struct rect area = rect_intersect(dst, c->clip);
    uint32_t step = (uint32_t)(((uint64_t)src->h << 16) / (uint32_t)dst.h);
    for (int y = area.y; y < area.y + area.h; y++) {
        int sy = (int)((step * (uint32_t)(y - dst.y) + step / 2) >> 16);
        gfx_row_scaled(c, y, dst.x, dst.x + dst.w, src, MIN(sy, src->h - 1), alpha);
    }
}

static inline uint32_t mix_px(uint32_t a, uint32_t b, unsigned f)
{
    uint32_t rb = ((a & 0xff00ffu) * (256 - f) + (b & 0xff00ffu) * f) >> 8 & 0xff00ffu;
    uint32_t ag = ((a >> 8 & 0xff00ffu) * (256 - f) + (b >> 8 & 0xff00ffu) * f) & 0xff00ff00u;
    return rb | ag;
}

#define SMOOTH_MAX 4096

/* Bilinear scaling for animations: the picture moves without the shimmer of dropped rows and columns. */
void gfx_scale_smooth(struct canvas *c, struct rect dst, const struct canvas *src, int alpha)
{
    static uint16_t col_i[SMOOTH_MAX];
    static uint8_t col_f[SMOOTH_MAX];
    if (dst.w <= 0 || dst.h <= 0 || alpha <= 0)
        return;
    if (dst.w > SMOOTH_MAX || src->w > SMOOTH_MAX || src->w < 2 || src->h < 2 ||
        (dst.w == src->w && dst.h == src->h)) {
        gfx_scale_fast(c, dst, src, alpha);
        return;
    }
    struct rect area = rect_intersect(dst, c->clip);
    if (rect_empty(area))
        return;
    int32_t sx = (int32_t)(((int64_t)src->w << 16) / dst.w), sy = (int32_t)(((int64_t)src->h << 16) / dst.h);
    int32_t xmax = (src->w - 1) << 16, ymax = (src->h - 1) << 16;
    for (int x = area.x; x < area.x + area.w; x++) {
        int32_t fx = CLAMP((x - dst.x) * sx + sx / 2 - 32768, 0, xmax);
        col_i[x - area.x] = (uint16_t)(fx >> 16);
        col_f[x - area.x] = (uint8_t)(fx >> 8);
    }
    for (int y = area.y; y < area.y + area.h; y++) {
        int32_t fy = CLAMP((y - dst.y) * sy + sy / 2 - 32768, 0, ymax);
        int r = fy >> 16;
        unsigned wy = (unsigned)(fy >> 8 & 0xff);
        const uint32_t *r0 = src->px + r * src->stride, *r1 = r0 + (r + 1 < src->h ? src->stride : 0);
        uint32_t *d = c->px + y * c->stride;
        for (int i = 0; i < area.w; i++) {
            int k = col_i[i], k1 = MIN(k + 1, src->w - 1);
            unsigned wx = col_f[i];
            uint32_t p = mix_px(mix_px(r0[k], r0[k1], wx), mix_px(r1[k], r1[k1], wx), wy);
            unsigned a = (p >> 24) * (unsigned)alpha / 255;
            if (a >= 254)
                d[area.x + i] = p | 0xff000000u;
            else if (a)
                d[area.x + i] = over(d[area.x + i], p, a);
        }
    }
}

/* Gives fully clear pixels the color of a neighbour, so smooth scaling does not pull dark seams in at the edges. */
void gfx_bleed(struct canvas *c)
{
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < c->w; x++) {
            if (row[x] >> 24)
                continue;
            if (x > 0 && row[x - 1] >> 24)
                row[x] = row[x - 1] & 0xffffffu;
            else if (x + 1 < c->w && row[x + 1] >> 24)
                row[x] = row[x + 1] & 0xffffffu;
        }
    }
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < c->w; x++) {
            if (row[x])
                continue;
            if (y > 0 && row[x - c->stride] >> 24)
                row[x] = row[x - c->stride] & 0xffffffu;
            else if (y + 1 < c->h && row[x + c->stride] >> 24)
                row[x] = row[x + c->stride] & 0xffffffu;
        }
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

#define ICON_SIZE 64

static uint32_t *icon_px[ICON_COUNT][3];
static uint8_t *symbol_px[SYM_COUNT];

static uint32_t *half_size(const uint32_t *src, int size)
{
    int h = size / 2;
    uint32_t *out = kmalloc((size_t)h * (size_t)h * 4);
    if (!out)
        return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < h; x++)
            out[y * h + x] = sample_box(src, size, x * 2, y * 2, x * 2 + 2, y * 2 + 2);
    return out;
}

static bool load_icon(enum icon_id id)
{
    if (icon_px[id][0])
        return true;
    const struct packed_image *p = &icon_pack[id];
    if (!p->data)
        return false;
    size_t raw = (size_t)p->size * p->size * 4;
    uint8_t *planes = kmalloc(raw);
    uint32_t *px = kmalloc(raw);
    if (!planes || !px || lz4_decode(p->data, p->length, planes, raw) != raw) {
        kfree(planes);
        kfree(px);
        return false;
    }
    unfilter_planes(planes, px, p->size, p->size);
    kfree(planes);
    icon_px[id][0] = px;
    icon_px[id][1] = half_size(px, ICON_SIZE);
    icon_px[id][2] = icon_px[id][1] ? half_size(icon_px[id][1], ICON_SIZE / 2) : NULL;
    return true;
}

const uint32_t *gfx_icon_pixels(enum icon_id id)
{
    return (unsigned)id < ICON_COUNT && load_icon(id) ? icon_px[id][0] : NULL;
}

struct scaled_icon {
    int id, size;
    uint32_t *px;
};

static struct scaled_icon scaled_icons[48];
static int scaled_next;

static const uint32_t *icon_at_size(enum icon_id id, int size)
{
    for (int level = 0; level < 3; level++)
        if (size == ICON_SIZE >> level && icon_px[id][level])
            return icon_px[id][level];
    if (size <= 0 || size > 256)
        return NULL;
    for (size_t i = 0; i < ARRAY_SIZE(scaled_icons); i++)
        if (scaled_icons[i].px && scaled_icons[i].id == (int)id && scaled_icons[i].size == size)
            return scaled_icons[i].px;
    struct scaled_icon *e = &scaled_icons[scaled_next++ % (int)ARRAY_SIZE(scaled_icons)];
    kfree(e->px);
    e->px = kzalloc((size_t)size * (size_t)size * 4);
    if (!e->px)
        return NULL;
    e->id = (int)id;
    e->size = size;
    int level = size <= 20 && icon_px[id][2] ? 2 : size <= 40 && icon_px[id][1] ? 1 : 0;
    int src = ICON_SIZE >> level;
    struct canvas tmp = canvas_make(e->px, size, size);
    draw_scaled(&tmp, (struct rect){0, 0, size, size}, icon_px[id][level], src, src, src, 255);
    return e->px;
}

void gfx_icon_alpha(struct canvas *c, int x, int y, enum icon_id id, int size, int alpha)
{
    if ((unsigned)id >= ICON_COUNT || !load_icon(id))
        return;
    const uint32_t *px = icon_at_size(id, size);
    if (!px)
        return;
    struct rect dst = rect_intersect((struct rect){x, y, size, size}, c->clip);
    for (int row = dst.y; row < dst.y + dst.h; row++) {
        const uint32_t *srow = px + (row - y) * size - x;
        uint32_t *d = c->px + row * c->stride;
        for (int col = dst.x; col < dst.x + dst.w; col++) {
            uint32_t p = srow[col];
            unsigned a = (p >> 24) * (unsigned)alpha / 255;
            if (a >= 255)
                d[col] = p;
            else if (a)
                d[col] = over(d[col], p, a);
        }
    }
}

void gfx_icon(struct canvas *c, int x, int y, enum icon_id id, int size)
{
    gfx_icon_alpha(c, x, y, id, size, 255);
}

static bool load_symbol(enum symbol_id id)
{
    if (symbol_px[id])
        return true;
    const struct packed_image *p = &symbol_pack[id];
    uint8_t *data = kmalloc(16 * 16 + 32 * 32);
    if (!data || !p->data || lz4_decode(p->data, p->length, data, 16 * 16 + 32 * 32) != 16 * 16 + 32 * 32) {
        kfree(data);
        return false;
    }
    unfilter_rows(data, 16, 16);
    unfilter_rows(data + 256, 32, 32);
    symbol_px[id] = data;
    return true;
}

void gfx_symbol(struct canvas *c, int x, int y, enum symbol_id id, int size, uint32_t color)
{
    if ((unsigned)id >= SYM_COUNT || !load_symbol(id))
        return;
    unsigned base = color >> 24;
    const uint8_t *src = size <= 20 ? symbol_px[id] : symbol_px[id] + 256;
    int s = size <= 20 ? 16 : 32;
    struct rect dst = rect_intersect((struct rect){x, y, size, size}, c->clip);
    for (int row = dst.y; row < dst.y + dst.h; row++)
        for (int col = dst.x; col < dst.x + dst.w; col++) {
            unsigned a;
            if (size == s) {
                a = src[(row - y) * s + (col - x)];
            } else {
                int fx = ((col - x) * 2 + 1) * s * 128 / size - 128, fy = ((row - y) * 2 + 1) * s * 128 / size - 128;
                fx = MAX(fx, 0);
                fy = MAX(fy, 0);
                int x0 = MIN(fx >> 8, s - 1), y0 = MIN(fy >> 8, s - 1), tx = fx & 255, ty = fy & 255;
                int x1 = MIN(x0 + 1, s - 1), y1 = MIN(y0 + 1, s - 1);
                a = ((unsigned)src[y0 * s + x0] * (unsigned)(256 - tx) * (unsigned)(256 - ty) +
                     (unsigned)src[y0 * s + x1] * (unsigned)tx * (unsigned)(256 - ty) +
                     (unsigned)src[y1 * s + x0] * (unsigned)(256 - tx) * (unsigned)ty +
                     (unsigned)src[y1 * s + x1] * (unsigned)tx * (unsigned)ty) >> 16;
            }
            put_a(c, col, row, color, a * base / 255);
        }
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

static const struct ui_glyph *lookup(const struct ui_font *f, uint32_t code)
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
    return NULL;
}

static const struct ui_glyph *find_glyph(const struct ui_font *f, uint32_t code)
{
    const struct ui_glyph *g = lookup(f, code);
    return g ? g : lookup(f, '?');
}

bool gfx_has_glyph(const struct ui_font *f, uint32_t code)
{
    return lookup(f, code) != NULL;
}

static const uint8_t *font_alpha(const struct ui_font *f)
{
    if (!*f->alpha) {
        uint8_t *data = kmalloc(f->size);
        if (data && lz4_decode(f->lz, f->lz_size, data, f->size) != f->size) {
            kfree(data);
            data = NULL;
        }
        *f->alpha = data;
    }
    return *f->alpha;
}

static void draw_glyph(struct canvas *c, int x, int y, const struct ui_font *f, const struct ui_glyph *g,
                       uint32_t color)
{
    const uint8_t *alpha = font_alpha(f);
    if (!alpha)
        return;
    const uint8_t *data = alpha + g->offset;
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
                put_a(c, px, py, color, base * a * 17 / 255);
        }
    }
}

int gfx_glyph(struct canvas *c, int x, int y, const struct ui_font *f, uint32_t code, uint32_t color)
{
    const struct ui_glyph *g = find_glyph(f, code);
    if (!g)
        return 0;
    if (c)
        draw_glyph(c, x, y, f, g, color);
    return g->advance;
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
