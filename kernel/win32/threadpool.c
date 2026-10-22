#include <synth/string.h>

#include "win32.h"

/* The thread pool API of Windows Vista: work items run on a fresh thread each, timers are waitable timers and waits
   are registered waits whose callbacks call the program's. */

#define INFINITE 0xffffffffu
#define WT_EXECUTEONLYONCE 8u

enum tp_kind {
    TP_WORK,
    TP_TIMER,
    TP_WAIT,
    TP_SIMPLE,
};

struct tp_item {
    enum tp_kind kind;
    void *callback;
    void *context;
    HANDLE timer, wait, object;
    volatile LONG running;
    bool set, closing;
};

static void *k32(const char *name)
{
    return w32_builtin_lookup("kernel32.dll", name);
}

static void tp_sleep(DWORD ms)
{
    ((void(WINAPI *)(DWORD))k32("Sleep"))(ms);
}

static BOOL tp_close_handle(HANDLE h)
{
    return ((BOOL(WINAPI *)(HANDLE))k32("CloseHandle"))(h);
}

static void tp_run(struct tp_item *it, DWORD result)
{
    void *instance = it;
    switch (it->kind) {
    case TP_WORK:
    case TP_TIMER: ((void(WINAPI *)(void *, void *, void *))it->callback)(instance, it->context, it); break;
    case TP_WAIT:
        ((void(WINAPI *)(void *, void *, void *, DWORD))it->callback)(instance, it->context, it, result);
        break;
    case TP_SIMPLE: ((void(WINAPI *)(void *, void *))it->callback)(instance, it->context); break;
    }
}

static DWORD WINAPI tp_thread(void *arg)
{
    struct tp_item *it = arg;
    tp_run(it, 0);
    __sync_fetch_and_sub(&it->running, 1);
    if (it->kind == TP_SIMPLE)
        w32_free(it);
    return 0;
}

static bool tp_spawn(struct tp_item *it)
{
    HANDLE(WINAPI * create)(void *, SIZE_T, void *, void *, DWORD, DWORD *) = k32("CreateThread");
    __sync_fetch_and_add(&it->running, 1);
    HANDLE t = create(NULL, 0, (void *)tp_thread, it, 0, NULL);
    if (!t) {
        __sync_fetch_and_sub(&it->running, 1);
        return false;
    }
    tp_close_handle(t);
    return true;
}

static void tp_drain(struct tp_item *it)
{
    while (it->running > 0)
        tp_sleep(1);
}

static struct tp_item *tp_new(enum tp_kind kind, void *callback, void *context)
{
    if (!callback) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    struct tp_item *it = w32_zalloc(sizeof(*it));
    if (!it) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    it->kind = kind;
    it->callback = callback;
    it->context = context;
    return it;
}

static void *WINAPI CreateThreadpoolWork(void *callback, void *context, void *env)
{
    UNUSED(env);
    return tp_new(TP_WORK, callback, context);
}

static void WINAPI SubmitThreadpoolWork(struct tp_item *it)
{
    if (it)
        tp_spawn(it);
}

static void WINAPI WaitForThreadpoolWorkCallbacks(struct tp_item *it, BOOL cancel)
{
    UNUSED(cancel);
    if (it)
        tp_drain(it);
}

static void WINAPI CloseThreadpoolWork(struct tp_item *it)
{
    if (!it)
        return;
    tp_drain(it);
    w32_free(it);
}

static BOOL WINAPI TrySubmitThreadpoolCallback(void *callback, void *context, void *env)
{
    UNUSED(env);
    struct tp_item *it = tp_new(TP_SIMPLE, callback, context);
    if (!it)
        return FALSE;
    if (tp_spawn(it))
        return TRUE;
    w32_free(it);
    return FALSE;
}

/* registered waits call back with (context, timed out) */
static void WINAPI tp_fired(struct tp_item *it, uint8_t timed_out)
{
    if (it->closing)
        return;
    __sync_fetch_and_add(&it->running, 1);
    if (it->kind == TP_WAIT)
        it->set = false;
    tp_run(it, timed_out ? 0x102u : 0);
    __sync_fetch_and_sub(&it->running, 1);
}

static void tp_unregister(struct tp_item *it)
{
    if (!it->wait)
        return;
    BOOL(WINAPI * unregister)(HANDLE, HANDLE) = k32("UnregisterWaitEx");
    unregister(it->wait, INVALID_HANDLE_VALUE);
    it->wait = NULL;
}

static bool tp_register(struct tp_item *it, HANDLE object, DWORD ms, DWORD flags)
{
    BOOL(WINAPI * reg)(HANDLE *, HANDLE, void *, void *, DWORD, DWORD) = k32("RegisterWaitForSingleObject");
    return reg(&it->wait, object, (void *)tp_fired, it, ms, flags);
}

static void *WINAPI CreateThreadpoolTimer(void *callback, void *context, void *env)
{
    UNUSED(env);
    struct tp_item *it = tp_new(TP_TIMER, callback, context);
    if (!it)
        return NULL;
    HANDLE(WINAPI * create)(void *, BOOL, const WCHAR *) = k32("CreateWaitableTimerW");
    it->timer = create(NULL, FALSE, NULL);
    if (!it->timer || !tp_register(it, it->timer, INFINITE, 0)) {
        if (it->timer)
            tp_close_handle(it->timer);
        w32_free(it);
        return NULL;
    }
    return it;
}

static void WINAPI SetThreadpoolTimer(struct tp_item *it, const int64_t *due, DWORD period, DWORD window)
{
    UNUSED(window);
    if (!it)
        return;
    if (!due) {
        ((BOOL(WINAPI *)(HANDLE))k32("CancelWaitableTimer"))(it->timer);
        it->set = false;
        return;
    }
    BOOL(WINAPI * set)(HANDLE, const int64_t *, LONG, void *, void *, BOOL) = k32("SetWaitableTimer");
    int64_t when = *due ? *due : -1;
    set(it->timer, &when, (LONG)period, NULL, NULL, FALSE);
    it->set = true;
}

static BOOL WINAPI IsThreadpoolTimerSet(struct tp_item *it)
{
    return it && it->set;
}

static void WINAPI WaitForThreadpoolTimerCallbacks(struct tp_item *it, BOOL cancel)
{
    if (!it)
        return;
    if (cancel)
        SetThreadpoolTimer(it, NULL, 0, 0);
    tp_drain(it);
}

static void WINAPI CloseThreadpoolTimer(struct tp_item *it)
{
    if (!it)
        return;
    it->closing = true;
    tp_unregister(it);
    tp_drain(it);
    tp_close_handle(it->timer);
    w32_free(it);
}

static void *WINAPI CreateThreadpoolWait(void *callback, void *context, void *env)
{
    UNUSED(env);
    return tp_new(TP_WAIT, callback, context);
}

static void WINAPI SetThreadpoolWait(struct tp_item *it, HANDLE object, const int64_t *timeout)
{
    if (!it)
        return;
    tp_unregister(it);
    it->set = false;
    if (!object)
        return;
    DWORD ms = INFINITE;
    if (timeout)
        ms = *timeout < 0 ? (DWORD)MIN(-*timeout / 10000, (int64_t)0xfffffffe) : 0;
    it->object = object;
    it->set = tp_register(it, object, ms, WT_EXECUTEONLYONCE);
}

static void WINAPI WaitForThreadpoolWaitCallbacks2(struct tp_item *it, BOOL cancel)
{
    if (!it)
        return;
    if (cancel)
        tp_unregister(it);
    tp_drain(it);
}

static void WINAPI CloseThreadpoolWait(struct tp_item *it)
{
    if (!it)
        return;
    it->closing = true;
    tp_unregister(it);
    tp_drain(it);
    w32_free(it);
}

/* pools, environments and instance hints: one pool, and the hints change nothing */
static void *WINAPI CreateThreadpool(void *reserved)
{
    UNUSED(reserved);
    return w32_zalloc(16);
}

static void WINAPI CloseThreadpool(void *pool)
{
    w32_free(pool);
}

static BOOL WINAPI SetThreadpoolThreadMinimum(void *pool, DWORD n)
{
    UNUSED(pool);
    UNUSED(n);
    return TRUE;
}

static void WINAPI SetThreadpoolThreadMaximum(void *pool, DWORD n)
{
    UNUSED(pool);
    UNUSED(n);
}

static BOOL WINAPI CallbackMayRunLong(void *instance)
{
    UNUSED(instance);
    return TRUE;
}

static void WINAPI instance_hint2(void *instance, void *arg)
{
    UNUSED(instance);
    UNUSED(arg);
}

static void WINAPI SetEventWhenCallbackReturns(void *instance, HANDLE event)
{
    UNUSED(instance);
    ((BOOL(WINAPI *)(HANDLE))k32("SetEvent"))(event);
}

static void WINAPI ReleaseSemaphoreWhenCallbackReturns(void *instance, HANDLE sem, DWORD n)
{
    UNUSED(instance);
    volatile BOOL done = ((BOOL(WINAPI *)(HANDLE, LONG, LONG *))k32("ReleaseSemaphore"))(sem, (LONG)n, NULL);
    UNUSED(done);
}

static void WINAPI DisassociateCurrentThreadFromCallback(void *instance)
{
    UNUSED(instance);
}

static const struct w32_export threadpool_exports[] = {
    W32_FN(CreateThreadpoolWork),
    W32_FN(SubmitThreadpoolWork),
    W32_FN(WaitForThreadpoolWorkCallbacks),
    W32_FN(CloseThreadpoolWork),
    W32_FN(TrySubmitThreadpoolCallback),
    W32_FN(CreateThreadpoolTimer),
    W32_FN(SetThreadpoolTimer),
    W32_AS("SetThreadpoolTimerEx", SetThreadpoolTimer),
    W32_FN(IsThreadpoolTimerSet),
    W32_FN(WaitForThreadpoolTimerCallbacks),
    W32_FN(CloseThreadpoolTimer),
    W32_FN(CreateThreadpoolWait),
    W32_FN(SetThreadpoolWait),
    W32_AS("WaitForThreadpoolWaitCallbacks", WaitForThreadpoolWaitCallbacks2),
    W32_FN(CloseThreadpoolWait),
    W32_FN(CreateThreadpool),
    W32_FN(CloseThreadpool),
    W32_FN(SetThreadpoolThreadMinimum),
    W32_FN(SetThreadpoolThreadMaximum),
    W32_FN(CallbackMayRunLong),
    W32_AS("LeaveCriticalSectionWhenCallbackReturns", instance_hint2),
    W32_AS("FreeLibraryWhenCallbackReturns", instance_hint2),
    W32_FN(SetEventWhenCallbackReturns),
    W32_FN(ReleaseSemaphoreWhenCallbackReturns),
    W32_FN(DisassociateCurrentThreadFromCallback),
};

W32_EXPORTS(threadpool_module, "kernel32.dll", threadpool_exports);
