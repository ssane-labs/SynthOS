#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/net.h>
#include <synth/string.h>
#include <synth/tls.h>

#define DEFAULT_MAX (64u << 20)
#define IDLE_TIMEOUT 20000

struct conn {
    int sock;
    struct tls *tls;
};

static size_t span_until(const char *s, const char *stop)
{
    size_t n = 0;
    while (s[n] && !strchr(stop, s[n]))
        n++;
    return n;
}

bool http_parse_url(const char *s, struct http_url *u)
{
    memset(u, 0, sizeof(*u));
    if (str_ncasecmp(s, "https://", 8) == 0) {
        u->https = true;
        s += 8;
    } else if (str_ncasecmp(s, "http://", 7) == 0) {
        s += 7;
    } else if (strstr(s, "://")) {
        return false;
    }
    size_t host_len = span_until(s, "/?#");
    if (!host_len || host_len >= sizeof(u->host))
        return false;
    memcpy(u->host, s, host_len);
    u->port = u->https ? 443 : 80;
    char *colon = strchr(u->host, ':');
    if (colon) {
        *colon = 0;
        long port = str_to_long(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535)
            return false;
        u->port = (uint16_t)port;
    }
    const char *rest = s + host_len;
    if (*rest == '/')
        str_copy(u->path, rest, sizeof(u->path));
    else
        ksnprintf(u->path, sizeof(u->path), "/%s", rest);
    char *hash = strchr(u->path, '#');
    if (hash)
        *hash = 0;
    return true;
}

static void normalize(char *path)
{
    char *query = strchr(path, '?');
    char tail[HTTP_URL_MAX] = "";
    if (query) {
        str_copy(tail, query, sizeof(tail));
        *query = 0;
    }
    char out[HTTP_URL_MAX];
    size_t n = 0;
    for (char *p = path; *p;) {
        while (*p == '/')
            p++;
        char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - seg);
        bool dir = *p == '/';
        if (len == 1 && seg[0] == '.')
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 0 && out[--n] != '/')
                ;
            continue;
        }
        if (n + len + 2 >= sizeof(out))
            break;
        out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
        if (!dir && !*p)
            break;
    }
    size_t plen = strlen(path);
    if (!n || (plen && path[plen - 1] == '/'))
        out[n++] = '/';
    out[n] = 0;
    ksnprintf(path, HTTP_URL_MAX, "%s%s", out, tail);
}

bool http_resolve_url(const char *base, const char *ref, char *out, size_t cap)
{
    while (*ref == ' ')
        ref++;
    if (str_ncasecmp(ref, "http://", 7) == 0 || str_ncasecmp(ref, "https://", 8) == 0) {
        str_copy(out, ref, cap);
        return true;
    }
    struct http_url b;
    if (!http_parse_url(base, &b))
        return false;
    const char *scheme = b.https ? "https" : "http";
    char origin[300];
    if (b.port == (b.https ? 443 : 80))
        ksnprintf(origin, sizeof(origin), "%s://%s", scheme, b.host);
    else
        ksnprintf(origin, sizeof(origin), "%s://%s:%u", scheme, b.host, b.port);
    if (ref[0] == '/' && ref[1] == '/') {
        ksnprintf(out, cap, "%s:%s", scheme, ref);
        return true;
    }
    char path[HTTP_URL_MAX];
    if (ref[0] == '/') {
        str_copy(path, ref, sizeof(path));
    } else if (ref[0] == '#' || !ref[0]) {
        str_copy(path, b.path, sizeof(path));
    } else if (ref[0] == '?') {
        str_copy(path, b.path, sizeof(path));
        char *q = strchr(path, '?');
        if (q)
            *q = 0;
        str_append(path, ref, sizeof(path));
    } else {
        str_copy(path, b.path, sizeof(path));
        char *q = strchr(path, '?');
        if (q)
            *q = 0;
        char *slash = strrchr(path, '/');
        if (slash)
            slash[1] = 0;
        str_append(path, ref, sizeof(path));
    }
    char *hash = strchr(path, '#');
    if (hash)
        *hash = 0;
    normalize(path);
    ksnprintf(out, cap, "%s%s", origin, path);
    return true;
}

bool http_header(const char *headers, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    for (const char *p = headers; p && *p;) {
        const char *eol = strchr(p, '\n');
        if (str_ncasecmp(p, name, n) == 0 && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t k = 0;
            while (v[k] && v[k] != '\r' && v[k] != '\n')
                k++;
            k = MIN(k, cap - 1);
            memcpy(out, v, k);
            out[k] = 0;
            return true;
        }
        p = eol ? eol + 1 : NULL;
    }
    return false;
}

static int conn_send(struct conn *c, const void *buf, size_t n)
{
    if (c->tls) {
        long k = tls_write(c->tls, buf, n);
        return k < 0 ? (int)k : 0;
    }
    const char *p = buf;
    uint64_t start = hal_ticks_ms();
    while (n) {
        long k = net_send(c->sock, p, n);
        if (k == E_AGAIN) {
            if (hal_ticks_ms() - start > IDLE_TIMEOUT)
                return E_TIMEDOUT;
            net_wait(1);
            continue;
        }
        if (k < 0)
            return (int)k;
        p += k;
        n -= (size_t)k;
        start = hal_ticks_ms();
    }
    return 0;
}

static long conn_recv(struct conn *c, void *buf, size_t n)
{
    return c->tls ? tls_read(c->tls, buf, n) : net_recv(c->sock, buf, n);
}

static void conn_close(struct conn *c)
{
    tls_close(c->tls);
    c->tls = NULL;
    if (c->sock >= 0)
        net_close(c->sock);
    c->sock = -1;
}

static size_t dechunk(char *body, size_t length, bool *complete)
{
    size_t in = 0, out = 0;
    *complete = false;
    while (in < length) {
        size_t size = 0;
        size_t start = in;
        while (in < length && is_xdigit(body[in])) {
            int c = body[in++];
            size = size * 16 + (size_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        if (in == start)
            break;
        while (in < length && body[in] != '\n')
            in++;
        in++;
        if (!size) {
            *complete = true;
            break;
        }
        if (in >= length)
            break;
        size = MIN(size, length - in);
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    return out;
}

static int body_state(const char *buf, size_t got, size_t *header_len, size_t *expected, bool *chunked)
{
    const char *end = NULL;
    for (size_t i = 3; i < got; i++)
        if (buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r') {
            end = buf + i + 1;
            break;
        }
    if (!end)
        return 0;
    *header_len = (size_t)(end - buf);
    char value[64];
    *chunked = false;
    *expected = (size_t)-1;
    char headers[4096];
    size_t hl = MIN(*header_len, sizeof(headers) - 1);
    memcpy(headers, buf, hl);
    headers[hl] = 0;
    if (http_header(headers, "Transfer-Encoding", value, sizeof(value)) && strstr(value, "chunked"))
        *chunked = true;
    else if (http_header(headers, "Content-Length", value, sizeof(value)))
        *expected = (size_t)str_to_long(value, NULL, 10);
    return 1;
}

static bool chunked_done(const char *body, size_t n)
{
    return n >= 5 && memcmp(body + n - 5, "0\r\n\r\n", 5) == 0;
}

#define MAX_COOKIES 160
#define COOKIE_HEADER_MAX 6144

struct cookie {
    char domain[128];
    char path[96];
    char name[64];
    char *value;
    bool secure, host_only;
};

static struct cookie *jar;
static int jar_next;

static bool domain_match(const char *host, const char *domain, bool exact)
{
    size_t h = strlen(host), d = strlen(domain);
    if (!str_casecmp(host, domain))
        return true;
    return !exact && h > d && host[h - d - 1] == '.' && !str_casecmp(host + h - d, domain);
}

static void cookie_store(const struct http_url *u, const char *line)
{
    struct cookie c = {0};
    size_t n = span_until(line, "=;");
    if (line[n] != '=' || !n || n >= sizeof(c.name))
        return;
    memcpy(c.name, line, n);
    const char *v = line + n + 1;
    size_t vl = span_until(v, ";");
    str_copy(c.domain, u->host, sizeof(c.domain));
    str_copy(c.path, "/", sizeof(c.path));
    c.host_only = true;
    bool remove = false;
    for (const char *a = v + vl; *a == ';';) {
        a++;
        while (*a == ' ')
            a++;
        size_t al = span_until(a, ";");
        char item[160];
        str_copy(item, a, MIN(al + 1, sizeof(item)));
        char *eq = strchr(item, '=');
        const char *val = eq ? eq + 1 : "";
        if (eq)
            *eq = 0;
        if (!str_casecmp(item, "domain") && *val) {
            if (*val == '.')
                val++;
            if (!domain_match(u->host, val, false))
                return;
            str_copy(c.domain, val, sizeof(c.domain));
            c.host_only = false;
        } else if (!str_casecmp(item, "path") && *val == '/') {
            str_copy(c.path, val, sizeof(c.path));
        } else if (!str_casecmp(item, "secure")) {
            c.secure = true;
        } else if (!str_casecmp(item, "max-age")) {
            remove = str_to_long(val, NULL, 10) <= 0;
        } else if (!str_casecmp(item, "expires")) {
            remove = strstr(val, "1970") != NULL;
        }
        a += al;
    }
    if (!jar && !(jar = kzalloc(MAX_COOKIES * sizeof(*jar))))
        return;
    int slot = -1;
    for (int i = 0; i < MAX_COOKIES; i++)
        if (jar[i].value && !strcmp(jar[i].name, c.name) && !str_casecmp(jar[i].domain, c.domain) &&
            !strcmp(jar[i].path, c.path))
            slot = i;
    if (remove) {
        if (slot >= 0) {
            kfree(jar[slot].value);
            jar[slot].value = NULL;
        }
        return;
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_COOKIES && slot < 0; i++)
            if (!jar[i].value)
                slot = i;
        if (slot < 0) {
            slot = jar_next;
            jar_next = (jar_next + 1) % MAX_COOKIES;
        }
    }
    c.value = kmalloc(vl + 1);
    if (!c.value)
        return;
    memcpy(c.value, v, vl);
    c.value[vl] = 0;
    kfree(jar[slot].value);
    jar[slot] = c;
}

static void cookies_from(const struct http_url *u, const char *headers)
{
    for (const char *p = headers; p && *p;) {
        if (!str_ncasecmp(p, "Set-Cookie:", 11)) {
            const char *v = p + 11;
            while (*v == ' ')
                v++;
            size_t n = span_until(v, "\r\n");
            char *line = kmalloc(n + 1);
            if (line) {
                memcpy(line, v, n);
                line[n] = 0;
                cookie_store(u, line);
                kfree(line);
            }
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
}

static int cookies_for(const struct http_url *u, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; jar && i < MAX_COOKIES; i++) {
        const struct cookie *c = &jar[i];
        if (!c->value || (c->secure && !u->https) || !domain_match(u->host, c->domain, c->host_only) ||
            !str_starts(u->path, c->path))
            continue;
        size_t need = strlen(c->name) + strlen(c->value) + 4;
        if (n + need + 16 > cap)
            break;
        n += (size_t)ksnprintf(out + n, cap - n, "%s%s=%s", n ? "; " : "Cookie: ", c->name, c->value);
    }
    if (n)
        n += (size_t)ksnprintf(out + n, cap - n, "\r\n");
    return (int)n;
}

void http_cookies_clear(void)
{
    for (int i = 0; jar && i < MAX_COOKIES; i++) {
        kfree(jar[i].value);
        jar[i].value = NULL;
    }
}

int http_cookie_count(void)
{
    int n = 0;
    for (int i = 0; jar && i < MAX_COOKIES; i++)
        n += jar[i].value != NULL;
    return n;
}

static int fetch_once(const struct http_request *req, const char *url, struct http_response *resp)
{
    struct http_url u;
    if (!http_parse_url(url, &u))
        return E_INVAL;
    uint32_t ip;
    if (req->progress)
        req->progress(req->ctx, "resolving", 0, 0);
    int err = net_resolve(u.host, &ip, 5000);
    if (err)
        return err;
    struct conn c = {net_socket(NET_TCP), NULL};
    if (c.sock < 0)
        return c.sock;
    if (req->progress)
        req->progress(req->ctx, "connecting", 0, 0);
    if ((err = net_connect(c.sock, ip, u.port, 10000))) {
        conn_close(&c);
        return err;
    }
    if (u.https) {
        if (req->progress)
            req->progress(req->ctx, "securing", 0, 0);
        c.tls = tls_connect(c.sock, u.host, !(req->flags & HTTP_INSECURE), &err);
        if (!c.tls) {
            conn_close(&c);
            return err;
        }
        ksnprintf(resp->tls, sizeof(resp->tls), "%s, %s, certificate for %s", tls_version(c.tls), tls_cipher(c.tls),
                  tls_peer_name(c.tls));
    }
    const char *method = req->method ? req->method : req->flags & HTTP_HEAD ? "HEAD" : "GET";
    char host[300];
    if (u.port == (u.https ? 443 : 80))
        str_copy(host, u.host, sizeof(host));
    else
        ksnprintf(host, sizeof(host), "%s:%u", u.host, u.port);
    char *head = kmalloc(HTTP_URL_MAX + 2048 + COOKIE_HEADER_MAX);
    if (!head) {
        conn_close(&c);
        return E_NOMEM;
    }
    int n = ksnprintf(head, HTTP_URL_MAX + 2048 + COOKIE_HEADER_MAX,
                      "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (SynthOS) SynthOS-HTTP/1.0\r\n"
                      "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
                      method, u.path, host);
    if (req->body)
        n += ksnprintf(head + n, (size_t)(HTTP_URL_MAX + 2048 + COOKIE_HEADER_MAX - n), "Content-Length: %u\r\n",
                       (unsigned)req->body_len);
    if (!(req->flags & HTTP_NO_COOKIES))
        n += cookies_for(&u, head + n, COOKIE_HEADER_MAX);
    if (req->headers)
        n += ksnprintf(head + n, (size_t)(HTTP_URL_MAX + 2048 + COOKIE_HEADER_MAX - n), "%s", req->headers);
    n += ksnprintf(head + n, (size_t)(HTTP_URL_MAX + 2048 + COOKIE_HEADER_MAX - n), "\r\n");
    err = conn_send(&c, head, (size_t)n);
    kfree(head);
    if (!err && req->body && req->body_len)
        err = conn_send(&c, req->body, req->body_len);
    if (err) {
        conn_close(&c);
        return err;
    }
    size_t cap = 64 * 1024, got = 0, limit = req->max_size ? req->max_size : DEFAULT_MAX;
    char *buf = kmalloc(cap + 1);
    if (!buf) {
        conn_close(&c);
        return E_NOMEM;
    }
    size_t header_len = 0, expected = (size_t)-1, streamed = 0;
    bool chunked = false, have_headers = false, stream = false;
    uint64_t last = hal_ticks_ms();
    for (;;) {
        if (req->cancelled && req->cancelled()) {
            err = E_INTR;
            break;
        }
        if (got == cap) {
            if (cap >= limit) {
                err = E_NOMEM;
                break;
            }
            size_t next = MIN(cap * 2, limit);
            char *grown = krealloc(buf, next + 1);
            if (!grown) {
                err = E_NOMEM;
                break;
            }
            buf = grown;
            cap = next;
        }
        long k = conn_recv(&c, buf + got, cap - got);
        if (k > 0) {
            got += (size_t)k;
            last = hal_ticks_ms();
            if (!have_headers) {
                have_headers = body_state(buf, got, &header_len, &expected, &chunked);
                stream = have_headers && req->sink && !chunked && strchr(buf, ' ') && strchr(buf, ' ')[1] == '2';
            }
            if (stream && (got - header_len >= (256u << 10) || got - header_len + streamed >= expected)) {
                size_t n = MIN(got - header_len, expected - streamed);
                if ((err = req->sink(req->ctx, buf + header_len, n)) != 0)
                    break;
                streamed += n;
                got = header_len;
            }
            if (have_headers && req->progress)
                req->progress(req->ctx, "downloading", streamed + got - header_len, expected == (size_t)-1 ? 0 : expected);
            if (have_headers && expected != (size_t)-1 && streamed + got - header_len >= expected)
                break;
            if (have_headers && chunked && chunked_done(buf + header_len, got - header_len))
                break;
            if (have_headers && (req->flags & HTTP_HEAD))
                break;
            continue;
        }
        if (k == 0)
            break;
        if (k != E_AGAIN) {
            if (!got)
                err = (int)k;
            break;
        }
        if (hal_ticks_ms() - last > IDLE_TIMEOUT) {
            err = E_TIMEDOUT;
            break;
        }
        net_wait(1);
    }
    conn_close(&c);
    if (!err && stream && got > header_len) {
        err = req->sink(req->ctx, buf + header_len, got - header_len);
        streamed += got - header_len;
        got = header_len;
    }
    if (!err && !have_headers)
        err = got ? E_INVAL : E_CONNRESET;
    if (err) {
        kfree(buf);
        return err;
    }
    buf[got] = 0;
    if (!str_starts(buf, "HTTP/") || !strchr(buf, ' ')) {
        kfree(buf);
        return E_INVAL;
    }
    resp->status = (int)str_to_long(strchr(buf, ' ') + 1, NULL, 10);
    resp->headers = kmalloc(header_len + 1);
    size_t body_len = got - header_len;
    if (chunked) {
        bool complete;
        body_len = dechunk(buf + header_len, body_len, &complete);
    } else if (expected != (size_t)-1) {
        body_len = MIN(body_len, expected);
    }
    resp->body = kmalloc(body_len + 1);
    if (!resp->headers || !resp->body) {
        kfree(buf);
        http_free(resp);
        return E_NOMEM;
    }
    memcpy(resp->headers, buf, header_len);
    resp->headers[header_len] = 0;
    if (!(req->flags & HTTP_NO_COOKIES))
        cookies_from(&u, resp->headers);
    memcpy(resp->body, buf + header_len, body_len);
    resp->body[body_len] = 0;
    resp->length = stream ? streamed : body_len;
    kfree(buf);
    return 0;
}

int http_fetch(const struct http_request *req, struct http_response *resp)
{
    memset(resp, 0, sizeof(*resp));
    str_copy(resp->url, req->url, sizeof(resp->url));
    struct http_request r = *req;
    for (int depth = 0;; depth++) {
        int err = fetch_once(&r, resp->url, resp);
        if (err)
            return err;
        int s = resp->status;
        char location[HTTP_URL_MAX];
        if ((req->flags & HTTP_NO_REDIRECT) || depth >= 8 || (s != 301 && s != 302 && s != 303 && s != 307 && s != 308) ||
            !http_header(resp->headers, "Location", location, sizeof(location)))
            return 0;
        char next[HTTP_URL_MAX];
        if (!http_resolve_url(resp->url, location, next, sizeof(next)))
            return 0;
        http_free(resp);
        str_copy(resp->url, next, sizeof(resp->url));
        if (s != 307 && s != 308) {
            r.method = NULL;
            r.body = NULL;
            r.body_len = 0;
        }
    }
}

void http_free(struct http_response *resp)
{
    kfree(resp->headers);
    kfree(resp->body);
    resp->headers = resp->body = NULL;
    resp->length = 0;
}
