#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "win32.h"

#define ERROR_NO_MORE_ITEMS 259
#define APPMODEL_ERROR_NO_PACKAGE 15700
#define REGDB_E_CLASSNOTREG 0x80040154u

static void *kernel32_fn(const char *name)
{
    return w32_builtin_lookup("kernel32.dll", name);
}

static BOOL WINAPI CheckRemoteDebuggerPresent(HANDLE process, BOOL *present)
{
    UNUSED(process);
    *present = FALSE;
    return TRUE;
}

static BOOL WINAPI CompareObjectHandles(HANDLE a, HANDLE b)
{
    return a == b;
}

static BOOL WINAPI FreeLibraryAndExitThread(HANDLE module, DWORD code)
{
    BOOL(WINAPI * free_library)(HANDLE) = (BOOL(WINAPI *)(HANDLE))kernel32_fn("FreeLibrary");
    void(WINAPI * exit_thread)(DWORD) = (void(WINAPI *)(DWORD))kernel32_fn("ExitThread");
    if (free_library)
        free_library(module);
    if (exit_thread)
        exit_thread(code);
    return FALSE;
}

static BOOL WINAPI GetConsoleDisplayMode(DWORD *flags)
{
    *flags = 0;
    return TRUE;
}

static void put_wide(WCHAR *dst, const char *src)
{
    while ((*dst++ = (uint8_t)*src++))
        ;
}

static DWORD WINAPI GetDynamicTimeZoneInformation(uint8_t *info)
{
    memset(info, 0, 432);
    put_wide((WCHAR *)(info + 4), "Coordinated Universal Time");
    put_wide((WCHAR *)(info + 88), "Coordinated Universal Time");
    put_wide((WCHAR *)(info + 172), "UTC");
    return 0;
}

static BOOL WINAPI TzSpecificLocalTimeToSystemTime(const void *tz, const uint16_t *local, uint16_t *utc)
{
    UNUSED(tz);
    memcpy(utc, local, 16);
    return TRUE;
}

static BOOL WINAPI GetFirmwareType(DWORD *type)
{
    *type = 1;
    return TRUE;
}

static DWORD WINAPI GetMaximumProcessorCount(WORD group)
{
    UNUSED(group);
    return 1;
}

static WORD WINAPI GetMaximumProcessorGroupCount(void)
{
    return 1;
}

static BOOL WINAPI GetNamedPipeProcessId(HANDLE pipe, ULONG *pid)
{
    UNUSED(pipe);
    DWORD(WINAPI * self)(void) = (DWORD(WINAPI *)(void))kernel32_fn("GetCurrentProcessId");
    *pid = self ? self() : 0;
    return TRUE;
}

static DWORD WINAPI GetPackagePathByFullName(LPCWSTR name, UINT *len, LPWSTR path)
{
    UNUSED(name);
    UNUSED(path);
    if (len)
        *len = 0;
    return APPMODEL_ERROR_NO_PACKAGE;
}

static DWORD WINAPI no_package2(UINT *len, WCHAR *buf)
{
    UNUSED(buf);
    if (len)
        *len = 0;
    return APPMODEL_ERROR_NO_PACKAGE;
}

static DWORD WINAPI no_package3(HANDLE process, UINT *len, WCHAR *buf)
{
    UNUSED(process);
    return no_package2(len, buf);
}

static DWORD WINAPI GetPackagesByPackageFamily(LPCWSTR family, UINT *count, LPWSTR *names, UINT *len, WCHAR *buf)
{
    UNUSED(family);
    UNUSED(names);
    UNUSED(buf);
    *count = 0;
    *len = 0;
    return 0;
}

static DWORD WINAPI GetPriorityClass(HANDLE process)
{
    UNUSED(process);
    return 0x20;
}

static BOOL WINAPI GetSystemPowerStatus(uint8_t *status)
{
    memset(status, 0, 12);
    status[0] = 1;
    status[1] = 128;
    status[2] = 255;
    memset(status + 4, 0xff, 8);
    return TRUE;
}

static BOOL WINAPI GetThreadGroupAffinity(HANDLE thread, uint32_t *affinity)
{
    UNUSED(thread);
    memset(affinity, 0, 12);
    affinity[0] = 1;
    return TRUE;
}

static BOOL WINAPI HeapWalk(HANDLE heap, void *entry)
{
    UNUSED(heap);
    UNUSED(entry);
    w32_set_error(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

static BOOL WINAPI HeapQueryInformation(HANDLE heap, int cls, void *info, SIZE_T len, SIZE_T *ret)
{
    UNUSED(heap);
    if (cls != 0) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (ret)
        *ret = 4;
    if (len < 4) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *(ULONG *)info = 2;
    return TRUE;
}

static BOOL WINAPI K32EnumProcessModules(HANDLE process, HANDLE *modules, DWORD size, DWORD *needed)
{
    UNUSED(process);
    DWORD n = 0;
    for (struct w32_image *img = w32->images; img; img = img->next, n++)
        if ((n + 1) * sizeof(HANDLE) <= size)
            modules[n] = (HANDLE)img->base;
    *needed = n * (DWORD)sizeof(HANDLE);
    return TRUE;
}

static DWORD WINAPI K32GetModuleFileNameExW(HANDLE process, HANDLE module, LPWSTR out, DWORD size)
{
    UNUSED(process);
    DWORD(WINAPI * fn)(HANDLE, LPWSTR, DWORD) = (DWORD(WINAPI *)(HANDLE, LPWSTR, DWORD))kernel32_fn("GetModuleFileNameW");
    return fn ? fn(module, out, size) : 0;
}

static DWORD WINAPI K32GetModuleFileNameExA(HANDLE process, HANDLE module, LPSTR out, DWORD size)
{
    UNUSED(process);
    DWORD(WINAPI * fn)(HANDLE, LPSTR, DWORD) = (DWORD(WINAPI *)(HANDLE, LPSTR, DWORD))kernel32_fn("GetModuleFileNameA");
    return fn ? fn(module, out, size) : 0;
}

static DWORD WINAPI K32GetModuleBaseNameW(HANDLE process, HANDLE module, LPWSTR out, DWORD size)
{
    WCHAR full[PATH_MAX_LEN];
    DWORD n = K32GetModuleFileNameExW(process, module, full, PATH_MAX_LEN);
    if (!n || !out || !size)
        return 0;
    DWORD start = n;
    while (start && full[start - 1] != '\\' && full[start - 1] != '/')
        start--;
    DWORD len = MIN(n - start, size - 1);
    memcpy(out, full + start, len * sizeof(WCHAR));
    out[len] = 0;
    return len;
}

static DWORD WINAPI K32GetModuleBaseNameA(HANDLE process, HANDLE module, LPSTR out, DWORD size)
{
    char full[PATH_MAX_LEN];
    DWORD n = K32GetModuleFileNameExA(process, module, full, PATH_MAX_LEN);
    if (!n || !out || !size)
        return 0;
    const char *base = strrchr(full, '\\');
    base = base ? base + 1 : full;
    return (DWORD)str_copy(out, base, size);
}

static BOOL WINAPI QueryFullProcessImageNameW(HANDLE process, DWORD flags, LPWSTR out, DWORD *size)
{
    UNUSED(flags);
    DWORD n = K32GetModuleFileNameExW(process, NULL, out, *size);
    if (!n || n >= *size) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *size = n;
    return TRUE;
}

static BOOL WINAPI QueryFullProcessImageNameA(HANDLE process, DWORD flags, LPSTR out, DWORD *size)
{
    UNUSED(flags);
    DWORD n = K32GetModuleFileNameExA(process, NULL, out, *size);
    if (!n || n >= *size) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *size = n;
    return TRUE;
}

static HANDLE WINAPI PowerCreateRequest(void *context)
{
    UNUSED(context);
    return w32_handle_new(WH_NULL);
}

static BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *session)
{
    UNUSED(pid);
    *session = 1;
    return TRUE;
}

static DWORD WINAPI WTSGetActiveConsoleSessionId(void)
{
    return 1;
}

static BOOL WINAPI QueryProcessCycleTime(HANDLE process, uint64_t *cycles)
{
    UNUSED(process);
    *cycles = hal_ticks_ms() * 1000000ull;
    return TRUE;
}

static BOOL WINAPI QueryUnbiasedInterruptTime(uint64_t *t)
{
    *t = hal_ticks_ms() * 10000ull;
    return TRUE;
}

static void WINAPI QueryUnbiasedInterruptTimePrecise(uint64_t *t)
{
    QueryUnbiasedInterruptTime(t);
}

static DWORD WINAPI SetThreadExecutionState(DWORD flags)
{
    UNUSED(flags);
    return 0x80000000u;
}

static DWORD WINAPI DiscardVirtualMemory(void *addr, SIZE_T size)
{
    UNUSED(addr);
    UNUSED(size);
    return 0;
}

static DWORD WINAPI RoInitialize(int type)
{
    DWORD(WINAPI * init)(void *, DWORD) = (DWORD(WINAPI *)(void *, DWORD))w32_builtin_lookup("ole32.dll", "CoInitializeEx");
    return init ? init(NULL, type == 1 ? 0 : 2) : 0;
}

static void WINAPI RoUninitialize(void)
{
    void(WINAPI * uninit)(void) = (void(WINAPI *)(void))w32_builtin_lookup("ole32.dll", "CoUninitialize");
    if (uninit)
        uninit();
}

static DWORD WINAPI RoGetActivationFactory(void *cls, const void *iid, void **factory)
{
    UNUSED(cls);
    UNUSED(iid);
    *factory = NULL;
    return REGDB_E_CLASSNOTREG;
}

static DWORD WINAPI RoActivateInstance(void *cls, void **instance)
{
    UNUSED(cls);
    *instance = NULL;
    return REGDB_E_CLASSNOTREG;
}

struct hstring {
    uint32_t reference;
    uint32_t length;
    const WCHAR *text;
    uint32_t pad[2];
};

static DWORD WINAPI WindowsCreateString(const WCHAR *src, uint32_t len, struct hstring **out)
{
    *out = NULL;
    if (!len)
        return 0;
    if (!src)
        return 0x80004003u;
    struct hstring *h = w32_alloc(sizeof(*h) + (len + 1) * sizeof(WCHAR));
    if (!h)
        return 0x8007000eu;
    WCHAR *text = (WCHAR *)(h + 1);
    memcpy(text, src, len * sizeof(WCHAR));
    text[len] = 0;
    *h = (struct hstring){0, len, text, {0, 0}};
    *out = h;
    return 0;
}

static DWORD WINAPI WindowsCreateStringReference(const WCHAR *src, uint32_t len, struct hstring *header,
                                                 struct hstring **out)
{
    *out = NULL;
    if (!len)
        return 0;
    *header = (struct hstring){1, len, src, {0, 0}};
    *out = header;
    return 0;
}

static DWORD WINAPI WindowsDeleteString(struct hstring *h)
{
    if (h && !h->reference)
        w32_free(h);
    return 0;
}

static const WCHAR *WINAPI WindowsGetStringRawBuffer(struct hstring *h, uint32_t *len)
{
    static const WCHAR empty[1];
    if (len)
        *len = h ? h->length : 0;
    return h ? h->text : empty;
}

static DWORD WINAPI NtWriteFile(HANDLE file, HANDLE event, void *apc, void *context, uint32_t *iosb, const void *buf,
                                ULONG len, void *offset, void *key)
{
    UNUSED(event);
    UNUSED(apc);
    UNUSED(context);
    UNUSED(offset);
    UNUSED(key);
    BOOL(WINAPI * write)(HANDLE, const void *, DWORD, DWORD *, void *) =
        (BOOL(WINAPI *)(HANDLE, const void *, DWORD, DWORD *, void *))kernel32_fn("WriteFile");
    DWORD done = 0;
    DWORD status = write && write(file, buf, len, &done, NULL) ? 0 : 0xc0000008u;
    if (iosb) {
        iosb[0] = status;
        iosb[1] = done;
    }
    return status;
}

static DWORD WINAPI RtlNtStatusToDosError(DWORD status)
{
    switch (status) {
    case 0: return 0;
    case 0x103: return 997;
    case 0xc0000008u: return ERROR_INVALID_HANDLE;
    case 0xc0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xc0000022u: return ERROR_ACCESS_DENIED;
    case 0xc0000034u: return 2;
    case 0xc000003au: return 3;
    case 0xc00000bbu: return 50;
    case 0xc0000002u: return ERROR_CALL_NOT_IMPLEMENTED;
    default: return status & 0x80000000u ? 317 : status;
    }
}

static DWORD WINAPI NtQuerySystemInformation(int cls, void *buf, ULONG len, ULONG *ret)
{
    UNUSED(cls);
    UNUSED(buf);
    UNUSED(len);
    if (ret)
        *ret = 0;
    return 0xc0000003u;
}

static BOOL WINAPI IsWow64Process2(HANDLE process, uint16_t *machine, uint16_t *native)
{
    UNUSED(process);
    bool wow = w32_wow64();
    *machine = wow ? 0x14c : 0;
    if (native)
        *native = wow ? 0x8664 : 0x14c;
    return TRUE;
}

static DWORD WINAPI SetThreadDescription(HANDLE thread, const WCHAR *text)
{
    UNUSED(thread);
    UNUSED(text);
    return 0;
}

static DWORD WINAPI GetThreadDescription(HANDLE thread, WCHAR **text)
{
    UNUSED(thread);
    *text = NULL;
    return 0x80004001u;
}

static BOOL WINAPI SetFileCompletionNotificationModes(HANDLE h, uint8_t flags)
{
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->skip_port = flags & 1;
    return TRUE;
}

static const struct w32_export sysinfo_kernel32[] = {
    W32_FN(IsWow64Process2),
    W32_FN(SetThreadDescription),
    W32_FN(GetThreadDescription),
    W32_FN(CheckRemoteDebuggerPresent),
    W32_FN(CompareObjectHandles),
    W32_FN(FreeLibraryAndExitThread),
    W32_FN(GetConsoleDisplayMode),
    W32_FN(GetDynamicTimeZoneInformation),
    W32_FN(TzSpecificLocalTimeToSystemTime),
    W32_FN(GetFirmwareType),
    W32_FN(GetMaximumProcessorCount),
    W32_FN(GetMaximumProcessorGroupCount),
    W32_AS("GetActiveProcessorGroupCount", GetMaximumProcessorGroupCount),
    W32_AS("GetNamedPipeClientProcessId", GetNamedPipeProcessId),
    W32_AS("GetNamedPipeServerProcessId", GetNamedPipeProcessId),
    W32_FN(GetPackagePathByFullName),
    W32_AS("GetCurrentPackageFullName", no_package2),
    W32_AS("GetCurrentPackageFamilyName", no_package2),
    W32_AS("GetCurrentPackageId", no_package2),
    W32_AS("GetCurrentPackagePath", no_package2),
    W32_AS("GetCurrentApplicationUserModelId", no_package2),
    W32_AS("GetPackageFullName", no_package3),
    W32_AS("GetPackageFamilyName", no_package3),
    W32_AS("GetApplicationUserModelId", no_package3),
    W32_FN(GetPackagesByPackageFamily),
    W32_FN(GetPriorityClass),
    W32_AS("SetPriorityClass", w32_one2),
    W32_AS("GetProcessInformation", w32_fail4),
    W32_AS("SetProcessInformation", w32_one4),
    W32_FN(GetSystemPowerStatus),
    W32_FN(GetThreadGroupAffinity),
    W32_AS("GetVolumeNameForVolumeMountPointW", w32_fail3),
    W32_AS("QueryDosDeviceW", w32_zero3),
    W32_AS("HeapLock", w32_one1),
    W32_AS("HeapUnlock", w32_one1),
    W32_FN(HeapWalk),
    W32_FN(HeapQueryInformation),
    W32_FN(K32EnumProcessModules),
    W32_AS("EnumProcessModules", K32EnumProcessModules),
    W32_FN(K32GetModuleFileNameExW),
    W32_AS("GetModuleFileNameExW", K32GetModuleFileNameExW),
    W32_FN(K32GetModuleFileNameExA),
    W32_AS("GetModuleFileNameExA", K32GetModuleFileNameExA),
    W32_FN(K32GetModuleBaseNameW),
    W32_AS("GetModuleBaseNameW", K32GetModuleBaseNameW),
    W32_FN(K32GetModuleBaseNameA),
    W32_AS("GetModuleBaseNameA", K32GetModuleBaseNameA),
    W32_FN(QueryFullProcessImageNameW),
    W32_FN(QueryFullProcessImageNameA),
    W32_AS("Module32FirstW", w32_fail2),
    W32_AS("Module32NextW", w32_fail2),
    W32_AS("CreateRemoteThreadEx", w32_fail8),
    W32_AS("OpenFileMappingA", w32_fail3),
    W32_FN(PowerCreateRequest),
    W32_AS("PowerSetRequest", w32_one2),
    W32_AS("PowerClearRequest", w32_one2),
    W32_FN(ProcessIdToSessionId),
    W32_FN(WTSGetActiveConsoleSessionId),
    W32_FN(QueryProcessCycleTime),
    W32_FN(QueryUnbiasedInterruptTime),
    W32_FN(QueryUnbiasedInterruptTimePrecise),
    W32_FN(SetThreadExecutionState),
    W32_FN(DiscardVirtualMemory),
    W32_AS("FlushInstructionCache", w32_one3),
    W32_AS("SetDllDirectoryW", w32_one1),
    W32_AS("SetDllDirectoryA", w32_one1),
    W32_FN(SetFileCompletionNotificationModes),
    W32_AS("ClearCommError", w32_fail3),
    W32_AS("EscapeCommFunction", w32_fail2),
    W32_AS("GetCommModemStatus", w32_fail2),
    W32_AS("GetCommState", w32_fail2),
    W32_AS("SetCommState", w32_fail2),
    W32_AS("SetCommTimeouts", w32_fail2),
    W32_AS("PurgeComm", w32_fail2),
    W32_FN(RoInitialize),
    W32_FN(RoUninitialize),
    W32_FN(RoGetActivationFactory),
    W32_FN(RoActivateInstance),
    W32_FN(WindowsCreateString),
    W32_FN(WindowsCreateStringReference),
    W32_FN(WindowsDeleteString),
    W32_FN(WindowsGetStringRawBuffer),
};

static const char *object_type_name(HANDLE h, const struct w32_object *o)
{
    if (h == W32_PSEUDO_PROCESS)
        return "Process";
    if (h == W32_PSEUDO_THREAD)
        return "Thread";
    switch (o->type) {
    case WH_EVENT: return "Event";
    case WH_MUTEX: return "Mutant";
    case WH_SEMAPHORE: return "Semaphore";
    case WH_THREAD: return "Thread";
    case WH_PROCESS: return "Process";
    case WH_TIMER: return "Timer";
    case WH_PORT: return "IoCompletion";
    case WH_SECTION: return "Section";
    case WH_TOKEN: return "Token";
    case WH_KEY: return "Key";
    case WH_JOB: return "Job";
    default: return "File";
    }
}

static DWORD WINAPI NtQueryObject(HANDLE h, int cls, uint8_t *info, ULONG len, ULONG *ret)
{
    static const struct w32_object pseudo = {.type = WH_PROCESS};
    const struct w32_object *o = h == W32_PSEUDO_PROCESS || h == W32_PSEUDO_THREAD ? &pseudo : w32_handle_get(h);
    if (!o)
        return 0xc0000008u;
    ULONG need;
    switch (cls) {
    case 0: need = 56; break;
    case 1: need = 8; break;
    case 2: need = 96 + ((ULONG)strlen(object_type_name(h, o)) + 1) * 2; break;
    case 4: need = 2; break;
    default: return 0xc0000003u;
    }
    if (ret)
        *ret = need;
    if (!info || len < need)
        return 0xc0000004u;
    memset(info, 0, need);
    if (cls == 0) {
        uint32_t basic[4] = {o->inherit ? 2u : 0u, o->access ? o->access : 0x1f0fffu, 1, 1};
        memcpy(info, basic, sizeof(basic));
    } else if (cls == 2) {
        const char *name = object_type_name(h, o);
        uint16_t n = (uint16_t)(strlen(name) * 2);
        uint16_t max = (uint16_t)(n + 2);
        uint32_t buffer = (uint32_t)(uintptr_t)(info + 96);
        memcpy(info, &n, 2);
        memcpy(info + 2, &max, 2);
        memcpy(info + 4, &buffer, 4);
        for (int i = 0; name[i]; i++)
            info[96 + i * 2] = (uint8_t)name[i];
    } else if (cls == 4) {
        info[0] = o->inherit;
    }
    return 0;
}

static DWORD WINAPI NtSetInformationObject(HANDLE h, int cls, const uint8_t *info, ULONG len)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return 0xc0000008u;
    if (cls != 4)
        return 0xc0000003u;
    if (len < 2)
        return 0xc0000004u;
    o->inherit = info[0] != 0;
    return 0;
}

static const struct w32_export sysinfo_ntdll[] = {
    W32_FN(NtQueryObject),
    W32_FN(NtSetInformationObject),
    W32_FN(NtWriteFile),
    W32_FN(RtlNtStatusToDosError),
    W32_FN(NtQuerySystemInformation),
};

W32_EXPORTS(sysinfo_kernel32_module, "kernel32.dll", sysinfo_kernel32);
W32_EXPORTS(sysinfo_ntdll_module, "ntdll.dll", sysinfo_ntdll);
