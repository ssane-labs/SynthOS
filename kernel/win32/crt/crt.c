#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "crt.h"

#define SIGINT 2
#define SIGILL 4
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
#define SIGABRT 22
#define SIG_DFL 0
#define SIG_IGN 1
#define SIG_ERR ((uintptr_t)-1)

W32_STATE int crt_errno_value;
W32_STATE int crt_fmode;
static W32_STATE unsigned long crt_doserrno;
static W32_STATE int crt_commode;
static W32_STATE int crt_app_type;
static W32_STATE int crt_mb_cur_max = 1;
static W32_STATE int crt_argc;
static W32_STATE char **crt_argv;
static W32_STATE WCHAR **crt_wargv;
static W32_STATE char **crt_environ;
static W32_STATE WCHAR **crt_wenviron;
static W32_STATE char **crt_initenv;
static W32_STATE WCHAR **crt_winitenv;
static W32_STATE char *crt_acmdln;
static W32_STATE WCHAR *crt_wcmdln;
static W32_STATE char *crt_pgmptr;
static W32_STATE WCHAR *crt_wpgmptr;
static unsigned crt_osver = 19045, crt_winver = 0x0a00, crt_winmajor = 10, crt_winminor;
static W32_STATE uintptr_t signals[32];
static W32_STATE void *new_handler;
static W32_STATE void *purecall_handler;
static W32_STATE void *invalid_parameter_handler;

void crt_set_errno(int e)
{
    crt_errno_value = e;
}

int crt_errno_from_vfs(int err)
{
    switch (err) {
    case E_NOENT:
        return CRT_ENOENT;
    case E_EXIST:
        return CRT_EEXIST;
    case E_NOTDIR:
        return CRT_ENOTDIR;
    case E_ISDIR:
        return CRT_EISDIR;
    case E_NOTEMPTY:
        return CRT_ENOTEMPTY;
    case E_NOMEM:
        return CRT_ENOMEM;
    case E_NOSPC:
        return CRT_ENOSPC;
    case E_PERM:
        return CRT_EACCES;
    case E_BADF:
        return CRT_EBADF;
    case E_MFILE:
        return CRT_EMFILE;
    case E_NAMETOOLONG:
        return CRT_ENAMETOOLONG;
    case E_XDEV:
        return CRT_EXDEV;
    case E_NOSYS:
        return CRT_ENOSYS;
    default:
        return err < 0 ? CRT_EIO : CRT_EINVAL;
    }
}

static int *crt_errno(void)
{
    return &crt_errno_value;
}

static unsigned long *crt_doserrno_fn(void)
{
    return &crt_doserrno;
}

static int crt_get_errno(int *out)
{
    *out = crt_errno_value;
    return 0;
}

static int crt_set_errno_fn(int v)
{
    crt_errno_value = v;
    return 0;
}

static void crt_attach(void)
{
    crt_errno_value = 0;
    crt_doserrno = 0;
    crt_fmode = 0;
    crt_commode = 0;
    crt_app_type = 0;
    crt_argc = w32->argc;
    crt_argv = w32->argv;
    crt_wargv = w32->wargv;
    crt_environ = crt_initenv = w32->environ;
    crt_wenviron = crt_winitenv = w32->wenviron;
    crt_acmdln = w32->cmdline;
    crt_wcmdln = w32->cmdline_w;
    crt_pgmptr = w32->argv ? w32->argv[0] : NULL;
    crt_wpgmptr = w32->wargv ? w32->wargv[0] : NULL;
    memset(signals, 0, sizeof(signals));
    new_handler = purecall_handler = invalid_parameter_handler = NULL;
    crt_stdio_reset();
    crt_time_reset();
}

static int crt_getmainargs(int *argc, char ***argv, char ***env, int glob, void *info)
{
    UNUSED(glob);
    UNUSED(info);
    *argc = crt_argc;
    *argv = crt_argv;
    *env = crt_environ;
    return 0;
}

static int crt_wgetmainargs(int *argc, WCHAR ***argv, WCHAR ***env, int glob, void *info)
{
    UNUSED(glob);
    UNUSED(info);
    *argc = crt_argc;
    *argv = crt_wargv;
    *env = crt_wenviron;
    return 0;
}

static void crt_set_app_type(int type)
{
    crt_app_type = type;
}

static int *p_argc(void) { return &crt_argc; }
static char ***p_argv(void) { return &crt_argv; }
static WCHAR ***p_wargv(void) { return &crt_wargv; }
static char ***p_environ(void) { return &crt_environ; }
static WCHAR ***p_wenviron(void) { return &crt_wenviron; }
static char ***p_initenv(void) { return &crt_initenv; }
static WCHAR ***p_winitenv(void) { return &crt_winitenv; }
static int *p_fmode(void) { return &crt_fmode; }
static int *p_commode(void) { return &crt_commode; }
static char **p_acmdln(void) { return &crt_acmdln; }
static WCHAR **p_wcmdln(void) { return &crt_wcmdln; }
static char **p_pgmptr(void) { return &crt_pgmptr; }
static WCHAR **p_wpgmptr(void) { return &crt_wpgmptr; }
static int *p_mb_cur_max(void) { return &crt_mb_cur_max; }
static int mb_cur_max_func(void) { return crt_mb_cur_max; }

static int crt_get_pgmptr(char **out)
{
    *out = crt_pgmptr;
    return 0;
}

static int crt_get_fmode(int *out)
{
    *out = crt_fmode;
    return 0;
}

static int crt_set_fmode(int mode)
{
    crt_fmode = mode;
    return 0;
}

typedef void (*vfn)(void);
typedef int (*ifn)(void);

static void crt_initterm(vfn *start, vfn *end)
{
    for (; start < end; start++)
        if (*start)
            (*start)();
}

static int crt_initterm_e(ifn *start, ifn *end)
{
    for (; start < end; start++)
        if (*start) {
            int r = (*start)();
            if (r)
                return r;
        }
    return 0;
}

static vfn crt_onexit(vfn fn)
{
    if (w32->atexit_count >= W32_ATEXIT_MAX)
        return NULL;
    w32->atexit_fns[w32->atexit_count++] = fn;
    return fn;
}

static int crt_atexit(vfn fn)
{
    return crt_onexit(fn) ? 0 : -1;
}

static vfn crt_dllonexit(vfn fn, vfn **start, vfn **end)
{
    size_t n = (size_t)(*end - *start);
    vfn *grown = w32_realloc(*start, (n + 1) * sizeof(vfn));
    if (!grown)
        return NULL;
    grown[n] = fn;
    *start = grown;
    *end = grown + n + 1;
    return fn;
}

void crt_exit(int code)
{
    w32_run_atexit();
    crt_close_all();
    w32_exit((uint32_t)code);
}

static void crt_exit_fn(int code)
{
    crt_exit(code);
}

static void crt_quick_exit(int code)
{
    crt_flush_all();
    w32_exit((uint32_t)code);
}

static void crt_cexit(void)
{
    w32_run_atexit();
    crt_flush_all();
}

static void crt_c_exit(void)
{
    crt_flush_all();
}

static void crt_abort(void)
{
    crt_flush_all();
    uintptr_t h = signals[SIGABRT];
    if (h > SIG_IGN && h != SIG_ERR) {
        signals[SIGABRT] = SIG_DFL;
        ((void (*)(int))h)(SIGABRT);
    }
    str_copy(w32->message, "abnormal program termination", sizeof(w32->message));
    w32_exit(3);
}

static void crt_amsg_exit(int code)
{
    ksnprintf(w32->message, sizeof(w32->message), "runtime error R60%02d", code);
    w32_exit(255);
}

static void crt_assert(const char *expr, const char *file, unsigned line)
{
    char buf[256];
    int n = ksnprintf(buf, sizeof(buf), "Assertion failed: %s, file %s, line %u\n", expr, file, line);
    crt_fd_write(2, buf, (size_t)MIN(n, (int)sizeof(buf) - 1));
    crt_abort();
}

static void crt_wassert(const WCHAR *expr, const WCHAR *file, unsigned line)
{
    char *e = w32_narrow(expr), *f = w32_narrow(file);
    crt_assert(e, f, line);
}

static uintptr_t crt_signal(int sig, uintptr_t handler)
{
    if (sig <= 0 || sig >= 32) {
        crt_set_errno(CRT_EINVAL);
        return SIG_ERR;
    }
    uintptr_t old = signals[sig];
    signals[sig] = handler;
    return old;
}

static int crt_raise(int sig)
{
    if (sig <= 0 || sig >= 32)
        return -1;
    uintptr_t h = signals[sig];
    if (h == SIG_IGN)
        return 0;
    if (h == SIG_DFL) {
        if (sig == SIGABRT)
            crt_abort();
        crt_flush_all();
        w32_exit(sig == SIGINT ? STATUS_CONTROL_C_EXIT : 3);
    }
    signals[sig] = SIG_DFL;
    ((void (*)(int))h)(sig);
    return 0;
}

static int crt_xcpt_filter(unsigned code, void *ptrs)
{
    UNUSED(code);
    UNUSED(ptrs);
    return 0;
}

static void *crt_set_new_handler(void *h)
{
    void *old = new_handler;
    new_handler = h;
    return old;
}

static void *crt_set_purecall(void *h)
{
    void *old = purecall_handler;
    purecall_handler = h;
    return old;
}

static void *crt_set_invalid_parameter(void *h)
{
    void *old = invalid_parameter_handler;
    invalid_parameter_handler = h;
    return old;
}

static void crt_purecall(void)
{
    str_copy(w32->message, "pure virtual function call", sizeof(w32->message));
    w32_exit(255);
}

static void crt_invalid_parameter(void)
{
    crt_set_errno(CRT_EINVAL);
}

static void crt_nop(void)
{
}

static int crt_zero(void)
{
    return 0;
}

static void crt_lock(int n)
{
    UNUSED(n);
}

static int crt_setusermatherr(void *fn)
{
    UNUSED(fn);
    return 0;
}

static unsigned x87_to_abstract(uint16_t cw)
{
    static const unsigned masks[6] = {0x10, 0x80000, 0x08, 0x04, 0x02, 0x01};
    unsigned a = 0;
    for (int i = 0; i < 6; i++)
        if (cw & (1u << i))
            a |= masks[i];
    unsigned pc = (cw >> 8) & 3, rc = (cw >> 10) & 3;
    a |= pc == 0 ? 0x20000 : pc == 2 ? 0x10000 : 0;
    a |= rc << 8;
    return a;
}

static uint16_t abstract_to_x87(unsigned a, uint16_t cw)
{
    static const unsigned masks[6] = {0x10, 0x80000, 0x08, 0x04, 0x02, 0x01};
    cw &= (uint16_t)~0x0f3f;
    for (int i = 0; i < 6; i++)
        if (a & masks[i])
            cw |= (uint16_t)(1u << i);
    unsigned pc = a & 0x30000;
    cw |= (uint16_t)((pc == 0x20000 ? 0 : pc == 0x10000 ? 2 : 3) << 8);
    cw |= (uint16_t)(((a >> 8) & 3) << 10);
    return cw;
}

static unsigned crt_controlfp(unsigned value, unsigned mask)
{
    uint16_t cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    unsigned cur = (x87_to_abstract(cw) & ~mask) | (value & mask);
    uint16_t ncw = abstract_to_x87(cur, cw);
    __asm__ volatile("fldcw %0" : : "m"(ncw));
    return cur;
}

static int crt_controlfp_s(unsigned *cur, unsigned value, unsigned mask)
{
    unsigned v = crt_controlfp(value, mask);
    if (cur)
        *cur = v;
    return 0;
}

static int crt_control87_2(unsigned value, unsigned mask, unsigned *x87, unsigned *sse)
{
    unsigned v = crt_controlfp(value, mask);
    if (x87)
        *x87 = v;
    if (sse)
        *sse = v & ~0x30000u;
    return 1;
}

static void crt_fpreset(void)
{
    __asm__ volatile("fninit");
}

static unsigned crt_clearfp(void)
{
    uint16_t sw;
    __asm__ volatile("fnstsw %0; fnclex" : "=m"(sw));
    return sw & 0x3f;
}

static void *crt_malloc(size_t n)
{
    void *p = w32_alloc(n ? n : 1);
    if (!p)
        crt_set_errno(CRT_ENOMEM);
    return p;
}

static void *crt_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) {
        crt_set_errno(CRT_ENOMEM);
        return NULL;
    }
    size_t total = n * size;
    void *p = w32_zalloc(total ? total : 1);
    if (!p)
        crt_set_errno(CRT_ENOMEM);
    return p;
}

static void *crt_realloc(void *p, size_t n)
{
    if (!p)
        return crt_malloc(n);
    if (!n) {
        w32_free(p);
        return NULL;
    }
    void *r = w32_realloc(p, n);
    if (!r)
        crt_set_errno(CRT_ENOMEM);
    return r;
}

static void *crt_recalloc(void *p, size_t n, size_t size)
{
    size_t old = p ? w32_msize(p) : 0, want = n * size;
    void *r = crt_realloc(p, want);
    if (r && want > old)
        memset((char *)r + old, 0, want - old);
    return r;
}

static void crt_free(void *p)
{
    w32_free(p);
}

static size_t crt_msize(void *p)
{
    return p ? w32_msize(p) : (size_t)-1;
}

static void *crt_expand(void *p, size_t n)
{
    return n <= w32_msize(p) ? p : NULL;
}

static void *crt_aligned_malloc(size_t n, size_t align)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    uint8_t *raw = w32_alloc(n + align + sizeof(void *));
    if (!raw)
        return NULL;
    uintptr_t p = ALIGN_UP((uintptr_t)(raw + sizeof(void *)), align);
    ((void **)p)[-1] = raw;
    return (void *)p;
}

static void crt_aligned_free(void *p)
{
    if (p)
        w32_free(((void **)p)[-1]);
}

static void *crt_aligned_realloc(void *p, size_t n, size_t align)
{
    if (!p)
        return crt_aligned_malloc(n, align);
    void *q = crt_aligned_malloc(n, align);
    if (q) {
        uint8_t *raw = ((void **)p)[-1];
        size_t old = w32_msize(raw) - (size_t)((uint8_t *)p - raw);
        memcpy(q, p, MIN(old, n));
        crt_aligned_free(p);
    }
    return q;
}

static int crt_heapchk(void)
{
    return -2;
}

static int crt_heapmin(void)
{
    return 0;
}

static intptr_t crt_get_heap_handle(void)
{
    return (intptr_t)&w32->heap;
}

static char *crt_getenv(const char *name)
{
    return (char *)w32_getenv(name);
}

static WCHAR *crt_wgetenv(const WCHAR *name)
{
    char *n = w32_narrow(name);
    const char *v = w32_getenv(n);
    w32_free(n);
    return v ? w32_widen(v) : NULL;
}

static int crt_getenv_s(size_t *len, char *buf, size_t cap, const char *name)
{
    const char *v = w32_getenv(name);
    size_t n = v ? strlen(v) + 1 : 0;
    if (len)
        *len = n;
    if (!v) {
        if (buf && cap)
            buf[0] = 0;
        return 0;
    }
    if (n > cap)
        return CRT_ERANGE;
    memcpy(buf, v, n);
    return 0;
}

static int crt_dupenv_s(char **out, size_t *len, const char *name)
{
    const char *v = w32_getenv(name);
    *out = v ? w32_strdup(v) : NULL;
    if (len)
        *len = v ? strlen(v) + 1 : 0;
    return 0;
}

static void sync_environ(void)
{
    crt_environ = w32->environ;
}

static int crt_putenv(const char *entry)
{
    const char *eq = strchr(entry, '=');
    if (!eq) {
        crt_set_errno(CRT_EINVAL);
        return -1;
    }
    char name[128];
    size_t n = MIN((size_t)(eq - entry), sizeof(name) - 1);
    memcpy(name, entry, n);
    name[n] = 0;
    int r = w32_putenv(name, eq[1] ? eq + 1 : NULL);
    sync_environ();
    return r;
}

static int crt_wputenv(const WCHAR *entry)
{
    char *e = w32_narrow(entry);
    int r = crt_putenv(e);
    w32_free(e);
    return r;
}

static int crt_putenv_s(const char *name, const char *value)
{
    int r = w32_putenv(name, value && *value ? value : NULL);
    sync_environ();
    return r ? CRT_EINVAL : 0;
}

static int crt_wputenv_s(const WCHAR *name, const WCHAR *value)
{
    char *n = w32_narrow(name), *v = w32_narrow(value ? value : (const WCHAR *)L"");
    int r = n && v ? crt_putenv_s(n, v) : CRT_EINVAL;
    w32_free(n);
    w32_free(v);
    return r;
}

static uint32_t system_thunk(void *arg)
{
    struct shell *sh = w32->ctx->sh;
    char saved[PATH_MAX_LEN];
    str_copy(saved, sh->cwd, sizeof(saved));
    str_copy(sh->cwd, w32->cwd, sizeof(sh->cwd));
    int rc = shell_exec(sh, arg);
    str_copy(sh->cwd, saved, sizeof(sh->cwd));
    return (uint32_t)rc;
}

static int crt_system(const char *cmd)
{
    if (!cmd)
        return 1;
    crt_flush_all();
    char *copy = kstrdup(cmd);
    if (!copy)
        return -1;
    int rc = (int)w32_kernel_call(system_thunk, copy);
    kfree(copy);
    return rc;
}

static int crt_wsystem(const WCHAR *cmd)
{
    char *c = cmd ? w32_narrow(cmd) : NULL;
    int r = crt_system(c);
    w32_free(c);
    return r;
}

static int crt_getpid(void)
{
    return 0x1000;
}

static void crt_sleep(unsigned long ms)
{
    w32_sleep((uint32_t)ms);
}

static void crt_beep(unsigned freq, unsigned ms)
{
    hal_beep(freq, ms);
}

static char *crt_setlocale(int category, const char *locale)
{
    UNUSED(category);
    static W32_STATE char name[32] = "C";
    if (locale && *locale)
        str_copy(name, locale, sizeof(name));
    else if (locale)
        str_copy(name, w32_acp() == 1251 ? "Russian_Russia.1251" : "English_United States.1252", sizeof(name));
    return name;
}

static WCHAR *crt_wsetlocale(int category, const WCHAR *locale)
{
    char *l = locale ? w32_narrow(locale) : NULL;
    char *r = crt_setlocale(category, l);
    w32_free(l);
    static WCHAR out[32];
    w32_cp_to_wide(r, -1, out, 32, 0);
    return out;
}

struct crt_lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
        *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn,
        n_sign_posn;
};

static struct crt_lconv *crt_localeconv(void)
{
    static struct crt_lconv lc = {".", "", "", "", "", "", "", "", "", "", 127, 127, 127, 127, 127, 127, 127, 127};
    return &lc;
}

static int crt_configthreadlocale(int v)
{
    UNUSED(v);
    return 2;
}

static void *crt_get_current_locale(void)
{
    static uint32_t dummy[4];
    return dummy;
}

static unsigned crt_getmbcp(void)
{
    return 0;
}

static int crt_setmbcp(int cp)
{
    UNUSED(cp);
    return 0;
}

static unsigned *crt_lc_codepage(void)
{
    static unsigned cp;
    cp = (unsigned)w32_acp();
    return &cp;
}

static int crt_resetstkoflw(void)
{
    return 1;
}

static unsigned long crt_threadid(void)
{
    return 0x1004;
}

struct thread_start {
    void (*fn)(void *);
    void *arg;
};

static DWORD WINAPI begin_trampoline(void *p)
{
    struct thread_start start = *(struct thread_start *)p;
    w32_free(p);
    start.fn(start.arg);
    return 0;
}

static uintptr_t crt_beginthread(void *fn, unsigned stack, void *arg)
{
    struct thread_start *start = w32_alloc(sizeof(*start));
    if (!start) {
        crt_set_errno(CRT_EINVAL);
        return (uintptr_t)-1;
    }
    start->fn = (void (*)(void *))fn;
    start->arg = arg;
    HANDLE h = w32_create_thread(stack, (void *)begin_trampoline, start, 0, NULL);
    return h ? (uintptr_t)h : (uintptr_t)-1;
}

static uintptr_t crt_beginthreadex(void *sec, unsigned stack, void *fn, void *arg, unsigned flags, unsigned *id)
{
    UNUSED(sec);
    DWORD tid = 0;
    HANDLE h = w32_create_thread(stack, fn, arg, flags, &tid);
    if (id)
        *id = tid;
    return (uintptr_t)h;
}

static void crt_endthreadex(unsigned code)
{
    typedef void WINAPI (*exit_fn)(DWORD);
    struct w32_image *k = w32_find_module("kernel32.dll");
    exit_fn fn = k ? (exit_fn)w32_get_proc(k, "ExitThread", 0) : NULL;
    if (fn)
        fn(code);
}

static void crt_endthread(void)
{
    crt_endthreadex(0);
}

static unsigned long crt_current_threadid(void)
{
    return w32_thread_id();
}

static const struct w32_export crt_exports[] = {
    W32_AS("_errno", crt_errno), W32_AS("__doserrno", crt_doserrno_fn), W32_AS("_get_errno", crt_get_errno),
    W32_AS("_set_errno", crt_set_errno_fn), W32_AS("__getmainargs", crt_getmainargs),
    W32_AS("__wgetmainargs", crt_wgetmainargs), W32_AS("__set_app_type", crt_set_app_type),
    W32_AS("_set_app_type", crt_set_app_type), W32_AS("__p___argc", p_argc), W32_AS("__p___argv", p_argv),
    W32_AS("__p___wargv", p_wargv), W32_AS("__p__environ", p_environ), W32_AS("__p__wenviron", p_wenviron),
    W32_AS("__p___initenv", p_initenv), W32_AS("__p___winitenv", p_winitenv), W32_AS("__p__fmode", p_fmode),
    W32_AS("__p__commode", p_commode), W32_AS("__p__acmdln", p_acmdln), W32_AS("__p__wcmdln", p_wcmdln),
    W32_AS("__p__pgmptr", p_pgmptr), W32_AS("__p__wpgmptr", p_wpgmptr), W32_AS("__p___mb_cur_max", p_mb_cur_max),
    W32_AS("___mb_cur_max_func", mb_cur_max_func), W32_AS("_get_pgmptr", crt_get_pgmptr),
    W32_AS("_get_fmode", crt_get_fmode), W32_AS("_set_fmode", crt_set_fmode), W32_AS("__argc", &crt_argc),
    W32_AS("__argv", &crt_argv), W32_AS("__wargv", &crt_wargv), W32_AS("_environ", &crt_environ),
    W32_AS("_wenviron", &crt_wenviron), W32_AS("__initenv", &crt_initenv), W32_AS("__winitenv", &crt_winitenv),
    W32_AS("_fmode", &crt_fmode), W32_AS("_commode", &crt_commode), W32_AS("_acmdln", &crt_acmdln),
    W32_AS("_wcmdln", &crt_wcmdln), W32_AS("_pgmptr", &crt_pgmptr), W32_AS("_wpgmptr", &crt_wpgmptr),
    W32_AS("__mb_cur_max", &crt_mb_cur_max), W32_AS("_osver", &crt_osver), W32_AS("_winver", &crt_winver),
    W32_AS("_winmajor", &crt_winmajor), W32_AS("_winminor", &crt_winminor), W32_AS("_initterm", crt_initterm),
    W32_AS("_initterm_e", crt_initterm_e), W32_AS("_onexit", crt_onexit), W32_AS("atexit", crt_atexit),
    W32_AS("_crt_atexit", crt_atexit), W32_AS("__dllonexit", crt_dllonexit), W32_AS("exit", crt_exit_fn),
    W32_AS("_exit", crt_quick_exit), W32_AS("_Exit", crt_quick_exit), W32_AS("quick_exit", crt_quick_exit),
    W32_AS("_cexit", crt_cexit), W32_AS("_c_exit", crt_c_exit), W32_AS("abort", crt_abort),
    W32_AS("_amsg_exit", crt_amsg_exit), W32_AS("_assert", crt_assert), W32_AS("_wassert", crt_wassert),
    W32_AS("signal", crt_signal), W32_AS("raise", crt_raise), W32_AS("_XcptFilter", crt_xcpt_filter),
    W32_AS("_set_new_handler", crt_set_new_handler), W32_AS("_set_purecall_handler", crt_set_purecall),
    W32_AS("_set_invalid_parameter_handler", crt_set_invalid_parameter),
    W32_AS("_set_thread_local_invalid_parameter_handler", crt_set_invalid_parameter),
    W32_AS("_purecall", crt_purecall), W32_AS("_invalid_parameter", crt_invalid_parameter),
    W32_AS("_invalid_parameter_noinfo", crt_invalid_parameter),
    W32_AS("_invalid_parameter_noinfo_noreturn", crt_invalid_parameter), W32_AS("_lock", crt_lock),
    W32_AS("_unlock", crt_lock), W32_AS("_lock_file", crt_lock_file), W32_AS("_unlock_file", crt_unlock_file),
    W32_AS("__setusermatherr", crt_setusermatherr), W32_AS("__lconv_init", crt_zero),
    W32_AS("_configthreadlocale", crt_configthreadlocale), W32_AS("_get_current_locale", crt_get_current_locale),
    W32_AS("_controlfp", crt_controlfp), W32_AS("_control87", crt_controlfp),
    W32_AS("__control87_2", crt_control87_2), W32_AS("_controlfp_s", crt_controlfp_s),
    W32_AS("_fpreset", crt_fpreset), W32_AS("_clearfp", crt_clearfp), W32_AS("_statusfp", crt_clearfp),
    W32_AS("malloc", crt_malloc), W32_AS("calloc", crt_calloc), W32_AS("realloc", crt_realloc),
    W32_AS("_recalloc", crt_recalloc), W32_AS("free", crt_free), W32_AS("_msize", crt_msize),
    W32_AS("_expand", crt_expand), W32_AS("_aligned_malloc", crt_aligned_malloc),
    W32_AS("_aligned_free", crt_aligned_free), W32_AS("_aligned_realloc", crt_aligned_realloc),
    W32_AS("_heapchk", crt_heapchk), W32_AS("_heapmin", crt_heapmin), W32_AS("_get_heap_handle", crt_get_heap_handle),
    W32_AS("getenv", crt_getenv), W32_AS("_wgetenv", crt_wgetenv), W32_AS("getenv_s", crt_getenv_s),
    W32_AS("_dupenv_s", crt_dupenv_s), W32_AS("_putenv", crt_putenv), W32_AS("putenv", crt_putenv),
    W32_AS("_wputenv", crt_wputenv), W32_AS("_putenv_s", crt_putenv_s), W32_AS("_wputenv_s", crt_wputenv_s), W32_AS("system", crt_system),
    W32_AS("_wsystem", crt_wsystem), W32_AS("_getpid", crt_getpid), W32_AS("getpid", crt_getpid),
    W32_AS("_sleep", crt_sleep), W32_AS("_beep", crt_beep), W32_AS("setlocale", crt_setlocale),
    W32_AS("_wsetlocale", crt_wsetlocale), W32_AS("localeconv", crt_localeconv), W32_AS("_getmbcp", crt_getmbcp),
    W32_AS("_setmbcp", crt_setmbcp), W32_AS("___lc_codepage_func", crt_lc_codepage),
    W32_AS("_resetstkoflw", crt_resetstkoflw), W32_AS("__threadid", crt_current_threadid),
    W32_AS("__threadhandle", crt_threadid), W32_AS("_beginthread", crt_beginthread),
    W32_AS("_beginthreadex", crt_beginthreadex), W32_AS("_endthread", crt_endthread), W32_AS("_endthreadex", crt_endthreadex),
    W32_AS("__CppXcptFilter", crt_xcpt_filter), W32_AS("_crt_debugger_hook", crt_nop),
};

W32_EXPORTS(crt_core_module, "msvcrt.dll", crt_exports, .attach = crt_attach);
