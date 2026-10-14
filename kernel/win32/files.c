#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/winpath.h>

#include "win32.h"

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL 0x10000000u
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_NORMAL 0x80
#define INVALID_ATTRIBUTES 0xffffffffu
#define INVALID_SET_FILE_POINTER 0xffffffffu

static char *narrow(LPCWSTR w)
{
    return w32_narrow(w);
}

static DWORD to_wide_buffer(const char *s, LPWSTR buf, DWORD size)
{
    size_t need = w32_cp_to_wide(s, -1, NULL, 0, 1);
    if (!buf || need > size)
        return (DWORD)need;
    w32_cp_to_wide(s, -1, buf, size, 1);
    return (DWORD)(need - 1);
}

static DWORD to_ansi_buffer(const char *s, LPSTR buf, DWORD size)
{
    size_t n = strlen(s);
    if (!buf || n + 1 > size)
        return (DWORD)n + 1;
    memcpy(buf, s, n + 1);
    return (DWORD)n;
}

static uint64_t filetime_of(uint32_t epoch)
{
    return ((uint64_t)epoch + 11644473600ull) * 10000000ull;
}

static void put_filetime(uint8_t *p, uint32_t epoch)
{
    uint64_t t = filetime_of(epoch);
    memcpy(p, &t, 8);
}

static bool is_device(const char *name, const char *device)
{
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    if (str_starts(name, "\\\\.\\"))
        base = name + 4;
    return str_casecmp(base, device) == 0;
}

static HANDLE std_dup(int which)
{
    struct w32_object *o = w32_handle_get(w32_std_handle(which));
    if (o)
        o->refs++;
    return w32_std_handle(which);
}

HANDLE w32_create_file(const char *name, DWORD access, DWORD disposition)
{
    if (is_device(name, "CONOUT$") || is_device(name, "CON"))
        return std_dup(access & GENERIC_READ && !(access & GENERIC_WRITE) ? 0 : 1);
    if (is_device(name, "CONIN$"))
        return std_dup(0);
    if (is_device(name, "NUL"))
        return w32_handle_new(WH_NULL);
    if (w32_is_pipe_name(name))
        return w32_npipe_open(name);
    char path[PATH_MAX_LEN];
    if (w32_path(name, path, sizeof(path)) != 0) {
        w32_set_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    bool exists = vfs_exists(path);
    bool dir = exists && vfs_is_dir(path);
    unsigned flags = 0;
    if (access & (GENERIC_READ | GENERIC_ALL | 0x1 | 0x02000000u))
        flags |= VFS_READ;
    if (access & (GENERIC_WRITE | GENERIC_ALL | 0x2 | 0x4))
        flags |= VFS_WRITE;
    if (!flags)
        flags = VFS_READ;
    bool append = (access & 0x4) && !(access & (GENERIC_WRITE | 0x2));
    switch (disposition) {
    case CREATE_NEW:
        if (exists) {
            w32_set_error(ERROR_FILE_EXISTS);
            return INVALID_HANDLE_VALUE;
        }
        flags |= VFS_CREATE | VFS_WRITE;
        break;
    case CREATE_ALWAYS:
        flags |= VFS_CREATE | VFS_TRUNC | VFS_WRITE;
        break;
    case OPEN_EXISTING:
        break;
    case OPEN_ALWAYS:
        flags |= VFS_CREATE;
        if (!exists)
            flags |= VFS_WRITE;
        break;
    case TRUNCATE_EXISTING:
        flags |= VFS_TRUNC | VFS_WRITE;
        break;
    default:
        w32_set_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    if (!exists && (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING)) {
        char parent[PATH_MAX_LEN];
        path_dirname(path, parent, sizeof(parent));
        w32_set_error(vfs_is_dir(parent) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (dir)
        flags = VFS_READ;
    struct file *f;
    int err = vfs_open(path, flags, &f);
    if (err) {
        w32_set_error(w32_error_from_vfs(err));
        return INVALID_HANDLE_VALUE;
    }
    if (flags & VFS_TRUNC)
        w32_fcache_truncate(path, 0);
    HANDLE h = w32_handle_new(WH_FILE);
    struct w32_object *o = w32_handle_get(h);
    if (!o) {
        vfs_close(f);
        return INVALID_HANDLE_VALUE;
    }
    o->file = f;
    o->append = append;
    o->path = kstrdup(path);
    w32_set_error(exists && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return h;
}

static HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags,
                                 HANDLE tmpl)
{
    UNUSED(share);
    UNUSED(flags);
    UNUSED(tmpl);
    return w32_inheritable(w32_create_file(name, access, disposition), sa);
}

static HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags,
                                 HANDLE tmpl)
{
    UNUSED(share);
    UNUSED(flags);
    UNUSED(tmpl);
    char *n = narrow(name);
    HANDLE h = w32_inheritable(w32_create_file(n, access, disposition), sa);
    w32_free(n);
    return h;
}

long w32_handle_read(HANDLE h, void *buf, size_t n)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return E_BADF;
    switch (o->type) {
    case WH_STDIN:
        return w32_console_read(buf, n);
    case WH_FILE:
        return vfs_read(o->file, buf, n);
    case WH_PIPE:
        return o->pipe && o->value != 1 ? w32_pipe_read(o->pipe, buf, n, true) : E_BADF;
    case WH_NULL:
        return 0;
    default:
        return E_BADF;
    }
}

long w32_handle_write(HANDLE h, const void *buf, size_t n)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return E_BADF;
    switch (o->type) {
    case WH_STDOUT:
    case WH_STDERR:
        w32_console_write(o->type, buf, n);
        return (long)n;
    case WH_FILE: {
        if (o->append)
            vfs_seek(o->file, 0, VFS_SEEK_END);
        uint32_t at = o->file->pos;
        long put = vfs_write(o->file, buf, n);
        if (put > 0 && o->path)
            w32_fcache_write(o->path, at, buf, (uint32_t)put);
        return put;
    }
    case WH_PIPE:
        if (o->value == 2)
            return o->pipe_out ? w32_pipe_write(o->pipe_out, buf, n) : E_PIPE;
        return o->pipe && o->value ? w32_pipe_write(o->pipe, buf, n) : E_BADF;
    case WH_NULL:
        return (long)n;
    default:
        return E_BADF;
    }
}

static BOOL WINAPI ReadFile(HANDLE h, void *buf, DWORD n, DWORD *read, void *overlapped)
{
    uint32_t *ov = overlapped;
    struct w32_object *o = w32_handle_get(h);
    if (read)
        *read = 0;
    if (ov && o && o->type == WH_PIPE) {
        int e = w32_pipe_overlapped(h, o, buf, n, read, ov, false);
        if (e)
            w32_set_error((DWORD)e);
        return e == 0;
    }
    if (ov && o && o->type == WH_FILE)
        vfs_seek(o->file, (long)ov[2], VFS_SEEK_SET);
    long got = w32_handle_read(h, buf, n);
    if (got < 0) {
        w32_set_error(got == E_BADF ? ERROR_INVALID_HANDLE : w32_error_from_vfs((int)got));
        return FALSE;
    }
    if (read)
        *read = (DWORD)got;
    if (ov) {
        if (!got && n && o && o->type == WH_FILE) {
            w32_io_complete(o, ov, 0xc0000011u, 0, true);
            w32_set_error(38);
            return FALSE;
        }
        w32_io_complete(o, ov, 0, (uint32_t)got, true);
    }
    return TRUE;
}

static BOOL WINAPI WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped)
{
    uint32_t *ov = overlapped;
    struct w32_object *o = w32_handle_get(h);
    if (written)
        *written = 0;
    if (ov && o && o->type == WH_PIPE) {
        int e = w32_pipe_overlapped(h, o, (void *)buf, n, written, ov, true);
        if (e)
            w32_set_error((DWORD)e);
        return e == 0;
    }
    if (ov && o && o->type == WH_FILE && !o->append)
        vfs_seek(o->file, (long)ov[2], VFS_SEEK_SET);
    long put = w32_handle_write(h, buf, n);
    if (put < 0) {
        w32_set_error(put == E_BADF ? ERROR_INVALID_HANDLE : w32_error_from_vfs((int)put));
        return FALSE;
    }
    if (written)
        *written = (DWORD)put;
    if (ov)
        w32_io_complete(o, ov, 0, (uint32_t)put, true);
    return TRUE;
}

static struct w32_object *file_object(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_FILE) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return o;
}

static DWORD WINAPI SetFilePointer(HANDLE h, LONG distance, LONG *high, DWORD method)
{
    struct w32_object *o = w32_handle_get(h);
    if (o && o->type != WH_FILE)
        return 0;
    if (!(o = file_object(h)))
        return INVALID_SET_FILE_POINTER;
    long off = distance;
    if (high && *high && *high != -1) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }
    long pos = vfs_seek(o->file, off, (int)method);
    if (pos < 0) {
        w32_set_error(131);
        return INVALID_SET_FILE_POINTER;
    }
    if (high)
        *high = 0;
    w32_set_error(0);
    return (DWORD)pos;
}

static BOOL WINAPI SetFilePointerEx(HANDLE h, LONGLONG distance, LONGLONG *out, DWORD method)
{
    struct w32_object *o = file_object(h);
    if (!o)
        return FALSE;
    long pos = vfs_seek(o->file, (long)distance, (int)method);
    if (pos < 0) {
        w32_set_error(131);
        return FALSE;
    }
    if (out)
        *out = pos;
    return TRUE;
}

static DWORD WINAPI GetFileSize(HANDLE h, DWORD *high)
{
    struct w32_object *o = file_object(h);
    struct vstat st;
    if (high)
        *high = 0;
    if (!o || vfs_fstat(o->file, &st) != 0)
        return 0xffffffffu;
    return st.size;
}

static BOOL WINAPI GetFileSizeEx(HANDLE h, LONGLONG *size)
{
    struct w32_object *o = file_object(h);
    struct vstat st;
    if (!o || vfs_fstat(o->file, &st) != 0)
        return FALSE;
    *size = st.size;
    return TRUE;
}

static DWORD WINAPI GetFileType(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return 0;
    switch (o->type) {
    case WH_STDIN:
        return stream_is_console(w32->ctx->in) ? 2 : 3;
    case WH_STDOUT:
        return stream_is_console(w32->ctx->out) ? 2 : 3;
    case WH_STDERR:
        return stream_is_console(w32->ctx->err) ? 2 : 3;
    case WH_NULL:
        return 2;
    case WH_PIPE:
        return 3;
    default:
        return 1;
    }
}

static BOOL WINAPI SetEndOfFile(HANDLE h)
{
    struct w32_object *o = file_object(h);
    if (!o || !o->file->vn->ops->truncate || o->file->vn->ops->truncate(o->file->vn, o->file->pos) != 0)
        return FALSE;
    if (o->path)
        w32_fcache_truncate(o->path, o->file->pos);
    return TRUE;
}

static BOOL WINAPI FlushFileBuffers(HANDLE h)
{
    UNUSED(h);
    return TRUE;
}

static BOOL fs_result(int err)
{
    if (err) {
        w32_set_error(w32_error_from_vfs(err));
        return FALSE;
    }
    return TRUE;
}

static BOOL delete_file(const char *name)
{
    char path[PATH_MAX_LEN];
    int err = w32_path(name, path, sizeof(path));
    if (!err && vfs_is_dir(path))
        err = E_PERM;
    if (!err)
        w32_fcache_forget(path);
    return fs_result(err ? err : vfs_unlink(path));
}

static BOOL WINAPI DeleteFileA(LPCSTR name)
{
    return delete_file(name);
}

static BOOL WINAPI DeleteFileW(LPCWSTR name)
{
    char *n = narrow(name);
    BOOL ok = delete_file(n);
    w32_free(n);
    return ok;
}

static BOOL move_file(const char *from, const char *to, DWORD flags)
{
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    int err = w32_path(from, a, sizeof(a));
    if (!err)
        err = w32_path(to, b, sizeof(b));
    if (err)
        return fs_result(err);
    if (vfs_exists(b)) {
        if (!(flags & 1)) {
            w32_set_error(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
        vfs_unlink(b);
    }
    w32_fcache_forget(a);
    w32_fcache_forget(b);
    err = vfs_rename(a, b);
    if (err == E_XDEV && (err = vfs_copy(a, b)) == 0)
        err = vfs_unlink(a);
    return fs_result(err);
}

static BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to)
{
    return move_file(from, to, 0);
}

static BOOL WINAPI MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    return move_file(from, to, flags);
}

static BOOL WINAPI MoveFileW(LPCWSTR from, LPCWSTR to)
{
    char *a = narrow(from), *b = narrow(to);
    BOOL ok = move_file(a, b, 0);
    w32_free(a);
    w32_free(b);
    return ok;
}

static BOOL WINAPI MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    char *a = narrow(from), *b = narrow(to);
    BOOL ok = move_file(a, b, flags);
    w32_free(a);
    w32_free(b);
    return ok;
}

static BOOL copy_file(const char *from, const char *to, BOOL fail_if_exists)
{
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    int err = w32_path(from, a, sizeof(a));
    if (!err)
        err = w32_path(to, b, sizeof(b));
    if (!err && fail_if_exists && vfs_exists(b)) {
        w32_set_error(ERROR_FILE_EXISTS);
        return FALSE;
    }
    return fs_result(err ? err : vfs_copy(a, b));
}

static BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL fail)
{
    return copy_file(from, to, fail);
}

static BOOL WINAPI CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail)
{
    char *a = narrow(from), *b = narrow(to);
    BOOL ok = copy_file(a, b, fail);
    w32_free(a);
    w32_free(b);
    return ok;
}

static BOOL make_dir(const char *name)
{
    char path[PATH_MAX_LEN];
    int err = w32_path(name, path, sizeof(path));
    return fs_result(err ? err : vfs_mkdir(path));
}

static BOOL WINAPI CreateDirectoryA(LPCSTR name, void *sa)
{
    UNUSED(sa);
    return make_dir(name);
}

static BOOL WINAPI CreateDirectoryW(LPCWSTR name, void *sa)
{
    UNUSED(sa);
    char *n = narrow(name);
    BOOL ok = make_dir(n);
    w32_free(n);
    return ok;
}

static BOOL remove_dir(const char *name)
{
    char path[PATH_MAX_LEN];
    int err = w32_path(name, path, sizeof(path));
    if (!err && !vfs_is_dir(path))
        err = vfs_exists(path) ? E_NOTDIR : E_NOENT;
    return fs_result(err ? err : vfs_unlink(path));
}

static BOOL WINAPI RemoveDirectoryA(LPCSTR name)
{
    return remove_dir(name);
}

static BOOL WINAPI RemoveDirectoryW(LPCWSTR name)
{
    char *n = narrow(name);
    BOOL ok = remove_dir(n);
    w32_free(n);
    return ok;
}

static DWORD attributes(const char *name, struct vstat *st)
{
    char path[PATH_MAX_LEN];
    if (w32_path(name, path, sizeof(path)) != 0 || vfs_stat(path, st) != 0) {
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return INVALID_ATTRIBUTES;
    }
    return st->type == VN_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE;
}

static DWORD WINAPI GetFileAttributesA(LPCSTR name)
{
    struct vstat st;
    return attributes(name, &st);
}

static DWORD WINAPI GetFileAttributesW(LPCWSTR name)
{
    char *n = narrow(name);
    struct vstat st;
    DWORD a = attributes(n, &st);
    w32_free(n);
    return a;
}

static BOOL attributes_ex(const char *name, uint8_t *data)
{
    struct vstat st;
    DWORD a = attributes(name, &st);
    if (a == INVALID_ATTRIBUTES)
        return FALSE;
    memset(data, 0, 36);
    memcpy(data, &a, 4);
    put_filetime(data + 4, st.mtime);
    put_filetime(data + 12, st.mtime);
    put_filetime(data + 20, st.mtime);
    memcpy(data + 32, &st.size, 4);
    return TRUE;
}

static BOOL WINAPI GetFileAttributesExA(LPCSTR name, int level, uint8_t *data)
{
    UNUSED(level);
    return attributes_ex(name, data);
}

static BOOL WINAPI GetFileAttributesExW(LPCWSTR name, int level, uint8_t *data)
{
    UNUSED(level);
    char *n = narrow(name);
    BOOL ok = attributes_ex(n, data);
    w32_free(n);
    return ok;
}

static BOOL WINAPI SetFileAttributesA(LPCSTR name, DWORD attrs)
{
    UNUSED(attrs);
    struct vstat st;
    return attributes(name, &st) != INVALID_ATTRIBUTES;
}

static BOOL WINAPI SetFileAttributesW(LPCWSTR name, DWORD attrs)
{
    UNUSED(attrs);
    char *n = narrow(name);
    struct vstat st;
    BOOL ok = attributes(n, &st) != INVALID_ATTRIBUTES;
    w32_free(n);
    return ok;
}

static BOOL WINAPI GetFileInformationByHandle(HANDLE h, uint8_t *info)
{
    struct w32_object *o = w32_handle_get(h);
    memset(info, 0, 52);
    if (!o)
        return FALSE;
    if (o->type != WH_FILE) {
        memcpy(info, &(DWORD){ATTR_NORMAL}, 4);
        return TRUE;
    }
    struct vstat st;
    vfs_fstat(o->file, &st);
    DWORD a = st.type == VN_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    memcpy(info, &a, 4);
    put_filetime(info + 4, st.mtime);
    put_filetime(info + 12, st.mtime);
    put_filetime(info + 20, st.mtime);
    memcpy(info + 28, &(DWORD){0x53594e54}, 4);
    memcpy(info + 36, &st.size, 4);
    memcpy(info + 40, &(DWORD){1}, 4);
    memcpy(info + 48, &st.inode, 4);
    return TRUE;
}

static BOOL WINAPI GetFileTime(HANDLE h, uint8_t *created, uint8_t *accessed, uint8_t *written)
{
    struct w32_object *o = file_object(h);
    struct vstat st;
    if (!o || vfs_fstat(o->file, &st) != 0)
        return FALSE;
    if (created)
        put_filetime(created, st.mtime);
    if (accessed)
        put_filetime(accessed, st.mtime);
    if (written)
        put_filetime(written, st.mtime);
    return TRUE;
}

static BOOL WINAPI SetFileTime(HANDLE h, const void *c, const void *a, const void *w)
{
    UNUSED(c);
    UNUSED(a);
    UNUSED(w);
    return file_object(h) != NULL;
}

static bool find_step(struct w32_find *f, struct w32_find_entry *out)
{
    bool root = strcmp(f->dir, "/") == 0;
    for (;;) {
        uint32_t i = f->index++;
        if (!root && i < 2) {
            str_copy(out->name, i == 0 ? "." : "..", sizeof(out->name));
            out->type = VN_DIR;
            out->size = 0;
            out->mtime = 0;
        } else {
            struct vdir d;
            if (vfs_opendir(f->dir, &d) != 0)
                return false;
            d.index = root ? i : i - 2;
            struct vfs_dirent ent;
            int err = vfs_readdir(&d, &ent);
            vfs_closedir(&d);
            if (err)
                return false;
            str_copy(out->name, ent.name, sizeof(out->name));
            out->type = ent.type;
            out->size = ent.size;
            char full[PATH_MAX_LEN];
            struct vstat st = {0};
            if (path_join(f->dir, ent.name, full, sizeof(full)) == 0)
                vfs_stat(full, &st);
            out->mtime = st.mtime;
        }
        if (strcmp(f->pattern, "*") == 0 || strcmp(f->pattern, "*.*") == 0 ||
            wildcard_match(f->pattern, out->name, true))
            return true;
    }
}

static void fill_find_a(uint8_t *data, const struct w32_find_entry *e)
{
    memset(data, 0, 320);
    DWORD a = e->type == VN_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    memcpy(data, &a, 4);
    put_filetime(data + 4, e->mtime);
    put_filetime(data + 12, e->mtime);
    put_filetime(data + 20, e->mtime);
    memcpy(data + 32, &e->size, 4);
    str_copy((char *)data + 44, e->name, 260);
}

static void fill_find_w(uint8_t *data, const struct w32_find_entry *e)
{
    memset(data, 0, 592);
    DWORD a = e->type == VN_DIR ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    memcpy(data, &a, 4);
    put_filetime(data + 4, e->mtime);
    put_filetime(data + 12, e->mtime);
    put_filetime(data + 20, e->mtime);
    memcpy(data + 32, &e->size, 4);
    w32_cp_to_wide(e->name, -1, (WCHAR *)(data + 44), 259, 1);
}

HANDLE w32_find_first(const char *spec, struct w32_find_entry *e)
{
    char copy[PATH_MAX_LEN];
    str_copy(copy, spec, sizeof(copy));
    char *sep = NULL;
    for (char *p = copy; *p; p++)
        if (*p == '\\' || *p == '/')
            sep = p;
    const char *pattern = copy;
    char dir_part[PATH_MAX_LEN] = ".";
    if (sep) {
        *sep = 0;
        str_copy(dir_part, copy[0] ? copy : "\\", sizeof(dir_part));
        if (strlen(dir_part) == 2 && dir_part[1] == ':')
            str_append(dir_part, "\\", sizeof(dir_part));
        pattern = sep + 1;
    } else if (copy[0] && copy[1] == ':') {
        dir_part[0] = copy[0];
        dir_part[1] = ':';
        dir_part[2] = 0;
        pattern = copy + 2;
    }
    struct w32_find *f = kzalloc(sizeof(*f));
    if (!f)
        return INVALID_HANDLE_VALUE;
    if (w32_path(dir_part, f->dir, sizeof(f->dir)) != 0 || !vfs_is_dir(f->dir)) {
        kfree(f);
        w32_set_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    str_copy(f->pattern, *pattern ? pattern : "*", sizeof(f->pattern));
    if (!find_step(f, e)) {
        kfree(f);
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = w32_handle_new(WH_FIND);
    struct w32_object *o = w32_handle_get(h);
    if (!o) {
        kfree(f);
        return INVALID_HANDLE_VALUE;
    }
    o->find = f;
    return h;
}

bool w32_find_next(HANDLE h, struct w32_find_entry *e)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_FIND) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return false;
    }
    if (!find_step(o->find, e)) {
        w32_set_error(ERROR_NO_MORE_FILES);
        return false;
    }
    return true;
}

static HANDLE find_first(const char *spec, uint8_t *data, bool wide)
{
    struct w32_find_entry e;
    HANDLE h = w32_find_first(spec, &e);
    if (h == INVALID_HANDLE_VALUE)
        return h;
    if (wide)
        fill_find_w(data, &e);
    else
        fill_find_a(data, &e);
    return h;
}

static BOOL find_next(HANDLE h, uint8_t *data, bool wide)
{
    struct w32_find_entry e;
    if (!w32_find_next(h, &e))
        return FALSE;
    if (wide)
        fill_find_w(data, &e);
    else
        fill_find_a(data, &e);
    return TRUE;
}

static HANDLE WINAPI FindFirstFileA(LPCSTR spec, uint8_t *data)
{
    return find_first(spec, data, false);
}

static HANDLE WINAPI FindFirstFileW(LPCWSTR spec, uint8_t *data)
{
    char *n = narrow(spec);
    HANDLE h = find_first(n, data, true);
    w32_free(n);
    return h;
}

static HANDLE WINAPI FindFirstFileExA(LPCSTR spec, int level, uint8_t *data, int op, void *filter, DWORD flags)
{
    UNUSED(level);
    UNUSED(op);
    UNUSED(filter);
    UNUSED(flags);
    return find_first(spec, data, false);
}

static HANDLE WINAPI FindFirstFileExW(LPCWSTR spec, int level, uint8_t *data, int op, void *filter, DWORD flags)
{
    UNUSED(level);
    UNUSED(op);
    UNUSED(filter);
    UNUSED(flags);
    return FindFirstFileW(spec, data);
}

static BOOL WINAPI FindNextFileA(HANDLE h, uint8_t *data)
{
    return find_next(h, data, false);
}

static BOOL WINAPI FindNextFileW(HANDLE h, uint8_t *data)
{
    return find_next(h, data, true);
}

static BOOL WINAPI FindClose(HANDLE h)
{
    return w32_handle_close(h);
}

static DWORD WINAPI GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    char win[PATH_MAX_LEN];
    w32_path_to_win(w32->cwd, win, sizeof(win));
    return to_ansi_buffer(win, buf, size);
}

static DWORD WINAPI GetCurrentDirectoryW(DWORD size, LPWSTR buf)
{
    char win[PATH_MAX_LEN];
    w32_path_to_win(w32->cwd, win, sizeof(win));
    return to_wide_buffer(win, buf, size);
}

static BOOL set_cwd(const char *name)
{
    char path[PATH_MAX_LEN];
    if (w32_path(name, path, sizeof(path)) != 0 || !vfs_is_dir(path)) {
        w32_set_error(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    str_copy(w32->cwd, path, sizeof(w32->cwd));
    return TRUE;
}

static BOOL WINAPI SetCurrentDirectoryA(LPCSTR name)
{
    return set_cwd(name);
}

static BOOL WINAPI SetCurrentDirectoryW(LPCWSTR name)
{
    char *n = narrow(name);
    BOOL ok = set_cwd(n);
    w32_free(n);
    return ok;
}

static void full_path(const char *name, char *out, size_t cap)
{
    char path[PATH_MAX_LEN];
    if (w32_path(name, path, sizeof(path)) != 0)
        str_copy(path, w32->cwd, sizeof(path));
    w32_path_to_win(path, out, cap);
}

static DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buf, LPSTR *file_part)
{
    char win[PATH_MAX_LEN];
    full_path(name, win, sizeof(win));
    DWORD n = to_ansi_buffer(win, buf, size);
    if (file_part && buf && n < size) {
        char *slash = strrchr(buf, '\\');
        *file_part = slash && slash[1] ? slash + 1 : NULL;
    }
    return n;
}

static DWORD WINAPI GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buf, LPWSTR *file_part)
{
    char *n = narrow(name);
    char win[PATH_MAX_LEN];
    full_path(n, win, sizeof(win));
    w32_free(n);
    DWORD len = to_wide_buffer(win, buf, size);
    if (file_part && buf && len < size) {
        *file_part = NULL;
        for (DWORD i = 0; i < len; i++)
            if (buf[i] == '\\' && i + 1 < len)
                *file_part = buf + i + 1;
    }
    return len;
}

static DWORD WINAPI GetTempPathA(DWORD size, LPSTR buf)
{
    return to_ansi_buffer("C:\\Windows\\Temp\\", buf, size);
}

static DWORD WINAPI GetTempPathW(DWORD size, LPWSTR buf)
{
    return to_wide_buffer("C:\\Windows\\Temp\\", buf, size);
}

static UINT temp_name(const char *dir, const char *prefix, UINT unique, char *out)
{
    for (UINT n = unique ? unique : (hal_entropy() & 0xffff) | 1;; n++) {
        ksnprintf(out, PATH_MAX_LEN, "%s%s%.3s%04X.tmp", dir, str_ends(dir, "\\") ? "" : "\\", prefix,
                  n & 0xffff);
        if (unique)
            return unique;
        char path[PATH_MAX_LEN];
        if (w32_path(out, path, sizeof(path)) == 0 && !vfs_exists(path)) {
            vfs_write_all(path, "", 0);
            return n & 0xffff;
        }
    }
}

static UINT WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out)
{
    return temp_name(dir, prefix, unique, out);
}

static UINT WINAPI GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR out)
{
    char *d = narrow(dir), *p = narrow(prefix), tmp[PATH_MAX_LEN];
    UINT r = temp_name(d, p, unique, tmp);
    w32_free(d);
    w32_free(p);
    w32_cp_to_wide(tmp, -1, out, 260, 1);
    return r;
}

static UINT WINAPI GetWindowsDirectoryA(LPSTR buf, UINT size)
{
    return to_ansi_buffer("C:\\Windows", buf, size);
}

static UINT WINAPI GetWindowsDirectoryW(LPWSTR buf, UINT size)
{
    return to_wide_buffer("C:\\Windows", buf, size);
}

static UINT WINAPI GetSystemDirectoryA(LPSTR buf, UINT size)
{
    return to_ansi_buffer("C:\\Windows\\System32", buf, size);
}

static UINT WINAPI GetSystemDirectoryW(LPWSTR buf, UINT size)
{
    return to_wide_buffer("C:\\Windows\\System32", buf, size);
}

static UINT WINAPI GetDriveTypeA(LPCSTR root)
{
    return !root || to_upper((uint8_t)root[0]) == 'C' || root[0] == '\\' ? 3 : 1;
}

static UINT WINAPI GetDriveTypeW(LPCWSTR root)
{
    return !root || root[0] == 'C' || root[0] == 'c' || root[0] == '\\' ? 3 : 1;
}

static DWORD WINAPI GetLogicalDrives(void)
{
    return 1u << 2;
}

static DWORD WINAPI GetLogicalDriveStringsA(DWORD size, LPSTR buf)
{
    if (size < 5)
        return 5;
    memcpy(buf, "C:\\\0", 5);
    return 4;
}

static void disk_space(uint64_t *total, uint64_t *free)
{
    *total = 64ull << 20;
    *free = 32ull << 20;
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        if ((strcmp(m->path, "/home") == 0 || strcmp(m->path, "/") == 0) && m->fs->statfs &&
            m->fs->statfs(m->root, total, free) == 0)
            break;
}

static BOOL WINAPI GetDiskFreeSpaceA(LPCSTR root, DWORD *spc, DWORD *bps, DWORD *free_clusters, DWORD *clusters)
{
    UNUSED(root);
    uint64_t total, free;
    disk_space(&total, &free);
    if (spc)
        *spc = 8;
    if (bps)
        *bps = 512;
    if (free_clusters)
        *free_clusters = (DWORD)(free / 4096);
    if (clusters)
        *clusters = (DWORD)(total / 4096);
    return TRUE;
}

static BOOL WINAPI GetDiskFreeSpaceW(LPCWSTR root, DWORD *spc, DWORD *bps, DWORD *free_clusters, DWORD *clusters)
{
    UNUSED(root);
    return GetDiskFreeSpaceA(NULL, spc, bps, free_clusters, clusters);
}

static BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR dir, ULONGLONG *avail, ULONGLONG *total, ULONGLONG *free)
{
    UNUSED(dir);
    uint64_t t, f;
    disk_space(&t, &f);
    if (avail)
        *avail = f;
    if (total)
        *total = t;
    if (free)
        *free = f;
    return TRUE;
}

static BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR dir, ULONGLONG *avail, ULONGLONG *total, ULONGLONG *free)
{
    UNUSED(dir);
    return GetDiskFreeSpaceExA(NULL, avail, total, free);
}

static BOOL WINAPI GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD name_size, DWORD *serial, DWORD *max_len,
                                         DWORD *flags, LPSTR fs, DWORD fs_size)
{
    UNUSED(root);
    if (name && name_size)
        str_copy(name, "SYNTHOS", name_size);
    if (serial)
        *serial = 0x53594e54;
    if (max_len)
        *max_len = 57;
    if (flags)
        *flags = 0x3;
    if (fs && fs_size)
        str_copy(fs, "SynthFS", fs_size);
    return TRUE;
}

static BOOL WINAPI GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD name_size, DWORD *serial, DWORD *max_len,
                                         DWORD *flags, LPWSTR fs, DWORD fs_size)
{
    UNUSED(root);
    if (name && name_size)
        w32_cp_to_wide("SYNTHOS", -1, name, name_size, 1);
    if (fs && fs_size)
        w32_cp_to_wide("SynthFS", -1, fs, fs_size, 1);
    return GetVolumeInformationA(NULL, NULL, 0, serial, max_len, flags, NULL, 0);
}

static DWORD WINAPI GetShortPathNameA(LPCSTR in, LPSTR out, DWORD size)
{
    return to_ansi_buffer(in, out, size);
}

static DWORD WINAPI GetShortPathNameW(LPCWSTR in, LPWSTR out, DWORD size)
{
    size_t n = w32_wcslen(in);
    if (!out || n + 1 > size)
        return (DWORD)n + 1;
    memmove(out, in, (n + 1) * sizeof(WCHAR));
    return (DWORD)n;
}

static BOOL WINAPI AreFileApisANSI(void)
{
    return TRUE;
}

static void WINAPI SetFileApisNop(void)
{
}

static char **env_find(const char *name)
{
    size_t n = strlen(name);
    for (char **e = w32->environ; *e; e++)
        if (str_ncasecmp(*e, name, n) == 0 && (*e)[n] == '=')
            return e;
    return NULL;
}

static const char *env_get(const char *name)
{
    char **e = env_find(name);
    return e ? *e + strlen(name) + 1 : NULL;
}

static void env_rebuild(void)
{
    size_t len = 1, count = 0;
    for (char **e = w32->environ; *e; e++, count++)
        len += strlen(*e) + 1;
    char *block = w32_alloc(len);
    size_t pos = 0;
    for (char **e = w32->environ; *e; e++) {
        size_t n = strlen(*e) + 1;
        memcpy(block + pos, *e, n);
        pos += n;
    }
    block[pos] = 0;
    w32->env_block = block;
    size_t wn = w32_cp_to_wide(block, (int)pos + 1, NULL, 0, 1);
    w32->env_block_w = w32_alloc((wn + 1) * sizeof(WCHAR));
    w32_cp_to_wide(block, (int)pos + 1, w32->env_block_w, wn, 1);
    w32->env_block_w[wn] = 0;
    UNUSED(count);
}

static BOOL env_set(const char *name, const char *value)
{
    char **slot = env_find(name);
    if (!value) {
        if (slot)
            for (; *slot; slot++)
                slot[0] = slot[1];
        env_rebuild();
        return TRUE;
    }
    size_t n = strlen(name) + strlen(value) + 2;
    char *entry = w32_alloc(n);
    ksnprintf(entry, n, "%s=%s", name, value);
    if (slot) {
        *slot = entry;
    } else {
        size_t count = 0;
        while (w32->environ[count])
            count++;
        char **grown = w32_alloc(sizeof(char *) * (count + 2));
        memcpy(grown, w32->environ, sizeof(char *) * count);
        grown[count] = entry;
        grown[count + 1] = NULL;
        w32->environ = grown;
    }
    env_rebuild();
    return TRUE;
}

const char *w32_getenv(const char *name)
{
    return env_get(name);
}

int w32_putenv(const char *name, const char *value)
{
    return env_set(name, value) ? 0 : -1;
}

static DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char *v = env_get(name);
    if (!v) {
        w32_set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    return to_ansi_buffer(v, buf, size);
}

static DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size)
{
    char *n = narrow(name);
    const char *v = env_get(n);
    w32_free(n);
    if (!v) {
        w32_set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    return to_wide_buffer(v, buf, size);
}

static BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    return env_set(name, value);
}

static BOOL WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
    char *n = narrow(name), *v = value ? narrow(value) : NULL;
    BOOL ok = env_set(n, v);
    w32_free(n);
    w32_free(v);
    return ok;
}

static LPSTR WINAPI GetEnvironmentStringsA(void)
{
    return w32->env_block;
}

static LPWSTR WINAPI GetEnvironmentStringsW(void)
{
    return w32->env_block_w;
}

static BOOL WINAPI FreeEnvironmentStrings(void *block)
{
    UNUSED(block);
    return TRUE;
}

static void expand(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    for (const char *p = in; *p && len + 1 < cap;) {
        const char *end = *p == '%' ? strchr(p + 1, '%') : NULL;
        if (end && end > p + 1) {
            char name[64];
            size_t n = MIN((size_t)(end - p - 1), sizeof(name) - 1);
            memcpy(name, p + 1, n);
            name[n] = 0;
            const char *v = env_get(name);
            if (v) {
                len = str_append(out, v, cap);
                p = end + 1;
                continue;
            }
        }
        out[len++] = *p++;
        out[len] = 0;
    }
}

static DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR in, LPSTR out, DWORD size)
{
    char tmp[1024];
    expand(in, tmp, sizeof(tmp));
    DWORD n = to_ansi_buffer(tmp, out, size);
    return out && n < size ? n + 1 : n;
}

static DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR in, LPWSTR out, DWORD size)
{
    char *n = narrow(in), tmp[1024];
    expand(n, tmp, sizeof(tmp));
    w32_free(n);
    DWORD len = to_wide_buffer(tmp, out, size);
    return out && len < size ? len + 1 : len;
}

static const struct w32_export kernel32_files[] = {
    W32_FN(CreateFileA), W32_FN(CreateFileW), W32_FN(ReadFile), W32_FN(WriteFile), W32_FN(SetFilePointer),
    W32_FN(SetFilePointerEx), W32_FN(GetFileSize), W32_FN(GetFileSizeEx), W32_FN(GetFileType),
    W32_FN(SetEndOfFile), W32_FN(FlushFileBuffers), W32_FN(DeleteFileA), W32_FN(DeleteFileW), W32_FN(MoveFileA),
    W32_FN(MoveFileW), W32_FN(MoveFileExA), W32_FN(MoveFileExW), W32_FN(CopyFileA), W32_FN(CopyFileW),
    W32_FN(CreateDirectoryA), W32_FN(CreateDirectoryW), W32_FN(RemoveDirectoryA), W32_FN(RemoveDirectoryW),
    W32_FN(GetFileAttributesA), W32_FN(GetFileAttributesW), W32_FN(GetFileAttributesExA),
    W32_FN(GetFileAttributesExW), W32_FN(SetFileAttributesA), W32_FN(SetFileAttributesW),
    W32_FN(GetFileInformationByHandle), W32_FN(GetFileTime), W32_FN(SetFileTime), W32_FN(FindFirstFileA),
    W32_FN(FindFirstFileW), W32_FN(FindFirstFileExA), W32_FN(FindFirstFileExW), W32_FN(FindNextFileA),
    W32_FN(FindNextFileW), W32_FN(FindClose), W32_FN(GetCurrentDirectoryA), W32_FN(GetCurrentDirectoryW),
    W32_FN(SetCurrentDirectoryA), W32_FN(SetCurrentDirectoryW), W32_FN(GetFullPathNameA),
    W32_FN(GetFullPathNameW), W32_FN(GetTempPathA), W32_FN(GetTempPathW), W32_FN(GetTempFileNameA),
    W32_FN(GetTempFileNameW), W32_FN(GetWindowsDirectoryA), W32_FN(GetWindowsDirectoryW),
    W32_AS("GetSystemWindowsDirectoryA", GetWindowsDirectoryA),
    W32_AS("GetSystemWindowsDirectoryW", GetWindowsDirectoryW), W32_FN(GetSystemDirectoryA),
    W32_FN(GetSystemDirectoryW), W32_FN(GetDriveTypeA), W32_FN(GetDriveTypeW), W32_FN(GetLogicalDrives),
    W32_FN(GetLogicalDriveStringsA), W32_FN(GetDiskFreeSpaceA), W32_FN(GetDiskFreeSpaceW),
    W32_FN(GetDiskFreeSpaceExA), W32_FN(GetDiskFreeSpaceExW), W32_FN(GetVolumeInformationA),
    W32_FN(GetVolumeInformationW), W32_FN(GetShortPathNameA), W32_FN(GetShortPathNameW),
    W32_AS("GetLongPathNameA", GetShortPathNameA), W32_AS("GetLongPathNameW", GetShortPathNameW),
    W32_FN(AreFileApisANSI), W32_AS("SetFileApisToOEM", SetFileApisNop), W32_AS("SetFileApisToANSI", SetFileApisNop),
    W32_FN(GetEnvironmentVariableA), W32_FN(GetEnvironmentVariableW), W32_FN(SetEnvironmentVariableA),
    W32_FN(SetEnvironmentVariableW), W32_FN(GetEnvironmentStringsA), W32_FN(GetEnvironmentStringsW),
    W32_AS("GetEnvironmentStrings", GetEnvironmentStringsA), W32_AS("FreeEnvironmentStringsA", FreeEnvironmentStrings),
    W32_AS("FreeEnvironmentStringsW", FreeEnvironmentStrings), W32_FN(ExpandEnvironmentStringsA),
    W32_FN(ExpandEnvironmentStringsW),
};

W32_EXPORTS(kernel32_files_module, "kernel32.dll", kernel32_files);
