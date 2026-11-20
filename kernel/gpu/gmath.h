#ifndef SYNTH_GPU_GMATH_H
#define SYNTH_GPU_GMATH_H

/* Float helpers for the GPU code (kernel/gpu), which has no C library: bit casts, sqrt, floor, exp2, log2, sin,
   cos, rounding, conversions and half floats. Static, so each _fp file gets its own SSE copy. */

#include <synth/types.h>

typedef union {
    uint32_t u;
    int32_t i;
    float f;
} word;

static inline __attribute__((unused)) float F(uint32_t u)
{
    word w = {.u = u};
    return w.f;
}

static inline __attribute__((unused)) uint32_t U(float f)
{
    word w = {.f = f};
    return w.u;
}

static inline __attribute__((unused)) float fsqrt(float x)
{
#if defined(__SSE__) || defined(__x86_64__)
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
#else
    if (x <= 0)
        return x == 0 ? 0 : F(0x7fc00000u);
    float r = x > 1 ? x : 1;
    for (int i = 0; i < 30; i++)
        r = 0.5f * (r + x / r);
    return r;
#endif
}

static inline __attribute__((unused)) float ffloor(float x)
{
    if (!(x > -8388608.0f && x < 8388608.0f)) /* large, infinite or NaN: already whole */
        return x;
    float t = (float)(int32_t)x;
    return t > x ? t - 1 : t;
}

static inline __attribute__((unused)) float fexp2(float x)
{
    if (x != x)
        return x;
    if (x > 128)
        return F(0x7f800000u);
    if (x < -150)
        return 0;
    float fl = ffloor(x), f = x - fl;
    /* 2^f on [0, 1): a polynomial good to a few ulp */
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015403f)))));
    int e = (int)fl;
    if (e < -126) { /* below the normal range */
        p *= F((uint32_t)(e + 126 + 127) << 23);
        e = -126;
    }
    return p * F((uint32_t)(e + 127) << 23);
}

static inline __attribute__((unused)) float flog2(float x)
{
    if (x != x || x < 0)
        return F(0x7fc00000u);
    if (x == 0)
        return F(0xff800000u);
    if (x == F(0x7f800000u))
        return x;
    uint32_t u = U(x);
    int e = (int)((u >> 23) & 255);
    if (!e) { /* denormal */
        x *= 8388608.0f;
        u = U(x);
        e = (int)((u >> 23) & 255) - 23;
    }
    e -= 127;
    float m = F((u & 0x7fffffu) | 0x3f800000u); /* 1..2 */
    if (m > 1.41421356f)
        m *= 0.5f, e++;
    float t = (m - 1) / (m + 1), t2 = t * t;
    float ln = 2 * t * (1 + t2 * (0.33333333f + t2 * (0.2f + t2 * (0.14285714f + t2 * 0.11111111f))));
    return (float)e + ln * 1.44269504f;
}

static inline __attribute__((unused)) float fsin(float x)
{
    if (x != x || x == F(0x7f800000u) || x == F(0xff800000u))
        return F(0x7fc00000u);
    float k = ffloor(x * 0.15915494f + 0.5f); /* to -pi..pi */
    x -= k * 6.28318531f;
    if (x > 1.57079633f)
        x = 3.14159265f - x;
    else if (x < -1.57079633f)
        x = -3.14159265f - x;
    float x2 = x * x;
    return x * (1 - x2 * (0.16666667f - x2 * (0.00833333f - x2 * (0.00019841f - x2 * 0.0000027526f))));
}

static inline __attribute__((unused)) float fcos(float x)
{
    return fsin(x + 1.57079633f);
}

static inline __attribute__((unused)) float round_even(float x)
{
    float f = ffloor(x), d = x - f;
    if (d > 0.5f)
        return f + 1;
    if (d < 0.5f)
        return f;
    return ((int64_t)f & 1) ? f + 1 : f;
}

static inline __attribute__((unused)) int32_t to_int(float f)
{
    if (f != f)
        return 0;
    if (f >= 2147483647.0f)
        return 0x7fffffff;
    if (f <= -2147483648.0f)
        return (int32_t)0x80000000u;
    return (int32_t)f;
}

static inline __attribute__((unused)) uint32_t to_uint(float f)
{
    if (f != f || f <= 0)
        return 0;
    if (f >= 4294967295.0f)
        return 0xffffffffu;
    return (uint32_t)f;
}

static inline __attribute__((unused)) uint32_t f32_to_f16(float f)
{
    uint32_t u = U(f), s = (u >> 16) & 0x8000u;
    int e = (int)((u >> 23) & 255) - 127 + 15;
    uint32_t m = u & 0x7fffffu;
    if (((u >> 23) & 255) == 255)
        return s | 0x7c00u | (m ? 0x200u : 0);
    if (e >= 31)
        return s | 0x7c00u;
    if (e <= 0) {
        if (e < -10)
            return s;
        m |= 0x800000u;
        return s | (m >> (14 - e));
    }
    return s | (uint32_t)e << 10 | (m >> 13);
}

static inline __attribute__((unused)) float f16_to_f32(uint32_t h)
{
    uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3ffu;
    if (e == 31)
        return F(s | 0x7f800000u | m << 13);
    if (!e) {
        if (!m)
            return F(s);
        float v = (float)m / 16777216.0f;
        return s ? -v : v;
    }
    return F(s | (e + 112) << 23 | m << 13);
}

static inline __attribute__((unused)) float fpow(float x, float y)
{
    if (x <= 0)
        return x == 0 && y > 0 ? 0 : x == 0 ? F(0x7f800000u) : F(0x7fc00000u);
    return fexp2(y * flog2(x));
}

static inline __attribute__((unused)) float fclamp(float v, float lo, float hi)
{
    return v != v ? lo : v < lo ? lo : v > hi ? hi : v;
}

#endif
