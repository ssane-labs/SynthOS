#include <synth/hal.h>

#include "internal.h"

int anim_progress(uint64_t start, int ms)
{
    uint64_t now = hal_ticks_ms();
    if (ms <= 0 || now >= start + (uint64_t)ms)
        return 1024;
    return (int)((now - start) * 1024 / (uint64_t)ms);
}

int ease_out(int t)
{
    t = CLAMP(t, 0, 1024);
    int u = 1024 - t;
    return 1024 - (int)((int64_t)u * u / 1024 * u / 1024);
}

int ease_in_out(int t)
{
    t = CLAMP(t, 0, 1024);
    if (t < 512)
        return (int)((int64_t)t * t / 1024 * t / 1024 * 4);
    int u = 1024 - t;
    return 1024 - (int)((int64_t)u * u / 1024 * u / 1024 * 4);
}

int ease_spring(int t)
{
    t = CLAMP(t, 0, 1024);
    int e = ease_out(t);
    int over = (int)((int64_t)t * (1024 - t) / 1024 * (1024 - t) / 1024);
    return e + over * 2 / 5;
}

int lerp_i(int a, int b, int t)
{
    return a + (int)((int64_t)(b - a) * t / 1024);
}
