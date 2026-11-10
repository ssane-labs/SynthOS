#include <synth/string.h>

#include "win64.h"

/* crypt32 for 64-bit programs: CERT_CONTEXT, CERT_INFO and the data blobs hold pointers, so a 64-bit program gets
   a copy of each certificate context in its own layout. The copy stays as long as the program runs and leads back to
   the 32-bit context when the program hands it in again. */

#define P(v) w64_ptr(v)
#define U(v) ((uint32_t)(uintptr_t)(v))

#define CTX64 40
#define INFO64 208

struct wrapped {
    uint32_t real;
    uint8_t *shadow;
};

static W32_STATE struct wrapped *wraps;
static W32_STATE int wrap_count, wrap_cap;

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void put32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

/* CERT_INFO: version, issuer, validity and subject are what the 32-bit code fills in */
static void info_to64(const uint8_t *i, uint8_t *o)
{
    memset(o, 0, INFO64);
    put32(o, rd32(i));
    put32(o + 48, rd32(i + 24));
    put64(o + 56, rd32(i + 28));
    memcpy(o + 64, i + 32, 16);
    put32(o + 80, rd32(i + 48));
    put64(o + 88, rd32(i + 52));
}

static uint64_t wrap(uint32_t real)
{
    if (!real)
        return 0;
    for (int i = 0; i < wrap_count; i++)
        if (wraps[i].real == real)
            return U(wraps[i].shadow);
    if (wrap_count == wrap_cap) {
        int cap = wrap_cap ? wrap_cap * 2 : 64;
        struct wrapped *grown = w32_zalloc(sizeof(*grown) * (size_t)cap);
        if (!grown)
            return 0;
        if (wraps) {
            memcpy(grown, wraps, sizeof(*grown) * (size_t)wrap_count);
            w32_free(wraps);
        }
        wraps = grown;
        wrap_cap = cap;
    }
    uint8_t *s = w32_zalloc(CTX64 + INFO64);
    if (!s)
        return 0;
    const uint8_t *c = (const uint8_t *)(uintptr_t)real;
    put32(s, rd32(c));
    put64(s + 8, rd32(c + 4));
    put32(s + 16, rd32(c + 8));
    if (rd32(c + 12)) {
        info_to64((const uint8_t *)(uintptr_t)rd32(c + 12), s + CTX64);
        put64(s + 24, U(s + CTX64));
    }
    put64(s + 32, rd32(c + 16));
    wraps[wrap_count++] = (struct wrapped){real, s};
    return U(s);
}

static uint32_t unwrap(uint64_t shadow)
{
    for (int i = 0; shadow && i < wrap_count; i++)
        if (U(wraps[i].shadow) == shadow)
            return wraps[i].real;
    return shadow ? U(P(shadow)) : 0;
}

/* CRYPT_DATA_BLOB {DWORD cbData; BYTE *pbData} */
static uint32_t blob32(uint64_t blob64, uint32_t *tmp)
{
    if (!blob64)
        return 0;
    const uint8_t *b = P(blob64);
    tmp[0] = rd32(b);
    tmp[1] = U(P(rd64(b + 8)));
    return U(tmp);
}

static uint64_t h_enum(uint64_t *a, const uint64_t *f, void *t)
{
    return wrap((uint32_t)T32(t, U(P(a[0])), unwrap(a[1])));
}

static uint64_t h_find(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t type = (uint32_t)a[3], tmp[2], para;
    if (type == 0x000d0000u)
        para = unwrap(a[4]);
    else if (type == 0x00010000u || type == 0x00200000u || type == 0x00020007u || type == 0x00020004u)
        para = blob32(a[4], tmp);
    else
        para = U(P(a[4]));
    return wrap((uint32_t)T32(t, U(P(a[0])), (uint32_t)a[1], (uint32_t)a[2], type, para, unwrap(a[5])));
}

static uint64_t h_duplicate(uint64_t *a, const uint64_t *f, void *t)
{
    return wrap((uint32_t)T32(t, unwrap(a[0])));
}

static uint64_t h_create(uint64_t *a, const uint64_t *f, void *t)
{
    return wrap((uint32_t)T32(t, (uint32_t)a[0], U(P(a[1])), (uint32_t)a[2]));
}

static uint64_t h_free(uint64_t *a, const uint64_t *f, void *t)
{
    return 1;
}

static uint64_t h_add_context(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t out = 0;
    uint32_t ok = (uint32_t)T32(t, U(P(a[0])), unwrap(a[1]), (uint32_t)a[2], a[3] ? U(&out) : 0);
    if (ok && a[3])
        put64(P(a[3]), wrap(out));
    return ok;
}

static uint64_t h_compare_name(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t x[2], y[2];
    return (uint32_t)T32(t, (uint32_t)a[0], blob32(a[1], x), blob32(a[2], y));
}

/* CERT_ENHKEY_USAGE {DWORD cUsageIdentifier; LPSTR *rgpszUsageIdentifier}: the 32-bit code reports no usages */
static uint64_t h_key_usage(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t *size = a[3] ? P(a[3]) : NULL;
    if (!size)
        return 0;
    if (a[2]) {
        if (rd32(size) < 16) {
            put32(size, 16);
            w32_set_error(234);
            return 0;
        }
        memset(P(a[2]), 0, 16);
    }
    put32(size, 16);
    return 1;
}

/* chain building works on 32-bit chain contexts; a 64-bit program is told there is none */
static uint64_t h_no_chain(uint64_t *a, const uint64_t *f, void *t)
{
    if (a[7])
        put64(P(a[7]), 0);
    w32_set_error(0x80092004u);
    return 0;
}

static uint64_t h_chain_policy(uint64_t *a, const uint64_t *f, void *t)
{
    w32_set_error(0x80092004u);
    return 0;
}

static const struct w64_export cert_exports[] = {
    W64_FN("CertEnumCertificatesInStore", h_enum),
    W64_FN("CertFindCertificateInStore", h_find),
    W64_FN("CertDuplicateCertificateContext", h_duplicate),
    W64_FN("CertCreateCertificateContext", h_create),
    W64_FN("CertFreeCertificateContext", h_free),
    W64_FN("CertAddCertificateContextToStore", h_add_context),
    W64_FN("CertCompareCertificateName", h_compare_name),
    W64_FN("CertGetEnhancedKeyUsage", h_key_usage),
    W64_FN("CertGetCertificateChain", h_no_chain),
    W64_FN("CertVerifyCertificateChainPolicy", h_chain_policy),
};
W64_EXPORTS(cert_module, "crypt32.dll", cert_exports);
