#ifndef SYNTH_DOS_CPU_H
#define SYNTH_DOS_CPU_H

#include <synth/types.h>

#define MEM_SIZE 0x100000u
#define MEM_MASK 0xfffffu

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };

enum {
    F_CF = 0x0001,
    F_PF = 0x0004,
    F_AF = 0x0010,
    F_ZF = 0x0040,
    F_SF = 0x0080,
    F_TF = 0x0100,
    F_IF = 0x0200,
    F_DF = 0x0400,
    F_OF = 0x0800,
};

struct cpu;

struct cpu_hooks {
    void (*trap)(struct cpu *c, uint8_t n);
    uint8_t (*in)(struct cpu *c, uint16_t port);
    void (*out)(struct cpu *c, uint16_t port, uint8_t value);
    void (*halt)(struct cpu *c);
    void (*invalid)(struct cpu *c, uint8_t opcode);
};

struct cpu {
    uint16_t r[8];
    uint16_t s[4];
    uint16_t ip;
    uint16_t flags;
    uint8_t *mem;
    const struct cpu_hooks *hooks;
    void *user;
    bool stop;
    bool video_touched;
    bool irq_shadow;
    uint64_t executed;
    uint16_t op_cs;
    uint16_t op_ip;
};

void cpu_reset(struct cpu *c);
void cpu_run(struct cpu *c, uint32_t budget);
void cpu_interrupt(struct cpu *c, uint8_t n);
bool cpu_irq(struct cpu *c, uint8_t n);

static inline uint32_t cpu_lin(uint16_t seg, uint16_t off)
{
    return (((uint32_t)seg << 4) + off) & MEM_MASK;
}

static inline uint8_t cpu_rb(struct cpu *c, uint32_t addr)
{
    return c->mem[addr & MEM_MASK];
}

static inline void cpu_wb(struct cpu *c, uint32_t addr, uint8_t v)
{
    addr &= MEM_MASK;
    if (addr >= 0xf0000)
        return;
    if (addr >= 0xb8000 && addr < 0xc0000)
        c->video_touched = true;
    c->mem[addr] = v;
}

static inline uint16_t cpu_rw(struct cpu *c, uint16_t seg, uint16_t off)
{
    return (uint16_t)(cpu_rb(c, cpu_lin(seg, off)) | cpu_rb(c, cpu_lin(seg, (uint16_t)(off + 1))) << 8);
}

static inline void cpu_ww(struct cpu *c, uint16_t seg, uint16_t off, uint16_t v)
{
    cpu_wb(c, cpu_lin(seg, off), (uint8_t)v);
    cpu_wb(c, cpu_lin(seg, (uint16_t)(off + 1)), (uint8_t)(v >> 8));
}

static inline uint8_t cpu_reg8(struct cpu *c, int i)
{
    return i < 4 ? (uint8_t)c->r[i] : (uint8_t)(c->r[i - 4] >> 8);
}

static inline void cpu_set_reg8(struct cpu *c, int i, uint8_t v)
{
    if (i < 4)
        c->r[i] = (uint16_t)((c->r[i] & 0xff00) | v);
    else
        c->r[i - 4] = (uint16_t)((c->r[i - 4] & 0x00ff) | (uint16_t)v << 8);
}

#define AL(c) cpu_reg8(c, 0)
#define CL(c) cpu_reg8(c, 1)
#define DL(c) cpu_reg8(c, 2)
#define BL(c) cpu_reg8(c, 3)
#define AH(c) cpu_reg8(c, 4)
#define CH(c) cpu_reg8(c, 5)
#define DH(c) cpu_reg8(c, 6)
#define BH(c) cpu_reg8(c, 7)

void cpu_push(struct cpu *c, uint16_t v);
uint16_t cpu_pop(struct cpu *c);

#endif
