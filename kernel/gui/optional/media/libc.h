/* The little of the C library the media decoders use, on top of the kernel's heap and string functions.
   Only the media module (a *_fp.c file, so floating point is allowed) includes this. */
#ifndef SYNTH_MEDIA_LIBC_H
#define SYNTH_MEDIA_LIBC_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <synth/heap.h>
#include <synth/string.h>

/* object-like names, so struct members called free or malloc (simplewebp's allocator) still work */
static inline void *media_malloc(size_t n)
{
    return kmalloc(n);
}
static inline void media_free(void *p)
{
    kfree(p);
}
static inline void *media_realloc(void *p, size_t n)
{
    return krealloc(p, n);
}
#define malloc media_malloc
#define free media_free
#define realloc media_realloc
#undef assert
#define assert(x) ((void)0)
/* stb_vorbis is always given an arena, so its alloca branch is never taken */
#define alloca(n) ((void *)0)

static inline int media_abs(int x)
{
    return x < 0 ? -x : x;
}
#define abs(x) media_abs(x)

/* qsort as a heap sort: no recursion, no extra memory */
static inline void media_swap(char *a, char *b, size_t size)
{
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static inline void media_sift(char *base, size_t root, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (cmp(base + root * size, base + child * size) >= 0)
            return;
        media_swap(base + root * size, base + child * size, size);
        root = child;
    }
}

static inline void media_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    if (n < 2)
        return;
    for (size_t i = n / 2; i-- > 0;)
        media_sift(b, i, n, size, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        media_swap(b, b + end * size, size);
        media_sift(b, 0, end, size, cmp);
    }
}
#define qsort(b, n, s, c) media_qsort(b, n, s, c)

/* ---- math, for the decoders' tables (double precision is plenty) ---- */

static inline double media_floor(double x)
{
    if (x >= 9.0e15 || x <= -9.0e15)
        return x;
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}

static inline double media_ldexp(double x, int e)
{
    while (e > 0) {
        int k = e > 60 ? 60 : e;
        x *= (double)(1ull << k);
        e -= k;
    }
    while (e < 0) {
        int k = -e > 60 ? 60 : -e;
        x /= (double)(1ull << k);
        e += k;
    }
    return x;
}

#define MEDIA_PI 3.14159265358979323846

/* sine of x in [-pi/4, pi/4] and cosine likewise, by their series */
static inline double media_sin_small(double x)
{
    double x2 = x * x, term = x, sum = x;
    for (int k = 1; k < 12; k++) {
        term *= -x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

static inline double media_cos_small(double x)
{
    double x2 = x * x, term = 1.0, sum = 1.0;
    for (int k = 1; k < 12; k++) {
        term *= -x2 / ((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}

static inline double media_sin(double x)
{
    double q = media_floor(x / (MEDIA_PI / 2) + 0.5);
    double r = x - q * (MEDIA_PI / 2);
    switch ((long long)q & 3) {
    case 0:
        return media_sin_small(r);
    case 1:
        return media_cos_small(r);
    case 2:
        return -media_sin_small(r);
    default:
        return -media_cos_small(r);
    }
}

static inline double media_cos(double x)
{
    return media_sin(x + MEDIA_PI / 2);
}

static inline double media_exp(double x)
{
    if (x > 700)
        x = 700;
    if (x < -700)
        return 0;
    double k = media_floor(x / 0.69314718055994530942 + 0.5);
    double r = x - k * 0.69314718055994530942, term = 1.0, sum = 1.0;
    for (int i = 1; i < 20; i++) {
        term *= r / i;
        sum += term;
    }
    return media_ldexp(sum, (int)k);
}

static inline double media_log(double x)
{
    if (x <= 0)
        return -1.0e300;
    int e = 0;
    while (x >= 2.0) {
        x /= 2.0;
        e++;
    }
    while (x < 1.0) {
        x *= 2.0;
        e--;
    }
    /* ln x = 2 atanh((x - 1) / (x + 1)) */
    double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
    for (int k = 1; k < 60; k += 2) {
        sum += term / k;
        term *= y2;
    }
    return 2 * sum + e * 0.69314718055994530942;
}

static inline double media_pow(double a, double b)
{
    if (b == 0)
        return 1;
    if (a == 0)
        return 0;
    return media_exp(b * media_log(a));
}

#define floor(x) media_floor(x)
#define ldexp(x, e) media_ldexp(x, e)
#define sin(x) media_sin(x)
#define cos(x) media_cos(x)
#define exp(x) media_exp(x)
#define log(x) media_log(x)
#define pow(a, b) media_pow(a, b)

#endif
