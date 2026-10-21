#include <synth/string.h>

#include "win64.h"

/* 32-bit functions that return a handle or pointer through a pointer: a 64-bit caller's variable is 8 bytes, so
   the dispatcher lets them write a 4-byte slot and stores it whole. These apply whatever DLL they come from. */

#define A(i) (1u << (i))
#define U(v) ((uint32_t)(uintptr_t)(v))

static const struct w64_export out_exports[] = {
    W64_OUT("BCryptOpenAlgorithmProvider", A(0)),
    W64_OUT("ConvertSidToStringSidA", A(1)),
    W64_OUT("ConvertSidToStringSidW", A(1)),
    W64_OUT("ConvertStringSidToSidA", A(1)),
    W64_OUT("ConvertStringSidToSidW", A(1)),
    W64_OUT("ConvertStringSecurityDescriptorToSecurityDescriptorA", A(2)),
    W64_OUT("ConvertStringSecurityDescriptorToSecurityDescriptorW", A(2)),
    W64_OUT("ConvertSecurityDescriptorToStringSecurityDescriptorW", A(3)),
    W64_OUT("CreateEnvironmentBlock", A(0)),
    W64_OUT("CreatePipe", A(0) | A(1)),
    W64_OUT("CryptAcquireContextA", A(0)),
    W64_OUT("CryptAcquireContextW", A(0)),
    W64_OUT("DeriveAppContainerSidFromAppContainerName", A(1)),
    W64_OUT("GetAce", A(2)),
    W64_OUT("GetAcceptExSockaddrs", A(4) | A(6)),
    W64_OUT("GetNamedSecurityInfoW", A(3) | A(4) | A(5) | A(6) | A(7)),
    W64_OUT("GetSecurityInfo", A(3) | A(4) | A(5) | A(6) | A(7)),
    W64_OUT("GetSecurityDescriptorOwner", A(1)),
    W64_OUT("GetSecurityDescriptorGroup", A(1)),
    W64_OUT("GetSecurityDescriptorDacl", A(2)),
    W64_OUT("GetSecurityDescriptorSacl", A(2)),
    W64_OUT("GetThreadDescription", A(1)),
    W64_OUT("GetQueuedCompletionStatus", A(2) | A(3)),
    W64_OUT("HeapQueryInformation", A(4)),
    W64_OUT("InitOnceBeginInitialize", A(3)),
    W64_INOUT("InitializeProcThreadAttributeList", A(3)),
    W64_OUT("NetGetJoinInformation", A(1)),
    W64_OUT("NotifyAddrChange", A(0)),
    W64_OUT("NotifyRouteChange", A(0)),
    W64_OUT("OpenProcessToken", A(2)),
    W64_OUT("OpenThreadToken", A(3)),
    W64_OUT("PathCchSkipRoot", A(1)),
    W64_OUT("PathIsUNCEx", A(1)),
    W64_OUT("PdhOpenQueryA", A(2)),
    W64_OUT("PdhOpenQueryW", A(2)),
    W64_OUT("PowerGetActiveScheme", A(1)),
    W64_OUT("PowerRegisterSuspendResumeNotification", A(2)),
    W64_OUT("PowerSettingRegisterNotification", A(3)),
    W64_OUT("ReadProcessMemory", A(4)),
    W64_OUT("WriteProcessMemory", A(4)),
    W64_OUT("RegCreateKeyA", A(2)),
    W64_OUT("RegCreateKeyW", A(2)),
    W64_OUT("RegOpenKeyA", A(2)),
    W64_OUT("RegOpenKeyW", A(2)),
    W64_OUT("RegOpenKeyExA", A(4)),
    W64_OUT("RegOpenKeyExW", A(4)),
    W64_OUT("RegCreateKeyExA", A(7)),
    W64_OUT("RegCreateKeyExW", A(7)),
    W64_INOUT("RpcStringFreeA", A(0)),
    W64_INOUT("RpcStringFreeW", A(0)),
    W64_OUT("RtlPcToFileHeader", A(1)),
    W64_OUT("SHGetKnownFolderPath", A(3)),
    W64_OUT("SearchPathA", A(5)),
    W64_OUT("SearchPathW", A(5)),
    W64_OUT("SetEntriesInAclA", A(3)),
    W64_OUT("SetEntriesInAclW", A(3)),
    W64_OUT("StringFromCLSID", A(1)),
    W64_OUT("StringFromIID", A(1)),
    W64_OUT("UuidToStringA", A(1)),
    W64_OUT("UuidToStringW", A(1)),
    W64_OUT("VerQueryValueA", A(2)),
    W64_OUT("VerQueryValueW", A(2)),
    W64_OUT("WSALookupServiceBeginW", A(2)),
    W64_OUT("WTSQuerySessionInformationW", A(3)),
    W64_OUT("WindowsCreateString", A(2)),
    W64_OUT("WindowsCreateStringReference", A(3)),
    W64_OUT("Wow64DisableWow64FsRedirection", A(0)),
    W64_OUT("WindowFromAccessibleObject", A(1)),
};
W64_EXPORTS(out_module, "*", out_exports);

/* DATA_BLOB: a DWORD size, then the pointer at offset 8 */
static uint32_t *blob_in(uint64_t at, uint32_t *b)
{
    if (!at)
        return NULL;
    const uint8_t *p = w64_ptr(at);
    uint64_t data;
    memcpy(&b[0], p, 4);
    memcpy(&data, p + 8, 8);
    b[1] = (uint32_t)(uintptr_t)w64_ptr(data);
    return b;
}

static void blob_out(uint64_t at, const uint32_t *b)
{
    if (!at)
        return;
    uint8_t *p = w64_ptr(at);
    uint64_t data = b[1];
    memset(p, 0, 16);
    memcpy(p, &b[0], 4);
    memcpy(p + 8, &data, 8);
}

/* in, description, entropy, reserved, prompt, flags, out */
static uint64_t h_protect(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t in[2], entropy[2], out[2] = {0, 0};
    uint32_t ok = (uint32_t)T32(t, U(blob_in(a[0], in)), U(w64_ptr(a[1])), U(blob_in(a[2], entropy)), 0, 0,
                                (uint32_t)a[5], a[6] ? U(out) : 0);
    if (ok)
        blob_out(a[6], out);
    return ok;
}

static uint64_t h_unprotect(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t in[2], entropy[2], out[2] = {0, 0}, desc = 0;
    uint32_t ok = (uint32_t)T32(t, U(blob_in(a[0], in)), a[1] ? U(&desc) : 0, U(blob_in(a[2], entropy)), 0, 0,
                                (uint32_t)a[5], a[6] ? U(out) : 0);
    if (ok) {
        blob_out(a[6], out);
        if (a[1]) {
            uint64_t d = desc;
            memcpy(w64_ptr(a[1]), &d, 8);
        }
    }
    return ok;
}

static const struct w64_export blob_exports[] = {
    W64_FN("CryptProtectData", h_protect),
    W64_FN("CryptUnprotectData", h_unprotect),
};
W64_EXPORTS(blob_module, "*", blob_exports);

/* COM objects built in for 32-bit programs have 32-bit vtables: 64-bit callers are told the class is not there */
#define CLASS_NOT_REGISTERED 0x80040154u

static uint64_t no_object(uint64_t *a, int out, uint32_t result)
{
    if (a[out])
        memset(w64_ptr(a[out]), 0, 8);
    return result;
}

static uint64_t h_no_object0(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 0, CLASS_NOT_REGISTERED);
}

static uint64_t h_no_object1(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 1, CLASS_NOT_REGISTERED);
}

static uint64_t h_no_object2(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 2, CLASS_NOT_REGISTERED);
}

static uint64_t h_no_object3(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 3, CLASS_NOT_REGISTERED);
}

static uint64_t h_no_object4(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 4, CLASS_NOT_REGISTERED);
}

static uint64_t h_no_object5(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 5, CLASS_NOT_REGISTERED);
}

static uint64_t h_no_error_info(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 1, 1);
}

static uint64_t h_no_filter(uint64_t *a, const uint64_t *f, void *t)
{
    return no_object(a, 1, 0);
}

static uint64_t h_no_d3d11(uint64_t *a, const uint64_t *f, void *t)
{
    no_object(a, 7, 0);
    return no_object(a, 9, CLASS_NOT_REGISTERED);
}

static const struct w64_export com_exports[] = {
    W64_FN("SHGetDesktopFolder", h_no_object0),
    W64_FN("SHGetMalloc", h_no_object0),
    W64_FN("CoGetMalloc", h_no_object1),
    W64_FN("CreateDXGIFactory", h_no_object1),
    W64_FN("CreateDXGIFactory1", h_no_object1),
    W64_FN("D3D12GetDebugInterface", h_no_object1),
    W64_FN("DXCoreCreateAdapterFactory", h_no_object1),
    W64_FN("CoCreateFreeThreadedMarshaler", h_no_object1),
    W64_FN("RoActivateInstance", h_no_object1),
    W64_FN("UiaHostProviderFromHwnd", h_no_object1),
    W64_FN("LoadTypeLib", h_no_object1),
    W64_FN("CreateDXGIFactory2", h_no_object2),
    W64_FN("DXGIGetDebugInterface1", h_no_object2),
    W64_FN("CreateStreamOnHGlobal", h_no_object2),
    W64_FN("SHGetPropertyStoreForWindow", h_no_object2),
    W64_FN("RoGetActivationFactory", h_no_object2),
    W64_FN("CoGetInterfaceAndReleaseStream", h_no_object2),
    W64_FN("CoMarshalInterThreadInterfaceInStream", h_no_object2),
    W64_FN("CoUnmarshalInterface", h_no_object2),
    W64_FN("D3D12CreateDevice", h_no_object3),
    W64_FN("SHCreateItemFromParsingName", h_no_object3),
    W64_FN("CoGetObject", h_no_object3),
    W64_FN("AccessibleObjectFromWindow", h_no_object3),
    W64_FN("CreateStdAccessibleObject", h_no_object3),
    W64_FN("CoCreateInstance", h_no_object4),
    W64_FN("CoGetClassObject", h_no_object4),
    W64_FN("LoadRegTypeLib", h_no_object4),
    W64_FN("OleLoadPicturePath", h_no_object5),
    W64_FN("D3D11CreateDevice", h_no_d3d11),
    W64_FN("GetErrorInfo", h_no_error_info),
    W64_FN("CoRegisterMessageFilter", h_no_filter),
};
W64_EXPORTS(com_module, "*", com_exports);
