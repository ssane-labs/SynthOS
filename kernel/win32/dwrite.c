#include <synth/string.h>

#include "win32.h"

#define S_OK 0u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define A uint32_t

static const uint8_t iid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t iid_factory[16] = {0x5a, 0xee, 0x59, 0xb8, 0x38, 0xd8, 0x5b, 0x4b,
                                        0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48};

static const uint8_t iid_collection[16] = {0x02, 0xee, 0x4c, 0xa8, 0xea, 0x3e, 0xee, 0x4e,
                                           0xa8, 0x27, 0x87, 0xc1, 0xa0, 0x2a, 0x0f, 0xcc};

struct factory {
    const void *const *vtbl;
    LONG refs;
    const uint8_t *iid;
};

static uint32_t WINAPI query_interface(struct factory *self, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!memcmp(iid, iid_unknown, 16) || !memcmp(iid, self->iid, 16)) {
        __atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED);
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static uint32_t WINAPI add_ref(struct factory *self)
{
    return (uint32_t)__atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED);
}

static uint32_t WINAPI release(struct factory *self)
{
    LONG n = __atomic_sub_fetch(&self->refs, 1, __ATOMIC_RELAXED);
    if (n == 0)
        w32_free(self);
    return (uint32_t)n;
}

static uint32_t WINAPI ok2(void *self, A a)
{
    UNUSED(self);
    UNUSED(a);
    return S_OK;
}

static uint32_t WINAPI none2(void *self, void **out)
{
    UNUSED(self);
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI none3(void *self, A a, void **out)
{
    UNUSED(a);
    return none2(self, out);
}

static uint32_t WINAPI none4(void *self, A a, A b, void **out)
{
    UNUSED(b);
    return none3(self, a, out);
}

static uint32_t WINAPI none5(void *self, A a, A b, A c, void **out)
{
    UNUSED(c);
    return none4(self, a, b, out);
}

static uint32_t WINAPI none7(void *self, A a, A b, A c, A d, A e, void **out)
{
    UNUSED(d);
    UNUSED(e);
    return none5(self, a, b, c, out);
}

static uint32_t WINAPI none9(void *self, A a, A b, A c, A d, A e, A f, A g, void **out)
{
    UNUSED(f);
    UNUSED(g);
    return none7(self, a, b, c, d, e, out);
}

static uint32_t WINAPI none10(void *self, A a, A b, A c, A d, A e, A f, A g, A h, void **out)
{
    UNUSED(h);
    return none9(self, a, b, c, d, e, f, g, out);
}

static UINT WINAPI family_count(void *self)
{
    UNUSED(self);
    return 0;
}

static uint32_t WINAPI find_family(void *self, const WCHAR *name, UINT *index, BOOL *exists)
{
    UNUSED(self);
    UNUSED(name);
    *index = 0xffffffffu;
    *exists = FALSE;
    return S_OK;
}

static const void *const collection_vtbl[] = {
    query_interface, add_ref, release,
    family_count,       /* GetFontFamilyCount */
    none3,              /* GetFontFamily */
    find_family,        /* FindFamilyName */
    none3,              /* GetFontFromFontFace */
};

static void *com_new(const void *const *vtbl, const uint8_t *iid)
{
    struct factory *o = w32_zalloc(sizeof(*o));
    if (o) {
        o->vtbl = vtbl;
        o->refs = 1;
        o->iid = iid;
    }
    return o;
}

static uint32_t WINAPI system_collection(void *self, void **out, BOOL check)
{
    UNUSED(self);
    UNUSED(check);
    *out = com_new(collection_vtbl, iid_collection);
    return *out ? S_OK : 0x8007000eu;
}

static const void *const factory_vtbl[] = {
    query_interface, add_ref, release,
    system_collection,  /* GetSystemFontCollection */
    none5,              /* CreateCustomFontCollection */
    ok2,                /* RegisterFontCollectionLoader */
    ok2,                /* UnregisterFontCollectionLoader */
    none4,              /* CreateFontFileReference */
    none5,              /* CreateCustomFontFileReference */
    none7,              /* CreateFontFace */
    none2,              /* CreateRenderingParams */
    none3,              /* CreateMonitorRenderingParams */
    none7,              /* CreateCustomRenderingParams */
    ok2,                /* RegisterFontFileLoader */
    ok2,                /* UnregisterFontFileLoader */
    none9,              /* CreateTextFormat */
    none2,              /* CreateTypography */
    none2,              /* GetGdiInterop */
    none7,              /* CreateTextLayout */
    none10,             /* CreateGdiCompatibleTextLayout */
    none3,              /* CreateEllipsisTrimmingSign */
    none2,              /* CreateTextAnalyzer */
    none5,              /* CreateNumberSubstitution */
    none9,              /* CreateGlyphRunAnalysis */
};

static uint32_t WINAPI DWriteCreateFactory(int type, const uint8_t *iid, void **out)
{
    UNUSED(type);
    struct factory *f = com_new(factory_vtbl, iid_factory);
    if (!f)
        return 0x8007000eu;
    uint32_t hr = query_interface(f, iid, out);
    release(f);
    return hr;
}

static const struct w32_export dwrite_exports[] = {
    W32_FN(DWriteCreateFactory),
};

W32_EXPORTS(dwrite_module, "dwrite.dll", dwrite_exports);
