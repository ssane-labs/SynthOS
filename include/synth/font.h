#ifndef SYNTH_FONT_H
#define SYNTH_FONT_H

#include <synth/types.h>

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

extern const uint8_t font8x16_cp437[256][16];
extern const uint8_t font8x16_cp866[256][16];

const uint8_t (*font_for_codepage(int codepage))[16];

#endif
