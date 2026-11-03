/* Multimedia timers: periodic callbacks, a one-shot that sets an event, and killing them. */
#include <stdio.h>
#include <windows.h>

static volatile LONG ticks, last_id;
static volatile DWORD_PTR seen_user;

static void CALLBACK on_tick(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR a, DWORD_PTR b)
{
    (void)msg;
    (void)a;
    (void)b;
    InterlockedIncrement(&ticks);
    last_id = (LONG)id;
    seen_user = user;
}

int main(void)
{
    UINT periodic = timeSetEvent(10, 1, on_tick, 0x1234, TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
    Sleep(300);
    MMRESULT killed = timeKillEvent(periodic);
    LONG at_kill = ticks;
    Sleep(100);
    int stopped = ticks <= at_kill + 1;

    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD start = GetTickCount();
    UINT once = timeSetEvent(50, 1, (LPTIMECALLBACK)ev, 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_SET);
    DWORD wait = WaitForSingleObject(ev, 2000);
    DWORD took = GetTickCount() - start;
    printf("mmtimer periodic=%d user=%d id=%d stopped=%d\n", at_kill >= 10, seen_user == 0x1234,
           last_id == (LONG)periodic, stopped);
    printf("mmtimer event=%d delay=%d kill=%u bad=%u\n", wait == WAIT_OBJECT_0 && once != 0, took >= 40 && took < 1000,
           killed, timeKillEvent(12345));
    CloseHandle(ev);
    return 0;
}
