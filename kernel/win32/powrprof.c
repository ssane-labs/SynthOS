#include <synth/string.h>

#include "win32.h"

#define STATUS_BUFFER_TOO_SMALL 0xc0000023u
#define STATUS_INVALID_PARAMETER 0xc000000du

static const uint8_t balanced_scheme[16] = {0x22, 0x42, 0x1b, 0x38, 0x94, 0xf6, 0xf0, 0x41,
                                            0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e};

static uint32_t fill(void *out, DWORD cap, const void *data, DWORD size)
{
    if (!out || cap < size)
        return STATUS_BUFFER_TOO_SMALL;
    memset(out, 0, cap);
    if (data)
        memcpy(out, data, size);
    return 0;
}

static uint32_t WINAPI CallNtPowerInformation(int level, void *in, DWORD in_size, void *out, DWORD out_size)
{
    UNUSED(in);
    UNUSED(in_size);
    switch (level) {
    case 4: return fill(out, out_size, NULL, 76);
    case 5: {
        uint8_t state[32] = {1};
        return fill(out, out_size, state, sizeof(state));
    }
    case 11: {
        DWORD cpu[6] = {0, 2000, 2000, 2000, 1, 0};
        return fill(out, out_size, cpu, sizeof(cpu));
    }
    case 12: {
        DWORD info[4] = {0, 0, 0, 0};
        return fill(out, out_size, info, sizeof(info));
    }
    case 16: {
        DWORD state = 0x80000000u;
        return fill(out, out_size, &state, sizeof(state));
    }
    default: return out ? fill(out, out_size, NULL, out_size) : STATUS_INVALID_PARAMETER;
    }
}

static int WINAPI PowerDeterminePlatformRoleEx(ULONG version)
{
    UNUSED(version);
    return 1;
}

static int WINAPI PowerDeterminePlatformRole(void)
{
    return 1;
}

static BOOL WINAPI GetPwrCapabilities(void *caps)
{
    if (!caps)
        return FALSE;
    memset(caps, 0, 76);
    return TRUE;
}

static DWORD WINAPI PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, HANDLE *out)
{
    UNUSED(flags);
    UNUSED(recipient);
    if (out)
        *out = (HANDLE)0x5e5e0001u;
    return 0;
}

static DWORD WINAPI PowerSettingRegisterNotification(const void *guid, DWORD flags, HANDLE recipient, HANDLE *out)
{
    UNUSED(guid);
    UNUSED(flags);
    UNUSED(recipient);
    if (out)
        *out = (HANDLE)0x5e5e0002u;
    return 0;
}

static DWORD WINAPI PowerGetActiveScheme(HANDLE root, void **out)
{
    UNUSED(root);
    void *(WINAPI * local_alloc)(UINT, SIZE_T) =
        (void *(WINAPI *)(UINT, SIZE_T))w32_builtin_lookup("kernel32.dll", "LocalAlloc");
    void *guid = local_alloc ? local_alloc(0, 16) : NULL;
    if (!guid || !out)
        return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(guid, balanced_scheme, 16);
    *out = guid;
    return 0;
}

static DWORD WINAPI PowerReadValueIndex(HANDLE root, const void *scheme, const void *sub, const void *setting,
                                        DWORD *value)
{
    UNUSED(root);
    UNUSED(scheme);
    UNUSED(sub);
    UNUSED(setting);
    UNUSED(value);
    return ERROR_FILE_NOT_FOUND;
}

static DWORD WINAPI PowerReadValue(HANDLE root, const void *scheme, const void *sub, const void *setting, DWORD *type,
                                   uint8_t *buffer, DWORD *size)
{
    UNUSED(root);
    UNUSED(scheme);
    UNUSED(sub);
    UNUSED(setting);
    UNUSED(type);
    UNUSED(buffer);
    UNUSED(size);
    return ERROR_FILE_NOT_FOUND;
}

static const struct w32_export powrprof_exports[] = {
    W32_FN(CallNtPowerInformation),
    W32_FN(PowerDeterminePlatformRoleEx),
    W32_FN(PowerDeterminePlatformRole),
    W32_FN(GetPwrCapabilities),
    W32_FN(PowerRegisterSuspendResumeNotification),
    W32_AS("PowerUnregisterSuspendResumeNotification", w32_zero1),
    W32_FN(PowerSettingRegisterNotification),
    W32_AS("PowerSettingUnregisterNotification", w32_zero1),
    W32_FN(PowerGetActiveScheme),
    W32_AS("PowerReadACValueIndex", PowerReadValueIndex),
    W32_AS("PowerReadDCValueIndex", PowerReadValueIndex),
    W32_AS("PowerReadACValue", PowerReadValue),
    W32_AS("PowerReadDCValue", PowerReadValue),
};

W32_EXPORTS(powrprof_module, "powrprof.dll", powrprof_exports);
