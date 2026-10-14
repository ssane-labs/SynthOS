/* Event ping-pong between threads in several processes at once: checks wake-ups are not lost or slow. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static HANDLE ping, pong;
static volatile LONG rounds;

static DWORD WINAPI worker(void *arg)
{
    SetEvent((HANDLE)arg);
    for (;;) {
        WaitForSingleObject(ping, INFINITE);
        InterlockedIncrement(&rounds);
        SetEvent(pong);
    }
}

static int run(const char *tag)
{
    DWORD start = GetTickCount();
    for (int t = 0; t < 20; t++) {
        HANDLE started = CreateEventW(NULL, TRUE, FALSE, NULL);
        ping = CreateEventW(NULL, FALSE, FALSE, NULL);
        pong = CreateEventW(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, worker, started, 0, NULL);
        if (WaitForSingleObject(started, 10000) != WAIT_OBJECT_0) {
            printf("%s: thread %d never started\n", tag, t);
            return 1;
        }
        for (int i = 0; i < 200; i++) {
            SetEvent(ping);
            if (WaitForSingleObject(pong, 10000) != WAIT_OBJECT_0) {
                printf("%s: lost wakeup at thread %d round %d\n", tag, t, i);
                return 2;
            }
        }
    }
    printf("%s: ok, %ld rounds in %lu ms\n", tag, rounds, GetTickCount() - start);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return run(argv[1]);
    char self[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, self, sizeof(self));
    PROCESS_INFORMATION pi[3];
    for (int i = 0; i < 3; i++) {
        STARTUPINFOA si = {sizeof(si)};
        snprintf(cmd, sizeof(cmd), "\"%s\" child%d", self, i);
        CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi[i]);
    }
    int rc = run("parent");
    for (int i = 0; i < 3; i++) {
        DWORD w = WaitForSingleObject(pi[i].hProcess, 60000), code = 77;
        GetExitCodeProcess(pi[i].hProcess, &code);
        printf("child%d wait=%lu exit=%lu\n", i, w, code);
    }
    return rc;
}
