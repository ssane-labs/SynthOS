#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "win32.h"

#define FAKE_KEY ((HANDLE)(uintptr_t)0x5e600000)
#define CSIDL_FLAG_CREATE 0x8000

static LONG WINAPI RegOpenKeyExA(HANDLE key, LPCSTR sub, DWORD opts, DWORD sam, HANDLE *out)
{
    UNUSED(key);
    UNUSED(sub);
    UNUSED(opts);
    UNUSED(sam);
    if (out)
        *out = NULL;
    return ERROR_FILE_NOT_FOUND;
}

static LONG WINAPI RegOpenKeyA(HANDLE key, LPCSTR sub, HANDLE *out)
{
    return RegOpenKeyExA(key, sub, 0, 0, out);
}

static LONG WINAPI RegCreateKeyExA(HANDLE key, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD opts, DWORD sam,
                                   void *sa, HANDLE *out, DWORD *disposition)
{
    UNUSED(key);
    UNUSED(sub);
    UNUSED(reserved);
    UNUSED(cls);
    UNUSED(opts);
    UNUSED(sam);
    UNUSED(sa);
    *out = FAKE_KEY;
    if (disposition)
        *disposition = 1;
    return ERROR_SUCCESS;
}

static LONG WINAPI RegCreateKeyA(HANDLE key, LPCSTR sub, HANDLE *out)
{
    return RegCreateKeyExA(key, sub, 0, NULL, 0, 0, NULL, out, NULL);
}

static LONG WINAPI RegQueryValueExA(HANDLE key, LPCSTR name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *size)
{
    UNUSED(key);
    UNUSED(name);
    UNUSED(reserved);
    UNUSED(type);
    UNUSED(data);
    UNUSED(size);
    return ERROR_FILE_NOT_FOUND;
}

static LONG WINAPI RegGetValueA(HANDLE key, LPCSTR sub, LPCSTR value, DWORD flags, DWORD *type, void *data,
                                DWORD *size)
{
    UNUSED(key);
    UNUSED(sub);
    UNUSED(value);
    UNUSED(flags);
    UNUSED(type);
    UNUSED(data);
    UNUSED(size);
    return ERROR_FILE_NOT_FOUND;
}

static LONG WINAPI RegEnumKeyExA(HANDLE key, DWORD index, LPSTR name, DWORD *len, DWORD *reserved, LPSTR cls,
                                 DWORD *cls_len, void *time)
{
    UNUSED(key);
    UNUSED(index);
    UNUSED(name);
    UNUSED(len);
    UNUSED(reserved);
    UNUSED(cls);
    UNUSED(cls_len);
    UNUSED(time);
    return ERROR_NO_MORE_ITEMS;
}

static LONG WINAPI RegQueryInfoKeyA(HANDLE key, LPSTR cls, DWORD *cls_len, DWORD *reserved, DWORD *subkeys,
                                    DWORD *max_sub, DWORD *max_cls, DWORD *values, DWORD *max_name, DWORD *max_value,
                                    DWORD *sec, void *time)
{
    UNUSED(key);
    UNUSED(cls);
    UNUSED(cls_len);
    UNUSED(reserved);
    UNUSED(max_sub);
    UNUSED(max_cls);
    UNUSED(max_name);
    UNUSED(max_value);
    UNUSED(sec);
    UNUSED(time);
    if (subkeys)
        *subkeys = 0;
    if (values)
        *values = 0;
    return ERROR_SUCCESS;
}

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
    W32_FN(RegOpenKeyExA), W32_AS("RegOpenKeyExW", RegOpenKeyExA), W32_FN(RegOpenKeyA),
    W32_AS("RegOpenKeyW", RegOpenKeyA), W32_FN(RegCreateKeyExA), W32_AS("RegCreateKeyExW", RegCreateKeyExA),
    W32_FN(RegCreateKeyA), W32_AS("RegCreateKeyW", RegCreateKeyA), W32_FN(RegQueryValueExA),
    W32_AS("RegQueryValueExW", RegQueryValueExA), W32_FN(RegGetValueA), W32_AS("RegGetValueW", RegGetValueA),
    W32_AS("RegSetValueExA", w32_zero6), W32_AS("RegSetValueExW", w32_zero6), W32_AS("RegCloseKey", w32_zero1),
    W32_AS("RegFlushKey", w32_zero1), W32_AS("RegDeleteKeyA", w32_zero2), W32_AS("RegDeleteKeyW", w32_zero2),
    W32_AS("RegDeleteValueA", w32_zero2), W32_AS("RegDeleteValueW", w32_zero2), W32_FN(RegEnumKeyExA),
    W32_AS("RegEnumKeyExW", RegEnumKeyExA), W32_AS("RegEnumValueA", RegEnumKeyExA),
    W32_AS("RegEnumValueW", RegEnumKeyExA), W32_FN(RegQueryInfoKeyA), W32_AS("RegQueryInfoKeyW", RegQueryInfoKeyA),
    W32_FN(GetUserNameA), W32_FN(GetUserNameW), W32_AS("SystemFunction036", RtlGenRandom),
    W32_FN(CryptAcquireContextA), W32_AS("CryptAcquireContextW", CryptAcquireContextA), W32_FN(CryptGenRandom),
    W32_AS("CryptReleaseContext", w32_one2),

    W32_AS("LookupPrivilegeValueA", w32_zero3), W32_AS("LookupPrivilegeValueW", w32_zero3),
    W32_AS("AdjustTokenPrivileges", w32_zero6), W32_AS("RegisterEventSourceA", w32_zero2),
    W32_AS("RegisterEventSourceW", w32_zero2), W32_AS("DeregisterEventSource", w32_one1),
};

W32_EXPORTS(advapi32_module, "advapi32.dll", advapi32_exports);

static const char *folder_for(int csidl)
{
    switch (csidl & 0xff) {
    case 0x00:
    case 0x10:
        return "C:\\Users\\%s\\Desktop";
    case 0x05:
        return "C:\\Users\\%s\\Documents";
    case 0x1a:
        return "C:\\Users\\%s\\AppData\\Roaming";
    case 0x1c:
        return "C:\\Users\\%s\\AppData\\Local";
    case 0x23:
        return "C:\\ProgramData";
    case 0x24:
        return "C:\\Windows";
    case 0x25:
    case 0x29:
        return "C:\\Windows\\System32";
    case 0x26:
    case 0x2a:
        return "C:\\Program Files";
    case 0x28:
        return "C:\\Users\\%s";
    default:
        return NULL;
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
    if ((csidl & CSIDL_FLAG_CREATE || strstr(fmt, "%s")) && w32_path(out, path, sizeof(path)) == 0)
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

static HANDLE shell_execute(const char *op, const char *file, const char *params)
{
    if (op && *op && str_casecmp(op, "open") != 0)
        return (HANDLE)(uintptr_t)31;
    char path[PATH_MAX_LEN], line[512];
    if (w32_path(file, path, sizeof(path)) != 0)
        str_copy(path, file, sizeof(path));
    ksnprintf(line, sizeof(line), "'%s' %s", path, params ? params : "");
    char *copy = kstrdup(line);
    if (copy) {
        w32_kernel_call(shell_thunk, copy);
        kfree(copy);
    }
    return (HANDLE)(uintptr_t)42;
}

static HANDLE WINAPI ShellExecuteA(HANDLE wnd, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, int show)
{
    UNUSED(wnd);
    UNUSED(dir);
    UNUSED(show);
    return shell_execute(op, file, params);
}

static HANDLE WINAPI ShellExecuteW(HANDLE wnd, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show)
{
    UNUSED(wnd);
    UNUSED(dir);
    UNUSED(show);
    char *o = op ? w32_narrow(op) : NULL, *f = w32_narrow(file), *p = params ? w32_narrow(params) : NULL;
    HANDLE r = shell_execute(o, f, p);
    w32_free(o);
    w32_free(f);
    w32_free(p);
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

static BOOL WINAPI ShellExecuteExW(uint32_t *info)
{
    char *o = info[3] ? w32_narrow((LPCWSTR)info[3]) : NULL, *f = info[4] ? w32_narrow((LPCWSTR)info[4]) : NULL;
    char *p = info[5] ? w32_narrow((LPCWSTR)info[5]) : NULL;
    HANDLE r = f ? shell_execute(o, f, p) : NULL;
    w32_free(o);
    w32_free(f);
    w32_free(p);
    info[8] = (uint32_t)r;
    if ((uint32_t)r <= 32) {
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
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

static long WINAPI shell_notimpl3(uint32_t a, uint32_t b, void **out)
{
    UNUSED(a);
    UNUSED(b);
    return shell_notimpl1(out);
}

static long WINAPI shell_notimpl4(uint32_t a, uint32_t b, uint32_t c, void **out)
{
    UNUSED(c);
    return shell_notimpl3(a, b, out);
}

static long WINAPI shell_notimpl5(uint32_t a, uint32_t b, uint32_t c, void **out, uint32_t e)
{
    UNUSED(e);
    return shell_notimpl4(a, b, c, out);
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

static const struct w32_export shell32_exports[] = {
    W32_FN(SHGetFolderPathA),        W32_FN(SHGetFolderPathW),   W32_FN(SHGetSpecialFolderPathA),
    W32_FN(SHGetSpecialFolderPathW), W32_FN(CommandLineToArgvW), W32_FN(ShellExecuteA),
    W32_FN(ShellExecuteW),           W32_FN(SHGetKnownFolderPath), W32_FN(ShellExecuteExW),
    W32_FN(SHQueryUserNotificationState), W32_FN(ILFree),      W32_FN(SHChangeNotify),
    W32_AS("SHGetMalloc", shell_notimpl1), W32_AS("SHGetDesktopFolder", shell_notimpl1),
    W32_AS("SHGetSpecialFolderLocation", shell_notimpl3), W32_AS("SHGetPathFromIDListW", w32_zero2),
    W32_AS("SHParseDisplayName", shell_notimpl5), W32_AS("SHCreateItemFromParsingName", shell_notimpl4),
    W32_AS("SHOpenFolderAndSelectItems", shell_notimpl4), W32_AS("SHGetFileInfoW", w32_zero5),
    W32_AS("SHAppBarMessage", w32_zero2), W32_AS("SHGetStockIconInfo", shell_notimpl3),
    W32_AS("SHGetPropertyStoreForWindow", shell_notimpl3), W32_AS("SetCurrentProcessExplicitAppUserModelID", w32_zero1),
    W32_AS("Shell_NotifyIconW", w32_one2), W32_AS("DragQueryFileW", w32_zero4),
    W32_AS("SHCreateAssociationRegistration", shell_notimpl3), W32_AS("SHChangeNotifyRegister", w32_zero6),
    W32_AS("SHChangeNotifyDeregister", w32_one1), W32_AS("ILCreateFromPathW", w32_zero1),
    W32_AS("IsUserAnAdmin", w32_one0),
};

static const char *const shell32_ordinals[681] = {
    [2] = "SHChangeNotifyRegister", [4] = "SHChangeNotifyDeregister", [155] = "ILFree",
    [190] = "ILCreateFromPathW",    [680] = "IsUserAnAdmin",
};

W32_EXPORTS(shell32_module, "shell32.dll", shell32_exports, .ordinals = shell32_ordinals,
            .ordinal_count = ARRAY_SIZE(shell32_ordinals));

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
    W32_FN(RtlGetVersion),
    W32_FN(RtlGetNtVersionNumbers),
    W32_AS("RtlGenRandom", RtlGenRandom),
};

W32_EXPORTS(ntdll_module, "ntdll.dll", ntdll_exports);
