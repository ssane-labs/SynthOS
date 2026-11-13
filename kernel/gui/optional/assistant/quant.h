/* Floating point helpers and ggml's quantised blocks turned back into floats (the layouts of ggml-common.h). */

static float *half_table; /* made when the first model is loaded */

static float half_slow(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1fu, m = h & 0x3ffu, bits;
    if (!e) {
        float f = (float)m * (1.0f / 16777216.0f); /* zero and the subnormals: m * 2^-24 */
        return s ? -f : f;
    }
    if (e == 31)
        bits = s | 0x7f800000u | m << 13;
    else
        bits = s | (e + 112) << 23 | m << 13;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static bool half_init(void)
{
    if (half_table)
        return true;
    float *t = kmalloc(sizeof(float) * 65536);
    if (!t)
        return false;
    for (uint32_t i = 0; i < 65536; i++)
        t[i] = half_slow((uint16_t)i);
    half_table = t;
    return true;
}

static inline float h2f(const uint8_t *p)
{
    return half_table[(uint32_t)p[0] | (uint32_t)p[1] << 8];
}

static inline float bf2f(const uint8_t *p)
{
    uint32_t bits = ((uint32_t)p[0] | (uint32_t)p[1] << 8) << 16;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static inline float ai_sqrtf(float x)
{
#ifdef __x86_64__
    __asm__("sqrtss %1, %0" : "=x"(x) : "x"(x));
#else
    __asm__("fsqrt" : "+t"(x));
#endif
    return x;
}

/* e^x to float precision: x = k ln 2 + r with |r| <= ln 2 / 2, a degree 7 series for e^r, 2^k into the exponent */
static inline float ai_expf(float x)
{
    if (x < -87.0f)
        return 0.0f;
    if (x > 88.0f)
        x = 88.0f;
    float kf = x * 1.44269504f;
    int k = (int)(kf + (kf >= 0 ? 0.5f : -0.5f));
    float r = x - (float)k * 0.693145752f - (float)k * 1.42860677e-6f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (1.6666667e-1f + r * (4.1666667e-2f + r * (8.3333333e-3f +
                                                                        r * (1.3888889e-3f + r * 1.9841270e-4f))))));
    uint32_t bits = (uint32_t)(k + 127) << 23;
    float scale;
    memcpy(&scale, &bits, 4);
    return p * scale;
}

/* double precision helpers for the tables made at load time */
static double ai_floor(double x)
{
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}

static double ai_exp(double x)
{
    if (x < -700)
        return 0;
    double k = ai_floor(x / 0.69314718055994530942 + 0.5), r = x - k * 0.69314718055994530942, term = 1, sum = 1;
    for (int i = 1; i < 22; i++) {
        term *= r / i;
        sum += term;
    }
    while (k > 0) {
        sum *= 2;
        k--;
    }
    while (k < 0) {
        sum /= 2;
        k++;
    }
    return sum;
}

static double ai_log(double x)
{
    if (x <= 0)
        return -1e300;
    int e = 0;
    while (x >= 2) {
        x /= 2;
        e++;
    }
    while (x < 1) {
        x *= 2;
        e--;
    }
    double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
    for (int k = 1; k < 80; k += 2) {
        sum += term / k;
        term *= y2;
    }
    return 2 * sum + e * 0.69314718055994530942;
}

/* sine and cosine of any angle: reduced to [-pi/4, pi/4] and a series */
static void ai_sincos(double a, double *s, double *c)
{
    const double half_pi = 1.57079632679489661923;
    double q = ai_floor(a / half_pi + 0.5), r = a - q * half_pi, r2 = r * r;
    double sn = r, cs = 1, ts = r, tc = 1;
    for (int k = 1; k < 12; k++) {
        ts *= -r2 / ((2 * k) * (2 * k + 1));
        tc *= -r2 / ((2 * k - 1) * (2 * k));
        sn += ts;
        cs += tc;
    }
    switch ((long long)q & 3) {
    case 0: *s = sn; *c = cs; break;
    case 1: *s = cs; *c = -sn; break;
    case 2: *s = -sn; *c = -cs; break;
    default: *s = -cs; *c = sn; break;
    }
}

static void scale_min_k4(int j, const uint8_t *q, int *d, int *m)
{
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xf) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4);
    }
}

/* one block of a quantised row as floats (32 or 256 of them; 1 for F32/F16/BF16) */
static void deq_block(uint32_t type, const uint8_t *b, float *y)
{
    switch (type) {
    case GT_F32: memcpy(y, b, 4); break;
    case GT_F16: y[0] = h2f(b); break;
    case GT_BF16: y[0] = bf2f(b); break;
    case GT_Q4_0: {
        float d = h2f(b);
        for (int j = 0; j < 16; j++) {
            y[j] = (float)((b[2 + j] & 15) - 8) * d;
            y[j + 16] = (float)((b[2 + j] >> 4) - 8) * d;
        }
        break;
    }
    case GT_Q4_1: {
        float d = h2f(b), m = h2f(b + 2);
        for (int j = 0; j < 16; j++) {
            y[j] = (float)(b[4 + j] & 15) * d + m;
            y[j + 16] = (float)(b[4 + j] >> 4) * d + m;
        }
        break;
    }
    case GT_Q5_0:
    case GT_Q5_1: {
        bool one = type == GT_Q5_1;
        float d = h2f(b), m = one ? h2f(b + 2) : 0.0f;
        const uint8_t *h = b + (one ? 4 : 2), *qs = h + 4;
        uint32_t qh = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        int off = one ? 0 : 16;
        for (int j = 0; j < 16; j++) {
            int x0 = (int)((qs[j] & 15) | (((qh >> j) << 4) & 0x10)) - off;
            int x1 = (int)((qs[j] >> 4) | ((qh >> (j + 12)) & 0x10)) - off;
            y[j] = (float)x0 * d + m;
            y[j + 16] = (float)x1 * d + m;
        }
        break;
    }
    case GT_Q8_0: {
        float d = h2f(b);
        const int8_t *q = (const int8_t *)(b + 2);
        for (int j = 0; j < 32; j++)
            y[j] = (float)q[j] * d;
        break;
    }
    case GT_Q2_K: {
        const uint8_t *sc = b, *qs = b + 16;
        float d = h2f(b + 80), mn = h2f(b + 82);
        int is = 0;
        for (int n = 0; n < 256; n += 128) {
            const uint8_t *q = qs + n / 4;
            for (int shift = 0; shift < 8; shift += 2)
                for (int half = 0; half < 2; half++) {
                    int s = sc[is++];
                    float dl = d * (float)(s & 15), ml = mn * (float)(s >> 4);
                    for (int l = 0; l < 16; l++)
                        *y++ = dl * (float)((q[l + 16 * half] >> shift) & 3) - ml;
                }
        }
        break;
    }
    case GT_Q3_K: {
        const uint8_t *hm = b, *qs = b + 32;
        float d = h2f(b + 108);
        uint32_t aux[4];
        memcpy(aux, b + 96, 12);
        uint32_t tmp = aux[2];
        const uint32_t k1 = 0x03030303u, k2 = 0x0f0f0f0fu;
        aux[2] = ((aux[0] >> 4) & k2) | (((tmp >> 4) & k1) << 4);
        aux[3] = ((aux[1] >> 4) & k2) | (((tmp >> 6) & k1) << 4);
        aux[0] = (aux[0] & k2) | (((tmp >> 0) & k1) << 4);
        aux[1] = (aux[1] & k2) | (((tmp >> 2) & k1) << 4);
        int8_t scales[16];
        memcpy(scales, aux, 16);
        int is = 0;
        uint8_t m = 1;
        for (int n = 0; n < 256; n += 128) {
            const uint8_t *q = qs + n / 4;
            for (int shift = 0; shift < 8; shift += 2, m = (uint8_t)(m << 1))
                for (int half = 0; half < 2; half++) {
                    float dl = d * (float)(scales[is++] - 32);
                    for (int l = 0; l < 16; l++) {
                        int i = l + 16 * half;
                        *y++ = dl * (float)((int)((q[i] >> shift) & 3) - ((hm[i] & m) ? 0 : 4));
                    }
                }
        }
        break;
    }
    case GT_Q4_K:
    case GT_Q5_K: {
        float d = h2f(b), mn = h2f(b + 2);
        const uint8_t *sc = b + 4, *qh = type == GT_Q5_K ? b + 16 : NULL, *ql = type == GT_Q5_K ? b + 48 : b + 16;
        int is = 0;
        uint8_t u1 = 1, u2 = 2;
        for (int j = 0; j < 256; j += 64) {
            int s1, m1, s2, m2;
            scale_min_k4(is, sc, &s1, &m1);
            scale_min_k4(is + 1, sc, &s2, &m2);
            float d1 = d * (float)s1, n1 = mn * (float)m1, d2 = d * (float)s2, n2 = mn * (float)m2;
            const uint8_t *q = ql + j / 2;
            for (int l = 0; l < 32; l++)
                *y++ = d1 * (float)((q[l] & 15) + (qh && (qh[l] & u1) ? 16 : 0)) - n1;
            for (int l = 0; l < 32; l++)
                *y++ = d2 * (float)((q[l] >> 4) + (qh && (qh[l] & u2) ? 16 : 0)) - n2;
            is += 2;
            u1 = (uint8_t)(u1 << 2);
            u2 = (uint8_t)(u2 << 2);
        }
        break;
    }
    case GT_Q6_K: {
        const uint8_t *ql = b, *qh = b + 128;
        const int8_t *sc = (const int8_t *)(b + 192);
        float d = h2f(b + 208);
        for (int n = 0; n < 256; n += 128) {
            const uint8_t *L = ql + n / 2, *H = qh + n / 4;
            const int8_t *S = sc + n / 16;
            for (int l = 0; l < 32; l++) {
                int i = l / 16;
                int q1 = (int)((L[l] & 15) | ((H[l] & 3) << 4)) - 32;
                int q2 = (int)((L[l + 32] & 15) | (((H[l] >> 2) & 3) << 4)) - 32;
                int q3 = (int)((L[l] >> 4) | (((H[l] >> 4) & 3) << 4)) - 32;
                int q4 = (int)((L[l + 32] >> 4) | (((H[l] >> 6) & 3) << 4)) - 32;
                y[n + l] = d * (float)S[i] * (float)q1;
                y[n + l + 32] = d * (float)S[i + 2] * (float)q2;
                y[n + l + 64] = d * (float)S[i + 4] * (float)q3;
                y[n + l + 96] = d * (float)S[i + 6] * (float)q4;
            }
        }
        break;
    }
    }
}

/* a whole row of n values */
static void deq_row(uint32_t type, const uint8_t *row, float *y, int n)
{
    int bytes = 0, per = gguf_block(type, &bytes);
    if (type == GT_F32) {
        memcpy(y, row, (size_t)n * 4);
        return;
    }
    for (int i = 0; i < n; i += per, row += bytes)
        deq_block(type, row, y + i);
}

static inline float dot(const float *a, const float *b, int n)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++)
        s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}
