#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static LONG WINAPI filter(EXCEPTION_POINTERS *info)
{
    printf("filter saw 0x%08lx\n", info->ExceptionRecord->ExceptionCode);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "info";
    if (!strcmp(mode, "crash")) {
        volatile int *p = (int *)(uintptr_t)(argc - 2);
        printf("about to crash\n");
        fflush(stdout);
        *p = 1;
    } else if (!strcmp(mode, "loop")) {
        printf("looping forever, press Ctrl+C\n");
        fflush(stdout);
        for (volatile unsigned i = 0;; i++)
            ;
    } else if (!strcmp(mode, "raise")) {
        SetUnhandledExceptionFilter(filter);
        RaiseException(0xE0001234, 0, 0, NULL);
    } else if (!strcmp(mode, "system")) {
        int rc = system("echo inside system; winhello nested; wintest info");
        printf("system returned %d\n", rc);
    } else if (!strcmp(mode, "box")) {
        int r = MessageBoxA(NULL, "SynthOS runs Windows programs.\nPress Y or N.", "Question", MB_YESNO);
        printf("answer=%s\n", r == IDYES ? "yes" : "no");
    } else {
        OSVERSIONINFOA v = {sizeof(v)};
        GetVersionExA(&v);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        MEMORYSTATUSEX mem = {sizeof(mem)};
        GlobalMemoryStatusEx(&mem);
        char name[64], user[64], dir[MAX_PATH];
        DWORD n = sizeof(name), u = sizeof(user);
        GetComputerNameA(name, &n);
        GetUserNameA(user, &u);
        GetWindowsDirectoryA(dir, sizeof(dir));
        SYSTEMTIME t;
        GetLocalTime(&t);
        printf("Windows %lu.%lu build %lu, %lu CPU, page %lu\n", v.dwMajorVersion, v.dwMinorVersion,
               v.dwBuildNumber, si.dwNumberOfProcessors, si.dwPageSize);
        printf("memory: %llu MB total, %llu MB free\n", mem.ullTotalPhys >> 20, mem.ullAvailPhys >> 20);
        printf("computer=%s user=%s windir=%s\n", name, user, dir);
        printf("date %04d-%02d-%02d, ticks %lu\n", t.wYear, t.wMonth, t.wDay, GetTickCount() > 0 ? 1UL : 0UL);
    }
    return 0;
}
