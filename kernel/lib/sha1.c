#include <synth/sha1.h>
#include <synth/string.h>

static uint32_t rol(uint32_t v, int n)
{
    return v << n | v >> (32 - n);
}

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5a827999u;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ed9eba1u;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdcu;
        else
            f = b ^ c ^ d, k = 0xca62c1d6u;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1_init(struct sha256 *s)
{
    static const uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    memcpy(s->h, h, sizeof(h));
    s->len = 0;
    s->fill = 0;
}

void sha1_update(struct sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t k = MIN(n, 64 - s->fill);
        memcpy(s->buf + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
        if (s->fill == 64) {
            block(s->h, s->buf);
            s->fill = 0;
        }
    }
}

void sha1_final(struct sha256 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, len[8];
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56)
        sha1_update(s, &pad, 1);
    for (int i = 0; i < 8; i++)
        len[7 - i] = (uint8_t)(bits >> (i * 8));
    sha1_update(s, len, 8);
    for (int i = 0; i < 20; i++)
        out[i] = (uint8_t)(s->h[i / 4] >> (24 - (i % 4) * 8));
}

void sha1(const void *data, size_t n, uint8_t out[20])
{
    struct sha256 s;
    sha1_init(&s);
    sha1_update(&s, data, n);
    sha1_final(&s, out);
}
