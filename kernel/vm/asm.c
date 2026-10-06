#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/sx.h>

#define SYM_LEN 64
#define MAX_OPERANDS 16
#define MAX_INCLUDE_DEPTH 8

struct symbol {
    char name[SYM_LEN];
    uint32_t value;
};

struct assembler {
    const struct sasm_options *opts;
    struct symbol *syms;
    size_t nsyms;
    size_t capsyms;
    uint8_t *out;
    size_t len;
    size_t cap;
    uint32_t pc;
    int pass;
    char scope[SYM_LEN];
    char entry[SYM_LEN];
    uint32_t mem_size;
    const char *file;
    int line;
    char *error;
    size_t errcap;
    bool failed;
    bool unresolved;
    int depth;
};

static void fail(struct assembler *a, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void fail(struct assembler *a, const char *fmt, ...)
{
    if (a->failed)
        return;
    a->failed = true;
    int n = ksnprintf(a->error, a->errcap, "%s:%d: ", a->file, a->line);
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(a->error + n, a->errcap - (size_t)n, fmt, ap);
    va_end(ap);
}

static struct symbol *sym_find(struct assembler *a, const char *name)
{
    for (size_t i = 0; i < a->nsyms; i++)
        if (strcmp(a->syms[i].name, name) == 0)
            return &a->syms[i];
    return NULL;
}

static void sym_define(struct assembler *a, const char *name, uint32_t value, bool redefinable)
{
    struct symbol *s = sym_find(a, name);
    if (s) {
        if (a->pass == 1 && !redefinable)
            fail(a, "symbol '%s' defined twice", name);
        s->value = value;
        return;
    }
    if (a->nsyms == a->capsyms) {
        size_t cap = a->capsyms ? a->capsyms * 2 : 64;
        struct symbol *syms = krealloc(a->syms, cap * sizeof(*syms));
        if (!syms) {
            fail(a, "out of memory");
            return;
        }
        a->syms = syms;
        a->capsyms = cap;
    }
    struct symbol *n = &a->syms[a->nsyms++];
    str_copy(n->name, name, sizeof(n->name));
    n->value = value;
}

static void scoped_name(struct assembler *a, const char *name, size_t len, char *out)
{
    char raw[SYM_LEN];
    len = MIN(len, sizeof(raw) - 1);
    memcpy(raw, name, len);
    raw[len] = 0;
    if (raw[0] == '.')
        ksnprintf(out, SYM_LEN, "%s%s", a->scope, raw);
    else
        str_copy(out, raw, SYM_LEN);
}

static void emit8(struct assembler *a, uint8_t v)
{
    if (a->pass == 2) {
        size_t off = a->pc - SX_LOAD_BASE;
        if (off >= a->cap) {
            size_t cap = a->cap ? a->cap * 2 : 4096;
            while (cap <= off)
                cap *= 2;
            uint8_t *out = krealloc(a->out, cap);
            if (!out) {
                fail(a, "out of memory");
                return;
            }
            memset(out + a->cap, 0, cap - a->cap);
            a->out = out;
            a->cap = cap;
        }
        a->out[off] = v;
        if (off + 1 > a->len)
            a->len = off + 1;
    }
    a->pc++;
}

static void emit16(struct assembler *a, uint32_t v)
{
    emit8(a, (uint8_t)v);
    emit8(a, (uint8_t)(v >> 8));
}

static void emit32(struct assembler *a, uint32_t v)
{
    emit16(a, v);
    emit16(a, v >> 16);
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int parse_escape(const char **pp)
{
    const char *p = *pp;
    int c = (uint8_t)*p++;
    if (c == '\\') {
        c = (uint8_t)*p++;
        switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case '0': c = 0; break;
        case 'e': c = 27; break;
        case 'a': c = 7; break;
        case 'b': c = 8; break;
        case 'x': {
            int v = 0;
            for (int i = 0; i < 2 && is_xdigit((uint8_t)*p); i++, p++)
                v = v * 16 + (is_digit((uint8_t)*p) ? *p - '0' : to_lower((uint8_t)*p) - 'a' + 10);
            c = v;
            break;
        }
        default: break;
        }
    }
    *pp = p;
    return c;
}

struct expr {
    struct assembler *a;
    const char *p;
};

static int32_t expr_binary(struct expr *e, int min_prec);

static int32_t expr_primary(struct expr *e)
{
    struct assembler *a = e->a;
    e->p = skip_ws(e->p);
    char c = *e->p;
    if (c == '(') {
        e->p++;
        int32_t v = expr_binary(e, 0);
        e->p = skip_ws(e->p);
        if (*e->p != ')')
            fail(a, "missing ')'");
        else
            e->p++;
        return v;
    }
    if (c == '-' || c == '~' || c == '+' || c == '!') {
        e->p++;
        int32_t v = expr_primary(e);
        return c == '-' ? -v : c == '~' ? ~v : c == '!' ? !v : v;
    }
    if (c == '\'') {
        e->p++;
        int v = parse_escape(&e->p);
        if (*e->p != '\'')
            fail(a, "bad character literal");
        else
            e->p++;
        return v;
    }
    if (c == '$' && !is_ident((uint8_t)e->p[1])) {
        e->p++;
        return (int32_t)a->pc;
    }
    if (is_digit((uint8_t)c)) {
        const char *end;
        long v = str_to_long(e->p, &end, 0);
        e->p = end;
        if (is_ident((uint8_t)*e->p))
            fail(a, "bad number");
        return (int32_t)v;
    }
    if (is_alpha((uint8_t)c) || c == '_' || c == '.') {
        const char *start = e->p;
        e->p++;
        while (is_ident((uint8_t)*e->p) || *e->p == '.')
            e->p++;
        char name[SYM_LEN];
        scoped_name(a, start, (size_t)(e->p - start), name);
        struct symbol *s = sym_find(a, name);
        if (s)
            return (int32_t)s->value;
        int sys = sx_find_syscall(name);
        if (str_starts(name, "SYS_") && (sys = sx_find_syscall(name + 4)) >= 0)
            return sys;
        a->unresolved = true;
        if (a->pass == 2)
            fail(a, "undefined symbol '%s'", name);
        return 0;
    }
    fail(a, c ? "unexpected '%c' in expression" : "missing expression%c", c);
    return 0;
}

static int prec_of(const char *p, int *len)
{
    static const struct {
        const char *op;
        int prec;
    } ops[] = {{"<<", 5}, {">>", 5}, {"|", 1}, {"^", 2}, {"&", 3}, {"+", 6}, {"-", 6}, {"*", 7}, {"/", 7}, {"%", 7}};
    for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
        size_t n = strlen(ops[i].op);
        if (strncmp(p, ops[i].op, n) == 0) {
            *len = (int)n;
            return ops[i].prec;
        }
    }
    return -1;
}

static int32_t expr_binary(struct expr *e, int min_prec)
{
    int32_t lhs = expr_primary(e);
    for (;;) {
        e->p = skip_ws(e->p);
        int len;
        int prec = prec_of(e->p, &len);
        if (prec < 0 || prec < min_prec)
            return lhs;
        char op = *e->p;
        e->p += len;
        int32_t rhs = expr_binary(e, prec + 1);
        switch (op) {
        case '+': lhs += rhs; break;
        case '-': lhs -= rhs; break;
        case '*': lhs *= rhs; break;
        case '/':
        case '%':
            if (rhs == 0) {
                if (e->a->pass == 2)
                    fail(e->a, "division by zero");
                rhs = 1;
            }
            lhs = op == '/' ? lhs / rhs : lhs % rhs;
            break;
        case '&': lhs &= rhs; break;
        case '|': lhs |= rhs; break;
        case '^': lhs ^= rhs; break;
        case '<': lhs = (int32_t)((uint32_t)lhs << (rhs & 31)); break;
        case '>': lhs = (int32_t)((uint32_t)lhs >> (rhs & 31)); break;
        }
    }
}

static int32_t eval(struct assembler *a, const char *text)
{
    struct expr e = {a, text};
    int32_t v = expr_binary(&e, 0);
    e.p = skip_ws(e.p);
    if (*e.p)
        fail(a, "unexpected '%s' after expression", e.p);
    return v;
}

static int parse_reg(const char *s)
{
    s = skip_ws(s);
    char name[8];
    size_t n = 0;
    while (is_alnum((uint8_t)s[n]) && n < sizeof(name) - 1)
        name[n] = (char)to_lower((uint8_t)s[n]), n++;
    name[n] = 0;
    if (*skip_ws(s + n))
        return -1;
    if (strcmp(name, "sp") == 0)
        return SX_REG_SP;
    if (strcmp(name, "fp") == 0)
        return SX_REG_FP;
    long r;
    if (name[0] == 'r' && str_parse_long(name + 1, &r) && r >= 0 && r < SX_REG_COUNT && is_digit((uint8_t)name[1]))
        return (int)r;
    return -1;
}

static int split_operands(char *s, char **ops)
{
    int n = 0, depth = 0;
    char quote = 0;
    s = (char *)skip_ws(s);
    if (!*s)
        return 0;
    ops[n++] = s;
    for (char *p = s; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '[' || *p == '(') {
            depth++;
        } else if (*p == ']' || *p == ')') {
            depth--;
        } else if (*p == ',' && depth == 0) {
            *p = 0;
            if (n < MAX_OPERANDS)
                ops[n++] = (char *)skip_ws(p + 1);
        }
    }
    for (int i = 0; i < n; i++)
        str_trim(ops[i]);
    return n;
}

static bool parse_mem(struct assembler *a, char *s, int *base, int32_t *off)
{
    size_t n = strlen(s);
    if (n < 2 || s[0] != '[' || s[n - 1] != ']') {
        fail(a, "expected memory operand like [r1+4]");
        return false;
    }
    s[n - 1] = 0;
    char *inner = (char *)skip_ws(s + 1);
    char *p = inner;
    while (is_alnum((uint8_t)*p))
        p++;
    char saved = *p;
    *p = 0;
    int reg = parse_reg(inner);
    *p = saved;
    if (reg < 0) {
        *base = SX_REG_NONE;
        *off = eval(a, inner);
        return true;
    }
    *base = reg;
    p = (char *)skip_ws(p);
    *off = *p ? eval(a, p) : 0;
    return true;
}

static void emit_insn(struct assembler *a, int op, int ra, int rb, int rc, bool imm, int32_t value)
{
    enum sx_shape shape = sx_ops[op].shape;
    bool always = shape == SHAPE_LOAD || shape == SHAPE_STORE || shape == SHAPE_SYS;
    emit8(a, (uint8_t)(op | (imm && !always ? SX_IMM_FLAG : 0)));
    emit8(a, (uint8_t)ra);
    emit8(a, (uint8_t)rb);
    emit8(a, (uint8_t)rc);
    if (imm || always)
        emit32(a, (uint32_t)value);
}

static int need_reg(struct assembler *a, const char *s)
{
    int r = parse_reg(s);
    if (r < 0)
        fail(a, "expected register, got '%s'", s);
    return r < 0 ? 0 : r;
}

static void instruction(struct assembler *a, int op, char **ops, int n)
{
    enum sx_shape shape = sx_ops[op].shape;
    static const int expected[] = {
        [SHAPE_NONE] = 0, [SHAPE_R] = 1, [SHAPE_RR] = 2, [SHAPE_R_RI] = 2, [SHAPE_R_R_RI] = 3,
        [SHAPE_RI] = 1,   [SHAPE_LOAD] = 2, [SHAPE_STORE] = 2, [SHAPE_SYS] = 1,
    };
    if (shape == SHAPE_R_R_RI && n == 2) {
        char *tmp[3] = {ops[0], ops[0], ops[1]};
        instruction(a, op, tmp, 3);
        return;
    }
    if (n != expected[shape]) {
        fail(a, "'%s' expects %d operand(s)", sx_ops[op].name, expected[shape]);
        return;
    }
    int r, base;
    int32_t v = 0;
    switch (shape) {
    case SHAPE_NONE:
        emit_insn(a, op, 0, 0, 0, false, 0);
        break;
    case SHAPE_R:
        emit_insn(a, op, need_reg(a, ops[0]), 0, 0, false, 0);
        break;
    case SHAPE_RR:
        emit_insn(a, op, need_reg(a, ops[0]), need_reg(a, ops[1]), 0, false, 0);
        break;
    case SHAPE_R_RI:
        r = parse_reg(ops[1]);
        if (r >= 0)
            emit_insn(a, op, need_reg(a, ops[0]), r, 0, false, 0);
        else
            emit_insn(a, op, need_reg(a, ops[0]), 0, 0, true, eval(a, ops[1]));
        break;
    case SHAPE_R_R_RI:
        r = parse_reg(ops[2]);
        if (r >= 0)
            emit_insn(a, op, need_reg(a, ops[0]), need_reg(a, ops[1]), r, false, 0);
        else
            emit_insn(a, op, need_reg(a, ops[0]), need_reg(a, ops[1]), 0, true, eval(a, ops[2]));
        break;
    case SHAPE_RI:
        r = parse_reg(ops[0]);
        if (r >= 0)
            emit_insn(a, op, r, 0, 0, false, 0);
        else
            emit_insn(a, op, 0, 0, 0, true, eval(a, ops[0]));
        break;
    case SHAPE_LOAD:
        if (parse_mem(a, ops[1], &base, &v))
            emit_insn(a, op, need_reg(a, ops[0]), base, 0, true, v);
        break;
    case SHAPE_STORE:
        if (parse_mem(a, ops[0], &base, &v))
            emit_insn(a, op, need_reg(a, ops[1]), base, 0, true, v);
        break;
    case SHAPE_SYS: {
        int sys = sx_find_syscall(ops[0]);
        emit_insn(a, op, 0, 0, 0, true, sys >= 0 ? sys : eval(a, ops[0]));
        break;
    }
    }
}

static bool parse_string(struct assembler *a, const char *s, char *out, size_t cap, size_t *len)
{
    if (*s != '"') {
        fail(a, "expected string");
        return false;
    }
    s++;
    size_t n = 0;
    while (*s && *s != '"') {
        int c = parse_escape(&s);
        if (n < cap)
            out[n++] = (char)c;
    }
    if (*s != '"') {
        fail(a, "unterminated string");
        return false;
    }
    *len = n;
    return true;
}

static int assemble_text(struct assembler *a, const char *name, const char *text);

static void directive(struct assembler *a, const char *dir, char *rest)
{
    char *ops[MAX_OPERANDS];
    int n;
    char buf[512];
    size_t len;
    if (strcmp(dir, ".entry") == 0) {
        scoped_name(a, rest, strlen(rest), a->entry);
    } else if (strcmp(dir, ".mem") == 0 || strcmp(dir, ".memory") == 0) {
        a->mem_size = (uint32_t)eval(a, rest);
    } else if (strcmp(dir, ".byte") == 0 || strcmp(dir, ".db") == 0) {
        n = split_operands(rest, ops);
        for (int i = 0; i < n; i++) {
            if (ops[i][0] == '"') {
                if (parse_string(a, ops[i], buf, sizeof(buf), &len))
                    for (size_t k = 0; k < len; k++)
                        emit8(a, (uint8_t)buf[k]);
            } else {
                emit8(a, (uint8_t)eval(a, ops[i]));
            }
        }
    } else if (strcmp(dir, ".half") == 0 || strcmp(dir, ".dh") == 0) {
        n = split_operands(rest, ops);
        for (int i = 0; i < n; i++)
            emit16(a, (uint32_t)eval(a, ops[i]));
    } else if (strcmp(dir, ".word") == 0 || strcmp(dir, ".dw") == 0 || strcmp(dir, ".dd") == 0) {
        n = split_operands(rest, ops);
        for (int i = 0; i < n; i++)
            emit32(a, (uint32_t)eval(a, ops[i]));
    } else if (strcmp(dir, ".ascii") == 0 || strcmp(dir, ".asciz") == 0 || strcmp(dir, ".string") == 0) {
        if (parse_string(a, skip_ws(rest), buf, sizeof(buf), &len)) {
            for (size_t k = 0; k < len; k++)
                emit8(a, (uint8_t)buf[k]);
            if (strcmp(dir, ".ascii") != 0)
                emit8(a, 0);
        }
    } else if (strcmp(dir, ".space") == 0 || strcmp(dir, ".zero") == 0 || strcmp(dir, ".fill") == 0) {
        n = split_operands(rest, ops);
        a->unresolved = false;
        int32_t count = n > 0 ? eval(a, ops[0]) : 0;
        uint8_t fill = n > 1 ? (uint8_t)eval(a, ops[1]) : 0;
        if (a->unresolved || count < 0 || count > (1 << 24))
            fail(a, "bad .space size");
        else
            for (int32_t i = 0; i < count; i++)
                emit8(a, fill);
    } else if (strcmp(dir, ".align") == 0) {
        int32_t al = eval(a, rest);
        if (al <= 0 || (al & (al - 1)))
            fail(a, ".align needs a power of two");
        else
            while (a->pc % (uint32_t)al)
                emit8(a, 0);
    } else if (strcmp(dir, ".equ") == 0 || strcmp(dir, ".set") == 0 || strcmp(dir, ".define") == 0) {
        n = split_operands(rest, ops);
        if (n != 2) {
            fail(a, "usage: %s NAME, value", dir);
            return;
        }
        char sym[SYM_LEN];
        scoped_name(a, ops[0], strlen(ops[0]), sym);
        sym_define(a, sym, (uint32_t)eval(a, ops[1]), true);
    } else if (strcmp(dir, ".include") == 0) {
        if (!parse_string(a, skip_ws(rest), buf, sizeof(buf) - 1, &len))
            return;
        buf[len] = 0;
        char *text = NULL;
        if (a->depth >= MAX_INCLUDE_DEPTH) {
            fail(a, "includes nested too deeply");
            return;
        }
        if (!a->opts || !a->opts->load || a->opts->load(a->opts->ctx, buf, &text) != 0) {
            fail(a, "cannot include '%s'", buf);
            return;
        }
        a->depth++;
        assemble_text(a, buf, text);
        a->depth--;
        kfree(text);
    } else {
        fail(a, "unknown directive '%s'", dir);
    }
}

static void strip_comment(char *s)
{
    char quote = 0;
    for (char *p = s; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == ';' || *p == '#') {
            *p = 0;
            return;
        }
    }
}

static void assemble_line(struct assembler *a, char *line)
{
    strip_comment(line);
    char *s = str_trim(line);
    for (;;) {
        char *p = s;
        if (*p == '.' || is_alpha((uint8_t)*p) || *p == '_') {
            p++;
            while (is_ident((uint8_t)*p) || *p == '.')
                p++;
            if (*p == ':') {
                char name[SYM_LEN];
                scoped_name(a, s, (size_t)(p - s), name);
                if (s[0] != '.')
                    str_copy(a->scope, name, sizeof(a->scope));
                sym_define(a, name, a->pc, a->pass == 2);
                s = (char *)skip_ws(p + 1);
                continue;
            }
        }
        break;
    }
    if (!*s)
        return;
    char *p = s;
    while (*p && !is_space((uint8_t)*p))
        p++;
    char word[32];
    size_t wl = MIN((size_t)(p - s), sizeof(word) - 1);
    memcpy(word, s, wl);
    word[wl] = 0;
    char *rest = (char *)skip_ws(p);

    if (rest[0] == '=' && rest[1] != '=') {
        char sym[SYM_LEN];
        scoped_name(a, word, strlen(word), sym);
        sym_define(a, sym, (uint32_t)eval(a, rest + 1), true);
        return;
    }
    if (word[0] == '.') {
        str_lower(word);
        directive(a, word, rest);
        return;
    }
    int op = sx_find_op(word);
    if (op < 0) {
        fail(a, "unknown instruction '%s'", word);
        return;
    }
    char *ops[MAX_OPERANDS];
    int n = split_operands(rest, ops);
    instruction(a, op, ops, n);
}

static int assemble_text(struct assembler *a, const char *name, const char *text)
{
    const char *saved_file = a->file;
    int saved_line = a->line;
    a->file = name;
    a->line = 0;
    char line[512];
    const char *p = text;
    while (*p && !a->failed) {
        size_t n = 0;
        while (*p && *p != '\n') {
            if (n + 1 < sizeof(line))
                line[n++] = *p;
            p++;
        }
        if (*p)
            p++;
        line[n] = 0;
        a->line++;
        assemble_line(a, line);
    }
    a->file = saved_file;
    a->line = saved_line;
    return a->failed ? -1 : 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int sasm_assemble(const char *name, const char *source, const struct sasm_options *opts, struct sasm_output *out)
{
    struct assembler a = {.opts = opts, .error = out->error, .errcap = sizeof(out->error)};
    out->data = NULL;
    out->size = 0;
    out->error[0] = 0;
    for (a.pass = 1; a.pass <= 2 && !a.failed; a.pass++) {
        a.pc = SX_LOAD_BASE;
        a.scope[0] = 0;
        a.mem_size = SX_DEFAULT_MEM;
        a.entry[0] = 0;
        assemble_text(&a, name, source);
    }
    uint32_t entry = SX_LOAD_BASE;
    if (!a.failed) {
        const char *entry_name = a.entry[0] ? a.entry : "main";
        struct symbol *s = sym_find(&a, entry_name);
        if (s)
            entry = s->value;
        else if (a.entry[0])
            fail(&a, "entry point '%s' is not defined", a.entry);
    }
    uint32_t image = (uint32_t)a.len;
    if (!a.failed && a.mem_size < image + SX_LOAD_BASE + SX_MIN_STACK)
        a.mem_size = ALIGN_UP(image + SX_LOAD_BASE + SX_MIN_STACK, 4096u);
    if (!a.failed) {
        out->size = SX_HEADER_SIZE + image;
        out->data = kmalloc(out->size);
        if (!out->data) {
            fail(&a, "out of memory");
        } else {
            memcpy(out->data, SX_MAGIC, 4);
            out->data[4] = SX_VERSION;
            out->data[5] = 0;
            out->data[6] = out->data[7] = 0;
            put32(out->data + 8, entry);
            put32(out->data + 12, image);
            put32(out->data + 16, a.mem_size);
            put32(out->data + 20, 0);
            if (image)
                memcpy(out->data + SX_HEADER_SIZE, a.out, image);
            out->entry = entry;
        }
    }
    kfree(a.out);
    kfree(a.syms);
    if (a.failed) {
        kfree(out->data);
        out->data = NULL;
        out->size = 0;
        return -1;
    }
    return 0;
}
