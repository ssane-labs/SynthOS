#include <synth/config.h>
#if CONFIG_GUI
#include <synth/string.h>

#include "dlgtmpl.h"

/* FindText/ReplaceText: the modeless common dialog, built from an in-memory template; the owner gets the
   "commdlg_FindReplace" message with the caller's FINDREPLACE (in its 32- or 64-bit layout) */
enum {
    FR_DOWN = 1,
    FR_WHOLEWORD = 2,
    FR_MATCHCASE = 4,
    FR_FINDNEXT = 8,
    FR_REPLACE = 0x10,
    FR_REPLACEALL = 0x20,
    FR_DIALOGTERM = 0x40,
    FR_NOUPDOWN = 0x400,
    FR_NOMATCHCASE = 0x800,
    FR_NOWHOLEWORD = 0x1000,
    FR_HIDEUPDOWN = 0x4000,
    FR_HIDEMATCHCASE = 0x8000,
    FR_HIDEWHOLEWORD = 0x10000,
    ID_FIND = 0x480,
    ID_WITH = 0x481,
    ID_WHOLE = 0x410,
    ID_CASE = 0x411,
    ID_UP = 0x420,
    ID_DOWN = 0x421,
    ID_REPLACE = 0x400,
    ID_REPLACEALL = 0x401,
    WM_INITDIALOG_ = 0x110,
    WM_CLOSE_ = 0x10,
};

struct find_state {
    uint8_t *fr;
    bool wide;
    HANDLE owner;
    UINT msg;
};

static int off_flags(void)
{
    return w32->win64 ? 24 : 12;
}

static uint32_t flags_of(const struct find_state *s)
{
    uint32_t f;
    memcpy(&f, s->fr + off_flags(), 4);
    return f;
}

static void *field_ptr(const struct find_state *s, int off32, int off64)
{
    uint32_t p;
    memcpy(&p, s->fr + (w32->win64 ? off64 : off32), 4);
    return (void *)(uintptr_t)p;
}

static int field_len(const struct find_state *s, int off32, int off64)
{
    uint16_t n;
    memcpy(&n, s->fr + (w32->win64 ? off64 : off32), 2);
    return n;
}

#define CALL(dll, name, type) ((type)w32_builtin_lookup(dll, name))

static void read_text(HANDLE dlg, int id, const struct find_state *s, void *buf, int len)
{
    if (!buf || len <= 0)
        return;
    if (s->wide)
        CALL("user32.dll", "GetDlgItemTextW", UINT(WINAPI *)(HANDLE, int, void *, int))(dlg, id, buf, len);
    else
        CALL("user32.dll", "GetDlgItemTextA", UINT(WINAPI *)(HANDLE, int, void *, int))(dlg, id, buf, len);
}

static void write_text(HANDLE dlg, int id, const struct find_state *s, const void *text)
{
    if (!text)
        return;
    if (s->wide)
        CALL("user32.dll", "SetDlgItemTextW", BOOL(WINAPI *)(HANDLE, int, const void *))(dlg, id, text);
    else
        CALL("user32.dll", "SetDlgItemTextA", BOOL(WINAPI *)(HANDLE, int, const void *))(dlg, id, text);
}

static bool checked(HANDLE dlg, int id)
{
    return CALL("user32.dll", "IsDlgButtonChecked", UINT(WINAPI *)(HANDLE, int))(dlg, id) == 1;
}

static void report(HANDLE dlg, struct find_state *s, uint32_t action)
{
    uint32_t f = flags_of(s) & ~(uint32_t)(FR_FINDNEXT | FR_REPLACE | FR_REPLACEALL | FR_DIALOGTERM | FR_DOWN |
                                           FR_MATCHCASE | FR_WHOLEWORD);
    if (action != FR_DIALOGTERM) {
        read_text(dlg, ID_FIND, s, field_ptr(s, 16, 32), field_len(s, 24, 48));
        read_text(dlg, ID_WITH, s, field_ptr(s, 20, 40), field_len(s, 26, 50));
        f |= (checked(dlg, ID_CASE) ? FR_MATCHCASE : 0) | (checked(dlg, ID_WHOLE) ? FR_WHOLEWORD : 0) |
             (checked(dlg, ID_UP) ? 0 : FR_DOWN);
    }
    f |= action;
    memcpy(s->fr + off_flags(), &f, 4);
    struct w32_window *owner = w32_win(s->owner);
    if (owner)
        w32_send(owner, s->msg, 0, (LPARAM)(uintptr_t)s->fr);
}

static intptr_t WINAPI find_proc(HANDLE dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(dlg);
    if (!w)
        return FALSE;
    struct find_state *s = (struct find_state *)(uintptr_t)w->dlg_user;
    if (msg == WM_INITDIALOG_) {
        w->dlg_user = lp;
        s = (struct find_state *)(uintptr_t)lp;
        write_text(dlg, ID_FIND, s, field_ptr(s, 16, 32));
        write_text(dlg, ID_WITH, s, field_ptr(s, 20, 40));
        uint32_t f = flags_of(s);
        BOOL(WINAPI * check)(HANDLE, int, UINT) = CALL("user32.dll", "CheckDlgButton", BOOL(WINAPI *)(HANDLE, int, UINT));
        check(dlg, ID_CASE, (f & FR_MATCHCASE) != 0);
        check(dlg, ID_WHOLE, (f & FR_WHOLEWORD) != 0);
        check(dlg, (f & FR_DOWN) ? ID_DOWN : ID_UP, 1);
        return TRUE;
    }
    if (!s)
        return FALSE;
    int id = msg == WM_CLOSE_ ? 2 : msg == WM_COMMAND && !(wp >> 16) ? (int)(wp & 0xffff) : 0;
    if (id == ID_UP || id == ID_DOWN) {
        CALL("user32.dll", "CheckDlgButton", BOOL(WINAPI *)(HANDLE, int, UINT))(dlg, id == ID_UP ? ID_DOWN : ID_UP, 0);
        CALL("user32.dll", "CheckDlgButton", BOOL(WINAPI *)(HANDLE, int, UINT))(dlg, id, 1);
        return TRUE;
    }
    uint32_t action = id == 1 ? FR_FINDNEXT : id == ID_REPLACE ? FR_REPLACE : id == ID_REPLACEALL ? FR_REPLACEALL
                    : id == 2 ? FR_DIALOGTERM : 0;
    if (!action)
        return FALSE;
    report(dlg, s, action);
    if (action == FR_DIALOGTERM) {
        w->dlg_user = 0;
        w32_free(s);
        CALL("user32.dll", "DestroyWindow", BOOL(WINAPI *)(HANDLE))(dlg);
    }
    return TRUE;
}

static HANDLE find_dialog(uint8_t *fr, bool wide, bool replace)
{
    if (!fr)
        return NULL;
    struct find_state *s = w32_zalloc(sizeof(*s));
    struct tmpl t = {w32_zalloc(2048), 0};
    if (!s || !t.p) {
        w32_free(s);
        w32_free(t.p);
        return NULL;
    }
    uint32_t owner;
    memcpy(&owner, fr + (w32->win64 ? 8 : 4), 4);
    *s = (struct find_state){fr, wide, (HANDLE)(uintptr_t)owner,
                             CALL("user32.dll", "RegisterWindowMessageA", UINT(WINAPI *)(const char *))(
                                 "commdlg_FindReplace")};
    uint32_t f = flags_of(s);
    bool updown = !replace && !(f & FR_HIDEUPDOWN), mcase = !(f & FR_HIDEMATCHCASE), whole = !(f & FR_HIDEWHOLEWORD);
    int count = (replace ? 8 : 4) + updown * 2 + mcase + whole;
    put32(&t, 0x80000000u | WS_CAPTION | WS_SYSMENU | 0x40 | 0x80 | 0x800);
    put32(&t, 0);
    put16(&t, (uint16_t)count);
    put16(&t, 30);
    put16(&t, 30);
    put16(&t, replace ? 230 : 236);
    put16(&t, replace ? 94 : 62);
    put16(&t, 0);
    put16(&t, 0);
    put_str(&t, replace ? "Replace" : "Find");
    put16(&t, 8);
    put_str(&t, "MS Shell Dlg");
    uint32_t edit = WS_BORDER | WS_TABSTOP | 0x80, box = WS_TABSTOP | 3, button = WS_TABSTOP;
    item(&t, 0, 4, 9, 48, 8, -1, 0x82, "Find what:");
    item(&t, edit, 54, 7, 114, 12, ID_FIND, 0x81, "");
    if (replace) {
        item(&t, 0, 4, 26, 48, 8, -1, 0x82, "Replace with:");
        item(&t, edit, 54, 24, 114, 12, ID_WITH, 0x81, "");
    }
    int y = replace ? 46 : 26;
    if (whole)
        item(&t, box | ((f & FR_NOWHOLEWORD) ? WS_DISABLED : 0), 5, y, 104, 12, ID_WHOLE, 0x80, "Match whole word only");
    if (mcase)
        item(&t, box | ((f & FR_NOMATCHCASE) ? WS_DISABLED : 0), 5, y + 16, 64, 12, ID_CASE, 0x80, "Match case");
    if (updown) {
        uint32_t dis = (f & FR_NOUPDOWN) ? WS_DISABLED : 0;
        item(&t, WS_TABSTOP | 4 | dis, 111, 42, 25, 12, ID_UP, 0x80, "Up");
        item(&t, WS_TABSTOP | 4 | dis, 138, 42, 35, 12, ID_DOWN, 0x80, "Down");
    }
    int bx = replace ? 174 : 182;
    item(&t, button | 1, bx, 5, 50, 14, 1, 0x80, "Find Next");
    if (replace) {
        item(&t, button, bx, 22, 50, 14, ID_REPLACE, 0x80, "Replace");
        item(&t, button, bx, 39, 50, 14, ID_REPLACEALL, 0x80, "Replace All");
    }
    item(&t, button, bx, replace ? 56 : 23, 50, 14, 2, 0x80, "Cancel");
    typedef HANDLE(WINAPI * create_fn)(HANDLE, const void *, HANDLE, void *, LPARAM);
    HANDLE dlg = CALL("user32.dll", "CreateDialogIndirectParamA", create_fn)(NULL, t.p, s->owner, (void *)find_proc,
                                                                              (LPARAM)(uintptr_t)s);
    w32_free(t.p);
    if (!dlg)
        w32_free(s);
    else
        CALL("user32.dll", "ShowWindow", BOOL(WINAPI *)(HANDLE, int))(dlg, 5);
    return dlg;
}

static HANDLE WINAPI FindTextW(uint8_t *fr)
{
    return find_dialog(fr, true, false);
}

static HANDLE WINAPI FindTextA(uint8_t *fr)
{
    return find_dialog(fr, false, false);
}

static HANDLE WINAPI ReplaceTextW(uint8_t *fr)
{
    return find_dialog(fr, true, true);
}

static HANDLE WINAPI ReplaceTextA(uint8_t *fr)
{
    return find_dialog(fr, false, true);
}

/* ChooseColor: a modal dialog with the basic palette, the caller's custom colors and RGB fields */
enum { CC_RGBINIT = 1, ID_RED = 0x2c2, ID_GREEN = 0x2c3, ID_BLUE = 0x2c4, ID_ADD = 0x2c8, WM_PAINT_ = 0xf,
       WM_LBUTTONDOWN_ = 0x201, EN_CHANGE_ = 0x300 };

static const uint32_t basic_colors[48] = {
    0xff8080, 0xffff80, 0x80ff80, 0x00ff80, 0x80ffff, 0x0080ff, 0xff80c0, 0xff80ff, 0xff0000, 0xffff00,
    0x80ff00, 0x00ff40, 0x00ffff, 0x0080c0, 0x8080c0, 0xff00ff, 0x804040, 0xff8040, 0x00ff00, 0x008080,
    0x004080, 0x8080ff, 0x800040, 0xff0080, 0x800000, 0xff8000, 0x008000, 0x008040, 0x0000ff, 0x0000a0,
    0x800080, 0x8000ff, 0x400000, 0x804000, 0x004000, 0x004040, 0x000080, 0x000040, 0x400040, 0x400080,
    0x000000, 0x808000, 0x808040, 0x808080, 0x408080, 0xc0c0c0, 0x400040, 0xffffff,
};

struct color_state {
    uint8_t *cc;
    uint32_t color, *custom;
    int next;
    bool setting;
};

static uint32_t colorref(uint32_t rgb)
{
    return (rgb >> 16 & 0xff) | (rgb & 0xff00) | (rgb & 0xff) << 16;
}

static RECT swatch_rect(HANDLE dlg, int i)
{
    int col = i % 8, row = i < 48 ? i / 8 : (i - 48) / 8;
    int y = i < 48 ? 14 + row * 13 : 108 + row * 13;
    RECT r = {4 + col * 16, y, 4 + col * 16 + 13, y + 10};
    CALL("user32.dll", "MapDialogRect", BOOL(WINAPI *)(HANDLE, RECT *))(dlg, &r);
    return r;
}

static RECT preview_rect(HANDLE dlg)
{
    RECT r = {140, 14, 226, 42};
    CALL("user32.dll", "MapDialogRect", BOOL(WINAPI *)(HANDLE, RECT *))(dlg, &r);
    return r;
}

static uint32_t swatch_color(const struct color_state *s, int i)
{
    return i < 48 ? colorref(basic_colors[i]) : s->custom ? s->custom[i - 48] : 0xffffff;
}

static void fill(HANDLE dc, RECT r, uint32_t color, bool frame)
{
    HANDLE brush = CALL("gdi32.dll", "CreateSolidBrush", HANDLE(WINAPI *)(uint32_t))(color);
    CALL("user32.dll", "FillRect", int(WINAPI *)(HANDLE, const RECT *, HANDLE))(dc, &r, brush);
    CALL("gdi32.dll", "DeleteObject", BOOL(WINAPI *)(HANDLE))(brush);
    if (frame) {
        HANDLE edge = CALL("gdi32.dll", "CreateSolidBrush", HANDLE(WINAPI *)(uint32_t))(0x808080);
        CALL("user32.dll", "FrameRect", int(WINAPI *)(HANDLE, const RECT *, HANDLE))(dc, &r, edge);
        CALL("gdi32.dll", "DeleteObject", BOOL(WINAPI *)(HANDLE))(edge);
    }
}

static void color_paint(HANDLE dlg, struct color_state *s)
{
    uint8_t ps[72];
    HANDLE dc = CALL("user32.dll", "BeginPaint", HANDLE(WINAPI *)(HANDLE, void *))(dlg, ps);
    if (!dc)
        return;
    for (int i = 0; i < 64; i++) {
        RECT r = swatch_rect(dlg, i);
        if (swatch_color(s, i) == s->color)
            fill(dc, (RECT){r.left - 2, r.top - 2, r.right + 2, r.bottom + 2}, 0, false);
        fill(dc, r, swatch_color(s, i), true);
    }
    fill(dc, preview_rect(dlg), s->color, true);
    CALL("user32.dll", "EndPaint", BOOL(WINAPI *)(HANDLE, void *))(dlg, ps);
}

static void color_fields(HANDLE dlg, struct color_state *s)
{
    BOOL(WINAPI * set)(HANDLE, int, UINT, BOOL) = CALL("user32.dll", "SetDlgItemInt", BOOL(WINAPI *)(HANDLE, int, UINT, BOOL));
    s->setting = true;
    set(dlg, ID_RED, s->color & 0xff, FALSE);
    set(dlg, ID_GREEN, s->color >> 8 & 0xff, FALSE);
    set(dlg, ID_BLUE, s->color >> 16 & 0xff, FALSE);
    s->setting = false;
}

static void color_read(HANDLE dlg, struct color_state *s)
{
    UINT(WINAPI * get)(HANDLE, int, BOOL *, BOOL) = CALL("user32.dll", "GetDlgItemInt", UINT(WINAPI *)(HANDLE, int, BOOL *, BOOL));
    uint32_t r = MIN(get(dlg, ID_RED, NULL, FALSE), 255u), g = MIN(get(dlg, ID_GREEN, NULL, FALSE), 255u),
             b = MIN(get(dlg, ID_BLUE, NULL, FALSE), 255u);
    s->color = r | g << 8 | b << 16;
}

static void color_redraw(HANDLE dlg)
{
    CALL("user32.dll", "InvalidateRect", BOOL(WINAPI *)(HANDLE, const RECT *, BOOL))(dlg, NULL, TRUE);
}

static intptr_t WINAPI color_proc(HANDLE dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(dlg);
    if (!w)
        return FALSE;
    struct color_state *s = (struct color_state *)(uintptr_t)w->dlg_user;
    if (msg == WM_INITDIALOG_) {
        w->dlg_user = lp;
        color_fields(dlg, (struct color_state *)(uintptr_t)lp);
        return TRUE;
    }
    if (!s)
        return FALSE;
    BOOL(WINAPI * end)(HANDLE, intptr_t) = CALL("user32.dll", "EndDialog", BOOL(WINAPI *)(HANDLE, intptr_t));
    if (msg == WM_PAINT_) {
        color_paint(dlg, s);
        return TRUE;
    }
    if (msg == WM_LBUTTONDOWN_) {
        int x = (int16_t)(lp & 0xffff), y = (int16_t)(lp >> 16 & 0xffff);
        for (int i = 0; i < 64; i++) {
            RECT r = swatch_rect(dlg, i);
            if (x >= r.left - 1 && x <= r.right + 1 && y >= r.top - 1 && y <= r.bottom + 1) {
                s->color = swatch_color(s, i);
                color_fields(dlg, s);
                color_redraw(dlg);
                return TRUE;
            }
        }
        return FALSE;
    }
    if (msg == WM_CLOSE_) {
        end(dlg, 0);
        return TRUE;
    }
    if (msg != WM_COMMAND)
        return FALSE;
    int id = (int)(wp & 0xffff);
    if ((id == ID_RED || id == ID_GREEN || id == ID_BLUE) && (wp >> 16) == EN_CHANGE_ && !s->setting) {
        color_read(dlg, s);
        color_redraw(dlg);
        return TRUE;
    }
    if (id == ID_ADD && s->custom) {
        s->custom[s->next] = s->color;
        s->next = (s->next + 1) % 16;
        color_redraw(dlg);
        return TRUE;
    }
    if (id == 1 || id == 2) {
        if (id == 1)
            color_read(dlg, s);
        end(dlg, id == 1);
        return TRUE;
    }
    return FALSE;
}

static BOOL choose_color(uint8_t *cc)
{
    if (!cc)
        return FALSE;
    int ps = w32->win64 ? 8 : 4;
    uint32_t owner, flags, custom;
    memcpy(&owner, cc + ps, 4);
    memcpy(&flags, cc + 5 * ps, 4);
    memcpy(&custom, cc + 4 * ps, 4);
    struct color_state s = {cc, 0xffffff, (uint32_t *)(uintptr_t)custom, 0, false};
    if (flags & CC_RGBINIT)
        memcpy(&s.color, cc + 3 * ps, 4);
    struct tmpl t = {w32_zalloc(2048), 0};
    if (!t.p)
        return FALSE;
    put32(&t, 0x80000000u | WS_CAPTION | WS_SYSMENU | 0x40 | 0x80);
    put32(&t, 0);
    put16(&t, 12);
    put16(&t, 30);
    put16(&t, 30);
    put16(&t, 232);
    put16(&t, 152);
    put16(&t, 0);
    put16(&t, 0);
    put_str(&t, "Color");
    put16(&t, 8);
    put_str(&t, "MS Shell Dlg");
    uint32_t edit = WS_BORDER | WS_TABSTOP | 0x80 | 0x2000;
    item(&t, 0, 4, 4, 120, 8, -1, 0x82, "Basic colors:");
    item(&t, 0, 4, 98, 120, 8, -1, 0x82, "Custom colors:");
    item(&t, 0, 140, 52, 30, 8, -1, 0x82, "Red:");
    item(&t, edit, 176, 50, 30, 12, ID_RED, 0x81, "");
    item(&t, 0, 140, 68, 30, 8, -1, 0x82, "Green:");
    item(&t, edit, 176, 66, 30, 12, ID_GREEN, 0x81, "");
    item(&t, 0, 140, 84, 30, 8, -1, 0x82, "Blue:");
    item(&t, edit, 176, 82, 30, 12, ID_BLUE, 0x81, "");
    item(&t, WS_TABSTOP | (custom ? 0 : WS_DISABLED), 140, 104, 86, 14, ID_ADD, 0x80, "Add to Custom Colors");
    item(&t, 0, 140, 4, 60, 8, -1, 0x82, "Color:");
    item(&t, WS_TABSTOP | 1, 132, 132, 46, 14, 1, 0x80, "OK");
    item(&t, WS_TABSTOP, 182, 132, 46, 14, 2, 0x80, "Cancel");
    typedef intptr_t(WINAPI * box_fn)(HANDLE, const void *, HANDLE, void *, LPARAM);
    intptr_t r = CALL("user32.dll", "DialogBoxIndirectParamA", box_fn)(NULL, t.p, (HANDLE)(uintptr_t)owner,
                                                                       (void *)color_proc, (LPARAM)(uintptr_t)&s);
    w32_free(t.p);
    if (r != 1)
        return FALSE;
    memcpy(cc + 3 * ps, &s.color, 4);
    return TRUE;
}

static BOOL WINAPI ChooseColorW(uint8_t *cc)
{
    return choose_color(cc);
}

static BOOL WINAPI ChooseColorA(uint8_t *cc)
{
    return choose_color(cc);
}

static const struct w32_export findtext_exports[] = {
    W32_FN(FindTextW), W32_FN(FindTextA), W32_FN(ReplaceTextW), W32_FN(ReplaceTextA), W32_FN(ChooseColorW),
    W32_FN(ChooseColorA),
};

W32_EXPORTS(findtext_module, "comdlg32.dll", findtext_exports);
#endif
