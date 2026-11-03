#include <synth/arch_win32.h>
#include <synth/hal.h>
#include <synth/log.h>

#include "io.h"

/* hal_guard: a CPU fault in kernel code called through it (a desktop app drawing or handling an event) unwinds
   back to it instead of stopping the system. Not inside interrupt handlers: those may hold hardware state. */
struct guard {
    void *buf[5];
    struct guard *prev;
    uint64_t start;
    uint32_t ticks, in_app;
};

static struct guard *current;
static volatile int caught;

int hal_guard(void (*fn)(void *ctx), void *ctx)
{
    struct guard g = {.prev = current, .start = hal_ticks_ms()};
    uint32_t flags = cpu_irq_save();
    cpu_irq_restore(flags);
    if (__builtin_setjmp(g.buf)) {
        current = g.prev;
        cpu_irq_restore(flags);
        return caught;
    }
    current = &g;
    fn(ctx);
    current = g.prev;
    return 0;
}

void arch_guard_catch(int vector, uintptr_t pc, uintptr_t address)
{
    if (!current || arch_irq_depth() > 0 || vector == 2 || vector == 8 || vector == 18)
        return;
    klog("guard: CPU exception %d at %lx (address %lx) caught, the app is closed", vector, (unsigned long)pc,
         (unsigned long)address);
    caught = vector;
    struct guard *g = current;
    __builtin_longjmp(g->buf, 1);
}

/* An app that has not returned for HANG_MS and spent most of that time in its own code (not waiting for the disk
   or inside the kernel's heap or files) is stuck in a loop: the timer interrupt resumes it at arch_guard_escape,
   which unwinds to the guard like a fault does. */
#define HANG_MS 6000

extern char __apps_text_start[], __apps_text_end[];

bool arch_guard_tick(uintptr_t pc)
{
    struct guard *g = current;
    if (!g || arch_irq_depth() != 1)
        return false;
    g->ticks++;
    if (pc >= (uintptr_t)__apps_text_start && pc < (uintptr_t)__apps_text_end)
        g->in_app++;
    else
        return false;
    return hal_ticks_ms() - g->start > HANG_MS && g->in_app * 4 >= g->ticks * 3;
}

void arch_guard_escape(void)
{
    klog("guard: an app stopped responding, it is closed");
    caught = HAL_GUARD_HUNG;
    __builtin_longjmp(current->buf, 1);
}
