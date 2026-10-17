#ifndef SYNTH_SEVENZIP_H
#define SYNTH_SEVENZIP_H

#include <synth/types.h>

struct sevenzip_result {
    int files, skipped;
    uint64_t bytes;
};

/* Extracts the 7z archive that starts at byte `base` of `archive` (-1: look for it in the first 8 MB, as in a
   self-extracting .exe) into `dest`. With `strip`, only names starting with it are kept, without it. Copy, LZMA and
   BCJ2 are unpacked; other methods are counted in result->skipped. */
int sevenzip_extract(const char *archive, int64_t base, const char *dest, const char *strip,
                     void (*progress)(void *ctx, const char *name, uint64_t done, uint64_t total), void *ctx,
                     struct sevenzip_result *result);

/* Where a 7z archive starts in `archive`, looking from byte `from` on (8 MB at most). */
int64_t sevenzip_find(const char *archive, uint64_t from);

#endif
