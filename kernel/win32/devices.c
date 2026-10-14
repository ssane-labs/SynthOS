#include <synth/string.h>

#include "win32.h"

#define ERROR_NOT_SUPPORTED 50u
#define ERROR_NO_MORE_ITEMS 259u
#define ERROR_NOT_FOUND 1168u
#define ERROR_CANCELLED 1223u
#define ERROR_NO_SUCH_DEVINST 0xe000020bu
#define HIDP_STATUS_INVALID_PREPARSED_DATA 0xc0110001u
#define CR_NO_SUCH_DEVNODE 13u
#define TRUST_E_NOSIGNATURE 0x800b0100u
#define PDH_CSTATUS_NO_OBJECT 0xc0000bb8u
#define PDH_NO_DATA 0x800007d5u
#define PDH_INVALID_DATA 0xc0000bc6u
#define TBS_E_TPM_NOT_FOUND 0x8028400fu
#define NTE_NOT_SUPPORTED 0x80090029u
#define E_NOTIMPL 0x80004001u
#define E_HANDLE 0x80070006u
#define TYPE_E_ELEMENTNOTFOUND 0x8002802bu
#define JET_ERR_FEATURE_NOT_AVAILABLE ((uint32_t)-1001)

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
ARGS(9);
ARGS(11);

#define STUB(name, n, value, error)                                                                                    \
    static uint32_t WINAPI name(struct args##n a)                                                                      \
    {                                                                                                                  \
        UNUSED(a);                                                                                                     \
        if (error)                                                                                                     \
            w32_set_error(error);                                                                                      \
        return value;                                                                                                  \
    }

/* setupapi: an empty device tree */
STUB(SetupDiGetClassDevsW, 4, 0x5e7d0001u, 0)
STUB(SetupDiGetClassDevsExW, 7, 0x5e7d0001u, 0)
STUB(SetupDiCreateDeviceInfoList, 2, 0x5e7d0001u, 0)
STUB(SetupDiEnumDeviceInfo, 3, 0, ERROR_NO_MORE_ITEMS)
STUB(SetupDiEnumDeviceInterfaces, 5, 0, ERROR_NO_MORE_ITEMS)
STUB(SetupDiGetDeviceInterfaceDetailW, 6, 0, ERROR_NO_MORE_ITEMS)
STUB(SetupDiGetDevicePropertyW, 8, 0, ERROR_NOT_FOUND)
STUB(SetupDiGetDeviceRegistryPropertyW, 7, 0, ERROR_NOT_FOUND)
STUB(SetupDiOpenDevRegKey, 6, 0xffffffffu, ERROR_NOT_FOUND)
STUB(SetupDiOpenDeviceInfoW, 5, 0, ERROR_NO_SUCH_DEVINST)
STUB(SetupDiOpenDeviceInterfaceW, 4, 0, ERROR_NO_SUCH_DEVINST)

static const struct w32_export setupapi_exports[] = {
    W32_FN(SetupDiGetClassDevsW),
    W32_AS("SetupDiGetClassDevsA", SetupDiGetClassDevsW),
    W32_FN(SetupDiGetClassDevsExW),
    W32_FN(SetupDiCreateDeviceInfoList),
    W32_AS("SetupDiDestroyDeviceInfoList", w32_one1),
    W32_FN(SetupDiEnumDeviceInfo),
    W32_FN(SetupDiEnumDeviceInterfaces),
    W32_FN(SetupDiGetDeviceInterfaceDetailW),
    W32_FN(SetupDiGetDevicePropertyW),
    W32_FN(SetupDiGetDeviceRegistryPropertyW),
    W32_FN(SetupDiOpenDevRegKey),
    W32_FN(SetupDiOpenDeviceInfoW),
    W32_FN(SetupDiOpenDeviceInterfaceW),
};

/* hid: no HID devices are opened through Windows paths */
STUB(HidD_GetAttributes, 2, 0, ERROR_NOT_SUPPORTED)
STUB(HidD_GetPreparsedData, 2, 0, ERROR_NOT_SUPPORTED)
STUB(HidD_GetString, 3, 0, ERROR_NOT_SUPPORTED)
STUB(HidP_Caps, 4, HIDP_STATUS_INVALID_PREPARSED_DATA, 0)
STUB(HidP_GetCaps, 2, HIDP_STATUS_INVALID_PREPARSED_DATA, 0)
STUB(HidP_Value, 8, HIDP_STATUS_INVALID_PREPARSED_DATA, 0)
STUB(HidP_GetUsagesEx, 7, HIDP_STATUS_INVALID_PREPARSED_DATA, 0)

static void WINAPI HidD_GetHidGuid(uint8_t *guid)
{
    static const uint8_t hid[16] = {0xb2, 0x55, 0x1e, 0x4d, 0x6f, 0xf1, 0xcf, 0x11,
                                    0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30};
    memcpy(guid, hid, 16);
}

static const struct w32_export hid_exports[] = {
    W32_FN(HidD_GetHidGuid),
    W32_AS("HidD_FreePreparsedData", w32_one1),
    W32_FN(HidD_GetAttributes),
    W32_FN(HidD_GetPreparsedData),
    W32_AS("HidD_GetProductString", HidD_GetString),
    W32_AS("HidD_GetManufacturerString", HidD_GetString),
    W32_AS("HidD_GetSerialNumberString", HidD_GetString),
    W32_AS("HidP_GetButtonCaps", HidP_Caps),
    W32_AS("HidP_GetValueCaps", HidP_Caps),
    W32_FN(HidP_GetCaps),
    W32_AS("HidP_GetScaledUsageValue", HidP_Value),
    W32_AS("HidP_GetUsageValue", HidP_Value),
    W32_FN(HidP_GetUsagesEx),
};

STUB(CM_Get_DevNode_PropertyW, 6, CR_NO_SUCH_DEVNODE, 0)
STUB(CM_Get_Device_IDW, 4, CR_NO_SUCH_DEVNODE, 0)
STUB(CM_Get_Parent, 3, CR_NO_SUCH_DEVNODE, 0)
STUB(CM_Locate_DevNodeW, 3, CR_NO_SUCH_DEVNODE, 0)

static const struct w32_export cfgmgr32_exports[] = {
    W32_FN(CM_Get_DevNode_PropertyW),
    W32_FN(CM_Get_Device_IDW),
    W32_FN(CM_Get_Parent),
    W32_FN(CM_Locate_DevNodeW),
};

/* wintrust: nothing is Authenticode-signed here */
STUB(WinVerifyTrust, 3, TRUST_E_NOSIGNATURE, 0)
STUB(CryptCATAdminAcquireContext, 3, 0, ERROR_NOT_SUPPORTED)
STUB(CryptCATAdminCalcHashFromFileHandle, 4, 0, ERROR_NOT_SUPPORTED)
STUB(CryptCATAdminEnumCatalogFromHash, 5, 0, ERROR_NOT_FOUND)
STUB(CryptCATCatalogInfoFromContext, 3, 0, ERROR_NOT_SUPPORTED)

static const struct w32_export wintrust_exports[] = {
    W32_FN(WinVerifyTrust),
    W32_FN(CryptCATAdminAcquireContext),
    W32_AS("CryptCATAdminAcquireContext2", w32_fail5),
    W32_FN(CryptCATAdminCalcHashFromFileHandle),
    W32_AS("CryptCATAdminCalcHashFromFileHandle2", w32_fail5),
    W32_FN(CryptCATAdminEnumCatalogFromHash),
    W32_AS("CryptCATAdminReleaseCatalogContext", w32_one3),
    W32_AS("CryptCATAdminReleaseContext", w32_one2),
    W32_FN(CryptCATCatalogInfoFromContext),
    W32_AS("WTHelperGetProvCertFromChain", w32_zero2),
    W32_AS("WTHelperGetProvSignerFromChain", w32_zero4),
    W32_AS("WTHelperProvDataFromStateData", w32_zero1),
};

STUB(EvtCreateRenderContext, 3, 0, ERROR_NOT_SUPPORTED)
STUB(EvtNext, 6, 0, ERROR_NO_MORE_ITEMS)
STUB(EvtQuery, 4, 0, ERROR_NOT_SUPPORTED)
STUB(EvtRender, 7, 0, ERROR_NOT_SUPPORTED)

static const struct w32_export wevtapi_exports[] = {
    W32_AS("EvtClose", w32_one1), W32_FN(EvtCreateRenderContext), W32_FN(EvtNext), W32_FN(EvtQuery), W32_FN(EvtRender),
};

/* pdh: no performance counters */
STUB(PdhAddCounterW, 4, PDH_CSTATUS_NO_OBJECT, 0)
STUB(PdhCollectQueryData, 1, PDH_NO_DATA, 0)
STUB(PdhGetFormattedCounterValue, 4, PDH_INVALID_DATA, 0)

static uint32_t WINAPI PdhOpenQueryW(const WCHAR *source, uintptr_t data, HANDLE *query)
{
    UNUSED(source);
    UNUSED(data);
    if (query)
        *query = (HANDLE)0x5e7d0002u;
    return 0;
}

static const struct w32_export pdh_exports[] = {
    W32_FN(PdhOpenQueryW),
    W32_AS("PdhOpenQueryA", PdhOpenQueryW),
    W32_AS("PdhAddEnglishCounterW", PdhAddCounterW),
    W32_FN(PdhAddCounterW),
    W32_AS("PdhCloseQuery", w32_zero1),
    W32_FN(PdhCollectQueryData),
    W32_FN(PdhGetFormattedCounterValue),
    W32_AS("PdhRemoveCounter", w32_zero1),
};

/* dbghelp: symbols are not available */
STUB(SymFromAddr, 5, 0, ERROR_NOT_FOUND)
STUB(SymGetLineFromAddr64, 5, 0, ERROR_NOT_FOUND)
STUB(SymGetModuleInfo64, 4, 0, ERROR_NOT_FOUND)
STUB(SymGetSearchPathW, 3, 0, ERROR_NOT_SUPPORTED)
STUB(SymSrvGetFileIndexInfo, 3, 0, ERROR_NOT_SUPPORTED)
STUB(MiniDumpWriteDump, 7, 0, ERROR_NOT_SUPPORTED)

static DWORD WINAPI SymSetOptions(DWORD options)
{
    return options;
}

static const struct w32_export dbghelp_exports[] = {
    W32_AS("SymCleanup", w32_one1),
    W32_FN(SymFromAddr),
    W32_FN(SymGetLineFromAddr64),
    W32_FN(SymGetModuleInfo64),
    W32_FN(SymGetSearchPathW),
    W32_AS("SymInitialize", w32_one3),
    W32_AS("SymInitializeW", w32_one3),
    W32_FN(SymSetOptions),
    W32_AS("SymGetOptions", w32_zero0),
    W32_AS("SymSetSearchPathW", w32_one2),
    W32_FN(SymSrvGetFileIndexInfo),
    W32_FN(MiniDumpWriteDump),
};

STUB(PSGetPropertyKeyFromName, 2, TYPE_E_ELEMENTNOTFOUND, 0)

static const struct w32_export propsys_exports[] = {
    W32_AS("InitPropVariantFromCLSID", w32_notimpl2),
    W32_AS("PSCreateMemoryPropertyStore", w32_notimpl2),
    W32_FN(PSGetPropertyKeyFromName),
    W32_AS("VariantCompare", w32_zero2),
};

/* tbs / ncrypt: no TPM and no key storage provider */
STUB(Tbs2, 2, TBS_E_TPM_NOT_FOUND, 0)
STUB(Tbs7, 7, TBS_E_TPM_NOT_FOUND, 0)

static const struct w32_export tbs_exports[] = {
    W32_AS("Tbsi_GetDeviceInfo", Tbs2),
    W32_AS("Tbsi_Context_Create", Tbs2),
    W32_AS("Tbsip_Submit_Command", Tbs7),
    W32_AS("Tbsip_Context_Close", w32_zero1),
};

STUB(NCrypt2, 2, NTE_NOT_SUPPORTED, 0)
STUB(NCrypt3, 3, NTE_NOT_SUPPORTED, 0)
STUB(NCrypt5, 5, NTE_NOT_SUPPORTED, 0)
STUB(NCrypt6, 6, NTE_NOT_SUPPORTED, 0)
STUB(NCrypt8, 8, NTE_NOT_SUPPORTED, 0)

static const struct w32_export ncrypt_exports[] = {
    W32_AS("NCryptCreateClaim", NCrypt8),     W32_AS("NCryptCreatePersistedKey", NCrypt6),
    W32_AS("NCryptDeleteKey", NCrypt2),       W32_AS("NCryptExportKey", NCrypt8),
    W32_AS("NCryptFinalizeKey", NCrypt2),     W32_AS("NCryptFreeObject", w32_zero1),
    W32_AS("NCryptFreeBuffer", w32_zero1),    W32_AS("NCryptGetProperty", NCrypt6),
    W32_AS("NCryptImportKey", NCrypt8),       W32_AS("NCryptIsAlgSupported", NCrypt3),
    W32_AS("NCryptOpenKey", NCrypt5),         W32_AS("NCryptOpenStorageProvider", NCrypt3),
    W32_AS("NCryptSetProperty", NCrypt5),     W32_AS("NCryptSignHash", NCrypt8),
};

/* winspool: no printers */
static BOOL WINAPI EnumPrintersW(DWORD flags, const WCHAR *name, DWORD level, uint8_t *buf, DWORD size, DWORD *needed,
                                 DWORD *returned)
{
    UNUSED(flags);
    UNUSED(name);
    UNUSED(level);
    UNUSED(buf);
    UNUSED(size);
    if (needed)
        *needed = 0;
    if (returned)
        *returned = 0;
    return TRUE;
}

STUB(OpenPrinterW, 3, 0, ERROR_NOT_FOUND)
STUB(GetPrinterW, 5, 0, ERROR_NOT_FOUND)
STUB(GetPrinterDriverW, 6, 0, ERROR_NOT_FOUND)
STUB(GetDefaultPrinterW, 2, 0, ERROR_FILE_NOT_FOUND)

static const struct w32_export winspool_exports[] = {
    W32_FN(EnumPrintersW),
    W32_AS("EnumPrintersA", EnumPrintersW),
    W32_FN(OpenPrinterW),
    W32_AS("ClosePrinter", w32_one1),
    W32_FN(GetPrinterW),
    W32_FN(GetPrinterDriverW),
    W32_FN(GetDefaultPrinterW),
    W32_AS("DeviceCapabilitiesW", w32_neg5),
    W32_AS("DocumentPropertiesW", w32_neg6),
};

/* uxtheme: classic look, no theme data */
STUB(Theme5, 5, E_HANDLE, 0)
STUB(Theme6, 6, E_HANDLE, 0)
STUB(Theme7, 7, E_HANDLE, 0)

static const struct w32_export uxtheme_exports[] = {
    W32_AS("OpenThemeData", w32_zero2),
    W32_AS("OpenThemeDataForDpi", w32_zero3),
    W32_AS("CloseThemeData", w32_zero1),
    W32_AS("IsThemeActive", w32_zero0),
    W32_AS("IsAppThemed", w32_zero0),
    W32_AS("SetWindowTheme", w32_zero3),
    W32_AS("DrawThemeBackground", Theme6),
    W32_AS("GetThemeBackgroundContentRect", Theme6),
    W32_AS("GetThemePartSize", Theme7),
    W32_AS("GetThemeColor", Theme5),
    W32_AS("BufferedPaintInit", w32_zero0),
    W32_AS("BufferedPaintUnInit", w32_zero0),
};

static const struct w32_export dcomp_exports[] = {
    W32_AS("DCompositionCreateDevice", w32_notimpl3),
    W32_AS("DCompositionCreateDevice2", w32_notimpl3),
    W32_AS("DCompositionCreateSurfaceHandle", w32_notimpl3),
};

/* winusb / bluetooth: no devices */
STUB(Usb2, 2, 0, ERROR_NOT_SUPPORTED)
STUB(Usb3, 3, 0, ERROR_NOT_SUPPORTED)
STUB(Usb4, 4, 0, ERROR_NOT_SUPPORTED)
STUB(Usb6, 6, 0, ERROR_NOT_SUPPORTED)
STUB(Usb7, 7, 0, ERROR_NOT_SUPPORTED)

static const struct w32_export winusb_exports[] = {
    W32_AS("WinUsb_ControlTransfer", Usb7),        W32_AS("WinUsb_Free", w32_one1),
    W32_AS("WinUsb_GetAssociatedInterface", Usb3), W32_AS("WinUsb_GetOverlappedResult", Usb4),
    W32_AS("WinUsb_Initialize", Usb2),             W32_AS("WinUsb_ReadPipe", Usb6),
    W32_AS("WinUsb_ResetPipe", Usb2),              W32_AS("WinUsb_SetCurrentAlternateSetting", Usb2),
    W32_AS("WinUsb_WritePipe", Usb6),
};

STUB(BluetoothFind, 2, 0, ERROR_NO_MORE_ITEMS)
STUB(BluetoothGetRadioInfo, 2, ERROR_INVALID_HANDLE, 0)
STUB(BluetoothSdp, 4, ERROR_INVALID_PARAMETER, 0)

static const struct w32_export bthprops_exports[] = {
    W32_AS("BluetoothFindFirstRadio", BluetoothFind),
    W32_AS("BluetoothFindRadioClose", w32_one1),
    W32_AS("BluetoothFindFirstDevice", BluetoothFind),
    W32_AS("BluetoothFindNextDevice", BluetoothFind),
    W32_AS("BluetoothFindDeviceClose", w32_one1),
    W32_FN(BluetoothGetRadioInfo),
    W32_AS("BluetoothIsConnectable", w32_zero1),
    W32_AS("BluetoothEnableDiscovery", w32_zero2),
    W32_AS("BluetoothEnableIncomingConnections", w32_zero2),
    W32_AS("BluetoothSdpGetAttributeValue", BluetoothSdp),
    W32_AS("BluetoothSdpGetContainerElementData", BluetoothSdp),
};

static const struct w32_export comdlg32_exports[] = {
    W32_AS("PrintDlgExW", w32_notimpl1),       W32_AS("GetOpenFileNameW", w32_zero1),
    W32_AS("GetSaveFileNameW", w32_zero1),     W32_AS("GetOpenFileNameA", w32_zero1),
    W32_AS("GetSaveFileNameA", w32_zero1),     W32_AS("CommDlgExtendedError", w32_zero0),
};

static DWORD WINAPI DhcpCApiInitialize(DWORD *version)
{
    if (version)
        *version = 2;
    return 0;
}

STUB(DhcpRequestParams, 11, ERROR_NOT_SUPPORTED, 0)

static const struct w32_export dhcpcsvc_exports[] = {
    W32_FN(DhcpCApiInitialize),
    W32_AS("DhcpCApiCleanup", w32_zero0),
    W32_FN(DhcpRequestParams),
};

/* esent: the Jet database engine is not available */
STUB(Jet1, 1, JET_ERR_FEATURE_NOT_AVAILABLE, 0)
STUB(Jet2, 2, JET_ERR_FEATURE_NOT_AVAILABLE, 0)
STUB(Jet4, 4, JET_ERR_FEATURE_NOT_AVAILABLE, 0)
STUB(Jet5, 5, JET_ERR_FEATURE_NOT_AVAILABLE, 0)
STUB(Jet6, 6, JET_ERR_FEATURE_NOT_AVAILABLE, 0)
STUB(Jet7, 7, JET_ERR_FEATURE_NOT_AVAILABLE, 0)
STUB(Jet8, 8, JET_ERR_FEATURE_NOT_AVAILABLE, 0)

static const struct w32_export esent_exports[] = {
    W32_AS("JetAttachDatabase2W", Jet4),     W32_AS("JetBeginSessionW", Jet4),
    W32_AS("JetCloseTable", Jet2),           W32_AS("JetCreateInstanceW", Jet2),
    W32_AS("JetGetTableColumnInfoW", Jet6),  W32_AS("JetInit", Jet1),
    W32_AS("JetMove", Jet4),                 W32_AS("JetOpenDatabaseW", Jet5),
    W32_AS("JetOpenTableW", Jet7),           W32_AS("JetRetrieveColumn", Jet8),
    W32_AS("JetSetSystemParameterW", Jet5),  W32_AS("JetTerm", Jet1),
};

STUB(CredUIPromptForWindowsCredentialsW, 9, ERROR_CANCELLED, 0)

static const struct w32_export credui_exports[] = {
    W32_FN(CredUIPromptForWindowsCredentialsW),
};

static const struct w32_export cryptui_exports[] = {
    W32_AS("CryptUIDlgCertMgr", w32_zero1),
    W32_AS("CryptUIDlgViewCertificateW", w32_zero2),
};

static const struct w32_export ndfapi_exports[] = {
    W32_AS("NdfCloseIncident", w32_zero1),
    W32_AS("NdfCreateWebIncident", w32_notimpl2),
    W32_AS("NdfExecuteDiagnosis", w32_notimpl2),
};

W32_EXPORTS(setupapi_module, "setupapi.dll", setupapi_exports);
W32_EXPORTS(hid_module, "hid.dll", hid_exports);
W32_EXPORTS(cfgmgr32_module, "cfgmgr32.dll", cfgmgr32_exports);
W32_EXPORTS(wintrust_module, "wintrust.dll", wintrust_exports);
W32_EXPORTS(wevtapi_module, "wevtapi.dll", wevtapi_exports);
W32_EXPORTS(pdh_module, "pdh.dll", pdh_exports);
W32_EXPORTS(dbghelp_module, "dbghelp.dll", dbghelp_exports);
W32_EXPORTS(propsys_module, "propsys.dll", propsys_exports);
W32_EXPORTS(tbs_module, "tbs.dll", tbs_exports);
W32_EXPORTS(ncrypt_module, "ncrypt.dll", ncrypt_exports);
W32_EXPORTS(winspool_module, "winspool.drv", winspool_exports);
W32_EXPORTS(uxtheme_module, "uxtheme.dll", uxtheme_exports);
W32_EXPORTS(dcomp_module, "dcomp.dll", dcomp_exports);
W32_EXPORTS(winusb_module, "winusb.dll", winusb_exports);
W32_EXPORTS(bthprops_module, "bthprops.cpl", bthprops_exports);
W32_EXPORTS(comdlg32_module, "comdlg32.dll", comdlg32_exports);
W32_EXPORTS(dhcpcsvc_module, "dhcpcsvc.dll", dhcpcsvc_exports);
W32_EXPORTS(esent_module, "esent.dll", esent_exports);
W32_EXPORTS(credui_module, "credui.dll", credui_exports);
W32_EXPORTS(cryptui_module, "cryptui.dll", cryptui_exports);
W32_EXPORTS(ndfapi_module, "ndfapi.dll", ndfapi_exports);
