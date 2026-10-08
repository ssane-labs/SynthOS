#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/string.h>

#include "crt.h"

#define CRT_MAX_FD 64
#define CRT_MAX_FILES 128
#define BUF_SIZE 4096
#define CRT_EOF (-1)
#define IOFBF 0x0
#define IOLBF 0x40
#define IONBF 0x4

struct crt_fd {
    HANDLE h;
    uint32_t flags;
    bool used;
    bool eof;
    int peek;
};

struct cstream {
    bool used;
    int fd;
    bool can_read;
    bool can_write;
    char *buf;
    size_t rpos;
    size_t rlen;
    size_t wlen;
    int bufmode;
    bool user_mode;
    int ungot[4];
    int nungot;
    bool eof;
    bool err;
    int locked;
    char *tmpname;
};

struct user_file {
    CRT_FILE pub;
    uint8_t lock[24];
    struct cstream st;
};

W32_STATE CRT_FILE crt_iob[3];
static W32_STATE struct cstream std_streams[3];
static W32_STATE struct user_file *files[CRT_MAX_FILES];
static W32_STATE struct crt_fd fds[CRT_MAX_FD];

static struct cstream *stream_of(CRT_FILE *f)
{
    if (f >= crt_iob && f < crt_iob + 3)
        return &std_streams[f - crt_iob];
    return &((struct user_file *)f)->st;
}

void crt_stdio_reset(void)
{
    memset(fds, 0, sizeof(fds));
    memset(files, 0, sizeof(files));
    memset(std_streams, 0, sizeof(std_streams));
    memset(crt_iob, 0, sizeof(crt_iob));
    for (int i = 0; i < 3; i++) {
        fds[i] = (struct crt_fd){w32_std_handle(i), CRT_O_TEXT, true, false, -1};
        crt_iob[i].file = i;
        crt_iob[i].flag = i == 0 ? 0x1 : 0x2;
        std_streams[i] = (struct cstream){.used = true, .fd = i, .can_read = i == 0, .can_write = i != 0,
                                         .bufmode = i == 2 ? IONBF : IOFBF};
    }
}

static struct crt_fd *fd_get(int fd)
{
    if (fd < 0 || fd >= CRT_MAX_FD || !fds[fd].used) {
        crt_set_errno(CRT_EBADF);
        return NULL;
    }
    return &fds[fd];
}

static int fd_alloc(HANDLE h, uint32_t flags, int from)
{
    for (int i = from; i < CRT_MAX_FD; i++)
        if (!fds[i].used) {
            fds[i] = (struct crt_fd){h, flags, true, false, -1};
            return i;
        }
    crt_set_errno(CRT_EMFILE);
    return -1;
}

int crt_isatty(int fd)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return 0;
    struct w32_object *o = w32_handle_get(e->h);
    if (!o)
        return 0;
    switch (o->type) {
    case WH_STDIN:
        return stream_is_console(w32->ctx->in);
    case WH_STDOUT:
        return stream_is_console(w32->ctx->out);
    case WH_STDERR:
        return stream_is_console(w32->ctx->err);
    case WH_NULL:
        return 1;
    default:
        return 0;
    }
}

static bool text_mode(const struct crt_fd *e)
{
    return !(e->flags & CRT_O_BINARY);
}

static long raw_write(struct crt_fd *e, const void *buf, size_t n)
{
    long r = w32_handle_write(e->h, buf, n);
    if (r < 0)
        crt_set_errno(r == E_BADF ? CRT_EBADF : crt_errno_from_vfs((int)r));
    return r;
}

long crt_fd_write(int fd, const void *buf, size_t n)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    if (!text_mode(e) || !memchr(buf, '\n', n))
        return raw_write(e, buf, n);
    char out[512];
    size_t len = 0;
    const char *s = buf;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            out[len++] = '\r';
        out[len++] = s[i];
        if (len >= sizeof(out) - 2) {
            if (raw_write(e, out, len) < 0)
                return -1;
            len = 0;
        }
    }
    if (len && raw_write(e, out, len) < 0)
        return -1;
    return (long)n;
}

static long raw_read(struct crt_fd *e, char *buf, size_t n)
{
    size_t got = 0;
    if (e->peek >= 0 && n) {
        buf[got++] = (char)e->peek;
        e->peek = -1;
    }
    if (got == n)
        return (long)got;
    long r = w32_handle_read(e->h, buf + got, n - got);
    if (r < 0) {
        crt_set_errno(r == E_BADF ? CRT_EBADF : crt_errno_from_vfs((int)r));
        return got ? (long)got : -1;
    }
    return (long)got + r;
}

long crt_fd_read(int fd, void *buf, size_t n)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    if (e->eof && text_mode(e))
        return 0;
    char *p = buf;
    for (;;) {
        long r = raw_read(e, p, n);
        if (r <= 0 || !text_mode(e))
            return r;
        size_t out = 0;
        for (long i = 0; i < r; i++) {
            if (p[i] == 0x1a && !crt_isatty(fd)) {
                e->eof = true;
                break;
            }
            if (p[i] == '\r') {
                if (i + 1 < r) {
                    if (p[i + 1] == '\n')
                        continue;
                } else {
                    char next;
                    long k = raw_read(e, &next, 1);
                    if (k == 1) {
                        if (next == '\n') {
                            p[out++] = '\n';
                            continue;
                        }
                        e->peek = (uint8_t)next;
                    }
                }
            }
            p[out++] = p[i];
        }
        if (out || e->eof)
            return (long)out;
    }
}

static long fd_seek(int fd, int64_t off, int whence)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    struct w32_object *o = w32_handle_get(e->h);
    if (!o || o->type != WH_FILE) {
        crt_set_errno(CRT_ESPIPE);
        return -1;
    }
    if (e->peek >= 0 && whence == VFS_SEEK_CUR)
        off--;
    e->peek = -1;
    e->eof = false;
    long r = vfs_seek(o->file, (long)off, whence);
    if (r < 0)
        crt_set_errno(CRT_EINVAL);
    return r;
}

static int open_path(const char *path, int flags)
{
    DWORD access = (flags & 3) == CRT_O_WRONLY ? 0x40000000u : (flags & 3) == CRT_O_RDWR ? 0xc0000000u : 0x80000000u;
    DWORD disp = 3;
    if (flags & CRT_O_CREAT)
        disp = flags & CRT_O_EXCL ? 1 : flags & CRT_O_TRUNC ? 2 : 4;
    else if (flags & CRT_O_TRUNC)
        disp = 5;
    HANDLE h = w32_create_file(path, access, disp);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = w32_error();
        crt_set_errno(err == ERROR_FILE_EXISTS ? CRT_EEXIST
                      : err == ERROR_ACCESS_DENIED ? CRT_EACCES
                                                   : CRT_ENOENT);
        return -1;
    }
    struct w32_object *o = w32_handle_get(h);
    if (o && (flags & CRT_O_APPEND))
        o->append = true;
    uint32_t mode = flags & (CRT_O_TEXT | CRT_O_BINARY) ? (uint32_t)flags : (uint32_t)flags | (uint32_t)crt_fmode;
    if (!(mode & CRT_O_BINARY))
        mode |= CRT_O_TEXT;
    int fd = fd_alloc(h, mode, 3);
    if (fd < 0)
        w32_handle_close(h);
    return fd;
}

static int fd_close(int fd)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    if (fd > 2 || w32_handle_get(e->h) == NULL || fd != (int)(((uintptr_t)e->h - 0x100) / 4))
        w32_handle_close(e->h);
    e->used = false;
    return 0;
}

static int CDECL_open(const char *path, int flags, ...)
{
    return open_path(path, flags);
}

static int crt_wopen(const WCHAR *path, int flags, ...)
{
    char *p = w32_narrow(path);
    int fd = open_path(p, flags);
    w32_free(p);
    return fd;
}

static int crt_sopen(const char *path, int flags, int share, ...)
{
    UNUSED(share);
    return open_path(path, flags);
}

static int crt_sopen_s(int *fd, const char *path, int flags, int share, int mode)
{
    UNUSED(share);
    UNUSED(mode);
    *fd = open_path(path, flags);
    return *fd < 0 ? crt_errno_value : 0;
}

static int crt_creat(const char *path, int mode)
{
    UNUSED(mode);
    return open_path(path, CRT_O_CREAT | CRT_O_TRUNC | CRT_O_RDWR);
}

static int crt_close(int fd)
{
    return fd_close(fd);
}

static int crt_read(int fd, void *buf, unsigned n)
{
    return (int)crt_fd_read(fd, buf, n);
}

static int crt_write(int fd, const void *buf, unsigned n)
{
    return (int)crt_fd_write(fd, buf, n);
}

static long crt_lseek(int fd, long off, int whence)
{
    return fd_seek(fd, off, whence);
}

static int64_t crt_lseeki64(int fd, int64_t off, int whence)
{
    return fd_seek(fd, off, whence);
}

static long crt_tell(int fd)
{
    return fd_seek(fd, 0, VFS_SEEK_CUR);
}

static int64_t crt_telli64(int fd)
{
    return fd_seek(fd, 0, VFS_SEEK_CUR);
}

static long fd_length(int fd)
{
    struct crt_fd *e = fd_get(fd);
    struct w32_object *o = e ? w32_handle_get(e->h) : NULL;
    struct vstat st;
    if (!o || o->type != WH_FILE || vfs_fstat(o->file, &st) != 0)
        return -1;
    return (long)st.size;
}

static long crt_filelength(int fd)
{
    return fd_length(fd);
}

static int64_t crt_filelengthi64(int fd)
{
    return fd_length(fd);
}

static int crt_eof(int fd)
{
    long pos = fd_seek(fd, 0, VFS_SEEK_CUR), len = fd_length(fd);
    if (pos < 0 || len < 0)
        return -1;
    return pos >= len;
}

static int crt_chsize(int fd, long size)
{
    struct crt_fd *e = fd_get(fd);
    struct w32_object *o = e ? w32_handle_get(e->h) : NULL;
    if (!o || o->type != WH_FILE || !o->file->vn->ops->truncate)
        return -1;
    return o->file->vn->ops->truncate(o->file->vn, (uint32_t)size) == 0 ? 0 : -1;
}

static int crt_chsize_s(int fd, int64_t size)
{
    return crt_chsize(fd, (long)size) == 0 ? 0 : CRT_EINVAL;
}

static int crt_commit(int fd)
{
    return fd_get(fd) ? 0 : -1;
}

static int crt_dup(int fd)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    struct w32_object *o = w32_handle_get(e->h);
    if (o)
        o->refs++;
    int n = fd_alloc(e->h, e->flags, 0);
    if (n < 0 && o)
        o->refs--;
    return n;
}

static int crt_dup2(int from, int to)
{
    struct crt_fd *e = fd_get(from);
    if (!e || to < 0 || to >= CRT_MAX_FD)
        return -1;
    if (from == to)
        return 0;
    if (fds[to].used)
        fd_close(to);
    struct w32_object *o = w32_handle_get(e->h);
    if (o)
        o->refs++;
    fds[to] = (struct crt_fd){e->h, e->flags, true, false, -1};
    return 0;
}

static int crt_isatty_export(int fd)
{
    return crt_isatty(fd) ? 0x40 : 0;
}

static int crt_setmode(int fd, int mode)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    int old = e->flags & CRT_O_BINARY ? CRT_O_BINARY : CRT_O_TEXT;
    e->flags = (e->flags & ~(uint32_t)(CRT_O_TEXT | CRT_O_BINARY | CRT_O_WTEXT | CRT_O_U16TEXT | CRT_O_U8TEXT)) |
               (uint32_t)mode;
    return old;
}

static intptr_t crt_get_osfhandle(int fd)
{
    struct crt_fd *e = fd_get(fd);
    return e ? (intptr_t)e->h : -1;
}

static int crt_open_osfhandle(intptr_t h, int flags)
{
    if (!w32_handle_get((HANDLE)h)) {
        crt_set_errno(CRT_EBADF);
        return -1;
    }
    return fd_alloc((HANDLE)h, (uint32_t)flags | (flags & CRT_O_BINARY ? 0 : CRT_O_TEXT), 3);
}

static int crt_locking(int fd, int mode, long n)
{
    UNUSED(mode);
    UNUSED(n);
    return fd_get(fd) ? 0 : -1;
}

static struct user_file *new_file(void)
{
    for (int i = 0; i < CRT_MAX_FILES; i++)
        if (!files[i]) {
            files[i] = w32_zalloc(sizeof(struct user_file));
            return files[i];
        }
    crt_set_errno(CRT_EMFILE);
    return NULL;
}

static void drop_file(CRT_FILE *f)
{
    for (int i = 0; i < CRT_MAX_FILES; i++)
        if (files[i] == (struct user_file *)f) {
            files[i] = NULL;
            w32_free(f);
        }
}

static bool parse_mode(const char *mode, int *flags, bool *rd, bool *wr)
{
    int f;
    switch (mode[0]) {
    case 'r':
        f = CRT_O_RDONLY;
        break;
    case 'w':
        f = CRT_O_WRONLY | CRT_O_CREAT | CRT_O_TRUNC;
        break;
    case 'a':
        f = CRT_O_WRONLY | CRT_O_CREAT | CRT_O_APPEND;
        break;
    default:
        crt_set_errno(CRT_EINVAL);
        return false;
    }
    for (const char *p = mode + 1; *p && *p != ','; p++) {
        if (*p == '+')
            f = (f & ~3) | CRT_O_RDWR;
        else if (*p == 'b')
            f |= CRT_O_BINARY;
        else if (*p == 't')
            f |= CRT_O_TEXT;
        else if (*p == 'x')
            f |= CRT_O_EXCL;
    }
    *flags = f;
    *rd = (f & 3) != CRT_O_WRONLY;
    *wr = (f & 3) != CRT_O_RDONLY;
    return true;
}

static void stream_init(struct cstream *st, int fd, bool rd, bool wr)
{
    memset(st, 0, sizeof(*st));
    st->used = true;
    st->fd = fd;
    st->can_read = rd;
    st->can_write = wr;
    st->bufmode = IOFBF;
}

static CRT_FILE *wrap_fd(int fd, bool rd, bool wr)
{
    struct user_file *u = new_file();
    if (!u)
        return NULL;
    u->pub.file = fd;
    u->pub.flag = (rd ? 0x1 : 0) | (wr ? 0x2 : 0) | (rd && wr ? 0x80 : 0);
    stream_init(&u->st, fd, rd, wr);
    return &u->pub;
}

static CRT_FILE *crt_fopen(const char *path, const char *mode)
{
    int flags;
    bool rd, wr;
    if (!path || !mode || !parse_mode(mode, &flags, &rd, &wr))
        return NULL;
    int fd = open_path(path, flags);
    if (fd < 0)
        return NULL;
    CRT_FILE *f = wrap_fd(fd, rd, wr);
    if (!f)
        fd_close(fd);
    return f;
}

static CRT_FILE *crt_wfopen(const WCHAR *path, const WCHAR *mode)
{
    char *p = w32_narrow(path), *m = w32_narrow(mode);
    CRT_FILE *f = crt_fopen(p, m);
    w32_free(p);
    w32_free(m);
    return f;
}

static int crt_fopen_s(CRT_FILE **out, const char *path, const char *mode)
{
    *out = crt_fopen(path, mode);
    return *out ? 0 : crt_errno_value;
}

static int crt_wfopen_s(CRT_FILE **out, const WCHAR *path, const WCHAR *mode)
{
    *out = crt_wfopen(path, mode);
    return *out ? 0 : crt_errno_value;
}

static CRT_FILE *crt_fsopen(const char *path, const char *mode, int share)
{
    UNUSED(share);
    return crt_fopen(path, mode);
}

static CRT_FILE *crt_fdopen(int fd, const char *mode)
{
    int flags;
    bool rd, wr;
    if (!fd_get(fd) || !parse_mode(mode, &flags, &rd, &wr))
        return NULL;
    return wrap_fd(fd, rd, wr);
}

static bool tty_stream(struct cstream *st)
{
    return st->fd >= 0 && crt_isatty(st->fd);
}

static int flush_write(struct cstream *st)
{
    if (!st->wlen)
        return 0;
    long r = crt_fd_write(st->fd, st->buf, st->wlen);
    st->wlen = 0;
    if (r < 0) {
        st->err = true;
        return CRT_EOF;
    }
    return 0;
}

static void drop_read(struct cstream *st)
{
    if (st->rpos < st->rlen || st->nungot) {
        long back = (long)(st->rlen - st->rpos) + st->nungot;
        fd_seek(st->fd, -back, VFS_SEEK_CUR);
    }
    st->rpos = st->rlen = 0;
    st->nungot = 0;
}

static bool ensure_buf(struct cstream *st)
{
    if (!st->buf)
        st->buf = w32_alloc(BUF_SIZE);
    return st->buf != NULL;
}

int crt_fflush(CRT_FILE *f)
{
    if (!f) {
        crt_flush_all();
        return 0;
    }
    struct cstream *st = stream_of(f);
    if (!st->used)
        return CRT_EOF;
    if (st->wlen)
        return flush_write(st);
    if (st->rlen)
        drop_read(st);
    return 0;
}

void crt_flush_all(void)
{
    for (int i = 0; i < 3; i++)
        if (std_streams[i].wlen)
            flush_write(&std_streams[i]);
    for (int i = 0; i < CRT_MAX_FILES; i++)
        if (files[i] && files[i]->st.wlen)
            flush_write(&files[i]->st);
}

static int write_bytes(struct cstream *st, const char *s, size_t n)
{
    if (!st->can_write) {
        st->err = true;
        crt_set_errno(CRT_EBADF);
        return CRT_EOF;
    }
    if (st->rlen || st->nungot)
        drop_read(st);
    if (st->bufmode == IONBF || !ensure_buf(st)) {
        if (flush_write(st) < 0 || crt_fd_write(st->fd, s, n) < 0) {
            st->err = true;
            return CRT_EOF;
        }
        return 0;
    }
    while (n) {
        size_t k = MIN(n, (size_t)BUF_SIZE - st->wlen);
        memcpy(st->buf + st->wlen, s, k);
        st->wlen += k;
        s += k;
        n -= k;
        if (st->wlen == BUF_SIZE && flush_write(st) < 0)
            return CRT_EOF;
    }
    return 0;
}

static struct cstream *valid_stream(CRT_FILE *f)
{
    if (f >= crt_iob && f < crt_iob + 3)
        return &std_streams[f - crt_iob];
    for (int i = 0; i < CRT_MAX_FILES; i++)
        if (files[i] && &files[i]->pub == f)
            return &files[i]->st;
    return NULL;
}

void crt_lock_file(CRT_FILE *f)
{
    struct cstream *st = valid_stream(f);
    if (st)
        st->locked++;
}

void crt_unlock_file(CRT_FILE *f)
{
    struct cstream *st = valid_stream(f);
    if (st && st->locked && !--st->locked)
        crt_stream_done(f);
}

void crt_stream_done(CRT_FILE *f)
{
    struct cstream *st = stream_of(f);
    if (!st->wlen || st->locked)
        return;
    if (st->bufmode == IONBF || (!st->user_mode && tty_stream(st)) ||
        (st->bufmode == IOLBF && memchr(st->buf, '\n', st->wlen)))
        flush_write(st);
}

void crt_stream_sink(void *ctx, const char *s, size_t n)
{
    write_bytes(stream_of(ctx), s, n);
}

int crt_fputs_raw(CRT_FILE *f, const char *s, size_t n)
{
    int r = write_bytes(stream_of(f), s, n);
    crt_stream_done(f);
    return r;
}

int crt_fputc(int c, CRT_FILE *f)
{
    char ch = (char)c;
    return crt_fputs_raw(f, &ch, 1) < 0 ? CRT_EOF : (uint8_t)c;
}

static bool fill(struct cstream *st)
{
    if (!st->can_read || st->eof)
        return false;
    if (st->fd == 0 && std_streams[1].wlen)
        flush_write(&std_streams[1]);
    if (st->wlen)
        flush_write(st);
    if (!ensure_buf(st))
        return false;
    long r = crt_fd_read(st->fd, st->buf, BUF_SIZE);
    if (r <= 0) {
        if (r < 0)
            st->err = true;
        else
            st->eof = true;
        return false;
    }
    st->rpos = 0;
    st->rlen = (size_t)r;
    return true;
}

int crt_fgetc(CRT_FILE *f)
{
    struct cstream *st = stream_of(f);
    if (st->nungot)
        return st->ungot[--st->nungot];
    if (st->rpos >= st->rlen && !fill(st))
        return CRT_EOF;
    return (uint8_t)st->buf[st->rpos++];
}

int crt_ungetc(int c, CRT_FILE *f)
{
    struct cstream *st = stream_of(f);
    if (c == CRT_EOF || st->nungot >= (int)ARRAY_SIZE(st->ungot))
        return CRT_EOF;
    st->ungot[st->nungot++] = (uint8_t)c;
    st->eof = false;
    return (uint8_t)c;
}

static int crt_fclose(CRT_FILE *f)
{
    if (!f)
        return CRT_EOF;
    struct cstream *st = stream_of(f);
    int r = flush_write(st);
    fd_close(st->fd);
    if (st->tmpname) {
        char path[PATH_MAX_LEN];
        if (w32_path(st->tmpname, path, sizeof(path)) == 0)
            vfs_unlink(path);
    }
    if (f >= crt_iob && f < crt_iob + 3) {
        st->used = false;
        return r;
    }
    drop_file(f);
    return r;
}

void crt_close_all(void)
{
    crt_flush_all();
    for (int i = 0; i < CRT_MAX_FILES; i++)
        if (files[i])
            crt_fclose(&files[i]->pub);
}

static int crt_fcloseall(void)
{
    int n = 0;
    for (int i = 0; i < CRT_MAX_FILES; i++)
        if (files[i]) {
            crt_fclose(&files[i]->pub);
            n++;
        }
    return n;
}

static int crt_flushall(void)
{
    crt_flush_all();
    return CRT_MAX_FILES;
}

static CRT_FILE *crt_freopen(const char *path, const char *mode, CRT_FILE *f)
{
    struct cstream *st = stream_of(f);
    flush_write(st);
    int flags;
    bool rd, wr;
    if (!parse_mode(mode, &flags, &rd, &wr))
        return NULL;
    if (!path) {
        st->can_read = rd;
        st->can_write = wr;
        return f;
    }
    int fd = open_path(path, flags);
    if (fd < 0)
        return NULL;
    int old = st->fd;
    if (old <= 2) {
        crt_dup2(fd, old);
        fd_close(fd);
        fd = old;
    } else {
        fd_close(old);
    }
    char *buf = st->buf;
    stream_init(st, fd, rd, wr);
    st->buf = buf;
    f->file = fd;
    return f;
}

static size_t crt_fwrite(const void *p, size_t size, size_t count, CRT_FILE *f)
{
    size_t total = size * count;
    if (!total)
        return 0;
    if (write_bytes(stream_of(f), p, total) < 0)
        return 0;
    crt_stream_done(f);
    return count;
}

static size_t crt_fread(void *p, size_t size, size_t count, CRT_FILE *f)
{
    size_t total = size * count, got = 0;
    if (!total)
        return 0;
    struct cstream *st = stream_of(f);
    char *out = p;
    while (got < total && st->nungot)
        out[got++] = (char)st->ungot[--st->nungot];
    while (got < total) {
        if (st->rpos < st->rlen) {
            size_t k = MIN(total - got, st->rlen - st->rpos);
            memcpy(out + got, st->buf + st->rpos, k);
            st->rpos += k;
            got += k;
            continue;
        }
        if (total - got >= BUF_SIZE && !st->eof && st->can_read) {
            if (st->wlen)
                flush_write(st);
            long r = crt_fd_read(st->fd, out + got, total - got);
            if (r <= 0) {
                if (r < 0)
                    st->err = true;
                else
                    st->eof = true;
                break;
            }
            got += (size_t)r;
            continue;
        }
        if (!fill(st))
            break;
    }
    return got / size;
}

static int crt_fputs(const char *s, CRT_FILE *f)
{
    return crt_fputs_raw(f, s, strlen(s)) < 0 ? CRT_EOF : 0;
}

static int crt_puts(const char *s)
{
    if (write_bytes(&std_streams[1], s, strlen(s)) < 0 || write_bytes(&std_streams[1], "\n", 1) < 0)
        return CRT_EOF;
    crt_stream_done(&crt_iob[1]);
    return 0;
}

static int crt_putchar(int c)
{
    return crt_fputc(c, &crt_iob[1]);
}

static int crt_getchar(void)
{
    return crt_fgetc(&crt_iob[0]);
}

static char *crt_fgets(char *buf, int n, CRT_FILE *f)
{
    if (n <= 0)
        return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = crt_fgetc(f);
        if (c == CRT_EOF)
            break;
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    buf[i] = 0;
    return buf;
}

static char *crt_gets(char *buf)
{
    int i = 0, c;
    while ((c = crt_fgetc(&crt_iob[0])) != CRT_EOF && c != '\n')
        buf[i++] = (char)c;
    if (c == CRT_EOF && i == 0)
        return NULL;
    buf[i] = 0;
    return buf;
}

static bool wide_target(struct cstream *st)
{
    struct crt_fd *e = st->fd >= 0 && st->fd < CRT_MAX_FD ? &fds[st->fd] : NULL;
    return e && e->used && (e->flags & (CRT_O_U16TEXT | CRT_O_WTEXT)) && !crt_isatty(st->fd);
}

static void put_wide(CRT_FILE *f, const WCHAR *w, size_t n)
{
    struct cstream *st = stream_of(f);
    if (wide_target(st)) {
        write_bytes(st, (const char *)w, n * 2);
        return;
    }
    struct crt_fd *e = &fds[st->fd];
    int cp = e->flags & CRT_O_U8TEXT ? 65001 : crt_isatty(st->fd) ? 1 : 0;
    char out[256];
    while (n) {
        size_t k = MIN(n, (size_t)64);
        size_t len = w32_wide_to_cp(w, (int)k, out, sizeof(out), cp);
        if (crt_isatty(st->fd)) {
            flush_write(st);
            w32_console_write_raw(st->fd == 2 ? WH_STDERR : WH_STDOUT, out, MIN(len, sizeof(out)));
        } else {
            write_bytes(st, out, MIN(len, sizeof(out)));
        }
        w += k;
        n -= k;
    }
}

static void wide_sink(void *ctx, const WCHAR *s, size_t n)
{
    put_wide(ctx, s, n);
}

static unsigned crt_fputwc(WCHAR c, CRT_FILE *f)
{
    put_wide(f, &c, 1);
    crt_stream_done(f);
    return c;
}

static int crt_fputws(const WCHAR *s, CRT_FILE *f)
{
    put_wide(f, s, w32_wcslen(s));
    crt_stream_done(f);
    return 0;
}

static int crt_putws(const WCHAR *s)
{
    put_wide(&crt_iob[1], s, w32_wcslen(s));
    put_wide(&crt_iob[1], (const WCHAR[]){'\n'}, 1);
    crt_stream_done(&crt_iob[1]);
    return 0;
}

static unsigned crt_putwchar(WCHAR c)
{
    return crt_fputwc(c, &crt_iob[1]);
}

static unsigned crt_fgetwc(CRT_FILE *f)
{
    struct cstream *st = stream_of(f);
    if (wide_target(st)) {
        int a = crt_fgetc(f), b = crt_fgetc(f);
        return a == CRT_EOF || b == CRT_EOF ? 0xffff : (unsigned)(a | b << 8);
    }
    int c = crt_fgetc(f);
    if (c == CRT_EOF)
        return 0xffff;
    char ch = (char)c;
    WCHAR w;
    struct crt_fd *e = &fds[st->fd];
    if (e->flags & CRT_O_U8TEXT && c >= 0xc0) {
        char seq[4] = {ch};
        int need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
        for (int i = 1; i < need; i++)
            seq[i] = (char)crt_fgetc(f);
        w32_cp_to_wide(seq, need, &w, 1, 65001);
        return w;
    }
    w32_cp_to_wide(&ch, 1, &w, 1, crt_isatty(st->fd) ? 1 : 0);
    return w;
}

static unsigned crt_ungetwc(unsigned c, CRT_FILE *f)
{
    if (c == 0xffff)
        return c;
    char ch;
    WCHAR w = (WCHAR)c;
    w32_wide_to_cp(&w, 1, &ch, 1, 0);
    crt_ungetc((uint8_t)ch, f);
    return c;
}

static unsigned crt_getwchar(void)
{
    return crt_fgetwc(&crt_iob[0]);
}

static WCHAR *crt_fgetws(WCHAR *buf, int n, CRT_FILE *f)
{
    int i = 0;
    while (i < n - 1) {
        unsigned c = crt_fgetwc(f);
        if (c == 0xffff)
            break;
        buf[i++] = (WCHAR)c;
        if (c == '\n')
            break;
    }
    if (!i)
        return NULL;
    buf[i] = 0;
    return buf;
}

static int crt_fseek(CRT_FILE *f, long off, int whence)
{
    struct cstream *st = stream_of(f);
    if (st->wlen)
        flush_write(st);
    if (whence == VFS_SEEK_CUR)
        off -= (long)(st->rlen - st->rpos) + st->nungot;
    st->rpos = st->rlen = 0;
    st->nungot = 0;
    st->eof = false;
    return fd_seek(st->fd, off, whence) < 0 ? -1 : 0;
}

static int crt_fseeki64(CRT_FILE *f, int64_t off, int whence)
{
    return crt_fseek(f, (long)off, whence);
}

static long crt_ftell(CRT_FILE *f)
{
    struct cstream *st = stream_of(f);
    long pos = fd_seek(st->fd, 0, VFS_SEEK_CUR);
    if (pos < 0)
        return -1;
    return pos - (long)(st->rlen - st->rpos) - st->nungot + (long)st->wlen;
}

static int64_t crt_ftelli64(CRT_FILE *f)
{
    return crt_ftell(f);
}

static int crt_fgetpos(CRT_FILE *f, int64_t *pos)
{
    long p = crt_ftell(f);
    if (p < 0)
        return -1;
    *pos = p;
    return 0;
}

static int crt_fsetpos(CRT_FILE *f, const int64_t *pos)
{
    return crt_fseek(f, (long)*pos, VFS_SEEK_SET);
}

static void crt_rewind(CRT_FILE *f)
{
    crt_fseek(f, 0, VFS_SEEK_SET);
    stream_of(f)->err = false;
}

static int crt_feof(CRT_FILE *f)
{
    return stream_of(f)->eof && !stream_of(f)->nungot ? 0x10 : 0;
}

static int crt_ferror(CRT_FILE *f)
{
    return stream_of(f)->err ? 0x20 : 0;
}

static void crt_clearerr(CRT_FILE *f)
{
    stream_of(f)->eof = false;
    stream_of(f)->err = false;
}

static int crt_fileno(CRT_FILE *f)
{
    return stream_of(f)->fd;
}

static int crt_setvbuf(CRT_FILE *f, char *buf, int mode, size_t size)
{
    UNUSED(buf);
    UNUSED(size);
    struct cstream *st = stream_of(f);
    flush_write(st);
    st->bufmode = mode;
    st->user_mode = true;
    return 0;
}

static void crt_setbuf(CRT_FILE *f, char *buf)
{
    crt_setvbuf(f, buf, buf ? IOFBF : IONBF, BUF_SIZE);
}

static int crt_filbuf(CRT_FILE *f)
{
    return crt_fgetc(f);
}

static int crt_flsbuf(int c, CRT_FILE *f)
{
    return crt_fputc(c, f);
}

static int crt_getw(CRT_FILE *f)
{
    int v;
    return crt_fread(&v, sizeof(v), 1, f) == 1 ? v : CRT_EOF;
}

static int crt_putw(int v, CRT_FILE *f)
{
    return crt_fwrite(&v, sizeof(v), 1, f) == 1 ? v : CRT_EOF;
}

static W32_STATE char tmp_counter;

static char *crt_tmpnam(char *buf)
{
    static char name[64];
    char *out = buf ? buf : name;
    for (;;) {
        ksnprintf(out, 64, "C:\\Windows\\Temp\\s%x%c.tmp", hal_entropy() & 0xffff, 'a' + (tmp_counter++ % 26));
        char path[PATH_MAX_LEN];
        if (w32_path(out, path, sizeof(path)) != 0 || !vfs_exists(path))
            return out;
    }
}

static char *crt_tempnam(const char *dir, const char *prefix)
{
    char tmp[64], *out = w32_alloc(PATH_MAX_LEN);
    crt_tmpnam(tmp);
    ksnprintf(out, PATH_MAX_LEN, "%s\\%s%s", dir ? dir : "C:\\Windows\\Temp", prefix ? prefix : "",
              strrchr(tmp, '\\') + 1);
    return out;
}

static CRT_FILE *crt_tmpfile(void)
{
    char name[64];
    crt_tmpnam(name);
    CRT_FILE *f = crt_fopen(name, "w+b");
    if (f)
        stream_of(f)->tmpname = w32_strdup(name);
    return f;
}

static const char *const errno_text[] = {
    [0] = "No error",
    [CRT_EPERM] = "Operation not permitted",
    [CRT_ENOENT] = "No such file or directory",
    [CRT_EINTR] = "Interrupted function call",
    [CRT_EIO] = "Input/output error",
    [CRT_EBADF] = "Bad file descriptor",
    [CRT_ENOMEM] = "Not enough space",
    [CRT_EACCES] = "Permission denied",
    [CRT_EEXIST] = "File exists",
    [CRT_EXDEV] = "Improper link",
    [CRT_ENOTDIR] = "Not a directory",
    [CRT_EISDIR] = "Is a directory",
    [CRT_EINVAL] = "Invalid argument",
    [CRT_EMFILE] = "Too many open files",
    [CRT_ENOSPC] = "No space left on device",
    [CRT_ESPIPE] = "Invalid seek",
    [CRT_EDOM] = "Domain error",
    [CRT_ERANGE] = "Result too large",
    [CRT_ENAMETOOLONG] = "Filename too long",
    [CRT_ENOSYS] = "Function not implemented",
    [CRT_ENOTEMPTY] = "Directory not empty",
};

static char *crt_strerror(int e)
{
    if (e >= 0 && e < (int)ARRAY_SIZE(errno_text) && errno_text[e])
        return (char *)errno_text[e];
    return "Unknown error";
}

static void crt_perror(const char *s)
{
    crt_flush_all();
    char buf[256];
    int n = ksnprintf(buf, sizeof(buf), "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "",
                      crt_strerror(crt_errno_value));
    crt_fd_write(2, buf, (size_t)MIN(n, (int)sizeof(buf) - 1));
}

static int path_op(const char *a, char *out)
{
    if (w32_path(a, out, PATH_MAX_LEN) != 0) {
        crt_set_errno(CRT_ENOENT);
        return -1;
    }
    return 0;
}

static int vfs_result(int err)
{
    if (err) {
        crt_set_errno(crt_errno_from_vfs(err));
        return -1;
    }
    return 0;
}

static int crt_remove(const char *name)
{
    char path[PATH_MAX_LEN];
    if (path_op(name, path) < 0)
        return -1;
    if (vfs_is_dir(path)) {
        crt_set_errno(CRT_EACCES);
        return -1;
    }
    return vfs_result(vfs_unlink(path));
}

static int crt_wremove(const WCHAR *name)
{
    char *n = w32_narrow(name);
    int r = crt_remove(n);
    w32_free(n);
    return r;
}

static int crt_rename(const char *from, const char *to)
{
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    if (path_op(from, a) < 0 || path_op(to, b) < 0)
        return -1;
    if (vfs_exists(b)) {
        crt_set_errno(CRT_EEXIST);
        return -1;
    }
    int err = vfs_rename(a, b);
    if (err == E_XDEV && (err = vfs_copy(a, b)) == 0)
        err = vfs_unlink(a);
    return vfs_result(err);
}

static int crt_wrename(const WCHAR *from, const WCHAR *to)
{
    char *a = w32_narrow(from), *b = w32_narrow(to);
    int r = crt_rename(a, b);
    w32_free(a);
    w32_free(b);
    return r;
}

static int crt_mkdir(const char *name)
{
    char path[PATH_MAX_LEN];
    return path_op(name, path) < 0 ? -1 : vfs_result(vfs_mkdir(path));
}

static int crt_wmkdir(const WCHAR *name)
{
    char *n = w32_narrow(name);
    int r = crt_mkdir(n);
    w32_free(n);
    return r;
}

static int crt_rmdir(const char *name)
{
    char path[PATH_MAX_LEN];
    if (path_op(name, path) < 0)
        return -1;
    if (!vfs_is_dir(path)) {
        crt_set_errno(vfs_exists(path) ? CRT_ENOTDIR : CRT_ENOENT);
        return -1;
    }
    return vfs_result(vfs_unlink(path));
}

static int crt_wrmdir(const WCHAR *name)
{
    char *n = w32_narrow(name);
    int r = crt_rmdir(n);
    w32_free(n);
    return r;
}

static int crt_chdir(const char *name)
{
    char path[PATH_MAX_LEN];
    if (path_op(name, path) < 0)
        return -1;
    if (!vfs_is_dir(path)) {
        crt_set_errno(CRT_ENOENT);
        return -1;
    }
    str_copy(w32->cwd, path, sizeof(w32->cwd));
    return 0;
}

static int crt_wchdir(const WCHAR *name)
{
    char *n = w32_narrow(name);
    int r = crt_chdir(n);
    w32_free(n);
    return r;
}

static char *crt_getcwd(char *buf, int size)
{
    char win[PATH_MAX_LEN];
    w32_path_to_win(w32->cwd, win, sizeof(win));
    size_t n = strlen(win) + 1;
    if (!buf) {
        buf = w32_alloc(MAX(n, (size_t)size));
        size = (int)MAX(n, (size_t)size);
    }
    if (n > (size_t)size) {
        crt_set_errno(CRT_ERANGE);
        return NULL;
    }
    memcpy(buf, win, n);
    return buf;
}

static char *crt_getdcwd(int drive, char *buf, int size)
{
    UNUSED(drive);
    return crt_getcwd(buf, size);
}

static WCHAR *crt_wgetcwd(WCHAR *buf, int size)
{
    char win[PATH_MAX_LEN];
    w32_path_to_win(w32->cwd, win, sizeof(win));
    size_t n = w32_cp_to_wide(win, -1, NULL, 0, 0);
    if (!buf) {
        size = (int)MAX(n, (size_t)size);
        buf = w32_alloc((size_t)size * sizeof(WCHAR));
    }
    if (n > (size_t)size) {
        crt_set_errno(CRT_ERANGE);
        return NULL;
    }
    w32_cp_to_wide(win, -1, buf, (size_t)size, 0);
    return buf;
}

static int crt_getdrive(void)
{
    char win[PATH_MAX_LEN];
    w32_path_to_win(w32->cwd, win, sizeof(win));
    return to_upper((uint8_t)win[0]) - 'A' + 1;
}

static int crt_chdrive(int drive)
{
    char root[4] = {(char)('A' + drive - 1), ':', '\\', 0};
    return crt_chdir(root);
}

static int stat_common(const char *path, struct vstat *st, uint16_t *mode)
{
    char p[PATH_MAX_LEN];
    if (path_op(path, p) < 0)
        return -1;
    if (vfs_stat(p, st) != 0) {
        crt_set_errno(CRT_ENOENT);
        return -1;
    }
    uint16_t m = st->type == VN_DIR ? 0x4000 | 0x40 : st->type == VN_DEV ? 0x2000 : 0x8000;
    m |= 0x100 | 0x80;
    const char *ext = path_ext(p);
    if (ext && (str_casecmp(ext, ".exe") == 0 || str_casecmp(ext, ".com") == 0 || str_casecmp(ext, ".bat") == 0 ||
                str_casecmp(ext, ".cmd") == 0))
        m |= 0x40;
    m |= (uint16_t)((m & 0x1c0) >> 3 | (m & 0x1c0) >> 6);
    *mode = m;
    return 0;
}

static void fill_stat(uint8_t *out, const struct vstat *st, uint16_t mode, bool size64, bool time64)
{
    memset(out, 0, time64 ? 56 : size64 ? 48 : 36);
    uint32_t dev = 2;
    memcpy(out, &dev, 4);
    memcpy(out + 6, &mode, 2);
    out[8] = 1;
    memcpy(out + 16, &dev, 4);
    if (time64) {
        uint64_t size = st->size, t = st->mtime;
        memcpy(out + 24, &size, 8);
        memcpy(out + 32, &t, 8);
        memcpy(out + 40, &t, 8);
        memcpy(out + 48, &t, 8);
    } else if (size64) {
        uint64_t size = st->size;
        memcpy(out + 24, &size, 8);
        memcpy(out + 32, &st->mtime, 4);
        memcpy(out + 36, &st->mtime, 4);
        memcpy(out + 40, &st->mtime, 4);
    } else {
        memcpy(out + 20, &st->size, 4);
        memcpy(out + 24, &st->mtime, 4);
        memcpy(out + 28, &st->mtime, 4);
        memcpy(out + 32, &st->mtime, 4);
    }
}

static int do_stat(const char *path, uint8_t *out, bool size64, bool time64)
{
    struct vstat st;
    uint16_t mode;
    if (stat_common(path, &st, &mode) < 0)
        return -1;
    fill_stat(out, &st, mode, size64, time64);
    return 0;
}

static int do_wstat(const WCHAR *path, uint8_t *out, bool size64, bool time64)
{
    char *p = w32_narrow(path);
    int r = do_stat(p, out, size64, time64);
    w32_free(p);
    return r;
}

static int do_fstat(int fd, uint8_t *out, bool size64, bool time64)
{
    struct crt_fd *e = fd_get(fd);
    if (!e)
        return -1;
    struct w32_object *o = w32_handle_get(e->h);
    struct vstat st = {0};
    uint16_t mode = 0x2000 | 0x1b6;
    if (o && o->type == WH_FILE) {
        vfs_fstat(o->file, &st);
        mode = (st.type == VN_DIR ? 0x4000 : 0x8000) | 0x1b6;
    } else if (o && !crt_isatty(fd)) {
        mode = 0x1000 | 0x1b6;
    }
    fill_stat(out, &st, mode, size64, time64);
    return 0;
}

static int crt_stat32(const char *p, uint8_t *o) { return do_stat(p, o, false, false); }
static int crt_stati64(const char *p, uint8_t *o) { return do_stat(p, o, true, false); }
static int crt_stat64(const char *p, uint8_t *o) { return do_stat(p, o, true, true); }
static int crt_wstat32(const WCHAR *p, uint8_t *o) { return do_wstat(p, o, false, false); }
static int crt_wstati64(const WCHAR *p, uint8_t *o) { return do_wstat(p, o, true, false); }
static int crt_wstat64(const WCHAR *p, uint8_t *o) { return do_wstat(p, o, true, true); }
static int crt_fstat32(int fd, uint8_t *o) { return do_fstat(fd, o, false, false); }
static int crt_fstati64(int fd, uint8_t *o) { return do_fstat(fd, o, true, false); }
static int crt_fstat64(int fd, uint8_t *o) { return do_fstat(fd, o, true, true); }

static int crt_access(const char *path, int mode)
{
    UNUSED(mode);
    char p[PATH_MAX_LEN];
    if (path_op(path, p) < 0)
        return -1;
    if (!vfs_exists(p)) {
        crt_set_errno(CRT_ENOENT);
        return -1;
    }
    return 0;
}

static int crt_waccess(const WCHAR *path, int mode)
{
    char *p = w32_narrow(path);
    int r = crt_access(p, mode);
    w32_free(p);
    return r;
}

static int crt_access_s(const char *path, int mode)
{
    return crt_access(path, mode) == 0 ? 0 : crt_errno_value;
}

static int crt_chmod(const char *path, int mode)
{
    return crt_access(path, mode);
}

static int crt_umask(int mode)
{
    UNUSED(mode);
    return 0;
}

static int crt_unlink(const char *name)
{
    return crt_remove(name);
}

static void fill_finddata(uint8_t *out, const struct w32_find_entry *e, int layout)
{
    uint32_t attr = e->type == VN_DIR ? 0x10 : 0x20;
    memset(out, 0, 304);
    memcpy(out, &attr, 4);
    uint64_t t = e->mtime, size = e->size;
    switch (layout) {
    case 0:
        memcpy(out + 4, &e->mtime, 4);
        memcpy(out + 8, &e->mtime, 4);
        memcpy(out + 12, &e->mtime, 4);
        memcpy(out + 16, &e->size, 4);
        str_copy((char *)out + 20, e->name, 260);
        break;
    case 1:
        memcpy(out + 4, &e->mtime, 4);
        memcpy(out + 8, &e->mtime, 4);
        memcpy(out + 12, &e->mtime, 4);
        memcpy(out + 16, &size, 8);
        str_copy((char *)out + 24, e->name, 260);
        break;
    case 2:
        memcpy(out + 8, &t, 8);
        memcpy(out + 16, &t, 8);
        memcpy(out + 24, &t, 8);
        memcpy(out + 32, &size, 8);
        str_copy((char *)out + 40, e->name, 260);
        break;
    default:
        memcpy(out + 8, &t, 8);
        memcpy(out + 16, &t, 8);
        memcpy(out + 24, &t, 8);
        memcpy(out + 32, &e->size, 4);
        str_copy((char *)out + 36, e->name, 260);
        break;
    }
}

static intptr_t find_first(const char *spec, uint8_t *out, int layout)
{
    struct w32_find_entry e;
    HANDLE h = w32_find_first(spec, &e);
    if (h == INVALID_HANDLE_VALUE) {
        crt_set_errno(CRT_ENOENT);
        return -1;
    }
    fill_finddata(out, &e, layout);
    return (intptr_t)h;
}

static int find_next(intptr_t h, uint8_t *out, int layout)
{
    struct w32_find_entry e;
    if (!w32_find_next((HANDLE)h, &e)) {
        crt_set_errno(CRT_ENOENT);
        return -1;
    }
    fill_finddata(out, &e, layout);
    return 0;
}

static intptr_t crt_findfirst(const char *s, uint8_t *o) { return find_first(s, o, 0); }
static intptr_t crt_findfirsti64(const char *s, uint8_t *o) { return find_first(s, o, 1); }
static intptr_t crt_findfirst64(const char *s, uint8_t *o) { return find_first(s, o, 2); }
static intptr_t crt_findfirst64i32(const char *s, uint8_t *o) { return find_first(s, o, 3); }
static int crt_findnext(intptr_t h, uint8_t *o) { return find_next(h, o, 0); }
static int crt_findnexti64(intptr_t h, uint8_t *o) { return find_next(h, o, 1); }
static int crt_findnext64(intptr_t h, uint8_t *o) { return find_next(h, o, 2); }
static int crt_findnext64i32(intptr_t h, uint8_t *o) { return find_next(h, o, 3); }

static int crt_findclose(intptr_t h)
{
    return w32_handle_close((HANDLE)h) ? 0 : -1;
}

static CRT_FILE *crt_iob_func(void)
{
    return crt_iob;
}

static CRT_FILE *crt_acrt_iob_func(unsigned i)
{
    return &crt_iob[i];
}

int crt_vfprintf_impl(CRT_FILE *f, const char *fmt, va_list ap)
{
    int n = crt_vformat(crt_stream_sink, f, fmt, ap);
    crt_stream_done(f);
    return n;
}

static int crt_vfprintf(CRT_FILE *f, const char *fmt, va_list ap)
{
    return crt_vfprintf_impl(f, fmt, ap);
}

static int crt_fprintf(CRT_FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfprintf_impl(f, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vprintf(const char *fmt, va_list ap)
{
    return crt_vfprintf_impl(&crt_iob[1], fmt, ap);
}

static int crt_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfprintf_impl(&crt_iob[1], fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vfwprintf(CRT_FILE *f, const WCHAR *fmt, va_list ap)
{
    int n = crt_vwformat(wide_sink, f, fmt, ap);
    crt_stream_done(f);
    return n;
}

static int crt_fwprintf(CRT_FILE *f, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfwprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vwprintf(const WCHAR *fmt, va_list ap)
{
    return crt_vfwprintf(&crt_iob[1], fmt, ap);
}

static int crt_wprintf(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfwprintf(&crt_iob[1], fmt, ap);
    va_end(ap);
    return n;
}

static int stream_get(void *ctx)
{
    return crt_fgetc(ctx);
}

static void stream_unget(int c, void *ctx)
{
    crt_ungetc(c, ctx);
}

static int crt_vfscanf(CRT_FILE *f, const char *fmt, va_list ap)
{
    crt_flush_all();
    return crt_vscan(stream_get, stream_unget, f, fmt, ap);
}

static int crt_fscanf(CRT_FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfscanf(&crt_iob[0], fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vscanf(const char *fmt, va_list ap)
{
    return crt_vfscanf(&crt_iob[0], fmt, ap);
}

static const struct w32_export stdio_exports[] = {
    W32_AS("_iob", crt_iob), W32_AS("__iob_func", crt_iob_func), W32_AS("__acrt_iob_func", crt_acrt_iob_func),
    W32_AS("_open", CDECL_open), W32_AS("_wopen", crt_wopen), W32_AS("_sopen", crt_sopen),
    W32_AS("_wsopen", crt_wopen), W32_AS("_sopen_s", crt_sopen_s), W32_AS("_creat", crt_creat),
    W32_AS("_close", crt_close), W32_AS("_read", crt_read), W32_AS("_write", crt_write),
    W32_AS("_lseek", crt_lseek), W32_AS("_lseeki64", crt_lseeki64), W32_AS("_tell", crt_tell),
    W32_AS("_telli64", crt_telli64), W32_AS("_filelength", crt_filelength),
    W32_AS("_filelengthi64", crt_filelengthi64), W32_AS("_eof", crt_eof), W32_AS("_chsize", crt_chsize),
    W32_AS("_chsize_s", crt_chsize_s), W32_AS("_commit", crt_commit), W32_AS("_dup", crt_dup),
    W32_AS("_dup2", crt_dup2), W32_AS("_isatty", crt_isatty_export), W32_AS("_setmode", crt_setmode),
    W32_AS("_get_osfhandle", crt_get_osfhandle), W32_AS("_open_osfhandle", crt_open_osfhandle),
    W32_AS("_locking", crt_locking), W32_AS("fopen", crt_fopen), W32_AS("_wfopen", crt_wfopen),
    W32_AS("fopen_s", crt_fopen_s), W32_AS("_wfopen_s", crt_wfopen_s), W32_AS("_fsopen", crt_fsopen),
    W32_AS("_fdopen", crt_fdopen), W32_AS("freopen", crt_freopen), W32_AS("fclose", crt_fclose),
    W32_AS("_fcloseall", crt_fcloseall), W32_AS("fflush", crt_fflush), W32_AS("_flushall", crt_flushall),
    W32_AS("fwrite", crt_fwrite), W32_AS("fread", crt_fread), W32_AS("fputc", crt_fputc),
    W32_AS("putc", crt_fputc), W32_AS("_fputc_nolock", crt_fputc), W32_AS("_putc_nolock", crt_fputc),
    W32_AS("fputs", crt_fputs), W32_AS("puts", crt_puts), W32_AS("putchar", crt_putchar),
    W32_AS("_fputchar", crt_putchar), W32_AS("fgetc", crt_fgetc), W32_AS("getc", crt_fgetc),
    W32_AS("_fgetc_nolock", crt_fgetc), W32_AS("_getc_nolock", crt_fgetc), W32_AS("getchar", crt_getchar),
    W32_AS("_fgetchar", crt_getchar), W32_AS("ungetc", crt_ungetc), W32_AS("fgets", crt_fgets),
    W32_AS("gets", crt_gets), W32_AS("fputwc", crt_fputwc), W32_AS("putwc", crt_fputwc),
    W32_AS("fputws", crt_fputws), W32_AS("_putws", crt_putws), W32_AS("putwchar", crt_putwchar),
    W32_AS("_fputwchar", crt_putwchar), W32_AS("fgetwc", crt_fgetwc), W32_AS("getwc", crt_fgetwc),
    W32_AS("getwchar", crt_getwchar), W32_AS("_fgetwchar", crt_getwchar), W32_AS("ungetwc", crt_ungetwc),
    W32_AS("fgetws", crt_fgetws), W32_AS("fseek", crt_fseek), W32_AS("_fseeki64", crt_fseeki64),
    W32_AS("ftell", crt_ftell), W32_AS("_ftelli64", crt_ftelli64), W32_AS("fgetpos", crt_fgetpos),
    W32_AS("fsetpos", crt_fsetpos), W32_AS("rewind", crt_rewind), W32_AS("feof", crt_feof),
    W32_AS("ferror", crt_ferror), W32_AS("clearerr", crt_clearerr), W32_AS("fileno", crt_fileno),
    W32_AS("_fileno", crt_fileno), W32_AS("setvbuf", crt_setvbuf), W32_AS("setbuf", crt_setbuf),
    W32_AS("_filbuf", crt_filbuf), W32_AS("_flsbuf", crt_flsbuf), W32_AS("_getw", crt_getw),
    W32_AS("_putw", crt_putw), W32_AS("tmpnam", crt_tmpnam), W32_AS("_tempnam", crt_tempnam),
    W32_AS("tmpfile", crt_tmpfile), W32_AS("strerror", crt_strerror), W32_AS("perror", crt_perror),
    W32_AS("remove", crt_remove), W32_AS("_wremove", crt_wremove), W32_AS("rename", crt_rename),
    W32_AS("_wrename", crt_wrename), W32_AS("_unlink", crt_unlink), W32_AS("unlink", crt_unlink),
    W32_AS("_wunlink", crt_wremove), W32_AS("_mkdir", crt_mkdir), W32_AS("mkdir", crt_mkdir),
    W32_AS("_wmkdir", crt_wmkdir), W32_AS("_rmdir", crt_rmdir), W32_AS("rmdir", crt_rmdir),
    W32_AS("_wrmdir", crt_wrmdir), W32_AS("_chdir", crt_chdir), W32_AS("chdir", crt_chdir),
    W32_AS("_wchdir", crt_wchdir), W32_AS("_getcwd", crt_getcwd), W32_AS("getcwd", crt_getcwd),
    W32_AS("_getdcwd", crt_getdcwd), W32_AS("_wgetcwd", crt_wgetcwd), W32_AS("_getdrive", crt_getdrive),
    W32_AS("_chdrive", crt_chdrive), W32_AS("_stat", crt_stat32), W32_AS("_stat32", crt_stat32),
    W32_AS("_stati64", crt_stati64), W32_AS("_stat32i64", crt_stati64), W32_AS("_stat64", crt_stat64),
    W32_AS("_wstat", crt_wstat32), W32_AS("_wstat32", crt_wstat32), W32_AS("_wstati64", crt_wstati64),
    W32_AS("_wstat64", crt_wstat64), W32_AS("_fstat", crt_fstat32), W32_AS("_fstat32", crt_fstat32),
    W32_AS("_fstati64", crt_fstati64), W32_AS("_fstat32i64", crt_fstati64), W32_AS("_fstat64", crt_fstat64),
    W32_AS("_access", crt_access), W32_AS("access", crt_access), W32_AS("_waccess", crt_waccess),
    W32_AS("_access_s", crt_access_s), W32_AS("_chmod", crt_chmod), W32_AS("_umask", crt_umask),
    W32_AS("_findfirst", crt_findfirst), W32_AS("_findfirst32", crt_findfirst),
    W32_AS("_findfirsti64", crt_findfirsti64), W32_AS("_findfirst64", crt_findfirst64),
    W32_AS("_findfirst64i32", crt_findfirst64i32), W32_AS("_findnext", crt_findnext),
    W32_AS("_findnext32", crt_findnext), W32_AS("_findnexti64", crt_findnexti64),
    W32_AS("_findnext64", crt_findnext64), W32_AS("_findnext64i32", crt_findnext64i32),
    W32_AS("_findclose", crt_findclose), W32_AS("vfprintf", crt_vfprintf), W32_AS("fprintf", crt_fprintf),
    W32_AS("vprintf", crt_vprintf), W32_AS("printf", crt_printf), W32_AS("vfwprintf", crt_vfwprintf),
    W32_AS("fwprintf", crt_fwprintf), W32_AS("vwprintf", crt_vwprintf), W32_AS("wprintf", crt_wprintf),
    W32_AS("vfscanf", crt_vfscanf), W32_AS("fscanf", crt_fscanf), W32_AS("scanf", crt_scanf),
    W32_AS("vscanf", crt_vscanf),
};

CRT_EXPORTS(crt_stdio_module, stdio_exports);
