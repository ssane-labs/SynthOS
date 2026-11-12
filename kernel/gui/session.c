#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* Session: the app windows that are open are written to ~/.session a few seconds after they change (app, the file
   it was opened with, place and size); after the next start the apps open again where they were (setting reopen). */

#define MAX_ARGS 24
#define SAVE_EVERY_MS 4000

struct launch_arg {
    struct window *w;
    char arg[PATH_MAX_LEN];
};
static struct launch_arg *args; /* on the heap: the kernel image has little room */
static int next_arg;
static bool reopen = true, restoring, ready; /* ready: the last session was reopened (or not wanted) */
static uint32_t saved_hash;
static uint64_t last_check;

void session_launched(const struct gui_app *app, const char *arg)
{
    struct window *w = wm_focused();
    if (!w || w->app != app || !arg || !*arg)
        return;
    if (!args && !(args = kzalloc(MAX_ARGS * sizeof(*args))))
        return;
    int slot = next_arg++ % MAX_ARGS;
    for (int i = 0; i < MAX_ARGS; i++)
        if (args[i].w == w)
            slot = i;
    args[slot].w = w;
    str_copy(args[slot].arg, arg, sizeof(args[slot].arg));
}

struct collect {
    char *text;
    size_t len, cap;
};

static void collect_window(struct window *w, void *ctx)
{
    struct collect *c = ctx;
    if (!w->app || w->owner || (w->flags & (WIN_HIDDEN | WIN_POPUP | WIN_DIALOG | WIN_OVERLAY)) || !w->app->launch)
        return;
    const char *arg = "";
    for (int i = 0; args && i < MAX_ARGS; i++)
        if (args[i].w == w && wm_window_alive(w))
            arg = args[i].arg;
    if (c->len + PATH_MAX_LEN + 128 > c->cap)
        return;
    c->len += (size_t)ksnprintf(c->text + c->len, c->cap - c->len, "%s\t%s\t%d\t%d\t%d\t%d\t%d\n", w->app->name, arg,
                                w->frame.x, w->frame.y, w->surface.w, w->surface.h,
                                (w->flags & WIN_MAXIMIZED) ? 1 : (w->flags & WIN_MINIMIZED) ? 2 : 0);
}

static void session_file(char *out, size_t cap)
{
    path_join(user_home(), ".session", out, cap);
}

/* from the desktop loop: writes the file when the windows changed */
void session_tick(void)
{
    uint64_t now = hal_ticks_ms();
    if (!ready || restoring || now - last_check < SAVE_EVERY_MS)
        return;
    last_check = now;
    struct collect c = {kmalloc(8192), 0, 8192};
    if (!c.text)
        return;
    c.text[0] = 0;
    wm_foreach(collect_window, &c);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < c.len; i++)
        h = (h ^ (uint8_t)c.text[i]) * 16777619u;
    if (h != saved_hash) {
        char path[PATH_MAX_LEN];
        session_file(path, sizeof(path));
        if (vfs_write_all(path, c.text, c.len) == 0)
            saved_hash = h;
    }
    kfree(c.text);
}

/* opens the apps of the last session; returns how many */
int session_restore(void)
{
    ready = true;
    if (!reopen)
        return 0;
    char path[PATH_MAX_LEN], *data;
    size_t len;
    session_file(path, sizeof(path));
    if (vfs_read_all(path, &data, &len) != 0)
        return 0;
    restoring = true;
    int opened = 0;
    for (char *line = data; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *f[7] = {line};
        int n = 1;
        for (char *p = line; *p && n < 7; p++)
            if (*p == '\t') {
                *p = 0;
                f[n++] = p + 1;
            }
        const struct gui_app *app = n == 7 ? gui_app_find(f[0]) : NULL;
        if (app && (!f[1][0] || vfs_exists(f[1]))) {
            gui_app_launch(app, f[1][0] ? f[1] : NULL);
            struct window *w = wm_focused();
            if (w && w->app == app) {
                long v[5];
                for (int k = 0; k < 5; k++)
                    v[k] = str_to_long(f[2 + k], NULL, 10);
                if (w->flags & WIN_RESIZABLE)
                    wm_resize_client(w, (int)MAX(v[2], (long)w->min_w), (int)MAX(v[3], (long)w->min_h));
                wm_move(w, (int)v[0], (int)v[1]);
                if (v[4] == 1 && !(w->flags & WIN_MAXIMIZED))
                    wm_toggle_maximize(w);
                opened++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    restoring = false;
    last_check = 0;
    return opened;
}

static void get_reopen(char *buf, size_t cap)
{
    setting_flag_get(reopen, buf, cap);
}

static int set_reopen(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return setting_flag(value, &reopen);
}

static const struct setting s_reopen = {"reopen", "on|off", "open the windows of the last session again at the start",
                                        get_reopen, set_reopen};
SETTING(s_reopen);

COMMAND(session, NULL, "system", "session [show|save|restore|forget]", "the app windows that open again at the start")
{
    const char *op = argc > 1 ? argv[1] : "show";
    char path[PATH_MAX_LEN];
    session_file(path, sizeof(path));
    if (!strcmp(op, "save")) {
        ready = true;
        last_check = 0;
        saved_hash = 0;
        session_tick();
    } else if (!strcmp(op, "restore")) {
        sh_printf(sh, "opened %d apps\n", session_restore());
        return 0;
    } else if (!strcmp(op, "forget")) {
        vfs_unlink(path);
        saved_hash = 0;
        return 0;
    } else if (strcmp(op, "show")) {
        return command_usage(sh, command_find("session"));
    }
    char *data = NULL;
    size_t len = 0;
    if (vfs_read_all(path, &data, &len) != 0 || !len) {
        sh_puts(sh, "no saved session\n");
        kfree(data);
        return 0;
    }
    for (char *line = data; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *tab = strchr(line, '\t'), *tab2 = tab ? strchr(tab + 1, '\t') : NULL;
        if (tab && tab2) {
            *tab = *tab2 = 0;
            sh_printf(sh, "%-16s %s\n", line, tab[1] ? tab + 1 : "-");
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    return 0;
}
