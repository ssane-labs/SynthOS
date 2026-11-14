/* The screen as text, for the assistant (and anyone at a terminal): what the windows show — buttons, fields,
   switches, lists, menus and text, each with a number — and hands that press, type and choose by number. Native
   windows describe themselves while they are painted for it (gui_ax in ui.c and gfx.c); Windows programs keep a
   list of their controls (w32_describe). Then `web`: a web page as text with numbered links and fields. */

#include <synth/html.h>

#define AX_NODES 320

/* errors go with the output, so the assistant reads them too */
static int ax_error(struct shell *sh, const char *who, const char *fmt, ...)
{
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sh_printf(sh, "%s: %s\n", who, msg);
    return 1;
}

static bool ax_starts(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if ((*s | 0x20) != (*prefix | 0x20) || !*s)
            return false;
    return true;
}

/* a growing piece of text */
struct wbuf {
    char *p;
    size_t len, cap;
};

static void sb_put(struct wbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->len + n + 256);
        char *p = kmalloc(cap);
        if (!p)
            return;
        if (b->p)
            memcpy(p, b->p, b->len);
        kfree(b->p);
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_puts(struct wbuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

struct ax_node {
    uint8_t kind, flags;
    bool gone;
    struct rect r; /* in the window's client area */
    struct window *win;
    int menu, item; /* a menu item: its menu and place, else -1 */
    char name[80];
    char value[96];
};

static struct ax_node *ax_nodes;
static int ax_count;

struct ax_collect {
    struct ax_sink sink;
    struct window *win;
};

static void ax_copy_text(char *out, size_t cap, const char *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && s[i] && k + 1 < cap; i++)
        out[k++] = s[i] == '\n' || s[i] == '\t' || s[i] == '\r' ? ' ' : s[i];
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static void ax_note(struct ax_sink *s, int kind, struct rect r, const char *name, size_t n, const char *value,
                    unsigned flags)
{
    struct ax_collect *c = (struct ax_collect *)s;
    if (ax_count >= AX_NODES || r.w <= 0 || r.h <= 0)
        return;
    struct ax_node *a = &ax_nodes[ax_count];
    memset(a, 0, sizeof(*a));
    a->kind = (uint8_t)kind;
    a->flags = (uint8_t)flags;
    a->r = r;
    a->win = c->win;
    a->menu = a->item = -1;
    ax_copy_text(a->name, sizeof(a->name), name, n);
    if (value)
        ax_copy_text(a->value, sizeof(a->value), value, strlen(value));
    if (kind == AX_TEXT && !a->name[0])
        return;
    ax_count++;
}

static bool ax_inside(struct rect outer, struct rect in)
{
    int cx = in.x + in.w / 2, cy = in.y + in.h / 2;
    return rect_contains(outer, cx, cy);
}

/* text drawn inside a control is its name; a switch takes the words on its left */
static void ax_tidy(int from)
{
    for (int i = from; i < ax_count; i++) {
        struct ax_node *t = &ax_nodes[i];
        if (t->kind != AX_TEXT)
            continue;
        for (int k = from; k < ax_count; k++) {
            struct ax_node *c = &ax_nodes[k];
            if (c->kind == AX_TEXT || c->kind == AX_LIST || c->gone || c->win != t->win || !ax_inside(c->r, t->r))
                continue;
            if (!c->name[0])
                str_copy(c->name, t->name, sizeof(c->name));
            else if (c->kind == AX_ITEM && !strstr(c->name, t->name) && strlen(c->name) + strlen(t->name) + 4 < sizeof(c->name)) {
                str_append(c->name, " | ", sizeof(c->name));
                str_append(c->name, t->name, sizeof(c->name));
            }
            t->gone = true;
            break;
        }
    }
    for (int k = from; k < ax_count; k++) {
        struct ax_node *c = &ax_nodes[k];
        if (c->kind != AX_SWITCH && c->kind != AX_SLIDER && c->kind != AX_FIELD)
            continue;
        if (c->name[0] && c->kind != AX_FIELD)
            continue;
        int best = -1, dist = 1 << 30;
        for (int i = from; i < ax_count; i++) {
            struct ax_node *t = &ax_nodes[i];
            int cy = c->r.y + c->r.h / 2, ty = t->r.y + t->r.h / 2;
            if (t->kind != AX_TEXT || t->win != c->win || t->r.x + t->r.w > c->r.x + 4 || ABS(cy - ty) > c->r.h)
                continue;
            int d = c->r.x - (t->r.x + t->r.w);
            if (d < dist) {
                dist = d;
                best = i;
            }
        }
        if (best >= 0 && dist < 600) {
            if (c->name[0]) { /* a field's own hint stays; the label in front of it says what it is */
                char hint[80];
                str_copy(hint, c->name, sizeof(hint));
                ksnprintf(c->name, sizeof(c->name), "%s (%s)", ax_nodes[best].name, hint);
            } else {
                str_copy(c->name, ax_nodes[best].name, sizeof(c->name));
            }
            ax_nodes[best].gone = true;
        }
    }
}

static void ax_menus(struct window *w)
{
    for (int m = 0; w->menus && m < w->menu_count; m++)
        for (int i = 0; i < w->menus[m].count && ax_count < AX_NODES; i++) {
            const struct menu_item *it = &w->menus[m].items[i];
            if (!it->label || !*it->label || it->key <= 0)
                continue;
            struct ax_node *a = &ax_nodes[ax_count++];
            memset(a, 0, sizeof(*a));
            a->kind = AX_ITEM;
            a->win = w;
            a->menu = m;
            a->item = i;
            str_copy(a->name, it->label, sizeof(a->name));
        }
}

/* describes one window into the list */
static void ax_window(struct window *w)
{
    int from = ax_count;
    struct ax_collect c = {{ax_note, w->surface.px}, w};
    gui_ax = &c.sink;
    w->dirty = true;
    wm_render();
    gui_ax = NULL;
    w32_describe(w, &c.sink);
    ax_tidy(from);
    ax_menus(w);
}

static const char *const ax_kind_names[AX_KINDS] = {"text", "button", "checkbox", "switch", "option",
                                                    "slider", "field", "tab", "item", "list"};

static void ax_print(struct shell *sh, int i)
{
    const struct ax_node *a = &ax_nodes[i];
    if (a->menu >= 0) {
        sh_printf(sh, "  [%d] menu %s > %s\n", i + 1, a->win->menus[a->menu].title, a->name);
        return;
    }
    sh_printf(sh, "  [%d] %s", i + 1, ax_kind_names[a->kind]);
    if (a->name[0])
        sh_printf(sh, " \"%s\"", a->name);
    if (a->kind == AX_FIELD || a->kind == AX_SLIDER)
        sh_printf(sh, " = \"%s\"", a->value);
    if (a->kind == AX_CHECK || a->kind == AX_SWITCH || a->kind == AX_RADIO)
        sh_puts(sh, a->flags & AX_ON ? " on" : " off");
    else if ((a->kind == AX_TAB || a->kind == AX_ITEM) && (a->flags & AX_ON))
        sh_puts(sh, " (selected)");
    else if (a->kind == AX_BUTTON && (a->flags & AX_ON))
        sh_puts(sh, " (on)");
    if (a->flags & AX_FOCUS)
        sh_puts(sh, " (typing here)");
    if (a->flags & AX_OFF)
        sh_puts(sh, " (disabled)");
    sh_puts(sh, "\n");
}

/* looks at the screen: the front window in full (or window `which`, or every window when all) */
static int ax_look(struct shell *sh, int which, bool all)
{
    struct window *list[64];
    int n = wm_window_list(list, 64);
    if (!n) {
        sh_puts(sh, "no windows are open\n");
        return 0;
    }
    if (!ax_nodes && !(ax_nodes = kmalloc(sizeof(*ax_nodes) * AX_NODES)))
        return ax_error(sh, "screen", "out of memory");
    ax_count = 0;
    struct window *front = wm_focused();
    sh_puts(sh, "Windows (front first):\n");
    for (int i = n - 1; i >= 0; i--) {
        struct window *w = list[i];
        bool full = all || (which ? which == i + 1 : (front ? w == front : i == n - 1));
        sh_printf(sh, "window %d \"%s\"%s%s\n", i + 1, w->title, w == front ? " (front)" : "",
                  w->flags & WIN_MINIMIZED ? " (minimized)" : "");
        if (!full || (w->flags & WIN_MINIMIZED))
            continue;
        int from = ax_count;
        ax_window(w);
        for (int k = from; k < ax_count; k++)
            if (!ax_nodes[k].gone)
                ax_print(sh, k);
    }
    return 0;
}

static struct ax_node *ax_find(struct shell *sh, const char *arg)
{
    long k;
    if (!str_parse_long(arg, &k) || k < 1 || k > ax_count) {
        ax_error(sh, "screen", "no element %s: look first (screen)", arg);
        return NULL;
    }
    struct ax_node *a = &ax_nodes[k - 1];
    if (!wm_window_alive(a->win)) {
        ax_error(sh, "screen", "that window is gone: look again");
        return NULL;
    }
    return a;
}

static void ax_pointer(int x, int y, unsigned buttons)
{
    struct pointer_event ev = {.placed = true, .x = x, .y = y, .buttons = buttons};
    wm_pointer(&ev);
}

static void ax_press(struct ax_node *a)
{
    wm_focus(a->win);
    if (a->menu >= 0) {
        wm_key(a->win->menus[a->menu].items[a->item].key);
        return;
    }
    struct rect cr = wm_client_rect(a->win);
    int x = cr.x + a->r.x + MIN(a->r.w / 2, 40), y = cr.y + a->r.y + a->r.h / 2;
    if (a->kind == AX_CHECK || a->kind == AX_RADIO)
        x = cr.x + a->r.x + 8; /* the box itself */
    ax_pointer(x, y, 0);
    ax_pointer(x, y, BUTTON_LEFT);
    ax_pointer(x, y, 0);
    wm_render();
}

static void ax_type(const char *s)
{
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n')
            wm_key(KEY_ENTER);
        else if (cp < 0x80)
            wm_key((int)cp);
        else
            wm_key(KEY_TEXT_CHAR(cp, 0));
    }
    wm_render();
}

static const struct {
    const char *name;
    int key;
} ax_keys[] = {{"enter", KEY_ENTER}, {"return", KEY_ENTER}, {"tab", KEY_TAB},      {"esc", KEY_ESC},
               {"escape", KEY_ESC},  {"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE}, {"up", KEY_UP},
               {"down", KEY_DOWN},   {"left", KEY_LEFT},   {"right", KEY_RIGHT},  {"home", KEY_HOME},
               {"end", KEY_END},     {"pgup", KEY_PGUP},   {"pgdn", KEY_PGDN},    {"space", ' '},
               {"f1", KEY_F1},       {"f2", KEY_F2},       {"f3", KEY_F3},        {"f5", KEY_F5}};

/* "enter", "ctrl+s", "shift+tab", "a" */
static int ax_key_code(const char *s)
{
    int mods = 0;
    for (;;) {
        if (ax_starts(s, "ctrl+"))
            mods |= KEY_MOD_CTRL, s += 5;
        else if (ax_starts(s, "shift+"))
            mods |= KEY_MOD_SHIFT, s += 6;
        else if (ax_starts(s, "alt+"))
            mods |= KEY_MOD_ALT, s += 4;
        else
            break;
    }
    for (size_t i = 0; i < ARRAY_SIZE(ax_keys); i++)
        if (!str_casecmp(s, ax_keys[i].name))
            return ax_keys[i].key | mods;
    if (s[0] && !s[1]) {
        if (mods & KEY_MOD_CTRL)
            return KEY_CTRL(s[0] | 0x20) | (mods & ~KEY_MOD_CTRL);
        return s[0] | mods;
    }
    return 0;
}

COMMAND(screen, NULL, "system",
        "screen [all|<window>|click <n>|type <n> <text>|set <n> <text>|key <name>|menu <menu> <item>]",
        "the screen as text, each control numbered; click, type, press keys and choose menu items by number")
{
    if (!wm_screen_count())
        return ax_error(sh, "screen", "the desktop is not running");
    const char *op = argc > 1 ? argv[1] : "";
    long k;
    if (argc == 1 || !strcmp(op, "all") || (argc == 2 && str_parse_long(op, &k)))
        return ax_look(sh, argc == 2 && str_parse_long(op, &k) ? (int)k : 0, !strcmp(op, "all"));
    if (!strcmp(op, "click") && argc == 3) {
        struct ax_node *a = ax_find(sh, argv[2]);
        if (!a)
            return 1;
        ax_press(a);
        sh_printf(sh, "clicked %s \"%s\"\n", a->menu >= 0 ? "menu item" : ax_kind_names[a->kind], a->name);
        return 0;
    }
    if ((!strcmp(op, "type") || !strcmp(op, "set")) && argc >= 4) {
        struct ax_node *a = ax_find(sh, argv[2]);
        if (!a)
            return 1;
        ax_press(a);
        if (!strcmp(op, "set")) { /* everything that was there goes first */
            wm_key(KEY_END);
            for (const char *v = a->value; *v; utf8_next(&v))
                wm_key(KEY_BACKSPACE);
        }
        char text[512] = "";
        for (int i = 3; i < argc; i++) {
            if (i > 3)
                str_append(text, " ", sizeof(text));
            str_append(text, argv[i], sizeof(text));
        }
        ax_type(text);
        sh_printf(sh, "typed into \"%s\"\n", a->name);
        return 0;
    }
    if (!strcmp(op, "key") && argc == 3) {
        int key = ax_key_code(argv[2]);
        if (!key)
            return ax_error(sh, "screen", "no key called %s", argv[2]);
        wm_key(key);
        wm_render();
        sh_printf(sh, "pressed %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(op, "menu") && argc == 4) {
        struct window *w = wm_focused();
        for (int m = 0; w && w->menus && m < w->menu_count; m++) {
            if (str_casecmp(w->menus[m].title, argv[2]))
                continue;
            for (int i = 0; i < w->menus[m].count; i++) {
                const struct menu_item *it = &w->menus[m].items[i];
                if (it->label && it->key > 0 && ax_starts(it->label, argv[3])) {
                    wm_key(it->key);
                    wm_render();
                    sh_printf(sh, "chose %s > %s\n", w->menus[m].title, it->label);
                    return 0;
                }
            }
        }
        return ax_error(sh, "screen", "the front window has no menu item %s > %s", argv[2], argv[3]);
    }
    return command_usage(sh, command_find("screen"));
}

/* ---- web: a page as text for the assistant, with numbered links [n] and fields {n} ---- */

static struct {
    struct html_doc doc;
    bool open;
    char url[HTTP_URL_MAX];
} page;

static int web_load(struct shell *sh, const char *where, const char *body, size_t body_len)
{
    char url[HTTP_URL_MAX], path[PATH_MAX_LEN];
    struct html_doc d;
    int err;
    if (!strstr(where, "://") && !sh_resolve(sh, where, path, sizeof(path)) && vfs_exists(path))
        ksnprintf(url, sizeof(url), "file://%s", path);
    else if (strstr(where, "://"))
        str_copy(url, where, sizeof(url));
    else
        ksnprintf(url, sizeof(url), "https://%s", where);
    if (str_starts(url, "file://")) {
        char *data, *q = strchr(url, '?');
        if (q) /* a form sent to a file: the file */
            *q = 0;
        size_t size;
        if ((err = vfs_read_all(url + 7, &data, &size)))
            return ax_error(sh, "web", "%s: %s", url + 7, err_str(err));
        bool html = str_iends(url, ".html") || str_iends(url, ".htm");
        err = html ? html_parse(&d, data, size, url) : html_plain(&d, data, size, url);
        kfree(data);
    } else {
        struct http_request req = {.url = url, .method = body ? "POST" : NULL, .body = body, .body_len = body_len,
                                   .headers = body ? "Content-Type: application/x-www-form-urlencoded\r\n" : NULL};
        struct http_response resp;
        if ((err = http_fetch(&req, &resp)))
            return ax_error(sh, "web", "%s: %s", url, err_str(err));
        char ct[128] = "";
        http_header(resp.headers, "Content-Type", ct, sizeof(ct));
        str_lower(ct);
        size_t len = resp.length;
        char *utf = html_to_utf8(resp.body, resp.length, ct, &len);
        const char *data = utf ? utf : resp.body;
        err = !ct[0] || strstr(ct, "html") ? html_parse(&d, data, len, resp.url) : html_plain(&d, data, len, resp.url);
        str_copy(url, resp.url, sizeof(url));
        kfree(utf);
        http_free(&resp);
    }
    if (err) {
        html_free(&d);
        return ax_error(sh, "web", "%s", err_str(err));
    }
    if (page.open)
        html_free(&page.doc);
    page.doc = d;
    page.open = true;
    str_copy(page.url, url, sizeof(page.url));
    return 0;
}

static void web_field(struct wbuf *b, const struct html_doc *d, int i)
{
    const struct html_field *f = &d->fields[i];
    const char *v = html_str(d, f->value), *hint = html_str(d, f->hint);
    char box[200];
    switch (f->type) {
    case HT_CHECK:
    case HT_RADIO:
        ksnprintf(box, sizeof(box), "{%d %s %s}", i + 1, f->type == HT_CHECK ? "checkbox" : "option",
                  f->checked ? "on" : "off");
        break;
    case HT_SUBMIT:
        ksnprintf(box, sizeof(box), "{%d button \"%s\"}", i + 1, *v ? v : "Submit");
        break;
    case HT_PASSWORD:
        ksnprintf(box, sizeof(box), "{%d password field%s}", i + 1, *v ? " (filled)" : "");
        break;
    default:
        ksnprintf(box, sizeof(box), "{%d field%s%s%s = \"%.80s\"}", i + 1, *hint ? " \"" : "", hint, *hint ? "\"" : "", v);
    }
    sb_puts(b, box);
}

static void web_show(struct shell *sh)
{
    const struct html_doc *d = &page.doc;
    struct wbuf b = {0};
    sb_puts(&b, d->title[0] ? d->title : page.url);
    sb_puts(&b, "\n");
    sb_puts(&b, page.url);
    sb_puts(&b, "\n\n");
    for (int i = 0; i < d->run_count && b.len < 24000; i++) {
        const struct html_run *r = &d->runs[i];
        switch (r->kind) {
        case HR_TEXT:
            if (r->link <= -2) { /* a button of a form */
                if (i == 0 || d->runs[i - 1].link != r->link)
                    web_field(&b, d, HTML_LINK_FIELD(r->link));
                break;
            }
            sb_put(&b, d->text + r->off, r->len);
            if (r->link >= 0 && (i + 1 >= d->run_count || d->runs[i + 1].link != r->link)) {
                char ref[16];
                ksnprintf(ref, sizeof(ref), "[%d]", r->link + 1);
                sb_puts(&b, ref);
            }
            break;
        case HR_BREAK:
        case HR_BLOCK:
        case HR_RULE:
            if (b.len && b.p[b.len - 1] != '\n')
                sb_puts(&b, "\n");
            break;
        case HR_GAP:
            sb_puts(&b, "  ");
            break;
        case HR_FIELD:
            web_field(&b, d, HTML_LINK_FIELD(r->link));
            break;
        }
    }
    if (b.p) {
        sh_puts(sh, b.p);
        if (b.len && b.p[b.len - 1] != '\n')
            sh_puts(sh, "\n");
    }
    kfree(b.p);
}

COMMAND(web, NULL, "system", "web [<address>|link <n>|type <n> <text>|press <n>|show]",
        "a web page as text for the assistant: links are [n], fields and buttons {n}")
{
    if (argc < 2)
        return command_usage(sh, command_find("web"));
    const char *op = argv[1];
    long k;
    if (!strcmp(op, "show") || (argc >= 3 && (!strcmp(op, "link") || !strcmp(op, "type") || !strcmp(op, "press")))) {
        if (!page.open)
            return ax_error(sh, "web", "no page is open: web <address>");
        if (!strcmp(op, "show")) {
            web_show(sh);
            return 0;
        }
        if (!str_parse_long(argv[2], &k) || k < 1)
            return ax_error(sh, "web", "%s is not a number", argv[2]);
        struct html_doc *d = &page.doc;
        if (!strcmp(op, "link")) {
            if (k > d->link_count)
                return ax_error(sh, "web", "there is no link %ld", k);
            char url[HTTP_URL_MAX];
            str_copy(url, html_link(d, (int)k - 1), sizeof(url));
            if (web_load(sh, url, NULL, 0))
                return 1;
            web_show(sh);
            return 0;
        }
        if (k > d->field_count)
            return ax_error(sh, "web", "there is no field %ld", k);
        struct html_field *f = &d->fields[k - 1];
        if (!strcmp(op, "type")) {
            char text[1024] = "";
            for (int i = 3; i < argc; i++) {
                if (i > 3)
                    str_append(text, " ", sizeof(text));
                str_append(text, argv[i], sizeof(text));
            }
            if (f->type == HT_CHECK || f->type == HT_RADIO || f->type == HT_SUBMIT)
                return ax_error(sh, "web", "field %ld is not for text: press it", k);
            f->value = html_intern(d, text);
            sh_printf(sh, "field %ld = \"%s\"\n", k, text);
            return 0;
        }
        if (f->type == HT_CHECK) {
            f->checked = !f->checked;
            sh_printf(sh, "checkbox %ld %s\n", k, f->checked ? "on" : "off");
            return 0;
        }
        if (f->type == HT_RADIO) {
            for (int i = 0; i < d->field_count; i++)
                if (d->fields[i].type == HT_RADIO && d->fields[i].form == f->form && d->fields[i].name == f->name)
                    d->fields[i].checked = false;
            f->checked = true;
            sh_printf(sh, "option %ld on\n", k);
            return 0;
        }
        char url[HTTP_URL_MAX], *body;
        size_t body_len;
        int err = html_submit(d, (int)k - 1, url, sizeof(url), &body, &body_len);
        if (err)
            return ax_error(sh, "web", "field %ld is not in a form", k);
        err = web_load(sh, url, body, body_len);
        kfree(body);
        if (err)
            return 1;
        web_show(sh);
        return 0;
    }
    if (web_load(sh, op, NULL, 0))
        return 1;
    web_show(sh);
    return 0;
}
