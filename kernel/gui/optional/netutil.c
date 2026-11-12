#include "systool.h"

/* Network Utility: the interfaces and their addresses, ping, name lookup, open connections and a test download,
   each run through the shell command that does it. */

enum { P_INFO, P_PING, P_LOOKUP, P_CONNECTIONS, P_DOWNLOAD, P_COUNT };

static const struct tool_item items[P_COUNT] = {
    {SYM_NETWORK, "Info", "Network"},       {SYM_WIFI, "Ping", "Tools"},   {SYM_SEARCH, "Lookup", NULL},
    {SYM_LIST, "Connections", NULL}, {SYM_DOWNLOAD, "Download Test", NULL},
};

static const char *const hints[P_COUNT] = {
    "Interfaces, addresses and the DNS server",
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
        if (n->page == P_INFO || n->page == P_CONNECTIONS)
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
    bool asks = n->page != P_INFO && n->page != P_CONNECTIONS;
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
GUI_APP(netutil_app);
