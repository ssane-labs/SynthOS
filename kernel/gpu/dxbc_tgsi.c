/* DXBC to TGSI: a Shader Model 4/5 vertex or pixel shader as the text form of Gallium's shader language, which the
   host's virgl renderer takes (virtio-gpu 3D). Registers keep their numbers: r# are TEMP[#], v# IN[#], o# OUT[#],
   cb#[i] the uniform buffer CONST[#+1][i], t#/s# SVIEW[#]/SAMP[#]. Stages are linked by register, as Direct3D 11
   links them: what a vertex shader writes to o3 a pixel shader reads in v3, both GENERIC[3]. What has no TGSI form
   here (subroutines, unordered access, the other stages) fails with a reason, and the caller draws in software. */

#if defined(__x86_64__) || defined(RS_HOST)

#include "dxbc_internal.h"

#ifdef RS_HOST
#include <stdlib.h>
#include <string.h>
#define kmalloc malloc
#define kfree free
#else
#include <synth/heap.h>
#include <synth/string.h>
#endif

#define MAX_IMM 1024

struct text {
    char *p;
    size_t len, cap;
    bool full;
};

struct tg {
    const struct dxbc_shader *sh;
    struct dxbc_tgsi *out;
    struct text code;
    uint32_t imm[MAX_IMM][4];
    int nimm;
    int xbase[8];          /* x# starts at TEMP[xbase] (indexable temps follow the r#) */
    int s0;                /* scratch temps: TEMP[s0..s0+3] */
    int depth_reg;         /* OUT[] for oDepth */
    int8_t sv_in[32];      /* vertex shader inputs that are system values: SV[n], or -1 */
    int nsv;
    uint8_t sv_name[32];
    uint32_t used_cb, used_tex, cmp_tex;
    int naddr;
    bool fail;
};

/* ---- text */

static void put(struct text *t, const char *s)
{
    size_t n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->len + n + 1)
            cap *= 2;
        char *p = kmalloc(cap);
        if (!p) {
            t->full = true;
            return;
        }
        if (t->p)
            memcpy(p, t->p, t->len);
        kfree(t->p);
        t->p = p, t->cap = cap;
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = 0;
}

static void putu(struct text *t, uint32_t v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do
        b[--i] = (char)('0' + v % 10), v /= 10;
    while (v);
    put(t, b + i);
}

static void puti(struct text *t, int32_t v)
{
    if (v < 0) {
        put(t, "-");
        putu(t, (uint32_t)-(int64_t)v);
    } else {
        putu(t, (uint32_t)v);
    }
}

static bool fail(struct tg *g, const char *why)
{
    if (!g->fail) {
        size_t n = strlen(why);
        n = n < sizeof(g->out->error) - 1 ? n : sizeof(g->out->error) - 1;
        memcpy(g->out->error, why, n);
        g->out->error[n] = 0;
    }
    g->fail = true;
    return false;
}

/* ---- operands */

static int immediate(struct tg *g, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    for (int i = g->sh->icb_vec4; i < g->nimm; i++)
        if (g->imm[i][0] == a && g->imm[i][1] == b && g->imm[i][2] == c && g->imm[i][3] == d)
            return i;
    if (g->nimm == MAX_IMM)
        return fail(g, "too many immediates"), 0;
    g->imm[g->nimm][0] = a, g->imm[g->nimm][1] = b, g->imm[g->nimm][2] = c, g->imm[g->nimm][3] = d;
    return g->nimm++;
}

/* a register made by the translator: TEMP[n] with a swizzle */
static struct opd temp(int n, const char *swz)
{
    struct opd o;
    memset(&o, 0, sizeof(o));
    o.type = T_TEMP, o.ndim = 1, o.mask = 15, o.idx[0].imm = n;
    for (int i = 0; i < 3; i++)
        o.idx[i].rel_type = -1;
    for (int i = 0; i < 4; i++)
        o.swz[i] = (uint8_t)(swz[i] == 'x' ? 0 : swz[i] == 'y' ? 1 : swz[i] == 'z' ? 2 : 3);
    return o;
}

static struct opd imm_opd(uint32_t v)
{
    struct opd o = temp(0, "xyzw");
    o.type = T_IMM32, o.ndim = 0;
    o.imm[0] = o.imm[1] = o.imm[2] = o.imm[3] = v;
    return o;
}

static struct opd scalar_of(const struct opd *o, int c) /* one component of a source, replicated */
{
    struct opd r = *o;
    for (int i = 0; i < 4; i++)
        r.swz[i] = o->swz[c];
    return r;
}

/* relative indices: UARL ADDR[k].x, the index register's component, before the instruction */
static int addr_reg[MAX_OPS][3];

static void load_addr(struct tg *g, const struct opd *o, int slot)
{
    for (int d = 0; d < o->ndim && d < 3; d++) {
        addr_reg[slot][d] = -1;
        if (o->idx[d].rel_type < 0)
            continue;
        if (o->idx[d].rel_type != T_TEMP && o->idx[d].rel_type != T_INPUT) {
            fail(g, "relative index from this register");
            return;
        }
        if (g->naddr >= 3) {
            fail(g, "more than three relative indices");
            return;
        }
        int k = g->naddr++;
        addr_reg[slot][d] = k;
        put(&g->code, "UARL ADDR[");
        putu(&g->code, (uint32_t)k);
        put(&g->code, "].x, ");
        put(&g->code, o->idx[d].rel_type == T_TEMP ? "TEMP[" : "IN[");
        puti(&g->code, o->idx[d].rel_reg);
        put(&g->code, "].");
        char c[5] = {"xyzw"[o->idx[d].rel_comp & 3], "xyzw"[o->idx[d].rel_comp & 3], "xyzw"[o->idx[d].rel_comp & 3],
                     "xyzw"[o->idx[d].rel_comp & 3], 0};
        put(&g->code, c);
        put(&g->code, "\n");
    }
}

static void index_text(struct tg *g, const struct opd *o, int d, int slot, int base)
{
    put(&g->code, "[");
    if (slot >= 0 && addr_reg[slot][d] >= 0) {
        put(&g->code, "ADDR[");
        putu(&g->code, (uint32_t)addr_reg[slot][d]);
        put(&g->code, "].x+");
    }
    puti(&g->code, o->idx[d].imm + base);
    put(&g->code, "]");
}

/* the register of an operand, without swizzle or modifiers; slot: its place in the instruction (relative indices) */
static void reg_text(struct tg *g, const struct opd *o, int slot)
{
    struct text *t = &g->code;
    switch (o->type) {
    case T_TEMP:
        put(t, "TEMP"), index_text(g, o, 0, slot, 0);
        break;
    case T_INPUT:
        if (o->ndim != 1)
            fail(g, "input arrays");
        if (g->sh->stage == DXBC_VS && o->idx[0].imm >= 0 && o->idx[0].imm < 32 && g->sv_in[o->idx[0].imm] >= 0) {
            put(t, "SV[");
            putu(t, (uint32_t)g->sv_in[o->idx[0].imm]);
            put(t, "]");
        } else {
            put(t, "IN"), index_text(g, o, 0, slot, 0);
        }
        break;
    case T_OUTPUT:
        put(t, "OUT"), index_text(g, o, 0, slot, 0);
        break;
    case T_ODEPTH:
        put(t, "OUT[");
        putu(t, (uint32_t)g->depth_reg);
        put(t, "]");
        break;
    case T_XTEMP: {
        int x = o->idx[0].imm;
        if (x < 0 || x >= 8 || !g->sh->xtemps[x]) {
            fail(g, "undeclared indexable temp");
            break;
        }
        put(t, "TEMP"), index_text(g, o, 1, slot, g->xbase[x]);
        put(t, "(");
        putu(t, (uint32_t)x + 1);
        put(t, ")");
        break;
    }
    case T_CB: {
        int s = o->idx[0].imm;
        if (s < 0 || s >= DXBC_MAX_CB || o->ndim < 2) {
            fail(g, "constant buffer slot");
            break;
        }
        g->used_cb |= 1u << s;
        put(t, "CONST[");
        putu(t, (uint32_t)s + 1);
        put(t, "]");
        index_text(g, o, 1, slot, 0);
        break;
    }
    case T_ICB:
        if (o->idx[0].rel_type >= 0)
            fail(g, "relative index into the immediate constant buffer");
        if (o->idx[0].imm < 0 || o->idx[0].imm >= g->sh->icb_vec4)
            fail(g, "immediate constant buffer index");
        put(t, "IMM[");
        puti(t, o->idx[0].imm);
        put(t, "]");
        break;
    case T_IMM32:
        put(t, "IMM[");
        putu(t, (uint32_t)immediate(g, o->imm[0], o->imm[1], o->imm[2], o->imm[3]));
        put(t, "]");
        break;
    default:
        fail(g, "a register type TGSI has no form for here");
    }
}

static void src_text(struct tg *g, const struct opd *o, int slot, bool first)
{
    struct text *t = &g->code;
    put(t, first ? " " : ", ");
    if (o->mod & 1)
        put(t, "-");
    if (o->mod & 2)
        put(t, "|");
    reg_text(g, o, slot);
    char s[6] = {'.', "xyzw"[o->swz[0] & 3], "xyzw"[o->swz[1] & 3], "xyzw"[o->swz[2] & 3], "xyzw"[o->swz[3] & 3], 0};
    put(t, s);
    if (o->mod & 2)
        put(t, "|");
}

static void dst_text(struct tg *g, const struct opd *o, int slot)
{
    struct text *t = &g->code;
    put(t, " ");
    reg_text(g, o, slot);
    unsigned m = o->type == T_ODEPTH ? 4 : o->mask;
    if (m != 15) {
        char s[6];
        int n = 0;
        s[n++] = '.';
        for (int c = 0; c < 4; c++)
            if (m & (1u << c))
                s[n++] = "xyzw"[c];
        s[n] = 0;
        put(t, s);
    }
}

/* one instruction: name, then a destination (or none) and the sources */
static void emit(struct tg *g, const char *name, bool sat, const struct opd *dst, const struct opd *const *src,
                 int nsrc)
{
    g->naddr = 0;
    if (dst)
        load_addr(g, dst, 0);
    for (int i = 0; i < nsrc; i++)
        load_addr(g, src[i], i + 1);
    put(&g->code, name);
    if (sat)
        put(&g->code, "_SAT");
    if (dst)
        dst_text(g, dst, 0);
    for (int i = 0; i < nsrc; i++)
        src_text(g, src[i], i + 1, !dst && !i);
    put(&g->code, "\n");
}

static void emit1(struct tg *g, const char *name, bool sat, const struct opd *d, const struct opd *a)
{
    const struct opd *s[1] = {a};
    emit(g, name, sat, d, s, 1);
}

static void emit2(struct tg *g, const char *name, const struct opd *d, const struct opd *a, const struct opd *b)
{
    const struct opd *s[2] = {a, b};
    emit(g, name, false, d, s, 2);
}

static void emit3(struct tg *g, const char *name, const struct opd *d, const struct opd *a, const struct opd *b,
                  const struct opd *c)
{
    const struct opd *s[3] = {a, b, c};
    emit(g, name, false, d, s, 3);
}

static void line(struct tg *g, const char *s)
{
    put(&g->code, s);
    put(&g->code, "\n");
}

/* a scratch register with the destination's mask */
static struct opd scratch_dst(struct tg *g, int k, unsigned mask)
{
    struct opd o = temp(g->s0 + k, "xyzw");
    o.mask = (uint8_t)mask;
    return o;
}

/* operations TGSI does on one component (RCP, EX2, SIN...): one instruction per component written */
static void per_component(struct tg *g, const char *name, const struct dxbc_ins *in, const struct opd *dst,
                          const struct opd *src)
{
    if (dst->type == T_NULL)
        return;
    unsigned m = dst->type == T_ODEPTH ? 1 : dst->mask;
    struct opd tmp = scratch_dst(g, 0, m);
    for (int c = 0; c < 4; c++) {
        if (!(m & (1u << c)))
            continue;
        struct opd one = scalar_of(src, c), d = tmp;
        d.mask = (uint8_t)(1u << c);
        emit1(g, name, false, &d, &one);
    }
    struct opd from = temp(g->s0, "xyzw");
    if (dst->type == T_ODEPTH)
        from = temp(g->s0, "xxxx");
    emit1(g, "MOV", in->sat, dst, &from);
}

static void test_cond(struct tg *g, const struct dxbc_ins *in, const struct opd *cond) /* UIF on the condition */
{
    if (in->test_nz) {
        emit(g, "UIF", false, NULL, (const struct opd *const[]){cond}, 1);
        return;
    }
    struct opd d = scratch_dst(g, 0, 1), zero = imm_opd(0), r = temp(g->s0, "xxxx");
    emit2(g, "USEQ", &d, cond, &zero);
    emit(g, "UIF", false, NULL, (const struct opd *const[]){&r}, 1);
}

/* the masked shift amount Direct3D uses */
static struct opd shift_amount(struct tg *g, const struct opd *b)
{
    if (b->type == T_IMM32 && !b->mod) {
        struct opd r = *b;
        for (int i = 0; i < 4; i++)
            r.imm[i] &= 31;
        return r;
    }
    struct opd d = scratch_dst(g, 1, 15), mask = imm_opd(31);
    emit2(g, "AND", &d, b, &mask);
    return temp(g->s0 + 1, "xyzw");
}

static const char *const target_name[] = {"", "BUFFER", "1D", "2D", "2D_MSAA", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY",
                                          "2D_ARRAY_MSAA", "CUBE_ARRAY"};
static const char *const shadow_name[] = {"", "BUFFER", "SHADOW1D", "SHADOW2D", "2D_MSAA", "3D", "SHADOWCUBE",
                                          "SHADOW1D_ARRAY", "SHADOW2D_ARRAY", "2D_ARRAY_MSAA", "SHADOWCUBE_ARRAY"};

/* texture instructions: SAMPLE* name, SVIEW[t] and SAMP[t] (the backend binds the sampler state the shader pairs
   with t# at slot t); the result goes through the resource's swizzle */
static void texture(struct tg *g, const struct dxbc_ins *in, const char *name, int extra_first, int nextra,
                    bool sampler)
{
    const struct opd *dst = &in->o[0], *res = &in->o[2];
    if (dst->type == T_NULL)
        return;
    if (in->offset[0] || in->offset[1] || in->offset[2])
        return (void)fail(g, "texel offsets");
    int t = res->idx[0].imm;
    if (res->type != T_RESOURCE || t < 0 || t >= DXBC_MAX_RES || !g->sh->res_dim[t] || g->sh->res_dim[t] > 10)
        return (void)fail(g, "undeclared texture");
    g->used_tex |= 1u << t;
    if (sampler) {
        int s = in->o[3].idx[0].imm;
        if (in->o[3].type != T_SAMPLER || s < 0 || s >= 16)
            return (void)fail(g, "sampler operand");
        if (g->out->tex_sampler[t] == 0xff)
            g->out->tex_sampler[t] = (uint8_t)s;
    }
    if (!strncmp(name, "SAMPLE_C", 8))
        g->cmp_tex |= 1u << t;
    bool identity = res->swz[0] == 0 && res->swz[1] == 1 && res->swz[2] == 2 && res->swz[3] == 3;
    struct opd d = identity && !in->sat ? *dst : scratch_dst(g, 2, 15);
    g->naddr = 0;
    load_addr(g, &d, 0);
    load_addr(g, &in->o[1], 1);
    for (int i = 0; i < nextra; i++)
        load_addr(g, &in->o[extra_first + i], 2 + i);
    put(&g->code, name);
    dst_text(g, &d, 0);
    src_text(g, &in->o[1], 1, false);
    put(&g->code, ", SVIEW[");
    putu(&g->code, (uint32_t)t);
    put(&g->code, "]");
    if (sampler) {
        put(&g->code, ", SAMP[");
        putu(&g->code, (uint32_t)t);
        put(&g->code, "]");
    }
    for (int i = 0; i < nextra; i++) {
        const struct opd *e = &in->o[extra_first + i];
        struct opd one = !strcmp(name, "SAMPLE_D") ? *e : scalar_of(e, 0);
        src_text(g, &one, 2 + i, false);
    }
    put(&g->code, "\n");
    if (!(identity && !in->sat)) {
        struct opd from = temp(g->s0 + 2, "xyzw");
        for (int i = 0; i < 4; i++)
            from.swz[i] = res->swz[i];
        emit1(g, "MOV", in->sat, dst, &from);
    }
}

struct simple {
    uint16_t op;
    uint8_t nsrc;
    const char *name;
};

static const struct simple simple_ops[] = {
    {OP_ADD, 2, "ADD"},       {OP_MUL, 2, "MUL"},          {OP_DIV, 2, "DIV"},         {OP_MAD, 3, "MAD"},
    {OP_MIN, 2, "MIN"},       {OP_MAX, 2, "MAX"},          {OP_DP2, 2, "DP2"},         {OP_DP3, 2, "DP3"},
    {OP_DP4, 2, "DP4"},       {OP_MOV, 1, "MOV"},          {OP_MOVC, 3, "UCMP"},       {OP_FRC, 1, "FRC"},
    {OP_ROUND_NE, 1, "ROUND"}, {OP_ROUND_NI, 1, "FLR"},    {OP_ROUND_PI, 1, "CEIL"},   {OP_ROUND_Z, 1, "TRUNC"},
    {OP_EQ, 2, "FSEQ"},       {OP_NE, 2, "FSNE"},          {OP_LT, 2, "FSLT"},         {OP_GE, 2, "FSGE"},
    {OP_IEQ, 2, "USEQ"},      {OP_INE, 2, "USNE"},         {OP_ILT, 2, "ISLT"},        {OP_IGE, 2, "ISGE"},
    {OP_ULT, 2, "USLT"},      {OP_UGE, 2, "USGE"},         {OP_IADD, 2, "UADD"},       {OP_IMAD, 3, "UMAD"},
    {OP_UMAD, 3, "UMAD"},     {OP_IMAX, 2, "IMAX"},        {OP_IMIN, 2, "IMIN"},       {OP_UMAX, 2, "UMAX"},
    {OP_UMIN, 2, "UMIN"},     {OP_INEG, 1, "INEG"},        {OP_NOT, 1, "NOT"},         {OP_AND, 2, "AND"},
    {OP_OR, 2, "OR"},         {OP_XOR, 2, "XOR"},          {OP_FTOI, 1, "F2I"},        {OP_FTOU, 1, "F2U"},
    {OP_ITOF, 1, "I2F"},      {OP_UTOF, 1, "U2F"},         {OP_DERIV_RTX, 1, "DDX"},   {OP_DERIV_RTY, 1, "DDY"},
    {OP_DERIV_RTX_COARSE, 1, "DDX"}, {OP_DERIV_RTY_COARSE, 1, "DDY"}, {OP_DERIV_RTX_FINE, 1, "DDX_FINE"},
    {OP_DERIV_RTY_FINE, 1, "DDY_FINE"}, {OP_COUNTBITS, 1, "POPC"}, {OP_FIRSTBIT_LO, 1, "LSB"},
    {OP_BFREV, 1, "BREV"},
};

static const struct {
    uint16_t op;
    const char *name;
} scalar_ops[] = {
    {OP_RCP, "RCP"}, {OP_RSQ, "RSQ"}, {OP_SQRT, "SQRT"}, {OP_EXP, "EX2"}, {OP_LOG, "LG2"},
};

static bool last_ret(const struct dxbc_shader *sh, int i)
{
    for (int k = i + 1; k < sh->ncode; k++)
        if (sh->code[k].op != OP_NOP && sh->code[k].op != OP_RET)
            return false;
    return true;
}

static void instruction(struct tg *g, int pc)
{
    const struct dxbc_ins *in = &g->sh->code[pc];
    const struct opd *o = in->o;
    for (size_t i = 0; i < sizeof(simple_ops) / sizeof(simple_ops[0]); i++)
        if (simple_ops[i].op == in->op) {
            if (o[0].type == T_NULL)
                return;
            const struct opd *s[3] = {&o[1], &o[2], &o[3]};
            bool sat = in->sat && !dxbc_int_op(in->op);
            if (o[0].type == T_ODEPTH) { /* oDepth is one component: through a scratch x, then to .z */
                struct opd d = scratch_dst(g, 3, 1), x = temp(g->s0 + 3, "xxxx");
                emit(g, simple_ops[i].name, false, &d, s, simple_ops[i].nsrc);
                emit1(g, "MOV", sat, &o[0], &x);
            } else {
                emit(g, simple_ops[i].name, sat, &o[0], s, simple_ops[i].nsrc);
            }
            return;
        }
    for (size_t i = 0; i < sizeof(scalar_ops) / sizeof(scalar_ops[0]); i++)
        if (scalar_ops[i].op == in->op)
            return per_component(g, scalar_ops[i].name, in, &o[0], &o[1]);
    switch (in->op) {
    case OP_NOP:
    case OP_CUSTOMDATA:
        return;
    case OP_ISHL:
    case OP_ISHR:
    case OP_USHR: {
        if (o[0].type == T_NULL)
            return;
        struct opd b = shift_amount(g, &o[2]);
        emit2(g, in->op == OP_ISHL ? "SHL" : in->op == OP_ISHR ? "ISHR" : "USHR", &o[0], &o[1], &b);
        return;
    }
    case OP_UDIV:
    case OP_IMUL:
    case OP_UMUL: { /* two destinations: quotient and remainder, or high and low halves */
        const char *first = in->op == OP_UDIV ? "UDIV" : in->op == OP_IMUL ? "IMUL_HI" : "UMUL_HI";
        const char *second = in->op == OP_UDIV ? "UMOD" : "UMUL";
        bool both = o[0].type != T_NULL && o[1].type != T_NULL;
        if (o[1].type != T_NULL) {
            struct opd d = both ? scratch_dst(g, 0, o[1].mask) : o[1];
            emit2(g, second, &d, &o[2], &o[3]);
        }
        if (o[0].type != T_NULL)
            emit2(g, first, &o[0], &o[2], &o[3]);
        if (both) {
            struct opd from = temp(g->s0, "xyzw");
            emit1(g, "MOV", false, &o[1], &from);
        }
        return;
    }
    case OP_SINCOS: {
        if (o[0].type != T_NULL && o[1].type != T_NULL) {
            /* both from the same source: sines to a scratch register first, the source may be overwritten */
            unsigned m = o[0].mask;
            struct opd s = scratch_dst(g, 1, m);
            for (int c = 0; c < 4; c++)
                if (m & (1u << c)) {
                    struct opd one = scalar_of(&o[2], c), d = s;
                    d.mask = (uint8_t)(1u << c);
                    emit1(g, "SIN", false, &d, &one);
                }
            per_component(g, "COS", in, &o[1], &o[2]);
            struct opd from = temp(g->s0 + 1, "xyzw");
            emit1(g, "MOV", in->sat, &o[0], &from);
        } else {
            per_component(g, "SIN", in, &o[0], &o[2]);
            per_component(g, "COS", in, &o[1], &o[2]);
        }
        return;
    }
    case OP_FIRSTBIT_HI:
    case OP_FIRSTBIT_SHI: { /* counted from the top: 31 - the most significant bit, -1 for none */
        if (o[0].type == T_NULL)
            return;
        struct opd a = scratch_dst(g, 0, 15), neg = scratch_dst(g, 1, 15), flip = scratch_dst(g, 2, 15);
        struct opd ra = temp(g->s0, "xyzw"), rn = temp(g->s0 + 1, "xyzw"), rf = temp(g->s0 + 2, "xyzw");
        struct opd zero = imm_opd(0), k31 = imm_opd(31);
        emit1(g, in->op == OP_FIRSTBIT_HI ? "UMSB" : "IMSB", false, &a, &o[1]);
        emit2(g, "ISLT", &neg, &ra, &zero);
        emit2(g, "XOR", &flip, &ra, &k31);
        emit3(g, "UCMP", &o[0], &rn, &ra, &rf);
        return;
    }
    case OP_UBFE:
    case OP_IBFE: /* width, offset, value -> value, offset, width */
        if (o[0].type != T_NULL)
            emit3(g, in->op == OP_UBFE ? "UBFE" : "IBFE", &o[0], &o[3], &o[2], &o[1]);
        return;
    case OP_BFI: { /* width, offset, insert, base -> base, insert, offset, width */
        if (o[0].type == T_NULL)
            return;
        const struct opd *s[4] = {&o[4], &o[3], &o[2], &o[1]};
        emit(g, "BFI", false, &o[0], s, 4);
        return;
    }
    case OP_F32TOF16:
    case OP_F16TOF32: {
        if (o[0].type == T_NULL)
            return;
        for (int c = 0; c < 4; c++) {
            if (!(o[0].mask & (1u << c)))
                continue;
            struct opd one = scalar_of(&o[1], c), d = scratch_dst(g, 0, 15), x = temp(g->s0, "xxxx");
            struct opd tmp = scratch_dst(g, 3, 1u << c);
            emit1(g, in->op == OP_F32TOF16 ? "PK2H" : "UP2H", false, &d, &one);
            if (in->op == OP_F32TOF16) {
                struct opd low = imm_opd(0xffff);
                emit2(g, "AND", &tmp, &x, &low);
            } else {
                emit1(g, "MOV", false, &tmp, &x);
            }
        }
        struct opd from = temp(g->s0 + 3, "xyzw");
        emit1(g, "MOV", false, &o[0], &from);
        return;
    }
    case OP_IF:
        test_cond(g, in, &o[0]);
        return;
    case OP_ELSE:
        line(g, "ELSE");
        return;
    case OP_ENDIF:
        line(g, "ENDIF");
        return;
    case OP_LOOP:
        line(g, "BGNLOOP");
        return;
    case OP_ENDLOOP:
        line(g, "ENDLOOP");
        return;
    case OP_BREAK:
        line(g, "BRK");
        return;
    case OP_CONTINUE:
        line(g, "CONT");
        return;
    case OP_BREAKC:
    case OP_CONTINUEC:
    case OP_RETC:
        test_cond(g, in, &o[0]);
        line(g, in->op == OP_BREAKC ? "BRK" : in->op == OP_CONTINUEC ? "CONT" : "RET");
        line(g, "ENDIF");
        return;
    case OP_DISCARD:
        test_cond(g, in, &o[0]);
        line(g, "KILL");
        line(g, "ENDIF");
        return;
    case OP_RET:
        if (!last_ret(g->sh, pc))
            line(g, "RET");
        return;
    case OP_SWITCH:
        emit(g, "SWITCH", false, NULL, (const struct opd *const[]){&o[0]}, 1);
        return;
    case OP_CASE:
        emit(g, "CASE", false, NULL, (const struct opd *const[]){&o[0]}, 1);
        return;
    case OP_DEFAULT:
        line(g, "DEFAULT");
        return;
    case OP_ENDSWITCH:
        line(g, "ENDSWITCH");
        return;
    case OP_SAMPLE:
        return texture(g, in, "SAMPLE", 0, 0, true);
    case OP_SAMPLE_L:
        return texture(g, in, "SAMPLE_L", 4, 1, true);
    case OP_SAMPLE_B:
        return texture(g, in, "SAMPLE_B", 4, 1, true);
    case OP_SAMPLE_C:
        return texture(g, in, "SAMPLE_C", 4, 1, true);
    case OP_SAMPLE_C_LZ:
        return texture(g, in, "SAMPLE_C_LZ", 4, 1, true);
    case OP_SAMPLE_D:
        return texture(g, in, "SAMPLE_D", 4, 2, true);
    case OP_LD:
        return texture(g, in, "SAMPLE_I", 0, 0, false);
    case OP_RESINFO: { /* SVIEWINFO gives integers: floats unless the instruction asks for _uint */
        const struct opd *res = &in->o[2];
        int t = res->idx[0].imm;
        if (o[0].type == T_NULL)
            return;
        if (res->type != T_RESOURCE || t < 0 || t >= DXBC_MAX_RES || !g->sh->res_dim[t])
            return (void)fail(g, "undeclared texture");
        if (in->ret_type == 1)
            return (void)fail(g, "resinfo_rcpFloat");
        g->used_tex |= 1u << t;
        struct opd d = scratch_dst(g, 2, 15), mip = scalar_of(&o[1], 0);
        g->naddr = 0;
        load_addr(g, &mip, 1);
        put(&g->code, "SVIEWINFO");
        dst_text(g, &d, 0);
        src_text(g, &mip, 1, false);
        put(&g->code, ", SVIEW[");
        putu(&g->code, (uint32_t)t);
        put(&g->code, "]\n");
        struct opd from = temp(g->s0 + 2, "xyzw");
        for (int i = 0; i < 4; i++)
            from.swz[i] = res->swz[i];
        emit1(g, in->ret_type == 2 ? "MOV" : "U2F", false, &o[0], &from);
        return;
    }
    default:
        fail(g, "an instruction without a TGSI translation");
    }
}

/* ---- declarations */

static void decl_reg(struct text *t, const char *file, int n, const char *rest)
{
    put(t, "DCL ");
    put(t, file);
    put(t, "[");
    putu(t, (uint32_t)n);
    put(t, "]");
    put(t, rest);
    put(t, "\n");
}

static const char *interp_name(int mode)
{
    switch (mode) {
    case 1: return ", CONSTANT";
    case 3: return ", PERSPECTIVE, CENTROID";
    case 4: return ", LINEAR";
    case 5: return ", LINEAR, CENTROID";
    case 6: return ", PERSPECTIVE, SAMPLE";
    case 7: return ", LINEAR, SAMPLE";
    default: return ", PERSPECTIVE";
    }
}

static bool declarations(struct tg *g, struct text *t)
{
    const struct dxbc_shader *sh = g->sh;
    put(t, sh->stage == DXBC_VS ? "VERT\n" : "FRAG\n");
    if (sh->stage == DXBC_PS)
        put(t, "PROPERTY FS_COORD_ORIGIN UPPER_LEFT\nPROPERTY FS_COORD_PIXEL_CENTER HALF_INTEGER\n");
    uint32_t done = 0;
    if (sh->stage == DXBC_VS) {
        for (int i = 0; i < sh->nin; i++) {
            uint32_t r = sh->in[i].reg;
            if (r >= 32 || (done & (1u << r)) || g->sv_in[r] >= 0)
                continue;
            done |= 1u << r;
            decl_reg(t, "IN", (int)r, "");
        }
        for (int k = 0; k < g->nsv; k++)
            decl_reg(t, "SV", k, g->sv_name[k] == 6 ? ", VERTEXID" : ", INSTANCEID");
    } else {
        for (int i = 0; i < sh->nin; i++) {
            uint32_t r = sh->in[i].reg;
            if (r >= 32 || (done & (1u << r)))
                continue;
            if ((int)r == sh->pos_in_reg) {
                done |= 1u << r;
                decl_reg(t, "IN", (int)r, ", POSITION, LINEAR");
                continue;
            }
            if (!sh->interp[r])
                continue; /* not declared: not read */
            if (sh->in[i].system_value && sh->in[i].system_value != 1)
                return fail(g, "a pixel shader system value input");
            done |= 1u << r;
            put(t, "DCL IN[");
            putu(t, r);
            put(t, "], GENERIC[");
            putu(t, r);
            put(t, "]");
            put(t, interp_name(sh->interp[r]));
            put(t, "\n");
        }
    }
    done = 0;
    for (int i = 0; i < sh->nout; i++) {
        uint32_t r = sh->out[i].reg, sv = sh->out[i].system_value;
        if (sh->stage == DXBC_PS && (r == 0xffffffffu || !strncmp(sh->out[i].name, "SV_Depth", 8) ||
                                     !strncmp(sh->out[i].name, "SV_DEPTH", 8)))
            continue;
        if (r >= 32 || (done & (1u << r)))
            continue;
        done |= 1u << r;
        put(t, "DCL OUT[");
        putu(t, r);
        if (sh->stage == DXBC_PS) {
            if (strncmp(sh->out[i].name, "SV_Target", 9) && strncmp(sh->out[i].name, "SV_TARGET", 9))
                return fail(g, "a pixel shader output other than targets and depth");
            put(t, "], COLOR[");
            putu(t, r);
            put(t, "]\n");
        } else if (sv == 1) {
            put(t, "], POSITION\n");
        } else if (sv == 0) {
            put(t, "], GENERIC[");
            putu(t, r);
            put(t, "]\n");
        } else {
            return fail(g, "a vertex shader system value output (clip or cull distance)");
        }
    }
    if (sh->depth_out)
        decl_reg(t, "OUT", g->depth_reg, ", POSITION");
    int total = g->s0 + 4;
    put(t, "DCL TEMP[0..");
    putu(t, (uint32_t)(sh->temps > 0 ? sh->temps - 1 : 0));
    put(t, "]\n");
    for (int x = 0; x < 8; x++)
        if (sh->xtemps[x]) {
            put(t, "DCL TEMP[");
            putu(t, (uint32_t)g->xbase[x]);
            put(t, "..");
            putu(t, (uint32_t)(g->xbase[x] + sh->xsize[x] - 1));
            put(t, "], ARRAY(");
            putu(t, (uint32_t)x + 1);
            put(t, ")\n");
        }
    put(t, "DCL TEMP[");
    putu(t, (uint32_t)g->s0);
    put(t, "..");
    putu(t, (uint32_t)total - 1);
    put(t, "]\n");
    put(t, "DCL ADDR[0..2]\n");
    for (int s = 0; s < DXBC_MAX_CB; s++)
        if (g->used_cb & (1u << s)) {
            uint32_t size = sh->cb_size[s] ? sh->cb_size[s] : 4096;
            put(t, "DCL CONST[");
            putu(t, (uint32_t)s + 1);
            put(t, "][0..");
            putu(t, size - 1);
            put(t, "]\n");
        }
    static const char *const ret[] = {"FLOAT", "UNORM", "SNORM", "SINT", "UINT", "FLOAT"};
    for (int r = 0; r < DXBC_MAX_RES; r++)
        if (g->used_tex & (1u << r)) {
            int dim = sh->res_dim[r];
            if (g->out->tex_sampler[r] != 0xff)
                decl_reg(t, "SAMP", r, "");
            put(t, "DCL SVIEW[");
            putu(t, (uint32_t)r);
            put(t, "], ");
            put(t, (g->cmp_tex & (1u << r)) ? shadow_name[dim] : target_name[dim]);
            put(t, ", ");
            put(t, ret[sh->res_ret[r] <= 5 ? sh->res_ret[r] : 0]);
            put(t, "\n");
        }
    for (int i = 0; i < g->nimm; i++) {
        put(t, "IMM[");
        putu(t, (uint32_t)i);
        put(t, "] UINT32 {");
        for (int c = 0; c < 4; c++) {
            putu(t, g->imm[i][c]);
            put(t, c < 3 ? ", " : "}\n");
        }
    }
    return true;
}

bool dxbc_tgsi(const struct dxbc_shader *sh, struct dxbc_tgsi *out)
{
    memset(out, 0, sizeof(*out));
    memset(out->tex_sampler, 0xff, sizeof(out->tex_sampler));
    struct tg *g = kmalloc(sizeof(*g));
    if (!g) {
        memcpy(out->error, "out of memory", 14);
        return false;
    }
    memset(g, 0, sizeof(*g));
    g->sh = sh, g->out = out;
    if (sh->stage != DXBC_VS && sh->stage != DXBC_PS)
        fail(g, "only vertex and pixel shaders");
    if (sh->icb_vec4 > MAX_IMM / 2)
        fail(g, "a large immediate constant buffer");
    for (int i = 0; i < sh->icb_vec4 && !g->fail; i++) {
        const uint32_t *v = (const uint32_t *)&sh->icb[i * 4];
        for (int c = 0; c < 4; c++)
            g->imm[i][c] = v[c];
    }
    g->nimm = g->fail ? 0 : sh->icb_vec4;
    int next = sh->temps;
    for (int x = 0; x < 8; x++)
        if (sh->xtemps[x])
            g->xbase[x] = next, next += sh->xsize[x];
    g->s0 = next;
    g->depth_reg = sh->max_out_reg + 1;
    for (int r = 0; r < 32; r++)
        g->sv_in[r] = -1;
    if (sh->stage == DXBC_VS)
        for (int i = 0; i < sh->nin; i++) {
            uint32_t sv = sh->in[i].system_value, r = sh->in[i].reg;
            if (!sv || r >= 32 || g->sv_in[r] >= 0)
                continue;
            if (sv != 6 && sv != 8)
                fail(g, "a vertex shader system value input");
            g->sv_name[g->nsv] = (uint8_t)sv;
            g->sv_in[r] = (int8_t)g->nsv++;
        }
    for (int pc = 0; pc < sh->ncode && !g->fail; pc++)
        instruction(g, pc);
    put(&g->code, "END\n");
    struct text t = {0};
    if (!g->fail)
        declarations(g, &t);
    if (!g->fail)
        put(&t, g->code.p ? g->code.p : "");
    if (g->code.full || t.full)
        fail(g, "out of memory");
    kfree(g->code.p);
    bool ok = !g->fail;
    kfree(g);
    if (!ok) {
        kfree(t.p);
        return false;
    }
    out->text = t.p, out->len = t.len;
    return true;
}

void dxbc_tgsi_free(struct dxbc_tgsi *out)
{
    kfree(out->text);
    out->text = NULL, out->len = 0;
}

#endif
