/* Menus built and changed in code, child windows found by point and the message queue status. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void menu_test(void)
{
    HMENU m = CreatePopupMenu(), sub = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, 10, "&Open");
    AppendMenuA(m, MF_STRING, 20, "Save");
    AppendMenuA(sub, MF_STRING, 31, "One");
    AppendMenuA(sub, MF_STRING, 32, "Two");
    AppendMenuA(sub, MF_STRING, 33, "Three");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sub, "More");
    InsertMenuW(m, 20, MF_BYCOMMAND | MF_STRING, 15, L"Close");
    ModifyMenuW(m, 1, MF_BYPOSITION | MF_STRING, 16, L"Close all");

    MENUITEMINFOW mi = {sizeof(mi)};
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
    mi.wID = 40;
    mi.fState = MFS_DISABLED;
    mi.dwTypeData = L"Print";
    InsertMenuItemW(m, 2, TRUE, &mi);

    WCHAR text[32];
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_SUBMENU;
    mi.dwTypeData = text;
    mi.cch = 32;
    GetMenuItemInfoW(m, 2, TRUE, &mi);
    char name[32];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, name, sizeof(name), NULL, NULL);
    printf("menu count=%d item=%s id=%u state=%u cch=%u\n", GetMenuItemCount(m), name, mi.wID, mi.fState, mi.cch);

    char s[32];
    GetMenuStringA(m, 16, s, sizeof(s), MF_BYCOMMAND);
    printf("menu string=%s id1=%u sub=%d\n", s, GetMenuItemID(m, 1), (int)GetMenuItemID(m, 4));

    CheckMenuRadioItem(sub, 31, 33, 32, MF_BYCOMMAND);
    printf("menu radio=%d%d%d\n", !!(GetMenuState(m, 31, MF_BYCOMMAND) & MF_CHECKED),
           !!(GetMenuState(m, 32, MF_BYCOMMAND) & MF_CHECKED), !!(GetMenuState(m, 33, MF_BYCOMMAND) & MF_CHECKED));

    SetMenuDefaultItem(m, 10, FALSE);
    mi.fMask = MIIM_SUBMENU;
    GetMenuItemInfoW(m, 4, TRUE, &mi);
    RemoveMenu(m, 0, MF_BYPOSITION);
    DeleteMenu(m, 40, MF_BYCOMMAND);
    printf("menu default=%d submenu=%d left=%d gone=%d\n", (int)GetMenuDefaultItem(m, FALSE, 0), mi.hSubMenu == sub,
           GetMenuItemCount(m), GetMenuState(m, 10, MF_BYCOMMAND) == (UINT)-1);
    DestroyMenu(m);
}

static void point_test(void)
{
    HWND top = CreateWindowExA(0, "STATIC", "top", WS_POPUP, 0, 0, 200, 100, NULL, NULL, NULL, NULL);
    HWND child = CreateWindowExA(0, "BUTTON", "b", WS_CHILD | WS_VISIBLE, 10, 10, 50, 20, top, NULL, NULL, NULL);
    HWND hidden = CreateWindowExA(0, "BUTTON", "h", WS_CHILD, 100, 10, 50, 20, top, NULL, NULL, NULL);
    POINT in = {15, 15}, out = {300, 15}, empty = {80, 80}, hid = {110, 15};
    printf("point child=%d outside=%d empty=%d hidden=%d/%d\n", ChildWindowFromPoint(top, in) == child,
           ChildWindowFromPoint(top, out) == NULL, ChildWindowFromPoint(top, empty) == top,
           ChildWindowFromPoint(top, hid) == hidden, ChildWindowFromPointEx(top, hid, CWP_SKIPINVISIBLE) == top);

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        ;
    DWORD before = GetQueueStatus(QS_ALLINPUT);
    PostMessageA(top, WM_USER + 1, 0, 0);
    DWORD after = GetQueueStatus(QS_ALLINPUT);
    BOOL got = PeekMessageA(&msg, top, WM_USER + 1, WM_USER + 1, PM_REMOVE);
    printf("queue before=%lx after=%lx got=%d pos=%d\n", (unsigned long)before, (unsigned long)(after & QS_POSTMESSAGE),
           got, GetMessagePos() == (DWORD)MAKELONG(msg.pt.x, msg.pt.y));
    DestroyWindow(top);
}

int main(void)
{
    menu_test();
    point_test();
    printf("popup without desktop=%d\n", (int)TrackPopupMenu(CreatePopupMenu(), TPM_RETURNCMD, 0, 0, 0, NULL, NULL));
    return 0;
}
