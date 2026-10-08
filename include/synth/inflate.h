#ifndef SYNTH_INFLATE_H
#define SYNTH_INFLATE_H

#include <synth/types.h>

int inflate_into(const void *in, size_t in_len, void *out, size_t out_cap, size_t *out_len, size_t *used);
int inflate_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len, size_t *used);
int gunzip_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len);
int zlib_inflate_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len);

#endif
