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

/* 64-bit programs: d3d11_fp.c on the 64-bit kernel; on the 32-bit kernel the factory and adapter below, which
   have no device behind them (D3D11CreateDevice answers "class not registered", win64out.c) */
#ifndef SYNTH_X32

/* 64-bit programs: the same objects behind vtables of 64-bit stubs */

struct obj64 {
    uint64_t vtbl;
    int kind;
    struct obj64 *parent;
};

static W32_STATE struct obj64 *factory64, *adapter64;

static void put_ptr(uint64_t where, const void *p)
{
    uint64_t v = (uintptr_t)p;
    memcpy(w64_ptr(where), &v, 8);
}

static uint64_t qi64_of(struct obj64 *self, const uint8_t *iid, uint64_t out)
{
    if (!out)
        return E_POINTER;
    bool has = self && kind_has(self->kind, iid);
    put_ptr(out, has ? self : NULL);
    return has ? S_OK : E_NOINTERFACE;
}

static uint64_t h_qi(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    return qi64_of(w64_ptr(a[0]), w64_ptr(a[1]), a[2]);
}

static uint64_t h_one(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(a);
    UNUSED(f);
    UNUSED(t);
    return 1;
}

static uint64_t h_ok(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(a);
    UNUSED(f);
    UNUSED(t);
    return S_OK;
}

static uint64_t h_parent(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    const struct obj64 *self = w64_ptr(a[0]);
    return qi64_of(self->parent, w64_ptr(a[1]), a[2]);
}

static uint64_t h_get_window(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    put_ptr(a[1], NULL);
    return S_OK;
}

static uint64_t h_not_found(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    if (a[2])
        put_ptr(a[2], NULL);
    return DXGI_ERROR_NOT_FOUND;
}

/* CreateSwapChain(device, desc, out) and CreateSoftwareAdapter(module, out) */
static uint64_t h_unsupported(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    if (a[(uintptr_t)t])
        put_ptr(a[(uintptr_t)t], NULL);
    return DXGI_ERROR_UNSUPPORTED;
}

static uint64_t h_desc(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    return adapter_desc(w64_ptr(a[1]), 8, t != NULL);
}

static uint64_t h_version(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    return interface_version(a[2] ? w64_ptr(a[2]) : NULL);
}

static uint64_t h_enum_adapters(uint64_t *a, const uint64_t *f, void *t);

struct method {
    w64_handler fn;
    const char *name;
    void *target;
};

static const struct method adapter_methods[] = {
    {h_qi, "IDXGIAdapter::QueryInterface", NULL}, {h_one, "IDXGIAdapter::AddRef", NULL},
    {h_one, "IDXGIAdapter::Release", NULL},       {h_ok, "IDXGIAdapter::SetPrivateData", NULL},
    {h_ok, "IDXGIAdapter::SetPrivateDataInterface", NULL},
    {h_ok, "IDXGIAdapter::GetPrivateData", NULL}, {h_parent, "IDXGIAdapter::GetParent", NULL},
    {h_not_found, "IDXGIAdapter::EnumOutputs", NULL}, {h_desc, "IDXGIAdapter::GetDesc", NULL},
    {h_version, "IDXGIAdapter::CheckInterfaceSupport", NULL}, {h_desc, "IDXGIAdapter1::GetDesc1", (void *)1},
};

static const struct method factory_methods[] = {
    {h_qi, "IDXGIFactory::QueryInterface", NULL},
    {h_one, "IDXGIFactory::AddRef", NULL},
    {h_one, "IDXGIFactory::Release", NULL},
    {h_ok, "IDXGIFactory::SetPrivateData", NULL},
    {h_ok, "IDXGIFactory::SetPrivateDataInterface", NULL},
    {h_ok, "IDXGIFactory::GetPrivateData", NULL},
    {h_parent, "IDXGIFactory::GetParent", NULL},
    {h_enum_adapters, "IDXGIFactory::EnumAdapters", NULL},
    {h_ok, "IDXGIFactory::MakeWindowAssociation", NULL},
    {h_get_window, "IDXGIFactory::GetWindowAssociation", NULL},
    {h_unsupported, "IDXGIFactory::CreateSwapChain", (void *)3},
    {h_unsupported, "IDXGIFactory::CreateSoftwareAdapter", (void *)2},
    {h_enum_adapters, "IDXGIFactory1::EnumAdapters1", NULL},
    {h_one, "IDXGIFactory1::IsCurrent", NULL},
};

static W32_STATE uint64_t *vtables[2];

static uint64_t vtable(int kind)
{
    const struct method *m = kind == FACTORY ? factory_methods : adapter_methods;
    size_t n = kind == FACTORY ? ARRAY_SIZE(factory_methods) : ARRAY_SIZE(adapter_methods);
    if (!vtables[kind]) {
        uint64_t *v = w32_zalloc(n * sizeof(uint64_t));
        for (size_t i = 0; v && i < n; i++) {
            void *stub = m[i].target ? w64_target_stub(m[i].fn, m[i].target) : w64_kernel_stub(m[i].fn, m[i].name);
            if (!stub)
                return 0;
            v[i] = (uintptr_t)stub;
        }
        vtables[kind] = v;
    }
    return (uintptr_t)vtables[kind];
}

static struct obj64 *object64(struct obj64 **slot, int kind, struct obj64 *parent)
{
    if (!*slot) {
        uint64_t v = vtable(kind);
        if (v && (*slot = w32_zalloc(sizeof(**slot))))
            **slot = (struct obj64){v, kind, parent};
    }
    return *slot;
}

static uint64_t h_enum_adapters(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    if (!a[2])
        return E_POINTER;
    if ((uint32_t)a[1])
        return h_not_found(a, f, t);
    struct obj64 *adapter = object64(&adapter64, ADAPTER, w64_ptr(a[0]));
    put_ptr(a[2], adapter);
    return adapter ? S_OK : DXGI_ERROR_UNSUPPORTED;
}

static uint64_t create_factory(const uint8_t *iid, uint64_t out)
{
    if (!out)
        return E_POINTER;
    struct obj64 *factory = object64(&factory64, FACTORY, NULL);
    if (!factory) {
        put_ptr(out, NULL);
        return DXGI_ERROR_UNSUPPORTED;
    }
    return qi64_of(factory, iid, out);
}

static uint64_t h_create_factory(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    return create_factory(w64_ptr(a[0]), a[1]);
}

static uint64_t h_create_factory2(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    return create_factory(w64_ptr(a[1]), a[2]);
}

static uint64_t h_debug_interface(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    if (a[2])
        put_ptr(a[2], NULL);
    return E_NOINTERFACE;
}

static const struct w64_export dxgi64_exports[] = {
    W64_FN("CreateDXGIFactory", h_create_factory),
    W64_FN("CreateDXGIFactory1", h_create_factory),
    W64_FN("CreateDXGIFactory2", h_create_factory2),
    W64_FN("DXGIGetDebugInterface1", h_debug_interface),
};

W64_EXPORTS(dxgi64_module, "dxgi.dll", dxgi64_exports);

#endif
