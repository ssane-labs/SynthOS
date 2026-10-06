#include <synth/string.h>

#include "crt.h"

#define RC_DOWN 0x0400
#define RC_UP 0x0800
#define RC_TRUNC 0x0c00

static long double round_with(long double x, uint16_t mode)
{
    uint16_t cw, ncw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    ncw = (uint16_t)((cw & ~0x0c00) | mode);
    __asm__ volatile("fldcw %0" : : "m"(ncw));
    __asm__ volatile("frndint" : "=t"(x) : "0"(x));
    __asm__ volatile("fldcw %0" : : "m"(cw));
    return x;
}

static long double exp2_l(long double x)
{
    long double r;
    __asm__("fld %%st(0)\n\t"
            "frndint\n\t"
            "fxch\n\t"
            "fsub %%st(1), %%st\n\t"
            "f2xm1\n\t"
            "fld1\n\t"
            "faddp\n\t"
            "fscale\n\t"
            "fstp %%st(1)"
            : "=t"(r)
            : "0"(x));
    return r;
}

static long double log2_l(long double x)
{
    long double r;
    __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x));
    return r;
}

static bool is_nan(double x)
{
    return x != x;
}

static bool is_inf(double x)
{
    return x == __builtin_inf() || x == -__builtin_inf();
}

static double m_sqrt(double x)
{
    if (x < 0) {
        crt_set_errno(CRT_EDOM);
        return __builtin_nan("");
    }
    long double r;
    __asm__("fsqrt" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static double m_sin(double x)
{
    long double r;
    __asm__("fsin" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static double m_cos(double x)
{
    long double r;
    __asm__("fcos" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static double m_tan(double x)
{
    long double r;
    __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static long double atan2_l(long double y, long double x)
{
    long double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

static double m_atan2(double y, double x)
{
    return (double)atan2_l(y, x);
}

static double m_atan(double x)
{
    return (double)atan2_l(x, 1.0L);
}

static double m_asin(double x)
{
    if (x < -1 || x > 1) {
        crt_set_errno(CRT_EDOM);
        return __builtin_nan("");
    }
    long double s, v = 1.0L - (long double)x * x;
    __asm__("fsqrt" : "=t"(s) : "0"(v));
    return (double)atan2_l(x, s);
}

static double m_acos(double x)
{
    if (x < -1 || x > 1) {
        crt_set_errno(CRT_EDOM);
        return __builtin_nan("");
    }
    long double s, v = 1.0L - (long double)x * x;
    __asm__("fsqrt" : "=t"(s) : "0"(v));
    return (double)atan2_l(s, x);
}

static double m_exp(double x)
{
    if (is_nan(x))
        return x;
    if (x > 709.8) {
        crt_set_errno(CRT_ERANGE);
        return __builtin_inf();
    }
    if (x < -745.2)
        return 0;
    long double l2e;
    __asm__("fldl2e" : "=t"(l2e));
    return (double)exp2_l(x * l2e);
}

static double m_log(double x)
{
    if (x < 0 || is_nan(x)) {
        crt_set_errno(CRT_EDOM);
        return __builtin_nan("");
    }
    if (x == 0) {
        crt_set_errno(CRT_ERANGE);
        return -__builtin_inf();
    }
    long double r;
    __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static double m_log10(double x)
{
    if (x < 0 || is_nan(x)) {
        crt_set_errno(CRT_EDOM);
        return __builtin_nan("");
    }
    if (x == 0) {
        crt_set_errno(CRT_ERANGE);
        return -__builtin_inf();
    }
    long double r;
    __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static double m_log2(double x)
{
    if (x <= 0)
        return x == 0 ? -__builtin_inf() : __builtin_nan("");
    return (double)log2_l(x);
}

static double m_exp2(double x)
{
    return (double)exp2_l(x);
}

static double m_floor(double x)
{
    return (double)round_with(x, RC_DOWN);
}

static double m_ceil(double x)
{
    return (double)round_with(x, RC_UP);
}

static double m_trunc(double x)
{
    return (double)round_with(x, RC_TRUNC);
}

static double m_round(double x)
{
    return x < 0 ? -(double)round_with(-x + 0.5L, RC_DOWN) : (double)round_with(x + 0.5L, RC_DOWN);
}

static long m_lround(double x)
{
    return (long)m_round(x);
}

static double m_rint(double x)
{
    long double r;
    __asm__("frndint" : "=t"(r) : "0"((long double)x));
    return (double)r;
}

static double m_fabs(double x)
{
    return x < 0 ? -x : x == 0 ? 0.0 : x;
}

static double m_fmod(double x, double y)
{
    if (y == 0 || is_inf(x)) {
        crt_set_errno(CRT_EDOM);
        return __builtin_nan("");
    }
    long double r;
    __asm__("1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "sahf\n\t"
            "jp 1b"
            : "=t"(r)
            : "0"((long double)x), "u"((long double)y)
            : "ax", "cc");
    return (double)r;
}

static double m_pow(double x, double y)
{
    if (y == 0)
        return 1;
    if (is_nan(x) || is_nan(y))
        return x + y;
    long double iy = round_with(y, RC_TRUNC);
    bool integral = iy == (long double)y;
    if (x == 0) {
        if (y < 0) {
            crt_set_errno(CRT_ERANGE);
            return __builtin_inf();
        }
        return 0;
    }
    if (integral && iy <= 64 && iy >= -64) {
        long double r = 1, b = x;
        int n = (int)iy;
        unsigned k = (unsigned)(n < 0 ? -n : n);
        while (k) {
            if (k & 1)
                r *= b;
            b *= b;
            k >>= 1;
        }
        return (double)(n < 0 ? 1.0L / r : r);
    }
    bool neg = false;
    if (x < 0) {
        if (!integral) {
            crt_set_errno(CRT_EDOM);
            return __builtin_nan("");
        }
        long double half = round_with(iy / 2, RC_TRUNC);
        neg = half * 2 != iy;
        x = -x;
    }
    long double e = (long double)y * log2_l(x);
    if (e > 16384) {
        crt_set_errno(CRT_ERANGE);
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (e < -16400)
        return 0;
    long double r = exp2_l(e);
    return (double)(neg ? -r : r);
}

static double m_sinh(double x)
{
    double e = m_exp(x);
    return (e - 1 / e) / 2;
}

static double m_cosh(double x)
{
    double e = m_exp(x);
    return (e + 1 / e) / 2;
}

static double m_tanh(double x)
{
    if (x > 22)
        return 1;
    if (x < -22)
        return -1;
    double e = m_exp(2 * x);
    return (e - 1) / (e + 1);
}

static double m_hypot(double x, double y)
{
    long double r, v = (long double)x * x + (long double)y * y;
    __asm__("fsqrt" : "=t"(r) : "0"(v));
    return (double)r;
}

static double m_cbrt(double x)
{
    if (x == 0 || is_nan(x) || is_inf(x))
        return x;
    double r = (double)exp2_l(log2_l(x < 0 ? -x : x) / 3);
    r = r - (r * r * r - (x < 0 ? -x : x)) / (3 * r * r);
    return x < 0 ? -r : r;
}

static double m_ldexp(double x, int e)
{
    long double r;
    __asm__("fscale" : "=t"(r) : "0"((long double)x), "u"((long double)e));
    return (double)r;
}

static double m_frexp(double x, int *e)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    int ex = (int)((bits >> 52) & 0x7ff);
    if (x == 0 || ex == 0x7ff) {
        *e = 0;
        return x;
    }
    if (!ex) {
        x *= 18014398509481984.0;
        memcpy(&bits, &x, 8);
        ex = (int)((bits >> 52) & 0x7ff) - 54;
    }
    *e = ex - 1022;
    bits = (bits & ~(0x7ffull << 52)) | (1022ull << 52);
    memcpy(&x, &bits, 8);
    return x;
}

static double m_modf(double x, double *ip)
{
    double t = m_trunc(x);
    *ip = t;
    return is_inf(x) ? 0 : x - t;
}

static double m_copysign(double x, double y)
{
    uint64_t a, b;
    memcpy(&a, &x, 8);
    memcpy(&b, &y, 8);
    a = (a & ~(1ull << 63)) | (b & (1ull << 63));
    memcpy(&x, &a, 8);
    return x;
}

static double m_chgsign(double x)
{
    return -x;
}

static int m_finite(double x)
{
    return !is_nan(x) && !is_inf(x);
}

static int m_isnan(double x)
{
    return is_nan(x);
}

static int m_fpclass(double x)
{
    uint64_t bits;
    memcpy(&bits, &x, 8);
    bool neg = bits >> 63;
    int ex = (int)((bits >> 52) & 0x7ff);
    uint64_t mant = bits & 0xfffffffffffffull;
    if (ex == 0x7ff)
        return mant ? 0x2 : neg ? 0x4 : 0x200;
    if (!ex)
        return mant ? (neg ? 0x10 : 0x80) : (neg ? 0x20 : 0x40);
    return neg ? 0x8 : 0x100;
}

static double m_logb(double x)
{
    int e;
    m_frexp(x, &e);
    return e - 1;
}

static double m_nextafter(double x, double y)
{
    if (is_nan(x) || is_nan(y))
        return x + y;
    if (x == y)
        return y;
    uint64_t bits;
    if (x == 0) {
        bits = 1;
        memcpy(&x, &bits, 8);
        return y > 0 ? x : -x;
    }
    memcpy(&bits, &x, 8);
    if ((x < y) == (x > 0))
        bits++;
    else
        bits--;
    memcpy(&x, &bits, 8);
    return x;
}

static double m_difftime64(int64_t a, int64_t b)
{
    return (double)(a - b);
}

static double m_difftime32(int32_t a, int32_t b)
{
    return (double)(a - b);
}

static float m_sqrtf(float x) { return (float)m_sqrt(x); }
static float m_sinf(float x) { return (float)m_sin(x); }
static float m_cosf(float x) { return (float)m_cos(x); }
static float m_tanf(float x) { return (float)m_tan(x); }
static float m_atan2f(float y, float x) { return (float)m_atan2(y, x); }
static float m_atanf(float x) { return (float)m_atan(x); }
static float m_expf(float x) { return (float)m_exp(x); }
static float m_logf(float x) { return (float)m_log(x); }
static float m_log10f(float x) { return (float)m_log10(x); }
static float m_powf(float x, float y) { return (float)m_pow(x, y); }
static float m_floorf(float x) { return (float)m_floor(x); }
static float m_ceilf(float x) { return (float)m_ceil(x); }
static float m_fmodf(float x, float y) { return (float)m_fmod(x, y); }
static float m_fabsf(float x) { return x < 0 ? -x : x; }

__asm__(".intel_syntax noprefix\n"
        ".text\n"
        "crt_ftol:\n"
        "  sub esp, 12\n"
        "  fnstcw [esp]\n"
        "  mov ax, [esp]\n"
        "  or ax, 0x0c00\n"
        "  mov [esp+2], ax\n"
        "  fldcw [esp+2]\n"
        "  fistp qword ptr [esp+4]\n"
        "  fldcw [esp]\n"
        "  mov eax, [esp+4]\n"
        "  mov edx, [esp+8]\n"
        "  add esp, 12\n"
        "  ret\n"
        "crt_ci_unary:\n"
        "  sub esp, 8\n"
        "  fstp qword ptr [esp]\n"
        "  call ecx\n"
        "  add esp, 8\n"
        "  ret\n"
        "crt_ci_binary:\n"
        "  sub esp, 16\n"
        "  fstp qword ptr [esp+8]\n"
        "  fstp qword ptr [esp]\n"
        "  call ecx\n"
        "  add esp, 16\n"
        "  ret\n"
        ".att_syntax\n");

#define CI_UNARY(name, fn) __asm__(".text\n" #name ":\n\tmovl $" #fn ", %ecx\n\tjmp crt_ci_unary\n")
#define CI_BINARY(name, fn) __asm__(".text\n" #name ":\n\tmovl $" #fn ", %ecx\n\tjmp crt_ci_binary\n")

CI_UNARY(crt_ci_sin, m_sin_ci);
CI_UNARY(crt_ci_cos, m_cos_ci);
CI_UNARY(crt_ci_tan, m_tan_ci);
CI_UNARY(crt_ci_sqrt, m_sqrt_ci);
CI_UNARY(crt_ci_exp, m_exp_ci);
CI_UNARY(crt_ci_log, m_log_ci);
CI_UNARY(crt_ci_log10, m_log10_ci);
CI_UNARY(crt_ci_asin, m_asin_ci);
CI_UNARY(crt_ci_acos, m_acos_ci);
CI_UNARY(crt_ci_atan, m_atan_ci);
CI_UNARY(crt_ci_sinh, m_sinh_ci);
CI_UNARY(crt_ci_cosh, m_cosh_ci);
CI_UNARY(crt_ci_tanh, m_tanh_ci);
CI_BINARY(crt_ci_pow, m_pow_ci);
CI_BINARY(crt_ci_fmod, m_fmod_ci);
CI_BINARY(crt_ci_atan2, m_atan2_ci);

#define CI_WRAP1(f) __attribute__((used)) double f##_ci(double x) { return f(x); }
#define CI_WRAP2(f) __attribute__((used)) double f##_ci(double x, double y) { return f(x, y); }

CI_WRAP1(m_sin)
CI_WRAP1(m_cos)
CI_WRAP1(m_tan)
CI_WRAP1(m_sqrt)
CI_WRAP1(m_exp)
CI_WRAP1(m_log)
CI_WRAP1(m_log10)
CI_WRAP1(m_asin)
CI_WRAP1(m_acos)
CI_WRAP1(m_atan)
CI_WRAP1(m_sinh)
CI_WRAP1(m_cosh)
CI_WRAP1(m_tanh)
CI_WRAP2(m_pow)
CI_WRAP2(m_fmod)
CI_WRAP2(m_atan2)

extern char crt_ftol[], crt_ci_sin[], crt_ci_cos[], crt_ci_tan[], crt_ci_sqrt[], crt_ci_exp[], crt_ci_log[],
    crt_ci_log10[], crt_ci_asin[], crt_ci_acos[], crt_ci_atan[], crt_ci_sinh[], crt_ci_cosh[], crt_ci_tanh[],
    crt_ci_pow[], crt_ci_fmod[], crt_ci_atan2[];

static double huge_val = __builtin_inf();

static const struct w32_export math_exports[] = {
    W32_AS("sqrt", m_sqrt), W32_AS("sin", m_sin), W32_AS("cos", m_cos), W32_AS("tan", m_tan),
    W32_AS("atan", m_atan), W32_AS("atan2", m_atan2), W32_AS("asin", m_asin), W32_AS("acos", m_acos),
    W32_AS("exp", m_exp), W32_AS("log", m_log), W32_AS("log10", m_log10), W32_AS("log2", m_log2),
    W32_AS("exp2", m_exp2), W32_AS("pow", m_pow), W32_AS("floor", m_floor), W32_AS("ceil", m_ceil),
    W32_AS("trunc", m_trunc), W32_AS("round", m_round), W32_AS("lround", m_lround), W32_AS("rint", m_rint),
    W32_AS("nearbyint", m_rint), W32_AS("fabs", m_fabs), W32_AS("fmod", m_fmod), W32_AS("sinh", m_sinh),
    W32_AS("cosh", m_cosh), W32_AS("tanh", m_tanh), W32_AS("_hypot", m_hypot), W32_AS("hypot", m_hypot),
    W32_AS("cbrt", m_cbrt), W32_AS("ldexp", m_ldexp), W32_AS("frexp", m_frexp), W32_AS("modf", m_modf),
    W32_AS("_copysign", m_copysign), W32_AS("copysign", m_copysign), W32_AS("_chgsign", m_chgsign),
    W32_AS("_finite", m_finite), W32_AS("_isnan", m_isnan), W32_AS("_fpclass", m_fpclass),
    W32_AS("_logb", m_logb), W32_AS("_scalb", m_ldexp), W32_AS("_nextafter", m_nextafter),
    W32_AS("nextafter", m_nextafter), W32_AS("difftime", m_difftime32), W32_AS("_difftime32", m_difftime32),
    W32_AS("_difftime64", m_difftime64), W32_AS("sqrtf", m_sqrtf), W32_AS("sinf", m_sinf), W32_AS("cosf", m_cosf),
    W32_AS("tanf", m_tanf), W32_AS("atan2f", m_atan2f), W32_AS("atanf", m_atanf), W32_AS("expf", m_expf),
    W32_AS("logf", m_logf), W32_AS("log10f", m_log10f), W32_AS("powf", m_powf), W32_AS("floorf", m_floorf),
    W32_AS("ceilf", m_ceilf), W32_AS("fmodf", m_fmodf), W32_AS("fabsf", m_fabsf), W32_AS("_HUGE", &huge_val),
    W32_AS("_ftol", crt_ftol), W32_AS("_ftol2", crt_ftol), W32_AS("_ftol2_sse", crt_ftol),
    W32_AS("_CIsin", crt_ci_sin), W32_AS("_CIcos", crt_ci_cos), W32_AS("_CItan", crt_ci_tan),
    W32_AS("_CIsqrt", crt_ci_sqrt), W32_AS("_CIexp", crt_ci_exp), W32_AS("_CIlog", crt_ci_log),
    W32_AS("_CIlog10", crt_ci_log10), W32_AS("_CIasin", crt_ci_asin), W32_AS("_CIacos", crt_ci_acos),
    W32_AS("_CIatan", crt_ci_atan), W32_AS("_CIsinh", crt_ci_sinh), W32_AS("_CIcosh", crt_ci_cosh),
    W32_AS("_CItanh", crt_ci_tanh), W32_AS("_CIpow", crt_ci_pow), W32_AS("_CIfmod", crt_ci_fmod),
    W32_AS("_CIatan2", crt_ci_atan2),
};

CRT_EXPORTS(crt_math_module, math_exports);
