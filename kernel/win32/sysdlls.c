#include <synth/crypto.h>
#include <synth/fmt.h>
#include <synth/sha1.h>
#include <synth/sha256.h>
#include <synth/string.h>
#include <synth/tls.h>

#include "win32.h"

#define E_NOTIMPL 0x80004001u
#define REGDB_E_CLASSNOTREG 0x80040154u
#define CRYPT_E_NOT_FOUND 0x80092004u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813

static const uint8_t *rsrc_find(const uint8_t *file, size_t size, uint32_t *out_size)
{
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z')
        return NULL;
    uint32_t pe = *(const uint32_t *)(file + 0x3c);
    if (pe + 0x18 > size)
        return NULL;
    uint16_t sections = *(const uint16_t *)(file + pe + 6), opt_size = *(const uint16_t *)(file + pe + 20);
    uint32_t opt = pe + 24;
    bool pe64 = *(const uint16_t *)(file + opt) == 0x20b;
    uint32_t dirs = opt + (pe64 ? 112 : 96);
    if (dirs + 24 > size)
        return NULL;
    uint32_t rva = *(const uint32_t *)(file + dirs + 16), sec = opt + opt_size;
    uint32_t base_off = 0, base_va = 0;
    for (uint16_t i = 0; i < sections && sec + 40 <= size; i++, sec += 40) {
        uint32_t va = *(const uint32_t *)(file + sec + 12), vs = *(const uint32_t *)(file + sec + 16);
        if (rva >= va && rva < va + MAX(vs, *(const uint32_t *)(file + sec + 8))) {
            base_va = va;
            base_off = *(const uint32_t *)(file + sec + 20);
            break;
        }
    }
    if (!rva || !base_off)
        return NULL;
    const uint8_t *root = file + base_off + (rva - base_va);
    const uint8_t *dir = root;
    for (int level = 0; level < 3; level++) {
        if (dir + 16 > file + size)
            return NULL;
        uint16_t named = *(const uint16_t *)(dir + 12), ids = *(const uint16_t *)(dir + 14);
        const uint32_t *e = (const uint32_t *)(dir + 16);
        uint32_t next = 0;
        for (uint32_t i = 0; i < (uint32_t)named + ids && (const uint8_t *)(e + 2) <= file + size; i++, e += 2)
            if (level > 0 || e[0] == 16) {
                next = e[1];
                break;
            }
        if (!next)
            return NULL;
        if (level < 2) {
            if (!(next & 0x80000000u))
                return NULL;
            dir = root + (next & 0x7fffffffu);
        } else {
            const uint32_t *data = (const uint32_t *)(root + next);
            if ((const uint8_t *)(data + 2) > file + size || data[0] < base_va)
                return NULL;
            const uint8_t *blob = file + base_off + (data[0] - base_va);
            if (blob + data[1] > file + size)
                return NULL;
            *out_size = data[1];
            return blob;
        }
    }
    return NULL;
}

struct vbuild {
    uint8_t *out;
    uint32_t cap, pos;
};

static void vput(struct vbuild *b, const void *data, uint32_t n)
{
    if (b->out && b->pos + n <= b->cap)
        memcpy(b->out + b->pos, data, n);
    b->pos += n;
}

static void valign(struct vbuild *b)
{
    static const uint8_t zero[4];
    vput(b, zero, ALIGN_UP(b->pos, 4u) - b->pos);
}

static uint32_t vbegin(struct vbuild *b, const char *key, uint16_t value_len, uint16_t text)
{
    valign(b);
    uint32_t at = b->pos;
    uint16_t head[3] = {0, value_len, text};
    vput(b, head, 6);
    for (const char *k = key;; k++) {
        uint16_t c = (uint8_t)*k;
        vput(b, &c, 2);
        if (!*k)
            break;
    }
    valign(b);
    return at;
}

static void vend(struct vbuild *b, uint32_t at)
{
    uint16_t len = (uint16_t)(b->pos - at);
    if (b->out && at + 2 <= b->cap)
        memcpy(b->out + at, &len, 2);
}

static void vstring(struct vbuild *b, const char *key, const char *value)
{
    uint32_t at = vbegin(b, key, (uint16_t)(strlen(value) + 1), 1);
    for (const char *v = value;; v++) {
        uint16_t c = (uint8_t)*v;
        vput(b, &c, 2);
        if (!*v)
            break;
    }
    vend(b, at);
}

static uint32_t synth_version(uint8_t *out, uint32_t cap, const char *name)
{
    struct vbuild b = {out, cap, 0};
    uint32_t root = vbegin(&b, "VS_VERSION_INFO", 52, 0);
    uint32_t fixed[13] = {0xfeef04bdu, 0x00010000u, 10u << 16, (19041u << 16) | 5794u, 10u << 16,
                          (19041u << 16) | 5794u, 0x3f, 0, 0x40004u, 2, 0, 0, 0};
    vput(&b, fixed, sizeof(fixed));
    uint32_t sfi = vbegin(&b, "StringFileInfo", 0, 1);
    uint32_t table = vbegin(&b, "040904B0", 0, 1);
    vstring(&b, "CompanyName", "SynthOS");
    vstring(&b, "FileDescription", name);
    vstring(&b, "FileVersion", "10.0.19041.5794");
    vstring(&b, "InternalName", name);
    vstring(&b, "OriginalFilename", name);
    vstring(&b, "ProductName", "SynthOS Windows compatibility");
    vstring(&b, "ProductVersion", "10.0.19041.5794");
    vend(&b, table);
    vend(&b, sfi);
    uint32_t vfi = vbegin(&b, "VarFileInfo", 0, 1);
    uint32_t var = vbegin(&b, "Translation", 4, 0);
    uint16_t translation[2] = {0x0409, 0x04b0};
    vput(&b, translation, sizeof(translation));
    vend(&b, var);
    vend(&b, vfi);
    vend(&b, root);
    return b.pos;
}

static uint32_t version_blob(LPCWSTR path, uint8_t *out, uint32_t cap)
{
    char *p = w32_narrow(path);
    if (!p)
        return 0;
    char unix_path[PATH_MAX_LEN];
    uint32_t n = 0;
    bool disk = w32_path(p, unix_path, sizeof(unix_path)) == 0 && vfs_exists(unix_path);
    char *data = NULL;
    size_t size = 0;
    if (disk && vfs_read_all(unix_path, &data, &size) == 0) {
        uint32_t blob_size = 0;
        const uint8_t *blob = rsrc_find((const uint8_t *)data, size, &blob_size);
        if (blob) {
            n = blob_size;
            if (out && cap >= n)
                memcpy(out, blob, n);
        }
    }
    kfree(data);
    if (!n && w32_is_system_dll(path_basename(p)))
        n = synth_version(out, cap, path_basename(p));
    w32_free(p);
    return n;
}

static DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR path, DWORD *handle)
{
    if (handle)
        *handle = 0;
    uint32_t n = path ? version_blob(path, NULL, 0) : 0;
    if (!n)
        w32_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return n;
}

static BOOL WINAPI GetFileVersionInfoW(LPCWSTR path, DWORD handle, DWORD len, void *data)
{
    UNUSED(handle);
    uint32_t n = path ? version_blob(path, NULL, 0) : 0;
    if (!n || len < n) {
        w32_set_error(n ? ERROR_INSUFFICIENT_BUFFER : ERROR_RESOURCE_TYPE_NOT_FOUND);
        return FALSE;
    }
    version_blob(path, data, len);
    return TRUE;
}

static DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR path, DWORD *handle)
{
    UNUSED(flags);
    return GetFileVersionInfoSizeW(path, handle);
}

static BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR path, DWORD handle, DWORD len, void *data)
{
    UNUSED(flags);
    return GetFileVersionInfoW(path, handle, len, data);
}

static const uint8_t *node_value(const uint8_t *node, const WCHAR **key)
{
    *key = (const WCHAR *)(node + 6);
    const WCHAR *k = *key;
    while (*k)
        k++;
    uintptr_t after = (uintptr_t)(k + 1);
    return (const uint8_t *)ALIGN_UP(after, 4u);
}

static bool key_equal(const WCHAR *k, const WCHAR *part, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        WCHAR a = k[i], b = part[i];
        if (a >= 'a' && a <= 'z')
            a -= 32;
        if (b >= 'a' && b <= 'z')
            b -= 32;
        if (a != b)
            return false;
    }
    return k[n] == 0;
}

static BOOL WINAPI VerQueryValueW(const void *block, LPCWSTR sub, void **out, UINT *len)
{
    if (out)
        *out = NULL;
    if (len)
        *len = 0;
    if (!block || !sub)
        return FALSE;
    const uint8_t *node = block;
    while (*sub) {
        while (*sub == '\\')
            sub++;
        if (!*sub)
            break;
        size_t n = 0;
        while (sub[n] && sub[n] != '\\')
            n++;
        const WCHAR *key;
        uint16_t length = *(const uint16_t *)node, vlen = *(const uint16_t *)(node + 2);
        const uint8_t *value = node_value(node, &key);
        uint32_t vbytes = *(const uint16_t *)(node + 4) ? vlen * 2u : vlen;
        const uint8_t *child = (const uint8_t *)ALIGN_UP((uintptr_t)(value + vbytes), 4u), *end = node + length;
        const uint8_t *found = NULL;
        while (child + 6 <= end && *(const uint16_t *)child) {
            const WCHAR *ck;
            node_value(child, &ck);
            if (key_equal(ck, sub, n)) {
                found = child;
                break;
            }
            child = (const uint8_t *)ALIGN_UP((uintptr_t)(child + *(const uint16_t *)child), 4u);
        }
        if (!found)
            return FALSE;
        node = found;
        sub += n;
    }
    const WCHAR *key;
    const uint8_t *value = node_value(node, &key);
    uint16_t vlen = *(const uint16_t *)(node + 2);
    if (!vlen)
        return FALSE;
    if (out)
        *out = (void *)value;
    if (len)
        *len = vlen;
    return TRUE;
}

static const struct w32_export version_exports[] = {
    W32_FN(GetFileVersionInfoSizeW), W32_AS("GetFileVersionInfoSizeA", GetFileVersionInfoSizeW),
    W32_FN(GetFileVersionInfoW),     W32_AS("GetFileVersionInfoA", GetFileVersionInfoW),
    W32_FN(GetFileVersionInfoSizeExW), W32_FN(GetFileVersionInfoExW),
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

#define STORE_SIBLINGS 8

struct cert_store {
    uint32_t magic;
    int refs, count, cap, siblings;
    struct cert_context **certs;
    struct cert_store *sibling[STORE_SIBLINGS];
};

#define STORE_MAGIC 0x53435254u

static bool store_add(struct cert_store *s, const BYTE *data, DWORD size, const struct cert_context **out)
{
    if (s->count == s->cap) {
        int cap = s->cap ? s->cap * 2 : 16;
        struct cert_context **grown = w32_realloc(s->certs, sizeof(*grown) * (size_t)cap);
        if (!grown)
            return false;
        s->certs = grown;
        s->cap = cap;
    }
    struct cert_context *c = w32_zalloc(sizeof(*c));
    if (!c)
        return false;
    *c = (struct cert_context){1, data, size, NULL, s};
    s->certs[s->count++] = c;
    if (out)
        *out = c;
    return true;
}

static struct cert_store *store_of(HANDLE h)
{
    struct cert_store *s = h;
    return s && s->magic == STORE_MAGIC ? s : NULL;
}

static HANDLE open_store(bool roots)
{
    struct cert_store *s = w32_zalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->magic = STORE_MAGIC;
    s->refs = 1;
    for (int i = 0; roots && i < tls_roots_count(); i++) {
        const struct x509 *c = tls_root(i);
        store_add(s, c->raw, (DWORD)c->raw_len, NULL);
    }
    return s;
}

static bool is_root_store(const void *name, bool wide)
{
    char n[8] = {0};
    for (int i = 0; name && i < 7; i++) {
        int c = wide ? ((const WCHAR *)name)[i] : ((const char *)name)[i];
        if (!c)
            break;
        n[i] = (char)c;
    }
    return !str_casecmp(n, "ROOT");
}

/* The ROOT system store holds the trusted roots (Mozilla's and /home/certs); every other store starts empty. */
static HANDLE WINAPI CertOpenStore(const char *provider, DWORD encoding, HANDLE prov, DWORD flags, const void *para)
{
    UNUSED(encoding);
    UNUSED(prov);
    UNUSED(flags);
    uintptr_t kind = (uintptr_t)provider;
    bool roots = (kind == 9 || kind == 10 || kind == 13 || kind == 14) && is_root_store(para, kind != 9);
    return open_store(roots);
}

static HANDLE WINAPI CertOpenSystemStoreW(HANDLE prov, LPCWSTR name)
{
    UNUSED(prov);
    return open_store(is_root_store(name, true));
}

static HANDLE WINAPI CertOpenSystemStoreA(HANDLE prov, LPCSTR name)
{
    UNUSED(prov);
    return open_store(is_root_store(name, false));
}

/* A collection sees its siblings live; walking yields the store's own certificates, then each sibling's. */
static const struct cert_context *store_walk(struct cert_store *s, const struct cert_context **prev, DWORD type,
                                             const void *para, int depth);

static const struct cert_context *WINAPI CertEnumCertificatesInStore(HANDLE store, const struct cert_context *prev)
{
    struct cert_store *s = store_of(store);
    const struct cert_context *c = s ? store_walk(s, &prev, 0, NULL, 0) : NULL;
    if (!c)
        w32_set_error(CRYPT_E_NOT_FOUND);
    return c;
}

struct hash_blob {
    DWORD size;
    const BYTE *data;
};

static bool cert_matches(const struct cert_context *c, DWORD type, const void *para)
{
    const struct hash_blob *b = para;
    struct x509 x;
    switch (type) {
    case 0: return true;
    case 0x000d0000u: {
        const struct cert_context *other = para;
        return other && other->size == c->size && !memcmp(other->data, c->data, c->size);
    }
    case 0x00020007u:
    case 0x00020004u:
        if (!b || !x509_parse(c->data, c->size, &x))
            return false;
        return type == 0x00020007u ? b->size == x.subject_len && !memcmp(b->data, x.subject, b->size)
                                   : b->size == x.issuer_len && !memcmp(b->data, x.issuer, b->size);
    case 0x00010000u: {
        uint8_t hash[20];
        if (!b || b->size != 20)
            return false;
        sha1(c->data, c->size, hash);
        return !memcmp(hash, b->data, 20);
    }
    case 0x00200000u: {
        uint8_t hash[32];
        if (!b || b->size != 32)
            return false;
        sha256(c->data, c->size, hash);
        return !memcmp(hash, b->data, 32);
    }
    default: return false;
    }
}

static const struct cert_context *store_walk(struct cert_store *s, const struct cert_context **prev, DWORD type,
                                             const void *para, int depth)
{
    for (int i = 0; i < s->count; i++) {
        if (*prev) {
            if (s->certs[i] == *prev)
                *prev = NULL;
            continue;
        }
        if (cert_matches(s->certs[i], type, para))
            return s->certs[i];
    }
    for (int k = 0; k < s->siblings && depth < 4; k++) {
        const struct cert_context *c = store_walk(s->sibling[k], prev, type, para, depth + 1);
        if (c)
            return c;
    }
    return NULL;
}

static const struct cert_context *WINAPI CertFindCertificateInStore(HANDLE store, DWORD encoding, DWORD flags,
                                                                     DWORD type, const void *para,
                                                                     const struct cert_context *prev)
{
    UNUSED(encoding);
    UNUSED(flags);
    struct cert_store *s = store_of(store);
    const struct cert_context *c = s ? store_walk(s, &prev, type, para, 0) : NULL;
    if (!c)
        w32_set_error(CRYPT_E_NOT_FOUND);
    return c;
}

static BOOL WINAPI CryptHashCertificate(HANDLE prov, DWORD alg, DWORD flags, const BYTE *data, DWORD size, BYTE *out,
                                        DWORD *out_size)
{
    UNUSED(prov);
    UNUSED(flags);
    DWORD need = alg == 0x800cu ? 32 : alg == 0x8004u || !alg ? 20 : 0;
    if (!need || !out_size) {
        w32_set_error(0x80090008u);
        return FALSE;
    }
    if (!out || *out_size < need) {
        *out_size = need;
        if (out)
            w32_set_error(234);
        return out == NULL;
    }
    if (need == 32)
        sha256(data, size, out);
    else
        sha1(data, size, out);
    *out_size = need;
    return TRUE;
}

static BOOL WINAPI CertAddStoreToCollection(HANDLE collection, HANDLE sibling, DWORD flags, DWORD priority)
{
    UNUSED(flags);
    UNUSED(priority);
    struct cert_store *c = store_of(collection), *from = store_of(sibling);
    if (!c || !from || c == from || c->siblings == STORE_SIBLINGS)
        return FALSE;
    from->refs++;
    c->sibling[c->siblings++] = from;
    return TRUE;
}

static BOOL WINAPI CertAddCertificateContextToStore(HANDLE store, const struct cert_context *ctx, DWORD how,
                                                    const struct cert_context **out)
{
    UNUSED(how);
    struct cert_store *s = store_of(store);
    if (out)
        *out = NULL;
    return s && ctx && store_add(s, ctx->data, ctx->size, out);
}

static const struct cert_context *WINAPI CertDuplicateCertificateContext(const struct cert_context *ctx)
{
    return ctx;
}

static void store_release(struct cert_store *s)
{
    if (--s->refs > 0)
        return;
    s->magic = 0;
    for (int i = 0; i < s->siblings; i++)
        store_release(s->sibling[i]);
    for (int i = 0; i < s->count; i++)
        w32_free(s->certs[i]);
    w32_free(s->certs);
    w32_free(s);
}

static BOOL WINAPI CertCloseStore(HANDLE store, DWORD flags)
{
    UNUSED(flags);
    struct cert_store *s = store_of(store);
    if (s)
        store_release(s);
    return TRUE;
}

/* No usage restrictions recorded: zero identifiers with CRYPT_E_NOT_FOUND means "valid for every usage". */
static BOOL WINAPI CertGetEnhancedKeyUsage(const void *ctx, DWORD flags, uint32_t *usage, DWORD *size)
{
    UNUSED(ctx);
    UNUSED(flags);
    if (!size)
        return FALSE;
    if (usage) {
        if (*size < 8) {
            *size = 8;
            w32_set_error(234);
            return FALSE;
        }
        usage[0] = 0;
        usage[1] = 0;
    }
    *size = 8;
    w32_set_error(CRYPT_E_NOT_FOUND);
    return TRUE;
}

static const void *WINAPI CertEnumCRLsInStore(HANDLE store, const void *prev)
{
    UNUSED(store);
    UNUSED(prev);
    w32_set_error(CRYPT_E_NOT_FOUND);
    return NULL;
}

struct name_blob {
    DWORD size;
    const BYTE *data;
};

static BOOL WINAPI CertCompareCertificateName(DWORD encoding, const struct name_blob *a, const struct name_blob *b)
{
    UNUSED(encoding);
    return a && b && a->size == b->size && (!a->size || !memcmp(a->data, b->data, a->size));
}

static const struct w32_export crypt32_exports[] = {
    W32_FN(CertCompareCertificateName),
    W32_AS("CertControlStore", w32_one4),
    W32_AS("CertVerifyTimeValidity", w32_zero2),
    W32_AS("CryptMsgClose", w32_one1),
    W32_FN(CertOpenStore),
    W32_FN(CertOpenSystemStoreW),
    W32_FN(CertOpenSystemStoreA),
    W32_FN(CertEnumCertificatesInStore),
    W32_FN(CertCloseStore),
    W32_FN(CertGetEnhancedKeyUsage),
    W32_FN(CertEnumCRLsInStore),
    W32_AS("CertFreeCertificateContext", w32_one1),
    W32_AS("CertFreeCRLContext", w32_one1),
    W32_FN(CertAddStoreToCollection),
    W32_FN(CertFindCertificateInStore),
    W32_FN(CryptHashCertificate),
    W32_FN(CertAddCertificateContextToStore),
    W32_FN(CertDuplicateCertificateContext),
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

static DWORD WINAPI NotifyAddrChange(HANDLE *handle, void *overlapped)
{
    if (handle)
        *handle = NULL;
    return overlapped ? 997 : 50;
}

static DWORD WINAPI GetInterfaceInfo(void *table, ULONG *len)
{
    UNUSED(table);
    UNUSED(len);
    return 232;
}

static const struct w32_export iphlpapi_exports[] = {
    W32_FN(NotifyAddrChange),
    W32_AS("NotifyRouteChange", NotifyAddrChange),
    W32_AS("CancelIPChangeNotify", w32_one1),
    W32_FN(GetInterfaceInfo),
    W32_AS("IpReleaseAddress", not_supported1),
    W32_AS("IpRenewAddress", not_supported1),
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
    W32_AS("CoInitializeSecurity", com_ok9),
    W32_AS("CoCreateInstance", com_not_registered),
    W32_AS("CoSetProxyBlanket", com_notimpl8),
    W32_AS("ProgIDFromCLSID", not_implemented2),
    W32_AS("CLSIDFromProgID", not_implemented2),
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

static void com_call(void *obj, int slot)
{
    typedef uint32_t WINAPI (*method_fn)(void *);
    if (obj)
        ((method_fn)(*(void ***)obj)[slot])(obj);
}

static uint32_t WINAPI VariantClear(uint16_t *v)
{
    if (!v)
        return 0;
    void *p = *(void **)(v + 4);
    if (v[0] == 8)
        SysFreeString(p);
    else if (v[0] == 9 || v[0] == 13)
        com_call(p, 2);
    memset(v, 0, 16);
    return 0;
}

static uint32_t WINAPI VariantCopy(uint16_t *dst, const uint16_t *src)
{
    if (dst == src)
        return 0;
    VariantClear(dst);
    memcpy(dst, src, 16);
    void *p = *(void *const *)(src + 4);
    if (src[0] == 8 && p) {
        WCHAR *copy = SysAllocStringLen(p, SysStringLen(p));
        *(void **)(dst + 4) = copy;
        if (!copy)
            return 0x8007000eu;
    } else if (src[0] == 9 || src[0] == 13) {
        com_call(p, 1);
    }
    return 0;
}

static char *WINAPI SysAllocStringByteLen(const char *s, UINT n)
{
    uint8_t *b = w32_alloc(4 + (size_t)n + 2);
    if (!b)
        return NULL;
    *(uint32_t *)b = n;
    if (s)
        memcpy(b + 4, s, n);
    else
        memset(b + 4, 0, n);
    b[4 + n] = b[5 + n] = 0;
    return (char *)b + 4;
}

static UINT WINAPI SysStringByteLen(const WCHAR *s)
{
    return s ? *(const uint32_t *)((const uint8_t *)s - 4) : 0;
}

static uint32_t WINAPI VarBstrCmp(const WCHAR *a, const WCHAR *b, DWORD lcid, DWORD flags)
{
    UNUSED(lcid);
    UINT na = SysStringLen(a), nb = SysStringLen(b);
    for (UINT i = 0; i < na && i < nb; i++) {
        WCHAR x = a[i], y = b[i];
        if (flags & 1) {
            x = x >= 'a' && x <= 'z' ? (WCHAR)(x - 32) : x;
            y = y >= 'a' && y <= 'z' ? (WCHAR)(y - 32) : y;
        }
        if (x != y)
            return x < y ? 0 : 2;
    }
    return na == nb ? 1 : na < nb ? 0 : 2;
}

struct safearray {
    uint16_t dims, features;
    uint32_t element_size, locks;
    uint8_t *data;
    uint32_t count;
    int32_t lower;
};

static uint32_t vt_size(uint16_t vt)
{
    switch (vt) {
    case 16:
    case 17: return 1;
    case 2:
    case 11:
    case 18: return 2;
    case 5:
    case 6:
    case 7:
    case 20:
    case 21: return 8;
    case 12: return 16;
    default: return 4;
    }
}

static struct safearray *WINAPI SafeArrayCreateVector(uint16_t vt, int32_t lower, uint32_t count)
{
    uint32_t *block = w32_zalloc(4 + sizeof(struct safearray));
    if (!block)
        return NULL;
    struct safearray *sa = (struct safearray *)(block + 1);
    block[0] = vt;
    sa->dims = 1;
    sa->features = 0x80 | (vt == 8 ? 0x100 : vt == 13 ? 0x200 : vt == 9 ? 0x400 : vt == 12 ? 0x800 : 0);
    sa->element_size = vt_size(vt);
    sa->count = count;
    sa->lower = lower;
    sa->data = w32_zalloc((size_t)count * sa->element_size + 1);
    if (!sa->data) {
        w32_free(block);
        return NULL;
    }
    return sa;
}

static uint16_t sa_vartype(const struct safearray *sa)
{
    return (uint16_t)((const uint32_t *)sa)[-1];
}

static uint32_t WINAPI SafeArrayDestroy(struct safearray *sa)
{
    if (!sa)
        return 0;
    if (sa->locks)
        return 0x8002000du;
    for (uint32_t i = 0; i < sa->count; i++) {
        void *p = sa->data + (size_t)i * sa->element_size;
        if (sa->features & 0x100)
            SysFreeString(*(WCHAR **)p);
        else if (sa->features & 0x600)
            com_call(*(void **)p, 2);
        else if (sa->features & 0x800)
            VariantClear(p);
    }
    w32_free(sa->data);
    w32_free((uint32_t *)sa - 1);
    return 0;
}

static UINT WINAPI SafeArrayGetDim(const struct safearray *sa)
{
    return sa ? sa->dims : 0;
}

static uint32_t WINAPI SafeArrayGetLBound(const struct safearray *sa, UINT dim, int32_t *out)
{
    if (!sa || dim != 1)
        return 0x8002000bu;
    *out = sa->lower;
    return 0;
}

static uint32_t WINAPI SafeArrayGetUBound(const struct safearray *sa, UINT dim, int32_t *out)
{
    if (!sa || dim != 1)
        return 0x8002000bu;
    *out = sa->lower + (int32_t)sa->count - 1;
    return 0;
}

static uint32_t WINAPI SafeArrayGetVartype(const struct safearray *sa, uint16_t *vt)
{
    *vt = sa && (sa->features & 0x80) ? sa_vartype(sa) : 0;
    return sa ? 0 : 0x80070057u;
}

static uint32_t WINAPI SafeArrayAccessData(struct safearray *sa, void **out)
{
    if (!sa)
        return 0x80070057u;
    sa->locks++;
    *out = sa->data;
    return 0;
}

static uint32_t WINAPI SafeArrayUnaccessData(struct safearray *sa)
{
    if (!sa || !sa->locks)
        return 0x8000ffffu;
    sa->locks--;
    return 0;
}

static void *element(struct safearray *sa, const int32_t *index)
{
    int32_t i = index[0] - sa->lower;
    return i >= 0 && (uint32_t)i < sa->count ? sa->data + (size_t)i * sa->element_size : NULL;
}

static uint32_t WINAPI SafeArrayGetElement(struct safearray *sa, const int32_t *index, void *out)
{
    void *p = sa ? element(sa, index) : NULL;
    if (!p)
        return 0x8002000bu;
    if (sa->features & 0x800)
        return VariantCopy(out, p);
    memcpy(out, p, sa->element_size);
    if (sa->features & 0x100)
        *(WCHAR **)out = SysAllocStringLen(*(WCHAR **)p, SysStringLen(*(WCHAR **)p));
    else if (sa->features & 0x600)
        com_call(*(void **)p, 1);
    return 0;
}

static uint32_t WINAPI SafeArrayPutElement(struct safearray *sa, const int32_t *index, void *in)
{
    void *p = sa ? element(sa, index) : NULL;
    if (!p)
        return 0x8002000bu;
    if (sa->features & 0x800)
        return VariantCopy(p, in);
    if (sa->features & 0x100) {
        SysFreeString(*(WCHAR **)p);
        *(WCHAR **)p = in ? SysAllocStringLen(in, SysStringLen(in)) : NULL;
    } else if (sa->features & 0x600) {
        com_call(in, 1);
        com_call(*(void **)p, 2);
        *(void **)p = in;
    } else {
        memcpy(p, in, sa->element_size);
    }
    return 0;
}

static uint32_t WINAPI LoadRegTypeLib(const void *guid, uint16_t major, uint16_t minor, DWORD lcid, void **out)
{
    UNUSED(guid);
    UNUSED(major);
    UNUSED(minor);
    UNUSED(lcid);
    *out = NULL;
    return 0x8002801du;
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
    W32_FN(GetErrorInfo),   W32_FN(VariantCopy),       W32_FN(SysAllocStringByteLen),
    W32_FN(SysStringByteLen), W32_FN(VarBstrCmp),      W32_FN(SafeArrayCreateVector),
    W32_FN(SafeArrayDestroy), W32_FN(SafeArrayGetDim), W32_FN(SafeArrayGetLBound),
    W32_FN(SafeArrayGetUBound), W32_FN(SafeArrayGetVartype), W32_FN(SafeArrayAccessData),
    W32_FN(SafeArrayUnaccessData), W32_FN(SafeArrayGetElement), W32_FN(SafeArrayPutElement),
    W32_FN(LoadRegTypeLib),
};

static const char *const oleaut32_ordinals[] = {
    [2] = "SysAllocString", [4] = "SysAllocStringLen", [6] = "SysFreeString", [7] = "SysStringLen",
    [8] = "VariantInit",    [9] = "VariantClear",      [10] = "VariantCopy",
    [16] = "SafeArrayDestroy", [17] = "SafeArrayGetDim", [19] = "SafeArrayGetUBound",
    [20] = "SafeArrayGetLBound", [23] = "SafeArrayAccessData", [24] = "SafeArrayUnaccessData",
    [25] = "SafeArrayGetElement", [26] = "SafeArrayPutElement", [77] = "SafeArrayGetVartype",
    [149] = "SysStringByteLen", [150] = "SysAllocStringByteLen", [162] = "LoadRegTypeLib",
    [200] = "GetErrorInfo",  [314] = "VarBstrCmp",      [411] = "SafeArrayCreateVector",
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
