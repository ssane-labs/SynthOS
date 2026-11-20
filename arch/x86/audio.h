#ifndef ARCH_X86_AUDIO_H
#define ARCH_X86_AUDIO_H

#include <synth/types.h>

bool hda_probe(int rate);
int hda_open(int rate);
int hda_space(void);
int hda_write(const int16_t *frames, int count);
void hda_close(void);
const char *hda_name(void);
bool hda_record_present(bool *recording);
int hda_record_open(void);
int hda_record_read(int16_t *frames, int max);
void hda_record_close(void);

#endif
