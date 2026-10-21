#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <dwrite.h>

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
