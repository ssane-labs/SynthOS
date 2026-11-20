#include <winsock2.h>
#include <ws2tcpip.h>
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

static LRESULT CALLBACK class_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

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
    if (argc > 1 && !strcmp(argv[1], "echo")) {
        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            printf("echo %s", line);
            fflush(stdout);
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "raw")) { /* hello64 raw: a window in front counts the mouse's raw motion */
        WNDCLASSA rc = {0};
        rc.lpfnWndProc = DefWindowProcA;
        rc.lpszClassName = "HelloRaw";
        RegisterClassA(&rc);
        HWND w = CreateWindowExA(0, "HelloRaw", "raw input", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 300, 200,
                                 NULL, NULL, NULL, NULL);
        RAWINPUTDEVICE d = {1, 2, 0, NULL};
        RegisterRawInputDevices(&d, 1, sizeof(d));
        long n = 0, sx = 0, sy = 0;
        DWORD end = GetTickCount() + 8000;
        while (GetTickCount() < end) {
            MSG m;
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
                RAWINPUT ri;
                UINT sz = sizeof(ri);
                if (m.message == WM_INPUT &&
                    GetRawInputData((HRAWINPUT)m.lParam, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1)
                    n++, sx += ri.data.mouse.lLastX, sy += ri.data.mouse.lLastY;
                DispatchMessageA(&m);
            }
            Sleep(10);
        }
        printf("raw motion: %ld messages, %ld %ld\n", n, sx, sy);
        DestroyWindow(w);
        return 0;
    }
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

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo("localhost", "80", &hints, &res) == 0) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        printf("sockets: %s %s port %d, ", wsa.szDescription, inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
        freeaddrinfo(res);
    }
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
    fd_set w;
    FD_ZERO(&w);
    FD_SET(u, &w);
    struct timeval tv = {1, 0};
    printf("select %d set %d\n", select(0, NULL, &w, NULL, &tv), FD_ISSET(u, &w) ? 1 : 0);
    closesocket(u);

    WNDCLASSEXA wc = {sizeof(wc)};
    wc.lpfnWndProc = class_proc;
    wc.cbWndExtra = 16;
    wc.lpszClassName = "Hello64";
    WNDCLASSEXA back = {sizeof(back)};
    if (RegisterClassExA(&wc) && GetClassInfoExA(NULL, "Hello64", &back))
        printf("window class: proc %s, extra %d\n", back.lpfnWndProc == class_proc ? "same" : "different",
               back.cbWndExtra);

    /* raw input: the mouse registered for a window (in the background too), a move and a click sent with SendInput
       come back as WM_INPUT, the release through GetRawInputBuffer */
    HWND rw = CreateWindowExA(0, "Hello64", "raw", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    RAWINPUTDEVICE rid = {1, 2, RIDEV_INPUTSINK, rw};
    INPUT in[2] = {{0}};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dx = 7, in[0].mi.dy = -3, in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    BOOL reg = RegisterRawInputDevices(&rid, 1, sizeof(rid));
    UINT sent = SendInput(2, in, sizeof(INPUT));
    MSG msg;
    int got = 0, down = 0;
    long sx = 0, sy = 0;
    while (PeekMessageA(&msg, NULL, WM_INPUT, WM_INPUT, PM_REMOVE)) {
        RAWINPUT ri;
        UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && msg.hwnd == rw) {
            got++;
            sx += ri.data.mouse.lLastX, sy += ri.data.mouse.lLastY;
            down |= ri.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN;
        }
    }
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in[1], sizeof(INPUT));
    RAWINPUT rbuf[4];
    UINT cap = sizeof(rbuf), n = GetRawInputBuffer(rbuf, &cap, sizeof(RAWINPUTHEADER));
    printf("raw input: registered %d, sent %u, messages %d, moved %ld %ld, button %d, buffer %u up %d\n", reg, sent,
           got, sx, sy, down != 0, n, n == 1 && (rbuf[0].data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) != 0);
    DestroyWindow(rw);
    return 7;
}
