#ifndef ARCH_X86_AUDIO_H
#define ARCH_X86_AUDIO_H

#include <synth/types.h>

bool hda_probe(int rate);
int hda_open(int rate);
int hda_space(void);
int hda_write(const int16_t *frames, int count);
void hda_close(void);
const char *hda_name(void);

#endif
