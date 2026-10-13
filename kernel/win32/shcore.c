#include "win32.h"

static DWORD WINAPI GetDpiForMonitor(HANDLE monitor, int type, UINT *x, UINT *y)
{
    UNUSED(monitor);
    UNUSED(type);
    *x = *y = 96;
    return 0;
}

static DWORD WINAPI GetScaleFactorForMonitor(HANDLE monitor, int *scale)
{
    UNUSED(monitor);
    *scale = 100;
    return 0;
}

static DWORD WINAPI GetProcessDpiAwareness(HANDLE process, int *value)
{
    UNUSED(process);
    *value = 2;
    return 0;
}

static const struct w32_export shcore_exports[] = {
    W32_FN(GetDpiForMonitor),
    W32_FN(GetScaleFactorForMonitor),
    W32_FN(GetProcessDpiAwareness),
    W32_AS("SetProcessDpiAwareness", w32_zero1),
};

W32_EXPORTS(shcore_module, "shcore.dll", shcore_exports);
