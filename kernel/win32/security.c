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

/* What a program may ask about its own token: the user, its groups, owner and primary group, and fixed answers for
   the rest (a primary, elevated, medium-integrity token without privileges in session 1). */
static BOOL WINAPI GetTokenInformation(HANDLE token, int cls, uint8_t *buf, DWORD len, DWORD *ret)
{
    static const struct sid medium_label = {1, 1, {0, 0, 0, 0, 0, 16}, {0x2000}};
    struct w32_object *o = w32_handle_get(token);
    if (!o || o->type != WH_TOKEN) {
        w32_set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const struct sid *sid = NULL;
    DWORD head, need;
    switch (cls) {
    case 1: sid = &user_sid, head = 8; break;
    case 2: sid = &admins_sid, head = 12; break;
    case 4:
    case 5: sid = &user_sid, head = 4; break;
    case 25: sid = &medium_label, head = 8; break;
    case 10: head = 56; break;
    case 3:
    case 6:
    case 8:
    case 12:
    case 18:
    case 20:
    case 29: head = 4; break;
    default: w32_set_error(ERROR_INVALID_PARAMETER); return FALSE;
    }
    need = head + (sid ? sid_size(sid) : 0);
    if (ret)
        *ret = need;
    if (!buf || len < need) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, need);
    uint32_t *w = (uint32_t *)buf;
    if (sid) {
        memcpy(buf + head, sid, sid_size(sid));
        /* TOKEN_GROUPS starts with the count, the others with the SID pointer */
        w[cls == 2] = (uint32_t)(uintptr_t)(buf + head);
        if (cls == 2)
            w[0] = 1, w[2] = 7;
        else if (cls == 25)
            w[1] = 0x20;
    } else if (cls == 8 || cls == 12 || cls == 18 || cls == 20) {
        w[0] = 1;
    }
    return TRUE;
}

static WCHAR *local_wide(const char *s)
{
    size_t n = strlen(s);
    WCHAR *w = local_alloc((n + 1) * sizeof(WCHAR));
    if (w)
        for (size_t i = 0; i <= n; i++)
            w[i] = (uint8_t)s[i];
    return w;
}

static int sid_format(const struct sid *s, char *out, size_t size)
{
    int n = ksnprintf(out, size, "S-1-%u", s->authority[5]);
    for (int i = 0; i < s->count && (size_t)n < size; i++)
        n += ksnprintf(out + n, size - (size_t)n, "-%u", s->sub[i]);
    return n;
}

static BOOL WINAPI ConvertSidToStringSidW(const struct sid *s, WCHAR **out)
{
    char buf[128];
    if (!IsValidSid(s) || !out) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    sid_format(s, buf, sizeof(buf));
    *out = local_wide(buf);
    return *out != NULL;
}

static const struct sid *sid_alias(const char *s)
{
    static const struct {
        char name[3];
        const struct sid *sid;
    } aliases[] = {{"BA", &admins_sid}, {"SY", &system_sid}, {"WD", &world_sid}, {"OW", &user_sid}, {"CO", &user_sid}};
    for (size_t i = 0; i < ARRAY_SIZE(aliases); i++)
        if (!strcmp(s, aliases[i].name))
            return aliases[i].sid;
    return NULL;
}

static bool sid_parse(const char *s, struct sid *out)
{
    const struct sid *alias = sid_alias(s);
    if (alias) {
        *out = *alias;
        return true;
    }
    if ((s[0] != 'S' && s[0] != 's') || s[1] != '-')
        return false;
    s += 2;
    uint32_t parts[10];
    int n = 0;
    while (*s && n < 10) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (uint32_t)(*s++ - '0');
        parts[n++] = v;
        if (*s == '-')
            s++;
        else if (*s)
            return false;
    }
    if (n < 2 || parts[0] != 1 || n - 2 > 8)
        return false;
    memset(out, 0, sizeof(*out));
    out->revision = 1;
    out->count = (uint8_t)(n - 2);
    out->authority[5] = (uint8_t)parts[1];
    for (int i = 2; i < n; i++)
        out->sub[i - 2] = parts[i];
    return true;
}

static BOOL WINAPI ConvertStringSidToSidW(const WCHAR *text, struct sid **out)
{
    char buf[128];
    size_t i = 0;
    for (; text && text[i] && i < sizeof(buf) - 1; i++)
        buf[i] = (char)text[i];
    buf[i] = 0;
    struct sid s;
    if (!text || !out || !sid_parse(buf, &s)) {
        w32_set_error(1337);
        return FALSE;
    }
    *out = local_alloc(sizeof(s));
    if (!*out)
        return FALSE;
    memcpy(*out, &s, sizeof(s));
    return TRUE;
}

static BOOL WINAPI ConvertStringSidToSidA(const char *text, struct sid **out)
{
    struct sid s;
    if (!text || !out || !sid_parse(text, &s)) {
        w32_set_error(1337);
        return FALSE;
    }
    *out = local_alloc(sizeof(s));
    if (!*out)
        return FALSE;
    memcpy(*out, &s, sizeof(s));
    return TRUE;
}

static DWORD *WINAPI GetSidSubAuthority(struct sid *s, DWORD n)
{
    return &s->sub[n & 7];
}

static uint8_t *WINAPI GetSidSubAuthorityCount(struct sid *s)
{
    return &s->count;
}

static uint8_t *WINAPI GetSidIdentifierAuthority(struct sid *s)
{
    return s->authority;
}

static DWORD WINAPI GetSidLengthRequired(uint8_t count)
{
    return 8u + 4u * count;
}

static BOOL WINAPI InitializeSid(struct sid *s, const uint8_t *authority, uint8_t count)
{
    if (!s || !authority || count > 15)
        return FALSE;
    s->revision = 1;
    s->count = count;
    memcpy(s->authority, authority, 6);
    memset(s->sub, 0, 4u * MIN(count, (uint8_t)8));
    return TRUE;
}

static BOOL WINAPI CreateWellKnownSid(int type, const struct sid *domain, struct sid *out, DWORD *size)
{
    UNUSED(domain);
    static const struct {
        int type;
        struct sid sid;
    } known[] = {
        {1, {1, 1, {0, 0, 0, 0, 0, 1}, {0}}},         {3, {1, 1, {0, 0, 0, 0, 0, 3}, {0}}},
        {11, {1, 1, {0, 0, 0, 0, 0, 5}, {4}}},        {17, {1, 1, {0, 0, 0, 0, 0, 5}, {11}}},
        {22, {1, 1, {0, 0, 0, 0, 0, 5}, {18}}},       {23, {1, 1, {0, 0, 0, 0, 0, 5}, {19}}},
        {24, {1, 1, {0, 0, 0, 0, 0, 5}, {20}}},       {26, {1, 2, {0, 0, 0, 0, 0, 5}, {32, 544}}},
        {27, {1, 2, {0, 0, 0, 0, 0, 5}, {32, 545}}},  {84, {1, 2, {0, 0, 0, 0, 0, 15}, {2, 1}}},
        {66, {1, 1, {0, 0, 0, 0, 0, 16}, {4096}}},    {67, {1, 1, {0, 0, 0, 0, 0, 16}, {8192}}},
        {68, {1, 1, {0, 0, 0, 0, 0, 16}, {12288}}},   {65, {1, 1, {0, 0, 0, 0, 0, 16}, {0}}},
    };
    for (size_t i = 0; i < ARRAY_SIZE(known); i++) {
        if (known[i].type != type)
            continue;
        DWORD need = sid_size(&known[i].sid);
        if (!size || !out || *size < need) {
            if (size)
                *size = need;
            w32_set_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(out, &known[i].sid, need);
        *size = need;
        return TRUE;
    }
    w32_set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static BOOL WINAPI LookupAccountSidW(const WCHAR *system, const struct sid *s, WCHAR *name, DWORD *name_len,
                                     WCHAR *domain, DWORD *domain_len, int *use)
{
    UNUSED(system);
    char n[64], d[64];
    DWORD nl = sizeof(n), dl = sizeof(d);
    if (!LookupAccountSidA(NULL, s, n, &nl, d, &dl, use))
        return FALSE;
    if (!name || !domain || *name_len <= nl || *domain_len <= dl) {
        *name_len = nl + 1;
        *domain_len = dl + 1;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    for (DWORD i = 0; i <= nl; i++)
        name[i] = (uint8_t)n[i];
    for (DWORD i = 0; i <= dl; i++)
        domain[i] = (uint8_t)d[i];
    *name_len = nl;
    *domain_len = dl;
    return TRUE;
}

static LONG WINAPI RegUnLoadKeyW(HANDLE key, const WCHAR *sub)
{
    UNUSED(key);
    UNUSED(sub);
    return ERROR_ACCESS_DENIED;
}

static BOOL WINAPI LookupAccountNameW(const WCHAR *system, const WCHAR *name, struct sid *sid, DWORD *sid_len,
                                      WCHAR *domain, DWORD *domain_len, int *use)
{
    UNUSED(system);
    UNUSED(name);
    static const char dom[] = "SYNTHOS";
    DWORD need = sid_size(&user_sid);
    if (!sid || !sid_len || *sid_len < need || !domain || !domain_len || *domain_len < sizeof(dom)) {
        if (sid_len)
            *sid_len = need;
        if (domain_len)
            *domain_len = sizeof(dom);
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(sid, &user_sid, need);
    for (size_t i = 0; i < sizeof(dom); i++)
        domain[i] = (uint8_t)dom[i];
    *domain_len = sizeof(dom) - 1;
    if (use)
        *use = 1;
    return TRUE;
}

struct acl {
    uint8_t revision, pad;
    uint16_t size, count, pad2;
};

static BOOL WINAPI InitializeAcl(struct acl *acl, DWORD len, DWORD revision)
{
    if (!acl || len < sizeof(*acl)) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *acl = (struct acl){(uint8_t)revision, 0, (uint16_t)len, 0, 0};
    return TRUE;
}

static BOOL WINAPI GetAclInformation(struct acl *acl, void *info, DWORD len, int cls)
{
    DWORD *out = info;
    if (!acl || !info || (cls == 1 && len < 4) || (cls == 2 && len < 12) || (cls != 1 && cls != 2)) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (cls == 1) {
        out[0] = acl->revision;
    } else {
        out[0] = acl->count;
        out[1] = sizeof(*acl);
        out[2] = acl->size > sizeof(*acl) ? acl->size - (DWORD)sizeof(*acl) : 0;
    }
    return TRUE;
}

static BOOL WINAPI GetAce(struct acl *acl, DWORD index, void **ace)
{
    UNUSED(acl);
    UNUSED(index);
    *ace = NULL;
    w32_set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static BOOL WINAPI GetSecurityDescriptorControl(void *sd, uint16_t *control, DWORD *revision)
{
    UNUSED(sd);
    *control = 0;
    *revision = 1;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorAcl(void *sd, BOOL *present, void **acl, BOOL *defaulted)
{
    UNUSED(sd);
    *present = FALSE;
    *acl = NULL;
    if (defaulted)
        *defaulted = FALSE;
    return TRUE;
}

static BOOL WINAPI GetSecurityDescriptorSid(void *sd, const struct sid **sid, BOOL *defaulted)
{
    UNUSED(sd);
    *sid = &user_sid;
    if (defaulted)
        *defaulted = FALSE;
    return TRUE;
}

static DWORD WINAPI GetSecurityInfo(HANDLE h, int type, DWORD info, struct sid **owner, struct sid **group, void **dacl,
                                    void **sacl, void **descriptor)
{
    UNUSED(h);
    return GetNamedSecurityInfoW(NULL, type, info, owner, group, dacl, sacl, descriptor);
}

static DWORD WINAPI BuildSecurityDescriptorW(void *owner, void *group, ULONG naccess, void *access, ULONG naudit,
                                             void *audit, void *old, ULONG *size, void **out)
{
    UNUSED(owner);
    UNUSED(group);
    UNUSED(naccess);
    UNUSED(access);
    UNUSED(naudit);
    UNUSED(audit);
    UNUSED(old);
    uint8_t *sd = local_alloc(20);
    if (!sd)
        return ERROR_NOT_ENOUGH_MEMORY;
    sd[0] = 1;
    sd[3] = 0x80;
    *size = 20;
    *out = sd;
    return 0;
}

static BOOL WINAPI ConvertSecurityDescriptorToStringSecurityDescriptorW(void *sd, DWORD revision, DWORD info,
                                                                         WCHAR **out, ULONG *len)
{
    UNUSED(sd);
    UNUSED(revision);
    char buf[160] = "";
    int n = 0;
    if (info & 1) {
        n += ksnprintf(buf + n, sizeof(buf) - (size_t)n, "O:");
        n += sid_format(&user_sid, buf + n, sizeof(buf) - (size_t)n);
    }
    if (info & 4)
        ksnprintf(buf + n, sizeof(buf) - (size_t)n, "D:");
    *out = local_wide(buf);
    if (len)
        *len = (ULONG)strlen(buf) + 1;
    return *out != NULL;
}

struct trustee {
    void *multiple;
    int op, form, type;
    void *name;
};

struct explicit_access {
    DWORD permissions;
    int mode;
    DWORD inheritance;
    struct trustee trustee;
};

static void WINAPI BuildTrusteeWithSidW(struct trustee *t, void *sid)
{
    *t = (struct trustee){NULL, 0, 0, 0, sid};
}

static void WINAPI BuildExplicitAccessWithNameW(struct explicit_access *e, WCHAR *name, DWORD permissions, int mode,
                                                DWORD inheritance)
{
    *e = (struct explicit_access){permissions, mode, inheritance, {NULL, 0, 1, 0, name}};
}

static BOOL WINAPI AccessCheck(void *sd, HANDLE token, DWORD desired, void *mapping, void *privileges, DWORD *plen,
                               DWORD *granted, BOOL *status)
{
    UNUSED(sd);
    UNUSED(token);
    UNUSED(mapping);
    UNUSED(privileges);
    UNUSED(plen);
    *granted = desired;
    *status = TRUE;
    return TRUE;
}

static void WINAPI MapGenericMask(DWORD *mask, const DWORD *mapping)
{
    static const DWORD generic[4] = {0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u};
    for (int i = 0; i < 4; i++)
        if (*mask & generic[i])
            *mask |= mapping[i];
    *mask &= 0x0fffffffu;
}

static BOOL WINAPI AllocateLocallyUniqueId(uint32_t *luid)
{
    static uint32_t next = 0x10000;
    luid[0] = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
    luid[1] = 0;
    return TRUE;
}

static DWORD WINAPI LsaNtStatusToWinError(DWORD status)
{
    switch (status) {
    case 0: return 0;
    case 0xc0000022u: return ERROR_ACCESS_DENIED;
    case 0xc0000034u: return 2;
    case 0xc0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    default: return 317;
    }
}

static DWORD WINAPI lsa_denied4(DWORD a, DWORD b, DWORD c, DWORD d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return 0xc0000022u;
}

static DWORD WINAPI EventRegister(const void *guid, void *callback, void *context, uint64_t *handle)
{
    UNUSED(guid);
    UNUSED(callback);
    UNUSED(context);
    *handle = 1;
    return 0;
}

static DWORD WINAPI RegisterTraceGuidsW(void *request, void *context, const void *guid, ULONG count, void *regs,
                                        const WCHAR *mof, const WCHAR *resource, uint64_t *handle)
{
    UNUSED(request);
    UNUSED(context);
    UNUSED(guid);
    UNUSED(count);
    UNUSED(regs);
    UNUSED(mof);
    UNUSED(resource);
    *handle = 1;
    return 0;
}

/* event tracing sessions: none can be started, so there is none to stop, query or read */
#define ERROR_WMI_INSTANCE_NOT_FOUND 4201

static ULONG WINAPI StartTraceW(uint64_t *handle, const void *name, void *props)
{
    UNUSED(name);
    UNUSED(props);
    if (handle)
        *handle = 0;
    return ERROR_ACCESS_DENIED;
}

static ULONG WINAPI StopTraceW(uint64_t handle, const void *name, void *props)
{
    UNUSED(handle);
    UNUSED(name);
    UNUSED(props);
    return ERROR_WMI_INSTANCE_NOT_FOUND;
}

static ULONG WINAPI ControlTraceW(uint64_t handle, const void *name, void *props, ULONG code)
{
    UNUSED(code);
    return StopTraceW(handle, name, props);
}

static ULONG WINAPI QueryAllTracesW(void **props, ULONG count, ULONG *sessions)
{
    UNUSED(props);
    UNUSED(count);
    if (sessions)
        *sessions = 0;
    return 0;
}

static ULONG WINAPI EnableTraceEx2(uint64_t handle, const void *guid, ULONG control, uint8_t level, uint64_t any,
                                   uint64_t all, ULONG timeout, void *params)
{
    UNUSED(handle);
    UNUSED(guid);
    UNUSED(control);
    UNUSED(level);
    UNUSED(any);
    UNUSED(all);
    UNUSED(timeout);
    UNUSED(params);
    return ERROR_INVALID_HANDLE;
}

static uint64_t WINAPI OpenTraceW(void *logfile)
{
    UNUSED(logfile);
    w32_set_error(ERROR_ACCESS_DENIED);
    return ~0ull;
}

static ULONG WINAPI ProcessTrace(uint64_t *handles, ULONG count, void *start, void *end)
{
    UNUSED(handles);
    UNUSED(count);
    UNUSED(start);
    UNUSED(end);
    return ERROR_INVALID_HANDLE;
}

static ULONG WINAPI CloseTrace(uint64_t handle)
{
    UNUSED(handle);
    return ERROR_INVALID_HANDLE;
}

static uint64_t WINAPI GetTraceLoggerHandle(void *buffer)
{
    UNUSED(buffer);
    w32_set_error(ERROR_INVALID_HANDLE);
    return ~0ull;
}

/* UTF-16 text has a byte-order mark or mostly zero high bytes */
static BOOL WINAPI IsTextUnicode(const void *buf, int len, int *result)
{
    const uint8_t *b = buf;
    int zeros = 0, pairs = len / 2;
    for (int i = 0; i < pairs; i++)
        zeros += b[i * 2 + 1] == 0;
    BOOL yes = len >= 2 && ((b[0] == 0xff && b[1] == 0xfe) || (pairs && zeros * 4 >= pairs * 3));
    if (result)
        *result = yes ? 1 : 0;
    return yes;
}

static const struct w32_export security_exports[] = {
    W32_FN(ConvertSidToStringSidW),
    W32_FN(ConvertStringSidToSidW),
    W32_FN(ConvertStringSidToSidA),
    W32_FN(GetSidSubAuthority),
    W32_FN(GetSidSubAuthorityCount),
    W32_FN(LookupAccountNameW),
    W32_FN(InitializeAcl),
    W32_AS("IsValidAcl", w32_one1),
    W32_AS("IsValidSecurityDescriptor", w32_one1),
    W32_FN(GetAce),
    W32_FN(GetAclInformation),
    W32_AS("AddAce", w32_one5),
    W32_FN(GetSidIdentifierAuthority),
    W32_FN(GetSidLengthRequired),
    W32_FN(InitializeSid),
    W32_FN(CreateWellKnownSid),
    W32_FN(LookupAccountSidW),
    W32_FN(RegUnLoadKeyW),
    W32_AS("CreateServiceW", w32_fail13),
    W32_AS("DeleteService", w32_fail1),
    W32_AS("QueryServiceObjectSecurity", w32_fail5),
    W32_AS("SetServiceObjectSecurity", w32_fail3),
    W32_FN(IsTextUnicode),
    W32_AS("ChangeServiceConfig2W", w32_fail3),
    W32_AS("ImpersonateLoggedOnUser", w32_fail1),
    W32_AS("MakeAbsoluteSD", w32_fail11),
    W32_AS("SetSecurityDescriptorOwner", w32_one3),
    W32_AS("SetSecurityDescriptorGroup", w32_one3),
    W32_AS("StartServiceCtrlDispatcherW", w32_fail1),
    W32_AS("RegisterServiceCtrlHandlerW", w32_fail2),
    W32_AS("SetServiceStatus", w32_fail2),
    W32_AS("AddMandatoryAce", w32_one5),
    W32_AS("AddConditionalAce", w32_one8),
    W32_AS("AddAccessAllowedAce", w32_one4),
    W32_AS("AddAccessAllowedAceEx", w32_one5),
    W32_FN(GetSecurityDescriptorControl),
    W32_AS("GetSecurityDescriptorDacl", GetSecurityDescriptorAcl),
    W32_AS("GetSecurityDescriptorSacl", GetSecurityDescriptorAcl),
    W32_AS("GetSecurityDescriptorOwner", GetSecurityDescriptorSid),
    W32_AS("GetSecurityDescriptorGroup", GetSecurityDescriptorSid),
    W32_FN(GetSecurityInfo),
    W32_AS("SetSecurityInfo", w32_zero7),
    W32_AS("SetNamedSecurityInfoW", w32_zero7),
    W32_AS("SetFileSecurityW", w32_one3),
    W32_AS("SetEntriesInAclW", SetEntriesInAclA),
    W32_FN(BuildSecurityDescriptorW),
    W32_FN(ConvertSecurityDescriptorToStringSecurityDescriptorW),
    W32_FN(BuildTrusteeWithSidW),
    W32_FN(BuildExplicitAccessWithNameW),
    W32_FN(AccessCheck),
    W32_FN(MapGenericMask),
    W32_FN(AllocateLocallyUniqueId),
    W32_FN(LsaNtStatusToWinError),
    W32_AS("LsaOpenPolicy", lsa_denied4),
    W32_AS("LsaAddAccountRights", lsa_denied4),
    W32_AS("LsaClose", w32_zero1),
    W32_AS("ChangeServiceConfigW", w32_fail11),
    W32_AS("QueryServiceConfigW", w32_fail4),
    W32_AS("QueryServiceStatus", w32_fail2),
    W32_AS("CryptGetProvParam", w32_fail5),
    W32_AS("CryptSetHashParam", w32_fail4),
    W32_AS("CryptSignHashW", w32_fail6),
    W32_AS("RegDisablePredefinedCache", w32_zero0),
    W32_FN(EventRegister),
    W32_AS("EventUnregister", w32_zero2),
    W32_AS("EventSetInformation", w32_zero5),
    W32_AS("EventWrite", w32_zero5),
    W32_AS("EventWriteTransfer", w32_zero7),
    W32_AS("EventEnabled", w32_zero3),
    W32_AS("EventProviderEnabled", w32_zero5),
    W32_FN(RegisterTraceGuidsW),
    W32_FN(StartTraceW),
    W32_AS("StartTraceA", StartTraceW),
    W32_FN(StopTraceW),
    W32_AS("StopTraceA", StopTraceW),
    W32_FN(ControlTraceW),
    W32_AS("ControlTraceA", ControlTraceW),
    W32_AS("FlushTraceW", StopTraceW),
    W32_AS("FlushTraceA", StopTraceW),
    W32_FN(QueryAllTracesW),
    W32_AS("QueryAllTracesA", QueryAllTracesW),
    W32_FN(EnableTraceEx2),
    W32_FN(OpenTraceW),
    W32_AS("OpenTraceA", OpenTraceW),
    W32_FN(ProcessTrace),
    W32_FN(CloseTrace),
    W32_AS("UnregisterTraceGuids", w32_zero2),
    W32_AS("TraceEvent", w32_zero3),
    W32_AS("GetTraceEnableFlags", w32_zero2),
    W32_AS("GetTraceEnableLevel", w32_zero2),
    W32_FN(GetTraceLoggerHandle),
    W32_AS("LogonUserW", w32_fail6),
    W32_AS("CreateProcessAsUserW", w32_fail11),
    W32_AS("CreateProcessWithTokenW", w32_fail9),
    W32_AS("CreateRestrictedToken", w32_fail9),
    W32_AS("DuplicateTokenEx", w32_fail6),
    W32_AS("SetTokenInformation", w32_fail4),
    W32_AS("SetThreadToken", w32_fail2),
    W32_AS("ImpersonateAnonymousToken", w32_fail1),
    W32_AS("ImpersonateNamedPipeClient", w32_fail1),
    W32_AS("RevertToSelf", w32_fail0),
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
