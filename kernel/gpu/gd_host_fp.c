/* The graphics device on the host's graphics card: virtio-gpu with virgl. A draw whose shaders have a TGSI
   translation (dxbc_tgsi.c) and whose state the host can take goes there as a virgl command stream; others stay on
   the software pipeline. Resources keep living in kernel memory, which is also the host resource's guest memory: a
   transfer moves them between the two sides when the side that did not write them last needs them. */

#if defined(__x86_64__) && !defined(RS_HOST)

#include <synth/heap.h>
#include <synth/log.h>
#include <synth/command.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "../../arch/x86/virtio_gpu.h"
#include "gd_internal.h"

/* virgl's protocol (virgl_protocol.h, virgl_hw.h) */
enum {
    C_CREATE = 1, C_BIND = 2, C_DESTROY = 3, C_VIEWPORT = 4, C_FRAMEBUFFER = 5, C_VERTEX_BUFFERS = 6, C_CLEAR = 7,
    C_DRAW = 8, C_SAMPLER_VIEWS = 10, C_INDEX_BUFFER = 11, C_STENCIL_REF = 13, C_BLEND_COLOR = 14, C_SCISSOR = 15,
    C_SAMPLER_STATES = 18, C_BEGIN_QUERY = 19, C_END_QUERY = 20, C_QUERY_RESULT = 21, C_SAMPLE_MASK = 24,
    C_UNIFORM_BUFFER = 27, C_BIND_SHADER = 31,
    O_BLEND = 1, O_RASTERIZER = 2, O_DSA = 3, O_SHADER = 4, O_VERTEX_ELEMENTS = 5, O_SAMPLER_VIEW = 6,
    O_SAMPLER_STATE = 7, O_SURFACE = 8, O_QUERY = 9,
    B_DEPTH_STENCIL = 1 << 0, B_RENDER_TARGET = 1 << 1, B_SAMPLER_VIEW = 1 << 3, B_VERTEX = 1 << 4,
    B_CONSTANT = 1 << 6, B_CUSTOM = 1 << 17,
    SH_VERTEX = 0, SH_FRAGMENT = 1,
};

#define STREAM_WORDS 1024

static struct {
    int state; /* 0 not tried, 1 ready, -1 none */
    bool off;  /* the "hostgpu" setting */
    int gpu;
    uint32_t ctx, *cmd, n, next;
    /* state objects as last made, and what they were made from */
    uint32_t blend, dsa, rs, ve, samplers[2][16], null_fs;
    uint8_t blend_key[sizeof(struct gd_blend)], dsa_key[sizeof(struct gd_depth) + 4], rs_key[sizeof(struct gd_raster) + 4];
    uint8_t ve_key[sizeof(struct gd_input_layout) + 32 * 4];
    struct gd_sampler samp_key[2][16];
    /* occlusion: one query open across draws, read when someone asks */
    uint32_t query, query_buf, *query_mem, drawn_since;
    uint64_t draws, software;
    char why[64];
    volatile int lock;
} H;

static void lock(void)
{
    while (__atomic_test_and_set(&H.lock, __ATOMIC_ACQUIRE))
        __builtin_ia32_pause();
}

static void unlock(void)
{
    __atomic_clear(&H.lock, __ATOMIC_RELEASE);
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- the command stream: a page, sent when full and before anything that must see what it did */

static bool flush(bool wait)
{
    if (!H.n)
        return true;
    bool ok = virtio_gpu_submit_3d(H.gpu, H.ctx, H.cmd, H.n, wait);
    H.n = 0;
    return ok;
}

static void cmd(uint32_t c, uint32_t obj, uint32_t len)
{
    if (H.n + len + 1 > STREAM_WORDS)
        flush(false);
    H.cmd[H.n++] = c | obj << 8 | len << 16;
}

static void put(uint32_t w)
{
    if (H.n < STREAM_WORDS)
        H.cmd[H.n++] = w;
}

static uint32_t handle(void)
{
    return H.next++;
}

static void destroy(uint32_t type, uint32_t h)
{
    if (!h)
        return;
    cmd(C_DESTROY, type, 1);
    put(h);
}

static bool ready(void)
{
    if (H.off)
        return false;
    if (H.state)
        return H.state > 0;
    H.state = -1;
    H.gpu = virtio_gpu_find_3d();
    if (H.gpu < 0)
        return false;
    H.cmd = kmalloc_pages(STREAM_WORDS * 4);
    H.query_mem = kmalloc_pages(4096);
    if (!H.cmd || !H.query_mem)
        return false;
    H.ctx = virtio_gpu_ctx_create(H.gpu, "synthos-d3d");
    if (!H.ctx)
        return false;
    H.next = 1;
    /* the occlusion query writes its result into a small buffer */
    struct virtio_res3d qb = {.target = 0, .format = 64, .bind = B_CUSTOM, .w = 64};
    H.query_buf = virtio_gpu_resource_3d(H.gpu, H.ctx, &qb, H.query_mem, 4096);
    if (H.query_buf) {
        H.query = handle();
        cmd(C_CREATE, O_QUERY, 4);
        put(H.query), put(0), put(0), put(H.query_buf);
        cmd(C_BEGIN_QUERY, 0, 1);
        put(H.query);
    }
    /* a pixel shader for draws without one (depth only) */
    H.state = 1;
    klog("gd: drawing on the host's graphics card (virtio-gpu %d, virgl)", H.gpu);
    return true;
}

/* ---- formats */

static const uint16_t formats[GD_FORMAT_COUNT] = {
    [GD_R32G32B32A32_TYPELESS] = 31, [GD_R32G32B32A32_FLOAT] = 31, [GD_R32G32B32A32_UINT] = 196,
    [GD_R32G32B32A32_SINT] = 200, [GD_R32G32B32_TYPELESS] = 30, [GD_R32G32B32_FLOAT] = 30,
    [GD_R32G32B32_UINT] = 195, [GD_R32G32B32_SINT] = 199, [GD_R16G16B16A16_TYPELESS] = 94,
    [GD_R16G16B16A16_FLOAT] = 94, [GD_R16G16B16A16_UNORM] = 51, [GD_R16G16B16A16_UINT] = 188,
    [GD_R16G16B16A16_SNORM] = 59, [GD_R16G16B16A16_SINT] = 192, [GD_R32G32_TYPELESS] = 29,
    [GD_R32G32_FLOAT] = 29, [GD_R32G32_UINT] = 194, [GD_R32G32_SINT] = 198, [GD_R32G8X24_TYPELESS] = 126,
    [GD_D32_FLOAT_S8X24_UINT] = 126, [GD_R10G10B10A2_TYPELESS] = 8, [GD_R10G10B10A2_UNORM] = 8,
    [GD_R10G10B10A2_UINT] = 253, [GD_R11G11B10_FLOAT] = 124, [GD_R8G8B8A8_TYPELESS] = 67,
    [GD_R8G8B8A8_UNORM] = 67, [GD_R8G8B8A8_UNORM_SRGB] = 104, [GD_R8G8B8A8_UINT] = 180,
    [GD_R8G8B8A8_SNORM] = 77, [GD_R8G8B8A8_SINT] = 184, [GD_R16G16_TYPELESS] = 92, [GD_R16G16_FLOAT] = 92,
    [GD_R16G16_UNORM] = 49, [GD_R16G16_UINT] = 186, [GD_R16G16_SNORM] = 57, [GD_R16G16_SINT] = 190,
    [GD_R32_TYPELESS] = 28, [GD_D32_FLOAT] = 18, [GD_R32_FLOAT] = 28, [GD_R32_UINT] = 193, [GD_R32_SINT] = 197,
    [GD_R24G8_TYPELESS] = 19, [GD_D24_UNORM_S8_UINT] = 19, [GD_R24_UNORM_X8_TYPELESS] = 21,
    [GD_X24_TYPELESS_G8_UINT] = 136, [GD_R8G8_TYPELESS] = 65, [GD_R8G8_UNORM] = 65, [GD_R8G8_UINT] = 178,
    [GD_R8G8_SNORM] = 75, [GD_R8G8_SINT] = 182, [GD_R16_TYPELESS] = 91, [GD_R16_FLOAT] = 91,
    [GD_D16_UNORM] = 16, [GD_R16_UNORM] = 48, [GD_R16_UINT] = 185, [GD_R16_SNORM] = 56, [GD_R16_SINT] = 189,
    [GD_R8_TYPELESS] = 64, [GD_R8_UNORM] = 64, [GD_R8_UINT] = 177, [GD_R8_SNORM] = 74, [GD_R8_SINT] = 181,
    [GD_A8_UNORM] = 10, [GD_R9G9B9E5_SHAREDEXP] = 125, [GD_BC1_TYPELESS] = 106, [GD_BC1_UNORM] = 106,
    [GD_BC1_UNORM_SRGB] = 110, [GD_BC2_TYPELESS] = 107, [GD_BC2_UNORM] = 107, [GD_BC2_UNORM_SRGB] = 111,
    [GD_BC3_TYPELESS] = 108, [GD_BC3_UNORM] = 108, [GD_BC3_UNORM_SRGB] = 112, [GD_BC4_TYPELESS] = 113,
    [GD_BC4_UNORM] = 113, [GD_BC4_SNORM] = 114, [GD_BC5_TYPELESS] = 115, [GD_BC5_UNORM] = 115,
    [GD_BC5_SNORM] = 116, [GD_B5G6R5_UNORM] = 7, [GD_B5G5R5A1_UNORM] = 5, [GD_B8G8R8A8_UNORM] = 1,
    [GD_B8G8R8X8_UNORM] = 2, [GD_B8G8R8A8_TYPELESS] = 1, [GD_B8G8R8A8_UNORM_SRGB] = 100,
    [GD_B8G8R8X8_TYPELESS] = 2, [GD_B8G8R8X8_UNORM_SRGB] = 101, [GD_BC6H_TYPELESS] = 258, [GD_BC6H_UF16] = 258,
    [GD_BC6H_SF16] = 257, [GD_BC7_TYPELESS] = 255, [GD_BC7_UNORM] = 255, [GD_BC7_UNORM_SRGB] = 256,
    [GD_B4G4R4A4_UNORM] = 6,
};

static uint32_t host_format(uint32_t f)
{
    return f < GD_FORMAT_COUNT ? formats[f] : 0;
}

/* a resource's format on the host: depth formats for what is bound as depth */
static uint32_t resource_format(const struct gd_resource *r)
{
    uint32_t f = r->d.format;
    if (r->d.dim == GD_BUFFER)
        return 64; /* R8_UNORM: bytes */
    if (r->d.bind & GD_BIND_DEPTH_STENCIL) {
        if (f == GD_R32_TYPELESS || f == GD_D32_FLOAT)
            return 18;
        if (f == GD_R16_TYPELESS || f == GD_D16_UNORM)
            return 16;
        if (f == GD_R24G8_TYPELESS || f == GD_D24_UNORM_S8_UINT)
            return 19;
        if (f == GD_R32G8X24_TYPELESS || f == GD_D32_FLOAT_S8X24_UINT)
            return 126;
        return 0;
    }
    return host_format(f);
}

/* a view's format: views of depth resources read the depth format */
static uint32_t view_format(const struct gd_view *v)
{
    if (v->r->d.bind & GD_BIND_DEPTH_STENCIL)
        return resource_format(v->r);
    return host_format(v->format);
}

/* ---- resources */

static uint32_t target_of(const struct gd_resource *r)
{
    switch (r->d.dim) {
    case GD_BUFFER: return 0;
    case GD_TEX1D: return r->d.array > 1 ? 6 : 1;
    case GD_TEX3D: return 3;
    default:
        if (r->d.misc & GD_MISC_TEXTURECUBE)
            return r->d.array > 6 ? 8 : 4;
        return r->d.array > 1 ? 7 : 2;
    }
}

static bool transfer(struct gd_resource *r, bool to_host)
{
    bool ok = true;
    for (uint32_t s = 0; s < r->d.array && ok; s++)
        for (uint32_t m = 0; m < r->d.mips && ok; m++) {
            uint32_t sub = m + s * r->d.mips;
            const struct gd_layout *l = &r->lay[sub];
            uint32_t box[6] = {0, 0, 0, l->width, l->height, l->depth};
            if (r->d.dim == GD_TEX1D && r->d.array > 1)
                box[1] = s, box[4] = 1;
            else if (r->d.dim != GD_TEX3D)
                box[2] = s, box[5] = 1;
            bool last = s + 1 == r->d.array && m + 1 == r->d.mips;
            ok = virtio_gpu_transfer_box(H.gpu, H.ctx, r->host, to_host, m, box, r->off[sub],
                                         r->d.dim == GD_BUFFER ? 0 : l->row_pitch,
                                         r->d.dim == GD_BUFFER ? 0 : l->slice_pitch, !to_host && last);
        }
    return ok;
}

/* the host's copy, made and brought up to date; 0 when the host cannot have it */
static uint32_t to_gpu(struct gd_resource *r)
{
    if (!r->host) {
        if (r->d.samples > 1 || r->d.usage == 3 /* staging */)
            return 0;
        uint32_t format = resource_format(r), bind = 0, b = r->d.bind;
        if (!format)
            return 0;
        if (r->d.dim == GD_BUFFER) /* the host takes one use for a buffer; GL binds any buffer anywhere */
            bind = b & GD_BIND_SHADER_RESOURCE ? B_SAMPLER_VIEW : b == GD_BIND_CONSTANT ? B_CONSTANT : B_VERTEX;
        else
            bind = (b & GD_BIND_RENDER_TARGET ? B_RENDER_TARGET : 0) |
                   (b & GD_BIND_DEPTH_STENCIL ? B_DEPTH_STENCIL : B_SAMPLER_VIEW);
        struct virtio_res3d d = {.target = target_of(r), .format = format, .bind = bind, .w = r->d.width,
                                 .h = r->d.height, .depth = r->d.depth, .array = r->d.array, .levels = r->d.mips};
        flush(false);
        r->host = virtio_gpu_resource_3d(H.gpu, H.ctx, &d, r->data, r->alloc);
        if (!r->host)
            return 0;
        r->where = HOST_CPU_NEWER;
    }
    if (r->where == HOST_CPU_NEWER) {
        flush(false); /* draws sent before see the old contents */
        transfer(r, true);
        r->where = HOST_SAME;
    }
    return r->host;
}

void gd_host_cpu(struct gd_resource *r, bool write)
{
    if (!r || !r->host)
        return;
    lock();
    if (r->where == HOST_GPU_NEWER) {
        flush(false);
        transfer(r, false);
        r->where = HOST_SAME;
    }
    if (write)
        r->where = HOST_CPU_NEWER;
    unlock();
}

void gd_host_forget_resource(struct gd_resource *r)
{
    if (!r->host)
        return;
    lock();
    flush(false);
    virtio_gpu_resource_unref(H.gpu, H.ctx, r->host);
    r->host = 0;
    unlock();
}

void gd_host_forget_view(struct gd_view *v)
{
    if (!v->host)
        return;
    lock();
    destroy(v->d.kind == GD_VIEW_SRV ? O_SAMPLER_VIEW : O_SURFACE, v->host);
    v->host = 0;
    unlock();
}

void gd_host_forget_shader(struct gd_shader *s)
{
    if (s->host <= 0)
        return;
    lock();
    destroy(O_SHADER, (uint32_t)s->host);
    s->host = 0;
    unlock();
}

/* ---- objects */

static uint32_t view_target(const struct gd_view *v)
{
    static const uint8_t t[] = {[VD_BUFFER] = 0, [VD_1D] = 1, [VD_1D_ARRAY] = 6, [VD_2D] = 2, [VD_2D_ARRAY] = 7,
                                [VD_3D] = 3, [VD_CUBE] = 4, [VD_CUBE_ARRAY] = 8};
    return v->vdim >= VD_BUFFER && v->vdim <= VD_CUBE_ARRAY ? t[v->vdim] : 2;
}

static uint32_t sampler_view(struct gd_view *v)
{
    if (v->host)
        return v->host;
    uint32_t res = to_gpu(v->r), format = view_format(v);
    if (!res || !format)
        return 0;
    uint32_t h = handle(), val0, val1;
    if (v->vdim == VD_BUFFER) {
        uint32_t block, bytes = gd_format_bytes(v->format, &block);
        if (!bytes)
            return 0;
        val0 = v->d.first_element, val1 = v->d.first_element + v->d.elements - 1;
    } else {
        uint32_t slices = v->vdim == VD_3D ? 1 : v->d.slices ? v->d.slices : 1;
        val0 = v->vdim == VD_3D ? 0 : (v->d.first_slice | (v->d.first_slice + slices - 1) << 16);
        val1 = v->d.first_mip | (v->d.first_mip + (v->d.mips ? v->d.mips : 1) - 1) << 8;
    }
    cmd(C_CREATE, O_SAMPLER_VIEW, 6);
    put(h), put(res), put(format | view_target(v) << 24), put(val0), put(val1), put(0 | 1 << 3 | 2 << 6 | 3 << 9);
    v->host = h;
    return h;
}

static uint32_t surface(struct gd_view *v)
{
    if (v->host)
        return v->host;
    uint32_t res = to_gpu(v->r), format = view_format(v);
    if (!res || !format || v->r->d.dim == GD_BUFFER)
        return 0;
    uint32_t h = handle(), slices = v->d.slices ? v->d.slices : 1;
    cmd(C_CREATE, O_SURFACE, 5);
    put(h), put(res), put(format), put(v->d.first_mip), put(v->d.first_slice | (v->d.first_slice + slices - 1) << 16);
    v->host = h;
    return h;
}

/* the shader on the host, made from its TGSI text in pieces of a page */
static uint32_t shader(struct gd_shader *s, uint32_t type)
{
    if (s->host)
        return s->host > 0 ? (uint32_t)s->host : 0;
    struct dxbc_tgsi t;
    if (!dxbc_tgsi(&s->sh, &t)) {
        s->host = -1;
        str_copy(H.why, t.error, sizeof(H.why));
        klog("gd: a shader stays in software: %s", t.error);
        return 0;
    }
    uint32_t h = handle(), total = (uint32_t)t.len + 1, tokens = total / 2 + 512;
    for (uint32_t at = 0; at < total;) {
        uint32_t bytes = total - at < 3800 ? total - at : 3800, words = (bytes + 3) / 4;
        cmd(C_CREATE, O_SHADER, 5 + words);
        put(h), put(type), put(at ? (at | 0x80000000u) : total), put(tokens), put(0);
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = 0;
            for (uint32_t k = 0; k < 4 && at + i * 4 + k < total; k++)
                w |= (uint32_t)(uint8_t)(at + i * 4 + k < t.len ? t.text[at + i * 4 + k] : 0) << (8 * k);
            put(w);
        }
        at += bytes;
    }
    memcpy(s->tex_sampler, t.tex_sampler, sizeof(s->tex_sampler));
    s->used_cb = t.used_cb;
    dxbc_tgsi_free(&t);
    s->host = (int32_t)h;
    return h;
}

/* D3D11 numbers to gallium's */
static uint32_t blend_factor(uint32_t f)
{
    static const uint8_t m[20] = {0x11, 0x11, 1, 2, 0x12, 3, 0x13, 4, 0x14, 5, 0x15, 6, 0x11, 0x11, 7, 0x17, 9, 0x19,
                                  10, 0x1a};
    return f < 20 ? m[f] : 1;
}

static uint32_t stencil_op(uint32_t op)
{
    static const uint8_t m[9] = {0, 0, 1, 2, 3, 4, 7, 5, 6};
    return op < 9 ? m[op] : 0;
}

static uint32_t compare(uint32_t f)
{
    return f >= 1 && f <= 8 ? f - 1 : 7;
}

static void blend_state(const struct gd_context *c)
{
    if (H.blend && !memcmp(H.blend_key, &c->blend, sizeof(c->blend)))
        return;
    destroy(O_BLEND, H.blend);
    H.blend = handle();
    memcpy(H.blend_key, &c->blend, sizeof(c->blend));
    cmd(C_CREATE, O_BLEND, 11);
    put(H.blend), put((c->blend.independent ? 1u : 0) | (c->blend.alpha_to_coverage ? 1u << 3 : 0)), put(0);
    for (int i = 0; i < 8; i++) {
        const struct gd_blend_target *b = &c->blend.rt[c->blend.independent ? i : 0];
        uint32_t op = b->op >= 1 && b->op <= 5 ? b->op - 1 : 0, opa = b->op_alpha >= 1 && b->op_alpha <= 5 ? b->op_alpha - 1 : 0;
        put((b->enable ? 1u : 0) | op << 1 | blend_factor(b->src) << 4 | blend_factor(b->dst) << 9 | opa << 14 |
            blend_factor(b->src_alpha) << 17 | blend_factor(b->dst_alpha) << 22 | (b->write_mask & 15) << 27);
    }
    cmd(C_BIND, O_BLEND, 1);
    put(H.blend);
}

static uint32_t stencil_word(const struct gd_depth *d, const struct gd_stencil_op *s)
{
    return (d->stencil_enable ? 1u : 0) | compare(s->func) << 1 | stencil_op(s->fail) << 4 | stencil_op(s->pass) << 7 |
           stencil_op(s->depth_fail) << 10 | (d->read_mask & 0xff) << 13 | (d->write_mask_stencil & 0xff) << 21;
}

static void depth_state(const struct gd_context *c, bool has_depth)
{
    struct gd_depth d = c->depth;
    if (!has_depth)
        d.depth_enable = 0, d.stencil_enable = 0;
    uint8_t key[sizeof(H.dsa_key)];
    memset(key, 0, sizeof(key));
    memcpy(key, &d, sizeof(d));
    if (H.dsa && !memcmp(H.dsa_key, key, sizeof(key)))
        return;
    destroy(O_DSA, H.dsa);
    H.dsa = handle();
    memcpy(H.dsa_key, key, sizeof(key));
    cmd(C_CREATE, O_DSA, 5);
    put(H.dsa);
    put((d.depth_enable ? 1u : 0) | (d.depth_enable && d.write_mask ? 2u : 0) | compare(d.func) << 2);
    put(stencil_word(&d, &d.front)), put(stencil_word(&d, &d.back)), put(0);
    cmd(C_BIND, O_DSA, 1);
    put(H.dsa);
}

static void raster_state(const struct gd_context *c)
{
    const struct gd_raster *r = &c->raster;
    if (H.rs && !memcmp(H.rs_key, r, sizeof(*r)))
        return;
    destroy(O_RASTERIZER, H.rs);
    H.rs = handle();
    memcpy(H.rs_key, r, sizeof(*r));
    uint32_t cull = r->cull == 2 ? 1 : r->cull == 3 ? 2 : 0, fill = r->fill == 2 ? 1 : 0;
    bool offset = r->depth_bias || r->slope_scaled_depth_bias != 0;
    cmd(C_CREATE, O_RASTERIZER, 9);
    put(H.rs);
    put((r->depth_clip ? 1u << 1 : 0) | 1u << 2 /* clip z 0..w */ | 1u << 4 /* first vertex for flat */ | cull << 8 |
        fill << 10 | fill << 12 | (r->scissor ? 1u << 14 : 0) | (r->front_ccw ? 1u << 15 : 0) |
        (offset ? 1u << 20 : 0) | (r->multisample ? 1u << 25 : 0) | 1u << 29 /* pixel centers at .5 */);
    put(fbits(1)), put(0), put(0), put(fbits(1));
    put(fbits((float)r->depth_bias)), put(fbits(r->slope_scaled_depth_bias)), put(fbits(r->depth_bias_clamp));
    cmd(C_BIND, O_RASTERIZER, 1);
    put(H.rs);
}

static uint32_t wrap(uint32_t a)
{
    static const uint8_t m[6] = {0, 0, 4, 2, 3, 6};
    return a < 6 ? m[a] : 0;
}

static uint32_t sampler_state(int stage, int slot, const struct gd_sampler *s)
{
    if (H.samplers[stage][slot] && !memcmp(&H.samp_key[stage][slot], s, sizeof(*s)))
        return H.samplers[stage][slot];
    destroy(O_SAMPLER_STATE, H.samplers[stage][slot]);
    uint32_t h = handle(), f = s->filter & 0xff;
    bool aniso = (f & 0x40) != 0, cmp = (f & 0x80) != 0;
    uint32_t mip = aniso || (f & 1) ? 1 : 0, mag = aniso || (f & 4) ? 1 : 0, min = aniso || (f & 0x10) ? 1 : 0;
    uint32_t a = aniso ? (s->max_anisotropy > 16 ? 16 : s->max_anisotropy) : 0;
    float max_lod = s->max_lod > 1000 ? 1000 : s->max_lod, min_lod = s->min_lod < -1000 ? -1000 : s->min_lod;
    cmd(C_CREATE, O_SAMPLER_STATE, 9);
    put(h);
    put(wrap(s->address_u) | wrap(s->address_v) << 3 | wrap(s->address_w) << 6 | min << 9 | mip << 11 | mag << 13 |
        (cmp ? 1u << 15 : 0) | compare(s->compare) << 16 | 1u << 19 | (a & 0x1f) << 20);
    put(fbits(s->mip_lod_bias)), put(fbits(min_lod)), put(fbits(max_lod));
    for (int i = 0; i < 4; i++)
        put(fbits(s->border[i]));
    H.samplers[stage][slot] = h;
    H.samp_key[stage][slot] = *s;
    return h;
}

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* vertex elements: one per input register of the vertex shader, in register order (TGSI's IN[n] reads element n) */
static bool vertex_elements(const struct gd_context *c, const struct dxbc_shader *vs)
{
    uint32_t el[32][4];
    int n = 0;
    memset(el, 0, sizeof(el));
    for (int i = 0; i < vs->nin; i++) {
        const struct dxbc_sig_elem *s = &vs->in[i];
        if (s->system_value || s->reg >= 32)
            continue;
        uint32_t offset = 0, last_slot = 0xffffffffu;
        bool found = false;
        for (uint32_t k = 0; k < c->layout.count && !found; k++) {
            const struct gd_element *e = &c->layout.e[k];
            if (e->slot != last_slot)
                offset = 0, last_slot = e->slot;
            uint32_t at = e->offset == 0xffffffffu ? offset : e->offset, block;
            offset = at + gd_format_bytes(e->format, &block);
            if (!same_name(e->name, s->name) || e->index != s->index)
                continue;
            uint32_t f = host_format(e->format);
            if (!f || e->slot >= GD_SLOTS_VB)
                return false;
            el[s->reg][0] = at;
            el[s->reg][1] = e->per_instance ? (e->step ? e->step : 0xffffffffu) : 0;
            el[s->reg][2] = e->slot;
            el[s->reg][3] = f;
            found = true;
        }
        if (!found)
            return false;
        if ((int)s->reg + 1 > n)
            n = (int)s->reg + 1;
    }
    for (int r = 0; r < n; r++)
        if (!el[r][3]) /* a register the layout does not feed (vertex and instance numbers) */
            el[r][3] = 31, el[r][2] = 0, el[r][0] = 0;
    if (!n) { /* the host wants at least one */
        el[0][3] = 31;
        n = 1;
    }
    uint8_t key[sizeof(H.ve_key)];
    memset(key, 0, sizeof(key));
    memcpy(key, el, (size_t)n * 16);
    key[sizeof(key) - 1] = (uint8_t)n;
    if (!H.ve || memcmp(key, H.ve_key, sizeof(key))) {
        destroy(O_VERTEX_ELEMENTS, H.ve);
        H.ve = handle();
        memcpy(H.ve_key, key, sizeof(key));
        cmd(C_CREATE, O_VERTEX_ELEMENTS, (uint32_t)n * 4 + 1);
        put(H.ve);
        for (int r = 0; r < n; r++)
            put(el[r][0]), put(el[r][1]), put(el[r][2]), put(el[r][3]);
    }
    cmd(C_BIND, O_VERTEX_ELEMENTS, 1);
    put(H.ve);
    return true;
}

/* constant buffers, texture views and samplers of one stage */
static bool stage_resources(struct gd_context *c, int dstage, uint32_t type, struct gd_shader *s)
{
    const struct dxbc_shader *sh = &s->sh;
    struct stage *st = &c->stage[dstage];
    for (int i = 0; i < DXBC_MAX_CB && i < GD_SLOTS_CB; i++) {
        if (!(s->used_cb & (1u << i)))
            continue;
        struct gd_resource *r = st->cb[i];
        uint32_t res = r ? to_gpu(r) : 0;
        if (r && !res)
            return false;
        uint32_t first = st->cb_first[i] * 16, size = r ? r->d.width : 0;
        uint32_t len = st->cb_count[i] ? st->cb_count[i] * 16 : size;
        if (first > size)
            first = size;
        if (len > size - first)
            len = size - first;
        cmd(C_UNIFORM_BUFFER, 0, 5);
        put(type), put((uint32_t)i + 1), put(first), put(len), put(res);
    }
    uint32_t views[DXBC_MAX_RES], samps[16];
    int nv = 0, ns = 0;
    for (int t = 0; t < DXBC_MAX_RES; t++) {
        if (!sh->res_dim[t])
            continue;
        struct gd_view *v = t < GD_SLOTS_SRV ? st->srv[t] : NULL;
        uint32_t h = v ? sampler_view(v) : 0;
        if (v && !h)
            return false;
        while (nv < t)
            views[nv++] = 0;
        views[nv++] = h;
        int smp = s->tex_sampler[t];
        if (smp != 0xff && t < 16) {
            const struct gd_sampler *ss = &st->sampler[smp];
            static const struct gd_sampler dflt = {.filter = 0x15, .address_u = 3, .address_v = 3, .address_w = 3,
                                                   .max_anisotropy = 1, .compare = 1, .border = {1, 1, 1, 1},
                                                   .min_lod = -1000, .max_lod = 1000};
            while (ns < t)
                samps[ns++] = 0;
            samps[ns++] = sampler_state((int)type, t, st->sampler_set[smp] ? ss : &dflt);
        }
    }
    if (nv) {
        cmd(C_SAMPLER_VIEWS, 0, (uint32_t)nv + 2);
        put(type), put(0);
        for (int i = 0; i < nv; i++)
            put(views[i]);
    }
    if (ns) {
        cmd(C_SAMPLER_STATES, 0, (uint32_t)ns + 2);
        put(type), put(0);
        for (int i = 0; i < ns; i++)
            put(samps[i]);
    }
    return true;
}

static bool refuse(const char *why)
{
    str_copy(H.why, why, sizeof(H.why));
    H.software++;
    return false;
}

static const int8_t prims[14] = {-1, 0, 1, 3, 4, 5, -1, -1, -1, -1, 10, 11, 12, 13};

bool gd_host_draw(struct gd_context *c, bool indexed, uint32_t count, uint32_t start, int32_t base,
                  uint32_t instances, uint32_t first_instance)
{
    if (H.off || H.state < 0)
        return false;
    struct gd_shader *vs = c->stage[DXBC_VS].shader, *ps = c->stage[DXBC_PS].shader;
    if (!vs || vs->sh.stage != DXBC_VS || !count || !instances)
        return false;
    if (c->stage[DXBC_GS].shader || c->stage[DXBC_HS].shader || c->stage[DXBC_DS].shader)
        return refuse("geometry or tessellation shaders");
    if (c->topology >= 14 || prims[c->topology] < 0)
        return refuse("topology");
    for (int i = 0; i < GD_SLOTS_SO; i++)
        if (c->so[i])
            return refuse("stream output");
    for (int i = 0; i < GD_SLOTS_UAV; i++)
        if (c->stage[DXBC_PS].uav[i])
            return refuse("unordered access from the pixel shader");
    if (ps && ps->sh.stage != DXBC_PS)
        return false;
    if (!c->has_layout && vs->sh.nin)
        return false;
    if (vs->host < 0 || (ps && ps->host < 0))
        return refuse(H.why);
    lock();
    bool ok = ready();
    uint32_t vsh = ok ? shader(vs, SH_VERTEX) : 0, fsh = 0;
    if (ok && ps) {
        fsh = shader(ps, SH_FRAGMENT);
    } else if (ok) {
        if (!H.null_fs) {
            static struct gd_shader none;
            none.sh.stage = DXBC_PS;
            H.null_fs = shader(&none, SH_FRAGMENT);
        }
        fsh = H.null_fs;
    }
    if (!vsh || !fsh) {
        unlock();
        return ok ? refuse(H.why) : false;
    }
    /* the targets */
    uint32_t cbufs[8], zsurf = 0;
    int ncb = 0;
    for (int i = 0; i < 8; i++) {
        if (!c->rtv[i])
            continue;
        uint32_t h = surface(c->rtv[i]);
        if (!h)
            goto refused;
        while (ncb < i)
            cbufs[ncb++] = 0;
        cbufs[ncb++] = h;
    }
    if (c->dsv && !(zsurf = surface(c->dsv)))
        goto refused;
    if (!ncb && !zsurf)
        goto refused;
    if (!vertex_elements(c, &vs->sh) || !stage_resources(c, DXBC_VS, SH_VERTEX, vs) ||
        (ps && !stage_resources(c, DXBC_PS, SH_FRAGMENT, ps)))
        goto refused;
    /* vertex and index buffers */
    int nvb = 0;
    for (int i = 0; i < GD_SLOTS_VB; i++)
        if (c->vb[i])
            nvb = i + 1;
    uint32_t vbs[GD_SLOTS_VB];
    for (int i = 0; i < nvb; i++)
        if (c->vb[i] && !(vbs[i] = to_gpu(c->vb[i])))
            goto refused;
        else if (!c->vb[i])
            vbs[i] = 0;
    uint32_t ib = 0;
    if (indexed && (!c->ib || !(ib = to_gpu(c->ib))))
        goto refused;
    /* everything is on the host: the state, then the draw */
    cmd(C_BIND_SHADER, 0, 2);
    put(vsh), put(SH_VERTEX);
    cmd(C_BIND_SHADER, 0, 2);
    put(fsh), put(SH_FRAGMENT);
    blend_state(c);
    depth_state(c, zsurf != 0);
    raster_state(c);
    cmd(C_STENCIL_REF, 0, 1);
    put((c->stencil_ref & 0xff) | (c->stencil_ref & 0xff) << 8);
    cmd(C_BLEND_COLOR, 0, 4);
    for (int i = 0; i < 4; i++)
        put(fbits(c->blend_factor[i]));
    cmd(C_SAMPLE_MASK, 0, 1);
    put(c->sample_mask);
    cmd(C_FRAMEBUFFER, 0, (uint32_t)ncb + 2);
    put((uint32_t)ncb), put(zsurf);
    for (int i = 0; i < ncb; i++)
        put(cbufs[i]);
    float vx = 0, vy = 0, vw, vh, z0 = 0, z1 = 1;
    struct gd_view *first = c->rtv[0] ? c->rtv[0] : c->dsv;
    for (int i = 0; !first && i < 8; i++)
        first = c->rtv[i];
    struct gd_layout l = first->r->lay[first->d.first_mip];
    vw = (float)l.width, vh = (float)l.height;
    if (c->nviewports) {
        vx = c->viewport.x, vy = c->viewport.y, vw = c->viewport.w, vh = c->viewport.h;
        z0 = c->viewport.min_z, z1 = c->viewport.max_z;
    }
    cmd(C_VIEWPORT, 0, 7);
    put(0);
    put(fbits(vw / 2)), put(fbits(-vh / 2)), put(fbits(z1 - z0));
    put(fbits(vx + vw / 2)), put(fbits(vy + vh / 2)), put(fbits(z0));
    if (c->raster.scissor) {
        int32_t x0 = c->nscissors ? c->scissor.left : 0, y0 = c->nscissors ? c->scissor.top : 0;
        int32_t x1 = c->nscissors ? c->scissor.right : 0, y1 = c->nscissors ? c->scissor.bottom : 0;
        x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 < x0 ? x0 : x1, y1 = y1 < y0 ? y0 : y1;
        cmd(C_SCISSOR, 0, 3);
        put(0), put((uint32_t)x0 | (uint32_t)y0 << 16), put((uint32_t)x1 | (uint32_t)y1 << 16);
    }
    if (nvb) {
        cmd(C_VERTEX_BUFFERS, 0, (uint32_t)nvb * 3);
        for (int i = 0; i < nvb; i++)
            put(c->vb_stride[i]), put(c->vb_offset[i]), put(vbs[i]);
    }
    if (indexed) {
        cmd(C_INDEX_BUFFER, 0, 3);
        put(ib), put(c->ib_format == GD_R16_UINT ? 2 : 4), put(c->ib_offset);
    }
    uint32_t mode = (uint32_t)prims[c->topology];
    bool strip = mode == 3 || mode == 5 || mode == 11 || mode == 13;
    cmd(C_DRAW, 0, 12);
    put(start), put(count), put(mode), put(indexed ? 1 : 0), put(instances), put(indexed ? (uint32_t)base : 0);
    put(first_instance), put(indexed && strip ? 1 : 0), put(c->ib_format == GD_R16_UINT ? 0xffff : 0xffffffffu);
    put(0), put(0xffffffffu), put(0);
    for (int i = 0; i < 8; i++)
        if (c->rtv[i])
            c->rtv[i]->r->where = HOST_GPU_NEWER;
    if (c->dsv)
        c->dsv->r->where = HOST_GPU_NEWER;
    H.draws++;
    H.drawn_since++;
    unlock();
    return true;
refused:
    unlock();
    return refuse("a resource or view the host cannot have");
}

/* clears of what is on the host already happen there */
static bool host_clear(struct gd_view *v, uint32_t buffers, const float color[4], float depth, uint32_t stencil)
{
    if (!v->r->host || H.state <= 0 || H.off)
        return false;
    lock();
    uint32_t h = to_gpu(v->r) ? surface(v) : 0;
    if (!h) {
        unlock();
        return false;
    }
    cmd(C_FRAMEBUFFER, 0, buffers & 4 ? 3 : 2);
    if (buffers & 4)
        put(1), put(0), put(h);
    else
        put(0), put(h);
    double d = depth;
    uint64_t bits;
    memcpy(&bits, &d, 8);
    cmd(C_CLEAR, 0, 8);
    put(buffers);
    for (int i = 0; i < 4; i++)
        put(color ? fbits(color[i]) : 0);
    put((uint32_t)bits), put((uint32_t)(bits >> 32)), put(stencil);
    v->r->where = HOST_GPU_NEWER;
    unlock();
    return true;
}

bool gd_host_clear_rtv(struct gd_view *v, const float color[4])
{
    if (gd_format_class(v->format) != FC_FLOAT)
        return false;
    return host_clear(v, 4, color, 0, 0);
}

bool gd_host_clear_dsv(struct gd_view *v, uint32_t flags, float depth, uint32_t stencil)
{
    return host_clear(v, (flags & 1 ? 1u : 0) | (flags & 2 ? 2u : 0), NULL, depth, stencil);
}

/* the samples the host's draws let through, for occlusion queries */
void gd_host_count(uint64_t *pixels)
{
    if (!H.drawn_since || !H.query)
        return;
    lock();
    cmd(C_END_QUERY, 0, 1);
    put(H.query);
    cmd(C_QUERY_RESULT, 0, 2);
    put(H.query), put(1);
    flush(true);
    uint32_t box[6] = {0, 0, 0, 16, 1, 1};
    if (virtio_gpu_transfer_box(H.gpu, H.ctx, H.query_buf, false, 0, box, 0, 0, 0, true)) {
        uint64_t n;
        memcpy(&n, (const uint8_t *)H.query_mem + 8, 8);
        __atomic_add_fetch(pixels, n, __ATOMIC_RELAXED);
    }
    cmd(C_BEGIN_QUERY, 0, 1);
    put(H.query);
    H.drawn_since = 0;
    unlock();
}

/* ---- settings and a look at what it does */

static void get_host(char *buf, size_t cap)
{
    str_copy(buf, H.off ? "off" : "on", cap);
}

static int set_host(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return -1;
    H.off = !strcmp(value, "off");
    return 0;
}

static const struct setting s_host = {"hostgpu", "on|off",
                                      "draw Direct3D on the host's graphics card when there is one (virtio-gpu 3D)",
                                      get_host, set_host};
SETTING(s_host);

COMMAND(hostgpu, NULL, "system", "hostgpu", "what Direct3D draws on the host's graphics card (virtio-gpu 3D)")
{
    UNUSED(argc), UNUSED(argv);
    if (!H.state) {
        lock();
        ready();
        unlock();
    }
    sh_printf(sh, "host gpu: %s, %llu draws there, %llu in software%s%s\n",
              H.off ? "off" : H.state > 0 ? "virgl" : "none", (unsigned long long)H.draws,
              (unsigned long long)H.software, H.why[0] ? ", last: " : "", H.why);
    return 0;
}

#endif
