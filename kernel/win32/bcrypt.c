#include <synth/crypto.h>
#include <synth/heap.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>

#include "win32.h"

/* CNG (bcrypt.dll) and the CryptoAPI hashes on the kernel's crypto: SHA-1/256/384/512 and MD5, their HMACs, AES in
   ECB and CBC with PKCS#7 padding, random numbers. Algorithm, hash and key handles are slots in a per-process table,
   so a stale or forged handle is refused instead of followed. Asymmetric keys stay unsupported. */

#define STATUS_SUCCESS 0
#define STATUS_INVALID_HANDLE 0xc0000008u
#define STATUS_INVALID_PARAMETER 0xc000000du
#define STATUS_NO_MEMORY 0xc0000017u
#define STATUS_BUFFER_TOO_SMALL 0xc0000023u
#define STATUS_DATA_ERROR 0xc000003eu
#define STATUS_NOT_SUPPORTED 0xc00000bbu
#define STATUS_INVALID_BUFFER_SIZE 0xc0000206u
#define BCRYPT_HMAC_FLAG 0x8
#define BCRYPT_BLOCK_PADDING 0x1
#define KEY_DATA_MAGIC 0x4d42444bu
#define SLOTS 64
#define HANDLE_BASE 0x5bc00000u

enum bc_kind { BC_FREE, BC_HASH_ALG, BC_AES_ALG, BC_RNG_ALG, BC_OTHER_ALG, BC_HASH, BC_KEY };

struct bc_obj {
    enum bc_kind kind;
    const struct hash_alg *hash;
    bool hmac, ecb, done;
    struct hmac mac;
    union hash_ctx ctx;
    struct aes aes;
    uint8_t key[32];
    uint32_t key_len;
};

static W32_STATE struct bc_obj *slots[SLOTS];

static HANDLE bc_new(enum bc_kind kind)
{
    for (int i = 0; i < SLOTS; i++)
        if (!slots[i]) {
            slots[i] = kzalloc(sizeof(struct bc_obj));
            if (!slots[i])
                return NULL;
            slots[i]->kind = kind;
            return (HANDLE)(uintptr_t)(HANDLE_BASE + i + 1);
        }
    return NULL;
}

static struct bc_obj *bc_get(HANDLE h)
{
    uint32_t v = (uint32_t)(uintptr_t)h - HANDLE_BASE - 1;
    return v < SLOTS ? slots[v] : NULL;
}

static void bc_free(HANDLE h)
{
    uint32_t v = (uint32_t)(uintptr_t)h - HANDLE_BASE - 1;
    if (v < SLOTS && slots[v]) {
        memset(slots[v], 0, sizeof(struct bc_obj));
        kfree(slots[v]);
        slots[v] = NULL;
    }
}

static bool wide_is(LPCWSTR w, const char *s)
{
    if (!w)
        return false;
    for (; *s; s++, w++)
        if ((*w | 0x20) != (*s | 0x20) || *w > 0x7f)
            return false;
    return *w == 0;
}

static const struct hash_alg *hash_named(LPCWSTR id)
{
    return wide_is(id, "SHA256") ? &hash_sha256 : wide_is(id, "SHA384") ? &hash_sha384
         : wide_is(id, "SHA512") ? &hash_sha512 : wide_is(id, "SHA1") ? &hash_sha1
         : wide_is(id, "MD5")    ? &hash_md5 : NULL;
}

static LONG WINAPI BCryptOpenAlgorithmProvider(HANDLE *alg, LPCWSTR id, LPCWSTR impl, ULONG flags)
{
    UNUSED(impl);
    const struct hash_alg *h = hash_named(id);
    HANDLE out = bc_new(h ? BC_HASH_ALG : wide_is(id, "AES") ? BC_AES_ALG : wide_is(id, "RNG") ? BC_RNG_ALG : BC_OTHER_ALG);
    if (!out)
        return (LONG)STATUS_NO_MEMORY;
    struct bc_obj *o = bc_get(out);
    o->hash = h;
    o->hmac = h && (flags & BCRYPT_HMAC_FLAG);
    *alg = out;
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptCloseAlgorithmProvider(HANDLE alg, ULONG flags)
{
    UNUSED(flags);
    if (!bc_get(alg))
        return (LONG)STATUS_INVALID_HANDLE;
    bc_free(alg);
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptGenRandom(HANDLE alg, BYTE *buf, ULONG len, ULONG flags)
{
    UNUSED(alg);
    UNUSED(flags);
    random_bytes(buf, len);
    return STATUS_SUCCESS;
}

static LONG put_bytes(const void *value, ULONG size, BYTE *out, ULONG cap, ULONG *result)
{
    if (result)
        *result = size;
    if (!out)
        return STATUS_SUCCESS;
    if (cap < size)
        return (LONG)STATUS_BUFFER_TOO_SMALL;
    memcpy(out, value, size);
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptGetProperty(HANDLE h, LPCWSTR prop, BYTE *out, ULONG cap, ULONG *result, ULONG flags)
{
    UNUSED(flags);
    struct bc_obj *o = bc_get(h);
    if (!o)
        return (LONG)STATUS_INVALID_HANDLE;
    bool aes = o->kind == BC_AES_ALG || o->kind == BC_KEY;
    DWORD v;
    if (wide_is(prop, "ObjectLength"))
        v = 512;
    else if (wide_is(prop, "HashDigestLength") && o->hash)
        v = (DWORD)o->hash->digest;
    else if (wide_is(prop, "HashBlockLength") && o->hash)
        v = (DWORD)o->hash->block;
    else if (wide_is(prop, "BlockLength") && aes)
        v = 16;
    else if (wide_is(prop, "KeyLength") && o->kind == BC_KEY)
        v = o->key_len * 8;
    else if (wide_is(prop, "KeyLengths") && aes) {
        DWORD lengths[3] = {128, 256, 64};
        return put_bytes(lengths, sizeof(lengths), out, cap, result);
    } else if (wide_is(prop, "ChainingMode") && aes) {
        static const char *const names[] = {"ChainingModeCBC", "ChainingModeECB"};
        const char *n = names[o->ecb];
        WCHAR w[20];
        size_t len = strlen(n);
        for (size_t i = 0; i <= len; i++)
            w[i] = (WCHAR)n[i];
        return put_bytes(w, (ULONG)(len + 1) * 2, out, cap, result);
    } else {
        return (LONG)STATUS_NOT_SUPPORTED;
    }
    return put_bytes(&v, 4, out, cap, result);
}

static LONG WINAPI BCryptSetProperty(HANDLE h, LPCWSTR prop, BYTE *value, ULONG size, ULONG flags)
{
    UNUSED(size);
    UNUSED(flags);
    struct bc_obj *o = bc_get(h);
    if (!o)
        return (LONG)STATUS_INVALID_HANDLE;
    if (!wide_is(prop, "ChainingMode") || (o->kind != BC_AES_ALG && o->kind != BC_KEY))
        return (LONG)STATUS_NOT_SUPPORTED;
    if (wide_is((LPCWSTR)value, "ChainingModeCBC"))
        o->ecb = false;
    else if (wide_is((LPCWSTR)value, "ChainingModeECB"))
        o->ecb = true;
    else
        return (LONG)STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

static void hash_start(struct bc_obj *o, const void *secret, size_t len)
{
    o->done = false;
    if (o->hmac)
        hmac_init(&o->mac, o->hash, secret, len);
    else
        o->hash->init(&o->ctx);
}

static void hash_add(struct bc_obj *o, const void *data, size_t len)
{
    if (o->hmac)
        hmac_update(&o->mac, data, len);
    else
        o->hash->update(&o->ctx, data, len);
}

/* the digest of a copy, so the value can be asked for again */
static void hash_value(const struct bc_obj *o, uint8_t *out)
{
    if (o->hmac) {
        struct hmac m = o->mac;
        hmac_final(&m, out);
    } else {
        union hash_ctx c = o->ctx;
        o->hash->final(&c, out);
    }
}

static LONG WINAPI BCryptCreateHash(HANDLE alg, HANDLE *hash, BYTE *object, ULONG object_len, BYTE *secret,
                                    ULONG secret_len, ULONG flags)
{
    UNUSED(object);
    UNUSED(object_len);
    UNUSED(flags);
    struct bc_obj *a = bc_get(alg);
    if (!a || a->kind != BC_HASH_ALG || !hash)
        return (LONG)(a ? STATUS_NOT_SUPPORTED : STATUS_INVALID_HANDLE);
    HANDLE h = bc_new(BC_HASH);
    if (!h)
        return (LONG)STATUS_NO_MEMORY;
    struct bc_obj *o = bc_get(h);
    o->hash = a->hash;
    o->hmac = a->hmac;
    hash_start(o, secret, secret_len);
    *hash = h;
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptHashData(HANDLE hash, BYTE *data, ULONG len, ULONG flags)
{
    UNUSED(flags);
    struct bc_obj *o = bc_get(hash);
    if (!o || o->kind != BC_HASH)
        return (LONG)STATUS_INVALID_HANDLE;
    hash_add(o, data, len);
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptFinishHash(HANDLE hash, BYTE *out, ULONG len, ULONG flags)
{
    UNUSED(flags);
    struct bc_obj *o = bc_get(hash);
    if (!o || o->kind != BC_HASH)
        return (LONG)STATUS_INVALID_HANDLE;
    if (len != o->hash->digest)
        return (LONG)STATUS_INVALID_PARAMETER;
    hash_value(o, out);
    o->done = true;
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptDuplicateHash(HANDLE hash, HANDLE *copy, BYTE *object, ULONG object_len, ULONG flags)
{
    UNUSED(object);
    UNUSED(object_len);
    UNUSED(flags);
    struct bc_obj *o = bc_get(hash);
    if (!o || o->kind != BC_HASH)
        return (LONG)STATUS_INVALID_HANDLE;
    HANDLE h = bc_new(BC_HASH);
    if (!h)
        return (LONG)STATUS_NO_MEMORY;
    *bc_get(h) = *o;
    *copy = h;
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptHash(HANDLE alg, BYTE *secret, ULONG secret_len, BYTE *input, ULONG input_len, BYTE *out,
                              ULONG out_len)
{
    HANDLE h;
    LONG st = BCryptCreateHash(alg, &h, NULL, 0, secret, secret_len, 0);
    if (st)
        return st;
    BCryptHashData(h, input, input_len, 0);
    st = BCryptFinishHash(h, out, out_len, 0);
    bc_free(h);
    return st;
}

static LONG WINAPI BCryptDestroyHandle(HANDLE h)
{
    if (!bc_get(h))
        return (LONG)STATUS_INVALID_HANDLE;
    bc_free(h);
    return STATUS_SUCCESS;
}

static LONG make_key(HANDLE alg, HANDLE *key, const BYTE *secret, ULONG len)
{
    struct bc_obj *a = bc_get(alg);
    if (!a)
        return (LONG)STATUS_INVALID_HANDLE;
    if (a->kind != BC_AES_ALG)
        return (LONG)STATUS_NOT_SUPPORTED;
    if (len != 16 && len != 24 && len != 32)
        return (LONG)STATUS_INVALID_PARAMETER;
    HANDLE h = bc_new(BC_KEY);
    if (!h)
        return (LONG)STATUS_NO_MEMORY;
    struct bc_obj *k = bc_get(h);
    k->ecb = a->ecb;
    k->key_len = len;
    memcpy(k->key, secret, len);
    aes_init(&k->aes, secret, len);
    *key = h;
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptGenerateSymmetricKey(HANDLE alg, HANDLE *key, BYTE *object, ULONG object_len, BYTE *secret,
                                              ULONG secret_len, ULONG flags)
{
    UNUSED(object);
    UNUSED(object_len);
    UNUSED(flags);
    return make_key(alg, key, secret, secret_len);
}

struct key_blob {
    uint32_t magic, version, size;
};

static LONG WINAPI BCryptImportKey(HANDLE alg, HANDLE import_key, LPCWSTR type, HANDLE *key, BYTE *object,
                                   ULONG object_len, BYTE *input, ULONG input_len, ULONG flags)
{
    UNUSED(object);
    UNUSED(object_len);
    UNUSED(flags);
    struct key_blob b;
    if (import_key || !wide_is(type, "KeyDataBlob"))
        return (LONG)STATUS_NOT_SUPPORTED;
    if (input_len < sizeof(b))
        return (LONG)STATUS_INVALID_PARAMETER;
    memcpy(&b, input, sizeof(b));
    if (b.magic != KEY_DATA_MAGIC || b.version != 1 || b.size > input_len - sizeof(b))
        return (LONG)STATUS_INVALID_PARAMETER;
    return make_key(alg, key, input + sizeof(b), b.size);
}

static LONG WINAPI BCryptExportKey(HANDLE key, HANDLE export_key, LPCWSTR type, BYTE *out, ULONG cap, ULONG *result,
                                   ULONG flags)
{
    UNUSED(flags);
    struct bc_obj *k = bc_get(key);
    if (!k || k->kind != BC_KEY)
        return (LONG)STATUS_INVALID_HANDLE;
    if (export_key || !wide_is(type, "KeyDataBlob"))
        return (LONG)STATUS_NOT_SUPPORTED;
    uint8_t blob[sizeof(struct key_blob) + 32];
    struct key_blob b = {KEY_DATA_MAGIC, 1, k->key_len};
    memcpy(blob, &b, sizeof(b));
    memcpy(blob + sizeof(b), k->key, k->key_len);
    return put_bytes(blob, (ULONG)sizeof(b) + k->key_len, out, cap, result);
}

static LONG WINAPI BCryptEncrypt(HANDLE key, BYTE *in, ULONG in_len, void *padding_info, BYTE *iv, ULONG iv_len,
                                 BYTE *out, ULONG out_len, ULONG *result, ULONG flags)
{
    struct bc_obj *k = bc_get(key);
    if (!k || k->kind != BC_KEY)
        return (LONG)STATUS_INVALID_HANDLE;
    if (padding_info)
        return (LONG)STATUS_NOT_SUPPORTED;
    bool pad = flags & BCRYPT_BLOCK_PADDING;
    if (!pad && in_len % 16)
        return (LONG)STATUS_INVALID_BUFFER_SIZE;
    if (!k->ecb && (!iv || iv_len != 16))
        return (LONG)STATUS_INVALID_PARAMETER;
    ULONG total = pad ? (in_len / 16 + 1) * 16 : in_len;
    *result = total;
    if (!out)
        return STATUS_SUCCESS;
    if (out_len < total)
        return (LONG)STATUS_BUFFER_TOO_SMALL;
    uint8_t block[16];
    for (ULONG at = 0; at < total; at += 16) {
        ULONG n = at + 16 <= in_len ? 16 : in_len > at ? in_len - at : 0;
        memcpy(block, in + at, n);
        memset(block + n, (int)(16 - n), 16 - n);
        if (!k->ecb)
            for (int i = 0; i < 16; i++)
                block[i] ^= iv[i];
        aes_encrypt(&k->aes, block, out + at);
        if (!k->ecb)
            memcpy(iv, out + at, 16);
    }
    return STATUS_SUCCESS;
}

static LONG WINAPI BCryptDecrypt(HANDLE key, BYTE *in, ULONG in_len, void *padding_info, BYTE *iv, ULONG iv_len,
                                 BYTE *out, ULONG out_len, ULONG *result, ULONG flags)
{
    struct bc_obj *k = bc_get(key);
    if (!k || k->kind != BC_KEY)
        return (LONG)STATUS_INVALID_HANDLE;
    if (padding_info)
        return (LONG)STATUS_NOT_SUPPORTED;
    if (in_len % 16)
        return (LONG)STATUS_INVALID_BUFFER_SIZE;
    if (!k->ecb && (!iv || iv_len != 16))
        return (LONG)STATUS_INVALID_PARAMETER;
    bool pad = flags & BCRYPT_BLOCK_PADDING;
    if (!out) {
        *result = in_len;
        return STATUS_SUCCESS;
    }
    uint8_t *plain = kmalloc(in_len + 1);
    if (!plain)
        return (LONG)STATUS_NO_MEMORY;
    uint8_t chain[16], next[16];
    if (!k->ecb)
        memcpy(chain, iv, 16);
    for (ULONG at = 0; at < in_len; at += 16) {
        memcpy(next, in + at, 16);
        aes_decrypt(&k->aes, next, plain + at);
        if (!k->ecb) {
            for (int i = 0; i < 16; i++)
                plain[at + i] ^= chain[i];
            memcpy(chain, next, 16);
        }
    }
    ULONG len = in_len;
    if (pad) {
        uint8_t p = in_len ? plain[in_len - 1] : 0;
        bool ok = p >= 1 && p <= 16 && p <= in_len;
        for (ULONG i = 0; ok && i < p; i++)
            ok = plain[in_len - 1 - i] == p;
        if (!ok) {
            kfree(plain);
            return (LONG)STATUS_DATA_ERROR;
        }
        len -= p;
    }
    *result = len;
    LONG st = out_len < len ? (LONG)STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    if (!st) {
        memcpy(out, plain, len);
        if (!k->ecb)
            memcpy(iv, chain, 16);
    }
    kfree(plain);
    return st;
}

static const struct w32_export bcrypt_exports[] = {
    W32_FN(BCryptGenRandom),
    W32_FN(BCryptOpenAlgorithmProvider),
    W32_FN(BCryptCloseAlgorithmProvider),
    W32_FN(BCryptGetProperty),
    W32_FN(BCryptSetProperty),
    W32_FN(BCryptCreateHash),
    W32_FN(BCryptHashData),
    W32_FN(BCryptFinishHash),
    W32_FN(BCryptDuplicateHash),
    W32_FN(BCryptHash),
    W32_AS("BCryptDestroyHash", BCryptDestroyHandle),
    W32_AS("BCryptDestroyKey", BCryptDestroyHandle),
    W32_FN(BCryptGenerateSymmetricKey),
    W32_FN(BCryptImportKey),
    W32_FN(BCryptExportKey),
    W32_FN(BCryptEncrypt),
    W32_FN(BCryptDecrypt),
};

/* CryptoAPI hashes (advapi32): CryptCreateHash with a CALG_ id, the value through HP_HASHVAL */

#define HP_ALGID 1
#define HP_HASHVAL 2
#define HP_HASHSIZE 4
#define NTE_BAD_ALGID 0x80090008u
#define NTE_BAD_HASH 0x80090002u
#define NTE_BAD_TYPE 0x8009000au
#define NTE_BAD_HASH_STATE 0x8009000bu

static const struct hash_alg *calg(uint32_t id)
{
    switch (id) {
    case 0x8003: return &hash_md5;
    case 0x8004: return &hash_sha1;
    case 0x800c: return &hash_sha256;
    case 0x800d: return &hash_sha384;
    case 0x800e: return &hash_sha512;
    default: return NULL;
    }
}

struct capi_hash {
    uint32_t alg;
};

static BOOL WINAPI CryptCreateHash(HANDLE prov, uint32_t alg_id, HANDLE key, DWORD flags, uintptr_t *out)
{
    UNUSED(prov);
    UNUSED(flags);
    const struct hash_alg *h = calg(alg_id);
    if (!h || key) {
        w32_set_error(NTE_BAD_ALGID);
        return FALSE;
    }
    HANDLE hh = bc_new(BC_HASH);
    if (!hh) {
        w32_set_error(8);
        return FALSE;
    }
    struct bc_obj *o = bc_get(hh);
    o->hash = h;
    o->key_len = alg_id;
    hash_start(o, NULL, 0);
    *out = (uintptr_t)hh;
    return TRUE;
}

static BOOL WINAPI CryptHashData(uintptr_t hash, const BYTE *data, DWORD len, DWORD flags)
{
    UNUSED(flags);
    struct bc_obj *o = bc_get((HANDLE)hash);
    if (!o || o->kind != BC_HASH) {
        w32_set_error(NTE_BAD_HASH);
        return FALSE;
    }
    if (o->done) {
        w32_set_error(NTE_BAD_HASH_STATE);
        return FALSE;
    }
    hash_add(o, data, len);
    return TRUE;
}

static BOOL WINAPI CryptGetHashParam(uintptr_t hash, DWORD param, BYTE *data, DWORD *len, DWORD flags)
{
    UNUSED(flags);
    struct bc_obj *o = bc_get((HANDLE)hash);
    if (!o || o->kind != BC_HASH || !len) {
        w32_set_error(NTE_BAD_HASH);
        return FALSE;
    }
    uint8_t value[HASH_MAX];
    DWORD v, size = 4;
    const void *src = &v;
    if (param == HP_ALGID) {
        v = o->key_len;
    } else if (param == HP_HASHSIZE) {
        v = (DWORD)o->hash->digest;
    } else if (param == HP_HASHVAL) {
        hash_value(o, value);
        o->done = true;
        src = value;
        size = (DWORD)o->hash->digest;
    } else {
        w32_set_error(NTE_BAD_TYPE);
        return FALSE;
    }
    if (!data) {
        *len = size;
        return TRUE;
    }
    if (*len < size) {
        *len = size;
        w32_set_error(234);
        return FALSE;
    }
    memcpy(data, src, size);
    *len = size;
    return TRUE;
}

static BOOL WINAPI CryptDestroyHash(uintptr_t hash)
{
    if (!bc_get((HANDLE)hash))
        return FALSE;
    bc_free((HANDLE)hash);
    return TRUE;
}

static const struct w32_export advapi_hash_exports[] = {
    W32_FN(CryptCreateHash),
    W32_FN(CryptHashData),
    W32_FN(CryptGetHashParam),
    W32_FN(CryptDestroyHash),
};

/* CryptStringToBinary: Base64 (with or without a "-----BEGIN" header) and hex text to bytes */

#define CRYPT_STRING_BASE64HEADER 0
#define CRYPT_STRING_BASE64 1
#define CRYPT_STRING_BINARY 2
#define CRYPT_STRING_BASE64REQUESTHEADER 3
#define CRYPT_STRING_HEX 4
#define CRYPT_STRING_BASE64_ANY 6
#define CRYPT_STRING_ANY 7
#define CRYPT_STRING_HEX_ANY 8
#define CRYPT_STRING_BASE64X509CRLHEADER 9
#define CRYPT_STRING_HEXRAW 12

static int b64_value(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
         : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

/* the decoded length, or -1 when the text is not Base64; out may be NULL */
static long b64_decode(const char *s, size_t n, uint8_t *out)
{
    const char *end = s + n;
    const char *begin = NULL;
    for (const char *p = s; p + 10 < end; p++)
        if (!strncmp(p, "-----BEGIN", 10)) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            begin = nl ? nl + 1 : end;
            break;
        }
    if (begin) {
        s = begin;
        for (const char *p = s; p + 8 < end; p++)
            if (!strncmp(p, "-----END", 8)) {
                end = p;
                break;
            }
    }
    uint32_t acc = 0;
    int bits = 0;
    long len = 0;
    bool padding = false;
    for (const char *p = s; p < end; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=') {
            padding = true;
            continue;
        }
        int v = b64_value(c);
        if (v < 0 || padding)
            return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out)
                out[len] = (uint8_t)(acc >> bits);
            len++;
        }
    }
    return len;
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
}

static long hex_decode(const char *s, size_t n, uint8_t *out)
{
    long len = 0;
    int hi = -1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',')
            continue;
        int v = hex_value(c);
        if (v < 0)
            return -1;
        if (hi < 0) {
            hi = v;
            continue;
        }
        if (out)
            out[len] = (uint8_t)(hi << 4 | v);
        len++;
        hi = -1;
    }
    return hi < 0 ? len : -1;
}

static BOOL string_to_binary(const char *s, size_t n, DWORD flags, BYTE *out, DWORD *out_len, DWORD *skip,
                             DWORD *used)
{
    long len = -1;
    DWORD kind = flags;
    switch (flags) {
    case CRYPT_STRING_BASE64HEADER:
    case CRYPT_STRING_BASE64:
    case CRYPT_STRING_BASE64REQUESTHEADER:
    case CRYPT_STRING_BASE64_ANY:
    case CRYPT_STRING_BASE64X509CRLHEADER:
        len = b64_decode(s, n, NULL);
        kind = CRYPT_STRING_BASE64;
        break;
    case CRYPT_STRING_HEX:
    case CRYPT_STRING_HEX_ANY:
    case CRYPT_STRING_HEXRAW:
        len = hex_decode(s, n, NULL);
        kind = CRYPT_STRING_HEX;
        break;
    case CRYPT_STRING_BINARY:
        len = (long)n;
        break;
    case CRYPT_STRING_ANY:
        len = b64_decode(s, n, NULL);
        kind = len >= 0 ? CRYPT_STRING_BASE64 : CRYPT_STRING_BINARY;
        if (len < 0)
            len = (long)n;
        break;
    default:
        w32_set_error(87);
        return FALSE;
    }
    if (len < 0 || !out_len) {
        w32_set_error(13);
        return FALSE;
    }
    if (skip)
        *skip = 0;
    if (used)
        *used = kind;
    if (!out) {
        *out_len = (DWORD)len;
        return TRUE;
    }
    if (*out_len < (DWORD)len) {
        *out_len = (DWORD)len;
        w32_set_error(234);
        return FALSE;
    }
    if (kind == CRYPT_STRING_BASE64)
        b64_decode(s, n, out);
    else if (kind == CRYPT_STRING_HEX)
        hex_decode(s, n, out);
    else
        memcpy(out, s, (size_t)len);
    *out_len = (DWORD)len;
    return TRUE;
}

static BOOL WINAPI CryptStringToBinaryA(LPCSTR s, DWORD n, DWORD flags, BYTE *out, DWORD *out_len, DWORD *skip,
                                        DWORD *used)
{
    if (!s) {
        w32_set_error(87);
        return FALSE;
    }
    return string_to_binary(s, n ? n : strlen(s), flags, out, out_len, skip, used);
}

static BOOL WINAPI CryptStringToBinaryW(LPCWSTR s, DWORD n, DWORD flags, BYTE *out, DWORD *out_len, DWORD *skip,
                                        DWORD *used)
{
    if (!s) {
        w32_set_error(87);
        return FALSE;
    }
    size_t len = n ? n : w32_wcslen(s);
    char *narrow = kmalloc(len + 1);
    if (!narrow) {
        w32_set_error(8);
        return FALSE;
    }
    for (size_t i = 0; i < len; i++)
        narrow[i] = s[i] < 0x80 ? (char)s[i] : '?';
    BOOL ok = string_to_binary(narrow, len, flags, out, out_len, skip, used);
    kfree(narrow);
    return ok;
}

static const struct w32_export crypt32_string_exports[] = {
    W32_FN(CryptStringToBinaryA),
    W32_FN(CryptStringToBinaryW),
};

/* GetUserNameEx: the logon name as DOMAIN\user and the display name; directory names do not exist here */

#define NAME_DISPLAY 3
#define NAME_SAM_COMPATIBLE 2

static bool user_name_ex(int format, char *out, size_t cap)
{
    const char *user = user_current()->name, *host = sh_get(w32->ctx->sh, "HOSTNAME");
    if (format == NAME_DISPLAY) {
        str_copy(out, user, cap);
        return true;
    }
    if (format == NAME_SAM_COMPATIBLE) {
        str_copy(out, host ? host : "SYNTH", cap);
        str_upper(out);
        str_append(out, "\\", cap);
        str_append(out, user, cap);
        return true;
    }
    w32_set_error(1332);
    return false;
}

static BOOL WINAPI GetUserNameExA(int format, LPSTR buf, ULONG *size)
{
    char name[128];
    if (!size || !user_name_ex(format, name, sizeof(name)))
        return FALSE;
    size_t n = strlen(name);
    if (!buf || *size <= n) {
        *size = (ULONG)n + 1;
        w32_set_error(234);
        return FALSE;
    }
    memcpy(buf, name, n + 1);
    *size = (ULONG)n;
    return TRUE;
}

static BOOL WINAPI GetUserNameExW(int format, LPWSTR buf, ULONG *size)
{
    char name[128];
    if (!size || !user_name_ex(format, name, sizeof(name)))
        return FALSE;
    size_t n = strlen(name);
    if (!buf || *size <= n) {
        *size = (ULONG)n + 1;
        w32_set_error(234);
        return FALSE;
    }
    w32_cp_to_wide(name, -1, buf, n + 1, 65001);
    *size = (ULONG)n;
    return TRUE;
}

static const struct w32_export secur32_name_exports[] = {
    W32_FN(GetUserNameExA),
    W32_FN(GetUserNameExW),
};

W32_EXPORTS(bcrypt_module, "bcrypt.dll", bcrypt_exports);
W32_EXPORTS(advapi_hash_module, "advapi32.dll", advapi_hash_exports);
W32_EXPORTS(crypt32_string_module, "crypt32.dll", crypt32_string_exports);
W32_EXPORTS(secur32_name_module, "secur32.dll", secur32_name_exports);
