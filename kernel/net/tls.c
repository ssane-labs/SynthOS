#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/net.h>
#include <synth/string.h>
#include <synth/tls.h>

#define RECORD_MAX 16384
#define IN_CAP (RECORD_MAX + 512)
#define HANDSHAKE_TIMEOUT 20000
#define MAX_CHAIN 10

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23 };
enum {
    HS_CLIENT_HELLO = 1,
    HS_SERVER_HELLO = 2,
    HS_NEW_SESSION_TICKET = 4,
    HS_ENCRYPTED_EXTENSIONS = 8,
    HS_CERTIFICATE = 11,
    HS_CERTIFICATE_REQUEST = 13,
    HS_CERTIFICATE_VERIFY = 15,
    HS_FINISHED = 20,
    HS_KEY_UPDATE = 24,
    HS_MESSAGE_HASH = 254,
};
enum { GROUP_P256 = 0x17, GROUP_P384 = 0x18, GROUP_X25519 = 0x1d };

static const uint8_t hrr_random[32] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
                                       0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
                                       0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};

struct cipher {
    int suite;
    struct gcm gcm;
    uint8_t key[32];
    uint8_t iv[12];
    uint64_t seq;
    bool active;
};

struct tls {
    int sock;
    char host[256];
    bool verify;
    bool closed;
    int suite;
    const struct hash_alg *hash;
    struct cipher rd, wr;
    uint8_t secret_rd[HASH_MAX], secret_wr[HASH_MAX];
    uint8_t *transcript;
    size_t transcript_len, transcript_cap;
    uint8_t *hs;
    size_t hs_len, hs_cap;
    uint8_t in[IN_CAP];
    size_t in_len;
    uint8_t plain[RECORD_MAX + 256];
    size_t plain_off, plain_len;
    uint8_t record_type;
    struct x509 chain[MAX_CHAIN];
    uint8_t *chain_der;
    int chain_len;
    char peer[96];
};

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t get24(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static bool grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return true;
    size_t n = MAX(need, *cap * 2 + 1024);
    uint8_t *p = krealloc(*buf, n);
    if (!p)
        return false;
    *buf = p;
    *cap = n;
    return true;
}

static bool transcript_add(struct tls *t, const uint8_t *msg, size_t n)
{
    if (!grow(&t->transcript, &t->transcript_cap, t->transcript_len + n))
        return false;
    memcpy(t->transcript + t->transcript_len, msg, n);
    t->transcript_len += n;
    return true;
}

static void transcript_hash(struct tls *t, uint8_t *out)
{
    hash_digest(t->hash, t->transcript, t->transcript_len, out);
}

static void expand_label(const struct hash_alg *h, const uint8_t *secret, const char *label, const uint8_t *ctx,
                         size_t ctx_len, uint8_t *out, size_t len)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + HASH_MAX];
    size_t llen = strlen(label), n = 0;
    put16(info, (uint32_t)len);
    n = 2;
    info[n++] = (uint8_t)(6 + llen);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, llen);
    n += llen;
    info[n++] = (uint8_t)ctx_len;
    memcpy(info + n, ctx, ctx_len);
    n += ctx_len;
    hkdf_expand(h, secret, info, n, out, len);
}

static void derive_secret(struct tls *t, const uint8_t *secret, const char *label, bool with_transcript,
                          uint8_t *out)
{
    uint8_t h[HASH_MAX];
    if (with_transcript)
        transcript_hash(t, h);
    else
        hash_digest(t->hash, "", 0, h);
    expand_label(t->hash, secret, label, h, t->hash->digest, out, t->hash->digest);
}

static size_t key_len(int suite)
{
    return suite == 0x1301 ? 16 : 32;
}

static void set_keys(struct tls *t, struct cipher *c, const uint8_t *secret)
{
    c->suite = t->suite;
    expand_label(t->hash, secret, "key", NULL, 0, c->key, key_len(t->suite));
    expand_label(t->hash, secret, "iv", NULL, 0, c->iv, 12);
    if (t->suite != 0x1303)
        gcm_init(&c->gcm, c->key, key_len(t->suite));
    c->seq = 0;
    c->active = true;
}

static void nonce(const struct cipher *c, uint8_t out[12])
{
    memcpy(out, c->iv, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(c->seq >> (8 * i));
}

static int send_all(struct tls *t, const uint8_t *p, size_t n)
{
    uint64_t start = hal_ticks_ms();
    while (n) {
        long k = net_send(t->sock, p, n);
        if (k == E_AGAIN) {
            if (hal_ticks_ms() - start > HANDSHAKE_TIMEOUT)
                return E_TIMEDOUT;
            net_wait(1);
            continue;
        }
        if (k <= 0)
            return k ? (int)k : E_PIPE;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_record(struct tls *t, uint8_t type, const uint8_t *data, size_t n)
{
    static uint8_t out[RECORD_MAX + 64];
    if (!t->wr.active) {
        out[0] = type;
        put16(out + 1, 0x0303);
        put16(out + 3, (uint32_t)n);
        memcpy(out + 5, data, n);
        return send_all(t, out, n + 5);
    }
    uint8_t iv[12], *body = out + 5;
    memcpy(body, data, n);
    body[n] = type;
    size_t len = n + 1 + 16;
    out[0] = CT_APP;
    put16(out + 1, 0x0303);
    put16(out + 3, (uint32_t)len);
    nonce(&t->wr, iv);
    if (t->wr.suite == 0x1303)
        chacha20_poly1305_seal(t->wr.key, iv, out, 5, body, n + 1, body, body + n + 1);
    else
        gcm_seal(&t->wr.gcm, iv, out, 5, body, n + 1, body, body + n + 1);
    t->wr.seq++;
    return send_all(t, out, len + 5);
}

static int send_handshake(struct tls *t, const uint8_t *msg, size_t n)
{
    transcript_add(t, msg, n);
    return send_record(t, CT_HANDSHAKE, msg, n);
}

static int fill(struct tls *t, size_t need, bool block)
{
    uint64_t start = hal_ticks_ms();
    while (t->in_len < need) {
        long k = net_recv(t->sock, t->in + t->in_len, IN_CAP - t->in_len);
        if (k > 0) {
            t->in_len += (size_t)k;
            start = hal_ticks_ms();
            continue;
        }
        if (k == 0)
            return E_PIPE;
        if (k != E_AGAIN)
            return (int)k;
        if (!block)
            return E_AGAIN;
        if (hal_ticks_ms() - start > HANDSHAKE_TIMEOUT)
            return E_TIMEDOUT;
        net_wait(1);
    }
    return 0;
}

static int read_record(struct tls *t, bool block)
{
    int err = fill(t, 5, block);
    if (err)
        return err;
    size_t len = get16(t->in + 3);
    if (len > RECORD_MAX + 256)
        return TLS_ERR_PROTOCOL;
    if ((err = fill(t, 5 + len, block)))
        return err;
    uint8_t type = t->in[0], header[5];
    memcpy(header, t->in, 5);
    uint8_t *body = t->in + 5;
    size_t n = len;
    if (t->rd.active && type == CT_APP) {
        if (n < 17)
            return TLS_ERR_PROTOCOL;
        uint8_t iv[12];
        nonce(&t->rd, iv);
        n -= 16;
        bool ok = t->rd.suite == 0x1303
                      ? chacha20_poly1305_open(t->rd.key, iv, header, 5, body, n, t->plain, body + n)
                      : gcm_open(&t->rd.gcm, iv, header, 5, body, n, t->plain, body + n);
        if (!ok)
            return TLS_ERR_PROTOCOL;
        t->rd.seq++;
        while (n && !t->plain[n - 1])
            n--;
        if (!n)
            return TLS_ERR_PROTOCOL;
        type = t->plain[--n];
    } else {
        memcpy(t->plain, body, n);
    }
    memmove(t->in, t->in + 5 + len, t->in_len - 5 - len);
    t->in_len -= 5 + len;
    t->record_type = type;
    t->plain_off = 0;
    t->plain_len = n;
    if (type == CT_ALERT) {
        if (n >= 2 && t->plain[1] == 0) {
            t->closed = true;
            return E_PIPE;
        }
        klog("tls: alert %u from %s", n >= 2 ? t->plain[1] : 0, t->host);
        t->closed = true;
        return TLS_ERR_ALERT;
    }
    return 0;
}

static int next_handshake(struct tls *t, uint8_t **msg, size_t *len)
{
    for (;;) {
        if (t->hs_len >= 4) {
            size_t n = 4 + get24(t->hs + 1);
            if (t->hs_len >= n) {
                *msg = t->hs;
                *len = n;
                return 0;
            }
        }
        int err = read_record(t, true);
        if (err)
            return err;
        if (t->record_type == CT_CCS)
            continue;
        if (t->record_type != CT_HANDSHAKE)
            return TLS_ERR_PROTOCOL;
        if (!grow(&t->hs, &t->hs_cap, t->hs_len + t->plain_len))
            return E_NOMEM;
        memcpy(t->hs + t->hs_len, t->plain, t->plain_len);
        t->hs_len += t->plain_len;
        t->plain_len = 0;
    }
}

static void consume_handshake(struct tls *t, size_t len, bool record)
{
    if (record)
        transcript_add(t, t->hs, len);
    memmove(t->hs, t->hs + len, t->hs_len - len);
    t->hs_len -= len;
}

struct key_share {
    int group;
    uint8_t secret[48];
    uint8_t pub[97];
    size_t pub_len;
};

static bool make_share(struct key_share *k, int group)
{
    k->group = group;
    if (group == GROUP_X25519) {
        random_bytes(k->secret, 32);
        x25519_base(k->pub, k->secret);
        k->pub_len = 32;
        return true;
    }
    enum ec_curve c = group == GROUP_P256 ? EC_P256 : EC_P384;
    for (int tries = 0; tries < 8; tries++) {
        random_bytes(k->secret, ec_size(c));
        if (ec_public_key(c, k->secret, k->pub)) {
            k->pub_len = 1 + 2 * ec_size(c);
            return true;
        }
    }
    return false;
}

static size_t client_hello(struct tls *t, uint8_t *m, const struct key_share *k, const uint8_t *cookie,
                           size_t cookie_len, const uint8_t session[32])
{
    static const uint16_t suites[] = {0x1301, 0x1302, 0x1303};
    static const uint16_t groups[] = {GROUP_X25519, GROUP_P256, GROUP_P384};
    static const uint16_t sigs[] = {0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601};
    size_t n = 4;
    put16(m + n, 0x0303);
    n += 2;
    random_bytes(m + n, 32);
    n += 32;
    m[n++] = 32;
    memcpy(m + n, session, 32);
    n += 32;
    put16(m + n, sizeof(suites));
    n += 2;
    for (size_t i = 0; i < ARRAY_SIZE(suites); i++, n += 2)
        put16(m + n, suites[i]);
    m[n++] = 1;
    m[n++] = 0;
    size_t ext_start = n;
    n += 2;
    size_t host_len = strlen(t->host);
    bool is_ip = true;
    for (const char *p = t->host; *p; p++)
        if (!is_digit(*p) && *p != '.')
            is_ip = false;
    if (!is_ip) {
        put16(m + n, 0);
        put16(m + n + 2, (uint32_t)host_len + 5);
        put16(m + n + 4, (uint32_t)host_len + 3);
        m[n + 6] = 0;
        put16(m + n + 7, (uint32_t)host_len);
        memcpy(m + n + 9, t->host, host_len);
        n += 9 + host_len;
    }
    put16(m + n, 10);
    put16(m + n + 2, 2 + sizeof(groups));
    put16(m + n + 4, sizeof(groups));
    n += 6;
    for (size_t i = 0; i < ARRAY_SIZE(groups); i++, n += 2)
        put16(m + n, groups[i]);
    put16(m + n, 13);
    put16(m + n + 2, 2 + sizeof(sigs));
    put16(m + n + 4, sizeof(sigs));
    n += 6;
    for (size_t i = 0; i < ARRAY_SIZE(sigs); i++, n += 2)
        put16(m + n, sigs[i]);
    put16(m + n, 43);
    put16(m + n + 2, 3);
    m[n + 4] = 2;
    put16(m + n + 5, 0x0304);
    n += 7;
    put16(m + n, 51);
    put16(m + n + 2, (uint32_t)k->pub_len + 6);
    put16(m + n + 4, (uint32_t)k->pub_len + 4);
    put16(m + n + 6, (uint32_t)k->group);
    put16(m + n + 8, (uint32_t)k->pub_len);
    memcpy(m + n + 10, k->pub, k->pub_len);
    n += 10 + k->pub_len;
    put16(m + n, 45);
    put16(m + n + 2, 2);
    m[n + 4] = 1;
    m[n + 5] = 1;
    n += 6;
    static const char alpn[] = "http/1.1";
    put16(m + n, 16);
    put16(m + n + 2, sizeof(alpn) + 2);
    put16(m + n + 4, sizeof(alpn));
    m[n + 6] = sizeof(alpn) - 1;
    memcpy(m + n + 7, alpn, sizeof(alpn) - 1);
    n += 7 + sizeof(alpn) - 1;
    if (cookie) {
        put16(m + n, 44);
        put16(m + n + 2, (uint32_t)cookie_len);
        memcpy(m + n + 4, cookie, cookie_len);
        n += 4 + cookie_len;
    }
    put16(m + ext_start, (uint32_t)(n - ext_start - 2));
    m[0] = HS_CLIENT_HELLO;
    put24(m + 1, (uint32_t)(n - 4));
    return n;
}

struct server_hello {
    int suite;
    int version;
    int group;
    const uint8_t *key;
    size_t key_len;
    const uint8_t *cookie;
    size_t cookie_len;
    bool retry;
};

static int parse_server_hello(const uint8_t *m, size_t len, struct server_hello *sh)
{
    memset(sh, 0, sizeof(*sh));
    if (len < 4 + 2 + 32 + 1)
        return TLS_ERR_PROTOCOL;
    const uint8_t *p = m + 4, *end = m + len;
    sh->retry = memcmp(p + 2, hrr_random, 32) == 0;
    p += 34;
    p += 1 + p[0];
    if (p + 5 > end)
        return TLS_ERR_PROTOCOL;
    sh->suite = (int)get16(p);
    p += 3;
    size_t ext_len = get16(p);
    p += 2;
    if (p + ext_len > end)
        return TLS_ERR_PROTOCOL;
    const uint8_t *ext_end = p + ext_len;
    while (p + 4 <= ext_end) {
        uint32_t type = get16(p), n = get16(p + 2);
        const uint8_t *v = p + 4;
        p += 4 + n;
        if (p > ext_end)
            return TLS_ERR_PROTOCOL;
        if (type == 43 && n == 2)
            sh->version = (int)get16(v);
        else if (type == 51 && n >= 2) {
            sh->group = (int)get16(v);
            if (n >= 4) {
                sh->key_len = get16(v + 2);
                sh->key = v + 4;
                if (sh->key_len + 4 > n)
                    return TLS_ERR_PROTOCOL;
            }
        } else if (type == 44 && n >= 2) {
            sh->cookie = v;
            sh->cookie_len = n;
        }
    }
    return 0;
}

static int verify_chain(struct tls *t)
{
    if (!t->chain_len)
        return TLS_ERR_CERT_UNTRUSTED;
    const struct x509 *leaf = &t->chain[0];
    str_copy(t->peer, leaf->cn, sizeof(t->peer));
    if (!t->verify)
        return 0;
    if (!x509_matches_host(leaf, t->host))
        return TLS_ERR_CERT_NAME;
    int64_t now = x509_now();
    bool check_time = now > 1600000000;
    const struct x509 *cur = leaf;
    for (int depth = 0; depth < MAX_CHAIN; depth++) {
        if (check_time && (now < cur->not_before || now > cur->not_after))
            return TLS_ERR_CERT_EXPIRED;
        if (tls_root_contains(cur) || tls_find_root(cur->issuer, cur->issuer_len, cur))
            return 0;
        const struct x509 *next = NULL;
        for (int i = 1; i < t->chain_len && !next; i++) {
            const struct x509 *c = &t->chain[i];
            if (c != cur && c->ca && c->subject_len == cur->issuer_len &&
                memcmp(c->subject, cur->issuer, cur->issuer_len) == 0 && x509_verify_signature(cur, c))
                next = c;
        }
        if (!next)
            return TLS_ERR_CERT_UNTRUSTED;
        cur = next;
    }
    return TLS_ERR_CERT_UNTRUSTED;
}

static int parse_certificates(struct tls *t, const uint8_t *m, size_t len)
{
    const uint8_t *p = m + 4, *end = m + len;
    if (p >= end)
        return TLS_ERR_PROTOCOL;
    p += 1 + p[0];
    if (p + 3 > end)
        return TLS_ERR_PROTOCOL;
    size_t list = get24(p);
    p += 3;
    if (p + list > end)
        return TLS_ERR_PROTOCOL;
    kfree(t->chain_der);
    t->chain_der = kmalloc(list);
    if (!t->chain_der)
        return E_NOMEM;
    memcpy(t->chain_der, p, list);
    p = t->chain_der;
    end = p + list;
    t->chain_len = 0;
    while (p + 3 <= end && t->chain_len < MAX_CHAIN) {
        size_t n = get24(p);
        p += 3;
        if (p + n + 2 > end)
            return TLS_ERR_PROTOCOL;
        if (!x509_parse(p, n, &t->chain[t->chain_len]))
            return TLS_ERR_PROTOCOL;
        t->chain_len++;
        p += n;
        p += 2 + get16(p);
    }
    return verify_chain(t);
}

static bool der_item(const uint8_t **p, const uint8_t *end, uint8_t tag, const uint8_t **v, size_t *n)
{
    if (end - *p < 2 || (*p)[0] != tag)
        return false;
    size_t len = (*p)[1], hdr = 2;
    if (len == 0x81) {
        if (end - *p < 3)
            return false;
        len = (*p)[2];
        hdr = 3;
    } else if (len > 0x80) {
        return false;
    }
    if ((size_t)(end - *p) < hdr + len)
        return false;
    *v = *p + hdr;
    *n = len;
    *p += hdr + len;
    return true;
}

static bool der_signature(const uint8_t *sig, size_t len, const uint8_t **r, size_t *r_len, const uint8_t **s,
                          size_t *s_len)
{
    const uint8_t *p = sig, *seq;
    size_t seq_len;
    if (!der_item(&p, sig + len, 0x30, &seq, &seq_len))
        return false;
    p = seq;
    return der_item(&p, seq + seq_len, 0x02, r, r_len) && der_item(&p, seq + seq_len, 0x02, s, s_len);
}

static int check_certificate_verify(struct tls *t, const uint8_t *m, size_t len)
{
    if (len < 8 || !t->chain_len)
        return TLS_ERR_PROTOCOL;
    uint32_t alg = get16(m + 4);
    size_t sig_len = get16(m + 6);
    const uint8_t *sig = m + 8;
    if (8 + sig_len > len)
        return TLS_ERR_PROTOCOL;
    uint8_t content[64 + 34 + HASH_MAX];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    transcript_hash(t, content + 98);
    size_t content_len = 98 + t->hash->digest;
    const struct x509 *leaf = &t->chain[0];
    const struct hash_alg *h = alg == 0x0403 || alg == 0x0804 ? &hash_sha256
                             : alg == 0x0503 || alg == 0x0805 ? &hash_sha384
                             : alg == 0x0806 ? &hash_sha512 : NULL;
    if (!h)
        return TLS_ERR_UNSUPPORTED;
    uint8_t digest[HASH_MAX];
    hash_digest(h, content, content_len, digest);
    bool ok = false;
    if (alg == 0x0403 || alg == 0x0503) {
        enum ec_curve c = alg == 0x0403 ? EC_P256 : EC_P384;
        if ((leaf->key == PK_EC_P256 && c == EC_P256) || (leaf->key == PK_EC_P384 && c == EC_P384)) {
            const uint8_t *r, *sv;
            size_t r_len, s_len;
            if (der_signature(sig, sig_len, &r, &r_len, &sv, &s_len))
                ok = ecdsa_verify(c, leaf->ec_point, leaf->ec_point_len, digest, h->digest, r, r_len, sv, s_len);
        }
    } else if (leaf->key == PK_RSA) {
        ok = rsa_verify_pss(leaf->rsa_n, leaf->rsa_n_len, leaf->rsa_e, leaf->rsa_e_len, h, digest, sig, sig_len);
    }
    return ok || !t->verify ? 0 : TLS_ERR_SIGNATURE;
}

static int handshake(struct tls *t)
{
    static uint8_t hello[2048];
    uint8_t session[32];
    random_bytes(session, sizeof(session));
    struct key_share share;
    if (!make_share(&share, GROUP_X25519))
        return E_NOMEM;
    size_t n = client_hello(t, hello, &share, NULL, 0, session);
    transcript_add(t, hello, n);
    int err = send_record(t, CT_HANDSHAKE, hello, n);
    if (err)
        return err;
    struct server_hello sh;
    uint8_t *msg;
    size_t len;
    for (int round = 0;; round++) {
        if ((err = next_handshake(t, &msg, &len)))
            return err;
        if (msg[0] != HS_SERVER_HELLO)
            return TLS_ERR_PROTOCOL;
        if ((err = parse_server_hello(msg, len, &sh)))
            return err;
        if (sh.version != 0x0304)
            return TLS_ERR_UNSUPPORTED;
        t->suite = sh.suite;
        t->hash = sh.suite == 0x1302 ? &hash_sha384 : &hash_sha256;
        if (sh.suite < 0x1301 || sh.suite > 0x1303)
            return TLS_ERR_UNSUPPORTED;
        if (!sh.retry)
            break;
        if (round)
            return TLS_ERR_PROTOCOL;
        uint8_t h[HASH_MAX], first[4 + HASH_MAX] = {HS_MESSAGE_HASH, 0, 0, (uint8_t)t->hash->digest};
        hash_digest(t->hash, t->transcript, t->transcript_len, h);
        memcpy(first + 4, h, t->hash->digest);
        t->transcript_len = 0;
        transcript_add(t, first, 4 + t->hash->digest);
        uint8_t cookie[512];
        size_t cookie_len = MIN(sh.cookie_len, sizeof(cookie));
        if (sh.cookie)
            memcpy(cookie, sh.cookie, cookie_len);
        int group = sh.group ? sh.group : share.group;
        consume_handshake(t, len, true);
        if ((group != GROUP_X25519 && group != GROUP_P256 && group != GROUP_P384) || !make_share(&share, group))
            return TLS_ERR_UNSUPPORTED;
        n = client_hello(t, hello, &share, sh.cookie ? cookie : NULL, cookie_len, session);
        if ((err = send_handshake(t, hello, n)))
            return err;
    }
    if (sh.group != share.group || !sh.key)
        return TLS_ERR_PROTOCOL;
    uint8_t shared[48];
    size_t shared_len;
    if (share.group == GROUP_X25519) {
        if (sh.key_len != 32)
            return TLS_ERR_PROTOCOL;
        x25519(shared, share.secret, sh.key);
        shared_len = 32;
    } else {
        enum ec_curve c = share.group == GROUP_P256 ? EC_P256 : EC_P384;
        if (!ec_shared_secret(c, share.secret, sh.key, sh.key_len, shared))
            return TLS_ERR_PROTOCOL;
        shared_len = ec_size(c);
    }
    consume_handshake(t, len, true);
    size_t hl = t->hash->digest;
    uint8_t zero[HASH_MAX] = {0}, early[HASH_MAX], derived[HASH_MAX], hs_secret[HASH_MAX], master[HASH_MAX];
    uint8_t c_hs[HASH_MAX], s_hs[HASH_MAX];
    hkdf_extract(t->hash, NULL, 0, zero, hl, early);
    derive_secret(t, early, "derived", false, derived);
    hkdf_extract(t->hash, derived, hl, shared, shared_len, hs_secret);
    derive_secret(t, hs_secret, "c hs traffic", true, c_hs);
    derive_secret(t, hs_secret, "s hs traffic", true, s_hs);
    derive_secret(t, hs_secret, "derived", false, derived);
    hkdf_extract(t->hash, derived, hl, zero, hl, master);
    set_keys(t, &t->rd, s_hs);
    bool cert_requested = false, have_cert = false;
    for (;;) {
        if ((err = next_handshake(t, &msg, &len)))
            return err;
        uint8_t type = msg[0];
        if (type == HS_ENCRYPTED_EXTENSIONS) {
            consume_handshake(t, len, true);
        } else if (type == HS_CERTIFICATE_REQUEST) {
            cert_requested = true;
            consume_handshake(t, len, true);
        } else if (type == HS_CERTIFICATE) {
            if ((err = parse_certificates(t, msg, len)))
                return err;
            have_cert = true;
            consume_handshake(t, len, true);
        } else if (type == HS_CERTIFICATE_VERIFY) {
            if ((err = check_certificate_verify(t, msg, len)))
                return err;
            consume_handshake(t, len, true);
        } else if (type == HS_FINISHED) {
            uint8_t key[HASH_MAX], expected[HASH_MAX], h[HASH_MAX];
            expand_label(t->hash, s_hs, "finished", NULL, 0, key, hl);
            transcript_hash(t, h);
            hmac(t->hash, key, hl, h, hl, expected);
            if (len != 4 + hl || !crypto_equal(expected, msg + 4, hl))
                return TLS_ERR_SIGNATURE;
            consume_handshake(t, len, true);
            break;
        } else {
            return TLS_ERR_PROTOCOL;
        }
    }
    if (!have_cert && t->verify)
        return TLS_ERR_CERT_UNTRUSTED;
    uint8_t c_ap[HASH_MAX], s_ap[HASH_MAX];
    derive_secret(t, master, "c ap traffic", true, c_ap);
    derive_secret(t, master, "s ap traffic", true, s_ap);
    static const uint8_t ccs = 1;
    if ((err = send_record(t, CT_CCS, &ccs, 1)))
        return err;
    set_keys(t, &t->wr, c_hs);
    if (cert_requested) {
        static const uint8_t empty[8] = {HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0};
        if ((err = send_handshake(t, empty, sizeof(empty))))
            return err;
    }
    uint8_t fin[4 + HASH_MAX], key[HASH_MAX], h[HASH_MAX];
    expand_label(t->hash, c_hs, "finished", NULL, 0, key, hl);
    transcript_hash(t, h);
    fin[0] = HS_FINISHED;
    put24(fin + 1, (uint32_t)hl);
    hmac(t->hash, key, hl, h, hl, fin + 4);
    if ((err = send_handshake(t, fin, 4 + hl)))
        return err;
    memcpy(t->secret_rd, s_ap, hl);
    memcpy(t->secret_wr, c_ap, hl);
    set_keys(t, &t->rd, s_ap);
    set_keys(t, &t->wr, c_ap);
    kfree(t->transcript);
    t->transcript = NULL;
    t->transcript_len = t->transcript_cap = 0;
    return 0;
}

struct tls *tls_connect(int sock, const char *host, bool verify, int *error)
{
    struct tls *t = kzalloc(sizeof(*t));
    if (!t) {
        *error = E_NOMEM;
        return NULL;
    }
    t->sock = sock;
    t->verify = verify;
    str_copy(t->host, host, sizeof(t->host));
    int err = handshake(t);
    if (err) {
        if (err != TLS_ERR_ALERT && err != E_PIPE) {
            uint8_t alert[2] = {2, err == TLS_ERR_CERT_UNTRUSTED ? 48 : err == TLS_ERR_CERT_NAME ? 42
                                     : err == TLS_ERR_CERT_EXPIRED ? 45 : err == TLS_ERR_SIGNATURE ? 51 : 40};
            send_record(t, CT_ALERT, alert, 2);
        }
        *error = err;
        tls_close(t);
        return NULL;
    }
    *error = 0;
    return t;
}

static void key_update(struct tls *t, uint8_t *secret, struct cipher *c)
{
    uint8_t next[HASH_MAX];
    expand_label(t->hash, secret, "traffic upd", NULL, 0, next, t->hash->digest);
    memcpy(secret, next, t->hash->digest);
    set_keys(t, c, secret);
}

static int post_handshake(struct tls *t)
{
    if (!grow(&t->hs, &t->hs_cap, t->hs_len + t->plain_len))
        return E_NOMEM;
    memcpy(t->hs + t->hs_len, t->plain, t->plain_len);
    t->hs_len += t->plain_len;
    t->plain_len = 0;
    while (t->hs_len >= 4) {
        size_t n = 4 + get24(t->hs + 1);
        if (t->hs_len < n)
            break;
        if (t->hs[0] == HS_KEY_UPDATE && n == 5) {
            bool requested = t->hs[4] == 1;
            key_update(t, t->secret_rd, &t->rd);
            if (requested) {
                static const uint8_t update[5] = {HS_KEY_UPDATE, 0, 0, 1, 0};
                int err = send_record(t, CT_HANDSHAKE, update, sizeof(update));
                if (err)
                    return err;
                key_update(t, t->secret_wr, &t->wr);
            }
        }
        consume_handshake(t, n, false);
    }
    return 0;
}

long tls_read(struct tls *t, void *buf, size_t n)
{
    for (;;) {
        if (t->plain_len && t->record_type == CT_APP) {
            size_t k = MIN(n, t->plain_len);
            memcpy(buf, t->plain + t->plain_off, k);
            t->plain_off += k;
            t->plain_len -= k;
            return (long)k;
        }
        if (t->closed)
            return 0;
        int err = read_record(t, false);
        if (err == E_PIPE)
            return 0;
        if (err)
            return err;
        if (t->record_type == CT_HANDSHAKE && (err = post_handshake(t)))
            return err;
    }
}

bool tls_pending(struct tls *t)
{
    return (t->plain_len && t->record_type == CT_APP) || t->in_len >= 5;
}

long tls_write(struct tls *t, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t left = n;
    while (left) {
        size_t k = MIN(left, (size_t)RECORD_MAX);
        int err = send_record(t, CT_APP, p, k);
        if (err)
            return err;
        p += k;
        left -= k;
    }
    return (long)n;
}

void tls_close(struct tls *t)
{
    if (!t)
        return;
    if (t->wr.active && !t->closed) {
        static const uint8_t close_notify[2] = {1, 0};
        send_record(t, CT_ALERT, close_notify, 2);
    }
    kfree(t->transcript);
    kfree(t->hs);
    kfree(t->chain_der);
    memset(t, 0, sizeof(*t));
    kfree(t);
}

const char *tls_version(struct tls *t)
{
    UNUSED(t);
    return "TLS 1.3";
}

const char *tls_cipher(struct tls *t)
{
    return t->suite == 0x1301 ? "AES-128-GCM" : t->suite == 0x1302 ? "AES-256-GCM" : "ChaCha20-Poly1305";
}

const char *tls_peer_name(struct tls *t)
{
    return t->peer;
}

const char *tls_strerror(int err)
{
    switch (err) {
    case TLS_ERR_PROTOCOL: return "TLS protocol error";
    case TLS_ERR_ALERT: return "the server refused the connection";
    case TLS_ERR_CERT_UNTRUSTED: return "certificate is not trusted";
    case TLS_ERR_CERT_NAME: return "certificate does not match the host name";
    case TLS_ERR_CERT_EXPIRED: return "certificate has expired";
    case TLS_ERR_SIGNATURE: return "bad server signature";
    case TLS_ERR_UNSUPPORTED: return "server needs an unsupported TLS version or cipher";
    default: return err_str(err);
    }
}
