#include <synth/string.h>

size_t str_copy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);
    if (cap) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t str_append(char *dst, const char *src, size_t cap)
{
    size_t used = strnlen(dst, cap);
    if (used == cap)
        return used + strlen(src);
    return used + str_copy(dst + used, src, cap - used);
}

int str_casecmp(const char *a, const char *b)
{
    while (*a && to_lower((uint8_t)*a) == to_lower((uint8_t)*b))
        a++, b++;
    return to_lower((uint8_t)*a) - to_lower((uint8_t)*b);
}

int str_ncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = to_lower((uint8_t)*a), y = to_lower((uint8_t)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

bool str_starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && memcmp(s + a - b, suffix, b) == 0;
}

bool str_iends(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && str_casecmp(s + a - b, suffix) == 0;
}

char *str_trim(char *s)
{
    while (is_space((uint8_t)*s))
        s++;
    size_t n = strlen(s);
    while (n && is_space((uint8_t)s[n - 1]))
        s[--n] = 0;
    return s;
}

void str_upper(char *s)
{
    for (; *s; s++)
        *s = (char)to_upper((uint8_t)*s);
}

void str_lower(char *s)
{
    for (; *s; s++)
        *s = (char)to_lower((uint8_t)*s);
}

static int digit_value(int c)
{
    if (is_digit(c))
        return c - '0';
    if (is_lower(c))
        return c - 'a' + 10;
    if (is_upper(c))
        return c - 'A' + 10;
    return 99;
}

long str_to_long(const char *s, const char **end, int base)
{
    bool neg = false;
    unsigned long value = 0;
    while (is_space((uint8_t)*s))
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    if (base == 0) {
        base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && is_xdigit((uint8_t)s[2]))
            base = 16, s += 2;
        else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && (s[2] == '0' || s[2] == '1'))
            base = 2, s += 2;
        else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O'))
            base = 8, s += 2;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (int d; (d = digit_value((uint8_t)*s)) < base; s++)
        value = value * (unsigned long)base + (unsigned long)d;
    if (end)
        *end = s;
    return neg ? -(long)value : (long)value;
}

bool str_parse_long(const char *s, long *out)
{
    const char *end;
    if (!*s)
        return false;
    long v = str_to_long(s, &end, 0);
    if (*end || end == s || (end - s == 1 && (*s == '-' || *s == '+')))
        return false;
    *out = v;
    return true;
}

bool str_in_list(const char *name, const char *list)
{
    size_t n = strlen(name);
    while (list && *list) {
        const char *comma = strchr(list, ',');
        size_t len = comma ? (size_t)(comma - list) : strlen(list);
        if (len == n && memcmp(list, name, n) == 0)
            return true;
        list = comma ? comma + 1 : NULL;
    }
    return false;
}
