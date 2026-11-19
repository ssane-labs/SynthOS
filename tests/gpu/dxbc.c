/* DXBC interpreter checks with hand-assembled Shader Model 4 bytecode. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int jitted;

/* the same values; any NaN equals any other, since which operand's NaN a sum keeps depends on register order */
static int same_bits(const uint32_t *a, const uint32_t *b, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int na = (a[i] & 0x7f800000u) == 0x7f800000u && (a[i] & 0x7fffffu);
        int nb = (b[i] & 0x7f800000u) == 0x7f800000u && (b[i] & 0x7fffffu);
        if (a[i] != b[i] && !(na && nb))
            return 0;
    }
    return 1;
} /* instructions the compiler took, over all the checks */

/* every shader runs twice, interpreted and with its arithmetic compiled: the two must give the same bits */
static int run(int stage, int temps, const struct dxbc_env *env, unsigned live)
{
    static uint32_t ref[8][4][4];
    struct dxbc_shader sh;
    size_t size = container(stage, temps);
    int result = 0;
    for (int pass = 0; pass < 2; pass++) {
        dxbc_jit_enable(pass == 1);
        if (!dxbc_parse(&sh, blob, size)) {
            printf("  parse: %s\n", sh.error);
            dxbc_jit_enable(true);
            return 0;
        }
        jitted += pass ? dxbc_jit_count(&sh) : 0;
        struct dxbc_lanes l = {.v = v, .o = o, .live = live};
        memset(o, 0, sizeof(o));
        bool ok = dxbc_run(&sh, env, &l);
        if (!ok)
            printf("  run: %s\n", l.error);
        dxbc_free(&sh);
        int r = ok ? (int)(l.live | 0x100) : 0;
        if (!pass) {
            result = r;
            memcpy(ref, o, sizeof(o));
        } else if (r != result || !same_bits(&ref[0][0][0], &o[0][0][0], sizeof(o) / 4)) {
            printf("  compiled code differs from the interpreter\n");
            failures++;
        }
    }
    dxbc_jit_enable(true);
    return result;
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

/* random runs of arithmetic, interpreted and compiled (run compares them) */
static uint32_t seed = 12345;

static uint32_t rnd(uint32_t limit)
{
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed % limit;
}

static uint32_t value(void)
{
    static const uint32_t pool[] = {0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x406ccccd,
                                    0xc0100000, 0x7149f2ca, 0x8ba3c5d7, 0x7f800000, 0xff800000, 0x7fc00000,
                                    7,          0xffffffff, 0x41200000, 0x3e99999a, 0x447a0000, 0x00000003};
    return pool[rnd(sizeof(pool) / sizeof(pool[0]))];
}

static void random_src(int integer_count)
{
    if (integer_count) {
        immu(rnd(40));
        return;
    }
    int swz = (int)rnd(256), mod = (int)rnd(4), kind = (int)rnd(10);
    if (kind < 4) {
        if (mod)
            srcmod(TEMP, (int)rnd(6), swz, mod);
        else
            src(TEMP, (int)rnd(6), swz);
    } else if (kind < 7) {
        if (mod)
            srcmod(IN, (int)rnd(4), swz, mod);
        else
            src(IN, (int)rnd(4), swz);
    } else if (kind < 9) {
        cbsrc(0, (int)rnd(3), swz);
    } else {
        code[n++] = 2u | (uint32_t)IMM << 12;
        for (int k = 0; k < 4; k++)
            code[n++] = value();
    }
}

static void jit_fuzz(struct dxbc_env *env)
{
    static const struct {
        int op, srcs;
    } ops[] = {{0, 2},  {1, 2},  {14, 2}, {15, 2}, {16, 2}, {17, 2}, {24, 2}, {26, 1}, {29, 2}, {30, 2}, {32, 2},
               {33, 2}, {34, 2}, {39, 2}, {40, 1}, {41, 2}, {42, 2}, {43, 1}, {49, 2}, {50, 3}, {51, 2}, {52, 2},
               {54, 1}, {55, 3}, {56, 2}, {57, 2}, {59, 1}, {60, 2}, {68, 1}, {75, 1}, {85, 2}, {87, 2}, {129, 1},
               {25, 1}, {47, 1}, {64, 1}, {65, 1}, {66, 1}, {67, 1}, {27, 1}, {28, 1}, {86, 1}, {134, 1}, {135, 1},
               {136, 1}, {137, 1}, {141, 1}, {130, 1}, {131, 1}, {36, 2}, {37, 2}, {83, 2}, {84, 2}, {80, 2},
               {79, 2}, {35, 3}, {82, 3}};
    int before = failures;
    for (int round = 0; round < 3000 && failures == before; round++) {
        n = 0;
        for (int r = 0; r < 4; r++)
            for (int l = 0; l < 4; l++)
                for (int c = 0; c < 4; c++)
                    v[r][l][c] = value();
        for (int t = 0; t < 6; t++) { /* the temporaries start from inputs */
            op(54, 0), dst(TEMP, t, 15), src(IN, t & 3, (int)rnd(256)), end();
        }
        int count = 1 + (int)rnd(14);
        for (int i = 0; i < count; i++) {
            int k = (int)rnd(sizeof(ops) / sizeof(ops[0]));
            op(ops[k].op, rnd(4) ? 0 : 1 << 13);
            if (rnd(4))
                dst(TEMP, (int)rnd(6), 1 + (int)rnd(15));
            else
                dst(OUT, (int)rnd(2), 1 + (int)rnd(15));
            for (int j = 0; j < ops[k].srcs; j++)
                random_src(j == 1 && (ops[k].op == 41 || ops[k].op == 42 || ops[k].op == 85) && rnd(2));
            end();
        }
        for (int t = 0; t < 6; t++) {
            op(54, 0), dst(OUT, 2 + t, 15), src(TEMP, t, XYZW), end();
        }
        op(62, 0), end();
        if (!run(1, 6, env, 1 + rnd(15)))
            failures++;
    }
    expect(failures == before, "random arithmetic: compiled code gives the interpreter's bits");
}

/* how much faster a typical vertex shader's arithmetic runs compiled: informational */
static void jit_speed(const struct dxbc_env *env)
{
    n = 0;
    for (int i = 0; i < 4; i++) { /* a matrix, lighting and fog */
        op(17, 0), dst(TEMP, 0, 1 << i), src(IN, 0, XYZW), cbsrc(0, i & 1, XYZW), end();
    }
    op(16, 0), dst(TEMP, 1, 1), src(IN, 1, XYZW), src(IN, 1, XYZW), end();
    op(68, 0), dst(TEMP, 1, 1), sel(TEMP, 1, 0), end();
    op(56, 0), dst(TEMP, 2, 7), src(IN, 1, XYZW), sel(TEMP, 1, 0), end();
    op(16, 1 << 13), dst(TEMP, 3, 1), src(TEMP, 2, XYZW), cbsrc(0, 1, XYZW), end();
    op(50, 0), dst(TEMP, 4, 15), sel(TEMP, 3, 0), cbsrc(0, 0, XYZW), imm4(0.1f, 0.1f, 0.1f, 1), end();
    op(52, 0), dst(TEMP, 4, 15), src(TEMP, 4, XYZW), imm4(0, 0, 0, 0), end();
    op(51, 0), dst(TEMP, 4, 15), src(TEMP, 4, XYZW), imm4(1, 1, 1, 1), end();
    op(54, 0), dst(OUT, 0, 15), src(TEMP, 0, XYZW), end();
    op(54, 0), dst(OUT, 1, 15), src(TEMP, 4, XYZW), end();
    op(62, 0), end();
    size_t size = container(1, 5);
    double t[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        struct dxbc_shader sh;
        dxbc_jit_enable(pass == 1);
        if (!dxbc_parse(&sh, blob, size))
            return;
        struct dxbc_lanes l = {.v = v, .o = o, .live = 15};
        clock_t c0 = clock();
        for (int i = 0; i < 200000; i++)
            dxbc_run(&sh, env, &l);
        t[pass] = (double)(clock() - c0) / CLOCKS_PER_SEC;
        dxbc_free(&sh);
    }
    dxbc_jit_enable(true);
    printf("  vertex shader x200000: interpreted %.3f s, compiled %.3f s\n", t[0], t[1]);
}

/* the TGSI text a vertex and a pixel shader become for the host's renderer */
static int has(const char *text, const char *part)
{
    if (strstr(text, part))
        return 1;
    printf("  missing in TGSI: %s\n", part);
    return 0;
}

static void tgsi_tests(void)
{
    struct dxbc_shader sh;
    struct dxbc_tgsi t;
    /* vertex shader: dcl_constantbuffer cb0[4]; dp4 o0.x, v0, cb0[0]; mov o0.yzw, v0; rsq o1.yz, v0.xxyx;
       movc o1.x, v1.x, l(1.0), cb0[r0.x + 1].y (relative); udiv o1.w, null, v1.x, l(3); ishl r0.x, v1.x, l(33) */
    in_sig("POSITION", 0, 0, 0, 15), in_sig("TEXCOORD", 0, 0, 1, 15), in_sig("SV_VertexID", 0, 6, 2, 1);
    out_sig("SV_Position", 0, 1, 0, 15), out_sig("TEXCOORD", 0, 0, 1, 15);
    op(89, 0), code[n++] = 2u | 1u << 2 | XYZW << 4 | 8u << 12 | 2u << 20, code[n++] = 0, code[n++] = 4, end();
    op(17, 0), dst(OUT, 0, 1), src(IN, 0, XYZW), cbsrc(0, 0, XYZW), end();
    op(54, 0), dst(OUT, 0, 14), src(IN, 0, XYZW), end();
    op(68, 0), dst(OUT, 1, 6), src(IN, 0, SW(0, 0, 1, 0)), end();
    op(55, 0), dst(OUT, 1, 1), sel(IN, 1, 0), immf(1);
    code[n++] = 2u | 1u << 2 | XYZW << 4 | 8u << 12 | 2u << 20 | 3u << 25; /* cb0[r0.x + 1]: index 1 relative */
    code[n++] = 0, code[n++] = 1;
    code[n++] = 2u | 2u << 2 | 0u << 4 | 0u << 12 | 1u << 20, code[n++] = 0;
    end();
    op(78, 0), dst(OUT, 1, 8), code[n++] = 13u << 12, sel(IN, 1, 0), immu(3), end();
    op(41, 0), dst(TEMP, 0, 1), sel(IN, 2, 0), immu(33), end();
    op(62, 0), end();
    size_t size = container(1, 1);
    expect(dxbc_parse(&sh, blob, size), "parse a vertex shader for TGSI");
    bool ok = dxbc_tgsi(&sh, NULL, &t);
    if (!ok)
        printf("  TGSI: %s\n", t.error);
    expect(ok && has(t.text, "VERT\n") && has(t.text, "DCL IN[0]\n") && has(t.text, "DCL SV[0], VERTEXID") &&
               has(t.text, "DCL OUT[0], POSITION") && has(t.text, "DCL OUT[1], GENERIC[1]") &&
               has(t.text, "DCL CONST[1][0..3]") && has(t.text, "DP4 OUT[0].x, IN[0].xyzw, CONST[1][0].xyzw") &&
               has(t.text, "RSQ TEMP[1].y, IN[0].xxxx") && has(t.text, "RSQ TEMP[1].z, IN[0].yyyy") &&
               has(t.text, "MOV OUT[1].yz, TEMP[1].xyzw") && has(t.text, "UARL ADDR[0].x, TEMP[0].xxxx") &&
               has(t.text, "UCMP OUT[1].x, IN[1].xxxx, IMM[0].xxxx, CONST[1][ADDR[0].x+1].xyzw") &&
               has(t.text, "UDIV OUT[1].w, IN[1].xxxx, IMM[1].xxxx") && !strstr(t.text, "UMOD") &&
               has(t.text, "SHL TEMP[0].x, SV[0].xxxx, IMM[2].xxxx") && has(t.text, "IMM[2] UINT32 {1, 1, 1, 1}") &&
               has(t.text, "END\n"),
           "a vertex shader in TGSI");
    if (ok && getenv("TGSI_PRINT"))
        printf("%s", t.text);
    dxbc_tgsi_free(&t);
    dxbc_free(&sh);

    /* pixel shader: dcl_resource_texture2d t0; dcl_sampler s0; dcl_input_ps linear v1.xy; sample r0, v1.xyxx, t0.wzyx,
       s0; discard_z r0.w; if_nz r0.x; mov o0, r0; else; mov o0, l(0); endif; mov oDepth, v1.y */
    in_sig("SV_Position", 0, 1, 0, 15), in_sig("TEXCOORD", 0, 0, 1, 3);
    out_sig("SV_Target", 0, 0, 0, 15), out_sig("SV_Depth", 0, 0, 0xffffffffu, 1);
    op(88, 3 << 11), code[n++] = 7u << 12 | 1u << 20, code[n++] = 0, code[n++] = 0x5555, end();
    op(90, 0), code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
    op(98, 2 << 11), dst(IN, 1, 3), end();
    op(100, 4 << 11), dst(IN, 0, 15), code[n++] = 1, end();
    op(69, 0), dst(TEMP, 0, 15), src(IN, 1, SW(0, 1, 0, 0)), src(RES, 0, SW(3, 2, 1, 0));
    code[n++] = 6u << 12 | 1u << 20, code[n++] = 0, end();
    op(13, 0), sel(TEMP, 0, 3), end();
    op(31, 1 << 18), sel(TEMP, 0, 0), end();
    op(54, 0), dst(OUT, 0, 15), src(TEMP, 0, XYZW), end();
    op(18, 0), end();
    op(54, 0), dst(OUT, 0, 15), imm4(0, 0, 0, 0), end();
    op(21, 0), end();
    op(54, 0), code[n++] = 1u | 12u << 12, sel(IN, 1, 1), end();
    op(62, 0), end();
    size = container(0, 1);
    expect(dxbc_parse(&sh, blob, size), "parse a pixel shader for TGSI");
    ok = dxbc_tgsi(&sh, NULL, &t);
    if (!ok)
        printf("  TGSI: %s\n", t.error);
    expect(ok && has(t.text, "FRAG\n") && has(t.text, "DCL IN[0], POSITION, LINEAR") &&
               has(t.text, "DCL IN[1], GENERIC[1], PERSPECTIVE") && has(t.text, "DCL OUT[0], COLOR[0]") &&
               has(t.text, "DCL OUT[1], POSITION") && has(t.text, "DCL SAMP[0]") &&
               has(t.text, "DCL SVIEW[0], 2D, FLOAT") &&
               has(t.text, "MOV TEMP[1], IN[1].xyxx\nTEX TEMP[3], TEMP[1], SAMP[0], 2D") &&
               has(t.text, "MOV TEMP[0], TEMP[3].wzyx") && has(t.text, "USEQ TEMP[1].x, TEMP[0].wwww") &&
               has(t.text, "KILL\nENDIF") && has(t.text, "UIF TEMP[0].xxxx\nMOV OUT[0], TEMP[0].xyzw\nELSE") &&
               has(t.text, "MOV OUT[1].z, TEMP[4].xxxx") && t.tex_sampler[0] == 0,
           "a pixel shader in TGSI");
    if (ok && getenv("TGSI_PRINT"))
        printf("%s", t.text);
    dxbc_tgsi_free(&t);
    dxbc_free(&sh);
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

    tgsi_tests();
    jit_fuzz(&env);
    jit_speed(&env);
    expect(jitted > 0, "the compiler took part of the shaders");
    printf("  %d instructions compiled\n", jitted);
    return failures;
}
