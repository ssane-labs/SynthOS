#include <synth/command.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/module.h>
#include <synth/net.h>

#include "systool.h"

/* Network Utility: the interfaces and their addresses, the devices nearby, ping, name lookup, open connections and
   a test download, each run through the shell command that does it. discover asks the local network who is there:
   mDNS/DNS-SD (printers, AirPlay and Cast receivers, file shares, computers) and SSDP (UPnP media servers, TVs,
   routers). */

enum { P_INFO, P_DEVICES, P_PING, P_LOOKUP, P_CONNECTIONS, P_DOWNLOAD, P_COUNT };

static const struct tool_item items[P_COUNT] = {
    {SYM_NETWORK, "Info", "Network"},  {SYM_GRID, "Devices", NULL},     {SYM_WIFI, "Ping", "Tools"},
    {SYM_SEARCH, "Lookup", NULL},      {SYM_LIST, "Connections", NULL}, {SYM_DOWNLOAD, "Download Test", NULL},
};

static const char *const hints[P_COUNT] = {
    "Interfaces, addresses and the DNS server",
    "Printers, screens, speakers and shares that announce themselves",
    "Checks that a host answers and how fast",
    "Finds the address of a host name",
    "The connections that are open now",
    "Downloads a page and shows how long it took",
};

struct netutil {
    struct ui_state st;
    struct ui_text host;
    struct tool_text view;
    int page, count;
};

static struct window *open_win;

static void run(struct netutil *n)
{
    char cmd[320];
    const char *host = n->host.buf;
    switch (n->page) {
    case P_INFO: str_copy(cmd, "ifconfig", sizeof(cmd)); break;
    case P_DEVICES: str_copy(cmd, "discover", sizeof(cmd)); break;
    case P_PING: ksnprintf(cmd, sizeof(cmd), "ping -c %d %s", n->count, *host ? host : "10.0.2.2"); break;
    case P_LOOKUP: ksnprintf(cmd, sizeof(cmd), "nslookup %s", *host ? host : "example.com"); break;
    case P_CONNECTIONS: str_copy(cmd, "netstat", sizeof(cmd)); break;
    default:
        ksnprintf(cmd, sizeof(cmd), "time wget -O /tmp/netutil.download %s%s",
                  str_starts(host, "http") ? "" : "https://", *host ? host : "example.com");
        break;
    }
    char *out = tool_run(cmd);
    size_t len = strlen(out);
    char *text = kmalloc(len + strlen(cmd) + 8);
    if (!text) {
        tool_text_set(&n->view, out);
        return;
    }
    ksnprintf(text, len + strlen(cmd) + 8, "> %s\n%s", cmd, out);
    kfree(out);
    tool_text_set(&n->view, text);
    n->view.scroll = 0;
}

static void frame(struct ui *ui, void *data)
{
    struct netutil *n = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (tool_sidebar(ui, H, items, P_COUNT, &n->page)) {
        tool_text_set(&n->view, NULL);
        if (n->page == P_INFO || n->page == P_CONNECTIONS || n->page == P_DEVICES)
            run(n);
    }
    struct rect area = tool_page(ui, W, H, items[n->page].label, hints[n->page]);
    if (ui->e && ui->e->type == EV_TIMER) {
        ui->consumed = true;
        if (n->page == P_CONNECTIONS)
            run(n);
        return;
    }
    int x = area.x + 16, y = area.y + 14;
    bool asks = n->page != P_INFO && n->page != P_CONNECTIONS && n->page != P_DEVICES;
    if (asks) {
        const char *hint = n->page == P_DOWNLOAD ? "Address, like example.com/page.html" : "Host name or address";
        bool enter = ui_textbox_hint(ui, (struct rect){x, y, area.w - (n->page == P_PING ? 286 : 140), 30}, &n->host, hint);
        if (n->page == P_PING) {
            static const char *const counts[] = {"3", "5", "10"};
            int sel = n->count == 3 ? 0 : n->count == 5 ? 1 : 2;
            if (ui_segmented(ui, (struct rect){area.x + area.w - 258, y + 1, 136, 28}, counts, 3, &sel))
                n->count = sel == 0 ? 3 : sel == 1 ? 5 : 10;
        }
        const char *verb = n->page == P_PING ? "Ping" : n->page == P_LOOKUP ? "Look Up" : "Download";
        if (ui_button_primary(ui, (struct rect){area.x + area.w - 112, y + 1, 96, 28}, verb) || enter)
            run(n);
        y += 44;
    } else if (ui_button(ui, (struct rect){area.x + area.w - 112, y, 96, 28}, "Refresh")) {
        run(n);
    }
    if (n->page == P_INFO && ui_button(ui, (struct rect){area.x + area.w - 250, y, 130, 28}, "Renew Address")) {
        char *out = tool_run("dhcp");
        kfree(out);
        run(n);
    }
    if (!asks)
        y += 40;
    tool_text_view(ui, (struct rect){area.x + 12, y, area.w - 24, area.y + area.h - y - 12}, &n->view, "");
    if (!n->view.text && ui->c)
        gfx_text_box(ui->c, (struct rect){area.x, y + 40, area.w, 20}, &font_sans,
                     n->page == P_PING ? "Type a host and press Ping" : "Type an address and press Enter",
                     style.tertiary, ALIGN_CENTER);
}

static void paint(struct window *w, struct canvas *c)
{
    struct netutil *n = w->data;
    ui_run(w, &n->st, c, NULL, frame, n);
}

static void event(struct window *w, struct gui_event *e)
{
    struct netutil *n = w->data;
    ui_run(w, &n->st, NULL, e, frame, n);
}

static void destroy(struct window *w)
{
    struct netutil *n = w->data;
    if (open_win == w)
        open_win = NULL;
    kfree(n->view.text);
    kfree(n);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct netutil *n = kzalloc(sizeof(*n));
    if (!n)
        return;
    n->count = 3;
    if (arg && *arg) {
        n->page = P_PING;
        ui_text_set(&n->host, arg);
    } else {
        run(n);
    }
    open_win = tool_open("Network Utility", ICON_NETUTIL, 820, 540, &ops, n);
    if (!open_win) {
        kfree(n->view.text);
        kfree(n);
        return;
    }
    wm_set_timer(open_win, 3000);
}

static const struct gui_app netutil_app = {
    .name = "Network Utility", .description = "Addresses, ping, name lookup, connections and a download test",
    .icon = ICON_NETUTIL, .launch = launch};

/* ---- discover ---- */

#define MAX_DEVICES 32

struct device {
    uint32_t ip;
    char name[64], kinds[160];
};

struct found {
    struct device dev[MAX_DEVICES];
    int count;
};

static const char *const services[][2] = {
    {"_ipp._tcp", "printer"},          {"_printer._tcp", "printer"},     {"_pdl-datastream._tcp", "printer"},
    {"_uscan._tcp", "scanner"},        {"_scanner._tcp", "scanner"},     {"_airplay._tcp", "AirPlay screen"},
    {"_raop._tcp", "AirPlay speaker"}, {"_googlecast._tcp", "Cast"},     {"_smb._tcp", "file share (SMB)"},
    {"_afpovertcp._tcp", "file share (AFP)"}, {"_nfs._tcp", "file share (NFS)"}, {"_sftp-ssh._tcp", "SFTP"},
    {"_ssh._tcp", "SSH"},              {"_rfb._tcp", "screen sharing"},  {"_http._tcp", "web page"},
    {"_workstation._tcp", "computer"}, {"_device-info._tcp", "device"}, {"_spotify-connect._tcp", "Spotify"},
};

static struct device *device_for(struct found *f, uint32_t ip)
{
    for (int i = 0; i < f->count; i++)
        if (f->dev[i].ip == ip)
            return &f->dev[i];
    if (f->count == MAX_DEVICES)
        return NULL;
    struct device *d = &f->dev[f->count++];
    memset(d, 0, sizeof(*d));
    d->ip = ip;
    return d;
}

static void add_kind(struct device *d, const char *kind)
{
    if (strstr(d->kinds, kind))
        return;
    if (d->kinds[0])
        str_append(d->kinds, ", ", sizeof(d->kinds));
    str_append(d->kinds, kind, sizeof(d->kinds));
}

/* a DNS name at *at (following compression), as dotted text; *at moves past it */
static bool dns_name(const uint8_t *m, size_t n, size_t *at, char *out, size_t cap)
{
    size_t p = *at, o = 0;
    bool jumped = false;
    for (int guard = 0; p < n && guard < 64; guard++) {
        uint8_t len = m[p];
        if (!len) {
            if (!jumped)
                *at = p + 1;
            break;
        }
        if ((len & 0xc0) == 0xc0) {
            if (p + 1 >= n)
                return false;
            if (!jumped)
                *at = p + 2;
            jumped = true;
            p = (size_t)(len & 0x3f) << 8 | m[p + 1];
            continue;
        }
        if (p + 1 + len > n)
            return false;
        if (o && o + 1 < cap)
            out[o++] = '.';
        for (int i = 0; i < len && o + 1 < cap; i++)
            out[o++] = (char)m[p + 1 + i];
        p += 1 + (size_t)len;
    }
    out[o] = 0;
    return true;
}

static void mdns_answer(struct found *f, uint32_t from, const uint8_t *m, size_t n)
{
    if (n < 12)
        return;
    int qd = m[4] << 8 | m[5], an = (m[6] << 8 | m[7]) + (m[8] << 8 | m[9]) + (m[10] << 8 | m[11]);
    size_t at = 12;
    char name[128], data[128];
    for (int i = 0; i < qd; i++) {
        if (!dns_name(m, n, &at, name, sizeof(name)))
            return;
        at += 4;
    }
    struct device *d = device_for(f, from);
    for (int i = 0; i < an && d && at < n; i++) {
        if (!dns_name(m, n, &at, name, sizeof(name)) || at + 10 > n)
            return;
        int type = m[at] << 8 | m[at + 1];
        size_t len = (size_t)(m[at + 8] << 8 | m[at + 9]), rd = at + 10;
        at = rd + len;
        if (at > n)
            return;
        if (type == 12) { /* PTR: "Office Printer._ipp._tcp.local" */
            size_t q = rd;
            if (!dns_name(m, n, &q, data, sizeof(data)))
                continue;
            for (size_t k = 0; k < ARRAY_SIZE(services); k++)
                if (strstr(name, services[k][0])) {
                    add_kind(d, services[k][1]);
                    char *dot = strstr(data, services[k][0]);
                    if (dot && dot > data && !d->name[0]) {
                        dot[-1] = 0;
                        str_copy(d->name, data, sizeof(d->name));
                    }
                }
        } else if (type == 1 && len == 4 && !d->name[0]) { /* A: "printer.local" */
            char *dot = strstr(name, ".local");
            if (dot)
                *dot = 0;
            str_copy(d->name, name, sizeof(d->name));
        }
    }
}

static void ssdp_answer(struct found *f, uint32_t from, const char *m)
{
    struct device *d = device_for(f, from);
    if (!d)
        return;
    const char *server = NULL, *st = NULL;
    for (const char *line = m; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!str_ncasecmp(line, "SERVER:", 7))
            server = line + 7;
        else if (!str_ncasecmp(line, "ST:", 3))
            st = line + 3;
    }
    if (st && strstr(st, "MediaRenderer"))
        add_kind(d, "media player");
    else if (st && strstr(st, "MediaServer"))
        add_kind(d, "media server");
    else if (st && (strstr(st, "InternetGatewayDevice") || strstr(st, "WANIPConnection")))
        add_kind(d, "router");
    else if (st && strstr(st, "Printer"))
        add_kind(d, "printer");
    else
        add_kind(d, "UPnP device");
    if (server && !d->name[0]) {
        while (*server == ' ')
            server++;
        size_t k = 0;
        while (server[k] && server[k] != '\r' && server[k] != '\n' && k + 1 < sizeof(d->name))
            k++;
        memcpy(d->name, server, k);
        d->name[k] = 0;
    }
}

static size_t mdns_query(uint8_t *q, size_t cap)
{
    memset(q, 0, 12);
    size_t at = 12;
    int count = 0;
    for (size_t k = 0; k < ARRAY_SIZE(services) && at + 64 < cap; k++) {
        char name[64];
        ksnprintf(name, sizeof(name), "%s.local", services[k][0]);
        for (char *part = name; *part;) {
            char *dot = strchr(part, '.');
            size_t len = dot ? (size_t)(dot - part) : strlen(part);
            q[at++] = (uint8_t)len;
            memcpy(q + at, part, len);
            at += len;
            part += len + (dot ? 1 : 0);
        }
        q[at++] = 0;
        q[at++] = 0;
        q[at++] = 12;  /* PTR */
        q[at++] = 0x80; /* answer by unicast */
        q[at++] = 1;
        count++;
    }
    q[5] = (uint8_t)count;
    return at;
}

COMMAND(discover, NULL, "net", "discover [seconds]", "find printers, screens, speakers and shares on the local network")
{
    long wait = 3;
    if (argc > 2 || (argc == 2 && (!str_parse_long(argv[1], &wait) || wait < 1 || wait > 30)))
        return command_usage(sh, command_find("discover"));
    int md = net_socket(NET_UDP), up = net_socket(NET_UDP);
    if (md < 0 || up < 0 || net_bind(md, 0, 0) || net_bind(up, 0, 0)) {
        net_close(md);
        net_close(up);
        return sh_error(sh, "discover", "no network");
    }
    struct found *f = kzalloc(sizeof(*f));
    uint8_t *buf = kmalloc(1500);
    if (!f || !buf) {
        kfree(f);
        kfree(buf);
        net_close(md);
        net_close(up);
        return sh_error(sh, "discover", "out of memory");
    }
    size_t qn = mdns_query(buf, 1500);
    net_sendto(md, buf, qn, 0xe00000fbu, 5353);
    static const char search[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                 "MX: 2\r\nST: ssdp:all\r\n\r\n";
    net_sendto(up, search, sizeof(search) - 1, 0xeffffffau, 1900);
    uint64_t until = hal_ticks_ms() + (uint64_t)wait * 1000, resend = hal_ticks_ms() + 1000;
    while (hal_ticks_ms() < until && !input_break_pending()) {
        net_wait(50);
        uint32_t ip;
        uint16_t port;
        long r;
        while ((r = net_recvfrom(md, buf, 1500, &ip, &port)) > 0)
            mdns_answer(f, ip, buf, (size_t)r);
        while ((r = net_recvfrom(up, buf, 1499, &ip, &port)) > 0) {
            buf[r] = 0;
            ssdp_answer(f, ip, (const char *)buf);
        }
        if (hal_ticks_ms() > resend) { /* once more: the first query can go out before ARP and friends settle */
            resend = until;
            qn = mdns_query(buf, 1500);
            net_sendto(md, buf, qn, 0xe00000fbu, 5353);
            net_sendto(up, search, sizeof(search) - 1, 0xeffffffau, 1900);
        }
    }
    net_close(md);
    net_close(up);
    int shown = 0;
    for (int i = 0; i < f->count; i++) {
        if (!f->dev[i].kinds[0])
            continue;
        char ip[20];
        net_format_ip(f->dev[i].ip, ip, sizeof(ip));
        sh_printf(sh, "%-16s %-28s %s\n", ip, f->dev[i].name[0] ? f->dev[i].name : "-", f->dev[i].kinds);
        shown++;
    }
    if (!shown)
        sh_printf(sh, "No devices answered in %ld s (mDNS and SSDP). Devices only answer on the same local network.\n",
                  wait);
    kfree(f);
    kfree(buf);
    return 0;
}

static const struct command *const commands[] = {&cmddef_discover};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&netutil_app, NULL, &ext);
