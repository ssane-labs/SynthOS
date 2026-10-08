#include <synth/error.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/string.h>

#define FAST_BITS 9

struct huff {
    uint16_t count[16];
    uint16_t symbol[320];
    uint16_t fast[1 << FAST_BITS];
};

struct state {
    const uint8_t *in;
    size_t in_len, pos;
    uint32_t bits;
    int nbits;
    uint8_t *out;
    size_t out_len, out_cap, limit;
    bool grow;
};

static int need(struct state *s, int n)
{
    while (s->nbits < n) {
        if (s->pos >= s->in_len)
            return E_INVAL;
        s->bits |= (uint32_t)s->in[s->pos++] << s->nbits;
        s->nbits += 8;
    }
    return 0;
}

static int getbits(struct state *s, int n, uint32_t *v)
{
    if (!n) {
        *v = 0;
        return 0;
    }
    if (need(s, n))
        return E_INVAL;
    *v = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->nbits -= n;
    return 0;
}

static int put(struct state *s, uint8_t c)
{
    if (s->out_len == s->out_cap) {
        if (!s->grow)
            return E_NOSPC;
        size_t cap = s->out_cap ? s->out_cap * 2 : 65536;
        if (s->limit && cap > s->limit)
            cap = s->limit;
        if (cap <= s->out_len)
            return E_NOSPC;
        uint8_t *g = krealloc(s->out, cap);
        if (!g)
            return E_NOMEM;
        s->out = g;
        s->out_cap = cap;
    }
    s->out[s->out_len++] = c;
    return 0;
}

static void build(struct huff *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    memset(h->fast, 0, sizeof(h->fast));
    uint16_t next[16];
    uint16_t code = 0;
    for (int len = 1; len < 16; len++) {
        code = (uint16_t)((code + h->count[len - 1]) << 1);
        next[len] = code;
    }
    for (int i = 0; i < n; i++) {
        int len = lengths[i];
        if (!len)
            continue;
        uint16_t c = next[len]++;
        if (len > FAST_BITS)
            continue;
        uint16_t r = 0;
        for (int b = 0; b < len; b++)
            r = (uint16_t)(r | ((c >> b) & 1) << (len - 1 - b));
        for (int k = r; k < (1 << FAST_BITS); k += 1 << len)
            h->fast[k] = (uint16_t)(0x8000 | len << 9 | i);
    }
}

static int decode(struct state *s, const struct huff *h)
{
    if (need(s, FAST_BITS) == 0) {
        uint16_t e = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
        if (e) {
            int len = (e >> 9) & 15;
            s->bits >>= len;
            s->nbits -= len;
            return e & 511;
        }
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        uint32_t b;
        if (getbits(s, 1, &b))
            return -1;
        code |= (int)b;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct state *s, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0)
            return E_INVAL;
        if (sym < 256) {
            int err = put(s, (uint8_t)sym);
            if (err)
                return err;
            continue;
        }
        if (sym == 256)
            return 0;
        sym -= 257;
        if (sym >= 29)
            return E_INVAL;
        uint32_t extra;
        if (getbits(s, len_extra[sym], &extra))
            return E_INVAL;
        size_t len = len_base[sym] + extra;
        int d = decode(s, dist);
        if (d < 0 || d >= 30 || getbits(s, dist_extra[d], &extra))
            return E_INVAL;
        size_t back = dist_base[d] + extra;
        if (back > s->out_len)
            return E_INVAL;
        for (size_t i = 0; i < len; i++) {
            int err = put(s, s->out[s->out_len - back]);
            if (err)
                return err;
        }
    }
}

static int fixed_block(struct state *s)
{
    static struct huff lit, dist;
    static bool ready;
    if (!ready) {
        uint8_t l[288];
        for (int i = 0; i < 288; i++)
            l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        build(&lit, l, 288);
        for (int i = 0; i < 30; i++)
            l[i] = 5;
        build(&dist, l, 30);
        ready = true;
    }
    return codes(s, &lit, &dist);
}

static int dynamic_block(struct state *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint32_t hlit, hdist, hclen;
    if (getbits(s, 5, &hlit) || getbits(s, 5, &hdist) || getbits(s, 4, &hclen))
        return E_INVAL;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30)
        return E_INVAL;
    uint8_t lengths[320];
    memset(lengths, 0, sizeof(lengths));
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v;
        if (getbits(s, 3, &v))
            return E_INVAL;
        lengths[order[i]] = (uint8_t)v;
    }
    struct huff lencode, lit, dist;
    build(&lencode, lengths, 19);
    uint32_t i = 0;
    while (i < hlit + hdist) {
        int sym = decode(s, &lencode);
        if (sym < 0)
            return E_INVAL;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        uint32_t rep, v;
        uint8_t value = 0;
        if (sym == 16) {
            if (!i || getbits(s, 2, &v))
                return E_INVAL;
            value = lengths[i - 1];
            rep = 3 + v;
        } else if (sym == 17) {
            if (getbits(s, 3, &v))
                return E_INVAL;
            rep = 3 + v;
        } else {
            if (getbits(s, 7, &v))
                return E_INVAL;
            rep = 11 + v;
        }
        if (i + rep > hlit + hdist)
            return E_INVAL;
        while (rep--)
            lengths[i++] = value;
    }
    build(&lit, lengths, (int)hlit);
    build(&dist, lengths + hlit, (int)hdist);
    return codes(s, &lit, &dist);
}

static int run(struct state *s)
{
    uint32_t last;
    do {
        uint32_t type;
        if (getbits(s, 1, &last) || getbits(s, 2, &type))
            return E_INVAL;
        int err;
        if (type == 0) {
            s->bits = 0;
            s->nbits = 0;
            if (s->pos + 4 > s->in_len)
                return E_INVAL;
            uint32_t len = s->in[s->pos] | (uint32_t)s->in[s->pos + 1] << 8;
            uint32_t nlen = s->in[s->pos + 2] | (uint32_t)s->in[s->pos + 3] << 8;
            s->pos += 4;
            if ((len ^ 0xffff) != nlen || s->pos + len > s->in_len)
                return E_INVAL;
            err = 0;
            for (uint32_t i = 0; i < len && !err; i++)
                err = put(s, s->in[s->pos + i]);
            s->pos += len;
        } else if (type == 1) {
            err = fixed_block(s);
        } else if (type == 2) {
            err = dynamic_block(s);
        } else {
            err = E_INVAL;
        }
        if (err)
            return err;
    } while (!last);
    return 0;
}

int inflate_into(const void *in, size_t in_len, void *out, size_t out_cap, size_t *out_len, size_t *used)
{
    struct state s = {.in = in, .in_len = in_len, .out = out, .out_cap = out_cap};
    int err = run(&s);
    *out_len = s.out_len;
    if (used)
        *used = s.pos;
    return err;
}

int inflate_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len, size_t *used)
{
    struct state s = {.in = in, .in_len = in_len, .grow = true, .limit = limit};
    int err = run(&s);
    if (err) {
        kfree(s.out);
        return err;
    }
    *out = s.out ? s.out : kzalloc(1);
    *out_len = s.out_len;
    if (used)
        *used = s.pos;
    return *out ? 0 : E_NOMEM;
}

static size_t gzip_header(const uint8_t *p, size_t n)
{
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8)
        return 0;
    uint8_t flags = p[3];
    size_t i = 10;
    if (flags & 4) {
        if (i + 2 > n)
            return 0;
        i += 2 + (p[i] | (size_t)p[i + 1] << 8);
    }
    if (flags & 8)
        while (i < n && p[i++])
            ;
    if (flags & 16)
        while (i < n && p[i++])
            ;
    if (flags & 2)
        i += 2;
    return i < n ? i : 0;
}

int gunzip_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len)
{
    size_t start = gzip_header(in, in_len);
    if (!start)
        return E_INVAL;
    return inflate_alloc((const uint8_t *)in + start, in_len - start, limit, out, out_len, NULL);
}

int zlib_inflate_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len)
{
    const uint8_t *p = in;
    if (in_len < 2 || (p[0] & 0x0f) != 8 || ((p[0] << 8) | p[1]) % 31)
        return E_INVAL;
    return inflate_alloc(p + 2, in_len - 2, limit, out, out_len, NULL);
}
