#include <synth/error.h>
#include <synth/heap.h>
#include <synth/log.h>
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

/* A process snapshot lists the system entries and the Windows programs that run; the walk reads the ring of
   processes as it is when each entry is asked for. */
static HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{
    UNUSED(pid);
    HANDLE h = w32_handle_new(WH_SNAPSHOT);
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return INVALID_HANDLE_VALUE;
    o->max = flags;
    return h;
}

static struct w32_process *process_at(uint32_t index)
{
    struct w32_process *p = w32;
    for (uint32_t i = 0; p; i++) {
        if (i == index)
            return p;
        p = w32_ring_next(p);
        if (p == w32)
            break;
    }
    return NULL;
}

static uint32_t pid_of(struct w32_process *p)
{
    return p->child ? p->child->pid : 0x100;
}

static BOOL process_entry(HANDLE snap, uint8_t *e, bool wide, bool first)
{
    struct w32_object *o = w32_handle_get(snap);
    if (!o || o->type != WH_SNAPSHOT || !e || !(o->max & 2)) {
        w32_set_error(o ? ERROR_NO_MORE_FILES : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (first)
        o->value = 0;
    uint32_t i = o->value++, pid = 0, parent = 0, threads = 1;
    const char *name;
    if (i == 0) {
        name = "[System Process]";
    } else if (i == 1) {
        pid = 4;
        name = "System";
    } else {
        struct w32_process *p = process_at(i - 2);
        if (!p) {
            o->value--;
            w32_set_error(ERROR_NO_MORE_FILES);
            return FALSE;
        }
        pid = pid_of(p);
        parent = p->parent ? pid_of(p->parent) : p->parent_pid;
        threads = p->live_threads > 0 ? (uint32_t)p->live_threads : 1;
        name = p->main ? p->main->name : "?";
    }
    bool pe64 = w32->win64;
    uint32_t at_threads = pe64 ? 28 : 20, at_parent = pe64 ? 32 : 24, at_pri = pe64 ? 36 : 28,
             at_name = pe64 ? 44 : 36;
    memset(e + 4, 0, at_name - 4);
    memcpy(e + 8, &pid, 4);
    memcpy(e + at_threads, &threads, 4);
    memcpy(e + at_parent, &parent, 4);
    uint32_t pri = 8;
    memcpy(e + at_pri, &pri, 4);
    for (size_t k = 0; k < 260; k++) {
        char c = k < 259 ? name[k] : 0;
        if (wide) {
            e[at_name + k * 2] = (uint8_t)c;
            e[at_name + k * 2 + 1] = 0;
        } else {
            e[at_name + k] = (uint8_t)c;
        }
        if (!c)
            break;
    }
    return TRUE;
}

static BOOL WINAPI Process32First(HANDLE snap, void *e)
{
    return process_entry(snap, e, false, true);
}

static BOOL WINAPI Process32Next(HANDLE snap, void *e)
{
    return process_entry(snap, e, false, false);
}

static BOOL WINAPI Process32FirstW(HANDLE snap, void *e)
{
    return process_entry(snap, e, true, true);
}

static BOOL WINAPI Process32NextW(HANDLE snap, void *e)
{
    return process_entry(snap, e, true, false);
}

static BOOL WINAPI Thread32First(HANDLE snap, void *e)
{
    UNUSED(e);
    struct w32_object *o = w32_handle_get(snap);
    w32_set_error(o && o->type == WH_SNAPSHOT ? ERROR_NO_MORE_FILES : ERROR_INVALID_HANDLE);
    return FALSE;
}

/* Activation contexts pick side-by-side DLL versions (comctl32 6 for the visual styles); there is one version of each
   built-in DLL, so a context is a token that activates nothing. */
static HANDLE WINAPI CreateActCtxW(const void *ctx)
{
    UNUSED(ctx);
    return (HANDLE)0x5ac70001u;
}

static BOOL WINAPI ActivateActCtx(HANDLE ctx, ULONG_PTR *cookie)
{
    UNUSED(ctx);
    if (cookie)
        *cookie = 1;
    return TRUE;
}

static BOOL WINAPI DeactivateActCtx(DWORD flags, ULONG_PTR cookie)
{
    UNUSED(flags);
    UNUSED(cookie);
    return TRUE;
}

static void WINAPI ReleaseActCtx(HANDLE ctx)
{
    UNUSED(ctx);
}

static BOOL WINAPI GetCurrentActCtx(HANDLE *ctx)
{
    *ctx = NULL;
    return TRUE;
}

static BOOL WINAPI QueryActCtxW(DWORD flags, HANDLE ctx, void *sub, ULONG cls, void *buf, SIZE_T size, SIZE_T *done)
{
    UNUSED(flags);
    UNUSED(ctx);
    UNUSED(sub);
    uint32_t need = cls == 1 ? (w32->win64 ? 16 : 8) : 0;
    if (done)
        *done = need;
    if (!need) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!buf || size < need) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    /* ActivationContextBasicInformation: the context and its flags */
    memset(buf, 0, need);
    uint32_t h = 0x5ac70001u;
    memcpy(buf, &h, 4);
    return TRUE;
}

static BOOL WINAPI K32EnumProcesses(DWORD *pids, DWORD size, DWORD *needed)
{
    DWORD n = 0, cap = size / 4;
    if (n < cap)
        pids[n] = 0;
    n++;
    if (n < cap)
        pids[n] = 4;
    n++;
    for (struct w32_process *p; (p = process_at(n - 2)); n++)
        if (n < cap)
            pids[n] = pid_of(p);
    if (n > cap)
        n = cap;
    *needed = n * 4;
    return TRUE;
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
    if (err) {
        w32_set_error(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (vfs_exists(target)) {
        if (!(flags & 1)) {
            w32_set_error(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
        vfs_unlink(target);
    }
    w32_fcache_forget(o->path);
    w32_fcache_forget(target);
    if ((err = vfs_rename(o->path, target)) != 0) {
        if (w32_logging_errors())
            klog("win32: rename %s -> %s: %d", o->path, target, err);
        w32_set_error(w32_error_from_vfs(err));
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

static DWORD WINAPI nt_not_implemented4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return STATUS_NOT_IMPLEMENTED;
}

static const struct w32_export compat_kernel32[] = {
    W32_FN(CreateRemoteThread),
    W32_FN(CreateToolhelp32Snapshot),
    W32_FN(Process32First),
    W32_FN(Process32Next),
    W32_FN(Thread32First),
    W32_AS("Thread32Next", Thread32First),
    W32_FN(Process32FirstW),
    W32_FN(Process32NextW),
    W32_FN(K32EnumProcesses),
    W32_FN(CreateActCtxW),
    W32_AS("CreateActCtxA", CreateActCtxW),
    W32_FN(ActivateActCtx),
    W32_FN(DeactivateActCtx),
    W32_FN(ReleaseActCtx),
    W32_AS("AddRefActCtx", ReleaseActCtx),
    W32_FN(GetCurrentActCtx),
    W32_FN(QueryActCtxW),
    W32_AS("EnumProcesses", K32EnumProcesses),
    W32_AS("GetThreadContext", unsupported2),
    W32_AS("SetThreadContext", unsupported2),
    W32_AS("EnumResourceLanguagesA", unsupported5),
    W32_AS("EnumResourceLanguagesW", unsupported5),
    W32_AS("GetCurrentConsoleFontEx", unsupported3),
    W32_AS("GetLogicalProcessorInformationEx", unsupported3),
    W32_AS("ReadDirectoryChangesW", unsupported8),
    W32_AS("SetSystemTime", unsupported1),
    W32_FN(SetProcessAffinityMask),
    W32_FN(SetFileInformationByHandle),
    W32_FN(CompareFileTime),
};

static const struct w32_export compat_ntdll[] = {
    W32_AS("NtSetEaFile", nt_not_implemented4),
};

W32_EXPORTS(compat_kernel32_module, "kernel32.dll", compat_kernel32);
W32_EXPORTS(compat_ntdll_module, "ntdll.dll", compat_ntdll);
