/* wincompat: the system pieces older and packed Windows tools look for: a real export directory in the built-in DLLs,
   the process snapshot, timer queues, activation contexts, the connection tables, ODBC, the service manager,
   multibyte string functions and GDI+. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

static FARPROC proc(const char *dll, const char *name)
{
    HMODULE m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, name) : NULL;
}

/* look a name up the way the Unicode layer for Windows 9x does: through the export directory */
static void *walk_exports(HMODULE m, const char *name)
{
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress)
        return NULL;
    IMAGE_EXPORT_DIRECTORY *e = (IMAGE_EXPORT_DIRECTORY *)(base + d.VirtualAddress);
    DWORD *names = (DWORD *)(base + e->AddressOfNames), *funcs = (DWORD *)(base + e->AddressOfFunctions);
    WORD *ords = (WORD *)(base + e->AddressOfNameOrdinals);
    int lo = 0, hi = (int)e->NumberOfNames - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, (char *)base + names[mid]);
        if (!c)
            return base + funcs[ords[mid]];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

static void exports(void)
{
    void *a = walk_exports(GetModuleHandleA("kernel32.dll"), "FindResourceW");
    void *b = walk_exports(GetModuleHandleA("ntdll.dll"), "LdrUnloadDll");
    printf("exports walk=%d match=%d ntdll=%d\n", a != NULL, a == (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FindResourceW"),
           b != NULL);
}

static void processes(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 e = {sizeof(e)};
    int n = 0, self = 0;
    for (BOOL ok = Process32First(snap, &e); ok; ok = Process32Next(snap, &e), n++)
        if (e.th32ProcessID == GetCurrentProcessId() && !_stricmp(e.szExeFile, "wincompat.exe"))
            self = 1;
    CloseHandle(snap);
    DWORD pids[64], need = 0;
    BOOL(WINAPI * enum_processes)(DWORD *, DWORD, DWORD *) = (void *)proc("psapi.dll", "EnumProcesses");
    BOOL ok = enum_processes && enum_processes(pids, sizeof(pids), &need);
    printf("processes system=%d self=%d enum=%d\n", n >= 3, self, ok && need / 4 == (DWORD)n);
}

static volatile LONG fired;

static void CALLBACK on_timer(void *param, BOOLEAN timer)
{
    if (timer && param == (void *)7)
        InterlockedIncrement(&fired);
}

static void timer_queue(void)
{
    HANDLE q = CreateTimerQueue(), t = NULL;
    BOOL made = CreateTimerQueueTimer(&t, q, on_timer, (void *)7, 10, 10, 0);
    for (int i = 0; i < 200 && fired < 3; i++)
        Sleep(10);
    DeleteTimerQueueTimer(q, t, INVALID_HANDLE_VALUE);
    LONG after = fired;
    Sleep(50);
    DeleteTimerQueueEx(q, NULL);
    printf("timerqueue made=%d repeats=%d stopped=%d\n", made, fired >= 3, fired == after);
}

static void actctx(void)
{
    struct {
        HANDLE ctx;
        DWORD flags;
    } info = {0};
    BOOL(WINAPI * query)(DWORD, HANDLE, void *, ULONG, void *, SIZE_T, SIZE_T *) = (void *)proc("kernel32.dll", "QueryActCtxW");
    ULONG_PTR cookie = 0;
    HANDLE ctx = NULL;
    BOOL(WINAPI * current)(HANDLE *) = (void *)proc("kernel32.dll", "GetCurrentActCtx");
    BOOL(WINAPI * activate)(HANDLE, ULONG_PTR *) = (void *)proc("kernel32.dll", "ActivateActCtx");
    BOOL(WINAPI * deactivate)(DWORD, ULONG_PTR) = (void *)proc("kernel32.dll", "DeactivateActCtx");
    BOOL q = query && query(0x80000010, GetModuleHandleA(NULL), NULL, 1, &info, sizeof(info), NULL);
    BOOL a = activate && current && current(&ctx) && activate(info.ctx, &cookie) && deactivate(0, cookie);
    printf("actctx query=%d context=%d activate=%d\n", q, info.ctx != NULL, a);
}

static void tables(void)
{
    DWORD(WINAPI * tcp)(void *, DWORD *, BOOL, ULONG, ULONG, ULONG) = (void *)proc("iphlpapi.dll", "GetExtendedTcpTable");
    DWORD(WINAPI * udp)(void *, DWORD *, BOOL, ULONG, ULONG, ULONG) = (void *)proc("iphlpapi.dll", "GetExtendedUdpTable");
    DWORD size = 0, big[512];
    DWORD r1 = tcp ? tcp(NULL, &size, FALSE, AF_INET, 5, 0) : 1;
    DWORD r2 = tcp ? tcp(big, &size, FALSE, AF_INET, 5, 0) : 1;
    size = sizeof(big);
    DWORD r3 = udp ? udp(big, &size, FALSE, AF_INET, 1, 0) : 1;
    printf("tables size=%lu tcp=%lu udp=%lu\n", r1, r2, r3);
}

static void odbc(void)
{
    SHORT(WINAPI * alloc)(SHORT, void *, void **) = (void *)proc("odbc32.dll", "SQLAllocHandle");
    SHORT(WINAPI * attr)(void *, LONG, void *, LONG) = (void *)proc("odbc32.dll", "SQLSetEnvAttr");
    SHORT(WINAPI * connect)(void *, HWND, char *, SHORT, char *, SHORT, SHORT *, USHORT) = (void *)proc("odbc32.dll", "SQLDriverConnect");
    SHORT(WINAPI * diag)(SHORT, void *, SHORT, char *, LONG *, char *, SHORT, SHORT *) = (void *)proc("odbc32.dll", "SQLGetDiagRec");
    void *env = NULL, *dbc = NULL;
    char state[6] = "", text[256] = "";
    LONG native;
    SHORT len;
    if (!alloc || alloc(1, NULL, &env) || attr(env, 200, (void *)3, 0) || alloc(2, env, &dbc)) {
        printf("odbc no\n");
        return;
    }
    SHORT r = connect(dbc, NULL, "DSN=nothing", -3, NULL, 0, NULL, 0);
    diag(2, dbc, 1, state, &native, text, sizeof(text), &len);
    printf("odbc connect=%d state=%s\n", r, state);
}

static void services(void)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    SC_HANDLE svc = scm ? OpenServiceA(scm, "Everything", SERVICE_QUERY_STATUS) : NULL;
    DWORD err = GetLastError(), need = 0, count = 5, resume = 0;
    BYTE buf[256];
    BOOL listed = scm && EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_STATE_ALL, (void *)buf, sizeof(buf), &need, &count,
                                            &resume);
    printf("scm open=%d service=%d err=%lu listed=%d count=%lu\n", scm != NULL, svc != NULL, err, listed, count);
    CloseServiceHandle(scm);
}

static void mbs(void)
{
    unsigned char *(__cdecl * cat)(unsigned char *, const unsigned char *, size_t) = (void *)proc("msvcrt.dll", "_mbsnbcat");
    int(__cdecl * cmp)(const unsigned char *, const unsigned char *, size_t) = (void *)proc("msvcrt.dll", "_mbsnbicmp");
    unsigned char buf[32] = "Curr";
    cat(buf, (const unsigned char *)"Portsxyz", 5);
    printf("mbs %s cmp=%d\n", buf, cmp(buf, (const unsigned char *)"CURRPORTS!", 9));
}

typedef struct {
    UINT32 version;
    void *callback;
    BOOL no_thread, no_codecs;
} startup_input;

static void gdiplus(void)
{
    int(WINAPI * startup)(ULONG_PTR *, const startup_input *, void *) = (void *)proc("gdiplus.dll", "GdiplusStartup");
    int(WINAPI * create)(INT, INT, INT, INT, BYTE *, void **) = (void *)proc("gdiplus.dll", "GdipCreateBitmapFromScan0");
    int(WINAPI * context)(void *, void **) = (void *)proc("gdiplus.dll", "GdipGetImageGraphicsContext");
    int(WINAPI * brush)(DWORD, void **) = (void *)proc("gdiplus.dll", "GdipCreateSolidFill");
    int(WINAPI * fill)(void *, void *, INT, INT, INT, INT) = (void *)proc("gdiplus.dll", "GdipFillRectangleI");
    int(WINAPI * pixel)(void *, INT, INT, DWORD *) = (void *)proc("gdiplus.dll", "GdipBitmapGetPixel");
    ULONG_PTR token;
    startup_input in = {1, NULL, FALSE, FALSE};
    void *bmp = NULL, *g = NULL, *b = NULL;
    DWORD inside = 0, outside = 0;
    int ok = startup && !startup(&token, &in, NULL) && !create(16, 16, 0, 0x26200a, NULL, &bmp) && !context(bmp, &g) &&
             !brush(0xff336699, &b) && !fill(g, b, 2, 2, 4, 4) && !pixel(bmp, 3, 3, &inside) && !pixel(bmp, 10, 10, &outside);
    printf("gdiplus ok=%d inside=%08lx outside=%08lx\n", ok, inside, outside);
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "all";
    struct {
        const char *name;
        void (*fn)(void);
    } parts[] = {{"exports", exports}, {"processes", processes}, {"timerqueue", timer_queue}, {"actctx", actctx},
                 {"tables", tables},   {"odbc", odbc},           {"scm", services},           {"mbs", mbs},
                 {"gdiplus", gdiplus}};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        if (!strcmp(what, "all") || !strcmp(what, parts[i].name))
            parts[i].fn();
    return 0;
}
