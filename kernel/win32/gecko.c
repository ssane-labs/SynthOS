#include <synth/fmt.h>
#include <synth/string.h>

#include "win32.h"

/* What Firefox (Gecko) looks for beyond the APIs other programs use: most are features it probes and does without
   (transactions, touch, gestures, carets of IMEs, device notifications), a few do real work on top of kernel32,
   advapi32 and gdi32. */

#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_NOT_SUPPORTED 50u

#define ARGS(n)                                                                                                        \
    struct args##n {                                                                                                   \
        uint32_t v[n];                                                                                                 \
    }
ARGS(1);
ARGS(2);
ARGS(3);
ARGS(4);
ARGS(5);
ARGS(6);
ARGS(7);
ARGS(8);
ARGS(10);
ARGS(11);

#define STUB(name, n, value, error)                                                                                    \
    static uint32_t WINAPI name(struct args##n a)                                                                      \
    {                                                                                                                  \
        UNUSED(a);                                                                                                     \
        if (error)                                                                                                     \
            w32_set_error(error);                                                                                      \
        return value;                                                                                                  \
    }

#define STUB0(name, value)                                                                                             \
    static uint32_t WINAPI name(void)                                                                                  \
    {                                                                                                                  \
        return value;                                                                                                  \
    }

static void *fn(const char *module, const char *name)
{
    return w32_builtin_lookup(module, name);
}

/* advapi32 */
STUB(CreatePrivateObjectSecurityEx, 8, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(CredDeleteA, 3, 0, 1168)
STUB(CredFree, 1, 0, 0)
STUB(CredReadA, 4, 0, 1168)
STUB(CredWriteA, 2, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(DestroyPrivateObjectSecurity, 1, 1, 0)
STUB(DuplicateToken, 3, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(SetKernelObjectSecurity, 3, 1, 0)

/* avrt, ktmw32, cfgmgr32 */
STUB(AvSetMmThreadCharacteristicsA, 2, 1, 0)
STUB(AvRevertMmThreadCharacteristics, 1, 1, 0)
STUB(CreateTransaction, 7, (uint32_t)-1, ERROR_CALL_NOT_IMPLEMENTED)
STUB(CommitTransaction, 1, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(CM_Register_Notification, 4, 19, 0)
STUB(CM_Unregister_Notification, 1, 0, 0)

static DWORD WINAPI CM_Get_Device_Interface_List_SizeW(ULONG *len, const void *guid, const WCHAR *id, ULONG flags)
{
    UNUSED(guid);
    UNUSED(id);
    UNUSED(flags);
    if (len)
        *len = 1;
    return 0;
}

static DWORD WINAPI CM_Get_Device_Interface_ListW(const void *guid, const WCHAR *id, WCHAR *buf, ULONG len, ULONG flags)
{
    UNUSED(guid);
    UNUSED(id);
    UNUSED(flags);
    if (buf && len)
        buf[0] = 0;
    return 0;
}

/* comdlg32, crypt32, d3dcompiler, dbghelp, dnsapi, dwmapi */
STUB(ChooseColorW, 1, 0, 0)
STUB(CertNameToStrA, 5, 0, 0)
STUB(CryptMsgOpenToDecode, 6, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(CryptMsgUpdate, 4, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(D3DCompile, 11, E_NOTIMPL, 0)
STUB(EnumerateLoadedModules64, 3, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(SymLoadModule64, 7, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(DnsQuery_A, 6, 9003, 0)
STUB(DnsFree, 2, 0, 0)
STUB(DwmInvalidateIconicBitmaps, 1, E_NOTIMPL, 0)
STUB(DwmSetIconicLivePreviewBitmap, 4, E_NOTIMPL, 0)
STUB(DwmSetIconicThumbnail, 3, E_NOTIMPL, 0)

static BOOL WINAPI CryptBinaryToStringW(const uint8_t *data, DWORD len, DWORD flags, WCHAR *out, DWORD *chars)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if ((flags & 0xff) != 1 || !chars) {
        w32_set_error(87);
        return FALSE;
    }
    DWORD need = (len + 2) / 3 * 4 + 1;
    if (!out) {
        *chars = need;
        return TRUE;
    }
    if (*chars < need) {
        w32_set_error(234);
        return FALSE;
    }
    DWORD k = 0;
    for (DWORD i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < len ? data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
        out[k++] = (WCHAR)b64[v >> 18 & 63];
        out[k++] = (WCHAR)b64[v >> 12 & 63];
        out[k++] = i + 1 < len ? (WCHAR)b64[v >> 6 & 63] : '=';
        out[k++] = i + 2 < len ? (WCHAR)b64[v & 63] : '=';
    }
    out[k] = 0;
    *chars = k;
    return TRUE;
}

/* gdi32 */
STUB(AbortDoc, 1, (uint32_t)-1, 0)
STUB(CreateDIBitmap, 6, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(ExtCreateRegion, 3, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(GetCharWidth32A, 4, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(GetFontUnicodeRanges, 2, 0, 0)
STUB(GetGlyphIndicesW, 5, (uint32_t)-1, 0)
STUB(GetGlyphOutlineW, 7, (uint32_t)-1, 0)
STUB(GetOutlineTextMetricsA, 3, 0, 0)
STUB(GetRandomRgn, 3, 0, 0)
STUB(GetGraphicsMode, 1, 1, 0)

static int WINAPI ExtSelectClipRgn(HANDLE dc, HANDLE rgn, int mode)
{
    UNUSED(mode);
    int(WINAPI * select)(HANDLE, HANDLE) = fn("gdi32.dll", "SelectClipRgn");
    return select(dc, rgn);
}

/* imm32, iphlpapi */
STUB(ImmGetCompositionFontW, 2, 0, 0)
STUB(ImmSetCompositionFontW, 2, 0, 0)
STUB(ImmGetCompositionStringA, 4, 0, 0)
STUB(ImmGetDefaultIMEWnd, 1, 0, 0)
STUB(ImmGetDescriptionW, 3, 0, 0)
STUB(ImmGetProperty, 2, 0, 0)
STUB(ImmSetOpenStatus, 2, 0, 0)
STUB(NotifyIpInterfaceChange, 5, ERROR_NOT_SUPPORTED, 0)
STUB(CancelMibChangeNotify2, 1, 0, 0)
STUB(GetBestInterfaceEx, 2, ERROR_NOT_SUPPORTED, 0)
STUB(GetIpInterfaceTable, 2, ERROR_NOT_SUPPORTED, 0)

/* kernel32 */
STUB(CreateMemoryResourceNotification, 1, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(GetApplicationRestartSettings, 4, E_FAIL, 0)
STUB(RegisterApplicationRestart, 2, 0, 0)
STUB0(UnregisterApplicationRestart, 0)
/* one CPU set, like the one processor GetSystemInfo reports */
static BOOL WINAPI GetSystemCpuSetInformation(uint8_t *info, ULONG len, ULONG *ret, HANDLE process, ULONG flags)
{
    UNUSED(process);
    UNUSED(flags);
    if (ret)
        *ret = 32;
    if (!info || len < 32) {
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(info, 0, 32);
    info[0] = 32;
    memcpy(info + 8, &(DWORD){0x100}, 4);
    return TRUE;
}
STUB(K32QueryWorkingSet, 3, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(ReplaceFileA, 6, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(SetThreadIdealProcessor, 2, 0, 0)

static HANDLE WINAPI CreateWaitableTimerExA(void *sa, const char *name, DWORD flags, DWORD access)
{
    HANDLE(WINAPI * create)(void *, const WCHAR *, DWORD, DWORD) = fn("kernel32.dll", "CreateWaitableTimerExW");
    WCHAR *wide = name ? w32_widen(name) : NULL;
    HANDLE h = create(sa, wide, flags, access);
    w32_free(wide);
    return h;
}

static BOOL WINAPI GetComputerNameExA(int type, char *buf, DWORD *size)
{
    UNUSED(type);
    BOOL(WINAPI * get)(char *, DWORD *) = fn("kernel32.dll", "GetComputerNameA");
    return get(buf, size);
}

static BOOL WINAPI GetOverlappedResultEx(HANDLE h, void *ov, DWORD *bytes, DWORD ms, BOOL alertable)
{
    UNUSED(alertable);
    BOOL(WINAPI * get)(HANDLE, void *, DWORD *, BOOL) = fn("kernel32.dll", "GetOverlappedResult");
    return get(h, ov, bytes, ms != 0);
}

static DWORD WINAPI GetProcessIdOfThread(HANDLE thread)
{
    UNUSED(thread);
    DWORD(WINAPI * pid)(void) = fn("kernel32.dll", "GetCurrentProcessId");
    return pid();
}

static BOOL WINAPI GetTimeZoneInformationForYear(uint16_t year, void *dynamic, void *tzi)
{
    UNUSED(year);
    UNUSED(dynamic);
    DWORD(WINAPI * get)(void *) = fn("kernel32.dll", "GetTimeZoneInformation");
    return get(tzi) != 0xffffffffu;
}

static BOOL WINAPI SetThreadStackGuarantee(ULONG *size)
{
    if (size)
        *size = 0;
    return TRUE;
}

static BOOL WINAPI SetThreadErrorMode(DWORD mode, DWORD *old)
{
    UNUSED(mode);
    if (old)
        *old = 0;
    return TRUE;
}

/* msimg32, ntdll */
STUB(GradientFill, 6, 1, 0)
STUB(NtCancelIoFileEx, 3, 0xc0000225u, 0)
STUB(NtDeviceIoControlFile, 10, 0xc0000002u, 0)
STUB(NtOpenDirectoryObject, 3, 0xc0000034u, 0)
STUB(NtQueryDirectoryObject, 7, 0x8000001au, 0)

struct unicode_string {
    uint16_t length, max_length;
    WCHAR *buffer;
};

static DWORD WINAPI RtlDosPathNameToNtPathName_U_WithStatus(const WCHAR *dos, struct unicode_string *nt,
                                                            WCHAR **part, void *reserved)
{
    UNUSED(reserved);
    DWORD(WINAPI * full)(const WCHAR *, DWORD, WCHAR *, WCHAR **) = fn("kernel32.dll", "GetFullPathNameW");
    WCHAR path[PATH_MAX_LEN];
    DWORD n = dos ? full(dos, PATH_MAX_LEN, path, NULL) : 0;
    if (!n || n >= PATH_MAX_LEN)
        return 0xc000003bu;
    WCHAR *buf = w32_alloc((n + 5) * sizeof(WCHAR));
    if (!buf)
        return 0xc0000017u;
    static const WCHAR prefix[4] = {'\\', '?', '?', '\\'};
    memcpy(buf, prefix, sizeof(prefix));
    memcpy(buf + 4, path, (n + 1) * sizeof(WCHAR));
    nt->buffer = buf;
    nt->length = (uint16_t)((n + 4) * 2);
    nt->max_length = (uint16_t)((n + 5) * 2);
    if (part) {
        *part = NULL;
        for (WCHAR *p = buf; *p; p++)
            if (*p == '\\')
                *part = p + 1;
    }
    return 0;
}

/* ole32, oleaut32, propsys, rpcrt4 */
STUB(CoGetCallerTID, 1, E_NOTIMPL, 0)
STUB(CoRegisterChannelHook, 2, E_NOTIMPL, 0)
STUB0(OleFlushClipboard, 0)
STUB(OleGetClipboard, 1, E_FAIL, 0)
STUB(OleQueryLinkFromData, 1, 1, 0)
STUB(OleSetClipboard, 1, 0, 0)
STUB(StgCreateStorageEx, 8, E_NOTIMPL, 0)
STUB(CreateStdDispatch, 4, E_NOTIMPL, 0)
STUB(DispGetIDsOfNames, 4, E_NOTIMPL, 0)
STUB(OleCreatePropertyFrame, 11, E_NOTIMPL, 0)
STUB(PropVariantToString, 3, E_NOTIMPL, 0)

static void WINAPI SetOaNoCache(void)
{
}

static DWORD WINAPI UuidToStringA(const uint8_t *uuid, char **out)
{
    char *s = w32_alloc(37);
    if (!s)
        return 14;
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, uuid, 4);
    memcpy(&d2, uuid + 4, 2);
    memcpy(&d3, uuid + 6, 2);
    ksnprintf(s, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", d1, d2, d3, uuid[8], uuid[9], uuid[10],
              uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    *out = s;
    return 0;
}

/* shell32, shlwapi */
STUB(GetCurrentProcessExplicitAppUserModelID, 1, E_FAIL, 0)
STUB(SHAddToRecentDocs, 2, 0, 0)
STUB(SHGetKnownFolderItem, 5, E_NOTIMPL, 0)
STUB(PathIsNetworkPathW, 1, 0, 0)
STUB(PathUnExpandEnvStringsW, 3, 0, 0)

/* user32 */
STUB(ActivateKeyboardLayout, 2, 0x04090409u, 0)
STUB(LoadKeyboardLayoutA, 2, 0x04090409u, 0)
STUB(UnloadKeyboardLayout, 1, 1, 0)
STUB(AnimateWindow, 3, 1, 0)
STUB(BroadcastSystemMessageW, 5, 1, 0)
STUB(CloseGestureInfoHandle, 1, 1, 0)
STUB(CreateCaret, 4, 1, 0)
STUB0(DestroyCaret, 1)
STUB(ShowCaret, 1, 1, 0)
STUB(SetCaretPos, 2, 1, 0)
STUB(CreateSyntheticPointerDevice, 3, 0, ERROR_NOT_SUPPORTED)
STUB(InjectSyntheticPointerInput, 3, 0, ERROR_NOT_SUPPORTED)
STUB(GetAutoRotationState, 1, 0, ERROR_NOT_SUPPORTED)
STUB(GetClassWord, 2, 0, 0)
STUB(GetGUIThreadInfo, 2, 0, ERROR_CALL_NOT_IMPLEMENTED)
STUB(GetGestureInfo, 2, 0, ERROR_NOT_SUPPORTED)
STUB0(GetOpenClipboardWindow, 0)
STUB(GetPointerFrameTouchInfo, 3, 0, ERROR_NOT_SUPPORTED)
STUB(GetRawInputDeviceInfoA, 4, (uint32_t)-1, 0)
STUB(GetTopWindow, 1, 0, 0)
STUB(GetTouchInputInfo, 4, 0, ERROR_NOT_SUPPORTED)
STUB0(InSendMessage, 0)
STUB(InSendMessageEx, 1, 0, 0)
STUB(IsGUIThread, 1, 1, 0)
STUB(RegisterTouchWindow, 2, 1, 0)
STUB(ReplyMessage, 1, 0, 0)
STUB(SetGestureConfig, 5, 1, 0)
STUB(SetSysColors, 3, 1, 0)
STUB(WaitForInputIdle, 2, 0, 0)

static BOOL WINAPI GetKeyboardLayoutNameA(char *name)
{
    str_copy(name, "00000409", 9);
    return TRUE;
}

/* uxtheme, windowscodecs, wininet, winmm, ws2_32 */
STUB(BeginPanningFeedback, 1, 1, 0)
STUB(EndPanningFeedback, 2, 1, 0)
STUB(UpdatePanningFeedback, 4, 1, 0)
STUB(WICConvertBitmapSource, 3, E_NOTIMPL, 0)
STUB(midiInMessage, 4, 8, 0)
STUB(midiOutMessage, 4, 8, 0)
STUB(WSCGetProviderInfo, 6, (uint32_t)-1, 0)

static BOOL WINAPI InternetGetConnectedStateExW(DWORD *flags, WCHAR *name, DWORD len, DWORD reserved)
{
    UNUSED(reserved);
    if (flags)
        *flags = 2;
    if (name && len)
        name[0] = 0;
    return TRUE;
}

static const struct w32_export gecko_advapi32[] = {
    W32_FN(CreatePrivateObjectSecurityEx), W32_FN(CredDeleteA), W32_FN(CredFree), W32_FN(CredReadA),
    W32_FN(CredWriteA), W32_FN(DestroyPrivateObjectSecurity), W32_FN(DuplicateToken),
    W32_FN(SetKernelObjectSecurity),
};
static const struct w32_export gecko_avrt[] = {W32_FN(AvSetMmThreadCharacteristicsA),
                                               W32_AS("AvSetMmThreadCharacteristicsW", AvSetMmThreadCharacteristicsA),
                                               W32_FN(AvRevertMmThreadCharacteristics)};
static const struct w32_export gecko_ktmw32[] = {W32_FN(CreateTransaction), W32_FN(CommitTransaction)};
static const struct w32_export gecko_cfgmgr32[] = {W32_FN(CM_Register_Notification), W32_FN(CM_Unregister_Notification),
                                                   W32_FN(CM_Get_Device_Interface_List_SizeW),
                                                   W32_FN(CM_Get_Device_Interface_ListW)};
static const struct w32_export gecko_comdlg32[] = {W32_FN(ChooseColorW)};
static const struct w32_export gecko_crypt32[] = {W32_FN(CertNameToStrA), W32_FN(CryptMsgOpenToDecode),
                                                  W32_FN(CryptMsgUpdate), W32_FN(CryptBinaryToStringW)};
static const struct w32_export gecko_d3dcompiler[] = {W32_FN(D3DCompile)};
static const struct w32_export gecko_dbghelp[] = {W32_FN(EnumerateLoadedModules64), W32_FN(SymLoadModule64)};
static const struct w32_export gecko_dnsapi[] = {W32_FN(DnsQuery_A), W32_FN(DnsFree)};
static const struct w32_export gecko_dwmapi[] = {W32_FN(DwmInvalidateIconicBitmaps), W32_FN(DwmSetIconicLivePreviewBitmap),
                                                 W32_FN(DwmSetIconicThumbnail)};
static const struct w32_export gecko_gdi32[] = {
    W32_FN(AbortDoc), W32_FN(CreateDIBitmap), W32_FN(ExtCreateRegion), W32_FN(GetCharWidth32A),
    W32_FN(GetFontUnicodeRanges), W32_FN(GetGlyphIndicesW), W32_FN(GetGlyphOutlineW), W32_FN(GetOutlineTextMetricsA),
    W32_FN(GetRandomRgn), W32_FN(GetGraphicsMode), W32_FN(ExtSelectClipRgn),
};
static const struct w32_export gecko_imm32[] = {
    W32_FN(ImmGetCompositionFontW), W32_FN(ImmSetCompositionFontW), W32_FN(ImmGetCompositionStringA),
    W32_FN(ImmGetDefaultIMEWnd), W32_FN(ImmGetDescriptionW), W32_FN(ImmGetProperty), W32_FN(ImmSetOpenStatus),
};
static const struct w32_export gecko_iphlpapi[] = {W32_FN(NotifyIpInterfaceChange), W32_FN(CancelMibChangeNotify2),
                                                   W32_FN(GetBestInterfaceEx), W32_FN(GetIpInterfaceTable)};
static const struct w32_export gecko_kernel32[] = {
    W32_FN(CreateMemoryResourceNotification), W32_FN(GetApplicationRestartSettings), W32_FN(RegisterApplicationRestart),
    W32_FN(UnregisterApplicationRestart), W32_FN(GetSystemCpuSetInformation), W32_FN(K32QueryWorkingSet),
    W32_FN(ReplaceFileA), W32_FN(SetThreadIdealProcessor),
    W32_FN(CreateWaitableTimerExA), W32_FN(GetComputerNameExA), W32_FN(GetOverlappedResultEx),
    W32_FN(GetProcessIdOfThread), W32_FN(GetTimeZoneInformationForYear), W32_FN(SetThreadErrorMode),
    W32_FN(SetThreadStackGuarantee),
};
static const struct w32_export gecko_msimg32[] = {W32_FN(GradientFill)};
static const struct w32_export gecko_ntdll[] = {
    W32_FN(NtCancelIoFileEx), W32_FN(NtDeviceIoControlFile), W32_FN(NtOpenDirectoryObject),
    W32_FN(NtQueryDirectoryObject), W32_FN(RtlDosPathNameToNtPathName_U_WithStatus),
};
static const struct w32_export gecko_ole32[] = {
    W32_FN(CoGetCallerTID), W32_FN(CoRegisterChannelHook), W32_FN(OleFlushClipboard), W32_FN(OleGetClipboard),
    W32_FN(OleQueryLinkFromData), W32_FN(OleSetClipboard), W32_FN(StgCreateStorageEx),
};
static const struct w32_export gecko_oleaut32[] = {W32_FN(CreateStdDispatch), W32_FN(DispGetIDsOfNames),
                                                   W32_FN(OleCreatePropertyFrame), W32_FN(SetOaNoCache)};
static const struct w32_export gecko_propsys[] = {W32_FN(PropVariantToString)};
static const struct w32_export gecko_rpcrt4[] = {W32_FN(UuidToStringA)};
static const struct w32_export gecko_shell32[] = {W32_FN(GetCurrentProcessExplicitAppUserModelID),
                                                  W32_FN(SHAddToRecentDocs), W32_FN(SHGetKnownFolderItem)};
static const struct w32_export gecko_shlwapi[] = {W32_FN(PathIsNetworkPathW), W32_FN(PathUnExpandEnvStringsW)};
static const struct w32_export gecko_user32[] = {
    W32_FN(ActivateKeyboardLayout), W32_FN(LoadKeyboardLayoutA), W32_FN(UnloadKeyboardLayout), W32_FN(AnimateWindow),
    W32_FN(BroadcastSystemMessageW), W32_FN(CloseGestureInfoHandle), W32_FN(CreateCaret), W32_FN(DestroyCaret),
    W32_FN(ShowCaret), W32_FN(SetCaretPos), W32_FN(CreateSyntheticPointerDevice), W32_FN(InjectSyntheticPointerInput),
    W32_FN(GetAutoRotationState), W32_FN(GetClassWord), W32_FN(GetGUIThreadInfo), W32_FN(GetGestureInfo),
    W32_FN(GetOpenClipboardWindow), W32_FN(GetPointerFrameTouchInfo), W32_FN(GetRawInputDeviceInfoA),
    W32_FN(GetTopWindow), W32_FN(GetTouchInputInfo), W32_FN(InSendMessage), W32_FN(InSendMessageEx),
    W32_FN(IsGUIThread), W32_FN(RegisterTouchWindow), W32_FN(ReplyMessage), W32_FN(SetGestureConfig),
    W32_FN(SetSysColors), W32_FN(WaitForInputIdle), W32_FN(GetKeyboardLayoutNameA),
};
static const struct w32_export gecko_uxtheme[] = {W32_FN(BeginPanningFeedback), W32_FN(EndPanningFeedback),
                                                  W32_FN(UpdatePanningFeedback)};
static const struct w32_export gecko_wic[] = {W32_FN(WICConvertBitmapSource)};
static const struct w32_export gecko_wininet[] = {W32_FN(InternetGetConnectedStateExW)};
static const struct w32_export gecko_winmm[] = {W32_FN(midiInMessage), W32_FN(midiOutMessage)};
static const struct w32_export gecko_ws2_32[] = {W32_FN(WSCGetProviderInfo)};

W32_EXPORTS(gecko_advapi32_module, "advapi32.dll", gecko_advapi32);
W32_EXPORTS(gecko_avrt_module, "avrt.dll", gecko_avrt);
W32_EXPORTS(gecko_ktmw32_module, "ktmw32.dll", gecko_ktmw32);
W32_EXPORTS(gecko_cfgmgr32_module, "cfgmgr32.dll", gecko_cfgmgr32);
W32_EXPORTS(gecko_comdlg32_module, "comdlg32.dll", gecko_comdlg32);
W32_EXPORTS(gecko_crypt32_module, "crypt32.dll", gecko_crypt32);
W32_EXPORTS(gecko_d3dcompiler_module, "d3dcompiler_47.dll", gecko_d3dcompiler);
W32_EXPORTS(gecko_dbghelp_module, "dbghelp.dll", gecko_dbghelp);
W32_EXPORTS(gecko_dnsapi_module, "dnsapi.dll", gecko_dnsapi);
W32_EXPORTS(gecko_dwmapi_module, "dwmapi.dll", gecko_dwmapi);
W32_EXPORTS(gecko_gdi32_module, "gdi32.dll", gecko_gdi32);
W32_EXPORTS(gecko_imm32_module, "imm32.dll", gecko_imm32);
W32_EXPORTS(gecko_iphlpapi_module, "iphlpapi.dll", gecko_iphlpapi);
W32_EXPORTS(gecko_kernel32_module, "kernel32.dll", gecko_kernel32);
W32_EXPORTS(gecko_msimg32_module, "msimg32.dll", gecko_msimg32);
W32_EXPORTS(gecko_ntdll_module, "ntdll.dll", gecko_ntdll);
W32_EXPORTS(gecko_ole32_module, "ole32.dll", gecko_ole32);
W32_EXPORTS(gecko_oleaut32_module, "oleaut32.dll", gecko_oleaut32);
W32_EXPORTS(gecko_propsys_module, "propsys.dll", gecko_propsys);
W32_EXPORTS(gecko_rpcrt4_module, "rpcrt4.dll", gecko_rpcrt4);
W32_EXPORTS(gecko_shell32_module, "shell32.dll", gecko_shell32);
W32_EXPORTS(gecko_shlwapi_module, "shlwapi.dll", gecko_shlwapi);
W32_EXPORTS(gecko_user32_module, "user32.dll", gecko_user32);
W32_EXPORTS(gecko_uxtheme_module, "uxtheme.dll", gecko_uxtheme);
W32_EXPORTS(gecko_wic_module, "windowscodecs.dll", gecko_wic);
W32_EXPORTS(gecko_wininet_module, "wininet.dll", gecko_wininet);
W32_EXPORTS(gecko_winmm_module, "winmm.dll", gecko_winmm);
W32_EXPORTS(gecko_ws2_32_module, "ws2_32.dll", gecko_ws2_32);
