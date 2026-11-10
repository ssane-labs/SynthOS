#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
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

/* Black and white liquid glass: clear rods flowing across a gray field, each lit like glass (a bright rim on top, a
   highlight streak, a dark caustic line inside the lower edge) and casting a soft shadow below. */
struct ribbon {
    int mid, amp, waves, phase, thick, swell;
};

static uint32_t glass_rod(uint32_t p, int s, bool dark)
{
    int a = s < 0 ? -s : s;
    p = color_mix(p, RGB(255, 255, 255), dark ? 26 : 70);
    p = color_mix(p, dark ? RGB(0, 0, 0) : RGB(80, 80, 86), CLAMP(s, 0, 1024) * (dark ? 110 : 80) / 1024);
    if (a > 760) {
        int r = (a - 760) * 255 / 264;
        p = color_mix(p, RGB(255, 255, 255), r * r / 255 * (s < 0 ? 220 : 130) / 255);
    }
    int d = s + 520;
    d = d < 0 ? -d : d;
    if (d < 150) {
        int q = (150 - d) * 255 / 150;
        p = color_mix(p, RGB(255, 255, 255), q * q / 255 * (dark ? 235 : 200) / 255);
    }
    int line = 100 - (s > 680 ? s - 680 : 680 - s);
    if (line > 0)
        p = color_mix(p, RGB(0, 0, 0), line * (dark ? 140 : 80) / 100);
    return p;
}

static void paint_ribbon(struct canvas *c, const struct ribbon *r, bool dark)
{
    int w = c->w, h = c->h, shadow = MAX(4, h * 4 / 100);
    for (int x = 0; x < w; x++) {
        int t = x * SIN_STEPS / MAX(w, 1);
        int64_t mid = (int64_t)h * 16 * (r->mid * 16384 + r->amp * isin(t * r->waves / 100 + r->phase * SIN_STEPS / 1000)) /
                      (1000 * 16384);
        int64_t half = (int64_t)h * 16 * (r->thick * 16384 + r->swell * isin(t * r->waves * 17 / 1000 + 300)) /
                       (2000 * 16384);
        half = MAX(half, 32);
        int top = (int)((mid - half) / 16) - 1, bottom = (int)((mid + half) / 16) + 1;
        for (int y = MAX(bottom - 1, 0); y < MIN(bottom + shadow, h); y++) {
            int k = shadow - (y - bottom + 1);
            if (k > 0) {
                uint32_t *p = c->px + y * c->stride + x;
                *p = color_mix(*p, RGB(0, 0, 0), k * k * (dark ? 110 : 60) / (shadow * shadow));
            }
        }
        for (int y = MAX(top, 0); y <= MIN(bottom, h - 1); y++) {
            int64_t off = (int64_t)y * 16 + 8 - mid;
            int64_t inside = half - (off < 0 ? -off : off);
            int cover = (int)CLAMP(inside + 8, 0, 16);
            if (!cover)
                continue;
            int s = (int)CLAMP(off * 1024 / half, -1024, 1024);
            uint32_t *p = c->px + y * c->stride + x;
            *p = color_mix(*p, glass_rod(*p, s, dark), cover * 16);
        }
    }
}

static void paint_mono(struct canvas *c, bool dark)
{
    static const struct ribbon ribbons[] = {
        {270, 70, 65, 120, 60, 22},
        {520, 110, 90, 640, 120, 40},
        {800, 60, 140, 330, 70, 26},
    };
    sin_init();
    int w = c->w, h = c->h;
    uint32_t top = dark ? RGB(34, 34, 37) : RGB(240, 240, 242), bottom = dark ? RGB(3, 3, 4) : RGB(170, 170, 176);
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride, base = lerp(top, bottom, y, h);
        for (int x = 0; x < w; x++)
            row[x] = add_light(base, RGB(255, 255, 255), glow_at(x, y, w / 5, h / 6, h) * (dark ? 46 : 30) / 1024);
    }
    for (size_t i = 0; i < ARRAY_SIZE(ribbons); i++)
        paint_ribbon(c, &ribbons[i], dark);
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++)
            row[x] = dither(row[x], x, y);
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

/* Halo, the SynthOS picture: an eclipse ring of colored light rising from below the screen, a thin orbit across it
   and, at night, a few stars. */
static int ring_light(int d, int width)
{
    int64_t q = (int64_t)d * d * 1024 / ((int64_t)width * width);
    return (int)(1024 * 1024 / (1024 + q));
}

static void paint_halo(struct canvas *c, bool dark)
{
    int w = c->w, h = c->h;
    int cx = w * 60 / 100, cy = h * 112 / 100, R = MAX(h * 80 / 100, w * 40 / 100);
    int core = MAX(h / 70, 4), wide = core * 9;
    int ox = w * 30 / 100, oy = h * 128 / 100, oR = MAX(h * 105 / 100, w * 62 / 100);
    uint32_t top = dark ? RGB(7, 8, 20) : RGB(236, 239, 251), bottom = dark ? RGB(20, 11, 38) : RGB(251, 236, 244);
    uint32_t disk = dark ? RGB(4, 4, 10) : RGB(246, 246, 252);
    for (int y = 0; y < h; y++) {
        uint32_t *row = c->px + y * c->stride, base = lerp(top, bottom, y, h);
        for (int x = 0; x < w; x++) {
            int dx = x - cx, dy = y - cy;
            int dist = grid_sqrt((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
            int d = dist - R;
            uint32_t p = base;
            if (d < 0)
                p = color_mix(base, disk, CLAMP(-d * 255 / (core * 6), 0, 235));
            int u = (x - (cx - R)) * 1024 / (2 * R) + (cy - y) * 120 / MAX(R, 1);
            uint32_t tone = gfx_halo_tone(CLAMP(u, 0, 1023));
            int glow = ring_light(d, wide) * (dark ? 120 : 150) / 1024;
            int line = ring_light(d, core) * (dark ? 230 : 235) / 1024;
            if (d < 0)
                glow = glow * 2 / 3;
            if (dark) {
                p = add_light(p, tone, glow);
                p = add_light(p, color_mix(tone, RGB(255, 255, 255), 120), line);
            } else {
                p = color_mix(p, tone, glow * 2 / 3);
                p = color_mix(p, color_mix(tone, RGB(255, 255, 255), 60), line * 3 / 4);
            }
            int odx = x - ox, ody = y - oy;
            int od = grid_sqrt((uint64_t)((int64_t)odx * odx + (int64_t)ody * ody)) - oR;
            int orbit = ring_light(od, 2) * (dark ? 46 : 40) / 1024;
            if (orbit > 2)
                p = dark ? add_light(p, RGB(200, 210, 255), orbit) : color_mix(p, RGB(120, 110, 200), orbit);
            if (dark && d > 0) {
                uint32_t k = hash((uint32_t)(x * 7919) ^ (uint32_t)(y * 104729));
                if ((k & 1023) == 7)
                    p = add_light(p, RGB(255, 255, 255), 40 + (int)(k >> 24) / 2);
            }
            row[x] = dither(p, x, y);
        }
    }
}

/* The pictures that come with the system are photographs (NASA, public domain) in /system/wallpapers; the two drawn
   ones are the SynthOS marks: Halo and Mono. */
static const char *const photos[] = {"sunrise", "earthset", "aurora", "milkyway", "dunes", "glacier", "saturn"};
static const char *const photo_files[] = {"Sunrise", "Earthset", "Aurora", "MilkyWay", "Dunes", "Glacier", "Saturn"};

void wallpaper_paint(struct canvas *c, const char *kind, bool dark)
{
    c->clip = (struct rect){0, 0, c->w, c->h};
    if (!strcmp(kind, "halo")) {
        paint_halo(c, dark);
        return;
    }
    if (!strcmp(kind, "solid")) {
        gfx_clear(c, c->clip, dark ? RGB(30, 32, 40) : RGB(90, 120, 170));
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(photos); i++)
        if (!strcmp(kind, photos[i])) {
            char path[PATH_MAX_LEN];
            ksnprintf(path, sizeof(path), "/system/wallpapers/%s.jpg", photo_files[i]);
            if (paint_image(c, path))
                return;
        }
    if (strcmp(kind, "mono") && paint_image(c, kind))
        return;
    paint_mono(c, dark);
}
