#ifndef SYNTH_NET_INTERNAL_H
#define SYNTH_NET_INTERNAL_H

#include <synth/net.h>

#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17
#define IP_HEADER 20
#define LOOPBACK 0x7f000001u

struct dgram {
    uint32_t ip;
    uint16_t port;
    uint16_t length;
    uint8_t data[];
};

struct tcp_segment;

struct sock {
    bool used;
    enum net_type type;
    int error;
    uint32_t lip, rip;
    uint16_t lport, rport;
    bool bound, broadcast, orphan;

    struct dgram *queue[16];
    int queue_head, queue_count;

    enum tcp_state state;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, irs, rcv_nxt;
    uint16_t mss;
    uint8_t *tx;
    size_t tx_cap, tx_len;
    uint8_t *rx;
    size_t rx_cap, rx_head, rx_len;
    uint64_t rx_total;
    uint8_t snd_shift, rcv_shift, ack_due;
    struct tcp_segment *ahead;
    int ahead_count;
    bool fin_received, fin_queued, fin_sent;
    uint64_t rto_at, linger_until;
    uint32_t rto;
    int retries;
    int parent;
    int backlog;
    int accept_queue[16];
    int accept_count;
};

extern struct sock net_socks[NET_MAX_SOCKETS];

uint16_t net_checksum(const void *data, size_t length, uint32_t sum);
uint32_t net_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t length);
int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t length);
uint32_t net_source_for(uint32_t dst);
bool net_is_local(uint32_t ip);
uint64_t net_now(void);
uint16_t net_ephemeral_port(enum net_type type);

void udp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t length);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t length);
void tcp_tick(uint64_t now);
void tcp_release(struct sock *s);
void dhcp_input(struct net_iface *ifc, const uint8_t *data, size_t length);
void dhcp_tick(uint64_t now);

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#endif
