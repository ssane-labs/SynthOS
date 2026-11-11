#include <synth/fmt.h>
#include <synth/string.h>

#include "win32.h"

typedef struct {
    WORD year, month, weekday, day, hour, minute, second, ms;
} SYSTEMTIME;

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* OLE dates count days from 1899-12-30; the fraction is the time of day. */
static const int64_t ole_epoch = -25569;

static INT WINAPI SystemTimeToVariantTime(const SYSTEMTIME *st, double *out)
{
    if (!st || !out || st->month < 1 || st->month > 12 || st->day < 1 || st->day > 31 || st->year > 9999)
        return FALSE;
    int64_t days = days_from_civil(st->year, st->month, st->day) - ole_epoch;
    double frac = (st->hour * 3600.0 + st->minute * 60.0 + st->second) / 86400.0;
    *out = days >= 0 ? (double)days + frac : (double)days - frac;
    return TRUE;
}

static INT WINAPI VariantTimeToSystemTime(double v, SYSTEMTIME *st)
{
    if (!st || v < -657434.0 || v > 2958465.9999)
        return FALSE;
    int64_t days = (int64_t)v;
    double frac = v - (double)days;
    if (frac < 0)
        frac = -frac;
    int secs = (int)(frac * 86400.0 + 0.5);
    if (secs >= 86400) {
        secs -= 86400;
        days += v < 0 ? -1 : 1;
    }
    int y, m, d;
    civil_from_days(days + ole_epoch, &y, &m, &d);
    memset(st, 0, sizeof(*st));
    st->year = (uint16_t)y;
    st->month = (uint16_t)m;
    st->day = (uint16_t)d;
    st->weekday = (uint16_t)(((days + ole_epoch) % 7 + 11) % 7);
    st->hour = (uint16_t)(secs / 3600);
    st->minute = (uint16_t)(secs / 60 % 60);
    st->second = (uint16_t)(secs % 60);
    return TRUE;
}

/* the US English form: "11/11/2026 3:04:05 PM", the date or the time alone when the other part is zero */
static uint32_t WINAPI VarBstrFromDate(double v, DWORD lcid, DWORD flags, WCHAR **out)
{
    UNUSED(lcid);
    SYSTEMTIME st;
    if (!out || !VariantTimeToSystemTime(v, &st))
        return 0x80070057u;
    char text[48], date[24] = "", time[24] = "";
    bool no_time = (flags & 2) || (!st.hour && !st.minute && !st.second);
    bool no_date = (flags & 1) || (int64_t)v == 0;
    if (!no_date || no_time)
        ksnprintf(date, sizeof(date), "%u/%u/%u", st.month, st.day, st.year);
    if (!no_time)
        ksnprintf(time, sizeof(time), "%u:%02u:%02u %s", st.hour % 12 ? st.hour % 12 : 12, st.minute, st.second,
                  st.hour < 12 ? "AM" : "PM");
    ksnprintf(text, sizeof(text), "%s%s%s", date, *date && *time ? " " : "", time);
    WCHAR wide[48];
    size_t i = 0;
    for (; text[i]; i++)
        wide[i] = (WCHAR)text[i];
    wide[i] = 0;
    WCHAR *(WINAPI * alloc)(const WCHAR *) = (WCHAR * (WINAPI *)(const WCHAR *)) w32_builtin_lookup("oleaut32.dll", "SysAllocString");
    *out = alloc ? alloc(wide) : NULL;
    return *out ? 0 : 0x8007000eu;
}

static const struct w32_export oleaut_fp_exports[] = {
    W32_FN(VarBstrFromDate),
    W32_FN(SystemTimeToVariantTime),
    W32_FN(VariantTimeToSystemTime),
};

W32_EXPORTS(oleaut_fp_module, "oleaut32.dll", oleaut_fp_exports);
