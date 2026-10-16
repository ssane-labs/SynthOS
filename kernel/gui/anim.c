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

void spring_set(struct spring *s, int value)
{
    s->pos = s->target = value;
    s->vel = s->rem = 0;
}

bool spring_busy(const struct spring *s)
{
    return s->pos != s->target || s->vel;
}

bool spring_step(struct spring *s, int stiffness, int damping, int dt)
{
    if (!spring_busy(s))
        return false;
    dt = CLAMP(dt, 1, 64);
    while (dt > 0) {
        int step = MIN(dt, 4);
        int64_t force = (int64_t)stiffness * (s->target - s->pos) - (int64_t)damping * s->vel;
        s->vel += (int)(force * step / 1000);
        int64_t move = (int64_t)s->vel * step + s->rem;
        s->pos += (int)(move / 1000);
        s->rem = (int)(move % 1000);
        dt -= step;
    }
    int d = s->target - s->pos;
    if (d > -3 && d < 3 && s->vel > -60 && s->vel < 60) {
        s->pos = s->target;
        s->vel = s->rem = 0;
    }
    return true;
}
