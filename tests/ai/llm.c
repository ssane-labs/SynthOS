/* The assistant's language model engine on the host: loads a .gguf (tests/aimodel.py writes tiny ones) and prints
   the next tokens greedy decoding picks for a prompt of token numbers, the way `ai next` does in SynthOS.
   usage: llmtest model.gguf [--exact] [--plain] id id ... -n count
   --plain: no SIMD (the C paths), else every path this processor has
   usage: llmtest model.gguf [--plain] --bench n: prompt and generation speed on one core, like `ai bench n` */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define kmalloc malloc
#define kfree free
#define kzalloc(n) calloc(1, n)
#define ksnprintf snprintf
#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))
#define vmm_big_alloc(n) NULL
#define vmm_big_free(p, n) free(p)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HAL_SIMD_AVX 0x008u
#define HAL_SIMD_AVX2 0x010u
#define HAL_SIMD_FMA 0x020u
#define HAL_SIMD_F16C 0x040u
#define HAL_SIMD_AVXVNNI 0x080u
#define HAL_SIMD_AVX512 0x100u
#define HAL_SIMD_AVX512VNNI 0x200u
enum { E_NOMEM = -12, E_INVAL = -22, E_NOSYS = -38, E_RANGE = -34, E_INTR = -4 };

static void str_copy(char *d, const char *s, size_t cap)
{
    snprintf(d, cap, "%s", s);
}

static struct {
    int speed;
} gui_ai;
static bool plain;
static uint32_t hal_simd(void)
{
    uint32_t f = 0;
    if (plain)
        return 0;
    __builtin_cpu_init();
    f |= __builtin_cpu_supports("avx") ? HAL_SIMD_AVX : 0;
    f |= __builtin_cpu_supports("avx2") ? HAL_SIMD_AVX2 : 0;
    f |= __builtin_cpu_supports("fma") ? HAL_SIMD_FMA : 0;
    f |= __builtin_cpu_supports("f16c") ? HAL_SIMD_F16C : 0;
    f |= __builtin_cpu_supports("avxvnni") ? HAL_SIMD_AVXVNNI : 0;
    f |= __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") ? HAL_SIMD_AVX512 : 0;
    f |= __builtin_cpu_supports("avx512vnni") ? HAL_SIMD_AVX512VNNI : 0;
    return f;
}
static void hal_parallel(void (*fn)(void *, int, int), void *ctx, int count, int grain)
{
    (void)grain;
    fn(ctx, 0, count);
}
static const void *(*module_service)(const char *) = NULL;
struct ai_backend {
    const char *name;
    void *(*upload)(const void *, uint32_t, int, int, size_t);
    void (*release)(void *);
    bool (*matmul)(void *, const float *, int, float *, int, int);
};

#include "../../kernel/gui/optional/assistant/gguf.h"
#include "../../kernel/gui/optional/assistant/quant.h"
#include "../../kernel/gui/optional/assistant/tok.h"
#include "../../kernel/gui/optional/assistant/qdot.h"
#include "../../kernel/gui/optional/assistant/llm.h"

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(size);
    if (fread(file, 1, size, f) != size)
        return 2;
    fclose(f);
    int ids[512], n = 0, gen = 6, bench = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--exact"))
            gui_ai.speed = 1;
        else if (!strcmp(argv[i], "--plain"))
            plain = true;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc)
            bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            gen = atoi(argv[++i]);
        else
            ids[n++] = atoi(argv[i]);
    }
    static struct ai_model m;
    char why[160];
    int err = model_load(&m, file, size, 512, why, sizeof(why));
    if (err) {
        printf("load: %s\n", why);
        return 1;
    }
    if (bench > 0) {
        bench = MIN(bench, 256);
        for (int i = 0; i < bench; i++)
            ids[i] = (int)((uint32_t)(i * 7919 + 13) % (uint32_t)MIN(m.vocab, 1000));
        struct timespec a, b, c;
        clock_gettime(CLOCK_MONOTONIC, &a);
        model_feed(&m, ids, bench, NULL);
        clock_gettime(CLOCK_MONOTONIC, &b);
        int id = ids[bench - 1];
        for (int i = 0; i < bench / 4; i++)
            forward(&m, &id, 1, true);
        clock_gettime(CLOCK_MONOTONIC, &c);
        double p = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
        double g = (double)(c.tv_sec - b.tv_sec) + (double)(c.tv_nsec - b.tv_nsec) / 1e9;
        printf("%s: prompt %d tokens %.2f tokens/s, generation %d tokens %.2f tokens/s (%s, one core)\n", m.name,
               bench, bench / p, bench / 4, (bench / 4) / g, plain ? "C" : qdot_path());
        return 0;
    }
    model_feed(&m, ids, n, NULL);
    struct sampler s = {0, 0, 1.0f, 1};
    printf("next:");
    for (int i = 0; i < gen; i++) {
        int id = sample(&m, &s);
        printf(" %d", id);
        forward(&m, &id, 1, true);
    }
    printf("\n");
    return 0;
}
