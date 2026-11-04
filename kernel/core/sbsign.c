#include <synth/command.h>
#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/recovery.h>
#include <synth/sbsign.h>
#include <synth/sha256.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

/* Secure Boot key of this computer. Release builds may each be signed with another key, and shim then stops at
   MokManager after every update. The first update that brings a loader signed with another key than the installed
   one makes a key here (RSA-2048, self-signed certificate); from then on every new loader is signed with it, so its
   certificate (\SYNTHOS\synthos-key.cer) is enrolled once and stays valid. Windows programs cannot reach KEY_DIR. */

#define KEY_LEN 256
#define KEY_FILE SB_KEY_DIR "/key"
#define CERT_FILE SB_KEY_DIR "/key.cer"
#define SIG_MAX 2048

static const uint8_t key_magic[8] = {'S', 'Y', 'N', 'S', 'B', 'K', 'Y', '1'};
static const uint8_t oid_sha256[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};
static const uint8_t oid_rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};
static const uint8_t oid_rsa_sha256[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b};
static const uint8_t oid_signed_data[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02};
static const uint8_t oid_content_type[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x03};
static const uint8_t oid_message_digest[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x04};
static const uint8_t oid_spc_indirect[] = {0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04};
static const uint8_t oid_spc_pe_image[] = {0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0f};
static const uint8_t oid_cn[] = {0x55, 0x04, 0x03};
static const uint8_t oid_ski[] = {0x55, 0x1d, 0x0e};
static const uint8_t oid_basic[] = {0x55, 0x1d, 0x13};
static const uint8_t oid_aki[] = {0x55, 0x1d, 0x23};

struct key {
    uint8_t n[KEY_LEN], d[KEY_LEN];
    uint8_t *cert;
    size_t cert_len;
};

static bool created;

/* DER writer: a constructed value is opened with begin() and closed with end(), which puts its header in front */
struct der_out {
    uint8_t *p;
    size_t n, cap;
    bool bad;
};

static void put(struct der_out *b, const void *s, size_t n)
{
    if (b->n + n > b->cap) {
        b->bad = true;
        return;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static size_t begin(struct der_out *b)
{
    return b->n;
}

static void end(struct der_out *b, size_t at, uint8_t tag)
{
    size_t len = b->n - at, h = len < 0x80 ? 2 : len < 0x100 ? 3 : 4;
    if (b->bad || b->n + h > b->cap) {
        b->bad = true;
        return;
    }
    memmove(b->p + at + h, b->p + at, len);
    uint8_t *q = b->p + at;
    q[0] = tag;
    if (h == 2) {
        q[1] = (uint8_t)len;
    } else if (h == 3) {
        q[1] = 0x81;
        q[2] = (uint8_t)len;
    } else {
        q[1] = 0x82;
        q[2] = (uint8_t)(len >> 8);
        q[3] = (uint8_t)len;
    }
    b->n += h;
}

static void prim(struct der_out *b, uint8_t tag, const void *s, size_t n)
{
    size_t at = begin(b);
    put(b, s, n);
    end(b, at, tag);
}

static void unsigned_int(struct der_out *b, const uint8_t *s, size_t n)
{
    while (n > 1 && !*s)
        s++, n--;
    size_t at = begin(b);
    if (*s & 0x80)
        put(b, "", 1);
    put(b, s, n);
    end(b, at, 0x02);
}

static void small_int(struct der_out *b, uint8_t v)
{
    prim(b, 0x02, &v, 1);
}

static void alg(struct der_out *b, const uint8_t *oid, size_t len)
{
    size_t at = begin(b);
    prim(b, 0x06, oid, len);
    put(b, "\x05\x00", 2);
    end(b, at, 0x30);
}

/* the parts of a PE image that the Authenticode hash leaves out */
struct pe {
    size_t checksum, cert_dir, headers, table;
    int sections;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool pe_parse(const uint8_t *d, size_t len, struct pe *pe)
{
    if (len < 0x40)
        return false;
    size_t off = rd32(d + 0x3c);
    if (off > len - 24 || memcmp(d + off, "PE\0\0", 4))
        return false;
    pe->sections = d[off + 6] | d[off + 7] << 8;
    size_t opt_size = (size_t)(d[off + 20] | d[off + 21] << 8), opt = off + 24;
    if (opt_size < 136 || opt + opt_size > len)
        return false;
    size_t dirs = opt + ((d[opt] | d[opt + 1] << 8) == 0x20b ? 112 : 96);
    pe->checksum = opt + 64;
    pe->cert_dir = dirs + 4 * 8;
    pe->headers = rd32(d + opt + 60);
    pe->table = opt + opt_size;
    return pe->cert_dir + 8 <= opt + opt_size && pe->headers <= len && pe->headers >= pe->cert_dir + 8 &&
           pe->table + 40 * (size_t)pe->sections <= len;
}

static bool pe_digest(const uint8_t *d, size_t len, const struct pe *pe, uint8_t out[32])
{
    struct sha256 h;
    sha256_init(&h);
    sha256_update(&h, d, pe->checksum);
    sha256_update(&h, d + pe->checksum + 4, pe->cert_dir - pe->checksum - 4);
    sha256_update(&h, d + pe->cert_dir + 8, pe->headers - pe->cert_dir - 8);
    uint32_t ptr[96], size[96];
    int count = 0;
    for (int i = 0; i < pe->sections && count < 96; i++) {
        const uint8_t *s = d + pe->table + 40 * (size_t)i;
        uint32_t sz = rd32(s + 16), p = rd32(s + 20);
        if (!sz)
            continue;
        if (p > len || sz > len - p)
            return false;
        int j = count++;
        for (; j > 0 && ptr[j - 1] > p; j--)
            ptr[j] = ptr[j - 1], size[j] = size[j - 1];
        ptr[j] = p;
        size[j] = sz;
    }
    size_t done = pe->headers;
    for (int i = 0; i < count; i++) {
        sha256_update(&h, d + ptr[i], size[i]);
        done += size[i];
    }
    uint32_t cert_off = rd32(d + pe->cert_dir), cert_size = rd32(d + pe->cert_dir + 4);
    size_t stop = cert_off && cert_size <= len ? len - cert_size : len;
    if (stop > done)
        sha256_update(&h, d + done, stop - done);
    sha256_final(&h, out);
    return true;
}

struct der_in {
    const uint8_t *p;
    size_t n;
};

static bool der_next(struct der_in *d, uint8_t *tag, struct der_in *value, struct der_in *whole)
{
    if (d->n < 2)
        return false;
    size_t len = d->p[1], hdr = 2;
    if (len >= 0x80) {
        size_t k = len & 0x7f;
        if (!k || k > 3 || d->n < 2 + k)
            return false;
        len = 0;
        for (size_t i = 0; i < k; i++)
            len = len << 8 | d->p[2 + i];
        hdr += k;
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

static bool der_into(struct der_in *d, uint8_t want)
{
    uint8_t tag;
    struct der_in v;
    if (!der_next(d, &tag, &v, NULL) || tag != want)
        return false;
    *d = v;
    return true;
}

int sb_cert(const uint8_t *efi, size_t len, const uint8_t **cert, size_t *cert_len)
{
    struct pe pe;
    if (!pe_parse(efi, len, &pe))
        return E_INVAL;
    uint32_t off = rd32(efi + pe.cert_dir), size = rd32(efi + pe.cert_dir + 4);
    if (!off || size < 8 || off > len || size > len - off)
        return E_NOENT;
    struct der_in d = {efi + off + 8, size - 8}, v;
    uint8_t tag;
    if (!der_into(&d, 0x30) || !der_next(&d, &tag, &v, NULL) || tag != 0x06 || !der_into(&d, 0xa0) ||
        !der_into(&d, 0x30))
        return E_INVAL;
    for (int i = 0; i < 3; i++)
        if (!der_next(&d, &tag, &v, NULL))
            return E_INVAL;
    struct der_in whole;
    if (!der_into(&d, 0xa0) || !der_next(&d, &tag, &v, &whole) || tag != 0x30)
        return E_INVAL;
    *cert = whole.p;
    *cert_len = whole.n;
    return 0;
}

static void name(struct der_out *b)
{
    static const char cn[] = "SynthOS Secure Boot (this computer)";
    size_t seq = begin(b), set = begin(b), atv = begin(b);
    prim(b, 0x06, oid_cn, sizeof(oid_cn));
    prim(b, 0x0c, cn, sizeof(cn) - 1);
    end(b, atv, 0x30);
    end(b, set, 0x31);
    end(b, seq, 0x30);
}

static void utc_time(struct der_out *b, uint32_t epoch)
{
    struct rtc_time t;
    time_to_rtc(epoch, &t);
    if (t.year < 2000 || t.year > 2049)
        t.year = 2026;
    char s[16];
    ksnprintf(s, sizeof(s), "%02u%02u%02u%02u%02u%02uZ", (unsigned)(t.year % 100), (unsigned)t.month, (unsigned)t.day,
              (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    prim(b, 0x17, s, 13);
}

static bool sign_hash(const struct key *k, const uint8_t hash[32], struct der_out *b)
{
    static uint8_t sig[KEY_LEN];
    if (!rsa_sign_pkcs1(k->n, k->d, KEY_LEN, &hash_sha256, hash, sig))
        return false;
    put(b, sig, KEY_LEN);
    return true;
}

static int make_cert(struct key *k)
{
    struct der_out b = {kmalloc(SIG_MAX), 0, SIG_MAX, false};
    if (!b.p)
        return E_NOMEM;
    uint8_t serial[16], keyid[20], hash[32];
    random_bytes(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7f) | 0x01;
    size_t cert = begin(&b), tbs = begin(&b), at = begin(&b);
    small_int(&b, 2);
    end(&b, at, 0xa0);
    unsigned_int(&b, serial, sizeof(serial));
    alg(&b, oid_rsa_sha256, sizeof(oid_rsa_sha256));
    name(&b);
    at = begin(&b);
    utc_time(&b, time_now());
    prim(&b, 0x17, "491231235959Z", 13);
    end(&b, at, 0x30);
    name(&b);
    size_t spki = begin(&b);
    alg(&b, oid_rsa, sizeof(oid_rsa));
    size_t bits = begin(&b);
    put(&b, "", 1);
    size_t pub = begin(&b);
    unsigned_int(&b, k->n, KEY_LEN);
    prim(&b, 0x02, "\x01\x00\x01", 3);
    end(&b, pub, 0x30);
    hash_digest(&hash_sha1, b.p + pub, b.n - pub, keyid);
    end(&b, bits, 0x03);
    end(&b, spki, 0x30);
    size_t exts = begin(&b), list = begin(&b);
    at = begin(&b);
    prim(&b, 0x06, oid_ski, sizeof(oid_ski));
    size_t oct = begin(&b);
    prim(&b, 0x04, keyid, sizeof(keyid));
    end(&b, oct, 0x04);
    end(&b, at, 0x30);
    at = begin(&b);
    prim(&b, 0x06, oid_aki, sizeof(oid_aki));
    oct = begin(&b);
    size_t seq = begin(&b);
    prim(&b, 0x80, keyid, sizeof(keyid));
    end(&b, seq, 0x30);
    end(&b, oct, 0x04);
    end(&b, at, 0x30);
    at = begin(&b);
    prim(&b, 0x06, oid_basic, sizeof(oid_basic));
    put(&b, "\x01\x01\xff\x04\x05\x30\x03\x01\x01\xff", 10);
    end(&b, at, 0x30);
    end(&b, list, 0x30);
    end(&b, exts, 0xa3);
    end(&b, tbs, 0x30);
    if (b.bad) {
        kfree(b.p);
        return E_NOMEM;
    }
    sha256(b.p + tbs, b.n - tbs, hash);
    alg(&b, oid_rsa_sha256, sizeof(oid_rsa_sha256));
    bits = begin(&b);
    put(&b, "", 1);
    bool ok = sign_hash(k, hash, &b);
    end(&b, bits, 0x03);
    end(&b, cert, 0x30);
    if (!ok || b.bad) {
        kfree(b.p);
        return ok ? E_NOMEM : E_INVAL;
    }
    k->cert = b.p;
    k->cert_len = b.n;
    return 0;
}

static void key_free(struct key *k)
{
    kfree(k->cert);
    memset(k, 0, sizeof(*k));
    kfree(k);
}

static struct key *key_load(void)
{
    char *data, *cert;
    size_t size, cert_len;
    if (vfs_read_all(KEY_FILE, &data, &size))
        return NULL;
    struct key *k = NULL;
    if (size == 8 + 2 * KEY_LEN && !memcmp(data, key_magic, 8) && !vfs_read_all(CERT_FILE, &cert, &cert_len)) {
        if ((k = kzalloc(sizeof(*k))) != NULL) {
            memcpy(k->n, data + 8, KEY_LEN);
            memcpy(k->d, data + 8 + KEY_LEN, KEY_LEN);
            k->cert = (uint8_t *)cert;
            k->cert_len = cert_len;
        } else {
            kfree(cert);
        }
    }
    memset(data, 0, size);
    kfree(data);
    return k;
}

static int key_create(struct key **out)
{
    struct key *k = kzalloc(sizeof(*k));
    if (!k)
        return E_NOMEM;
    int err = rsa_generate(KEY_LEN, k->n, k->d) ? make_cert(k) : E_INVAL;
    uint8_t *file = err ? NULL : kmalloc(8 + 2 * KEY_LEN);
    if (!err && !file)
        err = E_NOMEM;
    if (!err) {
        memcpy(file, key_magic, 8);
        memcpy(file + 8, k->n, KEY_LEN);
        memcpy(file + 8 + KEY_LEN, k->d, KEY_LEN);
        vfs_mkdirs(SB_KEY_DIR);
        err = vfs_write_all(CERT_FILE, k->cert, k->cert_len);
        if (!err)
            err = vfs_write_all(KEY_FILE, file, 8 + 2 * KEY_LEN);
        memset(file, 0, 8 + 2 * KEY_LEN);
    }
    kfree(file);
    if (err) {
        key_free(k);
        return err;
    }
    created = true;
    *out = k;
    return 0;
}

bool sb_has_key(void)
{
    struct vstat st;
    return vfs_stat(KEY_FILE, &st) == 0;
}

bool sb_key_created(void)
{
    bool was = created;
    created = false;
    return was;
}

/* issuer and serial of the certificate, for the SignerInfo */
static bool issuer_serial(const struct key *k, struct der_in *issuer, struct der_in *serial)
{
    struct der_in d = {k->cert, k->cert_len}, v;
    uint8_t tag;
    if (!der_into(&d, 0x30) || !der_into(&d, 0x30) || !der_next(&d, &tag, &v, NULL) || tag != 0xa0 ||
        !der_next(&d, &tag, &v, serial) || tag != 0x02 || !der_next(&d, &tag, &v, NULL))
        return false;
    return der_next(&d, &tag, &v, issuer) && tag == 0x30;
}

static int sign_image(const struct key *k, uint8_t **data, size_t *len)
{
    struct pe pe;
    uint8_t *d = *data;
    size_t n = *len;
    if (!pe_parse(d, n, &pe))
        return E_INVAL;
    uint32_t off = rd32(d + pe.cert_dir), size = rd32(d + pe.cert_dir + 4);
    if (off && size && off <= n && size == n - off)
        n = off;
    wr32(d + pe.cert_dir, 0);
    wr32(d + pe.cert_dir + 4, 0);
    if (n % 8) {
        uint8_t *grown = krealloc(d, n + 8);
        if (!grown)
            return E_NOMEM;
        *data = d = grown;
        while (n % 8)
            d[n++] = 0;
    }
    uint8_t digest[32], hash[32];
    if (!pe_digest(d, n, &pe, digest))
        return E_INVAL;
    struct der_in issuer, serial;
    if (!issuer_serial(k, &issuer, &serial))
        return E_INVAL;
    struct der_out b = {kmalloc(SIG_MAX + k->cert_len), 0, SIG_MAX + k->cert_len, false};
    if (!b.p)
        return E_NOMEM;
    size_t p7 = begin(&b);
    prim(&b, 0x06, oid_signed_data, sizeof(oid_signed_data));
    size_t explicit = begin(&b), sd = begin(&b);
    small_int(&b, 1);
    size_t at = begin(&b);
    alg(&b, oid_sha256, sizeof(oid_sha256));
    end(&b, at, 0x31);
    size_t content = begin(&b);
    prim(&b, 0x06, oid_spc_indirect, sizeof(oid_spc_indirect));
    size_t wrap = begin(&b), indirect = begin(&b);
    at = begin(&b);
    prim(&b, 0x06, oid_spc_pe_image, sizeof(oid_spc_pe_image));
    static const uint8_t pe_data[] = {0x30, 0x25, 0x03, 0x01, 0x00, 0xa0, 0x20, 0xa2, 0x1e, 0x80, 0x1c,
                                      0, '<', 0, '<', 0, '<', 0, 'O', 0, 'b', 0, 's', 0, 'o', 0, 'l', 0,
                                      'e', 0, 't', 0, 'e', 0, '>', 0, '>', 0, '>'};
    put(&b, pe_data, sizeof(pe_data));
    end(&b, at, 0x30);
    at = begin(&b);
    alg(&b, oid_sha256, sizeof(oid_sha256));
    prim(&b, 0x04, digest, sizeof(digest));
    end(&b, at, 0x30);
    size_t body = b.n;
    sha256(b.p + indirect, body - indirect, hash);
    end(&b, indirect, 0x30);
    end(&b, wrap, 0xa0);
    end(&b, content, 0x30);
    at = begin(&b);
    put(&b, k->cert, k->cert_len);
    end(&b, at, 0xa0);
    size_t signers = begin(&b), signer = begin(&b);
    small_int(&b, 1);
    at = begin(&b);
    put(&b, issuer.p, issuer.n);
    put(&b, serial.p, serial.n);
    end(&b, at, 0x30);
    alg(&b, oid_sha256, sizeof(oid_sha256));
    size_t attrs = begin(&b);
    at = begin(&b);
    prim(&b, 0x06, oid_content_type, sizeof(oid_content_type));
    size_t set = begin(&b);
    prim(&b, 0x06, oid_spc_indirect, sizeof(oid_spc_indirect));
    end(&b, set, 0x31);
    end(&b, at, 0x30);
    at = begin(&b);
    prim(&b, 0x06, oid_message_digest, sizeof(oid_message_digest));
    set = begin(&b);
    prim(&b, 0x04, hash, sizeof(hash));
    end(&b, set, 0x31);
    end(&b, at, 0x30);
    end(&b, attrs, 0x31);
    bool ok = !b.bad;
    if (ok) {
        sha256(b.p + attrs, b.n - attrs, hash);
        b.p[attrs] = 0xa0;
    }
    alg(&b, oid_rsa, sizeof(oid_rsa));
    at = begin(&b);
    ok = ok && sign_hash(k, hash, &b);
    end(&b, at, 0x04);
    end(&b, signer, 0x30);
    end(&b, signers, 0x31);
    end(&b, sd, 0x30);
    end(&b, explicit, 0xa0);
    end(&b, p7, 0x30);
    if (!ok || b.bad) {
        kfree(b.p);
        return ok ? E_NOMEM : E_INVAL;
    }
    size_t entry = (8 + b.n + 7) & ~(size_t)7;
    uint8_t *grown = krealloc(d, n + entry);
    if (!grown) {
        kfree(b.p);
        return E_NOMEM;
    }
    *data = d = grown;
    memset(d + n, 0, entry);
    wr32(d + n, (uint32_t)(8 + b.n));
    d[n + 4] = 0x00;
    d[n + 5] = 0x02;
    d[n + 6] = 0x02;
    d[n + 7] = 0x00;
    memcpy(d + n + 8, b.p, b.n);
    kfree(b.p);
    wr32(d + pe.cert_dir, (uint32_t)n);
    wr32(d + pe.cert_dir + 4, (uint32_t)entry);
    *len = n + entry;
    return 0;
}

static int with_key(struct key **out)
{
    *out = key_load();
    return *out ? 0 : key_create(out);
}

int sb_sign_file(const char *path)
{
    struct key *k;
    int err = with_key(&k);
    if (err)
        return err;
    char *data;
    size_t size;
    if ((err = vfs_read_all(path, &data, &size)) == 0) {
        uint8_t *d = (uint8_t *)data;
        if ((err = sign_image(k, &d, &size)) == 0)
            err = vfs_write_all(path, d, size);
        kfree(d);
    }
    key_free(k);
    return err;
}

int sb_save_cert(const char *path)
{
    struct key *k;
    int err = with_key(&k);
    if (err)
        return err;
    err = vfs_write_all(path, k->cert, k->cert_len);
    key_free(k);
    return err;
}

bool sb_resign_needed(const char *installed, const uint8_t *efi, size_t len)
{
    if (sb_has_key())
        return true;
    char *old;
    size_t old_len;
    if (vfs_read_all(installed, &old, &old_len))
        return false;
    const uint8_t *a, *b;
    size_t a_len, b_len;
    bool differ = false;
    if (sb_cert((uint8_t *)old, old_len, &a, &a_len) == 0)
        differ = sb_cert(efi, len, &b, &b_len) || a_len != b_len || memcmp(a, b, a_len);
    kfree(old);
    return differ;
}

/* signs the SynthOS loaders of an EFI partition: \EFI\SynthOS next to Windows, else \EFI\BOOT (never shim) */
int sb_sign_esp(const char *root, bool windows, bool shim)
{
    static const char *const beside_windows[] = {"EFI/SynthOS/grubx64.efi", "EFI/SynthOS/synthos.efi"};
    const char *own = shim ? "EFI/BOOT/grubx64.efi" : "EFI/BOOT/BOOTX64.EFI";
    char path[PATH_MAX_LEN];
    int err = 0, done = 0;
    for (size_t i = 0; i < (windows ? ARRAY_SIZE(beside_windows) : 1) && !err; i++) {
        struct vstat st;
        ksnprintf(path, sizeof(path), "%s/%s", root, windows ? beside_windows[i] : own);
        if (vfs_stat(path, &st))
            continue;
        if ((err = sb_sign_file(path)) == 0)
            done++;
    }
    if (!err && done) {
        ksnprintf(path, sizeof(path), "%s/SYNTHOS/synthos-key.cer", root);
        err = sb_save_cert(path);
    }
    return err ? err : done ? 0 : E_NOENT;
}

static bool has(const char *root, const char *rel)
{
    char path[PATH_MAX_LEN];
    struct vstat st;
    ksnprintf(path, sizeof(path), "%s/%s", root, rel);
    return vfs_stat(path, &st) == 0;
}

static int sign_visit(const char *root, void *ctx)
{
    bool windows = has(root, "EFI/SynthOS");
    *(int *)ctx = sb_sign_esp(root, windows, has(root, windows ? "EFI/SynthOS/grubx64.efi" : "EFI/BOOT/grubx64.efi"));
    return 0;
}

COMMAND(secureboot, "sbkey", "system", "secureboot [sign]",
        "this computer's Secure Boot key; 'sign' signs the loader with it (then enroll SYNTHOS\\synthos-key.cer)")
{
    if (argc > 1 && !strcmp(argv[1], "sign")) {
        int result = E_NOENT;
        int err = recovery_own_esp(sign_visit, &result);
        if (err == E_NOENT)
            return sh_error(sh, "secureboot", "this start did not come from the UEFI loader");
        if (err || result)
            return sh_error(sh, "secureboot", "%s", err_str(result ? result : err));
        sh_printf(sh, "loader signed; enroll SYNTHOS\\synthos-key.cer in MokManager once\n");
        return 0;
    }
    if (argc > 1)
        return command_usage(sh, command_find("secureboot"));
    struct key *k = key_load();
    if (!k) {
        sh_printf(sh, "device key: none (release keys are used)\n");
        return 0;
    }
    uint8_t fp[32];
    sha256(k->cert, k->cert_len, fp);
    sh_printf(sh, "device key: RSA-2048, certificate SHA-256 ");
    for (int i = 0; i < 8; i++)
        sh_printf(sh, "%02x", fp[i]);
    sh_printf(sh, "...\n");
    key_free(k);
    return 0;
}
