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
    /* a child's boot thread runs the program on the process stack too */
    if (!t->stack) {
        t->stack = w32->stack;
        t->stack_size = w32->stack_size;
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
#ifdef __x86_64__
    /* what arch_switch pops: rflags, r15-r12, rbx, rbp, then it returns into entry */
    uint64_t *sp = (uint64_t *)(((uintptr_t)stack_top - 64) & ~(uintptr_t)15);
    *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)entry;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    *--sp = 0x202;
    t->esp = (uint32_t)(uintptr_t)sp;
#else
    uint32_t *sp = (uint32_t *)(((uint32_t)stack_top - 64) & ~15u);
    *--sp = 0;
    *--sp = (uint32_t)entry;
    for (int i = 0; i < 4; i++)
        *--sp = 0;
    *--sp = 0x202;
    t->esp = (uint32_t)sp;
#endif
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

#define SAFETY_NET_MS 250

/* mutex owners are told apart across processes too */
static uint32_t owner_id(const struct w32_process *p, uint32_t tid)
{
    return (p && p->child ? p->child->pid : 0) << 16 | (tid & 0xffffu);
}

static uint32_t my_owner_id(void)
{
    return owner_id(w32, w32_thread_id());
}

static bool object_ready(struct w32_object *o, const struct w32_thread *t)
{
    switch (o->type) {
    case WH_EVENT:
    case WH_SEMAPHORE: return *w32_sync_value(o) > 0;
    case WH_MUTEX: return !*w32_sync_value(o) || *w32_sync_owner(o) == owner_id(t->proc, t->id);
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
    /* Sockets watched with events or overlapped reads are checked when a thread of their process polls. */
    if (t->proc->net_watch && net_activity() != t->proc->net_seen)
        return true;
    switch (w->kind) {
    case W32_WAIT_TIME: return false;
    case W32_WAIT_OBJECTS:
        for (int i = 0; i < w->count; i++)
            if (object_ready(w->objects[i], t) != w->all)
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
            if (object_ready(w->objects[i], t))
                return true;
        return t->seen != signal_gen && hal_ticks_ms() - w->since >= SAFETY_NET_MS;
    default: return t->seen != signal_gen || t->proc->signal_gen != t->seen_local;
    }
}

static bool stale(const struct w32_thread *t)
{
    return !t->done && !t->suspended && t->waiting && wait_over(t);
}

bool w32_thread_ready(const struct w32_thread *t)
{
    return !t->done && !t->suspended && (!t->waiting || wait_over(t));
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
    /* precise waits are cheap to check: every process, not only the kicked ones (a timer or socket data
       does not kick) */
    struct w32_thread *t = w32->kicked || precise_waits ? stale_in(w32, cur) : NULL;
    for (struct w32_process *p = w32_ring_next(w32); !t && p && p != w32; p = w32_ring_next(p))
        if ((p->kicked || precise_waits) && !p->dead && !p->frozen && p->current && !w32_is_host(p))
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
    uint32_t reasons[8];
    uint64_t since;
    struct {
        uint32_t site, count;
        uint8_t kind;
    } sites[16];
    struct {
        uint32_t site, caller, count;
    } late[12];
} stats;

static void stats_late(uint32_t site, uint32_t caller)
{
    int slot = 0;
    for (int i = 0; i < (int)ARRAY_SIZE(stats.late); i++) {
        if (stats.late[i].site == site && stats.late[i].caller == caller) {
            stats.late[i].count++;
            return;
        }
        if (stats.late[i].count < stats.late[slot].count)
            slot = i;
    }
    stats.late[slot].site = site;
    stats.late[slot].caller = caller;
    stats.late[slot].count = 1;
}

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
    if (next->teb64)
        arch_w64_set_teb((uint32_t)next->teb64);
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
    /* a 64-bit program's frames are no 32-bit chain: stop at anything that leaves the thread's stack */
    uint32_t lo = (uint32_t)t->stack, hi = lo + t->stack_size;
    for (int i = 0; i < 16 && (uint32_t)frame >= lo && (uint32_t)frame + 8 <= hi; i++) {
        uint32_t ret = frame[1];
        if (ret >= VMM_USER_START && ret < 0xc0000000u) {
            t->wait_sp = (uint32_t)(frame + 2);
            return ret;
        }
        if (frame[0] <= (uint32_t)frame)
            break;
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
        /* a thread whose wait just ended goes before busy ones, like the boost Windows gives it */
        if (!next && precise_waits)
            next = pick_stale(cur);
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

static BOOL WINAPI GetThreadPriorityBoost(HANDLE h, BOOL *disabled)
{
    if (disabled)
        *disabled = FALSE;
    return thread_of(h) != NULL;
}

static BOOL WINAPI SetThreadPriorityBoost(HANDLE h, BOOL disable)
{
    UNUSED(disable);
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
        /* other threads, processes and the desktop run while this one sleeps: a parent polling its child must not
           starve it */
        struct w32_process *other = w32_ring_next(w32);
        if (w32->live_threads > 1 || (other && other != w32) || w32_host_thread())
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

/* Named events, mutexes and semaphores: a kernel-wide table of names. The process that made an object finds its
   handle again by name; another process gets a handle of its own to the same object, whose state they share. */
struct named_object {
    struct named_object *next;
    uint32_t pid, serial;
    HANDLE handle;
    enum w32_object_type type;
    char name[];
};

static struct named_object *named;
static uint32_t named_serial;

static uint32_t pid_of(const struct w32_process *p)
{
    return p->child ? p->child->pid : 0x100;
}

static struct w32_object *named_object_of(const struct named_object *n)
{
    struct w32_process *p = w32;
    do {
        if (pid_of(p) == n->pid) {
            struct w32_object *o = w32_handle_get_in(p, n->handle);
            return o && o->name_serial == n->serial ? o : NULL;
        }
        p = w32_ring_next(p);
    } while (p && p != w32);
    return NULL;
}

static const char *object_name(const char *name)
{
    if (str_starts(name, "Global\\"))
        return name + 7;
    if (str_starts(name, "Local\\"))
        return name + 6;
    if (str_starts(name, "Session\\")) {
        const char *slash = strchr(name + 8, '\\');
        return slash ? slash + 1 : name;
    }
    return name;
}

/* drops the names whose handle is gone; returns the live entry for name, the current process's own first */
static struct named_object *named_find(const char *name)
{
    struct named_object *found = NULL;
    for (struct named_object **pp = &named; *pp;) {
        struct named_object *n = *pp;
        if (!named_object_of(n)) {
            *pp = n->next;
            kfree(n);
            continue;
        }
        if (!strcmp(n->name, name) && (!found || n->pid == pid_of(w32)))
            found = n;
        pp = &n->next;
    }
    return found;
}

static void named_add(const char *name, HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    struct named_object *n = o ? kmalloc(sizeof(*n) + strlen(name) + 1) : NULL;
    if (!n)
        return;
    n->pid = pid_of(w32);
    n->serial = o->name_serial = ++named_serial;
    n->handle = h;
    n->type = o->type;
    memcpy(n->name, name, strlen(name) + 1);
    n->next = named;
    named = n;
}

/* NULL with *h set: a new object is to be made; otherwise the existing one (or a failure) */
static bool named_open(const char *name, enum w32_object_type type, HANDLE *h)
{
    *h = NULL;
    if (!name || !*name)
        return false;
    struct named_object *n = named_find(object_name(name));
    if (!n)
        return false;
    if (n->type != type) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return true;
    }
    if (n->pid != pid_of(w32)) {
        struct w32_object *src = named_object_of(n);
        if (!src || !(*h = w32_handle_put(w32, src)))
            return false;
        named_add(n->name, *h);
        return true;
    }
    w32_handle_get(n->handle)->refs++;
    *h = n->handle;
    return true;
}

static HANDLE create_named(const char *name, enum w32_object_type type, uint32_t value, uint32_t max, bool manual,
                           const void *sa, bool *existed)
{
    HANDLE h;
    bool exists = name && *name && named_find(object_name(name));
    if (existed)
        *existed = exists;
    if (named_open(name, type, &h)) {
        if (h)
            w32_set_error(ERROR_ALREADY_EXISTS);
        return h;
    }
    h = w32_inheritable(new_object(type, value, max, manual), sa);
    if (h && name && *name)
        named_add(object_name(name), h);
    if (h)
        w32_set_error(exists ? ERROR_ALREADY_EXISTS : 0);
    return h;
}

static HANDLE create_named_w(LPCWSTR name, enum w32_object_type type, uint32_t value, uint32_t max, bool manual,
                             const void *sa, bool *existed)
{
    char *n = w32_narrow(name);
    HANDLE h = create_named(n, type, value, max, manual, sa, existed);
    w32_free(n);
    return h;
}

static HANDLE open_named(const char *name, enum w32_object_type type, BOOL inherit)
{
    HANDLE h;
    struct named_object *n = name ? named_find(object_name(name)) : NULL;
    if (!n) {
        w32_set_error(name ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (n->type != type) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    if (named_open(name, type, &h))
        return h;
    struct w32_object *src = named_object_of(n);
    h = w32_handle_put(w32, src);
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->inherit = inherit != 0;
    if (h)
        named_add(object_name(name), h);
    return h;
}

static HANDLE open_named_w(LPCWSTR name, enum w32_object_type type, BOOL inherit)
{
    char *n = w32_narrow(name);
    HANDLE h = open_named(n, type, inherit);
    w32_free(n);
    return h;
}

static void own_mutex(HANDLE h, BOOL owner, bool existed)
{
    struct w32_object *o = w32_handle_get(h);
    if (o && owner && !existed) {
        *w32_sync_value(o) = 1;
        *w32_sync_owner(o) = my_owner_id();
    }
}

static HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, LPCSTR name)
{
    return create_named(name, WH_EVENT, initial ? 1 : 0, 1, manual, sa, NULL);
}

static HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    return create_named_w(name, WH_EVENT, initial ? 1 : 0, 1, manual, sa, NULL);
}

static HANDLE WINAPI CreateEventExW(void *sa, LPCWSTR name, DWORD flags, DWORD access)
{
    UNUSED(access);
    return create_named_w(name, WH_EVENT, (flags & 2) != 0, 1, (flags & 1) != 0, sa, NULL);
}

static HANDLE WINAPI CreateMutexA(void *sa, BOOL owner, LPCSTR name)
{
    bool existed;
    HANDLE h = create_named(name, WH_MUTEX, 0, 0, false, sa, &existed);
    own_mutex(h, owner, existed);
    return h;
}

static HANDLE WINAPI CreateMutexW(void *sa, BOOL owner, LPCWSTR name)
{
    bool existed;
    HANDLE h = create_named_w(name, WH_MUTEX, 0, 0, false, sa, &existed);
    own_mutex(h, owner, existed);
    return h;
}

static HANDLE WINAPI CreateMutexExW(void *sa, LPCWSTR name, DWORD flags, DWORD access)
{
    UNUSED(access);
    return CreateMutexW(sa, (flags & 1) != 0, name);
}

static HANDLE WINAPI OpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{
    UNUSED(access);
    return open_named(name, WH_EVENT, inherit);
}

static HANDLE WINAPI OpenEventW(DWORD access, BOOL inherit, LPCWSTR name)
{
    UNUSED(access);
    return open_named_w(name, WH_EVENT, inherit);
}

static HANDLE WINAPI OpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{
    UNUSED(access);
    return open_named(name, WH_MUTEX, inherit);
}

static HANDLE WINAPI OpenMutexW(DWORD access, BOOL inherit, LPCWSTR name)
{
    UNUSED(access);
    return open_named_w(name, WH_MUTEX, inherit);
}

static HANDLE WINAPI OpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name)
{
    UNUSED(access);
    return open_named(name, WH_SEMAPHORE, inherit);
}

static HANDLE WINAPI OpenSemaphoreW(DWORD access, BOOL inherit, LPCWSTR name)
{
    UNUSED(access);
    return open_named_w(name, WH_SEMAPHORE, inherit);
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
    if (initial < 0 || max <= 0 || initial > max) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return create_named(name, WH_SEMAPHORE, (uint32_t)initial, (uint32_t)max, false, sa, NULL);
}

static HANDLE WINAPI CreateSemaphoreW(void *sa, LONG initial, LONG max, LPCWSTR name)
{
    if (initial < 0 || max <= 0 || initial > max) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return create_named_w(name, WH_SEMAPHORE, (uint32_t)initial, (uint32_t)max, false, sa, NULL);
}

static HANDLE WINAPI CreateSemaphoreExW(void *sa, LONG initial, LONG max, LPCWSTR name, DWORD flags, DWORD access)
{
    UNUSED(flags);
    UNUSED(access);
    return CreateSemaphoreW(sa, initial, max, name);
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
        *w32_sync_value(o) = 1;
    w32_signal();
    return o != NULL;
}

static BOOL WINAPI ResetEvent(HANDLE h)
{
    struct w32_object *o = object(h, WH_EVENT);
    if (o)
        *w32_sync_value(o) = 0;
    return o != NULL;
}

static BOOL WINAPI PulseEvent(HANDLE h)
{
    struct w32_object *o = object(h, WH_EVENT);
    if (o) {
        *w32_sync_value(o) = 1;
        w32_signal();
        w32_yield();
        *w32_sync_value(o) = 0;
    }
    return o != NULL;
}

static BOOL WINAPI ReleaseMutex(HANDLE h)
{
    struct w32_object *o = object(h, WH_MUTEX);
    if (!o)
        return FALSE;
    if (*w32_sync_owner(o) != my_owner_id() || !*w32_sync_value(o)) {
        w32_set_error(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--*w32_sync_value(o) == 0)
        *w32_sync_owner(o) = 0;
    w32_signal();
    return TRUE;
}

static BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, LONG *previous)
{
    struct w32_object *o = object(h, WH_SEMAPHORE);
    if (!o)
        return FALSE;
    uint32_t *value = w32_sync_value(o);
    if (count <= 0 || *value + (uint32_t)count > o->max) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (previous)
        *previous = (LONG)*value;
    *value += (uint32_t)count;
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
    case WH_SEMAPHORE: return *w32_sync_value(o) > 0;
    case WH_MUTEX: return !*w32_sync_value(o) || *w32_sync_owner(o) == my_owner_id();
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
        *w32_sync_value(o) = 0;
    else if (o->type == WH_TIMER && !o->manual)
        o->due = 0;
    else if (o->type == WH_SEMAPHORE)
        --*w32_sync_value(o);
    else if (o->type == WH_MUTEX) {
        *w32_sync_owner(o) = my_owner_id();
        ++*w32_sync_value(o);
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
        *w32_sync_value(o) = 1;
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
    uint32_t site = w->site = (uint32_t)(uintptr_t)__builtin_return_address(0);
    w32_block();
    struct w32_thread *t = w32->current;
    int reason = w->deadline && hal_ticks_ms() >= w->deadline ? 0 : w32->kick_gen != w->seen_kick ? 1
                 : (net && net_activity() != w->seen_net) || (w32->net_watch && net_activity() != w32->net_seen) ? 2
                 : w->ready && w->ready(w->arg) ? 3 : 4;
    if (reason == 4)
        for (int i = 0; i < w->count; i++)
            if (object_ready(w->objects[i], t))
                reason = 5;
    if (reason == 4 && w32->io_gen != w32->io_seen)
        reason = 6;
    else if (reason == 4 && t->seen != signal_gen && hal_ticks_ms() - w->since >= SAFETY_NET_MS)
        reason = 7;
    stats.reasons[reason]++;
    t->late_site = reason == 7 ? site : 0;
    t->late_caller = t->wait_caller;
    w->kind = W32_WAIT_ANY;
    w->deadline = 0;
    w->count = 0;
    w->ready = NULL;
}

/* The wait a thread just left got what it waited for: if only the safety net woke it, a wake-up was missed. */
void w32_wait_done(void)
{
    struct w32_thread *t = w32 ? w32->current : NULL;
    if (t && t->late_site) {
        stats_late(t->late_site, t->late_caller);
        t->late_site = 0;
    }
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
    w32->current->wait.on = addr;
    while (!memcmp((const void *)addr, compare, size)) {
        if (ms == 0 || (ms != INFINITE && hal_ticks_ms() >= deadline)) {
            w32->current->wait.on = NULL;
            w32_set_error(ERROR_TIMEOUT);
            return FALSE;
        }
        if (size == 4 && !((uintptr_t)addr & 3))
            w32_wait_word(addr, ~0u, *(const uint32_t *)compare, false, deadline);
        else
            w32_block();
    }
    w32->current->wait.on = NULL;
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
        else
            w32_wait_done();
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
    w32_kick(w32);
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
    w32_kick(w32);
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
    W32_FN(SetThreadPriority), W32_FN(GetThreadPriority), W32_FN(GetThreadPriorityBoost),
    W32_FN(SetThreadPriorityBoost), W32_FN(SwitchToThread), W32_FN(Sleep), W32_FN(SleepEx),
    W32_FN(CreateEventA), W32_FN(CreateEventW), W32_FN(CreateEventExW), W32_FN(CreateMutexA), W32_FN(CreateMutexW),
    W32_FN(CreateMutexExW), W32_FN(OpenMutexA), W32_FN(OpenMutexW), W32_FN(OpenEventA), W32_FN(OpenEventW),
    W32_FN(OpenSemaphoreA), W32_FN(OpenSemaphoreW), W32_FN(QueueUserAPC), W32_FN(CreateSemaphoreA),
    W32_FN(CreateSemaphoreW), W32_FN(CreateSemaphoreExW), W32_FN(SetEvent), W32_FN(ResetEvent), W32_FN(PulseEvent),
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
        for (int i = 0; i < (int)ARRAY_SIZE(stats.late); i++)
            if (stats.late[i].count)
                klog("sched: missed wake %08x from %08x: %u", stats.late[i].site, stats.late[i].caller,
                     stats.late[i].count);
        klog("sched: woke by deadline %u, process %u, network %u, ready %u, other %u, objects %u, io %u, late %u",
             stats.reasons[0], stats.reasons[1], stats.reasons[2], stats.reasons[3], stats.reasons[4], stats.reasons[5],
             stats.reasons[6], stats.reasons[7]);
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
