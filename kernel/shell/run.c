#include <synth/command.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

#define MAX_REDIRS 8
#define NOGLOB_COMMANDS "calc,expr,let"

struct redir {
    enum tok_type type;
    const char *target;
};

static int alias_depth;
static const char *alias_stack[8];

bool sh_interrupted(void)
{
    static uint64_t last;
    uint64_t now = hal_ticks_ms();
    if (now - last >= 25) {
        last = now;
        input_poll();
    }
    return input_break_pending();
}

bool sh_is_command(struct shell *sh, const char *name)
{
    char path[PATH_MAX_LEN];
    return strmap_get(&sh->aliases, name) || sh_function_find(sh, name) || command_find(name) ||
           exec_find(sh, name, path, sizeof(path)) == 0;
}

static bool is_assignment(const char *s)
{
    if (!is_alpha((uint8_t)*s) && *s != '_')
        return false;
    while (is_ident((uint8_t)*s))
        s++;
    return *s == '=';
}

static void quote_append(char *dst, size_t cap, const char *arg)
{
    str_append(dst, " '", cap);
    for (const char *p = arg; *p; p++) {
        if (*p == '\'')
            str_append(dst, "'\"'\"'", cap);
        else {
            char ch[2] = {*p, 0};
            str_append(dst, ch, cap);
        }
    }
    str_append(dst, "'", cap);
}

static int run_alias(struct shell *sh, const char *value, int argc, char **argv)
{
    if (alias_depth >= 8)
        return sh_error(sh, argv[0], "alias loop detected");
    size_t cap = strlen(value) + 8;
    for (int i = 1; i < argc; i++)
        cap += strlen(argv[i]) * 5 + 4;
    char *line = kmalloc(cap);
    if (!line)
        return 1;
    str_copy(line, value, cap);
    for (int i = 1; i < argc; i++)
        quote_append(line, cap, argv[i]);
    alias_stack[alias_depth++] = argv[0];
    int rc = script_run_text(sh, line);
    alias_depth--;
    kfree(line);
    return rc;
}

static int run_external(struct shell *sh, int argc, char **argv)
{
    char path[PATH_MAX_LEN];
    int err = exec_find(sh, argv[0], path, sizeof(path));
    if (err) {
        if (strchr(argv[0], '/')) {
            sh_fs_error(sh, NULL, argv[0], err);
            return 127;
        }
        sh_role(sh, ROLE_ERROR);
        stream_printf(sh->err, "%s: command not found", argv[0]);
        sh_reset(sh);
        stream_puts(sh->err, "\n");
        return 127;
    }
    struct exec_ctx ctx = {
        .sh = sh,
        .path = path,
        .argc = argc,
        .argv = argv,
        .in = sh->in,
        .out = sh->out,
        .err = sh->err,
    };
    int rc = exec_program(&ctx);
    if (rc < 0) {
        sh_fs_error(sh, NULL, argv[0], rc);
        return 126;
    }
    return rc;
}

int run_argv(struct shell *sh, int argc, char **argv)
{
    if (argc == 0)
        return 0;
    bool all_assign = true;
    for (int i = 0; i < argc && all_assign; i++)
        all_assign = is_assignment(argv[i]);
    if (all_assign) {
        for (int i = 0; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            *eq = 0;
            sh_set(sh, argv[i], eq + 1);
            *eq = '=';
        }
        return 0;
    }

    const char *alias = strmap_get(&sh->aliases, argv[0]);
    for (int i = 0; alias && i < alias_depth; i++)
        if (strcmp(alias_stack[i], argv[0]) == 0)
            alias = NULL;
    if (alias)
        return run_alias(sh, alias, argc, argv);
    struct sh_function *fn = sh_function_find(sh, argv[0]);
    if (fn)
        return sh_function_call(sh, fn, argc, argv);
    const struct command *cmd = command_find(argv[0]);
    if (cmd)
        return cmd->run(sh, argc, argv);
    return run_external(sh, argc, argv);
}

static int run_simple(struct shell *sh, struct strvec *args, struct redir *redirs, int nredirs,
                      struct stream *in, struct stream *out)
{
    struct stream *saved_in = sh->in, *saved_out = sh->out, *saved_err = sh->err;
    struct stream *opened[MAX_REDIRS];
    int nopened = 0, rc = 0;
    sh->in = in;
    sh->out = out;

    for (int i = 0; i < nredirs; i++) {
        if (redirs[i].type == TOK_ERR_TO_OUT) {
            sh->err = sh->out;
            continue;
        }
        char *target = expand_single(sh, redirs[i].target);
        char path[PATH_MAX_LEN];
        int err = sh_resolve(sh, target, path, sizeof(path));
        unsigned flags = redirs[i].type == TOK_IN ? VFS_READ
                         : redirs[i].type == TOK_APPEND || redirs[i].type == TOK_ERR_APPEND
                             ? VFS_WRITE | VFS_CREATE | VFS_APPEND
                             : VFS_WRITE | VFS_CREATE | VFS_TRUNC;
        struct stream *s = err ? NULL : stream_file(path, flags, &err);
        if (!s) {
            sh->err = saved_err;
            sh_fs_error(sh, NULL, target, err);
            kfree(target);
            rc = 1;
            goto done;
        }
        kfree(target);
        opened[nopened++] = s;
        if (redirs[i].type == TOK_IN)
            sh->in = s;
        else if (redirs[i].type == TOK_ERR || redirs[i].type == TOK_ERR_APPEND)
            sh->err = s;
        else
            sh->out = s;
    }

    rc = run_argv(sh, (int)args->count, args->items);

done:
    for (int i = 0; i < nopened; i++)
        stream_close(opened[i]);
    sh->in = saved_in;
    sh->out = saved_out;
    sh->err = saved_err;
    return rc;
}

static int run_pipeline(struct shell *sh, struct token *toks, size_t count)
{
    struct stream *prev = NULL;
    int rc = 0;
    bool negate = false;
    if (count && toks[0].type == TOK_WORD && strcmp(toks[0].text, "!") == 0) {
        negate = true;
        toks++;
        count--;
    }
    size_t i = 0;
    while (i <= count) {
        size_t start = i;
        while (i < count && toks[i].type != TOK_PIPE)
            i++;
        bool last = i >= count;

        struct strvec args = {0};
        struct redir redirs[MAX_REDIRS];
        int nredirs = 0;
        bool glob = true;
        for (size_t k = start; k < i; k++) {
            if (toks[k].type == TOK_WORD) {
                expand_word_ex(sh, toks[k].text, &args, glob);
                if (args.count == 1 && k == start)
                    glob = !str_in_list(args.items[0], NOGLOB_COMMANDS);
            } else if (toks[k].type == TOK_ERR_TO_OUT) {
                if (nredirs < MAX_REDIRS)
                    redirs[nredirs++] = (struct redir){toks[k].type, NULL};
            } else if (k + 1 < i && toks[k + 1].type == TOK_WORD) {
                if (nredirs < MAX_REDIRS)
                    redirs[nredirs++] = (struct redir){toks[k].type, toks[k + 1].text};
                k++;
            } else {
                sh_error(sh, "syntax", "missing redirect target after '%s'", toks[k].text);
                strvec_free(&args);
                stream_close(prev);
                return 2;
            }
        }

        struct stream *out = last ? sh->out : stream_buffer();
        if (!out) {
            strvec_free(&args);
            stream_close(prev);
            return 1;
        }
        rc = run_simple(sh, &args, redirs, nredirs, prev ? prev : sh->in, out);
        strvec_free(&args);
        stream_close(prev);
        prev = NULL;
        if (!last) {
            stream_buffer_rewind(out);
            prev = out;
        }
        if (sh->ctl != CTL_NONE)
            break;
        i++;
    }
    stream_close(prev);
    return negate ? !rc : rc;
}

int run_statement(struct shell *sh, const char *stmt)
{
    struct token_list toks = {0};
    char err[64];
    if (lex(stmt, &toks, false, err, sizeof(err)) != 0) {
        tokens_free(&toks);
        sh_error(sh, "syntax", "%s", err);
        return sh->status = 2;
    }
    int rc = 0;
    size_t i = 0;
    enum tok_type op = TOK_AND;
    bool first = true;
    while (i < toks.count) {
        size_t start = i;
        while (i < toks.count && toks.items[i].type != TOK_AND && toks.items[i].type != TOK_OR)
            i++;
        bool skip = !first && ((op == TOK_AND && rc != 0) || (op == TOK_OR && rc == 0));
        if (start == i) {
            sh_error(sh, "syntax", "unexpected '%s'", i < toks.count ? toks.items[i].text : "end");
            rc = 2;
            break;
        }
        if (!skip) {
            rc = run_pipeline(sh, toks.items + start, i - start);
            sh->status = rc;
        }
        if (sh->ctl != CTL_NONE)
            break;
        if (i < toks.count)
            op = toks.items[i++].type;
        first = false;
    }
    tokens_free(&toks);
    sh->status = rc;
    return rc;
}
