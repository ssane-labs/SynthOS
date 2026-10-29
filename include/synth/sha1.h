#ifndef SYNTH_SHA1_H
#define SYNTH_SHA1_H

#include <synth/sha256.h>
#include <synth/types.h>

void sha1(const void *data, size_t n, uint8_t out[20]);
void sha1_init(struct sha256 *s);
void sha1_update(struct sha256 *s, const void *data, size_t n);
void sha1_final(struct sha256 *s, uint8_t out[20]);
void md5_init(struct sha256 *s);
void md5_update(struct sha256 *s, const void *data, size_t n);
void md5_final(struct sha256 *s, uint8_t out[16]);

#endif
