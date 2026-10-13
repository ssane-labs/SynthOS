#include <synth/error.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

#define STATUS_NOT_IMPLEMENTED 0xc0000002u
#define STATUS_INVALID_INFO_CLASS 0xc0000003u
#define STATUS_INVALID_HANDLE 0xc0000008u

static BOOL unsupported(void)
{
    w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

static HANDLE WINAPI CreateRemoteThread(HANDLE p, void *sa, SIZE_T stack, void *start, void *param, DWORD flags,
                                        DWORD *id)
{
    UNUSED(p);
    UNUSED(sa);
    UNUSED(stack);
    UNUSED(start);
    UNUSED(param);
    UNUSED(flags);
    UNUSED(id);
    unsupported();
    return NULL;
}

static HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{
    UNUSED(flags);
    UNUSED(pid);
    unsupported();
    return INVALID_HANDLE_VALUE;
}

static BOOL WINAPI unsupported1(uint32_t a)
{
    UNUSED(a);
    return unsupported();
}

static BOOL WINAPI unsupported2(uint32_t a, uint32_t b)
{
    UNUSED(a);
    UNUSED(b);
    return unsupported();
}

static BOOL WINAPI unsupported3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    return unsupported();
}

static BOOL WINAPI unsupported5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    return unsupported();
}

static BOOL WINAPI unsupported7(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{
    UNUSED(f);
    UNUSED(g);
    return unsupported5(a, b, c, d, e);
}

static BOOL WINAPI unsupported8(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                                uint32_t h)
{
    UNUSED(h);
    return unsupported7(a, b, c, d, e, f, g);
}

static BOOL WINAPI SetProcessAffinityMask(HANDLE p, DWORD mask)
{
    UNUSED(p);
    UNUSED(mask);
    return TRUE;
}

static BOOL rename_open(struct w32_object *o, const uint8_t *info, DWORD size)
{
    if (size < 12)
        return FALSE;
    uint32_t flags = *(const uint32_t *)info, len = *(const uint32_t *)(info + 8);
    const WCHAR *name = (const WCHAR *)(info + 12);
    if (12 + len > size)
        return FALSE;
    WCHAR *copy = w32_alloc(len + 2);
    if (!copy)
        return FALSE;
    memcpy(copy, name, len);
    copy[len / 2] = 0;
    char *win = w32_narrow(copy);
    w32_free(copy);
    char target[PATH_MAX_LEN];
    int err = win ? w32_path(win, target, sizeof(target)) : E_NOMEM;
    w32_free(win);
    if (err)
        return FALSE;
    if (vfs_exists(target)) {
        if (!(flags & 1)) {
            w32_set_error(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
        vfs_unlink(target);
    }
    if (vfs_rename(o->path, target) != 0) {
        w32_set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    kfree(o->path);
    o->path = kstrdup(target);
    return TRUE;
}

static BOOL WINAPI SetFileInformationByHandle(HANDLE h, DWORD cls, uint8_t *info, DWORD size)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_FILE || !info) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (cls) {
    case 0: return TRUE;
    case 3:
    case 22: return o->path && rename_open(o, info, size);
    case 4: o->delete_on_close = info[0] != 0; return TRUE;
    case 21: o->delete_on_close = (*(uint32_t *)info & 1) != 0; return TRUE;
    case 5: return TRUE;
    case 6: {
        uint32_t end = *(uint32_t *)info;
        return o->path && vfs_truncate(o->path, end) == 0;
    }
    default: return unsupported();
    }
}

static LONG WINAPI CompareFileTime(const uint32_t *a, const uint32_t *b)
{
    uint64_t x = (uint64_t)a[1] << 32 | a[0], y = (uint64_t)b[1] << 32 | b[0];
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef BOOL(WINAPI *info_fn)(HANDLE, DWORD, void *, DWORD);

static DWORD WINAPI NtQueryInformationFile(HANDLE h, uint32_t *iosb, void *out, DWORD size, DWORD cls)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return STATUS_INVALID_HANDLE;
    int ex = cls == 4 ? 0 : cls == 5 ? 1 : cls == 9 ? 2 : -1;
    if (ex < 0)
        return STATUS_INVALID_INFO_CLASS;
    info_fn fn = (info_fn)w32_builtin_lookup("kernel32.dll", "GetFileInformationByHandleEx");
    if (!fn || !fn(h, (DWORD)ex, out, size))
        return STATUS_INVALID_INFO_CLASS;
    if (iosb) {
        iosb[0] = 0;
        iosb[1] = size;
    }
    return 0;
}

static DWORD WINAPI nt_not_implemented4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return STATUS_NOT_IMPLEMENTED;
}

static DWORD WINAPI nt_not_implemented5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    UNUSED(e);
    return nt_not_implemented4(a, b, c, d);
}

static DWORD WINAPI NtQueryDirectoryFile(HANDLE h, HANDLE ev, void *apc, void *ctx, void *iosb, void *out, DWORD size,
                                         DWORD cls, BOOL single, void *mask, BOOL restart)
{
    UNUSED(h);
    UNUSED(ev);
    UNUSED(apc);
    UNUSED(ctx);
    UNUSED(iosb);
    UNUSED(out);
    UNUSED(size);
    UNUSED(cls);
    UNUSED(single);
    UNUSED(mask);
    UNUSED(restart);
    return STATUS_NOT_IMPLEMENTED;
}

static const struct w32_export compat_kernel32[] = {
    W32_FN(CreateRemoteThread),
    W32_FN(CreateToolhelp32Snapshot),
    W32_AS("Process32First", unsupported2),
    W32_AS("Process32Next", unsupported2),
    W32_AS("Process32FirstW", unsupported2),
    W32_AS("Process32NextW", unsupported2),
    W32_AS("GetThreadContext", unsupported2),
    W32_AS("SetThreadContext", unsupported2),
    W32_AS("DisconnectNamedPipe", unsupported1),
    W32_AS("EnumResourceLanguagesA", unsupported5),
    W32_AS("EnumResourceLanguagesW", unsupported5),
    W32_AS("GetCurrentConsoleFontEx", unsupported3),
    W32_AS("GetLogicalProcessorInformationEx", unsupported3),
    W32_AS("GetNamedPipeHandleStateA", unsupported7),
    W32_AS("GetNamedPipeHandleStateW", unsupported7),
    W32_AS("GetNamedPipeInfo", unsupported5),
    W32_AS("ReadDirectoryChangesW", unsupported8),
    W32_AS("SetSystemTime", unsupported1),
    W32_AS("WaitNamedPipeA", unsupported2),
    W32_AS("WaitNamedPipeW", unsupported2),
    W32_FN(SetProcessAffinityMask),
    W32_FN(SetFileInformationByHandle),
    W32_FN(CompareFileTime),
};

static const struct w32_export compat_ntdll[] = {
    W32_FN(NtQueryInformationFile),
    W32_FN(NtQueryDirectoryFile),
    W32_AS("NtQueryObject", nt_not_implemented5),
    W32_AS("NtSetEaFile", nt_not_implemented4),
};

W32_EXPORTS(compat_kernel32_module, "kernel32.dll", compat_kernel32);
W32_EXPORTS(compat_ntdll_module, "ntdll.dll", compat_ntdll);
