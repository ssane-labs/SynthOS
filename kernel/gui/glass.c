#include <synth/heap.h>
#include <synth/string.h>

#include "internal.h"

#define SCALE 4

static struct glass *glass_list;

int glass_margin(void)
{
    return 7 * SCALE * 2;
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

void glass_damage(struct rect r, int z)
{
    int m = glass_margin();
    for (struct glass *g = glass_list; g; g = g->next) {
        if (!g->valid || g->z <= z)
            continue;
        struct rect zone = {g->frame.x - m, g->frame.y - m, g->frame.w + 2 * m, g->frame.h + 2 * m};
        if (!rect_empty(rect_intersect(zone, r)))
            glass_invalidate(g);
    }
}

static void box_pass(uint32_t *px, int w, int h, int stride, int step_x, int step_y, int radius, uint32_t *tmp)
{
    int lines = step_x ? h : w, len = step_x ? w : h;
    int window = radius * 2 + 1;
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
            tmp[i] = 0xff000000u | (r / (unsigned)window) << 16 | (g / (unsigned)window) << 8 | (b / (unsigned)window);
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

static bool capture(struct canvas *c, struct glass *g, int blur)
{
    struct rect screen = wm_compose_screen();
    int m = glass_margin();
    struct rect src = rect_intersect((struct rect){g->frame.x - m, g->frame.y - m, g->frame.w + 2 * m, g->frame.h + 2 * m},
                                     screen);
    if (rect_empty(src) || !rect_inside(src, c->clip))
        return false;
    int sw = (src.w + SCALE - 1) / SCALE, sh = (src.h + SCALE - 1) / SCALE;
    if (!g->small.px || g->small.w != sw || g->small.h != sh) {
        canvas_free(&g->small);
        if (!canvas_alloc(&g->small, sw, sh))
            return false;
    }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            unsigned r = 0, gg = 0, b = 0, n = 0;
            int x0 = src.x + x * SCALE, y0 = src.y + y * SCALE;
            int x1 = MIN(x0 + SCALE, src.x + src.w), y1 = MIN(y0 + SCALE, src.y + src.h);
            for (int yy = y0; yy < y1; yy += 2)
                for (int xx = x0; xx < x1; xx += 2) {
                    uint32_t p = c->px[yy * c->stride + xx];
                    r += p >> 16 & 0xff;
                    gg += p >> 8 & 0xff;
                    b += p & 0xff;
                    n++;
                }
            n = MAX(n, 1u);
            g->small.px[y * sw + x] = 0xff000000u | (r / n) << 16 | (gg / n) << 8 | (b / n);
        }
    uint32_t *tmp = kmalloc((size_t)MAX(sw, sh) * 4);
    if (tmp) {
        for (int pass = 0; pass < 2; pass++) {
            box_pass(g->small.px, sw, sh, sw, 1, 0, blur, tmp);
            box_pass(g->small.px, sw, sh, sw, 0, 1, blur, tmp);
        }
        kfree(tmp);
    }
    g->src = src;
    g->valid = true;
    return true;
}

static uint32_t sample(const struct glass *g, int x, int y)
{
    int fx = (x - g->src.x) * 256 / SCALE - 128, fy = (y - g->src.y) * 256 / SCALE - 128;
    int w = g->small.w, h = g->small.h;
    fx = CLAMP(fx, 0, (w - 1) * 256);
    fy = CLAMP(fy, 0, (h - 1) * 256);
    int x0 = fx >> 8, y0 = fy >> 8, tx = fx & 255, ty = fy & 255;
    int x1 = MIN(x0 + 1, w - 1), y1 = MIN(y0 + 1, h - 1);
    const uint32_t *px = g->small.px;
    uint32_t a = px[y0 * w + x0], b = px[y0 * w + x1], c = px[y1 * w + x0], d = px[y1 * w + x1];
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        int top = (int)(a >> shift & 0xff) * (256 - tx) + (int)(b >> shift & 0xff) * tx;
        int bottom = (int)(c >> shift & 0xff) * (256 - tx) + (int)(d >> shift & 0xff) * tx;
        out |= (uint32_t)((top * (256 - ty) + bottom * ty) >> 16) << shift;
    }
    return out;
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

static void lens(struct rect f, int radius, int band, int x, int y, int *ox, int *oy)
{
    *ox = *oy = 0;
    int cx = x < f.x + radius ? f.x + radius : x >= f.x + f.w - radius ? f.x + f.w - radius - 1 : x;
    int cy = y < f.y + radius ? f.y + radius : y >= f.y + f.h - radius ? f.y + f.h - radius - 1 : y;
    int d, nx, ny;
    if (cx != x && cy != y) {
        int vx = x - cx, vy = y - cy;
        int len = (int)isqrt32((uint32_t)(vx * vx + vy * vy) * 256);
        if (!len)
            return;
        d = radius * 16 - len;
        nx = vx * 4096 / len;
        ny = vy * 4096 / len;
    } else {
        int dl = x - f.x, dr = f.x + f.w - 1 - x, dt = y - f.y, db = f.y + f.h - 1 - y;
        d = MIN(MIN(dl, dr), MIN(dt, db)) * 16;
        nx = d == dl * 16 ? -256 : d == dr * 16 ? 256 : 0;
        ny = nx ? 0 : d == dt * 16 ? -256 : 256;
    }
    if (d >= band * 16 || d < 0)
        return;
    int t = band * 16 - d;
    int shift = t * t / (band * 16) / 2;
    *ox = nx * shift / 256 / 16;
    *oy = ny * shift / 256 / 16;
}

void glass_draw(struct canvas *c, struct glass *g, const struct glass_look *look)
{
    struct rect f = g->frame;
    struct rect area = rect_intersect(f, c->clip);
    if (rect_empty(area))
        return;
    if (look->solid || (!g->valid && !capture(c, g, look->blur))) {
        uint32_t fallback = look->solid ? look->solid_color : ALPHA(look->tint, 255);
        gfx_round(c, f, look->radius, fallback);
        return;
    }
    unsigned tint_a = look->tint >> 24, sat = (unsigned)look->saturation;
    int band = look->liquid ? MIN(look->band, MIN(f.w, f.h) / 3) : 0;
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t *row = c->px + y * c->stride;
        bool corner_row = y < f.y + look->radius || y >= f.y + f.h - look->radius;
        for (int x = area.x; x < area.x + area.w; x++) {
            int cov = 256;
            if (corner_row && (x < f.x + look->radius || x >= f.x + f.w - look->radius)) {
                cov = gfx_corner_coverage(f, look->radius, x, y);
                if (!cov)
                    continue;
            }
            int ox = 0, oy = 0;
            if (band && (x < f.x + band + look->radius || x >= f.x + f.w - band - look->radius ||
                         y < f.y + band + look->radius || y >= f.y + f.h - band - look->radius))
                lens(f, look->radius, band, x, y, &ox, &oy);
            uint32_t p = sample(g, x - ox, y - oy);
            int r = (int)(p >> 16 & 0xff), gg = (int)(p >> 8 & 0xff), b = (int)(p & 0xff);
            int luma = (r * 77 + gg * 151 + b * 28) >> 8;
            r = CLAMP(luma + (r - luma) * (int)sat / 256, 0, 255);
            gg = CLAMP(luma + (gg - luma) * (int)sat / 256, 0, 255);
            b = CLAMP(luma + (b - luma) * (int)sat / 256, 0, 255);
            uint32_t out = 0xff000000u | (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            if (tint_a)
                out = color_mix(out, look->tint | 0xff000000u, (int)tint_a);
            row[x] = cov >= 256 ? out : color_mix(row[x], out, cov * 255 / 256);
        }
    }
    if (look->liquid) {
        gfx_round_frame_gradient(c, f, look->radius, ALPHA(0xffffffffu, look->rim), ALPHA(0xffffffffu, look->rim / 4));
        gfx_round_frame_gradient(c, rect_inset(f, 1), MAX(look->radius - 1, 0), ALPHA(0xffffffffu, look->rim / 3),
                                 ALPHA(0xffffffffu, look->rim / 2));
    } else if (look->rim) {
        gfx_round_frame_gradient(c, f, look->radius, ALPHA(0xffffffffu, look->rim), ALPHA(0xffffffffu, look->rim / 3));
    }
    if (look->outline)
        gfx_round_frame(c, (struct rect){f.x - 1, f.y - 1, f.w + 2, f.h + 2}, look->radius + 1, look->outline);
}

void glass_look_for(struct glass_look *look, enum glass_kind kind, int radius)
{
    memset(look, 0, sizeof(*look));
    look->radius = radius;
    bool dark = style.dark;
    look->solid = style.glass == GLASS_SOLID;
    look->liquid = style.glass == GLASS_LIQUID;
    look->blur = look->liquid ? 3 : 6;
    look->saturation = look->liquid ? 330 : 420;
    look->band = 14;
    look->rim = look->liquid ? 170 : 60;
    look->outline = dark ? ALPHA(0, 120) : ALPHA(0, 30);
    switch (kind) {
    case GLASS_MENUBAR:
        look->tint = dark ? RGBA(30, 30, 34, 90) : RGBA(250, 250, 252, 110);
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(236, 236, 238);
        look->rim = 0;
        look->outline = 0;
        look->liquid = false;
        break;
    case GLASS_DOCK:
        look->tint = dark ? RGBA(40, 40, 44, look->liquid ? 70 : 120) : RGBA(255, 255, 255, look->liquid ? 50 : 100);
        look->solid_color = dark ? RGB(52, 52, 56) : RGB(232, 232, 236);
        break;
    case GLASS_MENU:
        look->tint = dark ? RGBA(38, 38, 42, look->liquid ? 150 : 185) : RGBA(246, 246, 248, look->liquid ? 150 : 200);
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(246, 246, 248);
        look->band = 10;
        break;
    case GLASS_PANEL:
        look->tint = dark ? RGBA(36, 36, 40, look->liquid ? 120 : 170) : RGBA(248, 248, 250, look->liquid ? 120 : 170);
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(244, 244, 246);
        break;
    case GLASS_WINDOW:
        look->tint = 0;
        look->solid_color = style.sidebar | 0xff000000u;
        look->liquid = false;
        look->rim = 0;
        look->outline = 0;
        look->blur = 6;
        look->saturation = 400;
        break;
    case GLASS_OVERLAY:
        look->tint = dark ? RGBA(0, 0, 0, 90) : RGBA(0, 0, 0, 40);
        look->solid_color = dark ? RGB(20, 20, 24) : RGB(90, 90, 100);
        look->liquid = false;
        look->rim = 0;
        look->outline = 0;
        look->blur = 7;
        look->saturation = 300;
        break;
    }
}
