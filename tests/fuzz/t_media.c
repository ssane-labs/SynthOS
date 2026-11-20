/* MP3 (minimp3), Ogg Vorbis (stb_vorbis) and WebP (simplewebp) as the media module reads them */
#define SYNTH_MODULE
#define synth_module_app fuzz_media_app
#include "../../kernel/gui/optional/media_fp.c"
#include "targets.h"

void fuzz_media(const uint8_t *d, size_t n)
{
    struct picture p = {0};
    if (!webp_decode(d, n, &p))
        picture_free(&p);
    int16_t *frames = NULL;
    uint32_t count, rate;
    if (!audio_any(d, n, &frames, &count, &rate))
        kfree(frames);
}
