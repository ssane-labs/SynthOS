/* Integer dot products, the way llama.cpp gets its speed: the input of a matrix product is quantised once to 8
   bits with a scale for every 32 values, each block of weights is unpacked to 8-bit integers with a scale and an
   offset for every 16 (value = scale * q - offset; every quantised type fits that), and the products are summed
   as integers: 64 at a time with AVX-512 (VNNI's dpbusd when the processor has it), 32 at a time with AVX2 (or
   AVX-VNNI), in plain C elsewhere; the path is chosen by CPUID when a model is loaded. Rows of F32, F16 and BF16
   keep the float path. The key and value cache is kept in F16: F16C converts it where the processor has it. */

#ifdef __x86_64__
#define _MM_MALLOC_H_INCLUDED /* immintrin.h without its malloc wrapper: nothing to include it from here */
#include <immintrin.h>
#define QDOT_AVX2 1
#endif

/* chosen when the model is loaded */
static bool qdot_avx2, qdot_vnni256, qdot_avx512, qdot_vnni512, kv_f16c;

static void qdot_choose(void)
{
    uint32_t f = hal_simd();
#ifdef QDOT_AVX2
    qdot_avx2 = (f & (HAL_SIMD_AVX2 | HAL_SIMD_FMA)) == (HAL_SIMD_AVX2 | HAL_SIMD_FMA);
    qdot_vnni256 = qdot_avx2 && (f & HAL_SIMD_AVXVNNI);
    qdot_avx512 = qdot_avx2 && (f & HAL_SIMD_AVX512);
    qdot_vnni512 = qdot_avx512 && (f & HAL_SIMD_AVX512VNNI);
    kv_f16c = (f & (HAL_SIMD_F16C | HAL_SIMD_FMA | HAL_SIMD_AVX)) == (HAL_SIMD_F16C | HAL_SIMD_FMA | HAL_SIMD_AVX);
#else
    (void)f;
#endif
}

/* the path qdot takes, for ai bench and ai info */
static const char *qdot_path(void)
{
    return qdot_vnni512 ? "AVX-512 VNNI" : qdot_avx512 ? "AVX-512" : qdot_vnni256 ? "AVX-VNNI" : qdot_avx2 ? "AVX2"
                                                                                                        : "C";
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
/* the sums of 4 neighbouring products of |a| (unsigned) and v with a's sign: madd twice, or VNNI's dpbusd */
#define QDOT_BODY(name, attr, PAIRS)                                                                                   \
    __attribute__((target(attr))) static float name(const int8_t *w, const float *sc, const float *mn, int n,        \
                                                    const int8_t *x, const float *xd, const int32_t *xs)             \
    {                                                                                                                \
        __m256 acc = _mm256_setzero_ps();                                                                            \
        float off = 0;                                                                                               \
        for (int i = 0; i < n; i += 32) {                                                                            \
            int g = i / 16;                                                                                          \
            __m256i a = _mm256_loadu_si256((const __m256i *)(w + i));                                                \
            __m256i v = _mm256_loadu_si256((const __m256i *)(x + i));                                                \
            __m256i p = PAIRS(_mm256_abs_epi8(a), _mm256_sign_epi8(v, a));                                           \
            float d = xd[i / 32];                                                                                    \
            __m256 s = _mm256_set_m128(_mm_set1_ps(sc[g + 1] * d), _mm_set1_ps(sc[g] * d));                          \
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), s, acc);                                                    \
            off += d * (mn[g] * (float)xs[g] + mn[g + 1] * (float)xs[g + 1]);                                        \
        }                                                                                                            \
        __m128 h = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));                          \
        h = _mm_add_ps(h, _mm_movehl_ps(h, h));                                                                      \
        h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));                                                                  \
        return _mm_cvtss_f32(h) - off;                                                                               \
    }

#define PAIRS_AVX2(u, s) _mm256_madd_epi16(_mm256_maddubs_epi16(u, s), _mm256_set1_epi16(1))
#define PAIRS_VNNI(u, s) _mm256_dpbusd_avx_epi32(_mm256_setzero_si256(), u, s)
QDOT_BODY(qdot_avx, "avx2,fma", PAIRS_AVX2)
QDOT_BODY(qdot_avxvnni, "avx2,fma,avxvnni", PAIRS_VNNI)

/* 64 values at a time: four groups of 16, each with its own scale; a 32-value tail goes the AVX2 way */
#define QDOT512_BODY(name, attr, PAIRS)                                                                                \
    __attribute__((target(attr))) static float name(const int8_t *w, const float *sc, const float *mn, int n,        \
                                                    const int8_t *x, const float *xd, const int32_t *xs)             \
    {                                                                                                                \
        __m512 acc = _mm512_setzero_ps();                                                                            \
        const __m512i spread = _mm512_set_epi32(3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0);                    \
        float off = 0;                                                                                               \
        int i = 0;                                                                                                   \
        for (; i + 64 <= n; i += 64) {                                                                               \
            int g = i / 16;                                                                                          \
            __m512i a = _mm512_loadu_si512((const void *)(w + i)), v = _mm512_loadu_si512((const void *)(x + i));   \
            __mmask64 neg = _mm512_movepi8_mask(a);                                                                  \
            __m512i sv = _mm512_mask_sub_epi8(v, neg, _mm512_setzero_si512(), v);                                    \
            __m512i p = PAIRS(_mm512_abs_epi8(a), sv);                                                               \
            float d0 = xd[i / 32], d1 = xd[i / 32 + 1];                                                              \
            __m128 s4 = _mm_set_ps(sc[g + 3] * d1, sc[g + 2] * d1, sc[g + 1] * d0, sc[g] * d0);                     \
            __m512 s = _mm512_permutexvar_ps(spread, _mm512_castps128_ps512(s4));                                    \
            acc = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p), s, acc);                                                    \
            off += d0 * (mn[g] * (float)xs[g] + mn[g + 1] * (float)xs[g + 1]) +                                      \
                   d1 * (mn[g + 2] * (float)xs[g + 2] + mn[g + 3] * (float)xs[g + 3]);                               \
        }                                                                                                            \
        float r = _mm512_reduce_add_ps(acc) - off;                                                                   \
        if (i < n)                                                                                                   \
            r += qdot_avx(w + i, sc + i / 16, mn + i / 16, n - i, x + i, xd + i / 32, xs + i / 16);                 \
        return r;                                                                                                    \
    }

#define PAIRS_512(u, s) _mm512_madd_epi16(_mm512_maddubs_epi16(u, s), _mm512_set1_epi16(1))
#define PAIRS_512VNNI(u, s) _mm512_dpbusd_epi32(_mm512_setzero_si512(), u, s)
QDOT512_BODY(qdot_512, "avx512f,avx512bw,avx2,fma", PAIRS_512)
QDOT512_BODY(qdot_512vnni, "avx512f,avx512bw,avx512vnni,avx2,fma", PAIRS_512VNNI)
#endif

static inline float qdot(const int8_t *w, const float *sc, const float *mn, int n, const int8_t *x, const float *xd,
                         const int32_t *xs)
{
#ifdef QDOT_AVX2
    if (qdot_vnni512)
        return qdot_512vnni(w, sc, mn, n, x, xd, xs);
    if (qdot_avx512)
        return qdot_512(w, sc, mn, n, x, xd, xs);
    if (qdot_vnni256)
        return qdot_avxvnni(w, sc, mn, n, x, xd, xs);
    if (qdot_avx2)
        return qdot_avx(w, sc, mn, n, x, xd, xs);
#endif
    return qdot_c(w, sc, mn, n, x, xd, xs);
}

/* ---- K-quant blocks straight from the file: no unpacking into a buffer (what llama.cpp's vec_dot_q4_K_q8_K
   does); x, xd, xs start at the block. Each 32 values of Q4_K share a scale and a minimum, so a sub-block is one
   integer product times d * scale * xd minus dmin * min * xd * (sum of x); Q6_K's values are u - 32 with u in 0..63,
   so the unsigned product is corrected by 32 * the sum of x. ---- */

#ifdef QDOT_AVX2
static void q4k_scales(const uint8_t *b, float *sc, float *mn)
{
    float d = h2f(b), dmin = h2f(b + 2);
    for (int j = 0; j < 8; j++) {
        int s, m;
        scale_min_k4(j, b + 4, &s, &m);
        sc[j] = d * (float)s;
        mn[j] = dmin * (float)m;
    }
}

static inline __attribute__((target("avx2,fma"))) float hsum8(__m256 a)
{
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
    return _mm_cvtss_f32(h);
}

__attribute__((target("avx2,fma"))) static float q4k_avx2(const uint8_t *b, const int8_t *x, const float *xd,
                                                          const int32_t *xs)
{
    float sc[8], mn[8], off = 0;
    q4k_scales(b, sc, mn);
    const uint8_t *qs = b + 16;
    const __m256i m4 = _mm256_set1_epi8(15), ones = _mm256_set1_epi16(1);
    __m256 acc = _mm256_setzero_ps();
    for (int c = 0; c < 4; c++) {
        __m256i q = _mm256_loadu_si256((const __m256i *)(qs + 32 * c));
        __m256i lo = _mm256_and_si256(q, m4), hi = _mm256_and_si256(_mm256_srli_epi16(q, 4), m4);
        __m256i p0 = _mm256_madd_epi16(_mm256_maddubs_epi16(lo, _mm256_loadu_si256((const __m256i *)(x + 64 * c))),
                                       ones);
        __m256i p1 = _mm256_madd_epi16(
            _mm256_maddubs_epi16(hi, _mm256_loadu_si256((const __m256i *)(x + 64 * c + 32))), ones);
        int j = 2 * c;
        acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p0), _mm256_set1_ps(sc[j] * xd[j]), acc);
        acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p1), _mm256_set1_ps(sc[j + 1] * xd[j + 1]), acc);
        off += mn[j] * xd[j] * (float)(xs[2 * j] + xs[2 * j + 1]) +
               mn[j + 1] * xd[j + 1] * (float)(xs[2 * j + 2] + xs[2 * j + 3]);
    }
    return hsum8(acc) - off;
}

/* 64 values at a time: the low and high nibbles of 32 bytes side by side, VNNI's dpbusd where there is one */
#define Q4K512_BODY(name, attr, PAIRS)                                                                                 \
    __attribute__((target(attr))) static float name(const uint8_t *b, const int8_t *x, const float *xd,             \
                                                    const int32_t *xs)                                               \
    {                                                                                                                \
        float sc[8], mn[8], off = 0;                                                                                 \
        q4k_scales(b, sc, mn);                                                                                       \
        const uint8_t *qs = b + 16;                                                                                  \
        const __m256i m4 = _mm256_set1_epi8(15);                                                                     \
        __m512 acc = _mm512_setzero_ps();                                                                            \
        for (int c = 0; c < 4; c++) {                                                                                \
            __m256i q = _mm256_loadu_si256((const __m256i *)(qs + 32 * c));                                          \
            __m512i u = _mm512_inserti64x4(_mm512_castsi256_si512(_mm256_and_si256(q, m4)),                          \
                                           _mm256_and_si256(_mm256_srli_epi16(q, 4), m4), 1);                        \
            __m512i p = PAIRS(u, _mm512_loadu_si512((const void *)(x + 64 * c)));                                    \
            int j = 2 * c;                                                                                           \
            __m512 s = _mm512_mask_blend_ps((__mmask16)0xff00, _mm512_set1_ps(sc[j] * xd[j]),                        \
                                            _mm512_set1_ps(sc[j + 1] * xd[j + 1]));                                  \
            acc = _mm512_fmadd_ps(_mm512_cvtepi32_ps(p), s, acc);                                                    \
            off += mn[j] * xd[j] * (float)(xs[2 * j] + xs[2 * j + 1]) +                                              \
                   mn[j + 1] * xd[j + 1] * (float)(xs[2 * j + 2] + xs[2 * j + 3]);                                   \
        }                                                                                                            \
        return _mm512_reduce_add_ps(acc) - off;                                                                      \
    }

Q4K512_BODY(q4k_512, "avx512f,avx512bw,avx2,fma", PAIRS_512)
Q4K512_BODY(q4k_512vnni, "avx512f,avx512bw,avx512vnni,avx2,fma", PAIRS_512VNNI)

__attribute__((target("avx2,fma"))) static float q6k_avx2(const uint8_t *b, const int8_t *x, const float *xd,
                                                          const int32_t *xs)
{
    const uint8_t *ql = b, *qh = b + 128;
    const int8_t *scs = (const int8_t *)(b + 192);
    float d = h2f(b + 208), off = 0;
    const __m256i m4 = _mm256_set1_epi8(15), m2 = _mm256_set1_epi8(3), ones = _mm256_set1_epi16(1);
    __m256 acc = _mm256_setzero_ps();
    for (int n = 0; n < 256; n += 128) {
        __m256i l0 = _mm256_loadu_si256((const __m256i *)(ql + n / 2));
        __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + n / 2 + 32));
        __m256i h = _mm256_loadu_si256((const __m256i *)(qh + n / 4));
        __m256i u[4] = {
            _mm256_or_si256(_mm256_and_si256(l0, m4), _mm256_slli_epi16(_mm256_and_si256(h, m2), 4)),
            _mm256_or_si256(_mm256_and_si256(l1, m4),
                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m2), 4)),
            _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m2), 4)),
            _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m2), 4))};
        for (int k = 0; k < 4; k++) {
            int at = n + 32 * k, g = at / 16;
            __m256i p = _mm256_madd_epi16(
                _mm256_maddubs_epi16(u[k], _mm256_loadu_si256((const __m256i *)(x + at))), ones);
            float dd = d * xd[at / 32], s0 = dd * (float)scs[g], s1 = dd * (float)scs[g + 1];
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(p), _mm256_set_m128(_mm_set1_ps(s1), _mm_set1_ps(s0)), acc);
            off += 32.0f * (s0 * (float)xs[g] + s1 * (float)xs[g + 1]);
        }
    }
    return hsum8(acc) - off;
}
#endif

/* the types qdot_block takes straight from the file on this processor */
static bool qdot_direct(uint32_t type)
{
    return qdot_avx2 && (type == GT_Q4_K || type == GT_Q6_K || type == GT_Q8_0);
}

/* one block of a type qdot_direct takes, times the 8-bit input at its place */
static float qdot_block(uint32_t type, const uint8_t *b, const int8_t *x, const float *xd, const int32_t *xs)
{
#ifdef QDOT_AVX2
    if (type == GT_Q4_K)
        return qdot_vnni512 ? q4k_512vnni(b, x, xd, xs) : qdot_avx512 ? q4k_512(b, x, xd, xs) : q4k_avx2(b, x, xd, xs);
    if (type == GT_Q6_K)
        return q6k_avx2(b, x, xd, xs);
    if (type == GT_Q8_0) {
        float sc[2], mn[2] = {0, 0};
        sc[0] = sc[1] = h2f(b);
        return qdot_vnni256 ? qdot_avxvnni((const int8_t *)b + 2, sc, mn, 32, x, xd, xs)
                            : qdot_avx((const int8_t *)b + 2, sc, mn, 32, x, xd, xs);
    }
#endif
    (void)type, (void)b, (void)x, (void)xd, (void)xs;
    return 0;
}


/* ---- the key and value cache in F16 ---- */

/* float to half, rounded to nearest even; too big becomes infinity */
static inline uint16_t f2h(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    uint32_t sign = (b >> 16) & 0x8000u, e = (b >> 23) & 0xffu, m = b & 0x7fffffu;
    if (e == 255)
        return (uint16_t)(sign | 0x7c00u | (m ? 0x200u : 0));
    int he = (int)e - 112;
    if (he >= 31)
        return (uint16_t)(sign | 0x7c00u);
    if (he <= 0) { /* a subnormal half, or zero */
        if (he < -10)
            return (uint16_t)sign;
        m |= 0x800000u;
        int shift = 14 - he;
        uint32_t r = m >> shift, rest = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (r & 1)))
            r++;
        return (uint16_t)(sign | r);
    }
    uint32_t r = (uint32_t)he << 10 | m >> 13, rest = m & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (r & 1)))
        r++; /* may carry into the exponent, up to infinity: right */
    return (uint16_t)(sign | r);
}

#ifdef QDOT_AVX2
__attribute__((target("avx,f16c,fma"))) static void kv_put_f16c(uint16_t *d, const float *s, int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8)
        _mm_storeu_si128((__m128i *)(d + i), _mm256_cvtps_ph(_mm256_loadu_ps(s + i), _MM_FROUND_TO_NEAREST_INT));
    for (; i < n; i++)
        d[i] = f2h(s[i]);
}

__attribute__((target("avx,f16c,fma"))) static float kv_dot_f16c(const float *q, const uint16_t *k, int n)
{
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(q + i), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(k + i))), a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(q + i + 8),
                             _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(k + i + 8))), a1);
    }
    for (; i + 8 <= n; i += 8)
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(q + i), _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(k + i))), a0);
    a0 = _mm256_add_ps(a0, a1);
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(a0), _mm256_extractf128_ps(a0, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
    float r = _mm_cvtss_f32(h);
    for (; i < n; i++)
        r += q[i] * half_table[k[i]];
    return r;
}

__attribute__((target("avx,f16c,fma"))) static void kv_add_f16c(float *o, float w, const uint16_t *v, int n)
{
    __m256 ww = _mm256_set1_ps(w);
    int i = 0;
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(o + i, _mm256_fmadd_ps(ww, _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(v + i))),
                                                _mm256_loadu_ps(o + i)));
    for (; i < n; i++)
        o[i] += w * half_table[v[i]];
}
#endif

static inline void kv_put(uint16_t *d, const float *s, int n)
{
#ifdef QDOT_AVX2
    if (kv_f16c) {
        kv_put_f16c(d, s, n);
        return;
    }
#endif
    for (int i = 0; i < n; i++)
        d[i] = f2h(s[i]);
}

/* q . k with k in halves */
static inline float kv_dot(const float *q, const uint16_t *k, int n)
{
#ifdef QDOT_AVX2
    if (kv_f16c)
        return kv_dot_f16c(q, k, n);
#endif
    float s0 = 0, s1 = 0;
    int i = 0;
    for (; i + 2 <= n; i += 2) {
        s0 += q[i] * half_table[k[i]];
        s1 += q[i + 1] * half_table[k[i + 1]];
    }
    for (; i < n; i++)
        s0 += q[i] * half_table[k[i]];
    return s0 + s1;
}

/* o += w * v with v in halves */
static inline void kv_add(float *o, float w, const uint16_t *v, int n)
{
#ifdef QDOT_AVX2
    if (kv_f16c) {
        kv_add_f16c(o, w, v, n);
        return;
    }
#endif
    for (int i = 0; i < n; i++)
        o[i] += w * half_table[v[i]];
}
