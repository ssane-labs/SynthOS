/* The fixed-function tessellator (include/synth/tess.h). Built with SSE floating point (an _fp file). */

#if defined(__x86_64__) || defined(RS_HOST)

#include <synth/tess.h>

#include "gmath.h"

#ifdef RS_HOST
#include <stdlib.h>
#include <string.h>
#define kmalloc malloc
#define kfree free
#else
#include <synth/heap.h>
#include <synth/string.h>
#endif

#define MAX_SEGS 64

static uint32_t point(struct tess_mesh *m, float u, float v, float w)
{
    if (m->npt == m->ptcap) {
        uint32_t cap = m->ptcap ? m->ptcap * 2 : 64;
        float(*pt)[3] = cap < (1u << 20) ? kmalloc(sizeof(float[3]) * cap) : NULL;
        if (!pt) {
            m->full = true;
            return 0;
        }
        if (m->pt)
            memcpy(pt, m->pt, sizeof(float[3]) * m->npt);
        kfree(m->pt);
        m->pt = pt, m->ptcap = cap;
    }
    m->pt[m->npt][0] = u, m->pt[m->npt][1] = v, m->pt[m->npt][2] = w;
    return m->npt++;
}

static void prim(struct tess_mesh *m, const uint32_t *k)
{
    if (m->full)
        return;
    if (m->nind + 3 > m->indcap) {
        uint32_t cap = m->indcap ? m->indcap * 2 : 192;
        uint32_t *ind = cap < (1u << 22) ? kmalloc(sizeof(uint32_t) * cap) : NULL;
        if (!ind) {
            m->full = true;
            return;
        }
        if (m->ind)
            memcpy(ind, m->ind, sizeof(uint32_t) * m->nind);
        kfree(m->ind);
        m->ind = ind, m->indcap = cap;
    }
    for (int i = 0; i < m->per; i++)
        m->ind[m->nind++] = k[i];
}

/* where a domain point lies on the plane, to give every triangle the same turn: quads (u, v), triangles (v, w) */
static void plane(const struct tess_mesh *m, uint32_t i, bool tri, float *x, float *y)
{
    *x = m->pt[i][tri ? 1 : 0], *y = m->pt[i][tri ? 2 : 1];
}

static void triangle(struct tess_mesh *m, uint32_t a, uint32_t b, uint32_t c, bool tri, bool ccw)
{
    if (m->full || a == b || b == c || a == c)
        return;
    float ax, ay, bx, by, cx, cy;
    plane(m, a, tri, &ax, &ay), plane(m, b, tri, &bx, &by), plane(m, c, tri, &cx, &cy);
    float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (area == 0)
        return;
    bool flip = (area < 0) != ccw; /* cw: clockwise with v (triangles: w) pointing down */
    uint32_t k[3] = {a, flip ? c : b, flip ? b : c};
    prim(m, k);
}

/* a factor as a count of segments; 0: the patch is culled */
static int segments(float f, uint32_t partitioning, float max)
{
    if (!(f > 0))
        return 0;
    float hi = max >= 1 && max < MAX_SEGS ? max : MAX_SEGS;
    if (f > hi)
        f = hi;
    if (f < 1)
        f = 1;
    int n = (int)f;
    if ((float)n < f)
        n++;
    if (partitioning == 2) { /* pow2 */
        int p = 1;
        while (p < n)
            p <<= 1;
        n = p;
    } else if (partitioning == 3 && !(n & 1)) { /* fractional odd */
        n++;
    } else if (partitioning == 4 && (n & 1)) { /* fractional even */
        n++;
    }
    return n > MAX_SEGS ? MAX_SEGS : n;
}

/* triangles between two runs of points going the same way: a (the outer, na + 1 points) and b (nb + 1 points) */
static void stitch(struct tess_mesh *m, const uint32_t *a, int na, const uint32_t *b, int nb, bool tri, bool ccw)
{
    int i = 0, j = 0;
    while (i < na || j < nb) {
        /* advance on the run whose next point comes first along the side */
        bool on_a = j == nb || (i < na && (long)(i + 1) * nb <= (long)(j + 1) * na);
        if (on_a) {
            triangle(m, a[i], a[i + 1], b[j], tri, ccw);
            i++;
        } else {
            triangle(m, a[i], b[j + 1], b[j], tri, ccw);
            j++;
        }
    }
}

static void quad_patch(struct tess_mesh *m, const int *e, int nu, int nv, bool ccw)
{
    uint32_t ring[4 * MAX_SEGS + 1], inner[4 * MAX_SEGS + 1];
    uint32_t(*grid)[MAX_SEGS + 1] = kmalloc(sizeof(uint32_t[MAX_SEGS + 1][MAX_SEGS + 1]));
    if (!grid) {
        m->full = true;
        return;
    }
    if (nu == 1 && nv == 1 && e[0] == 1 && e[1] == 1 && e[2] == 1 && e[3] == 1) {
        uint32_t a = point(m, 0, 0, 0), b = point(m, 1, 0, 0), c = point(m, 1, 1, 0), d = point(m, 0, 1, 0);
        triangle(m, a, b, c, false, ccw), triangle(m, a, c, d, false, ccw);
        kfree(grid);
        return;
    }
    nu = nu < 2 ? 2 : nu, nv = nv < 2 ? 2 : nv;
    /* the outer ring, clockwise from (0, 0): top (edge 1), right (2), bottom (3), left (0) */
    static const int order[4] = {1, 2, 3, 0};
    static const float from[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, step[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    int at[5], n = 0;
    for (int s = 0; s < 4; s++) {
        int k = e[order[s]];
        at[s] = n;
        for (int i = 0; i < k; i++) {
            float t = (float)i / (float)k;
            ring[n++] = point(m, from[s][0] + step[s][0] * t, from[s][1] + step[s][1] * t, 0);
        }
    }
    at[4] = n;
    ring[n] = ring[0];
    /* the inside grid, without its outermost lines */
    for (int i = 1; i < nu; i++)
        for (int j = 1; j < nv; j++)
            grid[i][j] = point(m, (float)i / (float)nu, (float)j / (float)nv, 0);
    for (int i = 1; i + 1 < nu; i++)
        for (int j = 1; j + 1 < nv; j++) {
            triangle(m, grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], false, ccw);
            triangle(m, grid[i][j], grid[i + 1][j + 1], grid[i][j + 1], false, ccw);
        }
    /* the inside grid's border, the same way round: top, right, bottom, left */
    int in_at[5], k = 0;
    in_at[0] = k;
    for (int i = 1; i < nu - 1; i++)
        inner[k++] = grid[i][1];
    in_at[1] = k;
    for (int j = 1; j < nv - 1; j++)
        inner[k++] = grid[nu - 1][j];
    in_at[2] = k;
    for (int i = nu - 1; i > 1; i--)
        inner[k++] = grid[i][nv - 1];
    in_at[3] = k;
    for (int j = nv - 1; j > 1; j--)
        inner[k++] = grid[1][j];
    in_at[4] = k;
    inner[k] = grid[1][1];
    for (int s = 0; s < 4; s++)
        stitch(m, ring + at[s], at[s + 1] - at[s], inner + in_at[s], in_at[s + 1] - in_at[s], false, ccw);
    kfree(grid);
}

/* the corners of ring r of a triangle split into n: one coordinate 1 - 2r/n, the others r/n */
static void tri_ring(struct tess_mesh *m, int n, int r, int segs, uint32_t *out, int *at)
{
    float lo = (float)r / (float)n, hi = 1 - 2 * lo;
    const float c[3][3] = {{hi, lo, lo}, {lo, hi, lo}, {lo, lo, hi}};
    int k = 0;
    for (int s = 0; s < 3; s++) {
        at[s] = k;
        const float *a = c[s], *b = c[(s + 1) % 3];
        for (int i = 0; i < segs; i++) {
            float t = (float)i / (float)segs;
            out[k++] = point(m, a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
        }
    }
    at[3] = k;
    out[k] = out[0];
}

static void tri_patch(struct tess_mesh *m, const int *e, int n, bool ccw)
{
    uint32_t outer[3 * MAX_SEGS + 1], inner[3 * MAX_SEGS + 1];
    /* the outer ring from corner u to v (edge 2: w = 0), v to w (edge 0: u = 0), w to u (edge 1: v = 0) */
    static const int order[3] = {2, 0, 1};
    static const float c[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (n == 1 && e[0] == 1 && e[1] == 1 && e[2] == 1) {
        uint32_t a = point(m, 1, 0, 0), b = point(m, 0, 1, 0), d = point(m, 0, 0, 1);
        triangle(m, a, b, d, true, ccw);
        return;
    }
    int at[4], k = 0;
    for (int s = 0; s < 3; s++) {
        int segs = e[order[s]];
        at[s] = k;
        const float *a = c[s], *b = c[(s + 1) % 3];
        for (int i = 0; i < segs; i++) {
            float t = (float)i / (float)segs;
            outer[k++] = point(m, a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
        }
    }
    at[3] = k;
    outer[k] = outer[0];
    /* inward ring by ring: ring r has n - 3r segments a side; one with none is the centre */
    for (int r = 1;; r++) {
        int segs = n - 3 * r, in_at[4];
        if (segs <= 0) { /* the centre, or nothing further in: a fan to the centre */
            uint32_t centre = point(m, 1.0f / 3, 1.0f / 3, 1.0f / 3);
            for (int s = 0; s < 3; s++)
                stitch(m, outer + at[s], at[s + 1] - at[s], &centre, 0, true, ccw);
            return;
        }
        tri_ring(m, n, r, segs, inner, in_at);
        for (int s = 0; s < 3; s++)
            stitch(m, outer + at[s], at[s + 1] - at[s], inner + in_at[s], in_at[s + 1] - in_at[s], true, ccw);
        if (segs == 1) { /* the innermost ring is a triangle */
            triangle(m, inner[0], inner[1], inner[2], true, ccw);
            return;
        }
        memcpy(outer, inner, sizeof(uint32_t) * (size_t)(in_at[3] + 1));
        memcpy(at, in_at, sizeof(at));
    }
}

bool tess_patch(struct tess_mesh *m, uint32_t domain, uint32_t partitioning, uint32_t out_prim, const float *edge,
                const float *inside, float max)
{
    m->npt = m->nind = 0;
    m->full = false;
    bool ccw = out_prim == 4;
    m->per = out_prim == 1 ? 1 : out_prim == 2 ? 2 : 3;
    int e[4];
    if (domain == 1) { /* isolines: edge[0] how many lines, edge[1] how many segments each */
        int lines = segments(edge[0], 1, max), segs = segments(edge[1], partitioning, max);
        if (!lines || !segs)
            return false;
        if (m->per == 3)
            m->per = 2;
        for (int j = 0; j < lines; j++) {
            uint32_t first = m->npt;
            for (int i = 0; i <= segs; i++)
                point(m, (float)i / (float)segs, (float)j / (float)lines, 0);
            for (int i = 0; i < segs && m->per == 2; i++) {
                uint32_t k[2] = {first + (uint32_t)i, first + (uint32_t)i + 1};
                prim(m, k);
            }
        }
    } else if (domain == 2) {
        for (int i = 0; i < 3; i++)
            if (!(e[i] = segments(edge[i], partitioning, max)))
                return false;
        int n = segments(inside[0], partitioning, max);
        tri_patch(m, e, n ? n : 1, ccw);
    } else if (domain == 3) {
        for (int i = 0; i < 4; i++)
            if (!(e[i] = segments(edge[i], partitioning, max)))
                return false;
        int nu = segments(inside[0], partitioning, max), nv = segments(inside[1], partitioning, max);
        quad_patch(m, e, nu ? nu : 1, nv ? nv : 1, ccw);
    } else {
        return false;
    }
    if (m->per == 1) { /* points: every domain point once */
        m->nind = 0;
        for (uint32_t i = 0; i < m->npt; i++)
            prim(m, &i);
    }
    return true;
}

void tess_free(struct tess_mesh *m)
{
    kfree(m->pt);
    kfree(m->ind);
    memset(m, 0, sizeof(*m));
}

#endif
