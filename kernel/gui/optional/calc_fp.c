#include <synth/command.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/module.h>
#include <synth/string.h>

#include "../internal.h"

/* Calculator: four modes in one window.
     Basic       the four operations, worked out as they are pressed;
     Scientific  a whole expression with precedence and brackets: powers, roots, factorial, logarithms, the
                 trigonometric functions and their inverses in degrees or radians, pi and e; the answer is shown
                 as it is typed;
     Programmer  64-bit integers in hexadecimal, decimal, octal or binary at a word size of 8 to 64 bits, with
                 and/or/xor/not, shifts and mod; the value is shown in all four bases at once;
     Converter   length, area, volume, weight, temperature, speed, time, data and pressure.
   The mathematical functions are computed here (series after range reduction). 'scicalc', 'progcalc' and
   'convert' do the same in the terminal. */

#define DISPLAY_H 120
#define GAP 8
#define BAR_H 40
#define EXPR_MAX 160

enum mode { BASIC, SCIENTIFIC, PROGRAMMER, CONVERTER };

/* ---- numbers ---- */

static const double PI = 3.14159265358979323846, E = 2.71828182845904523536, LN2 = 0.69314718055994530942,
                    LN10 = 2.30258509299404568402;

static bool bad(double v)
{
    return v != v || v > 1e300 || v < -1e300;
}

static double fabs_(double v)
{
    return v < 0 ? -v : v;
}

static double round_(double v)
{
    return v < 0 ? -(double)(long long)(-v + 0.5) : (double)(long long)(v + 0.5);
}

static double sqrt_(double v)
{
    if (v <= 0)
        return 0;
    double x = v > 1 ? v / 2 : 1;
    for (int i = 0; i < 80; i++) {
        double n = (x + v / x) / 2;
        if (n == x)
            break;
        x = n;
    }
    return x;
}

static double exp_(double x)
{
    if (x > 709)
        return 1e308 * 10;
    if (x < -745)
        return 0;
    long long k = (long long)round_(x / LN2);
    double r = x - (double)k * LN2, term = 1, sum = 1;
    for (int i = 1; i < 30; i++) {
        term *= r / i;
        sum += term;
    }
    for (; k > 0; k--)
        sum *= 2;
    for (; k < 0; k++)
        sum /= 2;
    return sum;
}

static double ln_(double x)
{
    if (x <= 0)
        return 0.0 / 0.0;
    int k = 0;
    while (x > 1.41421356237309504880)
        x /= 2, k++;
    while (x < 0.70710678118654752440)
        x *= 2, k--;
    double s = (x - 1) / (x + 1), s2 = s * s, term = s, sum = 0;
    for (int i = 1; i < 80; i += 2) {
        sum += term / i;
        term *= s2;
    }
    return 2 * sum + k * LN2;
}

static double pow_(double a, double b, bool *err)
{
    if (b == round_(b) && fabs_(b) <= 1024) {
        double r = 1, base = a;
        long long n = (long long)fabs_(b);
        while (n) {
            if (n & 1)
                r *= base;
            base *= base;
            n >>= 1;
        }
        if (b < 0) {
            if (r == 0) {
                *err = true;
                return 0;
            }
            r = 1 / r;
        }
        return r;
    }
    if (a < 0) {
        *err = true;
        return 0;
    }
    return a == 0 ? 0 : exp_(b * ln_(a));
}

static double sin_(double x)
{
    x -= 2 * PI * round_(x / (2 * PI));
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i < 25; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static double cos_(double x)
{
    return sin_(x + PI / 2);
}

static double atan_(double x)
{
    if (x < 0)
        return -atan_(-x);
    if (x > 1)
        return PI / 2 - atan_(1 / x);
    if (x > 0.25) /* halve the angle until the series is quick */
        return 2 * atan_(x / (1 + sqrt_(1 + x * x)));
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i < 40; i++) {
        term *= -x2;
        sum += term / (2 * i + 1);
    }
    return sum;
}

static double snap(double v)
{
    return fabs_(v) < 1e-13 ? 0 : fabs_(v - round_(v)) < 1e-12 ? round_(v) : v;
}

static void format(double v, char *out, size_t cap)
{
    if (bad(v)) {
        str_copy(out, "Error", cap);
        return;
    }
    char buf[48];
    size_t n = 0;
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    if (v >= 1e15 || (v > 0 && v < 1e-6)) {
        int e = 0;
        while (v >= 10)
            v /= 10, e++;
        while (v < 1)
            v *= 10, e--;
        long long digits = (long long)(v * 1e9 + 0.5);
        if (digits >= 10000000000LL)
            digits /= 10, e++;
        buf[n++] = (char)('0' + digits / 1000000000);
        long long rest = digits % 1000000000;
        char tmp[16];
        for (int i = 8; i >= 0; i--) {
            tmp[i] = (char)('0' + rest % 10);
            rest /= 10;
        }
        int keep = 9;
        while (keep > 0 && tmp[keep - 1] == '0')
            keep--;
        if (keep) {
            buf[n++] = '.';
            memcpy(buf + n, tmp, (size_t)keep);
            n += (size_t)keep;
        }
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "e%d", e);
        str_copy(out, buf, cap);
        return;
    }
    long long whole = (long long)v;
    double frac = v - (double)whole;
    int int_digits = 1;
    for (long long t = whole; t >= 10; t /= 10)
        int_digits++;
    int decimals = MAX(0, 12 - int_digits);
    long long scale = 1;
    for (int i = 0; i < decimals; i++)
        scale *= 10;
    long long f = (long long)(frac * (double)scale + 0.5);
    if (f >= scale)
        whole++, f -= scale;
    n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "%lld", whole);
    if (f) {
        char fbuf[16];
        for (int i = decimals - 1; i >= 0; i--) {
            fbuf[i] = (char)('0' + f % 10);
            f /= 10;
        }
        int keep = decimals;
        while (keep && fbuf[keep - 1] == '0')
            keep--;
        buf[n++] = '.';
        memcpy(buf + n, fbuf, (size_t)keep);
        n += (size_t)keep;
    }
    buf[n] = 0;
    str_copy(out, buf, cap);
}

static double parse_number(const char *s)
{
    double v = 0, scale = 1;
    bool neg = *s == '-', point = false;
    if (neg)
        s++;
    for (; *s; s++) {
        if (*s == '.') {
            point = true;
            continue;
        }
        if (*s < '0' || *s > '9')
            break;
        if (point) {
            scale /= 10;
            v += (*s - '0') * scale;
        } else {
            v = v * 10 + (*s - '0');
        }
    }
    return neg ? -v : v;
}

/* ---- scientific expressions ----
   Kept in a compact form: digits and '.', + - * / ^, ( ), the functions as a letter followed by '(' (s c t sin cos
   tan, S C T their inverses, l ln, L log, r square root, a absolute value), p pi, e e, and the postfix q square,
   i inverse, ! factorial, % percent. */

struct sci {
    const char *p;
    bool deg, err;
    const char *why;
};

static double sci_expr(struct sci *s);

static bool sci_starts_primary(char c)
{
    return (c >= '0' && c <= '9') || c == '.' || c == '(' || c == 'p' || c == 'e' || strchr("sctSCTlLra", c);
}

static double angle_in(const struct sci *s, double v)
{
    return s->deg ? v * PI / 180 : v;
}

static double angle_out(const struct sci *s, double v)
{
    return s->deg ? v * 180 / PI : v;
}

static double sci_primary(struct sci *s)
{
    char c = *s->p;
    if ((c >= '0' && c <= '9') || c == '.') {
        double v = parse_number(s->p);
        while ((*s->p >= '0' && *s->p <= '9') || *s->p == '.')
            s->p++;
        return v;
    }
    if (c == 'p' || c == 'e') {
        s->p++;
        return c == 'p' ? PI : E;
    }
    bool func = c && strchr("sctSCTlLra", c);
    if (func)
        s->p++;
    if (*s->p != '(') {
        s->err = true;
        return 0;
    }
    s->p++;
    double v = sci_expr(s);
    if (*s->p == ')')
        s->p++;
    else if (*s->p) /* a bracket left open at the end is closed there */
        s->err = true;
    if (!func)
        return v;
    switch (c) {
    case 's': return snap(sin_(angle_in(s, v)));
    case 'c': return snap(cos_(angle_in(s, v)));
    case 't': {
        double co = cos_(angle_in(s, v));
        if (fabs_(co) < 1e-13) {
            s->err = true;
            s->why = "Invalid input";
            return 0;
        }
        return snap(sin_(angle_in(s, v)) / co);
    }
    case 'S': case 'C':
        if (v < -1 || v > 1) {
            s->err = true;
            s->why = "Invalid input";
            return 0;
        }
        {
            double as = fabs_(v) == 1 ? v * PI / 2 : atan_(v / sqrt_(1 - v * v));
            return snap(angle_out(s, c == 'S' ? as : PI / 2 - as));
        }
    case 'T': return snap(angle_out(s, atan_(v)));
    case 'l': case 'L':
        if (v <= 0) {
            s->err = true;
            s->why = "Invalid input";
            return 0;
        }
        return snap(c == 'l' ? ln_(v) : ln_(v) / LN10);
    case 'r':
        if (v < 0) {
            s->err = true;
            s->why = "Invalid input";
            return 0;
        }
        return snap(sqrt_(v));
    default: return fabs_(v);
    }
}

static double sci_postfix(struct sci *s)
{
    double v = sci_primary(s);
    for (;;) {
        char c = *s->p;
        if (c == 'q') {
            v *= v;
        } else if (c == 'i') {
            if (v == 0) {
                s->err = true;
                s->why = "Cannot divide by zero";
            } else {
                v = 1 / v;
            }
        } else if (c == '%') {
            v /= 100;
        } else if (c == '!') {
            if (v < 0 || v != round_(v) || v > 170) {
                s->err = true;
                s->why = v > 170 ? "Overflow" : "Invalid input";
            } else {
                double f = 1;
                for (int k = 2; k <= (int)v; k++)
                    f *= k;
                v = f;
            }
        } else {
            return v;
        }
        s->p++;
    }
}

static double sci_unary(struct sci *s);

static double sci_power(struct sci *s)
{
    double v = sci_postfix(s);
    if (*s->p == '^') {
        s->p++;
        double b = sci_unary(s);
        bool err = false;
        v = pow_(v, b, &err);
        if (err) {
            s->err = true;
            s->why = "Invalid input";
        }
    }
    return v;
}

static double sci_unary(struct sci *s)
{
    if (*s->p == '-') {
        s->p++;
        return -sci_unary(s);
    }
    if (*s->p == '+') {
        s->p++;
        return sci_unary(s);
    }
    return sci_power(s);
}

static double sci_term(struct sci *s)
{
    double v = sci_unary(s);
    for (;;) {
        char c = *s->p;
        if (c == '*' || c == '/') {
            s->p++;
            double b = sci_unary(s);
            if (c == '/' && b == 0) {
                s->err = true;
                s->why = "Cannot divide by zero";
                return 0;
            }
            v = c == '*' ? v * b : v / b;
        } else if (c && sci_starts_primary(c)) { /* 2π, 3(4+1) */
            v *= sci_unary(s);
        } else {
            return v;
        }
    }
}

static double sci_expr(struct sci *s)
{
    double v = sci_term(s);
    while (!s->err && (*s->p == '+' || *s->p == '-')) {
        char c = *s->p++;
        double b = sci_term(s);
        v = c == '+' ? v + b : v - b;
    }
    return v;
}

/* the value of an expression; *why says what is wrong when it returns false */
static bool sci_eval(const char *expr, bool deg, double *out, const char **why)
{
    struct sci s = {expr, deg, false, NULL};
    *why = NULL;
    if (!*expr)
        return false;
    double v = sci_expr(&s);
    if (s.err || *s.p || bad(v)) {
        *why = s.why ? s.why : bad(v) && !s.err && !*s.p ? "Overflow" : NULL;
        return false;
    }
    *out = v;
    return true;
}

/* the expression as it is shown */
static void sci_pretty(const char *expr, char *out, size_t cap)
{
    out[0] = 0;
    for (const char *p = expr; *p; p++) {
        const char *t;
        char one[2] = {*p, 0};
        switch (*p) {
        case 's': t = "sin"; break;
        case 'c': t = "cos"; break;
        case 't': t = "tan"; break;
        case 'S': t = "asin"; break;
        case 'C': t = "acos"; break;
        case 'T': t = "atan"; break;
        case 'l': t = "ln"; break;
        case 'L': t = "log"; break;
        case 'r': t = "√"; break;
        case 'a': t = "abs"; break;
        case 'p': t = "π"; break;
        case 'q': t = "²"; break;
        case 'i': t = "^-1"; break;
        case '*': t = " × "; break;
        case '/': t = " ÷ "; break;
        case '+': t = " + "; break;
        case '-': t = p == expr || strchr("(*/+-^", p[-1]) ? "−" : " − "; break;
        default: t = one; break;
        }
        str_append(out, t, cap);
    }
}

/* text typed in the terminal into the compact form */
static bool sci_from_text(const char *in, char *out, size_t cap)
{
    static const struct {
        const char *word;
        const char *code;
    } words[] = {{"asin", "S"}, {"acos", "C"}, {"atan", "T"}, {"sin", "s"}, {"cos", "c"}, {"tan", "t"},
                 {"ln", "l"},   {"log", "L"},  {"sqrt", "r"}, {"abs", "a"}, {"pi", "p"},  {"**", "^"},
                 {"×", "*"},    {"÷", "/"},    {"−", "-"},    {"π", "p"},   {"√", "r"},   {"²", "q"}};
    size_t n = 0;
    while (*in && n + 2 < cap) {
        if (*in == ' ') {
            in++;
            continue;
        }
        bool hit = false;
        for (size_t i = 0; i < ARRAY_SIZE(words) && !hit; i++) {
            size_t k = strlen(words[i].word);
            if (!str_ncasecmp(in, words[i].word, k)) {
                out[n++] = words[i].code[0];
                in += k;
                hit = true;
            }
        }
        if (hit)
            continue;
        char c = *in++;
        if (!((c >= '0' && c <= '9') || strchr(".+-*/^()e!%", c)))
            return false;
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}

/* ---- programmer: 64-bit integers ---- */

struct prog {
    const char *p;
    int base, bits;
    bool err;
    const char *why;
};

static int64_t wrap(int64_t v, int bits)
{
    if (bits >= 64)
        return v;
    uint64_t m = (UINT64_C(1) << bits) - 1, u = (uint64_t)v & m;
    if (u >> (bits - 1))
        u |= ~m;
    return (int64_t)u;
}

static int digit_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int64_t prog_or(struct prog *s);

static int64_t prog_unary(struct prog *s)
{
    char c = *s->p;
    if (c == '-' || c == '~') {
        s->p++;
        int64_t v = prog_unary(s);
        return wrap(c == '-' ? (int64_t)(0 - (uint64_t)v) : ~v, s->bits);
    }
    if (c == '(') {
        s->p++;
        int64_t v = prog_or(s);
        if (*s->p == ')')
            s->p++;
        else if (*s->p)
            s->err = true;
        return v;
    }
    int d = digit_value(c);
    if (d < 0 || d >= s->base) {
        s->err = true;
        return 0;
    }
    uint64_t v = 0;
    while ((d = digit_value(*s->p)) >= 0 && d < s->base) {
        v = v * (uint64_t)s->base + (uint64_t)d;
        s->p++;
    }
    return wrap((int64_t)v, s->bits);
}

static int64_t prog_mul(struct prog *s)
{
    int64_t v = prog_unary(s);
    while (!s->err && (*s->p == '*' || *s->p == '/' || *s->p == 'm')) {
        char c = *s->p++;
        int64_t b = prog_unary(s);
        if (c != '*' && b == 0) {
            s->err = true;
            s->why = "Cannot divide by zero";
            return 0;
        }
        v = wrap(c == '*' ? (int64_t)((uint64_t)v * (uint64_t)b) : c == '/' ? v / b : v % b, s->bits);
    }
    return v;
}

static int64_t prog_add(struct prog *s)
{
    int64_t v = prog_mul(s);
    while (!s->err && (*s->p == '+' || *s->p == '-')) {
        char c = *s->p++;
        uint64_t b = (uint64_t)prog_mul(s);
        v = wrap((int64_t)(c == '+' ? (uint64_t)v + b : (uint64_t)v - b), s->bits);
    }
    return v;
}

static int64_t prog_shift(struct prog *s)
{
    int64_t v = prog_add(s);
    while (!s->err && (*s->p == '<' || *s->p == '>')) {
        char c = *s->p++;
        int64_t b = prog_add(s);
        if (b < 0 || b >= 64)
            v = c == '<' || v >= 0 ? 0 : -1;
        else
            v = wrap(c == '<' ? (int64_t)((uint64_t)v << b) : v >> b, s->bits);
    }
    return v;
}

static int64_t prog_and(struct prog *s)
{
    int64_t v = prog_shift(s);
    while (!s->err && *s->p == '&') {
        s->p++;
        v &= prog_shift(s);
    }
    return v;
}

static int64_t prog_xor(struct prog *s)
{
    int64_t v = prog_and(s);
    while (!s->err && *s->p == 'x') {
        s->p++;
        v ^= prog_and(s);
    }
    return v;
}

static int64_t prog_or(struct prog *s)
{
    int64_t v = prog_xor(s);
    while (!s->err && *s->p == '|') {
        s->p++;
        v |= prog_xor(s);
    }
    return v;
}

static bool prog_eval(const char *expr, int base, int bits, int64_t *out, const char **why)
{
    struct prog s = {expr, base, bits, false, NULL};
    *why = NULL;
    if (!*expr)
        return false;
    int64_t v = prog_or(&s);
    if (s.err || *s.p) {
        *why = s.why;
        return false;
    }
    *out = v;
    return true;
}

/* the value in a base: decimal signed, the others as the bits of the word; binary in groups of four */
static void prog_format(int64_t v, int base, int bits, char *out, size_t cap)
{
    if (base == 10) {
        ksnprintf(out, cap, "%lld", (long long)v);
        return;
    }
    uint64_t u = bits >= 64 ? (uint64_t)v : (uint64_t)v & ((UINT64_C(1) << bits) - 1);
    char tmp[80];
    int n = 0;
    do {
        tmp[n++] = "0123456789ABCDEF"[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u);
    size_t o = 0;
    for (int i = n - 1; i >= 0 && o + 2 < cap; i--) {
        out[o++] = tmp[i];
        if (base == 2 && i && i % 4 == 0)
            out[o++] = ' ';
    }
    out[o] = 0;
}

static void prog_pretty(const char *expr, char *out, size_t cap)
{
    out[0] = 0;
    for (const char *p = expr; *p; p++) {
        char one[2] = {*p, 0};
        const char *t = *p == '<' ? " << " : *p == '>' ? " >> " : *p == 'm' ? " mod " : *p == 'x' ? " xor "
                      : *p == '&' ? " and " : *p == '|' ? " or " : *p == '~' ? "not " : *p == '*' ? " × "
                      : *p == '/' ? " ÷ " : *p == '+' ? " + " : *p == '-' && p > expr && !strchr("(<>mx&|~*/+-", p[-1])
                      ? " − " : *p == '-' ? "−" : one;
        str_append(out, t, cap);
    }
}

/* terminal text: 0x.. 0b.. 0o.. numbers, words and C operators, all turned into decimal */
static bool prog_from_text(const char *in, char *out, size_t cap)
{
    static const struct {
        const char *word;
        char code;
    } words[] = {{"<<", '<'}, {">>", '>'}, {"mod", 'm'}, {"xor", 'x'}, {"and", '&'}, {"or", '|'}, {"not", '~'},
                 {"^", 'x'},  {"%", 'm'}};
    size_t n = 0;
    while (*in && n + 24 < cap) {
        if (*in == ' ') {
            in++;
            continue;
        }
        if (in[0] == '0' && (in[1] == 'x' || in[1] == 'b' || in[1] == 'o')) {
            int base = in[1] == 'x' ? 16 : in[1] == 'b' ? 2 : 8, d;
            uint64_t v = 0;
            in += 2;
            char up;
            while ((up = (char)(*in >= 'a' && *in <= 'f' ? *in - 32 : *in)), (d = digit_value(up)) >= 0 && d < base)
                v = v * (uint64_t)base + (uint64_t)d, in++;
            n += (size_t)ksnprintf(out + n, cap - n, "%llu", (unsigned long long)v);
            continue;
        }
        bool hit = false;
        for (size_t i = 0; i < ARRAY_SIZE(words) && !hit; i++) {
            size_t k = strlen(words[i].word);
            if (!str_ncasecmp(in, words[i].word, k)) {
                out[n++] = words[i].code;
                in += k;
                hit = true;
            }
        }
        if (hit)
            continue;
        char c = *in++;
        if (!((c >= '0' && c <= '9') || strchr("+-*/()&|~", c)))
            return false;
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}

/* ---- units ---- */

struct unit {
    const char *name, *symbol;
    double factor; /* how many of the category's base unit one of these is */
};

struct category {
    const char *name;
    const struct unit *units;
    int count;
};

static const struct unit lengths[] = {
    {"Millimetres", "mm", 0.001}, {"Centimetres", "cm", 0.01}, {"Metres", "m", 1},         {"Kilometres", "km", 1000},
    {"Inches", "in", 0.0254},     {"Feet", "ft", 0.3048},      {"Yards", "yd", 0.9144},    {"Miles", "mi", 1609.344},
    {"Nautical miles", "nmi", 1852},
};
static const struct unit areas[] = {
    {"Square metres", "m2", 1},         {"Square kilometres", "km2", 1e6},     {"Hectares", "ha", 1e4},
    {"Square feet", "ft2", 0.09290304}, {"Acres", "ac", 4046.8564224},          {"Square miles", "mi2", 2589988.110336},
};
static const struct unit volumes[] = {
    {"Millilitres", "ml", 0.001},     {"Litres", "l", 1},           {"Cubic metres", "m3", 1000},
    {"US cups", "cup", 0.2365882365}, {"US gallons", "gal", 3.785411784}, {"Imperial gallons", "impgal", 4.54609},
    {"US fluid ounces", "floz", 0.0295735295625},
};
static const struct unit weights[] = {
    {"Milligrams", "mg", 1e-6}, {"Grams", "g", 0.001},           {"Kilograms", "kg", 1}, {"Tonnes", "t", 1000},
    {"Ounces", "oz", 0.028349523125}, {"Pounds", "lb", 0.45359237}, {"Stone", "st", 6.35029318},
};
static const struct unit temperatures[] = {{"Celsius", "c", 0}, {"Fahrenheit", "f", 0}, {"Kelvin", "k", 0}};
static const struct unit speeds[] = {
    {"Metres per second", "m/s", 1}, {"Kilometres per hour", "km/h", 1 / 3.6}, {"Miles per hour", "mph", 0.44704},
    {"Knots", "kn", 1852.0 / 3600},
};
static const struct unit times[] = {
    {"Seconds", "s", 1}, {"Minutes", "min", 60}, {"Hours", "h", 3600}, {"Days", "d", 86400}, {"Weeks", "wk", 604800},
    {"Years", "yr", 31557600},
};
static const struct unit data[] = {
    {"Bits", "bit", 0.125},       {"Bytes", "B", 1},           {"Kilobytes", "KB", 1000},  {"Kibibytes", "KiB", 1024},
    {"Megabytes", "MB", 1e6},     {"Mebibytes", "MiB", 1048576}, {"Gigabytes", "GB", 1e9}, {"Gibibytes", "GiB", 1073741824},
    {"Terabytes", "TB", 1e12},
};
static const struct unit pressures[] = {
    {"Pascals", "pa", 1}, {"Kilopascals", "kpa", 1000}, {"Bars", "bar", 1e5}, {"Atmospheres", "atm", 101325},
    {"Pounds per sq. inch", "psi", 6894.757293168},
};

static const struct category categories[] = {
    {"Length", lengths, ARRAY_SIZE(lengths)},     {"Area", areas, ARRAY_SIZE(areas)},
    {"Volume", volumes, ARRAY_SIZE(volumes)},     {"Weight", weights, ARRAY_SIZE(weights)},
    {"Temperature", temperatures, 3},             {"Speed", speeds, ARRAY_SIZE(speeds)},
    {"Time", times, ARRAY_SIZE(times)},           {"Data", data, ARRAY_SIZE(data)},
    {"Pressure", pressures, ARRAY_SIZE(pressures)},
};

static double convert(int cat, int from, int to, double v)
{
    if (categories[cat].units == temperatures) {
        double c = from == 0 ? v : from == 1 ? (v - 32) * 5 / 9 : v - 273.15;
        return to == 0 ? c : to == 1 ? c * 9 / 5 + 32 : c + 273.15;
    }
    return v * categories[cat].units[from].factor / categories[cat].units[to].factor;
}

static bool find_unit(const char *sym, int *cat, int *index)
{
    for (int c = 0; c < (int)ARRAY_SIZE(categories); c++)
        for (int i = 0; i < categories[c].count; i++)
            if (!str_casecmp(sym, categories[c].units[i].symbol) || !str_casecmp(sym, categories[c].units[i].name)) {
                *cat = c;
                *index = i;
                return true;
            }
    return false;
}

/* ---- the window ---- */

struct calc {
    struct ui_state st;
    struct window *win;
    enum mode mode;
    /* basic */
    char entry[32];
    double acc;
    char op;
    bool fresh, error;
    char history[64];
    /* scientific and programmer */
    char expr[EXPR_MAX];
    char shown[48];  /* the last answer */
    char line[EXPR_MAX * 3]; /* the expression above it */
    bool deg, done;
    int base, bits;
    /* converter */
    int cat, from, to;
    struct ui_text value;
    int picking; /* 1: the from unit, 2: the to unit, 3: the category */
};

static void clear(struct calc *c)
{
    str_copy(c->entry, "0", sizeof(c->entry));
    c->acc = 0;
    c->op = 0;
    c->fresh = true;
    c->error = false;
    c->history[0] = 0;
    c->expr[0] = 0;
    c->line[0] = 0;
    str_copy(c->shown, "0", sizeof(c->shown));
    c->done = false;
}

/* ---- basic: worked out as pressed ---- */

static double apply(double a, double b, char op, bool *error)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0) {
            *error = true;
            return 0;
        }
        return a / b;
    default: return b;
    }
}

static void digit(struct calc *c, char d)
{
    if (c->error)
        clear(c);
    if (c->fresh) {
        str_copy(c->entry, d == '.' ? "0." : "0", sizeof(c->entry));
        c->fresh = false;
        if (d == '.')
            return;
    }
    if (d == '.' && strchr(c->entry, '.'))
        return;
    size_t n = strlen(c->entry);
    if (n >= 16)
        return;
    if (!strcmp(c->entry, "0") && d != '.')
        c->entry[0] = d;
    else {
        c->entry[n] = d;
        c->entry[n + 1] = 0;
    }
}

static const char *op_symbol(char op)
{
    return op == '*' ? "×" : op == '/' ? "÷" : op == '-' ? "−" : "+";
}

static void operator(struct calc *c, char op)
{
    if (c->error)
        return;
    double value = parse_number(c->entry);
    if (c->op && !c->fresh)
        c->acc = apply(c->acc, value, c->op, &c->error);
    else if (!c->op)
        c->acc = value;
    c->op = op;
    c->fresh = true;
    format(c->acc, c->entry, sizeof(c->entry));
    ksnprintf(c->history, sizeof(c->history), "%s %s", c->entry, op_symbol(op));
    if (c->error)
        str_copy(c->entry, "Cannot divide by zero", sizeof(c->entry));
}

static void equals(struct calc *c)
{
    if (c->error || !c->op)
        return;
    double value = parse_number(c->entry);
    char a[32], b[32];
    format(c->acc, a, sizeof(a));
    format(value, b, sizeof(b));
    double result = apply(c->acc, value, c->op, &c->error);
    ksnprintf(c->history, sizeof(c->history), "%s %s %s =", a, op_symbol(c->op), b);
    c->op = 0;
    c->fresh = true;
    if (c->error)
        str_copy(c->entry, "Cannot divide by zero", sizeof(c->entry));
    else
        format(result, c->entry, sizeof(c->entry));
}

static void unary(struct calc *c, char kind)
{
    if (c->error)
        return;
    double v = parse_number(c->entry);
    if (kind == 'n') {
        v = -v;
    } else if (kind == '%') {
        v = c->op ? c->acc * v / 100 : v / 100;
    } else if (kind == 'r' || kind == 'i') {
        if ((kind == 'r' && v < 0) || (kind == 'i' && v == 0)) {
            c->error = true;
            str_copy(c->entry, kind == 'r' ? "Invalid input" : "Cannot divide by zero", sizeof(c->entry));
            return;
        }
        v = kind == 'r' ? sqrt_(v) : 1 / v;
    } else if (kind == 'q') {
        v = v * v;
    }
    format(v, c->entry, sizeof(c->entry));
    c->fresh = kind != 'n';
}

static void backspace(struct calc *c)
{
    if (c->fresh || c->error)
        return;
    size_t n = strlen(c->entry);
    if (n <= 1 || (n == 2 && c->entry[0] == '-'))
        str_copy(c->entry, "0", sizeof(c->entry));
    else
        c->entry[n - 1] = 0;
}

static void basic_press(struct calc *c, const char *l)
{
    if (l[0] >= '0' && l[0] <= '9' && !l[1])
        digit(c, l[0]);
    else if (!strcmp(l, "."))
        digit(c, '.');
    else if (!strcmp(l, "+"))
        operator(c, '+');
    else if (!strcmp(l, "−"))
        operator(c, '-');
    else if (!strcmp(l, "×"))
        operator(c, '*');
    else if (!strcmp(l, "÷"))
        operator(c, '/');
    else if (!strcmp(l, "="))
        equals(c);
    else if (!strcmp(l, "C"))
        clear(c);
    else if (!strcmp(l, "CE")) {
        str_copy(c->entry, "0", sizeof(c->entry));
        c->error = false;
    } else if (!strcmp(l, "⌫"))
        backspace(c);
    else if (!strcmp(l, "±"))
        unary(c, 'n');
    else if (!strcmp(l, "%"))
        unary(c, '%');
    else if (!strcmp(l, "√x"))
        unary(c, 'r');
    else if (!strcmp(l, "x²"))
        unary(c, 'q');
    else if (!strcmp(l, "1/x"))
        unary(c, 'i');
}

/* ---- scientific and programmer: an expression ---- */

static void refresh(struct calc *c)
{
    const char *why;
    if (c->mode == PROGRAMMER) {
        int64_t v;
        prog_pretty(c->expr, c->line, sizeof(c->line));
        if (prog_eval(c->expr, c->base, c->bits, &v, &why))
            prog_format(v, c->base, c->bits, c->shown, sizeof(c->shown));
        else if (!c->expr[0])
            str_copy(c->shown, "0", sizeof(c->shown));
    } else {
        double v;
        sci_pretty(c->expr, c->line, sizeof(c->line));
        if (sci_eval(c->expr, c->deg, &v, &why))
            format(v, c->shown, sizeof(c->shown));
        else if (!c->expr[0])
            str_copy(c->shown, "0", sizeof(c->shown));
    }
}

static void put(struct calc *c, const char *s)
{
    if (c->done) { /* after =: a number starts afresh, an operator goes on from the answer */
        bool number = (s[0] >= '0' && s[0] <= '9') || (s[0] >= 'A' && s[0] <= 'F' && c->mode == PROGRAMMER) ||
                      s[0] == '.' || (c->mode == SCIENTIFIC && strchr("sctSCTlLrape(", s[0])) || s[0] == '(' ||
                      s[0] == '~';
        if (number)
            c->expr[0] = 0;
        c->done = false;
    }
    if (strlen(c->expr) + strlen(s) < sizeof(c->expr) - 1)
        str_append(c->expr, s, sizeof(c->expr));
    refresh(c);
}

static void erase(struct calc *c)
{
    size_t n = strlen(c->expr);
    c->done = false;
    if (n >= 2 && c->expr[n - 1] == '(' && c->mode == SCIENTIFIC && strchr("sctSCTlLra", c->expr[n - 2]))
        n--;
    if (n)
        c->expr[n - 1] = 0;
    refresh(c);
}

static void evaluate(struct calc *c)
{
    const char *why;
    char result[80];
    if (c->mode == PROGRAMMER) {
        int64_t v;
        if (!prog_eval(c->expr, c->base, c->bits, &v, &why)) {
            str_copy(c->shown, why ? why : "Error", sizeof(c->shown));
            return;
        }
        prog_format(v, c->base, c->bits, result, sizeof(result));
        prog_pretty(c->expr, c->line, sizeof(c->line));
        str_copy(c->shown, result, sizeof(c->shown));
        /* the answer stays as the expression, without the spaces of binary */
        size_t o = 0;
        if (v < 0 && c->base == 10)
            c->expr[o++] = '-', v = -v;
        prog_format(v, c->base, c->bits, result, sizeof(result));
        for (const char *p = result; *p && o + 1 < sizeof(c->expr); p++)
            if (*p != ' ' && *p != '-')
                c->expr[o++] = *p;
        c->expr[o] = 0;
    } else {
        double v;
        if (!sci_eval(c->expr, c->deg, &v, &why)) {
            str_copy(c->shown, why ? why : "Error", sizeof(c->shown));
            return;
        }
        sci_pretty(c->expr, c->line, sizeof(c->line));
        format(v, result, sizeof(result));
        str_copy(c->shown, result, sizeof(c->shown));
        char *e = strchr(result, 'e'); /* the compact form has no exponent: a product with a power of ten */
        if (e) {
            *e = 0;
            ksnprintf(c->expr, sizeof(c->expr), "(%s*10^(%s))", result, e + 1);
        } else {
            str_copy(c->expr, result, sizeof(c->expr));
        }
    }
    str_append(c->line, " =", sizeof(c->line));
    c->done = true;
}

static void set_base(struct calc *c, int base)
{
    int64_t v;
    const char *why;
    bool ok = prog_eval(c->expr, c->base, c->bits, &v, &why);
    c->base = base;
    if (ok) {
        char text[80];
        size_t o = 0;
        if (v < 0 && base == 10)
            c->expr[o++] = '-', v = -v;
        prog_format(v, base, c->bits, text, sizeof(text));
        for (const char *p = text; *p && o + 1 < sizeof(c->expr); p++)
            if (*p != ' ' && *p != '-')
                c->expr[o++] = *p;
        c->expr[o] = 0;
    } else {
        c->expr[0] = 0;
    }
    refresh(c);
    app_state_set_number("Calculator", "base", base);
}

static void set_bits(struct calc *c, int bits)
{
    int64_t v;
    const char *why;
    if (prog_eval(c->expr, c->base, c->bits, &v, &why)) {
        v = wrap(v, bits);
        char text[80];
        size_t o = 0;
        if (v < 0 && c->base == 10)
            c->expr[o++] = '-', v = -v;
        prog_format(v, c->base, bits, text, sizeof(text));
        for (const char *p = text; *p && o + 1 < sizeof(c->expr); p++)
            if (*p != ' ' && *p != '-')
                c->expr[o++] = *p;
        c->expr[o] = 0;
    }
    c->bits = bits;
    refresh(c);
    app_state_set_number("Calculator", "bits", bits);
}

/* the keys of each mode: label and what it puts into the expression ("" for the special ones) */
struct key {
    const char *label, *code;
    int kind; /* 0 digit, 1 operator, 2 function, 3 control */
};

static const struct key basic_keys[] = {
    {"%", "", 3},   {"CE", "", 3}, {"C", "", 3},  {"⌫", "", 3}, {"1/x", "", 2}, {"x²", "", 2}, {"√x", "", 2},
    {"÷", "", 1},   {"7", "", 0},  {"8", "", 0},  {"9", "", 0}, {"×", "", 1},   {"4", "", 0},  {"5", "", 0},
    {"6", "", 0},   {"−", "", 1},  {"1", "", 0},  {"2", "", 0}, {"3", "", 0},   {"+", "", 1},  {"±", "", 2},
    {"0", "", 0},   {".", "", 0},  {"=", "", 1},
};

static const struct key sci_keys[] = {
    {"(", "(", 2},     {")", ")", 2},      {"n!", "!", 2},    {"AC", "", 3},    {"⌫", "", 3},  {"%", "%", 2},
    {"÷", "/", 1},     {"sin", "s(", 2},   {"cos", "c(", 2},  {"tan", "t(", 2}, {"7", "7", 0}, {"8", "8", 0},
    {"9", "9", 0},     {"×", "*", 1},      {"asin", "S(", 2}, {"acos", "C(", 2}, {"atan", "T(", 2}, {"4", "4", 0},
    {"5", "5", 0},     {"6", "6", 0},      {"−", "-", 1},     {"ln", "l(", 2}, {"log", "L(", 2}, {"e^x", "e^", 2},
    {"1", "1", 0},     {"2", "2", 0},      {"3", "3", 0},     {"+", "+", 1},   {"x²", "q", 2}, {"x^y", "^", 2},
    {"10^x", "10^", 2}, {"(−)", "-", 2},    {"0", "0", 0},     {".", ".", 0},   {"=", "", 1},  {"√", "r(", 2},
    {"π", "p", 2},     {"e", "e", 2},      {"1/x", "i", 2},   {"|x|", "a(", 2}, {"Deg", "", 3}, {"Ans", "", 3},
};

static const struct key prog_keys[] = {
    {"A", "A", 0},   {"<<", "<", 1},  {">>", ">", 1},  {"AC", "", 3},   {"⌫", "", 3},  {"B", "B", 0},
    {"(", "(", 2},   {")", ")", 2},   {"mod", "m", 1}, {"÷", "/", 1},  {"C", "C", 0}, {"7", "7", 0},
    {"8", "8", 0},   {"9", "9", 0},   {"×", "*", 1},   {"D", "D", 0},  {"4", "4", 0}, {"5", "5", 0},
    {"6", "6", 0},   {"−", "-", 1},   {"E", "E", 0},   {"1", "1", 0},  {"2", "2", 0}, {"3", "3", 0},
    {"+", "+", 1},   {"F", "F", 0},   {"and", "&", 1}, {"or", "|", 1}, {"0", "0", 0}, {"=", "", 1},
    {"xor", "x", 1}, {"not", "~", 2}, {"±", "-", 2},   {"", "", 3},    {"", "", 3},
};

static bool calc_key(struct ui *ui, struct rect r, const char *label, uint32_t bg, uint32_t fg, bool enabled)
{
    int id = ui_next_id(ui);
    bool clicked = false;
    struct gui_event *e = ui->e;
    if (e && enabled && label[0]) {
        bool in = ui_hover(ui, r);
        if (e->type == EV_MOUSE_DOWN && in && e->button == BUTTON_LEFT) {
            ui->st->active = id;
            ui->consumed = true;
            ui->changed = true;
        } else if (e->type == EV_MOUSE_UP && ui->st->active == id) {
            clicked = in;
            ui->consumed = true;
        }
    }
    if (ui->c && label[0]) {
        if (ui->st->active == id)
            bg = color_shade(bg, 60);
        gfx_round(ui->c, r, MIN(r.h, r.w) / 2, bg);
        const struct ui_font *f = strlen(label) > 2 || r.h < 46 ? &font_bold : &font_title;
        gfx_text_box(ui->c, r, f, label, enabled ? fg : color_shade(fg, -120), ALIGN_CENTER);
    }
    return clicked;
}

static const struct menu_item edit_items[] = {
    {"Copy", KEY_CTRL('c'), "⌘C"},
    {"Paste", KEY_CTRL('v'), "⌘V"},
    {"", 0, NULL},
    {"Clear", KEY_ESC, "Esc"},
};

static const struct menu_item view_items[] = {
    {"Basic", KEY_CTRL('1'), "⌘1"},
    {"Scientific", KEY_CTRL('2'), "⌘2"},
    {"Programmer", KEY_CTRL('3'), "⌘3"},
    {"Converter", KEY_CTRL('4'), "⌘4"},
};

static const struct app_menu menus[] = {
    {"Edit", edit_items, ARRAY_SIZE(edit_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
};

static const int widths[4] = {300, 480, 420, 400}, heights[4] = {520, 560, 600, 480};

static void set_mode(struct calc *c, enum mode m)
{
    if (c->mode == m)
        return;
    c->mode = m;
    clear(c);
    refresh(c);
    wm_resize_client(c->win, widths[m], heights[m]);
    app_state_set_number("Calculator", "mode", m);
}

static void pick_unit(struct window *owner, int index)
{
    struct calc *c = owner->data;
    if (index < 0)
        return;
    if (c->picking == 4) {
        c->picking = 0;
        set_mode(c, (enum mode)index);
        return;
    }
    if (c->picking == 3) {
        c->cat = index;
        c->from = 0;
        c->to = categories[index].count > 1 ? 1 : 0;
    } else if (c->picking == 1) {
        c->from = index;
    } else {
        c->to = index;
    }
    c->picking = 0;
    app_state_set_number("Calculator", "category", c->cat);
    wm_invalidate(owner);
}

static void unit_button(struct ui *ui, struct calc *c, struct rect r, const char *label, int which)
{
    if (ui_button(ui, r, label)) {
        static const char *items[16];
        int n = 0;
        if (which == 3)
            for (size_t i = 0; i < ARRAY_SIZE(categories); i++)
                items[n++] = categories[i].name;
        else
            for (int i = 0; i < categories[c->cat].count; i++)
                items[n++] = categories[c->cat].units[i].name;
        c->picking = which;
        ui_menu(c->win, r.x, r.y + r.h, items, n, pick_unit);
    }
}

static void converter(struct ui *ui, struct calc *c, int W, int top)
{
    const struct category *cat = &categories[c->cat];
    int x = 20, w = W - 40, y = top + 10;
    unit_button(ui, c, (struct rect){x, y, w, 30}, cat->name, 3);
    y += 50;
    ui_label(ui, (struct rect){x, y, w, 18}, "From", &font_small, style.tertiary, ALIGN_LEFT);
    y += 20;
    ui_textbox_hint(ui, (struct rect){x, y, w, 40}, &c->value, "Value");
    y += 48;
    unit_button(ui, c, (struct rect){x, y, w, 28}, cat->units[c->from].name, 1);
    y += 44;
    if (ui_symbol_button(ui, (struct rect){W / 2 - 16, y, 32, 32}, SYM_DOWN, false)) {
        int t = c->from;
        c->from = c->to;
        c->to = t;
    }
    y += 44;
    ui_label(ui, (struct rect){x, y, w, 18}, "To", &font_small, style.tertiary, ALIGN_LEFT);
    y += 20;
    char out[64] = "";
    const char *text = c->value.buf;
    bool number = text[0] != 0;
    for (const char *p = text[0] == '-' ? text + 1 : text; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ','))
            number = false;
    if (number) {
        char tmp[64];
        str_copy(tmp, text, sizeof(tmp));
        for (char *p = tmp; *p; p++)
            if (*p == ',')
                *p = '.';
        format(snap(convert(c->cat, c->from, c->to, parse_number(tmp)) * 1e9) / 1e9, out, sizeof(out));
    }
    if (ui->c) {
        gfx_round(ui->c, (struct rect){x, y, w, 40}, 8, style.dark ? RGB(36, 36, 40) : RGB(236, 236, 240));
        gfx_text_box(ui->c, (struct rect){x + 12, y, w - 24, 40}, &font_large, number ? out : "—", style.text,
                     ALIGN_LEFT);
    }
    y += 48;
    unit_button(ui, c, (struct rect){x, y, w, 28}, cat->units[c->to].name, 2);
}

static void display(struct ui *ui, struct calc *c, int W, const char *small, const char *big)
{
    if (!ui->c)
        return;
    gfx_text_box(ui->c, (struct rect){16, 34, W - 32, 20}, &font_sans, small, RGB(150, 150, 156), ALIGN_RIGHT);
    bool digits = true;
    for (const char *p = big; *p; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'G' && *p <= 'Z' && *p != 'E'))
            digits = false;
    const struct ui_font *f = digits && gfx_text_width(&font_display, big) <= W - 32 ? &font_display : &font_large;
    gfx_text_box(ui->c, (struct rect){16, 54, W - 32, DISPLAY_H - 50}, f, big, RGB(255, 255, 255), ALIGN_RIGHT);
    UNUSED(c);
}

static void keypad(struct ui *ui, struct calc *c, const struct key *keys, int count, int cols, int top, int W, int H)
{
    int rows = (count + cols - 1) / cols;
    int bw = (W - GAP * (cols + 1)) / cols, bh = MIN(bw, (H - top - GAP * (rows + 1)) / rows);
    for (int i = 0; i < count; i++) {
        const struct key *k = &keys[i];
        int r = i / cols, col = i % cols;
        struct rect b = {GAP + col * (bw + GAP), top + r * (bh + GAP), bw, bh};
        uint32_t bg = RGB(51, 51, 54), fg = RGB(255, 255, 255);
        bool enabled = true;
        if (k->kind == 1 || !strcmp(k->label, "=")) {
            bg = RGB(255, 159, 10);
        } else if (k->kind == 3) {
            bg = RGB(165, 165, 170);
            fg = RGB(20, 20, 22);
        } else if (k->kind == 2) {
            bg = RGB(80, 80, 84);
        }
        if (c->mode == PROGRAMMER && k->kind == 0) {
            int d = digit_value(k->code[0]);
            enabled = d >= 0 && d < c->base;
        }
        const char *label = k->label;
        if (c->mode == SCIENTIFIC && !strcmp(label, "Deg"))
            label = c->deg ? "Deg" : "Rad";
        if (c->mode == BASIC && k->kind == 1 && c->op && c->fresh && strcmp(label, "=") &&
            !strcmp(op_symbol(c->op), label)) {
            bg = RGB(255, 255, 255);
            fg = RGB(255, 159, 10);
        }
        if (!calc_key(ui, b, label, bg, fg, enabled))
            continue;
        if (c->mode == BASIC) {
            basic_press(c, label);
        } else if (!strcmp(label, "=")) {
            evaluate(c);
        } else if (!strcmp(label, "AC")) {
            clear(c);
            refresh(c);
        } else if (!strcmp(label, "⌫")) {
            erase(c);
        } else if (!strcmp(label, "Deg") || !strcmp(label, "Rad")) {
            c->deg = !c->deg;
            app_state_set_on("Calculator", "degrees", c->deg);
            refresh(c);
        } else if (!strcmp(label, "Ans")) {
            put(c, "(");
            put(c, c->shown);
            put(c, ")");
        } else {
            put(c, k->code);
        }
    }
}

static void frame(struct ui *ui, void *data)
{
    struct calc *c = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, RGB(28, 28, 30));
    /* the mode: a small button next to the window controls with the four modes in its menu */
    static const char *const names[4] = {"Basic", "Scientific", "Programmer", "Converter"};
    struct rect mode_btn = {66, 7, gfx_text_width(&font_small, names[c->mode]) + 30, 22};
    if (ui->c) {
        gfx_round(ui->c, mode_btn, 11, ui_hover(ui, mode_btn) ? RGB(70, 70, 75) : RGB(52, 52, 56));
        gfx_text(ui->c, mode_btn.x + 10, mode_btn.y + 4, &font_small, names[c->mode], RGB(235, 235, 240));
        gfx_symbol(ui->c, mode_btn.x + mode_btn.w - 16, mode_btn.y + 7, SYM_DOWN, 8, RGB(180, 180, 186));
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui->e->button == BUTTON_LEFT &&
        rect_contains(mode_btn, ui->e->x, ui->e->y)) {
        c->picking = 4;
        ui_menu(c->win, mode_btn.x, mode_btn.y + mode_btn.h, names, 4, pick_unit);
        ui->consumed = true;
    }
    int top = DISPLAY_H + 14;
    if (c->mode == BASIC) {
        display(ui, c, W, c->history, c->entry);
        keypad(ui, c, basic_keys, ARRAY_SIZE(basic_keys), 4, top, W, H);
    } else if (c->mode == SCIENTIFIC) {
        display(ui, c, W, c->line, c->shown);
        keypad(ui, c, sci_keys, ARRAY_SIZE(sci_keys), 7, top, W, H);
    } else if (c->mode == PROGRAMMER) {
        display(ui, c, W, c->line, c->shown);
        /* the value in every base; a click picks the base to type in */
        int64_t v = 0;
        const char *why;
        bool ok = prog_eval(c->expr, c->base, c->bits, &v, &why) || !c->expr[0];
        static const int bases[4] = {16, 10, 8, 2};
        static const char *const base_names[4] = {"HEX", "DEC", "OCT", "BIN"};
        int y = top - 6;
        for (int i = 0; i < 4; i++, y += 22) {
            struct rect row = {10, y, W - 20, 20};
            bool sel = c->base == bases[i];
            if (ui->c) {
                if (sel)
                    gfx_round(ui->c, row, 6, RGB(58, 58, 62));
                char text[96];
                prog_format(ok ? v : 0, bases[i], c->bits, text, sizeof(text));
                gfx_text(ui->c, row.x + 8, row.y + 3, &font_bold, base_names[i], sel ? RGB(255, 159, 10)
                                                                                     : RGB(150, 150, 156));
                gfx_text_box(ui->c, (struct rect){row.x + 50, row.y, row.w - 58, row.h}, &font_mono, text,
                             RGB(230, 230, 235), ALIGN_RIGHT);
            }
            if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(row, ui->e->x, ui->e->y)) {
                set_base(c, bases[i]);
                ui->consumed = true;
            }
        }
        static const char *const sizes[4] = {"QWORD", "DWORD", "WORD", "BYTE"};
        int size = c->bits == 64 ? 0 : c->bits == 32 ? 1 : c->bits == 16 ? 2 : 3;
        if (ui_segmented(ui, (struct rect){10, y + 4, W - 20, 22}, sizes, 4, &size))
            set_bits(c, size == 0 ? 64 : size == 1 ? 32 : size == 2 ? 16 : 8);
        keypad(ui, c, prog_keys, ARRAY_SIZE(prog_keys), 5, y + 34, W, H);
    } else {
        converter(ui, c, W, 34);
    }
    int k = ui_key(ui);
    if (k == KEY_NONE)
        return;
    int base = KEY_BASE(k);
    if (k == KEY_CTRL('1') || k == KEY_CTRL('2') || k == KEY_CTRL('3') || k == KEY_CTRL('4')) {
        set_mode(c, (enum mode)(k - KEY_CTRL('1')));
    } else if (c->mode == CONVERTER) {
        return; /* typing goes to the value field */
    } else if (k == KEY_CTRL('c')) {
        const char *t = c->mode == BASIC ? c->entry : c->shown;
        gui_set_clipboard(t, strlen(t));
    } else if (c->mode == BASIC) {
        if ((k >= '0' && k <= '9') || k == '.' || k == ',')
            digit(c, k == ',' ? '.' : (char)k);
        else if (k == '+' || k == '-' || k == '*' || k == '/')
            operator(c, (char)k);
        else if (k == '=' || base == KEY_ENTER || k == '\r')
            equals(c);
        else if (k == KEY_BACKSPACE)
            backspace(c);
        else if (base == KEY_ESC)
            clear(c);
        else if (k == '%')
            unary(c, '%');
        else if (k == KEY_CTRL('v')) {
            format(parse_number(gui_clipboard()), c->entry, sizeof(c->entry));
            c->fresh = false;
        } else
            return;
    } else {
        char one[2] = {(char)k, 0};
        if (k >= 'a' && k <= 'f' && c->mode == PROGRAMMER)
            one[0] = (char)(k - 32);
        if (k == '=' || base == KEY_ENTER || k == '\r')
            evaluate(c);
        else if (k == KEY_BACKSPACE)
            erase(c);
        else if (base == KEY_ESC)
            clear(c), refresh(c);
        else if (k == KEY_CTRL('v')) {
            char tmp[EXPR_MAX];
            if (c->mode == PROGRAMMER ? prog_from_text(gui_clipboard(), tmp, sizeof(tmp))
                                      : sci_from_text(gui_clipboard(), tmp, sizeof(tmp)))
                put(c, tmp);
        } else if (c->mode == PROGRAMMER && ((one[0] >= '0' && one[0] <= '9') || (one[0] >= 'A' && one[0] <= 'F'))) {
            int d = digit_value(one[0]);
            if (d < c->base)
                put(c, one);
        } else if (c->mode == PROGRAMMER && k == '&')
            put(c, "&");
        else if (c->mode == PROGRAMMER && k == '|')
            put(c, "|");
        else if (c->mode == PROGRAMMER && k == '^')
            put(c, "x");
        else if (c->mode == PROGRAMMER && k == '~')
            put(c, "~");
        else if (c->mode == PROGRAMMER && k == '%')
            put(c, "m");
        else if (c->mode == PROGRAMMER && k == '<')
            put(c, "<");
        else if (c->mode == PROGRAMMER && k == '>')
            put(c, ">");
        else if (c->mode == SCIENTIFIC && ((k >= '0' && k <= '9') || k == '.' || strchr("^!%", k)))
            put(c, one);
        else if (c->mode == SCIENTIFIC && k && k < 128 && strchr("sctlr", k)) {
            char fn[3] = {(char)k, '(', 0};
            put(c, fn);
        } else if (c->mode == SCIENTIFIC && (k == 'p' || k == 'e'))
            put(c, one);
        else if (c->mode == SCIENTIFIC && k == ',')
            put(c, ".");
        else if (k == '+' || k == '-' || k == '*' || k == '/' || k == '(' || k == ')' ||
                 (c->mode == PROGRAMMER && k >= '0' && k <= '9'))
            put(c, one);
        else
            return;
    }
    ui->consumed = true;
}

static void paint(struct window *w, struct canvas *cv)
{
    struct calc *c = w->data;
    ui_run(w, &c->st, cv, NULL, frame, c);
}

static void event(struct window *w, struct gui_event *e)
{
    struct calc *c = w->data;
    ui_run(w, &c->st, NULL, e, frame, c);
}

static void destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

/* arg: a mode's name to open in */
static void launch(const char *arg)
{
    struct calc *c = kzalloc(sizeof(*c));
    if (!c)
        return;
    c->mode = (enum mode)CLAMP(app_state_number("Calculator", "mode", BASIC), BASIC, CONVERTER);
    static const char *const names[4] = {"basic", "scientific", "programmer", "converter"};
    if (arg && arg[0]) {
        const char *word = strrchr(arg, '/') ? strrchr(arg, '/') + 1 : arg;
        for (int i = 0; i < 4; i++)
            if (!str_ncasecmp(word, names[i], strlen(word)))
                c->mode = (enum mode)i;
    }
    c->deg = app_state_on("Calculator", "degrees", true);
    c->base = (int)app_state_number("Calculator", "base", 10);
    c->bits = (int)app_state_number("Calculator", "bits", 64);
    if (c->base != 2 && c->base != 8 && c->base != 16)
        c->base = 10;
    if (c->bits != 8 && c->bits != 16 && c->bits != 32)
        c->bits = 64;
    c->cat = (int)CLAMP(app_state_number("Calculator", "category", 0), 0, (long)ARRAY_SIZE(categories) - 1);
    c->to = 1;
    ui_text_set(&c->value, "1");
    c->value.complete = COMPLETE_OFF;
    clear(c);
    refresh(c);
    c->win = wm_open("Calculator", ICON_CALC, widths[c->mode], heights[c->mode], &ops, c, WIN_FULLSIZE | WIN_NOTITLE);
    if (!c->win) {
        kfree(c);
        return;
    }
    wm_set_menus(c->win, menus, ARRAY_SIZE(menus));
}

static const struct gui_app calc_app = {"Calculator", "Basic, scientific and programmer calculator and unit converter",
                                        ICON_CALC, launch, NULL, NULL};

/* ---- commands ---- */

static void join(int argc, char **argv, int from, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from)
            str_append(out, " ", cap);
        str_append(out, argv[i], cap);
    }
}

COMMAND(scicalc, "sci", "text", "scicalc [-r] <expression>",
        "work out an expression: + - * / ^ ! % ( ), sin cos tan asin acos atan (degrees; -r radians), ln log sqrt "
        "abs, pi e")
{
    int from = 1;
    bool deg = true;
    if (argc > 1 && !strcmp(argv[1], "-r"))
        deg = false, from++;
    if (from >= argc)
        return command_usage(sh, command_find("scicalc"));
    char text[EXPR_MAX], expr[EXPR_MAX], out[64];
    join(argc, argv, from, text, sizeof(text));
    double v;
    const char *why;
    if (!sci_from_text(text, expr, sizeof(expr)))
        return sh_error(sh, "scicalc", "cannot read %s", text);
    if (!sci_eval(expr, deg, &v, &why))
        return sh_error(sh, "scicalc", "%s", why ? why : "the expression is not complete");
    format(v, out, sizeof(out));
    sh_printf(sh, "%s\n", out);
    return 0;
}

COMMAND(progcalc, "pcalc", "text", "progcalc [-8|-16|-32] <expression>",
        "integer arithmetic: + - * / mod and or xor not << >>, 0x/0b/0o numbers; shows hex, dec, oct and bin")
{
    int from = 1, bits = 64;
    if (argc > 1 && (!strcmp(argv[1], "-8") || !strcmp(argv[1], "-16") || !strcmp(argv[1], "-32")))
        bits = (int)str_to_long(argv[1] + 1, NULL, 10), from++;
    if (from >= argc)
        return command_usage(sh, command_find("progcalc"));
    char text[EXPR_MAX], expr[EXPR_MAX * 2];
    join(argc, argv, from, text, sizeof(text));
    int64_t v;
    const char *why;
    if (!prog_from_text(text, expr, sizeof(expr)))
        return sh_error(sh, "progcalc", "cannot read %s", text);
    if (!prog_eval(expr, 10, bits, &v, &why))
        return sh_error(sh, "progcalc", "%s", why ? why : "the expression is not complete");
    static const int bases[4] = {16, 10, 8, 2};
    static const char *const names[4] = {"hex", "dec", "oct", "bin"};
    for (int i = 0; i < 4; i++) {
        char out[96];
        prog_format(v, bases[i], bits, out, sizeof(out));
        sh_printf(sh, "%s %s\n", names[i], out);
    }
    return 0;
}

COMMAND(convert, "units", "text", "convert <value> <unit> <unit> | convert list",
        "convert between units of length, area, volume, weight, temperature, speed, time, data and pressure")
{
    if (argc == 2 && !strcmp(argv[1], "list")) {
        for (size_t c = 0; c < ARRAY_SIZE(categories); c++) {
            sh_printf(sh, "%-12s", categories[c].name);
            for (int i = 0; i < categories[c].count; i++)
                sh_printf(sh, " %s", categories[c].units[i].symbol);
            sh_printf(sh, "\n");
        }
        return 0;
    }
    if (argc != 4)
        return command_usage(sh, command_find("convert"));
    int c1, u1, c2, u2;
    if (!find_unit(argv[2], &c1, &u1))
        return sh_error(sh, "convert", "unknown unit %s (convert list shows them)", argv[2]);
    if (!find_unit(argv[3], &c2, &u2))
        return sh_error(sh, "convert", "unknown unit %s (convert list shows them)", argv[3]);
    if (c1 != c2)
        return sh_error(sh, "convert", "%s is %s and %s is %s", argv[2], categories[c1].name, argv[3],
                        categories[c2].name);
    char out[64];
    format(snap(convert(c1, u1, u2, parse_number(argv[1])) * 1e9) / 1e9, out, sizeof(out));
    sh_printf(sh, "%s %s\n", out, categories[c1].units[u2].symbol);
    return 0;
}

static const struct command *const commands[] = {&cmddef_scicalc, &cmddef_progcalc, &cmddef_convert};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&calc_app, NULL, &ext);
