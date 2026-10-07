#include <synth/string.h>

#include "crt.h"

#define MAX_DIGITS 40
#define EXACT_DIGITS 19

enum {
    F_LEFT = 1,
    F_PLUS = 2,
    F_SPACE = 4,
    F_ALT = 8,
    F_ZERO = 16,
    F_UPPER = 32,
};

enum { L_NONE, L_HH, L_H, L_L, L_LL, L_W };

struct out {
    crt_sink nsink;
    crt_wsink wsink;
    void *ctx;
    int count;
};

static W32_STATE int exponent_digits = 3;
int crt_slots64;

#define ARG_INT(ap) (crt_slots64 ? (int)va_arg(*(ap), uint64_t) : va_arg(*(ap), int))
#define ARG_UINT(ap) (crt_slots64 ? (unsigned)va_arg(*(ap), uint64_t) : va_arg(*(ap), unsigned))
#define ARG_PTR(ap) (crt_slots64 ? (void *)(uintptr_t)va_arg(*(ap), uint64_t) : va_arg(*(ap), void *))

static void out_chars(struct out *o, const char *s, size_t n)
{
    o->count += (int)n;
    if (o->nsink) {
        o->nsink(o->ctx, s, n);
        return;
    }
    WCHAR tmp[64];
    while (n) {
        size_t k = MIN(n, ARRAY_SIZE(tmp));
        size_t wn = w32_cp_to_wide(s, (int)k, tmp, ARRAY_SIZE(tmp), 0);
        o->wsink(o->ctx, tmp, MIN(wn, ARRAY_SIZE(tmp)));
        s += k;
        n -= k;
    }
}

static void out_wchars(struct out *o, const WCHAR *s, size_t n)
{
    o->count += (int)n;
    if (o->wsink) {
        o->wsink(o->ctx, s, n);
        return;
    }
    char tmp[128];
    while (n) {
        size_t k = MIN(n, (size_t)64);
        size_t len = w32_wide_to_cp(s, (int)k, tmp, sizeof(tmp), 0);
        o->nsink(o->ctx, tmp, MIN(len, sizeof(tmp)));
        s += k;
        n -= k;
    }
}

static void out_pad(struct out *o, char c, int n)
{
    char buf[32];
    memset(buf, c, sizeof(buf));
    while (n > 0) {
        int k = MIN(n, (int)sizeof(buf));
        out_chars(o, buf, (size_t)k);
        n -= k;
    }
}

static long double pow10l_int(int e)
{
    long double r = 1.0L, b = 10.0L;
    unsigned n = (unsigned)(e < 0 ? -e : e);
    while (n) {
        if (n & 1)
            r *= b;
        b *= b;
        n >>= 1;
    }
    return e < 0 ? 1.0L / r : r;
}

static int floor_log10(long double v)
{
    long double r;
    __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"(v));
    int e = (int)r;
    if (r < 0 && (long double)e != r)
        e--;
    return e;
}

static int gen_digits(long double v, int ndig, char *digits, int *exp10)
{
    int e = floor_log10(v);
    long double s = e >= 0 ? v / pow10l_int(e) : v * pow10l_int(-e);
    if (s >= 10.0L) {
        s /= 10.0L;
        e++;
    } else if (s < 1.0L) {
        s *= 10.0L;
        e--;
    }
    if (ndig < 0) {
        *exp10 = e;
        return 0;
    }
    int n = MIN(ndig, MAX_DIGITS);
    char raw[MAX_DIGITS + 2];
    for (int i = 0; i <= n; i++) {
        if (i < EXACT_DIGITS) {
            int d = (int)s;
            if (d > 9)
                d = 9;
            raw[i] = (char)d;
            s = (s - d) * 10.0L;
        } else {
            raw[i] = 0;
        }
    }
    if (raw[n] >= 5) {
        int i = n - 1;
        while (i >= 0 && raw[i] == 9)
            raw[i--] = 0;
        if (i >= 0) {
            raw[i]++;
        } else {
            memmove(raw + 1, raw, (size_t)n);
            raw[0] = 1;
            e++;
            if (n == 0) {
                digits[0] = '1';
                *exp10 = e;
                return 1;
            }
        }
    }
    for (int i = 0; i < n; i++)
        digits[i] = (char)('0' + raw[i]);
    *exp10 = e;
    return n;
}

static int fmt_fixed(long double v, int prec, bool alt, char *out)
{
    char digits[MAX_DIGITS + 2];
    int e = 0, n = 0, len = 0;
    if (v != 0.0L) {
        int probe;
        gen_digits(v, -1, digits, &probe);
        int want = probe + 1 + prec;
        if (want >= 0)
            n = gen_digits(v, want, digits, &e);
        if (want < 0 || n == 0) {
            if (want == 0 && n == 1) {
                n = 1;
            } else {
                n = 0;
                e = 0;
            }
        }
    }
    if (n == 0) {
        out[len++] = '0';
    } else if (e < 0) {
        out[len++] = '0';
    } else {
        for (int i = 0; i <= e; i++)
            out[len++] = i < n ? digits[i] : '0';
    }
    if (prec > 0 || alt)
        out[len++] = '.';
    for (int k = 0; k < prec; k++) {
        int pos = e + 1 + k;
        out[len++] = n && pos >= 0 && pos < n ? digits[pos] : '0';
    }
    out[len] = 0;
    return len;
}

static int put_exponent(char *out, int e, bool upper)
{
    int len = 0;
    out[len++] = upper ? 'E' : 'e';
    out[len++] = e < 0 ? '-' : '+';
    unsigned u = (unsigned)(e < 0 ? -e : e);
    char tmp[8];
    int t = 0;
    do
        tmp[t++] = (char)('0' + u % 10);
    while ((u /= 10));
    while (t < exponent_digits)
        tmp[t++] = '0';
    while (t)
        out[len++] = tmp[--t];
    return len;
}

static int fmt_exp(long double v, int prec, bool alt, bool upper, char *out)
{
    char digits[MAX_DIGITS + 2];
    int e = 0, n = 0, len = 0;
    if (v != 0.0L)
        n = gen_digits(v, prec + 1, digits, &e);
    out[len++] = n ? digits[0] : '0';
    if (prec > 0 || alt)
        out[len++] = '.';
    for (int i = 1; i <= prec; i++)
        out[len++] = i < n ? digits[i] : '0';
    len += put_exponent(out + len, e, upper);
    out[len] = 0;
    return len;
}

static int strip_zeros(char *s, int len)
{
    char *dot = strchr(s, '.');
    if (!dot)
        return len;
    char *exp = strchr(s, 'e');
    if (!exp)
        exp = strchr(s, 'E');
    char tail[16] = "";
    if (exp) {
        str_copy(tail, exp, sizeof(tail));
        *exp = 0;
        len = (int)(exp - s);
    }
    while (len > 0 && s[len - 1] == '0')
        s[--len] = 0;
    if (len > 0 && s[len - 1] == '.')
        s[--len] = 0;
    str_copy(s + len, tail, 16);
    return (int)strlen(s);
}

static int fmt_general(long double v, int prec, bool alt, bool upper, char *out)
{
    if (prec == 0)
        prec = 1;
    int e = 0;
    if (v != 0.0L) {
        char digits[MAX_DIGITS + 2];
        gen_digits(v, prec, digits, &e);
    }
    int len;
    if (e < -4 || e >= prec)
        len = fmt_exp(v, prec - 1, alt, upper, out);
    else
        len = fmt_fixed(v, prec - 1 - e, alt, out);
    return alt ? len : strip_zeros(out, len);
}

static int fmt_hex(double d, int prec, bool upper, char *out)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    int e = (int)((bits >> 52) & 0x7ff);
    uint64_t mant = bits & 0xfffffffffffffull;
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;
    out[len++] = '0';
    out[len++] = upper ? 'X' : 'x';
    out[len++] = e ? '1' : '0';
    if (e == 0 && mant == 0)
        e = 1023;
    else if (e == 0)
        e = 1;
    if (prec < 0) {
        prec = 13;
        while (prec > 0 && !((mant >> (4 * (13 - prec))) & 0xf))
            prec--;
    }
    if (prec > 0)
        out[len++] = '.';
    for (int i = 0; i < prec; i++)
        out[len++] = i < 13 ? hex[(mant >> (48 - 4 * i)) & 0xf] : '0';
    out[len++] = upper ? 'P' : 'p';
    int ex = e - 1023;
    out[len++] = ex < 0 ? '-' : '+';
    unsigned u = (unsigned)(ex < 0 ? -ex : ex);
    char tmp[8];
    int t = 0;
    do
        tmp[t++] = (char)('0' + u % 10);
    while ((u /= 10));
    while (t)
        out[len++] = tmp[--t];
    out[len] = 0;
    return len;
}

static void emit_number(struct out *o, const char *prefix, const char *body, int body_len, int width, int flags,
                        int zero_extra)
{
    int plen = (int)strlen(prefix);
    int total = plen + zero_extra + body_len;
    if (!(flags & F_LEFT) && !(flags & F_ZERO))
        out_pad(o, ' ', width - total);
    out_chars(o, prefix, (size_t)plen);
    if (!(flags & F_LEFT) && (flags & F_ZERO))
        out_pad(o, '0', width - total);
    out_pad(o, '0', zero_extra);
    out_chars(o, body, (size_t)body_len);
    if (flags & F_LEFT)
        out_pad(o, ' ', width - total);
}

static void emit_float(struct out *o, double d, char conv, int prec, int width, int flags)
{
    char buf[512];
    char prefix[4] = "";
    uint64_t bits;
    memcpy(&bits, &d, 8);
    bool neg = bits >> 63;
    if (neg)
        str_copy(prefix, "-", sizeof(prefix));
    else if (flags & F_PLUS)
        str_copy(prefix, "+", sizeof(prefix));
    else if (flags & F_SPACE)
        str_copy(prefix, " ", sizeof(prefix));
    bool upper = conv == 'E' || conv == 'F' || conv == 'G' || conv == 'A';
    int len;
    if (((bits >> 52) & 0x7ff) == 0x7ff) {
        bool nan = bits & 0xfffffffffffffull;
        str_copy(buf, nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), sizeof(buf));
        emit_number(o, prefix, buf, 3, width, flags & ~F_ZERO, 0);
        return;
    }
    long double v = neg ? -(long double)d : (long double)d;
    if (prec > 350)
        prec = 350;
    switch (conv) {
    case 'f':
    case 'F':
        len = fmt_fixed(v, prec < 0 ? 6 : prec, flags & F_ALT, buf);
        break;
    case 'e':
    case 'E':
        len = fmt_exp(v, prec < 0 ? 6 : prec, flags & F_ALT, upper, buf);
        break;
    case 'a':
    case 'A':
        len = fmt_hex(neg ? -d : d, prec, upper, buf);
        break;
    default:
        len = fmt_general(v, prec < 0 ? 6 : prec, flags & F_ALT, upper, buf);
        break;
    }
    emit_number(o, prefix, buf, len, width, flags, 0);
}

static int utoa_buf(uint64_t v, unsigned base, bool upper, char *end)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char *p = end;
    if (v <= 0xffffffffu) {
        uint32_t x = (uint32_t)v;
        do
            *--p = digits[x % base];
        while ((x /= base));
    } else {
        do
            *--p = digits[v % base];
        while ((v /= base));
    }
    return (int)(end - p);
}

static void emit_int(struct out *o, uint64_t v, bool neg, unsigned base, char conv, int prec, int width, int flags)
{
    char buf[72];
    char *end = buf + sizeof(buf);
    int len = (v == 0 && prec == 0) ? 0 : utoa_buf(v, base, conv == 'X', end);
    char prefix[4] = "";
    if (neg)
        str_copy(prefix, "-", sizeof(prefix));
    else if ((conv == 'd' || conv == 'i') && (flags & F_PLUS))
        str_copy(prefix, "+", sizeof(prefix));
    else if ((conv == 'd' || conv == 'i') && (flags & F_SPACE))
        str_copy(prefix, " ", sizeof(prefix));
    else if ((flags & F_ALT) && v && base == 16)
        str_copy(prefix, conv == 'X' ? "0X" : "0x", sizeof(prefix));
    int zero_extra = prec > len ? prec - len : 0;
    if ((flags & F_ALT) && base == 8 && zero_extra == 0 && (len == 0 || end[-len] != '0'))
        zero_extra = 1;
    if (prec >= 0)
        flags &= ~F_ZERO;
    emit_number(o, prefix, end - len, len, width, flags, zero_extra);
}

static void emit_text(struct out *o, const char *s, const WCHAR *w, int prec, int width, int flags)
{
    size_t n;
    if (w) {
        n = 0;
        while ((prec < 0 || (int)n < prec) && w[n])
            n++;
    } else {
        n = prec < 0 ? strlen(s) : strnlen(s, (size_t)prec);
    }
    if (!(flags & F_LEFT))
        out_pad(o, ' ', width - (int)n);
    if (w)
        out_wchars(o, w, n);
    else
        out_chars(o, s, n);
    if (flags & F_LEFT)
        out_pad(o, ' ', width - (int)n);
}

static int format_core(struct out *o, const void *fmt, bool wide_fmt, va_list *ap)
{
    const uint8_t *nf = fmt;
    const WCHAR *wf = fmt;
    size_t i = 0;
#define FC(k) (wide_fmt ? (unsigned)wf[(k)] : (unsigned)nf[(k)])
    while (FC(i)) {
        if (FC(i) != '%') {
            size_t start = i;
            while (FC(i) && FC(i) != '%')
                i++;
            if (wide_fmt)
                out_wchars(o, wf + start, i - start);
            else
                out_chars(o, (const char *)nf + start, i - start);
            continue;
        }
        i++;
        int flags = 0, width = 0, prec = -1, len = L_NONE;
        for (;; i++) {
            unsigned c = FC(i);
            if (c == '-')
                flags |= F_LEFT;
            else if (c == '+')
                flags |= F_PLUS;
            else if (c == ' ')
                flags |= F_SPACE;
            else if (c == '#')
                flags |= F_ALT;
            else if (c == '0')
                flags |= F_ZERO;
            else if (c != '\'')
                break;
        }
        if (FC(i) == '*') {
            width = ARG_INT(ap);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            i++;
        } else {
            while (FC(i) >= '0' && FC(i) <= '9')
                width = width * 10 + (int)(FC(i++) - '0');
        }
        if (FC(i) == '.') {
            i++;
            prec = 0;
            if (FC(i) == '*') {
                prec = ARG_INT(ap);
                i++;
            } else {
                while (FC(i) >= '0' && FC(i) <= '9')
                    prec = prec * 10 + (int)(FC(i++) - '0');
            }
        }
        for (bool more = true; more;) {
            switch (FC(i)) {
            case 'h':
                len = FC(i + 1) == 'h' ? (i++, L_HH) : L_H;
                i++;
                break;
            case 'l':
                len = FC(i + 1) == 'l' ? (i++, L_LL) : L_L;
                i++;
                break;
            case 'L':
            case 'q':
            case 'j':
                len = L_LL;
                i++;
                break;
            case 'w':
                len = L_W;
                i++;
                break;
            case 'z':
            case 't':
                if (crt_slots64)
                    len = L_LL;
                i++;
                break;
            case 'I':
                if (FC(i + 1) == '6' && FC(i + 2) == '4') {
                    len = L_LL;
                    i += 3;
                } else if (FC(i + 1) == '3' && FC(i + 2) == '2') {
                    i += 3;
                } else {
                    if (crt_slots64)
                        len = L_LL;
                    i++;
                }
                break;
            default:
                more = false;
            }
        }
        unsigned conv = FC(i);
        if (!conv)
            break;
        i++;
        switch (conv) {
        case 'd':
        case 'i': {
            int64_t v = len == L_LL ? va_arg(*ap, int64_t) : (int64_t)ARG_INT(ap);
            if (len == L_H)
                v = (short)v;
            else if (len == L_HH)
                v = (signed char)v;
            emit_int(o, v < 0 ? (uint64_t)-v : (uint64_t)v, v < 0, 10, 'd', prec, width, flags);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uint64_t v = len == L_LL ? va_arg(*ap, uint64_t) : (uint64_t)ARG_UINT(ap);
            if (len == L_H)
                v = (uint16_t)v;
            else if (len == L_HH)
                v = (uint8_t)v;
            emit_int(o, v, false, conv == 'u' ? 10 : conv == 'o' ? 8 : 16, (char)conv, prec, width, flags);
            break;
        }
        case 'p':
            if (crt_slots64)
                emit_int(o, va_arg(*ap, uint64_t), false, 16, 'X', 16, width, flags & ~F_ALT);
            else
                emit_int(o, (uintptr_t)va_arg(*ap, void *), false, 16, 'X', 8, width, flags & ~F_ALT);
            break;
        case 'c':
        case 'C': {
            bool wide = conv == 'c' ? (wide_fmt ? len != L_H : len == L_L || len == L_W)
                                    : (wide_fmt ? len == L_L || len == L_W : len != L_H);
            int v = ARG_INT(ap);
            WCHAR wc[2] = {(WCHAR)v, 0};
            char nc[2] = {(char)v, 0};
            if (!(flags & F_LEFT))
                out_pad(o, ' ', width - 1);
            if (wide)
                out_wchars(o, wc, 1);
            else
                out_chars(o, nc, 1);
            if (flags & F_LEFT)
                out_pad(o, ' ', width - 1);
            break;
        }
        case 's':
        case 'S':
        case 'Z': {
            bool wide = conv == 's' ? (wide_fmt ? len != L_H : len == L_L || len == L_W)
                                    : (wide_fmt ? len == L_L || len == L_W : len != L_H);
            void *p = ARG_PTR(ap);
            if (!p)
                emit_text(o, wide ? NULL : "(null)", wide ? (const WCHAR[]){'(', 'n', 'u', 'l', 'l', ')', 0} : NULL,
                          prec, width, flags);
            else
                emit_text(o, wide ? NULL : p, wide ? p : NULL, prec, width, flags);
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            emit_float(o, va_arg(*ap, double), (char)conv, prec, width, flags);
            break;
        case 'n': {
            void *p = ARG_PTR(ap);
            if (len == L_H)
                *(short *)p = (short)o->count;
            else if (len == L_LL)
                *(int64_t *)p = o->count;
            else
                *(int *)p = o->count;
            break;
        }
        case '%':
            out_chars(o, "%", 1);
            break;
        default:
            if (wide_fmt)
                out_wchars(o, wf + i - 1, 1);
            else
                out_chars(o, (const char *)nf + i - 1, 1);
            break;
        }
    }
#undef FC
    return o->count;
}

int crt_vformat(crt_sink sink, void *ctx, const char *fmt, va_list ap)
{
    struct out o = {sink, NULL, ctx, 0};
    va_list copy;
    va_copy(copy, ap);
    int n = format_core(&o, fmt, false, &copy);
    va_end(copy);
    return n;
}

int crt_vwformat(crt_wsink sink, void *ctx, const WCHAR *fmt, va_list ap)
{
    struct out o = {NULL, sink, ctx, 0};
    va_list copy;
    va_copy(copy, ap);
    int n = format_core(&o, fmt, true, &copy);
    va_end(copy);
    return n;
}

struct strbuf {
    char *buf;
    WCHAR *wbuf;
    size_t cap;
    size_t len;
};

static void str_sink(void *ctx, const char *s, size_t n)
{
    struct strbuf *b = ctx;
    for (size_t i = 0; i < n; i++, b->len++)
        if (b->buf && b->len < b->cap)
            b->buf[b->len] = s[i];
}

static void wstr_sink(void *ctx, const WCHAR *s, size_t n)
{
    struct strbuf *b = ctx;
    for (size_t i = 0; i < n; i++, b->len++)
        if (b->wbuf && b->len < b->cap)
            b->wbuf[b->len] = s[i];
}

static int vsn(char *buf, size_t cap, const char *fmt, va_list ap, bool c99)
{
    struct strbuf b = {buf, NULL, cap, 0};
    int n = crt_vformat(str_sink, &b, fmt, ap);
    if (!buf)
        return n;
    if ((size_t)n < cap)
        buf[n] = 0;
    else if (c99 && cap)
        buf[cap - 1] = 0;
    if (!c99 && (size_t)n > cap)
        return -1;
    return n;
}

static int vswn(WCHAR *buf, size_t cap, const WCHAR *fmt, va_list ap, bool c99)
{
    struct strbuf b = {NULL, buf, cap, 0};
    int n = crt_vwformat(wstr_sink, &b, fmt, ap);
    if (!buf)
        return n;
    if ((size_t)n < cap)
        buf[n] = 0;
    else if (c99 && cap)
        buf[cap - 1] = 0;
    if ((size_t)n >= cap)
        return -1;
    return n;
}

static int crt_vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsn(buf, (size_t)INT32_MAX, fmt, ap, true);
}

static int crt_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vsnprintf_ms(char *buf, size_t cap, const char *fmt, va_list ap)
{
    return vsn(buf, cap, fmt, ap, false);
}

static int crt_snprintf_ms(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsn(buf, cap, fmt, ap, false);
    va_end(ap);
    return n;
}

static int crt_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    return vsn(buf, cap, fmt, ap, true);
}

static int crt_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsn(buf, cap, fmt, ap, true);
    va_end(ap);
    return n;
}

static int crt_vsprintf_s(char *buf, size_t cap, const char *fmt, va_list ap)
{
    int n = vsn(buf, cap, fmt, ap, false);
    if (n < 0 && cap)
        buf[0] = 0;
    return n;
}

static int crt_sprintf_s(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vsprintf_s(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vsnprintf_s(char *buf, size_t cap, size_t count, const char *fmt, va_list ap)
{
    size_t limit = count == (size_t)-1 ? cap - 1 : MIN(count, cap - 1);
    int n = vsn(buf, limit, fmt, ap, false);
    buf[n < 0 ? limit : (size_t)n] = 0;
    return n;
}

static int crt_snprintf_s(char *buf, size_t cap, size_t count, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vsnprintf_s(buf, cap, count, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vscprintf(const char *fmt, va_list ap)
{
    return vsn(NULL, 0, fmt, ap, true);
}

static int crt_scprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vscprintf(fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vswprintf_old(WCHAR *buf, const WCHAR *fmt, va_list ap)
{
    return vswn(buf, (size_t)INT32_MAX, fmt, ap, true);
}

static int crt_swprintf_old(WCHAR *buf, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vswprintf_old(buf, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vsnwprintf(WCHAR *buf, size_t cap, const WCHAR *fmt, va_list ap)
{
    return vswn(buf, cap, fmt, ap, false);
}

static int crt_snwprintf(WCHAR *buf, size_t cap, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vswn(buf, cap, fmt, ap, false);
    va_end(ap);
    return n;
}

static int crt_vswprintf_s(WCHAR *buf, size_t cap, const WCHAR *fmt, va_list ap)
{
    return vswn(buf, cap, fmt, ap, true);
}

static int crt_swprintf_s(WCHAR *buf, size_t cap, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vswn(buf, cap, fmt, ap, true);
    va_end(ap);
    return n;
}

static int crt_vscwprintf(const WCHAR *fmt, va_list ap)
{
    return vswn(NULL, 0, fmt, ap, true);
}

static int crt_scwprintf(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vscwprintf(fmt, ap);
    va_end(ap);
    return n;
}

static int crt_set_output_format(int format)
{
    int old = exponent_digits == 2 ? 1 : 0;
    exponent_digits = format & 1 ? 2 : 3;
    return old;
}

static long double parse_decimal(const char *s, const char **end, bool *range)
{
    uint64_t mant = 0;
    int digits = 0, exp10 = 0;
    bool any = false;
    const char *p = s;
    while (is_digit(*p)) {
        any = true;
        if (digits < EXACT_DIGITS) {
            mant = mant * 10 + (uint64_t)(*p - '0');
            if (mant)
                digits++;
        } else {
            exp10++;
        }
        p++;
    }
    if (*p == '.') {
        p++;
        while (is_digit(*p)) {
            any = true;
            if (digits < EXACT_DIGITS) {
                mant = mant * 10 + (uint64_t)(*p - '0');
                if (mant)
                    digits++;
                exp10--;
            }
            p++;
        }
    }
    if (!any) {
        *end = s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        bool eneg = false;
        if (*q == '+' || *q == '-')
            eneg = *q++ == '-';
        if (is_digit(*q)) {
            int ev = 0;
            while (is_digit(*q)) {
                if (ev < 100000)
                    ev = ev * 10 + (*q - '0');
                q++;
            }
            exp10 += eneg ? -ev : ev;
            p = q;
        }
    }
    *end = p;
    if (!mant)
        return 0;
    if (exp10 + digits > 310) {
        *range = true;
        return 1e309L;
    }
    if (exp10 + digits < -330) {
        *range = true;
        return 0;
    }
    long double v = (long double)mant;
    return exp10 >= 0 ? v * pow10l_int(exp10) : v / pow10l_int(-exp10);
}

static long double parse_hex(const char *s, const char **end)
{
    long double v = 0;
    int exp2 = 0;
    bool any = false;
    const char *p = s;
    while (is_xdigit(*p)) {
        v = v * 16 + (is_digit(*p) ? *p - '0' : (to_lower(*p) - 'a' + 10));
        any = true;
        p++;
    }
    if (*p == '.') {
        p++;
        while (is_xdigit(*p)) {
            v = v * 16 + (is_digit(*p) ? *p - '0' : (to_lower(*p) - 'a' + 10));
            exp2 -= 4;
            any = true;
            p++;
        }
    }
    if (!any) {
        *end = s;
        return 0;
    }
    if (*p == 'p' || *p == 'P') {
        const char *q = p + 1;
        bool neg = false;
        if (*q == '+' || *q == '-')
            neg = *q++ == '-';
        if (is_digit(*q)) {
            int ev = 0;
            while (is_digit(*q))
                ev = MIN(ev * 10 + (*q++ - '0'), 100000);
            exp2 += neg ? -ev : ev;
            p = q;
        }
    }
    *end = p;
    while (exp2 > 0) {
        v *= 2;
        exp2--;
    }
    while (exp2 < 0) {
        v /= 2;
        exp2++;
    }
    return v;
}

static double crt_strtod(const char *s, char **endp)
{
    const char *p = s;
    while (is_space(*p))
        p++;
    bool neg = false;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    long double v;
    const char *end;
    bool range = false;
    if (str_ncasecmp(p, "inf", 3) == 0) {
        end = p + (str_ncasecmp(p, "infinity", 8) == 0 ? 8 : 3);
        v = __builtin_infl();
    } else if (str_ncasecmp(p, "nan", 3) == 0) {
        end = p + 3;
        if (*end == '(') {
            const char *q = end + 1;
            while (is_alnum(*q) || *q == '_')
                q++;
            if (*q == ')')
                end = q + 1;
        }
        v = __builtin_nanl("");
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        v = parse_hex(p + 2, &end);
        if (end == p + 2)
            end = p + 1;
    } else {
        v = parse_decimal(p, &end, &range);
        if (end == p)
            end = s;
    }
    if (endp)
        *endp = (char *)end;
    double d = (double)(neg ? -v : v);
    if (range || (v != 0 && (d == 0 || __builtin_isinf(d)) && !__builtin_isinf(v)))
        crt_set_errno(CRT_ERANGE);
    return d;
}

static float crt_strtof(const char *s, char **end)
{
    return (float)crt_strtod(s, end);
}

static double crt_atof(const char *s)
{
    return crt_strtod(s, NULL);
}

static double crt_wcstod(const WCHAR *s, WCHAR **end)
{
    char tmp[128];
    size_t i = 0;
    for (; s[i] && i < sizeof(tmp) - 1; i++)
        tmp[i] = s[i] < 0x80 ? (char)s[i] : '?';
    tmp[i] = 0;
    char *e;
    double d = crt_strtod(tmp, &e);
    if (end)
        *end = (WCHAR *)s + (e - tmp);
    return d;
}

static double crt_wtof(const WCHAR *s)
{
    return crt_wcstod(s, NULL);
}

static int crt_atodbl(double *out, char *s)
{
    *out = crt_strtod(s, NULL);
    return 0;
}

static char *crt_gcvt(double v, int digits, char *buf)
{
    struct strbuf b = {buf, NULL, 64, 0};
    struct out o = {str_sink, NULL, &b, 0};
    emit_float(&o, v, 'g', digits, 0, 0);
    buf[b.len] = 0;
    return buf;
}

static char *cvt(double v, int n, int *decpt, int *sign, bool fixed)
{
    static char buf[400];
    char digits[MAX_DIGITS + 2];
    long double x = v < 0 ? -(long double)v : v;
    *sign = v < 0;
    if (x == 0) {
        int k = fixed ? MAX(n, 0) : MAX(n, 1);
        memset(buf, '0', (size_t)MIN(k, 399));
        buf[MIN(k, 399)] = 0;
        *decpt = fixed ? 0 : 1;
        return buf;
    }
    int e, probe;
    gen_digits(x, -1, digits, &probe);
    int want = fixed ? probe + 1 + n : n;
    int got = want > 0 ? gen_digits(x, MIN(want, MAX_DIGITS), digits, &e) : 0;
    if (!got)
        e = probe;
    int len = 0;
    for (; len < want && len < 399; len++)
        buf[len] = len < got ? digits[len] : '0';
    buf[len] = 0;
    *decpt = e + 1;
    return buf;
}

static char *crt_ecvt(double v, int n, int *decpt, int *sign)
{
    return cvt(v, n, decpt, sign, false);
}

static char *crt_fcvt(double v, int n, int *decpt, int *sign)
{
    return cvt(v, n, decpt, sign, true);
}

struct scan_src {
    int (*get)(void *);
    void (*unget)(int, void *);
    void *ctx;
    int consumed;
};

static int sget(struct scan_src *s)
{
    int c = s->get(s->ctx);
    if (c >= 0)
        s->consumed++;
    return c;
}

static void sunget(struct scan_src *s, int c)
{
    if (c >= 0) {
        s->unget(c, s->ctx);
        s->consumed--;
    }
}

static void store_int(void *p, int len, uint64_t v)
{
    switch (len) {
    case L_HH:
        *(uint8_t *)p = (uint8_t)v;
        break;
    case L_H:
        *(uint16_t *)p = (uint16_t)v;
        break;
    case L_LL:
        *(uint64_t *)p = v;
        break;
    default:
        *(uint32_t *)p = (uint32_t)v;
        break;
    }
}

static int scan_core(struct scan_src *src, const void *fmt, bool wide_fmt, va_list *ap)
{
    const uint8_t *nf = fmt;
    const WCHAR *wf = fmt;
    size_t i = 0;
    int assigned = 0;
    bool any_input = false;
#define FC(k) (wide_fmt ? (unsigned)wf[(k)] : (unsigned)nf[(k)])
    while (FC(i)) {
        unsigned f = FC(i);
        if (is_space((int)f)) {
            int c;
            while ((c = sget(src)) >= 0 && is_space(c))
                ;
            sunget(src, c);
            while (is_space((int)FC(i)))
                i++;
            continue;
        }
        if (f != '%' || FC(i + 1) == '%') {
            if (f == '%')
                i++;
            int c = sget(src);
            if (c < 0)
                return any_input || assigned ? assigned : -1;
            if ((unsigned)c != f) {
                sunget(src, c);
                return assigned;
            }
            any_input = true;
            i++;
            continue;
        }
        i++;
        bool suppress = false;
        int width = 0, len = L_NONE;
        if (FC(i) == '*') {
            suppress = true;
            i++;
        }
        while (FC(i) >= '0' && FC(i) <= '9')
            width = width * 10 + (int)(FC(i++) - '0');
        for (bool more = true; more;) {
            switch (FC(i)) {
            case 'h':
                len = FC(i + 1) == 'h' ? (i++, L_HH) : L_H;
                i++;
                break;
            case 'l':
                len = FC(i + 1) == 'l' ? (i++, L_LL) : L_L;
                i++;
                break;
            case 'L':
            case 'q':
            case 'j':
                len = L_LL;
                i++;
                break;
            case 'w':
                len = L_W;
                i++;
                break;
            case 'z':
            case 't':
                if (crt_slots64)
                    len = L_LL;
                i++;
                break;
            case 'I':
                if (FC(i + 1) == '6' && FC(i + 2) == '4') {
                    len = L_LL;
                    i += 3;
                } else if (FC(i + 1) == '3' && FC(i + 2) == '2') {
                    i += 3;
                } else {
                    if (crt_slots64)
                        len = L_LL;
                    i++;
                }
                break;
            default:
                more = false;
            }
        }
        unsigned conv = FC(i++);
        if (!conv)
            break;
        if (conv == 'n') {
            if (!suppress)
                store_int(ARG_PTR(ap), len, (uint64_t)src->consumed);
            continue;
        }
        if (conv != 'c' && conv != '[' && conv != 'C') {
            int c;
            while ((c = sget(src)) >= 0 && is_space(c))
                ;
            if (c < 0)
                return any_input || assigned ? assigned : -1;
            sunget(src, c);
        }
        if (!width)
            width = conv == 'c' || conv == 'C' ? 1 : 0x7fffffff;
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            char buf[80];
            int n = 0, c = sget(src);
            if ((c == '+' || c == '-') && n < width) {
                buf[n++] = (char)c;
                c = sget(src);
            }
            int base = conv == 'o' ? 8 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'i' ? 0 : 10;
            if ((base == 0 || base == 16) && c == '0' && n < width) {
                buf[n++] = (char)c;
                c = sget(src);
                if ((c == 'x' || c == 'X') && n < width) {
                    buf[n++] = (char)c;
                    c = sget(src);
                    base = 16;
                } else if (base == 0) {
                    base = 8;
                }
            }
            if (base == 0)
                base = 10;
            while (c >= 0 && n < width && n < (int)sizeof(buf) - 1 &&
                   (base == 16 ? is_xdigit(c) : base == 8 ? (c >= '0' && c <= '7') : is_digit(c))) {
                buf[n++] = (char)c;
                c = sget(src);
            }
            sunget(src, c);
            buf[n] = 0;
            if (!n || ((buf[0] == '+' || buf[0] == '-') && n == 1))
                return assigned;
            any_input = true;
            if (!suppress) {
                uint64_t v = conv == 'd' || conv == 'i' ? (uint64_t)crt_strtoi64(buf, NULL, base)
                                                        : crt_strtoui64(buf, NULL, base);
                store_int(ARG_PTR(ap), conv == 'p' ? (crt_slots64 ? L_LL : L_NONE) : len, v);
                assigned++;
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            char buf[128];
            int n = 0, c = sget(src);
            while (c >= 0 && n < width && n < (int)sizeof(buf) - 1 &&
                   (is_alnum(c) || c == '.' || c == '+' || c == '-')) {
                if ((c == '+' || c == '-') && n > 0 && to_lower(buf[n - 1]) != 'e' && to_lower(buf[n - 1]) != 'p')
                    break;
                buf[n++] = (char)c;
                c = sget(src);
            }
            buf[n] = 0;
            char *end;
            double d = crt_strtod(buf, &end);
            int extra = n - (int)(end - buf);
            sunget(src, c);
            while (extra-- > 0)
                sunget(src, (uint8_t)buf[--n]);
            if (end == buf)
                return assigned;
            any_input = true;
            if (!suppress) {
                if (len == L_L || len == L_LL)
                    *(double *)ARG_PTR(ap) = d;
                else
                    *(float *)ARG_PTR(ap) = (float)d;
                assigned++;
            }
            break;
        }
        case 'c':
        case 'C':
        case 's':
        case 'S':
        case '[': {
            bool set[256] = {0};
            bool invert = false;
            if (conv == '[') {
                if (FC(i) == '^') {
                    invert = true;
                    i++;
                }
                if (FC(i) == ']')
                    set[']'] = true, i++;
                while (FC(i) && FC(i) != ']') {
                    unsigned a = FC(i) & 0xff;
                    if (FC(i + 1) == '-' && FC(i + 2) && FC(i + 2) != ']') {
                        for (unsigned k = a; k <= (FC(i + 2) & 0xff); k++)
                            set[k] = true;
                        i += 3;
                    } else {
                        set[a] = true;
                        i++;
                    }
                }
                if (FC(i) == ']')
                    i++;
            }
            bool lower = conv == 'c' || conv == 's' || conv == '[';
            bool wide = lower ? (wide_fmt ? len != L_H : len == L_L || len == L_W)
                              : (wide_fmt ? len == L_L || len == L_W : len != L_H);
            void *dst = suppress ? NULL : ARG_PTR(ap);
            int n = 0, c = sget(src);
            while (c >= 0 && n < width) {
                if ((conv == 's' || conv == 'S') && is_space(c))
                    break;
                if (conv == '[' && set[c & 0xff] == invert)
                    break;
                if (dst) {
                    if (wide) {
                        char ch = (char)c;
                        w32_cp_to_wide(&ch, 1, (WCHAR *)dst + n, 1, 0);
                    } else {
                        ((char *)dst)[n] = (char)c;
                    }
                }
                n++;
                c = sget(src);
            }
            sunget(src, c);
            if (!n)
                return any_input || c >= 0 ? assigned : (assigned ? assigned : -1);
            any_input = true;
            if (dst) {
                if (conv != 'c' && conv != 'C') {
                    if (wide)
                        ((WCHAR *)dst)[n] = 0;
                    else
                        ((char *)dst)[n] = 0;
                }
                assigned++;
            }
            break;
        }
        default:
            return assigned;
        }
    }
#undef FC
    return assigned;
}

int crt_vscan(int (*get)(void *), void (*unget)(int, void *), void *ctx, const char *fmt, va_list ap)
{
    struct scan_src src = {get, unget, ctx, 0};
    va_list copy;
    va_copy(copy, ap);
    int n = scan_core(&src, fmt, false, &copy);
    va_end(copy);
    return n;
}

struct str_src {
    const uint8_t *s;
    const WCHAR *w;
    size_t pos;
};

static int str_get(void *ctx)
{
    struct str_src *s = ctx;
    if (s->w) {
        WCHAR c = s->w[s->pos];
        if (!c)
            return -1;
        s->pos++;
        char out;
        w32_wide_to_cp(&c, 1, &out, 1, 0);
        return (uint8_t)out;
    }
    return s->s[s->pos] ? s->s[s->pos++] : -1;
}

static void str_unget(int c, void *ctx)
{
    UNUSED(c);
    struct str_src *s = ctx;
    if (s->pos)
        s->pos--;
}

static int crt_vsscanf(const char *str, const char *fmt, va_list ap)
{
    struct str_src s = {(const uint8_t *)str, NULL, 0};
    struct scan_src src = {str_get, str_unget, &s, 0};
    va_list copy;
    va_copy(copy, ap);
    int n = scan_core(&src, fmt, false, &copy);
    va_end(copy);
    return n;
}

static int crt_sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vsscanf(str, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_vswscanf(const WCHAR *str, const WCHAR *fmt, va_list ap)
{
    struct str_src s = {NULL, str, 0};
    struct scan_src src = {str_get, str_unget, &s, 0};
    va_list copy;
    va_copy(copy, ap);
    int n = scan_core(&src, fmt, true, &copy);
    va_end(copy);
    return n;
}

static int crt_swscanf(const WCHAR *str, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vswscanf(str, fmt, ap);
    va_end(ap);
    return n;
}

static int file_get(void *ctx)
{
    return crt_fgetc(ctx);
}

static void file_unget(int c, void *ctx)
{
    crt_ungetc(c, ctx);
}

static int crt_vfwscanf(CRT_FILE *f, const WCHAR *fmt, va_list ap)
{
    crt_flush_all();
    struct scan_src src = {file_get, file_unget, f, 0};
    va_list copy;
    va_copy(copy, ap);
    int n = scan_core(&src, fmt, true, &copy);
    va_end(copy);
    return n;
}

static int crt_fwscanf(CRT_FILE *f, const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfwscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int crt_wscanf(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = crt_vfwscanf(&crt_iob[0], fmt, ap);
    va_end(ap);
    return n;
}

static const struct w32_export format_exports[] = {
    W32_AS("sprintf", crt_sprintf), W32_AS("vsprintf", crt_vsprintf), W32_AS("_snprintf", crt_snprintf_ms),
    W32_AS("_vsnprintf", crt_vsnprintf_ms), W32_AS("snprintf", crt_snprintf), W32_AS("vsnprintf", crt_vsnprintf),
    W32_AS("sprintf_s", crt_sprintf_s), W32_AS("vsprintf_s", crt_vsprintf_s), W32_AS("_snprintf_s", crt_snprintf_s),
    W32_AS("_vsnprintf_s", crt_vsnprintf_s), W32_AS("_scprintf", crt_scprintf), W32_AS("_vscprintf", crt_vscprintf),
    W32_AS("swprintf", crt_swprintf_old), W32_AS("_swprintf", crt_swprintf_old),
    W32_AS("vswprintf", crt_vswprintf_old), W32_AS("_vswprintf", crt_vswprintf_old),
    W32_AS("_snwprintf", crt_snwprintf), W32_AS("_vsnwprintf", crt_vsnwprintf),
    W32_AS("swprintf_s", crt_swprintf_s), W32_AS("vswprintf_s", crt_vswprintf_s),
    W32_AS("_scwprintf", crt_scwprintf), W32_AS("_vscwprintf", crt_vscwprintf),
    W32_AS("_set_output_format", crt_set_output_format), W32_AS("strtod", crt_strtod),
    W32_AS("strtof", crt_strtof), W32_AS("atof", crt_atof), W32_AS("wcstod", crt_wcstod), W32_AS("_wtof", crt_wtof),
    W32_AS("_atodbl", crt_atodbl), W32_AS("_gcvt", crt_gcvt), W32_AS("_ecvt", crt_ecvt), W32_AS("_fcvt", crt_fcvt),
    W32_AS("sscanf", crt_sscanf), W32_AS("vsscanf", crt_vsscanf), W32_AS("swscanf", crt_swscanf),
    W32_AS("vswscanf", crt_vswscanf), W32_AS("fwscanf", crt_fwscanf), W32_AS("vfwscanf", crt_vfwscanf),
    W32_AS("wscanf", crt_wscanf),
};

static void format_attach(void)
{
    exponent_digits = 3;
}

W32_EXPORTS(crt_format_module, "msvcrt.dll", format_exports, .attach = format_attach);
