#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

#define MAX_ITEMS 28
#define PAD 8
#define GAP 6
#define BOTTOM 6
#define TOOLTIP_H 30
#define SEPARATOR_W 13
#define TRASH_DIR "/home/.Trash"

enum item_kind {
    DI_APP,
    DI_LAUNCHPAD,
    DI_SEPARATOR,
    DI_WINDOW,
    DI_TRASH,
};

struct item {
    enum item_kind kind;
    const struct gui_app *app;
    struct window *win;
    int size, x;
    bool running;
};

static const char *const pinned[] = {"Files",       "Launchpad",      "Browser",     "Terminal",
                                     "Notepad",     "Calculator",     "Paint",       "System Monitor",
                                     "Minesweeper", "System Settings"};

static struct item items[MAX_ITEMS];
static int count;
static int base_size = 52;
static bool magnify = true;
static bool autohide;
static int hover_x = -1, hover_item = -1, pressed_item = -1;
static bool revealed;
static int mag, mag_x = -1;
static const struct gui_app *bounce_app;
static uint64_t bounce_start;
static struct glass glass;
static struct rect dock_rect, zone;
static bool started;

static int max_size(void)
{
    return magnify ? base_size * 3 / 2 : base_size;
}

static bool is_pinned(const struct gui_app *app)
{
    for (size_t i = 0; i < ARRAY_SIZE(pinned); i++)
        if (!strcmp(pinned[i], app->name))
            return true;
    return false;
}

static void add_running(const struct gui_app *app, void *ctx)
{
    UNUSED(ctx);
    if (count < MAX_ITEMS - 4 && !is_pinned(app) && wm_app_window_count(app) > 0)
        items[count++] = (struct item){.kind = DI_APP, .app = app, .running = true};
}

static void build(void)
{
    count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(pinned) && count < MAX_ITEMS; i++) {
        if (!strcmp(pinned[i], "Launchpad")) {
            items[count++] = (struct item){.kind = DI_LAUNCHPAD};
            continue;
        }
        const struct gui_app *app = gui_app_find(pinned[i]);
        if (app)
            items[count++] = (struct item){.kind = DI_APP, .app = app, .running = wm_app_window_count(app) > 0};
    }
    gui_app_foreach(add_running, NULL);
    struct window *list[32];
    int n = wm_window_list(list, 32);
    for (int i = 0; i < n && count < MAX_ITEMS - 4; i++) {
        bool seen = !list[i]->app;
        for (int k = 0; k < count && !seen; k++)
            seen = items[k].kind == DI_APP && items[k].app == list[i]->app;
        if (!seen)
            items[count++] = (struct item){.kind = DI_APP, .app = list[i]->app, .running = true};
    }
    items[count++] = (struct item){.kind = DI_SEPARATOR};
    for (int i = 0; i < n && count < MAX_ITEMS - 1; i++)
        if ((list[i]->flags & WIN_MINIMIZED) && list[i]->thumb.px)
            items[count++] = (struct item){.kind = DI_WINDOW, .win = list[i]};
    items[count++] = (struct item){.kind = DI_TRASH};
}

static bool visible_now(void)
{
    return !autohide || revealed;
}

int dock_reserved(void)
{
    return autohide ? 0 : base_size + 2 * PAD + BOTTOM + 4;
}

bool dock_hidden(void)
{
    return !visible_now();
}

static int item_width(const struct item *it)
{
    return it->kind == DI_SEPARATOR ? SEPARATOR_W : it->size;
}

static void layout(void)
{
    struct rect s = wm_screen_rect(0);
    int base_total = 0;
    for (int i = 0; i < count; i++)
        base_total += (items[i].kind == DI_SEPARATOR ? SEPARATOR_W : base_size) + (i ? GAP : 0);
    int x0 = s.x + (s.w - base_total - 2 * PAD) / 2 + PAD;
    int big = max_size();
    for (int i = 0, x = x0; i < count; i++) {
        int w = items[i].kind == DI_SEPARATOR ? SEPARATOR_W : base_size;
        int center = x + w / 2;
        int size = base_size;
        if (mag > 0 && mag_x >= 0 && items[i].kind != DI_SEPARATOR && big > base_size) {
            int d = (mag_x > center ? mag_x - center : center - mag_x) * 256 / (base_size * 5 / 2);
            if (d < 256)
                size = base_size + (big - base_size) * (65536 - d * d) / 65536 * mag / 256;
        }
        items[i].size = size;
        x += w + GAP;
    }
    int total = 0;
    for (int i = 0; i < count; i++)
        total += item_width(&items[i]) + (i ? GAP : 0);
    int x = s.x + (s.w - total - 2 * PAD) / 2;
    int h = base_size + 2 * PAD;
    int y = visible_now() ? s.y + s.h - BOTTOM - h : s.y + s.h + 2;
    dock_rect = (struct rect){x, y, total + 2 * PAD, h};
    x += PAD;
    for (int i = 0; i < count; i++) {
        items[i].x = x;
        x += item_width(&items[i]) + GAP;
    }
    struct rect old = zone;
    int extra = big - base_size + TOOLTIP_H + 4;
    zone = (struct rect){dock_rect.x - 40, dock_rect.y - extra, dock_rect.w + 80, dock_rect.h + extra + BOTTOM};
    glass_set_frame(&glass, dock_rect);
    wm_damage_z(rect_union(old, zone), Z_DOCK);
}

static enum icon_id item_icon(const struct item *it)
{
    switch (it->kind) {
    case DI_APP: return it->app->icon;
    case DI_LAUNCHPAD: return ICON_LAUNCHPAD;
    case DI_TRASH: {
        struct vdir d;
        bool full = false;
        if (vfs_opendir(TRASH_DIR, &d) == 0) {
            struct vfs_dirent ent;
            while (!full && vfs_readdir(&d, &ent) == 0)
                full = ent.name[0] != '.';
            vfs_closedir(&d);
        }
        return full ? ICON_TRASH_FULL : ICON_TRASH;
    }
    default: return ICON_APP;
    }
}

static const char *item_label(const struct item *it)
{
    switch (it->kind) {
    case DI_APP: return it->app->name;
    case DI_LAUNCHPAD: return "Launchpad";
    case DI_TRASH: return "Trash";
    case DI_WINDOW: return it->win->title;
    default: return NULL;
    }
}

static int bounce_offset(const struct item *it)
{
    if (!bounce_app || it->kind != DI_APP || it->app != bounce_app)
        return 0;
    uint64_t t = hal_ticks_ms() - bounce_start;
    if (t >= 1000)
        return 0;
    int phase = (int)(t % 500) * 1024 / 500, height = t < 500 ? base_size / 2 : base_size / 4;
    return height * 4 * phase / 1024 * (1024 - phase) / 1024;
}

static struct rect icon_rect(const struct item *it)
{
    int bottom = dock_rect.y + PAD + base_size - bounce_offset(it);
    return (struct rect){it->x, bottom - it->size, it->size, it->size};
}

struct rect dock_window_rect(struct window *w)
{
    if (!started)
        return (struct rect){0, 0, 0, 0};
    for (int i = 0; i < count; i++)
        if (items[i].kind == DI_WINDOW && items[i].win == w)
            return icon_rect(&items[i]);
    return (struct rect){0, 0, 0, 0};
}

void dock_bounce(const struct gui_app *app)
{
    bounce_app = app;
    bounce_start = hal_ticks_ms();
}

bool dock_animate(void)
{
    if (!started)
        return false;
    bool busy = false;
    int target = hover_x >= 0 ? 256 : 0;
    if (mag != target) {
        int step = (target - mag) / 3;
        mag = step ? mag + step : target;
        if (hover_x >= 0)
            mag_x = hover_x;
        layout();
        busy = true;
    }
    if (bounce_app) {
        if (hal_ticks_ms() - bounce_start >= 1000)
            bounce_app = NULL;
        wm_damage_z(zone, Z_DOCK);
        busy = true;
    }
    return busy;
}

void dock_paint(struct canvas *c, int screen)
{
    if (!started || screen != 0 || rect_empty(rect_intersect(zone, c->clip)))
        return;
    struct glass_look look;
    int radius = dock_rect.h * 2 / 5;
    glass_look_for(&look, GLASS_DOCK, radius);
    if (style.glass == GLASS_LIQUID)
        gfx_drop_shadow(c, dock_rect, radius, 16, style.dark ? 90 : 40, 5);
    glass_draw(c, &glass, &look);
    for (int i = 0; i < count; i++) {
        struct item *it = &items[i];
        if (it->kind == DI_SEPARATOR) {
            gfx_fill(c, (struct rect){it->x + SEPARATOR_W / 2, dock_rect.y + 10, 1, dock_rect.h - 20},
                     style.dark ? ALPHA(0xffffffffu, 60) : ALPHA(0, 50));
            continue;
        }
        struct rect r = icon_rect(it);
        if (rect_empty(rect_intersect(r, c->clip)))
            continue;
        int alpha = pressed_item == i ? 170 : 255;
        if (it->kind == DI_WINDOW) {
            struct canvas *t = &it->win->thumb;
            int tw = r.w, th = r.w * t->h / MAX(t->w, 1);
            if (th > r.h) {
                th = r.h;
                tw = r.h * t->w / MAX(t->h, 1);
            }
            struct rect dst = {r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, tw, th};
            gfx_drop_shadow(c, dst, 4, 6, 70, 2);
            gfx_scale(c, dst, t, alpha);
            gfx_icon(c, r.x + r.w - 20, r.y + r.h - 20, it->win->icon, 20);
        } else if (it->kind == DI_APP && it->app->picture) {
            gfx_scale(c, (struct rect){r.x + r.w / 16, r.y + r.w / 16, r.w - r.w / 8, r.h - r.w / 8}, it->app->picture,
                      alpha);
        } else {
            gfx_icon_alpha(c, r.x, r.y, item_icon(it), r.w, alpha);
        }
        if (it->running) {
            int cx = r.x + r.w / 2, cy = dock_rect.y + dock_rect.h - 4;
            gfx_circle(c, cx, cy, 2, style.dark ? RGB(236, 236, 240) : RGB(30, 30, 36));
            gfx_circle(c, cx, cy, 1, style.dark ? RGB(255, 255, 255) : RGB(70, 70, 80));
        }
    }
    if (hover_item >= 0 && hover_item < count && pressed_item < 0) {
        const char *label = item_label(&items[hover_item]);
        if (label) {
            struct rect r = icon_rect(&items[hover_item]);
            int w = gfx_text_width(&font_sans, label) + 22;
            struct rect tip = {r.x + r.w / 2 - w / 2, r.y - TOOLTIP_H, w, 24};
            gfx_drop_shadow(c, tip, 12, 10, 50, 3);
            gfx_round(c, tip, 12, style.dark ? RGBA(46, 46, 52, 235) : RGBA(252, 252, 254, 235));
            gfx_round_frame_gradient(c, tip, 12, style.dark ? ALPHA(0xffffffffu, 60) : ALPHA(0xffffffffu, 255),
                                     style.dark ? ALPHA(0xffffffffu, 16) : ALPHA(0, 26));
            gfx_text_box(c, tip, &font_sans, label, style.text, ALIGN_CENTER);
        }
    }
}

static int item_at(int x, int y)
{
    for (int i = 0; i < count; i++) {
        if (items[i].kind == DI_SEPARATOR)
            continue;
        struct rect r = icon_rect(&items[i]);
        r = (struct rect){r.x - GAP / 2, MIN(r.y, dock_rect.y), r.w + GAP, dock_rect.y + dock_rect.h - MIN(r.y, dock_rect.y)};
        if (rect_contains(r, x, y))
            return i;
    }
    return -1;
}

bool dock_hit(int x, int y)
{
    if (!started)
        return false;
    if (!visible_now()) {
        struct rect s = wm_screen_rect(0);
        return autohide && y >= s.y + s.h - 2 && wm_screen_at(x, y) == 0;
    }
    return rect_contains(dock_rect, x, y) || item_at(x, y) >= 0;
}

static void empty_trash(void)
{
    struct vdir d;
    if (vfs_opendir(TRASH_DIR, &d) != 0)
        return;
    char names[64][NAME_MAX_LEN];
    int n = 0;
    struct vfs_dirent ent;
    while (n < 64 && vfs_readdir(&d, &ent) == 0)
        if (strcmp(ent.name, ".") && strcmp(ent.name, ".."))
            str_copy(names[n++], ent.name, NAME_MAX_LEN);
    vfs_closedir(&d);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX_LEN];
        path_join(TRASH_DIR, names[i], path, sizeof(path));
        if (vfs_is_dir(path)) {
            char cmd[PATH_MAX_LEN + 16];
            ksnprintf(cmd, sizeof(cmd), "rm -r '%s'", path);
            gui_terminal_run(cmd);
        } else {
            vfs_unlink(path);
        }
    }
    dock_changed();
}

static int menu_item;

static void pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (menu_item < 0 || menu_item >= count)
        return;
    struct item *it = &items[menu_item];
    if (it->kind == DI_TRASH) {
        if (index == 0)
            gui_open_file(TRASH_DIR);
        else
            empty_trash();
        return;
    }
    if (it->kind != DI_APP)
        return;
    if (index == 0)
        gui_app_launch(it->app, NULL);
    else if (index == 1)
        wm_focus_app(it->app);
    else if (index == 3)
        for (struct window *w = wm_app_window(it->app); w; w = wm_app_window(it->app)) {
            wm_close(w);
            if (wm_window_alive(w))
                break;
        }
}

static void activate(int i)
{
    struct item *it = &items[i];
    switch (it->kind) {
    case DI_APP:
        if (wm_app_window_count(it->app) > 0) {
            wm_focus_app(it->app);
        } else {
            dock_bounce(it->app);
            gui_app_launch(it->app, NULL);
        }
        break;
    case DI_LAUNCHPAD: launchpad_toggle(); break;
    case DI_WINDOW:
        if (wm_window_alive(it->win))
            wm_show(it->win);
        break;
    case DI_TRASH:
        vfs_mkdirs(TRASH_DIR);
        gui_open_file(TRASH_DIR);
        break;
    default: break;
    }
}

void dock_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    UNUSED(clicks);
    int i = item_at(x, y);
    if (down) {
        pressed_item = button == BUTTON_LEFT ? i : -1;
        wm_damage_z(zone, Z_DOCK);
        if (button == BUTTON_RIGHT && i >= 0) {
            menu_item = i;
            if (items[i].kind == DI_TRASH) {
                static const struct menu_item trash_items[] = {{"Open", 0, NULL}, {"Empty Trash", 0, NULL}};
                ui_menu_items(NULL, x, dock_rect.y - 70, trash_items, 2, pick);
            } else if (items[i].kind == DI_APP) {
                bool running = wm_app_window_count(items[i].app) > 0;
                static const struct menu_item app_items[] = {
                    {"New Window", 0, NULL}, {"Show All Windows", 0, NULL}, {"", 0, NULL}, {"Quit", 0, NULL}};
                static const struct menu_item open_items[] = {{"Open", 0, NULL}};
                if (running)
                    ui_menu_items(NULL, x, dock_rect.y - 110, app_items, 4, pick);
                else
                    ui_menu_items(NULL, x, dock_rect.y - 40, open_items, 1, pick);
            }
        }
        return;
    }
    int pressed = pressed_item;
    pressed_item = -1;
    wm_damage_z(zone, Z_DOCK);
    if (pressed >= 0 && pressed == i)
        activate(i);
}

void dock_hover(int x, int y)
{
    if (!started)
        return;
    struct rect s = wm_screen_rect(0);
    bool over = y >= 0 && wm_screen_at(x, y) == 0 && (rect_contains(zone, x, y) && y >= dock_rect.y - (max_size() - base_size));
    if (autohide) {
        bool want = revealed ? (y >= 0 && y >= dock_rect.y - (max_size() - base_size) - 10) : (y >= s.y + s.h - 2);
        if (want != revealed) {
            revealed = want;
            layout();
        }
    }
    int nx = over ? x : -1;
    int item = over ? item_at(x, y) : -1;
    if (nx == hover_x && item == hover_item)
        return;
    hover_x = nx;
    hover_item = item;
    if (nx >= 0 && mag > 0)
        mag_x = nx;
    layout();
}

void dock_changed(void)
{
    if (!started)
        return;
    build();
    if (hover_item >= count)
        hover_item = -1;
    layout();
}

void dock_start(void)
{
    glass = (struct glass){0};
    glass.z = Z_DOCK;
    hover_x = hover_item = pressed_item = -1;
    mag = 0;
    mag_x = -1;
    bounce_app = NULL;
    revealed = false;
    started = true;
    build();
    layout();
    glass.frame = dock_rect;
    glass_register(&glass);
}

void dock_stop(void)
{
    started = false;
    glass_unregister(&glass);
}


static void get_size(char *buf, size_t cap)
{
    str_copy(buf, base_size <= 40 ? "small" : base_size >= 64 ? "large" : "medium", cap);
}

static int set_size(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!strcmp(value, "small"))
        base_size = 40;
    else if (!strcmp(value, "medium"))
        base_size = 52;
    else if (!strcmp(value, "large"))
        base_size = 64;
    else
        return E_INVAL;
    dock_changed();
    if (gui_active())
        wm_damage_all();
    return 0;
}

static const struct setting s_docksize = {"docksize", "small|medium|large", "size of the dock icons", get_size,
                                          set_size};
SETTING(s_docksize);

static void get_magnify(char *buf, size_t cap)
{
    str_copy(buf, magnify ? "on" : "off", cap);
}

static int set_magnify(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    magnify = !strcmp(value, "on");
    dock_changed();
    return 0;
}

static const struct setting s_magnify = {"magnify", "on|off", "enlarge dock icons under the pointer", get_magnify,
                                         set_magnify};
SETTING(s_magnify);

static void get_hide(char *buf, size_t cap)
{
    str_copy(buf, autohide ? "on" : "off", cap);
}

static int set_hide(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    autohide = !strcmp(value, "on");
    revealed = false;
    dock_changed();
    if (gui_active())
        wm_damage_all();
    return 0;
}

static const struct setting s_dockhide = {"dockhide", "on|off", "hide the dock until the pointer reaches the bottom",
                                          get_hide, set_hide};
SETTING(s_dockhide);
