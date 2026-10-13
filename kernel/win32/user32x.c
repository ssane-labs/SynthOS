#include <synth/gui.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "w32gui.h"

#define PRIMARY_MONITOR ((HANDLE)(uintptr_t)0x00010001u)
#define DUMMY_HANDLE ((HANDLE)(uintptr_t)0x00020002u)
#define HWND_MESSAGE ((HANDLE)(intptr_t)-3)

static void *user32_fn(const char *name)
{
    return w32_builtin_lookup("user32.dll", name);
}

static bool is_child(struct w32_window *w)
{
    return w->parent && (w->style & WS_CHILD);
}

static bool wide_equal_ci(const WCHAR *w, const char *s)
{
    for (;; w++, s++) {
        WCHAR a = *w, b = (uint8_t)*s;
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return false;
        if (!a)
            return true;
    }
}

static bool wide_equal(const WCHAR *w, const char *s)
{
    for (;; w++, s++) {
        if (*w != (uint8_t)*s)
            return false;
        if (!*w)
            return true;
    }
}

static bool class_matches(struct w32_window *w, const WCHAR *cls)
{
    if (!cls)
        return true;
    if ((uintptr_t)cls < 0x10000)
        return w->cls->atom == (uint16_t)(uintptr_t)cls;
    return wide_equal_ci(cls, w->cls->name);
}

static HANDLE WINAPI FindWindowExW(HANDLE parent_h, HANDLE after_h, const WCHAR *cls, const WCHAR *title)
{
    struct w32_window *parent = parent_h && parent_h != HWND_MESSAGE ? w32_win(parent_h) : NULL;
    bool seen = after_h == NULL;
    for (struct w32_window *w = w32_windows(); w; w = w->next_all) {
        if (w->destroyed)
            continue;
        if (parent ? w->parent != parent || !is_child(w)
                   : is_child(w) || (parent_h == HWND_MESSAGE) != w->message_only)
            continue;
        if (!seen) {
            seen = w == after_h;
            continue;
        }
        if (class_matches(w, cls) && (!title || wide_equal(title, w->text)))
            return w;
    }
    w32_set_error(2);
    return NULL;
}

static HANDLE WINAPI FindWindowExA(HANDLE parent, HANDLE after, const char *cls, const char *title)
{
    WCHAR *c = cls && (uintptr_t)cls >= 0x10000 ? w32_widen(cls) : (WCHAR *)cls;
    WCHAR *t = title ? w32_widen(title) : NULL;
    HANDLE h = FindWindowExW(parent, after, c, t);
    if (c != (WCHAR *)cls)
        w32_free(c);
    w32_free(t);
    return h;
}

static BOOL WINAPI EnumThreadWindows(DWORD tid, void *fn, LPARAM lp)
{
    typedef BOOL WINAPI (*enum_fn)(HANDLE, LPARAM);
    for (struct w32_window *w = w32_windows(); w; w = w->next_all)
        if (!w->destroyed && !is_child(w) && w->thread == tid && !((enum_fn)fn)(w, lp))
            break;
    return TRUE;
}

static HANDLE WINAPI GetAncestor(HANDLE h, UINT flags)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return NULL;
    if (flags == 1)
        return is_child(w) ? w->parent : NULL;
    while (is_child(w))
        w = w->parent;
    if (flags == 3)
        while (w->owner && w32_win(w->owner))
            w = w->owner;
    return w;
}

static BOOL WINAPI IsChild(HANDLE parent, HANDLE h)
{
    struct w32_window *w = w32_win(h);
    while (w && is_child(w)) {
        if (w->parent == parent)
            return TRUE;
        w = w->parent;
    }
    return FALSE;
}

static int WINAPI GetClassNameW(HANDLE h, WCHAR *out, int max)
{
    struct w32_window *w = w32_win(h);
    if (!w || max <= 0)
        return 0;
    int n = 0;
    for (; w->cls->name[n] && n < max - 1; n++)
        out[n] = (uint8_t)w->cls->name[n];
    out[n] = 0;
    return n;
}

static BOOL WINAPI GetClassInfoW(HANDLE inst, const WCHAR *name, uint32_t *out)
{
    UNUSED(inst);
    char *n = (uintptr_t)name < 0x10000 ? NULL : w32_narrow(name);
    struct w32_class *c = w32_find_class(n ? n : (const char *)name);
    w32_free(n);
    if (!c) {
        w32_set_error(1411);
        return FALSE;
    }
    uint32_t v[10] = {c->style, (uint32_t)c->proc, (uint32_t)c->class_extra, (uint32_t)c->window_extra,
                      (uint32_t)c->instance, (uint32_t)c->icon, (uint32_t)c->cursor, (uint32_t)c->background, 0,
                      (uint32_t)name};
    memcpy(out, v, sizeof(v));
    return TRUE;
}

static DWORD WINAPI GetWindowThreadProcessId(HANDLE h, DWORD *pid)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return 0;
    if (pid) {
        DWORD(WINAPI * self)(void) = (DWORD(WINAPI *)(void))w32_builtin_lookup("kernel32.dll", "GetCurrentProcessId");
        *pid = self ? self() : 0;
    }
    return w->thread;
}

struct prop {
    struct w32_window *window;
    uintptr_t atom;
    char name[48];
    HANDLE value;
    struct prop *next;
};

static W32_STATE struct prop *props;

static struct prop **prop_find(struct w32_window *w, const WCHAR *name)
{
    for (struct prop **pp = &props; *pp; pp = &(*pp)->next) {
        struct prop *p = *pp;
        if (p->window != w)
            continue;
        if ((uintptr_t)name < 0x10000 ? p->atom == (uintptr_t)name : !p->atom && wide_equal_ci(name, p->name))
            return pp;
    }
    return NULL;
}

static BOOL WINAPI SetPropW(HANDLE h, const WCHAR *name, HANDLE value)
{
    struct w32_window *w = w32_win(h);
    if (!w || !name)
        return FALSE;
    struct prop **pp = prop_find(w, name);
    if (pp) {
        (*pp)->value = value;
        return TRUE;
    }
    struct prop *p = w32_zalloc(sizeof(*p));
    if (!p)
        return FALSE;
    p->window = w;
    if ((uintptr_t)name < 0x10000)
        p->atom = (uintptr_t)name;
    else
        for (size_t i = 0; name[i] && i < sizeof(p->name) - 1; i++)
            p->name[i] = (char)name[i];
    p->value = value;
    p->next = props;
    props = p;
    return TRUE;
}

static HANDLE WINAPI GetPropW(HANDLE h, const WCHAR *name)
{
    struct w32_window *w = w32_win(h);
    struct prop **pp = w && name ? prop_find(w, name) : NULL;
    return pp ? (*pp)->value : NULL;
}

static HANDLE WINAPI RemovePropW(HANDLE h, const WCHAR *name)
{
    struct w32_window *w = w32_win(h);
    struct prop **pp = w && name ? prop_find(w, name) : NULL;
    if (!pp)
        return NULL;
    struct prop *p = *pp;
    HANDLE v = p->value;
    *pp = p->next;
    w32_free(p);
    return v;
}

static BOOL WINAPI GetWindowPlacement(HANDLE h, uint32_t *wp)
{
    struct w32_window *w = w32_win(h);
    if (!w)
        return FALSE;
    BOOL(WINAPI * get_rect)(HANDLE, RECT *) = (BOOL(WINAPI *)(HANDLE, RECT *))user32_fn("GetWindowRect");
    memset(wp + 1, 0, 40);
    wp[0] = 44;
    wp[2] = !w->visible ? 0 : (w->wm && (w->wm->flags & WIN_MINIMIZED)) ? 2 : (w->wm && (w->wm->flags & WIN_MAXIMIZED)) ? 3 : 1;
    if (get_rect)
        get_rect(h, (RECT *)(wp + 7));
    return TRUE;
}

static HANDLE WINAPI MonitorFromWindow(HANDLE h, DWORD flags)
{
    UNUSED(h);
    UNUSED(flags);
    return PRIMARY_MONITOR;
}

static HANDLE WINAPI MonitorFromPoint(LONG x, LONG y, DWORD flags)
{
    UNUSED(x);
    UNUSED(y);
    UNUSED(flags);
    return PRIMARY_MONITOR;
}

static void screen_size(int *w, int *h)
{
    int(WINAPI * metrics)(int) = (int(WINAPI *)(int))user32_fn("GetSystemMetrics");
    *w = metrics ? metrics(0) : 1024;
    *h = metrics ? metrics(1) : 768;
}

static BOOL monitor_info(HANDLE mon, uint32_t *info, bool wide)
{
    if (mon != PRIMARY_MONITOR || !info || info[0] < 40) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    int w, h;
    screen_size(&w, &h);
    int top = gui_active() ? MENUBAR_HEIGHT : 0;
    uint32_t v[9] = {0, 0, (uint32_t)w, (uint32_t)h, 0, (uint32_t)top, (uint32_t)w, (uint32_t)h, 1};
    memcpy(info + 1, v, sizeof(v));
    static const char device[] = "\\\\.\\DISPLAY1";
    if (wide && info[0] >= 40 + 64) {
        WCHAR *d = (WCHAR *)(info + 10);
        for (size_t i = 0; i < sizeof(device); i++)
            d[i] = (uint8_t)device[i];
    } else if (!wide && info[0] >= 40 + 32) {
        memcpy(info + 10, device, sizeof(device));
    }
    return TRUE;
}

static BOOL WINAPI GetMonitorInfoW(HANDLE mon, uint32_t *info)
{
    return monitor_info(mon, info, true);
}

static BOOL WINAPI GetMonitorInfoA(HANDLE mon, uint32_t *info)
{
    return monitor_info(mon, info, false);
}

static BOOL WINAPI EnumDisplayMonitors(HANDLE dc, const RECT *clip, void *fn, LPARAM lp)
{
    UNUSED(dc);
    UNUSED(clip);
    int w, h;
    screen_size(&w, &h);
    RECT r = {0, 0, w, h};
    typedef BOOL WINAPI (*enum_fn)(HANDLE, HANDLE, RECT *, LPARAM);
    ((enum_fn)fn)(PRIMARY_MONITOR, NULL, &r, lp);
    return TRUE;
}

static void put_wide(WCHAR *dst, const char *src, size_t cap)
{
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++)
        dst[i] = (uint8_t)src[i];
    dst[i] = 0;
}

static BOOL WINAPI EnumDisplayDevicesW(const WCHAR *device, DWORD index, uint8_t *dd, DWORD flags)
{
    UNUSED(flags);
    if (index != 0 || !dd) {
        return FALSE;
    }
    DWORD cb;
    memcpy(&cb, dd, 4);
    memset(dd + 4, 0, cb > 4 ? cb - 4 : 0);
    if (cb < 840)
        return FALSE;
    put_wide((WCHAR *)(dd + 4), device ? "Monitor" : "\\\\.\\DISPLAY1", 32);
    put_wide((WCHAR *)(dd + 68), device ? "Generic Monitor" : "SynthOS Display Adapter", 128);
    DWORD state = 1 | 4;
    memcpy(dd + 324, &state, 4);
    put_wide((WCHAR *)(dd + 328), "PCI\\VEN_1234&DEV_1111", 128);
    return TRUE;
}

static BOOL WINAPI EnumDisplaySettingsExW(const WCHAR *name, DWORD mode, uint8_t *dm, DWORD flags)
{
    UNUSED(name);
    UNUSED(flags);
    if (mode != 0xffffffffu && mode != 0xfffffffeu && mode != 0)
        return FALSE;
    int w, h;
    screen_size(&w, &h);
    uint16_t size;
    memcpy(&size, dm + 68, 2);
    memset(dm, 0, MAX(size, 188));
    size = 220;
    memcpy(dm + 68, &size, 2);
    uint32_t fields = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u, bpp = 32, freq = 60;
    memcpy(dm + 72, &fields, 4);
    memcpy(dm + 168, &bpp, 4);
    memcpy(dm + 172, &w, 4);
    memcpy(dm + 176, &h, 4);
    memcpy(dm + 184, &freq, 4);
    return TRUE;
}

static BOOL WINAPI EnumDisplaySettingsW(const WCHAR *name, DWORD mode, uint8_t *dm)
{
    return EnumDisplaySettingsExW(name, mode, dm, 0);
}

static LONG WINAPI display_config_unsupported(void)
{
    return 50;
}

static LONG WINAPI GetDisplayConfigBufferSizes(UINT flags, UINT *paths, UINT *modes)
{
    UNUSED(flags);
    *paths = *modes = 0;
    return display_config_unsupported();
}

#define ATOM_MAX 128
static W32_STATE char atom_names[ATOM_MAX][48];

static UINT register_name(const char *name)
{
    for (int i = 0; i < ATOM_MAX; i++)
        if (atom_names[i][0] && !str_casecmp(atom_names[i], name))
            return 0xc000u + (UINT)i;
    for (int i = 0; i < ATOM_MAX; i++)
        if (!atom_names[i][0]) {
            str_copy(atom_names[i], name, sizeof(atom_names[i]));
            return 0xc000u + (UINT)i;
        }
    return 0;
}

static UINT WINAPI RegisterWindowMessageA(const char *name)
{
    return name ? register_name(name) : 0;
}

static UINT WINAPI RegisterWindowMessageW(const WCHAR *name)
{
    char *n = name ? w32_narrow(name) : NULL;
    UINT id = n ? register_name(n) : 0;
    w32_free(n);
    return id;
}

static int WINAPI GetClipboardFormatNameW(UINT format, WCHAR *out, int max)
{
    if (format < 0xc000u || format >= 0xc000u + ATOM_MAX || !atom_names[format - 0xc000u][0] || max <= 0)
        return 0;
    put_wide(out, atom_names[format - 0xc000u], (size_t)max);
    return (int)w32_wcslen(out);
}

static HANDLE WINAPI SetClipboardData(UINT format, HANDLE data)
{
    UNUSED(format);
    return data;
}

static HANDLE WINAPI GetKeyboardLayout(DWORD thread)
{
    UNUSED(thread);
    return (HANDLE)(uintptr_t)0x04090409u;
}

static int WINAPI GetKeyboardLayoutList(int n, HANDLE *list)
{
    if (n > 0 && list)
        list[0] = GetKeyboardLayout(0);
    return 1;
}

static BOOL WINAPI GetKeyboardLayoutNameW(WCHAR *out)
{
    put_wide(out, "00000409", 9);
    return TRUE;
}

static BOOL WINAPI GetKeyboardState(uint8_t *keys)
{
    memset(keys, 0, 256);
    return TRUE;
}

static UINT WINAPI MapVirtualKeyW(UINT code, UINT type)
{
    if (type == 2) {
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z'))
            return code;
        switch (code) {
        case 0x20: return ' ';
        case 0xbc: return ',';
        case 0xbe: return '.';
        case 0xbd: return '-';
        case 0xbb: return '=';
        default: return 0;
        }
    }
    return 0;
}

static UINT WINAPI MapVirtualKeyExW(UINT code, UINT type, HANDLE layout)
{
    UNUSED(layout);
    return MapVirtualKeyW(code, type);
}

static BOOL WINAPI GetLastInputInfo(uint32_t *info)
{
    info[1] = (uint32_t)hal_ticks_ms();
    return TRUE;
}

static UINT WINAPI GetCaretBlinkTime(void)
{
    return 530;
}

static UINT WINAPI GetDoubleClickTime(void)
{
    return 500;
}

static LONG WINAPI GetMessageTime(void)
{
    return (LONG)hal_ticks_ms();
}

static BOOL WINAPI GetCursorInfo(uint32_t *info)
{
    BOOL(WINAPI * pos)(POINT *) = (BOOL(WINAPI *)(POINT *))user32_fn("GetCursorPos");
    info[1] = 1;
    info[2] = 0;
    if (pos)
        pos((POINT *)(info + 3));
    return TRUE;
}

static UINT WINAPI SendInput(UINT count, void *inputs, int size)
{
    UNUSED(inputs);
    UNUSED(size);
    return count;
}

static UINT WINAPI GetRawInputDeviceList(void *list, UINT *count, UINT size)
{
    UNUSED(size);
    if (!list) {
        *count = 0;
        return 0;
    }
    *count = 0;
    return 0;
}

static UINT WINAPI raw_input_fail4(HANDLE a, UINT b, void *c, UINT *d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return 0xffffffffu;
}

static UINT WINAPI raw_input_fail5(HANDLE a, UINT b, void *c, UINT *d, UINT e)
{
    UNUSED(e);
    return raw_input_fail4(a, b, c, d);
}

static BOOL WINAPI GetPointerDevices(uint32_t *count, void *devices)
{
    UNUSED(devices);
    *count = 0;
    return TRUE;
}

static HANDLE WINAPI dummy_handle1(uint32_t a)
{
    UNUSED(a);
    return DUMMY_HANDLE;
}

static HANDLE WINAPI dummy_handle2(uint32_t a, uint32_t b)
{
    UNUSED(b);
    return dummy_handle1(a);
}

static HANDLE WINAPI dummy_handle3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(c);
    return dummy_handle2(a, b);
}

static HANDLE WINAPI dummy_handle4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    UNUSED(d);
    return dummy_handle3(a, b, c);
}

static HANDLE WINAPI dummy_handle7(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{
    UNUSED(e);
    UNUSED(f);
    UNUSED(g);
    return dummy_handle4(a, b, c, d);
}

static HANDLE WINAPI dummy_handle0(void)
{
    return DUMMY_HANDLE;
}

static BOOL WINAPI GetUserObjectInformationW(HANDLE obj, int index, void *info, DWORD len, DWORD *needed)
{
    UNUSED(obj);
    if (index == 1) {
        if (needed)
            *needed = 12;
        if (len < 12) {
            w32_set_error(122);
            return FALSE;
        }
        uint32_t v[3] = {0, 0, 1};
        memcpy(info, v, sizeof(v));
        return TRUE;
    }
    if (index == 2) {
        const char *name = obj == DUMMY_HANDLE ? "Default" : "WinSta0";
        DWORD n = (DWORD)(strlen(name) + 1) * 2;
        if (needed)
            *needed = n;
        if (len < n) {
            w32_set_error(122);
            return FALSE;
        }
        put_wide(info, name, n / 2);
        return TRUE;
    }
    w32_set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static UINT WINAPI DdeInitializeW(DWORD *inst, void *callback, DWORD cmd, DWORD reserved)
{
    UNUSED(callback);
    UNUSED(cmd);
    UNUSED(reserved);
    *inst = 0;
    return 0x400f;
}

static UINT WINAPI DdeGetLastError(DWORD inst)
{
    UNUSED(inst);
    return 0x400f;
}

static BOOL WINAPI EqualRect(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

static BOOL WINAPI IsRectEmpty(const RECT *r)
{
    return !r || r->right <= r->left || r->bottom <= r->top;
}

static BOOL WINAPI InflateRect(RECT *r, int dx, int dy)
{
    r->left -= dx;
    r->right += dx;
    r->top -= dy;
    r->bottom += dy;
    return TRUE;
}

static BOOL WINAPI OffsetRect(RECT *r, int dx, int dy)
{
    r->left += dx;
    r->right += dx;
    r->top += dy;
    r->bottom += dy;
    return TRUE;
}

static BOOL WINAPI SetRect(RECT *r, int left, int top, int right, int bottom)
{
    *r = (RECT){left, top, right, bottom};
    return TRUE;
}

static BOOL WINAPI SetRectEmpty(RECT *r)
{
    *r = (RECT){0, 0, 0, 0};
    return TRUE;
}

static BOOL WINAPI CopyRect(RECT *dst, const RECT *src)
{
    *dst = *src;
    return TRUE;
}

static BOOL WINAPI IntersectRect(RECT *out, const RECT *a, const RECT *b)
{
    RECT r = {MAX(a->left, b->left), MAX(a->top, b->top), MIN(a->right, b->right), MIN(a->bottom, b->bottom)};
    if (IsRectEmpty(&r)) {
        *out = (RECT){0, 0, 0, 0};
        return FALSE;
    }
    *out = r;
    return TRUE;
}

static BOOL WINAPI UnionRect(RECT *out, const RECT *a, const RECT *b)
{
    if (IsRectEmpty(a)) {
        *out = *b;
    } else if (IsRectEmpty(b)) {
        *out = *a;
    } else {
        *out = (RECT){MIN(a->left, b->left), MIN(a->top, b->top), MAX(a->right, b->right), MAX(a->bottom, b->bottom)};
    }
    return !IsRectEmpty(out);
}

static BOOL WINAPI PtInRect(const RECT *r, LONG x, LONG y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

static BOOL WINAPI CharToOemBuffW(const WCHAR *src, char *dst, DWORD n)
{
    for (DWORD i = 0; i < n; i++)
        dst[i] = src[i] < 0x80 ? (char)src[i] : '?';
    return TRUE;
}

static BOOL WINAPI OemToCharBuffA(const char *src, char *dst, DWORD n)
{
    if (src != dst)
        memmove(dst, src, n);
    return TRUE;
}

static DWORD WINAPI MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, DWORD ms, DWORD mask, DWORD flags)
{
    DWORD(WINAPI * wait)(DWORD, const HANDLE *, BOOL, DWORD, DWORD) =
        (DWORD(WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD, DWORD))user32_fn("MsgWaitForMultipleObjects");
    return wait ? wait(count, handles, flags & 1, ms, mask) : 0xffffffffu;
}

static LRESULT WINAPI SendMessageTimeoutW(HANDLE h, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT timeout,
                                          uintptr_t *result)
{
    UNUSED(flags);
    UNUSED(timeout);
    LRESULT(WINAPI * send)(HANDLE, UINT, WPARAM, LPARAM) =
        (LRESULT(WINAPI *)(HANDLE, UINT, WPARAM, LPARAM))user32_fn("SendMessageW");
    if (!send || !w32_win(h))
        return 0;
    LRESULT r = send(h, msg, wp, lp);
    if (result)
        *result = (uintptr_t)r;
    return 1;
}

static BOOL WINAPI InsertMenuItemW(HANDLE menu, UINT item, BOOL by_position, const uint32_t *info)
{
    UNUSED(item);
    UNUSED(by_position);
    uint32_t mask = info[1], type = info[2], id = info[5];
    char *text = (mask & 0x40) || ((mask & 0x10) && !(type & 0x800)) ? w32_narrow((const WCHAR *)info[9]) : NULL;
    bool ok = w32_menu_append(menu, (type & 0x800) ? 0x800 : 0, id, text ? text : "");
    w32_free(text);
    return ok;
}

static HANDLE WINAPI CreateIconIndirect(void *info)
{
    UNUSED(info);
    return w32_zalloc(16);
}

static void WINAPI no_op4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
}

static UINT WINAPI GetDpi(void)
{
    return 96;
}

static UINT WINAPI GetDpiForWindow(HANDLE h)
{
    UNUSED(h);
    return 96;
}

static int WINAPI GetSystemMetricsForDpi(int index, UINT dpi)
{
    UNUSED(dpi);
    int(WINAPI * metrics)(int) = (int(WINAPI *)(int))user32_fn("GetSystemMetrics");
    return metrics ? metrics(index) : 0;
}

static BOOL WINAPI AdjustWindowRectExForDpi(RECT *r, DWORD style, BOOL menu, DWORD ex, UINT dpi)
{
    UNUSED(dpi);
    BOOL(WINAPI * adjust)(RECT *, DWORD, BOOL, DWORD) =
        (BOOL(WINAPI *)(RECT *, DWORD, BOOL, DWORD))user32_fn("AdjustWindowRectEx");
    return adjust && adjust(r, style, menu, ex);
}

static HANDLE WINAPI GetDpiAwarenessContext(void)
{
    return (HANDLE)(intptr_t)-4;
}

static int WINAPI GetAwarenessFromDpiAwarenessContext(HANDLE context)
{
    UNUSED(context);
    return 2;
}

static BOOL WINAPI AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b)
{
    return a == b;
}

static const struct w32_export user32x_exports[] = {
    W32_AS("SetProcessDpiAwarenessContext", w32_one1),
    W32_AS("IsValidDpiAwarenessContext", w32_one1),
    W32_FN(AreDpiAwarenessContextsEqual),
    W32_FN(GetAwarenessFromDpiAwarenessContext),
    W32_AS("GetThreadDpiAwarenessContext", GetDpiAwarenessContext),
    W32_AS("SetThreadDpiAwarenessContext", GetDpiAwarenessContext),
    W32_AS("GetWindowDpiAwarenessContext", GetDpiAwarenessContext),
    W32_AS("GetDpiForSystem", GetDpi),
    W32_FN(GetDpiForWindow),
    W32_FN(GetSystemMetricsForDpi),
    W32_FN(AdjustWindowRectExForDpi),
    W32_AS("EnableNonClientDpiScaling", w32_one1),
    W32_FN(FindWindowExW),
    W32_FN(FindWindowExA),
    W32_FN(EnumThreadWindows),
    W32_FN(GetAncestor),
    W32_FN(IsChild),
    W32_FN(GetClassNameW),
    W32_FN(GetClassInfoW),
    W32_FN(GetWindowThreadProcessId),
    W32_FN(SetPropW),
    W32_FN(GetPropW),
    W32_FN(RemovePropW),
    W32_FN(GetWindowPlacement),
    W32_AS("SetWindowPlacement", w32_one2),
    W32_AS("GetWindowRgn", w32_zero2),
    W32_AS("SetWindowRgn", w32_one3),
    W32_AS("GetWindowDisplayAffinity", w32_one2),
    W32_AS("SetWindowDisplayAffinity", w32_one2),
    W32_AS("GetLayeredWindowAttributes", w32_zero4),
    W32_AS("UpdateLayeredWindow", w32_one9),
    W32_AS("PrintWindow", w32_zero3),
    W32_AS("WindowFromDC", w32_zero1),
    W32_AS("WindowFromPoint", w32_zero2),
    W32_AS("FlashWindowEx", w32_zero1),
    W32_AS("GetSystemMenu", w32_zero2),
    W32_AS("IsWindowArranged", w32_zero1),
    W32_AS("GetMenuInfo", w32_zero2),
    W32_AS("SetMenuInfo", w32_one2),
    W32_AS("GetMenuItemInfoW", w32_zero4),
    W32_AS("SetMenuItemInfoW", w32_one4),
    W32_FN(InsertMenuItemW),
    W32_AS("SetMenuDefaultItem", w32_one3),
    W32_AS("TrackPopupMenu", w32_zero7),
    W32_AS("DrawFrameControl", w32_one4),
    W32_AS("DrawIconEx", w32_one9),
    W32_FN(CreateIconIndirect),
    W32_AS("GetIconInfo", w32_zero2),
    W32_FN(MonitorFromWindow),
    W32_FN(MonitorFromPoint),
    W32_AS("MonitorFromRect", MonitorFromWindow),
    W32_FN(GetMonitorInfoW),
    W32_FN(GetMonitorInfoA),
    W32_FN(EnumDisplayMonitors),
    W32_FN(EnumDisplayDevicesW),
    W32_FN(EnumDisplaySettingsW),
    W32_FN(EnumDisplaySettingsExW),
    W32_FN(GetDisplayConfigBufferSizes),
    W32_AS("QueryDisplayConfig", w32_fail6),
    W32_AS("DisplayConfigGetDeviceInfo", display_config_unsupported),
    W32_AS("OpenClipboard", w32_one1),
    W32_AS("CloseClipboard", w32_one0),
    W32_AS("EmptyClipboard", w32_one0),
    W32_AS("GetClipboardData", w32_zero1),
    W32_FN(SetClipboardData),
    W32_AS("IsClipboardFormatAvailable", w32_zero1),
    W32_AS("EnumClipboardFormats", w32_zero1),
    W32_FN(GetClipboardFormatNameW),
    W32_AS("RegisterClipboardFormatW", RegisterWindowMessageW),
    W32_AS("RegisterClipboardFormatA", RegisterWindowMessageA),
    W32_AS("GetClipboardSequenceNumber", w32_one0),
    W32_AS("AddClipboardFormatListener", w32_one1),
    W32_AS("RemoveClipboardFormatListener", w32_one1),
    W32_FN(RegisterWindowMessageW),
    W32_FN(RegisterWindowMessageA),
    W32_FN(GetKeyboardLayoutList),
    W32_FN(GetKeyboardLayoutNameW),
    W32_FN(GetKeyboardState),
    W32_AS("SetKeyboardState", w32_one1),
    W32_FN(MapVirtualKeyW),
    W32_FN(MapVirtualKeyExW),
    W32_AS("ToUnicodeEx", w32_zero7),
    W32_FN(GetLastInputInfo),
    W32_FN(GetCaretBlinkTime),
    W32_FN(GetDoubleClickTime),
    W32_AS("GetMessageExtraInfo", w32_zero0),
    W32_AS("GetMessagePos", w32_zero0),
    W32_FN(GetMessageTime),
    W32_AS("GetQueueStatus", w32_zero1),
    W32_FN(GetCursorInfo),
    W32_AS("ClipCursor", w32_one1),
    W32_FN(SendInput),
    W32_AS("RegisterHotKey", w32_one4),
    W32_AS("UnregisterHotKey", w32_one2),
    W32_AS("TrackMouseEvent", w32_one1),
    W32_AS("RegisterRawInputDevices", w32_one3),
    W32_AS("GetRawInputData", raw_input_fail5),
    W32_AS("GetRawInputDeviceInfoW", raw_input_fail4),
    W32_FN(GetRawInputDeviceList),
    W32_AS("DefRawInputProc", w32_zero3),
    W32_AS("GetPointerDevice", w32_zero2),
    W32_FN(GetPointerDevices),
    W32_AS("GetPointerPenInfo", w32_zero2),
    W32_AS("GetPointerType", w32_zero2),
    W32_AS("RegisterPointerDeviceNotifications", w32_one2),
    W32_AS("CloseTouchInputHandle", w32_one1),
    W32_AS("SetWindowsHookExW", dummy_handle4),
    W32_AS("SetWindowsHookExA", dummy_handle4),
    W32_AS("UnhookWindowsHookEx", w32_one1),
    W32_AS("CallNextHookEx", w32_zero4),
    W32_AS("SetWinEventHook", dummy_handle7),
    W32_AS("UnhookWinEvent", w32_one1),
    W32_AS("NotifyWinEvent", no_op4),
    W32_AS("GetProcessWindowStation", dummy_handle0),
    W32_AS("GetThreadDesktop", dummy_handle1),
    W32_AS("OpenInputDesktop", dummy_handle3),
    W32_FN(GetUserObjectInformationW),
    W32_AS("CreateDesktopW", w32_fail6),
    W32_AS("CloseDesktop", w32_one1),
    W32_AS("SetThreadDesktop", w32_one1),
    W32_AS("CreateWindowStationW", w32_fail4),
    W32_AS("CloseWindowStation", w32_one1),
    W32_AS("SetProcessWindowStation", w32_one1),
    W32_AS("GetShellWindow", w32_zero0),
    W32_AS("AllowSetForegroundWindow", w32_one1),
    W32_FN(DdeInitializeW),
    W32_AS("DdeConnect", w32_zero4),
    W32_AS("DdeDisconnect", w32_zero1),
    W32_AS("DdeCreateStringHandleW", w32_zero3),
    W32_AS("DdeFreeStringHandle", w32_zero2),
    W32_AS("DdeClientTransaction", w32_zero8),
    W32_FN(DdeGetLastError),
    W32_AS("DdeUninitialize", w32_one1),
    W32_AS("RegisterDeviceNotificationW", dummy_handle3),
    W32_AS("UnregisterDeviceNotification", w32_one1),
    W32_AS("RegisterPowerSettingNotification", dummy_handle3),
    W32_AS("UnregisterPowerSettingNotification", w32_one1),
    W32_AS("RegisterSuspendResumeNotification", dummy_handle2),
    W32_AS("UnregisterSuspendResumeNotification", w32_one1),
    W32_FN(EqualRect),
    W32_FN(IsRectEmpty),
    W32_FN(InflateRect),
    W32_FN(OffsetRect),
    W32_FN(SetRect),
    W32_FN(SetRectEmpty),
    W32_FN(CopyRect),
    W32_FN(IntersectRect),
    W32_FN(UnionRect),
    W32_FN(PtInRect),
    W32_FN(CharToOemBuffW),
    W32_FN(OemToCharBuffA),
    W32_AS("GetGuiResources", w32_zero2),
    W32_FN(MsgWaitForMultipleObjectsEx),
    W32_FN(SendMessageTimeoutW),
    W32_AS("SetProcessDPIAware", w32_one0),
};

W32_EXPORTS(user32x_module, "user32.dll", user32x_exports);
