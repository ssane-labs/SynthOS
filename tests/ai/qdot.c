/* The assistant's integer dot products on the host: every path the processor here has (AVX2, AVX-VNNI, AVX-512,
   AVX-512 VNNI) must give what the plain C one gives, for every quantised type (and the K-quant blocks read straight
   from the file must give what their unpacked values give), and the F16 key/value cache must
   round like the processor's own conversion. Built and run by `make test-ai`. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kmalloc malloc
#define kfree free
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define HAL_SIMD_AVX 0x008u
#define HAL_SIMD_AVX2 0x010u
#define HAL_SIMD_FMA 0x020u
#define HAL_SIMD_F16C 0x040u
#define HAL_SIMD_AVXVNNI 0x080u
#define HAL_SIMD_AVX512 0x100u
#define HAL_SIMD_AVX512VNNI 0x200u

static uint32_t simd_bits;
static uint32_t hal_simd(void)
{
    return simd_bits;
}

enum {
    GT_F32 = 0, GT_F16 = 1, GT_Q4_0 = 2, GT_Q4_1 = 3, GT_Q5_0 = 6, GT_Q5_1 = 7, GT_Q8_0 = 8, GT_Q2_K = 10,
    GT_Q3_K = 11, GT_Q4_K = 12, GT_Q5_K = 13, GT_Q6_K = 14, GT_BF16 = 30
};

static int gguf_block(uint32_t type, int *bytes)
{
    (void)type;
    *bytes = 4;
    return 1;
}

#include "../../kernel/gui/optional/assistant/quant.h"
#include "../../kernel/gui/optional/assistant/qdot.h"

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint32_t host_simd(void)
{
    uint32_t f = 0;
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx"))
        f |= HAL_SIMD_AVX;
    if (__builtin_cpu_supports("avx2"))
        f |= HAL_SIMD_AVX2;
    if (__builtin_cpu_supports("fma"))
        f |= HAL_SIMD_FMA;
    if (__builtin_cpu_supports("f16c"))
        f |= HAL_SIMD_F16C;
    if (__builtin_cpu_supports("avxvnni"))
        f |= HAL_SIMD_AVXVNNI;
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))
        f |= HAL_SIMD_AVX512;
    if (__builtin_cpu_supports("avx512vnni"))
        f |= HAL_SIMD_AVX512VNNI;
    return f;
}

int main(void)
{
    static const uint32_t types[] = {GT_Q4_0, GT_Q4_1, GT_Q5_0, GT_Q5_1, GT_Q8_0,
                                     GT_Q2_K, GT_Q3_K, GT_Q4_K, GT_Q5_K, GT_Q6_K};
    static const uint32_t paths[] = {0, HAL_SIMD_AVX2 | HAL_SIMD_FMA, HAL_SIMD_AVX2 | HAL_SIMD_FMA | HAL_SIMD_AVXVNNI,
                                     HAL_SIMD_AVX2 | HAL_SIMD_FMA | HAL_SIMD_AVX512,
                                     HAL_SIMD_AVX2 | HAL_SIMD_FMA | HAL_SIMD_AVX512 | HAL_SIMD_AVX512VNNI};
    uint32_t have = host_simd();
    half_init();
    int fails = 0, runs = 0;
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++)
        for (int trial = 0; trial < 200; trial++) {
            uint8_t block[256];
            for (int i = 0; i < 256; i++)
                block[i] = (uint8_t)rnd();
            /* scales as small halves, so values stay sane */
            for (int i = 0; i < 256; i += 2) {
                uint16_t h = (uint16_t)(0x2000 + rnd() % 0x1000);
                if (types[t] >= GT_Q2_K && i != 0 && i != 80 && i != 82 && i != 108 && i != 208 && i != 2)
                    continue;
                if (types[t] < GT_Q2_K && i > 2)
                    break;
                block[i] = (uint8_t)h;
                block[i + 1] = (uint8_t)(h >> 8);
            }
            int8_t q[256];
            float sc[16], mn[16];
            int n = unpack_block(types[t], block, q, sc, mn);
            float x[256], xd[8];
            int8_t xq[256];
            int32_t xs[16];
            for (int i = 0; i < n; i++)
                x[i] = (float)((int)(rnd() % 2001) - 1000) / 250.0f;
            quant_act(x, n, xq, xd, xs);
            simd_bits = 0;
            qdot_choose();
            float want = qdot(q, sc, mn, n, xq, xd, xs);
            for (size_t p = 1; p < sizeof(paths) / sizeof(paths[0]); p++) {
                if ((have & paths[p]) != paths[p])
                    continue;
                simd_bits = paths[p];
                qdot_choose();
                float got = qdot(q, sc, mn, n, xq, xd, xs);
                runs++;
                if (fabsf(got - want) > 1e-3f * (1.0f + fabsf(want))) {
                    if (fails++ < 10)
                        printf("FAIL type %u path %s: %g, C %g\n", types[t], qdot_path(), got, want);
                }
                if (qdot_direct(types[t])) { /* the block straight from the file */
                    got = qdot_block(types[t], block, xq, xd, xs);
                    runs++;
                    if (fabsf(got - want) > 1e-3f * (1.0f + fabsf(want)) && fails++ < 10)
                        printf("FAIL direct type %u path %s: %g, C %g\n", types[t], qdot_path(), got, want);
                }
            }
        }
    /* halves: every float that rounds to a half must match the processor's conversion */
    simd_bits = have;
    qdot_choose();
    int hfail = 0;
    for (int i = 0; i < 2000000; i++) {
        uint32_t b = rnd();
        float f;
        memcpy(&f, &b, 4);
        if (f != f)
            continue;
        uint16_t mine = f2h(f);
        float eight[8] = {f, f, f, f, f, f, f, f};
        uint16_t out[8];
        kv_put(out, eight, 8);
        if (kv_f16c && out[0] != mine && hfail++ < 5)
            printf("FAIL half of %08x: %04x, F16C %04x\n", b, mine, out[0]);
    }
    float q[64], o[64] = {0}, ref = 0;
    uint16_t k[64];
    for (int i = 0; i < 64; i++) {
        q[i] = (float)((int)(rnd() % 200) - 100) / 37.0f;
        float v = (float)((int)(rnd() % 200) - 100) / 41.0f;
        k[i] = f2h(v);
        ref += q[i] * half_table[k[i]];
    }
    float d = kv_dot(q, k, 61);
    for (int i = 61; i < 64; i++)
        ref -= q[i] * half_table[k[i]];
    kv_add(o, 0.5f, k, 61);
    if (fabsf(d - ref) > 1e-3f || o[60] != 0.5f * half_table[k[60]] || o[61] != 0) {
        printf("FAIL kv_dot %g want %g\n", d, ref);
        hfail++;
    }
    printf("qdot: %d checks on %s and below, %d failed; halves %s%s\n", runs, qdot_path(), fails,
           hfail ? "FAILED" : "ok", kv_f16c ? " (F16C)" : "");
    return fails || hfail;
}
