#include <synth/fmt.h>
#include <synth/error.h>
#include <synth/hal.h>
#include <synth/net.h>
#include <synth/string.h>

#include "win32.h"

#define SOCKET_BASE 0x1000u
#define INVALID_SOCKET (~0u)
#define SOCKET_ERROR (-1)
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xffff
#define FIONBIO 0x8004667eu
#define FIONREAD 0x4004667fu

enum {
    WSAEINTR = 10004,
    WSAEBADF = 10009,
    WSAEFAULT = 10014,
    WSAEINVAL = 10022,
    WSAEMFILE = 10024,
    WSAEWOULDBLOCK = 10035,
    WSAEINPROGRESS = 10036,
    WSAEALREADY = 10037,
    WSAENOTSOCK = 10038,
    WSAEMSGSIZE = 10040,
    WSAEPROTONOSUPPORT = 10043,
    WSAEOPNOTSUPP = 10045,
    WSAEAFNOSUPPORT = 10047,
    WSAEADDRINUSE = 10048,
    WSAENETUNREACH = 10051,
    WSAECONNABORTED = 10053,
    WSAECONNRESET = 10054,
    WSAENOBUFS = 10055,
    WSAEISCONN = 10056,
    WSAENOTCONN = 10057,
    WSAESHUTDOWN = 10058,
    WSAETIMEDOUT = 10060,
    WSAECONNREFUSED = 10061,
    WSAHOST_NOT_FOUND = 11001,
};

struct wsock {
    bool used, nonblocking;
    int id;
    int type;
    DWORD recv_timeout, send_timeout;
    HANDLE port;
    uintptr_t key;
    HANDLE event;
    long mask;
};

#define STATUS_PENDING 0x103u
#define STATUS_CANCELLED 0xc0000120u
#define STATUS_CONNECTION_RESET 0xc000020du
#define WSA_IO_PENDING 997
#define WAIT_TIMEOUT 258u
#define MAX_PENDING 64

struct pending {
    bool used;
    UINT s;
    char *buf;
    ULONG len;
    uint32_t *ov;
    struct sockaddr_in *from;
    int *fromlen;
};

struct sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

struct fd_set {
    UINT count;
    UINT sockets[64];
};

struct timeval {
    LONG sec, usec;
};

struct addrinfo {
    int flags, family, socktype, protocol;
    SIZE_T addrlen;
    char *canonname;
    struct sockaddr_in *addr;
    struct addrinfo *next;
};

struct addrinfo_w {
    int flags, family, socktype, protocol;
    SIZE_T addrlen;
    WCHAR *canonname;
    struct sockaddr_in *addr;
    struct addrinfo_w *next;
};

struct hostent {
    char *name;
    char **aliases;
    int16_t addrtype;
    int16_t length;
    char **addr_list;
};

static W32_STATE struct wsock socks[NET_MAX_SOCKETS];
static W32_STATE struct pending pending[MAX_PENDING];
static W32_STATE int started;
static W32_STATE char ntoa_buffer[16];
static W32_STATE struct hostent host;
static W32_STATE char host_name[64];
static W32_STATE uint32_t host_addr;
static W32_STATE char *host_addrs[2];
static W32_STATE char *host_aliases[1];

static int wsa_error(int err)
{
    switch (err) {
    case E_AGAIN: return WSAEWOULDBLOCK;
    case E_CONNREFUSED: return WSAECONNREFUSED;
    case E_CONNRESET: return WSAECONNRESET;
    case E_TIMEDOUT: return WSAETIMEDOUT;
    case E_NOTCONN: return WSAENOTCONN;
    case E_ADDRINUSE: return WSAEADDRINUSE;
    case E_UNREACH: return WSAENETUNREACH;
    case E_BADF: return WSAENOTSOCK;
    case E_MFILE: return WSAEMFILE;
    case E_NOMEM: return WSAENOBUFS;
    case E_PIPE: return WSAESHUTDOWN;
    case E_NOENT: return WSAHOST_NOT_FOUND;
    case E_RANGE: return WSAEMSGSIZE;
    case E_PERM: return 10013;
    case E_BUSY: return WSAEISCONN;
    default: return WSAEINVAL;
    }
}

static int fail(int err)
{
    w32_set_error((DWORD)wsa_error(err));
    return SOCKET_ERROR;
}

static struct wsock *sock_of(UINT s)
{
    if (s < SOCKET_BASE || s >= SOCKET_BASE + NET_MAX_SOCKETS || !socks[s - SOCKET_BASE].used) {
        w32_set_error(WSAENOTSOCK);
        return NULL;
    }
    return &socks[s - SOCKET_BASE];
}

static UINT handle_of(int id)
{
    return SOCKET_BASE + (UINT)id;
}

static void wait_step(void)
{
    net_poll();
    w32_block();
}

static uint32_t to_host(uint32_t be)
{
    return net_htonl(be);
}

static void fill_addr(struct sockaddr_in *out, int *len, uint32_t ip, uint16_t port)
{
    if (!out)
        return;
    if (len && *len < (int)sizeof(*out)) {
        *len = sizeof(*out);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = AF_INET;
    out->port = net_htons(port);
    out->addr = net_htonl(ip);
    if (len)
        *len = sizeof(*out);
}

static int WINAPI WSAStartup(WORD version, uint8_t *data)
{
    if (data) {
        memset(data, 0, 400);
        WORD v = (WORD)((version & 0xff) > 2 ? 0x0202 : version);
        memcpy(data, &v, 2);
        WORD high = 0x0202;
        memcpy(data + 2, &high, 2);
        str_copy((char *)data + 4, "SynthOS sockets", 257);
        str_copy((char *)data + 261, "Running", 129);
        uint16_t max_sockets = NET_MAX_SOCKETS, max_udp = 1472;
        memcpy(data + 390, &max_sockets, 2);
        memcpy(data + 392, &max_udp, 2);
    }
    started++;
    return 0;
}

static int WINAPI WSACleanup(void)
{
    if (started)
        started--;
    return 0;
}

static int WINAPI WSAGetLastError(void)
{
    return (int)w32_error();
}

static void WINAPI WSASetLastError(int e)
{
    w32_set_error((DWORD)e);
}

static UINT WINAPI socket(int family, int type, int protocol)
{
    UNUSED(protocol);
    if (family != AF_INET) {
        w32_set_error(WSAEAFNOSUPPORT);
        return INVALID_SOCKET;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        w32_set_error(WSAEPROTONOSUPPORT);
        return INVALID_SOCKET;
    }
    int id = net_socket(type == SOCK_STREAM ? NET_TCP : NET_UDP);
    if (id < 0) {
        fail(id);
        return INVALID_SOCKET;
    }
    socks[id] = (struct wsock){true, false, id, type, 0, 0, NULL, 0, NULL, 0};
    return handle_of(id);
}

static UINT WINAPI WSASocketA(int family, int type, int protocol, void *info, UINT group, DWORD flags)
{
    UNUSED(info);
    UNUSED(group);
    UNUSED(flags);
    return socket(family, type, protocol);
}

static int WINAPI closesocket(UINT s)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    net_close(w->id);
    w->used = false;
    return 0;
}

static int WINAPI bind(UINT s, const struct sockaddr_in *addr, int len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    if (!addr || len < (int)sizeof(*addr) || addr->family != AF_INET)
        return fail(E_INVAL);
    int err = net_bind(w->id, to_host(addr->addr), net_htons(addr->port));
    return err ? fail(err) : 0;
}

static int WINAPI listen(UINT s, int backlog)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    int err = net_listen(w->id, backlog);
    return err ? fail(err) : 0;
}

static bool expired(uint64_t start, DWORD timeout)
{
    return timeout && hal_ticks_ms() - start >= timeout;
}

static UINT WINAPI accept(UINT s, struct sockaddr_in *addr, int *len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return INVALID_SOCKET;
    for (;;) {
        uint32_t ip;
        uint16_t port;
        int c = net_accept(w->id, &ip, &port);
        if (c >= 0) {
            socks[c] = (struct wsock){true, false, c, SOCK_STREAM, w->recv_timeout, w->send_timeout, NULL, 0, NULL, 0};
            fill_addr(addr, len, ip, port);
            return handle_of(c);
        }
        if (c != E_AGAIN || w->nonblocking) {
            fail(c);
            return INVALID_SOCKET;
        }
        wait_step();
    }
}

static int WINAPI connect(UINT s, const struct sockaddr_in *addr, int len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    if (!addr || len < (int)sizeof(*addr) || addr->family != AF_INET)
        return fail(E_INVAL);
    int err = net_connect_start(w->id, to_host(addr->addr), net_htons(addr->port));
    if (w->type == SOCK_DGRAM)
        return err ? fail(err) : 0;
    if (err != E_AGAIN)
        return fail(err == E_BUSY ? E_BUSY : err);
    if (w->nonblocking) {
        w32_set_error(WSAEWOULDBLOCK);
        return SOCKET_ERROR;
    }
    uint64_t start = hal_ticks_ms();
    while (!net_connected(w->id) && !net_error(w->id)) {
        if (hal_ticks_ms() - start > 21000)
            return fail(E_TIMEDOUT);
        wait_step();
    }
    return net_error(w->id) ? fail(net_error(w->id)) : 0;
}

static int WINAPI send(UINT s, const char *buf, int len, int flags)
{
    UNUSED(flags);
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    if (len < 0)
        return fail(E_INVAL);
    int sent = 0;
    uint64_t start = hal_ticks_ms();
    while (sent < len) {
        long n = net_send(w->id, buf + sent, (size_t)(len - sent));
        if (n > 0) {
            sent += (int)n;
            continue;
        }
        if (n != E_AGAIN)
            return sent ? sent : fail((int)n);
        if (w->nonblocking)
            return sent ? sent : fail(E_AGAIN);
        if (expired(start, w->send_timeout))
            return sent ? sent : fail(E_TIMEDOUT);
        wait_step();
    }
    net_poll();
    return sent;
}

static int WINAPI recv(UINT s, char *buf, int len, int flags)
{
    UNUSED(flags);
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    uint64_t start = hal_ticks_ms();
    for (;;) {
        long n = net_recv(w->id, buf, (size_t)MAX(len, 0));
        if (n >= 0)
            return (int)n;
        if (n != E_AGAIN || w->nonblocking)
            return fail((int)n);
        if (expired(start, w->recv_timeout))
            return fail(E_TIMEDOUT);
        wait_step();
    }
}

static int WINAPI sendto(UINT s, const char *buf, int len, int flags, const struct sockaddr_in *to, int tolen)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    if (w->type == SOCK_STREAM || !to)
        return send(s, buf, len, flags);
    if (tolen < (int)sizeof(*to))
        return fail(E_INVAL);
    long n = net_sendto(w->id, buf, (size_t)MAX(len, 0), to_host(to->addr), net_htons(to->port));
    return n < 0 ? fail((int)n) : (int)n;
}

static int WINAPI recvfrom(UINT s, char *buf, int len, int flags, struct sockaddr_in *from, int *fromlen)
{
    UNUSED(flags);
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    uint64_t start = hal_ticks_ms();
    for (;;) {
        uint32_t ip = 0;
        uint16_t port = 0;
        long n = net_recvfrom(w->id, buf, (size_t)MAX(len, 0), &ip, &port);
        if (n >= 0) {
            fill_addr(from, fromlen, ip, port);
            return (int)n;
        }
        if (n != E_AGAIN || w->nonblocking)
            return fail((int)n);
        if (expired(start, w->recv_timeout))
            return fail(E_TIMEDOUT);
        wait_step();
    }
}

static int WINAPI shutdown(UINT s, int how)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    if (how != 0)
        net_shutdown(w->id);
    return 0;
}

static int WINAPI ioctlsocket(UINT s, LONG cmd, ULONG *arg)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    if ((uint32_t)cmd == FIONBIO) {
        w->nonblocking = arg && *arg;
        return 0;
    }
    if ((uint32_t)cmd == FIONREAD) {
        struct net_socket_info list[NET_MAX_SOCKETS];
        int n = net_sockets(list, NET_MAX_SOCKETS);
        *arg = 0;
        for (int i = 0; i < n; i++)
            if (list[i].id == w->id)
                *arg = (ULONG)list[i].queued_rx;
        return 0;
    }
    return fail(E_INVAL);
}

static int WINAPI setsockopt(UINT s, int level, int name, const char *value, int len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    DWORD v = 0;
    if (value && len >= 4)
        memcpy(&v, value, 4);
    else if (value && len >= 1)
        v = (uint8_t)value[0];
    if (level == SOL_SOCKET) {
        if (name == 0x1006)
            w->recv_timeout = v;
        else if (name == 0x1005)
            w->send_timeout = v;
        else if (name == 0x20)
            net_set_broadcast(w->id, v != 0);
    }
    return 0;
}

static int WINAPI getsockopt(UINT s, int level, int name, char *value, int *len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    DWORD v = 0;
    if (level == SOL_SOCKET && name == 0x1007)
        v = net_error(w->id) ? (DWORD)wsa_error(net_error(w->id)) : 0;
    else if (level == SOL_SOCKET && name == 0x1008)
        v = (DWORD)w->type;
    else if (level == SOL_SOCKET && name == 0x1006)
        v = w->recv_timeout;
    else if (level == SOL_SOCKET && name == 0x1005)
        v = w->send_timeout;
    else if (level == SOL_SOCKET && (name == 0x1001 || name == 0x1002))
        v = 65536;
    if (value && len && *len >= 4) {
        memcpy(value, &v, 4);
        *len = 4;
    }
    return 0;
}

static int WINAPI getsockname(UINT s, struct sockaddr_in *addr, int *len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    uint32_t ip;
    uint16_t port;
    int err = net_local(w->id, &ip, &port);
    if (err)
        return fail(err);
    fill_addr(addr, len, ip, port);
    return 0;
}

static int WINAPI getpeername(UINT s, struct sockaddr_in *addr, int *len)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    uint32_t ip;
    uint16_t port;
    int err = net_peer(w->id, &ip, &port);
    if (err)
        return fail(err);
    fill_addr(addr, len, ip, port);
    return 0;
}

static bool in_set(const struct fd_set *set, UINT s)
{
    for (UINT i = 0; set && i < MIN(set->count, 64u); i++)
        if (set->sockets[i] == s)
            return true;
    return false;
}

static int WINAPI __WSAFDIsSet(UINT s, struct fd_set *set)
{
    return in_set(set, s);
}

static int filter(struct fd_set *set, bool (*ready)(int))
{
    if (!set)
        return 0;
    UINT kept = 0;
    for (UINT i = 0; i < MIN(set->count, 64u); i++) {
        struct wsock *w = sock_of(set->sockets[i]);
        if (w && ready(w->id))
            set->sockets[kept++] = set->sockets[i];
    }
    set->count = kept;
    return (int)kept;
}

static int count_ready(const struct fd_set *set, bool (*ready)(int))
{
    int n = 0;
    for (UINT i = 0; set && i < MIN(set->count, 64u); i++) {
        UINT s = set->sockets[i];
        if (s >= SOCKET_BASE && s < SOCKET_BASE + NET_MAX_SOCKETS && socks[s - SOCKET_BASE].used &&
            ready(socks[s - SOCKET_BASE].id))
            n++;
    }
    return n;
}

static bool has_error(int id)
{
    return net_error(id) != 0;
}

static bool write_ready(int id)
{
    return net_writable(id) && !net_error(id);
}

static int WINAPI select(int nfds, struct fd_set *r, struct fd_set *wr, struct fd_set *ex, const struct timeval *t)
{
    UNUSED(nfds);
    uint64_t start = hal_ticks_ms();
    uint64_t timeout = t ? (uint64_t)t->sec * 1000 + (uint64_t)t->usec / 1000 : 0;
    for (;;) {
        net_poll();
        int n = count_ready(r, net_readable) + count_ready(wr, write_ready) + count_ready(ex, has_error);
        if (n || (t && hal_ticks_ms() - start >= timeout)) {
            filter(r, net_readable);
            filter(wr, write_ready);
            filter(ex, has_error);
            return n;
        }
        w32_block();
    }
}

enum { FD_READ = 1, FD_WRITE = 2, FD_OOB = 4, FD_ACCEPT = 8, FD_CONNECT = 16, FD_CLOSE = 32 };

static long sock_events(struct wsock *w)
{
    long ev = 0;
    if (net_readable(w->id))
        ev |= FD_READ | FD_ACCEPT;
    if (write_ready(w->id))
        ev |= FD_WRITE | FD_CONNECT;
    if (net_error(w->id))
        ev |= FD_CLOSE | FD_READ;
    return ev & w->mask;
}

static HANDLE WINAPI WSACreateEvent(void)
{
    HANDLE h = w32_handle_new(WH_EVENT);
    struct w32_object *o = w32_handle_get(h);
    if (o)
        o->manual = true;
    return h;
}

static BOOL WINAPI WSASetEvent(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_EVENT)
        return FALSE;
    o->value = 1;
    return TRUE;
}

static BOOL WINAPI WSAResetEvent(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_EVENT)
        return FALSE;
    o->value = 0;
    return TRUE;
}

static BOOL WINAPI WSACloseEvent(HANDLE h)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (socks[i].used && socks[i].event == h)
            socks[i].event = NULL;
    return w32_handle_close(h);
}

static int WINAPI WSAEventSelect(UINT s, HANDLE event, long mask)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return SOCKET_ERROR;
    w->event = event;
    w->mask = event ? mask : 0;
    if (event)
        w->nonblocking = true;
    return 0;
}

struct network_events {
    long events;
    int errors[10];
};

static int WINAPI WSAEnumNetworkEvents(UINT s, HANDLE event, struct network_events *out)
{
    struct wsock *w = sock_of(s);
    if (!w || !out)
        return SOCKET_ERROR;
    net_poll();
    memset(out, 0, sizeof(*out));
    out->events = sock_events(w);
    if (net_error(w->id) && (out->events & FD_CLOSE))
        out->errors[5] = WSAECONNRESET;
    if (event)
        WSAResetEvent(event);
    return 0;
}

static DWORD WINAPI WSAWaitForMultipleEvents(DWORD count, const HANDLE *events, BOOL all, DWORD timeout, BOOL alertable)
{
    UNUSED(alertable);
    uint64_t start = hal_ticks_ms();
    for (;;) {
        net_poll();
        for (int i = 0; i < NET_MAX_SOCKETS; i++)
            if (socks[i].used && socks[i].event && sock_events(&socks[i]))
                WSASetEvent(socks[i].event);
        DWORD set = 0, first = WAIT_TIMEOUT;
        for (DWORD i = 0; i < count; i++) {
            struct w32_object *o = w32_handle_get(events[i]);
            if (o && o->type == WH_EVENT && o->value) {
                set++;
                if (first == WAIT_TIMEOUT)
                    first = i;
            }
        }
        if (set && (!all || set == count))
            return first;
        if (timeout != 0xffffffffu && hal_ticks_ms() - start >= timeout)
            return WAIT_TIMEOUT;
        w32_block();
    }
}

struct pollfd {
    UINT fd;
    int16_t events, revents;
};

static int WINAPI WSAPoll(struct pollfd *fds, ULONG count, int timeout)
{
    uint64_t start = hal_ticks_ms();
    for (;;) {
        net_poll();
        int ready = 0;
        for (ULONG i = 0; i < count; i++) {
            fds[i].revents = 0;
            struct wsock *w = sock_of(fds[i].fd);
            if (!w) {
                fds[i].revents = 0x4000;
                ready++;
                continue;
            }
            if ((fds[i].events & 0x300) && net_readable(w->id))
                fds[i].revents |= (int16_t)(fds[i].events & 0x300);
            if ((fds[i].events & 0x10) && write_ready(w->id))
                fds[i].revents |= 0x10;
            if (net_error(w->id))
                fds[i].revents |= 1;
            if (fds[i].revents)
                ready++;
        }
        if (ready || (timeout >= 0 && hal_ticks_ms() - start >= (uint64_t)timeout))
            return ready;
        w32_block();
    }
}

static uint32_t WINAPI ws_htonl(uint32_t v)
{
    return net_htonl(v);
}

static uint16_t WINAPI ws_htons(uint16_t v)
{
    return net_htons(v);
}

static uint32_t WINAPI inet_addr(const char *s)
{
    uint32_t ip;
    return s && net_parse_ip(s, &ip) ? net_htonl(ip) : 0xffffffffu;
}

static char *WINAPI inet_ntoa(uint32_t be)
{
    net_format_ip(net_htonl(be), ntoa_buffer, sizeof(ntoa_buffer));
    return ntoa_buffer;
}

static int WINAPI inet_pton(int family, const char *s, void *out)
{
    uint32_t ip;
    if (family != AF_INET) {
        w32_set_error(WSAEAFNOSUPPORT);
        return -1;
    }
    if (!net_parse_ip(s, &ip))
        return 0;
    uint32_t be = net_htonl(ip);
    memcpy(out, &be, 4);
    return 1;
}

static const char *WINAPI inet_ntop(int family, const void *addr, char *out, SIZE_T cap)
{
    if (family != AF_INET) {
        w32_set_error(WSAEAFNOSUPPORT);
        return NULL;
    }
    uint32_t be;
    memcpy(&be, addr, 4);
    net_format_ip(net_htonl(be), out, cap);
    return out;
}

static int WINAPI gethostname(char *out, int len)
{
    if (len < 8)
        return fail(E_INVAL);
    str_copy(out, "synthos", (size_t)len);
    return 0;
}

static struct hostent *WINAPI gethostbyname(const char *name)
{
    uint32_t ip;
    int err = net_resolve(name, &ip, 5000);
    if (err) {
        fail(err);
        return NULL;
    }
    str_copy(host_name, name, sizeof(host_name));
    host_addr = net_htonl(ip);
    host_addrs[0] = (char *)&host_addr;
    host_addrs[1] = NULL;
    host_aliases[0] = NULL;
    host = (struct hostent){host_name, host_aliases, AF_INET, 4, host_addrs};
    return &host;
}

static struct hostent *WINAPI gethostbyaddr(const char *addr, int len, int type)
{
    UNUSED(type);
    if (len < 4)
        return NULL;
    memcpy(&host_addr, addr, 4);
    net_format_ip(net_htonl(host_addr), host_name, sizeof(host_name));
    host_addrs[0] = (char *)&host_addr;
    host_addrs[1] = NULL;
    host_aliases[0] = NULL;
    host = (struct hostent){host_name, host_aliases, AF_INET, 4, host_addrs};
    return &host;
}

static uint16_t service_port(const char *service)
{
    static const struct {
        const char *name;
        uint16_t port;
    } known[] = {{"http", 80}, {"https", 443}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25},
                 {"domain", 53}, {"pop3", 110}, {"imap", 143}, {"ntp", 123}};
    if (!service)
        return 0;
    if (*service >= '0' && *service <= '9') {
        uint32_t v = 0;
        while (*service >= '0' && *service <= '9')
            v = v * 10 + (uint32_t)(*service++ - '0');
        return (uint16_t)v;
    }
    for (size_t i = 0; i < ARRAY_SIZE(known); i++)
        if (!strcmp(known[i].name, service))
            return known[i].port;
    return 0;
}

static int WINAPI getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                              struct addrinfo **out)
{
    if (hints && hints->family != 0 && hints->family != AF_INET)
        return WSAEAFNOSUPPORT;
    uint32_t ip = 0;
    if (node) {
        int err = net_resolve(node, &ip, 5000);
        if (err)
            return wsa_error(err);
    } else if (!(hints && (hints->flags & 1))) {
        ip = 0x7f000001u;
    }
    struct addrinfo *a = w32_zalloc(sizeof(*a) + sizeof(struct sockaddr_in));
    if (!a)
        return 8;
    a->family = AF_INET;
    a->socktype = hints && hints->socktype ? hints->socktype : SOCK_STREAM;
    a->protocol = a->socktype == SOCK_DGRAM ? 17 : 6;
    a->addrlen = sizeof(struct sockaddr_in);
    a->addr = (struct sockaddr_in *)(a + 1);
    fill_addr(a->addr, NULL, ip, service_port(service));
    if (node && hints && (hints->flags & 2))
        a->canonname = w32_strdup(node);
    *out = a;
    return 0;
}

static void WINAPI freeaddrinfo(struct addrinfo *a)
{
    while (a) {
        struct addrinfo *next = a->next;
        w32_free(a->canonname);
        w32_free(a);
        a = next;
    }
}

static int WINAPI GetAddrInfoW(const WCHAR *node, const WCHAR *service, const struct addrinfo_w *hints,
                               struct addrinfo_w **out)
{
    char *n = node ? w32_narrow(node) : NULL, *s = service ? w32_narrow(service) : NULL;
    struct addrinfo h = {0};
    if (hints) {
        h.flags = hints->flags & ~2;
        h.family = hints->family;
        h.socktype = hints->socktype;
    }
    struct addrinfo *a = NULL;
    int r = getaddrinfo(n, s, hints ? &h : NULL, &a);
    w32_free(n);
    w32_free(s);
    *out = (struct addrinfo_w *)a;
    return r;
}

static void WINAPI FreeAddrInfoW(struct addrinfo_w *a)
{
    freeaddrinfo((struct addrinfo *)a);
}

struct servent {
    char *name;
    char **aliases;
    int16_t port;
    char *proto;
};

static W32_STATE struct servent serv;
static W32_STATE char serv_name[16];
static W32_STATE char *serv_aliases[1];

static struct servent *WINAPI getservbyname(const char *name, const char *proto)
{
    uint16_t port = service_port(name);
    if (!port)
        return NULL;
    str_copy(serv_name, name, sizeof(serv_name));
    serv = (struct servent){serv_name, serv_aliases, (int16_t)net_htons(port), (char *)(proto ? proto : "tcp")};
    return &serv;
}

struct wsabuf {
    ULONG len;
    char *buf;
};

static void complete(UINT s, uint32_t *ov, DWORD bytes, uint32_t status)
{
    ov[0] = status;
    ov[1] = bytes;
    struct wsock *w = s >= SOCKET_BASE && s < SOCKET_BASE + NET_MAX_SOCKETS ? &socks[s - SOCKET_BASE] : NULL;
    if (w && w->port)
        w32_port_post(w->port, bytes, w->key, ov);
}

static long recv_now(struct wsock *w, char *buf, ULONG len, struct sockaddr_in *from, int *fromlen)
{
    if (!from)
        return net_recv(w->id, buf, len);
    uint32_t ip;
    uint16_t port;
    long n = net_recvfrom(w->id, buf, len, &ip, &port);
    if (n >= 0)
        fill_addr(from, fromlen, ip, port);
    return n;
}

static int overlapped_recv(UINT s, struct wsock *w, char *buf, ULONG len, DWORD *got, uint32_t *ov,
                           struct sockaddr_in *from, int *fromlen)
{
    long n = recv_now(w, buf, len, from, fromlen);
    if (n >= 0) {
        if (got)
            *got = (DWORD)n;
        complete(s, ov, (DWORD)n, 0);
        return 0;
    }
    if (n != E_AGAIN)
        return fail((int)n);
    for (int i = 0; i < MAX_PENDING; i++)
        if (!pending[i].used) {
            pending[i] = (struct pending){true, s, buf, len, ov, from, fromlen};
            ov[0] = STATUS_PENDING;
            w32_set_error(WSA_IO_PENDING);
            return SOCKET_ERROR;
        }
    return fail(E_NOMEM);
}

void w32_winsock_poll(void)
{
    net_poll();
    for (int i = 0; i < MAX_PENDING; i++) {
        struct pending *p = &pending[i];
        if (!p->used)
            continue;
        struct wsock *w = sock_of(p->s);
        long n = w ? recv_now(w, p->buf, p->len, p->from, p->fromlen) : E_BADF;
        if (n == E_AGAIN)
            continue;
        p->used = false;
        complete(p->s, p->ov, n > 0 ? (DWORD)n : 0, n >= 0 ? 0 : STATUS_CONNECTION_RESET);
    }
}

bool w32_winsock_cancel(UINT s, uint32_t *ov)
{
    bool found = false;
    for (int i = 0; i < MAX_PENDING; i++) {
        struct pending *p = &pending[i];
        if (p->used && p->s == s && (!ov || p->ov == ov)) {
            p->used = false;
            complete(p->s, p->ov, 0, STATUS_CANCELLED);
            found = true;
        }
    }
    return found;
}

bool w32_socket_associate(uintptr_t s, HANDLE port, uintptr_t key)
{
    if (s < SOCKET_BASE || s >= SOCKET_BASE + NET_MAX_SOCKETS || !socks[s - SOCKET_BASE].used)
        return false;
    socks[s - SOCKET_BASE].port = port;
    socks[s - SOCKET_BASE].key = key;
    return true;
}

static int WINAPI WSASend(UINT s, struct wsabuf *bufs, DWORD count, DWORD *sent, DWORD flags, void *ov, void *cb)
{
    UNUSED(cb);
    DWORD total = 0;
    for (DWORD i = 0; i < count; i++) {
        int n = send(s, bufs[i].buf, (int)bufs[i].len, (int)flags);
        if (n < 0)
            return SOCKET_ERROR;
        total += (DWORD)n;
    }
    if (sent)
        *sent = total;
    if (ov)
        complete(s, ov, total, 0);
    return 0;
}

static int WINAPI WSARecv(UINT s, struct wsabuf *bufs, DWORD count, DWORD *got, DWORD *flags, void *ov, void *cb)
{
    UNUSED(cb);
    UNUSED(flags);
    if (!count)
        return 0;
    if (ov) {
        struct wsock *w = sock_of(s);
        return w ? overlapped_recv(s, w, bufs[0].buf, bufs[0].len, got, ov, NULL, NULL) : SOCKET_ERROR;
    }
    int n = recv(s, bufs[0].buf, (int)bufs[0].len, 0);
    if (n < 0)
        return SOCKET_ERROR;
    if (got)
        *got = (DWORD)n;
    return 0;
}

static int WINAPI WSAUnsupported3(UINT s, uint32_t a, uint32_t b)
{
    UNUSED(s);
    UNUSED(a);
    UNUSED(b);
    w32_set_error(WSAEOPNOTSUPP);
    return SOCKET_ERROR;
}

static int WINAPI WSAUnsupported4(UINT s, uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(c);
    return WSAUnsupported3(s, a, b);
}

static BOOL WINAPI ext_unsupported8(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                                    uint32_t h)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    UNUSED(e);
    UNUSED(f);
    UNUSED(g);
    UNUSED(h);
    w32_set_error(WSAEOPNOTSUPP);
    return FALSE;
}

static BOOL WINAPI ext_unsupported7(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
{
    return ext_unsupported8(a, b, c, d, e, f, g, 0);
}

static BOOL WINAPI ConnectEx(UINT s, const struct sockaddr_in *name, int len, const char *buf, DWORD buflen,
                             DWORD *sent, uint32_t *ov)
{
    struct wsock *w = sock_of(s);
    if (!w)
        return FALSE;
    bool nb = w->nonblocking;
    w->nonblocking = false;
    int r = connect(s, name, len);
    w->nonblocking = nb;
    if (r)
        return FALSE;
    DWORD n = 0;
    if (buf && buflen) {
        int k = send(s, buf, (int)buflen, 0);
        if (k > 0)
            n = (DWORD)k;
    }
    if (sent)
        *sent = n;
    if (ov)
        complete(s, ov, n, 0);
    return TRUE;
}

static BOOL WINAPI ext_unsupported4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return ext_unsupported8(a, b, c, d, 0, 0, 0, 0);
}

static int WINAPI WSAIoctl(UINT s, DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len, DWORD *bytes,
                           void *ov, void *cb)
{
    UNUSED(s);
    UNUSED(ov);
    UNUSED(cb);
    if (code == 0xc8000006u && in && in_len >= 16 && out && out_len >= 4) {
        static const uint8_t accept_ex[4] = {0xb5, 0x36, 0x7c, 0xb9}, connect_ex[4] = {0xb9, 0x07, 0xa2, 0x25};
        void *fn = !memcmp(in, accept_ex, 4)    ? (void *)ext_unsupported8
                   : !memcmp(in, connect_ex, 4) ? (void *)ConnectEx
                                                : (void *)ext_unsupported4;
        const uint8_t *g = in;
        if (g[0] == 0x16 && g[1] == 0x96)
            fn = (void *)ext_unsupported4;
        if (g[0] == 0xe3 && g[1] == 0x7f)
            fn = (void *)ext_unsupported7;
        memcpy(out, &fn, 4);
        if (bytes)
            *bytes = 4;
        return 0;
    }
    w32_set_error(WSAEOPNOTSUPP);
    return SOCKET_ERROR;
}

static int WINAPI WSASendTo(UINT s, struct wsabuf *bufs, DWORD count, DWORD *sent, DWORD flags,
                            const struct sockaddr_in *to, int tolen, void *ov, void *cb)
{
    UNUSED(cb);
    DWORD total = 0;
    for (DWORD i = 0; i < count; i++) {
        int n = sendto(s, bufs[i].buf, (int)bufs[i].len, (int)flags, to, tolen);
        if (n < 0)
            return SOCKET_ERROR;
        total += (DWORD)n;
    }
    if (sent)
        *sent = total;
    if (ov)
        complete(s, ov, total, 0);
    return 0;
}

static int WINAPI WSARecvFrom(UINT s, struct wsabuf *bufs, DWORD count, DWORD *got, DWORD *flags,
                              struct sockaddr_in *from, int *fromlen, void *ov, void *cb)
{
    UNUSED(cb);
    UNUSED(flags);
    if (!count)
        return 0;
    if (ov) {
        struct wsock *w = sock_of(s);
        return w ? overlapped_recv(s, w, bufs[0].buf, bufs[0].len, got, ov, from, fromlen) : SOCKET_ERROR;
    }
    int n = recvfrom(s, bufs[0].buf, (int)bufs[0].len, 0, from, fromlen);
    if (n < 0)
        return SOCKET_ERROR;
    if (got)
        *got = (DWORD)n;
    return 0;
}

static int WINAPI WSAConnect(UINT s, const struct sockaddr_in *addr, int len, void *a, void *b, void *c, void *d)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return connect(s, addr, len);
}

static int WINAPI WSAStringToAddressW(const WCHAR *text, int family, void *info, struct sockaddr_in *addr, int *len)
{
    UNUSED(info);
    char buf[64];
    size_t n = 0;
    while (text && text[n] && n < sizeof(buf) - 1) {
        buf[n] = (char)text[n];
        n++;
    }
    buf[n] = 0;
    uint16_t port = 0;
    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = 0;
        port = (uint16_t)str_to_long(colon + 1, NULL, 10);
    }
    struct sockaddr_in a = {0};
    if (family != 2 || !len || *len < (int)sizeof(a) || inet_pton(2, buf, &a.addr) != 1) {
        w32_set_error(WSAEINVAL);
        return SOCKET_ERROR;
    }
    a.family = 2;
    a.port = net_htons(port);
    *addr = a;
    *len = sizeof(a);
    return 0;
}

static int WINAPI getnameinfo(const struct sockaddr_in *sa, int salen, char *host, DWORD hostlen, char *serv,
                              DWORD servlen, int flags)
{
    UNUSED(salen);
    UNUSED(flags);
    if (!sa || sa->family != 2)
        return WSAEAFNOSUPPORT;
    if (host && hostlen && !inet_ntop(2, &sa->addr, host, hostlen))
        return WSAEINVAL;
    if (serv && servlen)
        ksnprintf(serv, servlen, "%u", (unsigned)net_htons(sa->port));
    return 0;
}

static int WINAPI GetNameInfoW(const struct sockaddr_in *sa, int salen, WCHAR *host, DWORD hostlen, WCHAR *serv,
                               DWORD servlen, int flags)
{
    char h[64], sv[16];
    int r = getnameinfo(sa, salen, host ? h : NULL, sizeof(h), serv ? sv : NULL, sizeof(sv), flags);
    if (r)
        return r;
    for (DWORD i = 0; host && i < hostlen; i++)
        if (!(host[i] = (WCHAR)(uint8_t)h[i]))
            break;
    for (DWORD i = 0; serv && i < servlen; i++)
        if (!(serv[i] = (WCHAR)(uint8_t)sv[i]))
            break;
    return 0;
}

struct protoent {
    char *name;
    char **aliases;
    int16_t proto;
};

static W32_STATE struct protoent proto_entry;
static W32_STATE char proto_name[8];

static struct protoent *protocol(const char *name, int number)
{
    static const struct {
        const char *name;
        int number;
    } table[] = {{"ip", 0}, {"icmp", 1}, {"tcp", 6}, {"udp", 17}};
    for (size_t i = 0; i < ARRAY_SIZE(table); i++)
        if ((name && !str_casecmp(name, table[i].name)) || (!name && number == table[i].number)) {
            str_copy(proto_name, table[i].name, sizeof(proto_name));
            proto_entry = (struct protoent){proto_name, serv_aliases, (int16_t)table[i].number};
            return &proto_entry;
        }
    w32_set_error(11001);
    return NULL;
}

static struct protoent *WINAPI getprotobyname(const char *name)
{
    return protocol(name, -1);
}

static struct protoent *WINAPI getprotobynumber(int number)
{
    return protocol(NULL, number);
}

static struct servent *WINAPI getservbyport(int port, const char *proto)
{
    UNUSED(port);
    UNUSED(proto);
    w32_set_error(11004);
    return NULL;
}

static int WINAPI WSADuplicateSocketW(UINT s, DWORD pid, void *info)
{
    UNUSED(pid);
    UNUSED(info);
    return WSAUnsupported3(s, 0, 0);
}

void w32_winsock_cleanup(void)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (socks[i].used) {
            net_close(socks[i].id);
            socks[i].used = false;
        }
    started = 0;
}

static void winsock_attach(void)
{
    memset(socks, 0, sizeof(socks));
    started = 0;
}

struct addrinfo_ex_w {
    int flags, family, socktype, protocol;
    SIZE_T addrlen;
    WCHAR *canonname;
    struct sockaddr_in *addr;
    void *blob;
    SIZE_T bloblen;
    void *provider;
    struct addrinfo_ex_w *next;
};

static void WINAPI FreeAddrInfoExW(struct addrinfo_ex_w *a)
{
    while (a) {
        struct addrinfo_ex_w *next = a->next;
        w32_free(a->addr);
        w32_free(a->canonname);
        w32_free(a);
        a = next;
    }
}

static int WINAPI GetAddrInfoExW(const WCHAR *node, const WCHAR *service, DWORD ns, void *nsid,
                                 const struct addrinfo_ex_w *hints, struct addrinfo_ex_w **out, void *timeout,
                                 void *overlapped, void *completion, HANDLE *cancel)
{
    UNUSED(ns);
    UNUSED(nsid);
    UNUSED(timeout);
    UNUSED(completion);
    if (cancel)
        *cancel = NULL;
    *out = NULL;
    if (overlapped) {
        WSASetLastError(10045);
        return 10045;
    }
    struct addrinfo_w h = {0}, *list = NULL;
    if (hints) {
        h.flags = hints->flags;
        h.family = hints->family;
        h.socktype = hints->socktype;
        h.protocol = hints->protocol;
    }
    int r = GetAddrInfoW(node, service, hints ? &h : NULL, &list);
    if (r)
        return r;
    struct addrinfo_ex_w **tail = out;
    for (struct addrinfo_w *a = list; a; a = a->next) {
        struct addrinfo_ex_w *e = w32_zalloc(sizeof(*e));
        void *addr = e ? w32_alloc(a->addrlen) : NULL;
        if (!addr) {
            w32_free(e);
            FreeAddrInfoExW(*out);
            FreeAddrInfoW(list);
            *out = NULL;
            return 8;
        }
        memcpy(addr, a->addr, a->addrlen);
        *e = (struct addrinfo_ex_w){a->flags, a->family, a->socktype, a->protocol, a->addrlen, NULL, addr,
                                    NULL, 0, NULL, NULL};
        if (a->canonname) {
            size_t n = w32_wcslen(a->canonname) + 1;
            if ((e->canonname = w32_alloc(n * sizeof(WCHAR))))
                memcpy(e->canonname, a->canonname, n * sizeof(WCHAR));
        }
        *tail = e;
        tail = &e->next;
    }
    FreeAddrInfoW(list);
    return 0;
}

static int WINAPI WSALookupServiceBeginW(void *restrictions, DWORD flags, HANDLE *lookup)
{
    UNUSED(restrictions);
    UNUSED(flags);
    *lookup = (HANDLE)(uintptr_t)0x00050005u;
    return 0;
}

static int WINAPI WSALookupServiceNextW(HANDLE lookup, DWORD flags, DWORD *len, void *results)
{
    UNUSED(lookup);
    UNUSED(flags);
    UNUSED(len);
    UNUSED(results);
    WSASetLastError(10110);
    return -1;
}

static int WINAPI no_service_entries(DWORD *len, void *buf)
{
    UNUSED(buf);
    if (len)
        *len = 0;
    return 0;
}

static int WINAPI WSAEnumProtocolsW(int *protocols, void *buf, DWORD *len)
{
    UNUSED(protocols);
    return no_service_entries(len, buf);
}

static int WINAPI WSCEnumProtocols(int *protocols, void *buf, DWORD *len, int *error)
{
    UNUSED(error);
    return WSAEnumProtocolsW(protocols, buf, len);
}

static int WINAPI WSCGetProviderPath(const void *guid, WCHAR *path, int *len, int *error)
{
    UNUSED(guid);
    UNUSED(path);
    UNUSED(len);
    *error = 10022;
    return -1;
}

static int WINAPI socket_error_set3(uint32_t a, uint32_t b, uint32_t c)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    WSASetLastError(10045);
    return -1;
}

static BOOL WINAPI WSAGetOverlappedResult(uint32_t s, void *overlapped, DWORD *transferred, BOOL wait, DWORD *flags)
{
    UNUSED(s);
    UNUSED(overlapped);
    UNUSED(wait);
    if (transferred)
        *transferred = 0;
    if (flags)
        *flags = 0;
    WSASetLastError(996);
    return FALSE;
}

static const struct w32_export winsock_exports[] = {
    W32_FN(GetAddrInfoExW), W32_FN(FreeAddrInfoExW), W32_AS("GetAddrInfoExCancel", w32_one1),
    W32_FN(WSALookupServiceBeginW), W32_FN(WSALookupServiceNextW), W32_AS("WSALookupServiceEnd", w32_zero1),
    W32_AS("WSASetServiceW", socket_error_set3), W32_AS("WSAEnumNameSpaceProvidersW", no_service_entries),
    W32_FN(WSAEnumProtocolsW), W32_FN(WSCEnumProtocols), W32_FN(WSCGetProviderPath), W32_FN(WSAGetOverlappedResult),
    W32_FN(WSAStartup),     W32_FN(WSACleanup),       W32_FN(WSAGetLastError), W32_FN(WSASetLastError),
    W32_FN(socket),         W32_FN(WSASocketA),       W32_AS("WSASocketW", WSASocketA), W32_FN(closesocket),
    W32_FN(bind),           W32_FN(listen),           W32_FN(accept),          W32_FN(connect),
    W32_FN(send),           W32_FN(recv),             W32_FN(sendto),          W32_FN(recvfrom),
    W32_FN(shutdown),       W32_FN(ioctlsocket),      W32_FN(setsockopt),      W32_FN(getsockopt),
    W32_FN(getsockname),    W32_FN(getpeername),      W32_FN(select),          W32_FN(__WSAFDIsSet),
    W32_FN(WSAPoll),        W32_AS("htonl", ws_htonl), W32_AS("ntohl", ws_htonl), W32_AS("htons", ws_htons),
    W32_AS("ntohs", ws_htons), W32_FN(inet_addr),     W32_FN(inet_ntoa),       W32_FN(inet_pton),
    W32_FN(inet_ntop),      W32_FN(gethostname),      W32_FN(gethostbyname),   W32_FN(gethostbyaddr),
    W32_FN(getaddrinfo),    W32_FN(freeaddrinfo),     W32_FN(GetAddrInfoW),    W32_FN(FreeAddrInfoW),
    W32_FN(getservbyname),  W32_FN(WSASend),          W32_FN(WSARecv),
    W32_FN(WSACreateEvent),
    W32_FN(WSASetEvent),
    W32_FN(WSAResetEvent),
    W32_FN(WSACloseEvent),
    W32_FN(WSAEnumNetworkEvents),
    W32_FN(WSAWaitForMultipleEvents),
    W32_AS("WSAAsyncSelect", WSAUnsupported4),        W32_FN(WSAEventSelect),
    W32_FN(WSAIoctl),       W32_FN(WSASendTo),        W32_FN(WSARecvFrom),     W32_FN(WSAConnect),
    W32_FN(WSAStringToAddressW), W32_FN(getnameinfo), W32_FN(GetNameInfoW),    W32_FN(getprotobyname),
    W32_FN(getprotobynumber), W32_FN(getservbyport),  W32_FN(WSADuplicateSocketW),
};

static const char *const winsock_ordinals[] = {
    [1] = "accept",          [2] = "bind",           [3] = "closesocket",    [4] = "connect",
    [5] = "getpeername",     [6] = "getsockname",    [7] = "getsockopt",     [8] = "htonl",
    [9] = "htons",           [10] = "ioctlsocket",   [11] = "inet_addr",     [12] = "inet_ntoa",
    [13] = "listen",         [14] = "ntohl",         [15] = "ntohs",         [16] = "recv",
    [17] = "recvfrom",       [18] = "select",        [19] = "send",          [20] = "sendto",
    [21] = "setsockopt",     [22] = "shutdown",      [23] = "socket",        [51] = "gethostbyaddr",
    [52] = "gethostbyname",  [53] = "getprotobyname", [54] = "getprotobynumber", [55] = "getservbyname",
    [56] = "getservbyport", [57] = "gethostname",   [111] = "WSAGetLastError",
    [112] = "WSASetLastError", [115] = "WSAStartup", [116] = "WSACleanup",   [151] = "__WSAFDIsSet",
};

W32_EXPORTS(winsock_module, "ws2_32.dll", winsock_exports, .attach = winsock_attach, .ordinals = winsock_ordinals,
            .ordinal_count = ARRAY_SIZE(winsock_ordinals));
W32_EXPORTS(wsock32_module, "wsock32.dll", winsock_exports, .attach = winsock_attach, .ordinals = winsock_ordinals,
            .ordinal_count = ARRAY_SIZE(winsock_ordinals));
