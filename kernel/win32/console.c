#include <synth/codepage.h>
#include <synth/console.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/string.h>

#include "win32.h"

#define STD_INPUT ((DWORD)-10)
#define STD_OUTPUT ((DWORD)-11)
#define STD_ERROR ((DWORD)-12)
#define ENABLE_LINE_INPUT 0x2
#define ENABLE_ECHO_INPUT 0x4
#define KEY_EVENT 1

struct coord {
    int16_t x, y;
};

static char title[128] = "SynthOS";

static struct w32_object *console_object(HANDLE h, bool output)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return NULL;
    if (output && (o->type == WH_STDOUT || o->type == WH_STDERR))
        return stream_is_console(o->type == WH_STDOUT ? w32->ctx->out : w32->ctx->err) ? o : NULL;
    if (!output && o->type == WH_STDIN)
        return stream_is_console(w32->ctx->in) ? o : NULL;
    return NULL;
}

static bool require(HANDLE h, bool output)
{
    if (console_object(h, output))
        return true;
    w32_set_error(ERROR_INVALID_HANDLE);
    return false;
}

static int output_kind(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    return o && o->type == WH_STDERR ? WH_STDERR : WH_STDOUT;
}

static HANDLE WINAPI GetStdHandle(DWORD which)
{
    switch (which) {
    case STD_INPUT:
        return w32_std_handle(0);
    case STD_OUTPUT:
        return w32_std_handle(1);
    case STD_ERROR:
        return w32_std_handle(2);
    default:
        w32_set_error(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
}

static BOOL WINAPI SetStdHandle(DWORD which, HANDLE h)
{
    UNUSED(which);
    UNUSED(h);
    return TRUE;
}

static BOOL WINAPI GetConsoleMode(HANDLE h, DWORD *mode)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || (o->type != WH_STDIN && !console_object(h, true)) ||
        (o->type == WH_STDIN && !console_object(h, false))) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *mode = o->type == WH_STDIN ? w32->input_mode : w32->output_mode;
    return TRUE;
}

static BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return FALSE;
    if (o->type == WH_STDIN)
        w32->input_mode = mode;
    else
        w32->output_mode = mode;
    return TRUE;
}

static BOOL WINAPI WriteConsoleA(HANDLE h, const char *buf, DWORD n, DWORD *written, void *reserved)
{
    UNUSED(reserved);
    w32_console_write(output_kind(h), buf, n);
    if (written)
        *written = n;
    return TRUE;
}

static BOOL WINAPI WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *reserved)
{
    UNUSED(reserved);
    char out[256];
    for (DWORD done = 0; done < n;) {
        DWORD chunk = MIN(n - done, (DWORD)sizeof(out));
        size_t len = w32_wide_to_cp(buf + done, (int)chunk, out, sizeof(out), 1);
        w32_console_write_raw(output_kind(h), out, MIN(len, sizeof(out)));
        done += chunk;
    }
    if (written)
        *written = n;
    return TRUE;
}

static DWORD read_line(char *buf, DWORD n)
{
    long got = w32_console_read(buf, n);
    return got < 0 ? 0 : (DWORD)got;
}

static DWORD read_raw(char *buf, DWORD n)
{
    DWORD got = 0;
    while (got < n) {
        int key = w32_console_getkey(got == 0);
        if (key == KEY_NONE)
            break;
        WORD vk, scan;
        WCHAR ch;
        DWORD state;
        if (!w32_key_info(key, &vk, &scan, &ch, &state) || !ch)
            continue;
        buf[got++] = (char)(ch < 0x80 ? ch : '?');
        if (w32->input_mode & ENABLE_ECHO_INPUT)
            w32_console_write_raw(WH_STDOUT, buf + got - 1, 1);
    }
    return got;
}

static BOOL WINAPI ReadConsoleA(HANDLE h, char *buf, DWORD n, DWORD *read, void *control)
{
    UNUSED(control);
    UNUSED(h);
    DWORD got = (w32->input_mode & ENABLE_LINE_INPUT) || !stream_is_console(w32->ctx->in) ? read_line(buf, n)
                                                                                         : read_raw(buf, n);
    if (read)
        *read = got;
    return TRUE;
}

static BOOL WINAPI ReadConsoleW(HANDLE h, WCHAR *buf, DWORD n, DWORD *read, void *control)
{
    char tmp[512];
    DWORD got = 0;
    ReadConsoleA(h, tmp, MIN(n, (DWORD)sizeof(tmp)), &got, control);
    size_t wn = w32_cp_to_wide(tmp, (int)got, buf, n, 1);
    if (read)
        *read = (DWORD)MIN(wn, (size_t)n);
    return TRUE;
}

static BOOL WINAPI SetConsoleTextAttribute(HANDLE h, WORD attr)
{
    UNUSED(h);
    w32->console_attr = (uint8_t)attr;
    return TRUE;
}

static BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, uint8_t *info)
{
    if (!require(h, true))
        return FALSE;
    int x, y;
    con_cursor(&x, &y);
    int16_t cols = (int16_t)con_cols(), rows = (int16_t)con_rows();
    int16_t v[11] = {cols, rows, (int16_t)x, (int16_t)y, w32->console_attr, 0, 0, (int16_t)(cols - 1),
                     (int16_t)(rows - 1), cols, rows};
    memcpy(info, v, sizeof(v));
    return TRUE;
}

static BOOL WINAPI GetConsoleScreenBufferInfoEx(HANDLE h, uint8_t *info)
{
    if (!require(h, true))
        return FALSE;
    GetConsoleScreenBufferInfo(h, info + 4);
    return TRUE;
}

static struct coord unpack(DWORD packed)
{
    return (struct coord){(int16_t)(packed & 0xffff), (int16_t)(packed >> 16)};
}

static BOOL WINAPI SetConsoleCursorPosition(HANDLE h, DWORD packed)
{
    if (!require(h, true))
        return FALSE;
    struct coord c = unpack(packed);
    con_goto(c.x, c.y);
    con_flush();
    return TRUE;
}

static BOOL WINAPI GetConsoleCursorInfo(HANDLE h, DWORD *info)
{
    UNUSED(h);
    info[0] = 25;
    info[1] = con_cursor_visible();
    return TRUE;
}

static BOOL WINAPI SetConsoleCursorInfo(HANDLE h, const DWORD *info)
{
    UNUSED(h);
    con_show_cursor(info[1] != 0);
    con_flush();
    return TRUE;
}

static void fill_cells(DWORD packed, DWORD len, int ch, int attr, DWORD *written)
{
    struct coord c = unpack(packed);
    int cols = con_cols(), rows = con_rows();
    DWORD done = 0;
    for (int x = c.x, y = c.y; done < len && y < rows; done++) {
        struct cell old = con_get_cell(x, y);
        con_put_cell(x, y, ch < 0 ? old.ch : (uint8_t)ch, attr < 0 ? old.attr : (uint8_t)attr);
        if (++x >= cols) {
            x = 0;
            y++;
        }
    }
    con_flush();
    if (written)
        *written = done;
}

static BOOL WINAPI FillConsoleOutputCharacterA(HANDLE h, char ch, DWORD len, DWORD at, DWORD *written)
{
    if (!require(h, true))
        return FALSE;
    fill_cells(at, len, (uint8_t)ch, -1, written);
    return TRUE;
}

static BOOL WINAPI FillConsoleOutputCharacterW(HANDLE h, WCHAR ch, DWORD len, DWORD at, DWORD *written)
{
    char c;
    w32_wide_to_cp(&ch, 1, &c, 1, 1);
    return FillConsoleOutputCharacterA(h, c, len, at, written);
}

static BOOL WINAPI FillConsoleOutputAttribute(HANDLE h, WORD attr, DWORD len, DWORD at, DWORD *written)
{
    if (!require(h, true))
        return FALSE;
    fill_cells(at, len, -1, (uint8_t)attr, written);
    return TRUE;
}

static BOOL WINAPI WriteConsoleOutputCharacterA(HANDLE h, const char *s, DWORD len, DWORD at, DWORD *written)
{
    if (!require(h, true))
        return FALSE;
    struct coord c = unpack(at);
    int cols = con_cols();
    for (DWORD i = 0; i < len; i++) {
        int x = (c.x + (int)i) % cols, y = c.y + (c.x + (int)i) / cols;
        con_put_cell(x, y, (uint8_t)s[i], con_get_cell(x, y).attr);
    }
    con_flush();
    if (written)
        *written = len;
    return TRUE;
}

static BOOL WINAPI WriteConsoleOutputCharacterW(HANDLE h, const WCHAR *s, DWORD len, DWORD at, DWORD *written)
{
    char *tmp = w32_alloc(len + 1);
    w32_wide_to_cp(s, (int)len, tmp, len, 1);
    BOOL ok = WriteConsoleOutputCharacterA(h, tmp, len, at, written);
    w32_free(tmp);
    return ok;
}

static BOOL WINAPI WriteConsoleOutputAttribute(HANDLE h, const WORD *attrs, DWORD len, DWORD at, DWORD *written)
{
    if (!require(h, true))
        return FALSE;
    struct coord c = unpack(at);
    int cols = con_cols();
    for (DWORD i = 0; i < len; i++) {
        int x = (c.x + (int)i) % cols, y = c.y + (c.x + (int)i) / cols;
        con_put_cell(x, y, con_get_cell(x, y).ch, (uint8_t)attrs[i]);
    }
    con_flush();
    if (written)
        *written = len;
    return TRUE;
}

static BOOL write_output(HANDLE h, const uint8_t *cells, DWORD size, DWORD origin, int16_t *rect, bool wide)
{
    if (!require(h, true))
        return FALSE;
    struct coord sz = unpack(size), org = unpack(origin);
    for (int y = rect[1]; y <= rect[3]; y++)
        for (int x = rect[0]; x <= rect[2]; x++) {
            int sx = org.x + x - rect[0], sy = org.y + y - rect[1];
            if (sx >= sz.x || sy >= sz.y)
                continue;
            const uint8_t *cell = cells + ((size_t)sy * sz.x + sx) * 4;
            char ch = (char)cell[0];
            if (wide) {
                WCHAR w = (WCHAR)(cell[0] | cell[1] << 8);
                w32_wide_to_cp(&w, 1, &ch, 1, 1);
            }
            con_put_cell(x, y, (uint8_t)ch, cell[2]);
        }
    con_flush();
    return TRUE;
}

static BOOL WINAPI WriteConsoleOutputA(HANDLE h, const uint8_t *cells, DWORD size, DWORD origin, int16_t *rect)
{
    return write_output(h, cells, size, origin, rect, false);
}

static BOOL WINAPI WriteConsoleOutputW(HANDLE h, const uint8_t *cells, DWORD size, DWORD origin, int16_t *rect)
{
    return write_output(h, cells, size, origin, rect, true);
}

static BOOL WINAPI ReadConsoleOutputCharacterA(HANDLE h, char *out, DWORD len, DWORD at, DWORD *read)
{
    if (!require(h, true))
        return FALSE;
    struct coord c = unpack(at);
    int cols = con_cols();
    for (DWORD i = 0; i < len; i++)
        out[i] = (char)con_get_cell((c.x + (int)i) % cols, c.y + (c.x + (int)i) / cols).ch;
    if (read)
        *read = len;
    return TRUE;
}

static BOOL WINAPI ScrollConsoleScreenBufferA(HANDLE h, const int16_t *rect, const int16_t *clip, DWORD dest,
                                              const uint8_t *fill)
{
    UNUSED(clip);
    if (!require(h, true))
        return FALSE;
    struct coord d = unpack(dest);
    int w = rect[2] - rect[0] + 1, hgt = rect[3] - rect[1] + 1;
    if (w <= 0 || hgt <= 0)
        return TRUE;
    struct cell *copy = w32_alloc(sizeof(struct cell) * (size_t)w * (size_t)hgt);
    if (!copy)
        return FALSE;
    for (int y = 0; y < hgt; y++)
        for (int x = 0; x < w; x++) {
            copy[y * w + x] = con_get_cell(rect[0] + x, rect[1] + y);
            con_put_cell(rect[0] + x, rect[1] + y, fill[0], fill[2]);
        }
    for (int y = 0; y < hgt; y++)
        for (int x = 0; x < w; x++)
            if (d.x + x >= 0 && d.y + y >= 0 && d.x + x < con_cols() && d.y + y < con_rows())
                con_put_cell(d.x + x, d.y + y, copy[y * w + x].ch, copy[y * w + x].attr);
    w32_free(copy);
    con_flush();
    return TRUE;
}

static BOOL WINAPI SetConsoleScreenBufferSize(HANDLE h, DWORD size)
{
    UNUSED(size);
    return require(h, true);
}

static BOOL WINAPI SetConsoleWindowInfo(HANDLE h, BOOL absolute, const int16_t *rect)
{
    UNUSED(absolute);
    UNUSED(rect);
    return require(h, true);
}

static DWORD WINAPI GetLargestConsoleWindowSize(HANDLE h)
{
    UNUSED(h);
    return (DWORD)con_cols() | (DWORD)con_rows() << 16;
}

static int pending_key = KEY_NONE;

int w32_console_getkey(bool wait)
{
    if (pending_key != KEY_NONE) {
        int k = pending_key;
        pending_key = KEY_NONE;
        return k;
    }
    for (;;) {
        int key = input_getkey(wait ? 100 : 0);
        if (input_break_pending() || key == KEY_CTRL('c')) {
            w32->exit_code = STATUS_CONTROL_C_EXIT;
            w32_exit(STATUS_CONTROL_C_EXIT);
        }
        if (key != KEY_NONE || !wait)
            return key;
    }
}

static bool peek_key(void)
{
    if (pending_key == KEY_NONE)
        pending_key = w32_console_getkey(false);
    return pending_key != KEY_NONE;
}

bool w32_key_info(int key, WORD *vk, WORD *scan, WCHAR *ch, DWORD *state)
{
    static const struct {
        int key;
        WORD vk, scan;
    } special[] = {
        {KEY_UP, 0x26, 0x48},     {KEY_DOWN, 0x28, 0x50},   {KEY_LEFT, 0x25, 0x4b},   {KEY_RIGHT, 0x27, 0x4d},
        {KEY_HOME, 0x24, 0x47},   {KEY_END, 0x23, 0x4f},    {KEY_PGUP, 0x21, 0x49},   {KEY_PGDN, 0x22, 0x51},
        {KEY_INSERT, 0x2d, 0x52}, {KEY_DELETE, 0x2e, 0x53}, {KEY_F1, 0x70, 0x3b},     {KEY_F2, 0x71, 0x3c},
        {KEY_F3, 0x72, 0x3d},     {KEY_F4, 0x73, 0x3e},     {KEY_F5, 0x74, 0x3f},     {KEY_F6, 0x75, 0x40},
        {KEY_F7, 0x76, 0x41},     {KEY_F8, 0x77, 0x42},     {KEY_F9, 0x78, 0x43},     {KEY_F10, 0x79, 0x44},
        {KEY_F11, 0x7a, 0x85},    {KEY_F12, 0x7b, 0x86},
    };
    int base = KEY_BASE(key);
    *state = (key & KEY_MOD_SHIFT ? 0x10 : 0) | (key & KEY_MOD_CTRL ? 0x8 : 0) | (key & KEY_MOD_ALT ? 0x2 : 0);
    *ch = 0;
    for (size_t i = 0; i < ARRAY_SIZE(special); i++)
        if (special[i].key == base) {
            *vk = special[i].vk;
            *scan = special[i].scan;
            *state |= base <= KEY_DELETE ? 0x100 : 0;
            return true;
        }
    if (base >= 0x100)
        return false;
    switch (base) {
    case KEY_ENTER:
    case '\r':
        *vk = 0x0d, *scan = 0x1c, *ch = '\r';
        return true;
    case KEY_ESC:
        *vk = 0x1b, *scan = 0x01, *ch = 0x1b;
        return true;
    case KEY_BACKSPACE:
    case 0x7f:
        *vk = 0x08, *scan = 0x0e, *ch = 0x08;
        return true;
    case KEY_TAB:
        *vk = 0x09, *scan = 0x0f, *ch = '\t';
        return true;
    case ' ':
        *vk = 0x20, *scan = 0x39, *ch = ' ';
        return true;
    }
    if (base < 0x20) {
        *vk = (WORD)('A' + base - 1);
        *scan = 0;
        *ch = (WCHAR)base;
        *state |= 0x8;
        return true;
    }
    char c = (char)base;
    w32_cp_to_wide(&c, 1, ch, 1, 1);
    *vk = is_alpha(base) ? (WORD)to_upper(base) : is_digit(base) ? (WORD)base : 0xba;
    *scan = 0;
    if (is_upper(base))
        *state |= 0x10;
    return true;
}

static void fill_record(uint8_t *rec, int key, bool wide)
{
    WORD vk = 0, scan = 0;
    WCHAR ch = 0;
    DWORD state = 0;
    w32_key_info(key, &vk, &scan, &ch, &state);
    memset(rec, 0, 20);
    rec[0] = KEY_EVENT;
    rec[4] = 1;
    rec[8] = 1;
    memcpy(rec + 10, &vk, 2);
    memcpy(rec + 12, &scan, 2);
    if (wide) {
        memcpy(rec + 14, &ch, 2);
    } else {
        char c = 0;
        if (ch)
            w32_wide_to_cp(&ch, 1, &c, 1, 1);
        rec[14] = (uint8_t)c;
    }
    memcpy(rec + 16, &state, 4);
}

static BOOL read_input(uint8_t *rec, DWORD n, DWORD *read, bool wide, bool peek)
{
    if (read)
        *read = 0;
    if (!n)
        return TRUE;
    int key;
    if (peek) {
        if (!peek_key())
            return TRUE;
        key = pending_key;
    } else {
        do
            key = w32_console_getkey(true);
        while (key == KEY_NONE || key == KEY_RESIZE);
    }
    fill_record(rec, key, wide);
    if (read)
        *read = 1;
    return TRUE;
}

static BOOL WINAPI ReadConsoleInputA(HANDLE h, uint8_t *rec, DWORD n, DWORD *read)
{
    UNUSED(h);
    return read_input(rec, n, read, false, false);
}

static BOOL WINAPI ReadConsoleInputW(HANDLE h, uint8_t *rec, DWORD n, DWORD *read)
{
    UNUSED(h);
    return read_input(rec, n, read, true, false);
}

static BOOL WINAPI PeekConsoleInputA(HANDLE h, uint8_t *rec, DWORD n, DWORD *read)
{
    UNUSED(h);
    return read_input(rec, n, read, false, true);
}

static BOOL WINAPI PeekConsoleInputW(HANDLE h, uint8_t *rec, DWORD n, DWORD *read)
{
    UNUSED(h);
    return read_input(rec, n, read, true, true);
}

static BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, DWORD *count)
{
    UNUSED(h);
    *count = peek_key() ? 1 : 0;
    return TRUE;
}

static BOOL WINAPI FlushConsoleInputBuffer(HANDLE h)
{
    UNUSED(h);
    pending_key = KEY_NONE;
    while (input_getkey(0) != KEY_NONE)
        ;
    return TRUE;
}

static BOOL WINAPI SetConsoleTitleA(LPCSTR t)
{
    str_copy(title, t, sizeof(title));
    return TRUE;
}

static BOOL WINAPI SetConsoleTitleW(LPCWSTR t)
{
    w32_wide_to_cp(t, (int)MIN(w32_wcslen(t) + 1, sizeof(title) - 1), title, sizeof(title) - 1, 1);
    title[sizeof(title) - 1] = 0;
    return TRUE;
}

static DWORD WINAPI GetConsoleTitleA(LPSTR out, DWORD size)
{
    str_copy(out, title, size);
    return (DWORD)strlen(out);
}

static DWORD WINAPI GetConsoleTitleW(LPWSTR out, DWORD size)
{
    size_t n = w32_cp_to_wide(title, -1, out, size, 1);
    if (size)
        out[MIN(n, (size_t)size) - 1] = 0;
    return (DWORD)w32_wcslen(out);
}

static UINT WINAPI GetConsoleCP(void)
{
    return (UINT)w32->input_cp;
}

static UINT WINAPI GetConsoleOutputCP(void)
{
    return (UINT)w32->output_cp;
}

static bool valid_cp(UINT cp)
{
    return cp == 65001 || cp == 1251 || cp == 1252 || cp == 28591 || codepage_supported((int)cp);
}

static BOOL WINAPI SetConsoleCP(UINT cp)
{
    if (!valid_cp(cp))
        return FALSE;
    w32->input_cp = (int)cp;
    return TRUE;
}

static BOOL WINAPI SetConsoleOutputCP(UINT cp)
{
    if (!valid_cp(cp))
        return FALSE;
    w32->output_cp = (int)cp;
    return TRUE;
}

static void *WINAPI GetConsoleWindow(void)
{
    return NULL;
}

static BOOL WINAPI ConsoleTrue(void)
{
    return TRUE;
}

static BOOL WINAPI AttachConsole(DWORD pid)
{
    UNUSED(pid);
    return TRUE;
}

static HANDLE WINAPI CreateConsoleScreenBuffer(DWORD access, DWORD share, void *sa, DWORD flags, void *data)
{
    UNUSED(access);
    UNUSED(share);
    UNUSED(sa);
    UNUSED(flags);
    UNUSED(data);
    return w32_std_handle(1);
}

static BOOL WINAPI SetConsoleActiveScreenBuffer(HANDLE h)
{
    UNUSED(h);
    return TRUE;
}

static BOOL WINAPI GetCurrentConsoleFont(HANDLE h, BOOL max, DWORD *info)
{
    UNUSED(h);
    UNUSED(max);
    info[0] = 0;
    info[1] = 8 | 16u << 16;
    return TRUE;
}

static const struct w32_export console_exports[] = {
    W32_FN(GetStdHandle), W32_FN(SetStdHandle), W32_FN(GetConsoleMode), W32_FN(SetConsoleMode),
    W32_FN(WriteConsoleA), W32_FN(WriteConsoleW), W32_FN(ReadConsoleA), W32_FN(ReadConsoleW),
    W32_FN(SetConsoleTextAttribute), W32_FN(GetConsoleScreenBufferInfo), W32_FN(GetConsoleScreenBufferInfoEx),
    W32_FN(SetConsoleCursorPosition), W32_FN(GetConsoleCursorInfo), W32_FN(SetConsoleCursorInfo),
    W32_FN(FillConsoleOutputCharacterA), W32_FN(FillConsoleOutputCharacterW), W32_FN(FillConsoleOutputAttribute),
    W32_FN(WriteConsoleOutputCharacterA), W32_FN(WriteConsoleOutputCharacterW),
    W32_FN(WriteConsoleOutputAttribute), W32_FN(WriteConsoleOutputA), W32_FN(WriteConsoleOutputW),
    W32_FN(ReadConsoleOutputCharacterA), W32_FN(ScrollConsoleScreenBufferA),
    W32_AS("ScrollConsoleScreenBufferW", ScrollConsoleScreenBufferA), W32_FN(SetConsoleScreenBufferSize),
    W32_FN(SetConsoleWindowInfo), W32_FN(GetLargestConsoleWindowSize), W32_FN(ReadConsoleInputA),
    W32_FN(ReadConsoleInputW), W32_FN(PeekConsoleInputA), W32_FN(PeekConsoleInputW),
    W32_FN(GetNumberOfConsoleInputEvents), W32_FN(FlushConsoleInputBuffer), W32_FN(SetConsoleTitleA),
    W32_FN(SetConsoleTitleW), W32_FN(GetConsoleTitleA), W32_FN(GetConsoleTitleW), W32_FN(GetConsoleCP),
    W32_FN(GetConsoleOutputCP), W32_FN(SetConsoleCP), W32_FN(SetConsoleOutputCP), W32_FN(GetConsoleWindow),
    W32_AS("AllocConsole", ConsoleTrue), W32_AS("FreeConsole", ConsoleTrue), W32_FN(AttachConsole),
    W32_FN(CreateConsoleScreenBuffer), W32_FN(SetConsoleActiveScreenBuffer), W32_FN(GetCurrentConsoleFont),
};

static void console_attach(void)
{
    pending_key = KEY_NONE;
    str_copy(title, "SynthOS", sizeof(title));
}

W32_EXPORTS(console_module, "kernel32.dll", console_exports, .attach = console_attach);
