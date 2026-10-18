#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "net.h"

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10
#define TX_BUFFER (32u * 1024)
#define RX_BUFFER (64u * 1024)
#define RX_MAX (1024u * 1024)
#define WSCALE 5
#define AHEAD_MAX 64
#define DEFAULT_MSS 1460
#define RTO_MIN 400
#define RTO_MAX 8000
#define MAX_RETRIES 8
#define TIME_WAIT_MS 1000

static bool seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

const char *net_state_name(enum tcp_state state)
{
    static const char *const names[] = {"CLOSED",     "LISTEN",     "SYN_SENT",   "SYN_RECEIVED",
                                        "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT",
                                        "CLOSING",    "LAST_ACK",   "TIME_WAIT"};
    return state <= TCP_TIME_WAIT ? names[state] : "?";
}

static struct sock *get(int s)
{
    return s >= 0 && s < NET_MAX_SOCKETS && net_socks[s].used && net_socks[s].type == NET_TCP ? &net_socks[s] : NULL;
}

/* A segment that arrived before the ones in front of it; it waits here instead of being sent again. */
struct tcp_segment {
    struct tcp_segment *next;
    uint32_t seq;
    size_t length;
    bool fin;
    uint8_t data[];
};

static uint16_t window(struct sock *s, bool syn)
{
    size_t free = s->rx_cap - s->rx_len;
    return (uint16_t)MIN(syn ? free : free >> s->rcv_shift, (size_t)65535);
}

static int segment(struct sock *s, uint32_t seq, uint8_t flags, const uint8_t *data, size_t length)
{
    uint8_t *p = kmalloc(24 + length);
    if (!p)
        return E_NOMEM;
    size_t header = (flags & SYN) ? (s->rcv_shift ? 28 : 24) : 20;
    wr16(p, s->lport);
    wr16(p + 2, s->rport);
    wr32(p + 4, seq);
    wr32(p + 8, (flags & ACK) ? s->rcv_nxt : 0);
    p[12] = (uint8_t)(header / 4) << 4;
    p[13] = flags;
    wr16(p + 14, s->rx ? window(s, flags & SYN) : 0);
    wr16(p + 16, 0);
    wr16(p + 18, 0);
    if (flags & SYN) {
        p[20] = 2;
        p[21] = 4;
        wr16(p + 22, DEFAULT_MSS);
        if (s->rcv_shift) {
            p[24] = 1;
            p[25] = 3;
            p[26] = 3;
            p[27] = s->rcv_shift;
        }
    }
    if (length)
        memcpy(p + header, data, length);
    uint32_t src = s->lip ? s->lip : net_source_for(s->rip);
    s->lip = src;
    uint16_t total = (uint16_t)(header + length);
    wr16(p + 16, net_checksum(p, total, net_pseudo_sum(src, s->rip, IP_TCP, total)));
    int err = ip_send(src, s->rip, IP_TCP, p, total);
    kfree(p);
    return err;
}

static void reset_reply(uint32_t src, uint32_t dst, const uint8_t *p, size_t length)
{
    uint8_t flags = p[13];
    if (flags & RST)
        return;
    struct sock tmp = {0};
    tmp.lip = dst;
    tmp.rip = src;
    tmp.lport = rd16(p + 2);
    tmp.rport = rd16(p);
    size_t header = (size_t)(p[12] >> 4) * 4;
    uint32_t seq = rd32(p + 4), ack = rd32(p + 8);
    size_t data = length > header ? length - header : 0;
    if (flags & ACK) {
        segment(&tmp, ack, RST, NULL, 0);
    } else {
        tmp.rcv_nxt = seq + (uint32_t)data + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0);
        segment(&tmp, 0, RST | ACK, NULL, 0);
    }
}

static bool buffers(struct sock *s)
{
    if (!s->tx)
        s->tx = kmalloc(TX_BUFFER);
    if (!s->rx)
        s->rx = kmalloc(RX_BUFFER);
    s->tx_cap = TX_BUFFER;
    s->rx_cap = RX_BUFFER;
    return s->tx && s->rx;
}

static void free_sock(struct sock *s)
{
    while (s->ahead) {
        struct tcp_segment *next = s->ahead->next;
        kfree(s->ahead);
        s->ahead = next;
    }
    s->ahead_count = 0;
    kfree(s->tx);
    kfree(s->rx);
    s->tx = s->rx = NULL;
    s->used = false;
}

static void arm(struct sock *s)
{
    s->rto_at = net_now() + s->rto;
}

static void output(struct sock *s)
{
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT && s->state != TCP_FIN_WAIT_1 &&
        s->state != TCP_LAST_ACK && s->state != TCP_CLOSING)
        return;
    uint32_t sent = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
    while (sent < s->tx_len) {
        uint32_t wnd = MAX(s->snd_wnd, 1u);
        if (sent >= wnd)
            break;
        uint32_t n = (uint32_t)MIN((size_t)(s->tx_len - sent), (size_t)MIN((uint32_t)s->mss, wnd - sent));
        if (segment(s, s->snd_nxt, ACK | PSH, s->tx + sent, n))
            break;
        s->snd_nxt += n;
        sent += n;
        if (!s->rto_at)
            arm(s);
    }
    if (s->fin_queued && !s->fin_sent && sent == s->tx_len) {
        segment(s, s->snd_nxt, FIN | ACK, NULL, 0);
        s->snd_nxt++;
        s->fin_sent = true;
        if (!s->rto_at)
            arm(s);
        if (s->state == TCP_ESTABLISHED)
            s->state = TCP_FIN_WAIT_1;
        else if (s->state == TCP_CLOSE_WAIT)
            s->state = TCP_LAST_ACK;
    }
}

int net_connect_start(int id, uint32_t ip, uint16_t port)
{
    net_await_address();
    struct sock *s = id >= 0 && id < NET_MAX_SOCKETS && net_socks[id].used ? &net_socks[id] : NULL;
    if (!s)
        return E_BADF;
    if (s->type == NET_UDP) {
        s->rip = ip;
        s->rport = port;
        return s->bound ? 0 : net_bind(id, 0, 0);
    }
    if (s->state != TCP_CLOSED)
        return s->state == TCP_SYN_SENT ? E_AGAIN : E_BUSY;
    if (!buffers(s))
        return E_NOMEM;
    if (!s->bound) {
        int err = net_bind(id, 0, 0);
        if (err)
            return err;
    }
    s->rip = ip;
    s->rport = port;
    s->lip = net_source_for(ip);
    s->iss = hal_entropy();
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->snd_wnd = DEFAULT_MSS;
    s->mss = DEFAULT_MSS;
    s->rcv_shift = WSCALE;
    s->snd_shift = 0;
    s->rx_total = 0;
    s->rto = 1000;
    s->retries = 0;
    s->error = 0;
    s->state = TCP_SYN_SENT;
    segment(s, s->iss, SYN, NULL, 0);
    arm(s);
    return E_AGAIN;
}

int net_listen(int id, int backlog)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (!s->bound)
        return E_INVAL;
    s->state = TCP_LISTEN;
    s->backlog = CLAMP(backlog, 1, 16);
    return 0;
}

int net_accept(int id, uint32_t *ip, uint16_t *port)
{
    struct sock *s = get(id);
    if (!s || s->state != TCP_LISTEN)
        return s ? E_INVAL : E_BADF;
    if (!s->accept_count)
        return E_AGAIN;
    int child = s->accept_queue[0];
    memmove(s->accept_queue, s->accept_queue + 1, sizeof(int) * (size_t)--s->accept_count);
    net_socks[child].parent = -1;
    if (ip)
        *ip = net_socks[child].rip;
    if (port)
        *port = net_socks[child].rport;
    return child;
}

long net_send(int id, const void *buf, size_t n)
{
    struct sock *s = get(id);
    if (!s) {
        struct sock *u = id >= 0 && id < NET_MAX_SOCKETS && net_socks[id].used ? &net_socks[id] : NULL;
        if (u && u->type == NET_UDP)
            return u->rip ? net_sendto(id, buf, n, u->rip, u->rport) : E_NOTCONN;
        return E_BADF;
    }
    if (s->error)
        return s->error;
    if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECEIVED)
        return E_AGAIN;
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return s->fin_queued ? E_PIPE : E_NOTCONN;
    size_t k = MIN(n, s->tx_cap - s->tx_len);
    if (!k)
        return E_AGAIN;
    memcpy(s->tx + s->tx_len, buf, k);
    s->tx_len += k;
    output(s);
    return (long)k;
}

long net_recv(int id, void *buf, size_t n)
{
    struct sock *s = get(id);
    if (!s) {
        struct sock *u = id >= 0 && id < NET_MAX_SOCKETS && net_socks[id].used ? &net_socks[id] : NULL;
        return u ? net_recvfrom(id, buf, n, NULL, NULL) : E_BADF;
    }
    if (s->rx_len) {
        size_t k = MIN(n, s->rx_len), before = s->rx_cap - s->rx_len;
        size_t first = MIN(k, s->rx_cap - s->rx_head);
        memcpy(buf, s->rx + s->rx_head, first);
        memcpy((uint8_t *)buf + first, s->rx, k - first);
        s->rx_head = (s->rx_head + k) % s->rx_cap;
        s->rx_len -= k;
        size_t after = s->rx_cap - s->rx_len;
        bool was_full = before < 2u * s->mss || (before < s->rx_cap / 4 && after >= s->rx_cap / 4);
        if (was_full && (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 || s->state == TCP_FIN_WAIT_2))
            segment(s, s->snd_nxt, ACK, NULL, 0);
        return (long)k;
    }
    if (s->error)
        return s->error;
    if (s->fin_received || s->state == TCP_CLOSED)
        return 0;
    if (s->state == TCP_LISTEN)
        return E_NOTCONN;
    return E_AGAIN;
}

int net_shutdown(int id)
{
    struct sock *s = get(id);
    if (!s)
        return E_BADF;
    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT) {
        s->fin_queued = true;
        output(s);
    }
    return 0;
}

void tcp_release(struct sock *s)
{
    if (s->state == TCP_LISTEN) {
        for (int i = 0; i < s->accept_count; i++) {
            struct sock *c = &net_socks[s->accept_queue[i]];
            segment(c, c->snd_nxt, RST, NULL, 0);
            free_sock(c);
        }
        for (int i = 0; i < NET_MAX_SOCKETS; i++)
            if (net_socks[i].used && net_socks[i].parent == (int)(s - net_socks))
                free_sock(&net_socks[i]);
        free_sock(s);
        return;
    }
    if (s->state == TCP_CLOSED || s->state == TCP_SYN_SENT) {
        free_sock(s);
        return;
    }
    s->orphan = true;
    if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT || s->state == TCP_SYN_RECEIVED) {
        if (s->rx_len) {
            segment(s, s->snd_nxt, RST | ACK, NULL, 0);
            free_sock(s);
            return;
        }
        s->fin_queued = true;
        if (s->state == TCP_SYN_RECEIVED)
            s->state = TCP_ESTABLISHED;
        output(s);
    }
    s->linger_until = net_now() + 30000;
}

static void closed(struct sock *s, int error)
{
    s->state = TCP_CLOSED;
    s->error = error;
    s->rto_at = 0;
    if (s->orphan || s->parent >= 0)
        free_sock(s);
}

/* Bulk transfers get a bigger receive buffer (up to RX_MAX), so a far server can keep a full window in flight. */
static void grow_rx(struct sock *s)
{
    if (s->rx_cap >= RX_MAX || s->rx_total < 2 * s->rx_cap || !s->rcv_shift)
        return;
    size_t cap = s->rx_cap * 2;
    uint8_t *rx = kmalloc(cap);
    if (!rx)
        return;
    size_t first = MIN(s->rx_len, s->rx_cap - s->rx_head);
    memcpy(rx, s->rx + s->rx_head, first);
    memcpy(rx + first, s->rx, s->rx_len - first);
    kfree(s->rx);
    s->rx = rx;
    s->rx_cap = cap;
    s->rx_head = 0;
}

static size_t append(struct sock *s, const uint8_t *data, size_t length)
{
    grow_rx(s);
    size_t k = MIN(length, s->rx_cap - s->rx_len);
    size_t at = (s->rx_head + s->rx_len) % s->rx_cap, first = MIN(k, s->rx_cap - at);
    memcpy(s->rx + at, data, first);
    memcpy(s->rx, data + first, k - first);
    s->rx_len += k;
    s->rx_total += k;
    s->rcv_nxt += (uint32_t)k;
    return k;
}

static void keep_ahead(struct sock *s, uint32_t seq, const uint8_t *data, size_t length, bool fin)
{
    if (!length || s->ahead_count >= AHEAD_MAX || seq - s->rcv_nxt + length > s->rx_cap - s->rx_len)
        return;
    struct tcp_segment **pp = &s->ahead;
    while (*pp && seq_lt((*pp)->seq, seq))
        pp = &(*pp)->next;
    if (*pp && (*pp)->seq == seq)
        return;
    struct tcp_segment *seg = kmalloc(sizeof(*seg) + length);
    if (!seg)
        return;
    seg->seq = seq;
    seg->length = length;
    seg->fin = fin;
    memcpy(seg->data, data, length);
    seg->next = *pp;
    *pp = seg;
    s->ahead_count++;
}

static void receive_data(struct sock *s, uint32_t seq, const uint8_t *data, size_t length, bool fin)
{
    if (seq_lt(seq, s->rcv_nxt)) {
        uint32_t skip = s->rcv_nxt - seq;
        if (skip >= length) {
            fin = fin && seq + length == s->rcv_nxt;
            data += length;
            length = 0;
        } else {
            data += skip;
            length -= skip;
        }
        seq = s->rcv_nxt;
    }
    if (seq != s->rcv_nxt) {
        keep_ahead(s, seq, data, length, fin);
        segment(s, s->snd_nxt, ACK, NULL, 0);
        return;
    }
    size_t k = append(s, data, length);
    while (k == length && !fin && s->ahead && !seq_lt(s->rcv_nxt, s->ahead->seq)) {
        struct tcp_segment *seg = s->ahead;
        s->ahead = seg->next;
        s->ahead_count--;
        uint32_t skip = s->rcv_nxt - seg->seq;
        if (skip < seg->length) {
            length = seg->length - skip;
            k = append(s, seg->data + skip, length);
            fin = seg->fin;
        }
        kfree(seg);
    }
    if (fin && k == length && !s->fin_received) {
        s->fin_received = true;
        s->rcv_nxt++;
        if (s->state == TCP_ESTABLISHED)
            s->state = TCP_CLOSE_WAIT;
        else if (s->state == TCP_FIN_WAIT_1)
            s->state = TCP_CLOSING;
        else if (s->state == TCP_FIN_WAIT_2) {
            s->state = TCP_TIME_WAIT;
            s->linger_until = net_now() + TIME_WAIT_MS;
        }
    }
    if (fin || s->ahead || ++s->ack_due >= 2) {
        segment(s, s->snd_nxt, ACK, NULL, 0);
        s->ack_due = 0;
    }
}

static void process_ack(struct sock *s, uint32_t ack, uint32_t wnd)
{
    s->snd_wnd = wnd;
    if (!seq_lt(s->snd_una, ack) || seq_lt(s->snd_nxt, ack))
        return;
    uint32_t acked = ack - s->snd_una;
    bool fin_acked = s->fin_sent && ack == s->snd_nxt;
    size_t data = MIN((size_t)acked - (fin_acked ? 1 : 0), s->tx_len);
    memmove(s->tx, s->tx + data, s->tx_len - data);
    s->tx_len -= data;
    s->snd_una = ack;
    s->retries = 0;
    s->rto = MAX(RTO_MIN, s->rto / 2);
    s->rto_at = s->snd_una == s->snd_nxt ? 0 : net_now() + s->rto;
    if (!fin_acked)
        return;
    if (s->state == TCP_FIN_WAIT_1)
        s->state = TCP_FIN_WAIT_2;
    else if (s->state == TCP_CLOSING) {
        s->state = TCP_TIME_WAIT;
        s->linger_until = net_now() + TIME_WAIT_MS;
    } else if (s->state == TCP_LAST_ACK)
        closed(s, 0);
}

static struct sock *demux(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport)
{
    struct sock *listener = NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct sock *s = &net_socks[i];
        if (!s->used || s->type != NET_TCP || s->lport != dport)
            continue;
        if (s->state == TCP_LISTEN) {
            if (!s->lip || s->lip == dst || net_is_local(dst))
                listener = s;
            continue;
        }
        if (s->state != TCP_CLOSED && s->rip == src && s->rport == sport)
            return s;
    }
    return listener;
}

static void accept_syn(struct sock *l, uint32_t src, uint32_t dst, uint16_t sport, uint32_t seq, uint16_t mss, int ws)
{
    int pending = 0;
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        pending += net_socks[i].used && net_socks[i].parent == (int)(l - net_socks);
    if (pending >= l->backlog + 4)
        return;
    int id = net_socket(NET_TCP);
    if (id < 0)
        return;
    struct sock *c = &net_socks[id];
    if (!buffers(c)) {
        free_sock(c);
        return;
    }
    c->bound = true;
    c->lip = dst;
    c->lport = l->lport;
    c->rip = src;
    c->rport = sport;
    c->irs = seq;
    c->rcv_nxt = seq + 1;
    c->iss = hal_entropy();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_wnd = DEFAULT_MSS;
    c->mss = mss;
    c->snd_shift = ws > 0 ? (uint8_t)ws : 0;
    c->rcv_shift = ws >= 0 ? WSCALE : 0;
    c->rx_total = 0;
    c->rto = 1000;
    c->parent = (int)(l - net_socks);
    c->state = TCP_SYN_RECEIVED;
    segment(c, c->iss, SYN | ACK, NULL, 0);
    arm(c);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t length)
{
    if (length < 20 || net_checksum(p, length, net_pseudo_sum(src, dst, IP_TCP, (uint16_t)length)) != 0)
        return;
    uint16_t sport = rd16(p), dport = rd16(p + 2);
    uint32_t seq = rd32(p + 4), ack = rd32(p + 8);
    size_t header = (size_t)(p[12] >> 4) * 4;
    uint8_t flags = p[13];
    uint16_t wnd = rd16(p + 14);
    if (header < 20 || header > length)
        return;
    uint16_t mss = DEFAULT_MSS;
    int ws = -1;
    for (size_t i = 20; i + 1 < header;) {
        uint8_t kind = p[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        uint8_t len = p[i + 1];
        if (len < 2)
            break;
        if (kind == 2 && len == 4 && i + 4 <= header)
            mss = (uint16_t)MIN(rd16(p + i + 2), DEFAULT_MSS);
        else if (kind == 3 && len == 3 && i + 3 <= header)
            ws = MIN(p[i + 2], 14);
        i += len;
    }
    const uint8_t *data = p + header;
    size_t n = length - header;
    struct sock *s = demux(src, dst, sport, dport);
    if (!s) {
        reset_reply(src, dst, p, length);
        return;
    }
    if (s->state == TCP_LISTEN) {
        if ((flags & SYN) && !(flags & (ACK | RST)))
            accept_syn(s, src, dst, sport, seq, mss, ws);
        else if (!(flags & RST))
            reset_reply(src, dst, p, length);
        return;
    }
    if (flags & RST) {
        closed(s, s->state == TCP_SYN_SENT ? E_CONNREFUSED : E_CONNRESET);
        return;
    }
    if (s->state == TCP_SYN_SENT) {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && ack == s->snd_nxt) {
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = wnd;
            s->mss = mss;
            s->snd_shift = ws > 0 ? (uint8_t)ws : 0;
            if (ws < 0)
                s->rcv_shift = 0;
            s->rto_at = 0;
            s->state = TCP_ESTABLISHED;
            segment(s, s->snd_nxt, ACK, NULL, 0);
            output(s);
        }
        return;
    }
    if (flags & SYN) {
        if (s->state == TCP_SYN_RECEIVED && seq + 1 == s->rcv_nxt)
            segment(s, s->iss, SYN | ACK, NULL, 0);
        else
            segment(s, s->snd_nxt, ACK, NULL, 0);
        return;
    }
    if (!(flags & ACK))
        return;
    if (s->state == TCP_SYN_RECEIVED) {
        if (ack != s->snd_nxt) {
            reset_reply(src, dst, p, length);
            return;
        }
        s->snd_una = ack;
        s->snd_wnd = (uint32_t)wnd << s->snd_shift;
        s->rto_at = 0;
        s->state = TCP_ESTABLISHED;
        if (s->parent >= 0) {
            struct sock *l = &net_socks[s->parent];
            if (l->used && l->state == TCP_LISTEN && l->accept_count < 16)
                l->accept_queue[l->accept_count++] = (int)(s - net_socks);
        }
    }
    process_ack(s, ack, (uint32_t)wnd << s->snd_shift);
    if (!s->used || s->state == TCP_CLOSED)
        return;
    if (n || (flags & FIN))
        receive_data(s, seq, data, n, flags & FIN);
    output(s);
}

void tcp_tick(uint64_t now)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct sock *s = &net_socks[i];
        if (!s->used || s->type != NET_TCP)
            continue;
        if (s->ack_due) {
            s->ack_due = 0;
            segment(s, s->snd_nxt, ACK, NULL, 0);
        }
        if (s->state == TCP_TIME_WAIT && now >= s->linger_until) {
            closed(s, 0);
            continue;
        }
        if (s->orphan && s->linger_until && now >= s->linger_until && s->state != TCP_CLOSED) {
            segment(s, s->snd_nxt, RST | ACK, NULL, 0);
            free_sock(s);
            continue;
        }
        if (!s->rto_at || now < s->rto_at)
            continue;
        if (++s->retries > MAX_RETRIES) {
            closed(s, E_TIMEDOUT);
            continue;
        }
        s->rto = MIN(s->rto * 2, (uint32_t)RTO_MAX);
        if (s->state == TCP_SYN_SENT) {
            segment(s, s->iss, SYN, NULL, 0);
        } else if (s->state == TCP_SYN_RECEIVED) {
            segment(s, s->iss, SYN | ACK, NULL, 0);
        } else {
            s->snd_nxt = s->snd_una;
            s->fin_sent = false;
            s->snd_wnd = MAX(s->snd_wnd, (uint32_t)s->mss);
            if (s->state == TCP_FIN_WAIT_1)
                s->state = TCP_ESTABLISHED;
            else if (s->state == TCP_LAST_ACK)
                s->state = TCP_CLOSE_WAIT;
            else if (s->state == TCP_CLOSING)
                s->state = TCP_CLOSE_WAIT;
            s->rto_at = 0;
            output(s);
        }
        s->rto_at = now + s->rto;
    }
}
