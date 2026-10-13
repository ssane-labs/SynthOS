#include <synth/error.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/string.h>
#include <synth/zip.h>

static uint32_t crc_table[256];

uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t rd16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int zip_each(const uint8_t *data, size_t len, int (*fn)(const struct zip_entry *e, void *ctx), void *ctx)
{
    if (len < 22)
        return E_INVAL;
    size_t end = len - 22, stop = len > 65557 ? len - 65557 : 0;
    while (rd32(data + end) != 0x06054b50u) {
        if (end == stop)
            return E_INVAL;
        end--;
    }
    uint32_t count = rd16(data + end + 10), dir = rd32(data + end + 16);
    size_t pos = dir;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 46 > len || rd32(data + pos) != 0x02014b50u)
            return E_INVAL;
        struct zip_entry e = {
            .name = (const char *)data + pos + 46,
            .name_len = rd16(data + pos + 28),
            .method = rd16(data + pos + 10),
            .crc = rd32(data + pos + 16),
            .csize = rd32(data + pos + 20),
            .usize = rd32(data + pos + 24),
            .local = rd32(data + pos + 42),
            .attributes = rd32(data + pos + 38),
        };
        if (pos + 46 + e.name_len > len)
            return E_INVAL;
        int err = fn(&e, ctx);
        if (err)
            return err;
        pos += 46 + e.name_len + rd16(data + pos + 30) + rd16(data + pos + 32);
    }
    return 0;
}

int zip_extract(const uint8_t *data, size_t len, const struct zip_entry *e, uint8_t **out)
{
    *out = NULL;
    if ((size_t)e->local + 30 > len || rd32(data + e->local) != 0x04034b50u)
        return E_INVAL;
    size_t start = e->local + 30 + rd16(data + e->local + 26) + rd16(data + e->local + 28);
    if (start + e->csize > len)
        return E_INVAL;
    uint8_t *buf;
    if (e->method == 0) {
        buf = kmalloc(MAX(e->usize, 1u));
        if (!buf)
            return E_NOMEM;
        memcpy(buf, data + start, e->usize);
    } else if (e->method == 8) {
        buf = kmalloc(MAX(e->usize, 1u));
        if (!buf)
            return E_NOMEM;
        size_t got;
        int err = inflate_into(data + start, e->csize, buf, e->usize, &got, NULL);
        if (err || got != e->usize) {
            kfree(buf);
            return err ? err : E_INVAL;
        }
    } else {
        return E_NOSYS;
    }
    if (crc32_update(0, buf, e->usize) != e->crc) {
        kfree(buf);
        return E_IO;
    }
    *out = buf;
    return 0;
}
