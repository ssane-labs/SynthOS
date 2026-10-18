#ifndef SYNTH_BUNDLE_H
#define SYNTH_BUNDLE_H

#include <synth/types.h>

/* A program packed into the disk image between the system and the data partition (tools/mkbundle.py). */
struct bundle_info {
    char title[64], prefix[64];
    uint32_t sectors, files;
    uint64_t bytes;
};

bool bundle_info(struct bundle_info *out);
/* Unpacks every file into dest (without the prefix); the program's start file `last` is written at the end, so it
   only exists once everything else is in place. */
int bundle_extract(const char *dest, const char *last, void (*progress)(void *ctx, uint64_t done, uint64_t total),
                   void *ctx);

#endif
