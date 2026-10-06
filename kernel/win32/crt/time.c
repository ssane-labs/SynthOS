#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/string.h>
#include <synth/time.h>

#include "crt.h"

struct crt_tm {
    int sec, min, hour, mday, mon, year, wday, yday, isdst;
};

static W32_STATE uint64_t start_ms;
static W32_STATE struct crt_tm tm_buf;
static W32_STATE char text_buf[32];
static W32_STATE long crt_timezone;
static W32_STATE int crt_daylight;
static char *crt_tzname[2] = {"UTC", "UTC"};

void crt_time_reset(void)
{
    start_ms = hal_ticks_ms();
}

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool to_tm(int64_t t, struct crt_tm *out)
{
    if (t < 0)
        return false;
    int64_t days = t / 86400, rem = t % 86400;
    out->hour = (int)(rem / 3600);
    out->min = (int)(rem / 60 % 60);
    out->sec = (int)(rem % 60);
    out->wday = (int)((days + 4) % 7);
    int y = 1970;
    for (;;) {
        int len = leap(y) ? 366 : 365;
        if (days < len)
            break;
        days -= len;
        y++;
    }
    out->year = y - 1900;
    out->yday = (int)days;
    static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = 0;
    for (; m < 11; m++) {
        int dm = md[m] + (m == 1 && leap(y));
        if (days < dm)
            break;
        days -= dm;
    }
    out->mon = m;
    out->mday = (int)days + 1;
    out->isdst = 0;
    return true;
}

static int64_t from_tm(struct crt_tm *tm)
{
    int y = tm->year + 1900 + tm->mon / 12, m = tm->mon % 12;
    if (m < 0) {
        m += 12;
        y--;
    }
    int64_t t = days_from_civil(y, m + 1, 1) * 86400 + (int64_t)(tm->mday - 1) * 86400 + (int64_t)tm->hour * 3600 +
                (int64_t)tm->min * 60 + tm->sec;
    if (t < 0)
        return -1;
    to_tm(t, tm);
    return t;
}

static int32_t crt_time32(int32_t *out)
{
    int32_t t = (int32_t)time_now();
    if (out)
        *out = t;
    return t;
}

static int64_t crt_time64(int64_t *out)
{
    int64_t t = time_now();
    if (out)
        *out = t;
    return t;
}

static struct crt_tm *crt_gmtime32(const int32_t *t)
{
    return to_tm(*t, &tm_buf) ? &tm_buf : NULL;
}

static struct crt_tm *crt_gmtime64(const int64_t *t)
{
    return to_tm(*t, &tm_buf) ? &tm_buf : NULL;
}

static int crt_gmtime32_s(struct crt_tm *out, const int32_t *t)
{
    return to_tm(*t, out) ? 0 : CRT_EINVAL;
}

static int crt_gmtime64_s(struct crt_tm *out, const int64_t *t)
{
    return to_tm(*t, out) ? 0 : CRT_EINVAL;
}

static int32_t crt_mktime32(struct crt_tm *tm)
{
    return (int32_t)from_tm(tm);
}

static int64_t crt_mktime64(struct crt_tm *tm)
{
    return from_tm(tm);
}

static const char *const wday_short[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const wday_long[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const mon_short[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const mon_long[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};

static char *crt_asctime(const struct crt_tm *tm)
{
    ksnprintf(text_buf, sizeof(text_buf), "%s %s %2d %02d:%02d:%02d %d\n", wday_short[tm->wday % 7],
              mon_short[tm->mon % 12], tm->mday, tm->hour, tm->min, tm->sec, tm->year + 1900);
    return text_buf;
}

static char *crt_ctime32(const int32_t *t)
{
    struct crt_tm *tm = crt_gmtime32(t);
    return tm ? crt_asctime(tm) : NULL;
}

static char *crt_ctime64(const int64_t *t)
{
    struct crt_tm *tm = crt_gmtime64(t);
    return tm ? crt_asctime(tm) : NULL;
}

static size_t crt_strftime(char *out, size_t cap, const char *fmt, const struct crt_tm *tm)
{
    size_t len = 0;
    char tmp[64];
    for (const char *p = fmt; *p; p++) {
        const char *piece = tmp;
        tmp[0] = 0;
        if (*p != '%') {
            tmp[0] = *p;
            tmp[1] = 0;
        } else {
            p++;
            if (*p == '#')
                p++;
            switch (*p) {
            case 'a':
                piece = wday_short[tm->wday % 7];
                break;
            case 'A':
                piece = wday_long[tm->wday % 7];
                break;
            case 'b':
            case 'h':
                piece = mon_short[tm->mon % 12];
                break;
            case 'B':
                piece = mon_long[tm->mon % 12];
                break;
            case 'c':
                ksnprintf(tmp, sizeof(tmp), "%02d/%02d/%02d %02d:%02d:%02d", tm->mon + 1, tm->mday, tm->year % 100,
                          tm->hour, tm->min, tm->sec);
                break;
            case 'd':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->mday);
                break;
            case 'e':
                ksnprintf(tmp, sizeof(tmp), "%2d", tm->mday);
                break;
            case 'D':
            case 'x':
                ksnprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->mon + 1, tm->mday, tm->year % 100);
                break;
            case 'F':
                ksnprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->year + 1900, tm->mon + 1, tm->mday);
                break;
            case 'H':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->hour);
                break;
            case 'I':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->hour % 12 ? tm->hour % 12 : 12);
                break;
            case 'j':
                ksnprintf(tmp, sizeof(tmp), "%03d", tm->yday + 1);
                break;
            case 'm':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->mon + 1);
                break;
            case 'M':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->min);
                break;
            case 'n':
                piece = "\n";
                break;
            case 't':
                piece = "\t";
                break;
            case 'p':
                piece = tm->hour < 12 ? "AM" : "PM";
                break;
            case 'r':
                ksnprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s", tm->hour % 12 ? tm->hour % 12 : 12, tm->min, tm->sec,
                          tm->hour < 12 ? "AM" : "PM");
                break;
            case 'R':
                ksnprintf(tmp, sizeof(tmp), "%02d:%02d", tm->hour, tm->min);
                break;
            case 'S':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->sec);
                break;
            case 'T':
            case 'X':
                ksnprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->hour, tm->min, tm->sec);
                break;
            case 'u':
                ksnprintf(tmp, sizeof(tmp), "%d", tm->wday ? tm->wday : 7);
                break;
            case 'w':
                ksnprintf(tmp, sizeof(tmp), "%d", tm->wday);
                break;
            case 'U':
                ksnprintf(tmp, sizeof(tmp), "%02d", (tm->yday + 7 - tm->wday) / 7);
                break;
            case 'W':
                ksnprintf(tmp, sizeof(tmp), "%02d", (tm->yday + 7 - (tm->wday + 6) % 7) / 7);
                break;
            case 'y':
                ksnprintf(tmp, sizeof(tmp), "%02d", tm->year % 100);
                break;
            case 'Y':
                ksnprintf(tmp, sizeof(tmp), "%d", tm->year + 1900);
                break;
            case 'z':
                piece = "+0000";
                break;
            case 'Z':
                piece = crt_tzname[0];
                break;
            case '%':
                piece = "%";
                break;
            case 0:
                p--;
                break;
            default:
                break;
            }
        }
        size_t n = strlen(piece);
        if (len + n + 1 > cap)
            return 0;
        memcpy(out + len, piece, n);
        len += n;
    }
    out[len] = 0;
    return len;
}

static size_t crt_wcsftime(WCHAR *out, size_t cap, const WCHAR *fmt, const struct crt_tm *tm)
{
    char *f = w32_narrow(fmt);
    char *tmp = w32_alloc(cap * 4 + 1);
    size_t n = crt_strftime(tmp, cap * 4 + 1, f, tm);
    w32_free(f);
    if (!n || n + 1 > cap) {
        w32_free(tmp);
        return 0;
    }
    w32_cp_to_wide(tmp, (int)n + 1, out, cap, 0);
    w32_free(tmp);
    return n;
}

static char *crt_strdate(char *buf)
{
    struct crt_tm tm;
    to_tm(time_now(), &tm);
    ksnprintf(buf, 9, "%02d/%02d/%02d", tm.mon + 1, tm.mday, tm.year % 100);
    return buf;
}

static char *crt_strtime(char *buf)
{
    struct crt_tm tm;
    to_tm(time_now(), &tm);
    ksnprintf(buf, 9, "%02d:%02d:%02d", tm.hour, tm.min, tm.sec);
    return buf;
}

static long crt_clock(void)
{
    return (long)(hal_ticks_ms() - start_ms);
}

static void crt_ftime32(uint8_t *out)
{
    uint32_t t = time_now();
    uint16_t ms = (uint16_t)(hal_ticks_ms() % 1000);
    memset(out, 0, 12);
    memcpy(out, &t, 4);
    memcpy(out + 4, &ms, 2);
}

static void crt_ftime64(uint8_t *out)
{
    uint64_t t = time_now();
    uint16_t ms = (uint16_t)(hal_ticks_ms() % 1000);
    memset(out, 0, 16);
    memcpy(out, &t, 8);
    memcpy(out + 8, &ms, 2);
}

static void crt_tzset(void)
{
}

static long *p_timezone(void) { return &crt_timezone; }
static int *p_daylight(void) { return &crt_daylight; }
static char ***p_tzname(void)
{
    static char **names = crt_tzname;
    return &names;
}

static int crt_get_timezone(long *out)
{
    *out = 0;
    return 0;
}

static int crt_get_daylight(int *out)
{
    *out = 0;
    return 0;
}

static int crt_utime(const char *path, void *times)
{
    UNUSED(times);
    char p[PATH_MAX_LEN];
    return w32_path(path, p, sizeof(p)) == 0 && vfs_exists(p) ? 0 : -1;
}

static const struct w32_export time_exports[] = {
    W32_AS("time", crt_time32), W32_AS("_time32", crt_time32), W32_AS("_time64", crt_time64),
    W32_AS("gmtime", crt_gmtime32), W32_AS("_gmtime32", crt_gmtime32), W32_AS("_gmtime64", crt_gmtime64),
    W32_AS("localtime", crt_gmtime32), W32_AS("_localtime32", crt_gmtime32), W32_AS("_localtime64", crt_gmtime64),
    W32_AS("gmtime_s", crt_gmtime32_s), W32_AS("_gmtime32_s", crt_gmtime32_s), W32_AS("_gmtime64_s", crt_gmtime64_s),
    W32_AS("localtime_s", crt_gmtime32_s), W32_AS("_localtime32_s", crt_gmtime32_s),
    W32_AS("_localtime64_s", crt_gmtime64_s), W32_AS("mktime", crt_mktime32), W32_AS("_mktime32", crt_mktime32),
    W32_AS("_mktime64", crt_mktime64), W32_AS("_mkgmtime", crt_mktime32), W32_AS("_mkgmtime32", crt_mktime32),
    W32_AS("_mkgmtime64", crt_mktime64), W32_AS("asctime", crt_asctime), W32_AS("ctime", crt_ctime32),
    W32_AS("_ctime32", crt_ctime32), W32_AS("_ctime64", crt_ctime64), W32_AS("strftime", crt_strftime),
    W32_AS("wcsftime", crt_wcsftime), W32_AS("_strdate", crt_strdate), W32_AS("_strtime", crt_strtime),
    W32_AS("clock", crt_clock), W32_AS("_ftime", crt_ftime32), W32_AS("_ftime32", crt_ftime32),
    W32_AS("_ftime64", crt_ftime64), W32_AS("_tzset", crt_tzset), W32_AS("_timezone", &crt_timezone),
    W32_AS("_daylight", &crt_daylight), W32_AS("_tzname", crt_tzname), W32_AS("__p__timezone", p_timezone),
    W32_AS("__p__daylight", p_daylight), W32_AS("__p__tzname", p_tzname), W32_AS("_get_timezone", crt_get_timezone),
    W32_AS("_get_daylight", crt_get_daylight), W32_AS("_utime", crt_utime), W32_AS("_utime32", crt_utime),
    W32_AS("_utime64", crt_utime),
};

CRT_EXPORTS(crt_time_module, time_exports);
