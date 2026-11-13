#include <synth/config.h>
#include <synth/log.h>
#include <synth/path.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "systool.h"

/* Console: the kernel log and other live reports (Windows programs, connections, scheduler), followed as they
   grow, filtered by text or to errors only, and saved to a file; the logs of the three starts before this one too. */

enum { SRC_LOG, SRC_ERRORS, SRC_EARLIER, SRC_W32, SRC_NET, SRC_WINDOWS, SRC_MODULES, SRC_COUNT };

static const struct tool_item items[SRC_COUNT] = {
    {SYM_DOC, "Kernel Log", "Reports"},
    {SYM_INFO, "Errors & Warnings", NULL},
    {SYM_CLOCK, "Earlier Starts", NULL},
    {SYM_WINDOW, "Windows Programs", "Live"},
    {SYM_NETWORK, "Connections", NULL},
    {SYM_DESKTOP, "Desktop Windows", NULL},
    {SYM_APPS, "App Modules", NULL},
};

static const char *const commands[SRC_COUNT] = {NULL, NULL, NULL, "w32ps", "netstat", "windows", "modules"};

/* the logs of the starts before this one (saved by the desktop in /home/.logs), the latest first */
static char *earlier(void)
{
    size_t cap = 4096, len = 0;
    char *out = kmalloc(cap);
    for (int i = 1; out && i <= 3; i++) {
        char path[40], *data;
        size_t n;
        ksnprintf(path, sizeof(path), "/home/.logs/start-%d.log", i);
        if (vfs_read_all(path, &data, &n) != 0)
            continue;
        if (len + n + 64 > cap) {
            char *grown = krealloc(out, cap = len + n + 64);
            if (!grown) {
                kfree(data);
                break;
            }
            out = grown;
        }
        len += (size_t)ksnprintf(out + len, cap - len, "%s---- %d start%s ago ----\n", len ? "\n" : "", i,
                                 i > 1 ? "s" : "");
        memcpy(out + len, data, n);
        len += n;
        kfree(data);
    }
    if (out && !len)
        len = (size_t)ksnprintf(out, cap, "No logs of earlier starts yet: the desktop keeps them from now on.\n");
    if (out)
        out[len] = 0;
    return out;
}

struct console {
    struct ui_state st;
    struct ui_text find;
    struct tool_text view;
    int source;
    bool paused;
    char *log;
};

static struct window *open_win;

static void reload(struct console *c)
{
    if (c->source <= SRC_ERRORS) {
        if (!c->log)
            c->log = kmalloc(CONFIG_LOG_SIZE + 1);
        if (!c->log)
            return;
        size_t n = log_snapshot(c->log, CONFIG_LOG_SIZE + 1);
        char *copy = kmalloc(n + 1);
        if (!copy)
            return;
        if (c->source == SRC_LOG) {
            memcpy(copy, c->log, n);
            copy[n] = 0;
        } else { /* only the lines that look like trouble */
            size_t k = 0;
            for (const char *p = c->log; *p;) {
                const char *end = strchr(p, '\n');
                size_t len = end ? (size_t)(end - p) : strlen(p);
                if (tool_line_color(p, len) != style.text) {
                    memcpy(copy + k, p, len);
                    copy[k + len] = '\n';
                    k += len + 1;
                }
                p += len + (end ? 1 : 0);
            }
            copy[k] = 0;
        }
        tool_text_set(&c->view, copy);
    } else if (c->source == SRC_EARLIER) {
        tool_text_set(&c->view, earlier());
    } else {
        tool_text_set(&c->view, tool_run(commands[c->source]));
    }
}

static void save(struct console *c)
{
    char path[PATH_MAX_LEN], name[48];
    struct rtc_time t;
    time_local(&t);
    ksnprintf(name, sizeof(name), "Desktop/console-%04d%02d%02d-%02d%02d%02d.txt", t.year, t.month, t.day, t.hour,
              t.minute, t.second);
    path_join(user_home(), name, path, sizeof(path));
    const char *text = c->view.text ? c->view.text : "";
    if (vfs_write_all(path, text, strlen(text)) == 0) {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "Saved %s", path_basename(path));
        gui_notify("Console", msg);
    } else {
        gui_notify("Console", "Could not save the report");
    }
}

static void frame(struct ui *ui, void *data)
{
    struct console *c = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->e && ui->e->type == EV_TIMER) {
        if (!c->paused)
            reload(c);
        ui->consumed = true;
    }
    if (tool_sidebar(ui, H, items, SRC_COUNT, &c->source)) {
        c->view.follow = c->source <= SRC_ERRORS;
        c->view.scroll = 0;
        app_state_set_number("console", "source", c->source);
        reload(c);
    }
    char sub[32];
    ksnprintf(sub, sizeof(sub), "%d lines%s", c->view.lines, c->paused ? "  -  paused" : "");
    struct rect area = tool_page(ui, W, H, items[c->source].label, sub);
    int bx = W - 46;
    bx -= 30;
    if (ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, SYM_DOWNLOAD, false))
        save(c);
    bx -= 32;
    if (ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, SYM_RESTART, false))
        reload(c);
    bx -= 32;
    if (ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, c->paused ? SYM_PLAY : SYM_PAUSE, c->paused))
        c->paused = !c->paused;
    bx -= 190;
    ui_search(ui, (struct rect){bx, 12, 180, 28}, &c->find, "Filter");
    struct rect view = rect_inset(area, 12);
    tool_text_view(ui, view, &c->view, c->find.buf);
    if (ui->e && ui->e->type == EV_KEY && ui->e->key == KEY_CTRL('s')) {
        save(c);
        ui->consumed = true;
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct console *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct console *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    struct console *s = w->data;
    if (open_win == w)
        open_win = NULL;
    kfree(s->view.text);
    kfree(s->log);
    kfree(s);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct console *c = kzalloc(sizeof(*c));
    if (!c)
        return;
    c->source = (int)CLAMP(app_state_number("console", "source", SRC_LOG), 0, SRC_COUNT - 1);
    for (int i = 0; arg && i < SRC_COUNT; i++)
        if (!str_casecmp(arg, items[i].label) || (commands[i] && !str_casecmp(arg, commands[i])))
            c->source = i;
    c->view.follow = true;
    reload(c);
    open_win = tool_open("Console", ICON_CONSOLE, 860, 560, &ops, c);
    if (!open_win) {
        kfree(c->view.text);
        kfree(c->log);
        kfree(c);
        return;
    }
    wm_set_timer(open_win, 1000);
}

static const struct gui_app console_app = {
    .name = "Console", .description = "The kernel log and live reports, filtered and saved", .icon = ICON_CONSOLE,
    .launch = launch};
GUI_APP(console_app);
