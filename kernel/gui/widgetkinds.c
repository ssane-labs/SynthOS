#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "widgets.h"

/* What each kind of widget shows at each size, which parts of it take clicks, and the data behind it: the
   weather from wttr.in, the tasks in ~/.tasks, photos from a folder, the music player and the system. */

#define MAX_TASKS 8

static struct {
    char text[MAX_TASKS][64];
    bool done[MAX_TASKS];
    int count;
    bool loaded;
} tasks;

static struct spring bars[MEDIA_BANDS], progress;
static uint64_t last_tick, started_at;
static int last_minute = -1, last_second = -1;
static struct window *calendar;

static void damage_kind(int kind);
static bool any_kind(int kind);

static int month_days(int y, int m)
{
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return days[(m - 1) % 12];
}

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

static void weather_fail(void)
{
    net_resolve_cancel(&wx.dns);
    if (wx.sock >= 0)
        net_close(wx.sock);
    wx.sock = -1;
    wx.state = WX_FAILED;
    wx.next = hal_ticks_ms() + 10 * 60000;
    damage_kind(WK_WEATHER);
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
    damage_kind(WK_WEATHER);
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
    if (!any_kind(WK_WEATHER))
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

/* ---- shared bits */


static void damage_kind(int kind)
{
    int n;
    struct widget *ws = widget_list(&n);
    for (int i = 0; i < n; i++)
        if (ws[i].used && ws[i].kind == kind)
            widget_damage(&ws[i]);
}

static bool any_kind(int kind)
{
    int n;
    struct widget *ws = widget_list(&n);
    for (int i = 0; i < n; i++)
        if (ws[i].used && !ws[i].leaving && ws[i].kind == kind)
            return true;
    return false;
}

static bool hot(const struct widget *w, int part)
{
    return widget_hover_w == w && widget_hover_part == part;
}

static bool down(const struct widget *w, int part)
{
    return widget_pressed_w == w && widget_pressed_part == part;
}

static void fmt_time(char *buf, size_t cap, int ms)
{
    ksnprintf(buf, cap, "%d:%02d", ms / 60000, ms / 1000 % 60);
}

void widget_default_opt(struct widget *w)
{
    w->opt[0] = 0;
    switch (w->kind) {
    case WK_WORLD: str_copy(w->opt, "Tokyo|540", sizeof(w->opt)); break;
    case WK_APPS: str_copy(w->opt, "Files|Terminal|Music|Photos", sizeof(w->opt)); break;
    case WK_NOTES: str_copy(w->opt, "Right-click the widget and choose Edit Widget to write here.", sizeof(w->opt)); break;
    default: break;
    }
}

void widget_free_kind(struct widget *w)
{
    canvas_free(&w->pic);
}

/* ---- clocks */

static void paint_clock(struct canvas *c, struct widget *w, struct rect r)
{
    struct rtc_time t;
    time_local(&t);
    char time[16], date[48];
    gui_clock_time(&t, time, sizeof(time), false, false);
    int m = CLAMP((int)t.month, 1, 12);
    ksnprintf(date, sizeof(date), "%s, %u %s", panel_day_name(panel_weekday((int)t.year, m, (int)t.day)), t.day,
              panel_month_name(m - 1));
    uint32_t ink = widget_ink(w);
    switch (w->size) {
    case WS_SLIM:
        widget_text(c, w, (struct rect){r.x + 20, r.y, r.w / 2, r.h}, &font_large, time, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + r.w / 2, r.y, r.w / 2 - 20, r.h}, &font_sans, date, widget_soft(w),
                    ALIGN_RIGHT);
        break;
    case WS_SMALL:
        widget_text(c, w, (struct rect){r.x, r.y + r.h / 2 - 44, r.w, 60}, &font_display, time, ink, ALIGN_CENTER);
        widget_text(c, w, (struct rect){r.x + 8, r.y + r.h / 2 + 18, r.w - 16, 20}, &font_small, date,
                    widget_soft(w), ALIGN_CENTER);
        break;
    default: {
        int y = r.y + (r.h - 146) / 2;
        widget_text(c, w, (struct rect){r.x, y, r.w, 112}, &font_clock, time, ink, ALIGN_CENTER);
        widget_text(c, w, (struct rect){r.x, y + 112, r.w, 30}, &font_title, date, ALPHA(ink, 230), ALIGN_CENTER);
        if (w->size == WS_LARGE) {
            int day = (int)t.hour * 60 + (int)t.minute;
            struct rect bar = {r.x + 40, r.y + r.h - 50, r.w - 80, 6};
            gfx_round(c, bar, 3, ALPHA(ink, 40));
            gfx_round(c, (struct rect){bar.x, bar.y, MAX(bar.w * day / 1440, 6), 6}, 3, style.accent);
            char left[32];
            ksnprintf(left, sizeof(left), "%d%% of the day", day * 100 / 1440);
            widget_text(c, w, (struct rect){bar.x, bar.y + 12, bar.w, 16}, &font_small, left, widget_soft(w),
                        ALIGN_CENTER);
        }
    }
    }
}

static const int16_t sin_t[91] = {
    0,    18,   36,   54,   71,   89,   107,  125,  143,  160,  178,  195,  213,  230,  248,  265,  282,  299,
    316,  333,  350,  367,  384,  400,  416,  433,  449,  465,  481,  496,  512,  527,  543,  558,  573,  587,
    602,  616,  630,  644,  658,  672,  685,  698,  711,  724,  737,  749,  761,  773,  784,  796,  807,  818,
    828,  839,  849,  859,  868,  878,  887,  896,  904,  912,  920,  928,  935,  943,  949,  956,  962,  968,
    974,  979,  984,  989,  994,  998,  1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024};

/* A point len away from the center, deg10 tenths of a degree clockwise from twelve o'clock. */
static void polar(int cx, int cy, int deg10, int len, int *x, int *y)
{
    int a = (deg10 / 10) % 360, q = a / 90, k = a % 90;
    int s = q == 0 ? sin_t[k] : q == 1 ? sin_t[90 - k] : q == 2 ? -sin_t[k] : -sin_t[90 - k];
    int c = q == 0 ? sin_t[90 - k] : q == 1 ? -sin_t[k] : q == 2 ? -sin_t[90 - k] : sin_t[k];
    *x = cx + s * len / 1024;
    *y = cy - c * len / 1024;
}

static void hand(struct canvas *c, int cx, int cy, int deg10, int from, int len, int width, uint32_t col)
{
    int x0, y0, x1, y1;
    polar(cx, cy, deg10, from, &x0, &y0);
    polar(cx, cy, deg10, len, &x1, &y1);
    gfx_thick_line(c, x0, y0, x1, y1, width, col);
}

static void paint_analog(struct canvas *c, struct widget *w, struct rect r)
{
    struct rtc_time t;
    time_local(&t);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = MIN(r.w, r.h) / 2 - 6;
    uint32_t ink = widget_ink(w);
    if (w->shape != SH_ROUND)
        gfx_circle(c, cx, cy, rad, ALPHA(ink, 20));
    for (int i = 0; i < 60; i++) {
        bool hour = i % 5 == 0;
        if (!hour && w->size == WS_SMALL)
            continue;
        hand(c, cx, cy, i * 60, rad - (hour ? rad / 7 : rad / 16), rad - 2, hour ? 3 : 1, ALPHA(ink, hour ? 220 : 110));
    }
    if (w->size == WS_LARGE) {
        static const char *const nums[4] = {"12", "3", "6", "9"};
        for (int i = 0; i < 4; i++) {
            int px, py;
            polar(cx, cy, i * 900, rad - 46, &px, &py);
            widget_text(c, w, (struct rect){px - 20, py - 14, 40, 28}, &font_title, nums[i], ink, ALIGN_CENTER);
        }
    }
    int hdeg = ((int)t.hour % 12) * 300 + (int)t.minute * 5, mdeg = (int)t.minute * 60 + (int)t.second;
    hand(c, cx, cy, hdeg, 0, rad * 5 / 10, MAX(4, rad / 14), ink);
    hand(c, cx, cy, mdeg, 0, rad * 8 / 10, MAX(3, rad / 20), ink);
    hand(c, cx, cy, (int)t.second * 60, -rad / 8, rad * 9 / 10, 2, RGB(255, 149, 0));
    gfx_circle(c, cx, cy, MAX(4, rad / 16), RGB(255, 149, 0));
}

static void world_entry(const char *opt, char *city, size_t cap, int *offset)
{
    const char *bar = strchr(opt, '|');
    size_t n = bar ? MIN((size_t)(bar - opt), cap - 1) : 0;
    memcpy(city, opt, n);
    city[n] = 0;
    long v = 0;
    if (bar)
        str_parse_long(bar + 1, &v);
    *offset = (int)v;
    if (!n)
        str_copy(city, "UTC", cap);
}

static void paint_world(struct canvas *c, struct widget *w, struct rect r)
{
    char city[48], time[16], diff[24];
    int offset;
    world_entry(w->opt, city, sizeof(city), &offset);
    struct rtc_time t;
    time_to_rtc(time_now() + (uint32_t)(offset * 60), &t);
    gui_clock_time(&t, time, sizeof(time), false, false);
    int hours = (offset - time_zone()) / 60, mins = ABS((offset - time_zone()) % 60);
    if (mins)
        ksnprintf(diff, sizeof(diff), "%+dh %02dm", hours, mins);
    else
        ksnprintf(diff, sizeof(diff), hours ? "%+d hours" : "Same time", hours);
    uint32_t ink = widget_ink(w);
    bool night = t.hour < 7 || t.hour >= 20;
    if (w->size == WS_SLIM) {
        gfx_symbol(c, r.x + 20, r.y + r.h / 2 - 8, night ? SYM_MOON : SYM_SUN, 16, ink);
        widget_text(c, w, (struct rect){r.x + 46, r.y, r.w / 2, r.h}, &font_bold, city, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + r.w / 2, r.y, r.w / 2 - 20, r.h}, &font_large, time, ink, ALIGN_RIGHT);
        return;
    }
    gfx_symbol(c, r.x + 18, r.y + 18, night ? SYM_MOON : SYM_SUN, 16, ink);
    widget_text(c, w, (struct rect){r.x + 42, r.y + 14, r.w - 56, 22}, &font_bold, city, ink, ALIGN_LEFT);
    widget_text(c, w, (struct rect){r.x + 16, r.y + r.h / 2 - 26, r.w - 32, 60}, &font_display, time, ink,
                ALIGN_LEFT);
    widget_text(c, w, (struct rect){r.x + 18, r.y + r.h - 34, r.w - 36, 18}, &font_small, diff, widget_soft(w),
                ALIGN_LEFT);
    if (w->size == WS_MEDIUM) {
        struct rtc_time here;
        time_local(&here);
        char now[16];
        gui_clock_time(&here, now, sizeof(now), false, false);
        widget_text(c, w, (struct rect){r.x + r.w / 2, r.y + 14, r.w / 2 - 18, 22}, &font_bold, "Here", ink,
                    ALIGN_RIGHT);
        widget_text(c, w, (struct rect){r.x + r.w / 2, r.y + r.h / 2 - 26, r.w / 2 - 18, 60}, &font_display, now,
                    widget_soft(w), ALIGN_RIGHT);
    }
}

/* ---- weather */

static bool cloudy(void)
{
    return strstr(wx.cond, "loud") || strstr(wx.cond, "vercast") || strstr(wx.cond, "ain") || strstr(wx.cond, "ist") ||
           strstr(wx.cond, "og") || strstr(wx.cond, "now");
}

static void sun_icon(struct canvas *c, int cx, int cy, int r, bool clouds)
{
    gfx_glow(c, (struct rect){cx - r, cy - r, 2 * r, 2 * r}, r, r, 120, RGB(255, 190, 80));
    gfx_circle(c, cx, cy, r, RGB(255, 196, 70));
    gfx_circle(c, cx - r / 4, cy - r / 4, r / 2, ALPHA(0xffffffffu, 60));
    if (clouds) {
        uint32_t cl = RGB(250, 250, 252);
        gfx_circle(c, cx + r / 2, cy + r / 2, r * 2 / 3, cl);
        gfx_circle(c, cx + r * 3 / 2, cy + r * 2 / 3, r / 2, cl);
        gfx_round(c, (struct rect){cx - r / 3, cy + r / 2, r * 2 + r / 3, r * 2 / 3}, r / 3, cl);
    }
}

static void paint_weather(struct canvas *c, struct widget *w, struct rect r)
{
    bool ok = wx.state == WX_DONE;
    const char *temp = ok ? wx.temp : "--°";
    const char *cond = ok ? wx.cond : wx.state == WX_FAILED ? "No connection" : "Updating…";
    const char *place = ok ? wx.place : "wttr.in";
    uint32_t ink = widget_ink(w), soft = widget_soft(w);
    switch (w->size) {
    case WS_SLIM:
        sun_icon(c, r.x + 30, r.y + r.h / 2, 13, ok && cloudy());
        widget_text(c, w, (struct rect){r.x + 60, r.y, 80, r.h}, &font_large, temp, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + 140, r.y + r.h / 2 - 18, r.w - 156, 18}, &font_bold, cond, ink,
                    ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + 140, r.y + r.h / 2, r.w - 156, 16}, &font_small, place, soft,
                    ALIGN_LEFT);
        break;
    case WS_SMALL:
        widget_text(c, w, (struct rect){r.x + 16, r.y + 12, r.w - 32, 20}, &font_bold, place, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + 14, r.y + 34, r.w - 28, 60}, &font_display, temp, ink, ALIGN_LEFT);
        sun_icon(c, r.x + 30, r.y + r.h - 44, 12, ok && cloudy());
        widget_text(c, w, (struct rect){r.x + 16, r.y + r.h - 26, r.w - 32, 16}, &font_small, cond, soft, ALIGN_LEFT);
        break;
    default:
        sun_icon(c, r.x + 50, r.y + 54, 24, ok && cloudy());
        widget_text(c, w, (struct rect){r.x + 100, r.y + 18, r.w - 120, 60}, &font_display, temp, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + 100, r.y + 82, r.w - 120, 18}, &font_bold, cond, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + 100, r.y + 102, r.w - 120, 16}, &font_small, place, soft, ALIGN_LEFT);
        if (w->size == WS_LARGE) {
            struct rtc_time t;
            time_local(&t);
            char line[64];
            ksnprintf(line, sizeof(line), "Updated %s · click to refresh",
                      ok ? "a moment ago" : wx.state == WX_FAILED ? "never" : "now");
            gfx_fill(c, (struct rect){r.x + 20, r.y + 150, r.w - 40, 1}, ALPHA(ink, 40));
            widget_text(c, w, (struct rect){r.x + 20, r.y + 160, r.w - 40, 18}, &font_small, line, soft, ALIGN_LEFT);
            widget_text(c, w, (struct rect){r.x + 20, r.y + r.h - 60, r.w - 40, 40}, &font_large,
                        panel_day_name(panel_weekday((int)t.year, CLAMP((int)t.month, 1, 12), (int)t.day)), ink,
                        ALIGN_LEFT);
        }
        break;
    }
}

/* ---- music and the visualizer */

static void cover_art(struct canvas *c, struct rect r, int index, int radius)
{
    static const uint32_t tops[3] = {RGB(255, 120, 120), RGB(250, 196, 140), RGB(110, 150, 230)};
    static const uint32_t bottoms[3] = {RGB(90, 30, 120), RGB(150, 90, 120), RGB(30, 40, 90)};
    int i = index < 0 ? 2 : index % 3;
    gfx_round_gradient(c, r, radius, tops[i], bottoms[i]);
    struct canvas clip = *c;
    clip.clip = rect_intersect(c->clip, rect_inset(r, radius / 3));
    int cx = r.x + r.w / 2, cy = r.y + r.h * 58 / 100;
    gfx_circle(&clip, cx, cy, r.w / 4, ALPHA(RGB(255, 236, 160), 230));
    for (int k = 0; k < 4; k++)
        gfx_fill(&clip, (struct rect){r.x, cy + k * 5 - 2, r.w, 2 + k / 2}, ALPHA(bottoms[i], 200));
}

static struct rect music_button(const struct widget *w, struct rect r, int i)
{
    if (w->size == WS_SMALL)
        return (struct rect){r.x + r.w / 2 - 54 + i * 38, r.y + r.h - 44, 32, 32};
    if (w->size == WS_SLIM)
        return (struct rect){r.x + r.w - 132 + i * 40, r.y + r.h / 2 - 16, 32, 32};
    return (struct rect){r.x + 114 + i * 44, r.y + r.h - 46, 32, 32};
}

static void music_buttons(struct canvas *c, const struct widget *w, struct rect r)
{
    static const enum symbol_id syms[3] = {SYM_PREV, SYM_PLAY, SYM_NEXT};
    uint32_t ink = widget_ink(w);
    for (int i = 0; i < 3; i++) {
        struct rect br = music_button(w, r, i);
        int rad = i == 1 ? 16 : 13;
        if (down(w, i))
            rad -= 2;
        if (i == 1)
            gfx_circle(c, br.x + 16, br.y + 16, rad, hot(w, i) ? color_shade(style.accent, 30) : style.accent);
        else if (hot(w, i))
            gfx_circle(c, br.x + 16, br.y + 16, rad, ALPHA(ink, 30));
        enum symbol_id s = i == 1 && media_track()->playing ? SYM_PAUSE : syms[i];
        gfx_symbol(c, br.x + 8, br.y + 8, s, 16, i == 1 ? style.accent_text : ink);
    }
}

static void paint_music(struct canvas *c, struct widget *w, struct rect r)
{
    const struct media_track *t = media_track();
    uint32_t ink = widget_ink(w), soft = widget_soft(w);
    if (w->size == WS_SLIM) {
        cover_art(c, (struct rect){r.x + 14, r.y + 12, r.h - 24, r.h - 24}, t->index, 10);
        widget_text(c, w, (struct rect){r.x + r.h, r.y + r.h / 2 - 18, r.w - r.h - 140, 18}, &font_bold, t->title,
                    ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + r.h, r.y + r.h / 2, r.w - r.h - 140, 16}, &font_small, t->artist, soft,
                    ALIGN_LEFT);
        music_buttons(c, w, r);
        return;
    }
    if (w->size == WS_SMALL) {
        cover_art(c, (struct rect){r.x + 16, r.y + 14, 52, 52}, t->index, 12);
        widget_text(c, w, (struct rect){r.x + 16, r.y + 72, r.w - 32, 18}, &font_bold, t->title, ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){r.x + 16, r.y + 90, r.w - 32, 16}, &font_small, t->artist, soft, ALIGN_LEFT);
        music_buttons(c, w, r);
        return;
    }
    cover_art(c, (struct rect){r.x + 16, r.y + 16, r.h - 32, r.h - 32}, t->index, 16);
    int x = r.x + 114;
    widget_text(c, w, (struct rect){x, r.y + 18, r.w - 130, 20}, &font_bold, t->title, ink, ALIGN_LEFT);
    widget_text(c, w, (struct rect){x, r.y + 38, r.w - 130, 16}, &font_small, t->artist, soft, ALIGN_LEFT);
    struct rect bar = {x, r.y + 66, r.w - 134, 4};
    gfx_round(c, bar, 2, ALPHA(ink, 50));
    int fill = bar.w * CLAMP(progress.pos, 0, 1024) / 1024;
    if (fill > 0)
        gfx_round(c, (struct rect){bar.x, bar.y, fill, 4}, 2, style.accent);
    gfx_circle(c, bar.x + fill, bar.y + 2, 4, RGB(255, 255, 255));
    char a[12], b[12];
    fmt_time(a, sizeof(a), t->position_ms);
    fmt_time(b, sizeof(b), t->length_ms);
    widget_text(c, w, (struct rect){bar.x, bar.y + 8, 60, 14}, &font_small, a, soft, ALIGN_LEFT);
    widget_text(c, w, (struct rect){bar.x + bar.w - 60, bar.y + 8, 60, 14}, &font_small, b, soft, ALIGN_RIGHT);
    music_buttons(c, w, r);
}

static void paint_visualizer(struct canvas *c, struct widget *w, struct rect r)
{
    uint32_t ink = widget_ink(w);
    int top = 14;
    if (w->size != WS_SLIM) {
        widget_text(c, w, (struct rect){r.x + 18, r.y + 12, r.w - 36, 18}, &font_bold,
                    media_track()->playing ? media_track()->title : "Visualizer", widget_soft(w), ALIGN_LEFT);
        top = 40;
    }
    int n = w->size == WS_SMALL ? MEDIA_BANDS / 2 : MEDIA_BANDS, gap = w->size == WS_SMALL ? 5 : 6;
    int bw = (r.w - 36 - gap * (n - 1)) / n;
    int base = r.y + r.h - 16, height = r.h - top - 22;
    for (int i = 0; i < n; i++) {
        int v = CLAMP(bars[i * MEDIA_BANDS / n].pos, 0, 1100);
        int h = 4 + height * v / 1100;
        struct rect b = {r.x + 18 + i * (bw + gap), base - h, bw, h};
        gfx_round(c, b, MIN(bw / 2, 5), ALPHA(ink, 150 + 80 * v / 1100));
    }
}

/* ---- calendar and tasks */

static void paint_calendar(struct canvas *c, struct widget *w, struct rect r)
{
    struct rtc_time t;
    time_local(&t);
    int m = CLAMP((int)t.month, 1, 12);
    char day[8], month[32];
    ksnprintf(day, sizeof(day), "%u", t.day);
    ksnprintf(month, sizeof(month), "%s %u", panel_month_name(m - 1), t.year);
    uint32_t ink = widget_ink(w), soft = widget_soft(w);
    const char *weekday = panel_day_name(panel_weekday((int)t.year, m, (int)t.day));
    if (w->size == WS_LARGE) {
        if (w->style == ST_SOLID || w->style == ST_CLEAR)
            gfx_text_box(c, (struct rect){r.x + 20, r.y + 14, r.w - 40, 22}, &font_title, month, ink, ALIGN_LEFT);
        calendar_draw(c, (struct rect){r.x + 6, r.y + 20, r.w - 12, r.h - 30});
        return;
    }
    widget_text(c, w, (struct rect){r.x + 18, r.y + 14, r.w - 36, 18}, &font_bold, weekday, style.accent, ALIGN_LEFT);
    widget_text(c, w, (struct rect){r.x + 14, r.y + 30, 120, 70}, &font_display, day, ink, ALIGN_LEFT);
    widget_text(c, w, (struct rect){r.x + 18, r.y + r.h - 34, r.w - 36, 20}, &font_sans, month, soft, ALIGN_LEFT);
    if (w->size != WS_MEDIUM)
        return;
    int first = (panel_weekday((int)t.year, m, 1) + 6) % 7;
    int days = month_days((int)t.year, m), cell = (r.w - 160) / 7;
    static const char *const heads[7] = {"M", "T", "W", "T", "F", "S", "S"};
    for (int i = 0; i < 7; i++)
        widget_text(c, w, (struct rect){r.x + 146 + i * cell, r.y + 16, cell, 14}, &font_small, heads[i], soft,
                    ALIGN_CENTER);
    for (int d = 1; d <= days; d++) {
        int slot = first + d - 1, row = slot / 7;
        if (row > 5)
            break;
        int cx = r.x + 146 + (slot % 7) * cell + cell / 2, cy = r.y + 40 + row * 21;
        char n[4];
        ksnprintf(n, sizeof(n), "%d", d);
        if (d == (int)t.day)
            gfx_circle(c, cx, cy, 9, style.accent);
        gfx_text_box(c, (struct rect){cx - 10, cy - 8, 20, 16}, &font_small, n, d == (int)t.day ? style.accent_text
                                                                                             : slot % 7 >= 5 ? soft : ink,
                     ALIGN_CENTER);
    }
}

static int task_rows(const struct widget *w, struct rect r)
{
    return MIN(tasks.count, MAX(1, (r.h - 54) / 26));
}

static struct rect task_row(struct rect r, int i)
{
    return (struct rect){r.x + 10, r.y + 44 + i * 26, r.w - 20, 26};
}

static struct rect task_add(struct rect r)
{
    return (struct rect){r.x + r.w - 40, r.y + 10, 28, 28};
}

static void paint_tasks(struct canvas *c, struct widget *w, struct rect r)
{
    if (!tasks.loaded)
        tasks_load();
    uint32_t ink = widget_ink(w), soft = widget_soft(w);
    int left = 0;
    for (int i = 0; i < tasks.count; i++)
        left += !tasks.done[i];
    char head[32];
    ksnprintf(head, sizeof(head), left ? "%d to do" : "All done", left);
    widget_text(c, w, (struct rect){r.x + 18, r.y + 14, r.w - 70, 20}, &font_bold, head, ink, ALIGN_LEFT);
    struct rect add = task_add(r);
    gfx_circle(c, add.x + 14, add.y + 14, 13, hot(w, 0) ? style.accent : ALPHA(ink, 28));
    gfx_symbol(c, add.x + 6, add.y + 6, SYM_PLUS, 16, hot(w, 0) ? style.accent_text : ink);
    int n = task_rows(w, r);
    for (int i = 0; i < n; i++) {
        struct rect row = task_row(r, i);
        if (hot(w, i + 1))
            gfx_round(c, row, 9, ALPHA(ink, 16));
        int cx = row.x + 15, cy = row.y + row.h / 2;
        if (tasks.done[i]) {
            gfx_circle(c, cx, cy, 8, style.accent);
            gfx_symbol(c, cx - 8, cy - 8, SYM_CHECK, 16, style.accent_text);
        } else {
            gfx_ring(c, cx, cy, 8, ALPHA(ink, 120));
        }
        widget_text(c, w, (struct rect){row.x + 32, row.y, row.w - 38, row.h}, &font_sans, tasks.text[i],
                    tasks.done[i] ? soft : ink, ALIGN_LEFT);
        if (tasks.done[i]) {
            int tw = MIN(gfx_text_width(&font_sans, tasks.text[i]), row.w - 38);
            gfx_fill(c, (struct rect){row.x + 32, cy, tw, 1}, soft);
        }
    }
    if (!tasks.count)
        widget_text(c, w, task_row(r, 0), &font_sans, "Nothing planned", soft, ALIGN_CENTER);
}

static void task_added(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!value || !value[0] || tasks.count >= MAX_TASKS)
        return;
    str_copy(tasks.text[tasks.count], value, sizeof(tasks.text[0]));
    tasks.done[tasks.count++] = false;
    tasks_save();
    damage_kind(WK_TASKS);
}

/* ---- photo */

static bool photo_dir(const struct widget *w, char *dir, size_t cap)
{
    if (w->opt[0] == '~')
        path_join(user_home(), w->opt + (w->opt[1] == '/' ? 2 : 1), dir, cap);
    else if (w->opt[0])
        str_copy(dir, w->opt, cap);
    else
        path_join(user_home(), "Pictures", dir, cap);
    return vfs_exists(dir);
}

/* The index-th picture of the folder (it wraps around); the samples stand in for an empty folder. */
static bool photo_path(const struct widget *w, int index, char *out, size_t cap)
{
    char dirs[2][PATH_MAX_LEN];
    int nd = 0;
    if (photo_dir(w, dirs[nd], sizeof(dirs[0])))
        nd++;
    str_copy(dirs[nd++], "/system/samples", sizeof(dirs[0]));
    for (int d = 0; d < nd; d++) {
        int count = 0;
        for (int pass = 0; pass < 2; pass++) {
            struct vdir dir;
            if (vfs_opendir(dirs[d], &dir) != 0)
                break;
            struct vfs_dirent ent;
            int k = 0;
            while (vfs_readdir(&dir, &ent) == 0) {
                if (ent.name[0] == '.' || !picture_is_image(ent.name))
                    continue;
                if (pass == 1 && k == index % count) {
                    path_join(dirs[d], ent.name, out, cap);
                    vfs_closedir(&dir);
                    return true;
                }
                k++;
            }
            vfs_closedir(&dir);
            count = k;
            if (!count)
                break;
        }
    }
    return false;
}

static void photo_load(struct widget *w, struct rect r)
{
    char path[PATH_MAX_LEN];
    canvas_free(&w->pic);
    if (!photo_path(w, w->pic_index, path, sizeof(path)))
        return;
    struct picture p;
    if (picture_load(path, &p) != 0)
        return;
    struct canvas src = {p.px, p.w, p.h, p.w, {0, 0, p.w, p.h}};
    if (canvas_alloc(&w->pic, r.w, r.h)) {
        int sw = p.w, sh = p.h;
        if ((int64_t)p.w * r.h > (int64_t)p.h * r.w)
            sw = (int)((int64_t)p.h * r.w / MAX(r.h, 1));
        else
            sh = (int)((int64_t)p.w * r.h / MAX(r.w, 1));
        src.px += (p.h - sh) / 2 * p.w + (p.w - sw) / 2;
        src.w = sw;
        src.h = sh;
        src.clip = (struct rect){0, 0, sw, sh};
        gfx_scale_smooth(&w->pic, (struct rect){0, 0, r.w, r.h}, &src, 255);
    }
    picture_free(&p);
    w->pic_next = hal_ticks_ms() + 60000;
}

static void paint_photo(struct canvas *c, struct widget *w, struct rect r)
{
    struct rect full = w->r;
    if (!w->pic.px || w->pic.w != full.w || w->pic.h != full.h)
        photo_load(w, full);
    int rad = w->shape == SH_ROUND ? MIN(full.w, full.h) / 2 : w->shape == SH_SQUARE ? 10 : MIN(28, MIN(full.w, full.h) / 4);
    if (w->pic.px)
        gfx_blit_round(c, full, &w->pic, rad, false);
    else
        widget_text(c, w, r, &font_sans, "No pictures", widget_soft(w), ALIGN_CENTER);
}

/* ---- notes */

static void paint_notes(struct canvas *c, struct widget *w, struct rect r)
{
    uint32_t ink = widget_ink(w);
    gfx_round(c, (struct rect){r.x + 18, r.y + 18, 20, 4}, 2, RGB(255, 204, 0));
    widget_text(c, w, (struct rect){r.x + 44, r.y + 10, r.w - 60, 20}, &font_bold, "Notes", RGB(255, 204, 0),
                ALIGN_LEFT);
    ui_wrap_text(c, (struct rect){r.x + 18, r.y + 38, r.w - 36, r.h - 50}, w->size == WS_SMALL ? &font_small : &font_sans,
                 w->opt[0] ? w->opt : "Empty note", ink);
}

static struct widget *edit_note;

static void note_edited(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!edit_note || !edit_note->used || !value)
        return;
    str_copy(edit_note->opt, value, sizeof(edit_note->opt));
    widgets_changed();
}

/* ---- system */

static int disk_percent(void)
{
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        uint64_t total = 0, free = 0;
        if (!strcmp(m->path, "/home") && m->fs->statfs && m->fs->statfs(m->root, &total, &free) == 0 && total)
            return (int)((total - free) * 100 / total);
    }
    return -1;
}

static void paint_system(struct canvas *c, struct widget *w, struct rect r)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    int mem = hs.total ? (int)((uint64_t)hs.used * 100 / hs.total) : 0, disk = disk_percent();
    uint64_t sec = hal_ticks_ms() / 1000;
    char up[24], ip[16] = "Offline", a[16], b[16];
    if (sec < 3600)
        ksnprintf(up, sizeof(up), "%u min", (unsigned)(sec / 60));
    else
        ksnprintf(up, sizeof(up), "%u h %u min", (unsigned)(sec / 3600), (unsigned)(sec % 3600 / 60));
    if (net_enabled() && net_interface_count() && net_interface(0)->configured)
        net_format_ip(net_interface(0)->ip, ip, sizeof(ip));
    ksnprintf(a, sizeof(a), "%d%%", mem);
    ksnprintf(b, sizeof(b), disk >= 0 ? "%d%%" : "-", disk);
    uint32_t ink = widget_ink(w), soft = widget_soft(w);
    if (w->size == WS_SLIM) {
        char line[96];
        ksnprintf(line, sizeof(line), "Memory %s  ·  Disk %s  ·  Up %s", a, b, up);
        gfx_symbol(c, r.x + 18, r.y + r.h / 2 - 8, SYM_CHIP, 16, ink);
        widget_text(c, w, (struct rect){r.x + 44, r.y, r.w - 60, r.h}, &font_sans, line, ink, ALIGN_LEFT);
        return;
    }
    int bars_x = r.x + 18, bw = w->size == WS_SMALL ? r.w - 36 : r.w / 2 - 30;
    const char *labels[2] = {"Memory", "Disk"};
    const char *values[2] = {a, b};
    int pct[2] = {mem, MAX(disk, 0)};
    for (int i = 0; i < 2; i++) {
        int y = r.y + 16 + i * 46;
        widget_text(c, w, (struct rect){bars_x, y, bw, 18}, &font_bold, labels[i], ink, ALIGN_LEFT);
        widget_text(c, w, (struct rect){bars_x, y, bw, 18}, &font_sans, values[i], soft, ALIGN_RIGHT);
        struct rect bar = {bars_x, y + 24, bw, 8};
        gfx_round(c, bar, 4, ALPHA(ink, 40));
        gfx_round(c, (struct rect){bar.x, bar.y, MAX(bar.w * pct[i] / 100, 8), bar.h}, 4,
                  pct[i] > 85 ? RGB(255, 69, 58) : style.accent);
    }
    if (w->size == WS_SMALL) {
        widget_text(c, w, (struct rect){bars_x, r.y + r.h - 32, bw, 18}, &font_small, up, soft, ALIGN_LEFT);
        return;
    }
    int x = r.x + r.w / 2 + 10;
    widget_text(c, w, (struct rect){x, r.y + 16, r.w / 2 - 28, 18}, &font_bold, "Up for", ink, ALIGN_LEFT);
    widget_text(c, w, (struct rect){x, r.y + 36, r.w / 2 - 28, 18}, &font_sans, up, soft, ALIGN_LEFT);
    widget_text(c, w, (struct rect){x, r.y + 62, r.w / 2 - 28, 18}, &font_bold, "Network", ink, ALIGN_LEFT);
    widget_text(c, w, (struct rect){x, r.y + 82, r.w / 2 - 28, 18}, &font_sans, ip, soft, ALIGN_LEFT);
    widget_text(c, w, (struct rect){x, r.y + 108, r.w / 2 - 28, 18}, &font_bold, "Processor", ink, ALIGN_LEFT);
    widget_text(c, w, (struct rect){x, r.y + 128, r.w / 2 - 28, 18}, &font_small, hal_machine(), soft, ALIGN_LEFT);
}

/* ---- apps */

static int app_names(const struct widget *w, const struct gui_app **out, int max)
{
    int n = 0;
    for (const char *p = w->opt; *p && n < max;) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char name[48];
        memcpy(name, p, MIN(len, sizeof(name) - 1));
        name[MIN(len, sizeof(name) - 1)] = 0;
        const struct gui_app *app = gui_app_find(name);
        if (app && !str_casecmp(app->name, name))
            out[n++] = app;
        p += len + (e ? 1 : 0);
    }
    return n;
}

static int apps_max(const struct widget *w)
{
    return w->size == WS_MEDIUM ? 8 : 4;
}

static struct rect app_cell(const struct widget *w, struct rect r, int i)
{
    if (w->size == WS_SLIM) {
        int cw = r.w / 4;
        return (struct rect){r.x + i * cw + (cw - 52) / 2, r.y + (r.h - 52) / 2, 52, 52};
    }
    int cols = w->size == WS_MEDIUM ? 4 : 2, cw = r.w / cols, ch = r.h / 2;
    return (struct rect){r.x + (i % cols) * cw + (cw - 56) / 2, r.y + (i / cols) * ch + (ch - 56) / 2, 56, 56};
}

static void paint_apps(struct canvas *c, struct widget *w, struct rect r)
{
    const struct gui_app *apps[8];
    int n = app_names(w, apps, apps_max(w));
    for (int i = 0; i < n; i++) {
        struct rect cell = app_cell(w, r, i);
        int size = down(w, i) ? 44 : 48;
        if (hot(w, i))
            gfx_round(c, rect_inset(cell, -2), 14, ALPHA(widget_ink(w), 24));
        if (apps[i]->picture && apps[i]->picture->px)
            gfx_scale(c, (struct rect){cell.x + (56 - size) / 2, cell.y + (56 - size) / 2, size, size}, apps[i]->picture,
                      255);
        else
            gfx_icon(c, cell.x + (56 - size) / 2, cell.y + (56 - size) / 2, apps[i]->icon, size);
    }
    if (!n)
        widget_text(c, w, r, &font_small, "Edit the widget to pick apps", widget_soft(w), ALIGN_CENTER);
}

/* ---- dispatch */

void widget_paint_kind(struct canvas *c, struct widget *w, struct rect r)
{
    switch (w->kind) {
    case WK_CLOCK: paint_clock(c, w, r); break;
    case WK_WEATHER: paint_weather(c, w, r); break;
    case WK_MUSIC: paint_music(c, w, r); break;
    case WK_CALENDAR: paint_calendar(c, w, r); break;
    case WK_VISUALIZER: paint_visualizer(c, w, r); break;
    case WK_TASKS: paint_tasks(c, w, r); break;
    case WK_ANALOG: paint_analog(c, w, r); break;
    case WK_WORLD: paint_world(c, w, r); break;
    case WK_PHOTO: paint_photo(c, w, r); break;
    case WK_NOTES: paint_notes(c, w, r); break;
    case WK_SYSTEM: paint_system(c, w, r); break;
    case WK_APPS: paint_apps(c, w, r); break;
    }
}

int widget_parts(const struct widget *w, struct rect r, struct rect *out, int max)
{
    int n = 0;
    switch (w->kind) {
    case WK_MUSIC:
        for (int i = 0; i < 3 && n < max; i++)
            out[n++] = music_button(w, r, i);
        break;
    case WK_TASKS:
        out[n++] = task_add(r);
        for (int i = 0; i < task_rows(w, r) && n < max; i++)
            out[n++] = task_row(r, i);
        break;
    case WK_APPS: {
        const struct gui_app *apps[8];
        int count = app_names(w, apps, apps_max(w));
        for (int i = 0; i < count && n < max; i++)
            out[n++] = app_cell(w, r, i);
        break;
    }
    default: break;
    }
    return n;
}

void widget_click(struct widget *w, int part, int clicks)
{
    switch (w->kind) {
    case WK_MUSIC:
        if (part == 0)
            media_prev();
        else if (part == 1)
            media_toggle();
        else if (part == 2)
            media_next();
        else if (clicks >= 2)
            gui_app_launch(gui_app_find("Music"), NULL);
        break;
    case WK_TASKS:
        if (part == 0) {
            ui_prompt(NULL, "New task", "What do you want to do?", "", task_added);
        } else if (part > 0 && part <= tasks.count) {
            tasks.done[part - 1] = !tasks.done[part - 1];
            tasks_save();
        } else if (part == 200) {
            int k = 0;
            for (int i = 0; i < tasks.count; i++)
                if (!tasks.done[i]) {
                    str_copy(tasks.text[k], tasks.text[i], sizeof(tasks.text[0]));
                    tasks.done[k++] = false;
                }
            tasks.count = k;
            tasks_save();
        }
        break;
    case WK_APPS: {
        const struct gui_app *apps[8];
        int n = app_names(w, apps, apps_max(w));
        if (part >= 0 && part < n) {
            if (wm_app_window_count(apps[part]) > 0)
                wm_focus_app(apps[part]);
            else
                gui_app_launch(apps[part], NULL);
        }
        break;
    }
    case WK_WEATHER:
        if (wx.state == WX_FAILED || wx.state == WX_DONE)
            wx.next = hal_ticks_ms();
        break;
    case WK_PHOTO:
        if (clicks >= 2) {
            char path[PATH_MAX_LEN];
            if (photo_path(w, w->pic_index, path, sizeof(path)))
                gui_app_launch(gui_app_find("Photos"), path);
        } else {
            w->pic_index++;
            canvas_free(&w->pic);
        }
        break;
    case WK_NOTES:
        edit_note = w;
        ui_prompt(NULL, "Notes", "The note on the desktop:", w->opt, note_edited);
        break;
    case WK_SYSTEM:
        if (clicks >= 2)
            gui_app_launch(gui_app_find("System Monitor"), NULL);
        break;
    case WK_VISUALIZER:
        if (clicks >= 2)
            gui_app_launch(gui_app_find("Music"), NULL);
        break;
    case WK_CLOCK:
    case WK_ANALOG:
    case WK_CALENDAR:
        if (clicks >= 2) {
            struct rect s = wm_screen_rect(0);
            calendar_toggle(s.x + s.w - 12, status_bottom() + 8);
        }
        break;
    default: break;
    }
    widget_damage(w);
}

static bool kind_busy;

bool widget_kind_animating(void)
{
    return kind_busy;
}

void widget_kinds_tick(void)
{
    if (!started_at)
        started_at = hal_ticks_ms();
    uint64_t now = hal_ticks_ms();
    int dt = last_tick ? (int)MIN(now - last_tick, 64u) : 16;
    last_tick = now;
    kind_busy = false;
    bool sound = any_kind(WK_VISUALIZER) || any_kind(WK_MUSIC);
    if (media_tick() && sound) {
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
            damage_kind(WK_VISUALIZER);
            damage_kind(WK_MUSIC);
        }
    }
    struct rtc_time t;
    time_local(&t);
    if ((int)t.minute != last_minute) {
        last_minute = (int)t.minute;
        damage_kind(WK_CLOCK);
        damage_kind(WK_CALENDAR);
        damage_kind(WK_WORLD);
        damage_kind(WK_SYSTEM);
    }
    if ((int)t.second != last_second) {
        last_second = (int)t.second;
        damage_kind(WK_ANALOG);
    }
    int n;
    struct widget *ws = widget_list(&n);
    for (int i = 0; i < n; i++)
        if (ws[i].used && ws[i].kind == WK_PHOTO && ws[i].pic.px && now >= ws[i].pic_next) {
            ws[i].pic_index++;
            canvas_free(&ws[i].pic);
            widget_damage(&ws[i]);
        }
    weather_tick();
}

