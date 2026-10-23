#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win32.h"

/* INI files behind Get/WritePrivateProfileString: read and rewritten whole; sections and keys match without case. */

struct span {
    const char *s, *e;
};

static int ini_path(const char *file, char *out, size_t cap)
{
    char win[PATH_MAX_LEN];
    if (!strchr(file, '\\') && !strchr(file, '/') && !strchr(file, ':'))
        ksnprintf(win, sizeof(win), "C:\\Windows\\%s", file);
    else
        str_copy(win, file, sizeof(win));
    return w32_path(win, out, cap);
}

static char *ini_load(const char *file)
{
    char path[PATH_MAX_LEN];
    char *data = NULL;
    size_t len;
    if (!file || ini_path(file, path, sizeof(path)) || vfs_read_all(path, &data, &len))
        return NULL;
    return data;
}

static struct span trim(const char *s, const char *e)
{
    while (s < e && is_space(*s))
        s++;
    while (e > s && is_space(e[-1]))
        e--;
    return (struct span){s, e};
}

/* the next line without surrounding blanks; the raw line ends where *p now points */
static bool ini_line(const char **p, struct span *line)
{
    if (!**p)
        return false;
    const char *a = *p, *b = a;
    while (*b && *b != '\n')
        b++;
    *p = *b ? b + 1 : b;
    *line = trim(a, b);
    return true;
}

static bool span_is(struct span sp, const char *name)
{
    size_t n = strlen(name);
    return (size_t)(sp.e - sp.s) == n && !str_ncasecmp(sp.s, name, n);
}

static bool ini_section(struct span line, struct span *name)
{
    if (line.e - line.s < 2 || *line.s != '[')
        return false;
    const char *close = line.s + 1;
    while (close < line.e && *close != ']')
        close++;
    if (close == line.e)
        return false;
    *name = trim(line.s + 1, close);
    return true;
}

static bool ini_key(struct span line, struct span *key, struct span *value)
{
    if (line.s == line.e || *line.s == ';' || *line.s == '[')
        return false;
    const char *eq = line.s;
    while (eq < line.e && *eq != '=')
        eq++;
    if (eq == line.e)
        return false;
    *key = trim(line.s, eq);
    *value = trim(eq + 1, line.e);
    return true;
}

struct list {
    char *out;
    DWORD size, n;
    bool full;
};

static void list_add(struct list *l, struct span sp)
{
    size_t len = (size_t)(sp.e - sp.s);
    if (l->full)
        return;
    if (l->n + len + 2 > l->size) {
        len = l->size > l->n + 2 ? l->size - l->n - 2 : 0;
        l->full = true;
    }
    memcpy(l->out + l->n, sp.s, len);
    l->n += (DWORD)len;
    l->out[l->n++] = 0;
}

static DWORD ini_get(const char *file, const char *app, const char *key, const char *def, char *out, DWORD size,
                     bool *hit)
{
    if (hit)
        *hit = true;
    if (!out || !size)
        return 0;
    char *data = ini_load(file);
    const char *p = data ? data : "";
    struct span line, name, k, v;
    bool in = false;
    if (!app || !key) {
        if (size < 2) {
            out[0] = 0;
            return 0;
        }
        struct list l = {out, size, 0, false};
        while (ini_line(&p, &line)) {
            if (ini_section(line, &name)) {
                in = app && span_is(name, app);
                if (!app)
                    list_add(&l, name);
            } else if (in && ini_key(line, &k, &v)) {
                list_add(&l, k);
            }
        }
        out[l.n] = 0;
        kfree(data);
        return l.full ? size - 2 : l.n;
    }
    struct span found = trim(def ? def : "", def ? def + strlen(def) : "");
    bool matched = false;
    while (ini_line(&p, &line)) {
        if (ini_section(line, &name))
            in = span_is(name, app);
        else if (in && ini_key(line, &k, &v) && span_is(k, key)) {
            if (v.e - v.s >= 2 && (*v.s == '"' || *v.s == '\'') && v.e[-1] == *v.s)
                v = (struct span){v.s + 1, v.e - 1};
            found = v;
            matched = true;
            break;
        }
    }
    if (hit)
        *hit = matched;
    DWORD n = (DWORD)MIN((size_t)(found.e - found.s), size - 1);
    memmove(out, found.s, n);
    out[n] = 0;
    kfree(data);
    return n;
}

static void put(char **o, const char *s, size_t n)
{
    memcpy(*o, s, n);
    *o += n;
}

static void put_entry(char **o, const char *key, const char *value)
{
    put(o, key, strlen(key));
    put(o, "=", 1);
    put(o, value, strlen(value));
    put(o, "\r\n", 2);
}

/* value NULL removes the key, key NULL the whole section */
static BOOL ini_set(const char *file, const char *app, const char *key, const char *value)
{
    char path[PATH_MAX_LEN];
    if (!file || !app || ini_path(file, path, sizeof(path))) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    char *data = NULL;
    size_t len = 0;
    if (vfs_read_all(path, &data, &len))
        data = NULL, len = 0;
    char *buf = kmalloc(len + strlen(app) + (key ? strlen(key) : 0) + (value ? strlen(value) : 0) + 16);
    if (!buf) {
        kfree(data);
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    const char *p = data ? data : "";
    char *o = buf;
    bool in = false, seen = false, done = !key || !value;
    struct span line, name, k, v;
    for (const char *raw = p; ini_line(&p, &line); raw = p) {
        if (ini_section(line, &name)) {
            if (in && !done) {
                put_entry(&o, key, value);
                done = true;
            }
            in = span_is(name, app);
            seen |= in;
            if (in && !key)
                continue;
        } else if (in && !key) {
            continue;
        } else if (in && ini_key(line, &k, &v) && span_is(k, key)) {
            if (value && !done)
                put_entry(&o, key, value);
            done = true;
            continue;
        }
        put(&o, raw, (size_t)(p - raw));
        if (p[-1] != '\n')
            put(&o, "\r\n", 2);
    }
    if (!done) {
        if (!seen) {
            put(&o, "[", 1);
            put(&o, app, strlen(app));
            put(&o, "]\r\n", 3);
        }
        put_entry(&o, key, value);
    }
    w32_fcache_forget(path);
    int err = vfs_write_all(path, buf, (size_t)(o - buf));
    kfree(buf);
    kfree(data);
    if (err) {
        w32_set_error(w32_error_from_vfs(err));
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    return ini_get(file, app, key, def, out, size, NULL);
}

static DWORD WINAPI GetPrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size,
                                             LPCWSTR file)
{
    if (!out || !size)
        return 0;
    char *a = w32_narrow(app), *k = w32_narrow(key), *f = w32_narrow(file);
    DWORD cap = size * 3 + 2;
    char *buf = w32_alloc(cap);
    DWORD n = 0;
    bool hit;
    if (buf) {
        DWORD got = ini_get(f, a, k, "", buf, cap, &hit);
        size_t w = w32_cp_to_wide(buf, (int)got, out, size, 1);
        if (a && k && !hit) {
            /* the default goes back as it came, whatever characters it holds (NSIS tells a missing key by it) */
            static const WCHAR empty[1];
            const WCHAR *d = def ? def : empty;
            size_t len = w32_wcslen(d);
            while (len && d[len - 1] == ' ')
                len--;
            n = (DWORD)MIN(len, (size_t)size - 1);
            memmove(out, d, n * sizeof(WCHAR));
            out[n] = 0;
        } else if (a && k) {
            n = (DWORD)MIN(w, (size_t)size - 1);
            out[n] = 0;
        } else if (size < 2) {
            out[0] = 0;
        } else {
            n = (DWORD)MIN(w, (size_t)size - 2);
            out[n] = out[n + 1] = 0;
        }
        w32_free(buf);
    } else {
        out[0] = 0;
    }
    w32_free(a);
    w32_free(k);
    w32_free(f);
    return n;
}

static UINT WINAPI GetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file)
{
    char buf[32];
    if (!ini_get(file, app, key, "", buf, sizeof(buf), NULL))
        return (UINT)def;
    return (UINT)str_to_long(buf, NULL, 10);
}

static UINT WINAPI GetPrivateProfileIntW(LPCWSTR app, LPCWSTR key, INT def, LPCWSTR file)
{
    char *a = w32_narrow(app), *k = w32_narrow(key), *f = w32_narrow(file);
    UINT r = a && k ? GetPrivateProfileIntA(a, k, def, f) : (UINT)def;
    w32_free(a);
    w32_free(k);
    w32_free(f);
    return r;
}

static BOOL WINAPI WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR value, LPCSTR file)
{
    return ini_set(file, app, key, value);
}

static BOOL WINAPI WritePrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR value, LPCWSTR file)
{
    char *a = w32_narrow(app), *k = w32_narrow(key), *v = w32_narrow(value), *f = w32_narrow(file);
    BOOL ok = ini_set(f, a, k, v);
    w32_free(a);
    w32_free(k);
    w32_free(v);
    w32_free(f);
    return ok;
}

static const struct w32_export profile_exports[] = {
    W32_FN(GetPrivateProfileStringA), W32_FN(GetPrivateProfileStringW), W32_FN(GetPrivateProfileIntA),
    W32_FN(GetPrivateProfileIntW),    W32_FN(WritePrivateProfileStringA), W32_FN(WritePrivateProfileStringW),
};

W32_EXPORTS(profile_module, "kernel32.dll", profile_exports);
