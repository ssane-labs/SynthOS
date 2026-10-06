#include <synth/fmt.h>
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
