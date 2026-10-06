#include <synth/fmt.h>
#include <synth/string.h>

enum {
    F_LEFT = 1 << 0,
    F_ZERO = 1 << 1,
    F_PLUS = 1 << 2,
    F_SPACE = 1 << 3,
    F_ALT = 1 << 4,
    F_UPPER = 1 << 5,
};

struct out {
    fmt_sink_fn sink;
    void *ctx;
    int total;
};

static void emit(struct out *o, const char *s, size_t n)
{
    if (n) {
        o->sink(o->ctx, s, n);
        o->total += (int)n;
    }
}

static void pad(struct out *o, char c, int n)
{
    char block[16];
    memset(block, c, sizeof(block));
    while (n > 0) {
        int k = n > 16 ? 16 : n;
        emit(o, block, (size_t)k);
        n -= k;
    }
}

static void emit_number(struct out *o, uint64_t value, bool negative, unsigned base, int flags,
                        int width, int precision)
{
    char digits[66];
    const char *set = (flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        digits[n++] = set[value % base];
        value /= base;
    } while (value);
    if (precision == 0 && n == 1 && digits[0] == '0')
        n = 0;

    char prefix[3];
    int plen = 0;
    if (negative)
        prefix[plen++] = '-';
    else if (flags & F_PLUS)
        prefix[plen++] = '+';
    else if (flags & F_SPACE)
        prefix[plen++] = ' ';
    if ((flags & F_ALT) && base == 16) {
        prefix[plen++] = '0';
        prefix[plen++] = (flags & F_UPPER) ? 'X' : 'x';
    }

    int zeros = precision > n ? precision - n : 0;
    int body = plen + zeros + n;
    if ((flags & F_ZERO) && !(flags & F_LEFT) && precision < 0 && width > body) {
        zeros += width - body;
        body = width;
    }
    if (!(flags & F_LEFT))
        pad(o, ' ', width - body);
    emit(o, prefix, (size_t)plen);
    pad(o, '0', zeros);
    while (n)
        emit(o, &digits[--n], 1);
    if (flags & F_LEFT)
        pad(o, ' ', width - body);
}

int fmt_format(fmt_sink_fn sink, void *ctx, const char *fmt, va_list ap)
{
    struct out o = {sink, ctx, 0};
    while (*fmt) {
        const char *start = fmt;
        while (*fmt && *fmt != '%')
            fmt++;
        emit(&o, start, (size_t)(fmt - start));
        if (!*fmt)
            break;
        fmt++;

        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                flags |= F_LEFT;
            else if (*fmt == '0')
                flags |= F_ZERO;
            else if (*fmt == '+')
                flags |= F_PLUS;
            else if (*fmt == ' ')
                flags |= F_SPACE;
            else if (*fmt == '#')
                flags |= F_ALT;
            else
                break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0)
                flags |= F_LEFT, width = -width;
            fmt++;
        } else {
            while (is_digit((uint8_t)*fmt))
                width = width * 10 + (*fmt++ - '0');
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (is_digit((uint8_t)*fmt))
                    precision = precision * 10 + (*fmt++ - '0');
            }
        }

        int size = 0;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 't' || *fmt == 'j') {
            if (*fmt == 'l' || *fmt == 'j')
                size++;
            else if (*fmt == 'z' || *fmt == 't')
                size = sizeof(size_t) == 8 ? 2 : 0;
            fmt++;
        }

        char c = *fmt ? *fmt++ : 0;
        switch (c) {
        case 'd':
        case 'i': {
            int64_t v = size >= 2 ? va_arg(ap, long long) : size == 1 ? va_arg(ap, long) : va_arg(ap, int);
            uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            emit_number(&o, mag, v < 0, 10, flags, width, precision);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'b': {
            uint64_t v = size >= 2   ? va_arg(ap, unsigned long long)
                         : size == 1 ? va_arg(ap, unsigned long)
                                     : va_arg(ap, unsigned int);
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : c == 'b' ? 2 : 16;
            emit_number(&o, v, false, base, flags | (c == 'X' ? F_UPPER : 0), width, precision);
            break;
        }
        case 'p':
            emit_number(&o, (uintptr_t)va_arg(ap, void *), false, 16, flags | F_ALT, width,
                        precision);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            if (!(flags & F_LEFT))
                pad(&o, ' ', width - 1);
            emit(&o, &ch, 1);
            if (flags & F_LEFT)
                pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t len = precision >= 0 ? strnlen(s, (size_t)precision) : strlen(s);
            if (!(flags & F_LEFT))
                pad(&o, ' ', width - (int)len);
            emit(&o, s, len);
            if (flags & F_LEFT)
                pad(&o, ' ', width - (int)len);
            break;
        }
        case '%':
            emit(&o, "%", 1);
            break;
        case 0:
            break;
        default:
            emit(&o, "%", 1);
            emit(&o, &c, 1);
            break;
        }
    }
    return o.total;
}

struct buf_sink {
    char *buf;
    size_t cap;
    size_t len;
};

static void buf_write(void *ctx, const char *s, size_t n)
{
    struct buf_sink *b = ctx;
    if (b->len + 1 < b->cap) {
        size_t room = b->cap - 1 - b->len;
        memcpy(b->buf + b->len, s, n < room ? n : room);
    }
    b->len += n;
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct buf_sink b = {buf, cap, 0};
    int n = fmt_format(buf_write, &b, fmt, ap);
    if (cap)
        buf[b.len < cap ? b.len : cap - 1] = 0;
    return n;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

void fmt_size(char *buf, size_t cap, uint64_t bytes)
{
    static const char units[] = "BKMGT";
    unsigned u = 0;
    uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && u < sizeof(units) - 2) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        ksnprintf(buf, cap, "%uB", (unsigned)whole);
    else if (whole < 10)
        ksnprintf(buf, cap, "%u.%u%c", (unsigned)whole, (unsigned)frac, units[u]);
    else
        ksnprintf(buf, cap, "%u%c", (unsigned)whole, units[u]);
}
