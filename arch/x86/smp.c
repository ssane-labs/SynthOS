#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

/* Application processors: the boot processor wakes them with INIT and two startup IPIs into smpboot.asm, copied to
   a free page below 1 MB; each comes up in the kernel's paging with a stack of its own and waits in ap_entry. */

#define APIC_BASE_MSR 0x1bu
#define APIC_ENABLE (1u << 11)
#define APIC_X2 (1u << 10)
#define REG_ID 0x20u
#define REG_EOI 0xb0u
#define REG_SVR 0xf0u
#define REG_ICR_LO 0x300u
#define REG_ICR_HI 0x310u
#define ICR_INIT 0x00000500u
#define ICR_STARTUP 0x00000600u
#define ICR_ASSERT 0x00004000u
#define ICR_PENDING 0x00001000u
#define ICR_ALL_BUT_SELF 0x000c0000u
#define AP_STACK (16u * 1024)

extern const uint8_t smp_boot_start[], smp_boot_end[];

static volatile uint32_t *lapic;
static bool x2apic;
static volatile int online = 1;

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(msr));
}

static uint32_t apic_read(uint32_t reg)
{
    return x2apic ? (uint32_t)rdmsr(0x800u + reg / 16) : lapic[reg / 4];
}

static void apic_write(uint32_t reg, uint32_t v)
{
    if (x2apic)
        wrmsr(0x800u + reg / 16, v);
    else
        lapic[reg / 4] = v;
}

static void send_ipi(uint32_t icr)
{
    if (x2apic) {
        wrmsr(0x830u, icr);
        return;
    }
    apic_write(REG_ICR_HI, 0);
    apic_write(REG_ICR_LO, icr);
    for (int i = 0; i < 100000 && (apic_read(REG_ICR_LO) & ICR_PENDING); i++)
        __asm__ volatile("pause");
}

static void apic_local_enable(void)
{
    apic_write(REG_SVR, 0x100u | 0xffu);
}

/* Work pool: the other processors take parts of pure jobs (pixel loops on memory every address space shares) that
   the boot processor hands out with hal_parallel. They sleep in hlt and a fixed IPI wakes them; the job is published
   under a generation number, and `work` packs that generation (top 24 bits) with the next part (low 8 bits), so a
   processor that wakes late can never take a part of a newer job by mistake (parts plus two misses per processor
   stay below 256). */

#define WAKE_VECTOR 0xf0
#define SPURIOUS_VECTOR 0xff
#define JOB_NONE 0xffffffffu
#define MAX_PARTS 96

struct job {
    void (*fn)(void *ctx, int from, int to);
    void *ctx;
    int count, parts;
};

static struct job job;
static volatile uint32_t job_gen = JOB_NONE, work, done, generation;
static volatile int helpers, pool_busy;
volatile uint32_t *smp_eoi __attribute__((used));
uint32_t smp_jobs, smp_parts_helped;

#ifdef __x86_64__
__asm__(".text\n"
        "smp_wake_stub:\n"
        "  push %rax\n  push %rcx\n  push %rdx\n"
        "  movq smp_eoi(%rip), %rax\n"
        "  test %rax, %rax\n  jz 1f\n"
        "  movl $0, (%rax)\n  jmp 2f\n"
        "1: movl $0x80b, %ecx\n  xorl %eax, %eax\n  xorl %edx, %edx\n  wrmsr\n"
        "2: pop %rdx\n  pop %rcx\n  pop %rax\n  iretq\n"
        "smp_spurious_stub:\n  iretq\n");
#else
__asm__(".text\n"
        "smp_wake_stub:\n"
        "  push %eax\n  push %ecx\n  push %edx\n"
        "  movl smp_eoi, %eax\n"
        "  test %eax, %eax\n  jz 1f\n"
        "  movl $0, (%eax)\n  jmp 2f\n"
        "1: movl $0x80b, %ecx\n  xorl %eax, %eax\n  xorl %edx, %edx\n  wrmsr\n"
        "2: pop %edx\n  pop %ecx\n  pop %eax\n  iret\n"
        "smp_spurious_stub:\n  iret\n");
#endif
void smp_wake_stub(void);
void smp_spurious_stub(void);

static void run_part(const struct job *j, int part)
{
    j->fn(j->ctx, (int)((int64_t)j->count * part / j->parts), (int)((int64_t)j->count * (part + 1) / j->parts));
}

static int work_on(void)
{
    int taken = 0;
    for (;;) {
        uint32_t t = __atomic_fetch_add(&work, 1, __ATOMIC_SEQ_CST);
        uint32_t g1 = __atomic_load_n(&job_gen, __ATOMIC_ACQUIRE);
        struct job j = *(volatile struct job *)&job;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t g2 = __atomic_load_n(&job_gen, __ATOMIC_ACQUIRE);
        int part = (int)(t & 0xff);
        if (g1 != g2 || g1 != t >> 8 || part >= j.parts)
            return taken;
        run_part(&j, part);
        taken++;
        __atomic_add_fetch(&done, 1, __ATOMIC_RELEASE);
    }
}

void ap_entry(int index);

void ap_entry(int index)
{
    cpu_ap_init(index);
    apic_local_enable();
    /* only the IPI may reach this processor: legacy interrupts (LINT0/1) and local sources stay with the first */
    for (uint32_t reg = 0x320u; reg <= 0x370u; reg += 0x10u)
        apic_write(reg, 0x10000u);
    __atomic_add_fetch(&online, 1, __ATOMIC_SEQ_CST);
    if (!cpu_info()->sse) {
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    __atomic_add_fetch(&helpers, 1, __ATOMIC_SEQ_CST);
    uint32_t seen = JOB_NONE;
    for (;;) {
        uint32_t g;
        for (int spin = 0; (g = __atomic_load_n(&job_gen, __ATOMIC_ACQUIRE)) == seen || g == JOB_NONE; spin++) {
            if (spin < 4000) {
                __asm__ volatile("pause");
                continue;
            }
            __asm__ volatile("sti; hlt; cli" ::: "memory");
        }
        seen = g;
        int n = work_on();
        if (n)
            __atomic_add_fetch(&smp_parts_helped, (uint32_t)n, __ATOMIC_RELAXED);
    }
}

int hal_parallel_cpus(void)
{
    return helpers + 1;
}

void hal_parallel(void (*fn)(void *ctx, int from, int to), void *ctx, int count, int grain)
{
    if (count <= 0)
        return;
    int cpus = helpers + 1;
    grain = MAX(grain, 1);
    if (cpus < 2 || count < 2 * grain || hal_in_interrupt() || __atomic_exchange_n(&pool_busy, 1, __ATOMIC_ACQUIRE)) {
        fn(ctx, 0, count);
        return;
    }
    int parts = MIN(MIN(count / grain, cpus * 3), MAX_PARTS);
    __atomic_store_n(&job_gen, JOB_NONE, __ATOMIC_SEQ_CST);
    job = (struct job){fn, ctx, count, parts};
    done = 0;
    uint32_t g = (generation = (generation + 1) & 0xffffff);
    __atomic_store_n(&job_gen, g, __ATOMIC_SEQ_CST);
    __atomic_store_n(&work, g << 8, __ATOMIC_SEQ_CST);
    send_ipi(ICR_ALL_BUT_SELF | ICR_ASSERT | WAKE_VECTOR);
    work_on();
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < (uint32_t)parts)
        __asm__ volatile("pause");
    smp_jobs++;
    __atomic_store_n(&pool_busy, 0, __ATOMIC_RELEASE);
}

/* The highest free page below the video memory that the boot data does not use. */
static uint32_t low_page(void)
{
    const struct x86_bootinfo *bi = x86_boot_info();
    const struct e820_entry *map = (const struct e820_entry *)(uintptr_t)bi->mmap_addr;
    uint32_t keep_lo = MIN((uint32_t)(uintptr_t)bi, bi->mmap_addr) & ~4095u;
    uint32_t keep_hi = ALIGN_UP(MAX((uint32_t)(uintptr_t)bi + sizeof(*bi), bi->mmap_addr + bi->mmap_count * 24u), 4096u);
    uint32_t best = 0;
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        if (map[i].type != 1 || map[i].base >= 0x9f000u)
            continue;
        uint32_t lo = ALIGN_UP((uint32_t)MAX(map[i].base, 0x8000ull), 4096u);
        uint32_t hi = (uint32_t)MIN(map[i].base + map[i].length, 0x9f000ull) & ~4095u;
        for (uint32_t p = hi; p >= lo + 4096u; p -= 4096u) {
            uint32_t page = p - 4096u;
            if (page + 4096u <= keep_lo || page >= keep_hi) {
                best = MAX(best, page);
                break;
            }
        }
    }
    return best;
}

int hal_start_cpus(void)
{
    if (!cpu_info()->apic || !cpu_info()->sse || !vmm_pae())
        return 1;
    uint64_t base = rdmsr(APIC_BASE_MSR);
    x2apic = base & APIC_X2;
    if (!x2apic && !(lapic = vmm_map_uncached(base & 0xfffff000u, 4096)))
        return 1;
    smp_eoi = x2apic ? NULL : lapic + REG_EOI / 4;
    cpu_set_gate(WAKE_VECTOR, (uintptr_t)smp_wake_stub);
    cpu_set_gate(SPURIOUS_VECTOR, (uintptr_t)smp_spurious_stub);
    wrmsr(APIC_BASE_MSR, base | APIC_ENABLE);
    apic_local_enable();
    uint32_t page = low_page();
    size_t size = (size_t)(smp_boot_end - smp_boot_start);
    if (!page || size > 4096)
        return 1;
    uint8_t *code = (uint8_t *)(uintptr_t)page;
    memcpy(code, smp_boot_start, size);
    const uint16_t *off = (const uint16_t *)(code + 2038);
    uint32_t *vars = (uint32_t *)(code + off[4]);
    *(uint32_t *)(code + off[0]) = page + off[3];
    *(uint32_t *)(code + off[1]) = page + off[2];
    uintptr_t cr3, cr4;
    __asm__ volatile("mov %%cr3, %0; mov %%cr4, %1" : "=r"(cr3), "=r"(cr4));
    void **stacks = kzalloc(sizeof(void *) * MAX_CPUS);
    if (!stacks)
        return 1;
    for (int i = 1; i < MAX_CPUS; i++) {
        uint8_t *s = kmalloc(AP_STACK);
        stacks[i] = s ? s + AP_STACK : NULL;
    }
    vars[0] = (uint32_t)cr3;
    vars[1] = (uint32_t)cr4;
#ifdef __x86_64__
    /* the 64-bit start-up code keeps 8-byte entry and stack pointers */
    uint64_t entry = (uintptr_t)ap_entry, stack_list = (uintptr_t)stacks;
    memcpy(&vars[2], &entry, 8);
    memcpy(&vars[4], &stack_list, 8);
    vars[6] = 1;
    vars[7] = MAX_CPUS;
#else
    vars[2] = (uint32_t)(uintptr_t)ap_entry;
    vars[3] = (uint32_t)(uintptr_t)stacks;
    vars[4] = 1;
    vars[5] = MAX_CPUS;
#endif
    send_ipi(ICR_ALL_BUT_SELF | ICR_INIT | ICR_ASSERT);
    hal_sleep_ms(10);
    for (int i = 0; i < 2; i++) {
        send_ipi(ICR_ALL_BUT_SELF | ICR_STARTUP | (page >> 12));
        hal_sleep_ms(2);
    }
    /* every processor that answers checks in; done when none has for a while */
    int seen = 0;
    for (int quiet = 0, waited = 0; quiet < 4 || waited < 50; waited += 5) {
        hal_sleep_ms(5);
        int now = __atomic_load_n(&online, __ATOMIC_SEQ_CST);
        quiet = now == seen ? quiet + 1 : 0;
        seen = now;
    }
    klog("smp: %d processor%s (APIC %s, start-up page %05x), %d help%s draw", seen, seen == 1 ? "" : "s",
         x2apic ? "x2" : "xAPIC", page, helpers, helpers == 1 ? "s" : "");
    return seen;
}

int hal_cpu_count(void)
{
    return online;
}
