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
    int8_t sv_in[32];      /* vertex and pixel shader inputs that are system values: SV[n], or -1 */
    int face;              /* the pixel shader's SV_IsFrontFace input register (read from TEMP[s0 + 4]), or -1 */
    int clip_reg[2];       /* the output registers holding clip then cull distances, CLIPDIST[0] and [1], or -1 */
    int nclip, ncull;
    int nsv;
    uint8_t sv_name[32];
    unsigned gs_sv;
    uint32_t used_buf, used_img; /* BUFFER[n] and IMAGE[n] the shader reaches */
    uint8_t used_counter;        /* u# whose hidden counter it reaches, in BUFFER[24 + u] */
    uint8_t srv_high;            /* t16 .. t23 as buffers, in BUFFER[24 ..] too */
    bool shared;           /* reaches MEMORY[0], the group's shared memory */
    const uint16_t *uav_format;        /* geometry shader: reads the primitive number (1, SV[0]),
                                          the instance number (2, SV[1]) */
    uint32_t used_cb, used_tex, cmp_tex, plain_tex;
    int32_t cb_max[DXBC_MAX_CB]; /* the highest constant read with a fixed index, or 4095 when one is relative */
    int naddr;
    /* hull and domain shaders: the patch constants in TEMP[pcbase + reg]; the hull shader phase being translated
       (0 control points, 1 fork, 2 join), its instance and where it ends; the hull shader a domain shader runs after */
    int pcbase, npc;
    int phase, phase_inst, phase_end;
    bool hs_primid;
    const struct dxbc_shader *hs;
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
    for (int i = 0; i < g->nimm; i++)
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
        int rt = o->idx[d].rel_type;
        if (rt != T_TEMP && rt != T_INPUT && rt != T_OCPID && rt != T_FORKID && rt != T_JOINID) {
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
        if (rt == T_OCPID) {
            put(&g->code, "SV[0].");
        } else if (rt == T_FORKID || rt == T_JOINID) { /* the phase instance being translated */
            uint32_t i = (uint32_t)g->phase_inst;
            put(&g->code, "IMM[");
            putu(&g->code, (uint32_t)immediate(g, i, i, i, i));
            put(&g->code, "].");
        } else {
            put(&g->code, o->idx[d].rel_type == T_TEMP ? "TEMP[" : "IN[");
            puti(&g->code, o->idx[d].rel_reg);
            put(&g->code, "].");
        }
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
        if (g->sh->stage == DXBC_GS) { /* v[vertex][register] */
            if (o->ndim != 2) {
                fail(g, "a geometry shader input without a vertex");
                break;
            }
            put(t, "IN"), index_text(g, o, 0, slot, 0), index_text(g, o, 1, slot, 0);
            break;
        }
        if (o->ndim != 1)
            fail(g, "input arrays");
        if (g->sh->stage == DXBC_PS && g->face >= 0 && o->idx[0].imm == g->face && o->idx[0].rel_type < 0) {
            put(t, "TEMP[");
            putu(t, (uint32_t)g->s0 + 4);
            put(t, "]");
        } else if ((g->sh->stage == DXBC_VS || g->sh->stage == DXBC_PS) && o->idx[0].imm >= 0 && o->idx[0].imm < 32 &&
                   g->sv_in[o->idx[0].imm] >= 0) {
            put(t, "SV[");
            putu(t, (uint32_t)g->sv_in[o->idx[0].imm]);
            put(t, "]");
        } else {
            put(t, "IN"), index_text(g, o, 0, slot, 0);
        }
        break;
    case T_OUTPUT:
        if (g->sh->stage == DXBC_HS && g->phase) { /* a patch constant, kept in a temp until the end */
            if (o->idx[0].rel_type < 0 && (o->idx[0].imm < 0 || o->idx[0].imm >= g->npc)) {
                fail(g, "a patch constant output");
                break;
            }
            put(t, "TEMP"), index_text(g, o, 0, slot, g->pcbase);
            put(t, "(9)");
        } else if (g->sh->stage == DXBC_HS) { /* this invocation's control point */
            put(t, "OUT[0]"), index_text(g, o, 0, slot, 0);
        } else {
            put(t, "OUT"), index_text(g, o, 0, slot, 0);
        }
        break;
    case T_ICP: /* vicp[point][reg] (hull), vcp (domain) */
        if (o->ndim != 2 || (g->sh->stage != DXBC_HS && g->sh->stage != DXBC_DS)) {
            fail(g, "an input control point");
            break;
        }
        put(t, "IN"), index_text(g, o, 0, slot, 0), index_text(g, o, 1, slot, 0);
        break;
    case T_PC:
        if (o->idx[0].rel_type < 0 && (o->idx[0].imm < 0 || o->idx[0].imm >= g->npc)) {
            fail(g, "a patch constant input");
            break;
        }
        put(t, "TEMP"), index_text(g, o, 0, slot, g->pcbase);
        put(t, "(9)");
        break;
    case T_OCPID:
        put(t, "SV[0]");
        break;
    case T_FORKID:
    case T_JOINID: {
        uint32_t i = (uint32_t)g->phase_inst;
        put(t, "IMM[");
        putu(t, (uint32_t)immediate(g, i, i, i, i));
        put(t, "]");
        break;
    }
    case T_DOMAIN:
        put(t, "SV[0]");
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
        int32_t top = o->idx[1].rel_type >= 0 ? 4095 : o->idx[1].imm;
        g->cb_max[s] = top > g->cb_max[s] ? top : g->cb_max[s];
        put(t, "CONST[");
        putu(t, (uint32_t)s + 1);
        put(t, "]");
        index_text(g, o, 1, slot, 0);
        break;
    }
    case T_TID: /* SV_DispatchThreadID, worked out at the start */
        put(t, "TEMP[");
        putu(t, (uint32_t)g->s0 + 4);
        put(t, "]");
        break;
    case T_FLAT: /* SV_GroupIndex, worked out at the start */
        put(t, "TEMP[");
        putu(t, (uint32_t)g->s0 + 5);
        put(t, "]");
        break;
    case T_GTID:
        put(t, "SV[0]");
        break;
    case T_GID:
        put(t, "SV[1]");
        break;
    case T_PRIMID:
        if (g->sh->stage == DXBC_HS) {
            g->hs_primid = true;
            put(t, "SV[1]");
            break;
        }
        /* fall through */
    case T_GSINST:
        if (g->sh->stage != DXBC_GS) {
            fail(g, "a primitive or instance number outside a geometry shader");
            break;
        }
        g->gs_sv |= o->type == T_PRIMID ? 1 : 2;
        put(t, o->type == T_PRIMID ? "SV[0]" : "SV[1]");
        break;
    case T_ICB: /* in a constant buffer at slot 14 (the host has no indexed immediates) */
        if (o->idx[0].rel_type < 0 && (o->idx[0].imm < 0 || o->idx[0].imm >= g->sh->icb_vec4))
            fail(g, "immediate constant buffer index");
        g->out->icb = true;
        put(t, "CONST[15]");
        index_text(g, o, 0, slot, 0);
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

/* texture instructions the way GL samples (virglrenderer takes TEX, TXL, TXB, TXD, TXF and TXQ, not SAMPLE*): SAMP[t]
   is both the view at t and the sampler state the shader pairs with t#, which the backend binds at slot t. The
   coordinate is copied to TEMP[s0], where a level, bias or depth reference goes into the component GL reads it from
   (or into TEMP[s0 + 1] for the *2 forms); the result comes through TEMP[s0 + 2] when the resource has a swizzle. */
enum { TX_SAMPLE, TX_L, TX_B, TX_C, TX_C_LZ, TX_D, TX_LD, TX_LOD, TX_G, TX_G_C, TX_G_PO, TX_G_PO_C };

static const uint8_t coord_count[] = {0, 1, 1, 2, 2, 3, 3, 2, 3, 3, 4};

static void texture(struct tg *g, const struct dxbc_ins *in, int kind)
{
    /* gather4_po*: a programmable offset after the coordinate moves the other operands one on */
    bool po = kind == TX_G_PO || kind == TX_G_PO_C, gather = kind >= TX_G;
    int k = po ? 1 : 0;
    const struct opd *dst = &in->o[0], *res = &in->o[2 + k], *smp = &in->o[3 + k], *ref_op = &in->o[4 + k];
    if (dst->type == T_NULL)
        return;
    bool offset = in->offset[0] || in->offset[1] || in->offset[2];
    int t = res->idx[0].imm;
    if (res->type != T_RESOURCE || t < 0 || t >= DXBC_MAX_RES || !g->sh->res_dim[t] || g->sh->res_dim[t] > 10)
        return (void)fail(g, "undeclared texture");
    int dim = g->sh->res_dim[t];
    bool cmp = kind == TX_C || kind == TX_C_LZ || kind == TX_G_C || kind == TX_G_PO_C;
    if (kind != TX_LD) {
        int s = smp->idx[0].imm;
        if (smp->type != T_SAMPLER || s < 0 || s >= 16)
            return (void)fail(g, "sampler operand");
        if (g->out->tex_sampler[t] == 0xff)
            g->out->tex_sampler[t] = (uint8_t)s;
        if (dim == 1 || dim == 4 || dim == 9)
            return (void)fail(g, "sampling a buffer or a multisampled texture");
    }
    if (cmp ? (g->plain_tex >> t & 1) : (g->cmp_tex >> t & 1))
        return (void)fail(g, "a texture sampled with and without comparison");
    if (cmp && (dim == 5 || dim == 1))
        return (void)fail(g, "comparison on a 3D texture");
    g->used_tex |= 1u << t;
    if (cmp)
        g->cmp_tex |= 1u << t;
    else if (kind != TX_LD)
        g->plain_tex |= 1u << t;
    const char *target = cmp ? shadow_name[dim] : target_name[dim];
    bool four = coord_count[dim] == 4; /* cube arrays: nothing left in the coordinate for a level or reference */
    if (gather && (four || dim == 2 || dim == 7 || dim == 5))
        return (void)fail(g, "gather from a 1D, 3D or cube array texture");
    struct opd c = scratch_dst(g, 0, 15), cw = scratch_dst(g, 0, 8), cz = scratch_dst(g, 0, 4);
    struct opd e = scratch_dst(g, 1, 15), ex = scratch_dst(g, 1, 1), ey = scratch_dst(g, 1, 2);
    emit1(g, "MOV", false, &c, &in->o[1]);
    const char *name = "TEX";
    bool extra = false;
    if (kind == TX_L || kind == TX_B) {
        struct opd v = scalar_of(&in->o[4], 0);
        name = kind == TX_L ? (four ? "TXL2" : "TXL") : (four ? "TXB2" : "TXB");
        emit1(g, "MOV", false, four ? &ex : &cw, &v);
        extra = four;
    } else if (gather) {
        name = "TG4";
        if (cmp) {
            struct opd ref = scalar_of(ref_op, 0);
            emit1(g, "MOV", false, dim == 6 || dim == 8 ? &cw : &cz, &ref);
        }
        if (po) { /* the offset as integers in TEMP[s0 + 3] */
            struct opd o3 = scratch_dst(g, 3, 7);
            emit1(g, "MOV", false, &o3, &in->o[2]);
        }
    } else if (cmp) {
        struct opd ref = scalar_of(ref_op, 0), zero = imm_opd(0);
        bool in_w = dim == 6 || dim == 8; /* SHADOWCUBE, SHADOW2D_ARRAY */
        if (four) {
            emit1(g, "MOV", false, &ex, &ref);
            if (kind == TX_C_LZ)
                emit1(g, "MOV", false, &ey, &zero);
            name = kind == TX_C_LZ ? "TXL2" : "TEX2";
            extra = true;
        } else {
            emit1(g, "MOV", false, in_w ? &cw : &cz, &ref);
            if (kind == TX_C_LZ) {
                if (in_w)
                    emit1(g, "MOV", false, &ex, &zero), name = "TXL2", extra = true;
                else
                    emit1(g, "MOV", false, &cw, &zero), name = "TXL";
            }
        }
    } else if (kind == TX_D) {
        struct opd dy = scratch_dst(g, 3, 15);
        emit1(g, "MOV", false, &e, &in->o[4]);
        emit1(g, "MOV", false, &dy, &in->o[5]);
        name = "TXD";
    } else if (kind == TX_LD) {
        name = "TXF";
    } else if (kind == TX_LOD) { /* (clamped, unclamped, 0, 0) */
        struct opd zw = scratch_dst(g, 2, 12), zero = imm_opd(0);
        emit1(g, "MOV", false, &zw, &zero);
        name = "LODQ";
    }
    bool identity = res->swz[0] == 0 && res->swz[1] == 1 && res->swz[2] == 2 && res->swz[3] == 3 && kind != TX_LOD;
    struct opd d = identity && !in->sat ? *dst : scratch_dst(g, 2, kind == TX_LOD ? 3 : 15);
    g->naddr = 0;
    load_addr(g, &d, 0);
    put(&g->code, name);
    dst_text(g, &d, 0);
    put(&g->code, ", TEMP[");
    putu(&g->code, (uint32_t)g->s0);
    put(&g->code, "]");
    if (extra || kind == TX_D) {
        put(&g->code, ", TEMP[");
        putu(&g->code, (uint32_t)g->s0 + 1);
        put(&g->code, "]");
    }
    if (kind == TX_D) {
        put(&g->code, ", TEMP[");
        putu(&g->code, (uint32_t)g->s0 + 3);
        put(&g->code, "]");
    }
    if (gather) { /* the channel gathered: what the sampler operand selects (comparisons compare the first) */
        int comp = cmp ? 0 : smp->swz[0] & 3;
        put(&g->code, ", IMM[");
        putu(&g->code, (uint32_t)immediate(g, (uint32_t)comp, 0, 0, 0));
        put(&g->code, "].xxxx");
    }
    put(&g->code, ", SAMP[");
    putu(&g->code, (uint32_t)t);
    put(&g->code, "], ");
    put(&g->code, target);
    if (po) {
        put(&g->code, ", TEMP[");
        putu(&g->code, (uint32_t)g->s0 + 3);
        put(&g->code, "].xyz");
    } else if (offset) { /* aoffimmi: a constant texel offset, as an immediate the host reads x, y, z of */
        if (four || dim == 6 || dim == 10)
            return (void)fail(g, "texel offsets on a cube");
        put(&g->code, ", IMM[");
        putu(&g->code, (uint32_t)immediate(g, (uint32_t)in->offset[0], (uint32_t)in->offset[1],
                                           (uint32_t)in->offset[2], 0));
        put(&g->code, "].xyz");
    }
    put(&g->code, "\n");
    if (!(identity && !in->sat)) {
        struct opd from = temp(g->s0 + 2, "xyzw");
        for (int i = 0; i < 4; i++)
            from.swz[i] = res->swz[i];
        emit1(g, "MOV", in->sat, dst, &from);
    }
}

/* ---- memory: u# raw or structured as BUFFER[u], t# raw or structured as BUFFER[8 + t], g# in MEMORY[0] (the host
   gives a compute shader one block of shared memory: each g# at its offset), typed u# as IMAGE[u] */

static const struct {
    uint16_t format;
    const char *name;
} image_formats[] = {
    {1, "B8G8R8A8_UNORM"},      {8, "R10G10B10A2_UNORM"},    {28, "R32_FLOAT"},           {29, "R32G32_FLOAT"},
    {31, "R32G32B32A32_FLOAT"}, {48, "R16_UNORM"},           {49, "R16G16_UNORM"},        {51, "R16G16B16A16_UNORM"},
    {59, "R16G16B16A16_SNORM"}, {64, "R8_UNORM"},            {65, "R8G8_UNORM"},          {67, "R8G8B8A8_UNORM"},
    {77, "R8G8B8A8_SNORM"},     {91, "R16_FLOAT"},           {92, "R16G16_FLOAT"},        {94, "R16G16B16A16_FLOAT"},
    {124, "R11G11B10_FLOAT"},   {177, "R8_UINT"},            {178, "R8G8_UINT"},          {180, "R8G8B8A8_UINT"},
    {181, "R8_SINT"},           {184, "R8G8B8A8_SINT"},      {185, "R16_UINT"},           {186, "R16G16_UINT"},
    {188, "R16G16B16A16_UINT"}, {189, "R16_SINT"},           {190, "R16G16_SINT"},        {192, "R16G16B16A16_SINT"},
    {193, "R32_UINT"},          {194, "R32G32_UINT"},        {196, "R32G32B32A32_UINT"},  {197, "R32_SINT"},
    {198, "R32G32_SINT"},       {200, "R32G32B32A32_SINT"},  {253, "R10G10B10A2_UINT"},
};

static const char *image_format(struct tg *g, int u)
{
    uint16_t f = g->uav_format ? g->uav_format[u] : 0;
    for (size_t i = 0; f && i < sizeof(image_formats) / sizeof(image_formats[0]); i++)
        if (image_formats[i].format == f)
            return image_formats[i].name;
    if (f)
        fail(g, "a typed UAV format");
    int ret = g->sh->uav_ret[u];
    return ret == 4 ? "R32_UINT" : ret == 3 ? "R32_SINT" : "R32_FLOAT";
}

static const char *image_target(struct tg *g, int u)
{
    switch (g->sh->uav_dim[u]) {
    case 1: return "BUFFER";
    case 2: return "1D";
    case 3: return "2D";
    case 5: return "3D";
    case 7: return "1D_ARRAY";
    case 8: return "2D_ARRAY";
    default: fail(g, "a typed UAV dimension"); return "2D";
    }
}

/* the memory behind a u#, t# or g# operand: its name, the byte offset its accesses start at and its structure stride */
static bool memory_of(struct tg *g, const struct opd *r, struct text *name, uint32_t *base, uint32_t *stride)
{
    int i = r->idx[0].imm;
    *base = 0, *stride = 0;
    if (r->type == T_UAV && i >= 0 && i < DXBC_MAX_UAV && g->sh->uav_kind[i]) {
        g->used_buf |= 1u << i;
        *stride = g->sh->uav_stride[i];
        put(name, "BUFFER[");
        putu(name, (uint32_t)i);
    } else if (r->type == T_RESOURCE && i >= 0 && i < 24 && (g->sh->srv_raw >> i & 1)) {
        g->used_buf |= 1u << (8 + i);
        if (i >= 16)
            g->srv_high |= (uint8_t)(1u << (i - 16));
        *stride = g->sh->srv_stride[i];
        put(name, "BUFFER[");
        putu(name, (uint32_t)i + 8);
    } else if (r->type == T_TGSM && i >= 0 && i < DXBC_MAX_TGSM && g->sh->tgsm_size[i]) {
        g->shared = true;
        *base = g->sh->tgsm_off[i], *stride = g->sh->tgsm_stride[i];
        put(name, "MEMORY[0");
    } else {
        return fail(g, "an undeclared buffer");
    }
    put(name, "]");
    return true;
}

static void put_temp(struct tg *g, int k, const char *swz)
{
    put(&g->code, "TEMP[");
    putu(&g->code, (uint32_t)(g->s0 + k));
    put(&g->code, "]");
    put(&g->code, swz);
}

/* TEMP[s0].x = the byte address: raw a.x, structured a.x * stride + b.x, plus the block's base */
static void byte_address(struct tg *g, const struct opd *a, const struct opd *b, uint32_t stride, uint32_t base)
{
    struct opd x = scratch_dst(g, 0, 1), ax = scalar_of(a, 0);
    if (b) {
        struct opd bx = scalar_of(b, 0), k = imm_opd(stride);
        emit3(g, "UMAD", &x, &ax, &k, &bx);
    } else {
        emit1(g, "MOV", false, &x, &ax);
    }
    if (base) {
        struct opd tx = temp(g->s0, "xxxx"), k = imm_opd(base);
        emit2(g, "UADD", &x, &tx, &k);
    }
}

static const char *mask_text(unsigned m)
{
    static const char *const t[16] = {"", ".x", ".y", ".xy", ".z", ".xz", ".yz", ".xyz", ".w", ".xw", ".yw", ".xyw",
                                      ".zw", ".xzw", ".yzw", ".xyzw"};
    return t[m & 15];
}

/* the loaded value in TEMP[s0 + 2] to the destination, through the resource's swizzle */
static void from_load(struct tg *g, const struct opd *dst, const struct opd *res)
{
    struct opd from = temp(g->s0 + 2, "xyzw");
    for (int i = 0; i < 4; i++)
        from.swz[i] = res->swz[i];
    emit1(g, "MOV", false, dst, &from);
}

static void memory_op(struct tg *g, const struct dxbc_ins *in)
{
    const struct opd *o = in->o;
    struct text name = {0};
    uint32_t base, stride;
    switch (in->op) {
    case OP_IMM_ATOMIC_ALLOC: /* the view's hidden counter, a word in BUFFER[24 + u]: the old value, or the new one */
    case OP_IMM_ATOMIC_CONSUME: {
        int u = o[1].idx[0].imm;
        if (o[1].type != T_UAV || u < 0 || u >= 8 || !g->sh->uav_kind[u]) {
            fail(g, "a counter on a view that is not a buffer");
            break;
        }
        g->used_buf |= 1u << (24 + u), g->used_counter |= (uint8_t)(1u << u);
        bool add = in->op == OP_IMM_ATOMIC_ALLOC;
        struct opd at = scratch_dst(g, 0, 1), v = scratch_dst(g, 1, 1), zero = imm_opd(0);
        struct opd step = imm_opd(add ? 1 : 0xffffffffu), r = scratch_dst(g, 2, 1), rx = temp(g->s0 + 2, "xxxx");
        emit1(g, "MOV", false, &at, &zero);
        emit1(g, "MOV", false, &v, &step);
        put(&g->code, "ATOMUADD ");
        put_temp(g, 2, ".x, BUFFER[");
        putu(&g->code, 24u + (uint32_t)u);
        put(&g->code, "], ");
        put_temp(g, 0, ".xxxx, ");
        put_temp(g, 1, ".xxxx\n");
        if (!add)
            emit2(g, "UADD", &r, &rx, &step);
        if (o[0].type != T_NULL)
            emit1(g, "MOV", false, &o[0], &rx);
        break;
    }
    case OP_LD_RAW:
    case OP_LD_STRUCTURED: {
        bool st = in->op == OP_LD_STRUCTURED;
        const struct opd *res = &o[st ? 3 : 2];
        if (o[0].type == T_NULL || !memory_of(g, res, &name, &base, &stride))
            break;
        byte_address(g, &o[1], st ? &o[2] : NULL, stride, base);
        put(&g->code, "LOAD ");
        put_temp(g, 2, "");
        put(&g->code, ", ");
        put(&g->code, name.p);
        put(&g->code, ", ");
        put_temp(g, 0, ".xxxx\n");
        from_load(g, &o[0], res);
        break;
    }
    case OP_STORE_RAW:
    case OP_STORE_STRUCTURED: {
        bool st = in->op == OP_STORE_STRUCTURED;
        if (!memory_of(g, &o[0], &name, &base, &stride))
            break;
        struct opd v = scratch_dst(g, 1, 15);
        emit1(g, "MOV", false, &v, &o[st ? 3 : 2]);
        byte_address(g, &o[1], st ? &o[2] : NULL, stride, base);
        put(&g->code, "STORE ");
        put(&g->code, name.p);
        put(&g->code, mask_text(o[0].mask));
        put(&g->code, ", ");
        put_temp(g, 0, ".xxxx, ");
        put_temp(g, 1, "\n");
        break;
    }
    case OP_LD_UAV_TYPED:
    case OP_STORE_UAV_TYPED: {
        bool load = in->op == OP_LD_UAV_TYPED;
        const struct opd *u = load ? &o[2] : &o[0];
        int i = u->idx[0].imm;
        if (u->type != T_UAV || i < 0 || i >= DXBC_MAX_UAV || g->sh->uav_kind[i] || !g->sh->uav_dim[i]) {
            fail(g, "an undeclared typed UAV");
            break;
        }
        if (load && o[0].type == T_NULL)
            break;
        g->used_img |= 1u << i;
        struct opd c = scratch_dst(g, 0, 15), v = scratch_dst(g, 1, 15);
        emit1(g, "MOV", false, &c, &o[1]);
        if (!load)
            emit1(g, "MOV", false, &v, &o[2]);
        if (load) {
            put(&g->code, "LOAD ");
            put_temp(g, 2, ", IMAGE[");
        } else {
            put(&g->code, "STORE IMAGE[");
        }
        putu(&g->code, (uint32_t)i);
        put(&g->code, "], ");
        put_temp(g, 0, load ? "" : ", ");
        if (!load)
            put_temp(g, 1, "");
        put(&g->code, ", ");
        put(&g->code, image_target(g, i));
        put(&g->code, ", PIPE_FORMAT_");
        put(&g->code, image_format(g, i));
        put(&g->code, "\n");
        if (load)
            from_load(g, &o[0], u);
        break;
    }
    case OP_BUFINFO: {
        const struct opd *res = &o[1];
        if (o[0].type == T_NULL)
            break;
        int i = res->idx[0].imm;
        bool typed = res->type == T_UAV && i >= 0 && i < DXBC_MAX_UAV && !g->sh->uav_kind[i];
        if (typed) {
            if (g->sh->uav_dim[i] != 1) {
                fail(g, "bufinfo on a texture");
                break;
            }
            g->used_img |= 1u << i;
            put(&g->code, "RESQ ");
            put_temp(g, 2, ", IMAGE[");
            putu(&g->code, (uint32_t)i);
            put(&g->code, "], BUFFER, PIPE_FORMAT_");
            put(&g->code, image_format(g, i));
            put(&g->code, "\n");
        } else {
            if (!memory_of(g, res, &name, &base, &stride) || res->type == T_TGSM) {
                fail(g, "bufinfo on shared memory");
                break;
            }
            put(&g->code, "RESQ ");
            put_temp(g, 2, ", ");
            put(&g->code, name.p);
            put(&g->code, "\n");
            if (stride) { /* structured: elements, not bytes */
                struct opd x = scratch_dst(g, 2, 1), tx = temp(g->s0 + 2, "xxxx"), k = imm_opd(stride);
                emit2(g, "UDIV", &x, &tx, &k);
            }
        }
        struct opd d = o[0], x = temp(g->s0 + 2, "xxxx");
        emit1(g, "MOV", false, &d, &x);
        break;
    }
    case OP_SYNC: {
        unsigned f = in->ret_type, m = 0;
        if (f & 2)
            m |= 8; /* shared memory */
        if (f & 12)
            m |= 1 | 4 | (f & 8 ? 0 : 16); /* buffers and images, for the group or everyone */
        if (m) {
            int k = immediate(g, m, m, m, m);
            put(&g->code, "MEMBAR IMM[");
            putu(&g->code, (uint32_t)k);
            put(&g->code, "].xxxx\n");
        }
        if (f & 1)
            line(g, "BARRIER");
        break;
    }
    default: { /* the atomics */
        static const struct {
            uint16_t op, imm_op;
            const char *name;
        } atomics[] = {{OP_ATOMIC_AND, OP_IMM_ATOMIC_AND, "ATOMAND"},     {OP_ATOMIC_OR, OP_IMM_ATOMIC_OR, "ATOMOR"},
                       {OP_ATOMIC_XOR, OP_IMM_ATOMIC_XOR, "ATOMXOR"},
                       {OP_ATOMIC_IADD, OP_IMM_ATOMIC_IADD, "ATOMUADD"},
                       {OP_ATOMIC_IMAX, OP_IMM_ATOMIC_IMAX, "ATOMIMAX"},
                       {OP_ATOMIC_IMIN, OP_IMM_ATOMIC_IMIN, "ATOMIMIN"},
                       {OP_ATOMIC_UMAX, OP_IMM_ATOMIC_UMAX, "ATOMUMAX"},
                       {OP_ATOMIC_UMIN, OP_IMM_ATOMIC_UMIN, "ATOMUMIN"},
                       {OP_ATOMIC_CMP_STORE, OP_IMM_ATOMIC_CMP_EXCH, "ATOMCAS"},
                       {0xffff, OP_IMM_ATOMIC_EXCH, "ATOMXCHG"}};
        int a = -1;
        bool ret = false;
        for (int k = 0; k < (int)(sizeof(atomics) / sizeof(atomics[0])); k++)
            if (atomics[k].op == in->op || atomics[k].imm_op == in->op)
                a = k, ret = atomics[k].imm_op == in->op;
        if (a < 0) {
            fail(g, "an instruction without a TGSI translation");
            break;
        }
        bool cas = !strcmp(atomics[a].name, "ATOMCAS");
        const struct opd *res = &o[ret ? 1 : 0], *addr = &o[ret ? 2 : 1];
        const struct opd *cmp = cas ? &o[ret ? 3 : 2] : NULL, *val = &o[ret ? (cas ? 4 : 3) : (cas ? 3 : 2)];
        struct opd v = scratch_dst(g, 1, 1), vx = scalar_of(val, 0), c = scratch_dst(g, 3, 1);
        emit1(g, "MOV", false, &v, &vx);
        if (cas) {
            struct opd cx = scalar_of(cmp, 0);
            emit1(g, "MOV", false, &c, &cx);
        }
        int i = res->idx[0].imm;
        bool typed = res->type == T_UAV && i >= 0 && i < DXBC_MAX_UAV && !g->sh->uav_kind[i];
        if (typed) {
            if (!g->sh->uav_dim[i]) {
                fail(g, "an undeclared typed UAV");
                break;
            }
            g->used_img |= 1u << i;
            struct opd at = scratch_dst(g, 0, 15);
            emit1(g, "MOV", false, &at, addr);
            put(&name, "IMAGE[");
            putu(&name, (uint32_t)i);
            put(&name, "]");
        } else {
            if (!memory_of(g, res, &name, &base, &stride))
                break;
            struct opd y = scalar_of(addr, 1);
            byte_address(g, addr, stride ? &y : NULL, stride, base);
        }
        put(&g->code, atomics[a].name);
        put(&g->code, " ");
        put_temp(g, 2, ".x, ");
        put(&g->code, name.p);
        put(&g->code, ", ");
        put_temp(g, 0, typed ? ", " : ".xxxx, ");
        if (cas)
            put_temp(g, 3, ".xxxx, ");
        put_temp(g, 1, ".xxxx");
        if (typed) {
            put(&g->code, ", ");
            put(&g->code, image_target(g, i));
            put(&g->code, ", PIPE_FORMAT_");
            put(&g->code, image_format(g, i));
        }
        put(&g->code, "\n");
        if (ret && o[0].type != T_NULL) {
            struct opd x = temp(g->s0 + 2, "xxxx");
            emit1(g, "MOV", false, &o[0], &x);
        }
        break;
    }
    }
    kfree(name.p);
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
    case OP_LD_RAW: case OP_LD_STRUCTURED: case OP_STORE_RAW: case OP_STORE_STRUCTURED: case OP_LD_UAV_TYPED:
    case OP_STORE_UAV_TYPED: case OP_BUFINFO: case OP_SYNC: case OP_ATOMIC_AND: case OP_ATOMIC_OR: case OP_ATOMIC_XOR:
    case OP_ATOMIC_CMP_STORE: case OP_ATOMIC_IADD: case OP_ATOMIC_IMAX: case OP_ATOMIC_IMIN: case OP_ATOMIC_UMAX:
    case OP_ATOMIC_UMIN: case OP_IMM_ATOMIC_IADD: case OP_IMM_ATOMIC_AND: case OP_IMM_ATOMIC_OR: case OP_IMM_ATOMIC_XOR:
    case OP_IMM_ATOMIC_EXCH: case OP_IMM_ATOMIC_CMP_EXCH: case OP_IMM_ATOMIC_IMAX: case OP_IMM_ATOMIC_IMIN:
    case OP_IMM_ATOMIC_UMAX: case OP_IMM_ATOMIC_UMIN: case OP_IMM_ATOMIC_ALLOC: case OP_IMM_ATOMIC_CONSUME:
        return memory_op(g, in);
    case OP_EMIT:
    case OP_CUT:
    case OP_EMITTHENCUT:
    case OP_EMIT_STREAM:
    case OP_CUT_STREAM:
    case OP_EMITTHENCUT_STREAM: {
        bool streamed = in->op == OP_EMIT_STREAM || in->op == OP_CUT_STREAM || in->op == OP_EMITTHENCUT_STREAM;
        uint32_t stream = streamed ? (uint32_t)o[0].idx[0].imm : 0;
        int k = immediate(g, stream, stream, stream, stream);
        for (int pass = 0; pass < 2; pass++) {
            bool emit_now = pass == 0 && in->op != OP_CUT && in->op != OP_CUT_STREAM;
            bool cut_now = pass == 1 && in->op != OP_EMIT && in->op != OP_EMIT_STREAM;
            if (!emit_now && !cut_now)
                continue;
            put(&g->code, emit_now ? "EMIT IMM[" : "ENDPRIM IMM[");
            putu(&g->code, (uint32_t)k);
            put(&g->code, "].xxxx\n");
        }
        return;
    }
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
    case OP_UADDC:
    case OP_USUBB: { /* the sum or difference and the carry or borrow (1), worked out before either is written */
        struct opd sum = scratch_dst(g, 0, 15), carry = scratch_dst(g, 1, 15), one = imm_opd(1);
        struct opd s0 = temp(g->s0, "xyzw"), s1 = temp(g->s0 + 1, "xyzw");
        if (in->op == OP_UADDC) {
            emit2(g, "UADD", &sum, &o[2], &o[3]);
            emit2(g, "USLT", &carry, &s0, &o[2]);
        } else {
            emit2(g, "USLT", &carry, &o[2], &o[3]);
            struct opd neg = scratch_dst(g, 2, 15), n2 = temp(g->s0 + 2, "xyzw");
            emit1(g, "INEG", false, &neg, &o[3]);
            emit2(g, "UADD", &sum, &o[2], &n2);
        }
        emit2(g, "AND", &carry, &s1, &one);
        if (o[0].type != T_NULL)
            emit1(g, "MOV", false, &o[0], &s0);
        if (o[1].type != T_NULL)
            emit1(g, "MOV", false, &o[1], &s1);
        return;
    }
    case OP_SWAPC: { /* d0 = cond ? b : a, d1 = cond ? a : b */
        struct opd x = scratch_dst(g, 0, 15), y = scratch_dst(g, 1, 15);
        emit3(g, "UCMP", &x, &o[2], &o[4], &o[3]);
        emit3(g, "UCMP", &y, &o[2], &o[3], &o[4]);
        struct opd s0 = temp(g->s0, "xyzw"), s1 = temp(g->s0 + 1, "xyzw");
        if (o[0].type != T_NULL)
            emit1(g, "MOV", false, &o[0], &s0);
        if (o[1].type != T_NULL)
            emit1(g, "MOV", false, &o[1], &s1);
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
        if (in->op == OP_RETC && g->sh->stage == DXBC_HS)
            return (void)fail(g, "a return inside a hull shader phase");
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
        if (g->sh->stage == DXBC_HS) { /* the end of a phase; returning early would skip the phases after it */
            for (int k = pc + 1; k < g->phase_end; k++)
                if (g->sh->code[k].op != OP_NOP && g->sh->code[k].op != OP_RET)
                    return (void)fail(g, "a return inside a hull shader phase");
            return;
        }
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
        return texture(g, in, TX_SAMPLE);
    case OP_SAMPLE_L:
        return texture(g, in, TX_L);
    case OP_SAMPLE_B:
        return texture(g, in, TX_B);
    case OP_SAMPLE_C:
        return texture(g, in, TX_C);
    case OP_SAMPLE_C_LZ:
        return texture(g, in, TX_C_LZ);
    case OP_SAMPLE_D:
        return texture(g, in, TX_D);
    case OP_LD:
        return texture(g, in, TX_LD);
    case OP_LOD:
        return texture(g, in, TX_LOD);
    case OP_GATHER4:
        return texture(g, in, TX_G);
    case OP_GATHER4_C:
        return texture(g, in, TX_G_C);
    case OP_GATHER4_PO:
        return texture(g, in, TX_G_PO);
    case OP_GATHER4_PO_C:
        return texture(g, in, TX_G_PO_C);
    case OP_RESINFO: { /* TXQ gives integers: floats unless the instruction asks for _uint */
        const struct opd *res = &in->o[2];
        int t = res->idx[0].imm;
        if (o[0].type == T_NULL)
            return;
        if (res->type != T_RESOURCE || t < 0 || t >= DXBC_MAX_RES || !g->sh->res_dim[t])
            return (void)fail(g, "undeclared texture");
        if (in->ret_type == 1)
            return (void)fail(g, "resinfo_rcpFloat");
        g->used_tex |= 1u << t;
        struct opd d = scratch_dst(g, 2, 15), mip = scalar_of(&o[1], 0), m = scratch_dst(g, 0, 15);
        emit1(g, "MOV", false, &m, &mip);
        g->naddr = 0;
        put(&g->code, "TXQ");
        dst_text(g, &d, 0);
        put(&g->code, ", TEMP[");
        putu(&g->code, (uint32_t)g->s0);
        put(&g->code, "], SAMP[");
        putu(&g->code, (uint32_t)t);
        put(&g->code, "], ");
        put(&g->code, (g->cmp_tex >> t & 1) ? shadow_name[g->sh->res_dim[t]] : target_name[g->sh->res_dim[t]]);
        put(&g->code, "\n");
        struct opd from = temp(g->s0 + 2, "xyzw");
        for (int i = 0; i < 4; i++)
            from.swz[i] = res->swz[i];
        emit1(g, in->ret_type == 2 ? "MOV" : "U2F", false, &o[0], &from);
        return;
    }
    case OP_SAMPLE_INFO: /* the host draws one sample a pixel, as the software pipeline does */
    case OP_SAMPLE_POS: {
        if (o[0].type == T_NULL)
            return;
        uint32_t v = in->op == OP_SAMPLE_POS ? 0 : (in->ret_type & 1) ? 1 : 0x3f800000u;
        struct opd k = temp(0, "xyzw");
        k.type = T_IMM32, k.ndim = 0;
        k.imm[0] = v, k.imm[1] = k.imm[2] = k.imm[3] = 0;
        emit1(g, "MOV", in->sat, &o[0], &k);
        return;
    }
    case OP_EVAL_CENTROID:
    case OP_EVAL_SAMPLE_INDEX:
    case OP_EVAL_SNAPPED: {
        if (o[0].type == T_NULL)
            return;
        int r = o[1].idx[0].imm;
        bool plain = g->sh->stage != DXBC_PS || o[1].type != T_INPUT || o[1].ndim != 1 || o[1].idx[0].rel_type >= 0 ||
                     r < 0 || r >= 32 || r == g->face || r == g->sh->pos_in_reg || g->sv_in[r] >= 0;
        if (plain) { /* not an interpolated input: its value */
            emit1(g, "MOV", in->sat, &o[0], &o[1]);
            return;
        }
        if (in->op == OP_EVAL_CENTROID) {
            emit1(g, "INTERP_CENTROID", in->sat, &o[0], &o[1]);
        } else if (in->op == OP_EVAL_SAMPLE_INDEX) {
            struct opd i = scalar_of(&o[2], 0);
            const struct opd *src[2] = {&o[1], &i};
            emit(g, "INTERP_SAMPLE", in->sat, &o[0], src, 2);
        } else { /* an offset in sixteenths of a pixel, as integers */
            struct opd f = scratch_dst(g, 0, 3), sixteenth = imm_opd(0x3d800000u);
            emit1(g, "I2F", false, &f, &o[2]);
            struct opd fs = temp(g->s0, "xyxy");
            emit2(g, "MUL", &f, &fs, &sixteenth);
            const struct opd *src[2] = {&o[1], &fs};
            emit(g, "INTERP_OFFSET", in->sat, &o[0], src, 2);
        }
        return;
    }
    default:
        fail(g, "an instruction without a TGSI translation");
    }
}

/* ---- hull and domain shaders */

enum { TESS_OUTER = 32, TESS_INNER = 33, TESS_PATCH = 34 }; /* IN/OUT registers of the patch constants */

/* where a patch constant lives on the host: OUT/IN[reg] and the component, or the generic PATCH[reg] (comp -1) */
static int pc_place(const struct dxbc_sig_elem *e, int *comp)
{
    switch (e->system_value) {
    case 11: case 13: *comp = (int)e->index; return TESS_OUTER;  /* quad, triangle edges */
    case 12: case 14: *comp = (int)e->index; return TESS_INNER;  /* inside */
    case 15: *comp = 1; return TESS_OUTER;                        /* line detail: segments a line */
    case 16: *comp = 0; return TESS_OUTER;                        /* line density: lines */
    default: *comp = -1; return TESS_PATCH + (int)e->reg;
    }
}

static int low_comp(uint32_t mask)
{
    for (int c = 0; c < 4; c++)
        if (mask >> c & 1)
            return c;
    return 0;
}

/* the hull shader as one TESS_CTRL program: the control point phase in every invocation (or the input points passed
   through), then in invocation 0 each fork and join phase instance in turn, the patch constants in temps, written
   out at the end (the host takes tessellation factors one component at a time) */
static void hull_body(struct tg *g)
{
    const struct dxbc_shader *sh = g->sh;
    int cp = -1;
    for (int k = 0; k < sh->nphase; k++)
        if (sh->phase[k].kind == 0)
            cp = k;
    if (cp >= 0) {
        g->phase = 0, g->phase_inst = 0;
        g->phase_end = cp + 1 < sh->nphase ? sh->phase[cp + 1].start : sh->ncode;
        for (int pc = sh->phase[cp].start; pc < g->phase_end && !g->fail; pc++)
            instruction(g, pc);
    } else {
        uint32_t done = 0;
        put(&g->code, "UARL ADDR[0].x, SV[0].xxxx\n");
        for (int i = 0; i < sh->nout; i++) {
            uint32_t r = sh->out[i].reg;
            if (r >= 32 || (done >> r & 1))
                continue;
            done |= 1u << r;
            put(&g->code, "MOV OUT[0][");
            putu(&g->code, r);
            put(&g->code, "], IN[ADDR[0].x+0][");
            putu(&g->code, r);
            put(&g->code, "]\n");
        }
    }
    int zero = immediate(g, 0, 0, 0, 0);
    put(&g->code, "USEQ ");
    put_temp(g, 0, ".x, SV[0].xxxx, IMM[");
    putu(&g->code, (uint32_t)zero);
    put(&g->code, "].xxxx\nUIF ");
    put_temp(g, 0, ".xxxx\n");
    for (int k = 0; k < sh->nphase && !g->fail; k++) {
        if (sh->phase[k].kind == 0)
            continue;
        g->phase = sh->phase[k].kind;
        g->phase_end = k + 1 < sh->nphase ? sh->phase[k + 1].start : sh->ncode;
        for (uint32_t i = 0; i < sh->phase[k].instances && !g->fail; i++) {
            g->phase_inst = (int)i;
            for (int pc = sh->phase[k].start; pc < g->phase_end && !g->fail; pc++)
                instruction(g, pc);
        }
    }
    g->phase = 0;
    uint32_t max_bits; /* dcl_hs_maxtessfactor, up to 64 (as bits: positive floats order as integers) */
    memcpy(&max_bits, &sh->max_tess, 4);
    if (max_bits == 0 || max_bits >= 0x42800000u)
        max_bits = 0x42800000u;
    int cap = immediate(g, max_bits, max_bits, max_bits, max_bits);
    for (int i = 0; i < sh->npc; i++) {
        const struct dxbc_sig_elem *e = &sh->pc[i];
        int comp, reg = pc_place(e, &comp);
        if ((int)e->reg >= g->npc || comp > 3)
            continue;
        if (comp < 0) /* below, with the whole registers */
            continue;
        struct text *t = &g->code;
        put(t, comp >= 0 ? "MIN OUT[" : "MOV OUT[");
        putu(t, (uint32_t)reg);
        put(t, "]");
        if (comp >= 0) {
            char m[3] = {'.', "xyzw"[comp], 0};
            put(t, m);
        } else if (e->mask != 15) {
            put(t, mask_text(e->mask));
        }
        put(t, ", TEMP[");
        putu(t, (uint32_t)(g->pcbase + (int)e->reg));
        put(t, "](9)");
        if (comp >= 0) {
            char c = "xyzw"[low_comp(e->mask)];
            char sw[6] = {'.', c, c, c, c, 0};
            put(t, sw);
            put(t, ", IMM[");
            putu(t, (uint32_t)cap);
            put(t, "].xxxx");
        } else {
            put(t, ".xyzw");
        }
        put(t, "\n");
    }
    /* every patch constant register as a patch varying for the domain shader (which also reads the factors from
       there: the host's domain shader cannot read the tessellation levels back) */
    for (int r = 0; r < g->npc; r++) {
        put(&g->code, "MOV OUT[");
        putu(&g->code, (uint32_t)(TESS_PATCH + r));
        put(&g->code, "], TEMP[");
        putu(&g->code, (uint32_t)(g->pcbase + r));
        put(&g->code, "](9).xyzw\n");
    }
    put(&g->code, "ENDIF\n");
}

/* the domain shader's patch constants into their temps first */
static void domain_prologue(struct tg *g)
{
    for (int r = 0; r < g->npc; r++) {
        put(&g->code, "MOV TEMP[");
        putu(&g->code, (uint32_t)(g->pcbase + r));
        put(&g->code, "](9), IN[");
        putu(&g->code, (uint32_t)(TESS_PATCH + r));
        put(&g->code, "].xyzw\n");
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

/* Clip and cull distances: GL keeps them in one array, clip distances first, that TGSI spreads over CLIPDIST[0]
   and [1]. The shader writes them straight to its output registers, so the registers must hold exactly that array:
   clip distances from .x of the first, cull distances right after, and nothing else in them. */
static bool clip_layout(struct tg *g)
{
    const struct dxbc_shader *sh = g->sh;
    uint8_t kind[32][4] = {{0}}; /* 1 clip, 2 cull, 3 anything else */
    uint32_t regs = 0;
    g->clip_reg[0] = g->clip_reg[1] = -1;
    g->nclip = g->ncull = 0;
    if (sh->stage != DXBC_VS && sh->stage != DXBC_GS && sh->stage != DXBC_DS)
        return true;
    for (int i = 0; i < sh->nout; i++) {
        uint32_t r = sh->out[i].reg, sv = sh->out[i].system_value;
        if (r >= 32)
            continue;
        if (sv == 2 || sv == 3)
            regs |= 1u << r;
        for (int c = 0; c < 4; c++)
            if (sh->out[i].mask >> c & 1)
                kind[r][c] = sv == 2 ? 1 : sv == 3 ? 2 : 3;
    }
    if (!regs)
        return true;
    uint8_t seq[8] = {0};
    int n = 0;
    for (int r = 0; r < 32; r++)
        if (regs >> r & 1) {
            if (n == 2)
                return fail(g, "clip and cull distances in more than two registers");
            g->clip_reg[n] = r;
            memcpy(seq + 4 * n++, kind[r], 4);
        }
    int k = 0;
    while (k < 8 && seq[k] == 1)
        g->nclip++, k++;
    while (k < 8 && seq[k] == 2)
        g->ncull++, k++;
    while (k < 8 && seq[k] == 0)
        k++;
    if (k < 8 || (n == 2 && g->nclip + g->ncull <= 4))
        return fail(g, "clip and cull distances laid out unlike GL");
    return true;
}

static bool declarations(struct tg *g, struct text *t)
{
    const struct dxbc_shader *sh = g->sh;
    if (!clip_layout(g))
        return false;
    g->out->clip_planes = (uint8_t)((1u << g->nclip) - 1);
    put(t, sh->stage == DXBC_VS   ? "VERT\n"
           : sh->stage == DXBC_GS ? "GEOM\n"
           : sh->stage == DXBC_CS ? "COMP\n"
           : sh->stage == DXBC_HS ? "TESS_CTRL\n"
           : sh->stage == DXBC_DS ? "TESS_EVAL\n"
                                  : "FRAG\n");
    if (sh->stage == DXBC_HS) {
        put(t, "PROPERTY TCS_VERTICES_OUT ");
        putu(t, sh->cp_out ? sh->cp_out : sh->cp_in ? sh->cp_in : 1);
        put(t, "\n");
    }
    if (sh->stage == DXBC_DS) {
        /* spacing and winding come from the hull shader; D3D's domain has its origin at the top left, GL's at the
           bottom left, so the same triangles turn the other way */
        static const char *const prim[4] = {"", "1", "4", "7"}; /* gallium's LINES, TRIANGLES, QUADS */
        static const char spacing[5] = {'2', '2', '2', '0', '1'}; /* integer, pow2: equal; fractional odd, even */
        const struct dxbc_shader *hs = g->hs;
        if (!hs || sh->domain < 1 || sh->domain > 3 || hs->partitioning > 4)
            return fail(g, "a domain shader without its hull shader");
        put(t, "PROPERTY TES_PRIM_MODE ");
        put(t, prim[sh->domain]);
        put(t, "\nPROPERTY TES_SPACING ");
        char sp[2] = {spacing[hs->partitioning], 0};
        put(t, sp);
        put(t, "\nPROPERTY TES_VERTEX_ORDER_CW ");
        put(t, hs->out_prim == 4 ? "1" : "0");
        put(t, "\nPROPERTY TES_POINT_MODE ");
        put(t, hs->out_prim == 1 ? "1" : "0");
        put(t, "\n");
    }
    if (sh->stage == DXBC_CS) {
        static const char *const dim[3] = {"WIDTH", "HEIGHT", "DEPTH"};
        for (int i = 0; i < 3; i++) {
            put(t, "PROPERTY CS_FIXED_BLOCK_");
            put(t, dim[i]);
            put(t, " ");
            putu(t, sh->group[i] ? sh->group[i] : 1);
            put(t, "\n");
        }
        decl_reg(t, "SV", 0, ", THREAD_ID");
        decl_reg(t, "SV", 1, ", BLOCK_ID");
    }
    if (sh->stage == DXBC_GS) {
        static const char *const in_prim[] = {"", "POINTS", "LINES", "TRIANGLES", "", "", "LINES_ADJACENCY",
                                              "TRIANGLES_ADJACENCY"};
        static const char *const out_prim[] = {"", "POINTS", "", "LINE_STRIP", "", "TRIANGLE_STRIP"};
        if (sh->gs_in >= 8 || !in_prim[sh->gs_in][0] || sh->gs_out >= 6 || !out_prim[sh->gs_out][0])
            return fail(g, "a geometry shader primitive");
        put(t, "PROPERTY GS_INPUT_PRIMITIVE ");
        put(t, in_prim[sh->gs_in]);
        put(t, "\nPROPERTY GS_OUTPUT_PRIMITIVE ");
        put(t, out_prim[sh->gs_out]);
        put(t, "\nPROPERTY GS_MAX_OUTPUT_VERTICES ");
        putu(t, sh->gs_max ? sh->gs_max : 1);
        put(t, "\nPROPERTY GS_INVOCATIONS ");
        putu(t, sh->gs_instances ? sh->gs_instances : 1);
        put(t, "\n");
    }
    if (sh->stage == DXBC_PS)
        put(t, "PROPERTY FS_COORD_ORIGIN UPPER_LEFT\nPROPERTY FS_COORD_PIXEL_CENTER HALF_INTEGER\n");
    if (g->nclip + g->ncull) {
        put(t, "PROPERTY NUM_CLIPDIST_ENABLED ");
        putu(t, (uint32_t)g->nclip);
        put(t, "\nPROPERTY NUM_CULLDIST_ENABLED ");
        putu(t, (uint32_t)g->ncull);
        put(t, "\n");
    }
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
    } else if (sh->stage == DXBC_GS) {
        for (int i = 0; i < sh->nin; i++) {
            uint32_t r = sh->in[i].reg, sv = sh->in[i].system_value;
            if (r >= 32 || (done & (1u << r)))
                continue;
            done |= 1u << r;
            put(t, "DCL IN[][");
            putu(t, r);
            if (sv == 1) {
                put(t, "], POSITION\n");
            } else if (sv == 0) {
                put(t, "], GENERIC[");
                putu(t, r);
                put(t, "]\n");
            } else {
                return fail(g, "a geometry shader system value input");
            }
        }
        if (g->gs_sv & 1)
            decl_reg(t, "SV", 0, ", PRIMID");
        if (g->gs_sv & 2)
            decl_reg(t, "SV", 1, ", INVOCATIONID");
    } else if (sh->stage == DXBC_HS || sh->stage == DXBC_DS) {
        for (int i = 0; i < sh->nin; i++) { /* the control points */
            uint32_t r = sh->in[i].reg, sv = sh->in[i].system_value;
            if (r >= 32 || (done & (1u << r)))
                continue;
            done |= 1u << r;
            if (sv != 0 && sv != 1)
                return fail(g, "a control point system value");
            put(t, "DCL IN[][");
            putu(t, r);
            put(t, sv == 1 ? "], POSITION\n" : "], GENERIC[");
            if (sv != 1) {
                putu(t, r);
                put(t, "]\n");
            }
        }
        if (sh->stage == DXBC_HS) {
            decl_reg(t, "SV", 0, ", INVOCATIONID");
            if (g->hs_primid)
                decl_reg(t, "SV", 1, ", PRIMID");
        } else {
            decl_reg(t, "SV", 0, ", TESSCOORD");
        }
        /* the patch constants: every register as a patch varying, and the hull shader's tessellation factors */
        bool outer = false, inner = false;
        for (int i = 0; i < sh->npc && sh->stage == DXBC_HS; i++) {
            int comp, reg = pc_place(&sh->pc[i], &comp);
            outer |= reg == TESS_OUTER, inner |= reg == TESS_INNER;
        }
        if (outer)
            decl_reg(t, "OUT", TESS_OUTER, ", TESSOUTER");
        if (inner)
            decl_reg(t, "OUT", TESS_INNER, ", TESSINNER");
        for (int r = 0; r < g->npc; r++) {
            put(t, sh->stage == DXBC_HS ? "DCL OUT[" : "DCL IN[");
            putu(t, (uint32_t)(TESS_PATCH + r));
            put(t, "], PATCH[");
            putu(t, (uint32_t)r);
            put(t, "]\n");
        }
    } else {
        for (int i = 0; i < sh->nin; i++) {
            uint32_t r = sh->in[i].reg;
            if (r >= 32 || (done & (1u << r)))
                continue;
            if ((int)r == sh->pos_in_reg || sh->in[i].system_value == 1) {
                done |= 1u << r;
                decl_reg(t, "IN", (int)r, ", POSITION, LINEAR");
                continue;
            }
            if ((int)r == g->face) { /* gl_FrontFacing as +1 or -1, made a D3D boolean at the start */
                done |= 1u << r;
                decl_reg(t, "IN", (int)r, ", FACE, CONSTANT");
                continue;
            }
            if (sh->in[i].system_value && sh->in[i].system_value != 1) {
                if (g->sv_in[r] >= 0)
                    continue;
                if (!sh->interp[r])
                    continue; /* not read */
                return fail(g, "a pixel shader system value input");
            }
            done |= 1u << r;
            put(t, "DCL IN[");
            putu(t, r);
            put(t, "], GENERIC[");
            putu(t, r);
            put(t, "]");
            put(t, interp_name(sh->interp[r]));
            put(t, "\n");
        }
        for (int k = 0; k < g->nsv; k++)
            decl_reg(t, "SV", k, g->sv_name[k] == 7 ? ", PRIMID" : ", SAMPLEID");
    }
    done = 0;
    for (int i = 0; i < sh->nout && sh->stage == DXBC_HS; i++) {
        uint32_t r = sh->out[i].reg, sv = sh->out[i].system_value;
        if (r >= 32 || (done & (1u << r)))
            continue;
        done |= 1u << r;
        if (sv != 0 && sv != 1)
            return fail(g, "a control point system value output");
        put(t, "DCL OUT[][");
        putu(t, r);
        put(t, sv == 1 ? "], POSITION\n" : "], GENERIC[");
        if (sv != 1) {
            putu(t, r);
            put(t, "]\n");
        }
    }
    for (int i = 0; i < sh->nout && sh->stage != DXBC_HS; i++) {
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
        } else if ((int)r == g->clip_reg[0] || (int)r == g->clip_reg[1]) {
            put(t, "], CLIPDIST[");
            putu(t, (int)r == g->clip_reg[0] ? 0u : 1u);
            put(t, "]\n");
        } else if (sv == 4 || sv == 5) {
            put(t, sv == 4 ? "], LAYER\n" : "], VIEWPORT_INDEX\n");
        } else if (sv == 0) {
            put(t, "], GENERIC[");
            putu(t, r);
            put(t, "]\n");
        } else {
            return fail(g, "a vertex shader system value output");
        }
    }
    if (sh->depth_out)
        decl_reg(t, "OUT", g->depth_reg, ", POSITION");
    for (int i = 0; i < 32; i++)
        if (g->used_buf >> i & 1)
            decl_reg(t, "BUFFER", i, "");
    for (int u = 0; u < DXBC_MAX_UAV; u++)
        if (g->used_img >> u & 1) {
            put(t, "DCL IMAGE[");
            putu(t, (uint32_t)u);
            put(t, "], ");
            put(t, image_target(g, u));
            put(t, ", PIPE_FORMAT_");
            put(t, image_format(g, u));
            put(t, ", WR\n");
        }
    if (g->shared)
        decl_reg(t, "MEMORY", 0, ", SHARED");
    int total = g->s0 + (sh->stage == DXBC_CS ? 6 : g->face >= 0 ? 5 : 4);
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
    if (g->npc) { /* the patch constants, indexable */
        put(t, "DCL TEMP[");
        putu(t, (uint32_t)g->pcbase);
        put(t, "..");
        putu(t, (uint32_t)(g->pcbase + g->npc - 1));
        put(t, "], ARRAY(9)\n");
    }
    put(t, "DCL ADDR[0..2]\n");
    for (int s = 0; s < DXBC_MAX_CB; s++)
        if (g->used_cb & (1u << s)) {
            /* the declared size; without a declaration, up to what the shader reads (a block larger than the buffer
               bound to it reads nothing on some hosts) */
            uint32_t size = sh->cb_size[s] ? sh->cb_size[s] : (uint32_t)g->cb_max[s] + 1;
            size = size > 4096 ? 4096 : size;
            put(t, "DCL CONST[");
            putu(t, (uint32_t)s + 1);
            put(t, "][0..");
            putu(t, size - 1);
            put(t, "]\n");
        }
    if (g->out->icb && sh->icb_vec4) {
        put(t, "DCL CONST[15][0..");
        putu(t, (uint32_t)sh->icb_vec4 - 1);
        put(t, "]\n");
    }
    static const char *const ret[] = {"FLOAT", "UNORM", "SNORM", "SINT", "UINT", "FLOAT"};
    for (int r = 0; r < DXBC_MAX_RES; r++)
        if (g->used_tex & (1u << r)) {
            int dim = sh->res_dim[r];
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

bool dxbc_tgsi_tess(const struct dxbc_shader *sh, const struct dxbc_shader *hs, const uint16_t *uav_format,
                    struct dxbc_tgsi *out)
{
    memset(out, 0, sizeof(*out));
    memset(out->tex_sampler, 0xff, sizeof(out->tex_sampler));
    struct tg *g = kmalloc(sizeof(*g));
    if (!g) {
        memcpy(out->error, "out of memory", 14);
        return false;
    }
    memset(g, 0, sizeof(*g));
    g->sh = sh, g->out = out, g->uav_format = uav_format;
    g->hs = hs;
    int next = sh->temps;
    for (int x = 0; x < 8; x++)
        if (sh->xtemps[x])
            g->xbase[x] = next, next += sh->xsize[x];
    g->s0 = next;
    g->pcbase = next + 6;
    for (int i = 0; i < sh->npc; i++)
        if ((int)sh->pc[i].reg + 1 > g->npc && sh->pc[i].reg < 32)
            g->npc = (int)sh->pc[i].reg + 1;
    g->depth_reg = sh->max_out_reg + 1;
    for (int r = 0; r < 32; r++)
        g->sv_in[r] = -1;
    g->face = -1;
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
    if (sh->stage == DXBC_PS) /* SV_PrimitiveID and SV_SampleIndex as system values, SV_IsFrontFace from FACE */
        for (int i = 0; i < sh->nin; i++) {
            uint32_t sv = sh->in[i].system_value, r = sh->in[i].reg;
            if (r >= 32 || g->sv_in[r] >= 0 || (int)r == g->face)
                continue;
            if (sv == 9) {
                g->face = (int)r;
                int zero = immediate(g, 0, 0, 0, 0);
                put(&g->code, "FSLT ");
                put_temp(g, 4, ", IMM[");
                putu(&g->code, (uint32_t)zero);
                put(&g->code, "].xxxx, IN[");
                putu(&g->code, r);
                put(&g->code, "].xxxx\n");
            } else if (sv == 7 || sv == 10) {
                g->sv_name[g->nsv] = (uint8_t)sv;
                g->sv_in[r] = (int8_t)g->nsv++;
            }
        }
    if (sh->stage == DXBC_CS) { /* SV_DispatchThreadID and SV_GroupIndex from the group and the thread in it */
        uint32_t w = sh->group[0], h = sh->group[1];
        int k = immediate(g, w, h, sh->group[2], 0), wk = immediate(g, w, w, w, w), whk = immediate(g, w * h, w * h,
                                                                                                   w * h, w * h);
        put(&g->code, "UMAD ");
        put_temp(g, 4, ".xyz, SV[1].xyzz, IMM[");
        putu(&g->code, (uint32_t)k);
        put(&g->code, "].xyzz, SV[0].xyzz\nUMAD ");
        put_temp(g, 5, ".x, SV[0].yyyy, IMM[");
        putu(&g->code, (uint32_t)wk);
        put(&g->code, "].xxxx, SV[0].xxxx\nUMAD ");
        put_temp(g, 5, ".x, SV[0].zzzz, IMM[");
        putu(&g->code, (uint32_t)whk);
        put(&g->code, "].xxxx, ");
        put_temp(g, 5, ".xxxx\n");
    }
    if (sh->stage == DXBC_HS) {
        hull_body(g);
    } else {
        if (sh->stage == DXBC_DS)
            domain_prologue(g);
        for (int pc = 0; pc < sh->ncode && !g->fail; pc++)
            instruction(g, pc);
    }
    put(&g->code, "END\n");
    if (g->srv_high & g->used_counter)
        fail(g, "a buffer t16 or later and a hidden counter");
    struct text t = {0};
    if (!g->fail)
        declarations(g, &t);
    if (!g->fail)
        put(&t, g->code.p ? g->code.p : "");
    if (g->code.full || t.full)
        fail(g, "out of memory");
    kfree(g->code.p);
    out->used_cb = g->used_cb;
    out->used_uav = (g->used_buf & 0xff) | g->used_img;
    out->used_srv_buf = g->used_buf >> 8 & 0xffff;
    out->used_counter = g->used_counter;
    bool ok = !g->fail;
    kfree(g);
    if (!ok) {
        kfree(t.p);
        return false;
    }
    out->text = t.p, out->len = t.len;
    return true;
}

bool dxbc_tgsi(const struct dxbc_shader *sh, const uint16_t *uav_format, struct dxbc_tgsi *out)
{
    return dxbc_tgsi_tess(sh, NULL, uav_format, out);
}

void dxbc_tgsi_free(struct dxbc_tgsi *out)
{
    kfree(out->text);
    out->text = NULL, out->len = 0;
}

#endif
