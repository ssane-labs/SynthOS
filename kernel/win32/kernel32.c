#include <synth/arch_win32.h>
#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/time.h>

#include "win32.h"

#define TLS_SLOTS_OFFSET 0xe10
#define HEAP_ZERO_MEMORY 0x08
#define MEM_COMMIT 0x1000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#define WAIT_OBJECT_0 0
#define STILL_ACTIVE 259

typedef struct {
    DWORD low;
    DWORD high;
} FILETIME;

typedef struct {
    WORD year, month, weekday, day, hour, minute, second, ms;
} SYSTEMTIME;

static void *unhandled_filter;
static uint32_t error_mode;
static void *fls_values[W32_TLS_SLOTS];
static bool fls_used[W32_TLS_SLOTS];

static void WINAPI ExitProcess(UINT code)
{
    w32_exit(code);
}

static BOOL WINAPI TerminateProcess(HANDLE process, UINT code)
{
    if (process == W32_PSEUDO_PROCESS)
        w32_exit(code);
    w32_set_error(ERROR_ACCESS_DENIED);
    return FALSE;
}

static HANDLE WINAPI GetCurrentProcess(void)
{
    return W32_PSEUDO_PROCESS;
}

static HANDLE WINAPI GetCurrentThread(void)
{
    return W32_PSEUDO_THREAD;
}

static DWORD WINAPI GetCurrentProcessId(void)
{
    return 0x100;
}

static DWORD WINAPI GetCurrentThreadId(void)
{
    return 0x104;
}

static DWORD WINAPI GetLastError(void)
{
    return w32_error();
}

static void WINAPI SetLastError(DWORD e)
{
    w32_set_error(e);
}

static UINT WINAPI SetErrorMode(UINT mode)
{
    UINT old = error_mode;
    error_mode = mode;
    return old;
}

static UINT WINAPI GetErrorMode(void)
{
    return error_mode;
}

static HMODULE module_handle(const char *name)
{
    if (!name)
        return (HMODULE)w32->main->base;
    struct w32_image *img = w32_find_module(name);
    if (!img) {
        w32_set_error(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return (HMODULE)img->base;
}

static HMODULE WINAPI GetModuleHandleA(LPCSTR name)
{
    return module_handle(name);
}

static HMODULE WINAPI GetModuleHandleW(LPCWSTR name)
{
    char *n = w32_narrow(name);
    HMODULE h = module_handle(n);
    w32_free(n);
    return h;
}

static BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out)
{
    if (flags & 4) {
        *out = (HMODULE)w32->main->base;
        for (struct w32_image *img = w32->images; img; img = img->next)
            if ((uint32_t)name >= img->base && (uint32_t)name < img->base + img->size)
                *out = (HMODULE)img->base;
        return TRUE;
    }
    *out = module_handle(name);
    return *out != NULL;
}

static BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
{
    if (flags & 4)
        return GetModuleHandleExA(flags, (LPCSTR)name, out);
    char *n = w32_narrow(name);
    BOOL ok = GetModuleHandleExA(flags, n, out);
    w32_free(n);
    return ok;
}

static struct w32_image *image_of(HMODULE h)
{
    if (!h)
        return w32->main;
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (img->base == (uint32_t)h)
            return img;
    return NULL;
}

static DWORD WINAPI GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD size)
{
    struct w32_image *img = image_of(h);
    if (!img || !size)
        return 0;
    char win[PATH_MAX_LEN];
    w32_path_to_win(img->path, win, sizeof(win));
    size_t n = str_copy(buf, win, size);
    if (n >= size) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    return (DWORD)n;
}

static DWORD WINAPI GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD size)
{
    char tmp[PATH_MAX_LEN];
    DWORD n = GetModuleFileNameA(h, tmp, sizeof(tmp));
    if (!n || !size)
        return 0;
    size_t w = w32_cp_to_wide(tmp, (int)n, buf, size - 1, 1);
    buf[MIN(w, (size_t)size - 1)] = 0;
    return (DWORD)MIN(w, (size_t)size - 1);
}

static HMODULE load_library(const char *name)
{
    int err = 0;
    char dir[PATH_MAX_LEN];
    path_dirname(w32->main->path, dir, sizeof(dir));
    struct w32_image *img = w32_load(name, dir, &err);
    if (!img) {
        w32_set_error((DWORD)err);
        return NULL;
    }
    if (w32_init_modules() != 0) {
        w32_set_error(1114);
        return NULL;
    }
    return (HMODULE)img->base;
}

static HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    return load_library(name);
}

static HMODULE WINAPI LoadLibraryW(LPCWSTR name)
{
    char *n = w32_narrow(name);
    HMODULE h = load_library(n);
    w32_free(n);
    return h;
}

static HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    UNUSED(file);
    UNUSED(flags);
    return load_library(name);
}

static HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    UNUSED(file);
    UNUSED(flags);
    return LoadLibraryW(name);
}

static BOOL WINAPI FreeLibrary(HMODULE h)
{
    UNUSED(h);
    return TRUE;
}

static void *WINAPI GetProcAddress(HMODULE h, LPCSTR name)
{
    struct w32_image *img = image_of(h);
    if (!img) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    bool ordinal = (uint32_t)name < 0x10000u;
    void *fn = w32_get_proc(img, ordinal ? NULL : name, ordinal ? (uint32_t)name : 0);
    if (!fn)
        w32_set_error(ERROR_PROC_NOT_FOUND);
    return fn;
}

static LPSTR WINAPI GetCommandLineA(void)
{
    return w32->cmdline;
}

static LPWSTR WINAPI GetCommandLineW(void)
{
    return w32->cmdline_w;
}

static void WINAPI GetStartupInfoA(void *info)
{
    memset(info, 0, 68);
    *(DWORD *)info = 68;
}

static void WINAPI GetStartupInfoW(void *info)
{
    GetStartupInfoA(info);
}

static DWORD WINAPI GetVersion(void)
{
    return (19045u << 16) | 10u;
}

static BOOL fill_version(uint8_t *info, bool wide)
{
    DWORD size = *(DWORD *)info;
    memcpy(info + 4, &(DWORD){10}, 4);
    memcpy(info + 8, &(DWORD){0}, 4);
    memcpy(info + 12, &(DWORD){19045}, 4);
    memcpy(info + 16, &(DWORD){2}, 4);
    memset(info + 20, 0, wide ? 256 : 128);
    if ((wide && size >= 284) || (!wide && size >= 156)) {
        uint8_t *ex = info + 20 + (wide ? 256 : 128);
        memset(ex, 0, 8);
        ex[6] = 1;
    }
    return TRUE;
}

static BOOL WINAPI GetVersionExA(void *info)
{
    return fill_version(info, false);
}

static BOOL WINAPI GetVersionExW(void *info)
{
    return fill_version(info, true);
}

static BOOL WINAPI IsProcessorFeaturePresent(DWORD feature)
{
    switch (feature) {
    case 6:
    case 10:
        return arch_has_sse();
    case 3:
        return TRUE;
    default:
        return FALSE;
    }
}

static void WINAPI GetSystemInfo(uint8_t *info)
{
    memset(info, 0, 36);
    memcpy(info + 4, &(DWORD){4096}, 4);
    memcpy(info + 8, &(DWORD){0x10000}, 4);
    memcpy(info + 12, &(DWORD){0x7ffeffff}, 4);
    memcpy(info + 16, &(DWORD){1}, 4);
    memcpy(info + 20, &(DWORD){1}, 4);
    memcpy(info + 24, &(DWORD){586}, 4);
    memcpy(info + 28, &(DWORD){0x10000}, 4);
    memcpy(info + 32, &(WORD){6}, 2);
}

static BOOL WINAPI IsWow64Process(HANDLE h, BOOL *wow)
{
    UNUSED(h);
    *wow = FALSE;
    return TRUE;
}

static void *WINAPI SetUnhandledExceptionFilter(void *filter)
{
    void *old = unhandled_filter;
    unhandled_filter = filter;
    return old;
}

static LONG WINAPI UnhandledExceptionFilter(void *info)
{
    UNUSED(info);
    return 1;
}

static void WINAPI RaiseException(DWORD code, DWORD flags, DWORD argc, const ULONG_PTR *args)
{
    UNUSED(flags);
    UNUSED(argc);
    UNUSED(args);
    ksnprintf(w32->message, sizeof(w32->message), "unhandled exception 0x%08x", code);
    w32_exit(code);
}

static BOOL WINAPI IsDebuggerPresent(void)
{
    return FALSE;
}

static void WINAPI OutputDebugStringA(LPCSTR s)
{
    klog("win32: %s", s);
}

static void WINAPI OutputDebugStringW(LPCWSTR s)
{
    char *n = w32_narrow(s);
    klog("win32: %s", n ? n : "");
    w32_free(n);
}

static void WINAPI DebugBreak(void)
{
    str_copy(w32->message, "stopped at a debug breakpoint", sizeof(w32->message));
    w32_exit(0x80000003u);
}

static void WINAPI Sleep(DWORD ms)
{
    w32_sleep(ms);
}

void w32_sleep(uint32_t ms)
{
    con_flush();
    uint64_t end = hal_ticks_ms() + ms;
    while (hal_ticks_ms() < end) {
        hal_sleep_ms((uint32_t)MIN(end - hal_ticks_ms(), (uint64_t)10));
        if (sh_interrupted())
            w32_exit(STATUS_CONTROL_C_EXIT);
    }
}

static DWORD WINAPI SleepEx(DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    Sleep(ms);
    return 0;
}

static DWORD WINAPI GetTickCount(void)
{
    return (DWORD)hal_ticks_ms();
}

static ULONGLONG WINAPI GetTickCount64(void)
{
    return hal_ticks_ms();
}

static BOOL WINAPI QueryPerformanceCounter(LONGLONG *count)
{
    *count = (LONGLONG)hal_ticks_ms() * 1000;
    return TRUE;
}

static BOOL WINAPI QueryPerformanceFrequency(LONGLONG *freq)
{
    *freq = 1000000;
    return TRUE;
}

static uint64_t now_filetime(void)
{
    return ((uint64_t)time_now() + 11644473600ull) * 10000000ull + (hal_ticks_ms() % 1000) * 10000ull;
}

static void WINAPI GetSystemTimeAsFileTime(FILETIME *ft)
{
    uint64_t t = now_filetime();
    ft->low = (DWORD)t;
    ft->high = (DWORD)(t >> 32);
}

static void WINAPI GetSystemTimePreciseAsFileTime(FILETIME *ft)
{
    GetSystemTimeAsFileTime(ft);
}

static void to_systemtime(uint64_t ft, SYSTEMTIME *st)
{
    uint64_t secs = ft / 10000000ull;
    uint32_t epoch = secs >= 11644473600ull ? (uint32_t)(secs - 11644473600ull) : 0;
    struct rtc_time t;
    time_to_rtc(epoch, &t);
    st->year = t.year;
    st->month = t.month;
    st->day = t.day;
    st->hour = t.hour;
    st->minute = t.minute;
    st->second = t.second;
    st->ms = (WORD)(ft / 10000ull % 1000);
    st->weekday = (WORD)time_weekday(&t);
}

static void WINAPI GetSystemTime(SYSTEMTIME *st)
{
    to_systemtime(now_filetime(), st);
}

static void WINAPI GetLocalTime(SYSTEMTIME *st)
{
    to_systemtime(now_filetime(), st);
}

static BOOL WINAPI FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st)
{
    to_systemtime((uint64_t)ft->high << 32 | ft->low, st);
    return TRUE;
}

static BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft)
{
    struct rtc_time t = {st->year, (uint8_t)st->month, (uint8_t)st->day, (uint8_t)st->hour, (uint8_t)st->minute,
                         (uint8_t)st->second};
    uint64_t v = ((uint64_t)time_from_rtc(&t) + 11644473600ull) * 10000000ull + st->ms * 10000ull;
    ft->low = (DWORD)v;
    ft->high = (DWORD)(v >> 32);
    return TRUE;
}

static BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *in, FILETIME *out)
{
    *out = *in;
    return TRUE;
}

static BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *in, FILETIME *out)
{
    *out = *in;
    return TRUE;
}

static BOOL WINAPI SystemTimeToTzSpecificLocalTime(void *tz, const SYSTEMTIME *in, SYSTEMTIME *out)
{
    UNUSED(tz);
    *out = *in;
    return TRUE;
}

static DWORD WINAPI GetTimeZoneInformation(void *info)
{
    memset(info, 0, 172);
    return 0;
}

static BOOL WINAPI GetComputerNameA(LPSTR buf, DWORD *size)
{
    const char *host = sh_get(w32->ctx->sh, "HOSTNAME");
    size_t n = strlen(host ? host : "SYNTH");
    if (n + 1 > *size) {
        *size = (DWORD)n + 1;
        w32_set_error(111);
        return FALSE;
    }
    str_copy(buf, host ? host : "SYNTH", *size);
    str_upper(buf);
    *size = (DWORD)n;
    return TRUE;
}

static BOOL WINAPI GetComputerNameW(LPWSTR buf, DWORD *size)
{
    char tmp[64];
    DWORD n = sizeof(tmp);
    GetComputerNameA(tmp, &n);
    if (n + 1 > *size) {
        *size = n + 1;
        return FALSE;
    }
    w32_cp_to_wide(tmp, -1, buf, *size, 1);
    *size = n;
    return TRUE;
}

static BOOL WINAPI Beep(DWORD freq, DWORD ms)
{
    hal_beep(freq, ms);
    return TRUE;
}

static void *WINAPI VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect)
{
    UNUSED(protect);
    if (addr && !(type & 0x2000))
        return addr;
    void *p = heap_alloc_aligned(&w32->heap, ALIGN_UP(size, 4096u), 4096);
    if (!p)
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static BOOL WINAPI VirtualFree(void *addr, SIZE_T size, DWORD type)
{
    UNUSED(size);
    if (type & MEM_RELEASE)
        heap_free_aligned(&w32->heap, addr);
    return TRUE;
}

static BOOL WINAPI VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old)
{
    UNUSED(addr);
    UNUSED(size);
    UNUSED(protect);
    if (old)
        *old = 0x40;
    return TRUE;
}

static SIZE_T WINAPI VirtualQuery(const void *addr, uint8_t *info, SIZE_T len)
{
    if (len < 28)
        return 0;
    uint32_t a = (uint32_t)addr & ~4095u;
    uint32_t base = a, size = 4096;
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (a >= img->base && a < img->base + img->size)
            base = img->base, size = img->size;
    memcpy(info, &a, 4);
    memcpy(info + 4, &base, 4);
    memcpy(info + 8, &(DWORD){0x40}, 4);
    uint32_t region = base + size - a;
    memcpy(info + 12, &region, 4);
    memcpy(info + 16, &(DWORD){MEM_COMMIT}, 4);
    memcpy(info + 20, &(DWORD){0x40}, 4);
    memcpy(info + 24, &(DWORD){0x1000000}, 4);
    return 28;
}

static HANDLE WINAPI GetProcessHeap(void)
{
    return &w32->heap;
}

static HANDLE WINAPI HeapCreate(DWORD options, SIZE_T initial, SIZE_T max)
{
    UNUSED(options);
    UNUSED(initial);
    UNUSED(max);
    return &w32->heap;
}

static BOOL WINAPI HeapDestroy(HANDLE heap)
{
    UNUSED(heap);
    return TRUE;
}

static void *WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    UNUSED(heap);
    void *p = (flags & HEAP_ZERO_MEMORY) ? w32_zalloc(size) : w32_alloc(size);
    if (!p)
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static void *WINAPI HeapReAlloc(HANDLE heap, DWORD flags, void *ptr, SIZE_T size)
{
    UNUSED(heap);
    size_t old = w32_msize(ptr);
    void *p = w32_realloc(ptr, size);
    if (p && (flags & HEAP_ZERO_MEMORY) && size > old)
        memset((uint8_t *)p + old, 0, size - old);
    return p;
}

static BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, void *ptr)
{
    UNUSED(heap);
    UNUSED(flags);
    w32_free(ptr);
    return TRUE;
}

static SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, const void *ptr)
{
    UNUSED(heap);
    UNUSED(flags);
    return w32_msize((void *)ptr);
}

static BOOL WINAPI HeapValidate(HANDLE heap, DWORD flags, const void *ptr)
{
    UNUSED(heap);
    UNUSED(flags);
    UNUSED(ptr);
    return TRUE;
}

static BOOL WINAPI HeapSetInformation(HANDLE heap, int cls, void *info, SIZE_T len)
{
    UNUSED(heap);
    UNUSED(cls);
    UNUSED(info);
    UNUSED(len);
    return TRUE;
}

static void *WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
    return HeapAlloc(NULL, (flags & 0x40) ? HEAP_ZERO_MEMORY : 0, size);
}

static void *WINAPI GlobalReAlloc(void *mem, SIZE_T size, UINT flags)
{
    return HeapReAlloc(NULL, (flags & 0x40) ? HEAP_ZERO_MEMORY : 0, mem, size);
}

static void *WINAPI GlobalFree(void *mem)
{
    w32_free(mem);
    return NULL;
}

static void *WINAPI GlobalLock(void *mem)
{
    return mem;
}

static BOOL WINAPI GlobalUnlock(void *mem)
{
    UNUSED(mem);
    return TRUE;
}

static SIZE_T WINAPI GlobalSize(void *mem)
{
    return w32_msize(mem);
}

static void WINAPI GlobalMemoryStatus(uint32_t *status)
{
    struct heap_stats hs;
    heap_stats_of(&w32->heap, &hs);
    memset(status, 0, 32);
    status[0] = 32;
    status[1] = (uint32_t)(hs.used * 100 / (hs.total ? hs.total : 1));
    status[2] = (uint32_t)hs.total;
    status[3] = (uint32_t)hs.free;
    status[4] = (uint32_t)hs.total;
    status[5] = (uint32_t)hs.free;
    status[6] = 0x7ffe0000u;
    status[7] = (uint32_t)hs.free;
}

static BOOL WINAPI GlobalMemoryStatusEx(uint32_t *status)
{
    struct heap_stats hs;
    heap_stats_of(&w32->heap, &hs);
    memset(status, 0, 64);
    status[0] = 64;
    status[1] = (uint32_t)(hs.used * 100 / (hs.total ? hs.total : 1));
    status[2] = (uint32_t)hs.total;
    status[4] = (uint32_t)hs.free;
    status[6] = (uint32_t)hs.total;
    status[8] = (uint32_t)hs.free;
    status[10] = 0x7ffe0000u;
    status[12] = (uint32_t)hs.free;
    return TRUE;
}

static DWORD WINAPI TlsAlloc(void)
{
    for (DWORD i = 0; i < W32_TLS_SLOTS; i++) {
        if (!w32->tls_used[i]) {
            w32->tls_used[i] = true;
            memset(w32->teb + TLS_SLOTS_OFFSET + i * 4, 0, 4);
            return i;
        }
    }
    return 0xffffffffu;
}

static BOOL WINAPI TlsFree(DWORD index)
{
    if (index >= W32_TLS_SLOTS)
        return FALSE;
    w32->tls_used[index] = false;
    return TRUE;
}

static void *WINAPI TlsGetValue(DWORD index)
{
    if (index >= W32_TLS_SLOTS)
        return NULL;
    void *v;
    memcpy(&v, w32->teb + TLS_SLOTS_OFFSET + index * 4, 4);
    w32_set_error(0);
    return v;
}

static BOOL WINAPI TlsSetValue(DWORD index, void *value)
{
    if (index >= W32_TLS_SLOTS)
        return FALSE;
    memcpy(w32->teb + TLS_SLOTS_OFFSET + index * 4, &value, 4);
    return TRUE;
}

static DWORD WINAPI FlsAlloc(void *callback)
{
    UNUSED(callback);
    for (DWORD i = 0; i < W32_TLS_SLOTS; i++) {
        if (!fls_used[i]) {
            fls_used[i] = true;
            fls_values[i] = NULL;
            return i;
        }
    }
    return 0xffffffffu;
}

static BOOL WINAPI FlsFree(DWORD index)
{
    if (index < W32_TLS_SLOTS)
        fls_used[index] = false;
    return TRUE;
}

static void *WINAPI FlsGetValue(DWORD index)
{
    return index < W32_TLS_SLOTS ? fls_values[index] : NULL;
}

static BOOL WINAPI FlsSetValue(DWORD index, void *value)
{
    if (index >= W32_TLS_SLOTS)
        return FALSE;
    fls_values[index] = value;
    return TRUE;
}

static void WINAPI CriticalSectionNop(void *cs)
{
    UNUSED(cs);
}

static BOOL WINAPI InitializeCriticalSectionAndSpinCount(void *cs, DWORD spin)
{
    UNUSED(cs);
    UNUSED(spin);
    return TRUE;
}

static BOOL WINAPI InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD flags)
{
    UNUSED(cs);
    UNUSED(spin);
    UNUSED(flags);
    return TRUE;
}

static BOOL WINAPI TryEnterCriticalSection(void *cs)
{
    UNUSED(cs);
    return TRUE;
}

static HANDLE new_object(enum w32_object_type type, uint32_t value)
{
    HANDLE h = w32_handle_new(type);
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->value = value;
    return h;
}

static HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, LPCSTR name)
{
    UNUSED(sa);
    UNUSED(manual);
    UNUSED(name);
    return new_object(WH_EVENT, (uint32_t)initial);
}

static HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    UNUSED(name);
    return CreateEventA(sa, manual, initial, NULL);
}

static HANDLE WINAPI CreateMutexA(void *sa, BOOL owner, LPCSTR name)
{
    UNUSED(sa);
    UNUSED(name);
    return new_object(WH_MUTEX, (uint32_t)owner);
}

static HANDLE WINAPI CreateMutexW(void *sa, BOOL owner, LPCWSTR name)
{
    UNUSED(name);
    return CreateMutexA(sa, owner, NULL);
}

static HANDLE WINAPI CreateSemaphoreA(void *sa, LONG initial, LONG max, LPCSTR name)
{
    UNUSED(sa);
    UNUSED(max);
    UNUSED(name);
    return new_object(WH_SEMAPHORE, (uint32_t)initial);
}

static HANDLE WINAPI CreateSemaphoreW(void *sa, LONG initial, LONG max, LPCWSTR name)
{
    UNUSED(name);
    return CreateSemaphoreA(sa, initial, max, NULL);
}

static BOOL WINAPI SetEvent(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->value = 1;
    return o != NULL;
}

static BOOL WINAPI ResetEvent(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->value = 0;
    return o != NULL;
}

static BOOL WINAPI ReleaseMutex(HANDLE h)
{
    return w32_handle_get(h) != NULL;
}

static BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, LONG *previous)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return FALSE;
    if (previous)
        *previous = (LONG)o->value;
    o->value += (uint32_t)count;
    return TRUE;
}

static DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms)
{
    UNUSED(h);
    UNUSED(ms);
    return WAIT_OBJECT_0;
}

static DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    return WaitForSingleObject(h, ms);
}

static DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms)
{
    UNUSED(count);
    UNUSED(handles);
    UNUSED(all);
    UNUSED(ms);
    return WAIT_OBJECT_0;
}

static BOOL WINAPI CloseHandle(HANDLE h)
{
    if (h == W32_PSEUDO_PROCESS || h == W32_PSEUDO_THREAD)
        return TRUE;
    if (!w32_handle_close(h)) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit,
                                   DWORD options)
{
    UNUSED(sp);
    UNUSED(tp);
    UNUSED(access);
    UNUSED(inherit);
    struct w32_object *o = w32_handle_get(src);
    if (src == W32_PSEUDO_PROCESS || src == W32_PSEUDO_THREAD) {
        *out = src;
        return TRUE;
    }
    if (!o) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    o->refs++;
    *out = src;
    if (options & 1)
        w32_handle_close(src);
    return TRUE;
}

static HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, void *start, void *param, DWORD flags, DWORD *id)
{
    UNUSED(sa);
    UNUSED(stack);
    UNUSED(start);
    UNUSED(param);
    UNUSED(flags);
    UNUSED(id);
    w32_set_error(ERROR_NOT_SUPPORTED);
    return NULL;
}

static LONG WINAPI InterlockedIncrement(volatile LONG *v)
{
    return ++*v;
}

static LONG WINAPI InterlockedDecrement(volatile LONG *v)
{
    return --*v;
}

static LONG WINAPI InterlockedExchange(volatile LONG *v, LONG value)
{
    LONG old = *v;
    *v = value;
    return old;
}

static LONG WINAPI InterlockedExchangeAdd(volatile LONG *v, LONG value)
{
    LONG old = *v;
    *v += value;
    return old;
}

static LONG WINAPI InterlockedCompareExchange(volatile LONG *v, LONG value, LONG comparand)
{
    LONG old = *v;
    if (old == comparand)
        *v = value;
    return old;
}

static void WINAPI InitializeSListHead(void *head)
{
    memset(head, 0, 8);
}

static void WINAPI SrwNop(void *lock)
{
    UNUSED(lock);
}

static BOOL WINAPI SleepConditionVariableCS(void *cv, void *cs, DWORD ms)
{
    UNUSED(cv);
    UNUSED(cs);
    UNUSED(ms);
    return TRUE;
}

static BOOL WINAPI InitOnceExecuteOnce(uint32_t *once, void *fn, void *param, void **context)
{
    if (*once)
        return TRUE;
    *once = 1;
    typedef BOOL WINAPI (*init_fn)(uint32_t *, void *, void **);
    return ((init_fn)fn)(once, param, context);
}

static BOOL WINAPI SetConsoleCtrlHandler(void *handler, BOOL add)
{
    UNUSED(handler);
    UNUSED(add);
    return TRUE;
}

static BOOL WINAPI GetProcessAffinityMask(HANDLE h, DWORD *process, DWORD *system)
{
    UNUSED(h);
    *process = *system = 1;
    return TRUE;
}

static void *WINAPI EncodePointer(void *p)
{
    return p;
}

static void *WINAPI DecodePointer(void *p)
{
    return p;
}

static UINT WINAPI SetHandleCount(UINT n)
{
    return n;
}

static void build_cmdline(const char *app, const char *cmdline, char *out, size_t cap)
{
    out[0] = 0;
    if (cmdline && *cmdline)
        str_copy(out, cmdline, cap);
    else if (app)
        str_copy(out, app, cap);
}

static int spawn(const char *app, const char *cmdline, DWORD *exit_code)
{
    char line[1024];
    build_cmdline(app, cmdline, line, sizeof(line));
    char *argv[32];
    int argc = 0;
    char *p = line;
    while (*p && argc < 31) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        char *start = p;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            while (*p && *p != ' ')
                p++;
        }
        if (*p)
            *p++ = 0;
        argv[argc++] = start;
    }
    argv[argc] = NULL;
    if (!argc)
        return ERROR_INVALID_PARAMETER;
    char path[PATH_MAX_LEN];
    if (w32_path(app ? app : argv[0], path, sizeof(path)) != 0 || !vfs_exists(path)) {
        if (exec_find(w32->ctx->sh, path_basename(argv[0]), path, sizeof(path)) != 0) {
            char *dot = strrchr(argv[0], '.');
            if (dot && str_casecmp(dot, ".exe") == 0)
                *dot = 0;
            if (exec_find(w32->ctx->sh, argv[0], path, sizeof(path)) != 0)
                return ERROR_FILE_NOT_FOUND;
        }
    }
    struct exec_ctx ctx = {w32->ctx->sh, path, argc, argv, w32->ctx->in, w32->ctx->out, w32->ctx->err};
    char *image;
    size_t size;
    if (vfs_read_all(path, &image, &size) != 0)
        return ERROR_FILE_NOT_FOUND;
    const struct binfmt *fmt = exec_identify(path, (const uint8_t *)image, size);
    bool is_pe = fmt && strcmp(fmt->name, "pe") == 0;
    kfree(image);
    if (!fmt)
        return 193;
    if (is_pe)
        return ERROR_NOT_SUPPORTED;
    int rc = exec_program(&ctx);
    *exit_code = rc < 0 ? 1 : (DWORD)rc;
    return 0;
}

static BOOL create_process(const char *app, const char *cmdline, uint32_t *info)
{
    DWORD code = 0;
    int err = spawn(app, cmdline, &code);
    if (err) {
        w32_set_error((DWORD)err);
        return FALSE;
    }
    HANDLE process = new_object(WH_EVENT, code);
    if (info) {
        info[0] = (uint32_t)process;
        info[1] = (uint32_t)new_object(WH_EVENT, 0);
        info[2] = 0x200;
        info[3] = 0x204;
    }
    return TRUE;
}

static BOOL WINAPI CreateProcessA(LPCSTR app, LPSTR cmdline, void *psa, void *tsa, BOOL inherit, DWORD flags,
                                  void *env, LPCSTR dir, void *startup, uint32_t *info)
{
    UNUSED(psa);
    UNUSED(tsa);
    UNUSED(inherit);
    UNUSED(flags);
    UNUSED(env);
    UNUSED(dir);
    UNUSED(startup);
    return create_process(app, cmdline, info);
}

static BOOL WINAPI CreateProcessW(LPCWSTR app, LPWSTR cmdline, void *psa, void *tsa, BOOL inherit, DWORD flags,
                                  void *env, LPCWSTR dir, void *startup, uint32_t *info)
{
    UNUSED(psa);
    UNUSED(tsa);
    UNUSED(inherit);
    UNUSED(flags);
    UNUSED(env);
    UNUSED(dir);
    UNUSED(startup);
    char *a = w32_narrow(app), *c = w32_narrow(cmdline);
    BOOL ok = create_process(a, c, info);
    w32_free(a);
    w32_free(c);
    return ok;
}

static BOOL WINAPI GetExitCodeProcess(HANDLE h, DWORD *code)
{
    struct w32_object *o = w32_handle_get(h);
    if (h == W32_PSEUDO_PROCESS) {
        *code = STILL_ACTIVE;
        return TRUE;
    }
    if (!o)
        return FALSE;
    *code = o->value;
    return TRUE;
}

static UINT WINAPI WinExec(LPCSTR cmdline, UINT show)
{
    UNUSED(show);
    DWORD code;
    return spawn(NULL, cmdline, &code) == 0 ? 33 : 2;
}

static DWORD format_message(DWORD flags, DWORD id, char *out, size_t cap)
{
    UNUSED(flags);
    static const struct {
        DWORD id;
        const char *text;
    } messages[] = {
        {0, "The operation completed successfully."},
        {2, "The system cannot find the file specified."},
        {3, "The system cannot find the path specified."},
        {5, "Access is denied."},
        {6, "The handle is invalid."},
        {8, "Not enough memory resources are available to process this command."},
        {80, "The file exists."},
        {87, "The parameter is incorrect."},
        {120, "This function is not supported on this system."},
        {126, "The specified module could not be found."},
        {127, "The specified procedure could not be found."},
        {183, "Cannot create a file when that file already exists."},
    };
    const char *text = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(messages); i++)
        if (messages[i].id == id)
            text = messages[i].text;
    char buf[96];
    if (!text) {
        ksnprintf(buf, sizeof(buf), "Error %u.", id);
        text = buf;
    }
    size_t n = (size_t)ksnprintf(out, cap, "%s\r\n", text);
    return (DWORD)MIN(n, cap - 1);
}

static DWORD WINAPI FormatMessageA(DWORD flags, const void *source, DWORD id, DWORD lang, LPSTR buf, DWORD size,
                                   va_list *args)
{
    UNUSED(source);
    UNUSED(lang);
    UNUSED(args);
    char tmp[256];
    DWORD n = format_message(flags, id, tmp, sizeof(tmp));
    if (flags & 0x100) {
        char *mem = w32_alloc(n + 1);
        memcpy(mem, tmp, n + 1);
        *(char **)buf = mem;
        return n;
    }
    if (!size)
        return 0;
    str_copy(buf, tmp, size);
    return MIN(n, size - 1);
}

static DWORD WINAPI FormatMessageW(DWORD flags, const void *source, DWORD id, DWORD lang, LPWSTR buf, DWORD size,
                                   va_list *args)
{
    UNUSED(source);
    UNUSED(lang);
    UNUSED(args);
    char tmp[256];
    DWORD n = format_message(flags, id, tmp, sizeof(tmp));
    if (flags & 0x100) {
        WCHAR *mem = w32_widen(tmp);
        *(WCHAR **)buf = mem;
        return n;
    }
    if (!size)
        return 0;
    size_t w = w32_cp_to_wide(tmp, (int)n, buf, size - 1, 1);
    buf[MIN(w, (size_t)size - 1)] = 0;
    return (DWORD)MIN(w, (size_t)size - 1);
}

static void *WINAPI LocalFree(void *mem)
{
    w32_free(mem);
    return NULL;
}

static void *WINAPI LocalAlloc(UINT flags, SIZE_T size)
{
    return GlobalAlloc(flags, size);
}

static void *WINAPI LocalReAlloc(void *mem, SIZE_T size, UINT flags)
{
    return GlobalReAlloc(mem, size, flags);
}

static const struct w32_export kernel32_core[] = {
    W32_FN(ExitProcess), W32_FN(TerminateProcess), W32_FN(GetCurrentProcess), W32_FN(GetCurrentThread),
    W32_FN(GetCurrentProcessId), W32_FN(GetCurrentThreadId), W32_FN(GetLastError), W32_FN(SetLastError),
    W32_FN(SetErrorMode), W32_FN(GetErrorMode), W32_FN(GetModuleHandleA), W32_FN(GetModuleHandleW),
    W32_FN(GetModuleHandleExA), W32_FN(GetModuleHandleExW), W32_FN(GetModuleFileNameA),
    W32_FN(GetModuleFileNameW), W32_FN(LoadLibraryA), W32_FN(LoadLibraryW), W32_FN(LoadLibraryExA),
    W32_FN(LoadLibraryExW), W32_FN(FreeLibrary), W32_FN(GetProcAddress), W32_FN(GetCommandLineA),
    W32_FN(GetCommandLineW), W32_FN(GetStartupInfoA), W32_FN(GetStartupInfoW), W32_FN(GetVersion),
    W32_FN(GetVersionExA), W32_FN(GetVersionExW), W32_FN(IsProcessorFeaturePresent), W32_FN(GetSystemInfo),
    W32_AS("GetNativeSystemInfo", GetSystemInfo), W32_FN(IsWow64Process), W32_FN(SetUnhandledExceptionFilter),
    W32_FN(UnhandledExceptionFilter), W32_FN(RaiseException), W32_FN(IsDebuggerPresent),
    W32_FN(OutputDebugStringA), W32_FN(OutputDebugStringW), W32_FN(DebugBreak), W32_FN(Sleep), W32_FN(SleepEx),
    W32_FN(GetTickCount), W32_FN(GetTickCount64), W32_FN(QueryPerformanceCounter),
    W32_FN(QueryPerformanceFrequency), W32_FN(GetSystemTimeAsFileTime), W32_FN(GetSystemTimePreciseAsFileTime),
    W32_FN(GetSystemTime), W32_FN(GetLocalTime), W32_FN(FileTimeToSystemTime), W32_FN(SystemTimeToFileTime),
    W32_FN(FileTimeToLocalFileTime), W32_FN(LocalFileTimeToFileTime), W32_FN(SystemTimeToTzSpecificLocalTime),
    W32_FN(GetTimeZoneInformation), W32_FN(GetComputerNameA), W32_FN(GetComputerNameW), W32_FN(Beep),
    W32_FN(VirtualAlloc), W32_FN(VirtualFree), W32_FN(VirtualProtect), W32_FN(VirtualQuery),
    W32_FN(GetProcessHeap), W32_FN(HeapCreate), W32_FN(HeapDestroy), W32_FN(HeapAlloc), W32_FN(HeapReAlloc),
    W32_FN(HeapFree), W32_FN(HeapSize), W32_FN(HeapValidate), W32_FN(HeapSetInformation), W32_FN(GlobalAlloc),
    W32_FN(GlobalReAlloc), W32_FN(GlobalFree), W32_FN(GlobalLock), W32_FN(GlobalUnlock), W32_FN(GlobalSize),
    W32_FN(GlobalMemoryStatus), W32_FN(GlobalMemoryStatusEx), W32_FN(LocalAlloc), W32_FN(LocalReAlloc),
    W32_FN(LocalFree), W32_AS("LocalLock", GlobalLock), W32_AS("LocalUnlock", GlobalUnlock),
    W32_AS("LocalSize", GlobalSize), W32_FN(TlsAlloc), W32_FN(TlsFree), W32_FN(TlsGetValue), W32_FN(TlsSetValue),
    W32_FN(FlsAlloc), W32_FN(FlsFree), W32_FN(FlsGetValue), W32_FN(FlsSetValue),
    W32_AS("InitializeCriticalSection", CriticalSectionNop), W32_AS("EnterCriticalSection", CriticalSectionNop),
    W32_AS("LeaveCriticalSection", CriticalSectionNop), W32_AS("DeleteCriticalSection", CriticalSectionNop),
    W32_FN(InitializeCriticalSectionAndSpinCount), W32_FN(InitializeCriticalSectionEx),
    W32_FN(TryEnterCriticalSection), W32_FN(CreateEventA), W32_FN(CreateEventW), W32_FN(CreateMutexA),
    W32_FN(CreateMutexW), W32_FN(CreateSemaphoreA), W32_FN(CreateSemaphoreW), W32_FN(SetEvent),
    W32_FN(ResetEvent), W32_FN(ReleaseMutex), W32_FN(ReleaseSemaphore), W32_FN(WaitForSingleObject),
    W32_FN(WaitForSingleObjectEx), W32_FN(WaitForMultipleObjects), W32_FN(CloseHandle), W32_FN(DuplicateHandle),
    W32_FN(CreateThread), W32_FN(InterlockedIncrement), W32_FN(InterlockedDecrement), W32_FN(InterlockedExchange),
    W32_FN(InterlockedExchangeAdd), W32_FN(InterlockedCompareExchange), W32_FN(InitializeSListHead),
    W32_AS("InitializeSRWLock", SrwNop), W32_AS("AcquireSRWLockExclusive", SrwNop),
    W32_AS("ReleaseSRWLockExclusive", SrwNop), W32_AS("AcquireSRWLockShared", SrwNop),
    W32_AS("ReleaseSRWLockShared", SrwNop), W32_AS("InitializeConditionVariable", SrwNop),
    W32_AS("WakeConditionVariable", SrwNop), W32_AS("WakeAllConditionVariable", SrwNop),
    W32_FN(SleepConditionVariableCS), W32_FN(InitOnceExecuteOnce), W32_FN(SetConsoleCtrlHandler),
    W32_FN(GetProcessAffinityMask), W32_FN(EncodePointer), W32_FN(DecodePointer),
    W32_AS("EncodeSystemPointer", EncodePointer), W32_AS("DecodeSystemPointer", DecodePointer),
    W32_FN(SetHandleCount), W32_FN(CreateProcessA), W32_FN(CreateProcessW), W32_FN(GetExitCodeProcess),
    W32_FN(WinExec), W32_FN(FormatMessageA), W32_FN(FormatMessageW),
};

W32_EXPORTS(kernel32_core_module, "kernel32.dll", kernel32_core);
