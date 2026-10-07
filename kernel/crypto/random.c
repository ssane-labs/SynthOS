#include <synth/crypto.h>
#include <synth/hal.h>
#include <synth/string.h>

static uint8_t pool_key[32];
static uint32_t counter;
static bool seeded;

static void stir(void)
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, pool_key, sizeof(pool_key));
    for (int i = 0; i < 64; i++) {
        uint32_t e = hal_entropy();
        sha256_update(&s, &e, sizeof(e));
    }
    uint64_t ms = hal_ticks_ms();
    sha256_update(&s, &ms, sizeof(ms));
    struct rtc_time t;
    hal_rtc(&t);
    sha256_update(&s, &t, sizeof(t));
    sha256_final(&s, pool_key);
    seeded = true;
}

void random_bytes(void *out, size_t n)
{
    static const uint8_t nonce[12] = {'s', 'y', 'n', 't', 'h', 'r', 'n', 'g'};
    if (!seeded || (counter & 0xff) == 0)
        stir();
    uint8_t *p = out, block[64];
    while (n) {
        chacha20_block(pool_key, counter++, nonce, block);
        size_t k = MIN(n, (size_t)32);
        memcpy(p, block + 32, k);
        p += k;
        n -= k;
    }
    chacha20_block(pool_key, counter++, nonce, block);
    memcpy(pool_key, block, 32);
}
