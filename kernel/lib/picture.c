#include <synth/error.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/vfs.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int bmp_decode(const uint8_t *d, size_t size, struct picture *out)
{
    if (size < 54 || d[0] != 'B' || d[1] != 'M')
        return E_INVAL;
    uint32_t offset = rd32(d + 10), header = rd32(d + 14);
    int32_t w = (int32_t)rd32(d + 18), h = (int32_t)rd32(d + 22);
    int bpp = d[28] | d[29] << 8;
    uint32_t compression = rd32(d + 30), colors = rd32(d + 46);
    bool flip = h > 0;
    if (h < 0)
        h = -h;
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > PICTURE_MAX_PIXELS || (compression != 0 && compression != 3))
        return E_INVAL;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return E_INVAL;
    uint32_t row = ((uint32_t)w * (uint32_t)bpp + 31) / 32 * 4;
    if (offset > size || (uint64_t)row * (uint64_t)h > size - offset)
        return E_INVAL;
    const uint8_t *pal = d + 14 + header;
    if (!colors)
        colors = bpp <= 8 ? 1u << bpp : 0;
    if (bpp <= 8 && (size_t)(pal - d) + colors * 4 > size)
        return E_INVAL;
    out->px = kmalloc((size_t)w * (size_t)h * 4);
    if (!out->px)
        return E_NOMEM;
    out->w = w;
    out->h = h;
    out->frames = 1;
    out->delays = NULL;
    bool alpha = bpp == 32 && compression == 0 && header >= 40;
    bool any_alpha = false;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = d + offset + row * (uint32_t)(flip ? h - 1 - y : y);
        uint32_t *dst = out->px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp <= 8) {
                int per = 8 / bpp, shift = 8 - bpp * (x % per + 1);
                uint32_t i = (uint32_t)(src[x / per] >> shift) & ((1u << bpp) - 1);
                const uint8_t *p = pal + MIN(i, colors ? colors - 1 : 0) * 4;
                c = 0xff000000u | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
            } else if (bpp == 16) {
                unsigned v = src[x * 2] | src[x * 2 + 1] << 8;
                unsigned r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                c = 0xff000000u | (r * 255 / 31) << 16 | (g * 255 / 31) << 8 | b * 255 / 31;
            } else {
                const uint8_t *p = src + x * (bpp / 8);
                uint32_t a = alpha ? p[3] : 255;
                any_alpha |= alpha && a;
                c = a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
            }
            dst[x] = c;
        }
    }
    if (alpha && !any_alpha)
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            out->px[i] |= 0xff000000u;
    return 0;
}

int picture_decode(const uint8_t *data, size_t size, struct picture *out)
{
    memset(out, 0, sizeof(*out));
    if (size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8))
        return png_decode(data, size, out);
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8)
        return jpeg_decode(data, size, out);
    if (size >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6)))
        return gif_decode(data, size, out);
    if (size >= 2 && data[0] == 'B' && data[1] == 'M')
        return bmp_decode(data, size, out);
    return E_INVAL;
}

int picture_load(const char *path, struct picture *out)
{
    char *data;
    size_t size;
    memset(out, 0, sizeof(*out));
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    err = picture_decode((const uint8_t *)data, size, out);
    kfree(data);
    return err;
}

void picture_free(struct picture *p)
{
    kfree(p->px);
    kfree(p->delays);
    memset(p, 0, sizeof(*p));
}

bool picture_is_image(const char *name)
{
    const char *ext = path_ext(name);
    static const char *const exts[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp"};
    for (size_t i = 0; ext && i < ARRAY_SIZE(exts); i++)
        if (!str_casecmp(ext, exts[i]))
            return true;
    return false;
}
