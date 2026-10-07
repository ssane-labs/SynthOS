#include <synth/string.h>

#include "crt.h"

#define PRINTF_STANDARD_SNPRINTF 2ull

struct onexit_table {
    void (**first)(void);
    void (**last)(void);
    void (**end)(void);
};

static uint32_t exception_object;

int crt_setjmp3(void *buf);
void crt_longjmp(void *buf, int value);

static void *lookup(const char *name)
{
    return w32_builtin_lookup("msvcrt.dll", name);
}

static int set_new_mode(int mode)
{
    UNUSED(mode);
    return 0;
}

static int configure_argv(int mode)
{
    UNUSED(mode);
    return 0;
}

static int initialize_environment(void)
{
    return 0;
}

static char **initial_environment(void)
{
    return w32->environ;
}

static WCHAR **initial_wide_environment(void)
{
    return w32->wenviron;
}

static int at_quick_exit(void (*fn)(void))
{
    UNUSED(fn);
    return 0;
}

static int initialize_onexit_table(struct onexit_table *t)
{
    if (!t)
        return -1;
    t->first = t->last = t->end = NULL;
    return 0;
}

static int register_onexit_function(struct onexit_table *t, void (*fn)(void))
{
    if (!t)
        return -1;
    if (t->last == t->end) {
        size_t used = (size_t)(t->last - t->first), cap = used ? used * 2 : 16;
        void (**grown)(void) = w32_realloc(t->first, cap * sizeof(*grown));
        if (!grown)
            return -1;
        t->first = grown;
        t->last = grown + used;
        t->end = grown + cap;
    }
    *t->last++ = fn;
    return 0;
}

static int execute_onexit_table(struct onexit_table *t)
{
    if (!t)
        return -1;
    while (t->last > t->first && !w32->exiting) {
        void (*fn)(void) = *--t->last;
        if (fn)
            fn();
    }
    w32_free(t->first);
    t->first = t->last = t->end = NULL;
    return 0;
}

static int seh_filter(DWORD code, void *pointers)
{
    UNUSED(code);
    UNUSED(pointers);
    return 0;
}

static void type_info_destroy_list(void *list)
{
    UNUSED(list);
}

static void *current_exception(void)
{
    return &exception_object;
}

static void register_tls_atexit(void *fn)
{
    UNUSED(fn);
}

static void terminate(void)
{
    ((void (*)(void))lookup("abort"))();
}

static void *time_value(const char *name)
{
    void *(*fn)(void) = lookup(name);
    return fn ? fn() : NULL;
}

static int *daylight(void) { return time_value("__p__daylight"); }
static long *timezone(void) { return time_value("__p__timezone"); }
static char **tzname(void) { return time_value("__p__tzname"); }

static int ucrt_exponent(int format)
{
    return ((int (*)(int))lookup("_set_output_format"))(format);
}

#define UCRT_FORMAT(call)                                                                                     \
    do {                                                                                                       \
        int old_format = ucrt_exponent(1), result = (call);                                                    \
        ucrt_exponent(old_format);                                                                             \
        return result;                                                                                         \
    } while (0)

typedef int (*file_fmt)(void *, const void *, va_list);
typedef int (*buf_fmt)(void *, size_t, const void *, va_list);
typedef int (*buf3_fmt)(void *, size_t, size_t, const void *, va_list);

static int common_vfprintf(uint64_t options, void *f, const char *fmt, void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    UCRT_FORMAT(((file_fmt)lookup("vfprintf"))(f, fmt, ap));
}

static int common_vfwprintf(uint64_t options, void *f, const WCHAR *fmt, void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    UCRT_FORMAT(((file_fmt)lookup("vfwprintf"))(f, fmt, ap));
}

static int common_vsprintf(uint64_t options, char *buf, size_t count, const char *fmt, void *locale, va_list ap)
{
    UNUSED(locale);
    if (!buf || !count)
        UCRT_FORMAT(((int (*)(const void *, va_list))lookup("_vscprintf"))(fmt, ap));
    UCRT_FORMAT(((buf_fmt)lookup(options & PRINTF_STANDARD_SNPRINTF ? "vsnprintf" : "_vsnprintf"))(buf, count, fmt, ap));
}

static int common_vswprintf(uint64_t options, WCHAR *buf, size_t count, const WCHAR *fmt, void *locale, va_list ap)
{
    UNUSED(locale);
    if (!buf || !count)
        return ((int (*)(const void *, va_list))lookup("_vscwprintf"))(fmt, ap);
    int n = ((buf_fmt)lookup("_vsnwprintf"))(buf, count, fmt, ap);
    if (n < 0 && (options & PRINTF_STANDARD_SNPRINTF)) {
        buf[count - 1] = 0;
        return ((int (*)(const void *, va_list))lookup("_vscwprintf"))(fmt, ap);
    }
    return n;
}

static int common_vsprintf_s(uint64_t options, char *buf, size_t count, const char *fmt, void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    UCRT_FORMAT(((buf_fmt)lookup("vsprintf_s"))(buf, count, fmt, ap));
}

static int common_vswprintf_s(uint64_t options, WCHAR *buf, size_t count, const WCHAR *fmt, void *locale,
                              va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    return ((buf_fmt)lookup("vswprintf_s"))(buf, count, fmt, ap);
}

static int common_vsnprintf_s(uint64_t options, char *buf, size_t size, size_t count, const char *fmt, void *locale,
                              va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    UCRT_FORMAT(((buf3_fmt)lookup("_vsnprintf_s"))(buf, size, count, fmt, ap));
}

static int scan_limited(const char *scanner, const void *text, size_t count, size_t unit, const void *fmt, va_list ap)
{
    if (count == (size_t)-1)
        return ((file_fmt)lookup(scanner))((void *)text, fmt, ap);
    uint8_t *copy = w32_zalloc((count + 1) * unit);
    if (!copy)
        return -1;
    memcpy(copy, text, count * unit);
    int n = ((file_fmt)lookup(scanner))(copy, fmt, ap);
    w32_free(copy);
    return n;
}

static int common_vsscanf(uint64_t options, const char *text, size_t count, const char *fmt, void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    return scan_limited("vsscanf", text, count, 1, fmt, ap);
}

static int common_vswscanf(uint64_t options, const WCHAR *text, size_t count, const WCHAR *fmt, void *locale,
                           va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    return scan_limited("vswscanf", text, count, 2, fmt, ap);
}

static int common_vfscanf(uint64_t options, void *f, const char *fmt, void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    return ((file_fmt)lookup("vfscanf"))(f, fmt, ap);
}

static int common_vfwscanf(uint64_t options, void *f, const WCHAR *fmt, void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    return ((file_fmt)lookup("vfwscanf"))(f, fmt, ap);
}

static const struct w32_export ucrt_exports[] = {
    W32_AS("_setjmp3", crt_setjmp3),
    W32_AS("_setjmp", crt_setjmp3),
    W32_AS("setjmp", crt_setjmp3),
    W32_AS("longjmp", crt_longjmp),
    W32_AS("_set_new_mode", set_new_mode),
    W32_AS("_configure_narrow_argv", configure_argv),
    W32_AS("_configure_wide_argv", configure_argv),
    W32_AS("_initialize_narrow_environment", initialize_environment),
    W32_AS("_initialize_wide_environment", initialize_environment),
    W32_AS("_get_initial_narrow_environment", initial_environment),
    W32_AS("_get_initial_wide_environment", initial_wide_environment),
    W32_AS("_crt_at_quick_exit", at_quick_exit),
    W32_AS("at_quick_exit", at_quick_exit),
    W32_AS("_initialize_onexit_table", initialize_onexit_table),
    W32_AS("_register_onexit_function", register_onexit_function),
    W32_AS("_execute_onexit_table", execute_onexit_table),
    W32_AS("_seh_filter_exe", seh_filter),
    W32_AS("_seh_filter_dll", seh_filter),
    W32_AS("__std_type_info_destroy_list", type_info_destroy_list),
    W32_AS("__current_exception", current_exception),
    W32_AS("__current_exception_context", current_exception),
    W32_AS("_register_thread_local_exe_atexit_callback", register_tls_atexit),
    W32_AS("terminate", terminate),
    W32_AS("__daylight", daylight),
    W32_AS("__timezone", timezone),
    W32_AS("__tzname", tzname),
    W32_AS("__stdio_common_vfprintf", common_vfprintf),
    W32_AS("__stdio_common_vfprintf_s", common_vfprintf),
    W32_AS("__stdio_common_vfwprintf", common_vfwprintf),
    W32_AS("__stdio_common_vfwprintf_s", common_vfwprintf),
    W32_AS("__stdio_common_vsprintf", common_vsprintf),
    W32_AS("__stdio_common_vswprintf", common_vswprintf),
    W32_AS("__stdio_common_vsprintf_s", common_vsprintf_s),
    W32_AS("__stdio_common_vswprintf_s", common_vswprintf_s),
    W32_AS("__stdio_common_vsnprintf_s", common_vsnprintf_s),
    W32_AS("__stdio_common_vsscanf", common_vsscanf),
    W32_AS("__stdio_common_vswscanf", common_vswscanf),
    W32_AS("__stdio_common_vfscanf", common_vfscanf),
    W32_AS("__stdio_common_vfwscanf", common_vfwscanf),
};
CRT_EXPORTS(ucrt_module, ucrt_exports);
