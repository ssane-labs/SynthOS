#include <synth/error.h>
#include <synth/gui.h>
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

/* The icon of a Windows program: the first icon group in its resources, the largest 32-bit image up to `want`. */
static const uint8_t *resource_child(const uint8_t *rsrc, uint32_t size, uint32_t dir, uint32_t id, bool any)
{
    if (dir + 16 > size)
        return NULL;
    uint32_t count = (uint32_t)(rsrc[dir + 12] | rsrc[dir + 13] << 8) + (uint32_t)(rsrc[dir + 14] | rsrc[dir + 15] << 8);
    for (uint32_t i = 0; i < count && dir + 24 + i * 8 <= size; i++) {
        const uint8_t *e = rsrc + dir + 16 + i * 8;
        if (any || (!(rd32(e) & 0x80000000u) && rd32(e) == id))
            return e;
    }
    return NULL;
}

static const uint8_t *resource_find(const uint8_t *rsrc, uint32_t size, uint32_t rva, uint32_t type, uint32_t id,
                                    bool any, uint32_t *len)
{
    const uint8_t *e = resource_child(rsrc, size, 0, type, false);
    for (int level = 0; e && level < 2; level++) {
        if (!(rd32(e + 4) & 0x80000000u))
            return NULL;
        e = resource_child(rsrc, size, rd32(e + 4) & 0x7fffffffu, id, level == 1 || any);
    }
    if (!e || (rd32(e + 4) & 0x80000000u) || rd32(e + 4) + 16 > size)
        return NULL;
    const uint8_t *leaf = rsrc + rd32(e + 4);
    uint32_t at = rd32(leaf) - rva;
    *len = rd32(leaf + 4);
    return at < size && *len <= size - at ? rsrc + at : NULL;
}

bool image_load_exe_icon(const char *path, int want, struct image *out);

bool gui_program_icon(const char *path, int size, struct canvas *out)
{
    struct image img;
    if (!image_load_exe_icon(path, size, &img))
        return false;
    *out = (struct canvas){img.px, img.w, img.h, img.w, {0, 0, img.w, img.h}};
    return true;
}

bool image_load_exe_icon(const char *path, int want, struct image *out)
{
    struct file *f;
    if (vfs_open(path, VFS_READ, &f) != 0)
        return false;
    uint8_t head[1024];
    long n = vfs_read(f, head, sizeof(head));
    uint8_t *rsrc = NULL;
    uint32_t size = 0, rva = 0;
    uint32_t pe = n >= 64 ? rd32(head + 0x3c) : 0;
    if (pe && pe + 24 < (uint32_t)n && !memcmp(head + pe, "PE\0\0", 4)) {
        uint32_t opt = pe + 24, sections = (uint32_t)(head[pe + 6] | head[pe + 7] << 8);
        bool pe64 = head[opt] == 0x0b && head[opt + 1] == 0x02;
        uint32_t dirs = opt + (pe64 ? 112 : 96);
        rva = dirs + 24 <= (uint32_t)n ? rd32(head + dirs + 16) : 0;
        uint32_t sec = opt + (uint32_t)(head[pe + 20] | head[pe + 21] << 8);
        for (uint32_t i = 0; rva && i < sections && sec + 40 * i + 40 <= (uint32_t)n; i++) {
            const uint8_t *s = head + sec + 40 * i;
            uint32_t va = rd32(s + 12), raw = rd32(s + 16), ptr = rd32(s + 20);
            if (rva < va || rva >= va + raw || raw > (4u << 20))
                continue;
            size = raw - (rva - va);
            if ((rsrc = kmalloc(size)) && (vfs_seek(f, (long)(ptr + rva - va), VFS_SEEK_SET), true) &&
                vfs_read(f, rsrc, size) != (long)size) {
                kfree(rsrc);
                rsrc = NULL;
            }
            break;
        }
    }
    vfs_close(f);
    if (!rsrc)
        return false;
    bool ok = false;
    uint32_t glen, ilen = 0;
    const uint8_t *group = resource_find(rsrc, size, rva, 14, 0, true, &glen), *best = NULL;
    int best_w = 0;
    for (uint32_t i = 0; group && glen >= 6 && i < (uint32_t)(group[4] | group[5] << 8) && 6 + i * 14 + 14 <= glen; i++) {
        const uint8_t *g = group + 6 + i * 14;
        int w = g[0] ? g[0] : 256, bpp = g[6] | g[7] << 8;
        uint32_t len;
        const uint8_t *icon = resource_find(rsrc, size, rva, 3, (uint32_t)(g[12] | g[13] << 8), false, &len);
        if (!icon || bpp != 32 || len < 40 || rd32(icon) != 40 || w > want * 2)
            continue;
        if (!best || (best_w < want ? w > best_w : (w >= want && w < best_w))) {
            best = icon;
            best_w = w;
            ilen = len;
        }
    }
    if (best) {
        int w = (int)rd32(best + 4), h = (int)rd32(best + 8) / 2;
        if (w > 0 && w == h && w <= 256 && 40 + (uint32_t)(w * h * 4) <= ilen && (out->px = kmalloc((size_t)w * (size_t)h * 4))) {
            out->w = w;
            out->h = h;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = best + 40 + ((h - 1 - y) * w + x) * 4;
                    out->px[y * w + x] = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
                }
            ok = true;
        }
    }
    kfree(rsrc);
    return ok;
}
