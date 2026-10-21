#include <stdio.h>
#include <windows.h>

typedef int (*get_fn)(int);
typedef int (*flags_fn)(void);
static get_fn get;
static int other;

static DWORD WINAPI worker(LPVOID arg)
{
    other = get(1);
    return 0;
}

int main(void)
{
    HMODULE mod = LoadLibraryA("tls64.dll");
    get = (get_fn)GetProcAddress(mod, "tls64_get");
    flags_fn flags = (flags_fn)GetProcAddress(mod, "tls64_flags");
    if (!get || !flags) {
        printf("tls64.dll missing\n");
        return 1;
    }
    int first = get(10);
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    printf("dll tls main=%d thread=%d again=%d flags=%d\n", first, other, get(0), flags());
    return 0;
}
