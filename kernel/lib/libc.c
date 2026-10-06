#include <synth/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uintptr_t)d | (uintptr_t)s) & (sizeof(uintptr_t) - 1)) == 0) {
        while (n >= sizeof(uintptr_t)) {
            *(uintptr_t *)d = *(const uintptr_t *)s;
            d += sizeof(uintptr_t);
            s += sizeof(uintptr_t);
            n -= sizeof(uintptr_t);
        }
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint8_t v = (uint8_t)c;
    if (((uintptr_t)d & (sizeof(uintptr_t) - 1)) == 0 && n >= sizeof(uintptr_t)) {
        uintptr_t word = v;
        for (size_t i = 1; i < sizeof(uintptr_t); i++)
            word = (word << 8) | v;
        while (n >= sizeof(uintptr_t)) {
            *(uintptr_t *)d = word;
            d += sizeof(uintptr_t);
            n -= sizeof(uintptr_t);
        }
    }
    while (n--)
        *d++ = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    for (; n; n--, p++)
        if (*p == (uint8_t)c)
            return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (uint8_t)*a - (uint8_t)*b;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)haystack;
    for (; *haystack; haystack++)
        if (*haystack == *needle && strncmp(haystack, needle, n) == 0)
            return (char *)haystack;
    return NULL;
}
