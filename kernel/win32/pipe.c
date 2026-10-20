#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

struct w32_pipe {
    uint8_t *buf;
    size_t len, cap, rpos;
    int readers, writers, sync_waiters;
};

static void pipe_kick(struct w32_pipe *p);

static void pipe_drop(struct w32_pipe *p)
{
    w32_kick_all();
    if (p->readers <= 0 && p->writers <= 0) {
        kfree(p->buf);
        kfree(p);
    }
}

struct w32_npipe {
    int refs;
    char name[128];
    struct w32_pipe *in, *out;
    bool connected;
    struct w32_npipe *next;
};

static struct w32_npipe *npipes;

static void npipe_detach(struct w32_object *o)
{
    if (o->pipe) {
        o->pipe->readers--;
        pipe_drop(o->pipe);
    }
    if (o->pipe_out) {
        o->pipe_out->writers--;
        pipe_drop(o->pipe_out);
    }
    o->pipe = o->pipe_out = NULL;
}

static void npipe_remove(struct w32_npipe *np)
{
    for (struct w32_npipe **pp = &npipes; *pp; pp = &(*pp)->next)
        if (*pp == np) {
            *pp = np->next;
            break;
        }
    kfree(np);
}

void w32_pipe_release(struct w32_object *o)
{
    if (o->value == 2) {
        npipe_detach(o);
        if (o->npipe && --o->npipe->refs <= 0)
            npipe_remove(o->npipe);
        o->npipe = NULL;
        return;
    }
    struct w32_pipe *p = o->pipe;
    if (!p)
        return;
    if (o->value)
        p->writers--;
    else
        p->readers--;
    o->pipe = NULL;
    pipe_drop(p);
}

long w32_pipe_write(struct w32_pipe *p, const void *data, size_t n)
{
    w32_signal();
    if (p->readers <= 0)
        return E_PIPE;
    if (p->rpos && p->rpos == p->len)
        p->rpos = p->len = 0;
    if (p->len + n > p->cap) {
        size_t cap = MAX(p->cap * 2, p->len + n + 4096);
        uint8_t *g = krealloc(p->buf, cap);
        if (!g)
            return E_NOMEM;
        p->buf = g;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;
    pipe_kick(p);
    return (long)n;
}

long w32_pipe_read(struct w32_pipe *p, void *out, size_t n, bool wait)
{
    while (p->rpos == p->len && p->writers > 0 && wait && w32) {
        p->sync_waiters++;
        w32_block();
        p->sync_waiters--;
    }
    if (p->rpos == p->len && p->writers > 0)
        return 0;
    size_t k = MIN(n, p->len - p->rpos);
    memcpy(out, p->buf + p->rpos, k);
    p->rpos += k;
    return (long)k;
}

size_t w32_pipe_available(struct w32_pipe *p)
{
    return p->len - p->rpos;
}

static BOOL WINAPI CreatePipe(HANDLE *rd, HANDLE *wr, void *sa, DWORD size)
{
    UNUSED(size);
    struct w32_pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return FALSE;
    HANDLE r = w32_handle_new(WH_PIPE), w = w32_handle_new(WH_PIPE);
    struct w32_object *ro = w32_handle_get(r), *wo = w32_handle_get(w);
    if (!ro || !wo) {
        if (ro)
            w32_handle_close(r);
        kfree(p);
        return FALSE;
    }
    ro->pipe = p;
    wo->pipe = p;
    wo->value = 1;
    p->readers = p->writers = 1;
    *rd = w32_inheritable(r, sa);
    *wr = w32_inheritable(w, sa);
    return TRUE;
}

static BOOL WINAPI TransactNamedPipe(HANDLE h, const void *in, DWORD in_size, void *out, DWORD out_size, DWORD *read,
                                     uint32_t *overlapped)
{
    struct w32_object *o = w32_handle_get(h);
    if (read)
        *read = 0;
    if (!o || o->type != WH_PIPE) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    long put = w32_handle_write(h, in, in_size);
    long got = put < 0 ? put : w32_handle_read(h, out, out_size);
    if (got < 0) {
        w32_set_error(ERROR_BROKEN_PIPE);
        return FALSE;
    }
    if (read)
        *read = (DWORD)got;
    if (overlapped)
        w32_io_complete(o, overlapped, 0, (uint32_t)got, true);
    return TRUE;
}

static BOOL WINAPI PeekNamedPipe(HANDLE h, void *buf, DWORD size, DWORD *read, DWORD *avail, DWORD *left)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_PIPE || !o->pipe) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    struct w32_pipe *p = o->pipe;
    DWORD have = (DWORD)w32_pipe_available(p), k = MIN(size, have);
    if (buf && k)
        memcpy(buf, p->buf + p->rpos, k);
    if (read)
        *read = buf ? k : 0;
    if (avail)
        *avail = have;
    if (left)
        *left = 0;
    if (!have && p->writers <= 0) {
        w32_set_error(ERROR_BROKEN_PIPE);
        return FALSE;
    }
    return TRUE;
}

struct pipe_end {
    struct w32_pipe *pipe;
    bool write;
};

static long stream_pipe_write(struct stream *s, const void *buf, size_t n)
{
    struct pipe_end *e = s->ctx;
    long r = w32_pipe_write(e->pipe, buf, n);
    return r < 0 ? (long)n : r;
}

static long stream_pipe_read(struct stream *s, void *buf, size_t n)
{
    struct pipe_end *e = s->ctx;
    return w32_pipe_read(e->pipe, buf, n, true);
}

static void stream_pipe_close(struct stream *s)
{
    struct pipe_end *e = s->ctx;
    if (e->write)
        e->pipe->writers--;
    else
        e->pipe->readers--;
    pipe_drop(e->pipe);
    kfree(e);
}

static long stream_file_write(struct stream *s, const void *buf, size_t n)
{
    struct w32_object *o = s->ctx;
    if (o->append)
        vfs_seek(o->file, 0, VFS_SEEK_END);
    return vfs_write(o->file, buf, n);
}

static long stream_file_read(struct stream *s, void *buf, size_t n)
{
    struct w32_object *o = s->ctx;
    return vfs_read(o->file, buf, n);
}

static const struct stream_ops pipe_stream_ops = {stream_pipe_read, stream_pipe_write, stream_pipe_close};
static const struct stream_ops file_stream_ops = {stream_file_read, stream_file_write, NULL};
static long null_read(struct stream *s, void *buf, size_t n)
{
    UNUSED(s);
    UNUSED(buf);
    UNUSED(n);
    return 0;
}

static long null_write(struct stream *s, const void *buf, size_t n)
{
    UNUSED(s);
    UNUSED(buf);
    return (long)n;
}

static const struct stream_ops null_stream_ops = {null_read, null_write, NULL};

struct stream *w32_handle_stream(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type == WH_STDIN || o->type == WH_STDOUT || o->type == WH_STDERR)
        return NULL;
    const struct stream_ops *ops = o->type == WH_PIPE && o->pipe ? &pipe_stream_ops
                                   : o->type == WH_FILE           ? &file_stream_ops
                                                                  : &null_stream_ops;
    struct stream *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->ops = ops;
    s->ctx = o;
    if (ops == &pipe_stream_ops) {
        struct pipe_end *e = kzalloc(sizeof(*e));
        if (!e) {
            kfree(s);
            return NULL;
        }
        e->pipe = o->pipe;
        e->write = o->value != 0;
        if (e->write)
            o->pipe->writers++;
        else
            o->pipe->readers++;
        s->ctx = e;
    }
    return s;
}

bool w32_pipe_clone(struct w32_object *o)
{
    if (o->value == 2) {
        if (o->pipe)
            o->pipe->readers++;
        if (o->pipe_out)
            o->pipe_out->writers++;
        if (o->npipe)
            o->npipe->refs++;
    } else if (o->pipe) {
        if (o->value)
            o->pipe->writers++;
        else
            o->pipe->readers++;
    }
    return true;
}

bool w32_is_pipe_name(const char *name)
{
    return name && (name[0] == '\\' || name[0] == '/') && (name[1] == '\\' || name[1] == '/') &&
           (name[2] == '.' || name[2] == '?') && (name[3] == '\\' || name[3] == '/') &&
           !str_ncasecmp(name + 4, "pipe", 4) && (name[8] == '\\' || name[8] == '/');
}

static bool npipe_listen(struct w32_object *o, struct w32_npipe *np)
{
    struct w32_pipe *in = kzalloc(sizeof(*in)), *out = kzalloc(sizeof(*out));
    if (!in || !out) {
        kfree(in);
        kfree(out);
        return false;
    }
    in->readers = 1;
    out->writers = 1;
    o->pipe = in;
    o->pipe_out = out;
    np->in = in;
    np->out = out;
    np->connected = false;
    return true;
}

static HANDLE create_named_pipe(const char *name, DWORD open_mode)
{
    if (!w32_is_pipe_name(name)) {
        w32_set_error(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    if (open_mode & 0x00080000u)
        for (struct w32_npipe *np = npipes; np; np = np->next)
            if (!str_casecmp(np->name, name + 9)) {
                w32_set_error(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
    struct w32_npipe *np = kzalloc(sizeof(*np));
    HANDLE h = np ? w32_handle_new(WH_PIPE) : NULL;
    struct w32_object *o = w32_handle_get(h);
    if (!o || !npipe_listen(o, np)) {
        if (o)
            w32_handle_close(h);
        kfree(np);
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    str_copy(np->name, name + 9, sizeof(np->name));
    np->refs = 1;
    o->value = 2;
    o->npipe = np;
    np->next = npipes;
    npipes = np;
    return h;
}

static HANDLE WINAPI CreateNamedPipeA(LPCSTR name, DWORD open_mode, DWORD pipe_mode, DWORD instances, DWORD out_size,
                                      DWORD in_size, DWORD timeout, void *sa)
{
    UNUSED(pipe_mode);
    UNUSED(instances);
    UNUSED(out_size);
    UNUSED(in_size);
    UNUSED(timeout);
    return w32_inheritable(create_named_pipe(name, open_mode), sa);
}

static HANDLE WINAPI CreateNamedPipeW(LPCWSTR name, DWORD open_mode, DWORD pipe_mode, DWORD instances, DWORD out_size,
                                      DWORD in_size, DWORD timeout, void *sa)
{
    char *n = w32_narrow(name);
    HANDLE h = CreateNamedPipeA(n, open_mode, pipe_mode, instances, out_size, in_size, timeout, sa);
    w32_free(n);
    return h;
}

static struct w32_npipe *npipe_find(const char *name, bool *exists)
{
    *exists = false;
    for (struct w32_npipe *np = npipes; np; np = np->next)
        if (!str_casecmp(np->name, name + 9)) {
            *exists = true;
            if (!np->connected)
                return np;
        }
    return NULL;
}

HANDLE w32_npipe_open(const char *name)
{
    bool exists;
    struct w32_npipe *np = npipe_find(name, &exists);
    if (!np) {
        w32_set_error(exists ? 231 : ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = w32_handle_new(WH_PIPE);
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return INVALID_HANDLE_VALUE;
    o->value = 2;
    o->pipe = np->out;
    o->pipe_out = np->in;
    np->out->readers++;
    np->in->writers++;
    np->connected = true;
    w32_kick_all();
    return h;
}

static struct w32_object *server_object(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_PIPE || !o->npipe) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return o;
}

#define STATUS_PENDING 0x103u
#define STATUS_CANCELLED 0xc0000120u
#define STATUS_PIPE_BROKEN 0xc000014bu

struct pending_io {
    struct w32_process *proc;
    struct w32_pipe *pipe;
    HANDLE handle;
    uint8_t *buf;
    DWORD n;
    uint32_t *ov;
    bool connect;
    struct pending_io *next;
};

static struct pending_io *pending;

void w32_io_complete(struct w32_object *o, uint32_t *ov, uint32_t status, uint32_t bytes, bool sync)
{
    w32_signal();
    ov[0] = status;
    ov[1] = bytes;
    HANDLE event = (HANDLE)(uintptr_t)(ov[4] & ~1u);
    if (event) {
        BOOL(WINAPI * set)(HANDLE) = (BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("kernel32.dll", "SetEvent");
        if (set)
            set(event);
    }
    if (o && o->io_port && !(ov[4] & 1) && !(sync && o->skip_port))
        w32_port_post(o->io_port, bytes, o->io_key, ov);
}

static void pipe_kick(struct w32_pipe *p)
{
    w32_signal_tick();
    for (struct pending_io *q = pending; q; q = q->next)
        if (q->pipe == p) {
            q->proc->io_gen++;
            q->proc->kicked = true;
        }
}

static void pending_add(HANDLE h, struct w32_pipe *pipe, uint8_t *buf, DWORD n, uint32_t *ov, bool connect)
{
    struct pending_io *p = kzalloc(sizeof(*p));
    if (!p)
        return;
    *p = (struct pending_io){w32, pipe, h, buf, n, ov, connect, NULL};
    struct pending_io **tail = &pending;
    while (*tail)
        tail = &(*tail)->next;
    *tail = p;
    ov[0] = STATUS_PENDING;
    ov[1] = 0;
}

void w32_io_poll(void)
{
    w32_sock_poll();
    if (!w32)
        return;
    if (w32->net_watch)
        w32_winsock_poll();
    w32->io_seen = w32->io_gen;
    for (struct pending_io **pp = &pending; *pp;) {
        struct pending_io *p = *pp;
        if (p->proc != w32) {
            pp = &p->next;
            continue;
        }
        struct w32_object *o = w32_handle_get(p->handle);
        uint32_t status = STATUS_PENDING, bytes = 0;
        if (!o || o->type != WH_PIPE) {
            status = STATUS_CANCELLED;
        } else if (p->connect) {
            if (o->npipe && o->npipe->connected)
                status = 0;
        } else if (o->pipe && w32_pipe_available(o->pipe)) {
            long got = w32_pipe_read(o->pipe, p->buf, p->n, false);
            status = 0;
            bytes = got > 0 ? (uint32_t)got : 0;
        } else if (!o->pipe || o->pipe->writers <= 0) {
            status = STATUS_PIPE_BROKEN;
        }
        if (status == STATUS_PENDING) {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        w32_io_complete(o && o->type == WH_PIPE ? o : NULL, p->ov, status, bytes, false);
        kfree(p);
    }
}

void w32_io_drop(HANDLE h)
{
    for (struct pending_io **pp = &pending; *pp;) {
        struct pending_io *p = *pp;
        if (p->proc != w32 || p->handle != h) {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        kfree(p);
    }
}

void w32_io_forget(struct w32_process *proc)
{
    for (struct pending_io **pp = &pending; *pp;) {
        struct pending_io *p = *pp;
        if (p->proc != proc) {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        kfree(p);
    }
}

bool w32_io_cancel(HANDLE h, const uint32_t *ov)
{
    bool found = false;
    for (struct pending_io **pp = &pending; *pp;) {
        struct pending_io *p = *pp;
        if (p->proc != w32 || p->handle != h || (ov && p->ov != ov)) {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        w32_io_complete(w32_handle_get(h), p->ov, STATUS_CANCELLED, 0, false);
        kfree(p);
        found = true;
    }
    return found;
}

int w32_pipe_overlapped(HANDLE h, struct w32_object *o, void *buf, DWORD n, DWORD *done, uint32_t *ov, bool write)
{
    if (done)
        *done = 0;
    if (write) {
        struct w32_pipe *p = o->value == 2 ? o->pipe_out : o->value == 1 ? o->pipe : NULL;
        long put = p ? w32_pipe_write(p, buf, n) : E_PIPE;
        if (put < 0) {
            ov[0] = STATUS_PIPE_BROKEN;
            ov[1] = 0;
            return ERROR_BROKEN_PIPE;
        }
        if (done)
            *done = (DWORD)put;
        w32_io_complete(o, ov, 0, (uint32_t)put, true);
        return 0;
    }
    if (o->value == 1 || !o->pipe)
        return ERROR_INVALID_HANDLE;
    if (w32_pipe_available(o->pipe)) {
        long got = w32_pipe_read(o->pipe, buf, n, false);
        if (done)
            *done = (DWORD)got;
        w32_io_complete(o, ov, 0, (uint32_t)got, true);
        return 0;
    }
    if (o->pipe->writers <= 0 && !(o->npipe && !o->npipe->connected))
        return ERROR_BROKEN_PIPE;
    pending_add(h, o->pipe, buf, n, ov, false);
    return 997;
}

static BOOL WINAPI ConnectNamedPipe(HANDLE h, void *overlapped)
{
    struct w32_object *o = server_object(h);
    if (!o)
        return FALSE;
    if (o->npipe->connected) {
        w32_set_error(535);
        return FALSE;
    }
    if (overlapped) {
        pending_add(h, NULL, NULL, 0, overlapped, true);
        w32_set_error(997);
        return FALSE;
    }
    while (!o->npipe->connected)
        w32_block();
    return TRUE;
}

static BOOL WINAPI DisconnectNamedPipe(HANDLE h)
{
    struct w32_object *o = server_object(h);
    if (!o)
        return FALSE;
    npipe_detach(o);
    if (!npipe_listen(o, o->npipe)) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI WaitNamedPipeA(LPCSTR name, DWORD ms)
{
    uint64_t deadline = hal_ticks_ms() + (ms == 0 ? 50 : ms == INFINITE ? ~0u : ms);
    for (;;) {
        bool exists;
        if (npipe_find(name, &exists))
            return TRUE;
        if (!exists || hal_ticks_ms() >= deadline) {
            w32_set_error(exists ? 121 : ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        w32_block();
    }
}

static BOOL WINAPI WaitNamedPipeW(LPCWSTR name, DWORD ms)
{
    char *n = w32_narrow(name);
    BOOL ok = n && WaitNamedPipeA(n, ms);
    w32_free(n);
    return ok;
}

static BOOL WINAPI SetNamedPipeHandleState(HANDLE h, DWORD *mode, DWORD *max, DWORD *timeout)
{
    UNUSED(mode);
    UNUSED(max);
    UNUSED(timeout);
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_PIPE) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

static const struct w32_export pipe_exports[] = {
    W32_FN(TransactNamedPipe),
    W32_FN(CreatePipe),
    W32_FN(PeekNamedPipe),
    W32_FN(CreateNamedPipeA),
    W32_FN(CreateNamedPipeW),
    W32_FN(ConnectNamedPipe),
    W32_FN(DisconnectNamedPipe),
    W32_FN(WaitNamedPipeA),
    W32_FN(WaitNamedPipeW),
    W32_FN(SetNamedPipeHandleState),
};

W32_EXPORTS(pipe_module, "kernel32.dll", pipe_exports);

void w32_pipe_dump(void)
{
    char line[96];
    for (struct pending_io *q = pending; q; q = q->next)
        if (q->pipe && w32_pipe_available(q->pipe)) {
            ksnprintf(line, sizeof(line), "pid %u: read on pipe handle %x not completed, %u bytes waiting\n",
                      q->proc->child ? q->proc->child->pid : 0, (unsigned)(uintptr_t)q->handle,
                      (unsigned)w32_pipe_available(q->pipe));
            w32_console_write(WH_STDERR, line, strlen(line));
        }
}
