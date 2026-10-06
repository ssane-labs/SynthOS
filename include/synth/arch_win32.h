#ifndef SYNTH_ARCH_WIN32_H
#define SYNTH_ARCH_WIN32_H

#include <synth/types.h>

struct w32_jump {
    uint32_t ebx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t eip;
    uint32_t active;
};

typedef void (*arch_fault_fn)(int vector, uint32_t eip, uint32_t address);

uint32_t arch_w32_call(void *fn, int argc, const uint32_t *args, void *stack_top, struct w32_jump *jump);
NORETURN void arch_w32_abort(struct w32_jump *jump, uint32_t code);
void arch_w32_set_teb(uint32_t base);
void arch_set_fault_handler(arch_fault_fn fn);
void arch_set_tick_hook(void (*fn)(uint32_t eip));
bool arch_has_sse(void);

#endif
