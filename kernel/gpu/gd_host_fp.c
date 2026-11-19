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
    C_STREAMOUT_TARGETS = 25,
    C_UNIFORM_BUFFER = 27, C_BIND_SHADER = 31, C_SHADER_BUFFERS = 34, C_SHADER_IMAGES = 35, C_MEMORY_BARRIER = 36,
    C_LAUNCH_GRID = 37,
    O_BLEND = 1, O_RASTERIZER = 2, O_DSA = 3, O_SHADER = 4, O_VERTEX_ELEMENTS = 5, O_SAMPLER_VIEW = 6,
    O_SAMPLER_STATE = 7, O_SURFACE = 8, O_QUERY = 9, O_STREAMOUT_TARGET = 10,
    B_DEPTH_STENCIL = 1 << 0, B_RENDER_TARGET = 1 << 1, B_SAMPLER_VIEW = 1 << 3, B_VERTEX = 1 << 4,
    B_CONSTANT = 1 << 6, B_SHADER_BUFFER = 1 << 14, B_CUSTOM = 1 << 17,
    SH_VERTEX = 0, SH_FRAGMENT = 1, SH_GEOMETRY = 2, SH_COMPUTE = 5,
};

#define STREAM_WORDS 1024

static struct {
    int state; /* 0 not tried, 1 ready, -1 none */
    bool off;  /* the "hostgpu" setting */
    int gpu;
    uint32_t ctx, *cmd, n, next;
    /* state objects as last made, and what they were made from */
    uint32_t blend, dsa, rs, ve, samplers[6][16], null_fs;
    uint8_t blend_key[sizeof(struct gd_blend)], dsa_key[sizeof(struct gd_depth) + 4], rs_key[sizeof(struct gd_raster) + 4];
    uint8_t ve_key[sizeof(struct gd_input_layout) + 32 * 4];
    uint8_t ve_slot[16]; /* the vertex buffer slot behind each binding, one binding per shader input */
    int ve_count;
    struct gd_sampler samp_key[6][16];
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
    virtio_gpu_capset(H.gpu, 1, NULL, 0); /* the host's limits; without it texture buffers are 0 texels long */
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
    [GD_R24G8_TYPELESS] = 20, [GD_D24_UNORM_S8_UINT] = 20, [GD_R24_UNORM_X8_TYPELESS] = 21,
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
            return 20; /* S8_UINT_Z24_UNORM: the host has no Z24_UNORM_S8_UINT; see transfer() */
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

/* D24_UNORM_S8_UINT keeps the depth in the low 24 bits and the stencil above; the host's S8_UINT_Z24_UNORM has
   them the other way round, so the words turn around the transfer */
static void turn_depth(struct gd_resource *r, bool to_host)
{
    uint32_t *w = (uint32_t *)r->data;
    for (size_t i = 0; i < r->alloc / 4; i++)
        w[i] = to_host ? w[i] << 8 | w[i] >> 24 : w[i] >> 8 | w[i] << 24;
}

static bool transfer(struct gd_resource *r, bool to_host)
{
    bool ok = true, turn = resource_format(r) == 20;
    if (turn && to_host)
        turn_depth(r, true);
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
                                         r->d.dim == GD_BUFFER ? 0 : l->slice_pitch, (turn || !to_host) && last);
        }
    if (turn)
        turn_depth(r, false);
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
            bind = b & GD_BIND_SHADER_RESOURCE ? B_SAMPLER_VIEW : b & GD_BIND_UNORDERED_ACCESS ? B_SHADER_BUFFER
                 : b == GD_BIND_CONSTANT ? B_CONSTANT : B_VERTEX;
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
    if (s->host <= 0 && s->so_host <= 0)
        return;
    lock();
    if (s->host > 0)
        destroy(O_SHADER, (uint32_t)s->host);
    if (s->so_host > 0)
        destroy(O_SHADER, (uint32_t)s->so_host);
    s->host = s->so_host = 0;
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
/* a shader object on the host from s's TGSI: compute ones for the given typed UAV formats, vertex ones with the
   stream output so (or NULL); 0 when there is no translation (H.why says why) */
static uint32_t create_shader(struct gd_shader *s, uint32_t type, const uint16_t *uav_format, const struct so_decl *so)
{
    struct dxbc_tgsi t;
    if (!dxbc_tgsi(&s->sh, uav_format, &t)) {
        str_copy(H.why, t.error, sizeof(H.why));
        klog("gd: a shader stays in software: %s", t.error);
        return 0;
    }
    /* stream output: the outputs in dwords, each at its offset in its buffer's vertex */
    uint32_t so_out[SO_MAX_ENTRIES][2], nso = 0, at_dw[GD_SLOTS_SO] = {0};
    for (uint32_t i = 0; so && i < so->n; i++) {
        uint32_t b = so->e[i].slot;
        if (so->e[i].reg >= 0)
            so_out[nso][0] = (uint32_t)so->e[i].reg | (uint32_t)so->e[i].comp << 8 | (uint32_t)so->e[i].ncomp << 10 |
                             b << 13 | at_dw[b] << 16,
            so_out[nso++][1] = 0; /* stream 0 */
        at_dw[b] += so->e[i].ncomp;
    }
    uint32_t h = handle(), total = (uint32_t)t.len + 1, tokens = total / 2 + 512;
    uint32_t head = 5 + (nso ? 2 * nso + 4 : 0);
    for (uint32_t at = 0; at < total;) {
        uint32_t bytes = total - at < 3800 ? total - at : 3800, words = (bytes + 3) / 4;
        cmd(C_CREATE, O_SHADER, head + words);
        put(h), put(type), put(at ? (at | 0x80000000u) : total), put(tokens);
        if (type == SH_COMPUTE) {
            put(s->sh.tgsm_bytes); /* its shared memory */
        } else {
            put(nso);
            if (nso) {
                for (int b = 0; b < 4; b++)
                    put(b < GD_SLOTS_SO ? so->stride[b] / 4 : 0);
                for (uint32_t i = 0; i < nso; i++)
                    put(so_out[i][0]), put(so_out[i][1]);
            }
        }
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = 0;
            for (uint32_t k = 0; k < 4 && at + i * 4 + k < total; k++)
                w |= (uint32_t)(uint8_t)(at + i * 4 + k < t.len ? t.text[at + i * 4 + k] : 0) << (8 * k);
            put(w);
        }
        at += bytes;
    }
    memcpy(s->tex_sampler, t.tex_sampler, sizeof(s->tex_sampler));
    s->used_cb = t.used_cb, s->used_uav = t.used_uav, s->used_srv_buf = t.used_srv_buf;
    s->icb = t.icb, s->clip_planes = t.clip_planes;
    dxbc_tgsi_free(&t);
    return h;
}

static uint32_t shader_for(struct gd_shader *s, uint32_t type, const uint16_t *uav_format)
{
    if (s->host > 0 && uav_format && memcmp(s->uav_format, uav_format, sizeof(s->uav_format))) {
        destroy(O_SHADER, (uint32_t)s->host);
        s->host = 0;
    }
    if (uav_format && !s->host)
        memcpy(s->uav_format, uav_format, sizeof(s->uav_format));
    if (s->host)
        return s->host > 0 ? (uint32_t)s->host : 0;
    uint32_t h = create_shader(s, type, uav_format, NULL);
    s->host = h ? (int32_t)h : -1;
    return h;
}

/* the vertex shader vs writing the stream output that so_shader (a vertex shader in the geometry slot) declares:
   kept on so_shader for the vertex shader it was last made from */
static uint32_t so_shader_for(struct gd_shader *so_shader, struct gd_shader *vs)
{
    if (so_shader->so_host && so_shader->so_vs == vs->serial)
        return so_shader->so_host > 0 ? (uint32_t)so_shader->so_host : 0;
    if (so_shader->so_host > 0)
        destroy(O_SHADER, (uint32_t)so_shader->so_host);
    uint32_t h = create_shader(vs, SH_VERTEX, NULL, so_shader->so);
    so_shader->so_host = h ? (int32_t)h : -1, so_shader->so_vs = vs->serial;
    return h;
}

static uint32_t shader(struct gd_shader *s, uint32_t type)
{
    return shader_for(s, type, NULL);
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

/* discard: stream output only, nothing rasterized; clip: the clip distances the last vertex stage writes */
static void raster_state(const struct gd_context *c, bool discard, uint8_t clip)
{
    const struct gd_raster *r = &c->raster;
    if (H.rs && !memcmp(H.rs_key, r, sizeof(*r)) && H.rs_key[sizeof(*r)] == discard &&
        H.rs_key[sizeof(*r) + 1] == clip)
        return;
    destroy(O_RASTERIZER, H.rs);
    H.rs = handle();
    memcpy(H.rs_key, r, sizeof(*r));
    H.rs_key[sizeof(*r)] = discard, H.rs_key[sizeof(*r) + 1] = clip;
    uint32_t cull = r->cull == 2 ? 1 : r->cull == 3 ? 2 : 0, fill = r->fill == 2 ? 1 : 0;
    bool offset = r->depth_bias || r->slope_scaled_depth_bias != 0;
    cmd(C_CREATE, O_RASTERIZER, 9);
    put(H.rs);
    put((r->depth_clip ? 1u << 1 : 0) | (discard ? 1u << 3 : 0) | 1u << 2 /* clip z 0..w */ |
        1u << 4 /* first vertex for flat */ | cull << 8 |
        fill << 10 | fill << 12 | (r->scissor ? 1u << 14 : 0) | (r->front_ccw ? 1u << 15 : 0) |
        (offset ? 1u << 20 : 0) | (r->multisample ? 1u << 25 : 0) | 1u << 29 /* pixel centers at .5 */);
    put(fbits(1)), put(0), put((uint32_t)clip << 24), put(fbits(1));
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
            el[s->reg][2] = e->slot; /* turned into the register's own binding below */
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
    /* virglrenderer sets an element's instance divisor on the binding numbered as the element, not on the
       element's buffer: each element gets the binding of its own number, which then points at its buffer */
    if (n > 16)
        return false;
    for (int r = 0; r < n; r++)
        H.ve_slot[r] = (uint8_t)el[r][2], el[r][2] = (uint32_t)r;
    H.ve_count = n;
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
    if (s->icb && sh->icb_vec4) { /* the immediate constant buffer, made once */
        if (!s->icb_buf) {
            struct gd_res_desc d = {.dim = GD_BUFFER, .width = (uint32_t)sh->icb_vec4 * 16, .height = 1, .depth = 1,
                                    .mips = 1, .array = 1, .bind = GD_BIND_CONSTANT};
            struct gd_init init = {(uint64_t)(uintptr_t)sh->icb, 0, 0};
            uint32_t e;
            s->icb_buf = gd_resource_create(&d, &init, &e);
        }
        uint32_t res = s->icb_buf ? to_gpu(s->icb_buf) : 0;
        if (!res)
            return false;
        cmd(C_UNIFORM_BUFFER, 0, 5);
        put(type), put(15), put(0), put((uint32_t)sh->icb_vec4 * 16), put(res);
    }
    uint32_t views[DXBC_MAX_RES], samps[16];
    int nv = 0, ns = 0;
    for (int t = 0; t < DXBC_MAX_RES; t++) {
        if (!sh->res_dim[t] || (sh->srv_raw >> t & 1)) /* raw and structured ones are shader buffers */
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

/* ---- compute */

/* a buffer view's bytes (raw views count 4-byte words, structured ones the buffer's stride, typed ones the format) */
static void buffer_range(const struct gd_view *v, uint32_t *offset, uint32_t *length)
{
    uint32_t block = 1, size = v->r->d.misc & GD_MISC_BUFFER_STRUCTURED ? v->r->d.stride
                             : (v->d.flags & 1) ? 4 : gd_format_bytes(v->format, &block);
    uint32_t width = v->r->d.width;
    *offset = v->d.first_element * size, *length = v->d.elements * size;
    if (*offset > width)
        *offset = width;
    if (*length > width - *offset)
        *length = width - *offset;
}

/* the formats of the typed UAVs bound to a stage, which its shader is made for */
static void uav_formats(const struct stage *st, const struct dxbc_shader *sh, uint16_t *formats)
{
    memset(formats, 0, sizeof(uint16_t) * DXBC_MAX_UAV);
    for (int u = 0; u < DXBC_MAX_UAV && u < GD_SLOTS_UAV; u++)
        if (!sh->uav_kind[u] && st->uav[u])
            formats[u] = (uint16_t)host_format(st->uav[u]->format);
}

/* a stage's raw and structured buffers (u# at u, t# at 8 + t) and typed UAVs as images; false when one cannot be on
   the host */
static bool bind_memory(struct gd_context *c, int dstage, uint32_t type, struct gd_shader *s, const uint16_t *formats)
{
    struct stage *st = &c->stage[dstage];
    const struct dxbc_shader *sh = &s->sh;
    uint32_t buf[8 + DXBC_MAX_RES][3];
    int nbuf = 0;
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 8 + DXBC_MAX_RES && i < 32; i++) {
        bool uav = i < 8;
        if (uav ? !(s->used_uav >> i & 1) || !sh->uav_kind[i] : !(s->used_srv_buf >> (i - 8) & 1))
            continue;
        struct gd_view *v = uav ? (i < GD_SLOTS_UAV ? st->uav[i] : NULL)
                                : (i - 8 < GD_SLOTS_SRV ? st->srv[i - 8] : NULL);
        if (v) {
            if (v->r->d.dim != GD_BUFFER || !(buf[i][2] = to_gpu(v->r)))
                return false;
            buffer_range(v, &buf[i][0], &buf[i][1]);
        }
        nbuf = i + 1;
    }
    uint32_t img[DXBC_MAX_UAV][5];
    int nimg = 0;
    memset(img, 0, sizeof(img));
    for (int u = 0; u < DXBC_MAX_UAV && u < GD_SLOTS_UAV; u++) {
        if (!(s->used_uav >> u & 1) || sh->uav_kind[u])
            continue;
        struct gd_view *v = st->uav[u];
        if (v) {
            uint32_t res = to_gpu(v->r);
            if (!res || !formats[u])
                return false;
            img[u][0] = formats[u], img[u][1] = 3; /* read and write */
            if (v->r->d.dim == GD_BUFFER) {
                buffer_range(v, &img[u][2], &img[u][3]);
            } else {
                uint32_t slices = v->d.slices ? v->d.slices : 1;
                img[u][2] = v->d.first_slice | (v->d.first_slice + slices - 1) << 16, img[u][3] = v->d.first_mip;
            }
            img[u][4] = res;
        }
        nimg = u + 1;
    }
    if (nbuf) {
        cmd(C_SHADER_BUFFERS, 0, (uint32_t)nbuf * 3 + 2);
        put(type), put(0);
        for (int i = 0; i < nbuf; i++)
            put(buf[i][0]), put(buf[i][1]), put(buf[i][2]);
    }
    if (nimg) {
        cmd(C_SHADER_IMAGES, 0, (uint32_t)nimg * 5 + 2);
        put(type), put(0);
        for (int u = 0; u < nimg; u++)
            for (int k = 0; k < 5; k++)
                put(img[u][k]);
    }
    return true;
}

/* after a dispatch or a draw that wrote unordered access views: what it wrote, for everything after it */
static void wrote_memory(struct stage *st, const struct gd_shader *s)
{
    if (!s->used_uav)
        return;
    cmd(C_MEMORY_BARRIER, 0, 1);
    put(0xffffffffu);
    for (int u = 0; u < DXBC_MAX_UAV && u < GD_SLOTS_UAV; u++)
        if ((s->used_uav >> u & 1) && st->uav[u])
            st->uav[u]->r->where = HOST_GPU_NEWER;
}

bool gd_host_dispatch(struct gd_context *c, uint32_t x, uint32_t y, uint32_t z)
{
    if (H.off || H.state < 0)
        return false;
    struct stage *st = &c->stage[DXBC_CS];
    struct gd_shader *cs = st->shader;
    if (!cs)
        return false;
    uint16_t formats[DXBC_MAX_UAV];
    uav_formats(st, &cs->sh, formats);
    lock();
    if (!ready()) {
        unlock();
        return false;
    }
    uint32_t h = shader_for(cs, SH_COMPUTE, formats);
    if (!h) {
        unlock();
        return refuse(H.why);
    }
    if (!stage_resources(c, DXBC_CS, SH_COMPUTE, cs) || !bind_memory(c, DXBC_CS, SH_COMPUTE, cs, formats)) {
        unlock();
        return refuse("a compute resource");
    }
    cmd(C_BIND_SHADER, 0, 2);
    put(h), put(SH_COMPUTE);
    cmd(C_LAUNCH_GRID, 0, 8);
    for (int i = 0; i < 3; i++)
        put(cs->sh.group[i] ? cs->sh.group[i] : 1);
    put(x), put(y), put(z), put(0), put(0);
    wrote_memory(st, cs);
    H.draws++;
    unlock();
    return true;
}

/* the bytes a draw's stream output writes to each buffer: whole primitives while they fit in every buffer (the host's
   transform feedback stops the same way). Counted here, since the vertex shader writes it with nothing in between;
   false for indexed strips, where a restart index would end a strip */
static bool so_written(const struct gd_context *c, const struct so_decl *so, bool indexed, uint32_t count,
                       uint32_t instances, uint32_t *bytes)
{
    uint32_t t = c->topology, verts, prims;
    switch (t) {
    case 1: verts = 1, prims = count; break;
    case 2: verts = 2, prims = count / 2; break;
    case 3: verts = 2, prims = count > 1 ? count - 1 : 0; break;
    case 4: verts = 3, prims = count / 3; break;
    case 5: verts = 3, prims = count > 2 ? count - 2 : 0; break;
    case 10: verts = 2, prims = count / 4; break;
    case 11: verts = 2, prims = count > 3 ? count - 3 : 0; break;
    case 12: verts = 3, prims = count / 6; break;
    case 13: verts = 3, prims = count >= 6 ? count / 2 - 2 : 0; break;
    default: return false;
    }
    if (indexed && (t == 3 || t == 5 || t == 11 || t == 13))
        return false;
    uint64_t total = (uint64_t)prims * instances;
    for (int b = 0; b < GD_SLOTS_SO; b++) {
        const struct gd_resource *r = c->so[b];
        uint64_t per = (uint64_t)verts * so->stride[b];
        if (r && per) {
            uint64_t room = r->size > r->so_filled ? (r->size - r->so_filled) / per : 0;
            if (room < total)
                total = room;
        }
    }
    for (int b = 0; b < GD_SLOTS_SO; b++)
        bytes[b] = c->so[b] ? (uint32_t)(total * verts * so->stride[b]) : 0;
    return true;
}

bool gd_host_draw(struct gd_context *c, bool indexed, uint32_t count, uint32_t start, int32_t base,
                  uint32_t instances, uint32_t first_instance)
{
    if (H.off || H.state < 0)
        return false;
    struct gd_shader *vs = c->stage[DXBC_VS].shader, *ps = c->stage[DXBC_PS].shader, *gs = c->stage[DXBC_GS].shader;
    if (!vs || vs->sh.stage != DXBC_VS || !count || !instances)
        return false;
    if (c->stage[DXBC_HS].shader || c->stage[DXBC_DS].shader)
        return refuse("tessellation shaders");
    /* stream output: declared by the shader in the geometry slot, a geometry shader or a vertex shader made for it
       (then the vertex shader writes it, and nothing runs in between) */
    struct gd_shader *so_vs = NULL;
    if (gs && gs->sh.stage == DXBC_VS)
        so_vs = gs->so ? gs : NULL, gs = NULL;
    bool targets = false;
    for (int i = 0; i < GD_SLOTS_SO; i++)
        targets |= c->so[i] != NULL;
    const struct so_decl *so = so_vs ? so_vs->so : gs ? gs->so : NULL;
    bool discard = so && so->rasterized != 0;
    if (gs && gs->so && targets)
        return refuse("stream output from a geometry shader");
    if (gs && (gs->sh.stage != DXBC_GS || gs->host < 0))
        return gs->sh.stage == DXBC_GS ? refuse(H.why) : false;
    if (c->topology >= 14 || prims[c->topology] < 0)
        return refuse("topology");
    uint32_t so_bytes[GD_SLOTS_SO] = {0}; /* what stream output writes to each buffer */
    if (so_vs && targets && !so_written(c, so, indexed, count, instances, so_bytes))
        return refuse("stream output of an indexed strip");
    if (so_vs && so_vs->so_host < 0 && so_vs->so_vs == vs->serial)
        return refuse(H.why);
    if (ps && ps->sh.stage != DXBC_PS)
        return false;
    if (!c->has_layout && vs->sh.nin)
        return false;
    if (vs->host < 0 || (ps && ps->host < 0))
        return refuse(H.why);
    lock();
    bool ok = ready();
    uint32_t vsh = ok ? (so_vs ? so_shader_for(so_vs, vs) : shader(vs, SH_VERTEX)) : 0, fsh = 0;
    uint32_t gsh = ok && gs ? shader(gs, SH_GEOMETRY) : 0;
    uint16_t ps_formats[DXBC_MAX_UAV];
    if (ok && ps) {
        uav_formats(&c->stage[DXBC_PS], &ps->sh, ps_formats);
        fsh = shader_for(ps, SH_FRAGMENT, ps_formats);
    } else if (ok) {
        if (!H.null_fs) {
            static struct gd_shader none;
            none.sh.stage = DXBC_PS;
            H.null_fs = shader(&none, SH_FRAGMENT);
        }
        fsh = H.null_fs;
    }
    if (!vsh || !fsh || (gs && !gsh)) {
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
        (ps && (!stage_resources(c, DXBC_PS, SH_FRAGMENT, ps) ||
                !bind_memory(c, DXBC_PS, SH_FRAGMENT, ps, ps_formats))) ||
        (gs && !stage_resources(c, DXBC_GS, SH_GEOMETRY, gs)))
        goto refused;
    /* vertex and index buffers */
    int nvb = H.ve_count;
    uint32_t vbs[16];
    for (int i = 0; i < nvb; i++) {
        struct gd_resource *vb = c->vb[H.ve_slot[i]];
        vbs[i] = 0;
        if (vb && !(vbs[i] = to_gpu(vb)))
            goto refused;
    }
    uint32_t ib = 0;
    if (indexed && (!c->ib || !(ib = to_gpu(c->ib))))
        goto refused;
    /* everything is on the host: the state, then the draw */
    cmd(C_BIND_SHADER, 0, 2);
    put(vsh), put(SH_VERTEX);
    cmd(C_BIND_SHADER, 0, 2);
    put(fsh), put(SH_FRAGMENT);
    cmd(C_BIND_SHADER, 0, 2);
    put(gsh), put(SH_GEOMETRY);
    blend_state(c);
    depth_state(c, zsurf != 0);
    raster_state(c, discard, gs ? gs->clip_planes : vs->clip_planes);
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
            put(c->vb_stride[H.ve_slot[i]]), put(c->vb_offset[H.ve_slot[i]]), put(vbs[i]);
    }
    if (indexed) {
        cmd(C_INDEX_BUFFER, 0, 3);
        put(ib), put(c->ib_format == GD_R16_UINT ? 2 : 4), put(c->ib_offset);
    }
    uint32_t so_targets[GD_SLOTS_SO] = {0};
    int nso = 0;
    if (so_vs && targets) { /* each buffer from where it is filled to */
        for (int b = 0; b < GD_SLOTS_SO; b++) {
            struct gd_resource *r = c->so[b];
            if (!r)
                continue;
            uint32_t res = to_gpu(r), filled = r->so_filled < r->size ? r->so_filled : (uint32_t)r->size;
            if (!res)
                goto refused;
            so_targets[b] = handle();
            cmd(C_CREATE, O_STREAMOUT_TARGET, 4);
            put(so_targets[b]), put(res), put(filled), put((uint32_t)r->size - filled);
            nso = b + 1;
        }
        cmd(C_STREAMOUT_TARGETS, 0, 1 + (uint32_t)nso);
        put(0); /* none appended: each starts at its offset */
        for (int b = 0; b < nso; b++)
            put(so_targets[b]);
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
    if (ps)
        wrote_memory(&c->stage[DXBC_PS], ps);
    if (nso) {
        cmd(C_STREAMOUT_TARGETS, 0, 1);
        put(0);
        for (int b = 0; b < nso; b++)
            if (so_targets[b]) {
                destroy(O_STREAMOUT_TARGET, so_targets[b]);
                c->so[b]->so_filled += so_bytes[b];
                c->so[b]->where = HOST_GPU_NEWER;
            }
    }
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
