#include <synth/console.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/string.h>

#include "crt.h"

#define C_UPPER 0x1
#define C_LOWER 0x2
#define C_DIGIT 0x4
#define C_SPACE 0x8
#define C_PUNCT 0x10
#define C_CONTROL 0x20
#define C_BLANK 0x40
#define C_HEX 0x80
#define C_ALPHA 0x100

static W32_STATE uint16_t ctype_table[257];
static W32_STATE uint16_t *pctype = ctype_table + 1;
static W32_STATE uint16_t *pwctype = ctype_table + 1;
static W32_STATE uint8_t mbctype[257];
static W32_STATE uint32_t rand_state = 1;
static W32_STATE char *strtok_next;
static W32_STATE WCHAR *wcstok_next;

static void build_ctype(void)
{
    for (int c = 0; c < 256; c++) {
        uint16_t t = 0;
        if (c < 0x80) {
            if (is_upper(c))
                t |= C_UPPER | C_ALPHA;
            if (is_lower(c))
                t |= C_LOWER | C_ALPHA;
            if (is_digit(c))
                t |= C_DIGIT;
            if (is_xdigit(c))
                t |= C_HEX;
            if (c == ' ' || (c >= 9 && c <= 13))
                t |= C_SPACE;
            if (c == ' ' || c == '\t')
                t |= C_BLANK;
            if (c < 0x20 || c == 0x7f)
                t |= C_CONTROL;
            if (c > 0x20 && c < 0x7f && !is_alnum(c))
                t |= C_PUNCT;
        }
        ctype_table[c + 1] = t;
    }
}

static int ct(int c, int mask)
{
    return c >= -1 && c < 256 ? pctype[c] & mask : 0;
}

static int crt_isalpha(int c) { return ct(c, C_ALPHA); }
static int crt_isupper(int c) { return ct(c, C_UPPER); }
static int crt_islower(int c) { return ct(c, C_LOWER); }
static int crt_isdigit(int c) { return ct(c, C_DIGIT); }
static int crt_isxdigit(int c) { return ct(c, C_HEX); }
static int crt_isspace(int c) { return ct(c, C_SPACE); }
static int crt_ispunct(int c) { return ct(c, C_PUNCT); }
static int crt_isalnum(int c) { return ct(c, C_ALPHA | C_DIGIT); }
static int crt_isprint(int c) { return ct(c, C_ALPHA | C_DIGIT | C_PUNCT | C_BLANK) && c != '\t'; }
static int crt_isgraph(int c) { return ct(c, C_ALPHA | C_DIGIT | C_PUNCT); }
static int crt_iscntrl(int c) { return ct(c, C_CONTROL); }
static int crt_isblank(int c) { return c == ' ' || c == '\t'; }
static int crt_isascii(int c) { return (unsigned)c < 0x80; }
static int crt_iscsym(int c) { return crt_isalnum(c) || c == '_'; }
static int crt_iscsymf(int c) { return crt_isalpha(c) || c == '_'; }
static int crt_isctype(int c, int mask) { return ct(c, mask); }
static int crt_toupper(int c) { return c >= 0 && c < 256 && is_lower(c) ? c - 32 : c; }
static int crt_tolower(int c) { return c >= 0 && c < 256 && is_upper(c) ? c + 32 : c; }
static int crt_toupper_raw(int c) { return c - 32; }
static int crt_tolower_raw(int c) { return c + 32; }
static int crt_toascii(int c) { return c & 0x7f; }
static uint16_t **p_pctype(void) { return &pctype; }
static uint16_t *pctype_func(void) { return pctype; }
static uint16_t **p_pwctype(void) { return &pwctype; }

static int wct(unsigned c)
{
    WORD t;
    WCHAR w = (WCHAR)c;
    if (c < 0x80)
        return ctype_table[c + 1];
    t = 0;
    WCHAR up = w32_towupper(w), lo = w32_towlower(w);
    if (up != w || lo != w || (c >= 0xc0 && c <= 0x24f) || (c >= 0x400 && c <= 0x4ff))
        t |= C_ALPHA | (lo != w ? C_UPPER : up != w ? C_LOWER : 0);
    if (c == 0xa0 || c == 0x2000 || c == 0x3000)
        t |= C_SPACE | C_BLANK;
    return t;
}

static int crt_iswalpha(unsigned c) { return wct(c) & C_ALPHA; }
static int crt_iswupper(unsigned c) { return wct(c) & C_UPPER; }
static int crt_iswlower(unsigned c) { return wct(c) & C_LOWER; }
static int crt_iswdigit(unsigned c) { return c < 0x80 && is_digit((int)c); }
static int crt_iswxdigit(unsigned c) { return c < 0x80 && is_xdigit((int)c); }
static int crt_iswspace(unsigned c) { return wct(c) & C_SPACE; }
static int crt_iswpunct(unsigned c) { return wct(c) & C_PUNCT; }
static int crt_iswalnum(unsigned c) { return wct(c) & (C_ALPHA | C_DIGIT); }
static int crt_iswprint(unsigned c) { return c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xa0); }
static int crt_iswgraph(unsigned c) { return crt_iswprint(c) && c != ' '; }
static int crt_iswcntrl(unsigned c) { return c < 0x20 || c == 0x7f; }
static int crt_iswascii(unsigned c) { return c < 0x80; }
static int crt_iswctype(unsigned c, int mask) { return wct(c) & mask; }
static unsigned crt_towupper(unsigned c) { return w32_towupper((WCHAR)c); }
static unsigned crt_towlower(unsigned c) { return w32_towlower((WCHAR)c); }

static char *crt_strcpy(char *d, const char *s)
{
    memcpy(d, s, strlen(s) + 1);
    return d;
}

static char *crt_strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

static char *crt_strcat(char *d, const char *s)
{
    crt_strcpy(d + strlen(d), s);
    return d;
}

static char *crt_strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++)
        p[i] = s[i];
    p[i] = 0;
    return d;
}

static int crt_strcpy_s(char *d, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (!d || n + 1 > cap) {
        if (d && cap)
            d[0] = 0;
        return CRT_ERANGE;
    }
    memcpy(d, s, n + 1);
    return 0;
}

static int crt_strcat_s(char *d, size_t cap, const char *s)
{
    size_t n = strnlen(d, cap);
    if (n == cap)
        return CRT_EINVAL;
    return crt_strcpy_s(d + n, cap - n, s);
}

static int crt_strncpy_s(char *d, size_t cap, const char *s, size_t count)
{
    size_t n = strnlen(s, count == (size_t)-1 ? cap - 1 : count);
    if (n + 1 > cap) {
        if (count != (size_t)-1) {
            d[0] = 0;
            return CRT_ERANGE;
        }
        n = cap - 1;
    }
    memcpy(d, s, n);
    d[n] = 0;
    return 0;
}

static int crt_strncat_s(char *d, size_t cap, const char *s, size_t count)
{
    size_t n = strnlen(d, cap);
    if (n == cap)
        return CRT_EINVAL;
    return crt_strncpy_s(d + n, cap - n, s, count);
}

static size_t crt_strnlen_s(const char *s, size_t n)
{
    return s ? strnlen(s, n) : 0;
}

static int crt_memcpy_s(void *d, size_t cap, const void *s, size_t n)
{
    if (n > cap)
        return CRT_ERANGE;
    memcpy(d, s, n);
    return 0;
}

static int crt_memmove_s(void *d, size_t cap, const void *s, size_t n)
{
    if (n > cap)
        return CRT_ERANGE;
    memmove(d, s, n);
    return 0;
}

static int crt_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = to_lower((uint8_t)*a), y = to_lower((uint8_t)*b);
        if (x != y || !x)
            return x - y;
    }
}

static int crt_strnicmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = to_lower((uint8_t)*a), y = to_lower((uint8_t)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

static int crt_strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

static size_t crt_strxfrm(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (len < n)
        memcpy(d, s, len + 1);
    return len;
}

static char *crt_strdup(const char *s)
{
    return s ? w32_strdup(s) : NULL;
}

static char *crt_strpbrk(const char *s, const char *set)
{
    for (; *s; s++)
        if (strchr(set, *s))
            return (char *)s;
    return NULL;
}

static size_t crt_strspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && strchr(set, s[n]))
        n++;
    return n;
}

static size_t crt_strcspn(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && !strchr(set, s[n]))
        n++;
    return n;
}

static char *crt_strtok_s(char *s, const char *delim, char **ctx)
{
    if (!s)
        s = *ctx;
    if (!s)
        return NULL;
    s += crt_strspn(s, delim);
    if (!*s) {
        *ctx = NULL;
        return NULL;
    }
    char *end = s + crt_strcspn(s, delim);
    if (*end)
        *end++ = 0;
    else
        end = NULL;
    *ctx = end;
    return s;
}

static char *crt_strtok(char *s, const char *delim)
{
    return crt_strtok_s(s, delim, &strtok_next);
}

static char *crt_strupr(char *s)
{
    str_upper(s);
    return s;
}

static char *crt_strlwr(char *s)
{
    str_lower(s);
    return s;
}

static char *crt_strrev(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    return s;
}

static char *crt_strset(char *s, int c)
{
    for (char *p = s; *p; p++)
        *p = (char)c;
    return s;
}

static char *crt_strnset(char *s, int c, size_t n)
{
    for (char *p = s; *p && n; p++, n--)
        *p = (char)c;
    return s;
}

static void *crt_memccpy(void *d, const void *s, int c, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    for (size_t i = 0; i < n; i++) {
        dp[i] = sp[i];
        if (sp[i] == (uint8_t)c)
            return dp + i + 1;
    }
    return NULL;
}

static int crt_memicmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        int d = to_lower(x[i]) - to_lower(y[i]);
        if (d)
            return d;
    }
    return 0;
}

static size_t crt_wcslen(const WCHAR *s)
{
    return w32_wcslen(s);
}

static size_t crt_wcsnlen(const WCHAR *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static WCHAR *crt_wcscpy(WCHAR *d, const WCHAR *s)
{
    memmove(d, s, (w32_wcslen(s) + 1) * sizeof(WCHAR));
    return d;
}

static WCHAR *crt_wcsncpy(WCHAR *d, const WCHAR *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

static WCHAR *crt_wcscat(WCHAR *d, const WCHAR *s)
{
    crt_wcscpy(d + w32_wcslen(d), s);
    return d;
}

static WCHAR *crt_wcsncat(WCHAR *d, const WCHAR *s, size_t n)
{
    WCHAR *p = d + w32_wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++)
        p[i] = s[i];
    p[i] = 0;
    return d;
}

static int crt_wcscpy_s(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t n = w32_wcslen(s);
    if (n + 1 > cap) {
        if (cap)
            d[0] = 0;
        return CRT_ERANGE;
    }
    memcpy(d, s, (n + 1) * sizeof(WCHAR));
    return 0;
}

static int crt_wcscat_s(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t n = crt_wcsnlen(d, cap);
    return n == cap ? CRT_EINVAL : crt_wcscpy_s(d + n, cap - n, s);
}

static int crt_wcsncpy_s(WCHAR *d, size_t cap, const WCHAR *s, size_t count)
{
    size_t n = crt_wcsnlen(s, count == (size_t)-1 ? cap - 1 : count);
    if (n + 1 > cap)
        n = cap - 1;
    memcpy(d, s, n * sizeof(WCHAR));
    d[n] = 0;
    return 0;
}

static int crt_wcscmp(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++)
        if (*a != *b || !*a)
            return (int)*a - (int)*b;
}

static int crt_wcsncmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (int)*a - (int)*b;
    return 0;
}

static int crt_wcsicmp(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        int x = w32_towlower(*a), y = w32_towlower(*b);
        if (x != y || !x)
            return x - y;
    }
}

static int crt_wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = w32_towlower(*a), y = w32_towlower(*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

static WCHAR *crt_wcschr(const WCHAR *s, unsigned c)
{
    for (;; s++) {
        if (*s == (WCHAR)c)
            return (WCHAR *)s;
        if (!*s)
            return NULL;
    }
}

static WCHAR *crt_wcsrchr(const WCHAR *s, unsigned c)
{
    const WCHAR *last = NULL;
    for (;; s++) {
        if (*s == (WCHAR)c)
            last = s;
        if (!*s)
            return (WCHAR *)last;
    }
}

static WCHAR *crt_wcsstr(const WCHAR *h, const WCHAR *n)
{
    size_t nl = w32_wcslen(n);
    if (!nl)
        return (WCHAR *)h;
    for (; *h; h++)
        if (crt_wcsncmp(h, n, nl) == 0)
            return (WCHAR *)h;
    return NULL;
}

static WCHAR *crt_wcspbrk(const WCHAR *s, const WCHAR *set)
{
    for (; *s; s++)
        if (crt_wcschr(set, *s))
            return (WCHAR *)s;
    return NULL;
}

static size_t crt_wcsspn(const WCHAR *s, const WCHAR *set)
{
    size_t n = 0;
    while (s[n] && crt_wcschr(set, s[n]))
        n++;
    return n;
}

static size_t crt_wcscspn(const WCHAR *s, const WCHAR *set)
{
    size_t n = 0;
    while (s[n] && !crt_wcschr(set, s[n]))
        n++;
    return n;
}

static WCHAR *crt_wcstok_s(WCHAR *s, const WCHAR *delim, WCHAR **ctx)
{
    if (!s)
        s = *ctx;
    if (!s)
        return NULL;
    s += crt_wcsspn(s, delim);
    if (!*s) {
        *ctx = NULL;
        return NULL;
    }
    WCHAR *end = s + crt_wcscspn(s, delim);
    if (*end)
        *end++ = 0;
    else
        end = NULL;
    *ctx = end;
    return s;
}

static WCHAR *crt_wcstok(WCHAR *s, const WCHAR *delim)
{
    return crt_wcstok_s(s, delim, &wcstok_next);
}

static WCHAR *crt_wcsdup(const WCHAR *s)
{
    size_t n = (w32_wcslen(s) + 1) * sizeof(WCHAR);
    WCHAR *d = w32_alloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static WCHAR *crt_wcsupr(WCHAR *s)
{
    for (WCHAR *p = s; *p; p++)
        *p = w32_towupper(*p);
    return s;
}

static WCHAR *crt_wcslwr(WCHAR *s)
{
    for (WCHAR *p = s; *p; p++)
        *p = w32_towlower(*p);
    return s;
}

static WCHAR *crt_wcsrev(WCHAR *s)
{
    size_t n = w32_wcslen(s);
    for (size_t i = 0; i < n / 2; i++) {
        WCHAR t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    return s;
}

static size_t crt_wcsxfrm(WCHAR *d, const WCHAR *s, size_t n)
{
    size_t len = w32_wcslen(s);
    if (len < n)
        memcpy(d, s, (len + 1) * sizeof(WCHAR));
    return len;
}

static WCHAR *crt_wmemcpy(WCHAR *d, const WCHAR *s, size_t n)
{
    return memcpy(d, s, n * sizeof(WCHAR));
}

static WCHAR *crt_wmemmove(WCHAR *d, const WCHAR *s, size_t n)
{
    return memmove(d, s, n * sizeof(WCHAR));
}

static WCHAR *crt_wmemset(WCHAR *d, WCHAR c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        d[i] = c;
    return d;
}

static WCHAR *crt_wmemchr(const WCHAR *s, WCHAR c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == c)
            return (WCHAR *)s + i;
    return NULL;
}

static int crt_wmemcmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int digit_value(int c)
{
    if (is_digit(c))
        return c - '0';
    if (is_alpha(c))
        return to_lower(c) - 'a' + 10;
    return 99;
}

static uint64_t parse_unsigned(const char *s, char **end, int base, bool *neg, bool *overflow, uint64_t limit)
{
    const char *p = s;
    while (is_space((uint8_t)*p))
        p++;
    *neg = false;
    *overflow = false;
    if (*p == '+' || *p == '-')
        *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_value((uint8_t)p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    uint64_t v = 0;
    const char *start = p;
    while (digit_value((uint8_t)*p) < base) {
        int d = digit_value((uint8_t)*p++);
        if (v > (limit - (uint64_t)d) / (uint64_t)base)
            *overflow = true;
        else
            v = v * (uint64_t)base + (uint64_t)d;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

long crt_strtol(const char *s, char **end, int base)
{
    bool neg, of;
    uint64_t v = parse_unsigned(s, end, base, &neg, &of, 0x80000000u);
    if (of || (!neg && v > 0x7fffffffu)) {
        crt_set_errno(CRT_ERANGE);
        return neg ? (long)0x80000000u : 0x7fffffff;
    }
    return neg ? -(long)v : (long)v;
}

unsigned long crt_strtoul(const char *s, char **end, int base)
{
    bool neg, of;
    uint64_t v = parse_unsigned(s, end, base, &neg, &of, 0xffffffffu);
    if (of) {
        crt_set_errno(CRT_ERANGE);
        return 0xffffffffu;
    }
    return neg ? (unsigned long)-(long)v : (unsigned long)v;
}

int64_t crt_strtoi64(const char *s, char **end, int base)
{
    bool neg, of;
    uint64_t v = parse_unsigned(s, end, base, &neg, &of, 0x8000000000000000ull);
    if (of || (!neg && v > 0x7fffffffffffffffull)) {
        crt_set_errno(CRT_ERANGE);
        return neg ? (int64_t)0x8000000000000000ull : 0x7fffffffffffffffll;
    }
    return neg ? -(int64_t)v : (int64_t)v;
}

uint64_t crt_strtoui64(const char *s, char **end, int base)
{
    bool neg, of;
    uint64_t v = parse_unsigned(s, end, base, &neg, &of, ~0ull);
    if (of) {
        crt_set_errno(CRT_ERANGE);
        return ~0ull;
    }
    return neg ? (uint64_t)-(int64_t)v : v;
}

static int crt_atoi(const char *s)
{
    return (int)crt_strtol(s, NULL, 10);
}

static int64_t crt_atoi64(const char *s)
{
    return crt_strtoi64(s, NULL, 10);
}

static char *narrow_number(const WCHAR *w, char *buf, size_t cap)
{
    size_t i = 0;
    for (; w[i] && i < cap - 1; i++)
        buf[i] = w[i] < 0x80 ? (char)w[i] : '?';
    buf[i] = 0;
    return buf;
}

static long crt_wcstol(const WCHAR *s, WCHAR **end, int base)
{
    char buf[96], *e;
    long v = crt_strtol(narrow_number(s, buf, sizeof(buf)), &e, base);
    if (end)
        *end = (WCHAR *)s + (e - buf);
    return v;
}

static unsigned long crt_wcstoul(const WCHAR *s, WCHAR **end, int base)
{
    char buf[96], *e;
    unsigned long v = crt_strtoul(narrow_number(s, buf, sizeof(buf)), &e, base);
    if (end)
        *end = (WCHAR *)s + (e - buf);
    return v;
}

static int64_t crt_wcstoi64(const WCHAR *s, WCHAR **end, int base)
{
    char buf[96], *e;
    int64_t v = crt_strtoi64(narrow_number(s, buf, sizeof(buf)), &e, base);
    if (end)
        *end = (WCHAR *)s + (e - buf);
    return v;
}

static uint64_t crt_wcstoui64(const WCHAR *s, WCHAR **end, int base)
{
    char buf[96], *e;
    uint64_t v = crt_strtoui64(narrow_number(s, buf, sizeof(buf)), &e, base);
    if (end)
        *end = (WCHAR *)s + (e - buf);
    return v;
}

static int crt_wtoi(const WCHAR *s)
{
    return (int)crt_wcstol(s, NULL, 10);
}

static int64_t crt_wtoi64(const WCHAR *s)
{
    return crt_wcstoi64(s, NULL, 10);
}

static char *u64toa(uint64_t v, bool neg, char *out, int base)
{
    char tmp[72];
    int n = 0;
    do {
        int d = (int)(v % (uint64_t)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (uint64_t)base;
    } while (v);
    char *p = out;
    if (neg)
        *p++ = '-';
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return out;
}

static char *crt_itoa(int v, char *out, int base)
{
    bool neg = base == 10 && v < 0;
    return u64toa(neg ? (uint64_t)-(int64_t)v : (uint32_t)v, neg, out, base);
}

static char *crt_ultoa(unsigned long v, char *out, int base)
{
    return u64toa(v, false, out, base);
}

static char *crt_i64toa(int64_t v, char *out, int base)
{
    bool neg = base == 10 && v < 0;
    return u64toa(neg ? (uint64_t)-v : (uint64_t)v, neg, out, base);
}

static char *crt_ui64toa(uint64_t v, char *out, int base)
{
    return u64toa(v, false, out, base);
}

static int crt_itoa_s(int v, char *out, size_t cap, int base)
{
    char tmp[72];
    crt_itoa(v, tmp, base);
    return crt_strcpy_s(out, cap, tmp);
}

static WCHAR *widen_number(const char *s, WCHAR *out)
{
    size_t i = 0;
    for (; s[i]; i++)
        out[i] = (uint8_t)s[i];
    out[i] = 0;
    return out;
}

static WCHAR *crt_itow(int v, WCHAR *out, int base)
{
    char tmp[72];
    return widen_number(crt_itoa(v, tmp, base), out);
}

static WCHAR *crt_ultow(unsigned long v, WCHAR *out, int base)
{
    char tmp[72];
    return widen_number(crt_ultoa(v, tmp, base), out);
}

static WCHAR *crt_i64tow(int64_t v, WCHAR *out, int base)
{
    char tmp[72];
    return widen_number(crt_i64toa(v, tmp, base), out);
}

static WCHAR *crt_ui64tow(uint64_t v, WCHAR *out, int base)
{
    char tmp[72];
    return widen_number(crt_ui64toa(v, tmp, base), out);
}

static int crt_abs(int v)
{
    return v < 0 ? -v : v;
}

static int64_t crt_abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

static uint64_t crt_div(int a, int b)
{
    struct {
        int q, r;
    } d = {a / b, a % b};
    uint64_t out;
    memcpy(&out, &d, 8);
    return out;
}

static void crt_lldiv(int64_t *out, int64_t a, int64_t b)
{
    out[0] = a / b;
    out[1] = a % b;
}

static int crt_rand(void)
{
    rand_state = rand_state * 214013u + 2531011u;
    return (int)((rand_state >> 16) & 0x7fff);
}

static void crt_srand(unsigned seed)
{
    rand_state = seed;
}

static int crt_rand_s(unsigned *out)
{
    *out = hal_entropy() ^ ((unsigned)crt_rand() << 16) ^ (unsigned)crt_rand();
    return 0;
}

static unsigned rotl(unsigned v, int s)
{
    s &= 31;
    return s ? v << s | v >> (32 - s) : v;
}

static unsigned rotr(unsigned v, int s)
{
    s &= 31;
    return s ? v >> s | v << (32 - s) : v;
}

static uint64_t rotl64(uint64_t v, int s)
{
    s &= 63;
    return s ? v << s | v >> (64 - s) : v;
}

static uint64_t rotr64(uint64_t v, int s)
{
    s &= 63;
    return s ? v >> s | v << (64 - s) : v;
}

static uint16_t bswap16(uint16_t v)
{
    return (uint16_t)(v >> 8 | v << 8);
}

static uint32_t bswap32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static uint64_t bswap64(uint64_t v)
{
    return __builtin_bswap64(v);
}

typedef int (*cmp_fn)(const void *, const void *);
typedef int (*cmp_s_fn)(void *, const void *, const void *);

static void swap_bytes(uint8_t *a, uint8_t *b, size_t n)
{
    while (n--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sort_range(uint8_t *base, size_t n, size_t size, cmp_fn cmp, cmp_s_fn cmp_s, void *ctx)
{
#define CMP(x, y) (cmp ? cmp((x), (y)) : cmp_s(ctx, (x), (y)))
    while (n > 12) {
        uint8_t *mid = base + (n / 2) * size, *last = base + (n - 1) * size;
        if (CMP(mid, base) < 0)
            swap_bytes(mid, base, size);
        if (CMP(last, mid) < 0) {
            swap_bytes(last, mid, size);
            if (CMP(mid, base) < 0)
                swap_bytes(mid, base, size);
        }
        swap_bytes(mid, base, size);
        size_t i = 1, j = n - 1;
        for (;;) {
            while (i <= j && CMP(base + i * size, base) < 0)
                i++;
            while (j >= i && CMP(base + j * size, base) > 0)
                j--;
            if (i >= j)
                break;
            swap_bytes(base + i * size, base + j * size, size);
            i++;
            j--;
        }
        swap_bytes(base, base + j * size, size);
        if (j < n - j - 1) {
            sort_range(base, j, size, cmp, cmp_s, ctx);
            base += (j + 1) * size;
            n -= j + 1;
        } else {
            sort_range(base + (j + 1) * size, n - j - 1, size, cmp, cmp_s, ctx);
            n = j;
        }
    }
    for (size_t i = 1; i < n; i++)
        for (size_t k = i; k > 0 && CMP(base + (k - 1) * size, base + k * size) > 0; k--)
            swap_bytes(base + (k - 1) * size, base + k * size, size);
#undef CMP
}

static void crt_qsort(void *base, size_t n, size_t size, cmp_fn cmp)
{
    sort_range(base, n, size, cmp, NULL, NULL);
}

static void crt_qsort_s(void *base, size_t n, size_t size, cmp_s_fn cmp, void *ctx)
{
    sort_range(base, n, size, NULL, cmp, ctx);
}

static void *crt_bsearch(const void *key, const void *base, size_t n, size_t size, cmp_fn cmp)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const uint8_t *p = (const uint8_t *)base + mid * size;
        int r = cmp(key, p);
        if (!r)
            return (void *)p;
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static void *crt_lfind(const void *key, const void *base, unsigned *n, size_t size, cmp_fn cmp)
{
    for (unsigned i = 0; i < *n; i++)
        if (!cmp(key, (const uint8_t *)base + i * size))
            return (uint8_t *)base + i * size;
    return NULL;
}

static int crt_mblen(const char *s, size_t n)
{
    return !s ? 0 : !n ? -1 : *s ? 1 : 0;
}

static int crt_mbtowc(WCHAR *out, const char *s, size_t n)
{
    if (!s)
        return 0;
    if (!n)
        return -1;
    WCHAR w;
    w32_cp_to_wide(s, 1, &w, 1, 0);
    if (out)
        *out = w;
    return *s ? 1 : 0;
}

static int crt_wctomb(char *out, WCHAR w)
{
    if (!out)
        return 0;
    w32_wide_to_cp(&w, 1, out, 1, 0);
    return 1;
}

static size_t crt_mbstowcs(WCHAR *out, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (!out)
        return len;
    size_t k = MIN(len, n);
    w32_cp_to_wide(s, (int)k, out, k, 0);
    if (k < n)
        out[k] = 0;
    return k;
}

static size_t crt_wcstombs(char *out, const WCHAR *w, size_t n);

static int crt_mbstowcs_s(size_t *done, WCHAR *out, size_t cap, const char *s, size_t count)
{
    if (!s || (out && !cap)) {
        crt_set_errno(CRT_EINVAL);
        return CRT_EINVAL;
    }
    size_t limit = out ? MIN(cap - 1, count) : 0;
    size_t n = crt_mbstowcs(out, s, limit);
    if (out)
        out[MIN(n, cap - 1)] = 0;
    if (done)
        *done = (out ? MIN(n, cap - 1) : crt_mbstowcs(NULL, s, 0)) + 1;
    return 0;
}

static int crt_wcstombs_s(size_t *done, char *out, size_t cap, const WCHAR *w, size_t count)
{
    if (!w || (out && !cap)) {
        crt_set_errno(CRT_EINVAL);
        return CRT_EINVAL;
    }
    size_t limit = out ? MIN(cap - 1, count) : 0;
    size_t n = crt_wcstombs(out, w, limit);
    if (out)
        out[MIN(n, cap - 1)] = 0;
    if (done)
        *done = (out ? MIN(n, cap - 1) : crt_wcstombs(NULL, w, 0)) + 1;
    return 0;
}

static size_t crt_wcstombs(char *out, const WCHAR *w, size_t n)
{
    size_t len = w32_wcslen(w);
    if (!out)
        return len;
    size_t k = MIN(len, n);
    w32_wide_to_cp(w, (int)k, out, k, 0);
    if (k < n)
        out[k] = 0;
    return k;
}

static unsigned crt_btowc(int c)
{
    if (c < 0 || c > 255)
        return 0xffff;
    char ch = (char)c;
    WCHAR w;
    w32_cp_to_wide(&ch, 1, &w, 1, 0);
    return w;
}

static int crt_wctob(unsigned w)
{
    char c;
    WCHAR wc = (WCHAR)w;
    w32_wide_to_cp(&wc, 1, &c, 1, 0);
    return c == '?' && w != '?' ? -1 : (uint8_t)c;
}

static size_t crt_mbrtowc(WCHAR *out, const char *s, size_t n, void *state)
{
    UNUSED(state);
    if (!s)
        return 0;
    return (size_t)crt_mbtowc(out, s, n);
}

static size_t crt_wcrtomb(char *out, WCHAR w, void *state)
{
    UNUSED(state);
    return out ? (size_t)crt_wctomb(out, w) : 1;
}

static size_t crt_mbrlen(const char *s, size_t n, void *state)
{
    UNUSED(state);
    return (size_t)crt_mblen(s, n);
}

static int crt_mbsinit(const void *state)
{
    UNUSED(state);
    return 1;
}

static int crt_ismbblead(unsigned c)
{
    UNUSED(c);
    return 0;
}

static uint8_t *crt_mbschr(const uint8_t *s, unsigned c)
{
    return (uint8_t *)strchr((const char *)s, (int)c);
}

static uint8_t *crt_mbsrchr(const uint8_t *s, unsigned c)
{
    return (uint8_t *)strrchr((const char *)s, (int)c);
}

static W32_STATE int pending_scan = -1;

static int crt_kbhit(void)
{
    con_flush();
    return pending_scan >= 0 || w32_console_peek();
}

static int crt_getch(void)
{
    if (pending_scan >= 0) {
        int s = pending_scan;
        pending_scan = -1;
        return s;
    }
    con_flush();
    for (;;) {
        int key = w32_console_getkey(true);
        if (key == KEY_NONE || key == KEY_RESIZE)
            continue;
        WORD vk, scan;
        WCHAR ch;
        DWORD state;
        if (!w32_key_info(key, &vk, &scan, &ch, &state))
            continue;
        if (ch) {
            char c;
            w32_wide_to_cp(&ch, 1, &c, 1, 1);
            return (uint8_t)c;
        }
        pending_scan = scan;
        return KEY_BASE(key) >= KEY_F1 && KEY_BASE(key) <= KEY_F10 ? 0 : 0xe0;
    }
}

static int crt_getche(void)
{
    int c = crt_getch();
    if (c && c != 0xe0) {
        char ch = (char)c;
        w32_console_write_raw(WH_STDOUT, &ch, 1);
    }
    return c;
}

static int crt_putch(int c)
{
    char ch = (char)c;
    w32_console_write(WH_STDOUT, &ch, 1);
    return c;
}

static int crt_ungetch(int c)
{
    pending_scan = c;
    return c;
}

static unsigned short crt_getwche(void)
{
    return (unsigned short)crt_getche();
}

static unsigned short crt_ungetwch(unsigned short c)
{
    crt_ungetch(c);
    return c;
}

static int crt_cputs(const char *s)
{
    w32_console_write(WH_STDOUT, s, strlen(s));
    return 0;
}

static char *crt_cgets(char *buf)
{
    int max = (uint8_t)buf[0];
    long n = w32_console_read(buf + 2, (size_t)max);
    while (n > 0 && (buf[2 + n - 1] == '\n' || buf[2 + n - 1] == '\r'))
        n--;
    buf[1] = (char)n;
    buf[2 + n] = 0;
    return buf + 2;
}

static unsigned crt_getwch(void)
{
    int c = crt_getch();
    char ch = (char)c;
    WCHAR w;
    w32_cp_to_wide(&ch, 1, &w, 1, 1);
    return w;
}

static unsigned crt_putwch(WCHAR c)
{
    char out[4];
    size_t n = w32_wide_to_cp(&c, 1, out, sizeof(out), 1);
    w32_console_write_raw(WH_STDOUT, out, n);
    return c;
}

static int crt_cputws(const WCHAR *s)
{
    while (*s)
        crt_putwch(*s++);
    return 0;
}

static void stdlib_attach(void)
{
    build_ctype();
    memset(mbctype, 0, sizeof(mbctype));
    rand_state = 1;
    strtok_next = NULL;
    wcstok_next = NULL;
    pending_scan = -1;
}

static const struct w32_export stdlib_exports[] = {
    W32_AS("isalpha", crt_isalpha), W32_AS("isupper", crt_isupper), W32_AS("islower", crt_islower),
    W32_AS("isdigit", crt_isdigit), W32_AS("isxdigit", crt_isxdigit), W32_AS("isspace", crt_isspace),
    W32_AS("ispunct", crt_ispunct), W32_AS("isalnum", crt_isalnum), W32_AS("isprint", crt_isprint),
    W32_AS("isgraph", crt_isgraph), W32_AS("iscntrl", crt_iscntrl), W32_AS("isblank", crt_isblank),
    W32_AS("__isascii", crt_isascii), W32_AS("isascii", crt_isascii), W32_AS("__iscsym", crt_iscsym),
    W32_AS("__iscsymf", crt_iscsymf), W32_AS("_isctype", crt_isctype), W32_AS("toupper", crt_toupper),
    W32_AS("tolower", crt_tolower), W32_AS("_toupper", crt_toupper_raw), W32_AS("_tolower", crt_tolower_raw),
    W32_AS("__toascii", crt_toascii), W32_AS("toascii", crt_toascii), W32_AS("_pctype", &pctype),
    W32_AS("_pwctype", &pwctype), W32_AS("_ctype", ctype_table), W32_AS("_mbctype", mbctype),
    W32_AS("__p__pctype", p_pctype), W32_AS("__pctype_func", pctype_func), W32_AS("__p__pwctype", p_pwctype),
    W32_AS("__pwctype_func", pctype_func), W32_AS("iswalpha", crt_iswalpha), W32_AS("iswupper", crt_iswupper),
    W32_AS("iswlower", crt_iswlower), W32_AS("iswdigit", crt_iswdigit), W32_AS("iswxdigit", crt_iswxdigit),
    W32_AS("iswspace", crt_iswspace), W32_AS("iswpunct", crt_iswpunct), W32_AS("iswalnum", crt_iswalnum),
    W32_AS("iswprint", crt_iswprint), W32_AS("iswgraph", crt_iswgraph), W32_AS("iswcntrl", crt_iswcntrl),
    W32_AS("iswascii", crt_iswascii), W32_AS("iswctype", crt_iswctype), W32_AS("is_wctype", crt_iswctype),
    W32_AS("towupper", crt_towupper), W32_AS("towlower", crt_towlower), W32_AS("memcpy", memcpy),
    W32_AS("memmove", memmove), W32_AS("memset", memset), W32_AS("memcmp", memcmp), W32_AS("memchr", memchr),
    W32_AS("strlen", strlen), W32_AS("strnlen", strnlen), W32_AS("strcmp", strcmp), W32_AS("strncmp", strncmp),
    W32_AS("strchr", strchr), W32_AS("strrchr", strrchr), W32_AS("strstr", strstr), W32_AS("strcpy", crt_strcpy),
    W32_AS("strncpy", crt_strncpy), W32_AS("strcat", crt_strcat), W32_AS("strncat", crt_strncat),
    W32_AS("strcpy_s", crt_strcpy_s), W32_AS("strcat_s", crt_strcat_s), W32_AS("strncpy_s", crt_strncpy_s),
    W32_AS("strncat_s", crt_strncat_s), W32_AS("strnlen_s", crt_strnlen_s), W32_AS("memcpy_s", crt_memcpy_s),
    W32_AS("memmove_s", crt_memmove_s), W32_AS("_stricmp", crt_stricmp), W32_AS("_strcmpi", crt_stricmp),
    W32_AS("stricmp", crt_stricmp), W32_AS("strcmpi", crt_stricmp), W32_AS("_stricoll", crt_stricmp),
    W32_AS("_strnicmp", crt_strnicmp), W32_AS("strnicmp", crt_strnicmp), W32_AS("_strncoll", strncmp),
    W32_AS("_strnicoll", crt_strnicmp), W32_AS("strcoll", crt_strcoll), W32_AS("strxfrm", crt_strxfrm),
    W32_AS("_strdup", crt_strdup), W32_AS("strdup", crt_strdup), W32_AS("strpbrk", crt_strpbrk),
    W32_AS("strspn", crt_strspn), W32_AS("strcspn", crt_strcspn), W32_AS("strtok", crt_strtok),
    W32_AS("strtok_s", crt_strtok_s), W32_AS("_strupr", crt_strupr), W32_AS("strupr", crt_strupr),
    W32_AS("_strlwr", crt_strlwr), W32_AS("strlwr", crt_strlwr), W32_AS("_strrev", crt_strrev),
    W32_AS("_strset", crt_strset), W32_AS("_strnset", crt_strnset), W32_AS("_memccpy", crt_memccpy),
    W32_AS("_memicmp", crt_memicmp), W32_AS("wcslen", crt_wcslen), W32_AS("wcsnlen", crt_wcsnlen),
    W32_AS("wcscpy", crt_wcscpy), W32_AS("wcsncpy", crt_wcsncpy), W32_AS("wcscat", crt_wcscat),
    W32_AS("wcsncat", crt_wcsncat), W32_AS("wcscpy_s", crt_wcscpy_s), W32_AS("wcscat_s", crt_wcscat_s),
    W32_AS("wcsncpy_s", crt_wcsncpy_s), W32_AS("wcscmp", crt_wcscmp), W32_AS("wcsncmp", crt_wcsncmp),
    W32_AS("wcscoll", crt_wcscmp), W32_AS("_wcsicmp", crt_wcsicmp), W32_AS("_wcsicoll", crt_wcsicmp),
    W32_AS("_wcsnicmp", crt_wcsnicmp), W32_AS("wcschr", crt_wcschr), W32_AS("wcsrchr", crt_wcsrchr),
    W32_AS("wcsstr", crt_wcsstr), W32_AS("wcspbrk", crt_wcspbrk), W32_AS("wcsspn", crt_wcsspn),
    W32_AS("wcscspn", crt_wcscspn), W32_AS("wcstok", crt_wcstok), W32_AS("wcstok_s", crt_wcstok_s),
    W32_AS("_wcsdup", crt_wcsdup), W32_AS("_wcsupr", crt_wcsupr), W32_AS("_wcslwr", crt_wcslwr),
    W32_AS("_wcsrev", crt_wcsrev), W32_AS("wcsxfrm", crt_wcsxfrm), W32_AS("wmemcpy", crt_wmemcpy),
    W32_AS("wmemmove", crt_wmemmove), W32_AS("wmemset", crt_wmemset), W32_AS("wmemchr", crt_wmemchr),
    W32_AS("wmemcmp", crt_wmemcmp), W32_AS("strtol", crt_strtol), W32_AS("strtoul", crt_strtoul),
    W32_AS("_strtoi64", crt_strtoi64), W32_AS("_strtoui64", crt_strtoui64), W32_AS("strtoll", crt_strtoi64),
    W32_AS("strtoull", crt_strtoui64), W32_AS("strtoimax", crt_strtoi64), W32_AS("strtoumax", crt_strtoui64),
    W32_AS("atoi", crt_atoi), W32_AS("atol", crt_atoi), W32_AS("_atoi64", crt_atoi64), W32_AS("atoll", crt_atoi64),
    W32_AS("wcstol", crt_wcstol), W32_AS("wcstoul", crt_wcstoul), W32_AS("_wcstoi64", crt_wcstoi64),
    W32_AS("_wcstoui64", crt_wcstoui64), W32_AS("wcstoll", crt_wcstoi64), W32_AS("wcstoull", crt_wcstoui64),
    W32_AS("_wtoi", crt_wtoi), W32_AS("_wtol", crt_wtoi), W32_AS("_wtoi64", crt_wtoi64), W32_AS("_itoa", crt_itoa),
    W32_AS("itoa", crt_itoa), W32_AS("_ltoa", crt_itoa), W32_AS("ltoa", crt_itoa), W32_AS("_ultoa", crt_ultoa),
    W32_AS("_i64toa", crt_i64toa), W32_AS("_ui64toa", crt_ui64toa), W32_AS("_itoa_s", crt_itoa_s),
    W32_AS("_itow", crt_itow), W32_AS("_ltow", crt_itow), W32_AS("_ultow", crt_ultow), W32_AS("_i64tow", crt_i64tow),
    W32_AS("_ui64tow", crt_ui64tow), W32_AS("abs", crt_abs), W32_AS("labs", crt_abs), W32_AS("_abs64", crt_abs64),
    W32_AS("llabs", crt_abs64), W32_AS("div", crt_div), W32_AS("ldiv", crt_div), W32_AS("lldiv", crt_lldiv),
    W32_AS("rand", crt_rand), W32_AS("srand", crt_srand), W32_AS("rand_s", crt_rand_s), W32_AS("_rotl", rotl),
    W32_AS("_rotr", rotr), W32_AS("_lrotl", rotl), W32_AS("_lrotr", rotr), W32_AS("_rotl64", rotl64),
    W32_AS("_rotr64", rotr64), W32_AS("_byteswap_ushort", bswap16), W32_AS("_byteswap_ulong", bswap32),
    W32_AS("_byteswap_uint64", bswap64), W32_AS("qsort", crt_qsort), W32_AS("qsort_s", crt_qsort_s),
    W32_AS("bsearch", crt_bsearch), W32_AS("_lfind", crt_lfind), W32_AS("mblen", crt_mblen),
    W32_AS("mbtowc", crt_mbtowc), W32_AS("wctomb", crt_wctomb), W32_AS("mbstowcs", crt_mbstowcs),
    W32_AS("wcstombs", crt_wcstombs), W32_AS("mbstowcs_s", crt_mbstowcs_s), W32_AS("wcstombs_s", crt_wcstombs_s), W32_AS("btowc", crt_btowc), W32_AS("wctob", crt_wctob),
    W32_AS("mbrtowc", crt_mbrtowc), W32_AS("wcrtomb", crt_wcrtomb), W32_AS("mbrlen", crt_mbrlen),
    W32_AS("mbsinit", crt_mbsinit), W32_AS("_ismbblead", crt_ismbblead), W32_AS("_ismbbtrail", crt_ismbblead),
    W32_AS("_mbschr", crt_mbschr), W32_AS("_mbsrchr", crt_mbsrchr), W32_AS("_mbslen", strlen),
    W32_AS("_mbscmp", strcmp), W32_AS("_mbsicmp", crt_stricmp), W32_AS("_mbsstr", strstr),
    W32_AS("_kbhit", crt_kbhit), W32_AS("kbhit", crt_kbhit), W32_AS("_getch", crt_getch),
    W32_AS("getch", crt_getch), W32_AS("_getche", crt_getche), W32_AS("getche", crt_getche),
    W32_AS("_putch", crt_putch), W32_AS("putch", crt_putch), W32_AS("_ungetch", crt_ungetch),
    W32_AS("_getwche", crt_getwche), W32_AS("_ungetwch", crt_ungetwch),
    W32_AS("_cputs", crt_cputs), W32_AS("_cgets", crt_cgets), W32_AS("_getwch", crt_getwch),
    W32_AS("_putwch", crt_putwch), W32_AS("_cputws", crt_cputws),
};

W32_EXPORTS(crt_stdlib_module, "msvcrt.dll", stdlib_exports, .attach = stdlib_attach);
