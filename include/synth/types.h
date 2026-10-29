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

/* The 64-bit kernel's Windows subsystem is x32 code (64-bit mode, 4-byte pointers: the layout 32-bit programs
   expect). Kernel structures it shares with the rest of the kernel keep the 64-bit layout: K64(...) pads a pointer,
   size_t or long member to 8 bytes there, and registration tables hold 8-byte entries. */
#if defined(__x86_64__) && defined(__ILP32__)
#define SYNTH_X32 1
#define K64_CAT_(a, b) a##b
#define K64_CAT(a, b) K64_CAT_(a, b)
#define K64(...)                                                                    \
    union {                                                                         \
        __VA_ARGS__;                                                                \
        uint64_t K64_CAT(k64_pad_, __COUNTER__);                                    \
    }
/* an array of kernel pointers seen from x32 code */
#define KARR(T) uint64_t
#define SYNTH_REGISTER(table, type, object)                                         \
    static const union {                                                            \
        const type *p;                                                              \
        uint64_t pad;                                                               \
    } table##_entry_##object __attribute__((used, section(#table), aligned(8))) = {&object}

#define SYNTH_TABLE_DECLARE(table, type)                                            \
    extern const uint64_t __start_##table[];                                        \
    extern const uint64_t __stop_##table[]

#define SYNTH_TABLE_FOREACH(table, type, it)                                        \
    for (const type *const *it##_p = (const type *const *)__start_##table, *it = NULL; \
         (const uint64_t *)it##_p < __stop_##table &&                               \
         ((it = (const type *)(uintptr_t) * (const uint64_t *)it##_p), 1);          \
         it##_p = (const type *const *)((const uint64_t *)it##_p + 1))
#else
#define K64(...) __VA_ARGS__
#define KARR(T) T
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

#endif
