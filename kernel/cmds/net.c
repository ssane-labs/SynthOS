#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>

static int number(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static int net_fail(struct shell *sh, const char *who, const char *what, int err)
{
    return sh_error(sh, who, "%s: %s", what, err_str(err));
}

static void print_iface(struct shell *sh, struct net_iface *ifc)
{
    struct netdev *d = ifc->dev;
    bool up = !d->link || d->link(d);
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%-6s", d->name);
    sh_role(sh, ROLE_TEXT);
    sh_printf(sh, " %-8s %02x:%02x:%02x:%02x:%02x:%02x  link %s\n", d->driver, d->mac[0], d->mac[1], d->mac[2],
              d->mac[3], d->mac[4], d->mac[5], up ? "up" : "down");
    char ip[16], mask[16], gw[16], dns[16];
    net_format_ip(ifc->ip, ip, sizeof(ip));
    net_format_ip(ifc->mask, mask, sizeof(mask));
    net_format_ip(ifc->gateway, gw, sizeof(gw));
    net_format_ip(ifc->dns, dns, sizeof(dns));
    if (ifc->configured)
        sh_printf(sh, "       inet %s netmask %s gateway %s dns %s%s\n", ip, mask, gw, dns,
                  ifc->dhcp ? " (dhcp)" : "");
    else
        sh_printf(sh, "       not configured%s\n", ifc->dhcp ? " (waiting for dhcp)" : "");
    char rx[16], tx[16];
    fmt_size(rx, sizeof(rx), ifc->rx_bytes);
    fmt_size(tx, sizeof(tx), ifc->tx_bytes);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "       rx %llu packets %s, tx %llu packets %s\n", (unsigned long long)ifc->rx_packets, rx,
              (unsigned long long)ifc->tx_packets, tx);
    sh_role(sh, ROLE_TEXT);
}

static struct net_iface *find_iface(const char *name)
{
    for (int i = 0; i < net_interface_count(); i++)
        if (!strcmp(net_interface(i)->dev->name, name))
            return net_interface(i);
    return NULL;
}

COMMAND(ifconfig, "ip", "system", "ifconfig [iface [address netmask gateway [dns]]]",
        "show or set network interfaces")
{
    if (argc == 1) {
        if (!net_interface_count()) {
            sh_puts(sh, "no network interfaces (loopback 127.0.0.1 only)\n");
            return 0;
        }
        for (int i = 0; i < net_interface_count(); i++)
            print_iface(sh, net_interface(i));
        return 0;
    }
    struct net_iface *ifc = find_iface(argv[1]);
    if (!ifc)
        return sh_error(sh, "ifconfig", "%s: no such interface", argv[1]);
    if (argc == 2) {
        print_iface(sh, ifc);
        return 0;
    }
    uint32_t ip, mask, gw = 0, dns = 0;
    if (argc < 4 || !net_parse_ip(argv[2], &ip) || !net_parse_ip(argv[3], &mask) ||
        (argc > 4 && !net_parse_ip(argv[4], &gw)) || (argc > 5 && !net_parse_ip(argv[5], &dns)))
        return command_usage(sh, command_find("ifconfig"));
    ifc->dhcp = false;
    net_configure(ifc, ip, mask, gw, dns ? dns : gw);
    print_iface(sh, ifc);
    return 0;
}

COMMAND(dhcp, NULL, "system", "dhcp [iface]", "get a network address from a DHCP server")
{
    struct net_iface *ifc = argc > 1 ? find_iface(argv[1]) : net_interface(0);
    if (!ifc)
        return sh_error(sh, "dhcp", "no network interface");
    int err = net_dhcp(ifc, 8000);
    if (err)
        return net_fail(sh, "dhcp", ifc->dev->name, err);
    print_iface(sh, ifc);
    return 0;
}

static int resolve(struct shell *sh, const char *who, const char *host, uint32_t *ip)
{
    int err = net_resolve(host, ip, 4000);
    if (err == E_NOENT)
        sh_error(sh, who, "%s: host not found", host);
    else if (err)
        net_fail(sh, who, host, err);
    return err;
}

COMMAND(ping, NULL, "system", "ping [-c count] host", "check that a host answers")
{
    int count = 4, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-c")) {
        count = MAX(number(argv[i + 1]), 1);
        i += 2;
    }
    if (i >= argc)
        return command_usage(sh, command_find("ping"));
    uint32_t ip;
    if (resolve(sh, "ping", argv[i], &ip))
        return 1;
    char text[16];
    net_format_ip(ip, text, sizeof(text));
    sh_printf(sh, "PING %s (%s) 56 bytes of data\n", argv[i], text);
    int received = 0;
    uint32_t total = 0, best = 0xffffffffu, worst = 0;
    for (int seq = 1; seq <= count && !sh_interrupted(); seq++) {
        uint32_t rtt = 0;
        uint8_t ttl = 0;
        uint64_t start = hal_ticks_ms();
        int err = net_ping(ip, (uint16_t)seq, 56, 2000, &rtt, &ttl);
        if (err) {
            sh_printf(sh, "icmp_seq=%d %s\n", seq, err == E_TIMEDOUT ? "timed out" : err_str(err));
        } else {
            received++;
            total += rtt;
            best = MIN(best, rtt);
            worst = MAX(worst, rtt);
            sh_printf(sh, "64 bytes from %s: icmp_seq=%d ttl=%u time=%u ms\n", text, seq, ttl, rtt);
        }
        while (seq < count && hal_ticks_ms() - start < 1000 && !sh_interrupted())
            net_wait(5);
    }
    sh_printf(sh, "%d packets sent, %d received, %d%% loss", count, received, (count - received) * 100 / count);
    if (received)
        sh_printf(sh, ", rtt min/avg/max %u/%u/%u ms", best, total / (uint32_t)received, worst);
    sh_puts(sh, "\n");
    return received ? 0 : 1;
}

COMMAND(nslookup, "host", "system", "nslookup name", "look up the address of a host name")
{
    if (argc != 2)
        return command_usage(sh, command_find("nslookup"));
    uint32_t ip;
    if (resolve(sh, "nslookup", argv[1], &ip))
        return 1;
    char text[16];
    net_format_ip(ip, text, sizeof(text));
    sh_printf(sh, "%s has address %s\n", argv[1], text);
    return 0;
}

COMMAND(netstat, NULL, "system", "netstat", "list open network connections")
{
    UNUSED(argc);
    UNUSED(argv);
    struct net_socket_info list[NET_MAX_SOCKETS];
    int n = net_sockets(list, NET_MAX_SOCKETS);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "%-5s %-22s %-22s %s\n", "PROTO", "LOCAL", "REMOTE", "STATE");
    sh_role(sh, ROLE_TEXT);
    for (int i = 0; i < n; i++) {
        char l[16], r[16], la[24], ra[24];
        net_format_ip(list[i].local_ip, l, sizeof(l));
        net_format_ip(list[i].remote_ip, r, sizeof(r));
        ksnprintf(la, sizeof(la), "%s:%u", l, list[i].local_port);
        ksnprintf(ra, sizeof(ra), list[i].remote_port ? "%s:%u" : "*", r, list[i].remote_port);
        sh_printf(sh, "%-5s %-22s %-22s %s\n", list[i].type == NET_TCP ? "tcp" : "udp", la, ra,
                  list[i].type == NET_TCP ? net_state_name(list[i].state) : "");
    }
    return 0;
}

struct url {
    char host[128];
    char path[512];
    uint16_t port;
};

static bool parse_url(const char *s, struct url *u, bool *https)
{
    *https = str_starts(s, "https://");
    if (str_starts(s, "http://"))
        s += 7;
    else if (*https)
        s += 8;
    const char *slash = strchr(s, '/');
    size_t host_len = slash ? (size_t)(slash - s) : strlen(s);
    if (!host_len || host_len >= sizeof(u->host))
        return false;
    memcpy(u->host, s, host_len);
    u->host[host_len] = 0;
    u->port = *https ? 443 : 80;
    char *colon = strchr(u->host, ':');
    if (colon) {
        *colon = 0;
        u->port = (uint16_t)number(colon + 1);
    }
    str_copy(u->path, slash ? slash : "/", sizeof(u->path));
    return true;
}

struct conn {
    int sock;
    struct tls *tls;
};

static long conn_recv(struct conn *c, void *buf, size_t n)
{
    return c->tls ? tls_read(c->tls, buf, n) : net_recv(c->sock, buf, n);
}

static int conn_send(struct conn *c, const void *buf, size_t n)
{
    if (c->tls) {
        long k = tls_write(c->tls, buf, n);
        return k < 0 ? (int)k : 0;
    }
    const char *p = buf;
    while (n) {
        long k = net_send(c->sock, p, n);
        if (k == E_AGAIN) {
            net_wait(1);
            continue;
        }
        if (k < 0)
            return (int)k;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static void conn_close(struct conn *c)
{
    tls_close(c->tls);
    net_close(c->sock);
}

static long read_all(struct conn *c, char *buf, size_t cap, struct shell *sh)
{
    size_t got = 0;
    uint64_t last = hal_ticks_ms();
    while (got < cap && !sh_interrupted()) {
        long n = conn_recv(c, buf + got, cap - got);
        if (n > 0) {
            got += (size_t)n;
            last = hal_ticks_ms();
            continue;
        }
        if (n == 0)
            break;
        if (n != E_AGAIN)
            return got ? (long)got : n;
        if (hal_ticks_ms() - last > 15000)
            return E_TIMEDOUT;
        net_wait(1);
    }
    return (long)got;
}

static bool header_has(const char *headers, const char *name, const char *value)
{
    size_t n = strlen(name);
    for (const char *p = headers; (p = strchr(p, '\n')); ) {
        p++;
        if (str_ncasecmp(p, name, n) == 0 && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ')
                v++;
            size_t k = 0;
            while (v[k] && v[k] != '\r' && v[k] != '\n')
                k++;
            for (size_t i = 0; i + strlen(value) <= k; i++)
                if (str_ncasecmp(v + i, value, strlen(value)) == 0)
                    return true;
        }
    }
    return false;
}

static size_t dechunk(char *body, size_t length)
{
    size_t in = 0, out = 0;
    while (in < length) {
        size_t size = 0;
        while (in < length && is_xdigit(body[in])) {
            int c = body[in++];
            size = size * 16 + (size_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        while (in < length && body[in] != '\n')
            in++;
        in++;
        if (!size || in >= length)
            break;
        size = MIN(size, length - in);
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    return out;
}

static int tls_fail(struct shell *sh, const char *host, int err)
{
    return sh_error(sh, "wget", "%s: %s", host, tls_strerror(err));
}

static int fetch(struct shell *sh, const char *address, const char *output, bool verify, int depth)
{
    struct url u;
    bool https;
    if (!parse_url(address, &u, &https))
        return sh_error(sh, "wget", "%s: bad address", address);
    uint32_t ip;
    if (resolve(sh, "wget", u.host, &ip))
        return 1;
    char text[16];
    net_format_ip(ip, text, sizeof(text));
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "Connecting to %s (%s):%u... ", u.host, text, u.port);
    sh_role(sh, ROLE_TEXT);
    struct conn c = {net_socket(NET_TCP), NULL};
    if (c.sock < 0)
        return net_fail(sh, "wget", "socket", c.sock);
    int err = net_connect(c.sock, ip, u.port, 8000);
    if (err) {
        sh_puts(sh, "\n");
        net_close(c.sock);
        return net_fail(sh, "wget", u.host, err);
    }
    sh_puts(sh, "connected.\n");
    if (https) {
        c.tls = tls_connect(c.sock, u.host, verify, &err);
        if (!c.tls) {
            net_close(c.sock);
            return tls_fail(sh, u.host, err);
        }
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%s, %s, certificate for %s%s\n", tls_version(c.tls), tls_cipher(c.tls),
                  tls_peer_name(c.tls), verify ? " (verified)" : " (not verified)");
        sh_role(sh, ROLE_TEXT);
    }
    char request[768];
    int n = ksnprintf(request, sizeof(request),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SynthOS-wget/1.1\r\nAccept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      u.path, u.host);
    if ((err = conn_send(&c, request, (size_t)n))) {
        conn_close(&c);
        return net_fail(sh, "wget", "send", err);
    }
    size_t cap = 8u << 20;
    char *buf = kmalloc(cap + 1);
    if (!buf) {
        conn_close(&c);
        return net_fail(sh, "wget", "buffer", E_NOMEM);
    }
    long got = read_all(&c, buf, cap, sh);
    conn_close(&c);
    if (got < 0) {
        kfree(buf);
        return https && got <= TLS_ERR_PROTOCOL ? tls_fail(sh, u.host, (int)got) : net_fail(sh, "wget", "receive", (int)got);
    }
    buf[got] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    int status = 0;
    if (str_starts(buf, "HTTP/") && strchr(buf, ' '))
        status = number(strchr(buf, ' ') + 1);
    if (!body || !status) {
        kfree(buf);
        return sh_error(sh, "wget", "%s: not an HTTP response", u.host);
    }
    body[2] = 0;
    body += 4;
    size_t length = (size_t)(buf + got - body);
    if (header_has(buf, "Transfer-Encoding", "chunked"))
        length = dechunk(body, length);
    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && depth < 4) {
        char *loc = strstr(buf, "\nLocation: ");
        if (!loc)
            loc = strstr(buf, "\nlocation: ");
        if (loc) {
            char next[640];
            loc += 11;
            size_t k = 0;
            while (loc[k] && loc[k] != '\r' && loc[k] != '\n' && k + 1 < sizeof(next))
                k++;
            if (loc[0] == '/')
                ksnprintf(next, sizeof(next), "%s://%s:%u%.*s", https ? "https" : "http", u.host, u.port, (int)k,
                          loc);
            else
                ksnprintf(next, sizeof(next), "%.*s", (int)k, loc);
            sh_printf(sh, "HTTP %d, redirected to %s\n", status, next);
            kfree(buf);
            return fetch(sh, next, output, verify, depth + 1);
        }
    }
    sh_printf(sh, "HTTP request sent, response %d\n", status);
    if (status >= 400) {
        kfree(buf);
        return 1;
    }
    char name[PATH_MAX_LEN];
    if (output) {
        str_copy(name, output, sizeof(name));
    } else {
        const char *base = path_basename(u.path);
        str_copy(name, base && *base && strcmp(base, "/") ? base : "index.html", sizeof(name));
        char *q = strchr(name, '?');
        if (q)
            *q = 0;
    }
    if (!strcmp(name, "-")) {
        stream_write(sh->out, body, length);
        kfree(buf);
        return 0;
    }
    char path[PATH_MAX_LEN];
    err = sh_resolve(sh, name, path, sizeof(path));
    if (!err)
        err = vfs_write_all(path, body, length);
    kfree(buf);
    if (err)
        return sh_fs_error(sh, "wget", name, err);
    char size[16];
    fmt_size(size, sizeof(size), length);
    sh_printf(sh, "Saved %s (%s)\n", name, size);
    return 0;
}

COMMAND(wget, "fetch", "system", "wget [-k] [-O file] http[s]://host[:port]/path",
        "download a file over HTTP or HTTPS (-k: do not check the certificate)")
{
    const char *output = NULL;
    bool verify = true;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--insecure"))
            verify = false;
        else if (!strcmp(argv[i], "-O") && i + 1 < argc)
            output = argv[++i];
        else
            return command_usage(sh, command_find("wget"));
    }
    if (i >= argc)
        return command_usage(sh, command_find("wget"));
    return fetch(sh, argv[i], output, verify, 0);
}

COMMAND(certs, NULL, "system", "certs [reload|list]", "show the trusted HTTPS root certificates")
{
    if (argc > 1 && !strcmp(argv[1], "reload"))
        tls_roots_load();
    int n = tls_roots_count();
    if (argc > 1 && !strcmp(argv[1], "list")) {
        for (int i = 0; i < n; i++) {
            const struct x509 *c = tls_root(i);
            sh_printf(sh, "%-4s %s\n", c->key == KEY_RSA ? "RSA" : c->key == KEY_NONE ? "?" : "EC", c->cn);
        }
    }
    sh_printf(sh, "%d trusted root certificates (/etc/certs, /home/certs)\n", n);
    return 0;
}

COMMAND(httpd, "serve", "system", "httpd [port] [directory]", "serve a directory over HTTP until Ctrl+C")
{
    uint16_t port = argc > 1 ? (uint16_t)number(argv[1]) : 8080;
    char root[PATH_MAX_LEN];
    int err = sh_resolve(sh, argc > 2 ? argv[2] : ".", root, sizeof(root));
    if (err || !vfs_is_dir(root))
        return sh_error(sh, "httpd", "%s: not a directory", argc > 2 ? argv[2] : ".");
    int ls = net_socket(NET_TCP);
    if (ls < 0 || (err = net_bind(ls, 0, port)) || (err = net_listen(ls, 8))) {
        if (ls >= 0)
            net_close(ls);
        return net_fail(sh, "httpd", "listen", ls < 0 ? ls : err);
    }
    sh_printf(sh, "Serving %s on port %u, press Ctrl+C to stop\n", root, port);
    while (!sh_interrupted()) {
        uint32_t ip;
        uint16_t rport;
        int c = net_accept(ls, &ip, &rport);
        if (c == E_AGAIN) {
            net_wait(2);
            continue;
        }
        if (c < 0)
            break;
        char req[1024];
        size_t got = 0;
        uint64_t start = hal_ticks_ms();
        while (got < sizeof(req) - 1 && hal_ticks_ms() - start < 3000) {
            long n = net_recv(c, req + got, sizeof(req) - 1 - got);
            if (n > 0) {
                got += (size_t)n;
                req[got] = 0;
                if (strstr(req, "\r\n\r\n"))
                    break;
            } else if (n != E_AGAIN) {
                break;
            } else {
                net_wait(1);
            }
        }
        req[got] = 0;
        char target[PATH_MAX_LEN] = "/";
        if (str_starts(req, "GET ")) {
            const char *p = req + 4;
            size_t k = 0;
            while (p[k] && p[k] != ' ' && k + 1 < sizeof(target))
                k++;
            memcpy(target, p, k);
            target[k] = 0;
        }
        char path[PATH_MAX_LEN];
        path_join(root, target[0] == '/' ? target + 1 : target, path, sizeof(path));
        if (vfs_is_dir(path)) {
            char index[PATH_MAX_LEN];
            path_join(path, "index.html", index, sizeof(index));
            str_copy(path, index, sizeof(path));
        }
        char *data = NULL;
        size_t size = 0;
        bool ok = !strstr(target, "..") && vfs_read_all(path, &data, &size) == 0;
        char head[160];
        int hn = ok ? ksnprintf(head, sizeof(head),
                                "HTTP/1.0 200 OK\r\nContent-Length: %u\r\nServer: SynthOS\r\nConnection: close\r\n\r\n",
                                (unsigned)size)
                    : ksnprintf(head, sizeof(head),
                                "HTTP/1.0 404 Not Found\r\nContent-Length: 10\r\nConnection: close\r\n\r\nnot found\n");
        const char *parts[2] = {head, ok ? data : ""};
        size_t lengths[2] = {(size_t)hn, ok ? size : 0};
        for (int part = 0; part < 2; part++)
            for (size_t sent = 0; sent < lengths[part] && !sh_interrupted();) {
                long k = net_send(c, parts[part] + sent, lengths[part] - sent);
                if (k == E_AGAIN) {
                    net_wait(1);
                    continue;
                }
                if (k < 0)
                    break;
                sent += (size_t)k;
            }
        kfree(data);
        char from[16];
        net_format_ip(ip, from, sizeof(from));
        sh_printf(sh, "%s GET %s %d\n", from, target, ok ? 200 : 404);
        net_shutdown(c);
        net_close(c);
    }
    net_close(ls);
    return 0;
}
