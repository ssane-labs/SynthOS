#include <synth/path.h>
#include <synth/string.h>
#include <synth/winpath.h>
#include <synth/winshell.h>

#include "win32.h"

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NO_ASSOCIATION 0x80070483u

static bool is_sep(WCHAR c)
{
    return c == '\\' || c == '/';
}

static WCHAR lower(WCHAR c)
{
    return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c;
}

static WCHAR *WINAPI PathFindFileNameW(const WCHAR *path)
{
    const WCHAR *name = path;
    for (const WCHAR *p = path; *p; p++)
        if ((is_sep(*p) || *p == ':') && p[1])
            name = p + 1;
    return (WCHAR *)name;
}

static WCHAR *WINAPI PathFindExtensionW(const WCHAR *path)
{
    const WCHAR *dot = NULL, *p = path;
    for (; *p; p++) {
        if (is_sep(*p) || *p == ' ')
            dot = NULL;
        else if (*p == '.')
            dot = p;
    }
    return (WCHAR *)(dot ? dot : p);
}

static void WINAPI PathRemoveExtensionW(WCHAR *path)
{
    *PathFindExtensionW(path) = 0;
}

static BOOL WINAPI PathRemoveFileSpecW(WCHAR *path)
{
    WCHAR *name = PathFindFileNameW(path);
    if (name == path)
        return FALSE;
    if (name > path + 1 && is_sep(name[-1]) && name[-2] != ':')
        name--;
    *name = 0;
    return TRUE;
}

static void WINAPI PathStripPathW(WCHAR *path)
{
    WCHAR *name = PathFindFileNameW(path);
    if (name != path)
        memmove(path, name, (w32_wcslen(name) + 1) * sizeof(WCHAR));
}

static WCHAR *WINAPI PathAddBackslashW(WCHAR *path)
{
    size_t n = w32_wcslen(path);
    if (n && !is_sep(path[n - 1]) && n < 259) {
        path[n++] = '\\';
        path[n] = 0;
    }
    return path + n;
}

static BOOL WINAPI PathAppendW(WCHAR *path, const WCHAR *more)
{
    while (is_sep(*more))
        more++;
    size_t n = w32_wcslen(path), m = w32_wcslen(more);
    if (n && !is_sep(path[n - 1]))
        path[n++] = '\\';
    if (n + m >= 260)
        return FALSE;
    memcpy(path + n, more, (m + 1) * sizeof(WCHAR));
    return TRUE;
}

static WCHAR *WINAPI PathCombineW(WCHAR *out, const WCHAR *dir, const WCHAR *file)
{
    if (!out)
        return NULL;
    WCHAR tmp[260] = {0};
    if (file && (is_sep(file[0]) || (file[0] && file[1] == ':'))) {
        size_t n = MIN(w32_wcslen(file), 259u);
        memcpy(tmp, file, n * sizeof(WCHAR));
    } else {
        if (dir) {
            size_t n = MIN(w32_wcslen(dir), 259u);
            memcpy(tmp, dir, n * sizeof(WCHAR));
        }
        if (file && !PathAppendW(tmp, file))
            return NULL;
    }
    memcpy(out, tmp, sizeof(tmp));
    return out;
}

static BOOL WINAPI PathIsRelativeW(const WCHAR *path)
{
    return !(path && (is_sep(path[0]) || (path[0] && path[1] == ':')));
}

static BOOL WINAPI PathFileExistsW(const WCHAR *path)
{
    DWORD(WINAPI * attrs)(const WCHAR *) =
        (DWORD(WINAPI *)(const WCHAR *))w32_builtin_lookup("kernel32.dll", "GetFileAttributesW");
    return path && attrs && attrs(path) != 0xffffffffu;
}

static bool match_one(const WCHAR *name, const WCHAR *spec, const WCHAR *end)
{
    while (spec < end) {
        if (*spec == '*') {
            for (const WCHAR *n = name;; n++) {
                if (match_one(n, spec + 1, end))
                    return true;
                if (!*n)
                    return false;
            }
        }
        if (!*name || (*spec != '?' && lower(*spec) != lower(*name)))
            return false;
        spec++;
        name++;
    }
    return !*name;
}

static BOOL WINAPI PathMatchSpecW(const WCHAR *file, const WCHAR *spec)
{
    if (!file || !spec)
        return FALSE;
    const WCHAR *name = PathFindFileNameW(file);
    while (*spec) {
        while (*spec == ' ' || *spec == ';')
            spec++;
        const WCHAR *end = spec;
        while (*end && *end != ';')
            end++;
        const WCHAR *trim = end;
        while (trim > spec && trim[-1] == ' ')
            trim--;
        if (trim > spec && match_one(name, spec, trim))
            return TRUE;
        spec = end;
    }
    return FALSE;
}

static int WINAPI StrCmpNIW(const WCHAR *a, const WCHAR *b, int n)
{
    for (int i = 0; i < n; i++) {
        WCHAR x = lower(a[i]), y = lower(b[i]);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            break;
    }
    return 0;
}

static int WINAPI StrCmpIW(const WCHAR *a, const WCHAR *b)
{
    return StrCmpNIW(a, b, 0x7fffffff);
}

static WCHAR *WINAPI StrStrIW(const WCHAR *hay, const WCHAR *needle)
{
    size_t n = w32_wcslen(needle);
    for (; *hay; hay++)
        if (!StrCmpNIW(hay, needle, (int)n))
            return (WCHAR *)hay;
    return n ? NULL : (WCHAR *)hay;
}

#define E_POINTER 0x80004003u

/* answers from the associations installers registered (synth/winshell.h) */
static bool assoc_text(int what, const char *assoc, const char *verb, char *out, size_t cap)
{
    char command[512], program[PATH_MAX_LEN];
    const char *args;
    switch (what) {
    case 1: return winshell_assoc_command(assoc, verb, out, cap);
    case 2:
    case 4:
        if (!winshell_assoc_command(assoc, verb, command, sizeof(command)) ||
            !winshell_split(command, program, sizeof(program), &args))
            return false;
        if (what == 2) {
            winpath_from_vfs(program, out, cap);
        } else {
            str_copy(out, path_basename(program), cap);
            char *dot = strrchr(out, '.');
            if (dot)
                *dot = 0;
        }
        return true;
    case 20: return winshell_class(assoc, out, cap);
    default: return false;
    }
}

static DWORD WINAPI AssocQueryStringW(DWORD flags, int str, const WCHAR *assoc, const WCHAR *extra, WCHAR *out,
                                      DWORD *len)
{
    UNUSED(flags);
    char *a = w32_narrow(assoc), *v = extra ? w32_narrow(extra) : NULL, text[512];
    bool ok = a && assoc_text(str, a, v, text, sizeof(text));
    w32_free(a);
    w32_free(v);
    if (!ok)
        return E_NO_ASSOCIATION;
    DWORD need = (DWORD)w32_cp_to_wide(text, -1, NULL, 0, 65001);
    if (!out) {
        *len = need;
        return 1;
    }
    if (*len < need) {
        *len = need;
        return E_POINTER;
    }
    w32_cp_to_wide(text, -1, out, *len, 65001);
    *len = need;
    return S_OK;
}

static DWORD WINAPI AssocQueryStringA(DWORD flags, int str, const char *assoc, const char *extra, char *out,
                                      DWORD *len)
{
    UNUSED(flags);
    char text[512];
    if (!assoc || !assoc_text(str, assoc, extra, text, sizeof(text)))
        return E_NO_ASSOCIATION;
    DWORD need = (DWORD)strlen(text) + 1;
    if (!out) {
        *len = need;
        return 1;
    }
    if (*len < need) {
        *len = need;
        return E_POINTER;
    }
    memcpy(out, text, need);
    *len = need;
    return S_OK;
}

static BOOL WINAPI PathIsUNCServerShareW(const WCHAR *path)
{
    if (!path || !is_sep(path[0]) || !is_sep(path[1]))
        return FALSE;
    int seps = 0;
    for (const WCHAR *p = path + 2; *p; p++)
        if (is_sep(*p))
            seps++;
    return seps == 1;
}

static BOOL WINAPI PathStripToRootW(WCHAR *path)
{
    if (!path || !*path)
        return FALSE;
    if (path[1] == ':') {
        path[2 + is_sep(path[2])] = 0;
        return TRUE;
    }
    if (is_sep(path[0]) && is_sep(path[1])) {
        int seps = 0;
        for (WCHAR *p = path + 2; *p; p++)
            if (is_sep(*p) && ++seps == 2) {
                *p = 0;
                break;
            }
        return TRUE;
    }
    if (is_sep(path[0])) {
        path[1] = 0;
        return TRUE;
    }
    return FALSE;
}

static void WINAPI PathQuoteSpacesW(WCHAR *path)
{
    size_t n = path ? w32_wcslen(path) : 0;
    bool space = false;
    for (size_t i = 0; i < n; i++)
        space |= path[i] == ' ';
    if (!space || n + 3 > 260)
        return;
    memmove(path + 1, path, n * sizeof(WCHAR));
    path[0] = path[n + 1] = '"';
    path[n + 2] = 0;
}

static void WINAPI PathUnquoteSpacesW(WCHAR *path)
{
    size_t n = path ? w32_wcslen(path) : 0;
    if (n >= 2 && path[0] == '"' && path[n - 1] == '"') {
        memmove(path, path + 1, (n - 2) * sizeof(WCHAR));
        path[n - 2] = 0;
    }
}

/* "a\.\b\..\c" -> "a\c" */
static BOOL WINAPI PathCanonicalizeW(WCHAR *out, const WCHAR *in)
{
    if (!out || !in)
        return FALSE;
    WCHAR buf[260];
    size_t o = 0, n = w32_wcslen(in);
    for (size_t i = 0; i < n && o < 259;) {
        if (in[i] == '.' && (i == 0 || is_sep(in[i - 1])) && (is_sep(in[i + 1]) || !in[i + 1])) {
            i += in[i + 1] ? 2 : 1;
            continue;
        }
        if (in[i] == '.' && in[i + 1] == '.' && (i == 0 || is_sep(in[i - 1])) && (is_sep(in[i + 2]) || !in[i + 2])) {
            if (o && is_sep(buf[o - 1]))
                o--;
            while (o && !is_sep(buf[o - 1]) && buf[o - 1] != ':')
                o--;
            i += in[i + 2] ? 3 : 2;
            continue;
        }
        buf[o++] = in[i++];
    }
    if (o == 2 && buf[1] == ':')
        buf[o++] = '\\';
    buf[o] = 0;
    memcpy(out, buf, (o + 1) * sizeof(WCHAR));
    return TRUE;
}

static long WINAPI SHStrDupW(const WCHAR *s, WCHAR **out)
{
    void *(WINAPI *alloc)(SIZE_T) = (void *(WINAPI *)(SIZE_T))w32_builtin_lookup("ole32.dll", "CoTaskMemAlloc");
    size_t n = s ? w32_wcslen(s) + 1 : 0;
    *out = s && alloc ? alloc(n * sizeof(WCHAR)) : NULL;
    if (!*out)
        return s ? (long)0x8007000e : (long)0x80070057;
    memcpy(*out, s, n * sizeof(WCHAR));
    return 0;
}

static BOOL WINAPI IsOS(DWORD os)
{
    return os == 0 || os == 1;
}

struct qitab {
    const uint8_t *iid;
    int offset;
};

static DWORD WINAPI QISearch(void *self, const struct qitab *tab, const uint8_t *iid, void **out)
{
    static const uint8_t unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    for (const struct qitab *t = tab; t->iid; t++)
        if (!memcmp(t->iid, iid, 16) || (t == tab && !memcmp(iid, unknown, 16))) {
            void *obj = (uint8_t *)self + t->offset;
            typedef uint32_t WINAPI (*add_ref_fn)(void *);
            ((add_ref_fn)(*(void ***)obj)[1])(obj);
            *out = obj;
            return S_OK;
        }
    *out = NULL;
    return E_NOINTERFACE;
}

static void *WINAPI SHCreateMemStream(const void *data, UINT size)
{
    UNUSED(data);
    UNUSED(size);
    return NULL;
}

static uint32_t WINAPI UrlCanonicalizeW(const WCHAR *url, WCHAR *out, DWORD *len, DWORD flags)
{
    UNUSED(flags);
    if (!url || !out || !len)
        return 0x80070057u;
    DWORD n = (DWORD)w32_wcslen(url);
    if (*len <= n) {
        *len = n + 1;
        return 0x80004003u;
    }
    memmove(out, url, (n + 1) * sizeof(WCHAR));
    *len = n;
    return 0;
}

static const struct w32_export shlwapi_exports[] = {
    W32_FN(UrlCanonicalizeW),
    W32_FN(PathFindFileNameW),
    W32_FN(PathFindExtensionW),
    W32_FN(PathRemoveExtensionW),
    W32_FN(PathRemoveFileSpecW),
    W32_FN(PathStripPathW),
    W32_FN(PathAddBackslashW),
    W32_FN(PathAppendW),
    W32_FN(PathCombineW),
    W32_FN(PathIsRelativeW),
    W32_FN(PathFileExistsW),
    W32_FN(PathMatchSpecW),
    W32_FN(StrCmpNIW),
    W32_FN(StrCmpIW),
    W32_FN(StrStrIW),
    W32_FN(AssocQueryStringW),
    W32_FN(AssocQueryStringA),
    W32_FN(PathIsUNCServerShareW),
    W32_FN(PathStripToRootW),
    W32_FN(PathQuoteSpacesW),
    W32_FN(PathUnquoteSpacesW),
    W32_FN(PathCanonicalizeW),
    W32_FN(SHStrDupW),
    W32_FN(IsOS),
    W32_FN(QISearch),
    W32_FN(SHCreateMemStream),
};

static const char *const shlwapi_ordinals[438] = {[12] = "SHCreateMemStream", [219] = "QISearch", [437] = "IsOS"};

W32_EXPORTS(shlwapi_module, "shlwapi.dll", shlwapi_exports, .ordinals = shlwapi_ordinals,
            .ordinal_count = ARRAY_SIZE(shlwapi_ordinals));
