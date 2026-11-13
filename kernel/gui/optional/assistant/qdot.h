/* Integer dot products, the way llama.cpp gets its speed: the input of a matrix product is quantised once to 8
   bits with a scale for every 32 values, each block of weights is unpacked to 8-bit integers with a scale and an
   offset for every 16 (value = scale * q - offset; every quantised type fits that), and the products are summed
   as integers: 32 at a time with AVX2 where the processor has it, in plain C elsewhere. Rows of F32, F16 and BF16
   keep the float path. */

#ifdef __x86_64__
#define _MM_MALLOC_H_INCLUDED /* immintrin.h without its malloc wrapper: nothing to include it from here */
#include <immintrin.h>
#define QDOT_AVX2 1
#endif

static bool qdot_avx2; /* chosen when the model is loaded */

static void qdot_choose(void)
{
    qdot_avx2 = (hal_simd() & (HAL_SIMD_AVX2 | HAL_SIMD_FMA)) == (HAL_SIMD_AVX2 | HAL_SIMD_FMA);
}

static bool int_type(uint32_t type)
{
    switch (type) {
    case GT_Q4_0: case GT_Q4_1: case GT_Q5_0: case GT_Q5_1: case GT_Q8_0:
    case GT_Q2_K: case GT_Q3_K: case GT_Q4_K: case GT_Q5_K: case GT_Q6_K:
        return true;
    }
    return false;
}

/* n values (a multiple of 32) to 8 bits: q, a scale per 32 in d, the sum of every 16 in s16 */
static void quant_act(const float *x, int n, int8_t *q, float *d, int32_t *s16)
{
    for (int b = 0; b < n / 32; b++) {
        const float *v = x + b * 32;
        float amax = 0;
        for (int i = 0; i < 32; i++) {
            float a = v[i] < 0 ? -v[i] : v[i];
            if (a > amax)
                amax = a;
        }
        float dd = amax / 127.0f, id = dd > 0 ? 1.0f / dd : 0.0f;
        d[b] = dd;
        for (int h = 0; h < 2; h++) {
            int32_t sum = 0;
            for (int i = h * 16; i < h * 16 + 16; i++) {
                float s = v[i] * id;
                int r = s >= 0 ? (int)(s + 0.5f) : -(int)(-s + 0.5f);
                q[b * 32 + i] = (int8_t)r;
                sum += r;
            }
            s16[b * 2 + h] = sum;
        }
    }
}

/* one block of weights (32 or 256 values) as 8-bit integers, with a scale and an offset per 16; returns the count */
static int unpack_block(uint32_t type, const uint8_t *b, int8_t *q, float *sc, float *mn)
{
    switch (type) {
    case GT_Q4_0:
    case GT_Q4_1: {
        bool one = type == GT_Q4_1;
        float d = h2f(b), m = one ? h2f(b + 2) : 0.0f;
        const uint8_t *qs = b + (one ? 4 : 2);
        int off = one ? 0 : 8;
        for (int j = 0; j < 16; j++) {
            q[j] = (int8_t)((qs[j] & 15) - off);
            q[j + 16] = (int8_t)((qs[j] >> 4) - off);
        }
        sc[0] = sc[1] = d;
        mn[0] = mn[1] = -m;
        return 32;
    }
    case GT_Q5_0:
    case GT_Q5_1: {
        bool one = type == GT_Q5_1;
        float d = h2f(b), m = one ? h2f(b + 2) : 0.0f;
        const uint8_t *h = b + (one ? 4 : 2), *qs = h + 4;
        uint32_t qh = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        int off = one ? 0 : 16;
        for (int j = 0; j < 16; j++) {
            q[j] = (int8_t)((int)((qs[j] & 15) | (((qh >> j) << 4) & 0x10)) - off);
            q[j + 16] = (int8_t)((int)((qs[j] >> 4) | ((qh >> (j + 12)) & 0x10)) - off);
        }
        sc[0] = sc[1] = d;
        mn[0] = mn[1] = -m;
        return 32;
    }
    case GT_Q8_0:
        memcpy(q, b + 2, 32);
        sc[0] = sc[1] = h2f(b);
        mn[0] = mn[1] = 0;
        return 32;
    case GT_Q2_K: {
        const uint8_t *scs = b, *qs = b + 16;
        float d = h2f(b + 80), dmin = h2f(b + 82);
        int is = 0;
        for (int n = 0; n < 256; n += 128) {
            const uint8_t *src = qs + n / 4;
            for (int shift = 0; shift < 8; shift += 2)
                for (int half = 0; half < 2; half++, is++) {
                    sc[is] = d * (float)(scs[is] & 15);
                    mn[is] = dmin * (float)(scs[is] >> 4);
                    for (int l = 0; l < 16; l++)
                        q[is * 16 + l] = (int8_t)((src[l + 16 * half] >> shift) & 3);
                }
        }
        return 256;
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
            const uint8_t *src = qs + n / 4;
            for (int shift = 0; shift < 8; shift += 2, m = (uint8_t)(m << 1))
                for (int half = 0; half < 2; half++, is++) {
                    sc[is] = d * (float)(scales[is] - 32);
                    mn[is] = 0;
                    for (int l = 0; l < 16; l++) {
                        int i = l + 16 * half;
                        q[is * 16 + l] = (int8_t)((int)((src[i] >> shift) & 3) - ((hm[i] & m) ? 0 : 4));
                    }
                }
        }
        return 256;
    }
    case GT_Q4_K:
    case GT_Q5_K: {
        float d = h2f(b), dmin = h2f(b + 2);
        const uint8_t *scs = b + 4, *qh = type == GT_Q5_K ? b + 16 : NULL, *ql = type == GT_Q5_K ? b + 48 : b + 16;
        uint8_t u1 = 1, u2 = 2;
        for (int j = 0, is = 0; j < 256; j += 64, is += 2) {
            int s1, m1, s2, m2;
            scale_min_k4(is, scs, &s1, &m1);
            scale_min_k4(is + 1, scs, &s2, &m2);
            sc[j / 16] = sc[j / 16 + 1] = d * (float)s1;
            mn[j / 16] = mn[j / 16 + 1] = dmin * (float)m1;
            sc[j / 16 + 2] = sc[j / 16 + 3] = d * (float)s2;
            mn[j / 16 + 2] = mn[j / 16 + 3] = dmin * (float)m2;
            const uint8_t *src = ql + j / 2;
            for (int l = 0; l < 32; l++) {
                q[j + l] = (int8_t)((src[l] & 15) + (qh && (qh[l] & u1) ? 16 : 0));
                q[j + 32 + l] = (int8_t)((src[l] >> 4) + (qh && (qh[l] & u2) ? 16 : 0));
            }
            u1 = (uint8_t)(u1 << 2);
            u2 = (uint8_t)(u2 << 2);
        }
        return 256;
    }
    case GT_Q6_K: {
        const uint8_t *ql = b, *qh = b + 128;
        const int8_t *scs = (const int8_t *)(b + 192);
        float d = h2f(b + 208);
        for (int g = 0; g < 16; g++) {
            sc[g] = d * (float)scs[g];
            mn[g] = 0;
        }
        for (int n = 0; n < 256; n += 128) {
            const uint8_t *L = ql + n / 2, *H = qh + n / 4;
            for (int l = 0; l < 32; l++) {
                q[n + l] = (int8_t)((int)((L[l] & 15) | ((H[l] & 3) << 4)) - 32);
                q[n + l + 32] = (int8_t)((int)((L[l + 32] & 15) | (((H[l] >> 2) & 3) << 4)) - 32);
                q[n + l + 64] = (int8_t)((int)((L[l] >> 4) | (((H[l] >> 4) & 3) << 4)) - 32);
                q[n + l + 96] = (int8_t)((int)((L[l + 32] >> 4) | (((H[l] >> 6) & 3) << 4)) - 32);
            }
        }
        return 256;
    }
    }
    return 0;
}

/* sum over n values (a multiple of 32) of weight times input; x, xd, xs start at the block */
static float qdot_c(const int8_t *w, const float *sc, const float *mn, int n, const int8_t *x, const float *xd,
                    const int32_t *xs)
{
    float acc = 0;
    for (int g = 0; g < n / 16; g++) {
        int32_t s = 0;
        const int8_t *a = w + g * 16, *v = x + g * 16;
        for (int i = 0; i < 16; i++)
            s += (int32_t)a[i] * (int32_t)v[i];
        acc += xd[g / 2] * (sc[g] * (float)s - mn[g] * (float)xs[g]);
    }
    return acc;
}

#ifdef QDOT_AVX2
__attribute__((target("avx2,fma"))) static float qdot_avx(const int8_t *w, const float *sc, const float *mn, int n,
                                                           const int8_t *x, const float *xd, const int32_t *xs)
{
    __m256 acc = _mm256_setzero_ps();
    const __m256i ones = _mm256_set1_epi16(1);
    float off = 0;
    for (int i = 0; i < n; i += 32) {
        int g = i / 16;
        __m256i a = _mm256_loadu_si256((const __m256i *)(w + i)), v = _mm256_loadu_si256((const __m256i *)(x + i));
        /* unsigned times signed in pairs: |a| and the sign of a moved onto v */
        __m256i p = _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_abs_epi8(a), _mm256_sign_epi8(v, a)), ones);
        float d = xd[i / 32];
        __m256 s = _mm256_set_m128(_mm_set1_ps(sc[g + 1] * d), _mm_set1_ps(sc[g] * d));
        acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), s, acc);
        off += d * (mn[g] * (float)xs[g] + mn[g + 1] * (float)xs[g + 1]);
    }
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
    return _mm_cvtss_f32(h) - off;
}
#endif

static inline float qdot(const int8_t *w, const float *sc, const float *mn, int n, const int8_t *x, const float *xd,
                         const int32_t *xs)
{
#ifdef QDOT_AVX2
    if (qdot_avx2)
        return qdot_avx(w, sc, mn, n, x, xd, xs);
#endif
    return qdot_c(w, sc, mn, n, x, xd, xs);
}
