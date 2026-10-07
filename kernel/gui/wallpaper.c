#include <synth/heap.h>
#include <synth/string.h>

#include "image.h"
#include "internal.h"

#define SIN_STEPS 1024

static int16_t sin_table[SIN_STEPS];

static void sin_init(void)
{
    if (sin_table[SIN_STEPS / 4])
        return;
    int64_t x = 1LL << 30, y = 0;
    const int64_t c = 1073721612, s = 6588387;
    for (int i = 0; i < SIN_STEPS; i++) {
        sin_table[i] = (int16_t)(y >> 16);
        int64_t nx = (x * c - y * s) >> 30, ny = (x * s + y * c) >> 30;
        x = nx;
        y = ny;
    }
}

static int isin(int phase)
{
    return sin_table[phase & (SIN_STEPS - 1)];
}

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t lerp(uint32_t a, uint32_t b, int t, int span)
{
    return color_mix(a, b, span ? CLAMP(t * 255 / span, 0, 255) : 0);
}

static uint32_t dither(uint32_t c, int x, int y)
{
    int n = (int)(hash((uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663)) & 3) - 1;
    int r = CLAMP((int)(c >> 16 & 0xff) + n, 0, 255), g = CLAMP((int)(c >> 8 & 0xff) + n, 0, 255),
        b = CLAMP((int)(c & 0xff) + n, 0, 255);
    return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

struct layer {
    int base;
    int amp1, freq1, phase1;
    int amp2, freq2, phase2;
    uint32_t top, bottom;
};

static uint32_t layer_color(const struct layer *layers, const int *curve, int w, int h, int i, int x, int y)
{
    const struct layer *l = &layers[i];
    int edge = curve[i * w + x] >> 4;
    uint32_t p = lerp(l->top, l->bottom, y - edge, h - edge);
    int d = y - edge;
    if (d >= 0 && d < 5)
        p = color_mix(p, 0xffffffffu, (5 - d) * 14);
    return p;
}

static void paint_layers(struct canvas *c, uint32_t sky_top, uint32_t sky_bottom, const struct layer *layers, int n,
                         int shadow)
{
    sin_init();
    int w = c->w, h = c->h;
    int *curve = kmalloc(sizeof(int) * (size_t)w * (size_t)n);
    if (!curve)
        return;
    for (int i = 0; i < n; i++)
        for (int x = 0; x < w; x++) {
            const struct layer *l = &layers[i];
            int phase1 = l->phase1 + x * l->freq1 * SIN_STEPS / MAX(w, 1) / 4;
            int phase2 = l->phase2 + x * l->freq2 * SIN_STEPS / MAX(w, 1) / 4;
            int64_t v = (int64_t)h * l->base * 16 / 1000 + (int64_t)h * l->amp1 * 16 / 1000 * isin(phase1) / 16384 +
                        (int64_t)h * l->amp2 * 16 / 1000 * isin(phase2) / 16384;
            curve[i * w + x] = (int)v;
        }
    int shade = MAX(8, h / 12);
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        uint32_t sky = lerp(sky_top, sky_bottom, y, h);
        int y16 = y * 16;
        for (int x = 0; x < w; x++) {
            int top_layer = -1, cover = 16;
            for (int i = n - 1; i >= 0; i--) {
                int cv = curve[i * w + x];
                if (y16 + 16 > cv) {
                    top_layer = i;
                    cover = CLAMP(y16 + 16 - cv, 0, 16);
                    break;
                }
            }
            uint32_t p;
            if (top_layer < 0) {
                p = sky;
            } else {
                p = layer_color(layers, curve, w, h, top_layer, x, y);
                if (cover < 16) {
                    uint32_t under = top_layer > 0 ? layer_color(layers, curve, w, h, top_layer - 1, x, y) : sky;
                    p = color_mix(under, p, cover * 16);
                }
            }
            int next = top_layer + 1;
            if (next < n) {
                int d = (curve[next * w + x] >> 4) - y;
                if (d > 0 && d < shade) {
                    uint32_t tint = color_shade(layers[next].bottom, -90);
                    p = color_mix(p, tint, (shade - d) * (shade - d) * shadow / (shade * shade));
                }
            }
            row[x] = dither(p, x, y);
        }
    }
    kfree(curve);
}

static void paint_waves(struct canvas *c, bool dark)
{
    static const struct layer light[] = {
        {300, 60, 3, 100, 25, 7, 300, RGB(170, 200, 255), RGB(140, 170, 245)},
        {420, 70, 2, 600, 30, 5, 50, RGB(140, 160, 248), RGB(110, 120, 230)},
        {540, 60, 3, 300, 22, 6, 700, RGB(150, 120, 235), RGB(120, 90, 215)},
        {660, 55, 2, 900, 28, 5, 200, RGB(200, 120, 220), RGB(170, 90, 200)},
        {790, 50, 3, 450, 20, 7, 800, RGB(245, 140, 185), RGB(225, 110, 160)},
        {910, 40, 2, 150, 18, 6, 500, RGB(255, 170, 160), RGB(245, 140, 140)},
    };
    static const struct layer night[] = {
        {300, 60, 3, 100, 25, 7, 300, RGB(40, 52, 120), RGB(30, 40, 100)},
        {420, 70, 2, 600, 30, 5, 50, RGB(52, 50, 140), RGB(40, 36, 120)},
        {540, 60, 3, 300, 22, 6, 700, RGB(72, 44, 150), RGB(56, 32, 128)},
        {660, 55, 2, 900, 28, 5, 200, RGB(100, 40, 140), RGB(80, 28, 118)},
        {790, 50, 3, 450, 20, 7, 800, RGB(130, 40, 120), RGB(104, 28, 98)},
        {910, 40, 2, 150, 18, 6, 500, RGB(150, 48, 100), RGB(120, 34, 80)},
    };
    if (dark)
        paint_layers(c, RGB(10, 12, 36), RGB(28, 30, 80), night, 6, 110);
    else
        paint_layers(c, RGB(205, 225, 255), RGB(185, 205, 255), light, 6, 70);
}

static void paint_hills(struct canvas *c, bool dark)
{
    static const struct layer light[] = {
        {520, 40, 2, 200, 15, 5, 100, RGB(190, 216, 214), RGB(176, 206, 200)},
        {600, 45, 3, 500, 18, 6, 400, RGB(160, 204, 178), RGB(140, 190, 160)},
        {690, 50, 2, 800, 16, 5, 700, RGB(126, 188, 136), RGB(104, 170, 116)},
        {790, 45, 3, 100, 20, 7, 300, RGB(96, 168, 104), RGB(76, 148, 88)},
        {890, 40, 2, 650, 15, 6, 900, RGB(70, 146, 84), RGB(54, 126, 70)},
    };
    static const struct layer night[] = {
        {520, 40, 2, 200, 15, 5, 100, RGB(40, 62, 98), RGB(34, 54, 88)},
        {600, 45, 3, 500, 18, 6, 400, RGB(34, 64, 90), RGB(28, 56, 80)},
        {690, 50, 2, 800, 16, 5, 700, RGB(28, 64, 80), RGB(22, 56, 70)},
        {790, 45, 3, 100, 20, 7, 300, RGB(22, 60, 68), RGB(18, 52, 60)},
        {890, 40, 2, 650, 15, 6, 900, RGB(16, 54, 56), RGB(12, 44, 46)},
    };
    if (dark)
        paint_layers(c, RGB(8, 14, 40), RGB(40, 56, 100), night, 5, 120);
    else
        paint_layers(c, RGB(150, 200, 250), RGB(240, 236, 220), light, 5, 60);
}

static void paint_aurora(struct canvas *c, bool dark)
{
    struct blob {
        int x, y, r;
        uint32_t color;
    };
    int w = c->w, h = c->h;
    struct blob light[4] = {{w / 5, h / 4, h * 7 / 10, RGB(255, 154, 162)},
                            {w * 4 / 5, h / 5, h * 6 / 10, RGB(255, 216, 111)},
                            {w * 3 / 4, h, h * 8 / 10, RGB(127, 200, 255)},
                            {w / 6, h * 9 / 10, h * 6 / 10, RGB(179, 157, 255)}};
    struct blob night[4] = {{w / 5, h / 4, h * 7 / 10, RGB(106, 61, 232)},
                            {w * 4 / 5, h / 5, h * 6 / 10, RGB(232, 61, 132)},
                            {w * 3 / 4, h, h * 8 / 10, RGB(61, 160, 232)},
                            {w / 6, h * 9 / 10, h * 6 / 10, RGB(30, 200, 160)}};
    struct blob *b = dark ? night : light;
    uint32_t base = dark ? RGB(12, 10, 32) : RGB(244, 238, 255);
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++) {
            uint32_t p = base;
            for (int i = 0; i < 4; i++) {
                int dx = x - b[i].x, dy = y - b[i].y;
                int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy, r2 = (int64_t)b[i].r * b[i].r;
                if (d2 < r2) {
                    int t = (int)(255 - d2 * 255 / r2);
                    p = color_mix(p, b[i].color, t * t / 255 * (dark ? 230 : 210) / 255);
                }
            }
            row[x] = dither(p, x, y);
        }
    }
}

static int grid_strength(int d, int k, int depth)
{
    int fade = CLAMP((d * 300 / k - 6) * 12, 0, 255);
    return CLAMP(60 + d * 400 / depth, 60, 255) * fade / 255;
}

static int grid_sqrt(uint64_t v)
{
    uint64_t x = 0;
    for (uint64_t bit = 1ull << 62; bit; bit >>= 2) {
        if (v >= x + bit) {
            v -= x + bit;
            x = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
    }
    return (int)MAX(x, 1);
}

static void grid_blend(struct canvas *c, int x, int y, int a)
{
    if (x < 0 || x >= c->w || a <= 0)
        return;
    uint32_t *p = &c->px[y * c->stride + x];
    *p = color_mix(*p, RGB(255, 60, 200), MIN(a, 255));
}

static void grid_span(struct canvas *c, int y, int64_t xa, int64_t xb, int a)
{
    if (y < 0 || y >= c->h)
        return;
    if (xb < xa) {
        int64_t t = xa;
        xa = xb;
        xb = t;
    }
    if (xb - xa < 0x10000) {
        int64_t mid = (xa + xb) / 2;
        int x = (int)(mid >> 16), frac = (int)((mid & 0xffff) >> 8);
        grid_blend(c, x, y, a * (255 - frac) / 255);
        grid_blend(c, x + 1, y, a * frac / 255);
        return;
    }
    int x0 = (int)(xa >> 16), x1 = (int)(xb >> 16);
    for (int x = MAX(x0, 0); x <= MIN(x1, c->w - 1); x++)
        grid_blend(c, x, y, a);
}

static void paint_synthwave(struct canvas *c)
{
    int w = c->w, h = c->h, horizon = h * 62 / 100;
    int cx = w / 2, radius = h * 22 / 100, sun_y = horizon - radius / 4;
    uint32_t top = RGB(14, 3, 34), mid = RGB(96, 18, 108), low = RGB(255, 96, 110);
    for (int y = 0; y < horizon; y++) {
        uint32_t sky = y < horizon * 6 / 10 ? lerp(top, mid, y, horizon * 6 / 10)
                                             : lerp(mid, low, y - horizon * 6 / 10, horizon - horizon * 6 / 10);
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++) {
            uint32_t p = sky;
            uint32_t r = hash((uint32_t)(y * 7919 + x * 104729));
            if (y < horizon * 7 / 10 && r % 900 == 0)
                p = color_mix(p, 0xffffffffu, 120 + (int)(r >> 24) / 2);
            int dx = x - cx, dy = y - sun_y;
            int d2 = dx * dx + dy * dy;
            if (d2 <= radius * radius) {
                uint32_t sun = lerp(RGB(255, 230, 90), RGB(255, 50, 150), y - (sun_y - radius), 2 * radius);
                int rel = y - sun_y;
                bool gap = false;
                if (rel > -radius / 6) {
                    int period = MAX(6, radius / 7);
                    int gap_h = 1 + (rel + radius / 6) * (period - 2) / (radius + radius / 6);
                    gap = (rel + radius) % period < gap_h;
                }
                if (!gap)
                    p = sun;
            } else if (d2 < (radius * 14 / 10) * (radius * 14 / 10)) {
                int glow = 255 - (int)((uint32_t)(d2 - radius * radius) * 255 / (uint32_t)(radius * radius * 96 / 100));
                p = color_mix(p, RGB(255, 80, 160), CLAMP(glow, 0, 255) / 4);
            }
            row[x] = p;
        }
    }
    int ridge[64];
    for (int i = 0; i < 64; i++)
        ridge[i] = (int)(hash((uint32_t)i * 2654435761u) % (uint32_t)(h / 9)) + h / 40;
    for (int x = 0; x < w; x++) {
        int seg = x * 63 / MAX(w - 1, 1), off = x * 63 % MAX(w - 1, 1);
        int peak = ridge[seg] + (ridge[MIN(seg + 1, 63)] - ridge[seg]) * off / MAX(w - 1, 1);
        int center_cut = (x > cx - radius * 3 / 2 && x < cx + radius * 3 / 2) ? peak / 3 : peak;
        int top_y = horizon - center_cut;
        for (int y = MAX(top_y, 0); y < horizon; y++)
            c->px[y * c->stride + x] = y == top_y ? RGB(255, 90, 200)
                                                  : color_mix(RGB(40, 8, 64), RGB(14, 3, 30),
                                                              (horizon - y) * 255 / MAX(center_cut, 1));
    }
    int k = MAX(1, (h - horizon) * 300 / 292), depth = h - horizon;
    for (int y = horizon; y < h; y++) {
        int d = y - horizon + 1;
        uint32_t ground = lerp(RGB(46, 8, 66), RGB(8, 2, 20), y - horizon, depth);
        bool hline = (4000 * k / 300 / d) / 40 != (4000 * k / 300 / (d + 1)) / 40;
        uint32_t color = hline ? color_mix(ground, RGB(255, 60, 200), grid_strength(d, k, depth)) : ground;
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++)
            row[x] = color;
    }
    int lines = (w / 2 + 2) * k / 120 + 1;
    for (int i = -lines; i <= lines; i++) {
        int64_t wx = (int64_t)i * 120;
        int last = i ? (int)MIN((int64_t)depth, (int64_t)(w / 2 + 2) * k / (wx < 0 ? -wx : wx) + 1) : depth;
        int norm = grid_sqrt((uint64_t)k * (uint64_t)k + (uint64_t)(wx * wx));
        for (int d = 1; d <= last; d++) {
            int a = grid_strength(d, k, depth) * CLAMP((120 * d / norm - 4) * 255 / 12, 0, 255) / 255;
            if (!a)
                continue;
            int64_t xa = ((int64_t)cx << 16) + (wx * d << 16) / k;
            int64_t xb = ((int64_t)cx << 16) + (wx * (d + 1) << 16) / k;
            grid_span(c, horizon + d - 1, xa, xb, a);
        }
    }
}

static bool paint_image(struct canvas *c, const char *path)
{
    struct image img;
    if (!image_load(path, &img))
        return false;
    struct canvas src = canvas_make(img.px, img.w, img.h);
    for (int i = 0; i < img.w * img.h; i++)
        img.px[i] |= 0xff000000u;
    int cw = c->w, ch = c->h;
    int sw = img.w, sh = img.h;
    struct rect dst;
    if ((int64_t)sw * ch > (int64_t)sh * cw) {
        int w = (int)((int64_t)sw * ch / sh);
        dst = (struct rect){(cw - w) / 2, 0, w, ch};
    } else {
        int h = (int)((int64_t)sh * cw / sw);
        dst = (struct rect){0, (ch - h) / 2, cw, h};
    }
    gfx_scale(c, dst, &src, 255);
    image_free(&img);
    return true;
}

void wallpaper_paint(struct canvas *c, const char *kind, bool dark)
{
    c->clip = (struct rect){0, 0, c->w, c->h};
    if (!strcmp(kind, "hills"))
        paint_hills(c, dark);
    else if (!strcmp(kind, "aurora"))
        paint_aurora(c, dark);
    else if (!strcmp(kind, "synthwave"))
        paint_synthwave(c);
    else if (!strcmp(kind, "solid"))
        gfx_clear(c, c->clip, dark ? RGB(30, 32, 40) : RGB(90, 120, 170));
    else if (!strcmp(kind, "waves") || !paint_image(c, kind))
        paint_waves(c, dark);
}
