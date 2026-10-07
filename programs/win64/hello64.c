#include <windows.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static __thread int counter = 5;
static jmp_buf jump;
static volatile LONG total;

static int by_value(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static DWORD WINAPI worker(LPVOID param)
{
    for (int i = 0; i < 1000; i++)
        InterlockedIncrement(&total);
    counter += (int)(intptr_t)param;
    return (DWORD)counter;
}

static void bounce(int depth)
{
    if (depth == 3)
        longjmp(jump, 42);
    bounce(depth + 1);
}

int main(int argc, char **argv)
{
    printf("hello from 64-bit Windows code, %d args, argv[0] ends with %s\n", argc,
           strrchr(argv[0], '\\') ? strrchr(argv[0], '\\') + 1 : argv[0]);
    printf("pointer size %zu, long long %lld, hex %llx\n", sizeof(void *), 123456789012345LL, 0xfedcba9876ULL);
    printf("math %.4f %.4f %.3e\n", sqrt(2.0), sin(1.0), pow(10.0, 20.5));

    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%05d-%c", "fmt", 42, 'x');
    printf("snprintf [%s]\n", buf);
    int x = 0;
    char word[16];
    if (sscanf("17 apples", "%d %15s", &x, word) == 2)
        printf("sscanf %d %s\n", x, word);
    char *end;
    long v = strtol("0x7fz", &end, 16);
    printf("strtol %ld rest '%s'\n", v, end);
    printf("strtod %.2f\n", strtod("3.25e2", NULL));

    int numbers[] = {9, 3, 7, 1, 8, 2};
    qsort(numbers, 6, sizeof(int), by_value);
    printf("qsort");
    for (int i = 0; i < 6; i++)
        printf(" %d", numbers[i]);
    printf("\n");

    int r = setjmp(jump);
    if (r == 0)
        bounce(0);
    printf("longjmp returned %d\n", r);

    char *heap = malloc(1 << 20);
    memset(heap, 'a', 1 << 20);
    heap[(1 << 20) - 1] = 0;
    printf("malloc 1 MB, strlen %zu, high bits %s\n", strlen(heap), ((uintptr_t)heap >> 32) ? "set" : "clear");
    free(heap);

    FILE *f = fopen("hello64.txt", "w");
    fprintf(f, "line %d\n%s\n", 1, "two");
    fclose(f);
    f = fopen("hello64.txt", "r");
    char line[32];
    fgets(line, sizeof(line), f);
    fclose(f);
    remove("hello64.txt");
    printf("file read back: %s", line);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("arch %u, page %lu, cpus %lu\n", si.wProcessorArchitecture, si.dwPageSize, si.dwNumberOfProcessors);

    HANDLE threads[2];
    for (int i = 0; i < 2; i++)
        threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)(i + 1), 0, NULL);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    DWORD c0, c1;
    GetExitCodeThread(threads[0], &c0);
    GetExitCodeThread(threads[1], &c1);
    printf("threads %ld, tls %d %lu %lu\n", total, counter, c0, c1);

    time_t now = time(NULL);
    printf("time is %s\n", now > 1000000000 ? "plausible" : "wrong");
    printf("env SystemRoot=%s\n", getenv("SystemRoot"));
    return 7;
}
