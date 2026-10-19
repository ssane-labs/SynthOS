#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/keys.h>
#include <synth/string.h>
#include <synth/time.h>

#include "internal.h"

#define MAX_TITLES 12
#define PAD 10

struct title {
    struct rect r;
    const char *label;
    int kind;
};

enum title_kind {
    T_LOGO,
    T_APP,
    T_CUSTOM,
    T_WINDOW,
    T_HELP,
};

enum status_part {
    S_CONTROL,
    S_SEARCH,
    S_CLOCK,
    S_COUNT,
};

static struct glass bars[MAX_SCREENS];
static struct title titles[MAX_TITLES];
static int title_count;
static struct rect status[S_COUNT];
static int open_title = -1, hover_title = -1;
static struct window *open_menu, *menu_target;
static char app_name[64];
static bool started;

static void clock_text(char *buf, size_t cap)
{
    gui_clock_text(buf, cap, true);
}

static struct rect bar_rect(int screen)
{
    struct rect s = wm_screen_rect(screen);
    return (struct rect){s.x, s.y, s.w, MENUBAR_HEIGHT};
}

static void layout(void)
{
    struct window *f = wm_focused();
    while (f && (f->flags & WIN_DIALOG) && f->owner)
        f = f->owner;
    menu_target = f;
    const char *name = f && f->app ? f->app->name : "Files";
    str_copy(app_name, name, sizeof(app_name));
    struct rect bar = bar_rect(0);
    int x = bar.x + 8;
    title_count = 0;
    titles[title_count++] = (struct title){{x, bar.y, 34, MENUBAR_HEIGHT}, NULL, T_LOGO};
    x += 34;
    int w = gfx_text_width(&font_bold, app_name) + 2 * PAD;
    titles[title_count++] = (struct title){{x, bar.y, w, MENUBAR_HEIGHT}, app_name, T_APP};
    x += w;
    if (f && f->menus) {
        for (int i = 0; i < f->menu_count && title_count < MAX_TITLES - 2; i++) {
            w = gfx_text_width(&font_sans, f->menus[i].title) + 2 * PAD;
            titles[title_count++] = (struct title){{x, bar.y, w, MENUBAR_HEIGHT}, f->menus[i].title, T_CUSTOM + i * 16};
            x += w;
        }
    }
    static const char *const fixed[2] = {"Window", "Help"};
    for (int i = 0; i < 2; i++) {
        w = gfx_text_width(&font_sans, fixed[i]) + 2 * PAD;
        titles[title_count++] = (struct title){{x, bar.y, w, MENUBAR_HEIGHT}, fixed[i], i ? T_HELP : T_WINDOW};
        x += w;
    }
    char clock[48];
    clock_text(clock, sizeof(clock));
    int right = bar.x + bar.w - 10;
    w = gfx_text_width(&font_sans, clock) + 2 * PAD;
    status[S_CLOCK] = (struct rect){right - w, bar.y, w, MENUBAR_HEIGHT};
    right -= w;
    status[S_SEARCH] = (struct rect){right - 32, bar.y, 32, MENUBAR_HEIGHT};
    right -= 32;
    status[S_CONTROL] = (struct rect){right - 32, bar.y, 32, MENUBAR_HEIGHT};
}

static void damage_bars(void)
{
    for (int i = 0; i < wm_screen_count(); i++)
        wm_damage_z(bar_rect(i), Z_MENUBAR);
}

void menubar_paint(struct canvas *c, int screen)
{
    if (!started)
        return;
    panel_paint(c, screen);
    if (!menubar_height())
        return;
    struct rect bar = bar_rect(screen);
    if (rect_empty(rect_intersect(bar, c->clip)))
        return;
    uint32_t ink = style.text;
    if (style.glass == GLASS_LIQUID) {
        static uint32_t inks[MAX_SCREENS];
        struct rect probe = rect_intersect(bar, c->clip);
        if (probe.w >= bar.w / 2 || !inks[screen]) {
            unsigned sum = 0, n = 0;
            for (int y = probe.y; y < probe.y + probe.h; y += 4)
                for (int x = probe.x; x < probe.x + probe.w; x += 12) {
                    uint32_t p = c->px[y * c->stride + x];
                    sum += ((p >> 16 & 0xff) * 77 + (p >> 8 & 0xff) * 151 + (p & 0xff) * 28) >> 8;
                    n++;
                }
            inks[screen] = n && sum / n < 120 ? RGB(250, 250, 252) : RGB(24, 24, 28);
        }
        ink = inks[screen];
        bool light_ink = ink == RGB(250, 250, 252);
        gfx_gradient(c, bar, light_ink ? ALPHA(0, 40) : ALPHA(0xffffffffu, 70), ALPHA(0xffffffffu, 0));
    } else {
        struct glass_look look;
        glass_look_for(&look, GLASS_MENUBAR, 0);
        glass_draw(c, &bars[screen], &look);
        gfx_fill(c, (struct rect){bar.x, bar.y + bar.h - 1, bar.w, 1}, style.dark ? ALPHA(0, 120) : ALPHA(0, 22));
    }
    struct rect area = bar;
    for (int i = 0; i < title_count; i++) {
        struct rect r = titles[i].r;
        r.x += bar.x - bar_rect(0).x;
        if (i == open_title && screen == 0)
            gfx_round(c, (struct rect){r.x, r.y + 2, r.w, r.h - 4}, (r.h - 4) / 2,
                      ink == RGB(250, 250, 252) ? ALPHA(0xffffffffu, 60) : ALPHA(0, 30));
        if (titles[i].kind == T_LOGO)
            gfx_symbol(c, r.x + (r.w - 16) / 2, r.y + 4, SYM_LOGO, 16, ink);
        else
            gfx_text_box(c, r, titles[i].kind == T_APP ? &font_bold : &font_sans, titles[i].label, ink, ALIGN_CENTER);
    }
    (void)area;
    char clock[48];
    clock_text(clock, sizeof(clock));
    int dx = bar.x + bar.w - (bar_rect(0).x + bar_rect(0).w);
    struct rect clock_r = {status[S_CLOCK].x + dx, status[S_CLOCK].y, status[S_CLOCK].w, status[S_CLOCK].h};
    gfx_text_box(c, clock_r, &font_sans, clock, ink, ALIGN_CENTER);
    gfx_symbol(c, status[S_SEARCH].x + dx + 8, bar.y + 4, SYM_SEARCH, 16, ink);
    gfx_symbol(c, status[S_CONTROL].x + dx + 8, bar.y + 4, SYM_CONTROL, 16, ink);
}

static const struct menu_item logo_items[] = {
    {"About SynthOS", 0, NULL},     {"", 0, NULL},           {"System Settings…", 0, "⌘,"},
    {"Force Quit…", 0, NULL},       {"", 0, NULL},           {"Restart…", 0, NULL},
    {"Shut Down…", 0, NULL},        {"", 0, NULL},           {"Lock Screen", 0, "⌘L"},
    {"Log Out…", 0, NULL},          {"Leave to Text Mode", 0, NULL},
};

static void about_box(void)
{
    gui_message("SynthOS " SYNTH_VERSION,
                "\"" SYNTH_CODENAME "\" - an operating system written from scratch: own boot loaders, kernel, "
                "drivers, file systems, shell and desktop.");
}

static void confirm_quit(struct window *owner, const char *value);
static int pending_quit;
static void pick_logo(struct window *owner, int index);

void shell_system_menu(int x, int y)
{
    wm_close_popups();
    ui_menu_items(NULL, x, y, logo_items, ARRAY_SIZE(logo_items), pick_logo);
}

static void ask_quit(int action, const char *title, const char *text)
{
    pending_quit = action;
    ui_confirm(NULL, title, text, confirm_quit);
}

static void confirm_quit(struct window *owner, const char *value)
{
    UNUSED(owner);
    UNUSED(value);
    gui_quit(pending_quit);
}

void shell_power_menu(bool restart)
{
    if (restart)
        ask_quit(QUIT_REBOOT, "Restart", "Are you sure you want to restart your computer now?");
    else
        ask_quit(QUIT_POWEROFF, "Shut Down", "Are you sure you want to shut down your computer now?");
}

static void pick_logo(struct window *owner, int index)
{
    UNUSED(owner);
    switch (index) {
    case 0: about_box(); break;
    case 2: gui_app_launch(gui_app_find("System Settings"), NULL); break;
    case 3: gui_app_launch(gui_app_find("System Monitor"), "windows"); break;
    case 5: ask_quit(QUIT_REBOOT, "Restart", "Are you sure you want to restart your computer now?"); break;
    case 6: ask_quit(QUIT_POWEROFF, "Shut Down", "Are you sure you want to shut down your computer now?"); break;
    case 8: gui_lock(); break;
    case 9: ask_quit(QUIT_LOGOUT, "Log Out", "Are you sure you want to quit all applications and log out now?"); break;
    case 10: gui_quit(QUIT_TEXT); break;
    }
}

static struct menu_item app_items[9];
static char app_labels[3][80];

static void pick_app(struct window *owner, int index)
{
    UNUSED(owner);
    struct window *f = menu_target && wm_window_alive(menu_target) ? menu_target : NULL;
    const struct gui_app *app = f ? f->app : NULL;
    if (index == 0)
        gui_message(app_name, app && app->description ? app->description : "SynthOS desktop");
    else if (index == 2)
        gui_app_launch(gui_app_find("System Settings"), NULL);
    else if (index == 4 && f) {
        for (struct window *w = app ? wm_app_window(app) : f; w && !(w->flags & WIN_MINIMIZED);
             w = app ? wm_app_window(app) : NULL) {
            wm_minimize(w);
            if (!app)
                break;
        }
    } else if (index == 5)
        wm_bring_all();
    else if (index == 7 && f) {
        if (app)
            for (struct window *w = wm_app_window(app); w; w = wm_app_window(app)) {
                wm_close(w);
                if (wm_window_alive(w))
                    break;
            }
        else
            wm_close(f);
    }
}

static struct menu_item window_items[16];
static struct window *window_targets[16];
static char window_labels[16][72];

static void pick_window(struct window *owner, int index)
{
    UNUSED(owner);
    struct window *f = menu_target && wm_window_alive(menu_target) ? menu_target : NULL;
    if (index == 0 && f)
        wm_minimize(f);
    else if (index == 1 && f)
        wm_toggle_maximize(f);
    else if (index == 3)
        wm_bring_all();
    else if (index >= 5 && window_targets[index] && wm_window_alive(window_targets[index]))
        wm_show(window_targets[index]);
}

static const struct menu_item help_items[] = {
    {"SynthOS Help", 0, NULL},
    {"Keyboard Shortcuts", 0, NULL},
};

static void pick_help(struct window *owner, int index)
{
    UNUSED(owner);
    if (index == 0)
        gui_app_launch(gui_app_find("Notepad"), "/docs/desktop.txt");
    else
        gui_message("Keyboard Shortcuts",
                    "Win: Launchpad.  Win+Space or Ctrl+Space: Spotlight.  Win+Tab or Alt+Tab: next window.  "
                    "Win+Q: quit app.  Win+W: close window.  Win+M: minimize.  Win+L: lock screen.  "
                    "Win+C, V, X, S, O, N, Z work like Ctrl in every app.");
}

static int custom_index;

static void pick_custom(struct window *owner, int index)
{
    UNUSED(owner);
    struct window *f = menu_target && wm_window_alive(menu_target) ? menu_target : NULL;
    if (!f || !f->menus || custom_index >= f->menu_count || index >= f->menus[custom_index].count)
        return;
    int key = f->menus[custom_index].items[index].key;
    if (key > 0) {
        wm_focus(f);
        wm_key(key);
    }
}

static void open_title_menu(int i)
{
    wm_close_popups();
    open_menu = NULL;
    open_title = i;
    struct rect r = titles[i].r;
    int x = r.x, y = r.y + MENUBAR_HEIGHT + 2;
    switch (titles[i].kind) {
    case T_LOGO:
        open_menu = ui_menu_items(NULL, x, y, logo_items, ARRAY_SIZE(logo_items), pick_logo);
        break;
    case T_APP: {
        ksnprintf(app_labels[0], sizeof(app_labels[0]), "About %s", app_name);
        ksnprintf(app_labels[1], sizeof(app_labels[1]), "Hide %s", app_name);
        ksnprintf(app_labels[2], sizeof(app_labels[2]), "Quit %s", app_name);
        struct menu_item items[] = {{app_labels[0], 0, NULL}, {"", 0, NULL}, {"Settings…", 0, "⌘,"}, {"", 0, NULL},
                                    {app_labels[1], 0, "⌘H"}, {"Show All", 0, NULL}, {"", 0, NULL},
                                    {app_labels[2], 0, "⌘Q"}};
        memcpy(app_items, items, sizeof(items));
        open_menu = ui_menu_items(NULL, x, y, app_items, ARRAY_SIZE(items), pick_app);
        break;
    }
    case T_WINDOW: {
        int n = 0;
        window_items[n++] = (struct menu_item){"Minimize", menu_target ? 0 : -1, "⌘M"};
        window_items[n++] = (struct menu_item){"Zoom", menu_target ? 0 : -1, NULL};
        window_items[n++] = (struct menu_item){"", 0, NULL};
        window_items[n++] = (struct menu_item){"Bring All to Front", 0, NULL};
        window_items[n++] = (struct menu_item){"", 0, NULL};
        struct window *list[16];
        int count = wm_window_list(list, 11);
        for (int k = 0; k < count && n < 16; k++, n++) {
            ksnprintf(window_labels[n], sizeof(window_labels[n]), "%s%s", list[k] == menu_target ? "✓ " : "    ",
                      list[k]->title);
            window_targets[n] = list[k];
            window_items[n] = (struct menu_item){window_labels[n], 0, NULL};
        }
        if (!count)
            n--;
        open_menu = ui_menu_items(NULL, x, y, window_items, n, pick_window);
        break;
    }
    case T_HELP:
        open_menu = ui_menu_items(NULL, x, y, help_items, ARRAY_SIZE(help_items), pick_help);
        break;
    default:
        if (menu_target && menu_target->menus) {
            custom_index = (titles[i].kind - T_CUSTOM) / 16;
            const struct app_menu *m = &menu_target->menus[custom_index];
            open_menu = ui_menu_items(NULL, x, y, m->items, m->count, pick_custom);
        }
        break;
    }
    damage_bars();
}

void menubar_open_app_menu(void)
{
    layout();
    open_title_menu(0);
}

static int title_at(int x, int y)
{
    for (int i = 0; i < title_count; i++)
        if (rect_contains(titles[i].r, x, y))
            return i;
    return -1;
}

static bool menu_open(void)
{
    if (open_menu && !wm_window_alive(open_menu)) {
        open_menu = NULL;
        open_title = -1;
        damage_bars();
    }
    return open_menu != NULL;
}

static bool in_bar(int x, int y)
{
    if (!menubar_height())
        return false;
    for (int i = 0; i < wm_screen_count(); i++)
        if (rect_contains(bar_rect(i), x, y))
            return true;
    return false;
}

bool chrome_hit(int x, int y)
{
    if (!started)
        return false;
    if (in_bar(x, y) || panel_hit(x, y))
        return true;
    return dock_hit(x, y);
}

static bool panel_drag;

void chrome_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    if (down)
        panel_drag = panel_hit(x, y);
    if (panel_drag) {
        panel_mouse(x, y, button, down);
        return;
    }
    if (!in_bar(x, y) || (!down && dock_dragging())) {
        if (down)
            wm_close_popups();
        dock_mouse(x, y, button, down, clicks);
        return;
    }
    if (!down)
        return;
    struct rect main = bar_rect(0);
    int mx = x - wm_screen_rect(wm_screen_at(x, y)).x + main.x;
    int t = title_at(mx, y - wm_screen_rect(wm_screen_at(x, y)).y + main.y);
    if (t >= 0) {
        if (menu_open() && open_title == t) {
            wm_close_popups();
            open_menu = NULL;
            open_title = -1;
            damage_bars();
        } else {
            open_title_menu(t);
        }
        return;
    }
    wm_close_popups();
    open_menu = NULL;
    open_title = -1;
    struct rect s = wm_screen_rect(wm_screen_at(x, y));
    int dx = s.x + s.w - (main.x + main.w);
    if (rect_contains((struct rect){status[S_SEARCH].x + dx, s.y, status[S_SEARCH].w, MENUBAR_HEIGHT}, x, y))
        spotlight_toggle();
    else if (rect_contains((struct rect){status[S_CONTROL].x + dx, s.y, status[S_CONTROL].w, MENUBAR_HEIGHT}, x, y))
        control_center_toggle(status[S_CONTROL].x + dx + status[S_CONTROL].w);
    damage_bars();
}

void chrome_hover(int x, int y)
{
    if (!started)
        return;
    dock_hover(x, y);
    panel_hover(x, y);
    if (y < 0 || !in_bar(x, y))
        return;
    struct rect s = wm_screen_rect(wm_screen_at(x, y));
    int t = title_at(x - s.x + bar_rect(0).x, y - s.y);
    if (t != hover_title)
        hover_title = t;
    if (t >= 0 && menu_open() && t != open_title)
        open_title_menu(t);
}

void chrome_changed(void)
{
    if (!started)
        return;
    menu_open();
    layout();
    damage_bars();
    dock_changed();
    panel_changed();
}

void chrome_start(void)
{
    for (int i = 0; i < wm_screen_count() && i < MAX_SCREENS; i++) {
        bars[i] = (struct glass){0};
        bars[i].frame = bar_rect(i);
        bars[i].z = Z_MENUBAR;
        if (menubar_height())
            glass_register(&bars[i]);
    }
    open_menu = NULL;
    open_title = hover_title = -1;
    started = true;
    layout();
    dock_start();
    panel_start();
}

void chrome_stop(void)
{
    if (!started)
        return;
    started = false;
    for (int i = 0; i < MAX_SCREENS; i++)
        glass_unregister(&bars[i]);
    dock_stop();
    panel_stop();
    damage_bars();
}
