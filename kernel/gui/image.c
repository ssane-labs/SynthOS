#include <synth/error.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "image.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool image_load(const char *path, struct image *out)
{
    char *data;
    size_t size;
    if (vfs_read_all(path, &data, &size) != 0)
        return false;
    const uint8_t *d = (const uint8_t *)data;
    bool ok = false;
    if (size >= 54 && d[0] == 'B' && d[1] == 'M') {
        uint32_t offset = rd32(d + 10);
        int32_t w = (int32_t)rd32(d + 18), h = (int32_t)rd32(d + 22);
        uint16_t bpp = (uint16_t)(d[28] | d[29] << 8);
        uint32_t compression = rd32(d + 30);
        bool flip = h > 0;
        if (h < 0)
            h = -h;
        uint32_t row = ((uint32_t)w * bpp / 8 + 3) & ~3u;
        if (w > 0 && h > 0 && w <= 8192 && h <= 8192 && (bpp == 24 || bpp == 32) && (compression == 0 || compression == 3) &&
            offset + row * (uint32_t)h <= size && (out->px = kmalloc((size_t)w * (size_t)h * 4))) {
            out->w = w;
            out->h = h;
            for (int y = 0; y < h; y++) {
                const uint8_t *src = d + offset + row * (uint32_t)(flip ? h - 1 - y : y);
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = src + x * (bpp / 8);
                    out->px[y * w + x] = 0xff000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
                }
            }
            ok = true;
        }
    }
    kfree(data);
    return ok;
}

int image_save(const char *path, const uint32_t *px, int w, int h, int stride)
{
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h;
    uint8_t *buf = kzalloc(size);
    if (!buf)
        return E_NOMEM;
    buf[0] = 'B';
    buf[1] = 'M';
    wr32(buf + 2, size);
    wr32(buf + 10, 54);
    wr32(buf + 14, 40);
    wr32(buf + 18, (uint32_t)w);
    wr32(buf + 22, (uint32_t)h);
    buf[26] = 1;
    buf[28] = 24;
    wr32(buf + 34, row * (uint32_t)h);
    for (int y = 0; y < h; y++) {
        uint8_t *dst = buf + 54 + row * (uint32_t)(h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * stride + x];
            dst[x * 3] = (uint8_t)p;
            dst[x * 3 + 1] = (uint8_t)(p >> 8);
            dst[x * 3 + 2] = (uint8_t)(p >> 16);
        }
    }
    int err = vfs_write_all(path, buf, size);
    kfree(buf);
    return err;
}

void image_free(struct image *img)
{
    kfree(img->px);
    img->px = NULL;
}
