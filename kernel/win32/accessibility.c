#include <synth/string.h>

#include "win32.h"

#define E_FAIL 0x80004005u
#define E_NOTIMPL 0x80004001u

static uint32_t WINAPI AccessibleChildren(void *acc, LONG start, LONG count, void *children, LONG *obtained)
{
    UNUSED(acc);
    UNUSED(start);
    UNUSED(count);
    UNUSED(children);
    if (obtained)
        *obtained = 0;
    return E_FAIL;
}

static uint32_t WINAPI AccessibleObjectFromWindow(HANDLE hwnd, DWORD id, const void *iid, void **out)
{
    UNUSED(hwnd);
    UNUSED(id);
    UNUSED(iid);
    if (out)
        *out = NULL;
    return E_FAIL;
}

static uint32_t WINAPI WindowFromAccessibleObject(void *acc, HANDLE *hwnd)
{
    UNUSED(acc);
    if (hwnd)
        *hwnd = NULL;
    return E_FAIL;
}

static uint32_t WINAPI UiaGetReservedValue(void **out)
{
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI UiaHostProviderFromHwnd(HANDLE hwnd, void **out)
{
    UNUSED(hwnd);
    return UiaGetReservedValue(out);
}

static const struct w32_export oleacc_exports[] = {
    W32_FN(AccessibleChildren),
    W32_FN(AccessibleObjectFromWindow),
    W32_AS("CreateStdAccessibleObject", AccessibleObjectFromWindow),
    W32_AS("LresultFromObject", w32_zero3),
    W32_AS("ObjectFromLresult", AccessibleObjectFromWindow),
    W32_FN(WindowFromAccessibleObject),
};

static const struct w32_export uia_exports[] = {
    W32_AS("UiaClientsAreListening", w32_zero0),
    W32_AS("UiaDisconnectAllProviders", w32_zero0),
    W32_AS("UiaDisconnectProvider", w32_zero1),
    W32_AS("UiaGetReservedMixedAttributeValue", UiaGetReservedValue),
    W32_AS("UiaGetReservedNotSupportedValue", UiaGetReservedValue),
    W32_FN(UiaHostProviderFromHwnd),
    W32_AS("UiaRaiseAutomationEvent", w32_zero2),
    W32_AS("UiaRaiseAutomationPropertyChangedEvent", w32_zero10),
    W32_AS("UiaRaiseStructureChangedEvent", w32_zero4),
    W32_AS("UiaRaiseNotificationEvent", w32_zero5),
    W32_AS("UiaReturnRawElementProvider", w32_zero4),
};

W32_EXPORTS(oleacc_module, "oleacc.dll", oleacc_exports);
W32_EXPORTS(uia_module, "uiautomationcore.dll", uia_exports);
