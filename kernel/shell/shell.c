#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/lineedit.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "internal.h"

#define HISTORY_FILE "/home/.history"

static struct shell *current;

void interactive_hooks(struct shell *sh, struct lineedit_hooks *hooks);

struct shell *shell_current(void)
{
    return current;
}

struct shell *shell_create(void)
{
    struct shell *sh = kzalloc(sizeof(*sh));
    if (!sh)
        return NULL;
    sh->in = stream_console();
    sh->out = stream_console();
    sh->err = stream_console();
    str_copy(sh->cwd, "/", sizeof(sh->cwd));
    sh_set(sh, "HOME", "/home");
    sh_set(sh, "PATH", "/bin:/apps:/dos:/home/bin");
    sh_set(sh, "PATHEXT", ".sx;.com;.exe;.sh");
    sh_set(sh, "USER", CONFIG_USER);
    sh_set(sh, "HOSTNAME", CONFIG_HOSTNAME);
    sh_set(sh, "SHELL", "synsh");
    sh_set(sh, "OS", SYNTH_NAME);
    sh_set(sh, "VERSION", SYNTH_VERSION);
    sh_set(sh, "PROMPT", prompt_preset("neon"));
    sh_set(sh, "PROMPT2", "%{muted}  ...%{prompt.symbol}%>%{/} ");
    if (vfs_is_dir("/home"))
        str_copy(sh->cwd, "/home", sizeof(sh->cwd));
    if (!current)
        current = sh;
    return sh;
}

static char special[64];

const char *sh_get(struct shell *sh, const char *name)
{
    if (name[0] && !name[1]) {
        char c = name[0];
        if (c == '?') {
            ksnprintf(special, sizeof(special), "%d", sh->status);
            return special;
        }
        if (c == '#') {
            ksnprintf(special, sizeof(special), "%d", sh->argc > 0 ? sh->argc - 1 : 0);
            return special;
        }
        if (is_digit((uint8_t)c)) {
            int idx = c - '0';
            return idx < sh->argc ? sh->argv[idx] : "";
        }
        if (c == '@' || c == '*') {
            static char joined[CONFIG_LINE_MAX];
            joined[0] = 0;
            for (int i = 1; i < sh->argc; i++) {
                if (i > 1)
                    str_append(joined, " ", sizeof(joined));
                str_append(joined, sh->argv[i], sizeof(joined));
            }
            return joined;
        }
    }
    if (strcmp(name, "PWD") == 0)
        return sh->cwd;
    if (strcmp(name, "RANDOM") == 0) {
        ksnprintf(special, sizeof(special), "%u", hal_entropy() % 32768u);
        return special;
    }
    if (strcmp(name, "UPTIME") == 0) {
        ksnprintf(special, sizeof(special), "%u", (unsigned)(hal_ticks_ms() / 1000));
        return special;
    }
    if (strcmp(name, "COLUMNS") == 0 || strcmp(name, "LINES") == 0) {
        ksnprintf(special, sizeof(special), "%d", name[0] == 'C' ? con_cols() : con_rows());
        return special;
    }
    return strmap_get(&sh->vars, name);
}

void sh_set(struct shell *sh, const char *name, const char *value)
{
    strmap_set(&sh->vars, name, value);
}

void sh_unset(struct shell *sh, const char *name)
{
    strmap_remove(&sh->vars, name);
}

int sh_resolve(struct shell *sh, const char *path, char *out, size_t cap)
{
    if (is_alpha((uint8_t)path[0]) && path[1] == ':')
        return winpath_to_vfs(sh->cwd, path, out, cap);
    return path_resolve(sh->cwd, path, out, cap);
}

int sh_chdir(struct shell *sh, const char *path)
{
    char target[PATH_MAX_LEN];
    int err = sh_resolve(sh, path, target, sizeof(target));
    if (err)
        return err;
    struct vstat st;
    err = vfs_stat(target, &st);
    if (err)
        return err;
    if (st.type != VN_DIR)
        return E_NOTDIR;
    sh_set(sh, "OLDPWD", sh->cwd);
    str_copy(sh->cwd, target, sizeof(sh->cwd));
    return 0;
}

int sh_printf(struct shell *sh, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = stream_vprintf(sh->out, fmt, ap);
    va_end(ap);
    return n;
}

void sh_puts(struct shell *sh, const char *s)
{
    stream_puts(sh->out, s);
}

void sh_role(struct shell *sh, enum theme_role role)
{
    if (stream_is_console(sh->out))
        con_role(role);
}

void sh_reset(struct shell *sh)
{
    if (stream_is_console(sh->out) || stream_is_console(sh->err))
        con_reset();
}

static int vreport(struct shell *sh, const char *who, const char *fmt, va_list ap)
{
    bool color = stream_is_console(sh->err);
    if (color)
        con_role(ROLE_ERROR);
    if (who)
        stream_printf(sh->err, "%s: ", who);
    stream_vprintf(sh->err, fmt, ap);
    if (color)
        con_reset();
    stream_puts(sh->err, "\n");
    return 1;
}

int sh_error(struct shell *sh, const char *who, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(sh, who, fmt, ap);
    va_end(ap);
    return 1;
}

int sh_fail(struct shell *sh, int code, const char *who, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(sh, who, fmt, ap);
    va_end(ap);
    return code;
}

int sh_fs_error(struct shell *sh, const char *who, const char *path, int err)
{
    if (who)
        return sh_error(sh, who, "%s: %s", path, err_str(err));
    return sh_error(sh, path, "%s", err_str(err));
}

struct sh_function *sh_function_find(struct shell *sh, const char *name)
{
    for (size_t i = 0; i < sh->func_count; i++)
        if (strcmp(sh->funcs[i].name, name) == 0)
            return &sh->funcs[i];
    return NULL;
}

void sh_function_define(struct shell *sh, const char *name, const char *body)
{
    struct sh_function *fn = sh_function_find(sh, name);
    if (fn) {
        kfree(fn->body);
        fn->body = kstrdup(body);
        return;
    }
    struct sh_function *funcs = krealloc(sh->funcs, (sh->func_count + 1) * sizeof(*funcs));
    if (!funcs)
        return;
    sh->funcs = funcs;
    sh->funcs[sh->func_count++] = (struct sh_function){kstrdup(name), kstrdup(body)};
}

static int run_with_args(struct shell *sh, const char *text, int argc, char **argv, bool catch_exit)
{
    if (sh->call_depth >= SH_MAX_DEPTH)
        return sh_error(sh, NULL, "maximum nesting depth exceeded");
    int saved_argc = sh->argc, saved_loop = sh->loop_depth;
    char **saved_argv = sh->argv;
    sh->argc = argc;
    sh->argv = argv;
    sh->loop_depth = 0;
    sh->call_depth++;
    int rc = script_run_text(sh, text);
    sh->call_depth--;
    sh->argc = saved_argc;
    sh->argv = saved_argv;
    sh->loop_depth = saved_loop;
    if (sh->ctl == CTL_RETURN || (catch_exit && sh->ctl == CTL_EXIT)) {
        rc = sh->status;
        sh->ctl = CTL_NONE;
    }
    if (sh->ctl == CTL_BREAK || sh->ctl == CTL_CONTINUE)
        sh->ctl = CTL_NONE;
    return sh->status = rc;
}

int sh_function_call(struct shell *sh, struct sh_function *fn, int argc, char **argv)
{
    char *body = kstrdup(fn->body);
    if (!body)
        return 1;
    int rc = run_with_args(sh, body, argc, argv, false);
    kfree(body);
    return rc;
}

int shell_exec(struct shell *sh, const char *line)
{
    return sh->status = script_run_text(sh, line);
}

int shell_exec_script(struct shell *sh, const char *text, int argc, char **argv)
{
    return run_with_args(sh, text, argc, argv, true);
}

int shell_source(struct shell *sh, const char *path, int argc, char **argv)
{
    char *text;
    int err = vfs_read_all(path, &text, NULL);
    if (err)
        return err;
    int rc = shell_exec_script(sh, text, argc, argv);
    kfree(text);
    return rc;
}

int shell_capture(struct shell *sh, const char *line, char **out)
{
    struct stream *buf = stream_buffer();
    *out = NULL;
    if (!buf)
        return 1;
    struct stream *saved = sh->out;
    sh->out = buf;
    int saved_status = sh->status;
    int rc = script_run_text(sh, line);
    sh->out = saved;
    if (sh->ctl == CTL_RETURN)
        sh->ctl = CTL_NONE;
    size_t len;
    const char *data = stream_buffer_data(buf, &len);
    *out = kstrndup(data, len);
    stream_close(buf);
    UNUSED(saved_status);
    sh->status = rc;
    return rc;
}

static void render_status(int cols)
{
    struct shell *sh = current;
    struct rtc_time t;
    hal_rtc(&t);
    char left[96], right[64], clock[16];
    uint8_t base = theme_attr(ROLE_STATUS), accent = theme_attr(ROLE_STATUS_ACCENT);
    ksnprintf(left, sizeof(left), " \x04 %s ", SYNTH_NAME);
    con_status_put(0, left, accent);
    int x = (int)strlen(left);
    if (sh) {
        ksnprintf(left, sizeof(left), " %s ", sh->cwd);
        con_status_put(x, left, base);
    }
    ksnprintf(clock, sizeof(clock), " %02u:%02u ", t.hour, t.minute);
    struct heap_stats hs;
    heap_get_stats(&hs);
    ksnprintf(right, sizeof(right), "%s \xb3 mem %u%% \xb3", theme_current()->name,
              (unsigned)(hs.total ? hs.used * 100 / hs.total : 0));
    int rx = cols - (int)strlen(clock) - (int)strlen(right) - 1;
    if (rx > x + 2)
        con_status_put(rx, right, base);
    con_status_put(cols - (int)strlen(clock), clock, accent);
}

#define LB "\xdc"
#define UB "\xdf"
#define FB "\xdb"

void shell_banner(void)
{
    static const char *const letters[7][3] = {
        {LB UB UB UB, UB UB UB LB, LB LB LB UB},
        {FB "   " FB, UB LB LB LB UB, "  " FB "  "},
        {FB LB "  " FB, FB " " UB LB FB, FB "   " FB},
        {UB FB UB, " " FB " ", " " FB " "},
        {FB "  " FB, FB UB UB FB, FB "  " FB},
        {LB UB UB LB, FB "  " FB, UB LB LB UB},
        {LB UB UB UB, UB UB UB LB, LB LB LB UB},
    };
    static const enum theme_role colors[7] = {ROLE_LOGO1, ROLE_LOGO1, ROLE_LOGO2, ROLE_LOGO2,
                                              ROLE_LOGO2, ROLE_LOGO3, ROLE_LOGO3};
    if (con_cols() < 44) {
        con_role(ROLE_LOGO1);
        kprintf("%s %s\n", SYNTH_NAME, SYNTH_VERSION);
        con_reset();
        return;
    }
    kprintf("\n");
    for (int row = 0; row < 3; row++) {
        kprintf("   ");
        for (int l = 0; l < 7; l++) {
            con_role(colors[l]);
            kprintf("%s ", letters[l][row]);
        }
        con_reset();
        if (row == 0) {
            con_role(ROLE_MUTED);
            kprintf("  v%s", SYNTH_VERSION);
        } else if (row == 1) {
            con_role(ROLE_ACCENT2);
            kprintf("  \"%s\"", SYNTH_CODENAME);
        } else {
            con_role(ROLE_MUTED);
            kprintf("  %s", hal_arch());
        }
        con_reset();
        kprintf("\n");
    }
    kprintf("\n");
}

static void draw_prompt(void *ctx)
{
    prompt_render(ctx);
}

static void draw_continuation(void *ctx)
{
    struct shell *sh = ctx;
    const char *p2 = sh_get(sh, "PROMPT2");
    const char *saved = sh_get(sh, "PROMPT");
    char *copy = kstrdup(saved ? saved : "");
    sh_set(sh, "PROMPT", p2 ? p2 : "> ");
    prompt_render(sh);
    sh_set(sh, "PROMPT", copy ? copy : "");
    kfree(copy);
}

void shell_repl(struct shell *sh)
{
    current = sh;
    sh->interactive = true;
    con_set_status_renderer(render_status);
    history_load(&sh->history, HISTORY_FILE);

    char *line = kmalloc(CONFIG_LINE_MAX);
    char *text = kmalloc(CONFIG_LINE_MAX * 4);
    if (!line || !text)
        return;
    for (;;) {
        sh->ctl = CTL_NONE;
        int x;
        con_cursor(&x, NULL);
        if (x != 0)
            kprintf("\n");
        con_update_status();

        struct lineedit_hooks hooks;
        interactive_hooks(sh, &hooks);
        hooks.prompt = draw_prompt;
        text[0] = 0;
        int n;
        for (;;) {
            n = lineedit_read(line, CONFIG_LINE_MAX, &hooks);
            if (n < 0)
                break;
            if (text[0])
                str_append(text, "\n", CONFIG_LINE_MAX * 4);
            str_append(text, line, CONFIG_LINE_MAX * 4);
            if (block_balance(text) <= 0)
                break;
            hooks.prompt = draw_continuation;
        }
        if (n == LE_EOF && !con_is_grid())
            break;
        if (n < 0)
            continue;

        char *flat = kstrdup(text);
        if (flat) {
            for (char *p = flat; *p; p++)
                if (*p == '\n')
                    *p = ';';
            history_add(&sh->history, flat);
            kfree(flat);
        }
        input_set_break(true);
        shell_exec(sh, text);
        bool interrupted = input_break_pending();
        input_set_break(false);
        if (interrupted) {
            con_role(ROLE_MUTED);
            kprintf("^C\n");
            con_reset();
        }
        if (sh->ctl == CTL_EXIT)
            break;
    }
    history_save(&sh->history, HISTORY_FILE);
    kfree(line);
    kfree(text);
}
