/* 3D Test: the software 3D pipeline (kernel/gpu) drawing spinning, lit, textured cubes into a window, with the frame
   rate and how many triangles and pixels each frame took. On the 64-bit system only. */

#ifdef __x86_64__

#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/raster.h>
#include <synth/string.h>

#include "../internal.h"

struct m4 {
    float m[16];
};

struct vtx {
    float p[3], n[3], uv[2];
};

struct demo {
    float *depth;
    int dw, dh;
    uint64_t start, last, frames_at;
    int frames, fps;
    struct vtx cube[24];
    uint32_t idx[36];
    struct m4 world, wvp;
    float hue;
};

static float fsin(float x) /* good enough for turning cubes: x in radians */
{
    while (x > 3.14159265f)
        x -= 6.2831853f;
    while (x < -3.14159265f)
        x += 6.2831853f;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}

static float fcos(float x)
{
    return fsin(x + 1.5707963f);
}

static struct m4 mul(struct m4 a, struct m4 b)
{
    struct m4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

static void xform(const struct m4 *m, const float *v, float *out)
{
    for (int j = 0; j < 4; j++)
        out[j] = v[0] * m->m[j] + v[1] * m->m[4 + j] + v[2] * m->m[8 + j] + v[3] * m->m[12 + j];
}

static void make_cube(struct demo *d)
{
    static const float faces[6][3][3] = { /* normal, u, v */
        {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
    };
    for (int f = 0; f < 6; f++) {
        for (int c = 0; c < 4; c++) {
            float su = (c == 1 || c == 2) ? 1.0f : -1.0f, sv = c >= 2 ? 1.0f : -1.0f;
            struct vtx *x = &d->cube[f * 4 + c];
            for (int k = 0; k < 3; k++) {
                x->p[k] = faces[f][0][k] + faces[f][1][k] * su + faces[f][2][k] * sv;
                x->n[k] = faces[f][0][k];
            }
            x->uv[0] = (su + 1) / 2, x->uv[1] = (sv + 1) / 2;
        }
        uint32_t b = (uint32_t)f * 4, idx[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        memcpy(&d->idx[f * 6], idx, sizeof(idx));
    }
}

static void vs1(const void *ctx, uint32_t i, float *out)
{
    const struct demo *d = ctx;
    const struct vtx *v = &d->cube[i];
    float p[4] = {v->p[0], v->p[1], v->p[2], 1}, n[4] = {v->n[0], v->n[1], v->n[2], 0}, wn[4];
    xform(&d->wvp, p, out);
    xform(&d->world, n, wn);
    out[4] = wn[0], out[5] = wn[1], out[6] = wn[2], out[7] = 0;
    out[8] = v->uv[0], out[9] = v->uv[1], out[10] = 0, out[11] = 0;
}

static void vs(const void *ctx, uint32_t first, int n, float *out, void *scratch)
{
    UNUSED(scratch);
    for (int i = 0; i < n; i++)
        vs1(ctx, first + (uint32_t)i, out + i * 12);
}

static void ps(const void *ctx, const struct rs_quad *q, float c[4][4], unsigned *mask, void *scratch)
{
    const struct demo *d = ctx;
    UNUSED(mask);
    UNUSED(scratch);
    uint32_t tone = gfx_halo_tone((int)d->hue);
    float tr = (float)(tone >> 16 & 255) / 255.0f, tg = (float)(tone >> 8 & 255) / 255.0f, tb = (float)(tone & 255) / 255.0f;
    for (int p = 0; p < 4; p++) {
        const float *in = q->in[p];
        float nd = in[0] * 0.40f + in[1] * 0.75f - in[2] * 0.55f;
        float lit = 0.22f + 0.78f * (nd > 0 ? nd : 0);
        float u = in[4] * 6, v = in[5] * 6;
        int checker = ((int)u + (int)v) & 1;
        float edge = u - (float)(int)u < 0.06f || v - (float)(int)v < 0.06f ? 0.6f : 1.0f;
        c[p][0] = (checker ? tr : 0.12f) * lit * edge;
        c[p][1] = (checker ? tg : 0.13f) * lit * edge;
        c[p][2] = (checker ? tb : 0.18f) * lit * edge;
        c[p][3] = 1;
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct demo *d = w->data;
    if (d->dw != c->w || d->dh != c->h) {
        kfree(d->depth);
        d->depth = kmalloc(sizeof(float) * (size_t)c->w * (size_t)c->h);
        d->dw = c->w, d->dh = c->h;
    }
    if (!d->depth)
        return;
    uint64_t now = hal_ticks_ms();
    float t = (float)(now - d->start) / 1000.0f;
    struct rs_target tg = {c->px, c->stride, d->depth, c->w, c->w, c->h};
    rs_clear(&tg, 0xff0c0d12, true, 1.0f, true);
    struct rs_stats a, b;
    rs_get_stats(&a);
    float aspect = (float)c->w / (float)(c->h ? c->h : 1), zn = 0.5f, zf = 60.0f;
    float ys = 1.7f, xs = ys / aspect;
    struct m4 proj = {{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0}};
    struct rs_state s = {.vs = vs, .ps = ps, .ctx = d, .nvary = 2, .cull = RS_CULL_NONE, .depth_test = true,
                         .depth_write = true, .depth_func = RS_LESS};
    for (int k = 0; k < 5; k++) { /* five cubes: one in the middle, four around it */
        float ay = t * (0.7f + 0.1f * (float)k), ax = t * 0.45f + (float)k;
        struct m4 ry = {{fcos(ay), 0, -fsin(ay), 0, 0, 1, 0, 0, fsin(ay), 0, fcos(ay), 0, 0, 0, 0, 1}};
        struct m4 rx = {{1, 0, 0, 0, 0, fcos(ax), fsin(ax), 0, 0, -fsin(ax), fcos(ax), 0, 0, 0, 0, 1}};
        float ox = k ? 3.2f * fcos((float)k * 1.5707963f + t * 0.3f) : 0, oy = k ? 1.8f * fsin((float)k * 1.5707963f + t * 0.3f) : 0;
        float sc = k ? 0.55f : 1.0f;
        struct m4 scale = {{sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, sc, 0, 0, 0, 0, 1}};
        struct m4 at = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, ox, oy, 7.0f + (k ? 1.5f : 0), 1}};
        d->world = mul(mul(scale, ry), rx);
        d->wvp = mul(mul(d->world, at), proj);
        d->hue = t * 60 + (float)k * 200;
        rs_draw(&tg, &s, RS_TRIANGLES, 36, d->idx, 0);
    }
    rs_get_stats(&b);
    for (int i = 0; i < c->w * c->h; i += 1) /* the window is opaque */
        c->px[(i / c->w) * c->stride + i % c->w] |= 0xff000000u;
    d->frames++;
    if (now - d->frames_at >= 1000) {
        d->fps = (int)((uint64_t)d->frames * 1000 / (now - d->frames_at));
        d->frames = 0;
        d->frames_at = now;
    }
    char line[96];
    ksnprintf(line, sizeof(line), "%d fps   %u triangles   %u pixels   %d cores", d->fps,
              (unsigned)(b.triangles - a.triangles), (unsigned)(b.pixels - a.pixels), hal_parallel_cpus());
    gfx_text_box(c, (struct rect){14, c->h - 30, c->w - 28, 20}, &font_small, line, RGB(200, 204, 214), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){14, 44, c->w - 28, 20}, &font_bold, "Software 3D pipeline", RGB(240, 240, 244), ALIGN_LEFT);
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_TIMER)
        wm_invalidate(w);
}

static void destroy(struct window *w)
{
    struct demo *d = w->data;
    kfree(d->depth);
    kfree(d);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    struct demo *d = kzalloc(sizeof(*d));
    if (!d)
        return;
    make_cube(d);
    d->start = d->frames_at = hal_ticks_ms();
    struct window *w = wm_open("3D Test", ICON_GAMES, 720, 480, &ops, d, WIN_RESIZABLE | WIN_FULLSIZE);
    if (!w) {
        kfree(d);
        return;
    }
    wm_set_timer(w, 16);
}

static const struct gui_app gpu3d_app = {"3D Test", "Draws spinning cubes with the software 3D pipeline", ICON_GAMES,
                                         launch, NULL, NULL};
GUI_APP(gpu3d_app);

#endif
