#include <synth/fmt.h>
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

static BOOL WINAPI PathRenameExtensionW(WCHAR *path, const WCHAR *ext)
{
    WCHAR *dot = PathFindExtensionW(path);
    size_t keep = (size_t)(dot - path), add = w32_wcslen(ext);
    if (keep + add >= 260)
        return FALSE;
    memcpy(dot, ext, (add + 1) * sizeof(WCHAR));
    return TRUE;
}

static BOOL WINAPI PathRenameExtensionA(char *path, const char *ext)
{
    char *dot = NULL, *p = path;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ' ')
            dot = NULL;
        else if (*p == '.')
            dot = p;
    if (!dot)
        dot = p;
    if ((size_t)(dot - path) + strlen(ext) >= 260)
        return FALSE;
    memcpy(dot, ext, strlen(ext) + 1);
    return TRUE;
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

/* Explorer's order: digit runs compare as numbers, the rest without case */
static int WINAPI StrCmpLogicalW(const WCHAR *a, const WCHAR *b)
{
    if (!a || !b)
        return a ? 1 : b ? -1 : 0;
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0' && a[1] >= '0' && a[1] <= '9')
                a++;
            while (*b == '0' && b[1] >= '0' && b[1] <= '9')
                b++;
            size_t na = 0, nb = 0;
            while (a[na] >= '0' && a[na] <= '9')
                na++;
            while (b[nb] >= '0' && b[nb] <= '9')
                nb++;
            if (na != nb)
                return na < nb ? -1 : 1;
            for (size_t i = 0; i < na; i++)
                if (a[i] != b[i])
                    return a[i] < b[i] ? -1 : 1;
            a += na;
            b += nb;
            continue;
        }
        WCHAR x = lower(*a), y = lower(*b);
        if (x != y)
            return x < y ? -1 : 1;
        a++;
        b++;
    }
    return *a ? 1 : *b ? -1 : 0;
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
            W32_CALL(add_ref_fn, (*(void ***)obj)[1], obj);
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

static BOOL WINAPI PathIsUNCW(const WCHAR *path)
{
    return path && is_sep(path[0]) && is_sep(path[1]) && path[2] != '?';
}

static BOOL WINAPI PathIsRootW(const WCHAR *path)
{
    if (!path || !*path)
        return FALSE;
    if (path[1] == ':')
        return !path[2] || (is_sep(path[2]) && !path[3]);
    if (is_sep(path[0]) && !path[1])
        return TRUE;
    if (PathIsUNCW(path)) {
        int seps = 0;
        for (const WCHAR *p = path + 2; *p; p++)
            if (is_sep(*p) && (++seps > 1 || p[1]))
                return FALSE;
        return TRUE;
    }
    return FALSE;
}

static int WINAPI PathGetDriveNumberW(const WCHAR *path)
{
    if (!path || !path[0] || path[1] != ':')
        return -1;
    WCHAR c = lower(path[0]);
    return c >= 'a' && c <= 'z' ? c - 'a' : -1;
}

static BOOL WINAPI PathIsDirectoryW(const WCHAR *path)
{
    DWORD(WINAPI * attrs)(const WCHAR *) =
        (DWORD(WINAPI *)(const WCHAR *))w32_builtin_lookup("kernel32.dll", "GetFileAttributesW");
    DWORD a = path && attrs ? attrs(path) : 0xffffffffu;
    return a != 0xffffffffu && (a & 0x10) ? 0x10 : FALSE;
}

static BOOL WINAPI PathIsFileSpecW(const WCHAR *path)
{
    for (; path && *path; path++)
        if (is_sep(*path) || *path == ':')
            return FALSE;
    return TRUE;
}

static BOOL WINAPI PathAddExtensionW(WCHAR *path, const WCHAR *ext)
{
    static const WCHAR exe[] = {'.', 'e', 'x', 'e', 0};
    if (!path || *PathFindExtensionW(path))
        return FALSE;
    if (!ext || !*ext)
        ext = exe;
    size_t n = w32_wcslen(path), m = w32_wcslen(ext);
    if (n + m >= 260)
        return FALSE;
    memcpy(path + n, ext, (m + 1) * sizeof(WCHAR));
    return TRUE;
}

static WCHAR *WINAPI PathRemoveBackslashW(WCHAR *path)
{
    if (!path)
        return NULL;
    size_t n = w32_wcslen(path);
    if (n && is_sep(path[n - 1]) && !PathIsRootW(path))
        path[--n] = 0;
    return path + (n ? n - 1 : 0);
}

static void WINAPI PathRemoveBlanksW(WCHAR *path)
{
    if (!path)
        return;
    WCHAR *start = path;
    while (*start == ' ')
        start++;
    size_t n = w32_wcslen(start);
    while (n && start[n - 1] == ' ')
        n--;
    memmove(path, start, n * sizeof(WCHAR));
    path[n] = 0;
}

static WCHAR *WINAPI PathSkipRootW(const WCHAR *path)
{
    if (!path || !*path)
        return NULL;
    if (path[1] == ':')
        return (WCHAR *)path + 2 + is_sep(path[2]);
    if (PathIsUNCW(path)) {
        int seps = 0;
        for (const WCHAR *p = path + 2; *p; p++)
            if (is_sep(*p) && ++seps == 2)
                return (WCHAR *)p + 1;
        return (WCHAR *)path + w32_wcslen(path);
    }
    return is_sep(path[0]) ? (WCHAR *)path + 1 : NULL;
}

/* "C:\very\long\folder\file.txt" -> "C:\very\...\file.txt" in max characters with the terminator */
static BOOL WINAPI PathCompactPathExW(WCHAR *out, const WCHAR *path, UINT max, DWORD flags)
{
    UNUSED(flags);
    if (!out || !path || !max)
        return FALSE;
    size_t n = w32_wcslen(path);
    if (n < max) {
        memcpy(out, path, (n + 1) * sizeof(WCHAR));
        return TRUE;
    }
    const WCHAR *name = PathFindFileNameW(path);
    size_t name_len = w32_wcslen(name);
    if (name != path)
        name--, name_len++;
    if (name_len + 4 > max) {
        size_t keep = max > 4 ? max - 4 : 0;
        const WCHAR *from = name + (name != path);
        memcpy(out, from, keep * sizeof(WCHAR));
        size_t end = keep;
        for (int i = 0; i < 3 && end < max - 1; i++)
            out[end++] = '.';
        out[end] = 0;
        return TRUE;
    }
    size_t head = max - 1 - 3 - name_len;
    memcpy(out, path, head * sizeof(WCHAR));
    out[head] = out[head + 1] = out[head + 2] = '.';
    memcpy(out + head + 3, name, (name_len + 1) * sizeof(WCHAR));
    return TRUE;
}

/* ANSI versions: the path goes through the wide function in a 260-character buffer */
static void to_ansi(const WCHAR *w, char *out, size_t cap)
{
    char *s = w32_narrow(w);
    size_t n = s ? MIN(strlen(s), cap - 1) : 0;
    if (s)
        memcpy(out, s, n);
    out[n] = 0;
    w32_free(s);
}

static bool to_wide(const char *s, WCHAR *out)
{
    WCHAR *w = s ? w32_widen(s) : NULL;
    if (!w)
        return false;
    size_t n = MIN(w32_wcslen(w), 259u);
    memcpy(out, w, n * sizeof(WCHAR));
    out[n] = 0;
    w32_free(w);
    return true;
}

static bool sep_a(char c)
{
    return c == '\\' || c == '/';
}

static char *WINAPI PathFindFileNameA(const char *path)
{
    const char *name = path;
    for (const char *p = path; p && *p; p++)
        if ((sep_a(*p) || *p == ':') && p[1])
            name = p + 1;
    return (char *)name;
}

static char *WINAPI PathFindExtensionA(const char *path)
{
    const char *dot = NULL, *p = path;
    for (; p && *p; p++) {
        if (sep_a(*p) || *p == ' ')
            dot = NULL;
        else if (*p == '.')
            dot = p;
    }
    return (char *)(dot ? dot : p);
}

static BOOL WINAPI PathIsRelativeA(const char *path)
{
    return !(path && (sep_a(path[0]) || (path[0] && path[1] == ':')));
}

static BOOL WINAPI PathIsUNCA(const char *path)
{
    return path && sep_a(path[0]) && sep_a(path[1]) && path[2] != '?';
}

static int WINAPI PathGetDriveNumberA(const char *path)
{
    if (!path || !path[0] || path[1] != ':')
        return -1;
    WCHAR c = lower((WCHAR)(uint8_t)path[0]);
    return c >= 'a' && c <= 'z' ? c - 'a' : -1;
}

static BOOL WINAPI PathFileExistsA(const char *path)
{
    WCHAR w[260];
    return to_wide(path, w) && PathFileExistsW(w);
}

static BOOL WINAPI PathIsDirectoryA(const char *path)
{
    WCHAR w[260];
    return to_wide(path, w) ? PathIsDirectoryW(w) : FALSE;
}

static BOOL WINAPI PathIsRootA(const char *path)
{
    WCHAR w[260];
    return to_wide(path, w) && PathIsRootW(w);
}

static BOOL WINAPI PathIsFileSpecA(const char *path)
{
    for (; path && *path; path++)
        if (sep_a(*path) || *path == ':')
            return FALSE;
    return TRUE;
}

#define INPLACE_A(name, call)                                                                                          \
    static BOOL WINAPI name##A(char *path)                                                                             \
    {                                                                                                                  \
        WCHAR w[260];                                                                                                  \
        if (!to_wide(path, w))                                                                                         \
            return FALSE;                                                                                              \
        BOOL ok = call;                                                                                                \
        to_ansi(w, path, 260);                                                                                         \
        return ok;                                                                                                     \
    }

INPLACE_A(PathStripToRoot, PathStripToRootW(w))
INPLACE_A(PathRemoveFileSpec, PathRemoveFileSpecW(w))
INPLACE_A(PathStripPath, (PathStripPathW(w), TRUE))
INPLACE_A(PathRemoveExtension, (PathRemoveExtensionW(w), TRUE))
INPLACE_A(PathQuoteSpaces, (PathQuoteSpacesW(w), TRUE))
INPLACE_A(PathUnquoteSpaces, (PathUnquoteSpacesW(w), TRUE))
INPLACE_A(PathRemoveBlanks, (PathRemoveBlanksW(w), TRUE))

static char *WINAPI PathAddBackslashA(char *path)
{
    if (!path)
        return NULL;
    size_t n = strlen(path);
    if (n && !sep_a(path[n - 1]) && n < 259) {
        path[n++] = '\\';
        path[n] = 0;
    }
    return path + n;
}

static char *WINAPI PathRemoveBackslashA(char *path)
{
    if (!path)
        return NULL;
    size_t n = strlen(path);
    if (n && sep_a(path[n - 1]) && !PathIsRootA(path))
        path[--n] = 0;
    return path + (n ? n - 1 : 0);
}

static BOOL WINAPI PathAppendA(char *path, const char *more)
{
    WCHAR w[260], m[260];
    if (!to_wide(path, w) || !to_wide(more, m) || !PathAppendW(w, m))
        return FALSE;
    to_ansi(w, path, 260);
    return TRUE;
}

static BOOL WINAPI PathAddExtensionA(char *path, const char *ext)
{
    WCHAR w[260], e[260];
    if (!to_wide(path, w) || (ext && !to_wide(ext, e)) || !PathAddExtensionW(w, ext ? e : NULL))
        return FALSE;
    to_ansi(w, path, 260);
    return TRUE;
}

static char *WINAPI PathCombineA(char *out, const char *dir, const char *file)
{
    WCHAR d[260], f[260], o[260];
    if (!out || (dir && !to_wide(dir, d)) || (file && !to_wide(file, f)) ||
        !PathCombineW(o, dir ? d : NULL, file ? f : NULL))
        return NULL;
    to_ansi(o, out, 260);
    return out;
}

static BOOL WINAPI PathCanonicalizeA(char *out, const char *path)
{
    WCHAR p[260], o[260];
    if (!out || !to_wide(path, p) || !PathCanonicalizeW(o, p))
        return FALSE;
    to_ansi(o, out, 260);
    return TRUE;
}

static BOOL WINAPI PathCompactPathExA(char *out, const char *path, UINT max, DWORD flags)
{
    WCHAR p[260], o[260];
    if (!out || !max || !to_wide(path, p) || !PathCompactPathExW(o, p, MIN(max, 260u), flags))
        return FALSE;
    to_ansi(o, out, max);
    return TRUE;
}

/* HLS in 0..240 like the Windows colour dialog */
static WORD hls_value(int m1, int m2, int hue)
{
    hue = (hue + 240) % 240;
    if (hue < 40)
        m1 += ((m2 - m1) * hue + 20) / 40;
    else if (hue < 120)
        m1 = m2;
    else if (hue < 160)
        m1 += ((m2 - m1) * (160 - hue) + 20) / 40;
    return (WORD)((m1 * 255 + 120) / 240);
}

static DWORD WINAPI ColorHLSToRGB(WORD hue, WORD lum, WORD sat)
{
    if (!sat) {
        DWORD g = (DWORD)(lum * 255 + 120) / 240;
        return g | g << 8 | g << 16;
    }
    int m2 = lum <= 120 ? (lum * (240 + sat) + 120) / 240 : lum + sat - (lum * sat + 120) / 240;
    int m1 = 2 * lum - m2;
    return hls_value(m1, m2, hue + 80) | (DWORD)hls_value(m1, m2, hue) << 8 | (DWORD)hls_value(m1, m2, hue - 80) << 16;
}

static void WINAPI ColorRGBToHLS(DWORD rgb, WORD *hue, WORD *lum, WORD *sat)
{
    int r = rgb & 0xff, g = rgb >> 8 & 0xff, b = rgb >> 16 & 0xff;
    int hi = MAX(r, MAX(g, b)), lo = MIN(r, MIN(g, b)), l = ((hi + lo) * 240 + 255) / 510, h = 160, s = 0;
    if (hi != lo) {
        int d = hi - lo;
        s = l <= 120 ? (d * 240 + (hi + lo) / 2) / (hi + lo) : (d * 240 + (510 - hi - lo) / 2) / (510 - hi - lo);
        int rn = ((hi - r) * 40 + d / 2) / d, gn = ((hi - g) * 40 + d / 2) / d, bn = ((hi - b) * 40 + d / 2) / d;
        h = r == hi ? bn - gn : g == hi ? 80 + rn - bn : 160 + gn - rn;
        h = (h + 240) % 240;
    }
    if (hue)
        *hue = (WORD)h;
    if (lum)
        *lum = (WORD)l;
    if (sat)
        *sat = (WORD)s;
}

static DWORD WINAPI ColorAdjustLuma(DWORD rgb, int change, BOOL scale)
{
    WORD h, l, s;
    ColorRGBToHLS(rgb, &h, &l, &s);
    int nl = scale ? l + (l * change + 500) / 1000 : l + change;
    nl = nl < 0 ? 0 : nl > 240 ? 240 : nl;
    return ColorHLSToRGB(h, (WORD)nl, s);
}

typedef LONG(WINAPI *get_value_fn)(HANDLE, const void *, const void *, DWORD *, void *, DWORD *);

/* The user's value, then the machine's, then the caller's default */
static LONG us_value(const char *fn, const void *sub, const void *value, DWORD *type, void *data, DWORD *size,
                     BOOL ignore_user, const void *def, DWORD def_size)
{
    get_value_fn get = (get_value_fn)w32_builtin_lookup("shlwapi.dll", fn);
    DWORD cap = size ? *size : 0;
    for (int i = ignore_user ? 1 : 0; get && i < 2; i++) {
        if (size)
            *size = cap;
        if (!get((HANDLE)(uintptr_t)(i ? 0x80000002u : 0x80000001u), sub, value, type, data, size))
            return 0;
    }
    if (!def || !def_size)
        return 2;
    if (data && size && cap >= def_size)
        memcpy(data, def, def_size);
    if (size)
        *size = def_size;
    return data && cap < def_size ? 234 : 0;
}

static LONG WINAPI SHRegGetUSValueW(const WCHAR *sub, const WCHAR *value, DWORD *type, void *data, DWORD *size,
                                    BOOL ignore_user, const void *def, DWORD def_size)
{
    return us_value("SHGetValueW", sub, value, type, data, size, ignore_user, def, def_size);
}

static LONG WINAPI SHRegGetUSValueA(const char *sub, const char *value, DWORD *type, void *data, DWORD *size,
                                    BOOL ignore_user, const void *def, DWORD def_size)
{
    return us_value("SHGetValueA", sub, value, type, data, size, ignore_user, def, def_size);
}

static char lower_a(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static char *WINAPI StrStrIA(const char *s, const char *find)
{
    if (!s || !find)
        return NULL;
    size_t n = strlen(find);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && lower_a(s[i]) == lower_a(find[i]))
            i++;
        if (i == n)
            return (char *)s;
    }
    return NULL;
}

static char *WINAPI StrStrA(const char *s, const char *find)
{
    return s && find ? strstr(s, find) : NULL;
}

static WCHAR *WINAPI StrStrW(const WCHAR *s, const WCHAR *find)
{
    if (!s || !find)
        return NULL;
    size_t n = w32_wcslen(find);
    for (; *s; s++)
        if (!memcmp(s, find, n * sizeof(WCHAR)))
            return (WCHAR *)s;
    return NULL;
}

static WCHAR *WINAPI StrChrW(const WCHAR *s, WCHAR c)
{
    for (; s && *s; s++)
        if (*s == c)
            return (WCHAR *)s;
    return NULL;
}

static WCHAR *WINAPI StrChrIW(const WCHAR *s, WCHAR c)
{
    for (; s && *s; s++)
        if (lower(*s) == lower(c))
            return (WCHAR *)s;
    return NULL;
}

static char *WINAPI StrChrA(const char *s, WORD c)
{
    for (; s && *s; s++)
        if ((uint8_t)*s == c)
            return (char *)s;
    return NULL;
}

static WCHAR *WINAPI StrRChrW(const WCHAR *s, const WCHAR *end, WCHAR c)
{
    const WCHAR *found = NULL;
    for (; s && *s && (!end || s < end); s++)
        if (*s == c)
            found = s;
    return (WCHAR *)found;
}

static char *WINAPI StrRChrA(const char *s, const char *end, WORD c)
{
    const char *found = NULL;
    for (; s && *s && (!end || s < end); s++)
        if ((uint8_t)*s == c)
            found = s;
    return (char *)found;
}

static int WINAPI StrCmpW(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (int)*a - (int)*b;
}

static int WINAPI StrCmpNW(const WCHAR *a, const WCHAR *b, int n)
{
    for (; n > 0; n--, a++, b++)
        if (*a != *b || !*a)
            return (int)*a - (int)*b;
    return 0;
}

static int WINAPI StrCmpIA(const char *a, const char *b)
{
    while (*a && lower_a(*a) == lower_a(*b))
        a++, b++;
    return (int)(uint8_t)lower_a(*a) - (int)(uint8_t)lower_a(*b);
}

static int WINAPI StrCmpNIA(const char *a, const char *b, int n)
{
    for (; n > 0; n--, a++, b++)
        if (lower_a(*a) != lower_a(*b) || !*a)
            return (int)(uint8_t)lower_a(*a) - (int)(uint8_t)lower_a(*b);
    return 0;
}

static int WINAPI StrToIntW(const WCHAR *s)
{
    int sign = 1, v = 0;
    if (*s == '-')
        sign = -1, s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

static int WINAPI StrToIntA(const char *s)
{
    int sign = 1, v = 0;
    if (*s == '-')
        sign = -1, s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return sign * v;
}

static WCHAR *WINAPI StrCpyW(WCHAR *d, const WCHAR *s)
{
    memcpy(d, s, (w32_wcslen(s) + 1) * sizeof(WCHAR));
    return d;
}

static WCHAR *WINAPI StrCpyNW(WCHAR *d, const WCHAR *s, int n)
{
    if (n <= 0)
        return d;
    size_t m = MIN(w32_wcslen(s), (size_t)n - 1);
    memcpy(d, s, m * sizeof(WCHAR));
    d[m] = 0;
    return d;
}

static WCHAR *WINAPI StrCatW(WCHAR *d, const WCHAR *s)
{
    StrCpyW(d + w32_wcslen(d), s);
    return d;
}

static WCHAR *WINAPI StrCatBuffW(WCHAR *d, const WCHAR *s, int cap)
{
    size_t n = w32_wcslen(d);
    if ((int)n < cap)
        StrCpyNW(d + n, s, cap - (int)n);
    return d;
}

typedef void *(WINAPI *local_alloc_fn)(UINT, size_t);

static void *local_alloc(size_t n)
{
    local_alloc_fn alloc = (local_alloc_fn)w32_builtin_lookup("kernel32.dll", "LocalAlloc");
    return alloc ? alloc(0, n) : NULL;
}

static WCHAR *WINAPI StrDupW(const WCHAR *s)
{
    size_t n = s ? w32_wcslen(s) + 1 : 1;
    WCHAR *d = local_alloc(n * sizeof(WCHAR));
    if (d) {
        d[0] = 0;
        if (s)
            memcpy(d, s, n * sizeof(WCHAR));
    }
    return d;
}

static char *WINAPI StrDupA(const char *s)
{
    size_t n = s ? strlen(s) + 1 : 1;
    char *d = local_alloc(n);
    if (d) {
        d[0] = 0;
        if (s)
            memcpy(d, s, n);
    }
    return d;
}

static BOOL WINAPI StrTrimW(WCHAR *s, const WCHAR *chars)
{
    size_t len = w32_wcslen(s), n = len, start = 0;
    while (start < n && StrChrW(chars, s[start]))
        start++;
    while (n > start && StrChrW(chars, s[n - 1]))
        n--;
    if (!start && n == len)
        return FALSE;
    memmove(s, s + start, (n - start) * sizeof(WCHAR));
    s[n - start] = 0;
    return TRUE;
}

/* "1.50 KB"-style sizes with three significant digits */
static void byte_size(uint64_t bytes, char *out, size_t cap)
{
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    if (bytes < 1024) {
        ksnprintf(out, cap, "%u bytes", (unsigned)bytes);
        return;
    }
    int u = 0;
    uint64_t scaled = bytes * 100;
    while (scaled >= 1024 * 100 && u < 5)
        scaled /= 1024, u++;
    unsigned whole = (unsigned)(scaled / 100), frac = (unsigned)(scaled % 100);
    if (whole >= 100)
        ksnprintf(out, cap, "%u %s", whole, units[u]);
    else if (whole >= 10)
        ksnprintf(out, cap, "%u.%u %s", whole, frac / 10, units[u]);
    else
        ksnprintf(out, cap, "%u.%02u %s", whole, frac, units[u]);
}

static char *WINAPI StrFormatByteSize64A(uint64_t bytes, char *out, UINT cap)
{
    if (out && cap)
        byte_size(bytes, out, cap);
    return out;
}

static char *WINAPI StrFormatByteSizeA(DWORD bytes, char *out, UINT cap)
{
    return StrFormatByteSize64A(bytes, out, cap);
}

static WCHAR *WINAPI StrFormatByteSizeW(uint64_t bytes, WCHAR *out, UINT cap)
{
    char tmp[32];
    byte_size(bytes, tmp, sizeof(tmp));
    size_t n = MIN(strlen(tmp), cap ? cap - 1 : 0);
    for (size_t i = 0; i < n; i++)
        out[i] = (WCHAR)tmp[i];
    if (cap)
        out[n] = 0;
    return out;
}

static const struct w32_export shlwapi_exports[] = {
    W32_FN(UrlCanonicalizeW),
    W32_FN(PathFindFileNameW),
    W32_FN(PathFindExtensionW),
    W32_FN(PathRemoveExtensionW), W32_FN(PathRenameExtensionW), W32_FN(PathRenameExtensionA),
    W32_FN(PathRemoveFileSpecW),
    W32_FN(PathStripPathW),
    W32_FN(PathAddBackslashW),
    W32_FN(PathAppendW),
    W32_FN(PathCombineW),
    W32_FN(PathIsRelativeW),
    W32_FN(PathFileExistsW),
    W32_FN(PathMatchSpecW),
    W32_FN(StrCmpNIW),
    W32_FN(StrCmpIW), W32_FN(StrCmpLogicalW),
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
    W32_FN(PathIsUNCW),
    W32_FN(PathIsRootW),
    W32_FN(PathGetDriveNumberW),
    W32_FN(PathIsDirectoryW),
    W32_FN(PathIsFileSpecW),
    W32_FN(PathAddExtensionW),
    W32_FN(PathRemoveBackslashW),
    W32_FN(PathRemoveBlanksW),
    W32_FN(PathSkipRootW),
    W32_FN(PathCompactPathExW),
    W32_FN(PathFindFileNameA),
    W32_FN(PathFindExtensionA),
    W32_FN(PathIsRelativeA),
    W32_FN(PathIsUNCA),
    W32_FN(PathGetDriveNumberA),
    W32_FN(PathFileExistsA),
    W32_FN(PathIsDirectoryA),
    W32_FN(PathIsRootA),
    W32_FN(PathIsFileSpecA),
    W32_FN(PathAddBackslashA),
    W32_FN(PathRemoveBackslashA),
    W32_FN(PathAppendA),
    W32_FN(PathAddExtensionA),
    W32_FN(PathCombineA),
    W32_FN(PathCanonicalizeA),
    W32_FN(PathCompactPathExA),
    W32_FN(ColorHLSToRGB),
    W32_FN(ColorRGBToHLS),
    W32_FN(ColorAdjustLuma),
    W32_FN(SHRegGetUSValueW),
    W32_FN(SHRegGetUSValueA),
    W32_FN(StrStrIA),
    W32_FN(StrStrA),
    W32_FN(StrStrW),
    W32_FN(StrChrW),
    W32_FN(StrChrIW),
    W32_FN(StrChrA),
    W32_FN(StrRChrW),
    W32_FN(StrRChrA),
    W32_FN(StrCmpW),
    W32_FN(StrCmpNW),
    W32_FN(StrCmpIA),
    W32_FN(StrCmpNIA),
    W32_FN(StrToIntW),
    W32_FN(StrToIntA),
    W32_FN(StrCpyW),
    W32_FN(StrCpyNW),
    W32_FN(StrCatW),
    W32_FN(StrCatBuffW),
    W32_FN(StrDupW),
    W32_FN(StrDupA),
    W32_FN(StrTrimW),
    W32_FN(StrFormatByteSize64A),
    W32_FN(StrFormatByteSizeA),
    W32_FN(StrFormatByteSizeW),
    W32_FN(PathStripToRootA),
    W32_FN(PathRemoveFileSpecA),
    W32_FN(PathStripPathA),
    W32_FN(PathRemoveExtensionA),
    W32_FN(PathQuoteSpacesA),
    W32_FN(PathUnquoteSpacesA),
    W32_FN(PathRemoveBlanksA),
};

static const char *const shlwapi_ordinals[438] = {[12] = "SHCreateMemStream", [219] = "QISearch", [437] = "IsOS"};

W32_EXPORTS(shlwapi_module, "shlwapi.dll", shlwapi_exports, .ordinals = shlwapi_ordinals,
            .ordinal_count = ARRAY_SIZE(shlwapi_ordinals));
