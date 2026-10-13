#ifndef SYNTH_ZIP_H
#define SYNTH_ZIP_H

#include <synth/types.h>

struct zip_entry {
    const char *name;
    size_t name_len;
    uint32_t method, crc, csize, usize, local, attributes;
};

uint32_t crc32_update(uint32_t crc, const void *data, size_t n);
int zip_each(const uint8_t *data, size_t len, int (*fn)(const struct zip_entry *e, void *ctx), void *ctx);
int zip_extract(const uint8_t *data, size_t len, const struct zip_entry *e, uint8_t **out);

#endif
