#include <synth/arch_win32.h>
#include <synth/console.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "../io.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct table_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern const uint64_t isr_table[48];
void gdt_load(const struct table_ptr *ptr);

/* the same selectors as the 32-bit kernel: 0x08/0x10 flat 32-bit, 0x20/0x28 16-bit (BIOS calls), 0x30 64-bit code */
static uint64_t gdt[8] = {
    0, 0x00cf9a000000ffffull, 0x00cf92000000ffffull, 0x0040920000000fffull, 0x00009a000000ffffull,
    0x000092000000ffffull, 0x00209a0000000000ull, 0x0040920000001fffull,
};

static uint64_t data_descriptor(uint32_t base, uint32_t limit)
{
    return (uint64_t)(limit & 0xffff) | (uint64_t)(base & 0xffffff) << 16 | 0x92ull << 40 |
           (uint64_t)((limit >> 16) & 0xf) << 48 | 0x40ull << 48 | (uint64_t)(base >> 24) << 56;
}

/* FS of a 32-bit program: its TEB (entry 3), and the 64-bit TEB's low alias (entry 7) */
void arch_w32_set_teb(uint32_t base)
{
    gdt[3] = data_descriptor(base, 0xfff);
}

void arch_w64_set_teb(uint32_t base)
{
    gdt[7] = data_descriptor(base, 0x1fff);
}

void arch_load_teb_selector(void)
{
    __asm__ volatile("mov %0, %%fs" : : "r"(0x18) : "memory");
}
static struct idt_entry idt[256] __attribute__((aligned(16)));
static struct table_ptr idt_ptr;
static arch_fault_fn fault_handler;
static void (*tick_hook)(uint32_t eip);
static bool sse;
static struct cpu_info info;
static volatile int interrupt_depth;
static irq_handler irq_handlers[16];

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode",
    "device not available", "double fault", "coprocessor overrun", "invalid TSS",
    "segment not present", "stack fault", "general protection fault", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check", "SIMD floating point",
    "virtualization", "control protection",
};

void pic_program(uint8_t master, uint8_t slave)
{
    outb(0x20, 0x11);
    io_wait();
    outb(0xa0, 0x11);
    io_wait();
    outb(0x21, master);
    io_wait();
    outb(0xa1, slave);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xa1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xa1, 0x01);
    io_wait();
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

static void cpuid(uint32_t leaf, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

static void detect_cpu(void)
{
    uint32_t r[4];
    info.cpuid = true;
    cpuid(1, r);
    info.family = (r[0] >> 8) & 0xf;
    if (info.family == 0xf)
        info.family += (r[0] >> 20) & 0xff;
    info.tsc = r[3] & (1u << 4);
    info.sse = true;
    info.pae = true;
    info.pat = r[3] & (1u << 16);
    info.apic = r[3] & (1u << 9);
    info.long_mode = true;
    if (r[2] & (1u << 31)) {
        cpuid(0x40000000u, r);
        info.emulated = r[1] == 0x54474354u && r[2] == 0x43544743u && r[3] == 0x47435447u;
        memcpy_unrolled = info.emulated;
    }
}

/* PAT entry 1 (PWT set in a page entry) becomes write-combining, for the framebuffer */
static void pat_init(void)
{
    if (!info.pat)
        return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
    lo = (lo & ~0xff00u) | 0x0100u;
    __asm__ volatile("wbinvd; wrmsr; wbinvd" : : "a"(lo), "d"(hi), "c"(0x277u) : "memory");
}

static void cpu_local_init(void)
{
    pat_init();
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ull << 2) | (1ull << 3));
    cr0 |= (1ull << 1) | (1ull << 5);
    __asm__ volatile("mov %0, %%cr0; fninit" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    info.fpu = true;
    sse = true;
}

void cpu_init(void)
{
    struct table_ptr gp = {sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt};
    gdt_load(&gp);
    pic_program(0x20, 0x28);
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
    for (int i = 0; i < 48; i++) {
        uint64_t a = isr_table[i];
        idt[i] = (struct idt_entry){(uint16_t)a, 0x30, 0, 0x8e, (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0};
    }
    idt_ptr = (struct table_ptr){sizeof(idt) - 1, (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    detect_cpu();
    cpu_local_init();
}

void cpu_ap_init(int index)
{
    UNUSED(index);
    struct table_ptr gp = {sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt};
    gdt_load(&gp);
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    cpu_local_init();
}

const struct cpu_info *cpu_info(void)
{
    return &info;
}

bool arch_has_sse(void)
{
    return sse;
}

static struct regs *fault_regs;

void arch_fault_registers(uint32_t out[8])
{
    const struct regs *r = fault_regs;
    uint32_t v[8] = {(uint32_t)r->rax, (uint32_t)r->rbx, (uint32_t)r->rcx, (uint32_t)r->rdx,
                     (uint32_t)r->rsi, (uint32_t)r->rdi, (uint32_t)r->rbp, (uint32_t)r->rsp};
    memcpy(out, v, sizeof(v));
}

void arch_set_fault_handler(arch_fault_fn fn)
{
    fault_handler = fn;
}

void arch_set_tick_hook(void (*fn)(uint32_t eip))
{
    tick_hook = fn;
}

uint32_t arch_fault_rip_high(void)
{
    return fault_regs ? (uint32_t)(fault_regs->rip >> 32) : 0;
}

uint32_t arch_fault_address_high(void)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    return (uint32_t)(cr2 >> 32);
}

/* where a fault in the kernel came from: the frame pointer chain */
static void backtrace(const struct regs *r, bool console)
{
    uint64_t fp = r->rbp;
    if (!console)
        klog("fault %u at %016llx rax=%llx rdi=%llx rsi=%llx rsp=%llx", (unsigned)r->vector, (unsigned long long)r->rip,
             (unsigned long long)r->rax, (unsigned long long)r->rdi, (unsigned long long)r->rsi,
             (unsigned long long)r->rsp);
    for (int i = 0; i < 12 && fp >= 0x400000 && !(fp & 3) && vmm_mapped((uintptr_t)fp) && vmm_mapped((uintptr_t)fp + 8);
         i++, fp = *(uint64_t *)(uintptr_t)fp) {
        unsigned long long at = ((uint64_t *)(uintptr_t)fp)[1];
        if (console)
            kprintf("  called from %016llx\n", at);
        else
            klog("  called from %016llx", at);
    }
}

void x86_interrupt(struct regs *r)
{
    if (r->vector < 32) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (r->vector == 14 && !(r->error & 1) && cr2 >> 32 && vmm_high_fault(cr2))
            return;
        if (r->vector == 14 && !(r->error & 1) && !(cr2 >> 32) && vmm_commit_fault((uintptr_t)cr2))
            return;
        if (r->vector == 14 && (r->error & 3) == 3 && !(cr2 >> 32) && vmm_cow_fault((uintptr_t)cr2))
            return;
        if (fault_handler && r->vector != 8 && r->vector != 18) {
            fault_regs = r;
            if (r->cs == 0x30)
                backtrace(r, false);
            fault_handler((int)r->vector, (uint32_t)r->rip, (uint32_t)cr2);
        }
        backtrace(r, true);
        panic("CPU exception %u (%s) at %016llx err=%x cr2=%016llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsp=%llx",
              (unsigned)r->vector, exception_names[r->vector] ? exception_names[r->vector] : "reserved",
              (unsigned long long)r->rip, (unsigned)r->error, (unsigned long long)cr2, (unsigned long long)r->rax,
              (unsigned long long)r->rbx, (unsigned long long)r->rcx, (unsigned long long)r->rdx,
              (unsigned long long)r->rsp);
    }
    int irq = (int)r->vector - 32;
    if (irq >= 0 && irq < 16) {
        interrupt_depth++;
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        if (irq >= 8)
            outb(0xa0, 0x20);
        outb(0x20, 0x20);
        if (irq == 0 && tick_hook)
            tick_hook((uint32_t)r->rip);
        interrupt_depth--;
    }
}

uint32_t hal_irq_save(void)
{
    return cpu_irq_save();
}

void hal_irq_restore(uint32_t flags)
{
    cpu_irq_restore(flags);
}

bool hal_in_interrupt(void)
{
    return interrupt_depth > 0;
}

int arch_irq_depth(void)
{
    return interrupt_depth;
}

void arch_set_irq_depth(int depth)
{
    interrupt_depth = depth;
}

void arch_fpu_save(void *area)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void arch_fpu_restore(const void *area)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}
