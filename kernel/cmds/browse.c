#include <synth/codepage.h>
#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/html.h>
#include <synth/http.h>
#include <synth/input.h>
#include <synth/path.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>

struct text_out {
    struct shell *sh;
    const struct html_doc *d;
    int width, col, indent, role, blank;
    bool space;
};

static int chars(const char *s, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++)
        if (((uint8_t)s[i] & 0xc0) != 0x80)
            c++;
    return c;
}

static const char *fallback(uint32_t cp)
{
    switch (cp) {
    case 0x2022:
        return "*";
    case 0x2013:
    case 0x2014:
    case 0x2212:
        return "-";
    case 0x2018:
    case 0x2019:
    case 0x2032:
        return "'";
    case 0x201c:
    case 0x201d:
    case 0x201e:
        return "\"";
    case 0x2026:
        return "...";
    case 0x2039:
        return "<";
    case 0x203a:
        return ">";
    case 0x2190:
        return "<-";
    case 0x2192:
        return "->";
    case 0x00a9:
        return "(c)";
    case 0x00ae:
        return "(R)";
    case 0x2122:
        return "TM";
    default:
        return "?";
    }
}

static void put(struct text_out *t, const char *s, size_t n, int role)
{
    if (role != t->role) {
        sh_role(t->sh, role);
        t->role = role;
    }
    char buf[256];
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) {
            buf[o++] = (char)c;
            i++;
        } else {
            int extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
            uint32_t cp = c & (0x3fu >> extra);
            size_t k = 1;
            for (; k <= (size_t)extra && i + k < n && ((uint8_t)s[i + k] & 0xc0) == 0x80; k++)
                cp = cp << 6 | ((uint8_t)s[i + k] & 0x3f);
            i += k;
            int ch = k == (size_t)extra + 1 ? codepage_from_unicode(cp) : -1;
            if (ch >= 0) {
                buf[o++] = (char)ch;
            } else {
                const char *alt = fallback(cp);
                while (*alt)
                    buf[o++] = *alt++;
            }
        }
        if (o > sizeof(buf) - 8) {
            stream_write(t->sh->out, buf, o);
            o = 0;
        }
    }
    if (o)
        stream_write(t->sh->out, buf, o);
}

static void newline(struct text_out *t)
{
    put(t, "\n", 1, t->role);
    t->col = 0;
    t->space = false;
}

static void end_line(struct text_out *t)
{
    if (t->col) {
        newline(t);
        t->blank = 0;
    }
}

static void gap(struct text_out *t)
{
    end_line(t);
    if (!t->blank) {
        newline(t);
        t->blank = 1;
    }
}

static void word(struct text_out *t, const char *s, size_t n, int role)
{
    int w = chars(s, n);
    if (t->col && t->col + (t->space ? 1 : 0) + w > t->width)
        newline(t);
    if (!t->col) {
        for (int i = 0; i < t->indent * 2 && i < t->width / 2; i++)
            put(t, " ", 1, ROLE_TEXT);
        t->col = MIN(t->indent * 2, t->width / 2);
    } else if (t->space) {
        put(t, " ", 1, ROLE_TEXT);
        t->col++;
    }
    put(t, s, n, role);
    t->col += w;
    t->space = false;
    t->blank = 0;
}

static int role_of(const struct html_run *r)
{
    if (r->link >= 0)
        return ROLE_ACCENT;
    if (r->link != -1 || (r->flags & HS_MUTED))
        return ROLE_MUTED;
    if (r->font == HF_H1 || r->font == HF_H2 || r->font == HF_H3 || r->font == HF_BOLD)
        return ROLE_HIGHLIGHT;
    return ROLE_TEXT;
}

static void text_run(struct text_out *t, const struct html_run *r)
{
    const char *s = t->d->text + r->off;
    size_t n = r->len;
    int role = role_of(r);
    t->indent = r->indent;
    if (r->flags & HS_PRE) {
        for (size_t i = 0; i < n;) {
            size_t e = i;
            while (e < n && s[e] != '\n')
                e++;
            if (e > i) {
                put(t, s + i, e - i, role);
                t->col += chars(s + i, e - i);
            }
            if (e < n) {
                newline(t);
                t->blank = 0;
            }
            i = e + 1;
        }
        return;
    }
    for (size_t i = 0; i < n;) {
        if (s[i] == ' ') {
            t->space = t->col > 0;
            i++;
            continue;
        }
        size_t e = i;
        while (e < n && s[e] != ' ')
            e++;
        word(t, s + i, e - i, role);
        i = e;
    }
}

static void field(struct text_out *t, const struct html_run *r)
{
    const struct html_field *f = &t->d->fields[HTML_LINK_FIELD(r->link)];
    char box[96];
    const char *v = html_str(t->d, f->value);
    if (f->type == HT_CHECK || f->type == HT_RADIO)
        ksnprintf(box, sizeof(box), f->type == HT_CHECK ? "[%c]" : "(%c)", f->checked ? 'x' : ' ');
    else if (f->type == HT_PASSWORD)
        ksnprintf(box, sizeof(box), "[%s]", v[0] ? "********" : "________");
    else
        ksnprintf(box, sizeof(box), "[%.40s%s]", v[0] ? v : html_str(t->d, f->hint), v[0] ? "" : "____");
    word(t, box, strlen(box), ROLE_MUTED);
}

static void render(struct shell *sh, const struct html_doc *d, bool links)
{
    struct text_out t = {sh, d, MAX(20, MIN(con_cols(), 200) - 1), 0, 0, -1, 1, false};
    if (d->title[0]) {
        word(&t, d->title, strlen(d->title), ROLE_ACCENT);
        gap(&t);
    }
    for (int i = 0; i < d->run_count; i++) {
        const struct html_run *r = &d->runs[i];
        t.indent = r->indent;
        switch (r->kind) {
        case HR_TEXT:
            if (r->flags & HS_BUTTON)
                word(&t, "[", 1, ROLE_MUTED);
            text_run(&t, r);
            if (r->flags & HS_BUTTON)
                put(&t, "]", 1, ROLE_MUTED), t.col++;
            if (links && r->link >= 0 && (i + 1 >= d->run_count || d->runs[i + 1].link != r->link)) {
                char ref[16];
                ksnprintf(ref, sizeof(ref), "[%d]", r->link + 1);
                put(&t, ref, strlen(ref), ROLE_MUTED);
                t.col += (int)strlen(ref);
            }
            break;
        case HR_BREAK:
            if (t.col)
                newline(&t);
            else
                gap(&t);
            break;
        case HR_BLOCK:
            if (r->off >= 6)
                gap(&t);
            else
                end_line(&t);
            break;
        case HR_GAP:
            put(&t, "  ", 2, ROLE_TEXT);
            t.col += 2;
            t.space = false;
            break;
        case HR_RULE:
            end_line(&t);
            for (int k = 0; k < t.width; k++)
                put(&t, "-", 1, ROLE_MUTED);
            newline(&t);
            break;
        case HR_FIELD:
            field(&t, r);
            break;
        }
    }
    end_line(&t);
    if (links && d->link_count) {
        gap(&t);
        put(&t, "Links:\n", 7, ROLE_HIGHLIGHT);
        for (int i = 0; i < d->link_count; i++) {
            char n[16];
            ksnprintf(n, sizeof(n), "%4d. ", i + 1);
            put(&t, n, strlen(n), ROLE_MUTED);
            const char *u = html_link(d, i);
            put(&t, u, strlen(u), ROLE_TEXT);
            put(&t, "\n", 1, ROLE_TEXT);
        }
    }
    sh_role(sh, ROLE_TEXT);
}

static bool cancelled(void)
{
    return input_break_pending();
}

static bool html_name(const char *path)
{
    return str_iends(path, ".html") || str_iends(path, ".htm") || str_iends(path, ".xhtml");
}

COMMAND(browse, "lynx", "system", "browse [-k] [-n] url|file",
        "show a web page or HTML file as text (-k: do not check the certificate, -n: no link list)")
{
    unsigned flags = 0;
    bool links = true;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-k"))
            flags |= HTTP_INSECURE;
        else if (!strcmp(argv[i], "-n"))
            links = false;
        else
            return command_usage(sh, command_find("browse"));
    }
    if (i >= argc)
        return command_usage(sh, command_find("browse"));
    struct html_doc doc;
    char path[PATH_MAX_LEN];
    if (!strstr(argv[i], "://") && sh_resolve(sh, argv[i], path, sizeof(path)) == 0 && vfs_exists(path)) {
        char *data;
        size_t size;
        int err = vfs_read_all(path, &data, &size);
        if (err)
            return sh_fs_error(sh, "browse", argv[i], err);
        char url[HTTP_URL_MAX];
        ksnprintf(url, sizeof(url), "file://%s", path);
        err = html_name(path) ? html_parse(&doc, data, size, url) : html_plain(&doc, data, size, url);
        kfree(data);
        if (err) {
            html_free(&doc);
            return sh_error(sh, "browse", "%s", err_str(err));
        }
        render(sh, &doc, links);
        html_free(&doc);
        return 0;
    }
    char url[HTTP_URL_MAX];
    if (strstr(argv[i], "://"))
        str_copy(url, argv[i], sizeof(url));
    else
        ksnprintf(url, sizeof(url), "http://%s", argv[i]);
    struct http_request req = {.url = url, .flags = flags, .cancelled = cancelled};
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    if (err == E_NOENT)
        return sh_error(sh, "browse", "%s: host not found", url);
    if (err)
        return sh_error(sh, "browse", "%s: %s", url, tls_strerror(err));
    char ct[128] = "";
    http_header(resp.headers, "Content-Type", ct, sizeof(ct));
    str_lower(ct);
    size_t len = resp.length;
    char *utf = html_to_utf8(resp.body, resp.length, ct, &len);
    const char *data = utf ? utf : resp.body;
    bool html = !ct[0] || strstr(ct, "html");
    err = html ? html_parse(&doc, data, len, resp.url) : html_plain(&doc, data, len, resp.url);
    kfree(utf);
    int status = resp.status;
    http_free(&resp);
    if (err) {
        html_free(&doc);
        return sh_error(sh, "browse", "%s", err_str(err));
    }
    render(sh, &doc, links);
    html_free(&doc);
    return status >= 400 ? 1 : 0;
}
