#include <synth/string.h>

#include "win32.h"

#define S_OK 0u
#define E_NOTIMPL 0x80004001u
#define DWM_E_COMPOSITIONDISABLED 0x80263001u

static BOOL WINAPI InitCommonControlsEx(const uint32_t *info)
{
    UNUSED(info);
    return TRUE;
}

static void WINAPI InitCommonControls(void)
{
}

/* Image lists keep their size and count; the controls that would draw them paint without images. */
struct image_list {
    int cx, cy, count;
};

static struct image_list *WINAPI ImageList_Create(int cx, int cy, UINT flags, int initial, int grow)
{
    UNUSED(flags);
    UNUSED(initial);
    UNUSED(grow);
    struct image_list *il = w32_alloc(sizeof(*il));
    if (il)
        *il = (struct image_list){MAX(cx, 1), MAX(cy, 1), 0};
    return il;
}

static int WINAPI ImageList_Add(struct image_list *il, HANDLE bitmap, HANDLE mask)
{
    UNUSED(mask);
    if (!il)
        return -1;
    int(WINAPI * get_object)(HANDLE, int, void *) =
        (int(WINAPI *)(HANDLE, int, void *))w32_builtin_lookup("gdi32.dll", "GetObjectA");
    int32_t bm[6] = {0};
    int n = get_object && bitmap && get_object(bitmap, sizeof(bm), bm) ? MAX(bm[1] / il->cx, 1) : 1;
    il->count += n;
    return il->count - n;
}

static int WINAPI ImageList_AddMasked(struct image_list *il, HANDLE bitmap, DWORD color)
{
    UNUSED(color);
    return ImageList_Add(il, bitmap, NULL);
}

static int WINAPI ImageList_ReplaceIcon(struct image_list *il, int index, HANDLE icon)
{
    UNUSED(icon);
    if (!il || index >= il->count)
        return -1;
    return index >= 0 ? index : il->count++;
}

static int WINAPI ImageList_GetImageCount(struct image_list *il)
{
    return il ? il->count : 0;
}

static BOOL WINAPI ImageList_Remove(struct image_list *il, int index)
{
    if (!il || index >= il->count)
        return FALSE;
    il->count = index < 0 ? 0 : il->count - 1;
    return TRUE;
}

static BOOL WINAPI ImageList_GetIconSize(struct image_list *il, int *cx, int *cy)
{
    if (!il)
        return FALSE;
    if (cx)
        *cx = il->cx;
    if (cy)
        *cy = il->cy;
    return TRUE;
}

static BOOL WINAPI ImageList_Destroy(struct image_list *il)
{
    if (!il)
        return FALSE;
    w32_free(il);
    return TRUE;
}

static const struct w32_export comctl32_exports[] = {
    W32_FN(InitCommonControlsEx),    W32_FN(InitCommonControls),       W32_FN(ImageList_Create),
    W32_FN(ImageList_Add),           W32_FN(ImageList_AddMasked),      W32_FN(ImageList_ReplaceIcon),
    W32_FN(ImageList_GetImageCount), W32_FN(ImageList_Remove),         W32_FN(ImageList_GetIconSize),
    W32_FN(ImageList_Destroy),       W32_AS("ImageList_SetBkColor", w32_zero2),
    W32_AS("ImageList_Draw", w32_one6),
};

static uint32_t WINAPI DwmIsCompositionEnabled(BOOL *enabled)
{
    *enabled = FALSE;
    return S_OK;
}

static uint32_t WINAPI DwmGetWindowAttribute(HANDLE hwnd, DWORD attr, void *value, DWORD size)
{
    UNUSED(hwnd);
    UNUSED(attr);
    if (value)
        memset(value, 0, size);
    return E_NOTIMPL;
}

static BOOL WINAPI DwmDefWindowProc(HANDLE hwnd, UINT msg, uintptr_t wp, intptr_t lp, intptr_t *result)
{
    UNUSED(hwnd);
    UNUSED(msg);
    UNUSED(wp);
    UNUSED(lp);
    if (result)
        *result = 0;
    return FALSE;
}

static uint32_t WINAPI DwmGetCompositionTimingInfo(HANDLE hwnd, void *info)
{
    UNUSED(hwnd);
    UNUSED(info);
    return DWM_E_COMPOSITIONDISABLED;
}

static const struct w32_export dwmapi_exports[] = {
    W32_FN(DwmIsCompositionEnabled),
    W32_FN(DwmGetWindowAttribute),
    W32_AS("DwmSetWindowAttribute", w32_zero4),
    W32_AS("DwmExtendFrameIntoClientArea", w32_zero2),
    W32_FN(DwmDefWindowProc),
    W32_FN(DwmGetCompositionTimingInfo),
    W32_AS("DwmFlush", w32_zero0),
    W32_AS("DwmEnableBlurBehindWindow", w32_zero2),
};

/* One console session: numbers for the id and state classes, strings for the rest. */
static BOOL WINAPI WTSQuerySessionInformationW(HANDLE server, DWORD session, int cls, WCHAR **buf, DWORD *size)
{
    UNUSED(server);
    const char *text = cls == 5 ? (w32_getenv("USERNAME") ? w32_getenv("USERNAME") : "root")
                     : cls == 6 ? "Console" : cls == 7 ? "SYNTHOS" : "";
    bool number = cls == 4 || cls == 8;
    size_t n = number ? 4 : (strlen(text) + 1) * sizeof(WCHAR);
    uint8_t *out = w32_zalloc(MAX(n, (size_t)8));
    if (!out)
        return FALSE;
    if (cls == 4) {
        uint32_t id = session == 0xffffffffu ? 1 : session;
        memcpy(out, &id, 4);
    } else if (!number) {
        for (size_t i = 0; text[i]; i++)
            ((WCHAR *)out)[i] = (uint8_t)text[i];
    }
    *buf = (WCHAR *)out;
    if (size)
        *size = (DWORD)n;
    return TRUE;
}

static BOOL WINAPI WTSEnumerateSessionsW(HANDLE server, DWORD reserved, DWORD version, uint32_t **out, DWORD *count)
{
    UNUSED(server);
    UNUSED(reserved);
    UNUSED(version);
    static const WCHAR console[] = {'C', 'o', 'n', 's', 'o', 'l', 'e', 0};
    uint32_t *info = w32_zalloc(12 + sizeof(console));
    if (!info)
        return FALSE;
    memcpy(info + 3, console, sizeof(console));
    info[0] = 1;
    info[1] = (uint32_t)(uintptr_t)(info + 3);
    *out = info;
    *count = 1;
    return TRUE;
}

static void WINAPI WTSFreeMemory(void *p)
{
    w32_free(p);
}

static const struct w32_export wtsapi32_exports[] = {
    W32_FN(WTSQuerySessionInformationW),
    W32_FN(WTSFreeMemory),
    W32_FN(WTSEnumerateSessionsW),
    W32_AS("WTSRegisterSessionNotification", w32_one2),
    W32_AS("WTSUnRegisterSessionNotification", w32_one1),
};

static UINT WINAPI ImmGetIMEFileNameW(HANDLE layout, WCHAR *out, UINT size)
{
    UNUSED(layout);
    if (out && size)
        out[0] = 0;
    return 0;
}

static const struct w32_export imm32_exports[] = {
    W32_FN(ImmGetIMEFileNameW),
    W32_AS("ImmGetContext", w32_zero1),
    W32_AS("ImmReleaseContext", w32_one2),
    W32_AS("ImmAssociateContextEx", w32_one3),
    W32_AS("ImmSetCompositionWindow", w32_one2),
    W32_AS("ImmSetCandidateWindow", w32_one2),
    W32_AS("ImmGetCompositionStringW", w32_zero4),
    W32_AS("ImmNotifyIME", w32_zero4),
    W32_AS("ImmGetOpenStatus", w32_zero1),
    W32_AS("ImmIsIME", w32_zero1),
    W32_AS("ImmDisableIME", w32_one1),
};

#define DXGI_ERROR_UNSUPPORTED 0x887a0004u

static uint32_t WINAPI CreateDXGIFactory(const void *iid, void **out)
{
    UNUSED(iid);
    *out = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static uint32_t WINAPI CreateDXGIFactory2(UINT flags, const void *iid, void **out)
{
    UNUSED(flags);
    return CreateDXGIFactory(iid, out);
}

static const struct w32_export dxgi_exports[] = {
    W32_FN(CreateDXGIFactory),
    W32_AS("CreateDXGIFactory1", CreateDXGIFactory),
    W32_FN(CreateDXGIFactory2),
    W32_AS("DXGIGetDebugInterface1", CreateDXGIFactory2),
};

static uint32_t WINAPI D3D11CreateDevice(void *adapter, int driver, HANDLE software, UINT flags, const void *levels,
                                         UINT count, UINT sdk, void **device, void *level, void **context)
{
    UNUSED(adapter);
    UNUSED(driver);
    UNUSED(software);
    UNUSED(flags);
    UNUSED(levels);
    UNUSED(count);
    UNUSED(sdk);
    UNUSED(level);
    if (device)
        *device = NULL;
    if (context)
        *context = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static uint32_t WINAPI D3D12CreateDevice(void *adapter, int level, const void *iid, void **device)
{
    UNUSED(adapter);
    UNUSED(level);
    UNUSED(iid);
    if (device)
        *device = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static uint32_t WINAPI DXCoreCreateAdapterFactory(const void *iid, void **out)
{
    return CreateDXGIFactory(iid, out);
}

static const struct w32_export d3d11_exports[] = {
    W32_FN(D3D11CreateDevice),
};

static const struct w32_export d3d12_exports[] = {
    W32_FN(D3D12CreateDevice),
    W32_AS("D3D12GetDebugInterface", CreateDXGIFactory),
};

static const struct w32_export dxcore_exports[] = {
    W32_FN(DXCoreCreateAdapterFactory),
};

static const char *const d3d12_ordinals[102] = {[101] = "D3D12CreateDevice"};

W32_EXPORTS(comctl32_module, "comctl32.dll", comctl32_exports);
W32_EXPORTS(dwmapi_module, "dwmapi.dll", dwmapi_exports);
W32_EXPORTS(wtsapi32_module, "wtsapi32.dll", wtsapi32_exports);
W32_EXPORTS(imm32_module, "imm32.dll", imm32_exports);
W32_EXPORTS(dxgi_module, "dxgi.dll", dxgi_exports);
W32_EXPORTS(d3d11_module, "d3d11.dll", d3d11_exports);
W32_EXPORTS(d3d12_module, "d3d12.dll", d3d12_exports, .ordinals = d3d12_ordinals,
            .ordinal_count = ARRAY_SIZE(d3d12_ordinals));
W32_EXPORTS(dxcore_module, "dxcore.dll", dxcore_exports);
