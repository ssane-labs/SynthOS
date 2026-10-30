#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "net.h"

#define MAX_IFACES 4
#define ARP_SIZE 32
#define ARP_TTL 600000
#define LOOP_QUEUE 16
#define POLL_BUDGET 64

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
    uint64_t time, asked;
    uint8_t *pending;
    size_t pending_length;
};

struct loop_packet {
    size_t length;
    uint8_t data[NET_MTU];
};

static struct net_iface ifaces[MAX_IFACES];
static int iface_count;
static struct arp_entry arp[ARP_SIZE];
static struct loop_packet *loop_queue[LOOP_QUEUE];
static int loop_head, loop_count;
static bool polling, net_off;
static uint8_t rx_frame[NET_FRAME + 64];

struct ping_wait {
    uint16_t id, seq;
    bool done;
    uint8_t ttl;
};

static struct ping_wait ping;

uint64_t net_now(void)
{
    return hal_ticks_ms();
}

uint32_t net_ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d;
}

bool net_parse_ip(const char *s, uint32_t *out)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (v > 255)
                return false;
        }
        ip = ip << 8 | v;
        if (part < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *out = ip;
    return true;
}

void net_format_ip(uint32_t ip, char *out, size_t cap)
{
    ksnprintf(out, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

uint16_t net_checksum(const void *data, size_t length, uint32_t sum)
{
    const uint8_t *p = data;
    while (length > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        length -= 2;
    }
    if (length)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t net_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t length)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + length;
}

void net_register(struct netdev *dev)
{
    if (iface_count == MAX_IFACES)
        return;
    struct net_iface *ifc = &ifaces[iface_count];
    memset(ifc, 0, sizeof(*ifc));
    ifc->dev = dev;
    ksnprintf(dev->name, sizeof(dev->name), "eth%d", iface_count);
    iface_count++;
    klog("net: %s %s %02x:%02x:%02x:%02x:%02x:%02x", dev->name, dev->driver, dev->mac[0], dev->mac[1], dev->mac[2],
         dev->mac[3], dev->mac[4], dev->mac[5]);
}

struct net_iface *net_interface(int index)
{
    return index >= 0 && index < iface_count ? &ifaces[index] : NULL;
}

int net_interface_count(void)
{
    return iface_count;
}

int net_configure(struct net_iface *ifc, uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns)
{
    ifc->ip = ip;
    ifc->mask = mask;
    ifc->gateway = gateway;
    ifc->dns = dns;
    ifc->configured = ip != 0;
    for (int i = 0; i < ARP_SIZE; i++)
        arp[i].valid = false;
    return 0;
}

bool net_is_local(uint32_t ip)
{
    if ((ip >> 24) == 127)
        return true;
    for (int i = 0; i < iface_count; i++)
        if (ifaces[i].configured && ifaces[i].ip == ip)
            return true;
    return false;
}

static struct net_iface *route(uint32_t dst)
{
    for (int i = 0; i < iface_count; i++)
        if (ifaces[i].configured && (dst & ifaces[i].mask) == (ifaces[i].ip & ifaces[i].mask))
            return &ifaces[i];
    for (int i = 0; i < iface_count; i++)
        if (ifaces[i].configured && ifaces[i].gateway)
            return &ifaces[i];
    return iface_count ? &ifaces[0] : NULL;
}

uint32_t net_source_for(uint32_t dst)
{
    if ((dst >> 24) == 127)
        return LOOPBACK;
    if (net_is_local(dst))
        return dst;
    struct net_iface *ifc = route(dst);
    return ifc ? ifc->ip : 0;
}

static int eth_send(struct net_iface *ifc, const uint8_t *mac, uint16_t type, const void *payload, size_t length)
{
    static uint8_t frame[NET_FRAME];
    if (length > NET_MTU)
        return E_RANGE;
    if (net_off)
        return E_UNREACH;
    memcpy(frame, mac, 6);
    memcpy(frame + 6, ifc->dev->mac, 6);
    wr16(frame + 12, type);
    memcpy(frame + 14, payload, length);
    size_t total = MAX(length + 14, (size_t)60);
    if (length + 14 < 60)
        memset(frame + 14 + length, 0, 60 - 14 - length);
    int err = ifc->dev->send(ifc->dev, frame, total);
    if (!err) {
        ifc->tx_packets++;
        ifc->tx_bytes += total;
    }
    return err;
}

static void arp_send(struct net_iface *ifc, uint16_t op, const uint8_t *target_mac, uint32_t target_ip)
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t p[28];
    wr16(p, 1);
    wr16(p + 2, 0x0800);
    p[4] = 6;
    p[5] = 4;
    wr16(p + 6, op);
    memcpy(p + 8, ifc->dev->mac, 6);
    wr32(p + 14, ifc->ip);
    memcpy(p + 18, op == 1 ? (const uint8_t[6]){0} : target_mac, 6);
    wr32(p + 24, target_ip);
    eth_send(ifc, op == 1 ? broadcast : target_mac, 0x0806, p, sizeof(p));
}

static struct arp_entry *arp_find(uint32_t ip, bool create)
{
    struct arp_entry *free_slot = NULL, *oldest = &arp[0];
    for (int i = 0; i < ARP_SIZE; i++) {
        if ((arp[i].valid || arp[i].pending) && arp[i].ip == ip)
            return &arp[i];
        if (!arp[i].valid && !arp[i].pending && !free_slot)
            free_slot = &arp[i];
        if (arp[i].time < oldest->time)
            oldest = &arp[i];
    }
    if (!create)
        return NULL;
    struct arp_entry *e = free_slot ? free_slot : oldest;
    kfree(e->pending);
    memset(e, 0, sizeof(*e));
    e->ip = ip;
    return e;
}

static void arp_learn(struct net_iface *ifc, uint32_t ip, const uint8_t *mac)
{
    struct arp_entry *e = arp_find(ip, true);
    memcpy(e->mac, mac, 6);
    e->valid = true;
    e->time = net_now();
    if (e->pending) {
        eth_send(ifc, e->mac, 0x0800, e->pending, e->pending_length);
        kfree(e->pending);
        e->pending = NULL;
    }
}

static void arp_input(struct net_iface *ifc, const uint8_t *p, size_t length)
{
    if (length < 28 || rd16(p) != 1 || rd16(p + 2) != 0x0800)
        return;
    uint16_t op = rd16(p + 6);
    uint32_t sender = rd32(p + 14), target = rd32(p + 24);
    if (sender)
        arp_learn(ifc, sender, p + 8);
    if (op == 1 && ifc->configured && target == ifc->ip)
        arp_send(ifc, 2, p + 8, sender);
}

static void loop_push(const void *packet, size_t length)
{
    if (loop_count == LOOP_QUEUE || length > NET_MTU)
        return;
    struct loop_packet *p = kmalloc(sizeof(*p));
    if (!p)
        return;
    memcpy(p->data, packet, length);
    p->length = length;
    loop_queue[(loop_head + loop_count++) % LOOP_QUEUE] = p;
}

int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t length)
{
    static uint16_t ident;
    static uint8_t packet[NET_MTU];
    if (length + IP_HEADER > NET_MTU)
        return E_RANGE;
    if (!src)
        src = net_source_for(dst);
    uint8_t *h = packet;
    h[0] = 0x45;
    h[1] = 0;
    wr16(h + 2, (uint16_t)(length + IP_HEADER));
    wr16(h + 4, ++ident);
    wr16(h + 6, 0x4000);
    h[8] = 64;
    h[9] = proto;
    wr16(h + 10, 0);
    wr32(h + 12, src);
    wr32(h + 16, dst);
    wr16(h + 10, net_checksum(h, IP_HEADER, 0));
    memcpy(packet + IP_HEADER, payload, length);
    size_t total = length + IP_HEADER;
    if (net_is_local(dst)) {
        loop_push(packet, total);
        return 0;
    }
    struct net_iface *ifc = route(dst);
    if (!ifc)
        return E_UNREACH;
    bool broadcast = dst == 0xffffffffu || (ifc->configured && (dst | ifc->mask) == 0xffffffffu);
    if (broadcast)
        return eth_send(ifc, (const uint8_t[6]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 0x0800, packet, total);
    if (!ifc->configured)
        return E_UNREACH;
    uint32_t hop = (dst & ifc->mask) == (ifc->ip & ifc->mask) || !ifc->gateway ? dst : ifc->gateway;
    struct arp_entry *e = arp_find(hop, true);
    if (e->valid && net_now() - e->time < ARP_TTL)
        return eth_send(ifc, e->mac, 0x0800, packet, total);
    kfree(e->pending);
    e->pending = kmalloc(total);
    if (e->pending) {
        memcpy(e->pending, packet, total);
        e->pending_length = total;
    }
    e->valid = false;
    e->time = net_now();
    if (net_now() - e->asked > 200 || !e->asked) {
        e->asked = net_now();
        arp_send(ifc, 1, NULL, hop);
    }
    return 0;
}

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t length, uint8_t ttl)
{
    if (length < 8 || net_checksum(p, length, 0) != 0)
        return;
    if (p[0] == 8) {
        uint8_t *reply = kmalloc(length);
        if (!reply)
            return;
        memcpy(reply, p, length);
        reply[0] = 0;
        wr16(reply + 2, 0);
        wr16(reply + 2, net_checksum(reply, length, 0));
        ip_send(dst == 0xffffffffu ? 0 : dst, src, IP_ICMP, reply, length);
        kfree(reply);
    } else if (p[0] == 0 && rd16(p + 4) == ping.id && rd16(p + 6) == ping.seq) {
        ping.done = true;
        ping.ttl = ttl;
    }
}

static struct net_iface *iface_for(struct netdev *dev)
{
    for (int i = 0; i < iface_count; i++)
        if (ifaces[i].dev == dev)
            return &ifaces[i];
    return NULL;
}

static void ip_input(struct net_iface *ifc, const uint8_t *p, size_t length)
{
    if (length < IP_HEADER || (p[0] >> 4) != 4)
        return;
    size_t header = (size_t)(p[0] & 15) * 4, total = rd16(p + 2);
    if (header < IP_HEADER || total > length || total < header || net_checksum(p, header, 0) != 0)
        return;
    if (rd16(p + 6) & 0x3fff)
        return;
    uint32_t src = rd32(p + 12), dst = rd32(p + 16);
    bool mine = net_is_local(dst) || dst == 0xffffffffu ||
                (ifc && ifc->configured && (dst | ifc->mask) == 0xffffffffu) || (ifc && !ifc->configured);
    if (!mine)
        return;
    const uint8_t *payload = p + header;
    size_t n = total - header;
    switch (p[9]) {
    case IP_ICMP: icmp_input(src, dst, payload, n, p[8]); break;
    case IP_UDP:
        if (ifc && n >= 8 && rd16(payload + 2) == 68)
            dhcp_input(ifc, payload + 8, n - 8);
        else
            udp_input(src, dst, payload, n);
        break;
    case IP_TCP: tcp_input(src, dst, payload, n); break;
    }
}

static void eth_input(struct net_iface *ifc, const uint8_t *frame, size_t length)
{
    if (length < 14)
        return;
    ifc->rx_packets++;
    ifc->rx_bytes += length;
    uint16_t type = rd16(frame + 12);
    if (type == 0x0806)
        arp_input(ifc, frame + 14, length - 14);
    else if (type == 0x0800)
        ip_input(ifc, frame + 14, length - 14);
}

/* Counts received packets, so code waiting for the network knows when to look again. */
static uint32_t activity;

uint32_t net_activity(void)
{
    return activity;
}

static int poll_frames(void)
{
    if (polling)
        return 0;
    polling = true;
    int frames = 0;
    for (int i = 0; i < iface_count; i++) {
        struct netdev *dev = ifaces[i].dev;
        for (int k = 0; k < POLL_BUDGET; k++) {
            int n = dev->receive(dev, rx_frame, sizeof(rx_frame));
            if (n == 0)
                break;
            frames++;
            activity++;
            if (n > 0 && !net_off)
                eth_input(iface_for(dev), rx_frame, (size_t)n);
        }
    }
    for (int guard = 0; loop_count && guard < LOOP_QUEUE * 4; guard++) {
        struct loop_packet *p = loop_queue[loop_head];
        loop_head = (loop_head + 1) % LOOP_QUEUE;
        loop_count--;
        activity++;
        ip_input(NULL, p->data, p->length);
        kfree(p);
    }
    uint64_t now = net_now();
    tcp_tick(now);
    dhcp_tick(now);
    polling = false;
    return frames;
}

bool net_enabled(void)
{
    return !net_off;
}

static void get_network(char *buf, size_t cap)
{
    setting_flag_get(!net_off, buf, cap);
}

/* Off works like airplane mode: nothing is sent and what arrives is dropped; on asks DHCP for the address again. */
static int set_network(struct shell *sh, const char *value)
{
    UNUSED(sh);
    bool on;
    int err = setting_flag(value, &on);
    if (err || on == !net_off)
        return err;
    net_off = !on;
    for (int i = 0; on && i < iface_count; i++)
        if (ifaces[i].dhcp)
            net_dhcp_start(&ifaces[i]);
    return 0;
}

static const struct setting s_network = {"network", "on|off", "networking on, or off like airplane mode", get_network,
                                         set_network};
SETTING(s_network);

void net_poll(void)
{
    poll_frames();
}

/* Sleeps only when the network had nothing: a busy download is read back to back instead of in 2 ms steps. */
void net_wait(int ms)
{
    if (!poll_frames() && ms > 0 && !loop_count)
        hal_sleep_ms((uint32_t)MIN(ms, 2));
}

int net_ping(uint32_t ip, uint16_t seq, size_t size, int timeout_ms, uint32_t *rtt_ms, uint8_t *ttl)
{
    net_await_address();
    size = MIN(MAX(size, (size_t)8), (size_t)1400);
    uint8_t *p = kzalloc(size + 8);
    if (!p)
        return E_NOMEM;
    p[0] = 8;
    ping = (struct ping_wait){0x5359, seq, false, 0};
    wr16(p + 4, ping.id);
    wr16(p + 6, seq);
    for (size_t i = 0; i < size; i++)
        p[8 + i] = (uint8_t)('a' + i % 23);
    wr16(p + 2, net_checksum(p, size + 8, 0));
    uint64_t start = net_now();
    int err = ip_send(0, ip, IP_ICMP, p, size + 8);
    kfree(p);
    if (err)
        return err;
    while (!ping.done && net_now() - start < (uint64_t)timeout_ms)
        net_wait(1);
    if (!ping.done)
        return E_TIMEDOUT;
    if (rtt_ms)
        *rtt_ms = (uint32_t)(net_now() - start);
    if (ttl)
        *ttl = ping.ttl;
    return 0;
}

/* Boot does not wait for DHCP: the first thing that needs an address waits for the first lease a little. */
static uint64_t await_until;

void net_await_address(void)
{
    while (!hal_in_interrupt() && net_now() < await_until) {
        for (int i = 0; i < iface_count; i++)
            if (ifaces[i].configured)
                return;
        net_wait(5);
    }
}

void net_init(void)
{
    hal_net_init();
    if (!iface_count)
        return;
    await_until = net_now() + 4000;
    for (int i = 0; i < iface_count; i++)
        net_dhcp_start(&ifaces[i]);
    hal_set_idle(net_poll);
}
