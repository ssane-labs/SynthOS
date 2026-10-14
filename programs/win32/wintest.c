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

static int data_var = 1;

static void compat(void)
{
    LOGFONTW lf = {0};
    lf.lfHeight = -13;
    lf.lfWeight = FW_BOLD;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    HFONT font = CreateFontIndirectW(&lf);
    LOGFONTW back = {0};
    int size = GetObjectW(font, sizeof(back), &back);
    DeleteObject(font);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    BOOL spi = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    printf("logfontw=%d face=%s ncm=%s\n", size, wcscmp(back.lfFaceName, L"Segoe UI") ? "bad" : "ok",
           spi && ncm.lfMessageFont.lfFaceName[0] ? "ok" : "bad");

    printf("lstrcmpW null=%d,%d,%d\n", lstrcmpW(NULL, L"a") < 0, lstrcmpW(L"a", NULL) > 0, lstrcmpW(NULL, NULL) == 0);

    HANDLE rw = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL), ro = NULL, w = NULL;
    BOOL made = DuplicateHandle(GetCurrentProcess(), rw, GetCurrentProcess(), &ro, FILE_MAP_READ, FALSE, 0);
    BOOL widened = DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &w, FILE_MAP_WRITE, FALSE, 0);
    void *view = MapViewOfFile(ro, FILE_MAP_WRITE, 0, 0, 0);
    printf("section read-only=%s write-dup=%s write-view=%s\n", made ? "ok" : "bad", widened ? "allowed" : "denied",
           view ? "allowed" : "denied");

    char path[MAX_PATH];
    GetTempPathA(sizeof(path) - 16, path);
    strcat(path, "mapped.txt");
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n;
    WriteFile(file, "aaaa", 4, &n, NULL);
    HANDLE map = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    const char *text = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    WriteFile(file, "bb", 2, &n, NULL);
    printf("mapped view=%.4s\n", text ? text : "none");
    UnmapViewOfFile(text);
    CloseHandle(map);
    CloseHandle(file);
    DeleteFileA(path);

    MEMORY_BASIC_INFORMATION mbi = {0};
    VirtualQuery((void *)compat, &mbi, sizeof(mbi));
    DWORD old = 0;
    VirtualProtect(&data_var, sizeof(data_var), PAGE_READONLY, &old);
    DWORD now = 0;
    VirtualProtect(&data_var, sizeof(data_var), old, &now);
    printf("image code=%lx data=%lx protected=%lx\n", mbi.Protect, old, now);

    typedef LONG(WINAPI * query_fn)(HANDLE, int, void *, ULONG, ULONG *);
    query_fn query = (query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    struct {
        USHORT length, max;
        WCHAR *buffer;
        BYTE rest[200];
    } type = {0};
    LONG st = query ? query(rw, 2, &type, sizeof(type), NULL) : -1;
    HANDLE self = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    printf("object type=%ls self=%s\n", st == 0 ? type.buffer : L"?",
           self && self != GetCurrentProcess() ? "handle" : "pseudo");

    HMODULE power = LoadLibraryA("api-ms-win-power-base-l1-1-0.dll"), acc = LoadLibraryA("oleacc.dll");
    printf("power=%s oleacc=%s\n", power && GetProcAddress(power, "CallNtPowerInformation") ? "ok" : "missing",
           acc && GetProcAddress(acc, "AccessibleObjectFromWindow") ? "ok" : "missing");
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
    } else if (!strcmp(mode, "compat")) {
        compat();
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
