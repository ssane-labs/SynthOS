#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* Desktop widgets: they live on the desktop layer under the windows, each on its own piece of glass. */

#define MAX_TASKS 8
#define TASK_ROW 28

#define WIDGET_W 300
#define WIDGET_GAP 14
#define MAX_LOOKS 8

struct widget {
    struct rect r;
    struct glass glass;
    struct spring lift, intro, ax, ay;
    int dx, dy;
    bool shown, card, placed, leaving;
};

static const char *const widget_names[WIDGET_COUNT] = {"clock", "weather", "music", "calendar", "visualizer", "tasks"};
static const char *const widget_titles[WIDGET_COUNT] = {"Clock", "Weather", "Music", "Calendar", "Visualizer", "Tasks"};
static const enum symbol_id widget_syms[WIDGET_COUNT] = {SYM_CLOCK, SYM_WEATHER, SYM_MUSIC,
                                                         SYM_CALENDAR, SYM_VOLUME, SYM_LIST};
static const int column_order[WIDGET_COUNT] = {WIDGET_CLOCK, WIDGET_CALENDAR, WIDGET_WEATHER,
                                               WIDGET_TASKS, WIDGET_MUSIC, WIDGET_VISUALIZER};

static struct widget widgets[WIDGET_COUNT];
static unsigned look_masks[MAX_LOOKS];
static bool masks_loaded;
static bool editing;
static struct spring edit_pop;
static struct window *gallery;
static int right_columns;
static bool started;
static int hover = -1, hover_part = -1, pressed = -1, pressed_part = -1;
static int drag = -1, drag_x, drag_y;
static bool dragged;
static uint64_t last_tick, started_at;
static struct spring bars[MEDIA_BANDS], progress;
static int last_minute = -1;
static struct window *calendar;

#define W_TASKS WIDGET_TASKS

static void set_mask(unsigned mask);
static unsigned current_mask(void);
static int menu_widget = -1;

static void widget_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index == 0)
        widgets_edit(true);
    else if (index == 1 && menu_widget >= 0)
        set_mask(current_mask() & ~(1u << menu_widget));
}

static struct {
    char text[MAX_TASKS][64];
    bool done[MAX_TASKS];
    int count;
    bool loaded;
} tasks;

enum weather_state {
    WX_IDLE,
    WX_DNS,
    WX_CONNECT,
    WX_RECV,
    WX_DONE,
    WX_FAILED,
};

static struct {
    enum weather_state state;
    int sock;
    char buf[512];
    int len;
    bool sent;
    uint64_t started, next;
    struct dns_query dns;
    char temp[16], cond[48], place[48];
} wx = {.sock = -1, .dns = {.sock = -1}};

static char weather_city[48] = "";

static uint32_t ink(void)
{
    return style.dark ? RGB(246, 244, 250) : color_mix(RGB(30, 26, 24), style.tint | 0xff000000u, 60);
}

static uint32_t soft(void)
{
    return ALPHA(ink(), 160);
}

static int look_index(void)
{
    return CLAMP((int)(look_current() - look_get(0)), 0, MAX_LOOKS - 1);
}

static void masks_path(char *out, size_t cap)
{
    path_join(user_home(), ".widgets", out, cap);
}

static unsigned parse_names(const char *p, size_t len, bool *ok)
{
    unsigned mask = 0;
    const char *end = p + len;
    *ok = true;
    if (len == 3 && !strncmp(p, "all", 3))
        return (1u << WIDGET_COUNT) - 1;
    if (len == 4 && !strncmp(p, "none", 4))
        return 0;
    while (p < end) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        size_t n = comma ? (size_t)(comma - p) : (size_t)(end - p);
        bool found = false;
        for (int i = 0; i < WIDGET_COUNT; i++)
            if (strlen(widget_names[i]) == n && !strncmp(widget_names[i], p, n)) {
                mask |= 1u << i;
                found = true;
            }
        *ok &= found;
        p += n + 1;
    }
    return mask;
}

static size_t format_names(unsigned mask, char *buf, size_t cap)
{
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < WIDGET_COUNT; i++)
        if (mask >> i & 1)
            n += (size_t)ksnprintf(buf + n, cap - n, "%s%s", n ? "," : "", widget_names[i]);
    if (!n)
        n = (size_t)ksnprintf(buf, cap, "none");
    return n;
}

/* Every look keeps its own set of widgets in ~/.widgets ("dune clock,weather,music"), starting from its own. */
static void masks_load(void)
{
    masks_loaded = true;
    for (int i = 0; i < look_count() && i < MAX_LOOKS; i++)
        look_masks[i] = look_get(i)->widgets;
    char path[PATH_MAX_LEN], *data;
    size_t size;
    masks_path(path, sizeof(path));
    if (vfs_read_all(path, &data, &size) != 0)
        return;
    for (char *p = data, *end = data + size; p < end;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *space = memchr(p, ' ', n);
        for (int i = 0; space && i < look_count() && i < MAX_LOOKS; i++)
            if ((size_t)(space - p) == strlen(look_get(i)->name) && !strncmp(p, look_get(i)->name, (size_t)(space - p))) {
                bool ok;
                unsigned m = parse_names(space + 1, n - (size_t)(space + 1 - p), &ok);
                if (ok)
                    look_masks[i] = m;
            }
        p += n + 1;
    }
    kfree(data);
}

static void masks_save(void)
{
    char path[PATH_MAX_LEN], buf[MAX_LOOKS * 96];
    size_t n = 0;
    for (int i = 0; i < look_count() && i < MAX_LOOKS; i++) {
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "%s ", look_get(i)->name);
        n += format_names(look_masks[i], buf + n, sizeof(buf) - n);
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "\n");
    }
    masks_path(path, sizeof(path));
    vfs_write_all(path, buf, n);
}

static unsigned current_mask(void)
{
    if (!masks_loaded)
        masks_load();
    return look_masks[look_index()];
}

static bool on(int id)
{
    return current_mask() >> id & 1;
}

static int month_days(int y, int m)
{
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return days[(m - 1) % 12];
}

static void tasks_path(char *out, size_t cap)
{
    path_join(user_home(), ".tasks", out, cap);
}

static void tasks_load(void)
{
    tasks.count = 0;
    tasks.loaded = true;
    char path[PATH_MAX_LEN], *data;
    size_t size;
    tasks_path(path, sizeof(path));
    if (vfs_read_all(path, &data, &size) != 0) {
        static const char *const first[] = {"Try the looks in Settings", "Play some music", "Write a note"};
        for (int i = 0; i < 3; i++)
            str_copy(tasks.text[tasks.count++], first[i], sizeof(tasks.text[0]));
        return;
    }
    char *p = data, *end = data + size;
    while (p < end && tasks.count < MAX_TASKS) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n >= 4 && p[0] == '[' && p[2] == ']') {
            tasks.done[tasks.count] = p[1] == 'x';
            size_t k = MIN(n - 4, sizeof(tasks.text[0]) - 1);
            memcpy(tasks.text[tasks.count], p + 4, k);
            tasks.text[tasks.count][k] = 0;
            tasks.count++;
        }
        p += n + 1;
    }
    kfree(data);
}

static void tasks_save(void)
{
    char path[PATH_MAX_LEN], buf[MAX_TASKS * 70];
    size_t n = 0;
    for (int i = 0; i < tasks.count; i++)
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "[%c] %s\n", tasks.done[i] ? 'x' : ' ', tasks.text[i]);
    tasks_path(path, sizeof(path));
    vfs_write_all(path, buf, n);
}

static int widget_height(int id)
{
    switch (id) {
    case WIDGET_CLOCK: return 150;
    case WIDGET_WEATHER: return 104;
    case WIDGET_MUSIC: return 124;
    case WIDGET_CALENDAR: return 150;
    case WIDGET_VISUALIZER: return 128;
    default: return 64 + MAX(tasks.count, 1) * TASK_ROW;
    }
}

/* Widgets stand in a column at the side of the screen away from the dock and wrap into a second column. */
static void place_all(struct rect *out)
{
    struct rect s = wm_screen_rect(0);
    const struct gui_look *l = look_current();
    bool left = l->dock == DOCK_RAIL;
    int top = left ? s.y + 22 : status_bottom() + 14;
    int bottom = s.y + s.h - (l->dock == DOCK_CENTER ? 100 : 16);
    int step = WIDGET_W + 16;
    int x = left ? s.x + shell_left_reserved() + 24 : s.x + s.w - 16 - WIDGET_W, y = top;
    right_columns = 0;
    for (int k = 0; k < WIDGET_COUNT; k++) {
        int id = column_order[k];
        out[id] = (struct rect){0, 0, 0, 0};
        if (!on(id))
            continue;
        int h = widget_height(id);
        if (y + h > bottom && y > top) {
            x += left ? step : -step;
            y = top;
        }
        out[id] = (struct rect){x, y, WIDGET_W, h};
        y += h + WIDGET_GAP;
        if (!left)
            right_columns = MAX(right_columns, (s.x + s.w - 16 - x) / step + 1);
    }
}

int widgets_obstacles(struct rect *out, int max)
{
    int n = 0;
    for (int i = 0; i < WIDGET_COUNT && n < max; i++)
        if (started && widgets[i].shown)
            out[n++] = widgets[i].r;
    return n;
}

int widgets_right_reserved(void)
{
    struct rect spots[WIDGET_COUNT];
    place_all(spots);
    return right_columns * (WIDGET_W + 16);
}

static void layout(void)
{
    struct rect spots[WIDGET_COUNT];
    place_all(spots);
    for (int i = 0; i < WIDGET_COUNT; i++) {
        struct widget *w = &widgets[i];
        struct rect old = w->r;
        bool was = w->shown;
        struct rect r = w->leaving ? (struct rect){w->ax.target, w->ay.target, old.w, old.h} : spots[i];
        w->shown = (on(i) || w->leaving) && !rect_empty(r);
        if (w->shown && !w->leaving) {
            if (!w->placed) {
                spring_set(&w->ax, r.x);
                spring_set(&w->ay, r.y);
                w->placed = true;
            }
            w->ax.target = r.x;
            w->ay.target = r.y;
        } else if (!w->shown) {
            w->placed = false;
        }
        int rise = (1024 - CLAMP(w->intro.pos, -400, 1024)) * 30 / 1024;
        int lift = CLAMP(w->lift.pos, -300, 1300) * 8 / 1024;
        w->r = (struct rect){w->ax.pos + w->dx, w->ay.pos + w->dy + rise - lift, r.w, r.h};
        w->card = i != WIDGET_CLOCK;
        if (w->shown && w->card) {
            w->glass.z = Z_DESKTOP + 1;
            glass_set_frame(&w->glass, w->r);
            if (!w->glass.registered)
                glass_register(&w->glass);
        } else if (w->glass.registered) {
            glass_unregister(&w->glass);
        }
        if (was || w->shown)
            wm_damage_z(rect_union(rect_inset(old, -30), rect_inset(w->r, -30)), Z_DESKTOP);
    }
}

static void shadow_text(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t col,
                        enum text_align a)
{
    gfx_text_box(c, (struct rect){r.x + 1, r.y + 2, r.w, r.h}, f, s, ALPHA(0, style.dark ? 140 : 70), a);
    gfx_text_box(c, r, f, s, col, a);
}

static void card(struct canvas *c, struct widget *w, int alpha)
{
    struct glass_look look;
    int radius = w->r.h < 60 ? w->r.h / 2 : MIN(PANEL_RADIUS, w->r.h / 3);
    glass_look_for(&look, GLASS_PANEL, radius);
    int lift = CLAMP(w->lift.pos, 0, 1024);
    if (lift > 0)
        gfx_drop_shadow(c, w->r, radius, 8, lift * (style.dark ? 60 : 22) / 1024, 2);
    glass_draw(c, &w->glass, &look);
    gfx_round_frame(c, w->r, radius, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 90));
}

static void paint_clock(struct canvas *c, struct rect r)
{
    struct rtc_time t;
    time_local(&t);
    char time[16], date[48];
    gui_clock_time(&t, time, sizeof(time), false, false);
    int m = CLAMP((int)t.month, 1, 12);
    ksnprintf(date, sizeof(date), "%s, %u %s", panel_day_name(panel_weekday((int)t.year, m, (int)t.day)), t.day,
              panel_month_name(m - 1));
    bool left = look_current()->dock == DOCK_RAIL;
    enum text_align a = left ? ALIGN_LEFT : ALIGN_RIGHT;
    uint32_t col = style.dark || look_current()->dock == DOCK_CORNER ? RGB(255, 255, 255) : style.text;
    shadow_text(c, (struct rect){r.x, r.y, r.w, 110}, &font_clock, time, col, a);
    shadow_text(c, (struct rect){r.x, r.y + 108, r.w, 30}, &font_title, date, ALPHA(col, 230), a);
}

static void sun_icon(struct canvas *c, int cx, int cy, int r, bool cloudy)
{
    gfx_glow(c, (struct rect){cx - r, cy - r, 2 * r, 2 * r}, r, r, 120, RGB(255, 190, 80));
    gfx_circle(c, cx, cy, r, RGB(255, 196, 70));
    gfx_circle(c, cx - r / 4, cy - r / 4, r / 2, ALPHA(0xffffffffu, 60));
    if (cloudy) {
        uint32_t cl = RGB(250, 250, 252);
        gfx_circle(c, cx + r / 2, cy + r / 2, r * 2 / 3, cl);
        gfx_circle(c, cx + r * 3 / 2, cy + r * 2 / 3, r / 2, cl);
        gfx_round(c, (struct rect){cx - r / 3, cy + r / 2, r * 2 + r / 3, r * 2 / 3}, r / 3, cl);
    }
}

static bool cloudy(void)
{
    return strstr(wx.cond, "loud") || strstr(wx.cond, "vercast") || strstr(wx.cond, "ain") || strstr(wx.cond, "ist") ||
           strstr(wx.cond, "og") || strstr(wx.cond, "now");
}

static void paint_weather(struct canvas *c, struct rect r)
{
    bool ok = wx.state == WX_DONE;
    const char *temp = ok ? wx.temp : "--°";
    const char *cond = ok ? wx.cond : wx.state == WX_FAILED ? "No connection" : "Updating…";
    if (r.h < 60) {
        sun_icon(c, r.x + 26, r.y + r.h / 2, 11, ok && cloudy());
        gfx_text_box(c, (struct rect){r.x + 48, r.y, 60, r.h}, &font_title, temp, ink(), ALIGN_LEFT);
        gfx_text_box(c, (struct rect){r.x + 96, r.y, r.w - 104, r.h}, &font_small, cond, soft(), ALIGN_LEFT);
        return;
    }
    sun_icon(c, r.x + 40, r.y + 44, 18, ok && cloudy());
    gfx_text_box(c, (struct rect){r.x + 80, r.y + 14, r.w - 90, 40}, &font_large, temp, ink(), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){r.x + 80, r.y + 54, r.w - 90, 18}, &font_bold, cond, ink(), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){r.x + 80, r.y + 74, r.w - 90, 16}, &font_small, ok ? wx.place : "wttr.in", soft(),
                 ALIGN_LEFT);
}

static void cover_art(struct canvas *c, struct rect r, int index)
{
    static const uint32_t tops[3] = {RGB(255, 120, 120), RGB(250, 196, 140), RGB(110, 150, 230)};
    static const uint32_t bottoms[3] = {RGB(90, 30, 120), RGB(150, 90, 120), RGB(30, 40, 90)};
    int i = index < 0 ? 2 : index % 3;
    gfx_round_gradient(c, r, 16, tops[i], bottoms[i]);
    struct canvas clip = *c;
    clip.clip = rect_intersect(c->clip, r);
    int cx = r.x + r.w / 2, cy = r.y + r.h * 58 / 100;
    gfx_circle(&clip, cx, cy, r.w / 4, ALPHA(RGB(255, 236, 160), 230));
    for (int k = 0; k < 4; k++)
        gfx_fill(&clip, (struct rect){r.x, cy + k * 5 - 2, r.w, 2 + k / 2}, ALPHA(bottoms[i], 200));
    gfx_fill(&clip, (struct rect){r.x, cy + 16, r.w, r.h}, ALPHA(bottoms[i], 220));
}

static struct rect music_button(struct rect r, int i)
{
    int cx = r.x + 112 + i * 44, cy = r.y + r.h - 32;
    return (struct rect){cx - 16, cy - 16, 32, 32};
}

static void fmt_time(char *buf, size_t cap, int ms)
{
    ksnprintf(buf, cap, "%d:%02d", ms / 60000, ms / 1000 % 60);
}

static void paint_music(struct canvas *c, struct rect r)
{
    const struct media_track *t = media_track();
    cover_art(c, (struct rect){r.x + 14, r.y + 14, 84, 84}, t->index);
    gfx_text_box(c, (struct rect){r.x + 112, r.y + 16, r.w - 124, 20}, &font_bold, t->title, ink(), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){r.x + 112, r.y + 36, r.w - 124, 16}, &font_small, t->artist, soft(), ALIGN_LEFT);
    struct rect bar = {r.x + 112, r.y + 60, r.w - 128, 4};
    gfx_round(c, bar, 2, ALPHA(ink(), 50));
    int fill = bar.w * CLAMP(progress.pos, 0, 1024) / 1024;
    if (fill > 0)
        gfx_round(c, (struct rect){bar.x, bar.y, fill, 4}, 2, style.accent);
    gfx_circle(c, bar.x + fill, bar.y + 2, 4, RGB(255, 255, 255));
    char a[12], b[12];
    fmt_time(a, sizeof(a), t->position_ms);
    fmt_time(b, sizeof(b), t->length_ms);
    gfx_text_box(c, (struct rect){bar.x, bar.y + 8, 60, 14}, &font_small, a, soft(), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){bar.x + bar.w - 60, bar.y + 8, 60, 14}, &font_small, b, soft(), ALIGN_RIGHT);
    static const enum symbol_id syms[3] = {SYM_PREV, SYM_PLAY, SYM_NEXT};
    for (int i = 0; i < 3; i++) {
        struct rect br = music_button(r, i);
        bool hot = hover == WIDGET_MUSIC && hover_part == i;
        bool main = i == 1;
        int rad = main ? 16 : 13;
        if (pressed == WIDGET_MUSIC && pressed_part == i)
            rad -= 2;
        if (main)
            gfx_circle(c, br.x + 16, br.y + 16, rad, hot ? color_shade(style.accent, 30) : style.accent);
        else if (hot)
            gfx_circle(c, br.x + 16, br.y + 16, rad, ALPHA(ink(), 30));
        enum symbol_id s = i == 1 && t->playing ? SYM_PAUSE : syms[i];
        gfx_symbol(c, br.x + 8, br.y + 8, s, 16, main ? style.accent_text : ink());
    }
}

static void paint_calendar(struct canvas *c, struct rect r)
{
    struct rtc_time t;
    time_local(&t);
    int m = CLAMP((int)t.month, 1, 12);
    char day[8], month[32];
    ksnprintf(day, sizeof(day), "%u", t.day);
    ksnprintf(month, sizeof(month), "%s %u", panel_month_name(m - 1), t.year);
    gfx_text_box(c, (struct rect){r.x + 20, r.y + 14, r.w - 40, 18}, &font_bold,
                 panel_day_name(panel_weekday((int)t.year, m, (int)t.day)), style.accent, ALIGN_LEFT);
    gfx_text_box(c, (struct rect){r.x + 16, r.y + 26, r.w - 32, 90}, &font_display, day, ink(), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){r.x + 20, r.y + 112, r.w - 40, 20}, &font_sans, month, soft(), ALIGN_LEFT);
    int first = (panel_weekday((int)t.year, m, 1) + 6) % 7;
    int week = (first + (int)t.day - 1) / 7;
    for (int i = 0; i < 7; i++) {
        int d = week * 7 + i - first + 1;
        if (d < 1 || d > month_days((int)t.year, m))
            continue;
        int cx = r.x + 112 + i * 17, cy = r.y + 64;
        char n[4];
        ksnprintf(n, sizeof(n), "%d", d);
        if (d == (int)t.day)
            gfx_circle(c, cx, cy, 9, style.accent);
        gfx_text_box(c, (struct rect){cx - 9, cy - 8, 18, 16}, &font_small, n, d == (int)t.day ? style.accent_text : soft(),
                     ALIGN_CENTER);
    }
}

static struct rect task_row(struct rect r, int i)
{
    return (struct rect){r.x + 12, r.y + 46 + i * TASK_ROW, r.w - 24, TASK_ROW};
}

static struct rect task_add(struct rect r)
{
    return (struct rect){r.x + r.w - 40, r.y + 10, 28, 28};
}

static void paint_tasks(struct canvas *c, struct rect r)
{
    if (!tasks.loaded)
        tasks_load();
    gfx_text_box(c, (struct rect){r.x + 20, r.y + 14, r.w - 70, 20}, &font_bold, "Now", ink(), ALIGN_LEFT);
    struct rect add = task_add(r);
    bool hot = hover == W_TASKS && hover_part == 100;
    gfx_circle(c, add.x + 14, add.y + 14, 13, hot ? style.accent : ALPHA(ink(), 28));
    gfx_symbol(c, add.x + 6, add.y + 6, SYM_PLUS, 16, hot ? style.accent_text : ink());
    for (int i = 0; i < tasks.count; i++) {
        struct rect row = task_row(r, i);
        bool rh = hover == W_TASKS && hover_part == i;
        if (rh)
            gfx_round(c, row, 10, ALPHA(ink(), 16));
        int cx = row.x + 16, cy = row.y + row.h / 2;
        if (tasks.done[i]) {
            gfx_circle(c, cx, cy, 8, style.accent);
            gfx_symbol(c, cx - 8, cy - 8, SYM_CHECK, 16, style.accent_text);
        } else {
            gfx_ring(c, cx, cy, 8, ALPHA(ink(), 120));
        }
        gfx_text_box(c, (struct rect){row.x + 34, row.y, row.w - 40, row.h}, &font_sans, tasks.text[i],
                     tasks.done[i] ? soft() : ink(), ALIGN_LEFT);
        if (tasks.done[i]) {
            int tw = MIN(gfx_text_width(&font_sans, tasks.text[i]), row.w - 40);
            gfx_fill(c, (struct rect){row.x + 34, cy, tw, 1}, soft());
        }
    }
    if (!tasks.count)
        gfx_text_box(c, task_row(r, 0), &font_sans, "Nothing planned", soft(), ALIGN_CENTER);
}

static void paint_visualizer(struct canvas *c, struct rect r)
{
    gfx_text_box(c, (struct rect){r.x + 18, r.y + 12, r.w - 36, 18}, &font_bold,
                 media_track()->playing ? media_track()->title : "Visualizer", soft(), ALIGN_LEFT);
    int n = MEDIA_BANDS, gap = 6;
    int bw = (r.w - 36 - gap * (n - 1)) / n;
    int base = r.y + r.h - 18, height = r.h - 50;
    for (int i = 0; i < n; i++) {
        int v = CLAMP(bars[i].pos, 0, 1100);
        int h = 4 + height * v / 1100;
        struct rect b = {r.x + 18 + i * (bw + gap), base - h, bw, h};
        gfx_round(c, b, MIN(bw / 2, 5), ALPHA(ink(), 150 + 80 * v / 1100));
    }
}

void widgets_paint(struct canvas *c, struct rect area)
{
    if (!started)
        return;
    for (int i = 0; i < WIDGET_COUNT; i++) {
        struct widget *w = &widgets[i];
        if (!w->shown || rect_empty(rect_intersect(rect_inset(w->r, -30), area)))
            continue;
        int alpha = CLAMP(w->intro.pos, 0, 1024) * 255 / 1024;
        if (alpha < 8)
            continue;
        if (w->card)
            card(c, w, alpha);
        struct canvas clip = *c;
        clip.clip = rect_intersect(c->clip, rect_inset(w->r, -4));
        switch (i) {
        case WIDGET_CLOCK: paint_clock(&clip, w->r); break;
        case WIDGET_WEATHER: paint_weather(&clip, w->r); break;
        case WIDGET_MUSIC: paint_music(&clip, w->r); break;
        case WIDGET_CALENDAR: paint_calendar(&clip, w->r); break;
        case WIDGET_VISUALIZER: paint_visualizer(&clip, w->r); break;
        case W_TASKS: paint_tasks(&clip, w->r); break;
        }
        int k = CLAMP(edit_pop.pos, 0, 1200);
        if (k > 16 && !w->leaving) {
            int rad = w->card ? MIN(PANEL_RADIUS, w->r.h / 3) : 18;
            gfx_round_frame(&clip, rect_inset(w->r, -2), rad + 2, ALPHA(ink(), MIN(k, 1024) * 70 / 1024));
            int br = 12 * k / 1024, cx = w->r.x + 12, cy = w->r.y + 12;
            gfx_circle(&clip, cx, cy, br, style.dark ? RGB(70, 68, 80) : RGB(60, 58, 64));
            gfx_fill(&clip, (struct rect){cx - br / 2, cy - 1, br, 2}, RGB(255, 255, 255));
        }
    }
}

static int widget_at(int x, int y)
{
    for (int i = WIDGET_COUNT - 1; i >= 0; i--)
        if (widgets[i].shown && rect_contains(widgets[i].r, x, y))
            return i;
    return -1;
}

static int part_at(int id, int x, int y)
{
    struct rect r = widgets[id].r;
    if (id == WIDGET_MUSIC) {
        for (int i = 0; i < 3; i++)
            if (rect_contains(rect_inset(music_button(r, i), -4), x, y))
                return i;
    } else if (id == W_TASKS) {
        if (rect_contains(task_add(r), x, y))
            return 100;
        for (int i = 0; i < tasks.count; i++)
            if (rect_contains(task_row(r, i), x, y))
                return i;
    }
    return -1;
}

static void damage(int id)
{
    if (id >= 0)
        wm_damage_z(rect_inset(widgets[id].r, -30), Z_DESKTOP);
}

static void task_added(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!value || !value[0] || tasks.count >= MAX_TASKS)
        return;
    str_copy(tasks.text[tasks.count], value, sizeof(tasks.text[0]));
    tasks.done[tasks.count++] = false;
    tasks_save();
    layout();
}

static void click(int id, int part, int clicks)
{
    if (id == WIDGET_MUSIC) {
        if (part == 0)
            media_prev();
        else if (part == 1)
            media_toggle();
        else if (part == 2)
            media_next();
        else if (clicks >= 2)
            gui_app_launch(gui_app_find("Music"), NULL);
    } else if (id == W_TASKS) {
        if (part == 100) {
            ui_prompt(NULL, "New task", "What do you want to do?", "", task_added);
        } else if (part >= 0) {
            tasks.done[part] = !tasks.done[part];
            tasks_save();
        }
    } else if (id == WIDGET_WEATHER && clicks >= 1) {
        if (wx.state == WX_FAILED || wx.state == WX_DONE)
            wx.next = hal_ticks_ms();
    } else if ((id == WIDGET_CLOCK || id == WIDGET_CALENDAR) && clicks >= 2) {
        struct rect s = wm_screen_rect(0);
        calendar_toggle(s.x + s.w - 12, status_bottom() + 8);
    }
    damage(id);
}

bool widgets_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    if (!started)
        return false;
    if (down) {
        int id = widget_at(x, y);
        if (editing && button == BUTTON_LEFT) {
            if (id < 0) {
                widgets_edit(false);
                return true;
            }
            int dx = x - (widgets[id].r.x + 12), dy = y - (widgets[id].r.y + 12);
            if (dx * dx + dy * dy <= 16 * 16) {
                set_mask(current_mask() & ~(1u << id));
                return true;
            }
        }
        if (id >= 0 && button == BUTTON_RIGHT) {
            static const char *const items[] = {"Edit Widgets…", "Remove Widget"};
            menu_widget = id;
            ui_menu(NULL, x, y, items, 2, widget_pick);
            return true;
        }
        if (id < 0 || button != BUTTON_LEFT)
            return id >= 0;
        pressed = id;
        pressed_part = part_at(id, x, y);
        drag = pressed_part < 0 ? id : -1;
        drag_x = x;
        drag_y = y;
        dragged = false;
        widgets[id].lift.target = 1400;
        damage(id);
        return true;
    }
    int id = pressed;
    if (id < 0)
        return widget_at(x, y) >= 0;
    pressed = -1;
    drag = -1;
    widgets[id].lift.target = 0;
    if (!dragged && widget_at(x, y) == id && part_at(id, x, y) == pressed_part)
        click(id, pressed_part, clicks);
    pressed_part = -1;
    damage(id);
    return true;
}

void desktop_hover(int x, int y, bool over)
{
    if (desktop_icon_drag(x, y))
        return;
    desktop_icon_hover(x, over ? y : -1);
    widgets_hover(over || drag >= 0 ? x : -1, y);
}

void widgets_hover(int x, int y)
{
    if (!started)
        return;
    if (drag >= 0) {
        struct widget *w = &widgets[drag];
        int mx = x - drag_x, my = y - drag_y;
        if (dragged || mx * mx + my * my > 16) {
            dragged = true;
            w->dx += mx;
            w->dy += my;
            drag_x = x;
            drag_y = y;
            layout();
        }
        return;
    }
    int id = x >= 0 ? widget_at(x, y) : -1;
    int part = id >= 0 ? part_at(id, x, y) : -1;
    if (id != hover) {
        damage(hover);
        hover = id;
        damage(id);
    }
    if (part != hover_part) {
        hover_part = part;
        damage(id);
    }
}

static void weather_fail(void)
{
    net_resolve_cancel(&wx.dns);
    if (wx.sock >= 0)
        net_close(wx.sock);
    wx.sock = -1;
    wx.state = WX_FAILED;
    wx.next = hal_ticks_ms() + 10 * 60000;
    damage(WIDGET_WEATHER);
}

static void weather_parse(void)
{
    wx.buf[wx.len] = 0;
    char *body = strstr(wx.buf, "\r\n\r\n");
    if (!body || strncmp(wx.buf, "HTTP/1.", 7) || strncmp(wx.buf + 9, "200", 3)) {
        weather_fail();
        return;
    }
    body += 4;
    char *bar1 = strchr(body, '|'), *bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
    if (!bar1 || !bar2) {
        weather_fail();
        return;
    }
    *bar1 = *bar2 = 0;
    char *nl = strchr(bar2 + 1, '\n');
    if (nl)
        *nl = 0;
    const char *t = body[0] == '+' ? body + 1 : body;
    char temp[16];
    size_t n = 0;
    for (; *t && n + 3 < sizeof(temp); t++)
        if ((*t >= '0' && *t <= '9') || *t == '-')
            temp[n++] = *t;
        else
            break;
    temp[n] = 0;
    ksnprintf(wx.temp, sizeof(wx.temp), "%s°", n ? temp : "--");
    str_copy(wx.cond, str_trim(bar1 + 1), sizeof(wx.cond));
    str_copy(wx.place, str_trim(bar2 + 1), sizeof(wx.place));
    char *comma = strchr(wx.place, ',');
    if (comma)
        *comma = 0;
    if (wx.place[0] >= 'a' && wx.place[0] <= 'z')
        wx.place[0] = (char)(wx.place[0] - 32);
    if (wx.sock >= 0)
        net_close(wx.sock);
    wx.sock = -1;
    wx.state = WX_DONE;
    wx.next = hal_ticks_ms() + 30 * 60000;
    damage(WIDGET_WEATHER);
}

static void weather_connect(uint32_t ip)
{
    wx.sock = net_socket(NET_TCP);
    if (wx.sock < 0 || net_connect_start(wx.sock, ip, 80) < 0) {
        weather_fail();
        return;
    }
    wx.state = WX_CONNECT;
    wx.sent = false;
    wx.len = 0;
}

static void weather_tick(void)
{
    if (!on(WIDGET_WEATHER))
        return;
    uint64_t now = hal_ticks_ms();
    if (wx.state == WX_IDLE || wx.state == WX_DONE || wx.state == WX_FAILED) {
        if (now < wx.next || now - started_at < 3000)
            return;
        if (!net_interface_count() || !net_interface(0)->configured) {
            wx.next = now + 5000;
            return;
        }
        uint32_t ip;
        int r = net_resolve_start("wttr.in", &wx.dns, &ip);
        if (r < 0) {
            weather_fail();
            return;
        }
        wx.started = now;
        wx.state = WX_DNS;
        if (r == 1)
            return;
        weather_connect(ip);
        return;
    }
    if (wx.state == WX_DNS) {
        uint32_t ip;
        net_poll();
        int r = net_resolve_poll(&wx.dns, &ip, 3000);
        if (r == 0)
            weather_connect(ip);
        else if (r != E_AGAIN)
            weather_fail();
        return;
    }
    if (now - wx.started > 15000) {
        weather_fail();
        return;
    }
    net_poll();
    if (wx.state == WX_CONNECT && net_connected(wx.sock)) {
        char req[256], city[96];
        size_t k = 0;
        for (const char *p = weather_city; *p && k + 4 < sizeof(city); p++)
            if (*p == ' ')
                city[k++] = '+';
            else
                city[k++] = *p;
        city[k] = 0;
        int n = ksnprintf(req, sizeof(req),
                          "GET /%s?format=%%t|%%C|%%l&m HTTP/1.0\r\nHost: wttr.in\r\nUser-Agent: curl/8.5\r\n"
                          "Accept: text/plain\r\n\r\n",
                          city);
        net_send(wx.sock, req, (size_t)n);
        wx.state = WX_RECV;
    } else if (wx.state == WX_RECV) {
        for (;;) {
            long r = net_recv(wx.sock, wx.buf + wx.len, sizeof(wx.buf) - 1 - (size_t)wx.len);
            if (r > 0) {
                wx.len += (int)r;
                if (wx.len >= (int)sizeof(wx.buf) - 1) {
                    weather_parse();
                    return;
                }
                continue;
            }
            if (r == 0)
                weather_parse();
            else if (r != E_AGAIN)
                weather_fail();
            return;
        }
    }
}

bool widgets_animate(void)
{
    if (!started)
        return false;
    uint64_t now = hal_ticks_ms();
    int dt = last_tick ? (int)MIN(now - last_tick, 64u) : 16;
    last_tick = now;
    bool moved = false;
    for (int i = 0; i < WIDGET_COUNT; i++) {
        struct widget *w = &widgets[i];
        moved |= spring_step(&w->lift, 500, 26, dt);
        moved |= spring_step(&w->intro, w->leaving ? 260 : 140, w->leaving ? 26 : 18, dt);
        moved |= spring_step(&w->ax, 240, 24, dt);
        moved |= spring_step(&w->ay, 240, 24, dt);
        if (w->leaving && w->intro.pos <= -300) {
            w->leaving = false;
            moved = true;
        }
    }
    if (spring_step(&edit_pop, 420, 18, dt)) {
        moved = true;
        for (int i = 0; i < WIDGET_COUNT; i++)
            damage(i);
    }
    if (moved)
        layout();
    if (media_tick() && (widgets[WIDGET_VISUALIZER].shown || widgets[WIDGET_MUSIC].shown)) {
        int lv[MEDIA_BANDS];
        media_levels(lv, MEDIA_BANDS);
        bool busy = false;
        for (int i = 0; i < MEDIA_BANDS; i++) {
            bars[i].target = lv[i];
            busy |= spring_step(&bars[i], 900, 30, dt);
        }
        const struct media_track *t = media_track();
        progress.target = t->length_ms ? (int)((int64_t)t->position_ms * 1024 / t->length_ms) : 0;
        busy |= spring_step(&progress, 300, 34, dt);
        static int shown_second;
        if (busy || t->position_ms / 1000 != shown_second) {
            shown_second = t->position_ms / 1000;
            if (widgets[WIDGET_VISUALIZER].shown)
                damage(WIDGET_VISUALIZER);
            if (widgets[WIDGET_MUSIC].shown)
                damage(WIDGET_MUSIC);
        }
    }
    struct rtc_time t;
    time_local(&t);
    if ((int)t.minute != last_minute) {
        last_minute = (int)t.minute;
        damage(WIDGET_CLOCK);
        damage(WIDGET_CALENDAR);
    }
    weather_tick();
    return moved;
}

void widgets_start(void)
{
    if (started)
        widgets_stop();
    started = true;
    started_at = last_tick = hal_ticks_ms();
    last_tick = 0;
    hover = pressed = drag = hover_part = pressed_part = -1;
    if (!tasks.loaded)
        tasks_load();
    for (int i = 0; i < WIDGET_COUNT; i++) {
        struct widget *w = &widgets[i];
        w->glass = (struct glass){0};
        w->shown = false;
        w->dx = w->dy = 0;
        w->r = (struct rect){0, 0, 0, 0};
        w->placed = w->leaving = false;
        spring_set(&w->lift, 0);
        spring_set(&w->intro, -600 - i * 220);
        w->intro.target = 1024;
    }
    layout();
}

void widgets_stop(void)
{
    if (!started)
        return;
    widgets_edit(false);
    started = false;
    for (int i = 0; i < WIDGET_COUNT; i++) {
        if (widgets[i].glass.registered)
            glass_unregister(&widgets[i].glass);
        if (widgets[i].shown)
            wm_damage_z(rect_inset(widgets[i].r, -30), Z_DESKTOP);
        widgets[i].shown = false;
    }
}

void widgets_user_changed(void)
{
    tasks.loaded = false;
    masks_loaded = false;
    if (started)
        layout();
}

static void calendar_paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    calendar_draw(c, (struct rect){0, 0, c->w, c->h});
}

/* The month with today marked, 66 + 6 * 30 pixels tall. */
void calendar_draw(struct canvas *c, struct rect at)
{
    struct canvas k = *c;
    k.clip = rect_intersect(c->clip, at);
    k.px += at.y * c->stride + at.x;
    k.w = at.w;
    k.h = at.h;
    k.clip = (struct rect){k.clip.x - at.x, k.clip.y - at.y, k.clip.w, k.clip.h};
    c = &k;
    struct rtc_time t;
    time_local(&t);
    int y = (int)t.year, m = CLAMP((int)t.month, 1, 12), d = (int)t.day;
    char title[40];
    ksnprintf(title, sizeof(title), "%s %d", panel_month_name(m - 1), y);
    gfx_text_box(c, (struct rect){18, 14, c->w - 36, 22}, &font_title, title, style.text, ALIGN_LEFT);
    static const char *const heads[7] = {"M", "T", "W", "T", "F", "S", "S"};
    int cell = (c->w - 28) / 7;
    for (int i = 0; i < 7; i++)
        gfx_text_box(c, (struct rect){14 + i * cell, 46, cell, 16}, &font_small, heads[i], style.secondary, ALIGN_CENTER);
    int first = (panel_weekday(y, m, 1) + 6) % 7;
    for (int day = 1; day <= month_days(y, m); day++) {
        int slot = first + day - 1;
        struct rect r = {14 + (slot % 7) * cell, 66 + (slot / 7) * 30, cell, 28};
        char num[4];
        ksnprintf(num, sizeof(num), "%d", day);
        if (day == d) {
            gfx_circle(c, r.x + r.w / 2, r.y + r.h / 2, 13, style.accent);
            gfx_text_box(c, r, &font_bold, num, style.accent_text, ALIGN_CENTER);
        } else {
            gfx_text_box(c, r, &font_sans, num, slot % 7 >= 5 ? style.secondary : style.text, ALIGN_CENTER);
        }
    }
}

static void calendar_event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
    else if (e->type == EV_KEY && e->key == 27)
        wm_destroy(w);
}

static void calendar_destroy(struct window *w)
{
    if (calendar == w)
        calendar = NULL;
}

static const struct window_ops calendar_ops = {calendar_paint, calendar_event, NULL, calendar_destroy};

void calendar_toggle(int right, int y)
{
    if (calendar && wm_window_alive(calendar)) {
        wm_destroy(calendar);
        return;
    }
    wm_close_popups();
    int w = 280, h = 66 + 6 * 30 + 12;
    calendar = wm_popup(right - w, y, w, h, &calendar_ops, NULL, NULL);
    if (calendar)
        calendar->radius = 20;
}

/* Edit mode: every widget gets a remove badge and a gallery at the bottom adds or removes any of them. */

#define TILE_W 88
#define GALLERY_H 112
#define DONE_W 88

static int gallery_hover = -1;

static struct rect tile_rect(int i)
{
    return (struct rect){16 + i * TILE_W, 8, TILE_W - 6, GALLERY_H - 16};
}

static struct rect done_rect(struct window *w)
{
    return (struct rect){w->frame.w - DONE_W - 16, (GALLERY_H - 36) / 2, DONE_W, 36};
}

static void gallery_paint(struct window *w, struct canvas *c)
{
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    unsigned mask = current_mask();
    for (int i = 0; i < WIDGET_COUNT; i++) {
        struct rect t = tile_rect(i);
        bool shown = mask >> i & 1;
        if (gallery_hover == i)
            gfx_round(c, t, 18, style.hover);
        int cx = t.x + t.w / 2, cy = t.y + 34;
        gfx_circle(c, cx, cy, 24, shown ? style.accent : ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 22));
        gfx_symbol(c, cx - 11, cy - 11, widget_syms[i], 22, shown ? style.accent_text : style.text);
        int bx = cx + 18, by = cy - 18;
        gfx_circle(c, bx, by, 9, shown ? RGB(255, 255, 255) : style.accent);
        gfx_symbol(c, bx - 7, by - 7, shown ? SYM_CHECK : SYM_PLUS, 14, shown ? style.accent : style.accent_text);
        gfx_text_box(c, (struct rect){t.x, t.y + 64, t.w, 18}, &font_sans, widget_titles[i], style.text, ALIGN_CENTER);
    }
    struct rect d = done_rect(w);
    gfx_round(c, d, 18, gallery_hover == 100 ? color_shade(style.accent, 24) : style.accent);
    gfx_text_box(c, d, &font_bold, "Done", style.accent_text, ALIGN_CENTER);
}

static int gallery_at(struct window *w, int x, int y)
{
    if (rect_contains(done_rect(w), x, y))
        return 100;
    for (int i = 0; i < WIDGET_COUNT; i++)
        if (rect_contains(tile_rect(i), x, y))
            return i;
    return -1;
}

static void gallery_event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_MOUSE_MOVE) {
        int h = gallery_at(w, e->x, e->y);
        if (h != gallery_hover) {
            gallery_hover = h;
            wm_invalidate(w);
        }
    } else if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
    } else if (e->type == EV_MOUSE_UP && e->button == BUTTON_LEFT) {
        e->handled = true;
        int h = gallery_at(w, e->x, e->y);
        if (h == 100) {
            widgets_edit(false);
        } else if (h >= 0) {
            set_mask(current_mask() ^ (1u << h));
            wm_invalidate(w);
        }
    } else if (e->type == EV_KEY && e->key == 27) {
        widgets_edit(false);
    }
}

static void gallery_destroy(struct window *w)
{
    if (gallery == w)
        gallery = NULL;
    if (editing) {
        editing = false;
        edit_pop.target = 0;
        wm_peek_desktop(false);
    }
}

static const struct window_ops gallery_ops = {gallery_paint, gallery_event, NULL, gallery_destroy};

void widgets_edit(bool on_)
{
    if (on_ == editing || (on_ && !started))
        return;
    editing = on_;
    if (on_ && edit_pop.pos <= 0)
        spring_set(&edit_pop, 0);
    edit_pop.target = on_ ? 1024 : 0;
    wm_peek_desktop(on_);
    if (!on_) {
        if (gallery && wm_window_alive(gallery))
            wm_destroy(gallery);
        gallery = NULL;
        return;
    }
    struct rect s = wm_screen_rect(0), spots[WIDGET_COUNT], used = {0, 0, 0, 0};
    place_all(spots);
    for (int i = 0; i < WIDGET_COUNT; i++)
        used = rect_empty(used) ? spots[i] : rect_empty(spots[i]) ? used : rect_union(used, spots[i]);
    int lo = s.x, hi = s.x + s.w;
    if (!rect_empty(used) && used.x > s.x + s.w / 2)
        hi = used.x - 8;
    else if (!rect_empty(used))
        lo = used.x + used.w + 8;
    int w = WIDGET_COUNT * TILE_W + 32 + DONE_W + 16, x = MAX(s.x + 8, lo + (hi - lo - w) / 2);
    gallery_hover = -1;
    wm_close_popups();
    gallery = wm_popup(x, s.y + s.h - GALLERY_H - 108, w, GALLERY_H, &gallery_ops, NULL, NULL);
    if (gallery) {
        gallery->radius = 28;
        gallery->flags |= WIN_PASSIVE;
    }
    for (int i = 0; i < WIDGET_COUNT; i++)
        damage(i);
}

bool widgets_editing(void)
{
    return editing;
}

COMMAND(widgets, NULL, "system", "widgets [list|add <name>|remove <name>|edit|done]",
        "add and remove desktop widgets (each look keeps its own)")
{
    if (argc == 1 || !strcmp(argv[1], "list")) {
        unsigned mask = current_mask();
        for (int i = 0; i < WIDGET_COUNT; i++) {
            sh_printf(sh, "%-11s", widget_names[i]);
            sh_role(sh, mask >> i & 1 ? ROLE_OK : ROLE_MUTED);
            sh_puts(sh, mask >> i & 1 ? "on the desktop\n" : "off\n");
            sh_role(sh, ROLE_TEXT);
        }
        return 0;
    }
    if (!strcmp(argv[1], "edit") || !strcmp(argv[1], "done")) {
        if (!started)
            return sh_error(sh, "widgets", "the desktop is not running");
        widgets_edit(!strcmp(argv[1], "edit"));
        return 0;
    }
    bool add = !strcmp(argv[1], "add");
    if (argc != 3 || (!add && strcmp(argv[1], "remove")))
        return command_usage(sh, command_find("widgets"));
    bool ok;
    unsigned bits = parse_names(argv[2], strlen(argv[2]), &ok);
    if (!ok)
        return sh_error(sh, "widgets", "no widget called %s (clock, weather, music, calendar, visualizer, tasks)", argv[2]);
    set_mask(add ? current_mask() | bits : current_mask() & ~bits);
    return 0;
}

static void set_mask(unsigned mask)
{
    unsigned old = current_mask();
    look_masks[look_index()] = mask;
    masks_save();
    if (!started)
        return;
    for (int i = 0; i < WIDGET_COUNT; i++) {
        struct widget *w = &widgets[i];
        if ((mask >> i & 1) && !(old >> i & 1)) {
            w->leaving = false;
            spring_set(&w->intro, -500);
            w->intro.target = 1024;
        } else if (!(mask >> i & 1) && (old >> i & 1) && w->shown) {
            w->leaving = true;
            w->intro.target = -400;
        }
    }
    layout();
    gui_desktop_changed();
}

static void get_widgets(char *buf, size_t cap)
{
    format_names(current_mask(), buf, cap);
}

static int set_widgets(struct shell *sh, const char *value)
{
    UNUSED(sh);
    bool ok;
    unsigned mask = parse_names(value, strlen(value), &ok);
    if (!ok)
        return E_INVAL;
    set_mask(mask);
    return 0;
}

static const struct setting s_widgets = {"widgets", "all|none|clock,weather,music,calendar,visualizer,tasks",
                                         "widgets on the desktop of the current look", get_widgets, set_widgets};
SETTING(s_widgets);

static void get_city(char *buf, size_t cap)
{
    str_copy(buf, weather_city[0] ? weather_city : "auto", cap);
}

static int set_city(struct shell *sh, const char *value)
{
    UNUSED(sh);
    str_copy(weather_city, strcmp(value, "auto") ? value : "", sizeof(weather_city));
    wx.next = 0;
    if (wx.state == WX_DONE)
        wx.state = WX_IDLE;
    return 0;
}

static const struct setting s_city = {"weathercity", "auto|<city>", "where the weather widget looks (auto: by address)",
                                      get_city, set_city};
SETTING(s_city);
