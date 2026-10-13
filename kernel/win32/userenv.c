#include <synth/string.h>

#include "win32.h"

static void *kernel32_fn(const char *name)
{
    return w32_builtin_lookup("kernel32.dll", name);
}

static BOOL WINAPI CreateEnvironmentBlock(WCHAR **out, HANDLE token, BOOL inherit)
{
    UNUSED(token);
    UNUSED(inherit);
    WCHAR *(WINAPI * get)(void) = (WCHAR * (WINAPI *)(void)) kernel32_fn("GetEnvironmentStringsW");
    BOOL(WINAPI * release)(WCHAR *) = (BOOL(WINAPI *)(WCHAR *))kernel32_fn("FreeEnvironmentStringsW");
    WCHAR *env = get ? get() : NULL;
    if (!env)
        return FALSE;
    size_t n = 0;
    while (env[n] || env[n + 1])
        n++;
    WCHAR *copy = w32_alloc((n + 2) * sizeof(WCHAR));
    if (copy)
        memcpy(copy, env, (n + 2) * sizeof(WCHAR));
    if (release)
        release(env);
    *out = copy;
    return copy != NULL;
}

static BOOL WINAPI DestroyEnvironmentBlock(WCHAR *env)
{
    w32_free(env);
    return TRUE;
}

static BOOL profile_dir(const char *var, WCHAR *out, DWORD *size)
{
    DWORD(WINAPI * get)(const char *, char *, DWORD) =
        (DWORD(WINAPI *)(const char *, char *, DWORD))kernel32_fn("GetEnvironmentVariableA");
    char path[260] = "C:\\Users";
    if (var && get)
        get(var, path, sizeof(path));
    DWORD n = (DWORD)strlen(path) + 1;
    if (!out || *size < n) {
        *size = n;
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    for (DWORD i = 0; i < n; i++)
        out[i] = (uint8_t)path[i];
    *size = n;
    return TRUE;
}

static BOOL WINAPI GetUserProfileDirectoryW(HANDLE token, WCHAR *out, DWORD *size)
{
    UNUSED(token);
    return profile_dir("USERPROFILE", out, size);
}

static BOOL WINAPI GetProfilesDirectoryW(WCHAR *out, DWORD *size)
{
    return profile_dir(NULL, out, size);
}

static BOOL WINAPI GetAllUsersProfileDirectoryW(WCHAR *out, DWORD *size)
{
    return profile_dir("ALLUSERSPROFILE", out, size);
}

static BOOL WINAPI GetProfileType(DWORD *flags)
{
    *flags = 0;
    return TRUE;
}

static HANDLE WINAPI EnterCriticalPolicySection(BOOL machine)
{
    UNUSED(machine);
    return (HANDLE)(uintptr_t)0x00030003u;
}

static DWORD WINAPI DeriveAppContainerSidFromAppContainerName(const WCHAR *name, void **sid)
{
    UNUSED(name);
    *sid = NULL;
    return 0x80004001u;
}

static const struct w32_export userenv_exports[] = {
    W32_FN(CreateEnvironmentBlock),
    W32_FN(DestroyEnvironmentBlock),
    W32_FN(GetUserProfileDirectoryW),
    W32_FN(GetProfilesDirectoryW),
    W32_FN(GetAllUsersProfileDirectoryW),
    W32_AS("GetDefaultUserProfileDirectoryW", GetProfilesDirectoryW),
    W32_FN(GetProfileType),
    W32_FN(EnterCriticalPolicySection),
    W32_AS("LeaveCriticalPolicySection", w32_one1),
    W32_AS("RegisterGPNotification", w32_one2),
    W32_AS("UnregisterGPNotification", w32_one1),
    W32_FN(DeriveAppContainerSidFromAppContainerName),
};

W32_EXPORTS(userenv_module, "userenv.dll", userenv_exports);
