#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/time.h>

#include "internal.h"

static struct window *calendar;

static int month_days(int y, int m)
{
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return days[(m - 1) % 12];
}

static void calendar_paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    struct rtc_time t;
    hal_rtc(&t);
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
