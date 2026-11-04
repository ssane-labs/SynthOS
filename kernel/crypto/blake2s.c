#include <synth/crypto.h>
#include <synth/string.h>

/* BLAKE2s (RFC 7693): the hash of WireGuard, plain, keyed and with a short digest. */

static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

static const uint8_t sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static inline uint32_t ror(uint32_t v, int n)
{
    return v >> n | v << (32 - n);
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void compress(struct blake2s *s, const uint8_t *block, bool last)
{
    uint32_t m[16], v[16];
    for (int i = 0; i < 16; i++)
        m[i] = le32(block + i * 4);
    for (int i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = iv[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last)
        v[14] = ~v[14];
#define G(a, b, c, d, x, y)                                                                                            \
    do {                                                                                                               \
        v[a] += v[b] + (x);                                                                                            \
        v[d] = ror(v[d] ^ v[a], 16);                                                                                   \
        v[c] += v[d];                                                                                                  \
        v[b] = ror(v[b] ^ v[c], 12);                                                                                   \
        v[a] += v[b] + (y);                                                                                            \
        v[d] = ror(v[d] ^ v[a], 8);                                                                                    \
        v[c] += v[d];                                                                                                  \
        v[b] = ror(v[b] ^ v[c], 7);                                                                                    \
    } while (0)
    for (int r = 0; r < 10; r++) {
        const uint8_t *z = sigma[r];
        G(0, 4, 8, 12, m[z[0]], m[z[1]]);
        G(1, 5, 9, 13, m[z[2]], m[z[3]]);
        G(2, 6, 10, 14, m[z[4]], m[z[5]]);
        G(3, 7, 11, 15, m[z[6]], m[z[7]]);
        G(0, 5, 10, 15, m[z[8]], m[z[9]]);
        G(1, 6, 11, 12, m[z[10]], m[z[11]]);
        G(2, 7, 8, 13, m[z[12]], m[z[13]]);
        G(3, 4, 9, 14, m[z[14]], m[z[15]]);
    }
#undef G
    for (int i = 0; i < 8; i++)
        s->h[i] ^= v[i] ^ v[i + 8];
}

void blake2s_init_key(struct blake2s *s, size_t outlen, const void *key, size_t keylen)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++)
        s->h[i] = iv[i];
    s->h[0] ^= 0x01010000u ^ (uint32_t)keylen << 8 ^ (uint32_t)outlen;
    s->outlen = (uint32_t)outlen;
    if (keylen) {
        memcpy(s->buf, key, keylen);
        s->fill = 64;
    }
}

void blake2s_update(struct blake2s *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        if (s->fill == 64) {
            s->t[0] += 64;
            if (s->t[0] < 64)
                s->t[1]++;
            compress(s, s->buf, false);
            s->fill = 0;
        }
        size_t k = MIN(n, (size_t)(64 - s->fill));
        memcpy(s->buf + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
    }
}

void blake2s_final(struct blake2s *s, uint8_t *out)
{
    s->t[0] += s->fill;
    if (s->t[0] < s->fill)
        s->t[1]++;
    memset(s->buf + s->fill, 0, 64 - s->fill);
    compress(s, s->buf, true);
    uint8_t full[32];
    for (int i = 0; i < 8; i++) {
        full[i * 4] = (uint8_t)s->h[i];
        full[i * 4 + 1] = (uint8_t)(s->h[i] >> 8);
        full[i * 4 + 2] = (uint8_t)(s->h[i] >> 16);
        full[i * 4 + 3] = (uint8_t)(s->h[i] >> 24);
    }
    memcpy(out, full, s->outlen);
}

void blake2s(uint8_t *out, size_t outlen, const void *key, size_t keylen, const void *data, size_t n)
{
    struct blake2s s;
    blake2s_init_key(&s, outlen, key, keylen);
    blake2s_update(&s, data, n);
    blake2s_final(&s, out);
}

static void b2_init(union hash_ctx *c)
{
    blake2s_init_key(&c->b2s, 32, NULL, 0);
}

static void b2_update(union hash_ctx *c, const void *data, size_t n)
{
    blake2s_update(&c->b2s, data, n);
}

static void b2_final(union hash_ctx *c, uint8_t *out)
{
    blake2s_final(&c->b2s, out);
}

const struct hash_alg hash_blake2s = {"blake2s", 32, 64, b2_init, b2_update, b2_final};
