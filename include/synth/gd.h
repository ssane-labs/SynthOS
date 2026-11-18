#ifndef SYNTH_GD_H
#define SYNTH_GD_H

/* The graphics device: what Direct3D 10/11 programs ask of a graphics card, done by the software pipeline
   (raster.h) with the shaders the programs ship (dxbc.h). Resources, views, shaders and pipeline state live in
   kernel memory; the Windows layer's d3d11.dll and dxgi.dll are thin objects over it.

   The 64-bit kernel only. The Windows layer is x32 code, so everything shared here is fixed-size: structures hold
   only 32- and 64-bit numbers and floats, and objects are passed as pointers in arguments, never inside structures. */

#include <synth/types.h>

/* ---- formats: DXGI_FORMAT numbers */

enum {
    GD_UNKNOWN = 0,
    GD_R32G32B32A32_TYPELESS = 1, GD_R32G32B32A32_FLOAT, GD_R32G32B32A32_UINT, GD_R32G32B32A32_SINT,
    GD_R32G32B32_TYPELESS, GD_R32G32B32_FLOAT, GD_R32G32B32_UINT, GD_R32G32B32_SINT,
    GD_R16G16B16A16_TYPELESS, GD_R16G16B16A16_FLOAT, GD_R16G16B16A16_UNORM, GD_R16G16B16A16_UINT,
    GD_R16G16B16A16_SNORM, GD_R16G16B16A16_SINT,
    GD_R32G32_TYPELESS, GD_R32G32_FLOAT, GD_R32G32_UINT, GD_R32G32_SINT,
    GD_R32G8X24_TYPELESS, GD_D32_FLOAT_S8X24_UINT, GD_R32_FLOAT_X8X24_TYPELESS, GD_X32_TYPELESS_G8X24_UINT,
    GD_R10G10B10A2_TYPELESS, GD_R10G10B10A2_UNORM, GD_R10G10B10A2_UINT, GD_R11G11B10_FLOAT,
    GD_R8G8B8A8_TYPELESS, GD_R8G8B8A8_UNORM, GD_R8G8B8A8_UNORM_SRGB, GD_R8G8B8A8_UINT, GD_R8G8B8A8_SNORM,
    GD_R8G8B8A8_SINT,
    GD_R16G16_TYPELESS, GD_R16G16_FLOAT, GD_R16G16_UNORM, GD_R16G16_UINT, GD_R16G16_SNORM, GD_R16G16_SINT,
    GD_R32_TYPELESS, GD_D32_FLOAT, GD_R32_FLOAT, GD_R32_UINT, GD_R32_SINT,
    GD_R24G8_TYPELESS, GD_D24_UNORM_S8_UINT, GD_R24_UNORM_X8_TYPELESS, GD_X24_TYPELESS_G8_UINT,
    GD_R8G8_TYPELESS, GD_R8G8_UNORM, GD_R8G8_UINT, GD_R8G8_SNORM, GD_R8G8_SINT,
    GD_R16_TYPELESS, GD_R16_FLOAT, GD_D16_UNORM, GD_R16_UNORM, GD_R16_UINT, GD_R16_SNORM, GD_R16_SINT,
    GD_R8_TYPELESS, GD_R8_UNORM, GD_R8_UINT, GD_R8_SNORM, GD_R8_SINT, GD_A8_UNORM, GD_R1_UNORM,
    GD_R9G9B9E5_SHAREDEXP, GD_R8G8_B8G8_UNORM, GD_G8R8_G8B8_UNORM,
    GD_BC1_TYPELESS, GD_BC1_UNORM, GD_BC1_UNORM_SRGB, GD_BC2_TYPELESS, GD_BC2_UNORM, GD_BC2_UNORM_SRGB,
    GD_BC3_TYPELESS, GD_BC3_UNORM, GD_BC3_UNORM_SRGB, GD_BC4_TYPELESS, GD_BC4_UNORM, GD_BC4_SNORM,
    GD_BC5_TYPELESS, GD_BC5_UNORM, GD_BC5_SNORM,
    GD_B5G6R5_UNORM, GD_B5G5R5A1_UNORM, GD_B8G8R8A8_UNORM, GD_B8G8R8X8_UNORM, GD_R10G10B10_XR_BIAS_A2_UNORM,
    GD_B8G8R8A8_TYPELESS, GD_B8G8R8A8_UNORM_SRGB, GD_B8G8R8X8_TYPELESS, GD_B8G8R8X8_UNORM_SRGB,
    GD_BC6H_TYPELESS, GD_BC6H_UF16, GD_BC6H_SF16, GD_BC7_TYPELESS, GD_BC7_UNORM, GD_BC7_UNORM_SRGB,
    GD_B4G4R4A4_UNORM = 115,
    GD_FORMAT_COUNT = 116
};

/* what a format can be used for (D3D11_FORMAT_SUPPORT bits) */
#define GD_SUPPORT_BUFFER 0x1u
#define GD_SUPPORT_IA_VERTEX 0x2u
#define GD_SUPPORT_IA_INDEX 0x4u
#define GD_SUPPORT_TEX1D 0x10u
#define GD_SUPPORT_TEX2D 0x20u
#define GD_SUPPORT_TEX3D 0x40u
#define GD_SUPPORT_CUBE 0x80u
#define GD_SUPPORT_LOAD 0x100u
#define GD_SUPPORT_SAMPLE 0x200u
#define GD_SUPPORT_SAMPLE_COMPARISON 0x400u
#define GD_SUPPORT_MIP 0x1000u
#define GD_SUPPORT_MIP_AUTOGEN 0x2000u
#define GD_SUPPORT_RENDER_TARGET 0x4000u
#define GD_SUPPORT_BLENDABLE 0x8000u
#define GD_SUPPORT_DEPTH_STENCIL 0x10000u
#define GD_SUPPORT_CPU_LOCKABLE 0x20000u
#define GD_SUPPORT_MULTISAMPLE_RESOLVE 0x40000u
#define GD_SUPPORT_DISPLAY 0x80000u
#define GD_SUPPORT_CAST_WITHIN_BIT_LAYOUT 0x100000u
#define GD_SUPPORT_MULTISAMPLE_RENDERTARGET 0x200000u
#define GD_SUPPORT_MULTISAMPLE_LOAD 0x400000u
#define GD_SUPPORT_SHADER_GATHER 0x800000u
#define GD_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW 0x4000000u
#define GD_SUPPORT_SHADER_GATHER_COMPARISON 0x8000000u

struct gd_format {
    uint32_t block_bytes; /* bytes per texel, or per 4x4 block of a compressed format; 0: not known */
    uint32_t block;       /* 1, or 4 for block-compressed formats */
    uint32_t support;     /* GD_SUPPORT_* */
};

bool gd_format(uint32_t format, struct gd_format *out);

/* ---- resources */

enum { GD_BUFFER = 1, GD_TEX1D, GD_TEX2D, GD_TEX3D };

/* D3D11_BIND_* and D3D11_RESOURCE_MISC_* bits the device looks at */
#define GD_BIND_VERTEX 0x1u
#define GD_BIND_INDEX 0x2u
#define GD_BIND_CONSTANT 0x4u
#define GD_BIND_SHADER_RESOURCE 0x8u
#define GD_BIND_RENDER_TARGET 0x20u
#define GD_BIND_DEPTH_STENCIL 0x40u
#define GD_MISC_GENERATE_MIPS 0x1u
#define GD_MISC_TEXTURECUBE 0x4u
#define GD_BIND_UNORDERED_ACCESS 0x80u
#define GD_MISC_BUFFER_RAW 0x20u
#define GD_MISC_BUFFER_STRUCTURED 0x40u

struct gd_res_desc {
    uint32_t dim;    /* GD_BUFFER... */
    uint32_t width;  /* buffers: the size in bytes */
    uint32_t height, depth, mips, array, format, samples;
    uint32_t usage, bind, cpu, misc, stride;
};

/* the initial contents of one subresource, in the caller's memory */
struct gd_init {
    uint64_t data;
    uint32_t row_pitch, slice_pitch;
};

struct gd_resource;

/* init: one entry per subresource, or NULL (zeros); 0 and *error when the description is not allowed */
struct gd_resource *gd_resource_create(const struct gd_res_desc *desc, const struct gd_init *init, uint32_t *error);
void gd_resource_ref(struct gd_resource *r);
void gd_resource_unref(struct gd_resource *r);
void gd_resource_desc(const struct gd_resource *r, struct gd_res_desc *out);
uint32_t gd_subresources(const struct gd_resource *r);

/* all of a resource's subresources: whole pages of kernel memory, so a program's Map can see them in place */
uint8_t *gd_resource_memory(struct gd_resource *r, uint32_t *bytes);
struct gd_layout {
    uint32_t row_pitch, slice_pitch, size; /* rows of texels (blocks), slices of rows */
    uint32_t width, height, depth;         /* of this mip level, in texels */
};

/* where a subresource is: its bytes in kernel memory, or NULL for a subresource that does not exist */
uint8_t *gd_subresource(struct gd_resource *r, uint32_t sub, struct gd_layout *layout);

struct gd_box {
    uint32_t left, top, front, right, bottom, back;
};

/* copies from the caller's memory into a subresource; box NULL: all of it */
void gd_update(struct gd_resource *r, uint32_t sub, const struct gd_box *box, const void *src, uint32_t row_pitch,
               uint32_t slice_pitch);
size_t gd_update_size(struct gd_resource *r, uint32_t sub, const struct gd_box *box, uint32_t row_pitch,
                      uint32_t slice_pitch);
void gd_copy_resource(struct gd_resource *dst, struct gd_resource *src);
void gd_copy_region(struct gd_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                    struct gd_resource *src, uint32_t src_sub, const struct gd_box *box);

/* ---- views */

enum { GD_VIEW_SRV = 1, GD_VIEW_RTV, GD_VIEW_DSV, GD_VIEW_UAV };

struct gd_view_desc {
    uint32_t kind;       /* GD_VIEW_* */
    uint32_t format;     /* 0: the resource's */
    uint32_t first_mip, mips;     /* mips 0: all from first_mip */
    uint32_t first_slice, slices; /* array slices or 3D depth slices; slices 0: all */
    uint32_t first_element, elements; /* buffer views */
    uint32_t dim;        /* D3D11_SRV_DIMENSION numbering (2 1D, 3 1D array, 4 2D, 5 2D array, 8 3D, 9 cube...);
                            0: from the resource */
    uint32_t flags;      /* depth views: 1 read-only depth, 2 read-only stencil; buffer views: 1 raw (and for
                            unordered access, 2 append, 4 counter) */
};

struct gd_view;

struct gd_view *gd_view_create(struct gd_resource *r, const struct gd_view_desc *desc, uint32_t *error);
void gd_view_ref(struct gd_view *v);
void gd_view_unref(struct gd_view *v);
struct gd_resource *gd_view_resource(struct gd_view *v);
void gd_view_desc(const struct gd_view *v, struct gd_view_desc *out);

void gd_clear_rtv(struct gd_view *v, const float color[4]);
/* flags: 1 depth, 2 stencil */
void gd_clear_dsv(struct gd_view *v, uint32_t flags, float depth, uint32_t stencil);
void gd_generate_mips(struct gd_view *v);
/* ClearUnorderedAccessViewUint (floats false) and ...Float: every element of the view */
void gd_clear_uav(struct gd_view *v, const uint32_t value[4], bool floats);
/* the hidden counter of an append or counter buffer view (CopyStructureCount) */
uint32_t gd_uav_counter(struct gd_view *v);

/* ---- shaders */

struct gd_shader;

/* stage: 0 pixel, 1 vertex (the DXBC program types); others are taken but draw nothing yet */
struct gd_shader *gd_shader_create(const void *blob, uint32_t size, char error[64]);
void gd_shader_ref(struct gd_shader *s);
void gd_shader_unref(struct gd_shader *s);
int gd_shader_stage(const struct gd_shader *s);

/* ---- input layouts */

#define GD_MAX_ELEMENTS 32

struct gd_element {
    char name[32];
    uint32_t index, format, slot, offset; /* offset 0xffffffff: right after the previous element */
    uint32_t per_instance, step;
};

struct gd_input_layout {
    uint32_t count;
    struct gd_element e[GD_MAX_ELEMENTS];
};

/* ---- pipeline state, as the D3D11 descriptions (numbers as in D3D11) */

struct gd_blend_target {
    uint32_t enable, src, dst, op, src_alpha, dst_alpha, op_alpha, write_mask;
};

struct gd_blend {
    uint32_t alpha_to_coverage, independent;
    struct gd_blend_target rt[8];
};

struct gd_raster {
    uint32_t fill, cull, front_ccw;
    int32_t depth_bias;
    float depth_bias_clamp, slope_scaled_depth_bias;
    uint32_t depth_clip, scissor, multisample, antialiased_line;
};

struct gd_stencil_op {
    uint32_t fail, depth_fail, pass, func;
};

struct gd_depth {
    uint32_t depth_enable, write_mask, func;
    uint32_t stencil_enable, read_mask, write_mask_stencil;
    struct gd_stencil_op front, back;
};

struct gd_sampler {
    uint32_t filter, address_u, address_v, address_w;
    float mip_lod_bias;
    uint32_t max_anisotropy, compare;
    float border[4], min_lod, max_lod;
};

struct gd_viewport {
    float x, y, w, h, min_z, max_z;
};

struct gd_rect {
    int32_t left, top, right, bottom;
};

/* ---- the context: the state draws use, set piece by piece */

#define GD_SLOTS_VB 32
#define GD_SLOTS_CB 14
#define GD_SLOTS_SRV 128
#define GD_SLOTS_SAMPLER 16
#define GD_SLOTS_UAV 8
#define GD_STAGES 6 /* the DXBC program types: pixel, vertex, geometry, hull, domain, compute */

struct gd_context;

struct gd_context *gd_context_create(void);
void gd_context_destroy(struct gd_context *c);
/* forgets every binding (ClearState) */
void gd_context_clear(struct gd_context *c);

void gd_set_vertex_buffer(struct gd_context *c, uint32_t slot, struct gd_resource *r, uint32_t stride, uint32_t offset);
void gd_set_index_buffer(struct gd_context *c, struct gd_resource *r, uint32_t format, uint32_t offset);
void gd_set_topology(struct gd_context *c, uint32_t topology);
void gd_set_input_layout(struct gd_context *c, const struct gd_input_layout *layout);
void gd_set_shader(struct gd_context *c, uint32_t stage, struct gd_shader *s);
/* first and count in 16-byte constants; count 0: the whole buffer */
void gd_set_constant_buffer(struct gd_context *c, uint32_t stage, uint32_t slot, struct gd_resource *r,
                            uint32_t first, uint32_t count);
void gd_set_shader_resource(struct gd_context *c, uint32_t stage, uint32_t slot, struct gd_view *v);
void gd_set_sampler(struct gd_context *c, uint32_t stage, uint32_t slot, const struct gd_sampler *s);
void gd_set_viewports(struct gd_context *c, uint32_t count, const struct gd_viewport *v);
void gd_set_scissors(struct gd_context *c, uint32_t count, const struct gd_rect *r);
void gd_set_raster(struct gd_context *c, const struct gd_raster *r);
void gd_set_blend(struct gd_context *c, const struct gd_blend *b, const float factor[4], uint32_t sample_mask);
void gd_set_depth(struct gd_context *c, const struct gd_depth *d, uint32_t stencil_ref);
void gd_set_render_target(struct gd_context *c, uint32_t slot, struct gd_view *rtv);
void gd_set_depth_target(struct gd_context *c, struct gd_view *dsv);
/* the unordered access views of the compute stage (5) or of the pixel stage (0); count 0xffffffff keeps the hidden
   counter, anything else sets it */
void gd_set_uav(struct gd_context *c, uint32_t stage, uint32_t slot, struct gd_view *v, uint32_t count);

/* Dispatch: x * y * z groups of the compute shader's threads */
void gd_dispatch(struct gd_context *c, uint32_t x, uint32_t y, uint32_t z);

/* Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced */
void gd_draw(struct gd_context *c, bool indexed, uint32_t count, uint32_t start, int32_t base_vertex,
             uint32_t instances, uint32_t start_instance);

/* a 2D texture's subresource scaled into the rectangle dx, dy, dw, dh of 0xAARRGGBB pixels (stride: pixels per row),
   drawing only inside cx, cy, cw, ch: Present */
void gd_present(struct gd_resource *r, uint32_t sub, uint32_t *dst, int stride, int dx, int dy, int dw, int dh,
                int cx, int cy, int cw, int ch);
struct gd_stats {
    uint64_t draws, skipped, triangles, pixels, shader_faults, dispatches;
    char last_fault[64];
};

void gd_get_stats(struct gd_stats *out);

#endif
