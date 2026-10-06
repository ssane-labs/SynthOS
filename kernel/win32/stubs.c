#include "win32.h"

#define A uint32_t
#define STUBS(value, prefix)                                                                                           \
    uint32_t WINAPI prefix##0(void) { return value; }                                                                  \
    uint32_t WINAPI prefix##1(A a) { UNUSED(a); return value; }                                                        \
    uint32_t WINAPI prefix##2(A a, A b) { UNUSED(a); UNUSED(b); return value; }                                        \
    uint32_t WINAPI prefix##3(A a, A b, A c) { UNUSED(a); UNUSED(b); UNUSED(c); return value; }                        \
    uint32_t WINAPI prefix##4(A a, A b, A c, A d) { UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); return value; }        \
    uint32_t WINAPI prefix##5(A a, A b, A c, A d, A e)                                                                 \
    {                                                                                                                  \
        UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); UNUSED(e);                                                         \
        return value;                                                                                                  \
    }                                                                                                                  \
    uint32_t WINAPI prefix##6(A a, A b, A c, A d, A e, A f)                                                            \
    {                                                                                                                  \
        UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); UNUSED(e); UNUSED(f);                                              \
        return value;                                                                                                  \
    }

STUBS(0, w32_zero)
STUBS(1, w32_one)
