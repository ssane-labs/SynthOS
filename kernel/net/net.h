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

int ip6_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t length);
void ip6_input(struct net_iface *ifc, const uint8_t *p, size_t length);
void ip6_start(struct net_iface *ifc);
void ip6_tick(uint64_t now);
uint32_t net_source6_for(uint32_t dst);
uint32_t net_pseudo_sum6(uint32_t src, uint32_t dst, uint8_t proto, uint32_t length);
bool net_v6_local(uint32_t ip);
int net_eth_send(struct net_iface *ifc, const uint8_t *mac, uint16_t type, const void *payload, size_t length);
void net_ping_answer(uint16_t id, uint16_t seq, uint8_t ttl);
uint16_t net_mss_for(uint32_t dst);
uint32_t net_dns_server(void);
void net_set_dns_override(uint32_t server);
void net_ip_input(const uint8_t *p, size_t length);
int udp_send_raw(uint32_t dst, uint16_t dport, uint16_t sport, const void *data, size_t n);
bool wg_routes(uint32_t dst);
uint32_t wg_address(void);
bool wg_blocks_v6(uint32_t dst);
uint16_t wg_mss(void);
bool wg_claims(uint32_t src, uint16_t sport, uint16_t dport);
void wg_input(const uint8_t *m, size_t n);
int wg_output(const uint8_t *packet, size_t n);
void wg_tick(uint64_t now);
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
