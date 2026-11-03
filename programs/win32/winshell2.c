/* Volumes and DOS devices, shell file information, the window menu and mouse tracking. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>

static void volume_test(void)
{
    WCHAR vol[64], first[64], names[16];
    DWORD need = 0;
    BOOL ok = GetVolumeNameForVolumeMountPointW(L"C:\\", vol, 64);
    HANDLE f = FindFirstVolumeW(first, 64);
    BOOL more = FindNextVolumeW(f, first, 64);
    DWORD err = GetLastError();
    FindVolumeClose(f);
    BOOL paths = GetVolumePathNamesForVolumeNameW(vol, names, 16, &need);
    printf("volume ok=%d prefix=%d same=%d next=%d/%lu paths=%d %ls need=%lu\n", ok, !wcsncmp(vol, L"\\\\?\\Volume{", 11),
           !wcscmp(vol, first), more, err, paths, names, need);
    char dev[64];
    DWORD n = QueryDosDeviceA("C:", dev, sizeof(dev));
    WCHAR all[64];
    DWORD m = QueryDosDeviceW(NULL, all, 64);
    printf("dosdevice %s n=%lu list=%ls bad=%lu\n", dev, n, all, QueryDosDeviceA("Q:", dev, sizeof(dev)));
}

static void shell_test(void)
{
    SHFILEINFOA fi;
    memset(&fi, 0, sizeof(fi));
    DWORD_PTR r = SHGetFileInfoA("C:\\Windows", 0, &fi, sizeof(fi), SHGFI_DISPLAYNAME | SHGFI_TYPENAME | SHGFI_ATTRIBUTES);
    printf("fileinfo r=%d name=%s type=%s folder=%d\n", (int)r, fi.szDisplayName, fi.szTypeName,
           !!(fi.dwAttributes & 0x20000000));
    memset(&fi, 0, sizeof(fi));
    SHGetFileInfoA("notes.txt", FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_ICON);
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    DWORD_PTR exe = SHGetFileInfoA(self, 0, NULL, 0, SHGFI_EXETYPE);
    printf("fileinfo type=%s icon=%d exe=%s missing=%d\n", fi.szTypeName, fi.hIcon != NULL,
           LOWORD(exe) == 0x4550 ? "PE" : "?", (int)SHGetFileInfoA("C:\\nope.bin", 0, &fi, sizeof(fi), SHGFI_TYPENAME));
}

static int leaves, hovers;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSELEAVE)
        leaves++;
    if (m == WM_MOUSEHOVER)
        hovers++;
    return DefWindowProcA(h, m, w, l);
}

static void window_test(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = "shell2";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "shell2", "t", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    HMENU sys = GetSystemMenu(h, FALSE);
    int count = GetMenuItemCount(sys);
    char close[32] = "";
    GetMenuStringA(sys, SC_CLOSE, close, sizeof(close), MF_BYCOMMAND);
    char *tab = strchr(close, '\t');
    if (tab)
        *tab = 0;
    AppendMenuA(sys, MF_STRING, 0x100, "About");
    BOOL same = GetSystemMenu(h, FALSE) == sys;
    GetSystemMenu(h, TRUE);
    printf("sysmenu count=%d close=%s same=%d reverted=%d\n", count, close, same, GetMenuItemCount(GetSystemMenu(h, FALSE)));

    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE | TME_HOVER, h, 20};
    BOOL tracked = TrackMouseEvent(&t);
    MSG msg;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 300)
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
    TRACKMOUSEEVENT q = {sizeof(q), TME_QUERY};
    TrackMouseEvent(&q);
    printf("track ok=%d leave=%d hover=%d query=%lx\n", tracked, leaves, hovers, q.dwFlags);
    DestroyWindow(h);
}

int main(void)
{
    volume_test();
    shell_test();
    window_test();
    return 0;
}
