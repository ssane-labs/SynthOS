#ifndef SYNTH_LZ4_H
#define SYNTH_LZ4_H

#include <synth/types.h>

size_t lz4_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);
void unfilter_planes(const uint8_t *planes, uint32_t *out, int w, int h);
void unfilter_rows(uint8_t *data, int w, int h);

#endif
