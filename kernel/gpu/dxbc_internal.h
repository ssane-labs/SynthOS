#ifndef SYNTH_GPU_DXBC_INTERNAL_H
#define SYNTH_GPU_DXBC_INTERNAL_H

/* What the DXBC interpreter (dxbc_fp.c) and the shader compiler (dxbc_jit_fp.c) share: opcodes, operand types and
   decoded instructions. */

#include <synth/dxbc.h>

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
    OP_EVAL_SNAPPED = 203, OP_EVAL_SAMPLE_INDEX, OP_EVAL_CENTROID,
    OP_COUNT = 256
};

enum {
    T_TEMP, T_INPUT, T_OUTPUT, T_XTEMP, T_IMM32, T_IMM64, T_SAMPLER, T_RESOURCE, T_CB, T_ICB, T_LABEL, T_PRIMID,
    T_ODEPTH, T_NULL, T_RASTERIZER, T_OMASK,
    T_OCPID = 22, T_FORKID, T_JOINID, T_ICP, T_OCP, T_PC, T_DOMAIN,
    T_UAV = 30, T_TGSM, T_TID, T_GID, T_GTID, T_COVERAGE, T_FLAT = 36, T_GSINST = 37,
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

typedef uint32_t reg4[4][4]; /* [lane][component] */

/* Straight runs of arithmetic compiled to x86-64 code: fn[pc] runs the instructions pc..end[pc]-1 on the lanes in
   mask, or is NULL where the interpreter carries on. */
typedef void (*dxbc_block_fn)(reg4 *r, struct dxbc_lanes *lanes, const struct dxbc_env *env, unsigned mask);

struct dxbc_jit {
    dxbc_block_fn *fn;
    int32_t *end;
    uint8_t *mem;
    size_t size;
    int blocks, compiled;
};

void dxbc_jit_compile(struct dxbc_shader *sh);
/* the interpreter's arithmetic on one lane, and whether an operation reads integers */
void dxbc_alu4(int op, const uint32_t a[4], const uint32_t b[4], const uint32_t c[4], int nsrc, uint32_t out[4]);
bool dxbc_int_op(int op);
void dxbc_jit_free(struct dxbc_shader *sh);

#endif
