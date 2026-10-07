/* Winsock: TCP echo over loopback with a server thread, UDP, name lookup, select. */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static DWORD WINAPI server(LPVOID arg)
{
    SOCKET listener = (SOCKET)arg;
    SOCKET c = accept(listener, NULL, NULL);
    char buf[128];
    int n = recv(c, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = 0;
        char reply[160];
        int k = snprintf(reply, sizeof(reply), "echo: %s", buf);
        send(c, reply, k, 0);
    }
    shutdown(c, SD_SEND);
    closesocket(c);
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        puts("WSAStartup failed");
        return 1;
    }
    printf("winsock %d.%d %s\n", LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion), wsa.szDescription);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 4)) {
        printf("listen failed %d\n", WSAGetLastError());
        return 1;
    }
    HANDLE t = CreateThread(NULL, 0, server, (LPVOID)listener, 0, NULL);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(c, (struct sockaddr *)&addr, sizeof(addr))) {
        printf("connect failed %d\n", WSAGetLastError());
        return 1;
    }
    send(c, "ping", 4, 0);
    char buf[256];
    int total = 0, n;
    while ((n = recv(c, buf + total, sizeof(buf) - 1 - total, 0)) > 0)
        total += n;
    buf[total] = 0;
    printf("tcp: %s\n", buf);
    closesocket(c);
    WaitForSingleObject(t, INFINITE);
    closesocket(listener);

    SOCKET u1 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), u2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in ua = addr;
    ua.sin_port = htons(9001);
    bind(u2, (struct sockaddr *)&ua, sizeof(ua));
    sendto(u1, "datagram", 8, 0, (struct sockaddr *)&ua, sizeof(ua));
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    n = recvfrom(u2, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
    buf[n > 0 ? n : 0] = 0;
    printf("udp: %s from %s\n", buf, inet_ntoa(from.sin_addr));
    closesocket(u1);
    closesocket(u2);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("localhost", "http", &hints, &res) == 0) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        printf("getaddrinfo: %s port %d\n", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
        freeaddrinfo(res);
    }

    SOCKET nb = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long on = 1;
    ioctlsocket(nb, FIONBIO, &on);
    struct sockaddr_in closed = addr;
    closed.sin_port = htons(9002);
    int r = connect(nb, (struct sockaddr *)&closed, sizeof(closed));
    int pending = r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(nb, &w);
    FD_SET(nb, &e);
    struct timeval tv = {2, 0};
    int ready = select(0, NULL, &w, &e, &tv);
    int err = 0, len = sizeof(err);
    getsockopt(nb, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    printf("refused: pending=%d ready=%d error=%d\n", pending, ready, err);
    closesocket(nb);

    if (argc > 2) {
        hints.ai_family = AF_INET;
        if (getaddrinfo(argv[1], argv[2], &hints, &res) == 0) {
            SOCKET h = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (connect(h, res->ai_addr, (int)res->ai_addrlen) == 0) {
                char req[256];
                int k = snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", argv[1]);
                send(h, req, k, 0);
                total = 0;
                while ((n = recv(h, buf + total, sizeof(buf) - 1 - total, 0)) > 0 && total < 200)
                    total += n;
                buf[total] = 0;
                char *eol = strchr(buf, '\r');
                if (eol)
                    *eol = 0;
                printf("http: %s\n", buf);
            } else {
                printf("http: connect failed %d\n", WSAGetLastError());
            }
            closesocket(h);
            freeaddrinfo(res);
        }
    }
    WSACleanup();
    return 0;
}
