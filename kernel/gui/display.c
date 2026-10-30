#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>

#include "internal.h"

/* Display brightness and Night Shift. Screens without a backlight the kernel can drive are dimmed in software: the
   compositor maps every color through a table on its way to the framebuffer. Night Shift "auto" is warm from 21:00
   to 7:00 local time. */

static int brightness = 100, warmth = 50;
static enum { NIGHT_OFF, NIGHT_ON, NIGHT_AUTO } night;
static bool warm_now;

static void update(void)
{
    if (night == NIGHT_AUTO) {
        struct rtc_time t;
        time_local(&t);
        warm_now = t.hour >= 21 || t.hour < 7;
    } else {
        warm_now = night == NIGHT_ON;
    }
    wm_set_tint(brightness, warm_now ? MAX(warmth, 5) : 0);
}

void display_tick(void)
{
    if (night == NIGHT_AUTO)
        update();
}

int display_brightness(void)
{
    return brightness;
}

bool display_night(void)
{
    return warm_now;
}

static void get_brightness(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", brightness);
}

static int set_brightness(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 10 || v > 100)
        return E_INVAL;
    brightness = (int)v;
    update();
    return 0;
}

static void get_night(char *buf, size_t cap)
{
    str_copy(buf, night == NIGHT_AUTO ? "auto" : night == NIGHT_ON ? "on" : "off", cap);
}

static int set_night(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!strcmp(value, "auto"))
        night = NIGHT_AUTO;
    else {
        bool on;
        if (setting_flag(value, &on))
            return E_INVAL;
        night = on ? NIGHT_ON : NIGHT_OFF;
    }
    update();
    return 0;
}

static void get_warmth(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", warmth);
}

static int set_warmth(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 0 || v > 100)
        return E_INVAL;
    warmth = (int)v;
    update();
    return 0;
}

static const struct setting s_brightness = {"brightness", "10..100", "display brightness", get_brightness,
                                            set_brightness};
static const struct setting s_night = {"nightshift", "on|off|auto", "warmer colors (auto: from 21:00 to 7:00)",
                                       get_night, set_night};
static const struct setting s_warmth = {"nightwarmth", "0..100", "how warm Night Shift makes the screen", get_warmth,
                                        set_warmth};
SETTING(s_brightness);
SETTING(s_night);
SETTING(s_warmth);
