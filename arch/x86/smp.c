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

void ap_entry(int index);

void ap_entry(int index)
{
    cpu_ap_init(index);
    apic_local_enable();
    __atomic_add_fetch(&online, 1, __ATOMIC_SEQ_CST);
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* The highest free page below the video memory that the boot data does not use. */
static uint32_t low_page(void)
{
    const struct x86_bootinfo *bi = x86_boot_info();
    const struct e820_entry *map = (const struct e820_entry *)bi->mmap_addr;
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
    uint32_t cr3, cr4;
    __asm__ volatile("mov %%cr3, %0; mov %%cr4, %1" : "=r"(cr3), "=r"(cr4));
    void **stacks = kzalloc(sizeof(void *) * MAX_CPUS);
    if (!stacks)
        return 1;
    for (int i = 1; i < MAX_CPUS; i++) {
        uint8_t *s = kmalloc(AP_STACK);
        stacks[i] = s ? s + AP_STACK : NULL;
    }
    vars[0] = cr3;
    vars[1] = cr4;
    vars[2] = (uint32_t)(uintptr_t)ap_entry;
    vars[3] = (uint32_t)(uintptr_t)stacks;
    vars[4] = 1;
    vars[5] = MAX_CPUS;
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
    klog("smp: %d processor%s (APIC %s, start-up page %05x)", seen, seen == 1 ? "" : "s", x2apic ? "x2" : "xAPIC", page);
    return seen;
}

int hal_cpu_count(void)
{
    return online;
}
