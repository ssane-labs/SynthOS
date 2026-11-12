#include <synth/string.h>

#include "win64.h"

/* The parts of the 64-bit C++ runtime (vcruntime140) that programs reach outside exceptions: run-time type
   information. The tables MSVC writes for x64 hold 32-bit offsets from the image base: the vtable's slot -1 points
   to the complete object locator, which knows its own offset and so the base. */

enum { COL_OFFSET = 4, COL_TYPE = 12, COL_CLASS = 16, COL_SELF = 20, TD_NAME = 16 };

static uint64_t rd64(uint64_t a)
{
    uint64_t v;
    memcpy(&v, w64_ptr(a), 8);
    return v;
}

static int32_t rd32(uint64_t a)
{
    int32_t v;
    memcpy(&v, w64_ptr(a), 4);
    return v;
}

static uint64_t locator(uint64_t vfptr_at, uint64_t *base)
{
    uint64_t col = rd64(rd64(vfptr_at) - 8);
    if (rd32(col) != 1)
        return 0;
    *base = col - (uint32_t)rd32(col + COL_SELF);
    return col;
}

static bool same_type(uint64_t a, uint64_t b)
{
    return a == b || !strcmp(w64_ptr(a + TD_NAME), w64_ptr(b + TD_NAME));
}

static uint64_t h_typeid(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t base;
    uint64_t col = a[0] ? locator(a[0], &base) : 0;
    return col ? base + (uint32_t)rd32(col + COL_TYPE) : 0;
}

/* __RTDynamicCast(object, offset of its vfptr, source type, target type, is a reference) */
static uint64_t h_dynamic_cast(uint64_t *a, const uint64_t *f, void *t)
{
    if (!a[0])
        return 0;
    uint64_t at = a[0] + (uint64_t)(int64_t)(int32_t)a[1], base;
    uint64_t col = locator(at, &base);
    if (!col)
        return 0;
    uint64_t whole = at - (uint64_t)(int64_t)rd32(col + COL_OFFSET);
    uint64_t chd = base + (uint32_t)rd32(col + COL_CLASS);
    uint64_t list = base + (uint32_t)rd32(chd + 12);
    for (int i = 0, n = rd32(chd + 8); i < n; i++) {
        uint64_t bcd = base + (uint32_t)rd32(list + 4 * (uint64_t)i);
        if (!same_type(base + (uint32_t)rd32(bcd), a[3]))
            continue;
        int32_t mdisp = rd32(bcd + 8), pdisp = rd32(bcd + 12), vdisp = rd32(bcd + 16);
        if (pdisp < 0)
            return whole + (uint64_t)(int64_t)mdisp;
        uint64_t vbtable = rd64(whole + (uint64_t)(int64_t)pdisp);
        return whole + (uint64_t)(int64_t)pdisp + (uint64_t)(int64_t)rd32(vbtable + (uint64_t)vdisp) +
               (uint64_t)(int64_t)mdisp;
    }
    return 0;
}

/* type_info::name() and ==: both get the data after the vtable pointer, the decorated name 8 bytes in */
static uint64_t h_type_name(uint64_t *a, const uint64_t *f, void *t)
{
    return a[0] + 8;
}

static uint64_t h_type_compare(uint64_t *a, const uint64_t *f, void *t)
{
    if (a[0] == a[1])
        return 0;
    return w64_sx((uint32_t)strcmp((const char *)w64_ptr(a[0] + 9), (const char *)w64_ptr(a[1] + 9)));
}

static uint64_t h_zero(uint64_t *a, const uint64_t *f, void *t)
{
    return 0;
}

/* a C++ frame never catches here: the exception goes on to the next handler (SEH continue search) */
static uint64_t h_continue_search(uint64_t *a, const uint64_t *f, void *t)
{
    return 1;
}

static const struct w64_export cxx_exports[] = {
    W64_FN("__RTtypeid", h_typeid),
    W64_FN("__RTDynamicCast", h_dynamic_cast),
    W64_FN("__std_type_info_name", h_type_name),
    W64_FN("__std_type_info_compare", h_type_compare),
    W64_FN("__uncaught_exceptions", h_zero),
    W64_FN("__CxxFrameHandler4", h_continue_search),
};
W64_EXPORTS(cxx_module, "msvcrt.dll", cxx_exports);
