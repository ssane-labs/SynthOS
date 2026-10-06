#include <synth/command.h>
#include <synth/config.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/lineedit.h>
#include <synth/string.h>
#include <synth/vfs.h>

static bool valid_name(const char *s)
{
    if (!is_alpha((uint8_t)*s) && *s != '_')
        return false;
    for (; *s; s++)
        if (!is_ident((uint8_t)*s))
            return false;
    return true;
}

static void print_map(struct shell *sh, struct strmap *m, const char *prefix)
{
    strmap_sort(m);
    for (size_t i = 0; i < m->count; i++) {
        sh_puts(sh, prefix);
        sh_role(sh, ROLE_SYNTAX_VARIABLE);
        sh_puts(sh, m->items[i].key);
        sh_reset(sh);
        sh_printf(sh, "=%s\n", m->items[i].value);
    }
}

static int assign(struct shell *sh, const char *who, int argc, char **argv, struct strmap *target)
{
    if (argc == 1) {
        print_map(sh, target, "");
        return 0;
    }
    char *eq = strchr(argv[1], '=');
    char name[64];
    const char *value;
    if (eq) {
        size_t n = MIN((size_t)(eq - argv[1]), sizeof(name) - 1);
        memcpy(name, argv[1], n);
        name[n] = 0;
        value = eq + 1;
    } else {
        str_copy(name, argv[1], sizeof(name));
        value = argc > 2 ? NULL : "";
    }
    if (target == &sh->vars && !valid_name(name))
        return sh_error(sh, who, "invalid name '%s'", name);
    if (!value) {
        size_t cap = 1;
        for (int i = 2; i < argc; i++)
            cap += strlen(argv[i]) + 1;
        char *joined = kmalloc(cap);
        if (!joined)
            return 1;
        joined[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(joined, " ", cap);
            str_append(joined, argv[i], cap);
        }
        strmap_set(target, name, joined);
        kfree(joined);
    } else {
        strmap_set(target, name, value);
    }
    return 0;
}

COMMAND(set, "export", "shell", "set [NAME[=value] | NAME value...]", "show or set variables")
{
    return assign(sh, "set", argc, argv, &sh->vars);
}

COMMAND(env, NULL, "shell", "env", "list all variables")
{
    UNUSED(argc);
    UNUSED(argv);
    print_map(sh, &sh->vars, "");
    return 0;
}

COMMAND(unset, NULL, "shell", "unset NAME...", "remove variables")
{
    for (int i = 1; i < argc; i++)
        sh_unset(sh, argv[i]);
    return 0;
}

COMMAND(alias, NULL, "shell", "alias [name=command]", "define or list command aliases")
{
    if (argc == 1) {
        strmap_sort(&sh->aliases);
        for (size_t i = 0; i < sh->aliases.count; i++) {
            sh_role(sh, ROLE_SYNTAX_COMMAND);
            sh_puts(sh, sh->aliases.items[i].key);
            sh_reset(sh);
            sh_printf(sh, " = '%s'\n", sh->aliases.items[i].value);
        }
        return 0;
    }
    if (argc == 2 && !strchr(argv[1], '=')) {
        const char *v = strmap_get(&sh->aliases, argv[1]);
        if (!v)
            return sh_error(sh, "alias", "%s: not found", argv[1]);
        sh_printf(sh, "%s = '%s'\n", argv[1], v);
        return 0;
    }
    return assign(sh, "alias", argc, argv, &sh->aliases);
}

COMMAND(unalias, NULL, "shell", "unalias name...", "remove aliases")
{
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (!strmap_remove(&sh->aliases, argv[i]))
            rc = sh_error(sh, "unalias", "%s: not found", argv[i]);
    return rc;
}

COMMAND(source, ".", "shell", "source file [args...]", "run a script in the current shell")
{
    if (argc < 2)
        return command_usage(sh, command_find("source"));
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[1], path, sizeof(path));
    if (err)
        return sh_fs_error(sh, "source", argv[1], err);
    char *text;
    err = vfs_read_all(path, &text, NULL);
    if (err)
        return sh_fs_error(sh, "source", argv[1], err);
    int rc = shell_exec_script(sh, text, argc - 1, argv + 1);
    kfree(text);
    return rc;
}

COMMAND(run, "start", "shell", "run program [args...]", "run a program file (.sx, .com, .exe, .sh)")
{
    if (argc < 2)
        return command_usage(sh, command_find("run"));
    char path[PATH_MAX_LEN];
    int err = exec_find(sh, argv[1], path, sizeof(path));
    if (err) {
        err = sh_resolve(sh, argv[1], path, sizeof(path));
        if (!err && !vfs_exists(path))
            err = E_NOENT;
    }
    if (err)
        return sh_fs_error(sh, "run", argv[1], err);
    struct exec_ctx ctx = {sh, path, argc - 1, argv + 1, sh->in, sh->out, sh->err};
    int rc = exec_program(&ctx);
    if (rc < 0)
        return sh_fs_error(sh, "run", argv[1], rc);
    return rc;
}

COMMAND(true, NULL, "shell", "true", "do nothing, successfully")
{
    UNUSED(sh);
    return 0;
}

COMMAND(false, NULL, "shell", "false", "do nothing, unsuccessfully")
{
    UNUSED(sh);
    return 1;
}

static bool num(const char *s, long *out)
{
    return str_parse_long(s, out);
}

static int test_unary(struct shell *sh, const char *op, const char *arg)
{
    char path[PATH_MAX_LEN];
    struct vstat st;
    bool exists = sh_resolve(sh, arg, path, sizeof(path)) == 0 && vfs_stat(path, &st) == 0;
    switch (op[1]) {
    case 'e': return exists;
    case 'f': return exists && st.type == VN_FILE;
    case 'd': return exists && st.type == VN_DIR;
    case 's': return exists && st.size > 0;
    case 'x': return exists && exec_is_executable_name(arg);
    case 'z': return arg[0] == 0;
    case 'n': return arg[0] != 0;
    default: return -1;
    }
}

static int test_binary(const char *a, const char *op, const char *b)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return strcmp(a, b) == 0;
    if (strcmp(op, "!=") == 0)
        return strcmp(a, b) != 0;
    long x, y;
    if (!num(a, &x) || !num(b, &y))
        return -1;
    if (strcmp(op, "-eq") == 0) return x == y;
    if (strcmp(op, "-ne") == 0) return x != y;
    if (strcmp(op, "-lt") == 0) return x < y;
    if (strcmp(op, "-le") == 0) return x <= y;
    if (strcmp(op, "-gt") == 0) return x > y;
    if (strcmp(op, "-ge") == 0) return x >= y;
    return -1;
}

static int test_expr(struct shell *sh, int argc, char **argv)
{
    bool negate = false;
    while (argc > 0 && strcmp(argv[0], "!") == 0) {
        negate = !negate;
        argv++;
        argc--;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "-a") == 0) {
            int left = test_expr(sh, i, argv);
            if (left < 0)
                return -1;
            if (argv[i][1] == 'o' && left)
                return negate ? 0 : 1;
            if (argv[i][1] == 'a' && !left)
                return negate ? 1 : 0;
            int right = test_expr(sh, argc - i - 1, argv + i + 1);
            return right < 0 ? -1 : negate ? !right : right;
        }
    }
    int r;
    if (argc == 0)
        r = 0;
    else if (argc == 1)
        r = argv[0][0] != 0;
    else if (argc == 2 && argv[0][0] == '-')
        r = test_unary(sh, argv[0], argv[1]);
    else if (argc == 3)
        r = test_binary(argv[0], argv[1], argv[2]);
    else
        r = -1;
    return r < 0 ? r : negate ? !r : r;
}

COMMAND(test, "[", "shell", "test EXPR | [ EXPR ]", "evaluate a condition (-f -d -e -z -n = != -eq -lt ...)")
{
    if (strcmp(argv[0], "[") == 0) {
        if (strcmp(argv[argc - 1], "]") != 0)
            return sh_fail(sh, 2, "[", "missing ']'");
        argc--;
    }
    int r = test_expr(sh, argc - 1, argv + 1);
    if (r < 0)
        return sh_fail(sh, 2, "test", "bad expression");
    return r ? 0 : 1;
}

static char *join_args(int argc, char **argv)
{
    size_t cap = 1;
    for (int i = 0; i < argc; i++)
        cap += strlen(argv[i]) + 1;
    char *s = kmalloc(cap);
    if (!s)
        return NULL;
    s[0] = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            str_append(s, " ", cap);
        str_append(s, argv[i], cap);
    }
    return s;
}

COMMAND(calc, "expr", "shell", "calc expression", "integer calculator (+ - * / % << >> & | ^ ~ ! == < ...)")
{
    if (argc < 2)
        return command_usage(sh, command_find("calc"));
    char *expr = join_args(argc - 1, argv + 1);
    long v;
    char err[64];
    int rc = 0;
    if (expr && calc_eval(sh, expr, &v, err, sizeof(err)) == 0) {
        sh_printf(sh, "%ld\n", v);
        if (stream_is_console(sh->out) && (v > 9 || v < 0)) {
            sh_role(sh, ROLE_MUTED);
            sh_printf(sh, "  hex 0x%lx\n", (unsigned long)v);
            sh_reset(sh);
        }
    } else {
        rc = sh_error(sh, "calc", "%s", expr ? err : "out of memory");
    }
    kfree(expr);
    return rc;
}

COMMAND(let, NULL, "shell", "let NAME = expression", "assign the result of arithmetic to a variable")
{
    char *text = argc > 1 ? join_args(argc - 1, argv + 1) : NULL;
    char *eq = text ? strchr(text, '=') : NULL;
    if (!eq) {
        kfree(text);
        return command_usage(sh, command_find("let"));
    }
    *eq = 0;
    char *name = str_trim(text);
    long v;
    char err[64];
    int rc = 0;
    if (!valid_name(name))
        rc = sh_error(sh, "let", "invalid name '%s'", name);
    else if (calc_eval(sh, eq + 1, &v, err, sizeof(err)) != 0)
        rc = sh_error(sh, "let", "%s", err);
    else {
        char buf[24];
        ksnprintf(buf, sizeof(buf), "%ld", v);
        sh_set(sh, name, buf);
    }
    kfree(text);
    return rc;
}

COMMAND(which, "where", "shell", "which name...", "show what a command name refers to")
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *alias = strmap_get(&sh->aliases, argv[i]);
        char path[PATH_MAX_LEN];
        const struct command *cmd;
        sh_role(sh, ROLE_SYNTAX_COMMAND);
        sh_puts(sh, argv[i]);
        sh_reset(sh);
        if (alias)
            sh_printf(sh, ": alias for '%s'\n", alias);
        else if (sh_function_find(sh, argv[i]))
            sh_printf(sh, ": shell function\n");
        else if ((cmd = command_find(argv[i])))
            sh_printf(sh, ": built-in command (%s)\n", cmd->category);
        else if (exec_find(sh, argv[i], path, sizeof(path)) == 0)
            sh_printf(sh, ": %s\n", path);
        else
            sh_printf(sh, ": not found\n"), rc = 1;
    }
    return rc;
}

COMMAND(read, NULL, "shell", "read [-p prompt] NAME...", "read a line of input into variables")
{
    struct getopt g;
    getopt_init(&g, argc, argv);
    int c;
    const char *prompt = NULL;
    while ((c = getopt_next(&g, "p:")) != -1) {
        if (c != 'p')
            return command_usage(sh, command_find("read"));
        prompt = g.arg;
    }
    if (prompt)
        sh_puts(sh, prompt);
    char line[CONFIG_LINE_MAX];
    long n = stream_getline(sh->in, line, sizeof(line));
    if (n < 0)
        return 1;
    char *p = line;
    for (int i = g.index; i < argc; i++) {
        while (is_space((uint8_t)*p))
            p++;
        char *start = p;
        if (i + 1 < argc) {
            while (*p && !is_space((uint8_t)*p))
                p++;
            if (*p)
                *p++ = 0;
        }
        sh_set(sh, argv[i], start);
    }
    if (g.index >= argc)
        sh_set(sh, "REPLY", line);
    return 0;
}

COMMAND(funcs, "functions", "shell", "funcs [name]", "list shell functions or show one")
{
    for (size_t i = 0; i < sh->func_count; i++) {
        if (argc > 1 && strcmp(argv[1], sh->funcs[i].name) != 0)
            continue;
        sh_role(sh, ROLE_SYNTAX_COMMAND);
        sh_printf(sh, "func %s\n", sh->funcs[i].name);
        sh_reset(sh);
        if (argc > 1) {
            sh_puts(sh, sh->funcs[i].body);
            sh_role(sh, ROLE_SYNTAX_COMMAND);
            sh_printf(sh, "end\n");
            sh_reset(sh);
        }
    }
    return 0;
}
