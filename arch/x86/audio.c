#include <synth/error.h>
#include <synth/hal.h>

int hal_audio_open(int rate)
{
    UNUSED(rate);
    return E_NODEV;
}

int hal_audio_space(void)
{
    return 0;
}

int hal_audio_write(const int16_t *frames, int count)
{
    UNUSED(frames);
    UNUSED(count);
    return E_NODEV;
}

void hal_audio_close(void)
{
}

const char *hal_audio_name(void)
{
    return NULL;
}
