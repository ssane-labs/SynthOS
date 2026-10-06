#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(void)
{
    CreateDirectoryA("C:\\Temp\\wtest", NULL);
    FILE *f = fopen("C:\\Temp\\wtest\\notes.txt", "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    for (int i = 1; i <= 3; i++)
        fprintf(f, "line %d\n", i);
    fclose(f);

    f = fopen("c:\\temp\\WTEST\\NOTES.TXT", "r");
    char line[64];
    int count = 0;
    while (fgets(line, sizeof(line), f))
        count++;
    fseek(f, 0, SEEK_END);
    printf("lines=%d size=%ld\n", count, ftell(f));
    fclose(f);

    HANDLE h = CreateFileA("C:\\Temp\\wtest\\raw.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    WriteFile(h, "SYNTH", 5, &written, NULL);
    CloseHandle(h);
    CopyFileA("C:\\Temp\\wtest\\raw.bin", "C:\\Temp\\wtest\\copy.bin", FALSE);

    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("C:\\Temp\\wtest\\*", &fd);
    int files = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            printf("found %s (%lu bytes)\n", fd.cFileName, fd.nFileSizeLow);
            files++;
        }
    } while (FindNextFileA(find, &fd));
    FindClose(find);

    char cwd[MAX_PATH];
    SetCurrentDirectoryA("C:\\Temp\\wtest");
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    printf("cwd=%s files=%d\n", cwd, files);
    printf("attr=%lx missing=%lx err=%lu\n", GetFileAttributesA("copy.bin"), GetFileAttributesA("nope.txt"),
           GetLastError());
    DeleteFileA("copy.bin");
    return 0;
}
