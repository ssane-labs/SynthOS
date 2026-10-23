#include <synth/string.h>

#include "win32.h"

/* ntdll's run-time library and the native calls Firefox makes directly: strings, run-once, condition variables,
   memory queries and file views, each on top of the kernel32 function that does the same. */

#define STATUS_TIMEOUT 0x102u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_NO_MEMORY 0xc0000017u
#define STATUS_INVALID_INFO_CLASS 0xc0000003u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034u
#define STATUS_UNSUCCESSFUL 0xc0000001u

struct unicode_string {
    uint16_t length, max_length;
    WCHAR *buffer;
};

struct ansi_string {
    uint16_t length, max_length;
    char *buffer;
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

static void *k32(const char *name)
{
    return w32_builtin_lookup("kernel32.dll", name);
}

static SIZE_T WINAPI RtlCompareMemory(const void *a, const void *b, SIZE_T n)
{
    const uint8_t *x = a, *y = b;
    SIZE_T i = 0;
    while (i < n && x[i] == y[i])
        i++;
    return i;
}

static void WINAPI RtlInitAnsiString(struct ansi_string *s, const char *text)
{
    size_t n = text ? strlen(text) : 0;
    s->length = (uint16_t)MIN(n, 0xfffeu);
    s->max_length = text ? (uint16_t)(s->length + 1) : 0;
    s->buffer = (char *)text;
}

static DWORD WINAPI RtlAnsiStringToUnicodeString(struct unicode_string *dst, const struct ansi_string *src, BOOL alloc)
{
    size_t need = (size_t)src->length * 2;
    if (alloc) {
        dst->buffer = w32_alloc(need + 2);
        if (!dst->buffer)
            return STATUS_NO_MEMORY;
        dst->max_length = (uint16_t)(need + 2);
    } else if (need > dst->max_length) {
        return 0x80000005u;
    }
    for (size_t i = 0; i < src->length; i++)
        dst->buffer[i] = (uint8_t)src->buffer[i];
    dst->length = (uint16_t)need;
    if (need + 2 <= dst->max_length)
        dst->buffer[src->length] = 0;
    return 0;
}

static WCHAR fold(WCHAR c, BOOL ci)
{
    return ci && c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c;
}

static LONG WINAPI RtlCompareUnicodeString(const struct unicode_string *a, const struct unicode_string *b, BOOL ci)
{
    size_t n = MIN(a->length, b->length) / 2;
    for (size_t i = 0; i < n; i++) {
        WCHAR x = fold(a->buffer[i], ci), y = fold(b->buffer[i], ci);
        if (x != y)
            return (LONG)x - (LONG)y;
    }
    return (LONG)a->length - (LONG)b->length;
}

static BOOL WINAPI RtlEqualUnicodeString(const struct unicode_string *a, const struct unicode_string *b, BOOL ci)
{
    return a->length == b->length && !RtlCompareUnicodeString(a, b, ci);
}

static DWORD WINAPI RtlDuplicateUnicodeString(ULONG flags, const struct unicode_string *src, struct unicode_string *dst)
{
    if (!src || !dst)
        return STATUS_INVALID_PARAMETER;
    bool terminate = flags & 1;
    size_t bytes = src->length + (terminate ? 2 : 0);
    dst->buffer = bytes ? w32_alloc(bytes) : NULL;
    if (bytes && !dst->buffer)
        return STATUS_NO_MEMORY;
    if (src->length)
        memcpy(dst->buffer, src->buffer, src->length);
    if (terminate)
        dst->buffer[src->length / 2] = 0;
    dst->length = src->length;
    dst->max_length = (uint16_t)bytes;
    return 0;
}

static void WINAPI RtlRunOnceInitialize(void **once)
{
    *once = NULL;
}

static DWORD WINAPI RtlRunOnceExecuteOnce(void **once, void *fn, void *param, void **ctx)
{
    BOOL(WINAPI * exec)(void **, void *, void *, void **) = k32("InitOnceExecuteOnce");
    return exec && exec(once, fn, param, ctx) ? 0 : STATUS_UNSUCCESSFUL;
}

static DWORD WINAPI RtlSleepConditionVariableSRW(void *cv, void *lock, const int64_t *timeout, ULONG flags)
{
    BOOL(WINAPI * sleep)(void *, void *, DWORD, ULONG) = k32("SleepConditionVariableSRW");
    DWORD ms = 0xffffffffu;
    if (timeout)
        ms = *timeout < 0 ? (DWORD)MIN(-*timeout / 10000, 0xfffffffeLL) : 0;
    if (!sleep)
        return STATUS_UNSUCCESSFUL;
    return sleep(cv, lock, ms, flags) ? 0 : STATUS_TIMEOUT;
}

static DWORD WINAPI RtlSleepConditionVariableCS(void *cv, void *cs, const int64_t *timeout)
{
    BOOL(WINAPI * sleep)(void *, void *, DWORD) = k32("SleepConditionVariableCS");
    DWORD ms = timeout ? (*timeout < 0 ? (DWORD)MIN(-*timeout / 10000, 0xfffffffeLL) : 0) : 0xffffffffu;
    return sleep && sleep(cv, cs, ms) ? 0 : STATUS_TIMEOUT;
}

static DWORD WINAPI NtQueryVirtualMemory(HANDLE process, const void *addr, ULONG cls, void *out, SIZE_T len,
                                         SIZE_T *ret)
{
    UNUSED(process);
    if (cls != 0)
        return STATUS_INVALID_INFO_CLASS;
    SIZE_T(WINAPI * query)(const void *, void *, SIZE_T) = k32("VirtualQuery");
    SIZE_T n = query ? query(addr, out, len) : 0;
    if (ret)
        *ret = n;
    return n ? 0 : STATUS_INVALID_PARAMETER;
}

static DWORD WINAPI NtReadVirtualMemory(HANDLE process, const void *base, void *buf, SIZE_T size, SIZE_T *done)
{
    BOOL(WINAPI * read)(HANDLE, const void *, void *, SIZE_T, SIZE_T *) = k32("ReadProcessMemory");
    return read && read(process, base, buf, size, done) ? 0 : 0xc0000022u;
}

/* "\??\C:\path" and "\DosDevices\C:\path" are the native spellings of a Win32 path. */
static const WCHAR *win32_path(const struct unicode_string *name, WCHAR *buf, size_t cap)
{
    size_t n = MIN((size_t)name->length / 2, cap - 1);
    memcpy(buf, name->buffer, n * 2);
    buf[n] = 0;
    if (n >= 4 && buf[0] == '\\' && (buf[1] == '?' || buf[1] == '\\') && buf[2] == '?' && buf[3] == '\\')
        return buf + 4;
    return buf;
}

static DWORD WINAPI NtOpenFile(HANDLE *out, DWORD access, const struct object_attributes *attr, struct io_status *io,
                               ULONG share, ULONG options)
{
    if (!attr || !attr->name)
        return STATUS_INVALID_PARAMETER;
    WCHAR buf[PATH_MAX_LEN];
    const WCHAR *path = win32_path(attr->name, buf, ARRAY_SIZE(buf));
    HANDLE(WINAPI * create)(LPCWSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) = k32("CreateFileW");
    DWORD flags = options & 1 ? 0x02000000u : 0;
    HANDLE h = create ? create(path, access & 0xf01fffu, share, NULL, 3, flags, NULL) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    *out = h;
    if (io)
        *io = (struct io_status){0, 1};
    return 0;
}

static DWORD WINAPI NtReadFile(HANDLE h, HANDLE event, void *apc, void *apc_ctx, struct io_status *io, void *buf,
                               ULONG len, const int64_t *offset, ULONG *key)
{
    UNUSED(event);
    UNUSED(apc);
    UNUSED(apc_ctx);
    UNUSED(key);
    if (offset && *offset >= 0) {
        LONG high = (LONG)(*offset >> 32);
        ((DWORD(WINAPI *)(HANDLE, LONG, LONG *, DWORD))k32("SetFilePointer"))(h, (LONG)*offset, &high, 0);
    }
    DWORD done = 0;
    BOOL ok = ((BOOL(WINAPI *)(HANDLE, void *, DWORD, DWORD *, void *))k32("ReadFile"))(h, buf, len, &done, NULL);
    if (io)
        *io = (struct io_status){ok ? (done || !len ? 0 : 0xc0000011u) : 0xc0000001u, done};
    return !ok ? 0xc0000001u : done || !len ? 0 : 0xc0000011u;
}

static DWORD WINAPI NtMapViewOfSection(HANDLE section, HANDLE process, void **base, ULONG_PTR zero, SIZE_T commit,
                                       const int64_t *offset, SIZE_T *size, DWORD inherit, ULONG type, ULONG protect)
{
    UNUSED(process);
    UNUSED(zero);
    UNUSED(commit);
    UNUSED(inherit);
    UNUSED(type);
    void *(WINAPI * map)(HANDLE, DWORD, DWORD, DWORD, SIZE_T) = k32("MapViewOfFile");
    DWORD access = protect & 0x44 ? 2 : protect & 0x88 ? 1 : 4;
    uint64_t off = offset ? (uint64_t)*offset : 0;
    void *p = map ? map(section, access, (DWORD)(off >> 32), (DWORD)off, size ? *size : 0) : NULL;
    if (!p)
        return 0xc0000018u;
    *base = p;
    if (size && !*size) {
        uint32_t mbi[7];
        SIZE_T(WINAPI * query)(const void *, void *, SIZE_T) = k32("VirtualQuery");
        if (query && query(p, mbi, sizeof(mbi)))
            *size = mbi[3];
    }
    return 0;
}

static DWORD WINAPI NtUnmapViewOfSection(HANDLE process, void *base)
{
    UNUSED(process);
    BOOL(WINAPI * unmap)(void *) = k32("UnmapViewOfFile");
    return unmap && unmap(base) ? 0 : STATUS_INVALID_PARAMETER;
}

static const char *type_name(HANDLE h)
{
    if (h == (HANDLE)(intptr_t)-1)
        return "Process";
    if (h == (HANDLE)(intptr_t)-2)
        return "Thread";
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return NULL;
    static const char *const names[] = {
        [WH_STDIN] = "File",   [WH_STDOUT] = "File",       [WH_STDERR] = "File",   [WH_FILE] = "File",
        [WH_FIND] = "File",    [WH_EVENT] = "Event",       [WH_MUTEX] = "Mutant",  [WH_SEMAPHORE] = "Semaphore",
        [WH_NULL] = "File",    [WH_THREAD] = "Thread",     [WH_PROCESS] = "Process", [WH_TIMER] = "Timer",
        [WH_PIPE] = "File",    [WH_PORT] = "IoCompletion", [WH_SECTION] = "Section", [WH_TOKEN] = "Token",
        [WH_KEY] = "Key",      [WH_JOB] = "Job",
    };
    return (unsigned)o->type < ARRAY_SIZE(names) ? names[o->type] : NULL;
}

/* ObjectTypeInformation names the kind of object; names of objects themselves are not kept */
/* ObjectBasicInformation: what the handle may do (programs check that a file they pass on is read-only) */
static DWORD basic_info(HANDLE h, uint32_t *buf, ULONG len, ULONG *ret)
{
    if (ret)
        *ret = 56;
    if (!buf || len < 56)
        return 0xc0000004u;
    struct w32_object *o = h == (HANDLE)(intptr_t)-1 || h == (HANDLE)(intptr_t)-2 ? NULL : w32_handle_get(h);
    if (!o && h != (HANDLE)(intptr_t)-1 && h != (HANDLE)(intptr_t)-2)
        return 0xc0000008u;
    memset(buf, 0, 56);
    buf[1] = o && o->access ? o->access : o && (o->type == WH_FILE || o->type == WH_PIPE) ? 0x12019fu : 0x1f0fffu;
    buf[2] = 1;
    buf[3] = 1;
    return 0;
}

static DWORD WINAPI NtQueryObject(HANDLE h, ULONG cls, void *buf, ULONG len, ULONG *ret)
{
    if (cls == 0)
        return basic_info(h, buf, len, ret);
    if (cls != 2)
        return 0xc00000bbu;
    const char *name = type_name(h);
    if (!name)
        return 0xc0000008u;
    ULONG n = (ULONG)strlen(name), need = 96 + (n + 1) * 2;
    if (ret)
        *ret = need;
    if (!buf || len < need)
        return 0xc0000004u;
    memset(buf, 0, 96);
    struct unicode_string *u = buf;
    WCHAR *text = (WCHAR *)((uint8_t *)buf + 96);
    for (ULONG i = 0; i <= n; i++)
        text[i] = (uint8_t)name[i];
    *u = (struct unicode_string){(uint16_t)(n * 2), (uint16_t)(n * 2 + 2), text};
    return 0;
}

static ULONG WINAPI RtlNtStatusToDosError(DWORD status)
{
    switch (status) {
    case 0: return 0;
    case STATUS_OBJECT_NAME_NOT_FOUND: return 2;
    case 0xc0000022u: return 5;
    case STATUS_NO_MEMORY: return 8;
    case STATUS_INVALID_PARAMETER: return 87;
    case STATUS_TIMEOUT: return 1460;
    case 0xc00000bbu: return 50;
    default: return 317;
    }
}

static const struct w32_export ntrtl_exports[] = {
    W32_FN(RtlCompareMemory),
    W32_FN(RtlInitAnsiString),
    W32_FN(RtlAnsiStringToUnicodeString),
    W32_FN(RtlCompareUnicodeString),
    W32_FN(RtlEqualUnicodeString),
    W32_FN(RtlDuplicateUnicodeString),
    W32_FN(RtlRunOnceInitialize),
    W32_FN(RtlRunOnceExecuteOnce),
    W32_FN(RtlSleepConditionVariableSRW),
    W32_FN(RtlSleepConditionVariableCS),
    W32_FN(NtQueryVirtualMemory),
    W32_AS("ZwQueryVirtualMemory", NtQueryVirtualMemory),
    W32_FN(NtReadVirtualMemory),
    W32_FN(NtOpenFile),
    W32_FN(NtReadFile),
    W32_FN(NtMapViewOfSection),
    W32_FN(NtUnmapViewOfSection),
    W32_FN(NtQueryObject),
    W32_AS("ZwQueryObject", NtQueryObject),
    W32_FN(RtlNtStatusToDosError),
};

static const struct w32_export ntrtl_kernel32_exports[] = {
    W32_FN(RtlCompareMemory),
};

W32_EXPORTS(ntrtl_module, "ntdll.dll", ntrtl_exports);
W32_EXPORTS(ntrtl_kernel32_module, "kernel32.dll", ntrtl_kernel32_exports);
