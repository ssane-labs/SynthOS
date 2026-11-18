/* DXBC: the Direct3D 10/11 shader container and a four-lane interpreter for Shader Model 4 and 5 vertex and pixel
   shaders (include/synth/dxbc.h). Instructions are decoded once; running them keeps a mask of the lanes taking part,
   so branches, loops and switches work per lane as on a graphics card. */

#if defined(__x86_64__) || defined(RS_HOST)

#include <synth/dxbc.h>

#ifdef RS_HOST
#include <stdlib.h>
#include <string.h>
#define kmalloc malloc
#define kfree free
#else
#include <synth/heap.h>
#include <synth/string.h>
#endif

/* ---- opcodes (D3D10_SB_OPCODE_TYPE and the Shader Model 5 additions) */

enum {
    OP_ADD = 0, OP_AND, OP_BREAK, OP_BREAKC, OP_CALL, OP_CALLC, OP_CASE, OP_CONTINUE, OP_CONTINUEC, OP_CUT, OP_DEFAULT,
    OP_DERIV_RTX, OP_DERIV_RTY, OP_DISCARD, OP_DIV, OP_DP2, OP_DP3, OP_DP4, OP_ELSE, OP_EMIT, OP_EMITTHENCUT, OP_ENDIF,
    OP_ENDLOOP, OP_ENDSWITCH, OP_EQ, OP_EXP, OP_FRC, OP_FTOI, OP_FTOU, OP_GE, OP_IADD, OP_IF, OP_IEQ, OP_IGE, OP_ILT,
    OP_IMAD, OP_IMAX, OP_IMIN, OP_IMUL, OP_INE, OP_INEG, OP_ISHL, OP_ISHR, OP_ITOF, OP_LABEL, OP_LD, OP_LD_MS, OP_LOG,
    OP_LOOP, OP_LT, OP_MAD, OP_MIN, OP_MAX, OP_CUSTOMDATA, OP_MOV, OP_MOVC, OP_MUL, OP_NE, OP_NOP, OP_NOT, OP_OR,
    OP_RESINFO, OP_RET, OP_RETC, OP_ROUND_NE, OP_ROUND_NI, OP_ROUND_PI, OP_ROUND_Z, OP_RSQ, OP_SAMPLE, OP_SAMPLE_C,
    OP_SAMPLE_C_LZ, OP_SAMPLE_L, OP_SAMPLE_D, OP_SAMPLE_B, OP_SQRT, OP_SWITCH, OP_SINCOS, OP_UDIV, OP_ULT, OP_UGE,
    OP_UMUL, OP_UMAD, OP_UMAX, OP_UMIN, OP_USHR, OP_UTOF, OP_XOR,
    OP_DCL_RESOURCE, OP_DCL_CONSTANT_BUFFER, OP_DCL_SAMPLER, OP_DCL_INDEX_RANGE, OP_DCL_GS_OUTPUT_TOPOLOGY,
    OP_DCL_GS_INPUT, OP_DCL_MAX_OUTPUT_VERTEX_COUNT, OP_DCL_INPUT, OP_DCL_INPUT_SGV, OP_DCL_INPUT_SIV, OP_DCL_INPUT_PS,
    OP_DCL_INPUT_PS_SGV, OP_DCL_INPUT_PS_SIV, OP_DCL_OUTPUT, OP_DCL_OUTPUT_SGV, OP_DCL_OUTPUT_SIV, OP_DCL_TEMPS,
    OP_DCL_INDEXABLE_TEMP, OP_DCL_GLOBAL_FLAGS, /* 106 */
    OP_LOD = 108, OP_GATHER4, OP_SAMPLE_POS, OP_SAMPLE_INFO,
    OP_DERIV_RTX_COARSE = 122, OP_DERIV_RTX_FINE, OP_DERIV_RTY_COARSE, OP_DERIV_RTY_FINE, OP_GATHER4_C, OP_GATHER4_PO,
    OP_GATHER4_PO_C, OP_RCP, OP_F32TOF16, OP_F16TOF32, OP_UADDC, OP_USUBB, OP_COUNTBITS, OP_FIRSTBIT_HI,
    OP_FIRSTBIT_LO, OP_FIRSTBIT_SHI, OP_UBFE, OP_IBFE, OP_BFI, OP_BFREV, OP_SWAPC,
    OP_EMIT_STREAM = 117, OP_CUT_STREAM, OP_EMITTHENCUT_STREAM, OP_BUFINFO = 121,
    OP_DCL_THREAD_GROUP = 155, OP_DCL_UAV_TYPED, OP_DCL_UAV_RAW, OP_DCL_UAV_STRUCTURED, OP_DCL_TGSM_RAW,
    OP_DCL_TGSM_STRUCTURED, OP_DCL_RESOURCE_RAW, OP_DCL_RESOURCE_STRUCTURED, OP_LD_UAV_TYPED, OP_STORE_UAV_TYPED,
    OP_LD_RAW, OP_STORE_RAW, OP_LD_STRUCTURED, OP_STORE_STRUCTURED,
    OP_ATOMIC_AND, OP_ATOMIC_OR, OP_ATOMIC_XOR, OP_ATOMIC_CMP_STORE, OP_ATOMIC_IADD, OP_ATOMIC_IMAX, OP_ATOMIC_IMIN,
    OP_ATOMIC_UMAX, OP_ATOMIC_UMIN, OP_IMM_ATOMIC_ALLOC, OP_IMM_ATOMIC_CONSUME, OP_IMM_ATOMIC_IADD, OP_IMM_ATOMIC_AND,
    OP_IMM_ATOMIC_OR, OP_IMM_ATOMIC_XOR, OP_IMM_ATOMIC_EXCH, OP_IMM_ATOMIC_CMP_EXCH, OP_IMM_ATOMIC_IMAX,
    OP_IMM_ATOMIC_IMIN, OP_IMM_ATOMIC_UMAX, OP_IMM_ATOMIC_UMIN, OP_SYNC, /* 190 */
    OP_COUNT = 256
};

enum {
    T_TEMP, T_INPUT, T_OUTPUT, T_XTEMP, T_IMM32, T_IMM64, T_SAMPLER, T_RESOURCE, T_CB, T_ICB, T_LABEL, T_PRIMID,
    T_ODEPTH, T_NULL, T_RASTERIZER, T_OMASK,
    T_UAV = 30, T_TGSM, T_TID, T_GID, T_GTID, T_FLAT = 36, T_GSINST = 37,
    T_ODEPTH_GE = 38, T_ODEPTH_LE = 39,
};

#define MAX_OPS 6
#define MAX_TEMPS 4096

struct index {
    int32_t imm;
    int8_t rel_type;  /* -1: no relative part */
    uint8_t rel_comp;
    int32_t rel_reg, rel_reg2;
};

struct opd {
    uint8_t type, ndim, mod, mask;
    uint8_t swz[4];
    struct index idx[3];
    uint32_t imm[4];
};

struct dxbc_ins {
    uint16_t op;
    uint8_t nops, sat, test_nz, ret_type;
    int8_t offset[3];
    int32_t jump; /* if: else/endif, else: endif, loop: endloop, endloop: loop, switch: endswitch, case: switch */
    struct opd o[MAX_OPS];
};

/* ---- the container */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_sig(const uint8_t *chunk, uint32_t size, struct dxbc_sig_elem *out, int stride, int skip)
{
    if (size < 8)
        return 0;
    uint32_t count = rd32(chunk), first = rd32(chunk + 4);
    int n = 0;
    for (uint32_t i = 0; i < count && n < DXBC_MAX_SIG; i++) {
        const uint8_t *e = chunk + first + (size_t)i * (size_t)stride + (size_t)skip;
        if ((size_t)(e - chunk) + 24 > size)
            break;
        struct dxbc_sig_elem *s = &out[n++];
        uint32_t name = rd32(e);
        memset(s->name, 0, sizeof(s->name));
        for (uint32_t k = 0; name + k < size && k < sizeof(s->name) - 1 && chunk[name + k]; k++)
            s->name[k] = (char)chunk[name + k];
        s->index = rd32(e + 4), s->system_value = rd32(e + 8), s->component_type = rd32(e + 12);
        s->reg = rd32(e + 16), s->mask = e[20], s->rw_mask = e[21];
    }
    return n;
}

static const uint32_t *operand(const uint32_t *p, const uint32_t *end, struct opd *o, bool *bad)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < 3; i++)
        o->idx[i].rel_type = -1;
    if (p >= end) {
        *bad = true;
        return p;
    }
    uint32_t t = *p++;
    if (t & 0x80000000u) {
        uint32_t ext = p < end ? *p++ : 0;
        if ((ext & 0x3f) == 1)
            o->mod = (uint8_t)((ext >> 6) & 0xff);
    }
    int ncomp = (int)(t & 3);
    o->type = (uint8_t)((t >> 12) & 0xff);
    o->ndim = (uint8_t)((t >> 20) & 3);
    o->mask = 15;
    for (int i = 0; i < 4; i++)
        o->swz[i] = (uint8_t)i;
    if (ncomp == 2) {
        int mode = (int)((t >> 2) & 3);
        if (mode == 0)
            o->mask = (uint8_t)((t >> 4) & 15);
        else if (mode == 1)
            for (int i = 0; i < 4; i++)
                o->swz[i] = (uint8_t)((t >> (4 + 2 * i)) & 3);
        else
            for (int i = 0; i < 4; i++)
                o->swz[i] = (uint8_t)((t >> 4) & 3);
    } else if (ncomp == 1) {
        o->mask = 1;
        for (int i = 0; i < 4; i++)
            o->swz[i] = 0;
    } else if (ncomp == 0) {
        o->mask = 0;
    }
    if (o->type == T_IMM32) {
        int n = ncomp == 1 ? 1 : 4;
        for (int i = 0; i < n && p < end; i++)
            o->imm[i] = *p++;
        if (n == 1)
            o->imm[1] = o->imm[2] = o->imm[3] = o->imm[0];
        return p;
    }
    if (o->type == T_IMM64) {
        *bad = true;
        return p + (ncomp == 1 ? 2 : 8);
    }
    for (int d = 0; d < o->ndim; d++) {
        int rep = (int)((t >> (22 + 3 * d)) & 7);
        if (rep == 0 || rep == 3) {
            o->idx[d].imm = p < end ? (int32_t)*p++ : 0;
        } else if (rep == 1 || rep == 4) {
            o->idx[d].imm = p + 1 < end ? (int32_t)p[1] : 0;
            p += 2;
        }
        if (rep >= 2) { /* plus a register: r0.x, x0[1].y, ... */
            struct opd rel;
            p = operand(p, end, &rel, bad);
            o->idx[d].rel_type = (int8_t)rel.type;
            o->idx[d].rel_comp = rel.swz[0];
            o->idx[d].rel_reg = rel.idx[0].imm;
            o->idx[d].rel_reg2 = rel.idx[1].imm;
        }
    }
    return p;
}

static bool is_decl(int op)
{
    return (op >= OP_DCL_RESOURCE && op <= OP_DCL_GLOBAL_FLAGS) || (op >= 143 && op <= 162) || op == 113 ||
           op == 114 || op == 115 || op == 116 || op == OP_CUSTOMDATA || op == OP_LABEL || op == 203 || op == 204 ||
           op == 206;
}

static bool link_flow(struct dxbc_shader *sh)
{
    int stack[64], n = 0;
    for (int i = 0; i < sh->ncode; i++) {
        struct dxbc_ins *in = &sh->code[i];
        switch (in->op) {
        case OP_IF:
        case OP_LOOP:
        case OP_SWITCH:
            if (n == 64)
                return false;
            stack[n++] = i;
            break;
        case OP_ELSE:
            if (!n || sh->code[stack[n - 1]].op != OP_IF)
                return false;
            sh->code[stack[n - 1]].jump = i;
            stack[n - 1] = i;
            break;
        case OP_ENDIF:
            if (!n || (sh->code[stack[n - 1]].op != OP_IF && sh->code[stack[n - 1]].op != OP_ELSE))
                return false;
            sh->code[stack[--n]].jump = i;
            break;
        case OP_ENDLOOP:
            if (!n || sh->code[stack[n - 1]].op != OP_LOOP)
                return false;
            sh->code[stack[n - 1]].jump = i;
            in->jump = stack[--n];
            break;
        case OP_CASE:
        case OP_DEFAULT:
            if (!n || sh->code[stack[n - 1]].op != OP_SWITCH)
                return false;
            in->jump = stack[n - 1];
            break;
        case OP_ENDSWITCH:
            if (!n || sh->code[stack[n - 1]].op != OP_SWITCH)
                return false;
            sh->code[stack[--n]].jump = i;
            break;
        default: break;
        }
    }
    return n == 0;
}

void dxbc_free(struct dxbc_shader *sh)
{
    kfree(sh->code);
    kfree(sh->icb);
    sh->code = NULL;
    sh->icb = NULL;
}

static bool fail(struct dxbc_shader *sh, const char *why)
{
    size_t i = 0;
    for (; why[i] && i < sizeof(sh->error) - 1; i++)
        sh->error[i] = why[i];
    sh->error[i] = 0;
    dxbc_free(sh);
    return false;
}

bool dxbc_parse(struct dxbc_shader *sh, const void *blob, size_t size)
{
    memset(sh, 0, sizeof(*sh));
    sh->pos_in_reg = -1;
    const uint8_t *b = blob;
    if (size < 32 || memcmp(b, "DXBC", 4) || rd32(b + 24) > size)
        return fail(sh, "not DXBC");
    uint32_t chunks = rd32(b + 28);
    const uint32_t *code = NULL;
    uint32_t code_words = 0;
    for (uint32_t i = 0; i < chunks && 32 + i * 4 + 4 <= size; i++) {
        uint32_t off = rd32(b + 32 + i * 4);
        if (off + 8 > size)
            continue;
        const uint8_t *c = b + off;
        uint32_t len = rd32(c + 4);
        if (off + 8 + len > size)
            continue;
        if (!memcmp(c, "ISGN", 4))
            sh->nin = read_sig(c + 8, len, sh->in, 24, 0);
        else if (!memcmp(c, "OSGN", 4))
            sh->nout = read_sig(c + 8, len, sh->out, 24, 0);
        else if (!memcmp(c, "OSG5", 4))
            sh->nout = read_sig(c + 8, len, sh->out, 28, 4);
        else if (!memcmp(c, "SHDR", 4) || !memcmp(c, "SHEX", 4)) {
            code = (const uint32_t *)(c + 8);
            code_words = len / 4;
        }
    }
    if (!code || code_words < 2)
        return fail(sh, "no shader code");
    uint32_t version = code[0];
    sh->minor = (int)(version & 15), sh->major = (int)((version >> 4) & 15);
    sh->stage = (enum dxbc_stage)(version >> 16);
    uint32_t words = code[1] < code_words ? code[1] : code_words;
    const uint32_t *p = code + 2, *end = code + words;
    int cap = 64;
    sh->code = kmalloc(sizeof(struct dxbc_ins) * (size_t)cap);
    if (!sh->code)
        return fail(sh, "out of memory");
    sh->max_in_reg = sh->max_out_reg = -1;
    while (p < end) {
        uint32_t t = *p;
        int op = (int)(t & 0x7ff);
        uint32_t len = (t >> 24) & 0x7f;
        if (op == OP_CUSTOMDATA) {
            len = p + 1 < end ? p[1] : 0;
            if ((t >> 11) == 3 && len >= 2 && !sh->icb) { /* the immediate constant buffer */
                sh->icb_vec4 = (int)((len - 2) / 4);
                sh->icb = kmalloc(sizeof(float) * 4 * (size_t)(sh->icb_vec4 ? sh->icb_vec4 : 1));
                if (sh->icb)
                    memcpy(sh->icb, p + 2, sizeof(float) * 4 * (size_t)sh->icb_vec4);
            }
        }
        if (!len || p + len > end)
            return fail(sh, "bad instruction length");
        const uint32_t *next = p + len;
        if (op == OP_DCL_TEMPS) {
            sh->temps = (int)p[1];
        } else if (op == OP_DCL_INDEXABLE_TEMP && len >= 4) {
            uint32_t r = p[1];
            if (r < 8)
                sh->xtemps[r] = 1, sh->xsize[r] = (int)p[2];
        } else if (op == OP_DCL_INPUT_PS_SIV || op == OP_DCL_INPUT_SIV || op == OP_DCL_INPUT_SGV ||
                   op == OP_DCL_INPUT_PS_SGV || op == OP_DCL_INPUT || op == OP_DCL_INPUT_PS) {
            struct opd o;
            bool bad = false;
            const uint32_t *q = operand(p + 1, next, &o, &bad);
            if (o.type == T_INPUT && o.ndim >= 1) {
                sh->max_in_reg = o.idx[0].imm > sh->max_in_reg ? o.idx[0].imm : sh->max_in_reg;
                if ((op == OP_DCL_INPUT_PS || op == OP_DCL_INPUT_PS_SIV || op == OP_DCL_INPUT_PS_SGV) &&
                    o.idx[0].imm >= 0 && o.idx[0].imm < 32)
                    sh->interp[o.idx[0].imm] = (uint8_t)((t >> 11) & 15);
                if ((op == OP_DCL_INPUT_PS_SIV || op == OP_DCL_INPUT_SIV) && q < next && *q == 1)
                    sh->pos_in_reg = o.idx[0].imm;
            }
        } else if (op == OP_DCL_GS_INPUT) {
            sh->gs_in = (t >> 11) & 0x3f;
        } else if (op == OP_DCL_GS_OUTPUT_TOPOLOGY) {
            sh->gs_out = (t >> 11) & 0x3f;
        } else if (op == OP_DCL_MAX_OUTPUT_VERTEX_COUNT && len >= 2) {
            sh->gs_max = p[1];
        } else if (op == 206 && len >= 2) {
            sh->gs_instances = p[1];
        } else if (op == OP_DCL_THREAD_GROUP && len >= 4) {
            sh->group[0] = p[1], sh->group[1] = p[2], sh->group[2] = p[3];
        } else if (op >= OP_DCL_UAV_TYPED && op <= OP_DCL_RESOURCE_STRUCTURED) {
            struct opd o;
            bool bad = false;
            const uint32_t *q = operand(p + 1, next, &o, &bad);
            int slot = o.idx[0].imm;
            uint32_t a0 = q < next ? q[0] : 0, a1 = q + 1 < next ? q[1] : 0;
            if (o.type == T_UAV && slot >= 0 && slot < DXBC_MAX_UAV) {
                sh->uav_kind[slot] = (uint8_t)(op == OP_DCL_UAV_RAW ? 1 : op == OP_DCL_UAV_STRUCTURED ? 2 : 0);
                sh->uav_stride[slot] = op == OP_DCL_UAV_STRUCTURED ? a0 : 0;
            } else if (o.type == T_RESOURCE && slot >= 0 && slot < DXBC_MAX_RES) {
                sh->srv_stride[slot] = op == OP_DCL_RESOURCE_STRUCTURED ? a0 : 0;
            } else if (o.type == T_TGSM && slot >= 0 && slot < DXBC_MAX_TGSM) {
                uint32_t bytes = op == OP_DCL_TGSM_RAW ? a0 : a0 * a1;
                if (bytes > 32768)
                    return fail(sh, "too much group shared memory");
                sh->tgsm_stride[slot] = op == OP_DCL_TGSM_STRUCTURED ? a0 : 0;
                sh->tgsm_size[slot] = bytes;
                sh->tgsm_off[slot] = sh->tgsm_bytes;
                sh->tgsm_bytes += (bytes + 15) & ~15u;
            }
        } else if (op == OP_DCL_OUTPUT || op == OP_DCL_OUTPUT_SIV || op == OP_DCL_OUTPUT_SGV) {
            struct opd o;
            bool bad = false;
            operand(p + 1, next, &o, &bad);
            if (o.type == T_OUTPUT && o.ndim >= 1)
                sh->max_out_reg = o.idx[0].imm > sh->max_out_reg ? o.idx[0].imm : sh->max_out_reg;
            if (o.type == T_ODEPTH || o.type == T_ODEPTH_GE || o.type == T_ODEPTH_LE)
                sh->depth_out = 1;
        } else if (!is_decl(op)) {
            if (sh->ncode == cap) {
                struct dxbc_ins *more = kmalloc(sizeof(struct dxbc_ins) * (size_t)cap * 2);
                if (!more)
                    return fail(sh, "out of memory");
                memcpy(more, sh->code, sizeof(struct dxbc_ins) * (size_t)cap);
                kfree(sh->code);
                sh->code = more;
                cap *= 2;
            }
            struct dxbc_ins *in = &sh->code[sh->ncode++];
            memset(in, 0, sizeof(*in));
            in->op = (uint16_t)op;
            in->sat = (uint8_t)((t >> 13) & 1);
            in->test_nz = (uint8_t)((t >> 18) & 1);
            in->ret_type = (uint8_t)((t >> 11) & 3);
            in->jump = -1;
            const uint32_t *q = p + 1;
            uint32_t e = t;
            while ((e & 0x80000000u) && q < next) { /* extended opcode tokens: texel offsets */
                e = *q++;
                if ((e & 0x3f) == 1)
                    for (int k = 0; k < 3; k++) {
                        int v = (int)((e >> (9 + 4 * k)) & 15);
                        in->offset[k] = (int8_t)(v >= 8 ? v - 16 : v);
                    }
            }
            bool bad = false;
            while (q < next && in->nops < MAX_OPS)
                q = operand(q, next, &in->o[in->nops++], &bad);
            if (bad)
                return fail(sh, "unsupported operand");
            if (op == OP_DISCARD)
                sh->discards = true;
            if (op == OP_SYNC && (t & (1u << 11)))
                sh->syncs = true;
            for (int k = 0; k < in->nops; k++)
                if (in->o[k].type == T_ODEPTH)
                    sh->depth_out = 1;
        }
        p = next;
    }
    for (int i = 0; i < sh->nin; i++) /* the signatures cover registers a shader reads without declaring */
        if ((int)sh->in[i].reg > sh->max_in_reg && sh->in[i].reg < 32)
            sh->max_in_reg = (int)sh->in[i].reg;
    for (int i = 0; i < sh->nout; i++)
        if ((int)sh->out[i].reg > sh->max_out_reg && sh->out[i].reg < 32)
            sh->max_out_reg = (int)sh->out[i].reg;
    if (!sh->temps)
        sh->temps = 1;
    if (sh->temps > MAX_TEMPS)
        return fail(sh, "too many temporaries");
    if (!link_flow(sh))
        return fail(sh, "unbalanced flow control");
    return true;
}

#include "gmath.h"

/* ---- the machine */

typedef uint32_t reg4[4][4]; /* [lane][component] */

struct machine {
    const struct dxbc_shader *sh;
    const struct dxbc_env *env;
    struct dxbc_lanes *lanes;
    reg4 *r;
    reg4 *x[8];
    unsigned alive; /* not returned, not discarded */
};

static bool int_op(int op)
{
    switch (op) {
    case OP_AND: case OP_OR: case OP_XOR: case OP_NOT: case OP_IADD: case OP_IEQ: case OP_IGE: case OP_ILT: case OP_IMAD:
    case OP_IMAX: case OP_IMIN: case OP_IMUL: case OP_INE: case OP_INEG: case OP_ISHL: case OP_ISHR: case OP_ITOF:
    case OP_UDIV: case OP_ULT: case OP_UGE: case OP_UMUL: case OP_UMAD: case OP_UMAX: case OP_UMIN: case OP_USHR:
    case OP_UTOF: case OP_COUNTBITS: case OP_FIRSTBIT_HI: case OP_FIRSTBIT_LO: case OP_FIRSTBIT_SHI: case OP_UBFE:
    case OP_IBFE: case OP_BFI: case OP_BFREV: case OP_UADDC: case OP_USUBB: case OP_F16TOF32: case OP_LD:
    case OP_SWITCH: case OP_CASE:
        return true;
    default: return false;
    }
}

static int32_t index_of(struct machine *m, const struct index *ix, int lane)
{
    int32_t v = ix->imm;
    if (ix->rel_type == T_TEMP && ix->rel_reg >= 0 && ix->rel_reg < m->sh->temps)
        v += (int32_t)m->r[ix->rel_reg][lane][ix->rel_comp];
    else if (ix->rel_type == T_XTEMP && ix->rel_reg >= 0 && ix->rel_reg < 8 && m->x[ix->rel_reg] &&
             ix->rel_reg2 >= 0 && ix->rel_reg2 < m->sh->xsize[ix->rel_reg])
        v += (int32_t)m->x[ix->rel_reg][ix->rel_reg2][lane][ix->rel_comp];
    else if (ix->rel_type == T_INPUT)
        v += (int32_t)m->lanes->v[ix->rel_reg][lane][ix->rel_comp];
    return v;
}

/* the four components of a source operand in one lane, swizzled, with its modifier */
static void read_src(struct machine *m, const struct opd *o, int lane, bool integer, uint32_t out[4])
{
    uint32_t raw[4] = {0, 0, 0, 0};
    const uint32_t *src = raw;
    switch (o->type) {
    case T_IMM32: src = o->imm; break;
    case T_TEMP: {
        int32_t i = o->idx[0].imm;
        if (i >= 0 && i < m->sh->temps)
            src = m->r[i][lane];
        break;
    }
    case T_INPUT: {
        if (o->ndim > 1 && m->lanes->gsv) { /* a geometry shader's v[vertex][register] */
            int32_t vx = index_of(m, &o->idx[0], lane), i = index_of(m, &o->idx[1], lane);
            if (vx >= 0 && vx < 6 && i >= 0 && i < m->lanes->gsv_regs)
                src = m->lanes->gsv + ((size_t)vx * (size_t)m->lanes->gsv_regs + (size_t)i) * 4;
            break;
        }
        int32_t i = index_of(m, &o->idx[o->ndim > 1 ? 1 : 0], lane);
        if (i >= 0 && i < 32)
            src = m->lanes->v[i][lane];
        break;
    }
    case T_OUTPUT: {
        int32_t i = index_of(m, &o->idx[0], lane);
        if (i >= 0 && i < 32)
            src = m->lanes->o[i][lane];
        break;
    }
    case T_XTEMP: {
        int32_t xr = o->idx[0].imm, i = index_of(m, &o->idx[1], lane);
        if (xr >= 0 && xr < 8 && m->x[xr] && i >= 0 && i < m->sh->xsize[xr])
            src = m->x[xr][i][lane];
        break;
    }
    case T_CB: {
        int32_t slot = o->idx[0].imm, i = index_of(m, &o->idx[1], lane);
        if (slot >= 0 && slot < DXBC_MAX_CB && m->env->cb[slot] && i >= 0 && (uint32_t)i < m->env->cb_vec4[slot])
            src = (const uint32_t *)m->env->cb[slot] + (size_t)i * 4;
        break;
    }
    case T_TID: src = m->lanes->tid[lane]; break;
    case T_PRIMID: raw[0] = raw[1] = raw[2] = raw[3] = m->lanes->prim_id; break;
    case T_GSINST: raw[0] = raw[1] = raw[2] = raw[3] = m->lanes->gs_instance; break;
    case T_GID: src = m->lanes->gid; break;
    case T_GTID: src = m->lanes->gtid[lane]; break;
    case T_FLAT:
        raw[0] = raw[1] = raw[2] = raw[3] = m->lanes->flat[lane];
        break;
    case T_ICB: {
        int32_t i = index_of(m, &o->idx[0], lane);
        if (m->sh->icb && i >= 0 && i < m->sh->icb_vec4)
            src = (const uint32_t *)m->sh->icb + (size_t)i * 4;
        break;
    }
    default: break;
    }
    for (int c = 0; c < 4; c++) {
        uint32_t v = src[o->swz[c]];
        if (o->mod) {
            if (integer) {
                int32_t s = (int32_t)v;
                if (o->mod & 2)
                    s = s < 0 ? -s : s;
                if (o->mod & 1)
                    s = -s;
                v = (uint32_t)s;
            } else {
                if (o->mod & 2)
                    v &= 0x7fffffffu;
                if (o->mod & 1)
                    v ^= 0x80000000u;
            }
        }
        out[c] = v;
    }
}

static uint32_t *dst_ptr(struct machine *m, const struct opd *o, int lane)
{
    switch (o->type) {
    case T_TEMP: {
        int32_t i = o->idx[0].imm;
        return i >= 0 && i < m->sh->temps ? m->r[i][lane] : NULL;
    }
    case T_OUTPUT: {
        int32_t i = index_of(m, &o->idx[0], lane);
        return i >= 0 && i < 32 ? m->lanes->o[i][lane] : NULL;
    }
    case T_XTEMP: {
        int32_t xr = o->idx[0].imm, i = index_of(m, &o->idx[1], lane);
        return xr >= 0 && xr < 8 && m->x[xr] && i >= 0 && i < m->sh->xsize[xr] ? m->x[xr][i][lane] : NULL;
    }
    default: return NULL;
    }
}

static void write_dst(struct machine *m, const struct dxbc_ins *in, const struct opd *o, unsigned mask,
                      uint32_t val[4][4])
{
    if (o->type == T_NULL)
        return;
    for (int l = 0; l < 4; l++) {
        if (!(mask & (1u << l)))
            continue;
        if (o->type == T_ODEPTH || o->type == T_ODEPTH_GE || o->type == T_ODEPTH_LE) {
            float f = F(val[l][0]);
            m->lanes->depth[l] = in->sat ? (f < 0 ? 0 : f > 1 ? 1 : f) : f;
            continue;
        }
        uint32_t *d = dst_ptr(m, o, l);
        if (!d)
            continue;
        for (int c = 0; c < 4; c++)
            if (o->mask & (1u << c)) {
                uint32_t v = val[l][c];
                if (in->sat) {
                    float f = F(v);
                    v = U(f != f ? 0 : f < 0 ? 0 : f > 1 ? 1 : f);
                }
                d[c] = v;
            }
    }
}

static bool test(const struct dxbc_ins *in, uint32_t v)
{
    return in->test_nz ? v != 0 : v == 0;
}

static unsigned cond_lanes(struct machine *m, const struct dxbc_ins *in, unsigned mask)
{
    unsigned out = 0;
    for (int l = 0; l < 4; l++) {
        uint32_t v[4];
        if (!(mask & (1u << l)))
            continue;
        read_src(m, &in->o[0], l, true, v);
        if (test(in, v[0]))
            out |= 1u << l;
    }
    return out;
}

static uint32_t firstbit_hi(uint32_t v)
{
    if (!v)
        return 0xffffffffu;
    uint32_t n = 0;
    while (!(v & 0x80000000u))
        v <<= 1, n++;
    return n;
}

static uint32_t firstbit_lo(uint32_t v)
{
    if (!v)
        return 0xffffffffu;
    uint32_t n = 0;
    while (!(v & 1))
        v >>= 1, n++;
    return n;
}

/* one component-wise operation */
static uint32_t alu(int op, const uint32_t *a, const uint32_t *b, const uint32_t *c, int k)
{
    float fa = F(a[k]), fb = b ? F(b[k]) : 0;
    switch (op) {
    case OP_ADD: return U(fa + fb);
    case OP_MUL: return U(fa * fb);
    case OP_MAD: return U(fa * fb + F(c[k]));
    case OP_DIV: return U(fa / fb);
    case OP_MIN: return U(fa < fb || fb != fb ? fa : fb);
    case OP_MAX: return U(fa > fb || fb != fb ? fa : fb);
    case OP_MOV: return a[k];
    case OP_MOVC: return a[k] ? b[k] : c[k];
    case OP_FRC: return U(fa - ffloor(fa));
    case OP_SQRT: return U(fsqrt(fa));
    case OP_RSQ: return U(1.0f / fsqrt(fa));
    case OP_RCP: return U(1.0f / fa);
    case OP_EXP: return U(fexp2(fa));
    case OP_LOG: return U(flog2(fa));
    case OP_ROUND_NE: return U(round_even(fa));
    case OP_ROUND_NI: return U(ffloor(fa));
    case OP_ROUND_PI: return U(-ffloor(-fa));
    case OP_ROUND_Z: return U(fa < 0 ? -ffloor(-fa) : ffloor(fa));
    case OP_EQ: return fa == fb ? 0xffffffffu : 0;
    case OP_NE: return fa != fb ? 0xffffffffu : 0;
    case OP_LT: return fa < fb ? 0xffffffffu : 0;
    case OP_GE: return fa >= fb ? 0xffffffffu : 0;
    case OP_FTOI: return (uint32_t)to_int(fa);
    case OP_FTOU: return to_uint(fa);
    case OP_ITOF: return U((float)(int32_t)a[k]);
    case OP_UTOF: return U((float)a[k]);
    case OP_AND: return a[k] & b[k];
    case OP_OR: return a[k] | b[k];
    case OP_XOR: return a[k] ^ b[k];
    case OP_NOT: return ~a[k];
    case OP_IADD: return a[k] + b[k];
    case OP_IMUL: return (uint32_t)((int32_t)a[k] * (int32_t)b[k]);
    case OP_UMUL: return a[k] * b[k];
    case OP_IMAD:
    case OP_UMAD: return a[k] * b[k] + c[k];
    case OP_INEG: return (uint32_t)-(int32_t)a[k];
    case OP_ISHL: return a[k] << (b[k] & 31);
    case OP_ISHR: return (uint32_t)((int32_t)a[k] >> (b[k] & 31));
    case OP_USHR: return a[k] >> (b[k] & 31);
    case OP_IMAX: return (int32_t)a[k] > (int32_t)b[k] ? a[k] : b[k];
    case OP_IMIN: return (int32_t)a[k] < (int32_t)b[k] ? a[k] : b[k];
    case OP_UMAX: return a[k] > b[k] ? a[k] : b[k];
    case OP_UMIN: return a[k] < b[k] ? a[k] : b[k];
    case OP_IEQ: return a[k] == b[k] ? 0xffffffffu : 0;
    case OP_INE: return a[k] != b[k] ? 0xffffffffu : 0;
    case OP_IGE: return (int32_t)a[k] >= (int32_t)b[k] ? 0xffffffffu : 0;
    case OP_ILT: return (int32_t)a[k] < (int32_t)b[k] ? 0xffffffffu : 0;
    case OP_UGE: return a[k] >= b[k] ? 0xffffffffu : 0;
    case OP_ULT: return a[k] < b[k] ? 0xffffffffu : 0;
    case OP_COUNTBITS: {
        uint32_t v = a[k], n = 0;
        while (v)
            n += v & 1, v >>= 1;
        return n;
    }
    case OP_FIRSTBIT_HI: return firstbit_hi(a[k]);
    case OP_FIRSTBIT_LO: return firstbit_lo(a[k]);
    case OP_FIRSTBIT_SHI: return firstbit_hi((int32_t)a[k] < 0 ? ~a[k] : a[k]);
    case OP_BFREV: {
        uint32_t v = a[k], r = 0;
        for (int i = 0; i < 32; i++)
            r = r << 1 | ((v >> i) & 1);
        return r;
    }
    case OP_F32TOF16: return f32_to_f16(fa);
    case OP_F16TOF32: return U(f16_to_f32(a[k] & 0xffffu));
    default: return 0;
    }
}

static void deriv(uint32_t val[4][4], uint32_t a[4][4], bool x, bool coarse)
{
    for (int l = 0; l < 4; l++)
        for (int c = 0; c < 4; c++) {
            int row = coarse ? 0 : l & 2, col = coarse ? 0 : l & 1;
            float d = x ? F(a[row | 1][c]) - F(a[row][c]) : F(a[2 | col][c]) - F(a[col][c]);
            val[l][c] = U(d);
        }
}

static void texture_op(struct machine *m, const struct dxbc_ins *in, unsigned mask, uint32_t val[4][4])
{
    const struct dxbc_env *env = m->env;
    const struct opd *res = NULL, *smp = NULL;
    for (int k = 1; k < in->nops; k++) {
        if (in->o[k].type == T_RESOURCE && !res)
            res = &in->o[k];
        else if (in->o[k].type == T_SAMPLER && !smp)
            smp = &in->o[k];
    }
    int tex = res ? res->idx[0].imm : 0, s = smp ? smp->idx[0].imm : 0;
    uint32_t a[4][4];
    for (int l = 0; l < 4; l++)
        read_src(m, &in->o[1], l, in->op == OP_LD || in->op == OP_RESINFO, a[l]);
    int offset[3] = {in->offset[0], in->offset[1], in->offset[2]};
    for (int l = 0; l < 4; l++) {
        uint32_t raw[4] = {0, 0, 0, 0};
        if (!(mask & (1u << l)))
            continue;
        if (in->op == OP_LD) {
            if (env->load)
                env->load(env->ctx, tex, (const int32_t *)a[l], offset, raw);
        } else if (in->op == OP_GATHER4 || in->op == OP_GATHER4_C) {
            float coord[4], out[4] = {0, 0, 0, 0}, ref = 0;
            for (int c = 0; c < 4; c++)
                coord[c] = F(a[l][c]);
            if (in->op == OP_GATHER4_C && in->nops > 4) {
                uint32_t r[4];
                read_src(m, &in->o[4], l, false, r);
                ref = F(r[0]);
            }
            if (env->gather)
                env->gather(env->ctx, tex, s, coord, offset, smp ? smp->swz[0] : 0, in->op == OP_GATHER4_C ? &ref : NULL,
                            out);
            for (int c = 0; c < 4; c++)
                raw[c] = U(out[c]);
        } else if (in->op == OP_RESINFO) {
            uint32_t info[4] = {0, 0, 0, 0};
            if (env->info)
                env->info(env->ctx, tex, a[l][0], info);
            for (int c = 0; c < 4; c++)
                raw[c] = in->ret_type == 2 ? info[c]
                         : in->ret_type == 1 && c < 3 ? U(info[c] ? 1.0f / (float)info[c] : 0)
                                                      : U((float)info[c]);
        } else {
            float coord[4], ddx[4] = {0, 0, 0, 0}, ddy[4] = {0, 0, 0, 0}, lod = -1e31f, bias = 0;
            for (int c = 0; c < 4; c++) {
                coord[c] = F(a[l][c]);
                int row = l & 2, col = l & 1;
                ddx[c] = F(a[row | 1][c]) - F(a[row][c]);
                ddy[c] = F(a[2 | col][c]) - F(a[col][c]);
            }
            uint32_t extra[4] = {0, 0, 0, 0};
            if (in->nops > 4)
                read_src(m, &in->o[4], l, false, extra);
            if (in->op == OP_SAMPLE_L)
                lod = F(extra[0]);
            else if (in->op == OP_SAMPLE_C_LZ)
                lod = 0;
            else if (in->op == OP_SAMPLE_B)
                bias = F(extra[0]);
            else if (in->op == OP_SAMPLE_D && in->nops > 5) {
                uint32_t dy[4];
                read_src(m, &in->o[5], l, false, dy);
                for (int c = 0; c < 4; c++)
                    ddx[c] = F(extra[c]), ddy[c] = F(dy[c]);
            }
            float out[4] = {0, 0, 0, 1};
            bool cmp = in->op == OP_SAMPLE_C || in->op == OP_SAMPLE_C_LZ;
            float ref = F(extra[0]);
            if (env->sample)
                env->sample(env->ctx, tex, s, coord, ddx, ddy, lod, bias, offset, cmp ? &ref : NULL, out);
            if (cmp)
                out[1] = out[2] = out[3] = out[0];
            for (int c = 0; c < 4; c++)
                raw[c] = U(out[c]);
        }
        for (int c = 0; c < 4; c++) /* the resource operand's swizzle picks the components */
            val[l][c] = res ? raw[res->swz[c]] : raw[c];
    }
}

/* ---- memory: raw and structured buffers, unordered access views, group shared memory */

/* the bytes behind u#, t# or g#, their count and the element stride (0: raw); kind: 0 typed, 1 raw, 2 structured */
static uint8_t *mem_of(struct machine *m, const struct opd *r, uint32_t *size, uint32_t *stride, int *kind)
{
    const struct dxbc_shader *sh = m->sh;
    int slot = r->idx[0].imm;
    *size = *stride = 0, *kind = 1;
    if (r->type == T_TGSM) {
        if (slot < 0 || slot >= DXBC_MAX_TGSM || !m->env->tgsm)
            return NULL;
        *size = sh->tgsm_size[slot], *stride = sh->tgsm_stride[slot];
        *kind = *stride ? 2 : 1;
        return m->env->tgsm + sh->tgsm_off[slot];
    }
    if (r->type == T_UAV) {
        if (slot < 0 || slot >= DXBC_MAX_UAV)
            return NULL;
        *kind = sh->uav_kind[slot], *stride = sh->uav_stride[slot];
        return *kind && m->env->buffer ? m->env->buffer(m->env->ctx, true, slot, size) : NULL;
    }
    if (r->type == T_RESOURCE) {
        if (slot < 0 || slot >= DXBC_MAX_RES)
            return NULL;
        *stride = sh->srv_stride[slot];
        *kind = *stride ? 2 : 1;
        return m->env->buffer ? m->env->buffer(m->env->ctx, false, slot, size) : NULL;
    }
    return NULL;
}

/* the 32-bit word an atomic works on, or NULL when the address is outside the memory */
static uint32_t *atomic_word(struct machine *m, const struct opd *r, const uint32_t addr[4])
{
    uint32_t size, stride;
    int kind;
    if (r->type == T_UAV && r->idx[0].imm >= 0 && r->idx[0].imm < DXBC_MAX_UAV && !m->sh->uav_kind[r->idx[0].imm])
        return m->env->uav_ptr ? m->env->uav_ptr(m->env->ctx, r->idx[0].imm, addr) : NULL;
    uint8_t *base = mem_of(m, r, &size, &stride, &kind);
    uint64_t at = kind == 2 ? (uint64_t)addr[0] * stride + addr[1] : addr[0];
    if (!base || (at & 3) || at + 4 > size)
        return NULL;
    return (uint32_t *)(base + at);
}

static uint32_t atomic_apply(int op, uint32_t *w, uint32_t v, uint32_t v2)
{
    uint32_t old = __atomic_load_n(w, __ATOMIC_RELAXED), want;
    switch (op) {
    case OP_ATOMIC_AND: case OP_IMM_ATOMIC_AND: return __atomic_fetch_and(w, v, __ATOMIC_ACQ_REL);
    case OP_ATOMIC_OR: case OP_IMM_ATOMIC_OR: return __atomic_fetch_or(w, v, __ATOMIC_ACQ_REL);
    case OP_ATOMIC_XOR: case OP_IMM_ATOMIC_XOR: return __atomic_fetch_xor(w, v, __ATOMIC_ACQ_REL);
    case OP_ATOMIC_IADD: case OP_IMM_ATOMIC_IADD: return __atomic_fetch_add(w, v, __ATOMIC_ACQ_REL);
    case OP_IMM_ATOMIC_EXCH: return __atomic_exchange_n(w, v, __ATOMIC_ACQ_REL);
    case OP_ATOMIC_CMP_STORE: case OP_IMM_ATOMIC_CMP_EXCH:
        want = v;
        __atomic_compare_exchange_n(w, &want, v2, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        return want;
    default: break;
    }
    do {
        switch (op) {
        case OP_ATOMIC_IMAX: case OP_IMM_ATOMIC_IMAX: want = (int32_t)v > (int32_t)old ? v : old; break;
        case OP_ATOMIC_IMIN: case OP_IMM_ATOMIC_IMIN: want = (int32_t)v < (int32_t)old ? v : old; break;
        case OP_ATOMIC_UMAX: case OP_IMM_ATOMIC_UMAX: want = v > old ? v : old; break;
        default: want = v < old ? v : old; break;
        }
    } while (!__atomic_compare_exchange_n(w, &old, want, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    return old;
}

/* loads, stores, atomics and bufinfo; false: not one of them */
static bool memory_op(struct machine *m, const struct dxbc_ins *in, unsigned mask, uint32_t val[4][4])
{
    int op = in->op;
    const struct dxbc_env *env = m->env;
    uint32_t a[4], b[4], c[4], size, stride;
    int kind;
    switch (op) {
    case OP_LD_RAW:
    case OP_LD_STRUCTURED: {
        const struct opd *r = &in->o[op == OP_LD_RAW ? 2 : 3];
        uint8_t *base = mem_of(m, r, &size, &stride, &kind);
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)))
                continue;
            read_src(m, &in->o[1], l, true, a);
            uint64_t at = a[0];
            if (op == OP_LD_STRUCTURED) {
                read_src(m, &in->o[2], l, true, b);
                at = (uint64_t)a[0] * stride + b[0];
            }
            uint32_t raw[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++)
                if (base && !(at & 3) && at + 4 * (uint64_t)k + 4 <= size)
                    raw[k] = *(const uint32_t *)(base + at + 4 * k);
            for (int k = 0; k < 4; k++)
                val[l][k] = raw[r->swz[k]];
        }
        return true;
    }
    case OP_STORE_RAW:
    case OP_STORE_STRUCTURED: {
        const struct opd *r = &in->o[0];
        uint8_t *base = mem_of(m, r, &size, &stride, &kind);
        if (!base)
            return true;
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)))
                continue;
            read_src(m, &in->o[1], l, true, a);
            uint64_t at = a[0];
            if (op == OP_STORE_STRUCTURED) {
                read_src(m, &in->o[2], l, true, b);
                at = (uint64_t)a[0] * stride + b[0];
            }
            read_src(m, &in->o[op == OP_STORE_RAW ? 2 : 3], l, true, c);
            for (int k = 0; k < 4; k++)
                if ((r->mask & (1u << k)) && !(at & 3) && at + 4 * (uint64_t)k + 4 <= size)
                    *(uint32_t *)(base + at + 4 * k) = c[k];
        }
        return true;
    }
    case OP_LD_UAV_TYPED: {
        const struct opd *r = &in->o[2];
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)))
                continue;
            uint32_t raw[4] = {0, 0, 0, 0};
            read_src(m, &in->o[1], l, true, a);
            if (env->uav_load)
                env->uav_load(env->ctx, r->idx[0].imm, a, raw);
            for (int k = 0; k < 4; k++)
                val[l][k] = raw[r->swz[k]];
        }
        return true;
    }
    case OP_STORE_UAV_TYPED:
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)) || !env->uav_store)
                continue;
            read_src(m, &in->o[1], l, true, a);
            read_src(m, &in->o[2], l, false, c);
            env->uav_store(env->ctx, in->o[0].idx[0].imm, a, c, in->o[0].mask);
        }
        return true;
    case OP_IMM_ATOMIC_ALLOC:
    case OP_IMM_ATOMIC_CONSUME: {
        uint32_t *n = env->uav_counter ? env->uav_counter(env->ctx, in->o[1].idx[0].imm) : NULL;
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)))
                continue;
            uint32_t v = 0;
            if (n)
                v = op == OP_IMM_ATOMIC_ALLOC ? __atomic_fetch_add(n, 1, __ATOMIC_ACQ_REL)
                                              : __atomic_sub_fetch(n, 1, __ATOMIC_ACQ_REL);
            val[l][0] = val[l][1] = val[l][2] = val[l][3] = v;
        }
        return true;
    }
    case OP_BUFINFO: {
        const struct opd *r = &in->o[1];
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)))
                continue;
            uint32_t info[4] = {0, 0, 0, 0};
            uint8_t *base = mem_of(m, r, &size, &stride, &kind);
            if (base || kind)
                info[0] = stride ? size / stride : size;
            else if (r->type == T_UAV && env->uav_info)
                env->uav_info(env->ctx, r->idx[0].imm, info);
            else if (r->type == T_RESOURCE && env->info)
                env->info(env->ctx, r->idx[0].imm, 0, info);
            for (int k = 0; k < 4; k++)
                val[l][k] = info[r->swz[k]];
        }
        return true;
    }
    default: break;
    }
    if (op >= OP_ATOMIC_AND && op <= OP_IMM_ATOMIC_UMIN) {
        bool imm = op >= OP_IMM_ATOMIC_IADD;
        int at = imm ? 1 : 0;
        bool two = op == OP_ATOMIC_CMP_STORE || op == OP_IMM_ATOMIC_CMP_EXCH;
        for (int l = 0; l < 4; l++) {
            if (!(mask & (1u << l)))
                continue;
            read_src(m, &in->o[at + 1], l, true, a);
            read_src(m, &in->o[at + 2], l, true, b);
            if (two)
                read_src(m, &in->o[at + 3], l, true, c);
            uint32_t *w = atomic_word(m, &in->o[at], a), old = 0;
            if (w)
                old = atomic_apply(op, w, b[0], two ? c[0] : 0);
            val[l][0] = val[l][1] = val[l][2] = val[l][3] = old;
        }
        return true;
    }
    return false;
}

/* the common operations on whole registers at once: four components in one SSE register; false: not one of them */
typedef float v4f __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));

static bool alu_vec(int op, uint32_t a[4][4], uint32_t b[4][4], uint32_t c[4][4], uint32_t val[4][4], unsigned mask)
{
#define EACH for (int l = 0; l < 4; l++) if (mask & (1u << l))
#define LOAD(v, t, src) t v; memcpy(&v, src[l], 16)
#define STORE(v) memcpy(val[l], &v, 16)
    switch (op) {
    case OP_ADD: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4f r = x + y; STORE(r); } return true;
    case OP_MUL: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4f r = x * y; STORE(r); } return true;
    case OP_MAD: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); LOAD(z, v4f, c); v4f r = x * y + z; STORE(r); } return true;
    case OP_DIV: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4f r = x / y; STORE(r); } return true;
    case OP_RCP: EACH { LOAD(x, v4f, a); v4f one = {1, 1, 1, 1}; v4f r = one / x; STORE(r); } return true;
    case OP_MOV: EACH memcpy(val[l], a[l], 16); return true;
    case OP_MOVC:
        EACH {
            LOAD(k, v4u, a); LOAD(y, v4u, b); LOAD(z, v4u, c);
            v4u m = (v4u)(k != 0), r = (y & m) | (z & ~m);
            STORE(r);
        }
        return true;
    case OP_MIN:
    case OP_MAX: /* the other operand when one is NaN */
        EACH {
            LOAD(x, v4f, a); LOAD(y, v4f, b);
            v4u m = (v4u)(op == OP_MIN ? x < y : x > y) | (v4u)(y != y), ux, uy;
            memcpy(&ux, &x, 16), memcpy(&uy, &y, 16);
            v4u r = (ux & m) | (uy & ~m);
            STORE(r);
        }
        return true;
    case OP_LT: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4i r = x < y; STORE(r); } return true;
    case OP_GE: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4i r = x >= y; STORE(r); } return true;
    case OP_EQ: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4i r = x == y; STORE(r); } return true;
    case OP_NE: EACH { LOAD(x, v4f, a); LOAD(y, v4f, b); v4i r = x != y; STORE(r); } return true;
    case OP_IADD: EACH { LOAD(x, v4u, a); LOAD(y, v4u, b); v4u r = x + y; STORE(r); } return true;
    case OP_AND: EACH { LOAD(x, v4u, a); LOAD(y, v4u, b); v4u r = x & y; STORE(r); } return true;
    case OP_OR: EACH { LOAD(x, v4u, a); LOAD(y, v4u, b); v4u r = x | y; STORE(r); } return true;
    case OP_XOR: EACH { LOAD(x, v4u, a); LOAD(y, v4u, b); v4u r = x ^ y; STORE(r); } return true;
    case OP_IEQ: EACH { LOAD(x, v4u, a); LOAD(y, v4u, b); v4i r = (v4i)(x == y); STORE(r); } return true;
    case OP_INE: EACH { LOAD(x, v4u, a); LOAD(y, v4u, b); v4i r = (v4i)(x != y); STORE(r); } return true;
    case OP_ILT: EACH { LOAD(x, v4i, a); LOAD(y, v4i, b); v4i r = x < y; STORE(r); } return true;
    case OP_IGE: EACH { LOAD(x, v4i, a); LOAD(y, v4i, b); v4i r = x >= y; STORE(r); } return true;
    case OP_ITOF: EACH { LOAD(x, v4i, a); v4f r = __builtin_convertvector(x, v4f); STORE(r); } return true;
    default: return false;
    }
#undef EACH
#undef LOAD
#undef STORE
}

enum { F_IF, F_LOOP, F_SWITCH };

struct frame {
    int kind;
    unsigned saved, else_mask, broken, cont, matched;
    uint32_t sel[4];
};

/* where a run stopped at a sync: at the start of the scratch memory */
struct run_state {
    int pc, depth, steps;
    unsigned mask, alive;
    struct frame stack[64];
};

#define STATE_BYTES ((sizeof(struct run_state) + 63) & ~(size_t)63)

size_t dxbc_scratch(const struct dxbc_shader *sh)
{
    size_t n = (size_t)sh->temps;
    for (int i = 0; i < 8; i++)
        if (sh->xtemps[i] && sh->xsize[i] > 0)
            n += (size_t)sh->xsize[i];
    return STATE_BYTES + sizeof(reg4) * n;
}

bool dxbc_run(const struct dxbc_shader *sh, const struct dxbc_env *env, struct dxbc_lanes *lanes)
{
    struct machine m = {sh, env, lanes, NULL, {0}, lanes->live & 15};
    size_t need = dxbc_scratch(sh);
    uint8_t *mem = lanes->scratch ? lanes->scratch : kmalloc(need);
    bool ok = mem != NULL;
    struct run_state *rs = NULL;
    bool resume = false;
    if (ok) {
        rs = (struct run_state *)mem;
        resume = lanes->resume && lanes->scratch;
        if (!resume)
            memset(mem, 0, need);
        m.r = (reg4 *)(mem + STATE_BYTES);
        size_t at = STATE_BYTES + sizeof(reg4) * (size_t)sh->temps;
        for (int i = 0; i < 8; i++)
            if (sh->xtemps[i] && sh->xsize[i] > 0) {
                m.x[i] = (reg4 *)(mem + at);
                at += sizeof(reg4) * (size_t)sh->xsize[i];
            }
    }
    struct frame stack[64];
    int depth = 0, steps = 0, start = 0;
    unsigned mask = m.alive;
    lanes->error[0] = 0;
    lanes->paused = false;
    if (resume) {
        start = rs->pc, depth = rs->depth, steps = rs->steps, mask = rs->mask, m.alive = rs->alive;
        memcpy(stack, rs->stack, sizeof(struct frame) * (size_t)depth);
    } else {
        for (int l = 0; l < 4; l++)
            lanes->depth[l] = 0;
    }
    uint32_t a[4][4], b[4][4], c[4][4], val[4][4], val2[4][4];
    for (int pc = start; ok && pc < sh->ncode; pc++) {
        if (++steps > 1 << 22) {
            const char *why = "too many steps";
            int i = 0;
            for (; why[i]; i++)
                lanes->error[i] = why[i];
            lanes->error[i] = 0;
            ok = false;
            break;
        }
        const struct dxbc_ins *in = &sh->code[pc];
        unsigned exited = 0;
        for (int k = 0; k < depth; k++)
            exited |= stack[k].broken | stack[k].cont;
        switch (in->op) {
        case OP_IF: {
            if (depth == 64)
                goto unsupported;
            unsigned cond = cond_lanes(&m, in, mask);
            stack[depth++] = (struct frame){F_IF, mask, mask & ~cond, 0, 0, 0, {0}};
            mask &= cond;
            if (!mask) { /* nobody takes the branch: straight to else or endif */
                pc = in->jump - 1;
            }
            continue;
        }
        case OP_ELSE: {
            struct frame *f = &stack[depth - 1];
            mask = f->else_mask & m.alive & ~exited;
            if (!mask)
                pc = in->jump - 1;
            continue;
        }
        case OP_ENDIF: {
            struct frame *f = &stack[--depth];
            exited = 0;
            for (int k = 0; k < depth; k++)
                exited |= stack[k].broken | stack[k].cont;
            mask = f->saved & m.alive & ~exited;
            continue;
        }
        case OP_LOOP:
            if (depth == 64)
                goto unsupported;
            stack[depth++] = (struct frame){F_LOOP, mask, 0, 0, 0, 0, {0}};
            if (!mask)
                pc = in->jump - 1;
            continue;
        case OP_ENDLOOP: {
            struct frame *f = &stack[depth - 1];
            f->cont = 0;
            exited = 0;
            for (int k = 0; k < depth - 1; k++)
                exited |= stack[k].broken | stack[k].cont;
            mask = f->saved & ~f->broken & m.alive & ~exited;
            if (mask) {
                pc = in->jump; /* the loop's first instruction comes next */
            } else {
                depth--;
                mask = f->saved & m.alive & ~exited;
            }
            continue;
        }
        case OP_BREAK:
        case OP_BREAKC:
        case OP_CONTINUE:
        case OP_CONTINUEC: {
            unsigned who = in->op == OP_BREAK || in->op == OP_CONTINUE ? mask : cond_lanes(&m, in, mask);
            bool cont = in->op == OP_CONTINUE || in->op == OP_CONTINUEC;
            for (int k = depth - 1; k >= 0; k--)
                if (stack[k].kind == F_LOOP || (!cont && stack[k].kind == F_SWITCH)) {
                    if (cont)
                        stack[k].cont |= who;
                    else
                        stack[k].broken |= who;
                    break;
                }
            mask &= ~who;
            continue;
        }
        case OP_SWITCH: {
            if (depth == 64)
                goto unsupported;
            struct frame f = {F_SWITCH, mask, 0, 0, 0, 0, {0}};
            for (int l = 0; l < 4; l++) {
                uint32_t v[4];
                read_src(&m, &in->o[0], l, true, v);
                f.sel[l] = v[0];
            }
            stack[depth++] = f;
            mask = 0;
            continue;
        }
        case OP_CASE:
        case OP_DEFAULT: {
            struct frame *f = &stack[depth - 1];
            unsigned take = 0;
            for (int l = 0; l < 4; l++) {
                if (!(f->saved & (1u << l)))
                    continue;
                bool hit;
                if (in->op == OP_CASE) {
                    hit = f->sel[l] == in->o[0].imm[0];
                } else { /* default: no case of this switch matches */
                    hit = true;
                    for (int k = in->jump + 1; k < sh->ncode && sh->code[k].op != OP_ENDSWITCH; k++)
                        if (sh->code[k].op == OP_CASE && sh->code[k].jump == in->jump && sh->code[k].o[0].imm[0] == f->sel[l])
                            hit = false;
                }
                if (hit)
                    take |= 1u << l;
            }
            mask |= take & m.alive & ~f->broken; /* lanes already running fall through */
            continue;
        }
        case OP_ENDSWITCH: {
            struct frame *f = &stack[--depth];
            exited = 0;
            for (int k = 0; k < depth; k++)
                exited |= stack[k].broken | stack[k].cont;
            mask = f->saved & m.alive & ~exited;
            continue;
        }
        case OP_RET:
        case OP_RETC: {
            unsigned who = in->op == OP_RET ? mask : cond_lanes(&m, in, mask);
            m.alive &= ~who;
            mask &= ~who;
            if (!m.alive)
                pc = sh->ncode;
            continue;
        }
        case OP_DISCARD: {
            unsigned who = cond_lanes(&m, in, mask);
            m.alive &= ~who;
            mask &= ~who;
            lanes->live &= ~who;
            continue;
        }
        case OP_NOP:
            continue;
        case OP_EMIT: case OP_CUT: case OP_EMITTHENCUT:
        case OP_EMIT_STREAM: case OP_CUT_STREAM: case OP_EMITTHENCUT_STREAM: {
            bool e = in->op != OP_CUT && in->op != OP_CUT_STREAM;
            bool cut = in->op != OP_EMIT && in->op != OP_EMIT_STREAM;
            if (lanes->emit && (in->op < OP_EMIT_STREAM || in->o[0].idx[0].imm == 0)) /* stream 0 is drawn */
                for (int l = 0; l < 4; l++)
                    if (mask & (1u << l))
                        lanes->emit(lanes->user, lanes, l, e, cut);
            continue;
        }
        case OP_SYNC:
            if ((in->ret_type & 1) && lanes->scratch) { /* the other threads of the group run up to here first */
                rs->pc = pc + 1, rs->depth = depth, rs->steps = steps, rs->mask = mask, rs->alive = m.alive;
                memcpy(rs->stack, stack, sizeof(struct frame) * (size_t)depth);
                lanes->paused = true;
                return true;
            }
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            continue;
        case OP_CALL:
        case OP_CALLC:
            goto unsupported;
        default: break;
        }
        if (!mask && in->op != OP_DERIV_RTX && in->op != OP_DERIV_RTY)
            continue;
        int op = in->op;
        bool integer = int_op(op);
        /* sources: dot products, texture and the two-result operations read their own way */
        switch (op) {
        case OP_DP2:
        case OP_DP3:
        case OP_DP4: {
            int n = op == OP_DP2 ? 2 : op == OP_DP3 ? 3 : 4;
            for (int l = 0; l < 4; l++) {
                read_src(&m, &in->o[1], l, false, a[l]);
                read_src(&m, &in->o[2], l, false, b[l]);
                float s = 0;
                for (int k = 0; k < n; k++)
                    s += F(a[l][k]) * F(b[l][k]);
                for (int k = 0; k < 4; k++)
                    val[l][k] = U(s);
            }
            write_dst(&m, in, &in->o[0], mask, val);
            break;
        }
        case OP_SINCOS:
            for (int l = 0; l < 4; l++) {
                read_src(&m, &in->o[2], l, false, a[l]);
                for (int k = 0; k < 4; k++) {
                    val[l][k] = U(fsin(F(a[l][k])));
                    val2[l][k] = U(fcos(F(a[l][k])));
                }
            }
            write_dst(&m, in, &in->o[0], mask, val);
            write_dst(&m, in, &in->o[1], mask, val2);
            break;
        case OP_UDIV:
        case OP_IMUL:
        case OP_UMUL:
        case OP_UADDC:
        case OP_USUBB:
        case OP_SWAPC:
            if (in->nops < 4)
                goto unsupported;
            for (int l = 0; l < 4; l++) {
                read_src(&m, &in->o[2], l, op != OP_SWAPC, a[l]);
                read_src(&m, &in->o[3], l, true, b[l]);
                if (op == OP_SWAPC)
                    read_src(&m, &in->o[4], l, true, c[l]);
                for (int k = 0; k < 4; k++) {
                    uint32_t x = a[l][k], y = b[l][k];
                    switch (op) {
                    case OP_UDIV:
                        val[l][k] = y ? x / y : 0xffffffffu;
                        val2[l][k] = y ? x % y : 0xffffffffu;
                        break;
                    case OP_IMUL: {
                        int64_t p = (int64_t)(int32_t)x * (int64_t)(int32_t)y;
                        val[l][k] = (uint32_t)((uint64_t)p >> 32), val2[l][k] = (uint32_t)p;
                        break;
                    }
                    case OP_UMUL: {
                        uint64_t p = (uint64_t)x * y;
                        val[l][k] = (uint32_t)(p >> 32), val2[l][k] = (uint32_t)p;
                        break;
                    }
                    case OP_UADDC: val[l][k] = x + y, val2[l][k] = x + y < x; break;
                    case OP_USUBB: val[l][k] = x - y, val2[l][k] = x < y; break;
                    default: /* swapc d0, d1, cond, a, b */
                        val[l][k] = x ? c[l][k] : y;
                        val2[l][k] = x ? y : c[l][k];
                        break;
                    }
                }
            }
            write_dst(&m, in, &in->o[0], mask, val);
            write_dst(&m, in, &in->o[1], mask, val2);
            break;
        case OP_UBFE:
        case OP_IBFE:
        case OP_BFI:
            for (int l = 0; l < 4; l++) {
                read_src(&m, &in->o[1], l, true, a[l]);
                read_src(&m, &in->o[2], l, true, b[l]);
                read_src(&m, &in->o[3], l, true, c[l]);
                uint32_t d[4] = {0, 0, 0, 0};
                if (op == OP_BFI && in->nops > 4)
                    read_src(&m, &in->o[4], l, true, d);
                for (int k = 0; k < 4; k++) {
                    uint32_t width = a[l][k] & 31, off = b[l][k] & 31;
                    if (op == OP_BFI) {
                        uint32_t bits = ((1u << width) - 1) << off;
                        val[l][k] = ((c[l][k] << off) & bits) | (d[k] & ~bits);
                    } else if (!width) {
                        val[l][k] = 0;
                    } else if (width + off < 32) {
                        uint32_t v = c[l][k] << (32 - width - off);
                        val[l][k] = op == OP_IBFE ? (uint32_t)((int32_t)v >> (32 - width)) : v >> (32 - width);
                    } else {
                        val[l][k] = op == OP_IBFE ? (uint32_t)((int32_t)c[l][k] >> off) : c[l][k] >> off;
                    }
                }
            }
            write_dst(&m, in, &in->o[0], mask, val);
            break;
        case OP_DERIV_RTX:
        case OP_DERIV_RTY:
        case OP_DERIV_RTX_COARSE:
        case OP_DERIV_RTX_FINE:
        case OP_DERIV_RTY_COARSE:
        case OP_DERIV_RTY_FINE:
            for (int l = 0; l < 4; l++)
                read_src(&m, &in->o[1], l, false, a[l]);
            deriv(val, a, op == OP_DERIV_RTX || op == OP_DERIV_RTX_COARSE || op == OP_DERIV_RTX_FINE,
                  op == OP_DERIV_RTX_COARSE || op == OP_DERIV_RTY_COARSE);
            write_dst(&m, in, &in->o[0], mask, val);
            break;
        case OP_SAMPLE:
        case OP_SAMPLE_C:
        case OP_SAMPLE_C_LZ:
        case OP_SAMPLE_L:
        case OP_SAMPLE_D:
        case OP_SAMPLE_B:
        case OP_LD:
        case OP_RESINFO:
        case OP_GATHER4:
        case OP_GATHER4_C:
            texture_op(&m, in, mask, val);
            write_dst(&m, in, &in->o[0], mask, val);
            break;
        default: {
            if (memory_op(&m, in, mask, val)) {
                bool writes = op == OP_LD_RAW || op == OP_LD_STRUCTURED || op == OP_LD_UAV_TYPED || op == OP_BUFINFO ||
                              (op >= OP_IMM_ATOMIC_ALLOC && op <= OP_IMM_ATOMIC_UMIN);
                if (writes)
                    write_dst(&m, in, &in->o[0], mask, val);
                break;
            }
            int nsrc = in->nops - 1;
            if (nsrc < 1 || nsrc > 3 || op >= OP_COUNT)
                goto unsupported;
            switch (op) {
            case OP_ADD: case OP_MUL: case OP_MAD: case OP_DIV: case OP_MIN: case OP_MAX: case OP_MOV: case OP_MOVC:
            case OP_FRC: case OP_SQRT: case OP_RSQ: case OP_RCP: case OP_EXP: case OP_LOG: case OP_ROUND_NE:
            case OP_ROUND_NI: case OP_ROUND_PI: case OP_ROUND_Z: case OP_EQ: case OP_NE: case OP_LT: case OP_GE:
            case OP_FTOI: case OP_FTOU: case OP_ITOF: case OP_UTOF: case OP_AND: case OP_OR: case OP_XOR: case OP_NOT:
            case OP_IADD: case OP_IMAD: case OP_UMAD: case OP_INEG: case OP_ISHL: case OP_ISHR: case OP_USHR:
            case OP_IMAX: case OP_IMIN: case OP_UMAX: case OP_UMIN: case OP_IEQ: case OP_INE: case OP_IGE: case OP_ILT:
            case OP_UGE: case OP_ULT: case OP_COUNTBITS: case OP_FIRSTBIT_HI: case OP_FIRSTBIT_LO:
            case OP_FIRSTBIT_SHI: case OP_BFREV: case OP_F32TOF16: case OP_F16TOF32:
                break;
            default: goto unsupported;
            }
            for (int l = 0; l < 4; l++) {
                if (!(mask & (1u << l)))
                    continue;
                /* movc: the condition is bits; mov: copies bits (a float modifier still applies) */
                read_src(&m, &in->o[1], l, op == OP_MOVC ? true : integer, a[l]);
                if (nsrc > 1)
                    read_src(&m, &in->o[2], l, integer, b[l]);
                if (nsrc > 2)
                    read_src(&m, &in->o[3], l, integer, c[l]);
            }
            if (!alu_vec(op, a, b, c, val, mask))
                for (int l = 0; l < 4; l++)
                    if (mask & (1u << l))
                        for (int k = 0; k < 4; k++)
                            val[l][k] = alu(op, a[l], nsrc > 1 ? b[l] : NULL, nsrc > 2 ? c[l] : NULL, k);
            write_dst(&m, in, &in->o[0], mask, val);
            break;
        }
        }
        continue;
    unsupported : {
        static const char hex[] = "0123456789abcdef";
        const char *why = "unsupported instruction ";
        int i = 0;
        for (; why[i]; i++)
            lanes->error[i] = why[i];
        lanes->error[i++] = hex[(in->op >> 8) & 15];
        lanes->error[i++] = hex[(in->op >> 4) & 15];
        lanes->error[i++] = hex[in->op & 15];
        lanes->error[i] = 0;
        ok = false;
        break;
    }
    }
    if (!m.r || !ok) {
        if (!lanes->error[0]) {
            const char *why = "out of memory";
            int i = 0;
            for (; why[i]; i++)
                lanes->error[i] = why[i];
            lanes->error[i] = 0;
        }
        ok = false;
    }
    if (!lanes->scratch)
        kfree(mem);
    return ok;
}

#endif
