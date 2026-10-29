#include <synth/sha1.h>
#include <synth/string.h>

/* MD5 (RFC 1321), kept for the old checksums programs and installers still ask for. The state is a struct sha256,
   whose first four words and block buffer fit. */

static const uint32_t k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
    0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
    0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t shift[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

static void block(uint32_t h[4], const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)
            f = (b & c) | (~b & d), g = i;
        else if (i < 32)
            f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48)
            f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else
            f = c ^ (b | ~d), g = (7 * i) % 16;
        uint32_t t = d, x = a + f + k[i] + m[g];
        int r = shift[(i / 16) * 4 + i % 4];
        d = c;
        c = b;
        b = b + (x << r | x >> (32 - r));
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void md5_init(struct sha256 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->len = 0;
    s->fill = 0;
}

void md5_update(struct sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t c = MIN(n, 64 - s->fill);
        memcpy(s->buf + s->fill, p, c);
        s->fill += c;
        p += c;
        n -= c;
        if (s->fill == 64) {
            block(s->h, s->buf);
            s->fill = 0;
        }
    }
}

void md5_final(struct sha256 *s, uint8_t out[16])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, len[8];
    md5_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56)
        md5_update(s, &pad, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (i * 8));
    md5_update(s, len, 8);
    for (int i = 0; i < 16; i++)
        out[i] = (uint8_t)(s->h[i / 4] >> ((i % 4) * 8));
}
