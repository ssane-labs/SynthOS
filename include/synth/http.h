#ifndef SYNTH_HTTP_H
#define SYNTH_HTTP_H

#include <synth/types.h>

#define HTTP_URL_MAX 1024

enum { HTTP_INSECURE = 1, HTTP_NO_REDIRECT = 2, HTTP_HEAD = 4, HTTP_NO_COOKIES = 8 };

struct http_url {
    bool https;
    char host[256];
    uint16_t port;
    char path[HTTP_URL_MAX];
};

struct http_request {
    const char *method;
    const char *url;
    const char *headers;
    const void *body;
    size_t body_len;
    unsigned flags;
    size_t max_size;
    bool (*cancelled)(void);
    void (*progress)(void *ctx, const char *stage, size_t done, size_t total);
    void *ctx;
    /* When set, the body of a successful response goes here in pieces instead of resp->body. */
    int (*sink)(void *ctx, const void *data, size_t n);
};

struct http_response {
    int status;
    char *headers;
    char *body;
    size_t length;
    char url[HTTP_URL_MAX];
    char tls[96];
};

bool http_parse_url(const char *url, struct http_url *u);
bool http_resolve_url(const char *base, const char *ref, char *out, size_t cap);
int http_fetch(const struct http_request *req, struct http_response *resp);
bool http_header(const char *headers, const char *name, char *out, size_t cap);
void http_free(struct http_response *resp);
void http_cookies_clear(void);
int http_cookie_count(void);

#endif
