#include <synth/string.h>

#include "win32.h"
#include "win64.h"

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

/* One buffer with a structure of DWORDs ('d') and string pointers ('s', laid out for the program's pointer size),
   the strings after it; NetApiBufferFree frees it. */
static void *pack(const char *fmt, const char *const *strings, const DWORD *values)
{
    size_t ptr = w32->win64 ? 8 : 4, at = 0, chars = 0, n = strlen(fmt), offs[12];
    for (size_t i = 0; i < n && i < ARRAY_SIZE(offs); i++) {
        size_t size = fmt[i] == 's' ? ptr : 4;
        at = (at + size - 1) / size * size;
        offs[i] = at;
        at += size;
    }
    at = (at + ptr - 1) / ptr * ptr;
    for (size_t i = 0, k = 0; i < n; i++)
        if (fmt[i] == 's')
            chars += strlen(strings[k++]) + 1;
    uint8_t *buf = w32_zalloc(at + chars * sizeof(WCHAR));
    if (!buf)
        return NULL;
    WCHAR *text = (WCHAR *)(buf + at);
    for (size_t i = 0, k = 0, v = 0; i < n && i < ARRAY_SIZE(offs); i++) {
        if (fmt[i] == 's') {
            uint64_t p = (uint32_t)(uintptr_t)wide_copy(strings[k], text);
            memcpy(buf + offs[i], &p, ptr);
            text += strlen(strings[k++]) + 1;
        } else {
            memcpy(buf + offs[i], &values[v++], 4);
        }
    }
    return buf;
}

static void computer_name(char *out, DWORD cap)
{
    BOOL(WINAPI * get)(LPSTR, DWORD *) = (BOOL(WINAPI *)(LPSTR, DWORD *))w32_builtin_lookup("kernel32.dll", "GetComputerNameA");
    if (!get || !get(out, &cap))
        str_copy(out, "SYNTHOS", cap);
}

static void user_name(char *out, DWORD cap)
{
    BOOL(WINAPI * get)(LPSTR, DWORD *) = (BOOL(WINAPI *)(LPSTR, DWORD *))w32_builtin_lookup("advapi32.dll", "GetUserNameA");
    if (!get || !get(out, &cap))
        str_copy(out, "user", cap);
}

/* SERVER_INFO_100/101: a Windows NT 10.0 workstation (SV_TYPE_WORKSTATION | SV_TYPE_SERVER | SV_TYPE_NT) */
static DWORD WINAPI NetServerGetInfo(const WCHAR *server, DWORD level, void **out)
{
    UNUSED(server);
    char name[64];
    computer_name(name, sizeof(name));
    const char *strings[] = {name, ""};
    DWORD values[] = {500, 10, 0, 0x1003};
    if (level != 100 && level != 101)
        return 124u;
    *out = pack(level == 100 ? "ds" : "dsddds", strings, values);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}

/* WKSTA_INFO_100/101/102 */
static DWORD WINAPI NetWkstaGetInfo(const WCHAR *server, DWORD level, void **out)
{
    UNUSED(server);
    char name[64];
    computer_name(name, sizeof(name));
    const char *strings[] = {name, "WORKGROUP", ""};
    DWORD values[] = {500, 10, 0, 1};
    const char *fmt = level == 100 ? "dssdd" : level == 101 ? "dssdds" : level == 102 ? "dssddsd" : NULL;
    if (!fmt)
        return 124u;
    *out = pack(fmt, strings, values);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}

/* WKSTA_USER_INFO_0/1: the user logged on to this computer */
static DWORD WINAPI NetWkstaUserGetInfo(const WCHAR *reserved, DWORD level, void **out)
{
    UNUSED(reserved);
    char user[64], name[64];
    user_name(user, sizeof(user));
    computer_name(name, sizeof(name));
    const char *strings[] = {user, name, "", name};
    if (level != 0 && level != 1)
        return 124u;
    *out = pack(level == 0 ? "s" : "ssss", strings, NULL);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}

static const struct w32_export netapi32_exports[] = {
    W32_FN(NetServerGetInfo),
    W32_FN(NetWkstaGetInfo),
    W32_FN(NetWkstaUserGetInfo),
    W32_FN(DsRoleGetPrimaryDomainInformation),
    W32_FN(DsRoleFreeMemory),
    W32_FN(NetGetJoinInformation),
    W32_FN(NetApiBufferFree),
    W32_FN(NetUserGetInfo),
    W32_FN(NetGetAadJoinInformation),
    W32_FN(NetFreeAadJoinInformation),
};

W32_EXPORTS(netapi32_module, "netapi32.dll", netapi32_exports);

static const struct w64_export netapi64_exports[] = {
    W64_OUT("NetServerGetInfo", 1u << 2),
    W64_OUT("NetWkstaGetInfo", 1u << 2),
    W64_OUT("NetWkstaUserGetInfo", 1u << 2),
};

W64_EXPORTS(netapi64_module, "netapi32.dll", netapi64_exports);
