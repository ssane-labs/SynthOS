#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/string.h>

#include "win32.h"

#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_NO_TOKEN 1008

struct sid {
    uint8_t revision, count;
    uint8_t authority[6];
    uint32_t sub[8];
};

static const struct sid user_sid = {1, 5, {0, 0, 0, 0, 0, 5}, {21, 1000, 2000, 3000, 1001}};
static const struct sid admins_sid = {1, 2, {0, 0, 0, 0, 0, 5}, {32, 544}};
static const struct sid system_sid = {1, 1, {0, 0, 0, 0, 0, 5}, {18}};
static const struct sid world_sid = {1, 1, {0, 0, 0, 0, 0, 1}, {0}};

static DWORD sid_size(const struct sid *s)
{
    return 8u + 4u * s->count;
}

static bool sid_equal(const struct sid *a, const struct sid *b)
{
    return a && b && a->count == b->count && !memcmp(a, b, sid_size(a));
}

static void *local_alloc(SIZE_T n)
{
    void *(WINAPI *fn)(UINT, SIZE_T) = (void *(WINAPI *)(UINT, SIZE_T))w32_builtin_lookup("kernel32.dll", "LocalAlloc");
    return fn ? fn(0x40, n) : NULL;
}

static BOOL WINAPI AllocateAndInitializeSid(const uint8_t *authority, uint8_t count, DWORD s0, DWORD s1, DWORD s2,
                                            DWORD s3, DWORD s4, DWORD s5, DWORD s6, DWORD s7, struct sid **out)
{
    if (!authority || !out || count > 8) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct sid *s = local_alloc(sizeof(*s));
    if (!s) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    const DWORD sub[8] = {s0, s1, s2, s3, s4, s5, s6, s7};
    s->revision = 1;
    s->count = count;
    memcpy(s->authority, authority, 6);
    memcpy(s->sub, sub, sizeof(sub));
    *out = s;
    return TRUE;
}

static void *WINAPI FreeSid(struct sid *s)
{
    void *(WINAPI *fn)(void *) = (void *(WINAPI *)(void *))w32_builtin_lookup("kernel32.dll", "LocalFree");
    if (fn && s)
        fn(s);
    return NULL;
}

static DWORD WINAPI GetLengthSid(const struct sid *s)
{
    return s ? sid_size(s) : 0;
}

static BOOL WINAPI IsValidSid(const struct sid *s)
{
    return s && s->revision == 1 && s->count <= 15;
}

static BOOL WINAPI EqualSid(const struct sid *a, const struct sid *b)
{
    return sid_equal(a, b);
}

static BOOL WINAPI CopySid(DWORD len, struct sid *dst, const struct sid *src)
{
    if (!src || len < sid_size(src)) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(dst, src, sid_size(src));
    return TRUE;
}

static BOOL WINAPI IsWellKnownSid(const struct sid *s, int type)
{
    switch (type) {
    case 1: return sid_equal(s, &world_sid);
    case 22: return sid_equal(s, &system_sid);
    case 26: return sid_equal(s, &admins_sid);
    default: return FALSE;
    }
}

static BOOL WINAPI CheckTokenMembership(HANDLE token, const struct sid *s, BOOL *is_member)
{
    UNUSED(token);
    if (!is_member) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *is_member = sid_equal(s, &admins_sid) || sid_equal(s, &user_sid) || sid_equal(s, &world_sid);
    return TRUE;
}

static BOOL WINAPI ConvertSidToStringSidA(const struct sid *s, char **out)
{
    if (!IsValidSid(s) || !out) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    char *str = local_alloc(16 + 11u * s->count);
    if (!str) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    int n = ksnprintf(str, 16, "S-1-%u", s->authority[5]);
    for (int i = 0; i < s->count; i++)
        n += ksnprintf(str + n, 12, "-%u", s->sub[i]);
    *out = str;
    return TRUE;
}

static BOOL WINAPI LookupAccountSidA(LPCSTR system, const struct sid *s, char *name, DWORD *name_len, char *domain,
                                     DWORD *domain_len, int *use)
{
    UNUSED(system);
    const char *n = sid_equal(s, &admins_sid) ? "Administrators" : sid_equal(s, &system_sid) ? "SYSTEM" : CONFIG_USER;
    const char *d = sid_equal(s, &admins_sid) ? "BUILTIN" : sid_equal(s, &system_sid) ? "NT AUTHORITY" : "SYNTHOS";
    DWORD nl = (DWORD)strlen(n), dl = (DWORD)strlen(d);
    if (!name || !domain || *name_len <= nl || *domain_len <= dl) {
        *name_len = nl + 1;
        *domain_len = dl + 1;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(name, n, nl + 1);
    memcpy(domain, d, dl + 1);
    *name_len = nl;
    *domain_len = dl;
    if (use)
        *use = sid_equal(s, &user_sid) ? 1 : 4;
    return TRUE;
}

static DWORD WINAPI GetNamedSecurityInfoW(LPCWSTR name, int type, DWORD info, struct sid **owner, struct sid **group,
                                          void **dacl, void **sacl, void **descriptor)
{
    UNUSED(name);
    UNUSED(type);
    UNUSED(info);
    uint8_t *sd = local_alloc(20 + 2 * sizeof(struct sid) + 8);
    if (!sd)
        return ERROR_NOT_ENOUGH_MEMORY;
    struct sid *o = (struct sid *)(sd + 20), *g = o + 1;
    *o = user_sid;
    *g = user_sid;
    sd[0] = 1;
    if (owner)
        *owner = o;
    if (group)
        *group = g;
    if (dacl)
        *dacl = NULL;
    if (sacl)
        *sacl = NULL;
    if (descriptor)
        *descriptor = sd;
    return 0;
}

static BOOL WINAPI InitializeSecurityDescriptor(uint8_t *sd, DWORD revision)
{
    if (!sd)
        return FALSE;
    memset(sd, 0, 20);
    sd[0] = (uint8_t)revision;
    return TRUE;
}

static BOOL WINAPI SetSecurityDescriptorDacl(void *sd, BOOL present, void *dacl, BOOL defaulted)
{
    UNUSED(sd);
    UNUSED(present);
    UNUSED(dacl);
    UNUSED(defaulted);
    return TRUE;
}

static DWORD WINAPI SetEntriesInAclA(ULONG count, void *entries, void *old, void **out)
{
    UNUSED(count);
    UNUSED(entries);
    UNUSED(old);
    uint8_t *acl = local_alloc(8);
    if (!acl)
        return ERROR_NOT_ENOUGH_MEMORY;
    acl[0] = 2;
    acl[2] = 8;
    *out = acl;
    return 0;
}

static HANDLE WINAPI no_service(LPCSTR a, LPCSTR b, DWORD access)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(access);
    w32_set_error(ERROR_ACCESS_DENIED);
    return NULL;
}

static BOOL WINAPI CloseServiceHandle(HANDLE h)
{
    UNUSED(h);
    return TRUE;
}

static BOOL WINAPI QueryServiceStatusEx(HANDLE h, int level, void *buf, DWORD size, DWORD *needed)
{
    UNUSED(h);
    UNUSED(level);
    UNUSED(buf);
    UNUSED(size);
    UNUSED(needed);
    w32_set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

static BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, HANDLE *out)
{
    UNUSED(process);
    UNUSED(access);
    *out = w32_handle_new(WH_TOKEN);
    return *out != NULL;
}

static BOOL WINAPI OpenThreadToken(HANDLE thread, DWORD access, BOOL self, HANDLE *out)
{
    UNUSED(thread);
    UNUSED(access);
    UNUSED(self);
    *out = NULL;
    w32_set_error(ERROR_NO_TOKEN);
    return FALSE;
}

static BOOL WINAPI GetTokenInformation(HANDLE token, int cls, uint8_t *buf, DWORD len, DWORD *ret)
{
    struct w32_object *o = w32_handle_get(token);
    if (!o || o->type != WH_TOKEN) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    DWORD need;
    switch (cls) {
    case 1: need = 8 + sid_size(&user_sid); break;
    case 2: need = 4 + 8 + sid_size(&admins_sid); break;
    case 18:
    case 20: need = 4; break;
    default: w32_set_error(ERROR_INVALID_PARAMETER); return FALSE;
    }
    if (ret)
        *ret = need;
    if (!buf || len < need) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    uint32_t *w = (uint32_t *)buf;
    if (cls == 1) {
        w[0] = (uint32_t)(uintptr_t)(buf + 8);
        w[1] = 0;
        memcpy(buf + 8, &user_sid, sid_size(&user_sid));
    } else if (cls == 2) {
        w[0] = 1;
        w[1] = (uint32_t)(uintptr_t)(buf + 12);
        w[2] = 7;
        memcpy(buf + 12, &admins_sid, sid_size(&admins_sid));
    } else {
        w[0] = 1;
    }
    return TRUE;
}

static const struct w32_export security_exports[] = {
    W32_FN(AllocateAndInitializeSid),
    W32_FN(FreeSid),
    W32_FN(GetLengthSid),
    W32_FN(IsValidSid),
    W32_FN(EqualSid),
    W32_FN(CopySid),
    W32_FN(IsWellKnownSid),
    W32_FN(CheckTokenMembership),
    W32_FN(ConvertSidToStringSidA),
    W32_FN(LookupAccountSidA),
    W32_FN(GetNamedSecurityInfoW),
    W32_FN(InitializeSecurityDescriptor),
    W32_FN(SetSecurityDescriptorDacl),
    W32_FN(SetEntriesInAclA),
    W32_AS("OpenSCManagerA", no_service),
    W32_AS("OpenSCManagerW", no_service),
    W32_AS("OpenServiceA", no_service),
    W32_AS("OpenServiceW", no_service),
    W32_FN(CloseServiceHandle),
    W32_FN(QueryServiceStatusEx),
    W32_FN(OpenProcessToken),
    W32_FN(OpenThreadToken),
    W32_FN(GetTokenInformation),
};

W32_EXPORTS(security_module, "advapi32.dll", security_exports);
