#include <synth/config.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

/* Obscura is the browser of SynthOS: this window is its face, and the Obscura engine (Rust + V8, Apache-2.0,
   github.com/h4ckf0r0day/obscura) does the pages. The engine runs as a 64-bit Windows program and is driven over its
   standard input and output with the Model Context Protocol: navigate, evaluate and screenshot. The engine is
   64-bit only, so the 32-bit system keeps Chromium.

   The window: a strip of tabs that are only site icons (the open one also shows its title), small back, forward,
   reload and download buttons with the address beside them, and the page as a card under them. A new tab is a dark
   page with one field, "Ask anything...", and the sites visited last. */

#if CONFIG_WIN32 && defined(__x86_64__)

#define DIR "/home/apps/obscura"
#define ENGINE DIR "/obscura.exe"
#define TAB_ROW 42
#define NAV_ROW 34
#define TOP (TAB_ROW + NAV_ROW)
#define CARD 6
#define MAX_TABS 12
#define MAX_HISTORY 48
#define MAX_DOWNLOADS 8
#define URL_MAX 1024
#define SEARCH "https://html.duckduckgo.com/html/?q="

enum req {
    R_NONE,
    R_INIT,
    R_NAV,
    R_HISTORY,
    R_SHOT,
    R_INFO,
    R_CLICK,
    R_INPUT,
    R_SCROLL,
    R_SAVE,
    R_HOVER,
    R_CONTEXT,
};

enum cmd {
    CMD_NEW_TAB,
    CMD_CLOSE_TAB,
    CMD_LOCATION,
    CMD_SAVE,
    CMD_RELOAD,
    CMD_BACK,
    CMD_FORWARD,
    CMD_NEXT_TAB,
    CMD_PREV_TAB,
    CMD_DOWNLOADS,
    CMD_HOME,
    CMD_COUNT,
};

struct tab {
    char url[URL_MAX], title[96], icon_host[128], error[160];
    struct picture shot, icon;
};

struct action {
    enum req kind;
    char tool[32];
    char *args;
};

struct visit {
    char url[URL_MAX], title[96];
};

static struct browser {
    struct window *win;
    struct ui_state st;
    struct ui_text addr, ask;
    int addr_id, ask_id;
    struct tab tabs[MAX_TABS];
    int ntabs, cur, hover_tab;
    struct w32_link *link;
    char *rx;
    size_t rx_len, rx_cap;
    int next_id, wait_id;
    enum req wait_kind;
    uint64_t sent_at, moved_at;
    bool ready, page_input, dead, hover_due, focus_ask;
    struct action queue[16];
    int queued;
    char typed[512];
    int typed_len;
    int scroll_dy;
    int mouse_x, mouse_y, menu_x, menu_y;
    char status[160], hover[URL_MAX], menu_link[URL_MAX];
    struct visit history[MAX_HISTORY];
    int nhistory;
    char downloads[MAX_DOWNLOADS][PATH_MAX_LEN];
    int ndownloads;
} *br;

static void launch(const char *arg);
static const struct gui_app obscura_app = {"Obscura", "The web browser", ICON_BROWSER, launch,
                                           ".html;.htm;.xhtml;.url", NULL};
GUI_APP(obscura_app);

static const struct menu_item file_items[] = {
    {"New Tab", KEY_COMMAND(CMD_NEW_TAB), "Ctrl+T"},
    {"Open Location", KEY_COMMAND(CMD_LOCATION), "Ctrl+L"},
    {"Close Tab", KEY_COMMAND(CMD_CLOSE_TAB), "Ctrl+W"},
    {"", 0, NULL},
    {"Save Page", KEY_COMMAND(CMD_SAVE), "Ctrl+S"},
    {"Downloads", KEY_COMMAND(CMD_DOWNLOADS), NULL},
};

static const struct menu_item view_items[] = {
    {"Reload", KEY_COMMAND(CMD_RELOAD), "Ctrl+R"},
    {"", 0, NULL},
    {"Next Tab", KEY_COMMAND(CMD_NEXT_TAB), "Ctrl+Tab"},
    {"Previous Tab", KEY_COMMAND(CMD_PREV_TAB), NULL},
};

static const struct menu_item history_items[] = {
    {"Back", KEY_COMMAND(CMD_BACK), "Alt+Left"},
    {"Forward", KEY_COMMAND(CMD_FORWARD), "Alt+Right"},
    {"Start Page", KEY_COMMAND(CMD_HOME), NULL},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
    {"History", history_items, ARRAY_SIZE(history_items)},
};

/* JSON text and JavaScript literals: the same escapes */
static size_t escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)ksnprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
    return n;
}

/* the string value after "key": in a line of JSON, unescaped (\uXXXX kept to ASCII or '?') */
static char *json_text(const char *line, const char *key)
{
    char pat[32];
    ksnprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    size_t cap = 0;
    while (p[cap] && p[cap] != '"')
        cap += p[cap] == '\\' && p[cap + 1] ? 2 : 1;
    char *out = kmalloc(cap + 1);
    if (!out)
        return NULL;
    size_t n = 0;
    for (size_t i = 0; i < cap; i++) {
        char c = p[i];
        if (c != '\\' || i + 1 >= cap) {
            out[n++] = c;
            continue;
        }
        c = p[++i];
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': break;
        case 'u': {
            unsigned v = 0;
            for (int k = 0; k < 4 && i + 1 < cap; k++) {
                char h = p[++i];
                v = v * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
            }
            if (v < 0x80) {
                out[n++] = (char)v;
            } else if (v < 0x800) {
                out[n++] = (char)(0xc0 | v >> 6);
                out[n++] = (char)(0x80 | (v & 0x3f));
            } else {
                out[n++] = '?';
            }
            break;
        }
        default: out[n++] = c;
        }
    }
    out[n] = 0;
    return out;
}

static size_t base64_decode(const char *s, size_t len, uint8_t *out)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        int v = ch >= 'A' && ch <= 'Z' ? ch - 'A'
                : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
                : ch >= '0' && ch <= '9' ? ch - '0' + 52
                : ch == '+' ? 62
                : ch == '/' ? 63
                            : -1;
        if (v < 0)
            continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* where the page goes: a card under the bars */
static struct rect view_rect(const struct window *w)
{
    return (struct rect){CARD, TOP, MAX(w->surface.w - 2 * CARD, 1), MAX(w->surface.h - TOP - CARD, 1)};
}

static struct tab *tab(void)
{
    return &br->tabs[br->cur];
}

static void host_of(const char *url, char *out, size_t cap)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    if (!strncmp(h, "www.", 4))
        h += 4;
    size_t n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && h[n] != '?' && n + 1 < cap) {
        out[n] = h[n];
        n++;
    }
    out[n] = 0;
}

static uint32_t chrome_back(void)
{
    return style.dark ? RGB(28, 28, 30) : RGB(232, 232, 235);
}

/* ---- history and downloads ---- */

static void visits_path(char *out, size_t cap)
{
    path_join(user_home(), ".obscura-history", out, cap);
}

static void visits_save(void)
{
    size_t cap = (size_t)br->nhistory * (URL_MAX + 100) + 1;
    char *text = kmalloc(cap), path[PATH_MAX_LEN];
    if (!text)
        return;
    size_t n = 0;
    for (int i = 0; i < br->nhistory; i++)
        n += (size_t)ksnprintf(text + n, cap - n, "%s\t%s\n", br->history[i].url, br->history[i].title);
    visits_path(path, sizeof(path));
    vfs_write_all(path, text, n);
    kfree(text);
}

static void visits_load(void)
{
    char path[PATH_MAX_LEN], *data = NULL;
    size_t len = 0;
    visits_path(path, sizeof(path));
    if (vfs_read_all(path, &data, &len) != 0)
        return;
    for (char *line = data; line && *line && br->nhistory < MAX_HISTORY;) {
        char *nl = strchr(line, '\n'), *tabc = strchr(line, '\t');
        if (nl)
            *nl = 0;
        if (tabc) {
            *tabc = 0;
            str_copy(br->history[br->nhistory].url, line, URL_MAX);
            str_copy(br->history[br->nhistory].title, tabc + 1, sizeof(br->history[0].title));
            br->nhistory++;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
}

static void visits_add(const char *url, const char *title)
{
    if (strncmp(url, "http", 4))
        return;
    int i = 0;
    while (i < br->nhistory && strcmp(br->history[i].url, url))
        i++;
    if (i == br->nhistory && br->nhistory < MAX_HISTORY)
        br->nhistory++;
    i = MIN(i, br->nhistory - 1);
    memmove(&br->history[1], &br->history[0], sizeof(br->history[0]) * (size_t)i);
    str_copy(br->history[0].url, url, URL_MAX);
    str_copy(br->history[0].title, title, sizeof(br->history[0].title));
    visits_save();
}

/* ---- site icons ---- */

static uint64_t icon_deadline;

static bool icon_cancelled(void)
{
    return hal_ticks_ms() > icon_deadline;
}

static int icon_decode(const uint8_t *d, size_t n, struct picture *out)
{
    if (n > 22 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0) {
        int count = d[4] | d[5] << 8, best = -1, best_w = 0;
        for (int i = 0; i < count && 6 + 16 * i + 16 <= (int)n; i++) {
            int w = d[6 + 16 * i] ? d[6 + 16 * i] : 256;
            if (best < 0 || (best_w < 16 && w > best_w) || (w >= 16 && w < best_w))
                best = i, best_w = w;
        }
        if (best < 0)
            return E_INVAL;
        const uint8_t *e = d + 6 + 16 * best;
        uint32_t size = (uint32_t)(e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24);
        uint32_t at = (uint32_t)(e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24);
        if (at >= n || size > n - at || size < 40)
            return E_INVAL;
        if (d[at] == 0x89)
            return picture_decode(d + at, size, out);
        /* a BMP without its file header, twice as tall (the mask below the colors) */
        uint8_t *bmp = kmalloc(size + 14);
        if (!bmp)
            return E_NOMEM;
        memcpy(bmp + 14, d + at, size);
        uint32_t hdr = (uint32_t)(bmp[14] | bmp[15] << 8), bpp = (uint32_t)(bmp[28] | bmp[29] << 8);
        uint32_t colors = (uint32_t)(bmp[46] | bmp[47] << 8);
        if (!colors && bpp <= 8)
            colors = 1u << bpp;
        int32_t h = (int32_t)(bmp[22] | bmp[23] << 8 | bmp[24] << 16 | (uint32_t)bmp[25] << 24) / 2;
        uint32_t off = 14 + hdr + colors * 4, total = size + 14;
        bmp[0] = 'B';
        bmp[1] = 'M';
        memcpy(bmp + 2, &total, 4);
        memset(bmp + 6, 0, 4);
        memcpy(bmp + 10, &off, 4);
        memcpy(bmp + 22, &h, 4);
        int r = picture_decode(bmp, total, out);
        kfree(bmp);
        return r;
    }
    return picture_decode(d, n, out);
}

static void fetch_icon(struct tab *t, const char *href)
{
    char host[128], url[URL_MAX];
    host_of(t->url, host, sizeof(host));
    if (!host[0] || !strcmp(host, t->icon_host) || strncmp(t->url, "http", 4))
        return;
    str_copy(t->icon_host, host, sizeof(t->icon_host));
    picture_free(&t->icon);
    if (href && !strncmp(href, "http", 4)) {
        str_copy(url, href, sizeof(url));
    } else {
        const char *rest = strstr(t->url, "://");
        size_t n = rest ? (size_t)(strchr(rest + 3, '/') ? strchr(rest + 3, '/') - t->url : (long)strlen(t->url)) : 0;
        if (!n)
            return;
        ksnprintf(url, sizeof(url), "%.*s/favicon.ico", (int)n, t->url);
    }
    icon_deadline = hal_ticks_ms() + 1500;
    struct http_request req = {.method = "GET", .url = url, .max_size = 128 * 1024, .cancelled = icon_cancelled};
    struct http_response resp = {0};
    if (http_fetch(&req, &resp) == 0 && resp.status == 200 && resp.length)
        icon_decode((const uint8_t *)resp.body, resp.length, &t->icon);
    http_free(&resp);
}

/* ---- the engine ---- */

static void engine_stop(void)
{
    w32_link_close(br->link);
    br->link = NULL;
    br->ready = false;
    br->wait_kind = R_NONE;
    br->rx_len = 0;
    for (int i = 0; i < br->queued; i++)
        kfree(br->queue[i].args);
    br->queued = 0;
}

static void send_line(const char *line)
{
    if (br->link && w32_link_write(br->link, line, strlen(line)) < 0)
        br->dead = true;
}

static bool engine_start(void)
{
    if (br->link)
        return true;
    char *argv[] = {ENGINE, "mcp"};
    int err = gui_link_start(ENGINE, 2, argv, &br->link);
    if (err) {
        br->link = NULL;
        ksnprintf(br->status, sizeof(br->status), "The Obscura engine did not start (%s).", err_str(err));
        return false;
    }
    br->dead = false;
    br->wait_id = br->next_id = 1;
    br->wait_kind = R_INIT;
    br->sent_at = hal_ticks_ms();
    send_line("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\","
              "\"capabilities\":{},\"clientInfo\":{\"name\":\"SynthOS\",\"version\":\"1\"}}}\n"
              "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    str_copy(br->status, "Starting the engine...", sizeof(br->status));
    return true;
}

static bool busy(void)
{
    return br->wait_kind != R_NONE && br->wait_kind != R_HOVER;
}

static void push(enum req kind, const char *tool, const char *args)
{
    if (br->queued >= (int)ARRAY_SIZE(br->queue))
        return;
    /* a newer screenshot or page info makes the waiting one pointless */
    for (int i = 0; i < br->queued; i++)
        if (br->queue[i].kind == kind && (kind == R_SHOT || kind == R_INFO || kind == R_HOVER))
            return;
    struct action *a = &br->queue[br->queued];
    a->args = kstrdup(args ? args : "{}");
    if (!a->args)
        return;
    a->kind = kind;
    str_copy(a->tool, tool, sizeof(a->tool));
    br->queued++;
}

static void push_eval(enum req kind, const char *js)
{
    size_t cap = strlen(js) * 6 + 32;
    char *args = kmalloc(cap);
    if (!args)
        return;
    size_t n = (size_t)ksnprintf(args, cap, "{\"expression\":\"");
    n += escape(args + n, cap - n, js);
    ksnprintf(args + n, cap - n, "\"}");
    push(kind, "browser_evaluate", args);
    kfree(args);
}

static void push_shot(void)
{
    char args[64];
    struct rect v = view_rect(br->win);
    ksnprintf(args, sizeof(args), "{\"width\":%d,\"height\":%d}", v.w, v.h);
    push_eval(R_INFO, "location.href+'\\n'+document.title+'\\n'+"
                      "((document.querySelector('link[rel~=\"icon\"]')||{}).href||'')");
    push(R_SHOT, "browser_screenshot", args);
}

static void send_next(void)
{
    if (!br->ready || br->wait_kind != R_NONE || !br->queued)
        return;
    struct action a = br->queue[0];
    memmove(br->queue, br->queue + 1, sizeof(br->queue[0]) * (size_t)--br->queued);
    size_t cap = strlen(a.args) + 160;
    char *line = kmalloc(cap);
    if (line) {
        br->wait_id = ++br->next_id;
        br->wait_kind = a.kind;
        br->sent_at = hal_ticks_ms();
        ksnprintf(line, cap,
                  "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\","
                  "\"arguments\":%s}}\n",
                  br->wait_id, a.tool, a.args);
        send_line(line);
        kfree(line);
    }
    kfree(a.args);
}

static void drop_queue(void)
{
    for (int i = 0; i < br->queued; i++)
        kfree(br->queue[i].args);
    br->queued = 0;
}

static void navigate(const char *url)
{
    if (!url || !*url)
        return;
    str_copy(tab()->url, url, sizeof(tab()->url));
    ui_text_set(&br->addr, url);
    br->hover[0] = 0;
    tab()->error[0] = 0;
    if (!engine_start())
        return;
    drop_queue();
    size_t cap = strlen(url) * 2 + 32;
    char *args = kmalloc(cap);
    if (!args)
        return;
    size_t n = (size_t)ksnprintf(args, cap, "{\"url\":\"");
    n += escape(args + n, cap - n, url);
    ksnprintf(args + n, cap - n, "\"}");
    push(R_NAV, "browser_navigate", args);
    kfree(args);
    push_shot();
    br->page_input = false;
    char host[128];
    host_of(url, host, sizeof(host));
    ksnprintf(br->status, sizeof(br->status), "Loading %s...", host[0] ? host : url);
}

/* what was typed into the address or the start page: a link, or words to search for */
static void go(const char *text)
{
    while (*text == ' ')
        text++;
    if (!*text)
        return;
    char url[URL_MAX];
    if (strstr(text, "://") || !strncmp(text, "about:", 6)) {
        str_copy(url, text, sizeof(url));
    } else if (strchr(text, '.') && !strchr(text, ' ')) {
        ksnprintf(url, sizeof(url), "https://%s", text);
    } else {
        size_t n = (size_t)ksnprintf(url, sizeof(url), "%s", SEARCH);
        for (const char *p = text; *p && n + 4 < sizeof(url); p++) {
            unsigned char c = (unsigned char)*p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
                url[n++] = (char)c;
            else if (c == ' ')
                url[n++] = '+';
            else
                n += (size_t)ksnprintf(url + n, sizeof(url) - n, "%%%02X", c);
        }
        url[n] = 0;
    }
    navigate(url);
}

static void got_shot(const char *line)
{
    const char *p = strstr(line, "\"data\":\"");
    if (!p)
        return;
    p += 8;
    const char *end = strchr(p, '"');
    if (!end)
        return;
    size_t len = (size_t)(end - p);
    uint8_t *png = kmalloc(len * 3 / 4 + 4);
    if (!png)
        return;
    size_t n = base64_decode(p, len, png);
    struct picture pic;
    if (picture_decode(png, n, &pic) == 0) {
        picture_free(&tab()->shot);
        tab()->shot = pic;
    }
    kfree(png);
}

static void save_page(const char *html)
{
    char dir[PATH_MAX_LEN], name[120], path[PATH_MAX_LEN];
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    vfs_mkdirs(dir);
    size_t n = 0;
    for (const char *s = tab()->title[0] ? tab()->title : "page"; *s && n < 60; s++)
        name[n++] = strchr("/\\:*?\"<>|", *s) ? '_' : *s;
    ksnprintf(name + n, sizeof(name) - n, ".html");
    path_join(dir, name, path, sizeof(path));
    int err = vfs_write_all(path, html, strlen(html));
    if (err) {
        ksnprintf(br->status, sizeof(br->status), "Could not save the page (%s)", err_str(err));
        return;
    }
    ksnprintf(br->status, sizeof(br->status), "Saved to Downloads: %s", name);
    if (br->ndownloads == MAX_DOWNLOADS)
        br->ndownloads--;
    memmove(br->downloads[1], br->downloads[0], sizeof(br->downloads[0]) * (size_t)br->ndownloads);
    str_copy(br->downloads[0], path, sizeof(br->downloads[0]));
    br->ndownloads++;
}

static void context_menu(void);

static void got_info(char *text)
{
    char *nl = strchr(text, '\n'), *icon = NULL;
    if (nl) {
        *nl = 0;
        char *nl2 = strchr(nl + 1, '\n');
        if (nl2) {
            *nl2 = 0;
            icon = nl2 + 1;
        }
        str_copy(tab()->title, nl + 1, sizeof(tab()->title));
    }
    if (!strncmp(text, "about:", 6))
        return;
    str_copy(tab()->url, text, sizeof(tab()->url));
    if (br->st.focus != br->addr_id || !br->addr_id)
        ui_text_set(&br->addr, text);
    wm_set_title(br->win, tab()->title[0] ? tab()->title : "Obscura");
    visits_add(text, tab()->title);
    fetch_icon(tab(), icon);
}

static void answered(const char *line)
{
    enum req kind = br->wait_kind;
    br->wait_kind = R_NONE;
    char *text = json_text(line, "text");
    bool failed = strstr(line, "\"error\":{") || strstr(line, "\"isError\":true");
    if (failed && kind != R_HOVER && kind != R_CONTEXT) {
        char *msg = text ? text : json_text(line, "message");
        ksnprintf(br->status, sizeof(br->status), "%s", msg ? msg : "The page did not answer.");
        if (msg != text)
            kfree(msg);
    }
    switch (kind) {
    case R_INIT:
        br->ready = true;
        br->status[0] = 0;
        break;
    case R_NAV:
    case R_HISTORY:
        br->status[0] = 0;
        if (failed) {
            /* nothing to show: the reason goes on the page */
            drop_queue();
            str_copy(tab()->error, text && *text ? text : "The engine could not load it.", sizeof(tab()->error));
            picture_free(&tab()->shot);
        }
        break;
    case R_SHOT: got_shot(line); break;
    case R_INFO:
        if (text && !failed)
            got_info(text);
        break;
    case R_CLICK:
        if (text && !strncmp(text, "nav:", 4)) {
            navigate(text + 4);
        } else {
            br->page_input = text && strstr(text, ":input");
            push_shot();
        }
        break;
    case R_INPUT:
        if (text && !strncmp(text, "nav:", 4))
            navigate(text + 4);
        else
            push_shot();
        break;
    case R_SCROLL: push_shot(); break;
    case R_SAVE:
        if (text && !failed)
            save_page(text);
        break;
    case R_HOVER: str_copy(br->hover, text && !failed ? text : "", sizeof(br->hover)); break;
    case R_CONTEXT:
        str_copy(br->menu_link, text && !failed ? text : "", sizeof(br->menu_link));
        context_menu();
        break;
    case R_NONE: break;
    }
    kfree(text);
}

static void take_line(char *line)
{
    const char *id = strstr(line, "\"id\":");
    if (!id || str_to_long(id + 5, NULL, 10) != br->wait_id)
        return;
    answered(line);
    if (br->win)
        wm_invalidate(br->win);
}

/* typed keys and wheel turns go to the page in batches, once it is idle */
static void flush_input(void)
{
    if (br->wait_kind != R_NONE || br->queued)
        return;
    if (br->typed_len) {
        char lit[sizeof(br->typed) * 6];
        br->typed[br->typed_len] = 0;
        escape(lit, sizeof(lit), br->typed);
        br->typed_len = 0;
        char *js = kmalloc(sizeof(lit) + 900);
        if (!js)
            return;
        ksnprintf(js, sizeof(lit) + 900,
                  "(function(t){var e=document.activeElement;if(!e||!('value' in e))return '';"
                  "for(var i=0;i<t.length;i++){var c=t[i];"
                  "if(c=='\\b')e.value=e.value.slice(0,-1);"
                  "else if(c=='\\n'){var f=e.form;if(!f)return '';"
                  "if((f.getAttribute('method')||'get').toLowerCase()=='get'){"
                  "var q=[];for(var k=0;k<f.elements.length;k++){var x=f.elements[k];"
                  "if(x.name&&!x.disabled&&(!/checkbox|radio/.test(x.type)||x.checked))"
                  "q.push(encodeURIComponent(x.name)+'='+encodeURIComponent(x.value));}"
                  "var u=new URL(f.getAttribute('action')||location.href,location.href);"
                  "u.search=q.join('&');return 'nav:'+u.href;}"
                  "if(f.requestSubmit)f.requestSubmit();else f.submit();return 'sent';}"
                  "else e.value+=c;}"
                  "e.dispatchEvent(new Event('input',{bubbles:true}));return 'ok';})(\"%s\")",
                  lit);
        push_eval(R_INPUT, js);
        kfree(js);
    } else if (br->scroll_dy) {
        char js[48];
        ksnprintf(js, sizeof(js), "window.scrollBy(0,%d)", br->scroll_dy);
        br->scroll_dy = 0;
        push_eval(R_SCROLL, js);
    } else if (br->hover_due && hal_ticks_ms() - br->moved_at > 180) {
        /* the link under the pointer, for the status line */
        br->hover_due = false;
        struct rect v = view_rect(br->win);
        char js[220];
        ksnprintf(js, sizeof(js),
                  "(function(e){e=e&&e.closest?e.closest('a[href]'):null;return e?e.href:'';})"
                  "(document.elementFromPoint(%d,%d))",
                  br->mouse_x - v.x, br->mouse_y - v.y);
        push_eval(R_HOVER, js);
    }
}

static void pump(void)
{
    if (!br->link)
        return;
    char buf[4096];
    for (int rounds = 0; rounds < 64; rounds++) {
        int n = w32_link_read(br->link, buf, sizeof(buf));
        if (n < 0) {
            br->dead = true;
            break;
        }
        if (!n)
            break;
        if (br->rx_len + (size_t)n + 1 > br->rx_cap) {
            size_t cap = MAX(br->rx_cap * 2, br->rx_len + (size_t)n + 65536);
            char *g = krealloc(br->rx, cap);
            if (!g)
                break;
            br->rx = g;
            br->rx_cap = cap;
        }
        memcpy(br->rx + br->rx_len, buf, (size_t)n);
        br->rx_len += (size_t)n;
        br->rx[br->rx_len] = 0;
        char *start = br->rx, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            take_line(start);
            start = nl + 1;
        }
        br->rx_len -= (size_t)(start - br->rx);
        memmove(br->rx, start, br->rx_len + 1);
    }
    if (br->dead) {
        engine_stop();
        str_copy(br->status, "The Obscura engine stopped. Press Reload to start it again.", sizeof(br->status));
        if (br->win)
            wm_invalidate(br->win);
        return;
    }
    flush_input();
    send_next();
}

/* ---- tabs ---- */

static uint32_t site_color(const char *url)
{
    static const uint32_t colors[] = {RGB(66, 133, 244), RGB(234, 67, 53), RGB(52, 168, 83), RGB(251, 140, 0),
                                      RGB(142, 68, 173), RGB(0, 150, 136), RGB(233, 30, 99), RGB(96, 125, 139)};
    char host[128];
    host_of(url, host, sizeof(host));
    uint32_t h = 0;
    for (const char *p = host; *p; p++)
        h = h * 31 + (unsigned char)*p;
    return colors[h % ARRAY_SIZE(colors)];
}

/* a site's icon: its favicon, or its first letter on a color of its own */
static void site_icon(struct canvas *c, int x, int y, int size, const char *url, const struct picture *icon)
{
    if (icon && icon->px) {
        struct canvas src = canvas_make(icon->px, icon->w, icon->h);
        gfx_scale(c, (struct rect){x, y, size, size}, &src, 255);
        return;
    }
    if (!url[0]) {
        gfx_symbol(c, x, y, SYM_GLOBE, size, style.secondary);
        return;
    }
    char host[128];
    host_of(url, host, sizeof(host));
    char l[2] = {host[0] >= 'a' && host[0] <= 'z' ? (char)(host[0] - 32) : host[0] ? host[0] : 'O', 0};
    gfx_round(c, (struct rect){x, y, size, size}, size / 4, site_color(url));
    gfx_text_box(c, (struct rect){x, y, size, size}, size >= 24 ? &font_title : &font_bold, l, RGB(255, 255, 255),
                 ALIGN_CENTER);
}

static void select_tab(int i)
{
    if (i < 0 || i >= br->ntabs)
        return;
    br->cur = i;
    br->st.focus = 0;
    ui_text_set(&br->addr, tab()->url);
    ui_text_set(&br->ask, "");
    wm_set_title(br->win, tab()->title[0] ? tab()->title : "Obscura");
    if (tab()->url[0])
        navigate(tab()->url);
    else
        br->status[0] = 0;
}

static void new_tab(const char *url)
{
    if (br->ntabs >= MAX_TABS)
        return;
    memset(&br->tabs[br->ntabs], 0, sizeof(br->tabs[0]));
    br->cur = br->ntabs++;
    br->st.focus = 0;
    ui_text_set(&br->addr, "");
    ui_text_set(&br->ask, "");
    br->status[0] = 0;
    br->focus_ask = true;
    wm_set_title(br->win, "Obscura");
    if (url && *url)
        go(url);
}

static void close_tab(int i)
{
    picture_free(&br->tabs[i].shot);
    picture_free(&br->tabs[i].icon);
    memmove(&br->tabs[i], &br->tabs[i + 1], sizeof(br->tabs[0]) * (size_t)(br->ntabs - i - 1));
    if (--br->ntabs == 0) {
        /* the last tab gives way to a new one */
        br->ntabs = 0;
        new_tab(NULL);
        return;
    }
    int next = br->cur > i || br->cur == br->ntabs ? br->cur - 1 : br->cur;
    br->cur = -1;
    select_tab(CLAMP(next, 0, br->ntabs - 1));
}

static void history(const char *tool)
{
    if (!tab()->url[0] || !engine_start())
        return;
    drop_queue();
    push(R_HISTORY, tool, "{}");
    push_shot();
    str_copy(br->status, "Loading...", sizeof(br->status));
}

/* ---- menus ---- */

static void command(int cmd);

static void downloads_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index < br->ndownloads) {
        gui_open_file(br->downloads[index]);
    } else {
        char dir[PATH_MAX_LEN];
        path_join(user_home(), "Downloads", dir, sizeof(dir));
        gui_app_launch(gui_app_find("Files"), dir);
    }
}

static void show_downloads(int x, int y)
{
    static struct menu_item items[MAX_DOWNLOADS + 2];
    int n = 0;
    for (int i = 0; i < br->ndownloads; i++)
        items[n++] = (struct menu_item){path_basename(br->downloads[i]), 0, NULL};
    if (n)
        items[n++] = (struct menu_item){"", 0, NULL};
    items[n++] = (struct menu_item){"Show Downloads Folder", 0, NULL};
    ui_menu_items(br->win, x, y, items, n, downloads_pick);
}

static const struct menu_item link_menu[] = {
    {"Open Link", 0, NULL},       {"Open Link in New Tab", 0, NULL}, {"Copy Link", 0, NULL}, {"", 0, NULL},
    {"Back", 0, NULL},            {"Forward", 0, NULL},             {"Reload", 0, NULL},    {"", 0, NULL},
    {"Save Page As HTML", 0, NULL},
};

static void context_pick(struct window *owner, int index)
{
    UNUSED(owner);
    int base = br->menu_link[0] ? 0 : 4;
    switch (index + base) {
    case 0: navigate(br->menu_link); break;
    case 1: {
        char url[URL_MAX];
        str_copy(url, br->menu_link, sizeof(url));
        new_tab(url);
        break;
    }
    case 2: gui_set_clipboard(br->menu_link, strlen(br->menu_link)); break;
    case 4: command(CMD_BACK); break;
    case 5: command(CMD_FORWARD); break;
    case 6: command(CMD_RELOAD); break;
    case 8: command(CMD_SAVE); break;
    }
    wm_invalidate(br->win);
}

static void context_menu(void)
{
    int skip = br->menu_link[0] ? 0 : 4;
    ui_menu_items(br->win, br->menu_x, br->menu_y, link_menu + skip, (int)ARRAY_SIZE(link_menu) - skip, context_pick);
}

static void command(int cmd)
{
    switch (cmd) {
    case CMD_NEW_TAB: new_tab(NULL); break;
    case CMD_CLOSE_TAB: close_tab(br->cur); return;
    case CMD_LOCATION:
        br->st.focus = tab()->url[0] ? br->addr_id : br->ask_id;
        if (tab()->url[0])
            ui_text_set(&br->addr, tab()->url);
        break;
    case CMD_SAVE:
        if (tab()->url[0] && engine_start())
            push_eval(R_SAVE, "document.documentElement.outerHTML");
        break;
    case CMD_RELOAD:
        if (!br->link && tab()->url[0])
            navigate(tab()->url);
        else
            history("browser_reload");
        break;
    case CMD_BACK: history("browser_back"); break;
    case CMD_FORWARD: history("browser_forward"); break;
    case CMD_NEXT_TAB: select_tab((br->cur + 1) % br->ntabs); break;
    case CMD_PREV_TAB: select_tab((br->cur + br->ntabs - 1) % br->ntabs); break;
    case CMD_DOWNLOADS: show_downloads(br->win->surface.w / 2 - 100, TOP); break;
    case CMD_HOME:
        tab()->url[0] = 0;
        tab()->title[0] = 0;
        picture_free(&tab()->shot);
        ui_text_set(&br->addr, "");
        wm_set_title(br->win, "Obscura");
        break;
    }
    if (br && br->win)
        wm_invalidate(br->win);
}

/* ---- drawing ---- */

/* the page card's rounded corners: what lies outside them takes the window's color */
static void round_corners(struct canvas *c, struct rect r, int radius, uint32_t back)
{
    for (int y = 0; y < radius; y++)
        for (int x = 0; x < radius; x++) {
            int dx = radius - x, dy = radius - y;
            if (dx * dx + dy * dy <= radius * radius)
                continue;
            int xs[2] = {r.x + x, r.x + r.w - 1 - x}, ys[2] = {r.y + y, r.y + r.h - 1 - y};
            for (int k = 0; k < 4; k++) {
                int px = xs[k & 1], py = ys[k >> 1];
                if (rect_contains(c->clip, px, py))
                    c->px[(size_t)py * (size_t)c->stride + (size_t)px] = back;
            }
        }
}

static void tab_strip(struct ui *ui, int W)
{
    int x = 86, right = W - 44;
    br->hover_tab = -1;
    for (int i = 0; i < br->ntabs && x < right; i++) {
        struct tab *t = &br->tabs[i];
        bool sel = i == br->cur;
        int w = 34;
        if (sel) {
            const char *label = t->title[0] ? t->title : t->url[0] ? t->url : "New Tab";
            w = MIN(62 + gfx_text_width(&font_sans, label), 260);
        }
        w = MIN(w, right - x);
        struct rect r = {x, 7, w, 28};
        bool hot = ui->e ? rect_contains(r, ui->e->x, ui->e->y) : br->st.hot == -100 - i;
        if (ui->e && ui->e->type == EV_MOUSE_MOVE && rect_contains(r, ui->e->x, ui->e->y))
            ui->hot = -100 - i;
        if (ui->c) {
            if (sel)
                gfx_round(ui->c, r, 9, style.dark ? RGB(58, 58, 62) : RGB(255, 255, 255));
            else if (hot)
                gfx_round(ui->c, r, 9, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0, 14));
            site_icon(ui->c, r.x + 9, r.y + 6, 16, t->url, &t->icon);
            if (sel) {
                const char *label = t->title[0] ? t->title : t->url[0] ? t->url : "New Tab";
                struct canvas inner = *ui->c;
                inner.clip = rect_intersect(ui->c->clip, (struct rect){r.x + 32, r.y, r.w - 56, r.h});
                gfx_text(&inner, r.x + 32, r.y + (r.h - font_sans.height) / 2, &font_sans, label, style.text);
                gfx_symbol(ui->c, r.x + r.w - 20, r.y + 9, SYM_CLOSE, 10, style.secondary);
            }
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(r, ui->e->x, ui->e->y)) {
            ui->consumed = true;
            if (ui->e->button == BUTTON_MIDDLE || (sel && ui->e->x >= r.x + r.w - 26))
                close_tab(i);
            else if (i != br->cur)
                select_tab(i);
            return;
        }
        x += w + 4;
    }
    if (br->ntabs < MAX_TABS && ui_symbol_button(ui, (struct rect){x + 2, 9, 24, 24}, SYM_PLUS, false))
        command(CMD_NEW_TAB);
}

static void address(struct ui *ui, int W)
{
    int y = TAB_ROW;
    struct rect b = {14, y + 5, 24, 24};
    bool page = tab()->url[0] != 0;
    if (ui_symbol_button(ui, b, SYM_LEFT, false) && page)
        command(CMD_BACK);
    b.x += 28;
    if (ui_symbol_button(ui, b, SYM_RIGHT, false) && page)
        command(CMD_FORWARD);
    b.x += 28;
    if (ui_symbol_button(ui, b, SYM_RESTART, false) && page)
        command(CMD_RELOAD);
    b.x += 28;
    if (ui_symbol_button(ui, b, SYM_DOWNLOAD, br->ndownloads > 0)) {
        if (page && !br->ndownloads)
            command(CMD_SAVE);
        else
            show_downloads(b.x, b.y + b.h + 2);
    }
    int aw = MIN(W - 2 * (b.x + 40), 720);
    struct rect a = {(W - aw) / 2, y + 4, aw, 26};
    br->addr_id = ui->id + 1;
    bool editing = br->st.focus == br->addr_id;
    if (!editing && ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(a, ui->e->x, ui->e->y)) {
        ui_text_set(&br->addr, tab()->url);
        editing = true;
    }
    if (editing) {
        if (ui_textbox_hint(ui, a, &br->addr, "Search or enter address")) {
            char text[sizeof(br->addr.buf)];
            str_copy(text, br->addr.buf, sizeof(text));
            br->st.focus = 0;
            go(text);
        }
        return;
    }
    ui_next_id(ui);
    if (!ui->c)
        return;
    gfx_round(ui->c, a, 8, style.dark ? ALPHA(0xffffffffu, 14) : ALPHA(0, 10));
    char host[128];
    host_of(tab()->url, host, sizeof(host));
    int tx = a.x + 12, ty = a.y + (a.h - font_sans.height) / 2;
    struct canvas inner = *ui->c;
    inner.clip = rect_intersect(ui->c->clip, (struct rect){a.x + 8, a.y, a.w - 16, a.h});
    if (!page) {
        gfx_symbol(ui->c, tx, a.y + 6, SYM_SEARCH, 14, style.tertiary);
        gfx_text(&inner, tx + 20, ty, &font_sans, "Search or enter address", style.tertiary);
        return;
    }
    if (!strncmp(tab()->url, "https://", 8))
        gfx_symbol(ui->c, tx, a.y + 6, SYM_LOCK, 14, style.secondary);
    else
        gfx_symbol(ui->c, tx, a.y + 6, SYM_GLOBE, 14, style.tertiary);
    tx += 20;
    gfx_text(&inner, tx, ty, &font_bold, host, style.text);
    tx += gfx_text_width(&font_bold, host);
    if (tab()->title[0]) {
        gfx_text(&inner, tx, ty, &font_sans, "  -  ", style.tertiary);
        gfx_text(&inner, tx + gfx_text_width(&font_sans, "  -  "), ty, &font_sans, tab()->title, style.secondary);
    }
}

static void start_page(struct ui *ui, struct rect v)
{
    uint32_t back = style.dark ? RGB(20, 20, 22) : RGB(250, 250, 251);
    if (ui->c)
        gfx_clear(ui->c, v, back);
    int fw = MIN(v.w - 80, 620);
    int top = v.y + MAX(v.h / 2 - 90, 30);
    struct rect field = {v.x + (v.w - fw) / 2, top, fw, 52};
    if (ui->c) {
        gfx_text_box(ui->c, (struct rect){v.x, top - 58, v.w, 36}, &font_title, "What are you looking for?", style.text,
                     ALIGN_CENTER);
        gfx_drop_shadow(ui->c, field, 16, 14, style.dark ? 70 : 30, 4);
        gfx_round(ui->c, field, 16, style.dark ? RGB(40, 40, 44) : RGB(255, 255, 255));
        gfx_round_frame(ui->c, field, 16, style.dark ? ALPHA(0xffffffffu, 30) : ALPHA(0, 26));
    }
    br->ask_id = ui->id + 1;
    if (br->focus_ask) {
        br->focus_ask = false;
        br->st.focus = br->ask_id;
    }
    struct rect in = {field.x + 10, field.y + 9, field.w - 20, 34};
    if (ui_search(ui, in, &br->ask, "Ask anything...")) {
        char text[sizeof(br->ask.buf)];
        str_copy(text, br->ask.buf, sizeof(text));
        ui_text_set(&br->ask, "");
        br->st.focus = 0;
        go(text);
        return;
    }
    if (ui->c && br->status[0])
        gfx_text_box(ui->c, (struct rect){v.x, field.y + 62, v.w, 18}, &font_small, br->status, style.secondary,
                     ALIGN_CENTER);
    /* the sites visited last, one tile each */
    int n = MIN(br->nhistory, 8), cols = MIN(n, MAX((fw + 12) / 88, 1));
    if (!n)
        return;
    int tw = 76, gap = 12, rows = (n + cols - 1) / cols;
    int gx = v.x + (v.w - (cols * tw + (cols - 1) * gap)) / 2, gy = field.y + 96;
    if (ui->c)
        gfx_text_box(ui->c, (struct rect){v.x, gy - 4, v.w, 16}, &font_small, "Recently visited", style.tertiary,
                     ALIGN_CENTER);
    gy += 20;
    for (int i = 0; i < n && gy + (i / cols) * 92 + 84 < v.y + v.h; i++) {
        struct rect t = {gx + (i % cols) * (tw + gap), gy + (i / cols) * 92, tw, 84};
        bool hot = ui->e ? false : br->st.hot == -200 - i;
        if (ui->e && ui->e->type == EV_MOUSE_MOVE && rect_contains(t, ui->e->x, ui->e->y))
            ui->hot = -200 - i;
        if (ui->c) {
            if (hot)
                gfx_round(ui->c, t, 12, style.dark ? ALPHA(0xffffffffu, 18) : ALPHA(0, 10));
            gfx_round(ui->c, (struct rect){t.x + 18, t.y + 8, 40, 40}, 12, style.dark ? RGB(44, 44, 48) : RGB(236, 236, 240));
            site_icon(ui->c, t.x + 26, t.y + 16, 24, br->history[i].url, NULL);
            char host[128];
            host_of(br->history[i].url, host, sizeof(host));
            struct canvas inner = *ui->c;
            inner.clip = rect_intersect(ui->c->clip, t);
            gfx_text_box(&inner, (struct rect){t.x, t.y + 54, t.w, 18}, &font_small,
                         br->history[i].title[0] ? br->history[i].title : host, style.secondary, ALIGN_CENTER);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && rect_contains(t, ui->e->x, ui->e->y)) {
            ui->consumed = true;
            char url[URL_MAX];
            str_copy(url, br->history[i].url, sizeof(url));
            navigate(url);
            return;
        }
    }
    UNUSED(rows);
}

static void page_view(struct ui *ui, struct rect v)
{
    if (!ui->c)
        return;
    const struct picture *p = &tab()->shot;
    gfx_clear(ui->c, v, RGB(255, 255, 255));
    if (p->px) {
        struct canvas src = canvas_make(p->px, p->w, p->h);
        if (p->w == v.w && p->h == v.h) {
            gfx_blit(ui->c, v.x, v.y, &src, (struct rect){0, 0, p->w, p->h});
        } else {
            int w = MIN(v.w, p->w);
            gfx_scale_fast(ui->c, (struct rect){v.x, v.y, w, p->h * w / MAX(p->w, 1)}, &src, 255);
        }
    } else if (tab()->error[0]) {
        uint32_t back = style.dark ? RGB(20, 20, 22) : RGB(250, 250, 251);
        gfx_clear(ui->c, v, back);
        int cy = v.y + v.h / 2 - 80;
        gfx_symbol(ui->c, v.x + v.w / 2 - 20, cy, SYM_GLOBE, 40, style.tertiary);
        gfx_text_box(ui->c, (struct rect){v.x, cy + 54, v.w, 28}, &font_title, "This page could not be opened",
                     style.text, ALIGN_CENTER);
        char host[128];
        host_of(tab()->url, host, sizeof(host));
        gfx_text_box(ui->c, (struct rect){v.x, cy + 86, v.w, 18}, &font_bold, host, style.secondary, ALIGN_CENTER);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){v.x + 40, v.y, v.w - 80, v.h});
        gfx_text_box(&inner, (struct rect){v.x + 40, cy + 110, v.w - 80, 18}, &font_small, tab()->error,
                     style.tertiary, ALIGN_CENTER);
    } else {
        gfx_text_box(ui->c, (struct rect){v.x, v.y + v.h / 2 - 10, v.w, 20}, &font_sans, "Loading...",
                     RGB(140, 140, 145), ALIGN_CENTER);
    }
    if (busy() || br->queued) {
        int w = v.w / 4, x = (int)((hal_ticks_ms() / 3) % (uint64_t)(v.w + w)) - w;
        gfx_fill(ui->c, (struct rect){v.x + MAX(x, 0), v.y, MIN(x + w, v.w) - MAX(x, 0), 3}, style.accent);
    }
    const char *line = br->hover[0] ? br->hover : br->status;
    if (line[0]) {
        int tw = MIN(gfx_text_width(&font_small, line) + 20, v.w - 16);
        struct rect r = {v.x + 8, v.y + v.h - 30, tw, 22};
        gfx_round(ui->c, r, 7, ALPHA(RGB(34, 34, 38), 225));
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){r.x + 10, r.y, r.w - 20, r.h});
        gfx_text(&inner, r.x + 10, r.y + (r.h - font_small.height) / 2, &font_small, line, RGB(236, 236, 240));
    }
}

static void get_engine(struct ui *ui, struct rect v)
{
    bool bundled = gui_bundle_has(&obscura_app);
    if (ui->c) {
        gfx_clear(ui->c, v, style.dark ? RGB(20, 20, 22) : RGB(250, 250, 251));
        gfx_icon(ui->c, v.x + v.w / 2 - 32, v.y + v.h / 2 - 110, ICON_BROWSER, 64);
        gfx_text_box(ui->c, (struct rect){v.x, v.y + v.h / 2 - 34, v.w, 26}, &font_title, "Obscura", style.text,
                     ALIGN_CENTER);
        gfx_text_box(ui->c, (struct rect){v.x, v.y + v.h / 2, v.w, 18}, &font_sans,
                     bundled ? "The Obscura engine comes with SynthOS: set it up now."
                             : "The Obscura engine (72 MB) comes from the App Store.",
                     style.secondary, ALIGN_CENTER);
    }
    struct rect b = {v.x + v.w / 2 - 80, v.y + v.h / 2 + 34, 160, 30};
    if (ui_button_primary(ui, b, bundled ? "Set Up" : "Get the Engine")) {
        if (bundled)
            gui_bundle_unpack(NULL);
        else
            gui_app_launch(gui_app_find("App Store"), "obscura");
    }
}

static void frame(struct ui *ui, void *data)
{
    UNUSED(data);
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    uint32_t back = chrome_back();
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, back);
    tab_strip(ui, W);
    if (!br)
        return;
    address(ui, W);
    struct rect v = view_rect(w);
    if (!vfs_exists(ENGINE))
        get_engine(ui, v);
    else if (!tab()->url[0])
        start_page(ui, v);
    else
        page_view(ui, v);
    if (vfs_exists(ENGINE) && tab()->url[0] && tab()->error[0] && !tab()->shot.px &&
        ui_button(ui, (struct rect){v.x + v.w / 2 - 60, v.y + v.h / 2 + 62, 120, 30}, "Try Again")) {
        char url[URL_MAX];
        str_copy(url, tab()->url, sizeof(url));
        navigate(url);
    }
    if (ui->c)
        round_corners(ui->c, v, 10, back);
}

static void paint(struct window *w, struct canvas *c)
{
    ui_run(w, &br->st, c, NULL, frame, NULL);
}

static void page_event(struct window *w, struct gui_event *e)
{
    struct rect v = view_rect(w);
    if (!tab()->url[0] || !vfs_exists(ENGINE) || !br->link)
        return;
    bool inside = rect_contains(v, e->x, e->y);
    if (e->type == EV_MOUSE_DOWN && inside && (e->button == BUTTON_LEFT || e->button == BUTTON_RIGHT)) {
        char js[600];
        bool menu = e->button == BUTTON_RIGHT;
        if (menu)
            ksnprintf(js, sizeof(js),
                      "(function(e){e=e&&e.closest?e.closest('a[href]'):null;return e?e.href:'';})"
                      "(document.elementFromPoint(%d,%d))",
                      e->x - v.x, e->y - v.y);
        else
            ksnprintf(js, sizeof(js),
                      "(function(x,y){var e=document.elementFromPoint(x,y);if(!e)return '';"
                      "var a=e.closest?e.closest('a[href]'):null;"
                      "if(a&&a.href&&a.href.indexOf('javascript:')!=0)return 'nav:'+a.href;"
                      "if(e.focus)e.focus();if(e.click)e.click();"
                      "return e.tagName+((/INPUT|TEXTAREA/.test(e.tagName))?':input':'');})(%d,%d)",
                      e->x - v.x, e->y - v.y);
        br->menu_x = e->x;
        br->menu_y = e->y;
        br->st.focus = 0;
        push_eval(menu ? R_CONTEXT : R_CLICK, js);
        e->handled = true;
    } else if (e->type == EV_MOUSE_MOVE && inside) {
        br->mouse_x = e->x;
        br->mouse_y = e->y;
        br->moved_at = hal_ticks_ms();
        br->hover_due = true;
    } else if (e->type == EV_WHEEL && inside) {
        br->scroll_dy += e->wheel * 80;
        e->handled = true;
    } else if (e->type == EV_KEY && br->page_input && br->st.focus == 0) {
        int k = e->key;
        char c = KEY_IS_TEXT(k) ? (char)(KEY_TEXT_CP(k) < 0x80 ? KEY_TEXT_CP(k) : '?')
                 : k == KEY_BACKSPACE ? '\b'
                 : k == KEY_ENTER    ? '\n'
                 : (k >= 0x20 && k < 0x7f) ? (char)k
                                          : 0;
        if (c && br->typed_len < (int)sizeof(br->typed) - 1) {
            br->typed[br->typed_len++] = c;
            e->handled = true;
        }
    } else if (e->type == EV_KEY && br->st.focus == 0 &&
               (e->key == ' ' || e->key == KEY_PGDN || e->key == KEY_PGUP || e->key == KEY_DOWN || e->key == KEY_UP ||
                e->key == KEY_HOME || e->key == KEY_END)) {
        int k = e->key;
        br->scroll_dy += k == KEY_DOWN   ? 60
                         : k == KEY_UP   ? -60
                         : k == KEY_PGUP ? -v.h + 40
                         : k == KEY_HOME ? -1000000
                         : k == KEY_END  ? 1000000
                                         : v.h - 40;
        e->handled = true;
    }
}

static bool shortcut(int k)
{
    int mod = k & (KEY_MOD_CTRL | KEY_MOD_SUPER), base = KEY_BASE(k);
    if (k >= KEY_COMMAND(0) && k < KEY_COMMAND(CMD_COUNT)) {
        command(k - KEY_COMMAND(0));
        return true;
    }
    if ((k & KEY_MOD_ALT) && (base == KEY_LEFT || base == KEY_RIGHT)) {
        command(base == KEY_LEFT ? CMD_BACK : CMD_FORWARD);
        return true;
    }
    if (mod && base == KEY_TAB) {
        command(k & KEY_MOD_SHIFT ? CMD_PREV_TAB : CMD_NEXT_TAB);
        return true;
    }
    if (!mod)
        return false;
    switch (base | 0x20) {
    case 't': command(CMD_NEW_TAB); return true;
    case 'w': command(CMD_CLOSE_TAB); return true;
    case 'l': command(CMD_LOCATION); return true;
    case 'r': command(CMD_RELOAD); return true;
    case 's': command(CMD_SAVE); return true;
    case '[': command(CMD_BACK); return true;
    case ']': command(CMD_FORWARD); return true;
    default: return false;
    }
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_TIMER) {
        pump();
        if (br && (busy() || br->queued))
            wm_invalidate(w);
        if (br && busy() && hal_ticks_ms() - br->sent_at > 60000) {
            str_copy(br->status, "The page took too long.", sizeof(br->status));
            br->wait_kind = R_NONE;
        }
        return;
    }
    if (e->type == EV_KEY && shortcut(e->key)) {
        e->handled = true;
        return;
    }
    page_event(w, e);
    if (e->handled || !br)
        return;
    ui_run(w, &br->st, NULL, e, frame, NULL);
    if (br && e->type == EV_RESIZE && tab()->url[0] && br->link)
        push_shot();
}

static void destroy(struct window *w)
{
    UNUSED(w);
    engine_stop();
    for (int i = 0; i < br->ntabs; i++) {
        picture_free(&br->tabs[i].shot);
        picture_free(&br->tabs[i].icon);
    }
    kfree(br->rx);
    kfree(br);
    br = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void open_arg(const char *arg, bool fresh_tab)
{
    char url[URL_MAX];
    if (arg[0] == '/' && !strstr(arg, "://"))
        ksnprintf(url, sizeof(url), "file://%s", arg);
    else
        str_copy(url, arg, sizeof(url));
    if (fresh_tab && tab()->url[0])
        new_tab(url);
    else
        go(url);
}

static void launch(const char *arg)
{
    if (br) {
        if (arg && *arg)
            open_arg(arg, true);
        wm_show(br->win);
        wm_focus(br->win);
        wm_invalidate(br->win);
        return;
    }
    if (!(br = kzalloc(sizeof(*br))))
        return;
    struct rect a = wm_screen_rect(0);
    int w = CLAMP(a.w - 100, 640, 1360), h = CLAMP(a.h - 90, 420, 860);
    br->win = wm_open("Obscura", ICON_BROWSER, w, h, &ops, NULL, WIN_RESIZABLE | WIN_FULLSIZE | WIN_NOTITLE);
    if (!br->win) {
        kfree(br);
        br = NULL;
        return;
    }
    br->win->toolbar_h = TAB_ROW;
    br->win->min_w = 520;
    br->win->min_h = 320;
    wm_set_menus(br->win, menus, ARRAY_SIZE(menus));
    wm_set_timer(br->win, 30);
    br->ntabs = 1;
    br->focus_ask = true;
    visits_load();
    if (arg && *arg)
        open_arg(arg, false);
}

#endif
