#include <synth/config.h>
#if CONFIG_GUI
#include <synth/keys.h>
#include <synth/string.h>

#include "w32gui.h"

#define DS_SETFONT 0x40
#define DS_CENTER 0x800
#define BASE_X 7
#define BASE_Y 15

typedef intptr_t WINAPI (*DLGPROC)(HANDLE, UINT, WPARAM, LPARAM);

struct reader {
    const uint8_t *p;
};

static uint16_t word(struct reader *r)
{
    uint16_t v = (uint16_t)(r->p[0] | r->p[1] << 8);
    r->p += 2;
    return v;
}

static uint32_t dword(struct reader *r)
{
    uint32_t lo = word(r);
    return lo | (uint32_t)word(r) << 16;
}

static void align4(struct reader *r, const uint8_t *base)
{
    while ((r->p - base) & 3)
        r->p++;
}

static char *string_or_ordinal(struct reader *r, uint16_t *ordinal)
{
    *ordinal = 0;
    uint16_t first = (uint16_t)(r->p[0] | r->p[1] << 8);
    if (first == 0xffff) {
        r->p += 2;
        *ordinal = word(r);
        return NULL;
    }
    const WCHAR *s = (const WCHAR *)r->p;
    while (word(r))
        ;
    return *s ? w32_narrow(s) : NULL;
}

static const char *control_class(uint16_t ordinal, const char *name)
{
    switch (ordinal) {
    case 0x80: return "Button";
    case 0x81: return "Edit";
    case 0x82: return "Static";
    default: return name ? name : "Static";
    }
}

static int px_x(int dlu)
{
    return dlu * BASE_X / 4;
}

static int px_y(int dlu)
{
    return dlu * BASE_Y / 8;
}

static HANDLE create_dialog(HANDLE inst, const void *templ, HANDLE owner, DLGPROC proc, LPARAM param, bool modal)
{
    const uint8_t *base = templ;
    struct reader r = {base};
    bool ex = base[2] == 0xff && base[3] == 0xff;
    uint32_t style, exstyle;
    if (ex) {
        r.p += 4;
        dword(&r);
        exstyle = dword(&r);
        style = dword(&r);
    } else {
        style = dword(&r);
        exstyle = dword(&r);
    }
    int count = word(&r);
    int x = (int16_t)word(&r), y = (int16_t)word(&r), cx = (int16_t)word(&r), cy = (int16_t)word(&r);
    uint16_t ord;
    char *menu_name = string_or_ordinal(&r, &ord);
    uint16_t menu_ord = ord;
    char *class_name = string_or_ordinal(&r, &ord);
    char *title = string_or_ordinal(&r, &ord);
    HANDLE font = NULL;
    if (style & DS_SETFONT) {
        int points = word(&r);
        int weight = 400;
        if (ex) {
            weight = word(&r);
            r.p += 2;
        }
        char *face = string_or_ordinal(&r, &ord);
        typedef HANDLE WINAPI (*font_fn)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                         DWORD, LPCSTR);
        struct w32_image *g = w32_find_module("gdi32.dll");
        font_fn create = g ? (font_fn)w32_get_proc(g, "CreateFontA", 0) : NULL;
        if (create)
            font = create(-(points * 96 / 72), 0, 0, 0, weight, 0, 0, 0, 0, 0, 0, 0, 0, face);
        w32_free(face);
    }
    HANDLE menu = NULL;
    if (menu_name || menu_ord) {
        typedef HANDLE WINAPI (*menu_fn)(HANDLE, LPCSTR);
        struct w32_image *u = w32_find_module("user32.dll");
        menu_fn load = u ? (menu_fn)w32_get_proc(u, "LoadMenuA", 0) : NULL;
        if (load)
            menu = load(inst, menu_name ? menu_name : (LPCSTR)(uintptr_t)menu_ord);
    }
    uint32_t window_style = (style & ~(WS_VISIBLE | WS_CHILD)) | WS_CAPTION;
    if (style & WS_CHILD)
        window_style = style & ~WS_VISIBLE;
    int width = px_x(cx), height = px_y(cy) + ((style & WS_CHILD) ? 0 : TITLE_HEIGHT);
    HANDLE dlg = w32_create_window(exstyle, class_name ? class_name : "#32770", title ? title : "", window_style,
                                   (style & DS_CENTER) || (style & WS_CHILD) == 0 ? W32_CW_USEDEFAULT : px_x(x),
                                   px_y(y), width, height, owner, menu, inst, NULL);
    w32_free(menu_name);
    w32_free(class_name);
    w32_free(title);
    struct w32_window *w = w32_win(dlg);
    if (!w)
        return NULL;
    w->dlgproc = (void *)proc;
    w->font = font;
    HANDLE first_tab = NULL;
    for (int i = 0; i < count; i++) {
        align4(&r, base);
        uint32_t item_style, item_ex, id;
        if (ex) {
            dword(&r);
            item_ex = dword(&r);
            item_style = dword(&r);
        } else {
            item_style = dword(&r);
            item_ex = dword(&r);
        }
        int ix = (int16_t)word(&r), iy = (int16_t)word(&r), icx = (int16_t)word(&r), icy = (int16_t)word(&r);
        id = ex ? dword(&r) : word(&r);
        char *cls = string_or_ordinal(&r, &ord);
        const char *class_of = control_class(ord, cls);
        uint16_t title_ord;
        char *text = string_or_ordinal(&r, &title_ord);
        uint16_t extra = word(&r);
        r.p += extra;
        HANDLE c = w32_create_window(item_ex, class_of, text ? text : "", item_style | WS_CHILD, px_x(ix), px_y(iy),
                                     px_x(icx), px_y(icy), dlg, (HANDLE)(uintptr_t)id, inst, NULL);
        struct w32_window *cw = w32_win(c);
        if (cw) {
            cw->font = font;
            if (!first_tab && (item_style & WS_TABSTOP))
                first_tab = c;
        }
        w32_free(cls);
        w32_free(text);
    }
    if (proc && proc(dlg, WM_INITDIALOG, (WPARAM)first_tab, param) && first_tab)
        w32_set_focus(w32_win(first_tab));
    if ((style & WS_VISIBLE) || modal) {
        typedef BOOL WINAPI (*show_fn)(HANDLE, int);
        struct w32_image *u = w32_find_module("user32.dll");
        show_fn show = u ? (show_fn)w32_get_proc(u, "ShowWindow", 0) : NULL;
        if (show)
            show(dlg, 5);
    }
    return dlg;
}

LRESULT WINAPI w32_dialog_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    DLGPROC proc = (DLGPROC)w->dlgproc;
    intptr_t saved = w->dlg_msgresult;
    w->dlg_msgresult = 0;
    intptr_t handled = proc && msg != WM_NCCREATE && msg != WM_CREATE ? proc(h, msg, wp, lp) : 0;
    if (msg >= WM_CTLCOLOREDIT && msg <= WM_CTLCOLORSTATIC && handled)
        return handled;
    intptr_t result = w->dlg_msgresult;
    if (w32_win(h))
        w->dlg_msgresult = saved;
    if (handled)
        return msg == WM_INITDIALOG ? handled : result ? result : handled;
    switch (msg) {
    case WM_CLOSE:
        w32_post(w, WM_COMMAND, 2, 0);
        return 0;
    case WM_COMMAND:
        if (!proc && ((uint16_t)wp == 1 || (uint16_t)wp == 2)) {
            w->dlg_result = (uint16_t)wp;
            w->dlg_ended = true;
        }
        return 0;
    default: return w32_def_window_proc(h, msg, wp, lp);
    }
}

static struct w32_window *next_tab(struct w32_window *dlg, struct w32_window *from, bool backwards)
{
    struct w32_window *list[64];
    int n = 0, at = -1;
    for (struct w32_window *c = dlg->child; c && n < 64; c = c->sibling)
        if (c->visible && c->enabled && (c->style & WS_TABSTOP)) {
            if (c == from)
                at = n;
            list[n++] = c;
        }
    if (!n)
        return NULL;
    if (at < 0)
        return list[0];
    return list[(at + (backwards ? n - 1 : 1)) % n];
}

static struct w32_window *default_button(struct w32_window *dlg)
{
    for (struct w32_window *c = dlg->child; c; c = c->sibling)
        if (c->control == CK_BUTTON && (c->style & 0xf) == 1)
            return c;
    return NULL;
}

bool w32_dialog_key(struct w32_window *dlg, struct w32_window *focus, int key)
{
    int base = KEY_BASE(key);
    if (base == KEY_ESC) {
        w32_post(dlg, WM_COMMAND, 2, 0);
        return true;
    }
    if (base == KEY_TAB) {
        struct w32_window *next = next_tab(dlg, focus, key & KEY_MOD_SHIFT);
        if (next)
            w32_set_focus(next);
        return true;
    }
    if (base == KEY_ENTER || base == '\r' || base == '\n') {
        if (focus && focus->control == CK_EDIT && (focus->style & 4))
            return false;
        if (focus && focus->control == CK_BUTTON && focus != dlg)
            return false;
        struct w32_window *def = default_button(dlg);
        w32_post(dlg, WM_COMMAND, def ? (WPARAM)(uint16_t)def->id : 1, def ? (LPARAM)def : 0);
        return true;
    }
    return false;
}

static intptr_t run_modal(HANDLE dlg, HANDLE owner)
{
    struct w32_window *w = w32_win(dlg);
    struct w32_window *o = w32_win(owner);
    bool owner_enabled = o && o->enabled;
    if (o)
        o->enabled = false;
    while (w32_win(dlg) && !w->dlg_ended) {
        MSG m;
        if (!w32_next_message(&m)) {
            w32_block();
            continue;
        }
        if (m.message == WM_QUIT) {
            typedef void WINAPI (*quit_fn)(int);
            struct w32_image *u = w32_find_module("user32.dll");
            quit_fn quit = u ? (quit_fn)w32_get_proc(u, "PostQuitMessage", 0) : NULL;
            if (quit)
                quit((int)m.wParam);
            break;
        }
        struct w32_window *target = w32_win(m.hwnd);
        bool input = (m.message >= WM_KEYDOWN && m.message <= WM_SYSCHAR) ||
                     (m.message >= WM_MOUSEMOVE && m.message <= WM_MOUSEWHEEL);
        if (input && target && w32_top(target) != w)
            continue;
        w32_dispatch(&m);
    }
    intptr_t result = w32_win(dlg) ? w->dlg_result : -1;
    if (o)
        o->enabled = owner_enabled;
    extern BOOL WINAPI w32_destroy_window(HANDLE);
    if (w32_win(dlg))
        w32_destroy_window(dlg);
    if (o && o->wm)
        wm_focus(o->wm);
    return result;
}

static intptr_t WINAPI DialogBoxIndirectParamA(HANDLE inst, const void *templ, HANDLE owner, DLGPROC proc,
                                               LPARAM param)
{
    HANDLE dlg = templ ? create_dialog(inst, templ, owner, proc, param, true) : NULL;
    return dlg ? run_modal(dlg, owner) : -1;
}

static intptr_t dialog_box(HANDLE inst, const void *name, HANDLE owner, DLGPROC proc, LPARAM param, bool wide)
{
    const void *templ;
    if (wide && (uintptr_t)name >= 0x10000) {
        char *n = w32_narrow(name);
        templ = w32_find_resource(inst, (const void *)5, n, NULL);
        w32_free(n);
    } else {
        templ = w32_find_resource(inst, (const void *)5, name, NULL);
    }
    if (!templ) {
        w32_set_error(1812);
        return -1;
    }
    return DialogBoxIndirectParamA(inst, templ, owner, proc, param);
}

static intptr_t WINAPI DialogBoxParamA(HANDLE inst, LPCSTR name, HANDLE owner, DLGPROC proc, LPARAM param)
{
    return dialog_box(inst, name, owner, proc, param, false);
}

static intptr_t WINAPI DialogBoxParamW(HANDLE inst, LPCWSTR name, HANDLE owner, DLGPROC proc, LPARAM param)
{
    return dialog_box(inst, name, owner, proc, param, true);
}

static HANDLE WINAPI CreateDialogIndirectParamA(HANDLE inst, const void *templ, HANDLE owner, DLGPROC proc,
                                                LPARAM param)
{
    return templ ? create_dialog(inst, templ, owner, proc, param, false) : NULL;
}

static HANDLE create_dialog_named(HANDLE inst, const void *name, HANDLE owner, DLGPROC proc, LPARAM param, bool wide)
{
    const void *templ;
    if (wide && (uintptr_t)name >= 0x10000) {
        char *n = w32_narrow(name);
        templ = w32_find_resource(inst, (const void *)5, n, NULL);
        w32_free(n);
    } else {
        templ = w32_find_resource(inst, (const void *)5, name, NULL);
    }
    return templ ? create_dialog(inst, templ, owner, proc, param, false) : NULL;
}

static HANDLE WINAPI CreateDialogParamA(HANDLE inst, LPCSTR name, HANDLE owner, DLGPROC proc, LPARAM param)
{
    return create_dialog_named(inst, name, owner, proc, param, false);
}

static HANDLE WINAPI CreateDialogParamW(HANDLE inst, LPCWSTR name, HANDLE owner, DLGPROC proc, LPARAM param)
{
    return create_dialog_named(inst, name, owner, proc, param, true);
}

static BOOL WINAPI EndDialog(HANDLE h, intptr_t result)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    w->dlg_result = result;
    w->dlg_ended = true;
    return TRUE;
}

static LRESULT WINAPI DefDlgProcA(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    return w32_def_window_proc(h, msg, wp, lp);
}

static BOOL WINAPI MapDialogRect(HANDLE h, RECT *r)
{
    UNUSED(h);
    r->left = px_x(r->left);
    r->right = px_x(r->right);
    r->top = px_y(r->top);
    r->bottom = px_y(r->bottom);
    return TRUE;
}

static LONG WINAPI GetDialogBaseUnits(void)
{
    return BASE_X | BASE_Y << 16;
}

static BOOL WINAPI IsDialogMessageA(HANDLE dlg, MSG *msg)
{
    struct w32_window *w = w32_win(dlg), *target = w32_win(msg->hwnd);
    if (!w || !target || w32_top(target) != w)
        return FALSE;
    w32_dispatch(msg);
    return TRUE;
}

static HANDLE WINAPI GetNextDlgTabItem(HANDLE dlg, HANDLE ctl, BOOL previous)
{
    struct w32_window *w = w32_win(dlg);
    return w ? next_tab(w, w32_win(ctl), previous) : NULL;
}

static const struct w32_export dialog_exports[] = {
    W32_FN(DialogBoxParamA),
    W32_FN(DialogBoxParamW),
    W32_FN(DialogBoxIndirectParamA),
    W32_AS("DialogBoxIndirectParamW", DialogBoxIndirectParamA),
    W32_FN(CreateDialogParamA),
    W32_FN(CreateDialogParamW),
    W32_FN(CreateDialogIndirectParamA),
    W32_AS("CreateDialogIndirectParamW", CreateDialogIndirectParamA),
    W32_FN(EndDialog),
    W32_FN(DefDlgProcA),
    W32_AS("DefDlgProcW", DefDlgProcA),
    W32_FN(MapDialogRect),
    W32_FN(GetDialogBaseUnits),
    W32_FN(IsDialogMessageA),
    W32_AS("IsDialogMessageW", IsDialogMessageA),
    W32_AS("IsDialogMessage", IsDialogMessageA),
    W32_FN(GetNextDlgTabItem),
};

W32_EXPORTS(dialog_module, "user32.dll", dialog_exports);
#endif
