/* Built-in controls (ListBox, ComboBox, RichEdit, subclassed classes) and GDI/user extras. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <richedit.h>
#include <commctrl.h>

static int measured, drawn, last_notify;
static int tab_notify;

static LRESULT CALLBACK parent_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MEASUREITEM) {
        ((MEASUREITEMSTRUCT *)l)->itemHeight = 22;
        measured++;
        return TRUE;
    }
    if (m == WM_DRAWITEM) {
        drawn++;
        return TRUE;
    }
    if (m == WM_COMMAND)
        last_notify = HIWORD(w);
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == TCN_SELCHANGE)
        tab_notify = TCN_SELCHANGE;
    return DefWindowProcA(h, m, w, l);
}

static WNDPROC static_proc;
static int sub_calls;

static LRESULT CALLBACK my_static(HWND h, UINT m, WPARAM w, LPARAM l)
{
    sub_calls++;
    return CallWindowProcA(static_proc, h, m, w, l);
}

static DWORD CALLBACK feed(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG *got)
{
    const char **src = (const char **)cookie;
    LONG n = (LONG)strlen(*src);
    if (n > cb)
        n = cb;
    memcpy(buf, *src, (size_t)n);
    *src += n;
    *got = n;
    return 0;
}

static void controls_test(HWND parent)
{
    HWND lb = CreateWindowExA(0, "ListBox", "", WS_CHILD | LBS_NOTIFY | LBS_SORT, 0, 0, 100, 80, parent, (HMENU)1,
                              NULL, NULL);
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM) "pear");
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM) "apple");
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM) "melon");
    SendMessageA(lb, LB_SETITEMDATA, 1, 77);
    char first[16] = "";
    SendMessageA(lb, LB_GETTEXT, 0, (LPARAM)first);
    LRESULT found = SendMessageA(lb, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM) "PEAR");
    SendMessageA(lb, LB_SETCURSEL, 2, 0);
    printf("listbox count=%d first=%s find=%d cur=%d data=%d len=%d\n", (int)SendMessageA(lb, LB_GETCOUNT, 0, 0),
           first, (int)found, (int)SendMessageA(lb, LB_GETCURSEL, 0, 0), (int)SendMessageA(lb, LB_GETITEMDATA, 1, 0),
           (int)SendMessageA(lb, LB_GETTEXTLEN, 2, 0));

    HWND od = CreateWindowExA(0, "ListBox", "", WS_CHILD | WS_VISIBLE | LBS_OWNERDRAWVARIABLE, 0, 0, 100, 80, parent,
                              (HMENU)2, NULL, NULL);
    SendMessageA(od, LB_ADDSTRING, 0, 1234);
    SendMessageA(od, LB_ADDSTRING, 0, 5678);
    RECT r = {0};
    SendMessageA(od, LB_GETITEMRECT, 1, (LPARAM)&r);
    printf("ownerdraw measured=%d data=%d top=%ld bottom=%ld\n", measured, (int)SendMessageA(od, LB_GETITEMDATA, 1, 0),
           r.top, r.bottom);

    HWND cb = CreateWindowExA(0, "ComboBox", "", WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 120, 200, parent, (HMENU)3, NULL,
                              NULL);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"red");
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"green");
    SendMessageA(cb, CB_SETCURSEL, 1, 0);
    WCHAR wide[16] = L"";
    SendMessageW(cb, CB_GETLBTEXT, 1, (LPARAM)wide);
    char shown[16] = "";
    GetWindowTextA(cb, shown, sizeof(shown));
    RECT cr;
    GetWindowRect(cb, &cr);
    printf("combo count=%d cur=%d text=%ls shown=%s short=%d\n", (int)SendMessageA(cb, CB_GETCOUNT, 0, 0),
           (int)SendMessageA(cb, CB_GETCURSEL, 0, 0), wide, shown, cr.bottom - cr.top < 60);

    LoadLibraryA("riched20.dll");
    HWND re = CreateWindowExW(0, L"RichEdit20W", L"", WS_CHILD | ES_MULTILINE | ES_READONLY, 0, 0, 200, 100, parent,
                              (HMENU)4, NULL, NULL);
    const char *rtf = "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}\\f0 Hello \\b bold\\b0\\par Caf\\'e9 \\u8364? end}";
    EDITSTREAM es = {(DWORD_PTR)&rtf, 0, feed};
    LRESULT got = SendMessageW(re, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    WCHAR text[64] = L"";
    GetWindowTextW(re, text, 64);
    for (WCHAR *p = text; *p; p++)
        if (*p == '\r' || *p == '\n')
            *p = '|';
    printf("richedit chars=%d text=%ls euro=%d lines=%d\n", (int)got, text, wcschr(text, 0x20ac) != NULL,
           (int)SendMessageW(re, EM_GETLINECOUNT, 0, 0));

    WNDCLASSA wc;
    GetClassInfoA(NULL, "Static", &wc);
    static_proc = wc.lpfnWndProc;
    wc.lpfnWndProc = my_static;
    wc.lpszClassName = "MyStatic";
    RegisterClassA(&wc);
    HWND st = CreateWindowExA(0, "MyStatic", "hi", WS_CHILD | SS_NOTIFY, 0, 0, 50, 20, parent, (HMENU)5, NULL, NULL);
    last_notify = -1;
    SendMessageA(st, WM_LBUTTONUP, 0, 0);
    printf("subclass calls=%d notify=%d\n", sub_calls > 0, last_notify == STN_CLICKED);
}

static void tab_test(HWND parent)
{
    HWND tc = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD, 0, 0, 200, 100, parent, (HMENU)7, NULL, NULL);
    const WCHAR *names[] = {L"One", L"Two", L"Three"};
    for (int i = 0; i < 3; i++) {
        TCITEMW it = {0};
        it.mask = TCIF_TEXT | TCIF_PARAM;
        it.pszText = (WCHAR *)names[i];
        it.lParam = 11 * (i + 1);
        SendMessageW(tc, TCM_INSERTITEMW, i, (LPARAM)&it);
    }
    BOOL del = (BOOL)SendMessageW(tc, TCM_DELETEITEM, 0, 0);
    SendMessageW(tc, TCM_SETCURSEL, 1, 0);
    WCHAR buf[16] = {0};
    TCITEMW got = {0};
    got.mask = TCIF_TEXT | TCIF_PARAM;
    got.pszText = buf;
    got.cchTextMax = 16;
    SendMessageW(tc, TCM_GETITEMW, 1, (LPARAM)&got);
    RECT page = {0, 0, 200, 100}, first;
    SendMessageW(tc, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    SendMessageW(tc, TCM_GETITEMRECT, 0, (LPARAM)&first);
    tab_notify = 0;
    SendMessageW(tc, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(first.left + 3, first.top + 3));
    printf("tabs del=%d count=%d text=%ls param=%d page=%d click=%d notify=%d\n", del,
           (int)SendMessageW(tc, TCM_GETITEMCOUNT, 0, 0), buf, (int)got.lParam, page.top > first.bottom - 4,
           (int)SendMessageW(tc, TCM_GETCURSEL, 0, 0), tab_notify == TCN_SELCHANGE);
}

static void gdi_test(void)
{
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 40, 40);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all = {0, 0, 40, 40};
    FillRect(dc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 10, 10, 30, 30);
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    SelectObject(dc, red);
    BOOL filled = ExtFloodFill(dc, 20, 20, RGB(0, 0, 0), FLOODFILLBORDER);
    COLORREF in = GetPixel(dc, 20, 20), out = GetPixel(dc, 5, 5);
    printf("floodfill ok=%d inside=%06lx outside=%06lx\n", filled, (unsigned long)in, (unsigned long)out);

    struct {
        WORD version, count;
        PALETTEENTRY e[4];
    } lp = {0x300, 4, {{255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}, {255, 255, 255, 0}}};
    HPALETTE pal = CreatePalette((LOGPALETTE *)&lp);
    PALETTEENTRY back[4];
    UINT n = GetPaletteEntries(pal, 0, 4, back);
    UINT nearest = GetNearestPaletteIndex(pal, RGB(10, 10, 240));
    WORD count = 0;
    GetObjectA(pal, sizeof(count), &count);
    printf("palette entries=%u green=%u nearest=%u count=%u\n", n, back[1].peGreen, nearest, count);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteObject(pal);
    DeleteObject(red);
    DeleteObject(pen);
    DeleteDC(dc);
}

static void user_test(void)
{
    ATOM a = AddAtomA("SynthAtom");
    ATOM again = FindAtomA("synthatom");
    char name[32] = "";
    GetAtomNameA(a, name, sizeof(name));
    ATOM num = AddAtomA("#42");
    printf("atom high=%d same=%d name=%s num=%d\n", a >= 0xc000, again == a, name, num);
    WriteProfileStringA("SynthTest", "Key", "Value9");
    char v[32] = "";
    GetProfileStringA("SynthTest", "Key", "none", v, sizeof(v));
    WriteProfileStringA("SynthTest", "N", "17");
    printf("profile value=%s int=%u\n", v, GetProfileIntA("SynthTest", "N", 0));
}

static void ole_test(void)
{
    HMODULE ole = LoadLibraryA("oleaut32.dll");
    SAFEARRAY *(WINAPI *create)(VARTYPE, UINT, SAFEARRAYBOUND *) = (void *)GetProcAddress(ole, "SafeArrayCreate");
    HRESULT(WINAPI * put)(SAFEARRAY *, LONG *, void *) = (void *)GetProcAddress(ole, "SafeArrayPutElement");
    HRESULT(WINAPI * get)(SAFEARRAY *, LONG *, void *) = (void *)GetProcAddress(ole, "SafeArrayGetElement");
    HRESULT(WINAPI * ubound)(SAFEARRAY *, UINT, LONG *) = (void *)GetProcAddress(ole, "SafeArrayGetUBound");
    UINT(WINAPI * dims)(SAFEARRAY *) = (void *)GetProcAddress(ole, "SafeArrayGetDim");
    if (!create || !put || !get || !ubound || !dims) {
        printf("safearray missing\n");
        return;
    }
    SAFEARRAYBOUND b[2] = {{3, 0}, {4, 1}};
    SAFEARRAY *sa = create(VT_I4, 2, b);
    LONG idx[2] = {2, 4}, val = 99, back = 0, ub1 = 0, ub2 = 0;
    put(sa, idx, &val);
    get(sa, idx, &back);
    ubound(sa, 1, &ub1);
    ubound(sa, 2, &ub2);
    printf("safearray dims=%u ub1=%ld ub2=%ld value=%ld\n", dims(sa), ub1, ub2, back);

    HMODULE cc = LoadLibraryA("comctl32.dll");
    HANDLE(WINAPI * il_create)(int, int, UINT, int, int) = (void *)GetProcAddress(cc, "ImageList_Create");
    int(WINAPI * il_add)(HANDLE, HBITMAP, HBITMAP) = (void *)GetProcAddress(cc, "ImageList_Add");
    int(WINAPI * il_count)(HANDLE) = (void *)GetProcAddress(cc, "ImageList_GetImageCount");
    BOOL(WINAPI * il_size)(HANDLE, int *, int *) = (void *)GetProcAddress(cc, "ImageList_GetIconSize");
    if (!il_create || !il_add || !il_count || !il_size) {
        printf("imagelist missing\n");
        return;
    }
    HANDLE il = il_create(16, 16, ILC_COLOR32, 2, 2);
    HDC dc = GetDC(NULL);
    HBITMAP strip = CreateCompatibleBitmap(dc, 32, 16);
    ReleaseDC(NULL, dc);
    int first = il_add(il, strip, NULL);
    int cx = 0, cy = 0;
    il_size(il, &cx, &cy);
    printf("imagelist first=%d count=%d size=%dx%d\n", first, il_count(il), cx, cy);
}

int main(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = parent_proc;
    wc.lpszClassName = "ctlparent";
    RegisterClassA(&wc);
    HWND parent = CreateWindowExA(0, "ctlparent", "controls", WS_OVERLAPPEDWINDOW, 0, 0, 300, 300, NULL, NULL, NULL,
                                  NULL);
    if (!parent) {
        printf("controls need the SynthOS desktop\n");
    } else {
        controls_test(parent);
        tab_test(parent);
        DestroyWindow(parent);
    }
    gdi_test();
    user_test();
    ole_test();
    return 0;
}
