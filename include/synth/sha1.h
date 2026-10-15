#ifndef SYNTH_SHA1_H
#define SYNTH_SHA1_H

#include <synth/types.h>

void sha1(const void *data, size_t n, uint8_t out[20]);

#endif
