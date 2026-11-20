/* Block-compressed texture formats (BC1 to BC7): one texel of a 4x4 block at a time, so that sampling decodes only
   what it reads. */

#if defined(__x86_64__) || defined(RS_HOST)

#include "gd_internal.h"
#include "gmath.h"

#ifdef RS_HOST
#include <string.h>
#else
#include <synth/string.h>
#endif

/* ---- BC1 to BC5 */

static void rgb565(uint32_t c, float out[3])
{
    out[0] = (float)(c >> 11) / 31.0f;
    out[1] = (float)(c >> 5 & 63) / 63.0f;
    out[2] = (float)(c & 31) / 31.0f;
}

/* the color half of BC1, BC2 and BC3; four: always the four-color mode */
static void bc1_color(const uint8_t *b, int i, bool four, float out[4])
{
    uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8, c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8;
    uint32_t sel = (uint32_t)(b[4 + i / 4] >> ((i % 4) * 2)) & 3;
    float a[3], z[3];
    rgb565(c0, a);
    rgb565(c1, z);
    out[3] = 1;
    for (int k = 0; k < 3; k++) {
        if (four || c0 > c1) {
            static const float w[4] = {0, 1, 1.0f / 3, 2.0f / 3};
            out[k] = a[k] + (z[k] - a[k]) * w[sel];
        } else {
            static const float w[4] = {0, 1, 0.5f, 0};
            out[k] = sel == 3 ? 0 : a[k] + (z[k] - a[k]) * w[sel];
        }
    }
    if (!four && c0 <= c1 && sel == 3)
        out[3] = 0;
}

/* BC4: eight values between two endpoints, or six and the two ends of the range */
static float bc4(const uint8_t *b, int i, bool sign)
{
    float r0, r1;
    if (sign) {
        int s0 = (int8_t)b[0], s1 = (int8_t)b[1];
        r0 = (float)(s0 < -127 ? -127 : s0) / 127.0f;
        r1 = (float)(s1 < -127 ? -127 : s1) / 127.0f;
    } else {
        r0 = (float)b[0] / 255.0f, r1 = (float)b[1] / 255.0f;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++)
        bits |= (uint64_t)b[2 + k] << (8 * k);
    uint32_t sel = (uint32_t)(bits >> (3 * i)) & 7;
    bool eight = sign ? (int8_t)b[0] > (int8_t)b[1] : b[0] > b[1];
    if (sel == 0)
        return r0;
    if (sel == 1)
        return r1;
    if (eight)
        return (r0 * (float)(8 - sel) + r1 * (float)(sel - 1)) / 7.0f;
    if (sel == 6)
        return sign ? -1.0f : 0.0f;
    if (sel == 7)
        return 1.0f;
    return (r0 * (float)(6 - sel) + r1 * (float)(sel - 1)) / 5.0f;
}

/* ---- BC7 */

struct bits {
    const uint8_t *b;
    int at;
};

static uint32_t take(struct bits *s, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, s->at++)
        v |= (uint32_t)((s->b[s->at >> 3] >> (s->at & 7)) & 1) << i;
    return v;
}

static const uint16_t part2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8,
    0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110,
    0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696,
    0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720,
    0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
};

static const char *const part3[64] = {
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122",
    "0011001100220022", "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222",
    "0000111122222222", "0012001200120012", "0112011201120112", "0122012201220122", "0011011211221222",
    "0011200122002220", "0001001101121122", "0111001120012200", "0000112211221122", "0022002200221111",
    "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210", "0122012200110000",
    "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222",
    "0120012001200120", "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011",
    "0011112222000011", "0101010122222222", "0000000021212121", "0022112200221122", "0022001100220011",
    "0220122102201221", "0101222222220101", "0000212121212121", "0101010101012222", "0222011102220111",
    "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002", "0110011001102222",
    "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220",
};

static const uint8_t anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,
    8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,
    15, 15, 15, 15, 15, 2,  2,  15,
};

static const uint8_t anchor3a[64] = {
    3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3, 3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,
    8, 5,  15, 15, 8, 15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15, 3,  15, 5,  5,  5,  8,  5,  10,
    5, 10, 8,  13, 15, 12, 3,  3,
};

static const uint8_t anchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10,
    15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 3,  15, 15, 8,
};

static const uint8_t weights2[4] = {0, 21, 43, 64};
static const uint8_t weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static uint32_t interp(uint32_t a, uint32_t b, uint32_t sel, int bits)
{
    uint32_t w = bits == 2 ? weights2[sel] : bits == 3 ? weights3[sel] : weights4[sel];
    return ((64 - w) * a + w * b + 32) >> 6;
}

static int subset_of(int subsets, int partition, int i)
{
    if (subsets == 2)
        return (part2[partition] >> i) & 1;
    if (subsets == 3)
        return part3[partition][i] - '0';
    return 0;
}

static bool is_anchor(int subsets, int partition, int i)
{
    if (i == 0)
        return true;
    if (subsets == 2)
        return i == anchor2[partition];
    if (subsets == 3)
        return i == anchor3a[partition] || i == anchor3b[partition];
    return false;
}

/* the index of texel i in an index field starting at bit start with bits per index */
static uint32_t index_of(const uint8_t *b, int start, int bits, int subsets, int partition, int i)
{
    int at = start;
    for (int k = 0; k < i; k++)
        at += bits - (is_anchor(subsets, partition, k) ? 1 : 0);
    struct bits s = {b, at};
    return take(&s, bits - (is_anchor(subsets, partition, i) ? 1 : 0));
}

static void bc7(const uint8_t *b, int i, float out[4])
{
    int mode = 0;
    while (mode < 8 && !((b[0] >> mode) & 1))
        mode++;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (mode == 8)
        return;
    static const struct {
        uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
    } m[8] = {
        {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
        {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
        {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
    };
    struct bits s = {b, mode + 1};
    int ns = m[mode].ns;
    int partition = (int)take(&s, m[mode].pb), rotation = (int)take(&s, m[mode].rb), isel = (int)take(&s, m[mode].isb);
    uint32_t ep[3][2][4]; /* subset, end, rgba */
    int nchan = m[mode].ab ? 4 : 3;
    for (int c = 0; c < nchan; c++)
        for (int e = 0; e < ns * 2; e++)
            ep[e / 2][e % 2][c] = take(&s, c == 3 ? m[mode].ab : m[mode].cb);
    int cbits = m[mode].cb, abits = m[mode].ab;
    if (m[mode].epb || m[mode].spb) {
        uint32_t p[6];
        int np = m[mode].epb ? ns * 2 : ns;
        for (int k = 0; k < np; k++)
            p[k] = take(&s, 1);
        for (int e = 0; e < ns * 2; e++) {
            uint32_t bit = m[mode].epb ? p[e] : p[e / 2];
            for (int c = 0; c < nchan; c++)
                ep[e / 2][e % 2][c] = ep[e / 2][e % 2][c] << 1 | bit;
        }
        cbits++;
        if (abits)
            abits++;
    }
    for (int e = 0; e < ns * 2; e++)
        for (int c = 0; c < 4; c++) {
            int n = c == 3 ? abits : cbits;
            uint32_t *v = &ep[e / 2][e % 2][c];
            if (c == 3 && !abits)
                *v = 255;
            else
                *v = (*v << (8 - n)) | (*v >> (2 * n - 8));
        }
    int sub = subset_of(ns, partition, i);
    int ib = m[mode].ib, ib2 = m[mode].ib2;
    int idx_start = s.at;
    uint32_t i1 = index_of(b, idx_start, ib, ns, partition, i);
    uint32_t rgba[4];
    if (ib2) {
        int idx2_start = idx_start + 16 * ib - ns;
        uint32_t i2 = index_of(b, idx2_start, ib2, 1, 0, i);
        uint32_t ci = isel ? i2 : i1, ai = isel ? i1 : i2;
        int cb = isel ? ib2 : ib, ab = isel ? ib : ib2;
        for (int c = 0; c < 3; c++)
            rgba[c] = interp(ep[sub][0][c], ep[sub][1][c], ci, cb);
        rgba[3] = interp(ep[sub][0][3], ep[sub][1][3], ai, ab);
    } else {
        for (int c = 0; c < 4; c++)
            rgba[c] = interp(ep[sub][0][c], ep[sub][1][c], i1, ib);
    }
    if (rotation) {
        uint32_t t = rgba[3];
        rgba[3] = rgba[rotation - 1];
        rgba[rotation - 1] = t;
    }
    for (int c = 0; c < 4; c++)
        out[c] = (float)rgba[c] / 255.0f;
}

/* ---- BC6H: half-float colors */

static int32_t sext6(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

static int32_t unquantize6(int32_t c, int bits, bool sign)
{
    if (!sign) {
        if (bits >= 15)
            return c;
        if (c == 0)
            return 0;
        if (c == (1 << bits) - 1)
            return 0xffff;
        return ((c << 16) + 0x8000) >> bits;
    }
    if (bits >= 16)
        return c;
    bool neg = c < 0;
    if (neg)
        c = -c;
    int32_t u;
    if (c == 0)
        u = 0;
    else if (c >= (1 << (bits - 1)) - 1)
        u = 0x7fff;
    else
        u = ((c << 15) + 0x4000) >> (bits - 1);
    return neg ? -u : u;
}

static float finish6(int32_t v, bool sign)
{
    uint32_t h;
    if (sign) {
        int32_t s = v < 0 ? (-v * 31) >> 5 : (v * 31) >> 5;
        h = v < 0 ? (0x8000u | (uint32_t)s) : (uint32_t)s;
    } else {
        h = (uint32_t)((v * 31) >> 6);
    }
    return f16_to_f32(h & 0xffff);
}

/* the fourteen BC6H modes: endpoint bits of the base and the deltas, and where each bit of the header comes from */
struct mode6 {
    uint8_t code, regions, transformed, base_bits, delta[3]; /* red, green, blue delta bits */
};

static const struct mode6 modes6[14] = {
    {0x00, 2, 1, 10, {5, 5, 5}},  {0x01, 2, 1, 7, {6, 6, 6}},   {0x02, 2, 1, 11, {5, 4, 4}},
    {0x06, 2, 1, 11, {4, 5, 4}},  {0x0a, 2, 1, 11, {4, 4, 5}},  {0x0e, 2, 1, 9, {5, 5, 5}},
    {0x12, 2, 1, 8, {6, 5, 5}},   {0x16, 2, 1, 8, {5, 6, 5}},   {0x1a, 2, 1, 8, {5, 5, 6}},
    {0x1e, 2, 0, 6, {6, 6, 6}},   {0x03, 1, 0, 10, {10, 10, 10}}, {0x07, 1, 1, 11, {9, 9, 9}},
    {0x0b, 1, 1, 12, {8, 8, 8}},  {0x0f, 1, 1, 16, {4, 4, 4}},
};

/* Where the endpoint bits sit is the hard part of BC6H: each mode scatters them. These tables list, for each mode,
   the 82 (two regions) or 65 (one region) header bits after the mode as (endpoint value, bit): values are
   0 rw, 1 gw, 2 bw, 3 rx, 4 gx, 5 bx, 6 ry, 7 gy, 8 by, 9 rz, 10 gz, 11 bz, 12 partition. */
#define E(v, b) ((v) << 4 | (b))
static const uint8_t layout6[14][82] = {
    /* mode 0 */
    {E(7, 4), E(8, 4), E(11, 4), E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8),
     E(0, 9), E(1, 0), E(1, 1), E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0),
     E(2, 1), E(2, 2), E(2, 3), E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2),
     E(3, 3), E(3, 4), E(10, 4), E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4),
     E(11, 0), E(10, 0), E(10, 1), E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(11, 1),
     E(8, 0), E(8, 1), E(8, 2), E(8, 3), E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(11, 2), E(9, 0), E(9, 1),
     E(9, 2), E(9, 3), E(9, 4), E(11, 3), E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 1 */
    {E(7, 5), E(10, 4), E(10, 5), E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(11, 0), E(11, 1),
     E(8, 4), E(1, 0), E(1, 1), E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(8, 5), E(11, 2), E(7, 4), E(2, 0),
     E(2, 1), E(2, 2), E(2, 3), E(2, 4), E(2, 5), E(2, 6), E(11, 3), E(11, 5), E(11, 4), E(3, 0), E(3, 1), E(3, 2),
     E(3, 3), E(3, 4), E(3, 5), E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4),
     E(4, 5), E(10, 0), E(10, 1), E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(5, 5),
     E(8, 0), E(8, 1), E(8, 2), E(8, 3), E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(6, 5), E(9, 0), E(9, 1),
     E(9, 2), E(9, 3), E(9, 4), E(9, 5), E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 2 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(0, 10),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(1, 10), E(11, 0), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(2, 10), E(11, 1), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(11, 2), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(9, 4), E(11, 3),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 3 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(0, 10), E(10, 4),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(1, 10), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(2, 10), E(11, 1), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(11, 0), E(11, 2), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(7, 4), E(11, 3),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 4 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(0, 10), E(8, 4),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(1, 10), E(11, 0), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(2, 10), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(11, 1), E(11, 2), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(11, 4), E(11, 3),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 5 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(8, 4), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(7, 4), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(11, 4), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(10, 4),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(11, 0), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(11, 1), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(11, 2), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(9, 4), E(11, 3),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 6 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(10, 4), E(8, 4), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(11, 2), E(7, 4), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(11, 3), E(11, 4), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(3, 5),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(11, 0), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(11, 1), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(6, 5), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(9, 4), E(9, 5),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 7 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(11, 0), E(8, 4), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(7, 5), E(7, 4), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(10, 5), E(11, 4), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(10, 4),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(4, 5), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(11, 1), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(11, 2), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(9, 4), E(11, 3),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 8 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(11, 1), E(8, 4), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(8, 5), E(7, 4), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(11, 5), E(11, 4), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(10, 4),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(11, 0), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(5, 5), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(11, 2), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(9, 4), E(11, 3),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 9 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(10, 4), E(11, 0), E(11, 1), E(8, 4), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(7, 5), E(8, 5), E(11, 2), E(7, 4), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(10, 5), E(11, 3), E(11, 5), E(11, 4), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(3, 5),
     E(7, 0), E(7, 1), E(7, 2), E(7, 3), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(4, 5), E(10, 0), E(10, 1),
     E(10, 2), E(10, 3), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(5, 5), E(8, 0), E(8, 1), E(8, 2), E(8, 3),
     E(6, 0), E(6, 1), E(6, 2), E(6, 3), E(6, 4), E(6, 5), E(9, 0), E(9, 1), E(9, 2), E(9, 3), E(9, 4), E(9, 5),
     E(12, 0), E(12, 1), E(12, 2), E(12, 3), E(12, 4)},
    /* mode 10 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(3, 5),
     E(3, 6), E(3, 7), E(3, 8), E(3, 9), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(4, 5), E(4, 6), E(4, 7),
     E(4, 8), E(4, 9), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(5, 5), E(5, 6), E(5, 7), E(5, 8), E(5, 9)},
    /* mode 11 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(3, 5),
     E(3, 6), E(3, 7), E(3, 8), E(0, 10), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(4, 5), E(4, 6), E(4, 7),
     E(4, 8), E(1, 10), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(5, 5), E(5, 6), E(5, 7), E(5, 8), E(2, 10)},
    /* mode 12 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(3, 4), E(3, 5),
     E(3, 6), E(3, 7), E(0, 11), E(0, 10), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(4, 4), E(4, 5), E(4, 6), E(4, 7),
     E(1, 11), E(1, 10), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(5, 4), E(5, 5), E(5, 6), E(5, 7), E(2, 11), E(2, 10)},
    /* mode 13 */
    {E(0, 0), E(0, 1), E(0, 2), E(0, 3), E(0, 4), E(0, 5), E(0, 6), E(0, 7), E(0, 8), E(0, 9), E(1, 0), E(1, 1),
     E(1, 2), E(1, 3), E(1, 4), E(1, 5), E(1, 6), E(1, 7), E(1, 8), E(1, 9), E(2, 0), E(2, 1), E(2, 2), E(2, 3),
     E(2, 4), E(2, 5), E(2, 6), E(2, 7), E(2, 8), E(2, 9), E(3, 0), E(3, 1), E(3, 2), E(3, 3), E(0, 15), E(0, 14),
     E(0, 13), E(0, 12), E(0, 11), E(0, 10), E(4, 0), E(4, 1), E(4, 2), E(4, 3), E(1, 15), E(1, 14), E(1, 13),
     E(1, 12), E(1, 11), E(1, 10), E(5, 0), E(5, 1), E(5, 2), E(5, 3), E(2, 15), E(2, 14), E(2, 13), E(2, 12),
     E(2, 11), E(2, 10)},
};
#undef E

static void bc6h(const uint8_t *b, int i, bool sign, float out[4])
{
    out[0] = out[1] = out[2] = 0, out[3] = 1;
    uint32_t code = b[0] & 3;
    if (code >= 2)
        code = b[0] & 31;
    int mode = -1;
    for (int k = 0; k < 14; k++)
        if (modes6[k].code == code)
            mode = k;
    if (mode < 0)
        return;
    const struct mode6 *m = &modes6[mode];
    struct bits s = {b, code < 2 ? 2 : 5};
    uint32_t v[13] = {0};
    int header = m->regions == 2 ? (code < 2 ? 80 : 77) : 60;
    for (int k = 0; k < header; k++) {
        uint8_t e = layout6[mode][k];
        v[e >> 4] |= take(&s, 1) << (e & 15);
    }
    int partition = (int)v[12];
    /* endpoints: w x (region 0), y z (region 1), each r g b */
    int32_t ep[2][2][3];
    int bb = m->base_bits;
    for (int c = 0; c < 3; c++) {
        int32_t base = sign ? sext6(v[c], bb) : (int32_t)v[c];
        int db = m->delta[c];
        uint32_t raw[3] = {v[3 + c], v[6 + c], v[9 + c]};
        ep[0][0][c] = base;
        for (int e = 0; e < (m->regions == 2 ? 3 : 1); e++) {
            int32_t d = m->transformed ? sext6(raw[e], db) : (sign ? sext6(raw[e], db) : (int32_t)raw[e]);
            int32_t val = m->transformed ? (base + d) & ((1 << bb) - 1) : d;
            if (m->transformed && sign)
                val = sext6((uint32_t)val, bb);
            if (e == 0)
                ep[0][1][c] = val;
            else
                ep[1][e - 1][c] = val;
        }
    }
    int ib = m->regions == 2 ? 3 : 4;
    int sub = m->regions == 2 ? subset_of(2, partition, i) : 0;
    uint32_t sel = index_of(b, header + (code < 2 ? 2 : 5), ib, m->regions, partition, i);
    for (int c = 0; c < 3; c++) {
        int32_t a = unquantize6(ep[sub][0][c], bb, sign), z = unquantize6(ep[sub][1][c], bb, sign);
        uint32_t w = ib == 3 ? weights3[sel] : weights4[sel];
        int32_t val = (a * (int32_t)(64 - w) + z * (int32_t)w + 32) >> 6;
        out[c] = finish6(val, sign);
    }
}

void gd_bc_texel(uint32_t f, const uint8_t *b, int x, int y, float out[4])
{
    int i = y * 4 + x;
    out[0] = out[1] = out[2] = 0, out[3] = 1;
    switch (f) {
    case GD_BC1_UNORM: case GD_BC1_UNORM_SRGB:
        bc1_color(b, i, false, out);
        break;
    case GD_BC2_UNORM: case GD_BC2_UNORM_SRGB:
        bc1_color(b + 8, i, true, out);
        out[3] = (float)((b[i / 2] >> ((i & 1) * 4)) & 15) / 15.0f;
        break;
    case GD_BC3_UNORM: case GD_BC3_UNORM_SRGB:
        bc1_color(b + 8, i, true, out);
        out[3] = bc4(b, i, false);
        break;
    case GD_BC4_UNORM: out[0] = bc4(b, i, false); break;
    case GD_BC4_SNORM: out[0] = bc4(b, i, true); break;
    case GD_BC5_UNORM: out[0] = bc4(b, i, false), out[1] = bc4(b + 8, i, false); break;
    case GD_BC5_SNORM: out[0] = bc4(b, i, true), out[1] = bc4(b + 8, i, true); break;
    case GD_BC6H_UF16: bc6h(b, i, false, out); break;
    case GD_BC6H_SF16: bc6h(b, i, true, out); break;
    case GD_BC7_UNORM: case GD_BC7_UNORM_SRGB: bc7(b, i, out); break;
    default: break;
    }
}

#endif
