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

/* ---- an assembler for the few forms the tests need */

static uint32_t code[512];
static int n;

enum { TEMP = 0, IN = 1, OUT = 2, IMM = 4, SAMP = 6, RES = 7, CB = 8, NUL = 13 };
#define SW(a, b, c, d) ((a) | (b) << 2 | (c) << 4 | (d) << 6)
#define XYZW SW(0, 1, 2, 3)
#define XXXX SW(0, 0, 0, 0)

static int at;

static void op(int opcode, int extra)
{
    at = n;
    code[n++] = (uint32_t)opcode | (uint32_t)extra;
}

static void end(void)
{
    code[at] |= (uint32_t)(n - at) << 24;
}

static void dst(int type, int reg, int mask)
{
    code[n++] = 2u | (uint32_t)mask << 4 | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

static void src(int type, int reg, int swz)
{
    code[n++] = 2u | 1u << 2 | (uint32_t)swz << 4 | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

static void srcmod(int type, int reg, int swz, int mod)
{
    code[n++] = 2u | 1u << 2 | (uint32_t)swz << 4 | (uint32_t)type << 12 | 1u << 20 | 0x80000000u;
    code[n++] = 1u | (uint32_t)mod << 6;
    code[n++] = (uint32_t)reg;
}

static void cbsrc(int slot, int reg, int swz)
{
    code[n++] = 2u | 1u << 2 | (uint32_t)swz << 4 | (uint32_t)CB << 12 | 2u << 20;
    code[n++] = (uint32_t)slot;
    code[n++] = (uint32_t)reg;
}

static void sel(int type, int reg, int comp) /* r#.x as a scalar */
{
    code[n++] = 2u | 2u << 2 | (uint32_t)comp << 4 | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

static void imm4(float a, float b, float c, float d)
{
    code[n++] = 2u | (uint32_t)IMM << 12;
    memcpy(&code[n], (float[4]){a, b, c, d}, 16);
    n += 4;
}

static void immu(uint32_t v)
{
    code[n++] = 1u | (uint32_t)IMM << 12;
    code[n++] = v;
}

static void immf(float f)
{
    code[n++] = 1u | (uint32_t)IMM << 12;
    memcpy(&code[n++], &f, 4);
}

static uint8_t blob[4096];

static size_t container(int stage, int temps)
{
    /* dcl_temps goes first in the body */
    uint32_t body[520];
    int m = 0;
    body[m++] = (uint32_t)stage << 16 | 4u << 4;
    body[m++] = 0;
    body[m++] = 104u | 2u << 24;
    body[m++] = (uint32_t)temps;
    memcpy(&body[m], code, (size_t)n * 4);
    m += n;
    body[1] = (uint32_t)m;
    memset(blob, 0, sizeof(blob));
    memcpy(blob, "DXBC", 4);
    uint32_t total = 32 + 4 + 8 + (uint32_t)m * 4, one = 1, off = 36, len = (uint32_t)m * 4;
    memcpy(blob + 20, &one, 4);
    memcpy(blob + 24, &total, 4);
    memcpy(blob + 28, &one, 4);
    memcpy(blob + 32, &off, 4);
    memcpy(blob + 36, "SHDR", 4);
    memcpy(blob + 40, &len, 4);
    memcpy(blob + 44, body, len);
    n = 0;
    return total;
}

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
    struct dxbc_lanes l = {v, o, live, {0}, ""};
    memset(o, 0, sizeof(o));
    bool ok = dxbc_run(&sh, env, &l);
    if (!ok)
        printf("  run: %s\n", l.error);
    dxbc_free(&sh);
    return ok ? (int)(l.live | 0x100) : 0;
}

static int near(float a, float b)
{
    float d = a - b;
    return d < 1e-4f && d > -1e-4f;
}

static void sample(void *ctx, int texture, int sampler, const float coord[4], const float ddx[4], const float ddy[4],
                   float lod, float bias, const int offset[3], float out[4])
{
    (void)ctx, (void)sampler, (void)ddy, (void)lod, (void)bias, (void)offset;
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

    /* bad input is refused, not run */
    struct dxbc_shader sh;
    expect(!dxbc_parse(&sh, "DXBC", 4), "a short blob is refused");
    op(31, 1 << 18), sel(TEMP, 0, 0), end();
    op(62, 0), end();
    size_t size = container(1, 1);
    expect(!dxbc_parse(&sh, blob, size), "an if without endif is refused");

    return failures;
}
