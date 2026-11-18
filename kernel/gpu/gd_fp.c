/* The graphics device (include/synth/gd.h): resources, views, shaders and a context whose draws run the program's
   DXBC shaders on the software rasterizer, with Direct3D's input assembly, depth, stencil and blending. */

#if defined(__x86_64__) || defined(RS_HOST)

#include <synth/raster.h>

#include "gd_internal.h"
#include "gmath.h"

#ifdef RS_HOST
#include <stdlib.h>
#include <string.h>
#define kmalloc malloc
#define kfree free
static void *kzalloc(size_t n)
{
    return calloc(1, n);
}
static void *kmalloc_pages(size_t n)
{
    void *p = aligned_alloc(4096, n);
    if (p)
        memset(p, 0, n);
    return p;
}
#define kfree_pages free
static void hal_parallel(void (*fn)(void *ctx, int from, int to), void *ctx, int count, int grain)
{
    (void)grain;
    fn(ctx, 0, count);
}
#else
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vmm.h>
#endif

#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define MAX_BYTES ((size_t)1 << 31) /* one resource */

void gd_texture_init(void);

static struct gd_stats stats;

void gd_get_stats(struct gd_stats *out)
{
    *out = stats;
}

static void note_fault(const char *what)
{
    if (__atomic_add_fetch(&stats.shader_faults, 1, __ATOMIC_RELAXED) > 1)
        return; /* the first fault says why; the others come from the same shader */
    size_t i = 0;
    for (; what[i] && i < sizeof(stats.last_fault) - 1; i++)
        stats.last_fault[i] = what[i];
    stats.last_fault[i] = 0;
}

/* ---- resources */

static uint32_t mip_size(uint32_t v, uint32_t m)
{
    v >>= m;
    return v ? v : 1;
}

static uint32_t full_chain(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t big = w > h ? w : h, n = 1;
    big = big > d ? big : d;
    while (big > 1)
        big >>= 1, n++;
    return n;
}

struct gd_resource *gd_resource_create(const struct gd_res_desc *desc, const struct gd_init *init, uint32_t *error)
{
    gd_texture_init();
    *error = E_INVALIDARG;
    struct gd_res_desc d = *desc;
    if (d.dim < GD_BUFFER || d.dim > GD_TEX3D || !d.width)
        return NULL;
    uint32_t block = 1, bytes = 1;
    if (d.dim == GD_BUFFER) {
        d.height = d.depth = d.mips = d.array = 1;
    } else {
        bytes = gd_format_bytes(d.format, &block);
        if (!bytes)
            return NULL;
        d.height = d.dim == GD_TEX1D ? 1 : (d.height ? d.height : 1);
        d.depth = d.dim == GD_TEX3D ? (d.depth ? d.depth : 1) : 1;
        d.array = d.dim == GD_TEX3D ? 1 : (d.array ? d.array : 1);
        uint32_t chain = full_chain(d.width, d.height, d.depth);
        d.mips = d.mips && d.mips <= chain ? d.mips : (d.mips ? 0 : chain);
        if (!d.mips || d.width > 16384 || d.height > 16384 || d.depth > 2048 || d.array > 2048)
            return NULL;
    }
    struct gd_resource *r = kzalloc(sizeof(*r));
    if (!r) {
        *error = E_OUTOFMEMORY;
        return NULL;
    }
    r->refs = 1;
    r->d = d;
    r->subs = d.mips * d.array;
    r->lay = kzalloc(sizeof(*r->lay) * r->subs);
    r->off = kzalloc(sizeof(*r->off) * r->subs);
    if (!r->lay || !r->off)
        goto oom;
    size_t total = 0;
    for (uint32_t s = 0; s < d.array; s++)
        for (uint32_t m = 0; m < d.mips; m++) {
            struct gd_layout *l = &r->lay[m + s * d.mips];
            if (d.dim == GD_BUFFER) {
                *l = (struct gd_layout){d.width, d.width, d.width, d.width, 1, 1};
            } else {
                l->width = mip_size(d.width, m), l->height = mip_size(d.height, m), l->depth = mip_size(d.depth, m);
                uint32_t bw = (l->width + block - 1) / block, bh = (l->height + block - 1) / block;
                l->row_pitch = bw * bytes;
                l->slice_pitch = l->row_pitch * bh;
                l->size = l->slice_pitch * l->depth;
            }
            r->off[m + s * d.mips] = total;
            total += ((size_t)l->size + 15) & ~(size_t)15;
            if (total > MAX_BYTES) {
                *error = E_OUTOFMEMORY;
                goto fail;
            }
        }
    r->size = total;
    r->alloc = total ? (total + 4095) & ~(size_t)4095 : 4096;
    r->data = kmalloc_pages(r->alloc);
    if (!r->data)
        goto oom;
    if (init)
        for (uint32_t i = 0; i < r->subs; i++)
            if (init[i].data) {
                const struct gd_layout *l = &r->lay[i];
                gd_update(r, i, NULL, (const void *)(uintptr_t)init[i].data,
                          init[i].row_pitch ? init[i].row_pitch : l->row_pitch,
                          init[i].slice_pitch ? init[i].slice_pitch : l->slice_pitch);
            }
    *error = 0;
    return r;
oom:
    *error = E_OUTOFMEMORY;
fail:
    kfree(r->lay);
    kfree(r->off);
    kfree(r);
    return NULL;
}

void gd_resource_ref(struct gd_resource *r)
{
    if (r)
        __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
}

void gd_resource_unref(struct gd_resource *r)
{
    if (!r || __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL))
        return;
    kfree_pages(r->data);
    kfree(r->lay);
    kfree(r->off);
    kfree(r);
}

uint8_t *gd_resource_memory(struct gd_resource *r, uint32_t *bytes)
{
    *bytes = (uint32_t)r->alloc;
    return r->data;
}

void gd_resource_desc(const struct gd_resource *r, struct gd_res_desc *out)
{
    *out = r->d;
}

uint32_t gd_subresources(const struct gd_resource *r)
{
    return r->subs;
}

uint8_t *gd_subresource(struct gd_resource *r, uint32_t sub, struct gd_layout *layout)
{
    if (!r || sub >= r->subs)
        return NULL;
    if (layout)
        *layout = r->lay[sub];
    return r->data + r->off[sub];
}

static uint32_t res_block(const struct gd_resource *r, uint32_t *bytes)
{
    uint32_t block = 1;
    *bytes = r->d.dim == GD_BUFFER ? 1 : gd_format_bytes(r->d.format, &block);
    return block;
}

void gd_update(struct gd_resource *r, uint32_t sub, const struct gd_box *box, const void *src, uint32_t row_pitch,
               uint32_t slice_pitch)
{
    if (!r || sub >= r->subs || !src)
        return;
    const struct gd_layout *l = &r->lay[sub];
    uint8_t *base = r->data + r->off[sub];
    if (r->d.dim == GD_BUFFER) {
        uint32_t from = box ? box->left : 0, to = box ? box->right : l->width;
        if (to > l->width)
            to = l->width;
        if (from < to)
            memcpy(base + from, src, to - from);
        return;
    }
    uint32_t bytes, block = res_block(r, &bytes);
    struct gd_box b = box ? *box : (struct gd_box){0, 0, 0, l->width, l->height, l->depth};
    if (b.right > l->width)
        b.right = l->width;
    if (b.bottom > l->height)
        b.bottom = l->height;
    if (b.back > l->depth)
        b.back = l->depth;
    if (b.left >= b.right || b.top >= b.bottom || b.front >= b.back)
        return;
    uint32_t bx0 = b.left / block, bx1 = (b.right + block - 1) / block;
    uint32_t by0 = b.top / block, by1 = (b.bottom + block - 1) / block;
    size_t row = (size_t)(bx1 - bx0) * bytes;
    for (uint32_t z = b.front; z < b.back; z++)
        for (uint32_t y = by0; y < by1; y++)
            memcpy(base + (size_t)z * l->slice_pitch + (size_t)y * l->row_pitch + (size_t)bx0 * bytes,
                   (const uint8_t *)src + (size_t)(z - b.front) * slice_pitch + (size_t)(y - by0) * row_pitch, row);
}

void gd_copy_resource(struct gd_resource *dst, struct gd_resource *src)
{
    if (!dst || !src || dst == src)
        return;
    if (dst->size == src->size && dst->subs == src->subs) {
        memcpy(dst->data, src->data, dst->size);
        return;
    }
    for (uint32_t i = 0; i < dst->subs && i < src->subs; i++)
        gd_copy_region(dst, i, 0, 0, 0, src, i, NULL);
}

void gd_copy_region(struct gd_resource *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                    struct gd_resource *src, uint32_t src_sub, const struct gd_box *box)
{
    if (!dst || !src || dst_sub >= dst->subs || src_sub >= src->subs)
        return;
    const struct gd_layout *sl = &src->lay[src_sub], *dl = &dst->lay[dst_sub];
    uint8_t *sb = src->data + src->off[src_sub], *db = dst->data + dst->off[dst_sub];
    if (src->d.dim == GD_BUFFER || dst->d.dim == GD_BUFFER) {
        uint32_t from = box ? box->left : 0, to = box ? box->right : sl->width;
        to = to > sl->width ? sl->width : to;
        if (from < to && x < dl->width) {
            uint32_t n = to - from < dl->width - x ? to - from : dl->width - x;
            memmove(db + x, sb + from, n);
        }
        return;
    }
    uint32_t bytes, block = res_block(src, &bytes), dbytes, dblock = res_block(dst, &dbytes);
    if (bytes != dbytes || block != dblock)
        return;
    struct gd_box b = box ? *box : (struct gd_box){0, 0, 0, sl->width, sl->height, sl->depth};
    b.right = b.right > sl->width ? sl->width : b.right;
    b.bottom = b.bottom > sl->height ? sl->height : b.bottom;
    b.back = b.back > sl->depth ? sl->depth : b.back;
    if (b.left >= b.right || b.top >= b.bottom || b.front >= b.back)
        return;
    uint32_t bw = (b.right - b.left + block - 1) / block, bh = (b.bottom - b.top + block - 1) / block;
    uint32_t dbw = (dl->width - x + block - 1) / block, dbh = (dl->height - y + block - 1) / block;
    if (x >= dl->width || y >= dl->height || z >= dl->depth)
        return;
    bw = bw < dbw ? bw : dbw, bh = bh < dbh ? bh : dbh;
    for (uint32_t k = 0; k < b.back - b.front && z + k < dl->depth; k++)
        for (uint32_t j = 0; j < bh; j++)
            memmove(db + (size_t)(z + k) * dl->slice_pitch + (size_t)(y / block + j) * dl->row_pitch +
                        (size_t)(x / block) * bytes,
                    sb + (size_t)(b.front + k) * sl->slice_pitch + (size_t)(b.top / block + j) * sl->row_pitch +
                        (size_t)(b.left / block) * bytes,
                    (size_t)bw * bytes);
}

/* ---- views */

struct gd_view *gd_view_create(struct gd_resource *r, const struct gd_view_desc *desc, uint32_t *error)
{
    *error = E_INVALIDARG;
    if (!r || !desc || desc->kind < GD_VIEW_SRV || desc->kind > GD_VIEW_UAV)
        return NULL;
    struct gd_view_desc d = *desc;
    uint32_t format = d.format ? d.format : r->d.format;
    if (d.kind == GD_VIEW_SRV || d.kind == GD_VIEW_RTV || d.kind == GD_VIEW_UAV)
        format = gd_format_typed(format);
    int vdim;
    if (r->d.dim == GD_BUFFER) {
        vdim = VD_BUFFER;
        uint32_t block, bytes = r->d.misc & GD_MISC_BUFFER_STRUCTURED ? r->d.stride : gd_format_bytes(format, &block);
        if (!bytes)
            bytes = 4;
        if (!d.elements)
            d.elements = (uint32_t)(r->size / bytes) - d.first_element;
    } else {
        uint32_t block;
        if (!gd_format_bytes(format, &block))
            return NULL;
        switch (d.dim) {
        case 2: vdim = VD_1D; break;
        case 3: vdim = VD_1D_ARRAY; break;
        case 4: case 6: vdim = VD_2D; break;
        case 5: case 7: vdim = VD_2D_ARRAY; break;
        case 8: vdim = VD_3D; break;
        case 9: vdim = VD_CUBE; break;
        case 10: vdim = VD_CUBE_ARRAY; break;
        default:
            vdim = r->d.dim == GD_TEX1D ? (r->d.array > 1 ? VD_1D_ARRAY : VD_1D)
                 : r->d.dim == GD_TEX3D ? VD_3D
                 : (r->d.misc & GD_MISC_TEXTURECUBE) ? (r->d.array > 6 ? VD_CUBE_ARRAY : VD_CUBE)
                 : r->d.array > 1 ? VD_2D_ARRAY : VD_2D;
            break;
        }
        if (d.first_mip >= r->d.mips)
            return NULL;
        if (!d.mips || d.first_mip + d.mips > r->d.mips)
            d.mips = r->d.mips - d.first_mip;
        if (d.kind != GD_VIEW_SRV)
            d.mips = 1;
        uint32_t layers = vdim == VD_3D ? mip_size(r->d.depth, d.first_mip) : r->d.array;
        if (d.first_slice >= layers)
            d.first_slice = 0;
        if (!d.slices || d.first_slice + d.slices > layers)
            d.slices = layers - d.first_slice;
        if (vdim == VD_CUBE)
            d.slices = 6;
        if (vdim == VD_1D || vdim == VD_2D)
            d.slices = 1;
    }
    struct gd_view *v = kzalloc(sizeof(*v));
    if (!v) {
        *error = E_OUTOFMEMORY;
        return NULL;
    }
    v->refs = 1;
    v->r = r;
    gd_resource_ref(r);
    v->d = d;
    v->d.format = format;
    v->format = format;
    v->vdim = vdim;
    *error = 0;
    return v;
}

void gd_view_ref(struct gd_view *v)
{
    if (v)
        __atomic_add_fetch(&v->refs, 1, __ATOMIC_RELAXED);
}

void gd_view_unref(struct gd_view *v)
{
    if (!v || __atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL))
        return;
    gd_resource_unref(v->r);
    kfree(v);
}

struct gd_resource *gd_view_resource(struct gd_view *v)
{
    return v ? v->r : NULL;
}

void gd_view_desc(const struct gd_view *v, struct gd_view_desc *out)
{
    *out = v->d;
}

/* the subresource a render target or depth view writes, for one slice of it */
static uint8_t *target_of(struct gd_view *v, uint32_t slice, struct gd_layout *l)
{
    struct gd_resource *r = v->r;
    if (r->d.dim == GD_BUFFER)
        return NULL;
    if (r->d.dim == GD_TEX3D) {
        uint8_t *p = gd_subresource(r, v->d.first_mip, l);
        if (!p || v->d.first_slice + slice >= l->depth)
            return NULL;
        return p + (size_t)(v->d.first_slice + slice) * l->slice_pitch;
    }
    return gd_subresource(r, v->d.first_mip + (v->d.first_slice + slice) * r->d.mips, l);
}

void gd_clear_rtv(struct gd_view *v, const float color[4])
{
    if (!v || v->r->d.dim == GD_BUFFER)
        return;
    uint32_t block, bytes = gd_format_bytes(v->format, &block);
    if (!bytes || block != 1)
        return;
    uint8_t px[16] = {0};
    uint32_t raw[4];
    int cls = gd_format_class(v->format);
    for (int i = 0; i < 4; i++) /* integer targets get the numbers the floats stand for */
        raw[i] = cls == FC_FLOAT ? U(color[i]) : cls == FC_SINT ? (uint32_t)(int32_t)color[i] : (uint32_t)color[i];
    gd_texel_store(v->format, px, raw, 15);
    for (uint32_t s = 0; s < v->d.slices; s++) {
        struct gd_layout l;
        uint8_t *p = target_of(v, s, &l);
        if (!p)
            continue;
        for (uint32_t y = 0; y < l.height; y++) {
            uint8_t *row = p + (size_t)y * l.row_pitch;
            if (bytes == 4) {
                uint32_t w;
                memcpy(&w, px, 4);
                uint32_t *q = (uint32_t *)row;
                for (uint32_t x = 0; x < l.width; x++)
                    q[x] = w;
            } else {
                for (uint32_t x = 0; x < l.width; x++)
                    memcpy(row + (size_t)x * bytes, px, bytes);
            }
        }
    }
}

void gd_clear_dsv(struct gd_view *v, uint32_t flags, float depth, uint32_t stencil)
{
    if (!v || !gd_format_depth(v->r->d.format))
        return;
    uint32_t f = v->r->d.format, block, bytes = gd_format_bytes(f, &block);
    for (uint32_t s = 0; s < v->d.slices; s++) {
        struct gd_layout l;
        uint8_t *p = target_of(v, s, &l);
        if (!p)
            continue;
        for (uint32_t y = 0; y < l.height; y++)
            for (uint32_t x = 0; x < l.width; x++) {
                uint8_t *t = p + (size_t)y * l.row_pitch + (size_t)x * bytes;
                if (flags & 1)
                    gd_depth_write(f, t, depth);
                if (flags & 2)
                    gd_stencil_write(f, t, stencil);
            }
    }
}

void gd_generate_mips(struct gd_view *v)
{
    if (!v || v->r->d.dim == GD_BUFFER || v->r->d.dim == GD_TEX3D)
        return;
    struct gd_resource *r = v->r;
    uint32_t f = v->format, block, bytes = gd_format_bytes(f, &block);
    if (!bytes || block != 1 || gd_format_class(f) != FC_FLOAT)
        return;
    for (uint32_t s = v->d.first_slice; s < v->d.first_slice + v->d.slices; s++)
        for (uint32_t m = v->d.first_mip; m + 1 < v->d.first_mip + v->d.mips; m++) {
            struct gd_layout a = {0}, b = {0};
            const uint8_t *src = gd_subresource(r, m + s * r->d.mips, &a);
            uint8_t *dst = gd_subresource(r, m + 1 + s * r->d.mips, &b);
            if (!src || !dst)
                continue;
            for (uint32_t y = 0; y < b.height; y++)
                for (uint32_t x = 0; x < b.width; x++) {
                    float sum[4] = {0, 0, 0, 0};
                    for (int k = 0; k < 4; k++) {
                        uint32_t sx = x * 2 + (uint32_t)(k & 1), sy = y * 2 + (uint32_t)(k >> 1), t[4];
                        sx = sx < a.width ? sx : a.width - 1, sy = sy < a.height ? sy : a.height - 1;
                        gd_texel_load(f, src + (size_t)sy * a.row_pitch + (size_t)sx * bytes, 0, 0, t);
                        for (int c = 0; c < 4; c++)
                            sum[c] += F(t[c]);
                    }
                    uint32_t o[4];
                    for (int c = 0; c < 4; c++)
                        o[c] = U(sum[c] * 0.25f);
                    gd_texel_store(f, dst + (size_t)y * b.row_pitch + (size_t)x * bytes, o, 15);
                }
        }
}

/* ---- shaders */

struct gd_shader {
    int refs;
    struct dxbc_shader sh;
};

struct gd_shader *gd_shader_create(const void *blob, uint32_t size, char error[64])
{
    struct gd_shader *s = kzalloc(sizeof(*s));
    if (!s) {
        memcpy(error, "out of memory", 14);
        return NULL;
    }
    if (!dxbc_parse(&s->sh, blob, size)) {
        memcpy(error, s->sh.error, sizeof(s->sh.error));
        kfree(s);
        return NULL;
    }
    s->refs = 1;
    error[0] = 0;
    return s;
}

void gd_shader_ref(struct gd_shader *s)
{
    if (s)
        __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
}

void gd_shader_unref(struct gd_shader *s)
{
    if (!s || __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL))
        return;
    dxbc_free(&s->sh);
    kfree(s);
}

int gd_shader_stage(const struct gd_shader *s)
{
    return (int)s->sh.stage;
}

/* ---- the context */

struct stage {
    struct gd_shader *shader;
    struct gd_resource *cb[GD_SLOTS_CB];
    uint32_t cb_first[GD_SLOTS_CB], cb_count[GD_SLOTS_CB];
    struct gd_view *srv[GD_SLOTS_SRV];
    struct gd_sampler sampler[GD_SLOTS_SAMPLER];
    bool sampler_set[GD_SLOTS_SAMPLER];
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
};

static const struct gd_raster default_raster = {.fill = 3, .cull = 3, .depth_clip = 1};
static const struct gd_depth default_depth = {.depth_enable = 1, .write_mask = 1, .func = 2, .read_mask = 0xff,
                                              .write_mask_stencil = 0xff, .front = {1, 1, 1, 8},
                                              .back = {1, 1, 1, 8}};
static const struct gd_sampler default_sampler = {.filter = 0x15, .address_u = 3, .address_v = 3, .address_w = 3,
                                                  .max_anisotropy = 1, .compare = 1, .border = {1, 1, 1, 1},
                                                  .min_lod = -3.4e38f, .max_lod = 3.4e38f};

static void default_blend(struct gd_blend *b)
{
    memset(b, 0, sizeof(*b));
    for (int i = 0; i < 8; i++)
        b->rt[i] = (struct gd_blend_target){0, 2, 1, 1, 2, 1, 1, 15};
}

struct gd_context *gd_context_create(void)
{
    gd_texture_init();
    struct gd_context *c = kzalloc(sizeof(*c));
    if (c)
        gd_context_clear(c);
    return c;
}

void gd_context_clear(struct gd_context *c)
{
    for (int s = 0; s < GD_STAGES; s++) {
        struct stage *st = &c->stage[s];
        gd_shader_unref(st->shader);
        for (int i = 0; i < GD_SLOTS_CB; i++)
            gd_resource_unref(st->cb[i]);
        for (int i = 0; i < GD_SLOTS_SRV; i++)
            gd_view_unref(st->srv[i]);
    }
    for (int i = 0; i < GD_SLOTS_VB; i++)
        gd_resource_unref(c->vb[i]);
    gd_resource_unref(c->ib);
    for (int i = 0; i < 8; i++)
        gd_view_unref(c->rtv[i]);
    gd_view_unref(c->dsv);
    memset(c, 0, sizeof(*c));
    c->raster = default_raster;
    c->depth = default_depth;
    default_blend(&c->blend);
    c->blend_factor[0] = c->blend_factor[1] = c->blend_factor[2] = c->blend_factor[3] = 1;
    c->sample_mask = 0xffffffffu;
}

void gd_context_destroy(struct gd_context *c)
{
    if (!c)
        return;
    gd_context_clear(c);
    kfree(c);
}

void gd_set_vertex_buffer(struct gd_context *c, uint32_t slot, struct gd_resource *r, uint32_t stride, uint32_t offset)
{
    if (slot >= GD_SLOTS_VB)
        return;
    gd_resource_ref(r);
    gd_resource_unref(c->vb[slot]);
    c->vb[slot] = r, c->vb_stride[slot] = stride, c->vb_offset[slot] = offset;
}

void gd_set_index_buffer(struct gd_context *c, struct gd_resource *r, uint32_t format, uint32_t offset)
{
    gd_resource_ref(r);
    gd_resource_unref(c->ib);
    c->ib = r, c->ib_format = format, c->ib_offset = offset;
}

void gd_set_topology(struct gd_context *c, uint32_t topology)
{
    c->topology = topology;
}

void gd_set_input_layout(struct gd_context *c, const struct gd_input_layout *layout)
{
    c->has_layout = layout != NULL;
    if (layout)
        c->layout = *layout;
}

void gd_set_shader(struct gd_context *c, uint32_t stage, struct gd_shader *s)
{
    if (stage >= GD_STAGES)
        return;
    gd_shader_ref(s);
    gd_shader_unref(c->stage[stage].shader);
    c->stage[stage].shader = s;
}

void gd_set_constant_buffer(struct gd_context *c, uint32_t stage, uint32_t slot, struct gd_resource *r,
                            uint32_t first, uint32_t count)
{
    if (stage >= GD_STAGES || slot >= GD_SLOTS_CB)
        return;
    struct stage *st = &c->stage[stage];
    gd_resource_ref(r);
    gd_resource_unref(st->cb[slot]);
    st->cb[slot] = r, st->cb_first[slot] = first, st->cb_count[slot] = count;
}

void gd_set_shader_resource(struct gd_context *c, uint32_t stage, uint32_t slot, struct gd_view *v)
{
    if (stage >= GD_STAGES || slot >= GD_SLOTS_SRV)
        return;
    gd_view_ref(v);
    gd_view_unref(c->stage[stage].srv[slot]);
    c->stage[stage].srv[slot] = v;
}

void gd_set_sampler(struct gd_context *c, uint32_t stage, uint32_t slot, const struct gd_sampler *s)
{
    if (stage >= GD_STAGES || slot >= GD_SLOTS_SAMPLER)
        return;
    c->stage[stage].sampler_set[slot] = s != NULL;
    c->stage[stage].sampler[slot] = s ? *s : default_sampler;
}

void gd_set_viewports(struct gd_context *c, uint32_t count, const struct gd_viewport *v)
{
    c->nviewports = count;
    if (count)
        c->viewport = v[0];
}

void gd_set_scissors(struct gd_context *c, uint32_t count, const struct gd_rect *r)
{
    c->nscissors = count;
    if (count)
        c->scissor = r[0];
}

void gd_set_raster(struct gd_context *c, const struct gd_raster *r)
{
    c->raster = r ? *r : default_raster;
}

void gd_set_blend(struct gd_context *c, const struct gd_blend *b, const float factor[4], uint32_t sample_mask)
{
    if (b)
        c->blend = *b;
    else
        default_blend(&c->blend);
    for (int i = 0; i < 4; i++)
        c->blend_factor[i] = factor ? factor[i] : 1;
    c->sample_mask = sample_mask;
}

void gd_set_depth(struct gd_context *c, const struct gd_depth *d, uint32_t stencil_ref)
{
    c->depth = d ? *d : default_depth;
    c->stencil_ref = stencil_ref;
}

void gd_set_render_target(struct gd_context *c, uint32_t slot, struct gd_view *rtv)
{
    if (slot >= 8)
        return;
    gd_view_ref(rtv);
    gd_view_unref(c->rtv[slot]);
    c->rtv[slot] = rtv;
}

void gd_set_depth_target(struct gd_context *c, struct gd_view *dsv)
{
    gd_view_ref(dsv);
    gd_view_unref(c->dsv);
    c->dsv = dsv;
}

/* ---- drawing */

#define MAX_REGS 32

enum { IN_NONE, IN_ELEMENT, IN_VERTEX_ID, IN_INSTANCE_ID };

struct vs_input {
    int kind;
    uint32_t format, mask;
    const uint8_t *base; /* the element's first byte in the vertex buffer */
    size_t limit;        /* bytes from base that are in the buffer */
    uint32_t stride, step;
    bool per_instance;
};

enum { PSIN_NONE, PSIN_VARY, PSIN_POSITION, PSIN_FRONT, PSIN_ZERO };

struct ps_input {
    int kind, from; /* varying number */
};

struct target {
    uint8_t *base;
    uint32_t pitch, format, bytes, w, h;
    int cls;
    struct gd_blend_target blend;
};

struct stage_env {
    const struct stage *st;
};

struct draw {
    struct gd_context *c;
    const struct dxbc_shader *vs, *ps;
    struct vs_input in[MAX_REGS];
    int pos_reg, nvary;
    uint32_t instance, first_instance;
    struct ps_input psin[MAX_REGS];
    struct dxbc_env vs_env, ps_env;
    struct target rt[8];
    int nrt;
    uint32_t write_any;
    bool dual_source;
    /* depth and stencil */
    uint8_t *db;
    uint32_t dpitch, dformat, dbytes;
    bool depth_test, depth_write, stencil;
    float bias, slope_bias, bias_clamp;
    float min_z, max_z;
    bool early;
    size_t vs_scratch, ps_scratch;
};

static void env_sample(void *ctx, int texture, int sampler, const float coord[4], const float ddx[4],
                       const float ddy[4], float lod, float bias, const int offset[3], const float *ref, float out[4])
{
    const struct stage *st = ctx;
    const struct gd_view *v = texture >= 0 && texture < GD_SLOTS_SRV ? st->srv[texture] : NULL;
    const struct gd_sampler *s = sampler >= 0 && sampler < GD_SLOTS_SAMPLER && st->sampler_set[sampler]
                                     ? &st->sampler[sampler]
                                     : &default_sampler;
    if (!v) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    gd_sample(v, s, coord, ddx, ddy, lod, bias, offset, ref, out);
}

static void env_gather(void *ctx, int texture, int sampler, const float coord[4], const int offset[3], int component,
                       const float *ref, float out[4])
{
    const struct stage *st = ctx;
    const struct gd_view *v = texture >= 0 && texture < GD_SLOTS_SRV ? st->srv[texture] : NULL;
    const struct gd_sampler *s = sampler >= 0 && sampler < GD_SLOTS_SAMPLER && st->sampler_set[sampler]
                                     ? &st->sampler[sampler]
                                     : &default_sampler;
    gd_gather(v, s, coord, offset, component, ref, out);
}

static void env_load(void *ctx, int texture, const int32_t coord[4], const int offset[3], uint32_t out[4])
{
    const struct stage *st = ctx;
    gd_texel_fetch(texture >= 0 && texture < GD_SLOTS_SRV ? st->srv[texture] : NULL, coord, offset, out);
}

static void env_info(void *ctx, int texture, uint32_t mip, uint32_t out[4])
{
    const struct stage *st = ctx;
    gd_view_size(texture >= 0 && texture < GD_SLOTS_SRV ? st->srv[texture] : NULL, mip, out);
}

static void make_env(struct dxbc_env *e, const struct stage *st)
{
    memset(e, 0, sizeof(*e));
    e->ctx = (void *)st;
    e->sample = env_sample;
    e->gather = env_gather;
    e->load = env_load;
    e->info = env_info;
    for (int i = 0; i < GD_SLOTS_CB && i < DXBC_MAX_CB; i++) {
        struct gd_resource *r = st->cb[i];
        if (!r)
            continue;
        uint32_t vec4 = (uint32_t)(r->size / 16), first = st->cb_first[i];
        if (first >= vec4)
            continue;
        e->cb[i] = (const float *)(r->data + (size_t)first * 16);
        e->cb_vec4[i] = st->cb_count[i] && st->cb_count[i] < vec4 - first ? st->cb_count[i] : vec4 - first;
    }
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

/* the vertex shader's inputs: from the input layout's elements, or vertex and instance numbers */
static void link_inputs(struct draw *d)
{
    struct gd_context *c = d->c;
    for (int i = 0; i < d->vs->nin; i++) {
        const struct dxbc_sig_elem *s = &d->vs->in[i];
        if (s->reg >= MAX_REGS)
            continue;
        struct vs_input *in = &d->in[s->reg];
        if (s->system_value == 6) {
            in->kind = IN_VERTEX_ID;
            in->mask = s->mask;
            continue;
        }
        if (s->system_value == 8) {
            in->kind = IN_INSTANCE_ID;
            in->mask = s->mask;
            continue;
        }
        if (!c->has_layout)
            continue;
        uint32_t offset = 0, last_slot = 0xffffffffu;
        for (uint32_t k = 0; k < c->layout.count; k++) {
            const struct gd_element *e = &c->layout.e[k];
            if (e->slot != last_slot)
                offset = 0, last_slot = e->slot;
            uint32_t at = e->offset == 0xffffffffu ? offset : e->offset, block;
            offset = at + gd_format_bytes(e->format, &block);
            if (!same_name(e->name, s->name) || e->index != s->index)
                continue;
            if (e->slot >= GD_SLOTS_VB || !c->vb[e->slot])
                break;
            struct gd_resource *r = c->vb[e->slot];
            size_t start = (size_t)c->vb_offset[e->slot] + at;
            if (start >= r->size)
                break;
            in->kind = IN_ELEMENT;
            in->format = e->format;
            in->mask = s->mask;
            in->base = r->data + start;
            in->limit = r->size - start;
            in->stride = c->vb_stride[e->slot];
            in->per_instance = e->per_instance != 0;
            in->step = e->step;
            break;
        }
    }
}

static void vs_run(const void *ctx, uint32_t first, int n, float *out, void *scratch)
{
    const struct draw *d = ctx;
    uint8_t *mem = scratch;
    uint32_t (*v)[4][4] = (uint32_t(*)[4][4])mem, (*o)[4][4] = (uint32_t(*)[4][4])(mem + sizeof(uint32_t[MAX_REGS][4][4]));
    memset(v, 0, sizeof(uint32_t[MAX_REGS][4][4]) * 2);
    for (int r = 0; r <= d->vs->max_in_reg && r < MAX_REGS; r++) {
        const struct vs_input *in = &d->in[r];
        for (int l = 0; l < n; l++) {
            uint32_t vertex = first + (uint32_t)l;
            switch (in->kind) {
            case IN_ELEMENT: {
                uint32_t index = in->per_instance ? d->first_instance + (in->step ? d->instance / in->step : 0) : vertex;
                size_t at = (size_t)index * in->stride;
                uint32_t block, bytes = gd_format_bytes(in->format, &block);
                if (at + bytes <= in->limit)
                    gd_texel_load(in->format, in->base + at, 0, 0, v[r][l]);
                break;
            }
            case IN_VERTEX_ID: v[r][l][0] = v[r][l][1] = v[r][l][2] = v[r][l][3] = vertex; break;
            case IN_INSTANCE_ID:
                v[r][l][0] = v[r][l][1] = v[r][l][2] = v[r][l][3] = d->first_instance + d->instance;
                break;
            default: break;
            }
        }
    }
    struct dxbc_lanes lanes = {.v = v, .o = o, .live = (1u << n) - 1, .scratch = mem + sizeof(uint32_t[MAX_REGS][4][4]) * 2};
    int floats = 4 + d->nvary * 4;
    if (!dxbc_run(d->vs, &d->vs_env, &lanes)) {
        note_fault(lanes.error);
        for (int l = 0; l < n; l++)
            memset(out + l * floats, 0, sizeof(float) * (size_t)floats);
        return;
    }
    for (int l = 0; l < n; l++) {
        float *dst = out + l * floats;
        if (d->pos_reg >= 0)
            memcpy(dst, o[d->pos_reg][l], 16);
        else
            memset(dst, 0, 16);
        for (int k = 0; k < d->nvary; k++)
            memcpy(dst + 4 + k * 4, o[k][l], 16);
    }
}

/* the pixel shader's inputs: each from the vertex shader output with the same semantic */
static bool link_varyings(struct draw *d, uint32_t *flat, uint32_t *noperspective)
{
    *flat = *noperspective = 0;
    if (!d->ps)
        return true;
    for (int i = 0; i < d->ps->nin; i++) {
        const struct dxbc_sig_elem *s = &d->ps->in[i];
        if (s->reg >= MAX_REGS)
            continue;
        struct ps_input *p = &d->psin[s->reg];
        if (s->system_value == 1) {
            p->kind = PSIN_POSITION;
            continue;
        }
        if (s->system_value == 9) {
            p->kind = PSIN_FRONT;
            continue;
        }
        if (s->system_value == 7 || s->system_value == 10) {
            p->kind = PSIN_ZERO;
            continue;
        }
        for (int k = 0; k < d->vs->nout; k++) {
            const struct dxbc_sig_elem *o = &d->vs->out[k];
            if (o->reg < MAX_REGS && (int)o->reg < d->nvary && same_name(o->name, s->name) && o->index == s->index) {
                p->kind = PSIN_VARY;
                p->from = (int)o->reg;
                int mode = d->ps->interp[s->reg];
                if (mode == 1)
                    *flat |= 1u << o->reg;
                else if (mode >= 4 && mode != 5)
                    *noperspective |= 1u << o->reg;
                break;
            }
        }
    }
    return true;
}

static float blend_factor(uint32_t f, const float *src, const float *src1, const float *dst, const float *k, int c)
{
    switch (f) {
    case 1: return 0;
    case 2: return 1;
    case 3: return src[c];
    case 4: return 1 - src[c];
    case 5: return src[3];
    case 6: return 1 - src[3];
    case 7: return dst[3];
    case 8: return 1 - dst[3];
    case 9: return dst[c];
    case 10: return 1 - dst[c];
    case 11: return c == 3 ? 1 : (src[3] < 1 - dst[3] ? src[3] : 1 - dst[3]);
    case 14: return k[c];
    case 15: return 1 - k[c];
    case 16: return src1[c];
    case 17: return 1 - src1[c];
    case 18: return src1[3];
    case 19: return 1 - src1[3];
    default: return 1;
    }
}

static float blend_op(uint32_t op, float s, float d)
{
    switch (op) {
    case 2: return s - d;
    case 3: return d - s;
    case 4: return s < d ? s : d;
    case 5: return s > d ? s : d;
    default: return s + d;
    }
}

static void write_color(const struct draw *d, const struct target *t, int x, int y, const uint32_t *src_raw,
                        const uint32_t *src1_raw)
{
    uint8_t *p = t->base + (size_t)y * t->pitch + (size_t)x * t->bytes;
    unsigned mask = t->blend.write_mask & 15;
    if (!mask)
        return;
    if (!t->blend.enable || t->cls != FC_FLOAT) {
        gd_texel_store(t->format, p, src_raw, mask);
        return;
    }
    float s[4], s1[4], dst[4];
    uint32_t draw_raw[4];
    gd_texel_load(t->format, p, 0, 0, draw_raw);
    bool unorm = t->format != GD_R16G16B16A16_FLOAT && t->format != GD_R32G32B32A32_FLOAT &&
                 t->format != GD_R11G11B10_FLOAT && t->format != GD_R16G16_FLOAT && t->format != GD_R32_FLOAT &&
                 t->format != GD_R16_FLOAT && t->format != GD_R32G32_FLOAT && t->format != GD_R32G32B32_FLOAT;
    for (int c = 0; c < 4; c++) {
        s[c] = F(src_raw[c]), s1[c] = src1_raw ? F(src1_raw[c]) : 0, dst[c] = F(draw_raw[c]);
        if (unorm)
            s[c] = fclamp(s[c], 0, 1), s1[c] = fclamp(s1[c], 0, 1);
    }
    uint32_t out[4];
    const struct gd_blend_target *b = &t->blend;
    for (int c = 0; c < 4; c++) {
        bool alpha = c == 3;
        float fs = blend_factor(alpha ? b->src_alpha : b->src, s, s1, dst, d->c->blend_factor, c);
        float fd = blend_factor(alpha ? b->dst_alpha : b->dst, s, s1, dst, d->c->blend_factor, c);
        uint32_t op = alpha ? b->op_alpha : b->op;
        float v = op == 4 || op == 5 ? blend_op(op, s[c], dst[c]) : blend_op(op, s[c] * fs, dst[c] * fd);
        out[c] = U(v);
    }
    gd_texel_store(t->format, p, out, mask);
}

static bool depth_compare(uint32_t func, float z, float have)
{
    switch (func) {
    case 1: return false;
    case 2: return z < have;
    case 3: return z == have;
    case 4: return z <= have;
    case 5: return z > have;
    case 6: return z != have;
    case 7: return z >= have;
    default: return true;
    }
}

static uint32_t stencil_apply(uint32_t op, uint32_t v, uint32_t ref)
{
    switch (op) {
    case 2: return 0;
    case 3: return ref;
    case 4: return v < 255 ? v + 1 : 255;
    case 5: return v > 0 ? v - 1 : 0;
    case 6: return ~v & 255;
    case 7: return (v + 1) & 255;
    case 8: return (v - 1) & 255;
    default: return v;
    }
}

/* the depth and stencil tests of one pixel; writes what passing or failing them changes */
static bool depth_stencil(const struct draw *d, int x, int y, float z, bool front)
{
    uint8_t *p = d->db + (size_t)y * d->dpitch + (size_t)x * d->dbytes;
    const struct gd_depth *ds = &d->c->depth;
    bool pass = true;
    if (d->stencil) {
        const struct gd_stencil_op *op = front ? &ds->front : &ds->back;
        uint32_t have = gd_stencil_read(d->dformat, p), ref = d->c->stencil_ref & 255, rm = ds->read_mask;
        bool spass = depth_compare(op->func, (float)(ref & rm), (float)(have & rm));
        bool zpass = !d->depth_test || depth_compare(ds->func, z, gd_depth_read(d->dformat, p));
        uint32_t which = !spass ? op->fail : !zpass ? op->depth_fail : op->pass;
        uint32_t nv = stencil_apply(which, have, ref), wm = ds->write_mask_stencil & 255;
        if (nv != have)
            gd_stencil_write(d->dformat, p, (have & ~wm) | (nv & wm));
        pass = spass && zpass;
    } else if (d->depth_test) {
        pass = depth_compare(ds->func, z, gd_depth_read(d->dformat, p));
    }
    if (pass && d->depth_write)
        gd_depth_write(d->dformat, p, z);
    return pass;
}

static float biased(const struct draw *d, const struct rs_quad *q, int p)
{
    float z = q->depth[p];
    if (d->bias != 0 || d->slope_bias != 0) {
        float dzx = q->depth[1] - q->depth[0], dzy = q->depth[2] - q->depth[0];
        dzx = dzx < 0 ? -dzx : dzx, dzy = dzy < 0 ? -dzy : dzy;
        float b = d->bias + d->slope_bias * (dzx > dzy ? dzx : dzy);
        if (d->bias_clamp > 0 && b > d->bias_clamp)
            b = d->bias_clamp;
        else if (d->bias_clamp < 0 && b < d->bias_clamp)
            b = d->bias_clamp;
        z += b;
    }
    return fclamp(z, d->min_z, d->max_z);
}

static void ps_quad(const void *ctx, const struct rs_quad *q, unsigned mask, void *scratch)
{
    const struct draw *d = ctx;
    bool ds = d->db && (d->depth_test || d->stencil || d->depth_write);
    if (d->early && ds) {
        for (int p = 0; p < 4; p++)
            if ((mask & (1u << p)) && !depth_stencil(d, q->x + (p & 1), q->y + (p >> 1), biased(d, q, p), q->front))
                mask &= ~(1u << p);
        if (!mask)
            return;
    }
    if (!d->ps) { /* depth only */
        uint64_t passed = 0;
        for (int p = 0; p < 4; p++)
            if ((mask & (1u << p)) &&
                (d->early || !ds || depth_stencil(d, q->x + (p & 1), q->y + (p >> 1), biased(d, q, p), q->front)))
                passed++;
        if (passed)
            __atomic_add_fetch(&stats.pixels, passed, __ATOMIC_RELAXED);
        return;
    }
    uint8_t *mem = scratch;
    uint32_t (*v)[4][4] = (uint32_t(*)[4][4])mem, (*o)[4][4] = (uint32_t(*)[4][4])(mem + sizeof(uint32_t[MAX_REGS][4][4]));
    memset(o, 0, sizeof(uint32_t[MAX_REGS][4][4]));
    for (int r = 0; r <= d->ps->max_in_reg && r < MAX_REGS; r++) {
        const struct ps_input *in = &d->psin[r];
        for (int l = 0; l < 4; l++) {
            uint32_t *dst = v[r][l];
            switch (in->kind) {
            case PSIN_VARY: memcpy(dst, &q->in[l][in->from * 4], 16); break;
            case PSIN_POSITION:
                dst[0] = U((float)(q->x + (l & 1)) + 0.5f), dst[1] = U((float)(q->y + (l >> 1)) + 0.5f);
                dst[2] = U(q->depth[l]), dst[3] = U(q->w[l] != 0 ? 1.0f / q->w[l] : 1.0f);
                break;
            case PSIN_FRONT: dst[0] = dst[1] = dst[2] = dst[3] = q->front ? 0xffffffffu : 0; break;
            default: dst[0] = dst[1] = dst[2] = dst[3] = 0; break;
            }
        }
    }
    struct dxbc_lanes lanes = {.v = v, .o = o, .live = 15, .scratch = mem + sizeof(uint32_t[MAX_REGS][4][4]) * 2};
    if (!dxbc_run(d->ps, &d->ps_env, &lanes)) {
        note_fault(lanes.error);
        return;
    }
    mask &= lanes.live;
    if (d->c->blend.alpha_to_coverage) /* one sample: coverage is all or nothing */
        for (int p = 0; p < 4; p++)
            if (F(o[0][p][3]) < 0.5f)
                mask &= ~(1u << p);
    uint64_t passed = 0; /* what occlusion queries count */
    for (int p = 0; p < 4; p++) {
        if (!(mask & (1u << p)))
            continue;
        int x = q->x + (p & 1), y = q->y + (p >> 1);
        if (!d->early && ds) {
            float z = d->ps->depth_out ? fclamp(lanes.depth[p], d->min_z, d->max_z) : biased(d, q, p);
            if (!depth_stencil(d, x, y, z, q->front))
                continue;
        }
        passed++;
        for (int t = 0; t < d->nrt; t++)
            if (d->rt[t].base && x < (int)d->rt[t].w && y < (int)d->rt[t].h)
                write_color(d, &d->rt[t], x, y, o[t][p], d->dual_source && t == 0 ? o[1][p] : NULL);
    }
    if (passed)
        __atomic_add_fetch(&stats.pixels, passed, __ATOMIC_RELAXED);
}

static bool layout_target(struct gd_view *v, struct target *t)
{
    struct gd_layout l;
    uint8_t *p = target_of(v, 0, &l);
    if (!p)
        return false;
    uint32_t block;
    t->base = p, t->pitch = l.row_pitch, t->format = v->format, t->bytes = gd_format_bytes(v->format, &block);
    t->w = l.width, t->h = l.height, t->cls = gd_format_class(v->format);
    return t->bytes && block == 1;
}

static bool dual_source(const struct gd_blend_target *b)
{
    uint32_t f[4] = {b->src, b->dst, b->src_alpha, b->dst_alpha};
    for (int i = 0; i < 4; i++)
        if (f[i] >= 16 && f[i] <= 19)
            return true;
    return false;
}

void gd_draw(struct gd_context *c, bool indexed, uint32_t count, uint32_t start, int32_t base_vertex,
             uint32_t instances, uint32_t start_instance)
{
    stats.draws++;
    struct gd_shader *vs = c->stage[DXBC_VS].shader, *ps = c->stage[DXBC_PS].shader;
    enum rs_topology topo;
    if (c->topology == 4)
        topo = RS_TRIANGLES;
    else if (c->topology == 5)
        topo = RS_STRIP;
    else {
        stats.skipped++; /* points, lines, adjacency and patches: not yet */
        return;
    }
    if (!vs || vs->sh.stage != DXBC_VS || !count || !instances || c->raster.fill == 2 ||
        c->stage[DXBC_GS].shader || c->stage[DXBC_HS].shader) {
        stats.skipped++;
        return;
    }
    struct draw *d = kzalloc(sizeof(*d));
    if (!d)
        return;
    d->c = c;
    d->vs = &vs->sh;
    d->ps = ps && ps->sh.stage == DXBC_PS ? &ps->sh : NULL;
    d->pos_reg = -1;
    for (int i = 0; i < d->vs->nout; i++)
        if (d->vs->out[i].system_value == 1 && d->vs->out[i].reg < MAX_REGS)
            d->pos_reg = (int)d->vs->out[i].reg;
    d->nvary = d->vs->max_out_reg + 1;
    for (int i = 0; i < d->vs->nout; i++)
        if ((int)d->vs->out[i].reg + 1 > d->nvary)
            d->nvary = (int)d->vs->out[i].reg + 1;
    if (d->nvary > RS_MAX_VARY)
        d->nvary = RS_MAX_VARY;
    if (d->nvary < 0)
        d->nvary = 0;
    link_inputs(d);
    uint32_t flat, nopersp;
    link_varyings(d, &flat, &nopersp);
    make_env(&d->vs_env, &c->stage[DXBC_VS]);
    make_env(&d->ps_env, &c->stage[DXBC_PS]);
    /* the targets: they decide the size of the drawing */
    int w = 0, h = 0;
    for (int i = 0; i < 8; i++) {
        if (!c->rtv[i] || !layout_target(c->rtv[i], &d->rt[i]))
            continue;
        d->rt[i].blend = c->blend.rt[c->blend.independent ? i : 0];
        d->nrt = i + 1;
        if (!w || (int)d->rt[i].w < w)
            w = (int)d->rt[i].w;
        if (!h || (int)d->rt[i].h < h)
            h = (int)d->rt[i].h;
    }
    d->dual_source = d->nrt > 0 && d->rt[0].blend.enable && dual_source(&d->rt[0].blend);
    if (c->dsv && gd_format_depth(c->dsv->r->d.format)) {
        struct gd_layout l;
        uint8_t *p = target_of(c->dsv, 0, &l);
        if (p) {
            uint32_t block;
            d->db = p, d->dpitch = l.row_pitch, d->dformat = c->dsv->r->d.format;
            d->dbytes = gd_format_bytes(d->dformat, &block);
            if (!w || (int)l.width < w)
                w = (int)l.width;
            if (!h || (int)l.height < h)
                h = (int)l.height;
            const struct gd_depth *ds = &c->depth;
            d->depth_test = ds->depth_enable != 0;
            d->depth_write = ds->depth_enable && ds->write_mask && !(c->dsv->d.flags & 1);
            d->stencil = ds->stencil_enable && (d->dformat == GD_D24_UNORM_S8_UINT || d->dformat == GD_D32_FLOAT_S8X24_UINT);
            float unit = d->dformat == GD_D16_UNORM ? 1.0f / 65535.0f : d->dformat == GD_D24_UNORM_S8_UINT ? 1.0f / 16777215.0f
                                                                                                        : 1.0f / 8388608.0f;
            d->bias = (float)c->raster.depth_bias * unit;
            d->slope_bias = c->raster.slope_scaled_depth_bias;
            d->bias_clamp = c->raster.depth_bias_clamp;
        }
    }
    if (!w || !h) {
        stats.skipped++;
        kfree(d);
        return;
    }
    d->early = !d->ps || (!d->ps->discards && !d->ps->depth_out && !c->blend.alpha_to_coverage);
    struct rs_state s = {0};
    s.vs = vs_run;
    s.quad = ps_quad;
    s.ctx = d;
    s.nvary = d->nvary;
    s.flat = flat;
    s.noperspective = nopersp;
    size_t regs = sizeof(uint32_t[MAX_REGS][4][4]) * 2;
    size_t a = dxbc_scratch(d->vs), b = d->ps ? dxbc_scratch(d->ps) : 0;
    s.scratch = regs + (a > b ? a : b);
    s.cull = c->raster.cull == 2 ? RS_CULL_FRONT : c->raster.cull == 3 ? RS_CULL_BACK : RS_CULL_NONE;
    s.front_ccw = c->raster.front_ccw != 0;
    if (c->nviewports) {
        s.vx = c->viewport.x, s.vy = c->viewport.y, s.vw = c->viewport.w, s.vh = c->viewport.h;
        s.min_z = c->viewport.min_z, s.max_z = c->viewport.max_z;
        if (s.min_z == 0 && s.max_z == 0)
            s.max_z = 1e-30f; /* a viewport that squashes depth to 0 (the rasterizer reads 0..0 as unset) */
    } else {
        s.vw = (float)w, s.vh = (float)h, s.max_z = 1;
    }
    if (s.vw <= 0 || s.vh <= 0) {
        stats.skipped++;
        kfree(d);
        return;
    }
    d->min_z = s.min_z < s.max_z ? s.min_z : s.max_z;
    d->max_z = s.min_z < s.max_z ? s.max_z : s.min_z;
    if (c->raster.scissor) {
        if (!c->nscissors) {
            kfree(d);
            return;
        }
        s.scissor = true;
        s.sx = c->scissor.left, s.sy = c->scissor.top;
        s.sw = c->scissor.right - c->scissor.left, s.sh = c->scissor.bottom - c->scissor.top;
    }
    struct rs_target t = {NULL, 0, NULL, 0, w, h};
    /* the indices, as 32-bit numbers; strips are cut at the restart index */
    uint32_t *idx = NULL;
    if (indexed) {
        if (!c->ib) {
            stats.skipped++;
            kfree(d);
            return;
        }
        bool wide = c->ib_format == GD_R32_UINT;
        size_t isize = wide ? 4 : 2, at = (size_t)c->ib_offset + (size_t)start * isize;
        if (at >= c->ib->size) {
            kfree(d);
            return;
        }
        size_t avail = (c->ib->size - at) / isize;
        if (count > avail)
            count = (uint32_t)avail;
        idx = kmalloc(sizeof(uint32_t) * (count ? count : 1));
        if (!idx) {
            kfree(d);
            return;
        }
        const uint8_t *src = c->ib->data + at;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v;
            if (wide)
                memcpy(&v, src + i * 4, 4);
            else
                v = (uint32_t)src[i * 2] | (uint32_t)src[i * 2 + 1] << 8, v = v == 0xffff ? 0xffffffffu : v;
            idx[i] = v;
        }
    }
    d->first_instance = start_instance;
    for (uint32_t inst = 0; inst < instances; inst++) {
        d->instance = inst;
        if (!idx) {
            int n = rs_draw(&t, &s, topo, count, NULL, (int32_t)start);
            stats.triangles += n > 0 ? (uint64_t)n : 0;
            continue;
        }
        uint32_t from = 0;
        for (uint32_t i = 0; i <= count; i++) {
            bool cut = i == count || idx[i] == 0xffffffffu;
            if (!cut)
                continue;
            if (topo == RS_TRIANGLES && i < count) { /* a list has no restart: the index stands for itself */
                continue;
            }
            if (i - from >= 3) {
                int n = rs_draw(&t, &s, topo, i - from, idx + from, base_vertex);
                stats.triangles += n > 0 ? (uint64_t)n : 0;
            }
            from = i + 1;
        }
    }
    kfree(idx);
    kfree(d);
}

/* ---- presenting: a swap chain's buffer into a window */

struct present {
    const uint8_t *src;
    struct gd_layout l;
    uint32_t format;
    uint32_t *dst;
    int stride, dx, dy, dw, dh, x0, x1;
    int y0;
};

static uint32_t to8(float v)
{
    v = v != v ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (uint32_t)(v * 255.0f + 0.5f);
}

static void present_rows(void *ctx, int from, int to)
{
    const struct present *p = ctx;
    for (int y = p->y0 + from; y < p->y0 + to; y++) {
        uint32_t sy = (uint32_t)(((int64_t)(y - p->dy) * p->l.height) / p->dh);
        const uint8_t *row = p->src + (size_t)sy * p->l.row_pitch;
        uint32_t *out = p->dst + (size_t)y * (size_t)p->stride;
        for (int x = p->x0; x < p->x1; x++) {
            uint32_t sx = (uint32_t)(((int64_t)(x - p->dx) * p->l.width) / p->dw);
            uint32_t c;
            if (p->format == GD_B8G8R8A8_UNORM || p->format == GD_B8G8R8X8_UNORM ||
                p->format == GD_B8G8R8A8_UNORM_SRGB || p->format == GD_B8G8R8X8_UNORM_SRGB) {
                memcpy(&c, row + sx * 4, 4);
            } else if (p->format == GD_R8G8B8A8_UNORM || p->format == GD_R8G8B8A8_UNORM_SRGB) {
                const uint8_t *t = row + sx * 4;
                c = (uint32_t)t[0] << 16 | (uint32_t)t[1] << 8 | t[2];
            } else {
                uint32_t v[4];
                gd_texel_load(p->format, row + sx * gd_format_bytes(p->format, &(uint32_t){0}), 0, 0, v);
                float f[3];
                memcpy(f, v, sizeof(f));
                c = to8(f[0]) << 16 | to8(f[1]) << 8 | to8(f[2]);
            }
            out[x] = 0xff000000u | (c & 0xffffffu);
        }
    }
}

void gd_present(struct gd_resource *r, uint32_t sub, uint32_t *dst, int stride, int dx, int dy, int dw, int dh,
                int cx, int cy, int cw, int ch)
{
    struct present p = {.dst = dst, .stride = stride, .dx = dx, .dy = dy, .dw = dw, .dh = dh};
    if (!r || r->d.dim != GD_TEX2D || dw <= 0 || dh <= 0 || !(p.src = gd_subresource(r, sub, &p.l)))
        return;
    p.format = gd_format_typed(r->d.format);
    if (gd_format_class(p.format) != FC_FLOAT || gd_format_depth(p.format))
        return;
    int y1 = MIN(dy + dh, cy + ch);
    p.x0 = MAX(dx, cx), p.x1 = MIN(dx + dw, cx + cw), p.y0 = MAX(dy, cy);
    if (p.x0 >= p.x1 || p.y0 >= y1)
        return;
    hal_parallel(present_rows, &p, y1 - p.y0, 16);
}

#endif
