/* Built-in controls (ListBox, ComboBox, RichEdit, subclassed classes) and GDI/user extras. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <richedit.h>
#include <commctrl.h>

static int measured, drawn, last_notify;
static int tab_notify;
static int list_changed, last_command;
static int tree_selected, tree_expanding, tree_deleted;
static UINT find_msg;
static DWORD find_flags;

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
    if (m == WM_COMMAND) {
        last_notify = HIWORD(w);
        last_command = LOWORD(w);
    }
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == LVN_GETDISPINFOW) {
        NMLVDISPINFOW *d = (NMLVDISPINFOW *)l;
        if (d->item.mask & LVIF_TEXT)
            swprintf(d->item.pszText, d->item.cchTextMax, L"v%d.%d", d->item.iItem, d->item.iSubItem);
    }
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == LVN_GETDISPINFOA) {
        NMLVDISPINFOA *d = (NMLVDISPINFOA *)l;
        if (d->item.mask & LVIF_TEXT)
            snprintf(d->item.pszText, d->item.cchTextMax, "a%d.%d", d->item.iItem, d->item.iSubItem);
    }
    if (find_msg && m == find_msg)
        find_flags = ((FINDREPLACEW *)l)->Flags;
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == LVN_ITEMCHANGED)
        list_changed++;
    if (m == WM_NOTIFY && (((NMHDR *)l)->code == TVN_SELCHANGEDW || ((NMHDR *)l)->code == TVN_SELCHANGEDA))
        tree_selected = (int)((NMTREEVIEWW *)l)->itemNew.lParam;
    if (m == WM_NOTIFY && (((NMHDR *)l)->code == TVN_ITEMEXPANDINGW || ((NMHDR *)l)->code == TVN_ITEMEXPANDINGA))
        tree_expanding++;
    if (m == WM_NOTIFY && (((NMHDR *)l)->code == TVN_DELETEITEMW || ((NMHDR *)l)->code == TVN_DELETEITEMA))
        tree_deleted++;
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

static int CALLBACK by_param_desc(LPARAM a, LPARAM b, LPARAM sort)
{
    return (int)(b - a) * (int)sort;
}

static void list_test(HWND parent)
{
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 0, 0, 300, 200, parent, (HMENU)8, NULL, NULL);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 100;
    col.pszText = L"Name";
    SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = L"Size";
    SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    const WCHAR *names[] = {L"alpha", L"beta", L"gamma"};
    for (int i = 0; i < 3; i++) {
        LVITEMW it = {0};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = (WCHAR *)names[i];
        it.lParam = i + 1;
        SendMessageW(lv, LVM_INSERTITEMW, i, (LPARAM)&it);
        it.mask = LVIF_TEXT;
        it.iSubItem = 1;
        it.pszText = i == 1 ? LPSTR_TEXTCALLBACKW : L"10";
        SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
    SendMessageW(lv, LVM_SORTITEMS, 1, (LPARAM)by_param_desc);
    WCHAR first[16] = {0}, sub[16] = {0};
    LVITEMW get = {0};
    get.pszText = first;
    get.cchTextMax = 16;
    SendMessageW(lv, LVM_GETITEMTEXTW, 0, (LPARAM)&get);
    get.iSubItem = 1;
    get.pszText = sub;
    SendMessageW(lv, LVM_GETITEMTEXTW, 1, (LPARAM)&get);
    LVFINDINFOW find = {0};
    find.flags = LVFI_STRING;
    find.psz = L"Beta";
    int found = (int)SendMessageW(lv, LVM_FINDITEMW, -1, (LPARAM)&find);
    RECT r = {LVIR_BOUNDS};
    SendMessageW(lv, LVM_GETITEMRECT, 2, (LPARAM)&r);
    list_changed = 0;
    SendMessageW(lv, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(r.left + 5, r.top + 3));
    int sel = (int)SendMessageW(lv, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
    HWND virt = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_OWNERDATA, 0, 0, 300, 200, parent,
                                (HMENU)9, NULL, NULL);
    col.pszText = L"A";
    SendMessageW(virt, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    SendMessageW(virt, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    SendMessageW(virt, LVM_SETITEMCOUNT, 500, 0);
    WCHAR vtext[16] = {0};
    get.iSubItem = 1;
    get.pszText = vtext;
    SendMessageW(virt, LVM_GETITEMTEXTW, 420, (LPARAM)&get);
    printf("listview count=%d first=%ls sub=%ls find=%d sel=%d changed=%d virtual=%d text=%ls\n",
           (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0), first, sub, found, sel, list_changed > 0,
           (int)SendMessageW(virt, LVM_GETITEMCOUNT, 0, 0), vtext);

    TBBUTTON buttons[3] = {{0, 101, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, 0},
                           {0, 0, 0, BTNS_SEP, {0}, 0, 0},
                           {1, 102, TBSTATE_ENABLED, BTNS_CHECK, {0}, 0, 0}};
    HWND tb = CreateToolbarEx(parent, WS_CHILD | TBSTYLE_FLAT, 10, 0, NULL, 0, buttons, 3, 0, 0, 16, 16,
                              sizeof(TBBUTTON));
    RECT br;
    SendMessageW(tb, TB_GETITEMRECT, 2, (LPARAM)&br);
    last_command = 0;
    SendMessageW(tb, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(br.left + 3, br.top + 3));
    SendMessageW(tb, WM_LBUTTONUP, 0, MAKELPARAM(br.left + 3, br.top + 3));
    HWND sb = CreateStatusWindowW(WS_CHILD, L"Ready", parent, 11);
    int parts[2] = {100, -1};
    SendMessageW(sb, SB_SETPARTS, 2, (LPARAM)parts);
    SendMessageW(sb, SB_SETTEXTW, 1, (LPARAM)L"3 files");
    WCHAR st[16] = {0};
    SendMessageW(sb, SB_GETTEXTW, 1, (LPARAM)st);
    printf("toolbar count=%d cmd=%d checked=%d status=%ls parts=%d\n", (int)SendMessageW(tb, TB_BUTTONCOUNT, 0, 0),
           last_command, (int)SendMessageW(tb, TB_ISBUTTONCHECKED, 102, 0), st,
           (int)SendMessageW(sb, SB_GETPARTS, 0, 0));
}

static HTREEITEM tree_add(HWND tv, HTREEITEM parent, HTREEITEM after, const WCHAR *text, int param)
{
    TVINSERTSTRUCTW in = {0};
    in.hParent = parent;
    in.hInsertAfter = after;
    in.item.mask = TVIF_TEXT | TVIF_PARAM;
    in.item.pszText = (WCHAR *)text;
    in.item.lParam = param;
    return (HTREEITEM)SendMessageW(tv, TVM_INSERTITEMW, 0, (LPARAM)&in);
}

static void tree_test(HWND parent)
{
    HWND tv = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | TVS_HASBUTTONS | TVS_LINESATROOT, 0, 0, 200, 200, parent,
                              (HMENU)12, NULL, NULL);
    HTREEITEM root = tree_add(tv, TVI_ROOT, TVI_LAST, L"General", 1);
    tree_add(tv, root, TVI_SORT, L"Web", 3);
    HTREEITEM mail = tree_add(tv, root, TVI_SORT, L"Email", 2);
    tree_add(tv, TVI_ROOT, TVI_LAST, L"Backup", 4);
    SendMessageW(tv, TVM_EXPAND, TVE_EXPAND, (LPARAM)root);
    HTREEITEM child = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CHILD, (LPARAM)root);
    WCHAR text[16] = {0};
    TVITEMW it = {0};
    it.mask = TVIF_TEXT | TVIF_STATE;
    it.hItem = child;
    it.pszText = text;
    it.cchTextMax = 16;
    it.stateMask = TVIS_EXPANDED;
    SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&it);
    RECT r;
    *(HTREEITEM *)&r = mail;
    SendMessageW(tv, TVM_GETITEMRECT, TRUE, (LPARAM)&r);
    SendMessageW(tv, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(r.left + 3, r.top + 3));
    HTREEITEM sel = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CARET, 0);
    SendMessageW(tv, WM_KEYDOWN, VK_DOWN, 0);
    TVITEMW p = {0};
    p.mask = TVIF_PARAM;
    p.hItem = (HTREEITEM)SendMessageW(tv, TVM_GETNEXTITEM, TVGN_CARET, 0);
    SendMessageW(tv, TVM_GETITEMW, 0, (LPARAM)&p);
    SendMessageW(tv, TVM_DELETEITEM, 0, (LPARAM)root);
    printf("treeview first=%ls sel=%d down=%d notify=%d expanding=%d deleted=%d count=%d\n", text, sel == mail,
           (int)p.lParam, tree_selected, tree_expanding, tree_deleted, (int)SendMessageW(tv, TVM_GETCOUNT, 0, 0));
}

static int cbt_created, mdi_activations;
static HHOOK cbt_hook;

static LRESULT CALLBACK cbt_proc(int code, WPARAM w, LPARAM l)
{
    if (code == HCBT_CREATEWND && ((CBT_CREATEWNDA *)l)->lpcs)
        cbt_created++;
    return CallNextHookEx(cbt_hook, code, w, l);
}

static LRESULT CALLBACK doc_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MDIACTIVATE && (HWND)l == h)
        mdi_activations++;
    return DefMDIChildProcA(h, m, w, l);
}

/* an MDI client with two documents; a CBT hook sees each window being made */
static void mdi_test(HWND parent)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = doc_proc;
    wc.lpszClassName = "ctldoc";
    RegisterClassA(&wc);
    cbt_hook = SetWindowsHookExA(WH_CBT, cbt_proc, NULL, GetCurrentThreadId());
    CLIENTCREATESTRUCT ccs = {NULL, 100};
    HWND client = CreateWindowExA(0, "MDICLIENT", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 200, 200, parent, NULL, NULL, &ccs);
    MDICREATESTRUCTA mcs = {"ctldoc", "one", NULL, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0};
    HWND one = (HWND)SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs);
    mcs.szTitle = "two";
    HWND two = (HWND)SendMessageA(client, WM_MDICREATE, 0, (LPARAM)&mcs);
    UnhookWindowsHookEx(cbt_hook);
    int created = cbt_created;
    HWND active = (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0);
    SendMessageA(client, WM_MDINEXT, 0, 0);
    HWND next = (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0);
    SendMessageA(client, WM_MDIDESTROY, (WPARAM)next, 0);
    HWND left = (HWND)SendMessageA(client, WM_MDIGETACTIVE, 0, 0);
    RECT r;
    GetClientRect(two, &r);
    printf("mdi hook=%d active=%d next=%d left=%d id=%d fill=%d activations=%d top=%d\n", created, active == two, next == one,
           left == two && !IsWindow(one), GetDlgCtrlID(two), r.right == 200 && r.bottom == 200, mdi_activations,
           GetTopWindow(parent) != NULL);
    DestroyWindow(client);
}

static void find_test(HWND parent)
{
    static WCHAR what[32] = L"needle";
    static FINDREPLACEW fr;
    find_msg = RegisterWindowMessageW(FINDMSGSTRINGW);
    fr.lStructSize = sizeof(fr);
    fr.hwndOwner = parent;
    fr.lpstrFindWhat = what;
    fr.wFindWhatLen = 32;
    fr.Flags = FR_DOWN | FR_MATCHCASE;
    HWND d = FindTextW(&fr);
    WCHAR shown[32] = {0};
    GetDlgItemTextW(d, 0x480, shown, 32);
    SetDlgItemTextW(d, 0x480, L"hay");
    SendMessageW(d, WM_COMMAND, IDOK, 0);
    DWORD next = find_flags;
    SendMessageW(d, WM_COMMAND, IDCANCEL, 0);
    printf("findtext shown=%ls next=%d case=%d text=%ls term=%d gone=%d\n", shown, (next & FR_FINDNEXT) != 0,
           (next & FR_MATCHCASE) != 0, what, (find_flags & FR_DIALOGTERM) != 0, !IsWindow(d));
}

static const char *drive_title;
static int page_inits, page_applies;

static void CALLBACK drive_dialog(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    (void)m;
    (void)t;
    HWND d = FindWindowA(NULL, drive_title);
    if (!d)
        return;
    KillTimer(h, id);
    if (!strcmp(drive_title, "Color")) {
        SetDlgItemInt(d, 0x2c2, 10, FALSE);
        SetDlgItemInt(d, 0x2c3, 20, FALSE);
        SetDlgItemInt(d, 0x2c4, 30, FALSE);
    } else {
        HWND page = (HWND)SendMessageW(d, PSM_GETCURRENTPAGEHWND, 0, 0);
        SendMessageW(d, PSM_CHANGED, (WPARAM)page, 0);
    }
    PostMessageW(d, WM_COMMAND, IDOK, 0);
}

static INT_PTR CALLBACK page_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)h;
    (void)w;
    if (m == WM_INITDIALOG)
        page_inits++;
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == (UINT)PSN_APPLY)
        page_applies++;
    return m == WM_INITDIALOG;
}

static void dialogs_test(HWND parent)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc = {0};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = parent;
    cc.rgbResult = RGB(200, 0, 0);
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT;
    drive_title = "Color";
    SetTimer(parent, 7, 30, drive_dialog);
    BOOL picked = ChooseColorW(&cc);
    static WORD tmpl[64];
    DLGTEMPLATE *t = (DLGTEMPLATE *)tmpl;
    t->style = WS_CHILD | DS_CONTROL;
    t->cx = 120;
    t->cy = 60;
    WORD *p = (WORD *)(t + 1);
    p[0] = p[1] = 0;
    memcpy(p + 2, L"General", 16);
    PROPSHEETPAGEW pg[2] = {0};
    for (int i = 0; i < 2; i++) {
        pg[i].dwSize = sizeof(pg[i]);
        pg[i].dwFlags = PSP_DLGINDIRECT;
        pg[i].pResource = t;
        pg[i].pfnDlgProc = page_proc;
    }
    PROPSHEETHEADERW ph = {0};
    ph.dwSize = sizeof(ph);
    ph.dwFlags = PSH_PROPSHEETPAGE;
    ph.hwndParent = parent;
    ph.pszCaption = L"Sheet";
    ph.nPages = 2;
    ph.ppsp = pg;
    drive_title = "Sheet";
    SetTimer(parent, 8, 30, drive_dialog);
    INT_PTR r = PropertySheetW(&ph);
    printf("dialogs color=%d rgb=%d,%d,%d sheet=%d inits=%d applies=%d\n", picked, GetRValue(cc.rgbResult),
           GetGValue(cc.rgbResult), GetBValue(cc.rgbResult), (int)r, page_inits, page_applies);
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

    BITMAPINFOHEADER bi = {sizeof(bi), 5, 3, 1, 24, BI_RGB};
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    DIBSECTION ds = {0};
    BITMAP plain = {0};
    int got = GetObjectA(dib, sizeof(ds), &ds), small = GetObjectA(bmp, sizeof(ds), &plain);
    printf("dibsection size=%d bits=%d bpp=%d pitch=%ld height=%ld ddb=%d\n", got, ds.dsBm.bmBits == bits,
           ds.dsBmih.biBitCount, (long)ds.dsBm.bmWidthBytes, (long)ds.dsBmih.biHeight, small);
    DeleteObject(dib);
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
        list_test(parent);
        tree_test(parent);
        mdi_test(parent);
        find_test(parent);
        dialogs_test(parent);
        DestroyWindow(parent);
    }
    gdi_test();
    user_test();
    ole_test();
    return 0;
}
