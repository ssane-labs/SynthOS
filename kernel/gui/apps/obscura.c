#include <synth/log.h>
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
#include <synth/timeline.h>

#include "../internal.h"

/* Obscura is the browser of SynthOS: this window is its face, and the Obscura engine (Rust + V8, Apache-2.0,
   github.com/h4ckf0r0day/obscura) does the pages. The engine runs as a 64-bit Windows program and is driven over its
   standard input and output with the Model Context Protocol: navigate, evaluate and screenshot. The engine is
   64-bit only, so the 32-bit system keeps Chromium.

   The window: a dark strip of tabs that are only site icons (pinned and renamed ones carry a label, groups sit in a
   box of their own), the open tab cut like a folder tab into the page panel under it. The panel has small back,
   forward and reload buttons, the site and title in the middle and downloads on the right. A new tab is the start
   page: the eclipse logo, one "Ask anything..." box, two cards (default browser, search engine) and the pages read
   last as tilted cards at the bottom. Ctrl+L opens the command bar, Ctrl+F finds in the page. */

#if CONFIG_WIN32 && defined(__x86_64__)

#define DIR "/home/apps/obscura"
#define ENGINE DIR "/obscura.exe"
#define TAB_ROW 38
#define NAV_ROW 32
#define EDGE 6
#define MAX_TABS 24
#define MAX_GROUPS 6
#define MAX_HISTORY 64
#define MAX_BOOKMARKS 96
#define MAX_DOWNLOADS 12
#define MAX_CLOSED 10
#define MAX_CHIPS 4
#define URL_MAX 1024
#define SHOT_MAX 3072
#define THUMB_W 200
#define THUMB_H 130
#define INTERNAL "obscura://"
#define SIDE_W 232
#define SPLIT_GAP 6
#define BAR_ROW 30
#define RECENT 5

enum req {
    R_NONE,
    R_INIT,
    R_NAV,
    R_HISTORY,
    R_SHOT,
    R_INFO,
    R_CLICK,
    R_INPUT,
    R_SAVE,
    R_HOVER,
    R_CONTEXT,
    R_FIND,
    R_SOURCE,
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
    CMD_REOPEN,
    CMD_OPEN_FILE,
    CMD_FIND,
    CMD_FIND_NEXT,
    CMD_FIND_PREV,
    CMD_ZOOM_IN,
    CMD_ZOOM_OUT,
    CMD_ZOOM_RESET,
    CMD_SOURCE,
    CMD_FULLSCREEN,
    CMD_BOOKMARK,
    CMD_BOOKMARKS,
    CMD_HISTORY,
    CMD_SETTINGS,
    CMD_IMPORT,
    CMD_EXPORT,
    CMD_PIN,
    CMD_DUPLICATE,
    CMD_RENAME,
    CMD_GROUP,
    CMD_CLOSE_OTHERS,
    CMD_COPY_URL,
    CMD_ABOUT,
    CMD_SPLIT,
    CMD_SIDEBAR,
    CMD_FOCUS_MODE,
    CMD_CLOSE_UNPINNED,
    CMD_COPY_ALL,
    CMD_PASTE_GO,
    CMD_PRIVATE,
    CMD_BAR,
    CMD_PIN_HOME,
    CMD_PROFILES,
    CMD_NEXT_PROFILE,
    CMD_COUNT,
};

enum focus { F_NONE, F_ASK, F_CMD, F_FIND, F_LIST };

struct tab {
    char url[URL_MAX], title[96], icon_host[128], error[160], label[24], home[URL_MAX];
    struct picture shot, icon;
    int scroll, page_h, zoom, group, id;
    bool pinned;
};

struct group {
    char name[24];
    bool collapsed;
    int color;
};

struct action {
    enum req kind;
    char tool[32];
    char *args;
};

struct visit {
    char url[URL_MAX], title[96];
};

/* a place in the tab strip, worked out before drawing and hit-testing */
struct slot {
    struct rect r;
    int tab, group; /* tab index, or -1 for a group's header */
};

static struct browser {
    struct window *win;
    struct ui_state st;
    struct ui_text cmd, ask, find, filter;
    enum focus focus;
    bool cmd_open, find_open, menu_open;
    int cmd_sel;
    struct tab tabs[MAX_TABS];
    int ntabs, cur;
    struct group groups[MAX_GROUPS];
    struct slot slots[MAX_TABS + MAX_GROUPS];
    int nslots, strip_off, strip_end, drag_tab, tip_tab, menu_tab, menu_group;
    uint64_t tip_since;
    struct w32_link *link;
    char *rx;
    size_t rx_len, rx_cap;
    int next_id, wait_id;
    enum req wait_kind;
    uint64_t sent_at, moved_at, find_due;
    bool ready, page_input, dead, hover_due, new_download;
    struct action queue[16];
    int queued;
    char typed[512];
    int typed_len;
    int mouse_x, mouse_y, menu_x, menu_y;
    char status[160], hover[URL_MAX], menu_link[URL_MAX];
    struct visit history[MAX_HISTORY];
    int nhistory;
    struct visit bookmarks[MAX_BOOKMARKS];
    int nbookmarks;
    struct visit chips[MAX_CHIPS];
    int nchips;
    char closed[MAX_CLOSED][URL_MAX];
    int nclosed;
    char downloads[MAX_DOWNLOADS][PATH_MAX_LEN];
    int ndownloads;
    int find_count, find_index, list_scroll, engine;
    bool tip_shown, middle, cmd_fresh;
    int shown_cur, nfiles;
    /* Dia's layouts: tabs in a sidebar, focus mode (no tabs until the pointer asks), two pages side by side */
    bool sidebar, focus_mode, peek, side_drag;
    int split_a, split_b, split_pct, side_off, side_end, tab_ids, tip_pic_id, last_id;
    bool split_drag, alt_click;
    /* recently used tabs for Ctrl+Tab, the bookmarks bar, private browsing */
    int mru[MAX_TABS], nmru, switch_sel, bar_off;
    bool switching, private_mode, bar;
    char profile[28];
    struct picture tip_pic;
    char (*files)[NAME_MAX_LEN];
    struct canvas cards[3];
    char card_url[3][URL_MAX];
    uint32_t cards_key;
} *br;

static void launch(const char *arg);
static const struct gui_app obscura_app = {"Obscura", "The web browser", ICON_BROWSER, launch,
                                           ".html;.htm;.xhtml;.url", NULL};
GUI_APP(obscura_app);

static const struct menu_item file_items[] = {
    {"New Tab", KEY_COMMAND(CMD_NEW_TAB), "Ctrl+T"},
    {"Open Location", KEY_COMMAND(CMD_LOCATION), "Ctrl+L"},
    {"Open File...", KEY_COMMAND(CMD_OPEN_FILE), "Ctrl+O"},
    {"Reopen Closed Tab", KEY_COMMAND(CMD_REOPEN), "Ctrl+Shift+T"},
    {"Private Browsing", KEY_COMMAND(CMD_PRIVATE), "Ctrl+Shift+N"},
    {"Profiles...", KEY_COMMAND(CMD_PROFILES), NULL},
    {"Next Profile", KEY_COMMAND(CMD_NEXT_PROFILE), "Ctrl+Shift+M"},
    {"Close Tab", KEY_COMMAND(CMD_CLOSE_TAB), "Ctrl+W"},
    {"", 0, NULL},
    {"Save Page", KEY_COMMAND(CMD_SAVE), "Ctrl+S"},
    {"Import Bookmarks...", KEY_COMMAND(CMD_IMPORT), NULL},
    {"Export Bookmarks...", KEY_COMMAND(CMD_EXPORT), NULL},
};

static const struct menu_item edit_items[] = {
    {"Find in Page", KEY_COMMAND(CMD_FIND), "Ctrl+F"},
    {"Find Next", KEY_COMMAND(CMD_FIND_NEXT), "F3"},
    {"Find Previous", KEY_COMMAND(CMD_FIND_PREV), "Shift+F3"},
    {"", 0, NULL},
    {"Copy Page Address", KEY_COMMAND(CMD_COPY_URL), NULL},
    {"Copy All Tab Addresses", KEY_COMMAND(CMD_COPY_ALL), NULL},
    {"Paste and Go", KEY_COMMAND(CMD_PASTE_GO), "Ctrl+Shift+V"},
};

static const struct menu_item view_items[] = {
    {"Reload", KEY_COMMAND(CMD_RELOAD), "Ctrl+R"},
    {"", 0, NULL},
    {"Zoom In", KEY_COMMAND(CMD_ZOOM_IN), "Ctrl+="},
    {"Zoom Out", KEY_COMMAND(CMD_ZOOM_OUT), "Ctrl+-"},
    {"Actual Size", KEY_COMMAND(CMD_ZOOM_RESET), "Ctrl+0"},
    {"", 0, NULL},
    {"Page Source", KEY_COMMAND(CMD_SOURCE), "Ctrl+U"},
    {"", 0, NULL},
    {"Split View", KEY_COMMAND(CMD_SPLIT), "Ctrl+E"},
    {"Tabs in Sidebar", KEY_COMMAND(CMD_SIDEBAR), "Ctrl+Shift+S"},
    {"Focus Mode", KEY_COMMAND(CMD_FOCUS_MODE), "Ctrl+Shift+F"},
    {"Bookmarks Bar", KEY_COMMAND(CMD_BAR), "Ctrl+Shift+B"},
    {"Full Screen", KEY_COMMAND(CMD_FULLSCREEN), "F11"},
};

static const struct menu_item history_items[] = {
    {"Back", KEY_COMMAND(CMD_BACK), "Alt+Left"},
    {"Forward", KEY_COMMAND(CMD_FORWARD), "Alt+Right"},
    {"Start Page", KEY_COMMAND(CMD_HOME), "Alt+Home"},
    {"", 0, NULL},
    {"Show All History", KEY_COMMAND(CMD_HISTORY), "Ctrl+H"},
    {"Downloads", KEY_COMMAND(CMD_DOWNLOADS), "Ctrl+J"},
};

static const struct menu_item bookmark_items[] = {
    {"Bookmark This Page", KEY_COMMAND(CMD_BOOKMARK), "Ctrl+D"},
    {"Show All Bookmarks", KEY_COMMAND(CMD_BOOKMARKS), "Ctrl+Alt+B"},
    {"Bookmarks Bar", KEY_COMMAND(CMD_BAR), "Ctrl+Shift+B"},
};

static const struct menu_item tab_items[] = {
    {"Next Tab", KEY_COMMAND(CMD_NEXT_TAB), "Ctrl+Tab"},
    {"Previous Tab", KEY_COMMAND(CMD_PREV_TAB), "Ctrl+Shift+Tab"},
    {"", 0, NULL},
    {"Pin Tab", KEY_COMMAND(CMD_PIN), NULL},
    {"Duplicate Tab", KEY_COMMAND(CMD_DUPLICATE), NULL},
    {"Rename Tab...", KEY_COMMAND(CMD_RENAME), NULL},
    {"Add Tab to New Group", KEY_COMMAND(CMD_GROUP), NULL},
    {"Close Other Tabs", KEY_COMMAND(CMD_CLOSE_OTHERS), NULL},
    {"Close Unpinned Tabs", KEY_COMMAND(CMD_CLOSE_UNPINNED), NULL},
    {"Back to Pinned Page", KEY_COMMAND(CMD_PIN_HOME), NULL},
    {"", 0, NULL},
    {"Settings", KEY_COMMAND(CMD_SETTINGS), "Ctrl+,"},
    {"About Obscura", KEY_COMMAND(CMD_ABOUT), NULL},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"Edit", edit_items, ARRAY_SIZE(edit_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
    {"History", history_items, ARRAY_SIZE(history_items)},
    {"Bookmarks", bookmark_items, ARRAY_SIZE(bookmark_items)},
    {"Tab", tab_items, ARRAY_SIZE(tab_items)},
};

static const struct {
    const char *name, *url;
    uint32_t color;
} engines[] = {
    {"DuckDuckGo", "https://html.duckduckgo.com/html/?q=", RGB(222, 88, 51)},
    {"Google", "https://www.google.com/search?q=", RGB(66, 133, 244)},
    {"Bing", "https://www.bing.com/search?q=", RGB(0, 120, 212)},
    {"Brave Search", "https://search.brave.com/search?q=", RGB(251, 84, 43)},
    {"Startpage", "https://www.startpage.com/do/search?q=", RGB(101, 115, 255)},
    {"Ecosia", "https://www.ecosia.org/search?q=", RGB(53, 160, 80)},
};

static const int zooms[] = {50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200};

static const uint32_t group_colors[] = {RGB(90, 150, 255), RGB(176, 120, 255), RGB(70, 190, 120),
                                        RGB(255, 160, 60),  RGB(255, 100, 150), RGB(60, 200, 200)};

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

/* the panel under the tab strip, and the page in it under its row of buttons */
static int side_w(void)
{
    return br->sidebar && !br->focus_mode ? SIDE_W : 0;
}

static struct rect panel_rect(const struct window *w)
{
    int x = side_w() ? side_w() : EDGE, y = br->sidebar || br->focus_mode ? EDGE : TAB_ROW;
    return (struct rect){x, y, MAX(w->surface.w - x - EDGE, 1), MAX(w->surface.h - y - EDGE, 1)};
}

static struct rect view_rect(const struct window *w)
{
    struct rect p = panel_rect(w);
    int top = NAV_ROW + (br->bar ? BAR_ROW : 0);
    return (struct rect){p.x, p.y + top, p.w, MAX(p.h - top, 1)};
}

static int tab_index(int id)
{
    for (int i = 0; id && i < br->ntabs; i++)
        if (br->tabs[i].id == id)
            return i;
    return -1;
}

static bool split_on(void)
{
    return tab_index(br->split_a) >= 0 && tab_index(br->split_b) >= 0;
}

static bool in_split(int i)
{
    return i >= 0 && i < br->ntabs && br->tabs[i].id && (br->tabs[i].id == br->split_a || br->tabs[i].id == br->split_b);
}

/* the place of one side of the split, or the whole view */
static struct rect pane_rect(const struct window *w, int side)
{
    struct rect v = view_rect(w);
    if (!split_on())
        return v;
    int pct = br->split_pct ? br->split_pct : 50, left = (v.w - SPLIT_GAP) * pct / 100;
    return side == 0 ? (struct rect){v.x, v.y, left, v.h}
                     : (struct rect){v.x + left + SPLIT_GAP, v.y, v.w - left - SPLIT_GAP, v.h};
}

/* where the page of the current tab is shown */
static struct rect page_rect(const struct window *w)
{
    return pane_rect(w, split_on() && br->tabs[br->cur].id == br->split_b ? 1 : 0);
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
    return style.dark ? RGB(9, 10, 10) : RGB(222, 223, 226);
}

static uint32_t panel_back(void)
{
    return style.dark ? RGB(25, 25, 26) : RGB(250, 250, 251);
}

static bool internal(const char *url)
{
    return !strncmp(url, INTERNAL, sizeof(INTERNAL) - 1);
}

static int zoom(void)
{
    return tab()->zoom ? tab()->zoom : 100;
}

/* page pixels across the view: the zoom lays the page out narrower and scales it up */
static int layout_w(struct rect v)
{
    return MAX(v.w * 100 / zoom(), 200);
}

static int shot_w(struct rect v)
{
    return tab()->shot.px ? tab()->shot.w : layout_w(v);
}

/* window pixels to page pixels and back */
static int to_page(struct rect v, int d)
{
    return d * shot_w(v) / MAX(v.w, 1);
}

/* Profiles: "Default" keeps its data in ~/.obscura, every other one in ~/.obscura/profiles/<name> */
static bool default_profile(void)
{
    return !br || !br->profile[0] || !strcmp(br->profile, "Default");
}

static void obscura_dir(const char *name, char *out, size_t cap)
{
    char dir[PATH_MAX_LEN], sub[PATH_MAX_LEN];
    path_join(user_home(), ".obscura", dir, sizeof(dir));
    if (!default_profile()) {
        ksnprintf(sub, sizeof(sub), "%s/profiles/%s", dir, br->profile);
        str_copy(dir, sub, sizeof(dir));
    }
    vfs_mkdirs(dir);
    path_join(dir, name, out, cap);
}

static uint32_t url_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* ---- history and downloads ---- */

static void visits_path(char *out, size_t cap)
{
    if (default_profile())
        path_join(user_home(), ".obscura-history", out, cap);
    else
        obscura_dir("history", out, cap);
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

static void session_save(void);

static void visits_add(const char *url, const char *title)
{
    if (strncmp(url, "http", 4) || br->private_mode)
        return;
    session_save();
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
    timeline_add('w', title, url);
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

/* the link at a point: the engine finds only the block an inline link sits in, and gives inline boxes no height,
   so the nearest link in that block within a few pixels counts */
#define FIND_LINK                                                                                                   \
    "function L(e,x,y){if(!e)return null;var a=e.closest?e.closest('a[href]'):null;if(a)return a;"                  \
    "var l=e.querySelectorAll?e.querySelectorAll('a[href]'):[],laid=false,best=null,bd=17;"                        \
    "for(var i=0;i<l.length;i++){var r=l[i].getBoundingClientRect();if(r.width>0)laid=true;"                       \
    "var d=Math.max(r.left-x,0,x-r.right)+Math.max(r.top-y,0,y-r.bottom);if(d<bd){bd=d;best=l[i];}}"               \
    "return best||(!laid&&l.length==1?l[0]:null);}"

static void push_shot(void)
{
    push_eval(R_INFO, "location.href+'\\n'+document.title+'\\n'+"
                      "((document.querySelector('link[rel~=\"icon\"]')||{}).href||'')+'\\n'+"
                      "Math.max(document.documentElement.scrollHeight||0,document.body?document.body.scrollHeight:0)");
}

/* the engine draws pages from the top whatever the scroll position, so the whole page (up to SHOT_MAX rows) comes
   in one picture and scrolls here */
static void push_picture(void)
{
    char args[64];
    struct rect v = page_rect(br->win);
    int w = layout_w(v), rows = v.h * w / MAX(v.w, 1);
    ksnprintf(args, sizeof(args), "{\"width\":%d,\"height\":%d}", w, CLAMP(tab()->page_h, rows, SHOT_MAX));
    push(R_SHOT, "browser_screenshot", args);
}

static void scroll_by(int dy)
{
    struct rect v = page_rect(br->win);
    int top = MAX(tab()->shot.h - to_page(v, v.h), 0), at = CLAMP(tab()->scroll + to_page(v, dy), 0, top);
    if (at != tab()->scroll) {
        tab()->scroll = at;
        wm_invalidate(br->win);
    }
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

static bool download_link(const char *url);
static void download(const char *url);
static void save_thumb(void);
static void found(const char *text);
static void show_source(const char *html);
static const char *internal_title(const char *url);
static void files_refresh(void);
static void new_tab(const char *url);
static void split_open(const char *url);

static void navigate(const char *url)
{
    if (!url || !*url)
        return;
    if (download_link(url)) {
        download(url);
        return;
    }
    str_copy(tab()->url, url, sizeof(tab()->url));
    br->hover[0] = 0;
    tab()->error[0] = 0;
    tab()->scroll = 0;
    br->find_count = 0;
    if (internal(url)) {
        picture_free(&tab()->shot);
        str_copy(tab()->title, internal_title(url), sizeof(tab()->title));
        wm_set_title(br->win, tab()->title);
        br->status[0] = 0;
        br->list_scroll = 0;
        ui_text_set(&br->filter, "");
        if (!strcmp(url, INTERNAL "downloads"))
            files_refresh();
        return;
    }
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
static void go_as(const char *text, bool search)
{
    while (*text == ' ')
        text++;
    if (!*text)
        return;
    char url[URL_MAX];
    if (!search && (strstr(text, "://") || !strncmp(text, "about:", 6))) {
        str_copy(url, text, sizeof(url));
    } else if (!search && strchr(text, '.') && !strchr(text, ' ')) {
        ksnprintf(url, sizeof(url), "https://%s", text);
    } else {
        size_t n = (size_t)ksnprintf(url, sizeof(url), "%s", engines[br->engine].url);
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

static void go(const char *text)
{
    go_as(text, false);
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
        save_thumb();
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
            char *nl3 = strchr(icon, '\n');
            if (nl3) {
                *nl3 = 0;
                tab()->page_h = (int)str_to_long(nl3 + 1, NULL, 10);
            }
        }
        str_copy(tab()->title, nl + 1, sizeof(tab()->title));
    }
    if (!strncmp(text, "about:", 6))
        return;
    str_copy(tab()->url, text, sizeof(tab()->url));
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
    case R_HISTORY:
        if (failed) {
            /* no page to go to: the current one stays */
            drop_queue();
            break;
        }
        tab()->scroll = 0;
        br->status[0] = 0;
        break;
    case R_NAV:
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
        push_picture();
        break;
    case R_CLICK:
        if (text && !strncmp(text, "nav:", 4) && br->alt_click) {
            char href[URL_MAX];
            str_copy(href, text + 4, sizeof(href));
            br->alt_click = false;
            split_open(href);
        } else if (text && !strncmp(text, "nav:", 4)) {
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
    case R_FIND:
        if (text && !failed)
            found(text);
        break;
    case R_SOURCE:
        if (text && !failed)
            show_source(text);
        break;
    case R_SAVE:
        if (text && !failed)
            save_page(text);
        break;
    case R_HOVER: str_copy(br->hover, text && !failed ? text : "", sizeof(br->hover)); break;
    case R_CONTEXT:
        str_copy(br->menu_link, text && !failed ? text : "", sizeof(br->menu_link));
        if (br->middle) {
            if (br->menu_link[0]) {
                char url[URL_MAX];
                str_copy(url, br->menu_link, sizeof(url));
                new_tab(url);
            }
        } else {
            context_menu();
        }
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
    } else if (br->hover_due && hal_ticks_ms() - br->moved_at > 180) {
        /* the link under the pointer, for the status line */
        br->hover_due = false;
        struct rect v = page_rect(br->win);
        char js[900];
        ksnprintf(js, sizeof(js), "(function(x,y){" FIND_LINK "var a=L(document.elementFromPoint(x,y),x,y);"
                  "return a?a.href:'';})(%d,%d)",
                  to_page(v, br->mouse_x - v.x), to_page(v, br->mouse_y - v.y) + tab()->scroll);
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



/* ---- bookmarks, settings and page pictures ---- */

static int list_load(const char *name, struct visit *v, int max)
{
    char path[PATH_MAX_LEN], *data = NULL;
    size_t len = 0;
    obscura_dir(name, path, sizeof(path));
    if (vfs_read_all(path, &data, &len) != 0)
        return 0;
    int n = 0;
    for (char *line = data; line && *line && n < max;) {
        char *nl = strchr(line, '\n'), *tabc = strchr(line, '\t');
        if (nl)
            *nl = 0;
        if (tabc) {
            *tabc = 0;
            str_copy(v[n].url, line, URL_MAX);
            str_copy(v[n].title, tabc + 1, sizeof(v[0].title));
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    return n;
}

static void list_save(const char *name, const struct visit *v, int n)
{
    size_t cap = (size_t)n * (URL_MAX + 100) + 1;
    char *text = kmalloc(cap), path[PATH_MAX_LEN];
    if (!text)
        return;
    size_t len = 0;
    for (int i = 0; i < n; i++)
        len += (size_t)ksnprintf(text + len, cap - len, "%s\t%s\n", v[i].url, v[i].title);
    obscura_dir(name, path, sizeof(path));
    vfs_write_all(path, text, len);
    kfree(text);
}

static int bookmark_find(const char *url)
{
    for (int i = 0; i < br->nbookmarks; i++)
        if (!strcmp(br->bookmarks[i].url, url))
            return i;
    return -1;
}

static bool bookmark_add(const char *url, const char *title)
{
    if (!url[0] || internal(url) || bookmark_find(url) >= 0 || br->nbookmarks == MAX_BOOKMARKS)
        return false;
    str_copy(br->bookmarks[br->nbookmarks].url, url, URL_MAX);
    str_copy(br->bookmarks[br->nbookmarks].title, title, sizeof(br->bookmarks[0].title));
    br->nbookmarks++;
    return true;
}

static void bookmark_remove(int i)
{
    if (i < 0 || i >= br->nbookmarks)
        return;
    memmove(&br->bookmarks[i], &br->bookmarks[i + 1], sizeof(br->bookmarks[0]) * (size_t)(br->nbookmarks - i - 1));
    br->nbookmarks--;
    list_save("bookmarks", br->bookmarks, br->nbookmarks);
}

static void bookmark_toggle(void)
{
    int i = bookmark_find(tab()->url);
    if (i >= 0) {
        bookmark_remove(i);
        str_copy(br->status, "Bookmark removed", sizeof(br->status));
    } else if (bookmark_add(tab()->url, tab()->title)) {
        list_save("bookmarks", br->bookmarks, br->nbookmarks);
        str_copy(br->status, "Bookmarked", sizeof(br->status));
    }
}

static void thumb_path(const char *url, char *out, size_t cap)
{
    char dir[PATH_MAX_LEN], name[16];
    obscura_dir("thumbs", dir, sizeof(dir));
    vfs_mkdirs(dir);
    ksnprintf(name, sizeof(name), "%08x.png", url_hash(url));
    path_join(dir, name, out, cap);
}

/* the top of the page, small, for the start page's cards */
static void save_thumb(void)
{
    const struct picture *p = &tab()->shot;
    if (!p->px || strncmp(tab()->url, "http", 4) || br->private_mode)
        return;
    struct canvas t;
    if (!canvas_alloc(&t, THUMB_W, THUMB_H))
        return;
    struct canvas src = canvas_make(p->px, p->w, MIN(p->h, p->w * THUMB_H / THUMB_W));
    gfx_scale_smooth(&t, (struct rect){0, 0, THUMB_W, THUMB_H}, &src, 255);
    uint8_t *png = NULL;
    size_t len = 0;
    if (png_encode(t.px, t.w, t.h, t.stride, &png, &len) == 0) {
        char path[PATH_MAX_LEN];
        thumb_path(tab()->url, path, sizeof(path));
        vfs_write_all(path, png, len);
        kfree(png);
        br->cards_key = 0;
    }
    canvas_free(&t);
}

static void set_engine(int i)
{
    br->engine = CLAMP(i, 0, (int)ARRAY_SIZE(engines) - 1);
    app_state_set_number("obscura", "engine", br->engine);
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

static void blend_px(struct canvas *c, int x, int y, uint32_t color, int alpha)
{
    if (alpha <= 0 || !rect_contains(c->clip, x, y))
        return;
    uint32_t *d = &c->px[(size_t)y * (size_t)c->stride + (size_t)x];
    *d = color_mix(*d, color | 0xff000000u, MIN(alpha, 255));
}

/* the Obscura mark: a dark sphere eclipsed from the lower right, its lit rim in the SynthOS halo colors */
static void orb(struct canvas *c, int cx, int cy, int r, bool glow)
{
    int reach = glow ? r * 3 / 2 : r + 1;
    int ox = r * 16 * 13 / 100, oy = r * 16 * 16 / 100, rr = r * 16;
    for (int y = cy - reach; y <= cy + reach; y++)
        for (int x = cx - reach; x <= cx + reach; x++) {
            int dx = (x - cx) * 16 + 8, dy = (y - cy) * 16 + 8;
            int d = gfx_dist16(dx, dy);
            int u = 512 + (dx - dy) * 200 / MAX(rr, 1);
            uint32_t tone = gfx_halo_tone(u);
            if (d >= rr) {
                if (glow && d < rr * 3 / 2) {
                    int fall = (rr * 3 / 2 - d) * 60 / (rr / 2);
                    blend_px(c, x, y, tone, fall * fall / 60);
                }
                if (d < rr + 16)
                    blend_px(c, x, y, tone, (rr + 16 - d) * 7);
                continue;
            }
            int shade = 40 - (dy + rr) * 26 / (2 * rr);
            uint32_t base = style.dark ? RGB(shade, shade, shade + 3) : RGB(shade + 10, shade + 10, shade + 14);
            int e = gfx_dist16(dx - ox, dy - oy) - (rr - 8);
            int lit = CLAMP(e * 255 / 40, 0, 255);
            uint32_t col = color_mix(base, color_mix(tone, 0xffffffffu, 70), lit);
            blend_px(c, x, y, col, gfx_cover(rr - d));
        }
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
        orb(c, x + size / 2, y + size / 2, size / 2 - 1, false);
        return;
    }
    if (internal(url)) {
        const char *page = url + sizeof(INTERNAL) - 1;
        enum symbol_id s = !strncmp(page, "history", 7)     ? SYM_CLOCK
                           : !strncmp(page, "bookmarks", 9) ? SYM_STAR
                           : !strncmp(page, "downloads", 9) ? SYM_DOWNLOAD
                                                            : SYM_GEAR;
        gfx_symbol(c, x, y, s, size, style.secondary);
        return;
    }
    char host[128];
    host_of(url, host, sizeof(host));
    char l[2] = {host[0] >= 'a' && host[0] <= 'z' ? (char)(host[0] - 32) : host[0] ? host[0] : 'O', 0};
    gfx_round(c, (struct rect){x, y, size, size}, size / 4, site_color(url));
    gfx_text_box(c, (struct rect){x, y, size, size}, size >= 24 ? &font_title : &font_bold, l, RGB(255, 255, 255),
                 ALIGN_CENTER);
}

static const char *internal_title(const char *url)
{
    const char *page = url + sizeof(INTERNAL) - 1;
    return !strncmp(page, "history", 7)     ? "History"
           : !strncmp(page, "bookmarks", 9) ? "Bookmarks"
           : !strncmp(page, "downloads", 9) ? "Downloads"
                                            : "Settings";
}

static void close_overlays(void)
{
    br->cmd_open = false;
    br->hover[0] = 0;
    if (br->focus == F_CMD || br->focus == F_ASK || br->focus == F_LIST)
        br->focus = F_NONE;
}

/* the tabs used last come first, for Ctrl+Tab */
static void mru_touch(int id)
{
    int list[MAX_TABS], n = 0;
    list[n++] = id;
    for (int i = 0; i < br->nmru && n < MAX_TABS; i++)
        if (br->mru[i] != id && tab_index(br->mru[i]) >= 0)
            list[n++] = br->mru[i];
    memcpy(br->mru, list, sizeof(int) * (size_t)n);
    br->nmru = n;
}

static int recent_tabs(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < br->nmru && n < max; i++)
        if (tab_index(br->mru[i]) >= 0)
            out[n++] = tab_index(br->mru[i]);
    return n;
}

static void select_tab(int i)
{
    if (i < 0 || i >= br->ntabs)
        return;
    /* a page out of sight keeps no picture, except the other side of a split */
    if (br->cur >= 0 && br->cur < br->ntabs && br->cur != i) {
        br->last_id = tab()->id;
        if (!in_split(br->cur))
            picture_free(&tab()->shot);
    }
    br->cur = i;
    mru_touch(tab()->id);
    close_overlays();
    br->find_count = 0;
    if (tab()->group)
        br->groups[tab()->group - 1].collapsed = false;
    ui_text_set(&br->ask, "");
    wm_set_title(br->win, tab()->title[0] ? tab()->title : "Obscura");
    if (tab()->url[0] && !internal(tab()->url))
        navigate(tab()->url);
    else
        br->status[0] = 0;
}

static void new_tab(const char *url)
{
    if (br->ntabs >= MAX_TABS) {
        str_copy(br->status, "Too many tabs: close one first.", sizeof(br->status));
        return;
    }
    if (br->ntabs && !in_split(br->cur))
        picture_free(&tab()->shot);
    memset(&br->tabs[br->ntabs], 0, sizeof(br->tabs[0]));
    br->tabs[br->ntabs].id = ++br->tab_ids;
    br->cur = br->ntabs++;
    mru_touch(tab()->id);
    close_overlays();
    ui_text_set(&br->ask, "");
    br->status[0] = 0;
    br->find_count = 0;
    br->focus = F_ASK;
    wm_set_title(br->win, "Obscura");
    if (url && *url)
        go(url);
}

/* Split view: the current tab and another one side by side; the other side shows its last picture until it is
   clicked, as the engine draws one page at a time. */
static void split_with(int other, bool right)
{
    if (other < 0 || other >= br->ntabs || other == br->cur)
        return;
    int me = br->tabs[br->cur].id, them = br->tabs[other].id;
    br->split_a = right ? me : them;
    br->split_b = right ? them : me;
    if (!br->split_pct)
        br->split_pct = 50;
    select_tab(other);
}

static void split_open(const char *url)
{
    if (br->ntabs >= MAX_TABS)
        return;
    br->split_a = tab()->id;
    br->split_b = br->tab_ids + 1;
    if (!br->split_pct)
        br->split_pct = 50;
    new_tab(url);
}

static void split_close(int keep)
{
    int i = tab_index(keep);
    br->split_a = br->split_b = 0;
    for (int k = 0; k < br->ntabs; k++)
        if (k != br->cur)
            picture_free(&br->tabs[k].shot);
    if (i >= 0 && i != br->cur)
        select_tab(i);
    else if (br->link && tab()->url[0] && !internal(tab()->url))
        push_shot();
}

static void group_tidy(void)
{
    for (int g = 0; g < MAX_GROUPS; g++) {
        bool used = false;
        for (int i = 0; i < br->ntabs; i++)
            used |= br->tabs[i].group == g + 1;
        if (!used)
            br->groups[g].name[0] = 0;
    }
}

static void close_tab(int i)
{
    if (i < 0 || i >= br->ntabs)
        return;
    struct tab *t = &br->tabs[i];
    if (t->url[0] && !internal(t->url)) {
        memmove(br->closed[1], br->closed[0], sizeof(br->closed[0]) * (size_t)MIN(br->nclosed, MAX_CLOSED - 1));
        str_copy(br->closed[0], t->url, URL_MAX);
        br->nclosed = MIN(br->nclosed + 1, MAX_CLOSED);
    }
    bool split = in_split(i);
    if (split)
        br->split_a = br->split_b = 0;
    picture_free(&t->shot);
    picture_free(&t->icon);
    memmove(&br->tabs[i], &br->tabs[i + 1], sizeof(br->tabs[0]) * (size_t)(br->ntabs - i - 1));
    group_tidy();
    if (--br->ntabs == 0) {
        /* the last tab gives way to a new one */
        new_tab(NULL);
        return;
    }
    int next = br->cur > i || br->cur == br->ntabs ? br->cur - 1 : br->cur;
    bool same = br->cur != i;
    br->cur = -1;
    if (same) {
        br->cur = CLAMP(next, 0, br->ntabs - 1);
        if (split && br->link && tab()->url[0] && !internal(tab()->url))
            push_shot();
        return;
    }
    select_tab(CLAMP(next, 0, br->ntabs - 1));
}

static void move_tab(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= br->ntabs || to >= br->ntabs)
        return;
    struct tab t = br->tabs[from];
    if (from < to)
        memmove(&br->tabs[from], &br->tabs[from + 1], sizeof(t) * (size_t)(to - from));
    else
        memmove(&br->tabs[to + 1], &br->tabs[to], sizeof(t) * (size_t)(from - to));
    br->tabs[to] = t;
    if (br->cur == from)
        br->cur = to;
    else if (from < br->cur && to >= br->cur)
        br->cur--;
    else if (from > br->cur && to <= br->cur)
        br->cur++;
}

static int pinned_count(void)
{
    int n = 0;
    while (n < br->ntabs && br->tabs[n].pinned)
        n++;
    return n;
}

static void pin_tab(int i)
{
    int np = pinned_count();
    struct tab *t = &br->tabs[i];
    if (t->pinned) {
        t->pinned = false;
        t->home[0] = 0;
        move_tab(i, np - 1);
    } else {
        t->pinned = true;
        str_copy(t->home, t->url, sizeof(t->home));
        t->group = 0;
        move_tab(i, np);
        group_tidy();
    }
}

static int group_last(int g)
{
    int last = -1;
    for (int i = 0; i < br->ntabs; i++)
        if (br->tabs[i].group == g)
            last = i;
    return last;
}

static void group_tab(int i, int g)
{
    if (i < 0 || i >= br->ntabs)
        return;
    struct tab *t = &br->tabs[i];
    t->pinned = false;
    if (!g) {
        int old = t->group, last = group_last(old);
        t->group = 0;
        if (old && last > i)
            move_tab(i, last);
        group_tidy();
        return;
    }
    int last = group_last(g);
    t->group = g;
    if (last >= 0)
        move_tab(i, last > i ? last : last + 1);
    else if (i < pinned_count())
        move_tab(i, pinned_count() - 1);
    group_tidy();
}

static void new_group(int i, const char *name)
{
    for (int g = 0; g < MAX_GROUPS; g++)
        if (!br->groups[g].name[0]) {
            str_copy(br->groups[g].name, name && *name ? name : "Group", sizeof(br->groups[g].name));
            br->groups[g].collapsed = false;
            br->groups[g].color = g;
            group_tab(i, g + 1);
            return;
        }
    str_copy(br->status, "There are six groups already.", sizeof(br->status));
}

static void close_group(int g)
{
    for (int i = br->ntabs - 1; i >= 0; i--)
        if (br->tabs[i].group == g)
            close_tab(i);
}

static void history(const char *tool)
{
    if (!tab()->url[0] || internal(tab()->url) || !engine_start())
        return;
    drop_queue();
    push(R_HISTORY, tool, "{}");
    push_shot();
    str_copy(br->status, "Loading...", sizeof(br->status));
}

static bool loading(void)
{
    return br->link && (br->wait_kind == R_NAV || br->wait_kind == R_HISTORY || br->wait_kind == R_INIT ||
                        (br->queued && (br->queue[0].kind == R_NAV || br->queue[0].kind == R_HISTORY ||
                                        br->queue[0].kind == R_INFO || br->queue[0].kind == R_SHOT)) ||
                        br->wait_kind == R_INFO || (br->wait_kind == R_SHOT && !tab()->shot.px));
}

/* ---- downloads ---- */

static bool download_link(const char *url)
{
    static const char exts[] = ".zip.7z.rar.tar.gz.tgz.xz.bz2.zst.exe.msi.iso.img.dmg.deb.rpm.apk.bin.mp3.flac.wav.ogg."
                               "m4a.mp4.mkv.avi.mov.webm.pdf.doc.docx.xls.xlsx.ppt.pptx.odt.ods.epub.torrent.ttf.otf.";
    const char *rest = strstr(url, "://");
    if (strncmp(url, "http", 4) || !rest || !(rest = strchr(rest + 3, '/')))
        return false;
    size_t n = 0;
    while (rest[n] && rest[n] != '?' && rest[n] != '#')
        n++;
    const char *dot = NULL;
    for (size_t i = 0; i < n; i++)
        if (rest[i] == '.')
            dot = rest + i;
        else if (rest[i] == '/')
            dot = NULL;
    if (!dot || rest + n - dot > 8)
        return false;
    char ext[12];
    size_t k = 0;
    for (const char *p = dot; p < rest + n; p++)
        ext[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    ext[k++] = '.';
    ext[k] = 0;
    return strstr(exts, ext) != NULL;
}

static int download_sink(void *ctx, const void *data, size_t n)
{
    return vfs_write((struct file *)ctx, data, n) == (long)n ? 0 : E_IO;
}

static void remember_download(const char *path)
{
    for (int i = 0; i < br->ndownloads; i++)
        if (!strcmp(br->downloads[i], path)) {
            memmove(br->downloads[i], br->downloads[i + 1], sizeof(br->downloads[0]) * (size_t)(br->ndownloads - i - 1));
            br->ndownloads--;
            break;
        }
    if (br->ndownloads == MAX_DOWNLOADS)
        br->ndownloads--;
    memmove(br->downloads[1], br->downloads[0], sizeof(br->downloads[0]) * (size_t)br->ndownloads);
    str_copy(br->downloads[0], path, sizeof(br->downloads[0]));
    br->ndownloads++;
    br->new_download = true;
}

/* a file link: it goes to ~/Downloads, with a progress window while it comes */
static void download(const char *url)
{
    char dir[PATH_MAX_LEN], name[NAME_MAX_LEN], path[PATH_MAX_LEN];
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    vfs_mkdirs(dir);
    const char *base = strrchr(url, '/');
    size_t n = 0;
    for (const char *p = base ? base + 1 : url; *p && *p != '?' && *p != '#' && n + 1 < sizeof(name) - 8; p++) {
        if (p[0] == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            long v = str_to_long(hex, NULL, 16);
            name[n++] = v >= 0x20 && v < 0x7f && !strchr("/\\:*?\"<>|", (int)v) ? (char)v : '_';
            p += 2;
        } else {
            name[n++] = strchr("/\\:*?\"<>|", *p) ? '_' : *p;
        }
    }
    name[n] = 0;
    if (!n)
        str_copy(name, "download", sizeof(name));
    path_join(dir, name, path, sizeof(path));
    for (int i = 2; vfs_exists(path) && i < 100; i++) {
        char alt[NAME_MAX_LEN + 8];
        const char *dot = strrchr(name, '.');
        int stem = dot ? (int)(dot - name) : (int)strlen(name);
        ksnprintf(alt, sizeof(alt), "%.*s (%d)%s", stem, name, i, dot ? dot : "");
        path_join(dir, alt, path, sizeof(path));
    }
    struct file *f = NULL;
    int err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &f);
    if (err) {
        ksnprintf(br->status, sizeof(br->status), "Could not save %s (%s)", name, err_str(err));
        return;
    }
    progress_open("Downloading", path_basename(path));
    struct http_request req = {.method = "GET", .url = url, .max_size = (size_t)1 << 31, .progress = progress_update,
                               .sink = download_sink, .ctx = f};
    struct http_response resp = {0};
    err = http_fetch(&req, &resp);
    progress_close();
    vfs_close(f);
    if (!err && (resp.status < 200 || resp.status >= 300))
        err = E_IO;
    http_free(&resp);
    if (err) {
        vfs_unlink(path);
        ksnprintf(br->status, sizeof(br->status), "The download failed (%s)", err_str(err));
        return;
    }
    remember_download(path);
    ksnprintf(br->status, sizeof(br->status), "Downloaded %s", path_basename(path));
    gui_notify("Download complete", path_basename(path));
}

/* ---- find in page, page source, bookmark files ---- */

static void run_find(int index)
{
    if (!tab()->url[0] || internal(tab()->url) || !br->link)
        return;
    char lit[sizeof(br->find.buf) * 6];
    escape(lit, sizeof(lit), br->find.buf);
    size_t cap = sizeof(lit) + 1400;
    char *js = kmalloc(cap);
    if (!js)
        return;
    ksnprintf(js, cap,
              "(function(q,n){var d=document,o=d.querySelectorAll('mark[data-ob]');"
              "for(var i=0;i<o.length;i++){var m=o[i],p=m.parentNode;"
              "p.replaceChild(d.createTextNode(m.textContent),m);if(p.normalize)p.normalize();}"
              "if(!q||!d.body)return '0\\n0\\n0';var lq=q.toLowerCase(),ms=[];"
              "function walk(e){for(var c=e.firstChild;c;c=c.nextSibling){"
              "if(c.nodeType==1){if(!/^(SCRIPT|STYLE|NOSCRIPT|TEXTAREA)$/.test(c.tagName))walk(c);}"
              "else if(c.nodeType==3){var k=c.nodeValue.toLowerCase().indexOf(lq);if(k>=0){"
              "var r=c.splitText(k);r.splitText(q.length);var m=d.createElement('mark');"
              "m.setAttribute('data-ob','1');m.style.background='#ffe14d';m.style.color='#000';"
              "r.parentNode.replaceChild(m,r);m.appendChild(r);ms.push(m);c=m;}}}}"
              "walk(d.body);if(!ms.length)return '0\\n0\\n0';n=((n%%ms.length)+ms.length)%%ms.length;"
              "var e=ms[n];e.style.background='#ff9632';var t=e.getBoundingClientRect();"
              "if(!t.height&&e.parentNode&&e.parentNode.getBoundingClientRect)t=e.parentNode.getBoundingClientRect();"
              "return ms.length+'\\n'+n+'\\n'+Math.round(t.top+(window.scrollY||0));})(\"%s\",%d)",
              lit, index);
    push_eval(R_FIND, js);
    kfree(js);
}

static void found(const char *text)
{
    const char *p = text;
    br->find_count = (int)str_to_long(p, NULL, 10);
    p = strchr(p, '\n');
    br->find_index = p ? (int)str_to_long(p + 1, NULL, 10) : 0;
    p = p ? strchr(p + 1, '\n') : NULL;
    int top = p ? (int)str_to_long(p + 1, NULL, 10) : 0;
    if (br->find_count) {
        struct rect v = page_rect(br->win);
        int rows = to_page(v, v.h);
        tab()->scroll = CLAMP(top - rows / 3, 0, MAX(MAX(tab()->shot.h, tab()->page_h) - rows, 0));
    }
    push_picture();
}

static void find_step(int dir)
{
    if (!br->find.len) {
        br->find_open = true;
        br->focus = F_FIND;
        return;
    }
    run_find(br->find_count ? br->find_index + dir : 0);
}

static void find_close(void)
{
    br->find_open = false;
    if (br->focus == F_FIND)
        br->focus = F_NONE;
    if (br->find_count) {
        ui_text_set(&br->find, "");
        run_find(0);
    }
    br->find_count = 0;
    br->find_due = 0;
}

static void show_source(const char *html)
{
    char path[PATH_MAX_LEN];
    obscura_dir("source.txt", path, sizeof(path));
    if (vfs_write_all(path, html, strlen(html)) == 0)
        gui_app_launch(gui_app_find("Notepad"), path);
}

static const char *find_nocase(const char *s, const char *word)
{
    size_t n = strlen(word);
    for (; *s; s++)
        if (!str_ncasecmp(s, word, n))
            return s;
    return NULL;
}

/* a Netscape bookmark file, the format every browser exports */
static void import_done(void *ctx, const char *path)
{
    UNUSED(ctx);
    char *data = NULL;
    size_t len = 0;
    if (!br || !path || vfs_read_all(path, &data, &len) != 0)
        return;
    int added = 0;
    for (const char *p = data; (p = find_nocase(p, "href=\""));) {
        p += 6;
        const char *end = strchr(p, '"');
        if (!end)
            break;
        char url[URL_MAX], title[96];
        str_copy(url, p, MIN((size_t)(end - p) + 1, sizeof(url)));
        const char *gt = strchr(end, '>'), *close = gt ? find_nocase(gt, "</a") : NULL;
        size_t n = 0;
        for (const char *q = gt ? gt + 1 : end; close && q < close && n + 1 < sizeof(title); q++) {
            if (*q == '<') {
                while (q < close && *q != '>')
                    q++;
                continue;
            }
            if (*q != '\n' && *q != '\r' && *q != '\t')
                title[n++] = *q;
        }
        title[n] = 0;
        if (!strncmp(url, "http", 4) && bookmark_add(url, title[0] ? title : url))
            added++;
        p = end;
    }
    kfree(data);
    list_save("bookmarks", br->bookmarks, br->nbookmarks);
    char note[64];
    ksnprintf(note, sizeof(note), "%d new bookmark%s", added, added == 1 ? "" : "s");
    gui_notify("Bookmarks imported", note);
    app_state_set_on("obscura", "dismissed", true);
    wm_invalidate(br->win);
}

static void export_done(void *ctx, const char *path)
{
    UNUSED(ctx);
    if (!br || !path)
        return;
    size_t cap = (size_t)br->nbookmarks * (2 * URL_MAX + 240) + 400;
    char *text = kmalloc(cap);
    if (!text)
        return;
    size_t n = (size_t)ksnprintf(text, cap, "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<META HTTP-EQUIV=\"Content-Type\" "
                                            "CONTENT=\"text/html; charset=UTF-8\">\n<TITLE>Bookmarks</TITLE>\n"
                                            "<H1>Bookmarks</H1>\n<DL><p>\n");
    for (int i = 0; i < br->nbookmarks; i++)
        n += (size_t)ksnprintf(text + n, cap - n, "    <DT><A HREF=\"%s\">%s</A>\n", br->bookmarks[i].url,
                               br->bookmarks[i].title);
    n += (size_t)ksnprintf(text + n, cap - n, "</DL><p>\n");
    int err = vfs_write_all(path, text, n);
    kfree(text);
    gui_notify(err ? "Export failed" : "Bookmarks exported", path_basename(path));
}

static void open_file_done(void *ctx, const char *path)
{
    UNUSED(ctx);
    if (!br || !path)
        return;
    char url[URL_MAX];
    ksnprintf(url, sizeof(url), "file://%s", path);
    if (br->nchips < MAX_CHIPS && !tab()->url[0] && ctx) {
        str_copy(br->chips[br->nchips].url, url, URL_MAX);
        str_copy(br->chips[br->nchips].title, path_basename(path), sizeof(br->chips[0].title));
        br->nchips++;
    } else if (tab()->url[0]) {
        new_tab(url);
    } else {
        navigate(url);
    }
    wm_invalidate(br->win);
}

static void pick_file(bool as_chip)
{
    struct gui_picker o = {.title = "Open in Obscura", .button = "Open", .exts = NULL};
    gui_file_picker(br->win, &o, open_file_done, as_chip ? (void *)1 : NULL);
}

/* ---- menus ---- */

static void command(int cmd);

static void downloads_pick(struct window *owner, int index)
{
    UNUSED(owner);
    br->new_download = false;
    if (index < br->ndownloads) {
        gui_open_file(br->downloads[index]);
    } else if (index == br->ndownloads + (br->ndownloads ? 1 : 0)) {
        char dir[PATH_MAX_LEN];
        path_join(user_home(), "Downloads", dir, sizeof(dir));
        gui_app_launch(gui_app_find("Files"), dir);
    } else {
        new_tab(INTERNAL "downloads");
    }
}

static void show_downloads(int x, int y)
{
    static struct menu_item items[MAX_DOWNLOADS + 3];
    int n = 0;
    for (int i = 0; i < br->ndownloads; i++)
        items[n++] = (struct menu_item){path_basename(br->downloads[i]), 0, NULL};
    if (n)
        items[n++] = (struct menu_item){"", 0, NULL};
    items[n++] = (struct menu_item){"Show Downloads Folder", 0, NULL};
    items[n++] = (struct menu_item){"All Downloads", 0, "Ctrl+J"};
    ui_menu_items(br->win, x, y, items, n, downloads_pick);
}

static const struct menu_item link_menu[] = {
    {"Open Link", 0, NULL},
    {"Open Link in New Tab", 0, NULL},
    {"Copy Link", 0, NULL},
    {"Download Linked File", 0, NULL},
    {"Bookmark Link", 0, NULL},
    {"", 0, NULL},
    {"Back", 0, "Alt+Left"},
    {"Forward", 0, "Alt+Right"},
    {"Reload", 0, "Ctrl+R"},
    {"", 0, NULL},
    {"Find in Page...", 0, "Ctrl+F"},
    {"Bookmark Page", 0, "Ctrl+D"},
    {"Copy Page Address", 0, NULL},
    {"Save Page As HTML", 0, "Ctrl+S"},
    {"View Page Source", 0, "Ctrl+U"},
};

static void context_pick(struct window *owner, int index)
{
    UNUSED(owner);
    char url[URL_MAX];
    str_copy(url, br->menu_link, sizeof(url));
    switch (index + (br->menu_link[0] ? 0 : 6)) {
    case 0: navigate(url); break;
    case 1: new_tab(url); break;
    case 2: gui_set_clipboard(url, strlen(url)); break;
    case 3: download(url); break;
    case 4:
        if (bookmark_add(url, url))
            list_save("bookmarks", br->bookmarks, br->nbookmarks);
        break;
    case 6: command(CMD_BACK); break;
    case 7: command(CMD_FORWARD); break;
    case 8: command(CMD_RELOAD); break;
    case 10: command(CMD_FIND); break;
    case 11: command(CMD_BOOKMARK); break;
    case 12: command(CMD_COPY_URL); break;
    case 13: command(CMD_SAVE); break;
    case 14: command(CMD_SOURCE); break;
    }
    wm_invalidate(br->win);
}

static void context_menu(void)
{
    int skip = br->menu_link[0] ? 0 : 6;
    ui_menu_items(br->win, br->menu_x, br->menu_y, link_menu + skip, (int)ARRAY_SIZE(link_menu) - skip, context_pick);
}

static const struct menu_item logo_menu[] = {
    {"New Tab", 0, "Ctrl+T"},     {"Reopen Closed Tab", 0, "Ctrl+Shift+T"}, {"", 0, NULL},
    {"History", 0, "Ctrl+H"},     {"Bookmarks", 0, "Ctrl+Alt+B"},         {"Downloads", 0, "Ctrl+J"},
    {"Settings", 0, "Ctrl+,"},    {"", 0, NULL},                            {"Import Bookmarks...", 0, NULL},
    {"Export Bookmarks...", 0, NULL}, {"", 0, NULL},                         {"Profiles...", 0, NULL},
    {"Private Browsing", 0, "Ctrl+Shift+N"}, {"", 0, NULL},                   {"About Obscura", 0, NULL},
};

static void logo_pick(struct window *owner, int index)
{
    UNUSED(owner);
    static const int cmds[] = {CMD_NEW_TAB, CMD_REOPEN,   -1,         CMD_HISTORY, CMD_BOOKMARKS, CMD_DOWNLOADS,
                               CMD_SETTINGS, -1,          CMD_IMPORT, CMD_EXPORT,  -1,            CMD_PROFILES,
                               CMD_PRIVATE,  -1,          CMD_ABOUT};
    if (index >= 0 && index < (int)ARRAY_SIZE(cmds) && cmds[index] >= 0)
        command(cmds[index]);
}

static char tab_names[MAX_TABS][64];

static void tabs_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index < br->ntabs)
        select_tab(index);
    else if (index == br->ntabs + 1)
        command(CMD_REOPEN);
    else if (index == br->ntabs + 2)
        command(CMD_CLOSE_OTHERS);
    wm_invalidate(br->win);
}

static void show_tabs(int x, int y)
{
    static struct menu_item items[MAX_TABS + 3];
    int n = 0;
    for (int i = 0; i < br->ntabs; i++) {
        const struct tab *t = &br->tabs[i];
        char host[64];
        host_of(t->url, host, sizeof(host));
        ksnprintf(tab_names[i], sizeof(tab_names[i]), "%s%.56s", i == br->cur ? "> " : "   ",
                  t->label[0] ? t->label : t->title[0] ? t->title : host[0] ? host : "New Tab");
        items[n++] = (struct menu_item){tab_names[i], 0, NULL};
    }
    items[n++] = (struct menu_item){"", 0, NULL};
    items[n++] = (struct menu_item){"Reopen Closed Tab", 0, "Ctrl+Shift+T"};
    items[n++] = (struct menu_item){"Close Other Tabs", 0, NULL};
    ui_menu_items(br->win, x, y, items, n, tabs_pick);
}

static void rename_done(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!br || !value || br->menu_tab < 0 || br->menu_tab >= br->ntabs)
        return;
    str_copy(br->tabs[br->menu_tab].label, value, sizeof(br->tabs[0].label));
    wm_invalidate(br->win);
}

static void group_name_done(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!br || !value || br->menu_tab < 0 || br->menu_tab >= br->ntabs)
        return;
    new_group(br->menu_tab, value);
    wm_invalidate(br->win);
}

static void group_rename_done(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!br || !value || !*value || br->menu_group < 1 || br->menu_group > MAX_GROUPS)
        return;
    str_copy(br->groups[br->menu_group - 1].name, value, sizeof(br->groups[0].name));
    wm_invalidate(br->win);
}

enum {
    TM_RELOAD,
    TM_DUPLICATE,
    TM_PIN,
    TM_RENAME,
    TM_NEW_GROUP,
    TM_UNGROUP,
    TM_BOOKMARK,
    TM_COPY,
    TM_CLOSE,
    TM_CLOSE_OTHERS,
    TM_CLOSE_RIGHT,
    TM_SPLIT,
    TM_UNSPLIT,
    TM_COPY_MD,
    TM_GROUP, /* then one entry per group */
};

static int tab_menu_ids[24 + MAX_GROUPS];
static char group_labels[MAX_GROUPS][40];

static void tab_menu_pick(struct window *owner, int index)
{
    UNUSED(owner);
    int i = br->menu_tab;
    if (i < 0 || i >= br->ntabs || index < 0)
        return;
    struct tab *t = &br->tabs[i];
    switch (tab_menu_ids[index]) {
    case TM_RELOAD:
        select_tab(i);
        break;
    case TM_DUPLICATE: {
        char url[URL_MAX];
        str_copy(url, t->url, sizeof(url));
        new_tab(url);
        break;
    }
    case TM_PIN: pin_tab(i); break;
    case TM_RENAME: ui_prompt(br->win, "Rename Tab", "Label shown in the tab strip:", t->label, rename_done); break;
    case TM_NEW_GROUP: ui_prompt(br->win, "New Tab Group", "Group name:", "Work", group_name_done); break;
    case TM_UNGROUP: group_tab(i, 0); break;
    case TM_BOOKMARK:
        if (bookmark_add(t->url, t->title))
            list_save("bookmarks", br->bookmarks, br->nbookmarks);
        break;
    case TM_COPY: gui_set_clipboard(t->url, strlen(t->url)); break;
    case TM_COPY_MD: {
        char md[URL_MAX + 120];
        int n = ksnprintf(md, sizeof(md), "[%s](%s)", t->title[0] ? t->title : t->url, t->url);
        gui_set_clipboard(md, (size_t)n);
        break;
    }
    case TM_SPLIT: split_with(i, true); break;
    case TM_UNSPLIT: split_close(t->id); break;
    case TM_CLOSE: close_tab(i); break;
    case TM_CLOSE_OTHERS:
        br->cur = i;
        command(CMD_CLOSE_OTHERS);
        break;
    case TM_CLOSE_RIGHT:
        while (br->ntabs > i + 1)
            close_tab(br->ntabs - 1);
        if (br->cur > i)
            select_tab(i);
        break;
    default: group_tab(i, tab_menu_ids[index] - TM_GROUP + 1); break;
    }
    wm_invalidate(br->win);
}

static void tab_menu(int i, int x, int y)
{
    static struct menu_item items[24 + MAX_GROUPS];
    int n = 0;
    const struct tab *t = &br->tabs[i];
#define ITEM(label, id) (tab_menu_ids[n] = (id), items[n++] = (struct menu_item){label, 0, NULL})
    br->menu_tab = i;
    ITEM("Reload", TM_RELOAD);
    ITEM("Duplicate", TM_DUPLICATE);
    ITEM(t->pinned ? "Unpin Tab" : "Pin Tab", TM_PIN);
    ITEM("Rename...", TM_RENAME);
    if (in_split(i) && split_on())
        ITEM("Close Split View", TM_UNSPLIT);
    else if (i != br->cur)
        ITEM("Open in Split View", TM_SPLIT);
    ITEM("", -1);
    ITEM("Add to New Group...", TM_NEW_GROUP);
    for (int g = 0; g < MAX_GROUPS; g++)
        if (br->groups[g].name[0] && t->group != g + 1) {
            ksnprintf(group_labels[g], sizeof(group_labels[g]), "Add to \"%s\"", br->groups[g].name);
            ITEM(group_labels[g], TM_GROUP + g);
        }
    if (t->group)
        ITEM("Remove from Group", TM_UNGROUP);
    ITEM("", -1);
    ITEM("Bookmark Tab", TM_BOOKMARK);
    ITEM("Copy Address", TM_COPY);
    ITEM("Copy as Markdown", TM_COPY_MD);
    ITEM("", -1);
    ITEM("Close Tab", TM_CLOSE);
    ITEM("Close Other Tabs", TM_CLOSE_OTHERS);
    ITEM("Close Tabs to the Right", TM_CLOSE_RIGHT);
#undef ITEM
    ui_menu_items(br->win, x, y, items, n, tab_menu_pick);
}

static const struct menu_item group_menu_items[] = {
    {"Collapse or Expand", 0, NULL}, {"Rename Group...", 0, NULL}, {"Ungroup", 0, NULL}, {"", 0, NULL},
    {"Close Group", 0, NULL},
};

static void group_menu_pick(struct window *owner, int index)
{
    UNUSED(owner);
    int g = br->menu_group;
    if (g < 1 || g > MAX_GROUPS || !br->groups[g - 1].name[0])
        return;
    switch (index) {
    case 0: br->groups[g - 1].collapsed = !br->groups[g - 1].collapsed; break;
    case 1:
        ui_prompt(br->win, "Rename Group", "Group name:", br->groups[g - 1].name, group_rename_done);
        break;
    case 2:
        for (int i = 0; i < br->ntabs; i++)
            if (br->tabs[i].group == g)
                br->tabs[i].group = 0;
        group_tidy();
        break;
    case 4: close_group(g); break;
    }
    wm_invalidate(br->win);
}

static char engine_labels[ARRAY_SIZE(engines)][32];

static void engine_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index >= 0 && index < (int)ARRAY_SIZE(engines))
        set_engine(index);
    wm_invalidate(br->win);
}

static void show_engines(int x, int y)
{
    static struct menu_item items[ARRAY_SIZE(engines)];
    for (int i = 0; i < (int)ARRAY_SIZE(engines); i++) {
        str_copy(engine_labels[i], engines[i].name, sizeof(engine_labels[i]));
        items[i] = (struct menu_item){engine_labels[i], 0, i == br->engine ? "Current" : NULL};
    }
    ui_menu_items(br->win, x, y, items, (int)ARRAY_SIZE(engines), engine_pick);
}

/* the start page's "+": open tabs become chips that narrow the search to their sites, or a file opens */
static int add_tab_ids[MAX_TABS];

static void add_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index == 0) {
        pick_file(true);
        return;
    }
    index -= 2;
    if (index < 0 || br->nchips >= MAX_CHIPS)
        return;
    const struct tab *t = &br->tabs[add_tab_ids[index]];
    str_copy(br->chips[br->nchips].url, t->url, URL_MAX);
    str_copy(br->chips[br->nchips].title, t->title[0] ? t->title : t->url, sizeof(br->chips[0].title));
    br->nchips++;
    br->focus = F_ASK;
    wm_invalidate(br->win);
}

static void show_add(int x, int y)
{
    static struct menu_item items[MAX_TABS + 2];
    int n = 0, k = 0;
    items[n++] = (struct menu_item){"Open a File...", 0, "Ctrl+O"};
    items[n++] = (struct menu_item){"", 0, NULL};
    for (int i = 0; i < br->ntabs; i++) {
        const struct tab *t = &br->tabs[i];
        if (!t->url[0] || internal(t->url))
            continue;
        char host[64];
        host_of(t->url, host, sizeof(host));
        ksnprintf(tab_names[i], sizeof(tab_names[i]), "%.56s", t->title[0] ? t->title : host);
        add_tab_ids[k++] = i;
        items[n++] = (struct menu_item){tab_names[i], 0, NULL};
    }
    if (!k)
        n--;
    ui_menu_items(br->win, x, y, items, n, add_pick);
}

static void close_others(void)
{
    for (int i = br->ntabs - 1; i >= 0; i--)
        if (i != br->cur && !br->tabs[i].pinned)
            close_tab(i);
}

static void set_zoom(int dir)
{
    int z = zoom(), k = 0;
    while (k < (int)ARRAY_SIZE(zooms) - 1 && zooms[k] < z)
        k++;
    k = dir ? CLAMP(k + dir, 0, (int)ARRAY_SIZE(zooms) - 1) : 5;
    if (zooms[k] == z || !tab()->url[0] || internal(tab()->url))
        return;
    tab()->scroll = tab()->scroll * z / zooms[k];
    tab()->zoom = zooms[k];
    ksnprintf(br->status, sizeof(br->status), "Zoom %d%%", zooms[k]);
    if (br->link)
        push_shot();
}

static void open_cmd(const char *text)
{
    br->cmd_open = true;
    br->cmd_sel = 0;
    br->focus = F_CMD;
    ui_text_set(&br->cmd, text);
    br->cmd_fresh = text[0] != 0;
}

/* the open tabs are kept for the next start: pinned tabs always come back (at the page they were pinned at), the
   others when "Restore tabs" is on */
static void session_save(void)
{
    if (!br || br->private_mode)
        return;
    struct visit *v = kmalloc(sizeof(*v) * MAX_TABS);
    if (!v)
        return;
    int n = 0;
    for (int i = 0; i < br->ntabs; i++) {
        const struct tab *t = &br->tabs[i];
        const char *url = t->pinned && t->home[0] ? t->home : t->url;
        if (!url[0] || !strncmp(url, "file:", 5))
            continue;
        str_copy(v[n].url, url, URL_MAX);
        ksnprintf(v[n].title, sizeof(v[n].title), "%c%s\x1f%s", t->pinned ? '*' : '-', t->label, t->title);
        n++;
    }
    list_save("session", v, n);
    kfree(v);
    app_state_set_number("obscura", "session_cur", br->cur);
}

static int session_load(void)
{
    struct visit *v = kmalloc(sizeof(*v) * MAX_TABS);
    if (!v)
        return 0;
    int n = list_load("session", v, MAX_TABS), k = 0;
    bool all = app_state_on("obscura", "restore", true);
    for (int i = 0; i < n; i++) {
        bool pinned = v[i].title[0] == '*';
        if (!pinned && !all)
            continue;
        struct tab *t = &br->tabs[k++];
        memset(t, 0, sizeof(*t));
        t->id = ++br->tab_ids;
        t->pinned = pinned;
        str_copy(t->url, v[i].url, sizeof(t->url));
        if (pinned)
            str_copy(t->home, v[i].url, sizeof(t->home));
        char *sep = strchr(v[i].title, 0x1f);
        if (sep) {
            *sep = 0;
            str_copy(t->title, sep + 1, sizeof(t->title));
        }
        str_copy(t->label, v[i].title[0] ? v[i].title + 1 : "", sizeof(t->label));
    }
    kfree(v);
    if (k) {
        br->ntabs = k;
        br->cur = CLAMP((int)app_state_number("obscura", "session_cur", 0), 0, k - 1);
        if (!all)
            br->cur = 0;
    }
    return k;
}

#define MAX_PROFILES 9

static int profiles(char names[][28], int max)
{
    char list[MAX_PROFILES * 28];
    str_copy(list, app_state("obscura", "profiles", "Default"), sizeof(list));
    int n = 0;
    for (char *p = list; p && *p && n < max;) {
        char *bar = strchr(p, '|');
        if (bar)
            *bar = 0;
        if (*p)
            str_copy(names[n++], p, 28);
        p = bar ? bar + 1 : NULL;
    }
    if (!n)
        str_copy(names[n++], "Default", 28);
    return n;
}

static void profiles_save(char names[][28], int n)
{
    char list[MAX_PROFILES * 28];
    size_t len = 0;
    list[0] = 0;
    for (int i = 0; i < n; i++)
        len += (size_t)ksnprintf(list + len, sizeof(list) - len, "%s%s", i ? "|" : "", names[i]);
    app_state_set("obscura", "profiles", list);
}

static uint32_t profile_color(void)
{
    return group_colors[url_hash(br->profile) % ARRAY_SIZE(group_colors)];
}

/* the window turns to another profile: its own tabs, history and bookmarks */
static void profile_use(const char *name)
{
    if (!strcmp(name, br->profile))
        return;
    session_save();
    br->split_a = br->split_b = 0;
    for (int i = 0; i < br->ntabs; i++) {
        picture_free(&br->tabs[i].shot);
        picture_free(&br->tabs[i].icon);
    }
    str_copy(br->profile, name, sizeof(br->profile));
    app_state_set("obscura", "profile", br->profile);
    memset(br->tabs, 0, sizeof(br->tabs));
    memset(br->groups, 0, sizeof(br->groups));
    br->ntabs = 1;
    br->cur = 0;
    br->nmru = br->nhistory = br->nclosed = br->nchips = 0;
    br->tabs[0].id = ++br->tab_ids;
    visits_load();
    br->nbookmarks = list_load("bookmarks", br->bookmarks, MAX_BOOKMARKS);
    session_load();
    mru_touch(tab()->id);
    br->cards_key = 0;
    br->focus = tab()->url[0] ? F_NONE : F_ASK;
    ksnprintf(br->status, sizeof(br->status), "Profile: %s", br->profile);
    wm_set_title(br->win, tab()->title[0] ? tab()->title : "Obscura");
    if (tab()->url[0] && !internal(tab()->url) && vfs_exists(ENGINE))
        navigate(tab()->url);
}

static void profile_new_done(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!br || !value)
        return;
    char name[28];
    size_t n = 0;
    for (const char *c = value; *c && n < sizeof(name) - 1; c++)
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' ||
            *c == '_' || *c == ' ')
            name[n++] = *c;
    name[n] = 0;
    if (!n)
        return;
    char names[MAX_PROFILES][28];
    int count = profiles(names, MAX_PROFILES), i = 0;
    while (i < count && strcmp(names[i], name))
        i++;
    if (i == count) {
        if (count >= MAX_PROFILES)
            return;
        str_copy(names[count++], name, 28);
        profiles_save(names, count);
    }
    profile_use(name);
    wm_invalidate(br->win);
}

static void profile_delete_done(struct window *owner, const char *value)
{
    UNUSED(owner);
    UNUSED(value);
    if (!br || default_profile())
        return;
    char names[MAX_PROFILES][28], gone[28];
    int count = profiles(names, MAX_PROFILES), k = 0;
    str_copy(gone, br->profile, sizeof(gone));
    for (int i = 0; i < count; i++)
        if (strcmp(names[i], gone))
            str_copy(names[k++], names[i], 28);
    profiles_save(names, k);
    profile_use("Default");
    char dir[PATH_MAX_LEN];
    ksnprintf(dir, sizeof(dir), "%s/.obscura/profiles/%s", user_home(), gone);
    vfs_remove_tree(dir);
    wm_invalidate(br->win);
}

static char profile_labels[MAX_PROFILES][40];
static int profile_count;

static void profile_pick(struct window *owner, int index)
{
    UNUSED(owner);
    char names[MAX_PROFILES][28];
    int n = profiles(names, MAX_PROFILES);
    if (index >= 0 && index < n)
        profile_use(names[index]);
    else if (index == profile_count + 1)
        ui_prompt(br->win, "New Profile", "A profile keeps its own tabs, history and bookmarks. Name:", "Work",
                  profile_new_done);
    else if (index == profile_count + 2 && !default_profile())
        ui_confirm(br->win, "Delete Profile", "Delete this profile with its history and bookmarks?",
                   profile_delete_done);
    wm_invalidate(br->win);
}

static void show_profiles(int x, int y)
{
    static struct menu_item items[MAX_PROFILES + 3];
    static const char *const keys[] = {"Alt+1", "Alt+2", "Alt+3", "Alt+4", "Alt+5",
                                       "Alt+6", "Alt+7", "Alt+8", "Alt+9"};
    char names[MAX_PROFILES][28];
    int n = profiles(names, MAX_PROFILES), k = 0;
    for (int i = 0; i < n; i++) {
        ksnprintf(profile_labels[i], sizeof(profile_labels[i]), "%s%s", strcmp(names[i], br->profile) ? "   " : "> ",
                  names[i]);
        items[k++] = (struct menu_item){profile_labels[i], 0, keys[i]};
    }
    profile_count = n;
    items[k++] = (struct menu_item){"", 0, NULL};
    items[k++] = (struct menu_item){"New Profile...", 0, NULL};
    if (!default_profile())
        items[k++] = (struct menu_item){"Delete This Profile...", 0, NULL};
    ui_menu_items(br->win, x, y, items, k, profile_pick);
}

static void copy_all(void)
{
    size_t cap = (size_t)br->ntabs * URL_MAX + 1, n = 0;
    char *text = kmalloc(cap);
    if (!text)
        return;
    for (int i = 0; i < br->ntabs; i++)
        if (br->tabs[i].url[0] && !internal(br->tabs[i].url))
            n += (size_t)ksnprintf(text + n, cap - n, "%s\n", br->tabs[i].url);
    gui_set_clipboard(text, n);
    kfree(text);
    ksnprintf(br->status, sizeof(br->status), "Copied the addresses of the open tabs.");
}

static void paste_go(void)
{
    const char *clip = gui_clipboard();
    if (!clip || !*clip)
        return;
    char text[URL_MAX];
    str_copy(text, clip, sizeof(text));
    for (char *c = text; *c; c++)
        if (*c == '\n' || *c == '\r' || *c == '\t')
            *c = ' ';
    go(text);
}

static void command(int cmd)
{
    switch (cmd) {
    case CMD_NEW_TAB: new_tab(NULL); break;
    case CMD_CLOSE_TAB: close_tab(br->cur); break;
    case CMD_LOCATION:
        if (!tab()->url[0]) {
            close_overlays();
            br->focus = F_ASK;
        } else {
            open_cmd(internal(tab()->url) ? "" : tab()->url);
        }
        break;
    case CMD_SAVE:
        if (tab()->url[0] && !internal(tab()->url) && engine_start())
            push_eval(R_SAVE, "document.documentElement.outerHTML");
        break;
    case CMD_SOURCE:
        if (tab()->url[0] && !internal(tab()->url) && engine_start())
            push_eval(R_SOURCE, "document.documentElement.outerHTML");
        break;
    case CMD_RELOAD:
        if (internal(tab()->url))
            break;
        if (!br->link && tab()->url[0])
            navigate(tab()->url);
        else
            history("browser_reload");
        break;
    case CMD_BACK: history("browser_back"); break;
    case CMD_FORWARD: history("browser_forward"); break;
    case CMD_NEXT_TAB: select_tab((br->cur + 1) % br->ntabs); break;
    case CMD_PREV_TAB: select_tab((br->cur + br->ntabs - 1) % br->ntabs); break;
    case CMD_DOWNLOADS: new_tab(INTERNAL "downloads"); break;
    case CMD_HISTORY: new_tab(INTERNAL "history"); break;
    case CMD_BOOKMARKS: new_tab(INTERNAL "bookmarks"); break;
    case CMD_SETTINGS: new_tab(INTERNAL "settings"); break;
    case CMD_HOME:
        tab()->url[0] = 0;
        tab()->title[0] = 0;
        picture_free(&tab()->shot);
        picture_free(&tab()->icon);
        tab()->icon_host[0] = 0;
        br->focus = F_ASK;
        wm_set_title(br->win, "Obscura");
        break;
    case CMD_REOPEN:
        if (br->nclosed) {
            char url[URL_MAX];
            str_copy(url, br->closed[0], sizeof(url));
            memmove(br->closed[0], br->closed[1], sizeof(br->closed[0]) * (size_t)--br->nclosed);
            new_tab(url);
        }
        break;
    case CMD_OPEN_FILE: pick_file(false); break;
    case CMD_FIND:
        if (tab()->url[0] && !internal(tab()->url)) {
            br->find_open = true;
            br->focus = F_FIND;
        }
        break;
    case CMD_FIND_NEXT: find_step(1); break;
    case CMD_FIND_PREV: find_step(-1); break;
    case CMD_ZOOM_IN: set_zoom(1); break;
    case CMD_ZOOM_OUT: set_zoom(-1); break;
    case CMD_ZOOM_RESET: set_zoom(0); break;
    case CMD_FULLSCREEN: wm_toggle_fullscreen(br->win); break;
    case CMD_BOOKMARK: bookmark_toggle(); break;
    case CMD_IMPORT: {
        struct gui_picker o = {.title = "Import Bookmarks", .exts = ".html;.htm", .button = "Import"};
        gui_file_picker(br->win, &o, import_done, NULL);
        break;
    }
    case CMD_EXPORT: {
        struct gui_picker o = {.save = true, .title = "Export Bookmarks", .name = "bookmarks.html", .button = "Export"};
        gui_file_picker(br->win, &o, export_done, NULL);
        break;
    }
    case CMD_PIN: pin_tab(br->cur); break;
    case CMD_DUPLICATE:
        if (tab()->url[0]) {
            char url[URL_MAX];
            str_copy(url, tab()->url, sizeof(url));
            new_tab(url);
        }
        break;
    case CMD_RENAME:
        br->menu_tab = br->cur;
        ui_prompt(br->win, "Rename Tab", "Label shown in the tab strip:", tab()->label, rename_done);
        break;
    case CMD_GROUP:
        br->menu_tab = br->cur;
        ui_prompt(br->win, "New Tab Group", "Group name:", "Work", group_name_done);
        break;
    case CMD_CLOSE_OTHERS: close_others(); break;
    case CMD_COPY_URL: gui_set_clipboard(tab()->url, strlen(tab()->url)); break;
    case CMD_ABOUT:
        gui_message("About Obscura",
                    "Obscura, the web browser of SynthOS.\n\nPages are drawn by the Obscura engine (Rust and V8, "
                    "Apache License 2.0). Tabs, bookmarks, history, downloads and the start page are part of "
                    "SynthOS.");
        break;
    case CMD_SPLIT:
        if (split_on())
            split_close(tab()->id);
        else
            split_open(NULL);
        break;
    case CMD_SIDEBAR:
        br->sidebar = !br->sidebar;
        br->focus_mode = false;
        app_state_set_on("obscura", "sidebar", br->sidebar);
        br->win->toolbar_h = br->sidebar ? 1 : TAB_ROW;
        if (br->link && tab()->url[0] && !internal(tab()->url))
            push_shot();
        break;
    case CMD_PROFILES: show_profiles(80, TAB_ROW); break;
    case CMD_NEXT_PROFILE: {
        char names[MAX_PROFILES][28];
        int n = profiles(names, MAX_PROFILES), i = 0;
        while (i < n && strcmp(names[i], br->profile))
            i++;
        profile_use(names[(i + 1) % n]);
        break;
    }
    case CMD_CLOSE_UNPINNED:
        for (int i = br->ntabs - 1; i >= 0; i--)
            if (!br->tabs[i].pinned)
                close_tab(i);
        break;
    case CMD_COPY_ALL: copy_all(); break;
    case CMD_PASTE_GO: paste_go(); break;
    case CMD_PIN_HOME:
        if (tab()->pinned && tab()->home[0] && strcmp(tab()->home, tab()->url)) {
            char home[URL_MAX];
            str_copy(home, tab()->home, sizeof(home));
            navigate(home);
        }
        break;
    case CMD_PRIVATE:
        br->private_mode = !br->private_mode;
        str_copy(br->status,
                 br->private_mode ? "Private browsing: pages are not kept in history, pictures or the saved tabs."
                                  : "Private browsing is off.",
                 sizeof(br->status));
        break;
    case CMD_BAR:
        br->bar = !br->bar;
        app_state_set_on("obscura", "bar", br->bar);
        if (br->link && tab()->url[0] && !internal(tab()->url))
            push_shot();
        break;
    case CMD_FOCUS_MODE:
        br->focus_mode = !br->focus_mode;
        br->peek = false;
        br->win->toolbar_h = br->focus_mode || br->sidebar ? 1 : TAB_ROW;
        str_copy(br->status, br->focus_mode ? "Focus mode: move the pointer to the edge for the tabs." : "",
                 sizeof(br->status));
        if (br->link && tab()->url[0] && !internal(tab()->url))
            push_shot();
        break;
    }
    if (br && br->win)
        wm_invalidate(br->win);
}

/* ---- drawing helpers ---- */

static uint32_t dim(void)
{
    return style.dark ? RGB(146, 146, 150) : RGB(112, 112, 118);
}

static uint32_t hover_back(void)
{
    return style.dark ? ALPHA(0xffffffffu, 20) : ALPHA(0, 14);
}

/* something drawn by hand that clicks like a button: true when the left button comes up on it */
static bool area(struct ui *ui, struct rect r, bool *hot)
{
    int id = ui_next_id(ui);
    bool in = ui_hover(ui, r);
    if (in)
        ui->hot = id;
    if (hot)
        *hot = ui->e ? in : ui->st->hot == id;
    if (!ui->e)
        return false;
    if (ui->e->type == EV_MOUSE_DOWN && in && ui->e->button == BUTTON_LEFT) {
        ui->st->active = id;
        ui->consumed = true;
    } else if (ui->e->type == EV_MOUSE_UP && ui->st->active == id) {
        ui->consumed = true;
        return in;
    }
    return false;
}

static bool icon_button(struct ui *ui, struct rect r, enum symbol_id s, int size, uint32_t color)
{
    bool hot, hit = area(ui, r, &hot);
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, r, 7, hover_back());
        gfx_symbol(ui->c, r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, s, size, hot ? style.text : color);
    }
    return hit;
}

static bool pill_button(struct ui *ui, struct rect r, const char *label, uint32_t back, uint32_t fore, bool check)
{
    bool hot, hit = area(ui, r, &hot);
    if (ui->c) {
        gfx_round(ui->c, r, 6, hot ? color_shade(back, style.dark && color_luma(back) < 128 ? 18 : -14) : back);
        int tw = gfx_text_width(&font_sans, label) + (check ? 18 : 0), x = r.x + (r.w - tw) / 2;
        if (check)
            gfx_symbol(ui->c, x, r.y + (r.h - 12) / 2, SYM_CHECK, 12, fore);
        gfx_text(ui->c, x + (check ? 18 : 0), r.y + (r.h - font_sans.height) / 2, &font_sans, label, fore);
    }
    return hit;
}

/* a borderless text field: the box around it is drawn by its owner */
static bool field(struct ui *ui, struct rect r, struct ui_text *t, const char *hint, enum focus which,
                  const struct ui_font *f)
{
    ui_next_id(ui);
    bool focused = br->focus == which;
    if (ui->e) {
        if (ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, r) && ui->e->button == BUTTON_LEFT) {
            br->focus = which;
            int x = ui->e->x - r.x + t->scroll, k = 0;
            while (k < t->len && gfx_text_width_n(f, t->buf, (size_t)k + 1) <= x)
                k++;
            t->cursor = k;
            ui->consumed = true;
        } else if (ui->e->type == EV_KEY && focused && !ui->consumed) {
            if (ui->e->key == KEY_ENTER) {
                ui->consumed = true;
                return true;
            }
            /* the address the command bar opened with is selected: typing replaces it */
            int k = ui->e->key;
            if (which == F_CMD && br->cmd_fresh) {
                br->cmd_fresh = false;
                if (KEY_IS_TEXT(k) || (k >= 0x20 && k < 0x7f) || k == KEY_BACKSPACE || KEY_BASE(k) == KEY_DELETE)
                    ui_text_set(t, "");
                if (k == KEY_BACKSPACE || KEY_BASE(k) == KEY_DELETE) {
                    ui->consumed = true;
                    return false;
                }
            }
            if (ui_text_edit(t, k)) {
                ui->consumed = true;
                if (which == F_FIND)
                    br->find_due = hal_ticks_ms();
                if (which == F_CMD)
                    br->cmd_sel = 0;
                if (which == F_LIST)
                    br->list_scroll = 0;
            }
        }
        return false;
    }
    if (!ui->c)
        return false;
    int cw = gfx_text_width_n(f, t->buf, (size_t)t->cursor);
    if (cw - t->scroll > r.w - 4)
        t->scroll = cw - r.w + 4;
    if (cw < t->scroll)
        t->scroll = cw;
    struct canvas inner = *ui->c;
    inner.clip = rect_intersect(ui->c->clip, r);
    int ty = r.y + (r.h - f->height) / 2;
    if (focused && which == F_CMD && br->cmd_fresh && t->len)
        gfx_round(&inner, (struct rect){r.x - t->scroll - 2, ty - 1, gfx_text_width(f, t->buf) + 4, f->height + 2}, 3,
                  ALPHA(style.accent, 110));
    if (t->len)
        gfx_text(&inner, r.x - t->scroll, ty, f, t->buf, style.text);
    else
        gfx_text(&inner, r.x, ty, f, hint, style.dark ? RGB(138, 138, 142) : RGB(128, 128, 134));
    if (focused)
        gfx_fill(&inner, (struct rect){r.x + cw - t->scroll, ty + 1, 1, f->height - 2}, style.text);
    return false;
}

static void spinner(struct canvas *c, int cx, int cy, uint32_t color)
{
    static const signed char pts[8][2] = {{0, -6}, {4, -4}, {6, 0}, {4, 4}, {0, 6}, {-4, 4}, {-6, 0}, {-4, -4}};
    int phase = (int)(hal_ticks_ms() / 90 % 8);
    for (int i = 0; i < 8; i++) {
        int a = 40 + (i - phase + 8) % 8 * 215 / 7;
        gfx_round(c, (struct rect){cx + pts[i][0] - 1, cy + pts[i][1] - 1, 3, 3}, 1, ALPHA(color, a));
    }
}

/* the inside curve where the open tab flows into the panel */
static void notch(struct canvas *c, int x0, int r, int cx, uint32_t color)
{
    int cy = TAB_ROW - r;
    for (int y = cy; y < TAB_ROW; y++)
        for (int x = x0; x < x0 + r; x++) {
            int d = gfx_dist16((x - cx) * 16 + 8, (y - cy) * 16 + 8);
            blend_px(c, x, y, color, gfx_cover(d - r * 16) * 255 / 256);
        }
}

/* the corners of a rectangle cut round: what lies outside them takes the window's color */
static void corners(struct canvas *c, struct rect r, int radius, uint32_t back, bool top)
{
    for (int y = 0; y < radius; y++)
        for (int x = 0; x < radius; x++) {
            int d = gfx_dist16((radius - x) * 16 - 8, (radius - y) * 16 - 8);
            int a = gfx_cover(d - radius * 16) * 255 / 256;
            if (a <= 0)
                continue;
            int xs[2] = {r.x + x, r.x + r.w - 1 - x}, ys[2] = {r.y + y, r.y + r.h - 1 - y};
            for (int k = top ? 0 : 2; k < 4; k++)
                blend_px(c, xs[k & 1], ys[k >> 1], back, a);
        }
}

/* a picture turned about its center: sn and cs are the sine and cosine times 65536 */
static void blit_turned(struct canvas *c, const struct canvas *s, int cx, int cy, int sn, int cs, int radius)
{
    int ex = (ABS(cs) * s->w + ABS(sn) * s->h) / 131072 + 2, ey = (ABS(sn) * s->w + ABS(cs) * s->h) / 131072 + 2;
    struct rect box = rect_intersect(c->clip, (struct rect){cx - ex, cy - ey, 2 * ex, 2 * ey});
    int sw = s->w * 256, sh = s->h * 256, rr = radius * 256;
    for (int y = box.y; y < box.y + box.h; y++)
        for (int x = box.x; x < box.x + box.w; x++) {
            int64_t dx = 2 * (x - cx) + 1, dy = 2 * (y - cy) + 1;
            int u = (int)((dx * cs + dy * sn) >> 9) + sw / 2, v = (int)((dy * cs - dx * sn) >> 9) + sh / 2;
            int edge = MIN(MIN(u, sw - u), MIN(v, sh - v));
            if (edge <= 0)
                continue;
            int qx = u < rr ? rr - u : u > sw - rr ? u - (sw - rr) : 0;
            int qy = v < rr ? rr - v : v > sh - rr ? v - (sh - rr) : 0;
            if (qx && qy)
                edge = MIN(edge, ((rr >> 4) - gfx_dist16(qx >> 4, qy >> 4)) * 16);
            if (edge <= 0)
                continue;
            blend_px(c, x, y, s->px[(size_t)(v >> 8) * (size_t)s->stride + (size_t)(u >> 8)], MIN(edge, 255));
        }
}

static void bubble(struct canvas *c, struct rect v, const char *line)
{
    int tw = MIN(gfx_text_width(&font_small, line) + 20, v.w - 16);
    struct rect r = {v.x + 8, v.y + v.h - 30, tw, 22};
    gfx_round(c, r, 7, ALPHA(RGB(34, 34, 38), 230));
    struct canvas inner = *c;
    inner.clip = rect_intersect(c->clip, (struct rect){r.x + 10, r.y, r.w - 20, r.h});
    gfx_text(&inner, r.x + 10, r.y + (r.h - font_small.height) / 2, &font_small, line, RGB(236, 236, 240));
}

/* ---- the tab strip ---- */

static int slot_w(const struct tab *t)
{
    if (t->label[0])
        return 38 + MIN(gfx_text_width(&font_sans, t->label), 140);
    return t->pinned ? 40 : 32;
}

static void layout(int left)
{
    int x = left - br->strip_off, n = 0;
    for (int i = 0; i < br->ntabs; i++) {
        const struct tab *t = &br->tabs[i];
        const struct group *g = t->group ? &br->groups[t->group - 1] : NULL;
        bool first = g && (i == 0 || br->tabs[i - 1].group != t->group);
        bool last = g && (i + 1 == br->ntabs || br->tabs[i + 1].group != t->group);
        if (first) {
            int w = 30 + gfx_text_width(&font_sans, g->name);
            br->slots[n++] = (struct slot){{x + 6, 9, w, 20}, -1, t->group};
            x += w + 10;
        }
        if (!g || !g->collapsed || i == br->cur) {
            int w = slot_w(t);
            br->slots[n++] = (struct slot){{x, 4, w, TAB_ROW - 4}, i, t->group};
            x += w + 2;
        }
        if (last)
            x += g->collapsed && br->cur != i ? 8 : 28;
    }
    br->nslots = n;
    br->strip_end = x + br->strip_off;
}

static const char *tab_label(const struct tab *t)
{
    return t->label[0] ? t->label : t->title[0] ? t->title : t->url[0] ? t->url : "New Tab";
}

static void draw_tab(struct canvas *c, const struct slot *s, bool hot)
{
    const struct tab *t = &br->tabs[s->tab];
    struct rect r = s->r;
    bool sel = s->tab == br->cur, pill = t->label[0] || t->pinned;
    uint32_t panel = panel_back();
    if (sel) {
        gfx_round(c, (struct rect){r.x, 5, r.w, TAB_ROW + 8}, 9, panel);
        notch(c, r.x - 8, 8, r.x - 8, panel);
        notch(c, r.x + r.w, 8, r.x + r.w + 8, panel);
    } else if (pill) {
        struct rect p = {r.x + 1, 8, r.w - 2, 24};
        gfx_round(c, p, 9, hot ? (style.dark ? RGB(30, 30, 32) : RGB(236, 236, 240))
                                : (style.dark ? RGB(16, 16, 17) : RGB(232, 232, 236)));
        gfx_round_frame(c, p, 9, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 30));
    } else if (hot) {
        gfx_round(c, (struct rect){r.x + 2, 8, r.w - 4, 24}, 8, hover_back());
    }
    int ix = t->label[0] ? r.x + 11 : r.x + (r.w - 16) / 2, iy = sel ? 12 : 12;
    if (sel && loading())
        spinner(c, ix + 8, iy + 8, style.text);
    else
        site_icon(c, ix, iy, 16, t->url, &t->icon);
    if (t->label[0]) {
        struct canvas inner = *c;
        inner.clip = rect_intersect(c->clip, (struct rect){ix + 22, r.y, r.w - 38 + 4, r.h});
        gfx_text(&inner, ix + 22, iy + 8 - font_sans.height / 2, &font_sans, t->label,
                 sel || hot ? style.text : style.secondary);
    }
}

static void tooltip(struct canvas *c, int W)
{
    if (br->tip_tab < 0 || br->tip_tab >= br->ntabs || br->drag_tab >= 0 || br->cmd_open ||
        (br->focus_mode && !br->peek) ||
        hal_ticks_ms() - br->tip_since < 550)
        return;
    const struct slot *s = NULL;
    for (int i = 0; i < br->nslots; i++)
        if (br->slots[i].tab == br->tip_tab)
            s = &br->slots[i];
    if (!s)
        return;
    const struct tab *t = &br->tabs[br->tip_tab];
    char host[128];
    host_of(t->url, host, sizeof(host));
    const char *title = t->title[0] ? t->title : t->url[0] ? host : "New Tab";
    int w = MIN(MAX(gfx_text_width(&font_bold, title), gfx_text_width(&font_small, host)) + 24, 380);
    int h = host[0] && t->title[0] ? 44 : 30;
    /* the page's last picture under the name, when there is one */
    bool pic = false;
    if (t->url[0] && !internal(t->url) && t->id != tab()->id) {
        int key = (int)url_hash(t->url);
        if (key != br->tip_pic_id) {
            char path[PATH_MAX_LEN];
            picture_free(&br->tip_pic);
            br->tip_pic_id = key;
            thumb_path(t->url, path, sizeof(path));
            if (!vfs_exists(path) || picture_load(path, &br->tip_pic) != 0)
                picture_free(&br->tip_pic);
        }
        pic = br->tip_pic.px != NULL;
    }
    int pw = THUMB_W, ph = THUMB_H;
    if (pic) {
        w = MAX(w, pw + 16);
        h += ph + 8;
    }
    bool side = br->sidebar && (!br->focus_mode || br->peek);
    struct rect r = side ? (struct rect){s->r.x + s->r.w + 12, CLAMP(s->r.y, 6, c->h - h - 6), w, h}
                         : (struct rect){CLAMP(s->r.x + s->r.w / 2 - w / 2, 6, W - w - 6), TAB_ROW + 4, w, h};
    gfx_drop_shadow(c, r, 9, 12, 60, 3);
    gfx_round(c, r, 9, style.dark ? RGB(42, 42, 45) : RGB(255, 255, 255));
    gfx_round_frame(c, r, 9, style.dark ? ALPHA(0xffffffffu, 26) : ALPHA(0, 26));
    struct canvas inner = *c;
    inner.clip = rect_intersect(c->clip, (struct rect){r.x + 12, r.y, r.w - 24, r.h});
    gfx_text(&inner, r.x + 12, r.y + 7, &font_bold, title, style.text);
    if (host[0] && t->title[0])
        gfx_text(&inner, r.x + 12, r.y + 25, &font_small, host, style.secondary);
    if (pic) {
        struct rect pr = {r.x + 8, r.y + r.h - ph - 8, r.w - 16, ph};
        struct canvas src = canvas_make(br->tip_pic.px, br->tip_pic.w, br->tip_pic.h);
        gfx_scale_smooth(c, pr, &src, 255);
        corners(c, pr, 6, style.dark ? RGB(42, 42, 45) : RGB(255, 255, 255), false);
    }
    br->tip_shown = true;
}

static void window_controls(struct ui *ui, int x0, int y, int bw)
{
    for (int k = 0; k < 3; k++) {
        struct rect r = {x0 + k * bw + 1, y, bw - 2, 28};
        bool hot, hit = area(ui, r, &hot);
        if (hit) {
            if (k == 0)
                wm_minimize(br->win);
            else if (k == 1)
                wm_toggle_maximize(br->win);
            else
                wm_close(br->win);
            return;
        }
        if (!ui->c)
            continue;
        uint32_t fg = style.dark ? RGB(214, 214, 218) : RGB(40, 40, 44);
        if (hot) {
            gfx_round(ui->c, r, 7, k == 2 ? RGB(210, 46, 38) : hover_back());
            if (k == 2)
                fg = RGB(255, 255, 255);
        }
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (k == 0) {
            gfx_fill(ui->c, (struct rect){cx - 5, cy, 11, 1}, fg);
        } else if (k == 1) {
            if (br->win->flags & WIN_MAXIMIZED) {
                gfx_round_frame(ui->c, (struct rect){cx - 5, cy - 3, 9, 9}, 2, fg);
                gfx_fill(ui->c, (struct rect){cx - 2, cy - 6, 8, 1}, fg);
                gfx_fill(ui->c, (struct rect){cx + 6, cy - 6, 1, 8}, fg);
            } else {
                gfx_round_frame(ui->c, (struct rect){cx - 5, cy - 5, 11, 11}, 2, fg);
            }
        } else {
            gfx_line(ui->c, cx - 5, cy - 5, cx + 5, cy + 5, fg);
            gfx_line(ui->c, cx - 5, cy + 5, cx + 5, cy - 5, fg);
        }
    }
}

static void strip(struct ui *ui, int W)
{
    int left = 70, right = MAX(W - 3 * 46 - 40, left + 40);
    struct gui_event *e = ui->e;
    bool hot;
    struct rect lr = {8, 6, 30, 26};
    if (area(ui, lr, &hot)) {
        ui_menu_items(br->win, lr.x, TAB_ROW - 2, logo_menu, (int)ARRAY_SIZE(logo_menu), logo_pick);
        return;
    }
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, lr, 7, hover_back());
        orb(ui->c, lr.x + 15, lr.y + 13, 8, false);
    }
    struct rect cr = {40, 6, 24, 26};
    if (area(ui, cr, &hot)) {
        show_tabs(cr.x, TAB_ROW - 2);
        return;
    }
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, cr, 7, hover_back());
        gfx_symbol(ui->c, cr.x + 6, cr.y + 7, SYM_DOWN, 12, dim());
    }

    layout(left);
    if (br->shown_cur != br->cur) {
        br->shown_cur = br->cur;
        for (int i = 0; i < br->nslots; i++)
            if (br->slots[i].tab == br->cur) {
                struct rect r = br->slots[i].r;
                if (r.x + r.w + 12 > right)
                    br->strip_off += r.x + r.w + 12 - right;
                else if (r.x - 12 < left)
                    br->strip_off -= left - (r.x - 12);
            }
    }
    br->strip_off = CLAMP(br->strip_off, 0, MAX(br->strip_end - right + 8, 0));
    layout(left);
    struct rect band = {left, 0, right - left, TAB_ROW};

    if (e && e->type == EV_WHEEL && rect_contains(band, e->x, e->y)) {
        br->strip_off = CLAMP(br->strip_off + e->wheel * 40, 0, MAX(br->strip_end - right + 8, 0));
        ui->consumed = true;
        return;
    }
    if (e && e->type == EV_MOUSE_UP)
        br->drag_tab = -1;
    if (e && e->type == EV_MOUSE_MOVE) {
        int over = -1;
        for (int i = 0; i < br->nslots; i++)
            if (br->slots[i].tab >= 0 && rect_contains(band, e->x, e->y) && rect_contains(br->slots[i].r, e->x, e->y))
                over = br->slots[i].tab;
        if (over != br->tip_tab) {
            br->tip_tab = over;
            br->tip_since = hal_ticks_ms();
            br->tip_shown = false;
            wm_invalidate(br->win);
        }
        if (over >= 0)
            ui->hot = -100 - over;
        if (br->drag_tab >= 0 && (e->buttons & BUTTON_LEFT) && over >= 0 && over != br->drag_tab &&
            br->tabs[over].pinned == br->tabs[br->drag_tab].pinned) {
            int group = br->tabs[over].group;
            move_tab(br->drag_tab, over);
            br->tabs[over].group = group;
            br->drag_tab = over;
            group_tidy();
            br->shown_cur = br->cur;
            ui->consumed = true;
            return;
        }
    }

    struct canvas clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, (struct rect){left - 10, 0, right - left + 20, TAB_ROW + 10});
    }
    /* the groups' boxes first, the tabs over them */
    for (int i = 0; i < br->nslots; i++) {
        const struct slot *s = &br->slots[i];
        if (s->tab >= 0)
            continue;
        struct group *g = &br->groups[s->group - 1];
        int end = s->r.x + s->r.w;
        for (int k = i + 1; k < br->nslots && br->slots[k].group == s->group && br->slots[k].tab >= 0; k++)
            end = br->slots[k].r.x + br->slots[k].r.w;
        bool open = !g->collapsed || (br->tabs[br->cur].group == s->group);
        struct rect box = {s->r.x - 4, 5, end - s->r.x + 4 + (open ? 26 : 4), 30};
        struct rect head = {s->r.x - 2, 7, s->r.w + 4, 26};
        struct rect x = {end + 4, 11, 18, 18};
        uint32_t tone = group_colors[g->color % (int)ARRAY_SIZE(group_colors)];
        if (ui->c) {
            gfx_round(&clipped, box, 10, style.dark ? RGB(22, 22, 24) : RGB(210, 210, 215));
            gfx_round_frame(&clipped, box, 10, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0, 22));
            gfx_round(&clipped, (struct rect){head.x + 8, head.y + 9, 8, 8}, 4, tone);
            gfx_text(&clipped, head.x + 22, head.y + (head.h - font_sans.height) / 2, &font_sans, g->name,
                     style.text);
        }
        if (area(ui, head, NULL) && rect_contains(band, head.x + head.w / 2, head.y + 4)) {
            g->collapsed = !g->collapsed;
            return;
        }
        if (e && e->type == EV_MOUSE_DOWN && e->button == BUTTON_RIGHT && rect_contains(head, e->x, e->y)) {
            br->menu_group = s->group;
            ui_menu_items(br->win, e->x, e->y, group_menu_items, (int)ARRAY_SIZE(group_menu_items), group_menu_pick);
            ui->consumed = true;
            return;
        }
        if (open) {
            bool xhot;
            if (area(ui, x, &xhot) && rect_contains(band, x.x + 4, x.y + 4)) {
                close_group(s->group);
                return;
            }
            if (ui->c) {
                if (xhot)
                    gfx_round(&clipped, x, 5, hover_back());
                gfx_symbol(&clipped, x.x + 4, x.y + 4, SYM_CLOSE, 10, xhot ? style.text : dim());
            }
        }
    }
    for (int i = 0; i < br->nslots; i++) {
        const struct slot *s = &br->slots[i];
        if (s->tab < 0)
            continue;
        if (ui->c)
            draw_tab(&clipped, s, br->st.hot == -100 - s->tab);
        if (!e || e->type != EV_MOUSE_DOWN || !rect_contains(band, e->x, e->y) || !rect_contains(s->r, e->x, e->y))
            continue;
        ui->consumed = true;
        br->tip_tab = -1;
        if (e->button == BUTTON_MIDDLE) {
            close_tab(s->tab);
        } else if (e->button == BUTTON_RIGHT) {
            tab_menu(s->tab, e->x, e->y);
        } else {
            br->drag_tab = s->tab;
            if (s->tab != br->cur)
                select_tab(s->tab);
            else if (e->clicks >= 2)
                command(CMD_LOCATION);
        }
        return;
    }
    if (br->ntabs < MAX_TABS &&
        icon_button(ui, (struct rect){MIN(br->strip_end - br->strip_off, right) + 4, 8, 26, 24}, SYM_PLUS, 14, dim()))
        command(CMD_NEW_TAB);
}

/* ---- the sidebar: window buttons, pinned tabs as a grid of tiles, New Tab, then the tabs and groups as rows ---- */

static void tab_press(struct ui *ui, struct gui_event *e, int i)
{
    ui->consumed = true;
    br->tip_tab = -1;
    if (e->button == BUTTON_MIDDLE) {
        close_tab(i);
    } else if (e->button == BUTTON_RIGHT) {
        tab_menu(i, e->x, e->y);
    } else {
        br->drag_tab = i;
        if (i != br->cur)
            select_tab(i);
        else if (e->clicks >= 2)
            command(CMD_LOCATION);
    }
}

static void row_close(struct ui *ui, struct canvas *c, struct rect row, int i, bool show)
{
    struct rect x = {row.x + row.w - 24, row.y + (row.h - 20) / 2, 20, 20};
    bool hot;
    if (area(ui, x, &hot) && show) {
        close_tab(i);
        return;
    }
    if (c && show) {
        if (hot)
            gfx_round(c, x, 5, hover_back());
        gfx_symbol(c, x.x + 5, x.y + 5, SYM_CLOSE, 10, hot ? style.text : dim());
    }
}

static void sidebar(struct ui *ui, int H)
{
    struct gui_event *e = ui->e;
    int W = SIDE_W;
    window_controls(ui, 6, 6, 30);
    if (!br)
        return;
    bool hot;
    struct rect lr = {W - 64, 7, 28, 26};
    if (area(ui, lr, &hot)) {
        ui_menu_items(br->win, lr.x, lr.y + lr.h, logo_menu, (int)ARRAY_SIZE(logo_menu), logo_pick);
        return;
    }
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, lr, 7, hover_back());
        orb(ui->c, lr.x + 14, lr.y + 13, 8, false);
    }
    if (icon_button(ui, (struct rect){W - 34, 7, 26, 26}, SYM_SIDEBAR, 14, dim())) {
        command(CMD_SIDEBAR);
        return;
    }

    int n = 0, np = pinned_count(), y = 42;
    int tw = (W - 16 - 3 * 6) / 4, th = 40;
    if (e && e->type == EV_MOUSE_UP)
        br->drag_tab = -1;
    for (int i = 0; i < np; i++) {
        struct rect r = {8 + (i % 4) * (tw + 6), y + (i / 4) * (th + 6), tw, th};
        br->slots[n++] = (struct slot){r, i, 0};
        bool over = ui_hover(ui, r), sel = i == br->cur;
        if (ui->c) {
            gfx_round(ui->c, r, 10, sel ? panel_back() : over ? hover_back()
                                        : (style.dark ? RGB(20, 20, 22) : RGB(232, 232, 236)));
            if (sel && loading())
                spinner(ui->c, r.x + r.w / 2, r.y + r.h / 2, style.text);
            else
                site_icon(ui->c, r.x + (r.w - 18) / 2, r.y + (r.h - 18) / 2, 18, br->tabs[i].url, &br->tabs[i].icon);
        }
        if (e && e->type == EV_MOUSE_DOWN && rect_contains(r, e->x, e->y)) {
            tab_press(ui, e, i);
            return;
        }
    }
    if (np)
        y += ((np + 3) / 4) * (th + 6) + 4;
    struct rect nt = {8, y, W - 16, 30};
    bool nhot;
    if (area(ui, nt, &nhot)) {
        command(CMD_NEW_TAB);
        return;
    }
    if (ui->c) {
        if (nhot)
            gfx_round(ui->c, nt, 8, hover_back());
        gfx_symbol(ui->c, nt.x + 10, nt.y + 8, SYM_PLUS, 14, dim());
        gfx_text(ui->c, nt.x + 34, nt.y + (nt.h - font_sans.height) / 2, &font_sans, "New Tab", style.secondary);
    }
    y += 36;
    if (ui->c)
        gfx_fill(ui->c, (struct rect){14, y - 3, W - 28, 1}, style.dark ? ALPHA(0xffffffffu, 20) : ALPHA(0, 20));

    struct rect list = {0, y, W, MAX(H - y - 8, 0)};
    if (e && e->type == EV_WHEEL && rect_contains(list, e->x, e->y)) {
        br->side_off = CLAMP(br->side_off + e->wheel * 40, 0, MAX(br->side_end - list.h, 0));
        ui->consumed = true;
        return;
    }
    br->side_off = CLAMP(br->side_off, 0, MAX(br->side_end - list.h, 0));
    struct canvas clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, list);
    }
    struct canvas *c = ui->c ? &clipped : NULL;
    int ry = y - br->side_off;
    for (int i = np; i < br->ntabs; i++) {
        struct tab *t = &br->tabs[i];
        struct group *g = t->group ? &br->groups[t->group - 1] : NULL;
        bool first = g && (i == np || br->tabs[i - 1].group != t->group);
        if (first) {
            struct rect head = {8, ry, W - 16, 28};
            br->slots[n++] = (struct slot){head, -1, t->group};
            uint32_t tone = group_colors[g->color % (int)ARRAY_SIZE(group_colors)];
            bool ghot = rect_contains(list, head.x + 4, head.y + 4) && ui_hover(ui, head);
            if (c) {
                if (ghot)
                    gfx_round(c, head, 8, hover_back());
                gfx_symbol(c, head.x + 8, head.y + 8, g->collapsed ? SYM_RIGHT : SYM_DOWN, 10, dim());
                gfx_round(c, (struct rect){head.x + 24, head.y + 10, 8, 8}, 4, tone);
                gfx_text(c, head.x + 40, head.y + (head.h - font_bold.height) / 2, &font_bold, g->name, style.text);
            }
            int gid = t->group;
            if (ghot && e && e->type == EV_MOUSE_DOWN && rect_contains(head, e->x, e->y)) {
                ui->consumed = true;
                if (e->button == BUTTON_RIGHT) {
                    br->menu_group = gid;
                    ui_menu_items(br->win, e->x, e->y, group_menu_items, (int)ARRAY_SIZE(group_menu_items),
                                  group_menu_pick);
                } else if (e->x > head.x + head.w - 26) {
                    close_group(gid);
                } else {
                    g->collapsed = !g->collapsed;
                }
                return;
            }
            if (c && ghot)
                gfx_symbol(c, head.x + head.w - 19, head.y + 9, SYM_CLOSE, 10, dim());
            ry += 30;
        }
        if (g && g->collapsed && i != br->cur)
            continue;
        struct rect row = {g ? 20 : 8, ry, W - (g ? 28 : 16), 32};
        br->slots[n++] = (struct slot){row, i, t->group};
        bool sel = i == br->cur, over = rect_contains(list, row.x + 4, row.y + 4) && ui_hover(ui, row);
        if (c) {
            if (sel)
                gfx_round(c, row, 9, panel_back());
            else if (over)
                gfx_round(c, row, 9, hover_back());
            if (in_split(i) && split_on())
                gfx_round(c, (struct rect){row.x + 3, row.y + 8, 3, 16}, 1, style.accent);
            if (sel && loading())
                spinner(c, row.x + 18, row.y + 16, style.text);
            else
                site_icon(c, row.x + 10, row.y + 8, 16, t->url, &t->icon);
            struct canvas inner = *c;
            inner.clip = rect_intersect(c->clip, (struct rect){row.x + 34, row.y, row.w - 34 - 28, row.h});
            gfx_text(&inner, row.x + 34, row.y + (row.h - font_sans.height) / 2, &font_sans, tab_label(t),
                     sel || over ? style.text : style.secondary);
        }
        if (rect_contains(list, row.x + 4, row.y + 4))
            row_close(ui, c, row, i, sel || over);
        if (!br || i >= br->ntabs)
            return;
        if (e && e->type == EV_MOUSE_DOWN && rect_contains(list, e->x, e->y) && rect_contains(row, e->x, e->y) &&
            !(e->button == BUTTON_LEFT && e->x > row.x + row.w - 26)) {
            tab_press(ui, e, i);
            return;
        }
        if (e && e->type == EV_MOUSE_MOVE && br->drag_tab >= np && (e->buttons & BUTTON_LEFT) && i != br->drag_tab &&
            rect_contains(row, e->x, e->y)) {
            int group = t->group;
            move_tab(br->drag_tab, i);
            br->tabs[i].group = group;
            br->drag_tab = i;
            group_tidy();
            ui->consumed = true;
            return;
        }
        ry += 34;
    }
    br->side_end = ry + br->side_off - y;
    br->nslots = n;
    if (e && e->type == EV_MOUSE_MOVE) {
        int over = -1;
        for (int i = 0; i < n; i++)
            if (br->slots[i].tab >= 0 && rect_contains(br->slots[i].r, e->x, e->y) &&
                (br->slots[i].r.y < y || rect_contains(list, e->x, e->y)))
                over = br->slots[i].tab;
        if (over != br->tip_tab) {
            br->tip_tab = over;
            br->tip_since = hal_ticks_ms();
            br->tip_shown = false;
            wm_invalidate(br->win);
        }
    }
    /* the empty sidebar moves the window, and a double click fills the screen with it */
    if (e && e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT && e->x < W && !ui->consumed) {
        ui->consumed = true;
        if (e->clicks >= 2)
            wm_toggle_maximize(br->win);
        else
            wm_begin_move(br->win);
    }
}

/* ---- the panel's row: back, forward, reload, the site in the middle, downloads ---- */

static void nav_row(struct ui *ui, struct rect p)
{
    bool page = tab()->url[0] && !internal(tab()->url);
    struct rect b = {p.x + 8, p.y + 4, 26, 24};
    if (icon_button(ui, b, SYM_LEFT, 14, dim()) && page)
        command(CMD_BACK);
    b.x += 28;
    if (icon_button(ui, b, SYM_RIGHT, 14, dim()) && page)
        command(CMD_FORWARD);
    b.x += 28;
    if (icon_button(ui, b, SYM_RESTART, 14, dim()))
        command(CMD_RELOAD);
    if (tab()->pinned && tab()->home[0] && strcmp(tab()->home, tab()->url)) {
        b.x += 28;
        if (icon_button(ui, b, SYM_PIN, 13, style.accent))
            command(CMD_PIN_HOME);
    }
    int bx = b.x + 34;
    if (br->private_mode) {
        struct rect pv = {bx, p.y + 6, 62, 20};
        if (pill_button(ui, pv, "Private", RGB(88, 54, 150), RGB(255, 255, 255), false))
            command(CMD_PRIVATE);
        bx += 68;
    }
    if (!default_profile()) {
        struct rect pr = {bx, p.y + 6, MIN(gfx_text_width(&font_sans, br->profile) + 20, 140), 20};
        if (pill_button(ui, pr, br->profile, profile_color(), RGB(255, 255, 255), false))
            show_profiles(pr.x, pr.y + pr.h + 4);
    }
    struct rect d = {p.x + p.w - 34, p.y + 4, 26, 24};
    bool dhot;
    if (area(ui, d, &dhot))
        show_downloads(d.x - 200, d.y + d.h + 4);
    if (ui->c) {
        uint32_t fg = br->new_download ? style.accent : dhot ? style.text : dim();
        int cx = d.x + d.w / 2, cy = d.y + 5;
        if (dhot)
            gfx_round(ui->c, d, 7, hover_back());
        gfx_fill(ui->c, (struct rect){cx, cy, 1, 11}, fg);
        gfx_line(ui->c, cx - 4, cy + 7, cx, cy + 11, fg);
        gfx_line(ui->c, cx + 4, cy + 7, cx, cy + 11, fg);
        gfx_fill(ui->c, (struct rect){cx - 5, cy + 14, 11, 1}, fg);
    }
    if (ui->c && br->new_download)
        gfx_round(ui->c, (struct rect){d.x + 19, d.y + 3, 6, 6}, 3, style.accent);
    if (!tab()->url[0])
        return;
    /* the site and the title, quietly in the middle: a click opens the command bar */
    char host[128];
    if (internal(tab()->url))
        str_copy(host, "Obscura", sizeof(host));
    else
        host_of(tab()->url, host, sizeof(host));
    const char *title = tab()->title[0] ? tab()->title : "";
    bool lock = !strncmp(tab()->url, "https://", 8);
    int room = MAX(p.w - 2 * 140, 80);
    int hw = gfx_text_width(&font_small, host), tw = title[0] ? gfx_text_width(&font_small, title) + 18 : 0;
    int w = MIN((lock ? 16 : 0) + hw + tw, room);
    bool bookmarked = page && bookmark_find(tab()->url) >= 0;
    int extra = page ? 26 + (zoom() != 100 ? 50 : 0) : 0;
    struct rect t = {p.x + (p.w - w - extra) / 2, p.y + 6, w, 20};
    bool hot;
    if (area(ui, (struct rect){t.x - 8, t.y - 1, t.w + 16, t.h + 2}, &hot)) {
        open_cmd(page ? tab()->url : "");
        return;
    }
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, (struct rect){t.x - 8, t.y - 1, t.w + 16, t.h + 2}, 7, hover_back());
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, t);
        int x = t.x, ty = t.y + (t.h - font_small.height) / 2;
        if (lock) {
            gfx_symbol(&inner, x, t.y + 4, SYM_LOCK, 11, dim());
            x += 16;
        }
        x += gfx_text(&inner, x, ty, &font_small, host, style.secondary);
        if (title[0]) {
            gfx_text(&inner, x + 6, ty, &font_small, "-", dim());
            gfx_text(&inner, x + 18, ty, &font_small, title, dim());
        }
    }
    if (!page)
        return;
    struct rect star = {t.x + t.w + 12, p.y + 5, 22, 22};
    if (icon_button(ui, star, SYM_STAR, 13, bookmarked ? RGB(255, 196, 64) : dim()))
        command(CMD_BOOKMARK);
    if (zoom() != 100) {
        char z[8];
        ksnprintf(z, sizeof(z), "%d%%", zoom());
        if (pill_button(ui, (struct rect){star.x + 26, p.y + 6, 44, 20}, z,
                        style.dark ? RGB(44, 44, 47) : RGB(228, 228, 232), style.secondary, false))
            command(CMD_ZOOM_RESET);
    }
}

/* ---- the start page ---- */

struct card_spec {
    const char *title, *url;
    uint32_t top, bottom;
};

static const struct card_spec feature_cards[3] = {
    {"Your Bookmarks", INTERNAL "bookmarks", RGB(66, 36, 104), RGB(26, 16, 44)},
    {"Pages You Read", INTERNAL "history", RGB(222, 150, 84), RGB(86, 120, 170)},
    {"Make It Yours", INTERNAL "settings", RGB(34, 52, 60), RGB(14, 18, 22)},
};

static void card_make(struct canvas *out, const char *url, const char *title, uint32_t top, uint32_t bottom)
{
    if (!canvas_alloc(out, THUMB_W, THUMB_H))
        return;
    struct rect all = {0, 0, THUMB_W, THUMB_H};
    struct picture pic = {0};
    char path[PATH_MAX_LEN];
    bool shot = false;
    if (!internal(url)) {
        thumb_path(url, path, sizeof(path));
        shot = vfs_exists(path) && picture_load(path, &pic) == 0;
    }
    if (shot) {
        struct canvas src = canvas_make(pic.px, pic.w, pic.h);
        gfx_scale_smooth(out, all, &src, 255);
        picture_free(&pic);
    } else {
        gfx_gradient(out, all, top, bottom);
        if (!internal(url))
            site_icon(out, THUMB_W - 46, 14, 32, url, NULL);
        else
            orb(out, THUMB_W - 32, 30, 14, true);
    }
    /* the title on top, where the cards show above the page's edge */
    gfx_gradient(out, (struct rect){0, 0, THUMB_W, THUMB_H * 70 / 100}, ALPHA(0, 215), ALPHA(0, 0));
    struct rect tr = {12, 10, THUMB_W - 24, 2 * (font_title.height + 2)};
    struct canvas inner = *out;
    inner.clip = rect_intersect(out->clip, tr);
    ui_wrap_text(&inner, tr, &font_title, title, RGB(255, 255, 255));
}

static void cards_refresh(void)
{
    uint32_t key = 0x9e3779b9u ^ (uint32_t)style.dark;
    for (int i = 0; i < MIN(br->nhistory, 3); i++)
        key = key * 31 + url_hash(br->history[i].url);
    if (key == br->cards_key && br->cards[0].px)
        return;
    br->cards_key = key;
    for (int i = 0; i < 3; i++) {
        canvas_free(&br->cards[i]);
        if (i < br->nhistory) {
            const struct visit *h = &br->history[i];
            str_copy(br->card_url[i], h->url, URL_MAX);
            uint32_t tone = site_color(h->url);
            card_make(&br->cards[i], h->url, h->title[0] ? h->title : h->url, color_shade(tone, -40),
                      color_shade(tone, -150));
        } else {
            const struct card_spec *f = &feature_cards[i];
            str_copy(br->card_url[i], f->url, URL_MAX);
            card_make(&br->cards[i], f->url, f->title, f->top, f->bottom);
        }
    }
}

static void submit_ask(void)
{
    char text[sizeof(br->ask.buf)];
    str_copy(text, br->ask.buf, sizeof(text));
    char *s = text;
    while (*s == ' ')
        s++;
    char urls[MAX_CHIPS][URL_MAX];
    int nurls = 0;
    char site[160] = "";
    for (int i = 0; i < br->nchips; i++) {
        if (*s && !strncmp(br->chips[i].url, "http", 4) && !site[0]) {
            char host[128];
            host_of(br->chips[i].url, host, sizeof(host));
            ksnprintf(site, sizeof(site), " site:%s", host);
        } else if (!*s || strncmp(br->chips[i].url, "http", 4)) {
            str_copy(urls[nurls++], br->chips[i].url, URL_MAX);
        }
    }
    br->nchips = 0;
    ui_text_set(&br->ask, "");
    br->focus = F_NONE;
    if (*s) {
        char q[sizeof(text) + 160];
        ksnprintf(q, sizeof(q), "%s%s", s, site);
        go(q);
    }
    for (int i = 0; i < nurls; i++) {
        if (i == 0 && !*s)
            navigate(urls[i]);
        else
            new_tab(urls[i]);
    }
}

static void ask_box(struct ui *ui, struct rect b)
{
    if (ui->c) {
        if (!style.dark)
            gfx_drop_shadow(ui->c, b, 12, 14, 24, 3);
        gfx_round(ui->c, b, 12, style.dark ? RGB(44, 44, 45) : RGB(255, 255, 255));
        if (!style.dark)
            gfx_round_frame(ui->c, b, 12, ALPHA(0, 26));
        gfx_symbol(ui->c, b.x + 16, b.y + 18, SYM_SEARCH, 14, dim());
    }
    if (field(ui, (struct rect){b.x + 40, b.y + 12, b.w - 56, 26}, &br->ask, "Ask anything...", F_ASK, &font_sans)) {
        submit_ask();
        return;
    }
    /* the second row: "+ Add tabs or files", the chips added, and the send button */
    int ry = b.y + b.h - 36;
    int aw = br->nchips ? 30 : 40 + gfx_text_width(&font_sans, "Add tabs or files");
    struct rect add = {b.x + 8, ry + 2, aw, 28};
    bool hot;
    if (area(ui, add, &hot)) {
        show_add(add.x, add.y + add.h + 4);
        return;
    }
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, add, 7, hover_back());
        gfx_symbol(ui->c, add.x + 8, add.y + 7, SYM_PLUS, 14, hot ? style.text : dim());
        if (!br->nchips)
            gfx_text(ui->c, add.x + 32, add.y + (add.h - font_sans.height) / 2, &font_sans, "Add tabs or files", dim());
    }
    int cx = add.x + add.w + 6;
    for (int i = 0; i < br->nchips && cx < b.x + b.w - 120; i++) {
        int tw = MIN(gfx_text_width(&font_small, br->chips[i].title), 130);
        struct rect chip = {cx, ry + 5, tw + 50, 22};
        struct rect x = {chip.x + chip.w - 20, chip.y + 3, 16, 16};
        bool xh;
        if (area(ui, x, &xh)) {
            memmove(&br->chips[i], &br->chips[i + 1], sizeof(br->chips[0]) * (size_t)(br->nchips - i - 1));
            br->nchips--;
            return;
        }
        if (ui->c) {
            gfx_round(ui->c, chip, 8, style.dark ? RGB(60, 60, 62) : RGB(236, 236, 240));
            site_icon(ui->c, chip.x + 6, chip.y + 4, 14, br->chips[i].url, NULL);
            struct canvas inner = *ui->c;
            inner.clip = rect_intersect(ui->c->clip, (struct rect){chip.x + 24, chip.y, tw, chip.h});
            gfx_text(&inner, chip.x + 24, chip.y + (chip.h - font_small.height) / 2, &font_small,
                     br->chips[i].title, style.text);
            gfx_symbol(ui->c, x.x + 4, x.y + 4, SYM_CLOSE, 8, xh ? style.text : dim());
        }
        cx += chip.w + 6;
    }
    bool ready = br->ask.len || br->nchips;
    struct rect send = {b.x + b.w - 36, ry + 3, 26, 26};
    bool sh, hit = area(ui, send, &sh);
    if (ui->c) {
        uint32_t back = ready ? style.dark ? RGB(236, 236, 238) : RGB(30, 30, 32)
                              : style.dark ? RGB(58, 58, 60) : RGB(232, 232, 236);
        gfx_round(ui->c, send, 7, sh ? color_shade(back, style.dark ? -20 : 30) : back);
        gfx_symbol(ui->c, send.x + 6, send.y + 6, SYM_UP, 14,
                   ready ? style.dark ? RGB(20, 20, 22) : RGB(255, 255, 255) : dim());
    }
    if (hit && ready)
        submit_ask();
}

static const char *const headline[2] = {"Search the web. Find in pages. Keep bookmarks.",
                                        "Group tabs and download files, all in SynthOS."};

static void link_text(struct ui *ui, int *x, int y, const char *text, bool link, int *hit)
{
    int w = gfx_text_width(&font_small, text);
    if (link) {
        bool hot;
        if (area(ui, (struct rect){*x, y - 2, w, font_small.height + 4}, &hot))
            *hit = 1;
        if (ui->c)
            gfx_fill(ui->c, (struct rect){*x, y + font_small.height - 1, w, 1}, hot ? style.text : style.secondary);
    }
    if (ui->c)
        gfx_text(ui->c, *x, y, &font_small, text, link ? style.text : style.secondary);
    *x += w;
}

static void intro(struct ui *ui, struct rect v, int y, int bw)
{
    int cx = v.x + v.w / 2;
    if (ui->c) {
        for (int i = 0; i < 2; i++)
            gfx_text_box(ui->c, (struct rect){v.x, y + i * 30, v.w, 30}, &font_title, headline[i], style.text,
                         ALIGN_CENTER);
        gfx_text_box(ui->c, (struct rect){v.x, y + 70, v.w, 18}, &font_small,
                     "Make Obscura your browser and pick a search engine to get started.", style.secondary,
                     ALIGN_CENTER);
    }
    int cw = (bw + 40 - 12) / 2, x0 = cx - cw - 6, cy = y + 104;
    uint32_t card = style.dark ? RGB(12, 12, 13) : RGB(236, 236, 239);
    for (int k = 0; k < 2; k++) {
        struct rect r = {x0 + k * (cw + 12), cy, cw, 48};
        if (ui->c) {
            gfx_round(ui->c, r, 9, card);
            if (k == 0)
                orb(ui->c, r.x + 24, r.y + 24, 10, false);
            else
                gfx_round(ui->c, (struct rect){r.x + 14, r.y + 14, 20, 20}, 10, engines[br->engine].color);
            if (k == 1) {
                char l[2] = {engines[br->engine].name[0], 0};
                gfx_text_box(ui->c, (struct rect){r.x + 14, r.y + 14, 20, 20}, &font_bold, l, RGB(255, 255, 255),
                             ALIGN_CENTER);
            }
            gfx_text(ui->c, r.x + 46, r.y + (r.h - font_sans.height) / 2, &font_sans,
                     k == 0 ? "Obscura" : engines[br->engine].name, style.text);
        }
        struct rect b = {r.x + r.w - 104, r.y + 9, 94, 30};
        uint32_t grey = style.dark ? RGB(44, 44, 47) : RGB(222, 222, 226);
        if (k == 0) {
            bool is_default = gui_default_browser() == &obscura_app;
            if (is_default) {
                pill_button(ui, b, "Default", grey, style.secondary, true);
            } else if (pill_button(ui, b, "Make Default", style.dark ? RGB(255, 255, 255) : RGB(30, 30, 32),
                                   style.dark ? RGB(18, 18, 20) : RGB(255, 255, 255), false)) {
                gui_setting("browser", "obscura");
            }
        } else if (pill_button(ui, b, "Change", grey, style.text, false)) {
            show_engines(b.x, b.y + b.h + 4);
        }
    }
    const char *parts[] = {"Coming from another browser? ", "Import bookmarks", " or ", "dismiss", "."};
    int w = 0;
    for (int i = 0; i < 5; i++)
        w += gfx_text_width(&font_small, parts[i]);
    int x = cx - w / 2, imp = 0, dis = 0;
    for (int i = 0; i < 5; i++)
        link_text(ui, &x, cy + 62, parts[i], i == 1 || i == 3, i == 1 ? &imp : &dis);
    if (imp)
        command(CMD_IMPORT);
    if (dis)
        app_state_set_on("obscura", "dismissed", true);
}

/* the pages read last, as cards tilted at the bottom of the page */
static void tilted_cards(struct ui *ui, struct rect v)
{
    static const struct {
        int dx, dy, sn, cs;
    } place[3] = {{-196, 26, 9102, 64905}, {0, 6, -7987, 65047}, {196, 32, 15855, 63589}};
    cards_refresh();
    int bottom = v.y + v.h;
    int hot = -1;
    for (int i = 0; i < 3; i++) {
        struct rect hit = {v.x + v.w / 2 + place[i].dx - THUMB_W / 2 + 10, bottom - 96 + place[i].dy, THUMB_W - 20,
                           96 - place[i].dy};
        bool h;
        if (area(ui, hit, &h)) {
            char url[URL_MAX];
            str_copy(url, br->card_url[i], sizeof(url));
            if (internal(url))
                new_tab(url);
            else
                navigate(url);
            return;
        }
        if (h)
            hot = i;
    }
    if (!ui->c)
        return;
    struct canvas inner = *ui->c;
    inner.clip = rect_intersect(ui->c->clip, v);
    for (int i = 0; i < 3; i++) {
        if (!br->cards[i].px)
            continue;
        int lift = hot == i ? 14 : 0;
        blit_turned(&inner, &br->cards[i], v.x + v.w / 2 + place[i].dx, bottom - 24 + place[i].dy - lift, place[i].sn,
                    place[i].cs, 12);
    }
}

static void start_page(struct ui *ui, struct rect v)
{
    if (ui->c)
        gfx_clear(ui->c, v, panel_back());
    bool show_intro = !app_state_on("obscura", "dismissed", false);
    bool cards = v.h >= 560 && v.w >= 640;
    int bw = MIN(v.w - 64, 560), bh = 96;
    int stack = 48 + 28 + bh + (show_intro ? 36 + 186 : 0);
    int top = v.y + MAX((v.h - (cards ? 130 : 0) - stack) / 2 - 8, 16);
    int cx = v.x + v.w / 2;
    if (ui->c)
        orb(ui->c, cx, top + 24, 23, true);
    struct rect box = {cx - bw / 2, top + 48 + 28, bw, bh};
    ask_box(ui, box);
    if (show_intro)
        intro(ui, v, box.y + bh + 36, bw);
    if (cards)
        tilted_cards(ui, v);
}

/* ---- obscura:// pages: history, bookmarks, downloads and settings ---- */

struct row {
    const char *title, *url;
    int index;
};

static void files_refresh(void)
{
    kfree(br->files);
    br->files = NULL;
    br->nfiles = 0;
    char dir[PATH_MAX_LEN];
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    int cap = 0;
    while (vfs_readdir(&d, &ent) == 0 && br->nfiles < 200) {
        if (ent.name[0] == '.' || ent.type == VN_DIR)
            continue;
        if (br->nfiles == cap) {
            cap = cap ? cap * 2 : 32;
            char (*g)[NAME_MAX_LEN] = krealloc(br->files, (size_t)cap * NAME_MAX_LEN);
            if (!g)
                break;
            br->files = g;
        }
        str_copy(br->files[br->nfiles++], ent.name, NAME_MAX_LEN);
    }
    vfs_closedir(&d);
}

static void clear_history_done(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (!br || !value)
        return;
    br->nhistory = 0;
    visits_save();
    char dir[PATH_MAX_LEN];
    obscura_dir("thumbs", dir, sizeof(dir));
    vfs_remove_tree(dir);
    br->cards_key = 0;
    wm_invalidate(br->win);
}

static void list_page(struct ui *ui, struct rect v, int kind)
{
    static const char *const titles[] = {"History", "Bookmarks", "Downloads"};
    static const char *const empty[] = {"Pages you open show up here.", "Press Ctrl+D on a page to keep it here.",
                                        "Files you download go to the Downloads folder."};
    if (ui->c)
        gfx_clear(ui->c, v, panel_back());
    int cw = MIN(v.w - 64, 760), x0 = v.x + (v.w - cw) / 2, y = v.y + 24;
    if (ui->c)
        gfx_text(ui->c, x0, y, &font_large, titles[kind], style.text);
    struct rect sb = {x0 + cw - 380, y + 2, 240, 30};
    if (ui->c) {
        gfx_round(ui->c, sb, 8, style.dark ? RGB(40, 40, 42) : RGB(236, 236, 240));
        gfx_symbol(ui->c, sb.x + 10, sb.y + 8, SYM_SEARCH, 14, dim());
    }
    field(ui, (struct rect){sb.x + 32, sb.y + 2, sb.w - 40, 26}, &br->filter, "Search", F_LIST, &font_sans);
    static const char *const actions[][2] = {{"Clear History", NULL}, {"Import...", "Export..."}, {"Open Folder", NULL}};
    struct rect ab = {x0 + cw - 130, y + 2, 130, 30};
    if (actions[kind][1]) {
        ab.w = 62;
        if (pill_button(ui, (struct rect){ab.x + 68, ab.y, 62, 30}, actions[kind][1],
                        style.dark ? RGB(44, 44, 47) : RGB(228, 228, 232), style.text, false))
            command(CMD_EXPORT);
    }
    if (pill_button(ui, ab, actions[kind][0], style.dark ? RGB(44, 44, 47) : RGB(228, 228, 232), style.text, false)) {
        if (kind == 0) {
            ui_confirm(br->win, "Clear History", "Forget every page visited and the pictures of them?",
                       clear_history_done);
        } else if (kind == 1) {
            command(CMD_IMPORT);
        } else {
            char dir[PATH_MAX_LEN];
            path_join(user_home(), "Downloads", dir, sizeof(dir));
            gui_app_launch(gui_app_find("Files"), dir);
        }
        return;
    }
    /* the rows that pass the filter */
    static struct row rows[MAX_BOOKMARKS + 200];
    static char paths[200][PATH_MAX_LEN];
    int n = 0;
    char dir[PATH_MAX_LEN];
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    int total = kind == 0 ? br->nhistory : kind == 1 ? br->nbookmarks : br->nfiles;
    for (int i = 0; i < total; i++) {
        struct row r = {NULL, NULL, i};
        if (kind == 0) {
            r.title = br->history[i].title;
            r.url = br->history[i].url;
        } else if (kind == 1) {
            r.title = br->bookmarks[i].title;
            r.url = br->bookmarks[i].url;
        } else {
            path_join(dir, br->files[i], paths[n], sizeof(paths[0]));
            r.title = br->files[i];
            r.url = paths[n];
        }
        if (br->filter.len && !find_nocase(r.title, br->filter.buf) && !find_nocase(r.url, br->filter.buf))
            continue;
        rows[n++] = r;
    }
    struct rect list = {x0, y + 56, cw, v.y + v.h - (y + 56)};
    int row_h = 46, content = n * row_h;
    struct gui_event *e = ui->e;
    if (e && e->type == EV_WHEEL && rect_contains(list, e->x, e->y)) {
        br->list_scroll += e->wheel * 60;
        ui->consumed = true;
    }
    br->list_scroll = CLAMP(br->list_scroll, 0, MAX(content - list.h + 12, 0));
    if (!n) {
        if (ui->c)
            gfx_text_box(ui->c, (struct rect){list.x, list.y + 40, list.w, 20}, &font_sans,
                         br->filter.len ? "Nothing matches." : empty[kind], style.secondary, ALIGN_CENTER);
        return;
    }
    struct canvas inner;
    if (ui->c) {
        inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, list);
    }
    for (int i = 0; i < n; i++) {
        struct rect r = {list.x, list.y + i * row_h - br->list_scroll, list.w, row_h - 4};
        if (r.y + r.h < list.y || r.y > list.y + list.h)
            continue;
        bool vis = r.y >= list.y && r.y + r.h <= list.y + list.h;
        struct rect x = {r.x + r.w - 34, r.y + (r.h - 24) / 2, 24, 24};
        bool xhot = false, hot;
        bool remove = area(ui, x, &xhot) && vis;
        bool open = area(ui, (struct rect){r.x, r.y, r.w - 40, r.h}, &hot) && vis;
        hot |= xhot;
        if (remove) {
            if (kind == 0) {
                memmove(&br->history[rows[i].index], &br->history[rows[i].index + 1],
                        sizeof(br->history[0]) * (size_t)(br->nhistory - rows[i].index - 1));
                br->nhistory--;
                visits_save();
            } else if (kind == 1) {
                bookmark_remove(rows[i].index);
            } else {
                vfs_unlink(rows[i].url);
                files_refresh();
            }
            return;
        }
        if (open) {
            char url[URL_MAX];
            str_copy(url, rows[i].url, sizeof(url));
            if (kind == 2)
                gui_open_file(url);
            else
                navigate(url);
            return;
        }
        if (!ui->c)
            continue;
        if (hot)
            gfx_round(&inner, r, 9, hover_back());
        if (kind == 2)
            gfx_symbol(&inner, r.x + 14, r.y + (r.h - 18) / 2, SYM_DOC, 18, dim());
        else
            site_icon(&inner, r.x + 14, r.y + (r.h - 18) / 2, 18, rows[i].url, NULL);
        struct canvas text = inner;
        text.clip = rect_intersect(inner.clip, (struct rect){r.x + 44, r.y, r.w - 90, r.h});
        char host[128];
        if (kind == 2)
            str_copy(host, rows[i].url, sizeof(host));
        else
            host_of(rows[i].url, host, sizeof(host));
        gfx_text(&text, r.x + 44, r.y + 5, &font_sans, rows[i].title[0] ? rows[i].title : host, style.text);
        gfx_text(&text, r.x + 44, r.y + 23, &font_small, kind == 2 ? host : rows[i].url, dim());
        if (hot) {
            if (xhot)
                gfx_round(&inner, x, 6, hover_back());
            gfx_symbol(&inner, x.x + 7, x.y + 7, kind == 2 ? SYM_TRASH : SYM_CLOSE, 10, xhot ? style.text : dim());
        }
    }
}

static void settings_page(struct ui *ui, struct rect v)
{
    if (ui->c)
        gfx_clear(ui->c, v, panel_back());
    int cw = MIN(v.w - 64, 640), x0 = v.x + (v.w - cw) / 2, y = v.y + 24 - br->list_scroll;
    struct gui_event *e = ui->e;
    if (e && e->type == EV_WHEEL && rect_contains(v, e->x, e->y)) {
        br->list_scroll = CLAMP(br->list_scroll + e->wheel * 60, 0, MAX(860 - v.h, 0));
        ui->consumed = true;
    }
    struct canvas inner;
    struct canvas *saved = ui->c;
    if (ui->c) {
        inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, v);
        ui->c = &inner;
        gfx_text(ui->c, x0, y, &font_large, "Settings", style.text);
    }
    y += 52;
#define HEAD(text)                                                                                                     \
    do {                                                                                                               \
        if (ui->c)                                                                                                     \
            gfx_text(ui->c, x0 + 4, y, &font_bold, text, style.secondary);                                             \
        y += 24;                                                                                                       \
    } while (0)
    HEAD("General");
    struct rect g = ui_group(ui, x0, y, cw, 4, 46);
    bool is_default = gui_default_browser() == &obscura_app;
    ui_group_label(ui, (struct rect){g.x, g.y, g.w, 46}, "Default browser",
                   is_default ? "Obscura opens links" : NULL, false);
    if (!is_default && ui_button(ui, (struct rect){g.x + g.w - 136, g.y + 8, 122, 30}, "Make Default"))
        gui_setting("browser", "obscura");
    bool intro_on = !app_state_on("obscura", "dismissed", false);
    ui_group_label(ui, (struct rect){g.x, g.y + 46, g.w, 46}, "Show the welcome cards on new tabs", NULL, true);
    if (ui_switch(ui, (struct rect){g.x + g.w - 58, g.y + 46 + 12, 44, 22}, &intro_on))
        app_state_set_on("obscura", "dismissed", !intro_on);
    bool restore = app_state_on("obscura", "restore", true);
    ui_group_label(ui, (struct rect){g.x, g.y + 92, g.w, 46}, "Open the tabs of last time on start",
                   "Pinned tabs always come back", true);
    if (ui_switch(ui, (struct rect){g.x + g.w - 58, g.y + 92 + 12, 44, 22}, &restore))
        app_state_set_on("obscura", "restore", restore);
    ui_group_label(ui, (struct rect){g.x, g.y + 138, g.w, 46}, "Show the bookmarks bar", NULL, true);
    bool bar = br->bar;
    if (ui_switch(ui, (struct rect){g.x + g.w - 58, g.y + 138 + 12, 44, 22}, &bar))
        command(CMD_BAR);
    y += g.h + 22;
    HEAD("Search engine");
    g = ui_group(ui, x0, y, cw, (int)ARRAY_SIZE(engines), 40);
    for (int i = 0; i < (int)ARRAY_SIZE(engines); i++) {
        struct rect r = {g.x, g.y + i * 40, g.w, 40};
        if (ui_radio(ui, (struct rect){r.x + 14, r.y, r.w - 28, r.h}, engines[i].name, i == br->engine))
            set_engine(i);
        if (ui->c && i + 1 < (int)ARRAY_SIZE(engines))
            gfx_fill(ui->c, (struct rect){r.x + 14, r.y + r.h - 1, r.w - 28, 1}, style.separator);
    }
    y += g.h + 22;
    HEAD("Data");
    g = ui_group(ui, x0, y, cw, 3, 46);
    char line[64];
    ksnprintf(line, sizeof(line), "%d page%s", br->nhistory, br->nhistory == 1 ? "" : "s");
    ui_group_label(ui, (struct rect){g.x, g.y, g.w - 150, 46}, "History", line, false);
    if (ui_button(ui, (struct rect){g.x + g.w - 136, g.y + 8, 122, 30}, "Clear..."))
        ui_confirm(br->win, "Clear History", "Forget every page visited and the pictures of them?",
                   clear_history_done);
    ksnprintf(line, sizeof(line), "%d saved", br->nbookmarks);
    ui_group_label(ui, (struct rect){g.x, g.y + 46, g.w - 150, 46}, "Bookmarks", line, false);
    if (ui_button(ui, (struct rect){g.x + g.w - 270, g.y + 46 + 8, 122, 30}, "Import..."))
        command(CMD_IMPORT);
    if (ui_button(ui, (struct rect){g.x + g.w - 136, g.y + 46 + 8, 122, 30}, "Export..."))
        command(CMD_EXPORT);
    ui_group_label(ui, (struct rect){g.x, g.y + 92, g.w - 150, 46}, "Site cookies kept by SynthOS", NULL, true);
    if (ui_button(ui, (struct rect){g.x + g.w - 136, g.y + 92 + 8, 122, 30}, "Clear"))
        http_cookies_clear();
    y += g.h + 22;
    HEAD("Engine");
    g = ui_group(ui, x0, y, cw, 2, 46);
    ui_group_label(ui, (struct rect){g.x, g.y, g.w, 46}, "Obscura engine",
                   br->link ? (br->ready ? "Running" : "Starting") : vfs_exists(ENGINE) ? "Stopped" : "Not installed",
                   false);
    ui_group_label(ui, (struct rect){g.x, g.y + 46, g.w - 150, 46}, "Restart the engine", NULL, true);
    if (ui_button(ui, (struct rect){g.x + g.w - 136, g.y + 46 + 8, 122, 30}, "Restart"))
        engine_stop();
#undef HEAD
    ui->c = saved;
}

/* ---- the page ---- */

static void page_view(struct ui *ui, struct rect v, struct tab *t)
{
    if (!ui->c)
        return;
    const struct picture *p = &t->shot;
    gfx_clear(ui->c, v, RGB(255, 255, 255));
    if (p->px) {
        int at = CLAMP(t->scroll, 0, MAX(p->h - 1, 0));
        if (p->w == v.w) {
            struct canvas src = canvas_make(p->px, p->w, p->h);
            gfx_blit(ui->c, v.x, v.y, &src, (struct rect){0, at, p->w, MIN(v.h, p->h - at)});
        } else {
            int rows = MIN(to_page(v, v.h), p->h - at);
            struct canvas src = canvas_make(p->px + (size_t)at * (size_t)p->w, p->w, MAX(rows, 1));
            gfx_scale_smooth(ui->c, (struct rect){v.x, v.y, v.w, rows * v.w / MAX(p->w, 1)}, &src, 255);
        }
    } else if (t->error[0]) {
        gfx_clear(ui->c, v, panel_back());
        int cy = v.y + v.h / 2 - 80;
        gfx_symbol(ui->c, v.x + v.w / 2 - 20, cy, SYM_GLOBE, 40, style.tertiary);
        gfx_text_box(ui->c, (struct rect){v.x, cy + 54, v.w, 28}, &font_title, "This page could not be opened",
                     style.text, ALIGN_CENTER);
        char host[128];
        host_of(t->url, host, sizeof(host));
        gfx_text_box(ui->c, (struct rect){v.x, cy + 86, v.w, 18}, &font_bold, host, style.secondary, ALIGN_CENTER);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){v.x + 40, v.y, v.w - 80, v.h});
        gfx_text_box(&inner, (struct rect){v.x + 40, cy + 110, v.w - 80, 18}, &font_small, t->error,
                     style.tertiary, ALIGN_CENTER);
    } else {
        gfx_clear(ui->c, v, panel_back());
        spinner(ui->c, v.x + v.w / 2, v.y + v.h / 2 - 10, style.secondary);
    }
    if (t == tab() && (busy() || br->queued)) {
        int w = v.w / 4, x = (int)((hal_ticks_ms() / 3) % (uint64_t)(v.w + w)) - w;
        gfx_fill(ui->c, (struct rect){v.x + MAX(x, 0), v.y, MIN(x + w, v.w) - MAX(x, 0), 2}, style.accent);
    }
    /* where the page is: a thin bar on the right while it scrolls */
    int rows = to_page(v, v.h);
    if (p->px && p->h > rows) {
        int th = MAX(v.h * rows / p->h, 24), ty = v.y + (v.h - th) * t->scroll / MAX(p->h - rows, 1);
        gfx_round(ui->c, (struct rect){v.x + v.w - 7, ty, 4, th}, 2, ALPHA(RGB(60, 60, 64), 120));
    }
}

/* the other side of a split: its last picture, or a card with its name, a little dimmed */
static void other_pane(struct ui *ui, struct rect v, struct tab *t)
{
    if (!ui->c)
        return;
    if (t->shot.px && t->url[0] && !internal(t->url)) {
        page_view(ui, v, t);
    } else {
        gfx_clear(ui->c, v, panel_back());
        int cy = v.y + v.h / 2 - 40;
        if (t->url[0] && !internal(t->url))
            site_icon(ui->c, v.x + v.w / 2 - 16, cy, 32, t->url, &t->icon);
        else
            orb(ui->c, v.x + v.w / 2, cy + 16, 16, true);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){v.x + 20, v.y, v.w - 40, v.h});
        gfx_text_box(&inner, (struct rect){v.x + 20, cy + 44, v.w - 40, 22}, &font_bold, tab_label(t), style.text,
                     ALIGN_CENTER);
        gfx_text_box(&inner, (struct rect){v.x + 20, cy + 68, v.w - 40, 18}, &font_small, "Click to show this side",
                     style.secondary, ALIGN_CENTER);
    }
    gfx_fill(ui->c, v, style.dark ? ALPHA(0, 70) : ALPHA(RGB(255, 255, 255), 60));
}

static void get_engine(struct ui *ui, struct rect v)
{
    bool bundled = gui_bundle_has(&obscura_app);
    if (ui->c) {
        gfx_clear(ui->c, v, panel_back());
        orb(ui->c, v.x + v.w / 2, v.y + v.h / 2 - 80, 28, true);
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

/* ---- the command bar and the find bar ---- */

enum { S_GO, S_TAB, S_URL };

struct sugg {
    int kind, tab;
    enum symbol_id sym;
    const char *title, *detail, *url;
};

static bool looks_like_address(const char *s)
{
    return strstr(s, "://") || (strchr(s, '.') && !strchr(s, ' ')) || !strncmp(s, "about:", 6);
}

static int suggestions(struct sugg *out, int max)
{
    static const char *const pages[][2] = {{"History", INTERNAL "history"}, {"Bookmarks", INTERNAL "bookmarks"},
                                           {"Downloads", INTERNAL "downloads"}, {"Settings", INTERNAL "settings"}};
    static char search_line[96];
    const char *q = br->cmd.buf;
    int n = 0;
    if (q[0]) {
        bool addr = looks_like_address(q);
        ksnprintf(search_line, sizeof(search_line), addr ? "Open" : "Search %s", engines[br->engine].name);
        out[n++] = (struct sugg){S_GO, -1, addr ? SYM_GLOBE : SYM_SEARCH, q, search_line, NULL};
    }
    for (int i = 0; i < br->ntabs && n < max; i++) {
        const struct tab *t = &br->tabs[i];
        if (i == br->cur || !t->url[0] || (q[0] && !find_nocase(t->title, q) && !find_nocase(t->url, q)))
            continue;
        out[n++] = (struct sugg){S_TAB, i, SYM_WINDOW, tab_label(t), "Switch to Tab", t->url};
    }
    for (int i = 0; i < br->nbookmarks && n < max && q[0]; i++)
        if (find_nocase(br->bookmarks[i].title, q) || find_nocase(br->bookmarks[i].url, q))
            out[n++] = (struct sugg){S_URL, -1, SYM_STAR, br->bookmarks[i].title, br->bookmarks[i].url,
                                     br->bookmarks[i].url};
    for (int i = 0; i < br->nhistory && n < max; i++)
        if (!q[0] || find_nocase(br->history[i].title, q) || find_nocase(br->history[i].url, q))
            out[n++] = (struct sugg){S_URL, -1, SYM_CLOCK, br->history[i].title[0] ? br->history[i].title
                                                                                   : br->history[i].url,
                                     br->history[i].url, br->history[i].url};
    for (int i = 0; i < 4 && n < max && q[0]; i++)
        if (find_nocase(pages[i][0], q))
            out[n++] = (struct sugg){S_URL, -1, SYM_GEAR, pages[i][0], "Obscura", pages[i][1]};
    return n;
}

static void suggestion_open(const struct sugg *s)
{
    char text[URL_MAX];
    str_copy(text, s->kind == S_GO ? br->cmd.buf : s->url ? s->url : "", sizeof(text));
    int t = s->tab;
    br->cmd_open = false;
    br->focus = F_NONE;
    /* Alt+Enter opens it next to the current page */
    if (hal_key_modifiers() & KEY_MOD_ALT) {
        if (s->kind == S_TAB)
            split_with(t, true);
        else if (text[0])
            split_open(text);
    } else if (s->kind == S_TAB)
        select_tab(t);
    else if (s->kind == S_GO)
        go_as(text, (hal_key_modifiers() & KEY_MOD_SHIFT) != 0);
    else
        navigate(text);
}

static void cmd_bar(struct ui *ui, struct rect p)
{
    struct sugg list[8];
    int n = suggestions(list, 8);
    br->cmd_sel = CLAMP(br->cmd_sel, 0, MAX(n - 1, 0));
    int w = MIN(p.w - 80, 660), row = 36;
    struct rect r = {p.x + (p.w - w) / 2, p.y + 4, w, 46 + n * row + (n ? 8 : 0)};
    struct gui_event *e = ui->e;
    if (e && e->type == EV_MOUSE_DOWN && !rect_contains(r, e->x, e->y)) {
        br->cmd_open = false;
        br->focus = F_NONE;
        ui->consumed = true;
        return;
    }
    if (ui->c) {
        gfx_drop_shadow(ui->c, r, 14, 28, style.dark ? 120 : 50, 8);
        gfx_round(ui->c, r, 14, style.dark ? RGB(36, 36, 38) : RGB(255, 255, 255));
        gfx_round_frame(ui->c, r, 14, style.dark ? ALPHA(0xffffffffu, 30) : ALPHA(0, 30));
        gfx_symbol(ui->c, r.x + 16, r.y + 16, SYM_SEARCH, 14, dim());
        if (n)
            gfx_fill(ui->c, (struct rect){r.x + 12, r.y + 45, r.w - 24, 1}, style.separator);
    }
    if (field(ui, (struct rect){r.x + 40, r.y + 10, r.w - 56, 26}, &br->cmd, "Search or enter address", F_CMD,
              &font_sans)) {
        if (n)
            suggestion_open(&list[br->cmd_sel]);
        else
            br->cmd_open = false;
        return;
    }
    for (int i = 0; i < n; i++) {
        struct rect s = {r.x + 6, r.y + 50 + i * row, r.w - 12, row - 2};
        bool hot;
        if (area(ui, s, &hot)) {
            suggestion_open(&list[i]);
            return;
        }
        if (e && e->type == EV_MOUSE_MOVE && hot)
            br->cmd_sel = i;
        if (!ui->c)
            continue;
        if (i == br->cmd_sel)
            gfx_round(ui->c, s, 8, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0, 16));
        if (list[i].kind == S_URL && list[i].sym != SYM_GEAR)
            site_icon(ui->c, s.x + 10, s.y + (s.h - 16) / 2, 16, list[i].url, NULL);
        else
            gfx_symbol(ui->c, s.x + 10, s.y + (s.h - 16) / 2, list[i].sym, 16, style.secondary);
        int dw = MIN(gfx_text_width(&font_small, list[i].detail), s.w / 2);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){s.x + 36, s.y, s.w - 56 - dw, s.h});
        gfx_text(&inner, s.x + 36, s.y + (s.h - font_sans.height) / 2,
                 list[i].kind == S_GO ? &font_bold : &font_sans, list[i].title, style.text);
        inner.clip = rect_intersect(ui->c->clip, (struct rect){s.x + s.w - 12 - dw, s.y, dw, s.h});
        gfx_text(&inner, s.x + s.w - 12 - dw, s.y + (s.h - font_small.height) / 2, &font_small, list[i].detail,
                 dim());
    }
}

static struct rect find_rect(struct rect v)
{
    return (struct rect){v.x + v.w - 372, v.y + 10, 360, 40};
}

static void find_bar(struct ui *ui, struct rect v)
{
    struct rect r = find_rect(v);
    if (ui->c) {
        gfx_drop_shadow(ui->c, r, 12, 18, style.dark ? 100 : 40, 4);
        gfx_round(ui->c, r, 12, style.dark ? RGB(40, 40, 43) : RGB(255, 255, 255));
        gfx_round_frame(ui->c, r, 12, style.dark ? ALPHA(0xffffffffu, 30) : ALPHA(0, 30));
        gfx_symbol(ui->c, r.x + 12, r.y + 13, SYM_SEARCH, 14, dim());
    }
    if (field(ui, (struct rect){r.x + 34, r.y + 7, 170, 26}, &br->find, "Find in page", F_FIND, &font_sans)) {
        br->find_due = 0;
        find_step(1);
    }
    if (ui->c) {
        char count[32] = "";
        if (br->find.len)
            ksnprintf(count, sizeof(count), br->find_count ? "%d of %d" : "No matches", br->find_index + 1,
                      br->find_count);
        gfx_text_box(ui->c, (struct rect){r.x + 206, r.y, 70, r.h}, &font_small, count, dim(), ALIGN_RIGHT);
    }
    if (icon_button(ui, (struct rect){r.x + 280, r.y + 8, 24, 24}, SYM_UP, 12, dim()))
        find_step(-1);
    if (icon_button(ui, (struct rect){r.x + 304, r.y + 8, 24, 24}, SYM_DOWN, 12, dim()))
        find_step(1);
    if (icon_button(ui, (struct rect){r.x + 330, r.y + 8, 24, 24}, SYM_CLOSE, 10, dim()))
        find_close();
}

/* ---- the window ---- */

/* Ctrl+Tab: the tabs used last as cards in the middle of the window */
static void switcher(struct ui *ui, struct rect p)
{
    int list[RECENT], n = recent_tabs(list, RECENT);
    int cw = 150, chh = 112, gap = 10, w = n * cw + (n - 1) * gap + 24;
    struct rect r = {p.x + (p.w - w) / 2, p.y + (p.h - chh - 24) / 2, w, chh + 24};
    struct gui_event *e = ui->e;
    if (ui->c) {
        gfx_drop_shadow(ui->c, r, 16, 30, style.dark ? 140 : 60, 8);
        gfx_round(ui->c, r, 16, style.dark ? RGB(34, 34, 37) : RGB(250, 250, 252));
        gfx_round_frame(ui->c, r, 16, style.dark ? ALPHA(0xffffffffu, 30) : ALPHA(0, 26));
    }
    for (int k = 0; k < n; k++) {
        struct tab *t = &br->tabs[list[k]];
        struct rect c = {r.x + 12 + k * (cw + gap), r.y + 12, cw, chh};
        if (e && e->type == EV_MOUSE_DOWN && rect_contains(c, e->x, e->y)) {
            br->switching = false;
            select_tab(list[k]);
            ui->consumed = true;
            return;
        }
        if (!ui->c)
            continue;
        bool sel = k == br->switch_sel;
        gfx_round(ui->c, c, 12, sel ? (style.dark ? RGB(58, 58, 63) : RGB(226, 226, 232)) : panel_back());
        if (sel)
            gfx_round_frame(ui->c, c, 12, style.accent);
        if (t->url[0] && !internal(t->url))
            site_icon(ui->c, c.x + (c.w - 32) / 2, c.y + 18, 32, t->url, &t->icon);
        else
            orb(ui->c, c.x + c.w / 2, c.y + 34, 14, true);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){c.x + 8, c.y, c.w - 16, c.h});
        gfx_text_box(&inner, (struct rect){c.x + 8, c.y + 62, c.w - 16, 18}, &font_bold, tab_label(t), style.text,
                     ALIGN_CENTER);
        char host[128];
        host_of(t->url, host, sizeof(host));
        gfx_text_box(&inner, (struct rect){c.x + 8, c.y + 82, c.w - 16, 16}, &font_small,
                     t->url[0] && !internal(t->url) ? host : "Obscura", style.secondary, ALIGN_CENTER);
    }
}

/* the bookmarks bar under the panel's row: the first bookmarks as chips, the rest behind a button */
static int bar_ids[MAX_BOOKMARKS], bar_menu_mark;
static char bar_names[MAX_BOOKMARKS][64];

static void bar_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index >= 0 && index < MAX_BOOKMARKS && bar_ids[index] >= 0 && bar_ids[index] < br->nbookmarks) {
        char url[URL_MAX];
        str_copy(url, br->bookmarks[bar_ids[index]].url, sizeof(url));
        navigate(url);
    }
}

static const struct menu_item mark_menu[] = {
    {"Open", 0, NULL}, {"Open in New Tab", 0, NULL}, {"Open in Split View", 0, NULL}, {"", 0, NULL},
    {"Open All Bookmarks", 0, NULL}, {"", 0, NULL}, {"Delete", 0, NULL},
};

static void mark_pick(struct window *owner, int index)
{
    UNUSED(owner);
    int b = bar_menu_mark;
    if (b < 0 || b >= br->nbookmarks)
        return;
    char url[URL_MAX];
    str_copy(url, br->bookmarks[b].url, sizeof(url));
    switch (index) {
    case 0: navigate(url); break;
    case 1: new_tab(url); break;
    case 2: split_open(url); break;
    case 4:
        for (int i = 0; i < br->nbookmarks && br->ntabs < MAX_TABS; i++) {
            str_copy(url, br->bookmarks[i].url, sizeof(url));
            new_tab(url);
        }
        break;
    case 6:
        memmove(&br->bookmarks[b], &br->bookmarks[b + 1], sizeof(br->bookmarks[0]) * (size_t)(br->nbookmarks - b - 1));
        br->nbookmarks--;
        list_save("bookmarks", br->bookmarks, br->nbookmarks);
        break;
    }
    wm_invalidate(br->win);
}

static void bookmarks_bar(struct ui *ui, struct rect p)
{
    struct rect bar = {p.x, p.y + NAV_ROW, p.w, BAR_ROW};
    struct gui_event *e = ui->e;
    if (ui->c)
        gfx_fill(ui->c, (struct rect){bar.x + 10, bar.y + bar.h - 1, bar.w - 20, 1}, style.separator);
    if (!br->nbookmarks) {
        if (ui->c)
            gfx_text(ui->c, bar.x + 14, bar.y + (bar.h - font_small.height) / 2, &font_small,
                     "Bookmarks show here: press Ctrl+D on a page to add one.", dim());
        return;
    }
    int x = bar.x + 8, right = bar.x + bar.w - 40, i = 0;
    for (; i < br->nbookmarks; i++) {
        const struct visit *b = &br->bookmarks[i];
        const char *name = b->title[0] ? b->title : b->url;
        int w = MIN(gfx_text_width(&font_small, name) + 34, 170);
        if (x + w > right)
            break;
        struct rect r = {x, bar.y + 3, w, bar.h - 7};
        bool hot;
        if (e && e->type == EV_MOUSE_DOWN && rect_contains(r, e->x, e->y) && e->button != BUTTON_LEFT) {
            ui->consumed = true;
            if (e->button == BUTTON_MIDDLE) {
                char url[URL_MAX];
                str_copy(url, b->url, sizeof(url));
                new_tab(url);
            } else {
                bar_menu_mark = i;
                ui_menu_items(br->win, e->x, e->y, mark_menu, (int)ARRAY_SIZE(mark_menu), mark_pick);
            }
            return;
        }
        if (area(ui, r, &hot)) {
            char url[URL_MAX];
            str_copy(url, b->url, sizeof(url));
            if (hal_key_modifiers() & KEY_MOD_ALT)
                split_open(url);
            else
                navigate(url);
            return;
        }
        if (ui->c) {
            if (hot)
                gfx_round(ui->c, r, 7, hover_back());
            site_icon(ui->c, r.x + 7, r.y + (r.h - 14) / 2, 14, b->url, NULL);
            struct canvas inner = *ui->c;
            inner.clip = rect_intersect(ui->c->clip, (struct rect){r.x + 26, r.y, r.w - 32, r.h});
            gfx_text(&inner, r.x + 26, r.y + (r.h - font_small.height) / 2, &font_small, name, style.secondary);
        }
        x += w + 4;
    }
    if (i < br->nbookmarks) {
        struct rect more = {bar.x + bar.w - 34, bar.y + 3, 26, bar.h - 7};
        if (icon_button(ui, more, SYM_MORE, 14, dim())) {
            static struct menu_item items[MAX_BOOKMARKS];
            int n = 0;
            for (int k = i; k < br->nbookmarks; k++) {
                ksnprintf(bar_names[n], sizeof(bar_names[n]), "%.60s",
                          br->bookmarks[k].title[0] ? br->bookmarks[k].title : br->bookmarks[k].url);
                bar_ids[n] = k;
                items[n] = (struct menu_item){bar_names[n], 0, NULL};
                n++;
            }
            ui_menu_items(br->win, more.x - 160, more.y + more.h + 4, items, n, bar_pick);
        }
    }
}

/* what is over the page in focus mode while the pointer is at the window's edge */
static struct rect peek_rect(const struct window *w)
{
    return br->sidebar ? (struct rect){0, 0, SIDE_W, w->surface.h} : (struct rect){0, 0, w->surface.w, TAB_ROW};
}

static void content(struct ui *ui, struct rect v)
{
    const char *url = tab()->url;
    if (!vfs_exists(ENGINE) && !internal(url))
        get_engine(ui, v);
    else if (!url[0])
        start_page(ui, v);
    else if (!strcmp(url, INTERNAL "history"))
        list_page(ui, v, 0);
    else if (!strcmp(url, INTERNAL "bookmarks"))
        list_page(ui, v, 1);
    else if (!strcmp(url, INTERNAL "downloads"))
        list_page(ui, v, 2);
    else if (internal(url))
        settings_page(ui, v);
    else
        page_view(ui, v, tab());
    if (vfs_exists(ENGINE) && url[0] && !internal(url) && tab()->error[0] && !tab()->shot.px &&
        ui_button(ui, (struct rect){v.x + v.w / 2 - 60, v.y + v.h / 2 + 62, 120, 30}, "Try Again")) {
        char again[URL_MAX];
        str_copy(again, url, sizeof(again));
        navigate(again);
    }
}

/* the split's two sides, the line between them that moves, and a button on each side that closes the other */
static void split_view(struct ui *ui, struct rect v)
{
    struct window *w = ui->win;
    struct gui_event *e = ui->e;
    int mine = br->tabs[br->cur].id == br->split_b ? 1 : 0;
    int other = tab_index(mine ? br->split_a : br->split_b);
    struct rect a = pane_rect(w, 0), b = pane_rect(w, 1), cur = mine ? b : a, rest = mine ? a : b;
    struct rect gap = {a.x + a.w, v.y, SPLIT_GAP, v.h};
    if (ui->c) {
        gfx_fill(ui->c, (struct rect){gap.x, v.y - 1, gap.w, v.h + 1}, chrome_back());
        other_pane(ui, rest, &br->tabs[other]);
    }
    struct canvas clipped;
    struct canvas *saved = ui->c;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, cur);
        ui->c = &clipped;
    }
    content(ui, cur);
    ui->c = saved;
    if (!br)
        return;
    struct rect grip = {gap.x - 3, gap.y, gap.w + 6, gap.h};
    if (e && e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT && rect_contains(grip, e->x, e->y)) {
        br->split_drag = true;
        ui->consumed = true;
    } else if (e && e->type == EV_MOUSE_MOVE && br->split_drag) {
        br->split_pct = CLAMP((e->x - v.x) * 100 / MAX(v.w, 1), 20, 80);
        ui->consumed = true;
    } else if (e && e->type == EV_MOUSE_UP && br->split_drag) {
        br->split_drag = false;
        if (br->link && tab()->url[0] && !internal(tab()->url))
            push_shot();
    }
    for (int side = 0; side < 2; side++) {
        struct rect r = side ? b : a;
        struct rect x = {r.x + r.w - 30, r.y + 8, 22, 22};
        bool hot;
        if (area(ui, x, &hot)) {
            split_close(side ? br->split_a : br->split_b);
            return;
        }
        if (ui->c) {
            gfx_round(ui->c, x, 11, hot ? (style.dark ? RGB(70, 70, 74) : RGB(225, 225, 230))
                                        : ALPHA(style.dark ? RGB(40, 40, 44) : RGB(255, 255, 255), 210));
            gfx_symbol(ui->c, x.x + 6, x.y + 6, SYM_CLOSE, 10, hot ? style.text : style.secondary);
        }
    }
    if (ui->c) {
        corners(ui->c, a, 10, chrome_back(), false);
        corners(ui->c, b, 10, chrome_back(), false);
        gfx_round_frame(ui->c, cur, 10, ALPHA(style.accent, 200));
        if (br->split_drag || ui_hover(ui, grip))
            gfx_round(ui->c, (struct rect){gap.x + gap.w / 2 - 1, v.y + v.h / 2 - 20, 3, 40}, 1, style.accent);
    }
}

static void frame(struct ui *ui, void *data)
{
    UNUSED(data);
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    struct rect p = panel_rect(w), v = view_rect(w), pr = page_rect(w);
    struct gui_event *e = ui->e;
    bool peek = br->focus_mode && br->peek;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, chrome_back());
        gfx_round(ui->c, p, 10, panel_back());
    }
    if (br->cmd_open && e && e->type != EV_TIMER && e->type != EV_RESIZE)
        ui->e = NULL;
    if (peek && e && e->type != EV_TIMER && e->type != EV_RESIZE && rect_contains(peek_rect(w), e->x, e->y))
        ui->e = NULL;
    /* a tab dropped on the page goes next to it */
    if (e && e->type == EV_MOUSE_UP && br->drag_tab == br->cur && tab_index(br->last_id) >= 0 &&
        br->last_id != tab()->id && rect_contains(v, e->x, e->y)) {
        bool right = e->x > v.x + v.w / 2;
        br->drag_tab = -1;
        br->split_a = right ? br->last_id : tab()->id;
        br->split_b = right ? tab()->id : br->last_id;
        if (!br->split_pct)
            br->split_pct = 50;
        if (br->link && tab()->url[0] && !internal(tab()->url))
            push_shot();
        ui->consumed = true;
        return;
    }
    if (!br->focus_mode) {
        if (br->sidebar) {
            sidebar(ui, H);
        } else {
            strip(ui, W);
            if (!br)
                return;
            window_controls(ui, W - 3 * 46, 5, 46);
        }
        if (!br)
            return;
    }
    nav_row(ui, p);
    if (!br)
        return;
    if (br->bar)
        bookmarks_bar(ui, p);
    if (!br)
        return;
    if (split_on())
        split_view(ui, v);
    else
        content(ui, v);
    if (!br)
        return;
    const char *url = tab()->url;
    if (br->find_open && url[0] && !internal(url))
        find_bar(ui, pr);
    if (ui->c) {
        const char *line = br->hover[0] ? br->hover : br->status;
        if (line[0])
            bubble(ui->c, pr, line);
        if (!split_on())
            corners(ui->c, v, 10, chrome_back(), false);
    }
    if (peek) {
        ui->e = br->cmd_open ? NULL : e;
        struct rect o = peek_rect(w);
        if (ui->c) {
            gfx_drop_shadow(ui->c, o, 0, 24, style.dark ? 140 : 60, 4);
            gfx_fill(ui->c, o, chrome_back());
        }
        if (br->sidebar) {
            sidebar(ui, H);
        } else {
            strip(ui, W);
            if (br)
                window_controls(ui, W - 3 * 46, 5, 46);
        }
        if (!br)
            return;
    }
    ui->e = e;
    if (br->cmd_open)
        cmd_bar(ui, p);
    if (br && br->switching)
        switcher(ui, p);
    if (br && ui->c)
        tooltip(ui->c, W);
}

static void paint(struct window *w, struct canvas *c)
{
    ui_run(w, &br->st, c, NULL, frame, NULL);
}

static void page_event(struct window *w, struct gui_event *e)
{
    struct rect v = page_rect(w);
    bool mouse = e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_MOVE || e->type == EV_WHEEL;
    if (mouse && ((br->focus_mode && br->peek && rect_contains(peek_rect(w), e->x, e->y)) || br->split_drag))
        return;
    /* a click on the other side of a split brings that side's page */
    if (e->type == EV_MOUSE_DOWN && split_on() && !br->cmd_open) {
        int mine = br->tabs[br->cur].id == br->split_b ? 1 : 0;
        struct rect o = pane_rect(w, !mine);
        if (rect_contains(o, e->x, e->y) && !rect_contains((struct rect){o.x + o.w - 30, o.y + 8, 22, 22}, e->x, e->y)) {
            select_tab(tab_index(mine ? br->split_a : br->split_b));
            e->handled = true;
            wm_invalidate(w);
            return;
        }
    }
    if (!tab()->url[0] || internal(tab()->url) || !vfs_exists(ENGINE) || !br->link || br->cmd_open)
        return;
    bool inside = rect_contains(v, e->x, e->y) && !(br->find_open && rect_contains(find_rect(v), e->x, e->y)) &&
                  !(split_on() && rect_contains((struct rect){v.x + v.w - 30, v.y + 8, 22, 22}, e->x, e->y));
    if (e->type == EV_MOUSE_DOWN && inside && (e->button == BUTTON_LEFT || e->button == BUTTON_RIGHT ||
                                               e->button == BUTTON_MIDDLE)) {
        char js[1200];
        bool menu = e->button != BUTTON_LEFT;
        int px = to_page(v, e->x - v.x), py = to_page(v, e->y - v.y) + tab()->scroll;
        if (menu)
            ksnprintf(js, sizeof(js), "(function(x,y){" FIND_LINK "var a=L(document.elementFromPoint(x,y),x,y);"
                      "return a?a.href:'';})(%d,%d)",
                      px, py);
        else
            ksnprintf(js, sizeof(js),
                      "(function(x,y){" FIND_LINK "var e=document.elementFromPoint(x,y);if(!e)return '';"
                      "var a=L(e,x,y);"
                      "if(a&&a.href&&a.href.indexOf('javascript:')!=0)return 'nav:'+a.href;"
                      "if(e.focus)e.focus();if(e.click)e.click();"
                      "return e.tagName+((/INPUT|TEXTAREA/.test(e.tagName))?':input':'');})(%d,%d)",
                      px, py);
        br->menu_x = e->x;
        br->menu_y = e->y;
        br->middle = e->button == BUTTON_MIDDLE;
        br->alt_click = e->button == BUTTON_LEFT && (hal_key_modifiers() & KEY_MOD_ALT);
        br->focus = F_NONE;
        push_eval(menu ? R_CONTEXT : R_CLICK, js);
        e->handled = true;
    } else if (e->type == EV_MOUSE_MOVE && inside) {
        br->mouse_x = e->x;
        br->mouse_y = e->y;
        br->moved_at = hal_ticks_ms();
        br->hover_due = true;
    } else if (e->type == EV_WHEEL && inside) {
        scroll_by(e->wheel * 80);
        e->handled = true;
    } else if (e->type == EV_KEY && br->page_input && br->focus == F_NONE) {
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
    } else if (e->type == EV_KEY && br->focus == F_NONE &&
               (e->key == ' ' || e->key == KEY_PGDN || e->key == KEY_PGUP || e->key == KEY_DOWN || e->key == KEY_UP ||
                e->key == KEY_HOME || e->key == KEY_END)) {
        int k = e->key;
        scroll_by(k == KEY_DOWN   ? 60
                         : k == KEY_UP   ? -60
                         : k == KEY_PGUP ? -v.h + 40
                         : k == KEY_HOME ? -1000000
                         : k == KEY_END  ? 1000000
                                         : v.h - 40);
        e->handled = true;
    }
}

static bool shortcut(int k)
{
    /* the keyboard turns Ctrl+letter into a control character, and leaves Ctrl off digits: what is held says */
    unsigned held = hal_key_modifiers();
    int base = KEY_BASE(k);
    bool shift = (k & KEY_MOD_SHIFT) || (held & KEY_MOD_SHIFT);
    bool mod = (k & (KEY_MOD_CTRL | KEY_MOD_SUPER)) || (held & (KEY_MOD_CTRL | KEY_MOD_SUPER)) ||
               (base >= 1 && base <= 26 && base != KEY_BACKSPACE && base != KEY_TAB && base != KEY_ENTER && base != 13);
    if (k >= KEY_COMMAND(0) && k < KEY_COMMAND(CMD_COUNT)) {
        command(k - KEY_COMMAND(0));
        return true;
    }
    if ((k & KEY_MOD_ALT) && (base == KEY_LEFT || base == KEY_RIGHT || base == KEY_HOME)) {
        command(base == KEY_LEFT ? CMD_BACK : base == KEY_RIGHT ? CMD_FORWARD : CMD_HOME);
        return true;
    }
    if (((k & KEY_MOD_ALT) || (held & KEY_MOD_ALT)) && !(held & KEY_MOD_CTRL) && base >= '1' && base <= '9') {
        char names[MAX_PROFILES][28];
        int n = profiles(names, MAX_PROFILES);
        if (base - '1' < n)
            profile_use(names[base - '1']);
        return true;
    }
    if (base == KEY_F3) {
        command(shift ? CMD_FIND_PREV : CMD_FIND_NEXT);
        return true;
    }
    if (base == KEY_F5 || base == KEY_F11) {
        command(base == KEY_F5 ? CMD_RELOAD : CMD_FULLSCREEN);
        return true;
    }
    /* Ctrl+Tab goes through the tabs used last while Ctrl is held, and opens the one chosen when it is let go */
    if (((k & KEY_MOD_CTRL) || (held & KEY_MOD_CTRL)) && base == KEY_TAB) {
        int list[RECENT], n = recent_tabs(list, RECENT);
        if (n < 2)
            return true;
        if (!br->switching) {
            br->switching = true;
            br->switch_sel = shift ? n - 1 : 1;
        } else {
            br->switch_sel = (br->switch_sel + (shift ? n - 1 : 1)) % n;
        }
        return true;
    }
    if (!mod)
        return false;
    if (base >= 1 && base <= 26)
        base += 'a' - 1;
    if (base >= '1' && base <= '9') {
        select_tab(base == '9' ? br->ntabs - 1 : base - '1');
        return true;
    }
    switch (base >= 'A' && base <= 'Z' ? base + 32 : base) {
    case 't': command(shift ? CMD_REOPEN : CMD_NEW_TAB); return true;
    case 'n': command(shift ? CMD_PRIVATE : CMD_NEW_TAB); return true;
    case 'v':
        if (!shift)
            return false;
        command(CMD_PASTE_GO);
        return true;
    case 'w': command(CMD_CLOSE_TAB); return true;
    case 'l':
    case 'k': command(CMD_LOCATION); return true;
    case 'r': command(CMD_RELOAD); return true;
    case 's': command(shift ? CMD_SIDEBAR : CMD_SAVE); return true;
    case 'o': command(CMD_OPEN_FILE); return true;
    case 'd': command(CMD_BOOKMARK); return true;
    case 'b':
        if ((k & KEY_MOD_ALT) || (held & KEY_MOD_ALT))
            command(CMD_BOOKMARKS);
        else if (shift)
            command(CMD_BAR);
        else
            return false;
        return true;
    case 'f': command(shift ? CMD_FOCUS_MODE : CMD_FIND); return true;
    case 'e': command(CMD_SPLIT); return true;
    case 'g': command(shift ? CMD_FIND_PREV : CMD_FIND_NEXT); return true;
    case 'u': command(CMD_SOURCE); return true;
    case 'm':
        if (!shift)
            return false;
        command(CMD_NEXT_PROFILE);
        return true;
    case 'h': command(CMD_HISTORY); return true;
    case 'j': command(CMD_DOWNLOADS); return true;
    case ',': command(CMD_SETTINGS); return true;
    case '=':
    case '+': command(CMD_ZOOM_IN); return true;
    case '-':
    case '_': command(CMD_ZOOM_OUT); return true;
    case '0': command(CMD_ZOOM_RESET); return true;
    case '[': command(CMD_BACK); return true;
    case ']': command(CMD_FORWARD); return true;
    default: return false;
    }
}

static bool escape_key(void)
{
    if (br->switching) {
        br->switching = false;
    } else if (br->cmd_open) {
        br->cmd_open = false;
        br->focus = F_NONE;
    } else if (br->find_open) {
        find_close();
    } else if (br->focus != F_NONE) {
        br->focus = F_NONE;
    } else if (busy() || br->queued) {
        drop_queue();
        br->status[0] = 0;
    } else {
        return false;
    }
    return true;
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_TIMER) {
        pump();
        if (!br)
            return;
        if (busy() || br->queued)
            wm_invalidate(w);
        if (busy() && hal_ticks_ms() - br->sent_at > 60000) {
            str_copy(br->status, "The page took too long.", sizeof(br->status));
            br->wait_kind = R_NONE;
        }
        if (br->find_due && hal_ticks_ms() - br->find_due > 300 && !busy() && !br->queued) {
            br->find_due = 0;
            run_find(0);
        }
        if (br->tip_tab >= 0 && !br->tip_shown && hal_ticks_ms() - br->tip_since > 550)
            wm_invalidate(w);
        if (br->switching && !(hal_key_modifiers() & KEY_MOD_CTRL)) {
            int list[RECENT], n = recent_tabs(list, RECENT);
            br->switching = false;
            if (br->switch_sel < n)
                select_tab(list[br->switch_sel]);
            wm_invalidate(w);
        }
        return;
    }
    if (br->focus_mode && (e->type == EV_MOUSE_MOVE || e->type == EV_LEAVE)) {
        bool edge = e->type == EV_MOUSE_MOVE && (br->sidebar ? e->x < 8 : e->y < 8);
        bool keep = e->type == EV_MOUSE_MOVE && rect_contains(peek_rect(w), e->x, e->y);
        bool now = br->peek ? keep || edge : edge;
        if (now != br->peek && !(e->buttons & BUTTON_LEFT)) {
            br->peek = now;
            br->tip_tab = -1;
            wm_invalidate(w);
        }
    }
    if (e->type == EV_LEAVE && br->tip_tab >= 0) {
        br->tip_tab = -1;
        wm_invalidate(w);
    }
    if (e->type == EV_KEY) {
        if (shortcut(e->key) || (e->key == KEY_ESC && escape_key())) {
            e->handled = true;
            wm_invalidate(w);
            return;
        }
        if (br->cmd_open && (e->key == KEY_UP || e->key == KEY_DOWN)) {
            br->cmd_sel += e->key == KEY_UP ? -1 : 1;
            e->handled = true;
            wm_invalidate(w);
            return;
        }
    }
    page_event(w, e);
    if (e->handled || !br)
        return;
    ui_run(w, &br->st, NULL, e, frame, NULL);
    if (br && e->type == EV_RESIZE && tab()->url[0] && !internal(tab()->url) && br->link)
        push_shot();
}

static void destroy(struct window *w)
{
    session_save();
    if (!(w->flags & (WIN_MAXIMIZED | WIN_FULLSCREEN))) {
        app_state_set_number("obscura", "x", w->frame.x);
        app_state_set_number("obscura", "y", w->frame.y);
        app_state_set_number("obscura", "w", w->surface.w);
        app_state_set_number("obscura", "h", w->surface.h);
    }
    engine_stop();
    for (int i = 0; i < br->ntabs; i++) {
        picture_free(&br->tabs[i].shot);
        picture_free(&br->tabs[i].icon);
    }
    for (int i = 0; i < 3; i++)
        canvas_free(&br->cards[i]);
    picture_free(&br->tip_pic);
    kfree(br->files);
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
    int sw = (int)app_state_number("obscura", "w", 0), sh = (int)app_state_number("obscura", "h", 0);
    if (sw >= 560 && sh >= 360 && sw <= a.w && sh <= a.h) {
        w = sw;
        h = sh;
    }
    br->win = wm_open("Obscura", ICON_BROWSER, w, h, &ops, NULL,
                      WIN_RESIZABLE | WIN_FULLSIZE | WIN_NOTITLE | WIN_OWNCONTROLS);
    if (!br->win) {
        kfree(br);
        br = NULL;
        return;
    }
    br->sidebar = app_state_on("obscura", "sidebar", false);
    br->win->toolbar_h = br->sidebar ? 1 : TAB_ROW;
    br->bar = app_state_on("obscura", "bar", false);
    int sx = (int)app_state_number("obscura", "x", -100000), sy = (int)app_state_number("obscura", "y", -100000);
    if (sw && sx > -100000 && sx >= a.x - w / 2 && sx < a.x + a.w - 80 && sy >= a.y && sy < a.y + a.h - 60)
        wm_move(br->win, sx, sy);
    br->win->min_w = 560;
    br->win->min_h = 360;
    wm_set_menus(br->win, menus, ARRAY_SIZE(menus));
    wm_set_timer(br->win, 30);
    br->ntabs = 1;
    br->tabs[0].id = ++br->tab_ids;
    str_copy(br->profile, app_state("obscura", "profile", "Default"), sizeof(br->profile));
    session_load();
    mru_touch(tab()->id);
    br->drag_tab = br->tip_tab = br->shown_cur = -1;
    br->focus = F_ASK;
    br->engine = CLAMP((int)app_state_number("obscura", "engine", 0), 0, (int)ARRAY_SIZE(engines) - 1);
    visits_load();
    br->nbookmarks = list_load("bookmarks", br->bookmarks, MAX_BOOKMARKS);
    if (tab()->url[0]) {
        br->focus = F_NONE;
        wm_set_title(br->win, tab()->title[0] ? tab()->title : "Obscura");
    }
    if (arg && *arg)
        open_arg(arg, true);
    else if (tab()->url[0] && !internal(tab()->url) && vfs_exists(ENGINE))
        navigate(tab()->url);
}

#endif
