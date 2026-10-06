#include <synth/arch_win32.h>
#include <synth/log.h>
#include <synth/string.h>

#include "io.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t offset_high;
} __attribute__((packed));

struct table_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

extern const uint32_t isr_table[48];
void gdt_flush(const struct table_ptr *ptr);

static struct gdt_entry gdt[4];
static arch_fault_fn fault_handler;
static void (*tick_hook)(uint32_t eip);
static bool sse;
static struct cpu_info info;

static void fpu_init(void);
static struct idt_entry idt[256];
static irq_handler irq_handlers[16];

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode",
    "device not available", "double fault", "coprocessor overrun", "invalid TSS",
    "segment not present", "stack fault", "general protection fault", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check", "SIMD floating point",
    "virtualization", "control protection",
};

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i] = (struct gdt_entry){
        .limit_low = (uint16_t)(limit & 0xffff),
        .base_low = (uint16_t)(base & 0xffff),
        .base_mid = (uint8_t)(base >> 16),
        .access = access,
        .granularity = (uint8_t)(((limit >> 16) & 0x0f) | (gran & 0xf0)),
        .base_high = (uint8_t)(base >> 24),
    };
}

static void pic_remap(void)
{
    uint8_t m1 = inb(0x21), m2 = inb(0xa1);
    outb(0x20, 0x11);
    io_wait();
    outb(0xa0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xa1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xa1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xa1, 0x01);
    io_wait();
    UNUSED(m1);
    UNUSED(m2);
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
}

static void irq_unmask(int irq)
{
    uint16_t port = irq < 8 ? 0x21 : 0xa1;
    outb(port, (uint8_t)(inb(port) & ~(1u << (irq & 7))));
    if (irq >= 8)
        outb(0x21, (uint8_t)(inb(0x21) & ~(1u << 2)));
}

void irq_install(int irq, irq_handler handler)
{
    irq_handlers[irq] = handler;
    irq_unmask(irq);
}

void cpu_init(void)
{
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xfffff, 0x9a, 0xcf);
    gdt_set(2, 0, 0xfffff, 0x92, 0xcf);
    gdt_set(3, 0, 0xfff, 0x92, 0x40);
    struct table_ptr gp = {sizeof(gdt) - 1, (uint32_t)gdt};
    gdt_flush(&gp);

    pic_remap();
    for (int i = 0; i < 48; i++) {
        uint32_t addr = isr_table[i];
        idt[i] = (struct idt_entry){(uint16_t)(addr & 0xffff), 0x08, 0, 0x8e, (uint16_t)(addr >> 16)};
    }
    struct table_ptr ip = {sizeof(idt) - 1, (uint32_t)idt};
    __asm__ volatile("lidt %0" : : "m"(ip));
    fpu_init();
}

static bool has_cpuid(void)
{
    uint32_t a, b;
    __asm__ volatile("pushfl; pushfl; xorl $0x200000, (%%esp); popfl; pushfl; popl %0; popl %1; pushl %1; popfl"
                     : "=r"(a), "=r"(b));
    return (a ^ b) & 0x200000;
}

static void detect_cpu(void)
{
    info.family = 4;
    if (!has_cpuid())
        return;
    info.cpuid = true;
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    info.family = (eax >> 8) & 0xf;
    if (info.family == 0xf)
        info.family += (eax >> 20) & 0xff;
    info.tsc = edx & (1u << 4);
    info.sse = (edx & (1u << 25)) && (edx & (1u << 24));
}

static void fpu_init(void)
{
    detect_cpu();
    uint32_t cr0;
    uint16_t status = 0xffff;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1u << 2) | (1u << 3));
    cr0 |= (1u << 1) | (1u << 5);
    __asm__ volatile("mov %0, %%cr0; fninit; fnstsw %1" : : "r"(cr0), "m"(status) : "memory");
    info.fpu = status == 0;
    if (!info.fpu) {
        cr0 = (cr0 | (1u << 2)) & ~(1u << 1);
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
        return;
    }
    if (info.sse) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9) | (1u << 10);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
        sse = true;
    }
}

const struct cpu_info *cpu_info(void)
{
    return &info;
}

bool arch_has_sse(void)
{
    return sse;
}

void arch_w32_set_teb(uint32_t base)
{
    gdt_set(3, base, 0xfff, 0x92, 0x40);
}

void arch_set_fault_handler(arch_fault_fn fn)
{
    fault_handler = fn;
}

void arch_set_tick_hook(void (*fn)(uint32_t eip))
{
    tick_hook = fn;
}

void x86_interrupt(struct regs *r)
{
    if (r->vector < 32) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (fault_handler && r->vector != 8 && r->vector != 18)
            fault_handler((int)r->vector, r->eip, cr2);
        panic("CPU exception %u (%s) at %08x err=%x cr2=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x",
              r->vector, exception_names[r->vector] ? exception_names[r->vector] : "reserved", r->eip,
              r->error, cr2, r->eax, r->ebx, r->ecx, r->edx, r->esp);
    }
    int irq = (int)r->vector - 32;
    if (irq >= 0 && irq < 16) {
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        if (irq >= 8)
            outb(0xa0, 0x20);
        outb(0x20, 0x20);
        if (irq == 0 && tick_hook)
            tick_hook(r->eip);
    }
}
