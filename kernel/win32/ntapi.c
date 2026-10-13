#include <synth/hal.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

#define STATUS_SUCCESS 0u
#define STATUS_INVALID_INFO_CLASS 0xc0000003u
#define STATUS_INVALID_HANDLE 0xc0000008u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034u
#define STATUS_NO_MEMORY 0xc0000017u
#define ERROR_INSUFFICIENT_BUFFER 122

struct unicode_string {
    uint16_t length, max_length;
    WCHAR *buffer;
};

static void WINAPI RtlInitUnicodeString(struct unicode_string *s, const WCHAR *text)
{
    size_t n = text ? w32_wcslen(text) : 0;
    s->length = (uint16_t)MIN(n * 2, 0xfffcu);
    s->max_length = text ? (uint16_t)(s->length + 2) : 0;
    s->buffer = (WCHAR *)text;
}

static void WINAPI RtlFreeUnicodeString(struct unicode_string *s)
{
    if (s && s->buffer)
        w32_free(s->buffer);
    if (s)
        *s = (struct unicode_string){0, 0, NULL};
}

static DWORD WINAPI RtlFormatCurrentUserKeyPath(struct unicode_string *out)
{
    static const char path[] = "\\Registry\\User\\S-1-5-21-1000-2000-3000-1001";
    WCHAR *w = w32_widen(path);
    if (!w)
        return STATUS_NO_MEMORY;
    out->buffer = w;
    out->length = (uint16_t)((sizeof(path) - 1) * 2);
    out->max_length = (uint16_t)(out->length + 2);
    return STATUS_SUCCESS;
}

static DWORD WINAPI RtlGetLastNtStatus(void)
{
    return 0;
}

static DWORD WINAPI NtClose(HANDLE h)
{
    return w32_handle_close(h) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static DWORD WINAPI NtOpenKeyEx(HANDLE *out, DWORD access, void *attributes, DWORD options)
{
    UNUSED(access);
    UNUSED(attributes);
    UNUSED(options);
    if (out)
        *out = NULL;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static DWORD WINAPI NtOpenKey(HANDLE *out, DWORD access, void *attributes)
{
    return NtOpenKeyEx(out, access, attributes, 0);
}

static DWORD WINAPI NtCreateKey(HANDLE *out, DWORD access, void *attributes, DWORD index, void *cls, DWORD options,
                                DWORD *disposition)
{
    UNUSED(access);
    UNUSED(attributes);
    UNUSED(index);
    UNUSED(cls);
    UNUSED(options);
    *out = w32_handle_new(WH_KEY);
    if (disposition)
        *disposition = 1;
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

static DWORD WINAPI NtQueryValueKey(HANDLE key, void *name, int cls, void *info, DWORD len, DWORD *result)
{
    UNUSED(key);
    UNUSED(name);
    UNUSED(cls);
    UNUSED(info);
    UNUSED(len);
    if (result)
        *result = 0;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static DWORD WINAPI NtSetValueKey(HANDLE key, void *name, DWORD index, DWORD type, const void *data, DWORD size)
{
    UNUSED(key);
    UNUSED(name);
    UNUSED(index);
    UNUSED(type);
    UNUSED(data);
    UNUSED(size);
    return STATUS_SUCCESS;
}

static DWORD WINAPI NtQueryInformationProcess(HANDLE process, int cls, void *info, DWORD len, DWORD *result)
{
    UNUSED(process);
    if (cls == 0 && len >= 24) {
        uint32_t *p = info;
        memset(p, 0, 24);
        p[0] = 0x103;
        p[1] = (uint32_t)w32->peb;
        p[2] = 1;
        p[3] = 8;
        p[4] = 0x100;
        if (result)
            *result = 24;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_INFO_CLASS;
}

static BOOL WINAPI GetLogicalProcessorInformation(uint32_t *buf, DWORD *len)
{
    if (!len)
        return FALSE;
    if (!buf || *len < 24) {
        *len = 24;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, 24);
    buf[0] = 1;
    buf[1] = 0;
    *len = 24;
    return TRUE;
}

static BOOL WINAPI GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, DWORD *type)
{
    UNUSED(major);
    UNUSED(minor);
    UNUSED(sp_major);
    UNUSED(sp_minor);
    *type = 0x30;
    return TRUE;
}

static BOOL WINAPI K32GetModuleInformation(HANDLE process, HANDLE module, uint32_t *info, DWORD size)
{
    UNUSED(process);
    if (size < 12)
        return FALSE;
    uint32_t base = module ? (uint32_t)module : w32->main->base;
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (img->base == base) {
            info[0] = img->base;
            info[1] = img->size;
            info[2] = img->entry;
            return TRUE;
        }
    w32_set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

static BOOL WINAPI QueryThreadCycleTime(HANDLE thread, uint64_t *cycles)
{
    UNUSED(thread);
    *cycles = hal_ticks_ms() * 1000000ull;
    return TRUE;
}

static BOOL WINAPI copy_memory(HANDLE process, void *a, const void *b, SIZE_T n, SIZE_T *done)
{
    if (process != W32_PSEUDO_PROCESS) {
        w32_set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    memcpy(a, b, n);
    if (done)
        *done = n;
    return TRUE;
}

static BOOL WINAPI ReadProcessMemory(HANDLE process, const void *from, void *to, SIZE_T n, SIZE_T *done)
{
    return copy_memory(process, to, from, n, done);
}

static BOOL WINAPI WriteProcessMemory(HANDLE process, void *to, const void *from, SIZE_T n, SIZE_T *done)
{
    return copy_memory(process, to, from, n, done);
}

static HANDLE WINAPI CreateJobObjectW(void *sa, const WCHAR *name)
{
    UNUSED(sa);
    UNUSED(name);
    return w32_handle_new(WH_JOB);
}

static BOOL WINAPI GetProcessMitigationPolicy(HANDLE process, int policy, void *buf, SIZE_T len)
{
    UNUSED(process);
    UNUSED(policy);
    memset(buf, 0, len);
    return TRUE;
}

static DWORD WINAPI GetCurrentProcessorNumber(void)
{
    return 0;
}

static BOOL WINAPI GetProcessHandleCount(HANDLE process, DWORD *count)
{
    UNUSED(process);
    DWORD n = 0;
    for (int i = 0; i < W32_MAX_HANDLES; i++)
        n += w32->handles[i].type != WH_FREE;
    *count = n;
    return TRUE;
}

static DWORD WINAPI GetProcessHeaps(DWORD count, HANDLE *heaps)
{
    if (count >= 1 && heaps)
        heaps[0] = &w32->heap;
    return 1;
}

static BOOL WINAPI K32GetPerformanceInfo(uint32_t *info, DWORD size)
{
    if (size < 56)
        return FALSE;
    memset(info, 0, size);
    info[0] = 56;
    info[1] = info[2] = info[3] = 65536;
    info[4] = 65536;
    info[5] = 32768;
    info[10] = 4096;
    info[11] = 256;
    info[12] = 4;
    info[13] = 16;
    return TRUE;
}

static BOOL WINAPI K32GetProcessMemoryInfo(HANDLE process, uint32_t *counters, DWORD size)
{
    UNUSED(process);
    if (size < 40)
        return FALSE;
    memset(counters, 0, size);
    counters[0] = size;
    return TRUE;
}

static BOOL WINAPI ReplaceFileW(const WCHAR *replaced, const WCHAR *replacement, const WCHAR *backup, DWORD flags,
                                void *exclude, void *reserved)
{
    UNUSED(backup);
    UNUSED(flags);
    UNUSED(exclude);
    UNUSED(reserved);
    BOOL(WINAPI * move)(const WCHAR *, const WCHAR *, DWORD) =
        (BOOL(WINAPI *)(const WCHAR *, const WCHAR *, DWORD))w32_builtin_lookup("kernel32.dll", "MoveFileExW");
    return move && move(replacement, replaced, 1);
}

static DWORD WINAPI zero_frames(DWORD skip, DWORD count, void **frames, DWORD *hash)
{
    UNUSED(skip);
    UNUSED(count);
    UNUSED(frames);
    if (hash)
        *hash = 0;
    return 0;
}

static const struct w32_export ntapi_ntdll[] = {
    W32_FN(RtlInitUnicodeString),
    W32_FN(RtlFreeUnicodeString),
    W32_FN(RtlFormatCurrentUserKeyPath),
    W32_FN(RtlGetLastNtStatus),
    W32_FN(NtClose),
    W32_FN(NtOpenKeyEx),
    W32_FN(NtOpenKey),
    W32_FN(NtCreateKey),
    W32_FN(NtQueryValueKey),
    W32_FN(NtSetValueKey),
    W32_AS("NtDeleteKey", w32_zero1),
    W32_FN(NtQueryInformationProcess),
    W32_AS("RtlCaptureStackBackTrace", zero_frames),
};

static const struct w32_export ntapi_kernel32[] = {
    W32_FN(GetLogicalProcessorInformation),
    W32_FN(GetProductInfo),
    W32_FN(K32GetModuleInformation),
    W32_AS("GetModuleInformation", K32GetModuleInformation),
    W32_AS("K32GetMappedFileNameW", w32_zero4),
    W32_AS("K32QueryWorkingSetEx", w32_fail3),
    W32_FN(QueryThreadCycleTime),
    W32_FN(ReadProcessMemory),
    W32_FN(WriteProcessMemory),
    W32_AS("RtlCaptureStackBackTrace", zero_frames),
    W32_FN(CreateJobObjectW),
    W32_AS("SetInformationJobObject", w32_one4),
    W32_AS("AssignProcessToJobObject", w32_one2),
    W32_AS("TerminateJobObject", w32_one2),
    W32_AS("QueryInformationJobObject", w32_fail5),
    W32_AS("SetProcessMitigationPolicy", w32_one3),
    W32_FN(GetProcessMitigationPolicy),
    W32_AS("SetThreadInformation", w32_one4),
    W32_AS("SetThreadAffinityMask", w32_one2),
    W32_AS("TransactNamedPipe", w32_fail7),
    W32_AS("WerRegisterRuntimeExceptionModule", w32_zero2),
    W32_AS("WerUnregisterRuntimeExceptionModule", w32_zero2),
    W32_AS("EnumSystemLocalesEx", w32_one4),
    W32_FN(GetCurrentProcessorNumber),
    W32_FN(GetProcessHandleCount),
    W32_FN(GetProcessHeaps),
    W32_FN(K32GetPerformanceInfo),
    W32_FN(K32GetProcessMemoryInfo),
    W32_AS("GetProcessMemoryInfo", K32GetProcessMemoryInfo),
    W32_AS("PrefetchVirtualMemory", w32_one4),
    W32_FN(ReplaceFileW),
    W32_AS("SetDefaultDllDirectories", w32_one1),
    W32_AS("SetProcessDEPPolicy", w32_one1),
    W32_AS("SetProcessShutdownParameters", w32_one2),
};

W32_EXPORTS(ntapi_ntdll_module, "ntdll.dll", ntapi_ntdll);
W32_EXPORTS(ntapi_kernel32_module, "kernel32.dll", ntapi_kernel32);
