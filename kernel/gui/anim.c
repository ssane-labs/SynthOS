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

/* A critically damped motion sampled over the animation: it starts from rest, slows down as it arrives and never
   passes the end, so the last frame meets the still one exactly. */
static const uint16_t spring_curve[65] = {
    0,   7,   27,  56,  92,  133, 178, 224, 271,  318,  364,  409,  453,  495,  535,  573,  608,
    642, 673, 703, 730, 755, 779, 800, 820, 839,  855,  871,  885,  898,  910,  921,  931,  940,
    948, 955, 962, 968, 974, 979, 983, 988, 991,  995,  998,  1001, 1003, 1005, 1008, 1009, 1011,
    1013, 1014, 1015, 1017, 1018, 1019, 1019, 1020, 1021, 1022, 1022, 1023, 1023, 1024,
};

int ease_spring(int t)
{
    t = CLAMP(t, 0, 1024);
    int i = t >> 4, f = t & 15;
    if (i >= 64)
        return 1024;
    return spring_curve[i] + (spring_curve[i + 1] - spring_curve[i]) * f / 16;
}

int lerp_i(int a, int b, int t)
{
    return a + (int)((int64_t)(b - a) * t / 1024);
}

#define FIX 16
#define ONE (1 << FIX)

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

void spring_set(struct spring *s, int value)
{
    s->pos = s->target = value;
    s->vel = 0;
    s->x = (int64_t)value * ONE;
    s->v = 0;
}

bool spring_busy(const struct spring *s)
{
    return s->pos != s->target || s->vel || s->x != (int64_t)s->target * ONE;
}

/* Moves a spring on by dt ms. The position keeps 16 fraction bits, so it glides into its target instead of stopping
   a few units short and jumping; the damping is never below critical, so nothing bounces or wobbles. */
bool spring_step(struct spring *s, int stiffness, int damping, int dt)
{
    if ((s->x + ONE / 2) >> FIX != s->pos)
        s->x = (int64_t)s->pos * ONE;
    if (s->v >> FIX != s->vel)
        s->v = (int64_t)s->vel * ONE;
    if (!spring_busy(s))
        return false;
    int k = MAX(stiffness, 1);
    int c = MAX(damping, isqrt(k) * 2);
    int64_t goal = (int64_t)s->target * ONE;
    dt = CLAMP(dt, 1, 64);
    while (dt > 0) {
        int step = MIN(dt, 2);
        int64_t accel = (int64_t)k * (goal - s->x) - (int64_t)c * s->v;
        s->v += accel * step / 1000;
        s->x += s->v * step / 1000;
        dt -= step;
    }
    int64_t d = goal - s->x;
    if (d > -ONE / 4 && d < ONE / 4 && s->v > -4 * ONE && s->v < 4 * ONE) {
        s->x = goal;
        s->v = 0;
    }
    s->pos = (int)((s->x + ONE / 2) >> FIX);
    s->vel = (int)(s->v >> FIX);
    return true;
}
