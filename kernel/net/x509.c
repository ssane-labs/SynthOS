#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>

enum { SIG_RSA_SHA256 = 1, SIG_RSA_SHA384, SIG_RSA_SHA512, SIG_ECDSA_SHA256, SIG_ECDSA_SHA384, SIG_ECDSA_SHA512 };

struct der {
    const uint8_t *p;
    size_t n;
};

static const uint8_t oid_rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};
static const uint8_t oid_rsa_sha256[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b};
static const uint8_t oid_rsa_sha384[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c};
static const uint8_t oid_rsa_sha512[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d};
static const uint8_t oid_ec[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
static const uint8_t oid_p256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
static const uint8_t oid_p384[] = {0x2b, 0x81, 0x04, 0x00, 0x22};
static const uint8_t oid_ecdsa_sha256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};
static const uint8_t oid_ecdsa_sha384[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03};
static const uint8_t oid_ecdsa_sha512[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04};
static const uint8_t oid_cn[] = {0x55, 0x04, 0x03};
static const uint8_t oid_san[] = {0x55, 0x1d, 0x11};
static const uint8_t oid_basic[] = {0x55, 0x1d, 0x13};

static bool der_read(struct der *d, uint8_t *tag, struct der *value, struct der *whole)
{
    if (d->n < 2)
        return false;
    size_t len, hdr = 2;
    uint8_t l = d->p[1];
    if (l < 0x80) {
        len = l;
    } else {
        size_t k = l & 0x7f;
        if (!k || k > 4 || d->n < 2 + k)
            return false;
        len = 0;
        for (size_t i = 0; i < k; i++)
            len = len << 8 | d->p[2 + i];
        hdr = 2 + k;
    }
    if (len > d->n - hdr)
        return false;
    *tag = d->p[0];
    if (whole) {
        whole->p = d->p;
        whole->n = hdr + len;
    }
    value->p = d->p + hdr;
    value->n = len;
    d->p += hdr + len;
    d->n -= hdr + len;
    return true;
}

static bool der_expect(struct der *d, uint8_t tag, struct der *value, struct der *whole)
{
    struct der save = *d;
    uint8_t t;
    if (der_read(d, &t, value, whole) && t == tag)
        return true;
    *d = save;
    return false;
}

static bool oid_is(const struct der *o, const uint8_t *oid, size_t len)
{
    return o->n == len && memcmp(o->p, oid, len) == 0;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int digits(const uint8_t *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = v * 10 + (p[i] - '0');
    return v;
}

static bool parse_time(struct der *d, int64_t *out)
{
    uint8_t tag;
    struct der v;
    if (!der_read(d, &tag, &v, NULL))
        return false;
    const uint8_t *p = v.p;
    int year;
    if (tag == 0x17 && v.n >= 12) {
        year = digits(p, 2);
        year += year < 50 ? 2000 : 1900;
        p += 2;
    } else if (tag == 0x18 && v.n >= 14) {
        year = digits(p, 4);
        p += 4;
    } else {
        return false;
    }
    int64_t days = days_from_civil(year, digits(p, 2), digits(p + 2, 2));
    *out = days * 86400 + digits(p + 4, 2) * 3600 + digits(p + 6, 2) * 60 + digits(p + 8, 2);
    return true;
}

int64_t x509_now(void)
{
    struct rtc_time t;
    hal_rtc(&t);
    return days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

static void parse_cn(const struct der *name, char *out, size_t cap)
{
    struct der rdns = *name, set, atv, oid, value;
    uint8_t tag;
    out[0] = 0;
    while (der_expect(&rdns, 0x31, &set, NULL)) {
        while (der_expect(&set, 0x30, &atv, NULL)) {
            if (!der_expect(&atv, 0x06, &oid, NULL) || !der_read(&atv, &tag, &value, NULL))
                continue;
            if (oid_is(&oid, oid_cn, sizeof(oid_cn))) {
                size_t k = MIN(value.n, cap - 1);
                memcpy(out, value.p, k);
                out[k] = 0;
            }
        }
    }
}

static bool parse_spki(struct der *spki, struct x509 *c)
{
    struct der alg, oid, bits;
    if (!der_expect(spki, 0x30, &alg, NULL) || !der_expect(&alg, 0x06, &oid, NULL) ||
        !der_expect(spki, 0x03, &bits, NULL) || bits.n < 2)
        return false;
    struct der key = {bits.p + 1, bits.n - 1};
    if (oid_is(&oid, oid_rsa, sizeof(oid_rsa))) {
        struct der seq, n, e;
        if (!der_expect(&key, 0x30, &seq, NULL) || !der_expect(&seq, 0x02, &n, NULL) || !der_expect(&seq, 0x02, &e, NULL))
            return false;
        c->key = KEY_RSA;
        c->rsa_n = n.p;
        c->rsa_n_len = n.n;
        c->rsa_e = e.p;
        c->rsa_e_len = e.n;
        return true;
    }
    if (oid_is(&oid, oid_ec, sizeof(oid_ec))) {
        struct der curve;
        if (!der_expect(&alg, 0x06, &curve, NULL))
            return false;
        if (oid_is(&curve, oid_p256, sizeof(oid_p256)))
            c->key = KEY_EC_P256;
        else if (oid_is(&curve, oid_p384, sizeof(oid_p384)))
            c->key = KEY_EC_P384;
        c->ec_point = key.p;
        c->ec_point_len = key.n;
        return true;
    }
    c->key = KEY_NONE;
    return true;
}

static int sig_alg(const struct der *oid)
{
    if (oid_is(oid, oid_rsa_sha256, sizeof(oid_rsa_sha256)))
        return SIG_RSA_SHA256;
    if (oid_is(oid, oid_rsa_sha384, sizeof(oid_rsa_sha384)))
        return SIG_RSA_SHA384;
    if (oid_is(oid, oid_rsa_sha512, sizeof(oid_rsa_sha512)))
        return SIG_RSA_SHA512;
    if (oid_is(oid, oid_ecdsa_sha256, sizeof(oid_ecdsa_sha256)))
        return SIG_ECDSA_SHA256;
    if (oid_is(oid, oid_ecdsa_sha384, sizeof(oid_ecdsa_sha384)))
        return SIG_ECDSA_SHA384;
    if (oid_is(oid, oid_ecdsa_sha512, sizeof(oid_ecdsa_sha512)))
        return SIG_ECDSA_SHA512;
    return 0;
}

static void parse_extensions(struct der *exts, struct x509 *c)
{
    struct der list, ext, oid, value, crit;
    if (!der_expect(exts, 0x30, &list, NULL))
        return;
    while (der_expect(&list, 0x30, &ext, NULL)) {
        if (!der_expect(&ext, 0x06, &oid, NULL))
            continue;
        der_expect(&ext, 0x01, &crit, NULL);
        if (!der_expect(&ext, 0x04, &value, NULL))
            continue;
        if (oid_is(&oid, oid_san, sizeof(oid_san))) {
            struct der names;
            if (der_expect(&value, 0x30, &names, NULL)) {
                c->san = names.p;
                c->san_len = names.n;
            }
        } else if (oid_is(&oid, oid_basic, sizeof(oid_basic))) {
            struct der seq, flag;
            if (der_expect(&value, 0x30, &seq, NULL) && der_expect(&seq, 0x01, &flag, NULL) && flag.n == 1)
                c->ca = flag.p[0] != 0;
        }
    }
}

bool x509_parse(const uint8_t *der, size_t len, struct x509 *c)
{
    memset(c, 0, sizeof(*c));
    struct der all = {der, len}, cert, tbs, tbs_whole, alg, oid, sig, field;
    if (!der_expect(&all, 0x30, &cert, NULL))
        return false;
    c->raw = der;
    c->raw_len = (size_t)(cert.p + cert.n - der);
    if (!der_expect(&cert, 0x30, &tbs, &tbs_whole) || !der_expect(&cert, 0x30, &alg, NULL) ||
        !der_expect(&alg, 0x06, &oid, NULL) || !der_expect(&cert, 0x03, &sig, NULL) || sig.n < 2)
        return false;
    c->tbs = tbs_whole.p;
    c->tbs_len = tbs_whole.n;
    c->sig_alg = sig_alg(&oid);
    c->sig = sig.p + 1;
    c->sig_len = sig.n - 1;
    der_expect(&tbs, 0xa0, &field, NULL);
    struct der serial, inner_alg, issuer_v, issuer_w, validity, subject_v, subject_w, spki;
    if (!der_expect(&tbs, 0x02, &serial, NULL) || !der_expect(&tbs, 0x30, &inner_alg, NULL) ||
        !der_expect(&tbs, 0x30, &issuer_v, &issuer_w) || !der_expect(&tbs, 0x30, &validity, NULL) ||
        !der_expect(&tbs, 0x30, &subject_v, &subject_w) || !der_expect(&tbs, 0x30, &spki, NULL))
        return false;
    c->issuer = issuer_w.p;
    c->issuer_len = issuer_w.n;
    c->subject = subject_w.p;
    c->subject_len = subject_w.n;
    if (!parse_time(&validity, &c->not_before) || !parse_time(&validity, &c->not_after))
        return false;
    parse_cn(&subject_v, c->cn, sizeof(c->cn));
    if (!parse_spki(&spki, c))
        return false;
    struct der exts;
    while (tbs.n) {
        uint8_t tag;
        if (!der_read(&tbs, &tag, &exts, NULL))
            break;
        if (tag == 0xa3)
            parse_extensions(&exts, c);
    }
    return true;
}

bool x509_verify_signature(const struct x509 *cert, const struct x509 *issuer)
{
    const struct hash_alg *h;
    switch (cert->sig_alg) {
    case SIG_RSA_SHA256: case SIG_ECDSA_SHA256: h = &hash_sha256; break;
    case SIG_RSA_SHA384: case SIG_ECDSA_SHA384: h = &hash_sha384; break;
    case SIG_RSA_SHA512: case SIG_ECDSA_SHA512: h = &hash_sha512; break;
    default: return false;
    }
    uint8_t digest[HASH_MAX];
    hash_digest(h, cert->tbs, cert->tbs_len, digest);
    if (cert->sig_alg <= SIG_RSA_SHA512)
        return issuer->key == KEY_RSA &&
               rsa_verify_pkcs1(issuer->rsa_n, issuer->rsa_n_len, issuer->rsa_e, issuer->rsa_e_len, h, digest,
                                cert->sig, cert->sig_len);
    if (issuer->key != KEY_EC_P256 && issuer->key != KEY_EC_P384)
        return false;
    struct der sig = {cert->sig, cert->sig_len}, seq, r, s;
    if (!der_expect(&sig, 0x30, &seq, NULL) || !der_expect(&seq, 0x02, &r, NULL) || !der_expect(&seq, 0x02, &s, NULL))
        return false;
    return ecdsa_verify(issuer->key == KEY_EC_P256 ? EC_P256 : EC_P384, issuer->ec_point, issuer->ec_point_len, digest,
                        h->digest, r.p, r.n, s.p, s.n);
}

static bool name_matches(const char *pattern, size_t plen, const char *host)
{
    size_t hlen = strlen(host);
    if (plen > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot || dot == host)
            return false;
        size_t rest = hlen - (size_t)(dot - host);
        return rest == plen - 1 && str_ncasecmp(dot, pattern + 1, rest) == 0;
    }
    return plen == hlen && str_ncasecmp(pattern, host, hlen) == 0;
}

static bool parse_ipv4(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        if (!is_digit(*s))
            return false;
        int v = 0;
        while (is_digit(*s))
            v = v * 10 + (*s++ - '0');
        if (v > 255 || (i < 3 && *s++ != '.'))
            return false;
        out[i] = (uint8_t)v;
    }
    return *s == 0;
}

bool x509_matches_host(const struct x509 *c, const char *host)
{
    uint8_t ip[4];
    bool is_ip = parse_ipv4(host, ip);
    if (c->san) {
        struct der names = {c->san, c->san_len}, v;
        uint8_t tag;
        while (der_read(&names, &tag, &v, NULL)) {
            if (tag == 0x82 && !is_ip && name_matches((const char *)v.p, v.n, host))
                return true;
            if (tag == 0x87 && is_ip && v.n == 4 && memcmp(v.p, ip, 4) == 0)
                return true;
        }
        return false;
    }
    return !is_ip && name_matches(c->cn, strlen(c->cn), host);
}

static int b64(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

int x509_load_pem(const char *text, size_t len, void (*fn)(const uint8_t *der, size_t len, void *ctx), void *ctx)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----", end[] = "-----END CERTIFICATE-----";
    int count = 0;
    const char *p = text, *limit = text + len;
    while (p < limit) {
        const char *b = NULL;
        for (const char *q = p; q + sizeof(begin) - 1 <= limit; q++)
            if (*q == '-' && memcmp(q, begin, sizeof(begin) - 1) == 0) {
                b = q + sizeof(begin) - 1;
                break;
            }
        if (!b)
            break;
        const char *e = NULL;
        for (const char *q = b; q + sizeof(end) - 1 <= limit; q++)
            if (*q == '-' && memcmp(q, end, sizeof(end) - 1) == 0) {
                e = q;
                break;
            }
        if (!e)
            break;
        uint8_t *der = kmalloc((size_t)(e - b) * 3 / 4 + 4);
        if (!der)
            return count;
        size_t n = 0;
        uint32_t acc = 0;
        int bits = 0;
        for (const char *q = b; q < e; q++) {
            int v = b64(*q);
            if (v < 0)
                continue;
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der[n++] = (uint8_t)(acc >> bits);
            }
        }
        fn(der, n, ctx);
        kfree(der);
        count++;
        p = e + sizeof(end) - 1;
    }
    return count;
}

#define MAX_ROOTS 256

static struct x509 roots[MAX_ROOTS];
static int root_count;
static bool roots_loaded;

static void add_root(const uint8_t *der, size_t len, void *ctx)
{
    UNUSED(ctx);
    if (root_count >= MAX_ROOTS)
        return;
    uint8_t *copy = kmalloc(len);
    if (!copy)
        return;
    memcpy(copy, der, len);
    if (x509_parse(copy, len, &roots[root_count]))
        root_count++;
    else
        kfree(copy);
}

static void load_file(const char *path)
{
    char *data;
    size_t size;
    if (vfs_read_all(path, &data, &size) != 0)
        return;
    if (x509_load_pem(data, size, add_root, NULL) == 0 && size > 4 && (uint8_t)data[0] == 0x30)
        add_root((const uint8_t *)data, size, NULL);
    kfree(data);
}

static void load_dir(const char *dir)
{
    struct vdir d;
    struct vfs_dirent e;
    if (vfs_opendir(dir, &d) != 0)
        return;
    while (vfs_readdir(&d, &e) == 0) {
        size_t n = strlen(e.name);
        if (n < 5 || (str_casecmp(e.name + n - 4, ".pem") && str_casecmp(e.name + n - 4, ".crt") &&
                      str_casecmp(e.name + n - 4, ".cer")))
            continue;
        char path[PATH_MAX_LEN];
        ksnprintf(path, sizeof(path), "%s/%s", dir, e.name);
        load_file(path);
    }
    vfs_closedir(&d);
}

int tls_roots_load(void)
{
    for (int i = 0; i < root_count; i++)
        kfree((void *)roots[i].raw);
    root_count = 0;
    load_dir("/etc/certs");
    load_dir("/home/certs");
    roots_loaded = true;
    return root_count;
}

int tls_roots_count(void)
{
    if (!roots_loaded)
        tls_roots_load();
    return root_count;
}

const struct x509 *tls_root(int index)
{
    return index >= 0 && index < tls_roots_count() ? &roots[index] : NULL;
}

const struct x509 *tls_find_root(const uint8_t *subject, size_t len, const struct x509 *child)
{
    int n = tls_roots_count();
    for (int i = 0; i < n; i++)
        if (roots[i].subject_len == len && memcmp(roots[i].subject, subject, len) == 0 &&
            (!child || x509_verify_signature(child, &roots[i])))
            return &roots[i];
    return NULL;
}

bool tls_root_contains(const struct x509 *c)
{
    int n = tls_roots_count();
    for (int i = 0; i < n; i++)
        if (roots[i].raw_len == c->raw_len && memcmp(roots[i].raw, c->raw, c->raw_len) == 0)
            return true;
    return false;
}
