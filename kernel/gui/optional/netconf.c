/* Network settings beyond the basics: the system proxy and DHCPv6.
 *
 * The proxy (`proxy set http://[user:password@]host:port`) carries every download the system makes (the store,
 * updates, feeds, wget, the language pack): HTTPS through a CONNECT tunnel, so the proxy never sees inside, plain
 * HTTP as absolute requests. Hosts on the bypass list, the computer itself and the local network go direct. The
 * setting lives in /home/etc/proxy.conf. */
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/module.h>
#include <synth/net.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "../internal.h"

#define PROXY_FILE "/home/etc/proxy.conf"

/* ---- the proxy ---- */

static struct {
    bool loaded, on;
    char host[128];
    uint16_t port;
    char auth[200]; /* "Basic ..." */
    char user[64];
    char bypass[512];
} proxy;

static void base64(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = abc[v >> 18 & 63];
        out[o++] = abc[v >> 12 & 63];
        out[o++] = i + 1 < n ? abc[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? abc[v & 63] : '=';
    }
    out[o] = 0;
}

/* "http://user:password@host:port" (the scheme and the credentials are optional) */
static bool parse_proxy(const char *url, char *why, size_t cap)
{
    char tmp[300];
    str_copy(tmp, url, sizeof(tmp));
    char *p = tmp;
    if (!strncmp(p, "http://", 7))
        p += 7;
    else if (strstr(p, "://")) {
        ksnprintf(why, cap, "only HTTP proxies are supported (http://host:port)");
        return false;
    }
    char *slash = strchr(p, '/');
    if (slash)
        *slash = 0;
    proxy.auth[0] = proxy.user[0] = 0;
    char *at = strrchr(p, '@');
    if (at) {
        *at = 0;
        char basic[160];
        base64((const uint8_t *)p, strlen(p), basic, sizeof(basic));
        ksnprintf(proxy.auth, sizeof(proxy.auth), "Basic %s", basic);
        str_copy(proxy.user, p, sizeof(proxy.user));
        char *colon = strchr(proxy.user, ':');
        if (colon)
            *colon = 0;
        p = at + 1;
    }
    char *colon = strrchr(p, ':');
    long port = 8080;
    if (colon && !strchr(colon, ']')) {
        *colon = 0;
        port = str_to_long(colon + 1, NULL, 10);
    }
    if (!p[0] || port <= 0 || port > 65535) {
        ksnprintf(why, cap, "give the proxy as http://host:port");
        return false;
    }
    if (p[0] == '[') {
        p++;
        char *close = strchr(p, ']');
        if (close)
            *close = 0;
    }
    str_copy(proxy.host, p, sizeof(proxy.host));
    proxy.port = (uint16_t)port;
    proxy.on = true;
    return true;
}

static void load_proxy(void)
{
    proxy.loaded = true;
    proxy.on = false;
    proxy.bypass[0] = 0;
    char *text;
    size_t len;
    if (vfs_read_all(PROXY_FILE, &text, &len) != 0)
        return;
    char why[80];
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        if (!strncmp(line, "proxy=", 6))
            parse_proxy(line + 6, why, sizeof(why));
        else if (!strncmp(line, "bypass=", 7))
            str_copy(proxy.bypass, line + 7, sizeof(proxy.bypass));
        line = end ? end + 1 : NULL;
    }
    kfree(text);
}

static int save_proxy(const char *url)
{
    char text[900];
    int n = ksnprintf(text, sizeof(text), "# the system proxy (proxy set / proxy off)\n");
    if (url)
        n += ksnprintf(text + n, sizeof(text) - (size_t)n, "proxy=%s\n", url);
    if (proxy.bypass[0])
        n += ksnprintf(text + n, sizeof(text) - (size_t)n, "bypass=%s\n", proxy.bypass);
    vfs_mkdirs("/home/etc");
    return vfs_write_all(PROXY_FILE, text, (size_t)n);
}

/* the bypass list, comma or space separated: "example.com" (and its subdomains), "*.lan", "10.1.2.3", "<local>"
   for names without a dot; the computer itself always goes direct */
static bool bypassed(const char *host)
{
    if (!str_casecmp(host, "localhost") || !strncmp(host, "127.", 4) || !strcmp(host, "::1"))
        return true;
    const char *p = proxy.bypass;
    while (*p) {
        while (*p == ',' || *p == ' ')
            p++;
        char item[128];
        size_t n = 0;
        while (*p && *p != ',' && *p != ' ' && n + 1 < sizeof(item))
            item[n++] = *p++;
        item[n] = 0;
        if (!n)
            break;
        if (!strcmp(item, "<local>")) {
            if (!strchr(host, '.'))
                return true;
            continue;
        }
        const char *suffix = item[0] == '*' ? item + 1 : item;
        size_t hl = strlen(host), sl = strlen(suffix);
        if (sl && (!str_casecmp(host, suffix[0] == '.' ? suffix + 1 : suffix) ||
                   (hl > sl && !str_casecmp(host + hl - sl, suffix) && (suffix[0] == '.' || host[hl - sl - 1] == '.'))))
            return true;
    }
    return false;
}

static bool route(const struct http_url *u, char *host, size_t cap, uint16_t *port, char *auth, size_t auth_cap)
{
    if (!proxy.loaded)
        load_proxy();
    if (!proxy.on || bypassed(u->host))
        return false;
    str_copy(host, proxy.host, cap);
    *port = proxy.port;
    str_copy(auth, proxy.auth, auth_cap);
    return true;
}

static const struct http_proxy proxy_service = {route};

COMMAND(proxy, NULL, "net", "proxy [set http://[user:password@]host:port | bypass HOSTS... | off]",
        "the proxy every download of the system goes through")
{
    if (!proxy.loaded)
        load_proxy();
    char why[120];
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        if (!parse_proxy(argv[2], why, sizeof(why))) {
            load_proxy();
            return sh_error(sh, "proxy", "%s", why);
        }
        if (save_proxy(argv[2]))
            return sh_error(sh, "proxy", "can't write %s", PROXY_FILE);
    } else if (argc >= 2 && !strcmp(argv[1], "bypass")) {
        proxy.bypass[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(proxy.bypass, ",", sizeof(proxy.bypass));
            str_append(proxy.bypass, argv[i], sizeof(proxy.bypass));
        }
        /* keep the proxy line as it was */
        char *text = NULL, url[300] = "";
        size_t len;
        if (vfs_read_all(PROXY_FILE, &text, &len) == 0) {
            char *at = strstr(text, "proxy=");
            if (at) {
                char *end = strchr(at, '\n');
                if (end)
                    *end = 0;
                str_copy(url, at + 6, sizeof(url));
            }
            kfree(text);
        }
        if (save_proxy(url[0] ? url : NULL))
            return sh_error(sh, "proxy", "can't write %s", PROXY_FILE);
    } else if (argc == 2 && !strcmp(argv[1], "off")) {
        proxy.on = false;
        save_proxy(NULL);
    } else if (argc != 1) {
        return command_usage(sh, command_find("proxy"));
    }
    if (proxy.on)
        sh_printf(sh, "proxy: http://%s%s%s:%u%s\n", proxy.user, proxy.user[0] ? "@" : "", proxy.host, proxy.port,
                  proxy.auth[0] ? " (with a password)" : "");
    else
        sh_printf(sh, "proxy: none, downloads go direct\n");
    if (proxy.bypass[0])
        sh_printf(sh, "direct: %s\n", proxy.bypass);
    return 0;
}

/* ---- DHCPv6 ----
   When the router says so (its M or O flag), or gives no DNS server, ask a DHCPv6 server: stateless
   (Information-Request: DNS servers and search domains) or stateful (Solicit/Request: an address of its own). */

enum { SOLICIT = 1, ADVERTISE = 2, REQUEST = 3, REPLY = 7, INFO_REQUEST = 11 };
enum { OPT_CLIENTID = 1, OPT_SERVERID = 2, OPT_IA_NA = 3, OPT_IAADDR = 5, OPT_ORO = 6, OPT_ELAPSED = 8,
       OPT_STATUS = 13, OPT_RAPID = 14, OPT_DNS = 23, OPT_DOMAINS = 24 };

struct dhcp6_state {
    bool asked, answered, leased;
    uint8_t server[16]; /* the DHCPv6 server's address */
    uint8_t dns[3][16];
    int ndns;
    char domains[128];
    uint8_t addr[16];
    uint32_t valid;
    uint64_t at;
};

static struct dhcp6_state d6[4];

static size_t put_opt(uint8_t *p, uint16_t code, const void *data, uint16_t len)
{
    p[0] = (uint8_t)(code >> 8);
    p[1] = (uint8_t)code;
    p[2] = (uint8_t)(len >> 8);
    p[3] = (uint8_t)len;
    if (len)
        memcpy(p + 4, data, len);
    return 4u + len;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* the option with this code among n bytes of options, or NULL */
static const uint8_t *find_opt(const uint8_t *p, size_t n, uint16_t code, uint16_t *len)
{
    for (size_t at = 0; at + 4 <= n;) {
        uint16_t c = be16(p + at), l = be16(p + at + 2);
        if (at + 4 + l > n)
            break;
        if (c == code) {
            *len = l;
            return p + at + 4;
        }
        at += 4u + l;
    }
    return NULL;
}

/* DNS wire names ("\7example\3com\0") to "example.com, ..." */
static void domain_list(const uint8_t *p, uint16_t n, char *out, size_t cap)
{
    out[0] = 0;
    size_t o = 0;
    for (uint16_t at = 0; at < n && o + 2 < cap;) {
        uint8_t l = p[at++];
        if (!l) {
            if (at < n && o + 3 < cap) {
                out[o++] = ',';
                out[o++] = ' ';
            }
            continue;
        }
        if (o && out[o - 1] != ' ')
            out[o++] = '.';
        for (uint8_t k = 0; k < l && at < n && o + 2 < cap; k++)
            out[o++] = (char)p[at++];
    }
    out[o] = 0;
}

static size_t dhcp6_message(uint8_t *m, int type, const uint8_t xid[3], const uint8_t mac[6], int index,
                            const uint8_t *server_id, uint16_t server_len, const uint8_t *ia, uint16_t ia_len)
{
    m[0] = (uint8_t)type;
    memcpy(m + 1, xid, 3);
    size_t n = 4;
    uint8_t duid[10] = {0, 3, 0, 1}; /* DUID-LL, Ethernet */
    memcpy(duid + 4, mac, 6);
    n += put_opt(m + n, OPT_CLIENTID, duid, sizeof(duid));
    if (server_id)
        n += put_opt(m + n, OPT_SERVERID, server_id, server_len);
    uint8_t zero[2] = {0, 0};
    n += put_opt(m + n, OPT_ELAPSED, zero, 2);
    uint8_t oro[4] = {0, OPT_DNS, 0, OPT_DOMAINS};
    n += put_opt(m + n, OPT_ORO, oro, sizeof(oro));
    if (type == SOLICIT || type == REQUEST) {
        if (ia) {
            n += put_opt(m + n, OPT_IA_NA, ia, ia_len);
        } else {
            uint8_t iana[12] = {0, 0, 0, (uint8_t)(index + 1)}; /* IAID, T1 and T2 left to the server */
            n += put_opt(m + n, OPT_IA_NA, iana, sizeof(iana));
        }
        if (type == SOLICIT)
            n += put_opt(m + n, OPT_RAPID, NULL, 0);
    }
    return n;
}

static void take_reply(struct dhcp6_state *st, struct net6 *v, const uint8_t *opts, size_t n)
{
    uint16_t len;
    const uint8_t *o = find_opt(opts, n, OPT_DNS, &len);
    st->ndns = 0;
    for (uint16_t at = 0; o && at + 16 <= len && st->ndns < 3; at += 16)
        memcpy(st->dns[st->ndns++], o + at, 16);
    if ((o = find_opt(opts, n, OPT_DOMAINS, &len)))
        domain_list(o, len, st->domains, sizeof(st->domains));
    if (st->ndns && !v->dns_ok) { /* the router gave none: use the first */
        memcpy(v->dns, st->dns[0], 16);
        v->dns_ok = true;
        v->dns_h = net_v6_handle(v->dns);
    }
    const uint8_t *ia = find_opt(opts, n, OPT_IA_NA, &len);
    uint16_t alen;
    const uint8_t *addr = ia && len > 12 ? find_opt(ia + 12, len - 12u, OPT_IAADDR, &alen) : NULL;
    if (addr && alen >= 24) {
        memcpy(st->addr, addr, 16);
        st->valid = be32(addr + 20);
        st->leased = true;
        if (!v->global_ok) {
            memcpy(v->global, st->addr, 16);
            v->global_ok = true;
            v->global_h = net_v6_handle(v->global);
        }
    }
    st->answered = true;
    st->at = hal_ticks_ms();
}

/* One exchange on interface i; stateful when the router asks for it. Returns 0 when a server answered. */
static int dhcp6_run(int i, int timeout_ms)
{
    struct net6 *v = net_v6_info(i);
    struct net_iface *ifc = net_interface(i);
    if (!v || !ifc || !v->ll_ok || i >= (int)ARRAY_SIZE(d6))
        return E_INVAL;
    struct dhcp6_state *st = &d6[i];
    st->asked = true;
    int sock = net_socket(NET_UDP);
    if (sock < 0)
        return sock;
    if (net_bind(sock, v->ll_h, 546)) { /* this interface's link-local address: the request leaves through it */
        net_close(sock);
        return E_BUSY;
    }
    static const uint8_t servers[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2};
    uint32_t dst = net_v6_handle(servers);
    bool stateful = v->ra_flags & 0x80;
    uint8_t xid[3] = {(uint8_t)hal_ticks_ms(), (uint8_t)(hal_ticks_ms() >> 8), (uint8_t)i};
    uint8_t m[512], r[1500];
    size_t n = dhcp6_message(m, stateful ? SOLICIT : INFO_REQUEST, xid, ifc->dev->mac, i, NULL, 0, NULL, 0);
    int err = E_TIMEDOUT;
    uint64_t start = hal_ticks_ms(), sent = 0;
    while (hal_ticks_ms() - start < (uint64_t)timeout_ms) {
        if (!sent || hal_ticks_ms() - sent > 1000) {
            net_sendto(sock, m, n, dst, 547);
            sent = hal_ticks_ms();
        }
        uint32_t from;
        uint16_t port;
        long k = net_recvfrom(sock, r, sizeof(r), &from, &port);
        if (k == E_AGAIN || k < 4 || memcmp(r + 1, xid, 3)) {
            net_wait(10);
            continue;
        }
        net_v6_addr(from, st->server);
        uint16_t sid_len;
        const uint8_t *sid = find_opt(r + 4, (size_t)k - 4, OPT_SERVERID, &sid_len);
        if (r[0] == ADVERTISE && sid) { /* stateful: ask that server for the address it offered */
            uint16_t ia_len;
            const uint8_t *ia = find_opt(r + 4, (size_t)k - 4, OPT_IA_NA, &ia_len);
            uint8_t sid_copy[128];
            sid_len = MIN(sid_len, (uint16_t)sizeof(sid_copy));
            memcpy(sid_copy, sid, sid_len);
            uint8_t ia_copy[256];
            ia_len = ia ? MIN(ia_len, (uint16_t)sizeof(ia_copy)) : 0;
            if (ia)
                memcpy(ia_copy, ia, ia_len);
            xid[2] ^= 0x80;
            n = dhcp6_message(m, REQUEST, xid, ifc->dev->mac, i, sid_copy, sid_len, ia ? ia_copy : NULL, ia_len);
            sent = 0;
            continue;
        }
        if (r[0] == REPLY) {
            uint16_t sl;
            const uint8_t *status = find_opt(r + 4, (size_t)k - 4, OPT_STATUS, &sl);
            if (status && sl >= 2 && be16(status) != 0) {
                err = E_PERM;
                break;
            }
            take_reply(st, v, r + 4, (size_t)k - 4);
            err = 0;
            break;
        }
    }
    net_close(sock);
    return err;
}

static void tick(void)
{
    static uint64_t next;
    uint64_t now = hal_ticks_ms();
    if (now < next)
        return;
    next = now + 2000;
    for (int i = 0; i < net_interface_count() && i < (int)ARRAY_SIZE(d6); i++) {
        struct net6 *v = net_v6_info(i);
        /* once the router has spoken: when it asks for DHCPv6, or leaves the DNS server out */
        if (v && v->ll_ok && v->router_ok && !d6[i].asked && ((v->ra_flags & 0xc0) || !v->dns_ok))
            dhcp6_run(i, 3000);
        /* renew a lease at half its life */
        if (d6[i].leased && d6[i].valid != 0xffffffffu && now - d6[i].at > (uint64_t)d6[i].valid * 500)
            dhcp6_run(i, 3000);
    }
}

COMMAND(dhcpv6, NULL, "net", "dhcpv6 [ask]", "IPv6 settings from a DHCPv6 server: DNS servers, search domains, an address")
{
    bool ask = argc > 1 && !strcmp(argv[1], "ask");
    if (argc > 1 && !ask)
        return command_usage(sh, command_find("dhcpv6"));
    int shown = 0;
    for (int i = 0; i < net_interface_count() && i < (int)ARRAY_SIZE(d6); i++) {
        struct net6 *v = net_v6_info(i);
        struct net_iface *ifc = net_interface(i);
        if (!v || !v->ll_ok)
            continue;
        shown++;
        sh_printf(sh, "%s: the router %s\n", ifc->dev->name,
                  !v->router_ok              ? "has not answered yet"
                  : v->ra_flags & 0x80       ? "hands out addresses by DHCPv6 (M)"
                  : v->ra_flags & 0x40       ? "gives other settings by DHCPv6 (O)"
                                             : "uses SLAAC only");
        if (ask || !d6[i].asked) {
            int err = dhcp6_run(i, 4000);
            if (err) {
                sh_printf(sh, "  no DHCPv6 server answered\n");
                continue;
            }
        }
        if (!d6[i].answered) {
            sh_printf(sh, "  no DHCPv6 server answered\n");
            continue;
        }
        char text[48];
        net_format_ip6(d6[i].server, text, sizeof(text));
        sh_printf(sh, "  server %s\n", text);
        for (int k = 0; k < d6[i].ndns; k++) {
            net_format_ip6(d6[i].dns[k], text, sizeof(text));
            sh_printf(sh, "  DNS server %s\n", text);
        }
        if (d6[i].domains[0])
            sh_printf(sh, "  search domains %s\n", d6[i].domains);
        if (d6[i].leased) {
            net_format_ip6(d6[i].addr, text, sizeof(text));
            sh_printf(sh, "  address %s, valid %u s%s\n", text, d6[i].valid,
                      v->global_ok && memcmp(v->global, d6[i].addr, 16) ? " (the SLAAC address stays in use)" : "");
        }
    }
    if (!shown)
        sh_printf(sh, "IPv6 is not up on any interface\n");
    return 0;
}

static const struct command *const commands[] = {&cmddef_proxy, &cmddef_dhcpv6};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, "http-proxy",
                                      &proxy_service};
GUI_APP_FULL(NULL, tick, &ext);
