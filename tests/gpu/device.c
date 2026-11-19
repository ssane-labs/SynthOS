/* The graphics device (include/synth/gd.h) on the build machine: formats, block compression, sampling, and draws
   with hand-assembled shaders through input layouts, depth, stencil, blending, indices and instances. The kernel
   builds the same file into the "gdtest" command (kernel/gpu/gdtest_fp.c), so the draws also run on the host's
   graphics card through virgl. */

#ifdef RS_HOST
#include <stdio.h>
#include <string.h>
#endif

#include <synth/gd.h>
#include <synth/tess.h>

#include "../../kernel/gpu/gd_internal.h"
#include "asm.h"
#include "bc_cases.h"

int device_tests(void);

static int failures;

static void expect(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

static int near(float a, float b, float tol)
{
    float d = a - b;
    return d <= tol && d >= -tol;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float bitsf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int roundtrip(uint32_t format, const float in[4], float tol, unsigned comps)
{
    uint8_t px[16] = {0};
    uint32_t raw[4] = {fbits(in[0]), fbits(in[1]), fbits(in[2]), fbits(in[3])}, back[4];
    gd_texel_store(format, px, raw, 15);
    gd_texel_load(format, px, 0, 0, back);
    for (unsigned c = 0; c < 4; c++)
        if ((comps & (1u << c)) && !near(bitsf(back[c]), in[c], tol))
            return 0;
    return 1;
}

static void format_tests(void)
{
    const float a[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    expect(roundtrip(GD_R8G8B8A8_UNORM, a, 0.003f, 15), "R8G8B8A8_UNORM stores and loads");
    expect(roundtrip(GD_B8G8R8A8_UNORM, a, 0.003f, 15), "B8G8R8A8_UNORM keeps the channels in place");
    expect(roundtrip(GD_R8G8B8A8_UNORM_SRGB, a, 0.01f, 15), "sRGB targets convert both ways");
    expect(roundtrip(GD_R16G16B16A16_FLOAT, (const float[4]){-2.5f, 1000, 0.001f, 1}, 0.01f, 15), "half floats");
    expect(roundtrip(GD_R11G11B10_FLOAT, (const float[4]){0.5f, 3.25f, 100, 1}, 0.4f, 7), "R11G11B10 floats");
    expect(roundtrip(GD_R10G10B10A2_UNORM, a, 0.002f, 7), "R10G10B10A2");
    expect(roundtrip(GD_B5G6R5_UNORM, (const float[4]){1, 0.5f, 0, 1}, 0.04f, 7), "B5G6R5");
    expect(roundtrip(GD_R16G16_SNORM, (const float[4]){-0.5f, 0.25f, 0, 1}, 0.001f, 3), "R16G16 snorm");
    uint8_t px[4];
    gd_depth_write(GD_D24_UNORM_S8_UINT, (uint8_t *)memset(px, 0, 4), 0.5f);
    gd_stencil_write(GD_D24_UNORM_S8_UINT, px, 7);
    expect(near(gd_depth_read(GD_D24_UNORM_S8_UINT, px), 0.5f, 1e-6f) && gd_stencil_read(GD_D24_UNORM_S8_UINT, px) == 7,
           "D24S8 depth and stencil");
    int bad = 0;
    for (size_t k = 0; k < sizeof(bc_cases) / sizeof(bc_cases[0]); k++) {
        const struct bc_case *b = &bc_cases[k];
        for (int i = 0; i < 16; i++) {
            float o[4];
            gd_bc_texel(b->format, b->block, i % 4, i / 4, o);
            for (int c = 0; c < b->chans; c++) {
                float v = o[c] < 0 ? 0 : o[c] > 1 ? 1 : o[c];
                if (!near(v * 255.0f, b->px[i][c], 1.6f))
                    bad++;
            }
        }
    }
    expect(!bad, "BC1, BC3, BC4, BC5, BC6H and BC7 decode as Pillow does");
}

/* ---- textures and sampling */

static struct gd_resource *texture(uint32_t w, uint32_t h, uint32_t mips, const uint32_t *pixels)
{
    struct gd_res_desc d = {.dim = GD_TEX2D, .width = w, .height = h, .mips = mips, .array = 1,
                            .format = GD_R8G8B8A8_UNORM, .bind = GD_BIND_SHADER_RESOURCE};
    struct gd_init init[8] = {{(uint64_t)(uintptr_t)pixels, w * 4, w * h * 4}};
    uint32_t err;
    return gd_resource_create(&d, init, &err);
}

static void sample_tests(void)
{
    /* 2x2: red green / blue white */
    static const uint32_t px[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    struct gd_resource *r = texture(2, 2, 2, px);
    struct gd_view_desc vd = {.kind = GD_VIEW_SRV};
    uint32_t err;
    struct gd_view *v = gd_view_create(r, &vd, &err);
    expect(r && v, "a texture and its view");
    if (!v)
        return;
    struct gd_sampler point = {.filter = 0, .address_u = 3, .address_v = 3, .address_w = 3, .max_lod = 100};
    struct gd_sampler linear = point;
    linear.filter = 0x15;
    const float zero[4] = {0}, z3[3] = {0};
    const int off[3] = {0};
    float o[4];
    (void)z3;
    gd_sample(v, &point, (const float[4]){0.75f, 0.25f, 0, 0}, zero, zero, 0, 0, off, NULL, o);
    expect(near(o[0], 0, 0.01f) && near(o[1], 1, 0.01f), "point sampling picks the texel");
    gd_sample(v, &linear, (const float[4]){0.5f, 0.5f, 0, 0}, zero, zero, 0, 0, off, NULL, o);
    expect(near(o[0], 0.5f, 0.01f) && near(o[1], 0.5f, 0.01f) && near(o[2], 0.5f, 0.01f), "bilinear in the middle");
    struct gd_sampler wrap = point;
    wrap.address_u = 1;
    gd_sample(v, &wrap, (const float[4]){1.25f, 0.25f, 0, 0}, zero, zero, 0, 0, off, NULL, o);
    expect(near(o[0], 1, 0.01f) && near(o[1], 0, 0.01f), "wrap");
    struct gd_sampler border = point;
    border.address_u = 4, border.border[0] = 0.25f, border.border[3] = 1;
    gd_sample(v, &border, (const float[4]){1.5f, 0.25f, 0, 0}, zero, zero, 0, 0, off, NULL, o);
    expect(near(o[0], 0.25f, 0.001f), "border color outside");
    /* mip 1 is zeros: lod 1 reads it, derivatives of a whole texture per pixel pick it too */
    gd_sample(v, &point, (const float[4]){0.25f, 0.25f, 0, 0}, zero, zero, 1, 0, off, NULL, o);
    expect(near(o[0], 0, 0.01f) && near(o[3], 0, 0.01f), "an explicit level of detail");
    gd_sample(v, &point, (const float[4]){0.25f, 0.25f, 0, 0}, (const float[4]){1, 0, 0, 0},
              (const float[4]){0, 1, 0, 0}, -1e31f, 0, off, NULL, o);
    expect(near(o[3], 0, 0.01f), "the level from the derivatives");
    uint32_t info[4];
    gd_view_size(v, 0, info);
    expect(info[0] == 2 && info[1] == 2 && info[3] == 2, "resinfo: size and levels");
    int32_t at[4] = {1, 1, 0, 0};
    uint32_t t[4];
    gd_texel_fetch(v, at, off, t);
    expect(near(bitsf(t[2]), 1, 0.01f) && near(bitsf(t[0]), 1, 0.01f), "ld reads the texel");
    gd_view_unref(v);
    gd_resource_unref(r);
}

/* ---- draws */

#define W 64
#define H 64

struct vert {
    float p[3], c[4];
};

static struct gd_context *ctx;
static struct gd_resource *rt_tex, *ds_tex;
static struct gd_view *rtv, *dsv;

static uint32_t pixel(int x, int y)
{
    struct gd_layout l;
    uint8_t *p = gd_subresource(rt_tex, 0, &l);
    uint32_t v;
    memcpy(&v, p + (size_t)y * l.row_pitch + (size_t)x * 4, 4);
    return v & 0xffffff; /* 0xBBGGRR */
}

static int close_rgb(uint32_t v, uint32_t want)
{
    for (int i = 0; i < 3; i++) {
        int a = (int)(v >> (8 * i) & 255), b = (int)(want >> (8 * i) & 255);
        if (a - b > 2 || b - a > 2)
            return 0;
    }
    return 1;
}

static struct gd_resource *buffer(const void *data, uint32_t size, uint32_t bind)
{
    struct gd_res_desc d = {.dim = GD_BUFFER, .width = size, .bind = bind};
    struct gd_init init = {(uint64_t)(uintptr_t)data, 0, 0};
    uint32_t err;
    return gd_resource_create(&d, &init, &err);
}

/* a rectangle in clip space as two clockwise triangles */
static void quad(struct vert *v, float x0, float y0, float x1, float y1, float z, const float c[4])
{
    const float p[6][2] = {{x0, y1}, {x1, y1}, {x1, y0}, {x0, y1}, {x1, y0}, {x0, y0}};
    for (int i = 0; i < 6; i++) {
        v[i].p[0] = p[i][0], v[i].p[1] = p[i][1], v[i].p[2] = z;
        memcpy(v[i].c, c, 16);
    }
}

static struct gd_shader *vs_color, *ps_color, *ps_discard, *vs_inst;

static void make_shaders(void)
{
    char err[64];
    size_t size;
    /* vs: position and color through */
    in_sig("POSITION", 0, 0, 0, 7), in_sig("COLOR", 0, 0, 1, 15);
    out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
    op(54, 0), dst(OUT, 0, 7), src(IN, 0, XYZW), end();
    op(54, 0), dst(OUT, 0, 8), immf(1), end();
    op(54, 0), dst(OUT, 1, 15), src(IN, 1, XYZW), end();
    op(62, 0), end();
    size = container(1, 1);
    vs_color = gd_shader_create(blob, (uint32_t)size, err);
    /* vs with a per-instance offset */
    in_sig("POSITION", 0, 0, 0, 7), in_sig("COLOR", 0, 0, 1, 15), in_sig("OFFSET", 0, 0, 2, 3);
    out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
    op(0, 0), dst(OUT, 0, 3), src(IN, 0, XYZW), src(IN, 2, XYZW), end();
    op(54, 0), dst(OUT, 0, 4), src(IN, 0, XYZW), end();
    op(54, 0), dst(OUT, 0, 8), immf(1), end();
    op(54, 0), dst(OUT, 1, 15), src(IN, 1, XYZW), end();
    op(62, 0), end();
    size = container(1, 1);
    vs_inst = gd_shader_create(blob, (uint32_t)size, err);
    /* ps: the color */
    in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
    out_sig("SV_Target", 0, 0, 0, 15);
    op(54, 0), dst(OUT, 0, 15), src(IN, 1, XYZW), end();
    op(62, 0), end();
    size = container(0, 1);
    ps_color = gd_shader_create(blob, (uint32_t)size, err);
    /* ps: the color, but pixels left of x = 32 discarded */
    in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
    out_sig("SV_Target", 0, 0, 0, 15);
    op(49, 0), dst(TEMP, 0, 1), sel(IN, 0, 0), immf(32), end();
    op(13, 1 << 18), sel(TEMP, 0, 0), end();
    op(54, 0), dst(OUT, 0, 15), src(IN, 1, XYZW), end();
    op(62, 0), end();
    size = container(0, 1);
    ps_discard = gd_shader_create(blob, (uint32_t)size, err);
}

static void immv(uint32_t a, uint32_t b, uint32_t c, uint32_t d) /* a vector of four integers */
{
    code[n++] = 2u | (uint32_t)IMM << 12;
    code[n++] = a, code[n++] = b, code[n++] = c, code[n++] = d;
}

static void draw_verts(const struct vert *v, uint32_t count)
{
    struct gd_resource *vb = buffer(v, (uint32_t)(sizeof(struct vert) * count), GD_BIND_VERTEX);
    gd_set_vertex_buffer(ctx, 0, vb, sizeof(struct vert), 0);
    gd_draw(ctx, false, count, 0, 0, 1, 0);
    gd_set_vertex_buffer(ctx, 0, NULL, 0, 0);
    gd_resource_unref(vb);
}

static void draw_tests(void)
{
    uint32_t err;
    struct gd_res_desc rd = {.dim = GD_TEX2D, .width = W, .height = H, .mips = 1, .array = 1,
                             .format = GD_R8G8B8A8_UNORM, .bind = GD_BIND_RENDER_TARGET};
    rt_tex = gd_resource_create(&rd, NULL, &err);
    /* typeless with a depth view, as a depth texture that shaders may also read */
    rd.format = GD_R24G8_TYPELESS, rd.bind = GD_BIND_DEPTH_STENCIL | GD_BIND_SHADER_RESOURCE;
    ds_tex = gd_resource_create(&rd, NULL, &err);
    rtv = gd_view_create(rt_tex, &(struct gd_view_desc){.kind = GD_VIEW_RTV}, &err);
    dsv = gd_view_create(ds_tex, &(struct gd_view_desc){.kind = GD_VIEW_DSV, .format = GD_D24_UNORM_S8_UINT}, &err);
    ctx = gd_context_create();
    make_shaders();
    expect(vs_color && ps_color && ps_discard && vs_inst && rtv && dsv, "targets and shaders");
    if (!vs_color || !ps_color || !rtv || !dsv)
        return;
    struct gd_input_layout il = {2, {{"POSITION", 0, GD_R32G32B32_FLOAT, 0, 0, 0, 0},
                                     {"COLOR", 0, GD_R32G32B32A32_FLOAT, 0, 12, 0, 0}}};
    gd_set_input_layout(ctx, &il);
    gd_set_topology(ctx, 4);
    gd_set_shader(ctx, 1, vs_color);
    gd_set_shader(ctx, 0, ps_color);
    gd_set_render_target(ctx, 0, rtv);
    gd_set_depth_target(ctx, dsv);
    gd_set_viewports(ctx, 1, &(struct gd_viewport){0, 0, W, H, 0, 1});
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    gd_clear_dsv(dsv, 3, 1.0f, 0);
    expect(pixel(5, 5) == 0, "cleared to black");

    static const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, half[4] = {1, 1, 1, 0.5f};
    struct vert v[12];
    quad(v, -1, -1, 1, 1, 0.5f, red);
    draw_verts(v, 6);
    expect(pixel(0, 0) == 0x0000ff && pixel(63, 63) == 0x0000ff && pixel(31, 40) == 0x0000ff, "a quad fills the target");
    quad(v, -1, -1, 1, 1, 0.7f, green);
    draw_verts(v, 6);
    expect(pixel(20, 20) == 0x0000ff, "the depth test hides what is behind");
    quad(v, -1, -1, 0, 1, 0.3f, blue);
    draw_verts(v, 6);
    expect(pixel(10, 20) == 0xff0000 && pixel(50, 20) == 0x0000ff, "and shows what is in front, half the screen");

    /* culling: the same quad wound the other way is a back face */
    struct vert back[6];
    quad(back, -1, -1, 1, 1, 0.1f, green);
    for (int i = 0; i < 6; i += 3) {
        struct vert t = back[i + 1];
        back[i + 1] = back[i + 2], back[i + 2] = t;
    }
    draw_verts(back, 6);
    expect(pixel(10, 20) == 0xff0000, "back faces are culled");

    /* blending */
    struct gd_blend b;
    memset(&b, 0, sizeof(b));
    b.rt[0] = (struct gd_blend_target){1, 5, 6, 1, 2, 1, 1, 15};
    gd_set_blend(ctx, &b, NULL, 0xffffffffu);
    gd_set_depth(ctx, &(struct gd_depth){.depth_enable = 0}, 0);
    quad(v, -1, -1, 1, 1, 0.0f, half);
    draw_verts(v, 6);
    expect(close_rgb(pixel(10, 20), 0xff8080) && close_rgb(pixel(50, 20), 0x8080ff), "alpha blending");
    gd_set_blend(ctx, NULL, NULL, 0xffffffffu);

    /* copies between two textures (on the host's card when there is one): the whole, then the left half moved right */
    struct gd_res_desc cd = {.dim = GD_TEX2D, .width = W, .height = H, .mips = 1, .array = 1,
                             .format = GD_R8G8B8A8_UNORM, .bind = GD_BIND_RENDER_TARGET | GD_BIND_SHADER_RESOURCE};
    struct gd_resource *copy = gd_resource_create(&cd, NULL, &err);
    if (copy) {
        gd_copy_resource(copy, rt_tex);
        gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
        gd_copy_region(rt_tex, 0, W / 2, 0, 0, copy, 0, &(struct gd_box){0, 0, 0, W / 2, H, 1});
        expect(pixel(10, 20) == 0 && close_rgb(pixel(W / 2 + 10, 20), 0xff8080), "texture copies");
        /* UpdateSubresource of a box in what the host wrote keeps the rest */
        static const uint32_t blue4[16] = {0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u,
                                           0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u,
                                           0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u,
                                           0xffff0000u};
        gd_update(rt_tex, 0, &(struct gd_box){2, 2, 0, 6, 6, 1}, blue4, 16, 64);
        expect(pixel(3, 3) == 0xff0000 && pixel(1, 1) == 0 && close_rgb(pixel(W / 2 + 10, 20), 0xff8080),
               "a box updated in a drawn texture");
        gd_resource_unref(copy);
    }

    /* stencil: mark the top half, then draw only where marked */
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    struct gd_depth sd = {.depth_enable = 0, .stencil_enable = 1, .read_mask = 255, .write_mask_stencil = 255,
                          .front = {1, 1, 3, 8}, .back = {1, 1, 3, 8}};
    gd_set_depth(ctx, &sd, 1);
    quad(v, -1, 0, 1, 1, 0.5f, red);
    draw_verts(v, 6);
    sd.front = sd.back = (struct gd_stencil_op){1, 1, 1, 3};
    gd_set_depth(ctx, &sd, 1);
    quad(v, -1, -1, 1, 1, 0.5f, green);
    draw_verts(v, 6);
    expect(pixel(30, 10) == 0x00ff00 && pixel(30, 50) == 0, "the stencil test");
    gd_set_depth(ctx, NULL, 0);

    /* indices: two quads in one triangle strip, cut by the restart index */
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    gd_clear_dsv(dsv, 1, 1.0f, 0);
    struct vert sv[8];
    const float sp[8][2] = {{-1, 1}, {-0.5f, 1}, {-1, 0}, {-0.5f, 0}, {0.5f, 0}, {1, 0}, {0.5f, -1}, {1, -1}};
    for (int i = 0; i < 8; i++) {
        sv[i].p[0] = sp[i][0], sv[i].p[1] = sp[i][1], sv[i].p[2] = 0.5f;
        memcpy(sv[i].c, blue, 16);
    }
    static const uint16_t ix[9] = {0, 1, 2, 3, 0xffff, 4, 5, 6, 7};
    struct gd_resource *vb = buffer(sv, sizeof(sv), GD_BIND_VERTEX), *ib = buffer(ix, sizeof(ix), GD_BIND_INDEX);
    gd_set_vertex_buffer(ctx, 0, vb, sizeof(struct vert), 0);
    gd_set_index_buffer(ctx, ib, GD_R16_UINT, 0);
    gd_set_topology(ctx, 5);
    gd_draw(ctx, true, 9, 0, 0, 1, 0);
    expect(pixel(4, 4) == 0xff0000 && pixel(60, 60) == 0xff0000 && pixel(32, 32) == 0 && pixel(60, 4) == 0,
           "an indexed strip with a restart");
    gd_set_topology(ctx, 4);
    gd_resource_unref(vb);
    gd_resource_unref(ib);

    /* instances: one small quad, drawn three times at offsets from a second buffer */
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    gd_clear_dsv(dsv, 1, 1.0f, 0);
    struct gd_input_layout il2 = {3, {{"POSITION", 0, GD_R32G32B32_FLOAT, 0, 0, 0, 0},
                                      {"COLOR", 0, GD_R32G32B32A32_FLOAT, 0, 12, 0, 0},
                                      {"OFFSET", 0, GD_R32G32_FLOAT, 1, 0, 1, 1}}};
    gd_set_input_layout(ctx, &il2);
    gd_set_shader(ctx, 1, vs_inst);
    static const float offs[3][2] = {{0, 0}, {1, 0}, {1.5f, -1.5f}};
    quad(v, -1, 0.5f, -0.5f, 1, 0.5f, green);
    vb = buffer(v, sizeof(struct vert) * 6, GD_BIND_VERTEX);
    struct gd_resource *ob = buffer(offs, sizeof(offs), GD_BIND_VERTEX);
    gd_set_vertex_buffer(ctx, 0, vb, sizeof(struct vert), 0);
    gd_set_vertex_buffer(ctx, 1, ob, 8, 0);
    gd_draw(ctx, false, 6, 0, 0, 3, 0);
    expect(pixel(4, 4) == 0x00ff00 && pixel(36, 4) == 0x00ff00 && pixel(52, 52) == 0x00ff00 && pixel(20, 4) == 0,
           "instanced drawing with a per-instance element");
    gd_set_vertex_buffer(ctx, 1, NULL, 0, 0);
    gd_resource_unref(vb);
    gd_resource_unref(ob);
    gd_set_input_layout(ctx, &il);
    gd_set_shader(ctx, 1, vs_color);

    /* a shader that discards: depth is tested after it, and only the kept pixels write depth */
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    gd_clear_dsv(dsv, 1, 1.0f, 0);
    gd_set_shader(ctx, 0, ps_discard);
    quad(v, -1, -1, 1, 1, 0.2f, red);
    draw_verts(v, 6);
    gd_set_shader(ctx, 0, ps_color);
    quad(v, -1, -1, 1, 1, 0.5f, green);
    draw_verts(v, 6);
    expect(pixel(10, 10) == 0x00ff00 && pixel(50, 10) == 0x0000ff, "discard keeps depth unwritten");

    /* lines, points and wireframe: one pixel wide, both sides drawn */
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    gd_set_depth(ctx, &(struct gd_depth){0}, 0);
#define PX(x) (((x) + 0.5f) / W * 2 - 1)
#define PY(y) (1 - ((y) + 0.5f) / H * 2)
    struct vert lv[4] = {{{PX(2), PY(10), 0.5f}, {0, 1, 0, 1}}, {{PX(60), PY(10), 0.5f}, {0, 1, 0, 1}},
                         {{PX(20), PY(20), 0.5f}, {0, 1, 0, 1}}, {{PX(20), PY(50), 0.5f}, {0, 1, 0, 1}}};
    gd_set_topology(ctx, 2);
    draw_verts(lv, 4);
    expect(pixel(30, 10) == 0x00ff00 && pixel(30, 12) == 0 && pixel(30, 8) == 0 && pixel(20, 35) == 0x00ff00 &&
               pixel(22, 35) == 0,
           "line lists are one pixel wide");
    struct vert pv[1] = {{{PX(5), PY(40), 0.5f}, {1, 0, 0, 1}}};
    gd_set_topology(ctx, 1);
    draw_verts(pv, 1);
    expect(pixel(5, 40) == 0x0000ff && pixel(6, 40) == 0 && pixel(5, 41) == 0, "a point covers one pixel");
    gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
    gd_set_topology(ctx, 4);
    gd_set_raster(ctx, &(struct gd_raster){.fill = 2, .cull = 1, .depth_clip = 1});
    quad(v, PX(10), PY(50), PX(50), PY(10), 0.5f, green);
    draw_verts(v, 6);
    expect(pixel(30, 10) == 0x00ff00 && pixel(10, 30) == 0x00ff00 && pixel(20, 30) == 0, "wireframe draws edges only");
    gd_set_raster(ctx, NULL);

    /* a texture through a sampler, scaled by a constant buffer's second constant: the left texel red, the right
       blue, with point and then linear filtering */
    {
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Target", 0, 0, 0, 15);
        op(88, 3 << 11), code[n++] = 7u << 12 | 1u << 20, code[n++] = 0, code[n++] = 0x5555, end(); /* t0 float */
        op(90, 0), code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();                          /* s0 */
        op(89, 0), code[n++] = 2u | 1u << 2 | XYZW << 4 | 8u << 12 | 2u << 20, code[n++] = 0, code[n++] = 2,
            end(); /* dcl_constantbuffer cb0[2] */
        op(69, 0), dst(TEMP, 0, 15), src(IN, 1, SW(0, 1, 0, 0)), src(RES, 0, XYZW);
        code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end(); /* sample r0, v1.xyxx, t0, s0 */
        op(56, 0), dst(OUT, 0, 15), src(TEMP, 0, XYZW), cbsrc(0, 1, XYZW), end();
        op(62, 0), end();
        size_t size = container(0, 1);
        char e[64];
        struct gd_shader *ps_tex = gd_shader_create(blob, (uint32_t)size, e);
        static const uint32_t texels[2] = {0xff0000ffu, 0xffff0000u};
        struct gd_resource *tex = texture(2, 1, 1, texels);
        struct gd_view *srv = tex ? gd_view_create(tex, &(struct gd_view_desc){.kind = GD_VIEW_SRV}, &err) : NULL;
        static const float consts[8] = {0, 0, 0, 0, 0.5f, 1, 1, 1};
        struct gd_resource *cb = buffer(consts, sizeof(consts), GD_BIND_CONSTANT);
        expect(ps_tex && srv && cb, "a textured pixel shader");
        if (ps_tex && srv && cb) {
            gd_set_input_layout(ctx, &il);
            gd_set_topology(ctx, 4);
            gd_set_shader(ctx, 1, vs_color);
            gd_set_shader(ctx, 0, ps_tex);
            gd_set_depth(ctx, &(struct gd_depth){.depth_enable = 0}, 0);
            gd_set_shader_resource(ctx, 0, 0, srv);
            gd_set_constant_buffer(ctx, 0, 0, cb, 0, 0);
            struct gd_sampler smp = {.filter = 0, .address_u = 3, .address_v = 3, .address_w = 3, .max_lod = 1000};
            gd_set_sampler(ctx, 0, 0, &smp);
            /* texture coordinates in the color: u from 0 on the left to 1 on the right */
            static const float p[6][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, 1}, {1, -1}, {-1, -1}};
            struct vert tv[6];
            for (int i = 0; i < 6; i++) {
                tv[i].p[0] = p[i][0], tv[i].p[1] = p[i][1], tv[i].p[2] = 0.5f;
                tv[i].c[0] = (p[i][0] + 1) / 2, tv[i].c[1] = (1 - p[i][1]) / 2, tv[i].c[2] = 0, tv[i].c[3] = 1;
            }
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            gd_clear_dsv(dsv, 1, 1.0f, 0);
            draw_verts(tv, 6);
            expect(close_rgb(pixel(10, 30), 0x000080) && pixel(50, 30) == 0xff0000, "point sampling times cb0[1]");
            smp.filter = 0x15;
            gd_set_sampler(ctx, 0, 0, &smp);
            draw_verts(tv, 6);
            /* pixel 31: u = 31.5 / 64, 0.484375 of the way from the red texel's center to the blue one's */
            uint32_t mid = pixel(31, 30);
            int r = (int)(mid & 255), b = (int)(mid >> 16);
            expect(pixel(2, 30) == 0x000080 && r >= 63 && r <= 68 && b >= 121 && b <= 126 && pixel(61, 30) == 0xff0000,
                   "linear filtering between texels");
            /* the same with aoffimmi(1, 0, 0): the left half reads the blue texel too, the right one clamps to it */
            in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
            out_sig("SV_Target", 0, 0, 0, 15);
            op(88, 3 << 11), code[n++] = 7u << 12 | 1u << 20, code[n++] = 0, code[n++] = 0x5555, end();
            op(90, 0), code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
            op(89, 0), code[n++] = 2u | 1u << 2 | XYZW << 4 | 8u << 12 | 2u << 20, code[n++] = 0, code[n++] = 2,
                end();
            op(69, (int)(1u << 31)), code[n++] = 1u | 1u << 9; /* sample_aoffimmi(1,0,0) */
            dst(TEMP, 0, 15), src(IN, 1, SW(0, 1, 0, 0)), src(RES, 0, XYZW);
            code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
            op(56, 0), dst(OUT, 0, 15), src(TEMP, 0, XYZW), cbsrc(0, 1, XYZW), end();
            op(62, 0), end();
            size = container(0, 1);
            struct gd_shader *ps_off = gd_shader_create(blob, (uint32_t)size, e);
            expect(ps_off != NULL, "a texel offset parses");
            if (ps_off) {
                smp.filter = 0;
                gd_set_sampler(ctx, 0, 0, &smp);
                gd_set_shader(ctx, 0, ps_off);
                draw_verts(tv, 6);
                expect(pixel(10, 30) == 0xff0000 && pixel(50, 30) == 0xff0000, "a constant texel offset");
                gd_shader_unref(ps_off);
            }
            /* gather4 between the two texels: x and w from the left one, y and z from the right; the red channel, then
               the blue one the sampler operand selects */
            for (int ch = 0; ch < 2; ch++) {
                in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
                out_sig("SV_Target", 0, 0, 0, 15);
                op(88, 3 << 11), code[n++] = 7u << 12 | 1u << 20, code[n++] = 0, code[n++] = 0x5555, end();
                op(90, 0), code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
                /* gather4 o0, (.5,.5), t0, s0 */
                op(109, 0), dst(OUT, 0, 15), imm4(0.5f, 0.5f, 0, 0), src(RES, 0, XYZW);
                code[n++] = 2u | 2u << 2 | (ch ? 2u : 0u) << 4 | 6u << 12 | 1u << 20, code[n++] = 0, end();
                op(62, 0), end();
                size = container(0, 1);
                struct gd_shader *ps_g = gd_shader_create(blob, (uint32_t)size, e);
                if (!ps_g) {
                    expect(false, "gather4 parses");
                    break;
                }
                gd_set_shader(ctx, 0, ps_g);
                draw_verts(tv, 6);
                /* red: (1, 0, 0, 1) -> 0x0000ff; blue: (0, 1, 1, 0) -> 0xffff00 */
                expect(pixel(10, 30) == (ch ? 0xffff00u : 0x0000ffu), ch ? "gather4 of the channel the sampler selects"
                                                                         : "gather4 of four texels");
                if (pixel(10, 30) != (ch ? 0xffff00u : 0x0000ffu))
                    printf("  %06x\n", pixel(10, 30));
                gd_shader_unref(ps_g);
            }
            /* lod: the texture spans the screen, two texels over 64 pixels: 2^-5 texels a pixel,
               level 0 after clamping */
            {
                in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
                out_sig("SV_Target", 0, 0, 0, 15);
                op(88, 3 << 11), code[n++] = 7u << 12 | 1u << 20, code[n++] = 0, code[n++] = 0x5555, end();
                op(90, 0), code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
                op(108, 0), dst(TEMP, 0, 15), src(IN, 1, SW(0, 1, 0, 0)), src(RES, 0, XYZW); /* lod r0, v1.xy, t0, s0 */
                code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
                op(24, 0), dst(TEMP, 1, 1), sel(TEMP, 0, 0), immf(0), end();     /* eq r1.x, r0.x, 0 */
                op(49, 0), dst(TEMP, 1, 2), sel(TEMP, 0, 1), immf(-4.5f), end(); /* lt r1.y, r0.y, -4.5 */
                op(49, 0), dst(TEMP, 1, 4), immf(-5.5f), sel(TEMP, 0, 1), end(); /* lt r1.z, -5.5, r0.y */
                op(1, 0), dst(TEMP, 1, 1), sel(TEMP, 1, 0), sel(TEMP, 1, 1), end();
                op(1, 0), dst(TEMP, 1, 1), sel(TEMP, 1, 0), sel(TEMP, 1, 2), end();
                op(55, 0), dst(OUT, 0, 15), src(TEMP, 1, XXXX), imm4(0, 1, 0, 1), imm4(1, 0, 0, 1), end();
                op(62, 0), end();
                size = container(0, 8);
                struct gd_shader *ps_lod = gd_shader_create(blob, (uint32_t)size, e);
                expect(ps_lod != NULL, "lod parses");
                if (ps_lod) {
                    gd_set_shader(ctx, 0, ps_lod);
                    draw_verts(tv, 6);
                    expect(pixel(20, 30) == 0x00ff00, "lod: clamped and as worked out");
                    gd_shader_unref(ps_lod);
                }
            }
            /* uaddc, usubb and swapc: every result compared, green when all are right */
            {
                in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
                out_sig("SV_Target", 0, 0, 0, 15);
                op(132, 0), dst(TEMP, 0, 1), dst(TEMP, 1, 1), immu(0xffffffffu), immu(2), end(); /* uaddc: 1, carry 1 */
                op(133, 0), dst(TEMP, 0, 2), dst(TEMP, 1, 2), immu(1), immu(2), end(); /* usubb: ~0, borrow 1 */
                op(142, 0), dst(TEMP, 2, 3), dst(TEMP, 3, 3), immv(1, 0, 0, 0), immv(10, 20, 0, 0), immv(30, 40, 0, 0),
                    end(); /* swapc: (30, 20) and (10, 40) */
                op(32, 0), dst(TEMP, 4, 3), src(TEMP, 0, XYZW), immv(1, 0xffffffffu, 0, 0), end();
                op(32, 0), dst(TEMP, 5, 3), src(TEMP, 1, XYZW), immv(1, 1, 0, 0), end();
                op(32, 0), dst(TEMP, 6, 3), src(TEMP, 2, XYZW), immv(30, 20, 0, 0), end();
                op(32, 0), dst(TEMP, 7, 3), src(TEMP, 3, XYZW), immv(10, 40, 0, 0), end();
                op(1, 0), dst(TEMP, 4, 3), src(TEMP, 4, XYZW), src(TEMP, 5, XYZW), end();
                op(1, 0), dst(TEMP, 4, 3), src(TEMP, 4, XYZW), src(TEMP, 6, XYZW), end();
                op(1, 0), dst(TEMP, 4, 3), src(TEMP, 4, XYZW), src(TEMP, 7, XYZW), end();
                op(1, 0), dst(TEMP, 4, 1), sel(TEMP, 4, 0), sel(TEMP, 4, 1), end();
                op(55, 0), dst(OUT, 0, 15), src(TEMP, 4, XXXX), imm4(0, 1, 0, 1), imm4(1, 0, 0, 1), end();
                op(62, 0), end();
                size = container(0, 8);
                struct gd_shader *ps_int = gd_shader_create(blob, (uint32_t)size, e);
                expect(ps_int != NULL, "uaddc, usubb and swapc parse");
                if (ps_int) {
                    gd_set_shader(ctx, 0, ps_int);
                    draw_verts(tv, 6);
                    expect(pixel(20, 30) == 0x00ff00, "uaddc, usubb and swapc");
                    gd_shader_unref(ps_int);
                }
            }
            /* eval_centroid, eval_sample_index, eval_snapped, sample_info and sample_pos: one sample a pixel, so the
               color as interpolated at the centre */
            {
                const uint32_t rast = 2u | 1u << 2 | (uint32_t)XYZW << 4 | 14u << 12; /* the rasterizer operand */
                in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
                out_sig("SV_Target", 0, 0, 0, 15);
                op(205, 0), dst(TEMP, 0, 15), src(IN, 1, XYZW), end();
                op(204, 0), dst(TEMP, 1, 15), src(IN, 1, XYZW), immu(0), end();
                op(203, 0), dst(TEMP, 2, 15), src(IN, 1, XYZW), immv(0, 0, 0, 0), end();
                op(111, 0), dst(TEMP, 3, 1), code[n++] = rast, end();
                op(110, 0), dst(TEMP, 4, 3), code[n++] = rast, immu(0), end();
                op(0, 0), dst(TEMP, 0, 15), src(TEMP, 0, XYZW), src(TEMP, 1, XYZW), end();
                op(0, 0), dst(TEMP, 0, 15), src(TEMP, 0, XYZW), src(TEMP, 2, XYZW), end();
                op(56, 0), dst(TEMP, 0, 15), src(TEMP, 0, XYZW), imm4(1 / 3.0f, 1 / 3.0f, 1 / 3.0f, 1 / 3.0f), end();
                op(56, 0), dst(TEMP, 0, 15), src(TEMP, 0, XYZW), src(TEMP, 3, XXXX), end();
                op(0, 0), dst(OUT, 0, 15), src(TEMP, 0, XYZW), src(TEMP, 4, SW(0, 1, 0, 1)), end();
                op(62, 0), end();
                size = container(0, 8);
                struct gd_shader *ps_eval = gd_shader_create(blob, (uint32_t)size, e);
                expect(ps_eval != NULL, "eval_* and sample_info parse");
                if (ps_eval) {
                    gd_set_shader(ctx, 0, ps_color);
                    draw_verts(tv, 6);
                    uint32_t want[2] = {pixel(20, 30), pixel(50, 10)};
                    gd_set_shader(ctx, 0, ps_eval);
                    draw_verts(tv, 6);
                    expect(close_rgb(pixel(20, 30), want[0]) && close_rgb(pixel(50, 10), want[1]) && want[0] != want[1],
                           "eval_* at the pixel centre, one sample");
                    gd_shader_unref(ps_eval);
                }
            }
            gd_set_shader_resource(ctx, 0, 0, NULL);
            gd_set_constant_buffer(ctx, 0, 0, NULL, 0, 0);
            gd_set_shader(ctx, 0, ps_color);
            gd_set_depth(ctx, NULL, 0);
        }
        if (srv)
            gd_view_unref(srv);
        gd_resource_unref(tex);
        gd_resource_unref(cb);
        if (ps_tex)
            gd_shader_unref(ps_tex);
    }

    /* a shader the host cannot compile ends its context: the draw is made in software, the context made again, and
       the next draws go to the host as before */
    {
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Target", 0, 0, 0, 15);
        op(54, 0), dst(OUT, 0, 15), src(IN, 1, XYZW), end();
        op(62, 0), end();
        size_t size = container(0, 1);
        char e[64];
        struct gd_shader *ps_bad = gd_shader_create(blob, (uint32_t)size, e);
        expect(ps_bad != NULL, "a pixel shader for the lost context");
        if (ps_bad) {
            struct vert bv[6];
            uint64_t before = gd_host_draws();
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            gd_set_shader(ctx, 0, ps_bad);
            gd_host_test_bad_shader();
            quad(bv, -1, -1, 0, 1, 0.5f, (const float[4]){1, 0, 0, 1});
            draw_verts(bv, 6);
            expect(pixel(10, 30) == 0x0000ff && pixel(50, 30) == 0,
                   "a shader the host cannot use is drawn in software");
            gd_set_shader(ctx, 0, ps_color);
            quad(bv, 0, -1, 1, 1, 0.5f, (const float[4]){0, 0, 1, 1});
            draw_verts(bv, 6);
            expect(pixel(10, 30) == 0x0000ff && pixel(50, 30) == 0xff0000 && (!before || gd_host_draws() > before),
                   "the host draws again after its context is made again");
            gd_clear_dsv(dsv, 1, 1.0f, 0);
            gd_shader_unref(ps_bad);
        }
    }

    /* a 4-sample target: drawn into, read back with ld_ms, and resolved (one sample a pixel, as on the host) */
    {
        struct gd_res_desc md = {.dim = GD_TEX2D, .width = W, .height = H, .mips = 1, .array = 1, .samples = 4,
                                 .format = GD_R8G8B8A8_UNORM,
                                 .bind = GD_BIND_RENDER_TARGET | GD_BIND_SHADER_RESOURCE};
        struct gd_resource *ms = gd_resource_create(&md, NULL, &err);
        struct gd_view *ms_rtv = ms ? gd_view_create(ms, &(struct gd_view_desc){.kind = GD_VIEW_RTV}, &err) : NULL;
        struct gd_view *ms_srv = ms ? gd_view_create(ms, &(struct gd_view_desc){.kind = GD_VIEW_SRV}, &err) : NULL;
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Target", 0, 0, 0, 15);
        op(88, 4 << 11 | 4 << 16), code[n++] = 7u << 12 | 1u << 20, code[n++] = 0, code[n++] = 0x5555,
            end(); /* dcl_resource_texture2dms(4) t0 float */
        op(28, 0), dst(TEMP, 0, 3), src(IN, 0, XYZW), end(); /* ftou r0.xy, v0 */
        op(54, 0), dst(TEMP, 0, 12), immv(0, 0, 0, 0), end();
        op(46, 0), dst(TEMP, 1, 15), src(TEMP, 0, XYZW), src(RES, 0, XYZW), immu(2), end(); /* ld_ms r1, r0, t0, 2 */
        op(54, 0), dst(OUT, 0, 15), src(TEMP, 1, XYZW), end();
        op(62, 0), end();
        size_t size = container(0, 2);
        char e[64];
        struct gd_shader *ps_ms = gd_shader_create(blob, (uint32_t)size, e);
        expect(ms_rtv && ms_srv && ps_ms, "a multisampled target and ld_ms");
        if (ms_rtv && ms_srv && ps_ms) {
            struct vert mv[6];
            gd_set_render_target(ctx, 0, ms_rtv);
            gd_set_depth_target(ctx, NULL);
            gd_clear_rtv(ms_rtv, (const float[4]){0, 0, 1, 1});
            quad(mv, -1, -1, 0, 1, 0.5f, (const float[4]){1, 0, 0, 1});
            draw_verts(mv, 6);
            gd_set_render_target(ctx, 0, rtv);
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            gd_copy_region(rt_tex, 0, 0, 0, 0, ms, 0, NULL); /* ResolveSubresource */
            expect(pixel(10, 30) == 0x0000ff && pixel(50, 30) == 0xff0000, "a multisampled target resolves");
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            gd_set_shader(ctx, 0, ps_ms);
            gd_set_shader_resource(ctx, 0, 0, ms_srv);
            quad(mv, -1, -1, 1, 1, 0.5f, (const float[4]){0, 1, 0, 1});
            draw_verts(mv, 6);
            expect(pixel(10, 30) == 0x0000ff && pixel(50, 30) == 0xff0000, "ld_ms reads a multisampled texture");
            gd_set_shader_resource(ctx, 0, 0, NULL);
            gd_set_shader(ctx, 0, ps_color);
            gd_set_depth_target(ctx, dsv);
        }
        if (ps_ms)
            gd_shader_unref(ps_ms);
        if (ms_srv)
            gd_view_unref(ms_srv);
        if (ms_rtv)
            gd_view_unref(ms_rtv);
        if (ms)
            gd_resource_unref(ms);
    }

    /* a geometry shader that makes a square of every point */
    {
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
        op(93, 1 << 11), end();     /* dcl_inputprimitive point */
        op(92, 5 << 11), end();     /* dcl_outputtopology trianglestrip */
        op(94, 0), code[n++] = 4, end(); /* dcl_maxout 4 */
        const float k = 4.0f / W * 2;
        const float corner[4][2] = {{-k, -k}, {k, -k}, {-k, k}, {k, k}};
        for (int i = 0; i < 4; i++) {
            op(0, 0), dst(OUT, 0, 15); /* add o0, v[0][0], offset */
            code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)IN << 12 | 2u << 20, code[n++] = 0, code[n++] = 0;
            imm4(corner[i][0], corner[i][1], 0, 0), end();
            op(54, 0), dst(OUT, 1, 15);
            code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)IN << 12 | 2u << 20, code[n++] = 0, code[n++] = 1;
            end();
            op(19, 0), end(); /* emit */
        }
        op(62, 0), end();
        char gerr[64];
        size_t gsize = container(2, 1);
        struct gd_shader *gsh = gd_shader_create(blob, (uint32_t)gsize, gerr);
        expect(gsh != NULL, "geometry shader parses");
        gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
        gd_set_raster(ctx, &(struct gd_raster){.fill = 3, .cull = 1, .depth_clip = 1});
        gd_set_shader(ctx, 2, gsh);
        gd_set_topology(ctx, 1);
        struct vert gp[1] = {{{PX(32), PY(32), 0.5f}, {0, 1, 0, 1}}};
        draw_verts(gp, 1);
        expect(pixel(32, 32) == 0x00ff00 && pixel(29, 30) == 0x00ff00 && pixel(35, 35) == 0x00ff00 &&
                   pixel(40, 32) == 0 && pixel(32, 24) == 0,
               "the geometry shader's triangles are drawn");
        /* its stream output: the two triangles of the strip, then nothing while a third does not fit */
        {
            struct gd_so_entry so[1] = {{0, "SV_Position", 0, 0, 4, 0}};
            uint32_t stride = 16;
            static uint8_t zero[6 * 16 + 20];
            struct gd_resource *sob = buffer(zero, sizeof(zero), GD_BIND_STREAM_OUTPUT);
            expect(gsh && gd_shader_set_so(gsh, so, 1, &stride, 1, 0), "a geometry shader with stream output");
            gd_set_so_target(ctx, 0, sob, 0);
            draw_verts(gp, 1);
            draw_verts(gp, 1);
            gd_set_so_target(ctx, 0, NULL, 0);
            float got[8];
            memcpy(got, gd_subresource(sob, 0, NULL), sizeof(got));
            expect(gd_so_filled(sob) == 96 && near(got[0], PX(32) - k, 1e-5f) && near(got[1], PY(32) - k, 1e-5f) &&
                       got[2] == 0.5f && got[3] == 1 && near(got[4], PX(32) + k, 1e-5f),
                   "a geometry shader's stream output");
            if (gd_so_filled(sob) != 96)
                printf("  %u bytes\n", gd_so_filled(sob));
            gd_resource_unref(sob);
        }
        gd_set_shader(ctx, 2, NULL);
        gd_set_raster(ctx, NULL);
        gd_set_topology(ctx, 4);
        gd_shader_unref(gsh);
    }

    /* clip and cull distances: x clips the left half away, y culls the quad below the middle */
    {
        in_sig("POSITION", 0, 0, 0, 7), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_ClipDistance", 0, 2, 2, 1), out_sig("SV_CullDistance", 0, 3, 2, 2);
        op(54, 0), dst(OUT, 0, 7), src(IN, 0, XYZW), end();
        op(54, 0), dst(OUT, 0, 8), immf(1), end();
        op(54, 0), dst(OUT, 1, 15), src(IN, 1, XYZW), end();
        op(54, 0), dst(OUT, 2, 1), sel(IN, 0, 0), end();
        op(54, 0), dst(OUT, 2, 2), sel(IN, 0, 1), end();
        op(62, 0), end();
        char cerr[64];
        size_t csize = container(1, 1);
        struct gd_shader *vsc = gd_shader_create(blob, (uint32_t)csize, cerr);
        expect(vsc != NULL, "a vertex shader with clip and cull distances parses");
        gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
        gd_clear_dsv(dsv, 3, 1.0f, 0);
        gd_set_shader(ctx, 1, vsc);
        struct vert cq[12];
        quad(cq, -1, 0.1f, 1, 1, 0.5f, red);
        quad(cq + 6, -1, -1, 1, -0.1f, 0.5f, red);
        draw_verts(cq, 12);
        expect(pixel(50, 10) == 0x0000ff && pixel(33, 10) == 0x0000ff && pixel(30, 10) == 0 && pixel(10, 10) == 0 &&
                   pixel(50, 55) == 0 && pixel(10, 55) == 0,
               "clip distances clip, cull distances cull");
        gd_set_shader(ctx, 1, vs_color);
        gd_shader_unref(vsc);
    }

    /* tessellation: a triangle patch whose hull shader asks for factor 4 everywhere (16 triangles), the domain shader
       placing each point between the control points */
    {
        enum { ICP = 25, DOM = 28 };
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
        for (uint32_t i = 0; i < 3; i++)
            pc_sig("SV_TessFactor", i, 13, i, 1);
        pc_sig("SV_InsideTessFactor", 0, 14, 3, 1);
        op(113, 0), end();           /* hs_decls */
        op(147, 3 << 11), end();     /* dcl_input_control_point_count 3 */
        op(148, 3 << 11), end();     /* dcl_output_control_point_count 3 */
        op(149, 2 << 11), end();     /* dcl_tessellator_domain tri */
        op(150, 1 << 11), end();     /* dcl_tessellator_partitioning integer */
        op(151, 3 << 11), end();     /* dcl_tessellator_output_primitive triangle_cw */
        op(115, 0), end();           /* hs_fork_phase */
        for (int r = 0; r < 4; r++)
            op(54, 0), dst(OUT, r, 1), immf(4), end();
        op(62, 0), end();
        char terr[64];
        size_t size = container(3, 1);
        struct gd_shader *hsh = gd_shader_create(blob, (uint32_t)size, terr);
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
        op(149, 2 << 11), end();
        /* o0 = vicp[0][0] * u + vicp[1][0] * v + vicp[2][0] * w, o1 = vicp[0][1] */
        const int comp[3] = {0, 1, 2};
        for (int k = 0; k < 3; k++) {
            op(k ? 50 : 56, 0), dst(k == 2 ? OUT : TEMP, 0, 15);
            code[n++] = 2u | 1u << 2 | (uint32_t)SW(comp[k], comp[k], comp[k], comp[k]) << 4 | (uint32_t)DOM << 12;
            code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)ICP << 12 | 2u << 20, code[n++] = (uint32_t)k,
            code[n++] = 0;
            if (k)
                src(TEMP, 0, XYZW);
            end();
        }
        op(54, 0), dst(OUT, 1, 15);
        code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)ICP << 12 | 2u << 20, code[n++] = 0, code[n++] = 1;
        end();
        op(62, 0), end();
        size = container(4, 1);
        struct gd_shader *dsh = gd_shader_create(blob, (uint32_t)size, terr);
        expect(hsh && dsh, "hull and domain shaders parse");
        if (hsh && dsh) { /* as the host takes them */
            struct dxbc_tgsi th, td;
            bool okh = dxbc_tgsi(&hsh->sh, NULL, &th), okd = dxbc_tgsi_tess(&dsh->sh, &hsh->sh, NULL, &td);
            expect(okh && okd, "hull and domain shaders translate to TGSI");
            if (!okh || !okd)
                printf("%s\n%s\n%s\n", okh ? th.text : th.error, okd ? td.text : td.error, hsh->sh.error);
            if (okh)
                dxbc_tgsi_free(&th);
            if (okd)
                dxbc_tgsi_free(&td);
        }
        gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
        gd_clear_dsv(dsv, 3, 1.0f, 0);
        gd_set_shader(ctx, 3, hsh);
        gd_set_shader(ctx, 4, dsh);
        gd_set_topology(ctx, 35); /* 3 control point patches */
        struct gd_stats before, after;
        gd_get_stats(&before);
        struct vert tp[3] = {{{-1, 1, 0.5f}, {0, 1, 0, 1}}, {{1, 1, 0.5f}, {0, 1, 0, 1}}, {{1, -1, 0.5f}, {0, 1, 0, 1}}};
        draw_verts(tp, 3);
        gd_get_stats(&after);
        uint64_t tris = after.triangles - before.triangles; /* counted in software; the host's card draws them unseen */
        expect((tris == 16 || tris == 0) && after.shader_faults == before.shader_faults &&
                   pixel(60, 4) == 0x00ff00 && pixel(40, 20) == 0x00ff00 && pixel(4, 60) == 0,
               "a tessellated patch is drawn as 16 triangles");
        gd_set_shader(ctx, 3, NULL);
        gd_set_shader(ctx, 4, NULL);
        gd_set_topology(ctx, 4);
        gd_shader_unref(hsh);
        gd_shader_unref(dsh);
    }

    /* tessellation as fxc writes it: a control point phase reading vicp[vOutputControlPointID], a fork phase of three
       instances writing o[vForkInstanceID].x, a join phase reading what the fork wrote (vpc0) and a patch constant
       the domain shader takes its color from */
    {
        enum { OCPID = 22, FORKID = 23, ICP = 25, PC = 27, DOM = 28 };
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
        for (uint32_t i = 0; i < 3; i++)
            pc_sig("SV_TessFactor", i, 13, i, 1);
        pc_sig("SV_InsideTessFactor", 0, 14, 3, 1);
        pc_sig("PATCHCOLOR", 0, 0, 4, 15);
        op(113, 0), end();
        op(147, 3 << 11), end();
        op(148, 3 << 11), end();
        op(149, 2 << 11), end();
        op(150, 1 << 11), end();
        op(151, 3 << 11), end();
        op(114, 0), end(); /* hs_control_point_phase: o# = vicp[vOutputControlPointID][#] */
        for (uint32_t r = 0; r < 2; r++) {
            op(54, 0), dst(OUT, (int)r, 15);
            code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)ICP << 12 | 2u << 20 | 2u << 22;
            code[n++] = 2u | 2u << 2 | (uint32_t)OCPID << 12; /* vOutputControlPointID.x */
            code[n++] = r;
            end();
        }
        op(62, 0), end();
        op(115, 0), end();                   /* hs_fork_phase */
        op(153, 0), code[n++] = 3, end();     /* dcl_hs_fork_phase_instance_count 3 */
        op(54, 0);                            /* mov o[vForkInstanceID].x, 2 */
        code[n++] = 2u | 1u << 4 | (uint32_t)OUT << 12 | 1u << 20 | 2u << 22;
        code[n++] = 2u | 2u << 2 | (uint32_t)FORKID << 12;
        immf(2), end();
        op(62, 0), end();
        op(116, 0), end(); /* hs_join_phase: the inside factor from vpc0, the color */
        op(54, 0), dst(OUT, 3, 1), code[n++] = 2u | 2u << 2 | (uint32_t)PC << 12 | 1u << 20, code[n++] = 0, end();
        op(54, 0), dst(OUT, 4, 15), imm4(0, 0, 1, 1), end();
        op(62, 0), end();
        char terr[64];
        size_t size = container(3, 1);
        struct gd_shader *hsh = gd_shader_create(blob, (uint32_t)size, terr);
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
        for (uint32_t i = 0; i < 3; i++)
            pc_sig("SV_TessFactor", i, 13, i, 1);
        pc_sig("SV_InsideTessFactor", 0, 14, 3, 1);
        pc_sig("PATCHCOLOR", 0, 0, 4, 15);
        op(149, 2 << 11), end();
        for (int k = 0; k < 3; k++) { /* o0 = the control points weighted by the domain location */
            op(k ? 50 : 56, 0), dst(k == 2 ? OUT : TEMP, 0, 15);
            code[n++] = 2u | 1u << 2 | (uint32_t)SW(k, k, k, k) << 4 | (uint32_t)DOM << 12;
            code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)ICP << 12 | 2u << 20, code[n++] = (uint32_t)k,
            code[n++] = 0;
            if (k)
                src(TEMP, 0, XYZW);
            end();
        }
        op(54, 0), dst(OUT, 1, 15), code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | (uint32_t)PC << 12 | 1u << 20,
            code[n++] = 4, end();
        op(62, 0), end();
        size = container(4, 1);
        struct gd_shader *dsh = gd_shader_create(blob, (uint32_t)size, terr);
        expect(hsh && dsh, "phased hull shader and its domain shader parse");
        if (hsh && dsh) {
            struct dxbc_tgsi th, td;
            bool okh = dxbc_tgsi(&hsh->sh, NULL, &th), okd = dxbc_tgsi_tess(&dsh->sh, &hsh->sh, NULL, &td);
            expect(okh && okd, "the phased hull shader translates to TGSI");
            if (!okh || !okd)
                printf("%s\n%s\n", okh ? th.text : th.error, okd ? td.text : td.error);
            if (okh)
                dxbc_tgsi_free(&th);
            if (okd)
                dxbc_tgsi_free(&td);
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            gd_clear_dsv(dsv, 3, 1.0f, 0);
            gd_set_shader(ctx, 3, hsh);
            gd_set_shader(ctx, 4, dsh);
            gd_set_topology(ctx, 35);
            struct vert tp[3] = {{{-1, 1, 0.5f}, {0, 1, 0, 1}}, {{1, 1, 0.5f}, {0, 1, 0, 1}},
                                 {{1, -1, 0.5f}, {0, 1, 0, 1}}};
            draw_verts(tp, 3);
            expect(pixel(60, 4) == 0xff0000 && pixel(40, 20) == 0xff0000 && pixel(4, 60) == 0,
                   "fork and join phases: factors by instance, a patch constant read back");
            gd_set_shader(ctx, 3, NULL);
            gd_set_shader(ctx, 4, NULL);
            gd_set_topology(ctx, 4);
        }
        gd_shader_unref(hsh);
        gd_shader_unref(dsh);
    }

    /* stream output: the vertex shader's outputs written to a buffer while each triangle fits, not drawn; DrawAuto
       draws what was written */
    {
        struct gd_so_entry so[2] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "COLOR", 0, 0, 4, 0}};
        uint32_t stride = 32;
        static uint8_t zero[3 * 32 + 50];
        struct gd_resource *sob = buffer(zero, sizeof(zero), GD_BIND_STREAM_OUTPUT | GD_BIND_VERTEX);
        expect(gd_shader_set_so(vs_color, so, 2, &stride, 1, GD_SO_NO_RASTER), "a stream output declaration");
        gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
        gd_clear_dsv(dsv, 3, 1.0f, 0);
        gd_set_shader(ctx, 2, vs_color);
        gd_set_so_target(ctx, 0, sob, 0);
        struct vert q[6];
        quad(q, -1, -1, 1, 1, 0.5f, red);
        draw_verts(q, 6);
        gd_set_so_target(ctx, 0, sob, 0xffffffffu);
        draw_verts(q, 3);
        float got[8];
        memcpy(got, gd_subresource(sob, 0, NULL), sizeof(got)); /* as Map reads it, after the host wrote it */
        expect(gd_so_filled(sob) == 96 && got[0] == -1 && got[1] == 1 && got[2] == 0.5f && got[3] == 1 && got[4] == 1 &&
                   got[5] == 0 && got[7] == 1 && pixel(60, 4) == 0,
               "stream output writes whole triangles while they fit, and draws nothing");
        gd_set_shader(ctx, 2, NULL);
        gd_set_so_target(ctx, 0, NULL, 0);
        struct gd_input_layout auto_il = {2, {{"POSITION", 0, GD_R32G32B32_FLOAT, 0, 0, 0, 0},
                                              {"COLOR", 0, GD_R32G32B32A32_FLOAT, 0, 16, 0, 0}}};
        gd_set_input_layout(ctx, &auto_il);
        gd_set_vertex_buffer(ctx, 0, sob, 32, 0);
        gd_draw_auto(ctx);
        expect(pixel(60, 4) == 0x0000ff && pixel(4, 60) == 0, "DrawAuto draws what stream output wrote");
        gd_set_vertex_buffer(ctx, 0, NULL, 0, 0);
        gd_set_input_layout(ctx, &il);
        gd_resource_unref(sob);
    }

    /* a pixel shader that counts its pixels into a typed unordered access view u1 (u0 is the render target's) */
    {
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Target", 0, 0, 0, 15);
        op(156, 1 << 11), code[n++] = 30u << 12 | 1u << 20, code[n++] = 1, code[n++] = 0x4444, end();
        op(173, 0), dst(30, 1, 1), immu(0), immu(1), end(); /* atomic_iadd u1, 0, 1 */
        op(54, 0), dst(OUT, 0, 15), src(IN, 1, XYZW), end(); /* mov o0, v1 */
        op(62, 0), end();
        size_t size = container(0, 1);
        char e[64];
        struct gd_shader *ps_count = gd_shader_create(blob, (uint32_t)size, e);
        uint32_t zero = 0;
        struct gd_res_desc rd = {.dim = GD_BUFFER, .width = 4, .height = 1, .depth = 1, .mips = 1, .array = 1,
                                 .bind = GD_BIND_UNORDERED_ACCESS};
        struct gd_resource *count = gd_resource_create(&rd, &(struct gd_init){(uint64_t)(uintptr_t)&zero, 0, 0}, &err);
        struct gd_view *uav = count ? gd_view_create(count, &(struct gd_view_desc){.kind = GD_VIEW_UAV,
                                                       .format = GD_R32_UINT, .dim = 1}, &err) : NULL;
        expect(ps_count && uav, "a pixel shader with an unordered access view");
        if (ps_count && uav) {
            gd_set_shader(ctx, 1, vs_color);
            gd_set_shader(ctx, 0, ps_count);
            gd_set_topology(ctx, 4);
            gd_set_depth(ctx, &(struct gd_depth){.depth_enable = 0}, 0);
            gd_set_uav(ctx, 0, 1, uav, 0xffffffffu);
            static const float p[6][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, 1}, {1, -1}, {-1, -1}};
            struct vert q[6];
            for (int i = 0; i < 6; i++) {
                q[i].p[0] = p[i][0], q[i].p[1] = p[i][1], q[i].p[2] = 0.5f;
                q[i].c[0] = 0, q[i].c[1] = 1, q[i].c[2] = 0, q[i].c[3] = 1;
            }
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            draw_verts(q, 6);
            uint32_t got = *(const uint32_t *)gd_subresource(count, 0, NULL);
            expect(got == W * H && pixel(5, 5) == 0x00ff00, "every pixel adds one to the counter");
            if (got != W * H)
                printf("  %u of %u\n", got, W * H);
            /* no targets at all: the viewport alone sets the area drawn */
            gd_set_render_target(ctx, 0, NULL);
            gd_set_depth_target(ctx, NULL);
            gd_set_viewports(ctx, 1, &(struct gd_viewport){0, 0, 32, 16, 0, 1});
            draw_verts(q, 6);
            uint32_t more = *(const uint32_t *)gd_subresource(count, 0, NULL) - got;
            expect(more == 32 * 16, "a draw without targets runs its pixel shader over the viewport");
            if (more != 32 * 16)
                printf("  %u of %u\n", more, 32 * 16);
            gd_set_viewports(ctx, 1, &(struct gd_viewport){0, 0, W, H, 0, 1});
            gd_set_render_target(ctx, 0, rtv);
            gd_set_depth_target(ctx, dsv);
            gd_set_uav(ctx, 0, 1, NULL, 0xffffffffu);
            gd_set_shader(ctx, 0, ps_color);
            gd_set_depth(ctx, NULL, 0);
        }
        if (uav)
            gd_view_unref(uav);
        if (count)
            gd_resource_unref(count);
        if (ps_count)
            gd_shader_unref(ps_count);
    }

    /* SV_IsFrontFace: green on the front, red on the back (nothing culled) */
    {
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("SV_IsFrontFace", 0, 9, 1, 1);
        out_sig("SV_Target", 0, 0, 0, 15);
        op(99, 1 << 11), code[n++] = 1u | 1u << 4 | IN << 12 | 1u << 20, code[n++] = 1, code[n++] = 9, end();
        op(55, 0), dst(OUT, 0, 15), src(IN, 1, XXXX), imm4(0, 1, 0, 1), imm4(1, 0, 0, 1), end();
        op(62, 0), end();
        size_t size = container(0, 1);
        char e[64];
        struct gd_shader *ps_face = gd_shader_create(blob, (uint32_t)size, e);
        expect(ps_face != NULL, "SV_IsFrontFace parses");
        if (ps_face) {
            gd_set_shader(ctx, 1, vs_color);
            gd_set_shader(ctx, 0, ps_face);
            gd_set_topology(ctx, 4);
            gd_set_depth(ctx, &(struct gd_depth){.depth_enable = 0}, 0);
            gd_set_raster(ctx, &(struct gd_raster){.fill = 3, .cull = 1, .depth_clip = 1});
            struct vert q[6], back[6];
            quad(q, -1, -1, 0, 1, 0.5f, red);
            quad(back, 0, -1, 1, 1, 0.5f, red);
            for (int i = 0; i < 3; i++) { /* the other way round */
                struct vert t = back[i];
                back[i] = back[5 - i], back[5 - i] = t;
            }
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            draw_verts(q, 6);
            draw_verts(back, 6);
            expect(pixel(10, 30) == 0x00ff00 && pixel(50, 30) == 0x0000ff, "SV_IsFrontFace on both sides");
            if (pixel(10, 30) != 0x00ff00 || pixel(50, 30) != 0x0000ff)
                printf("  %06x %06x\n", pixel(10, 30), pixel(50, 30));
            gd_set_raster(ctx, NULL);
            gd_set_shader(ctx, 0, ps_color);
            gd_set_depth(ctx, NULL, 0);
            gd_shader_unref(ps_face);
        }
    }

    /* an immediate constant buffer read with a register in the index: icb[r0.x + 1] with r0.x = 1, the green entry */
    {
        in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
        out_sig("SV_Target", 0, 0, 0, 15);
        static const float icb[16] = {1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1};
        code[n++] = 53u | 3u << 11, code[n++] = 2 + 16; /* dcl_immediateConstantBuffer */
        memcpy(&code[n], icb, sizeof(icb));
        n += 16;
        op(54, 0), dst(TEMP, 0, 1), immu(1), end();
        op(54, 0), dst(OUT, 0, 15);
        code[n++] = 2u | 1u << 2 | (uint32_t)XYZW << 4 | 9u << 12 | 1u << 20 | 3u << 22, code[n++] = 1;
        sel(TEMP, 0, 0), end(); /* mov o0, icb[r0.x + 1] */
        op(62, 0), end();
        size_t size = container(0, 1);
        char e[64];
        struct gd_shader *ps_icb = gd_shader_create(blob, (uint32_t)size, e);
        expect(ps_icb != NULL, "an immediate constant buffer parses");
        if (ps_icb) {
            gd_set_shader(ctx, 1, vs_color);
            gd_set_shader(ctx, 0, ps_icb);
            gd_set_topology(ctx, 4);
            gd_set_depth(ctx, &(struct gd_depth){.depth_enable = 0}, 0);
            struct vert q[6];
            quad(q, -1, -1, 1, 1, 0.5f, red);
            gd_clear_rtv(rtv, (const float[4]){0, 0, 0, 1});
            draw_verts(q, 6);
            expect(pixel(10, 30) == 0x00ff00, "a relative index into the immediate constant buffer");
            if (pixel(10, 30) != 0x00ff00)
                printf("  %06x\n", pixel(10, 30));
            gd_set_shader(ctx, 0, ps_color);
            gd_set_depth(ctx, NULL, 0);
            gd_shader_unref(ps_icb);
        }
    }

    struct gd_stats st;
    gd_get_stats(&st);
    expect(st.shader_faults == 0, "no shader faults");
    if (st.shader_faults)
        printf("  %s\n", st.last_fault);
    gd_context_destroy(ctx);
    gd_view_unref(rtv);
    gd_view_unref(dsv);
    gd_resource_unref(rt_tex);
    gd_resource_unref(ds_tex);
    gd_shader_unref(vs_color);
    gd_shader_unref(vs_inst);
    gd_shader_unref(ps_color);
    gd_shader_unref(ps_discard);
}

/* ---- compute: group shared memory with a sync, raw and typed unordered access views, an atomic */

static void opd(int type, int reg, int ncomp) /* u#, g#: one index, no components (declarations) */
{
    code[n++] = (uint32_t)ncomp | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

/* an append buffer: 24 threads append their numbers after the 5 already there, then 24 threads consume them */
static void append_tests(void)
{
    enum { UAV = 30 };
    const uint32_t thread_x = 2u | 2u << 2 | 32u << 12; /* vThreadID.x */
    struct gd_shader *cs[2];
    for (int k = 0; k < 2; k++) {
        op(155, 0), code[n++] = 8, code[n++] = 1, code[n++] = 1, end(); /* dcl_thread_group 8, 1, 1 */
        op(158, 0), opd(UAV, 0, 0), code[n++] = 4, end();              /* dcl_uav_structured u0, 4 */
        op(157, 0), opd(UAV, 1, 0), end();                             /* dcl_uav_raw u1 */
        op(k ? 179 : 178, 0), dst(TEMP, 0, 1), opd(UAV, 0, 0), end();   /* imm_atomic_alloc / consume r0.x, u0 */
        if (!k) {
            op(168, 0), dst(UAV, 0, 1), sel(TEMP, 0, 0), immu(0), code[n++] = thread_x, end(); /* u0[r0.x] = id */
        } else {
            op(167, 0), dst(TEMP, 1, 1), sel(TEMP, 0, 0), immu(0), src(UAV, 0, XXXX), end(); /* r1.x = u0[r0.x] */
            op(41, 0), dst(TEMP, 0, 2), code[n++] = thread_x, immu(2), end();                 /* r0.y = id * 4 */
            op(166, 0), dst(UAV, 1, 1), sel(TEMP, 0, 1), sel(TEMP, 1, 0), end();             /* u1[id] = r1.x */
        }
        op(62, 0), end();
        char err[64];
        size_t size = container(5, 2);
        cs[k] = gd_shader_create(blob, (uint32_t)size, err);
    }
    expect(cs[0] && cs[1], "imm_atomic_alloc and imm_atomic_consume parse");
    uint32_t e;
    struct gd_res_desc rd = {.dim = GD_BUFFER, .width = 4 * 32, .height = 1, .depth = 1, .mips = 1, .array = 1,
                             .bind = GD_BIND_UNORDERED_ACCESS, .misc = GD_MISC_BUFFER_STRUCTURED, .stride = 4};
    struct gd_resource *list = gd_resource_create(&rd, NULL, &e);
    rd.misc = GD_MISC_BUFFER_RAW, rd.stride = 0;
    struct gd_resource *got = gd_resource_create(&rd, NULL, &e);
    struct gd_view_desc vd = {.kind = GD_VIEW_UAV, .dim = 1, .elements = 32, .flags = 2};
    struct gd_view *va = list ? gd_view_create(list, &vd, &e) : NULL;
    vd.format = GD_R32_TYPELESS, vd.flags = 1;
    struct gd_view *vg = got ? gd_view_create(got, &vd, &e) : NULL;
    if (!cs[0] || !cs[1] || !va || !vg) {
        expect(0, "an append buffer");
        goto out;
    }
    struct gd_context *c = gd_context_create();
    gd_set_shader(c, 5, cs[0]);
    gd_set_uav(c, 5, 0, va, 5);
    gd_set_uav(c, 5, 1, vg, 0xffffffffu);
    gd_dispatch(c, 3, 1, 1);
    uint32_t n_after = gd_uav_counter(va), seen = 0;
    const uint32_t *l = (const uint32_t *)gd_subresource(list, 0, NULL);
    for (int i = 5; i < 29; i++)
        if (l[i] < 24)
            seen |= 1u << l[i];
    expect(n_after == 29 && seen == 0xffffff, "imm_atomic_alloc: every thread a slot of its own after the count");
    gd_set_shader(c, 5, cs[1]);
    gd_set_uav(c, 5, 0, va, 0xffffffffu); /* the count stays */
    gd_dispatch(c, 3, 1, 1);
    seen = 0;
    const uint32_t *g = (const uint32_t *)gd_subresource(got, 0, NULL);
    for (int i = 0; i < 24; i++)
        if (g[i] < 24)
            seen |= 1u << g[i];
    expect(gd_uav_counter(va) == 5 && seen == 0xffffff, "imm_atomic_consume takes them back");
    gd_context_destroy(c);
out:
    gd_view_unref(va);
    gd_view_unref(vg);
    gd_resource_unref(list);
    gd_resource_unref(got);
    gd_shader_unref(cs[0]);
    gd_shader_unref(cs[1]);
}

static void compute_tests(void)
{
    enum { UAV = 30, TGSM = 31, FLAT = 36 };
    op(155, 0), code[n++] = 8, code[n++] = 1, code[n++] = 1, end(); /* dcl_thread_group 8, 1, 1 */
    op(159, 0), opd(TGSM, 0, 0), code[n++] = 32, end();                /* dcl_tgsm_raw g0, 32 */
    op(157, 0), opd(UAV, 0, 0), end();                                 /* dcl_uav_raw u0 */
    op(156, 1 << 11), opd(UAV, 1, 0), code[n++] = 0x4444, end();       /* dcl_uav_typed_buffer (uint) u1 */
    /* g0[t] = t, then every thread reads its neighbour after the sync */
    op(41, 0), dst(TEMP, 0, 1), code[n++] = 1u | FLAT << 12, immu(2), end(); /* ishl r0.x, vFlat, 2 */
    op(166, 0), dst(TGSM, 0, 1), sel(TEMP, 0, 0), code[n++] = 1u | FLAT << 12, end();
    op(190, 0x1800), end(); /* sync_g_t */
    op(30, 0), dst(TEMP, 0, 2), code[n++] = 1u | FLAT << 12, immu(1), end(); /* iadd r0.y, vFlat, 1 */
    op(1, 0), dst(TEMP, 0, 2), sel(TEMP, 0, 1), immu(7), end();               /* and r0.y, r0.y, 7 */
    op(41, 0), dst(TEMP, 0, 2), sel(TEMP, 0, 1), immu(2), end();              /* ishl r0.y, r0.y, 2 */
    op(165, 0), dst(TEMP, 1, 1), sel(TEMP, 0, 1), src(TGSM, 0, XXXX), end();  /* ld_raw r1.x, r0.y, g0.x */
    /* u0[SV_DispatchThreadID.x] = the neighbour's number; u1[0] += 1 */
    op(41, 0), dst(TEMP, 0, 4), code[n++] = 2u | 2u << 2 | 32u << 12, immu(2), end(); /* ishl r0.z, vThreadID.x, 2 */
    op(166, 0), dst(UAV, 0, 1), sel(TEMP, 0, 2), sel(TEMP, 1, 0), end();
    op(173, 0), dst(UAV, 1, 1), immu(0), immu(1), end(); /* atomic_iadd u1, 0, 1 */
    op(62, 0), end();
    char err[64];
    size_t size = container(5, 2);
    struct gd_shader *cs = gd_shader_create(blob, (uint32_t)size, err);
    expect(cs != NULL, "compute shader parses");
    if (!cs) {
        printf("  %s\n", err);
        return;
    }
    uint32_t e;
    struct gd_res_desc rd = {.dim = GD_BUFFER, .width = 96, .height = 1, .depth = 1, .mips = 1, .array = 1,
                             .bind = GD_BIND_UNORDERED_ACCESS, .misc = GD_MISC_BUFFER_RAW};
    struct gd_resource *out = gd_resource_create(&rd, NULL, &e);
    rd.width = 4, rd.misc = 0;
    struct gd_resource *count = gd_resource_create(&rd, NULL, &e);
    struct gd_view_desc vd = {.kind = GD_VIEW_UAV, .format = GD_R32_TYPELESS, .dim = 1, .flags = 1};
    struct gd_view *v0 = gd_view_create(out, &vd, &e);
    vd.format = GD_R32_UINT, vd.flags = 0;
    struct gd_view *v1 = gd_view_create(count, &vd, &e);
    expect(v0 && v1, "unordered access views");
    gd_clear_uav(v0, (const uint32_t[4]){0xdead, 0, 0, 0}, false);
    expect(((uint32_t *)out->data)[23] == 0xdead, "ClearUnorderedAccessViewUint fills a raw buffer");
    struct gd_context *c = gd_context_create();
    gd_set_shader(c, 5, cs);
    gd_set_uav(c, 5, 0, v0, 0xffffffffu);
    gd_set_uav(c, 5, 1, v1, 0xffffffffu);
    gd_dispatch(c, 3, 1, 1);
    gd_subresource(out, 0, NULL), gd_subresource(count, 0, NULL); /* what the host wrote, as Map reads it */
    int good = 1;
    for (int i = 0; i < 24; i++)
        good &= ((uint32_t *)out->data)[i] == (uint32_t)((i % 8 + 1) % 8);
    expect(good, "threads see the group's shared memory after a sync");
    expect(*(uint32_t *)count->data == 24, "atomic add from every thread");
    if (!good || *(uint32_t *)count->data != 24)
        printf("  out[0..8] %u %u %u %u %u %u %u %u count %u\n", ((uint32_t *)out->data)[0], ((uint32_t *)out->data)[1],
               ((uint32_t *)out->data)[2], ((uint32_t *)out->data)[3], ((uint32_t *)out->data)[4],
               ((uint32_t *)out->data)[5], ((uint32_t *)out->data)[6], ((uint32_t *)out->data)[7],
               *(uint32_t *)count->data);
    struct gd_stats st;
    gd_get_stats(&st);
    expect(st.dispatches == 1, "one dispatch");
    gd_context_destroy(c);
    gd_view_unref(v0);
    gd_view_unref(v1);
    gd_resource_unref(out);
    gd_resource_unref(count);
    gd_shader_unref(cs);
    append_tests();
}

/* ---- the tessellator: every triangle the same way round, the patch covered once, edges split by their factors */

static int mesh_ok(const struct tess_mesh *m, bool tri, float want_area, uint32_t want_tris)
{
    float total = 0;
    for (uint32_t i = 0; i + 3 <= m->nind; i += 3) {
        const float *a = m->pt[m->ind[i]], *b = m->pt[m->ind[i + 1]], *c = m->pt[m->ind[i + 2]];
        int x = tri ? 1 : 0, y = tri ? 2 : 1;
        float area = ((b[x] - a[x]) * (c[y] - a[y]) - (c[x] - a[x]) * (b[y] - a[y])) / 2;
        if (!(area > 0))
            return 0;
        total += area;
    }
    return near(total, want_area, 1e-4f) && (!want_tris || m->nind / 3 == want_tris) && !m->full;
}

static uint32_t on_edge(const struct tess_mesh *m, int coord, float value)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->npt; i++)
        n += m->pt[i][coord] == value;
    return n;
}

static void tess_tests(void)
{
    struct tess_mesh m = {0};
    const float ones[4] = {1, 1, 1, 1}, fours[4] = {4, 4, 4, 4};
    expect(tess_patch(&m, 3, 1, 3, ones, ones, 64) && m.npt == 4 && mesh_ok(&m, false, 1, 2), "a quad of factor 1");
    expect(tess_patch(&m, 3, 1, 3, fours, fours, 64) && m.npt == 25 && mesh_ok(&m, false, 1, 32),
           "a quad of factor 4: a 4 x 4 grid");
    const float mixed[4] = {2, 5, 3, 7}, in[2] = {3, 6};
    expect(tess_patch(&m, 3, 1, 3, mixed, in, 64) && mesh_ok(&m, false, 1, 0) && on_edge(&m, 0, 0) == 3 &&
               on_edge(&m, 1, 0) == 6 && on_edge(&m, 0, 1) == 4 && on_edge(&m, 1, 1) == 8,
           "a quad's edges are split by their own factors, the inside by its own");
    const float tri3[3] = {3, 3, 3}, tri_mixed[3] = {1, 4, 6};
    expect(tess_patch(&m, 2, 1, 3, tri3, tri3, 64) && m.npt == 10 && mesh_ok(&m, true, 0.5f, 9),
           "a triangle of factor 3");
    expect(tess_patch(&m, 2, 1, 3, tri_mixed, (const float[1]){5}, 64) && mesh_ok(&m, true, 0.5f, 0) &&
               on_edge(&m, 0, 0) == 2 && on_edge(&m, 1, 0) == 5 && on_edge(&m, 2, 0) == 7,
           "a triangle's edges are split by their own factors");
    expect(tess_patch(&m, 3, 3, 3, (const float[4]){1.5f, 2, 2, 2}, (const float[2]){2, 2}, 64) &&
               on_edge(&m, 0, 0) == 4 && mesh_ok(&m, false, 1, 0),
           "fractional odd partitioning rounds up to odd");
    expect(!tess_patch(&m, 3, 1, 3, (const float[4]){0, 4, 4, 4}, fours, 64), "a zero factor culls the patch");
    expect(tess_patch(&m, 1, 1, 2, (const float[2]){3, 4}, NULL, 64) && m.per == 2 && m.nind == 3 * 4 * 2,
           "isolines: 3 lines of 4 segments");
    expect(tess_patch(&m, 3, 1, 4, fours, fours, 64) && m.nind == 96 && !mesh_ok(&m, false, 1, 32),
           "counter-clockwise output turns the other way");
    tess_free(&m);
}

int device_tests(void)
{
    tess_tests();
    format_tests();
    sample_tests();
    draw_tests();
    compute_tests();
    return failures;
}
