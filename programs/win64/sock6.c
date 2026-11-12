#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winternl.h>

typedef NTSTATUS(NTAPI *create_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                   ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(NTAPI *ioctl_fn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);

/* mio (tokio) waits for sockets through \Device\Afd and IOCTL_AFD_POLL completed on a port */
static int afd_poll(SOCKET s)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    create_fn create = (create_fn)(void *)GetProcAddress(nt, "NtCreateFile");
    ioctl_fn ioctl = (ioctl_fn)(void *)GetProcAddress(nt, "NtDeviceIoControlFile");
    UNICODE_STRING name = {30, 32, L"\\Device\\Afd\\Mio"};
    OBJECT_ATTRIBUTES oa = {.Length = sizeof(oa), .ObjectName = &name};
    IO_STATUS_BLOCK iosb;
    HANDLE afd = NULL;
    if (!create || !ioctl || create(&afd, SYNCHRONIZE, &oa, &iosb, NULL, 0, 3, 1, 0, NULL, 0) != 0)
        return 0;
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    CreateIoCompletionPort(afd, port, 7, 0);
    struct {
        LONGLONG timeout;
        ULONG count, exclusive;
        struct {
            ULONGLONG socket;
            ULONG events, status;
        } h[1];
    } info = {INT64_MAX, 1, 0, {{(ULONGLONG)s, 1, 0}}};
    IO_STATUS_BLOCK pio;
    NTSTATUS st = ioctl(afd, NULL, NULL, &pio, &pio, 0x12024, &info, sizeof(info), &info, sizeof(info));
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = NULL;
    BOOL got = GetQueuedCompletionStatus(port, &bytes, &key, &ov, 2000);
    CloseHandle(port);
    CloseHandle(afd);
    return st == 0x103 && got && key == 7 && ov == (OVERLAPPED *)&pio && (info.h[0].events & 1) ? 1 : 2;
}

/* Steam's interface talks to its client over an IPv6 loopback socket */
int main(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET srv = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in6 a = {0};
    a.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::1", &a.sin6_addr);
    int v6only = 0;
    setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
    int ok = bind(srv, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(srv, 1) == 0;
    int len = sizeof(a);
    getsockname(srv, (struct sockaddr *)&a, &len);
    SOCKET c = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    ok = ok && connect(c, (struct sockaddr *)&a, sizeof(a)) == 0;
    struct sockaddr_in6 peer;
    int plen = sizeof(peer);
    SOCKET s = accept(srv, (struct sockaddr *)&peer, &plen);
    char buf[8] = "", text[64] = "";
    send(c, "hi6", 3, 0);
    int afd = afd_poll(s);
    int n = recv(s, buf, sizeof(buf), 0);
    inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof(text));
    printf("sock6 ok=%d family=%d len=%d port=%d recv=%.*s peer=%s afd=%d\n", ok, a.sin6_family, len, ntohs(a.sin6_port) != 0,
           n > 0 ? n : 0, buf, text, afd);
    closesocket(s);
    closesocket(c);
    closesocket(srv);
    return 0;
}
