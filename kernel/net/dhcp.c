#include <synth/error.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/string.h>

#include "net.h"

#define DHCP_MAGIC 0x63825363u

enum {
    DISCOVER = 1,
    OFFER = 2,
    REQUEST = 3,
    ACK = 5,
    NAK = 6,
};

enum dhcp_state {
    IDLE,
    SELECTING,
    REQUESTING,
    BOUND,
};

struct lease {
    enum dhcp_state state;
    uint32_t xid, offered, server, mask, router, dns, lease;
    uint64_t next, renew_at;
    int attempts;
};

static struct lease leases[4];

static struct lease *lease_of(struct net_iface *ifc)
{
    for (int i = 0; i < net_interface_count() && i < 4; i++)
        if (net_interface(i) == ifc)
            return &leases[i];
    return &leases[0];
}

static void send_dhcp(struct net_iface *ifc, struct lease *l, uint8_t type)
{
    uint8_t p[8 + 300];
    memset(p, 0, sizeof(p));
    uint8_t *b = p + 8;
    b[0] = 1;
    b[1] = 1;
    b[2] = 6;
    wr32(b + 4, l->xid);
    wr16(b + 10, 0x8000);
    memcpy(b + 28, ifc->dev->mac, 6);
    wr32(b + 236, DHCP_MAGIC);
    uint8_t *o = b + 240;
    *o++ = 53;
    *o++ = 1;
    *o++ = type;
    *o++ = 61;
    *o++ = 7;
    *o++ = 1;
    memcpy(o, ifc->dev->mac, 6);
    o += 6;
    *o++ = 12;
    *o++ = 7;
    memcpy(o, "synthos", 7);
    o += 7;
    if (type == REQUEST) {
        *o++ = 50;
        *o++ = 4;
        wr32(o, l->offered);
        o += 4;
        *o++ = 54;
        *o++ = 4;
        wr32(o, l->server);
        o += 4;
    }
    *o++ = 55;
    *o++ = 3;
    *o++ = 1;
    *o++ = 3;
    *o++ = 6;
    *o++ = 255;
    size_t length = (size_t)(o - b);
    if (length < 300)
        length = 300;
    wr16(p, 68);
    wr16(p + 2, 67);
    wr16(p + 4, (uint16_t)(length + 8));
    wr16(p + 6, 0);
    uint16_t sum = net_checksum(p, length + 8, net_pseudo_sum(0, 0xffffffffu, IP_UDP, (uint16_t)(length + 8)));
    wr16(p + 6, sum ? sum : 0xffff);
    ip_send(0, 0xffffffffu, IP_UDP, p, length + 8);
}

void net_dhcp_start(struct net_iface *ifc)
{
    struct lease *l = lease_of(ifc);
    memset(l, 0, sizeof(*l));
    l->xid = hal_entropy();
    l->state = SELECTING;
    l->next = net_now() + 250;
    ifc->dhcp = true;
    send_dhcp(ifc, l, DISCOVER);
}

void dhcp_input(struct net_iface *ifc, const uint8_t *b, size_t length)
{
    struct lease *l = lease_of(ifc);
    if (length < 240 || b[0] != 2 || rd32(b + 4) != l->xid || rd32(b + 236) != DHCP_MAGIC)
        return;
    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0, lease = 3600;
    for (size_t i = 240; i < length;) {
        uint8_t code = b[i];
        if (code == 255)
            break;
        if (code == 0) {
            i++;
            continue;
        }
        if (i + 1 >= length)
            break;
        uint8_t n = b[i + 1];
        const uint8_t *v = b + i + 2;
        if (i + 2 + n > length)
            break;
        if (code == 53 && n >= 1)
            type = v[0];
        else if (code == 1 && n >= 4)
            mask = rd32(v);
        else if (code == 3 && n >= 4)
            router = rd32(v);
        else if (code == 6 && n >= 4)
            dns = rd32(v);
        else if (code == 54 && n >= 4)
            server = rd32(v);
        else if (code == 51 && n >= 4)
            lease = rd32(v);
        i += 2 + n;
    }
    uint32_t yiaddr = rd32(b + 16);
    if (type == OFFER && l->state == SELECTING) {
        l->offered = yiaddr;
        l->server = server;
        l->state = REQUESTING;
        l->next = net_now() + 1000;
        send_dhcp(ifc, l, REQUEST);
    } else if (type == ACK && l->state == REQUESTING) {
        l->state = BOUND;
        l->mask = mask ? mask : 0xffffff00u;
        l->router = router;
        l->dns = dns;
        l->lease = lease;
        l->renew_at = net_now() + (uint64_t)MAX(lease / 2, 60u) * 1000;
        net_configure(ifc, yiaddr, l->mask, router, dns ? dns : router);
        ifc->dhcp = true;
        char ip[16];
        net_format_ip(yiaddr, ip, sizeof(ip));
        klog("dhcp: %s is %s", ifc->dev->name, ip);
    } else if (type == NAK) {
        net_dhcp_start(ifc);
    }
}

void dhcp_tick(uint64_t now)
{
    for (int i = 0; i < net_interface_count() && i < 4; i++) {
        struct net_iface *ifc = net_interface(i);
        struct lease *l = &leases[i];
        if (!ifc->dhcp)
            continue;
        if (l->state == BOUND) {
            if (now >= l->renew_at) {
                l->state = REQUESTING;
                l->xid = hal_entropy();
                l->next = now + 2000;
                send_dhcp(ifc, l, REQUEST);
            }
            continue;
        }
        if (l->state == IDLE || now < l->next)
            continue;
        l->attempts++;
        uint64_t backoff = (uint64_t)MIN(250 << MIN(l->attempts, 7), 30000);
        l->next = now + backoff;
        if (l->state == REQUESTING && l->attempts % 3 == 0)
            l->state = SELECTING;
        send_dhcp(ifc, l, l->state == SELECTING ? DISCOVER : REQUEST);
    }
}

int net_dhcp(struct net_iface *ifc, int timeout_ms)
{
    net_configure(ifc, 0, 0, 0, 0);
    net_dhcp_start(ifc);
    uint64_t start = net_now();
    while (!ifc->configured) {
        if (net_now() - start > (uint64_t)timeout_ms)
            return E_TIMEDOUT;
        net_wait(5);
    }
    return 0;
}
