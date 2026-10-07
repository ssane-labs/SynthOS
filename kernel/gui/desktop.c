#include <synth/config.h>
#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/theme.h>
#include <synth/time.h>
#include <synth/vfs.h>

#include "image.h"
#include "internal.h"

SYNTH_TABLE_DECLARE(synth_gui_apps, struct gui_app);

#define ICON_CELL_W 84
#define ICON_CELL_H 80
#define START_W 56
#define TASK_BUTTON_W 170
#define TRAY_W 96
#define MENU_W 340
#define MENU_ROW 40

struct gui_style style;

static struct canvas wallpaper;
static bool active;
static int quit_action = -1;
static struct shell *session;
static char wallpaper_setting[PATH_MAX_LEN] = "synthwave";
static bool autostart = true;
static char clipboard[4096];
static int last_minute = -1;
static struct window *start_menu;
static int task_hover = -1;
static bool start_hover;

struct desktop_icon {
    const char *label;
    enum icon_id icon;
    const char *app;
    const char *arg;
};

static const struct desktop_icon desktop_icons[] = {
    {"Computer", ICON_COMPUTER, "Files", "/"},
    {"Home", ICON_HOME, "Files", "/home"},
    {"Programs", ICON_FOLDER, "Files", "/apps"},
    {"Terminal", ICON_TERMINAL, "Terminal", NULL},
    {"Notepad", ICON_NOTEPAD, "Notepad", NULL},
    {"Paint", ICON_PAINT, "Paint", NULL},
    {"Settings", ICON_SETTINGS, "Settings", NULL},
};

static int selected_icon = -1;

bool gui_active(void)
{
    return active;
}

static uint32_t palette_color(int index)
{
    return 0xff000000u | theme_current()->palette[index & 15];
}

static int luminance(uint32_t c)
{
    return (int)((c >> 16 & 0xff) * 3 + (c >> 8 & 0xff) * 6 + (c & 0xff)) / 10;
}

void gui_style_refresh(void)
{
    uint8_t text_attr = theme_attr(ROLE_TEXT);
    uint32_t bg = palette_color(ATTR_BG(text_attr));
    uint32_t fg = palette_color(ATTR_FG(text_attr));
    uint32_t accent = palette_color(ATTR_FG(theme_attr(ROLE_ACCENT)));
    uint32_t accent2 = palette_color(ATTR_FG(theme_attr(ROLE_ACCENT2)));
    bool dark = luminance(bg) < 128;
    for (int i = 0; i < 16; i++)
        style.palette[i] = palette_color(i);
    style.desktop = bg;
    style.accent = accent;
    style.accent2 = accent2;
    style.text = fg;
    style.muted = palette_color(ATTR_FG(theme_attr(ROLE_MUTED)));
    style.error = palette_color(ATTR_FG(theme_attr(ROLE_ERROR)));
    style.ok = palette_color(ATTR_FG(theme_attr(ROLE_OK)));
    style.selection = ALPHA(accent, 80);
    if (dark) {
        style.window = color_mix(bg, 0xffffffffu, 16);
        style.panel = color_mix(bg, 0xffffffffu, 30);
        style.border = color_mix(bg, 0xffffffffu, 60);
        style.field = color_mix(bg, 0xff000000u, 70);
        style.button = color_mix(bg, 0xffffffffu, 40);
        style.button_hover = color_mix(bg, 0xffffffffu, 64);
        style.title_active_top = color_mix(color_mix(accent2, bg, 120), 0xffffffffu, 20);
        style.title_active_bottom = color_mix(accent2, bg, 150);
        style.title_inactive = color_mix(bg, 0xffffffffu, 34);
        style.title_text = RGB(250, 250, 255);
        style.taskbar = ALPHA(color_mix(bg, 0xff000000u, 90), 228);
    } else {
        style.window = color_mix(bg, 0xffffffffu, 150);
        style.panel = color_mix(bg, 0xff000000u, 14);
        style.border = color_mix(bg, 0xff000000u, 70);
        style.field = RGB(255, 255, 255);
        style.button = color_mix(bg, 0xff000000u, 22);
        style.button_hover = color_mix(bg, 0xff000000u, 40);
        style.title_active_top = color_mix(accent, 0xffffffffu, 120);
        style.title_active_bottom = color_mix(accent, 0xffffffffu, 70);
        style.title_inactive = color_mix(bg, 0xff000000u, 26);
        style.title_text = RGB(25, 25, 35);
        style.taskbar = ALPHA(color_mix(bg, 0xffffffffu, 120), 235);
    }
}

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t lerp3(uint32_t a, uint32_t b, int t, int span)
{
    return color_mix(a, b, span ? CLAMP(t * 255 / span, 0, 255) : 0);
}

static void paint_synthwave(struct canvas *c)
{
    int w = c->w, h = c->h, horizon = h * 62 / 100;
    int cx = w / 2, radius = h * 22 / 100, sun_y = horizon - radius / 4;
    uint32_t top = RGB(14, 3, 34), mid = RGB(96, 18, 108), low = RGB(255, 96, 110);
    for (int y = 0; y < horizon; y++) {
        uint32_t sky = y < horizon * 6 / 10 ? lerp3(top, mid, y, horizon * 6 / 10)
                                             : lerp3(mid, low, y - horizon * 6 / 10, horizon - horizon * 6 / 10);
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++) {
            uint32_t p = sky;
            uint32_t r = hash((uint32_t)(y * 7919 + x * 104729));
            if (y < horizon * 7 / 10 && r % 900 == 0)
                p = color_mix(p, 0xffffffffu, 120 + (int)(r >> 24) / 2);
            int dx = x - cx, dy = y - sun_y;
            int d2 = dx * dx + dy * dy;
            if (d2 <= radius * radius) {
                uint32_t sun = lerp3(RGB(255, 230, 90), RGB(255, 50, 150), y - (sun_y - radius), 2 * radius);
                int rel = y - sun_y;
                bool gap = false;
                if (rel > -radius / 6) {
                    int period = MAX(6, radius / 7);
                    int gap_h = 1 + (rel + radius / 6) * (period - 2) / (radius + radius / 6);
                    gap = (rel + radius) % period < gap_h;
                }
                if (!gap)
                    p = sun;
            } else if (d2 < (radius * 14 / 10) * (radius * 14 / 10)) {
                int glow = 255 - (int)((uint32_t)(d2 - radius * radius) * 255 / (uint32_t)(radius * radius * 96 / 100));
                p = color_mix(p, RGB(255, 80, 160), CLAMP(glow, 0, 255) / 4);
            }
            row[x] = p;
        }
    }
    int ridge[64];
    for (int i = 0; i < 64; i++)
        ridge[i] = (int)(hash((uint32_t)i * 2654435761u) % (uint32_t)(h / 9)) + h / 40;
    for (int x = 0; x < w; x++) {
        int seg = x * 63 / MAX(w - 1, 1), off = x * 63 % MAX(w - 1, 1);
        int peak = ridge[seg] + (ridge[MIN(seg + 1, 63)] - ridge[seg]) * off / MAX(w - 1, 1);
        int center_cut = (x > cx - radius * 3 / 2 && x < cx + radius * 3 / 2) ? peak / 3 : peak;
        int top_y = horizon - center_cut;
        for (int y = MAX(top_y, 0); y < horizon; y++)
            c->px[y * c->stride + x] = y == top_y ? RGB(255, 90, 200) : color_mix(RGB(40, 8, 64), RGB(14, 3, 30), (horizon - y) * 255 / MAX(center_cut, 1));
    }
    for (int y = horizon; y < h; y++) {
        int d = y - horizon + 1;
        uint32_t ground = lerp3(RGB(46, 8, 66), RGB(8, 2, 20), y - horizon, h - horizon);
        bool hline = (4000 / d) / 40 != (4000 / (d + 1)) / 40;
        int fade = CLAMP((d - 6) * 12, 0, 255);
        int strength = CLAMP(60 + d * 400 / (h - horizon), 60, 255) * fade / 255;
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < w; x++) {
            int wx = (x - cx) * 300 / d;
            int m = ((wx % 120) + 120) % 120;
            bool vline = m < 300 / d + 1;
            uint32_t p = ground;
            if (vline || hline)
                p = color_mix(ground, RGB(255, 60, 200), strength);
            row[x] = p;
        }
    }
}

static void paint_aurora(struct canvas *c)
{
    uint32_t base = color_mix(style.desktop, 0xff000000u, 60);
    struct {
        int x, y, r;
        uint32_t color;
    } blobs[3] = {{c->w / 5, c->h / 4, c->h / 2, style.accent},
                  {c->w * 4 / 5, c->h / 3, c->h * 6 / 10, style.accent2},
                  {c->w / 2, c->h, c->h * 7 / 10, style.palette[COLOR_CYAN]}};
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + y * c->stride;
        uint32_t line = color_mix(base, color_mix(style.desktop, 0xff000000u, 140), y * 255 / MAX(c->h - 1, 1));
        for (int x = 0; x < c->w; x++) {
            uint32_t p = line;
            for (int i = 0; i < 3; i++) {
                int dx = x - blobs[i].x, dy = y - blobs[i].y;
                int d2 = dx * dx + dy * dy, r2 = blobs[i].r * blobs[i].r;
                if (d2 < r2)
                    p = color_mix(p, blobs[i].color, (r2 - d2) / (r2 / 140 + 1));
            }
            row[x] = p;
        }
    }
}

static bool paint_image(struct canvas *c, const char *path)
{
    struct image img;
    if (!image_load(path, &img))
        return false;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            int sx = x * img.w / c->w, sy = y * img.h / c->h;
            c->px[y * c->stride + x] = 0xff000000u | img.px[sy * img.w + sx];
        }
    image_free(&img);
    return true;
}

void gui_wallpaper_refresh(void)
{
    int w, h;
    wm_screen_size(&w, &h);
    if (!wallpaper.px && !canvas_alloc(&wallpaper, w, h))
        return;
    if (strcmp(wallpaper_setting, "aurora") == 0)
        paint_aurora(&wallpaper);
    else if (strcmp(wallpaper_setting, "solid") == 0)
        gfx_fill(&wallpaper, (struct rect){0, 0, w, h}, style.desktop);
    else if (strcmp(wallpaper_setting, "synthwave") == 0 || !paint_image(&wallpaper, wallpaper_setting))
        paint_synthwave(&wallpaper);
    wm_damage_all();
}

static struct rect icon_cell(int i)
{
    int h;
    wm_screen_size(NULL, &h);
    int per_column = MAX(1, (h - TASKBAR_HEIGHT - 16) / ICON_CELL_H);
    return (struct rect){12 + (i / per_column) * ICON_CELL_W, 12 + (i % per_column) * ICON_CELL_H, ICON_CELL_W - 4,
                         ICON_CELL_H - 4};
}

static void shadowed_text(struct canvas *c, struct rect r, const char *s)
{
    gfx_text_box(c, (struct rect){r.x + 1, r.y + 1, r.w, r.h}, &font_bold, s, ALPHA(0, 200), ALIGN_CENTER);
    gfx_text_box(c, r, &font_bold, s, RGB(255, 255, 255), ALIGN_CENTER);
}

void desktop_paint(struct canvas *c)
{
    if (wallpaper.px)
        gfx_blit(c, 0, 0, &wallpaper, (struct rect){0, 0, wallpaper.w, wallpaper.h});
    else
        gfx_fill(c, (struct rect){0, 0, c->w, c->h}, style.desktop);
    for (int i = 0; i < (int)ARRAY_SIZE(desktop_icons); i++) {
        struct rect cell = icon_cell(i);
        if (rect_empty(rect_intersect(cell, c->clip)))
            continue;
        if (i == selected_icon)
            gfx_round(c, cell, 8, ALPHA(style.accent, 90));
        gfx_icon(c, cell.x + (cell.w - 32) / 2, cell.y + 8, desktop_icons[i].icon, 32);
        shadowed_text(c, (struct rect){cell.x + 2, cell.y + 46, cell.w - 4, 20}, desktop_icons[i].label);
    }
}

static void launch(const char *app_name, const char *arg)
{
    const struct gui_app *app = gui_app_find(app_name);
    if (app)
        app->launch(arg);
}

void desktop_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    if (!down)
        return;
    int hit = -1;
    for (int i = 0; i < (int)ARRAY_SIZE(desktop_icons); i++)
        if (rect_contains(icon_cell(i), x, y))
            hit = i;
    if (hit != selected_icon) {
        if (selected_icon >= 0)
            wm_damage(icon_cell(selected_icon));
        if (hit >= 0)
            wm_damage(icon_cell(hit));
        selected_icon = hit;
    }
    if (hit >= 0 && button == BUTTON_LEFT && clicks >= 2)
        launch(desktop_icons[hit].app, desktop_icons[hit].arg);
}

static struct rect taskbar_rect(void)
{
    int w, h;
    wm_screen_size(&w, &h);
    return (struct rect){0, h - TASKBAR_HEIGHT, w, TASKBAR_HEIGHT};
}

bool taskbar_hit(int x, int y)
{
    return rect_contains(taskbar_rect(), x, y);
}

void taskbar_changed(void)
{
    wm_damage(taskbar_rect());
}

static struct rect start_rect(void)
{
    struct rect bar = taskbar_rect();
    return (struct rect){bar.x + 6, bar.y + 4, START_W - 12, TASKBAR_HEIGHT - 8};
}

static int task_layout(struct window **list, int *button_w)
{
    struct rect bar = taskbar_rect();
    int n = wm_window_list(list, 32);
    int space = bar.w - START_W - TRAY_W - 12;
    *button_w = n ? MIN(TASK_BUTTON_W, space / n - 4) : TASK_BUTTON_W;
    return n;
}

static struct rect task_rect(int i, int button_w)
{
    struct rect bar = taskbar_rect();
    return (struct rect){bar.x + START_W + 4 + i * (button_w + 4), bar.y + 4, button_w, TASKBAR_HEIGHT - 8};
}

void taskbar_paint(struct canvas *c)
{
    struct rect bar = taskbar_rect();
    if (rect_empty(rect_intersect(bar, c->clip)))
        return;
    gfx_fill(c, bar, style.taskbar);
    gfx_fill(c, (struct rect){bar.x, bar.y, bar.w, 1}, ALPHA(0xffffffffu, 40));
    struct rect sr = start_rect();
    if (start_hover || start_menu)
        gfx_round(c, sr, 6, ALPHA(0xffffffffu, start_menu ? 60 : 35));
    gfx_icon(c, sr.x + (sr.w - 32) / 2, sr.y, ICON_LOGO, 32);

    struct window *list[32];
    int button_w;
    int n = task_layout(list, &button_w);
    struct window *focus = wm_focused();
    for (int i = 0; i < n; i++) {
        struct rect r = task_rect(i, button_w);
        bool is_focus = list[i] == focus;
        bool minimized = list[i]->flags & WIN_MINIMIZED;
        if (is_focus || task_hover == i)
            gfx_round(c, r, 6, ALPHA(0xffffffffu, is_focus ? 45 : 28));
        if (is_focus)
            gfx_round(c, (struct rect){r.x + r.w / 2 - 12, r.y + r.h - 3, 24, 3}, 1, style.accent);
        else if (!minimized)
            gfx_round(c, (struct rect){r.x + r.w / 2 - 4, r.y + r.h - 3, 8, 3}, 1, ALPHA(0xffffffffu, 120));
        gfx_icon(c, r.x + 8, r.y + (r.h - 16) / 2, list[i]->icon, 16);
        gfx_text_box(c, (struct rect){r.x + 30, r.y, r.w - 36, r.h - 2}, &font_sans, list[i]->title,
                     ALPHA(luminance(style.taskbar) < 128 ? 0xffffffffu : 0xff101018u, minimized ? 150 : 255), ALIGN_LEFT);
    }

    struct rtc_time t;
    hal_rtc(&t);
    char clock[16], date[16];
    ksnprintf(clock, sizeof(clock), "%02u:%02u", t.hour, t.minute);
    ksnprintf(date, sizeof(date), "%02u.%02u.%04u", t.day, t.month, t.year);
    struct rect tray = {bar.x + bar.w - TRAY_W, bar.y, TRAY_W - 8, bar.h};
    uint32_t tc = luminance(style.taskbar) < 128 ? 0xffffffffu : 0xff101018u;
    gfx_text_box(c, (struct rect){tray.x, tray.y + 3, tray.w, 18}, &font_bold, clock, tc, ALIGN_RIGHT);
    gfx_text_box(c, (struct rect){tray.x, tray.y + 19, tray.w, 18}, &font_sans, date, ALPHA(tc, 180), ALIGN_RIGHT);
}

void taskbar_hover(int x, int y)
{
    bool start = rect_contains(start_rect(), x, y);
    int hover = -1;
    struct window *list[32];
    int button_w;
    int n = task_layout(list, &button_w);
    for (int i = 0; i < n; i++)
        if (rect_contains(task_rect(i, button_w), x, y))
            hover = i;
    if (start != start_hover || hover != task_hover) {
        start_hover = start;
        task_hover = hover;
        taskbar_changed();
    }
}

void taskbar_mouse(int x, int y, unsigned button, bool down)
{
    if (!down)
        return;
    if (rect_contains(start_rect(), x, y)) {
        start_menu_toggle();
        return;
    }
    struct window *list[32];
    int button_w;
    int n = task_layout(list, &button_w);
    for (int i = 0; i < n; i++) {
        if (!rect_contains(task_rect(i, button_w), x, y))
            continue;
        struct window *w = list[i];
        if (button == BUTTON_RIGHT) {
            wm_close(w);
        } else if (w == wm_focused() && !(w->flags & WIN_MINIMIZED)) {
            wm_minimize(w);
        } else {
            w->flags &= ~WIN_MINIMIZED;
            wm_focus(w);
            wm_damage_all();
        }
        return;
    }
}

struct menu_entry {
    const char *label;
    enum icon_id icon;
    const struct gui_app *app;
    int action;
};

struct start_state {
    struct menu_entry entries[32];
    int count;
    int hover;
};

static void collect(const struct gui_app *app, void *ctx)
{
    struct start_state *s = ctx;
    if (s->count < 28)
        s->entries[s->count++] = (struct menu_entry){app->name, app->icon, app, -1};
}

static const char *const power_labels[3] = {"Text mode", "Restart", "Shut down"};

static struct rect power_rect(struct window *w, int i)
{
    int bw = (w->frame.w - 24) / 3;
    return (struct rect){12 + i * bw, w->frame.h - 46, bw - 6, 34};
}

static int menu_row_at(struct window *w, int x, int y)
{
    struct start_state *s = w->data;
    for (int i = 0; i < s->count; i++)
        if (y >= 70 + i * MENU_ROW && y < 70 + (i + 1) * MENU_ROW && x > 8 && x < w->frame.w - 8)
            return i;
    for (int i = 0; i < 3; i++)
        if (rect_contains(power_rect(w, i), x, y))
            return 100 + i;
    return -1;
}

static void start_paint(struct window *w, struct canvas *c)
{
    struct start_state *s = w->data;
    gfx_fill(c, (struct rect){0, 0, c->w, c->h}, color_mix(style.window, 0xff000000u, 20));
    gfx_gradient(c, (struct rect){0, 0, c->w, 60}, style.title_active_top, style.title_active_bottom);
    gfx_icon(c, 14, 14, ICON_LOGO, 32);
    gfx_text(c, 56, 12, &font_title, "SynthOS", style.title_text);
    const char *user = session ? sh_get(session, "USER") : NULL;
    char line[64];
    ksnprintf(line, sizeof(line), "%s  ·  %s", user ? user : "root", "v" SYNTH_VERSION " \"" SYNTH_CODENAME "\"");
    gfx_text(c, 58, 36, &font_sans, line, ALPHA(style.title_text, 200));
    for (int i = 0; i < s->count; i++) {
        struct rect row = {8, 70 + i * MENU_ROW, c->w - 16, MENU_ROW - 2};
        if (s->hover == i)
            gfx_round(c, row, 6, style.selection);
        gfx_icon(c, row.x + 8, row.y + 3, s->entries[i].icon, 32);
        gfx_text_box(c, (struct rect){row.x + 50, row.y + 2, row.w - 56, 18}, &font_bold, s->entries[i].label,
                     style.text, ALIGN_LEFT);
        if (s->entries[i].app && s->entries[i].app->description)
            gfx_text_box(c, (struct rect){row.x + 50, row.y + 19, row.w - 56, 16}, &font_sans,
                         s->entries[i].app->description, style.muted, ALIGN_LEFT);
    }
    gfx_fill(c, (struct rect){0, c->h - 58, c->w, 1}, style.border);
    for (int i = 0; i < 3; i++) {
        struct rect r = power_rect(w, i);
        gfx_round(c, r, 6, s->hover == 100 + i ? (i == 2 ? style.error : style.button_hover) : style.button);
        gfx_text_box(c, r, &font_bold, power_labels[i], style.text, ALIGN_CENTER);
    }
}

static void start_activate(struct window *w, int row)
{
    struct start_state *s = w->data;
    if (row >= 100) {
        wm_destroy(w);
        gui_quit(row - 100);
        return;
    }
    if (row < 0 || row >= s->count)
        return;
    const struct gui_app *app = s->entries[row].app;
    wm_destroy(w);
    if (app)
        app->launch(NULL);
}

static void start_event(struct window *w, struct gui_event *e)
{
    struct start_state *s = w->data;
    int old = s->hover;
    if (e->type == EV_MOUSE_MOVE)
        s->hover = menu_row_at(w, e->x, e->y);
    else if (e->type == EV_LEAVE)
        s->hover = -1;
    else if (e->type == EV_MOUSE_UP && e->button == BUTTON_LEFT) {
        start_activate(w, menu_row_at(w, e->x, e->y));
        return;
    } else if (e->type == EV_KEY) {
        int key = KEY_BASE(e->key);
        if (key == KEY_ESC) {
            wm_destroy(w);
            return;
        }
        if (key == KEY_DOWN)
            s->hover = s->hover < 0 || s->hover >= s->count - 1 ? 0 : s->hover + 1;
        else if (key == KEY_UP)
            s->hover = s->hover <= 0 ? s->count - 1 : s->hover - 1;
        else if (key == KEY_ENTER || key == '\r') {
            start_activate(w, s->hover);
            return;
        }
    }
    if (old != s->hover)
        wm_invalidate(w);
}

static void start_destroy(struct window *w)
{
    kfree(w->data);
    if (start_menu == w)
        start_menu = NULL;
    taskbar_changed();
}

static const struct window_ops start_ops = {start_paint, start_event, NULL, start_destroy};

static int compare_entries(const void *a, const void *b)
{
    return strcmp(((const struct menu_entry *)a)->label, ((const struct menu_entry *)b)->label);
}

static void sort_entries(struct menu_entry *e, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && compare_entries(&e[j - 1], &e[j]) > 0; j--) {
            struct menu_entry t = e[j];
            e[j] = e[j - 1];
            e[j - 1] = t;
        }
}

void start_menu_toggle(void)
{
    if (start_menu) {
        wm_destroy(start_menu);
        return;
    }
    struct start_state *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    s->hover = -1;
    gui_app_foreach(collect, s);
    sort_entries(s->entries, s->count);
    int h = 70 + s->count * MENU_ROW + 66;
    struct rect bar = taskbar_rect();
    start_menu = wm_popup(bar.x + 6, bar.y - h - 6, MENU_W, h, &start_ops, s, NULL);
    if (!start_menu)
        kfree(s);
    taskbar_changed();
}

static bool word_prefix(const char *name, const char *prefix)
{
    size_t n = strlen(prefix);
    for (const char *w = name; *w; w++)
        if ((w == name || w[-1] == ' ') && str_ncasecmp(w, prefix, n) == 0)
            return true;
    return false;
}

const struct gui_app *gui_app_find(const char *name)
{
    if (!name || !*name)
        return NULL;
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        if (str_casecmp(app->name, name) == 0)
            return app;
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        if (word_prefix(app->name, name))
            return app;
    return NULL;
}

void gui_app_foreach(void (*fn)(const struct gui_app *app, void *ctx), void *ctx)
{
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        fn(app, ctx);
}

static bool extension_listed(const char *list, const char *ext)
{
    if (!list || !ext)
        return false;
    size_t n = strlen(ext);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && str_ncasecmp(p, ext, n) == 0)
            return true;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}

bool gui_open_file(const char *path)
{
    if (vfs_is_dir(path)) {
        launch("Files", path);
        return true;
    }
    const char *ext = path_ext(path);
    SYNTH_TABLE_FOREACH(synth_gui_apps, struct gui_app, app)
        if (extension_listed(app->extensions, ext)) {
            app->launch(path);
            return true;
        }
    if (exec_is_executable_name(path)) {
        char line[PATH_MAX_LEN + 8];
        ksnprintf(line, sizeof(line), "'%s'", path);
        gui_terminal_run(line);
        return true;
    }
    launch("Notepad", path);
    return true;
}

const char *gui_clipboard(void)
{
    return clipboard;
}

void gui_set_clipboard(const char *text, size_t n)
{
    n = MIN(n, sizeof(clipboard) - 1);
    memcpy(clipboard, text, n);
    clipboard[n] = 0;
}

void gui_terminal_show(void)
{
    struct window *t = terminal_window();
    if (t)
        wm_show(t);
}

void gui_terminal_run(const char *command)
{
    gui_terminal_show();
    if (!session || !session->idle) {
        gui_message("Terminal is busy", "A program is still running in the terminal. Wait for it to finish or "
                                        "press Ctrl+C there.");
        return;
    }
    input_inject(KEY_CTRL('u'));
    for (const char *p = command; *p; p++)
        input_inject((uint8_t)*p);
    input_inject(KEY_ENTER);
}

void gui_quit(int action)
{
    if (!session)
        return;
    if (!session->idle) {
        gui_message("Cannot leave yet", "A program is still running in the terminal.");
        gui_terminal_show();
        return;
    }
    quit_action = action;
    session->ctl = CTL_EXIT;
    input_inject(KEY_EOF);
}

static void handle_key(int key)
{
    int base = KEY_BASE(key);
    if (base == KEY_SUPER || (key == (KEY_ESC | KEY_MOD_CTRL))) {
        start_menu_toggle();
        return;
    }
    if (key == (KEY_TAB | KEY_MOD_ALT)) {
        wm_close_popups();
        wm_cycle();
        return;
    }
    if (key == (KEY_F4 | KEY_MOD_ALT)) {
        struct window *f = wm_focused();
        if (f)
            wm_close(f);
        return;
    }
    struct window *popup = wm_top_popup();
    if (popup && popup->ops && popup->ops->event) {
        struct gui_event e = {.type = EV_KEY, .key = key};
        popup->ops->event(popup, &e);
        return;
    }
    wm_key(key);
}

static bool pumping;

static void pump(int timeout_ms)
{
    if (pumping)
        return;
    pumping = true;
    bool busy = false;
    for (int i = 0; i < 64; i++) {
        int key = hal_key_read(0);
        if (key == KEY_NONE)
            break;
        handle_key(key);
        busy = true;
    }
    struct pointer_event ev;
    for (int i = 0; i < 64 && hal_pointer_read(&ev); i++) {
        wm_pointer(&ev);
        busy = true;
    }
    wm_timers();
    struct rtc_time t;
    hal_rtc(&t);
    if (t.minute != last_minute) {
        last_minute = t.minute;
        taskbar_changed();
    }
    terminal_flush();
    wm_render();
    pumping = false;
    if (!busy && timeout_ms != 0)
        hal_sleep_ms((uint32_t)(timeout_ms > 0 ? MIN(timeout_ms, 8) : 8));
}

int gui_run(struct shell *sh)
{
    if (active)
        return sh_error(sh, "desktop", "the desktop is already running");
    struct framebuffer fb;
    if (!hal_graphics_enter(&fb))
        return sh_error(sh, "desktop", "no graphics mode available on this display");
    if (!wm_start(&fb)) {
        hal_graphics_leave();
        return sh_error(sh, "desktop", "not enough memory for the desktop");
    }
    struct display *text_display = con_display();
    active = true;
    session = sh;
    quit_action = -1;
    last_minute = -1;
    selected_icon = -1;
    gui_style_refresh();
    gui_wallpaper_refresh();
    if (!terminal_open()) {
        active = false;
        wm_stop();
        hal_graphics_leave();
        con_init(text_display);
        return sh_error(sh, "desktop", "cannot open the terminal window");
    }
    input_set_pump(pump);
    con_role(ROLE_ACCENT);
    kprintf("SynthOS desktop");
    con_role(ROLE_MUTED);
    kprintf(" - this terminal runs the shell. Type 'exit' or use the start menu to leave.\n");
    con_reset();

    shell_repl(sh);

    sh->ctl = CTL_NONE;
    input_set_pump(NULL);
    terminal_close();
    wm_stop();
    canvas_free(&wallpaper);
    active = false;
    session = NULL;
    hal_graphics_leave();
    con_init(text_display);
    if (quit_action == QUIT_REBOOT)
        hal_reboot();
    if (quit_action == QUIT_POWEROFF)
        hal_poweroff();
    return 0;
}

static void get_wallpaper(char *buf, size_t cap)
{
    str_copy(buf, wallpaper_setting, cap);
}

static int set_wallpaper(struct shell *sh, const char *value)
{
    UNUSED(sh);
    str_copy(wallpaper_setting, value, sizeof(wallpaper_setting));
    if (active) {
        gui_style_refresh();
        gui_wallpaper_refresh();
    }
    return 0;
}

static const struct setting s_wallpaper = {"wallpaper", "synthwave|aurora|solid|<file.bmp>", "desktop background",
                                           get_wallpaper, set_wallpaper};
SETTING(s_wallpaper);

static void get_autostart(char *buf, size_t cap)
{
    str_copy(buf, autostart ? "on" : "off", cap);
}

static int set_autostart(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    autostart = strcmp(value, "on") == 0;
    return 0;
}

static const struct setting s_autostart = {"desktop", "on|off", "start the graphical desktop at boot",
                                           get_autostart, set_autostart};
SETTING(s_autostart);

bool gui_autostart(void)
{
    return autostart;
}

COMMAND(desktop, "startx,gui", "system", "desktop", "start the graphical desktop")
{
    UNUSED(argc);
    UNUSED(argv);
    return gui_run(sh);
}

static void list_app(const struct gui_app *app, void *ctx)
{
    struct shell *sh = ctx;
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "  %-16s", app->name);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, " %s\n", app->description);
    sh_role(sh, ROLE_TEXT);
}

COMMAND(launch, "xopen", "system", "launch [app|file] [file]", "start a desktop app or open a file in one")
{
    if (argc < 2) {
        gui_app_foreach(list_app, sh);
        return 0;
    }
    if (!active)
        return sh_error(sh, "launch", "the desktop is not running (start it with 'desktop')");
    char path[PATH_MAX_LEN];
    const struct gui_app *app = gui_app_find(argv[1]);
    if (app) {
        if (argc > 2 && sh_resolve(sh, argv[2], path, sizeof(path)) != 0)
            return sh_error(sh, "launch", "bad path: %s", argv[2]);
        app->launch(argc > 2 ? path : NULL);
        return 0;
    }
    if (sh_resolve(sh, argv[1], path, sizeof(path)) != 0 || !vfs_exists(path))
        return sh_error(sh, "launch", "no such app or file: %s", argv[1]);
    return gui_open_file(path) ? 0 : sh_error(sh, "launch", "nothing can open %s", argv[1]);
}
