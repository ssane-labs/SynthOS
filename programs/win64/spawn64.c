#include <stdio.h>
#include <string.h>
#include <windows.h>

/* 64-bit programs that start each other run side by side (Steam and its CEF helpers talk while both run) */
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {
        HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, "spawn64-go");
        HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "Local\\spawn64-map");
        char *shared = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 4096) : NULL;
        if (!go || WaitForSingleObject(go, 5000) != WAIT_OBJECT_0)
            return 9;
        if (shared && !strcmp(shared, "ping"))
            strcpy(shared, "pong");
        return 5;
    }
    HANDLE go = CreateEventA(NULL, TRUE, FALSE, "spawn64-go");
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "Local\\spawn64-map");
    char *shared = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 4096);
    strcpy(shared, "ping");
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "\"%s\" child", argv[0]);
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        SetEvent(go);
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    HMODULE dll32 = LoadLibraryA("greet.dll");
    DWORD dll_error = GetLastError();
    CreateDirectoryA("C:\\Temp", NULL);
    HANDLE f = CreateFileA("C:\\Temp\\ov64.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done = 0;
    WriteFile(f, "0123456789", 10, &done, NULL);
    OVERLAPPED ov = {0};
    ov.Offset = 6;
    char buf[8] = "";
    BOOL rd = ReadFile(f, buf, 3, NULL, &ov);
    DWORD got = 0;
    GetOverlappedResult(f, &ov, &got, TRUE);
    CloseHandle(f);
    DeleteFileA("C:\\Temp\\ov64.bin");
    printf("spawn64 child=%lu map=%s dll32=%d/%lu overlapped=%d %lu %.3s\n", (unsigned long)code, shared,
           dll32 != NULL, (unsigned long)dll_error, rd, (unsigned long)got, buf);
    return 0;
}
