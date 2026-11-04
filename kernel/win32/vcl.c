#include <synth/string.h>

#include "win32.h"

/* Odds and ends Delphi/VCL programs and their installers import */

static DWORD WINAPI CharUpperBuffW(LPWSTR s, DWORD n)
{
    for (DWORD i = 0; s && i < n; i++)
        s[i] = w32_towupper(s[i]);
    return s ? n : 0;
}

static DWORD WINAPI CharLowerBuffW(LPWSTR s, DWORD n)
{
    for (DWORD i = 0; s && i < n; i++)
        s[i] = w32_towlower(s[i]);
    return s ? n : 0;
}

/* code page 1 is the OEM one, 0 the ANSI one, as in w32_cp_to_wide */
static BOOL WINAPI CharToOemBuffA(LPCSTR in, LPSTR out, DWORD n)
{
    WCHAR *w = in && out ? w32_alloc((n + 1) * sizeof(WCHAR)) : NULL;
    if (!w)
        return FALSE;
    w32_cp_to_wide(in, (int)n, w, n, 0);
    w32_wide_to_cp(w, (int)n, out, n, 1);
    w32_free(w);
    return TRUE;
}

static BOOL WINAPI OemToCharBuffA(LPCSTR in, LPSTR out, DWORD n)
{
    WCHAR *w = in && out ? w32_alloc((n + 1) * sizeof(WCHAR)) : NULL;
    if (!w)
        return FALSE;
    w32_cp_to_wide(in, (int)n, w, n, 1);
    w32_wide_to_cp(w, (int)n, out, n, 0);
    w32_free(w);
    return TRUE;
}

static BOOL WINAPI CharToOemBuffW(LPCWSTR in, LPSTR out, DWORD n)
{
    if (!in || !out)
        return FALSE;
    w32_wide_to_cp(in, (int)n, out, n, 1);
    return TRUE;
}

static BOOL WINAPI OemToCharBuffW(LPCSTR in, LPWSTR out, DWORD n)
{
    if (!in || !out)
        return FALSE;
    w32_cp_to_wide(in, (int)n, out, n, 1);
    return TRUE;
}

static HANDLE WINAPI CreateIcon(HANDLE inst, int w, int h, BYTE planes, BYTE bpp, const void *and_bits,
                                const void *xor_bits)
{
    UNUSED(inst);
    UNUSED(planes);
    UNUSED(bpp);
    UNUSED(and_bits);
    UNUSED(xor_bits);
    if (w <= 0 || h <= 0)
        return NULL;
    HANDLE(WINAPI * make)(void *) = (HANDLE(WINAPI *)(void *))w32_builtin_lookup("user32.dll", "CreateIconIndirect");
    return make ? make(NULL) : NULL;
}

static BOOL WINAPI DrawIcon(HANDLE dc, int x, int y, HANDLE icon)
{
    BOOL(WINAPI * draw)(HANDLE, int, int, HANDLE, int, int, UINT, HANDLE, UINT) =
        (BOOL(WINAPI *)(HANDLE, int, int, HANDLE, int, int, UINT, HANDLE, UINT))w32_builtin_lookup("user32.dll",
                                                                                                    "DrawIconEx");
    return draw ? draw(dc, x, y, icon, 0, 0, 0, NULL, 3) : FALSE;
}

static const struct w32_export vcl_user32[] = {
    W32_FN(CharUpperBuffW), W32_FN(CharLowerBuffW), W32_FN(CharToOemBuffA), W32_FN(OemToCharBuffA),
    W32_FN(CharToOemBuffW), W32_FN(OemToCharBuffW), W32_FN(CreateIcon), W32_FN(DrawIcon),
};

W32_EXPORTS(vcl_user32_module, "user32.dll", vcl_user32);

/* the Gregorian calendar's strings, for programs that list them */
static const char *calendar_string(DWORD type)
{
    switch (type & 0xffff) {
    case 2: return "Gregorian Calendar";
    case 4: return "A.D.";
    case 5: return "M/d/yyyy";
    case 6: return "dddd, MMMM d, yyyy";
    case 0x2f: return "MMMM yyyy";
    default: return NULL;
    }
}

static BOOL enum_calendar(void *fn, DWORD calendar, DWORD type, bool wide)
{
    if (!fn) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const char *s = calendar == 1 || calendar == 0xffffffffu ? calendar_string(type) : NULL;
    if (!s)
        return TRUE;
    size_t n = strlen(s);
    void *buf = w32_zalloc((n + 1) * (wide ? 2 : 1));
    if (!buf)
        return FALSE;
    for (size_t i = 0; i < n; i++) {
        if (wide)
            ((WCHAR *)buf)[i] = (WCHAR)(uint8_t)s[i];
        else
            ((char *)buf)[i] = s[i];
    }
    W32_CALL(BOOL(WINAPI *)(void *), fn, buf);
    w32_free(buf);
    return TRUE;
}

static BOOL WINAPI EnumCalendarInfoW(void *fn, DWORD lcid, DWORD calendar, DWORD type)
{
    UNUSED(lcid);
    return enum_calendar(fn, calendar, type, true);
}

static BOOL WINAPI EnumCalendarInfoA(void *fn, DWORD lcid, DWORD calendar, DWORD type)
{
    UNUSED(lcid);
    return enum_calendar(fn, calendar, type, false);
}

static const struct w32_export vcl_kernel32[] = {
    W32_FN(EnumCalendarInfoW),
    W32_FN(EnumCalendarInfoA),
};

W32_EXPORTS(vcl_kernel32_module, "kernel32.dll", vcl_kernel32);

/* mpr: SynthOS has no network drives; the answers say so the way Windows does */
#define ERROR_NOT_CONNECTED 2250u
#define ERROR_NO_NETWORK 1222u
#define ERROR_NO_MORE_ITEMS 259u

static DWORD WINAPI WNetGetConnectionW(LPCWSTR local, LPWSTR remote, DWORD *size)
{
    UNUSED(remote);
    UNUSED(size);
    return local ? ERROR_NOT_CONNECTED : ERROR_INVALID_PARAMETER;
}

static DWORD WINAPI WNetGetUniversalNameW(LPCWSTR local, DWORD level, void *out, DWORD *size)
{
    UNUSED(level);
    UNUSED(out);
    UNUSED(size);
    return local ? ERROR_NOT_CONNECTED : ERROR_INVALID_PARAMETER;
}

static DWORD WINAPI WNetOpenEnumW(DWORD scope, DWORD type, DWORD usage, void *resource, HANDLE *out)
{
    UNUSED(scope);
    UNUSED(type);
    UNUSED(usage);
    UNUSED(resource);
    if (out)
        *out = NULL;
    return ERROR_NO_NETWORK;
}

static DWORD WINAPI WNetEnumResourceW(HANDLE h, DWORD *count, void *buf, DWORD *size)
{
    UNUSED(h);
    UNUSED(buf);
    UNUSED(size);
    if (count)
        *count = 0;
    return ERROR_NO_MORE_ITEMS;
}

static DWORD WINAPI WNetCloseEnum(HANDLE h)
{
    UNUSED(h);
    return 0;
}

static DWORD WINAPI WNetGetUserW(LPCWSTR name, LPWSTR out, DWORD *size)
{
    UNUSED(name);
    static const char user[] = "root";
    if (!size)
        return ERROR_INVALID_PARAMETER;
    if (!out || *size < sizeof(user)) {
        *size = sizeof(user);
        return 234;
    }
    for (size_t i = 0; i < sizeof(user); i++)
        out[i] = (WCHAR)user[i];
    return 0;
}

static const struct w32_export vcl_mpr[] = {
    W32_FN(WNetGetConnectionW), W32_AS("WNetGetConnectionA", WNetGetConnectionW), W32_FN(WNetGetUniversalNameW),
    W32_AS("WNetGetUniversalNameA", WNetGetUniversalNameW), W32_FN(WNetOpenEnumW), W32_AS("WNetOpenEnumA", WNetOpenEnumW),
    W32_FN(WNetEnumResourceW), W32_AS("WNetEnumResourceA", WNetEnumResourceW), W32_FN(WNetCloseEnum),
    W32_FN(WNetGetUserW),
};

W32_EXPORTS(vcl_mpr_module, "mpr.dll", vcl_mpr);

static DWORD WINAPI CoDisconnectObject(void *obj, DWORD reserved)
{
    UNUSED(reserved);
    return obj ? 0 : 0x80070057u;
}

static void WINAPI CoFreeUnusedLibraries(void)
{
}

static void WINAPI CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved)
{
    UNUSED(delay);
    UNUSED(reserved);
}

static BOOL WINAPI IsEqualGUID(const void *a, const void *b)
{
    return a && b && !memcmp(a, b, 16);
}

static const struct w32_export vcl_ole32[] = {
    W32_FN(CoDisconnectObject),
    W32_FN(CoFreeUnusedLibraries),
    W32_FN(CoFreeUnusedLibrariesEx),
    W32_FN(IsEqualGUID),
};

W32_EXPORTS(vcl_ole32_module, "ole32.dll", vcl_ole32);

/* icons are drawn from the built-in set, so a file's icon is the generic application one */
static HANDLE WINAPI ExtractIconW(HANDLE inst, LPCWSTR file, UINT index)
{
    UNUSED(inst);
    if (!file)
        return NULL;
    if (index == 0xffffffffu)
        return (HANDLE)1;
    return (HANDLE)(uintptr_t)(0x30000u | 32512u);
}

static HANDLE WINAPI ExtractIconA(HANDLE inst, LPCSTR file, UINT index)
{
    return ExtractIconW(inst, (LPCWSTR)file, index);
}

static const struct w32_export vcl_shell32[] = {
    W32_FN(ExtractIconW),
    W32_FN(ExtractIconA),
};

W32_EXPORTS(vcl_shell32_module, "shell32.dll", vcl_shell32);
