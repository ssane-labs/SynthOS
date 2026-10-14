#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

#include "win32.h"

#define S_OK 0
#define E_INVALIDARG 0x80070057u
#define E_BUFFER_TOO_SMALL 0x8007007au
#define E_FILE_NOT_FOUND 0x80070002u

static bool is_sep(WCHAR c)
{
    return c == '\\' || c == '/';
}

static size_t root_length(const WCHAR *p)
{
    if (!p || !p[0])
        return 0;
    if (is_sep(p[0]) && is_sep(p[1])) {
        size_t i = 2;
        if (p[2] == '?' && is_sep(p[3])) {
            i = 4;
            if (p[i] && p[i + 1] == ':')
                return is_sep(p[i + 2]) ? i + 3 : i + 2;
        }
        for (int part = 0; part < 2; part++) {
            while (p[i] && !is_sep(p[i]))
                i++;
            if (p[i])
                i++;
        }
        return i;
    }
    if (p[0] && p[1] == ':')
        return is_sep(p[2]) ? 3 : 2;
    return is_sep(p[0]) ? 1 : 0;
}

static uint32_t WINAPI PathCchSkipRoot(const WCHAR *path, const WCHAR **end)
{
    size_t n = root_length(path);
    if (!n) {
        if (end)
            *end = NULL;
        return E_INVALIDARG;
    }
    *end = path + n;
    return S_OK;
}

static size_t canonicalize(const WCHAR *in, WCHAR *out, size_t cap)
{
    size_t root = root_length(in), o = 0;
    if (root >= cap)
        return (size_t)-1;
    for (size_t i = 0; i < root; i++)
        out[o++] = is_sep(in[i]) ? '\\' : in[i];
    size_t base = o;
    const WCHAR *p = in + root;
    while (*p) {
        while (is_sep(*p))
            p++;
        const WCHAR *s = p;
        while (*p && !is_sep(*p))
            p++;
        size_t len = (size_t)(p - s);
        if (!len || (len == 1 && s[0] == '.'))
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (o > base && out[o - 1] != '\\')
                o--;
            if (o > base)
                o--;
            continue;
        }
        if (o > base || (o && out[o - 1] != '\\' && root))
            out[o++] = '\\';
        if (o + len + 1 >= cap)
            return (size_t)-1;
        memcpy(out + o, s, len * sizeof(WCHAR));
        o += len;
    }
    if (o > base && is_sep(in[w32_wcslen(in) ? w32_wcslen(in) - 1 : 0]) && o + 1 < cap)
        out[o++] = '\\';
    if (!o)
        out[o++] = '\\';
    out[o] = 0;
    return o;
}

static uint32_t WINAPI PathCchCanonicalizeEx(WCHAR *out, size_t cap, const WCHAR *in, ULONG flags)
{
    UNUSED(flags);
    if (!out || !in)
        return E_INVALIDARG;
    WCHAR *tmp = w32_alloc((w32_wcslen(in) + 2) * sizeof(WCHAR));
    if (!tmp)
        return E_INVALIDARG;
    size_t n = canonicalize(in, tmp, w32_wcslen(in) + 2);
    uint32_t r = n == (size_t)-1 || n + 1 > cap ? E_BUFFER_TOO_SMALL : S_OK;
    if (r == S_OK)
        memcpy(out, tmp, (n + 1) * sizeof(WCHAR));
    else if (cap)
        out[0] = 0;
    w32_free(tmp);
    return r;
}

static uint32_t WINAPI PathCchCombineEx(WCHAR *out, size_t cap, const WCHAR *a, const WCHAR *b, ULONG flags)
{
    UNUSED(flags);
    if (!out)
        return E_INVALIDARG;
    size_t la = a ? w32_wcslen(a) : 0, lb = b ? w32_wcslen(b) : 0;
    WCHAR *joined = w32_alloc((la + lb + 4) * sizeof(WCHAR));
    if (!joined)
        return E_INVALIDARG;
    size_t n = 0;
    if (b && root_length(b) && !(is_sep(b[0]) && !is_sep(b[1]))) {
        memcpy(joined, b, lb * sizeof(WCHAR));
        n = lb;
    } else if (b && is_sep(b[0]) && a && a[0] && a[1] == ':') {
        joined[n++] = a[0];
        joined[n++] = ':';
        memcpy(joined + n, b, lb * sizeof(WCHAR));
        n += lb;
    } else {
        if (la)
            memcpy(joined, a, la * sizeof(WCHAR));
        n = la;
        if (n && lb && !is_sep(joined[n - 1]))
            joined[n++] = '\\';
        if (lb)
            memcpy(joined + n, b, lb * sizeof(WCHAR));
        n += lb;
    }
    joined[n] = 0;
    uint32_t r = PathCchCanonicalizeEx(out, cap, joined, 0);
    w32_free(joined);
    return r;
}

static uint32_t WINAPI PathCchCombine(WCHAR *out, size_t cap, const WCHAR *a, const WCHAR *b)
{
    return PathCchCombineEx(out, cap, a, b, 0);
}

static uint32_t WINAPI PathCchCanonicalize(WCHAR *out, size_t cap, const WCHAR *in)
{
    return PathCchCanonicalizeEx(out, cap, in, 0);
}

static BOOL WINAPI PathIsUNCEx(const WCHAR *p, const WCHAR **server)
{
    bool unc = p && is_sep(p[0]) && is_sep(p[1]) && p[2] != '?';
    if (server)
        *server = unc ? p + 2 : NULL;
    return unc;
}

static uint32_t WINAPI PathCchRemoveFileSpec(WCHAR *p, size_t cap)
{
    UNUSED(cap);
    size_t root = root_length(p), n = w32_wcslen(p);
    while (n > root && !is_sep(p[n - 1]))
        n--;
    while (n > root && is_sep(p[n - 1]))
        n--;
    bool changed = p[n] != 0;
    p[n] = 0;
    return changed ? S_OK : 1;
}

static DWORD final_path(HANDLE h, char *out, size_t cap)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || !o->path) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    char win[PATH_MAX_LEN];
    w32_path_to_win(o->path, win, sizeof(win));
    ksnprintf(out, cap, "\\\\?\\%s", win);
    return (DWORD)strlen(out);
}

static DWORD WINAPI GetFinalPathNameByHandleW(HANDLE h, WCHAR *buf, DWORD cch, DWORD flags)
{
    UNUSED(flags);
    char path[PATH_MAX_LEN + 8];
    DWORD n = final_path(h, path, sizeof(path));
    if (!n)
        return 0;
    if (n + 1 > cch)
        return n + 1;
    for (DWORD i = 0; i <= n; i++)
        buf[i] = (WCHAR)(uint8_t)path[i];
    return n;
}

static DWORD WINAPI GetFinalPathNameByHandleA(HANDLE h, char *buf, DWORD cch, DWORD flags)
{
    UNUSED(flags);
    char path[PATH_MAX_LEN + 8];
    DWORD n = final_path(h, path, sizeof(path));
    if (!n)
        return 0;
    if (n + 1 > cch)
        return n + 1;
    memcpy(buf, path, n + 1);
    return n;
}

static void put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static uint64_t filetime(uint32_t epoch)
{
    return ((uint64_t)epoch + 11644473600ull) * 10000000ull;
}

static BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, int cls, uint8_t *buf, DWORD size)
{
    struct w32_object *o = w32_handle_get(h);
    struct vstat st = {0};
    if (!o) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (o->type == WH_FILE)
        vfs_fstat(o->file, &st);
    DWORD attr = o->type != WH_FILE ? 0x80 : st.type == VN_DIR ? 0x10 : 0x20;
    memset(buf, 0, size);
    switch (cls) {
    case 0:
        if (size < 40)
            break;
        for (int i = 0; i < 4; i++)
            put64(buf + i * 8, filetime(st.mtime));
        memcpy(buf + 32, &attr, 4);
        return TRUE;
    case 1:
        if (size < 24)
            break;
        put64(buf, (st.size + 4095) & ~4095u);
        put64(buf + 8, st.size);
        memcpy(buf + 16, &(DWORD){1}, 4);
        buf[21] = st.type == VN_DIR;
        return TRUE;
    case 2: {
        char path[PATH_MAX_LEN + 8];
        if (!final_path(h, path, sizeof(path)))
            return FALSE;
        const char *p = path + 6;
        DWORD n = (DWORD)strlen(p);
        if (size < 4 + n * 2) {
            w32_set_error(234);
            return FALSE;
        }
        DWORD bytes = n * 2;
        memcpy(buf, &bytes, 4);
        for (DWORD i = 0; i < n; i++)
            ((WCHAR *)(buf + 4))[i] = (WCHAR)(uint8_t)p[i];
        return TRUE;
    }
    case 9:
        if (size < 8)
            break;
        memcpy(buf, &attr, 4);
        return TRUE;
    case 18:
        if (size < 24)
            break;
        put64(buf, 0x53594e54);
        memcpy(buf + 8, &st.inode, 4);
        return TRUE;
    case 4:
    case 5:
        if (size < 24)
            break;
        put64(buf, st.size);
        put64(buf + 8, (st.size + 4095) & ~4095u);
        return TRUE;
    default:
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    w32_set_error(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
}

static DWORD WINAPI GetActiveProcessorCount(WORD group)
{
    UNUSED(group);
    return 1;
}

static BOOL WINAPI GetProcessTimes(HANDLE h, uint8_t *created, uint8_t *exited, uint8_t *kernel, uint8_t *user)
{
    UNUSED(h);
    uint64_t now = filetime((uint32_t)time_now()), ticks = hal_ticks_ms() * 10000ull;
    if (created)
        put64(created, now - ticks);
    if (exited)
        put64(exited, 0);
    if (kernel)
        put64(kernel, 0);
    if (user)
        put64(user, ticks);
    return TRUE;
}

static BOOL WINAPI GetThreadTimes(HANDLE h, uint8_t *created, uint8_t *exited, uint8_t *kernel, uint8_t *user)
{
    return GetProcessTimes(h, created, exited, kernel, user);
}

static void *WINAPI AddVectoredExceptionHandler(ULONG first, void *handler)
{
    UNUSED(first);
    return handler;
}

static void *WINAPI AddDllDirectory(const WCHAR *dir)
{
    return (void *)dir;
}

static uint64_t WINAPI VerSetConditionMask(uint64_t mask, DWORD type, BYTE cond)
{
    for (int i = 0; i < 8; i++)
        if (type & (1u << i))
            mask |= (uint64_t)(cond & 7) << (i * 3);
    return mask;
}

static BOOL WINAPI GetComputerNameExW(int format, WCHAR *buf, DWORD *size)
{
    UNUSED(format);
    static const char name[] = "synth";
    if (!size || *size < sizeof(name)) {
        if (size)
            *size = sizeof(name);
        w32_set_error(234);
        return FALSE;
    }
    for (size_t i = 0; i < sizeof(name); i++)
        buf[i] = (WCHAR)name[i];
    *size = sizeof(name) - 1;
    return TRUE;
}

static DWORD WINAPI GetLogicalDriveStringsW(DWORD len, WCHAR *buf)
{
    static const WCHAR drives[] = {'C', ':', '\\', 0, 0};
    if (len < ARRAY_SIZE(drives))
        return ARRAY_SIZE(drives);
    memcpy(buf, drives, sizeof(drives));
    return ARRAY_SIZE(drives) - 1;
}

static BOOL WINAPI GetVolumePathNameW(const WCHAR *path, WCHAR *out, DWORD len)
{
    UNUSED(path);
    if (len < 4)
        return FALSE;
    out[0] = 'C';
    out[1] = ':';
    out[2] = '\\';
    out[3] = 0;
    return TRUE;
}

static HANDLE WINAPI FindFirstVolumeW(WCHAR *name, DWORD len)
{
    UNUSED(name);
    UNUSED(len);
    w32_set_error(ERROR_NO_MORE_FILES);
    return INVALID_HANDLE_VALUE;
}

static uint32_t WINAPI CopyFile2(const WCHAR *from, const WCHAR *to, const void *params)
{
    UNUSED(params);
    char *a = w32_narrow(from), *b = w32_narrow(to);
    char pa[PATH_MAX_LEN], pb[PATH_MAX_LEN];
    int err = !a || !b ? E_NOMEM : w32_path(a, pa, sizeof(pa)) || w32_path(b, pb, sizeof(pb)) ? E_NOENT
                                                                                            : vfs_copy(pa, pb);
    w32_free(a);
    w32_free(b);
    return err ? 0x80070000u | w32_error_from_vfs(err) : S_OK;
}

static BOOL WINAPI GetSystemTimeAdjustment(DWORD *adjust, DWORD *increment, BOOL *disabled)
{
    *adjust = 156250;
    *increment = 156250;
    *disabled = TRUE;
    return TRUE;
}

static HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    UNUSED(access);
    UNUSED(inherit);
    if (!pid) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (pid == w32_current_pid() && !w32->child)
        return W32_PSEUDO_PROCESS;
    struct w32_process *p = w32_process_by_pid(pid);
    if (p && p->child) {
        HANDLE h = w32_handle_new(WH_PROCESS);
        struct w32_object *n = w32_handle_get(h);
        if (n) {
            n->max = pid;
            n->child = p->child;
            n->child->refs++;
        }
        return h;
    }
    for (int i = 0; i < W32_MAX_HANDLES; i++) {
        struct w32_object *o = &w32->handles[i];
        if (o->type == WH_PROCESS && o->max == pid) {
            HANDLE h = w32_handle_new(WH_PROCESS);
            struct w32_object *n = w32_handle_get(h);
            if (n) {
                n->value = o->value;
                n->max = pid;
                n->child = o->child;
                if (n->child)
                    n->child->refs++;
            }
            return h;
        }
    }
    w32_set_error(ERROR_INVALID_PARAMETER);
    return NULL;
}

static HANDLE new_timer(bool manual)
{
    HANDLE h = w32_handle_new(WH_TIMER);
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->manual = manual;
    return h;
}

static HANDLE WINAPI CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD access)
{
    UNUSED(sa);
    UNUSED(name);
    UNUSED(access);
    return new_timer(flags & 1);
}

static HANDLE WINAPI CreateWaitableTimerW(void *sa, BOOL manual, const WCHAR *name)
{
    UNUSED(sa);
    UNUSED(name);
    return new_timer(manual);
}

static BOOL WINAPI SetWaitableTimerEx(HANDLE h, const int64_t *due, LONG period, void *cb, void *arg, void *ctx,
                                      ULONG delay)
{
    UNUSED(period);
    UNUSED(cb);
    UNUSED(arg);
    UNUSED(ctx);
    UNUSED(delay);
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_TIMER) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    int64_t d = *due;
    uint64_t now = hal_ticks_ms();
    if (d < 0) {
        o->due = now + (uint64_t)((-d + 9999) / 10000);
    } else {
        uint64_t wall = filetime((uint32_t)time_now());
        o->due = (uint64_t)d > wall ? now + ((uint64_t)d - wall) / 10000 : now;
    }
    if (!o->due)
        o->due = 1;
    return TRUE;
}

static BOOL WINAPI SetWaitableTimer(HANDLE h, const int64_t *due, LONG period, void *cb, void *arg, BOOL resume)
{
    UNUSED(resume);
    return SetWaitableTimerEx(h, due, period, cb, arg, NULL, 0);
}

static BOOL WINAPI CancelWaitableTimer(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (o && o->type == WH_TIMER)
        o->due = 0;
    return o != NULL;
}

#define PORT_SLOTS 256

struct w32_completion {
    DWORD bytes;
    uintptr_t key;
    void *overlapped;
};

struct w32_port {
    struct w32_completion queue[PORT_SLOTS];
    unsigned head, tail;
};

static struct w32_port *port_of(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    return o && o->type == WH_PORT ? o->port : NULL;
}

static HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE port, uintptr_t key, DWORD threads)
{
    UNUSED(threads);
    if (port && port_of(port)) {
        struct w32_object *o = file != INVALID_HANDLE_VALUE ? w32_handle_get(file) : NULL;
        if (o) {
            o->io_port = port;
            o->io_key = key;
        } else if (file != INVALID_HANDLE_VALUE) {
            w32_socket_associate((uintptr_t)file, port, key);
        }
        return port;
    }
    if (file != INVALID_HANDLE_VALUE && port) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    HANDLE h = w32_handle_new(WH_PORT);
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return NULL;
    o->port = kzalloc(sizeof(struct w32_port));
    if (!o->port) {
        w32_handle_close(h);
        return NULL;
    }
    if (file != INVALID_HANDLE_VALUE)
        return CreateIoCompletionPort(file, h, key, threads);
    return h;
}

static BOOL WINAPI PostQueuedCompletionStatus(HANDLE h, DWORD bytes, uintptr_t key, void *ov)
{
    w32_signal();
    struct w32_port *p = port_of(h);
    if (!p || p->tail - p->head >= PORT_SLOTS) {
        w32_set_error(p ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    p->queue[p->tail++ % PORT_SLOTS] = (struct w32_completion){bytes, key, ov};
    return TRUE;
}

void w32_port_post(HANDLE port, DWORD bytes, uintptr_t key, void *ov)
{
    PostQueuedCompletionStatus(port, bytes, key, ov);
}

static bool port_wait(struct w32_port *p, DWORD ms)
{
    uint64_t deadline = ms == INFINITE ? 0 : hal_ticks_ms() + ms;
    w32_winsock_poll();
    w32_io_poll();
    while (p->head == p->tail) {
        if (ms == 0 || (ms != INFINITE && hal_ticks_ms() >= deadline))
            return false;
        w32_block();
        w32_winsock_poll();
        w32_io_poll();
    }
    return true;
}

static BOOL WINAPI GetOverlappedResult(HANDLE h, uint32_t *ov, DWORD *bytes, BOOL wait)
{
    UNUSED(h);
    w32_io_poll();
    while (ov[0] == 0x103u && wait) {
        w32_winsock_poll();
        w32_io_poll();
        if (ov[0] == 0x103u)
            w32_block();
    }
    if (ov[0] == 0x103u) {
        w32_set_error(996);
        return FALSE;
    }
    if (bytes)
        *bytes = ov[1];
    if (ov[0]) {
        w32_set_error(ov[0] == 0xc0000120u ? 995 : 64);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI CancelIoEx(HANDLE h, uint32_t *ov)
{
    if (w32_io_cancel(h, ov) || w32_winsock_cancel((UINT)(uintptr_t)h, ov))
        return TRUE;
    w32_set_error(1168);
    return FALSE;
}

static BOOL WINAPI CancelIo(HANDLE h)
{
    return CancelIoEx(h, NULL);
}

static BOOL WINAPI GetQueuedCompletionStatus(HANDLE h, DWORD *bytes, uintptr_t *key, void **ov, DWORD ms)
{
    struct w32_port *p = port_of(h);
    if (ov)
        *ov = NULL;
    if (!p) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!port_wait(p, ms)) {
        w32_set_error(258);
        return FALSE;
    }
    struct w32_completion c = p->queue[p->head++ % PORT_SLOTS];
    if (bytes)
        *bytes = c.bytes;
    if (key)
        *key = c.key;
    if (ov)
        *ov = c.overlapped;
    return TRUE;
}

static BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE h, uint32_t *entries, ULONG count, ULONG *removed, DWORD ms,
                                               BOOL alertable)
{
    UNUSED(alertable);
    struct w32_port *p = port_of(h);
    if (removed)
        *removed = 0;
    if (!p) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!port_wait(p, ms)) {
        w32_set_error(258);
        return FALSE;
    }
    ULONG n = 0;
    while (n < count && p->head != p->tail) {
        struct w32_completion c = p->queue[p->head++ % PORT_SLOTS];
        entries[n * 4] = (uint32_t)c.key;
        entries[n * 4 + 1] = (uint32_t)c.overlapped;
        entries[n * 4 + 2] = 0;
        entries[n * 4 + 3] = c.bytes;
        n++;
    }
    if (removed)
        *removed = n;
    return TRUE;
}

static BOOL WINAPI not_implemented5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

static BOOL WINAPI not_implemented4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return not_implemented5(a, b, c, d, 0);
}

static BOOL WINAPI not_implemented3(uint32_t a, uint32_t b, uint32_t c)
{
    return not_implemented5(a, b, c, 0, 0);
}

static BOOL WINAPI false7(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    UNUSED(f);
    UNUSED(g);
    w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

static BOOL WINAPI false8(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    UNUSED(h);
    return false7(a, b, c, d, e, f, g);
}

static BOOL WINAPI false6(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{
    return false7(a, b, c, d, e, f, 0);
}

static void *WINAPI no_pointer3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

static uint32_t WINAPI pss_failed4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static uint32_t WINAPI pss_failed2(uint32_t a, uint32_t b)
{
    return pss_failed4(a, b, 0, 0);
}

#define ATTRIBUTE_SLOTS_MAX 16

static BOOL WINAPI InitializeProcThreadAttributeList(uint32_t *list, DWORD count, DWORD flags, SIZE_T *size)
{
    UNUSED(flags);
    SIZE_T need = 16 + count * 16;
    if (!list || !size || *size < need) {
        if (size)
            *size = need;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(list, 0, need);
    list[0] = MIN(count, ATTRIBUTE_SLOTS_MAX);
    return TRUE;
}

static BOOL WINAPI UpdateProcThreadAttribute(uint32_t *list, DWORD flags, uintptr_t attribute, void *value,
                                             SIZE_T size, void *previous, SIZE_T *returned)
{
    UNUSED(flags);
    UNUSED(previous);
    UNUSED(returned);
    if (!list || list[1] >= list[0]) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint32_t *slot = list + 4 + list[1]++ * 4;
    slot[0] = (uint32_t)attribute;
    slot[1] = (uint32_t)(uintptr_t)value;
    slot[2] = (uint32_t)size;
    return TRUE;
}

const HANDLE *w32_attribute_handles(const void *attributes, int *count)
{
    const uint32_t *list = attributes;
    *count = 0;
    for (uint32_t i = 0; list && i < list[1] && i < ATTRIBUTE_SLOTS_MAX; i++)
        if (list[4 + i * 4] == 0x20002u) {
            *count = (int)(list[4 + i * 4 + 2] / sizeof(HANDLE));
            return (const HANDLE *)(uintptr_t)list[4 + i * 4 + 1];
        }
    return NULL;
}

static BOOL WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (mask & 1)
        o->inherit = flags & 1;
    return TRUE;
}

static BOOL WINAPI GetHandleInformation(HANDLE h, DWORD *flags)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *flags = o->inherit ? 1 : 0;
    return TRUE;
}

static const struct w32_export vista_exports[] = {
    W32_FN(PathCchSkipRoot),
    W32_FN(PathCchCombineEx),
    W32_FN(PathCchCombine),
    W32_FN(PathCchCanonicalizeEx),
    W32_FN(PathCchCanonicalize),
    W32_FN(PathCchRemoveFileSpec),
    W32_FN(PathIsUNCEx),
    W32_FN(GetFinalPathNameByHandleW),
    W32_FN(GetFinalPathNameByHandleA),
    W32_FN(GetFileInformationByHandleEx),
    W32_FN(GetActiveProcessorCount),
    W32_FN(GetProcessTimes),
    W32_FN(GetThreadTimes),
    W32_FN(AddVectoredExceptionHandler),
    W32_AS("AddVectoredContinueHandler", AddVectoredExceptionHandler),
    W32_AS("RemoveVectoredExceptionHandler", w32_one1),
    W32_AS("RemoveVectoredContinueHandler", w32_one1),
    W32_FN(AddDllDirectory),
    W32_AS("RemoveDllDirectory", w32_one1),
    W32_AS("DisableThreadLibraryCalls", w32_one1),
    W32_FN(GetHandleInformation),
    W32_FN(SetHandleInformation),
    W32_FN(VerSetConditionMask),
    W32_AS("VerifyVersionInfoA", w32_one4),
    W32_AS("VerifyVersionInfoW", w32_one4),
    W32_FN(GetComputerNameExW),
    W32_FN(GetLogicalDriveStringsW),
    W32_FN(GetVolumePathNameW),
    W32_FN(FindFirstVolumeW),
    W32_AS("FindNextVolumeW", not_implemented3),
    W32_AS("FindVolumeClose", w32_one1),
    W32_AS("GetVolumePathNamesForVolumeNameW", not_implemented4),
    W32_AS("LockFile", w32_one5),
    W32_AS("UnlockFile", w32_one5),
    W32_AS("LockFileEx", w32_one6),
    W32_AS("UnlockFileEx", w32_one5),
    W32_AS("CreateHardLinkW", not_implemented3),
    W32_AS("CreateHardLinkA", not_implemented3),
    W32_AS("CreateSymbolicLinkW", not_implemented3),
    W32_AS("CreateSymbolicLinkA", not_implemented3),
    W32_FN(CopyFile2),
    W32_FN(GetSystemTimeAdjustment),
    W32_AS("HeapCompact", w32_zero2),
    W32_AS("VirtualLock", w32_one2),
    W32_AS("VirtualUnlock", w32_one2),
    W32_AS("NeedCurrentDirectoryForExePathW", w32_one1),
    W32_AS("NeedCurrentDirectoryForExePathA", w32_one1),
    W32_AS("GenerateConsoleCtrlEvent", w32_one2),
    W32_FN(OpenProcess),
    W32_FN(CreateWaitableTimerExW),
    W32_FN(CreateWaitableTimerW),
    W32_AS("CreateWaitableTimerA", CreateWaitableTimerW),
    W32_FN(SetWaitableTimerEx),
    W32_FN(SetWaitableTimer),
    W32_FN(CancelWaitableTimer),
    W32_FN(CreateIoCompletionPort),
    W32_FN(GetQueuedCompletionStatus),
    W32_FN(GetQueuedCompletionStatusEx),
    W32_FN(PostQueuedCompletionStatus),
    W32_FN(GetOverlappedResult),
    W32_FN(CancelIoEx),
    W32_FN(CancelIo),
    W32_AS("DeviceIoControl", false8),
    W32_AS("OpenFileMappingW", no_pointer3),
    W32_AS("RegisterWaitForSingleObject", false6),
    W32_AS("UnregisterWait", w32_one1),
    W32_AS("UnregisterWaitEx", w32_one2),
    W32_FN(InitializeProcThreadAttributeList),
    W32_FN(UpdateProcThreadAttribute),
    W32_AS("DeleteProcThreadAttributeList", w32_zero1),
    W32_AS("PssCaptureSnapshot", pss_failed4),
    W32_AS("PssQuerySnapshot", pss_failed4),
    W32_AS("PssFreeSnapshot", pss_failed2),
};

W32_EXPORTS(vista_module, "kernel32.dll", vista_exports);
