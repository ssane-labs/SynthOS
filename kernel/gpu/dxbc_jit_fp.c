/* Compiles straight runs of DXBC arithmetic to x86-64 code (SSE2). The interpreter (dxbc_fp.c) keeps branches, loops,
   textures and memory; when it reaches the start of a compiled run it calls the run's code, which does, lane after
   lane, what the interpreter would: the same registers in the same layout ([register][lane][component]), the same
   operations in the same order, so the results are the same bits. An instruction this compiler does not know ends
   the run; a shader without such runs is left to the interpreter. */

#if defined(__x86_64__) || defined(RS_HOST)

#include "dxbc_internal.h"
#include "gmath.h"

#ifdef RS_HOST
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define kmalloc malloc
#define kfree free
#else
#include <synth/heap.h>
#include <synth/string.h>
#endif

#define MIN_RUN 2 /* shorter runs cost more to call than to interpret */

/* the data in front of the code: constants, then the immediate operands */
enum {
    D_SIGN = 0,
    D_ABS = 16,
    D_ONE = 32,
    D_ONES = 48,
    D_MASK = 64,          /* 16 write masks */
    D_NOTMASK = 64 + 256, /* and their complements */
    D_IMM = 64 + 512,
};

struct emit {
    uint8_t *code;
    size_t at, cap;
    uint32_t *data;
    size_t data_at, data_cap; /* in bytes, past D_IMM */
    bool full;
};

static void b(struct emit *e, uint8_t v)
{
    if (e->at < e->cap)
        e->code[e->at] = v;
    else
        e->full = true;
    e->at++;
}

static void d32(struct emit *e, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b(e, (uint8_t)(v >> (8 * i)));
}

/* general registers by number */
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R12 = 12, R13 = 13, R14 = 14, R15 = 15 };

/* an SSE instruction with a memory operand [base + disp32]: prefix (0, 0x66, 0xf3), 0x0f, op */
static void sse_m(struct emit *e, uint8_t prefix, uint8_t op, int x, int base, int32_t disp)
{
    if (prefix)
        b(e, prefix);
    if (base >= 8)
        b(e, 0x41);
    b(e, 0x0f);
    b(e, op);
    b(e, (uint8_t)(0x80 | (x & 7) << 3 | (base & 7)));
    if ((base & 7) == RSP)
        b(e, 0x24);
    d32(e, (uint32_t)disp);
}

static void sse_r(struct emit *e, uint8_t prefix, uint8_t op, int dst, int src)
{
    if (prefix)
        b(e, prefix);
    b(e, 0x0f);
    b(e, op);
    b(e, (uint8_t)(0xc0 | (dst & 7) << 3 | (src & 7)));
}

static void pshufd(struct emit *e, int dst, int src, uint8_t imm)
{
    sse_r(e, 0x66, 0x70, dst, src);
    b(e, imm);
}

static void cmpps(struct emit *e, int dst, int src, uint8_t pred)
{
    sse_r(e, 0, 0xc2, dst, src);
    b(e, pred);
}

/* psrad/pslld/psrld by an immediate: 66 0f 72 /4, /6, /2 */
static void shift_imm(struct emit *e, int ext, int x, uint8_t count)
{
    b(e, 0x66);
    b(e, 0x0f);
    b(e, 0x72);
    b(e, (uint8_t)(0xc0 | ext << 3 | (x & 7)));
    b(e, count);
}

enum {
    MOVUPS_LOAD = 0x10, MOVUPS_STORE = 0x11, MOVAPS = 0x28, SQRTPS = 0x51, ANDPS = 0x54, ANDNPS = 0x55, ORPS = 0x56,
    XORPS = 0x57, ADDPS = 0x58, MULPS = 0x59, CVTDQ2PS = 0x5b, SUBPS = 0x5c, MINPS = 0x5d, DIVPS = 0x5e, MAXPS = 0x5f,
    PCMPGTD = 0x66, PCMPEQD = 0x76, PAND = 0xdb, PANDN = 0xdf, POR = 0xeb, PXOR = 0xef, PSUBD = 0xfa, PADDD = 0xfe,
};

enum { CMP_EQ = 0, CMP_LT = 1, CMP_LE = 2, CMP_UNORD = 3, CMP_NEQ = 4, CMP_ORD = 7 };

static void patch(struct emit *e, size_t where)
{
    if (where + 4 <= e->cap) {
        uint32_t rel = (uint32_t)(e->at - (where + 4));
        for (int i = 0; i < 4; i++)
            e->code[where + i] = (uint8_t)(rel >> (8 * i));
    }
}

/* an immediate operand's four values in the data area: its offset from the data's start */
static int32_t imm_data(struct emit *e, const uint32_t v[4])
{
    if (!e->data) /* the first pass only counts bytes */
        return D_IMM;
    for (size_t at = 0; at + 16 <= e->data_at; at += 16)
        if (!memcmp((const uint8_t *)e->data + D_IMM + at, v, 16))
            return (int32_t)(D_IMM + at);
    if (e->data_at + 16 > e->data_cap) {
        e->full = true;
        return D_SIGN;
    }
    memcpy((uint8_t *)e->data + D_IMM + e->data_at, v, 16);
    e->data_at += 16;
    return (int32_t)(D_IMM + e->data_at - 16);
}

/* ---- what can be compiled */

/* no relative part: the interpreter adds one only from these registers */
static bool plain_index(const struct index *ix)
{
    return ix->rel_type != T_TEMP && ix->rel_type != T_XTEMP && ix->rel_type != T_INPUT;
}

static bool src_ok(const struct dxbc_shader *sh, const struct opd *o)
{
    switch (o->type) {
    case T_IMM32: return true;
    case T_TEMP: return o->idx[0].imm >= 0 && o->idx[0].imm < sh->temps;
    case T_INPUT: return o->ndim == 1 && plain_index(&o->idx[0]) && o->idx[0].imm >= 0 && o->idx[0].imm < 32;
    case T_CB:
        return o->ndim >= 2 && plain_index(&o->idx[1]) && o->idx[0].imm >= 0 && o->idx[0].imm < DXBC_MAX_CB &&
               o->idx[1].imm >= 0 && o->idx[1].imm < (1 << 20);
    default: return false;
    }
}

static bool dst_ok(const struct dxbc_shader *sh, const struct opd *o)
{
    switch (o->type) {
    case T_NULL: return true;
    case T_TEMP: return o->idx[0].imm >= 0 && o->idx[0].imm < sh->temps;
    case T_OUTPUT: return plain_index(&o->idx[0]) && o->idx[0].imm >= 0 && o->idx[0].imm < 32;
    default: return false;
    }
}

static int sources(int op)
{
    switch (op) {
    case OP_MOV: case OP_RCP: case OP_SQRT: case OP_RSQ: case OP_ITOF: case OP_NOT: case OP_INEG: return 1;
    case OP_MAD: case OP_MOVC: return 3;
    case OP_ADD: case OP_MUL: case OP_DIV: case OP_MIN: case OP_MAX: case OP_DP2: case OP_DP3: case OP_DP4:
    case OP_LT: case OP_GE: case OP_EQ: case OP_NE: case OP_IADD: case OP_AND: case OP_OR: case OP_XOR: case OP_IEQ:
    case OP_INE: case OP_ILT: case OP_IGE: case OP_ISHL: case OP_ISHR: case OP_USHR:
        return 2;
    default: return 0;
    }
}

static bool int_src(int op)
{
    switch (op) {
    case OP_AND: case OP_OR: case OP_XOR: case OP_NOT: case OP_IADD: case OP_IEQ: case OP_INE: case OP_ILT: case OP_IGE:
    case OP_INEG: case OP_ITOF: case OP_ISHL: case OP_ISHR: case OP_USHR:
        return true;
    default: return false;
    }
}

static bool compilable(const struct dxbc_shader *sh, const struct dxbc_ins *in)
{
    int n = sources(in->op);
    if (!n || in->nops != n + 1 || !dst_ok(sh, &in->o[0]))
        return false;
    for (int i = 1; i <= n; i++)
        if (!src_ok(sh, &in->o[i]))
            return false;
    if (in->op == OP_ISHL || in->op == OP_ISHR || in->op == OP_USHR) { /* one count for every component */
        const struct opd *c = &in->o[2];
        if (c->type != T_IMM32 || c->mod)
            return false;
        for (int k = 0; k < 4; k++)
            if ((c->imm[c->swz[k]] & 31) != (c->imm[c->swz[0]] & 31))
                return false;
    }
    return true;
}

/* ---- code for one lane */

#define V_OFF ((int32_t)__builtin_offsetof(struct dxbc_lanes, v))
#define O_OFF ((int32_t)__builtin_offsetof(struct dxbc_lanes, o))
#define CB_OFF ((int32_t)__builtin_offsetof(struct dxbc_env, cb))
#define CBN_OFF ((int32_t)__builtin_offsetof(struct dxbc_env, cb_vec4))

/* registers while the code runs: r15 the temporaries, rbx the inputs, r12 the outputs, r13 the environment, r14d
   the lane mask, rbp the data */

static void load_src(struct emit *e, const struct opd *o, int lane, bool integer, int x)
{
    int32_t l16 = lane * 16;
    switch (o->type) {
    case T_IMM32: { /* swizzled in the data */
        uint32_t v[4];
        for (int k = 0; k < 4; k++)
            v[k] = o->imm[o->swz[k]];
        sse_m(e, 0, MOVUPS_LOAD, x, RBP, imm_data(e, v));
        break;
    }
    case T_TEMP: sse_m(e, 0, MOVUPS_LOAD, x, R15, o->idx[0].imm * 64 + l16); break;
    case T_INPUT: sse_m(e, 0, MOVUPS_LOAD, x, RBX, o->idx[0].imm * 64 + l16); break;
    default: { /* cb[slot][index], zeros past the buffer's end */
        int32_t slot = o->idx[0].imm, i = o->idx[1].imm;
        b(e, 0x49), b(e, 0x8b), b(e, 0x85), d32(e, (uint32_t)(CB_OFF + slot * 8)); /* mov rax, [r13 + cb] */
        b(e, 0x48), b(e, 0x85), b(e, 0xc0);                                       /* test rax, rax */
        b(e, 0x0f), b(e, 0x84);                                                   /* jz zero */
        size_t to_zero = e->at;
        d32(e, 0);
        b(e, 0x41), b(e, 0x81), b(e, 0xbd), d32(e, (uint32_t)(CBN_OFF + slot * 4)), d32(e, (uint32_t)i);
        b(e, 0x0f), b(e, 0x86); /* jbe zero: cb_vec4 <= index */
        size_t to_zero2 = e->at;
        d32(e, 0);
        sse_m(e, 0, MOVUPS_LOAD, x, RAX, i * 16);
        b(e, 0xe9);
        size_t to_done = e->at;
        d32(e, 0);
        patch(e, to_zero);
        patch(e, to_zero2);
        sse_r(e, 0, XORPS, x, x);
        patch(e, to_done);
        break;
    }
    }
    uint8_t swz = (uint8_t)(o->swz[0] | o->swz[1] << 2 | o->swz[2] << 4 | o->swz[3] << 6);
    if (o->type != T_IMM32 && swz != 0xe4)
        pshufd(e, x, x, swz);
    if (!o->mod)
        return;
    if (!integer) {
        if (o->mod & 2)
            sse_m(e, 0, ANDPS, x, RBP, D_ABS);
        if (o->mod & 1)
            sse_m(e, 0, XORPS, x, RBP, D_SIGN);
        return;
    }
    if (o->mod & 2) { /* |x| = (x ^ s) - s with s = x >> 31 */
        sse_r(e, 0, MOVAPS, 7, x);
        shift_imm(e, 4, 7, 31);
        sse_r(e, 0x66, PXOR, x, 7);
        sse_r(e, 0x66, PSUBD, x, 7);
    }
    if (o->mod & 1) { /* -x = 0 - x */
        sse_r(e, 0x66, PXOR, 7, 7);
        sse_r(e, 0x66, PSUBD, 7, x);
        sse_r(e, 0, MOVAPS, x, 7);
    }
}

/* the result in xmm0 to the destination, with saturation and the write mask */
static void store_dst(struct emit *e, const struct dxbc_ins *in, int lane)
{
    const struct opd *o = &in->o[0];
    if (in->sat) { /* NaN to 0, then clamp to [0, 1] keeping -0 */
        sse_r(e, 0, MOVAPS, 1, 0);
        cmpps(e, 1, 1, CMP_ORD);
        sse_r(e, 0, ANDPS, 0, 1);
        sse_r(e, 0, XORPS, 1, 1);
        sse_r(e, 0, MAXPS, 1, 0);
        sse_m(e, 0, MOVUPS_LOAD, 0, RBP, D_ONE);
        sse_r(e, 0, MINPS, 0, 1);
    }
    int base = o->type == T_TEMP ? R15 : R12;
    int32_t disp = o->idx[0].imm * 64 + lane * 16;
    unsigned m = o->mask & 15;
    if (m != 15) {
        sse_m(e, 0, MOVUPS_LOAD, 1, base, disp);
        sse_m(e, 0, ANDPS, 1, RBP, D_NOTMASK + (int32_t)m * 16);
        sse_m(e, 0, ANDPS, 0, RBP, D_MASK + (int32_t)m * 16);
        sse_r(e, 0, ORPS, 0, 1);
    }
    sse_m(e, 0, MOVUPS_STORE, 0, base, disp);
}

/* the sum of the first n products in xmm0, from 0 up as the interpreter adds them, in every component */
static void dot(struct emit *e, int n)
{
    sse_r(e, 0, MULPS, 0, 1);
    sse_r(e, 0, XORPS, 2, 2);
    for (int k = 0; k < n; k++) {
        if (k)
            pshufd(e, 3, 0, (uint8_t)(k * 0x55));
        b(e, 0xf3), b(e, 0x0f), b(e, ADDPS), b(e, (uint8_t)(0xc0 | 2 << 3 | (k ? 3 : 0))); /* addss xmm2, xmm3/0 */
    }
    pshufd(e, 0, 2, 0);
}

static void ins_lane(struct emit *e, const struct dxbc_ins *in, int lane)
{
    int op = in->op, n = sources(op);
    if (in->o[0].type == T_NULL)
        return;
    bool integer = int_src(op);
    for (int i = 0; i < n; i++) {
        bool as_int = op == OP_MOVC ? i == 0 : integer;
        if ((op == OP_ISHL || op == OP_ISHR || op == OP_USHR) && i == 1)
            continue;
        load_src(e, &in->o[i + 1], lane, as_int, i);
    }
    switch (op) {
    case OP_MOV: break;
    case OP_ADD: sse_r(e, 0, ADDPS, 0, 1); break;
    case OP_MUL: sse_r(e, 0, MULPS, 0, 1); break;
    case OP_MAD: sse_r(e, 0, MULPS, 0, 1), sse_r(e, 0, ADDPS, 0, 2); break;
    case OP_DIV: sse_r(e, 0, DIVPS, 0, 1); break;
    case OP_RCP:
        sse_m(e, 0, MOVUPS_LOAD, 1, RBP, D_ONE);
        sse_r(e, 0, DIVPS, 1, 0);
        sse_r(e, 0, MOVAPS, 0, 1);
        break;
    case OP_SQRT: sse_r(e, 0, SQRTPS, 0, 0); break;
    case OP_RSQ:
        sse_r(e, 0, SQRTPS, 0, 0);
        sse_m(e, 0, MOVUPS_LOAD, 1, RBP, D_ONE);
        sse_r(e, 0, DIVPS, 1, 0);
        sse_r(e, 0, MOVAPS, 0, 1);
        break;
    case OP_MIN:
    case OP_MAX: /* m = (min: x < y, max: y < x) | y unordered; x where m, else y */
        if (op == OP_MIN) {
            sse_r(e, 0, MOVAPS, 2, 0);
            cmpps(e, 2, 1, CMP_LT);
        } else {
            sse_r(e, 0, MOVAPS, 2, 1);
            cmpps(e, 2, 0, CMP_LT);
        }
        sse_r(e, 0, MOVAPS, 3, 1);
        cmpps(e, 3, 3, CMP_UNORD);
        sse_r(e, 0, ORPS, 2, 3);
        sse_r(e, 0, ANDPS, 0, 2);
        sse_r(e, 0, ANDNPS, 2, 1);
        sse_r(e, 0, ORPS, 0, 2);
        break;
    case OP_DP2: dot(e, 2); break;
    case OP_DP3: dot(e, 3); break;
    case OP_DP4: dot(e, 4); break;
    case OP_LT: cmpps(e, 0, 1, CMP_LT); break;
    case OP_EQ: cmpps(e, 0, 1, CMP_EQ); break;
    case OP_NE: cmpps(e, 0, 1, CMP_NEQ); break;
    case OP_GE: /* y <= x */
        cmpps(e, 1, 0, CMP_LE);
        sse_r(e, 0, MOVAPS, 0, 1);
        break;
    case OP_MOVC: /* m = (k == 0): z where m, else y */
        sse_r(e, 0x66, PXOR, 3, 3);
        sse_r(e, 0x66, PCMPEQD, 0, 3);
        sse_r(e, 0, ANDPS, 2, 0);
        sse_r(e, 0, ANDNPS, 0, 1);
        sse_r(e, 0, ORPS, 0, 2);
        break;
    case OP_ITOF: sse_r(e, 0, CVTDQ2PS, 0, 0); break;
    case OP_IADD: sse_r(e, 0x66, PADDD, 0, 1); break;
    case OP_AND: sse_r(e, 0x66, PAND, 0, 1); break;
    case OP_OR: sse_r(e, 0x66, POR, 0, 1); break;
    case OP_XOR: sse_r(e, 0x66, PXOR, 0, 1); break;
    case OP_NOT: sse_m(e, 0, XORPS, 0, RBP, D_ONES); break;
    case OP_INEG:
        sse_r(e, 0x66, PXOR, 1, 1);
        sse_r(e, 0x66, PSUBD, 1, 0);
        sse_r(e, 0, MOVAPS, 0, 1);
        break;
    case OP_IEQ: sse_r(e, 0x66, PCMPEQD, 0, 1); break;
    case OP_INE:
        sse_r(e, 0x66, PCMPEQD, 0, 1);
        sse_m(e, 0, XORPS, 0, RBP, D_ONES);
        break;
    case OP_ILT: /* y > x */
        sse_r(e, 0x66, PCMPGTD, 1, 0);
        sse_r(e, 0, MOVAPS, 0, 1);
        break;
    case OP_IGE: /* !(y > x) */
        sse_r(e, 0x66, PCMPGTD, 1, 0);
        sse_m(e, 0, XORPS, 1, RBP, D_ONES);
        sse_r(e, 0, MOVAPS, 0, 1);
        break;
    case OP_ISHL:
    case OP_ISHR:
    case OP_USHR: {
        const struct opd *c = &in->o[2];
        uint8_t count = (uint8_t)(c->imm[c->swz[0]] & 31);
        if (count)
            shift_imm(e, op == OP_ISHL ? 6 : op == OP_ISHR ? 4 : 2, 0, count);
        break;
    }
    default: break;
    }
    store_dst(e, in, lane);
}

/* one run: for each lane in the mask, every instruction of the run */
static void run_code(struct emit *e, const struct dxbc_shader *sh, int start, int end)
{
    b(e, 0x55), b(e, 0x53), b(e, 0x41), b(e, 0x54), b(e, 0x41), b(e, 0x55), b(e, 0x41), b(e, 0x56), b(e, 0x41),
        b(e, 0x57);                                          /* push rbp, rbx, r12-r15 */
    b(e, 0x49), b(e, 0x89), b(e, 0xff);                      /* mov r15, rdi */
    b(e, 0x48), b(e, 0x8b), b(e, 0x9e), d32(e, (uint32_t)V_OFF); /* mov rbx, [rsi + v] */
    b(e, 0x4c), b(e, 0x8b), b(e, 0xa6), d32(e, (uint32_t)O_OFF); /* mov r12, [rsi + o] */
    b(e, 0x49), b(e, 0x89), b(e, 0xd5);                      /* mov r13, rdx */
    b(e, 0x41), b(e, 0x89), b(e, 0xce);                      /* mov r14d, ecx */
    b(e, 0x48), b(e, 0x8d), b(e, 0x2d);                      /* lea rbp, [rip - (code offset + data)] */
    d32(e, (uint32_t)(-(int32_t)(e->at + 4) - (int32_t)(D_IMM + e->data_cap)));
    for (int lane = 0; lane < 4; lane++) {
        b(e, 0x41), b(e, 0xf7), b(e, 0xc6), d32(e, 1u << lane); /* test r14d, lane */
        b(e, 0x0f), b(e, 0x84);                                /* jz next lane */
        size_t skip = e->at;
        d32(e, 0);
        for (int pc = start; pc < end; pc++)
            ins_lane(e, &sh->code[pc], lane);
        patch(e, skip);
    }
    b(e, 0x41), b(e, 0x5f), b(e, 0x41), b(e, 0x5e), b(e, 0x41), b(e, 0x5d), b(e, 0x41), b(e, 0x5c), b(e, 0x5b),
        b(e, 0x5d), b(e, 0xc3); /* pop r15-r12, rbx, rbp; ret */
}

/* ---- memory for code */

static uint8_t *code_alloc(size_t size)
{
#ifdef RS_HOST
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#else
    return kmalloc(size);
#endif
}

static void code_free(uint8_t *p, size_t size)
{
#ifdef RS_HOST
    if (p)
        munmap(p, size);
#else
    UNUSED(size);
    kfree(p);
#endif
}

void dxbc_jit_free(struct dxbc_shader *sh)
{
    struct dxbc_jit *j = sh->jit;
    if (!j)
        return;
    code_free(j->mem, j->size);
    kfree(j->fn);
    kfree(j->end);
    kfree(j);
    sh->jit = NULL;
}

static bool jit_disabled;

void dxbc_jit_enable(bool on)
{
    jit_disabled = !on;
}

bool dxbc_jit_enabled(void)
{
    return !jit_disabled;
}

int dxbc_jit_count(const struct dxbc_shader *sh)
{
    return sh->jit ? sh->jit->compiled : 0;
}

#ifndef RS_HOST
#include <synth/shell.h>

static void get_jit(char *buf, size_t cap)
{
    str_copy(buf, jit_disabled ? "off" : "on", cap);
}

static int set_jit(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return -1;
    jit_disabled = !strcmp(value, "off");
    return 0;
}

static const struct setting s_jit = {"shaderjit", "on|off",
                                     "compile shader arithmetic to machine code (for shaders created from now on)",
                                     get_jit, set_jit};
SETTING(s_jit);
#endif

/* the runs: [start, end) of consecutive compilable instructions, at least MIN_RUN long */
static int find_runs(const struct dxbc_shader *sh, int32_t *end)
{
    int runs = 0;
    for (int pc = 0; pc < sh->ncode;) {
        int s = pc;
        while (pc < sh->ncode && compilable(sh, &sh->code[pc]))
            pc++;
        if (pc - s >= MIN_RUN) {
            end[s] = pc;
            runs++;
        }
        if (pc == s)
            pc++;
    }
    return runs;
}

void dxbc_jit_compile(struct dxbc_shader *sh)
{
    if (jit_disabled || sh->ncode <= 0)
        return;
    struct dxbc_jit *j = kmalloc(sizeof(*j));
    if (!j)
        return;
    memset(j, 0, sizeof(*j));
    j->end = kmalloc(sizeof(int32_t) * (size_t)sh->ncode);
    j->fn = kmalloc(sizeof(dxbc_block_fn) * (size_t)sh->ncode);
    if (!j->end || !j->fn)
        goto fail;
    for (int i = 0; i < sh->ncode; i++)
        j->end[i] = 0, j->fn[i] = NULL;
    j->blocks = find_runs(sh, j->end);
    if (!j->blocks)
        goto fail;
    /* sizes: a first pass counts the bytes, the second writes them */
    size_t data_cap = 0, code_size = 0;
    int ins = 0;
    for (int pc = 0; pc < sh->ncode; pc++)
        if (j->end[pc]) {
            ins += j->end[pc] - pc;
            data_cap += (size_t)(j->end[pc] - pc) * 3 * 16;
        }
    struct emit e = {NULL, 0, 0, NULL, 0, data_cap, false};
    for (int pc = 0; pc < sh->ncode; pc++)
        if (j->end[pc])
            run_code(&e, sh, pc, j->end[pc]);
    code_size = e.at;
    size_t data_bytes = D_IMM + data_cap;
    j->size = data_bytes + code_size + 64;
    j->mem = code_alloc(j->size);
    if (!j->mem)
        goto fail;
    uint8_t *base = (uint8_t *)(((uintptr_t)j->mem + 15) & ~(uintptr_t)15);
    uint32_t *data = (uint32_t *)base;
    memset(base, 0, data_bytes);
    for (int i = 0; i < 4; i++) {
        data[D_SIGN / 4 + i] = 0x80000000u;
        data[D_ABS / 4 + i] = 0x7fffffffu;
        data[D_ONE / 4 + i] = U(1.0f);
        data[D_ONES / 4 + i] = 0xffffffffu;
    }
    for (unsigned m = 0; m < 16; m++)
        for (int c = 0; c < 4; c++) {
            data[(D_MASK + m * 16) / 4 + (unsigned)c] = (m >> c) & 1 ? 0xffffffffu : 0;
            data[(D_NOTMASK + m * 16) / 4 + (unsigned)c] = (m >> c) & 1 ? 0 : 0xffffffffu;
        }
    e = (struct emit){base + data_bytes, 0, code_size, data, 0, data_cap, false};
    for (int pc = 0; pc < sh->ncode; pc++)
        if (j->end[pc]) {
            j->fn[pc] = (dxbc_block_fn)(void *)(e.code + e.at);
            run_code(&e, sh, pc, j->end[pc]);
        }
    if (e.full || e.at != code_size)
        goto fail;
    j->compiled = ins;
    sh->jit = j;
    return;
fail:
    if (j->mem)
        code_free(j->mem, j->size);
    kfree(j->fn);
    kfree(j->end);
    kfree(j);
}

#endif
