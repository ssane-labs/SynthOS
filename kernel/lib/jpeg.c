#include <synth/error.h>
#include <synth/heap.h>
#include <synth/picture.h>
#include <synth/string.h>

/* Baseline and progressive JPEG with Huffman coding; any chroma subsampling. */

static const uint8_t dezigzag[64 + 16] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,
    6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31,
    39, 46, 53, 60, 61, 54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

struct huff {
    uint8_t fast[512];
    uint8_t size[257];
    uint16_t code[256];
    uint8_t values[256];
    int maxcode[18];
    int delta[17];
    bool present;
};

struct comp {
    int id, h, v, tq;
    int bw, bh;
    int16_t *coef;
    uint8_t *plane;
    int dc_pred;
    int hd, ha;
};

struct jpeg {
    const uint8_t *data, *end, *pos;
    uint32_t bits;
    int nbits;
    bool marker;
    struct huff huff[8];
    uint16_t quant[4][64];
    struct comp comp[4];
    int ncomp, w, h, hmax, vmax, mcus_x, mcus_y;
    bool progressive;
    int restart, eob_run;
    bool adobe, no_memory;
    int transform;
};

static bool build_huff(struct huff *t, const uint8_t *counts)
{
    int k = 0, code = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < counts[i]; j++) {
            if (k >= 256)
                return false;
            t->size[k++] = (uint8_t)(i + 1);
        }
    t->size[k] = 0;
    int j = 0;
    for (int len = 1; len <= 16; len++) {
        t->delta[len] = j - code;
        if (t->size[j] == len) {
            while (t->size[j] == len)
                t->code[j++] = (uint16_t)code++;
            if (code - 1 >= 1 << len)
                return false;
        }
        t->maxcode[len] = code << (16 - len);
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff;
    memset(t->fast, 255, sizeof(t->fast));
    for (int i = 0; i < k; i++) {
        int s = t->size[i];
        if (s <= 9) {
            int c = t->code[i] << (9 - s), m = 1 << (9 - s);
            for (int n = 0; n < m; n++)
                t->fast[c + n] = (uint8_t)i;
        }
    }
    t->present = true;
    return true;
}

static void fill(struct jpeg *j)
{
    while (j->nbits <= 24) {
        uint32_t b = 0;
        if (!j->marker && j->pos < j->end) {
            b = *j->pos;
            if (b == 0xff) {
                uint8_t next = j->pos + 1 < j->end ? j->pos[1] : 0xd9;
                if (next) {
                    j->marker = true;
                    b = 0;
                } else {
                    j->pos += 2;
                }
            } else {
                j->pos++;
            }
        }
        j->bits |= b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int get_bits(struct jpeg *j, int n)
{
    if (n <= 0 || n > 16)
        return 0;
    if (j->nbits < n)
        fill(j);
    int v = (int)(j->bits >> (32 - n));
    j->bits <<= n;
    j->nbits -= n;
    return v;
}

static int get_bit(struct jpeg *j)
{
    return get_bits(j, 1);
}

static int extend(struct jpeg *j, int n)
{
    if (n <= 0 || n > 16)
        return 0;
    int v = get_bits(j, n);
    return v < 1 << (n - 1) ? v - (1 << n) + 1 : v;
}

static int decode(struct jpeg *j, const struct huff *t)
{
    if (j->nbits < 16)
        fill(j);
    int f = t->fast[j->bits >> 23];
    if (f < 255) {
        int s = t->size[f];
        j->bits <<= s;
        j->nbits -= s;
        return t->values[f];
    }
    uint32_t top = j->bits >> 16;
    int len;
    for (len = 10; len <= 16; len++)
        if ((int)top < t->maxcode[len])
            break;
    if (len > 16)
        return -1;
    int idx = (int)(top >> (16 - len)) + t->delta[len];
    if (idx < 0 || idx > 255 || t->size[idx] != len)
        return -1;
    j->bits <<= len;
    j->nbits -= len;
    return t->values[idx];
}

static bool block_baseline(struct jpeg *j, struct comp *c, int16_t *d)
{
    int t = decode(j, &j->huff[c->hd]);
    if (t < 0)
        return false;
    c->dc_pred += extend(j, t);
    d[0] = (int16_t)c->dc_pred;
    for (int k = 1; k < 64;) {
        int rs = decode(j, &j->huff[4 + c->ha]);
        if (rs < 0)
            return false;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (rs != 0xf0)
                break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return false;
        d[dezigzag[k++]] = (int16_t)extend(j, s);
    }
    return true;
}

static bool block_dc(struct jpeg *j, struct comp *c, int16_t *d, int ah, int al)
{
    if (!ah) {
        int t = decode(j, &j->huff[c->hd]);
        if (t < 0)
            return false;
        c->dc_pred += extend(j, t);
        d[0] = (int16_t)(c->dc_pred * (1 << MIN(al, 13)));
    } else if (get_bit(j)) {
        d[0] = (int16_t)(d[0] | (1 << MIN(al, 13)));
    }
    return true;
}

static bool block_ac(struct jpeg *j, struct comp *c, int16_t *d, int ss, int se, int ah, int al)
{
    const struct huff *t = &j->huff[4 + c->ha];
    if (!ah) {
        if (j->eob_run) {
            j->eob_run--;
            return true;
        }
        for (int k = ss; k <= se;) {
            int rs = decode(j, t);
            if (rs < 0)
                return false;
            int s = rs & 15, r = rs >> 4;
            if (!s) {
                if (r < 15) {
                    j->eob_run = (1 << r) - 1 + (r ? get_bits(j, r) : 0);
                    break;
                }
                k += 16;
            } else {
                k += r;
                if (k > 63)
                    return false;
                d[dezigzag[k++]] = (int16_t)(extend(j, s) * (1 << MIN(al, 13)));
            }
        }
        return true;
    }
    int bit = 1 << MIN(al, 13);
    if (j->eob_run) {
        j->eob_run--;
        for (int k = ss; k <= se; k++) {
            int16_t *p = &d[dezigzag[k]];
            if (*p && get_bit(j) && !(*p & bit))
                *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
        }
        return true;
    }
    int k = ss;
    do {
        int rs = decode(j, t);
        if (rs < 0)
            return false;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (r < 15) {
                j->eob_run = (1 << r) - 1 + (r ? get_bits(j, r) : 0);
                r = 64;
            }
        } else {
            s = get_bit(j) ? bit : -bit;
        }
        while (k <= se) {
            int16_t *p = &d[dezigzag[k++]];
            if (*p) {
                if (get_bit(j) && !(*p & bit))
                    *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (!r) {
                    *p = (int16_t)s;
                    break;
                }
                r--;
            }
        }
    } while (k <= se);
    return true;
}

static void reset(struct jpeg *j)
{
    j->bits = 0;
    j->nbits = 0;
    j->eob_run = 0;
    for (int i = 0; i < j->ncomp; i++)
        j->comp[i].dc_pred = 0;
    while (j->pos + 1 < j->end && !(j->pos[0] == 0xff && j->pos[1] >= 0xd0 && j->pos[1] <= 0xd7)) {
        if (j->pos[0] == 0xff && j->pos[1] && j->pos[1] != 0xff)
            break;
        j->pos++;
    }
    if (j->pos + 1 < j->end && j->pos[0] == 0xff && j->pos[1] >= 0xd0 && j->pos[1] <= 0xd7)
        j->pos += 2;
    j->marker = false;
}

static bool scan(struct jpeg *j, struct comp **list, int n, int ss, int se, int ah, int al)
{
    j->bits = 0;
    j->nbits = 0;
    j->marker = false;
    j->eob_run = 0;
    for (int i = 0; i < j->ncomp; i++)
        j->comp[i].dc_pred = 0;
    int todo = j->restart ? j->restart : 0x7fffffff;
    if (n == 1) {
        struct comp *c = list[0];
        int bw = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8, bh = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int16_t *d = c->coef + ((size_t)y * (size_t)c->bw + (size_t)x) * 64;
                bool ok = !j->progressive ? block_baseline(j, c, d)
                          : ss == 0       ? block_dc(j, c, d, ah, al)
                                          : block_ac(j, c, d, ss, se, ah, al);
                if (!ok)
                    return false;
                if (--todo <= 0) {
                    reset(j);
                    todo = j->restart;
                }
            }
        return true;
    }
    for (int my = 0; my < j->mcus_y; my++)
        for (int mx = 0; mx < j->mcus_x; mx++) {
            for (int i = 0; i < n; i++) {
                struct comp *c = list[i];
                for (int by = 0; by < c->v; by++)
                    for (int bx = 0; bx < c->h; bx++) {
                        int x = mx * c->h + bx, y = my * c->v + by;
                        int16_t *d = c->coef + ((size_t)y * (size_t)c->bw + (size_t)x) * 64;
                        bool ok = !j->progressive ? block_baseline(j, c, d) : block_dc(j, c, d, ah, al);
                        if (!ok)
                            return false;
                    }
            }
            if (--todo <= 0) {
                reset(j);
                todo = j->restart;
            }
        }
    return true;
}

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                                 \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                                       \
    p2 = s2;                                                                                       \
    p3 = s6;                                                                                       \
    p1 = (p2 + p3) * 2217;                                                                         \
    t2 = p1 + p3 * -7567;                                                                          \
    t3 = p1 + p2 * 3135;                                                                           \
    p2 = s0;                                                                                       \
    p3 = s4;                                                                                       \
    t0 = (p2 + p3) * 4096;                                                                         \
    t1 = (p2 - p3) * 4096;                                                                         \
    x0 = t0 + t3;                                                                                  \
    x3 = t0 - t3;                                                                                  \
    x1 = t1 + t2;                                                                                  \
    x2 = t1 - t2;                                                                                  \
    t0 = s7;                                                                                       \
    t1 = s5;                                                                                       \
    t2 = s3;                                                                                       \
    t3 = s1;                                                                                       \
    p3 = t0 + t2;                                                                                  \
    p4 = t1 + t3;                                                                                  \
    p1 = t0 + t3;                                                                                  \
    p2 = t1 + t2;                                                                                  \
    p5 = (p3 + p4) * 4816;                                                                         \
    t0 = t0 * 1223;                                                                                \
    t1 = t1 * 8410;                                                                                \
    t2 = t2 * 12586;                                                                               \
    t3 = t3 * 6149;                                                                                \
    p1 = p5 + p1 * -3685;                                                                          \
    p2 = p5 + p2 * -10497;                                                                         \
    p3 = p3 * -8034;                                                                               \
    p4 = p4 * -1597;                                                                               \
    t3 += p1 + p4;                                                                                 \
    t2 += p2 + p3;                                                                                 \
    t1 += p2 + p4;                                                                                 \
    t0 += p1 + p3;

static inline uint8_t clamp(int x)
{
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

static void idct(const int16_t *in, const uint16_t *q, uint8_t *out, int stride)
{
    int d[64], v[64];
    for (int i = 0; i < 64; i++)
        d[i] = CLAMP(in[i] * q[i], -4096, 4096);
    for (int i = 0; i < 8; i++) {
        const int *s = d + i;
        int *o = v + i;
        if (!s[8] && !s[16] && !s[24] && !s[32] && !s[40] && !s[48] && !s[56]) {
            int dc = s[0] * 4;
            for (int k = 0; k < 8; k++)
                o[k * 8] = dc;
            continue;
        }
        IDCT_1D(s[0], s[8], s[16], s[24], s[32], s[40], s[48], s[56])
        x0 += 512;
        x1 += 512;
        x2 += 512;
        x3 += 512;
        o[0] = (x0 + t3) >> 10;
        o[56] = (x0 - t3) >> 10;
        o[8] = (x1 + t2) >> 10;
        o[48] = (x1 - t2) >> 10;
        o[16] = (x2 + t1) >> 10;
        o[40] = (x2 - t1) >> 10;
        o[24] = (x3 + t0) >> 10;
        o[32] = (x3 - t0) >> 10;
    }
    for (int i = 0; i < 8; i++) {
        const int *s = v + i * 8;
        uint8_t *o = out + i * stride;
        IDCT_1D(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7])
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = clamp((x0 + t3) >> 17);
        o[7] = clamp((x0 - t3) >> 17);
        o[1] = clamp((x1 + t2) >> 17);
        o[6] = clamp((x1 - t2) >> 17);
        o[2] = clamp((x2 + t1) >> 17);
        o[5] = clamp((x2 - t1) >> 17);
        o[3] = clamp((x3 + t0) >> 17);
        o[4] = clamp((x3 - t0) >> 17);
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static bool frame_header(struct jpeg *j, const uint8_t *p, int len)
{
    if (len < 6 || p[0] != 8)
        return false;
    j->h = be16(p + 1);
    j->w = be16(p + 3);
    j->ncomp = p[5];
    if (j->w <= 0 || j->h <= 0 || (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) || len < 6 + j->ncomp * 3 ||
        (uint64_t)j->w * (uint64_t)j->h > PICTURE_MAX_PIXELS)
        return false;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->id = p[6 + i * 3];
        c->h = p[7 + i * 3] >> 4;
        c->v = p[7 + i * 3] & 15;
        c->tq = p[8 + i * 3] & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return false;
        j->hmax = MAX(j->hmax, c->h);
        j->vmax = MAX(j->vmax, c->v);
    }
    j->mcus_x = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcus_y = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->bw = j->mcus_x * c->h;
        c->bh = j->mcus_y * c->v;
        c->coef = kzalloc((size_t)c->bw * (size_t)c->bh * 64 * 2);
        if (!c->coef) {
            j->no_memory = true;
            return false;
        }
    }
    return true;
}

static const uint8_t *next_marker(const uint8_t *p, const uint8_t *end)
{
    while (p + 1 < end && !(p[0] == 0xff && p[1] != 0 && p[1] != 0xff && (p[1] < 0xd0 || p[1] > 0xd7)))
        p++;
    return p;
}

static void release(struct jpeg *j)
{
    for (int i = 0; i < 4; i++) {
        kfree(j->comp[i].coef);
        kfree(j->comp[i].plane);
    }
}

static int finish(struct jpeg *j, struct picture *out)
{
    for (int i = 0; i < j->ncomp; i++) {
        struct comp *c = &j->comp[i];
        c->plane = kmalloc((size_t)c->bw * (size_t)c->bh * 64);
        if (!c->plane)
            return E_NOMEM;
        int stride = c->bw * 8;
        for (int y = 0; y < c->bh; y++)
            for (int x = 0; x < c->bw; x++)
                idct(c->coef + ((size_t)y * (size_t)c->bw + (size_t)x) * 64, j->quant[c->tq],
                     c->plane + (size_t)y * 8 * (size_t)stride + (size_t)x * 8, stride);
    }
    out->px = kmalloc((size_t)j->w * (size_t)j->h * 4);
    if (!out->px)
        return E_NOMEM;
    out->w = j->w;
    out->h = j->h;
    out->frames = 1;
    for (int y = 0; y < j->h; y++) {
        uint32_t *row = out->px + (size_t)y * (size_t)j->w;
        for (int x = 0; x < j->w; x++) {
            int s[4];
            for (int i = 0; i < j->ncomp; i++) {
                struct comp *c = &j->comp[i];
                size_t stride = (size_t)c->bw * 8;
                if (c->h == j->hmax && c->v == j->vmax) {
                    s[i] = c->plane[(size_t)y * stride + (size_t)x];
                    continue;
                }
                int fx = MAX(0, (2 * x + 1) * c->h * 8 / j->hmax - 8), fy = MAX(0, (2 * y + 1) * c->v * 8 / j->vmax - 8);
                int x0 = fx >> 4, y0 = fy >> 4, tx = fx & 15, ty = fy & 15;
                int x1 = MIN(x0 + 1, (j->w * c->h + j->hmax - 1) / j->hmax - 1);
                int y1 = MIN(y0 + 1, (j->h * c->v + j->vmax - 1) / j->vmax - 1);
                x1 = MAX(x1, x0);
                y1 = MAX(y1, y0);
                const uint8_t *r0 = c->plane + (size_t)y0 * stride, *r1 = c->plane + (size_t)y1 * stride;
                int top = r0[x0] * (16 - tx) + r0[x1] * tx, bottom = r1[x0] * (16 - tx) + r1[x1] * tx;
                s[i] = (top * (16 - ty) + bottom * ty + 128) >> 8;
            }
            int r, g, b;
            if (j->ncomp == 1) {
                r = g = b = s[0];
            } else if (j->ncomp == 3 && !(j->adobe && j->transform == 0)) {
                int yy = s[0] << 16, cb = s[1] - 128, cr = s[2] - 128;
                r = clamp((yy + 91881 * cr + 32768) >> 16);
                g = clamp((yy - 22554 * cb - 46802 * cr + 32768) >> 16);
                b = clamp((yy + 116130 * cb + 32768) >> 16);
            } else if (j->ncomp == 3) {
                r = s[0], g = s[1], b = s[2];
            } else {
                int cc = s[0], m = s[1], yy = s[2], k = s[3];
                if (j->transform == 2) {
                    int Y = cc << 16, cb = m - 128, cr = yy - 128;
                    cc = clamp((Y + 91881 * cr + 32768) >> 16);
                    m = clamp((Y - 22554 * cb - 46802 * cr + 32768) >> 16);
                    yy = clamp((Y + 116130 * cb + 32768) >> 16);
                }
                r = cc * k / 255;
                g = m * k / 255;
                b = yy * k / 255;
            }
            row[x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
    return 0;
}

int jpeg_decode(const uint8_t *data, size_t size, struct picture *out)
{
    struct jpeg *j = kzalloc(sizeof(*j));
    if (!j)
        return E_NOMEM;
    j->data = data;
    j->end = data + size;
    const uint8_t *p = data + 2;
    int err = E_INVAL;
    bool framed = false, scanned = false;
    while (p + 4 <= j->end) {
        p = next_marker(p, j->end);
        if (p + 2 > j->end)
            break;
        int m = p[1];
        p += 2;
        if (m == 0xd9)
            break;
        if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01)
            continue;
        if (p + 2 > j->end)
            break;
        int len = be16(p);
        if (len < 2 || p + len > j->end)
            break;
        const uint8_t *b = p + 2;
        int n = len - 2;
        if (m == 0xc4) {
            while (n >= 17) {
                int tc = b[0] >> 4, th = b[0] & 3, total = 0;
                for (int i = 0; i < 16; i++)
                    total += b[1 + i];
                if (total > 256 || n < 17 + total || tc > 1)
                    goto out;
                struct huff *t = &j->huff[tc * 4 + th];
                memcpy(t->values, b + 17, (size_t)total);
                if (!build_huff(t, b + 1))
                    goto out;
                b += 17 + total;
                n -= 17 + total;
            }
        } else if (m == 0xdb) {
            while (n >= 65) {
                int pq = b[0] >> 4, tq = b[0] & 3;
                int need = pq ? 129 : 65;
                if (n < need)
                    goto out;
                for (int i = 0; i < 64; i++)
                    j->quant[tq][dezigzag[i]] = pq ? be16(b + 1 + i * 2) : b[1 + i];
                b += need;
                n -= need;
            }
        } else if (m == 0xdd && n >= 2) {
            j->restart = be16(b);
        } else if (m == 0xee && n >= 12 && !memcmp(b, "Adobe", 5)) {
            j->adobe = true;
            j->transform = b[11];
        } else if (m == 0xc0 || m == 0xc1 || m == 0xc2) {
            if (framed || !frame_header(j, b, n)) {
                if (j->no_memory)
                    err = E_NOMEM;
                goto out;
            }
            framed = true;
            j->progressive = m == 0xc2;
        } else if ((m >= 0xc3 && m <= 0xcf) && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            err = E_NOSYS;
            goto out;
        } else if (m == 0xda) {
            if (!framed || n < 1)
                goto out;
            int ns = b[0];
            if (ns < 1 || ns > 4 || n < 4 + ns * 2)
                goto out;
            struct comp *list[4];
            for (int i = 0; i < ns; i++) {
                list[i] = NULL;
                for (int k = 0; k < j->ncomp; k++)
                    if (j->comp[k].id == b[1 + i * 2])
                        list[i] = &j->comp[k];
                if (!list[i])
                    goto out;
                list[i]->hd = b[2 + i * 2] >> 4 & 3;
                list[i]->ha = b[2 + i * 2] & 3;
            }
            int ss = b[1 + ns * 2], se = b[2 + ns * 2], ah = b[3 + ns * 2] >> 4, al = b[3 + ns * 2] & 15;
            if (!j->progressive) {
                ss = 0;
                se = 63;
                ah = al = 0;
            }
            if (ss > se || se > 63)
                goto out;
            j->pos = p + len;
            if (!scan(j, list, ns, ss, se, ah, al) && !scanned)
                goto out;
            scanned = true;
            p = j->pos;
            continue;
        }
        p += len;
    }
    if (scanned)
        err = finish(j, out);
out:
    release(j);
    kfree(j);
    if (err)
        picture_free(out);
    return err;
}
