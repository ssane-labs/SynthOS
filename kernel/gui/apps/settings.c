#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/theme.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

#define SIDEBAR_W 220
#define ROW 40
#define SETTINGS_FILE "/home/.synth.conf"
#define WALLS 10
#define LOOK_W 132
#define LOOK_H 84
#define THUMB_W 132
#define THUMB_H 82

enum pane {
    P_APPEARANCE,
    P_DOCK,
    P_LOGIN,
    P_WALLPAPER,
    P_DISPLAYS,
    P_NETWORK,
    P_LOCK,
    P_USERS,
    P_TERMINAL,
    P_MOUSE,
    P_ABOUT,
    P_COUNT,
};

static const struct {
    const char *name;
    const char *key;
    enum symbol_id sym;
    uint32_t color;
} panes[P_COUNT] = {
    {"Looks", "appearance", SYM_APPEARANCE, RGB(232, 120, 70)},
    {"Desktop & Widgets", "dock", SYM_DOCK, RGB(44, 44, 48)},
    {"Login Items", "login", SYM_LIST, RGB(120, 110, 220)},
    {"Wallpaper", "wallpaper", SYM_PHOTO, RGB(36, 168, 220)},
    {"Displays", "displays", SYM_DISPLAY, RGB(28, 118, 240)},
    {"Network", "network", SYM_WIFI, RGB(28, 118, 240)},
    {"Lock Screen", "lock", SYM_LOCK, RGB(44, 44, 48)},
    {"Users & Groups", "users", SYM_USERS, RGB(28, 118, 240)},
    {"Terminal", "terminal", SYM_TERMINAL, RGB(60, 60, 64)},
    {"Mouse", "mouse", SYM_MOUSE, RGB(142, 142, 148)},
    {"About", "about", SYM_INFO, RGB(142, 142, 148)},
};

static const char *const wall_names[WALLS] = {"From the look", "Dune", "Mist", "Night", "Sunset",
                                              "Waves", "Hills", "Aurora", "Synthwave", "Solid"};
static const char *const wall_values[WALLS] = {"auto", "dune", "mist", "night", "sunset",
                                               "waves", "hills", "aurora", "synthwave", "solid"};

struct settings {
    struct ui_state st;
    int pane;
    int scroll;
    struct ui_text search;
    char themes[48][24];
    int theme_count;
    struct ui_list theme_list;
    struct ui_text wallpaper_file, wallpaper_url;
    struct canvas thumbs[WALLS];
    bool thumbs_dark;
    const struct gui_look *thumbs_look;
    struct canvas look_thumbs[8];
    struct ui_text login_item, city;
    int user_sel;
    char pending_user[USER_NAME_MAX];
};

static struct window *instance;

static void apply(const char *name, const char *value)
{
    setting_apply(shell_current(), name, value);
    settings_save(SETTINGS_FILE);
}

static void current(const char *name, char *buf, size_t cap)
{
    const struct setting *st = setting_find(name);
    buf[0] = 0;
    if (st && st->get)
        st->get(buf, cap);
}

static bool is_on(const char *name)
{
    char v[16];
    current(name, v, sizeof(v));
    return !strcmp(v, "on");
}

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

static void make_thumbs(struct settings *s)
{
    if (s->thumbs[0].px && s->thumbs_dark == style.dark && s->thumbs_look == look_current())
        return;
    for (int i = 0; i < WALLS; i++) {
        if (!s->thumbs[i].px && !canvas_alloc(&s->thumbs[i], THUMB_W, THUMB_H))
            continue;
        wallpaper_paint(&s->thumbs[i], i ? wall_values[i] : look_current()->name, style.dark);
    }
    s->thumbs_dark = style.dark;
    s->thumbs_look = look_current();
}

static void make_look_thumbs(struct settings *s)
{
    for (int i = 0; i < look_count() && i < 8; i++) {
        if (s->look_thumbs[i].px || !canvas_alloc(&s->look_thumbs[i], LOOK_W, LOOK_H))
            continue;
        const struct gui_look *l = look_get(i);
        wallpaper_paint(&s->look_thumbs[i], l->name, l->dark);
    }
}

static void draw_layout(struct canvas *c, struct rect r, const struct gui_look *l)
{
    uint32_t glass = l->dark ? RGBA(70, 64, 90, 200) : RGBA(255, 255, 255, 190);
    struct canvas k = *c;
    k.clip = rect_intersect(c->clip, r);
    int u = r.w / 44;
    struct rect win = {r.x + r.w / 4, r.y + r.h / 4, r.w / 2, r.h / 2};
    switch (l->dock) {
    case DOCK_CENTER:
        gfx_fill(&k, (struct rect){r.x, r.y, r.w, 4 * u}, glass);
        gfx_round(&k, (struct rect){r.x + r.w / 4, r.y + r.h - 9 * u, r.w / 2, 6 * u}, 3 * u, glass);
        break;
    case DOCK_RAIL:
        gfx_round(&k, (struct rect){r.x + 2 * u, r.y + 2 * u, 7 * u, r.h - 4 * u}, 3 * u, glass);
        win.x += 4 * u;
        break;
    case DOCK_CORNER:
        gfx_round(&k, (struct rect){r.x + 2 * u, r.y + 2 * u, 12 * u, r.h / 2}, 3 * u, glass);
        gfx_round(&k, (struct rect){r.x - 4 * u, r.y + r.h - 9 * u, 22 * u, 14 * u}, 4 * u, glass);
        win.x += 4 * u;
        break;
    }
    if (l->status == STATUS_PILL)
        gfx_round(&k, (struct rect){r.x + r.w - 15 * u, r.y + 2 * u, 13 * u, 3 * u}, (3 * u) / 2, glass);
    gfx_round(&k, win, 2 * u, l->dark ? RGB(32, 30, 40) : RGB(250, 250, 252));
    gfx_round(&k, (struct rect){win.x, win.y, win.w, 3 * u}, 2 * u, l->dark ? RGB(46, 44, 56) : RGB(236, 236, 240));
}

static struct rect control(struct rect row, int w, int h)
{
    return (struct rect){row.x + row.w - 14 - w, row.y + (row.h - h) / 2, w, h};
}

static struct rect row_at(struct rect group, int i)
{
    return (struct rect){group.x, group.y + i * ROW, group.w, ROW};
}

static int heading(struct ui *ui, int x, int y, int w, const char *text)
{
    ui_label(ui, (struct rect){x + 4, y, w, 20}, text, &font_bold, style.text, ALIGN_LEFT);
    return y + 26;
}

static void pane_appearance(struct ui *ui, struct settings *s, int x, int y, int w)
{
    char value[32];
    make_look_thumbs(s);
    y = heading(ui, x, y, w, "Look");
    int n = look_count(), gap = (w - n * LOOK_W) / MAX(n - 1, 1);
    gap = CLAMP(gap, 8, 24);
    for (int i = 0; i < n; i++) {
        const struct gui_look *l = look_get(i);
        struct rect t = {x + i * (LOOK_W + gap), y, LOOK_W, LOOK_H};
        bool sel = l == look_current();
        bool hot = ui_hover(ui, (struct rect){t.x, t.y, t.w, t.h + 40});
        if (ui->c && s->look_thumbs[i].px) {
            if (hot || sel)
                gfx_glow(ui->c, t, 12, 16, sel ? 120 : 60, l->accent);
            gfx_drop_shadow(ui->c, t, 12, 8, 60, 3);
            gfx_blit_round(ui->c, t, &s->look_thumbs[i], 12, false);
            draw_layout(ui->c, t, l);
            if (sel)
                gfx_round_frame(ui->c, rect_inset(t, -3), 15, style.accent);
            gfx_text_box(ui->c, (struct rect){t.x, t.y + t.h + 8, t.w, 18}, sel ? &font_bold : &font_sans, l->title,
                         style.text, ALIGN_CENTER);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
            ui->consumed = true;
            apply("look", l->name);
        }
    }
    y += LOOK_H + 34;
    ui_label(ui, (struct rect){x + 4, y, w, 18}, look_current()->description, &font_small, style.secondary, ALIGN_LEFT);
    y += 30;
    struct rect g = ui_group(ui, x, y, w, 3, ROW);
    struct rect r = row_at(g, 0);
    ui_group_label(ui, r, "Mode", NULL, false);
    current("appearance", value, sizeof(value));
    static const char *const modes[] = {"Auto", "Light", "Dark"};
    static const char *const mode_values[] = {"auto", "light", "dark"};
    int sel = !strcmp(value, "light") ? 1 : !strcmp(value, "dark") ? 2 : 0;
    if (ui_segmented(ui, control(r, 240, 24), modes, 3, &sel))
        apply("appearance", mode_values[sel]);
    r = row_at(g, 1);
    ui_group_label(ui, r, "Accent color", NULL, false);
    static const char *const accents[] = {"auto", "blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite"};
    static const uint32_t swatches[] = {0, RGB(0, 122, 255), RGB(149, 61, 150), RGB(247, 79, 158), RGB(224, 56, 62),
                                        RGB(247, 130, 27), RGB(230, 168, 0), RGB(98, 186, 70), RGB(140, 140, 140)};
    current("accent", value, sizeof(value));
    for (int i = 0; i < 9; i++) {
        struct rect dot = control(r, 9 * 24, 18);
        dot.x += i * 24;
        dot.w = 18;
        uint32_t col = i ? swatches[i] : look_current()->accent;
        if (ui->c) {
            gfx_circle(ui->c, dot.x + 9, dot.y + 9, 8, col);
            if (!i)
                gfx_ring(ui->c, dot.x + 9, dot.y + 9, 8, ALPHA(0xffffffffu, 200));
            if (!strcmp(value, accents[i]))
                gfx_circle(ui->c, dot.x + 9, dot.y + 9, 3, RGB(255, 255, 255));
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, dot)) {
            ui->consumed = true;
            apply("accent", accents[i]);
        }
    }
    r = row_at(g, 2);
    ui_group_label(ui, r, "Transparency", NULL, true);
    current("glass", value, sizeof(value));
    static const char *const glass[] = {"Liquid Glass", "Frosted", "Solid"};
    static const char *const glass_values[] = {"liquid", "frosted", "solid"};
    sel = !strcmp(value, "frosted") ? 1 : !strcmp(value, "solid") ? 2 : 0;
    if (ui_segmented(ui, control(r, 270, 24), glass, 3, &sel))
        apply("glass", glass_values[sel]);
}

static void switch_row(struct ui *ui, struct rect r, const char *label, const char *setting, bool last);

static bool widget_on(const char *name)
{
    char v[96];
    current("widgets", v, sizeof(v));
    return strstr(v, name) != NULL;
}

static void widget_toggle(const char *name, bool on)
{
    static const char *const names[] = {"clock", "weather", "music", "calendar", "visualizer", "tasks"};
    char out[96] = "";
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        bool want = !strcmp(names[i], name) ? on : widget_on(names[i]);
        if (want) {
            if (out[0])
                str_append(out, ",", sizeof(out));
            str_append(out, names[i], sizeof(out));
        }
    }
    apply("widgets", out[0] ? out : "none");
}

static void pane_login(struct ui *ui, struct settings *s, int x, int y, int w)
{
    static char items[AUTOSTART_MAX][AUTOSTART_LEN];
    int n = autostart_items(items, AUTOSTART_MAX);
    y = heading(ui, x, y, w, "Open when you log in");
    struct rect g = ui_group(ui, x, y, w, MAX(n, 1), ROW);
    if (!n)
        ui_group_label(ui, row_at(g, 0), "Nothing yet", NULL, true);
    for (int i = 0; i < n; i++) {
        struct rect r = row_at(g, i);
        const struct gui_app *app = gui_app_find(items[i]);
        bool is_app = app && !str_casecmp(app->name, items[i]);
        if (ui->c)
            gfx_icon(ui->c, r.x + 12, r.y + 8, is_app ? app->icon : items[i][0] == '!' ? ICON_TERMINAL : ICON_FILE, 24);
        ui_label(ui, (struct rect){r.x + 46, r.y, r.w - 120, r.h}, items[i][0] == '!' ? items[i] + 1 : items[i], &font_sans,
                 style.text, ALIGN_LEFT);
        if (i + 1 < n && ui->c)
            gfx_fill(ui->c, (struct rect){r.x + 46, r.y + r.h - 1, r.w - 46, 1}, style.separator);
        if (ui_symbol_button(ui, control(r, 26, 26), SYM_MINUS, false))
            autostart_remove(i);
    }
    y += MAX(n, 1) * ROW + 20;
    y = heading(ui, x, y, w, "Add");
    g = ui_group(ui, x, y, w, 1, ROW + 8);
    struct rect field = {g.x + 14, g.y + 10, g.w - 120, 28};
    bool go = ui_textbox_hint(ui, field, &s->login_item, "App name, file path or !command");
    if ((ui_button(ui, (struct rect){g.x + g.w - 96, g.y + 10, 82, 28}, "Add") || go) && s->login_item.len) {
        autostart_add(s->login_item.buf);
        ui_text_set(&s->login_item, "");
    }
    y += ROW + 8 + 14;
    static const char *const quick[] = {"Music", "Notepad", "Files", "Browser", "Photos"};
    for (size_t i = 0; i < ARRAY_SIZE(quick); i++) {
        struct rect b = {x + (int)i * 104, y, 96, 28};
        if (ui_button(ui, b, quick[i]))
            autostart_add(quick[i]);
    }
}

static void switch_row(struct ui *ui, struct rect r, const char *label, const char *setting, bool last)
{
    ui_group_label(ui, r, label, NULL, last);
    bool on = is_on(setting);
    if (ui_switch(ui, control(r, 36, 22), &on))
        apply(setting, on ? "on" : "off");
}

static void pane_dock(struct ui *ui, struct settings *s, int x, int y, int w)
{
    y = heading(ui, x, y, w, "Widgets");
    static const char *const names[] = {"clock", "weather", "music", "calendar", "visualizer"};
    static const char *const labels[] = {"Clock", "Weather", "Music", "Calendar and tasks", "Visualizer"};
    struct rect g = ui_group(ui, x, y, w, 6, ROW);
    for (int i = 0; i < 5; i++) {
        struct rect r = row_at(g, i);
        bool here = look_current()->widgets >> i & 1;
        ui_group_label(ui, r, labels[i], here ? NULL : "not in this look", false);
        bool on = widget_on(names[i]);
        if (ui_switch(ui, control(r, 36, 22), &on))
            widget_toggle(names[i], on);
    }
    struct rect r = row_at(g, 5);
    ui_group_label(ui, r, "Weather city", NULL, true);
    struct rect field = control(r, 200, 26);
    if (!s->city.len && !ui->st->focus) {
        char city[48];
        current("weathercity", city, sizeof(city));
        if (strcmp(city, "auto"))
            ui_text_set(&s->city, city);
    }
    if (ui_textbox_hint(ui, field, &s->city, "By address"))
        apply("weathercity", s->city.len ? s->city.buf : "auto");
    y += 6 * ROW + 20;
    y = heading(ui, x, y, w, look_current()->dock == DOCK_CENTER ? "Dock" : "Dock (in the Mist look)");
    g = ui_group(ui, x, y, w, 3, ROW);
    r = row_at(g, 0);
    ui_group_label(ui, r, "Size", NULL, false);
    char value[16];
    current("docksize", value, sizeof(value));
    static const char *const sizes[] = {"Small", "Medium", "Large"};
    static const char *const size_values[] = {"small", "medium", "large"};
    int sel = !strcmp(value, "small") ? 0 : !strcmp(value, "large") ? 2 : 1;
    if (ui_segmented(ui, control(r, 240, 24), sizes, 3, &sel))
        apply("docksize", size_values[sel]);
    switch_row(ui, row_at(g, 1), "Magnification", "magnify", false);
    switch_row(ui, row_at(g, 2), "Automatically hide and show the Dock", "dockhide", true);
    y += 3 * ROW + 20;
    y = heading(ui, x, y, w, "Desktop");
    g = ui_group(ui, x, y, w, 2, ROW);
    switch_row(ui, row_at(g, 0), "Start the desktop when SynthOS starts", "desktop", false);
    switch_row(ui, row_at(g, 1), "Animations", "animations", true);
}

static void pane_wallpaper(struct ui *ui, struct settings *s, int x, int y, int w)
{
    make_thumbs(s);
    char value[PATH_MAX_LEN];
    current("wallpaper", value, sizeof(value));
    struct rect g = ui_group(ui, x, y, w, 1, 128);
    const struct canvas *wp = desktop_wallpaper(0);
    struct rect big = {g.x + 16, g.y + 14, 160, 100};
    if (ui->c && wp) {
        struct canvas inner = *ui->c;
        gfx_drop_shadow(&inner, big, 8, 8, 60, 2);
        struct canvas shot;
        if (canvas_alloc(&shot, big.w, big.h)) {
            gfx_scale(&shot, (struct rect){0, 0, big.w, big.h}, wp, 255);
            gfx_blit_round(ui->c, big, &shot, 8, false);
            canvas_free(&shot);
        }
    }
    const char *shown = value;
    for (int i = 0; i < WALLS; i++)
        if (!strcmp(value, wall_values[i]))
            shown = wall_names[i];
    ui_label(ui, (struct rect){big.x + big.w + 20, big.y + 24, w - 220, 22}, shown, &font_title, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){big.x + big.w + 20, big.y + 50, w - 220, 18},
             style.dark ? "Dark variant shown" : "Light variant shown", &font_sans, style.secondary, ALIGN_LEFT);
    y += 128 + 20;
    y = heading(ui, x, y, w, "Wallpapers");
    int cols = MAX(1, (w + 16) / (THUMB_W + 16));
    for (int i = 0; i < WALLS; i++) {
        struct rect t = {x + (i % cols) * (THUMB_W + 16), y + (i / cols) * (THUMB_H + 34), THUMB_W, THUMB_H};
        bool sel = !strcmp(value, wall_values[i]);
        if (ui->c && s->thumbs[i].px) {
            gfx_drop_shadow(ui->c, t, 8, 6, 50, 2);
            gfx_blit_round(ui->c, t, &s->thumbs[i], 8, false);
            if (sel)
                gfx_round_frame(ui->c, rect_inset(t, -3), 11, style.accent);
            gfx_text_box(ui->c, (struct rect){t.x, t.y + t.h + 6, t.w, 18}, &font_sans, wall_names[i], style.text,
                         ALIGN_CENTER);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, t)) {
            ui->consumed = true;
            apply("wallpaper", wall_values[i]);
        }
    }
    y += ((WALLS + cols - 1) / cols) * (THUMB_H + 34) + 10;
    y = heading(ui, x, y, w, "From the Internet");
    g = ui_group(ui, x, y, w, 2, ROW + 8);
    struct rect net = {g.x, g.y, g.w, ROW + 8};
    if (ui->c)
        gfx_symbol(ui->c, net.x + 16, net.y + (net.h - 18) / 2, SYM_GLOBE, 18, style.accent);
    ui_label(ui, (struct rect){net.x + 44, net.y, net.w - 330, net.h}, "A new photo for the desktop", &font_sans,
             style.text, ALIGN_LEFT);
    if (ui_button(ui, (struct rect){net.x + net.w - 290, net.y + 10, 150, 28}, "Photo of the Day"))
        wallpaper_fetch("bing");
    if (ui_button(ui, (struct rect){net.x + net.w - 132, net.y + 10, 118, 28}, "Random Photo"))
        wallpaper_fetch("random");
    struct rect link = {g.x, g.y + ROW + 8, g.w, ROW + 8};
    struct rect url_field = {link.x + 14, link.y + 10, link.w - 140, 28};
    bool fetch_go = ui_textbox_hint(ui, url_field, &s->wallpaper_url, "https://example.com/picture.jpg");
    if ((ui_button(ui, (struct rect){link.x + link.w - 116, link.y + 10, 102, 28}, "Download") || fetch_go) &&
        s->wallpaper_url.buf[0])
        wallpaper_fetch(s->wallpaper_url.buf);
    y += 2 * (ROW + 8) + 20;
    y = heading(ui, x, y, w, "Picture");
    g = ui_group(ui, x, y, w, 1, ROW + 8);
    struct rect r = {g.x, g.y, g.w, ROW + 8};
    struct rect field = {r.x + 14, r.y + 10, r.w - 120, 28};
    bool go = ui_textbox_hint(ui, field, &s->wallpaper_file, "/home/Pictures/photo.jpg");
    if (ui_button(ui, (struct rect){r.x + r.w - 96, r.y + 10, 82, 28}, "Use") || go)
        apply("wallpaper", s->wallpaper_file.buf);
}

static int menu_display;
static struct display_mode menu_modes[24];
static char menu_labels[24][24];
static struct menu_item menu_items[24];

static void pick_mode(struct window *owner, int index)
{
    UNUSED(owner);
    char value[24];
    ksnprintf(value, sizeof(value), "%ux%u", menu_modes[index].width, menu_modes[index].height);
    apply(menu_display ? "resolution2" : "resolution", value);
}

static void pane_displays(struct ui *ui, struct settings *s, int x, int y, int w)
{
    UNUSED(s);
    int n = wm_screen_count();
    struct rect g = ui_group(ui, x, y, w, 1, 170);
    struct rect bounds = wm_screen_rect(0);
    for (int i = 1; i < n; i++)
        bounds = rect_union(bounds, wm_screen_rect(i));
    int scale = MAX(1, MAX(bounds.w / (w - 80) + 1, bounds.h / 120 + 1));
    int ox = g.x + (w - bounds.w / scale) / 2, oy = g.y + 16 + (120 - bounds.h / scale) / 2;
    for (int i = 0; i < n && ui->c; i++) {
        struct rect a = wm_screen_rect(i);
        struct rect r = {ox + (a.x - bounds.x) / scale + 2, oy + (a.y - bounds.y) / scale + 2, a.w / scale - 4,
                         a.h / scale - 4};
        const struct canvas *wp = desktop_wallpaper(i);
        if (wp) {
            struct canvas shot;
            if (canvas_alloc(&shot, MAX(r.w, 1), MAX(r.h, 1))) {
                gfx_scale(&shot, (struct rect){0, 0, r.w, r.h}, wp, 255);
                gfx_blit_round(ui->c, r, &shot, 5, false);
                canvas_free(&shot);
            }
        } else {
            gfx_round(ui->c, r, 5, style.control);
        }
        gfx_round_frame(ui->c, r, 5, i == 0 ? style.accent : ALPHA(0, 60));
        gfx_fill(ui->c, (struct rect){r.x + 4, r.y + 3, r.w - 8, 3}, ALPHA(0xffffffffu, 210));
        char label[8];
        ksnprintf(label, sizeof(label), "%d", i + 1);
        gfx_text_box(ui->c, r, &font_title, label, RGB(255, 255, 255), ALIGN_CENTER);
    }
    if (ui->c)
        gfx_text_box(ui->c, (struct rect){g.x, g.y + 146, g.w, 18}, &font_small,
                     n > 1 ? "Display 1 is the main display with the Dock" : "One display in use", style.secondary,
                     ALIGN_CENTER);
    y += 170 + 20;
    int available = hal_screen_count();
    if (available > 1) {
        g = ui_group(ui, x, y, w, 1, ROW);
        struct rect r = row_at(g, 0);
        ui_group_label(ui, r, "Second display", NULL, true);
        char value[16];
        current("arrange", value, sizeof(value));
        static const char *const labels[] = {"Right", "Left", "Above", "Below", "Off"};
        static const char *const values[] = {"right", "left", "above", "below", "off"};
        int sel = 0;
        for (int i = 0; i < 5; i++)
            if (!strcmp(value, values[i]))
                sel = i;
        if (ui_segmented(ui, control(r, 320, 24), labels, 5, &sel)) {
            apply("arrange", values[sel]);
            return;
        }
        y += ROW + 20;
    }
    for (int i = 0; i < n; i++) {
        int hal = gui_display_hal(i);
        char title[64], res[32], sc[16];
        ksnprintf(title, sizeof(title), "Display %d — %s", i + 1, hal_screen_name(hal));
        y = heading(ui, x, y, w, title);
        struct rect a = wm_screen_rect(i);
        int scale_factor = a.w >= 2400 ? 2 : 1;
        ksnprintf(res, sizeof(res), "%d × %d  ▾", a.w * scale_factor, a.h * scale_factor);
        str_copy(sc, scale_factor == 2 ? "200%" : "100%", sizeof(sc));
        g = ui_group(ui, x, y, w, 2, ROW);
        struct rect r = row_at(g, 0);
        ui_group_label(ui, r, "Resolution", NULL, false);
        if (ui_button(ui, control(r, 150, 26), res) && i < 2) {
            int count = hal_screen_modes(hal, menu_modes, 24);
            for (int k = 0; k < count; k++) {
                ksnprintf(menu_labels[k], sizeof(menu_labels[k]), "%u × %u", menu_modes[k].width, menu_modes[k].height);
                menu_items[k] = (struct menu_item){menu_labels[k], 0, NULL};
            }
            menu_display = i;
            if (count)
                ui_menu_items(ui->win, r.x + r.w - 164, r.y + r.h, menu_items, count, pick_mode);
        }
        ui_group_label(ui, row_at(g, 1), "Scale", sc, true);
        y += 2 * ROW + 20;
    }
}

static void pane_network(struct ui *ui, struct settings *s, int x, int y, int w)
{
    UNUSED(s);
    if (!net_interface_count()) {
        ui_label(ui, (struct rect){x, y, w, 20}, "No network hardware was found.", &font_sans, style.secondary,
                 ALIGN_LEFT);
        ui_label(ui, (struct rect){x, y + 22, w, 20}, "Supported: Intel e1000/e1000e, Realtek RTL8139/RTL8111.",
                 &font_small, style.secondary, ALIGN_LEFT);
        return;
    }
    for (int i = 0; i < net_interface_count(); i++) {
        struct net_iface *ifc = net_interface(i);
        struct netdev *d = ifc->dev;
        bool link = !d->link || d->link(d);
        char title[48], ip[16], mask[16], gw[16], dns[16], mac[24], status[48];
        ksnprintf(title, sizeof(title), "Ethernet (%s, %s)", d->name, d->driver);
        net_format_ip(ifc->ip, ip, sizeof(ip));
        net_format_ip(ifc->mask, mask, sizeof(mask));
        net_format_ip(ifc->gateway, gw, sizeof(gw));
        net_format_ip(ifc->dns, dns, sizeof(dns));
        ksnprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", d->mac[0], d->mac[1], d->mac[2], d->mac[3],
                  d->mac[4], d->mac[5]);
        str_copy(status, !link ? "Cable unplugged" : ifc->configured ? "Connected" : "Waiting for an address",
                 sizeof(status));
        ui_label(ui, (struct rect){x, y, w, 20}, title, &font_bold, style.text, ALIGN_LEFT);
        if (ui->c)
            gfx_circle(ui->c, x + w - 8, y + 10, 5,
                       ifc->configured && link ? RGB(40, 200, 64) : link ? RGB(254, 188, 46) : RGB(255, 95, 87));
        y += 26;
        const char *rows[][2] = {{"Status", status},   {"IP address", ifc->configured ? ip : "—"},
                                 {"Subnet mask", mask}, {"Router", gw},
                                 {"DNS server", dns},  {"Hardware address", mac},
                                 {"Configure", ifc->dhcp ? "Using DHCP" : "Manually"}};
        struct rect g = ui_group(ui, x, y, w, ARRAY_SIZE(rows), ROW);
        for (size_t k = 0; k < ARRAY_SIZE(rows); k++)
            ui_group_label(ui, row_at(g, (int)k), rows[k][0], rows[k][1], k + 1 == ARRAY_SIZE(rows));
        y += (int)ARRAY_SIZE(rows) * ROW + 12;
        if (ui_button(ui, (struct rect){x + w - 170, y, 170, 30}, "Renew DHCP Lease")) {
            net_dhcp_start(ifc);
            gui_notify(d->name, "Asking the network for a new address.");
        }
        y += 46;
    }
}

static void pane_lock(struct ui *ui, struct settings *s, int x, int y, int w)
{
    UNUSED(s);
    struct rect g = ui_group(ui, x, y, w, 3, ROW);
    struct rect r = row_at(g, 0);
    ui_group_label(ui, r, "Lock the screen when idle for", NULL, false);
    char value[16];
    current("locktime", value, sizeof(value));
    static const char *const labels[] = {"Never", "1 min", "5 min", "15 min", "1 hour"};
    static const char *const values[] = {"0", "1", "5", "15", "60"};
    int sel = 0;
    for (int i = 0; i < 5; i++)
        if (!strcmp(value, values[i]))
            sel = i;
    if (ui_segmented(ui, control(r, 300, 24), labels, 5, &sel))
        apply("locktime", values[sel]);
    const struct user *me = user_current();
    r = row_at(g, 1);
    ui_group_label(ui, r, "Log in automatically as", NULL, false);
    current("autologin", value, sizeof(value));
    bool automatic = !strcmp(value, me->name);
    if (ui_switch(ui, control(r, 36, 22), &automatic))
        apply("autologin", automatic ? me->name : "off");
    if (ui->c)
        gfx_text_box(ui->c, (struct rect){r.x + r.w - 260, r.y, 190, r.h}, &font_sans, me->full, style.secondary,
                     ALIGN_RIGHT);
    r = row_at(g, 2);
    ui_group_label(ui, r, "Password", me->has_password ? "Required to unlock" : "Not set — anyone can unlock", true);
    y += 3 * ROW + 20;
    if (ui_button_primary(ui, (struct rect){x + w - 160, y, 160, 30}, "Lock Screen Now")) {
        ui->win = NULL;
        gui_lock();
    }
}

static struct settings *dialog_settings(struct window *w)
{
    return wm_window_alive(w) ? w->data : NULL;
}

static void done_password(struct window *w, const char *value)
{
    struct settings *s = dialog_settings(w);
    if (!s || !s->pending_user[0])
        return;
    int err = user_set_password(s->pending_user, value);
    if (err)
        gui_message("The password could not be changed", err_str(err));
    else
        gui_notify("Password changed", *value ? "The new password is in effect." : "The password was removed.");
    s->pending_user[0] = 0;
    wm_invalidate(w);
}

static void done_new_user_password(struct window *w, const char *value)
{
    struct settings *s = dialog_settings(w);
    if (!s || !s->pending_user[0])
        return;
    int err = user_add(s->pending_user, s->pending_user, value, false);
    if (err)
        gui_message("The user could not be created",
                    err == E_EXIST ? "A user with this name already exists." : err == E_INVAL
                                                                                 ? "Use lowercase letters, digits, - and _."
                                                                                 : err_str(err));
    s->pending_user[0] = 0;
    wm_invalidate(w);
}

static void done_new_user(struct window *w, const char *value)
{
    struct settings *s = dialog_settings(w);
    if (!s || !*value)
        return;
    if (!user_name_valid(value)) {
        gui_message("Invalid user name", "Use lowercase letters, digits, - and _.");
        return;
    }
    str_copy(s->pending_user, value, sizeof(s->pending_user));
    ui_prompt_secret(w, "Password", "Choose a password for the new user (leave empty for none):",
                     done_new_user_password);
}

static void done_delete(struct window *w, const char *value)
{
    UNUSED(value);
    struct settings *s = dialog_settings(w);
    if (!s || !s->pending_user[0])
        return;
    int err = user_delete(s->pending_user);
    if (err)
        gui_message("The user could not be deleted", err == E_BUSY ? "You can't delete root or yourself." : err_str(err));
    s->pending_user[0] = 0;
    wm_invalidate(w);
}

static void pane_users(struct ui *ui, struct settings *s, int x, int y, int w)
{
    const struct user *me = user_current();
    int n = users_count();
    struct rect g = ui_group(ui, x, y, w, n, 54);
    for (int i = 0; i < n; i++) {
        const struct user *u = users_get(i);
        struct rect r = {g.x, g.y + i * 54, g.w, 54};
        if (ui->c) {
            gfx_icon(ui->c, r.x + 14, r.y + 9, ICON_USER, 36);
            char name[96];
            ksnprintf(name, sizeof(name), "%s%s", u->full, u == me ? "  (you)" : "");
            gfx_text_box(ui->c, (struct rect){r.x + 60, r.y + 9, r.w / 2, 18}, &font_bold, name, style.text, ALIGN_LEFT);
            char sub[96];
            ksnprintf(sub, sizeof(sub), "%s  ·  %s  ·  %s", u->name, u->admin ? "Admin" : "Standard",
                      u->has_password ? "Password set" : "No password");
            gfx_text_box(ui->c, (struct rect){r.x + 60, r.y + 28, r.w / 2, 16}, &font_small, sub, style.secondary,
                         ALIGN_LEFT);
            if (i < n - 1)
                gfx_fill(ui->c, (struct rect){r.x + 60, r.y + 53, r.w - 74, 1}, style.separator);
        }
        bool may = me->admin || u == me;
        if (may && ui_button(ui, (struct rect){r.x + r.w - (i && u != me ? 220 : 142), r.y + 13, 128, 28}, "Password…")) {
            str_copy(s->pending_user, u->name, sizeof(s->pending_user));
            ui_prompt_secret(ui->win, "Change Password", "Enter a new password (leave empty to remove it):",
                             done_password);
        }
        if (me->admin && i && u != me && ui_button(ui, (struct rect){r.x + r.w - 86, r.y + 13, 72, 28}, "Delete")) {
            char msg[128];
            ksnprintf(msg, sizeof(msg), "Delete the user \"%s\"? The home folder %s is kept.", u->name, u->home);
            str_copy(s->pending_user, u->name, sizeof(s->pending_user));
            ui_confirm(ui->win, "Delete User", msg, done_delete);
        }
    }
    y += n * 54 + 16;
    if (me->admin && ui_button(ui, (struct rect){x + w - 140, y, 140, 30}, "Add User…"))
        ui_prompt(ui->win, "New User", "Short name for the new account (lowercase):", "", done_new_user);
}

static void draw_theme(struct canvas *c, struct rect row, int index, bool sel, void *ctx)
{
    struct settings *s = ctx;
    struct theme t;
    if (theme_find(s->themes[index], &t) == 0) {
        for (int i = 0; i < 8; i++)
            gfx_fill(c, (struct rect){row.x + 20 + i * 12, row.y + 8, 12, row.h - 16}, 0xff000000u | t.palette[i + 8]);
        gfx_round_frame(c, (struct rect){row.x + 19, row.y + 7, 98, row.h - 14}, 3, ALPHA(0, 50));
    }
    gfx_text_box(c, (struct rect){row.x + 130, row.y, row.w - 140, row.h}, &font_sans, s->themes[index], ui_row_text(sel),
                 ALIGN_LEFT);
}

static void pane_terminal(struct ui *ui, struct settings *s, int x, int y, int w)
{
    y = heading(ui, x, y, w, "Color theme");
    struct rect g = ui_group(ui, x, y, w, 1, 6 * 32 + 12);
    s->theme_list.row_h = 32;
    int r = ui_list(ui, (struct rect){g.x, g.y + 6, g.w, 6 * 32}, &s->theme_list, draw_theme, s);
    if ((r == LIST_SELECT || r == LIST_ACTIVATE) && s->theme_list.selected >= 0)
        apply("theme", s->themes[s->theme_list.selected]);
    y += 6 * 32 + 12 + 20;
    g = ui_group(ui, x, y, w, 4, ROW);
    struct rect row = row_at(g, 0);
    ui_group_label(ui, row, "Cursor", NULL, false);
    char value[16];
    current("cursor", value, sizeof(value));
    static const char *const shapes[] = {"Block", "Underline", "Bar"};
    static const char *const shape_values[] = {"block", "underline", "bar"};
    int sel = !strcmp(value, "underline") ? 1 : !strcmp(value, "bar") ? 2 : 0;
    if (ui_segmented(ui, control(row, 240, 24), shapes, 3, &sel))
        apply("cursor", shape_values[sel]);
    row = row_at(g, 1);
    ui_group_label(ui, row, "Character set", NULL, false);
    static const char *const sets[] = {"437 Western", "866 Cyrillic"};
    int cp = codepage_current() == 866 ? 1 : 0;
    if (ui_segmented(ui, control(row, 240, 24), sets, 2, &cp))
        apply("codepage", cp ? "866" : "437");
    switch_row(ui, row_at(g, 2), "Status bar", "statusbar", false);
    switch_row(ui, row_at(g, 3), "Syntax highlighting while typing", "highlight", true);
}

static void pane_mouse(struct ui *ui, struct settings *s, int x, int y, int w)
{
    UNUSED(s);
    struct rect g = ui_group(ui, x, y, w, 2, ROW + 8);
    struct rect r = {g.x, g.y, g.w, ROW + 8};
    ui_group_label(ui, r, "Tracking speed", NULL, false);
    char value[16];
    current("mousespeed", value, sizeof(value));
    long speed = 5;
    str_parse_long(value, &speed);
    int v = (int)speed;
    if (ui_slider(ui, control(r, 220, 20), &v, 1, 10)) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", v);
        apply("mousespeed", buf);
    }
    r.y += ROW + 8;
    switch_row(ui, r, "Natural scrolling", "naturalscroll", true);
}

static void pane_about(struct ui *ui, struct settings *s, int x, int y, int w)
{
    UNUSED(s);
    if (ui->c)
        gfx_icon(ui->c, x + (w - 96) / 2, y, ICON_LOGO, 96);
    ui_label(ui, (struct rect){x, y + 104, w, 30}, "SynthOS", &font_large, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){x, y + 136, w, 18}, "Version " SYNTH_VERSION " \"" SYNTH_CODENAME "\"", &font_sans,
             style.secondary, ALIGN_CENTER);
    y += 172;
    struct heap_stats hs;
    heap_get_stats(&hs);
    char mem[48], used[16], total[16], screen[48];
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(total, sizeof(total), hs.total);
    ksnprintf(mem, sizeof(mem), "%s of %s used", used, total);
    struct rect a = wm_screen_rect(0);
    ksnprintf(screen, sizeof(screen), "%d × %d%s", a.w, a.h, wm_screen_count() > 1 ? " and more" : "");
    const char *rows[][2] = {{"Processor", hal_machine()}, {"Architecture", hal_arch()}, {"Memory", mem},
                             {"Display", screen},          {"User", user_current()->full}};
    struct rect g = ui_group(ui, x, y, w, ARRAY_SIZE(rows), ROW);
    for (size_t i = 0; i < ARRAY_SIZE(rows); i++)
        ui_group_label(ui, row_at(g, (int)i), rows[i][0], rows[i][1], i + 1 == ARRAY_SIZE(rows));
    y += (int)ARRAY_SIZE(rows) * ROW + 16;
    if (ui_button(ui, (struct rect){x + w - 150, y, 150, 30}, "System Report…"))
        gui_app_launch(gui_app_find("System Monitor"), NULL);
}

static bool matches(struct settings *s, int i)
{
    const char *q = s->search.buf;
    size_t n = strlen(q);
    if (!n)
        return true;
    for (const char *p = panes[i].name; *p; p++)
        if (!str_ncasecmp(p, q, n))
            return true;
    return false;
}

static void sidebar(struct ui *ui, struct settings *s, int H)
{
    ui_search(ui, (struct rect){18, 44, SIDEBAR_W - 30, 28}, &s->search, "Search");
    const struct user *me = user_current();
    struct rect card = {14, 82, SIDEBAR_W - 26, 52};
    if (ui->c) {
        if (s->pane == P_USERS)
            gfx_round(ui->c, card, 8, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 24));
        gfx_icon(ui->c, card.x + 6, card.y + 8, ICON_USER, 36);
        gfx_text_box(ui->c, (struct rect){card.x + 50, card.y + 8, card.w - 54, 18}, &font_bold, me->full, style.text,
                     ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){card.x + 50, card.y + 26, card.w - 54, 16}, &font_small, "Users & Groups",
                     style.secondary, ALIGN_LEFT);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, card)) {
        ui->consumed = true;
        s->pane = P_USERS;
        s->scroll = 0;
    }
    int y = 146;
    for (int i = 0; i < P_COUNT; i++) {
        if (!matches(s, i))
            continue;
        struct rect r = {14, y, SIDEBAR_W - 26, 28};
        if (y + 28 > H)
            break;
        bool clicked = false;
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, r)) {
            ui->consumed = true;
            clicked = true;
        }
        if (ui->c) {
            if (s->pane == i)
                gfx_round(ui->c, r, 7, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 24));
            ui_settings_icon(ui->c, r.x + 6, r.y + 4, 20, panes[i].sym, panes[i].color);
            gfx_text_box(ui->c, (struct rect){r.x + 34, r.y, r.w - 38, r.h}, &font_sans, panes[i].name, style.text,
                         ALIGN_LEFT);
        }
        if (clicked) {
            s->pane = i;
            s->scroll = 0;
        }
        y += 30;
        if (i == P_LOCK || i == P_TERMINAL)
            y += 8;
    }
}

static void frame(struct ui *ui, void *data)
{
    struct settings *s = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){SIDEBAR_W, 0, W - SIDEBAR_W, H}, style.window);
        ui_sidebar_panel(ui, SIDEBAR_W, H, style.window);
    }
    sidebar(ui, s, H);
    int cx = SIDEBAR_W + 1;
    ui_label(ui, (struct rect){cx + 20, 0, W - cx - 40, TOOLBAR_HEIGHT}, panes[s->pane].name, &font_bold, style.text,
             ALIGN_LEFT);
    struct rect content = {cx, TOOLBAR_HEIGHT, W - cx, H - TOOLBAR_HEIGHT};
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(content, ui->e->x, ui->e->y)) {
        s->scroll = MAX(0, s->scroll + ui->e->wheel * 40);
        ui->changed = true;
    }
    struct canvas *saved = ui->c, clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, content);
        ui->c = &clipped;
    }
    int x = cx + 24, y = TOOLBAR_HEIGHT + 8 - s->scroll, wdt = MIN(W - cx - 48, 620);
    switch (s->pane) {
    case P_APPEARANCE: pane_appearance(ui, s, x, y, wdt); break;
    case P_DOCK: pane_dock(ui, s, x, y, wdt); break;
    case P_LOGIN: pane_login(ui, s, x, y, wdt); break;
    case P_WALLPAPER: pane_wallpaper(ui, s, x, y, wdt); break;
    case P_DISPLAYS: pane_displays(ui, s, x, y, wdt); break;
    case P_NETWORK: pane_network(ui, s, x, y, wdt); break;
    case P_LOCK: pane_lock(ui, s, x, y, wdt); break;
    case P_USERS: pane_users(ui, s, x, y, wdt); break;
    case P_TERMINAL: pane_terminal(ui, s, x, y, wdt); break;
    case P_MOUSE: pane_mouse(ui, s, x, y, wdt); break;
    default: pane_about(ui, s, x, y, wdt); break;
    }
    if (ui->win && saved)
        ui->c = saved;
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

static void destroy(struct window *w)
{
    struct settings *s = w->data;
    for (int i = 0; i < WALLS; i++)
        canvas_free(&s->thumbs[i]);
    for (int i = 0; i < 8; i++)
        canvas_free(&s->look_thumbs[i]);
    if (instance == w)
        instance = NULL;
    kfree(s);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void select_pane(struct settings *s, const char *arg)
{
    if (!arg)
        return;
    for (int i = 0; i < P_COUNT; i++)
        if (!str_casecmp(arg, panes[i].key))
            s->pane = i;
}

static void launch(const char *arg)
{
    if (instance && wm_window_alive(instance)) {
        select_pane(instance->data, arg);
        wm_show(instance);
        wm_invalidate(instance);
        return;
    }
    struct settings *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    load_themes(s);
    select_pane(s, arg);
    char wp[PATH_MAX_LEN];
    current("wallpaper", wp, sizeof(wp));
    if (strchr(wp, '/'))
        ui_text_set(&s->wallpaper_file, wp);
    instance = wm_open("System Settings", ICON_SETTINGS, 820, 560, &ops, s,
                       WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!instance) {
        kfree(s);
        return;
    }
    instance->toolbar_h = TOOLBAR_HEIGHT;
    instance->min_w = 640;
    instance->min_h = 420;
}

static const struct gui_app settings_app = {"System Settings", "Appearance, wallpaper, users, displays and more",
                                            ICON_SETTINGS, launch, NULL, NULL};
GUI_APP(settings_app);
