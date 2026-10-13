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
void arch_fault_registers(uint32_t out[8]);
void arch_set_tick_hook(void (*fn)(uint32_t eip));
bool arch_has_sse(void);
void arch_switch(uint32_t *save_esp, uint32_t new_esp);
int arch_irq_depth(void);
void arch_set_irq_depth(int depth);
void arch_fpu_save(void *area);
void arch_fpu_restore(const void *area);
void arch_load_teb_selector(void);
void arch_w64_set_teb(uint32_t base);
bool arch_long_supported(void);
bool arch_long_enter(void);
void arch_long_leave(void);
bool arch_long_active(void);
uint64_t arch_w64_call(uint32_t fn, int argc, const uint64_t *args);
uint32_t arch_call_on_stack(uint32_t (*fn)(void *), void *arg, void *stack_top);

#endif
