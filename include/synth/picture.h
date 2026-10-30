#ifndef SYNTH_PICTURE_H
#define SYNTH_PICTURE_H

#include <synth/types.h>

/* Decoded pictures are 32-bit ARGB, straight alpha; animations keep every frame one after another. */
struct picture {
    uint32_t *px;
    int w, h;
    int frames;
    int *delays;
};

#define PICTURE_MAX_PIXELS (32u << 20)

int picture_decode(const uint8_t *data, size_t size, struct picture *out);
int picture_load(const char *path, struct picture *out);
void picture_free(struct picture *p);
bool picture_is_image(const char *name);
int png_decode(const uint8_t *data, size_t size, struct picture *out);
int jpeg_decode(const uint8_t *data, size_t size, struct picture *out);
int gif_decode(const uint8_t *data, size_t size, struct picture *out);
int bmp_decode(const uint8_t *data, size_t size, struct picture *out);
int png_encode(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *len);

#endif
