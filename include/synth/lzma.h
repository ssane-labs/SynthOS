#ifndef SYNTH_LZMA_H
#define SYNTH_LZMA_H

#include <synth/types.h>

/* Decodes an LZMA stream (5 property bytes: lc/lp/pb and the dictionary size) into exactly out_size bytes. */
int lzma_decode(const uint8_t props[5], const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size);

#endif
