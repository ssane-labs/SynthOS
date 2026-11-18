/* DXBC interpreter checks with hand-assembled Shader Model 4 bytecode. */

#include <stdio.h>
#include <string.h>

#include <synth/dxbc.h>

int dxbc_tests(void);

static int failures;

static void expect(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

#include "asm.h"

static uint32_t v[8][4][4], o[8][4][4];

static float of(int reg, int lane, int c)
{
    float f;
    memcpy(&f, &o[reg][lane][c], 4);
    return f;
}

static void setv(int reg, int lane, float x, float y, float z, float w)
{
    memcpy(v[reg][lane], (float[4]){x, y, z, w}, 16);
}

static int run(int stage, int temps, const struct dxbc_env *env, unsigned live)
{
    struct dxbc_shader sh;
    size_t size = container(stage, temps);
    if (!dxbc_parse(&sh, blob, size)) {
        printf("  parse: %s\n", sh.error);
        return 0;
    }
    struct dxbc_lanes l = {.v = v, .o = o, .live = live};
    memset(o, 0, sizeof(o));
    bool ok = dxbc_run(&sh, env, &l);
    if (!ok)
        printf("  run: %s\n", l.error);
    dxbc_free(&sh);
    return ok ? (int)(l.live | 0x100) : 0;
}

static void load(void *ctx, int texture, const int32_t coord[4], const int offset[3], uint32_t out[4])
{
    (void)ctx, (void)offset;
    out[0] = (uint32_t)(coord[0] * 100 + coord[1]), out[1] = (uint32_t)texture, out[2] = out[3] = 0;
}

static int near(float a, float b)
{
    float d = a - b;
    return d < 1e-4f && d > -1e-4f;
}

static void sample(void *ctx, int texture, int sampler, const float coord[4], const float ddx[4], const float ddy[4],
                   float lod, float bias, const int offset[3], const float *ref, float out[4])
{
    (void)ctx, (void)sampler, (void)ddy, (void)lod, (void)bias, (void)offset, (void)ref;
    out[0] = coord[0] * 10, out[1] = coord[1] * 10, out[2] = (float)texture, out[3] = ddx[0];
}

int dxbc_tests(void)
{
    struct dxbc_env env = {0};
    float cb0[8] = {2, 3, 4, 5, 0.5f, 0, 0, 0};
    env.cb[0] = cb0, env.cb_vec4[0] = 2;
    env.sample = sample;

    /* mad o0, v0, cb0[0], l(1,1,1,1) ; dp3 o1.x, v0.xyzx, v0.xyzx */
    for (int l = 0; l < 4; l++)
        setv(0, l, (float)l, 1, 2, 3);
    op(50, 0), dst(OUT, 0, 15), src(IN, 0, XYZW), cbsrc(0, 0, XYZW), imm4(1, 1, 1, 1), end();
    op(16, 0), dst(OUT, 1, 1), src(IN, 0, SW(0, 1, 2, 0)), src(IN, 0, SW(0, 1, 2, 0)), end();
    op(62, 0), end();
    int r = run(1, 1, &env, 15);
    expect(r && near(of(0, 3, 0), 7) && near(of(0, 3, 3), 16) && near(of(1, 2, 0), 9), "mad with a constant buffer, dp3");

    /* a loop: r1.x += 2 five times */
    op(54, 0), dst(TEMP, 0, 1), immu(0), end();
    op(54, 0), dst(TEMP, 1, 1), immf(0), end();
    op(48, 0), end();
    op(33, 0), dst(TEMP, 2, 1), sel(TEMP, 0, 0), immu(5), end();
    op(3, 1 << 18), sel(TEMP, 2, 0), end();
    op(30, 0), dst(TEMP, 0, 1), sel(TEMP, 0, 0), immu(1), end();
    op(0, 0), dst(TEMP, 1, 1), sel(TEMP, 1, 0), immf(2), end();
    op(22, 0), end();
    op(54, 0), dst(OUT, 0, 1), sel(TEMP, 1, 0), end();
    op(62, 0), end();
    r = run(1, 3, &env, 15);
    expect(r && near(of(0, 0, 0), 10) && near(of(0, 3, 0), 10), "loop with breakc");

    /* lanes that diverge: if (v0.x < 1.5) o0 = 1 else o0 = 2 */
    op(49, 0), dst(TEMP, 0, 1), sel(IN, 0, 0), immf(1.5f), end();
    op(31, 1 << 18), sel(TEMP, 0, 0), end();
    op(54, 0), dst(OUT, 0, 15), imm4(1, 1, 1, 1), end();
    op(18, 0), end();
    op(54, 0), dst(OUT, 0, 15), imm4(2, 2, 2, 2), end();
    op(21, 0), end();
    op(62, 0), end();
    r = run(1, 1, &env, 15);
    expect(r && near(of(0, 0, 0), 1) && near(of(0, 1, 0), 1) && near(of(0, 2, 0), 2) && near(of(0, 3, 0), 2),
           "if and else per lane");

    /* a loop whose length differs per lane: count to v0.x */
    op(54, 0), dst(TEMP, 0, 1), immf(0), end();
    op(48, 0), end();
    op(29, 0), dst(TEMP, 1, 1), sel(TEMP, 0, 0), sel(IN, 0, 0), end();
    op(3, 1 << 18), sel(TEMP, 1, 0), end();
    op(0, 0), dst(TEMP, 0, 1), sel(TEMP, 0, 0), immf(1), end();
    op(22, 0), end();
    op(54, 0), dst(OUT, 0, 1), sel(TEMP, 0, 0), end();
    op(62, 0), end();
    r = run(1, 2, &env, 15);
    expect(r && near(of(0, 0, 0), 0) && near(of(0, 1, 0), 1) && near(of(0, 3, 0), 3), "loops of different lengths");

    /* switch: case 1 -> 10, case 2 falls through to case 3 -> 30, default -> 99 */
    for (int l = 0; l < 4; l++)
        v[1][l][0] = (uint32_t)l;
    op(76, 0), sel(IN, 1, 0), end();
    op(6, 0), immu(1), end();
    op(54, 0), dst(OUT, 0, 1), immf(10), end();
    op(2, 0), end();
    op(6, 0), immu(2), end();
    op(6, 0), immu(3), end();
    op(54, 0), dst(OUT, 0, 1), immf(30), end();
    op(2, 0), end();
    op(10, 0), end();
    op(54, 0), dst(OUT, 0, 1), immf(99), end();
    op(2, 0), end();
    op(23, 0), end();
    op(62, 0), end();
    r = run(1, 1, &env, 15);
    expect(r && near(of(0, 0, 0), 99) && near(of(0, 1, 0), 10) && near(of(0, 2, 0), 30) && near(of(0, 3, 0), 30),
           "switch with fall-through and default");

    /* pixel shader: derivatives across the quad, discard, sample, a negated source, saturate */
    for (int l = 0; l < 4; l++)
        setv(0, l, (float)(l & 1) * 0.25f, (float)(l >> 1) * 0.5f, 0, 1);
    op(11, 0), dst(OUT, 0, 3), src(IN, 0, XYZW), end();
    op(12, 0), dst(OUT, 1, 3), src(IN, 0, SW(1, 0, 0, 0)), end();
    op(69, 0), dst(OUT, 2, 15), src(IN, 0, XYZW), src(RES, 3, XYZW), src(SAMP, 0, XXXX), end();
    op(54, 1 << 13), dst(OUT, 3, 1), srcmod(IN, 0, XXXX, 1), end();
    op(13, 1 << 18), sel(IN, 0, 1), end();
    op(62, 0), end();
    r = run(0, 1, &env, 15);
    expect(r == (0x100 | 3), "discard leaves the lanes with y == 0");
    expect(near(of(0, 0, 0), 0.25f) && near(of(1, 3, 0), 0.5f), "deriv_rtx, deriv_rty");
    expect(near(of(2, 1, 0), 2.5f) && near(of(2, 1, 2), 3) && near(of(2, 1, 3), 0.25f), "sample gets the coordinates and derivatives");
    expect(near(of(3, 1, 0), 0), "saturate of a negated input");

    /* integer and conversion work: ftoi, ishl, ubfe, itof, bfrev, countbits */
    op(27, 0), dst(TEMP, 0, 1), immf(-7.9f), end();
    op(41, 0), dst(TEMP, 0, 2), immu(3), immu(4), end();
    op(138, 0), dst(TEMP, 0, 4), immu(4), immu(4), immu(0xabcd), end();
    op(43, 0), dst(OUT, 0, 7), src(TEMP, 0, XYZW), end();
    op(141, 0), dst(TEMP, 1, 1), immu(1), end();
    op(134, 0), dst(TEMP, 1, 2), immu(0xf0f0), end();
    op(54, 0), dst(OUT, 1, 3), src(TEMP, 1, XYZW), end();
    op(62, 0), end();
    r = run(1, 2, &env, 1);
    expect(r && near(of(0, 0, 0), -7) && near(of(0, 0, 1), 48) && near(of(0, 0, 2), 12) && o[1][0][0] == 0x80000000u &&
               o[1][0][1] == 8,
           "integer ops and conversions");

    /* exp, log, sqrt, sincos, round */
    op(25, 0), dst(OUT, 0, 1), immf(3), end();
    op(47, 0), dst(OUT, 0, 2), immf(0.125f), end();
    op(75, 0), dst(OUT, 0, 4), immf(2.25f), end();
    op(77, 0), dst(OUT, 1, 1), dst(OUT, 2, 1), immf(0.5f), end();
    op(64, 0), dst(OUT, 3, 1), immf(2.5f), end();
    op(64, 0), dst(OUT, 3, 2), immf(3.5f), end();
    op(62, 0), end();
    r = run(1, 1, &env, 1);
    expect(r && near(of(0, 0, 0), 8) && near(of(0, 0, 1), -3) && near(of(0, 0, 2), 1.5f) && near(of(1, 0, 0), 0.479426f) &&
               near(of(2, 0, 0), 0.877583f) && near(of(3, 0, 0), 2) && near(of(3, 0, 1), 4),
           "exp, log, sqrt, sincos, round_ne");

    /* multisampling, drawn with one sample: ld_ms loads it whatever the sample index, sample_info says 1,
       sample_pos is the centre, eval_centroid is the input, and a coverage mask without bit 0 drops the pixel */
    env.load = load;
    for (int l = 0; l < 4; l++) {
        setv(0, l, 0.25f * (float)l, 2, 0, 1);
        uint32_t addr[4] = {(uint32_t)l, 7, 0, 0}, cover[4] = {l == 1 ? 6u : 1u, 0, 0, 0};
        memcpy(v[1][l], addr, 16), memcpy(v[2][l], cover, 16);
    }
    op(46, 0), dst(OUT, 0, 15), src(IN, 1, XYZW), src(RES, 2, XYZW), immu(3), end();
    op(111, 1 << 11), dst(OUT, 1, 1), code[n++] = 14u << 12, end();
    op(110, 0), dst(OUT, 2, 3), src(RES, 2, XYZW), immu(1), end();
    op(205, 0), dst(OUT, 3, 15), src(IN, 0, XYZW), end();
    op(54, 0), code[n++] = 1u | 15u << 12, sel(IN, 2, 0), end();
    op(62, 0), end();
    r = run(0, 1, &env, 15);
    expect(r == (0xd | 0x100) && o[0][2][0] == 207 && o[0][3][1] == 2 && o[1][0][0] == 1 && near(of(2, 2, 0), 0) &&
               near(of(3, 3, 0), 0.75f) && near(of(3, 3, 1), 2),
           "ld_ms, sample_info, sample_pos, eval_centroid, oMask");
    env.load = NULL;

    /* bad input is refused, not run */
    struct dxbc_shader sh;
    expect(!dxbc_parse(&sh, "DXBC", 4), "a short blob is refused");
    op(31, 1 << 18), sel(TEMP, 0, 0), end();
    op(62, 0), end();
    size_t size = container(1, 1);
    expect(!dxbc_parse(&sh, blob, size), "an if without endif is refused");

    return failures;
}
