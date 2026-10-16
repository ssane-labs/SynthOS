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

static int smooth(int t)
{
    return t * t * (768 - 2 * t) >> 16;
}

static int vnoise(int x, int y, uint32_t seed)
{
    int xi = x >> 8, yi = y >> 8, fx = smooth(x & 255), fy = smooth(y & 255);
    uint32_t k = seed * 0x9e3779b9u;
    int a = (int)(hash((uint32_t)xi * 73856093u ^ (uint32_t)yi * 19349663u ^ k) & 4095);
    int b = (int)(hash((uint32_t)(xi + 1) * 73856093u ^ (uint32_t)yi * 19349663u ^ k) & 4095);
    int c = (int)(hash((uint32_t)xi * 73856093u ^ (uint32_t)(yi + 1) * 19349663u ^ k) & 4095);
    int d = (int)(hash((uint32_t)(xi + 1) * 73856093u ^ (uint32_t)(yi + 1) * 19349663u ^ k) & 4095);
    int top = a + ((b - a) * fx >> 8), bottom = c + ((d - c) * fx >> 8);
    return top + ((bottom - top) * fy >> 8);
}

static int fbm(int x, int y, int octaves, uint32_t seed)
{
    int sum = 0, amp = 512, total = 0;
    for (int i = 0; i < octaves; i++) {
        sum += vnoise(x, y, seed + (uint32_t)i) * amp;
        total += amp;
        amp /= 2;
        x = x * 2 + 1371;
        y = y * 2 + 917;
    }
    return sum / total;
}

static uint32_t ramp(const uint32_t *stops, int n, int t)
{
    t = CLAMP(t, 0, 1023);
    int seg = t * (n - 1) / 1024, off = t * (n - 1) - seg * 1024;
    return color_mix(stops[seg], stops[MIN(seg + 1, n - 1)], off >> 2);
}

static uint32_t add_light(uint32_t p, uint32_t light, int a)
{
    a = CLAMP(a, 0, 255);
    int r = (int)(p >> 16 & 0xff) + (int)(light >> 16 & 0xff) * a / 255;
    int g = (int)(p >> 8 & 0xff) + (int)(light >> 8 & 0xff) * a / 255;
    int b = (int)(p & 0xff) + (int)(light & 0xff) * a / 255;
    return RGB(MIN(r, 255), MIN(g, 255), MIN(b, 255));
}

static int glow_at(int x, int y, int cx, int cy, int r)
{
    int64_t d2 = (int64_t)(x - cx) * (x - cx) + (int64_t)(y - cy) * (y - cy), r2 = (int64_t)r * r;
    if (d2 >= r2)
        return 0;
    int t = (int)(1024 - d2 * 1024 / r2);
    return t * t / 1024;
}

static void stars(struct canvas *c, int limit, int density, int strength)
{
    for (int y = 0; y < limit; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t r = hash((uint32_t)(y * 7919 + x * 104729) ^ 0x51ed27u);
            if (r % (uint32_t)density)
                continue;
            int a = (int)(r >> 24) * strength / 255 * (limit - y) / MAX(limit, 1);
            uint32_t tint = (r >> 8 & 3) == 0 ? RGB(255, 220, 190) : (r >> 8 & 3) == 1 ? RGB(190, 210, 255) : RGB(255, 255, 255);
            uint32_t *p = &c->px[y * c->stride + x];
            *p = add_light(*p, tint, a);
            if ((r >> 12 & 31) == 0) {
                for (int k = 1; k <= 2; k++) {
                    int fade = a / (k * 3);
                    if (x - k >= 0)
                        p[-k] = add_light(p[-k], tint, fade);
                    if (x + k < c->w)
                        p[k] = add_light(p[k], tint, fade);
                    if (y - k >= 0)
                        p[-k * c->stride] = add_light(p[-k * c->stride], tint, fade);
                    if (y + k < c->h)
                        p[k * c->stride] = add_light(p[k * c->stride], tint, fade);
                }
            }
        }
}

static int dune_profile(int u)
{
    int t = u & 4095;
    const int crest = 2800;
    if (t < crest) {
        int64_t q = (int64_t)t * 4096 / crest;
        return (int)(q * q * (3 * 4096 - 2 * q) / (4096LL * 4096 * 4));
    }
    int64_t f = (int64_t)(4095 - t) * 4096 / (4096 - crest);
    return (int)(f * f / 16384);
}

static void paint_dune(struct canvas *c, bool dark)
{
    sin_init();
    int w = c->w, h = c->h;
    enum { N = 5 };
    static const int base[N] = {500, 575, 655, 760, 900}, amp1[N] = {70, 90, 105, 130, 150}, amp2[N] = {25, 35, 40, 50, 60};
    static const int wave1[N] = {900, 1150, 1400, 1700, 2100}, wave2[N] = {340, 420, 520, 640, 760};
    uint32_t lit[N], shade[N], sky[4];
    if (dark) {
        static const uint32_t l[N] = {RGB(84, 72, 120), RGB(88, 68, 112), RGB(92, 64, 102), RGB(96, 60, 92), RGB(98, 56, 80)};
        static const uint32_t s2[N] = {RGB(58, 50, 92), RGB(54, 44, 84), RGB(48, 38, 74), RGB(42, 32, 64), RGB(34, 26, 52)};
        memcpy(lit, l, sizeof(lit));
        memcpy(shade, s2, sizeof(shade));
        sky[0] = RGB(14, 14, 36), sky[1] = RGB(36, 30, 70), sky[2] = RGB(86, 60, 104), sky[3] = RGB(150, 96, 120);
    } else {
        static const uint32_t l[N] = {RGB(240, 192, 150), RGB(238, 176, 124), RGB(234, 158, 100), RGB(226, 138, 78), RGB(214, 118, 60)};
        static const uint32_t s2[N] = {RGB(214, 158, 132), RGB(198, 132, 104), RGB(178, 108, 80), RGB(156, 86, 60), RGB(128, 66, 44)};
        memcpy(lit, l, sizeof(lit));
        memcpy(shade, s2, sizeof(shade));
        sky[0] = RGB(122, 104, 150), sky[1] = RGB(214, 150, 140), sky[2] = RGB(246, 196, 150), sky[3] = RGB(255, 226, 180);
    }
    int *crest = kmalloc(sizeof(int) * (size_t)w * N), *slope = kmalloc(sizeof(int) * (size_t)w * N);
    if (!crest || !slope) {
        kfree(crest);
        kfree(slope);
        return;
    }
    for (int i = 0; i < N; i++) {
        for (int x = 0; x < w; x++) {
            int u1 = (x * 4096 / MAX(1, wave1[i] * w / 1024)) + i * 1337;
            int u2 = (x * 4096 / MAX(1, wave2[i] * w / 1024)) + i * 3911;
            slope[i * w + x] = h * 16 * base[i] / 1000 - h * 16 * amp1[i] / 1000 * dune_profile(u1) / 1024 -
                               h * 16 * amp2[i] / 1000 * dune_profile(u2) / 1024 + (fbm(x * 2, i * 4096, 3, 7) - 2048) * h / 4000;
        }
        for (int x = 0; x < w; x++)
            crest[i * w + x] = slope[i * w + x];
        for (int x = 0; x < w; x++) {
            int l = MAX(x - 4, 0), r = MIN(x + 4, w - 1);
            slope[i * w + x] = (crest[i * w + r] - crest[i * w + l]) * 8 / MAX(r - l, 1);
        }
        for (int x = 0; x < w; x++)
            crest[i * w + x] >>= 4;
    }
    int sun_x = w * 70 / 100, sun_y = h * 46 / 100;
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        uint32_t sky_px = ramp(sky, 4, y * 1024 / MAX(1, h * 55 / 100));
        for (int x = 0; x < w; x++) {
            int layer = -1;
            for (int i = N - 1; i >= 0; i--)
                if (y >= crest[i * w + x]) {
                    layer = i;
                    break;
                }
            uint32_t p;
            if (layer < 0) {
                p = sky_px;
                int haze = fbm(x, y * 4, 4, 3);
                p = color_mix(p, dark ? RGB(70, 60, 110) : RGB(255, 236, 214), CLAMP((haze - 2000) / 12, 0, 60));
                p = add_light(p, dark ? RGB(170, 170, 220) : RGB(255, 240, 210), glow_at(x, y, sun_x, sun_y, h * 45 / 100) * (dark ? 70 : 160) / 1024);
            } else {
                int top = crest[layer * w + x], d = y - top;
                int sl = slope[layer * w + x];
                int light = CLAMP(600 - sl * 4, 0, 1024);
                light = light * light / 1024;
                uint32_t lc = color_mix(shade[layer], lit[layer], light >> 2);
                int depth = CLAMP(d * 1024 / MAX(1, h / 5), 0, 1024);
                lc = color_mix(lc, color_shade(shade[layer], -30), depth / 6);
                if (d < 3)
                    lc = color_mix(lc, dark ? RGB(180, 170, 230) : RGB(255, 236, 200), (3 - d) * 50 * light / 1024);
                if (layer >= 2) {
                    int rip = isin(d * 48 * (8 - layer) / 6 + (fbm(x * 3, y * 3, 2, 9) >> 3) + x / 3) / 1024;
                    lc = color_shade(lc, rip * (layer - 1) * light / 4096);
                }
                int haze = (N - 1 - layer) * 34;
                p = color_mix(lc, sky[3], haze);
                p = add_light(p, dark ? RGB(80, 80, 120) : RGB(255, 200, 140), glow_at(x, y, sun_x, sun_y, h * 50 / 100) * 40 / 1024);
            }
            int grain = (int)(hash((uint32_t)(x * 1973 + y * 9277)) & 7) - 3;
            row[x] = color_shade(p, grain);
        }
    }
    if (dark)
        stars(c, h * 40 / 100, 260, 200);
    kfree(crest);
    kfree(slope);
}

static void paint_mist(struct canvas *c, bool dark)
{
    int w = c->w, h = c->h;
    enum { N = 5 };
    static const int base[N] = {450, 540, 640, 750, 880}, amp[N] = {150, 160, 170, 180, 190};
    uint32_t far = dark ? RGB(62, 74, 100) : RGB(188, 200, 216), near = dark ? RGB(12, 16, 26) : RGB(52, 66, 84);
    uint32_t sky_top = dark ? RGB(12, 16, 30) : RGB(200, 212, 228), sky_low = dark ? RGB(44, 54, 78) : RGB(242, 244, 247);
    uint32_t fog = dark ? RGB(96, 110, 136) : RGB(250, 251, 252);
    int *ridge = kmalloc(sizeof(int) * (size_t)w * N);
    if (!ridge)
        return;
    for (int i = 0; i < N; i++)
        for (int x = 0; x < w; x++) {
            int n = fbm(x * (3 + i) * 512 / MAX(w, 1), i * 3000, 4, 11 + (uint32_t)i);
            int rn = fbm(x * (5 + 2 * i) * 1024 / MAX(w, 1), i * 3000 + 700, 3, 17 + (uint32_t)i);
            int sharp = 4096 - (rn > 2048 ? rn - 2048 : 2048 - rn) * 2;
            int peak = CLAMP((n - 1000) * 4096 / 2100, 0, 4096) * 7 / 8 + sharp / 8;
            ridge[i * w + x] = h * base[i] / 1000 - h * amp[i] / 1000 * peak / 4096;
        }
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        uint32_t sky = lerp(sky_top, sky_low, y, h * 6 / 10);
        int band = 0;
        for (int i = 1; i < N; i++) {
            int mid = h * (base[i] - 40) / 1000, spread = h * 7 / 100;
            int dy = y > mid ? y - mid : mid - y;
            band = MAX(band, CLAMP(1024 - dy * 1024 / spread, 0, 1024));
        }
        for (int x = 0; x < w; x++) {
            int layer = -1;
            for (int i = N - 1; i >= 0; i--)
                if (y >= ridge[i * w + x]) {
                    layer = i;
                    break;
                }
            uint32_t p = sky;
            if (layer < 0) {
                p = add_light(p, dark ? RGB(160, 170, 210) : RGB(255, 250, 240), glow_at(x, y, w * 30 / 100, h * 30 / 100, h * 40 / 100) * (dark ? 60 : 90) / 1024);
            } else {
                uint32_t col = color_mix(far, near, layer * 255 / (N - 1));
                int d = y - ridge[layer * w + x];
                col = color_mix(col, color_shade(col, dark ? -40 : -24), CLAMP(d * 255 / MAX(1, h / 5), 0, 255) / 3);
                p = col;
            }
            int wisp = fbm(x * 256 / 300, y * 256 / 26, 4, 31);
            int a = band * CLAMP((wisp - 1500) / 5, 0, 255) / 1024 * (dark ? 120 : 210) / 255;
            p = color_mix(p, fog, a);
            int low = CLAMP((y - h * 78 / 100) * 255 / MAX(1, h * 22 / 100), 0, 255);
            p = color_mix(p, fog, low * CLAMP((fbm(x * 256 / 260, y * 256 / 30, 3, 77) - 1300) / 4, 0, 255) / 255 * 170 / 255);
            int grain = (int)(hash((uint32_t)(x * 1973 + y * 9277)) & 3) - 1;
            row[x] = color_shade(p, grain);
        }
    }
    if (dark)
        stars(c, h * 35 / 100, 340, 160);
    kfree(ridge);
}

static void paint_night(struct canvas *c, bool dark)
{
    int w = c->w, h = c->h;
    uint32_t top = dark ? RGB(5, 6, 11) : RGB(46, 44, 84), bottom = dark ? RGB(14, 14, 28) : RGB(120, 100, 150);
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        uint32_t base = lerp(top, bottom, y, h);
        for (int x = 0; x < w; x++) {
            int band = (x * 2 - y * 3 + h * 2) * 1024 / MAX(1, w * 2 + h * 3);
            int n = fbm(x, y, 5, 101);
            int m = fbm(x * 2 + 9000, y * 2, 4, 211);
            int center = 1024 - CLAMP((band > 560 ? band - 560 : 560 - band) * 4, 0, 1024);
            int a = CLAMP((n - 1700) / 6, 0, 255) * center / 1024;
            uint32_t p = base;
            p = add_light(p, RGB(90, 60, 200), a * 70 / 255);
            p = add_light(p, RGB(30, 150, 170), CLAMP((m - 2100) / 7, 0, 255) * center / 1024 * 45 / 255);
            int horizon = h * 88 / 100 - (fbm(x * 3, 0, 4, 5) - 2048) * h / 40000;
            if (y >= horizon)
                p = color_mix(RGB(4, 4, 8), RGB(10, 10, 18), CLAMP((y - horizon) * 255 / MAX(1, h - horizon), 0, 255));
            else
                p = add_light(p, RGB(120, 70, 160), CLAMP(255 - (horizon - y) * 255 / MAX(1, h / 4), 0, 255) * 40 / 255);
            row[x] = dither(p, x, y);
        }
    }
    stars(c, h * 86 / 100, 130, 255);
    int mx = w * 82 / 100, my = h * 18 / 100, mr = h * 4 / 100;
    for (int y = my - mr * 6; y < my + mr * 6; y++)
        for (int x = mx - mr * 6; x < mx + mr * 6; x++) {
            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;
            uint32_t *p = &c->px[y * c->stride + x];
            int g = glow_at(x, y, mx, my, mr * 6);
            *p = add_light(*p, RGB(150, 150, 210), g * 50 / 1024);
            int dx = x - mx, dy = y - my, ex = x - mx - mr * 45 / 100, ey = y - my + mr * 20 / 100;
            if (dx * dx + dy * dy <= mr * mr && ex * ex + ey * ey > mr * mr * 80 / 100)
                *p = color_mix(*p, RGB(240, 236, 220), 235);
        }
}

static void paint_sunset(struct canvas *c, bool dark)
{
    int w = c->w, h = c->h, horizon = h * 64 / 100;
    static const uint32_t sky_light[] = {RGB(44, 18, 92), RGB(112, 34, 120), RGB(208, 62, 112), RGB(250, 118, 92), RGB(255, 186, 112)};
    static const uint32_t sky_dark[] = {RGB(12, 6, 32), RGB(46, 14, 70), RGB(110, 30, 90), RGB(170, 56, 86), RGB(220, 110, 90)};
    const uint32_t *sky = dark ? sky_dark : sky_light;
    int sun_x = w / 2, sun_r = h * 14 / 100;
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++) {
            uint32_t p;
            if (y < horizon) {
                p = ramp(sky, 5, y * 1024 / horizon);
                int cloud = fbm(x * 256 / 400, y * 256 / 30, 5, 404);
                int band = CLAMP(1024 - (y > horizon * 55 / 100 ? y - horizon * 55 / 100 : horizon * 55 / 100 - y) * 1024 / MAX(1, horizon / 3), 0, 1024);
                int a = CLAMP((cloud - 2000) / 5, 0, 255) * band / 1024;
                uint32_t lit = color_mix(RGB(255, 140, 120), RGB(255, 210, 150), y * 255 / horizon);
                p = color_mix(p, color_mix(RGB(120, 40, 110), lit, CLAMP((cloud - 2000) / 4, 0, 255)), a * 3 / 4);
                p = add_light(p, RGB(255, 130, 80), glow_at(x, y, sun_x, horizon, h * 55 / 100) * 80 / 1024);
                int dx = x - sun_x, dy = y - horizon + sun_r / 3;
                if (dx * dx + dy * dy <= sun_r * sun_r) {
                    uint32_t sun = lerp(RGB(255, 236, 140), RGB(255, 110, 120), y - (horizon - sun_r - sun_r / 3), sun_r + sun_r / 3);
                    int edge = sun_r * sun_r - dx * dx - dy * dy;
                    p = color_mix(p, sun, CLAMP(edge * 2 / MAX(1, sun_r) * 255 / 4, 0, 255));
                }
                int left = horizon - h * 9 / 100 + (fbm(x * 256 / 60, 100, 4, 21) - 2048) * h / 30000 + CLAMP((x - w / 3) * h / MAX(1, w * 2), 0, h) * 3;
                int right = horizon - h * 7 / 100 + (fbm(x * 256 / 60, 300, 4, 22) - 2048) * h / 30000 + CLAMP((w * 2 / 3 - x) * h / MAX(1, w * 2), 0, h) * 3;
                int ridge = MIN(left, right);
                if (y >= ridge) {
                    p = color_mix(RGB(70, 22, 74), RGB(30, 10, 44), CLAMP((y - ridge) * 255 / MAX(1, horizon - ridge), 0, 255));
                    if (y - ridge < 2)
                        p = color_mix(p, RGB(255, 130, 140), 120);
                }
            } else {
                int d = y - horizon;
                p = lerp(dark ? RGB(70, 24, 70) : RGB(120, 40, 100), dark ? RGB(8, 4, 20) : RGB(26, 10, 44), d, h - horizon);
                int spread = sun_r + d * 2 / 3;
                int dx = x - sun_x;
                int col = CLAMP(1024 - (dx < 0 ? -dx : dx) * 1024 / MAX(1, spread), 0, 1024);
                int streak = fbm(x * 256 / 40, y * 128, 3, 505);
                int s = CLAMP((streak - 1800) / 5, 0, 255) * col / 1024;
                p = add_light(p, RGB(255, 150, 100), s * CLAMP(255 - d * 255 / MAX(1, h - horizon), 40, 255) / 255);
                int wave = fbm(x * 256 / 30, y * 128, 2, 606);
                p = color_shade(p, (wave - 2048) / 160);
            }
            row[x] = dither(p, x, y);
        }
    }
    stars(c, horizon / 3, 400, 140);
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
    if (!strcmp(kind, "dune"))
        paint_dune(c, dark);
    else if (!strcmp(kind, "mist"))
        paint_mist(c, dark);
    else if (!strcmp(kind, "night"))
        paint_night(c, dark);
    else if (!strcmp(kind, "sunset"))
        paint_sunset(c, dark);
    else if (!strcmp(kind, "hills"))
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
