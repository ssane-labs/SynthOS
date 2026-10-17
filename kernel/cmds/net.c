#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/input.h>
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

static bool cancelled(void)
{
    return input_break_pending();
}

struct wget_sink {
    struct shell *sh;
    struct file *f;
    uint64_t bytes, shown;
};

static int wget_write(void *ctx, const void *data, size_t n)
{
    struct wget_sink *w = ctx;
    w->bytes += n;
    return vfs_write(w->f, data, n) == (long)n ? 0 : E_NOSPC;
}

static void wget_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    struct wget_sink *w = ctx;
    if (strcmp(stage, "downloading") || done < w->shown + (4u << 20))
        return;
    w->shown = done;
    char a[16], b[16];
    fmt_size(a, sizeof(a), done);
    fmt_size(b, sizeof(b), total);
    if (total)
        sh_printf(w->sh, "\r  %s of %s (%u%%)  ", a, b, (unsigned)((uint64_t)done * 100 / total));
    else
        sh_printf(w->sh, "\r  %s  ", a);
}

COMMAND(wget, "fetch", "system", "wget [-k] [-O file] http[s]://host[:port]/path",
        "download a file over HTTP or HTTPS (-k: do not check the certificate)")
{
    const char *output = NULL;
    unsigned flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--insecure"))
            flags |= HTTP_INSECURE;
        else if (!strcmp(argv[i], "-O") && i + 1 < argc)
            output = argv[++i];
        else
            return command_usage(sh, command_find("wget"));
    }
    if (i >= argc)
        return command_usage(sh, command_find("wget"));
    char url[HTTP_URL_MAX];
    if (strstr(argv[i], "://"))
        str_copy(url, argv[i], sizeof(url));
    else
        ksnprintf(url, sizeof(url), "http://%s", argv[i]);
    struct http_url u;
    if (!http_parse_url(url, &u))
        return sh_error(sh, "wget", "%s: bad address", argv[i]);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "Fetching %s://%s:%u%s\n", u.https ? "https" : "http", u.host, u.port, u.path);
    sh_role(sh, ROLE_TEXT);
    char name[PATH_MAX_LEN];
    if (output) {
        str_copy(name, output, sizeof(name));
    } else {
        char *q = strchr(u.path, '?');
        if (q)
            *q = 0;
        const char *base = path_basename(u.path);
        str_copy(name, base && *base && strcmp(base, "/") ? base : "index.html", sizeof(name));
    }
    bool to_stdout = !strcmp(name, "-");
    struct wget_sink ws = {.sh = sh};
    char path[PATH_MAX_LEN];
    if (!to_stdout) {
        int err = sh_resolve(sh, name, path, sizeof(path));
        if (!err)
            err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &ws.f);
        if (err)
            return sh_fs_error(sh, "wget", name, err);
    }
    struct http_request req = {.url = url, .flags = flags | (to_stdout ? 0 : HTTP_GZIP), .cancelled = cancelled,
                               .sink = to_stdout ? NULL : wget_write, .progress = to_stdout ? NULL : wget_progress,
                               .ctx = &ws};
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    if (ws.shown)
        sh_puts(sh, "\r                                        \r");
    if (!err && !to_stdout && !ws.bytes && resp.status < 400 && resp.length)
        err = wget_write(&ws, resp.body, resp.length);
    if (ws.f)
        vfs_close(ws.f);
    if (err || (!to_stdout && resp.status >= 400))
        if (!to_stdout)
            vfs_unlink(path);
    if (err == E_NOENT)
        return sh_error(sh, "wget", "%s: host not found", u.host);
    if (err)
        return sh_error(sh, "wget", "%s: %s", u.host, tls_strerror(err));
    if (resp.tls[0]) {
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%s%s\n", resp.tls, flags & HTTP_INSECURE ? " (not verified)" : " (verified)");
        sh_role(sh, ROLE_TEXT);
    }
    if (strcmp(resp.url, url))
        sh_printf(sh, "Redirected to %s\n", resp.url);
    sh_printf(sh, "HTTP request sent, response %d\n", resp.status);
    if (resp.status >= 400) {
        http_free(&resp);
        return 1;
    }
    if (to_stdout) {
        stream_write(sh->out, resp.body, resp.length);
        http_free(&resp);
        return 0;
    }
    http_free(&resp);
    char size[16];
    fmt_size(size, sizeof(size), ws.bytes);
    sh_printf(sh, "Saved %s (%s)\n", name, size);
    return 0;
}

COMMAND(certs, NULL, "system", "certs [reload|list]", "show the trusted HTTPS root certificates")
{
    if (argc > 1 && !strcmp(argv[1], "reload"))
        tls_roots_load();
    int n = tls_roots_count();
    if (argc > 1 && !strcmp(argv[1], "list")) {
        for (int i = 0; i < n; i++) {
            const struct x509 *c = tls_root(i);
            sh_printf(sh, "%-4s %s\n", c->key == PK_RSA ? "RSA" : c->key == PK_NONE ? "?" : "EC", c->cn);
        }
    }
    sh_printf(sh, "%d trusted root certificates (/etc/certs, /home/certs)\n", n);
    return 0;
}

COMMAND(cookies, NULL, "system", "cookies [clear]", "show or forget the cookies web sites have set")
{
    if (argc > 1 && !strcmp(argv[1], "clear"))
        http_cookies_clear();
    else if (argc > 1)
        return command_usage(sh, command_find("cookies"));
    int n = http_cookie_count();
    sh_printf(sh, "%d cookie%s stored\n", n, n == 1 ? "" : "s");
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
