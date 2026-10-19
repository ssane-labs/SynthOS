#include <synth/fmt.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>

#include "internal.h"

/* How the menu bar, the status pill, the lock screen and the clock widget show the time. */

static bool clock24 = true, seconds, show_date = true;

void gui_clock_time(const struct rtc_time *t, char *buf, size_t cap, bool with_seconds, bool suffix)
{
    unsigned h = t->hour;
    if (!clock24)
        h = h % 12 ? h % 12 : 12;
    if (with_seconds)
        ksnprintf(buf, cap, clock24 ? "%02u:%02u:%02u" : "%u:%02u:%02u", h, t->minute, t->second);
    else
        ksnprintf(buf, cap, clock24 ? "%02u:%02u" : "%u:%02u", h, t->minute);
    if (!clock24 && suffix)
        str_append(buf, t->hour < 12 ? " AM" : " PM", cap);
}

void gui_clock_text(char *buf, size_t cap, bool date)
{
    struct rtc_time t;
    time_local(&t);
    char time[24];
    gui_clock_time(&t, time, sizeof(time), seconds, true);
    if (date && show_date)
        ksnprintf(buf, cap, "%s %u %s  %s", time_weekday_name(time_weekday(&t)), t.day, time_month_name(t.month),
                  time);
    else
        str_copy(buf, time, cap);
}

bool gui_clock_seconds(void)
{
    return seconds;
}

static void changed(void)
{
    if (gui_active())
        chrome_changed();
}

static void get_24(char *buf, size_t cap)
{
    setting_flag_get(clock24, buf, cap);
}

static int set_24(struct shell *sh, const char *value)
{
    UNUSED(sh);
    int err = setting_flag(value, &clock24);
    changed();
    return err;
}

static void get_seconds(char *buf, size_t cap)
{
    setting_flag_get(seconds, buf, cap);
}

static int set_seconds(struct shell *sh, const char *value)
{
    UNUSED(sh);
    int err = setting_flag(value, &seconds);
    changed();
    return err;
}

static void get_date(char *buf, size_t cap)
{
    setting_flag_get(show_date, buf, cap);
}

static int set_date(struct shell *sh, const char *value)
{
    UNUSED(sh);
    int err = setting_flag(value, &show_date);
    changed();
    return err;
}

static const struct setting s_24 = {"clock24", "on|off", "24-hour time on the desktop", get_24, set_24};
static const struct setting s_seconds = {"clockseconds", "on|off", "show seconds in the menu bar clock", get_seconds,
                                         set_seconds};
static const struct setting s_date = {"clockdate", "on|off", "show the date in the menu bar", get_date, set_date};
SETTING(s_24);
SETTING(s_seconds);
SETTING(s_date);
