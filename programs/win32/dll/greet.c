#include <stdio.h>
#include <windows.h>

static int attached;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        attached = 1;
    return TRUE;
}

__declspec(dllexport) int greet_add(int a, int b)
{
    return a + b + (attached ? 0 : 1000);
}

__declspec(dllexport) const char *greet_text(void)
{
    return "hello from greet.dll";
}

__declspec(dllexport) int greet_counter = 41;
