#include <synth/command.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/log.h>
#include <synth/net.h>
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

/* Bumped whenever something a waiting thread might be waiting for happens. */
static uint32_t signal_gen;

static struct {
    uint32_t site, count;
} kick_sites[8];

static void kick_note(uint32_t site)
{
    int slot = 0;
    for (int i = 0; i < (int)ARRAY_SIZE(kick_sites); i++) {
        if (kick_sites[i].site == site) {
            kick_sites[i].count++;
            return;
        }
        if (kick_sites[i].count < kick_sites[slot].count)
            slot = i;
    }
    kick_sites[slot].site = site;
    kick_sites[slot].count = 1;
}

void w32_signal(void)
{
    signal_gen++;
    if (w32) {
        w32->kicked = true;
        w32->signal_gen++;
    }
}

/* Something for the process from outside its own locks and events: pipe data, desktop events, its messages. */
void w32_kick(struct w32_process *p)
{
    kick_note((uint32_t)(uintptr_t)__builtin_return_address(0));
    signal_gen++;
    if (p) {
        p->kicked = true;
        p->signal_gen++;
        p->kick_gen++;
    }
}

/* The periodic nudge for waits that poll; precise waits do not need it. */
void w32_signal_tick(void)
{
    signal_gen++;
}

void w32_kick_all(void)
{
    kick_note((uint32_t)(uintptr_t)__builtin_return_address(0));
    signal_gen++;
    struct w32_process *p = w32;
    do {
        if (p) {
            p->kicked = true;
            p->signal_gen++;
            p->kick_gen++;
            p->io_gen++;
        }
    } while ((p = w32_ring_next(p)) && p != w32);
}

/* Locks, condition variables and WaitOnAddress live in one process's memory: only its own threads need to look. */
static bool precise_waits = true;

void w32_signal_local(void)
{
    if (!precise_waits)
        signal_gen++;
    if (w32) {
        w32->signal_gen++;
        w32->kicked = true;
    }
}

#define SAFETY_NET_MS 20

static bool object_ready(const struct w32_object *o, uint32_t tid)
{
    switch (o->type) {
    case WH_EVENT:
    case WH_SEMAPHORE: return o->value > 0;
    case WH_MUTEX: return !o->value || o->owner == tid;
    case WH_THREAD: return o->thread->done;
    case WH_PROCESS: return !o->child || o->child->done;
    case WH_TIMER: return o->due && hal_ticks_ms() >= o->due;
    default: return true;
    }
}

static bool wait_over(const struct w32_thread *t)
{
    const struct w32_wait *w = &t->wait;
    if (!precise_waits)
        return t->seen != signal_gen;
    if (w->deadline && hal_ticks_ms() >= w->deadline)
        return true;
    /* Pending pipe I/O completes when a thread of its process polls: any one of them will do. */
    if (t->proc->io_gen != t->proc->io_seen)
        return true;
    switch (w->kind) {
    case W32_WAIT_TIME: return false;
    case W32_WAIT_OBJECTS:
        for (int i = 0; i < w->count; i++)
            if (object_ready(w->objects[i], t->id) != w->all)
                return !w->all;
        return w->all;
    case W32_WAIT_WORD:
        if (t->proc == w32)
            return ((*w->addr & w->mask) == w->value) == w->equal;
        return t->proc->signal_gen != t->seen_local;
    case W32_WAIT_PROCESS:
        /* Signals for the process, network data, or what the caller can check from anywhere; anything else that
           happened elsewhere is looked at now and then. */
        if (t->proc->kick_gen != w->seen_kick || (w->net && net_activity() != w->seen_net) ||
            (w->ready && w->ready(w->arg)))
            return true;
        for (int i = 0; i < w->count; i++)
            if (object_ready(w->objects[i], t->id))
                return true;
        return t->seen != signal_gen && hal_ticks_ms() - w->since >= SAFETY_NET_MS;
    default: return t->seen != signal_gen || t->proc->signal_gen != t->seen_local;
    }
}

static bool stale(const struct w32_thread *t)
{
    return !t->done && !t->suspended && t->waiting && wait_over(t);
}

/* A waiting thread, here or in another process, that has not looked since the last signal. */
static struct w32_thread *pick_stale(struct w32_thread *cur)
{
    for (struct w32_thread *t = cur->next; t != cur; t = t->next)
        if (stale(t))
            return t;
    for (struct w32_process *p = w32_ring_next(w32); p && p != w32; p = w32_ring_next(p)) {
        if (p->dead || p->frozen || !p->current || w32_is_host(p))
            continue;
        struct w32_thread *t = p->current;
        do {
            if (stale(t))
                return t;
            t = t->next;
        } while (t != p->current);
    }
    return NULL;
}

/* Busy threads would starve waiters that were signaled: preemption starts a short pass over kicked processes. */
static int sweep_left;

static struct w32_thread *stale_in(struct w32_process *p, struct w32_thread *from)
{
    struct w32_thread *t = from;
    do {
        t = t->next;
        if (stale(t))
            return t;
    } while (t != from);
    p->kicked = false;
    return NULL;
}

static struct w32_thread *sweep_next(struct w32_thread *cur)
{
    if (sweep_left <= 0)
        return NULL;
    struct w32_thread *t = w32->kicked ? stale_in(w32, cur) : NULL;
    for (struct w32_process *p = w32_ring_next(w32); !t && p && p != w32; p = w32_ring_next(p))
        if (p->kicked && !p->dead && !p->frozen && p->current && !w32_is_host(p))
            t = stale_in(p, p->current);
    sweep_left = t ? sweep_left - 1 : 0;
    return t;
}

static struct w32_thread *pick_other(bool active_only)
{
    for (struct w32_process *p = w32_ring_next(w32); p && p != w32; p = w32_ring_next(p)) {
        if (p->dead || p->frozen || !p->current || w32_is_host(p))
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

static struct {
    uint32_t switches, process_switches, blocks, idles;
    uint32_t reasons[6];
    uint64_t since;
    struct {
        uint32_t site, count;
        uint8_t kind;
    } sites[16];
} stats;

static void stats_site(uint32_t site, uint8_t kind)
{
    int slot = 0;
    for (int i = 0; i < (int)ARRAY_SIZE(stats.sites); i++) {
        if (stats.sites[i].site == site && stats.sites[i].kind == kind) {
            stats.sites[i].count++;
            return;
        }
        if (stats.sites[i].count < stats.sites[slot].count)
            slot = i;
    }
    stats.sites[slot].site = site;
    stats.sites[slot].kind = kind;
    stats.sites[slot].count = 1;
}

static void stats_tick(void);

static void switch_to(struct w32_thread *next)
{
    struct w32_thread *cur = w32->current;
    if (next == cur)
        return;
    stats.switches++;
    if (next->proc && next->proc != w32)
        stats.process_switches++;
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
        next = w32_host_thread();
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
    if (!next)
        next = pick_stale(w32->current);
    if (next)
        switch_to(next);
}

static uint32_t user_caller(struct w32_thread *t)
{
    const uint32_t *frame = __builtin_frame_address(0);
    for (int i = 0; i < 16 && frame && (uint32_t)frame >= 0xc0000000u; i++) {
        uint32_t ret = frame[1];
        if (ret >= VMM_USER_START && ret < 0xc0000000u) {
            t->wait_sp = (uint32_t)(frame + 2);
            return ret;
        }
        frame = (const uint32_t *)frame[0];
    }
    return 0;
}

void w32_block(void)
{
    static uint64_t polled, net_polled;
    uint64_t now = hal_ticks_ms();
    if (now - polled >= 20) {
        polled = now;
        input_poll();
    }
    if (now != net_polled) {
        net_polled = now;
        net_poll();
    }
    w32_io_poll();
    if (input_info_take())
        w32_dump_status();
    stats.blocks++;
    if (w32->current) {
        struct w32_wait *w = &w32->current->wait;
        stats_site(w->site ? w->site : (uint32_t)(uintptr_t)__builtin_return_address(0), w->kind);
        w->site = 0;
    }
    stats_tick();
    struct w32_thread *cur = w32->current;
    if (!cur) {
        w32_idle(1);
    } else {
        cur->wait_caller = user_caller(cur);
        cur->waiting = true;
        cur->seen = signal_gen;
        cur->seen_local = cur->proc->signal_gen;
        struct w32_thread *next = sweep_next(cur);
        if (!next && w32->live_threads > 1)
            next = pick(cur, true);
        if (!next)
            next = pick_other(true);
        if (!next)
            next = pick_stale(cur);
        if (!next)
            next = w32_host_thread();
        if (!next) {
            stats.idles++;
            net_poll();
            w32_idle(1);
            signal_gen++;
            next = pick_stale(cur);
        }
        if (next)
            switch_to(next);
        cur->waiting = false;
    }
    if (sh_interrupted() && !w32->detached)
        w32_exit(STATUS_CONTROL_C_EXIT);
}

/* The desktop is the most interactive thing on the machine: it runs at least every 30 ms, and at once when a key or
   a pointer movement waits, so busy programs never make the screen feel stuck. */
static uint64_t host_ran;

void w32_input_boost(void)
{
    struct w32_thread *h = w32_host_thread();
    uint64_t now = hal_ticks_ms();
    if (!h || !w32 || !w32->current || w32->current == h || now - host_ran < 15)
        return;
    host_ran = now;
    switch_to(h);
}

void w32_preempt(void)
{
    static unsigned turn;
    if (!w32 || !w32->current)
        return;
    sweep_left = 64;
    struct w32_thread *next = sweep_next(w32->current);
    if (!next && ++turn % 2 == 0)
        next = pick_other(true);
    if (!next && w32->live_threads > 1) {
        next = pick(w32->current, true);
        if (!next)
            next = pick(w32->current, false);
    }
    if (!next)
        next = pick_other(true);
    uint64_t now = hal_ticks_ms();
    if (w32_host_thread() && (now - host_ran >= 30 || hal_input_pending())) {
        next = w32_host_thread();
        host_ran = now;
    }
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
    w32_signal();
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
    v = w32_current_pid();
    memcpy(teb + 0x20, &v, 4);
    memcpy(teb + 0x24, &t->id, 4);
    memcpy(teb + 0x30, w32->teb + 0x30, 4);
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

static HANDLE WINAPI OpenNamedObject(DWORD access, BOOL inherit, LPCWSTR name)
{
    UNUSED(access);
    UNUSED(inherit);
    UNUSED(name);
    w32_set_error(ERROR_FILE_NOT_FOUND);
    return NULL;
}

static DWORD WINAPI QueueUserAPC(void *fn, HANDLE thread, uintptr_t data)
{
    UNUSED(fn);
    UNUSED(thread);
    UNUSED(data);
    w32_set_error(ERROR_NOT_SUPPORTED);
    return 0;
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
    w32_signal();
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
        w32_signal();
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
    w32_signal();
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
    w32_signal();
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
        struct w32_wait *w = &w32->current->wait;
        if (count <= ARRAY_SIZE(w->objects)) {
            w->kind = W32_WAIT_OBJECTS;
            for (DWORD i = 0; i < count && w->kind; i++)
                if (!(w->objects[i] = objects[i]) || handles[i] == W32_PSEUDO_THREAD)
                    w->kind = W32_WAIT_ANY;
            w->count = (uint8_t)count;
            w->all = all;
            w->deadline = deadline;
        }
        w32_block();
        w->kind = W32_WAIT_ANY;
        w->deadline = 0;
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
    DWORD me = w32_thread_id(), free = 0;
    if (__atomic_load_n(&cs->owner, __ATOMIC_ACQUIRE) != me &&
        !__atomic_compare_exchange_n(&cs->owner, &free, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return FALSE;
    cs->recursion++;
    __atomic_add_fetch(&cs->lock_count, 1, __ATOMIC_RELAXED);
    return TRUE;
}

void w32_wait_process(uint64_t deadline, bool net, bool (*ready)(const void *), const void *arg)
{
    struct w32_wait *w = &w32->current->wait;
    if (w->kind != W32_WAIT_OBJECTS)
        w->count = 0;
    w->kind = W32_WAIT_PROCESS;
    w->deadline = deadline;
    w->net = net;
    w->seen_net = net_activity();
    w->seen_kick = w32->kick_gen;
    w->ready = ready;
    w->arg = arg;
    w->since = hal_ticks_ms();
    w->site = (uint32_t)(uintptr_t)__builtin_return_address(0);
    w32_block();
    struct w32_thread *t = w32->current;
    int reason = w->deadline && hal_ticks_ms() >= w->deadline ? 0 : w32->kick_gen != w->seen_kick ? 1
                 : net && net_activity() != w->seen_net ? 2 : w->ready && w->ready(w->arg) ? 3 : 4;
    if (reason == 4)
        for (int i = 0; i < w->count; i++)
            if (object_ready(w->objects[i], t->id))
                reason = 5;
    stats.reasons[reason]++;
    w->kind = W32_WAIT_ANY;
    w->deadline = 0;
    w->count = 0;
    w->ready = NULL;
}

void w32_wait_word(const volatile uint32_t *addr, uint32_t mask, uint32_t value, bool equal, uint64_t deadline)
{
    struct w32_wait *w = &w32->current->wait;
    w->kind = W32_WAIT_WORD;
    w->addr = addr;
    w->mask = mask;
    w->value = value;
    w->equal = equal;
    w->deadline = deadline;
    w->site = (uint32_t)(uintptr_t)__builtin_return_address(0);
    w32_block();
    w->kind = W32_WAIT_ANY;
    w->deadline = 0;
}

static void WINAPI EnterCriticalSection(struct critical_section *cs)
{
    while (!TryEnterCriticalSection(cs))
        w32_wait_word((const volatile uint32_t *)&cs->owner, ~0u, 0, true, 0);
}

static void WINAPI LeaveCriticalSection(struct critical_section *cs)
{
    if (cs->owner != w32_thread_id() || cs->recursion <= 0)
        return;
    __atomic_sub_fetch(&cs->lock_count, 1, __ATOMIC_RELAXED);
    if (--cs->recursion == 0) {
        __atomic_store_n(&cs->owner, 0, __ATOMIC_RELEASE);
        w32_signal_local();
    }
}

static void WINAPI InitializeSRWLock(uint32_t *lock)
{
    *lock = 0;
}

/* bit 0: held exclusively; the rest counts shared holders in steps of 2 */
static uint8_t WINAPI TryAcquireSRWLockExclusive(uint32_t *lock)
{
    uint32_t free = 0;
    return __atomic_compare_exchange_n(lock, &free, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void WINAPI AcquireSRWLockExclusive(uint32_t *lock)
{
    while (!TryAcquireSRWLockExclusive(lock))
        w32_wait_word(lock, ~0u, 0, true, 0);
}

static void WINAPI ReleaseSRWLockExclusive(uint32_t *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    w32_signal_local();
}

static uint8_t WINAPI TryAcquireSRWLockShared(uint32_t *lock)
{
    uint32_t v = __atomic_load_n(lock, __ATOMIC_RELAXED);
    while (!(v & 1))
        if (__atomic_compare_exchange_n(lock, &v, v + 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
    return 0;
}

static void WINAPI AcquireSRWLockShared(uint32_t *lock)
{
    while (!TryAcquireSRWLockShared(lock))
        w32_wait_word(lock, 1, 0, true, 0);
}

static void WINAPI ReleaseSRWLockShared(uint32_t *lock)
{
    uint32_t v = __atomic_load_n(lock, __ATOMIC_RELAXED);
    while (v >= 2 && !__atomic_compare_exchange_n(lock, &v, v - 2, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        ;
    w32_signal_local();
}

static void WINAPI InitializeConditionVariable(uint32_t *cv)
{
    *cv = 0;
}

static void WINAPI WakeConditionVariable(uint32_t *cv)
{
    __atomic_add_fetch(cv, 1, __ATOMIC_RELEASE);
    w32_signal_local();
}

static bool wait_generation(uint32_t *cv, uint32_t generation, DWORD ms)
{
    uint64_t deadline = ms == INFINITE ? 0 : hal_ticks_ms() + ms;
    while (__atomic_load_n(cv, __ATOMIC_ACQUIRE) == generation) {
        if (ms == 0 || (ms != INFINITE && hal_ticks_ms() >= deadline)) {
            w32_set_error(ERROR_TIMEOUT);
            return false;
        }
        w32_wait_word(cv, ~0u, generation, false, deadline);
    }
    return true;
}

static BOOL WINAPI SleepConditionVariableCS(uint32_t *cv, struct critical_section *cs, DWORD ms)
{
    uint32_t generation = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
    LONG depth = cs->recursion;
    cs->recursion = 0;
    __atomic_sub_fetch(&cs->lock_count, depth, __ATOMIC_RELAXED);
    __atomic_store_n(&cs->owner, 0, __ATOMIC_RELEASE);
    w32_signal_local();
    bool woke = wait_generation(cv, generation, ms);
    DWORD me = w32_thread_id(), free = 0;
    while (!__atomic_compare_exchange_n(&cs->owner, &free, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        free = 0;
        w32_wait_word((const volatile uint32_t *)&cs->owner, ~0u, 0, true, 0);
    }
    cs->recursion = depth;
    __atomic_add_fetch(&cs->lock_count, depth, __ATOMIC_RELAXED);
    return woke;
}

static BOOL WINAPI SleepConditionVariableSRW(uint32_t *cv, uint32_t *lock, DWORD ms, ULONG flags)
{
    uint32_t generation = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
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
        w32_wait_word(once, 3, 1, false, 0);
    }
}

static void once_finish(uint32_t *once, void *context, bool ok)
{
    __atomic_store_n(once, ok ? (((uint32_t)(uintptr_t)context & ~3u) | 2) : 0, __ATOMIC_RELEASE);
    w32_signal_local();
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
        if (size == 4 && !((uintptr_t)addr & 3))
            w32_wait_word(addr, ~0u, *(const uint32_t *)compare, false, deadline);
        else
            w32_block();
    }
    return TRUE;
}

static void WINAPI WakeByAddress(void *addr)
{
    w32_signal_local();
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

/* RegisterWaitForSingleObject: one wait thread per process polls every registration when woken. */
struct registered_wait {
    HANDLE object, done_event;
    void(WINAPI *callback)(void *, uint8_t);
    void *context;
    DWORD ms, flags;
    uint64_t deadline;
    bool running, cancelled, spent;
    struct registered_wait *next;
};

static W32_STATE struct registered_wait *reg_waits;
static W32_STATE DWORD reg_thread;

static void reg_finish(struct registered_wait *w)
{
    if (w->done_event && w->done_event != INVALID_HANDLE_VALUE)
        SetEvent(w->done_event);
    w32_free(w);
}

static bool reg_fire_one(void)
{
    uint64_t now = hal_ticks_ms();
    for (struct registered_wait **pp = &reg_waits; *pp; pp = &(*pp)->next) {
        struct registered_wait *w = *pp;
        if (w->cancelled) {
            *pp = w->next;
            reg_finish(w);
            return true;
        }
        if (w->spent)
            continue;
        struct w32_object *o = w32_handle_get(w->object);
        bool ready = signaled(w->object, o), timeout = w->ms != INFINITE && now >= w->deadline;
        if (!ready && !timeout)
            continue;
        if (ready)
            acquire(o);
        w->spent = (w->flags & 8) != 0;
        w->deadline = now + w->ms;
        w->running = true;
        w->callback(w->context, !ready);
        w->running = false;
        w32_signal();
        return true;
    }
    return false;
}

static void reg_wait(void)
{
    struct w32_wait *w = &w32->current->wait;
    uint64_t deadline = 0;
    w->count = 0;
    for (struct registered_wait *r = reg_waits; r; r = r->next) {
        if (r->spent)
            continue;
        struct w32_object *o = w32_handle_get(r->object);
        if (!o || w->count == ARRAY_SIZE(w->objects)) {
            w32_block();
            return;
        }
        w->objects[w->count++] = o;
        if (r->ms != INFINITE && (!deadline || r->deadline < deadline))
            deadline = MAX(r->deadline, (uint64_t)1);
    }
    w->kind = W32_WAIT_OBJECTS;
    w32_wait_process(deadline, false, NULL, NULL);
}

static DWORD WINAPI reg_wait_thread(void *arg)
{
    UNUSED(arg);
    for (;;)
        if (!reg_fire_one())
            reg_wait();
    return 0;
}

static BOOL WINAPI RegisterWaitForSingleObject(HANDLE *out, HANDLE object, void *callback, void *context, DWORD ms,
                                               DWORD flags)
{
    if (!out || !callback || !w32_handle_get(object)) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!reg_thread) {
        HANDLE t = w32_create_thread(0, (void *)reg_wait_thread, NULL, 0, &reg_thread);
        if (!t)
            return FALSE;
        w32_handle_close(t);
    }
    struct registered_wait *w = w32_zalloc(sizeof(*w));
    if (!w) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    w->object = object;
    w->callback = (void(WINAPI *)(void *, uint8_t))callback;
    w->context = context;
    w->ms = ms;
    w->flags = flags;
    w->deadline = ms == INFINITE ? 0 : hal_ticks_ms() + ms;
    w->next = reg_waits;
    reg_waits = w;
    w32_signal();
    *out = (HANDLE)w;
    return TRUE;
}

static BOOL WINAPI UnregisterWaitEx(HANDLE handle, HANDLE completion)
{
    struct registered_wait *w = reg_waits;
    while (w && w != (struct registered_wait *)handle)
        w = w->next;
    if (!w || w->cancelled) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    w->cancelled = true;
    w->done_event = completion;
    w32_signal();
    if (completion == INVALID_HANDLE_VALUE && w32_thread_id() != reg_thread)
        while (w->running)
            w32_block();
    return TRUE;
}

static BOOL WINAPI UnregisterWait(HANDLE handle)
{
    return UnregisterWaitEx(handle, NULL);
}

static const struct w32_export thread_exports[] = {
    W32_FN(RegisterWaitForSingleObject), W32_FN(UnregisterWaitEx), W32_FN(UnregisterWait),
    W32_FN(CreateThread), W32_FN(ExitThread), W32_FN(TerminateThread), W32_FN(GetExitCodeThread),
    W32_FN(SuspendThread), W32_FN(ResumeThread), W32_FN(GetCurrentThreadId), W32_FN(GetThreadId),
    W32_FN(SetThreadPriority), W32_FN(GetThreadPriority), W32_FN(SwitchToThread), W32_FN(Sleep), W32_FN(SleepEx),
    W32_FN(CreateEventA), W32_FN(CreateEventW), W32_FN(CreateEventExW), W32_FN(CreateMutexA), W32_FN(CreateMutexW),
    W32_AS("OpenMutexW", OpenNamedObject), W32_AS("OpenMutexA", OpenNamedObject), W32_AS("OpenEventW", OpenNamedObject),
    W32_AS("OpenEventA", OpenNamedObject), W32_AS("OpenSemaphoreW", OpenNamedObject), W32_FN(QueueUserAPC),
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

static uint32_t stats_every;

static void stats_take(char *buf, size_t cap)
{
    uint64_t now = hal_ticks_ms(), ms = now - stats.since ? now - stats.since : 1;
    ksnprintf(buf, cap, "%u ms: %u switches (%u between processes, %u/s), %u waits (%u/s), %u idle ms",
              (unsigned)ms, stats.switches, stats.process_switches, (unsigned)(stats.switches * 1000ull / ms),
              stats.blocks, (unsigned)(stats.blocks * 1000ull / ms), stats.idles);
    memset(&stats, 0, sizeof(stats));
    stats.since = now;
}

static void stats_tick(void)
{
    if (stats_every && hal_ticks_ms() - stats.since >= stats_every) {
        for (int i = 0; i < (int)ARRAY_SIZE(stats.sites); i++)
            if (stats.sites[i].count > 50)
                klog("sched: site %08x kind %u: %u", stats.sites[i].site, stats.sites[i].kind, stats.sites[i].count);
        for (int i = 0; i < (int)ARRAY_SIZE(kick_sites); i++)
            if (kick_sites[i].count > 50)
                klog("sched: kick from %08x: %u", kick_sites[i].site, kick_sites[i].count);
        memset(kick_sites, 0, sizeof(kick_sites));
        klog("sched: woke by deadline %u, process %u, network %u, ready %u, other %u, objects %u", stats.reasons[0],
             stats.reasons[1], stats.reasons[2], stats.reasons[3], stats.reasons[4], stats.reasons[5]);
        char line[160];
        stats_take(line, sizeof(line));
        klog("sched: %s", line);
    }
}

COMMAND(schedstat, NULL, "dev", "schedstat [seconds]",
        "Windows thread switches and waits since the last call; with seconds, log them that often")
{
    if (argc > 1) {
        long seconds;
        if (!str_parse_long(argv[1], &seconds) || seconds < 0)
            return command_usage(sh, command_find("schedstat"));
        stats_every = (uint32_t)seconds * 1000u;
        return 0;
    }
    char line[160];
    stats_take(line, sizeof(line));
    sh_printf(sh, "%s\n", line);
    return 0;
}

static void get_precise(char *buf, size_t cap)
{
    str_copy(buf, precise_waits ? "on" : "off", cap);
}

static int set_precise(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    precise_waits = !strcmp(value, "on");
    return 0;
}

static const struct setting s_precise = {"w32precise", "on|off",
                                         "wake a waiting Windows thread only when what it waits for happened",
                                         get_precise, set_precise};
SETTING(s_precise);
