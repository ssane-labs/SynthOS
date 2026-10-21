/* Times the things typical programs do: console output, C and Win32 file I/O, the heap and API calls.
   usage: winbench [directory] [report-file] */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD start_ms;
static DWORD total_ms;
static FILE *report;

static void begin(void)
{
    start_ms = GetTickCount();
}

static void end(const char *name, double amount, const char *unit)
{
    DWORD ms = GetTickCount() - start_ms;
    total_ms += ms;
    double rate = amount * 1000.0 / (ms ? ms : 1);
    fprintf(report, "%-22s %6lu ms  %10.0f %s/s\n", name, (unsigned long)ms, rate, unit);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    report = argc > 2 ? fopen(argv[2], "w") : stderr;
    if (!report)
        report = stderr;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\winbench.tmp", dir);
    static char block[64 * 1024];
    memset(block, 'x', sizeof(block));

    begin();
    for (int i = 0; i < 3000; i++)
        printf("line %d of console output from a Windows program\n", i);
    fflush(stdout);
    end("printf lines", 3000, "lines");

    begin();
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < 64; i++)
        fwrite(block, 1, sizeof(block), f);
    fclose(f);
    end("fwrite 4 MB", 4096, "KB");

    begin();
    f = fopen(path, "rb");
    long got = 0;
    for (int c; (c = fgetc(f)) != EOF;)
        got += c == 'x';
    fclose(f);
    end("fgetc 4 MB", 4096, "KB");

    begin();
    HANDLE h = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n, sum = 0;
    for (int round = 0; round < 4; round++) {
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        while (ReadFile(h, block, 4096, &n, NULL) && n)
            sum += n;
    }
    CloseHandle(h);
    end("ReadFile 4K x 4096", sum / 1024.0, "KB");

    begin();
    f = fopen(path, "w");
    for (int i = 0; i < 20000; i++)
        fprintf(f, "%d,%s,%.2f\n", i, "record", i * 0.5);
    fclose(f);
    end("fprintf 20000 rows", 20000, "rows");
    DeleteFileA(path);

    begin();
    void *ptrs[1000];
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 1000; i++)
            ptrs[i] = malloc(16 + (i * 37) % 500);
        for (int i = 0; i < 1000; i++)
            free(ptrs[i]);
    }
    end("malloc/free 200000", 200000, "pairs");

    begin();
    HANDLE heap = GetProcessHeap();
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 1000; i++)
            ptrs[i] = HeapAlloc(heap, 0, 32 + i % 200);
        for (int i = 999; i >= 0; i--)
            HeapFree(heap, 0, ptrs[i]);
    }
    end("HeapAlloc/Free 200000", 200000, "pairs");

    begin();
    char buf[128];
    for (int i = 0; i < 100000; i++)
        sprintf(buf, "%s %d %x %5.1f", "item", i, i * 3, i / 7.0);
    end("sprintf 100000", 100000, "calls");

    begin();
    DWORD acc = 0;
    for (int i = 0; i < 300000; i++)
        acc += GetCurrentThreadId() + GetLastError();
    end("API calls 600000", 600000, "calls");

    begin();
    int files = 0;
    for (int round = 0; round < 50; round++) {
        WIN32_FIND_DATAA fd;
        char pattern[MAX_PATH];
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        HANDLE fh = FindFirstFileA(pattern, &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do
                files++;
            while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
    }
    end("FindFirstFile x 50", files, "entries");

    /* an installer unpacking into one folder: every name is looked up without case before it is made */
    char folder[MAX_PATH];
    snprintf(folder, sizeof(folder), "%s\\winbench.dir", dir);
    CreateDirectoryA(folder, NULL);
    begin();
    for (int i = 0; i < 1000; i++) {
        char name[MAX_PATH];
        snprintf(name, sizeof(name), "%s\\Package_File_%04d.Dat", folder, i);
        HANDLE fh = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE)
            CloseHandle(fh);
    }
    end("create 1000 files", 1000, "files");
    for (int i = 0; i < 1000; i++) {
        char name[MAX_PATH];
        snprintf(name, sizeof(name), "%s\\package_file_%04d.dat", folder, i);
        DeleteFileA(name);
    }
    RemoveDirectoryA(folder);

    fprintf(report, "total %lu ms (check %ld %lu %lu)\n", (unsigned long)total_ms, got & 1, (unsigned long)(sum & 1),
            (unsigned long)(acc & 0));
    return 0;
}
