#include <synth/string.h>

#include "win64.h"

#define POLL_MAX 256
#define BUF_MAX 64

#define P(v) w64_ptr(v)
#define U(v) ((uint32_t)(uintptr_t)(v))

static W32_STATE uint8_t host64[32];
static W32_STATE uint64_t host_aliases64[8], host_addrs64[16];
static W32_STATE uint8_t serv64[32];
static W32_STATE uint64_t serv_aliases64[8];

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void put32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static void widen_list(uint32_t list, uint64_t *out, int cap)
{
    int n = 0;
    for (const uint32_t *p = P(list); p && *p && n < cap - 1; p++)
        out[n++] = *p;
    out[n] = 0;
}

static uint64_t h_wsa_startup(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t d[400];
    uint32_t r = (uint32_t)T32(t, U(P(a[0])), U(d));
    if (a[1]) {
        uint8_t *o = P(a[1]);
        memset(o, 0, 408);
        memcpy(o, d, 4);
        memcpy(o + 4, d + 390, 4);
        put64(o + 8, rd32(d + 396));
        memcpy(o + 16, d + 4, 257);
        memcpy(o + 273, d + 261, 129);
    }
    return w64_sx(r);
}

static uint64_t addrinfo64(uint32_t list)
{
    uint64_t head = 0;
    uint8_t *prev = NULL;
    for (const uint32_t *ai = P(list); ai; ai = P(ai[7])) {
        uint8_t *n = w32_zalloc(56);
        if (!n)
            break;
        memcpy(n, ai, 16);
        put64(n + 16, ai[4]);
        put64(n + 24, ai[5]);
        put64(n + 32, ai[6]);
        if (prev)
            put64(prev + 40, U(n));
        else
            head = U(n);
        prev = n;
    }
    if (head)
        put32(P(head + 48), list);
    return head;
}

static uint64_t h_getaddrinfo(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t res = 0;
    uint32_t r = (uint32_t)T32(t, U(P(a[0])), U(P(a[1])), U(P(a[2])), U(&res));
    if (a[3])
        put64(P(a[3]), r == 0 ? addrinfo64(res) : 0);
    return w64_sx(r);
}

static uint64_t h_freeaddrinfo(uint64_t *a, const uint64_t *f, void *t)
{
    if (!a[0])
        return 0;
    uint32_t original = rd32(P(a[0] + 48));
    for (uint64_t p = a[0]; p;) {
        uint64_t next = rd64(P(p + 40));
        w32_free(P(p));
        p = next;
    }
    T32(t, original);
    return 0;
}

static void fdset_in(uint64_t p, uint32_t *buf)
{
    uint32_t n = MIN(rd32(P(p)), 64u);
    buf[0] = n;
    for (uint32_t i = 0; i < n; i++)
        buf[1 + i] = (uint32_t)rd64(P(p + 8 + i * 8));
}

static void fdset_out(uint64_t p, const uint32_t *buf)
{
    put32(P(p), buf[0]);
    for (uint32_t i = 0; i < buf[0]; i++)
        put64(P(p + 8 + i * 8), w64_ret(buf[1 + i]));
}

static uint64_t h_select(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t sets[3][65];
    for (int i = 0; i < 3; i++)
        if (a[1 + i])
            fdset_in(a[1 + i], sets[i]);
    uint32_t r = (uint32_t)T32(t, U(P(a[0])), a[1] ? U(sets[0]) : 0, a[2] ? U(sets[1]) : 0,
                               a[3] ? U(sets[2]) : 0, U(P(a[4])));
    for (int i = 0; i < 3; i++)
        if (a[1 + i])
            fdset_out(a[1 + i], sets[i]);
    return w64_sx(r);
}

static uint64_t h_fd_is_set(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t n = MIN(rd32(P(a[1])), 64u);
    for (uint32_t i = 0; i < n; i++)
        if (rd64(P(a[1] + 8 + i * 8)) == a[0])
            return 1;
    return 0;
}

static uint64_t hostent64(uint32_t h)
{
    if (!h)
        return 0;
    const uint32_t *src = P(h);
    memset(host64, 0, sizeof(host64));
    put64(host64, src[0]);
    widen_list(src[1], host_aliases64, ARRAY_SIZE(host_aliases64));
    put64(host64 + 8, U(host_aliases64));
    memcpy(host64 + 16, src + 2, 4);
    widen_list(src[3], host_addrs64, ARRAY_SIZE(host_addrs64));
    put64(host64 + 24, U(host_addrs64));
    return U(host64);
}

static uint64_t h_gethostby(uint64_t *a, const uint64_t *f, void *t)
{
    return hostent64((uint32_t)T32(t, U(P(a[0])), U(P(a[1])), U(P(a[2]))));
}

static uint64_t h_getservby(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t s = (uint32_t)T32(t, U(P(a[0])), U(P(a[1])));
    if (!s)
        return 0;
    const uint32_t *src = P(s);
    memset(serv64, 0, sizeof(serv64));
    put64(serv64, src[0]);
    widen_list(src[1], serv_aliases64, ARRAY_SIZE(serv_aliases64));
    put64(serv64 + 8, U(serv_aliases64));
    put64(serv64 + 16, src[3]);
    memcpy(serv64 + 24, src + 2, 2);
    return U(serv64);
}

static uint64_t h_poll(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t n = (uint32_t)MIN(a[1], (uint64_t)POLL_MAX);
    uint32_t fds[POLL_MAX * 2];
    uint8_t *src = P(a[0]);
    for (uint32_t i = 0; i < n; i++) {
        fds[i * 2] = (uint32_t)rd64(src + i * 16);
        memcpy(&fds[i * 2 + 1], src + i * 16 + 8, 4);
    }
    uint32_t r = (uint32_t)T32(t, U(fds), n, U(P(a[2])));
    for (uint32_t i = 0; i < n; i++)
        memcpy(src + i * 16 + 10, (uint8_t *)&fds[i * 2 + 1] + 2, 2);
    return w64_sx(r);
}

/* functions handed out by WSAIoctl (ConnectEx, AcceptEx...) are 32-bit: 64-bit callers get a stub for each */
static uint64_t h_ext_fn(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_sx((uint32_t)T32(t, U(P(a[0])), U(P(a[1])), U(P(a[2])), U(P(a[3])), U(P(a[4])), U(P(a[5])),
                                U(P(a[6])), U(P(a[7]))));
}

static uint64_t h_wsa_ioctl(uint64_t *a, const uint64_t *f, void *t)
{
    bool ext = (uint32_t)a[1] == 0xc8000006u && a[4] && (uint32_t)a[5] >= 8;
    uint32_t fn = 0;
    uint32_t r = (uint32_t)T32(t, U(P(a[0])), (uint32_t)a[1], U(P(a[2])), (uint32_t)a[3], ext ? U(&fn) : U(P(a[4])),
                               ext ? 4 : (uint32_t)a[5], U(P(a[6])), U(P(a[7])), U(P(a[8])));
    if (ext && !r) {
        uint64_t stub = fn ? U(w64_target_stub(h_ext_fn, (void *)(uintptr_t)fn)) : 0;
        memcpy(P(a[4]), &stub, 8);
        if (a[6])
            put32(P(a[6]), 8);
    }
    return w64_sx(r);
}

static uint64_t h_wsa_buffers(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t n = (uint32_t)MIN(a[2], (uint64_t)BUF_MAX), bufs[BUF_MAX * 2];
    const uint8_t *src = P(a[1]);
    for (uint32_t i = 0; i < n; i++) {
        bufs[i * 2] = rd32(src + i * 16);
        bufs[i * 2 + 1] = (uint32_t)rd64(src + i * 16 + 8);
    }
    uint32_t r = (uint32_t)T32(t, U(P(a[0])), U(bufs), n, U(P(a[3])), U(P(a[4])), U(P(a[5])),
                               U(P(a[6])), U(P(a[7])), U(P(a[8])));
    return w64_sx(r);
}

static const struct w64_export ws2_exports[] = {
    W64_FN("WSAStartup", h_wsa_startup),
    W64_FN("getaddrinfo", h_getaddrinfo),
    W64_FN("GetAddrInfoW", h_getaddrinfo),
    W64_FN("freeaddrinfo", h_freeaddrinfo),
    W64_FN("FreeAddrInfoW", h_freeaddrinfo),
    W64_FN("select", h_select),
    W64_FN("__WSAFDIsSet", h_fd_is_set),
    W64_FN("gethostbyname", h_gethostby),
    W64_FN("gethostbyaddr", h_gethostby),
    W64_FN("getservbyname", h_getservby),
    W64_FN("getservbyport", h_getservby),
    W64_FN("WSAPoll", h_poll),
    W64_FN("WSAIoctl", h_wsa_ioctl),
    W64_FN("WSASend", h_wsa_buffers),
    W64_FN("WSARecv", h_wsa_buffers),
    W64_FN("WSASendTo", h_wsa_buffers),
    W64_FN("WSARecvFrom", h_wsa_buffers),
};
W64_EXPORTS(ws2_module, "ws2_32.dll", ws2_exports);
W64_EXPORTS(wsock_module, "wsock32.dll", ws2_exports);
