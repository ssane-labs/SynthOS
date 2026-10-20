#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <shellapi.h>

/* What installers such as NSIS rely on: INI files, named objects, SearchPath, SHFileOperation, polling a child. */

static void ini_test(void)
{
    const char *ini = "C:\\Temp\\wi\\setup.ini";
    WritePrivateProfileStringA("Main", "Name", "Steam", ini);
    WritePrivateProfileStringA("Main", "Count", "42", ini);
    WritePrivateProfileStringA("Other", "Key", "x", ini);
    WritePrivateProfileStringA("Main", "Name", "Valve", ini);
    char buf[64], keys[64];
    GetPrivateProfileStringA("main", "NAME", "none", buf, sizeof(buf), ini);
    UINT count = GetPrivateProfileIntA("Main", "Count", 0, ini);
    WritePrivateProfileStringA("Other", NULL, NULL, ini);
    DWORD n = GetPrivateProfileStringA("Main", NULL, "", keys, sizeof(keys), ini);
    WCHAR wbuf[16];
    GetPrivateProfileStringW(L"Other", L"Key", L"gone", wbuf, 16, L"C:\\Temp\\wi\\setup.ini");
    printf("ini name=%s count=%u keys=%s,%s/%lu other=%ls\n", buf, count, keys, keys + strlen(keys) + 1,
           (unsigned long)n, wbuf);
}

static void named_test(void)
{
    SetLastError(ERROR_ALREADY_EXISTS);
    HANDLE a = CreateMutexW(NULL, FALSE, L"Global\\wininst-mutex");
    DWORD first = GetLastError();
    HANDLE b = CreateMutexW(NULL, FALSE, L"wininst-mutex");
    DWORD second = GetLastError();
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, "wininst-event");
    HANDLE o = OpenEventA(EVENT_ALL_ACCESS, FALSE, "wininst-event");
    SetEvent(o);
    DWORD wait = WaitForSingleObject(e, 0);
    HANDLE missing = OpenMutexA(SYNCHRONIZE, FALSE, "wininst-none");
    DWORD missing_error = GetLastError();
    HANDLE wrong = CreateEventA(NULL, FALSE, FALSE, "wininst-mutex");
    printf("named first=%lu second=%lu same=%d wait=%lu missing=%d/%lu wrong=%d\n", (unsigned long)first,
           (unsigned long)second, a == b, (unsigned long)wait, missing != NULL, (unsigned long)missing_error,
           wrong != NULL);
    CloseHandle(b);
    CloseHandle(a);
    CloseHandle(o);
    CloseHandle(e);
    SetLastError(0);
    a = CreateMutexW(NULL, TRUE, L"wininst-mutex");
    printf("named again=%lu\n", (unsigned long)GetLastError());
    CloseHandle(a);
}

static void search_test(void)
{
    char path[MAX_PATH], *part = NULL;
    DWORD n = SearchPathA("C:\\Temp\\wi", "setup", ".ini", sizeof(path), path, &part);
    char found[MAX_PATH];
    strcpy(found, part ? part : "-");
    DWORD sys = SearchPathA(NULL, "kernel32.dll", NULL, sizeof(path), path, NULL);
    DWORD none = SearchPathA("C:\\Temp\\wi", "nothing.txt", NULL, sizeof(path), path, NULL);
    printf("search found=%d part=%s system=%d none=%lu\n", n > 0, found, sys > 0, (unsigned long)none);
}

static void fileop_test(void)
{
    CreateDirectoryA("C:\\Temp\\wi\\src", NULL);
    CreateDirectoryA("C:\\Temp\\wi\\src\\sub", NULL);
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "C:\\Temp\\wi\\src\\%c.txt", 'a' + i);
        FILE *f = fopen(name, "w");
        fputs("data", f);
        fclose(f);
    }
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_COPY;
    op.pFrom = L"C:\\Temp\\wi\\src\\*.txt\0";
    op.pTo = L"C:\\Temp\\wi\\dst\0";
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_SILENT;
    int copy = SHFileOperationW(&op);
    BOOL copied = GetFileAttributesA("C:\\Temp\\wi\\dst\\b.txt") != INVALID_FILE_ATTRIBUTES;
    op.wFunc = FO_DELETE;
    op.pFrom = L"C:\\Temp\\wi\\src\0C:\\Temp\\wi\\dst\0";
    op.pTo = NULL;
    int del = SHFileOperationW(&op);
    BOOL gone = GetFileAttributesA("C:\\Temp\\wi\\src") == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesA("C:\\Temp\\wi\\dst") == INVALID_FILE_ATTRIBUTES;
    FILE *f = fopen("C:\\Temp\\wi\\later.txt", "w");
    fclose(f);
    BOOL later = MoveFileExA("C:\\Temp\\wi\\later.txt", NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    printf("fileop copy=%d copied=%d delete=%d gone=%d reboot-delete=%d\n", copy, copied, del, gone, later);
}

/* nsExec waits for a program like this: one thread, Sleep between looks at the child */
static void poll_test(const char *self)
{
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "\"%s\" child", self);
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("poll create failed %lu\n", (unsigned long)GetLastError());
        return;
    }
    int polls = 0;
    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && polls < 500) {
        Sleep(10);
        polls++;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("poll child exit=%lu done=%d\n", (unsigned long)code, polls < 500);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {
        Sleep(30);
        return 7;
    }
    CreateDirectoryA("C:\\Temp", NULL);
    CreateDirectoryA("C:\\Temp\\wi", NULL);
    ini_test();
    named_test();
    search_test();
    fileop_test();
    poll_test(argv[0]);
    DeleteFileA("C:\\Temp\\wi\\setup.ini");
    RemoveDirectoryA("C:\\Temp\\wi");
    return 0;
}
