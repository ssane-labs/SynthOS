/* Threads, critical sections, events, semaphores and condition variables. */
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static CRITICAL_SECTION lock;
static volatile LONG counter;
static long shared_total;
static HANDLE ready, items;
static CONDITION_VARIABLE cv;
static int queue_len, consumed;

static DWORD WINAPI adder(LPVOID arg)
{
    int n = (int)(INT_PTR)arg;
    for (int i = 0; i < n; i++) {
        EnterCriticalSection(&lock);
        shared_total += 1;
        LeaveCriticalSection(&lock);
        InterlockedIncrement(&counter);
        if (i % 1000 == 0)
            Sleep(0);
    }
    return (DWORD)n;
}

static DWORD WINAPI spinner(LPVOID arg)
{
    volatile int *stop = arg;
    unsigned long spins = 0;
    while (!*stop)
        spins++;
    return spins > 0;
}

static DWORD WINAPI waiter(LPVOID arg)
{
    (void)arg;
    WaitForSingleObject(ready, INFINITE);
    for (int i = 0; i < 3; i++)
        ReleaseSemaphore(items, 1, NULL);
    return 7;
}

static unsigned __stdcall producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; i++) {
        EnterCriticalSection(&lock);
        queue_len++;
        LeaveCriticalSection(&lock);
        WakeConditionVariable(&cv);
        Sleep(5);
    }
    return 0;
}

static LPVOID main_fiber;
static int fiber_steps;

static void WINAPI fiber_body(LPVOID arg)
{
    volatile double half = 0.5;
    for (;;) {
        fiber_steps += (int)(INT_PTR)arg + (int)(half * 2) - 1;
        SwitchToFiber(main_fiber);
    }
}

int main(void)
{
    InitializeCriticalSection(&lock);
    HANDLE threads[4];
    for (int i = 0; i < 4; i++)
        threads[i] = CreateThread(NULL, 0, adder, (LPVOID)(INT_PTR)5000, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(threads[0], &code);
    printf("threads done: total=%ld counter=%ld code=%lu\n", shared_total, counter, code);

    volatile int stop = 0;
    HANDLE spin = CreateThread(NULL, 0, spinner, (LPVOID)&stop, 0, NULL);
    Sleep(30);
    stop = 1;
    DWORD r = WaitForSingleObject(spin, 2000);
    printf("preemption: %s\n", r == WAIT_OBJECT_0 ? "ok" : "stuck");

    ready = CreateEvent(NULL, TRUE, FALSE, NULL);
    items = CreateSemaphore(NULL, 0, 10, NULL);
    HANDLE w = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    printf("timeout: %s\n", WaitForSingleObject(items, 20) == WAIT_TIMEOUT ? "ok" : "wrong");
    SetEvent(ready);
    int got = 0;
    while (WaitForSingleObject(items, 1000) == WAIT_OBJECT_0 && ++got < 3)
        ;
    WaitForSingleObject(w, INFINITE);
    GetExitCodeThread(w, &code);
    printf("semaphore: got=%d waiter=%lu\n", got, code);

    InitializeConditionVariable(&cv);
    HANDLE p = (HANDLE)_beginthreadex(NULL, 0, producer, NULL, 0, NULL);
    EnterCriticalSection(&lock);
    while (consumed < 5) {
        while (queue_len == 0)
            SleepConditionVariableCS(&cv, &lock, INFINITE);
        queue_len--;
        consumed++;
    }
    LeaveCriticalSection(&lock);
    WaitForSingleObject(p, INFINITE);
    printf("condition variable: consumed=%d\n", consumed);
    main_fiber = ConvertThreadToFiber(NULL);
    LPVOID f = CreateFiber(0, fiber_body, (LPVOID)2);
    for (int i = 0; i < 3 && f; i++)
        SwitchToFiber(f);
    if (f)
        DeleteFiber(f);
    printf("fibers: steps=%d back=%d\n", fiber_steps, GetCurrentFiber() == main_fiber);

    /* the small functions that run without the call gate: TLS (in the TEB and past it), last error, locks, CRT */
    DWORD slots[70];
    for (int i = 0; i < 70; i++)
        slots[i] = TlsAlloc();
    TlsSetValue(slots[0], (void *)1234);
    TlsSetValue(slots[69], (void *)5678);
    SetLastError(77);
    DWORD e1 = GetLastError();
    DWORD v1 = (DWORD)(uintptr_t)TlsGetValue(slots[0]), e2 = GetLastError();
    DWORD v2 = (DWORD)(uintptr_t)TlsGetValue(slots[69]);
    EnterCriticalSection(&lock);
    EnterCriticalSection(&lock);
    LONG depth = lock.RecursionCount;
    LeaveCriticalSection(&lock);
    LeaveCriticalSection(&lock);
    printf("tls: %lu %lu errors=%lu,%lu depth=%ld free=%d tid=%d\n", v1, v2, e1, e2, depth, lock.OwningThread == 0,
           GetCurrentThreadId() == GetThreadId(GetCurrentThread()));
    void *(*volatile move)(void *, const void *, size_t) = memmove;
    void *(*volatile fill)(void *, int, size_t) = memset;
    size_t (*volatile len)(const char *) = strlen;
    int (*volatile cmp)(const char *, const char *) = strcmp;
    char text[16] = "abcdefgh";
    move(text + 1, text, 5);
    move(text + 8, text + 2, 0);
    fill(text + 6, 'x', 2);
    printf("crt: %s %d %d %d %d\n", text, (int)len(text), cmp("ab", "ac") < 0, cmp("b", "a") > 0, cmp(text, text));
    return 0;
}
