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

W32_EXPORTS(comctl32_module, "comctl32.dll", comctl32_exports);
W32_EXPORTS(dwmapi_module, "dwmapi.dll", dwmapi_exports);
W32_EXPORTS(wtsapi32_module, "wtsapi32.dll", wtsapi32_exports);
W32_EXPORTS(imm32_module, "imm32.dll", imm32_exports);
