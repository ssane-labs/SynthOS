#include <synth/crypto.h>
#include <synth/fmt.h>
#include <synth/string.h>
#include <synth/tls.h>

#include "win32.h"

#define E_NOTIMPL 0x80004001u
#define REGDB_E_CLASSNOTREG 0x80040154u
#define CRYPT_E_NOT_FOUND 0x80092004u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813

static DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR path, DWORD *handle)
{
    UNUSED(path);
    if (handle)
        *handle = 0;
    w32_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return 0;
}

static BOOL WINAPI GetFileVersionInfoW(LPCWSTR path, DWORD handle, DWORD len, void *data)
{
    UNUSED(path);
    UNUSED(handle);
    UNUSED(len);
    UNUSED(data);
    w32_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return FALSE;
}

static BOOL WINAPI VerQueryValueW(const void *block, LPCWSTR sub, void **out, UINT *len)
{
    UNUSED(block);
    UNUSED(sub);
    if (out)
        *out = NULL;
    if (len)
        *len = 0;
    return FALSE;
}

static const struct w32_export version_exports[] = {
    W32_FN(GetFileVersionInfoSizeW), W32_AS("GetFileVersionInfoSizeA", GetFileVersionInfoSizeW),
    W32_FN(GetFileVersionInfoW),     W32_AS("GetFileVersionInfoA", GetFileVersionInfoW),
    W32_AS("GetFileVersionInfoSizeExW", w32_zero3), W32_AS("GetFileVersionInfoExW", w32_zero5),
    W32_FN(VerQueryValueW),          W32_AS("VerQueryValueA", VerQueryValueW),
};

static LONG WINAPI BCryptGenRandom(HANDLE alg, BYTE *buf, ULONG len, ULONG flags)
{
    UNUSED(alg);
    UNUSED(flags);
    random_bytes(buf, len);
    return 0;
}

static LONG WINAPI BCryptOpenAlgorithmProvider(HANDLE *alg, LPCWSTR id, LPCWSTR impl, ULONG flags)
{
    UNUSED(id);
    UNUSED(impl);
    UNUSED(flags);
    *alg = (HANDLE)(uintptr_t)0x5bc00001u;
    return 0;
}

static const struct w32_export bcrypt_exports[] = {
    W32_FN(BCryptGenRandom),
    W32_FN(BCryptOpenAlgorithmProvider),
    W32_AS("BCryptCloseAlgorithmProvider", w32_zero2),
};

struct cert_context {
    DWORD encoding;
    const BYTE *data;
    DWORD size;
    void *info;
    HANDLE store;
};

struct cert_store {
    uint32_t magic;
    int count;
    struct cert_context certs[];
};

#define STORE_MAGIC 0x53435254u

static HANDLE WINAPI CertOpenStore(const char *provider, DWORD encoding, HANDLE prov, DWORD flags, const void *para)
{
    UNUSED(provider);
    UNUSED(encoding);
    UNUSED(prov);
    UNUSED(flags);
    UNUSED(para);
    int n = tls_roots_count();
    struct cert_store *s = w32_zalloc(sizeof(*s) + (size_t)n * sizeof(struct cert_context));
    if (!s)
        return NULL;
    s->magic = STORE_MAGIC;
    for (int i = 0; i < n; i++) {
        const struct x509 *c = tls_root(i);
        s->certs[s->count++] = (struct cert_context){1, c->raw, (DWORD)c->raw_len, NULL, s};
    }
    return s;
}

static HANDLE WINAPI CertOpenSystemStoreW(HANDLE prov, LPCWSTR name)
{
    UNUSED(name);
    return CertOpenStore(NULL, 0, prov, 0, NULL);
}

static const struct cert_context *WINAPI CertEnumCertificatesInStore(HANDLE store, const struct cert_context *prev)
{
    struct cert_store *s = store;
    if (!s || s->magic != STORE_MAGIC)
        return NULL;
    int next = prev ? (int)(prev - s->certs) + 1 : 0;
    if (next < 0 || next >= s->count) {
        w32_set_error(CRYPT_E_NOT_FOUND);
        return NULL;
    }
    return &s->certs[next];
}

static BOOL WINAPI CertCloseStore(HANDLE store, DWORD flags)
{
    UNUSED(flags);
    struct cert_store *s = store;
    if (s && s->magic == STORE_MAGIC) {
        s->magic = 0;
        w32_free(s);
    }
    return TRUE;
}

static BOOL WINAPI CertGetEnhancedKeyUsage(const void *ctx, DWORD flags, void *usage, DWORD *size)
{
    UNUSED(ctx);
    UNUSED(flags);
    UNUSED(usage);
    UNUSED(size);
    w32_set_error(CRYPT_E_NOT_FOUND);
    return FALSE;
}

static const void *WINAPI CertEnumCRLsInStore(HANDLE store, const void *prev)
{
    UNUSED(store);
    UNUSED(prev);
    w32_set_error(CRYPT_E_NOT_FOUND);
    return NULL;
}

static const struct w32_export crypt32_exports[] = {
    W32_FN(CertOpenStore),
    W32_FN(CertOpenSystemStoreW),
    W32_AS("CertOpenSystemStoreA", CertOpenSystemStoreW),
    W32_FN(CertEnumCertificatesInStore),
    W32_FN(CertCloseStore),
    W32_FN(CertGetEnhancedKeyUsage),
    W32_FN(CertEnumCRLsInStore),
    W32_AS("CertFreeCertificateContext", w32_one1),
    W32_AS("CertFreeCRLContext", w32_one1),
    W32_AS("CertAddStoreToCollection", w32_one4),
};

static uint32_t WINAPI not_supported1(uint32_t a)
{
    UNUSED(a);
    return ERROR_NOT_SUPPORTED;
}

static uint32_t WINAPI not_supported2(uint32_t a, uint32_t b)
{
    UNUSED(a);
    UNUSED(b);
    return ERROR_NOT_SUPPORTED;
}

static uint32_t WINAPI not_supported3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    return ERROR_NOT_SUPPORTED;
}

static uint32_t WINAPI not_supported5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    return ERROR_NOT_SUPPORTED;
}

static const struct w32_export iphlpapi_exports[] = {
    W32_AS("if_nametoindex", w32_zero1),
    W32_AS("if_indextoname", w32_zero2),
    W32_AS("FreeMibTable", w32_zero1),
    W32_AS("GetIfTable2Ex", not_supported2),
    W32_AS("GetIfTable2", not_supported1),
    W32_AS("ConvertInterfaceLuidToNameW", not_supported3),
    W32_AS("GetAdaptersAddresses", not_supported5),
    W32_AS("GetAdaptersInfo", not_supported2),
    W32_AS("GetNetworkParams", not_supported2),
    W32_AS("GetBestInterface", not_supported2),
};

static LONG WINAPI UuidCreate(BYTE *uuid)
{
    random_bytes(uuid, 16);
    uuid[7] = (BYTE)((uuid[7] & 0x0f) | 0x40);
    uuid[8] = (BYTE)((uuid[8] & 0x3f) | 0x80);
    return 0;
}

static LONG WINAPI UuidToStringW(const BYTE *u, WCHAR **out)
{
    char text[40];
    ksnprintf(text, sizeof(text), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              (unsigned)(u[0] | u[1] << 8 | u[2] << 16 | (uint32_t)u[3] << 24), (unsigned)(u[4] | u[5] << 8),
              (unsigned)(u[6] | u[7] << 8), u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    WCHAR *w = w32_alloc(sizeof(WCHAR) * 37);
    if (!w)
        return 14;
    for (int i = 0; i < 37; i++)
        w[i] = (WCHAR)(uint8_t)text[i];
    *out = w;
    return 0;
}

static int hexval(WCHAR c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static LONG WINAPI UuidFromStringW(const WCHAR *s, BYTE *u)
{
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    BYTE raw[16];
    int n = 0;
    for (int i = 0; s && s[i] && n < 32; i++) {
        if (s[i] == '-' || s[i] == '{' || s[i] == '}')
            continue;
        int v = hexval(s[i]);
        if (v < 0)
            return 1705;
        if (n % 2 == 0)
            raw[n / 2] = (BYTE)(v << 4);
        else
            raw[n / 2] |= (BYTE)v;
        n++;
    }
    if (n != 32)
        return 1705;
    for (int i = 0; i < 16; i++)
        u[order[i]] = raw[i];
    return 0;
}

static LONG WINAPI RpcStringFreeW(WCHAR **s)
{
    if (s && *s) {
        w32_free(*s);
        *s = NULL;
    }
    return 0;
}

static const struct w32_export rpcrt4_exports[] = {
    W32_FN(UuidCreate),
    W32_AS("UuidCreateSequential", UuidCreate),
    W32_FN(UuidToStringW),
    W32_FN(UuidFromStringW),
    W32_FN(RpcStringFreeW),
    W32_AS("RpcStringFreeA", RpcStringFreeW),
};

static uint32_t WINAPI com_not_registered(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t out)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    if (out)
        *(void **)(uintptr_t)out = NULL;
    return REGDB_E_CLASSNOTREG;
}

static uint32_t WINAPI com_ok9(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                               uint32_t h, uint32_t i)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    UNUSED(f);
    UNUSED(g);
    UNUSED(h);
    UNUSED(i);
    return 0;
}

static uint32_t WINAPI com_notimpl8(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                                    uint32_t h)
{
    UNUSED(h);
    return com_ok9(a, b, c, d, e, f, g, 0, 0) | E_NOTIMPL;
}

static uint32_t WINAPI not_implemented2(uint32_t a, uint32_t b)
{
    UNUSED(a);
    UNUSED(b);
    return E_NOTIMPL;
}

static void *WINAPI CoTaskMemAlloc(SIZE_T n)
{
    return w32_alloc(n ? n : 1);
}

static void WINAPI CoTaskMemFree(void *p)
{
    w32_free(p);
}

static void *WINAPI CoTaskMemRealloc(void *p, SIZE_T n)
{
    return w32_realloc(p, n);
}

static const struct w32_export ole32_exports[] = {
    W32_AS("CoInitialize", w32_zero1),
    W32_AS("CoInitializeEx", w32_zero2),
    W32_AS("CoUninitialize", w32_zero0),
    W32_AS("CoInitializeSecurity", com_ok9),
    W32_AS("CoCreateInstance", com_not_registered),
    W32_AS("CoSetProxyBlanket", com_notimpl8),
    W32_AS("ProgIDFromCLSID", not_implemented2),
    W32_AS("CLSIDFromProgID", not_implemented2),
    W32_AS("StringFromCLSID", not_implemented2),
    W32_AS("CLSIDFromString", not_implemented2),
    W32_FN(CoTaskMemAlloc),
    W32_FN(CoTaskMemFree),
    W32_FN(CoTaskMemRealloc),
};

static WCHAR *WINAPI SysAllocStringLen(const WCHAR *s, UINT n)
{
    uint8_t *b = w32_alloc(4 + (size_t)n * 2 + 2);
    if (!b)
        return NULL;
    *(uint32_t *)b = n * 2;
    WCHAR *w = (WCHAR *)(b + 4);
    if (s)
        memcpy(w, s, (size_t)n * 2);
    else
        memset(w, 0, (size_t)n * 2);
    w[n] = 0;
    return w;
}

static WCHAR *WINAPI SysAllocString(const WCHAR *s)
{
    if (!s)
        return NULL;
    UINT n = 0;
    while (s[n])
        n++;
    return SysAllocStringLen(s, n);
}

static void WINAPI SysFreeString(WCHAR *s)
{
    if (s)
        w32_free((uint8_t *)s - 4);
}

static UINT WINAPI SysStringLen(const WCHAR *s)
{
    return s ? *(const uint32_t *)((const uint8_t *)s - 4) / 2 : 0;
}

static uint32_t WINAPI VariantClear(uint16_t *v)
{
    if (v)
        memset(v, 0, 16);
    return 0;
}

static uint32_t WINAPI GetErrorInfo(uint32_t reserved, void **out)
{
    UNUSED(reserved);
    if (out)
        *out = NULL;
    return 1;
}

static const struct w32_export oleaut32_exports[] = {
    W32_FN(SysAllocString), W32_FN(SysAllocStringLen), W32_FN(SysFreeString),
    W32_FN(SysStringLen),   W32_FN(VariantClear),      W32_AS("VariantInit", VariantClear),
    W32_FN(GetErrorInfo),
};

static const char *const oleaut32_ordinals[] = {
    [2] = "SysAllocString", [4] = "SysAllocStringLen", [6] = "SysFreeString", [7] = "SysStringLen",
    [8] = "VariantInit",    [9] = "VariantClear",      [200] = "GetErrorInfo",
};

static BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(const WCHAR *text, DWORD revision, void **sd,
                                                                       ULONG *size)
{
    UNUSED(text);
    UNUSED(revision);
    uint8_t *d = w32_zalloc(20);
    if (!d)
        return FALSE;
    d[0] = 1;
    d[2] = 0x04;
    d[3] = 0x80;
    *sd = d;
    if (size)
        *size = 20;
    return TRUE;
}

static LONG WINAPI reg_not_found5(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    return ERROR_FILE_NOT_FOUND;
}

static LONG WINAPI reg_not_found3(uint32_t a, uint32_t b, uint32_t c)
{
    return reg_not_found5(a, b, c, 0, 0);
}

static LONG WINAPI reg_not_found4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return reg_not_found5(a, b, c, d, 0);
}

static BOOL WINAPI report_event(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                                uint32_t h, uint32_t i)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    UNUSED(f);
    UNUSED(g);
    UNUSED(h);
    UNUSED(i);
    return TRUE;
}

static const struct w32_export advapi_extra_exports[] = {
    W32_FN(ConvertStringSecurityDescriptorToSecurityDescriptorW),
    W32_AS("ConvertStringSecurityDescriptorToSecurityDescriptorA", ConvertStringSecurityDescriptorToSecurityDescriptorW),
    W32_AS("RegConnectRegistryW", reg_not_found3),
    W32_AS("RegDeleteKeyExW", reg_not_found4),
    W32_AS("RegLoadKeyW", reg_not_found3),
    W32_AS("RegSaveKeyW", reg_not_found3),
    W32_AS("ReportEventW", report_event),
    W32_AS("ReportEventA", report_event),
    W32_AS("RegisterEventSourceW", w32_one2),
    W32_AS("DeregisterEventSource", w32_one1),
};

W32_EXPORTS(version_module, "version.dll", version_exports);
W32_EXPORTS(bcrypt_module, "bcrypt.dll", bcrypt_exports);
W32_EXPORTS(advapi_extra_module, "advapi32.dll", advapi_extra_exports);
W32_EXPORTS(crypt32_module, "crypt32.dll", crypt32_exports);
W32_EXPORTS(iphlpapi_module, "iphlpapi.dll", iphlpapi_exports);
W32_EXPORTS(rpcrt4_module, "rpcrt4.dll", rpcrt4_exports);
W32_EXPORTS(ole32_module, "ole32.dll", ole32_exports);
W32_EXPORTS(oleaut32_module, "oleaut32.dll", oleaut32_exports, .ordinals = oleaut32_ordinals,
            .ordinal_count = ARRAY_SIZE(oleaut32_ordinals));
