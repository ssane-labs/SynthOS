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

static const struct w32_export comctl32_exports[] = {
    W32_FN(InitCommonControlsEx),
    W32_FN(InitCommonControls),
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

static BOOL WINAPI WTSQuerySessionInformationW(HANDLE server, DWORD session, int cls, WCHAR **buf, DWORD *size)
{
    UNUSED(server);
    UNUSED(session);
    UNUSED(cls);
    *buf = w32_zalloc(8);
    if (size)
        *size = *buf ? 8 : 0;
    return *buf != NULL;
}

static void WINAPI WTSFreeMemory(void *p)
{
    w32_free(p);
}

static const struct w32_export wtsapi32_exports[] = {
    W32_FN(WTSQuerySessionInformationW),
    W32_FN(WTSFreeMemory),
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
