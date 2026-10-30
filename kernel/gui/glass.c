#include <synth/heap.h>
#include <synth/string.h>

#include "internal.h"

#define LENS_SCALE 2

static struct glass *glass_list;
uint64_t glass_stat_renders, glass_stat_pixels;

int glass_margin(void)
{
    return 56;
}

void glass_register(struct glass *g)
{
    if (g->registered)
        return;
    g->registered = true;
    g->valid = false;
    g->next = glass_list;
    glass_list = g;
    int m = glass_margin();
    wm_damage_z((struct rect){g->frame.x - m, g->frame.y - m, g->frame.w + 2 * m, g->frame.h + 2 * m}, g->z);
}

void glass_unregister(struct glass *g)
{
    if (!g->registered)
        return;
    for (struct glass **p = &glass_list; *p; p = &(*p)->next)
        if (*p == g) {
            *p = g->next;
            break;
        }
    g->registered = false;
    canvas_free(&g->small);
    canvas_free(&g->sharp);
    kfree(g->geo);
    g->geo = NULL;
    canvas_free(&g->out);
}

void glass_invalidate(struct glass *g)
{
    if (!g->valid)
        return;
    g->valid = false;
    struct rect r = g->frame;
    int m = glass_margin();
    wm_damage_z((struct rect){r.x - m, r.y - m, r.w + 2 * m, r.h + 2 * m}, g->z);
}

void glass_invalidate_all(void)
{
    for (struct glass *g = glass_list; g; g = g->next)
        glass_invalidate(g);
}

void glass_set_frame(struct glass *g, struct rect frame)
{
    if (g->frame.x == frame.x && g->frame.y == frame.y && g->frame.w == frame.w && g->frame.h == frame.h)
        return;
    struct rect old = g->frame;
    g->frame = frame;
    if (g->valid) {
        g->valid = false;
        wm_damage_z(old, g->z);
    }
    int m = glass_margin();
    wm_damage_z((struct rect){frame.x - m, frame.y - m, frame.w + 2 * m, frame.h + 2 * m}, g->z);
}

static bool deferring;

void glass_damage(struct rect r, int z)
{
    int m = glass_margin();
    for (struct glass *g = glass_list; g; g = g->next) {
        if (!g->valid || g->z <= z)
            continue;
        struct rect zone = {g->frame.x - m, g->frame.y - m, g->frame.w + 2 * m, g->frame.h + 2 * m};
        if (rect_empty(rect_intersect(zone, r)))
            continue;
        if (deferring)
            g->stale = true;
        else
            glass_invalidate(g);
    }
}

/* While a window is dragged the glass above it keeps its last blur; it catches up when the drag ends. */
void glass_defer(bool on)
{
    if (deferring == on)
        return;
    deferring = on;
    if (on)
        return;
    for (struct glass *g = glass_list; g; g = g->next)
        if (g->stale) {
            g->stale = false;
            glass_invalidate(g);
        }
}

static const uint8_t refract_lut[64] = {233, 255, 249, 236, 222, 207, 193, 179, 166, 155, 143, 133, 123, 114, 106, 98,
                                        91,  84,  78,  72,  67,  62,  57,  53,  48,  45,  41,  37,  34,  31,  29,  26,
                                        24,  21,  19,  17,  15,  14,  12,  11,  10,  8,   7,   6,   5,   5,   4,   3,
                                        3,   2,   2,   1,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0};
static const uint8_t rim_lut[64] = {253, 243, 230, 213, 195, 175, 155, 136, 118, 101, 85, 72, 60, 49, 41, 33,
                                    27,  22,  17,  14,  11,  9,   7,   5,   4,   3,   3,  2,  2,  1,  1,  1};

static void box_pass(uint32_t *px, int w, int h, int stride, int step_x, int step_y, int radius, uint32_t *tmp)
{
    UNUSED(step_y);
    int lines = step_x ? h : w, len = step_x ? w : h;
    int window = radius * 2 + 1;
    unsigned inv = 65536u / (unsigned)window;
    for (int l = 0; l < lines; l++) {
        uint32_t *base = step_x ? px + l * stride : px + l;
        int step = step_x ? 1 : stride;
        unsigned r = 0, g = 0, b = 0;
        for (int i = -radius; i <= radius; i++) {
            uint32_t p = base[CLAMP(i, 0, len - 1) * step];
            r += p >> 16 & 0xff;
            g += p >> 8 & 0xff;
            b += p & 0xff;
        }
        for (int i = 0; i < len; i++) {
            tmp[i] = 0xff000000u | (r * inv >> 16) << 16 | (g * inv >> 16) << 8 | (b * inv >> 16);
            uint32_t out = base[CLAMP(i - radius, 0, len - 1) * step];
            uint32_t in = base[CLAMP(i + radius + 1, 0, len - 1) * step];
            r += (in >> 16 & 0xff) - (out >> 16 & 0xff);
            g += (in >> 8 & 0xff) - (out >> 8 & 0xff);
            b += (in & 0xff) - (out & 0xff);
        }
        for (int i = 0; i < len; i++)
            base[i * step] = tmp[i];
    }
}

static bool no_capture;

void glass_freeze(bool on)
{
    no_capture = on;
}

static void blur_canvas(struct canvas *cv, int blur, int passes)
{
    uint32_t *tmp = kmalloc((size_t)MAX(cv->w, cv->h) * 4);
    if (!tmp)
        return;
    for (int pass = 0; pass < passes; pass++) {
        box_pass(cv->px, cv->w, cv->h, cv->w, 1, 0, blur, tmp);
        box_pass(cv->px, cv->w, cv->h, cv->w, 0, 1, blur, tmp);
    }
    kfree(tmp);
}

static bool fit(struct canvas *cv, int w, int h)
{
    if (cv->px && cv->w == w && cv->h == h)
        return true;
    canvas_free(cv);
    return canvas_alloc(cv, w, h);
}

/* Averages scale x scale blocks of `from` (sampling every step-th pixel) into `to`. */
static void shrink(const uint32_t *from, int stride, struct rect area, int scale, int step, struct canvas *to)
{
    for (int y = 0; y < to->h; y++)
        for (int x = 0; x < to->w; x++) {
            unsigned r = 0, gg = 0, b = 0, n = 0;
            int x0 = area.x + x * scale, y0 = area.y + y * scale;
            int x1 = MIN(x0 + scale, area.x + area.w), y1 = MIN(y0 + scale, area.y + area.h);
            for (int yy = y0; yy < y1; yy += step)
                for (int xx = x0; xx < x1; xx += step) {
                    uint32_t p = from[yy * stride + xx];
                    r += p >> 16 & 0xff;
                    gg += p >> 8 & 0xff;
                    b += p & 0xff;
                    n++;
                }
            n = MAX(n, 1u);
            to->px[y * to->w + x] = 0xff000000u | (r / n) << 16 | (gg / n) << 8 | (b / n);
        }
}

/* The frosted copy is `scale` times smaller and blurred; liquid glass also keeps a sharp half-size copy that the
   edge refracts, so the lens shows the background crisp at the rim and frosted inside. */
static bool capture(struct canvas *c, struct glass *g, int blur, int scale, bool lens)
{
    if (no_capture)
        return false;
    struct rect screen = wm_compose_screen();
    int m = glass_margin();
    struct rect src = rect_intersect((struct rect){g->frame.x - m, g->frame.y - m, g->frame.w + 2 * m, g->frame.h + 2 * m},
                                     screen);
    if (rect_empty(src) || !rect_inside(src, c->clip))
        return false;
    int sw = (src.w + scale - 1) / scale, sh = (src.h + scale - 1) / scale;
    if (!fit(&g->small, sw, sh))
        return false;
    if (lens && scale % LENS_SCALE == 0 && fit(&g->sharp, (src.w + LENS_SCALE - 1) / LENS_SCALE,
                                              (src.h + LENS_SCALE - 1) / LENS_SCALE)) {
        shrink(c->px, c->stride, src, LENS_SCALE, 1, &g->sharp);
        shrink(g->sharp.px, g->sharp.w, (struct rect){0, 0, g->sharp.w, g->sharp.h}, scale / LENS_SCALE, 1, &g->small);
        blur_canvas(&g->sharp, 1, 1);
    } else {
        canvas_free(&g->sharp);
        shrink(c->px, c->stride, src, scale, scale >= 4 ? 2 : 1, &g->small);
    }
    if (blur > 0)
        blur_canvas(&g->small, blur, 2);
    g->src = src;
    g->scale = scale;
    g->valid = true;
    unsigned sum = 0, count = 0;
    for (int i = 0; i < g->small.w * g->small.h; i += 3, count++) {
        uint32_t p = g->small.px[i];
        sum += ((p >> 16 & 0xff) * 77 + (p >> 8 & 0xff) * 151 + (p & 0xff) * 28) >> 8;
    }
    g->luma = count ? (int)(sum / count) : 128;
    g->stamp++;
    return true;
}

void glass_prepare(struct canvas *c, struct glass *g, const struct glass_look *look)
{
    int scale = look->scale ? look->scale : 4;
    if (!look->solid && (!g->valid || g->scale != scale))
        capture(c, g, look->blur, scale, look->liquid);
}

static uint32_t sample_from(const struct canvas *cv, struct rect src, int scale, int fx, int fy)
{
    fx = (fx - src.x * 256) / scale - 128;
    fy = (fy - src.y * 256) / scale - 128;
    int w = cv->w, h = cv->h;
    fx = CLAMP(fx, 0, (w - 1) * 256);
    fy = CLAMP(fy, 0, (h - 1) * 256);
    int x0 = fx >> 8, y0 = fy >> 8, tx = fx & 255, ty = fy & 255;
    int x1 = MIN(x0 + 1, w - 1), y1 = MIN(y0 + 1, h - 1);
    const uint32_t *px = cv->px;
    uint32_t a = px[y0 * w + x0], b = px[y0 * w + x1], c = px[y1 * w + x0], d = px[y1 * w + x1];
    uint32_t wa = (uint32_t)((256 - tx) * (256 - ty)), wb = (uint32_t)(tx * (256 - ty)), wc = (uint32_t)((256 - tx) * ty),
             wd = (uint32_t)(tx * ty);
    uint32_t rb = ((a & 0xff00ffu) * (wa >> 8) + (b & 0xff00ffu) * (wb >> 8) + (c & 0xff00ffu) * (wc >> 8) +
                   (d & 0xff00ffu) * (wd >> 8)) >> 8;
    uint32_t gch = ((a & 0xff00u) * (wa >> 8) + (b & 0xff00u) * (wb >> 8) + (c & 0xff00u) * (wc >> 8) +
                    (d & 0xff00u) * (wd >> 8)) >> 8;
    return 0xff000000u | (rb & 0xff00ffu) | (gch & 0xff00u);
}

/* What the lens shows at (fx, fy): the sharp copy when there is one, else the frosted one. */
static uint32_t sample(const struct glass *g, int fx, int fy)
{
    if (g->sharp.px)
        return sample_from(&g->sharp, g->src, LENS_SCALE, fx, fy);
    return sample_from(&g->small, g->src, g->scale, fx, fy);
}

struct tap {
    uint16_t i0, i1;
    uint16_t t;
};

static void taps(struct tap *out, int from, int n, int origin, int scale, int size)
{
    for (int i = 0; i < n; i++) {
        int f = ((from + i) * 256 + 128 - origin * 256) / scale - 128;
        f = CLAMP(f, 0, (size - 1) * 256);
        out[i] = (struct tap){(uint16_t)(f >> 8), (uint16_t)MIN((f >> 8) + 1, size - 1), (uint16_t)(f & 255)};
    }
}

static inline uint32_t bilerp(const uint32_t *r0, const uint32_t *r1, const struct tap *tx, int ty)
{
    uint32_t a = r0[tx->i0], b = r0[tx->i1], c = r1[tx->i0], d = r1[tx->i1];
    int t = tx->t;
    uint32_t wa = (uint32_t)((256 - t) * (256 - ty)), wb = (uint32_t)(t * (256 - ty)), wc = (uint32_t)((256 - t) * ty),
             wd = (uint32_t)(t * ty);
    uint32_t rb = ((a & 0xff00ffu) * (wa >> 8) + (b & 0xff00ffu) * (wb >> 8) + (c & 0xff00ffu) * (wc >> 8) +
                   (d & 0xff00ffu) * (wd >> 8)) >> 8;
    uint32_t gch = ((a & 0xff00u) * (wa >> 8) + (b & 0xff00u) * (wb >> 8) + (c & 0xff00u) * (wc >> 8) +
                    (d & 0xff00u) * (wd >> 8)) >> 8;
    return 0xff000000u | (rb & 0xff00ffu) | (gch & 0xff00u);
}

static uint32_t isqrt32(uint32_t v)
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

/* distance from the edge in 1/16 px (negative outside) and the outward normal in 1/256 */
static int edge(struct rect f, int radius, int x, int y, int *nx, int *ny)
{
    int px = x * 16 + 8, py = y * 16 + 8;
    int l = f.x * 16, t = f.y * 16, r = (f.x + f.w) * 16, b = (f.y + f.h) * 16, rad = radius * 16;
    int cx = px < l + rad ? l + rad : px > r - rad ? r - rad : px;
    int cy = py < t + rad ? t + rad : py > b - rad ? b - rad : py;
    if (cx != px && cy != py) {
        int vx = px - cx, vy = py - cy;
        int len = (int)isqrt32((uint32_t)(vx * vx + vy * vy));
        if (!len) {
            *nx = *ny = 0;
            return rad;
        }
        *nx = vx * 256 / len;
        *ny = vy * 256 / len;
        return rad - len;
    }
    int dl = px - l, dr = r - px, dt = py - t, db = b - py;
    int d = MIN(MIN(dl, dr), MIN(dt, db));
    *nx = d == dl ? -256 : d == dr ? 256 : 0;
    *ny = *nx ? 0 : d == dt ? -256 : 256;
    return d;
}

struct geo {
    int16_t d;
    int8_t nx, ny;
};

static const struct geo *geometry(struct glass *g, struct rect f, int radius)
{
    if (g->geo && g->geo_w == f.w && g->geo_h == f.h && g->geo_r == radius)
        return g->geo;
    kfree(g->geo);
    g->geo = kmalloc((size_t)f.w * (size_t)f.h * sizeof(struct geo));
    if (!g->geo)
        return NULL;
    g->geo_w = f.w;
    g->geo_h = f.h;
    g->geo_r = radius;
    struct geo *out = g->geo;
    struct rect local = {0, 0, f.w, f.h};
    for (int y = 0; y < f.h; y++)
        for (int x = 0; x < f.w; x++) {
            int nx, ny;
            int d = edge(local, radius, x, y, &nx, &ny);
            *out++ = (struct geo){(int16_t)CLAMP(d, -32768, 32767), (int8_t)(nx / 2 > 127 ? 127 : nx / 2 < -127 ? -127 : nx / 2),
                                  (int8_t)(ny / 2 > 127 ? 127 : ny / 2 < -127 ? -127 : ny / 2)};
        }
    return g->geo;
}

static inline int clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void render(struct glass *g, const struct glass_look *look, struct rect f, uint32_t *dst, int stride)
{
    int radius = MIN(look->radius, MIN(f.w, f.h) / 2);
    unsigned tint_a = look->tint >> 24, sat = (unsigned)look->saturation;
    const void *ctx = look->shape_ctx;
    bool sdf = look->shape && look->shape_sdf && look->liquid;
    int band = look->liquid && (!look->shape || sdf) ? MIN(look->band, MIN(f.w, f.h) / 2) : 0;
    int band16 = MAX(band, 1) * 16;
    int refract = look->refract * 256;
    int inner = band + radius;
    int lift = look->brightness;
    if (look->tint && !style.dark && g->luma < 150)
        lift += (150 - g->luma) * 3 / 4;
    else if (look->tint && style.dark && g->luma > 120)
        lift -= (g->luma - 120) * 2 / 3;
    const struct geo *geo = band && !look->shape ? geometry(g, f, radius) : NULL;
    struct tap *cols = kmalloc(sizeof(struct tap) * (size_t)(f.w + f.h));
    if (!cols)
        return;
    struct tap *rows = cols + f.w;
    taps(cols, f.x, f.w, g->src.x, g->scale, g->small.w);
    taps(rows, f.y, f.h, g->src.y, g->scale, g->small.h);
    int cw = f.w + 4;
    uint16_t *cover = look->shape && !sdf ? kmalloc(sizeof(uint16_t) * (size_t)cw * (size_t)(f.h + 4)) : NULL;
    if (cover)
        for (int y = 0; y < f.h + 4; y++) {
            int x0 = 0, x1 = 0;
            if (look->shape_span && look->shape_span(f.y + y - 2, look->shape_ctx, &x0, &x1)) {
                x0 = CLAMP(x0 - f.x + 2, 0, cw);
                x1 = CLAMP(x1 - f.x + 2, x0, cw);
            }
            for (int x = 0; x < cw; x++) {
                if (x == x0 && x1 > x0) {
                    for (; x < x1; x++)
                        cover[y * cw + x] = 256;
                    if (x == cw)
                        break;
                }
                cover[y * cw + x] = (uint16_t)CLAMP(look->shape(f.x + x - 2, f.y + y - 2, look->shape_ctx), 0, 256);
            }
        }
    for (int y = f.y; y < f.y + f.h; y++) {
        uint32_t *row = dst + (y - f.y) * stride - f.x;
        int plain_x = INT32_MIN;
        const struct tap *ry = &rows[y - f.y];
        const uint32_t *r0 = g->small.px + ry->i0 * g->small.w, *r1 = g->small.px + ry->i1 * g->small.w;
        bool edge_row = y < f.y + inner || y >= f.y + f.h - inner;
        for (int x = f.x; x < f.x + f.w; x++) {
            int cov = 256, d = band16, nx = 0, ny = 0;
            int shape_rim = 0;
            if (sdf) {
                d = look->shape_sdf(x, y, ctx, &nx, &ny);
                if (d <= -8) {
                    row[x] = 0;
                    continue;
                }
                if (d < 8)
                    cov = (d + 8) * 16;
            } else if (cover) {
                const uint16_t *cp = cover + (y - f.y + 2) * cw + (x - f.x + 2);
                cov = *cp;
                if (cov <= 0) {
                    row[x] = 0;
                    continue;
                }
                int inner = MIN(MIN(cp[-2], cp[2]), MIN(cp[-2 * cw], cp[2 * cw]));
                shape_rim = (256 - inner) * (y < f.y + f.h / 2 ? 80 : 44) >> 8;
            } else if (look->shape) {
                row[x] = 0;
                continue;
            } else if (edge_row || x < f.x + inner || x >= f.x + f.w - inner) {
                if (geo) {
                    const struct geo *e = &geo[(y - f.y) * f.w + (x - f.x)];
                    d = e->d;
                    nx = e->nx * 2;
                    ny = e->ny * 2;
                } else {
                    d = edge(f, radius, x, y, &nx, &ny);
                }
                if (d <= -8) {
                    row[x] = 0;
                    continue;
                }
                if (d < 8)
                    cov = (d + 8) * 16;
            }
            bool plain = cov >= 256 && !shape_rim && !(band && d < band16);
            if (plain && plain_x == x - 1 && (x & 1)) {
                row[x] = row[x - 1];
                continue;
            }
            if (plain)
                plain_x = x;
            uint32_t p;
            int spec = 0, ta = (int)tint_a, shade = 0;
            if (band && d < band16) {
                int i = MAX(d, 0) * 64 / band16;
                ta = ta * (112 + i * 2) >> 8;
                int k = refract * refract_lut[i] / 255;
                int dx = nx * k >> 8, dy = ny * k >> 8;
                int sx = x * 256 + 128, sy = y * 256 + 128;
                if (look->dispersion) {
                    uint32_t pr = sample(g, sx + dx * 9 / 8, sy + dy * 9 / 8);
                    uint32_t pg = sample(g, sx + dx, sy + dy);
                    uint32_t pb = sample(g, sx + dx * 7 / 8, sy + dy * 7 / 8);
                    p = (pr & 0xffff0000u) | (pg & 0xff00u) | (pb & 0xffu);
                } else {
                    p = sample(g, sx + dx, sy + dy);
                }
                if (g->sharp.px)
                    p = color_mix(bilerp(r0, r1, &cols[x - f.x], ry->t), p, 255 - i * 4);
                int light = (-nx * 181 - ny * 181) >> 8;
                int dir = light > 0 ? light * light >> 8 : (light * light >> 8) * 5 / 8;
                spec = rim_lut[i] * (look->rim * (64 + dir * 3 / 4) >> 8) >> 8;
                if (d < 28)
                    spec += (28 - MAX(d, 0)) * (90 + dir * 2 / 3) / 28;
                shade = refract_lut[i] * 26 >> 8;
                if (look->pointer_light) {
                    int mx = mouse_x - x, my = mouse_y - y;
                    int dist = mx * mx + my * my, reach = look->pointer_light * look->pointer_light;
                    if (dist < reach)
                        spec += rim_lut[i] * (reach - dist) / reach * 3 / 4;
                }
            } else {
                p = bilerp(r0, r1, &cols[x - f.x], ry->t);
            }
            int r = (int)(p >> 16 & 0xff), gg = (int)(p >> 8 & 0xff), b = (int)(p & 0xff);
            int luma = (r * 77 + gg * 151 + b * 28) >> 8;
            r = clamp8(luma + ((r - luma) * (int)sat >> 8) + lift - shade);
            gg = clamp8(luma + ((gg - luma) * (int)sat >> 8) + lift - shade);
            b = clamp8(luma + ((b - luma) * (int)sat >> 8) + lift - shade);
            uint32_t out = (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            if (ta)
                out = color_mix(out | 0xff000000u, look->tint | 0xff000000u, ta) & 0xffffffu;
            spec += shape_rim;
            if (spec > 0) {
                spec = MIN(spec, 255);
                r = (int)(out >> 16 & 0xff);
                gg = (int)(out >> 8 & 0xff);
                b = (int)(out & 0xff);
                r += (255 - r) * spec >> 8;
                gg += (255 - gg) * spec >> 8;
                b += (255 - b) * spec >> 8;
                out = (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            }
            row[x] = out | (uint32_t)(cov >= 256 ? 255 : cov * 255 / 256) << 24;
        }
    }
    kfree(cover);
    kfree(cols);
}

static bool pointer_near(const struct glass_look *look, struct rect f)
{
    if (!look->pointer_light)
        return false;
    int m = look->pointer_light;
    return rect_contains((struct rect){f.x - m, f.y - m, f.w + 2 * m, f.h + 2 * m}, mouse_x, mouse_y);
}

void glass_draw(struct canvas *c, struct glass *g, const struct glass_look *look)
{
    struct rect f = g->frame;
    struct rect area = rect_intersect(f, c->clip);
    if (rect_empty(area))
        return;
    int scale = look->scale ? look->scale : 4;
    if (g->valid && g->scale != scale)
        g->valid = false;
    if (look->solid || (!g->valid && !capture(c, g, look->blur, scale, look->liquid))) {
        uint32_t fallback = look->solid ? look->solid_color : ALPHA(look->tint, 255);
        if (look->shape)
            gfx_shape(c, f, look->shape, look->shape_ctx, fallback);
        else
            gfx_round(c, f, look->radius, fallback);
        return;
    }
    bool near = pointer_near(look, f);
    bool fresh = g->out.px && g->out.w == f.w && g->out.h == f.h && g->out_stamp == g->stamp &&
                 g->out_x == f.x && g->out_y == f.y && !memcmp(&g->out_look, look, sizeof(*look)) &&
                 (!near ? !g->out_near : g->out_mx == mouse_x && g->out_my == mouse_y);
    if (!fresh) {
        if (!g->out.px || g->out.w != f.w || g->out.h != f.h) {
            canvas_free(&g->out);
            if (!canvas_alloc(&g->out, f.w, f.h))
                return;
        }
        render(g, look, f, g->out.px, g->out.stride);
        glass_stat_renders++;
        glass_stat_pixels += (uint64_t)f.w * (uint64_t)f.h;
        g->out_stamp = g->stamp;
        g->out_x = f.x;
        g->out_y = f.y;
        g->out_look = *look;
        g->out_near = near;
        g->out_mx = mouse_x;
        g->out_my = mouse_y;
    }
    for (int y = area.y; y < area.y + area.h; y++) {
        const uint32_t *src = g->out.px + (y - f.y) * g->out.stride - f.x;
        uint32_t *row = c->px + y * c->stride;
        for (int x = area.x, end = area.x + area.w; x < end;) {
            int run = x;
            while (run < end && src[run] >> 24 == 255)
                run++;
            if (run > x) {
                memcpy(row + x, src + x, sizeof(uint32_t) * (size_t)(run - x));
                x = run;
                continue;
            }
            uint32_t p = src[x];
            unsigned a = p >> 24;
            if (a)
                row[x] = color_mix(row[x], p | 0xff000000u, (int)a);
            x++;
        }
    }
    int radius = MIN(look->radius, MIN(f.w, f.h) / 2);
    if (look->shape)
        return;
    if (!look->liquid && look->rim)
        gfx_round_frame_gradient(c, f, radius, ALPHA(0xffffffffu, look->rim), ALPHA(0xffffffffu, look->rim / 3));
    if (look->outline)
        gfx_round_frame(c, (struct rect){f.x - 1, f.y - 1, f.w + 2, f.h + 2}, radius + 1, look->outline);
}

void glass_look_for(struct glass_look *look, enum glass_kind kind, int radius)
{
    memset(look, 0, sizeof(*look));
    look->radius = radius;
    bool dark = style.dark;
    look->solid = style.glass == GLASS_SOLID;
    look->liquid = style.glass == GLASS_LIQUID;
    look->scale = 8;
    look->blur = 3;
    look->saturation = look->liquid ? 330 : 420;
    look->band = 16;
    look->refract = 14;
    look->rim = look->liquid ? 210 : 60;
    look->brightness = dark ? -6 : 10;
    look->outline = dark ? ALPHA(0, 110) : ALPHA(0, 26);
    switch (kind) {
    case GLASS_MENUBAR:
        look->tint = ALPHA(color_mix(dark ? RGB(30, 30, 34) : RGB(250, 250, 252), style.tint | 0xff000000u, dark ? 40 : 30), dark ? 90 : 110);
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(236, 236, 238);
        look->rim = 0;
        look->outline = 0;
        look->liquid = false;
        break;
    case GLASS_DOCK:
        look->tint = ALPHA(color_mix(dark ? RGB(30, 30, 36) : RGB(255, 255, 255), style.tint | 0xff000000u, dark ? 60 : 40),
                           look->liquid ? (dark ? 90 : 70) : (dark ? 150 : 130));
        look->solid_color = dark ? RGB(52, 52, 56) : RGB(232, 232, 236);
        if (look->liquid) {
            look->scale = 4;
            look->blur = 3;
            look->band = 22;
            look->refract = 20;
            look->saturation = 300;
            look->dispersion = true;
            look->pointer_light = 110;
            look->outline = dark ? ALPHA(0, 90) : 0;
        }
        break;
    case GLASS_MENU:
        look->tint = ALPHA(color_mix(dark ? RGB(34, 34, 38) : RGB(250, 250, 252), style.tint | 0xff000000u, dark ? 50 : 26),
                           look->liquid ? 160 : 180);
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(246, 246, 248);
        look->band = 12;
        look->refract = 10;
        look->scale = 6;
        look->blur = 4;
        if (look->liquid)
            look->outline = dark ? ALPHA(0, 90) : ALPHA(0, 14);
        break;
    case GLASS_PANEL:
        look->tint = ALPHA(color_mix(dark ? RGB(58, 54, 68) : RGB(250, 250, 252), style.tint | 0xff000000u, dark ? 40 : 34),
                           look->liquid ? (dark ? 96 : 78) : (dark ? 130 : 120));
        look->brightness = dark ? 8 : 16;
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(244, 244, 246);
        look->blur = 3;
        look->band = 26;
        look->refract = 24;
        look->dispersion = look->liquid;
        if (look->liquid)
            look->outline = dark ? ALPHA(0, 90) : ALPHA(0, 14);
        break;
    case GLASS_WINDOW:
        look->tint = 0;
        look->solid_color = style.sidebar | 0xff000000u;
        look->liquid = false;
        look->rim = 0;
        look->outline = 0;
        look->blur = 3;
        look->saturation = 400;
        look->brightness = 0;
        break;
    case GLASS_OVERLAY:
        look->tint = dark ? RGBA(0, 0, 0, 90) : RGBA(0, 0, 0, 40);
        look->solid_color = dark ? RGB(20, 20, 24) : RGB(90, 90, 100);
        look->liquid = false;
        look->rim = 0;
        look->outline = 0;
        look->blur = 7;
        look->saturation = 300;
        look->brightness = 0;
        break;
    }
    int k = gui_glass_strength();
    if (!look->liquid || k == 70)
        return;
    if (!k) {
        look->liquid = false;
        look->dispersion = false;
        look->rim = 60;
        return;
    }
    look->refract = look->refract * k / 70;
    look->band = MAX(4, look->band * (k + 70) / 140);
    look->rim = MIN(255, look->rim * k / 70);
    look->pointer_light = MIN(255, look->pointer_light * k / 70);
    look->dispersion = look->dispersion && k >= 40;
    unsigned a = look->tint >> 24;
    a = k < 70 ? a + (255 - a) * (unsigned)(70 - k) / 110 : a * (unsigned)(170 - k) / 100;
    look->tint = (look->tint & 0xffffffu) | MIN(a, 255u) << 24;
}
