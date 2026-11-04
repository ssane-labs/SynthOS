#include <synth/command.h>
#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>
#include <synth/vpn.h>

#include "net.h"

/* A WireGuard VPN client. The configuration is the usual wg-quick file the VPN service gives (in /home/etc/vpn);
   while it is up, IPv4 traffic for the peer's AllowedIPs goes through the tunnel (the local network and the
   endpoint itself stay outside), DNS goes to the tunnel's server, and IPv6 outside the tunnel stops when the
   tunnel takes ::/0, so nothing leaks around it. This side starts the handshakes (Noise_IKpsk2). */

#define VPN_DIR "/home/etc/vpn"
#define MAX_ALLOWED 16
#define REKEY_AFTER 120000
#define REJECT_AFTER 180000
#define REKEY_TIMEOUT 5000
#define KEEPALIVE_TIMEOUT 10000
#define HANDSHAKE_TRIES 18
#define PENDING 8
#define TUNNEL_MTU 1420

struct route4 {
    uint32_t ip, mask;
};

struct wg_config {
    char name[32];
    uint8_t priv[32], pub[32], peer[32], psk[32];
    uint32_t addr, mask, dns;
    char endpoint[128];
    uint16_t port;
    struct route4 allowed[MAX_ALLOWED];
    int allowed_count;
    bool v6_all;
    int keepalive;
};

struct wg {
    bool want, up;
    struct wg_config cfg;
    uint32_t endpoint;
    uint16_t lport;
    struct dns_query dns;
    bool resolving;
    uint8_t mac1_key[32], mac1_peer_key[32];
    /* handshake in flight */
    uint32_t hs_index;
    uint8_t hs_priv[32], hs_hash[32], hs_chain[32];
    uint64_t hs_sent;
    int hs_tries;
    bool hs_waiting;
    /* session */
    bool keys;
    uint32_t local_index, remote_index;
    uint8_t send_key[32], recv_key[32];
    uint64_t send_counter, recv_max, recv_bitmap;
    uint64_t session_at, last_rx, last_tx, last_handshake;
    uint64_t rx_bytes, tx_bytes;
    uint8_t *pending[PENDING];
    size_t pending_len[PENDING];
    int pending_count;
    char error[96];
};

static struct wg *wg;
static char wanted[32];

static const char construction[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char identifier[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";

/* --- small helpers --- */

static int b64_value(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
           : c == '+'                                                                    ? 62
           : c == '/'                                                                    ? 63
                                                                                         : -1;
}

static bool key_decode(const char *s, uint8_t out[32])
{
    uint8_t buf[33];
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        int v = b64_value(*s);
        if (v < 0)
            return false;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == sizeof(buf))
                return false;
            buf[n++] = (uint8_t)(acc >> bits);
        }
    }
    if (n != 32)
        return false;
    memcpy(out, buf, 32);
    return true;
}

static void key_encode(const uint8_t in[32], char out[45])
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (int i = 0; i < 33; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < 32 ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < 32 ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < 32 ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < 32 ? t[v & 63] : '=';
    }
    out[44] = 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void mix_hash(uint8_t h[32], const void *data, size_t n)
{
    struct blake2s s;
    blake2s_init_key(&s, 32, NULL, 0);
    blake2s_update(&s, h, 32);
    blake2s_update(&s, data, n);
    blake2s_final(&s, h);
}

/* KDF_n of the protocol: HMAC-BLAKE2s chained from the chaining key. */
static void kdf(const uint8_t key[32], const void *input, size_t n, uint8_t *t1, uint8_t *t2, uint8_t *t3)
{
    uint8_t prk[32], t[33];
    hmac(&hash_blake2s, key, 32, input, n, prk);
    t[0] = 1;
    hmac(&hash_blake2s, prk, 32, t, 1, t);
    if (t1)
        memcpy(t1, t, 32);
    t[32] = 2;
    hmac(&hash_blake2s, prk, 32, t, 33, t);
    if (t2)
        memcpy(t2, t, 32);
    t[32] = 3;
    hmac(&hash_blake2s, prk, 32, t, 33, t);
    if (t3)
        memcpy(t3, t, 32);
    memset(prk, 0, sizeof(prk));
}

static void nonce_for(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    put_le64(nonce + 4, counter);
}

static void seal(const uint8_t key[32], uint64_t counter, const uint8_t *in, size_t n, const uint8_t *aad,
                 size_t aad_len, uint8_t *out)
{
    uint8_t nonce[12];
    nonce_for(counter, nonce);
    chacha20_poly1305_seal(key, nonce, aad, aad_len, in, n, out, out + n);
}

static bool open_box(const uint8_t key[32], uint64_t counter, const uint8_t *in, size_t n, const uint8_t *aad,
                     size_t aad_len, uint8_t *out)
{
    if (n < 16)
        return false;
    uint8_t nonce[12];
    nonce_for(counter, nonce);
    return chacha20_poly1305_open(key, nonce, aad, aad_len, in, n - 16, out, in + n - 16);
}

static void label_key(const char *label, const uint8_t pub[32], uint8_t out[32])
{
    struct blake2s s;
    blake2s_init_key(&s, 32, NULL, 0);
    blake2s_update(&s, label, 8);
    blake2s_update(&s, pub, 32);
    blake2s_final(&s, out);
}

/* The server takes an initiation only with a newer timestamp than the last one, so it never goes back. */
static void tai64n(uint8_t out[12])
{
    static uint64_t last;
    uint64_t stamp = (uint64_t)time_now() << 30 | (hal_ticks_ms() % 1000) * 1000000u;
    if (stamp <= last)
        stamp = last + 1;
    last = stamp;
    uint64_t secs = 0x400000000000000aull + (stamp >> 30);
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(secs >> (56 - 8 * i));
    wr32(out + 8, (uint32_t)(stamp & ((1u << 30) - 1)));
}

/* --- configuration --- */

static char *trim(char *s)
{
    while (is_space(*s))
        s++;
    size_t n = strlen(s);
    while (n && is_space(s[n - 1]))
        s[--n] = 0;
    return s;
}

static bool parse_cidr(const char *s, uint32_t *ip, uint32_t *mask)
{
    char buf[64];
    str_copy(buf, s, sizeof(buf));
    char *slash = strchr(buf, '/');
    int bits = 32;
    if (slash) {
        *slash = 0;
        bits = (int)str_to_long(slash + 1, NULL, 10);
    }
    if (bits < 0 || bits > 32 || !net_parse_ip(buf, ip))
        return false;
    *mask = bits ? 0xffffffffu << (32 - bits) : 0;
    *ip &= *mask;
    return true;
}

static int wg_parse(const char *text, struct wg_config *c, char *why, size_t cap)
{
    memset(c, 0, sizeof(*c));
    bool have_priv = false, have_peer = false, in_peer = false;
    int peers = 0;
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char line[512];
        str_copy(line, p, MIN(n + 1, sizeof(line)));
        p += n + (end ? 1 : 0);
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *l = trim(line);
        if (!*l)
            continue;
        if (*l == '[') {
            in_peer = !str_ncasecmp(l, "[peer]", 6);
            peers += in_peer;
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = trim(l), *value = trim(eq + 1);
        if (!in_peer && !str_casecmp(key, "PrivateKey")) {
            have_priv = key_decode(value, c->priv);
        } else if (!in_peer && !str_casecmp(key, "Address")) {
            for (char *item = value; item && *item;) {
                char *comma = strchr(item, ',');
                if (comma)
                    *comma = 0;
                uint32_t ip, mask;
                if (!c->addr && !strchr(item, ':') && parse_cidr(trim(item), &ip, &mask)) {
                    char *slash = strchr(item, '/');
                    if (slash)
                        *slash = 0;
                    net_parse_ip(trim(item), &c->addr);
                    c->mask = mask;
                }
                item = comma ? comma + 1 : NULL;
            }
        } else if (!in_peer && !str_casecmp(key, "DNS")) {
            for (char *item = value; item && *item && !c->dns;) {
                char *comma = strchr(item, ',');
                if (comma)
                    *comma = 0;
                uint32_t ip;
                if (!strchr(item, ':') && net_parse_ip(trim(item), &ip))
                    c->dns = ip;
                item = comma ? comma + 1 : NULL;
            }
        } else if (in_peer && peers == 1 && !str_casecmp(key, "PublicKey")) {
            have_peer = key_decode(value, c->peer);
        } else if (in_peer && peers == 1 && !str_casecmp(key, "PresharedKey")) {
            key_decode(value, c->psk);
        } else if (in_peer && peers == 1 && !str_casecmp(key, "Endpoint")) {
            char *colon = value[0] == '[' ? strchr(value, ']') : strrchr(value, ':');
            if (value[0] == '[' && colon) {
                *colon = 0;
                str_copy(c->endpoint, value + 1, sizeof(c->endpoint));
                colon = strchr(colon + 1, ':');
            } else if (colon) {
                *colon = 0;
                str_copy(c->endpoint, value, sizeof(c->endpoint));
            }
            c->port = colon ? (uint16_t)str_to_long(colon + 1, NULL, 10) : 0;
        } else if (in_peer && peers == 1 && !str_casecmp(key, "AllowedIPs")) {
            for (char *item = value; item && *item;) {
                char *comma = strchr(item, ',');
                if (comma)
                    *comma = 0;
                char *t = trim(item);
                if (strchr(t, ':')) {
                    if (!strcmp(t, "::/0"))
                        c->v6_all = true;
                } else if (c->allowed_count < MAX_ALLOWED &&
                           parse_cidr(t, &c->allowed[c->allowed_count].ip, &c->allowed[c->allowed_count].mask)) {
                    c->allowed_count++;
                }
                item = comma ? comma + 1 : NULL;
            }
        } else if (in_peer && peers == 1 && !str_casecmp(key, "PersistentKeepalive")) {
            c->keepalive = (int)CLAMP(str_to_long(value, NULL, 10), 0, 65535);
        }
    }
    const char *bad = !have_priv                   ? "no valid PrivateKey in [Interface]"
                      : !c->addr                   ? "no IPv4 Address in [Interface]"
                      : !peers                     ? "no [Peer]"
                      : !have_peer                 ? "no valid PublicKey in [Peer]"
                      : !c->endpoint[0] || !c->port ? "no Endpoint (host:port) in [Peer]"
                      : !c->allowed_count          ? "no IPv4 AllowedIPs in [Peer]"
                                                   : NULL;
    if (bad) {
        str_copy(why, bad, cap);
        return E_INVAL;
    }
    x25519_base(c->pub, c->priv);
    return 0;
}

static int load(const char *name, struct wg_config *c, char *why, size_t cap)
{
    char path[PATH_MAX_LEN], *text;
    size_t size;
    ksnprintf(path, sizeof(path), "%s/%s.conf", VPN_DIR, name);
    int err = vfs_read_all(path, &text, &size);
    if (err) {
        ksnprintf(why, cap, "no VPN called %s", name);
        return err;
    }
    err = wg_parse(text, c, why, cap);
    kfree(text);
    if (!err)
        str_copy(c->name, name, sizeof(c->name));
    return err;
}

/* --- routing --- */

bool wg_routes(uint32_t dst)
{
    if (!wg || !wg->up || !dst || net_is_v6(dst) || dst == wg->endpoint || dst >> 24 == 127 || dst == 0xffffffffu ||
        dst == wg->cfg.addr)
        return false;
    for (int i = 0; i < net_interface_count(); i++) {
        struct net_iface *ifc = net_interface(i);
        if (ifc->configured && (dst & ifc->mask) == (ifc->ip & ifc->mask))
            return false;
    }
    for (int i = 0; i < wg->cfg.allowed_count; i++)
        if ((dst & wg->cfg.allowed[i].mask) == wg->cfg.allowed[i].ip)
            return true;
    return false;
}

uint32_t wg_address(void)
{
    return wg && wg->up ? wg->cfg.addr : 0;
}

bool wg_blocks_v6(uint32_t dst)
{
    return wg && wg->up && wg->cfg.v6_all && (!dst || dst != wg->endpoint);
}

uint16_t wg_mss(void)
{
    return TUNNEL_MTU - 40;
}

/* --- the wire --- */

static void transmit(const uint8_t *msg, size_t n)
{
    if (!wg->endpoint)
        return;
    int err = udp_send_raw(wg->endpoint, wg->cfg.port, wg->lport, msg, n);
    if (!err)
        wg->last_tx = net_now();
}

static void send_initiation(void)
{
    uint8_t m[148] = {1};
    uint8_t chain[32], hash[32], key[32], dh[32], ts[12];
    random_bytes(&wg->hs_index, sizeof(wg->hs_index));
    put_le32(m + 4, wg->hs_index);
    blake2s(chain, 32, NULL, 0, construction, sizeof(construction) - 1);
    memcpy(hash, chain, 32);
    mix_hash(hash, identifier, sizeof(identifier) - 1);
    mix_hash(hash, wg->cfg.peer, 32);
    random_bytes(wg->hs_priv, 32);
    x25519_base(m + 8, wg->hs_priv);
    kdf(chain, m + 8, 32, chain, NULL, NULL);
    mix_hash(hash, m + 8, 32);
    x25519(dh, wg->hs_priv, wg->cfg.peer);
    kdf(chain, dh, 32, chain, key, NULL);
    seal(key, 0, wg->cfg.pub, 32, hash, 32, m + 40);
    mix_hash(hash, m + 40, 48);
    x25519(dh, wg->cfg.priv, wg->cfg.peer);
    kdf(chain, dh, 32, chain, key, NULL);
    tai64n(ts);
    seal(key, 0, ts, 12, hash, 32, m + 88);
    mix_hash(hash, m + 88, 28);
    blake2s(m + 116, 16, wg->mac1_peer_key, 32, m, 116);
    memcpy(wg->hs_chain, chain, 32);
    memcpy(wg->hs_hash, hash, 32);
    wg->hs_waiting = true;
    wg->hs_sent = net_now();
    wg->hs_tries++;
    memset(dh, 0, sizeof(dh));
    memset(key, 0, sizeof(key));
    transmit(m, sizeof(m));
}

static void send_data(const uint8_t *packet, size_t n)
{
    size_t padded = (n + 15) & ~(size_t)15;
    if (padded > TUNNEL_MTU)
        padded = n;
    uint8_t *m = kmalloc(16 + padded + 16);
    if (!m)
        return;
    uint8_t *plain = kzalloc(padded ? padded : 1);
    if (!plain) {
        kfree(m);
        return;
    }
    if (n)
        memcpy(plain, packet, n);
    m[0] = 4;
    m[1] = m[2] = m[3] = 0;
    put_le32(m + 4, wg->remote_index);
    put_le64(m + 8, wg->send_counter);
    seal(wg->send_key, wg->send_counter, plain, padded, NULL, 0, m + 16);
    wg->send_counter++;
    wg->tx_bytes += n;
    transmit(m, 16 + padded + 16);
    kfree(plain);
    kfree(m);
}

static void flush_pending(void)
{
    for (int i = 0; i < wg->pending_count; i++) {
        send_data(wg->pending[i], wg->pending_len[i]);
        kfree(wg->pending[i]);
    }
    wg->pending_count = 0;
}

static void response_input(const uint8_t *m, size_t n)
{
    if (n != 92 || !wg->hs_waiting || get_le32(m + 8) != wg->hs_index)
        return;
    uint8_t mac[16];
    blake2s(mac, 16, wg->mac1_key, 32, m, 60);
    if (memcmp(mac, m + 60, 16))
        return;
    uint8_t chain[32], hash[32], dh[32], tau[32], key[32], empty[1];
    memcpy(chain, wg->hs_chain, 32);
    memcpy(hash, wg->hs_hash, 32);
    kdf(chain, m + 12, 32, chain, NULL, NULL);
    mix_hash(hash, m + 12, 32);
    x25519(dh, wg->hs_priv, m + 12);
    kdf(chain, dh, 32, chain, NULL, NULL);
    x25519(dh, wg->cfg.priv, m + 12);
    kdf(chain, dh, 32, chain, NULL, NULL);
    kdf(chain, wg->cfg.psk, 32, chain, tau, key);
    mix_hash(hash, tau, 32);
    if (!open_box(key, 0, m + 44, 16, hash, 32, empty)) {
        str_copy(wg->error, "the server did not accept the keys", sizeof(wg->error));
        return;
    }
    kdf(chain, NULL, 0, wg->send_key, wg->recv_key, NULL);
    wg->keys = true;
    wg->local_index = wg->hs_index;
    wg->remote_index = get_le32(m + 4);
    wg->send_counter = 0;
    wg->recv_max = 0;
    wg->recv_bitmap = 0;
    wg->session_at = wg->last_handshake = net_now();
    wg->hs_waiting = false;
    wg->hs_tries = 0;
    wg->error[0] = 0;
    memset(wg->hs_priv, 0, 32);
    memset(dh, 0, sizeof(dh));
    memset(key, 0, sizeof(key));
    if (wg->pending_count)
        flush_pending();
    else
        send_data(NULL, 0);
}

static bool replay_ok(uint64_t c)
{
    if (c + 64 <= wg->recv_max && wg->recv_max >= 64)
        return false;
    if (c > wg->recv_max) {
        uint64_t shift = c - wg->recv_max;
        wg->recv_bitmap = shift >= 64 ? 0 : wg->recv_bitmap << shift;
        wg->recv_bitmap |= 1;
        wg->recv_max = c;
        return true;
    }
    uint64_t bit = 1ull << (wg->recv_max - c);
    if (wg->recv_bitmap & bit)
        return false;
    wg->recv_bitmap |= bit;
    return true;
}

static void data_input(const uint8_t *m, size_t n)
{
    if (!wg->keys || n < 32 || get_le32(m + 4) != wg->local_index || net_now() - wg->session_at >= REJECT_AFTER)
        return;
    uint64_t counter = get_le64(m + 8);
    uint8_t *plain = kmalloc(n);
    if (!plain)
        return;
    if (!open_box(wg->recv_key, counter, m + 16, n - 16, NULL, 0, plain) || !replay_ok(counter)) {
        kfree(plain);
        return;
    }
    size_t len = n - 32;
    if (len)
        wg->last_rx = net_now();
    if (len >= IP_HEADER && plain[0] >> 4 == 4) {
        size_t total = rd16(plain + 2);
        uint32_t src = rd32(plain + 12);
        bool allowed = false;
        for (int i = 0; i < wg->cfg.allowed_count; i++)
            if ((src & wg->cfg.allowed[i].mask) == wg->cfg.allowed[i].ip)
                allowed = true;
        if (allowed && total <= len) {
            wg->rx_bytes += total;
            net_ip_input(plain, total);
        }
    }
    kfree(plain);
}

bool wg_claims(uint32_t src, uint16_t sport, uint16_t dport)
{
    return wg && wg->up && dport == wg->lport && src == wg->endpoint && sport == wg->cfg.port;
}

void wg_input(const uint8_t *m, size_t n)
{
    if (n < 4 || m[1] || m[2] || m[3])
        return;
    if (m[0] == 2)
        response_input(m, n);
    else if (m[0] == 4)
        data_input(m, n);
}

int wg_output(const uint8_t *packet, size_t n)
{
    if (n > TUNNEL_MTU)
        return E_RANGE;
    if (wg->keys && net_now() - wg->session_at < REJECT_AFTER) {
        send_data(packet, n);
        if (net_now() - wg->session_at > REKEY_AFTER && !wg->hs_waiting)
            send_initiation();
        return 0;
    }
    if (wg->pending_count == PENDING) {
        kfree(wg->pending[0]);
        memmove(wg->pending, wg->pending + 1, (PENDING - 1) * sizeof(wg->pending[0]));
        memmove(wg->pending_len, wg->pending_len + 1, (PENDING - 1) * sizeof(wg->pending_len[0]));
        wg->pending_count--;
    }
    uint8_t *copy = kmalloc(n);
    if (copy) {
        memcpy(copy, packet, n);
        wg->pending[wg->pending_count] = copy;
        wg->pending_len[wg->pending_count++] = n;
    }
    if (!wg->hs_waiting || net_now() - wg->hs_sent > REKEY_TIMEOUT) {
        wg->hs_tries = 0;
        send_initiation();
    }
    return 0;
}

static void apply_dns(void)
{
    net_set_dns_override(wg && wg->up ? wg->cfg.dns : 0);
}

static void go_up(void)
{
    wg->up = true;
    wg->hs_tries = 0;
    apply_dns();
    send_initiation();
    klog("vpn: %s up through %s:%u", wg->cfg.name, wg->cfg.endpoint, wg->cfg.port);
}

void wg_tick(uint64_t now)
{
    if (!wg || !wg->want)
        return;
    if (!wg->endpoint) {
        if (!net_interface_count())
            return;
        uint32_t ip = 0;
        int err;
        if (!wg->resolving) {
            bool ready = false;
            for (int i = 0; i < net_interface_count(); i++)
                ready |= net_interface(i)->configured || net_v6_info(i)->global_ok;
            if (!ready)
                return;
            err = net_resolve_start(wg->cfg.endpoint, &wg->dns, &ip);
            if (err == 1) {
                wg->resolving = true;
                return;
            }
        } else {
            err = net_resolve_poll(&wg->dns, &ip, 4000);
            if (err == E_AGAIN)
                return;
            wg->resolving = false;
        }
        if (err) {
            ksnprintf(wg->error, sizeof(wg->error), "%s: %s", wg->cfg.endpoint,
                      err == E_NOENT ? "server not found" : err_str(err));
            wg->want = false;
            return;
        }
        wg->endpoint = ip;
        go_up();
        return;
    }
    if (!wg->up)
        return;
    if (wg->keys && now - wg->session_at > REJECT_AFTER + 60000)
        wg->keys = false;
    if (wg->hs_waiting && now - wg->hs_sent > REKEY_TIMEOUT) {
        if (wg->hs_tries < HANDSHAKE_TRIES && (!wg->keys || wg->pending_count || now - wg->last_tx < REKEY_AFTER)) {
            send_initiation();
        } else {
            wg->hs_waiting = false;
            if (!wg->keys)
                str_copy(wg->error, "the VPN server does not answer", sizeof(wg->error));
        }
    }
    if (!wg->keys)
        return;
    if (wg->cfg.keepalive && now - wg->last_tx > (uint64_t)wg->cfg.keepalive * 1000)
        send_data(NULL, 0);
    else if (wg->last_rx > wg->last_tx && now - wg->last_rx > KEEPALIVE_TIMEOUT)
        send_data(NULL, 0);
    if (now - wg->session_at > REKEY_AFTER && !wg->hs_waiting && now - wg->last_tx < REKEY_AFTER)
        send_initiation();
}

/* --- control --- */

int vpn_up(const char *name, char *why, size_t cap)
{
    struct wg_config *c = kmalloc(sizeof(*c));
    if (!c)
        return E_NOMEM;
    int err = load(name, c, why, cap);
    if (err) {
        kfree(c);
        return err;
    }
    vpn_down();
    if (!wg && !(wg = kzalloc(sizeof(*wg)))) {
        kfree(c);
        return E_NOMEM;
    }
    memset(wg, 0, sizeof(*wg));
    wg->cfg = *c;
    kfree(c);
    label_key("mac1----", wg->cfg.pub, wg->mac1_key);
    label_key("mac1----", wg->cfg.peer, wg->mac1_peer_key);
    uint16_t r;
    random_bytes(&r, sizeof(r));
    wg->lport = (uint16_t)(49152 + r % 16000);
    wg->want = true;
    str_copy(wanted, name, sizeof(wanted));
    return 0;
}

void vpn_down(void)
{
    if (!wg)
        return;
    if (wg->resolving)
        net_resolve_cancel(&wg->dns);
    bool was = wg->up;
    for (int i = 0; i < wg->pending_count; i++)
        kfree(wg->pending[i]);
    memset(wg, 0, sizeof(*wg));
    wanted[0] = 0;
    apply_dns();
    if (was)
        klog("vpn: down");
}

void vpn_status(struct vpn_status *st)
{
    memset(st, 0, sizeof(*st));
    if (!wg || !wg->want) {
        if (wg && wg->error[0])
            str_copy(st->error, wg->error, sizeof(st->error));
        return;
    }
    str_copy(st->name, wg->cfg.name, sizeof(st->name));
    st->on = true;
    st->connected = wg->keys && net_now() - wg->session_at < REJECT_AFTER;
    st->address = wg->cfg.addr;
    st->dns = wg->cfg.dns;
    st->endpoint = wg->endpoint;
    st->port = wg->cfg.port;
    str_copy(st->host, wg->cfg.endpoint, sizeof(st->host));
    st->handshake_ago = wg->last_handshake ? (uint32_t)((net_now() - wg->last_handshake) / 1000) : 0;
    st->rx = wg->rx_bytes;
    st->tx = wg->tx_bytes;
    str_copy(st->error, wg->error, sizeof(st->error));
}

int vpn_list(char names[][32], int max)
{
    struct vdir dir;
    struct vfs_dirent e;
    int count = 0;
    if (vfs_opendir(VPN_DIR, &dir))
        return 0;
    while (count < max && vfs_readdir(&dir, &e) == 0) {
        size_t len = strlen(e.name);
        if (len > 5 && !str_casecmp(e.name + len - 5, ".conf") && len - 5 < 32)
            str_copy(names[count++], e.name, len - 4);
    }
    vfs_closedir(&dir);
    return count;
}

int vpn_import(const char *file, const char *name, char *out_name, char *why, size_t cap)
{
    char *text;
    size_t size;
    int err = vfs_read_all(file, &text, &size);
    if (err) {
        ksnprintf(why, cap, "%s: %s", file, err_str(err));
        return err;
    }
    struct wg_config *c = kmalloc(sizeof(*c));
    if (!c) {
        kfree(text);
        return E_NOMEM;
    }
    err = wg_parse(text, c, why, cap);
    kfree(c);
    if (err) {
        kfree(text);
        return err;
    }
    char base[32];
    if (name && *name) {
        str_copy(base, name, sizeof(base));
    } else {
        const char *slash = strrchr(file, '/');
        str_copy(base, slash ? slash + 1 : file, sizeof(base));
        char *dot = strrchr(base, '.');
        if (dot)
            *dot = 0;
    }
    for (char *q = base; *q; q++)
        if (!is_alnum(*q) && *q != '-' && *q != '_')
            *q = '-';
    if (!base[0])
        str_copy(base, "vpn", sizeof(base));
    vfs_mkdirs(VPN_DIR);
    char path[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), "%s/%s.conf", VPN_DIR, base);
    err = vfs_write_all(path, text, size);
    kfree(text);
    if (err) {
        ksnprintf(why, cap, "%s: %s", path, err_str(err));
        return err;
    }
    str_copy(out_name, base, 32);
    return 0;
}

int vpn_remove(const char *name)
{
    if (wg && wg->want && !strcmp(wg->cfg.name, name))
        vpn_down();
    char path[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), "%s/%s.conf", VPN_DIR, name);
    return vfs_unlink(path);
}

static void get_vpn(char *buf, size_t cap)
{
    str_copy(buf, wanted[0] ? wanted : "off", cap);
}

static int set_vpn(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!strcmp(value, "off")) {
        vpn_down();
        return 0;
    }
    char why[96];
    return vpn_up(value, why, sizeof(why));
}

static const struct setting s_vpn = {"vpn", "off|<name>", "the WireGuard VPN that is on (see vpn list)", get_vpn,
                                     set_vpn};
SETTING(s_vpn);

static void print_status(struct shell *sh)
{
    struct vpn_status st;
    vpn_status(&st);
    if (!st.on) {
        sh_printf(sh, "VPN is off%s%s\n", st.error[0] ? ": " : "", st.error);
        return;
    }
    char addr[48], dns[48], ep[48], rx[16], tx[16];
    net_format_ip(st.address, addr, sizeof(addr));
    net_format_ip(st.dns, dns, sizeof(dns));
    net_format_ip(st.endpoint, ep, sizeof(ep));
    fmt_size(rx, sizeof(rx), st.rx);
    fmt_size(tx, sizeof(tx), st.tx);
    sh_printf(sh, "VPN %s: %s\n", st.name, st.connected ? "connected" : st.error[0] ? st.error : "connecting");
    sh_printf(sh, "  address %s, dns %s, server %s", addr, st.dns ? dns : "-", st.host);
    if (strcmp(st.host, ep))
        sh_printf(sh, " (%s)", st.endpoint ? ep : "?");
    sh_printf(sh, " port %u\n", st.port);
    if (st.connected)
        sh_printf(sh, "  handshake %u s ago, received %s, sent %s\n", st.handshake_ago, rx, tx);
}

COMMAND(vpn, NULL, "system",
        "vpn [status | list | up <name> | down | import <file.conf> [name] | remove <name> | key]",
        "WireGuard VPN: import the configuration your VPN service gives, then turn it on")
{
    const char *op = argc > 1 ? argv[1] : "status";
    char why[96], name[32];
    if (!strcmp(op, "status")) {
        print_status(sh);
        return 0;
    }
    if (!strcmp(op, "list")) {
        char names[16][32];
        int n = vpn_list(names, 16);
        if (!n)
            sh_puts(sh, "no VPN configurations (vpn import <file.conf>)\n");
        for (int i = 0; i < n; i++)
            sh_printf(sh, "%s%s\n", names[i], wg && wg->want && !strcmp(wg->cfg.name, names[i]) ? " (on)" : "");
        return 0;
    }
    if (!strcmp(op, "up") && argc == 3) {
        int err = vpn_up(argv[2], why, sizeof(why));
        if (err)
            return sh_error(sh, "vpn", "%s", why);
        uint64_t start = hal_ticks_ms();
        struct vpn_status st;
        do {
            net_wait(5);
            vpn_status(&st);
        } while (!st.connected && !st.error[0] && st.on && hal_ticks_ms() - start < 8000 && !sh_interrupted());
        print_status(sh);
        return st.connected ? 0 : 1;
    }
    if (!strcmp(op, "down")) {
        vpn_down();
        sh_puts(sh, "VPN is off\n");
        return 0;
    }
    if (!strcmp(op, "import") && (argc == 3 || argc == 4)) {
        int err = vpn_import(argv[2], argc == 4 ? argv[3] : NULL, name, why, sizeof(why));
        if (err)
            return sh_error(sh, "vpn", "%s", why);
        sh_printf(sh, "Added VPN %s (vpn up %s turns it on)\n", name, name);
        return 0;
    }
    if (!strcmp(op, "remove") && argc == 3) {
        int err = vpn_remove(argv[2]);
        if (err)
            return sh_error(sh, "vpn", "%s: %s", argv[2], err_str(err));
        return 0;
    }
    if (!strcmp(op, "key")) {
        uint8_t priv[32], pub[32];
        char a[45], b[45];
        random_bytes(priv, 32);
        priv[0] &= 248;
        priv[31] = (uint8_t)((priv[31] & 127) | 64);
        x25519_base(pub, priv);
        key_encode(priv, a);
        key_encode(pub, b);
        sh_printf(sh, "PrivateKey = %s\nPublicKey = %s\n", a, b);
        memset(priv, 0, sizeof(priv));
        return 0;
    }
    return command_usage(sh, command_find("vpn"));
}
