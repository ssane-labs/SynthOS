#ifndef SYNTH_SHA256_H
#define SYNTH_SHA256_H

#include <synth/types.h>

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);
void hmac_sha256(const void *key, size_t key_len, const void *data, size_t n, uint8_t out[32]);
void pbkdf2_sha256(const void *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32]);

#endif
