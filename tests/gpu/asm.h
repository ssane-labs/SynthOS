/* A small assembler for Shader Model 4 bytecode, for the GPU tests: instructions, operands and the DXBC container
   with input and output signatures. */

#ifndef TESTS_GPU_ASM_H
#define TESTS_GPU_ASM_H

#include <string.h>

#include <synth/types.h>

/* ---- an assembler for the few forms the tests need */

static __attribute__((unused)) uint32_t code[512];
static __attribute__((unused)) int n;

enum { TEMP = 0, IN = 1, OUT = 2, IMM = 4, SAMP = 6, RES = 7, CB = 8, NUL = 13 };
#define SW(a, b, c, d) ((a) | (b) << 2 | (c) << 4 | (d) << 6)
#define XYZW SW(0, 1, 2, 3)
#define XXXX SW(0, 0, 0, 0)

static __attribute__((unused)) int at;

static __attribute__((unused)) void op(int opcode, int extra)
{
    at = n;
    code[n++] = (uint32_t)opcode | (uint32_t)extra;
}

static __attribute__((unused)) void end(void)
{
    code[at] |= (uint32_t)(n - at) << 24;
}

static __attribute__((unused)) void dst(int type, int reg, int mask)
{
    code[n++] = 2u | (uint32_t)mask << 4 | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

static __attribute__((unused)) void src(int type, int reg, int swz)
{
    code[n++] = 2u | 1u << 2 | (uint32_t)swz << 4 | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

static __attribute__((unused)) void srcmod(int type, int reg, int swz, int mod)
{
    code[n++] = 2u | 1u << 2 | (uint32_t)swz << 4 | (uint32_t)type << 12 | 1u << 20 | 0x80000000u;
    code[n++] = 1u | (uint32_t)mod << 6;
    code[n++] = (uint32_t)reg;
}

static __attribute__((unused)) void cbsrc(int slot, int reg, int swz)
{
    code[n++] = 2u | 1u << 2 | (uint32_t)swz << 4 | (uint32_t)CB << 12 | 2u << 20;
    code[n++] = (uint32_t)slot;
    code[n++] = (uint32_t)reg;
}

static __attribute__((unused)) void sel(int type, int reg, int comp) /* r#.x as a scalar */
{
    code[n++] = 2u | 2u << 2 | (uint32_t)comp << 4 | (uint32_t)type << 12 | 1u << 20;
    code[n++] = (uint32_t)reg;
}

static __attribute__((unused)) void imm4(float a, float b, float c, float d)
{
    code[n++] = 2u | (uint32_t)IMM << 12;
    memcpy(&code[n], (float[4]){a, b, c, d}, 16);
    n += 4;
}

static __attribute__((unused)) void immu(uint32_t v)
{
    code[n++] = 1u | (uint32_t)IMM << 12;
    code[n++] = v;
}

static __attribute__((unused)) void immf(float f)
{
    code[n++] = 1u | (uint32_t)IMM << 12;
    memcpy(&code[n++], &f, 4);
}

static __attribute__((unused)) uint8_t blob[8192];

/* signature elements for the next container: name, semantic index, system value, register, mask */
struct sig {
    const char *name;
    uint32_t index, sv, reg, mask;
};
static struct sig sig_in[16], sig_out[16];
static int nsig_in, nsig_out;

static __attribute__((unused)) void in_sig(const char *name, uint32_t index, uint32_t sv, uint32_t reg, uint32_t mask)
{
    sig_in[nsig_in++] = (struct sig){name, index, sv, reg, mask};
}

static __attribute__((unused)) void out_sig(const char *name, uint32_t index, uint32_t sv, uint32_t reg, uint32_t mask)
{
    sig_out[nsig_out++] = (struct sig){name, index, sv, reg, mask};
}

static size_t put_sig(uint8_t *at, const char *tag, const struct sig *s, int count)
{
    uint8_t *d = at + 8;
    uint32_t v = (uint32_t)count;
    memcpy(d, &v, 4);
    v = 8;
    memcpy(d + 4, &v, 4);
    size_t names = 8 + (size_t)count * 24, end = names;
    for (int i = 0; i < count; i++) {
        uint8_t *e = d + 8 + i * 24;
        uint32_t f[5] = {(uint32_t)end, s[i].index, s[i].sv, 3, s[i].reg};
        memcpy(e, f, 20);
        e[20] = (uint8_t)s[i].mask, e[21] = (uint8_t)s[i].mask, e[22] = e[23] = 0;
        size_t len = strlen(s[i].name) + 1;
        memcpy(d + end, s[i].name, len);
        end += len;
    }
    end = (end + 3) & ~(size_t)3;
    memcpy(at, tag, 4);
    v = (uint32_t)end;
    memcpy(at + 4, &v, 4);
    return 8 + end;
}

/* the decoded instructions so far, with dcl_temps first, in a DXBC container with the signatures given */
static __attribute__((unused)) size_t container(int stage, int temps)
{
    uint32_t body[1100];
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
    uint32_t chunks = 3, one = 1;
    size_t at = 32 + 4 * chunks;
    uint32_t offs[3];
    offs[0] = (uint32_t)at;
    at += put_sig(blob + at, "ISGN", sig_in, nsig_in);
    offs[1] = (uint32_t)at;
    at += put_sig(blob + at, "OSGN", sig_out, nsig_out);
    offs[2] = (uint32_t)at;
    uint32_t len = (uint32_t)m * 4;
    memcpy(blob + at, "SHDR", 4);
    memcpy(blob + at + 4, &len, 4);
    memcpy(blob + at + 8, body, len);
    at += 8 + len;
    uint32_t total = (uint32_t)at;
    memcpy(blob + 20, &one, 4);
    memcpy(blob + 24, &total, 4);
    memcpy(blob + 28, &chunks, 4);
    memcpy(blob + 32, offs, sizeof(offs));
    n = 0, nsig_in = nsig_out = 0;
    return total;
}

#endif
