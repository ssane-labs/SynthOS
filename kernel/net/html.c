#include <synth/codepage.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/html.h>
#include <synth/path.h>
#include <synth/string.h>

#define MAX_LISTS 16

struct parser {
    struct html_doc *d;
    int bold, mono, pre, small, italic, heading, indent;
    int link, button, form, select;
    int lists, list_num[MAX_LISTS];
    bool list_ordered[MAX_LISTS];
    bool space, line_start, cell_first, failed;
    int suppress;
};

struct style {
    uint8_t font, flags, indent;
    int32_t link;
};

static const struct {
    const char *name;
    uint16_t cp;
} entities[] = {
    {"amp", '&'},      {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0xa0},    {"copy", 0xa9},     {"reg", 0xae},      {"trade", 0x2122},  {"mdash", 0x2014},
    {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xab},    {"raquo", 0xbb},    {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201c},  {"rdquo", 0x201d},  {"bdquo", 0x201e},  {"bull", 0x2022},
    {"middot", 0xb7},  {"times", 0xd7},    {"divide", 0xf7},   {"deg", 0xb0},      {"plusmn", 0xb1},
    {"euro", 0x20ac},  {"pound", 0xa3},    {"yen", 0xa5},      {"cent", 0xa2},     {"sect", 0xa7},
    {"para", 0xb6},    {"larr", 0x2190},   {"rarr", 0x2192},   {"uarr", 0x2191},   {"darr", 0x2193},
    {"harr", 0x2194},  {"shy", 0},         {"thinsp", ' '},    {"ensp", ' '},      {"emsp", ' '},
    {"zwj", 0},        {"zwnj", 0},        {"lrm", 0},         {"rlm", 0},         {"iexcl", 0xa1},
    {"iquest", 0xbf},  {"frac12", 0xbd},   {"frac14", 0xbc},   {"frac34", 0xbe},   {"sup2", 0xb2},
    {"sup3", 0xb3},    {"micro", 0xb5},    {"auml", 0xe4},     {"ouml", 0xf6},     {"uuml", 0xfc},
    {"Auml", 0xc4},    {"Ouml", 0xd6},     {"Uuml", 0xdc},     {"szlig", 0xdf},    {"eacute", 0xe9},
    {"egrave", 0xe8},  {"aacute", 0xe1},   {"agrave", 0xe0},   {"oacute", 0xf3},   {"iacute", 0xed},
    {"uacute", 0xfa},  {"ntilde", 0xf1},   {"ccedil", 0xe7},   {"Eacute", 0xc9},   {"minus", 0x2212},
    {"le", 0x2264},    {"ge", 0x2265},     {"ne", 0x2260},     {"infin", 0x221e},  {"pi", 0x3c0},
    {"check", 0x2713}, {"star", 0x2606},   {"rsaquo", 0x203a}, {"lsaquo", 0x2039}, {"prime", 0x2032},
};

static const struct {
    const char *name;
    uint8_t gap;
} blocks[] = {
    {"p", 10},      {"div", 0},     {"section", 0},     {"article", 0}, {"header", 0},  {"footer", 0},
    {"nav", 0},     {"main", 0},    {"aside", 0},       {"figure", 6},  {"figcaption", 0}, {"address", 0},
    {"center", 0},  {"details", 0}, {"summary", 0},     {"fieldset", 6}, {"legend", 0}, {"dl", 6},
    {"dt", 4},      {"table", 8},   {"caption", 0},     {"body", 0},    {"form", 0},    {"h1", 16},
    {"h2", 14},     {"h3", 12},     {"h4", 10},         {"h5", 10},     {"h6", 10},     {"ul", 6},
    {"ol", 6},      {"menu", 6},    {"blockquote", 10}, {"pre", 10},    {"dd", 0},      {"li", 2},
    {"tr", 0},      {"hgroup", 0},  {"search", 0},
};

static const char *const void_tags[] = {"area", "base", "br",   "col",   "embed", "hr",  "img",
                                        "input", "link", "meta", "source", "track", "wbr", "param"};

static bool reserve(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    size_t n = MAX(*cap * 2, MAX(need, (size_t)64));
    void *g = krealloc(*p, n * elem);
    if (!g)
        return false;
    *p = g;
    *cap = n;
    return true;
}

uint32_t html_intern(struct html_doc *d, const char *s)
{
    size_t n = strlen(s);
    if (!reserve((void **)&d->pool, &d->pool_cap, d->pool_len + n + 1, 1))
        return 0;
    uint32_t off = (uint32_t)d->pool_len;
    memcpy(d->pool + off, s, n + 1);
    d->pool_len += n + 1;
    return off;
}

const char *html_str(const struct html_doc *d, uint32_t off)
{
    return d->pool && off < d->pool_len ? d->pool + off : "";
}

const char *html_link(const struct html_doc *d, int link)
{
    return link >= 0 && link < d->link_count ? html_str(d, d->links[link]) : "";
}

static void doc_init(struct html_doc *d, const char *base)
{
    memset(d, 0, sizeof(*d));
    str_copy(d->base, base ? base : "", sizeof(d->base));
    html_intern(d, "");
}

void html_free(struct html_doc *d)
{
    kfree(d->text);
    kfree(d->runs);
    kfree(d->pool);
    kfree(d->links);
    kfree(d->fields);
    kfree(d->forms);
    memset(d, 0, sizeof(*d));
}

static bool has_scheme(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == ':')
            return p > s;
        if (!is_alnum(*p) && *p != '+' && *p != '-' && *p != '.')
            return false;
    }
    return false;
}

static bool resolve(const struct html_doc *d, const char *ref, char *out, size_t cap)
{
    while (is_space(*ref))
        ref++;
    if (has_scheme(ref)) {
        str_copy(out, ref, cap);
        return true;
    }
    if (str_starts(d->base, "file://")) {
        if (ref[0] == '#' || !ref[0]) {
            str_copy(out, d->base, cap);
            char *hash = strchr(out, '#');
            if (hash)
                *hash = 0;
            return true;
        }
        char dir[PATH_MAX_LEN], path[PATH_MAX_LEN], clean[PATH_MAX_LEN];
        str_copy(dir, d->base + 7, sizeof(dir));
        char *slash = strrchr(dir, '/');
        if (slash)
            slash[slash == dir ? 1 : 0] = 0;
        str_copy(clean, ref, sizeof(clean));
        for (char *c = clean; *c; c++)
            if (*c == '?' || *c == '#')
                *c = 0;
        if (path_resolve(dir, clean, path, sizeof(path)))
            return false;
        ksnprintf(out, cap, "file://%s", path);
        return true;
    }
    return http_resolve_url(d->base, ref, out, cap);
}

static size_t entity(const char *s, size_t n, uint32_t *cp)
{
    if (n > 2 && s[1] == '#') {
        size_t i = 2;
        bool hex = s[i] == 'x' || s[i] == 'X';
        if (hex)
            i++;
        size_t start = i;
        uint32_t v = 0;
        while (i < n && i < 12 && (hex ? is_xdigit(s[i]) : is_digit(s[i]))) {
            int c = s[i];
            v = v * (hex ? 16 : 10) + (uint32_t)(is_digit(c) ? c - '0' : to_lower(c) - 'a' + 10);
            i++;
        }
        if (i == start)
            return 0;
        if (i < n && s[i] == ';')
            i++;
        *cp = v && v < 0x110000 ? v : 0xfffd;
        return i;
    }
    size_t i = 1;
    while (i < n && i < 12 && is_alnum(s[i]))
        i++;
    size_t len = i - 1;
    for (size_t k = 0; k < ARRAY_SIZE(entities); k++) {
        if (strlen(entities[k].name) == len && !memcmp(entities[k].name, s + 1, len)) {
            *cp = entities[k].cp;
            if (i < n && s[i] == ';')
                i++;
            return i;
        }
    }
    return 0;
}

static void decode(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    bool space = true;
    for (size_t i = 0; i < n && o + 5 < cap;) {
        if (s[i] == '&') {
            uint32_t cp;
            size_t used = entity(s + i, n - i, &cp);
            if (used) {
                i += used;
                if (cp)
                    o += utf8_encode(cp, out + o);
                space = false;
                continue;
            }
        }
        if (is_space(s[i])) {
            if (!space)
                out[o++] = ' ';
            space = true;
            i++;
            continue;
        }
        out[o++] = s[i++];
        space = false;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

static bool attr(const char *a, const char *end, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    while (a < end) {
        while (a < end && (is_space(*a) || *a == '/'))
            a++;
        const char *k = a;
        while (a < end && !is_space(*a) && *a != '=' && *a != '>' && *a != '/')
            a++;
        size_t kl = (size_t)(a - k);
        while (a < end && is_space(*a))
            a++;
        const char *v = "";
        size_t vl = 0;
        if (a < end && *a == '=') {
            a++;
            while (a < end && is_space(*a))
                a++;
            if (a < end && (*a == '"' || *a == '\'')) {
                char q = *a++;
                v = a;
                while (a < end && *a != q)
                    a++;
                vl = (size_t)(a - v);
                if (a < end)
                    a++;
            } else {
                v = a;
                while (a < end && !is_space(*a) && *a != '>')
                    a++;
                vl = (size_t)(a - v);
            }
        }
        if (!kl) {
            if (a < end)
                a++;
            continue;
        }
        if (kl == nl && !str_ncasecmp(k, name, nl)) {
            if (out)
                decode(v, vl, out, cap);
            return true;
        }
    }
    return false;
}

static struct style current(struct parser *p)
{
    struct style s;
    if (p->heading)
        s.font = p->heading == 1 ? HF_H1 : p->heading == 2 ? HF_H2 : HF_H3;
    else if (p->mono || p->pre)
        s.font = HF_MONO;
    else if (p->bold)
        s.font = HF_BOLD;
    else if (p->small)
        s.font = HF_SMALL;
    else
        s.font = HF_NORMAL;
    s.flags = (uint8_t)((p->pre ? HS_PRE : 0) | (p->button >= 0 ? HS_BUTTON : 0) | (p->italic ? HS_ITALIC : 0));
    s.indent = (uint8_t)MIN(p->indent, 12);
    s.link = p->button >= 0 ? HTML_FIELD_LINK(p->button) : p->link;
    return s;
}

static struct html_run *add_run(struct parser *p, uint8_t kind, struct style s)
{
    struct html_doc *d = p->d;
    if (!reserve((void **)&d->runs, &d->run_cap, (size_t)d->run_count + 1, sizeof(struct html_run))) {
        p->failed = true;
        return NULL;
    }
    struct html_run *r = &d->runs[d->run_count++];
    *r = (struct html_run){(uint32_t)d->text_len, 0, kind, s.font, s.flags, s.indent, s.link};
    return r;
}

static void emit(struct parser *p, struct style s, const char *str, size_t n)
{
    struct html_doc *d = p->d;
    struct html_run *last = d->run_count ? &d->runs[d->run_count - 1] : NULL;
    if (!last || last->kind != HR_TEXT || last->font != s.font || last->flags != s.flags ||
        last->indent != s.indent || last->link != s.link)
        last = add_run(p, HR_TEXT, s);
    if (!last)
        return;
    if (!reserve((void **)&d->text, &d->text_cap, d->text_len + n + 1, 1)) {
        p->failed = true;
        return;
    }
    memcpy(d->text + d->text_len, str, n);
    d->text_len += n;
    d->text[d->text_len] = 0;
    last->len += (uint32_t)n;
}

static void text_flags(struct parser *p, const char *s, size_t n, uint8_t extra)
{
    if (p->suppress || !n)
        return;
    struct style st = current(p);
    st.flags |= extra;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (p->pre) {
            if (c == '\r') {
                i++;
                continue;
            }
            if (c == '\t') {
                emit(p, st, "    ", 4);
                i++;
                p->line_start = false;
                continue;
            }
        } else if (is_space(c)) {
            if (!p->space && !p->line_start)
                emit(p, st, " ", 1);
            p->space = true;
            i++;
            continue;
        }
        if (c == '&') {
            uint32_t cp;
            size_t used = entity(s + i, n - i, &cp);
            if (used) {
                i += used;
                if (cp) {
                    char buf[4];
                    emit(p, st, buf, utf8_encode(cp == 0xa0 ? ' ' : cp, buf));
                    p->space = false;
                    p->line_start = false;
                }
                continue;
            }
        }
        size_t j = i + 1;
        while (j < n && s[j] != '&' && !is_space(s[j]) && s[j] != '\r' && s[j] != '\t')
            j++;
        emit(p, st, s + i, j - i);
        i = j;
        p->space = false;
        p->line_start = false;
    }
}

static void text(struct parser *p, const char *s, size_t n)
{
    text_flags(p, s, n, 0);
}

static void block(struct parser *p, int gap)
{
    struct html_doc *d = p->d;
    p->line_start = true;
    p->space = true;
    struct html_run *last = d->run_count ? &d->runs[d->run_count - 1] : NULL;
    if (last && last->kind == HR_BLOCK) {
        last->off = MAX(last->off, (uint32_t)gap);
        return;
    }
    struct html_run *r = add_run(p, HR_BLOCK, current(p));
    if (r)
        r->off = (uint32_t)gap;
}

static int add_link(struct parser *p, const char *href)
{
    struct html_doc *d = p->d;
    char url[HTTP_URL_MAX];
    if (str_ncasecmp(href, "javascript:", 11) == 0 || !resolve(d, href, url, sizeof(url)))
        return -1;
    if (!reserve((void **)&d->links, &d->link_cap, (size_t)d->link_count + 1, sizeof(uint32_t))) {
        p->failed = true;
        return -1;
    }
    d->links[d->link_count] = html_intern(d, url);
    return d->link_count++;
}

static int add_field(struct parser *p, uint8_t type, const char *a, const char *ae)
{
    struct html_doc *d = p->d;
    if (!reserve((void **)&d->fields, &d->field_cap, (size_t)d->field_count + 1, sizeof(struct html_field))) {
        p->failed = true;
        return -1;
    }
    char v[512];
    struct html_field *f = &d->fields[d->field_count];
    *f = (struct html_field){.form = p->form, .type = type};
    if (attr(a, ae, "name", v, sizeof(v)))
        f->name = html_intern(d, v);
    if (attr(a, ae, "value", v, sizeof(v)))
        f->value = html_intern(d, v);
    else if (type == HT_CHECK || type == HT_RADIO)
        f->value = html_intern(d, "on");
    if (attr(a, ae, "placeholder", v, sizeof(v)))
        f->hint = html_intern(d, v);
    f->checked = attr(a, ae, "checked", NULL, 0) || attr(a, ae, "selected", NULL, 0);
    return d->field_count++;
}

static void field_run(struct parser *p, int field)
{
    if (field < 0 || p->suppress)
        return;
    struct style s = current(p);
    s.link = HTML_FIELD_LINK(field);
    add_run(p, HR_FIELD, s);
    p->space = false;
    p->line_start = false;
}

static void button_text(struct parser *p, int field, const char *label)
{
    int saved = p->button;
    if (!p->space && !p->line_start)
        text(p, " ", 1);
    p->button = field;
    text(p, label, strlen(label));
    p->button = saved;
    p->space = false;
}

static int block_gap(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(blocks); i++)
        if (!strcmp(blocks[i].name, name))
            return blocks[i].gap;
    return -1;
}

static bool in_list(const char *name, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(list[i], name))
            return true;
    return false;
}

static void input(struct parser *p, const char *a, const char *ae)
{
    struct html_doc *d = p->d;
    char type[16] = "text", v[256];
    attr(a, ae, "type", type, sizeof(type));
    str_lower(type);
    if (!strcmp(type, "hidden")) {
        add_field(p, HT_HIDDEN, a, ae);
    } else if (!strcmp(type, "submit") || !strcmp(type, "image") || !strcmp(type, "button")) {
        int f = add_field(p, !strcmp(type, "button") ? HT_HIDDEN : HT_SUBMIT, a, ae);
        if (f < 0)
            return;
        if (!strcmp(type, "button"))
            d->fields[f].name = 0;
        if (!attr(a, ae, "value", v, sizeof(v)) || !v[0])
            if (strcmp(type, "image") || !attr(a, ae, "alt", v, sizeof(v)) || !v[0])
                str_copy(v, "Submit", sizeof(v));
        button_text(p, f, v);
    } else if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        field_run(p, add_field(p, type[0] == 'c' ? HT_CHECK : HT_RADIO, a, ae));
    } else if (strcmp(type, "reset") && strcmp(type, "file")) {
        field_run(p, add_field(p, !strcmp(type, "password") ? HT_PASSWORD : HT_TEXT, a, ae));
    }
}

static void bullet(struct parser *p)
{
    char b[16];
    if (p->lists && p->list_ordered[MIN(p->lists, MAX_LISTS) - 1])
        ksnprintf(b, sizeof(b), "%d. ", ++p->list_num[MIN(p->lists, MAX_LISTS) - 1]);
    else
        str_copy(b, "\xe2\x80\xa2 ", sizeof(b));
    int saved = p->link;
    p->link = -1;
    text(p, b, strlen(b));
    p->link = saved;
    p->space = true;
}

static void start_tag(struct parser *p, const char *name, const char *a, const char *ae)
{
    struct html_doc *d = p->d;
    char v[HTTP_URL_MAX];
    int gap = block_gap(name);
    if (gap >= 0)
        block(p, gap);
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        p->heading = name[1] - '0';
    } else if (!strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "menu")) {
        if (p->lists < MAX_LISTS) {
            p->list_ordered[p->lists] = name[0] == 'o';
            p->list_num[p->lists] = 0;
            if (name[0] == 'o' && attr(a, ae, "start", v, sizeof(v)))
                p->list_num[p->lists] = (int)str_to_long(v, NULL, 10) - 1;
        }
        p->lists++;
        p->indent++;
    } else if (!strcmp(name, "li")) {
        bullet(p);
    } else if (!strcmp(name, "blockquote") || !strcmp(name, "dd")) {
        p->indent++;
    } else if (!strcmp(name, "pre")) {
        p->pre++;
    } else if (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "th") || !strcmp(name, "dt")) {
        p->bold++;
    } else if (!strcmp(name, "i") || !strcmp(name, "em") || !strcmp(name, "cite") || !strcmp(name, "var") ||
               !strcmp(name, "dfn")) {
        p->italic++;
    } else if (!strcmp(name, "code") || !strcmp(name, "kbd") || !strcmp(name, "samp") || !strcmp(name, "tt")) {
        p->mono++;
    } else if (!strcmp(name, "small") || !strcmp(name, "sub") || !strcmp(name, "sup")) {
        p->small++;
    }
    if (!strcmp(name, "td") || !strcmp(name, "th")) {
        if (!p->cell_first && !p->suppress) {
            add_run(p, HR_GAP, current(p));
            p->space = true;
        }
        p->cell_first = false;
    } else if (!strcmp(name, "tr")) {
        p->cell_first = true;
    } else if (!strcmp(name, "a")) {
        p->link = attr(a, ae, "href", v, sizeof(v)) ? add_link(p, v) : -1;
    } else if (!strcmp(name, "img")) {
        if (attr(a, ae, "alt", v, sizeof(v)) && v[0]) {
            char label[300];
            ksnprintf(label, sizeof(label), "[%s]", v);
            if (!p->space && !p->line_start)
                text(p, " ", 1);
            text_flags(p, label, strlen(label), HS_MUTED);
        }
    } else if (!strcmp(name, "br")) {
        if (!p->suppress)
            add_run(p, HR_BREAK, current(p));
        p->line_start = true;
        p->space = true;
    } else if (!strcmp(name, "hr")) {
        block(p, 8);
        add_run(p, HR_RULE, current(p));
        block(p, 8);
    } else if (!strcmp(name, "base")) {
        char url[HTTP_URL_MAX];
        if (attr(a, ae, "href", v, sizeof(v)) && resolve(d, v, url, sizeof(url)))
            str_copy(d->base, url, sizeof(d->base));
    } else if (!strcmp(name, "form")) {
        if (reserve((void **)&d->forms, &d->form_cap, (size_t)d->form_count + 1, sizeof(struct html_form))) {
            struct html_form *f = &d->forms[d->form_count];
            char url[HTTP_URL_MAX];
            if (!attr(a, ae, "action", v, sizeof(v)) || !resolve(d, v, url, sizeof(url)))
                str_copy(url, d->base, sizeof(url));
            f->action = html_intern(d, url);
            f->post = attr(a, ae, "method", v, sizeof(v)) && !str_casecmp(v, "post");
            p->form = d->form_count++;
        }
    } else if (!strcmp(name, "input")) {
        input(p, a, ae);
    } else if (!strcmp(name, "button")) {
        char type[16] = "submit";
        attr(a, ae, "type", type, sizeof(type));
        if (!str_casecmp(type, "submit")) {
            if (!p->space && !p->line_start)
                text(p, " ", 1);
            p->button = add_field(p, HT_SUBMIT, a, ae);
            p->space = true;
        }
    } else if (!strcmp(name, "select")) {
        p->select = add_field(p, HT_SELECT, a, ae);
        p->suppress++;
    } else if (!strcmp(name, "option") && p->select >= 0) {
        struct html_field *f = &d->fields[p->select];
        bool selected = attr(a, ae, "selected", NULL, 0);
        if (attr(a, ae, "value", v, sizeof(v)) && (!f->checked || selected) && (selected || !f->value)) {
            f->value = html_intern(d, v);
            f->checked = selected;
        }
    }
}

static void end_tag(struct parser *p, const char *name)
{
    int gap = block_gap(name);
    if (gap >= 0)
        block(p, gap);
#define DEC(x) (x) = (x) > 0 ? (x) - 1 : 0
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        p->heading = 0;
    } else if (!strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "menu")) {
        DEC(p->lists);
        DEC(p->indent);
    } else if (!strcmp(name, "blockquote") || !strcmp(name, "dd")) {
        DEC(p->indent);
    } else if (!strcmp(name, "pre")) {
        DEC(p->pre);
    } else if (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "th") || !strcmp(name, "dt")) {
        DEC(p->bold);
    } else if (!strcmp(name, "i") || !strcmp(name, "em") || !strcmp(name, "cite") || !strcmp(name, "var") ||
               !strcmp(name, "dfn")) {
        DEC(p->italic);
    } else if (!strcmp(name, "code") || !strcmp(name, "kbd") || !strcmp(name, "samp") || !strcmp(name, "tt")) {
        DEC(p->mono);
    } else if (!strcmp(name, "small") || !strcmp(name, "sub") || !strcmp(name, "sup")) {
        DEC(p->small);
    } else if (!strcmp(name, "a")) {
        p->link = -1;
    } else if (!strcmp(name, "button")) {
        p->button = -1;
    } else if (!strcmp(name, "form")) {
        p->form = -1;
    } else if (!strcmp(name, "select") && p->select >= 0) {
        DEC(p->suppress);
        field_run(p, p->select);
        p->select = -1;
    }
#undef DEC
}

static size_t find_close(const char *src, size_t i, size_t len, const char *name)
{
    size_t nl = strlen(name);
    for (; i + nl + 2 <= len; i++) {
        if (src[i] == '<' && src[i + 1] == '/' && !str_ncasecmp(src + i + 2, name, nl) &&
            (i + nl + 2 == len || !is_alnum(src[i + nl + 2])))
            return i;
    }
    return len;
}

static size_t skip_tag(const char *src, size_t i, size_t len)
{
    const char *gt = memchr(src + i, '>', len - i);
    return gt ? (size_t)(gt - src) + 1 : len;
}

static size_t skip_element(const char *src, size_t i, size_t len, const char *name)
{
    size_t nl = strlen(name);
    int depth = 1;
    while (i < len) {
        const char *lt = memchr(src + i, '<', len - i);
        if (!lt)
            return len;
        i = (size_t)(lt - src);
        bool close = i + 1 < len && src[i + 1] == '/';
        size_t k = i + 1 + (close ? 1 : 0);
        if (k + nl <= len && !str_ncasecmp(src + k, name, nl) && (k + nl == len || !is_alnum(src[k + nl]))) {
            depth += close ? -1 : 1;
            if (!depth)
                return skip_tag(src, i, len);
        }
        i++;
    }
    return len;
}

static bool hidden(const char *a, const char *ae)
{
    char v[256];
    if (attr(a, ae, "hidden", NULL, 0))
        return true;
    if (!attr(a, ae, "style", v, sizeof(v)))
        return false;
    for (char *s = v, *o = v;; s++) {
        if (*s != ' ')
            *o++ = (char)to_lower(*s);
        if (!*s)
            break;
    }
    return strstr(v, "display:none") != NULL;
}

int html_parse(struct html_doc *d, const char *src, size_t len, const char *base_url)
{
    doc_init(d, base_url);
    struct parser p = {.d = d, .link = -1, .button = -1, .form = -1, .select = -1, .space = true, .line_start = true};
    size_t i = 0;
    while (i < len && !p.failed) {
        if (src[i] != '<') {
            const char *lt = memchr(src + i, '<', len - i);
            size_t j = lt ? (size_t)(lt - src) : len;
            text(&p, src + i, j - i);
            i = j;
            continue;
        }
        if (i + 4 <= len && !memcmp(src + i, "<!--", 4)) {
            const char *e = NULL;
            for (size_t k = i + 4; k + 3 <= len; k++)
                if (src[k] == '-' && src[k + 1] == '-' && src[k + 2] == '>') {
                    e = src + k;
                    break;
                }
            i = e ? (size_t)(e - src) + 3 : len;
            continue;
        }
        size_t k = i + 1;
        bool close = k < len && src[k] == '/';
        if (close)
            k++;
        if (k >= len || !(is_alpha(src[k]) || (!close && (src[k] == '!' || src[k] == '?')))) {
            text(&p, "<", 1);
            i++;
            continue;
        }
        if (src[k] == '!' || src[k] == '?') {
            i = skip_tag(src, k, len);
            continue;
        }
        size_t e = k;
        char q = 0, prev = 0;
        while (e < len && (q || src[e] != '>')) {
            if (q) {
                if (src[e] == q)
                    q = 0;
            } else if ((src[e] == '"' || src[e] == '\'') && prev == '=') {
                q = src[e];
            }
            if (!is_space(src[e]))
                prev = src[e];
            e++;
        }
        char name[16];
        size_t nl = 0;
        while (k < e && (is_alnum(src[k]) || src[k] == '-' || src[k] == ':')) {
            if (nl < sizeof(name) - 1)
                name[nl++] = (char)to_lower(src[k]);
            k++;
        }
        name[nl] = 0;
        const char *a = src + k, *ae = src + e;
        i = e < len ? e + 1 : len;
        if (close) {
            end_tag(&p, name);
            continue;
        }
        if (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "svg") || !strcmp(name, "template") ||
            !strcmp(name, "iframe") || !strcmp(name, "title") || !strcmp(name, "textarea") ||
            !strcmp(name, "math") || !strcmp(name, "noembed") || !strcmp(name, "object")) {
            size_t c = find_close(src, i, len, name);
            if (!strcmp(name, "title") && !d->title[0]) {
                decode(src + i, c - i, d->title, sizeof(d->title));
            } else if (!strcmp(name, "textarea")) {
                int f = add_field(&p, HT_TEXTAREA, a, ae);
                if (f >= 0) {
                    char *v = kmalloc(c - i + 8);
                    if (v) {
                        decode(src + i, c - i, v, c - i + 8);
                        d->fields[f].value = html_intern(d, v);
                        kfree(v);
                    }
                    field_run(&p, f);
                }
            }
            i = c < len ? skip_tag(src, c, len) : len;
            continue;
        }
        if (!in_list(name, void_tags, ARRAY_SIZE(void_tags)) && hidden(a, ae)) {
            i = skip_element(src, i, len, name);
            continue;
        }
        start_tag(&p, name, a, ae);
        if (!strcmp(name, "pre") && i < len && src[i] == '\n')
            i++;
        else if (!strcmp(name, "pre") && i + 1 < len && src[i] == '\r' && src[i + 1] == '\n')
            i += 2;
    }
    return p.failed ? E_NOMEM : 0;
}

int html_plain(struct html_doc *d, const char *src, size_t len, const char *base_url)
{
    doc_init(d, base_url);
    struct parser p = {.d = d, .link = -1, .button = -1, .form = -1, .select = -1, .pre = 1, .line_start = true};
    struct style st = current(&p);
    for (size_t i = 0; i < len && !p.failed;) {
        size_t j = i;
        while (j < len && src[j] != '\r' && src[j] != '\t')
            j++;
        if (j > i)
            emit(&p, st, src + i, j - i);
        if (j < len && src[j] == '\t')
            emit(&p, st, "    ", 4);
        i = j + 1;
    }
    return p.failed ? E_NOMEM : 0;
}

static const uint16_t cp1251_high[64] = {
    0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x0409, 0x2039, 0x040a,
    0x040c, 0x040b, 0x040f, 0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0xfffd, 0x2122,
    0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f, 0x00a0, 0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6,
    0x00a7, 0x0401, 0x00a9, 0x0404, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x0407, 0x00b0, 0x00b1, 0x0406, 0x0456,
    0x0491, 0x00b5, 0x00b6, 0x00b7, 0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457,
};

static const uint16_t cp1252_high[32] = {
    0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
    0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd, 0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178,
};

static bool find_charset(const char *s, size_t len, char *out, size_t cap)
{
    for (size_t i = 0; i + 8 < len; i++) {
        if (str_ncasecmp(s + i, "charset=", 8))
            continue;
        i += 8;
        while (i < len && (s[i] == '"' || s[i] == '\'' || s[i] == ' '))
            i++;
        size_t n = 0;
        while (i < len && n + 1 < cap && (is_alnum(s[i]) || s[i] == '-' || s[i] == '_'))
            out[n++] = (char)to_lower(s[i++]);
        out[n] = 0;
        return n > 0;
    }
    return false;
}

char *html_to_utf8(const char *src, size_t len, const char *content_type, size_t *out_len)
{
    char cs[32];
    if (!(content_type && find_charset(content_type, strlen(content_type), cs, sizeof(cs))) &&
        !find_charset(src, MIN(len, (size_t)4096), cs, sizeof(cs)))
        return NULL;
    int table;
    if (!strcmp(cs, "windows-1251") || !strcmp(cs, "cp1251") || !strcmp(cs, "x-cp1251"))
        table = 1;
    else if (!strcmp(cs, "windows-1252") || !strcmp(cs, "iso-8859-1") || !strcmp(cs, "latin1") ||
             !strcmp(cs, "iso-8859-15") || !strcmp(cs, "us-ascii"))
        table = 2;
    else
        return NULL;
    char *out = kmalloc(len * 3 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)src[i];
        uint32_t cp = c;
        if (c >= 0x80) {
            if (table == 1)
                cp = c >= 0xc0 ? 0x410u + (c - 0xc0u) : cp1251_high[c - 0x80];
            else if (c < 0xa0)
                cp = cp1252_high[c - 0x80];
        }
        o += utf8_encode(cp, out + o);
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

struct sbuf {
    char *p;
    size_t len, cap;
    bool failed;
};

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
    if (!reserve((void **)&b->p, &b->cap, b->len + n + 1, 1)) {
        b->failed = true;
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_encode(struct sbuf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            sb_put(b, s, 1);
        } else if (c == ' ') {
            sb_put(b, "+", 1);
        } else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            sb_put(b, e, 3);
        }
    }
}

int html_submit(const struct html_doc *d, int field, char *url, size_t url_cap, char **body, size_t *body_len)
{
    *body = NULL;
    *body_len = 0;
    if (field < 0 || field >= d->field_count || d->fields[field].form < 0)
        return E_INVAL;
    int form = d->fields[field].form;
    int submitter = d->fields[field].type == HT_SUBMIT ? field : -1;
    for (int i = 0; i < d->field_count && submitter < 0; i++)
        if (d->fields[i].form == form && d->fields[i].type == HT_SUBMIT)
            submitter = i;
    struct sbuf b = {0};
    for (int i = 0; i < d->field_count; i++) {
        const struct html_field *f = &d->fields[i];
        if (f->form != form || !f->name || (f->type == HT_SUBMIT && i != submitter) ||
            ((f->type == HT_CHECK || f->type == HT_RADIO) && !f->checked))
            continue;
        if (b.len)
            sb_put(&b, "&", 1);
        sb_encode(&b, html_str(d, f->name));
        sb_put(&b, "=", 1);
        sb_encode(&b, html_str(d, f->value));
    }
    if (b.failed) {
        kfree(b.p);
        return E_NOMEM;
    }
    const struct html_form *f = &d->forms[form];
    str_copy(url, html_str(d, f->action), url_cap);
    if (f->post) {
        *body = b.p ? b.p : kzalloc(1);
        *body_len = b.len;
        return *body ? 0 : E_NOMEM;
    }
    char *q = strchr(url, '?');
    if (q)
        *q = 0;
    char *hash = strchr(url, '#');
    if (hash)
        *hash = 0;
    str_append(url, "?", url_cap);
    if (b.p)
        str_append(url, b.p, url_cap);
    kfree(b.p);
    return 0;
}
