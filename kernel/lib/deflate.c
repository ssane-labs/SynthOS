#include <synth/error.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/string.h>

/* A small deflate compressor: LZ77 over a 32 KiB window with hash chains, written as one block with the fixed
   Huffman codes. Screenshots and other flat pictures shrink well with it; zlib_deflate_alloc wraps the result the
   way PNG wants it. */

#define WINDOW 32768
#define HASH_BITS 15
#define CHAIN 24
#define MIN_MATCH 3
#define MAX_MATCH 258

struct bits {
    uint8_t *out;
    size_t n, cap;
    uint32_t acc;
    int count;
    bool fail;
};

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void flush_byte(struct bits *b, uint8_t v)
{
    if (b->n == b->cap) {
        size_t cap = b->cap * 2;
        uint8_t *bigger = krealloc(b->out, cap);
        if (!bigger) {
            b->fail = true;
            return;
        }
        b->out = bigger;
        b->cap = cap;
    }
    b->out[b->n++] = v;
}

static void put(struct bits *b, uint32_t v, int n)
{
    b->acc |= v << b->count;
    b->count += n;
    while (b->count >= 8) {
        flush_byte(b, (uint8_t)b->acc);
        b->acc >>= 8;
        b->count -= 8;
    }
}

/* Huffman codes go most significant bit first. */
static void put_code(struct bits *b, uint32_t code, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= (code >> i & 1) << (n - 1 - i);
    put(b, r, n);
}

static void literal(struct bits *b, int v)
{
    if (v < 144)
        put_code(b, 0x30 + (uint32_t)v, 8);
    else if (v < 256)
        put_code(b, 0x190 + (uint32_t)(v - 144), 9);
    else if (v < 280)
        put_code(b, (uint32_t)(v - 256), 7);
    else
        put_code(b, 0xc0 + (uint32_t)(v - 280), 8);
}

static void match(struct bits *b, int length, int dist)
{
    int i = 28;
    while (len_base[i] > length)
        i--;
    literal(b, 257 + i);
    put(b, (uint32_t)(length - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist)
        d--;
    put_code(b, (uint32_t)d, 5);
    put(b, (uint32_t)(dist - dist_base[d]), dist_extra[d]);
}

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HASH_BITS);
}

int deflate_alloc(const void *in, size_t n, uint8_t **out, size_t *out_len, size_t reserve)
{
    const uint8_t *src = in;
    int32_t *head = kmalloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = kmalloc(sizeof(int32_t) * WINDOW);
    struct bits b = {kmalloc(n / 4 + 1024 + reserve), reserve, n / 4 + 1024 + reserve, 0, 0, false};
    if (!head || !prev || !b.out) {
        kfree(head);
        kfree(prev);
        kfree(b.out);
        return E_NOMEM;
    }
    memset(head, 0xff, sizeof(int32_t) << HASH_BITS);
    put(&b, 1, 1);
    put(&b, 1, 2);
    size_t i = 0;
    while (i < n && !b.fail) {
        int best = 0, best_dist = 0;
        if (i + MIN_MATCH <= n) {
            uint32_t h = hash3(src + i);
            int32_t cand = head[h];
            size_t limit = MIN((size_t)MAX_MATCH, n - i);
            for (int chain = 0; cand >= 0 && i - (size_t)cand <= WINDOW && chain < CHAIN; chain++) {
                const uint8_t *a = src + cand, *c = src + i;
                if (a[best] == c[best]) {
                    size_t k = 0;
                    while (k < limit && a[k] == c[k])
                        k++;
                    if ((int)k > best) {
                        best = (int)k;
                        best_dist = (int)(i - (size_t)cand);
                        if (k == limit)
                            break;
                    }
                }
                cand = prev[cand % WINDOW];
            }
            prev[i % WINDOW] = head[h];
            head[h] = (int32_t)i;
        }
        if (best >= MIN_MATCH) {
            match(&b, best, best_dist);
            for (size_t k = 1; k < (size_t)best && i + k + MIN_MATCH <= n; k++) {
                uint32_t h = hash3(src + i + k);
                prev[(i + k) % WINDOW] = head[h];
                head[h] = (int32_t)(i + k);
            }
            i += (size_t)best;
        } else {
            literal(&b, src[i]);
            i++;
        }
    }
    literal(&b, 256);
    if (b.count)
        put(&b, 0, 8 - b.count);
    kfree(head);
    kfree(prev);
    if (b.fail) {
        kfree(b.out);
        return E_NOMEM;
    }
    *out = b.out;
    *out_len = b.n;
    return 0;
}

int zlib_deflate_alloc(const void *in, size_t n, uint8_t **out, size_t *out_len)
{
    uint8_t *data;
    size_t len;
    int err = deflate_alloc(in, n, &data, &len, 2);
    if (err)
        return err;
    data[0] = 0x78;
    data[1] = 0x01;
    uint32_t a = 1, s = 0;
    const uint8_t *p = in;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        s = (s + a) % 65521;
    }
    uint8_t *bigger = krealloc(data, len + 4);
    if (!bigger) {
        kfree(data);
        return E_NOMEM;
    }
    uint32_t adler = s << 16 | a;
    for (int k = 0; k < 4; k++)
        bigger[len + (size_t)k] = (uint8_t)(adler >> (24 - 8 * k));
    *out = bigger;
    *out_len = len + 4;
    return 0;
}
