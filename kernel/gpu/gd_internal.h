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
    /* the copy on the host's graphics card (gd_host.c): its resource id (0 none), and which side is newer */
    uint32_t host;
    uint32_t host_gen; /* the host context it was last used in (a lost context leaves the resource to attach again) */
    uint8_t where; /* HOST_SAME, HOST_CPU_NEWER, HOST_GPU_NEWER */
};

enum { HOST_SAME, HOST_CPU_NEWER, HOST_GPU_NEWER };

/* what a view looks at, as the shader sees it */
enum { VD_BUFFER = 1, VD_1D, VD_1D_ARRAY, VD_2D, VD_2D_ARRAY, VD_3D, VD_CUBE, VD_CUBE_ARRAY };

struct gd_view {
    int refs;
    struct gd_resource *r;
    struct gd_view_desc d; /* with the defaults filled in */
    uint32_t format;
    int vdim;
    uint32_t counter; /* append and counter buffers */
    struct gd_resource *counter_res; /* the counter on the host: a word, newer than counter while counter_on_host */
    bool counter_on_host;
    uint32_t host;    /* the host's sampler view or surface for it, or 0 */
    uint32_t host_gen; /* the host context host belongs to */
};

#define SO_MAX_ENTRIES 64

/* a stream output declaration, resolved: component comp .. comp + ncomp - 1 of output register reg (-1: a gap) */
struct so_decl {
    uint32_t n, stride[GD_SLOTS_SO], rasterized;
    struct {
        int16_t reg;
        uint8_t comp, ncomp, slot;
    } e[SO_MAX_ENTRIES];
};

struct gd_shader {
    int refs;
    struct dxbc_shader sh;
    struct so_decl *so;
    /* on the host's graphics card: the shader object (0 not made), -1 when it has no translation or the host could
       not compile it; host_gen: the host context host and so_host belong to */
    int32_t host;
    uint32_t host_gen;
    uint8_t tex_sampler[DXBC_MAX_RES];
    uint32_t used_cb, used_uav, used_srv_buf;
    bool icb;                       /* the host shader reads its immediate constant buffer from icb_buf */
    uint8_t clip_planes;            /* the clip distances the host shader writes */
    uint8_t used_counter;           /* u# whose hidden counter the host shader reaches */
    struct gd_resource *icb_buf;
    uint16_t uav_format[DXBC_MAX_UAV]; /* compute: the typed UAV formats the host shader was made for */
    uint32_t ms; /* the t# read as multisampled textures in the host shader */
    uint32_t serial; /* a number no other shader had */
    /* a vertex shader in the geometry slot for stream output: the host's vertex shader with it, made from the
       vertex shader numbered so_vs (0 not made, -1 cannot be); a domain shader: the host's, made for the hull
       shader numbered so_vs */
    int32_t so_host;
    uint32_t so_vs;
};

struct stage {
    struct gd_shader *shader;
    struct gd_resource *cb[GD_SLOTS_CB];
    uint32_t cb_first[GD_SLOTS_CB], cb_count[GD_SLOTS_CB];
    struct gd_view *srv[GD_SLOTS_SRV];
    struct gd_sampler sampler[GD_SLOTS_SAMPLER];
    bool sampler_set[GD_SLOTS_SAMPLER];
    struct gd_view *uav[GD_SLOTS_UAV];
};

struct gd_context {
    struct stage stage[GD_STAGES];
    struct gd_resource *vb[GD_SLOTS_VB];
    uint32_t vb_stride[GD_SLOTS_VB], vb_offset[GD_SLOTS_VB];
    struct gd_resource *ib;
    uint32_t ib_format, ib_offset;
    uint32_t topology;
    struct gd_input_layout layout;
    bool has_layout;
    struct gd_viewport viewport;
    uint32_t nviewports;
    struct gd_rect scissor;
    uint32_t nscissors;
    struct gd_raster raster;
    struct gd_blend blend;
    float blend_factor[4];
    uint32_t sample_mask;
    struct gd_depth depth;
    uint32_t stencil_ref;
    struct gd_view *rtv[8];
    struct gd_view *dsv;
    struct gd_resource *so[GD_SLOTS_SO];
};

/* ---- the host's graphics card (virtio-gpu with virgl, gd_host.c): draws it can do go there, and resources move
   between the two memories when one side needs what the other wrote */
#if defined(RS_HOST)
static inline bool gd_host_draw(struct gd_context *c, bool indexed, uint32_t count, uint32_t start, int32_t base,
                                uint32_t instances, uint32_t first_instance)
{
    (void)c, (void)indexed, (void)count, (void)start, (void)base, (void)instances, (void)first_instance;
    return false;
}
static inline void gd_host_cpu(struct gd_resource *r, bool write)
{
    (void)r, (void)write;
}
static inline bool gd_host_clear_rtv(struct gd_view *v, const float color[4])
{
    (void)v, (void)color;
    return false;
}
static inline bool gd_host_copy(struct gd_resource *dst, int dsub, uint32_t x, uint32_t y, uint32_t z,
                                struct gd_resource *src, int ssub, const struct gd_box *box)
{
    (void)dst, (void)dsub, (void)x, (void)y, (void)z, (void)src, (void)ssub, (void)box;
    return false;
}
static inline bool gd_host_can_update(const struct gd_resource *r)
{
    (void)r;
    return false;
}
static inline bool gd_host_update(struct gd_resource *r, uint32_t sub, const struct gd_box *box)
{
    (void)r, (void)sub, (void)box;
    return false;
}
static inline bool gd_host_clear_dsv(struct gd_view *v, uint32_t flags, float depth, uint32_t stencil)
{
    (void)v, (void)flags, (void)depth, (void)stencil;
    return false;
}
static inline void gd_host_forget_resource(struct gd_resource *r)
{
    (void)r;
}
static inline void gd_host_forget_view(struct gd_view *v)
{
    (void)v;
}
static inline void gd_host_forget_shader(struct gd_shader *s)
{
    (void)s;
}
static inline void gd_host_count(uint64_t *pixels)
{
    (void)pixels;
}
static inline bool gd_host_dispatch(struct gd_context *c, uint32_t x, uint32_t y, uint32_t z)
{
    (void)c, (void)x, (void)y, (void)z;
    return false;
}
static inline uint64_t gd_host_draws(void)
{
    return 0;
}
static inline void gd_host_test_bad_shader(void)
{
}
#else
bool gd_host_draw(struct gd_context *c, bool indexed, uint32_t count, uint32_t start, int32_t base,
                  uint32_t instances, uint32_t first_instance);
/* before the CPU reads (write false) or changes a resource */
void gd_host_cpu(struct gd_resource *r, bool write);
bool gd_host_clear_rtv(struct gd_view *v, const float color[4]);
bool gd_host_clear_dsv(struct gd_view *v, uint32_t flags, float depth, uint32_t stencil);
/* a copy done on the host when either resource is there (dsub < 0: every subresource); false: the CPU copies */
bool gd_host_copy(struct gd_resource *dst, int dsub, uint32_t x, uint32_t y, uint32_t z, struct gd_resource *src,
                  int ssub, const struct gd_box *box);
/* gd_host_can_update: the host has written the resource, so gd_update writes its box here without reading it back
   and gd_host_update sends that box alone */
bool gd_host_can_update(const struct gd_resource *r);
bool gd_host_update(struct gd_resource *r, uint32_t sub, const struct gd_box *box);
void gd_host_forget_resource(struct gd_resource *r);
void gd_host_forget_view(struct gd_view *v);
void gd_host_forget_shader(struct gd_shader *s);
/* adds the samples the host's draws let through since the last call (occlusion queries) */
void gd_host_count(uint64_t *pixels);
bool gd_host_dispatch(struct gd_context *c, uint32_t x, uint32_t y, uint32_t z);
/* for tests: the draws and dispatches that ran on the host so far; the next shader made for the host made unusable
   there, as one the host fails to compile */
uint64_t gd_host_draws(void);
void gd_host_test_bad_shader(void);
#endif

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
void gd_lod(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const float ddx[4],
            const float ddy[4], float out[2]);
void gd_gather(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const int offset[3],
               int component, const float *ref, float out[4]);
void gd_texel_fetch(const struct gd_view *v, const int32_t coord[4], const int offset[3], uint32_t out[4]);
void gd_view_size(const struct gd_view *v, uint32_t mip, uint32_t out[4]);

#endif
