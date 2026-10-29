#include <synth/crypto.h>
#include <synth/string.h>

static uint8_t sbox[256], inv_sbox[256];
static uint32_t te[4][256];
static bool tables_ready;

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0));
}

static void build_tables(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ p << 1 ^ (p & 0x80 ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80)
            q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++)
        inv_sbox[sbox[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = sbox[i], s2 = xtime(s), s3 = s2 ^ s;
        uint32_t t = (uint32_t)s2 << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | s3;
        for (int k = 0; k < 4; k++)
            te[k][i] = t >> (8 * k) | t << (32 - 8 * k);
    }
    tables_ready = true;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t sub_word(uint32_t w)
{
    return (uint32_t)sbox[w >> 24] << 24 | (uint32_t)sbox[(w >> 16) & 0xff] << 16 |
           (uint32_t)sbox[(w >> 8) & 0xff] << 8 | sbox[w & 0xff];
}

void aes_init(struct aes *a, const uint8_t *key, size_t key_len)
{
    if (!tables_ready)
        build_tables();
    int nk = (int)key_len / 4;
    a->rounds = nk + 6;
    int total = 4 * (a->rounds + 1);
    for (int i = 0; i < nk; i++)
        a->rk[i] = be32(key + 4 * i);
    uint32_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = a->rk[i - 1];
        if (i % nk == 0) {
            t = sub_word(t << 8 | t >> 24) ^ rcon << 24;
            rcon = xtime((uint8_t)rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        a->rk[i] = a->rk[i - nk] ^ t;
    }
}

void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    const uint32_t *rk = a->rk;
    uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1], s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
    for (int r = 1; r < a->rounds; r++) {
        rk += 4;
        uint32_t t0 = te[0][s0 >> 24] ^ te[1][(s1 >> 16) & 0xff] ^ te[2][(s2 >> 8) & 0xff] ^ te[3][s3 & 0xff] ^ rk[0];
        uint32_t t1 = te[0][s1 >> 24] ^ te[1][(s2 >> 16) & 0xff] ^ te[2][(s3 >> 8) & 0xff] ^ te[3][s0 & 0xff] ^ rk[1];
        uint32_t t2 = te[0][s2 >> 24] ^ te[1][(s3 >> 16) & 0xff] ^ te[2][(s0 >> 8) & 0xff] ^ te[3][s1 & 0xff] ^ rk[2];
        uint32_t t3 = te[0][s3 >> 24] ^ te[1][(s0 >> 16) & 0xff] ^ te[2][(s1 >> 8) & 0xff] ^ te[3][s2 & 0xff] ^ rk[3];
        s0 = t0;
        s1 = t1;
        s2 = t2;
        s3 = t3;
    }
    rk += 4;
    put_be32(out, sub_word((s0 & 0xff000000) | (s1 & 0xff0000) | (s2 & 0xff00) | (s3 & 0xff)) ^ rk[0]);
    put_be32(out + 4, sub_word((s1 & 0xff000000) | (s2 & 0xff0000) | (s3 & 0xff00) | (s0 & 0xff)) ^ rk[1]);
    put_be32(out + 8, sub_word((s2 & 0xff000000) | (s3 & 0xff0000) | (s0 & 0xff00) | (s1 & 0xff)) ^ rk[2]);
    put_be32(out + 12, sub_word((s3 & 0xff000000) | (s0 & 0xff0000) | (s1 & 0xff00) | (s2 & 0xff)) ^ rk[3]);
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1)
            p ^= a;
    return p;
}

/* the inverse cipher, byte by byte: decryption is rare (Windows programs that store data with BCrypt or CryptoAPI) */
void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ (uint8_t)(a->rk[4 * a->rounds + i / 4] >> (24 - 8 * (i % 4)));
    for (int r = a->rounds - 1; r >= 0; r--) {
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++)
                t[((c + row) % 4) * 4 + row] = s[c * 4 + row];
        for (int i = 0; i < 16; i++)
            s[i] = inv_sbox[t[i]] ^ (uint8_t)(a->rk[4 * r + i / 4] >> (24 - 8 * (i % 4)));
        if (!r)
            break;
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
            s[4 * c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
            s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
            s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
            s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
        }
    }
    memcpy(out, s, 16);
}

/* GHASH with 8-bit tables in 32-bit words (Shoup): m[b] is H times the byte b, last8[r] folds back the 8 bits
   that a byte shift drops. One table lookup per byte instead of a bit loop or 64-bit shifts on a 32-bit CPU. */
static uint32_t last8[256];

static void shr1(uint32_t z[4])
{
    uint32_t carry = z[3] & 1;
    z[3] = z[3] >> 1 | z[2] << 31;
    z[2] = z[2] >> 1 | z[1] << 31;
    z[1] = z[1] >> 1 | z[0] << 31;
    z[0] = z[0] >> 1 ^ (carry ? 0xe1000000u : 0);
}

void gcm_init(struct gcm *g, const uint8_t *key, size_t key_len)
{
    uint8_t h[16] = {0};
    aes_init(&g->aes, key, key_len);
    aes_encrypt(&g->aes, h, h);
    if (!last8[1])
        for (int r = 0; r < 256; r++) {
            uint32_t z[4] = {0, 0, 0, (uint32_t)r};
            for (int k = 0; k < 8; k++)
                shr1(z);
            last8[r] = z[0];
        }
    memset(g->m[0], 0, sizeof(g->m[0]));
    uint32_t v[4] = {be32(h), be32(h + 4), be32(h + 8), be32(h + 12)};
    for (int b = 128; b; b >>= 1) {
        memcpy(g->m[b], v, sizeof(v));
        shr1(v);
    }
    for (int b = 2; b < 256; b <<= 1)
        for (int j = 1; j < b; j++)
            for (int k = 0; k < 4; k++)
                g->m[b + j][k] = g->m[b][k] ^ g->m[j][k];
}

static void gmult(const struct gcm *g, uint8_t x[16])
{
    const uint32_t *m = g->m[x[15]];
    uint32_t z0 = m[0], z1 = m[1], z2 = m[2], z3 = m[3];
    for (int i = 14; i >= 0; i--) {
        uint32_t rem = z3 & 0xff;
        z3 = z3 >> 8 | z2 << 24;
        z2 = z2 >> 8 | z1 << 24;
        z1 = z1 >> 8 | z0 << 24;
        z0 = z0 >> 8 ^ last8[rem];
        m = g->m[x[i]];
        z0 ^= m[0];
        z1 ^= m[1];
        z2 ^= m[2];
        z3 ^= m[3];
    }
    put_be32(x, z0);
    put_be32(x + 4, z1);
    put_be32(x + 8, z2);
    put_be32(x + 12, z3);
}

static void ghash(const struct gcm *g, uint8_t acc[16], const uint8_t *data, size_t n)
{
    while (n) {
        size_t k = MIN(n, (size_t)16);
        for (size_t i = 0; i < k; i++)
            acc[i] ^= data[i];
        gmult(g, acc);
        data += k;
        n -= k;
    }
}

static void increment(uint8_t ctr[16])
{
    for (int i = 15; i >= 12; i--)
        if (++ctr[i])
            break;
}

static void gcm_crypt(const struct gcm *g, const uint8_t iv[12], const uint8_t *in, size_t n, uint8_t *out,
                      uint8_t j0_block[16])
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    aes_encrypt(&g->aes, ctr, j0_block);
    while (n) {
        increment(ctr);
        aes_encrypt(&g->aes, ctr, ks);
        size_t k = MIN(n, (size_t)16);
        for (size_t i = 0; i < k; i++)
            out[i] = in[i] ^ ks[i];
        in += k;
        out += k;
        n -= k;
    }
}

static void gcm_tag(const struct gcm *g, const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t n,
                    const uint8_t j0_block[16], uint8_t tag[16])
{
    uint8_t acc[16] = {0}, lens[16];
    ghash(g, acc, aad, aad_len);
    ghash(g, acc, ct, n);
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)n * 8;
    put_be32(lens, (uint32_t)(abits >> 32));
    put_be32(lens + 4, (uint32_t)abits);
    put_be32(lens + 8, (uint32_t)(cbits >> 32));
    put_be32(lens + 12, (uint32_t)cbits);
    ghash(g, acc, lens, 16);
    for (int i = 0; i < 16; i++)
        tag[i] = acc[i] ^ j0_block[i];
}

void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aad_len, const uint8_t *in,
              size_t n, uint8_t *out, uint8_t tag[16])
{
    uint8_t j0[16];
    gcm_crypt(g, iv, in, n, out, j0);
    gcm_tag(g, aad, aad_len, out, n, j0, tag);
}

bool gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t aad_len, const uint8_t *in,
              size_t n, uint8_t *out, const uint8_t tag[16])
{
    uint8_t ctr[16], j0[16], expected[16];
    memcpy(ctr, iv, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    aes_encrypt(&g->aes, ctr, j0);
    gcm_tag(g, aad, aad_len, in, n, j0, expected);
    if (!crypto_equal(expected, tag, 16))
        return false;
    gcm_crypt(g, iv, in, n, out, j0);
    return true;
}
