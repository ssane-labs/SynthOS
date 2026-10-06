#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/lineedit.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/vfs.h>

struct console_ctx {
    char line[CONFIG_LINE_MAX];
    size_t len;
    size_t pos;
    bool eof;
};

struct buffer_ctx {
    char *data;
    size_t len;
    size_t cap;
    size_t pos;
};

struct file_ctx {
    struct file *file;
    char buf[512];
    size_t len;
    size_t pos;
    char out[1024];
    size_t out_len;
};

static long file_flush(struct file_ctx *f)
{
    long n = 0;
    if (f->out_len) {
        n = vfs_write(f->file, f->out, f->out_len);
        f->out_len = 0;
    }
    return n < 0 ? n : 0;
}

static long console_read(struct stream *s, void *buf, size_t n)
{
    struct console_ctx *c = s->ctx;
    if (c->pos >= c->len) {
        if (c->eof) {
            c->eof = false;
            return 0;
        }
        int len = lineedit_simple(c->line, sizeof(c->line) - 1);
        if (len < 0)
            return 0;
        c->line[len++] = '\n';
        c->len = (size_t)len;
        c->pos = 0;
    }
    size_t k = MIN(n, c->len - c->pos);
    memcpy(buf, c->line + c->pos, k);
    c->pos += k;
    return (long)k;
}

static long console_write(struct stream *s, const void *buf, size_t n)
{
    UNUSED(s);
    con_write(buf, n);
    return (long)n;
}

static const struct stream_ops console_ops = {console_read, console_write, NULL};
static struct console_ctx console_state;
static struct stream console_stream = {&console_ops, &console_state, STREAM_CONSOLE | STREAM_STATIC};

struct stream *stream_console(void)
{
    return &console_stream;
}

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

static const struct stream_ops null_ops = {null_read, null_write, NULL};
static struct stream null_stream = {&null_ops, NULL, STREAM_STATIC};

struct stream *stream_null(void)
{
    return &null_stream;
}

static long buffer_read(struct stream *s, void *buf, size_t n)
{
    struct buffer_ctx *b = s->ctx;
    size_t k = MIN(n, b->len - b->pos);
    memcpy(buf, b->data + b->pos, k);
    b->pos += k;
    return (long)k;
}

static long buffer_write(struct stream *s, const void *buf, size_t n)
{
    struct buffer_ctx *b = s->ctx;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *data = krealloc(b->data, cap);
        if (!data)
            return E_NOMEM;
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, buf, n);
    b->len += n;
    b->data[b->len] = 0;
    return (long)n;
}

static void buffer_close(struct stream *s)
{
    struct buffer_ctx *b = s->ctx;
    kfree(b->data);
    kfree(b);
}

static const struct stream_ops buffer_ops = {buffer_read, buffer_write, buffer_close};

struct stream *stream_buffer(void)
{
    struct stream *s = kzalloc(sizeof(*s));
    struct buffer_ctx *b = kzalloc(sizeof(*b));
    if (!s || !b) {
        kfree(s);
        kfree(b);
        return NULL;
    }
    s->ops = &buffer_ops;
    s->ctx = b;
    return s;
}

const char *stream_buffer_data(struct stream *s, size_t *len)
{
    struct buffer_ctx *b = s->ctx;
    if (len)
        *len = b->len;
    return b->data ? b->data : "";
}

void stream_buffer_rewind(struct stream *s)
{
    ((struct buffer_ctx *)s->ctx)->pos = 0;
}

static long file_read(struct stream *s, void *buf, size_t n)
{
    struct file_ctx *f = s->ctx;
    file_flush(f);
    if (f->pos >= f->len) {
        if (n >= sizeof(f->buf))
            return vfs_read(f->file, buf, n);
        long got = vfs_read(f->file, f->buf, sizeof(f->buf));
        if (got <= 0)
            return got;
        f->len = (size_t)got;
        f->pos = 0;
    }
    size_t k = MIN(n, f->len - f->pos);
    memcpy(buf, f->buf + f->pos, k);
    f->pos += k;
    return (long)k;
}

static long file_write(struct stream *s, const void *buf, size_t n)
{
    struct file_ctx *f = s->ctx;
    if (f->out_len + n > sizeof(f->out)) {
        long err = file_flush(f);
        if (err < 0)
            return err;
        if (n >= sizeof(f->out))
            return vfs_write(f->file, buf, n);
    }
    memcpy(f->out + f->out_len, buf, n);
    f->out_len += n;
    return (long)n;
}

static void file_close(struct stream *s)
{
    struct file_ctx *f = s->ctx;
    file_flush(f);
    vfs_close(f->file);
    kfree(f);
}

static const struct stream_ops file_ops = {file_read, file_write, file_close};

struct stream *stream_file(const char *path, unsigned flags, int *err)
{
    struct file *file;
    int e = vfs_open(path, flags, &file);
    if (e) {
        if (err)
            *err = e;
        return NULL;
    }
    struct stream *s = kzalloc(sizeof(*s));
    struct file_ctx *f = kzalloc(sizeof(*f));
    if (!s || !f) {
        kfree(s);
        kfree(f);
        vfs_close(file);
        if (err)
            *err = E_NOMEM;
        return NULL;
    }
    f->file = file;
    s->ops = &file_ops;
    s->ctx = f;
    return s;
}

long stream_read(struct stream *s, void *buf, size_t n)
{
    return s->ops->read ? s->ops->read(s, buf, n) : E_BADF;
}

long stream_write(struct stream *s, const void *buf, size_t n)
{
    return s->ops->write ? s->ops->write(s, buf, n) : E_BADF;
}

int stream_getc(struct stream *s)
{
    uint8_t c;
    return stream_read(s, &c, 1) == 1 ? c : -1;
}

long stream_getline(struct stream *s, char *buf, size_t cap)
{
    size_t len = 0;
    int c;
    while ((c = stream_getc(s)) >= 0) {
        if (c == '\n')
            break;
        if (c != '\r' && len + 1 < cap)
            buf[len++] = (char)c;
    }
    buf[len] = 0;
    return c < 0 && len == 0 ? -1 : (long)len;
}

void stream_puts(struct stream *s, const char *str)
{
    stream_write(s, str, strlen(str));
}

void stream_putc(struct stream *s, char c)
{
    stream_write(s, &c, 1);
}

static void stream_sink(void *ctx, const char *s, size_t n)
{
    stream_write(ctx, s, n);
}

int stream_vprintf(struct stream *s, const char *fmt, va_list ap)
{
    return fmt_format(stream_sink, s, fmt, ap);
}

int stream_printf(struct stream *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = stream_vprintf(s, fmt, ap);
    va_end(ap);
    return n;
}

long stream_read_all(struct stream *s, char **out)
{
    struct stream *b = stream_buffer();
    if (!b)
        return E_NOMEM;
    char chunk[512];
    long n;
    while ((n = stream_read(s, chunk, sizeof(chunk))) > 0)
        stream_write(b, chunk, (size_t)n);
    size_t len;
    const char *data = stream_buffer_data(b, &len);
    *out = kmalloc(len + 1);
    if (*out) {
        memcpy(*out, data, len);
        (*out)[len] = 0;
    }
    stream_close(b);
    return *out ? (long)len : E_NOMEM;
}

void stream_close(struct stream *s)
{
    if (!s || (s->flags & STREAM_STATIC))
        return;
    if (s->ops->close)
        s->ops->close(s);
    kfree(s);
}

bool stream_is_console(const struct stream *s)
{
    return s && (s->flags & STREAM_CONSOLE);
}
