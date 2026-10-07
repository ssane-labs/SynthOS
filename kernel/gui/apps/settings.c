#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/theme.h>
#include <synth/vfs.h>

#include "../internal.h"

#define NAV_W 170
#define SETTINGS_FILE "/home/.synth.conf"

enum section {
    SEC_APPEARANCE,
    SEC_DESKTOP,
    SEC_SYSTEM,
    SEC_COUNT,
};

static const char *const section_names[SEC_COUNT] = {"Appearance", "Desktop", "About"};
static const enum icon_id section_icons[SEC_COUNT] = {ICON_PAINT, ICON_COMPUTER, ICON_INFO};

struct settings {
    struct ui_state st;
    int section;
    char themes[48][24];
    int theme_count;
    struct ui_list theme_list;
    struct ui_text wallpaper_file;
    bool saved;
};

static void add_theme(struct settings *s, const char *name)
{
    for (int i = 0; i < s->theme_count; i++)
        if (!strcmp(s->themes[i], name))
            return;
    if (s->theme_count < (int)ARRAY_SIZE(s->themes))
        str_copy(s->themes[s->theme_count++], name, sizeof(s->themes[0]));
}

static void scan_themes(struct settings *s, const char *dir)
{
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        const char *ext = path_ext(ent.name);
        if (ext && !strcmp(ext, ".theme")) {
            char name[24];
            size_t n = MIN((size_t)(ext - ent.name), sizeof(name) - 1);
            memcpy(name, ent.name, n);
            name[n] = 0;
            add_theme(s, name);
        }
    }
    vfs_closedir(&d);
}

static void load_themes(struct settings *s)
{
    s->theme_count = 0;
    for (size_t i = 0; i < theme_builtin_count(); i++)
        add_theme(s, theme_builtin(i)->name);
    scan_themes(s, "/etc/themes");
    scan_themes(s, "/home/themes");
    s->theme_list.count = s->theme_count;
    s->theme_list.selected = -1;
    for (int i = 0; i < s->theme_count; i++)
        if (!strcmp(s->themes[i], theme_current()->name))
            s->theme_list.selected = i;
}

static void draw_theme(struct canvas *c, struct rect row, int index, bool sel, void *ctx)
{
    UNUSED(sel);
    struct settings *s = ctx;
    struct theme t;
    if (theme_find(s->themes[index], &t) == 0) {
        for (int i = 0; i < 8; i++)
            gfx_fill(c, (struct rect){row.x + 10 + i * 12, row.y + 8, 12, row.h - 16}, 0xff000000u | t.palette[i + 8]);
        gfx_round_frame(c, (struct rect){row.x + 9, row.y + 7, 98, row.h - 14}, 3, ALPHA(0xffffffffu, 60));
    }
    gfx_text_box(c, (struct rect){row.x + 120, row.y, row.w - 130, row.h}, &font_bold, s->themes[index], style.text,
                 ALIGN_LEFT);
}

static void apply(const char *name, const char *value)
{
    struct shell *sh = shell_current();
    setting_apply(sh, name, value);
}

static void current(const char *name, char *buf, size_t cap)
{
    const struct setting *st = setting_find(name);
    buf[0] = 0;
    if (st && st->get)
        st->get(buf, cap);
}

static int heading(struct ui *ui, int x, int y, const char *text)
{
    ui_label(ui, (struct rect){x, y, 400, 26}, text, &font_title, style.text, ALIGN_LEFT);
    return y + 34;
}

static int subheading(struct ui *ui, int x, int y, const char *text)
{
    ui_label(ui, (struct rect){x, y, 400, 20}, text, &font_bold, style.muted, ALIGN_LEFT);
    return y + 24;
}

static void appearance(struct ui *ui, struct settings *s, struct rect area)
{
    int x = area.x + 20, y = heading(ui, x, area.y + 16, "Appearance");
    y = subheading(ui, x, y, "Color theme (also colors the terminal)");
    s->theme_list.row_h = 34;
    struct rect list = {x, y, MIN(area.w - 40, 420), MIN(34 * 6, area.h - (y - area.y) - 150)};
    int r = ui_list(ui, list, &s->theme_list, draw_theme, s);
    if ((r == LIST_SELECT || r == LIST_ACTIVATE) && s->theme_list.selected >= 0) {
        apply("theme", s->themes[s->theme_list.selected]);
        s->saved = false;
    }
    y = list.y + list.h + 16;
    y = subheading(ui, x, y, "Wallpaper");
    char wp[PATH_MAX_LEN];
    current("wallpaper", wp, sizeof(wp));
    static const char *const kinds[] = {"synthwave", "aurora", "solid"};
    static const char *const labels[] = {"Synthwave", "Aurora", "Solid color"};
    bool custom = true;
    for (int i = 0; i < 3; i++) {
        bool on = !strcmp(wp, kinds[i]);
        custom &= !on;
        if (ui_radio(ui, (struct rect){x + i * 130, y, 124, 24}, labels[i], on)) {
            apply("wallpaper", kinds[i]);
            s->saved = false;
        }
    }
    y += 32;
    if (ui_radio(ui, (struct rect){x, y, 124, 30}, "Picture", custom) ||
        ui_textbox(ui, (struct rect){x + 130, y, MIN(area.w - 190, 290), 30}, &s->wallpaper_file)) {
        apply("wallpaper", s->wallpaper_file.buf);
        s->saved = false;
    }
}

static void desktop_section(struct ui *ui, struct settings *s, struct rect area)
{
    int x = area.x + 20, y = heading(ui, x, area.y + 16, "Desktop and terminal");
    char value[32];
    current("desktop", value, sizeof(value));
    bool on = !strcmp(value, "on");
    if (ui_checkbox(ui, (struct rect){x, y, 380, 28}, "Start the desktop when SynthOS boots", &on)) {
        apply("desktop", on ? "on" : "off");
        s->saved = false;
    }
    y += 34;
    current("statusbar", value, sizeof(value));
    on = !strcmp(value, "on");
    if (ui_checkbox(ui, (struct rect){x, y, 380, 28}, "Status bar in the terminal", &on)) {
        apply("statusbar", on ? "on" : "off");
        s->saved = false;
    }
    y += 34;
    current("highlight", value, sizeof(value));
    on = !strcmp(value, "on");
    if (ui_checkbox(ui, (struct rect){x, y, 380, 28}, "Syntax highlighting while typing", &on)) {
        apply("highlight", on ? "on" : "off");
        s->saved = false;
    }
    y += 44;
    y = subheading(ui, x, y, "Character set");
    int cp = codepage_current();
    if (ui_radio(ui, (struct rect){x, y, 200, 24}, "437  (Western)", cp == 437)) {
        apply("codepage", "437");
        s->saved = false;
    }
    if (ui_radio(ui, (struct rect){x + 210, y, 200, 24}, "866  (Cyrillic)", cp == 866)) {
        apply("codepage", "866");
        s->saved = false;
    }
    y += 40;
    y = subheading(ui, x, y, "Cursor");
    current("cursor", value, sizeof(value));
    static const char *const shapes[] = {"block", "underline", "bar"};
    for (int i = 0; i < 3; i++)
        if (ui_radio(ui, (struct rect){x + i * 130, y, 124, 24}, shapes[i], !strcmp(value, shapes[i]))) {
            apply("cursor", shapes[i]);
            s->saved = false;
        }
}

static void about(struct ui *ui, struct rect area)
{
    int x = area.x + 20, y = area.y + 16;
    if (ui->c)
        gfx_icon(ui->c, x, y, ICON_LOGO, 32);
    ui_label(ui, (struct rect){x + 44, y - 2, 300, 26}, "SynthOS", &font_title, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){x + 44, y + 20, 300, 18}, "Version " SYNTH_VERSION " \"" SYNTH_CODENAME "\"",
             &font_sans, style.muted, ALIGN_LEFT);
    y += 60;
    struct heap_stats hs;
    heap_get_stats(&hs);
    char mem[32], used[16], total[16], screen[32];
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(total, sizeof(total), hs.total);
    ksnprintf(mem, sizeof(mem), "%s of %s used", used, total);
    int sw, sh;
    wm_screen_size(&sw, &sh);
    ksnprintf(screen, sizeof(screen), "%d x %d", sw, sh);
    const char *rows[][2] = {{"Processor", hal_machine()}, {"Architecture", hal_arch()}, {"Memory", mem},
                              {"Screen", screen}, {"Theme", theme_current()->name}};
    for (size_t i = 0; i < ARRAY_SIZE(rows); i++) {
        ui_label(ui, (struct rect){x, y, 120, 24}, rows[i][0], &font_bold, style.muted, ALIGN_LEFT);
        ui_label(ui, (struct rect){x + 130, y, area.w - 170, 24}, rows[i][1], &font_sans, style.text, ALIGN_LEFT);
        y += 26;
    }
    y += 16;
    if (ui->c)
        ui_wrap_text(ui->c, (struct rect){x, y, area.w - 40, 200}, &font_sans,
                     "SynthOS is written from scratch: its own boot loaders for BIOS and UEFI, kernel, drivers, "
                     "file systems, shell, desktop, a virtual machine for SX programs, an MS-DOS emulator and a "
                     "Windows program loader.",
                     style.muted);
}

static void frame(struct ui *ui, void *data)
{
    struct settings *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    ui_panel(ui, (struct rect){0, 0, W, H}, style.window);
    ui_panel(ui, (struct rect){0, 0, NAV_W, H}, style.panel);
    for (int i = 0; i < SEC_COUNT; i++)
        if (ui_tool(ui, (struct rect){8, 12 + i * 36, NAV_W - 16, 32}, section_icons[i], section_names[i], s->section == i))
            s->section = i;
    if (ui_button(ui, (struct rect){12, H - 48, NAV_W - 24, 34}, s->saved ? "Saved" : "Save settings")) {
        s->saved = settings_save(SETTINGS_FILE) == 0;
        if (!s->saved)
            gui_message("Settings", "Could not save the settings to " SETTINGS_FILE ".");
    }
    struct rect area = {NAV_W, 0, W - NAV_W, H};
    if (s->section == SEC_APPEARANCE)
        appearance(ui, s, area);
    else if (s->section == SEC_DESKTOP)
        desktop_section(ui, s, area);
    else
        about(ui, area);
}

static void paint(struct window *w, struct canvas *c)
{
    struct settings *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct settings *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static struct window *instance;

static void destroy(struct window *w)
{
    if (instance == w)
        instance = NULL;
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (instance && wm_window_alive(instance)) {
        wm_show(instance);
        return;
    }
    struct settings *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    load_themes(s);
    char wp[PATH_MAX_LEN];
    current("wallpaper", wp, sizeof(wp));
    ui_text_set(&s->wallpaper_file, strchr(wp, '/') ? wp : "/home/wallpaper.bmp");
    s->saved = true;
    instance = wm_open("Settings", ICON_SETTINGS, 660, 520, &ops, s, WIN_RESIZABLE);
    if (!instance)
        kfree(s);
}

static const struct gui_app settings_app = {"Settings", "Themes, wallpaper and system information", ICON_SETTINGS,
                                            launch, NULL};
GUI_APP(settings_app);
