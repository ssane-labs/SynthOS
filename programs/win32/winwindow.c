/* A classic Win32 window: controls, a menu, a timer and GDI painting. */
#include <math.h>
#include <stdio.h>
#include <windows.h>

enum {
    ID_HELLO = 101,
    ID_COUNT,
    ID_ANIMATE,
    ID_NAME,
    ID_LABEL,
    ID_FILE_RESET = 201,
    ID_FILE_EXIT,
    ID_HELP_ABOUT,
};

static int clicks;
static int phase;
static HFONT title_font;

static void paint(HWND hwnd, HDC dc)
{
    RECT client;
    GetClientRect(hwnd, &client);
    RECT canvas = {250, 20, client.right - 20, client.bottom - 20};
    HBRUSH sky = CreateSolidBrush(RGB(232, 240, 255));
    FillRect(dc, &canvas, sky);
    DeleteObject(sky);

    HPEN outline = CreatePen(PS_SOLID, 2, RGB(40, 70, 160));
    HGDIOBJ old_pen = SelectObject(dc, outline);
    HBRUSH sun = CreateSolidBrush(RGB(255, 196, 40));
    HGDIOBJ old_brush = SelectObject(dc, sun);
    int cx = (canvas.left + canvas.right) / 2, cy = (canvas.top + canvas.bottom) / 2;
    int r = 34 + (int)(10 * sin(phase / 6.0));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);

    HBRUSH star = CreateSolidBrush(RGB(230, 60, 90));
    SelectObject(dc, star);
    POINT pts[10];
    for (int i = 0; i < 10; i++) {
        double a = phase / 20.0 + i * 3.14159265 / 5;
        int len = i % 2 ? 14 : 32;
        pts[i].x = canvas.left + 50 + (int)(len * cos(a));
        pts[i].y = canvas.bottom - 50 + (int)(len * sin(a));
    }
    Polygon(dc, pts, 10);

    for (int i = 0; i < 8; i++) {
        HBRUSH b = CreateSolidBrush(RGB(30 * i, 120 + 15 * i, 255 - 25 * i));
        RECT bar = {canvas.right - 30 - i * 14, canvas.bottom - 20 - (i + 1) * 9, canvas.right - 20 - i * 14,
                    canvas.bottom - 20};
        FillRect(dc, &bar, b);
        DeleteObject(b);
    }

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(sun);
    DeleteObject(star);
    DeleteObject(outline);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, title_font);
    SetTextColor(dc, RGB(20, 30, 60));
    TextOutA(dc, canvas.left + 12, canvas.top + 10, "GDI on SynthOS", 14);
    SelectObject(dc, old_font);
    char line[64];
    snprintf(line, sizeof(line), "Frame %d, clicks %d", phase, clicks);
    RECT text = {canvas.left, canvas.top + 40, canvas.right, canvas.top + 60};
    SetTextColor(dc, RGB(80, 90, 110));
    DrawTextA(dc, line, -1, &text, DT_CENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = ((CREATESTRUCT *)lp)->hInstance;
        CreateWindowA("STATIC", "Your name:", WS_CHILD | WS_VISIBLE, 20, 22, 200, 20, hwnd, NULL, inst, NULL);
        CreateWindowA("EDIT", "World", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 44, 210, 26, hwnd, (HMENU)ID_NAME,
                      inst, NULL);
        CreateWindowA("BUTTON", "Say &hello", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 84, 210, 30, hwnd,
                      (HMENU)ID_HELLO, inst, NULL);
        CreateWindowA("BUTTON", "Count clicks", WS_CHILD | WS_VISIBLE, 20, 122, 210, 30, hwnd, (HMENU)ID_COUNT, inst,
                      NULL);
        CreateWindowA("BUTTON", "Animate", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 166, 210, 22, hwnd,
                      (HMENU)ID_ANIMATE, inst, NULL);
        CreateWindowA("STATIC", "No clicks yet", WS_CHILD | WS_VISIBLE, 20, 200, 210, 40, hwnd, (HMENU)ID_LABEL, inst,
                      NULL);
        CheckDlgButton(hwnd, ID_ANIMATE, BST_CHECKED);
        title_font = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI");
        SetTimer(hwnd, 1, 50, NULL);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_HELLO: {
            char name[64], text[96];
            GetDlgItemTextA(hwnd, ID_NAME, name, sizeof(name));
            snprintf(text, sizeof(text), "Hello, %s!", name);
            MessageBoxA(hwnd, text, "Win32 demo", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        case ID_COUNT: {
            char text[48];
            clicks++;
            snprintf(text, sizeof(text), "Clicked %d time%s", clicks, clicks == 1 ? "" : "s");
            SetDlgItemTextA(hwnd, ID_LABEL, text);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case ID_FILE_RESET:
            clicks = 0;
            phase = 0;
            SetDlgItemTextA(hwnd, ID_LABEL, "No clicks yet");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case ID_FILE_EXIT: DestroyWindow(hwnd); return 0;
        case ID_HELP_ABOUT:
            MessageBoxA(hwnd, "A Win32 program running on the SynthOS desktop.", "About", MB_OK);
            return 0;
        }
        break;
    case WM_TIMER:
        if (IsDlgButtonChecked(hwnd, ID_ANIMATE)) {
            phase++;
            RECT area;
            GetClientRect(hwnd, &area);
            area.left = 250;
            InvalidateRect(hwnd, &area, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    WNDCLASSEXA wc = {sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "SynthDemo";
    RegisterClassExA(&wc);

    HMENU bar = CreateMenu(), file = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, ID_FILE_RESET, "&Reset\tCtrl+R");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, ID_FILE_EXIT, "E&xit");
    AppendMenuA(help, MF_STRING, ID_HELP_ABOUT, "&About Win32 demo");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");

    HWND hwnd = CreateWindowExA(0, "SynthDemo", "Win32 demo", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                640, 360, NULL, bar, inst, NULL);
    if (!hwnd) {
        puts("winwindow needs the SynthOS desktop");
        return 1;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
