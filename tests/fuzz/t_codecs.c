/* the pictures and sounds of the codecs module (TIFF, Targa, PNM, QOI, ICO, FLAC, AIFF) */
#define SYNTH_MODULE
#define synth_module_app fuzz_codecs_app
#include "../../kernel/gui/optional/codecs.c"
#include "targets.h"

void fuzz_codecs(const uint8_t *d, size_t n)
{
    struct picture p = {0};
    if (!picture_any(d, n, &p))
        picture_free(&p);
    int16_t *frames = NULL;
    uint32_t count, rate;
    if (!audio_any(d, n, &frames, &count, &rate))
        kfree(frames);
}
