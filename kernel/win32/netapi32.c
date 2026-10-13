#include <synth/string.h>

#include "win32.h"

#define NERR_USER_NOT_FOUND 2221

static WCHAR *wide_copy(const char *s, void *at)
{
    WCHAR *w = at;
    size_t i = 0;
    for (; s[i]; i++)
        w[i] = (uint8_t)s[i];
    w[i] = 0;
    return w;
}

static DWORD WINAPI DsRoleGetPrimaryDomainInformation(const WCHAR *server, int level, void **out)
{
    UNUSED(server);
    if (level != 1)
        return ERROR_INVALID_PARAMETER;
    uint32_t *info = w32_zalloc(64);
    if (!info)
        return ERROR_NOT_ENOUGH_MEMORY;
    info[2] = (uint32_t)wide_copy("WORKGROUP", info + 10);
    *out = info;
    return 0;
}

static DWORD WINAPI NetGetJoinInformation(const WCHAR *server, WCHAR **name, int *status)
{
    UNUSED(server);
    *name = w32_alloc(20 * sizeof(WCHAR));
    if (!*name)
        return ERROR_NOT_ENOUGH_MEMORY;
    wide_copy("WORKGROUP", *name);
    *status = 2;
    return 0;
}

static DWORD WINAPI NetApiBufferFree(void *buf)
{
    w32_free(buf);
    return 0;
}

static void WINAPI DsRoleFreeMemory(void *buf)
{
    w32_free(buf);
}

static DWORD WINAPI NetUserGetInfo(const WCHAR *server, const WCHAR *user, DWORD level, void **out)
{
    UNUSED(server);
    UNUSED(user);
    UNUSED(level);
    *out = NULL;
    return NERR_USER_NOT_FOUND;
}

static DWORD WINAPI NetGetAadJoinInformation(const WCHAR *tenant, void **info)
{
    UNUSED(tenant);
    *info = NULL;
    return 0;
}

static void WINAPI NetFreeAadJoinInformation(void *info)
{
    UNUSED(info);
}

static const struct w32_export netapi32_exports[] = {
    W32_FN(DsRoleGetPrimaryDomainInformation),
    W32_FN(DsRoleFreeMemory),
    W32_FN(NetGetJoinInformation),
    W32_FN(NetApiBufferFree),
    W32_FN(NetUserGetInfo),
    W32_FN(NetGetAadJoinInformation),
    W32_FN(NetFreeAadJoinInformation),
};

W32_EXPORTS(netapi32_module, "netapi32.dll", netapi32_exports);
