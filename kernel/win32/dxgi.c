#include <synth/heap.h>
#include <synth/string.h>

#include "win64.h"

/* DXGI without a display driver: one adapter, the Microsoft Basic Render Driver (software), and no outputs. Programs
   that look at the adapter (Chromium blocks GPU access on it and draws with SwiftShader) see a PC whose graphics
   driver is not installed. */

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define DXGI_ERROR_NOT_FOUND 0x887a0002u
#define DXGI_ERROR_UNSUPPORTED 0x887a0004u

#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                                      \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),               \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), a, b, c, d, e, f, g, h}

static const uint8_t iid_unknown[16] = GUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_object[16] = GUID(0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e);
static const uint8_t iid_factory[16] = GUID(0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69);
static const uint8_t iid_factory1[16] =
    GUID(0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87);
static const uint8_t iid_adapter[16] = GUID(0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0);
static const uint8_t iid_adapter1[16] =
    GUID(0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05);

enum { FACTORY, ADAPTER };

static bool kind_has(int kind, const uint8_t *iid)
{
    if (!memcmp(iid, iid_unknown, 16) || !memcmp(iid, iid_object, 16))
        return true;
    if (kind == FACTORY)
        return !memcmp(iid, iid_factory, 16) || !memcmp(iid, iid_factory1, 16);
    return !memcmp(iid, iid_adapter, 16) || !memcmp(iid, iid_adapter1, 16);
}

/* DXGI_ADAPTER_DESC1 for either pointer size: 128 WCHARs, four UINTs, three SIZE_Ts, a LUID and the flags */
static size_t adapter_desc(uint8_t *out, size_t word, bool flags)
{
    static const char name[] = "Microsoft Basic Render Driver";
    memset(out, 0, 256 + 16 + 3 * word + 8 + (flags ? 4 : 0));
    for (size_t i = 0; name[i]; i++)
        out[i * 2] = (uint8_t)name[i];
    const uint32_t ids[4] = {0x1414, 0x8c, 0, 0};
    memcpy(out + 256, ids, sizeof(ids));
    uint64_t shared = 256u << 20;
    memcpy(out + 272 + 2 * word, &shared, word);
    const uint32_t luid[2] = {0x5359, 0};
    memcpy(out + 272 + 3 * word, luid, sizeof(luid));
    if (flags) {
        uint32_t software = 2;
        memcpy(out + 280 + 3 * word, &software, 4);
    }
    return 0;
}

/* CheckInterfaceSupport: the user-mode driver version of a Windows 10 install */
static uint32_t interface_version(uint32_t *version)
{
    if (version) {
        version[0] = 19041u << 16 | 1;
        version[1] = 10u << 16;
    }
    return S_OK;
}

/* 32-bit programs */

struct obj32 {
    const void *const *vtbl;
    int kind;
    struct obj32 *parent;
};

static W32_STATE struct obj32 *factory32, *adapter32;

static uint32_t WINAPI qi32(struct obj32 *self, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = kind_has(self->kind, iid) ? self : NULL;
    return *out ? S_OK : E_NOINTERFACE;
}

static uint32_t WINAPI one32(void *self)
{
    UNUSED(self);
    return 1;
}

static uint32_t WINAPI ok32_3(void *self, uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(self);
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    return S_OK;
}

static uint32_t WINAPI ok32_2(void *self, uint32_t a, uint32_t b)
{
    return ok32_3(self, a, b, 0);
}

static uint32_t WINAPI unsupported32_2(void *self, uint32_t a, void **out)
{
    UNUSED(self);
    UNUSED(a);
    if (out)
        *out = NULL;
    return DXGI_ERROR_UNSUPPORTED;
}

static uint32_t WINAPI unsupported32_3(void *self, uint32_t a, uint32_t b, void **out)
{
    UNUSED(b);
    return unsupported32_2(self, a, out);
}

static uint32_t WINAPI parent32(struct obj32 *self, const uint8_t *iid, void **out)
{
    return self->parent ? qi32(self->parent, iid, out) : unsupported32_2(self, 0, out);
}

static uint32_t WINAPI get_window32(void *self, uint32_t *window)
{
    UNUSED(self);
    *window = 0;
    return S_OK;
}

static uint32_t WINAPI not_found32(void *self, uint32_t index, void **out)
{
    UNUSED(self);
    UNUSED(index);
    *out = NULL;
    return DXGI_ERROR_NOT_FOUND;
}

static uint32_t WINAPI desc32(void *self, uint8_t *out)
{
    UNUSED(self);
    return adapter_desc(out, 4, false);
}

static uint32_t WINAPI desc1_32(void *self, uint8_t *out)
{
    UNUSED(self);
    return adapter_desc(out, 4, true);
}

static uint32_t WINAPI version32(void *self, const void *iid, uint32_t *version)
{
    UNUSED(self);
    UNUSED(iid);
    return interface_version(version);
}

static uint32_t WINAPI true32(void *self)
{
    UNUSED(self);
    return 1;
}

static const void *const adapter_vtbl32[] W32_VTABLE = {
    qi32, one32, one32, ok32_3, ok32_2, ok32_3, parent32, not_found32, desc32, version32, desc1_32,
};

static uint32_t WINAPI enum_adapters32(struct obj32 *self, uint32_t index, void **out)
{
    if (!out)
        return E_POINTER;
    if (index)
        return not_found32(self, index, out);
    if (!adapter32 && (adapter32 = w32_zalloc(sizeof(*adapter32))))
        *adapter32 = (struct obj32){adapter_vtbl32, ADAPTER, self};
    *out = adapter32;
    return adapter32 ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

static const void *const factory_vtbl32[] W32_VTABLE = {
    qi32,           one32,           one32,         ok32_3,          ok32_2,           ok32_3,
    parent32,       enum_adapters32, ok32_2,        get_window32,    unsupported32_3,  unsupported32_2,
    enum_adapters32, true32,
};

static uint32_t WINAPI CreateDXGIFactory(const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!factory32 && (factory32 = w32_zalloc(sizeof(*factory32))))
        *factory32 = (struct obj32){factory_vtbl32, FACTORY, NULL};
    if (!factory32) {
        *out = NULL;
        return DXGI_ERROR_UNSUPPORTED;
    }
    return qi32(factory32, iid, out);
}

static uint32_t WINAPI CreateDXGIFactory2(UINT flags, const uint8_t *iid, void **out)
{
    UNUSED(flags);
    return CreateDXGIFactory(iid, out);
}

static uint32_t WINAPI DXGIGetDebugInterface1(UINT flags, const uint8_t *iid, void **out)
{
    UNUSED(flags);
    UNUSED(iid);
    *out = NULL;
    return E_NOINTERFACE;
}

static const struct w32_export dxgi_exports[] = {
    W32_FN(CreateDXGIFactory),
    W32_AS("CreateDXGIFactory1", CreateDXGIFactory),
    W32_FN(CreateDXGIFactory2),
    W32_FN(DXGIGetDebugInterface1),
};

W32_EXPORTS(dxgi_module, "dxgi.dll", dxgi_exports);

/* 64-bit programs: d3d11_fp.c */
