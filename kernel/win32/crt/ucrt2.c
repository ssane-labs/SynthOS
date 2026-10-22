#include <synth/string.h>

#include "crt.h"

/* More of the Universal CRT: secure variants and helpers Firefox and the MSVC C++ library use. */

static void *lookup(const char *name)
{
    return w32_builtin_lookup("msvcrt.dll", name);
}

static void lock_locales(void)
{
}

static int ltoa_s(long v, char *out, size_t cap, int base)
{
    return ((int (*)(int, char *, size_t, int))lookup("_itoa_s"))((int)v, out, cap, base);
}

static WCHAR *wfullpath(WCHAR *buf, const WCHAR *path, size_t size)
{
    DWORD(WINAPI * full)(LPCWSTR, DWORD, LPWSTR, LPWSTR *) =
        (DWORD(WINAPI *)(LPCWSTR, DWORD, LPWSTR, LPWSTR *))w32_builtin_lookup("kernel32.dll", "GetFullPathNameW");
    if (!path || !full)
        return NULL;
    bool own = !buf;
    if (own) {
        size = PATH_MAX_LEN;
        if (!(buf = w32_alloc(size * sizeof(WCHAR))))
            return NULL;
    }
    DWORD n = full(path, (DWORD)size, buf, NULL);
    if (!n || n >= size) {
        if (own)
            w32_free(buf);
        crt_set_errno(n ? 34 : CRT_EINVAL);
        return NULL;
    }
    return buf;
}

static int put_part(WCHAR *dst, size_t cap, const WCHAR *from, size_t n)
{
    if (!dst)
        return 0;
    if (n >= cap) {
        if (cap)
            dst[0] = 0;
        return 34;
    }
    memcpy(dst, from, n * sizeof(WCHAR));
    dst[n] = 0;
    return 0;
}

static int wsplitpath_s(const WCHAR *path, WCHAR *drive, size_t drive_n, WCHAR *dir, size_t dir_n, WCHAR *name,
                        size_t name_n, WCHAR *ext, size_t ext_n)
{
    if (!path)
        return CRT_EINVAL;
    const WCHAR *p = path;
    size_t len = w32_wcslen(path);
    int err = 0;
    if (len >= 2 && path[1] == ':') {
        err |= put_part(drive, drive_n, path, 2);
        p += 2;
    } else {
        err |= put_part(drive, drive_n, p, 0);
    }
    const WCHAR *end = path + len, *last = NULL, *dot = NULL;
    for (const WCHAR *q = p; q < end; q++) {
        if (*q == '\\' || *q == '/')
            last = q;
        else if (*q == '.')
            dot = q;
    }
    const WCHAR *base = last ? last + 1 : p;
    if (dot && dot < base)
        dot = NULL;
    err |= put_part(dir, dir_n, p, (size_t)(base - p));
    err |= put_part(name, name_n, base, (size_t)((dot ? dot : end) - base));
    err |= put_part(ext, ext_n, dot ? dot : end, dot ? (size_t)(end - dot) : 0);
    if (err)
        crt_set_errno(34);
    return err ? 34 : 0;
}

static int common_vsnwprintf_s(uint64_t options, WCHAR *buf, size_t size, size_t count, const WCHAR *fmt,
                               void *locale, va_list ap)
{
    UNUSED(options);
    UNUSED(locale);
    if (!buf || !size)
        return -1;
    size_t limit = count == (size_t)-1 ? size - 1 : MIN(count, size - 1);
    int n = ((int (*)(WCHAR *, size_t, const WCHAR *, va_list))lookup("_vsnwprintf"))(buf, limit, fmt, ap);
    if (n < 0 || (size_t)n >= limit + (count == (size_t)-1 ? 1 : 0)) {
        buf[limit] = 0;
        return count == (size_t)-1 ? -1 : (n < 0 || (size_t)n > limit ? -1 : n);
    }
    buf[n] = 0;
    return n;
}

static int stream_buffer_pointers(CRT_FILE *f, char ***base, char ***ptr, int **cnt)
{
    if (!f)
        return CRT_EINVAL;
    if (base)
        *base = &f->base;
    if (ptr)
        *ptr = &f->ptr;
    if (cnt)
        *cnt = &f->cnt;
    return 0;
}

static int wcsupr_s(WCHAR *s, size_t cap)
{
    if (!s || w32_wcslen(s) >= cap)
        return CRT_EINVAL;
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s = (WCHAR)(*s - 32);
    return 0;
}

static int wcslwr_s(WCHAR *s, size_t cap)
{
    if (!s || w32_wcslen(s) >= cap)
        return CRT_EINVAL;
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (WCHAR)(*s + 32);
    return 0;
}

static size_t mbsrtowcs(WCHAR *dst, const char **src, size_t n, void *state)
{
    UNUSED(state);
    const char *s = *src;
    size_t i = 0;
    for (; !dst || i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (dst)
            dst[i] = c;
        if (!c) {
            if (dst)
                *src = NULL;
            return i;
        }
    }
    *src = s + i;
    return i;
}

struct imaxdiv_result {
    int64_t quot, rem;
};

static struct imaxdiv_result imaxdiv(int64_t num, int64_t den)
{
    return (struct imaxdiv_result){num / den, num % den};
}

static WCHAR *wgetdcwd(int drive, WCHAR *buf, int size)
{
    UNUSED(drive);
    return ((WCHAR * (*)(WCHAR *, int)) lookup("_wgetcwd"))(buf, size);
}

static unsigned long getdrives(void)
{
    return 1u << 2;
}

static int mktemp_s(char *tmpl, size_t size)
{
    return ((char *(*)(char *))lookup("_mktemp"))(tmpl) && strlen(tmpl) < size ? 0 : CRT_EINVAL;
}

static int wmktemp_s(WCHAR *tmpl, size_t size)
{
    return ((WCHAR * (*)(WCHAR *)) lookup("_wmktemp"))(tmpl) && w32_wcslen(tmpl) < size ? 0 : CRT_EINVAL;
}

static void *wfsopen(const WCHAR *name, const WCHAR *mode, int share)
{
    UNUSED(share);
    return ((void *(*)(const WCHAR *, const WCHAR *))lookup("_wfopen"))(name, mode);
}

static const unsigned char *mbsinc(const unsigned char *s)
{
    return s + 1;
}

static unsigned char *mbsdec(const unsigned char *start, const unsigned char *s)
{
    return s > start ? (unsigned char *)s - 1 : NULL;
}


static void *base_alloc(size_t n)
{
    return ((void *(*)(size_t))lookup("malloc"))(n);
}

static void *calloc_base(size_t n, size_t size)
{
    return ((void *(*)(size_t, size_t))lookup("calloc"))(n, size);
}

static void free_base(void *p)
{
    ((void (*)(void *))lookup("free"))(p);
}

static int callnewh(size_t size)
{
    UNUSED(size);
    return 0;
}

static void *aligned_offset_malloc(size_t n, size_t align, size_t offset)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    uint8_t *raw = w32_alloc(n + align + offset + sizeof(void *));
    if (!raw)
        return NULL;
    uintptr_t p = ALIGN_UP((uintptr_t)(raw + sizeof(void *) + offset), align) - offset;
    ((void **)p)[-1] = raw;
    return (void *)p;
}

static WCHAR *no_locale_names[6];

static WCHAR **lc_locale_name(void)
{
    return no_locale_names;
}

static unsigned lc_collate_cp(void)
{
    return 0;
}

struct crt_locale {
    void *locinfo, *mbcinfo;
};

static struct crt_locale *create_locale(int category, const char *name)
{
    UNUSED(category);
    UNUSED(name);
    struct crt_locale *l = w32_zalloc(sizeof(*l) + 2 * 1024);
    if (!l)
        return NULL;
    l->locinfo = (uint8_t *)(l + 1);
    l->mbcinfo = (uint8_t *)(l + 1) + 1024;
    return l;
}

static void free_locale(struct crt_locale *l)
{
    w32_free(l);
}

struct std_exception_data {
    const char *what;
    bool do_free;
};

static void std_exception_copy(const struct std_exception_data *from, struct std_exception_data *to)
{
    if (!from->do_free || !from->what) {
        *to = *from;
        return;
    }
    char *copy = w32_alloc(strlen(from->what) + 1);
    if (copy)
        str_copy(copy, from->what, strlen(from->what) + 1);
    to->what = copy;
    to->do_free = copy != NULL;
}

static void std_exception_destroy(struct std_exception_data *data)
{
    if (data->do_free)
        w32_free((void *)data->what);
    data->what = NULL;
    data->do_free = false;
}

static int wcsicmp_l(const WCHAR *a, const WCHAR *b, void *locale)
{
    UNUSED(locale);
    return ((int (*)(const WCHAR *, const WCHAR *))lookup("_wcsicmp"))(a, b);
}

static int towlower_l(int c, void *locale)
{
    UNUSED(locale);
    return ((int (*)(int))lookup("towlower"))(c);
}

static int towupper_l(int c, void *locale)
{
    UNUSED(locale);
    return ((int (*)(int))lookup("towupper"))(c);
}

static int wmemcpy_s(WCHAR *dst, size_t cap, const WCHAR *src, size_t n)
{
    if (!n)
        return 0;
    if (!dst || !src || n > cap)
        return CRT_EINVAL;
    memcpy(dst, src, n * sizeof(WCHAR));
    return 0;
}

static size_t strncnt(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i])
        i++;
    return i;
}

static int i64tow_s(int64_t v, WCHAR *out, size_t cap, int base)
{
    WCHAR tmp[72];
    ((WCHAR * (*)(int64_t, WCHAR *, int)) lookup("_i64tow"))(v, tmp, base);
    size_t n = w32_wcslen(tmp);
    if (!out || n >= cap)
        return CRT_EINVAL;
    memcpy(out, tmp, (n + 1) * sizeof(WCHAR));
    return 0;
}

static int splitpath_s(const char *path, char *drive, size_t dn, char *dir, size_t dirn, char *name, size_t nn,
                       char *ext, size_t en)
{
    WCHAR *w = w32_widen(path), d[8], di[PATH_MAX_LEN], na[PATH_MAX_LEN], ex[PATH_MAX_LEN];
    if (!w)
        return CRT_EINVAL;
    int err = wsplitpath_s(w, d, 8, di, PATH_MAX_LEN, na, PATH_MAX_LEN, ex, PATH_MAX_LEN);
    w32_free(w);
    const WCHAR *parts[4] = {d, di, na, ex};
    char *outs[4] = {drive, dir, name, ext};
    size_t caps[4] = {dn, dirn, nn, en};
    for (int i = 0; i < 4 && !err; i++) {
        if (!outs[i])
            continue;
        size_t n = w32_wcslen(parts[i]);
        if (n >= caps[i])
            return 34;
        for (size_t k = 0; k <= n; k++)
            outs[i][k] = (char)parts[i][k];
    }
    return err;
}

static int makepath_s(char *out, size_t cap, const char *drive, const char *dir, const char *name, const char *ext)
{
    if (!out || !cap)
        return CRT_EINVAL;
    out[0] = 0;
    if (drive && *drive) {
        str_append(out, drive, cap);
        if (drive[1] != ':')
            str_append(out, ":", cap);
    }
    if (dir && *dir) {
        str_append(out, dir, cap);
        char last = dir[strlen(dir) - 1];
        if (last != '\\' && last != '/')
            str_append(out, "\\", cap);
    }
    if (name)
        str_append(out, name, cap);
    if (ext && *ext) {
        if (*ext != '.')
            str_append(out, ".", cap);
        str_append(out, ext, cap);
    }
    return strlen(out) + 1 >= cap ? 34 : 0;
}

static WCHAR *wide_winmain_command_line(void)
{
    WCHAR **line = lookup("_wcmdln");
    WCHAR *p = line ? *line : NULL;
    if (!p)
        return NULL;
    bool quoted = false;
    for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++)
        if (*p == '"')
            quoted = !quoted;
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static void invoke_watson(const WCHAR *expr, const WCHAR *fn, const WCHAR *file, unsigned line, uintptr_t reserved)
{
    UNUSED(expr);
    UNUSED(fn);
    UNUSED(file);
    UNUSED(line);
    UNUSED(reserved);
    ((void (*)(void))lookup("abort"))();
}

static int set_doserrno(unsigned long v)
{
    UNUSED(v);
    return 0;
}

static int wsopen_s(int *fd, const WCHAR *name, int flags, int share, int mode)
{
    UNUSED(share);
    *fd = ((int (*)(const WCHAR *, int, int, int))lookup("_wsopen"))(name, flags, 0x40, mode);
    return *fd < 0 ? *(int *)((int *(*)(void))lookup("_errno"))() : 0;
}

static const char *const day_names[14] = {"Sun", "Sunday", "Mon", "Monday", "Tue", "Tuesday", "Wed", "Wednesday",
                                          "Thu", "Thursday", "Fri", "Friday", "Sat", "Saturday"};
static const char *const month_names[24] = {"Jan", "January", "Feb", "February", "Mar", "March", "Apr", "April",
                                            "May", "May", "Jun", "June", "Jul", "July", "Aug", "August",
                                            "Sep", "September", "Oct", "October", "Nov", "November", "Dec", "December"};

static char *joined(const char *const *names, int n)
{
    size_t len = 1;
    for (int i = 0; i < n; i++)
        len += strlen(names[i]) + 1;
    char *out = base_alloc(len);
    if (!out)
        return NULL;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        str_append(out, ":", len);
        str_append(out, names[i], len);
    }
    return out;
}

static char *getdays(void)
{
    return joined(day_names, 14);
}

static char *getmonths(void)
{
    return joined(month_names, 24);
}

static WCHAR *widen_joined(char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    WCHAR *w = base_alloc((n + 1) * sizeof(WCHAR));
    for (size_t i = 0; w && i <= n; i++)
        w[i] = (uint8_t)s[i];
    free_base(s);
    return w;
}

static WCHAR *w_getdays(void)
{
    return widen_joined(getdays());
}

static WCHAR *w_getmonths(void)
{
    return widen_joined(getmonths());
}

/* __lc_time_data of the "C" locale: narrow names, formats, then the same in UTF-16, in one block. */
static void *gettnames(void)
{
    static const char *const formats[3] = {"MM/dd/yy", "dddd, MMMM dd, yyyy", "HH:mm:ss"};
    static const char *const ampm[2] = {"AM", "PM"};
    const char *strs[45];
    int k = 0;
    for (int i = 0; i < 7; i++)
        strs[k++] = day_names[2 * i];
    for (int i = 0; i < 7; i++)
        strs[k++] = day_names[2 * i + 1];
    for (int i = 0; i < 12; i++)
        strs[k++] = month_names[2 * i];
    for (int i = 0; i < 12; i++)
        strs[k++] = month_names[2 * i + 1];
    strs[k++] = ampm[0];
    strs[k++] = ampm[1];
    for (int i = 0; i < 3; i++)
        strs[k++] = formats[i];
    size_t text = 0;
    for (int i = 0; i < k; i++)
        text += (strlen(strs[i]) + 1) * 3;
    size_t head = (size_t)k * 4 + 8 + (size_t)k * 4 + 4;
    uint8_t *block = w32_zalloc(head + text + 4);
    if (!block)
        return NULL;
    char **narrow = (char **)block;
    WCHAR **wide = (WCHAR **)(block + (size_t)k * 4 + 8);
    char *pool = (char *)(block + head);
    for (int i = 0; i < k; i++) {
        size_t n = strlen(strs[i]);
        memcpy(pool, strs[i], n + 1);
        narrow[i] = pool;
        pool += n + 1;
    }
    WCHAR *wpool = (WCHAR *)ALIGN_UP((uintptr_t)pool, 2);
    for (int i = 0; i < k; i++) {
        size_t n = strlen(strs[i]);
        for (size_t c = 0; c <= n; c++)
            wpool[c] = (uint8_t)strs[i][c];
        wide[i] = wpool;
        wpool += n + 1;
    }
    ((int *)(block + (size_t)k * 4))[1] = 1;
    return block;
}

static size_t strftime_l(char *out, size_t cap, const char *fmt, const void *tm, void *data)
{
    UNUSED(data);
    return ((size_t(*)(char *, size_t, const char *, const void *))lookup("strftime"))(out, cap, fmt, tm);
}

static size_t wcsftime_l(WCHAR *out, size_t cap, const WCHAR *fmt, const void *tm, void *data)
{
    UNUSED(data);
    return ((size_t(*)(WCHAR *, size_t, const WCHAR *, const void *))lookup("wcsftime"))(out, cap, fmt, tm);
}


static int except_handler3(void *record, void *frame, void *context, void *dispatch)
{
    UNUSED(record);
    UNUSED(frame);
    UNUSED(context);
    UNUSED(dispatch);
    return 1;
}

static const struct w32_export ucrt2_exports[] = {
    W32_AS("_except_handler3", except_handler3),
    W32_AS("_malloc_base", base_alloc),
    W32_AS("_calloc_base", calloc_base),
    W32_AS("_free_base", free_base),
    W32_AS("_callnewh", callnewh),
    W32_AS("_aligned_offset_malloc", aligned_offset_malloc),
    W32_AS("___lc_locale_name_func", lc_locale_name),
    W32_AS("___lc_collate_cp_func", lc_collate_cp),
    W32_AS("_create_locale", create_locale),
    W32_AS("_free_locale", free_locale),
    W32_AS("__std_exception_copy", std_exception_copy),
    W32_AS("__std_exception_destroy", std_exception_destroy),
    W32_AS("_wcsicmp_l", wcsicmp_l),
    W32_AS("_towlower_l", towlower_l),
    W32_AS("_towupper_l", towupper_l),
    W32_AS("wmemcpy_s", wmemcpy_s),
    W32_AS("__strncnt", strncnt),
    W32_AS("_i64tow_s", i64tow_s),
    W32_AS("_splitpath_s", splitpath_s),
    W32_AS("_makepath_s", makepath_s),
    W32_AS("_get_wide_winmain_command_line", wide_winmain_command_line),
    W32_AS("_invoke_watson", invoke_watson),
    W32_AS("_set_doserrno", set_doserrno),
    W32_AS("_wsopen_s", wsopen_s),
    W32_AS("_Getdays", getdays),
    W32_AS("_Getmonths", getmonths),
    W32_AS("_Gettnames", gettnames),
    W32_AS("_W_Getdays", w_getdays),
    W32_AS("_W_Getmonths", w_getmonths),
    W32_AS("_W_Gettnames", gettnames),
    W32_AS("_Strftime", strftime_l),
    W32_AS("_Wcsftime", wcsftime_l),

    W32_AS("_lock_locales", lock_locales),
    W32_AS("_unlock_locales", lock_locales),
    W32_AS("_ltoa_s", ltoa_s),
    W32_AS("_wfullpath", wfullpath),
    W32_AS("_wsplitpath_s", wsplitpath_s),
    W32_AS("__stdio_common_vsnwprintf_s", common_vsnwprintf_s),
    W32_AS("_get_stream_buffer_pointers", stream_buffer_pointers),
    W32_AS("_wcsupr_s", wcsupr_s),
    W32_AS("_wcslwr_s", wcslwr_s),
    W32_AS("mbsrtowcs", mbsrtowcs),
    W32_AS("imaxdiv", imaxdiv),
    W32_AS("lldiv", imaxdiv),
    W32_AS("_wgetdcwd", wgetdcwd),
    W32_AS("_getdrives", getdrives),
    W32_AS("_mktemp_s", mktemp_s),
    W32_AS("_wmktemp_s", wmktemp_s),
    W32_AS("_wfsopen", wfsopen),
    W32_AS("_mbsinc", mbsinc),
    W32_AS("_mbsdec", mbsdec),
};
CRT_EXPORTS(ucrt2_module, ucrt2_exports);
