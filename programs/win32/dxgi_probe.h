/* What a program sees of the graphics adapter through DXGI (Chromium decides on GPU use from it). Shared by the 32-
   and 64-bit test programs. */
#define COBJMACROS
#include <dxgi.h>

static void dxgi_probe(void)
{
    static const GUID factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    static const GUID device = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    HRESULT(WINAPI * create)(REFIID, void **) =
        (HRESULT(WINAPI *)(REFIID, void **))(void (*)(void))GetProcAddress(LoadLibraryA("dxgi.dll"),
                                                                            "CreateDXGIFactory1");
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *a = NULL, *next = NULL;
    IDXGIOutput *out = NULL;
    DXGI_ADAPTER_DESC1 d;
    LARGE_INTEGER umd = {{0, 0}};
    char name[64] = "";
    memset(&d, 0, sizeof(d));
    int made = create && create(&factory1, (void **)&f) == S_OK && f;
    int one = made && IDXGIFactory1_EnumAdapters1(f, 0, &a) == S_OK && IDXGIAdapter1_GetDesc1(a, &d) == S_OK;
    int more = made && IDXGIFactory1_EnumAdapters1(f, 1, &next) == DXGI_ERROR_NOT_FOUND && !next;
    int outputs = one && IDXGIAdapter1_EnumOutputs(a, 0, &out) == DXGI_ERROR_NOT_FOUND;
    int umd_ok = one && IDXGIAdapter1_CheckInterfaceSupport(a, &device, &umd) == S_OK && HIWORD(umd.HighPart) == 10;
    WideCharToMultiByte(CP_ACP, 0, d.Description, -1, name, sizeof(name), NULL, NULL);
    printf("dxgi made=%d vendor=%x device=%x software=%d more=%d outputs=%d umd=%d %s\n", made,
           (unsigned)d.VendorId, (unsigned)d.DeviceId, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, more, outputs,
           umd_ok, name);
    if (a)
        IDXGIAdapter1_Release(a);
    if (f)
        IDXGIFactory1_Release(f);
}
