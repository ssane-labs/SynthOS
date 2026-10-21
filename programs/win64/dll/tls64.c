#include <windows.h>
#include <intrin.h>

/* implicit TLS in a 64-bit DLL: its own slot in every thread, TLS callbacks before DllMain */
extern ULONG _tls_index;
extern char _tls_start;
__attribute__((section(".tls$X"))) int tls_value = 1234;
static int attached, before_main, threads;

static void NTAPI tls_callback(PVOID inst, DWORD reason, PVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        before_main = !attached;
    if (reason == DLL_THREAD_ATTACH)
        threads++;
}

__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK tls64_callback = tls_callback;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        attached = 1;
    return TRUE;
}

static int *slot(void)
{
    char **blocks = (char **)__readgsqword(0x58);
    return (int *)(blocks[_tls_index] + ((char *)&tls_value - &_tls_start));
}

__declspec(dllexport) int tls64_get(int add)
{
    int *v = slot();
    *v += add;
    return *v;
}

__declspec(dllexport) int tls64_flags(void)
{
    return attached + before_main * 2 + (threads > 0) * 4;
}
