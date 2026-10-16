#include <synth/heap.h>
#include <synth/string.h>

#include "internal.h"


static struct glass *glass_list;

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

static bool capture(struct canvas *c, struct glass *g, int blur, int scale)
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
    if (!g->small.px || g->small.w != sw || g->small.h != sh) {
        canvas_free(&g->small);
        if (!canvas_alloc(&g->small, sw, sh))
            return false;
    }
    int step = scale >= 4 ? 2 : 1;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            unsigned r = 0, gg = 0, b = 0, n = 0;
            int x0 = src.x + x * scale, y0 = src.y + y * scale;
            int x1 = MIN(x0 + scale, src.x + src.w), y1 = MIN(y0 + scale, src.y + src.h);
            for (int yy = y0; yy < y1; yy += step)
                for (int xx = x0; xx < x1; xx += step) {
                    uint32_t p = c->px[yy * c->stride + xx];
                    r += p >> 16 & 0xff;
                    gg += p >> 8 & 0xff;
                    b += p & 0xff;
                    n++;
                }
            n = MAX(n, 1u);
            g->small.px[y * sw + x] = 0xff000000u | (r / n) << 16 | (gg / n) << 8 | (b / n);
        }
    if (blur > 0) {
        uint32_t *tmp = kmalloc((size_t)MAX(sw, sh) * 4);
        if (tmp) {
            for (int pass = 0; pass < 2; pass++) {
                box_pass(g->small.px, sw, sh, sw, 1, 0, blur, tmp);
                box_pass(g->small.px, sw, sh, sw, 0, 1, blur, tmp);
            }
            kfree(tmp);
        }
    }
    g->src = src;
    g->scale = scale;
    g->valid = true;
    g->stamp++;
    return true;
}

void glass_prepare(struct canvas *c, struct glass *g, const struct glass_look *look)
{
    int scale = look->scale ? look->scale : 4;
    if (!look->solid && (!g->valid || g->scale != scale))
        capture(c, g, look->blur, scale);
}

static uint32_t sample(const struct glass *g, int fx, int fy)
{
    fx = (fx - g->src.x * 256) / g->scale - 128;
    fy = (fy - g->src.y * 256) / g->scale - 128;
    int w = g->small.w, h = g->small.h;
    fx = CLAMP(fx, 0, (w - 1) * 256);
    fy = CLAMP(fy, 0, (h - 1) * 256);
    int x0 = fx >> 8, y0 = fy >> 8, tx = fx & 255, ty = fy & 255;
    int x1 = MIN(x0 + 1, w - 1), y1 = MIN(y0 + 1, h - 1);
    const uint32_t *px = g->small.px;
    uint32_t a = px[y0 * w + x0], b = px[y0 * w + x1], c = px[y1 * w + x0], d = px[y1 * w + x1];
    uint32_t wa = (uint32_t)((256 - tx) * (256 - ty)), wb = (uint32_t)(tx * (256 - ty)), wc = (uint32_t)((256 - tx) * ty),
             wd = (uint32_t)(tx * ty);
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
    int band = look->liquid && !look->shape ? MIN(look->band, MIN(f.w, f.h) / 2) : 0;
    int band16 = MAX(band, 1) * 16;
    int refract = look->refract * 256;
    int inner = band + radius;
    int lift = look->brightness;
    const struct geo *geo = band ? geometry(g, f, radius) : NULL;
    for (int y = f.y; y < f.y + f.h; y++) {
        uint32_t *row = dst + (y - f.y) * stride - f.x;
        bool edge_row = y < f.y + inner || y >= f.y + f.h - inner;
        for (int x = f.x; x < f.x + f.w; x++) {
            int cov = 256, d = band16, nx = 0, ny = 0;
            int shape_rim = 0;
            if (look->shape) {
                cov = look->shape(x, y, look->shape_ctx);
                if (cov <= 0) {
                    row[x] = 0;
                    continue;
                }
                const void *k = look->shape_ctx;
                int inner = MIN(MIN(look->shape(x - 2, y, k), look->shape(x + 2, y, k)),
                                MIN(look->shape(x, y - 2, k), look->shape(x, y + 2, k)));
                shape_rim = (256 - inner) * (y < f.y + f.h / 2 ? 150 : 90) >> 8;
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
            uint32_t p;
            int spec = 0;
            if (band && d < band16) {
                int i = MAX(d, 0) * 64 / band16;
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
                int light = (-nx * 181 - ny * 181) >> 8;
                int dir = light > 0 ? light * light >> 8 : (light * light >> 8) * 5 / 8;
                spec = rim_lut[i] * (look->rim * (64 + dir * 3 / 4) >> 8) >> 8;
                if (look->pointer_light) {
                    int mx = mouse_x - x, my = mouse_y - y;
                    int dist = mx * mx + my * my, reach = look->pointer_light * look->pointer_light;
                    if (dist < reach)
                        spec += rim_lut[i] * (reach - dist) / reach * 3 / 4;
                }
            } else {
                p = sample(g, x * 256 + 128, y * 256 + 128);
            }
            int r = (int)(p >> 16 & 0xff), gg = (int)(p >> 8 & 0xff), b = (int)(p & 0xff);
            int luma = (r * 77 + gg * 151 + b * 28) >> 8;
            r = clamp8(luma + ((r - luma) * (int)sat >> 8) + lift);
            gg = clamp8(luma + ((gg - luma) * (int)sat >> 8) + lift);
            b = clamp8(luma + ((b - luma) * (int)sat >> 8) + lift);
            uint32_t out = (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            if (tint_a)
                out = color_mix(out | 0xff000000u, look->tint | 0xff000000u, (int)tint_a) & 0xffffffu;
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
    if (look->solid || (!g->valid && !capture(c, g, look->blur, scale))) {
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
        for (int x = area.x; x < area.x + area.w; x++) {
            uint32_t p = src[x];
            unsigned a = p >> 24;
            if (a == 255)
                row[x] = p;
            else if (a)
                row[x] = color_mix(row[x], p | 0xff000000u, (int)a);
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
    look->scale = 4;
    look->blur = look->liquid ? 2 : 6;
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
            look->scale = 2;
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
                           look->liquid ? 185 : 200);
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(246, 246, 248);
        look->band = 12;
        look->refract = 10;
        look->blur = look->liquid ? 4 : 6;
        if (look->liquid)
            look->outline = dark ? ALPHA(0, 90) : ALPHA(0, 14);
        break;
    case GLASS_PANEL:
        look->tint = ALPHA(color_mix(dark ? RGB(58, 54, 68) : RGB(250, 250, 252), style.tint | 0xff000000u, dark ? 40 : 34),
                           look->liquid ? (dark ? 120 : 120) : 170);
        look->brightness = dark ? 10 : 10;
        look->solid_color = dark ? RGB(44, 44, 48) : RGB(244, 244, 246);
        look->blur = look->liquid ? 4 : 6;
        look->band = 18;
        look->refract = 16;
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
        look->blur = 6;
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
}
