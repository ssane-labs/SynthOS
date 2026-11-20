#include <synth/error.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/module.h>
#include <synth/picture.h>
#include <synth/string.h>

#include "../internal.h"

/* More file formats for the whole system (Photos, Files, Music, Quick Look, wallpapers): TIFF, Targa, PNM, QOI and
   ICO pictures, FLAC and AIFF sound. The kernel asks this module when it does not know a file itself. */

static uint32_t *new_pixels(struct picture *out, uint32_t w, uint32_t h)
{
    if (!w || !h || (uint64_t)w * h > PICTURE_MAX_PIXELS)
        return NULL;
    memset(out, 0, sizeof(*out));
    out->px = kmalloc((size_t)w * h * 4);
    if (!out->px)
        return NULL;
    out->w = (int)w;
    out->h = (int)h;
    out->frames = 1;
    return out->px;
}

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b)
{
    return a << 24 | r << 16 | g << 8 | b;
}

/* ---- TIFF ---- */

struct tiff {
    const uint8_t *d;
    size_t size;
    bool be;
};

static uint32_t t16(const struct tiff *t, size_t o)
{
    if (o + 2 > t->size)
        return 0;
    return t->be ? (uint32_t)t->d[o] << 8 | t->d[o + 1] : (uint32_t)t->d[o + 1] << 8 | t->d[o];
}

static uint32_t t32(const struct tiff *t, size_t o)
{
    if (o + 4 > t->size)
        return 0;
    return t->be ? t16(t, o) << 16 | t16(t, o + 2) : t16(t, o + 2) << 16 | t16(t, o);
}

/* the i-th value of a tag (type 3 SHORT or 4 LONG), stored in place or at its offset */
static uint32_t tag_value(const struct tiff *t, size_t entry, uint32_t i)
{
    uint32_t type = t16(t, entry + 2), count = t32(t, entry + 4);
    int size = type == 3 ? 2 : type == 1 ? 1 : 4;
    size_t at = (size_t)size * count <= 4 ? entry + 8 : t32(t, entry + 8);
    at += (size_t)i * (size_t)size;
    return size == 2 ? t16(t, at) : size == 1 ? (at < t->size ? t->d[at] : 0) : t32(t, at);
}

static size_t lzw_tiff(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], first[4096];
    static uint8_t stack[4096];
    for (int i = 0; i < 256; i++) {
        prefix[i] = 0xffff;
        suffix[i] = first[i] = (uint8_t)i;
    }
    int next = 258, bits = 9, old = -1;
    size_t pos = 0, o = 0;
    while (o < cap) {
        if ((pos + (size_t)bits) > n * 8)
            break;
        int code = 0;
        for (int b = 0; b < bits; b++, pos++)
            code = code << 1 | ((in[pos >> 3] >> (7 - (pos & 7))) & 1);
        if (code == 257)
            break;
        if (code == 256) {
            next = 258;
            bits = 9;
            old = -1;
            continue;
        }
        int c = code, sp = 0;
        if (code >= next) {
            if (old < 0)
                break;
            stack[sp++] = first[old];
            c = old;
        }
        while (c >= 258 && sp < 4095) {
            stack[sp++] = suffix[c];
            c = prefix[c];
        }
        stack[sp++] = (uint8_t)c;
        uint8_t head = stack[sp - 1];
        while (sp && o < cap)
            out[o++] = stack[--sp];
        if (old >= 0 && next < 4096) {
            prefix[next] = (uint16_t)old;
            suffix[next] = head;
            first[next] = first[old];
            next++;
        }
        if (next + 1 >= (1 << bits) && bits < 12)
            bits++;
        old = code;
    }
    return o;
}

static size_t packbits(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n && o < cap) {
        int8_t c = (int8_t)in[i++];
        if (c >= 0) {
            for (int k = 0; k <= c && i < n && o < cap; k++)
                out[o++] = in[i++];
        } else if (c != -128 && i < n) {
            for (int k = 0; k < 1 - c && o < cap; k++)
                out[o++] = in[i];
            i++;
        }
    }
    return o;
}

static int tiff_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 8 || !((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) ||
                      (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42)))
        return E_INVAL;
    struct tiff t = {d, size, d[0] == 'M'};
    size_t ifd = t32(&t, 4);
    uint32_t n = t16(&t, ifd);
    uint32_t w = 0, h = 0, bps = 1, comp = 1, photo = 1, spp = 1, rows = 0xffffffff, pred = 1, extra = 0;
    size_t offs = 0, counts = 0, cmap = 0;
    uint32_t strips = 0;
    for (uint32_t i = 0; i < n && ifd + 2 + (i + 1) * 12 <= size; i++) {
        size_t e = ifd + 2 + i * 12;
        uint32_t tag = t16(&t, e);
        switch (tag) {
        case 256: w = tag_value(&t, e, 0); break;
        case 257: h = tag_value(&t, e, 0); break;
        case 258: bps = tag_value(&t, e, 0); break;
        case 259: comp = tag_value(&t, e, 0); break;
        case 262: photo = tag_value(&t, e, 0); break;
        case 273: offs = e; strips = t32(&t, e + 4); break;
        case 277: spp = tag_value(&t, e, 0); break;
        case 278: rows = tag_value(&t, e, 0); break;
        case 279: counts = e; break;
        case 317: pred = tag_value(&t, e, 0); break;
        case 320: cmap = e; break;
        case 338: extra = 1; break;
        }
    }
    if (!w || !h || !offs || !counts || (bps != 8 && bps != 16 && bps != 1) || spp < 1 || spp > 4)
        return E_INVAL;
    if (comp != 1 && comp != 5 && comp != 8 && comp != 32946 && comp != 32773)
        return E_INVAL;
    size_t bpp_bits = (size_t)bps * spp, row = (w * bpp_bits + 7) / 8, total = row * h;
    if ((uint64_t)w * h > PICTURE_MAX_PIXELS)
        return E_INVAL;
    uint8_t *raw = kzalloc(total + 16);
    if (!raw)
        return E_NOMEM;
    size_t at = 0;
    rows = MIN(rows, h);
    for (uint32_t s = 0; s < strips && at < total; s++) {
        size_t o = tag_value(&t, offs, s), c = tag_value(&t, counts, s);
        if (o >= size)
            break;
        c = MIN(c, size - o);
        size_t want = MIN(total - at, row * rows);
        if (comp == 1) {
            memcpy(raw + at, d + o, MIN(c, want));
        } else if (comp == 5) {
            lzw_tiff(d + o, c, raw + at, want);
        } else if (comp == 32773) {
            packbits(d + o, c, raw + at, want);
        } else {
            uint8_t *z;
            size_t zl;
            if (zlib_inflate_alloc(d + o, c, want + 64, &z, &zl) == 0) {
                memcpy(raw + at, z, MIN(zl, want));
                kfree(z);
            }
        }
        at += want;
    }
    uint32_t *px = new_pixels(out, w, h);
    if (!px) {
        kfree(raw);
        return E_NOMEM;
    }
    int step = bps == 16 ? 2 : 1, hi = bps == 16 && !t.be ? 1 : 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        if (pred == 2 && bps == 8)
            for (size_t i = spp; i < (size_t)w * spp; i++)
                r[i] = (uint8_t)(r[i] + r[i - spp]);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v[4] = {0, 0, 0, 255};
            if (bps == 1) {
                int bit = (r[x / 8] >> (7 - x % 8)) & 1;
                v[0] = (uint32_t)((photo == 0 ? !bit : bit) ? 255 : 0);
            } else {
                for (uint32_t k = 0; k < spp; k++)
                    v[k] = r[((size_t)x * spp + k) * (size_t)step + (size_t)hi];
            }
            uint32_t c;
            if (photo == 2 && spp >= 3)
                c = argb(spp == 4 && extra ? v[3] : 255, v[0], v[1], v[2]);
            else if (photo == 3 && cmap) {
                uint32_t i = v[0], ncol = 1u << bps;
                c = argb(255, tag_value(&t, cmap, i) >> 8, tag_value(&t, cmap, ncol + i) >> 8,
                         tag_value(&t, cmap, 2 * ncol + i) >> 8);
            } else {
                uint32_t g = photo == 0 && bps != 1 ? 255 - v[0] : v[0];
                c = argb(spp == 2 ? v[1] : 255, g, g, g);
            }
            px[(size_t)y * w + x] = c;
        }
    }
    kfree(raw);
    return 0;
}

/* ---- Targa ---- */

static int tga_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 18)
        return E_INVAL;
    int idlen = d[0], cmtype = d[1], type = d[2], bpp = d[16], desc = d[17];
    int cmfirst = d[3] | d[4] << 8, cmlen = d[5] | d[6] << 8, cmbits = d[7];
    uint32_t w = (uint32_t)(d[12] | d[13] << 8), h = (uint32_t)(d[14] | d[15] << 8);
    bool rle = type >= 9;
    int base = rle ? type - 8 : type;
    if ((base != 1 && base != 2 && base != 3) || cmtype > 1 || (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
        return E_INVAL;
    size_t p = 18 + (size_t)idlen, cmap = p;
    if (cmtype)
        p += (size_t)cmlen * (size_t)((cmbits + 7) / 8);
    uint32_t *px = new_pixels(out, w, h);
    if (!px || p > size)
        return px ? (picture_free(out), E_INVAL) : E_NOMEM;
    int bytes = (bpp + 7) / 8;
    size_t count = (size_t)w * h;
    for (size_t i = 0; i < count && p < size;) { /* a file cut short leaves the rest of the picture empty */
        int run = 1;
        bool repeat = false;
        if (rle) {
            if (p >= size)
                break;
            uint8_t hd = d[p++];
            run = (hd & 0x7f) + 1;
            repeat = hd & 0x80;
        }
        for (int k = 0; k < run && i < count; k++, i++) {
            if (p + (size_t)bytes > size) {
                p = size;
                break;
            }
            const uint8_t *s = d + p;
            uint32_t c;
            if (base == 1) {
                size_t e = cmap + (size_t)(s[0] - cmfirst) * (size_t)((cmbits + 7) / 8);
                c = s[0] >= cmfirst && s[0] - cmfirst < cmlen && e + 3 <= size ? argb(cmbits == 32 && e + 4 <= size ? d[e + 3] : 255, d[e + 2], d[e + 1], d[e])
                                  : 0xff000000;
            } else if (base == 3) {
                c = argb(255, s[0], s[0], s[0]);
            } else if (bytes == 2) {
                uint32_t v = (uint32_t)(s[0] | s[1] << 8);
                c = argb(255, ((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31);
            } else {
                c = argb(bytes == 4 ? s[3] : 255, s[2], s[1], s[0]);
            }
            size_t x = i % w, y = i / w;
            if (!(desc & 0x20))
                y = h - 1 - y;
            if (desc & 0x10)
                x = w - 1 - x;
            px[y * w + x] = c;
            if (!repeat || k == run - 1)
                p += (size_t)bytes;
        }
    }
    return 0;
}

/* ---- PNM (P1..P6) ---- */

static long pnm_number(const uint8_t *d, size_t size, size_t *p)
{
    while (*p < size) {
        if (d[*p] == '#')
            while (*p < size && d[*p] != '\n')
                (*p)++;
        else if (d[*p] == ' ' || d[*p] == '\n' || d[*p] == '\r' || d[*p] == '\t')
            (*p)++;
        else
            break;
    }
    long v = -1;
    while (*p < size && d[*p] >= '0' && d[*p] <= '9')
        v = (v < 0 ? 0 : v * 10) + (d[(*p)++] - '0');
    return v;
}

static int pnm_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 8 || d[0] != 'P' || d[1] < '1' || d[1] > '6')
        return E_INVAL;
    int kind = d[1] - '0';
    size_t p = 2;
    long w = pnm_number(d, size, &p), h = pnm_number(d, size, &p);
    long max = kind == 1 || kind == 4 ? 1 : pnm_number(d, size, &p);
    if (w <= 0 || h <= 0 || max <= 0 || max > 65535)
        return E_INVAL;
    p++;
    uint32_t *px = new_pixels(out, (uint32_t)w, (uint32_t)h);
    if (!px)
        return E_NOMEM;
    int comps = kind == 3 || kind == 6 ? 3 : 1, wide = max > 255 ? 2 : 1;
    for (long y = 0; y < h; y++)
        for (long x = 0; x < w; x++) {
            uint32_t v[3] = {0, 0, 0};
            for (int k = 0; k < comps; k++) {
                long s;
                if (kind <= 3) {
                    s = pnm_number(d, size, &p);
                } else if (kind == 4) {
                    size_t at = p + (size_t)y * (size_t)((w + 7) / 8) + (size_t)x / 8;
                    s = at < size ? (d[at] >> (7 - x % 8)) & 1 : 0;
                } else {
                    size_t at = p + ((size_t)(y * w + x) * (size_t)comps + (size_t)k) * (size_t)wide;
                    s = at + (size_t)wide <= size ? (wide == 2 ? d[at] << 8 | d[at + 1] : d[at]) : 0;
                }
                if (kind == 1 || kind == 4)
                    s = s ? 0 : max;
                v[k] = (uint32_t)(CLAMP(s, 0, max) * 255 / max);
            }
            px[y * w + x] = comps == 3 ? argb(255, v[0], v[1], v[2]) : argb(255, v[0], v[0], v[0]);
        }
    return 0;
}

/* ---- QOI ---- */

static int qoi_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 22 || memcmp(d, "qoif", 4))
        return E_INVAL;
    uint32_t w = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
    uint32_t h = (uint32_t)d[8] << 24 | (uint32_t)d[9] << 16 | (uint32_t)d[10] << 8 | d[11];
    uint32_t *px = new_pixels(out, w, h);
    if (!px)
        return E_NOMEM;
    uint8_t seen[64][4];
    memset(seen, 0, sizeof(seen));
    uint8_t r = 0, g = 0, b = 0, a = 255;
    size_t p = 14, n = (size_t)w * h;
    for (size_t i = 0; i < n;) {
        int run = 0;
        if (p >= size)
            break;
        uint8_t c = d[p++];
        if (c == 0xfe && p + 3 <= size) {
            r = d[p];
            g = d[p + 1];
            b = d[p + 2];
            p += 3;
        } else if (c == 0xff && p + 4 <= size) {
            r = d[p];
            g = d[p + 1];
            b = d[p + 2];
            a = d[p + 3];
            p += 4;
        } else if ((c >> 6) == 0) {
            r = seen[c][0];
            g = seen[c][1];
            b = seen[c][2];
            a = seen[c][3];
        } else if ((c >> 6) == 1) {
            r = (uint8_t)(r + ((c >> 4) & 3) - 2);
            g = (uint8_t)(g + ((c >> 2) & 3) - 2);
            b = (uint8_t)(b + (c & 3) - 2);
        } else if ((c >> 6) == 2 && p < size) {
            int dg = (c & 63) - 32, n2 = d[p++];
            r = (uint8_t)(r + dg - 8 + (n2 >> 4));
            g = (uint8_t)(g + dg);
            b = (uint8_t)(b + dg - 8 + (n2 & 15));
        } else {
            run = c & 63;
        }
        int slot = (r * 3 + g * 5 + b * 7 + a * 11) % 64;
        seen[slot][0] = r;
        seen[slot][1] = g;
        seen[slot][2] = b;
        seen[slot][3] = a;
        for (int k = 0; k <= run && i < n; k++)
            px[i++] = argb(a, r, g, b);
    }
    return 0;
}

/* ---- ICO / CUR: the biggest picture inside ---- */

static int ico_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 22 || d[0] || d[1] || (d[2] != 1 && d[2] != 2) || d[3])
        return E_INVAL;
    int count = d[4] | d[5] << 8, best = -1, best_area = -1;
    for (int i = 0; i < count && 6 + (size_t)(i + 1) * 16 <= size; i++) {
        const uint8_t *e = d + 6 + i * 16;
        int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, bpp = e[6] | e[7] << 8;
        int area = w * h * 64 + bpp;
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    if (best < 0)
        return E_INVAL;
    const uint8_t *e = d + 6 + best * 16;
    size_t len = (size_t)(e[8] | e[9] << 8 | e[10] << 16) | (size_t)e[11] << 24;
    size_t off = (size_t)(e[12] | e[13] << 8 | e[14] << 16) | (size_t)e[15] << 24;
    if (off >= size)
        return E_INVAL;
    len = MIN(len, size - off);
    const uint8_t *p = d + off;
    if (len >= 8 && !memcmp(p, "\x89PNG", 4))
        return png_decode(p, len, out);
    if (len < 40)
        return E_INVAL;
    int32_t w = (int32_t)(p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24);
    int32_t h2 = (int32_t)(p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24);
    int bpp = p[14] | p[15] << 8, hsize = p[0] | p[1] << 8;
    int32_t h = h2 / 2;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1))
        return E_INVAL;
    int colors = bpp <= 8 ? 1 << bpp : 0;
    const uint8_t *pal = p + hsize, *bits = pal + colors * 4;
    size_t row = ((size_t)w * (size_t)bpp + 31) / 32 * 4, mrow = ((size_t)w + 31) / 32 * 4;
    const uint8_t *mask = bits + row * (size_t)h;
    if ((size_t)(mask + mrow * (size_t)h - p) > len)
        return E_INVAL;
    uint32_t *px = new_pixels(out, (uint32_t)w, (uint32_t)h);
    if (!px)
        return E_NOMEM;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *r = bits + row * (size_t)(h - 1 - y), *m = mask + mrow * (size_t)(h - 1 - y);
        for (int32_t x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 32) {
                c = argb(r[x * 4 + 3], r[x * 4 + 2], r[x * 4 + 1], r[x * 4]);
            } else if (bpp == 24) {
                c = argb(255, r[x * 3 + 2], r[x * 3 + 1], r[x * 3]);
            } else {
                int i = bpp == 8 ? r[x] : bpp == 4 ? (r[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (r[x / 8] >> (7 - x % 8)) & 1;
                c = argb(255, pal[i * 4 + 2], pal[i * 4 + 1], pal[i * 4]);
            }
            if (bpp != 32 && ((m[x / 8] >> (7 - x % 8)) & 1))
                c = 0;
            px[y * w + x] = c;
        }
    }
    return 0;
}

static int picture_any(const uint8_t *d, size_t size, struct picture *out)
{
    if (size >= 4 && ((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')))
        return tiff_decode(d, size, out);
    if (size >= 4 && !memcmp(d, "qoif", 4))
        return qoi_decode(d, size, out);
    if (size >= 2 && d[0] == 'P' && d[1] >= '1' && d[1] <= '6')
        return pnm_decode(d, size, out);
    if (size >= 6 && !d[0] && !d[1] && (d[2] == 1 || d[2] == 2) && !d[3] && ico_decode(d, size, out) == 0)
        return 0; /* an uncompressed Targa header can look the same */
    if (size >= 18 && d[1] <= 1 && ((d[2] >= 1 && d[2] <= 3) || (d[2] >= 9 && d[2] <= 11)))
        return tga_decode(d, size, out);
    return E_INVAL;
}

/* ---- FLAC ---- */

struct bits {
    const uint8_t *d;
    size_t size, pos; /* in bits */
};

static uint32_t get(struct bits *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->pos++)
        v = v << 1 | (b->pos / 8 < b->size ? (b->d[b->pos / 8] >> (7 - b->pos % 8)) & 1 : 0);
    return v;
}

static int32_t sget(struct bits *b, int n)
{
    if (!n)
        return 0;
    uint32_t v = get(b, n);
    return (int32_t)(v << (32 - n)) >> (32 - n);
}

static uint32_t unary(struct bits *b)
{
    uint32_t n = 0;
    while (b->pos / 8 < b->size && !get(b, 1))
        n++;
    return n;
}

static bool residual(struct bits *b, int32_t *s, int n, int order)
{
    int method = (int)get(b, 2);
    if (method > 1)
        return false;
    int pbits = (int)get(b, 4), parts = 1 << pbits, rbits = method ? 5 : 4, esc = method ? 31 : 15;
    int at = order;
    for (int p = 0; p < parts; p++) {
        int count = (n >> pbits) - (p == 0 ? order : 0);
        if (count < 0)
            return false;
        int k = (int)get(b, rbits);
        if (k == esc) {
            int nb = (int)get(b, 5);
            for (int i = 0; i < count && at < n; i++)
                s[at++] = sget(b, nb);
        } else {
            for (int i = 0; i < count && at < n; i++) {
                uint32_t q = unary(b), v = q << k | get(b, k);
                s[at++] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
            }
        }
    }
    return true;
}

static bool subframe(struct bits *b, int32_t *s, int n, int bps)
{
    get(b, 1);
    int type = (int)get(b, 6), wasted = 0;
    if (get(b, 1)) {
        wasted = 1;
        while (!get(b, 1) && wasted < 32) /* past the end of the file every bit reads 0 */
            wasted++;
        bps -= wasted;
    }
    if (bps < 1 || bps > 32)
        return false;
    if (type == 0) {
        int32_t v = sget(b, bps);
        for (int i = 0; i < n; i++)
            s[i] = v;
    } else if (type == 1) {
        for (int i = 0; i < n; i++)
            s[i] = sget(b, bps);
    } else if (type >= 8 && type <= 12) {
        int order = type - 8;
        for (int i = 0; i < order && i < n; i++)
            s[i] = sget(b, bps);
        if (!residual(b, s, n, order))
            return false;
        for (int i = order; i < n; i++) {
            int32_t p = order == 0 ? 0 : order == 1 ? s[i - 1] : order == 2 ? 2 * s[i - 1] - s[i - 2]
                        : order == 3 ? 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]
                                     : 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4];
            s[i] += p;
        }
    } else if (type >= 32) {
        int order = type - 31;
        if (order > n)
            return false;
        for (int i = 0; i < order; i++)
            s[i] = sget(b, bps);
        int prec = (int)get(b, 4) + 1;
        int shift = sget(b, 5);
        int32_t coef[32];
        for (int i = 0; i < order; i++)
            coef[i] = sget(b, prec);
        if (!residual(b, s, n, order) || shift < 0)
            return false;
        for (int i = order; i < n; i++) {
            int64_t sum = 0;
            for (int k = 0; k < order; k++)
                sum += (int64_t)coef[k] * s[i - 1 - k];
            s[i] += (int32_t)(sum >> shift);
        }
    } else {
        return false;
    }
    if (wasted)
        for (int i = 0; i < n; i++)
            s[i] <<= wasted;
    return true;
}

static int flac_decode(const uint8_t *d, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    if (size < 42 || memcmp(d, "fLaC", 4))
        return E_INVAL;
    size_t p = 4;
    uint32_t sample_rate = 0, channels = 0, bps = 0;
    uint64_t total = 0;
    for (bool last = false; !last && p + 4 <= size;) {
        last = d[p] & 0x80;
        int type = d[p] & 0x7f;
        size_t len = (size_t)d[p + 1] << 16 | (size_t)d[p + 2] << 8 | d[p + 3];
        if (type == 0 && p + 4 + 18 <= size) {
            const uint8_t *s = d + p + 4;
            sample_rate = (uint32_t)s[10] << 12 | (uint32_t)s[11] << 4 | s[12] >> 4;
            channels = ((s[12] >> 1) & 7) + 1;
            bps = (uint32_t)((s[12] & 1) << 4 | s[13] >> 4) + 1;
            total = (uint64_t)(s[13] & 15) << 32 | (uint32_t)s[14] << 24 | (uint32_t)s[15] << 16 | (uint32_t)s[16] << 8 | s[17];
        }
        p += 4 + len;
    }
    if (!sample_rate || channels > 8 || bps > 32 || !total || total > 48000u * 60 * 30)
        return E_INVAL;
    int16_t *out = kmalloc((size_t)total * 4 + 4);
    int32_t *ch[8] = {0};
    for (uint32_t c = 0; c < channels; c++)
        ch[c] = kmalloc(65536 * sizeof(int32_t));
    for (uint32_t c = 0; c < channels; c++)
        if (!ch[c])
            out = (kfree(out), NULL);
    if (!out) {
        for (uint32_t c = 0; c < channels; c++)
            kfree(ch[c]);
        return E_NOMEM;
    }
    uint64_t done = 0;
    struct bits b = {d, size, p * 8};
    while (done < total && b.pos / 8 + 4 < size) {
        size_t at = b.pos / 8;
        if (d[at] != 0xff || (d[at + 1] & 0xfe) != 0xf8) {
            b.pos = (at + 1) * 8;
            continue;
        }
        b.pos += 16;
        int bs_code = (int)get(&b, 4), sr_code = (int)get(&b, 4), ch_code = (int)get(&b, 4), ss_code = (int)get(&b, 3);
        get(&b, 1);
        uint32_t lead = get(&b, 8);
        int extra = 0;
        while (lead & (0x80 >> extra) && extra < 7)
            extra++;
        if (extra > 1)
            get(&b, 8 * (extra - 1));
        int n = bs_code == 1 ? 192 : bs_code <= 5 ? 576 << (bs_code - 2) : bs_code == 6 ? (int)get(&b, 8) + 1
                : bs_code == 7 ? (int)get(&b, 16) + 1 : 256 << (bs_code - 8);
        if (sr_code == 12)
            get(&b, 8);
        else if (sr_code == 13 || sr_code == 14)
            get(&b, 16);
        get(&b, 8); /* CRC-8 */
        int sbps = ss_code == 1 ? 8 : ss_code == 2 ? 12 : ss_code == 4 ? 16 : ss_code == 5 ? 20 : ss_code == 6 ? 24 : (int)bps;
        int nch = ch_code < 8 ? ch_code + 1 : 2;
        if (n > 65536 || n <= 0 || nch > (int)channels)
            break;
        bool ok = true;
        for (int c = 0; c < nch && ok; c++) {
            int extra_bit = (ch_code == 8 && c == 1) || (ch_code == 9 && c == 0) || (ch_code == 10 && c == 1);
            ok = subframe(&b, ch[c], n, sbps + extra_bit);
        }
        if (!ok)
            break;
        if (ch_code == 8)
            for (int i = 0; i < n; i++)
                ch[1][i] = ch[0][i] - ch[1][i];
        else if (ch_code == 9)
            for (int i = 0; i < n; i++)
                ch[0][i] += ch[1][i];
        else if (ch_code == 10)
            for (int i = 0; i < n; i++) {
                int32_t mid = ch[0][i] << 1 | (ch[1][i] & 1), side = ch[1][i];
                ch[0][i] = (mid + side) >> 1;
                ch[1][i] = (mid - side) >> 1;
            }
        int shift = sbps - 16;
        for (int i = 0; i < n && done < total; i++, done++)
            for (int c = 0; c < 2; c++) {
                int32_t v = ch[MIN(c, nch - 1)][i];
                v = shift >= 0 ? v >> shift : v << -shift;
                out[done * 2 + (uint64_t)c] = (int16_t)CLAMP(v, -32768, 32767);
            }
        b.pos = (b.pos + 7) / 8 * 8 + 16; /* CRC-16 */
    }
    for (uint32_t c = 0; c < channels; c++)
        kfree(ch[c]);
    if (!done) {
        kfree(out);
        return E_INVAL;
    }
    *frames = out;
    *count = (uint32_t)done;
    *rate = sample_rate;
    return 0;
}

/* ---- AIFF: big-endian PCM ---- */

static int aiff_decode(const uint8_t *d, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    if (size < 12 || memcmp(d, "FORM", 4) || (memcmp(d + 8, "AIFF", 4) && memcmp(d + 8, "AIFC", 4)))
        return E_INVAL;
    int channels = 0, bits = 0;
    uint32_t sr = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    for (size_t p = 12; p + 8 <= size;) {
        size_t len = (size_t)d[p + 4] << 24 | (size_t)d[p + 5] << 16 | (size_t)d[p + 6] << 8 | d[p + 7];
        if (!memcmp(d + p, "COMM", 4) && p + 26 <= size) {
            channels = d[p + 8] << 8 | d[p + 9];
            bits = d[p + 14] << 8 | d[p + 15];
            int exp = ((d[p + 16] & 0x7f) << 8 | d[p + 17]) - 16383;
            uint32_t mant = (uint32_t)d[p + 18] << 24 | (uint32_t)d[p + 19] << 16 | (uint32_t)d[p + 20] << 8 | d[p + 21];
            sr = exp >= 0 && exp < 32 ? mant >> (31 - exp) : 0;
        } else if (!memcmp(d + p, "SSND", 4) && p + 16 <= size) {
            size_t off = (size_t)d[p + 8] << 24 | (size_t)d[p + 9] << 16 | (size_t)d[p + 10] << 8 | d[p + 11];
            /* the samples start off bytes into the chunk: inside the chunk and inside the file, or not at all */
            if (len >= 8 && off <= len - 8 && off <= size - (p + 16)) {
                pcm = d + p + 16 + off;
                pcm_len = MIN(len - 8 - off, size - (size_t)(pcm - d));
            }
        }
        if (len > size - p - 8) /* the last chunk (and no wrapping around on the 32-bit system) */
            break;
        p += 8 + len + (len & 1);
    }
    int bytes = (bits + 7) / 8;
    if (!pcm || channels < 1 || channels > 8 || bytes < 1 || bytes > 4 || sr < 4000 || sr > 192000)
        return E_INVAL;
    uint32_t n = (uint32_t)(pcm_len / (size_t)(channels * bytes));
    int16_t *out = kmalloc((size_t)n * 4 + 4);
    if (!out)
        return E_NOMEM;
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            const uint8_t *s = pcm + ((size_t)i * (size_t)channels + (size_t)MIN(c, channels - 1)) * (size_t)bytes;
            out[i * 2 + (uint32_t)c] = bytes == 1 ? (int16_t)((int8_t)s[0] << 8) : (int16_t)(s[0] << 8 | s[1]);
        }
    *frames = out;
    *count = n;
    *rate = sr;
    return 0;
}

static int audio_any(const uint8_t *d, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    if (size >= 4 && !memcmp(d, "fLaC", 4))
        return flac_decode(d, size, frames, count, rate);
    if (size >= 4 && !memcmp(d, "FORM", 4))
        return aiff_decode(d, size, frames, count, rate);
    return E_INVAL;
}

static const struct module_ext ext = {NULL, 0, ".tif .tiff .tga .ppm .pgm .pbm .pnm .qoi .ico .cur", picture_any,
                                      ".flac .aif .aiff .aifc", audio_any, NULL, NULL, NULL};
GUI_APP_FULL(NULL, NULL, &ext);
