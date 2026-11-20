/* A coverage-guided fuzzer for the file parsers of SynthOS, on the host (tests/fuzz, `make fuzz`). The parsers are
   built with -fsanitize-coverage=trace-pc (every basic block they enter calls __sanitizer_cov_trace_pc below) and
   with AddressSanitizer and UndefinedBehaviorSanitizer, which stop at the first bad read, write or overflow. Inputs
   start from the seed files of each target; a mutated input that reaches new code joins the corpus. When something
   goes wrong the input is saved as crash-<target>.bin first. Same seed, same run (FUZZ_SEED). */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "targets.h"

#define MAP_SIZE (1u << 16)
#define FUZZ_MAX (1u << 20)
#define MAX_CORPUS 4096

static uint8_t map[MAP_SIZE], seen[MAP_SIZE];
static uintptr_t prev;

void __sanitizer_cov_trace_pc(void)
{
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    uintptr_t h = (pc ^ (pc >> 16)) & (MAP_SIZE - 1);
    map[h ^ prev]++;
    prev = h >> 1;
}

struct input {
    uint8_t *d;
    size_t n;
};

static struct input corpus[MAX_CORPUS];
static int corpus_n;
static const uint8_t *cur;
static size_t cur_n;
static const char *cur_target;
static const char *out_dir = ".";
static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void save_crash(void)
{
    if (!cur)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/crash-%s.bin", out_dir, cur_target ? cur_target : "unknown");
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(cur, 1, cur_n, f);
        fclose(f);
        fprintf(stderr, "fuzz: the input is in %s\n", path);
    }
}

void __sanitizer_set_death_callback(void (*cb)(void));

static void on_signal(int sig)
{
    (void)sig;
    fprintf(stderr, "fuzz: %s took more than 10 seconds\n", cur_target);
    save_crash();
    _exit(1);
}

/* runs one input; true when it reached code no input reached before */
static int run(const struct target *t, const uint8_t *d, size_t n)
{
    memset(map, 0, sizeof(map));
    prev = 0;
    cur = d;
    cur_n = n;
    alarm(10);
    t->fn(d, n);
    alarm(0);
    int fresh = 0;
    for (size_t i = 0; i < MAP_SIZE; i++)
        if (map[i]) {
            /* hit counts in buckets, as AFL does: one, two, three, 4-7, 8-15, ... */
            uint8_t c = map[i], b = c >= 128 ? 128 : c >= 32 ? 64 : c >= 16 ? 32 : c >= 8 ? 16 : c >= 4 ? 8 : c;
            if (!(seen[i] & b)) {
                seen[i] |= b;
                fresh = 1;
            }
        }
    return fresh;
}

static void add(const uint8_t *d, size_t n)
{
    if (corpus_n >= MAX_CORPUS)
        return;
    corpus[corpus_n].d = malloc(n ? n : 1);
    memcpy(corpus[corpus_n].d, d, n);
    corpus[corpus_n++].n = n;
}

static const uint32_t interesting[] = {0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                                       0x7fffffff, 0x80000000u, 0xffffffffu, 16, 32, 64, 4096};

static size_t mutate(uint8_t *d, size_t n, size_t cap)
{
    int count = 1 + (int)(rnd() % 6);
    for (int k = 0; k < count; k++) {
        size_t at = n ? rnd() % n : 0;
        switch (rnd() % 9) {
        case 0: /* flip a bit */
            if (n)
                d[at] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        case 1: /* a random byte */
            if (n)
                d[at] = (uint8_t)rnd();
            break;
        case 2: /* an interesting number, either byte order */
            if (n >= 4 && at + 4 <= n) {
                uint32_t v = interesting[rnd() % (sizeof(interesting) / sizeof(*interesting))];
                if (rnd() & 1)
                    v = __builtin_bswap32(v);
                memcpy(d + at, &v, 1 + rnd() % 4);
            }
            break;
        case 3: /* add or take a little from a byte */
            if (n)
                d[at] += (uint8_t)(rnd() % 33) - 16;
            break;
        case 4: /* cut the end off */
            if (n > 1)
                n = 1 + rnd() % n;
            break;
        case 5: /* delete a run */
            if (n > 2) {
                size_t len = 1 + rnd() % (n - at < 64 ? n - at : 64);
                if (at + len <= n) {
                    memmove(d + at, d + at + len, n - at - len);
                    n -= len;
                }
            }
            break;
        case 6: /* repeat a run */
            if (n) {
                size_t len = 1 + rnd() % (n - at < 64 ? n - at : 64);
                if (n + len <= cap) {
                    memmove(d + at + len, d + at, n - at);
                    n += len;
                }
            }
            break;
        case 7: /* copy a run from elsewhere in the input */
            if (n > 8) {
                size_t from = rnd() % n, len = 1 + rnd() % 32;
                if (from + len <= n && at + len <= n)
                    memmove(d + at, d + from, len);
            }
            break;
        case 8: /* splice in a piece of another corpus entry */
            if (corpus_n > 1 && n) {
                const struct input *o = &corpus[rnd() % corpus_n];
                if (o->n) {
                    size_t from = rnd() % o->n, len = 1 + rnd() % 256;
                    if (from + len > o->n)
                        len = o->n - from;
                    if (at + len > n)
                        len = n - at;
                    memcpy(d + at, o->d + from, len);
                }
            }
            break;
        }
    }
    return n;
}

static void load_seeds(const char *dir)
{
    DIR *dd = opendir(dir);
    if (!dd)
        return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.')
            continue;
        char path[1536];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        uint8_t *buf = malloc(FUZZ_MAX);
        size_t n = fread(buf, 1, FUZZ_MAX, f);
        fclose(f);
        add(buf, n);
        free(buf);
    }
    closedir(dd);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fuzz SECONDS SEED_DIR [TARGET]   (crashes go to FUZZ_OUT, the seed is FUZZ_SEED)\n");
        return 2;
    }
    double seconds = atof(argv[1]);
    const char *only = argc > 3 ? argv[3] : NULL;
    if (getenv("FUZZ_OUT"))
        out_dir = getenv("FUZZ_OUT");
    if (getenv("FUZZ_SEED"))
        rng = strtoull(getenv("FUZZ_SEED"), NULL, 0) | 1;
    __sanitizer_set_death_callback(save_crash);
    signal(SIGALRM, on_signal);
    int count = 0;
    for (const struct target *t = targets; t->name; t++)
        count += !only || !strcmp(only, t->name);
    uint8_t *buf = malloc(FUZZ_MAX);
    for (const struct target *t = targets; t->name; t++) {
        if (only && strcmp(only, t->name))
            continue;
        cur_target = t->name;
        for (int i = 0; i < corpus_n; i++)
            free(corpus[i].d);
        corpus_n = 0;
        memset(seen, 0, sizeof(seen));
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s/%s", argv[2], t->name);
        load_seeds(dir);
        if (!corpus_n)
            add((const uint8_t *)"", 0);
        int seeds = corpus_n;
        for (int i = 0; i < seeds; i++)
            run(t, corpus[i].d, corpus[i].n);
        long execs = 0;
        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        double budget = seconds / count;
        for (;;) {
            if (!(execs & 15)) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (now.tv_sec - start.tv_sec + (now.tv_nsec - start.tv_nsec) / 1e9 >= budget)
                    break;
            }
            const struct input *base = &corpus[rnd() % corpus_n];
            size_t n = base->n < FUZZ_MAX ? base->n : FUZZ_MAX;
            memcpy(buf, base->d, n);
            n = mutate(buf, n, FUZZ_MAX);
            if (run(t, buf, n))
                add(buf, n);
            execs++;
        }
        printf("  FUZZ    %-8s %7ld runs, %4d seeds, corpus %4d\n", t->name, execs, seeds, corpus_n);
        fflush(stdout);
    }
    cur = NULL;
    free(buf);
    return 0;
}
