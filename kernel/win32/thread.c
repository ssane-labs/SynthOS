#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "win32.h"


#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xffffffffu
#define ERROR_TIMEOUT 1460
#define ERROR_NOT_OWNER 288
#define STILL_ACTIVE 259
#define CREATE_SUSPENDED 4
#define DEFAULT_STACK (256u * 1024)
#define MAX_STACK (4u * 1024 * 1024)
#define STACK_GUARD (64u * 1024)
#define STACK_CANARY 0x5ac0ffeeu

struct critical_section {
    void *debug;
    LONG lock_count;
    LONG recursion;
    DWORD owner;
    HANDLE semaphore;
    ULONG_PTR spin;
};

static uint8_t *fpu_of(struct w32_thread *t)
{
    return (uint8_t *)(((uintptr_t)t->fpu_area + 15) & ~(uintptr_t)15);
}

uint32_t w32_thread_id(void)
{
    return w32 && w32->current ? w32->current->id : 0x104;
}

bool w32_threads_start(void)
{
    struct w32_thread *t = w32->threads;
    if (!t) {
        t = kzalloc(sizeof(*t));
        if (!t)
            return false;
        t->stack = w32->stack;
        t->stack_size = w32->stack_size;
        t->next = t;
        w32->threads = w32->current = t;
        w32->live_threads = 1;
    }
    t->id = 0x104;
    t->teb = w32->teb;
    t->proc = w32;
    w32->next_tid = 0x108;
    return true;
}

void w32_threads_end(void)
{
    struct w32_thread *t = w32->threads;
    if (t) {
        do {
            struct w32_thread *next = t->next;
            if (!t->boot) {
                if (t->stack != w32->stack)
                    kfree_pages(t->stack - STACK_GUARD);
                kfree(t);
            }
            t = next;
        } while (t != w32->threads);
    }
    w32->threads = w32->current = NULL;
    w32->live_threads = 0;
    arch_set_irq_depth(0);
}

void w32_thread_frame(struct w32_thread *t, void (*entry)(void), uint8_t *stack_top)
{
    arch_fpu_save((uint8_t *)(((uintptr_t)t->fpu_area + 15) & ~(uintptr_t)15));
    uint32_t *sp = (uint32_t *)(((uint32_t)stack_top - 64) & ~15u);
    *--sp = 0;
    *--sp = (uint32_t)entry;
    for (int i = 0; i < 4; i++)
        *--sp = 0;
    *--sp = 0x202;
    t->esp = (uint32_t)sp;
}

void w32_idle(int ms)
{
    con_flush();
    if (hal_in_interrupt())
        return;
    if (input_poll() || ms <= 0)
        return;
    hal_sleep_ms((uint32_t)ms);
}

static struct w32_thread *pick(struct w32_thread *cur, bool active_only)
{
    for (struct w32_thread *t = cur->next; t != cur; t = t->next) {
        if (t->done || t->suspended)
            continue;
        if (active_only && t->waiting)
            continue;
        return t;
    }
    return NULL;
}

static struct w32_thread *pick_other(bool active_only)
{
    for (struct w32_process *p = w32_ring_next(w32); p && p != w32; p = w32_ring_next(p)) {
        if (p->dead || p->frozen || !p->current)
            continue;
        struct w32_thread *t = p->current;
        do {
            if (!t->done && !t->suspended && (!active_only || !t->waiting))
                return t;
            t = t->next;
        } while (t != p->current);
    }
    return NULL;
}

static void check_stack(struct w32_thread *t);

static void switch_to(struct w32_thread *next)
{
    struct w32_thread *cur = w32->current;
    if (next == cur)
        return;
    check_stack(cur);
    cur->irq_depth = arch_irq_depth();
    arch_fpu_save(fpu_of(cur));
    if (next->proc && next->proc != w32)
        w32_process_enter(next->proc);
    w32->current = next;
    w32->teb = next->teb;
    w32->teb64 = next->teb64;
    if (next->teb)
        arch_w32_set_teb((uint32_t)next->teb);
    if (next->teb64)
        arch_w64_set_teb((uint32_t)next->teb64);
    arch_load_teb_selector();
    arch_switch(&cur->esp, next->esp);
    arch_fpu_restore(fpu_of(cur));
    arch_set_irq_depth(cur->irq_depth);
    w32_reap();
}

void w32_switch_thread(struct w32_thread *t)
{
    switch_to(t);
}

NORETURN void w32_leave_forever(void)
{
    struct w32_thread *next = pick_other(true);
    if (!next)
        next = pick_other(false);
    if (!next)
        panic("win32: no process left to run");
    w32_process_enter(next->proc);
    w32->current = next;
    w32->teb = next->teb;
    w32->teb64 = next->teb64;
    if (next->teb)
        arch_w32_set_teb((uint32_t)next->teb);
    arch_load_teb_selector();
    static uint32_t dead_esp;
    arch_switch(&dead_esp, next->esp);
    panic("win32: a finished process was resumed");
}

void w32_yield(void)
{
    if (!w32 || !w32->current)
        return;
    struct w32_thread *next = w32->live_threads > 1 ? pick(w32->current, true) : NULL;
    if (!next)
        next = pick_other(true);
    if (next)
        switch_to(next);
}

void w32_block(void)
{
    w32_io_poll();
    struct w32_thread *cur = w32->current;
    if (!cur) {
        w32_idle(1);
    } else {
        cur->waiting = true;
        struct w32_thread *next = w32->live_threads > 1 ? pick(cur, true) : NULL;
        if (!next)
            next = pick_other(true);
        if (!next) {
            w32_idle(1);
            next = w32->live_threads > 1 ? pick(cur, false) : NULL;
            if (!next)
                next = pick_other(false);
        }
        if (next)
            switch_to(next);
        cur->waiting = false;
    }
    if (sh_interrupted())
        w32_exit(STATUS_CONTROL_C_EXIT);
}

void w32_preempt(void)
{
    static unsigned turn;
    if (!w32 || !w32->current)
        return;
    struct w32_thread *next = NULL;
    if (++turn % 2 == 0)
        next = pick_other(true);
    if (!next && w32->live_threads > 1) {
        next = pick(w32->current, true);
        if (!next)
            next = pick(w32->current, false);
    }
    if (!next)
        next = pick_other(true);
    if (next)
        switch_to(next);
}

static bool tls_attach(uint8_t *teb, int index)
{
    void **slots;
    memcpy(&slots, teb + 0x2c, 4);
    if (!slots) {
        slots = w32_zalloc(sizeof(void *) * W32_TLS_MODULES);
        if (!slots)
            return false;
        memcpy(teb + 0x2c, &slots, 4);
    }
    const struct w32_tls_module *m = &w32->tls_mods[index];
    uint8_t *block = w32_zalloc(m->size + m->zero + 16);
    if (!block)
        return false;
    memcpy(block, (const void *)m->start, m->size);
    slots[index] = block;
    return true;
}

void w32_tls_register(struct w32_image *img)
{
    const uint8_t *mem = (const uint8_t *)img->base;
    uint32_t opt = *(const uint32_t *)(mem + 0x3c) + 24;
    uint32_t rva = *(const uint32_t *)(mem + opt + 96 + 9 * 8);
    if (!rva || w32->tls_mod_count >= W32_TLS_MODULES)
        return;
    const uint32_t *dir = (const uint32_t *)(mem + rva);
    int index = w32->tls_mod_count++;
    w32->tls_mods[index] = (struct w32_tls_module){img->base, dir[0], dir[1] - dir[0], dir[4], dir[3]};
    if (dir[2])
        *(uint32_t *)dir[2] = (uint32_t)index;
    struct w32_thread *t = w32->threads;
    if (t)
        do
            tls_attach(t->teb, index);
        while ((t = t->next) != w32->threads);
}

void w32_tls_callbacks(uint32_t base, DWORD reason)
{
    for (int i = 0; i < w32->tls_mod_count && !w32->exiting; i++) {
        const struct w32_tls_module *m = &w32->tls_mods[i];
        if ((base && m->base != base) || !m->callbacks)
            continue;
        for (const uint32_t *cb = (const uint32_t *)m->callbacks; *cb; cb++) {
            uint32_t args[3] = {m->base, reason, 0};
            w32_call((void *)*cb, 3, args);
        }
    }
}

static void run_tls_callbacks(DWORD reason)
{
    for (int i = 0; i < w32->tls_mod_count; i++) {
        const struct w32_tls_module *m = &w32->tls_mods[i];
        for (const uint32_t *cb = (const uint32_t *)m->callbacks; cb && *cb; cb++) {
            typedef void WINAPI (*tls_fn)(uint32_t, DWORD, void *);
            ((tls_fn)*cb)(m->base, reason, NULL);
        }
    }
}

static NORETURN void finish(uint32_t code)
{
    struct w32_thread *t = w32->current;
    run_tls_callbacks(3);
    if (w32->win64)
        w64_tls_callbacks(3);
    t->done = true;
    t->exit_code = code;
    if (--w32->live_threads <= 0)
        w32_exit(code);
    for (;;) {
        struct w32_thread *next = pick(t, false);
        if (next)
            switch_to(next);
        else
            w32_exit(code);
    }
}

static void thread_entry(void)
{
    struct w32_thread *t = w32->current;
    arch_fpu_restore(fpu_of(t));
    arch_set_irq_depth(0);
    arch_load_teb_selector();
    run_tls_callbacks(2);
    typedef DWORD WINAPI (*start_fn)(void *);
    finish(((start_fn)t->start)(t->param));
}

static uint8_t *make_teb(struct w32_thread *t)
{
    uint8_t *teb = heap_alloc_aligned(&w32->heap, 4096, 4096);
    if (!teb)
        return NULL;
    memset(teb, 0, 4096);
    uint32_t v;
    v = 0xffffffffu;
    memcpy(teb + 0x00, &v, 4);
    v = (uint32_t)t->stack + t->stack_size;
    memcpy(teb + 0x04, &v, 4);
    v = (uint32_t)t->stack;
    memcpy(teb + 0x08, &v, 4);
    v = (uint32_t)teb;
    memcpy(teb + 0x18, &v, 4);
    v = 0x100;
    memcpy(teb + 0x20, &v, 4);
    memcpy(teb + 0x24, &t->id, 4);
    memcpy(teb + 0x30, w32->teb + 0x30, 4);
    memcpy(teb + 0xe10, w32->threads->teb + 0xe10, 64 * 4);
    for (int i = 0; i < w32->tls_mod_count; i++)
        if (!tls_attach(teb, i))
            return NULL;
    return teb;
}

static struct w32_thread *thread_of(HANDLE h)
{
    if (h == W32_PSEUDO_THREAD)
        return w32->current;
    struct w32_object *o = w32_handle_get(h);
    return o && o->type == WH_THREAD ? o->thread : NULL;
}

static uint32_t image_stack_reserve(void)
{
    if (!w32->main || !w32->main->base)
        return DEFAULT_STACK;
    const uint8_t *mem = (const uint8_t *)w32->main->base;
    uint32_t lfanew, reserve;
    memcpy(&lfanew, mem + 0x3c, 4);
    memcpy(&reserve, mem + lfanew + 24 + 72, 4);
    return reserve;
}

static void check_stack(struct w32_thread *t)
{
    if (!t->stack || t->stack == w32->stack || t->boot)
        return;
    const uint32_t *canary = (const uint32_t *)(t->stack - STACK_GUARD);
    for (int i = 0; i < 16; i++)
        if (canary[i] != STACK_CANARY) {
            klog("win32: thread %u overflowed its %u KB stack", t->id, t->stack_size / 1024);
            ksnprintf(w32->message, sizeof(w32->message), "crashed: stack overflow in thread %u", t->id);
            w32_exit(0xc00000fdu);
        }
}

HANDLE w32_create_thread(SIZE_T stack, void *start, void *param, DWORD flags, DWORD *id)
{
    struct w32_thread *t = kzalloc(sizeof(*t));
    if (!t) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    uint32_t reserve = heap_free_bytes() >= 256u * 1024 * 1024 ? MIN(image_stack_reserve(), 1024u * 1024) : DEFAULT_STACK;
    uint32_t size = (flags & 0x10000) ? (uint32_t)stack : MAX((uint32_t)stack, reserve);
    t->stack_size = (uint32_t)MIN(MAX(ALIGN_UP(size ? size : reserve, 4096), DEFAULT_STACK), MAX_STACK);
    uint8_t *block = kmalloc_pages_uninit(t->stack_size + STACK_GUARD);
    t->stack = block ? block + STACK_GUARD : NULL;
    for (int i = 0; block && i < 16; i++)
        ((uint32_t *)block)[i] = STACK_CANARY;
    t->proc = w32;
    t->id = w32->next_tid;
    w32->next_tid += 4;
    if (!t->stack || !(t->teb = make_teb(t)) || (w32->win64 && !(t->teb64 = w64_make_teb((uint32_t)t->stack, t->stack_size, t->id)))) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    t->start = start;
    t->param = param;
    t->suspended = (flags & CREATE_SUSPENDED) ? 1 : 0;
    w32_thread_frame(t, thread_entry, t->stack + t->stack_size);
    HANDLE h = w32_handle_new(WH_THREAD);
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return NULL;
    o->thread = t;
    struct w32_thread *last = w32->threads;
    while (last->next != w32->threads)
        last = last->next;
    t->next = w32->threads;
    last->next = t;
    w32->live_threads++;
    if (id)
        *id = t->id;
    return h;
}

static HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, void *start, void *param, DWORD flags, DWORD *id)
{
    UNUSED(sa);
    return w32_create_thread(stack, start, param, flags, id);
}

static void WINAPI ExitThread(DWORD code)
{
    finish(code);
}

static BOOL WINAPI TerminateThread(HANDLE h, DWORD code)
{
    struct w32_thread *t = thread_of(h);
    if (!t)
        return FALSE;
    if (t == w32->current)
        finish(code);
    if (!t->done) {
        t->done = true;
        t->exit_code = code;
        w32->live_threads--;
    }
    return TRUE;
}

static BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD *code)
{
    struct w32_thread *t = thread_of(h);
    if (!t) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *code = t->done ? t->exit_code : STILL_ACTIVE;
    return TRUE;
}

static DWORD WINAPI SuspendThread(HANDLE h)
{
    struct w32_thread *t = thread_of(h);
    if (!t)
        return 0xffffffffu;
    DWORD previous = (DWORD)t->suspended++;
    if (t == w32->current)
        w32_block();
    return previous;
}

static DWORD WINAPI ResumeThread(HANDLE h)
{
    struct w32_thread *t = thread_of(h);
    if (!t)
        return 0xffffffffu;
    DWORD previous = (DWORD)t->suspended;
    if (t->suspended)
        t->suspended--;
    return previous;
}

static DWORD WINAPI GetCurrentThreadId(void)
{
    return w32_thread_id();
}

static DWORD WINAPI GetThreadId(HANDLE h)
{
    struct w32_thread *t = thread_of(h);
    return t ? t->id : 0;
}

static BOOL WINAPI SetThreadPriority(HANDLE h, int priority)
{
    UNUSED(priority);
    return thread_of(h) != NULL;
}

static int WINAPI GetThreadPriority(HANDLE h)
{
    UNUSED(h);
    return 0;
}

static BOOL WINAPI SwitchToThread(void)
{
    struct w32_thread *cur = w32->current;
    bool other = cur && pick(cur, true);
    w32_yield();
    return other;
}

void w32_sleep(uint32_t ms)
{
    con_flush();
    if (ms == 0) {
        w32_yield();
        return;
    }
    uint64_t end = hal_ticks_ms() + ms;
    while (hal_ticks_ms() < end) {
        if (w32->live_threads > 1)
            w32_block();
        else
            hal_sleep_ms((uint32_t)MIN(end - hal_ticks_ms(), (uint64_t)10));
        if (sh_interrupted())
            w32_exit(STATUS_CONTROL_C_EXIT);
    }
}

static void WINAPI Sleep(DWORD ms)
{
    w32_sleep(ms);
}

static DWORD WINAPI SleepEx(DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    w32_sleep(ms);
    return 0;
}

static HANDLE new_object(enum w32_object_type type, uint32_t value, uint32_t max, bool manual)
{
    HANDLE h = w32_handle_new(type);
    struct w32_object *o = w32_handle_get(h);
    if (o) {
        o->value = value;
        o->max = max;
        o->manual = manual;
    }
    return h;
}

static HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, LPCSTR name)
{
    UNUSED(name);
    return w32_inheritable(new_object(WH_EVENT, initial ? 1 : 0, 1, manual), sa);
}

static HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    UNUSED(name);
    return CreateEventA(sa, manual, initial, NULL);
}

static HANDLE WINAPI CreateEventExW(void *sa, LPCWSTR name, DWORD flags, DWORD access)
{
    UNUSED(name);
    UNUSED(access);
    return CreateEventA(sa, (flags & 1) != 0, (flags & 2) != 0, NULL);
}

static HANDLE WINAPI CreateMutexA(void *sa, BOOL owner, LPCSTR name)
{
    UNUSED(sa);
    UNUSED(name);
    HANDLE h = new_object(WH_MUTEX, owner ? 1 : 0, 0, false);
    struct w32_object *o = w32_handle_get(h);
    if (o && owner)
        o->owner = w32_thread_id();
    return h;
}

static HANDLE WINAPI CreateMutexW(void *sa, BOOL owner, LPCWSTR name)
{
    UNUSED(name);
    return CreateMutexA(sa, owner, NULL);
}

static HANDLE WINAPI CreateSemaphoreA(void *sa, LONG initial, LONG max, LPCSTR name)
{
    UNUSED(sa);
    UNUSED(name);
    if (initial < 0 || max <= 0 || initial > max) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return new_object(WH_SEMAPHORE, (uint32_t)initial, (uint32_t)max, false);
}

static HANDLE WINAPI CreateSemaphoreW(void *sa, LONG initial, LONG max, LPCWSTR name)
{
    UNUSED(name);
    return CreateSemaphoreA(sa, initial, max, NULL);
}

static struct w32_object *object(HANDLE h, enum w32_object_type type)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != type) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return o;
}

static BOOL WINAPI SetEvent(HANDLE h)
{
    struct w32_object *o = object(h, WH_EVENT);
    if (o)
        o->value = 1;
    return o != NULL;
}

static BOOL WINAPI ResetEvent(HANDLE h)
{
    struct w32_object *o = object(h, WH_EVENT);
    if (o)
        o->value = 0;
    return o != NULL;
}

static BOOL WINAPI PulseEvent(HANDLE h)
{
    struct w32_object *o = object(h, WH_EVENT);
    if (o) {
        o->value = 1;
        w32_yield();
        o->value = 0;
    }
    return o != NULL;
}

static BOOL WINAPI ReleaseMutex(HANDLE h)
{
    struct w32_object *o = object(h, WH_MUTEX);
    if (!o)
        return FALSE;
    if (o->owner != w32_thread_id() || !o->value) {
        w32_set_error(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->value == 0)
        o->owner = 0;
    return TRUE;
}

static BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, LONG *previous)
{
    struct w32_object *o = object(h, WH_SEMAPHORE);
    if (!o)
        return FALSE;
    if (count <= 0 || o->value + (uint32_t)count > o->max) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (previous)
        *previous = (LONG)o->value;
    o->value += (uint32_t)count;
    return TRUE;
}

static bool signaled(HANDLE h, struct w32_object *o)
{
    if (h == W32_PSEUDO_THREAD)
        return false;
    if (!o)
        return true;
    switch (o->type) {
    case WH_EVENT:
    case WH_SEMAPHORE: return o->value > 0;
    case WH_MUTEX: return !o->value || o->owner == w32_thread_id();
    case WH_THREAD: return o->thread->done;
    case WH_PROCESS: return !o->child || o->child->done;
    case WH_TIMER: return o->due && hal_ticks_ms() >= o->due;
    default: return true;
    }
}

static void acquire(struct w32_object *o)
{
    if (!o)
        return;
    if (o->type == WH_EVENT && !o->manual)
        o->value = 0;
    else if (o->type == WH_TIMER && !o->manual)
        o->due = 0;
    else if (o->type == WH_SEMAPHORE)
        o->value--;
    else if (o->type == WH_MUTEX) {
        o->owner = w32_thread_id();
        o->value++;
    }
}

static DWORD wait_objects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms)
{
    if (!count || count > 64) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    struct w32_object *objects[64];
    for (DWORD i = 0; i < count; i++) {
        objects[i] = w32_handle_get(handles[i]);
        if (!objects[i] && handles[i] != W32_PSEUDO_THREAD && handles[i] != W32_PSEUDO_PROCESS) {
            w32_set_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    con_flush();
    uint64_t deadline = ms == INFINITE ? 0 : hal_ticks_ms() + ms;
    for (;;) {
        if (all) {
            bool ready = true;
            for (DWORD i = 0; i < count && ready; i++)
                ready = signaled(handles[i], objects[i]);
            if (ready) {
                for (DWORD i = 0; i < count; i++)
                    acquire(objects[i]);
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < count; i++)
                if (signaled(handles[i], objects[i])) {
                    acquire(objects[i]);
                    return WAIT_OBJECT_0 + i;
                }
        }
        if (ms == 0 || (ms != INFINITE && hal_ticks_ms() >= deadline))
            return WAIT_TIMEOUT;
        w32_block();
    }
}

static DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms)
{
    return wait_objects(1, &h, FALSE, ms);
}

static DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    return wait_objects(1, &h, FALSE, ms);
}

static DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms)
{
    return wait_objects(count, handles, all, ms);
}

static DWORD WINAPI WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL all, DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    return wait_objects(count, handles, all, ms);
}

static DWORD WINAPI SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    struct w32_object *o = w32_handle_get(signal);
    if (o && o->type == WH_EVENT)
        o->value = 1;
    else if (o && o->type == WH_MUTEX)
        ReleaseMutex(signal);
    else if (o && o->type == WH_SEMAPHORE)
        ReleaseSemaphore(signal, 1, NULL);
    return wait_objects(1, &wait, FALSE, ms);
}

static void WINAPI InitializeCriticalSection(struct critical_section *cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->lock_count = -1;
}

static BOOL WINAPI InitializeCriticalSectionAndSpinCount(struct critical_section *cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->spin = spin;
    return TRUE;
}

static BOOL WINAPI InitializeCriticalSectionEx(struct critical_section *cs, DWORD spin, DWORD flags)
{
    UNUSED(flags);
    return InitializeCriticalSectionAndSpinCount(cs, spin);
}

static void WINAPI DeleteCriticalSection(struct critical_section *cs)
{
    UNUSED(cs);
}

static BOOL WINAPI TryEnterCriticalSection(struct critical_section *cs)
{
    DWORD me = w32_thread_id();
    if (cs->owner && cs->owner != me && cs->recursion > 0)
        return FALSE;
    cs->owner = me;
    cs->recursion++;
    cs->lock_count++;
    return TRUE;
}

static void WINAPI EnterCriticalSection(struct critical_section *cs)
{
    while (!TryEnterCriticalSection(cs))
        w32_block();
}

static void WINAPI LeaveCriticalSection(struct critical_section *cs)
{
    if (cs->owner != w32_thread_id() || cs->recursion <= 0)
        return;
    cs->lock_count--;
    if (--cs->recursion == 0)
        cs->owner = 0;
}

static void WINAPI InitializeSRWLock(uint32_t *lock)
{
    *lock = 0;
}

static uint8_t try_exclusive(uint32_t *lock)
{
    if (*lock)
        return 0;
    *lock = 1;
    return 1;
}

static uint8_t WINAPI TryAcquireSRWLockExclusive(uint32_t *lock)
{
    return try_exclusive(lock);
}

static void WINAPI AcquireSRWLockExclusive(uint32_t *lock)
{
    while (!try_exclusive(lock))
        w32_block();
}

static void WINAPI ReleaseSRWLockExclusive(uint32_t *lock)
{
    *lock = 0;
}

static uint8_t WINAPI TryAcquireSRWLockShared(uint32_t *lock)
{
    if (*lock & 1)
        return 0;
    *lock += 2;
    return 1;
}

static void WINAPI AcquireSRWLockShared(uint32_t *lock)
{
    while (!TryAcquireSRWLockShared(lock))
        w32_block();
}

static void WINAPI ReleaseSRWLockShared(uint32_t *lock)
{
    if (*lock >= 2)
        *lock -= 2;
}

static void WINAPI InitializeConditionVariable(uint32_t *cv)
{
    *cv = 0;
}

static void WINAPI WakeConditionVariable(uint32_t *cv)
{
    (*cv)++;
}

static bool wait_generation(uint32_t *cv, uint32_t generation, DWORD ms)
{
    uint64_t deadline = ms == INFINITE ? 0 : hal_ticks_ms() + ms;
    while (*cv == generation) {
        if (ms == 0 || (ms != INFINITE && hal_ticks_ms() >= deadline)) {
            w32_set_error(ERROR_TIMEOUT);
            return false;
        }
        w32_block();
    }
    return true;
}

static BOOL WINAPI SleepConditionVariableCS(uint32_t *cv, struct critical_section *cs, DWORD ms)
{
    uint32_t generation = *cv;
    LONG depth = cs->recursion;
    cs->recursion = 0;
    cs->owner = 0;
    cs->lock_count -= depth;
    bool woke = wait_generation(cv, generation, ms);
    while (cs->owner && cs->owner != w32_thread_id())
        w32_block();
    cs->owner = w32_thread_id();
    cs->recursion = depth;
    cs->lock_count += depth;
    return woke;
}

static BOOL WINAPI SleepConditionVariableSRW(uint32_t *cv, uint32_t *lock, DWORD ms, ULONG flags)
{
    uint32_t generation = *cv;
    bool shared = flags & 1;
    if (shared)
        ReleaseSRWLockShared(lock);
    else
        ReleaseSRWLockExclusive(lock);
    bool woke = wait_generation(cv, generation, ms);
    if (shared)
        AcquireSRWLockShared(lock);
    else
        AcquireSRWLockExclusive(lock);
    return woke;
}

static bool once_claim(uint32_t *once, void **context, bool check_only, bool *done)
{
    for (;;) {
        uint32_t v = __atomic_load_n(once, __ATOMIC_ACQUIRE);
        if ((v & 3) == 2) {
            if (context)
                *context = (void *)(uintptr_t)(v & ~3u);
            *done = true;
            return true;
        }
        if (check_only)
            return false;
        uint32_t expected = 0;
        if ((v & 3) == 0 && __atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            *done = false;
            return true;
        }
        w32_block();
    }
}

static void once_finish(uint32_t *once, void *context, bool ok)
{
    __atomic_store_n(once, ok ? (((uint32_t)(uintptr_t)context & ~3u) | 2) : 0, __ATOMIC_RELEASE);
}

static BOOL WINAPI InitOnceExecuteOnce(uint32_t *once, void *fn, void *param, void **context)
{
    bool done;
    once_claim(once, context, false, &done);
    if (done)
        return TRUE;
    void *ctx = NULL;
    typedef BOOL WINAPI (*init_fn)(uint32_t *, void *, void **);
    BOOL ok = ((init_fn)fn)(once, param, &ctx);
    once_finish(once, ctx, ok);
    if (ok && context)
        *context = ctx;
    return ok;
}

static BOOL WINAPI InitOnceBeginInitialize(uint32_t *once, DWORD flags, BOOL *pending, void **context)
{
    bool done;
    if (!once_claim(once, context, flags & 1, &done)) {
        w32_set_error(31);
        return FALSE;
    }
    *pending = !done;
    return TRUE;
}

static BOOL WINAPI InitOnceComplete(uint32_t *once, DWORD flags, void *context)
{
    once_finish(once, context, !(flags & 4));
    return TRUE;
}

static BOOL WINAPI WaitOnAddress(volatile void *addr, const void *compare, SIZE_T size, DWORD ms)
{
    uint64_t deadline = ms == INFINITE ? 0 : hal_ticks_ms() + ms;
    while (!memcmp((const void *)addr, compare, size)) {
        if (ms == 0 || (ms != INFINITE && hal_ticks_ms() >= deadline)) {
            w32_set_error(ERROR_TIMEOUT);
            return FALSE;
        }
        w32_block();
    }
    return TRUE;
}

static void WINAPI WakeByAddress(void *addr)
{
    UNUSED(addr);
}

static HANDLE WINAPI OpenThread(DWORD access, BOOL inherit, DWORD id)
{
    UNUSED(access);
    UNUSED(inherit);
    for (struct w32_thread *t = w32->threads; t; t = t->next) {
        if (t->id == id && !t->done) {
            HANDLE h = w32_handle_new(WH_THREAD);
            struct w32_object *o = w32_handle_get(h);
            if (o)
                o->thread = t;
            return h;
        }
        if (t->next == w32->threads)
            break;
    }
    w32_set_error(ERROR_INVALID_PARAMETER);
    return NULL;
}

static DWORD WINAPI FlsAlloc(void *callback)
{
    UNUSED(callback);
    static W32_STATE uint64_t used;
    for (DWORD i = 1; i < 64; i++)
        if (!(used & (1ull << i))) {
            used |= 1ull << i;
            for (struct w32_thread *t = w32->threads;; t = t->next) {
                t->fls[i] = NULL;
                if (t->next == w32->threads)
                    break;
            }
            return i;
        }
    return 0xffffffffu;
}

static BOOL WINAPI FlsFree(DWORD index)
{
    return index < 64;
}

static void *WINAPI FlsGetValue(DWORD index)
{
    return index < 64 ? w32->current->fls[index] : NULL;
}

static BOOL WINAPI FlsSetValue(DWORD index, void *value)
{
    if (index >= 64)
        return FALSE;
    w32->current->fls[index] = value;
    return TRUE;
}

static const struct w32_export thread_exports[] = {
    W32_FN(CreateThread), W32_FN(ExitThread), W32_FN(TerminateThread), W32_FN(GetExitCodeThread),
    W32_FN(SuspendThread), W32_FN(ResumeThread), W32_FN(GetCurrentThreadId), W32_FN(GetThreadId),
    W32_FN(SetThreadPriority), W32_FN(GetThreadPriority), W32_FN(SwitchToThread), W32_FN(Sleep), W32_FN(SleepEx),
    W32_FN(CreateEventA), W32_FN(CreateEventW), W32_FN(CreateEventExW), W32_FN(CreateMutexA), W32_FN(CreateMutexW),
    W32_FN(CreateSemaphoreA), W32_FN(CreateSemaphoreW), W32_FN(SetEvent), W32_FN(ResetEvent), W32_FN(PulseEvent),
    W32_FN(ReleaseMutex), W32_FN(ReleaseSemaphore), W32_FN(WaitForSingleObject), W32_FN(WaitForSingleObjectEx),
    W32_FN(WaitForMultipleObjects), W32_FN(WaitForMultipleObjectsEx), W32_FN(SignalObjectAndWait),
    W32_FN(InitializeCriticalSection), W32_FN(InitializeCriticalSectionAndSpinCount),
    W32_FN(InitializeCriticalSectionEx), W32_FN(DeleteCriticalSection), W32_FN(TryEnterCriticalSection),
    W32_FN(EnterCriticalSection), W32_FN(LeaveCriticalSection), W32_FN(InitializeSRWLock),
    W32_FN(TryAcquireSRWLockExclusive), W32_FN(AcquireSRWLockExclusive), W32_FN(ReleaseSRWLockExclusive),
    W32_FN(TryAcquireSRWLockShared), W32_FN(AcquireSRWLockShared), W32_FN(ReleaseSRWLockShared),
    W32_FN(InitializeConditionVariable), W32_FN(WakeConditionVariable),
    W32_AS("WakeAllConditionVariable", WakeConditionVariable), W32_FN(SleepConditionVariableCS),
    W32_FN(SleepConditionVariableSRW), W32_FN(InitOnceExecuteOnce), W32_FN(FlsAlloc), W32_FN(FlsFree),
    W32_FN(FlsGetValue), W32_FN(FlsSetValue),
    W32_AS("FlsGetValue2", FlsGetValue), W32_FN(InitOnceBeginInitialize), W32_FN(InitOnceComplete),
    W32_FN(WaitOnAddress), W32_AS("WakeByAddressAll", WakeByAddress), W32_AS("WakeByAddressSingle", WakeByAddress),
    W32_FN(OpenThread),
};

W32_EXPORTS(thread_module, "kernel32.dll", thread_exports);
