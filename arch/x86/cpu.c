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

static struct gdt_entry gdt[3];
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
    struct table_ptr gp = {sizeof(gdt) - 1, (uint32_t)gdt};
    gdt_flush(&gp);

    pic_remap();
    for (int i = 0; i < 48; i++) {
        uint32_t addr = isr_table[i];
        idt[i] = (struct idt_entry){(uint16_t)(addr & 0xffff), 0x08, 0, 0x8e, (uint16_t)(addr >> 16)};
    }
    struct table_ptr ip = {sizeof(idt) - 1, (uint32_t)idt};
    __asm__ volatile("lidt %0" : : "m"(ip));
}

void x86_interrupt(struct regs *r)
{
    if (r->vector < 32) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
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
    }
}
