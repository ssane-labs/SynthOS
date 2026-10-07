#ifndef SYNTH_TLS_H
#define SYNTH_TLS_H

#include <synth/types.h>

enum x509_key { PK_NONE, PK_RSA, PK_EC_P256, PK_EC_P384 };

struct x509 {
    const uint8_t *raw, *tbs, *issuer, *subject, *sig, *san;
    size_t raw_len, tbs_len, issuer_len, subject_len, sig_len, san_len;
    int sig_alg;
    enum x509_key key;
    const uint8_t *rsa_n, *rsa_e, *ec_point;
    size_t rsa_n_len, rsa_e_len, ec_point_len;
    int64_t not_before, not_after;
    bool ca;
    char cn[96];
};

enum {
    TLS_ERR_PROTOCOL = -100,
    TLS_ERR_ALERT = -101,
    TLS_ERR_CERT_UNTRUSTED = -102,
    TLS_ERR_CERT_NAME = -103,
    TLS_ERR_CERT_EXPIRED = -104,
    TLS_ERR_SIGNATURE = -105,
    TLS_ERR_UNSUPPORTED = -106,
};

struct tls;

bool x509_parse(const uint8_t *der, size_t len, struct x509 *c);
bool x509_verify_signature(const struct x509 *cert, const struct x509 *issuer);
bool x509_matches_host(const struct x509 *c, const char *host);
int x509_load_pem(const char *text, size_t len, void (*fn)(const uint8_t *der, size_t len, void *ctx), void *ctx);
int64_t x509_now(void);

int tls_roots_load(void);
int tls_roots_count(void);
const struct x509 *tls_root(int index);
bool tls_root_contains(const struct x509 *c);
const struct x509 *tls_find_root(const uint8_t *subject, size_t len, const struct x509 *child);

struct tls *tls_connect(int sock, const char *host, bool verify, int *error);
long tls_read(struct tls *t, void *buf, size_t n);
long tls_write(struct tls *t, const void *buf, size_t n);
bool tls_pending(struct tls *t);
void tls_close(struct tls *t);
const char *tls_version(struct tls *t);
const char *tls_cipher(struct tls *t);
const char *tls_peer_name(struct tls *t);
const char *tls_strerror(int err);

#endif
