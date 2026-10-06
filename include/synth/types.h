#ifndef SYNTH_TYPES_H
#define SYNTH_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))
#define UNUSED(x) ((void)(x))

#define NORETURN __attribute__((noreturn))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#define SYNTH_REGISTER(table, type, object)                                         \
    static const type *const table##_entry_##object                                 \
        __attribute__((used, section(#table), aligned(sizeof(void *)))) = &object

#define SYNTH_TABLE_DECLARE(table, type)                                            \
    extern const type *const __start_##table[];                                     \
    extern const type *const __stop_##table[]

#define SYNTH_TABLE_FOREACH(table, type, it)                                        \
    for (const type *const *it##_p = __start_##table, *it = NULL;                   \
         it##_p < __stop_##table && ((it = *it##_p), 1); it##_p++)

#endif
