#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "internal.h"

struct script {
    char **stmts;
    size_t count;
};

static int exec_range(struct shell *sh, const struct script *s, size_t from, size_t to);

static bool opens_block(enum keyword kw)
{
    return kw == KW_IF || kw == KW_WHILE || kw == KW_FOR || kw == KW_FUNC;
}

static size_t find_end(const struct script *s, size_t from, size_t to, size_t *branches, size_t *nbranches)
{
    int depth = 0;
    size_t nb = 0;
    for (size_t i = from + 1; i < to; i++) {
        enum keyword kw = statement_keyword(s->stmts[i], NULL);
        if (opens_block(kw)) {
            depth++;
        } else if (kw == KW_END) {
            if (depth-- == 0) {
                if (nbranches)
                    *nbranches = nb;
                return i;
            }
        } else if ((kw == KW_ELIF || kw == KW_ELSE) && depth == 0 && branches && nb < 16) {
            branches[nb++] = i;
        }
    }
    return to;
}

static int exec_if(struct shell *sh, const struct script *s, size_t i, size_t end, size_t *branches,
                   size_t nb)
{
    size_t cond = i;
    for (size_t b = 0; b <= nb; b++) {
        size_t body_end = b < nb ? branches[b] : end;
        const char *rest;
        enum keyword kw = statement_keyword(s->stmts[cond], &rest);
        bool take = kw == KW_ELSE;
        if (!take) {
            if (!*rest)
                return sh_fail(sh, 2, "if", "missing condition");
            take = run_statement(sh, rest) == 0;
            if (sh->ctl != CTL_NONE)
                return sh->status;
        }
        if (take)
            return exec_range(sh, s, cond + 1, body_end);
        cond = body_end;
    }
    sh->status = 0;
    return 0;
}

static bool loop_control(struct shell *sh)
{
    if (sh->ctl == CTL_BREAK) {
        sh->ctl = CTL_NONE;
        return true;
    }
    if (sh->ctl == CTL_CONTINUE)
        sh->ctl = CTL_NONE;
    return sh->ctl != CTL_NONE || sh_interrupted();
}

static int exec_while(struct shell *sh, const struct script *s, size_t i, size_t end)
{
    const char *cond;
    statement_keyword(s->stmts[i], &cond);
    int rc = 0;
    sh->loop_depth++;
    while (run_statement(sh, cond) == 0 && sh->ctl == CTL_NONE) {
        rc = exec_range(sh, s, i + 1, end);
        if (loop_control(sh))
            break;
    }
    sh->loop_depth--;
    return rc;
}

static int exec_for(struct shell *sh, const struct script *s, size_t i, size_t end)
{
    const char *rest;
    statement_keyword(s->stmts[i], &rest);
    struct token_list toks = {0};
    char err[64];
    if (lex(rest, &toks, false, err, sizeof(err)) != 0 || toks.count < 2 ||
        strcmp(toks.items[1].text, "in") != 0) {
        tokens_free(&toks);
        return sh_fail(sh, 2, "for", "usage: for NAME in WORDS...");
    }
    struct strvec values = {0};
    for (size_t k = 2; k < toks.count; k++)
        expand_word(sh, toks.items[k].text, &values);
    char *var = kstrdup(toks.items[0].text);
    tokens_free(&toks);

    int rc = 0;
    sh->loop_depth++;
    for (size_t k = 0; k < values.count; k++) {
        sh_set(sh, var, values.items[k]);
        rc = exec_range(sh, s, i + 1, end);
        if (loop_control(sh))
            break;
    }
    sh->loop_depth--;
    strvec_free(&values);
    kfree(var);
    return rc;
}

static int define_func(struct shell *sh, const struct script *s, size_t i, size_t end)
{
    const char *name;
    statement_keyword(s->stmts[i], &name);
    if (!*name)
        return sh_fail(sh, 2, "func", "missing function name");
    size_t len = 1;
    for (size_t k = i + 1; k < end; k++)
        len += strlen(s->stmts[k]) + 1;
    char *body = kmalloc(len);
    if (!body)
        return 1;
    body[0] = 0;
    for (size_t k = i + 1; k < end; k++) {
        str_append(body, s->stmts[k], len);
        str_append(body, "\n", len);
    }
    char fname[64];
    size_t n = 0;
    while (name[n] && !is_space((uint8_t)name[n]) && name[n] != '(' && n + 1 < sizeof(fname))
        fname[n] = name[n], n++;
    fname[n] = 0;
    sh_function_define(sh, fname, body);
    kfree(body);
    return 0;
}

static int exec_range(struct shell *sh, const struct script *s, size_t from, size_t to)
{
    int rc = sh->status;
    size_t i = from;
    while (i < to && sh->ctl == CTL_NONE) {
        const char *rest;
        enum keyword kw = statement_keyword(s->stmts[i], &rest);
        size_t branches[16], nb = 0, end;
        long n;
        switch (kw) {
        case KW_IF:
            end = find_end(s, i, to, branches, &nb);
            if (end >= to)
                return sh_fail(sh, 2, "if", "missing 'end'");
            rc = exec_if(sh, s, i, end, branches, nb);
            i = end + 1;
            break;
        case KW_WHILE:
        case KW_FOR:
        case KW_FUNC:
            end = find_end(s, i, to, NULL, NULL);
            if (end >= to)
                return sh_fail(sh, 2, s->stmts[i], "missing 'end'");
            rc = kw == KW_WHILE ? exec_while(sh, s, i, end)
                 : kw == KW_FOR ? exec_for(sh, s, i, end)
                                : define_func(sh, s, i, end);
            i = end + 1;
            break;
        case KW_BREAK:
        case KW_CONTINUE:
            if (sh->loop_depth == 0)
                return sh_fail(sh, 1, kw == KW_BREAK ? "break" : "continue", "not inside a loop");
            sh->ctl = kw == KW_BREAK ? CTL_BREAK : CTL_CONTINUE;
            return rc;
        case KW_RETURN:
            if (*rest) {
                char *v = expand_single(sh, rest);
                rc = str_parse_long(v, &n) ? (int)n : 1;
                kfree(v);
            }
            sh->status = rc;
            sh->ctl = CTL_RETURN;
            return rc;
        case KW_ELIF:
        case KW_ELSE:
        case KW_END:
            return sh_fail(sh, 2, "syntax", "unexpected '%s'", s->stmts[i]);
        default:
            rc = run_statement(sh, s->stmts[i]);
            i++;
            break;
        }
        sh->status = rc;
    }
    return rc;
}

int script_run_text(struct shell *sh, const char *text)
{
    struct strvec stmts = {0};
    split_statements(text, &stmts);
    struct script s = {stmts.items, stmts.count};
    int rc = exec_range(sh, &s, 0, s.count);
    strvec_free(&stmts);
    return rc;
}
