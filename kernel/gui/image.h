#ifndef SYNTH_GUI_IMAGE_H
#define SYNTH_GUI_IMAGE_H

#include <synth/types.h>

struct image {
    uint32_t *px;
    int w, h;
};

bool image_load(const char *path, struct image *out);
int image_save(const char *path, const uint32_t *px, int w, int h, int stride);
void image_free(struct image *img);
bool image_load_exe_icon(const char *path, int want, struct image *out);

#endif
