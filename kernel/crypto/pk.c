#include <synth/crypto.h>
#include <synth/string.h>

#define MP_MAX 128

struct mont {
    int n;
    uint32_t m[MP_MAX];
    uint32_t rr[MP_MAX];
    uint32_t one[MP_MAX];
    uint32_t inv;
};

static size_t strip(const uint8_t **b, size_t len)
{
    while (len && !**b) {
        (*b)++;
        len--;
    }
    return len;
}

static void mp_from_be(uint32_t *r, int n, const uint8_t *b, size_t len)
{
    memset(r, 0, (size_t)n * 4);
    for (size_t i = 0; i < len && i < (size_t)n * 4; i++)
        r[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
}

static void mp_to_be(uint8_t *out, size_t len, const uint32_t *a, int n)
{
    for (size_t i = 0; i < len; i++)
        out[len - 1 - i] = i < (size_t)n * 4 ? (uint8_t)(a[i / 4] >> (8 * (i % 4))) : 0;
}

static int mp_cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool mp_zero(const uint32_t *a, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i])
            return false;
    return true;
}

static uint32_t mp_add(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

static uint32_t mp_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n)
{
    int64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (int64_t)a[i] - b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)(c & 1);
}

static int mp_bits(const uint32_t *a, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i])
            return i * 32 + 32 - __builtin_clz(a[i]);
    return 0;
}

static void mod_double(const struct mont *c, uint32_t *r)
{
    uint32_t carry = mp_add(r, r, r, c->n);
    if (carry || mp_cmp(r, c->m, c->n) >= 0)
        mp_sub(r, r, c->m, c->n);
}

static void mont_init(struct mont *c, const uint32_t *m, int n)
{
    c->n = n;
    memcpy(c->m, m, (size_t)n * 4);
    uint32_t x = 1;
    for (int i = 0; i < 5; i++)
        x *= 2 - m[0] * x;
    c->inv = -x;
    memset(c->one, 0, sizeof(c->one));
    c->one[0] = 1;
    if (mp_cmp(c->one, c->m, n) >= 0)
        mp_sub(c->one, c->one, c->m, n);
    for (int i = 0; i < 32 * n; i++)
        mod_double(c, c->one);
    memcpy(c->rr, c->one, sizeof(c->rr));
    for (int i = 0; i < 32 * n; i++)
        mod_double(c, c->rr);
}

static void mont_mul(const struct mont *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = c->n;
    uint32_t t[MP_MAX + 2] = {0};
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t x = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + carry;
            t[j] = (uint32_t)x;
            carry = x >> 32;
        }
        uint64_t x = (uint64_t)t[n] + carry;
        t[n] = (uint32_t)x;
        t[n + 1] = (uint32_t)(x >> 32);
        uint32_t m = t[0] * c->inv;
        carry = ((uint64_t)t[0] + (uint64_t)m * c->m[0]) >> 32;
        for (int j = 1; j < n; j++) {
            x = (uint64_t)t[j] + (uint64_t)m * c->m[j] + carry;
            t[j - 1] = (uint32_t)x;
            carry = x >> 32;
        }
        x = (uint64_t)t[n] + carry;
        t[n - 1] = (uint32_t)x;
        t[n] = t[n + 1] + (uint32_t)(x >> 32);
    }
    if (t[n] || mp_cmp(t, c->m, n) >= 0)
        mp_sub(r, t, c->m, n);
    else
        memcpy(r, t, (size_t)n * 4);
}

static void to_mont(const struct mont *c, uint32_t *r, const uint32_t *a)
{
    mont_mul(c, r, a, c->rr);
}

static void from_mont(const struct mont *c, uint32_t *r, const uint32_t *a)
{
    uint32_t one[MP_MAX] = {1};
    mont_mul(c, r, a, one);
}

static void mont_pow(const struct mont *c, uint32_t *r, const uint32_t *base, const uint32_t *exp, int exp_limbs)
{
    uint32_t acc[MP_MAX];
    memcpy(acc, c->one, sizeof(acc));
    for (int i = mp_bits(exp, exp_limbs) - 1; i >= 0; i--) {
        mont_mul(c, acc, acc, acc);
        if (exp[i / 32] >> (i % 32) & 1)
            mont_mul(c, acc, acc, base);
    }
    memcpy(r, acc, (size_t)c->n * 4);
}

static bool rsa_public(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *sig,
                       size_t sig_len, uint8_t *out, size_t *k, int *mod_bits)
{
    n_len = strip(&n, n_len);
    e_len = strip(&e, e_len);
    if (!n_len || n_len > MP_MAX * 4 || !e_len || e_len > MP_MAX * 4 || !(n[n_len - 1] & 1))
        return false;
    int limbs = (int)(n_len + 3) / 4;
    static struct mont c;
    uint32_t m[MP_MAX], s[MP_MAX], x[MP_MAX];
    mp_from_be(m, limbs, n, n_len);
    sig_len = strip(&sig, sig_len);
    if (sig_len > n_len)
        return false;
    mp_from_be(s, limbs, sig, sig_len);
    if (mp_cmp(s, m, limbs) >= 0)
        return false;
    mont_init(&c, m, limbs);
    uint32_t exp[MP_MAX];
    mp_from_be(exp, (int)(e_len + 3) / 4, e, e_len);
    to_mont(&c, x, s);
    mont_pow(&c, x, x, exp, (int)(e_len + 3) / 4);
    from_mont(&c, x, x);
    *k = n_len;
    *mod_bits = mp_bits(m, limbs);
    mp_to_be(out, n_len, x, limbs);
    return true;
}

static const uint8_t *digest_info(const struct hash_alg *h, size_t *len)
{
    static const uint8_t sha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    static const uint8_t sha384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
    static const uint8_t sha512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};
    *len = sizeof(sha256);
    return h == &hash_sha256 ? sha256 : h == &hash_sha384 ? sha384 : sha512;
}

bool rsa_verify_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const struct hash_alg *h,
                      const uint8_t *hash, const uint8_t *sig, size_t sig_len)
{
    static uint8_t em[MP_MAX * 4];
    size_t k, info_len;
    int bits;
    if (!rsa_public(n, n_len, e, e_len, sig, sig_len, em, &k, &bits))
        return false;
    const uint8_t *info = digest_info(h, &info_len);
    size_t t_len = info_len + h->digest;
    if (k < t_len + 11 || em[0] != 0 || em[1] != 1)
        return false;
    size_t ps = k - t_len - 3;
    for (size_t i = 0; i < ps; i++)
        if (em[2 + i] != 0xff)
            return false;
    return em[2 + ps] == 0 && memcmp(em + 3 + ps, info, info_len) == 0 &&
           crypto_equal(em + 3 + ps + info_len, hash, h->digest);
}

bool rsa_verify_pss(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const struct hash_alg *h,
                    const uint8_t *hash, const uint8_t *sig, size_t sig_len)
{
    static uint8_t buf[MP_MAX * 4], db[MP_MAX * 4];
    size_t k, hl = h->digest;
    int bits;
    if (!rsa_public(n, n_len, e, e_len, sig, sig_len, buf, &k, &bits))
        return false;
    int em_bits = bits - 1;
    size_t em_len = (size_t)(em_bits + 7) / 8;
    const uint8_t *em = buf + (k - em_len);
    if (k > em_len && buf[0])
        return false;
    if (em_len < hl * 2 + 2 || em[em_len - 1] != 0xbc)
        return false;
    size_t db_len = em_len - hl - 1;
    const uint8_t *hh = em + db_len;
    uint8_t counter[4] = {0}, block[HASH_MAX];
    for (size_t done = 0; done < db_len; counter[3]++) {
        union hash_ctx c;
        h->init(&c);
        h->update(&c, hh, hl);
        h->update(&c, counter, 4);
        h->final(&c, block);
        for (size_t i = 0; i < hl && done < db_len; i++, done++)
            db[done] = em[done] ^ block[i];
    }
    db[0] &= (uint8_t)(0xff >> (8 * em_len - (size_t)em_bits));
    size_t i = 0;
    while (i < db_len - hl - 1 && db[i] == 0)
        i++;
    if (db[i] != 1 || db_len - i - 1 != hl)
        return false;
    uint8_t zeros[8] = {0}, check[HASH_MAX];
    union hash_ctx c;
    h->init(&c);
    h->update(&c, zeros, 8);
    h->update(&c, hash, hl);
    h->update(&c, db + i + 1, hl);
    h->final(&c, check);
    return crypto_equal(check, hh, hl);
}

struct curve {
    int n;
    size_t bytes;
    struct mont p, q;
    uint32_t b[12], gx[12], gy[12], three[12];
    bool ready;
};

struct point {
    uint32_t x[12], y[12], z[12];
};

static struct curve curves[2];

static void hex_mp(uint32_t *r, int n, const char *hex)
{
    uint8_t b[48];
    size_t len = strlen(hex) / 2;
    for (size_t i = 0; i < len; i++) {
        int hi = hex[i * 2], lo = hex[i * 2 + 1];
        hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
        lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    mp_from_be(r, n, b, len);
}

static struct curve *curve_get(enum ec_curve id)
{
    static const char *const params[2][5] = {
        {"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
         "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
         "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
         "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
         "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"},
        {"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
         "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
         "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
         "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
         "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"},
    };
    struct curve *c = &curves[id];
    if (c->ready)
        return c;
    c->n = id == EC_P256 ? 8 : 12;
    c->bytes = (size_t)c->n * 4;
    uint32_t t[12];
    hex_mp(t, c->n, params[id][0]);
    mont_init(&c->p, t, c->n);
    hex_mp(t, c->n, params[id][1]);
    mont_init(&c->q, t, c->n);
    hex_mp(t, c->n, params[id][2]);
    to_mont(&c->p, c->b, t);
    hex_mp(t, c->n, params[id][3]);
    to_mont(&c->p, c->gx, t);
    hex_mp(t, c->n, params[id][4]);
    to_mont(&c->p, c->gy, t);
    uint32_t three[12] = {3};
    to_mont(&c->p, c->three, three);
    c->ready = true;
    return c;
}

static void fadd(const struct curve *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t carry = mp_add(r, a, b, c->n);
    if (carry || mp_cmp(r, c->p.m, c->n) >= 0)
        mp_sub(r, r, c->p.m, c->n);
}

static void fsub(const struct curve *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    if (mp_sub(r, a, b, c->n))
        mp_add(r, r, c->p.m, c->n);
}

static void fmul(const struct curve *c, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    mont_mul(&c->p, r, a, b);
}

static bool is_infinity(const struct curve *c, const struct point *p)
{
    return mp_zero(p->z, c->n);
}

static void point_double(const struct curve *c, struct point *r, const struct point *p)
{
    if (is_infinity(c, p)) {
        *r = *p;
        return;
    }
    uint32_t delta[12], gamma[12], beta[12], alpha[12], t[12], u[12];
    fmul(c, delta, p->z, p->z);
    fmul(c, gamma, p->y, p->y);
    fmul(c, beta, p->x, gamma);
    fsub(c, t, p->x, delta);
    fadd(c, u, p->x, delta);
    fmul(c, alpha, t, u);
    fmul(c, alpha, alpha, c->three);
    struct point o;
    fadd(c, t, p->y, p->z);
    fmul(c, t, t, t);
    fsub(c, t, t, gamma);
    fsub(c, o.z, t, delta);
    fmul(c, o.x, alpha, alpha);
    fadd(c, t, beta, beta);
    fadd(c, t, t, t);
    fadd(c, u, t, t);
    fsub(c, o.x, o.x, u);
    fsub(c, t, t, o.x);
    fmul(c, o.y, alpha, t);
    fmul(c, gamma, gamma, gamma);
    fadd(c, gamma, gamma, gamma);
    fadd(c, gamma, gamma, gamma);
    fadd(c, gamma, gamma, gamma);
    fsub(c, o.y, o.y, gamma);
    memcpy(r, &o, sizeof(o));
}

static void point_add(const struct curve *c, struct point *r, const struct point *a, const struct point *b)
{
    if (is_infinity(c, a)) {
        *r = *b;
        return;
    }
    if (is_infinity(c, b)) {
        *r = *a;
        return;
    }
    uint32_t z1z1[12], z2z2[12], u1[12], u2[12], s1[12], s2[12], h[12], rr[12], i[12], j[12], v[12], t[12];
    fmul(c, z1z1, a->z, a->z);
    fmul(c, z2z2, b->z, b->z);
    fmul(c, u1, a->x, z2z2);
    fmul(c, u2, b->x, z1z1);
    fmul(c, s1, a->y, b->z);
    fmul(c, s1, s1, z2z2);
    fmul(c, s2, b->y, a->z);
    fmul(c, s2, s2, z1z1);
    fsub(c, h, u2, u1);
    fsub(c, rr, s2, s1);
    if (mp_zero(h, c->n)) {
        if (mp_zero(rr, c->n)) {
            point_double(c, r, a);
        } else {
            memset(r, 0, sizeof(*r));
        }
        return;
    }
    fadd(c, rr, rr, rr);
    fadd(c, i, h, h);
    fmul(c, i, i, i);
    fmul(c, j, h, i);
    fmul(c, v, u1, i);
    struct point o;
    fmul(c, o.x, rr, rr);
    fsub(c, o.x, o.x, j);
    fsub(c, o.x, o.x, v);
    fsub(c, o.x, o.x, v);
    fsub(c, t, v, o.x);
    fmul(c, o.y, rr, t);
    fmul(c, t, s1, j);
    fadd(c, t, t, t);
    fsub(c, o.y, o.y, t);
    fadd(c, t, a->z, b->z);
    fmul(c, t, t, t);
    fsub(c, t, t, z1z1);
    fsub(c, t, t, z2z2);
    fmul(c, o.z, t, h);
    memcpy(r, &o, sizeof(o));
}

static void scalar_mul(const struct curve *c, struct point *r, const struct point *p, const uint32_t *k)
{
    struct point acc, dummy;
    memset(&acc, 0, sizeof(acc));
    for (int i = c->n * 32 - 1; i >= 0; i--) {
        point_double(c, &acc, &acc);
        if (k[i / 32] >> (i % 32) & 1)
            point_add(c, &acc, &acc, p);
        else
            point_add(c, &dummy, &acc, p);
    }
    *r = acc;
}

static bool to_affine(const struct curve *c, const struct point *p, uint32_t *x, uint32_t *y)
{
    if (is_infinity(c, p))
        return false;
    uint32_t exp[12], two[12] = {2}, zi[12], zi2[12];
    mp_sub(exp, c->p.m, two, c->n);
    mont_pow(&c->p, zi, p->z, exp, c->n);
    fmul(c, zi2, zi, zi);
    fmul(c, x, p->x, zi2);
    from_mont(&c->p, x, x);
    if (y) {
        fmul(c, zi2, zi2, zi);
        fmul(c, y, p->y, zi2);
        from_mont(&c->p, y, y);
    }
    return true;
}

static bool decode_point(const struct curve *c, const uint8_t *data, size_t len, struct point *p)
{
    if (len != 1 + 2 * c->bytes || data[0] != 4)
        return false;
    uint32_t x[12], y[12], lhs[12], rhs[12], t[12];
    mp_from_be(x, c->n, data + 1, c->bytes);
    mp_from_be(y, c->n, data + 1 + c->bytes, c->bytes);
    if (mp_cmp(x, c->p.m, c->n) >= 0 || mp_cmp(y, c->p.m, c->n) >= 0)
        return false;
    to_mont(&c->p, p->x, x);
    to_mont(&c->p, p->y, y);
    memcpy(p->z, c->p.one, sizeof(p->z));
    fmul(c, lhs, p->y, p->y);
    fmul(c, rhs, p->x, p->x);
    fmul(c, rhs, rhs, p->x);
    fmul(c, t, p->x, c->three);
    fsub(c, rhs, rhs, t);
    fadd(c, rhs, rhs, c->b);
    return mp_cmp(lhs, rhs, c->n) == 0;
}

static void generator(const struct curve *c, struct point *g)
{
    memcpy(g->x, c->gx, sizeof(g->x));
    memcpy(g->y, c->gy, sizeof(g->y));
    memcpy(g->z, c->p.one, sizeof(g->z));
}

size_t ec_size(enum ec_curve id)
{
    return id == EC_P256 ? 32 : 48;
}

bool ec_public_key(enum ec_curve id, const uint8_t *secret, uint8_t *point)
{
    struct curve *c = curve_get(id);
    uint32_t k[12], x[12], y[12];
    struct point g, r;
    mp_from_be(k, c->n, secret, c->bytes);
    if (mp_zero(k, c->n) || mp_cmp(k, c->q.m, c->n) >= 0)
        return false;
    generator(c, &g);
    scalar_mul(c, &r, &g, k);
    if (!to_affine(c, &r, x, y))
        return false;
    point[0] = 4;
    mp_to_be(point + 1, c->bytes, x, c->n);
    mp_to_be(point + 1 + c->bytes, c->bytes, y, c->n);
    return true;
}

bool ec_shared_secret(enum ec_curve id, const uint8_t *secret, const uint8_t *point, size_t point_len, uint8_t *out)
{
    struct curve *c = curve_get(id);
    struct point p, r;
    uint32_t k[12], x[12];
    if (!decode_point(c, point, point_len, &p))
        return false;
    mp_from_be(k, c->n, secret, c->bytes);
    scalar_mul(c, &r, &p, k);
    if (!to_affine(c, &r, x, NULL))
        return false;
    mp_to_be(out, c->bytes, x, c->n);
    return true;
}

static bool scalar_from(const struct curve *c, uint32_t *r, const uint8_t *b, size_t len)
{
    len = strip(&b, len);
    if (!len || len > c->bytes)
        return false;
    mp_from_be(r, c->n, b, len);
    return mp_cmp(r, c->q.m, c->n) < 0;
}

bool ecdsa_verify(enum ec_curve id, const uint8_t *point, size_t point_len, const uint8_t *hash, size_t hash_len,
                  const uint8_t *rb, size_t r_len, const uint8_t *sb, size_t s_len)
{
    struct curve *c = curve_get(id);
    struct point q, g, gq, acc;
    uint32_t r[12], s[12], e[12], w[12], u1[12], u2[12], t[12], exp[12], two[12] = {2}, x[12];
    if (!decode_point(c, point, point_len, &q) || !scalar_from(c, r, rb, r_len) || !scalar_from(c, s, sb, s_len))
        return false;
    mp_from_be(e, c->n, hash, MIN(hash_len, c->bytes));
    if (mp_cmp(e, c->q.m, c->n) >= 0)
        mp_sub(e, e, c->q.m, c->n);
    mp_sub(exp, c->q.m, two, c->n);
    to_mont(&c->q, t, s);
    mont_pow(&c->q, w, t, exp, c->n);
    to_mont(&c->q, t, e);
    mont_mul(&c->q, u1, t, w);
    from_mont(&c->q, u1, u1);
    to_mont(&c->q, t, r);
    mont_mul(&c->q, u2, t, w);
    from_mont(&c->q, u2, u2);
    generator(c, &g);
    point_add(c, &gq, &g, &q);
    memset(&acc, 0, sizeof(acc));
    for (int i = c->n * 32 - 1; i >= 0; i--) {
        point_double(c, &acc, &acc);
        int b1 = u1[i / 32] >> (i % 32) & 1, b2 = u2[i / 32] >> (i % 32) & 1;
        if (b1 && b2)
            point_add(c, &acc, &acc, &gq);
        else if (b1)
            point_add(c, &acc, &acc, &g);
        else if (b2)
            point_add(c, &acc, &acc, &q);
    }
    if (!to_affine(c, &acc, x, NULL))
        return false;
    if (mp_cmp(x, c->q.m, c->n) >= 0)
        mp_sub(x, x, c->q.m, c->n);
    return mp_cmp(x, r, c->n) == 0;
}
