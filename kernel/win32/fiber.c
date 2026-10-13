#include <synth/arch_win32.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "win32.h"

#define FIBER_MIN_STACK (64u * 1024)
#define FIBER_MAX_STACK (8u * 1024 * 1024)

struct w32_fiber {
    void *data;
    uint32_t esp;
    uint8_t *stack;
    uint32_t stack_size;
    void *start;
    struct w32_thread *converted;
    uint8_t fpu_area[528];
};

static uint8_t *fpu_of(struct w32_fiber *f)
{
    return (uint8_t *)(((uintptr_t)f->fpu_area + 15) & ~(uintptr_t)15);
}

static struct w32_fiber *current_fiber(void)
{
    uint32_t v;
    memcpy(&v, w32->current->teb + 0x10, 4);
    return w32->current->fiber ? (struct w32_fiber *)v : NULL;
}

static void set_current(struct w32_fiber *f)
{
    uint8_t *teb = w32->current->teb;
    uint32_t top = (uint32_t)f->stack + f->stack_size, bottom = (uint32_t)f->stack, self = (uint32_t)f;
    memcpy(teb + 0x10, &self, 4);
    if (f->stack) {
        memcpy(teb + 0x04, &top, 4);
        memcpy(teb + 0x08, &bottom, 4);
    }
}

static void *WINAPI ConvertThreadToFiberEx(void *data, DWORD flags)
{
    UNUSED(flags);
    struct w32_thread *t = w32->current;
    if (t->fiber) {
        w32_set_error(1280);
        return NULL;
    }
    struct w32_fiber *f = kzalloc(sizeof(*f));
    if (!f) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    f->data = data;
    f->converted = t;
    memcpy(&f->stack_size, t->teb + 0x04, 4);
    memcpy(&f->stack, t->teb + 0x08, 4);
    f->stack_size -= (uint32_t)f->stack;
    t->fiber = f;
    set_current(f);
    return f;
}

static void *WINAPI ConvertThreadToFiber(void *data)
{
    return ConvertThreadToFiberEx(data, 0);
}

static BOOL WINAPI ConvertFiberToThread(void)
{
    struct w32_thread *t = w32->current;
    struct w32_fiber *f = t->fiber;
    if (!f) {
        w32_set_error(1281);
        return FALSE;
    }
    uint32_t zero = 0;
    memcpy(t->teb + 0x10, &zero, 4);
    t->fiber = NULL;
    if (f->converted == t)
        kfree(f);
    return TRUE;
}

static BOOL WINAPI IsThreadAFiber(void)
{
    return w32->current->fiber != NULL;
}

static NORETURN void fiber_entry(void)
{
    struct w32_fiber *f = current_fiber();
    arch_fpu_restore(fpu_of(f));
    typedef void WINAPI (*fiber_fn)(void *);
    ((fiber_fn)f->start)(f->data);
    void(WINAPI * exit_thread)(DWORD) = (void(WINAPI *)(DWORD))w32_builtin_lookup("kernel32.dll", "ExitThread");
    exit_thread(0);
    for (;;)
        ;
}

static void *WINAPI CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, void *start, void *data)
{
    UNUSED(flags);
    struct w32_fiber *f = kzalloc(sizeof(*f));
    uint32_t size = (uint32_t)MIN(MAX(ALIGN_UP(MAX(commit, reserve), 4096), FIBER_MIN_STACK), FIBER_MAX_STACK);
    if (!reserve && !commit)
        size = 1024 * 1024;
    uint8_t *stack = f ? kmalloc_pages_uninit(size) : NULL;
    if (!stack) {
        kfree(f);
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    f->data = data;
    f->start = start;
    f->stack = stack;
    f->stack_size = size;
    arch_fpu_save(fpu_of(f));
    uint32_t *sp = (uint32_t *)(((uint32_t)stack + size - 64) & ~15u);
    *--sp = 0;
    *--sp = (uint32_t)fiber_entry;
    for (int i = 0; i < 4; i++)
        *--sp = 0;
    *--sp = 0x202;
    f->esp = (uint32_t)sp;
    return f;
}

static void *WINAPI CreateFiber(SIZE_T stack, void *start, void *data)
{
    return CreateFiberEx(0, stack, 0, start, data);
}

static void WINAPI SwitchToFiber(struct w32_fiber *next)
{
    struct w32_fiber *cur = current_fiber();
    if (!cur || !next || cur == next)
        return;
    arch_fpu_save(fpu_of(cur));
    set_current(next);
    arch_switch(&cur->esp, next->esp);
    arch_fpu_restore(fpu_of(cur));
}

static void WINAPI DeleteFiber(struct w32_fiber *f)
{
    if (!f)
        return;
    if (f == current_fiber()) {
        void(WINAPI * exit_thread)(DWORD) = (void(WINAPI *)(DWORD))w32_builtin_lookup("kernel32.dll", "ExitThread");
        exit_thread(0);
    }
    if (f->converted) {
        if (f->converted->fiber == f)
            f->converted->fiber = NULL;
    } else {
        kfree_pages(f->stack);
    }
    kfree(f);
}

static const struct w32_export fiber_exports[] = {
    W32_FN(ConvertThreadToFiberEx), W32_FN(ConvertThreadToFiber), W32_FN(ConvertFiberToThread),
    W32_FN(IsThreadAFiber),         W32_FN(CreateFiberEx),        W32_FN(CreateFiber),
    W32_FN(SwitchToFiber),          W32_FN(DeleteFiber),
};

W32_EXPORTS(fiber_module, "kernel32.dll", fiber_exports);
