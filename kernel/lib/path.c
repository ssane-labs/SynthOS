#include <synth/error.h>
#include <synth/path.h>
#include <synth/string.h>

int path_resolve(const char *cwd, const char *path, char *out, size_t cap)
{
    char tmp[PATH_MAX_LEN * 2];
    if (path[0] == '/') {
        if (str_copy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
            return E_NAMETOOLONG;
    } else {
        str_copy(tmp, cwd && *cwd ? cwd : "/", sizeof(tmp));
        str_append(tmp, "/", sizeof(tmp));
        if (str_append(tmp, path, sizeof(tmp)) >= sizeof(tmp))
            return E_NAMETOOLONG;
    }

    size_t marks[PATH_MAX_LEN / 2];
    size_t depth = 0, len = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t sl = (size_t)(p - seg);
        if (sl == 0 || (sl == 1 && seg[0] == '.'))
            continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth)
                len = marks[--depth];
            continue;
        }
        if (sl >= NAME_MAX_LEN || depth >= ARRAY_SIZE(marks) || len + sl + 2 > cap)
            return E_NAMETOOLONG;
        marks[depth++] = len;
        out[len++] = '/';
        memcpy(out + len, seg, sl);
        len += sl;
    }
    if (len == 0) {
        if (cap < 2)
            return E_NAMETOOLONG;
        out[len++] = '/';
    }
    out[len] = 0;
    return 0;
}

int path_join(const char *dir, const char *name, char *out, size_t cap)
{
    return path_resolve(dir, name, out, cap);
}

const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return path;
    if (slash[1] == 0 && slash == path)
        return path;
    return slash + 1;
}

void path_dirname(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        str_copy(out, ".", cap);
        return;
    }
    if (slash == path) {
        str_copy(out, "/", cap);
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

const char *path_ext(const char *path)
{
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot && dot != base ? dot : "";
}

bool has_wildcard(const char *s)
{
    for (; *s; s++)
        if (*s == '*' || *s == '?' || *s == '[')
            return true;
    return false;
}

static bool class_match(const char **pp, int c, bool icase)
{
    const char *p = *pp + 1;
    bool negate = false, matched = false;
    if (*p == '!' || *p == '^')
        negate = true, p++;
    if (icase)
        c = to_lower(c);
    for (bool first = true; *p && (first || *p != ']'); first = false) {
        int lo = (uint8_t)*p++, hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            hi = (uint8_t)p[1];
            p += 2;
        }
        if (icase)
            lo = to_lower(lo), hi = to_lower(hi);
        if (c >= lo && c <= hi)
            matched = true;
    }
    *pp = *p == ']' ? p + 1 : p;
    return matched != negate;
}

bool wildcard_match(const char *pattern, const char *s, bool icase)
{
    const char *star = NULL, *resume = NULL;
    while (*s) {
        if (*pattern == '*') {
            star = pattern++;
            resume = s;
            continue;
        }
        if (*pattern == '[') {
            const char *p = pattern;
            if (class_match(&p, (uint8_t)*s, icase)) {
                pattern = p;
                s++;
                continue;
            }
        } else if (*pattern == '?' ||
                   (icase ? to_lower((uint8_t)*pattern) == to_lower((uint8_t)*s)
                          : *pattern == *s)) {
            if (*pattern) {
                pattern++;
                s++;
                continue;
            }
        }
        if (!star)
            return false;
        pattern = star + 1;
        s = ++resume;
    }
    while (*pattern == '*')
        pattern++;
    return *pattern == 0;
}
