#include <synth/command.h>
#include <synth/error.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/vfs.h>

#include "systool.h"

/* Help and tips: the guides in /docs (and the welcome tour) in one window with a search that looks through all of
   them and through every command's description at once, the command reference, and a page of tips. A tip of the
   day comes as a notification once a day after login, unless switched off ('tips off' or the switch on the Tips
   page). 'tips' prints tips in the terminal and 'apropos' searches the guides and commands from there. */

#define TIPS_STATE "/home/etc/tips"
#define TIP_DELAY_MS 20000

struct guide {
    const char *title, *path;
    enum symbol_id sym;
};

static const struct guide guides[] = {
    {"Welcome", "/home/readme.txt", SYM_STAR},
    {"The Desktop", "/docs/desktop.txt", SYM_DESKTOP},
    {"Making It Yours", "/docs/customize.txt", SYM_BRUSH},
    {"The Shell", "/docs/shell.txt", SYM_TERMINAL},
    {"Windows Programs", "/docs/windows.txt", SYM_APPS},
    {"Internet", "/docs/internet.txt", SYM_GLOBE},
    {"MS-DOS Programs", "/docs/dos.txt", SYM_DOC},
    {"SX Programs", "/docs/sx.txt", SYM_DOC},
};

static const char *const tips[] = {
    "Press Win or Ctrl+Esc for Launchpad, then just type part of an app's name.",
    "Ctrl+Space searches apps, files, settings and what is inside your documents.",
    "Drag a window to the left or right edge to give it half the screen, to a corner for a quarter.",
    "Win+arrow keys tile the active window without the mouse.",
    "F3 or Ctrl+Up shows every window side by side in Mission Control.",
    "Win+Ctrl+Left and Right switch between desktops; Win+Ctrl+D adds one.",
    "In Files, select a file and press Space to look at it without opening an app.",
    "Win+Shift+4 takes a screenshot of an area you drag; the picture also goes on the clipboard.",
    "Right-click the desktop and pick Edit Widgets to add a clock, the weather, notes or music.",
    "Rest the pointer on an app in the Dock to see its windows.",
    "Hot corners can open Mission Control or Launchpad: Settings > Desktop & Dock.",
    "The App Store installs programs with one click: Git, Python and more (store list in the terminal).",
    "Double-clicking a .exe runs it: 32-bit and 64-bit Windows programs work without emulation.",
    "Double-click a .msi, .msix or .ttf file to install it.",
    "autostart add Music opens an app, a file or a command every time you log in.",
    "config dnd on stops notification banners; they still collect in Notification Center (click the clock).",
    "Night Shift warms the colours in the evening: config nightshift auto.",
    "theme lists colour themes for the terminal with swatches; theme use <name> switches.",
    "Ctrl+R in the terminal searches the commands you typed before.",
    "help <command> explains any command, and apropos <words> finds the command you are looking for.",
    "sudo runs one command as an administrator when you are signed in with a standard account.",
    "Older versions of a file are kept: right-click it in Files for Versions.",
    "Deleted files go to the Trash in the Dock until you empty it.",
    "Time Machine-style restore points: the Restore app brings back system settings and files.",
    "vpn import my.conf adds a WireGuard VPN; vpn up my connects it.",
    "Split view in Chromium shows two pages side by side; the sidebar holds your tabs.",
    "wallpaper bing makes the photo of the day your wallpaper.",
    "config look night switches the whole design: Dune, Mist or Night.",
    "Lock the screen with Win+L; logout switches to another person.",
    "Unpack .zip and .7z archives by double-clicking them in Files.",
};

/* ---- tips state: "on <day>" or "off <day>", the day the last tip was shown ---- */

static bool tips_on = true;
static uint32_t tips_day, tips_next;
static bool state_read;

static void state_load(void)
{
    if (state_read)
        return;
    state_read = true;
    char *text;
    size_t size;
    if (vfs_read_all(TIPS_STATE, &text, &size) != 0)
        return;
    tips_on = strncmp(text, "off", 3) != 0;
    const char *p = strchr(text, ' ');
    tips_day = p ? (uint32_t)str_to_long(p + 1, NULL, 10) : 0;
    p = p ? strchr(p + 1, ' ') : NULL;
    tips_next = p ? (uint32_t)str_to_long(p + 1, NULL, 10) : 0;
    kfree(text);
}

static void state_save(void)
{
    char text[48];
    int n = ksnprintf(text, sizeof(text), "%s %u %u\n", tips_on ? "on" : "off", tips_day, tips_next);
    vfs_mkdirs("/home/etc");
    vfs_write_all(TIPS_STATE, text, (size_t)n);
}

static uint32_t today(void)
{
    return time_now() / 86400;
}

/* the next tip in turn, so each one comes up before any repeats */
static const char *next_tip(void)
{
    state_load();
    const char *tip = tips[tips_next % ARRAY_SIZE(tips)];
    tips_next = (tips_next + 1) % ARRAY_SIZE(tips);
    state_save();
    return tip;
}

static void tick(void)
{
    static uint64_t started;
    static bool done;
    if (done)
        return;
    if (!started)
        started = hal_ticks_ms();
    if (hal_ticks_ms() - started < TIP_DELAY_MS || gui_locked() || gui_setup_active())
        return;
    done = true;
    state_load();
    if (!tips_on || tips_day == today())
        return;
    tips_day = today();
    gui_notify("Tip", next_tip());
}

/* ---- search ---- */

static bool has(const char *line, size_t n, const char *word, size_t k)
{
    for (size_t i = 0; k && i + k <= n; i++)
        if (!str_ncasecmp(line + i, word, k))
            return true;
    return false;
}

/* every word of query is in the line */
static bool matches(const char *line, size_t n, const char *query)
{
    bool any = false;
    for (const char *w = query; *w;) {
        while (*w == ' ')
            w++;
        size_t k = 0;
        while (w[k] && w[k] != ' ')
            k++;
        if (!k)
            break;
        if (!has(line, n, w, k))
            return false;
        any = true;
        w += k;
    }
    return any;
}

struct found {
    char *out;
    size_t len, cap;
    const char *query;
    int hits;
    bool header;
};

static void add(struct found *f, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void add(struct found *f, const char *fmt, ...)
{
    if (f->len + 300 >= f->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    f->len += (size_t)kvsnprintf(f->out + f->len, f->cap - f->len, fmt, ap);
    va_end(ap);
}

static void command_hit(const struct command *c, void *ctx)
{
    struct found *f = ctx;
    char line[300];
    int n = ksnprintf(line, sizeof(line), "%s %s %s", c->name, c->aliases ? c->aliases : "", c->summary);
    if (!matches(line, (size_t)n, f->query))
        return;
    if (!f->header)
        add(f, "Commands\n");
    f->header = true;
    add(f, "  %-12s %s\n", c->name, c->summary);
    f->hits++;
}

/* the matching lines of every guide and the matching commands, as text; kfree it */
static char *search(const char *query, int *hits)
{
    struct found f = {kmalloc(16384), 0, 16384, query, 0, false};
    *hits = 0;
    if (!f.out)
        return NULL;
    f.out[0] = 0;
    command_foreach(command_hit, &f);
    for (size_t g = 0; g < ARRAY_SIZE(guides); g++) {
        char *text;
        size_t size;
        if (vfs_read_all(guides[g].path, &text, &size) != 0)
            continue;
        f.header = false;
        for (const char *p = text; p < text + size;) {
            const char *end = memchr(p, '\n', (size_t)(text + size - p));
            size_t n = end ? (size_t)(end - p) : (size_t)(text + size - p);
            if (matches(p, n, query)) {
                if (!f.header)
                    add(&f, "%s%s\n", f.len ? "\n" : "", guides[g].title);
                f.header = true;
                add(&f, "  %.*s\n", (int)MIN(n, 200), p);
                f.hits++;
            }
            p += n + 1;
        }
        kfree(text);
    }
    *hits = f.hits;
    return f.out;
}

/* ---- the app ---- */

enum { PAGE_TIPS = -2, PAGE_COMMANDS = -1 }; /* 0.. are guides */

struct help_app {
    struct ui_state st;
    int page, tip;
    struct ui_text query;
    char last_query[128];
    int shown_page, hits;
    char *text;
    int scroll;
};

static struct window *open_win;

static void set_text(struct help_app *a, char *text)
{
    kfree(a->text);
    a->text = text;
    a->scroll = 0;
}

/* plain text: headings (a line in capitals or followed by ===) in bold */
static void text_view(struct ui *ui, struct rect r, struct help_app *a)
{
    const struct ui_font *f = &font_mono;
    int lh = f->height + 3, rows = MAX(1, (r.h - 16) / lh), lines = 0;
    for (const char *p = a->text; p && *p; p++)
        lines += *p == '\n';
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(r, ui->e->x, ui->e->y)) {
        a->scroll -= ui->e->wheel * 3;
        ui->consumed = true;
    }
    int key = ui_key(ui);
    if (key == KEY_PGUP || key == KEY_PGDN || key == KEY_HOME || key == KEY_END || key == KEY_UP || key == KEY_DOWN) {
        a->scroll += key == KEY_PGUP ? -rows : key == KEY_PGDN ? rows : key == KEY_UP ? -1 : key == KEY_DOWN ? 1
                   : key == KEY_HOME ? -1000000 : 1000000;
        ui->consumed = true;
    }
    a->scroll = CLAMP(a->scroll, 0, MAX(0, lines - rows));
    int bar = a->scroll * lh;
    if (ui_scrollbar(ui, (struct rect){r.x + r.w - 12, r.y + 6, 10, r.h - 12}, &bar, lines * lh, rows * lh))
        a->scroll = bar / lh;
    if (!ui->c || !a->text)
        return;
    struct canvas clip = *ui->c;
    clip.clip = rect_intersect(ui->c->clip, rect_inset(r, 4));
    int index = 0, y = r.y + 8;
    for (const char *p = a->text; *p && y < r.y + r.h - 8;) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (index++ >= a->scroll) {
            const char *next = end ? end + 1 : p + n;
            bool heading = n && p[0] != ' ' && (a->shown_page == -100 || *next == '=' || (n > 3 && p[1] >= 'A' && p[1] <= 'Z' && p[2] != ' '
                                                                && p[n - 1] >= 'A' && p[n - 1] <= 'Z')) && p[0] != '=';
            if (n && p[0] != '=')
                gfx_text_n(&clip, r.x + 14, y, heading ? &font_bold : f, p, n, heading ? style.text : style.secondary);
            if (n || index > 1)
                y += lh;
        }
        p += n + (end ? 1 : 0);
    }
}

static void load_page(struct help_app *a)
{
    const char *q = a->query.buf;
    if (q[0]) {
        if (!strcmp(q, a->last_query) && a->shown_page == -100)
            return;
        str_copy(a->last_query, q, sizeof(a->last_query));
        a->shown_page = -100;
        set_text(a, search(q, &a->hits));
        return;
    }
    if (a->shown_page == a->page)
        return;
    a->shown_page = a->page;
    a->last_query[0] = 0;
    if (a->page == PAGE_COMMANDS) {
        set_text(a, tool_run("help"));
    } else if (a->page >= 0) {
        char *text;
        size_t size;
        set_text(a, vfs_read_all(guides[a->page].path, &text, &size) == 0 ? text : kstrdup("This guide is missing."));
    } else {
        set_text(a, NULL);
    }
}

static void tips_page(struct ui *ui, struct rect r, struct help_app *a)
{
    int n = (int)ARRAY_SIZE(tips);
    struct rect card = {r.x + 40, r.y + 40, r.w - 80, 112};
    if (ui->c) {
        gfx_round(ui->c, card, 16, style.dark ? RGB(36, 36, 42) : RGB(244, 244, 248));
        ui_settings_icon(ui->c, card.x + 24, card.y + 24, 36, SYM_STAR, style.accent);
        char count[24];
        ksnprintf(count, sizeof(count), "Tip %d of %d", a->tip + 1, n);
        gfx_text(ui->c, card.x + 76, card.y + 30, &font_small, count, style.tertiary);
        ui_wrap_text(ui->c, (struct rect){card.x + 76, card.y + 54, card.w - 100, card.h - 60}, &font_sans,
                     tips[a->tip], style.text);
    }
    int y = card.y + card.h + 16;
    if (ui_button(ui, (struct rect){card.x, y, 100, 28}, "Previous"))
        a->tip = (a->tip + n - 1) % n;
    if (ui_button_primary(ui, (struct rect){card.x + 110, y, 100, 28}, "Next Tip"))
        a->tip = (a->tip + 1) % n;
    int key = ui_key(ui);
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        a->tip = (a->tip + (key == KEY_LEFT ? n - 1 : 1)) % n;
        ui->consumed = true;
    }
    state_load();
    bool on = tips_on;
    ui_label(ui, (struct rect){card.x, y + 52, card.w - 70, 24}, "Show a tip after logging in, once a day",
             &font_sans, style.text, ALIGN_LEFT);
    if (ui_switch(ui, (struct rect){card.x + card.w - 50, y + 52, 50, 24}, &on)) {
        tips_on = on;
        state_save();
    }
}

static void frame(struct ui *ui, void *data)
{
    struct help_app *a = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    ui_sidebar_panel(ui, TOOL_SIDE, H, style.window);
    ui_search(ui, (struct rect){14, 44, TOOL_SIDE - 28, 26}, &a->query, "Search Help");
    int y = 80;
    ui_sidebar_header(ui, (struct rect){22, y, TOOL_SIDE - 32, 20}, "Guides");
    y += 22;
    for (size_t i = 0; i < ARRAY_SIZE(guides); i++, y += TOOL_ROW)
        if (ui_sidebar_item(ui, (struct rect){14, y, TOOL_SIDE - 26, TOOL_ROW - 2}, guides[i].sym, guides[i].title,
                            !a->query.buf[0] && a->page == (int)i)) {
            a->page = (int)i;
            ui_text_set(&a->query, "");
        }
    ui_sidebar_header(ui, (struct rect){22, y + 6, TOOL_SIDE - 32, 20}, "More");
    y += 28;
    if (ui_sidebar_item(ui, (struct rect){14, y, TOOL_SIDE - 26, TOOL_ROW - 2}, SYM_TERMINAL, "Commands",
                        !a->query.buf[0] && a->page == PAGE_COMMANDS)) {
        a->page = PAGE_COMMANDS;
        ui_text_set(&a->query, "");
    }
    y += TOOL_ROW;
    if (ui_sidebar_item(ui, (struct rect){14, y, TOOL_SIDE - 26, TOOL_ROW - 2}, SYM_STAR, "Tips",
                        !a->query.buf[0] && a->page == PAGE_TIPS)) {
        a->page = PAGE_TIPS;
        ui_text_set(&a->query, "");
    }
    load_page(a);
    char sub[48] = "";
    const char *title = a->page >= 0 ? guides[a->page].title : a->page == PAGE_COMMANDS ? "Commands" : "Tips";
    if (a->query.buf[0]) {
        title = "Search";
        ksnprintf(sub, sizeof(sub), a->hits == 1 ? "1 result" : "%d results", a->hits);
    }
    struct rect r = tool_page(ui, W, H, title, sub[0] ? sub : NULL);
    if (!a->query.buf[0] && a->page == PAGE_TIPS) {
        tips_page(ui, r, a);
        return;
    }
    if (a->query.buf[0] && !a->hits) {
        ui_label(ui, (struct rect){r.x, r.y + 60, r.w, 20}, "Nothing in the guides or commands matches", &font_sans,
                 style.tertiary, ALIGN_CENTER);
        return;
    }
    text_view(ui, (struct rect){r.x + 8, r.y + 8, r.w - 16, r.h - 16}, a);
}

static void paint(struct window *win, struct canvas *c)
{
    struct help_app *a = win->data;
    ui_run(win, &a->st, c, NULL, frame, a);
}

static void event(struct window *win, struct gui_event *e)
{
    struct help_app *a = win->data;
    ui_run(win, &a->st, NULL, e, frame, a);
}

static void destroy(struct window *win)
{
    struct help_app *a = win->data;
    kfree(a->text);
    kfree(a);
    if (open_win == win)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

/* arg: a guide's title, "tips", "commands", or words to search for */
static void launch(const char *arg)
{
    if (open_win && wm_window_alive(open_win))
        wm_destroy(open_win);
    struct help_app *a = kzalloc(sizeof(*a));
    if (!a)
        return;
    a->shown_page = -99;
    state_load();
    a->tip = (int)(tips_next % ARRAY_SIZE(tips));
    if (arg && arg[0] == '/' && !vfs_exists(arg)) /* 'launch help word' passes the word as a path */
        arg = path_basename(arg);
    if (arg && arg[0]) {
        bool found = false;
        for (size_t i = 0; i < ARRAY_SIZE(guides) && !found; i++)
            if (!str_casecmp(arg, guides[i].title))
                a->page = (int)i, found = true;
        if (!found && !str_casecmp(arg, "tips"))
            a->page = PAGE_TIPS, found = true;
        if (!found && !str_casecmp(arg, "commands"))
            a->page = PAGE_COMMANDS, found = true;
        if (!found)
            ui_text_set(&a->query, arg);
    }
    open_win = tool_open("Help", ICON_TEXT, 900, 620, &ops, a);
    if (!open_win)
        kfree(a);
}

static const struct gui_app help_app = {"Help", "Guides, tips and a search through every command", ICON_TEXT, launch,
                                        NULL, NULL};

/* ---- commands ---- */

COMMAND(tips, NULL, "system", "tips [all|on|off]", "a tip for using SynthOS; on/off: the tip of the day after login")
{
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        state_load();
        tips_on = argv[1][1] == 'n';
        state_save();
        sh_printf(sh, "tip of the day %s\n", tips_on ? "on" : "off");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "all")) {
        for (size_t i = 0; i < ARRAY_SIZE(tips); i++)
            sh_printf(sh, "%2u. %s\n", (unsigned)i + 1, tips[i]);
        return 0;
    }
    if (argc != 1)
        return command_usage(sh, command_find("tips"));
    sh_printf(sh, "%s\n", next_tip());
    return 0;
}

COMMAND(apropos, "helpsearch", "system", "apropos <words>", "search the guides and every command's description")
{
    if (argc < 2)
        return command_usage(sh, command_find("apropos"));
    char query[128] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            str_append(query, " ", sizeof(query));
        str_append(query, argv[i], sizeof(query));
    }
    int hits;
    char *out = search(query, &hits);
    if (!out)
        return sh_error(sh, "apropos", "out of memory");
    if (!hits)
        sh_printf(sh, "nothing matches %s\n", query);
    else
        sh_puts(sh, out);
    kfree(out);
    return hits ? 0 : 1;
}

static const struct command *const commands[] = {&cmddef_tips, &cmddef_apropos};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&help_app, tick, &ext);
