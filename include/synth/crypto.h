#ifndef SYNTH_CRYPTO_H
#define SYNTH_CRYPTO_H

#include <synth/sha256.h>
#include <synth/types.h>

#define HASH_MAX 64
#define HASH_BLOCK_MAX 128

struct sha512 {
    uint64_t h[8];
    uint64_t len;
    uint8_t buf[128];
    size_t fill;
};

union hash_ctx {
    struct sha256 s256;
    struct sha512 s512;
};

struct hash_alg {
    K64(const char *name);
    K64(size_t digest);
    K64(size_t block);
    K64(void (*init)(union hash_ctx *c));
    K64(void (*update)(union hash_ctx *c, const void *data, size_t n));
    K64(void (*final)(union hash_ctx *c, uint8_t *out));
};

extern const struct hash_alg hash_sha256, hash_sha384, hash_sha512, hash_sha1, hash_md5;

void sha384_init(struct sha512 *s);
void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, size_t n);
void sha512_final(struct sha512 *s, uint8_t *out, size_t digest);
void hash_digest(const struct hash_alg *h, const void *data, size_t n, uint8_t *out);

struct hmac {
    K64(const struct hash_alg *alg);
    union hash_ctx inner, outer;
};

void hmac_init(struct hmac *m, const struct hash_alg *h, const void *key, size_t key_len);
void hmac_update(struct hmac *m, const void *data, size_t n);
void hmac_final(struct hmac *m, uint8_t *out);
void hmac(const struct hash_alg *h, const void *key, size_t key_len, const void *data, size_t n, uint8_t *out);
void hkdf_extract(const struct hash_alg *h, const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                  uint8_t *prk);
void hkdf_expand(const struct hash_alg *h, const uint8_t *prk, const void *info, size_t info_len, uint8_t *out,
                 size_t len);

struct aes {
    uint32_t rk[60];
    int rounds;
};

void aes_init(struct aes *a, const uint8_t *key, size_t key_len);
void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);

struct gcm {
    struct aes aes;
    uint32_t m[256][4];
};

void gcm_init(struct gcm *g, const uint8_t *key, size_t key_len);
void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aad_len, const uint8_t *in,
              size_t n, uint8_t *out, uint8_t tag[16]);
bool gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aad_len, const uint8_t *in,
              size_t n, uint8_t *out, const uint8_t tag[16]);

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                            const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[16]);
bool chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                            const uint8_t *in, size_t n, uint8_t *out, const uint8_t tag[16]);

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

enum ec_curve { EC_P256, EC_P384 };

size_t ec_size(enum ec_curve c);
bool ec_public_key(enum ec_curve c, const uint8_t *secret, uint8_t *point);
bool ec_shared_secret(enum ec_curve c, const uint8_t *secret, const uint8_t *point, size_t point_len, uint8_t *out);
bool ecdsa_verify(enum ec_curve c, const uint8_t *point, size_t point_len, const uint8_t *hash, size_t hash_len,
                  const uint8_t *r, size_t r_len, const uint8_t *s, size_t s_len);

bool rsa_verify_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const struct hash_alg *h,
                      const uint8_t *hash, const uint8_t *sig, size_t sig_len);
bool rsa_verify_pss(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const struct hash_alg *h,
                    const uint8_t *hash, const uint8_t *sig, size_t sig_len);

void random_bytes(void *out, size_t n);
bool crypto_equal(const void *a, const void *b, size_t n);

#endif
