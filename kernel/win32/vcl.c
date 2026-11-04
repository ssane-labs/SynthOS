#include <synth/fmt.h>
#include <synth/string.h>

#include "w32gui.h"

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

static uint32_t WINAPI shell_drag_cancel(void *w, void *data, void *source, DWORD effect, DWORD *out)
{
    UNUSED(w);
    UNUSED(data);
    UNUSED(source);
    UNUSED(effect);
    if (out)
        *out = 0;
    return 0x00040101u;
}

static DWORD WINAPI internet_error_dialog(HANDLE w, HANDLE request, DWORD error, DWORD flags, void **data)
{
    UNUSED(w);
    UNUSED(request);
    UNUSED(error);
    UNUSED(flags);
    UNUSED(data);
    return 1223;
}

/* window positions are applied at once, so a deferred batch is just a token */
typedef BOOL WINAPI (*set_pos_fn)(HANDLE, HANDLE, int, int, int, int, UINT);

static HANDLE WINAPI BeginDeferWindowPos(int count)
{
    UNUSED(count);
    return (HANDLE)0x5e7d0002u;
}

static HANDLE WINAPI DeferWindowPos(HANDLE batch, HANDLE w, HANDLE after, int x, int y, int cx, int cy, UINT flags)
{
    set_pos_fn set = (set_pos_fn)w32_builtin_lookup("user32.dll", "SetWindowPos");
    if (!batch || !set || !set(w, after, x, y, cx, cy, flags))
        return NULL;
    return batch;
}

static BOOL WINAPI EndDeferWindowPos(HANDLE batch)
{
    return batch != NULL;
}

static BOOL WINAPI SubtractRect(RECT *out, const RECT *a, const RECT *b)
{
    RECT r = *a;
    bool spans_x = b->left <= r.left && b->right >= r.right, spans_y = b->top <= r.top && b->bottom >= r.bottom;
    if (spans_x && b->top <= r.top && b->bottom > r.top)
        r.top = MIN(b->bottom, r.bottom);
    else if (spans_x && b->bottom >= r.bottom && b->top < r.bottom)
        r.bottom = MAX(b->top, r.top);
    else if (spans_y && b->left <= r.left && b->right > r.left)
        r.left = MIN(b->right, r.right);
    else if (spans_y && b->right >= r.right && b->left < r.right)
        r.right = MAX(b->left, r.left);
    *out = r;
    if (r.left >= r.right || r.top >= r.bottom) {
        *out = (RECT){0, 0, 0, 0};
        return FALSE;
    }
    return TRUE;
}

static W32_STATE POINT caret_pos;

static BOOL WINAPI SetCaretPos(int x, int y)
{
    caret_pos = (POINT){x, y};
    return TRUE;
}

static BOOL WINAPI GetCaretPos(POINT *p)
{
    if (!p)
        return FALSE;
    *p = caret_pos;
    return TRUE;
}

static HANDLE WINAPI CopyIcon(HANDLE icon)
{
    return icon;
}

static const struct w32_export vcl_user32_more[] = {
    W32_FN(BeginDeferWindowPos), W32_FN(DeferWindowPos), W32_FN(EndDeferWindowPos), W32_FN(SubtractRect),
    W32_FN(SetCaretPos), W32_FN(GetCaretPos), W32_AS("HideCaret", w32_one1), W32_FN(CopyIcon),
    W32_AS("AttachThreadInput", w32_one3), W32_AS("DrawCaption", w32_one4), W32_AS("EndMenu", w32_one0),
    W32_AS("LockWindowUpdate", w32_one1), W32_AS("PhysicalToLogicalPoint", w32_one2),
    W32_AS("LogicalToPhysicalPoint", w32_one2),
};

W32_EXPORTS(vcl_user32_more_module, "user32.dll", vcl_user32_more);

/* change notifications: a handle that is never signaled (nothing watches the file system for programs yet) */
typedef HANDLE WINAPI (*event_fn)(void *, BOOL, BOOL, LPCWSTR);

static HANDLE WINAPI FindFirstChangeNotificationW(LPCWSTR path, BOOL subtree, DWORD filter)
{
    UNUSED(subtree);
    UNUSED(filter);
    event_fn create = (event_fn)w32_builtin_lookup("kernel32.dll", "CreateEventW");
    HANDLE h = path && create ? create(NULL, FALSE, FALSE, NULL) : NULL;
    return h ? h : (HANDLE)(intptr_t)-1;
}

static HANDLE WINAPI FindFirstChangeNotificationA(LPCSTR path, BOOL subtree, DWORD filter)
{
    return FindFirstChangeNotificationW((LPCWSTR)path, subtree, filter);
}

static BOOL WINAPI FindCloseChangeNotification(HANDLE h)
{
    typedef BOOL WINAPI (*close_fn)(HANDLE);
    close_fn close = (close_fn)w32_builtin_lookup("kernel32.dll", "CloseHandle");
    return close ? close(h) : FALSE;
}

static const struct w32_export vcl_kernel32_more[] = {
    W32_FN(FindFirstChangeNotificationW), W32_FN(FindFirstChangeNotificationA),
    W32_AS("FindNextChangeNotification", w32_one1), W32_FN(FindCloseChangeNotification),
};

W32_EXPORTS(vcl_kernel32_more_module, "kernel32.dll", vcl_kernel32_more);

static UINT WINAPI ExtractIconExW(LPCWSTR file, int index, HANDLE *large, HANDLE *small, UINT count)
{
    if (!file)
        return 0;
    if (index == -1 && !large && !small)
        return 1;
    if (large && count)
        *large = ExtractIconW(NULL, file, 0);
    if (small && count)
        *small = ExtractIconW(NULL, file, 0);
    return (large || small) && count ? 1 : 0;
}

static UINT WINAPI ExtractIconExA(LPCSTR file, int index, HANDLE *large, HANDLE *small, UINT count)
{
    return ExtractIconExW((LPCWSTR)file, index, large, small, count);
}

static const struct w32_export vcl_shell32_more[] = {
    W32_FN(ExtractIconExW), W32_FN(ExtractIconExA), W32_AS("DragAcceptFiles", w32_zero2),
    W32_AS("DragFinish", w32_zero1), W32_AS("DragQueryFileA", w32_zero4), W32_AS("DragQueryPoint", w32_zero2),
    W32_AS("SHDoDragDrop", shell_drag_cancel),
};

W32_EXPORTS(vcl_shell32_more_module, "shell32.dll", vcl_shell32_more);

/* "1 min 5 sec": the units Explorer uses, to the given number of significant digits */
static int WINAPI StrFromTimeIntervalW(LPWSTR buf, UINT size, DWORD ms, int digits)
{
    char text[48];
    DWORD s = ms / 1000, h = s / 3600, m = s / 60 % 60;
    s %= 60;
    int n = 0;
    if (h)
        n = ksnprintf(text, sizeof(text), " %u hr", (unsigned)h);
    if (m || (h && s && digits > 2))
        n += ksnprintf(text + n, sizeof(text) - (size_t)n, " %u min", (unsigned)m);
    if (!h && (s || !m))
        n += ksnprintf(text + n, sizeof(text) - (size_t)n, " %u sec", (unsigned)s);
    if (!buf || !size)
        return n;
    UINT i = 0;
    for (; i < (UINT)n && i + 1 < size; i++)
        buf[i] = (WCHAR)(uint8_t)text[i];
    buf[i] = 0;
    return (int)i;
}

static const struct w32_export vcl_shlwapi[] = {W32_FN(StrFromTimeIntervalW)};

W32_EXPORTS(vcl_shlwapi_module, "shlwapi.dll", vcl_shlwapi);

static uint32_t WINAPI CreateDataAdviseHolder(void **out)
{
    if (out)
        *out = NULL;
    return 0x80004001u;
}

static const struct w32_export vcl_ole32_more[] = {W32_FN(CreateDataAdviseHolder)};

W32_EXPORTS(vcl_ole32_more_module, "ole32.dll", vcl_ole32_more);

static BOOL WINAPI InternetGetLastResponseInfoW(DWORD *error, LPWSTR buf, DWORD *len)
{
    if (error)
        *error = 0;
    if (buf && len && *len)
        buf[0] = 0;
    if (len)
        *len = 0;
    return TRUE;
}

static const struct w32_export vcl_wininet[] = {
    W32_FN(InternetGetLastResponseInfoW), W32_AS("InternetGetLastResponseInfoA", InternetGetLastResponseInfoW),
    W32_AS("InternetAttemptConnect", w32_zero1), W32_AS("InternetErrorDlg", internet_error_dialog),
};

W32_EXPORTS(vcl_wininet_module, "wininet.dll", vcl_wininet);

/* font and print dialogs: the user cancelled (CommDlgExtendedError stays 0) */
static const struct w32_export vcl_comdlg32[] = {
    W32_AS("ChooseFontW", w32_zero1), W32_AS("ChooseFontA", w32_zero1), W32_AS("PrintDlgW", w32_zero1),
    W32_AS("PrintDlgA", w32_zero1),
};

W32_EXPORTS(vcl_comdlg32_module, "comdlg32.dll", vcl_comdlg32);

static BOOL WINAPI GetFileSecurityW(LPCWSTR name, DWORD info, void *sd, DWORD size, DWORD *need)
{
    UNUSED(name);
    UNUSED(info);
    UNUSED(sd);
    UNUSED(size);
    if (need)
        *need = 0;
    w32_set_error(50);
    return FALSE;
}

static const struct w32_export vcl_advapi32[] = {W32_FN(GetFileSecurityW),
                                                 W32_AS("GetFileSecurityA", GetFileSecurityW)};

W32_EXPORTS(vcl_advapi32_module, "advapi32.dll", vcl_advapi32);

static const struct w32_export vcl_imm32[] = {W32_AS("ImmAssociateContext", w32_zero2),
                                              W32_AS("ImmGetVirtualKey", w32_zero1)};

W32_EXPORTS(vcl_imm32_module, "imm32.dll", vcl_imm32);
