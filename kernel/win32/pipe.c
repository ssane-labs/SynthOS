#include <synth/error.h>
#include <synth/heap.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

struct w32_pipe {
    uint8_t *buf;
    size_t len, cap, rpos;
    int readers, writers;
};

static void pipe_drop(struct w32_pipe *p)
{
    if (p->readers <= 0 && p->writers <= 0) {
        kfree(p->buf);
        kfree(p);
    }
}

void w32_pipe_release(struct w32_object *o)
{
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
    return (long)n;
}

long w32_pipe_read(struct w32_pipe *p, void *out, size_t n, bool wait)
{
    while (p->rpos == p->len && p->writers > 0 && wait && w32)
        w32_block();
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
    UNUSED(sa);
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
    *rd = r;
    *wr = w;
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

static long stream_pipe_write(struct stream *s, const void *buf, size_t n)
{
    long r = w32_pipe_write(s->ctx, buf, n);
    return r < 0 ? (long)n : r;
}

static long stream_pipe_read(struct stream *s, void *buf, size_t n)
{
    return w32_pipe_read(s->ctx, buf, n, false);
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

static const struct stream_ops pipe_stream_ops = {stream_pipe_read, stream_pipe_write, NULL};
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
    s->ctx = ops == &pipe_stream_ops ? (void *)o->pipe : (void *)o;
    return s;
}

static const struct w32_export pipe_exports[] = {
    W32_FN(CreatePipe),
    W32_FN(PeekNamedPipe),
};

W32_EXPORTS(pipe_module, "kernel32.dll", pipe_exports);
