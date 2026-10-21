#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

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
    int n = recv(s, buf, sizeof(buf), 0);
    inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof(text));
    printf("sock6 ok=%d family=%d len=%d port=%d recv=%.*s peer=%s\n", ok, a.sin6_family, len, ntohs(a.sin6_port) != 0,
           n > 0 ? n : 0, buf, text);
    closesocket(s);
    closesocket(c);
    closesocket(srv);
    return 0;
}
