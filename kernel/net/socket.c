#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "net.h"

#define UDP_QUEUE 16

struct sock net_socks[NET_MAX_SOCKETS];

static struct sock *get(int s)
{
    return s >= 0 && s < NET_MAX_SOCKETS && net_socks[s].used ? &net_socks[s] : NULL;
}

static bool port_used(enum net_type type, uint16_t port)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct sock *s = &net_socks[i];
        if (s->used && s->type == type && s->bound && s->lport == port &&
            !(type == NET_TCP && s->state == TCP_TIME_WAIT))
            return true;
    }
    return false;
}

uint16_t net_ephemeral_port(enum net_type type)
{
    static uint16_t next = 49152;
    for (int i = 0; i < 16384; i++) {
        uint16_t port = next++;
        if (next < 49152)
            next = 49152;
        if (!port_used(type, port))
            return port;
    }
    return 0;
}

int net_socket(enum net_type type)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (!net_socks[i].used) {
            struct sock *s = &net_socks[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->type = type;
            s->parent = -1;
            s->state = TCP_CLOSED;
            return i;
        }
    return E_MFILE;
}

int net_bind(int id, uint32_t ip, uint16_t port)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (s->bound)
        return E_INVAL;
    if (!port)
        port = net_ephemeral_port(s->type);
    else if (port_used(s->type, port))
        return E_ADDRINUSE;
    s->lip = ip;
    s->lport = port;
    s->bound = true;
    return 0;
}

int net_local(int id, uint32_t *ip, uint16_t *port)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    *ip = s->lip;
    *port = s->lport;
    return 0;
}

int net_peer(int id, uint32_t *ip, uint16_t *port)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (!s->rip && s->type == NET_TCP)
        return E_NOTCONN;
    *ip = s->rip;
    *port = s->rport;
    return 0;
}

int net_error(int id)
{
    struct sock *s = get(id);
    return s ? s->error : E_BADF;
}

int net_set_broadcast(int id, bool on)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    s->broadcast = on;
    return 0;
}

static int udp_send(struct sock *s, const void *buf, size_t n, uint32_t ip, uint16_t port)
{
    if (n + 8 > NET_MTU - IP_HEADER)
        return E_RANGE;
    if (!s->bound) {
        int err = net_bind((int)(s - net_socks), 0, 0);
        if (err)
            return err;
    }
    uint8_t *p = kmalloc(n + 8);
    if (!p)
        return E_NOMEM;
    uint32_t src = s->lip ? s->lip : net_source_for(ip);
    wr16(p, s->lport);
    wr16(p + 2, port);
    wr16(p + 4, (uint16_t)(n + 8));
    wr16(p + 6, 0);
    memcpy(p + 8, buf, n);
    uint16_t sum = net_checksum(p, n + 8, net_pseudo_sum(src, ip, IP_UDP, (uint16_t)(n + 8)));
    wr16(p + 6, sum ? sum : 0xffff);
    int err = ip_send(src, ip, IP_UDP, p, n + 8);
    kfree(p);
    return err ? err : (int)n;
}

void udp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t length)
{
    if (length < 8)
        return;
    uint16_t sport = rd16(p), dport = rd16(p + 2), ulen = rd16(p + 4);
    if (ulen < 8 || ulen > length)
        return;
    if (rd16(p + 6) && net_checksum(p, ulen, net_pseudo_sum(src, dst, IP_UDP, ulen)) != 0)
        return;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct sock *s = &net_socks[i];
        if (!s->used || s->type != NET_UDP || !s->bound || s->lport != dport)
            continue;
        if (s->lip && s->lip != dst && !(dst == 0xffffffffu && s->broadcast))
            continue;
        if (s->rip && (s->rip != src || s->rport != sport))
            continue;
        if (s->queue_count == UDP_QUEUE)
            return;
        struct dgram *d = kmalloc(sizeof(*d) + ulen - 8);
        if (!d)
            return;
        d->ip = src;
        d->port = sport;
        d->length = (uint16_t)(ulen - 8);
        memcpy(d->data, p + 8, d->length);
        s->queue[(s->queue_head + s->queue_count++) % UDP_QUEUE] = d;
        return;
    }
}

long net_sendto(int id, const void *buf, size_t n, uint32_t ip, uint16_t port)
{
    if (ip != 0xffffffffu)
        net_await_address();
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (s->type != NET_UDP)
        return net_send(id, buf, n);
    if (ip == 0xffffffffu && !s->broadcast)
        return E_PERM;
    return udp_send(s, buf, n, ip, port);
}

long net_recvfrom(int id, void *buf, size_t n, uint32_t *ip, uint16_t *port)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (s->type != NET_UDP) {
        long r = net_recv(id, buf, n);
        if (r >= 0) {
            if (ip)
                *ip = s->rip;
            if (port)
                *port = s->rport;
        }
        return r;
    }
    if (!s->queue_count)
        return E_AGAIN;
    struct dgram *d = s->queue[s->queue_head];
    s->queue_head = (s->queue_head + 1) % UDP_QUEUE;
    s->queue_count--;
    size_t k = MIN(n, (size_t)d->length);
    memcpy(buf, d->data, k);
    if (ip)
        *ip = d->ip;
    if (port)
        *port = d->port;
    kfree(d);
    return (long)k;
}

bool net_readable(int id)
{
    struct sock *s = get(id);
    if (!s)
        return true;
    if (s->type == NET_UDP)
        return s->queue_count > 0;
    if (s->state == TCP_LISTEN)
        return s->accept_count > 0;
    return s->rx_len > 0 || s->fin_received || s->error || s->state == TCP_CLOSED;
}

bool net_writable(int id)
{
    struct sock *s = get(id);
    if (!s)
        return true;
    if (s->type == NET_UDP)
        return true;
    if (s->error)
        return true;
    return (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) && s->tx_len < s->tx_cap;
}

bool net_connected(int id)
{
    struct sock *s = get(id);
    return s && s->type == NET_TCP && (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT);
}

int net_connect(int id, uint32_t ip, uint16_t port, int timeout_ms)
{
    int err = net_connect_start(id, ip, port);
    if (err && err != E_AGAIN)
        return err;
    struct sock *s = get(id);
    if (!s || s->type == NET_UDP)
        return err;
    uint64_t start = net_now();
    while (s->state == TCP_SYN_SENT && !s->error) {
        if (timeout_ms >= 0 && net_now() - start > (uint64_t)timeout_ms)
            return E_TIMEDOUT;
        net_wait(1);
    }
    return s->error ? s->error : 0;
}

int net_close(int id)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (s->type == NET_UDP) {
        while (s->queue_count) {
            kfree(s->queue[s->queue_head]);
            s->queue_head = (s->queue_head + 1) % UDP_QUEUE;
            s->queue_count--;
        }
        s->used = false;
        return 0;
    }
    tcp_release(s);
    return 0;
}

int net_sockets(struct net_socket_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < NET_MAX_SOCKETS && n < max; i++) {
        struct sock *s = &net_socks[i];
        if (!s->used)
            continue;
        out[n++] = (struct net_socket_info){i,       s->type,  s->state,  s->lip,   s->rip,
                                            s->lport, s->rport, s->rx_len + (size_t)s->queue_count, s->tx_len};
    }
    return n;
}
