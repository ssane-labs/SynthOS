#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

void strvec_push_owned(struct strvec *v, char *s)
{
    if (!s)
        return;
    if (v->count + 1 >= v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        char **items = krealloc(v->items, cap * sizeof(char *));
        if (!items) {
            kfree(s);
            return;
        }
        v->items = items;
        v->cap = cap;
    }
    v->items[v->count++] = s;
    v->items[v->count] = NULL;
}

void strvec_push(struct strvec *v, const char *s)
{
    strvec_push_owned(v, kstrdup(s));
}

void strvec_free(struct strvec *v)
{
    for (size_t i = 0; i < v->count; i++)
        kfree(v->items[i]);
    kfree(v->items);
    v->items = NULL;
    v->count = v->cap = 0;
}

void strvec_sort(struct strvec *v)
{
    for (size_t i = 1; i < v->count; i++) {
        char *s = v->items[i];
        size_t j = i;
        while (j > 0 && strcmp(v->items[j - 1], s) > 0) {
            v->items[j] = v->items[j - 1];
            j--;
        }
        v->items[j] = s;
    }
}

static void token_push(struct token_list *t, enum tok_type type, const char *line, size_t off, size_t len)
{
    if (t->count == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        struct token *items = krealloc(t->items, cap * sizeof(*items));
        if (!items)
            return;
        t->items = items;
        t->cap = cap;
    }
    t->items[t->count++] = (struct token){type, kstrndup(line + off, len), off, len};
}

void tokens_free(struct token_list *t)
{
    for (size_t i = 0; i < t->count; i++)
        kfree(t->items[i].text);
    kfree(t->items);
    t->items = NULL;
    t->count = t->cap = 0;
}

static size_t skip_subst(const char *s, size_t i)
{
    int depth = 0;
    char quote = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && s[i + 1])
                i++;
            else if (c == quote)
                quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '\\' && s[i + 1]) {
            i++;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (--depth == 0)
                return i + 1;
        }
    }
    return i;
}

static bool is_operator_start(const char *s, size_t i)
{
    char c = s[i];
    return c == '|' || c == '<' || c == '>' || c == ';' || (c == '&' && s[i + 1] == '&');
}

int lex(const char *line, struct token_list *out, bool keep_comments, char *err, size_t errcap)
{
    size_t i = 0;
    while (line[i]) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (c == '#') {
            if (keep_comments)
                token_push(out, TOK_COMMENT, line, i, strlen(line + i));
            break;
        }
        if (c == ';') {
            token_push(out, TOK_SEMI, line, i, 1);
            i++;
        } else if (c == '|' && line[i + 1] == '|') {
            token_push(out, TOK_OR, line, i, 2);
            i += 2;
        } else if (c == '|') {
            token_push(out, TOK_PIPE, line, i, 1);
            i++;
        } else if (c == '&' && line[i + 1] == '&') {
            token_push(out, TOK_AND, line, i, 2);
            i += 2;
        } else if (c == '>' && line[i + 1] == '>') {
            token_push(out, TOK_APPEND, line, i, 2);
            i += 2;
        } else if (c == '>') {
            token_push(out, TOK_OUT, line, i, 1);
            i++;
        } else if (c == '<') {
            token_push(out, TOK_IN, line, i, 1);
            i++;
        } else if (c == '2' && line[i + 1] == '>' && line[i + 2] == '&' && line[i + 3] == '1') {
            token_push(out, TOK_ERR_TO_OUT, line, i, 4);
            i += 4;
        } else if (c == '2' && line[i + 1] == '>' && line[i + 2] == '>') {
            token_push(out, TOK_ERR_APPEND, line, i, 3);
            i += 3;
        } else if (c == '2' && line[i + 1] == '>') {
            token_push(out, TOK_ERR, line, i, 2);
            i += 2;
        } else {
            size_t start = i;
            while (line[i] && !is_space((uint8_t)line[i]) && !is_operator_start(line, i)) {
                char q = line[i];
                if (q == '\'' || q == '"') {
                    size_t j = i + 1;
                    while (line[j] && line[j] != q) {
                        if (q == '"' && line[j] == '\\' && line[j + 1])
                            j++;
                        else if (q == '"' && line[j] == '$' && line[j + 1] == '(') {
                            j = skip_subst(line, j + 1);
                            continue;
                        }
                        j++;
                    }
                    if (!line[j]) {
                        ksnprintf(err, errcap, "unterminated %s quote", q == '"' ? "double" : "single");
                        token_push(out, TOK_WORD, line, start, j - start);
                        return -1;
                    }
                    i = j + 1;
                } else if (q == '\\' && line[i + 1]) {
                    i += 2;
                } else if (q == '$' && line[i + 1] == '(') {
                    size_t j = skip_subst(line, i + 1);
                    if (line[j - 1] != ')' || j == i + 1) {
                        ksnprintf(err, errcap, "unterminated $(");
                        token_push(out, TOK_WORD, line, start, j - start);
                        return -1;
                    }
                    i = j;
                } else {
                    i++;
                }
            }
            token_push(out, TOK_WORD, line, start, i - start);
        }
    }
    return 0;
}

struct expander {
    struct shell *sh;
    struct strvec *out;
    char *buf;
    size_t len;
    size_t cap;
    bool present;
    bool glob;
    bool noglob;
};

static void put(struct expander *e, const char *s, size_t n)
{
    if (e->len + n + 1 > e->cap) {
        size_t cap = e->cap ? e->cap : 64;
        while (cap < e->len + n + 1)
            cap *= 2;
        char *buf = krealloc(e->buf, cap);
        if (!buf)
            return;
        e->buf = buf;
        e->cap = cap;
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    e->buf[e->len] = 0;
    e->present = true;
}

static void glob_expand(struct expander *e, const char *pattern)
{
    const char *slash = strrchr(pattern, '/');
    char dir_prefix[PATH_MAX_LEN] = "";
    const char *base = pattern;
    if (slash) {
        size_t n = (size_t)(slash - pattern) + 1;
        if (n >= sizeof(dir_prefix)) {
            strvec_push(e->out, pattern);
            return;
        }
        memcpy(dir_prefix, pattern, n);
        dir_prefix[n] = 0;
        if (has_wildcard(dir_prefix)) {
            strvec_push(e->out, pattern);
            return;
        }
        base = slash + 1;
    }
    char dir[PATH_MAX_LEN];
    struct vdir d;
    if (sh_resolve(e->sh, dir_prefix[0] ? dir_prefix : ".", dir, sizeof(dir)) != 0 ||
        vfs_opendir(dir, &d) != 0) {
        strvec_push(e->out, pattern);
        return;
    }
    struct strvec found = {0};
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (ent.name[0] == '.' && base[0] != '.')
            continue;
        if (wildcard_match(base, ent.name, false)) {
            char full[PATH_MAX_LEN];
            ksnprintf(full, sizeof(full), "%s%s", dir_prefix, ent.name);
            strvec_push(&found, full);
        }
    }
    vfs_closedir(&d);
    if (found.count == 0) {
        strvec_push(e->out, pattern);
        return;
    }
    strvec_sort(&found);
    for (size_t i = 0; i < found.count; i++)
        strvec_push_owned(e->out, found.items[i]);
    kfree(found.items);
}

static void finish(struct expander *e)
{
    if (e->present) {
        const char *s = e->buf ? e->buf : "";
        if (e->glob && !e->noglob && has_wildcard(s))
            glob_expand(e, s);
        else
            strvec_push(e->out, s);
    }
    e->len = 0;
    if (e->buf)
        e->buf[0] = 0;
    e->present = false;
    e->glob = false;
}

static void put_split(struct expander *e, const char *s)
{
    for (; *s; s++) {
        if (is_space((uint8_t)*s)) {
            finish(e);
            continue;
        }
        put(e, s, 1);
    }
}

static size_t expand_dollar(struct expander *e, const char *raw, size_t i, bool quoted)
{
    char name[64];
    size_t n = 0;
    const char *value = NULL;
    char *owned = NULL;
    char c = raw[i + 1];

    if (c == '(') {
        size_t end = skip_subst(raw, i + 1);
        size_t inner_len = end - i - 3;
        if (end < i + 3)
            inner_len = 0;
        char *inner = kstrndup(raw + i + 2, inner_len);
        if (inner) {
            shell_capture(e->sh, inner, &owned);
            kfree(inner);
        }
        if (owned) {
            size_t l = strlen(owned);
            while (l && (owned[l - 1] == '\n' || owned[l - 1] == '\r'))
                owned[--l] = 0;
        }
        value = owned;
        i = end;
    } else if (c == '{') {
        size_t j = i + 2;
        while (raw[j] && raw[j] != '}' && n + 1 < sizeof(name))
            name[n++] = raw[j++];
        name[n] = 0;
        value = sh_get(e->sh, name);
        i = raw[j] == '}' ? j + 1 : j;
    } else if (c == '?' || c == '#' || c == '@' || c == '*' || is_digit((uint8_t)c)) {
        name[0] = c;
        name[1] = 0;
        value = sh_get(e->sh, name);
        i += 2;
    } else if (is_ident((uint8_t)c)) {
        size_t j = i + 1;
        while (is_ident((uint8_t)raw[j]) && n + 1 < sizeof(name))
            name[n++] = raw[j++];
        name[n] = 0;
        value = sh_get(e->sh, name);
        i = j;
    } else {
        put(e, "$", 1);
        return i + 1;
    }

    if (value) {
        if (quoted)
            put(e, value, strlen(value));
        else
            put_split(e, value);
    }
    if (quoted)
        e->present = true;
    kfree(owned);
    return i;
}

int expand_word(struct shell *sh, const char *raw, struct strvec *out)
{
    return expand_word_ex(sh, raw, out, true);
}

int expand_word_ex(struct shell *sh, const char *raw, struct strvec *out, bool glob)
{
    struct expander e = {.sh = sh, .out = out, .noglob = !glob};
    size_t i = 0;
    if (raw[0] == '~' && (raw[1] == 0 || raw[1] == '/')) {
        const char *home = sh_get(sh, "HOME");
        put(&e, home ? home : "/", strlen(home ? home : "/"));
        i = 1;
    }
    while (raw[i]) {
        char c = raw[i];
        if (c == '\'') {
            size_t j = i + 1;
            while (raw[j] && raw[j] != '\'')
                j++;
            put(&e, raw + i + 1, j - i - 1);
            e.present = true;
            i = raw[j] ? j + 1 : j;
        } else if (c == '"') {
            e.present = true;
            i++;
            while (raw[i] && raw[i] != '"') {
                if (raw[i] == '\\' && raw[i + 1] && strchr("\"\\$`", raw[i + 1])) {
                    put(&e, raw + i + 1, 1);
                    i += 2;
                } else if (raw[i] == '$') {
                    i = expand_dollar(&e, raw, i, true);
                } else {
                    put(&e, raw + i, 1);
                    i++;
                }
            }
            if (raw[i] == '"')
                i++;
        } else if (c == '\\' && raw[i + 1]) {
            put(&e, raw + i + 1, 1);
            i += 2;
        } else if (c == '$') {
            i = expand_dollar(&e, raw, i, false);
        } else {
            if (c == '*' || c == '?' || c == '[')
                e.glob = true;
            put(&e, raw + i, 1);
            i++;
        }
    }
    finish(&e);
    kfree(e.buf);
    return 0;
}

char *expand_single(struct shell *sh, const char *raw)
{
    struct strvec v = {0};
    expand_word(sh, raw, &v);
    char *result = NULL;
    if (v.count == 1) {
        result = v.items[0];
        v.items[0] = NULL;
        v.count = 0;
    } else if (v.count > 1) {
        size_t total = 0;
        for (size_t k = 0; k < v.count; k++)
            total += strlen(v.items[k]) + 1;
        result = kmalloc(total);
        if (result) {
            result[0] = 0;
            for (size_t k = 0; k < v.count; k++) {
                if (k)
                    str_append(result, " ", total);
                str_append(result, v.items[k], total);
            }
        }
    }
    strvec_free(&v);
    return result ? result : kstrdup("");
}

size_t split_statements(const char *text, struct strvec *out)
{
    size_t start_count = out->count;
    size_t cap = strlen(text) + 1;
    char *cur = kmalloc(cap);
    if (!cur)
        return 0;
    size_t len = 0;
    char quote = 0;
    int depth = 0;
    bool word_start = true;

    for (size_t i = 0;; i++) {
        char c = text[i];
        bool split = false;
        if (c == 0) {
            split = true;
        } else if (quote) {
            if (c == '\\' && quote == '"' && text[i + 1]) {
                cur[len++] = c;
                c = text[++i];
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '\\' && text[i + 1] == '\n') {
            i++;
            continue;
        } else if (c == '\\' && text[i + 1]) {
            cur[len++] = c;
            c = text[++i];
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '$' && text[i + 1] == '(') {
            depth++;
            cur[len++] = c;
            c = text[++i];
        } else if (c == '(' && depth) {
            depth++;
        } else if (c == ')' && depth) {
            depth--;
        } else if (c == '#' && word_start && depth == 0) {
            while (text[i + 1] && text[i + 1] != '\n')
                i++;
            continue;
        } else if ((c == '\n' || c == ';') && depth == 0) {
            split = true;
        }

        if (split) {
            cur[len] = 0;
            char *s = str_trim(cur);
            if (*s)
                strvec_push(out, s);
            len = 0;
            word_start = true;
            if (c == 0)
                break;
            continue;
        }
        word_start = c == ' ' || c == '\t' || c == '\r';
        cur[len++] = c;
    }
    kfree(cur);
    return out->count - start_count;
}

enum keyword statement_keyword(const char *stmt, const char **rest)
{
    static const struct {
        const char *word;
        enum keyword kw;
    } words[] = {
        {"if", KW_IF},         {"elif", KW_ELIF},     {"else", KW_ELSE},
        {"end", KW_END},       {"fi", KW_END},        {"done", KW_END},
        {"while", KW_WHILE},   {"for", KW_FOR},       {"func", KW_FUNC},
        {"function", KW_FUNC}, {"break", KW_BREAK},   {"continue", KW_CONTINUE},
        {"return", KW_RETURN},
    };
    size_t n = 0;
    while (stmt[n] && !is_space((uint8_t)stmt[n]))
        n++;
    for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
        if (strlen(words[i].word) == n && memcmp(words[i].word, stmt, n) == 0) {
            if (rest) {
                const char *r = stmt + n;
                while (is_space((uint8_t)*r))
                    r++;
                *rest = r;
            }
            return words[i].kw;
        }
    }
    if (rest)
        *rest = stmt;
    return KW_NONE;
}

int block_balance(const char *text)
{
    struct strvec stmts = {0};
    split_statements(text, &stmts);
    int depth = 0;
    for (size_t i = 0; i < stmts.count; i++) {
        enum keyword kw = statement_keyword(stmts.items[i], NULL);
        if (kw == KW_IF || kw == KW_WHILE || kw == KW_FOR || kw == KW_FUNC)
            depth++;
        else if (kw == KW_END)
            depth--;
    }
    strvec_free(&stmts);
    return depth;
}
