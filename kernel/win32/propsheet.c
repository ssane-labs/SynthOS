#include <synth/config.h>
#if CONFIG_GUI
#include "dlgtmpl.h"

/* comctl32 property sheets (tabbed and wizard), TaskDialog and drag lists for 32-bit programs; the sheet is a
   built-in dialog with a tab control, each page a child dialog from the program's template */
enum {
    PSH_PROPSHEETPAGE = 8,
    PSH_WIZARD = 0x20,
    PSH_USEPSTARTPAGE = 0x40,
    PSH_NOAPPLYNOW = 0x80,
    PSH_USECALLBACK = 0x100,
    PSH_MODELESS = 0x400,
    PSH_AEROWIZARD = 0x4000,
    PSH_WIZARD97 = 0x1000000,
    PSP_DLGINDIRECT = 1,
    PSP_USETITLE = 8,
    PSP_USECALLBACK = 0x80,
    PSN_SETACTIVE = -200,
    PSN_KILLACTIVE = -201,
    PSN_APPLY = -202,
    PSN_RESET = -203,
    PSN_WIZBACK = -206,
    PSN_WIZNEXT = -207,
    PSN_WIZFINISH = -208,
    TCN_SELCHANGE = -551,
    PSM_SETCURSEL = 0x465,
    PSM_REMOVEPAGE = 0x466,
    PSM_CHANGED = 0x468,
    PSM_CANCELTOCLOSE = 0x46b,
    PSM_QUERYSIBLINGS = 0x46c,
    PSM_UNCHANGED = 0x46d,
    PSM_APPLY = 0x46e,
    PSM_SETTITLEA = 0x46f,
    PSM_SETWIZBUTTONS = 0x470,
    PSM_PRESSBUTTON = 0x471,
    PSM_SETFINISHTEXTA = 0x473,
    PSM_GETTABCONTROL = 0x474,
    PSM_ISDIALOGMESSAGE = 0x475,
    PSM_GETCURRENTPAGEHWND = 0x476,
    PSM_SETTITLEW = 0x478,
    PSM_SETFINISHTEXTW = 0x479,
    PSM_GETRESULT = 0x487,
    ID_APPLY = 0x3021,
    ID_BACK = 0x3023,
    ID_NEXT = 0x3024,
    ID_FINISH = 0x3025,
    ID_TAB = 0x3020,
    MAX_PAGES = 32,
};

struct page {
    uint8_t *psp;
    const uint8_t *tmpl;
    HANDLE hwnd;
    bool owned;
};

struct sheet {
    bool wide, wizard;
    uint32_t flags;
    HANDLE dlg, tab;
    int count, cur, result;
    uint32_t wizbuttons;
    void *callback;
    const void *caption;
    struct page pages[MAX_PAGES];
};

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static const uint8_t *page_template(const uint8_t *psp, bool wide)
{
    uint32_t flags = rd32(psp + 4), res = rd32(psp + 12);
    if (flags & PSP_DLGINDIRECT)
        return (const uint8_t *)(uintptr_t)res;
    HANDLE inst = (HANDLE)(uintptr_t)rd32(psp + 8);
    typedef HANDLE(WINAPI * find_fn)(HANDLE, const void *, const void *);
    HANDLE r = DLG_CALL("kernel32.dll", wide ? "FindResourceW" : "FindResourceA", find_fn)(
        inst, (const void *)(uintptr_t)res, (const void *)5);
    HANDLE g = r ? DLG_CALL("kernel32.dll", "LoadResource", HANDLE(WINAPI *)(HANDLE, HANDLE))(inst, r) : NULL;
    return g ? DLG_CALL("kernel32.dll", "LockResource", const uint8_t *(WINAPI *)(HANDLE))(g) : NULL;
}

static bool template_ex(const uint8_t *t)
{
    return rd16(t) == 1 && rd16(t + 2) == 0xffff;
}

static void template_size(const uint8_t *t, int *cx, int *cy)
{
    int off = template_ex(t) ? 22 : 14;
    *cx = (int16_t)rd16(t + off);
    *cy = (int16_t)rd16(t + off + 2);
}

static const uint8_t *skip_sz_or_ord(const uint8_t *p)
{
    if (rd16(p) == 0xffff)
        return p + 4;
    while (rd16(p))
        p += 2;
    return p + 2;
}

static void template_title(const uint8_t *t, char *out, size_t cap)
{
    const uint8_t *p = skip_sz_or_ord(skip_sz_or_ord(t + (template_ex(t) ? 26 : 18)));
    size_t n = 0;
    for (; rd16(p) && n + 1 < cap; p += 2)
        out[n++] = rd16(p) < 0x80 ? (char)rd16(p) : '?';
    out[n] = 0;
}

static void text_of(const void *text, HANDLE inst, bool wide, char *out, size_t cap)
{
    out[0] = 0;
    if (!text)
        return;
    if ((uintptr_t)text < 0x10000) {
        WCHAR buf[128];
        int n = DLG_CALL("user32.dll", "LoadStringW", int(WINAPI *)(HANDLE, UINT, WCHAR *, int))(
            inst, (UINT)(uintptr_t)text, buf, 128);
        size_t i = 0;
        for (; i < (size_t)n && i + 1 < cap; i++)
            out[i] = buf[i] < 0x80 ? (char)buf[i] : '?';
        out[i] = 0;
        return;
    }
    if (!wide) {
        str_copy(out, text, cap);
        return;
    }
    const WCHAR *w = text;
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++)
        out[i] = w[i] < 0x80 ? (char)w[i] : '?';
    out[i] = 0;
}

static LRESULT notify(struct sheet *s, int i, int code)
{
    struct page *p = &s->pages[i];
    if (!p->hwnd)
        return 0;
    uint32_t n[4] = {(uint32_t)(uintptr_t)s->dlg, 0, (uint32_t)code, 0};
    struct w32_window *w = w32_win(p->hwnd);
    return w ? w32_send(w, WM_NOTIFY, 0, (LPARAM)(uintptr_t)n) : 0;
}

static RECT page_area(struct sheet *s)
{
    RECT r;
    DLG_CALL("user32.dll", "GetWindowRect", BOOL(WINAPI *)(HANDLE, RECT *))(s->tab, &r);
    POINT a = {r.left, r.top}, b = {r.right, r.bottom};
    BOOL(WINAPI * to_client)(HANDLE, POINT *) = DLG_CALL("user32.dll", "ScreenToClient", BOOL(WINAPI *)(HANDLE, POINT *));
    to_client(s->dlg, &a);
    to_client(s->dlg, &b);
    r = (RECT){a.x, a.y, b.x, b.y};
    if (!s->wizard) {
        RECT in = {0, 0, b.x - a.x, b.y - a.y};
        struct w32_window *t = w32_win(s->tab);
        if (t)
            w32_send(t, 0x1328, FALSE, (LPARAM)(uintptr_t)&in);
        r = (RECT){a.x + in.left, a.y + in.top, a.x + in.right, a.y + in.bottom};
    }
    return r;
}

static void call_page_callback(struct page *p, UINT msg)
{
    typedef UINT(WINAPI * cb_fn)(HANDLE, UINT, void *);
    if (!(rd32(p->psp + 4) & PSP_USECALLBACK) || !rd32(p->psp + 32))
        return;
    W32_CALL(cb_fn, (cb_fn)(uintptr_t)rd32(p->psp + 32), NULL, msg, p->psp);
}

static bool create_page(struct sheet *s, int i)
{
    struct page *p = &s->pages[i];
    if (p->hwnd)
        return true;
    if (!p->tmpl)
        return false;
    typedef HANDLE(WINAPI * create_fn)(HANDLE, const void *, HANDLE, void *, LPARAM);
    p->hwnd = DLG_CALL("user32.dll", "CreateDialogIndirectParamA", create_fn)(
        (HANDLE)(uintptr_t)rd32(p->psp + 8), p->tmpl, s->dlg, (void *)(uintptr_t)rd32(p->psp + 24),
        (LPARAM)(uintptr_t)p->psp);
    if (!p->hwnd)
        return false;
    struct w32_window *w = w32_win(p->hwnd);
    if (w && !(w->style & WS_CHILD)) {
        w->style = (w->style & ~(uint32_t)(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME)) | WS_CHILD;
        DLG_CALL("user32.dll", "SetParent", HANDLE(WINAPI *)(HANDLE, HANDLE))(p->hwnd, s->dlg);
    }
    RECT r = page_area(s);
    DLG_CALL("user32.dll", "SetWindowPos", BOOL(WINAPI *)(HANDLE, HANDLE, int, int, int, int, UINT))(
        p->hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, 0x14);
    return true;
}

static void show(HANDLE h, bool on)
{
    if (h)
        DLG_CALL("user32.dll", "ShowWindow", BOOL(WINAPI *)(HANDLE, int))(h, on ? 5 : 0);
}

static void enable(HANDLE dlg, int id, bool on)
{
    HANDLE c = DLG_CALL("user32.dll", "GetDlgItem", HANDLE(WINAPI *)(HANDLE, int))(dlg, id);
    if (c)
        DLG_CALL("user32.dll", "EnableWindow", BOOL(WINAPI *)(HANDLE, BOOL))(c, on);
}

static void wizard_buttons(struct sheet *s)
{
    enable(s->dlg, ID_BACK, (s->wizbuttons & 1) && s->cur > 0);
    bool last = (s->wizbuttons & 4) || s->cur == s->count - 1;
    HANDLE next = DLG_CALL("user32.dll", "GetDlgItem", HANDLE(WINAPI *)(HANDLE, int))(s->dlg, ID_NEXT);
    HANDLE finish = DLG_CALL("user32.dll", "GetDlgItem", HANDLE(WINAPI *)(HANDLE, int))(s->dlg, ID_FINISH);
    show(next, !last);
    show(finish, last);
    enable(s->dlg, ID_NEXT, (s->wizbuttons & 2) != 0);
    enable(s->dlg, ID_FINISH, !(s->wizbuttons & 8));
}

static bool select_page(struct sheet *s, int i)
{
    if (i < 0 || i >= s->count)
        return false;
    if (s->cur >= 0 && s->cur != i && notify(s, s->cur, PSN_KILLACTIVE))
        return false;
    if (!create_page(s, i))
        return false;
    if (s->cur >= 0 && s->cur != i)
        show(s->pages[s->cur].hwnd, false);
    s->cur = i;
    if (s->tab) {
        struct w32_window *t = w32_win(s->tab);
        if (t)
            w32_send(t, 0x130c, (WPARAM)i, 0);
    }
    if (s->wizard)
        s->wizbuttons = 2 | (i > 0);
    LRESULT r = notify(s, i, PSN_SETACTIVE);
    if (r == -1 && s->wizard)
        return false;
    show(s->pages[i].hwnd, true);
    if (s->wizard)
        wizard_buttons(s);
    return true;
}

static void end_sheet(struct sheet *s, int result)
{
    s->result = result;
    if (s->flags & PSH_MODELESS)
        DLG_CALL("user32.dll", "DestroyWindow", BOOL(WINAPI *)(HANDLE))(s->dlg);
    else
        DLG_CALL("user32.dll", "EndDialog", BOOL(WINAPI *)(HANDLE, intptr_t))(s->dlg, result);
}

static bool apply_all(struct sheet *s)
{
    if (s->cur >= 0 && notify(s, s->cur, PSN_KILLACTIVE))
        return false;
    for (int i = 0; i < s->count; i++)
        if (notify(s, i, PSN_APPLY) == 2)
            return false;
    enable(s->dlg, ID_APPLY, false);
    return true;
}

static void wizard_step(struct sheet *s, int code)
{
    LRESULT r = notify(s, s->cur, code);
    if (r == -1)
        return;
    if (code == PSN_WIZFINISH) {
        if (!r)
            end_sheet(s, 1);
        return;
    }
    int step = code == PSN_WIZNEXT ? 1 : -1;
    for (int i = s->cur + step; i >= 0 && i < s->count; i += step)
        if (select_page(s, i))
            return;
}

static void init_sheet(HANDLE dlg, struct sheet *s)
{
    s->dlg = dlg;
    s->tab = DLG_CALL("user32.dll", "GetDlgItem", HANDLE(WINAPI *)(HANDLE, int))(dlg, ID_TAB);
    char title[160];
    text_of(s->caption, NULL, s->wide, title, sizeof(title));
    if (title[0])
        DLG_CALL("user32.dll", "SetWindowTextA", BOOL(WINAPI *)(HANDLE, const char *))(dlg, title);
    struct w32_window *t = w32_win(s->tab);
    for (int i = 0; t && !s->wizard && i < s->count; i++) {
        char name[96] = "";
        struct page *p = &s->pages[i];
        if (rd32(p->psp + 4) & PSP_USETITLE)
            text_of((const void *)(uintptr_t)rd32(p->psp + 20), (HANDLE)(uintptr_t)rd32(p->psp + 8), s->wide, name,
                    sizeof(name));
        else if (p->tmpl)
            template_title(p->tmpl, name, sizeof(name));
        uint32_t item[7] = {1, 0, 0, (uint32_t)(uintptr_t)name, 0, (uint32_t)-1, 0};
        w32_send(t, 0x1307, (WPARAM)i, (LPARAM)(uintptr_t)item);
    }
    int start = s->cur;
    s->cur = -1;
    if (!select_page(s, start))
        for (int i = 0; i < s->count && !select_page(s, i); i++)
            ;
    if (s->callback && (s->flags & PSH_USECALLBACK))
        W32_CALL(int(WINAPI *)(HANDLE, UINT, LPARAM), (int(WINAPI *)(HANDLE, UINT, LPARAM))s->callback, dlg, 1, 0);
}

static intptr_t WINAPI sheet_proc(HANDLE dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(dlg);
    if (!w)
        return FALSE;
    struct sheet *s = (struct sheet *)(uintptr_t)w->dlg_user;
    if (msg == WM_INITDIALOG) {
        w->dlg_user = lp;
        init_sheet(dlg, (struct sheet *)(uintptr_t)lp);
        return TRUE;
    }
    if (!s)
        return FALSE;
    switch (msg) {
    case WM_NOTIFY: {
        const uint32_t *n = (const uint32_t *)(uintptr_t)lp;
        if (n && (int)n[2] == TCN_SELCHANGE) {
            struct w32_window *t = w32_win(s->tab);
            int i = t ? (int)w32_send(t, 0x130b, 0, 0) : -1;
            if (!select_page(s, i) && t)
                w32_send(t, 0x130c, (WPARAM)s->cur, 0);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = (int)(wp & 0xffff);
        if (id == 1 || id == ID_APPLY) {
            if (apply_all(s) && id == 1)
                end_sheet(s, 1);
        } else if (id == 2) {
            for (int i = 0; i < s->count; i++)
                notify(s, i, PSN_RESET);
            end_sheet(s, 0);
        } else if (id == ID_NEXT || id == ID_BACK || id == ID_FINISH) {
            wizard_step(s, id == ID_NEXT ? PSN_WIZNEXT : id == ID_BACK ? PSN_WIZBACK : PSN_WIZFINISH);
        } else {
            return FALSE;
        }
        return TRUE;
    }
    case WM_CLOSE:
        DLG_CALL("user32.dll", "PostMessageA", BOOL(WINAPI *)(HANDLE, UINT, WPARAM, LPARAM))(dlg, WM_COMMAND, 2, 0);
        return TRUE;
    case WM_DESTROY:
        if (s->flags & PSH_MODELESS) {
            w->dlg_user = 0;
            for (int i = 0; i < s->count; i++)
                if (s->pages[i].owned)
                    w32_free(s->pages[i].psp);
            w32_free(s);
        }
        return FALSE;
    case PSM_SETCURSEL:
        if (lp) {
            for (int i = 0; i < s->count; i++)
                if (s->pages[i].psp == (uint8_t *)(uintptr_t)lp)
                    w->dlg_msgresult = select_page(s, i);
        } else {
            w->dlg_msgresult = select_page(s, (int)wp);
        }
        return TRUE;
    case PSM_CHANGED:
        enable(dlg, ID_APPLY, true);
        return TRUE;
    case PSM_UNCHANGED:
        enable(dlg, ID_APPLY, false);
        return TRUE;
    case PSM_CANCELTOCLOSE:
        DLG_CALL("user32.dll", "SetDlgItemTextA", BOOL(WINAPI *)(HANDLE, int, const char *))(dlg, 2, "Close");
        enable(dlg, 1, false);
        return TRUE;
    case PSM_QUERYSIBLINGS:
        for (int i = 0; i < s->count; i++) {
            struct w32_window *p = s->pages[i].hwnd ? w32_win(s->pages[i].hwnd) : NULL;
            LRESULT r = p ? w32_send(p, PSM_QUERYSIBLINGS, wp, lp) : 0;
            if (r) {
                w->dlg_msgresult = r;
                break;
            }
        }
        return TRUE;
    case PSM_APPLY:
        w->dlg_msgresult = apply_all(s);
        return TRUE;
    case PSM_SETTITLEA:
    case PSM_SETTITLEW: {
        char title[160];
        text_of((const void *)(uintptr_t)lp, NULL, msg == PSM_SETTITLEW, title, sizeof(title));
        DLG_CALL("user32.dll", "SetWindowTextA", BOOL(WINAPI *)(HANDLE, const char *))(dlg, title);
        return TRUE;
    }
    case PSM_SETWIZBUTTONS:
        s->wizbuttons = (uint32_t)lp;
        if (s->wizard)
            wizard_buttons(s);
        return TRUE;
    case PSM_SETFINISHTEXTA:
    case PSM_SETFINISHTEXTW: {
        char text[64];
        text_of((const void *)(uintptr_t)lp, NULL, msg == PSM_SETFINISHTEXTW, text, sizeof(text));
        DLG_CALL("user32.dll", "SetDlgItemTextA", BOOL(WINAPI *)(HANDLE, int, const char *))(dlg, ID_FINISH, text);
        s->wizbuttons = (s->wizbuttons & ~3u) | 4;
        if (s->wizard)
            wizard_buttons(s);
        return TRUE;
    }
    case PSM_PRESSBUTTON: {
        static const int ids[] = {ID_BACK, ID_NEXT, ID_FINISH, 1, ID_APPLY, 2, 9};
        if (wp < ARRAY_SIZE(ids))
            DLG_CALL("user32.dll", "PostMessageA", BOOL(WINAPI *)(HANDLE, UINT, WPARAM, LPARAM))(dlg, WM_COMMAND,
                                                                                                (WPARAM)ids[wp], 0);
        return TRUE;
    }
    case PSM_GETTABCONTROL:
        w->dlg_msgresult = (intptr_t)(uintptr_t)s->tab;
        return TRUE;
    case PSM_GETCURRENTPAGEHWND:
        w->dlg_msgresult = s->cur >= 0 ? (intptr_t)(uintptr_t)s->pages[s->cur].hwnd : 0;
        return TRUE;
    case PSM_ISDIALOGMESSAGE:
        w->dlg_msgresult = FALSE;
        return TRUE;
    case PSM_GETRESULT:
        w->dlg_msgresult = s->result;
        return TRUE;
    default: return FALSE;
    }
}

static intptr_t property_sheet(const uint8_t *h, bool wide)
{
    if (!h || w32->win64)
        return -1;
    struct sheet *s = w32_zalloc(sizeof(*s));
    if (!s)
        return -1;
    s->wide = wide;
    s->flags = rd32(h + 4);
    s->wizard = (s->flags & (PSH_WIZARD | PSH_WIZARD97 | PSH_AEROWIZARD)) != 0;
    s->caption = (const void *)(uintptr_t)rd32(h + 20);
    s->callback = (void *)(uintptr_t)rd32(h + 36);
    uint32_t n = MIN(rd32(h + 24), (uint32_t)MAX_PAGES), start = rd32(h + 28), list = rd32(h + 32);
    const uint8_t *array = (const uint8_t *)(uintptr_t)list;
    for (uint32_t i = 0; array && i < n; i++) {
        struct page *p = &s->pages[s->count];
        if (s->flags & PSH_PROPSHEETPAGE) {
            p->psp = (uint8_t *)array;
            array += MAX(rd32(array), 40u);
        } else {
            p->psp = (uint8_t *)(uintptr_t)rd32(array + i * 4);
        }
        if (!p->psp)
            continue;
        call_page_callback(p, 2);
        p->tmpl = page_template(p->psp, wide);
        s->count++;
    }
    s->cur = 0;
    if (s->flags & PSH_USEPSTARTPAGE) {
        char want[96], name[96];
        text_of((const void *)(uintptr_t)start, (HANDLE)(uintptr_t)rd32(h + 12), wide, want, sizeof(want));
        for (int i = 0; i < s->count; i++) {
            text_of((const void *)(uintptr_t)rd32(s->pages[i].psp + 20), NULL, wide, name, sizeof(name));
            if (!str_casecmp(name, want))
                s->cur = i;
        }
    } else if (start < (uint32_t)s->count) {
        s->cur = (int)start;
    }
    int cx = 212, cy = 188;
    for (int i = 0; i < s->count; i++)
        if (s->pages[i].tmpl) {
            int w, hh;
            template_size(s->pages[i].tmpl, &w, &hh);
            cx = MAX(cx, w);
            cy = MAX(cy, hh);
        }
    struct tmpl t = {w32_zalloc(2048), 0};
    if (!t.p) {
        w32_free(s);
        return -1;
    }
    bool apply = !s->wizard && !(s->flags & PSH_NOAPPLYNOW);
    int tab_h = s->wizard ? cy : cy + 16, width = cx + 14, height = tab_h + 32;
    dialog_header(&t, 0x80000000u | WS_CAPTION | WS_SYSMENU | 0x40 | 0x80, s->wizard ? 5 : 3 + apply, width, height,
                  "Properties");
    item_class(&t, s->wizard ? 0 : WS_TABSTOP, 7, 7, cx, tab_h, ID_TAB, s->wizard ? "Static" : "SysTabControl32", "");
    int by = tab_h + 13, bx = width - 7 - 50;
    if (s->wizard) {
        item(&t, WS_TABSTOP, bx, by, 50, 14, 2, 0x80, "Cancel");
        item(&t, WS_TABSTOP | 1, bx - 54, by, 50, 14, ID_FINISH, 0x80, "Finish");
        item(&t, WS_TABSTOP | 1, bx - 54, by, 50, 14, ID_NEXT, 0x80, "Next >");
        item(&t, WS_TABSTOP, bx - 104, by, 50, 14, ID_BACK, 0x80, "< Back");
    } else {
        if (apply) {
            item(&t, WS_TABSTOP | WS_DISABLED, bx, by, 50, 14, ID_APPLY, 0x80, "Apply");
            bx -= 54;
        }
        item(&t, WS_TABSTOP, bx, by, 50, 14, 2, 0x80, "Cancel");
        item(&t, WS_TABSTOP | 1, bx - 54, by, 50, 14, 1, 0x80, "OK");
    }
    HANDLE owner = (HANDLE)(uintptr_t)rd32(h + 8);
    intptr_t r;
    if (s->flags & PSH_MODELESS) {
        typedef HANDLE(WINAPI * create_fn)(HANDLE, const void *, HANDLE, void *, LPARAM);
        HANDLE dlg = DLG_CALL("user32.dll", "CreateDialogIndirectParamA", create_fn)(NULL, t.p, owner, (void *)sheet_proc,
                                                                                    (LPARAM)(uintptr_t)s);
        w32_free(t.p);
        if (!dlg) {
            w32_free(s);
            return -1;
        }
        show(dlg, true);
        return (intptr_t)(uintptr_t)dlg;
    }
    typedef intptr_t(WINAPI * box_fn)(HANDLE, const void *, HANDLE, void *, LPARAM);
    r = DLG_CALL("user32.dll", "DialogBoxIndirectParamA", box_fn)(NULL, t.p, owner, (void *)sheet_proc,
                                                                 (LPARAM)(uintptr_t)s);
    w32_free(t.p);
    for (int i = 0; i < s->count; i++)
        if (s->pages[i].owned)
            w32_free(s->pages[i].psp);
    w32_free(s);
    return r < 0 ? -1 : r;
}

static intptr_t WINAPI PropertySheetW(const uint8_t *h)
{
    return property_sheet(h, true);
}

static intptr_t WINAPI PropertySheetA(const uint8_t *h)
{
    return property_sheet(h, false);
}

static void *WINAPI CreatePropertySheetPageW(const uint8_t *psp)
{
    if (!psp || w32->win64)
        return NULL;
    uint32_t size = MAX(rd32(psp), 40u);
    uint8_t *copy = w32_alloc(size);
    if (copy) {
        memcpy(copy, psp, size);
        call_page_callback(&(struct page){copy, NULL, NULL, false}, 0);
    }
    return copy;
}

static BOOL WINAPI DestroyPropertySheetPage(void *page)
{
    if (page)
        call_page_callback(&(struct page){page, NULL, NULL, false}, 1);
    w32_free(page);
    return TRUE;
}

/* TaskDialog through the message box: common buttons pick the box type, custom buttons map onto it */
enum { TDCBF_OK = 1, TDCBF_YES = 2, TDCBF_NO = 4, TDCBF_CANCEL = 8, TDCBF_RETRY = 0x10, TDCBF_CLOSE = 0x20 };

static long task_dialog(HANDLE owner, HANDLE inst, const void *title, const void *main, const void *content,
                        uint32_t common, int custom, const uint8_t *buttons, int stride, int *pressed)
{
    char head[160], body[1024], text[1200];
    text_of(title, inst, true, head, sizeof(head));
    text_of(main, inst, true, text, sizeof(text));
    text_of(content, inst, true, body, sizeof(body));
    if (text[0] && body[0])
        str_copy(text + strlen(text), "\n\n", sizeof(text) - strlen(text));
    str_copy(text + strlen(text), body, sizeof(text) - strlen(text));
    UINT type = 0;
    int ids[3] = {1, 2, 0};
    if (custom >= 2) {
        type = 1;
        ids[0] = (int)rd32(buttons);
        ids[1] = (int)rd32(buttons + stride);
    } else if (custom == 1) {
        ids[0] = (int)rd32(buttons);
    } else if ((common & (TDCBF_YES | TDCBF_NO)) == (TDCBF_YES | TDCBF_NO)) {
        type = common & TDCBF_CANCEL ? 3 : 4;
    } else if (common & TDCBF_RETRY) {
        type = 5;
    } else if (common & TDCBF_CANCEL) {
        type = common & TDCBF_OK ? 1 : 0;
    }
    int r = DLG_CALL("user32.dll", "MessageBoxA", int(WINAPI *)(HANDLE, const char *, const char *, UINT))(
        owner, text, head[0] ? head : "Message", type);
    if (custom) {
        r = r == 1 ? ids[0] : r == 2 && custom >= 2 ? ids[1] : r;
    } else if (!type && (common & TDCBF_CANCEL)) {
        r = 2;
    } else if (!type && (common & TDCBF_CLOSE) && !(common & TDCBF_OK)) {
        r = 8;
    }
    if (pressed)
        *pressed = r;
    return 0;
}

static long WINAPI TaskDialog(HANDLE owner, HANDLE inst, const WCHAR *title, const WCHAR *main, const WCHAR *content,
                              uint32_t buttons, const WCHAR *icon, int *pressed)
{
    UNUSED(icon);
    return task_dialog(owner, inst, title, main, content, buttons ? buttons : TDCBF_OK, 0, NULL, 0, pressed);
}

static long WINAPI TaskDialogIndirect(const uint8_t *c, int *pressed, int *radio, BOOL *verify)
{
    if (!c)
        return (long)0x80070057;
    bool w64 = w32->win64;
    int ps = w64 ? 8 : 4;
    const uint8_t *f = c + 4;
    HANDLE owner = (HANDLE)(uintptr_t)rd32(f), inst = (HANDLE)(uintptr_t)rd32(f + ps);
    f += 2 * ps;
    uint32_t common = rd32(f + 4);
    f += 8;
    const void *title = (const void *)(uintptr_t)rd32(f);
    f += 2 * ps;
    const void *main = (const void *)(uintptr_t)rd32(f), *content = (const void *)(uintptr_t)rd32(f + ps);
    f += 2 * ps;
    int count = (int)rd32(f);
    const uint8_t *list = (const uint8_t *)(uintptr_t)rd32(f + 4);
    if (radio)
        *radio = 0;
    if (verify)
        *verify = FALSE;
    return task_dialog(owner, inst, title, main, content, common || count ? common : TDCBF_OK, list ? count : 0, list,
                       4 + ps, pressed);
}

/* drag lists: the list box gets the drag messages from the program's own handling; the helpers find items */
static BOOL WINAPI MakeDragList(HANDLE lb)
{
    return lb != NULL;
}

static void WINAPI DrawInsert(HANDLE parent, HANDLE lb, int item)
{
    UNUSED(parent);
    UNUSED(lb);
    UNUSED(item);
}

static int WINAPI LBItemFromPt(HANDLE lb, LONG x, LONG y, BOOL scroll)
{
    POINT pt = {x, y};
    UNUSED(scroll);
    struct w32_window *w = w32_win(lb);
    if (!w)
        return -1;
    DLG_CALL("user32.dll", "ScreenToClient", BOOL(WINAPI *)(HANDLE, POINT *))(lb, &pt);
    RECT r;
    DLG_CALL("user32.dll", "GetClientRect", BOOL(WINAPI *)(HANDLE, RECT *))(lb, &r);
    if (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)
        return -1;
    LRESULT v = w32_send(w, 0x1a9, 0, (LPARAM)((uint32_t)(pt.x & 0xffff) | (uint32_t)pt.y << 16));
    return v >> 16 & 0xffff ? -1 : (int)(v & 0xffff);
}

static const struct w32_export propsheet_exports[] = {
    W32_FN(PropertySheetW),
    W32_FN(PropertySheetA),
    W32_FN(CreatePropertySheetPageW),
    W32_AS("CreatePropertySheetPageA", CreatePropertySheetPageW),
    W32_FN(DestroyPropertySheetPage),
    W32_FN(TaskDialog),
    W32_FN(TaskDialogIndirect),
    W32_FN(MakeDragList),
    W32_FN(DrawInsert),
    W32_FN(LBItemFromPt),
};

static const char *const comctl32_ordinals[] = {
    [344] = "TaskDialog",
    [345] = "TaskDialogIndirect",
};

W32_EXPORTS(propsheet_module, "comctl32.dll", propsheet_exports, .ordinals = comctl32_ordinals,
            .ordinal_count = ARRAY_SIZE(comctl32_ordinals));
#endif
