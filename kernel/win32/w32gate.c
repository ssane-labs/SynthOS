#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "win32.h"

#ifdef __x86_64__

/* The 64-bit kernel's call gate for 32-bit programs. Every built-in function a program can reach has a 10-byte
   stub (push its index; jmp w32_gate32); the gate switches to 64-bit mode and w32_gate_dispatch moves the
   arguments from the program's stack into System V registers as the signature (tools/w32sig.py) says, calls the
   function and hands the result back in eax, edx:eax or st0. */

enum { K_ZX32 = 1, K_I64, K_F32, K_F64, K_SX8, K_ZX8, K_SX16, K_ZX16, K_MEM, K_PIECE, K_PIECE_F64, K_F80, K_VARARGS };
enum { R_VOID, R_I32, R_I64, R_F32, R_F64, R_F80, R_SRET, R_PAIR };

struct gate_ret {
    uint64_t rax, rdx;
    uint8_t fp[16];
    uint8_t kind;
};

extern const struct w32_sig __start_w32sigs[], __stop_w32sigs[];
extern char __start_text32[], __stop_text32[], __text_start[], w32_gate32[];
uint32_t crt_cxx_frame_gate;

void w32_native_call(const void *fn, const uint64_t ints[6], const uint64_t sse[8], const uint64_t *stack,
                     size_t words, struct gate_ret *out);
uint32_t w32_gate_dispatch(uint32_t index, uint32_t args, struct gate_ret *r);

static const struct w32_sig **sigs;
static size_t sig_count;
static uint8_t *stubs;

#define STUB 10

bool w32_native_code(const void *fn)
{
    return (const char *)fn >= __start_text32 && (const char *)fn < __stop_text32;
}

static void sort_by_address(const struct w32_sig **a, size_t n)
{
    for (size_t gap = n / 2; gap; gap /= 2)
        for (size_t i = gap; i < n; i++) {
            const struct w32_sig *v = a[i];
            size_t j = i;
            for (; j >= gap && (const char *)a[j - gap]->fn > (const char *)v->fn; j -= gap)
                a[j] = a[j - gap];
            a[j] = v;
        }
}

void w32_gate_init(void)
{
    if (sigs)
        return;
    size_t n = (size_t)(__stop_w32sigs - __start_w32sigs);
    sigs = kmalloc(sizeof(*sigs) * (n ? n : 1));
    stubs = kmalloc_pages(ALIGN_UP(n * STUB, 4096u));
    if (!sigs || !stubs)
        panic("win32: no memory for the call gate");
    for (size_t i = 0; i < n; i++)
        sigs[i] = &__start_w32sigs[i];
    sort_by_address(sigs, n);
    /* the same function may be described by several sources (a shared header); keep one */
    size_t out = 0;
    for (size_t i = 0; i < n; i++)
        if (!out || sigs[out - 1]->fn != sigs[i]->fn)
            sigs[out++] = sigs[i];
    sig_count = out;
    for (size_t i = 0; i < sig_count; i++) {
        uint8_t *s = stubs + i * STUB;
        uint32_t index = (uint32_t)i;
        int32_t rel = (int32_t)((uintptr_t)w32_gate32 - (uintptr_t)(s + STUB));
        s[0] = 0x68;
        memcpy(s + 1, &index, 4);
        s[5] = 0xe9;
        memcpy(s + 6, &rel, 4);
    }
    extern int cxx_frame_handler();
    extern void w32_math_gates(void);
    crt_cxx_frame_gate = (uint32_t)(uintptr_t)w32_guest_addr((const void *)cxx_frame_handler);
    w32_math_gates();
    klog("win32: call gate for %u functions", (unsigned)sig_count);
}

static const struct w32_sig *sig_of(const void *fn, size_t *index)
{
    size_t lo = 0, hi = sig_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (sigs[mid]->fn == fn) {
            *index = mid;
            return sigs[mid];
        }
        if ((const char *)sigs[mid]->fn < (const char *)fn)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

void *w32_guest_addr(const void *fn)
{
    size_t i;
    /* program code, 32-bit stubs and data stay as they are */
    if (!fn || w32_native_code(fn) || (const char *)fn < __text_start || (const char *)fn >= __start_text32)
        return (void *)fn;
    if (!sigs)
        w32_gate_init();
    if (sig_of(fn, &i))
        return stubs + i * STUB;
    klog("win32: %p has no call gate entry", fn);
    return NULL;
}

/* a pointer a program may hold for a built-in function, back to the function (for the kernel's own calls) */
const void *w32_gate_target(uint32_t addr)
{
    uintptr_t a = addr;
    if (stubs && a >= (uintptr_t)stubs && a < (uintptr_t)stubs + sig_count * STUB && !((a - (uintptr_t)stubs) % STUB))
        return sigs[(a - (uintptr_t)stubs) / STUB]->fn;
    return NULL;
}

uint32_t w32_gate_dispatch(uint32_t index, uint32_t args, struct gate_ret *r)
{
    const struct w32_sig *s = sigs[index];
    const uint8_t *g = (const uint8_t *)(uintptr_t)args;
    uint64_t ints[6] = {0}, sse[8] = {0}, stack[64];
    int ni = 0, nx = 0;
    size_t ns = 0;
    for (int k = 0; k < s->nargs; k++) {
        uint8_t kind = (uint8_t)s->args[k * 3];
        const uint8_t *p = g + (uint8_t)s->args[k * 3 + 1] * 4u;
        uint32_t words = (uint8_t)s->args[k * 3 + 2];
        uint64_t v = 0;
        bool fp = false;
        switch (kind) {
        case K_ZX32: v = *(const uint32_t *)p; break;
        case K_I64: memcpy(&v, p, 8); break;
        case K_F32: v = *(const uint32_t *)p; fp = true; break;
        case K_F64: memcpy(&v, p, 8); fp = true; break;
        case K_SX8: v = (uint64_t)(int64_t)(int8_t)p[0]; break;
        case K_ZX8: v = p[0]; break;
        case K_SX16: v = (uint64_t)(int64_t)*(const int16_t *)p; break;
        case K_ZX16: v = *(const uint16_t *)p; break;
        case K_PIECE: memcpy(&v, p, words * 4u); break;
        case K_PIECE_F64: memcpy(&v, p, 8); fp = true; break;
        case K_VARARGS: v = (uint32_t)(uintptr_t)p; break;
        case K_MEM:
        case K_F80: {
            size_t bytes = kind == K_F80 ? 16 : words * 4u, n = (bytes + 7) / 8;
            if (ns + n > ARRAY_SIZE(stack))
                panic("win32: call gate stack overflow");
            memset(&stack[ns], 0, n * 8);
            memcpy(&stack[ns], p, kind == K_F80 ? 10 : bytes);
            ns += n;
            continue;
        }
        default: panic("win32: bad call gate signature");
        }
        if (fp && nx < 8)
            sse[nx++] = v;
        else if (!fp && ni < 6)
            ints[ni++] = v;
        else if (ns < ARRAY_SIZE(stack))
            stack[ns++] = v;
    }
    memset(r, 0, sizeof(*r));
    r->kind = s->ret == R_F80 ? 2 : 0;
    w32_native_call(s->fn, ints, sse, stack, ns, r);
    switch (s->ret) {
    case R_VOID: r->kind = 0; break;
    case R_I32:
    case R_SRET: r->kind = 0; break;
    case R_I64:
        r->rdx = r->rax >> 32;
        r->kind = 0;
        break;
    case R_F32: r->kind = 3; break;
    case R_F64: r->kind = 1; break;
    case R_F80: r->kind = 2; break;
    case R_PAIR: {
        uint32_t out = *(const uint32_t *)g;
        memcpy((void *)(uintptr_t)out, &r->rax, 8);
        memcpy((void *)(uintptr_t)out + 8, &r->rdx, 8);
        r->rax = out;
        r->kind = 0;
        break;
    }
    }
    return s->pop;
}

/* calls from the kernel into a program's (or a stub's) code with 32-bit arguments */
uint32_t w32_call(void *fn, int argc, const uint32_t *args)
{
    if (!w32->jump.active)
        return arch_w32_call(fn, argc, args, w32->stack + w32->stack_size - (w32->win64 ? 4096 : 16), &w32->jump);
    return (uint32_t)arch_w32_enter((uint32_t)(uintptr_t)fn, argc, args, 0, 0, 0);
}

uint32_t arch_call_funclet(void *fn, uint32_t ebp)
{
    return (uint32_t)arch_w32_enter((uint32_t)(uintptr_t)fn, 0, NULL, 0, ebp, 0);
}

void arch_thiscall(void *fn, void *self, int argc, uint32_t a, uint32_t b)
{
    uint32_t args[2] = {a, b};
    arch_w32_enter((uint32_t)(uintptr_t)fn, argc, args, 0, 0, (uint32_t)(uintptr_t)self);
}

#endif
