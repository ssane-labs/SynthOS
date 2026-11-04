/* 64-bit layouts of system information: processors, UI language, class info. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>

static __attribute__((noinline)) int target(int x)
{
    return x + 1;
}

static __attribute__((noinline)) int hook(int x)
{
    return x * 10;
}

/* code hooks the way Delphi libraries patch functions: make the page writable, write a jump, call it */
static void hook_test(void)
{
    unsigned char *code = (unsigned char *)target;
    DWORD old = 0;
    BOOL made = VirtualProtect(code, 16, PAGE_EXECUTE_READWRITE, &old);
    LONG rel = (LONG)((unsigned char *)hook - (code + 5));
    code[0] = 0xe9;
    memcpy(code + 1, &rel, 4);
    DWORD back = 0;
    VirtualProtect(code, 16, old, &back);
    FlushInstructionCache(GetCurrentProcess(), code, 16);
    int (*volatile fn)(int) = target;
    printf("codehook protect=%d old=%lx rwx=%lx result=%d\n", made, old, back, fn(4));
}


static int tab_notify;

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

static LRESULT CALLBACK tab_parent(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NOTIFY && ((NMHDR *)l)->hwndFrom && ((NMHDR *)l)->code == TCN_SELCHANGE)
        tab_notify = TCN_SELCHANGE;
    return DefWindowProcW(h, m, w, l);
}

static void window_test(void)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = tab_parent;
    wc.lpszClassName = L"tabparent";
    RegisterClassW(&wc);
    HWND parent = CreateWindowExW(0, L"tabparent", L"tabs", WS_OVERLAPPEDWINDOW, 0, 0, 300, 300, NULL, NULL, NULL, NULL);
    if (!parent)
        return;
    tab_test(parent);
    DestroyWindow(parent);
}

int main(void)
{
    DWORD len = 0;
    BOOL first = GetLogicalProcessorInformation(NULL, &len);
    DWORD err = GetLastError();
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = malloc(len);
    BOOL ok = GetLogicalProcessorInformation(info, &len);
    int cores = 0, caches = 0, l2 = 0;
    for (DWORD i = 0; ok && i < len / sizeof(*info); i++) {
        cores += info[i].Relationship == RelationProcessorCore;
        if (info[i].Relationship == RelationCache) {
            caches++;
            if (info[i].Cache.Level == 2)
                l2 = (int)(info[i].Cache.Size / 1024);
        }
    }
    printf("cpuinfo first=%d err=%lu ok=%d stride=%d cores=%d caches=%d l2=%dk\n", first, err, ok,
           (int)(len % sizeof(*info)), cores, caches, l2);
    free(info);
    WNDCLASSW wc;
    BOOL cls = GetClassInfoW(NULL, L"Button", &wc);
    printf("uilang thread=%x class=%d proc=%d name=%d\n", GetThreadUILanguage(), cls, wc.lpfnWndProc != NULL,
           wc.lpszClassName != NULL);
    hook_test();
    window_test();
    return 0;
}
