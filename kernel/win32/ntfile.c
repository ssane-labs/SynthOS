#include <synth/error.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "win32.h"

/* The NT file calls that runtimes built on libuv (Node.js, Electron) and newer CRTs use instead of kernel32:
   information about an open file and its volume, changing it, and reading a directory through its handle. */

#define STATUS_SUCCESS 0u
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_INVALID_INFO_CLASS 0xc0000003u
#define STATUS_INVALID_HANDLE 0xc0000008u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_ACCESS_DENIED 0xc0000022u
#define STATUS_BUFFER_TOO_SMALL 0xc0000023u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034u
#define STATUS_NOT_SUPPORTED 0xc00000bbu
#define STATUS_INFO_LENGTH_MISMATCH 0xc0000004u
#define STATUS_NOT_A_DIRECTORY 0xc0000103u

struct io_status {
    uint32_t status;
    uint32_t information;
};

struct unicode_string {
    uint16_t length, max_length;
    WCHAR *buffer;
};

static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static uint64_t filetime(uint32_t epoch)
{
    return ((uint64_t)epoch + 11644473600ull) * 10000000ull;
}

static DWORD done(struct io_status *iosb, DWORD status, uint32_t info)
{
    if (iosb) {
        iosb->status = status;
        iosb->information = info;
    }
    return status;
}

static struct w32_object *file_of(HANDLE h, struct vstat *st)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_FILE)
        return NULL;
    memset(st, 0, sizeof(*st));
    if (o->file)
        vfs_fstat(o->file, st);
    else if (o->path)
        vfs_stat(o->path, st);
    return o;
}

static uint32_t attributes(const struct vstat *st)
{
    return st->type == VN_DIR ? 0x10 : 0x20;
}

static void basic(uint8_t *p, const struct vstat *st)
{
    for (int i = 0; i < 4; i++)
        put64(p + i * 8, filetime(st->mtime));
    put32(p + 32, attributes(st));
    put32(p + 36, 0);
}

static void standard(uint8_t *p, const struct vstat *st, bool pending)
{
    put64(p, ((uint64_t)st->size + 4095) & ~4095ull);
    put64(p + 8, st->size);
    put32(p + 16, 1);
    p[20] = pending;
    p[21] = st->type == VN_DIR;
    p[22] = p[23] = 0;
}

/* The name without the drive, as NT gives it: \Users\me\file.txt */
static uint32_t name_info(struct w32_object *o, uint8_t *p, uint32_t room)
{
    char win[PATH_MAX_LEN];
    win[0] = 0;
    if (o->path)
        winpath_from_vfs(o->path, win, sizeof(win));
    const char *name = win[0] && win[1] == ':' ? win + 2 : win;
    uint32_t n = (uint32_t)strlen(name);
    put32(p, n * 2);
    uint32_t fit = MIN(n, room / 2);
    for (uint32_t i = 0; i < fit; i++)
        ((WCHAR *)(p + 4))[i] = name[i] == '/' ? '\\' : (WCHAR)(uint8_t)name[i];
    return n * 2;
}

static DWORD WINAPI NtQueryInformationFile(HANDLE h, struct io_status *iosb, uint8_t *out, ULONG size, DWORD cls)
{
    struct vstat st;
    struct w32_object *o = file_of(h, &st);
    if (!o)
        return done(iosb, STATUS_INVALID_HANDLE, 0);
    static const uint8_t need[] = {[4] = 40, [5] = 24, [6] = 8, [7] = 4, [8] = 4, [9] = 4, [14] = 8, [16] = 4,
                                   [17] = 4, [18] = 100, [34] = 56, [35] = 8};
    if (cls >= sizeof(need) || !need[cls])
        return done(iosb, STATUS_INVALID_INFO_CLASS, 0);
    if (size < need[cls])
        return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
    memset(out, 0, need[cls]);
    uint32_t pos = o->file ? o->file->pos : 0;
    switch (cls) {
    case 4: basic(out, &st); break;
    case 5: standard(out, &st, o->delete_on_close); break;
    case 6: put64(out, st.inode); break;
    case 8: put32(out, o->access ? o->access : 0x1f01ff); break;
    case 9: {
        uint32_t bytes = name_info(o, out, size - 4);
        return done(iosb, bytes + 4 > size ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, MIN(bytes + 4, size));
    }
    case 14: put64(out, pos); break;
    case 18: {
        basic(out, &st);
        standard(out + 40, &st, o->delete_on_close);
        put64(out + 64, st.inode);
        put32(out + 76, o->access ? o->access : 0x1f01ff);
        put64(out + 80, pos);
        uint32_t bytes = name_info(o, out + 96, size - 100);
        return done(iosb, bytes + 100 > size ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, MIN(bytes + 100, size));
    }
    case 34:
        for (int i = 0; i < 4; i++)
            put64(out + i * 8, filetime(st.mtime));
        put64(out + 32, ((uint64_t)st.size + 4095) & ~4095ull);
        put64(out + 40, st.size);
        put32(out + 48, attributes(&st));
        break;
    case 35: put32(out, attributes(&st)); break;
    default: break;
    }
    return done(iosb, STATUS_SUCCESS, need[cls]);
}

typedef BOOL(WINAPI *set_info_fn)(HANDLE, DWORD, void *, DWORD);

static DWORD WINAPI NtSetInformationFile(HANDLE h, struct io_status *iosb, uint8_t *in, ULONG size, DWORD cls)
{
    struct vstat st;
    struct w32_object *o = file_of(h, &st);
    if (!o)
        return done(iosb, STATUS_INVALID_HANDLE, 0);
    set_info_fn set = (set_info_fn)w32_builtin_lookup("kernel32.dll", "SetFileInformationByHandle");
    switch (cls) {
    case 4:
    case 16:
        return done(iosb, STATUS_SUCCESS, 0);
    case 10:
    case 65:
        if (size < 12 || !set || !set(h, 3, in, size))
            return done(iosb, STATUS_ACCESS_DENIED, 0);
        return done(iosb, STATUS_SUCCESS, 0);
    case 13:
        if (size < 1)
            return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        o->delete_on_close = in[0] != 0;
        return done(iosb, STATUS_SUCCESS, 0);
    case 64: {
        uint32_t flags;
        if (size < 4)
            return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        memcpy(&flags, in, 4);
        o->delete_on_close = flags & 1;
        return done(iosb, STATUS_SUCCESS, 0);
    }
    case 14: {
        uint64_t pos;
        if (size < 8 || !o->file)
            return done(iosb, STATUS_INVALID_PARAMETER, 0);
        memcpy(&pos, in, 8);
        o->file->pos = (uint32_t)pos;
        return done(iosb, STATUS_SUCCESS, 0);
    }
    case 19:
    case 20: {
        uint64_t end;
        if (size < 8 || !o->path)
            return done(iosb, STATUS_INVALID_PARAMETER, 0);
        memcpy(&end, in, 8);
        if (cls == 19 && end <= st.size)
            return done(iosb, STATUS_SUCCESS, 0);
        w32_fcache_forget(o->path);
        int err = vfs_truncate(o->path, (uint32_t)end);
        return done(iosb, err ? STATUS_ACCESS_DENIED : STATUS_SUCCESS, 0);
    }
    default:
        return done(iosb, STATUS_INVALID_INFO_CLASS, 0);
    }
}

static struct mount *mount_of(const char *path)
{
    struct mount *best = NULL;
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        size_t n = strlen(m->path);
        if (!strncmp(path, m->path, n) && (path[n] == '/' || !path[n] || (n == 1 && m->path[0] == '/')) &&
            (!best || n > strlen(best->path)))
            best = m;
    }
    return best;
}

static DWORD WINAPI NtQueryVolumeInformationFile(HANDLE h, struct io_status *iosb, uint8_t *out, ULONG size,
                                                 DWORD cls)
{
    struct vstat st;
    struct w32_object *o = file_of(h, &st);
    if (!o)
        return done(iosb, STATUS_INVALID_HANDLE, 0);
    uint64_t total = 64ull << 30, free = 32ull << 30;
    struct mount *m = o->path ? mount_of(o->path) : NULL;
    if (m && m->fs->statfs)
        m->fs->statfs(m->root, &total, &free);
    switch (cls) {
    case 1: /* FileFsVolumeInformation */
        if (size < 18)
            return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        memset(out, 0, MIN(size, 24u));
        put64(out, filetime(0));
        put32(out + 8, 0x53594e54);
        return done(iosb, STATUS_SUCCESS, MIN(size, 24u));
    case 3: /* FileFsSizeInformation */
    case 7: /* FileFsFullSizeInformation */
        if (size < (cls == 3 ? 24u : 32u))
            return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        put64(out, total / 4096);
        put64(out + 8, free / 4096);
        if (cls == 7)
            put64(out + 16, free / 4096);
        put32(out + (cls == 3 ? 16 : 24), 8);
        put32(out + (cls == 3 ? 20 : 28), 512);
        return done(iosb, STATUS_SUCCESS, cls == 3 ? 24 : 32);
    case 4: /* FileFsDeviceInformation */
        if (size < 8)
            return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        put32(out, 7);
        put32(out + 4, 0);
        return done(iosb, STATUS_SUCCESS, 8);
    case 5: { /* FileFsAttributeInformation */
        static const char name[] = "NTFS";
        if (size < 12)
            return done(iosb, STATUS_INFO_LENGTH_MISMATCH, 0);
        put32(out, 0x3 | 0x4 | 0x8);
        put32(out + 4, 255);
        put32(out + 8, 8);
        uint32_t fit = MIN(4u, (size - 12) / 2);
        for (uint32_t i = 0; i < fit; i++)
            ((WCHAR *)(out + 12))[i] = (WCHAR)name[i];
        return done(iosb, fit < 4 ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS, 12 + fit * 2);
    }
    default:
        return done(iosb, STATUS_INVALID_INFO_CLASS, 0);
    }
}

/* Directory entries: the classes differ in what follows the common 64-byte head and where the name begins. */
static int head_size(DWORD cls)
{
    switch (cls) {
    case 1: return 64;  /* FileDirectoryInformation */
    case 2: return 68;  /* FileFullDirectoryInformation */
    case 3: return 94;  /* FileBothDirectoryInformation */
    case 12: return 12; /* FileNamesInformation */
    case 37: return 104; /* FileIdBothDirectoryInformation */
    case 38: return 80; /* FileIdFullDirectoryInformation */
    default: return 0;
    }
}

static bool wild_match(const char *pat, const char *s)
{
    if (!*pat)
        return !*s;
    if (*pat == '*') {
        for (; *s; s++)
            if (wild_match(pat + 1, s))
                return true;
        return wild_match(pat + 1, s);
    }
    if (!*s)
        return false;
    if (*pat != '?' && str_ncasecmp(pat, s, 1))
        return false;
    return wild_match(pat + 1, s + 1);
}

static DWORD WINAPI NtQueryDirectoryFile(HANDLE h, HANDLE ev, void *apc, void *ctx, struct io_status *iosb,
                                         uint8_t *out, ULONG size, DWORD cls, BOOL single,
                                         struct unicode_string *mask, BOOL restart)
{
    UNUSED(ev);
    UNUSED(apc);
    UNUSED(ctx);
    struct vstat st;
    struct w32_object *o = file_of(h, &st);
    if (!o || !o->path)
        return done(iosb, STATUS_INVALID_HANDLE, 0);
    if (st.type != VN_DIR)
        return done(iosb, STATUS_NOT_A_DIRECTORY, 0);
    int head = head_size(cls);
    if (!head)
        return done(iosb, STATUS_INVALID_INFO_CLASS, 0);
    char pattern[NAME_MAX_LEN] = "*";
    if (mask && mask->buffer && mask->length) {
        uint32_t n = MIN((uint32_t)mask->length / 2, (uint32_t)sizeof(pattern) - 1);
        for (uint32_t i = 0; i < n; i++)
            pattern[i] = (char)mask->buffer[i];
        pattern[n] = 0;
    }
    if (restart)
        o->value = 0;
    struct vdir d;
    if (vfs_opendir(o->path, &d) != 0)
        return done(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
    /* entry 0 and 1 are "." and "..", then the directory's own entries */
    uint32_t index = 0, used = 0, prev = 0, count = 0;
    bool full = false;
    struct vfs_dirent ent;
    bool have = true;
    while (!full && (index < 2 || (have = vfs_readdir(&d, &ent) == 0))) {
        const char *name;
        struct vstat es = {0};
        if (index < 2) {
            name = index ? ".." : ".";
            es.type = VN_DIR;
            es.mtime = st.mtime;
        } else {
            name = ent.name;
            char full_path[PATH_MAX_LEN];
            path_join(o->path, ent.name, full_path, sizeof(full_path));
            if (vfs_stat(full_path, &es) != 0) {
                es.type = ent.type;
                es.size = ent.size;
            }
        }
        if (index++ < o->value || !wild_match(pattern, name))
            continue;
        uint32_t n = (uint32_t)strlen(name);
        uint32_t len = ((uint32_t)head + n * 2 + 7) & ~7u;
        if (used + (uint32_t)head + n * 2 > size) {
            if (!count) {
                vfs_closedir(&d);
                return done(iosb, STATUS_BUFFER_OVERFLOW, 0);
            }
            index--;
            break;
        }
        uint8_t *e = out + used;
        memset(e, 0, (size_t)head);
        if (count)
            put32(out + prev, used - prev);
        put32(e + 4, index - 1);
        if (cls == 12) {
            put32(e + 8, n * 2);
        } else {
            for (int i = 0; i < 4; i++)
                put64(e + 8 + i * 8, filetime(es.mtime));
            put64(e + 40, es.size);
            put64(e + 48, ((uint64_t)es.size + 4095) & ~4095ull);
            put32(e + 56, attributes(&es));
            put32(e + 60, n * 2);
            if (cls == 37)
                put64(e + 96, es.inode);
            else if (cls == 38)
                put64(e + 72, es.inode);
        }
        for (uint32_t i = 0; i < n; i++)
            ((WCHAR *)(e + head))[i] = (WCHAR)(uint8_t)name[i];
        prev = used;
        used += MIN(len, size - used);
        count++;
        o->value = index;
        if (single)
            full = true;
    }
    vfs_closedir(&d);
    if (!count)
        return done(iosb, STATUS_NO_MORE_FILES, 0);
    return done(iosb, STATUS_SUCCESS, used);
}

static DWORD WINAPI NtFlushBuffersFile(HANDLE h, struct io_status *iosb)
{
    struct vstat st;
    return done(iosb, file_of(h, &st) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE, 0);
}

typedef HANDLE(WINAPI *create_thread_fn)(void *, SIZE_T, void *, void *, DWORD, DWORD *);
typedef BOOL(WINAPI *duplicate_fn)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);

/* The thread pool of old: every work item gets a thread of its own. */
static BOOL WINAPI QueueUserWorkItem(void *fn, void *ctx, ULONG flags)
{
    UNUSED(flags);
    create_thread_fn create = (create_thread_fn)w32_builtin_lookup("kernel32.dll", "CreateThread");
    HANDLE t = create ? create(NULL, 0, fn, ctx, 0, NULL) : NULL;
    if (!t)
        return FALSE;
    w32_handle_close(t);
    return TRUE;
}

static HANDLE WINAPI ReOpenFile(HANDLE h, DWORD access, DWORD share, DWORD flags)
{
    UNUSED(share);
    UNUSED(flags);
    duplicate_fn dup = (duplicate_fn)w32_builtin_lookup("kernel32.dll", "DuplicateHandle");
    HANDLE out = NULL;
    if (!dup || !dup(W32_PSEUDO_PROCESS, h, W32_PSEUDO_PROCESS, &out, access, FALSE, 0))
        return INVALID_HANDLE_VALUE;
    struct w32_object *o = w32_handle_get(out);
    if (o && access)
        o->access = access;
    return out;
}

static BOOL WINAPI WriteConsoleInputW(HANDLE h, const void *records, DWORD count, DWORD *written)
{
    UNUSED(h);
    UNUSED(records);
    if (written)
        *written = count;
    return TRUE;
}

static DWORD WINAPI UnDecorateSymbolName(const char *name, char *out, DWORD cap, DWORD flags)
{
    UNUSED(flags);
    if (!name || !out || !cap)
        return 0;
    str_copy(out, name, cap);
    return (DWORD)strlen(out);
}

static DWORD WINAPI ConvertInterfaceIndexToLuid(DWORD index, uint64_t *luid)
{
    if (!luid)
        return ERROR_INVALID_PARAMETER;
    *luid = (uint64_t)6 << 48 | (uint64_t)index << 24;
    return 0;
}

static DWORD WINAPI GetBestRoute2(void *luid, DWORD index, const void *src, const void *dst, ULONG options,
                                  void *route, void *best)
{
    UNUSED(luid);
    UNUSED(index);
    UNUSED(src);
    UNUSED(dst);
    UNUSED(options);
    UNUSED(route);
    UNUSED(best);
    return 1168;
}

static const struct w32_export ntfile_kernel32[] = {
    W32_FN(QueueUserWorkItem),
    W32_FN(ReOpenFile),
    W32_FN(WriteConsoleInputW),
    W32_AS("WriteConsoleInputA", WriteConsoleInputW),
};

static const struct w32_export ntfile_dbghelp[] = {
    W32_FN(UnDecorateSymbolName),
};

static const struct w32_export ntfile_iphlpapi[] = {
    W32_FN(ConvertInterfaceIndexToLuid),
    W32_FN(GetBestRoute2),
};

static const struct w32_export ntfile_advapi32[] = {
    W32_AS("CryptEnumProvidersW", w32_fail6),
    W32_AS("CryptEnumProvidersA", w32_fail6),
    W32_AS("CryptExportKey", w32_fail6),
    W32_AS("CryptGetUserKey", w32_fail3),
};

W32_EXPORTS(ntfile_kernel32_module, "kernel32.dll", ntfile_kernel32);
W32_EXPORTS(ntfile_dbghelp_module, "dbghelp.dll", ntfile_dbghelp);
W32_EXPORTS(ntfile_iphlpapi_module, "iphlpapi.dll", ntfile_iphlpapi);
W32_EXPORTS(ntfile_advapi32_module, "advapi32.dll", ntfile_advapi32);

static const struct w32_export ntfile_ntdll[] = {
    W32_FN(NtQueryInformationFile),
    W32_AS("ZwQueryInformationFile", NtQueryInformationFile),
    W32_FN(NtSetInformationFile),
    W32_AS("ZwSetInformationFile", NtSetInformationFile),
    W32_FN(NtQueryVolumeInformationFile),
    W32_AS("ZwQueryVolumeInformationFile", NtQueryVolumeInformationFile),
    W32_FN(NtQueryDirectoryFile),
    W32_AS("ZwQueryDirectoryFile", NtQueryDirectoryFile),
    W32_FN(NtFlushBuffersFile),
};

W32_EXPORTS(ntfile_ntdll_module, "ntdll.dll", ntfile_ntdll);
