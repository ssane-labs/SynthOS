/* The graphics device (include/synth/gd.h) on the build machine: formats, block compression, sampling, and draws
   with hand-assembled shaders through input layouts, depth, stencil, blending, indices and instances. */

#include <stdio.h>
#include <string.h>

#include <synth/gd.h>

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
    rd.format = GD_D24_UNORM_S8_UINT, rd.bind = GD_BIND_DEPTH_STENCIL;
    ds_tex = gd_resource_create(&rd, NULL, &err);
    rtv = gd_view_create(rt_tex, &(struct gd_view_desc){.kind = GD_VIEW_RTV}, &err);
    dsv = gd_view_create(ds_tex, &(struct gd_view_desc){.kind = GD_VIEW_DSV}, &err);
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
        gd_set_shader(ctx, 2, NULL);
        gd_set_raster(ctx, NULL);
        gd_set_topology(ctx, 4);
        gd_shader_unref(gsh);
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
}

int device_tests(void)
{
    format_tests();
    sample_tests();
    draw_tests();
    compute_tests();
    return failures;
}
