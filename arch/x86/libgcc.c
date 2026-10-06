#include <synth/types.h>

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    if (d == 0) {
        if (rem)
            *rem = 0;
        return 0;
    }
    if ((n >> 32) == 0 && (d >> 32) == 0) {
        uint32_t n32 = (uint32_t)n, d32 = (uint32_t)d;
        if (rem)
            *rem = n32 % d32;
        return n32 / d32;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d);
uint64_t __umoddi3(uint64_t n, uint64_t d);
int64_t __divdi3(int64_t n, int64_t d);
int64_t __moddi3(int64_t n, int64_t d);

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return udivmod64(n, d, NULL);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    udivmod64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    bool neg = (n < 0) != (d < 0);
    uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, NULL);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem);
int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem);

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    return udivmod64(n, d, rem);
}

int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem)
{
    int64_t q = __divdi3(n, d);
    if (rem)
        *rem = n - q * d;
    return q;
}
