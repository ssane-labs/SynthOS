#ifndef SYNTH_NET_H
#define SYNTH_NET_H

#include <synth/types.h>

#define NET_MTU 1500
#define NET_FRAME 1514
#define NET_MAX_SOCKETS 32

struct netdev {
    char name[16];
    const char *driver;
    uint8_t mac[6];
    int (*send)(struct netdev *dev, const void *frame, size_t length);
    int (*receive)(struct netdev *dev, void *frame, size_t cap);
    bool (*link)(struct netdev *dev);
    void *priv;
};

struct net_iface {
    struct netdev *dev;
    uint32_t ip, mask, gateway, dns;
    bool configured, dhcp;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, dropped;
};

enum net_type {
    NET_TCP = 1,
    NET_UDP = 2,
};

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct net_socket_info {
    int id;
    enum net_type type;
    enum tcp_state state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    size_t queued_rx, queued_tx;
};

void net_register(struct netdev *dev);
void net_init(void);
void net_poll(void);
void net_wait(int ms);
struct net_iface *net_interface(int index);
int net_interface_count(void);
int net_configure(struct net_iface *ifc, uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);
int net_dhcp(struct net_iface *ifc, int timeout_ms);
void net_dhcp_start(struct net_iface *ifc);
const char *net_state_name(enum tcp_state state);

uint32_t net_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
bool net_parse_ip(const char *s, uint32_t *out);
void net_format_ip(uint32_t ip, char *out, size_t cap);
int net_resolve(const char *name, uint32_t *ip, int timeout_ms);
int net_ping(uint32_t ip, uint16_t seq, size_t size, int timeout_ms, uint32_t *rtt_ms, uint8_t *ttl);

int net_socket(enum net_type type);
int net_bind(int s, uint32_t ip, uint16_t port);
int net_listen(int s, int backlog);
int net_connect_start(int s, uint32_t ip, uint16_t port);
int net_connect(int s, uint32_t ip, uint16_t port, int timeout_ms);
int net_accept(int s, uint32_t *ip, uint16_t *port);
long net_send(int s, const void *buf, size_t n);
long net_recv(int s, void *buf, size_t n);
long net_sendto(int s, const void *buf, size_t n, uint32_t ip, uint16_t port);
long net_recvfrom(int s, void *buf, size_t n, uint32_t *ip, uint16_t *port);
int net_shutdown(int s);
int net_close(int s);
bool net_readable(int s);
bool net_writable(int s);
bool net_connected(int s);
int net_error(int s);
int net_local(int s, uint32_t *ip, uint16_t *port);
int net_peer(int s, uint32_t *ip, uint16_t *port);
int net_sockets(struct net_socket_info *out, int max);
int net_set_broadcast(int s, bool on);

static inline uint16_t net_htons(uint16_t v)
{
    return (uint16_t)(v << 8 | v >> 8);
}

static inline uint32_t net_htonl(uint32_t v)
{
    return v << 24 | (v & 0xff00) << 8 | (v >> 8 & 0xff00) | v >> 24;
}

#endif
