#ifndef SYNTH_GPU_GD_INTERNAL_H
#define SYNTH_GPU_GD_INTERNAL_H

/* Inside the graphics device (include/synth/gd.h): resources, views, texel formats and the texture unit. */

#include <synth/dxbc.h>
#include <synth/gd.h>

struct gd_resource {
    int refs;
    struct gd_res_desc d;
    uint32_t subs;
    struct gd_layout *lay; /* per subresource (mip + slice * mips) */
    size_t *off;
    uint8_t *data;
    size_t size, alloc; /* the subresources' bytes; the pages they are in */
    uint32_t so_filled;  /* buffers: the bytes stream output wrote */
};

/* what a view looks at, as the shader sees it */
enum { VD_BUFFER = 1, VD_1D, VD_1D_ARRAY, VD_2D, VD_2D_ARRAY, VD_3D, VD_CUBE, VD_CUBE_ARRAY };

struct gd_view {
    int refs;
    struct gd_resource *r;
    struct gd_view_desc d; /* with the defaults filled in */
    uint32_t format;
    int vdim;
    uint32_t counter; /* append and counter buffers */
};

/* format classes */
enum { FC_NONE, FC_FLOAT, FC_UINT, FC_SINT };

int gd_format_class(uint32_t f); /* what a shader reads or writes: floats or integers */
bool gd_format_srgb(uint32_t f);
bool gd_format_depth(uint32_t f);   /* has depth or stencil */
uint32_t gd_format_bytes(uint32_t f, uint32_t *block);
/* the format a typeless one is read as when nothing more is said (R24G8_TYPELESS: the depth) */
uint32_t gd_format_typed(uint32_t f);

/* one texel (bx, by: inside a 4x4 block for compressed formats) as four 32-bit values: float bits for float, unorm
   and snorm formats, numbers for integer ones; missing components are 0, alpha 1 */
void gd_texel_load(uint32_t f, const uint8_t *p, int bx, int by, uint32_t out[4]);
/* writes components in mask (1 r, 2 g, 4 b, 8 a) */
void gd_texel_store(uint32_t f, uint8_t *p, const uint32_t v[4], unsigned mask);

float gd_depth_read(uint32_t f, const uint8_t *p);
void gd_depth_write(uint32_t f, uint8_t *p, float depth);
uint32_t gd_stencil_read(uint32_t f, const uint8_t *p);
void gd_stencil_write(uint32_t f, uint8_t *p, uint32_t s);

float gd_srgb_to_linear(float c);
float gd_linear_to_srgb(float c);

/* block-compressed formats: one texel of a 4x4 block, as floats */
void gd_bc_texel(uint32_t f, const uint8_t *block, int x, int y, float out[4]);

/* the texture unit */
void gd_sample(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const float ddx[4],
               const float ddy[4], float lod, float bias, const int offset[3], const float *ref, float out[4]);
void gd_gather(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const int offset[3],
               int component, const float *ref, float out[4]);
void gd_texel_fetch(const struct gd_view *v, const int32_t coord[4], const int offset[3], uint32_t out[4]);
void gd_view_size(const struct gd_view *v, uint32_t mip, uint32_t out[4]);

#endif
