#include <synth/codepage.h>
#include <synth/fmt.h>
#include <synth/string.h>
#include <synth/time.h>

#include "win32.h"

#define LCMAP_LOWERCASE 0x100
#define LCMAP_UPPERCASE 0x200
#define NORM_IGNORECASE 0x1
#define LOCALE_RETURN_NUMBER 0x20000000u
#define LOCALE_NOUSEROVERRIDE 0x80000000u

WCHAR w32_towupper(WCHAR c)
{
    if (c < 0x80)
        return (WCHAR)to_upper(c);
    if ((c >= 0xe0 && c <= 0xfe && c != 0xf7) || (c >= 0x430 && c <= 0x44f))
        return (WCHAR)(c - 0x20);
    if (c >= 0x450 && c <= 0x45f)
        return (WCHAR)(c - 0x50);
    return c;
}

WCHAR w32_towlower(WCHAR c)
{
    if (c < 0x80)
        return (WCHAR)to_lower(c);
    if ((c >= 0xc0 && c <= 0xde && c != 0xd7) || (c >= 0x410 && c <= 0x42f))
        return (WCHAR)(c + 0x20);
    if (c >= 0x400 && c <= 0x40f)
        return (WCHAR)(c + 0x50);
    return c;
}

static bool w_alpha(WCHAR c)
{
    return c < 0x80 ? is_alpha(c) : (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) || (c >= 0x400 && c <= 0x4ff);
}

static int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int len, LPWSTR out, int cap)
{
    UNUSED(flags);
    if (!s || len == 0 || cap < 0) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t need = w32_cp_to_wide(s, len, NULL, 0, (int)cp);
    if (cap == 0)
        return (int)need;
    if (need > (size_t)cap) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    w32_cp_to_wide(s, len, out, (size_t)cap, (int)cp);
    return (int)need;
}

static int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int len, LPSTR out, int cap, LPCSTR def,
                                      BOOL *used_default)
{
    UNUSED(flags);
    UNUSED(def);
    if (used_default)
        *used_default = FALSE;
    if (!w || len == 0 || cap < 0) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t need = w32_wide_to_cp(w, len, NULL, 0, (int)cp);
    if (cap == 0)
        return (int)need;
    if (need > (size_t)cap) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    w32_wide_to_cp(w, len, out, (size_t)cap, (int)cp);
    return (int)need;
}

static UINT WINAPI GetACP(void)
{
    return (UINT)w32_acp();
}

static UINT WINAPI GetOEMCP(void)
{
    return (UINT)codepage_current();
}

static BOOL WINAPI IsValidCodePage(UINT cp)
{
    return cp == 65001 || cp == 1251 || cp == 1252 || cp == 28591 || cp == 437 || cp == 866;
}

static BOOL WINAPI GetCPInfo(UINT cp, uint8_t *info)
{
    memset(info, 0, 18);
    info[0] = cp == 65001 ? 4 : 1;
    info[4] = '?';
    return TRUE;
}

static BOOL WINAPI GetCPInfoExA(UINT cp, DWORD flags, uint8_t *info)
{
    UNUSED(flags);
    GetCPInfo(cp, info);
    memcpy(info + 18, &(WCHAR){'?'}, 2);
    memcpy(info + 20, &cp, 4);
    ksnprintf((char *)info + 24, 260, "%u", cp);
    return TRUE;
}

static BOOL WINAPI GetCPInfoExW(UINT cp, DWORD flags, uint8_t *info)
{
    UNUSED(flags);
    GetCPInfo(cp, info);
    memcpy(info + 18, &(WCHAR){'?'}, 2);
    memcpy(info + 20, &cp, 4);
    char tmp[16];
    ksnprintf(tmp, sizeof(tmp), "%u", cp);
    w32_cp_to_wide(tmp, -1, (WCHAR *)(info + 24), 16, 1);
    return TRUE;
}

static BOOL WINAPI IsDBCSLeadByte(BYTE c)
{
    UNUSED(c);
    return FALSE;
}

static BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE c)
{
    UNUSED(cp);
    UNUSED(c);
    return FALSE;
}

static int WINAPI lstrlenA(LPCSTR s)
{
    return s ? (int)strlen(s) : 0;
}

static int WINAPI lstrlenW(LPCWSTR s)
{
    return s ? (int)w32_wcslen(s) : 0;
}

static LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s)
{
    memmove(d, s, strlen(s) + 1);
    return d;
}

static LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s)
{
    memmove(d, s, (w32_wcslen(s) + 1) * sizeof(WCHAR));
    return d;
}

static LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n)
{
    if (n <= 0)
        return d;
    int i = 0;
    for (; i < n - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
    return d;
}

static LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n)
{
    if (n <= 0)
        return d;
    int i = 0;
    for (; i < n - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
    return d;
}

static LPSTR WINAPI lstrcatA(LPSTR d, LPCSTR s)
{
    lstrcpyA(d + strlen(d), s);
    return d;
}

static LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s)
{
    lstrcpyW(d + w32_wcslen(d), s);
    return d;
}

static int wcompare(LPCWSTR a, int alen, LPCWSTR b, int blen, bool icase)
{
    if (!a || !b)
        return a == b ? 0 : a ? 1 : -1;
    size_t na = alen < 0 ? w32_wcslen(a) : (size_t)alen, nb = blen < 0 ? w32_wcslen(b) : (size_t)blen;
    for (size_t i = 0; i < na && i < nb; i++) {
        WCHAR x = icase ? w32_towlower(a[i]) : a[i], y = icase ? w32_towlower(b[i]) : b[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return na == nb ? 0 : na < nb ? -1 : 1;
}

static int acompare(LPCSTR a, int alen, LPCSTR b, int blen, bool icase)
{
    if (!a || !b)
        return a == b ? 0 : a ? 1 : -1;
    WCHAR *wa = w32_widen(a), *wb = w32_widen(b);
    if (alen >= 0)
        alen = (int)MIN((size_t)alen, w32_wcslen(wa));
    if (blen >= 0)
        blen = (int)MIN((size_t)blen, w32_wcslen(wb));
    int r = wcompare(wa, alen, wb, blen, icase);
    w32_free(wa);
    w32_free(wb);
    return r;
}

static int WINAPI lstrcmpA(LPCSTR a, LPCSTR b)
{
    return acompare(a, -1, b, -1, false);
}

static int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b)
{
    return acompare(a, -1, b, -1, true);
}

static int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b)
{
    return wcompare(a, -1, b, -1, false);
}

static int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b)
{
    return wcompare(a, -1, b, -1, true);
}

static int WINAPI CompareStringA(DWORD lcid, DWORD flags, LPCSTR a, int alen, LPCSTR b, int blen)
{
    UNUSED(lcid);
    return acompare(a, alen, b, blen, flags & NORM_IGNORECASE) + 2;
}

static int WINAPI CompareStringW(DWORD lcid, DWORD flags, LPCWSTR a, int alen, LPCWSTR b, int blen)
{
    UNUSED(lcid);
    return wcompare(a, alen, b, blen, flags & NORM_IGNORECASE) + 2;
}

static int WINAPI CompareStringEx(LPCWSTR locale, DWORD flags, LPCWSTR a, int alen, LPCWSTR b, int blen,
                                  void *v, void *r, uint32_t p)
{
    UNUSED(locale);
    UNUSED(v);
    UNUSED(r);
    UNUSED(p);
    return wcompare(a, alen, b, blen, flags & NORM_IGNORECASE) + 2;
}

static int WINAPI CompareStringOrdinal(LPCWSTR a, int alen, LPCWSTR b, int blen, BOOL icase)
{
    return wcompare(a, alen, b, blen, icase) + 2;
}

static int map_wide(DWORD flags, LPCWSTR src, int len, LPWSTR dst, int cap)
{
    size_t n = len < 0 ? w32_wcslen(src) + 1 : (size_t)len;
    if (cap == 0)
        return (int)n;
    if (n > (size_t)cap) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (size_t i = 0; i < n; i++)
        dst[i] = flags & LCMAP_UPPERCASE ? w32_towupper(src[i]) : flags & LCMAP_LOWERCASE ? w32_towlower(src[i])
                                                                                          : src[i];
    return (int)n;
}

static int WINAPI LCMapStringW(DWORD lcid, DWORD flags, LPCWSTR src, int len, LPWSTR dst, int cap)
{
    UNUSED(lcid);
    return map_wide(flags, src, len, dst, cap);
}

static int WINAPI LCMapStringEx(LPCWSTR locale, DWORD flags, LPCWSTR src, int len, LPWSTR dst, int cap, void *v,
                                void *r, uint32_t p)
{
    UNUSED(locale);
    UNUSED(v);
    UNUSED(r);
    UNUSED(p);
    return map_wide(flags, src, len, dst, cap);
}

static int WINAPI LCMapStringA(DWORD lcid, DWORD flags, LPCSTR src, int len, LPSTR dst, int cap)
{
    UNUSED(lcid);
    size_t n = len < 0 ? strlen(src) + 1 : (size_t)len;
    if (cap == 0)
        return (int)n;
    if (n > (size_t)cap) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        WCHAR w;
        w32_cp_to_wide(src + i, 1, &w, 1, 0);
        w = flags & LCMAP_UPPERCASE ? w32_towupper(w) : flags & LCMAP_LOWERCASE ? w32_towlower(w) : w;
        w32_wide_to_cp(&w, 1, dst + i, 1, 0);
    }
    return (int)n;
}

static WORD ctype1(WCHAR c)
{
    WORD t = 0x200;
    if (w_alpha(c)) {
        t |= 0x100;
        if (w32_towlower(c) != c)
            t |= 0x1;
        else if (w32_towupper(c) != c)
            t |= 0x2;
    }
    if (c >= '0' && c <= '9')
        t |= 0x4 | 0x80;
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
        t |= 0x80;
    if (c == ' ' || (c >= 9 && c <= 13) || c == 0xa0)
        t |= 0x8;
    if (c == ' ' || c == '\t' || c == 0xa0)
        t |= 0x40;
    if (c < 0x20 || c == 0x7f)
        t |= 0x20;
    if ((c > 0x20 && c < 0x7f && !is_alnum(c)) || (c >= 0xa1 && c < 0xc0))
        t |= 0x10;
    return t;
}

static BOOL WINAPI GetStringTypeW(DWORD type, LPCWSTR src, int len, WORD *out)
{
    size_t n = len < 0 ? w32_wcslen(src) + 1 : (size_t)len;
    for (size_t i = 0; i < n; i++)
        out[i] = type == 1 ? ctype1(src[i]) : 0;
    return TRUE;
}

static BOOL WINAPI GetStringTypeExW(DWORD lcid, DWORD type, LPCWSTR src, int len, WORD *out)
{
    UNUSED(lcid);
    return GetStringTypeW(type, src, len, out);
}

static BOOL WINAPI GetStringTypeA(DWORD lcid, DWORD type, LPCSTR src, int len, WORD *out)
{
    UNUSED(lcid);
    size_t n = len < 0 ? strlen(src) + 1 : (size_t)len;
    for (size_t i = 0; i < n; i++) {
        WCHAR w;
        w32_cp_to_wide(src + i, 1, &w, 1, 0);
        out[i] = type == 1 ? ctype1(w) : 0;
    }
    return TRUE;
}

static DWORD default_lcid(void)
{
    return codepage_current() == 866 ? 0x419 : 0x409;
}

static const char *locale_value(DWORD type, char *tmp, size_t cap)
{
    bool ru = default_lcid() == 0x419;
    switch (type & 0xffff) {
    case 0x01:
        ksnprintf(tmp, cap, "%04x", (unsigned)default_lcid());
        return tmp;
    case 0x02:
    case 0x1001:
        return ru ? "Russian" : "English";
    case 0x03:
        return ru ? "RUS" : "ENU";
    case 0x04:
        return ru ? "русский" : "English";
    case 0x06:
    case 0x1002:
        return ru ? "Russia" : "United States";
    case 0x07:
        return ru ? "RUS" : "USA";
    case 0x0b:
        ksnprintf(tmp, cap, "%d", codepage_current());
        return tmp;
    case 0x1004:
        ksnprintf(tmp, cap, "%d", w32_acp());
        return tmp;
    case 0x0c:
        return ru ? ";" : ",";
    case 0x0e:
        return ru ? "," : ".";
    case 0x0f:
        return ru ? "\xa0" : ",";
    case 0x10:
        return "3;0";
    case 0x11:
        return "2";
    case 0x14:
        return ru ? "\xa4" : "$";
    case 0x15:
        return "USD";
    case 0x16:
        return ".";
    case 0x17:
        return ",";
    case 0x1d:
        return ru ? "." : "/";
    case 0x1e:
        return ":";
    case 0x1f:
        return ru ? "dd.MM.yyyy" : "M/d/yyyy";
    case 0x20:
        return "dddd, MMMM d, yyyy";
    case 0x1003:
        return "HH:mm:ss";
    case 0x28:
        return "AM";
    case 0x29:
        return "PM";
    case 0x59:
        return ru ? "ru" : "en";
    case 0x5a:
        return ru ? "RU" : "US";
    case 0x5c:
        return ru ? "ru-RU" : "en-US";
    case 0x1009:
        return "0";
    default:
        return "";
    }
}

static int locale_info(DWORD type, char *out, int cap, bool *number, DWORD *value)
{
    char tmp[16];
    const char *v = locale_value(type, tmp, sizeof(tmp));
    *number = type & LOCALE_RETURN_NUMBER;
    if (*number) {
        *value = (DWORD)str_to_long(v, NULL, (type & 0xffff) == 1 ? 16 : 10);
        return 0;
    }
    int n = (int)strlen(v) + 1;
    if (cap && out) {
        if (n > cap) {
            w32_set_error(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memcpy(out, v, (size_t)n);
    }
    return n;
}

static int WINAPI GetLocaleInfoA(DWORD lcid, DWORD type, LPSTR out, int cap)
{
    UNUSED(lcid);
    bool number;
    DWORD value;
    int n = locale_info(type, out, cap, &number, &value);
    if (number) {
        if (cap >= 4)
            memcpy(out, &value, 4);
        return 2;
    }
    return n;
}

static int WINAPI GetLocaleInfoW(DWORD lcid, DWORD type, LPWSTR out, int cap)
{
    UNUSED(lcid);
    char tmp[64];
    bool number;
    DWORD value;
    int n = locale_info(type, tmp, sizeof(tmp), &number, &value);
    if (number) {
        if (cap >= 2)
            memcpy(out, &value, 4);
        return 2;
    }
    size_t wn = w32_cp_to_wide(tmp, n, NULL, 0, 0);
    if (!cap || !out)
        return (int)wn;
    if (wn > (size_t)cap) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    w32_cp_to_wide(tmp, n, out, (size_t)cap, 0);
    return (int)wn;
}

static int WINAPI GetLocaleInfoEx(LPCWSTR name, DWORD type, LPWSTR out, int cap)
{
    UNUSED(name);
    return GetLocaleInfoW(0, type, out, cap);
}

static DWORD WINAPI GetUserDefaultLCID(void)
{
    return default_lcid();
}

static WORD WINAPI GetUserDefaultLangID(void)
{
    return (WORD)default_lcid();
}

static BOOL WINAPI IsValidLocale(DWORD lcid, DWORD flags)
{
    UNUSED(flags);
    return lcid == 0x409 || lcid == 0x419 || lcid == 0x400 || lcid == 0x800 || lcid == 0;
}

static BOOL WINAPI SetThreadLocale(DWORD lcid)
{
    UNUSED(lcid);
    return TRUE;
}

static int WINAPI GetUserDefaultLocaleName(LPWSTR out, int cap)
{
    return (int)w32_cp_to_wide(default_lcid() == 0x419 ? "ru-RU" : "en-US", -1, out, (size_t)cap, 0);
}

static DWORD WINAPI LocaleNameToLCID(LPCWSTR name, DWORD flags)
{
    UNUSED(flags);
    return name && (name[0] == 'r' || name[0] == 'R') ? 0x419 : name && name[0] ? 0x409 : default_lcid();
}

static int WINAPI LCIDToLocaleName(DWORD lcid, LPWSTR out, int cap, DWORD flags)
{
    UNUSED(flags);
    return (int)w32_cp_to_wide(lcid == 0x419 ? "ru-RU" : "en-US", -1, out, (size_t)cap, 0);
}

static BOOL WINAPI EnumSystemLocalesA(void *fn, DWORD flags)
{
    UNUSED(flags);
    uint32_t arg = (uint32_t)(uintptr_t) "00000409";
    w32_call(fn, 1, &arg);
    return TRUE;
}

static BOOL WINAPI EnumSystemLocalesW(void *fn, DWORD flags)
{
    UNUSED(flags);
    static const WCHAR name[] = {'0', '0', '0', '0', '0', '4', '0', '9', 0};
    uint32_t arg = (uint32_t)(uintptr_t)name;
    w32_call(fn, 1, &arg);
    return TRUE;
}

static int WINAPI GetNumberFormatA(DWORD lcid, DWORD flags, LPCSTR value, const void *fmt, LPSTR out, int cap)
{
    UNUSED(lcid);
    UNUSED(flags);
    UNUSED(fmt);
    int n = (int)strlen(value) + 1;
    if (cap && n <= cap)
        memcpy(out, value, (size_t)n);
    return n;
}

static int date_format(const uint16_t *st, LPCSTR fmt, char *out, size_t cap, bool time)
{
    uint16_t tm[8] = {0};
    if (st)
        memcpy(tm, st, sizeof(tm));
    else {
        struct rtc_time t;
        time_to_rtc(time_now(), &t);
        uint16_t v[8] = {t.year, t.month, 0, t.day, t.hour, t.minute, t.second, 0};
        memcpy(tm, v, sizeof(v));
    }
    if (!fmt)
        fmt = time ? "HH:mm:ss" : "M/d/yyyy";
    size_t len = 0;
    char tmp[16];
    for (const char *p = fmt; *p;) {
        char c = *p;
        int run = 1;
        while (p[run] == c)
            run++;
        tmp[0] = 0;
        switch (c) {
        case 'y':
            ksnprintf(tmp, sizeof(tmp), run >= 4 ? "%04u" : "%02u", run >= 4 ? tm[0] : tm[0] % 100);
            break;
        case 'M':
            ksnprintf(tmp, sizeof(tmp), run >= 2 ? "%02u" : "%u", tm[1]);
            break;
        case 'd':
            ksnprintf(tmp, sizeof(tmp), run >= 2 ? "%02u" : "%u", tm[3]);
            break;
        case 'H':
        case 'h':
            ksnprintf(tmp, sizeof(tmp), run >= 2 ? "%02u" : "%u",
                      c == 'H' ? tm[4] : (tm[4] % 12 ? tm[4] % 12 : 12));
            break;
        case 'm':
            ksnprintf(tmp, sizeof(tmp), run >= 2 ? "%02u" : "%u", tm[5]);
            break;
        case 's':
            ksnprintf(tmp, sizeof(tmp), run >= 2 ? "%02u" : "%u", tm[6]);
            break;
        case 't':
            str_copy(tmp, tm[4] < 12 ? "AM" : "PM", run >= 2 ? 3 : 2);
            break;
        default:
            run = 1;
            tmp[0] = c;
            tmp[1] = 0;
            break;
        }
        for (const char *q = tmp; *q; q++) {
            if (out && len + 1 < cap)
                out[len] = *q;
            len++;
        }
        p += run;
    }
    if (out && cap)
        out[MIN(len, cap - 1)] = 0;
    return (int)len + 1;
}

static int WINAPI GetDateFormatA(DWORD lcid, DWORD flags, const uint16_t *st, LPCSTR fmt, LPSTR out, int cap)
{
    UNUSED(lcid);
    UNUSED(flags);
    return date_format(st, fmt, out, (size_t)cap, false);
}

static int WINAPI GetTimeFormatA(DWORD lcid, DWORD flags, const uint16_t *st, LPCSTR fmt, LPSTR out, int cap)
{
    UNUSED(lcid);
    UNUSED(flags);
    return date_format(st, fmt, out, (size_t)cap, true);
}

static int wide_date(const uint16_t *st, LPCWSTR fmt, LPWSTR out, int cap, bool time)
{
    char *f = fmt ? w32_narrow(fmt) : NULL, tmp[128];
    int n = date_format(st, f, tmp, sizeof(tmp), time);
    w32_free(f);
    if (cap && out)
        w32_cp_to_wide(tmp, -1, out, (size_t)cap, 0);
    return n;
}

static int WINAPI GetDateFormatW(DWORD lcid, DWORD flags, const uint16_t *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    UNUSED(lcid);
    UNUSED(flags);
    return wide_date(st, fmt, out, cap, false);
}

static int WINAPI GetTimeFormatW(DWORD lcid, DWORD flags, const uint16_t *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    UNUSED(lcid);
    UNUSED(flags);
    return wide_date(st, fmt, out, cap, true);
}

static int WINAPI GetDateFormatEx(LPCWSTR name, DWORD flags, const uint16_t *st, LPCWSTR fmt, LPWSTR out, int cap,
                                   LPCWSTR calendar)
{
    UNUSED(name);
    UNUSED(flags);
    UNUSED(calendar);
    return wide_date(st, fmt, out, cap, false);
}

static int WINAPI GetTimeFormatEx(LPCWSTR name, DWORD flags, const uint16_t *st, LPCWSTR fmt, LPWSTR out, int cap)
{
    UNUSED(name);
    UNUSED(flags);
    return wide_date(st, fmt, out, cap, true);
}

static int WINAPI GetNumberFormatW(DWORD lcid, DWORD flags, LPCWSTR value, const void *fmt, LPWSTR out, int cap)
{
    UNUSED(lcid);
    UNUSED(flags);
    UNUSED(fmt);
    int n = (int)w32_wcslen(value) + 1;
    if (cap && out) {
        if (n > cap) {
            w32_set_error(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memcpy(out, value, (size_t)n * sizeof(WCHAR));
    }
    return n;
}

static int WINAPI GetNumberFormatEx(LPCWSTR name, DWORD flags, LPCWSTR value, const void *fmt, LPWSTR out, int cap)
{
    UNUSED(name);
    return GetNumberFormatW(0, flags, value, fmt, out, cap);
}

static BOOL WINAPI IsValidLocaleName(LPCWSTR name)
{
    return name && LocaleNameToLCID(name, 0) != 0;
}

static int WINAPI ResolveLocaleName(LPCWSTR name, LPWSTR out, int cap)
{
    static const WCHAR fallback[] = {'e', 'n', '-', 'U', 'S', 0};
    const WCHAR *src = name && IsValidLocaleName(name) ? name : fallback;
    int n = (int)w32_wcslen(src) + 1;
    if (out && cap >= n)
        memcpy(out, src, (size_t)n * sizeof(WCHAR));
    return out && cap < n ? 0 : n;
}

static BOOL WINAPI preferred_languages(DWORD flags, ULONG *count, LPWSTR buf, ULONG *len)
{
    UNUSED(flags);
    static const WCHAR list[] = {'e', 'n', '-', 'U', 'S', 0, 0};
    *count = 1;
    if (buf && *len >= ARRAY_SIZE(list))
        memcpy(buf, list, sizeof(list));
    else if (buf) {
        *len = ARRAY_SIZE(list);
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *len = ARRAY_SIZE(list);
    return TRUE;
}

static int WINAPI FoldStringW(DWORD flags, LPCWSTR src, int n, LPWSTR out, int cap)
{
    UNUSED(flags);
    if (n < 0)
        n = (int)w32_wcslen(src) + 1;
    if (!cap)
        return n;
    if (cap < n) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(out, src, (size_t)n * sizeof(WCHAR));
    return n;
}

static int WINAPI GetGeoInfoW(DWORD id, DWORD type, LPWSTR out, int cap, DWORD lang)
{
    UNUSED(id);
    UNUSED(lang);
    static const WCHAR us[] = {'U', 'S', 0}, usa[] = {'U', 'S', 'A', 0}, nation[] = {'2', '4', '4', 0};
    const WCHAR *v = type == 4 ? us : type == 5 ? usa : nation;
    int n = (int)w32_wcslen(v) + 1;
    if (out && cap >= n)
        memcpy(out, v, (size_t)n * sizeof(WCHAR));
    return n;
}

static DWORD WINAPI GetUserGeoID(DWORD cls)
{
    UNUSED(cls);
    return 244;
}

static const struct w32_export locale_exports[] = {
    W32_FN(MultiByteToWideChar), W32_FN(WideCharToMultiByte), W32_FN(GetACP), W32_FN(GetOEMCP),
    W32_FN(IsValidCodePage), W32_FN(GetCPInfo), W32_FN(GetCPInfoExA), W32_FN(GetCPInfoExW), W32_FN(IsDBCSLeadByte),
    W32_FN(IsDBCSLeadByteEx), W32_FN(lstrlenA), W32_FN(lstrlenW), W32_AS("lstrlen", lstrlenA), W32_FN(lstrcpyA),
    W32_FN(lstrcpyW), W32_AS("lstrcpy", lstrcpyA), W32_FN(lstrcpynA), W32_FN(lstrcpynW),
    W32_AS("lstrcpyn", lstrcpynA), W32_FN(lstrcatA), W32_FN(lstrcatW), W32_AS("lstrcat", lstrcatA),
    W32_FN(lstrcmpA), W32_FN(lstrcmpW), W32_AS("lstrcmp", lstrcmpA), W32_FN(lstrcmpiA), W32_FN(lstrcmpiW),
    W32_AS("lstrcmpi", lstrcmpiA), W32_FN(CompareStringA), W32_FN(CompareStringW), W32_FN(CompareStringEx),
    W32_FN(CompareStringOrdinal), W32_FN(LCMapStringA), W32_FN(LCMapStringW), W32_FN(LCMapStringEx),
    W32_FN(GetStringTypeA), W32_FN(GetStringTypeW), W32_FN(GetStringTypeExW),
    W32_AS("GetStringTypeExA", GetStringTypeA), W32_FN(GetLocaleInfoA), W32_FN(GetLocaleInfoW),
    W32_FN(GetLocaleInfoEx), W32_FN(GetUserDefaultLCID), W32_AS("GetSystemDefaultLCID", GetUserDefaultLCID),
    W32_AS("GetThreadLocale", GetUserDefaultLCID), W32_FN(GetUserDefaultLangID),
    W32_AS("GetSystemDefaultLangID", GetUserDefaultLangID), W32_AS("GetUserDefaultUILanguage", GetUserDefaultLangID),
    W32_AS("GetSystemDefaultUILanguage", GetUserDefaultLangID), W32_FN(IsValidLocale), W32_FN(SetThreadLocale),
    W32_FN(GetUserDefaultLocaleName), W32_AS("GetSystemDefaultLocaleName", GetUserDefaultLocaleName),
    W32_FN(LocaleNameToLCID), W32_FN(LCIDToLocaleName), W32_FN(EnumSystemLocalesA), W32_FN(EnumSystemLocalesW),
    W32_FN(GetNumberFormatA), W32_FN(GetDateFormatA), W32_FN(GetDateFormatW), W32_FN(GetTimeFormatA),
    W32_FN(GetTimeFormatW),
    W32_FN(GetDateFormatEx), W32_FN(GetTimeFormatEx), W32_FN(GetNumberFormatW), W32_FN(GetNumberFormatEx),
    W32_AS("GetCurrencyFormatW", GetNumberFormatW), W32_AS("GetCurrencyFormatEx", GetNumberFormatEx),
    W32_FN(IsValidLocaleName), W32_FN(ResolveLocaleName), W32_AS("GetUserPreferredUILanguages", preferred_languages),
    W32_AS("GetThreadPreferredUILanguages", preferred_languages),
    W32_AS("GetSystemPreferredUILanguages", preferred_languages),
    W32_AS("GetProcessPreferredUILanguages", preferred_languages), W32_FN(FoldStringW), W32_FN(GetGeoInfoW),
    W32_FN(GetUserGeoID),
};

W32_EXPORTS(locale_module, "kernel32.dll", locale_exports);
