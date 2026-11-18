/* The software 3D pipeline (include/synth/raster.h). Built with SSE floating point (an _fp file). */

#if defined(__x86_64__) || defined(RS_HOST)

#include <synth/raster.h>

#include "gmath.h"

#ifdef RS_HOST /* tests/gpu: the same code on the build machine */
#include <stdlib.h>
#include <string.h>
#define kmalloc malloc
#define kfree free
static void hal_parallel(void (*fn)(void *ctx, int from, int to), void *ctx, int count, int grain)
{
    (void)grain;
    fn(ctx, 0, count);
}
static int hal_parallel_cpus(void)
{
    return 1;
}
#else
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>
#endif

#define TILE 64
#define SUB 16 /* subpixel steps: positions are 28.4 fixed point */
#define FLOATS (4 + RS_MAX_VARY * 4)

/* a clipped vertex: clip-space position, then the varyings */
struct cvert {
    float v[FLOATS];
};

/* a triangle ready to draw: fixed-point screen positions, edges, and what is interpolated */
struct tri {
    int64_t x[3], y[3];
    int64_t bias[3];
    int minx, miny, maxx, maxy;
    float inv_area;
    float z[3], iw[3];                     /* depth after the viewport, 1/w */
    bool front;
    float attr[3][RS_MAX_VARY * 4];        /* varyings divided by w (screen-space ones as they are) */
};

struct draw {
    struct rs_target *t;
    const struct rs_state *s;
    struct tri *tris;
    int ntris, cap;
    int tiles_x, tiles_y;
    int *bin_count, *bin_start, *bins;
    int cminx, cminy, cmaxx, cmaxy; /* the clip rectangle: target, scissor */
    bool no_cull;                   /* lines and points: both sides drawn */
    float vx, vy, vw, vh, min_z, max_z;
    uint64_t quads, pixels;
    /* scratch memory for the shaders: one slot per processor at work, taken from a bitmap */
    uint8_t *scratch;
    size_t slot_bytes;
    uint64_t slots_free;
};

static void *take_slot(struct draw *d, int *slot)
{
    *slot = -1;
    if (!d->scratch)
        return NULL;
    for (;;) {
        uint64_t f = __atomic_load_n(&d->slots_free, __ATOMIC_ACQUIRE);
        if (!f) {
            __asm__ volatile("pause");
            continue;
        }
        int i = __builtin_ctzll(f);
        if (__atomic_compare_exchange_n(&d->slots_free, &f, f & ~(1ull << i), false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            *slot = i;
            return d->scratch + (size_t)i * d->slot_bytes;
        }
    }
}

static void give_slot(struct draw *d, int slot)
{
    if (slot >= 0)
        __atomic_or_fetch(&d->slots_free, 1ull << slot, __ATOMIC_RELEASE);
}

static struct rs_stats stats;

void rs_get_stats(struct rs_stats *out)
{
    *out = stats;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void rs_clear(struct rs_target *t, uint32_t color, bool clear_color, float depth, bool clear_depth)
{
    for (int y = 0; y < t->h; y++) {
        if (clear_color && t->color)
            for (int x = 0; x < t->w; x++)
                t->color[y * t->stride + x] = color;
        if (clear_depth && t->depth)
            for (int x = 0; x < t->w; x++)
                t->depth[y * t->depth_stride + x] = depth;
    }
}

/* ---- clipping against the six planes of the clip volume (-w <= x, y <= w, 0 <= z <= w) */

static float plane_dist(const float *v, int p)
{
    switch (p) {
    case 0: return v[3] + v[0];
    case 1: return v[3] - v[0];
    case 2: return v[3] + v[1];
    case 3: return v[3] - v[1];
    case 4: return v[2];
    default: return v[3] - v[2];
    }
}

static int clip_polygon(struct cvert *in, int n, struct cvert *out, int floats)
{
    struct cvert buf[2][12];
    struct cvert *src = in;
    for (int p = 0; p < 6 && n >= 3; p++) {
        struct cvert *dst = p == 5 ? out : buf[p & 1];
        int m = 0;
        for (int i = 0; i < n && m < 11; i++) {
            const struct cvert *a = &src[i], *b = &src[(i + 1) % n];
            float da = plane_dist(a->v, p), db = plane_dist(b->v, p);
            if (da >= 0)
                dst[m++] = *a;
            if ((da >= 0) != (db >= 0) && m < 11) {
                float k = da / (da - db);
                for (int f = 0; f < floats; f++)
                    dst[m].v[f] = a->v[f] + (b->v[f] - a->v[f]) * k;
                m++;
            }
        }
        n = m;
        src = dst;
    }
    if (src != out)
        for (int i = 0; i < n; i++)
            out[i] = src[i];
    return n >= 3 ? n : 0;
}

/* ---- triangle setup */

static bool add_tri(struct draw *d, const struct cvert *a, const struct cvert *b, const struct cvert *c,
                    const float *flat)
{
    const struct rs_state *s = d->s;
    const struct cvert *v[3] = {a, b, c};
    struct tri t;
    for (int i = 0; i < 3; i++) {
        float w = v[i]->v[3], iw = 1.0f / w;
        float sx = d->vx + (v[i]->v[0] * iw + 1.0f) * 0.5f * d->vw;
        float sy = d->vy + (1.0f - v[i]->v[1] * iw) * 0.5f * d->vh;
        t.x[i] = (int64_t)(sx * SUB + (sx >= 0 ? 0.5f : -0.5f));
        t.y[i] = (int64_t)(sy * SUB + (sy >= 0 ? 0.5f : -0.5f));
        t.z[i] = d->min_z + clampf(v[i]->v[2] * iw, 0.0f, 1.0f) * (d->max_z - d->min_z);
        t.iw[i] = iw;
        for (int f = 0; f < s->nvary * 4; f++) {
            uint32_t bit = 1u << (f >> 2);
            t.attr[i][f] = s->flat & bit ? flat[f] : s->noperspective & bit ? v[i]->v[4 + f] : v[i]->v[4 + f] * iw;
        }
    }
    int64_t area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.y[1] - t.y[0]) * (t.x[2] - t.x[0]);
    if (area == 0)
        return false;
    /* y points down: a positive area turns clockwise on the screen */
    bool cw = area > 0, front = s->front_ccw ? !cw : cw;
    t.front = front;
    if (!d->no_cull && ((s->cull == RS_CULL_BACK && !front) || (s->cull == RS_CULL_FRONT && front))) {
        stats.culled++;
        return false;
    }
    if (area < 0) { /* make it clockwise: swap the last two corners */
        int64_t tx = t.x[1], ty = t.y[1];
        t.x[1] = t.x[2], t.y[1] = t.y[2], t.x[2] = tx, t.y[2] = ty;
        float tz = t.z[1], tw = t.iw[1];
        t.z[1] = t.z[2], t.iw[1] = t.iw[2], t.z[2] = tz, t.iw[2] = tw;
        for (int f = 0; f < s->nvary * 4; f++) {
            float k = t.attr[1][f];
            t.attr[1][f] = t.attr[2][f];
            t.attr[2][f] = k;
        }
        area = -area;
    }
    t.inv_area = 1.0f / (float)area;
    /* edge i runs from corner i+1 to corner i+2; E >= 0 inside. The top-left rule: a pixel exactly on an edge
       belongs to the triangle only for a top or a left edge. */
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3, k = (i + 2) % 3;
        int64_t nx = -(t.y[k] - t.y[j]), ny = t.x[k] - t.x[j]; /* points inside */
        bool top_left = nx > 0 || (nx == 0 && ny > 0);
        t.bias[i] = top_left ? 0 : -1;
    }
    int64_t minx = t.x[0], maxx = t.x[0], miny = t.y[0], maxy = t.y[0];
    for (int i = 1; i < 3; i++) {
        minx = t.x[i] < minx ? t.x[i] : minx;
        maxx = t.x[i] > maxx ? t.x[i] : maxx;
        miny = t.y[i] < miny ? t.y[i] : miny;
        maxy = t.y[i] > maxy ? t.y[i] : maxy;
    }
    t.minx = (int)((minx) / SUB) - 1;
    t.miny = (int)((miny) / SUB) - 1;
    t.maxx = (int)((maxx + SUB - 1) / SUB) + 1;
    t.maxy = (int)((maxy + SUB - 1) / SUB) + 1;
    t.minx = t.minx < d->cminx ? d->cminx : t.minx;
    t.miny = t.miny < d->cminy ? d->cminy : t.miny;
    t.maxx = t.maxx > d->cmaxx ? d->cmaxx : t.maxx;
    t.maxy = t.maxy > d->cmaxy ? d->cmaxy : t.maxy;
    if (t.minx >= t.maxx || t.miny >= t.maxy)
        return false;
    if (d->ntris == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        struct tri *more = kmalloc(sizeof(struct tri) * (size_t)cap);
        if (!more)
            return false;
        if (d->tris) {
            memcpy(more, d->tris, sizeof(struct tri) * (size_t)d->ntris);
            kfree(d->tris);
        }
        d->tris = more;
        d->cap = cap;
    }
    d->tris[d->ntris++] = t;
    stats.triangles++;
    return true;
}

/* ---- per pixel */

static int64_t edge(const struct tri *t, int i, int64_t px, int64_t py)
{
    int j = (i + 1) % 3, k = (i + 2) % 3;
    return (t->x[k] - t->x[j]) * (py - t->y[j]) - (t->y[k] - t->y[j]) * (px - t->x[j]);
}

static bool depth_pass(enum rs_compare f, float z, float have)
{
    switch (f) {
    case RS_NEVER: return false;
    case RS_LESS: return z < have;
    case RS_EQUAL: return z == have;
    case RS_LESS_EQUAL: return z <= have;
    case RS_GREATER: return z > have;
    case RS_NOT_EQUAL: return z != have;
    case RS_GREATER_EQUAL: return z >= have;
    default: return true;
    }
}

static uint32_t to8(float v)
{
    return (uint32_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static void write_pixel(const struct rs_state *s, uint32_t *dst, const float *c)
{
    float r = c[0], g = c[1], b = c[2], a = c[3];
    if (s->blend != RS_BLEND_NONE) {
        uint32_t p = *dst;
        float dr = (float)(p >> 16 & 255) / 255.0f, dg = (float)(p >> 8 & 255) / 255.0f, db = (float)(p & 255) / 255.0f,
              da = (float)(p >> 24) / 255.0f;
        float sa = clampf(a, 0.0f, 1.0f);
        switch (s->blend) {
        case RS_BLEND_ALPHA:
            r = r * sa + dr * (1 - sa), g = g * sa + dg * (1 - sa), b = b * sa + db * (1 - sa), a = sa + da * (1 - sa);
            break;
        case RS_BLEND_ADD: r += dr, g += dg, b += db, a += da; break;
        default: r += dr * (1 - sa), g += dg * (1 - sa), b += db * (1 - sa), a = sa + da * (1 - sa); break;
        }
    }
    uint32_t out = to8(a) << 24 | to8(r) << 16 | to8(g) << 8 | to8(b);
    unsigned m = s->write_mask ? s->write_mask : 15;
    if (m != 15) {
        uint32_t keep = (m & 1 ? 0 : 0xff0000u) | (m & 2 ? 0 : 0xff00u) | (m & 4 ? 0 : 0xffu) | (m & 8 ? 0 : 0xff000000u);
        out = (out & ~keep) | (*dst & keep);
    }
    *dst = out;
}

static void draw_tri_in_tile(struct draw *d, const struct tri *t, int x0, int y0, int x1, int y1, void *scratch)
{
    const struct rs_state *s = d->s;
    struct rs_target *tg = d->t;
    int nf = s->nvary * 4;
    x0 = (t->minx > x0 ? t->minx : x0) & ~1;
    y0 = (t->miny > y0 ? t->miny : y0) & ~1;
    x1 = t->maxx < x1 ? t->maxx : x1;
    y1 = t->maxy < y1 ? t->maxy : y1;
    struct rs_quad q;
    float color[4][4];
    for (int y = y0; y < y1; y += 2)
        for (int x = x0; x < x1; x += 2) {
            int64_t e[4][3];
            unsigned mask = 0;
            for (int p = 0; p < 4; p++) {
                int px = x + (p & 1), py = y + (p >> 1);
                int64_t cx = (int64_t)px * SUB + SUB / 2, cy = (int64_t)py * SUB + SUB / 2;
                bool in = px < x1 && py < y1 && px >= d->cminx && py >= d->cminy; /* x1, y1: this tile's end */
                for (int i = 0; i < 3; i++) {
                    e[p][i] = edge(t, i, cx, cy);
                    in = in && e[p][i] + t->bias[i] >= 0;
                }
                if (in)
                    mask |= 1u << p;
            }
            if (!mask)
                continue;
            q.x = x, q.y = y;
            q.front = t->front;
            for (int p = 0; p < 4; p++) {
                float l0 = (float)e[p][0] * t->inv_area, l1 = (float)e[p][1] * t->inv_area, l2 = 1.0f - l0 - l1;
                q.depth[p] = l0 * t->z[0] + l1 * t->z[1] + l2 * t->z[2];
                float w = 1.0f / (l0 * t->iw[0] + l1 * t->iw[1] + l2 * t->iw[2]);
                q.w[p] = w;
                for (int f = 0; f < nf; f++) {
                    uint32_t bit = 1u << (f >> 2);
                    float v = l0 * t->attr[0][f] + l1 * t->attr[1][f] + l2 * t->attr[2][f];
                    q.in[p][f] = s->flat & bit ? t->attr[0][f] : s->noperspective & bit ? v : v * w;
                }
            }
            if (s->quad) {
                d->quads++;
                s->quad(s->ctx, &q, mask, scratch);
                d->pixels += (uint64_t)((mask & 1) + (mask >> 1 & 1) + (mask >> 2 & 1) + (mask >> 3 & 1));
                continue;
            }
            if (s->depth_test && tg->depth) /* before the shader: no shader here writes depth */
                for (int p = 0; p < 4; p++)
                    if (mask & (1u << p)) {
                        int px = x + (p & 1), py = y + (p >> 1);
                        if (!depth_pass(s->depth_func, q.depth[p], tg->depth[py * tg->depth_stride + px]))
                            mask &= ~(1u << p);
                    }
            if (!mask)
                continue;
            d->quads++;
            if (s->ps)
                s->ps(s->ctx, &q, color, &mask, scratch);
            for (int p = 0; p < 4; p++) {
                if (!(mask & (1u << p)))
                    continue;
                int px = x + (p & 1), py = y + (p >> 1);
                if (tg->depth && s->depth_write)
                    tg->depth[py * tg->depth_stride + px] = q.depth[p];
                if (tg->color && s->ps)
                    write_pixel(s, &tg->color[py * tg->stride + px], color[p]);
                d->pixels++;
            }
        }
}

/* the tiles are drawn in parallel: every one by one core, its triangles in the order they came */
static void draw_tiles(void *ctx, int from, int to)
{
    struct draw *d = ctx;
    uint64_t quads = 0, pixels = 0;
    struct draw local = *d;
    local.quads = local.pixels = 0;
    int slot;
    void *scratch = take_slot(d, &slot);
    for (int tile = from; tile < to; tile++) {
        int tx = tile % d->tiles_x, ty = tile / d->tiles_x;
        int x0 = tx * TILE, y0 = ty * TILE;
        int x1 = x0 + TILE < d->cmaxx ? x0 + TILE : d->cmaxx, y1 = y0 + TILE < d->cmaxy ? y0 + TILE : d->cmaxy;
        for (int k = 0; k < d->bin_count[tile]; k++)
            draw_tri_in_tile(&local, &d->tris[d->bins[d->bin_start[tile] + k]], x0, y0, x1, y1, scratch);
    }
    give_slot(d, slot);
    quads = local.quads, pixels = local.pixels;
    __atomic_add_fetch(&d->quads, quads, __ATOMIC_RELAXED);
    __atomic_add_fetch(&d->pixels, pixels, __ATOMIC_RELAXED);
}

/* ---- primitives */

static void emit_tri(struct draw *d, struct cvert poly[3], int floats)
{
    const struct rs_state *s = d->s;
    bool inside = true, outside = false;
    for (int p = 0; p < 6; p++) {
        int behind = 0;
        for (int c = 0; c < 3; c++)
            if (plane_dist(poly[c].v, p) < 0)
                behind++, inside = false;
        if (behind == 3)
            outside = true;
    }
    if (outside)
        return;
    float flat[RS_MAX_VARY * 4];
    if (s->flat) /* the first vertex of the triangle gives the flat varyings, also after clipping */
        memcpy(flat, poly[0].v + 4, sizeof(float) * (size_t)s->nvary * 4);
    if (inside) {
        add_tri(d, &poly[0], &poly[1], &poly[2], flat);
        return;
    }
    struct cvert out[12];
    int n = clip_polygon(poly, 3, out, floats);
    stats.clipped++;
    for (int c = 1; c + 1 < n; c++)
        add_tri(d, &out[0], &out[c], &out[c + 1], flat);
}

/* a line as a quad one pixel wide, or a point as a one-pixel square, from shaded vertices */
static void emit_line(struct draw *d, const float *va, const float *vb, int floats, bool point)
{
    struct cvert q[4]; /* the ends a, b first, then the corners */
    memcpy(q[2].v, va, sizeof(float) * (size_t)floats);
    memcpy(q[3].v, vb, sizeof(float) * (size_t)floats);
    struct cvert *a = &q[2], *b = &q[3];
    float da = a->v[2], db = b->v[2]; /* the near plane first, so that w is positive at both ends */
    if (da < 0 && (point || db < 0))
        return;
    if (!point && (da < 0 || db < 0)) {
        float k = da / (da - db);
        struct cvert *in = da < 0 ? a : b;
        for (int f = 0; f < floats; f++)
            in->v[f] = a->v[f] + (b->v[f] - a->v[f]) * k;
    }
    if (a->v[3] <= 1e-6f || b->v[3] <= 1e-6f)
        return;
    float ox, oy; /* half a pixel across the line, in normalized device units */
    if (point) {
        ox = 1.0f / d->vw, oy = 1.0f / d->vh;
    } else {
        float dx = (b->v[0] / b->v[3] - a->v[0] / a->v[3]) * 0.5f * d->vw;
        float dy = (b->v[1] / b->v[3] - a->v[1] / a->v[3]) * 0.5f * d->vh;
        float len = fsqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
            return;
        ox = -dy / len / d->vw, oy = dx / len / d->vh;
    }
    /* corners 0 and 1 from a, b (or the point) on one side; then 2 and 3 on the other, in place */
    q[0] = *a, q[1] = point ? *a : *b;
    float wa = a->v[3], wb = q[1].v[3];
    if (point) {
        q[0].v[0] -= ox * wa, q[0].v[1] -= oy * wa;
        q[1].v[0] += ox * wa, q[1].v[1] -= oy * wa;
        q[2].v[0] += ox * wa, q[2].v[1] += oy * wa;
        q[3] = q[2], q[3].v[0] -= 2 * ox * wa;
    } else {
        q[0].v[0] += ox * wa, q[0].v[1] += oy * wa;
        q[1].v[0] += ox * wb, q[1].v[1] += oy * wb;
        q[2] = *b, q[2].v[0] -= ox * wb, q[2].v[1] -= oy * wb;
        q[3] = q[0], q[3].v[0] -= 2 * ox * wa, q[3].v[1] -= 2 * oy * wa;
    }
    d->no_cull = true;
    emit_tri(d, q, floats);   /* 0 1 2 */
    q[1] = q[3];
    emit_tri(d, q, floats);   /* 0 3 2 */
    d->no_cull = false;
}

/* ---- the draw */

struct shade {
    const struct rs_state *s;
    struct draw *d;
    float *out;
    uint32_t first;
    int floats, count;
};

/* in groups of four vertices */
static void shade_vertices(void *ctx, int from, int to)
{
    struct shade *sh = ctx;
    int slot;
    void *scratch = take_slot(sh->d, &slot);
    for (int g = from; g < to; g++) {
        int i = g * 4, n = sh->count - i < 4 ? sh->count - i : 4;
        sh->s->vs(sh->s->ctx, sh->first + (uint32_t)i, n, sh->out + (size_t)i * (size_t)sh->floats, scratch);
    }
    give_slot(sh->d, slot);
}

int rs_draw(struct rs_target *t, const struct rs_state *s, enum rs_topology topology, uint32_t count,
            const uint32_t *indices, int32_t base_vertex)
{
    if (!s->vs || s->nvary < 0 || s->nvary > RS_MAX_VARY || !count)
        return 0;
    struct draw d = {.t = t, .s = s};
    d.cminx = 0, d.cminy = 0, d.cmaxx = t->w, d.cmaxy = t->h;
    if (s->scissor) {
        d.cminx = s->sx > 0 ? s->sx : 0;
        d.cminy = s->sy > 0 ? s->sy : 0;
        d.cmaxx = s->sx + s->sw < t->w ? s->sx + s->sw : t->w;
        d.cmaxy = s->sy + s->sh < t->h ? s->sy + s->sh : t->h;
        if (d.cminx >= d.cmaxx || d.cminy >= d.cmaxy)
            return 0;
    }
    d.vx = s->vw > 0 ? s->vx : 0, d.vy = s->vw > 0 ? s->vy : 0;
    d.vw = s->vw > 0 ? s->vw : (float)t->w, d.vh = s->vw > 0 ? s->vh : (float)t->h;
    d.min_z = s->min_z, d.max_z = s->min_z == 0 && s->max_z == 0 ? 1.0f : s->max_z; /* none given: 0..1 */
    /* every vertex the draw uses, shaded once */
    uint32_t lo = 0, hi = count - 1;
    if (indices) {
        lo = 0xffffffffu, hi = 0;
        for (uint32_t i = 0; i < count; i++) {
            lo = indices[i] < lo ? indices[i] : lo;
            hi = indices[i] > hi ? indices[i] : hi;
        }
    }
    int floats = 4 + s->nvary * 4;
    uint32_t nverts = hi - lo + 1;
    if (nverts > (1u << 24))
        return -1;
    float *verts = kmalloc(sizeof(float) * (size_t)nverts * (size_t)floats);
    if (!verts)
        return -1;
    if (s->scratch) {
        int slots = hal_parallel_cpus() < 64 ? hal_parallel_cpus() : 64;
        d.slot_bytes = (s->scratch + 63) & ~(size_t)63;
        d.scratch = kmalloc(d.slot_bytes * (size_t)slots);
        if (!d.scratch) {
            kfree(verts);
            return -1;
        }
        d.slots_free = slots == 64 ? ~0ull : (1ull << slots) - 1;
    }
    struct shade sh = {s, &d, verts, (uint32_t)((int32_t)lo + base_vertex), floats, (int)nverts};
    hal_parallel(shade_vertices, &sh, (int)(nverts + 3) / 4, 64);
    /* the primitives: triangles clipped where they cross the clip volume; lines and points as small quads */
#define VERT(i) (verts + (size_t)((indices ? indices[(i)] : (i)) - lo) * (size_t)floats)
    switch (topology) {
    case RS_TRIANGLES:
    case RS_STRIP:
    case RS_TRIANGLES_ADJ: {
        uint32_t ntri = topology == RS_STRIP ? (count >= 3 ? count - 2 : 0) : topology == RS_TRIANGLES_ADJ ? count / 6
                                                                                                         : count / 3;
        for (uint32_t i = 0; i < ntri; i++) {
            uint32_t k[3];
            if (topology == RS_STRIP)
                k[0] = i, k[1] = i + 1 + (i & 1), k[2] = i + 2 - (i & 1);
            else if (topology == RS_TRIANGLES_ADJ)
                k[0] = i * 6, k[1] = i * 6 + 2, k[2] = i * 6 + 4;
            else
                k[0] = i * 3, k[1] = i * 3 + 1, k[2] = i * 3 + 2;
            if (s->wireframe) {
                for (int e = 0; e < 3; e++)
                    emit_line(&d, VERT(k[e]), VERT(k[(e + 1) % 3]), floats, false);
                continue;
            }
            struct cvert poly[3];
            for (int c = 0; c < 3; c++)
                memcpy(poly[c].v, VERT(k[c]), sizeof(float) * (size_t)floats);
            emit_tri(&d, poly, floats);
        }
        break;
    }
    case RS_LINES:
    case RS_LINES_ADJ:
        for (uint32_t i = 0; i + 1 < count; i += topology == RS_LINES ? 2 : 4)
            emit_line(&d, VERT(i + (topology == RS_LINES ? 0 : 1)), VERT(i + (topology == RS_LINES ? 1 : 2)), floats,
                      false);
        break;
    case RS_LINE_STRIP:
    case RS_LINE_STRIP_ADJ: {
        uint32_t first = topology == RS_LINE_STRIP ? 0 : 1, last = topology == RS_LINE_STRIP ? count : count - 1;
        for (uint32_t i = first; i + 1 < last; i++)
            emit_line(&d, VERT(i), VERT(i + 1), floats, false);
        break;
    }
    case RS_POINTS:
        for (uint32_t i = 0; i < count; i++)
            emit_line(&d, VERT(i), VERT(i), floats, true);
        break;
    }
#undef VERT
    kfree(verts);
    if (!d.ntris) {
        kfree(d.scratch);
        return 0;
    }
    /* binning: which tiles each triangle touches */
    d.tiles_x = (t->w + TILE - 1) / TILE, d.tiles_y = (t->h + TILE - 1) / TILE;
    int tiles = d.tiles_x * d.tiles_y;
    d.bin_count = kmalloc(sizeof(int) * (size_t)tiles * 2);
    if (!d.bin_count) {
        kfree(d.tris);
        kfree(d.scratch);
        return -1;
    }
    d.bin_start = d.bin_count + tiles;
    memset(d.bin_count, 0, sizeof(int) * (size_t)tiles);
    size_t total = 0;
    for (int i = 0; i < d.ntris; i++) {
        const struct tri *tr = &d.tris[i];
        for (int ty = tr->miny / TILE; ty <= (tr->maxy - 1) / TILE; ty++)
            for (int tx = tr->minx / TILE; tx <= (tr->maxx - 1) / TILE; tx++)
                d.bin_count[ty * d.tiles_x + tx]++, total++;
    }
    d.bins = kmalloc(sizeof(int) * (total ? total : 1));
    if (!d.bins) {
        kfree(d.bin_count);
        kfree(d.tris);
        kfree(d.scratch);
        return -1;
    }
    int at = 0;
    for (int i = 0; i < tiles; i++) {
        d.bin_start[i] = at;
        at += d.bin_count[i];
        d.bin_count[i] = 0;
    }
    for (int i = 0; i < d.ntris; i++) {
        const struct tri *tr = &d.tris[i];
        for (int ty = tr->miny / TILE; ty <= (tr->maxy - 1) / TILE; ty++)
            for (int tx = tr->minx / TILE; tx <= (tr->maxx - 1) / TILE; tx++) {
                int tile = ty * d.tiles_x + tx;
                d.bins[d.bin_start[tile] + d.bin_count[tile]++] = i;
            }
    }
    hal_parallel(draw_tiles, &d, tiles, 1);
    stats.quads += d.quads;
    stats.pixels += d.pixels;
    int drawn = d.ntris;
    kfree(d.bins);
    kfree(d.bin_count);
    kfree(d.tris);
    kfree(d.scratch);
    return drawn;
}

#endif
