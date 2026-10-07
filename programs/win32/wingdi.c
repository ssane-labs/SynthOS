/* GDI drawing into memory bitmaps: brushes, pens, shapes, text and blits. */
#include <stdio.h>
#include <windows.h>

static int count(HDC dc, int w, int h, COLORREF c)
{
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            n += GetPixel(dc, x, y) == c;
    return n;
}

int main(void)
{
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 64;
    bmi.bmiHeader.biHeight = -64;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(dc, bmp);

    RECT all = {0, 0, 64, 64};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &all, white);
    printf("white=%d\n", count(dc, 64, 64, RGB(255, 255, 255)));

    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    HPEN none = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(dc, red);
    SelectObject(dc, none);
    Rectangle(dc, 0, 0, 10, 10);
    printf("red rect=%d\n", count(dc, 64, 64, RGB(255, 0, 0)));

    HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
    SelectObject(dc, blue);
    Ellipse(dc, 20, 20, 60, 60);
    int disc = count(dc, 64, 64, RGB(0, 0, 255));
    printf("ellipse %s\n", disc > 1100 && disc < 1400 ? "round" : "wrong");

    HPEN green = CreatePen(PS_SOLID, 1, RGB(0, 128, 0));
    SelectObject(dc, green);
    MoveToEx(dc, 0, 63, NULL);
    LineTo(dc, 64, 63);
    printf("line=%d\n", count(dc, 64, 64, RGB(0, 128, 0)));

    unsigned *px = bits;
    printf("dib pixel=%06x\n", px[5 * 64 + 5] & 0xffffff);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    SIZE size;
    GetTextExtentPoint32A(dc, "Hello", 5, &size);
    TextOutA(dc, 12, 2, "Hi", 2);
    printf("text extent %s, ink %s\n", size.cx > 20 && size.cy > 8 ? "ok" : "bad",
           count(dc, 64, 20, RGB(0, 0, 0)) > 0 ? "ok" : "none");

    HDC copy = CreateCompatibleDC(dc);
    HBITMAP second = CreateCompatibleBitmap(dc, 64, 64);
    SelectObject(copy, second);
    BitBlt(copy, 0, 0, 64, 64, dc, 0, 0, SRCCOPY);
    printf("blit red=%d\n", count(copy, 64, 64, RGB(255, 0, 0)));
    StretchBlt(copy, 0, 0, 32, 32, dc, 0, 0, 64, 64, SRCCOPY);
    printf("stretch red=%d\n", count(copy, 16, 16, RGB(255, 0, 0)));

    BITMAP info;
    GetObject(second, sizeof(info), &info);
    printf("bitmap %ldx%ld %dbpp\n", info.bmWidth, info.bmHeight, info.bmBitsPixel);

    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(copy);
    DeleteDC(dc);
    printf("screen %dx%d\n", GetSystemMetrics(SM_CXSCREEN) > 0, GetSystemMetrics(SM_CYSCREEN) > 0);
    return 0;
}
