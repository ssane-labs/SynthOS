#include <synth/codepage.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/html.h>
#include <synth/http.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

#define BAR_H 44
#define STATUS_H 22
#define PAD 20
#define INDENT 24
#define MAX_TEXT_W 980
#define HISTORY 48
#define HOME_URL "about:home"
#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

enum { B_TEXT, B_RULE, B_FIELD };

enum {
    CMD_OPEN,
    CMD_SAVE,
    CMD_RELOAD,
    CMD_BACK,
    CMD_FORWARD,
    CMD_HOME,
    CMD_COPY_URL,
    CMD_COOKIES,
};

static const struct menu_item file_items[] = {
    {"Open Location…", KEY_COMMAND(CMD_OPEN), "⌘L"},
    {"Save Page As…", KEY_COMMAND(CMD_SAVE), "⌘S"},
    {"", 0, NULL},
    {"Copy Address", KEY_COMMAND(CMD_COPY_URL), NULL},
    {"Clear Cookies", KEY_COMMAND(CMD_COOKIES), NULL},
};

static const struct menu_item view_items[] = {
    {"Reload", KEY_COMMAND(CMD_RELOAD), "⌘R"},
};

static const struct menu_item go_items[] = {
    {"Back", KEY_COMMAND(CMD_BACK), "⌘["},
    {"Forward", KEY_COMMAND(CMD_FORWARD), "⌘]"},
    {"Home", KEY_COMMAND(CMD_HOME), NULL},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
    {"Go", go_items, ARRAY_SIZE(go_items)},
};

static const char home_page[] =
    "<title>Start Page</title><h1>SynthOS Browser</h1>"
    "<p>Type an address or a few words to search for in the bar above and press Enter.</p>"
    "<form action=\"https://lite.duckduckgo.com/lite/\"><input name=q placeholder=\"Search the web\">"
    "<input type=submit value=Search></form>"
    "<h3>Places to visit</h3><ul>"
    "<li><a href=\"https://example.com/\">example.com</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">A random Wikipedia article</a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a></li>"
    "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
    "<li><a href=\"https://text.npr.org/\">NPR text-only news</a></li>"
    "<li><a href=\"file:///home\">Your files</a></li></ul>"
    "<p><small>Pages are shown as text and links: scripts, style sheets and pictures are not loaded. "
    "HTTPS certificates are checked against the roots in /etc/certs.</small></p>";

struct box {
    int32_t x, y, w;
    uint32_t off;
    uint16_t len;
    uint8_t kind, font, flags, h, asc;
    int32_t link;
};

struct browser {
    char downloaded[PATH_MAX_LEN];
    struct ui_state st;
    struct ui_text addr;
    char url[HTTP_URL_MAX];
    char pending[HTTP_URL_MAX];
    char *post;
    size_t post_len;
    bool loading, push;
    struct html_doc doc;
    char *source;
    size_t source_len;
    struct box *boxes;
    size_t box_cap;
    int box_count;
    int content_h, layout_w;
    int scroll;
    int hover;
    int focus;
    int edit_field;
    int addr_id;
    bool secure;
    char tls[96];
    char status[160];
    uint64_t last_paint;
    struct window *win;
    char (*history)[HTTP_URL_MAX];
    int hist_pos, hist_count;
};

static const struct ui_font *font_of(int f)
{
    switch (f) {
    case HF_BOLD:
    case HF_H3:
        return &font_bold;
    case HF_SMALL:
        return &font_small;
    case HF_H1:
        return &font_large;
    case HF_H2:
        return &font_title;
    case HF_MONO:
        return &font_mono;
    default:
        return &font_sans;
    }
}

struct line {
    struct browser *b;
    int left, right, x, y, first, gap;
    bool empty;
};

static struct box *new_box(struct line *l, uint8_t kind, const struct html_run *r, int w, int h, int asc)
{
    struct browser *b = l->b;
    if ((size_t)b->box_count == b->box_cap) {
        size_t cap = b->box_cap ? b->box_cap * 2 : 1024;
        struct box *g = krealloc(b->boxes, cap * sizeof(*g));
        if (!g)
            return NULL;
        b->boxes = g;
        b->box_cap = cap;
    }
    if (l->empty) {
        if (b->box_count && l->gap)
            l->y += l->gap;
        l->gap = 0;
        l->x = MIN(l->left + r->indent * INDENT, l->right - 120) + (r->flags & HS_BUTTON ? 8 : 0);
        l->empty = false;
    }
    struct box *bx = &b->boxes[b->box_count++];
    *bx = (struct box){l->x, 0, w, r->off, 0, kind, r->font, r->flags, (uint8_t)h, (uint8_t)asc, r->link};
    l->x += w;
    return bx;
}

static void flush(struct line *l, int empty_h)
{
    struct browser *b = l->b;
    if (l->empty) {
        l->y += empty_h;
        return;
    }
    int asc = 0, desc = 0;
    for (int i = l->first; i < b->box_count; i++) {
        asc = MAX(asc, b->boxes[i].asc);
        desc = MAX(desc, b->boxes[i].h - b->boxes[i].asc);
    }
    for (int i = l->first; i < b->box_count; i++)
        b->boxes[i].y = l->y + asc - b->boxes[i].asc;
    l->y += asc + desc + 3;
    l->first = b->box_count;
    l->empty = true;
}

static void place_text(struct line *l, const struct html_run *r, uint32_t off, uint32_t len)
{
    struct browser *b = l->b;
    const struct ui_font *f = font_of(r->font);
    const char *s = b->doc.text;
    struct box *last = NULL;
    uint32_t end = off + len;
    while (off < end) {
        uint32_t e = off;
        while (e < end && s[e] != ' ')
            e++;
        uint32_t we = e;
        while (e < end && s[e] == ' ')
            e++;
        if (l->empty && we == off) {
            off = e;
            continue;
        }
        int w = gfx_text_width_n(f, s + off, we - off);
        if (!l->empty && l->x + w > l->right) {
            flush(l, 0);
            last = NULL;
            continue;
        }
        if (l->empty) {
            int room = l->right - MIN(l->left + r->indent * INDENT, l->right - 120);
            if (w > room) {
                uint32_t cut = off + 1;
                while (cut < we && gfx_text_width_n(f, s + off, cut + 1 - off) <= room)
                    cut++;
                we = e = cut;
                w = gfx_text_width_n(f, s + off, cut - off);
            }
        }
        int ws = e > we ? gfx_text_width_n(f, s + off, e - off) : w;
        if (last && last == &b->boxes[b->box_count - 1] && last->off + last->len == off &&
            last->len + (e - off) < 0xffff) {
            last->len = (uint16_t)(last->len + (e - off));
            last->w += ws;
            l->x += ws;
        } else {
            last = new_box(l, B_TEXT, r, ws, f->height, f->ascent);
            if (!last)
                return;
            last->off = off;
            last->len = (uint16_t)(e - off);
        }
        off = e;
    }
}

static void place_pre(struct line *l, const struct html_run *r)
{
    struct browser *b = l->b;
    const struct ui_font *f = font_of(r->font);
    const char *s = b->doc.text;
    uint32_t off = r->off, end = r->off + r->len;
    while (off < end) {
        uint32_t e = off;
        while (e < end && s[e] != '\n')
            e++;
        if (e > off) {
            uint32_t chunk = MIN(e - off, (uint32_t)0xfff0);
            struct box *bx = new_box(l, B_TEXT, r, gfx_text_width_n(f, s + off, chunk), f->height, f->ascent);
            if (!bx)
                return;
            bx->off = off;
            bx->len = (uint16_t)chunk;
        }
        if (e < end)
            flush(l, f->height + 3);
        off = e + 1;
    }
}

static void field_size(struct browser *b, int field, int *w, int *h)
{
    const struct html_field *f = &b->doc.fields[field];
    *h = 24;
    switch (f->type) {
    case HT_CHECK:
    case HT_RADIO:
        *w = *h = 16;
        break;
    case HT_SELECT:
        *w = MAX(120, gfx_text_width(&font_sans, html_str(&b->doc, f->value)) + 40);
        break;
    case HT_TEXTAREA:
        *w = 360;
        break;
    default:
        *w = 240;
    }
}

static void layout(struct browser *b, int width)
{
    b->box_count = 0;
    b->layout_w = width;
    int right = MIN(width - PAD, PAD + MAX_TEXT_W);
    struct line l = {b, PAD, MAX(right, PAD + 200), PAD, PAD, 0, 0, true};
    for (int i = 0; i < b->doc.run_count; i++) {
        const struct html_run *r = &b->doc.runs[i];
        switch (r->kind) {
        case HR_TEXT:
            if ((r->flags & HS_BUTTON) && !l.empty)
                l.x += 8;
            if (r->flags & HS_PRE)
                place_pre(&l, r);
            else
                place_text(&l, r, r->off, r->len);
            if (r->flags & HS_BUTTON)
                l.x += 8;
            break;
        case HR_BREAK:
            flush(&l, font_sans.height + 3);
            break;
        case HR_BLOCK:
            flush(&l, 0);
            l.gap = MAX(l.gap, (int)r->off);
            break;
        case HR_GAP:
            if (!l.empty)
                l.x += INDENT;
            break;
        case HR_RULE: {
            flush(&l, 0);
            struct box *bx = new_box(&l, B_RULE, r, l.right - l.left, 1, 1);
            if (bx)
                bx->x = l.left;
            flush(&l, 0);
            break;
        }
        case HR_FIELD: {
            int w, h, field = HTML_LINK_FIELD(r->link);
            field_size(b, field, &w, &h);
            if (!l.empty && l.x + w > l.right)
                flush(&l, 0);
            new_box(&l, B_FIELD, r, w, h, h - 6);
            l.x += 4;
            break;
        }
        }
    }
    flush(&l, 0);
    b->content_h = l.y + PAD;
}

static struct rect page_area(struct window *w)
{
    return (struct rect){0, BAR_H, w->surface.w, w->surface.h - BAR_H - STATUS_H};
}

static void set_status(struct browser *b, const char *s)
{
    str_copy(b->status, s, sizeof(b->status));
}

static void progress(void *ctx, const char *stage, size_t done, size_t total)
{
    struct browser *b = ctx;
    char size[16], all[16];
    fmt_size(size, sizeof(size), done);
    if (total) {
        fmt_size(all, sizeof(all), total);
        ksnprintf(b->status, sizeof(b->status), "Downloading %s of %s…", size, all);
    } else if (!strcmp(stage, "downloading")) {
        ksnprintf(b->status, sizeof(b->status), "Downloading %s…", size);
    } else {
        ksnprintf(b->status, sizeof(b->status), "%c%s…", to_upper(stage[0]), stage + 1);
    }
    uint64_t now = hal_ticks_ms();
    if (now - b->last_paint >= 120) {
        b->last_paint = now;
        wm_invalidate(b->win);
        wm_render();
    }
}

static void show_html(struct browser *b, const char *html, const char *url)
{
    html_free(&b->doc);
    html_parse(&b->doc, html, strlen(html), url);
}

static void error_page(struct browser *b, const char *url, const char *why)
{
    char page[2048];
    ksnprintf(page, sizeof(page),
              "<title>Can't open the page</title><h1>Can't open this page</h1><p>%s</p>"
              "<p><small>Address: %s</small></p><p><a href=\"about:home\">Go to the start page</a></p>",
              why, url);
    show_html(b, page, url);
}

static bool is_html_name(const char *path)
{
    return str_iends(path, ".html") || str_iends(path, ".htm") || str_iends(path, ".xhtml");
}

static void load_dir(struct browser *b, const char *path, const char *url)
{
    struct vdir d;
    if (vfs_opendir(path, &d)) {
        error_page(b, url, "The folder can't be read.");
        return;
    }
    size_t cap = 8192, len = 0;
    char *page = kmalloc(cap);
    if (!page) {
        vfs_closedir(&d);
        return;
    }
    len += (size_t)ksnprintf(page, cap, "<title>%s</title><h2>Index of %s</h2><ul>", path, path);
    if (strcmp(path, "/"))
        len += (size_t)ksnprintf(page + len, cap - len, "<li><a href=\"..\">Parent folder</a></li>");
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (ent.name[0] == '.')
            continue;
        if (len + 3 * NAME_MAX_LEN + 64 > cap) {
            char *g = krealloc(page, cap * 2);
            if (!g)
                break;
            page = g;
            cap *= 2;
        }
        bool dir = ent.type == VN_DIR;
        char size[16] = "";
        if (!dir)
            fmt_size(size, sizeof(size), ent.size);
        len += (size_t)ksnprintf(page + len, cap - len, "<li><a href=\"%s/%s%s\">%s%s</a> <small>%s</small></li>",
                                 strcmp(path, "/") ? path : "", ent.name, "", ent.name, dir ? "/" : "", size);
    }
    vfs_closedir(&d);
    str_append(page, "</ul>", cap);
    show_html(b, page, url);
    kfree(page);
}

static void load_file(struct browser *b, const char *url)
{
    char path[PATH_MAX_LEN];
    str_copy(path, url + 7, sizeof(path));
    if (!path[0])
        str_copy(path, "/", sizeof(path));
    if (vfs_is_dir(path)) {
        load_dir(b, path, url);
        return;
    }
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err) {
        error_page(b, url, err == E_NOENT ? "The file doesn't exist." : "The file can't be read.");
        return;
    }
    html_free(&b->doc);
    if (is_html_name(path))
        html_parse(&b->doc, data, size, url);
    else
        html_plain(&b->doc, data, size, url);
    b->source = data;
    b->source_len = size;
}

static bool looks_html(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && is_space(s[i]))
        i++;
    return i < n && s[i] == '<';
}

static bool is_text_type(const char *ct)
{
    return str_starts(ct, "text/") || strstr(ct, "json") || strstr(ct, "xml") || strstr(ct, "javascript");
}

static void download(struct browser *b, struct http_response *resp)
{
    struct http_url u;
    char name[NAME_MAX_LEN] = "download";
    if (http_parse_url(resp->url, &u)) {
        char *q = strchr(u.path, '?');
        if (q)
            *q = 0;
        const char *base = path_basename(u.path);
        if (base && *base && strcmp(base, "/"))
            str_copy(name, base, sizeof(name));
    }
    char dir[PATH_MAX_LEN], path[PATH_MAX_LEN];
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    vfs_mkdirs(dir);
    path_join(dir, name, path, sizeof(path));
    int err = vfs_write_all(path, resp->body, resp->length);
    str_copy(b->downloaded, err ? "" : path, sizeof(b->downloaded));
    char size[16], page[1024];
    fmt_size(size, sizeof(size), resp->length);
    if (err)
        ksnprintf(page, sizeof(page), "<title>Download failed</title><h1>Download failed</h1><p>%s could not be "
                  "saved: %s.</p>", name, err_str(err));
    else
        ksnprintf(page, sizeof(page), "<title>Downloaded %s</title><h1>Download complete</h1><p>Saved %s (%s) to "
                  "<a href=\"file://%s\">%s</a>.</p><p><a href=\"open:%s\">Open %s</a></p>", name, name, size, dir,
                  path, path, name);
    show_html(b, page, resp->url);
}

static void load_http(struct browser *b, const char *url)
{
    struct http_request req = {
        .method = b->post ? "POST" : NULL,
        .url = url,
        .headers = b->post ? "Content-Type: application/x-www-form-urlencoded\r\n" : NULL,
        .body = b->post,
        .body_len = b->post_len,
        .max_size = 512u << 20,
        .progress = progress,
        .ctx = b,
    };
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    if (err) {
        char why[160];
        if (err == E_NOENT)
            ksnprintf(why, sizeof(why), "The server's address could not be found.");
        else
            ksnprintf(why, sizeof(why), "The connection failed: %s.", tls_strerror(err));
        error_page(b, url, why);
        return;
    }
    str_copy(b->url, resp.url, sizeof(b->url));
    str_copy(b->tls, resp.tls, sizeof(b->tls));
    b->secure = resp.tls[0] != 0;
    char ct[128] = "";
    http_header(resp.headers, "Content-Type", ct, sizeof(ct));
    str_lower(ct);
    bool html = str_starts(ct, "text/html") || strstr(ct, "xhtml") ||
                (!ct[0] && looks_html(resp.body, resp.length));
    if (!html && !is_text_type(ct) && ct[0]) {
        download(b, &resp);
        http_free(&resp);
        return;
    }
    size_t len = resp.length;
    char *utf = html_to_utf8(resp.body, resp.length, ct, &len);
    const char *data = utf ? utf : resp.body;
    html_free(&b->doc);
    if (html)
        html_parse(&b->doc, data, len, resp.url);
    else
        html_plain(&b->doc, data, len, resp.url);
    if (utf) {
        kfree(utf);
        b->source = resp.body;
        b->source_len = resp.length;
        resp.body = NULL;
    } else {
        b->source = resp.body;
        b->source_len = resp.length;
        resp.body = NULL;
    }
    if (resp.status >= 400 && !b->doc.title[0])
        ksnprintf(b->doc.title, sizeof(b->doc.title), "Error %d", resp.status);
    http_free(&resp);
}

static void set_address(struct browser *b)
{
    ui_text_set(&b->addr, b->url);
}

static void load(struct window *w, struct browser *b)
{
    char url[HTTP_URL_MAX];
    str_copy(url, b->pending, sizeof(url));
    str_copy(b->url, url, sizeof(b->url));
    kfree(b->source);
    b->source = NULL;
    b->source_len = 0;
    b->tls[0] = 0;
    b->secure = false;
    if (!strcmp(url, HOME_URL))
        show_html(b, home_page, url);
    else if (str_starts(url, "file://"))
        load_file(b, url);
    else if (str_starts(url, "http://") || str_starts(url, "https://"))
        load_http(b, url);
    else
        error_page(b, url, "This kind of address is not supported.");
    kfree(b->post);
    b->post = NULL;
    b->post_len = 0;
    b->loading = false;
    b->scroll = 0;
    b->hover = -1;
    b->focus = -1;
    if (b->push) {
        if (b->hist_count && !strcmp(b->history[b->hist_pos], b->url)) {
            b->hist_count = b->hist_pos + 1;
        } else {
            if (b->hist_count && b->hist_pos + 1 >= HISTORY) {
                memmove(b->history[0], b->history[1], (HISTORY - 1) * HTTP_URL_MAX);
                b->hist_pos--;
            }
            b->hist_pos = b->hist_count ? b->hist_pos + 1 : 0;
            b->hist_count = b->hist_pos + 1;
            str_copy(b->history[b->hist_pos], b->url, HTTP_URL_MAX);
        }
    } else if (b->hist_count) {
        str_copy(b->history[b->hist_pos], b->url, HTTP_URL_MAX);
    }
    layout(b, w->surface.w - 12);
    set_address(b);
    set_status(b, b->tls[0] ? b->tls : "");
    wm_set_title(w, b->doc.title[0] ? b->doc.title : b->url);
    wm_invalidate(w);
}

static void navigate(struct window *w, struct browser *b, const char *url, bool push)
{
    if (str_starts(url, "open:")) {
        if (b->downloaded[0] && !strcmp(url + 5, b->downloaded))
            gui_open_file(b->downloaded);
        return;
    }
    str_copy(b->pending, url, sizeof(b->pending));
    b->loading = true;
    b->push = push;
    char s[160];
    ksnprintf(s, sizeof(s), "Opening %s…", url);
    set_status(b, s);
    ui_text_set(&b->addr, url);
    wm_set_timer(w, 20);
    wm_invalidate(w);
}

static void go_typed(struct window *w, struct browser *b, const char *typed)
{
    while (is_space(*typed))
        typed++;
    if (!*typed)
        return;
    char url[HTTP_URL_MAX];
    bool spaces = strchr(typed, ' ') != NULL;
    if (strstr(typed, "://") || str_starts(typed, "about:")) {
        str_copy(url, typed, sizeof(url));
    } else if (typed[0] == '/') {
        ksnprintf(url, sizeof(url), "file://%s", typed);
    } else if (!spaces && (strchr(typed, '.') || str_starts(typed, "localhost"))) {
        bool plain = is_digit(typed[0]) || str_starts(typed, "localhost");
        ksnprintf(url, sizeof(url), "%s://%s", plain ? "http" : "https", typed);
    } else {
        str_copy(url, SEARCH_URL, sizeof(url));
        size_t n = strlen(url);
        static const char hex[] = "0123456789ABCDEF";
        for (const char *s = typed; *s && n + 4 < sizeof(url); s++) {
            uint8_t c = (uint8_t)*s;
            if (is_alnum(c) || c == '-' || c == '.' || c == '_')
                url[n++] = (char)c;
            else if (c == ' ')
                url[n++] = '+';
            else {
                url[n++] = '%';
                url[n++] = hex[c >> 4];
                url[n++] = hex[c & 15];
            }
        }
        url[n] = 0;
    }
    navigate(w, b, url, true);
}

static void submit(struct window *w, struct browser *b, int field)
{
    char url[HTTP_URL_MAX];
    char *body;
    size_t len;
    if (html_submit(&b->doc, field, url, sizeof(url), &body, &len))
        return;
    kfree(b->post);
    b->post = body;
    b->post_len = len;
    navigate(w, b, url, true);
}

static bool form_ready(struct browser *b, int field)
{
    int form = b->doc.fields[field].form;
    if (form < 0)
        return false;
    for (int i = 0; i < b->doc.field_count; i++) {
        const struct html_field *f = &b->doc.fields[i];
        if (f->form == form && (f->type == HT_TEXT || f->type == HT_PASSWORD) && !html_str(&b->doc, f->value)[0])
            return false;
    }
    return true;
}

static void on_field(struct window *w, const char *value)
{
    struct browser *b = w->data;
    if (b->edit_field < 0 || b->edit_field >= b->doc.field_count)
        return;
    int field = b->edit_field;
    struct html_field *f = &b->doc.fields[field];
    f->value = html_intern(&b->doc, value);
    layout(b, b->layout_w);
    if (value[0] && f->type != HT_TEXTAREA && f->type != HT_SELECT && form_ready(b, field))
        submit(w, b, field);
    wm_invalidate(w);
}

static void activate(struct window *w, struct browser *b, int link)
{
    if (link >= 0) {
        const char *url = html_link(&b->doc, link);
        char here[HTTP_URL_MAX], there[HTTP_URL_MAX];
        str_copy(here, b->url, sizeof(here));
        str_copy(there, url, sizeof(there));
        char *h = strchr(here, '#'), *t = strchr(there, '#');
        if (h)
            *h = 0;
        if (t)
            *t = 0;
        if (t && !strcmp(here, there))
            return;
        navigate(w, b, url, true);
        return;
    }
    int field = HTML_LINK_FIELD(link);
    if (field < 0 || field >= b->doc.field_count)
        return;
    struct html_field *f = &b->doc.fields[field];
    b->edit_field = field;
    switch (f->type) {
    case HT_SUBMIT:
        submit(w, b, field);
        break;
    case HT_CHECK:
        f->checked = !f->checked;
        break;
    case HT_RADIO:
        for (int i = 0; i < b->doc.field_count; i++)
            if (b->doc.fields[i].type == HT_RADIO && b->doc.fields[i].form == f->form &&
                !strcmp(html_str(&b->doc, b->doc.fields[i].name), html_str(&b->doc, f->name)))
                b->doc.fields[i].checked = false;
        f->checked = true;
        break;
    case HT_PASSWORD:
        ui_prompt_secret(w, "Password", "Enter the password for this page:", on_field);
        break;
    default: {
        const char *hint = html_str(&b->doc, f->hint);
        ui_prompt(w, hint[0] ? hint : "Enter text", hint[0] ? hint : "Type the text for this field:",
                  html_str(&b->doc, f->value), on_field);
    }
    }
}

static void go_history(struct window *w, struct browser *b, int delta)
{
    int pos = b->hist_pos + delta;
    if (pos < 0 || pos >= b->hist_count || b->loading)
        return;
    b->hist_pos = pos;
    navigate(w, b, b->history[pos], false);
}

static void on_save(struct window *w, const char *value)
{
    struct browser *b = w->data;
    if (!*value || !b->source)
        return;
    char path[PATH_MAX_LEN];
    if (path_resolve(user_home(), value, path, sizeof(path)))
        str_copy(path, value, sizeof(path));
    int err = vfs_write_all(path, b->source, b->source_len);
    if (err) {
        char msg[128];
        ksnprintf(msg, sizeof(msg), "The page could not be saved: %s.", err_str(err));
        gui_message("Save failed", msg);
    }
}

static void save_page(struct window *w, struct browser *b)
{
    if (!b->source) {
        gui_message("Nothing to save", "This page has no source to save.");
        return;
    }
    char path[PATH_MAX_LEN];
    path_join(user_home(), "Downloads/page.html", path, sizeof(path));
    ui_prompt(w, "Save Page As", "Choose a file name and folder:", path, on_save);
}

static int box_at(struct browser *b, struct rect area, int x, int y)
{
    int py = y - area.y + b->scroll;
    for (int i = 0; i < b->box_count; i++) {
        const struct box *bx = &b->boxes[i];
        if (bx->y > py)
            break;
        if (bx->link != -1 && py >= bx->y - 2 && py < bx->y + bx->h + 2 && x >= bx->x - 2 && x < bx->x + bx->w + 2)
            return i;
    }
    return -1;
}

static int first_visible(struct browser *b, int top)
{
    int lo = 0, hi = b->box_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (b->boxes[mid].y + 40 < top)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int link_of(struct browser *b, int box)
{
    return box >= 0 && box < b->box_count ? b->boxes[box].link : -1;
}

static void move_focus(struct browser *b, bool back, int view)
{
    int n = b->box_count, cur = b->focus, link = link_of(b, cur);
    for (int step = 1; step <= n; step++) {
        int i = ((back ? cur - step : cur + step) % n + n) % n;
        if (cur < 0)
            i = back ? n - step : step - 1;
        int l = b->boxes[i].link;
        if (l == -1 || l == link)
            continue;
        if (back)
            while (i > 0 && b->boxes[i - 1].link == l)
                i--;
        b->focus = i;
        const struct box *bx = &b->boxes[i];
        if (bx->y < b->scroll || bx->y + bx->h > b->scroll + view)
            b->scroll = CLAMP(bx->y - view / 3, 0, MAX(0, b->content_h - view));
        return;
    }
}

static void key(struct window *w, struct browser *b, int k, int view)
{
    int base = KEY_BASE(k);
    int step = 0;
    if (base == KEY_UP)
        step = -40;
    else if (base == KEY_DOWN)
        step = 40;
    else if (base == KEY_PGUP)
        step = -(view - 40);
    else if (base == KEY_PGDN || k == ' ')
        step = view - 40;
    else if (base == KEY_HOME)
        step = -b->content_h;
    else if (base == KEY_END)
        step = b->content_h;
    if (step) {
        b->scroll = CLAMP(b->scroll + step, 0, MAX(0, b->content_h - view));
        return;
    }
    if (base == KEY_TAB) {
        move_focus(b, k & KEY_MOD_SHIFT, view);
        return;
    }
    if ((base == KEY_ENTER || k == '\r') && link_of(b, b->focus) != -1) {
        activate(w, b, link_of(b, b->focus));
        return;
    }
    if (k == KEY_BACKSPACE || k == KEY_COMMAND(CMD_BACK) || k == (KEY_LEFT | KEY_MOD_ALT))
        go_history(w, b, -1);
    else if (k == KEY_COMMAND(CMD_FORWARD) || k == (KEY_RIGHT | KEY_MOD_ALT))
        go_history(w, b, 1);
    else if (k == KEY_CTRL('r') || base == KEY_F5 || k == KEY_COMMAND(CMD_RELOAD))
        navigate(w, b, b->url, false);
    else if (k == KEY_COMMAND(CMD_HOME))
        navigate(w, b, HOME_URL, true);
    else if (k == KEY_CTRL('s') || k == KEY_COMMAND(CMD_SAVE))
        save_page(w, b);
    else if (k == KEY_COMMAND(CMD_COPY_URL))
        gui_set_clipboard(b->url, strlen(b->url));
    else if (k == KEY_COMMAND(CMD_COOKIES))
        http_cookies_clear();
}

static void draw_field(struct canvas *c, struct browser *b, const struct box *bx, int x, int y)
{
    const struct html_field *f = &b->doc.fields[HTML_LINK_FIELD(bx->link)];
    struct rect r = {x, y, bx->w, bx->h};
    if (f->type == HT_CHECK || f->type == HT_RADIO) {
        int radius = f->type == HT_RADIO ? 8 : 4;
        gfx_round(c, r, radius, f->checked ? style.accent : style.field);
        gfx_round_frame(c, r, radius, f->checked ? style.accent : style.field_border);
        if (f->checked)
            gfx_symbol(c, x + 1, y + 1, SYM_CHECK, 14, style.accent_text);
        return;
    }
    gfx_round(c, r, 5, style.field);
    gfx_round_frame(c, r, 5, style.field_border);
    struct canvas inner = *c;
    inner.clip = rect_intersect(c->clip, (struct rect){x + 4, y, bx->w - 8, bx->h});
    const char *v = html_str(&b->doc, f->value);
    int ty = y + (bx->h - font_sans.height) / 2;
    if (f->type == HT_PASSWORD) {
        char dots[64];
        size_t n = MIN(strlen(v), (size_t)20), o = 0;
        for (size_t i = 0; i < n; i++)
            o += utf8_encode(0x2022, dots + o);
        dots[o] = 0;
        gfx_text(&inner, x + 8, ty, &font_sans, dots, style.text);
    } else if (v[0]) {
        gfx_text(&inner, x + 8, ty, &font_sans, v, style.text);
    } else {
        gfx_text(&inner, x + 8, ty, &font_sans, html_str(&b->doc, f->hint), style.tertiary);
    }
    if (f->type == HT_SELECT)
        gfx_symbol(c, x + bx->w - 20, y + 5, SYM_DOWN, 14, style.secondary);
}

static void draw_page(struct canvas *c, struct browser *b, struct rect area)
{
    gfx_clear(c, area, style.content);
    struct canvas inner = *c;
    inner.clip = rect_intersect(c->clip, area);
    int hover_link = link_of(b, b->hover), focus_link = link_of(b, b->focus);
    for (int i = first_visible(b, b->scroll); i < b->box_count; i++) {
        const struct box *bx = &b->boxes[i];
        int x = area.x + bx->x, y = area.y + bx->y - b->scroll;
        if (y > area.y + area.h)
            break;
        if (bx->kind == B_RULE) {
            gfx_fill(&inner, (struct rect){x, y, bx->w, 1}, style.separator);
            continue;
        }
        if (bx->link != -1 && bx->link == focus_link) {
            int pad = bx->flags & HS_BUTTON ? 10 : 3;
            gfx_round_frame(&inner, (struct rect){x - pad, y - pad + 1, bx->w + 2 * pad, bx->h + 2 * pad - 2}, 5,
                            style.accent);
        }
        if (bx->kind == B_FIELD) {
            draw_field(&inner, b, bx, x, y);
            continue;
        }
        const struct ui_font *f = font_of(bx->font);
        uint32_t color = style.text;
        if (bx->flags & HS_BUTTON) {
            struct rect r = {x - 8, y - 3, bx->w + 16, bx->h + 6};
            gfx_round(&inner, r, 6, bx->link == hover_link ? style.button_hover : style.button);
            gfx_round_frame(&inner, r, 6, style.control_border);
        } else if (bx->link >= 0) {
            color = style.accent;
        } else if (bx->flags & HS_MUTED) {
            color = style.secondary;
        }
        int len = bx->len;
        while (len && b->doc.text[bx->off + len - 1] == ' ')
            len--;
        gfx_text_n(&inner, x, y, f, b->doc.text + bx->off, (size_t)len, color);
        if (bx->link >= 0 && bx->link == hover_link && !(bx->flags & HS_BUTTON))
            gfx_fill(&inner, (struct rect){x, y + f->ascent + 2, gfx_text_width_n(f, b->doc.text + bx->off,
                                                                                (size_t)len), 1}, color);
    }
}

static void frame(struct ui *ui, void *data)
{
    struct browser *b = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    struct rect area = page_area(w);
    struct gui_event *e = ui->e;
    if (e && e->type == EV_TIMER && b->loading) {
        wm_set_timer(w, 0);
        load(w, b);
        return;
    }
    if (e && e->type == EV_RESIZE && W - 12 != b->layout_w && !b->loading) {
        layout(b, W - 12);
        ui->changed = true;
    }
    if (e && e->type == EV_MOUSE_MOVE && rect_contains(area, e->x, e->y)) {
        int hit = box_at(b, area, e->x, e->y);
        int old = b->hover >= 0 && b->hover < b->box_count ? b->boxes[b->hover].link : -1;
        int now = hit >= 0 ? b->boxes[hit].link : -1;
        b->hover = hit;
        if (old != now)
            ui->changed = true;
    }
    if (e && e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT && rect_contains(area, e->x, e->y)) {
        ui->st->focus = -1;
        ui->consumed = true;
        int hit = box_at(b, area, e->x, e->y);
        if (hit >= 0 && !b->loading)
            activate(w, b, b->boxes[hit].link);
        ui->changed = true;
    }
    if (e && e->type == EV_WHEEL && rect_contains(area, e->x, e->y)) {
        b->scroll = CLAMP(b->scroll + e->wheel * 60, 0, MAX(0, b->content_h - area.h));
        ui->changed = true;
    }
    int k = ui_key(ui);
    if (k == KEY_CTRL('l') || k == KEY_COMMAND(CMD_OPEN)) {
        ui->st->focus = b->addr_id;
        b->addr.cursor = b->addr.len;
        ui->consumed = true;
    } else if (k != KEY_NONE && ui->st->focus != b->addr_id) {
        key(w, b, k, area.h);
        ui->consumed = true;
        ui->changed = true;
    }

    if (ui->c) {
        gfx_clear(ui->c, (struct rect){0, 0, W, BAR_H}, style.window);
        gfx_fill(ui->c, (struct rect){0, BAR_H - 1, W, 1}, style.separator);
    }
    if (ui_symbol_button(ui, (struct rect){10, 8, 28, 28}, SYM_LEFT, false))
        go_history(w, b, -1);
    if (ui_symbol_button(ui, (struct rect){40, 8, 28, 28}, SYM_RIGHT, false))
        go_history(w, b, 1);
    if (ui_symbol_button(ui, (struct rect){70, 8, 28, 28}, SYM_RESTART, false) && !b->loading)
        navigate(w, b, b->url, false);
    if (ui_symbol_button(ui, (struct rect){100, 8, 28, 28}, SYM_HOME, false) && !b->loading)
        navigate(w, b, HOME_URL, true);
    int ax = 138;
    if (b->secure && ui->c)
        gfx_symbol(ui->c, ax, 15, SYM_LOCK, 14, style.secondary);
    if (b->secure)
        ax += 20;
    b->addr_id = ui->id + 1;
    if (ui_textbox_hint(ui, (struct rect){ax, 8, W - ax - 12, 28}, &b->addr, "Search or enter an address")) {
        ui->st->focus = -1;
        if (strcmp(b->addr.buf, b->url) || strlen(b->url) < sizeof(b->addr.buf) - 1)
            go_typed(w, b, b->addr.buf);
        else
            navigate(w, b, b->url, false);
    }
    ui_scrollbar(ui, (struct rect){W - 10, area.y, 10, area.h}, &b->scroll, b->content_h, area.h);

    if (!ui->c)
        return;
    struct canvas *c = ui->c;
    draw_page(c, b, area);
    gfx_clear(c, (struct rect){0, H - STATUS_H, W, STATUS_H}, style.window);
    gfx_fill(c, (struct rect){0, H - STATUS_H, W, 1}, style.separator);
    const char *status = b->status;
    int hover_link = b->hover >= 0 && b->hover < b->box_count ? b->boxes[b->hover].link : -1;
    if (!b->loading && hover_link >= 0)
        status = html_link(&b->doc, hover_link);
    gfx_text_box(c, (struct rect){12, H - STATUS_H, W - 24, STATUS_H}, &font_small, status, style.secondary,
                 ALIGN_LEFT);
}

static void paint(struct window *w, struct canvas *c)
{
    struct browser *b = w->data;
    ui_run(w, &b->st, c, NULL, frame, b);
}

static void event(struct window *w, struct gui_event *e)
{
    struct browser *b = w->data;
    ui_run(w, &b->st, NULL, e, frame, b);
}

static bool on_close(struct window *w)
{
    UNUSED(w);
    return true;
}

static void destroy(struct window *w)
{
    struct browser *b = w->data;
    html_free(&b->doc);
    kfree(b->boxes);
    kfree(b->source);
    kfree(b->post);
    kfree(b->history);
    kfree(b);
}

static const struct window_ops ops = {paint, event, on_close, destroy};

static void launch(const char *arg)
{
    struct browser *b = kzalloc(sizeof(*b));
    if (!b)
        return;
    b->history = kzalloc(HISTORY * HTTP_URL_MAX);
    if (!b->history) {
        kfree(b);
        return;
    }
    b->st.focus = -1;
    b->hover = -1;
    b->focus = -1;
    b->edit_field = -1;
    struct window *w = wm_open("Browser", ICON_BROWSER, 900, 620, &ops, b, WIN_RESIZABLE);
    if (!w) {
        kfree(b->history);
        kfree(b);
        return;
    }
    b->win = w;
    w->min_w = 420;
    w->min_h = 260;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    layout(b, w->surface.w - 12);
    if (arg && *arg) {
        if (arg[0] == '/' && !strstr(arg, "://")) {
            char url[HTTP_URL_MAX];
            ksnprintf(url, sizeof(url), "file://%s", arg);
            navigate(w, b, url, true);
        } else {
            go_typed(w, b, arg);
        }
    } else {
        navigate(w, b, HOME_URL, true);
    }
}

static const struct gui_app browser_app = {"Browser", "Browse the web", ICON_BROWSER, launch, ".html;.htm;.xhtml", NULL};
GUI_APP(browser_app);
