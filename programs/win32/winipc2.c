/* A parent hands each of two children one end of a pipe with DuplicateHandle; the children then talk directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static HANDLE port;

static BOOL wait_io(DWORD *bytes)
{
    ULONG_PTR key = 0;
    OVERLAPPED *done = NULL;
    return GetQueuedCompletionStatus(port, bytes, &key, &done, 10000) && done;
}

static BOOL transfer(HANDLE pipe, char *buf, DWORD size, DWORD *n, BOOL write)
{
    OVERLAPPED ov = {0};
    BOOL ok = write ? WriteFile(pipe, buf, size, n, &ov) : ReadFile(pipe, buf, size, n, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        return FALSE;
    return wait_io(n);
}

static int child(const char *role, HANDLE control)
{
    char line[32] = {0};
    DWORD n = 0;
    if (!ReadFile(control, line, sizeof(line) - 1, &n, NULL) || !n)
        return 10;
    HANDLE pipe = (HANDLE)(uintptr_t)strtoul(line, NULL, 10);
    port = CreateIoCompletionPort(pipe, NULL, 1, 1);
    char buf[64] = {0};
    if (!strcmp(role, "a")) {
        if (!transfer(pipe, "ping", 4, &n, TRUE))
            return 11;
        if (!transfer(pipe, buf, sizeof(buf) - 1, &n, FALSE))
            return 12;
        return strncmp(buf, "pong", 4) ? 13 : 0;
    }
    if (!transfer(pipe, buf, sizeof(buf) - 1, &n, FALSE))
        return 21;
    if (strncmp(buf, "ping", 4))
        return 22;
    return transfer(pipe, "pong", 4, &n, TRUE) ? 0 : 23;
}

static HANDLE spawn(const char *role, HANDLE *control_write, PROCESS_INFORMATION *pi)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE r, w;
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0);
    char self[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(cmd, sizeof(cmd), "\"%s\" %s %lu", self, role, (unsigned long)(uintptr_t)r);
    STARTUPINFOA si = {sizeof(si)};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, pi))
        return NULL;
    CloseHandle(r);
    *control_write = w;
    return pi->hProcess;
}

int main(int argc, char **argv)
{
    if (argc > 2)
        return child(argv[1], (HANDLE)(uintptr_t)strtoul(argv[2], NULL, 10));
    char name[64];
    snprintf(name, sizeof(name), "\\\\.\\pipe\\winipc2.%lu", GetCurrentProcessId());
    HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 4096, 4096, 0,
                                     NULL);
    HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    ConnectNamedPipe(server, NULL);
    PROCESS_INFORMATION pa, pb;
    HANDLE ca, cb;
    HANDLE a = spawn("a", &ca, &pa), b = spawn("b", &cb, &pb);
    HANDLE in_a = NULL, in_b = NULL;
    BOOL da = DuplicateHandle(GetCurrentProcess(), server, a, &in_a, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    BOOL db = DuplicateHandle(GetCurrentProcess(), client, b, &in_b, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    char line[32];
    DWORD n;
    snprintf(line, sizeof(line), "%lu", (unsigned long)(uintptr_t)in_a);
    WriteFile(ca, line, (DWORD)strlen(line), &n, NULL);
    snprintf(line, sizeof(line), "%lu", (unsigned long)(uintptr_t)in_b);
    WriteFile(cb, line, (DWORD)strlen(line), &n, NULL);
    DWORD ra = 99, rb = 99;
    WaitForSingleObject(a, 20000);
    WaitForSingleObject(b, 20000);
    GetExitCodeProcess(a, &ra);
    GetExitCodeProcess(b, &rb);
    printf("duplicate %s/%s, child a exit %lu, child b exit %lu\n", da ? "ok" : "failed", db ? "ok" : "failed", ra, rb);
    return 0;
}
