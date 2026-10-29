#ifndef SYNTH_STREAM_H
#define SYNTH_STREAM_H

#include <synth/types.h>

enum {
    STREAM_CONSOLE = 1u << 0,
    STREAM_STATIC = 1u << 1,
};

struct stream;

struct stream_ops {
    K64(long (*read)(struct stream *s, void *buf, size_t n));
    K64(long (*write)(struct stream *s, const void *buf, size_t n));
    K64(void (*close)(struct stream *s));
};

struct stream {
    K64(const struct stream_ops *ops);
    K64(void *ctx);
    unsigned flags;
};

struct stream *stream_console(void);
struct stream *stream_null(void);
struct stream *stream_log(void);
struct stream *stream_buffer(void);
struct stream *stream_file(const char *path, unsigned flags, int *err);

long stream_read(struct stream *s, void *buf, size_t n);
long stream_write(struct stream *s, const void *buf, size_t n);
int stream_getc(struct stream *s);
long stream_getline(struct stream *s, char *buf, size_t cap);
void stream_puts(struct stream *s, const char *str);
void stream_putc(struct stream *s, char c);
int stream_printf(struct stream *s, const char *fmt, ...) PRINTF_LIKE(2, 3);
int stream_vprintf(struct stream *s, const char *fmt, va_list ap);
long stream_read_all(struct stream *s, char **out);
void stream_close(struct stream *s);
bool stream_is_console(const struct stream *s);

const char *stream_buffer_data(struct stream *s, size_t *len);
void stream_buffer_rewind(struct stream *s);

#endif
