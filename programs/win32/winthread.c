/* Threads, critical sections, events, semaphores and condition variables. */
#include <process.h>
#include <stdio.h>
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
    return 0;
}
