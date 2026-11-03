/* A 64-bit window procedure and callbacks called by the system (needs the desktop or w32headless on). */
#include <stdio.h>
#include <windows.h>

static int creates, customs;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CREATE)
        creates++;
    if (m == WM_USER + 7) {
        customs++;
        return w + l;
    }
    return DefWindowProcA(h, m, w, l);
}

static BOOL CALLBACK each(HWND h, LPARAM l)
{
    (void)h;
    (*(int *)l)++;
    return TRUE;
}

static int fired;

static void CALLBACK on_timer(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    (void)h;
    (void)m;
    (void)t;
    fired += id == 5;
}

int main(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "proc64";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "proc64", "t", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    LRESULT r = SendMessageA(h, WM_USER + 7, 40, 2);
    int n = 0;
    EnumWindows(each, (LPARAM)&n);
    SetTimer(h, 5, 10, on_timer);
    MSG msg;
    DWORD start = GetTickCount();
    while (!fired && GetTickCount() - start < 2000 && GetMessageA(&msg, NULL, 0, 0))
        DispatchMessageA(&msg);
    printf("proc64 create=%d custom=%d r=%d enum=%d timer=%d\n", creates, customs, (int)r, n > 0, fired > 0);
    DestroyWindow(h);
    return 0;
}
