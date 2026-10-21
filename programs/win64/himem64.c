#include <stdio.h>
#include <string.h>
#include <windows.h>

/* Chromium's allocator reserves 16 GB pools above 4 GB and hands that memory to the API like any other */
int main(void)
{
    char *want = (char *)0x78dc00000000ull;
    char *pool = VirtualAlloc(want, 16ull << 30, MEM_RESERVE, PAGE_NOACCESS);
    char *p = pool + (2u << 20) - 0x3000;
    char *got = VirtualAlloc(p, 3u << 20, MEM_COMMIT, PAGE_READWRITE);
    int data = got == p;
    for (size_t i = 0; data && i < (3u << 20); i += 4096)
        p[i] = (char)(i >> 12);
    for (size_t i = 0; data && i < (3u << 20); i += 4096)
        data = p[i] == (char)(i >> 12) && p[i + 1] == 0;
    char *name = p + 0x2ff0;
    DWORD n = GetModuleFileNameA(NULL, name, MAX_PATH);
    char *copy = lstrcpyA(p + 0x100000, name);
    int api = n == strlen(name) && copy == p + 0x100000 && !strcmp(copy, name) && strstr(name, "himem64");
    CreateDirectoryA("C:\\Temp", NULL);
    HANDLE f = CreateFileA("C:\\Temp\\himem.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done = 0, back = 0;
    memset(p + 0x1000, 'x', 0x3000);
    WriteFile(f, p + 0x1000, 0x3000, &done, NULL);
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    ReadFile(f, p + 0x200000, 0x3000, &back, NULL);
    CloseHandle(f);
    DeleteFileA("C:\\Temp\\himem.bin");
    int io = done == 0x3000 && back == 0x3000 && p[0x200000] == 'x' && p[0x202fff] == 'x';
    MEMORY_BASIC_INFORMATION m;
    VirtualQuery(p + 5, &m, sizeof(m));
    int query = m.BaseAddress == p && m.AllocationBase == pool && m.State == MEM_COMMIT && m.RegionSize == (3u << 20);
    VirtualFree(p, 1u << 20, MEM_DECOMMIT);
    VirtualQuery(p, &m, sizeof(m));
    int decommit = m.State == MEM_RESERVE && m.RegionSize >= (1u << 20);
    char *any = VirtualAlloc(NULL, 0x7ffff0000ull, MEM_RESERVE, PAGE_NOACCESS);
    int placed = (ULONG_PTR)any >= (1ull << 32) && VirtualAlloc(any, 4096, MEM_COMMIT, PAGE_READWRITE) == any;
    if (placed)
        any[100] = 7;
    int freed = VirtualFree(pool, 0, MEM_RELEASE) && VirtualFree(any, 0, MEM_RELEASE) &&
                VirtualAlloc(want, 4096, MEM_RESERVE, PAGE_NOACCESS) == want;
    printf("himem pool=%d data=%d api=%d io=%d query=%d decommit=%d placed=%d freed=%d\n", pool == want, data, api,
           io, query, decommit, placed, freed);
    return 0;
}
