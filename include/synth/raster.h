#ifndef SYNTH_RASTER_H
#define SYNTH_RASTER_H

/* The software 3D pipeline: what a graphics card does, on the processor's cores. Vertices go through a vertex
   shader, triangles are clipped, culled and cut into 64x64 tiles that are drawn in parallel, 2x2 pixels at a time,
   with a pixel shader, a depth test and blending. Shaders are C functions for now; the Direct3D layer gives them
   compiled shader code later. */

#include <synth/types.h>

#define RS_MAX_VARY 16 /* float4 outputs of a vertex shader besides the position */

enum rs_topology { RS_TRIANGLES, RS_STRIP };
enum rs_cull { RS_CULL_NONE, RS_CULL_BACK, RS_CULL_FRONT };
/* the Direct3D comparison functions */
enum rs_compare { RS_NEVER = 1, RS_LESS, RS_EQUAL, RS_LESS_EQUAL, RS_GREATER, RS_NOT_EQUAL, RS_GREATER_EQUAL, RS_ALWAYS };
enum rs_blend { RS_BLEND_NONE, RS_BLEND_ALPHA, RS_BLEND_ADD, RS_BLEND_PREMULTIPLIED };

struct rs_target {
    uint32_t *color; /* 0xAARRGGBB, or NULL */
    int stride;      /* in pixels */
    float *depth;    /* 0..1, or NULL */
    int depth_stride;
    int w, h;
};

/* four pixels: (x, y), (x+1, y), (x, y+1), (x+1, y+1); a pixel outside the triangle still has its inputs so that
   the shader can take differences across the quad (ddx, ddy, the mip level) */
struct rs_quad {
    int x, y;
    float depth[4];
    float in[4][RS_MAX_VARY * 4];
};

/* out: the clip-space position (x, y, z, w), then nvary float4s */
typedef void (*rs_vertex_fn)(const void *ctx, uint32_t index, float *out);
/* writes four colors (r, g, b, a in 0..1); clears bits of *mask to discard pixels */
typedef void (*rs_pixel_fn)(const void *ctx, const struct rs_quad *q, float color[4][4], unsigned *mask);

struct rs_state {
    rs_vertex_fn vs;
    rs_pixel_fn ps;
    const void *ctx;
    int nvary;
    enum rs_cull cull;
    bool front_ccw;
    bool depth_test, depth_write;
    enum rs_compare depth_func;
    enum rs_blend blend;
    unsigned write_mask; /* 1 red, 2 green, 4 blue, 8 alpha; 0 means all */
    bool scissor;
    int sx, sy, sw, sh;
    float vx, vy, vw, vh, min_z, max_z; /* the viewport; vw 0: the whole target */
};

struct rs_stats {
    uint64_t triangles, culled, clipped, quads, pixels;
};

/* Draws count vertices (or count indices, when indices is not NULL) as triangles. */
int rs_draw(struct rs_target *t, const struct rs_state *s, enum rs_topology topology, uint32_t count,
            const uint32_t *indices, int32_t base_vertex);
void rs_clear(struct rs_target *t, uint32_t color, bool clear_color, float depth, bool clear_depth);
void rs_get_stats(struct rs_stats *out);

#endif
