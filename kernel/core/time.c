#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : days[m - 1];
}

uint32_t time_from_rtc(const struct rtc_time *t)
{
    uint32_t days = 0;
    for (int y = 1970; y < t->year; y++)
        days += leap(y) ? 366 : 365;
    for (int m = 1; m < t->month; m++)
        days += (uint32_t)days_in_month(t->year, m);
    days += (uint32_t)(t->day - 1);
    return days * 86400u + t->hour * 3600u + t->minute * 60u + t->second;
}

void time_to_rtc(uint32_t epoch, struct rtc_time *t)
{
    uint32_t days = epoch / 86400u, rem = epoch % 86400u;
    t->hour = (uint8_t)(rem / 3600);
    t->minute = (uint8_t)(rem % 3600 / 60);
    t->second = (uint8_t)(rem % 60);
    int y = 1970;
    while (days >= (uint32_t)(leap(y) ? 366 : 365))
        days -= leap(y) ? 366 : 365, y++;
    int m = 1;
    while (days >= (uint32_t)days_in_month(y, m))
        days -= (uint32_t)days_in_month(y, m), m++;
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)(days + 1);
}

uint32_t time_now(void)
{
    struct rtc_time t;
    hal_rtc(&t);
    return time_from_rtc(&t);
}

/* The hardware clock keeps UTC; clocks people read add the time zone. */
static int zone_minutes;

void time_local(struct rtc_time *t)
{
    hal_rtc(t);
    if (zone_minutes)
        time_to_rtc((uint32_t)((int64_t)time_from_rtc(t) + zone_minutes * 60), t);
}

int time_zone(void)
{
    return zone_minutes;
}

void time_zone_format(int minutes, char *buf, size_t cap)
{
    int m = minutes < 0 ? -minutes : minutes;
    ksnprintf(buf, cap, "%c%02d:%02d", minutes < 0 ? '-' : '+', m / 60, m % 60);
}

static void get_zone(char *buf, size_t cap)
{
    time_zone_format(zone_minutes, buf, cap);
}

static int set_zone(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!str_casecmp(value, "utc") || !str_casecmp(value, "gmt")) {
        zone_minutes = 0;
        return 0;
    }
    int sign = *value == '-' ? -1 : 1;
    if (*value == '+' || *value == '-')
        value++;
    long hours = 0, minutes = 0;
    char part[8];
    const char *colon = strchr(value, ':');
    str_copy(part, value, MIN(sizeof(part), colon ? (size_t)(colon - value) + 1 : sizeof(part)));
    if (!str_parse_long(part, &hours) || (colon && !str_parse_long(colon + 1, &minutes)) || minutes < 0 ||
        minutes > 59)
        return E_INVAL;
    int total = sign * (int)(hours * 60 + minutes);
    if (total < -12 * 60 || total > 14 * 60)
        return E_INVAL;
    zone_minutes = total;
    return 0;
}

static const struct setting s_zone = {"timezone", "UTC|+HH:MM|-HH:MM", "time zone for clocks, date and cal", get_zone,
                                      set_zone};
SETTING(s_zone);

int time_weekday(const struct rtc_time *t)
{
    return (int)((time_from_rtc(t) / 86400u + 4) % 7);
}

const char *time_weekday_name(int day)
{
    static const char *const names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return names[day % 7];
}

const char *time_month_name(int month)
{
    static const char *const names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return names[(month + 11) % 12];
}

void time_format(uint32_t epoch, char *buf, size_t cap)
{
    struct rtc_time t;
    time_to_rtc(epoch, &t);
    ksnprintf(buf, cap, "%04u-%02u-%02u %02u:%02u", t.year, t.month, t.day, t.hour, t.minute);
}
