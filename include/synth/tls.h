#ifndef SYNTH_TLS_H
#define SYNTH_TLS_H

#include <synth/types.h>

enum x509_key { PK_NONE, PK_RSA, PK_EC_P256, PK_EC_P384 };

struct x509 {
    K64(const uint8_t *raw);
    K64(const uint8_t *tbs);
    K64(const uint8_t *issuer);
    K64(const uint8_t *subject);
    K64(const uint8_t *sig);
    K64(const uint8_t *san);
    K64(size_t raw_len);
    K64(size_t tbs_len);
    K64(size_t issuer_len);
    K64(size_t subject_len);
    K64(size_t sig_len);
    K64(size_t san_len);
    int sig_alg;
    enum x509_key key;
    K64(const uint8_t *rsa_n);
    K64(const uint8_t *rsa_e);
    K64(const uint8_t *ec_point);
    K64(size_t rsa_n_len);
    K64(size_t rsa_e_len);
    K64(size_t ec_point_len);
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
