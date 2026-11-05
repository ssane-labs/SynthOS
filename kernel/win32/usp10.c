/* usp10 (Uniscribe) for left-to-right text: one item per run, a glyph per UTF-16 unit (the glyph index is the
   character, as GetGlyphIndicesW reports it), advances from the selected font. */
#include <synth/string.h>

#include "w32gui.h"
#include "win32.h"

#define S_OK 0u
#define S_FALSE 1u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define E_PENDING 0x8000000au

typedef BOOL(WINAPI *char_width_fn)(HANDLE, UINT, UINT, int *);
typedef HANDLE(WINAPI *current_fn)(HANDLE, UINT);
typedef HANDLE(WINAPI *dc_fn)(HANDLE);
typedef HANDLE(WINAPI *select_fn)(HANDLE, HANDLE);
typedef BOOL(WINAPI *delete_fn)(HANDLE);
typedef BOOL(WINAPI *metrics_fn)(HANDLE, uint8_t *);
typedef BOOL(WINAPI *text_fn)(HANDLE, int, int, UINT, const RECT *, LPCWSTR, UINT, const int *);

static void *gdi(const char *name)
{
    return w32_builtin_lookup("gdi32.dll", name);
}

#define DEFAULT_FONT ((void *)1)

/* The cache remembers the font, so later calls work without a DC. */
static uint32_t font_dc(HANDLE dc, void **cache, HANDLE *out, HANDLE *temp)
{
    *temp = NULL;
    if (dc) {
        if (cache && !*cache) {
            HANDLE font = ((current_fn)gdi("GetCurrentObject"))(dc, 6);
            *cache = font ? font : DEFAULT_FONT;
        }
        *out = dc;
        return S_OK;
    }
    if (!cache || !*cache)
        return E_PENDING;
    HANDLE mem = ((dc_fn)gdi("CreateCompatibleDC"))(NULL);
    if (!mem)
        return E_OUTOFMEMORY;
    if (*cache != DEFAULT_FONT)
        ((select_fn)gdi("SelectObject"))(mem, *cache);
    *out = *temp = mem;
    return S_OK;
}

static void font_done(HANDLE temp)
{
    if (temp)
        ((delete_fn)gdi("DeleteDC"))(temp);
}

static uint32_t WINAPI ScriptItemize(const WCHAR *text, int n, int max, const void *control, const void *state,
                                     uint8_t *items, int *count)
{
    UNUSED(control);
    UNUSED(state);
    if (!text || n <= 0 || !items || max < 2)
        return E_INVALIDARG;
    memset(items, 0, 16);
    memcpy(items + 8, &n, 4);
    if (count)
        *count = 1;
    return S_OK;
}

static uint32_t WINAPI ScriptLayout(int runs, const uint8_t *levels, int *visual_to_logical, int *logical_to_visual)
{
    if (!levels || runs < 0)
        return E_INVALIDARG;
    for (int i = 0; i < runs; i++) {
        if (visual_to_logical)
            visual_to_logical[i] = i;
        if (logical_to_visual)
            logical_to_visual[i] = i;
    }
    return S_OK;
}

static uint32_t WINAPI ScriptShape(HANDLE dc, void **cache, const WCHAR *text, int n, int max, void *analysis,
                                   WORD *glyphs, WORD *clusters, WORD *attrs, int *count)
{
    UNUSED(analysis);
    if (!text || n <= 0 || !glyphs || !count)
        return E_INVALIDARG;
    if (max < n)
        return E_OUTOFMEMORY;
    HANDLE use, temp;
    uint32_t hr = font_dc(dc, cache, &use, &temp);
    if (hr)
        return hr;
    font_done(temp);
    for (int i = 0; i < n; i++) {
        glyphs[i] = text[i];
        if (clusters)
            clusters[i] = (WORD)i;
        if (attrs)
            attrs[i] = text[i] == 0x200b || text[i] == 0xfeff ? 0x10 | 0x40 : 0x10;
    }
    *count = n;
    return S_OK;
}

static uint32_t WINAPI ScriptPlace(HANDLE dc, void **cache, const WORD *glyphs, int n, const WORD *attrs,
                                   void *analysis, int *advance, int32_t *offsets, int32_t *abc)
{
    UNUSED(attrs);
    UNUSED(analysis);
    if (!glyphs || n <= 0 || !advance)
        return E_INVALIDARG;
    HANDLE use, temp;
    uint32_t hr = font_dc(dc, cache, &use, &temp);
    if (hr)
        return hr;
    char_width_fn width = (char_width_fn)gdi("GetCharWidthW");
    int total = 0;
    for (int i = 0; i < n; i++) {
        int w = 0;
        if (glyphs[i] != 0x200b && glyphs[i] != 0xfeff)
            width(use, glyphs[i], glyphs[i], &w);
        advance[i] = w;
        total += w;
        if (offsets)
            offsets[i * 2] = offsets[i * 2 + 1] = 0;
    }
    font_done(temp);
    if (abc)
        abc[0] = 0, abc[1] = total, abc[2] = 0;
    return S_OK;
}

static uint32_t WINAPI ScriptBreak(const WCHAR *text, int n, const void *analysis, uint8_t *attrs)
{
    UNUSED(analysis);
    if (!text || n <= 0 || !attrs)
        return E_INVALIDARG;
    for (int i = 0; i < n; i++) {
        bool space = text[i] == ' ' || text[i] == '\t' || text[i] == 0x3000;
        bool after = i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t' || text[i - 1] == '-' || text[i - 1] == 0x3000);
        bool low = text[i] >= 0xdc00 && text[i] <= 0xdfff;
        attrs[i] = (uint8_t)((after && !space ? 1 | 8 : 0) | (space ? 2 : 0) | (low ? 0 : 4) | (!i ? 8 : 0));
    }
    return S_OK;
}

static uint32_t WINAPI ScriptGetLogicalWidths(const void *analysis, int chars, int glyphs, const int *widths,
                                              const WORD *clusters, const WORD *attrs, int *dx)
{
    UNUSED(analysis);
    UNUSED(attrs);
    if (!widths || !clusters || !dx || chars <= 0)
        return E_INVALIDARG;
    for (int i = 0; i < chars;) {
        int j = i + 1;
        while (j < chars && clusters[j] == clusters[i])
            j++;
        int g0 = clusters[i], g1 = j < chars ? clusters[j] : glyphs, sum = 0;
        for (int g = g0; g < g1 && g < glyphs; g++)
            sum += widths[g];
        for (int k = i; k < j; k++)
            dx[k] = sum / (j - i) + (k == j - 1 ? sum % (j - i) : 0);
        i = j;
    }
    return S_OK;
}

static uint32_t WINAPI ScriptFreeCache(void **cache)
{
    if (cache)
        *cache = NULL;
    return S_OK;
}

static uint32_t WINAPI ScriptIsComplex(const WCHAR *text, int n, DWORD flags)
{
    UNUSED(flags);
    for (int i = 0; text && i < n; i++)
        if (text[i] >= 0x0590 && text[i] < 0x2000)
            return S_OK;
    return S_FALSE;
}

static uint32_t WINAPI ScriptCacheGetHeight(HANDLE dc, void **cache, int32_t *height)
{
    if (!height)
        return E_INVALIDARG;
    HANDLE use, temp;
    uint32_t hr = font_dc(dc, cache, &use, &temp);
    if (hr)
        return hr;
    uint8_t tm[64];
    ((metrics_fn)gdi("GetTextMetricsW"))(use, tm);
    font_done(temp);
    memcpy(height, tm, 4);
    return S_OK;
}

static uint32_t WINAPI ScriptGetFontProperties(HANDLE dc, void **cache, uint8_t *props)
{
    if (!props)
        return E_INVALIDARG;
    HANDLE use, temp;
    uint32_t hr = font_dc(dc, cache, &use, &temp);
    if (hr)
        return hr;
    font_done(temp);
    WORD glyphs[4] = {' ', '?', 0, 0};
    memcpy(props + 4, glyphs, 8);
    memset(props + 12, 0, 4);
    return S_OK;
}

static uint32_t WINAPI ScriptTextOut(HANDLE dc, void **cache, int x, int y, UINT options, const RECT *rect,
                                     const void *analysis, const WCHAR *reserved, int reserved2, const WORD *glyphs,
                                     int n, const int *advance, const int *justify, const void *offsets)
{
    UNUSED(cache);
    UNUSED(analysis);
    UNUSED(reserved);
    UNUSED(reserved2);
    UNUSED(justify);
    UNUSED(offsets);
    if (!dc || !glyphs || n <= 0)
        return E_INVALIDARG;
    ((text_fn)gdi("ExtTextOutW"))(dc, x, y, options & 6, rect, glyphs, (UINT)n, advance);
    return S_OK;
}

static uint32_t WINAPI ScriptRecordDigitSubstitution(DWORD locale, uint8_t *sub)
{
    UNUSED(locale);
    if (!sub)
        return E_INVALIDARG;
    memset(sub, 0, 12);
    return S_OK;
}

static uint32_t WINAPI ScriptApplyDigitSubstitution(const void *sub, uint8_t *control, uint8_t *state)
{
    UNUSED(sub);
    if (control)
        memset(control, 0, 4);
    if (state)
        memset(state, 0, 2);
    return S_OK;
}

static const struct w32_export usp10_exports[] = {
    W32_FN(ScriptItemize),           W32_FN(ScriptLayout),          W32_FN(ScriptShape),
    W32_FN(ScriptPlace),             W32_FN(ScriptBreak),           W32_FN(ScriptGetLogicalWidths),
    W32_FN(ScriptFreeCache),         W32_FN(ScriptIsComplex),       W32_FN(ScriptCacheGetHeight),
    W32_FN(ScriptGetFontProperties), W32_FN(ScriptTextOut),         W32_FN(ScriptRecordDigitSubstitution),
    W32_FN(ScriptApplyDigitSubstitution),
};

W32_EXPORTS(usp10_module, "usp10.dll", usp10_exports);
