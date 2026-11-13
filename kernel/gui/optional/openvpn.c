/* An OpenVPN client, for the .ovpn files VPN services give.
 *
 * `openvpn up FILE.ovpn [USER PASSWORD]` connects over UDP or TCP; the control channel (TLS 1.2/1.3 with the
 * file's own <ca>, wrapped by <tls-auth> or <tls-crypt> when the file has one) authenticates with the user name
 * and password, the server pushes the tunnel's address, routes and DNS, and the data channel carries IP packets
 * sealed with AES-GCM or ChaCha20-Poly1305 (keys from the OpenVPN PRF). While it is up it is the system's VPN:
 * the routes it pushes (redirect-gateway: everything) go through it, DNS goes to its server, IPv6 too when the
 * server gives an IPv6 address. Client certificates and the old CBC ciphers are not supported. */
#include <synth/command.h>
#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/net.h>
#include <synth/sha1.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/tls.h>
#include <synth/vfs.h>
#include <synth/vpn.h>

#include "../internal.h"

enum {
    P_CONTROL_SOFT_RESET_V1 = 3,
    P_CONTROL_V1 = 4,
    P_ACK_V1 = 5,
    P_DATA_V1 = 6,
    P_CONTROL_HARD_RESET_CLIENT_V2 = 7,
    P_CONTROL_HARD_RESET_SERVER_V2 = 8,
    P_DATA_V2 = 9,
};

#define CONTROL_CHUNK 1100
#define SENT_MAX 32
#define RX_SLOTS 16
#define MAX_ROUTES 16
#define RETRANSMIT_MS 1500
#define TUN_MTU 1400

static const uint8_t ping_magic[16] = {0x2a, 0x18, 0x7b, 0xf3, 0x64, 0x1e, 0xb4, 0xcb,
                                       0x07, 0xed, 0x2d, 0x0a, 0x98, 0x1f, 0xc7, 0x48};

enum wrap { WRAP_NONE, WRAP_AUTH, WRAP_CRYPT };
enum cipher { C_AES128, C_AES256, C_CHACHA };

struct ovpn_config {
    char name[64];
    char host[128];
    uint16_t port;
    bool tcp;
    char *ca;
    size_t ca_len;
    enum wrap wrap;
    uint8_t static_key[256];
    int key_direction; /* -1: none given */
    const struct hash_alg *auth;
    char verify_name[96];
};

struct sent {
    uint32_t id;
    uint8_t *packet;
    size_t len;
    uint64_t at;
    int tries;
};

struct route {
    uint32_t net, mask;
};

struct route6 {
    uint8_t net[16];
    int bits;
};

static struct ovpn {
    bool connected, lost;
    struct ovpn_config cfg;
    int sock;
    uint32_t server;
    uint8_t sid[8], peer_sid[8];
    bool have_peer;
    /* the control channel */
    uint32_t next_msg, expect_msg;
    struct sent sent[SENT_MAX];
    int nsent;
    uint32_t acks[8];
    int nacks;
    struct {
        bool used;
        uint32_t id;
        uint8_t *data;
        size_t len;
    } slots[RX_SLOTS];
    uint8_t *stream; /* TLS bytes in order, not read yet */
    size_t stream_len, stream_cap;
    uint8_t tcp_buf[4096];
    size_t tcp_len;
    /* tls-auth / tls-crypt */
    uint8_t wrap_out_hmac[64], wrap_in_hmac[64], wrap_out_cipher[32], wrap_in_cipher[32];
    uint32_t wrap_pid;
    struct tls *tls;
    /* the data channel */
    enum cipher cipher;
    struct gcm gcm_out, gcm_in;
    uint8_t key_out[32], key_in[32], iv_out[8], iv_in[8];
    uint32_t data_pid, recv_max;
    uint64_t recv_bitmap;
    int peer_id;
    uint64_t last_rx, last_tx, up_at;
    uint64_t rx_bytes, tx_bytes;
    int ping_every, ping_restart;
    /* what the server pushed */
    uint32_t addr, mask, dns, gateway;
    struct route routes[MAX_ROUTES];
    int nroutes;
    bool redirect, block6;
    uint8_t addr6[16];
    bool has6;
    struct route6 routes6[4];
    int nroutes6;
    char error[120];
} *ov;

static char last_error[120];

/* ---- the .ovpn file ---- */

static char *inline_block(const char *text, const char *tag, size_t *len)
{
    char open[32], close[32];
    ksnprintf(open, sizeof(open), "<%s>", tag);
    ksnprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(text, open), *b = a ? strstr(a, close) : NULL;
    if (!a || !b)
        return NULL;
    a += strlen(open);
    *len = (size_t)(b - a);
    char *out = kmalloc(*len + 1);
    if (out) {
        memcpy(out, a, *len);
        out[*len] = 0;
    }
    return out;
}

static int hexval(int c)
{
    return is_digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "-----BEGIN OpenVPN Static key V1-----" and 256 bytes of hex */
static bool parse_static_key(const char *text, uint8_t out[256])
{
    const char *p = strstr(text, "-----BEGIN OpenVPN Static key V1-----");
    if (!p)
        return false;
    p = strchr(p, '\n');
    int n = 0, hi = -1;
    for (; p && *p && n < 256; p++) {
        if (*p == '-')
            break;
        int v = hexval(*p);
        if (v < 0)
            continue;
        if (hi < 0) {
            hi = v;
        } else {
            out[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return n == 256;
}

static int parse_ovpn(const char *text, struct ovpn_config *c, char *why, size_t cap)
{
    memset(c, 0, sizeof(*c));
    c->port = 1194;
    c->key_direction = -1;
    c->auth = &hash_sha1;
    bool remote = false;
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char line[512];
        str_copy(line, p, MIN(n + 1, sizeof(line)));
        p += n + (end ? 1 : 0);
        char *l = line;
        while (*l == ' ' || *l == '\t')
            l++;
        size_t ll = strlen(l);
        while (ll && (l[ll - 1] == '\r' || l[ll - 1] == ' '))
            l[--ll] = 0;
        if (!*l || *l == '#' || *l == ';')
            continue;
        if (*l == '<') { /* skip an inline block; read below */
            char close[40];
            char *gt = strchr(l, '>');
            if (gt && l[1] != '/') {
                *gt = 0;
                ksnprintf(close, sizeof(close), "</%s>", l + 1);
                const char *e = strstr(p, close);
                if (e) {
                    p = strchr(e, '\n');
                    if (!p)
                        p = e + strlen(e);
                }
            }
            continue;
        }
        char *argv[6];
        int argc = 0;
        for (char *t = l; *t && argc < 6;) {
            while (*t == ' ' || *t == '\t')
                *t++ = 0;
            if (!*t)
                break;
            argv[argc++] = t;
            while (*t && *t != ' ' && *t != '\t')
                t++;
        }
        if (!argc)
            continue;
        if (!strcmp(argv[0], "remote") && argc >= 2 && !remote) {
            str_copy(c->host, argv[1], sizeof(c->host));
            if (argc >= 3)
                c->port = (uint16_t)str_to_long(argv[2], NULL, 10);
            if (argc >= 4)
                c->tcp = !strncmp(argv[3], "tcp", 3);
            remote = true;
        } else if (!strcmp(argv[0], "port") && argc >= 2) {
            c->port = (uint16_t)str_to_long(argv[1], NULL, 10);
        } else if (!strcmp(argv[0], "proto") && argc >= 2) {
            c->tcp = !strncmp(argv[1], "tcp", 3);
        } else if (!strcmp(argv[0], "dev") && argc >= 2 && strncmp(argv[1], "tun", 3)) {
            ksnprintf(why, cap, "only 'dev tun' is supported, not %s", argv[1]);
            return E_INVAL;
        } else if (!strcmp(argv[0], "key-direction") && argc >= 2) {
            c->key_direction = (int)str_to_long(argv[1], NULL, 10);
        } else if (!strcmp(argv[0], "auth") && argc >= 2) {
            c->auth = !str_casecmp(argv[1], "SHA256") ? &hash_sha256 : !str_casecmp(argv[1], "SHA512") ? &hash_sha512
                      : !str_casecmp(argv[1], "SHA384") ? &hash_sha384 : &hash_sha1;
        } else if (!strcmp(argv[0], "verify-x509-name") && argc >= 2) {
            str_copy(c->verify_name, argv[1], sizeof(c->verify_name));
        } else if ((!strcmp(argv[0], "cert") || !strcmp(argv[0], "pkcs12")) && argc >= 2) {
            ksnprintf(why, cap, "client certificates are not supported yet");
            return E_NOSYS;
        }
    }
    size_t len;
    char *block;
    if ((block = inline_block(text, "cert", &len))) {
        kfree(block);
        ksnprintf(why, cap, "client certificates are not supported yet");
        return E_NOSYS;
    }
    c->ca = inline_block(text, "ca", &c->ca_len);
    if ((block = inline_block(text, "tls-crypt", &len))) {
        c->wrap = parse_static_key(block, c->static_key) ? WRAP_CRYPT : WRAP_NONE;
        kfree(block);
    } else if ((block = inline_block(text, "tls-auth", &len))) {
        c->wrap = parse_static_key(block, c->static_key) ? WRAP_AUTH : WRAP_NONE;
        kfree(block);
    }
    const char *bad = !c->host[0] ? "no 'remote' line" : !c->ca ? "no <ca> block (the server's certificate authority)"
                    : (strstr(text, "<tls-crypt>") || strstr(text, "<tls-auth>")) && c->wrap == WRAP_NONE
                        ? "the <tls-auth> or <tls-crypt> key is not a valid OpenVPN static key"
                        : NULL;
    if (bad) {
        kfree(c->ca);
        c->ca = NULL;
        str_copy(why, bad, cap);
        return E_INVAL;
    }
    return 0;
}

/* ---- the wire ---- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void transmit(const uint8_t *p, size_t n)
{
    if (ov->cfg.tcp) {
        uint8_t len[2] = {(uint8_t)(n >> 8), (uint8_t)n};
        net_send(ov->sock, len, 2);
        net_send(ov->sock, p, n);
    } else {
        net_sendto(ov->sock, p, n, ov->server, ov->cfg.port);
    }
    ov->last_tx = hal_ticks_ms();
}

static void ctr(const uint8_t key[32], const uint8_t iv[16], uint8_t *p, size_t n)
{
    struct aes a;
    aes_init(&a, key, 32);
    uint8_t counter[16], block[16];
    memcpy(counter, iv, 16);
    for (size_t at = 0; at < n; at += 16) {
        aes_encrypt(&a, counter, block);
        for (size_t k = 0; k < 16 && at + k < n; k++)
            p[at + k] ^= block[k];
        for (int k = 15; k >= 0 && !++counter[k]; k--)
            ;
    }
}

/* A control packet: the opcode and session id, then (wrapped) the acks, the message id and the payload */
static size_t control_packet(uint8_t *out, int op, bool with_id, uint32_t id, const uint8_t *payload, size_t n)
{
    uint8_t body[CONTROL_CHUNK + 64];
    size_t b = 0;
    int nacks = MIN(ov->nacks, 4);
    body[b++] = (uint8_t)nacks;
    for (int i = 0; i < nacks; i++, b += 4)
        put32(body + b, ov->acks[i]);
    if (nacks) {
        memcpy(body + b, ov->peer_sid, 8);
        b += 8;
        memmove(ov->acks, ov->acks + nacks, sizeof(uint32_t) * (size_t)(ov->nacks - nacks));
        ov->nacks -= nacks;
    }
    if (with_id) {
        put32(body + b, id);
        b += 4;
    }
    memcpy(body + b, payload, n);
    b += n;
    size_t o = 0;
    out[o++] = (uint8_t)(op << 3);
    memcpy(out + o, ov->sid, 8);
    o += 8;
    if (ov->cfg.wrap == WRAP_NONE) {
        memcpy(out + o, body, b);
        return o + b;
    }
    uint8_t replay[8];
    put32(replay, ++ov->wrap_pid);
    put32(replay + 4, (uint32_t)time_now());
    if (ov->cfg.wrap == WRAP_AUTH) {
        /* HMAC over the replay fields, the opcode and session id, then the rest */
        size_t hl = ov->cfg.auth->digest;
        struct hmac m;
        hmac_init(&m, ov->cfg.auth, ov->wrap_out_hmac, hl);
        hmac_update(&m, replay, 8);
        hmac_update(&m, out, 9);
        hmac_update(&m, body, b);
        hmac_final(&m, out + o);
        o += hl;
        memcpy(out + o, replay, 8);
        o += 8;
        memcpy(out + o, body, b);
        return o + b;
    }
    /* tls-crypt: HMAC-SHA256 over the header and the plain body, then the body under AES-256-CTR */
    memcpy(out + o, replay, 8);
    o += 8;
    struct hmac m;
    hmac_init(&m, &hash_sha256, ov->wrap_out_hmac, 32);
    hmac_update(&m, out, o);
    hmac_update(&m, body, b);
    hmac_final(&m, out + o);
    ctr(ov->wrap_out_cipher, out + o, body, b);
    o += 32;
    memcpy(out + o, body, b);
    return o + b;
}

/* Undoes the wrapping of a received control packet: the plain body (acks...) or NULL */
static const uint8_t *unwrap(uint8_t *p, size_t n, size_t *body_len)
{
    if (ov->cfg.wrap == WRAP_NONE) {
        *body_len = n - 9;
        return p + 9;
    }
    if (ov->cfg.wrap == WRAP_AUTH) {
        size_t hl = ov->cfg.auth->digest;
        if (n < 9 + hl + 8)
            return NULL;
        uint8_t mac[64];
        struct hmac m;
        hmac_init(&m, ov->cfg.auth, ov->wrap_in_hmac, hl);
        hmac_update(&m, p + 9 + hl, 8);
        hmac_update(&m, p, 9);
        hmac_update(&m, p + 9 + hl + 8, n - 9 - hl - 8);
        hmac_final(&m, mac);
        if (!crypto_equal(mac, p + 9, hl))
            return NULL;
        *body_len = n - 9 - hl - 8;
        return p + 9 + hl + 8;
    }
    if (n < 9 + 8 + 32)
        return NULL;
    uint8_t *body = p + 9 + 8 + 32;
    size_t b = n - 9 - 8 - 32;
    ctr(ov->wrap_in_cipher, p + 17, body, b);
    uint8_t mac[32];
    struct hmac m;
    hmac_init(&m, &hash_sha256, ov->wrap_in_hmac, 32);
    hmac_update(&m, p, 17);
    hmac_update(&m, body, b);
    hmac_final(&m, mac);
    if (!crypto_equal(mac, p + 17, 32))
        return NULL;
    *body_len = b;
    return body;
}

static void send_control(int op, const uint8_t *payload, size_t n)
{
    uint8_t *packet = kmalloc(CONTROL_CHUNK + 200);
    if (!packet)
        return;
    uint32_t id = ov->next_msg++;
    size_t len = control_packet(packet, op, true, id, payload, n);
    transmit(packet, len);
    if (ov->nsent < SENT_MAX) {
        ov->sent[ov->nsent++] = (struct sent){id, packet, len, hal_ticks_ms(), 1};
    } else {
        kfree(packet);
    }
}

static void send_acks(void)
{
    while (ov->nacks && ov->have_peer) {
        uint8_t packet[200];
        transmit(packet, control_packet(packet, P_ACK_V1, false, 0, NULL, 0));
    }
}

static void stream_add(const uint8_t *p, size_t n)
{
    if (ov->stream_len + n > ov->stream_cap) {
        size_t cap = MAX(ov->stream_cap * 2, ov->stream_len + n + 4096);
        uint8_t *grown = krealloc(ov->stream, cap);
        if (!grown)
            return;
        ov->stream = grown;
        ov->stream_cap = cap;
    }
    memcpy(ov->stream + ov->stream_len, p, n);
    ov->stream_len += n;
}

static void data_input(const uint8_t *p, size_t n);

static void control_input(uint8_t *p, size_t n)
{
    int op = p[0] >> 3;
    size_t b;
    const uint8_t *body = unwrap(p, n, &b);
    if (!body || b < 1)
        return;
    if (ov->have_peer && memcmp(p + 1, ov->peer_sid, 8) && op != P_CONTROL_HARD_RESET_SERVER_V2)
        return;
    if (!ov->have_peer) {
        memcpy(ov->peer_sid, p + 1, 8);
        ov->have_peer = true;
    }
    size_t at = 1, nacks = body[0];
    if (at + nacks * 4 + (nacks ? 8 : 0) > b)
        return;
    for (size_t i = 0; i < nacks; i++) {
        uint32_t id = get32(body + at + i * 4);
        for (int k = 0; k < ov->nsent; k++)
            if (ov->sent[k].id == id) {
                kfree(ov->sent[k].packet);
                ov->sent[k] = ov->sent[--ov->nsent];
                break;
            }
    }
    at += nacks * 4 + (nacks ? 8 : 0);
    if (op == P_ACK_V1 || at + 4 > b)
        return;
    uint32_t id = get32(body + at);
    at += 4;
    if (ov->nacks < 8)
        ov->acks[ov->nacks++] = id;
    if (op == P_CONTROL_SOFT_RESET_V1) { /* the server wants new keys: not supported, the tunnel goes down */
        ov->lost = true;
        str_copy(ov->error, "the server renewed its keys; reconnect", sizeof(ov->error));
        return;
    }
    if (id < ov->expect_msg)
        return; /* again: the ack is on its way */
    if (id == ov->expect_msg) {
        stream_add(body + at, b - at);
        ov->expect_msg++;
        for (bool more = true; more;) { /* anything that waited for this one */
            more = false;
            for (int s = 0; s < RX_SLOTS; s++)
                if (ov->slots[s].used && ov->slots[s].id == ov->expect_msg) {
                    stream_add(ov->slots[s].data, ov->slots[s].len);
                    kfree(ov->slots[s].data);
                    ov->slots[s].used = false;
                    ov->expect_msg++;
                    more = true;
                }
        }
        return;
    }
    for (int s = 0; s < RX_SLOTS; s++)
        if (!ov->slots[s].used || ov->slots[s].id == id) {
            if (ov->slots[s].used)
                return;
            ov->slots[s].data = kmalloc(b - at + 1);
            if (!ov->slots[s].data)
                return;
            memcpy(ov->slots[s].data, body + at, b - at);
            ov->slots[s] = (typeof(ov->slots[s])){true, id, ov->slots[s].data, b - at};
            return;
        }
}

static void packet_input(uint8_t *p, size_t n)
{
    if (n < 9)
        return;
    ov->last_rx = hal_ticks_ms();
    int op = p[0] >> 3;
    if (op == P_DATA_V2 || op == P_DATA_V1)
        data_input(p, n);
    else
        control_input(p, n);
}

/* reads what has arrived, retransmits what was not acknowledged, sends the acks */
static void pump(void)
{
    uint8_t buf[2048];
    for (int rounds = 0; rounds < 64; rounds++) {
        long k;
        if (ov->cfg.tcp) {
            k = net_recv(ov->sock, ov->tcp_buf + ov->tcp_len, sizeof(ov->tcp_buf) - ov->tcp_len);
            if (k == 0) {
                ov->lost = true;
                str_copy(ov->error, "the server closed the connection", sizeof(ov->error));
                break;
            }
            if (k < 0)
                break;
            ov->tcp_len += (size_t)k;
            while (ov->tcp_len >= 2) {
                size_t len = (size_t)ov->tcp_buf[0] << 8 | ov->tcp_buf[1];
                if (ov->tcp_len < 2 + len)
                    break;
                memcpy(buf, ov->tcp_buf + 2, MIN(len, sizeof(buf)));
                memmove(ov->tcp_buf, ov->tcp_buf + 2 + len, ov->tcp_len - 2 - len);
                ov->tcp_len -= 2 + len;
                packet_input(buf, MIN(len, sizeof(buf)));
            }
        } else {
            uint32_t from;
            uint16_t port;
            k = net_recvfrom(ov->sock, buf, sizeof(buf), &from, &port);
            if (k < 0)
                break;
            if (from == ov->server)
                packet_input(buf, (size_t)k);
        }
    }
    uint64_t now = hal_ticks_ms();
    for (int i = 0; i < ov->nsent; i++)
        if (now - ov->sent[i].at > (uint64_t)RETRANSMIT_MS * ov->sent[i].tries) {
            transmit(ov->sent[i].packet, ov->sent[i].len);
            ov->sent[i].at = now;
            ov->sent[i].tries++;
        }
    send_acks();
}

/* the TLS session's transport: OpenVPN control messages */
static long io_send(void *ctx, const void *data, size_t n)
{
    UNUSED(ctx);
    const uint8_t *p = data;
    for (size_t at = 0; at < n; at += CONTROL_CHUNK)
        send_control(P_CONTROL_V1, p + at, MIN((size_t)CONTROL_CHUNK, n - at));
    return (long)n;
}

static long io_recv(void *ctx, void *data, size_t n)
{
    UNUSED(ctx);
    pump();
    if (ov->lost)
        return 0;
    if (!ov->stream_len)
        return E_AGAIN;
    size_t k = MIN(n, ov->stream_len);
    memcpy(data, ov->stream, k);
    memmove(ov->stream, ov->stream + k, ov->stream_len - k);
    ov->stream_len -= k;
    return (long)k;
}

/* ---- keys ---- */

/* TLS 1.0's PRF: P_MD5 over the first half of the secret XOR P_SHA1 over the second */
static void p_hash(const struct hash_alg *h, const uint8_t *secret, size_t slen, const uint8_t *seed, size_t seed_len,
                   uint8_t *out, size_t n)
{
    uint8_t a[20], block[20];
    size_t d = h->digest;
    hmac(h, secret, slen, seed, seed_len, a);
    for (size_t at = 0; at < n; at += d) {
        struct hmac m;
        hmac_init(&m, h, secret, slen);
        hmac_update(&m, a, d);
        hmac_update(&m, seed, seed_len);
        hmac_final(&m, block);
        for (size_t k = 0; k < d && at + k < n; k++)
            out[at + k] ^= block[k];
        hmac(h, secret, slen, a, d, a);
    }
}

static void prf(const uint8_t *secret, size_t slen, const char *label, const uint8_t *seed, size_t seed_len,
                uint8_t *out, size_t n)
{
    uint8_t s[200];
    size_t l = strlen(label);
    memcpy(s, label, l);
    memcpy(s + l, seed, seed_len);
    memset(out, 0, n);
    size_t half = (slen + 1) / 2;
    p_hash(&hash_md5, secret, half, s, l + seed_len, out, n);
    p_hash(&hash_sha1, secret + slen - half, half, s, l + seed_len, out, n);
}

/* key method 2: what the client sends inside TLS, then the data channel keys from both sides' randoms */
static uint8_t pre_master[48], c_random1[32], c_random2[32];

static void str16(uint8_t *out, size_t *o, const char *s)
{
    size_t n = strlen(s) + 1;
    out[(*o)++] = (uint8_t)(n >> 8);
    out[(*o)++] = (uint8_t)n;
    memcpy(out + *o, s, n);
    *o += n;
}

static int send_key_method(const char *user, const char *pass)
{
    uint8_t m[1024];
    size_t o = 0;
    memset(m, 0, 4);
    o = 4;
    m[o++] = 2;
    random_bytes(pre_master, 48);
    random_bytes(c_random1, 32);
    random_bytes(c_random2, 32);
    memcpy(m + o, pre_master, 48);
    memcpy(m + o + 48, c_random1, 32);
    memcpy(m + o + 80, c_random2, 32);
    o += 112;
    char options[200];
    ksnprintf(options, sizeof(options), "V4,dev-type tun,link-mtu 1549,tun-mtu 1500,proto %s,keysize 256,key-method 2,"
              "tls-client", ov->cfg.tcp ? "TCPv4_CLIENT" : "UDPv4");
    str16(m, &o, options);
    str16(m, &o, user);
    str16(m, &o, pass);
    str16(m, &o, "IV_VER=2.6.0\nIV_PLAT=synthos\nIV_PROTO=6\nIV_NCP=2\nIV_CIPHERS=AES-256-GCM:AES-128-GCM:"
                 "CHACHA20-POLY1305\nIV_GUI_VER=SynthOS\n");
    return tls_write(ov->tls, m, o) < 0 ? E_IO : 0;
}

/* reads one message of the TLS channel (key method reply, or a NUL-terminated string) */
static long read_tls(uint8_t *buf, size_t cap, int timeout_ms)
{
    uint64_t start = hal_ticks_ms();
    while (hal_ticks_ms() - start < (uint64_t)timeout_ms) {
        long k = tls_read(ov->tls, buf, cap);
        if (k > 0)
            return k;
        if (k == 0 || (k < 0 && k != E_AGAIN) || ov->lost)
            return k ? k : E_PIPE;
        net_wait(5);
    }
    return E_TIMEDOUT;
}

static void derive_keys(const uint8_t *s_random1, const uint8_t *s_random2)
{
    uint8_t master[48], seed[128], block[256];
    memcpy(seed, c_random1, 32);
    memcpy(seed + 32, s_random1, 32);
    prf(pre_master, 48, "OpenVPN master secret", seed, 64, master, 48);
    memcpy(seed, c_random2, 32);
    memcpy(seed + 32, s_random2, 32);
    memcpy(seed + 64, ov->sid, 8);
    memcpy(seed + 72, ov->peer_sid, 8);
    prf(master, 48, "OpenVPN key expansion", seed, 80, block, 256);
    /* two keys of cipher (64) + HMAC (64); the client sends with the first and receives with the second */
    const uint8_t *out = block, *in = block + 128;
    size_t klen = ov->cipher == C_AES128 ? 16 : 32;
    memcpy(ov->key_out, out, klen);
    memcpy(ov->key_in, in, klen);
    memcpy(ov->iv_out, out + 64, 8); /* AEAD: the implicit IV comes from the HMAC half */
    memcpy(ov->iv_in, in + 64, 8);
    if (ov->cipher != C_CHACHA) {
        gcm_init(&ov->gcm_out, ov->key_out, klen);
        gcm_init(&ov->gcm_in, ov->key_in, klen);
    }
    memset(pre_master, 0, sizeof(pre_master));
    memset(master, 0, sizeof(master));
    memset(block, 0, sizeof(block));
}

/* ---- the data channel ---- */

static void seal_send(const uint8_t *packet, size_t n)
{
    uint8_t *out = kmalloc(n + 64);
    if (!out)
        return;
    size_t o = 0;
    bool v2 = ov->peer_id >= 0;
    if (v2) {
        put32(out, (uint32_t)P_DATA_V2 << 27 | (uint32_t)ov->peer_id);
        o = 4;
    } else {
        out[o++] = P_DATA_V1 << 3;
    }
    uint32_t pid = ++ov->data_pid;
    put32(out + o, pid);
    uint8_t nonce[12], aad[8];
    memcpy(nonce, out + o, 4);
    memcpy(nonce + 4, ov->iv_out, 8);
    size_t aad_len = 0;
    if (v2) {
        memcpy(aad, out, 4);
        aad_len = 4;
    }
    memcpy(aad + aad_len, out + o, 4);
    aad_len += 4;
    o += 4;
    uint8_t *tag = out + o, *ct = out + o + 16; /* OpenVPN puts the tag first */
    if (ov->cipher == C_CHACHA)
        chacha20_poly1305_seal(ov->key_out, nonce, aad, aad_len, packet, n, ct, tag);
    else
        gcm_seal(&ov->gcm_out, nonce, aad, aad_len, packet, n, ct, tag);
    transmit(out, o + 16 + n);
    ov->tx_bytes += n;
    kfree(out);
}

static bool replay_ok(uint32_t pid)
{
    if (pid > ov->recv_max) {
        uint32_t shift = pid - ov->recv_max;
        ov->recv_bitmap = shift >= 64 ? 0 : ov->recv_bitmap << shift;
        ov->recv_bitmap |= 1;
        ov->recv_max = pid;
        return true;
    }
    uint32_t back = ov->recv_max - pid;
    if (back >= 64 || ov->recv_bitmap >> back & 1)
        return false;
    ov->recv_bitmap |= 1ull << back;
    return true;
}

static void data_input(const uint8_t *p, size_t n)
{
    if (!ov->connected)
        return;
    bool v2 = p[0] >> 3 == P_DATA_V2;
    size_t o = v2 ? 4 : 1;
    if (n < o + 4 + 16)
        return;
    uint8_t nonce[12], aad[8];
    memcpy(nonce, p + o, 4);
    memcpy(nonce + 4, ov->iv_in, 8);
    size_t aad_len = 0;
    if (v2) {
        memcpy(aad, p, 4);
        aad_len = 4;
    }
    memcpy(aad + aad_len, p + o, 4);
    aad_len += 4;
    uint32_t pid = get32(p + o);
    size_t len = n - o - 4 - 16;
    uint8_t *plain = kmalloc(len + 1);
    bool ok = false;
    if (plain)
        ok = ov->cipher == C_CHACHA
                 ? chacha20_poly1305_open(ov->key_in, nonce, aad, aad_len, p + o + 20, len, plain, p + o + 4)
                 : gcm_open(&ov->gcm_in, nonce, aad, aad_len, p + o + 20, len, plain, p + o + 4);
    if (ok && replay_ok(pid) && !(len == 16 && !memcmp(plain, ping_magic, 16)) && len) {
        ov->rx_bytes += len;
        vpn_tunnel_input(plain, len);
    }
    kfree(plain);
}

/* ---- routing ---- */

static bool routes(uint32_t dst)
{
    if (!ov || !ov->connected || dst == ov->server)
        return false;
    if (ov->redirect)
        return true;
    if (ov->mask && (dst & ov->mask) == (ov->addr & ov->mask))
        return true;
    for (int i = 0; i < ov->nroutes; i++)
        if ((dst & ov->routes[i].mask) == ov->routes[i].net)
            return true;
    return false;
}

static bool prefix_match(const uint8_t *a, const uint8_t *net, int bits)
{
    int whole = bits / 8;
    if (memcmp(a, net, (size_t)whole))
        return false;
    return bits % 8 == 0 || !((a[whole] ^ net[whole]) & (0xff00 >> (bits % 8)));
}

static bool routes6(const uint8_t *d)
{
    if (!ov || !ov->connected || !ov->has6)
        return false;
    for (int i = 0; i < ov->nroutes6; i++)
        if (prefix_match(d, ov->routes6[i].net, ov->routes6[i].bits))
            return true;
    return false;
}

static int output(const uint8_t *packet, size_t n)
{
    if (!ov || !ov->connected)
        return E_UNREACH;
    if (n > TUN_MTU)
        return E_RANGE;
    seal_send(packet, n);
    return 0;
}

static void tunnel_tick(uint64_t now)
{
    UNUSED(now);
    if (!ov || !ov->connected)
        return;
    pump();
    uint64_t t = hal_ticks_ms();
    if (ov->ping_every && t - ov->last_tx > (uint64_t)ov->ping_every * 1000)
        seal_send(ping_magic, 16);
    if (ov->ping_restart && t - ov->last_rx > (uint64_t)ov->ping_restart * 1000 && !ov->lost) {
        ov->lost = true;
        str_copy(ov->error, "the server stopped answering", sizeof(ov->error));
    }
}

static struct vpn_tunnel tunnel = {.routes = routes, .routes6 = routes6, .output = output, .tick = tunnel_tick};

/* ---- the server's push ---- */

static uint32_t parse_ip(const char *s)
{
    uint32_t ip = 0;
    net_parse_ip(s, &ip);
    return ip;
}

static void take_push(char *reply)
{
    ov->peer_id = -1;
    ov->ping_every = 10;
    ov->ping_restart = 60;
    for (char *item = reply; item && *item;) {
        char *comma = strchr(item, ',');
        if (comma)
            *comma = 0;
        char *argv[5];
        int argc = 0;
        for (char *t = item; *t && argc < 5;) {
            while (*t == ' ')
                *t++ = 0;
            if (!*t)
                break;
            argv[argc++] = t;
            while (*t && *t != ' ')
                t++;
        }
        if (argc >= 3 && !strcmp(argv[0], "ifconfig")) {
            ov->addr = parse_ip(argv[1]);
            uint32_t second = parse_ip(argv[2]);
            if ((second >> 24) == 255) {
                ov->mask = second; /* topology subnet */
            } else if (ov->nroutes < MAX_ROUTES) {
                ov->routes[ov->nroutes++] = (struct route){second, 0xffffffffu}; /* net30 / p2p: the peer */
                ov->gateway = second;
            }
        } else if (argc >= 2 && !strcmp(argv[0], "route-gateway")) {
            ov->gateway = parse_ip(argv[1]);
        } else if (argc >= 2 && !strcmp(argv[0], "route") && ov->nroutes < MAX_ROUTES) {
            uint32_t net = parse_ip(argv[1]), mask = argc >= 3 ? parse_ip(argv[2]) : 0xffffffffu;
            ov->routes[ov->nroutes++] = (struct route){net & mask, mask};
        } else if (argc >= 1 && !strcmp(argv[0], "redirect-gateway")) {
            ov->redirect = true;
            for (int i = 1; i < argc; i++)
                if (!strcmp(argv[i], "block-ipv6") || !strcmp(argv[i], "ipv6"))
                    ov->block6 = true;
        } else if (argc >= 1 && !strcmp(argv[0], "block-ipv6")) {
            ov->block6 = true;
        } else if (argc >= 3 && !strcmp(argv[0], "dhcp-option") && !strcmp(argv[1], "DNS") && !ov->dns) {
            ov->dns = parse_ip(argv[2]);
        } else if (argc >= 2 && !strcmp(argv[0], "peer-id")) {
            ov->peer_id = (int)str_to_long(argv[1], NULL, 10);
        } else if (argc >= 2 && !strcmp(argv[0], "cipher")) {
            ov->cipher = !str_casecmp(argv[1], "AES-128-GCM")         ? C_AES128
                         : !str_casecmp(argv[1], "CHACHA20-POLY1305") ? C_CHACHA
                                                                      : C_AES256;
        } else if (argc >= 2 && !strcmp(argv[0], "ping")) {
            ov->ping_every = (int)str_to_long(argv[1], NULL, 10);
        } else if (argc >= 2 && !strcmp(argv[0], "ping-restart")) {
            ov->ping_restart = (int)str_to_long(argv[1], NULL, 10);
        } else if (argc >= 2 && !strcmp(argv[0], "ifconfig-ipv6")) {
            char *slash = strchr(argv[1], '/');
            int bits = slash ? (int)str_to_long(slash + 1, NULL, 10) : 64;
            if (slash)
                *slash = 0;
            if (net_parse_ip6(argv[1], ov->addr6)) {
                ov->has6 = true;
                if (ov->nroutes6 < 4) {
                    memcpy(ov->routes6[ov->nroutes6].net, ov->addr6, 16);
                    ov->routes6[ov->nroutes6++].bits = bits;
                }
            }
        } else if (argc >= 2 && !strcmp(argv[0], "route-ipv6") && ov->nroutes6 < 4) {
            char *slash = strchr(argv[1], '/');
            int bits = slash ? (int)str_to_long(slash + 1, NULL, 10) : 128;
            if (slash)
                *slash = 0;
            if (net_parse_ip6(argv[1], ov->routes6[ov->nroutes6].net))
                ov->routes6[ov->nroutes6++].bits = bits;
        }
        item = comma ? comma + 1 : NULL;
    }
}

/* ---- connecting ---- */

static void wrap_keys(void)
{
    /* the static key: two halves of cipher (64) + HMAC (64); key-direction 1 (the client's) sends with the
       second and receives with the first, tls-crypt always so */
    const uint8_t *k = ov->cfg.static_key;
    bool inverse = ov->cfg.wrap == WRAP_CRYPT || ov->cfg.key_direction == 1;
    bool bidi = ov->cfg.wrap == WRAP_AUTH && ov->cfg.key_direction < 0;
    const uint8_t *out = bidi ? k : inverse ? k + 128 : k, *in = bidi ? k : inverse ? k : k + 128;
    memcpy(ov->wrap_out_cipher, out, 32);
    memcpy(ov->wrap_in_cipher, in, 32);
    memcpy(ov->wrap_out_hmac, out + 64, 64);
    memcpy(ov->wrap_in_hmac, in + 64, 64);
}

static void teardown(void)
{
    if (!ov)
        return;
    vpn_tunnel_set(NULL);
    tls_close(ov->tls);
    if (ov->sock >= 0)
        net_close(ov->sock);
    for (int i = 0; i < ov->nsent; i++)
        kfree(ov->sent[i].packet);
    for (int s = 0; s < RX_SLOTS; s++)
        if (ov->slots[s].used)
            kfree(ov->slots[s].data);
    kfree(ov->stream);
    kfree(ov->cfg.ca);
    memset(ov, 0, sizeof(*ov));
    kfree(ov);
    ov = NULL;
}

static int fail(const char *fmt, const char *arg, int err)
{
    ksnprintf(last_error, sizeof(last_error), fmt, arg);
    teardown();
    return err;
}

static int connect(const char *path, const char *user, const char *pass)
{
    teardown();
    vpn_down(); /* one VPN at a time */
    char *text;
    size_t size;
    if (vfs_read_all(path, &text, &size) != 0)
        return fail("can't read %s", path, E_NOENT);
    ov = kzalloc(sizeof(*ov));
    if (!ov) {
        kfree(text);
        return E_NOMEM;
    }
    ov->sock = -1;
    char why[120];
    int err = parse_ovpn(text, &ov->cfg, why, sizeof(why));
    kfree(text);
    if (err)
        return fail("%s", why, err);
    const char *base = strrchr(path, '/');
    str_copy(ov->cfg.name, base ? base + 1 : path, sizeof(ov->cfg.name));
    char *dot = strrchr(ov->cfg.name, '.');
    if (dot)
        *dot = 0;
    wrap_keys();
    if ((err = net_resolve(ov->cfg.host, &ov->server, 5000)))
        return fail("can't find %s", ov->cfg.host, err);
    ov->sock = net_socket(ov->cfg.tcp ? NET_TCP : NET_UDP);
    if (ov->sock < 0)
        return fail("%s", "no socket", ov->sock);
    if (ov->cfg.tcp && (err = net_connect(ov->sock, ov->server, ov->cfg.port, 10000)))
        return fail("can't connect to %s", ov->cfg.host, err);
    random_bytes(ov->sid, 8);
    /* the hard reset, then TLS inside control messages */
    send_control(P_CONTROL_HARD_RESET_CLIENT_V2, NULL, 0);
    uint64_t start = hal_ticks_ms();
    while (!ov->have_peer && hal_ticks_ms() - start < 8000) {
        pump();
        net_wait(5);
    }
    if (!ov->have_peer)
        return fail("%s did not answer (check the address, the port and the tls-auth/tls-crypt key)", ov->cfg.host,
                    E_TIMEDOUT);
    static struct tls_io io;
    io = (struct tls_io){.send = io_send, .recv = io_recv, .ca_pem = ov->cfg.ca, .ca_len = ov->cfg.ca_len};
    ov->tls = tls_connect_io(&io, ov->cfg.verify_name, true, &err);
    if (!ov->tls)
        return fail("the secure channel failed: %s", tls_strerror(err), err);
    if ((err = send_key_method(user, pass)))
        return fail("%s", "can't send the credentials", err);
    uint8_t reply[2048];
    long k = read_tls(reply, sizeof(reply), 10000);
    if (k < 5 + 64 || reply[4] != 2)
        return fail("%s", "the server did not accept the connection", E_PERM);
    /* the server's options carry its cipher; AES-256-GCM unless it says otherwise in the push */
    uint8_t s_random1[32], s_random2[32];
    memcpy(s_random1, reply + 5, 32);
    memcpy(s_random2, reply + 37, 32);
    tls_write(ov->tls, "PUSH_REQUEST", 13);
    char push[2048] = "";
    start = hal_ticks_ms();
    while (hal_ticks_ms() - start < 10000 && strncmp(push, "PUSH_REPLY", 10)) {
        k = read_tls((uint8_t *)push, sizeof(push) - 1, 3000);
        if (k == E_TIMEDOUT) {
            tls_write(ov->tls, "PUSH_REQUEST", 13);
            continue;
        }
        if (k <= 0)
            break;
        push[k] = 0;
        if (!strncmp(push, "AUTH_FAILED", 11))
            return fail("%s", "the user name or password was not accepted", E_PERM);
    }
    if (strncmp(push, "PUSH_REPLY", 10))
        return fail("%s", "the server sent no settings", E_IO);
    take_push(push + 11);
    if (!ov->addr)
        return fail("%s", "the server gave no address", E_IO);
    derive_keys(s_random1, s_random2);
    ov->connected = true;
    ov->up_at = ov->last_rx = hal_ticks_ms();
    tunnel.address = ov->addr;
    tunnel.dns = ov->dns;
    tunnel.has6 = ov->has6;
    memcpy(tunnel.address6, ov->addr6, 16);
    tunnel.block6 = ov->block6 || ov->redirect;
    tunnel.mtu = TUN_MTU;
    vpn_tunnel_set(&tunnel);
    last_error[0] = 0;
    return 0;
}

/* ---- the command ---- */

static void status(struct shell *sh)
{
    if (!ov) {
        sh_printf(sh, "OpenVPN is off%s%s\n", last_error[0] ? ": " : "", last_error);
        return;
    }
    char addr[20], dns[20], a6[48];
    net_format_ip(ov->addr, addr, sizeof(addr));
    net_format_ip(ov->dns, dns, sizeof(dns));
    sh_printf(sh, "OpenVPN %s: %s\n", ov->cfg.name, ov->lost ? ov->error : "connected");
    sh_printf(sh, "  address %s", addr);
    if (ov->has6) {
        net_format_ip6(ov->addr6, a6, sizeof(a6));
        sh_printf(sh, " %s", a6);
    }
    sh_printf(sh, ", dns %s, server %s port %u/%s\n", ov->dns ? dns : "-", ov->cfg.host, ov->cfg.port,
              ov->cfg.tcp ? "tcp" : "udp");
    sh_printf(sh, "  %s, %s, %s%s; received %u KB, sent %u KB\n",
              ov->cipher == C_CHACHA ? "CHACHA20-POLY1305" : ov->cipher == C_AES128 ? "AES-128-GCM" : "AES-256-GCM",
              ov->cfg.wrap == WRAP_CRYPT ? "tls-crypt" : ov->cfg.wrap == WRAP_AUTH ? "tls-auth" : "no tls wrap",
              ov->redirect ? "all traffic" : "routes pushed by the server", ov->block6 && !ov->has6 ? ", IPv6 blocked" : "",
              (unsigned)(ov->rx_bytes / 1024), (unsigned)(ov->tx_bytes / 1024));
}

COMMAND(openvpn, NULL, "net", "openvpn up FILE.ovpn [USER PASSWORD] | down | status",
        "connect to an OpenVPN server with its .ovpn file")
{
    if (argc >= 3 && !strcmp(argv[1], "up")) {
        char path[256];
        path_resolve(sh->cwd, argv[2], path, sizeof(path));
        int err = connect(path, argc >= 4 ? argv[3] : "", argc >= 5 ? argv[4] : "");
        if (err)
            return sh_error(sh, "openvpn", "%s", last_error);
        status(sh);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "down")) {
        bool was = ov != NULL;
        teardown();
        last_error[0] = 0;
        sh_printf(sh, was ? "OpenVPN disconnected\n" : "OpenVPN is off\n");
        return 0;
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        status(sh);
        return 0;
    }
    return command_usage(sh, command_find("openvpn"));
}

static const struct command *const commands[] = {&cmddef_openvpn};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(NULL, NULL, &ext);
