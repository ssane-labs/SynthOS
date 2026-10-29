#include <stdarg.h>
#include <stdio.h>

#include "gfx.h"

static HANDLE heap;

void *gfx_alloc(size_t n)
{
    if (!heap)
        heap = GetProcessHeap();
    return HeapAlloc(heap, HEAP_ZERO_MEMORY, n ? n : 1);
}

void *gfx_realloc(void *p, size_t n)
{
    if (!p)
        return gfx_alloc(n);
    return HeapReAlloc(heap, HEAP_ZERO_MEMORY, p, n ? n : 1);
}

void gfx_free(void *p)
{
    if (p)
        HeapFree(heap, 0, p);
}

void gfx_log(const char *fmt, ...)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = GetEnvironmentVariableA("SYNTH_GFX_LOG", NULL, 0) > 0;
    if (!enabled)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    n = GFX_MIN(n, (int)sizeof(buf) - 2);
    buf[n++] = '\n';
    DWORD done;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)n, &done, NULL);
}

static inline uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float bitsf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

float gfx_trunc(float x)
{
    if (!(gfx_abs(x) < 8388608.0f))
        return x;
    return (float)(int32_t)x;
}

float gfx_floor(float x)
{
    float t = gfx_trunc(x);
    return t > x ? t - 1.0f : t;
}

float gfx_ceil(float x)
{
    float t = gfx_trunc(x);
    return t < x ? t + 1.0f : t;
}

float gfx_round_even(float x)
{
    if (!(gfx_abs(x) < 8388608.0f))
        return x;
    float f = gfx_floor(x), d = x - f;
    if (d > 0.5f || (d == 0.5f && ((int32_t)f & 1)))
        f += 1.0f;
    return f;
}

float gfx_exp2(float x)
{
    if (x != x)
        return x;
    if (x >= 128.0f)
        return bitsf(0x7f800000);
    if (x <= -150.0f)
        return 0.0f;
    float n = gfx_floor(x), f = x - n;
    /* 2^f on [0,1) */
    float p = 1.535336188319500e-4f;
    p = p * f + 1.339887440266574e-3f;
    p = p * f + 9.618437357674640e-3f;
    p = p * f + 5.550332471162809e-2f;
    p = p * f + 2.402264791363012e-1f;
    p = p * f + 6.931472028550421e-1f;
    p = p * f + 1.0f;
    int e = (int)n;
    if (e < -126) {
        p *= bitsf((uint32_t)(e + 126 + 127) << 23);
        return p * bitsf(1u << 23);
    }
    return p * bitsf((uint32_t)(e + 127) << 23);
}

float gfx_log2(float x)
{
    if (x != x || x < 0.0f)
        return bitsf(0x7fc00000);
    if (x == 0.0f)
        return bitsf(0xff800000);
    if (x == bitsf(0x7f800000))
        return x;
    uint32_t u = fbits(x);
    int e = (int)((u >> 23) & 0xff);
    if (!e) {
        x *= 8388608.0f;
        u = fbits(x);
        e = (int)((u >> 23) & 0xff) - 23;
    }
    e -= 127;
    float m = bitsf((u & 0x7fffff) | 0x3f800000);
    if (m > 1.41421356f) {
        m *= 0.5f;
        e++;
    }
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    float s = t * (2.0f + t2 * (0.666666667f + t2 * (0.4f + t2 * (0.285714286f + t2 * 0.222222222f))));
    return (float)e + s * 1.442695041f;
}

float gfx_pow(float x, float y)
{
    if (y == 0.0f)
        return 1.0f;
    if (x == 0.0f)
        return y > 0.0f ? 0.0f : bitsf(0x7f800000);
    if (x < 0.0f) {
        float iy = gfx_trunc(y);
        if (iy != y)
            return bitsf(0x7fc00000);
        float r = gfx_exp2(y * gfx_log2(-x));
        return ((int64_t)iy & 1) ? -r : r;
    }
    return gfx_exp2(y * gfx_log2(x));
}

static float sin_poly(float r)
{
    float r2 = r * r;
    return r * (1.0f + r2 * (-1.6666654611e-1f + r2 * (8.3321608736e-3f + r2 * -1.9515295891e-4f)));
}

static float cos_poly(float r)
{
    float r2 = r * r;
    return 1.0f + r2 * (-0.5f + r2 * (4.166664568298827e-2f + r2 * (-1.388731625493765e-3f + r2 * 2.443315711809948e-5f)));
}

static float reduce(float x, int *quadrant)
{
    double k = (double)gfx_round_even((float)(x * 0.63661977236758134));
    double r = (double)x - k * 1.5707963267948966;
    *quadrant = (int)((int64_t)k & 3);
    return (float)r;
}

float gfx_sin(float x)
{
    if (!(gfx_abs(x) < 1e7f))
        return x != x || gfx_abs(x) == bitsf(0x7f800000) ? bitsf(0x7fc00000) : 0.0f;
    int q;
    float r = reduce(x, &q);
    switch (q) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
    }
}

float gfx_cos(float x)
{
    if (!(gfx_abs(x) < 1e7f))
        return x != x || gfx_abs(x) == bitsf(0x7f800000) ? bitsf(0x7fc00000) : 1.0f;
    int q;
    float r = reduce(x, &q);
    switch (q) {
    case 0: return cos_poly(r);
    case 1: return -sin_poly(r);
    case 2: return -cos_poly(r);
    default: return sin_poly(r);
    }
}

float gfx_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (e == 0) {
        if (!m)
            return bitsf(sign);
        float f = (float)m * (1.0f / 16777216.0f);
        return sign ? -f : f;
    }
    if (e == 31)
        return bitsf(sign | 0x7f800000 | (m << 13));
    return bitsf(sign | ((e + 112) << 23) | (m << 13));
}

uint16_t gfx_float_to_half(float f)
{
    uint32_t u = fbits(f), sign = (u >> 16) & 0x8000;
    int e = (int)((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffff;
    if (((u >> 23) & 0xff) == 0xff)
        return (uint16_t)(sign | 0x7c00 | (m ? 0x200 : 0));
    if (e >= 31)
        return (uint16_t)(sign | 0x7c00);
    if (e <= 0) {
        if (e < -10)
            return (uint16_t)sign;
        m |= 0x800000;
        uint32_t shift = (uint32_t)(14 - e);
        uint32_t v = m >> shift, rest = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (v & 1)))
            v++;
        return (uint16_t)(sign | v);
    }
    uint32_t v = ((uint32_t)e << 10) | (m >> 13), rest = m & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (v & 1)))
        v++;
    return (uint16_t)(sign | v);
}

float gfx_srgb_to_linear(float c)
{
    if (c <= 0.04045f)
        return c * (1.0f / 12.92f);
    return gfx_pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

float gfx_linear_to_srgb(float c)
{
    if (!(c > 0.0031308f))
        return c > 0.0f ? c * 12.92f : 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    return 1.055f * gfx_pow(c, 1.0f / 2.4f) - 0.055f;
}
