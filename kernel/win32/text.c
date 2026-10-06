#include <synth/codepage.h>
#include <synth/string.h>

#include "win32.h"

static const uint16_t cp1252_high[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

static const uint16_t cp1251_high[64] = {
    0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x0409,
    0x2039, 0x040a, 0x040c, 0x040b, 0x040f, 0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f, 0x00a0,
    0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6, 0x00a7, 0x0401, 0x00a9, 0x0404, 0x00ab,
    0x00ac, 0x00ad, 0x00ae, 0x0407, 0x00b0, 0x00b1, 0x0406, 0x0456, 0x0491, 0x00b5, 0x00b6,
    0x00b7, 0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457,
};

int w32_acp(void)
{
    return codepage_current() == 866 ? 1251 : 1252;
}

static int real_cp(int cp)
{
    if (cp == 0 || cp == 3)
        return w32_acp();
    if (cp == 1 || cp == 2)
        return codepage_current();
    return cp;
}

static uint32_t to_unicode(int cp, uint8_t ch)
{
    if (ch < 0x80)
        return ch;
    switch (cp) {
    case 1252:
        return ch < 0xa0 ? cp1252_high[ch - 0x80] : ch;
    case 1251:
        return ch < 0xc0 ? cp1251_high[ch - 0x80] : 0x0410u + (ch - 0xc0u);
    case 437:
    case 866:
        if (cp == codepage_current())
            return codepage_to_unicode(ch);
        return '?';
    case 28591:
        return ch;
    default:
        return '?';
    }
}

static int from_unicode(int cp, uint32_t u)
{
    if (u < 0x80)
        return (int)u;
    switch (cp) {
    case 1252:
        if (u >= 0xa0 && u <= 0xff)
            return (int)u;
        for (int i = 0; i < 32; i++)
            if (cp1252_high[i] == u)
                return 0x80 + i;
        return -1;
    case 1251:
        if (u >= 0x0410 && u <= 0x044f)
            return (int)(0xc0 + (u - 0x0410));
        for (int i = 0; i < 64; i++)
            if (cp1251_high[i] == u)
                return 0x80 + i;
        return -1;
    case 28591:
        return u <= 0xff ? (int)u : -1;
    default:
        return codepage_from_unicode(u);
    }
}

static size_t utf8_decode(const uint8_t *s, size_t n, uint32_t *out)
{
    uint8_t c = s[0];
    int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (extra < 0) {
        *out = 0xfffd;
        return 1;
    }
    uint32_t cp = extra ? (uint32_t)(c & (0x3f >> extra)) : c;
    for (int i = 1; i <= extra; i++) {
        if ((size_t)i >= n || (s[i] & 0xc0) != 0x80) {
            *out = 0xfffd;
            return (size_t)i;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    *out = cp;
    return (size_t)extra + 1;
}

size_t w32_cp_to_wide(const char *s, int len, WCHAR *out, size_t cap, int codepage)
{
    int cp = real_cp(codepage);
    size_t n = len < 0 ? strlen(s) + 1 : (size_t)len, count = 0;
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n;) {
        uint32_t u;
        if (cp == 65001) {
            i += utf8_decode(p + i, n - i, &u);
        } else {
            u = to_unicode(cp, p[i]);
            i++;
        }
        if (u >= 0x10000) {
            if (out && count + 2 <= cap) {
                u -= 0x10000;
                out[count] = (WCHAR)(0xd800 + (u >> 10));
                out[count + 1] = (WCHAR)(0xdc00 + (u & 0x3ff));
            }
            count += 2;
        } else {
            if (out && count < cap)
                out[count] = (WCHAR)u;
            count++;
        }
    }
    return count;
}

size_t w32_wide_to_cp(const WCHAR *w, int wlen, char *out, size_t cap, int codepage)
{
    int cp = real_cp(codepage);
    size_t n = wlen < 0 ? w32_wcslen(w) + 1 : (size_t)wlen, count = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t u = w[i];
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < n && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
            u = 0x10000 + ((u - 0xd800) << 10) + (w[i + 1] - 0xdc00u);
            i++;
        }
        if (cp == 65001) {
            char buf[4];
            size_t k = utf8_encode(u, buf);
            for (size_t j = 0; j < k; j++, count++)
                if (out && count < cap)
                    out[count] = buf[j];
        } else {
            int ch = from_unicode(cp, u);
            if (out && count < cap)
                out[count] = (char)(ch < 0 ? '?' : ch);
            count++;
        }
    }
    return count;
}

size_t w32_wcslen(const WCHAR *w)
{
    size_t n = 0;
    while (w[n])
        n++;
    return n;
}

char *w32_narrow(const WCHAR *w)
{
    if (!w)
        return NULL;
    size_t n = w32_wide_to_cp(w, -1, NULL, 0, 1);
    char *s = w32_alloc(n + 1);
    if (s) {
        w32_wide_to_cp(w, -1, s, n, 1);
        s[n] = 0;
    }
    return s;
}

WCHAR *w32_widen(const char *s)
{
    if (!s)
        return NULL;
    size_t n = w32_cp_to_wide(s, -1, NULL, 0, 1);
    WCHAR *w = w32_alloc((n + 1) * sizeof(WCHAR));
    if (w) {
        w32_cp_to_wide(s, -1, w, n, 1);
        w[n] = 0;
    }
    return w;
}
