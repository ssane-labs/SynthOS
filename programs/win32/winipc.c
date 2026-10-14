/* Two processes talking over an inherited overlapped named pipe with I/O completion ports, like Chrome's Mojo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static HANDLE port;

static BOOL wait_io(DWORD *bytes, DWORD ms)
{
    ULONG_PTR key = 0;
    OVERLAPPED *done = NULL;
    return GetQueuedCompletionStatus(port, bytes, &key, &done, ms) && done;
}

static BOOL read_some(HANDLE pipe, char *buf, DWORD size, DWORD *got)
{
    OVERLAPPED ov = {0};
    if (ReadFile(pipe, buf, size, got, &ov))
        return wait_io(got, 5000);
    if (GetLastError() != ERROR_IO_PENDING) {
        printf("ReadFile failed %lu\n", GetLastError());
        return FALSE;
    }
    return wait_io(got, 5000);
}

static BOOL write_all(HANDLE pipe, const char *text)
{
    OVERLAPPED ov = {0};
    DWORD put = 0;
    if (!WriteFile(pipe, text, (DWORD)strlen(text), &put, &ov) && GetLastError() != ERROR_IO_PENDING)
        return FALSE;
    return wait_io(&put, 5000);
}

static int child(HANDLE pipe)
{
    port = CreateIoCompletionPort(pipe, NULL, 1, 1);
    char buf[64] = {0};
    DWORD got = 0;
    if (!read_some(pipe, buf, sizeof(buf) - 1, &got))
        return 2;
    buf[got] = 0;
    char reply[96];
    snprintf(reply, sizeof(reply), "child got '%s'", buf);
    return write_all(pipe, reply) ? 0 : 3;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "child"))
        return child((HANDLE)(uintptr_t)strtoul(argv[2], NULL, 10));

    char name[64];
    snprintf(name, sizeof(name), "\\\\.\\pipe\\winipc.%lu", GetCurrentProcessId());
    HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 4096, 4096, 5000, NULL);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    BOOL connected = ConnectNamedPipe(server, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
    printf("pipe %s, connect %s\n", server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE ? "ok" : "bad",
           connected ? "ok" : "bad");

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = malloc(size);
    InitializeProcThreadAttributeList(list, 1, 0, &size);
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &client, sizeof(client), NULL, NULL);
    STARTUPINFOEXA si = {{sizeof(si)}};
    si.lpAttributeList = list;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 64], self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(cmd, sizeof(cmd), "\"%s\" child %lu", self, (unsigned long)(uintptr_t)client);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(client);

    port = CreateIoCompletionPort(server, NULL, 7, 1);
    BOOL sent = write_all(server, "hello");
    char buf[96] = {0};
    DWORD got = 0;
    BOOL heard = read_some(server, buf, sizeof(buf) - 1, &got);
    buf[got] = 0;
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("sent %s, reply: %s, child exit %lu\n", sent ? "ok" : "failed", heard ? buf : "none", code);
    return 0;
}
