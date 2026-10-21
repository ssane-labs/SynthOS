#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/string.h>
#include <synth/tls.h>

#include "win32.h"

#define MAX_HANDLES 64
#define HANDLE_BASE 0x4e10u
#define FLAG_SECURE 0x00800000u
#define FLAG_IGNORE_CERT 0x00003000u
#define QUERY_NUMBER 0x20000000u

enum { ERROR_TIMEOUT = 12002, ERROR_NAME_NOT_RESOLVED = 12007, ERROR_CANNOT_CONNECT = 12029,
       ERROR_CONNECTION_RESET = 12031, ERROR_INVALID_CA = 12045, ERROR_CERT_CN = 12038, ERROR_CERT_DATE = 12037,
       ERROR_SECURE_FAILURE = 12175, ERROR_INCORRECT_HANDLE = 12018, ERROR_HEADER_NOT_FOUND = 12150 };

enum kind { K_FREE, K_SESSION, K_CONNECT, K_REQUEST };

struct inet {
    enum kind kind;
    struct inet *parent;
    char host[256];
    uint16_t port;
    bool https;
    unsigned flags;
    char verb[16];
    char *url;
    char *headers;
    uint8_t *body;
    size_t body_len;
    struct http_response resp;
    bool done;
    size_t pos;
};

static W32_STATE struct inet *handles[MAX_HANDLES];

static HANDLE new_handle(enum kind kind, struct inet *parent)
{
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (handles[i])
            continue;
        struct inet *h = w32_zalloc(sizeof(*h));
        if (!h)
            break;
        h->kind = kind;
        h->parent = parent;
        if (parent) {
            str_copy(h->host, parent->host, sizeof(h->host));
            h->port = parent->port;
            h->https = parent->https;
            h->flags = parent->flags;
        }
        handles[i] = h;
        return (HANDLE)(uintptr_t)(HANDLE_BASE + i * 4);
    }
    w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static struct inet *get(HANDLE handle)
{
    uintptr_t v = (uintptr_t)handle;
    if (v < HANDLE_BASE || (v - HANDLE_BASE) % 4 || (v - HANDLE_BASE) / 4 >= MAX_HANDLES ||
        !handles[(v - HANDLE_BASE) / 4]) {
        w32_set_error(ERROR_INCORRECT_HANDLE);
        return NULL;
    }
    return handles[(v - HANDLE_BASE) / 4];
}

static void release(struct inet *h)
{
    http_free(&h->resp);
    w32_free(h->url);
    w32_free(h->headers);
    w32_free(h->body);
    w32_free(h);
}

static BOOL WINAPI InternetCloseHandle(HANDLE handle)
{
    struct inet *h = get(handle);
    if (!h)
        return FALSE;
    handles[((uintptr_t)handle - HANDLE_BASE) / 4] = NULL;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (handles[i] && handles[i]->parent == h)
            handles[i]->parent = NULL;
    release(h);
    return TRUE;
}

void w32_inet_cleanup(void)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (handles[i]) {
            release(handles[i]);
            handles[i] = NULL;
        }
}

static void add_headers(struct inet *h, const char *text, long len)
{
    if (!text)
        return;
    size_t n = len < 0 ? strlen(text) : (size_t)len;
    size_t old = h->headers ? strlen(h->headers) : 0;
    char *grown = w32_realloc(h->headers, old + n + 3);
    if (!grown)
        return;
    memcpy(grown + old, text, n);
    grown[old + n] = 0;
    if (n < 2 || grown[old + n - 1] != '\n')
        str_append(grown, "\r\n", old + n + 3);
    h->headers = grown;
}

static char *narrow_n(const WCHAR *w, long len)
{
    if (!w)
        return NULL;
    if (len < 0)
        return w32_narrow(w);
    WCHAR *copy = w32_alloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!copy)
        return NULL;
    memcpy(copy, w, (size_t)len * sizeof(WCHAR));
    copy[len] = 0;
    char *s = w32_narrow(copy);
    w32_free(copy);
    return s;
}

static DWORD map_error(int err)
{
    switch (err) {
    case E_NOENT: return ERROR_NAME_NOT_RESOLVED;
    case E_TIMEDOUT: return ERROR_TIMEOUT;
    case E_CONNREFUSED: case E_UNREACH: return ERROR_CANNOT_CONNECT;
    case E_CONNRESET: case E_PIPE: return ERROR_CONNECTION_RESET;
    case TLS_ERR_CERT_UNTRUSTED: return ERROR_INVALID_CA;
    case TLS_ERR_CERT_NAME: return ERROR_CERT_CN;
    case TLS_ERR_CERT_EXPIRED: return ERROR_CERT_DATE;
    default: return err <= TLS_ERR_PROTOCOL ? ERROR_SECURE_FAILURE : ERROR_CANNOT_CONNECT;
    }
}

static BOOL perform(struct inet *h)
{
    if (h->done)
        return TRUE;
    struct http_request req = {
        .method = h->verb[0] ? h->verb : NULL,
        .url = h->url,
        .headers = h->headers,
        .body = h->body,
        .body_len = h->body_len,
        .flags = (h->flags & FLAG_IGNORE_CERT) ? HTTP_INSECURE : 0,
    };
    int err = http_fetch(&req, &h->resp);
    if (err) {
        w32_set_error(map_error(err));
        return FALSE;
    }
    h->done = true;
    h->pos = 0;
    return TRUE;
}

static void set_url(struct inet *h, const char *object)
{
    char url[HTTP_URL_MAX];
    if (h->port == (h->https ? 443 : 80))
        ksnprintf(url, sizeof(url), "%s://%s%s%s", h->https ? "https" : "http", h->host, object[0] == '/' ? "" : "/",
                  object);
    else
        ksnprintf(url, sizeof(url), "%s://%s:%u%s%s", h->https ? "https" : "http", h->host, h->port,
                  object[0] == '/' ? "" : "/", object);
    h->url = w32_strdup(url);
}

static HANDLE WINAPI InternetOpenA(LPCSTR agent, DWORD access, LPCSTR proxy, LPCSTR bypass, DWORD flags)
{
    UNUSED(agent);
    UNUSED(access);
    UNUSED(proxy);
    UNUSED(bypass);
    UNUSED(flags);
    return new_handle(K_SESSION, NULL);
}

static HANDLE WINAPI InternetOpenW(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
    return InternetOpenA(NULL, access, NULL, NULL, flags);
}

static HANDLE connect_to(HANDLE session, const char *server, uint32_t port, DWORD flags)
{
    struct inet *s = get(session);
    if (!s)
        return NULL;
    HANDLE h = new_handle(K_CONNECT, s);
    struct inet *c = h ? get(h) : NULL;
    if (!c)
        return NULL;
    str_copy(c->host, server, sizeof(c->host));
    c->https = port == 443;
    c->port = (uint16_t)(port ? port : 80);
    c->flags = flags;
    return h;
}

static HANDLE WINAPI InternetConnectA(HANDLE session, LPCSTR server, DWORD port, LPCSTR user, LPCSTR password,
                                      DWORD service, DWORD flags, uintptr_t ctx)
{
    UNUSED(user);
    UNUSED(password);
    UNUSED(service);
    UNUSED(ctx);
    return connect_to(session, server, port, flags);
}

static HANDLE WINAPI InternetConnectW(HANDLE session, LPCWSTR server, DWORD port, LPCWSTR user, LPCWSTR password,
                                      DWORD service, DWORD flags, uintptr_t ctx)
{
    char *s = w32_narrow(server);
    HANDLE h = InternetConnectA(session, s, port, NULL, NULL, service, flags, ctx);
    w32_free(s);
    return h;
}

static HANDLE open_request(HANDLE connect, const char *verb, const char *object, DWORD flags)
{
    struct inet *c = get(connect);
    if (!c)
        return NULL;
    HANDLE h = new_handle(K_REQUEST, c);
    struct inet *r = h ? get(h) : NULL;
    if (!r)
        return NULL;
    str_copy(r->verb, verb ? verb : "GET", sizeof(r->verb));
    r->flags |= flags;
    if (flags & FLAG_SECURE) {
        r->https = true;
        if (r->port == 80)
            r->port = 443;
    }
    set_url(r, object && *object ? object : "/");
    return h;
}

static HANDLE WINAPI HttpOpenRequestA(HANDLE connect, LPCSTR verb, LPCSTR object, LPCSTR version, LPCSTR referrer,
                                      LPCSTR *accept, DWORD flags, uintptr_t ctx)
{
    UNUSED(version);
    UNUSED(referrer);
    UNUSED(accept);
    UNUSED(ctx);
    return open_request(connect, verb, object, flags);
}

static HANDLE WINAPI HttpOpenRequestW(HANDLE connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                                      LPCWSTR referrer, LPCWSTR *accept, DWORD flags, uintptr_t ctx)
{
    char *v = verb ? w32_narrow(verb) : NULL, *o = object ? w32_narrow(object) : NULL;
    HANDLE h = HttpOpenRequestA(connect, v, o, NULL, NULL, NULL, flags, ctx);
    w32_free(v);
    w32_free(o);
    return h;
}

static BOOL send_request(HANDLE request, const char *headers, long len, const void *body, DWORD body_len)
{
    struct inet *r = get(request);
    if (!r || r->kind != K_REQUEST)
        return FALSE;
    add_headers(r, headers, len);
    if (body && body_len) {
        r->body = w32_alloc(body_len);
        if (r->body) {
            memcpy(r->body, body, body_len);
            r->body_len = body_len;
        }
    }
    return perform(r);
}

static BOOL WINAPI HttpSendRequestA(HANDLE request, LPCSTR headers, DWORD len, const void *body, DWORD body_len)
{
    return send_request(request, headers, len == 0xffffffffu ? -1 : (long)len, body, body_len);
}

static BOOL WINAPI HttpSendRequestW(HANDLE request, LPCWSTR headers, DWORD len, const void *body, DWORD body_len)
{
    char *h = narrow_n(headers, len == 0xffffffffu ? -1 : (long)len);
    BOOL ok = send_request(request, h, -1, body, body_len);
    w32_free(h);
    return ok;
}

static BOOL WINAPI HttpAddRequestHeadersA(HANDLE request, LPCSTR headers, DWORD len, DWORD modifiers)
{
    UNUSED(modifiers);
    struct inet *r = get(request);
    if (!r)
        return FALSE;
    add_headers(r, headers, len == 0xffffffffu ? -1 : (long)len);
    return TRUE;
}

static BOOL WINAPI HttpAddRequestHeadersW(HANDLE request, LPCWSTR headers, DWORD len, DWORD modifiers)
{
    char *h = narrow_n(headers, len == 0xffffffffu ? -1 : (long)len);
    BOOL ok = HttpAddRequestHeadersA(request, h, 0xffffffffu, modifiers);
    w32_free(h);
    return ok;
}

static HANDLE open_url(HANDLE session, const char *url, const char *headers, long len, DWORD flags)
{
    struct inet *s = get(session);
    if (!s)
        return NULL;
    HANDLE h = new_handle(K_REQUEST, s);
    struct inet *r = h ? get(h) : NULL;
    if (!r)
        return NULL;
    r->flags = flags;
    r->url = w32_strdup(url);
    add_headers(r, headers, len);
    if (!perform(r)) {
        DWORD err = w32_error();
        InternetCloseHandle(h);
        w32_set_error(err);
        return NULL;
    }
    return h;
}

static HANDLE WINAPI InternetOpenUrlA(HANDLE session, LPCSTR url, LPCSTR headers, DWORD len, DWORD flags,
                                      uintptr_t ctx)
{
    UNUSED(ctx);
    return open_url(session, url, headers, len == 0xffffffffu ? -1 : (long)len, flags);
}

static HANDLE WINAPI InternetOpenUrlW(HANDLE session, LPCWSTR url, LPCWSTR headers, DWORD len, DWORD flags,
                                      uintptr_t ctx)
{
    UNUSED(ctx);
    char *u = w32_narrow(url), *h = narrow_n(headers, len == 0xffffffffu ? -1 : (long)len);
    HANDLE r = open_url(session, u, h, -1, flags);
    w32_free(u);
    w32_free(h);
    return r;
}

static BOOL WINAPI InternetReadFile(HANDLE request, void *buf, DWORD n, DWORD *read)
{
    struct inet *r = get(request);
    if (read)
        *read = 0;
    if (!r || !perform(r))
        return FALSE;
    size_t k = MIN((size_t)n, r->resp.length - r->pos);
    memcpy(buf, r->resp.body + r->pos, k);
    r->pos += k;
    if (read)
        *read = (DWORD)k;
    return TRUE;
}

static BOOL WINAPI InternetQueryDataAvailable(HANDLE request, DWORD *avail, DWORD flags, uintptr_t ctx)
{
    UNUSED(flags);
    UNUSED(ctx);
    struct inet *r = get(request);
    if (!r || !perform(r))
        return FALSE;
    if (avail)
        *avail = (DWORD)(r->resp.length - r->pos);
    return TRUE;
}

static bool query(struct inet *r, DWORD level, char *text, size_t cap, DWORD *number)
{
    const char *names[] = {[1] = "Content-Type", [5] = "Content-Length", [9] = "Date", [33] = "Location",
                           [11] = "Expires", [44] = "Last-Modified", [3] = "Content-Encoding"};
    level &= 0xffff;
    if (level == 19) {
        ksnprintf(text, cap, "%d", r->resp.status);
    } else if (level == 18) {
        str_copy(text, "HTTP/1.1", cap);
    } else if (level == 20) {
        const char *p = strchr(r->resp.headers, ' ');
        p = p ? strchr(p + 1, ' ') : NULL;
        size_t k = 0;
        while (p && p[1 + k] && p[1 + k] != '\r')
            k++;
        ksnprintf(text, cap, "%.*s", (int)k, p ? p + 1 : "");
    } else if (level == 22 || level == 21) {
        str_copy(text, r->resp.headers, cap);
    } else if (level < ARRAY_SIZE(names) && names[level]) {
        if (!http_header(r->resp.headers, names[level], text, cap))
            return false;
    } else {
        return false;
    }
    *number = (DWORD)str_to_long(text, NULL, 10);
    return true;
}

static BOOL query_out(struct inet *r, DWORD level, void *buf, DWORD *len, bool wide)
{
    char text[4096];
    DWORD number;
    if (!r || !perform(r))
        return FALSE;
    if (!query(r, level, text, sizeof(text), &number)) {
        w32_set_error(ERROR_HEADER_NOT_FOUND);
        return FALSE;
    }
    if (level & QUERY_NUMBER) {
        if (*len < 4) {
            *len = 4;
            w32_set_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(buf, &number, 4);
        *len = 4;
        return TRUE;
    }
    size_t n = strlen(text), unit = wide ? 2 : 1;
    if (!buf || *len < (n + 1) * unit) {
        *len = (DWORD)((n + 1) * unit);
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (wide)
        for (size_t i = 0; i <= n; i++)
            ((WCHAR *)buf)[i] = (uint8_t)text[i];
    else
        memcpy(buf, text, n + 1);
    *len = (DWORD)(n * unit);
    return TRUE;
}

static BOOL WINAPI HttpQueryInfoA(HANDLE request, DWORD level, void *buf, DWORD *len, DWORD *index)
{
    UNUSED(index);
    return query_out(get(request), level, buf, len, false);
}

static BOOL WINAPI HttpQueryInfoW(HANDLE request, DWORD level, void *buf, DWORD *len, DWORD *index)
{
    UNUSED(index);
    return query_out(get(request), level, buf, len, true);
}

static BOOL WINAPI InternetSetOptionA(HANDLE h, DWORD option, void *buf, DWORD len)
{
    UNUSED(option);
    UNUSED(buf);
    UNUSED(len);
    struct inet *r = h ? get(h) : NULL;
    if (r && option == 31 && buf && len >= 4)
        r->flags |= *(DWORD *)buf & 0x100 ? FLAG_IGNORE_CERT : 0;
    return TRUE;
}

static BOOL WINAPI InternetQueryOptionA(HANDLE h, DWORD option, void *buf, DWORD *len)
{
    UNUSED(h);
    UNUSED(option);
    if (buf && len && *len >= 4) {
        memset(buf, 0, 4);
        *len = 4;
    }
    return TRUE;
}

static BOOL WINAPI InternetGetConnectedState(DWORD *flags, DWORD reserved)
{
    UNUSED(reserved);
    if (flags)
        *flags = 0x2;
    return TRUE;
}

static void *WINAPI InternetSetStatusCallback(HANDLE h, void *fn)
{
    UNUSED(h);
    UNUSED(fn);
    return NULL;
}

static BOOL WINAPI InternetCheckConnectionA(LPCSTR url, DWORD flags, DWORD reserved)
{
    UNUSED(url);
    UNUSED(flags);
    UNUSED(reserved);
    return TRUE;
}

struct url_components;
static BOOL WINAPI WinHttpCrackUrl(const WCHAR *url, DWORD len, DWORD flags, struct url_components *uc);

/* the same URL_COMPONENTS as WinHTTP; only the scheme numbers differ (INTERNET_SCHEME_HTTP 3, HTTPS 4) */
static BOOL WINAPI InternetCrackUrlW(const WCHAR *url, DWORD len, DWORD flags, uint32_t *uc)
{
    if (!WinHttpCrackUrl(url, len, flags, (struct url_components *)uc))
        return FALSE;
    uc[3] = uc[3] == 2 ? 4 : 3;
    return TRUE;
}

static const struct w32_export wininet_exports[] = {
    W32_FN(InternetCrackUrlW),
    W32_AS("GetUrlCacheEntryInfoExW", w32_fail7),
    W32_FN(InternetOpenA), W32_FN(InternetOpenW), W32_FN(InternetConnectA), W32_FN(InternetConnectW),
    W32_FN(HttpOpenRequestA), W32_FN(HttpOpenRequestW), W32_FN(HttpSendRequestA), W32_FN(HttpSendRequestW),
    W32_FN(HttpAddRequestHeadersA), W32_FN(HttpAddRequestHeadersW), W32_FN(InternetOpenUrlA),
    W32_FN(InternetOpenUrlW), W32_FN(InternetReadFile), W32_FN(InternetQueryDataAvailable), W32_FN(HttpQueryInfoA),
    W32_FN(HttpQueryInfoW), W32_FN(InternetCloseHandle), W32_FN(InternetSetOptionA),
    W32_AS("InternetSetOptionW", InternetSetOptionA), W32_FN(InternetQueryOptionA),
    W32_AS("InternetQueryOptionW", InternetQueryOptionA), W32_FN(InternetGetConnectedState),
    W32_FN(InternetSetStatusCallback), W32_AS("InternetSetStatusCallbackA", InternetSetStatusCallback),
    W32_AS("InternetSetStatusCallbackW", InternetSetStatusCallback), W32_FN(InternetCheckConnectionA),
    W32_AS("InternetCheckConnectionW", InternetCheckConnectionA),
};
W32_EXPORTS(wininet_module, "wininet.dll", wininet_exports);

static HANDLE WINAPI WinHttpOpen(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
    return InternetOpenW(agent, access, proxy, bypass, flags);
}

static HANDLE WINAPI WinHttpConnect(HANDLE session, LPCWSTR server, uint32_t port, DWORD reserved)
{
    UNUSED(reserved);
    char *s = w32_narrow(server);
    HANDLE h = connect_to(session, s, port & 0xffff, 0);
    w32_free(s);
    return h;
}

static HANDLE WINAPI WinHttpOpenRequest(HANDLE connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                                        LPCWSTR referrer, LPCWSTR *accept, DWORD flags)
{
    return HttpOpenRequestW(connect, verb, object, version, referrer, accept, flags, 0);
}

static BOOL WINAPI WinHttpSendRequest(HANDLE request, LPCWSTR headers, DWORD len, const void *body, DWORD body_len,
                                      DWORD total, uintptr_t ctx)
{
    UNUSED(total);
    UNUSED(ctx);
    struct inet *r = get(request);
    if (!r)
        return FALSE;
    char *h = narrow_n(headers, len == 0xffffffffu ? -1 : (long)len);
    add_headers(r, h, -1);
    w32_free(h);
    if (body && body_len) {
        r->body = w32_alloc(body_len);
        if (r->body) {
            memcpy(r->body, body, body_len);
            r->body_len = body_len;
        }
    }
    return TRUE;
}

static BOOL WINAPI WinHttpWriteData(HANDLE request, const void *buf, DWORD len, DWORD *written)
{
    struct inet *r = get(request);
    if (!r)
        return FALSE;
    uint8_t *grown = len ? w32_realloc(r->body, r->body_len + len) : r->body;
    if (len && !grown) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (len)
        memcpy(grown + r->body_len, buf, len);
    r->body = grown;
    r->body_len += len;
    if (written)
        *written = len;
    return TRUE;
}

struct url_part {
    WCHAR *text;
    DWORD len;
};

struct url_components {
    DWORD size;
    struct url_part scheme;
    int nscheme;
    struct url_part host;
    uint16_t port, pad;
    struct url_part user, password, path, extra;
};

static bool url_part(struct url_part *p, const WCHAR *start, DWORD len)
{
    if (!p->len && !p->text)
        return true;
    if (!p->text) {
        p->text = (WCHAR *)start;
        p->len = len;
        return true;
    }
    if (p->len <= len) {
        p->len = len + 1;
        return false;
    }
    memcpy(p->text, start, len * sizeof(WCHAR));
    p->text[len] = 0;
    p->len = len;
    return true;
}

static BOOL WINAPI WinHttpCrackUrl(const WCHAR *url, DWORD len, DWORD flags, struct url_components *uc)
{
    UNUSED(flags);
    if (!url || !uc) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!len)
        while (url[len])
            len++;
    const WCHAR *end = url + len, *p = url;
    while (p < end && *p != ':')
        p++;
    if (p + 2 >= end || p[1] != '/' || p[2] != '/') {
        w32_set_error(12006);
        return FALSE;
    }
    DWORD scheme_len = (DWORD)(p - url);
    bool https = scheme_len == 5 && (url[4] | 0x20) == 's';
    const WCHAR *auth = p + 3, *q = auth;
    while (q < end && *q != '/' && *q != '?' && *q != '#')
        q++;
    const WCHAR *host = auth, *at = NULL;
    for (const WCHAR *c = auth; c < q; c++)
        if (*c == '@')
            at = c;
    const WCHAR *user_end = NULL;
    if (at) {
        host = at + 1;
        user_end = auth;
        while (user_end < at && *user_end != ':')
            user_end++;
    }
    const WCHAR *colon = NULL;
    for (const WCHAR *c = host; c < q; c++)
        if (*c == ':')
            colon = c;
    unsigned port = https ? 443 : 80;
    if (colon) {
        port = 0;
        for (const WCHAR *c = colon + 1; c < q; c++)
            port = port * 10 + (unsigned)(*c - '0');
    }
    const WCHAR *extra = q;
    while (extra < end && *extra != '?' && *extra != '#')
        extra++;
    bool ok = url_part(&uc->scheme, url, scheme_len);
    ok &= url_part(&uc->host, host, (DWORD)((colon ? colon : q) - host));
    ok &= url_part(&uc->user, auth, at ? (DWORD)(user_end - auth) : 0);
    ok &= url_part(&uc->password, at && user_end < at ? user_end + 1 : auth,
                   at && user_end < at ? (DWORD)(at - user_end - 1) : 0);
    ok &= url_part(&uc->path, q, (DWORD)(extra - q));
    ok &= url_part(&uc->extra, extra, (DWORD)(end - extra));
    uc->nscheme = https ? 2 : 1;
    uc->port = (uint16_t)port;
    if (!ok)
        w32_set_error(ERROR_INSUFFICIENT_BUFFER);
    return ok;
}

static BOOL WINAPI WinHttpReceiveResponse(HANDLE request, void *reserved)
{
    UNUSED(reserved);
    struct inet *r = get(request);
    return r && perform(r);
}

static BOOL WINAPI WinHttpQueryHeaders(HANDLE request, DWORD level, LPCWSTR name, void *buf, DWORD *len,
                                       DWORD *index)
{
    UNUSED(name);
    UNUSED(index);
    return query_out(get(request), level, buf, len, true);
}

static BOOL WINAPI WinHttpAddRequestHeaders(HANDLE request, LPCWSTR headers, DWORD len, DWORD modifiers)
{
    return HttpAddRequestHeadersW(request, headers, len, modifiers);
}

static BOOL WINAPI WinHttpSetOption(HANDLE h, DWORD option, void *buf, DWORD len)
{
    struct inet *r = h ? get(h) : NULL;
    if (r && option == 31 && buf && len >= 4)
        r->flags |= *(DWORD *)buf ? FLAG_IGNORE_CERT : 0;
    return TRUE;
}

static BOOL WINAPI WinHttpSetTimeouts(HANDLE h, int resolve, int connect, int send, int receive)
{
    UNUSED(h);
    UNUSED(resolve);
    UNUSED(connect);
    UNUSED(send);
    UNUSED(receive);
    return TRUE;
}

static BOOL WINAPI WinHttpQueryDataAvailable(HANDLE request, DWORD *avail)
{
    return InternetQueryDataAvailable(request, avail, 0, 0);
}

static void *WINAPI WinHttpSetStatusCallback(HANDLE h, void *fn, DWORD flags, uintptr_t reserved)
{
    UNUSED(flags);
    UNUSED(reserved);
    return InternetSetStatusCallback(h, fn);
}

static BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(void *config)
{
    if (config)
        memset(config, 0, 16);
    return TRUE;
}

static const struct w32_export winhttp_exports[] = {
    W32_AS("WinHttpCreateProxyResolver", w32_one2), W32_AS("WinHttpFreeProxyResult", w32_zero1),
    W32_AS("WinHttpGetProxyForUrl", w32_fail4), W32_AS("WinHttpGetProxyForUrlEx", w32_one4),
    W32_AS("WinHttpGetProxyResult", w32_one2),
    W32_FN(WinHttpOpen), W32_FN(WinHttpConnect), W32_FN(WinHttpOpenRequest), W32_FN(WinHttpSendRequest),
    W32_FN(WinHttpReceiveResponse), W32_FN(WinHttpQueryHeaders), W32_FN(WinHttpAddRequestHeaders),
    W32_FN(WinHttpQueryDataAvailable), W32_AS("WinHttpReadData", InternetReadFile),
    W32_AS("WinHttpCloseHandle", InternetCloseHandle), W32_FN(WinHttpSetOption), W32_FN(WinHttpSetTimeouts),
    W32_FN(WinHttpGetIEProxyConfigForCurrentUser), W32_FN(WinHttpSetStatusCallback), W32_FN(WinHttpWriteData),
    W32_FN(WinHttpCrackUrl),
};
W32_EXPORTS(winhttp_module, "winhttp.dll", winhttp_exports);

static int download(const char *url, const char *file)
{
    struct http_request req = {.url = url};
    struct http_response resp;
    if (http_fetch(&req, &resp) != 0)
        return -1;
    int rc = -1;
    char path[PATH_MAX_LEN];
    if (resp.status < 400 && w32_path(file, path, sizeof(path)) == 0 &&
        vfs_write_all(path, resp.body, resp.length) == 0)
        rc = 0;
    http_free(&resp);
    return rc;
}

static uint32_t WINAPI URLDownloadToFileA(void *caller, LPCSTR url, LPCSTR file, DWORD reserved, void *callback)
{
    UNUSED(caller);
    UNUSED(reserved);
    UNUSED(callback);
    return download(url, file) == 0 ? 0 : 0x800c0005u;
}

static uint32_t WINAPI URLDownloadToFileW(void *caller, LPCWSTR url, LPCWSTR file, DWORD reserved, void *callback)
{
    char *u = w32_narrow(url), *f = w32_narrow(file);
    uint32_t r = URLDownloadToFileA(caller, u, f, reserved, callback);
    w32_free(u);
    w32_free(f);
    return r;
}

static const struct w32_export urlmon_exports[] = {
    W32_AS("CoInternetCreateSecurityManager", w32_notimpl3),
    W32_AS("CreateURLMonikerEx", w32_notimpl4),
    W32_FN(URLDownloadToFileA),
    W32_FN(URLDownloadToFileW),
};
W32_EXPORTS(urlmon_module, "urlmon.dll", urlmon_exports);
