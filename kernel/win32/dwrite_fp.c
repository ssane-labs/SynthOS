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

#define MAX_EDGES 16384
#define SUBSAMPLES 4

struct edge {
    float x0, y0, x1, y1;
    int dir;
};

struct raster {
    struct edge *e;
    int n;
    const float *m;
    float ppd;
};

static int floor_i(float v)
{
    int i = (int)v;
    return (float)i > v ? i - 1 : i;
}

static struct point place(const struct raster *r, struct point p)
{
    const float *m = r->m;
    return (struct point){(p.x * m[0] + p.y * m[2] + m[4]) * r->ppd, (p.x * m[1] + p.y * m[3] + m[5]) * r->ppd};
}

static void add_line(struct raster *r, struct point a, struct point b)
{
    if (a.y == b.y || r->n >= MAX_EDGES)
        return;
    struct point pa = place(r, a), pb = place(r, b);
    if (pa.y == pb.y)
        return;
    int dir = pa.y < pb.y ? 1 : -1;
    if (dir < 0) {
        struct point t = pa;
        pa = pb;
        pb = t;
    }
    r->e[r->n++] = (struct edge){pa.x, pa.y, pb.x, pb.y, dir};
}

static void add_quad(struct raster *r, struct point a, struct point c, struct point b)
{
    struct point prev = a;
    for (int i = 1; i <= 8; i++) {
        float t = (float)i / 8, u = 1 - t;
        struct point p = {u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y};
        add_line(r, prev, p);
        prev = p;
    }
}

static void raster_contour(struct raster *r, const struct point *p, const uint8_t *on, int n)
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
    struct point cur = start, ctrl = start;
    bool pending = false;
    for (int i = from; i < to; i++) {
        if (on[i]) {
            if (pending)
                add_quad(r, cur, ctrl, p[i]);
            else
                add_line(r, cur, p[i]);
            cur = p[i];
            pending = false;
        } else {
            if (pending) {
                struct point m = mid(ctrl, p[i]);
                add_quad(r, cur, ctrl, m);
                cur = m;
            }
            ctrl = p[i];
            pending = true;
        }
    }
    if (pending)
        add_quad(r, cur, ctrl, start);
    else
        add_line(r, cur, start);
}

static void cover_span(float *row, int w, float a, float b, float weight)
{
    if (b <= 0 || a >= (float)w || b <= a)
        return;
    if (a < 0)
        a = 0;
    if (b > (float)w)
        b = (float)w;
    int ia = floor_i(a), ib = floor_i(b);
    if (ia == ib) {
        row[ia] += (b - a) * weight;
        return;
    }
    row[ia] += ((float)(ia + 1) - a) * weight;
    for (int x = ia + 1; x < ib && x < w; x++)
        row[x] += weight;
    if (ib < w)
        row[ib] += (b - (float)ib) * weight;
}

/* Coverage of a glyph run: rect = {left, top, right, bottom} in pixels, *out = one byte per pixel. */
uint32_t dwrite_raster(int face, uint32_t em_bits, uint32_t ppd_bits, const uint32_t *matrix, uint32_t ox_bits,
                       uint32_t oy_bits, const uint16_t *glyphs, const uint32_t *advances, const uint32_t *offsets,
                       UINT count, int32_t rect[4], uint8_t **out)
{
    float m[6] = {1, 0, 0, 1, 0, 0};
    for (int i = 0; matrix && i < 6; i++)
        m[i] = as_float(matrix[i]);
    float em = as_float(em_bits), ox = as_float(ox_bits), oy = as_float(oy_bits);
    struct raster r = {kmalloc(MAX_EDGES * sizeof(struct edge)), 0, m, as_float(ppd_bits)};
    struct dw_point *pts = kmalloc(MAX_POINTS * sizeof(*pts));
    struct point *xy = kmalloc(MAX_POINTS * sizeof(*xy));
    uint8_t *on = kmalloc(MAX_POINTS);
    rect[0] = rect[1] = rect[2] = rect[3] = 0;
    *out = NULL;
    uint32_t hr = 0x8007000eu;
    if (!r.e || !pts || !xy || !on)
        goto done;
    if (r.ppd <= 0)
        r.ppd = 1;
    float pen = 0;
    for (UINT g = 0; g < count; g++) {
        int advance_units, units;
        int n = dwrite_glyph_outline(face, glyphs[g], pts, MAX_POINTS, &advance_units, &units);
        float scale = em / (float)units;
        float gx = ox + pen, gy = oy;
        if (offsets) {
            gx += as_float(offsets[g * 2]);
            gy -= as_float(offsets[g * 2 + 1]);
        }
        int startp = 0;
        for (int i = 0; i < n; i++) {
            xy[i] = (struct point){gx + (float)pts[i].x * scale, gy - (float)pts[i].y * scale};
            on[i] = pts[i].on;
            if (pts[i].end) {
                raster_contour(&r, xy + startp, on + startp, i + 1 - startp);
                startp = i + 1;
            }
        }
        pen += advances ? as_float(advances[g]) : (float)advance_units * scale;
    }
    hr = 0;
    if (!r.n)
        goto done;
    float x0 = r.e[0].x0, x1 = x0, y0 = r.e[0].y0, y1 = r.e[0].y1;
    for (int i = 0; i < r.n; i++) {
        const struct edge *e = &r.e[i];
        x0 = MIN(x0, MIN(e->x0, e->x1));
        x1 = MAX(x1, MAX(e->x0, e->x1));
        y0 = MIN(y0, e->y0);
        y1 = MAX(y1, e->y1);
    }
    int left = floor_i(x0), top = floor_i(y0), right = floor_i(x1) + 1, bottom = floor_i(y1) + 1;
    int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
        goto done;
    uint8_t *bits = w32_zalloc((size_t)w * (size_t)h);
    float *row = kmalloc((size_t)w * sizeof(float));
    float *xs = kmalloc(MAX_EDGES * sizeof(float));
    int *dirs = kmalloc(MAX_EDGES * sizeof(int));
    if (!bits || !row || !xs || !dirs) {
        w32_free(bits);
        kfree(row);
        kfree(xs);
        kfree(dirs);
        hr = 0x8007000eu;
        goto done;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++)
            row[x] = 0;
        for (int s = 0; s < SUBSAMPLES; s++) {
            float sy = (float)(top + y) + ((float)s + 0.5f) / SUBSAMPLES;
            int k = 0;
            for (int i = 0; i < r.n; i++) {
                const struct edge *e = &r.e[i];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                float x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0) - (float)left;
                int j = k++;
                while (j > 0 && xs[j - 1] > x) {
                    xs[j] = xs[j - 1];
                    dirs[j] = dirs[j - 1];
                    j--;
                }
                xs[j] = x;
                dirs[j] = e->dir;
            }
            int wind = 0;
            for (int i = 0; i + 1 < k; i++) {
                wind += dirs[i];
                if (wind)
                    cover_span(row, w, xs[i], xs[i + 1], 1.0f / SUBSAMPLES);
            }
        }
        for (int x = 0; x < w; x++) {
            float v = row[x] * 255 + 0.5f;
            bits[y * w + x] = v >= 255 ? 255 : v <= 0 ? 0 : (uint8_t)v;
        }
    }
    kfree(row);
    kfree(xs);
    kfree(dirs);
    rect[0] = left;
    rect[1] = top;
    rect[2] = right;
    rect[3] = bottom;
    *out = bits;
done:
    kfree(r.e);
    kfree(pts);
    kfree(xy);
    kfree(on);
    return hr;
}
