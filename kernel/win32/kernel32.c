#include <synth/arch_win32.h>
#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vmm.h>

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

static W32_STATE void *unhandled_filter;
static W32_STATE uint32_t error_mode;

static void WINAPI ExitProcess(UINT code)
{
    w32_exit(code);
}

static BOOL WINAPI TerminateProcess(HANDLE process, UINT code)
{
    if (process == W32_PSEUDO_PROCESS)
        w32_exit(code);
    struct w32_object *o = w32_handle_get(process);
    if (o && o->type == WH_PROCESS && o->child) {
        w32_terminate_child(o->child, code);
        return TRUE;
    }
    if (o && o->type == WH_PROCESS)
        return TRUE;
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
    return w32_current_pid();
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
    static const char *const always[] = {"ntdll", "ntdll.dll", "kernel32", "kernel32.dll", "kernelbase.dll"};
    for (size_t i = 0; !img && i < ARRAY_SIZE(always); i++)
        if (!str_casecmp(name, always[i])) {
            int err = 0;
            img = w32_load(always[i][strlen(always[i]) - 4] == '.' ? always[i] : always[i + 1], NULL, &err);
        }
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
        klog("win32: LoadLibrary(%s) failed with %d%s%s", name, err, w32->message[0] ? ": " : "", w32->message);
        w32->message[0] = 0;
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
    if (img->builtin && !ordinal)
        w32_trace_note(img->name, name);
    if (!fn) {
        if (img->builtin) {
            if (ordinal)
                klog("win32: GetProcAddress(%s, #%u) not found", img->name, (uint32_t)name);
            else
                klog("win32: GetProcAddress(%s, %s) not found", img->name, name);
        }
        w32_set_error(ERROR_PROC_NOT_FOUND);
    }
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

#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING 0x2
#define EXCEPTION_EXIT_UNWIND 0x4
#define SEH_END 0xffffffffu

struct seh_frame {
    struct seh_frame *prev;
    void *handler;
};

typedef int (*seh_handler)(uint8_t *record, struct seh_frame *frame, uint8_t *context, void *dispatcher);

static struct seh_frame **seh_head(void)
{
    return (struct seh_frame **)w32->teb;
}

static bool seh_valid(struct seh_frame *f)
{
    return (uintptr_t)f != SEH_END && f && (uintptr_t)f >= VMM_USER_START && (uintptr_t)f < VMM_USER_END;
}

static void build_record(uint8_t *rec, DWORD code, DWORD flags, DWORD argc, const ULONG_PTR *args, void *addr)
{
    memset(rec, 0, 80);
    memcpy(rec, &code, 4);
    memcpy(rec + 4, &flags, 4);
    memcpy(rec + 12, &addr, 4);
    argc = MIN(argc, 15u);
    memcpy(rec + 16, &argc, 4);
    if (args)
        memcpy(rec + 20, args, argc * 4);
}

static NORETURN void unhandled(uint8_t *rec, uint8_t *ctx)
{
    DWORD code;
    memcpy(&code, rec, 4);
    if (unhandled_filter) {
        uint32_t ptrs[2] = {(uint32_t)(uintptr_t)rec, (uint32_t)(uintptr_t)ctx};
        uint32_t arg = (uint32_t)(uintptr_t)ptrs;
        w32_call(unhandled_filter, 1, &arg);
    }
    if (!w32->message[0])
        ksnprintf(w32->message, sizeof(w32->message), "unhandled exception 0x%08x", code);
    w32_exit(code);
}

static void dispatch_exception(uint8_t *rec, uint8_t *ctx)
{
    for (struct seh_frame *f = *seh_head(); seh_valid(f); f = f->prev) {
        int r = ((seh_handler)f->handler)(rec, f, ctx, NULL);
        DWORD flags;
        memcpy(&flags, rec + 4, 4);
        if (r == 0 && !(flags & EXCEPTION_NONCONTINUABLE))
            return;
    }
    unhandled(rec, ctx);
}

static void WINAPI RaiseException(DWORD code, DWORD flags, DWORD argc, const ULONG_PTR *args)
{
    uint8_t rec[80];
    uint8_t *ctx = w32_zalloc(716);
    build_record(rec, code, flags & EXCEPTION_NONCONTINUABLE, argc, args, __builtin_return_address(0));
    if (ctx)
        memcpy(ctx, &(DWORD){0x1003f}, 4);
    dispatch_exception(rec, ctx);
    w32_free(ctx);
}

static void WINAPI RtlUnwind(struct seh_frame *target, void *ip, uint8_t *rec, void *retval)
{
    UNUSED(ip);
    UNUSED(retval);
    uint8_t local[80];
    if (!rec) {
        build_record(local, 0xc0000027u, 0, 0, NULL, __builtin_return_address(0));
        rec = local;
    }
    DWORD flags;
    memcpy(&flags, rec + 4, 4);
    flags |= target ? EXCEPTION_UNWINDING : EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND;
    memcpy(rec + 4, &flags, 4);
    struct seh_frame **head = seh_head();
    while (seh_valid(*head) && *head != target) {
        struct seh_frame *f = *head;
        ((seh_handler)f->handler)(rec, f, NULL, NULL);
        *head = f->prev;
    }
}

static void WINAPI RtlCaptureContext(uint8_t *ctx)
{
    memset(ctx, 0, 716);
    memcpy(ctx, &(DWORD){0x1003f}, 4);
}


static DWORD WINAPI GetProcessId(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    return o && o->type == WH_PROCESS && o->max ? o->max : GetCurrentProcessId();
}

static void *WINAPI InterlockedPushEntrySList(uint32_t *head, uint32_t *entry)
{
    void *old = (void *)(uintptr_t)head[0];
    entry[0] = head[0];
    head[0] = (uint32_t)(uintptr_t)entry;
    head[1]++;
    return old;
}

static void *WINAPI InterlockedPopEntrySList(uint32_t *head)
{
    uint32_t *first = (uint32_t *)(uintptr_t)head[0];
    if (first) {
        head[0] = first[0];
        head[1]--;
    }
    return first;
}

static void *WINAPI InterlockedFlushSList(uint32_t *head)
{
    void *first = (void *)(uintptr_t)head[0];
    head[0] = 0;
    head[1] &= 0xffff0000u;
    return first;
}

static WORD WINAPI QueryDepthSList(uint32_t *head)
{
    return (WORD)head[1];
}

static void *WINAPI RtlPcToFileHeader(void *pc, void **base)
{
    *base = NULL;
    for (struct w32_image *img = w32->images; img; img = img->next)
        if ((uintptr_t)pc >= img->base && (uintptr_t)pc < img->base + img->size)
            *base = (void *)(uintptr_t)img->base;
    return *base;
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

static struct w32_process *process_of(HANDLE h)
{
    if (h == W32_PSEUDO_PROCESS)
        return w32;
    struct w32_object *o = w32_handle_get(h);
    return o && o->type == WH_PROCESS && o->child ? o->child->proc : NULL;
}

static BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit,
                                   DWORD options)
{
    struct w32_process *from = process_of(sp), *to = tp ? process_of(tp) : NULL;
    if (!from || (tp && !to)) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (from == w32 && (src == W32_PSEUDO_PROCESS || src == W32_PSEUDO_THREAD)) {
        if (out)
            *out = src;
        return TRUE;
    }
    struct w32_object *o = w32_handle_get_in(from, src);
    if (!o) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (o->type == WH_SECTION && !(options & 2) && o->access && (access & ~o->access & 0x2fu)) {
        w32_set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    HANDLE h = NULL;
    if (to) {
        bool shared_state = o->type == WH_EVENT || o->type == WH_MUTEX || o->type == WH_SEMAPHORE ||
                            o->type == WH_TIMER || o->type == WH_THREAD || o->type == WH_PORT;
        if (to == from && shared_state) {
            o->refs++;
            h = src;
        } else {
            h = w32_handle_put(to, o);
            if (!h) {
                w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
                return FALSE;
            }
            struct w32_object *n = w32_handle_get_in(to, h);
            n->inherit = inherit != 0;
            n->access = (options & 2) ? o->access : access;
        }
    }
    if (options & 1 && h != src && --o->refs <= 0)
        w32_object_release(o);
    if (out)
        *out = h;
    return TRUE;
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

struct spawn_args {
    char *app;
    char *cmdline;
    char *env;
    char *cwd;
    DWORD exit_code;
    struct stream *in, *out, *err;
    struct w32_child *child;
    struct w32_inherit *handles;
};

bool w32_is_pe64(const uint8_t *file, size_t size);

static bool async_target(const char *path)
{
    if (w32->win64)
        return false;
    struct file *f;
    if (vfs_open(path, VFS_READ, &f) != 0)
        return false;
    uint8_t *head = kmalloc(4096);
    long n = head ? vfs_read(f, head, 4096) : -1;
    vfs_close(f);
    bool ok = n >= 64 && head[0] == 'M' && head[1] == 'Z' && !w32_is_pe64(head, (size_t)n);
    kfree(head);
    return ok;
}

static bool spawn_async(struct spawn_args *a, const char *path, int argc, char **argv)
{
    struct exec_ctx *c = kzalloc(sizeof(*c));
    char **copy = kzalloc(sizeof(char *) * (size_t)(argc + 1));
    char *p = kstrdup(path);
    if (!c || !copy || !p) {
        kfree(c);
        kfree(copy);
        kfree(p);
        return false;
    }
    for (int i = 0; i < argc; i++)
        copy[i] = kstrdup(argv[i]);
    *c = (struct exec_ctx){w32->ctx->sh, p, argc, copy, a->in ? a->in : w32->ctx->in, a->out ? a->out : w32->ctx->out,
                           a->err ? a->err : w32->ctx->err, a->env, a->cwd, a->handles};
    unsigned own = (a->in ? 1u : 0) | (a->out ? 2u : 0) | (a->err ? 4u : 0);
    struct w32_child *child = w32_spawn_async(c, own);
    if (!child) {
        for (int i = 0; i < argc; i++)
            kfree(copy[i]);
        kfree(copy);
        kfree(p);
        kfree(c);
        return false;
    }
    a->child = child;
    a->handles = NULL;
    a->in = a->out = a->err = NULL;
    a->env = a->cwd = NULL;
    return true;
}

static int run_cmd(struct spawn_args *a, const char *rest)
{
    struct shell *sh = w32->ctx->sh;
    struct stream *in = sh->in, *out = sh->out, *err = sh->err;
    char saved[PATH_MAX_LEN];
    str_copy(saved, sh->cwd, sizeof(saved));
    str_copy(sh->cwd, w32->cwd, sizeof(sh->cwd));
    if (a->in)
        sh->in = a->in;
    if (a->out)
        sh->out = a->out;
    if (a->err)
        sh->err = a->err;
    int rc = shell_exec(sh, rest);
    sh->in = in;
    sh->out = out;
    sh->err = err;
    str_copy(sh->cwd, saved, sizeof(sh->cwd));
    a->exit_code = rc < 0 ? 1 : (DWORD)rc;
    return 0;
}

static int split_args(const char *line, char *buf, char **argv, int max)
{
    int argc = 0;
    const char *p = line;
    char *o = buf;
    while (argc < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[argc++] = o;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            size_t slashes = 0;
            while (*p == '\\') {
                slashes++;
                p++;
            }
            if (*p == '"') {
                for (size_t i = 0; i < slashes / 2; i++)
                    *o++ = '\\';
                if (slashes % 2) {
                    *o++ = '"';
                } else if (quoted && p[1] == '"') {
                    *o++ = '"';
                    p++;
                } else {
                    quoted = !quoted;
                }
                p++;
            } else {
                for (size_t i = 0; i < slashes; i++)
                    *o++ = '\\';
                if (*p)
                    *o++ = *p++;
            }
        }
        *o++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

static int spawn_args_run(struct spawn_args *a, int argc, char **argv);

static int spawn_now(struct spawn_args *a)
{
    const char *line = a->cmdline && *a->cmdline ? a->cmdline : a->app ? a->app : "";
    char *buf = kmalloc(strlen(line) + 2);
    char **argv = kmalloc(256 * sizeof(char *));
    int rc = ERROR_NOT_ENOUGH_MEMORY;
    if (buf && argv) {
        int argc = split_args(line, buf, argv, 255);
        rc = argc ? spawn_args_run(a, argc, argv) : ERROR_INVALID_PARAMETER;
    }
    kfree(buf);
    kfree(argv);
    return rc;
}

static int spawn_args_run(struct spawn_args *a, int argc, char **argv)
{
    const char *app = a->app;
    DWORD *exit_code = &a->exit_code;
    const char *base = path_basename(argv[0]);
    if ((!str_casecmp(base, "cmd") || !str_casecmp(base, "cmd.exe")) && argc > 1 &&
        (!str_casecmp(argv[1], "/c") || !str_casecmp(argv[1], "/k"))) {
        char rest[1024] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(rest, " ", sizeof(rest));
            str_append(rest, argv[i], sizeof(rest));
        }
        return run_cmd(a, rest);
    }
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
    if (async_target(path) && spawn_async(a, path, argc, argv))
        return 0;
    struct exec_ctx ctx = {w32->ctx->sh, path, argc, argv, a->in ? a->in : w32->ctx->in,
                           a->out ? a->out : w32->ctx->out, a->err ? a->err : w32->ctx->err, a->env, a->cwd,
                           a->handles};
    a->handles = NULL;
    char *image;
    size_t size;
    if (vfs_read_all(path, &image, &size) != 0)
        return ERROR_FILE_NOT_FOUND;
    const struct binfmt *fmt = exec_identify(path, (const uint8_t *)image, size);
    kfree(image);
    if (!fmt)
        return 193;
    int rc = exec_program(&ctx);
    *exit_code = rc < 0 ? 1 : (DWORD)rc;
    return 0;
}

static uint32_t spawn_thunk(void *arg)
{
    return (uint32_t)spawn_now(arg);
}

static char *copy_env_block(const void *env, bool wide)
{
    if (!env)
        env = w32->env_block;
    if (!env)
        return NULL;
    size_t len = 0;
    char *out;
    if (wide) {
        const WCHAR *w = env;
        size_t wl = 0;
        while (w[wl] || w[wl + 1])
            wl++;
        len = w32_wide_to_cp(w, (int)wl + 2, NULL, 0, 65001);
        out = kzalloc(len + 2);
        if (out)
            w32_wide_to_cp(w, (int)wl + 2, out, len, 65001);
        return out;
    }
    const char *s = env;
    while (s[len] || s[len + 1])
        len++;
    out = kzalloc(len + 3);
    if (out)
        memcpy(out, s, len + 2);
    return out;
}

static int spawn(const char *app, const char *cmdline, const uint32_t *startup, DWORD *exit_code,
                 struct w32_child **child, const void *env, bool wide_env, const char *dir, struct w32_inherit *handles)
{
    struct spawn_args *a = kzalloc(sizeof(*a));
    if (!a)
        return ERROR_NOT_ENOUGH_MEMORY;
    const char *shown = cmdline ? cmdline : app ? app : "?";
    for (size_t off = 0, len = strlen(shown); off < len; off += 160)
        klog("win32: %s %.160s", off ? "  ..." : "CreateProcess", shown + off);
    a->app = app ? kstrdup(app) : NULL;
    a->cmdline = cmdline ? kstrdup(cmdline) : NULL;
    a->env = copy_env_block(env, wide_env);
    if (dir) {
        char path[PATH_MAX_LEN];
        if (w32_path(dir, path, sizeof(path)) == 0)
            a->cwd = kstrdup(path);
    } else {
        a->cwd = kstrdup(w32->cwd);
    }
    if (startup && (startup[11] & 0x100)) {
        a->in = w32_handle_stream((HANDLE)startup[14]);
        a->out = w32_handle_stream((HANDLE)startup[15]);
        a->err = w32_handle_stream((HANDLE)startup[16]);
    }
    a->handles = handles;
    int err = (int)w32_kernel_call(spawn_thunk, a);
    w32_inherit_free(a->handles);
    *exit_code = a->exit_code;
    *child = a->child;
    if (a->in)
        stream_close(a->in);
    if (a->out)
        stream_close(a->out);
    if (a->err)
        stream_close(a->err);
    kfree(a->app);
    kfree(a->cmdline);
    kfree(a->env);
    kfree(a->cwd);
    kfree(a);
    return err;
}

static HANDLE new_object(enum w32_object_type type, uint32_t value)
{
    HANDLE h = w32_handle_new(type);
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->value = value;
    return h;
}

static BOOL create_process(const char *app, const char *cmdline, const void *startup, uint32_t *info, const void *env,
                           bool wide_env, const char *dir, BOOL inherit, DWORD flags)
{
    DWORD code = 0;
    struct w32_child *child = NULL;
    struct w32_inherit *handles = NULL;
    if (inherit) {
        int count = 0;
        const HANDLE *list = (flags & 0x80000) && startup ? w32_attribute_handles(((void *const *)startup)[17], &count)
                                                          : NULL;
        handles = w32_inherit_collect(list, count);
    }
    int err = spawn(app, cmdline, startup, &code, &child, env, wide_env, dir, handles);
    if (err) {
        w32_set_error((DWORD)err);
        return FALSE;
    }
    static W32_STATE uint32_t next_pid;
    next_pid = MAX(next_pid, 0x2000u) + 8;
    if (child)
        next_pid = child->pid;
    HANDLE process = new_object(WH_PROCESS, code);
    struct w32_object *o = w32_handle_get(process);
    if (o) {
        o->max = next_pid;
        o->child = child;
    } else {
        w32_child_release(child);
    }
    if (info) {
        info[0] = (uint32_t)process;
        info[1] = (uint32_t)new_object(WH_PROCESS, 0);
        info[2] = next_pid;
        info[3] = next_pid + 4;
    } else {
        w32_handle_close(process);
    }
    return TRUE;
}

static BOOL WINAPI CreateProcessA(LPCSTR app, LPSTR cmdline, void *psa, void *tsa, BOOL inherit, DWORD flags,
                                  void *env, LPCSTR dir, void *startup, uint32_t *info)
{
    UNUSED(psa);
    UNUSED(tsa);
    return create_process(app, cmdline, startup, info, env, (flags & 0x400) != 0, dir, inherit, flags);
}

static BOOL WINAPI CreateProcessW(LPCWSTR app, LPWSTR cmdline, void *psa, void *tsa, BOOL inherit, DWORD flags,
                                  void *env, LPCWSTR dir, void *startup, uint32_t *info)
{
    UNUSED(psa);
    UNUSED(tsa);
    char *a = w32_narrow(app), *c = w32_narrow(cmdline), *d = dir ? w32_narrow(dir) : NULL;
    BOOL ok = create_process(a, c, startup, info, env, (flags & 0x400) != 0, d, inherit, flags);
    w32_free(a);
    w32_free(c);
    w32_free(d);
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
    *code = o->child ? (o->child->done ? o->child->exit_code : STILL_ACTIVE) : o->value;
    return TRUE;
}

static UINT WINAPI WinExec(LPCSTR cmdline, UINT show)
{
    UNUSED(show);
    DWORD code;
    struct w32_child *child = NULL;
    int err = spawn(NULL, cmdline, NULL, &code, &child, NULL, false, NULL, NULL);
    w32_child_release(child);
    return err == 0 ? 33 : 2;
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
    W32_FN(GetCurrentProcessId), W32_FN(GetLastError), W32_FN(SetLastError),
    W32_FN(SetErrorMode), W32_FN(GetErrorMode), W32_FN(GetModuleHandleA), W32_FN(GetModuleHandleW),
    W32_FN(GetModuleHandleExA), W32_FN(GetModuleHandleExW), W32_FN(GetModuleFileNameA),
    W32_FN(GetModuleFileNameW), W32_FN(LoadLibraryA), W32_FN(LoadLibraryW), W32_FN(LoadLibraryExA),
    W32_FN(LoadLibraryExW), W32_FN(FreeLibrary), W32_FN(GetProcAddress), W32_FN(GetCommandLineA),
    W32_FN(GetCommandLineW), W32_FN(GetStartupInfoA), W32_FN(GetStartupInfoW), W32_FN(GetVersion),
    W32_FN(GetVersionExA), W32_FN(GetVersionExW), W32_FN(IsProcessorFeaturePresent), W32_FN(GetSystemInfo),
    W32_AS("GetNativeSystemInfo", GetSystemInfo), W32_FN(IsWow64Process), W32_FN(SetUnhandledExceptionFilter),
    W32_FN(UnhandledExceptionFilter), W32_FN(RaiseException), W32_FN(RtlUnwind), W32_FN(RtlCaptureContext),
    W32_FN(GetProcessId), W32_FN(InterlockedPushEntrySList), W32_FN(InterlockedPopEntrySList),
    W32_FN(InterlockedFlushSList), W32_FN(QueryDepthSList), W32_FN(RtlPcToFileHeader), W32_FN(IsDebuggerPresent),
    W32_FN(OutputDebugStringA), W32_FN(OutputDebugStringW), W32_FN(DebugBreak), W32_FN(GetTickCount), W32_FN(GetTickCount64), W32_FN(QueryPerformanceCounter),
    W32_FN(QueryPerformanceFrequency), W32_FN(GetSystemTimeAsFileTime), W32_FN(GetSystemTimePreciseAsFileTime),
    W32_FN(GetSystemTime), W32_FN(GetLocalTime), W32_FN(FileTimeToSystemTime), W32_FN(SystemTimeToFileTime),
    W32_FN(FileTimeToLocalFileTime), W32_FN(LocalFileTimeToFileTime), W32_FN(SystemTimeToTzSpecificLocalTime),
    W32_FN(GetTimeZoneInformation), W32_FN(GetComputerNameA), W32_FN(GetComputerNameW), W32_FN(Beep),
    W32_FN(GetProcessHeap), W32_FN(HeapCreate), W32_FN(HeapDestroy), W32_FN(HeapAlloc), W32_FN(HeapReAlloc),
    W32_FN(HeapFree), W32_FN(HeapSize), W32_FN(HeapValidate), W32_FN(HeapSetInformation), W32_FN(GlobalAlloc),
    W32_FN(GlobalReAlloc), W32_FN(GlobalFree), W32_FN(GlobalLock), W32_FN(GlobalUnlock), W32_FN(GlobalSize),
    W32_FN(GlobalMemoryStatus), W32_FN(GlobalMemoryStatusEx), W32_FN(LocalAlloc), W32_FN(LocalReAlloc),
    W32_FN(LocalFree), W32_AS("LocalLock", GlobalLock), W32_AS("LocalUnlock", GlobalUnlock),
    W32_AS("LocalSize", GlobalSize), W32_FN(TlsAlloc), W32_FN(TlsFree), W32_FN(TlsGetValue), W32_FN(TlsSetValue),
    W32_FN(CloseHandle), W32_FN(DuplicateHandle),
    W32_FN(InterlockedIncrement), W32_FN(InterlockedDecrement), W32_FN(InterlockedExchange),
    W32_FN(InterlockedExchangeAdd), W32_FN(InterlockedCompareExchange), W32_FN(InitializeSListHead),
    W32_FN(SetConsoleCtrlHandler),
    W32_FN(GetProcessAffinityMask), W32_FN(EncodePointer), W32_FN(DecodePointer),
    W32_AS("EncodeSystemPointer", EncodePointer), W32_AS("DecodeSystemPointer", DecodePointer),
    W32_FN(SetHandleCount), W32_FN(CreateProcessA), W32_FN(CreateProcessW), W32_FN(GetExitCodeProcess),
    W32_FN(WinExec), W32_FN(FormatMessageA), W32_FN(FormatMessageW),
};

static void kernel32_attach(void)
{
    unhandled_filter = NULL;
    error_mode = 0;
}

W32_EXPORTS(kernel32_core_module, "kernel32.dll", kernel32_core, .attach = kernel32_attach);
