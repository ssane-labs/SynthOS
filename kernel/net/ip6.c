#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "net.h"

/* IPv6 beside IPv4. Every IPv6 address the system meets gets a 32-bit handle in 240.0.0.0/4 (an IPv4 range that is
   never used), so TCP, UDP, sockets, DNS and the Windows sockets keep their 32-bit addresses and work over IPv6
   unchanged; only this layer sees the 128-bit ones. Addresses come from router advertisements (SLAAC, RDNSS);
   neighbours are found with NDP. */

#define H6_BASE 0xf0000000u
#define H6_COUNT 512
#define NEIGHBORS 16
#define NEIGHBOR_TTL 600000
#define IP6_HEADER 40
#define ICMP6 58

struct handle6 {
    uint8_t a[16];
    bool used;
    uint64_t time;
};

struct neighbor {
    uint8_t a[16];
    uint8_t mac[6];
    bool valid;
    uint64_t time, asked;
    uint8_t *pending;
    size_t pending_length;
};

static struct handle6 *handles;
static struct neighbor neighbors[NEIGHBORS];
static struct net6 v6[NET_MAX_IFACES];
static bool v6_off;

static const uint8_t all_nodes[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t all_routers[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

bool net_is_v6(uint32_t ip)
{
    return ip >= H6_BASE && ip < H6_BASE + H6_COUNT;
}

static bool zero16(const uint8_t *a)
{
    for (int i = 0; i < 16; i++)
        if (a[i])
            return false;
    return true;
}

static bool is_mapped(const uint8_t *a)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return !memcmp(a, prefix, 12);
}

static bool is_loopback6(const uint8_t *a)
{
    return !memcmp(a, (const uint8_t[16]){0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16);
}

static bool link_local(const uint8_t *a)
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

static bool in_use(uint32_t h)
{
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        struct sock *s = &net_socks[i];
        if (!s->used)
            continue;
        if (s->lip == h || s->rip == h)
            return true;
        for (int k = 0; k < s->queue_count; k++)
            if (s->queue[(s->queue_head + k) % 16]->ip == h)
                return true;
    }
    for (int i = 0; i < NET_MAX_IFACES; i++)
        if (v6[i].ll_h == h || v6[i].global_h == h || v6[i].dns_h == h)
            return true;
    return false;
}

uint32_t net_v6_handle(const uint8_t a[16])
{
    if (is_mapped(a))
        return rd32(a + 12);
    if (is_loopback6(a))
        return LOOPBACK;
    if (zero16(a) || (!handles && !(handles = kzalloc(H6_COUNT * sizeof(*handles)))))
        return 0;
    int victim = -1;
    for (int i = 0; i < H6_COUNT; i++) {
        if (handles[i].used && !memcmp(handles[i].a, a, 16)) {
            handles[i].time = net_now();
            return H6_BASE + (uint32_t)i;
        }
        if (!handles[i].used && victim < 0)
            victim = i;
    }
    if (victim < 0) {
        uint64_t oldest = ~0ull;
        for (int i = 0; i < H6_COUNT; i++)
            if (handles[i].time < oldest && !in_use(H6_BASE + (uint32_t)i)) {
                oldest = handles[i].time;
                victim = i;
            }
        if (victim < 0)
            return 0;
    }
    memcpy(handles[victim].a, a, 16);
    handles[victim].used = true;
    handles[victim].time = net_now();
    return H6_BASE + (uint32_t)victim;
}

/* The 128-bit form of any address: an IPv6 handle, or IPv4 as ::ffff:a.b.c.d (127/8 as ::1). */
bool net_v6_addr(uint32_t ip, uint8_t out[16])
{
    if (net_is_v6(ip)) {
        if (!handles || !handles[ip - H6_BASE].used)
            return false;
        memcpy(out, handles[ip - H6_BASE].a, 16);
        return true;
    }
    memset(out, 0, 16);
    if (ip >> 24 == 127) {
        out[15] = 1;
        return true;
    }
    out[10] = out[11] = 0xff;
    wr32(out + 12, ip);
    return true;
}

bool net_parse_ip6(const char *s, uint8_t out[16])
{
    uint16_t words[8];
    int n = 0, gap = -1;
    if (s[0] == ':' && s[1] == ':') {
        gap = 0;
        s += 2;
    }
    while (*s) {
        if (n == 8)
            return false;
        const char *dot = NULL;
        for (const char *t = s; *t && *t != ':'; t++)
            if (*t == '.')
                dot = t;
        if (dot) {
            uint32_t v4;
            if (n > 6 || !net_parse_ip(s, &v4))
                return false;
            words[n++] = (uint16_t)(v4 >> 16);
            words[n++] = (uint16_t)v4;
            s += strlen(s);
            break;
        }
        uint32_t v = 0;
        int digits = 0;
        while (is_xdigit(*s) && digits < 5) {
            v = v * 16 + (uint32_t)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
            s++;
            digits++;
        }
        if (!digits || digits > 4)
            return false;
        words[n++] = (uint16_t)v;
        if (!*s)
            break;
        if (*s != ':')
            return false;
        s++;
        if (*s == ':') {
            if (gap >= 0)
                return false;
            gap = n;
            s++;
        } else if (!*s) {
            return false;
        }
    }
    if (gap < 0 ? n != 8 : n > 7)
        return false;
    memset(out, 0, 16);
    int tail = gap < 0 ? 0 : n - gap;
    for (int i = 0; i < n - tail; i++)
        wr16(out + i * 2, words[i]);
    for (int i = 0; i < tail; i++)
        wr16(out + 16 - (tail - i) * 2, words[n - tail + i]);
    return true;
}

void net_format_ip6(const uint8_t a[16], char *out, size_t cap)
{
    if (is_mapped(a)) {
        char v4[16];
        net_format_ip(rd32(a + 12), v4, sizeof(v4));
        ksnprintf(out, cap, "::ffff:%s", v4);
        return;
    }
    int best = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        if (rd16(a + i * 2)) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && !rd16(a + j * 2))
            j++;
        if (j - i > best_len && j - i > 1) {
            best = i;
            best_len = j - i;
        }
        i = j;
    }
    size_t n = 0;
    if (cap)
        out[0] = 0;
    for (int i = 0; i < 8 && n + 6 < cap; i++) {
        if (i == best) {
            n += (size_t)ksnprintf(out + n, cap - n, i == 0 ? "::" : ":");
            i += best_len - 1;
            continue;
        }
        n += (size_t)ksnprintf(out + n, cap - n, "%x%s", rd16(a + i * 2), i < 7 ? ":" : "");
    }
}

uint32_t net_pseudo_sum6(uint32_t src, uint32_t dst, uint8_t proto, uint32_t length)
{
    uint8_t s[16], d[16];
    net_v6_addr(src, s);
    net_v6_addr(dst, d);
    uint32_t sum = (length >> 16) + (length & 0xffff) + proto;
    for (int i = 0; i < 16; i += 2)
        sum += rd16(s + i) + rd16(d + i);
    return sum;
}

struct net6 *net_v6_info(int index)
{
    return index >= 0 && index < NET_MAX_IFACES ? &v6[index] : NULL;
}

bool net_v6_local(uint32_t ip)
{
    for (int i = 0; i < NET_MAX_IFACES; i++)
        if (ip && (v6[i].ll_h == ip || v6[i].global_h == ip))
            return true;
    return false;
}

static int iface_index(struct net_iface *ifc)
{
    for (int i = 0; i < NET_MAX_IFACES; i++)
        if (net_interface(i) == ifc)
            return i;
    return -1;
}

static struct net6 *v6_for(struct net_iface *ifc)
{
    int i = iface_index(ifc);
    return i >= 0 ? &v6[i] : NULL;
}

/* The interface and source address for a destination: link-local stays on its link, the rest needs a global
   address. */
static struct net_iface *pick(const uint8_t *dst, uint8_t *src)
{
    int best = -1;
    for (int i = 0; i < net_interface_count(); i++) {
        struct net6 *v = &v6[i];
        if (!v->ll_ok)
            continue;
        if (link_local(dst) || dst[0] == 0xff) {
            best = i;
            break;
        }
        if (!v->global_ok)
            continue;
        if (!memcmp(dst, v->global, 8)) {
            best = i;
            break;
        }
        if (v->router_ok && best < 0)
            best = i;
    }
    if (best < 0)
        return NULL;
    memcpy(src, link_local(dst) || dst[0] == 0xff ? v6[best].ll : v6[best].global, 16);
    return net_interface(best);
}

uint32_t net_source6_for(uint32_t dst)
{
    uint8_t d[16], s[16];
    if (!net_v6_addr(dst, d) || !pick(d, s))
        return 0;
    return net_v6_handle(s);
}

static void multicast_mac(const uint8_t *a, uint8_t *mac)
{
    mac[0] = 0x33;
    mac[1] = 0x33;
    memcpy(mac + 2, a + 12, 4);
}

static struct neighbor *neighbor_find(const uint8_t *a, bool create)
{
    struct neighbor *free_slot = NULL, *oldest = &neighbors[0];
    for (int i = 0; i < NEIGHBORS; i++) {
        if ((neighbors[i].valid || neighbors[i].pending) && !memcmp(neighbors[i].a, a, 16))
            return &neighbors[i];
        if (!neighbors[i].valid && !neighbors[i].pending && !free_slot)
            free_slot = &neighbors[i];
        if (neighbors[i].time < oldest->time)
            oldest = &neighbors[i];
    }
    if (!create)
        return NULL;
    struct neighbor *e = free_slot ? free_slot : oldest;
    kfree(e->pending);
    memset(e, 0, sizeof(*e));
    memcpy(e->a, a, 16);
    return e;
}

static int raw_send(struct net_iface *ifc, const uint8_t *src, const uint8_t *dst, uint8_t proto, uint8_t hops,
                    const void *payload, size_t length);

static void solicit(struct net_iface *ifc, const uint8_t *src, const uint8_t *target)
{
    uint8_t node[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
    memcpy(node + 13, target + 13, 3);
    uint8_t p[32] = {135};
    memcpy(p + 8, target, 16);
    p[24] = 1;
    p[25] = 1;
    memcpy(p + 26, ifc->dev->mac, 6);
    raw_send(ifc, src, node, ICMP6, 255, p, sizeof(p));
}

static void neighbor_learn(struct net_iface *ifc, const uint8_t *a, const uint8_t *mac)
{
    struct neighbor *e = neighbor_find(a, true);
    memcpy(e->mac, mac, 6);
    e->valid = true;
    e->time = net_now();
    if (e->pending) {
        net_eth_send(ifc, e->mac, 0x86dd, e->pending, e->pending_length);
        kfree(e->pending);
        e->pending = NULL;
    }
}

static int raw_send(struct net_iface *ifc, const uint8_t *src, const uint8_t *dst, uint8_t proto, uint8_t hops,
                    const void *payload, size_t length)
{
    static uint8_t packet[NET_MTU];
    if (length + IP6_HEADER > NET_MTU)
        return E_RANGE;
    uint8_t *h = packet;
    memset(h, 0, 4);
    h[0] = 0x60;
    wr16(h + 4, (uint16_t)length);
    h[6] = proto;
    h[7] = hops;
    memcpy(h + 8, src, 16);
    memcpy(h + 24, dst, 16);
    memcpy(packet + IP6_HEADER, payload, length);
    size_t total = length + IP6_HEADER;
    if (proto == ICMP6) {
        uint8_t *icmp = packet + IP6_HEADER;
        wr16(icmp + 2, 0);
        uint32_t sum = (uint32_t)length + ICMP6;
        for (int i = 0; i < 32; i += 2)
            sum += rd16(h + 8 + i);
        wr16(icmp + 2, net_checksum(icmp, length, sum));
    }
    uint8_t mac[6];
    if (dst[0] == 0xff) {
        multicast_mac(dst, mac);
        return net_eth_send(ifc, mac, 0x86dd, packet, total);
    }
    struct net6 *v = v6_for(ifc);
    const uint8_t *hop = dst;
    if (!link_local(dst) && v && v->global_ok && memcmp(dst, v->global, 8)) {
        if (!v->router_ok)
            return E_UNREACH;
        hop = v->router;
    }
    struct neighbor *e = neighbor_find(hop, true);
    if (e->valid && net_now() - e->time < NEIGHBOR_TTL)
        return net_eth_send(ifc, e->mac, 0x86dd, packet, total);
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
        solicit(ifc, link_local(src) || !v ? src : v->ll, hop);
    }
    return 0;
}

int ip6_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t length)
{
    uint8_t d[16], s[16], chosen[16];
    if (!net_v6_addr(dst, d) || v6_off)
        return E_UNREACH;
    struct net_iface *ifc = pick(d, chosen);
    if (!ifc)
        return E_UNREACH;
    if (!src || !net_v6_addr(src, s))
        memcpy(s, chosen, 16);
    if (proto == IP_ICMP)
        proto = ICMP6;
    return raw_send(ifc, s, d, proto, 64, payload, length);
}

static const uint8_t *option(const uint8_t *p, size_t length, size_t at, uint8_t type, size_t *len)
{
    while (at + 2 <= length && p[at + 1]) {
        size_t n = (size_t)p[at + 1] * 8;
        if (at + n > length)
            break;
        if (p[at] == type) {
            *len = n;
            return p + at;
        }
        at += n;
    }
    return NULL;
}

static void router_advert(struct net_iface *ifc, struct net6 *v, const uint8_t *src, const uint8_t *p, size_t length)
{
    if (length < 16)
        return;
    size_t n;
    const uint8_t *o = option(p, length, 16, 1, &n);
    if (o && n >= 8)
        neighbor_learn(ifc, src, o + 2);
    if (rd16(p + 6)) {
        memcpy(v->router, src, 16);
        v->router_ok = true;
    }
    for (size_t at = 16; at + 2 <= length && p[at + 1];) {
        size_t len = (size_t)p[at + 1] * 8;
        if (at + len > length)
            break;
        const uint8_t *q = p + at;
        if (q[0] == 3 && len >= 32 && q[2] == 64 && (q[3] & 0x40) && !v->global_ok && rd32(q + 4)) {
            memcpy(v->global, q + 16, 8);
            memcpy(v->global + 8, v->ll + 8, 8);
            v->global_ok = true;
            v->global_h = net_v6_handle(v->global);
            char text[48];
            net_format_ip6(v->global, text, sizeof(text));
            klog("net: %s IPv6 %s/64", ifc->dev->name, text);
        }
        if (q[0] == 25 && len >= 24 && rd32(q + 4)) {
            memcpy(v->dns, q + 8, 16);
            v->dns_ok = true;
            v->dns_h = net_v6_handle(v->dns);
        }
        at += len;
    }
}

static bool ours(const struct net6 *v, const uint8_t *a)
{
    return (v->ll_ok && !memcmp(a, v->ll, 16)) || (v->global_ok && !memcmp(a, v->global, 16));
}

static void icmp6_input(struct net_iface *ifc, const uint8_t *src, const uint8_t *dst, const uint8_t *p,
                        size_t length, uint8_t hops)
{
    if (length < 8)
        return;
    uint32_t sum = (uint32_t)length + ICMP6;
    for (int i = 0; i < 16; i += 2)
        sum += rd16(src + i) + rd16(dst + i);
    if (net_checksum(p, length, sum) != 0)
        return;
    struct net6 *v = v6_for(ifc);
    if (!v)
        return;
    switch (p[0]) {
    case 128: {
        if (dst[0] == 0xff)
            return;
        uint8_t *reply = kmalloc(length);
        if (!reply)
            return;
        memcpy(reply, p, length);
        reply[0] = 129;
        raw_send(ifc, dst, src, ICMP6, 64, reply, length);
        kfree(reply);
        break;
    }
    case 129: net_ping_answer(rd16(p + 4), rd16(p + 6), hops); break;
    case 134:
        if (hops == 255 && link_local(src))
            router_advert(ifc, v, src, p, length);
        break;
    case 135: {
        if (hops != 255 || length < 24 || !ours(v, p + 8))
            return;
        size_t n;
        const uint8_t *o = option(p, length, 24, 1, &n);
        if (zero16(src))
            return;
        if (o && n >= 8)
            neighbor_learn(ifc, src, o + 2);
        uint8_t na[32] = {136};
        na[4] = 0x60;
        memcpy(na + 8, p + 8, 16);
        na[24] = 2;
        na[25] = 1;
        memcpy(na + 26, ifc->dev->mac, 6);
        raw_send(ifc, p + 8, src, ICMP6, 255, na, sizeof(na));
        break;
    }
    case 136: {
        if (hops != 255 || length < 24)
            return;
        size_t n;
        const uint8_t *o = option(p, length, 24, 2, &n);
        if (o && n >= 8 && neighbor_find(p + 8, false))
            neighbor_learn(ifc, p + 8, o + 2);
        break;
    }
    }
}

void ip6_input(struct net_iface *ifc, const uint8_t *p, size_t length)
{
    if (v6_off || length < IP6_HEADER || (p[0] >> 4) != 6)
        return;
    size_t total = IP6_HEADER + rd16(p + 4);
    if (total > length)
        return;
    struct net6 *v = v6_for(ifc);
    const uint8_t *src = p + 8, *dst = p + 24;
    if (!v || src[0] == 0xff)
        return;
    bool mine = ours(v, dst) || !memcmp(dst, all_nodes, 16) ||
                (dst[0] == 0xff && dst[1] == 2 && dst[11] == 1 && dst[12] == 0xff && !memcmp(dst + 13, v->ll + 13, 3));
    if (!mine)
        return;
    uint8_t next = p[6];
    size_t at = IP6_HEADER;
    while ((next == 0 || next == 43 || next == 60) && at + 8 <= total) {
        next = p[at];
        at += ((size_t)p[at + 1] + 1) * 8;
    }
    if (at > total)
        return;
    const uint8_t *payload = p + at;
    size_t n = total - at;
    if (next == ICMP6) {
        icmp6_input(ifc, src, dst, payload, n, p[7]);
        return;
    }
    if (next != IP_TCP && next != IP_UDP)
        return;
    uint32_t sh = net_v6_handle(src), dh = net_v6_handle(dst[0] == 0xff ? v->ll : dst);
    if (!sh || !dh)
        return;
    if (next == IP_TCP)
        tcp_input(sh, dh, payload, n);
    else
        udp_input(sh, dh, payload, n);
}

/* The link-local address from the MAC (EUI-64), then router solicitations until a router answers. */
void ip6_start(struct net_iface *ifc)
{
    struct net6 *v = v6_for(ifc);
    if (!v || v->ll_ok)
        return;
    const uint8_t *m = ifc->dev->mac;
    uint8_t ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, (uint8_t)(m[0] ^ 2), m[1], m[2], 0xff, 0xfe, m[3], m[4], m[5]};
    memcpy(v->ll, ll, 16);
    v->ll_ok = true;
    v->ll_h = net_v6_handle(v->ll);
    v->rs_at = 0;
    v->rs_count = 0;
}

void ip6_tick(uint64_t now)
{
    if (v6_off)
        return;
    for (int i = 0; i < net_interface_count(); i++) {
        struct net6 *v = &v6[i];
        struct net_iface *ifc = net_interface(i);
        if (!v->ll_ok || v->router_ok || v->rs_count >= 3 || (v->rs_at && now - v->rs_at < 4000))
            continue;
        if (ifc->dev->link && !ifc->dev->link(ifc->dev))
            continue;
        v->rs_at = now;
        v->rs_count++;
        uint8_t rs[16] = {133};
        rs[8] = 1;
        rs[9] = 1;
        memcpy(rs + 10, ifc->dev->mac, 6);
        raw_send(ifc, v->ll, all_routers, ICMP6, 255, rs, sizeof(rs));
    }
}

static void get_ipv6(char *buf, size_t cap)
{
    str_copy(buf, v6_off ? "off" : "on", cap);
}

static int set_ipv6(struct shell *sh, const char *value)
{
    UNUSED(sh);
    bool on;
    int err = setting_flag(value, &on);
    if (err)
        return err;
    v6_off = !on;
    for (int i = 0; on && i < NET_MAX_IFACES; i++)
        v6[i].rs_count = 0;
    return 0;
}

static const struct setting s_ipv6 = {"ipv6", "on|off", "IPv6 beside IPv4 (addresses from the router)", get_ipv6,
                                      set_ipv6};
SETTING(s_ipv6);
