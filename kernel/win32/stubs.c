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

#define ARGS(n)                                                                                                        \
    struct w32_args##n {                                                                                               \
        uint32_t v[n];                                                                                                 \
    }
ARGS(7);
ARGS(8);
ARGS(9);
ARGS(10);
ARGS(11);
ARGS(12);
ARGS(13);
ARGS(14);

#define WIDE(prefix, body)                                                                                             \
    uint32_t WINAPI prefix##7(struct w32_args7 a) { UNUSED(a); body }                                                  \
    uint32_t WINAPI prefix##8(struct w32_args8 a) { UNUSED(a); body }                                                  \
    uint32_t WINAPI prefix##9(struct w32_args9 a) { UNUSED(a); body }                                                  \
    uint32_t WINAPI prefix##10(struct w32_args10 a) { UNUSED(a); body }                                                \
    uint32_t WINAPI prefix##11(struct w32_args11 a) { UNUSED(a); body }                                                \
    uint32_t WINAPI prefix##12(struct w32_args12 a) { UNUSED(a); body }                                                \
    uint32_t WINAPI prefix##13(struct w32_args13 a) { UNUSED(a); body }                                                \
    uint32_t WINAPI prefix##14(struct w32_args14 a) { UNUSED(a); body }

#define FAMILY(value, prefix, pre)                                                                                     \
    uint32_t WINAPI prefix##0(void) { pre return value; }                                                              \
    uint32_t WINAPI prefix##1(A a) { UNUSED(a); pre return value; }                                                    \
    uint32_t WINAPI prefix##2(A a, A b) { UNUSED(a); UNUSED(b); pre return value; }                                    \
    uint32_t WINAPI prefix##3(A a, A b, A c) { UNUSED(a); UNUSED(b); UNUSED(c); pre return value; }                    \
    uint32_t WINAPI prefix##4(A a, A b, A c, A d) { UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); pre return value; }    \
    uint32_t WINAPI prefix##5(A a, A b, A c, A d, A e)                                                                 \
    {                                                                                                                  \
        UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); UNUSED(e);                                                         \
        pre return value;                                                                                              \
    }                                                                                                                  \
    uint32_t WINAPI prefix##6(A a, A b, A c, A d, A e, A f)                                                            \
    {                                                                                                                  \
        UNUSED(a); UNUSED(b); UNUSED(c); UNUSED(d); UNUSED(e); UNUSED(f);                                              \
        pre return value;                                                                                              \
    }                                                                                                                  \
    WIDE(prefix, pre return value;)

WIDE(w32_zero, return 0;)
WIDE(w32_one, return 1;)
FAMILY(0, w32_fail, w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);)
FAMILY(0xc00000bbu, w32_status, )
FAMILY(0x80004001u, w32_notimpl, )
FAMILY(0xffffffffu, w32_neg, w32_set_error(ERROR_CALL_NOT_IMPLEMENTED);)
FAMILY(0x80090302u, w32_secerr, )
