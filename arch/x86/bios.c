#include <synth/hal.h>
#include <synth/string.h>

#include "io.h"

#define THUNK_ADDR 0x1000u
#define THUNK_REGS 0x1f00u
#define BDA_TICK_MS 55u

extern const uint8_t bios_thunk_start[], bios_thunk_end[];

static bool ready;
static uint64_t tsc_per_ms;
static uint64_t tsc_remainder;

void bios_init(void)
{
    size_t size = (size_t)(bios_thunk_end - bios_thunk_start);
    if (size > THUNK_REGS - THUNK_ADDR)
        return;
    memcpy((void *)THUNK_ADDR, bios_thunk_start, size);
    if (cpu_info()->tsc) {
        uint64_t t0 = cpu_rdtsc();
        hal_sleep_ms(10);
        tsc_per_ms = (cpu_rdtsc() - t0) / 10;
    }
    ready = true;
}

bool bios_available(void)
{
    return ready;
}

bool bios_call(uint8_t intno, struct bios_regs *r)
{
    if (!ready)
        return false;
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    uint8_t mask1 = inb(0x21), mask2 = inb(0xa1);
    struct bios_regs *shared = (struct bios_regs *)THUNK_REGS;
    *shared = *r;
    shared->intno = intno;
    pic_program(0x08, 0x70);
    outb(0x21, 0x1a);
    outb(0xa1, 0x00);
    uint64_t start = tsc_per_ms ? cpu_rdtsc() : 0;
    pit_bios_mode(true);
    ((void (*)(void))THUNK_ADDR)();
    pit_bios_mode(false);
    pic_program(0x20, 0x28);
    outb(0x21, mask1);
    outb(0xa1, mask2);
    *r = *shared;
    if (tsc_per_ms) {
        tsc_remainder += cpu_rdtsc() - start;
        pit_advance_ms((uint32_t)(tsc_remainder / tsc_per_ms));
        tsc_remainder %= tsc_per_ms;
    } else {
        uint32_t elapsed = shared->ticks_after - shared->ticks_before;
        if (elapsed > 1 && elapsed < 0x10000u)
            pit_advance_ms((elapsed - 1) * BDA_TICK_MS);
    }
    if (flags & 0x200)
        __asm__ volatile("sti");
    return !(r->eflags & 1);
}
