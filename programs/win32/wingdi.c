/* GDI drawing into memory bitmaps: brushes, pens, shapes, text and blits. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <usp10.h>

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

    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64 * 64 * 4 + 4096, NULL);
    unsigned *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    void *shared_bits = NULL;
    HBITMAP shared = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &shared_bits, section, 4096);
    view[1024 + 7 * 64 + 3] = 0x00123456;
    printf("section dib=%06x\n", shared_bits ? ((unsigned *)shared_bits)[7 * 64 + 3] & 0xffffff : 0);
    DeleteObject(shared);
    UnmapViewOfFile(view);
    CloseHandle(section);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    SIZE size;
    GetTextExtentPoint32A(dc, "Hello", 5, &size);
    TextOutA(dc, 12, 2, "Hi", 2);
    printf("text extent %s, ink %s\n", size.cx > 20 && size.cy > 8 ? "ok" : "bad",
           count(dc, 64, 20, RGB(0, 0, 0)) > 0 ? "ok" : "none");

    WORD index = 0;
    GLYPHMETRICS gm;
    MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    GetGlyphIndicesW(dc, L"A", 1, &index, GGI_MARK_NONEXISTING_GLYPHS);
    DWORD need = GetGlyphOutlineW(dc, index, GGO_GLYPH_INDEX | GGO_GRAY8_BITMAP, &gm, 0, NULL, &mat);
    unsigned char glyph[4096];
    int ink = 0;
    if (need != GDI_ERROR && need <= sizeof(glyph) &&
        GetGlyphOutlineW(dc, index, GGO_GLYPH_INDEX | GGO_GRAY8_BITMAP, &gm, need, glyph, &mat) == need)
        for (DWORD i = 0; i < need; i++)
            ink += glyph[i] == 64;
    struct {
        OUTLINETEXTMETRICW m;
        DWORD guard;
    } otm = {.guard = 0x5a5a5a5a};
    UINT full = GetOutlineTextMetricsW(dc, 0, NULL);
    GetOutlineTextMetricsW(dc, sizeof(otm.m), &otm.m);
    printf("glyph %s, outline metrics %s\n", ink > 4 && gm.gmBlackBoxY > 4 && gm.gmCellIncX > 4 ? "ok" : "bad",
           full > sizeof(otm.m) && otm.guard == 0x5a5a5a5a && otm.m.otmTextMetrics.tmHeight > 0 ? "ok" : "bad");
    const WCHAR *line = L"Hello world";
    SCRIPT_ITEM items[4];
    int nitems = 0, nglyphs = 0, adv[16];
    WORD glyphs[16], clusters[16];
    SCRIPT_VISATTR vis[16];
    SCRIPT_LOGATTR breaks[16];
    GOFFSET offs[16];
    ABC abc;
    SCRIPT_CACHE cache = NULL;
    SIZE wide;
    GetTextExtentPoint32W(dc, line, 11, &wide);
    HRESULT pending = ScriptShape(NULL, &cache, line, 11, 16, &items[0].a, glyphs, clusters, vis, &nglyphs);
    BOOL shaped = ScriptItemize(line, 11, 4, NULL, NULL, items, &nitems) == S_OK &&
                  ScriptShape(dc, &cache, line, 11, 16, &items[0].a, glyphs, clusters, vis, &nglyphs) == S_OK &&
                  ScriptPlace(NULL, &cache, glyphs, nglyphs, vis, &items[0].a, adv, offs, &abc) == S_OK &&
                  ScriptBreak(line, 11, &items[0].a, breaks) == S_OK;
    ScriptFreeCache(&cache);
    printf("uniscribe items=%d glyphs=%d pending=%d width %s break=%d\n", nitems, nglyphs, pending == E_PENDING,
           shaped && (int)abc.abcB == wide.cx ? "ok" : "bad", breaks[6].fSoftBreak + breaks[5].fWhiteSpace);
    DEVMODEW mode;
    memset(&mode, 0x77, sizeof(mode));
    BOOL listed = EnumDisplaySettingsW(NULL, 0, &mode);
    printf("display mode %s\n", listed && mode.dmPelsWidth > 0 && mode.dmBitsPerPel == 32 ? "ok" : "bad");

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
