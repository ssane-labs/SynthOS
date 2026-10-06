#include <stdio.h>
#include <windows.h>

__declspec(dllimport) int greet_add(int a, int b);
__declspec(dllimport) const char *greet_text(void);
__declspec(dllimport) int greet_counter;

typedef int (*add_fn)(int, int);

int main(void)
{
    printf("%s, 2+3=%d, counter=%d\n", greet_text(), greet_add(2, 3), greet_counter + 1);
    HMODULE mod = LoadLibraryA("greet.dll");
    add_fn add = (add_fn)GetProcAddress(mod, "greet_add");
    printf("dynamic: module=%s add=%d\n", mod ? "ok" : "missing", add ? add(40, 2) : -1);
    printf("missing proc: %p\n", (void *)GetProcAddress(mod, "nope"));
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    printf("kernel32 GetTickCount via GetProcAddress: %s\n", GetProcAddress(k32, "GetTickCount") ? "found" : "missing");
    return 0;
}
