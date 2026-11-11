#include <synth/string.h>

#include "win32.h"

/* The native calls programs make below kernel32 (the Chromium sandbox resolves and patches them in ntdll): files,
   virtual memory of any process, sections, processes, threads, tokens and waits, each on top of the Win32 function
   that does the same. A NTSTATUS comes from the Win32 error. */

#define STATUS_TIMEOUT 0x102u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_NO_TOKEN 0xc000007cu

struct unicode_string {
    uint16_t length, max_length;
    WCHAR *buffer;
};

struct object_attributes {
    ULONG length;
    HANDLE root;
    struct unicode_string *name;
    ULONG attributes;
    void *security, *qos;
};

struct io_status {
    DWORD status;
    ULONG_PTR information;
};

struct client_id {
    HANDLE process, thread;
};

static void *k32(const char *name)
{
    return w32_builtin_lookup("kernel32.dll", name);
}

static void *adv(const char *name)
{
    return w32_builtin_lookup("advapi32.dll", name);
}

static DWORD status_of(DWORD error)
{
    static const struct {
        DWORD error, status;
    } map[] = {
        {2, 0xc0000034u},   {3, 0xc000003au},   {5, 0xc0000022u},   {6, 0xc0000008u},   {8, 0xc0000017u},
        {32, 0xc0000043u},  {80, 0xc0000035u},  {183, 0xc0000035u}, {87, STATUS_INVALID_PARAMETER},
        {487, 0xc0000018u}, {1008, STATUS_NO_TOKEN}, {122, 0xc0000023u}, {267, 0xc0000103u}, {145, 0xc0000101u},
    };
    for (size_t i = 0; i < ARRAY_SIZE(map); i++)
        if (map[i].error == error)
            return map[i].status;
    return error ? 0xc0000001u : 0;
}

static DWORD failed(void)
{
    return status_of(w32_error());
}

/* "\??\C:\path" and "\DosDevices\C:\path" are the native spellings of a Win32 path; "\Device\NamedPipe\x" is
   "\\.\pipe\x" */
static const WCHAR *object_path(const struct object_attributes *attr, WCHAR *buf, size_t cap)
{
    if (!attr || !attr->name || !attr->name->buffer)
        return NULL;
    size_t n = MIN((size_t)attr->name->length / 2, cap - 10);
    memcpy(buf + 8, attr->name->buffer, n * 2);
    WCHAR *p = buf + 8;
    p[n] = 0;
    static const char pipe[] = "\\Device\\NamedPipe\\";
    size_t k = 0;
    while (pipe[k] && p[k] == (WCHAR)pipe[k])
        k++;
    if (!pipe[k]) {
        static const char dot[] = "\\\\.\\pipe\\";
        p += k - 9;
        for (k = 0; dot[k]; k++)
            p[k] = (WCHAR)dot[k];
        return p;
    }
    if (n >= 4 && p[0] == '\\' && (p[1] == '?' || p[1] == '\\') && p[2] == '?' && p[3] == '\\')
        return p + 4;
    if (n >= 12 && p[0] == '\\' && p[1] == 'D' && p[2] == 'o' && p[3] == 's')
        return p + 12;
    return p;
}

static DWORD WINAPI NtCreateFile(HANDLE *out, DWORD access, const struct object_attributes *attr, struct io_status *io,
                          const int64_t *alloc, ULONG attrs, ULONG share, ULONG disposition, ULONG options, void *ea,
                          ULONG ea_len)
{
    UNUSED(alloc);
    UNUSED(ea);
    UNUSED(ea_len);
    WCHAR buf[PATH_MAX_LEN + 16];
    const WCHAR *path = object_path(attr, buf, ARRAY_SIZE(buf));
    if (!path || !out)
        return STATUS_INVALID_PARAMETER;
    static const DWORD how[] = {2, 3, 1, 4, 5, 2};
    if (disposition > 5)
        return STATUS_INVALID_PARAMETER;
    DWORD flags = attrs & 0x7fa7;
    if (options & 1)
        flags |= 0x02000000u;
    if (options & 0x1000)
        flags |= 0x04000000u;
    if (!(options & 0x30))
        flags |= 0x40000000u;
    if (options & 8)
        flags |= 0x80000000u;
    BOOL existed = ((DWORD(WINAPI *)(LPCWSTR))k32("GetFileAttributesW"))(path) != 0xffffffffu;
    if ((options & 1) && (disposition == 2 || disposition == 3) && !existed &&
        !((BOOL(WINAPI *)(LPCWSTR, void *))k32("CreateDirectoryW"))(path, NULL))
        return failed();
    HANDLE h = ((HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE))k32("CreateFileW"))(
        path, access, share, NULL, (options & 1) ? 3 : how[disposition], flags, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return failed();
    *out = h;
    if (io)
        *io = (struct io_status){0, !existed ? 2 : disposition == 0 ? 0 : disposition >= 4 ? 3 : 1};
    return 0;
}

static DWORD WINAPI NtOpenFile(HANDLE *out, DWORD access, const struct object_attributes *attr, struct io_status *io,
                        ULONG share, ULONG options)
{
    return NtCreateFile(out, access, attr, io, NULL, 0, share, 1, options, NULL, 0);
}

struct attribute_data {
    DWORD attributes;
    uint64_t created, accessed, written;
    DWORD size_high, size_low;
} __attribute__((packed));

static DWORD query_attributes(const struct object_attributes *attr, struct attribute_data *d)
{
    WCHAR buf[PATH_MAX_LEN + 16];
    const WCHAR *path = object_path(attr, buf, ARRAY_SIZE(buf));
    if (!path)
        return STATUS_INVALID_PARAMETER;
    if (!((BOOL(WINAPI *)(LPCWSTR, int, void *))k32("GetFileAttributesExW"))(path, 0, d))
        return failed();
    return 0;
}

/* FILE_BASIC_INFORMATION: four times, then the attributes */
static DWORD WINAPI NtQueryAttributesFile(const struct object_attributes *attr, uint64_t *info)
{
    struct attribute_data d;
    DWORD r = query_attributes(attr, &d);
    if (r)
        return r;
    info[0] = d.created;
    info[1] = d.accessed;
    info[2] = d.written;
    info[3] = d.written;
    info[4] = d.attributes;
    return 0;
}

/* FILE_NETWORK_OPEN_INFORMATION: the times, allocation size, end of file and attributes */
static DWORD WINAPI NtQueryFullAttributesFile(const struct object_attributes *attr, uint64_t *info)
{
    struct attribute_data d;
    DWORD r = query_attributes(attr, &d);
    if (r)
        return r;
    uint64_t size = (uint64_t)d.size_high << 32 | d.size_low;
    info[0] = d.created;
    info[1] = d.accessed;
    info[2] = d.written;
    info[3] = d.written;
    info[4] = (size + 4095) & ~4095ull;
    info[5] = size;
    info[6] = d.attributes;
    return 0;
}

static DWORD WINAPI NtAllocateVirtualMemory(HANDLE process, void **base, ULONG_PTR zero, SIZE_T *size, ULONG type,
                                     ULONG protect)
{
    UNUSED(zero);
    if (!base || !size)
        return STATUS_INVALID_PARAMETER;
    void *p = ((void *(WINAPI *)(HANDLE, void *, SIZE_T, DWORD, DWORD))k32("VirtualAllocEx"))(process, *base, *size,
                                                                                            type, protect);
    if (!p)
        return failed();
    uintptr_t from = (uintptr_t)(*base ? *base : p), start = *base ? from & ~4095u : from;
    *size = ALIGN_UP(from + *size, 4096u) - start;
    *base = (void *)start;
    return 0;
}

static DWORD WINAPI NtFreeVirtualMemory(HANDLE process, void **base, SIZE_T *size, ULONG type)
{
    if (!base || !size)
        return STATUS_INVALID_PARAMETER;
    if (!((BOOL(WINAPI *)(HANDLE, void *, SIZE_T, DWORD))k32("VirtualFreeEx"))(process, *base, *size, type))
        return failed();
    return 0;
}

static DWORD WINAPI NtProtectVirtualMemory(HANDLE process, void **base, SIZE_T *size, ULONG protect, ULONG *old)
{
    if (!base || !size)
        return STATUS_INVALID_PARAMETER;
    DWORD previous = 0;
    if (!((BOOL(WINAPI *)(HANDLE, void *, SIZE_T, DWORD, DWORD *))k32("VirtualProtectEx"))(process, *base, *size,
                                                                                         protect, &previous))
        return failed();
    uintptr_t start = (uintptr_t)*base & ~4095u;
    *size = ALIGN_UP((uintptr_t)*base + *size, 4096u) - start;
    *base = (void *)start;
    if (old)
        *old = previous;
    return 0;
}

static DWORD WINAPI NtWriteVirtualMemory(HANDLE process, void *base, const void *buf, SIZE_T size, SIZE_T *done)
{
    BOOL(WINAPI * write)(HANDLE, void *, const void *, SIZE_T, SIZE_T *) = k32("WriteProcessMemory");
    return write(process, base, buf, size, done) ? 0 : failed();
}

static DWORD WINAPI NtDuplicateObject(HANDLE source_process, HANDLE source, HANDLE target_process, HANDLE *out,
                               DWORD access, ULONG attributes, ULONG options)
{
    BOOL(WINAPI * dup)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD) = k32("DuplicateHandle");
    return dup(source_process, source, target_process, out, access, (attributes & 2) != 0, options) ? 0 : failed();
}

static WCHAR *object_name(const struct object_attributes *attr, WCHAR *buf, size_t cap)
{
    if (!attr || !attr->name || !attr->name->buffer)
        return NULL;
    size_t n = MIN((size_t)attr->name->length / 2, cap - 1);
    memcpy(buf, attr->name->buffer, n * 2);
    buf[n] = 0;
    for (size_t i = n; i-- > 0;)
        if (buf[i] == '\\')
            return buf + i + 1;
    return buf;
}

static DWORD WINAPI NtCreateSection(HANDLE *out, DWORD access, const struct object_attributes *attr, const int64_t *size,
                             ULONG protect, ULONG attributes, HANDLE file)
{
    UNUSED(access);
    WCHAR buf[128];
    uint64_t n = size ? (uint64_t)*size : 0;
    HANDLE h = ((HANDLE(WINAPI *)(HANDLE, void *, DWORD, DWORD, DWORD, LPCWSTR))k32("CreateFileMappingW"))(
        file ? file : INVALID_HANDLE_VALUE, NULL, protect | (attributes & 0x1f000000u), (DWORD)(n >> 32), (DWORD)n,
        object_name(attr, buf, ARRAY_SIZE(buf)));
    if (!h)
        return failed();
    *out = h;
    return 0;
}

static DWORD WINAPI NtOpenProcess(HANDLE *out, DWORD access, const struct object_attributes *attr,
                           const struct client_id *id)
{
    UNUSED(attr);
    if (!out || !id)
        return STATUS_INVALID_PARAMETER;
    HANDLE h = ((HANDLE(WINAPI *)(DWORD, BOOL, DWORD))k32("OpenProcess"))(access, FALSE, (DWORD)id->process);
    if (!h)
        return 0xc000000bu;
    *out = h;
    return 0;
}

static DWORD WINAPI NtOpenThread(HANDLE *out, DWORD access, const struct object_attributes *attr, const struct client_id *id)
{
    UNUSED(attr);
    if (!out || !id)
        return STATUS_INVALID_PARAMETER;
    HANDLE h = ((HANDLE(WINAPI *)(DWORD, BOOL, DWORD))k32("OpenThread"))(access, FALSE, (DWORD)id->thread);
    if (!h)
        return 0xc000000bu;
    *out = h;
    return 0;
}

static DWORD WINAPI NtOpenProcessTokenEx(HANDLE process, DWORD access, ULONG attributes, HANDLE *out)
{
    UNUSED(attributes);
    return ((BOOL(WINAPI *)(HANDLE, DWORD, HANDLE *))adv("OpenProcessToken"))(process, access, out) ? 0 : failed();
}

static DWORD WINAPI NtOpenProcessToken(HANDLE process, DWORD access, HANDLE *out)
{
    return NtOpenProcessTokenEx(process, access, 0, out);
}

static DWORD WINAPI NtOpenThreadTokenEx(HANDLE thread, DWORD access, uint8_t self, ULONG attributes, HANDLE *out)
{
    UNUSED(attributes);
    BOOL(WINAPI * open)(HANDLE, DWORD, BOOL, HANDLE *) = adv("OpenThreadToken");
    return open(thread, access, self, out) ? 0 : failed();
}

static DWORD WINAPI NtOpenThreadToken(HANDLE thread, DWORD access, uint8_t self, HANDLE *out)
{
    return NtOpenThreadTokenEx(thread, access, self, 0, out);
}

/* ThreadImpersonationToken (5) is the one class with an effect here */
static DWORD WINAPI NtSetInformationThread(HANDLE thread, int cls, const void *info, ULONG len)
{
    if (cls == 5) {
        if (!info || len < sizeof(HANDLE))
            return STATUS_INVALID_PARAMETER;
        HANDLE t = thread;
        return ((BOOL(WINAPI *)(HANDLE *, HANDLE))adv("SetThreadToken"))(&t, *(const HANDLE *)info) ? 0 : failed();
    }
    return 0;
}

static DWORD wait_ms(const int64_t *timeout)
{
    if (!timeout)
        return 0xffffffffu;
    if (*timeout >= 0)
        return 0;
    uint64_t ms = (uint64_t)-*timeout / 10000;
    return ms >= 0xfffffffeu ? 0xfffffffeu : (DWORD)ms;
}

static DWORD wait_status(DWORD r)
{
    return r == 0xffffffffu ? failed() : r == 258 ? STATUS_TIMEOUT : r;
}

static DWORD WINAPI NtWaitForSingleObject(HANDLE h, uint8_t alertable, const int64_t *timeout)
{
    DWORD(WINAPI * wait)(HANDLE, DWORD, BOOL) = k32("WaitForSingleObjectEx");
    return wait_status(wait(h, wait_ms(timeout), alertable));
}

static DWORD WINAPI NtSignalAndWaitForSingleObject(HANDLE signal, HANDLE h, uint8_t alertable, const int64_t *timeout)
{
    DWORD(WINAPI * wait)(HANDLE, HANDLE, DWORD, BOOL) = k32("SignalObjectAndWait");
    return wait_status(wait(signal, h, wait_ms(timeout), alertable));
}

static DWORD WINAPI NtDelayExecution(uint8_t alertable, const int64_t *interval)
{
    ((DWORD(WINAPI *)(DWORD, BOOL))k32("SleepEx"))(wait_ms(interval), alertable);
    return 0;
}

static DWORD WINAPI NtTerminateProcess(HANDLE process, DWORD code)
{
    if (!process)
        return 0;
    return ((BOOL(WINAPI *)(HANDLE, UINT))k32("TerminateProcess"))(process, code) ? 0 : failed();
}

static DWORD WINAPI NtCreateEvent(HANDLE *out, DWORD access, const struct object_attributes *attr, int type, uint8_t state)
{
    UNUSED(access);
    WCHAR buf[128];
    HANDLE h = ((HANDLE(WINAPI *)(void *, BOOL, BOOL, LPCWSTR))k32("CreateEventW"))(NULL, type == 0, state,
                                                                                 object_name(attr, buf, 128));
    if (!h)
        return failed();
    *out = h;
    return 0;
}

static DWORD WINAPI NtOpenEvent(HANDLE *out, DWORD access, const struct object_attributes *attr)
{
    WCHAR buf[128];
    HANDLE h = ((HANDLE(WINAPI *)(DWORD, BOOL, LPCWSTR))k32("OpenEventW"))(access, FALSE, object_name(attr, buf, 128));
    if (!h)
        return failed();
    *out = h;
    return 0;
}

static DWORD WINAPI NtQuerySystemTime(uint64_t *time)
{
    ((void(WINAPI *)(uint64_t *))k32("GetSystemTimeAsFileTime"))(time);
    return 0;
}

static DWORD WINAPI NtYieldExecution(void)
{
    ((DWORD(WINAPI *)(DWORD, BOOL))k32("SleepEx"))(0, FALSE);
    return 0;
}

static HANDLE WINAPI RtlCreateHeap(ULONG flags, void *base, SIZE_T reserve, SIZE_T commit, void *lock, void *params)
{
    UNUSED(base);
    UNUSED(lock);
    UNUSED(params);
    return ((HANDLE(WINAPI *)(DWORD, SIZE_T, SIZE_T))k32("HeapCreate"))(flags & 0x5, commit, reserve);
}

static HANDLE WINAPI RtlDestroyHeap(HANDLE heap)
{
    return ((BOOL(WINAPI *)(HANDLE))k32("HeapDestroy"))(heap) ? NULL : heap;
}

static const struct w32_export ntcore_exports[] = {
    W32_FN(NtCreateFile),
    W32_AS("ZwCreateFile", NtCreateFile),
    W32_FN(NtOpenFile),
    W32_AS("ZwOpenFile", NtOpenFile),
    W32_FN(NtQueryAttributesFile),
    W32_AS("ZwQueryAttributesFile", NtQueryAttributesFile),
    W32_FN(NtQueryFullAttributesFile),
    W32_AS("ZwQueryFullAttributesFile", NtQueryFullAttributesFile),
    W32_FN(NtAllocateVirtualMemory),
    W32_AS("ZwAllocateVirtualMemory", NtAllocateVirtualMemory),
    W32_FN(NtFreeVirtualMemory),
    W32_AS("ZwFreeVirtualMemory", NtFreeVirtualMemory),
    W32_FN(NtProtectVirtualMemory),
    W32_AS("ZwProtectVirtualMemory", NtProtectVirtualMemory),
    W32_FN(NtWriteVirtualMemory),
    W32_AS("ZwWriteVirtualMemory", NtWriteVirtualMemory),
    W32_FN(NtDuplicateObject),
    W32_AS("ZwDuplicateObject", NtDuplicateObject),
    W32_FN(NtCreateSection),
    W32_AS("ZwCreateSection", NtCreateSection),
    W32_FN(NtOpenProcess),
    W32_AS("ZwOpenProcess", NtOpenProcess),
    W32_FN(NtOpenThread),
    W32_AS("ZwOpenThread", NtOpenThread),
    W32_FN(NtOpenProcessToken),
    W32_AS("ZwOpenProcessToken", NtOpenProcessToken),
    W32_FN(NtOpenProcessTokenEx),
    W32_AS("ZwOpenProcessTokenEx", NtOpenProcessTokenEx),
    W32_FN(NtOpenThreadToken),
    W32_AS("ZwOpenThreadToken", NtOpenThreadToken),
    W32_FN(NtOpenThreadTokenEx),
    W32_AS("ZwOpenThreadTokenEx", NtOpenThreadTokenEx),
    W32_FN(NtSetInformationThread),
    W32_AS("ZwSetInformationThread", NtSetInformationThread),
    W32_FN(NtWaitForSingleObject),
    W32_AS("ZwWaitForSingleObject", NtWaitForSingleObject),
    W32_FN(NtSignalAndWaitForSingleObject),
    W32_AS("ZwSignalAndWaitForSingleObject", NtSignalAndWaitForSingleObject),
    W32_FN(NtDelayExecution),
    W32_AS("ZwDelayExecution", NtDelayExecution),
    W32_FN(NtTerminateProcess),
    W32_AS("ZwTerminateProcess", NtTerminateProcess),
    W32_FN(NtCreateEvent),
    W32_AS("ZwCreateEvent", NtCreateEvent),
    W32_FN(NtOpenEvent),
    W32_AS("ZwOpenEvent", NtOpenEvent),
    W32_FN(NtQuerySystemTime),
    W32_AS("ZwQuerySystemTime", NtQuerySystemTime),
    W32_FN(NtYieldExecution),
    W32_AS("ZwYieldExecution", NtYieldExecution),
    W32_FN(RtlCreateHeap),
    W32_FN(RtlDestroyHeap),
};
W32_EXPORTS(ntcore_module, "ntdll.dll", ntcore_exports);
