#include <synth/lz4.h>
#include <synth/string.h>

static size_t length_ext(const uint8_t **p, const uint8_t *end, size_t len)
{
    if (len != 15)
        return len;
    uint8_t b;
    do {
        if (*p >= end)
            return len;
        b = *(*p)++;
        len += b;
    } while (b == 255);
    return len;
}

size_t lz4_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    const uint8_t *p = src, *end = src + n;
    size_t o = 0;
    while (p < end) {
        uint8_t token = *p++;
        size_t lit = length_ext(&p, end, token >> 4);
        if (lit > (size_t)(end - p) || lit > cap - o)
            return 0;
        memcpy(dst + o, p, lit);
        p += lit;
        o += lit;
        if (p >= end)
            break;
        if (end - p < 2)
            return 0;
        size_t offset = (size_t)p[0] | (size_t)p[1] << 8;
        p += 2;
        size_t len = length_ext(&p, end, token & 15) + 4;
        if (!offset || offset > o || len > cap - o)
            return 0;
        for (size_t i = 0; i < len; i++, o++)
            dst[o] = dst[o - offset];
    }
    return o;
}

void unfilter_planes(const uint8_t *planes, uint32_t *out, int w, int h)
{
    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; i++)
        out[i] = 0;
    for (int plane = 0; plane < 4; plane++) {
        int shift = 24 - plane * 8;
        const uint8_t *src = planes + count * (size_t)plane;
        for (int y = 0; y < h; y++) {
            uint8_t prev = 0;
            for (int x = 0; x < w; x++) {
                prev = (uint8_t)(prev + src[y * w + x]);
                out[y * w + x] |= (uint32_t)prev << shift;
            }
        }
    }
}

void unfilter_rows(uint8_t *data, int w, int h)
{
    for (int y = 0; y < h; y++) {
        uint8_t prev = 0;
        for (int x = 0; x < w; x++) {
            prev = (uint8_t)(prev + data[y * w + x]);
            data[y * w + x] = prev;
        }
    }
}
