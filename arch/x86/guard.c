#include <synth/arch_win32.h>
#include <synth/hal.h>
#include <synth/log.h>

#include "io.h"

/* hal_guard: a CPU fault in kernel code called through it (a desktop app drawing or handling an event) unwinds
   back to it instead of stopping the system. Not inside interrupt handlers: those may hold hardware state. */
struct guard {
    void *buf[5];
    struct guard *prev;
};

static struct guard *current;
static volatile int caught;

int hal_guard(void (*fn)(void *ctx), void *ctx)
{
    struct guard g = {.prev = current};
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
