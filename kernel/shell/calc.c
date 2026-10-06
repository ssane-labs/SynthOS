#include <synth/fmt.h>
#include <synth/string.h>

#include "internal.h"

struct calc {
    struct shell *sh;
    const char *p;
    const char *error;
};

static long parse_expr(struct calc *c, int min_prec);

static void skip(struct calc *c)
{
    while (is_space((uint8_t)*c->p))
        c->p++;
}

static long parse_primary(struct calc *c)
{
    skip(c);
    char ch = *c->p;
    if (ch == '(') {
        c->p++;
        long v = parse_expr(c, 0);
        skip(c);
        if (*c->p != ')')
            c->error = "missing ')'";
        else
            c->p++;
        return v;
    }
    if (ch == '-' || ch == '+' || ch == '!' || ch == '~') {
        c->p++;
        long v = parse_primary(c);
        return ch == '-' ? -v : ch == '+' ? v : ch == '!' ? !v : ~v;
    }
    if (ch == '\'' && c->p[1] && c->p[2] == '\'') {
        long v = (uint8_t)c->p[1];
        c->p += 3;
        return v;
    }
    if (is_digit((uint8_t)ch)) {
        const char *end;
        long v = str_to_long(c->p, &end, 0);
        c->p = end;
        return v;
    }
    if (is_alpha((uint8_t)ch) || ch == '_' || ch == '$') {
        if (ch == '$')
            c->p++;
        char name[64];
        size_t n = 0;
        while (is_ident((uint8_t)*c->p) && n + 1 < sizeof(name))
            name[n++] = *c->p++;
        name[n] = 0;
        const char *val = c->sh ? sh_get(c->sh, name) : NULL;
        long v = 0;
        if (val && *val && !str_parse_long(val, &v))
            c->error = "variable is not a number";
        return v;
    }
    c->error = ch ? "unexpected character" : "unexpected end of expression";
    return 0;
}

static int binary_op(const char *p, int *len)
{
    static const struct {
        const char *op;
        int prec;
    } ops[] = {
        {"||", 1}, {"&&", 2}, {"==", 6}, {"!=", 6}, {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8},
        {"|", 3},  {"^", 4},  {"&", 5},  {"<", 7},  {">", 7},  {"+", 9},  {"-", 9},  {"*", 10},
        {"/", 10}, {"%", 10},
    };
    for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
        size_t n = strlen(ops[i].op);
        if (strncmp(p, ops[i].op, n) == 0) {
            *len = (int)n;
            return ops[i].prec;
        }
    }
    return -1;
}

static long apply(struct calc *c, const char *op, int len, long a, long b)
{
    switch (op[0]) {
    case '|': return len == 2 ? (a || b) : (a | b);
    case '&': return len == 2 ? (a && b) : (a & b);
    case '^': return a ^ b;
    case '=': return a == b;
    case '!': return a != b;
    case '<': return len == 2 && op[1] == '<' ? (long)((unsigned long)a << (b & 63)) : len == 2 ? a <= b : a < b;
    case '>': return len == 2 && op[1] == '>' ? a >> (b & 63) : len == 2 ? a >= b : a > b;
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
    case '%':
        if (b == 0) {
            c->error = "division by zero";
            return 0;
        }
        return op[0] == '/' ? a / b : a % b;
    }
    return 0;
}

static long parse_expr(struct calc *c, int min_prec)
{
    long lhs = parse_primary(c);
    for (;;) {
        skip(c);
        int len;
        int prec = binary_op(c->p, &len);
        if (prec < 0 || prec < min_prec || c->error)
            return lhs;
        const char *op = c->p;
        c->p += len;
        long rhs = parse_expr(c, prec + 1);
        lhs = apply(c, op, len, lhs, rhs);
    }
}

int calc_eval(struct shell *sh, const char *expr, long *out, char *err, size_t errcap)
{
    struct calc c = {sh, expr, NULL};
    long v = parse_expr(&c, 0);
    skip(&c);
    if (!c.error && *c.p)
        c.error = "unexpected trailing input";
    if (c.error) {
        ksnprintf(err, errcap, "%s", c.error);
        return -1;
    }
    *out = v;
    return 0;
}
