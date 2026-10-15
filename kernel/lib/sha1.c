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

void sha1(const void *data, size_t n, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    const uint8_t *p = data;
    size_t left = n;
    for (; left >= 64; left -= 64, p += 64)
        block(h, p);
    uint8_t tail[128] = {0};
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t len = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++)
        tail[len - 1 - i] = (uint8_t)(bits >> (i * 8));
    block(h, tail);
    if (len == 128)
        block(h, tail + 64);
    for (int i = 0; i < 20; i++)
        out[i] = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
}
