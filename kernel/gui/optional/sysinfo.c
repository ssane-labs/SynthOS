#include <synth/config.h>
#include <synth/hal.h>
#include <synth/vfs.h>

#include "systool.h"

/* System Information: an overview of this computer, then the hardware, storage, network and software reports of
   the shell commands, each searchable and refreshable. */

struct page {
    enum symbol_id sym;
    const char *label, *header, *command;
};

static const struct page pages[] = {
    {SYM_LOGO, "Overview", "This Computer", NULL},
    {SYM_MEMORY, "Memory", "Hardware", "mem"},
    {SYM_DISPLAY, "Displays", NULL, "displays"},
    {SYM_VOLUME, "Sound", NULL, "sound"},
    {SYM_USB, "USB", NULL, "lsusb"},
    {SYM_DISK, "Volumes", "Storage", "df"},
    {SYM_FOLDER, "Mounts", NULL, "mount"},
    {SYM_CHIP, "Disks & Partitions", NULL, "part"},
    {SYM_NETWORK, "Interfaces", "Network", "ifconfig"},
    {SYM_LOCK, "Certificates", NULL, "certs"},
    {SYM_APPS, "Program Formats", "Software", "formats"},
    {SYM_USERS, "Users", NULL, "users"},
    {SYM_GEAR, "Settings", NULL, "config"},
};
#define PAGES ((int)ARRAY_SIZE(pages))

struct sysinfo {
    struct ui_state st;
    struct ui_text find;
    struct tool_text view;
    int page;
};

static struct window *open_win;

static void reload(struct sysinfo *s)
{
    if (pages[s->page].command)
        tool_text_set(&s->view, tool_run(pages[s->page].command));
    s->view.scroll = 0;
}

static void card(struct ui *ui, struct rect r, enum symbol_id sym, const char *title, const char *value,
                 const char *detail, int permille)
{
    if (ui->c) {
        gfx_round(ui->c, r, 12, style.group);
        gfx_round_frame(ui->c, r, 12, style.separator);
        gfx_symbol(ui->c, r.x + 16, r.y + 16, sym, 16, style.accent);
    }
    ui_label(ui, (struct rect){r.x + 42, r.y + 12, r.w - 54, 22}, title, &font_small, style.secondary, ALIGN_LEFT);
    ui_label(ui, (struct rect){r.x + 16, r.y + 38, r.w - 32, 26}, value, &font_bold, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){r.x + 16, r.y + 62, r.w - 32, 18}, detail, &font_small, style.tertiary, ALIGN_LEFT);
    if (permille >= 0 && ui->c) {
        struct rect bar = {r.x + 16, r.y + r.h - 18, r.w - 32, 6};
        gfx_round(ui->c, bar, 3, style.dark ? RGB(70, 70, 76) : RGB(222, 222, 228));
        gfx_round(ui->c, (struct rect){bar.x, bar.y, MAX(6, bar.w * MIN(permille, 1000) / 1000), 6}, 3,
                  permille > 850 ? RGB(255, 105, 97) : style.accent);
    }
}

static void overview(struct ui *ui, struct rect area)
{
    int x = area.x + 28, w = area.w - 56, y = area.y + 24;
    if (ui->c)
        gfx_icon(ui->c, x, y, ICON_LOGO, 72);
    char line[96];
    ksnprintf(line, sizeof(line), "%s %s", SYNTH_NAME, SYNTH_VERSION);
    ui_label(ui, (struct rect){x + 92, y + 4, w - 92, 32}, line, &font_title, style.text, ALIGN_LEFT);
    ksnprintf(line, sizeof(line), "\"%s\"  -  %s kernel", SYNTH_CODENAME, hal_arch());
    ui_label(ui, (struct rect){x + 92, y + 38, w - 92, 20}, line, &font_sans, style.secondary, ALIGN_LEFT);
    y += 100;
    int cols = w >= 600 ? 3 : 2, gap = 14, cw = (w - (cols - 1) * gap) / cols, ch = 104;
    struct rect cell[6];
    for (int i = 0; i < 6; i++)
        cell[i] = (struct rect){x + (i % cols) * (cw + gap), y + (i / cols) * (ch + gap), cw, ch};
    char value[64], detail[96];
    int cpus = hal_cpu_count();
    ksnprintf(detail, sizeof(detail), "%d processor%s  -  %s", cpus, cpus == 1 ? "" : "s", hal_arch());
    card(ui, cell[0], SYM_CHIP, "Processor", hal_machine(), detail, -1);
    struct heap_stats hs;
    heap_get_stats(&hs);
    char used[16], total[16];
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(total, sizeof(total), hs.total);
    ksnprintf(value, sizeof(value), "%s", total);
    ksnprintf(detail, sizeof(detail), "%s in use", used);
    card(ui, cell[1], SYM_MEMORY, "Memory", value, detail, hs.total ? (int)(hs.used * 1000 / hs.total) : 0);
    uint64_t t_all = 0, f_all = 0;
    int volumes = 0;
    for (struct mount *mt = vfs_mounts(); mt; mt = mt->next) {
        uint64_t t = 0, f = 0;
        if (mt->fs->statfs && mt->fs->statfs(mt->root, &t, &f) == 0 && t && strcmp(mt->fs->name, "ramfs") &&
            strcmp(mt->fs->name, "devfs")) {
            t_all += t, f_all += f;
            volumes++;
        }
    }
    fmt_size(total, sizeof(total), t_all);
    fmt_size(used, sizeof(used), f_all);
    ksnprintf(detail, sizeof(detail), "%s free on %d volume%s", used, volumes, volumes == 1 ? "" : "s");
    card(ui, cell[2], SYM_DISK, "Storage", volumes ? total : "None", detail,
         t_all ? (int)((t_all - f_all) * 1000 / t_all) : -1);
    struct rect screen = wm_screen_rect(0);
    ksnprintf(value, sizeof(value), "%d x %d", screen.w, screen.h);
    card(ui, cell[3], SYM_DISPLAY, "Display", value, "the main display", -1);
    uint64_t up = hal_ticks_ms() / 1000;
    if (up >= 86400)
        ksnprintf(value, sizeof(value), "%ud %uh %um", (unsigned)(up / 86400), (unsigned)(up / 3600 % 24),
                  (unsigned)(up / 60 % 60));
    else
        ksnprintf(value, sizeof(value), "%uh %02um %02us", (unsigned)(up / 3600), (unsigned)(up / 60 % 60),
                  (unsigned)(up % 60));
    struct rtc_time t;
    time_local(&t);
    ksnprintf(detail, sizeof(detail), "%s, %d %s %d", time_weekday_name(time_weekday(&t)), t.day,
              time_month_name(t.month), t.year);
    card(ui, cell[4], SYM_CLOCK, "Up Time", value, detail, -1);
    struct window *list[64];
    int windows = wm_window_list(list, 64);
    ksnprintf(value, sizeof(value), "%d window%s open", windows, windows == 1 ? "" : "s");
    card(ui, cell[5], SYM_WINDOW, "Desktop", value, "Console shows the kernel log", -1);
    int by = y + 2 * (ch + gap) + 6;
    if (ui_button(ui, (struct rect){x, by, 130, 28}, "Open Console"))
        tool_launch("Console", NULL);
    if (ui_button(ui, (struct rect){x + 140, by, 150, 28}, "Activity Monitor"))
        tool_launch("System Monitor", NULL);
    if (ui_button(ui, (struct rect){x + 300, by, 120, 28}, "Disk Utility"))
        tool_launch("Disk Utility", NULL);
}

static void frame(struct ui *ui, void *data)
{
    struct sysinfo *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    struct tool_item items[PAGES];
    for (int i = 0; i < PAGES; i++)
        items[i] = (struct tool_item){pages[i].sym, pages[i].label, pages[i].header};
    if (tool_sidebar(ui, H, items, PAGES, &s->page)) {
        ui_text_set(&s->find, "");
        app_state_set_number("sysinfo", "page", s->page);
        reload(s);
    }
    const struct page *p = &pages[s->page];
    char sub[64] = "";
    if (p->command)
        ksnprintf(sub, sizeof(sub), "from the '%s' command", p->command);
    struct rect area = tool_page(ui, W, H, p->label, sub);
    if (ui->e && ui->e->type == EV_TIMER)
        ui->consumed = true;
    if (!p->command) {
        overview(ui, area);
        return;
    }
    if (ui_symbol_button(ui, (struct rect){W - 76, 12, 28, 28}, SYM_RESTART, false))
        reload(s);
    ui_search(ui, (struct rect){W - 266, 12, 180, 28}, &s->find, "Search");
    tool_text_view(ui, rect_inset(area, 12), &s->view, s->find.buf);
}

static void paint(struct window *w, struct canvas *c)
{
    struct sysinfo *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct sysinfo *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    struct sysinfo *s = w->data;
    if (open_win == w)
        open_win = NULL;
    kfree(s->view.text);
    kfree(s);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct sysinfo *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    s->page = (int)CLAMP(app_state_number("sysinfo", "page", 0), 0, PAGES - 1);
    for (int i = 0; arg && i < PAGES; i++)
        if (!str_casecmp(arg, pages[i].label) || (pages[i].command && !str_casecmp(arg, pages[i].command)))
            s->page = i;
    reload(s);
    open_win = tool_open("System Information", ICON_SYSINFO, 860, 580, &ops, s);
    if (!open_win) {
        kfree(s->view.text);
        kfree(s);
        return;
    }
    wm_set_timer(open_win, 1000);
}

static const struct gui_app sysinfo_app = {
    .name = "System Information", .description = "Hardware, storage, network and software of this computer",
    .icon = ICON_SYSINFO, .launch = launch};
GUI_APP(sysinfo_app);
