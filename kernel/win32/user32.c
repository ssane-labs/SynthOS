#include <synth/config.h>
#include <synth/gui.h>
#include <synth/console.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/string.h>
#include <synth/theme.h>

#include "crt/crt.h"
#include "win32.h"

#define MB_TYPEMASK 0xf
#define MB_OKCANCEL 1
#define MB_ABORTRETRYIGNORE 2
#define MB_YESNOCANCEL 3
#define MB_YESNO 4
#define MB_RETRYCANCEL 5
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

struct button {
    const char *label;
    int id;
    char key;
};

static int buttons_for(UINT type, struct button *out)
{
    static const struct button sets[][3] = {
        {{"OK", IDOK, 'o'}},
        {{"OK", IDOK, 'o'}, {"Cancel", IDCANCEL, 'c'}},
        {{"Abort", IDABORT, 'a'}, {"Retry", IDRETRY, 'r'}, {"Ignore", IDIGNORE, 'i'}},
        {{"Yes", IDYES, 'y'}, {"No", IDNO, 'n'}, {"Cancel", IDCANCEL, 'c'}},
        {{"Yes", IDYES, 'y'}, {"No", IDNO, 'n'}},
        {{"Retry", IDRETRY, 'r'}, {"Cancel", IDCANCEL, 'c'}},
    };
    static const int counts[] = {1, 2, 3, 3, 2, 2};
    UINT t = type & MB_TYPEMASK;
    if (t >= ARRAY_SIZE(counts))
        t = 0;
    memcpy(out, sets[t], sizeof(sets[t]));
    return counts[t];
}

static int message_box_text(const char *text, const char *caption, UINT type)
{
#if CONFIG_GUI
    if (gui_active()) {
        int w32_gui_message_box(const char *text, const char *caption, UINT type);
        int r = w32_gui_message_box(text, caption, type);
        if (r)
            return r;
    }
#endif
    struct button buttons[3];
    int count = buttons_for(type, buttons);
    if (!con_is_grid() || !stream_is_console(w32->ctx->out)) {
        char line[512];
        ksnprintf(line, sizeof(line), "[%s] %s\n", caption ? caption : "Message", text ? text : "");
        stream_puts(w32->ctx->err, line);
        return buttons[0].id;
    }
    int cols = con_cols(), rows = con_rows();
    int width = (int)strlen(caption ? caption : "") + 6;
    int lines = 1, cur = 0;
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '\n') {
            lines++;
            cur = 0;
        } else if (++cur + 6 > width) {
            width = cur + 6;
        }
    }
    width = MIN(MAX(width, 30), cols - 4);
    int height = lines + 6;
    int x0 = (cols - width) / 2, y0 = MAX((rows - height) / 2, 0);
    uint8_t frame = theme_attr(ROLE_ACCENT), body = theme_attr(ROLE_TEXT), title = theme_attr(ROLE_HIGHLIGHT);
    struct cell *saved = w32_alloc(sizeof(struct cell) * (size_t)width * (size_t)height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (saved)
                saved[y * width + x] = con_get_cell(x0 + x, y0 + y);
    con_fill(x0, y0, width, height, ' ', body);
    for (int x = 1; x < width - 1; x++) {
        con_put_cell(x0 + x, y0, 0xcd, frame);
        con_put_cell(x0 + x, y0 + height - 1, 0xcd, frame);
    }
    for (int y = 1; y < height - 1; y++) {
        con_put_cell(x0, y0 + y, 0xba, frame);
        con_put_cell(x0 + width - 1, y0 + y, 0xba, frame);
    }
    con_put_cell(x0, y0, 0xc9, frame);
    con_put_cell(x0 + width - 1, y0, 0xbb, frame);
    con_put_cell(x0, y0 + height - 1, 0xc8, frame);
    con_put_cell(x0 + width - 1, y0 + height - 1, 0xbc, frame);
    char head[96];
    ksnprintf(head, sizeof(head), " %s ", caption ? caption : "Message");
    con_put_text(x0 + 2, y0, head, title);
    int ty = y0 + 2, tx = x0 + 3;
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '\n') {
            ty++;
            tx = x0 + 3;
            continue;
        }
        if (tx < x0 + width - 3)
            con_put_cell(tx++, ty, (uint8_t)*p, body);
    }
    int selected = 0;
    for (;;) {
        int bx = x0 + width / 2;
        int total = 0;
        for (int i = 0; i < count; i++)
            total += (int)strlen(buttons[i].label) + 6;
        bx -= total / 2;
        for (int i = 0; i < count; i++) {
            char label[24];
            ksnprintf(label, sizeof(label), i == selected ? "[ %s ]" : "  %s  ", buttons[i].label);
            con_put_text(bx, y0 + height - 2, label, i == selected ? theme_attr(ROLE_SELECTION) : body);
            bx += (int)strlen(label) + 2;
        }
        con_show_cursor(false);
        con_flush();
        int key = w32_console_getkey(true);
        int base = KEY_BASE(key);
        int result = 0;
        if (base == KEY_LEFT || (base == KEY_TAB && (key & KEY_MOD_SHIFT)))
            selected = (selected + count - 1) % count;
        else if (base == KEY_RIGHT || base == KEY_TAB)
            selected = (selected + 1) % count;
        else if (base == KEY_ENTER || base == '\r' || base == ' ')
            result = buttons[selected].id;
        else if (base == KEY_ESC)
            result = count > 1 ? buttons[count - 1].id : buttons[0].id;
        else
            for (int i = 0; i < count; i++)
                if (to_lower(base) == buttons[i].key)
                    result = buttons[i].id;
        if (result) {
            for (int y = 0; y < height && saved; y++)
                for (int x = 0; x < width; x++)
                    con_put_cell(x0 + x, y0 + y, saved[y * width + x].ch, saved[y * width + x].attr);
            w32_free(saved);
            con_show_cursor(true);
            con_flush();
            return result;
        }
    }
}

static int WINAPI MessageBoxA(HANDLE wnd, LPCSTR text, LPCSTR caption, UINT type)
{
    UNUSED(wnd);
    return message_box_text(text, caption, type);
}

static int WINAPI MessageBoxW(HANDLE wnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
    UNUSED(wnd);
    char *t = w32_narrow(text), *c = w32_narrow(caption);
    int r = message_box_text(t, c, type);
    w32_free(t);
    w32_free(c);
    return r;
}

static int WINAPI MessageBoxExA(HANDLE wnd, LPCSTR text, LPCSTR caption, UINT type, WORD lang)
{
    UNUSED(lang);
    return MessageBoxA(wnd, text, caption, type);
}

static int WINAPI MessageBoxExW(HANDLE wnd, LPCWSTR text, LPCWSTR caption, UINT type, WORD lang)
{
    UNUSED(lang);
    return MessageBoxW(wnd, text, caption, type);
}

/* MSGBOXPARAMS: cbSize, hwndOwner, hInstance, lpszText, lpszCaption, dwStyle, ... */
static int WINAPI MessageBoxIndirectA(const uint32_t *p)
{
    return MessageBoxA((HANDLE)p[1], (LPCSTR)p[3], (LPCSTR)p[4], p[5]);
}

static int WINAPI MessageBoxIndirectW(const uint32_t *p)
{
    return MessageBoxW((HANDLE)p[1], (LPCWSTR)p[3], (LPCWSTR)p[4], p[5]);
}

static BOOL WINAPI ExitWindowsEx(UINT flags, DWORD reason)
{
    UNUSED(flags);
    UNUSED(reason);
    w32_set_error(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI MessageBeep(UINT type)
{
    UNUSED(type);
    hal_beep(880, 60);
    return TRUE;
}

struct strbuf {
    char *buf;
    size_t len;
};

static void buf_sink(void *ctx, const char *s, size_t n)
{
    struct strbuf *b = ctx;
    for (size_t i = 0; i < n && b->len < 1023; i++)
        b->buf[b->len++] = s[i];
}

static int WINAPI wvsprintfA(LPSTR out, LPCSTR fmt, w32_va_list ap)
{
    struct strbuf b = {out, 0};
    crt_vformat(buf_sink, &b, fmt, ap);
    out[b.len] = 0;
    return (int)b.len;
}

static int WINAPIV wsprintfA(LPSTR out, LPCSTR fmt, ...)
{
    w32_va_list ap;
    w32_va_start(ap, fmt);
    int n = wvsprintfA(out, fmt, ap);
    w32_va_end(ap);
    return n;
}

struct wstrbuf {
    WCHAR *buf;
    size_t len;
};

static void wbuf_sink(void *ctx, const WCHAR *s, size_t n)
{
    struct wstrbuf *b = ctx;
    for (size_t i = 0; i < n && b->len < 1023; i++)
        b->buf[b->len++] = s[i];
}

static int WINAPI wvsprintfW(LPWSTR out, LPCWSTR fmt, w32_va_list ap)
{
    struct wstrbuf b = {out, 0};
    crt_vwformat(wbuf_sink, &b, fmt, ap);
    out[b.len] = 0;
    return (int)b.len;
}

static int WINAPIV wsprintfW(LPWSTR out, LPCWSTR fmt, ...)
{
    w32_va_list ap;
    w32_va_start(ap, fmt);
    int n = wvsprintfW(out, fmt, ap);
    w32_va_end(ap);
    return n;
}

static LPSTR WINAPI CharUpperA(LPSTR s)
{
    if ((uintptr_t)s < 0x10000)
        return (LPSTR)(uintptr_t)to_upper((int)(uintptr_t)s);
    str_upper(s);
    return s;
}

static LPSTR WINAPI CharLowerA(LPSTR s)
{
    if ((uintptr_t)s < 0x10000)
        return (LPSTR)(uintptr_t)to_lower((int)(uintptr_t)s);
    str_lower(s);
    return s;
}

static LPWSTR WINAPI CharUpperW(LPWSTR s)
{
    if ((uintptr_t)s < 0x10000)
        return (LPWSTR)(uintptr_t)w32_towupper((WCHAR)(uintptr_t)s);
    for (LPWSTR p = s; *p; p++)
        *p = w32_towupper(*p);
    return s;
}

static LPWSTR WINAPI CharLowerW(LPWSTR s)
{
    if ((uintptr_t)s < 0x10000)
        return (LPWSTR)(uintptr_t)w32_towlower((WCHAR)(uintptr_t)s);
    for (LPWSTR p = s; *p; p++)
        *p = w32_towlower(*p);
    return s;
}

static DWORD WINAPI CharUpperBuffA(LPSTR s, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        s[i] = (char)to_upper((uint8_t)s[i]);
    return n;
}

static DWORD WINAPI CharLowerBuffA(LPSTR s, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        s[i] = (char)to_lower((uint8_t)s[i]);
    return n;
}

static LPSTR WINAPI CharNextA(LPCSTR s)
{
    return (LPSTR)(*s ? s + 1 : s);
}

static LPSTR WINAPI CharPrevA(LPCSTR start, LPCSTR s)
{
    return (LPSTR)(s > start ? s - 1 : start);
}

static LPSTR WINAPI CharPrevExA(WORD cp, LPCSTR start, LPCSTR s, DWORD flags)
{
    UNUSED(cp);
    UNUSED(flags);
    return CharPrevA(start, s);
}

static LPSTR WINAPI CharNextExA(WORD cp, LPCSTR s, DWORD flags)
{
    UNUSED(cp);
    UNUSED(flags);
    return CharNextA(s);
}

static LPWSTR WINAPI CharNextW(LPCWSTR s)
{
    return (LPWSTR)(*s ? s + 1 : s);
}

static LPWSTR WINAPI CharPrevW(LPCWSTR start, LPCWSTR s)
{
    return (LPWSTR)(s > start ? s - 1 : start);
}

static BOOL WINAPI CharToOemA(LPCSTR in, LPSTR out)
{
    WCHAR *w = w32_widen(in);
    w32_wide_to_cp(w, -1, out, strlen(in) + 1, 1);
    w32_free(w);
    return TRUE;
}

static BOOL WINAPI OemToCharA(LPCSTR in, LPSTR out)
{
    size_t n = strlen(in) + 1;
    WCHAR *w = w32_alloc(n * sizeof(WCHAR));
    w32_cp_to_wide(in, (int)n, w, n, 1);
    w32_wide_to_cp(w, (int)n, out, n, 0);
    w32_free(w);
    return TRUE;
}

static BOOL WINAPI IsCharAlphaA(char c)
{
    return is_alpha((uint8_t)c);
}

static BOOL WINAPI IsCharAlphaNumericA(char c)
{
    return is_alnum((uint8_t)c);
}





static DWORD WINAPI GetKeyboardLayout(DWORD thread)
{
    UNUSED(thread);
    return 0x04090409;
}

static const struct w32_export user32_exports[] = {
    W32_FN(MessageBoxA), W32_FN(MessageBoxW), W32_FN(MessageBoxExA), W32_FN(MessageBoxExW),
    W32_FN(MessageBoxIndirectA), W32_FN(MessageBoxIndirectW), W32_FN(ExitWindowsEx), W32_FN(MessageBeep),
    W32_FN(wsprintfA), W32_FN(wsprintfW),
    W32_FN(wvsprintfA), W32_FN(wvsprintfW), W32_FN(CharUpperA), W32_FN(CharLowerA), W32_FN(CharUpperW),
    W32_FN(CharLowerW), W32_FN(CharUpperBuffA), W32_FN(CharLowerBuffA), W32_FN(CharNextA), W32_FN(CharPrevA), W32_FN(CharPrevExA), W32_FN(CharNextExA),
    W32_FN(CharNextW), W32_FN(CharPrevW), W32_FN(CharToOemA), W32_FN(OemToCharA), W32_AS("AnsiToOem", CharToOemA),
    W32_AS("OemToAnsi", OemToCharA), W32_FN(IsCharAlphaA), W32_FN(IsCharAlphaNumericA),
    W32_AS("GetKeyboardLayout", GetKeyboardLayout),
};

W32_EXPORTS(user32_module, "user32.dll", user32_exports);
