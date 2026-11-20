/* tests/gpu: the software 3D pipeline on the build machine. Draws a textured, lit cube with a depth buffer, a
   triangle cut by the near plane and a see-through quad, checks pixels, and writes the picture to argv[1]. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <synth/raster.h>

#define W 320
#define H 240

static uint32_t color[W * H];
static float depth[W * H];
static int failures;
int dxbc_tests(void);
int device_tests(void);

struct mat {
    float m[16];
};

static struct mat mul(struct mat a, struct mat b)
{
    struct mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

/* row vectors times the matrix, as Direct3D does */
static void xform(const struct mat *m, const float *v, float *out)
{
    for (int j = 0; j < 4; j++)
        out[j] = v[0] * m->m[j] + v[1] * m->m[4 + j] + v[2] * m->m[8 + j] + v[3] * m->m[12 + j];
}

static struct mat perspective(float fov, float aspect, float zn, float zf)
{
    float ys = 1.0f / tanf(fov / 2), xs = ys / aspect;
    struct mat r = {{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0}};
    return r;
}

static struct mat rot_y(float a)
{
    struct mat r = {{cosf(a), 0, -sinf(a), 0, 0, 1, 0, 0, sinf(a), 0, cosf(a), 0, 0, 0, 0, 1}};
    return r;
}

static struct mat rot_x(float a)
{
    struct mat r = {{1, 0, 0, 0, 0, cosf(a), sinf(a), 0, 0, -sinf(a), cosf(a), 0, 0, 0, 0, 1}};
    return r;
}

static struct mat move(float x, float y, float z)
{
    struct mat r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}};
    return r;
}

/* ---- the cube: 6 faces, 4 corners each, position, normal, texture coordinates */

struct vtx {
    float p[3], n[3], uv[2];
};

static struct vtx cube[24];
static uint32_t cube_idx[36];

static void make_cube(void)
{
    static const float faces[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    for (int f = 0; f < 6; f++) {
        const float *n = faces[f];
        float u[3] = {n[1] != 0 ? 1.0f : n[2] != 0 ? n[2] : 0, 0, n[0] != 0 ? -n[0] : 0};
        if (n[1] != 0)
            u[0] = 1, u[2] = 0;
        float v[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
        for (int c = 0; c < 4; c++) {
            float su = (c == 1 || c == 2) ? 1.0f : -1.0f, sv = c >= 2 ? 1.0f : -1.0f;
            struct vtx *x = &cube[f * 4 + c];
            for (int k = 0; k < 3; k++) {
                x->p[k] = n[k] + u[k] * su + v[k] * sv;
                x->n[k] = n[k];
            }
            x->uv[0] = (su + 1) / 2, x->uv[1] = (sv + 1) / 2;
        }
        /* both windings are fine: the test draws without culling first, then checks culling separately */
        uint32_t b = (uint32_t)f * 4;
        uint32_t idx[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        memcpy(&cube_idx[f * 6], idx, sizeof(idx));
    }
}

struct scene {
    struct mat world, wvp;
};

static void cube_vs1(const void *ctx, uint32_t i, float *out)
{
    const struct scene *s = ctx;
    const struct vtx *v = &cube[i];
    float p[4] = {v->p[0], v->p[1], v->p[2], 1}, n[4] = {v->n[0], v->n[1], v->n[2], 0}, wn[4];
    xform(&s->wvp, p, out);
    xform(&s->world, n, wn);
    out[4] = wn[0], out[5] = wn[1], out[6] = wn[2], out[7] = 0;
    out[8] = v->uv[0], out[9] = v->uv[1], out[10] = 0, out[11] = 0;
}

static void cube_vs(const void *ctx, uint32_t first, int n, float *out, void *scratch)
{
    (void)scratch;
    for (int i = 0; i < n; i++)
        cube_vs1(ctx, first + (uint32_t)i, out + i * 12);
}

static void cube_ps(const void *ctx, const struct rs_quad *q, float c[4][4], unsigned *mask, void *scratch)
{
    (void)ctx, (void)mask, (void)scratch;
    static const float light[3] = {0.4f, 0.7f, -0.6f};
    for (int p = 0; p < 4; p++) {
        const float *in = q->in[p];
        float nd = in[0] * light[0] + in[1] * light[1] + in[2] * light[2];
        float lit = 0.25f + 0.75f * (nd > 0 ? nd : 0);
        int checker = ((int)(in[4] * 4) + (int)(in[5] * 4)) & 1;
        float base[3] = {checker ? 0.95f : 0.2f, checker ? 0.55f : 0.35f, checker ? 0.2f : 0.9f};
        for (int k = 0; k < 3; k++)
            c[p][k] = base[k] * lit;
        c[p][3] = 1;
    }
}

/* ---- a flat triangle, given in clip space */

static float flat[3][4];
static float flat_color[4];

static void flat_vs(const void *ctx, uint32_t first, int n, float *out, void *scratch)
{
    (void)ctx, (void)scratch;
    for (int i = 0; i < n; i++)
        memcpy(out + i * 4, flat[first + (uint32_t)i], sizeof(flat[0]));
}

static void flat_ps(const void *ctx, const struct rs_quad *q, float c[4][4], unsigned *mask, void *scratch)
{
    (void)ctx, (void)q, (void)mask, (void)scratch;
    for (int p = 0; p < 4; p++)
        memcpy(c[p], flat_color, sizeof(flat_color));
}

static uint32_t at(int x, int y)
{
    return color[y * W + x] & 0xffffff;
}

static void expect(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

int main(int argc, char **argv)
{
    struct rs_target t = {color, W, depth, W, W, H};
    make_cube();
    rs_clear(&t, 0xff101018, true, 1.0f, true);

    struct scene sc;
    sc.world = mul(rot_y(0.6f), rot_x(0.45f));
    sc.wvp = mul(mul(sc.world, move(0, 0, 5)), perspective(1.0f, (float)W / H, 0.5f, 50));
    struct rs_state s = {.vs = cube_vs, .ps = cube_ps, .ctx = &sc, .nvary = 2, .depth_test = true, .depth_write = true,
                         .depth_func = RS_LESS};
    int drawn = rs_draw(&t, &s, RS_TRIANGLES, 36, cube_idx, 0);
    expect(drawn >= 6 && drawn <= 12, "the cube's triangles reach the screen");
    expect(at(W / 2, H / 2) != 0x101018, "the cube covers the middle");
    expect(at(2, 2) == 0x101018 && at(W - 3, H - 3) == 0x101018, "the corners stay clear");
    expect(depth[(H / 2) * W + W / 2] > 0 && depth[(H / 2) * W + W / 2] < 1, "the depth buffer has the cube");

    /* the same cube again, nothing changes: equal depth fails LESS */
    uint32_t before = at(W / 2, H / 2);
    flat_color[0] = 1, flat_color[1] = 0, flat_color[2] = 0, flat_color[3] = 1;
    rs_draw(&t, &s, RS_TRIANGLES, 36, cube_idx, 0);
    expect(at(W / 2, H / 2) == before, "drawing again at the same depth changes nothing");

    /* a red triangle behind the cube: hidden where the cube is, seen beside it */
    float far_tri[3][4] = {{-0.95f, -0.9f, 0.99f, 1}, {0.0f, 0.95f, 0.99f, 1}, {0.95f, -0.9f, 0.99f, 1}};
    memcpy(flat, far_tri, sizeof(flat));
    struct rs_state fs = {.vs = flat_vs, .ps = flat_ps, .nvary = 0, .depth_test = true, .depth_func = RS_LESS};
    rs_draw(&t, &fs, RS_TRIANGLES, 3, NULL, 0);
    expect(at(W / 2, H / 2) == before, "a triangle behind the cube is hidden by it");
    expect(at(W / 2, H - 20) == 0xff0000, "and seen where the cube is not");

    /* a triangle cut by the near plane (z < 0 at one corner): only the part in front of the camera is drawn */
    float near_tri[3][4] = {{-1.0f, 0.75f, 0.5f, 1}, {-0.6f, 0.95f, -0.5f, 1}, {-0.2f, 0.75f, 0.5f, 1}};
    memcpy(flat, near_tri, sizeof(flat));
    flat_color[0] = 0, flat_color[1] = 1, flat_color[2] = 0;
    struct rs_state ns = {.vs = flat_vs, .ps = flat_ps, .nvary = 0};
    struct rs_stats st0, st1;
    rs_get_stats(&st0);
    rs_draw(&t, &ns, RS_TRIANGLES, 3, NULL, 0);
    rs_get_stats(&st1);
    expect(st1.clipped == st0.clipped + 1, "the triangle crossing the near plane is clipped");
    expect(at((int)(0.2f * W / 2 + 0.5f * W / 2 * 0), (int)(H * 0.11f)) == 0x00ff00 || at(W / 5, H / 9) == 0x00ff00,
           "its front part is drawn");

    /* half see-through blue over everything */
    float glass_tri[3][4] = {{0.2f, -0.2f, 0.0f, 1}, {0.2f, 0.95f, 0.0f, 1}, {0.95f, -0.2f, 0.0f, 1}};
    memcpy(flat, glass_tri, sizeof(flat));
    flat_color[0] = 0, flat_color[1] = 0, flat_color[2] = 1, flat_color[3] = 0.5f;
    uint32_t under = at((int)(W * 0.75f), (int)(H * 0.4f));
    struct rs_state gs = {.vs = flat_vs, .ps = flat_ps, .nvary = 0, .blend = RS_BLEND_ALPHA};
    rs_draw(&t, &gs, RS_TRIANGLES, 3, NULL, 0);
    uint32_t over = at((int)(W * 0.75f), (int)(H * 0.4f));
    expect((over & 0xff) >= 0x7f && (over >> 16) <= ((under >> 16) + 1) / 2 + 1, "alpha blending mixes half and half");

    /* culling: a clockwise triangle disappears with back faces culled, front faces clockwise by default */
    rs_clear(&t, 0, true, 1.0f, true);
    float cw_tri[3][4] = {{-0.5f, -0.5f, 0.5f, 1}, {0.0f, 0.5f, 0.5f, 1}, {0.5f, -0.5f, 0.5f, 1}};
    memcpy(flat, cw_tri, sizeof(flat));
    flat_color[0] = flat_color[1] = flat_color[2] = flat_color[3] = 1;
    struct rs_state cs = {.vs = flat_vs, .ps = flat_ps, .cull = RS_CULL_BACK};
    expect(rs_draw(&t, &cs, RS_TRIANGLES, 3, NULL, 0) == 1, "a clockwise triangle is a front face");
    cs.front_ccw = true;
    expect(rs_draw(&t, &cs, RS_TRIANGLES, 3, NULL, 0) == 0, "with counter-clockwise fronts it is culled");

    /* two triangles sharing an edge: every pixel of the square is drawn exactly once (the top-left rule) */
    rs_clear(&t, 0, true, 1.0f, true);
    float quad[4][4] = {{-0.5f, -0.5f, 0.5f, 1}, {-0.5f, 0.5f, 0.5f, 1}, {0.5f, -0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}};
    static float qv[6][4];
    int order[6] = {0, 1, 2, 2, 1, 3};
    for (int i = 0; i < 6; i++)
        memcpy(qv[i], quad[order[i]], sizeof(qv[i]));
    flat_color[0] = flat_color[1] = flat_color[2] = 0.25f, flat_color[3] = 1;
    struct rs_state as = {.vs = flat_vs, .ps = flat_ps, .blend = RS_BLEND_ADD};
    memcpy(flat, qv, sizeof(flat));
    rs_draw(&t, &as, RS_TRIANGLES, 3, NULL, 0);
    memcpy(flat, qv[3], sizeof(flat));
    rs_draw(&t, &as, RS_TRIANGLES, 3, NULL, 0);
    int twice = 0, once = 0;
    for (int i = 0; i < W * H; i++) {
        uint32_t v = color[i] & 0xff;
        twice += v > 0x50;
        once += v > 0x30 && v <= 0x50;
    }
    expect(twice == 0 && once == (W / 2) * (H / 2), "shared edges: each pixel once, the square exactly 160 x 120");

    /* the picture of the first scene, for a person to look at */
    if (argc > 1) {
        rs_clear(&t, 0xff101018, true, 1.0f, true);
        rs_draw(&t, &s, RS_TRIANGLES, 36, cube_idx, 0);
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6 %d %d 255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            unsigned char px[3] = {(unsigned char)(color[i] >> 16), (unsigned char)(color[i] >> 8), (unsigned char)color[i]};
            fwrite(px, 1, 3, f);
        }
        fclose(f);
    }
    failures += dxbc_tests();
    failures += device_tests();
    printf("gpu: %d failed\n", failures);
    return failures != 0;
}
