#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/lnk.h>
#include <synth/vfs.h>
#include <synth/winpath.h>
#include <synth/winshell.h>

#include "win32.h"

#define FAKE_KEY ((HANDLE)(uintptr_t)0x5e600000)
#define CSIDL_FLAG_CREATE 0x8000

static BOOL WINAPI GetUserNameA(LPSTR buf, DWORD *size)
{
    const char *user = w32_getenv("USERNAME");
    if (!user)
        user = "user";
    DWORD need = (DWORD)strlen(user) + 1;
    if (*size < need) {
        *size = need;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, user, need);
    *size = need;
    return TRUE;
}

static BOOL WINAPI GetUserNameW(LPWSTR buf, DWORD *size)
{
    char tmp[64];
    DWORD n = sizeof(tmp);
    GetUserNameA(tmp, &n);
    if (*size < n) {
        *size = n;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    w32_cp_to_wide(tmp, -1, buf, *size, 0);
    *size = n;
    return TRUE;
}

static BOOL WINAPI RtlGenRandom(BYTE *buf, ULONG len)
{
    for (ULONG i = 0; i < len; i++)
        buf[i] = (BYTE)(hal_entropy() >> (i % 4 * 8));
    return TRUE;
}

static BOOL WINAPI CryptGenRandom(HANDLE prov, DWORD len, BYTE *buf)
{
    UNUSED(prov);
    return RtlGenRandom(buf, len);
}

static BOOL WINAPI CryptAcquireContextA(HANDLE *out, LPCSTR container, LPCSTR provider, DWORD type, DWORD flags)
{
    UNUSED(container);
    UNUSED(provider);
    UNUSED(type);
    UNUSED(flags);
    *out = FAKE_KEY;
    return TRUE;
}

static const struct w32_export advapi32_exports[] = {
    W32_FN(GetUserNameA), W32_FN(GetUserNameW), W32_AS("SystemFunction036", RtlGenRandom),
    W32_FN(CryptAcquireContextA), W32_AS("CryptAcquireContextW", CryptAcquireContextA), W32_FN(CryptGenRandom),
    W32_AS("CryptReleaseContext", w32_one2),
    W32_AS("LookupPrivilegeValueA", w32_zero3), W32_AS("LookupPrivilegeValueW", w32_zero3),
    W32_AS("AdjustTokenPrivileges", w32_zero6), W32_AS("RegisterEventSourceA", w32_zero2),
    W32_AS("RegisterEventSourceW", w32_zero2), W32_AS("DeregisterEventSource", w32_one1),
};

W32_EXPORTS(advapi32_module, "advapi32.dll", advapi32_exports);

#define START_MENU "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu"
#define COMMON_START_MENU "C:\\ProgramData\\Microsoft\\Windows\\Start Menu"

/* the shell folders by CSIDL, as Windows 10 lays them out (%s: the user) */
static const char *folder_for(int csidl)
{
    switch (csidl & 0xff) {
    case 0x00:
    case 0x10: return "C:\\Users\\%s\\Desktop";
    case 0x02: return "C:\\Users\\%s" START_MENU "\\Programs";
    case 0x05: return "C:\\Users\\%s\\Documents";
    case 0x06: return "C:\\Users\\%s\\Favorites";
    case 0x07: return "C:\\Users\\%s" START_MENU "\\Programs\\Startup";
    case 0x08: return "C:\\Users\\%s\\AppData\\Roaming\\Microsoft\\Windows\\Recent";
    case 0x09: return "C:\\Users\\%s\\AppData\\Roaming\\Microsoft\\Windows\\SendTo";
    case 0x0b: return "C:\\Users\\%s" START_MENU;
    case 0x0d: return "C:\\Users\\%s\\Music";
    case 0x0e: return "C:\\Users\\%s\\Videos";
    case 0x14: return "C:\\Windows\\Fonts";
    case 0x15: return "C:\\Users\\%s\\AppData\\Roaming\\Microsoft\\Windows\\Templates";
    case 0x16: return COMMON_START_MENU;
    case 0x17: return COMMON_START_MENU "\\Programs";
    case 0x18: return COMMON_START_MENU "\\Programs\\Startup";
    case 0x19: return "C:\\Users\\Public\\Desktop";
    case 0x1a: return "C:\\Users\\%s\\AppData\\Roaming";
    case 0x1c: return "C:\\Users\\%s\\AppData\\Local";
    case 0x1f: return "C:\\Users\\Public\\Favorites";
    case 0x20: return "C:\\Users\\%s\\AppData\\Local\\Microsoft\\Windows\\INetCache";
    case 0x21: return "C:\\Users\\%s\\AppData\\Local\\Microsoft\\Windows\\INetCookies";
    case 0x22: return "C:\\Users\\%s\\AppData\\Local\\Microsoft\\Windows\\History";
    case 0x23: return "C:\\ProgramData";
    case 0x24: return "C:\\Windows";
    case 0x25:
    case 0x29: return "C:\\Windows\\System32";
    case 0x26:
    case 0x2a: return "C:\\Program Files";
    case 0x27: return "C:\\Users\\%s\\Pictures";
    case 0x28: return "C:\\Users\\%s";
    case 0x2b:
    case 0x2c: return "C:\\Program Files\\Common Files";
    case 0x2d: return "C:\\ProgramData\\Microsoft\\Windows\\Templates";
    case 0x2e: return "C:\\Users\\Public\\Documents";
    case 0x2f: return COMMON_START_MENU "\\Programs\\Administrative Tools";
    case 0x30: return "C:\\Users\\%s" START_MENU "\\Programs\\Administrative Tools";
    case 0x35: return "C:\\Users\\Public\\Music";
    case 0x36: return "C:\\Users\\Public\\Pictures";
    case 0x37: return "C:\\Users\\Public\\Videos";
    case 0x38: return "C:\\Windows\\Resources";
    case 0x3b: return "C:\\Users\\%s\\AppData\\Local\\Microsoft\\Windows\\Burn\\Burn";
    default: return NULL;
    }
}

static long folder_path(int csidl, char *out)
{
    const char *fmt = folder_for(csidl);
    if (!fmt)
        return (long)0x80070057;
    const char *user = w32_getenv("USERNAME");
    ksnprintf(out, 260, fmt, user ? user : "user");
    char path[PATH_MAX_LEN];
    if ((csidl & CSIDL_FLAG_CREATE || strstr(fmt, "%s") || strstr(fmt, "Start Menu")) &&
        w32_path(out, path, sizeof(path)) == 0)
        vfs_mkdirs(path);
    return 0;
}

static long WINAPI SHGetFolderPathA(HANDLE wnd, int csidl, HANDLE token, DWORD flags, LPSTR out)
{
    UNUSED(wnd);
    UNUSED(token);
    UNUSED(flags);
    return folder_path(csidl, out);
}

static long WINAPI SHGetFolderPathW(HANDLE wnd, int csidl, HANDLE token, DWORD flags, LPWSTR out)
{
    UNUSED(wnd);
    UNUSED(token);
    UNUSED(flags);
    char tmp[260];
    long r = folder_path(csidl, tmp);
    if (!r)
        w32_cp_to_wide(tmp, -1, out, 260, 0);
    return r;
}

static BOOL WINAPI SHGetSpecialFolderPathA(HANDLE wnd, LPSTR out, int csidl, BOOL create)
{
    UNUSED(wnd);
    return folder_path(csidl | (create ? CSIDL_FLAG_CREATE : 0), out) == 0;
}

static BOOL WINAPI SHGetSpecialFolderPathW(HANDLE wnd, LPWSTR out, int csidl, BOOL create)
{
    return SHGetFolderPathW(wnd, csidl | (create ? CSIDL_FLAG_CREATE : 0), NULL, 0, out) == 0;
}

static LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR line, int *argc)
{
    size_t len = w32_wcslen(line);
    LPWSTR *argv = w32_alloc(sizeof(LPWSTR) * (len / 2 + 2) + (len + 1) * sizeof(WCHAR));
    if (!argv)
        return NULL;
    WCHAR *buf = (WCHAR *)(argv + len / 2 + 2), *out = buf;
    int n = 0;
    const WCHAR *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[n++] = out;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '\\') {
                size_t slashes = 0;
                while (p[slashes] == '\\')
                    slashes++;
                if (p[slashes] == '"') {
                    for (size_t i = 0; i < slashes / 2; i++)
                        *out++ = '\\';
                    p += slashes;
                    if (slashes % 2) {
                        *out++ = '"';
                        p++;
                    }
                } else {
                    for (size_t i = 0; i < slashes; i++)
                        *out++ = '\\';
                    p += slashes;
                }
            } else if (*p == '"') {
                quoted = !quoted;
                p++;
            } else {
                *out++ = *p++;
            }
        }
        *out++ = 0;
    }
    argv[n] = NULL;
    *argc = n;
    return argv;
}

static uint32_t shell_thunk(void *line)
{
    return (uint32_t)shell_exec(w32->ctx->sh, line);
}

void (*winshell_open_hook)(const char *target);

static bool program_ext(const char *ext)
{
    static const char *const list[] = {".exe", ".com", ".scr", ".bat", ".cmd"};
    for (size_t i = 0; ext && i < ARRAY_SIZE(list); i++)
        if (!str_casecmp(ext, list[i]))
            return true;
    return false;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void url_to_path(const char *url, char *out, size_t cap)
{
    const char *p = url + 5;
    while (*p == '/')
        p++;
    size_t o = 0;
    for (; *p && o + 1 < cap; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = hex_digit(p[1]), lo = hex_digit(p[2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)(hi << 4 | lo);
                p += 2;
                continue;
            }
        }
        out[o++] = *p == '/' ? '\\' : *p;
    }
    out[o] = 0;
}

/* what nothing in the registry claims (folders, SynthOS's own file types, links without a registered browser)
   opens the way the desktop opens it */
static HANDLE open_natively(const char *target, const char *params)
{
    if (winshell_open_hook) {
        winshell_open_hook(target);
        return (HANDLE)(uintptr_t)42;
    }
    char line[512];
    ksnprintf(line, sizeof(line), "'%s' %s", target, params ? params : "");
    char *copy = kstrdup(line);
    if (copy) {
        w32_kernel_call(shell_thunk, copy);
        kfree(copy);
    }
    return (HANDLE)(uintptr_t)42;
}

/* ShellExecute as on Windows: programs start as processes, shortcuts open what they point to, documents and links
   open with the program the registry associates with them. */
static HANDLE shell_execute(const char *verb, const char *file, const char *params, const char *dir, HANDLE *process,
                            int depth)
{
    if (process)
        *process = NULL;
    if (!file || !*file)
        return (HANDLE)(uintptr_t)2;
    char path[PATH_MAX_LEN], win[PATH_MAX_LEN], line[1024], local[PATH_MAX_LEN];
    if (!str_ncasecmp(file, "file:", 5)) {
        url_to_path(file, local, sizeof(local));
        file = local;
    }
    bool url = winshell_is_url(file);
    if (!url) {
        char joined[PATH_MAX_LEN];
        if (dir && *dir && !strchr(file, ':') && file[0] != '\\' && file[0] != '/') {
            ksnprintf(joined, sizeof(joined), "%s\\%s", dir, file);
            if (w32_path(joined, path, sizeof(path)) == 0 && vfs_exists(path))
                file = joined;
        }
        if (w32_path(file, path, sizeof(path)) != 0 || !vfs_exists(path)) {
            ksnprintf(line, sizeof(line), "\"%s\" %s", file, params ? params : "");
            uint32_t info[4];
            if (!w32_create_process(line, dir, info))
                return (HANDLE)(uintptr_t)2;
            w32_handle_close((HANDLE)info[1]);
            if (process)
                *process = (HANDLE)info[0];
            else
                w32_handle_close((HANDLE)info[0]);
            return (HANDLE)(uintptr_t)42;
        }
        winpath_from_vfs(path, win, sizeof(win));
    }
    const char *ext = url ? NULL : path_ext(path);
    if (ext && !str_casecmp(ext, ".lnk") && depth < 4) {
        struct lnk *l = w32_alloc(sizeof(*l));
        HANDLE r = (HANDLE)(uintptr_t)2;
        if (l && lnk_read(path, l) == 0)
            r = shell_execute(verb, l->target, params && *params ? params : l->args, l->dir[0] ? l->dir : dir, process,
                              depth + 1);
        w32_free(l);
        return r;
    }
    if (!url && program_ext(ext))
        ksnprintf(line, sizeof(line), "\"%s\" %s", win, params ? params : "");
    else if (url || !vfs_is_dir(path) ? !winshell_command(verb, url ? file : path, params, line, sizeof(line)) : true)
        return open_natively(url ? file : path, params);
    klog("win32: ShellExecute %.160s", line);
    uint32_t info[4];
    if (!w32_create_process(line, dir, info))
        return (HANDLE)(uintptr_t)(w32_error() == ERROR_FILE_NOT_FOUND ? 2 : 31);
    w32_handle_close((HANDLE)info[1]);
    if (process)
        *process = (HANDLE)info[0];
    else
        w32_handle_close((HANDLE)info[0]);
    return (HANDLE)(uintptr_t)42;
}

static HANDLE WINAPI ShellExecuteA(HANDLE wnd, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    UNUSED(wnd);
    UNUSED(show);
    return shell_execute(op, file, params, dir, NULL, 0);
}

static HANDLE WINAPI ShellExecuteW(HANDLE wnd, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show)
{
    UNUSED(wnd);
    UNUSED(show);
    char *o = op ? w32_narrow(op) : NULL, *f = file ? w32_narrow(file) : NULL, *p = params ? w32_narrow(params) : NULL;
    char *d = dir ? w32_narrow(dir) : NULL;
    HANDLE r = shell_execute(o, f, p, d, NULL, 0);
    w32_free(o);
    w32_free(f);
    w32_free(p);
    w32_free(d);
    return r;
}

struct guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
};

static const struct {
    struct guid id;
    int csidl;
    const char *sub;
} known_folders[] = {
    {{0x3eb685db, 0x65f9, 0x4cf6, {0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d}}, 0x1a, NULL},
    {{0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}}, 0x1c, NULL},
    {{0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}}, 0x10, NULL},
    {{0xfdd39ad0, 0x238f, 0x46af, {0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7}}, 0x05, NULL},
    {{0x905e63b6, 0xc1bf, 0x494e, {0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a}}, 0x26, NULL},
    {{0x7c5a40ef, 0xa0fb, 0x4bfc, {0x87, 0x4a, 0xc0, 0xf2, 0xe0, 0xb9, 0xfa, 0x8e}}, 0x2a, NULL},
    {{0x1ac14e77, 0x02e7, 0x4e5d, {0xb7, 0x44, 0x2e, 0xb1, 0xae, 0x51, 0x98, 0xb7}}, 0x25, NULL},
    {{0xf38bf404, 0x1d43, 0x42f2, {0x93, 0x05, 0x67, 0xde, 0x0b, 0x28, 0xfc, 0x23}}, 0x24, NULL},
    {{0x5e6c858f, 0x0e22, 0x4760, {0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}}, 0x28, NULL},
    {{0x62ab5d82, 0xfdc1, 0x4dc3, {0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97}}, 0x23, NULL},
    {{0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}}, 0x28, "\\Downloads"},
    {{0xa520a1a4, 0x1780, 0x4ff6, {0xbd, 0x18, 0x16, 0x73, 0x43, 0xc5, 0xaf, 0x16}}, 0x28, "\\AppData\\LocalLow"},
    {{0xfd228cb7, 0xae11, 0x4ae3, {0x86, 0x4c, 0x16, 0xf3, 0x91, 0x0a, 0xb8, 0xfe}}, 0x24, "\\Fonts"},
    {{0xa77f5d77, 0x2e2b, 0x44c3, {0xa6, 0xa2, 0xab, 0xa6, 0x01, 0x05, 0x4a, 0x51}}, 0x02, NULL},
    {{0x625b53c3, 0xab48, 0x4ec1, {0xba, 0x1f, 0xa1, 0xef, 0x41, 0x46, 0xfc, 0x19}}, 0x0b, NULL},
    {{0xb97d20bb, 0xf46a, 0x4c97, {0xba, 0x10, 0x5e, 0x36, 0x08, 0x43, 0x08, 0x54}}, 0x07, NULL},
    {{0x0139d44e, 0x6afe, 0x49f2, {0x86, 0x90, 0x3d, 0xaf, 0xca, 0xe6, 0xff, 0xb8}}, 0x17, NULL},
    {{0xa4115719, 0xd62e, 0x491d, {0xaa, 0x7c, 0xe7, 0x4b, 0x8b, 0xe3, 0xb0, 0x67}}, 0x16, NULL},
    {{0x82a5ea35, 0xd9cd, 0x47c5, {0x96, 0x29, 0xe1, 0x5d, 0x2f, 0x71, 0x4e, 0x6e}}, 0x18, NULL},
    {{0xc4aa340d, 0xf20f, 0x4863, {0xaf, 0xef, 0xf8, 0x7e, 0xf2, 0xe6, 0xba, 0x25}}, 0x19, NULL},
    {{0xf7f1ed05, 0x9f6d, 0x47a2, {0xaa, 0xae, 0x29, 0xd3, 0x17, 0xc6, 0xf0, 0x66}}, 0x2b, NULL},
    {{0xde974d24, 0xd9c6, 0x4d3e, {0xbf, 0x91, 0xf4, 0x45, 0x51, 0x20, 0xb9, 0x17}}, 0x2c, NULL},
    {{0x6d809377, 0x6af0, 0x444b, {0x89, 0x57, 0xa3, 0x77, 0x3f, 0x02, 0x20, 0x0e}}, 0x26, NULL},
    {{0xd65231b0, 0xb2f1, 0x4857, {0xa4, 0xce, 0xa8, 0xe7, 0xc6, 0xea, 0x7d, 0x27}}, 0x29, NULL},
    {{0x5cd7aee2, 0x2219, 0x4a67, {0xb8, 0x5d, 0x6c, 0x9c, 0xe1, 0x56, 0x60, 0xcb}}, 0x1c, "\\Programs"},
    {{0x33e28130, 0x4e1e, 0x4676, {0x83, 0x5a, 0x98, 0x39, 0x5c, 0x3b, 0xc3, 0xbb}}, 0x27, NULL},
    {{0x4bd8d571, 0x6d19, 0x48d3, {0xbe, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0e, 0x43}}, 0x0d, NULL},
    {{0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}}, 0x0e, NULL},
    {{0xa63293e8, 0x664e, 0x48db, {0xa0, 0x79, 0xdf, 0x75, 0x9e, 0x05, 0x09, 0xf7}}, 0x15, NULL},
    {{0x352481e8, 0x33be, 0x4251, {0xba, 0x85, 0x60, 0x07, 0xca, 0xed, 0xcf, 0x9d}}, 0x20, NULL},
    {{0x2b0f765d, 0xc0e9, 0x4171, {0x90, 0x8e, 0x08, 0xa6, 0x11, 0xb8, 0x4f, 0xf6}}, 0x21, NULL},
    {{0xd9dc8a3b, 0xb784, 0x432e, {0xa7, 0x81, 0x5a, 0x11, 0x30, 0xa7, 0x59, 0x63}}, 0x22, NULL},
    {{0x8983036c, 0x27c0, 0x404b, {0x8f, 0x08, 0x10, 0x2d, 0x10, 0xdc, 0xfd, 0x74}}, 0x09, NULL},
    {{0xae50c081, 0xebd2, 0x438a, {0x86, 0x55, 0x8a, 0x09, 0x2e, 0x34, 0x98, 0x7a}}, 0x08, NULL},
    {{0x1777f761, 0x68ad, 0x4d8a, {0x87, 0xbd, 0x30, 0xb7, 0x59, 0xfa, 0x33, 0xdd}}, 0x06, NULL},
    {{0xed4824af, 0xdce4, 0x45a8, {0x81, 0xe2, 0xfc, 0x79, 0x65, 0x08, 0x36, 0x34}}, 0x2e, NULL},
};

static long WINAPI SHGetKnownFolderPath(const struct guid *id, DWORD flags, HANDLE token, WCHAR **out)
{
    UNUSED(token);
    *out = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(known_folders); i++) {
        if (memcmp(id, &known_folders[i].id, sizeof(*id)))
            continue;
        char tmp[300];
        long r = folder_path(known_folders[i].csidl | ((flags & 0x8000) ? CSIDL_FLAG_CREATE : 0), tmp);
        if (r)
            return r;
        if (known_folders[i].sub) {
            str_copy(tmp + strlen(tmp), known_folders[i].sub, sizeof(tmp) - strlen(tmp));
            char path[PATH_MAX_LEN];
            if (w32_path(tmp, path, sizeof(path)) == 0)
                vfs_mkdirs(path);
        }
        size_t n = strlen(tmp) + 1;
        *out = w32_alloc(n * sizeof(WCHAR));
        if (!*out)
            return (long)0x8007000e;
        w32_cp_to_wide(tmp, -1, *out, n, 0);
        return 0;
    }
    return (long)0x80070002;
}

/* Item ID lists stand for file system paths: one item that holds the path as UTF-16, then the empty terminator.
   They come from the task allocator, so ILFree, CoTaskMemFree and IMalloc::Free all release them. */
#define PIDL_MAGIC 0x44495053u

static void *pidl_new(const WCHAR *path)
{
    size_t n = w32_wcslen(path);
    size_t cb = 2 + 4 + (n + 1) * sizeof(WCHAR);
    uint8_t *p = w32_zalloc(cb + 2);
    if (!p)
        return NULL;
    p[0] = (uint8_t)cb;
    p[1] = (uint8_t)(cb >> 8);
    uint32_t magic = PIDL_MAGIC;
    memcpy(p + 2, &magic, 4);
    memcpy(p + 6, path, (n + 1) * sizeof(WCHAR));
    return p;
}

static const WCHAR *pidl_path(const void *pidl)
{
    const uint8_t *p = pidl;
    uint32_t magic;
    if (!p)
        return NULL;
    memcpy(&magic, p + 2, 4);
    return magic == PIDL_MAGIC ? (const WCHAR *)(p + 6) : NULL;
}

static long folder_pidl(int csidl, void **out)
{
    char tmp[300];
    if (!out)
        return (long)0x80070057;
    *out = NULL;
    long r = folder_path(csidl, tmp);
    if (r)
        return r;
    WCHAR *w = w32_widen(tmp);
    *out = w ? pidl_new(w) : NULL;
    w32_free(w);
    return *out ? 0 : (long)0x8007000e;
}

static long WINAPI SHGetSpecialFolderLocation(HANDLE wnd, int csidl, void **out)
{
    UNUSED(wnd);
    return folder_pidl(csidl, out);
}

static long WINAPI SHGetFolderLocation(HANDLE wnd, int csidl, HANDLE token, DWORD reserved, void **out)
{
    UNUSED(wnd);
    UNUSED(token);
    UNUSED(reserved);
    return folder_pidl(csidl, out);
}

static long WINAPI SHGetKnownFolderIDList(const struct guid *id, DWORD flags, HANDLE token, void **out)
{
    WCHAR *path = NULL;
    long r = SHGetKnownFolderPath(id, flags, token, &path);
    *out = r ? NULL : pidl_new(path);
    w32_free(path);
    return r ? r : *out ? 0 : (long)0x8007000e;
}

static BOOL WINAPI SHGetPathFromIDListW(const void *pidl, WCHAR *out)
{
    const WCHAR *path = pidl_path(pidl);
    if (!path || !out)
        return FALSE;
    memcpy(out, path, (MIN(w32_wcslen(path), 259u) + 1) * sizeof(WCHAR));
    out[259] = 0;
    return TRUE;
}

/* SHSTOCKICONINFO: cbSize, hIcon, iSysImageIndex, iIcon, szPath[260] (536 bytes, 544 with a 64-bit hIcon); the shield
   (77) is the system's, the rest the application icon, all said to live in shell32.dll */
static long WINAPI SHGetStockIconInfo(int id, UINT flags, uint32_t *info)
{
    if (!info || (info[0] != 536 && info[0] != 544))
        return (long)0x80070057;
    HANDLE(WINAPI * load)(HANDLE, uintptr_t) =
        (HANDLE(WINAPI *)(HANDLE, uintptr_t))w32_builtin_lookup("user32.dll", "LoadIconW");
    uint32_t icon = (flags & 0x100) && load ? (uint32_t)load(NULL, id == 77 ? 32518 : 32512) : 0, *p = info + 2;
    info[1] = icon;
    if (info[0] == 544) {
        info[1] = 0;
        info[2] = icon;
        info[3] = 0;
        p = info + 4;
    }
    p[0] = (uint32_t)id;
    p[1] = (uint32_t)id;
    w32_cp_to_wide("C:\\Windows\\System32\\shell32.dll", -1, (WCHAR *)(p + 2), 260, 0);
    return 0;
}

/* "Show in folder": opens the folder in Files; with no items the PIDL names the item itself, so its parent opens */
static long WINAPI SHOpenFolderAndSelectItems(const void *folder, UINT count, const void **items, DWORD flags)
{
    UNUSED(items);
    UNUSED(flags);
    const WCHAR *w = pidl_path(folder);
    char *name = w ? w32_narrow(w) : NULL, path[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    long r = (long)0x80070002;
    if (name && w32_path(name, path, sizeof(path)) == 0 && vfs_exists(path)) {
        path_dirname(path, dir, sizeof(dir));
        open_natively(count ? path : dir, NULL);
        r = 0;
    }
    w32_free(name);
    return r;
}

static BOOL WINAPI SHGetPathFromIDListA(const void *pidl, char *out)
{
    const WCHAR *path = pidl_path(pidl);
    if (!path || !out)
        return FALSE;
    size_t n = w32_wide_to_cp(path, -1, out, 259, 0);
    out[MIN(n, 259u)] = 0;
    return TRUE;
}

static BOOL WINAPI SHGetPathFromIDListEx(const void *pidl, WCHAR *out, DWORD cap, int flags)
{
    UNUSED(flags);
    const WCHAR *path = pidl_path(pidl);
    if (!path || !out || w32_wcslen(path) >= cap)
        return FALSE;
    memcpy(out, path, (w32_wcslen(path) + 1) * sizeof(WCHAR));
    return TRUE;
}

static void *WINAPI ILCreateFromPathW(const WCHAR *path)
{
    return path ? pidl_new(path) : NULL;
}

static void *WINAPI ILCreateFromPathA(const char *path)
{
    WCHAR *w = path ? w32_widen(path) : NULL;
    void *p = w ? pidl_new(w) : NULL;
    w32_free(w);
    return p;
}

static long WINAPI SHParseDisplayName(const WCHAR *name, void *ctx, void **out, DWORD want, DWORD *attrs)
{
    UNUSED(ctx);
    UNUSED(want);
    if (attrs)
        *attrs = 0;
    *out = name ? pidl_new(name) : NULL;
    return *out ? 0 : (long)0x80070057;
}

static UINT WINAPI ILGetSize(const void *pidl)
{
    const uint8_t *p = pidl;
    UINT n = 0;
    while (p && (p[n] | p[n + 1]))
        n += (UINT)(p[n] | p[n + 1] << 8);
    return p ? n + 2 : 0;
}

static void *WINAPI ILClone(const void *pidl)
{
    UINT n = ILGetSize(pidl);
    void *copy = n ? w32_alloc(n) : NULL;
    if (copy)
        memcpy(copy, pidl, n);
    return copy;
}

/* IMalloc over the task allocator, for programs that free shell memory through SHGetMalloc */
static long WINAPI malloc_qi(void *self, const void *iid, void **out)
{
    UNUSED(iid);
    *out = self;
    return 0;
}

static ULONG WINAPI malloc_ref(void *self)
{
    UNUSED(self);
    return 1;
}

static void *WINAPI malloc_alloc(void *self, SIZE_T n)
{
    UNUSED(self);
    return w32_alloc(n ? n : 1);
}

static void *WINAPI malloc_realloc(void *self, void *p, SIZE_T n)
{
    UNUSED(self);
    return w32_realloc(p, n);
}

static void WINAPI malloc_free(void *self, void *p)
{
    UNUSED(self);
    w32_free(p);
}

static SIZE_T WINAPI malloc_size(void *self, void *p)
{
    UNUSED(self);
    UNUSED(p);
    return (SIZE_T)-1;
}

static int WINAPI malloc_did(void *self, void *p)
{
    UNUSED(self);
    UNUSED(p);
    return -1;
}

static void WINAPI malloc_minimize(void *self)
{
    UNUSED(self);
}

static const void *const malloc_vtbl[] W32_VTABLE = {malloc_qi,   malloc_ref,  malloc_ref, malloc_alloc,   malloc_realloc,
                                          malloc_free, malloc_size, malloc_did, malloc_minimize};
static const void *const shell_malloc = malloc_vtbl;

static long WINAPI SHGetMalloc(const void **out)
{
    *out = &shell_malloc;
    return 0;
}

#define SEE_MASK_NOCLOSEPROCESS 0x40

/* SHELLEXECUTEINFO: fMask 1, lpVerb 3, lpFile 4, lpParameters 5, lpDirectory 6, hInstApp 8, hProcess 14 */
static BOOL shell_execute_ex(uint32_t *info, const char *verb, const char *file, const char *params, const char *dir)
{
    HANDLE process = NULL;
    HANDLE r = shell_execute(verb, file, params, dir, (info[1] & SEE_MASK_NOCLOSEPROCESS) ? &process : NULL, 0);
    info[8] = (uint32_t)r;
    if (info[1] & SEE_MASK_NOCLOSEPROCESS)
        info[14] = (uint32_t)process;
    if ((uint32_t)r <= 32) {
        w32_set_error((uint32_t)r == 2 ? ERROR_FILE_NOT_FOUND : 1155);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI ShellExecuteExW(uint32_t *info)
{
    char *o = info[3] ? w32_narrow((LPCWSTR)info[3]) : NULL, *f = info[4] ? w32_narrow((LPCWSTR)info[4]) : NULL;
    char *p = info[5] ? w32_narrow((LPCWSTR)info[5]) : NULL, *d = info[6] ? w32_narrow((LPCWSTR)info[6]) : NULL;
    BOOL ok = shell_execute_ex(info, o, f, p, d);
    w32_free(o);
    w32_free(f);
    w32_free(p);
    w32_free(d);
    return ok;
}

static BOOL WINAPI ShellExecuteExA(uint32_t *info)
{
    return shell_execute_ex(info, (const char *)info[3], (const char *)info[4], (const char *)info[5],
                            (const char *)info[6]);
}

static long WINAPI SHQueryUserNotificationState(int *state)
{
    *state = 5;
    return 0;
}

static long WINAPI shell_notimpl1(void **out)
{
    if (out)
        *out = NULL;
    return (long)0x80004001;
}

static long WINAPI shell_notimpl2(uint32_t a, void **out)
{
    UNUSED(a);
    return shell_notimpl1(out);
}

static long WINAPI shell_notimpl3(uint32_t a, uint32_t b, void **out)
{
    UNUSED(a);
    UNUSED(b);
    return shell_notimpl1(out);
}

/* no property store (AppUserModelIDs on shortcuts and the like): callers go on without it */
static long WINAPI SHGetPropertyStoreFromParsingName(const WCHAR *path, void *bc, int flags, const void *iid, void **out)
{
    UNUSED(path);
    UNUSED(bc);
    UNUSED(flags);
    UNUSED(iid);
    if (out)
        *out = NULL;
    return (long)0x80004001;
}

static void WINAPI ILFree(void *pidl)
{
    w32_free(pidl);
}

static void WINAPI SHChangeNotify(LONG event, UINT flags, const void *a, const void *b)
{
    UNUSED(event);
    UNUSED(flags);
    UNUSED(a);
    UNUSED(b);
}

#define FO_MOVE 1
#define FO_COPY 2
#define FO_DELETE 3
#define FO_RENAME 4
#define FOF_MULTIDESTFILES 0x1

/* one source path (no wildcards) to its target */
static int file_op_one(UINT func, const char *src, const char *dst)
{
    w32_fcache_forget(src);
    if (func == FO_DELETE)
        return vfs_remove_tree(src);
    w32_fcache_forget(dst);
    if (func == FO_COPY)
        return vfs_copy(src, dst);
    if (vfs_exists(dst) && strcmp(src, dst))
        vfs_remove_tree(dst);
    int err = vfs_rename(src, dst);
    if (err == E_XDEV && (err = vfs_copy(src, dst)) == 0)
        err = vfs_remove_tree(src);
    return err;
}

/* a source, expanded when its last part has wildcards; into_dir puts each one under dst by its own name */
static int file_op_source(UINT func, const char *src, const char *dst, bool into_dir)
{
    const char *base = path_basename(src);
    if (!has_wildcard(base)) {
        char target[PATH_MAX_LEN] = "";
        if (func != FO_DELETE && into_dir && path_join(dst, base, target, sizeof(target)))
            return E_NAMETOOLONG;
        return file_op_one(func, src, func == FO_DELETE ? NULL : into_dir ? target : dst);
    }
    char dir[PATH_MAX_LEN];
    path_dirname(src, dir, sizeof(dir));
    struct vdir d;
    int err = vfs_opendir(dir, &d);
    if (err)
        return err;
    char(*names)[NAME_MAX_LEN] = NULL;
    int count = 0, cap = 0;
    struct vfs_dirent ent;
    while (!err && vfs_readdir(&d, &ent) == 0) {
        if (!wildcard_match(base, ent.name, true))
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            void *grown = w32_alloc((size_t)cap * NAME_MAX_LEN);
            if (!grown) {
                err = E_NOMEM;
                break;
            }
            if (names)
                memcpy(grown, names, (size_t)count * NAME_MAX_LEN);
            w32_free(names);
            names = grown;
        }
        str_copy(names[count++], ent.name, NAME_MAX_LEN);
    }
    vfs_closedir(&d);
    for (int i = 0; i < count && !err; i++) {
        char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
        if (path_join(dir, names[i], from, sizeof(from)) ||
            (func != FO_DELETE && path_join(dst, names[i], to, sizeof(to))))
            err = E_NAMETOOLONG;
        else
            err = file_op_one(func, from, func == FO_DELETE ? NULL : to);
    }
    w32_free(names);
    return err;
}

static int file_operation(UINT func, const char *from, const char *to, WORD flags)
{
    int sources = 0;
    bool wild = false;
    for (const char *s = from; *s; s += strlen(s) + 1, sources++)
        wild |= has_wildcard(path_basename(s));
    if (func < FO_MOVE || func > FO_RENAME || !sources || (func != FO_DELETE && (!to || !*to)))
        return ERROR_INVALID_PARAMETER;
    const char *t = to;
    int err = 0;
    for (const char *s = from; *s && !err; s += strlen(s) + 1) {
        char src[PATH_MAX_LEN], dst[PATH_MAX_LEN] = "";
        if (w32_path(s, src, sizeof(src)) || (func != FO_DELETE && w32_path(t, dst, sizeof(dst))))
            return ERROR_INVALID_PARAMETER;
        bool into_dir = false;
        if (func == FO_COPY || func == FO_MOVE) {
            into_dir = wild || vfs_is_dir(dst) || (sources > 1 && !(flags & FOF_MULTIDESTFILES));
            if (into_dir && !vfs_is_dir(dst))
                err = vfs_mkdirs(dst);
        } else if (func == FO_DELETE && !has_wildcard(path_basename(src)) && !vfs_exists(src)) {
            continue;
        }
        if (!err)
            err = file_op_source(func, src, dst, into_dir);
        if ((flags & FOF_MULTIDESTFILES) && func != FO_DELETE && t[strlen(t) + 1])
            t += strlen(t) + 1;
    }
    if (err && w32_logging_errors())
        klog("win32: SHFileOperation %u %s: %d", func, from, err);
    return err ? (int)w32_error_from_vfs(err) : 0;
}

static char *narrow_list(const WCHAR *w)
{
    if (!w)
        return NULL;
    size_t n = 0;
    while (w[n] || w[n + 1])
        n++;
    n += 2;
    size_t len = w32_wide_to_cp(w, (int)n, NULL, 0, 1);
    char *s = w32_alloc(len + 2);
    if (s) {
        w32_wide_to_cp(w, (int)n, s, len, 1);
        s[len] = s[len + 1] = 0;
    }
    return s;
}

/* SHFILEOPSTRUCT: hwnd, wFunc, pFrom, pTo, fFlags, fAnyOperationsAborted, hNameMappings, lpszProgressTitle */
static int WINAPI SHFileOperationA(uint32_t *op)
{
    int r = file_operation(op[1], (const char *)op[2], (const char *)op[3], (WORD)op[4]);
    op[5] = r != 0;
    op[6] = 0;
    return r;
}

static int WINAPI SHFileOperationW(uint32_t *op)
{
    char *from = narrow_list((const WCHAR *)op[2]), *to = narrow_list((const WCHAR *)op[3]);
    int r = from ? file_operation(op[1], from, to, (WORD)op[4]) : ERROR_INVALID_PARAMETER;
    w32_free(from);
    w32_free(to);
    op[5] = r != 0;
    op[6] = 0;
    return r;
}

static const struct w32_export shell32_exports[] = {
    W32_FN(SHGetFolderPathA),        W32_FN(SHGetFolderPathW),   W32_FN(SHGetSpecialFolderPathA),
    W32_FN(SHGetSpecialFolderPathW), W32_FN(CommandLineToArgvW), W32_FN(ShellExecuteA),
    W32_FN(ShellExecuteW),           W32_FN(SHGetKnownFolderPath), W32_FN(ShellExecuteExW), W32_FN(ShellExecuteExA),
    W32_FN(SHQueryUserNotificationState), W32_FN(ILFree),      W32_FN(SHChangeNotify), W32_FN(SHGetPropertyStoreFromParsingName),
    W32_FN(SHGetMalloc), W32_AS("SHGetDesktopFolder", shell_notimpl1),
    W32_FN(SHGetSpecialFolderLocation), W32_FN(SHGetFolderLocation), W32_FN(SHGetKnownFolderIDList),
    W32_FN(SHGetPathFromIDListW), W32_FN(SHGetPathFromIDListA), W32_FN(SHGetPathFromIDListEx),
    W32_FN(SHParseDisplayName), W32_FN(ILCreateFromPathA), W32_FN(ILClone), W32_FN(ILGetSize),
    W32_FN(SHOpenFolderAndSelectItems), W32_AS("SHGetFileInfoW", w32_zero5),
    W32_AS("SHAppBarMessage", w32_zero2), W32_FN(SHGetStockIconInfo),
    W32_AS("SHGetPropertyStoreForWindow", shell_notimpl3), W32_AS("SetCurrentProcessExplicitAppUserModelID", w32_zero1),
    W32_AS("Shell_NotifyIconW", w32_one2), W32_AS("DragQueryFileW", w32_zero4),
    W32_AS("SHCreateAssociationRegistration", shell_notimpl2), W32_AS("SHChangeNotifyRegister", w32_zero6),
    W32_AS("SHChangeNotifyDeregister", w32_one1), W32_FN(ILCreateFromPathW),
    W32_AS("IsUserAnAdmin", w32_one0), W32_FN(SHFileOperationA), W32_FN(SHFileOperationW),
    W32_AS("SHBrowseForFolderA", w32_zero1), W32_AS("SHBrowseForFolderW", w32_zero1),
};

static const char *const shell32_ordinals[681] = {
    [2] = "SHChangeNotifyRegister", [4] = "SHChangeNotifyDeregister", [155] = "ILFree",
    [18] = "ILClone",               [152] = "ILGetSize",                [157] = "ILCreateFromPathW",
    [189] = "ILCreateFromPathA",    [190] = "ILCreateFromPathW",        [680] = "IsUserAnAdmin",
};

W32_EXPORTS(shell32_module, "shell32.dll", shell32_exports, .ordinals = shell32_ordinals,
            .ordinal_count = ARRAY_SIZE(shell32_ordinals));

static LONG WINAPI RtlVerifyVersionInfo(const uint8_t *info, DWORD mask, uint64_t cond)
{
    const uint32_t *v = (const uint32_t *)info;
    return w32_verify_version(v[1], v[2], v[3], v[4], *(const uint16_t *)(info + 276), *(const uint16_t *)(info + 278),
                              info[282], mask, cond)
               ? 0
               : (LONG)0xc0000059;
}

static LONG WINAPI RtlGetVersion(DWORD *info)
{
    info[1] = 10;
    info[2] = 0;
    info[3] = 19045;
    info[4] = 2;
    return 0;
}

static void WINAPI RtlGetNtVersionNumbers(DWORD *major, DWORD *minor, DWORD *build)
{
    if (major)
        *major = 10;
    if (minor)
        *minor = 0;
    if (build)
        *build = 0xf0000000u | 19045;
}

static const struct w32_export ntdll_exports[] = {
    W32_FN(RtlGetVersion), W32_FN(RtlVerifyVersionInfo),
    W32_FN(RtlGetNtVersionNumbers),
    W32_AS("RtlGenRandom", RtlGenRandom),
};

W32_EXPORTS(ntdll_module, "ntdll.dll", ntdll_exports);
