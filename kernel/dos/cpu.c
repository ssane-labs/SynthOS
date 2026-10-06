#include <synth/string.h>

#include "cpu.h"

enum { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };

struct ea {
    bool is_reg;
    uint8_t reg;
    uint8_t rm;
    uint16_t seg;
    uint16_t off;
};

struct decoder {
    int seg_override;
    int rep;
};

static uint8_t parity_table[256];

static void init_parity(void)
{
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int b = 0; b < 8; b++)
            bits += (i >> b) & 1;
        parity_table[i] = (bits & 1) ? 0 : F_PF;
    }
}

void cpu_reset(struct cpu *c)
{
    init_parity();
    memset(c->r, 0, sizeof(c->r));
    memset(c->s, 0, sizeof(c->s));
    c->ip = 0;
    c->flags = 0xf002;
    c->stop = false;
    c->executed = 0;
}

static inline void set_flag(struct cpu *c, uint16_t f, bool on)
{
    if (on)
        c->flags |= f;
    else
        c->flags &= (uint16_t)~f;
}

static inline bool flag(struct cpu *c, uint16_t f)
{
    return (c->flags & f) != 0;
}

static inline uint8_t fetch8(struct cpu *c)
{
    return cpu_rb(c, cpu_lin(c->s[CS], c->ip++));
}

static inline uint16_t fetch16(struct cpu *c)
{
    uint16_t lo = fetch8(c);
    return (uint16_t)(lo | fetch8(c) << 8);
}

void cpu_push(struct cpu *c, uint16_t v)
{
    c->r[SP] -= 2;
    cpu_ww(c, c->s[SS], c->r[SP], v);
}

uint16_t cpu_pop(struct cpu *c)
{
    uint16_t v = cpu_rw(c, c->s[SS], c->r[SP]);
    c->r[SP] += 2;
    return v;
}

void cpu_interrupt(struct cpu *c, uint8_t n)
{
    cpu_push(c, c->flags);
    cpu_push(c, c->s[CS]);
    cpu_push(c, c->ip);
    set_flag(c, F_IF | F_TF, false);
    c->ip = cpu_rw(c, 0, (uint16_t)(n * 4));
    c->s[CS] = cpu_rw(c, 0, (uint16_t)(n * 4 + 2));
}

bool cpu_irq(struct cpu *c, uint8_t n)
{
    if (!flag(c, F_IF) || c->irq_shadow)
        return false;
    cpu_interrupt(c, n);
    return true;
}

static void szp8(struct cpu *c, uint8_t r)
{
    c->flags = (uint16_t)((c->flags & ~(F_SF | F_ZF | F_PF)) | (r & 0x80 ? F_SF : 0) | (r ? 0 : F_ZF) |
                          parity_table[r]);
}

static void szp16(struct cpu *c, uint16_t r)
{
    c->flags = (uint16_t)((c->flags & ~(F_SF | F_ZF | F_PF)) | (r & 0x8000 ? F_SF : 0) | (r ? 0 : F_ZF) |
                          parity_table[r & 0xff]);
}

static void szp(struct cpu *c, uint32_t r, bool word)
{
    if (word)
        szp16(c, (uint16_t)r);
    else
        szp8(c, (uint8_t)r);
}

static uint32_t alu(struct cpu *c, int op, uint32_t a, uint32_t b, bool word)
{
    uint32_t mask = word ? 0xffff : 0xff, sign = word ? 0x8000 : 0x80, r;
    uint32_t carry = flag(c, F_CF) ? 1 : 0;
    switch (op) {
    case OP_ADD:
    case OP_ADC:
        if (op == OP_ADD)
            carry = 0;
        r = a + b + carry;
        set_flag(c, F_CF, r > mask);
        set_flag(c, F_AF, (a ^ b ^ r) & 0x10);
        set_flag(c, F_OF, (r ^ a) & (r ^ b) & sign);
        r &= mask;
        szp(c, r, word);
        return r;
    case OP_SUB:
    case OP_SBB:
    case OP_CMP:
        if (op != OP_SBB)
            carry = 0;
        r = (a - b - carry) & mask;
        set_flag(c, F_CF, a < b + carry);
        set_flag(c, F_AF, (a ^ b ^ r) & 0x10);
        set_flag(c, F_OF, (a ^ b) & (a ^ r) & sign);
        szp(c, r, word);
        return r;
    case OP_OR:
        r = a | b;
        break;
    case OP_AND:
        r = a & b;
        break;
    default:
        r = a ^ b;
        break;
    }
    set_flag(c, F_CF | F_OF | F_AF, false);
    szp(c, r, word);
    return r;
}

static uint32_t inc_dec(struct cpu *c, uint32_t v, bool dec, bool word)
{
    bool cf = flag(c, F_CF);
    uint32_t r = alu(c, dec ? OP_SUB : OP_ADD, v, 1, word);
    set_flag(c, F_CF, cf);
    return r;
}

static void decode_ea(struct cpu *c, struct decoder *d, struct ea *e)
{
    uint8_t modrm = fetch8(c);
    uint8_t mod = modrm >> 6;
    e->reg = (modrm >> 3) & 7;
    e->rm = modrm & 7;
    e->is_reg = mod == 3;
    if (e->is_reg)
        return;
    uint16_t off = 0;
    int seg = DS;
    switch (e->rm) {
    case 0: off = (uint16_t)(c->r[BX] + c->r[SI]); break;
    case 1: off = (uint16_t)(c->r[BX] + c->r[DI]); break;
    case 2: off = (uint16_t)(c->r[BP] + c->r[SI]); seg = SS; break;
    case 3: off = (uint16_t)(c->r[BP] + c->r[DI]); seg = SS; break;
    case 4: off = c->r[SI]; break;
    case 5: off = c->r[DI]; break;
    case 6:
        if (mod == 0)
            off = fetch16(c);
        else {
            off = c->r[BP];
            seg = SS;
        }
        break;
    case 7: off = c->r[BX]; break;
    }
    if (mod == 1)
        off = (uint16_t)(off + (int8_t)fetch8(c));
    else if (mod == 2)
        off = (uint16_t)(off + fetch16(c));
    e->off = off;
    e->seg = c->s[d->seg_override >= 0 ? d->seg_override : seg];
}

static uint32_t get_rm(struct cpu *c, struct ea *e, bool word)
{
    if (e->is_reg)
        return word ? c->r[e->rm] : cpu_reg8(c, e->rm);
    return word ? cpu_rw(c, e->seg, e->off) : cpu_rb(c, cpu_lin(e->seg, e->off));
}

static void set_rm(struct cpu *c, struct ea *e, bool word, uint32_t v)
{
    if (e->is_reg) {
        if (word)
            c->r[e->rm] = (uint16_t)v;
        else
            cpu_set_reg8(c, e->rm, (uint8_t)v);
    } else if (word) {
        cpu_ww(c, e->seg, e->off, (uint16_t)v);
    } else {
        cpu_wb(c, cpu_lin(e->seg, e->off), (uint8_t)v);
    }
}

static uint32_t get_reg(struct cpu *c, int r, bool word)
{
    return word ? c->r[r] : cpu_reg8(c, r);
}

static void set_reg(struct cpu *c, int r, bool word, uint32_t v)
{
    if (word)
        c->r[r] = (uint16_t)v;
    else
        cpu_set_reg8(c, r, (uint8_t)v);
}

static uint32_t shift(struct cpu *c, int op, uint32_t v, unsigned count, bool word)
{
    unsigned bits = word ? 16 : 8;
    uint32_t mask = word ? 0xffff : 0xff, msb = word ? 0x8000 : 0x80;
    count &= 0x1f;
    if (count == 0)
        return v;
    uint32_t r = v;
    bool cf = flag(c, F_CF);
    switch (op) {
    case 0:
        for (unsigned i = 0; i < count; i++) {
            cf = r & msb;
            r = ((r << 1) | (cf ? 1 : 0)) & mask;
        }
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((r & msb) != 0) != cf);
        return r;
    case 1:
        for (unsigned i = 0; i < count; i++) {
            cf = r & 1;
            r = (r >> 1) | (cf ? msb : 0);
        }
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((r ^ (r << 1)) & msb) != 0);
        return r;
    case 2:
        for (unsigned i = 0; i < count; i++) {
            bool out = r & msb;
            r = ((r << 1) | (cf ? 1 : 0)) & mask;
            cf = out;
        }
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((r & msb) != 0) != cf);
        return r;
    case 3:
        for (unsigned i = 0; i < count; i++) {
            bool out = r & 1;
            r = (r >> 1) | (cf ? msb : 0);
            cf = out;
        }
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((r ^ (r << 1)) & msb) != 0);
        return r;
    case 4:
    case 6:
        cf = count <= bits ? (v >> (bits - count)) & 1 : 0;
        r = count < 32 ? (v << count) & mask : 0;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, ((r & msb) != 0) != cf);
        break;
    case 5:
        cf = count <= bits ? (v >> (count - 1)) & 1 : 0;
        r = count < bits ? v >> count : 0;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, v & msb);
        break;
    case 7: {
        int32_t sv = word ? (int16_t)v : (int8_t)v;
        unsigned n = count >= bits ? bits - 1 : count;
        cf = (sv >> (n - 1)) & 1;
        if (count >= bits)
            cf = sv < 0;
        r = (uint32_t)(sv >> n) & mask;
        set_flag(c, F_CF, cf);
        set_flag(c, F_OF, false);
        break;
    }
    }
    szp(c, r, word);
    return r;
}

static bool condition(struct cpu *c, int cc)
{
    bool r;
    switch (cc >> 1) {
    case 0: r = flag(c, F_OF); break;
    case 1: r = flag(c, F_CF); break;
    case 2: r = flag(c, F_ZF); break;
    case 3: r = flag(c, F_CF) || flag(c, F_ZF); break;
    case 4: r = flag(c, F_SF); break;
    case 5: r = flag(c, F_PF); break;
    case 6: r = flag(c, F_SF) != flag(c, F_OF); break;
    default: r = flag(c, F_ZF) || (flag(c, F_SF) != flag(c, F_OF)); break;
    }
    return (cc & 1) ? !r : r;
}

static void divide_error(struct cpu *c)
{
    cpu_interrupt(c, 0);
}

static void group3(struct cpu *c, struct decoder *d, bool word)
{
    struct ea e;
    decode_ea(c, d, &e);
    uint32_t v = get_rm(c, &e, word);
    switch (e.reg) {
    case 0:
    case 1:
        alu(c, OP_AND, v, word ? fetch16(c) : fetch8(c), word);
        break;
    case 2:
        set_rm(c, &e, word, ~v);
        break;
    case 3: {
        uint32_t r = alu(c, OP_SUB, 0, v, word);
        set_flag(c, F_CF, v != 0);
        set_rm(c, &e, word, r);
        break;
    }
    case 4:
        if (word) {
            uint32_t r = (uint32_t)c->r[AX] * v;
            c->r[AX] = (uint16_t)r;
            c->r[DX] = (uint16_t)(r >> 16);
            set_flag(c, F_CF | F_OF, c->r[DX] != 0);
        } else {
            c->r[AX] = (uint16_t)(AL(c) * v);
            set_flag(c, F_CF | F_OF, AH(c) != 0);
        }
        szp(c, word ? c->r[AX] : AL(c), word);
        break;
    case 5:
        if (word) {
            int32_t r = (int32_t)(int16_t)c->r[AX] * (int16_t)v;
            c->r[AX] = (uint16_t)r;
            c->r[DX] = (uint16_t)((uint32_t)r >> 16);
            set_flag(c, F_CF | F_OF, r != (int16_t)r);
        } else {
            int16_t r = (int16_t)((int8_t)AL(c) * (int8_t)v);
            c->r[AX] = (uint16_t)r;
            set_flag(c, F_CF | F_OF, r != (int8_t)r);
        }
        break;
    case 6:
        if (v == 0) {
            divide_error(c);
            return;
        }
        if (word) {
            uint32_t n = (uint32_t)c->r[DX] << 16 | c->r[AX];
            uint32_t q = n / v;
            if (q > 0xffff) {
                divide_error(c);
                return;
            }
            c->r[DX] = (uint16_t)(n % v);
            c->r[AX] = (uint16_t)q;
        } else {
            uint16_t n = c->r[AX];
            uint16_t q = (uint16_t)(n / v);
            if (q > 0xff) {
                divide_error(c);
                return;
            }
            c->r[AX] = (uint16_t)((n % v) << 8 | q);
        }
        break;
    case 7:
        if (v == 0) {
            divide_error(c);
            return;
        }
        if (word) {
            int32_t n = (int32_t)((uint32_t)c->r[DX] << 16 | c->r[AX]);
            int32_t dv = (int16_t)v;
            if (n == INT32_MIN && dv == -1) {
                divide_error(c);
                return;
            }
            int32_t q = n / dv;
            if (q > 32767 || q < -32768) {
                divide_error(c);
                return;
            }
            c->r[AX] = (uint16_t)q;
            c->r[DX] = (uint16_t)(n % dv);
        } else {
            int16_t n = (int16_t)c->r[AX];
            int16_t dv = (int8_t)v;
            int16_t q = (int16_t)(n / dv);
            if (q > 127 || q < -128) {
                divide_error(c);
                return;
            }
            c->r[AX] = (uint16_t)((uint8_t)(n % dv) << 8 | (uint8_t)q);
        }
        break;
    }
}

static uint16_t string_seg(struct cpu *c, struct decoder *d)
{
    return c->s[d->seg_override >= 0 ? d->seg_override : DS];
}

static void string_op(struct cpu *c, struct decoder *d, uint8_t op)
{
    bool word = op & 1;
    int step = word ? 2 : 1;
    if (flag(c, F_DF))
        step = -step;
    bool rep = d->rep != 0;
    if (rep && c->r[CX] == 0)
        return;
    for (;;) {
        uint16_t src = string_seg(c, d);
        switch (op) {
        case 0xa4:
            cpu_wb(c, cpu_lin(c->s[ES], c->r[DI]), cpu_rb(c, cpu_lin(src, c->r[SI])));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        case 0xa5:
            cpu_ww(c, c->s[ES], c->r[DI], cpu_rw(c, src, c->r[SI]));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        case 0xa6:
        case 0xa7: {
            uint32_t a = word ? cpu_rw(c, src, c->r[SI]) : cpu_rb(c, cpu_lin(src, c->r[SI]));
            uint32_t b = word ? cpu_rw(c, c->s[ES], c->r[DI]) : cpu_rb(c, cpu_lin(c->s[ES], c->r[DI]));
            alu(c, OP_CMP, a, b, word);
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        }
        case 0xaa:
            cpu_wb(c, cpu_lin(c->s[ES], c->r[DI]), AL(c));
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        case 0xab:
            cpu_ww(c, c->s[ES], c->r[DI], c->r[AX]);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        case 0xac:
            cpu_set_reg8(c, 0, cpu_rb(c, cpu_lin(src, c->r[SI])));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            break;
        case 0xad:
            c->r[AX] = cpu_rw(c, src, c->r[SI]);
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            break;
        case 0xae:
        case 0xaf: {
            uint32_t b = word ? cpu_rw(c, c->s[ES], c->r[DI]) : cpu_rb(c, cpu_lin(c->s[ES], c->r[DI]));
            alu(c, OP_CMP, word ? c->r[AX] : AL(c), b, word);
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        }
        case 0x6c:
        case 0x6d: {
            uint8_t v = c->hooks->in(c, c->r[DX]);
            cpu_wb(c, cpu_lin(c->s[ES], c->r[DI]), v);
            if (word)
                cpu_wb(c, cpu_lin(c->s[ES], (uint16_t)(c->r[DI] + 1)), c->hooks->in(c, (uint16_t)(c->r[DX] + 1)));
            c->r[DI] = (uint16_t)(c->r[DI] + step);
            break;
        }
        case 0x6e:
        case 0x6f:
            c->hooks->out(c, c->r[DX], cpu_rb(c, cpu_lin(src, c->r[SI])));
            if (word)
                c->hooks->out(c, (uint16_t)(c->r[DX] + 1), cpu_rb(c, cpu_lin(src, (uint16_t)(c->r[SI] + 1))));
            c->r[SI] = (uint16_t)(c->r[SI] + step);
            break;
        }
        if (!rep)
            return;
        if (--c->r[CX] == 0)
            return;
        bool compare = op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf;
        if (compare && ((d->rep == 0xf3) != flag(c, F_ZF)))
            return;
    }
}

static void daa_das(struct cpu *c, bool sub)
{
    uint8_t al = AL(c), old = al;
    bool cf = flag(c, F_CF);
    if ((al & 0x0f) > 9 || flag(c, F_AF)) {
        al = sub ? (uint8_t)(al - 6) : (uint8_t)(al + 6);
        set_flag(c, F_AF, true);
    } else {
        set_flag(c, F_AF, false);
    }
    if (old > 0x99 || cf) {
        al = sub ? (uint8_t)(al - 0x60) : (uint8_t)(al + 0x60);
        set_flag(c, F_CF, true);
    } else {
        set_flag(c, F_CF, false);
    }
    cpu_set_reg8(c, 0, al);
    szp8(c, al);
}

static void aaa_aas(struct cpu *c, bool sub)
{
    if ((AL(c) & 0x0f) > 9 || flag(c, F_AF)) {
        if (sub) {
            c->r[AX] = (uint16_t)(c->r[AX] - 6);
            cpu_set_reg8(c, 4, (uint8_t)(AH(c) - 1));
        } else {
            c->r[AX] = (uint16_t)(c->r[AX] + 0x106);
        }
        set_flag(c, F_AF | F_CF, true);
    } else {
        set_flag(c, F_AF | F_CF, false);
    }
    cpu_set_reg8(c, 0, AL(c) & 0x0f);
}

static void step(struct cpu *c)
{
    struct decoder d = {.seg_override = -1, .rep = 0};
    struct ea e;
    uint32_t a, b;
    c->op_cs = c->s[CS];
    c->op_ip = c->ip;
    c->irq_shadow = false;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        if (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e)
            d.seg_override = (op >> 3) & 3;
        else if (op == 0xf2 || op == 0xf3)
            d.rep = op;
        else if (op != 0xf0)
            break;
    }
    bool word = op & 1;

    if (op < 0x40 && (op & 7) < 6) {
        int alu_op = op >> 3;
        switch (op & 7) {
        case 0:
        case 1:
            decode_ea(c, &d, &e);
            a = alu(c, alu_op, get_rm(c, &e, word), get_reg(c, e.reg, word), word);
            if (alu_op != OP_CMP)
                set_rm(c, &e, word, a);
            break;
        case 2:
        case 3:
            decode_ea(c, &d, &e);
            a = alu(c, alu_op, get_reg(c, e.reg, word), get_rm(c, &e, word), word);
            if (alu_op != OP_CMP)
                set_reg(c, e.reg, word, a);
            break;
        case 4:
            a = alu(c, alu_op, AL(c), fetch8(c), false);
            if (alu_op != OP_CMP)
                cpu_set_reg8(c, 0, (uint8_t)a);
            break;
        case 5:
            a = alu(c, alu_op, c->r[AX], fetch16(c), true);
            if (alu_op != OP_CMP)
                c->r[AX] = (uint16_t)a;
            break;
        }
        return;
    }

    switch (op) {
    case 0x06: case 0x0e: case 0x16: case 0x1e:
        cpu_push(c, c->s[op >> 3]);
        break;
    case 0x07: case 0x17: case 0x1f:
        c->s[op >> 3] = cpu_pop(c);
        if (op == 0x17)
            c->irq_shadow = true;
        break;
    case 0x27: daa_das(c, false); break;
    case 0x2f: daa_das(c, true); break;
    case 0x37: aaa_aas(c, false); break;
    case 0x3f: aaa_aas(c, true); break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->r[op & 7] = (uint16_t)inc_dec(c, c->r[op & 7], false, true);
        break;
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        c->r[op & 7] = (uint16_t)inc_dec(c, c->r[op & 7], true, true);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
        cpu_push(c, c->r[op & 7]);
        break;
    case 0x54:
        cpu_push(c, (uint16_t)(c->r[SP] - 2));
        break;
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        c->r[op & 7] = cpu_pop(c);
        break;
    case 0x60: {
        uint16_t sp = c->r[SP];
        for (int i = 0; i < 8; i++)
            cpu_push(c, i == SP ? sp : c->r[i]);
        break;
    }
    case 0x61:
        for (int i = 7; i >= 0; i--) {
            uint16_t v = cpu_pop(c);
            if (i != SP)
                c->r[i] = v;
        }
        break;
    case 0x62: {
        decode_ea(c, &d, &e);
        int16_t idx = (int16_t)c->r[e.reg];
        int16_t lo = (int16_t)cpu_rw(c, e.seg, e.off), hi = (int16_t)cpu_rw(c, e.seg, (uint16_t)(e.off + 2));
        if (idx < lo || idx > hi) {
            c->ip = c->op_ip;
            cpu_interrupt(c, 5);
        }
        break;
    }
    case 0x68: cpu_push(c, fetch16(c)); break;
    case 0x6a: cpu_push(c, (uint16_t)(int8_t)fetch8(c)); break;
    case 0x69:
    case 0x6b: {
        decode_ea(c, &d, &e);
        int32_t src = (int16_t)get_rm(c, &e, true);
        int32_t imm = op == 0x69 ? (int16_t)fetch16(c) : (int8_t)fetch8(c);
        int32_t r = src * imm;
        c->r[e.reg] = (uint16_t)r;
        set_flag(c, F_CF | F_OF, r != (int16_t)r);
        break;
    }
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:
    case 0xa4: case 0xa5: case 0xa6: case 0xa7:
    case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        string_op(c, &d, op);
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
        int8_t rel = (int8_t)fetch8(c);
        if (condition(c, op & 15))
            c->ip = (uint16_t)(c->ip + rel);
        break;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
        decode_ea(c, &d, &e);
        a = get_rm(c, &e, word);
        b = op == 0x81 ? fetch16(c) : op == 0x83 ? (uint16_t)(int8_t)fetch8(c) : fetch8(c);
        a = alu(c, e.reg, a, b, word);
        if (e.reg != OP_CMP)
            set_rm(c, &e, word, a);
        break;
    case 0x84: case 0x85:
        decode_ea(c, &d, &e);
        alu(c, OP_AND, get_rm(c, &e, word), get_reg(c, e.reg, word), word);
        break;
    case 0x86: case 0x87:
        decode_ea(c, &d, &e);
        a = get_rm(c, &e, word);
        set_rm(c, &e, word, get_reg(c, e.reg, word));
        set_reg(c, e.reg, word, a);
        break;
    case 0x88: case 0x89:
        decode_ea(c, &d, &e);
        set_rm(c, &e, word, get_reg(c, e.reg, word));
        break;
    case 0x8a: case 0x8b:
        decode_ea(c, &d, &e);
        set_reg(c, e.reg, word, get_rm(c, &e, word));
        break;
    case 0x8c:
        decode_ea(c, &d, &e);
        set_rm(c, &e, true, c->s[e.reg & 3]);
        break;
    case 0x8d:
        decode_ea(c, &d, &e);
        c->r[e.reg] = e.off;
        break;
    case 0x8e:
        decode_ea(c, &d, &e);
        c->s[e.reg & 3] = (uint16_t)get_rm(c, &e, true);
        if ((e.reg & 3) == SS)
            c->irq_shadow = true;
        break;
    case 0x8f:
        decode_ea(c, &d, &e);
        set_rm(c, &e, true, cpu_pop(c));
        break;
    case 0x90:
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = c->r[AX];
        c->r[AX] = c->r[op & 7];
        c->r[op & 7] = t;
        break;
    }
    case 0x98: c->r[AX] = (uint16_t)(int8_t)AL(c); break;
    case 0x99: c->r[DX] = (c->r[AX] & 0x8000) ? 0xffff : 0; break;
    case 0x9a: {
        uint16_t ip = fetch16(c), cs = fetch16(c);
        cpu_push(c, c->s[CS]);
        cpu_push(c, c->ip);
        c->s[CS] = cs;
        c->ip = ip;
        break;
    }
    case 0x9b: break;
    case 0x9c: cpu_push(c, c->flags | 0xf002); break;
    case 0x9d: c->flags = (uint16_t)((cpu_pop(c) & 0x0fd5) | 0xf002); break;
    case 0x9e: c->flags = (uint16_t)((c->flags & 0xff00) | (AH(c) & 0xd5) | 2); break;
    case 0x9f: cpu_set_reg8(c, 4, (uint8_t)c->flags); break;
    case 0xa0: {
        uint16_t off = fetch16(c);
        cpu_set_reg8(c, 0, cpu_rb(c, cpu_lin(string_seg(c, &d), off)));
        break;
    }
    case 0xa1: c->r[AX] = cpu_rw(c, string_seg(c, &d), fetch16(c)); break;
    case 0xa2: {
        uint16_t off = fetch16(c);
        cpu_wb(c, cpu_lin(string_seg(c, &d), off), AL(c));
        break;
    }
    case 0xa3: {
        uint16_t off = fetch16(c);
        cpu_ww(c, string_seg(c, &d), off, c->r[AX]);
        break;
    }
    case 0xa8: alu(c, OP_AND, AL(c), fetch8(c), false); break;
    case 0xa9: alu(c, OP_AND, c->r[AX], fetch16(c), true); break;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        cpu_set_reg8(c, op & 7, fetch8(c));
        break;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        c->r[op & 7] = fetch16(c);
        break;
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        decode_ea(c, &d, &e);
        unsigned count = op <= 0xc1 ? fetch8(c) : op <= 0xd1 ? 1 : CL(c);
        set_rm(c, &e, word, shift(c, e.reg, get_rm(c, &e, word), count, word));
        break;
    }
    case 0xc2: {
        uint16_t n = fetch16(c);
        c->ip = cpu_pop(c);
        c->r[SP] = (uint16_t)(c->r[SP] + n);
        break;
    }
    case 0xc3: c->ip = cpu_pop(c); break;
    case 0xc4: case 0xc5:
        decode_ea(c, &d, &e);
        c->r[e.reg] = cpu_rw(c, e.seg, e.off);
        c->s[op == 0xc4 ? ES : DS] = cpu_rw(c, e.seg, (uint16_t)(e.off + 2));
        break;
    case 0xc6: case 0xc7:
        decode_ea(c, &d, &e);
        set_rm(c, &e, word, word ? fetch16(c) : fetch8(c));
        break;
    case 0xc8: {
        uint16_t size = fetch16(c);
        uint8_t level = fetch8(c) & 31;
        cpu_push(c, c->r[BP]);
        uint16_t frame = c->r[SP];
        for (int i = 1; i < level; i++) {
            c->r[BP] = (uint16_t)(c->r[BP] - 2);
            cpu_push(c, cpu_rw(c, c->s[SS], c->r[BP]));
        }
        if (level)
            cpu_push(c, frame);
        c->r[BP] = frame;
        c->r[SP] = (uint16_t)(c->r[SP] - size);
        break;
    }
    case 0xc9:
        c->r[SP] = c->r[BP];
        c->r[BP] = cpu_pop(c);
        break;
    case 0xca: {
        uint16_t n = fetch16(c);
        c->ip = cpu_pop(c);
        c->s[CS] = cpu_pop(c);
        c->r[SP] = (uint16_t)(c->r[SP] + n);
        break;
    }
    case 0xcb:
        c->ip = cpu_pop(c);
        c->s[CS] = cpu_pop(c);
        break;
    case 0xcc: cpu_interrupt(c, 3); break;
    case 0xcd: cpu_interrupt(c, fetch8(c)); break;
    case 0xce:
        if (flag(c, F_OF))
            cpu_interrupt(c, 4);
        break;
    case 0xcf:
        c->ip = cpu_pop(c);
        c->s[CS] = cpu_pop(c);
        c->flags = (uint16_t)((cpu_pop(c) & 0x0fd5) | 0xf002);
        break;
    case 0xd4: {
        uint8_t base = fetch8(c);
        if (base == 0) {
            divide_error(c);
            break;
        }
        uint8_t al = AL(c);
        cpu_set_reg8(c, 4, al / base);
        cpu_set_reg8(c, 0, al % base);
        szp8(c, AL(c));
        break;
    }
    case 0xd5: {
        uint8_t base = fetch8(c);
        cpu_set_reg8(c, 0, (uint8_t)(AL(c) + AH(c) * base));
        cpu_set_reg8(c, 4, 0);
        szp8(c, AL(c));
        break;
    }
    case 0xd6: cpu_set_reg8(c, 0, flag(c, F_CF) ? 0xff : 0); break;
    case 0xd7:
        cpu_set_reg8(c, 0, cpu_rb(c, cpu_lin(string_seg(c, &d), (uint16_t)(c->r[BX] + AL(c)))));
        break;
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
        decode_ea(c, &d, &e);
        break;
    case 0xe0: case 0xe1: case 0xe2: {
        int8_t rel = (int8_t)fetch8(c);
        c->r[CX]--;
        bool jump = c->r[CX] != 0;
        if (op == 0xe0)
            jump = jump && !flag(c, F_ZF);
        else if (op == 0xe1)
            jump = jump && flag(c, F_ZF);
        if (jump)
            c->ip = (uint16_t)(c->ip + rel);
        break;
    }
    case 0xe3: {
        int8_t rel = (int8_t)fetch8(c);
        if (c->r[CX] == 0)
            c->ip = (uint16_t)(c->ip + rel);
        break;
    }
    case 0xe4: cpu_set_reg8(c, 0, c->hooks->in(c, fetch8(c))); break;
    case 0xe5: {
        uint8_t port = fetch8(c);
        c->r[AX] = (uint16_t)(c->hooks->in(c, port) | c->hooks->in(c, (uint16_t)(port + 1)) << 8);
        break;
    }
    case 0xe6: c->hooks->out(c, fetch8(c), AL(c)); break;
    case 0xe7: {
        uint8_t port = fetch8(c);
        c->hooks->out(c, port, AL(c));
        c->hooks->out(c, (uint16_t)(port + 1), AH(c));
        break;
    }
    case 0xe8: {
        uint16_t rel = fetch16(c);
        cpu_push(c, c->ip);
        c->ip = (uint16_t)(c->ip + rel);
        break;
    }
    case 0xe9: {
        uint16_t rel = fetch16(c);
        c->ip = (uint16_t)(c->ip + rel);
        break;
    }
    case 0xea: {
        uint16_t ip = fetch16(c), cs = fetch16(c);
        c->ip = ip;
        c->s[CS] = cs;
        break;
    }
    case 0xeb: {
        int8_t rel = (int8_t)fetch8(c);
        c->ip = (uint16_t)(c->ip + rel);
        break;
    }
    case 0xec: cpu_set_reg8(c, 0, c->hooks->in(c, c->r[DX])); break;
    case 0xed:
        c->r[AX] = (uint16_t)(c->hooks->in(c, c->r[DX]) | c->hooks->in(c, (uint16_t)(c->r[DX] + 1)) << 8);
        break;
    case 0xee: c->hooks->out(c, c->r[DX], AL(c)); break;
    case 0xef:
        c->hooks->out(c, c->r[DX], AL(c));
        c->hooks->out(c, (uint16_t)(c->r[DX] + 1), AH(c));
        break;
    case 0xf1: c->hooks->trap(c, fetch8(c)); break;
    case 0xf4: c->hooks->halt(c); break;
    case 0xf5: c->flags ^= F_CF; break;
    case 0xf6: case 0xf7: group3(c, &d, word); break;
    case 0xf8: set_flag(c, F_CF, false); break;
    case 0xf9: set_flag(c, F_CF, true); break;
    case 0xfa: set_flag(c, F_IF, false); break;
    case 0xfb: set_flag(c, F_IF, true); c->irq_shadow = true; break;
    case 0xfc: set_flag(c, F_DF, false); break;
    case 0xfd: set_flag(c, F_DF, true); break;
    case 0xfe:
        decode_ea(c, &d, &e);
        if (e.reg > 1) {
            c->hooks->invalid(c, op);
            break;
        }
        set_rm(c, &e, false, inc_dec(c, get_rm(c, &e, false), e.reg == 1, false));
        break;
    case 0xff:
        decode_ea(c, &d, &e);
        switch (e.reg) {
        case 0:
        case 1:
            set_rm(c, &e, true, inc_dec(c, get_rm(c, &e, true), e.reg == 1, true));
            break;
        case 2:
            a = get_rm(c, &e, true);
            cpu_push(c, c->ip);
            c->ip = (uint16_t)a;
            break;
        case 3: {
            uint16_t ip = cpu_rw(c, e.seg, e.off), cs = cpu_rw(c, e.seg, (uint16_t)(e.off + 2));
            cpu_push(c, c->s[CS]);
            cpu_push(c, c->ip);
            c->ip = ip;
            c->s[CS] = cs;
            break;
        }
        case 4:
            c->ip = (uint16_t)get_rm(c, &e, true);
            break;
        case 5:
            a = cpu_rw(c, e.seg, e.off);
            c->s[CS] = cpu_rw(c, e.seg, (uint16_t)(e.off + 2));
            c->ip = (uint16_t)a;
            break;
        case 6:
            cpu_push(c, (uint16_t)get_rm(c, &e, true));
            break;
        default:
            c->hooks->invalid(c, op);
            break;
        }
        break;
    default:
        c->hooks->invalid(c, op);
        break;
    }
}

void cpu_run(struct cpu *c, uint32_t budget)
{
    while (budget-- && !c->stop) {
        step(c);
        c->executed++;
    }
}
