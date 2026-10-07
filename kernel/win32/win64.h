#ifndef SYNTH_WIN64_INTERNAL_H
#define SYNTH_WIN64_INTERNAL_H

#include "win32.h"

typedef uint64_t (*w64_handler)(uint64_t *a, const uint64_t *f, void *target);

enum {
    W64F_NATIVE = 1,
    W64F_DATA = 2,
    W64F_INTDATA = 4,
    W64F_FMT = 8,
    W64F_RET64 = 16,
    W64F_VA = 32,
    W64F_MATH = 64,
    W64F_FILE = 128,
    W64F_SPLIT0 = 1 << 16,
};

enum {
    M_D_D = 1,
    M_D_DD,
    M_D_DI,
    M_D_DP,
    M_F_F,
    M_F_FF,
    M_I_D,
    M_D_LL,
};

struct w64_export {
    const char *name;
    w64_handler fn;
    uint32_t flags;
    const void *data;
    const char *target;
};

struct w64_module {
    const char *name;
    const struct w64_export *exports;
    size_t count;
};

#define W64_FN(n, f) {n, f, 0, NULL, NULL}
#define W64_TO(n, t, fl) {n, NULL, fl, NULL, t}
#define W64_FLAG(n, fl) {n, NULL, fl, NULL, NULL}
#define W64_NATIVE(n, a) {n, NULL, W64F_NATIVE, (const void *)(a), NULL}
#define W64_DATA(n, g) {n, NULL, W64F_DATA, (const void *)(g), NULL}
#define W64_INTDATA(n) {n, NULL, W64F_INTDATA, NULL, NULL}
#define W64_VA(n, fmt, t) {n, NULL, W64F_VA | (fmt) << 8, NULL, t}
#define W64_MATH(n, kind) {n, NULL, W64F_MATH | (kind) << 8, NULL, NULL}
#define W64_EXPORTS(id, dll, table)                                                                               \
    static const struct w64_module id = {dll, table, ARRAY_SIZE(table)};                                          \
    SYNTH_REGISTER(synth_win64, struct w64_module, id)

static inline void *w64_ptr(uint64_t v)
{
    return (void *)(uintptr_t)v;
}

static inline uint64_t w64_sx(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

static inline uint64_t w64_ret(uint32_t v)
{
    return v >= 0xffff0000u ? w64_sx(v) : v;
}

#define T32(target, ...) w64_call32(target, (const uint32_t[]){__VA_ARGS__}, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4))
#define CALL(module, name, ...) T32(w32_builtin_lookup(module, name), __VA_ARGS__)

uint64_t w64_call32(void *fn, const uint32_t *args, int count);
uint64_t w64_math(void *target, int kind, const uint64_t *a, const uint64_t *f);
uint64_t w64_call(uint64_t fn, int argc, const uint64_t *args);
void *w64_kernel_stub(w64_handler fn, const char *name);
void *w64_target_stub(w64_handler fn, void *target);
bool w64_stub_info(uint64_t addr, w64_handler fn, void **target);
bool w64_is_code64(uint64_t addr);
void w64_seh_setup(void);

#endif
