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

static const struct w32_export shell32_exports[] = {
    W32_FN(SHGetFolderPathA),        W32_FN(SHGetFolderPathW),   W32_FN(SHGetSpecialFolderPathA),
    W32_FN(SHGetSpecialFolderPathW), W32_FN(CommandLineToArgvW), W32_FN(ShellExecuteA),
    W32_FN(ShellExecuteW),
};

W32_EXPORTS(shell32_module, "shell32.dll", shell32_exports);

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
