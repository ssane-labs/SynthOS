#ifndef SYNTH_FBCON_H
#define SYNTH_FBCON_H

#include <synth/hal.h>

struct framebuffer {
    uint8_t *base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
};

struct display *fbcon_create(const struct framebuffer *fb);
void fbcon_codepage_changed(void);

#endif
