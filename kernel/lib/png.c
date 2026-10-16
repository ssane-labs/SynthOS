#include <synth/error.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/picture.h>
#include <synth/string.h>

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static bool unfilter(uint8_t *data, size_t avail, int rows, size_t stride, int bpp)
{
    uint8_t *prev = NULL;
    for (int y = 0; y < rows; y++) {
        if ((size_t)(y + 1) * (stride + 1) > avail)
            return false;
        uint8_t *line = data + (size_t)y * (stride + 1);
        int type = line[0];
        uint8_t *row = line + 1;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)bpp ? row[x - (size_t)bpp] : 0, b = prev ? prev[x] : 0;
            int c = prev && x >= (size_t)bpp ? prev[x - (size_t)bpp] : 0;
            switch (type) {
            case 0: break;
            case 1: row[x] = (uint8_t)(row[x] + a); break;
            case 2: row[x] = (uint8_t)(row[x] + b); break;
            case 3: row[x] = (uint8_t)(row[x] + (a + b) / 2); break;
            case 4: row[x] = (uint8_t)(row[x] + paeth(a, b, c)); break;
            default: return false;
            }
        }
        prev = row;
    }
    return true;
}

struct png {
    int w, h, depth, type, channels;
    uint8_t palette[256][4];
    int palette_count;
    bool has_key;
    uint16_t key[3];
};

static int sample(const uint8_t *row, int x, int depth)
{
    if (depth == 8)
        return row[x];
    if (depth == 16)
        return row[x * 2];
    int per = 8 / depth;
    return row[x / per] >> (8 - depth * (x % per + 1)) & ((1 << depth) - 1);
}

static uint16_t sample16(const uint8_t *row, int i, int depth)
{
    if (depth == 16)
        return (uint16_t)(row[i * 2] << 8 | row[i * 2 + 1]);
    return (uint16_t)sample(row, i, depth);
}

static uint32_t pixel(const struct png *p, const uint8_t *row, int x)
{
    int d = p->depth, scale = d < 8 ? 255 / ((1 << d) - 1) : 1;
    switch (p->type) {
    case 0: {
        int g = sample(row, x, d) * scale;
        bool clear = p->has_key && sample16(row, x, d) == p->key[0];
        return (uint32_t)(clear ? 0 : 255) << 24 | (uint32_t)g * 0x010101u;
    }
    case 2: {
        int r = sample(row, x * 3, d), g = sample(row, x * 3 + 1, d), b = sample(row, x * 3 + 2, d);
        bool clear = p->has_key && sample16(row, x * 3, d) == p->key[0] && sample16(row, x * 3 + 1, d) == p->key[1] &&
                     sample16(row, x * 3 + 2, d) == p->key[2];
        return (uint32_t)(clear ? 0 : 255) << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
    case 3: {
        int i = sample(row, x, d);
        const uint8_t *c = p->palette[i < p->palette_count ? i : 0];
        return (uint32_t)c[3] << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    }
    case 4: {
        int g = sample(row, x * 2, d), a = sample(row, x * 2 + 1, d);
        return (uint32_t)a << 24 | (uint32_t)g * 0x010101u;
    }
    default: {
        int r = sample(row, x * 4, d), g = sample(row, x * 4 + 1, d), b = sample(row, x * 4 + 2, d),
            a = sample(row, x * 4 + 3, d);
        return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
    }
}

static size_t row_bytes(const struct png *p, int w)
{
    return ((size_t)w * (size_t)p->channels * (size_t)p->depth + 7) / 8;
}

int png_decode(const uint8_t *data, size_t size, struct picture *out)
{
    struct png p;
    memset(&p, 0, sizeof(p));
    if (size < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8))
        return E_INVAL;
    uint8_t *z = NULL;
    size_t zlen = 0, zcap = 0;
    int interlace = 0;
    bool header = false;
    for (size_t off = 8; off + 12 <= size;) {
        uint32_t len = be32(data + off);
        const uint8_t *type = data + off + 4, *body = data + off + 8;
        if (len > size - off - 12)
            break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            p.w = (int)be32(body);
            p.h = (int)be32(body + 4);
            p.depth = body[8];
            p.type = body[9];
            interlace = body[12];
            header = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            p.palette_count = (int)MIN(len / 3, 256u);
            for (int i = 0; i < p.palette_count; i++) {
                memcpy(p.palette[i], body + i * 3, 3);
                p.palette[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            if (p.type == 3)
                for (uint32_t i = 0; i < len && i < 256; i++)
                    p.palette[i][3] = body[i];
            else if (p.type == 0 && len >= 2)
                p.has_key = true, p.key[0] = (uint16_t)(body[0] << 8 | body[1]);
            else if (p.type == 2 && len >= 6) {
                p.has_key = true;
                for (int i = 0; i < 3; i++)
                    p.key[i] = (uint16_t)(body[i * 2] << 8 | body[i * 2 + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (zlen + len > zcap) {
                size_t cap = MAX(zcap * 2, zlen + len + 65536);
                uint8_t *n = krealloc(z, cap);
                if (!n) {
                    kfree(z);
                    return E_NOMEM;
                }
                z = n;
                zcap = cap;
            }
            memcpy(z + zlen, body, len);
            zlen += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + len;
    }
    static const int channels[7] = {1, 0, 3, 1, 2, 0, 4};
    if (!header || p.w <= 0 || p.h <= 0 || p.type > 6 || !channels[p.type] || interlace > 1 ||
        (uint64_t)p.w * (uint64_t)p.h > PICTURE_MAX_PIXELS || !z ||
        (p.depth != 1 && p.depth != 2 && p.depth != 4 && p.depth != 8 && p.depth != 16)) {
        kfree(z);
        return E_INVAL;
    }
    p.channels = channels[p.type];
    int bpp = MAX(1, p.channels * p.depth / 8);
    static const int ax[7] = {0, 4, 0, 2, 0, 1, 0}, ay[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    size_t need = 0;
    for (int pass = 0; pass < (interlace ? 7 : 1); pass++) {
        int pw = interlace ? (p.w - ax[pass] + dx[pass] - 1) / dx[pass] : p.w;
        int ph = interlace ? (p.h - ay[pass] + dy[pass] - 1) / dy[pass] : p.h;
        if (pw > 0 && ph > 0)
            need += (row_bytes(&p, pw) + 1) * (size_t)ph;
    }
    uint8_t *raw;
    size_t raw_len;
    int err = zlib_inflate_alloc(z, zlen, need + 16, &raw, &raw_len);
    kfree(z);
    if (err)
        return err;
    out->px = kzalloc((size_t)p.w * (size_t)p.h * 4);
    if (!out->px) {
        kfree(raw);
        return E_NOMEM;
    }
    out->w = p.w;
    out->h = p.h;
    out->frames = 1;
    size_t pos = 0;
    for (int pass = 0; pass < (interlace ? 7 : 1); pass++) {
        int pw = interlace ? (p.w - ax[pass] + dx[pass] - 1) / dx[pass] : p.w;
        int ph = interlace ? (p.h - ay[pass] + dy[pass] - 1) / dy[pass] : p.h;
        if (pw <= 0 || ph <= 0)
            continue;
        size_t stride = row_bytes(&p, pw);
        if (pos > raw_len || !unfilter(raw + pos, raw_len - pos, ph, stride, bpp)) {
            kfree(raw);
            picture_free(out);
            return E_INVAL;
        }
        for (int y = 0; y < ph; y++) {
            const uint8_t *row = raw + pos + (size_t)y * (stride + 1) + 1;
            int oy = interlace ? ay[pass] + y * dy[pass] : y;
            for (int x = 0; x < pw; x++) {
                int ox = interlace ? ax[pass] + x * dx[pass] : x;
                out->px[(size_t)oy * (size_t)p.w + (size_t)ox] = pixel(&p, row, x);
            }
        }
        pos += (stride + 1) * (size_t)ph;
    }
    kfree(raw);
    return 0;
}
