#ifndef SYNTH_TESS_H
#define SYNTH_TESS_H

/* The fixed-function tessellator of Direct3D 11: a patch's domain points and the primitives between them, from the
   hull shader's tessellation factors. Outer edges are split exactly by their own factors, so patches that share an
   edge meet without cracks; the inside is concentric rings stitched to each other. Fractional partitioning is
   rounded up to the next odd or even count (no fractional segments). */

#include <synth/types.h>

struct tess_mesh {
    float (*pt)[3];     /* domain coordinates: (u, v, w) for triangles, (u, v, 0) for quads and isolines */
    uint32_t npt, ptcap;
    uint32_t *ind;      /* per indices make one primitive */
    uint32_t nind, indcap;
    int per;            /* 1 points, 2 lines, 3 triangles */
    bool full;          /* out of memory: what is there is all there is */
};

/* domain: 1 isoline, 2 triangle, 3 quad; partitioning: 1 integer, 2 pow2, 3 fractional odd, 4 fractional even;
   out_prim: 1 point, 2 line, 3 triangle cw, 4 triangle ccw. edge: 4 (quad), 3 (triangle) or 2 (isoline: detail,
   density) factors; inside: 2 (quad) or 1 (triangle). false: the patch is culled (a factor is 0, negative or NaN).
   The mesh is reset first; tess_free releases it. */
bool tess_patch(struct tess_mesh *m, uint32_t domain, uint32_t partitioning, uint32_t out_prim, const float *edge,
                const float *inside, float max);
void tess_free(struct tess_mesh *m);

#endif
