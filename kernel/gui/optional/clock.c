#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>
#include <synth/time.h>

#include "../internal.h"

/* Clock: world clock, alarms, stopwatch and timer. Alarms and the timer belong to the app, not its window: the
   desktop calls tick() while Clock is installed, so they ring with the window closed too. */

enum { TAB_WORLD, TAB_ALARMS, TAB_STOPWATCH, TAB_TIMER, TAB_COUNT };
#define MAX_ALARMS 8
#define MAX_CITIES 8
#define MAX_LAPS 40

struct city {
    const char *name;
    int offset; /* minutes from UTC, standard time */
};

static const struct city cities[] = {
    {"London", 0},       {"Reykjavik", 0},  {"Paris", 60},     {"Berlin", 60},     {"Rome", 60},
    {"Cairo", 120},      {"Kyiv", 120},     {"Istanbul", 180}, {"Moscow", 180},    {"Dubai", 240},
    {"Karachi", 300},    {"Delhi", 330},    {"Bangkok", 420},  {"Singapore", 480}, {"Beijing", 480},
    {"Tokyo", 540},      {"Sydney", 600},   {"Auckland", 720}, {"Honolulu", -600}, {"Los Angeles", -480},
    {"Denver", -420},    {"Chicago", -360}, {"New York", -300}, {"Sao Paulo", -180},
};

struct alarm {
    int minute; /* of the day */
    bool on;
};

static struct {
    bool loaded;
    struct alarm alarms[MAX_ALARMS];
    int alarm_count;
    int city[MAX_CITIES];
    int city_count;
    int fired_minute;
    /* stopwatch: running since start (ms), with what came before */
    bool sw_running;
    uint64_t sw_start, sw_before, laps[MAX_LAPS];
    int lap_count;
    /* timer: set length, and when it ends while running or how much was left when paused */
    int timer_set;
    bool timer_running;
    uint64_t timer_end, timer_left;
    struct window *win;
} k;

struct clock_win {
    struct ui_state st;
    int tab, selected;
};

static void save(void)
{
    char text[160] = "";
    for (int i = 0; i < k.alarm_count; i++)
        ksnprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%02d%02d%c", i ? "," : "",
                  k.alarms[i].minute / 60, k.alarms[i].minute % 60, k.alarms[i].on ? '+' : '-');
    app_state_set("clock", "alarms", text);
    text[0] = 0;
    for (int i = 0; i < k.city_count; i++)
        ksnprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%d", i ? "," : "", k.city[i]);
    app_state_set("clock", "cities", text);
    app_state_set_number("clock", "timer", k.timer_set);
}

static void load(void)
{
    if (k.loaded)
        return;
    k.loaded = true;
    k.fired_minute = -1;
    const char *a = app_state("clock", "alarms", "");
    while (*a && k.alarm_count < MAX_ALARMS) {
        int v = 0, n = 0;
        while (n < 4 && a[n] >= '0' && a[n] <= '9')
            v = v * 10 + a[n++] - '0';
        if (n == 4 && v / 100 < 24 && v % 100 < 60)
            k.alarms[k.alarm_count++] = (struct alarm){v / 100 * 60 + v % 100, a[4] != '-'};
        a = strchr(a, ',');
        if (!a)
            break;
        a++;
    }
    const char *c = app_state("clock", "cities", "0,15,22");
    while (*c && k.city_count < MAX_CITIES) {
        long v;
        char num[8];
        const char *comma = strchr(c, ',');
        size_t n = comma ? (size_t)(comma - c) : strlen(c);
        str_copy(num, c, MIN(n + 1, sizeof(num)));
        if (str_parse_long(num, &v) && v >= 0 && v < (long)ARRAY_SIZE(cities))
            k.city[k.city_count++] = (int)v;
        c += n + (c[n] ? 1 : 0);
    }
    k.timer_set = (int)CLAMP(app_state_number("clock", "timer", 300), 1, 99 * 3600);
}

static void ring(const char *what)
{
    gui_notify("Clock", what);
    hal_beep(880, 250);
    hal_beep(1175, 350);
}

static void tick(void)
{
    load();
    struct rtc_time t;
    time_local(&t);
    int minute = t.hour * 60 + t.minute;
    if (minute != k.fired_minute) {
        for (int i = 0; i < k.alarm_count; i++)
            if (k.alarms[i].on && k.alarms[i].minute == minute && t.second < 30) {
                char text[32];
                ksnprintf(text, sizeof(text), "Alarm %02d:%02d", minute / 60, minute % 60);
                k.fired_minute = minute;
                ring(text);
            }
    }
    if (k.timer_running && hal_ticks_ms() >= k.timer_end) {
        k.timer_running = false;
        k.timer_left = 0;
        ring("The timer is done");
        if (k.win)
            wm_invalidate(k.win);
    }
}

/* sin of a whole degree, scaled by 16384 */
static int sin_deg(int deg)
{
    static const uint16_t q[91] = {
        0,     286,   572,   857,   1143,  1428,  1713,  1997,  2280,  2563,  2845,  3126,  3406,  3686,  3964,  4240,
        4516,  4790,  5063,  5334,  5604,  5872,  6138,  6402,  6664,  6924,  7182,  7438,  7692,  7943,  8192,  8438,
        8682,  8923,  9162,  9397,  9630,  9860,  10087, 10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982,
        12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598,
        14726, 14849, 14968, 15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083,
        16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382, 16384};
    deg = ((deg % 360) + 360) % 360;
    return deg <= 90 ? q[deg] : deg <= 180 ? q[180 - deg] : deg <= 270 ? -q[deg - 180] : -q[360 - deg];
}

static void hand(struct canvas *c, int cx, int cy, int deg10, int len, int back, int width, uint32_t color)
{
    int deg = deg10 / 10;
    int s = sin_deg(deg), co = sin_deg(deg + 90);
    gfx_thick_line(c, cx - back * s / 16384, cy + back * co / 16384, cx + len * s / 16384, cy - len * co / 16384, width,
                   color);
}

static void dial(struct canvas *c, int cx, int cy, int r, const struct rtc_time *t, bool day)
{
    gfx_circle(c, cx, cy, r, day ? RGB(250, 250, 250) : RGB(28, 28, 30));
    gfx_ring(c, cx, cy, r, style.separator);
    uint32_t ink = day ? RGB(28, 28, 30) : RGB(240, 240, 240);
    for (int i = 0; i < 60; i++) {
        int s = sin_deg(i * 6), co = sin_deg(i * 6 + 90), in = i % 5 ? r - r / 12 : r - r / 6;
        gfx_thick_line(c, cx + in * s / 16384, cy - in * co / 16384, cx + (r - 4) * s / 16384,
                       cy - (r - 4) * co / 16384, i % 5 ? 1 : 3, i % 5 ? ALPHA(ink, 120) : ink);
    }
    hand(c, cx, cy, (t->hour % 12) * 300 + t->minute * 5, r * 5 / 10, r / 10, 5, ink);
    hand(c, cx, cy, t->minute * 60 + t->second, r * 8 / 10, r / 10, 3, ink);
    hand(c, cx, cy, t->second * 60, r * 9 / 10, r / 6, 1, RGB(255, 159, 10));
    gfx_circle(c, cx, cy, 4, RGB(255, 159, 10));
}

static void city_time(int offset, struct rtc_time *t)
{
    time_to_rtc((uint32_t)((int64_t)time_now() + offset * 60), t);
}

static void pick_city(struct window *owner, int index)
{
    UNUSED(owner);
    if (index < 0 || index >= (int)ARRAY_SIZE(cities) || k.city_count == MAX_CITIES)
        return;
    k.city[k.city_count++] = index;
    save();
    if (k.win)
        wm_invalidate(k.win);
}

static void world(struct ui *ui, struct clock_win *s, struct rect area)
{
    struct rtc_time now;
    time_local(&now);
    int r = MIN(area.h / 2 - 40, 110);
    int cx = area.x + 40 + r, cy = area.y + 24 + r;
    if (ui->c)
        dial(ui->c, cx, cy, r, &now, now.hour >= 7 && now.hour < 19);
    char text[64];
    ksnprintf(text, sizeof(text), "%02d:%02d:%02d", now.hour, now.minute, now.second);
    ui_label(ui, (struct rect){area.x + 24, cy + r + 12, 2 * r + 32, 34}, text, &font_large, style.text, ALIGN_CENTER);
    ksnprintf(text, sizeof(text), "%s, %d %s", time_weekday_name(time_weekday(&now)), now.day,
              time_month_name(now.month));
    ui_label(ui, (struct rect){area.x + 24, cy + r + 46, 2 * r + 32, 20}, text, &font_sans, style.secondary,
             ALIGN_CENTER);
    int x = cx + r + 40, w = area.x + area.w - x - 20, y = area.y + 20;
    ui_label(ui, (struct rect){x, y, w, 24}, "World Clock", &font_title, style.text, ALIGN_LEFT);
    y += 34;
    int local = time_zone();
    for (int i = 0; i < k.city_count && y + 44 < area.y + area.h - 44; i++) {
        const struct city *ct = &cities[k.city[i]];
        struct rtc_time t;
        city_time(ct->offset, &t);
        struct rect row = {x, y, w, 44};
        bool sel = s->selected == i;
        if (ui->c && sel)
            gfx_round(ui->c, row, 8, ALPHA(style.accent, 50));
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(row, ui->e->x, ui->e->y))
            s->selected = sel ? -1 : i;
        int diff = ct->offset - local;
        ksnprintf(text, sizeof(text), "%s%d:%02d h", diff < 0 ? "-" : "+", ABS(diff) / 60, ABS(diff) % 60);
        ui_label(ui, (struct rect){x + 10, y + 3, w - 100, 20}, ct->name, &font_bold, style.text, ALIGN_LEFT);
        ui_label(ui, (struct rect){x + 10, y + 22, w - 100, 18}, diff ? text : "Same time", &font_sans, style.secondary,
                 ALIGN_LEFT);
        ksnprintf(text, sizeof(text), "%02d:%02d", t.hour, t.minute);
        ui_label(ui, (struct rect){x + w - 100, y, 90, 44}, text, &font_large, style.text, ALIGN_RIGHT);
        y += 48;
    }
    struct rect add = {x, area.y + area.h - 40, 90, 28};
    if (ui_button(ui, add, "Add City") && k.city_count < MAX_CITIES) {
        static const char *names[ARRAY_SIZE(cities)];
        for (size_t i = 0; i < ARRAY_SIZE(cities); i++)
            names[i] = cities[i].name;
        ui_menu(ui->win, add.x, add.y - 8, names, (int)ARRAY_SIZE(cities), pick_city);
    }
    if (ui_button(ui, (struct rect){x + 100, add.y, 90, 28}, "Remove") && s->selected >= 0 &&
        s->selected < k.city_count) {
        memmove(&k.city[s->selected], &k.city[s->selected + 1], sizeof(int) * (size_t)(k.city_count - s->selected - 1));
        k.city_count--;
        s->selected = -1;
        save();
    }
}

static bool parse_time(const char *text, int *minute)
{
    int h = 0, m = 0, digits = 0;
    const char *p = text;
    for (; *p >= '0' && *p <= '9' && digits < 2; p++, digits++)
        h = h * 10 + *p - '0';
    if (!digits || (*p != ':' && *p != '.' && *p != ' '))
        return false;
    p++;
    digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 2; p++, digits++)
        m = m * 10 + *p - '0';
    if (digits != 2 || h > 23 || m > 59)
        return false;
    *minute = h * 60 + m;
    return true;
}

static void add_alarm(struct window *owner, const char *value)
{
    UNUSED(owner);
    int minute;
    if (!value || !parse_time(value, &minute)) {
        if (value)
            gui_notify("Clock", "Type the time as HH:MM, for example 07:30");
        return;
    }
    if (k.alarm_count == MAX_ALARMS)
        return;
    int i = k.alarm_count++;
    while (i > 0 && k.alarms[i - 1].minute > minute) {
        k.alarms[i] = k.alarms[i - 1];
        i--;
    }
    k.alarms[i] = (struct alarm){minute, true};
    save();
    if (k.win)
        wm_invalidate(k.win);
}

static void alarms(struct ui *ui, struct clock_win *s, struct rect area)
{
    int x = area.x + 24, w = area.w - 48, y = area.y + 20;
    ui_label(ui, (struct rect){x, y, w, 24}, "Alarms", &font_title, style.text, ALIGN_LEFT);
    y += 36;
    if (!k.alarm_count)
        ui_label(ui, (struct rect){x, y + 40, w, 20}, "No alarms. They ring even when Clock is closed.", &font_sans,
                 style.secondary, ALIGN_CENTER);
    for (int i = 0; i < k.alarm_count; i++) {
        struct rect row = {x, y, w, 56};
        if (ui->c)
            gfx_round(ui->c, row, 10, s->selected == i ? ALPHA(style.accent, 50) : style.group);
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains((struct rect){x, y, w - 80, 56}, ui->e->x, ui->e->y))
            s->selected = s->selected == i ? -1 : i;
        char text[16];
        ksnprintf(text, sizeof(text), "%02d:%02d", k.alarms[i].minute / 60, k.alarms[i].minute % 60);
        ui_label(ui, (struct rect){x + 16, y, 160, 56}, text, &font_large, k.alarms[i].on ? style.text : style.tertiary,
                 ALIGN_LEFT);
        ui_label(ui, (struct rect){x + 120, y, 200, 56}, "Every day", &font_sans, style.secondary, ALIGN_LEFT);
        if (ui_switch(ui, (struct rect){x + w - 66, y + 16, 50, 26}, &k.alarms[i].on))
            save();
        y += 62;
    }
    int by = area.y + area.h - 44;
    if (ui_button_primary(ui, (struct rect){x, by, 110, 28}, "New Alarm") && k.alarm_count < MAX_ALARMS)
        ui_prompt(ui->win, "New Alarm", "Time (HH:MM)", "07:00", add_alarm);
    if (ui_button(ui, (struct rect){x + 120, by, 90, 28}, "Delete") && s->selected >= 0 && s->selected < k.alarm_count) {
        memmove(&k.alarms[s->selected], &k.alarms[s->selected + 1],
                sizeof(k.alarms[0]) * (size_t)(k.alarm_count - s->selected - 1));
        k.alarm_count--;
        s->selected = -1;
        save();
    }
}

static uint64_t sw_elapsed(void)
{
    return k.sw_before + (k.sw_running ? hal_ticks_ms() - k.sw_start : 0);
}

static void format_ms(char *out, size_t cap, uint64_t ms, bool hundredths)
{
    unsigned s = (unsigned)(ms / 1000), h = s / 3600;
    if (hundredths)
        ksnprintf(out, cap, h ? "%u:%02u:%02u.%02u" : "%02u:%02u.%02u", h ? h : s / 60 % 60, h ? s / 60 % 60 : s % 60,
                  h ? s % 60 : (unsigned)(ms / 10 % 100), (unsigned)(ms / 10 % 100));
    else
        ksnprintf(out, cap, h ? "%u:%02u:%02u" : "%02u:%02u", h ? h : s / 60, h ? s / 60 % 60 : s % 60, s % 60);
}

static void stopwatch(struct ui *ui, struct rect area)
{
    char text[32];
    uint64_t ms = sw_elapsed();
    format_ms(text, sizeof(text), ms, true);
    ui_label(ui, (struct rect){area.x, area.y + 30, area.w, 80}, text, &font_display, style.text, ALIGN_CENTER);
    int cx = area.x + area.w / 2;
    if (ui_button(ui, (struct rect){cx - 130, area.y + 130, 120, 32}, k.sw_running ? "Lap" : "Reset")) {
        if (k.sw_running && k.lap_count < MAX_LAPS) {
            memmove(&k.laps[1], &k.laps[0], sizeof(k.laps[0]) * (size_t)k.lap_count);
            k.laps[0] = ms;
            k.lap_count++;
        } else if (!k.sw_running) {
            k.sw_before = 0;
            k.lap_count = 0;
        }
    }
    if (ui_button_primary(ui, (struct rect){cx + 10, area.y + 130, 120, 32}, k.sw_running ? "Stop" : "Start")) {
        if (k.sw_running)
            k.sw_before = ms;
        k.sw_start = hal_ticks_ms();
        k.sw_running = !k.sw_running;
    }
    int y = area.y + 184;
    for (int i = 0; i < k.lap_count && y + 26 < area.y + area.h; i++) {
        uint64_t lap = k.laps[i] - (i + 1 < k.lap_count ? k.laps[i + 1] : 0);
        char name[16];
        ksnprintf(name, sizeof(name), "Lap %d", k.lap_count - i);
        format_ms(text, sizeof(text), lap, true);
        ui_separator(ui, area.x + 60, y, area.w - 120);
        ui_label(ui, (struct rect){area.x + 70, y, 140, 26}, name, &font_sans, style.text, ALIGN_LEFT);
        ui_label(ui, (struct rect){area.x + area.w - 230, y, 160, 26}, text, &font_mono, style.text, ALIGN_RIGHT);
        y += 28;
    }
}

static void timer(struct ui *ui, struct rect area)
{
    char text[32];
    int cx = area.x + area.w / 2, cy = area.y + 120, r = 92;
    uint64_t left = k.timer_running ? (k.timer_end > hal_ticks_ms() ? k.timer_end - hal_ticks_ms() : 0)
                                    : k.timer_left ? k.timer_left : (uint64_t)k.timer_set * 1000;
    if (ui->c) {
        gfx_circle(ui->c, cx, cy, r, style.group);
        int done = (int)(360 - left * 360 / ((uint64_t)k.timer_set * 1000 + 1));
        for (int d = done; d < 360; d += 2) {
            int s = sin_deg(d), co = sin_deg(d + 90);
            gfx_circle(ui->c, cx + (r - 6) * s / 16384, cy - (r - 6) * co / 16384, 4, RGB(255, 159, 10));
        }
        gfx_circle(ui->c, cx, cy, r - 14, style.content);
    }
    format_ms(text, sizeof(text), left + 999, false);
    ui_label(ui, (struct rect){cx - r, cy - 30, 2 * r, 60}, text, &font_large, style.text, ALIGN_CENTER);
    bool idle = !k.timer_running && !k.timer_left;
    int y = cy + r + 20;
    if (idle) {
        static const int presets[] = {60, 180, 300, 600, 900, 1800, 3600};
        static const char *const names[] = {"1 min", "3 min", "5 min", "10 min", "15 min", "30 min", "1 hour"};
        int bw = 70, x = cx - (int)ARRAY_SIZE(presets) * (bw + 6) / 2;
        for (size_t i = 0; i < ARRAY_SIZE(presets); i++)
            if (ui_button(ui, (struct rect){x + (int)i * (bw + 6), y, bw, 26}, names[i])) {
                k.timer_set = presets[i];
                save();
            }
        y += 34;
        if (ui_button(ui, (struct rect){cx - 150, y, 70, 28}, "-1 min") && k.timer_set > 60) {
            k.timer_set -= 60;
            save();
        }
        if (ui_button(ui, (struct rect){cx + 80, y, 70, 28}, "+1 min") && k.timer_set < 99 * 3600) {
            k.timer_set += 60;
            save();
        }
    }
    if (ui_button_primary(ui, (struct rect){cx - 60, y, 120, 28}, k.timer_running ? "Pause" : idle ? "Start" : "Resume")) {
        if (k.timer_running) {
            k.timer_left = left;
        } else {
            k.timer_end = hal_ticks_ms() + (k.timer_left ? k.timer_left : (uint64_t)k.timer_set * 1000);
            k.timer_left = 0;
        }
        k.timer_running = !k.timer_running;
        if (!k.timer_running && !k.timer_left)
            k.timer_left = 1;
    }
    if (!idle && ui_button(ui, (struct rect){cx - 60, y + 36, 120, 28}, "Cancel")) {
        k.timer_running = false;
        k.timer_left = 0;
    }
}

static const char *const tab_names[TAB_COUNT] = {"World Clock", "Alarms", "Stopwatch", "Timer"};

static void frame(struct ui *ui, void *data)
{
    struct clock_win *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->e && ui->e->type == EV_KEY)
        for (int i = 0; i < TAB_COUNT; i++)
            if (ui->e->key == KEY_COMMAND(i + 1)) {
                s->tab = i;
                ui->consumed = true;
            }
    ui_panel(ui, (struct rect){0, 0, W, H}, style.content);
    int old = s->tab;
    ui_segmented(ui, (struct rect){W / 2 - 220, 12, 440, 28}, tab_names, TAB_COUNT, &s->tab);
    if (s->tab != old) {
        s->selected = -1;
        app_state_set_number("clock", "tab", s->tab);
    }
    struct rect area = {0, 48, W, H - 48};
    switch (s->tab) {
    case TAB_WORLD: world(ui, s, area); break;
    case TAB_ALARMS: alarms(ui, s, area); break;
    case TAB_STOPWATCH: stopwatch(ui, area); break;
    default: timer(ui, area); break;
    }
    if (ui->c)
        wm_set_timer(ui->win, s->tab == TAB_STOPWATCH && k.sw_running ? 50 : s->tab == TAB_TIMER ? 250 : 1000);
}

static void paint(struct window *w, struct canvas *c)
{
    struct clock_win *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct clock_win *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    if (k.win == w)
        k.win = NULL;
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    load();
    if (k.win && wm_window_alive(k.win)) {
        wm_show(k.win);
        return;
    }
    struct clock_win *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    s->selected = -1;
    s->tab = (int)CLAMP(app_state_number("clock", "tab", 0), 0, TAB_COUNT - 1);
    for (int i = 0; arg && i < TAB_COUNT; i++)
        if (!str_casecmp(arg, tab_names[i]) || (i == TAB_TIMER && !str_casecmp(arg, "timer")))
            s->tab = i;
    k.win = wm_open("Clock", ICON_CLOCK, 640, 460, &ops, s, WIN_RESIZABLE);
    if (!k.win) {
        kfree(s);
        return;
    }
    wm_set_timer(k.win, 1000);
}

static const struct gui_app clock_app = {
    .name = "Clock", .description = "World clock, alarms, stopwatch and timer", .icon = ICON_CLOCK, .launch = launch};
GUI_APP_TICK(clock_app, tick);
