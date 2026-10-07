#include <synth/string.h>

#include "win64.h"

static double D(uint64_t v)
{
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static float F(uint64_t v)
{
    float x;
    uint32_t lo = (uint32_t)v;
    memcpy(&x, &lo, 4);
    return x;
}

static uint64_t RD(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

static uint64_t RF(float x)
{
    uint32_t v;
    memcpy(&v, &x, 4);
    return v;
}

uint64_t w64_math(void *target, int kind, const uint64_t *a, const uint64_t *f)
{
    switch (kind) {
    case M_D_D:
        return RD(((double (*)(double))target)(D(f[0])));
    case M_D_DD:
        return RD(((double (*)(double, double))target)(D(f[0]), D(f[1])));
    case M_D_DI:
        return RD(((double (*)(double, int))target)(D(f[0]), (int)a[1]));
    case M_D_DP:
        return RD(((double (*)(double, void *))target)(D(f[0]), w64_ptr(a[1])));
    case M_F_F:
        return RF(((float (*)(float))target)(F(f[0])));
    case M_F_FF:
        return RF(((float (*)(float, float))target)(F(f[0]), F(f[1])));
    case M_I_D:
        return w64_sx((uint32_t)((int (*)(double))target)(D(f[0])));
    case M_D_LL:
        return RD(((double (*)(int64_t, int64_t))target)((int64_t)a[0], (int64_t)a[1]));
    default:
        return 0;
    }
}

uint64_t w64_strtod(uint64_t *a, const uint64_t *f, void *target, bool wide, bool single)
{
    UNUSED(f);
    uint32_t end = 0;
    double d = wide ? ((double (*)(const WCHAR *, uint32_t *))target)(w64_ptr(a[0]), a[1] ? &end : NULL)
                    : ((double (*)(const char *, uint32_t *))target)(w64_ptr(a[0]), a[1] ? &end : NULL);
    if (a[1]) {
        uint64_t e = end;
        memcpy(w64_ptr(a[1]), &e, 8);
    }
    return single ? RF((float)d) : RD(d);
}

uint64_t w64_atof(void *target, uint64_t s)
{
    return RD(((double (*)(const char *))target)(w64_ptr(s)));
}
