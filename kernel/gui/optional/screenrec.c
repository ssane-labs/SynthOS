#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

/* Screen recording: the screen is captured a few times a second, every frame is coded as a baseline JPEG
   (integer DCT, the standard tables, 4:2:0) and written to a Motion JPEG AVI that Video plays. A small badge in the
   top right corner shows the time and stops the recording. The index and the frame count go into the file when
   it stops. */

#define FPS_DEFAULT 8
#define QUALITY 75
#define BADGE_W 132
#define BADGE_H 30

/* ---- JPEG ---- */

static const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                   12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                   35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                   58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const uint8_t base_luma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                      14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                      18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                      49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

static const uint8_t base_chroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                        24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

/* cos((2x+1)uπ/16) · C(u)/2 · 4096 */
static const int16_t dct_cos[8][8] = {
    {1448, 1448, 1448, 1448, 1448, 1448, 1448, 1448},    {2009, 1703, 1138, 400, -400, -1138, -1703, -2009},
    {1892, 784, -784, -1892, -1892, -784, 784, 1892},    {1703, -400, -2009, -1138, 1138, 2009, 400, -1703},
    {1448, -1448, -1448, 1448, 1448, -1448, -1448, 1448}, {1138, -2009, 400, 1703, -1703, -400, 2009, -1138},
    {784, -1892, 1892, -784, -784, 1892, -1892, 784},    {400, -1138, 1703, -2009, 2009, -1703, 1138, -400}};

static const uint8_t dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct huff {
    uint16_t code[256];
    uint8_t len[256];
};

struct jpeg {
    uint8_t q[2][64]; /* in zigzag order */
    struct huff dc[2], ac[2];
    uint8_t *out;
    size_t len, cap;
    uint32_t bits;
    int nbits;
    bool failed;
};

static void build_huff(struct huff *h, const uint8_t *bits, const uint8_t *vals)
{
    uint16_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++, k++) {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

static void jpeg_init(struct jpeg *j, int quality)
{
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        j->q[0][i] = (uint8_t)CLAMP((base_luma[zigzag[i]] * scale + 50) / 100, 1, 255);
        j->q[1][i] = (uint8_t)CLAMP((base_chroma[zigzag[i]] * scale + 50) / 100, 1, 255);
    }
    build_huff(&j->dc[0], dc_luma_bits, dc_vals);
    build_huff(&j->dc[1], dc_chroma_bits, dc_vals);
    build_huff(&j->ac[0], ac_luma_bits, ac_luma_vals);
    build_huff(&j->ac[1], ac_chroma_bits, ac_chroma_vals);
}

static void put(struct jpeg *j, const void *data, size_t n)
{
    if (j->len + n > j->cap) {
        size_t cap = MAX(j->cap * 2, j->len + n + 65536);
        uint8_t *grown = krealloc(j->out, cap);
        if (!grown) {
            j->failed = true;
            return;
        }
        j->out = grown;
        j->cap = cap;
    }
    memcpy(j->out + j->len, data, n);
    j->len += n;
}

static void put8(struct jpeg *j, int v)
{
    uint8_t b = (uint8_t)v;
    put(j, &b, 1);
}

static void put16(struct jpeg *j, int v)
{
    put8(j, v >> 8);
    put8(j, v);
}

static void put_bits(struct jpeg *j, uint32_t code, int n)
{
    j->bits = (j->bits << n) | (code & ((1u << n) - 1));
    j->nbits += n;
    while (j->nbits >= 8) {
        int b = (int)(j->bits >> (j->nbits - 8)) & 0xff;
        put8(j, b);
        if (b == 0xff)
            put8(j, 0);
        j->nbits -= 8;
    }
}

static void put_table(struct jpeg *j, int id, const uint8_t *bits, const uint8_t *vals, int n)
{
    put8(j, id);
    put(j, bits, 16);
    put(j, vals, (size_t)n);
}

static void headers(struct jpeg *j, int w, int h)
{
    static const uint8_t soi_app0[] = {0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    put(j, soi_app0, sizeof(soi_app0));
    put16(j, 0xffdb);
    put16(j, 2 + 2 * 65);
    for (int t = 0; t < 2; t++) {
        put8(j, t);
        put(j, j->q[t], 64);
    }
    put16(j, 0xffc0);
    put16(j, 17);
    put8(j, 8);
    put16(j, h);
    put16(j, w);
    put8(j, 3);
    static const uint8_t comps[9] = {1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
    put(j, comps, sizeof(comps));
    put16(j, 0xffc4);
    put16(j, 2 + 4 * 17 + 12 + 12 + 162 + 162);
    put_table(j, 0x00, dc_luma_bits, dc_vals, 12);
    put_table(j, 0x10, ac_luma_bits, ac_luma_vals, 162);
    put_table(j, 0x01, dc_chroma_bits, dc_vals, 12);
    put_table(j, 0x11, ac_chroma_bits, ac_chroma_vals, 162);
    static const uint8_t sos[] = {0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    put(j, sos, sizeof(sos));
}

static int magnitude(int v, uint32_t *bits)
{
    int a = v < 0 ? -v : v, n = 0;
    while (a >> n)
        n++;
    *bits = (uint32_t)(v < 0 ? v - 1 : v);
    return n;
}

/* one 8×8 block of samples (0..255, level-shifted here) */
static void block(struct jpeg *j, const int16_t *px, int table, int *dc_prev)
{
    /* c[u][7-x] = ±c[u][x]: even rows take the sums of mirrored samples, odd rows the differences */
    int32_t tmp[64], coef[64];
    for (int y = 0; y < 8; y++) {
        const int16_t *row = px + y * 8;
        int32_t sum[4], dif[4];
        for (int x = 0; x < 4; x++) {
            sum[x] = row[x] + row[7 - x] - 256;
            dif[x] = row[x] - row[7 - x];
        }
        for (int u = 0; u < 8; u++) {
            const int32_t *v = u & 1 ? dif : sum;
            tmp[y * 8 + u] = (v[0] * dct_cos[u][0] + v[1] * dct_cos[u][1] + v[2] * dct_cos[u][2] +
                              v[3] * dct_cos[u][3]) >> 8;
        }
    }
    for (int u = 0; u < 8; u++) {
        int32_t sum[4], dif[4];
        for (int y = 0; y < 4; y++) {
            sum[y] = tmp[y * 8 + u] + tmp[(7 - y) * 8 + u];
            dif[y] = tmp[y * 8 + u] - tmp[(7 - y) * 8 + u];
        }
        for (int v = 0; v < 8; v++) {
            const int32_t *t = v & 1 ? dif : sum;
            coef[v * 8 + u] =
                (t[0] * dct_cos[v][0] + t[1] * dct_cos[v][1] + t[2] * dct_cos[v][2] + t[3] * dct_cos[v][3]) >> 16;
        }
    }
    int q[64];
    for (int i = 0; i < 64; i++) {
        int c = coef[zigzag[i]], d = j->q[table][i];
        q[i] = c >= 0 ? (c + d / 2) / d : -((-c + d / 2) / d);
    }
    uint32_t bits;
    int diff = q[0] - *dc_prev, n = magnitude(diff, &bits);
    *dc_prev = q[0];
    put_bits(j, j->dc[table].code[n], j->dc[table].len[n]);
    if (n)
        put_bits(j, bits, n);
    int run = 0;
    for (int i = 1; i < 64; i++) {
        if (!q[i]) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(j, j->ac[table].code[0xf0], j->ac[table].len[0xf0]);
            run -= 16;
        }
        n = magnitude(q[i], &bits);
        int sym = run << 4 | n;
        put_bits(j, j->ac[table].code[sym], j->ac[table].len[sym]);
        put_bits(j, bits, n);
        run = 0;
    }
    if (run)
        put_bits(j, j->ac[table].code[0], j->ac[table].len[0]);
}

/* codes a picture (w and h even) into j->out, from len 0 */
static void jpeg_encode(struct jpeg *j, const uint32_t *px, int stride, int w, int h)
{
    j->len = 0;
    j->bits = 0;
    j->nbits = 0;
    j->failed = false;
    headers(j, w, h);
    int dc[3] = {0, 0, 0};
    int16_t Y[4][64], cb[64], cr[64];
    for (int my = 0; my < h; my += 16)
        for (int mx = 0; mx < w; mx += 16) {
            int sum_b[64] = {0}, sum_r[64] = {0};
            for (int y = 0; y < 16; y++) {
                const uint32_t *row = px + MIN(my + y, h - 1) * stride;
                for (int x = 0; x < 16; x++) {
                    uint32_t p = row[MIN(mx + x, w - 1)];
                    int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
                    Y[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)] = (int16_t)((77 * r + 150 * g + 29 * b) >> 8);
                    int k = (y >> 1) * 8 + (x >> 1);
                    sum_b[k] += (-43 * r - 85 * g + 128 * b) >> 8;
                    sum_r[k] += (128 * r - 107 * g - 21 * b) >> 8;
                }
            }
            for (int k = 0; k < 64; k++) {
                cb[k] = (int16_t)(sum_b[k] / 4 + 128);
                cr[k] = (int16_t)(sum_r[k] / 4 + 128);
            }
            for (int b = 0; b < 4; b++)
                block(j, Y[b], 0, &dc[0]);
            block(j, cb, 1, &dc[1]);
            block(j, cr, 1, &dc[2]);
        }
    if (j->nbits)
        put_bits(j, 0x7f, 8 - j->nbits);
    put16(j, 0xffd9);
}

/* ---- AVI ---- */

#define AVI_HEADER 224 /* up to and including the "movi" tag */

struct recording {
    struct file *f;
    char path[PATH_MAX_LEN];
    int w, h, half, fps;
    uint32_t frames, biggest;
    uint32_t *offsets, *lengths, cap;
    uint32_t written; /* bytes after the "movi" tag */
    uint64_t started, due;
    uint32_t cost_ms; /* how long the last frame took */
    struct jpeg jpeg;
    struct canvas small;
    struct window *badge;
    struct ui_state st;
    char error[64];
};

static struct recording *rec;

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* the rate the frames really came at */
static uint32_t us_per_frame(struct recording *r)
{
    uint64_t ms = hal_ticks_ms() - r->started;
    return r->frames > 1 ? (uint32_t)(ms * 1000 / r->frames) : 1000000u / (uint32_t)r->fps;
}

static void avi_header(struct recording *r, uint8_t *b, uint32_t file_size)
{
    memset(b, 0, AVI_HEADER);
    uint32_t us = us_per_frame(r);
    memcpy(b, "RIFF", 4);
    le32(b + 4, file_size - 8);
    memcpy(b + 8, "AVI LIST", 8);
    le32(b + 16, 192);
    memcpy(b + 20, "hdrlavih", 8);
    le32(b + 28, 56);
    le32(b + 32, us);
    le32(b + 36, (uint32_t)((uint64_t)r->biggest * 1000000u / us));
    le32(b + 44, 0x10); /* has an index */
    le32(b + 48, r->frames);
    le32(b + 56, 1);
    le32(b + 60, r->biggest);
    le32(b + 64, (uint32_t)r->w);
    le32(b + 68, (uint32_t)r->h);
    memcpy(b + 88, "LIST", 4);
    le32(b + 92, 116);
    memcpy(b + 96, "strlstrh", 8);
    le32(b + 104, 56);
    memcpy(b + 108, "vidsMJPG", 8);
    le32(b + 128, us);
    le32(b + 132, 1000000u);
    le32(b + 140, r->frames);
    le32(b + 144, r->biggest);
    le32(b + 148, 0xffffffffu);
    le32(b + 160, (uint32_t)r->w | (uint32_t)r->h << 16);
    memcpy(b + 164, "strf", 4);
    le32(b + 168, 40);
    le32(b + 172, 40);
    le32(b + 176, (uint32_t)r->w);
    le32(b + 180, (uint32_t)r->h);
    le32(b + 184, 1 | 24 << 16);
    memcpy(b + 188, "MJPG", 4);
    le32(b + 192, (uint32_t)(r->w * r->h * 3));
    memcpy(b + 212, "LIST", 4);
    le32(b + 216, 4 + r->written);
    memcpy(b + 220, "movi", 4);
}

static void capture(struct recording *r)
{
    uint64_t begin = hal_ticks_ms();
    struct canvas shot;
    if (!wm_capture(wm_screen_rect(0), &shot))
        return;
    const uint32_t *px = shot.px;
    int stride = shot.stride;
    if (r->half) {
        for (int y = 0; y < r->h; y++)
            for (int x = 0; x < r->w; x++) {
                const uint32_t *p = shot.px + y * 2 * shot.stride + x * 2;
                uint32_t a = p[0], b = p[1], c = p[shot.stride], d = p[shot.stride + 1], o = 0;
                for (int s = 0; s < 24; s += 8)
                    o |= (((a >> s & 255) + (b >> s & 255) + (c >> s & 255) + (d >> s & 255)) / 4) << s;
                r->small.px[y * r->small.stride + x] = o;
            }
        px = r->small.px;
        stride = r->small.stride;
    }
    jpeg_encode(&r->jpeg, px, stride, r->w, r->h);
    canvas_free(&shot);
    if (r->jpeg.failed)
        return;
    if (r->frames == r->cap) {
        uint32_t cap = r->cap ? r->cap * 2 : 256;
        uint32_t *o = krealloc(r->offsets, cap * 4), *l = o ? krealloc(r->lengths, cap * 4) : NULL;
        if (o)
            r->offsets = o;
        if (!l)
            return;
        r->lengths = l;
        r->cap = cap;
    }
    uint32_t len = (uint32_t)r->jpeg.len;
    uint8_t chunk[8] = {'0', '0', 'd', 'c'};
    le32(chunk + 4, len);
    if (vfs_write(r->f, chunk, 8) != 8 || vfs_write(r->f, r->jpeg.out, len) != (long)len ||
        ((len & 1) && vfs_write(r->f, "", 1) != 1)) {
        str_copy(r->error, "The disk is full", sizeof(r->error));
        return;
    }
    r->offsets[r->frames] = 4 + r->written;
    r->lengths[r->frames++] = len;
    r->written += 8 + len + (len & 1);
    r->biggest = MAX(r->biggest, len);
    uint64_t end = hal_ticks_ms();
    r->cost_ms = (uint32_t)(end - begin);
    /* at the asked rate, but never more than a third of the time: the desktop has to stay usable */
    r->due = MAX(begin + 1000u / (unsigned)r->fps, end + 2 * r->cost_ms);
}


static int finish(struct recording *r)
{
    int err = 0;
    size_t idx_len = 8 + (size_t)r->frames * 16;
    uint8_t *idx = kmalloc(MAX(idx_len, (size_t)AVI_HEADER));
    if (!idx) {
        err = E_NOMEM;
    } else {
        memcpy(idx, "idx1", 4);
        le32(idx + 4, r->frames * 16);
        for (uint32_t i = 0; i < r->frames; i++) {
            uint8_t *e = idx + 8 + i * 16;
            memcpy(e, "00dc", 4);
            le32(e + 4, 0x10);
            le32(e + 8, r->offsets[i]);
            le32(e + 12, r->lengths[i]);
        }
        if (vfs_write(r->f, idx, idx_len) != (long)idx_len)
            err = E_NOSPC;
        avi_header(r, idx, AVI_HEADER + r->written + (uint32_t)idx_len);
        if (vfs_seek(r->f, 0, VFS_SEEK_SET) != 0 || vfs_write(r->f, idx, AVI_HEADER) != AVI_HEADER)
            err = err ? err : E_IO;
        kfree(idx);
    }
    vfs_close(r->f);
    return err;
}

/* ---- the badge ---- */

static void stop_recording(void);

static void badge_frame(struct ui *ui, void *data)
{
    struct recording *r = data;
    struct canvas *c = ui->c;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (c) {
        gfx_round(c, (struct rect){0, 0, W, H}, H / 2, ALPHA(style.menu, 236));
        gfx_round_frame(c, (struct rect){0, 0, W, H}, H / 2, style.border);
        bool on = (hal_ticks_ms() / 600) % 2 == 0;
        gfx_circle(c, 16, H / 2, 5, on ? style.error : ALPHA(style.error, 110));
        uint32_t s = (uint32_t)((hal_ticks_ms() - r->started) / 1000);
        char t[16];
        ksnprintf(t, sizeof(t), "%u:%02u", s / 60, s % 60);
        gfx_text_box(c, (struct rect){28, 0, 44, H}, &font_sans, t, style.text, ALIGN_LEFT);
    }
    if (ui_button(ui, (struct rect){W - 58, 4, 52, H - 8}, "Stop"))
        stop_recording();
}

static void badge_paint(struct window *w, struct canvas *c)
{
    if (rec && w == rec->badge)
        ui_run(w, &rec->st, c, NULL, badge_frame, rec);
}

static void badge_event(struct window *w, struct gui_event *e)
{
    if (!rec || w != rec->badge)
        return;
    if (e->type == EV_TIMER) {
        if (hal_ticks_ms() >= rec->due) {
            capture(rec);
            if (rec->error[0]) {
                stop_recording();
                return;
            }
        }
        wm_invalidate(w);
        return;
    }
    ui_run(w, &rec->st, NULL, e, badge_frame, rec);
}

static void badge_destroy(struct window *w)
{
    if (rec && rec->badge == w)
        rec->badge = NULL;
}

static const struct window_ops badge_ops = {badge_paint, badge_event, NULL, badge_destroy};

static void free_recording(struct recording *r)
{
    kfree(r->offsets);
    kfree(r->lengths);
    kfree(r->jpeg.out);
    canvas_free(&r->small);
    kfree(r);
}

static void default_path(char *out, size_t cap)
{
    char dir[PATH_MAX_LEN], name[64];
    path_join(user_home(), "Movies", dir, sizeof(dir));
    vfs_mkdirs(dir);
    struct rtc_time t;
    hal_rtc(&t);
    ksnprintf(name, sizeof(name), "Screen Recording %04d-%02d-%02d at %02d.%02d.%02d.avi", t.year, t.month, t.day,
              t.hour, t.minute, t.second);
    path_join(dir, name, out, cap);
}

static int start_recording(const char *path, bool half, int fps)
{
    if (rec)
        return E_BUSY;
    struct rect s = wm_screen_rect(0);
    struct recording *r = kzalloc(sizeof(*r));
    if (!r)
        return E_NOMEM;
    r->half = half;
    r->fps = CLAMP(fps, 1, 25);
    r->w = (half ? s.w / 2 : s.w) & ~1;
    r->h = (half ? s.h / 2 : s.h) & ~1;
    if (path)
        str_copy(r->path, path, sizeof(r->path));
    else
        default_path(r->path, sizeof(r->path));
    jpeg_init(&r->jpeg, QUALITY);
    uint8_t header[AVI_HEADER];
    avi_header(r, header, AVI_HEADER);
    int err = half && !canvas_alloc(&r->small, r->w, r->h) ? E_NOMEM : 0;
    if (!err)
        err = vfs_open(r->path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &r->f);
    if (!err && vfs_write(r->f, header, AVI_HEADER) != AVI_HEADER) {
        vfs_close(r->f);
        err = E_IO;
    }
    if (err) {
        free_recording(r);
        return err;
    }
    rec = r;
    r->started = hal_ticks_ms();
    capture(r);
    struct window *was = wm_focused();
    r->badge = wm_open("Screen Recording", ICON_VIDEO, BADGE_W, BADGE_H, &badge_ops, NULL,
                       WIN_NOTITLE | WIN_FULLSIZE | WIN_OWNCONTROLS | WIN_NOACTIVATE | WIN_PINNED);
    if (r->badge) {
        r->badge->radius = BADGE_H / 2;
        wm_move(r->badge, s.x + s.w - BADGE_W - 12, s.y + MENUBAR_HEIGHT + 8);
        wm_set_timer(r->badge, 40);
        if (was && wm_window_alive(was))
            wm_focus(was); /* the badge never takes the keyboard */
    }
    return 0;
}

static void stop_recording(void)
{
    struct recording *r = rec;
    if (!r)
        return;
    rec = NULL;
    if (r->badge)
        wm_close(r->badge);
    int err = finish(r);
    char text[PATH_MAX_LEN + 64];
    if (r->error[0] || err)
        ksnprintf(text, sizeof(text), "%s", r->error[0] ? r->error : err_str(err));
    else
        ksnprintf(text, sizeof(text), "%s (%u frames)", path_basename(r->path), r->frames);
    gui_notify("Screen Recording", text);
    free_recording(r);
}

COMMAND(screenrec, NULL, "system", "screenrec [start [file] [--half] [--fps n]|stop|status|frame]",
        "record the screen to a Motion JPEG AVI video")
{
    const char *op = argc > 1 ? argv[1] : "status";
    if (!strcmp(op, "start")) {
        if (rect_empty(wm_screen_rect(0)))
            return sh_error(sh, "screenrec", "the desktop is not running");
        const char *file = NULL;
        bool half = false;
        int fps = FPS_DEFAULT;
        for (int i = 2; i < argc; i++)
            if (!strcmp(argv[i], "--half"))
                half = true;
            else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
                fps = (int)str_to_long(argv[++i], NULL, 10);
            else
                file = argv[i];
        char path[PATH_MAX_LEN];
        if (file && sh_resolve(sh, file, path, sizeof(path)) != 0)
            return sh_error(sh, "screenrec", "bad path: %s", file);
        int err = start_recording(file ? path : NULL, half, fps);
        if (err)
            return sh_error(sh, "screenrec", "%s", err == E_BUSY ? "already recording" : err_str(err));
        sh_printf(sh, "recording %dx%d at %d fps to %s\n", rec->w, rec->h, rec->fps, rec->path);
    } else if (!strcmp(op, "frame")) {
        if (!rec)
            return sh_error(sh, "screenrec", "not recording");
        capture(rec);
        sh_printf(sh, "%u frames\n", rec->frames);
    } else if (!strcmp(op, "stop")) {
        if (!rec)
            return sh_error(sh, "screenrec", "not recording");
        uint32_t frames = rec->frames;
        char path[PATH_MAX_LEN];
        str_copy(path, rec->path, sizeof(path));
        stop_recording();
        sh_printf(sh, "saved %u frames to %s\n", frames, path);
    } else if (!strcmp(op, "status")) {
        if (rec)
            sh_printf(sh, "recording %s: %u frames, %u KB, %u ms a frame\n", rec->path, rec->frames,
                      rec->written / 1024, rec->cost_ms);
        else
            sh_puts(sh, "not recording\n");
    } else {
        return command_usage(sh, command_find("screenrec"));
    }
    return 0;
}

static const struct command *const commands[] = {&cmddef_screenrec};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(NULL, NULL, &ext);
