#include <synth/error.h>
#include <synth/heap.h>
#include <synth/lzma.h>
#include <synth/string.h>

/* LZMA decoder (the algorithm of 7-Zip's LzmaSpec): a range coder drives literal/match decisions; the output buffer
   is the dictionary, so the unpacked size has to be known. */

#define PROB_INIT 1024
#define STATES 12
#define POS_STATES_MAX 16
#define END_MARKER 0xffffffffu

struct range {
    const uint8_t *in, *end;
    uint32_t range, code;
    bool bad;
};

static uint8_t next_byte(struct range *rc)
{
    if (rc->in < rc->end)
        return *rc->in++;
    rc->bad = true;
    return 0;
}

static void normalize(struct range *rc)
{
    if (rc->range < (1u << 24)) {
        rc->range <<= 8;
        rc->code = rc->code << 8 | next_byte(rc);
    }
}

static unsigned bit(struct range *rc, uint16_t *p)
{
    uint32_t bound = (rc->range >> 11) * *p;
    unsigned b;
    if (rc->code < bound) {
        rc->range = bound;
        *p = (uint16_t)(*p + ((2048 - *p) >> 5));
        b = 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *p = (uint16_t)(*p - (*p >> 5));
        b = 1;
    }
    normalize(rc);
    return b;
}

static uint32_t direct(struct range *rc, int count)
{
    uint32_t res = 0;
    while (count--) {
        rc->range >>= 1;
        rc->code -= rc->range;
        uint32_t t = 0u - (rc->code >> 31);
        rc->code += rc->range & t;
        if (rc->code == rc->range)
            rc->bad = true;
        normalize(rc);
        res = (res << 1) + (t + 1);
    }
    return res;
}

static unsigned tree(struct range *rc, uint16_t *probs, int bits)
{
    unsigned m = 1;
    for (int i = 0; i < bits; i++)
        m = (m << 1) + bit(rc, &probs[m]);
    return m - (1u << bits);
}

static unsigned tree_reverse(struct range *rc, uint16_t *probs, int bits)
{
    unsigned m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        unsigned b = bit(rc, &probs[m]);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

struct len_coder {
    uint16_t choice, choice2;
    uint16_t low[POS_STATES_MAX][8], mid[POS_STATES_MAX][8], high[256];
};

static unsigned decode_len(struct range *rc, struct len_coder *l, unsigned pos_state)
{
    if (!bit(rc, &l->choice))
        return tree(rc, l->low[pos_state], 3);
    if (!bit(rc, &l->choice2))
        return 8 + tree(rc, l->mid[pos_state], 3);
    return 16 + tree(rc, l->high, 8);
}

struct model {
    uint16_t is_match[STATES][POS_STATES_MAX], is_rep[STATES], is_rep_g0[STATES], is_rep_g1[STATES],
        is_rep_g2[STATES], is_rep0_long[STATES][POS_STATES_MAX];
    uint16_t pos_slot[4][64], pos_special[115], align[16];
    struct len_coder len, rep_len;
};

static void init_probs(uint16_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = PROB_INIT;
}

int lzma_decode(const uint8_t props[5], const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
    unsigned d = props[0];
    if (d >= 9 * 5 * 5)
        return E_INVAL;
    unsigned lc = d % 9, lp = (d / 9) % 5, pb = d / 45;
    size_t lit_count = (size_t)0x300 << (lc + lp);
    uint16_t *lit = kmalloc(lit_count * sizeof(uint16_t));
    struct model *m = kmalloc(sizeof(*m));
    if (!lit || !m) {
        kfree(lit);
        kfree(m);
        return E_NOMEM;
    }
    init_probs(lit, lit_count);
    init_probs((uint16_t *)m, sizeof(*m) / sizeof(uint16_t));
    struct range rc = {in, in + in_size, 0xffffffffu, 0, false};
    if (next_byte(&rc) != 0)
        rc.bad = true;
    for (int i = 0; i < 4; i++)
        rc.code = rc.code << 8 | next_byte(&rc);
    size_t pos = 0;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    unsigned state = 0;
    unsigned pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;
    while (pos < out_size && !rc.bad) {
        unsigned pos_state = (unsigned)pos & pb_mask;
        if (!bit(&rc, &m->is_match[state][pos_state])) {
            unsigned prev = pos ? out[pos - 1] : 0;
            uint16_t *probs = &lit[0x300 * ((((unsigned)pos & lp_mask) << lc) + (prev >> (8 - lc)))];
            unsigned sym = 1;
            if (state >= 7 && pos > rep0) {
                unsigned match = out[pos - rep0 - 1];
                do {
                    unsigned mb = (match >> 7) & 1;
                    match <<= 1;
                    unsigned b = bit(&rc, &probs[((1 + mb) << 8) + sym]);
                    sym = (sym << 1) | b;
                    if (mb != b)
                        break;
                } while (sym < 0x100);
            }
            while (sym < 0x100)
                sym = (sym << 1) | bit(&rc, &probs[sym]);
            out[pos++] = (uint8_t)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        unsigned len;
        if (bit(&rc, &m->is_rep[state])) {
            if (!pos) {
                rc.bad = true;
                break;
            }
            if (!bit(&rc, &m->is_rep_g0[state])) {
                if (!bit(&rc, &m->is_rep0_long[state][pos_state])) {
                    state = state < 7 ? 9 : 11;
                    out[pos] = out[pos - rep0 - 1];
                    pos++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!bit(&rc, &m->is_rep_g1[state])) {
                    dist = rep1;
                } else {
                    if (!bit(&rc, &m->is_rep_g2[state])) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = decode_len(&rc, &m->rep_len, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = decode_len(&rc, &m->len, pos_state);
            state = state < 7 ? 7 : 10;
            unsigned slot = tree(&rc, m->pos_slot[MIN(len, 3u)], 6);
            uint32_t dist = slot;
            if (slot >= 4) {
                int bits = (int)(slot >> 1) - 1;
                dist = (2 | (slot & 1)) << bits;
                if (slot < 14)
                    dist += tree_reverse(&rc, &m->pos_special[dist - slot], bits);
                else
                    dist += (direct(&rc, bits - 4) << 4) + tree_reverse(&rc, m->align, 4);
            }
            if (dist == END_MARKER)
                break;
            rep0 = dist;
        }
        len += 2;
        if (rep0 >= pos) {
            rc.bad = true;
            break;
        }
        for (; len && pos < out_size; len--, pos++)
            out[pos] = out[pos - rep0 - 1];
    }
    kfree(lit);
    kfree(m);
    return rc.bad || pos < out_size ? E_INVAL : 0;
}
