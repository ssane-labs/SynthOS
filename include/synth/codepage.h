#ifndef SYNTH_CODEPAGE_H
#define SYNTH_CODEPAGE_H

#include <synth/types.h>

int codepage_current(void);
bool codepage_set(int cp);
bool codepage_supported(int cp);
uint32_t codepage_to_unicode(uint8_t ch);
int codepage_from_unicode(uint32_t cp);
size_t utf8_encode(uint32_t cp, char out[4]);

#endif
