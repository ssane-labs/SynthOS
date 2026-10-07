#include <synth/crypto.h>
#include <synth/string.h>

static const uint64_t k512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static inline uint64_t ror64(uint64_t x, int n)
{
    return x >> n | x << (64 - n);
}

static void block512(struct sha512 *s, const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v = v << 8 | p[i * 8 + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + k512[i] + w[i];
        uint64_t t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void sha512_init(struct sha512 *s)
{
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                                   0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                                   0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

void sha384_init(struct sha512 *s)
{
    static const uint64_t iv[8] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull,
                                   0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
                                   0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

void sha512_update(struct sha512 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        if (s->fill == 0 && n >= 128) {
            block512(s, p);
            p += 128;
            n -= 128;
            continue;
        }
        size_t k = MIN(n, 128 - s->fill);
        memcpy(s->buf + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
        if (s->fill == 128) {
            block512(s, s->buf);
            s->fill = 0;
        }
    }
}

void sha512_final(struct sha512 *s, uint8_t *out, size_t digest)
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha512_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 112)
        sha512_update(s, &pad, 1);
    uint8_t len[16] = {0};
    for (int i = 0; i < 8; i++)
        len[15 - i] = (uint8_t)(bits >> (i * 8));
    sha512_update(s, len, 16);
    for (size_t i = 0; i < digest; i++)
        out[i] = (uint8_t)(s->h[i / 8] >> (56 - (i % 8) * 8));
}

static void s256_init(union hash_ctx *c) { sha256_init(&c->s256); }
static void s256_update(union hash_ctx *c, const void *d, size_t n) { sha256_update(&c->s256, d, n); }
static void s256_final(union hash_ctx *c, uint8_t *o) { sha256_final(&c->s256, o); }
static void s384_init(union hash_ctx *c) { sha384_init(&c->s512); }
static void s512_init(union hash_ctx *c) { sha512_init(&c->s512); }
static void s512_update(union hash_ctx *c, const void *d, size_t n) { sha512_update(&c->s512, d, n); }
static void s384_final(union hash_ctx *c, uint8_t *o) { sha512_final(&c->s512, o, 48); }
static void s512_final(union hash_ctx *c, uint8_t *o) { sha512_final(&c->s512, o, 64); }

const struct hash_alg hash_sha256 = {"sha256", 32, 64, s256_init, s256_update, s256_final};
const struct hash_alg hash_sha384 = {"sha384", 48, 128, s384_init, s512_update, s384_final};
const struct hash_alg hash_sha512 = {"sha512", 64, 128, s512_init, s512_update, s512_final};

void hash_digest(const struct hash_alg *h, const void *data, size_t n, uint8_t *out)
{
    union hash_ctx c;
    h->init(&c);
    h->update(&c, data, n);
    h->final(&c, out);
}

void hmac_init(struct hmac *m, const struct hash_alg *h, const void *key, size_t key_len)
{
    uint8_t k[HASH_BLOCK_MAX] = {0}, pad[HASH_BLOCK_MAX];
    if (key_len > h->block)
        hash_digest(h, key, key_len, k);
    else
        memcpy(k, key, key_len);
    m->alg = h;
    for (size_t i = 0; i < h->block; i++)
        pad[i] = k[i] ^ 0x36;
    h->init(&m->inner);
    h->update(&m->inner, pad, h->block);
    for (size_t i = 0; i < h->block; i++)
        pad[i] = k[i] ^ 0x5c;
    h->init(&m->outer);
    h->update(&m->outer, pad, h->block);
}

void hmac_update(struct hmac *m, const void *data, size_t n)
{
    m->alg->update(&m->inner, data, n);
}

void hmac_final(struct hmac *m, uint8_t *out)
{
    uint8_t inner[HASH_MAX];
    m->alg->final(&m->inner, inner);
    m->alg->update(&m->outer, inner, m->alg->digest);
    m->alg->final(&m->outer, out);
}

void hmac(const struct hash_alg *h, const void *key, size_t key_len, const void *data, size_t n, uint8_t *out)
{
    struct hmac m;
    hmac_init(&m, h, key, key_len);
    hmac_update(&m, data, n);
    hmac_final(&m, out);
}

void hkdf_extract(const struct hash_alg *h, const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                  uint8_t *prk)
{
    uint8_t zero[HASH_MAX] = {0};
    if (!salt || !salt_len) {
        salt = zero;
        salt_len = h->digest;
    }
    hmac(h, salt, salt_len, ikm, ikm_len, prk);
}

void hkdf_expand(const struct hash_alg *h, const uint8_t *prk, const void *info, size_t info_len, uint8_t *out,
                 size_t len)
{
    uint8_t t[HASH_MAX];
    size_t t_len = 0, done = 0;
    for (uint8_t counter = 1; done < len; counter++) {
        struct hmac m;
        hmac_init(&m, h, prk, h->digest);
        hmac_update(&m, t, t_len);
        hmac_update(&m, info, info_len);
        hmac_update(&m, &counter, 1);
        hmac_final(&m, t);
        t_len = h->digest;
        size_t k = MIN(t_len, len - done);
        memcpy(out + done, t, k);
        done += k;
    }
}

bool crypto_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}
