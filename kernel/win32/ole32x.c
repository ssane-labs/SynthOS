#include <synth/crypto.h>
#include <synth/fmt.h>
#include <synth/string.h>

#include "win32.h"

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define RPC_E_CHANGED_MODE 0x80010106u
#define CO_E_NOTINITIALIZED 0x800401f0u
#define CO_E_CLASSSTRING 0x800401f3u
#define REGDB_E_CLASSNOTREG 0x80040154u

static uint32_t init_apartment(uint8_t apartment)
{
    struct w32_thread *t = w32->current;
    if (t->com_inits && t->com_apartment != apartment)
        return RPC_E_CHANGED_MODE;
    t->com_apartment = apartment;
    return t->com_inits++ ? S_FALSE : S_OK;
}

static uint32_t WINAPI CoInitializeEx(void *reserved, DWORD flags)
{
    UNUSED(reserved);
    return init_apartment((flags & 2) ? 1 : 2);
}

static uint32_t WINAPI CoInitialize(void *reserved)
{
    return CoInitializeEx(reserved, 2);
}

static void WINAPI CoUninitialize(void)
{
    struct w32_thread *t = w32->current;
    if (t->com_inits > 0 && --t->com_inits == 0)
        t->com_apartment = 0;
}

static uint32_t WINAPI OleInitialize(void *reserved)
{
    return CoInitializeEx(reserved, 2);
}

static uint32_t WINAPI CoGetApartmentType(int *type, int *qualifier)
{
    if (!type || !qualifier)
        return E_INVALIDARG;
    struct w32_thread *t = w32->current;
    *qualifier = 0;
    if (!t->com_inits) {
        *type = -1;
        return CO_E_NOTINITIALIZED;
    }
    *type = t->com_apartment == 1 ? 0 : 1;
    return S_OK;
}

static uint32_t WINAPI CoCreateGuid(uint8_t *guid)
{
    random_bytes(guid, 16);
    guid[7] = (uint8_t)((guid[7] & 0x0f) | 0x40);
    guid[8] = (uint8_t)((guid[8] & 0x3f) | 0x80);
    return S_OK;
}

static int guid_format(const uint8_t *g, WCHAR *out)
{
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    char buf[40];
    int n = ksnprintf(buf, sizeof(buf), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9],
                      g[10], g[11], g[12], g[13], g[14], g[15]);
    for (int i = 0; i <= n; i++)
        out[i] = (uint8_t)buf[i];
    return n + 1;
}

static int WINAPI StringFromGUID2(const uint8_t *guid, WCHAR *out, int max)
{
    if (max < 39)
        return 0;
    return guid_format(guid, out);
}

static uint32_t WINAPI StringFromCLSID(const uint8_t *guid, WCHAR **out)
{
    *out = w32_alloc(39 * sizeof(WCHAR));
    if (!*out)
        return 0x8007000eu;
    guid_format(guid, *out);
    return S_OK;
}

static int hex_digit(WCHAR c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool hex_field(const WCHAR **p, int digits, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < digits; i++) {
        int d = hex_digit((*p)[i]);
        if (d < 0)
            return false;
        v = v << 4 | (uint32_t)d;
    }
    *p += digits;
    *out = v;
    return true;
}

static uint32_t WINAPI CLSIDFromString(const WCHAR *s, uint8_t *guid)
{
    if (!s || *s != '{') {
        memset(guid, 0, 16);
        return s && !*s ? S_OK : CO_E_CLASSSTRING;
    }
    s++;
    uint32_t d1, d2, d3, b;
    if (!hex_field(&s, 8, &d1) || *s++ != '-' || !hex_field(&s, 4, &d2) || *s++ != '-' || !hex_field(&s, 4, &d3) ||
        *s++ != '-')
        return CO_E_CLASSSTRING;
    uint8_t tail[8];
    for (int i = 0; i < 8; i++) {
        if (i == 2 && *s++ != '-')
            return CO_E_CLASSSTRING;
        if (!hex_field(&s, 2, &b))
            return CO_E_CLASSSTRING;
        tail[i] = (uint8_t)b;
    }
    if (*s != '}')
        return CO_E_CLASSSTRING;
    uint16_t w2 = (uint16_t)d2, w3 = (uint16_t)d3;
    memcpy(guid, &d1, 4);
    memcpy(guid + 4, &w2, 2);
    memcpy(guid + 6, &w3, 2);
    memcpy(guid + 8, tail, 8);
    return S_OK;
}

static uint32_t WINAPI CoRegisterInitializeSpy(void *spy, uint64_t *cookie)
{
    UNUSED(spy);
    *cookie = 1;
    return S_OK;
}

static uint32_t WINAPI CoRegisterMessageFilter(void *filter, void **old)
{
    UNUSED(filter);
    if (old)
        *old = NULL;
    return S_OK;
}

static uint32_t WINAPI no_object2(void *a, void **out)
{
    UNUSED(a);
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI no_object3(void *a, void *b, void **out)
{
    UNUSED(b);
    return no_object2(a, out);
}

static uint32_t WINAPI no_object4(void *a, void *b, void *c, void **out)
{
    UNUSED(c);
    return no_object3(a, b, out);
}

static uint32_t WINAPI DoDragDrop(void *data, void *source, DWORD effects, DWORD *effect)
{
    UNUSED(data);
    UNUSED(source);
    UNUSED(effects);
    if (effect)
        *effect = 0;
    return 0x00040101u;
}

static void com_release(void *obj)
{
    typedef uint32_t WINAPI (*release_fn)(void *);
    if (obj)
        ((release_fn)(*(void ***)obj)[2])(obj);
}

static void com_add_ref(void *obj)
{
    typedef uint32_t WINAPI (*add_ref_fn)(void *);
    if (obj)
        ((add_ref_fn)(*(void ***)obj)[1])(obj);
}

static void WINAPI ReleaseStgMedium(uint32_t *medium)
{
    if (!medium)
        return;
    if (medium[0] == 1) {
        void *(WINAPI * global_free)(void *) =
            (void *(WINAPI *)(void *))w32_builtin_lookup("kernel32.dll", "GlobalFree");
        if (!medium[2] && global_free && medium[1])
            global_free((void *)medium[1]);
    } else if ((medium[0] == 4 || medium[0] == 8) && !medium[2]) {
        com_release((void *)medium[1]);
    }
    com_release((void *)medium[2]);
    memset(medium, 0, 12);
}

static uint32_t WINAPI PropVariantClear(uint16_t *pv)
{
    if (!pv)
        return S_OK;
    void *p = *(void **)(pv + 4);
    switch (pv[0]) {
    case 8:
    case 30:
    case 31:
    case 72:
        if (pv[0] == 8) {
            void(WINAPI * free_bstr)(void *) = (void(WINAPI *)(void *))w32_builtin_lookup("oleaut32.dll", "SysFreeString");
            if (free_bstr)
                free_bstr(p);
        } else {
            w32_free(p);
        }
        break;
    case 9:
    case 13:
        com_release(p);
        break;
    default:
        break;
    }
    memset(pv, 0, 16);
    return S_OK;
}

static uint32_t WINAPI PropVariantCopy(uint16_t *dst, const uint16_t *src)
{
    memcpy(dst, src, 16);
    void *p = *(void *const *)(src + 4);
    if ((src[0] == 31 || src[0] == 30) && p) {
        size_t n = src[0] == 31 ? (w32_wcslen(p) + 1) * sizeof(WCHAR) : strlen(p) + 1;
        void *copy = w32_alloc(n);
        if (!copy)
            return 0x8007000eu;
        memcpy(copy, p, n);
        *(void **)(dst + 4) = copy;
    } else if (src[0] == 8 && p) {
        WCHAR *(WINAPI * dup)(const WCHAR *) =
            (WCHAR * (WINAPI *)(const WCHAR *)) w32_builtin_lookup("oleaut32.dll", "SysAllocString");
        *(void **)(dst + 4) = dup ? dup(p) : NULL;
    } else if (src[0] == 9 || src[0] == 13) {
        com_add_ref(p);
    }
    return S_OK;
}

static const struct w32_export ole32x_exports[] = {
    W32_FN(CoInitialize),
    W32_FN(CoInitializeEx),
    W32_FN(CoUninitialize),
    W32_FN(OleInitialize),
    W32_AS("OleUninitialize", CoUninitialize),
    W32_FN(CoGetApartmentType),
    W32_FN(CoCreateGuid),
    W32_FN(StringFromGUID2),
    W32_FN(StringFromCLSID),
    W32_AS("StringFromIID", StringFromCLSID),
    W32_FN(CLSIDFromString),
    W32_AS("IIDFromString", CLSIDFromString),
    W32_AS("CoAllowSetForegroundWindow", w32_zero2),
    W32_AS("CoCreateFreeThreadedMarshaler", no_object2),
    W32_AS("CoGetInterfaceAndReleaseStream", no_object3),
    W32_AS("CoGetObject", no_object4),
    W32_AS("CoMarshalInterThreadInterfaceInStream", no_object3),
    W32_AS("CoUnmarshalInterface", no_object3),
    W32_FN(CoRegisterInitializeSpy),
    W32_AS("CoRevokeInitializeSpy", w32_zero2),
    W32_FN(CoRegisterMessageFilter),
    W32_AS("CreateILockBytesOnHGlobal", no_object3),
    W32_AS("CreateStreamOnHGlobal", no_object3),
    W32_AS("GetHGlobalFromILockBytes", no_object2),
    W32_AS("GetHGlobalFromStream", no_object2),
    W32_AS("StgCreateDocfileOnILockBytes", no_object4),
    W32_AS("RoGetAgileReference", no_object4),
    W32_FN(DoDragDrop),
    W32_AS("RegisterDragDrop", w32_zero2),
    W32_AS("RevokeDragDrop", w32_zero1),
    W32_AS("OleDuplicateData", w32_zero3),
    W32_FN(ReleaseStgMedium),
    W32_FN(PropVariantClear),
    W32_FN(PropVariantCopy),
};

W32_EXPORTS(ole32x_module, "ole32.dll", ole32x_exports);
