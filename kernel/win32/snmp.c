/* snmpapi.dll's object-identifier helpers and uptime, and tdh.dll: there are no ETW event providers, so no event
   has a schema to look up. */
#include <synth/hal.h>
#include <synth/string.h>

#include "win64.h"

#define ERROR_NOT_FOUND 1168

static DWORD WINAPI SnmpSvcGetUptime(void)
{
    return (DWORD)(hal_ticks_ms() / 10);
}

static void *WINAPI SnmpUtilMemAlloc(UINT size)
{
    return w32_zalloc(size ? size : 1);
}

static void WINAPI SnmpUtilMemFree(void *p)
{
    w32_free(p);
}

/* AsnObjectIdentifier {UINT idLength; UINT *ids} */
static int WINAPI SnmpUtilOidCpy(uint32_t *dst, const uint32_t *src)
{
    if (!dst || !src)
        return 0;
    UINT n = src[0];
    UINT *ids = n ? w32_alloc(n * 4) : NULL;
    if (n && !ids)
        return 0;
    if (n)
        memcpy(ids, (const void *)(uintptr_t)src[1], n * 4);
    dst[0] = n;
    dst[1] = (uint32_t)(uintptr_t)ids;
    return 1;
}

static void WINAPI SnmpUtilOidFree(uint32_t *oid)
{
    if (!oid)
        return;
    w32_free((void *)(uintptr_t)oid[1]);
    oid[0] = oid[1] = 0;
}

static int oid_cmp(const UINT *a, UINT na, const UINT *b, UINT nb, UINT limit)
{
    UINT n = MIN(MIN(na, nb), limit);
    for (UINT i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    if (n == limit)
        return 0;
    return na < nb ? -1 : na > nb ? 1 : 0;
}

static int WINAPI SnmpUtilOidNCmp(const uint32_t *a, const uint32_t *b, UINT n)
{
    if (!a || !b)
        return 0;
    return oid_cmp((const UINT *)(uintptr_t)a[1], a[0], (const UINT *)(uintptr_t)b[1], b[0], n);
}

static int WINAPI SnmpUtilOidCmp(const uint32_t *a, const uint32_t *b)
{
    return SnmpUtilOidNCmp(a, b, ~0u);
}

static DWORD WINAPI TdhGetEventInformation(const void *event, ULONG count, const void *context, void *info,
                                           ULONG *size)
{
    UNUSED(event);
    UNUSED(count);
    UNUSED(context);
    UNUSED(info);
    UNUSED(size);
    return ERROR_NOT_FOUND;
}

static DWORD WINAPI TdhGetPropertySize(const void *event, ULONG count, const void *context, ULONG props,
                                       const void *data, ULONG *size)
{
    UNUSED(event);
    UNUSED(count);
    UNUSED(context);
    UNUSED(props);
    UNUSED(data);
    UNUSED(size);
    return ERROR_NOT_FOUND;
}

static const struct w32_export snmp_exports[] = {
    W32_FN(SnmpSvcGetUptime), W32_FN(SnmpUtilMemAlloc), W32_FN(SnmpUtilMemFree), W32_FN(SnmpUtilOidCpy),
    W32_FN(SnmpUtilOidFree), W32_FN(SnmpUtilOidNCmp), W32_FN(SnmpUtilOidCmp),
};

W32_EXPORTS(snmp_module, "snmpapi.dll", snmp_exports);

static const struct w32_export tdh_exports[] = {
    W32_FN(TdhGetEventInformation),
    W32_FN(TdhGetPropertySize),
};

W32_EXPORTS(tdh_module, "tdh.dll", tdh_exports);

/* Win64: the identifier is {UINT idLength; UINT *ids} with a 64-bit pointer at offset 8 */
static uint64_t h_oid_cpy(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    uint8_t *dst = w64_ptr(a[0]);
    const uint8_t *src = w64_ptr(a[1]);
    if (!dst || !src)
        return 0;
    UINT n;
    uint64_t from;
    memcpy(&n, src, 4);
    memcpy(&from, src + 8, 8);
    UINT *ids = n ? w32_alloc(n * 4) : NULL;
    if (n && !ids)
        return 0;
    if (n)
        memcpy(ids, w64_ptr(from), n * 4);
    uint64_t to = (uint64_t)(uintptr_t)ids;
    memcpy(dst, &n, 4);
    memcpy(dst + 8, &to, 8);
    return 1;
}

static uint64_t h_oid_free(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    uint8_t *oid = w64_ptr(a[0]);
    if (oid) {
        uint64_t p;
        memcpy(&p, oid + 8, 8);
        w32_free(w64_ptr(p));
        memset(oid, 0, 16);
    }
    return 0;
}

static uint64_t h_oid_ncmp(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    const uint8_t *x = w64_ptr(a[0]), *y = w64_ptr(a[1]);
    if (!x || !y)
        return 0;
    UINT nx, ny;
    uint64_t px, py;
    memcpy(&nx, x, 4);
    memcpy(&ny, y, 4);
    memcpy(&px, x + 8, 8);
    memcpy(&py, y + 8, 8);
    return w64_sx((uint32_t)oid_cmp(w64_ptr(px), nx, w64_ptr(py), ny, a[2] ? (UINT)a[2] : 0));
}

static uint64_t h_oid_cmp(uint64_t *a, const uint64_t *f, void *t)
{
    a[2] = ~0u;
    return h_oid_ncmp(a, f, t);
}

static const struct w64_export snmp64_exports[] = {
    W64_FN("SnmpUtilOidCpy", h_oid_cpy), W64_FN("SnmpUtilOidFree", h_oid_free),
    W64_FN("SnmpUtilOidNCmp", h_oid_ncmp), W64_FN("SnmpUtilOidCmp", h_oid_cmp),
};

W64_EXPORTS(snmp64_module, "snmpapi.dll", snmp64_exports);
