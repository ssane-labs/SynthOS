#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/string.h>
#include <synth/time.h>

#include "../internal.h"

/* Weather: the forecast from open-meteo.com (no key needed) for a list of places found with its geocoding API.
   Numbers come in tenths so no floating point is needed. */

#define MAX_PLACES 8
#define HOURS 24
#define DAYS 7

struct place {
    char name[40], country[24], lat[12], lon[12];
};

struct forecast {
    bool ok;
    int temp, feels, humidity, wind, code, hour; /* tenths of degrees / km/h, WMO code, the local hour */
    bool day;
    int hourly[HOURS], hourly_code[HOURS];
    int high[DAYS], low[DAYS], daily_code[DAYS], rain[DAYS];
    struct rtc_time date[DAYS];
    uint64_t fetched;
};

struct weather {
    struct ui_state st;
    struct place places[MAX_PLACES];
    int count, current;
    bool fahrenheit, pending;
    struct forecast f;
    char error[64];
};

static struct window *open_win;

static void save(struct weather *s)
{
    char text[MAX_PLACES * 96] = "";
    for (int i = 0; i < s->count; i++)
        ksnprintf(text + strlen(text), sizeof(text) - strlen(text), "%s|%s|%s|%s;", s->places[i].name,
                  s->places[i].country, s->places[i].lat, s->places[i].lon);
    app_state_set("weather", "places", text);
    app_state_set_number("weather", "current", s->current);
    app_state_set_number("weather", "fahrenheit", s->fahrenheit);
}

static void field(const char **p, char *out, size_t cap, char end)
{
    size_t n = 0;
    while (**p && **p != end && **p != ';') {
        if (n + 1 < cap)
            out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    if (**p == end)
        (*p)++;
}

static void load(struct weather *s)
{
    const char *p = app_state("weather", "places", "London|United Kingdom|51.5085|-0.1257;");
    while (*p && s->count < MAX_PLACES) {
        struct place *pl = &s->places[s->count];
        field(&p, pl->name, sizeof(pl->name), '|');
        field(&p, pl->country, sizeof(pl->country), '|');
        field(&p, pl->lat, sizeof(pl->lat), '|');
        field(&p, pl->lon, sizeof(pl->lon), ';');
        if (pl->name[0] && pl->lat[0] && pl->lon[0])
            s->count++;
        if (*p == ';')
            p++;
    }
    s->current = (int)CLAMP(app_state_number("weather", "current", 0), 0, MAX(s->count - 1, 0));
    s->fahrenheit = app_state_number("weather", "fahrenheit", 0) != 0;
}

/* "12.34" -> 123 */
static int tenths(const char *p, const char **end)
{
    bool neg = *p == '-';
    if (neg)
        p++;
    int v = 0, frac = -1;
    for (; (*p >= '0' && *p <= '9') || (*p == '.' && frac < 0); p++) {
        if (*p == '.') {
            frac = 0;
            continue;
        }
        if (frac < 0)
            v = v * 10 + *p - '0';
        else if (frac++ == 0)
            v = v * 10 + *p - '0';
    }
    if (frac <= 0)
        v *= 10;
    if (end)
        *end = p;
    return neg ? -v : v;
}

/* "key":<number> inside the object that starts at "section":{ */
static bool scalar(const char *json, const char *section, const char *key, int *out)
{
    char pat[48];
    ksnprintf(pat, sizeof(pat), "\"%s\":{", section);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    const char *end = strchr(p, '}');
    ksnprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(p, pat);
    if (!p || (end && p > end))
        return false;
    p += strlen(pat);
    if (*p != '-' && (*p < '0' || *p > '9'))
        return false;
    *out = tenths(p, NULL);
    return true;
}

/* "key":[n,n,...] in the section's object */
static int array(const char *json, const char *section, const char *key, int *out, int max)
{
    char pat[48];
    ksnprintf(pat, sizeof(pat), "\"%s\":{", section);
    const char *p = strstr(json, pat);
    if (!p)
        return 0;
    ksnprintf(pat, sizeof(pat), "\"%s\":[", key);
    p = strstr(p, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    int n = 0;
    while (n < max && *p && *p != ']') {
        if (*p == 'n') /* null */
            out[n++] = 0, p += 4;
        else
            out[n++] = tenths(p, &p);
        if (*p == ',')
            p++;
    }
    return n;
}

static bool string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[40];
    ksnprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return false;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' && *p)
            c = (unsigned char)*p++;
        out[n++] = c < 0x80 ? (char)c : '?';
        while (c >= 0xc0 && (*p & 0xc0) == 0x80) /* the CP437 font has no use for the rest of a UTF-8 letter */
            p++;
    }
    out[n] = 0;
    return true;
}

static int get(const char *url, struct http_response *resp)
{
    struct http_request req = {.url = url, .max_size = 1u << 17, .flags = HTTP_NO_COOKIES | HTTP_GZIP};
    int err = http_fetch(&req, resp);
    if (err)
        return err;
    if (resp->status != 200 || !resp->body) {
        http_free(resp);
        return E_IO;
    }
    return 0;
}

static void refresh(struct weather *s)
{
    s->pending = false;
    s->error[0] = 0;
    if (!s->count)
        return;
    const struct place *pl = &s->places[s->current];
    char url[512];
    ksnprintf(url, sizeof(url),
              "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&forecast_days=7"
              "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
              "&hourly=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min,"
              "precipitation_probability_max",
              pl->lat, pl->lon);
    struct http_response resp;
    int err = get(url, &resp);
    if (err) {
        ksnprintf(s->error, sizeof(s->error), "Can't reach open-meteo.com (%s)", err_str(err));
        return;
    }
    struct forecast *f = &s->f;
    memset(f, 0, sizeof(*f));
    const char *j = resp.body;
    int day = 1;
    f->ok = scalar(j, "current", "temperature_2m", &f->temp);
    scalar(j, "current", "apparent_temperature", &f->feels);
    scalar(j, "current", "relative_humidity_2m", &f->humidity);
    scalar(j, "current", "wind_speed_10m", &f->wind);
    scalar(j, "current", "weather_code", &f->code);
    scalar(j, "current", "is_day", &day);
    f->day = day != 0;
    f->code /= 10;
    char now[24] = "";
    const char *cur = strstr(j, "\"current\":{");
    if (cur)
        string(cur, "time", now, sizeof(now));
    f->hour = strlen(now) >= 13 ? (now[11] - '0') * 10 + now[12] - '0' : 0;
    int temps[48], codes[48];
    int n = array(j, "hourly", "temperature_2m", temps, 48);
    array(j, "hourly", "weather_code", codes, 48);
    for (int i = 0; i < HOURS; i++) {
        int k = MIN(f->hour + i, n - 1);
        f->hourly[i] = k >= 0 ? temps[k] : 0;
        f->hourly_code[i] = k >= 0 ? codes[k] / 10 : 0;
    }
    array(j, "daily", "temperature_2m_max", f->high, DAYS);
    array(j, "daily", "temperature_2m_min", f->low, DAYS);
    array(j, "daily", "weather_code", f->daily_code, DAYS);
    array(j, "daily", "precipitation_probability_max", f->rain, DAYS);
    const char *d = strstr(j, "\"daily\":{");
    d = d ? strstr(d, "\"time\":[\"") : NULL;
    for (int i = 0; d && i < DAYS; i++) {
        d = strchr(d + 8 * !i, '"'); /* the opening quote of the date */
        if (!d || strlen(d) < 11)
            break;
        f->date[i] = (struct rtc_time){(uint16_t)str_to_long(d + 1, NULL, 10), (uint8_t)str_to_long(d + 6, NULL, 10),
                                       (uint8_t)str_to_long(d + 9, NULL, 10), 0, 0, 0};
        d = strchr(d + 1, '"');
        d = d ? d + 1 : NULL;
    }
    for (int i = 0; i < DAYS; i++)
        f->daily_code[i] /= 10;
    f->fetched = hal_ticks_ms();
    if (!f->ok)
        str_copy(s->error, "open-meteo.com sent no forecast", sizeof(s->error));
    http_free(&resp);
}

static const char *condition(int code)
{
    if (code == 0)
        return "Clear";
    if (code <= 2)
        return code == 1 ? "Mostly Clear" : "Partly Cloudy";
    if (code == 3)
        return "Cloudy";
    if (code <= 48)
        return "Fog";
    if (code <= 57)
        return "Drizzle";
    if (code <= 67)
        return code >= 66 ? "Freezing Rain" : "Rain";
    if (code <= 77)
        return "Snow";
    if (code <= 82)
        return "Showers";
    if (code <= 86)
        return "Snow Showers";
    return "Thunderstorm";
}

static void cloud(struct canvas *c, int cx, int cy, int r, uint32_t color)
{
    gfx_circle(c, cx - r / 2, cy, r * 2 / 3, color);
    gfx_circle(c, cx + r / 3, cy - r / 4, r * 5 / 6, color);
    gfx_round(c, (struct rect){cx - r - r / 6, cy, 2 * r + r / 3, r * 2 / 3}, r / 3, color);
}

static void icon(struct canvas *c, int cx, int cy, int r, int code, bool day)
{
    uint32_t sun = RGB(255, 196, 70), white = RGB(240, 242, 246), grey = RGB(170, 176, 188);
    if (code <= 2) {
        int sx = code ? cx - r / 3 : cx, sy = code ? cy - r / 3 : cy;
        if (day)
            gfx_circle(c, sx, sy, code ? r * 2 / 3 : r * 4 / 5, sun);
        else {
            gfx_circle(c, sx, sy, r * 2 / 3, RGB(230, 232, 210));
            gfx_circle(c, sx + r / 3, sy - r / 4, r / 2, style.content);
        }
        if (code)
            cloud(c, cx + r / 4, cy + r / 4, r * 3 / 4, white);
        return;
    }
    if (code == 3 || code >= 45) {
        bool heavy = code >= 51;
        cloud(c, cx, cy - r / 5, r, heavy ? grey : white);
    }
    if (code >= 45 && code <= 48) {
        for (int i = 0; i < 3; i++)
            gfx_round(c, (struct rect){cx - r + i * r / 4, cy + r / 2 + i * r / 4, 2 * r - i * r / 2, 3}, 1, grey);
        return;
    }
    bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    for (int i = 0; code >= 51 && code < 95 && i < 3; i++) {
        int x = cx - r / 2 + i * r / 2, y = cy + r / 2 + (i & 1) * r / 5;
        if (snow)
            gfx_circle(c, x, y + 3, 3, white);
        else
            gfx_thick_line(c, x + 2, y, x - 2, y + r / 3, 2, RGB(90, 170, 255));
    }
    if (code >= 95) {
        int x = cx, y = cy + r / 3;
        gfx_thick_line(c, x + 3, y, x - 4, y + r / 3, 3, sun);
        gfx_thick_line(c, x - 4, y + r / 3, x + 4, y + r / 3, 3, sun);
        gfx_thick_line(c, x + 4, y + r / 3, x - 3, y + r * 2 / 3, 3, sun);
    }
}

static void degrees(const struct weather *s, int t, char *out, size_t cap)
{
    int v = s->fahrenheit ? t * 9 / 5 + 320 : t;
    v = v >= 0 ? (v + 5) / 10 : -((-v + 5) / 10);
    ksnprintf(out, cap, "%d\xf8", v); /* CP437 degree sign */
}

static void found(struct window *owner, const char *value);

static void search(struct window *owner)
{
    ui_prompt(owner, "Add a Place", "City name", "", found);
}

static void found(struct window *owner, const char *value)
{
    struct weather *s = owner && wm_window_alive(owner) ? owner->data : NULL;
    if (!s || !value || !*value || s->count == MAX_PLACES)
        return;
    char url[256], name[96];
    size_t k = 0;
    for (const char *p = value; *p && k + 4 < sizeof(name); p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
            name[k++] = (char)c;
        else
            k += (size_t)ksnprintf(name + k, sizeof(name) - k, "%%%02X", c);
    }
    name[k] = 0;
    ksnprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&name=%s", name);
    struct http_response resp;
    int err = get(url, &resp);
    if (err) {
        gui_notify("Weather", "Can't reach open-meteo.com");
        return;
    }
    struct place pl = {0};
    const char *r = strstr(resp.body, "\"results\":[");
    int lat = 0, lon = 0;
    const char *p;
    bool ok = r && string(r, "name", pl.name, sizeof(pl.name)) && (p = strstr(r, "\"latitude\":")) &&
              ((lat = tenths(p + 11, NULL)), (p = strstr(r, "\"longitude\":"))) && ((lon = tenths(p + 12, NULL)), true);
    if (ok) {
        string(r, "country", pl.country, sizeof(pl.country));
        /* keep the coordinates as text, they go back into the URL as they are */
        const char *a = strstr(r, "\"latitude\":") + 11, *b = strstr(r, "\"longitude\":") + 12;
        size_t na = 0, nb = 0;
        while (na + 1 < sizeof(pl.lat) && (a[na] == '-' || a[na] == '.' || (a[na] >= '0' && a[na] <= '9')))
            na++;
        while (nb + 1 < sizeof(pl.lon) && (b[nb] == '-' || b[nb] == '.' || (b[nb] >= '0' && b[nb] <= '9')))
            nb++;
        memcpy(pl.lat, a, na);
        memcpy(pl.lon, b, nb);
        UNUSED(lat);
        UNUSED(lon);
    }
    http_free(&resp);
    if (!ok) {
        char text[96];
        ksnprintf(text, sizeof(text), "No place called %s", value);
        gui_notify("Weather", text);
        search(owner);
        return;
    }
    s->places[s->count] = pl;
    s->current = s->count++;
    save(s);
    s->pending = true;
    wm_set_timer(owner, 30);
    wm_invalidate(owner);
}

static void frame(struct ui *ui, void *data)
{
    struct weather *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h, side = 200;
    if (ui->e && ui->e->type == EV_TIMER && s->pending) {
        refresh(s);
        wm_set_timer(ui->win, 60000);
        ui->consumed = true;
        return;
    }
    if (ui->e && ui->e->type == EV_TIMER && hal_ticks_ms() - s->f.fetched > 15 * 60000) {
        s->pending = true;
        wm_set_timer(ui->win, 30);
    }
    ui_sidebar_panel(ui, side, H, style.window);
    ui_sidebar_header(ui, (struct rect){22, 46, side - 32, 20}, "Places");
    for (int i = 0; i < s->count; i++)
        if (ui_sidebar_item(ui, (struct rect){14, 70 + i * 30, side - 26, 28}, SYM_PIN, s->places[i].name,
                            i == s->current) &&
            i != s->current) {
            s->current = i;
            s->pending = true;
            memset(&s->f, 0, sizeof(s->f));
            save(s);
            wm_set_timer(ui->win, 30);
        }
    if (ui_button(ui, (struct rect){18, H - 48, 82, 28}, "Add Place") && s->count < MAX_PLACES)
        search(ui->win);
    if (ui_button(ui, (struct rect){106, H - 48, 80, 28}, "Remove") && s->count > 1) {
        memmove(&s->places[s->current], &s->places[s->current + 1],
                sizeof(s->places[0]) * (size_t)(s->count - s->current - 1));
        s->count--;
        s->current = MIN(s->current, s->count - 1);
        memset(&s->f, 0, sizeof(s->f));
        s->pending = true;
        save(s);
        wm_set_timer(ui->win, 30);
    }
    struct rect main = {side + 1, 0, W - side - 1, H};
    uint32_t top = s->f.ok && !s->f.day ? RGB(24, 32, 60) : RGB(70, 130, 200),
             bottom = s->f.ok && !s->f.day ? RGB(50, 44, 90) : RGB(120, 170, 220);
    if (s->f.ok && s->f.code >= 3)
        top = RGB(90, 100, 118), bottom = RGB(130, 140, 156);
    if (ui->c)
        gfx_gradient(ui->c, main, top, bottom);
    uint32_t ink = RGB(255, 255, 255), soft = ALPHA(RGB(255, 255, 255), 190);
    int x = main.x + 24, w = main.w - 48;
    const char *units[] = {"\xf8" "C", "\xf8" "F"};
    int unit = s->fahrenheit;
    if (ui_segmented(ui, (struct rect){main.x + main.w - 136, 14, 88, 24}, units, 2, &unit)) {
        s->fahrenheit = unit != 0;
        save(s);
    }
    if (ui_button(ui, (struct rect){main.x + main.w - 222, 14, 76, 24}, "Refresh")) {
        s->pending = true;
        wm_set_timer(ui->win, 30);
    }
    if (!s->count)
        return;
    const struct place *pl = &s->places[s->current];
    ui_label(ui, (struct rect){x, 40, w, 30}, pl->name, &font_title, ink, ALIGN_CENTER);
    ui_label(ui, (struct rect){x, 66, w, 18}, pl->country, &font_sans, soft, ALIGN_CENTER);
    if (!s->f.ok) {
        ui_label(ui, (struct rect){x, H / 2 - 10, w, 20}, s->error[0] ? s->error : "Updating...", &font_sans, ink,
                 ALIGN_CENTER);
        return;
    }
    const struct forecast *f = &s->f;
    char text[64], a[16], b[16];
    degrees(s, f->temp, text, sizeof(text));
    ui_label(ui, (struct rect){x, 86, w, 70}, text, &font_display, ink, ALIGN_CENTER);
    degrees(s, f->high[0], a, sizeof(a));
    degrees(s, f->low[0], b, sizeof(b));
    ksnprintf(text, sizeof(text), "%s   H:%s  L:%s", condition(f->code), a, b);
    ui_label(ui, (struct rect){x, 158, w, 20}, text, &font_bold, ink, ALIGN_CENTER);
    degrees(s, f->feels, a, sizeof(a));
    ksnprintf(text, sizeof(text), "Feels like %s   Humidity %d%%   Wind %d km/h", a, f->humidity / 10,
              (f->wind + 5) / 10);
    ui_label(ui, (struct rect){x, 180, w, 18}, text, &font_sans, soft, ALIGN_CENTER);
    if (ui->c)
        icon(ui->c, x + w / 2 - 120, 120, 26, f->code, f->day);
    /* the next hours */
    struct rect card = {x, 210, w, 104};
    if (ui->c)
        gfx_round(ui->c, card, 12, ALPHA(RGB(0, 0, 0), 50));
    int cols = MIN(HOURS, MAX(4, (w - 20) / 52)), cw = (w - 20) / cols;
    for (int i = 0; i < cols; i++) {
        int cx = card.x + 10 + i * cw + cw / 2;
        if (i)
            ksnprintf(text, sizeof(text), "%02d", (f->hour + i) % 24);
        ui_label(ui, (struct rect){cx - cw / 2, card.y + 8, cw, 18}, i ? text : "Now", &font_small, soft,
                 ALIGN_CENTER);
        int hr = (f->hour + i) % 24;
        if (ui->c)
            icon(ui->c, cx, card.y + 48, 11, f->hourly_code[i], hr >= 7 && hr < 19);
        degrees(s, f->hourly[i], text, sizeof(text));
        ui_label(ui, (struct rect){cx - cw / 2, card.y + 74, cw, 20}, text, &font_bold, ink, ALIGN_CENTER);
    }
    /* the week, bars spanning the week's range */
    int lo = f->low[0], hi = f->high[0];
    for (int i = 1; i < DAYS; i++)
        lo = MIN(lo, f->low[i]), hi = MAX(hi, f->high[i]);
    struct rect week = {x, card.y + card.h + 12, w, H - card.y - card.h - 24};
    if (ui->c)
        gfx_round(ui->c, week, 12, ALPHA(RGB(0, 0, 0), 50));
    int rh = MIN(34, (week.h - 12) / DAYS), bx = week.x + 210, bw = week.w - 210 - 80;
    for (int i = 0; i < DAYS && rh >= 20; i++) {
        int y = week.y + 6 + i * rh;
        const char *name = i ? time_weekday_name(time_weekday(&f->date[i])) : "Today";
        ksnprintf(text, sizeof(text), "%.3s", name);
        ui_label(ui, (struct rect){week.x + 14, y, 60, rh}, i ? text : name, &font_bold, ink, ALIGN_LEFT);
        if (ui->c)
            icon(ui->c, week.x + 100, y + rh / 2, 9, f->daily_code[i], true);
        if (f->rain[i] >= 200) {
            ksnprintf(text, sizeof(text), "%d%%", f->rain[i] / 10);
            ui_label(ui, (struct rect){week.x + 118, y, 50, rh}, text, &font_small, RGB(140, 200, 255), ALIGN_LEFT);
        }
        degrees(s, f->low[i], a, sizeof(a));
        degrees(s, f->high[i], b, sizeof(b));
        ui_label(ui, (struct rect){bx - 44, y, 40, rh}, a, &font_sans, soft, ALIGN_RIGHT);
        ui_label(ui, (struct rect){bx + bw + 6, y, 40, rh}, b, &font_bold, ink, ALIGN_LEFT);
        if (ui->c && bw > 20 && hi > lo) {
            struct rect track = {bx, y + rh / 2 - 2, bw, 5};
            gfx_round(ui->c, track, 2, ALPHA(RGB(0, 0, 0), 60));
            int x0 = bx + (f->low[i] - lo) * bw / (hi - lo), x1 = bx + (f->high[i] - lo) * bw / (hi - lo);
            gfx_round_gradient(ui->c, (struct rect){x0, track.y, MAX(x1 - x0, 5), 5}, 2, RGB(120, 200, 255),
                               RGB(255, 180, 60));
        }
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct weather *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct weather *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    if (open_win == w)
        open_win = NULL;
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct weather *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    load(s);
    s->pending = true;
    open_win = wm_open("Weather", ICON_WEATHER, 780, 580, &ops, s, WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!open_win) {
        kfree(s);
        return;
    }
    open_win->toolbar_h = TOOLBAR_HEIGHT;
    wm_set_timer(open_win, 300);
}

static const struct gui_app weather_app = {
    .name = "Weather", .description = "Forecasts for your places from open-meteo.com", .icon = ICON_WEATHER,
    .launch = launch};
GUI_APP(weather_app);
