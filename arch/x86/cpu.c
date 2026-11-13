#include <synth/arch_win32.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

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

struct idt64_entry {
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
    uint32_t base;
} __attribute__((packed));

extern const uint32_t isr_table[48];
extern const uint32_t isr64_table[48];
void gdt_flush(const struct table_ptr *ptr);
void long_switch_on(uint32_t pml4, const struct table_ptr *idt);
void long_switch_off(uint32_t root, const struct table_ptr *idt);

static struct gdt_entry gdt[10];
static struct idt64_entry idt64[256] __attribute__((aligned(16)));
static struct table_ptr idt32_ptr, idt64_ptr;
static bool long_active;
static arch_fault_fn fault_handler;
static void (*tick_hook)(uint32_t eip);
static bool sse;
static struct cpu_info info;
static volatile int interrupt_depth;

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

static void pic_remap(void)
{
    pic_program(0x20, 0x28);
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

/* Programs run at CPL 0 on their own stacks: when a program moves its stack into committed memory that has no page
   yet (Go's goroutine stacks), the page fault cannot push its frame and becomes a double fault. A task gate gives
   that one a stack of its own: the missing page is mapped and the task switch back retries the instruction. */
struct tss32 {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} __attribute__((packed));

static struct tss32 main_tss, df_tss;
void df_task(void);

void cpu_tss_cr3(uint32_t root)
{
    main_tss.cr3 = df_tss.cr3 = root;
}

static bool stack_fixup(uint32_t esp, uint32_t cr2)
{
    uint32_t at[3] = {cr2, esp - 1, (esp & ~15u) - 64};
    bool fixed = false;
    for (int i = 0; i < 3; i++) {
        if (at[i] < VMM_USER_START || vmm_mapped(at[i]))
            continue;
        if (!vmm_commit_fault(at[i]))
            return false;
        fixed = true;
    }
    return fixed;
}

static uint32_t read_cr0(void)
{
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

/* the task switch back sets CR0.TS: the next FPU instruction lands in x86_interrupt, which clears it */
void x86_double_fault(void)
{
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (!stack_fixup(main_tss.esp, cr2))
        panic("CPU exception 8 (double fault) at %08x cr2=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x",
              main_tss.eip, cr2, main_tss.eax, main_tss.ebx, main_tss.ecx, main_tss.edx, main_tss.esp);
}

static void tss_init(void)
{
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    main_tss.iomap = df_tss.iomap = sizeof(struct tss32);
    df_tss.cr3 = main_tss.cr3 = cr3;
    df_tss.eip = (uint32_t)df_task;
    df_tss.eflags = 0x2;
    df_tss.cs = 0x08;
    df_tss.ss = df_tss.ds = df_tss.es = 0x10;
    gdt_set(8, (uint32_t)&main_tss, sizeof(struct tss32) - 1, 0x89, 0x00);
    gdt_set(9, (uint32_t)&df_tss, sizeof(struct tss32) - 1, 0x89, 0x00);
}

/* once there is a heap (hal_start_cpus): the double-fault task gets its stack and the gate */
static struct gdt_entry (*ap_gdt)[10];

/* before the other processors start: the double-fault task's stack and their GDTs, on the heap */
void cpu_df_init(void)
{
    ap_gdt = kzalloc(sizeof(*ap_gdt) * MAX_CPUS);
    uint8_t *stack = kmalloc(8192);
    if (!stack)
        return;
    df_tss.esp = (uint32_t)(stack + 8192) & ~15u;
    idt[8] = (struct idt_entry){0, 0x48, 0, 0x85, 0};
}

void cpu_init(void)
{
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xfffff, 0x9a, 0xcf);
    gdt_set(2, 0, 0xfffff, 0x92, 0xcf);
    gdt_set(3, 0, 0xfff, 0x92, 0x40);
    gdt_set(4, 0, 0xffff, 0x9a, 0x00);
    gdt_set(5, 0, 0xffff, 0x92, 0x00);
    gdt_set(6, 0, 0xfffff, 0x9a, 0xa0);
    gdt_set(7, 0, 0x1fff, 0x92, 0x40);
    tss_init();
    struct table_ptr gp = {sizeof(gdt) - 1, (uint32_t)gdt};
    gdt_flush(&gp);
    __asm__ volatile("ltr %w0" : : "r"(0x40));

    pic_remap();
    for (int i = 0; i < 48; i++) {
        uint32_t addr = isr_table[i];
        idt[i] = (struct idt_entry){(uint16_t)(addr & 0xffff), 0x08, 0, 0x8e, (uint16_t)(addr >> 16)};
    }
    idt32_ptr = (struct table_ptr){sizeof(idt) - 1, (uint32_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idt32_ptr));
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
    info.pae = edx & (1u << 6);
    info.pat = edx & (1u << 16);
    info.apic = edx & (1u << 9);
    if (ecx & (1u << 31)) {
        eax = 0x40000000u;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
        info.emulated = ebx == 0x54474354u && ecx == 0x43544743u && edx == 0x47435447u;
        memcpy_unrolled = info.emulated;
    }
    eax = 0x80000000u;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (eax >= 0x80000001u) {
        eax = 0x80000001u;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
        info.long_mode = info.pae && (edx & (1u << 29));
    }
}

/* PAT entry 1 (PWT set in a page entry) becomes write-combining. The firmware's MTRRs make video memory uncached,
   and in a virtual machine every uncached pixel store is slow; write-combined stores go out in bursts. */
static void pat_init(void)
{
    if (!info.pat)
        return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
    lo = (lo & ~0xff00u) | 0x0100u;
    __asm__ volatile("wbinvd; wrmsr; wbinvd" : : "a"(lo), "d"(hi), "c"(0x277u) : "memory");
}

static void cpu_local_init(void);

static void fpu_init(void)
{
    detect_cpu();
    cpu_local_init();
}

/* The control registers every processor sets for itself: x87 (or its emulation trap), SSE, write-combining. */
static void cpu_local_init(void)
{
    pat_init();
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


/* An application processor's own GDT (it will hold that processor's TEB segments), the shared IDT, its control
   registers. */
void cpu_set_gate(int vector, uintptr_t handler)
{
    uint32_t addr = (uint32_t)handler;
    idt[vector] = (struct idt_entry){(uint16_t)(addr & 0xffff), 0x08, 0, 0x8e, (uint16_t)(addr >> 16)};
}

void cpu_ap_init(int index)
{
    memcpy(ap_gdt[index], gdt, sizeof(gdt));
    struct table_ptr gp = {sizeof(gdt) - 1, (uint32_t)ap_gdt[index]};
    gdt_flush(&gp);
    __asm__ volatile("lidt %0" : : "m"(idt32_ptr));
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

void arch_w32_set_teb(uint32_t base)
{
    gdt_set(3, base, 0xfff, 0x92, 0x40);
}

void arch_w64_set_teb(uint32_t base)
{
    gdt_set(7, base, 0x1fff, 0x92, 0x40);
}

bool arch_long_supported(void)
{
    return info.long_mode && info.sse && vmm_pae();
}

bool arch_long_enter(void)
{
    if (long_active)
        return true;
    if (!arch_long_supported())
        return false;
    uint32_t root = vmm_long_mode_root();
    if (!root)
        return false;
    if (!idt64_ptr.base) {
        for (int i = 0; i < 48; i++) {
            uint32_t addr = isr64_table[i];
            idt64[i] = (struct idt64_entry){(uint16_t)addr, 0x30, 0, 0x8e, (uint16_t)(addr >> 16), 0, 0};
        }
        idt64_ptr = (struct table_ptr){sizeof(idt64) - 1, (uint32_t)idt64};
    }
    long_switch_on(root, &idt64_ptr);
    long_active = true;
    return true;
}

void arch_long_leave(void)
{
    if (!long_active)
        return;
    long_switch_off(vmm_pae_root(), &idt32_ptr);
    long_active = false;
}

bool arch_long_active(void)
{
    return long_active;
}

static struct regs *fault_regs;

void arch_fault_registers(uint32_t out[8])
{
    const struct regs *r = fault_regs;
    uint32_t v[8] = {r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp, r->esp + 20};
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

extern uint32_t long_cr2_high, long_rip_high;

uint32_t arch_fault_rip_high(void)
{
    return arch_long_active() ? long_rip_high : 0;
}

uint32_t arch_fault_address_high(void)
{
    return arch_long_active() ? long_cr2_high : 0;
}

void x86_interrupt(struct regs *r)
{
    if (r->vector < 32) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        uint32_t high = arch_fault_address_high();
        if (r->vector == 14 && high && !(r->error & 1) && vmm_high_fault((uint64_t)high << 32 | cr2))
            return;
        if (r->vector == 14 && !high && !(r->error & 1) && vmm_commit_fault(cr2))
            return;
        if (r->vector == 14 && !high && (r->error & 3) == 3 && vmm_cow_fault(cr2))
            return;
        if (r->vector == 7 && read_cr0() & 8) {
            __asm__ volatile("clts");
            return;
        }
        if (fault_handler && r->vector != 8 && r->vector != 18) {
            fault_regs = r;
            fault_handler((int)r->vector, r->eip, cr2);
        }
        if (r->cs == 0x08)
            arch_guard_catch((int)r->vector, r->eip, cr2);
        panic("CPU exception %u (%s) at %08x err=%x cr2=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x",
              r->vector, exception_names[r->vector] ? exception_names[r->vector] : "reserved", r->eip,
              r->error, cr2, r->eax, r->ebx, r->ecx, r->edx, r->esp);
    }
    int irq = (int)r->vector - 32;
    if (irq >= 0 && irq < 16) {
        interrupt_depth++;
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        if (irq >= 8)
            outb(0xa0, 0x20);
        outb(0x20, 0x20);
        if (irq == 0 && r->cs == 0x08 && arch_guard_tick((uintptr_t)r->eip))
            r->eip = (uintptr_t)arch_guard_escape;
        if (irq == 0 && tick_hook)
            tick_hook(r->eip);
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

/* the 32-bit kernel keeps to SSE2 (FXSAVE) */
uint32_t hal_simd(void)
{
    return sse ? HAL_SIMD_SSE2 : 0;
}

uint32_t arch_fpu_area_size(void)
{
    return 512;
}

void arch_fpu_save(void *area)
{
    if (!info.fpu)
        return;
    if (sse)
        __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
    else
        __asm__ volatile("fnsave (%0); frstor (%0)" : : "r"(area) : "memory");
}

void arch_fpu_restore(const void *area)
{
    if (!info.fpu)
        return;
    if (sse)
        __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
    else
        __asm__ volatile("frstor (%0)" : : "r"(area) : "memory");
}

/* the descriptor a selector names in this processor's GDT (0 when it names none) */
uint64_t arch_selector_entry(uint32_t sel)
{
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } g = {0, 0};
    __asm__ volatile("sgdt %0" : "=m"(g));
    if ((sel & 4) || (sel | 7) > g.limit || !(sel & ~7u))
        return 0;
    uint64_t d;
    memcpy(&d, (const uint8_t *)(uintptr_t)g.base + (sel & ~7u), 8);
    return d;
}

void arch_load_teb_selector(void)
{
    __asm__ volatile("mov %0, %%fs" : : "r"(0x18) : "memory");
}
