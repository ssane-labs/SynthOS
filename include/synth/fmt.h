#ifndef SYNTH_FMT_H
#define SYNTH_FMT_H

#include <synth/types.h>

typedef void (*fmt_sink_fn)(void *ctx, const char *s, size_t n);

int fmt_format(fmt_sink_fn sink, void *ctx, const char *fmt, va_list ap);
int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t cap, const char *fmt, ...) PRINTF_LIKE(3, 4);
void fmt_size(char *buf, size_t cap, uint64_t bytes);

#endif
