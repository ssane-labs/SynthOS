#include <synth/command.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/lineedit.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

static void color_word(const char *buf, size_t off, size_t len, uint8_t *attrs, uint8_t base)
{
    uint8_t str = theme_attr(ROLE_SYNTAX_STRING), var = theme_attr(ROLE_SYNTAX_VARIABLE);
    char quote = 0;
    for (size_t i = off; i < off + len; i++) {
        char c = buf[i];
        if (quote) {
            attrs[i] = str;
            if (c == quote)
                quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < off + len)
                attrs[++i] = str;
            else if (c == '$' && quote == '"') {
                attrs[i] = var;
                while (i + 1 < off + len && (is_ident((uint8_t)buf[i + 1]) || buf[i + 1] == '{' || buf[i + 1] == '}'))
                    attrs[++i] = var;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            attrs[i] = str;
        } else if (c == '$') {
            attrs[i] = var;
            if (i + 1 < off + len && buf[i + 1] == '(') {
                int depth = 0;
                for (; i < off + len; i++) {
                    attrs[i] = var;
                    if (buf[i] == '(')
                        depth++;
                    else if (buf[i] == ')' && --depth == 0)
                        break;
                }
                continue;
            }
            while (i + 1 < off + len && (is_ident((uint8_t)buf[i + 1]) || strchr("{}?#@*", buf[i + 1])))
                attrs[++i] = var;
        } else {
            attrs[i] = base;
        }
    }
}

static void highlight(void *ctx, const char *buf, size_t len, uint8_t *attrs)
{
    struct shell *sh = ctx;
    struct token_list toks = {0};
    char err[48];
    UNUSED(len);
    lex(buf, &toks, true, err, sizeof(err));
    bool expect_cmd = true;
    for (size_t i = 0; i < toks.count; i++) {
        struct token *t = &toks.items[i];
        if (t->type == TOK_COMMENT) {
            memset(attrs + t->offset, theme_attr(ROLE_MUTED), t->length);
            continue;
        }
        if (t->type != TOK_WORD) {
            memset(attrs + t->offset, theme_attr(ROLE_SYNTAX_OPERATOR), t->length);
            expect_cmd = t->type == TOK_PIPE || t->type == TOK_AND || t->type == TOK_OR || t->type == TOK_SEMI;
            if (!expect_cmd && i + 1 < toks.count && toks.items[i + 1].type == TOK_WORD) {
                i++;
                color_word(buf, toks.items[i].offset, toks.items[i].length, attrs, theme_attr(ROLE_TEXT));
            }
            continue;
        }
        if (!expect_cmd) {
            color_word(buf, t->offset, t->length, attrs, theme_attr(ROLE_TEXT));
            continue;
        }
        enum keyword kw = statement_keyword(t->text, NULL);
        uint8_t base;
        if (kw != KW_NONE) {
            base = theme_attr(ROLE_SYNTAX_OPERATOR);
            expect_cmd = kw == KW_IF || kw == KW_ELIF || kw == KW_WHILE || kw == KW_ELSE;
        } else if (strchr(t->text, '=') && is_alpha((uint8_t)t->text[0])) {
            base = theme_attr(ROLE_SYNTAX_VARIABLE);
        } else {
            char *name = expand_single(sh, t->text);
            bool known = strcmp(t->text, "!") == 0 || sh_is_command(sh, name);
            kfree(name);
            base = theme_attr(known ? ROLE_SYNTAX_COMMAND : ROLE_SYNTAX_ERROR);
            expect_cmd = strcmp(t->text, "!") == 0;
        }
        color_word(buf, t->offset, t->length, attrs, base);
    }
    tokens_free(&toks);
}

struct cmd_collect {
    struct completion *c;
    const char *prefix;
};

static void add_command(const struct command *cmd, void *ctx)
{
    struct cmd_collect *cc = ctx;
    if (str_starts(cmd->name, cc->prefix))
        completion_add(cc->c, cmd->name);
}

static void complete_commands(struct shell *sh, const char *prefix, struct completion *c)
{
    struct cmd_collect cc = {c, prefix};
    command_foreach(add_command, &cc);
    for (size_t i = 0; i < sh->aliases.count; i++)
        if (str_starts(sh->aliases.items[i].key, prefix))
            completion_add(c, sh->aliases.items[i].key);
    for (size_t i = 0; i < sh->func_count; i++)
        if (str_starts(sh->funcs[i].name, prefix))
            completion_add(c, sh->funcs[i].name);

    const char *path = sh_get(sh, "PATH");
    char dir[PATH_MAX_LEN];
    while (path && *path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        if (n < sizeof(dir)) {
            memcpy(dir, path, n);
            dir[n] = 0;
            struct vdir d;
            if (vfs_opendir(dir, &d) == 0) {
                struct vfs_dirent ent;
                while (vfs_readdir(&d, &ent) == 0) {
                    if (ent.type == VN_DIR || !exec_is_executable_name(ent.name) || !str_starts(ent.name, prefix))
                        continue;
                    char *dot = strrchr(ent.name, '.');
                    if (dot)
                        *dot = 0;
                    completion_add(c, ent.name);
                }
                vfs_closedir(&d);
            }
        }
        path = sep ? sep + 1 : NULL;
    }
}

static void complete_paths(struct shell *sh, const char *word, bool dirs_only, struct completion *c)
{
    const char *slash = strrchr(word, '/');
    char dir_part[PATH_MAX_LEN] = "";
    const char *prefix = word;
    if (slash) {
        size_t n = MIN((size_t)(slash - word) + 1, sizeof(dir_part) - 1);
        memcpy(dir_part, word, n);
        dir_part[n] = 0;
        prefix = slash + 1;
    }
    char dir[PATH_MAX_LEN];
    char *expanded = expand_single(sh, dir_part[0] ? dir_part : ".");
    int err = sh_resolve(sh, expanded, dir, sizeof(dir));
    kfree(expanded);
    struct vdir d;
    if (err || vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (!str_starts(ent.name, prefix) || (ent.name[0] == '.' && prefix[0] != '.'))
            continue;
        if (dirs_only && ent.type != VN_DIR)
            continue;
        char item[PATH_MAX_LEN];
        ksnprintf(item, sizeof(item), "%s%s%s", dir_part, ent.name, ent.type == VN_DIR ? "/" : "");
        completion_add(c, item);
    }
    vfs_closedir(&d);
}

static void complete_themes(const char *prefix, struct completion *c)
{
    for (size_t i = 0; i < theme_builtin_count(); i++)
        if (str_starts(theme_builtin(i)->name, prefix))
            completion_add(c, theme_builtin(i)->name);
    struct vdir d;
    if (vfs_opendir("/etc/themes", &d) == 0) {
        struct vfs_dirent ent;
        while (vfs_readdir(&d, &ent) == 0) {
            if (!str_ends(ent.name, ".theme") || !str_starts(ent.name, prefix))
                continue;
            ent.name[strlen(ent.name) - 6] = 0;
            completion_add(c, ent.name);
        }
        vfs_closedir(&d);
    }
}

SYNTH_TABLE_DECLARE(synth_settings, struct setting);

static void prompt_collect(const char *name, const char *format, void *ctx)
{
    UNUSED(format);
    struct cmd_collect *cc = ctx;
    if (str_starts(name, cc->prefix))
        completion_add(cc->c, name);
}

static void complete(void *ctx, const char *buf, size_t cursor, struct completion *out)
{
    struct shell *sh = ctx;
    size_t start = cursor;
    while (start > 0 && !strchr(" \t|;&<>", buf[start - 1]))
        start--;
    out->start = start;
    char word[PATH_MAX_LEN];
    size_t wl = MIN(cursor - start, sizeof(word) - 1);
    memcpy(word, buf + start, wl);
    word[wl] = 0;

    size_t p = start;
    while (p > 0 && (buf[p - 1] == ' ' || buf[p - 1] == '\t'))
        p--;
    bool command_pos = p == 0 || strchr("|;&", buf[p - 1]);

    char first[32] = "";
    size_t cmd_start = p;
    while (cmd_start > 0 && !strchr("|;&", buf[cmd_start - 1]))
        cmd_start--;
    while (buf[cmd_start] == ' ')
        cmd_start++;
    size_t fl = 0;
    while (buf[cmd_start + fl] && buf[cmd_start + fl] != ' ' && fl + 1 < sizeof(first))
        first[fl] = buf[cmd_start + fl], fl++;
    first[fl] = 0;

    if (command_pos && !strchr(word, '/')) {
        complete_commands(sh, word, out);
        if (out->count == 0)
            complete_paths(sh, word, false, out);
    } else if (strcmp(first, "theme") == 0 && !strchr(word, '/')) {
        complete_themes(word, out);
        static const char *const subs[] = {"list", "show", "save", "edit", "reset"};
        for (size_t i = 0; i < ARRAY_SIZE(subs); i++)
            if (str_starts(subs[i], word))
                completion_add(out, subs[i]);
    } else if ((strcmp(first, "config") == 0 || strcmp(first, "set") == 0) && !strchr(word, '/')) {
        SYNTH_TABLE_FOREACH(synth_settings, struct setting, st)
            if (str_starts(st->name, word))
                completion_add(out, st->name);
    } else if (strcmp(first, "prompt") == 0) {
        struct cmd_collect cc = {out, word};
        prompt_presets(prompt_collect, &cc);
    } else if (strcmp(first, "help") == 0 || strcmp(first, "which") == 0) {
        complete_commands(sh, word, out);
    } else {
        complete_paths(sh, word, strcmp(first, "cd") == 0 || strcmp(first, "rmdir") == 0, out);
    }
}

void interactive_hooks(struct shell *sh, struct lineedit_hooks *hooks)
{
    const char *hl = sh_get(sh, "HIGHLIGHT");
    const char *sg = sh_get(sh, "SUGGEST");
    *hooks = (struct lineedit_hooks){
        .ctx = sh,
        .highlight = hl && strcmp(hl, "off") == 0 ? NULL : highlight,
        .complete = complete,
        .history = &sh->history,
        .suggest = !(sg && strcmp(sg, "off") == 0),
    };
}
