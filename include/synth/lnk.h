#ifndef SYNTH_LNK_H
#define SYNTH_LNK_H

#include <synth/types.h>

/* Windows shell links (.lnk, MS-SHLLINK); strings are UTF-8, paths are Windows paths. */
struct lnk {
    char target[260];
    char args[1024];
    char dir[260];
    char desc[260];
    char icon[260];
    int icon_index;
    int show;
};

size_t lnk_encode(const struct lnk *l, uint8_t *out, size_t cap);
int lnk_decode(const uint8_t *data, size_t size, struct lnk *l);
int lnk_read(const char *path, struct lnk *l);

#endif
