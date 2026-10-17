#include <synth/error.h>
#include <synth/hal.h>
#include <synth/string.h>

#include "net.h"

#define CACHE 16

struct cached {
    char name[64];
    uint32_t ip;
    uint64_t expires;
};

static struct cached cache[CACHE];
static int cache_next;

static size_t encode(const char *name, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t len = dot ? (size_t)(dot - name) : strlen(name);
        if (!len || len > 63 || n + len + 2 > cap)
            return 0;
        out[n++] = (uint8_t)len;
        memcpy(out + n, name, len);
        n += len;
        name += len;
        if (*name == '.')
            name++;
    }
    out[n++] = 0;
    return n;
}

static size_t skip_name(const uint8_t *p, size_t at, size_t length)
{
    while (at < length) {
        uint8_t len = p[at];
        if (len == 0)
            return at + 1;
        if ((len & 0xc0) == 0xc0)
            return at + 2;
        at += 1 + len;
    }
    return length;
}

static bool parse(const uint8_t *p, size_t length, uint16_t id, uint32_t *ip, uint32_t *ttl)
{
    if (length < 12 || rd16(p) != id || !(p[2] & 0x80) || (p[3] & 0x0f))
        return false;
    uint16_t questions = rd16(p + 4), answers = rd16(p + 6);
    size_t at = 12;
    for (uint16_t i = 0; i < questions; i++)
        at = skip_name(p, at, length) + 4;
    for (uint16_t i = 0; i < answers && at < length; i++) {
        at = skip_name(p, at, length);
        if (at + 10 > length)
            return false;
        uint16_t type = rd16(p + at), cls = rd16(p + at + 2), size = rd16(p + at + 8);
        uint32_t t = rd32(p + at + 4);
        at += 10;
        if (type == 1 && cls == 1 && size == 4 && at + 4 <= length) {
            *ip = rd32(p + at);
            *ttl = t;
            return true;
        }
        at += size;
    }
    return false;
}

static void remember(const char *name, uint32_t ip, uint32_t ttl)
{
    struct cached *c = &cache[cache_next++ % CACHE];
    str_copy(c->name, name, sizeof(c->name));
    c->ip = ip;
    c->expires = net_now() + (uint64_t)MIN(MAX(ttl, 30u), 3600u) * 1000;
}

int net_resolve_start(const char *name, struct dns_query *q, uint32_t *ip)
{
    q->sock = -1;
    if (net_parse_ip(name, ip))
        return 0;
    if (!str_casecmp(name, "localhost")) {
        *ip = LOOPBACK;
        return 0;
    }
    uint64_t now = net_now();
    for (int i = 0; i < CACHE; i++)
        if (cache[i].name[0] && !str_casecmp(cache[i].name, name) && now < cache[i].expires) {
            *ip = cache[i].ip;
            return 0;
        }
    uint32_t server = 0;
    for (int i = 0; i < net_interface_count() && !server; i++)
        if (net_interface(i)->configured)
            server = net_interface(i)->dns;
    if (!server)
        return E_UNREACH;
    memset(q->query, 0, 12);
    q->id = (uint16_t)hal_entropy();
    wr16(q->query, q->id);
    q->query[2] = 0x01;
    wr16(q->query + 4, 1);
    size_t n = encode(name, q->query + 12, sizeof(q->query) - 16);
    if (!n)
        return E_INVAL;
    wr16(q->query + 12 + n, 1);
    wr16(q->query + 14 + n, 1);
    q->length = (uint16_t)(16 + n);
    q->server = server;
    q->attempt = 0;
    str_copy(q->name, name, sizeof(q->name));
    q->sock = net_socket(NET_UDP);
    if (q->sock < 0)
        return q->sock;
    net_sendto(q->sock, q->query, q->length, server, 53);
    q->sent = net_now();
    return 1;
}

int net_resolve_poll(struct dns_query *q, uint32_t *ip, int timeout_ms)
{
    if (q->sock < 0)
        return E_INVAL;
    int err = E_AGAIN;
    uint8_t reply[512];
    uint32_t from;
    uint16_t port;
    long got;
    while (err == E_AGAIN && (got = net_recvfrom(q->sock, reply, sizeof(reply), &from, &port)) > 0) {
        uint32_t ttl = 0;
        if (parse(reply, (size_t)got, q->id, ip, &ttl)) {
            remember(q->name, *ip, ttl);
            err = 0;
        } else if (got >= 4 && rd16(reply) == q->id) {
            err = E_NOENT;
        }
    }
    if (err == E_AGAIN && net_now() - q->sent >= (uint64_t)(timeout_ms / 3 + 1)) {
        if (++q->attempt >= 3) {
            err = E_TIMEDOUT;
        } else {
            net_sendto(q->sock, q->query, q->length, q->server, 53);
            q->sent = net_now();
        }
    }
    if (err != E_AGAIN)
        net_resolve_cancel(q);
    return err;
}

void net_resolve_cancel(struct dns_query *q)
{
    if (q->sock >= 0)
        net_close(q->sock);
    q->sock = -1;
}

int net_resolve(const char *name, uint32_t *ip, int timeout_ms)
{
    struct dns_query q;
    int err = net_resolve_start(name, &q, ip);
    while (err == 1 || err == E_AGAIN) {
        net_wait(2);
        err = net_resolve_poll(&q, ip, timeout_ms);
    }
    return err;
}
