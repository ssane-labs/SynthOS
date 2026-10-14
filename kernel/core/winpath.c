#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

static const struct winpath_rule rules[] = {
    {"C:\\Windows\\System32", "/system/win32"},
    {"C:\\Windows\\SysWOW64", "/system/win32"},
    {"C:\\Windows\\Temp", "/tmp"},
    {"C:\\Windows\\Fonts", "/home/fonts"},
    {"C:\\Windows", "/system"},
    {"C:\\Program Files", "/apps"},
    {"C:\\Program Files (x86)", "/apps"},
    {"C:\\ProgramData", "/system/data"},
    {"C:\\Users\\*", "/home"},
    {"C:\\Users", "/home"},
    {"C:\\Temp", "/tmp"},
    {"C:\\", "/"},
};

void winpath_rules(void (*fn)(const struct winpath_rule *rule, void *ctx), void *ctx)
{
    for (size_t i = 0; i < ARRAY_SIZE(rules); i++)
        fn(&rules[i], ctx);
}

bool vfs_find_nocase(const char *dir, const char *name, char *out, size_t cap)
{
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return false;
    struct vfs_dirent ent;
    bool found = false;
    while (vfs_readdir(&d, &ent) == 0) {
        if (str_casecmp(ent.name, name) == 0) {
            str_copy(out, ent.name, cap);
            found = true;
            break;
        }
    }
    vfs_closedir(&d);
    return found;
}

int vfs_resolve_nocase(const char *path, char *out, size_t cap)
{
    char fixed[PATH_MAX_LEN] = "/";
    const char *s = path[0] == '/' ? path + 1 : path;
    while (*s) {
        const char *slash = strchr(s, '/');
        size_t len = slash ? (size_t)(slash - s) : strlen(s);
        char comp[NAME_MAX_LEN], real[NAME_MAX_LEN], candidate[PATH_MAX_LEN];
        if (len >= sizeof(comp))
            return E_NAMETOOLONG;
        memcpy(comp, s, len);
        comp[len] = 0;
        int err = path_join(fixed, comp, candidate, sizeof(candidate));
        if (err)
            return err;
        if (!vfs_exists(candidate) && vfs_find_nocase(fixed, comp, real, sizeof(real)))
            path_join(fixed, real, candidate, sizeof(candidate));
        str_copy(fixed, candidate, sizeof(fixed));
        s = slash ? slash + 1 : s + len;
    }
    str_copy(out, fixed, cap);
    return 0;
}

static size_t match_rule(const char *path, const struct winpath_rule *rule)
{
    const char *w = rule->windows, *p = path;
    while (*w) {
        if (*w == '*') {
            while (*p && *p != '\\')
                p++;
            w++;
            continue;
        }
        if (to_lower((uint8_t)*w) != to_lower((uint8_t)*p))
            return 0;
        w++;
        p++;
    }
    if (*p && *p != '\\' && w[-1] != '\\')
        return 0;
    return (size_t)(p - path);
}

static bool is_drive(const char *p)
{
    return is_alpha((uint8_t)p[0]) && p[1] == ':';
}

int winpath_to_vfs(const char *cwd, const char *path, char *out, size_t cap)
{
    char win[PATH_MAX_LEN];
    size_t n = 0;
    for (const char *p = path; *p && n + 1 < sizeof(win); p++)
        win[n++] = *p == '/' ? '\\' : *p;
    win[n] = 0;
    while (n && win[n - 1] == ' ')
        win[--n] = 0;
    if (str_starts(win, "\\\\?\\") || str_starts(win, "\\\\.\\"))
        memmove(win, win + 4, strlen(win + 4) + 1);

    char upath[PATH_MAX_LEN * 2];
    if (is_drive(win)) {
        char drive = (char)to_upper((uint8_t)win[0]);
        char rest[PATH_MAX_LEN];
        str_copy(rest, win + 2, sizeof(rest));
        if (drive != 'C') {
            if (drive == 'Z')
                upath[0] = 0;
            else
                ksnprintf(upath, sizeof(upath), "/mnt/%c", to_lower(drive));
            for (char *r = rest; *r; r++)
                if (*r == '\\')
                    *r = '/';
            if (rest[0] != '/')
                str_append(upath, "/", sizeof(upath));
            str_append(upath, rest, sizeof(upath));
        } else {
            if (rest[0] != '\\') {
                char rel[PATH_MAX_LEN];
                str_copy(rel, rest, sizeof(rel));
                for (char *r = rel; *r; r++)
                    if (*r == '\\')
                        *r = '/';
                char abs[PATH_MAX_LEN];
                int err = path_resolve(cwd, rel, abs, sizeof(abs));
                if (err)
                    return err;
                return vfs_resolve_nocase(abs, out, cap);
            }
            str_copy(upath, "", sizeof(upath));
            char full[PATH_MAX_LEN];
            ksnprintf(full, sizeof(full), "C:%s", rest);
            for (size_t i = 0; i < ARRAY_SIZE(rules); i++) {
                size_t m = match_rule(full, &rules[i]);
                if (!m)
                    continue;
                str_copy(upath, rules[i].synth, sizeof(upath));
                const char *tail = full + m;
                if (*tail == '\\')
                    tail++;
                if (*tail) {
                    if (!str_ends(upath, "/"))
                        str_append(upath, "/", sizeof(upath));
                    str_append(upath, tail, sizeof(upath));
                }
                break;
            }
            for (char *r = upath; *r; r++)
                if (*r == '\\')
                    *r = '/';
        }
    } else {
        for (char *r = win; *r; r++)
            if (*r == '\\')
                *r = '/';
        if (win[0] == '/') {
            char full[PATH_MAX_LEN + 2];
            ksnprintf(full, sizeof(full), "C:%s", win);
            return winpath_to_vfs(cwd, full, out, cap);
        }
        str_copy(upath, win, sizeof(upath));
    }
    char abs[PATH_MAX_LEN];
    int err = path_resolve(cwd, upath[0] ? upath : "/", abs, sizeof(abs));
    if (err)
        return err;
    return vfs_resolve_nocase(abs, out, cap);
}

void winpath_from_vfs(const char *path, char *out, size_t cap)
{
    const struct winpath_rule *best = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < ARRAY_SIZE(rules); i++) {
        const char *s = rules[i].synth;
        size_t n = strlen(s);
        if (strchr(rules[i].windows, '*') || strcmp(rules[i].windows, "C:\\Temp") == 0)
            continue;
        bool hit = n == 1 || (strncmp(path, s, n) == 0 && (path[n] == 0 || path[n] == '/'));
        if (hit && n > best_len) {
            best = &rules[i];
            best_len = n;
        }
    }
    char head[PATH_MAX_LEN];
    if (strcmp(best->synth, "/home") == 0) {
        struct shell *sh = shell_current();
        const char *user = sh ? sh_get(sh, "USER") : NULL;
        ksnprintf(head, sizeof(head), "C:\\Users\\%s", user ? user : "root");
    } else {
        str_copy(head, best->windows, sizeof(head));
    }
    const char *tail = path + (best_len == 1 ? 1 : best_len);
    if (*tail == '/')
        tail++;
    size_t n = str_copy(out, head, cap);
    if (*tail && n < cap && out[n - 1] != '\\')
        str_append(out, "\\", cap);
    str_append(out, tail, cap);
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
}
