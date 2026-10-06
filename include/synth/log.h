#ifndef SYNTH_LOG_H
#define SYNTH_LOG_H

#include <synth/types.h>

void klog(const char *fmt, ...) PRINTF_LIKE(1, 2);
size_t log_snapshot(char *buf, size_t cap);
NORETURN void panic(const char *fmt, ...) PRINTF_LIKE(1, 2);

#endif
