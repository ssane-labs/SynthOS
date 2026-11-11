#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <dwrite_3.h>

/* Chromium draws text with DirectWrite: a 64-bit program walks the built-in fonts and rasterizes a glyph run */
typedef HRESULT(WINAPI *create_fn)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

int main()
{
    create_fn create = (create_fn)(void *)GetProcAddress(LoadLibraryA("dwrite.dll"), "DWriteCreateFactory");
    IDWriteFactory *factory = NULL;
    HRESULT hr = create ? create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&factory) : E_FAIL;
    if (FAILED(hr) || !factory) {
        printf("dwrite factory failed %lx\n", (unsigned long)hr);
        return 1;
    }
    IDWriteFontCollection *fonts = NULL;
    factory->GetSystemFontCollection(&fonts, FALSE);
    UINT32 index = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(L"Arial", &index, &exists);
    IDWriteFontFamily *family = NULL;
    fonts->GetFontFamily(exists ? index : 0, &family);
    IDWriteLocalizedStrings *names = NULL;
    family->GetFamilyNames(&names);
    WCHAR name[64] = L"";
    names->GetString(0, name, 64);
    IDWriteFont *font = NULL;
    family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font);
    IDWriteFontFace *face = NULL;
    font->CreateFontFace(&face);
    UINT32 codepoints[2] = {'A', 'g'};
    UINT16 glyphs[2] = {0, 0};
    face->GetGlyphIndices(codepoints, 2, glyphs);
    DWRITE_GLYPH_METRICS gm[2];
    face->GetDesignGlyphMetrics(glyphs, 2, gm, FALSE);
    DWRITE_FONT_METRICS fm;
    face->GetMetrics(&fm);
    const void *data = NULL;
    UINT32 size = 0;
    void *context = NULL;
    BOOL has = FALSE;
    face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('h', 'e', 'a', 'd'), &data, &size, &context, &has);
    UINT32 nfiles = 1;
    IDWriteFontFile *file = NULL;
    face->GetFiles(&nfiles, &file);
    const void *key = NULL;
    UINT32 key_size = 0;
    file->GetReferenceKey(&key, &key_size);
    IDWriteFontFileLoader *loader = NULL;
    file->GetLoader(&loader);
    IDWriteLocalFontFileLoader *local = NULL;
    loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void **)&local);
    WCHAR path[MAX_PATH] = L"";
    if (local)
        local->GetFilePathFromKey(key, key_size, path, MAX_PATH);
    FLOAT advances[2] = {20, 20};
    DWRITE_GLYPH_OFFSET offsets[2] = {};
    DWRITE_GLYPH_RUN run = {face, 32.0f, 2, glyphs, advances, offsets, FALSE, 0};
    IDWriteGlyphRunAnalysis *analysis = NULL;
    factory->CreateGlyphRunAnalysis(&run, 1.0f, NULL, DWRITE_RENDERING_MODE_NATURAL, DWRITE_MEASURING_MODE_NATURAL,
                                    0.0f, 32.0f, &analysis);
    RECT box = {0, 0, 0, 0};
    if (analysis)
        analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &box);
    IDWriteFontFace *again = NULL;
    IDWriteFont *back = NULL;
    fonts->GetFontFromFontFace(face, &back);
    if (back)
        back->CreateFontFace(&again);
    printf("dwrite families=%d name=%d glyphs=%d advance=%d upem=%d head=%d path=%d raster=%d same=%d\n",
           fonts->GetFontFamilyCount() > 0, name[0] != 0, glyphs[0] && glyphs[1], gm[0].advanceWidth > 0,
           fm.designUnitsPerEm > 0, has && size == 54, wcsstr(path, L".ttf") != NULL, box.right > box.left,
           again != NULL);
    /* Chromium asks for the Windows 10 factory: font fallback and grayscale glyph runs */
    IDWriteFactory2 *f2 = NULL;
    IDWriteFactory3 *f3 = NULL;
    factory->QueryInterface(__uuidof(IDWriteFactory2), (void **)&f2);
    factory->QueryInterface(__uuidof(IDWriteFactory3), (void **)&f3);
    IDWriteFontCollection *fonts3 = NULL;
    if (f3)
        static_cast<IDWriteFactory *>(f3)->GetSystemFontCollection(&fonts3, FALSE);
    IDWriteFontFallback *fallback = NULL;
    if (f2)
        f2->GetSystemFontFallback(&fallback);
    IDWriteGlyphRunAnalysis *gray = NULL;
    if (f2)
        f2->CreateGlyphRunAnalysis(&run, NULL, DWRITE_RENDERING_MODE_NATURAL, DWRITE_MEASURING_MODE_NATURAL,
                                   DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 32.0f,
                                   &gray);
    RECT gbox = {0, 0, 0, 0};
    int levels = 0;
    if (gray && SUCCEEDED(gray->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &gbox)) && gbox.right > gbox.left) {
        UINT32 n = (UINT32)((gbox.right - gbox.left) * (gbox.bottom - gbox.top));
        BYTE *alpha = (BYTE *)HeapAlloc(GetProcessHeap(), 0, n);
        if (alpha && SUCCEEDED(gray->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &gbox, alpha, n)))
            for (UINT32 i = 0; i < n; i++)
                levels += alpha[i] > 0 && alpha[i] < 255;
        HeapFree(GetProcessHeap(), 0, alpha);
    }
    printf("dwrite3 factory2=%d factory3=%d collection=%d fallback=%d gray=%d\n", f2 != NULL, f3 != NULL,
           fonts3 && fonts3->GetFontFamilyCount() > 0, fallback != NULL, levels > 0);
    if (gray)
        gray->Release();
    if (fallback)
        fallback->Release();
    if (fonts3)
        fonts3->Release();
    if (f3)
        f3->Release();
    if (f2)
        f2->Release();
    if (analysis)
        analysis->Release();
    if (again)
        again->Release();
    if (back)
        back->Release();
    if (local)
        local->Release();
    loader->Release();
    file->Release();
    face->Release();
    font->Release();
    names->Release();
    family->Release();
    fonts->Release();
    factory->Release();
    return 0;
}
