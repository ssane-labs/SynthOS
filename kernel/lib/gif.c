#include <synth/error.h>
#include <synth/heap.h>
#include <synth/picture.h>
#include <synth/string.h>

#define MAX_FRAMES 256

struct lzw {
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint8_t first[4096];
    uint8_t stack[4097];
};

static int lzw_decode(struct lzw *t, int min_size, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (min_size < 2 || min_size > 11)
        return E_INVAL;
    int clear = 1 << min_size, end = clear + 1, size = min_size + 1, next = clear + 2, prev = -1;
    uint32_t bits = 0;
    int nbits = 0;
    size_t o = 0, i = 0;
    for (int k = 0; k < clear; k++) {
        t->suffix[k] = t->first[k] = (uint8_t)k;
        t->prefix[k] = 0xffff;
    }
    while (o < cap) {
        while (nbits < size && i < len) {
            bits |= (uint32_t)in[i++] << nbits;
            nbits += 8;
        }
        if (nbits < size)
            break;
        int code = (int)(bits & ((1u << size) - 1));
        bits >>= size;
        nbits -= size;
        if (code == clear) {
            size = min_size + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (code == end)
            break;
        int cur = code, sp = 0;
        if (prev >= 0 && code >= next) {
            if (code > next)
                return E_INVAL;
            t->stack[sp++] = t->first[prev];
            cur = prev;
        } else if (code >= next) {
            return E_INVAL;
        }
        while (cur >= clear && sp < 4096) {
            t->stack[sp++] = t->suffix[cur];
            cur = t->prefix[cur];
        }
        t->stack[sp++] = (uint8_t)cur;
        if (prev >= 0 && next < 4096) {
            t->prefix[next] = (uint16_t)prev;
            t->suffix[next] = (uint8_t)cur;
            t->first[next] = t->first[prev];
            next++;
            if (next == 1 << size && size < 12)
                size++;
        }
        while (sp && o < cap)
            out[o++] = t->stack[--sp];
        prev = code;
    }
    return (int)o;
}

static size_t gather(const uint8_t *d, size_t size, size_t off, uint8_t **out, size_t *len)
{
    size_t total = 0, p = off;
    while (p < size && d[p]) {
        total += d[p];
        p += 1 + d[p];
    }
    *out = kmalloc(total + 1);
    if (!*out)
        return 0;
    size_t n = 0;
    p = off;
    while (p < size && d[p]) {
        size_t k = MIN((size_t)d[p], size - p - 1);
        memcpy(*out + n, d + p + 1, k);
        n += k;
        p += 1 + d[p];
    }
    *len = n;
    return p + 1;
}

int gif_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 13)
        return E_INVAL;
    int w = d[6] | d[7] << 8, h = d[8] | d[9] << 8;
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > PICTURE_MAX_PIXELS / 4)
        return E_INVAL;
    uint32_t global[256] = {0};
    size_t off = 13;
    if (d[10] & 0x80) {
        int n = 2 << (d[10] & 7);
        if (off + (size_t)n * 3 > size)
            return E_INVAL;
        for (int i = 0; i < n; i++)
            global[i] = 0xff000000u | (uint32_t)d[off + i * 3] << 16 | (uint32_t)d[off + i * 3 + 1] << 8 | d[off + i * 3 + 2];
        off += (size_t)n * 3;
    }
    size_t frame_px = (size_t)w * (size_t)h;
    uint32_t *canvas = kzalloc(frame_px * 4), *frames = NULL;
    int *delays = NULL, count = 0, delay = 10, transparent = -1, dispose = 0;
    struct lzw *t = kmalloc(sizeof(*t));
    uint8_t *idx = kmalloc(frame_px);
    int err = 0;
    if (!canvas || !t || !idx)
        err = E_NOMEM;
    while (!err && off < size && count < MAX_FRAMES) {
        uint8_t tag = d[off++];
        if (tag == 0x3b)
            break;
        if (tag == 0x21) {
            if (off + 1 > size)
                break;
            uint8_t label = d[off++];
            if (label == 0xf9 && off + 5 <= size && d[off] >= 4) {
                dispose = d[off + 1] >> 2 & 7;
                delay = d[off + 2] | d[off + 3] << 8;
                transparent = d[off + 1] & 1 ? d[off + 4] : -1;
            }
            while (off < size && d[off])
                off += 1 + d[off];
            off++;
            continue;
        }
        if (tag != 0x2c || off + 9 > size) {
            err = count ? 0 : E_INVAL;
            break;
        }
        int fx = d[off] | d[off + 1] << 8, fy = d[off + 2] | d[off + 3] << 8;
        int fw = d[off + 4] | d[off + 5] << 8, fh = d[off + 6] | d[off + 7] << 8;
        uint8_t flags = d[off + 8];
        off += 9;
        uint32_t local[256];
        const uint32_t *pal = global;
        if (flags & 0x80) {
            int n = 2 << (flags & 7);
            if (off + (size_t)n * 3 > size) {
                err = E_INVAL;
                break;
            }
            for (int i = 0; i < n; i++)
                local[i] = 0xff000000u | (uint32_t)d[off + i * 3] << 16 | (uint32_t)d[off + i * 3 + 1] << 8 |
                           d[off + i * 3 + 2];
            off += (size_t)n * 3;
            pal = local;
        }
        if (off >= size) {
            err = E_INVAL;
            break;
        }
        int min = d[off++];
        uint8_t *lz;
        size_t lz_len = 0;
        off = gather(d, size, off, &lz, &lz_len);
        if (!off) {
            err = E_NOMEM;
            break;
        }
        size_t want = (size_t)MAX(fw, 0) * (size_t)MAX(fh, 0);
        int got = want && want <= frame_px ? lzw_decode(t, min, lz, lz_len, idx, want) : 0;
        kfree(lz);
        uint32_t *saved = NULL;
        if (dispose == 3) {
            saved = kmalloc(frame_px * 4);
            if (saved)
                memcpy(saved, canvas, frame_px * 4);
        }
        bool interlaced = flags & 0x40;
        static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
        int row = 0, pass = 0;
        for (int i = 0; i < fh && got > 0; i++) {
            int y = i;
            if (interlaced) {
                while (pass < 4 && start[pass] + row * step[pass] >= fh) {
                    pass++;
                    row = 0;
                }
                y = pass < 4 ? start[pass] + row * step[pass] : i;
                row++;
            }
            for (int x = 0; x < fw; x++) {
                size_t k = (size_t)i * (size_t)fw + (size_t)x;
                if (k >= (size_t)got)
                    break;
                int c = idx[k];
                int px = fx + x, py = fy + y;
                if (c == transparent || px >= w || py >= h)
                    continue;
                canvas[(size_t)py * (size_t)w + (size_t)px] = pal[c];
            }
        }
        uint32_t *grown = krealloc(frames, frame_px * 4 * (size_t)(count + 1));
        int *dl = krealloc(delays, sizeof(int) * (size_t)(count + 1));
        if (!grown || !dl) {
            kfree(saved);
            if (grown)
                frames = grown;
            if (dl)
                delays = dl;
            err = E_NOMEM;
            break;
        }
        frames = grown;
        delays = dl;
        memcpy(frames + frame_px * (size_t)count, canvas, frame_px * 4);
        delays[count++] = MAX(delay, 2) * 10;
        if (dispose == 2) {
            for (int y = fy; y < MIN(fy + fh, h); y++)
                for (int x = fx; x < MIN(fx + fw, w); x++)
                    canvas[(size_t)y * (size_t)w + (size_t)x] = 0;
        } else if (dispose == 3 && saved) {
            memcpy(canvas, saved, frame_px * 4);
        }
        kfree(saved);
        transparent = -1;
        dispose = 0;
        delay = 10;
        if (count * frame_px * 4 > PICTURE_MAX_PIXELS * 4)
            break;
    }
    kfree(canvas);
    kfree(t);
    kfree(idx);
    if (!err && !count)
        err = E_INVAL;
    if (err) {
        kfree(frames);
        kfree(delays);
        return err;
    }
    out->px = frames;
    out->w = w;
    out->h = h;
    out->frames = count;
    out->delays = delays;
    return 0;
}
