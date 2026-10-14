#include <synth/heap.h>

#include "win32.h"

#define MAX_POINTS 4096

struct point {
    float x, y;
};

struct sink;
typedef void(WINAPI *fill_fn)(struct sink *, int);
typedef void(WINAPI *begin_fn)(struct sink *, struct point, int);
typedef void(WINAPI *lines_fn)(struct sink *, const struct point *, UINT);
typedef void(WINAPI *beziers_fn)(struct sink *, const struct point *, UINT);
typedef void(WINAPI *end_fn)(struct sink *, int);

struct sink {
    void **vtbl;
};

static float as_float(uint32_t v)
{
    union {
        uint32_t u;
        float f;
    } c = {v};
    return c.f;
}

static struct point mid(struct point a, struct point b)
{
    return (struct point){(a.x + b.x) / 2, (a.y + b.y) / 2};
}

static void quad(struct sink *s, struct point p0, struct point c, struct point p1)
{
    struct point b[3] = {{p0.x + (c.x - p0.x) * 2 / 3, p0.y + (c.y - p0.y) * 2 / 3},
                         {p1.x + (c.x - p1.x) * 2 / 3, p1.y + (c.y - p1.y) * 2 / 3},
                         p1};
    ((beziers_fn)s->vtbl[7])(s, b, 1);
}

static void contour(struct sink *s, const struct point *p, const uint8_t *on, int n)
{
    if (n < 2)
        return;
    int from = 0, to = n;
    struct point start;
    if (on[0])
        start = p[0], from = 1;
    else if (on[n - 1])
        start = p[n - 1], to = n - 1;
    else
        start = mid(p[n - 1], p[0]);
    ((begin_fn)s->vtbl[5])(s, start, 0);
    struct point cur = start, ctrl = start;
    bool pending = false;
    for (int i = from; i < to; i++) {
        if (on[i]) {
            if (pending)
                quad(s, cur, ctrl, p[i]);
            else
                ((lines_fn)s->vtbl[6])(s, &p[i], 1);
            cur = p[i];
            pending = false;
        } else {
            if (pending) {
                struct point m = mid(ctrl, p[i]);
                quad(s, cur, ctrl, m);
                cur = m;
            }
            ctrl = p[i];
            pending = true;
        }
    }
    if (pending)
        quad(s, cur, ctrl, start);
    ((end_fn)s->vtbl[8])(s, 1);
}

uint32_t dwrite_outline(int face, uint32_t em_bits, const uint16_t *glyphs, const uint32_t *advances,
                        const void *offsets, UINT count, BOOL rtl, void *out)
{
    struct sink *s = out;
    if (!s || (count && !glyphs))
        return 0x80070057u;
    struct dw_point *pts = kmalloc(MAX_POINTS * sizeof(*pts));
    struct point *xy = kmalloc(MAX_POINTS * sizeof(*xy));
    uint8_t *on = kmalloc(MAX_POINTS);
    if (!pts || !xy || !on) {
        kfree(pts);
        kfree(xy);
        kfree(on);
        return 0x8007000eu;
    }
    ((fill_fn)s->vtbl[3])(s, 1);
    float em = as_float(em_bits), pen = 0;
    const float *offs = offsets;
    for (UINT g = 0; g < count; g++) {
        int advance_units, units;
        int n = dwrite_glyph_outline(face, glyphs[g], pts, MAX_POINTS, &advance_units, &units);
        float scale = em / (float)units;
        float advance = advances ? as_float(advances[g]) : (float)advance_units * scale;
        if (rtl)
            pen -= advance;
        float ox = pen, oy = 0;
        if (offs) {
            ox += rtl ? -offs[g * 2] : offs[g * 2];
            oy -= offs[g * 2 + 1];
        }
        int startp = 0;
        for (int i = 0; i < n; i++) {
            xy[i] = (struct point){ox + (float)pts[i].x * scale, oy - (float)pts[i].y * scale};
            on[i] = pts[i].on;
            if (pts[i].end) {
                contour(s, xy + startp, on + startp, i + 1 - startp);
                startp = i + 1;
            }
        }
        if (!rtl)
            pen += advance;
    }
    kfree(pts);
    kfree(xy);
    kfree(on);
    return 0;
}
