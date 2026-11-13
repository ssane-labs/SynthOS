/* An IKEv2 VPN client (RFC 7296).
 *
 * `ikev2 up SERVER[:PORT] USER PASSWORD [--ca FILE] [--remote-id ID]` logs in with EAP-MSCHAPv2 after checking
 * the server's certificate (the system's roots, or only the CA in FILE); `ikev2 up SERVER[:PORT] --psk SECRET
 * [--id ID] [--remote-id ID]` uses a pre-shared key instead. The client always talks UDP-encapsulated (NAT-T,
 * port 4500 unless given), so it works behind NAT; IKE is AES-CBC with HMAC-SHA-256 or AES-GCM, key exchange
 * Curve25519, P-256 or P-384; ESP is AES-GCM or AES-CBC with HMAC-SHA-256. The server's address, DNS and traffic
 * selectors (what goes through the tunnel) come over the configuration payload; the server can rekey the ESP
 * SA, and dead-peer detection answers both ways. Large replies are taken in IKEv2 fragments (RFC 7383). */
#include <synth/command.h>
#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/sha1.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>
#include <synth/vpn.h>

#include "../internal.h"

/* ---- MD4, DES and MS-CHAPv2 (RFC 2759, RFC 3079) ---- */

static uint32_t rol32(uint32_t x, int s)
{
    return x << s | x >> (32 - s);
}

static void md4(const uint8_t *msg, size_t len, uint8_t out[16])
{
    uint8_t buf[320];
    size_t total = (len + 8) / 64 * 64 + 64;
    if (total > sizeof(buf))
        return;
    memset(buf, 0, total);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        buf[total - 8 + i] = (uint8_t)(bits >> (8 * i));
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    static const uint8_t s1[4] = {3, 7, 11, 19}, s2[4] = {3, 5, 9, 13}, s3[4] = {3, 9, 11, 15};
    static const uint8_t i2[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
    static const uint8_t i3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    for (size_t at = 0; at < total; at += 64) {
        uint32_t x[16], v[4];
        for (int i = 0; i < 16; i++)
            x[i] = (uint32_t)buf[at + 4 * i] | (uint32_t)buf[at + 4 * i + 1] << 8 |
                   (uint32_t)buf[at + 4 * i + 2] << 16 | (uint32_t)buf[at + 4 * i + 3] << 24;
        memcpy(v, h, sizeof(v));
        for (int r = 0; r < 3; r++)
            for (int i = 0; i < 16; i++) {
                int k = (4 - i % 4) % 4;
                uint32_t b = v[(k + 1) % 4], c = v[(k + 2) % 4], d = v[(k + 3) % 4], f;
                if (r == 0)
                    f = (b & c) | (~b & d), f += x[i], v[k] = rol32(v[k] + f, s1[i % 4]);
                else if (r == 1)
                    f = (b & c) | (b & d) | (c & d), f += x[i2[i]] + 0x5a827999, v[k] = rol32(v[k] + f, s2[i % 4]);
                else
                    f = b ^ c ^ d, f += x[i3[i]] + 0x6ed9eba1, v[k] = rol32(v[k] + f, s3[i % 4]);
            }
        for (int i = 0; i < 4; i++)
            h[i] += v[i];
    }
    for (int i = 0; i < 16; i++)
        out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
}

static const uint8_t des_ip[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                   62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                   57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                   61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t des_fp[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                   38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                   36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                   34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
static const uint8_t des_e[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                  12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                  22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const uint8_t des_p[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                  2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const uint8_t des_pc1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                                    10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                                    63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                                    14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const uint8_t des_pc2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                    26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t des_s[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0, 15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5,  3,  8,  4,  1,  14, 8,  13, 6, 2,  11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3,  13, 4,  7,  15, 2,
     8,  14, 12, 0,  1,  10, 6,  9,  11, 5, 0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,
     9,  3,  2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,
     6,  10, 2,  8,  5,  14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12,
     5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2, 8,  5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15,
     0,  3,  4,  7,  2,  12, 1,  10, 14, 9, 10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
     5,  2,  8,  4,  3,  15, 0,  6,  10, 1, 13, 8,  9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5,  0,  15, 10, 3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
     9,  5,  6,  1,  13, 14, 0,  11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
     1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0,  11, 7,  4,  9,
     1,  10, 14, 3,  5,  12, 2,  15, 8,  6, 1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,
     0,  5,  9,  2,  6,  11, 13, 8,  1,  4, 10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,
     7,  4,  12, 5,  6,  11, 0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13,
     15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

/* output bit i (from the top of an n-bit value) is input bit t[i] (1-based from the top of a width-bit value) */
static uint64_t permute(uint64_t in, const uint8_t *t, int n, int width)
{
    uint64_t out = 0;
    for (int i = 0; i < n; i++)
        out = out << 1 | (in >> (width - t[i]) & 1);
    return out;
}

/* one DES block with a 56-bit key given as 7 bytes (MS-CHAP's way) */
static void des_7(const uint8_t key7[7], const uint8_t in[8], uint8_t out[8])
{
    uint64_t k56 = 0, block = 0;
    for (int i = 0; i < 7; i++)
        k56 = k56 << 8 | key7[i];
    uint64_t k64 = 0; /* seven bits per byte, the parity bit left 0 */
    for (int i = 0; i < 8; i++)
        k64 = k64 << 8 | ((k56 >> (49 - 7 * i) & 0x7f) << 1);
    for (int i = 0; i < 8; i++)
        block = block << 8 | in[i];
    uint64_t cd = permute(k64, des_pc1, 56, 64);
    uint32_t c = (uint32_t)(cd >> 28), d = (uint32_t)(cd & 0xfffffff);
    static const uint8_t shifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
    uint64_t sub[16];
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < shifts[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xfffffff;
            d = ((d << 1) | (d >> 27)) & 0xfffffff;
        }
        sub[r] = permute((uint64_t)c << 28 | d, des_pc2, 48, 56);
    }
    block = permute(block, des_ip, 64, 64);
    uint32_t l = (uint32_t)(block >> 32), rr = (uint32_t)block;
    for (int r = 0; r < 16; r++) {
        uint64_t e = permute(rr, des_e, 48, 32) ^ sub[r];
        uint32_t f = 0;
        for (int s = 0; s < 8; s++) {
            int six = (int)(e >> (42 - 6 * s) & 0x3f);
            int row = (six >> 4 & 2) | (six & 1), col = six >> 1 & 0xf;
            f = f << 4 | des_s[s][row * 16 + col];
        }
        f = (uint32_t)permute(f, des_p, 32, 32);
        uint32_t t = l ^ f;
        l = rr;
        rr = t;
    }
    block = permute((uint64_t)rr << 32 | l, des_fp, 64, 64);
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(block >> (56 - 8 * i));
}

static void nt_password_hash(const char *password, uint8_t out[16])
{
    uint8_t u[256];
    size_t n = 0;
    for (const char *p = password; *p && n + 2 <= sizeof(u); p++) {
        u[n++] = (uint8_t)*p; /* UTF-16LE of ASCII; other bytes pass through as Latin-1 */
        u[n++] = 0;
    }
    md4(u, n, out);
}

static void challenge_hash(const uint8_t peer[16], const uint8_t auth[16], const char *user, uint8_t out[8])
{
    struct sha256 s;
    uint8_t d[20];
    sha1_init(&s);
    sha1_update(&s, peer, 16);
    sha1_update(&s, auth, 16);
    sha1_update(&s, user, strlen(user));
    sha1_final(&s, d);
    memcpy(out, d, 8);
}

static void nt_response(const uint8_t peer[16], const uint8_t auth[16], const char *user, const char *password,
                        uint8_t out[24])
{
    uint8_t ch[8], z[21] = {0};
    challenge_hash(peer, auth, user, ch);
    nt_password_hash(password, z);
    for (int i = 0; i < 3; i++)
        des_7(z + 7 * i, ch, out + 8 * i);
}

static const char magic_sign1[] = "Magic server to client signing constant";
static const char magic_sign2[] = "Pad to make it do more than one iteration";

/* the "S=..." the server must send back, as 40 upper-case hex digits */
static void authenticator_response(const char *password, const uint8_t nt[24], const uint8_t peer[16],
                                   const uint8_t auth[16], const char *user, char out[41])
{
    uint8_t ph[16], phh[16], d[20], ch[8];
    struct sha256 s;
    nt_password_hash(password, ph);
    md4(ph, 16, phh);
    sha1_init(&s);
    sha1_update(&s, phh, 16);
    sha1_update(&s, nt, 24);
    sha1_update(&s, magic_sign1, 39);
    sha1_final(&s, d);
    challenge_hash(peer, auth, user, ch);
    sha1_init(&s);
    sha1_update(&s, d, 20);
    sha1_update(&s, ch, 8);
    sha1_update(&s, magic_sign2, 41);
    sha1_final(&s, d);
    for (int i = 0; i < 20; i++) {
        out[2 * i] = "0123456789ABCDEF"[d[i] >> 4];
        out[2 * i + 1] = "0123456789ABCDEF"[d[i] & 15];
    }
    out[40] = 0;
}

/* the 32-byte MSK: the client's send key, then its receive key (RFC 3079's keys, the order IKEv2 servers use) */
static void mschap_msk(const char *password, const uint8_t nt[24], uint8_t msk[32])
{
    static const char master[] = "This is the MPPE Master Key";
    static const char magic2[] = "On the client side, this is the send key; on the server side, it is the receive key.";
    static const char magic3[] = "On the client side, this is the receive key; on the server side, it is the send key.";
    uint8_t ph[16], phh[16], d[20], mk[16], pad1[40], pad2[40];
    struct sha256 s;
    nt_password_hash(password, ph);
    md4(ph, 16, phh);
    sha1_init(&s);
    sha1_update(&s, phh, 16);
    sha1_update(&s, nt, 24);
    sha1_update(&s, master, 27);
    sha1_final(&s, d);
    memcpy(mk, d, 16);
    memset(pad1, 0, 40);
    memset(pad2, 0xf2, 40);
    for (int k = 0; k < 2; k++) {
        sha1_init(&s);
        sha1_update(&s, mk, 16);
        sha1_update(&s, pad1, 40);
        sha1_update(&s, k == 0 ? magic2 : magic3, 84);
        sha1_update(&s, pad2, 40);
        sha1_final(&s, d);
        memcpy(msk + 16 * k, d, 16);
    }
}

/* ---- end of MS-CHAPv2 ---- */

enum { IKE_SA_INIT = 34, IKE_AUTH = 35, CREATE_CHILD_SA = 36, INFORMATIONAL = 37 };
enum {
    PL_SA = 33,
    PL_KE = 34,
    PL_IDI = 35,
    PL_IDR = 36,
    PL_CERT = 37,
    PL_CERTREQ = 38,
    PL_AUTH = 39,
    PL_NONCE = 40,
    PL_NOTIFY = 41,
    PL_DELETE = 42,
    PL_TSI = 44,
    PL_TSR = 45,
    PL_SK = 46,
    PL_CP = 47,
    PL_EAP = 48,
    PL_SKF = 53,
};
enum {
    N_NO_PROPOSAL_CHOSEN = 14,
    N_INVALID_KE_PAYLOAD = 17,
    N_AUTHENTICATION_FAILED = 24,
    N_TS_UNACCEPTABLE = 38,
    N_NAT_SOURCE = 16388,
    N_NAT_DESTINATION = 16389,
    N_COOKIE = 16390,
    N_REKEY_SA = 16393,
    N_FRAGMENTATION = 16430,
    N_SIGNATURE_HASHES = 16431,
};
enum { T_ENCR = 1, T_PRF = 2, T_INTEG = 3, T_DH = 4, T_ESN = 5 };
enum { ENCR_AES_CBC = 12, ENCR_AES_GCM16 = 20, PRF_SHA256 = 5, INTEG_SHA256 = 12 };
enum { DH_P256 = 19, DH_P384 = 20, DH_X25519 = 31 };

#define MSG_MAX 4096
#define MAX_TS 8
#define TUN_MTU 1360

struct msg {
    uint8_t b[MSG_MAX];
    size_t n, next_at;
};

struct payload {
    int type;
    const uint8_t *p;
    size_t n;
};

struct range {
    uint32_t lo, hi;
};

struct range6 {
    uint8_t lo[16], hi[16];
};

struct esp {
    uint8_t spi_in[4], spi_out[4];
    bool gcm;
    size_t key_len;
    uint8_t salt_out[4], salt_in[4], int_out[32], int_in[32];
    struct gcm gcm_out, gcm_in;
    struct aes aes_out, aes_in;
    uint32_t seq_out, recv_max;
    uint64_t bitmap;
};

static struct ike {
    bool up, lost, use_psk;
    char host[128], user[96], pass[96], psk[128], id[128], remote_id[128];
    char *ca;
    size_t ca_len;
    uint32_t server;
    uint16_t port;
    int sock;
    uint8_t spi_i[8], spi_r[8];
    uint32_t next_id;
    /* the key exchange */
    int group;
    uint8_t dh_secret[48];
    uint8_t ni[32], nr[256];
    size_t nr_len;
    uint8_t *init_req, *init_resp;
    size_t init_req_len, init_resp_len;
    uint8_t cookie[256];
    size_t cookie_len;
    bool fragments;
    /* the IKE SA */
    bool ike_gcm;
    size_t ike_key_len;
    uint8_t sk_d[32], sk_ai[32], sk_ar[32], sk_pi[32], sk_pr[32], salt_i[4], salt_r[4];
    struct aes aes_i, aes_r;
    struct gcm gcm_i, gcm_r;
    /* the request in flight */
    uint8_t *req;
    size_t req_len;
    uint32_t req_id;
    uint64_t req_at;
    int req_tries;
    /* a response, or fragments of one */
    struct msg in;
    uint8_t *frag[16];
    size_t frag_len[16];
    int frag_total, frag_first_type;
    uint32_t frag_id;
    /* the tunnel */
    struct esp esp;
    struct esp old; /* the SA a rekey replaced: still taken in until the server deletes it */
    bool has_old, new_confirmed; /* sending stays on the old SA until the new one has carried a packet */
    uint8_t child_ni[32];
    uint32_t addr, dns;
    uint8_t addr6[16];
    bool has6;
    struct range ts[MAX_TS];
    int nts;
    struct range6 ts6[4];
    int nts6;
    uint64_t up_at, last_rx, last_tx, last_ike_rx;
    uint64_t rx_bytes, tx_bytes;
    unsigned rekeys;
    char error[120];
    char cipher_desc[80];
    char expect_s[41]; /* the MS-CHAPv2 success the server must send */
    uint8_t idr[256];
    size_t idr_len;
    /* scratch */
    struct msg out, inner;
} *ike;

static char last_error[120];

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- building messages ---- */

static void msg_put(struct msg *m, const void *p, size_t n)
{
    if (m->n + n > MSG_MAX)
        n = MSG_MAX - m->n;
    memcpy(m->b + m->n, p, n);
    m->n += n;
}

static void msg_byte(struct msg *m, uint32_t v)
{
    uint8_t b = (uint8_t)v;
    msg_put(m, &b, 1);
}

static void msg_16(struct msg *m, uint32_t v)
{
    uint8_t b[2];
    put16(b, v);
    msg_put(m, b, 2);
}

static void msg_32(struct msg *m, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    msg_put(m, b, 4);
}

/* starts a payload: links it from the previous one; returns where its header is */
static size_t pl_begin(struct msg *m, int type)
{
    m->b[m->next_at] = (uint8_t)type;
    size_t at = m->n;
    m->next_at = at;
    msg_32(m, 0);
    return at;
}

static void pl_end(struct msg *m, size_t at)
{
    put16(m->b + at + 2, (uint32_t)(m->n - at));
}

/* the payloads of an encrypted message: b[0] receives the first payload's type */
static void inner_begin(struct msg *m)
{
    m->n = 1;
    m->next_at = 0;
    m->b[0] = 0;
}

static void header(struct msg *m, int exchange, uint32_t id, bool response)
{
    memset(m->b, 0, 28);
    memcpy(m->b, ike->spi_i, 8);
    memcpy(m->b + 8, ike->spi_r, 8);
    m->b[17] = 0x20;
    m->b[18] = (uint8_t)exchange;
    m->b[19] = (uint8_t)(0x08 | (response ? 0x20 : 0));
    put32(m->b + 20, id);
    m->n = 28;
    m->next_at = 16;
}

static void notify(struct msg *m, int protocol, int type, const void *data, size_t n)
{
    size_t at = pl_begin(m, PL_NOTIFY);
    msg_byte(m, protocol);
    msg_byte(m, 0);
    msg_16(m, type);
    msg_put(m, data, n);
    pl_end(m, at);
}

static void transform(struct msg *m, bool last, int type, int id, int key_bits)
{
    msg_byte(m, last ? 0 : 3);
    msg_byte(m, 0);
    msg_16(m, key_bits ? 12 : 8);
    msg_byte(m, type);
    msg_byte(m, 0);
    msg_16(m, id);
    if (key_bits) {
        msg_16(m, 0x800e);
        msg_16(m, key_bits);
    }
}

/* ---- the PRF ---- */

static void prf(const void *key, size_t key_len, const void *data, size_t n, uint8_t out[32])
{
    hmac(&hash_sha256, key, key_len, data, n, out);
}

static void prf_plus(const uint8_t *key, size_t key_len, const uint8_t *seed, size_t seed_len, uint8_t *out,
                     size_t len)
{
    uint8_t t[32];
    size_t have = 0;
    for (int i = 1; have < len; i++) {
        struct hmac m;
        hmac_init(&m, &hash_sha256, key, key_len);
        if (i > 1)
            hmac_update(&m, t, 32);
        hmac_update(&m, seed, seed_len);
        uint8_t c = (uint8_t)i;
        hmac_update(&m, &c, 1);
        hmac_final(&m, t);
        size_t k = MIN((size_t)32, len - have);
        memcpy(out + have, t, k);
        have += k;
    }
}

/* ---- sending ---- */

static void transmit_ike(const uint8_t *p, size_t n)
{
    uint8_t *b = kmalloc(n + 4);
    if (!b)
        return;
    memset(b, 0, 4); /* the non-ESP marker of UDP encapsulation */
    memcpy(b + 4, p, n);
    net_sendto(ike->sock, b, n + 4, ike->server, ike->port);
    kfree(b);
    ike->last_tx = hal_ticks_ms();
}

static void cbc_encrypt(const struct aes *a, uint8_t iv[16], uint8_t *p, size_t n)
{
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t at = 0; at < n; at += 16) {
        for (int k = 0; k < 16; k++)
            p[at + k] ^= prev[k];
        aes_encrypt(a, p + at, p + at);
        memcpy(prev, p + at, 16);
    }
}

static void cbc_decrypt(const struct aes *a, const uint8_t iv[16], uint8_t *p, size_t n)
{
    uint8_t prev[16], save[16];
    memcpy(prev, iv, 16);
    for (size_t at = 0; at < n; at += 16) {
        memcpy(save, p + at, 16);
        aes_decrypt(a, p + at, p + at);
        for (int k = 0; k < 16; k++)
            p[at + k] ^= prev[k];
        memcpy(prev, save, 16);
    }
}

/* wraps inner's payloads in an SK payload after the header already in out */
static void seal(struct msg *out, struct msg *inner)
{
    size_t at = pl_begin(out, PL_SK);
    out->b[at] = inner->b[0];
    size_t plain = inner->n - 1;
    size_t block = ike->ike_gcm ? 1 : 16, iv_len = ike->ike_gcm ? 8 : 16;
    size_t pad = (block - (plain + 1) % block) % block;
    uint8_t *iv = out->b + out->n;
    random_bytes(iv, iv_len);
    out->n += iv_len;
    uint8_t *body = out->b + out->n;
    memcpy(body, inner->b + 1, plain);
    memset(body + plain, 0, pad);
    body[plain + pad] = (uint8_t)pad;
    size_t len = plain + pad + 1;
    out->n += len + 16;
    put16(out->b + at + 2, (uint32_t)(out->n - at));
    put32(out->b + 24, (uint32_t)out->n);
    if (ike->ike_gcm) {
        uint8_t nonce[12];
        memcpy(nonce, ike->salt_i, 4);
        memcpy(nonce + 4, iv, 8);
        /* the associated data: the IKE header and the SK payload's header */
        gcm_seal(&ike->gcm_i, nonce, out->b, at + 4, body, len, body, body + len);
    } else {
        cbc_encrypt(&ike->aes_i, iv, body, len);
        uint8_t mac[32];
        hmac(&hash_sha256, ike->sk_ai, 32, out->b, out->n - 16, mac);
        memcpy(body + len, mac, 16);
    }
}

static void send_request(struct msg *m)
{
    kfree(ike->req);
    ike->req = kmalloc(m->n);
    if (!ike->req)
        return;
    memcpy(ike->req, m->b, m->n);
    ike->req_len = m->n;
    ike->req_id = get32(m->b + 20);
    ike->req_at = hal_ticks_ms();
    ike->req_tries = 1;
    transmit_ike(m->b, m->n);
}

static void request(int exchange, struct msg *inner)
{
    header(&ike->out, exchange, ike->next_id++, false);
    seal(&ike->out, inner);
    send_request(&ike->out);
}

/* ---- receiving ---- */

/* decrypts an SK or SKF payload in place; returns the plaintext and its length */
static const uint8_t *open_sk(uint8_t *msg, size_t msg_len, size_t at, size_t pl_len, size_t skip, size_t *out_len)
{
    size_t iv_len = ike->ike_gcm ? 8 : 16;
    if (pl_len < 4 + skip + iv_len + 16 + 1)
        return NULL;
    uint8_t *iv = msg + at + 4 + skip, *body = iv + iv_len;
    size_t len = pl_len - 4 - skip - iv_len - 16;
    if (ike->ike_gcm) {
        uint8_t nonce[12];
        memcpy(nonce, ike->salt_r, 4);
        memcpy(nonce + 4, iv, 8);
        if (!gcm_open(&ike->gcm_r, nonce, msg, at + 4 + skip, body, len, body, body + len))
            return NULL;
    } else {
        if (len % 16)
            return NULL;
        uint8_t mac[32];
        hmac(&hash_sha256, ike->sk_ar, 32, msg, msg_len - 16, mac);
        if (!crypto_equal(mac, msg + msg_len - 16, 16))
            return NULL;
        cbc_decrypt(&ike->aes_r, iv, body, len);
    }
    size_t pad = body[len - 1];
    if (pad + 1 > len)
        return NULL;
    *out_len = len - pad - 1;
    return body;
}

static int parse_chain(int first, const uint8_t *p, size_t n, struct payload *out, int max)
{
    int count = 0;
    for (int type = first; type && n >= 4 && count < max;) {
        size_t len = get16(p + 2);
        if (len < 4 || len > n)
            break;
        out[count++] = (struct payload){type, p + 4, len - 4};
        type = p[0];
        p += len;
        n -= len;
    }
    return count;
}

static const struct payload *find(const struct payload *pl, int count, int type)
{
    for (int i = 0; i < count; i++)
        if (pl[i].type == type)
            return &pl[i];
    return NULL;
}

/* an error notify among the payloads (0 if none) */
static int error_notify(const struct payload *pl, int count)
{
    for (int i = 0; i < count; i++)
        if (pl[i].type == PL_NOTIFY && pl[i].n >= 4 && get16(pl[i].p + 2) < 16384)
            return (int)get16(pl[i].p + 2);
    return 0;
}

static void drop_fragments(void)
{
    for (int i = 0; i < 16; i++) {
        kfree(ike->frag[i]);
        ike->frag[i] = NULL;
    }
    ike->frag_total = 0;
}

/* An incoming encrypted IKE message: its plaintext payloads go to ike->in (b[0] the first type). Returns false
   when it is not complete yet (fragments) or does not check out. */
static bool decrypt_message(uint8_t *p, size_t n)
{
    if (n < 28 + 4 || get32(p + 24) != n)
        return false;
    int first = p[16];
    size_t at = 28, pl_len = get16(p + 30);
    if (pl_len < 4 || at + pl_len > n)
        return false;
    size_t len;
    const uint8_t *plain;
    if (first == PL_SK) {
        if (!(plain = open_sk(p, n, at, pl_len, 0, &len)))
            return false;
        ike->in.b[0] = p[at];
        ike->in.n = 1;
        msg_put(&ike->in, plain, len);
        return true;
    }
    if (first != PL_SKF || pl_len < 8)
        return false;
    int number = (int)get16(p + at + 4), total = (int)get16(p + at + 6);
    if (number < 1 || total < 1 || number > total || total > 16)
        return false;
    if (!(plain = open_sk(p, n, at, pl_len, 4, &len)))
        return false;
    uint32_t id = get32(p + 20);
    if (ike->frag_total && (ike->frag_id != id || ike->frag_total != total))
        drop_fragments();
    ike->frag_total = total;
    ike->frag_id = id;
    if (number == 1)
        ike->frag_first_type = p[at];
    if (!ike->frag[number - 1] && (ike->frag[number - 1] = kmalloc(len + 1))) {
        memcpy(ike->frag[number - 1], plain, len);
        ike->frag_len[number - 1] = len;
    }
    for (int i = 0; i < total; i++)
        if (!ike->frag[i])
            return false;
    ike->in.b[0] = (uint8_t)ike->frag_first_type;
    ike->in.n = 1;
    for (int i = 0; i < total; i++)
        msg_put(&ike->in, ike->frag[i], ike->frag_len[i]);
    drop_fragments();
    return true;
}

static void esp_input(const uint8_t *p, size_t n);
static void peer_request(uint8_t *p, size_t n);

/* One packet from the server. Returns true when it is the response to the request in flight (left in ike->in, or
   for IKE_SA_INIT the raw message). */
static bool packet_input(uint8_t *p, size_t n)
{
    ike->last_rx = hal_ticks_ms();
    if (n == 1 && p[0] == 0xff)
        return false; /* NAT keepalive */
    if (n >= 8 && get32(p)) {
        esp_input(p, n);
        return false;
    }
    if (n < 4 + 28)
        return false;
    p += 4;
    n -= 4;
    if (memcmp(p, ike->spi_i, 8))
        return false;
    ike->last_ike_rx = hal_ticks_ms();
    bool response = p[19] & 0x20;
    uint32_t id = get32(p + 20);
    if (!response) {
        peer_request(p, n);
        return false;
    }
    if (!ike->req || id != ike->req_id)
        return false;
    if (p[18] == IKE_SA_INIT) {
        if (n > MSG_MAX)
            return false;
        memcpy(ike->in.b, p, n);
        ike->in.n = n;
        return true;
    }
    return decrypt_message(p, n);
}

/* reads what has come; true when the response to the request in flight is in */
static bool pump(void)
{
    static uint8_t buf[2048];
    bool answered = false;
    for (int rounds = 0; rounds < 64 && !answered; rounds++) {
        uint32_t from;
        uint16_t port;
        long k = net_recvfrom(ike->sock, buf, sizeof(buf), &from, &port);
        if (k < 0)
            break;
        if (from == ike->server && packet_input(buf, (size_t)k)) {
            answered = true;
            kfree(ike->req);
            ike->req = NULL;
        }
    }
    uint64_t now = hal_ticks_ms();
    if (ike->req && now - ike->req_at > 2000u * (uint64_t)ike->req_tries && ike->req_tries < 6) {
        transmit_ike(ike->req, ike->req_len);
        ike->req_at = now;
        ike->req_tries++;
    }
    return answered;
}

/* waits for the response to the request in flight */
static bool await(int timeout_ms)
{
    uint64_t start = hal_ticks_ms();
    while (hal_ticks_ms() - start < (uint64_t)timeout_ms) {
        if (pump())
            return true;
        if (!ike->req)
            return false;
        net_wait(5);
    }
    return false;
}

/* ---- the key exchange ---- */

static size_t dh_public(uint8_t *out)
{
    random_bytes(ike->dh_secret, sizeof(ike->dh_secret));
    if (ike->group == DH_X25519) {
        x25519_base(out, ike->dh_secret);
        return 32;
    }
    enum ec_curve c = ike->group == DH_P256 ? EC_P256 : EC_P384;
    size_t size = ec_size(c);
    uint8_t point[97];
    ike->dh_secret[0] &= 0x7f; /* below the order */
    if (!ec_public_key(c, ike->dh_secret, point))
        return 0;
    memcpy(out, point + 1, 2 * size); /* IKE sends x | y */
    return 2 * size;
}

static size_t dh_shared(const uint8_t *peer, size_t n, uint8_t *out)
{
    if (ike->group == DH_X25519) {
        if (n != 32)
            return 0;
        x25519(out, ike->dh_secret, peer);
        return 32;
    }
    enum ec_curve c = ike->group == DH_P256 ? EC_P256 : EC_P384;
    size_t size = ec_size(c);
    uint8_t point[97];
    if (n != 2 * size)
        return 0;
    point[0] = 4;
    memcpy(point + 1, peer, n);
    return ec_shared_secret(c, ike->dh_secret, point, n + 1, out) ? size : 0;
}

static void nat_hash(const uint8_t *spi_r, uint32_t ip, uint16_t port, uint8_t out[20])
{
    uint8_t b[22];
    memcpy(b, ike->spi_i, 8);
    memcpy(b + 8, spi_r, 8);
    put32(b + 16, ip);
    put16(b + 20, port);
    sha1(b, 22, out);
}

static void build_init(struct msg *m)
{
    static const uint8_t zero[8];
    header(m, IKE_SA_INIT, 0, false);
    memset(m->b + 8, 0, 8);
    if (ike->cookie_len)
        notify(m, 0, N_COOKIE, ike->cookie, ike->cookie_len);
    size_t sa = pl_begin(m, PL_SA);
    msg_byte(m, 2); /* more proposals */
    msg_byte(m, 0);
    size_t p1 = m->n;
    msg_16(m, 0);
    msg_byte(m, 1);
    msg_byte(m, 1); /* IKE */
    msg_byte(m, 0);
    msg_byte(m, 7);
    transform(m, false, T_ENCR, ENCR_AES_CBC, 256);
    transform(m, false, T_ENCR, ENCR_AES_CBC, 128);
    transform(m, false, T_PRF, PRF_SHA256, 0);
    transform(m, false, T_INTEG, INTEG_SHA256, 0);
    transform(m, false, T_DH, DH_X25519, 0);
    transform(m, false, T_DH, DH_P256, 0);
    transform(m, true, T_DH, DH_P384, 0);
    put16(m->b + p1, (uint32_t)(m->n - p1 + 2));
    msg_byte(m, 0); /* the AEAD proposal */
    msg_byte(m, 0);
    size_t p2 = m->n;
    msg_16(m, 0);
    msg_byte(m, 2);
    msg_byte(m, 1);
    msg_byte(m, 0);
    msg_byte(m, 6);
    transform(m, false, T_ENCR, ENCR_AES_GCM16, 256);
    transform(m, false, T_ENCR, ENCR_AES_GCM16, 128);
    transform(m, false, T_PRF, PRF_SHA256, 0);
    transform(m, false, T_DH, DH_X25519, 0);
    transform(m, false, T_DH, DH_P256, 0);
    transform(m, true, T_DH, DH_P384, 0);
    put16(m->b + p2, (uint32_t)(m->n - p2 + 2));
    pl_end(m, sa);
    size_t ke = pl_begin(m, PL_KE);
    msg_16(m, (uint32_t)ike->group);
    msg_16(m, 0);
    uint8_t pub[96];
    msg_put(m, pub, dh_public(pub));
    pl_end(m, ke);
    size_t nonce = pl_begin(m, PL_NONCE);
    msg_put(m, ike->ni, 32);
    pl_end(m, nonce);
    /* a source hash that can't match, so the server always encapsulates (the client is behind NAT anyway) */
    uint8_t h[20];
    random_bytes(h, 20);
    notify(m, 0, N_NAT_SOURCE, h, 20);
    nat_hash(zero, ike->server, ike->port, h);
    notify(m, 0, N_NAT_DESTINATION, h, 20);
    notify(m, 0, N_FRAGMENTATION, NULL, 0);
    static const uint8_t hashes[] = {0, 2, 0, 3, 0, 4}; /* SHA-256, -384, -512 */
    notify(m, 0, N_SIGNATURE_HASHES, hashes, sizeof(hashes));
    put32(m->b + 24, (uint32_t)m->n);
}

/* reads the transforms the server chose */
static bool take_ike_sa(const struct payload *sa)
{
    if (!sa || sa->n < 8)
        return false;
    const uint8_t *p = sa->p;
    size_t len = get16(p + 2);
    if (len > sa->n || p[5] != 1)
        return false;
    int count = p[7];
    size_t at = 8 + p[6];
    ike->ike_key_len = 0;
    int group = 0;
    bool prf_ok = false, integ_ok = false;
    for (int i = 0; i < count && at + 8 <= len; i++) {
        size_t tl = get16(p + at + 2);
        int type = p[at + 4], id = (int)get16(p + at + 6);
        int bits = tl >= 12 && get16(p + at + 8) == 0x800e ? (int)get16(p + at + 10) : 0;
        if (type == T_ENCR && (id == ENCR_AES_CBC || id == ENCR_AES_GCM16) && (bits == 128 || bits == 256)) {
            ike->ike_gcm = id == ENCR_AES_GCM16;
            ike->ike_key_len = (size_t)bits / 8;
        } else if (type == T_PRF && id == PRF_SHA256) {
            prf_ok = true;
        } else if (type == T_INTEG && id == INTEG_SHA256) {
            integ_ok = true;
        } else if (type == T_DH) {
            group = id;
        }
        if (tl < 8)
            return false;
        at += tl;
    }
    return ike->ike_key_len && prf_ok && (integ_ok || ike->ike_gcm) && group == ike->group;
}

static bool derive_ike_keys(const uint8_t *shared, size_t shared_len)
{
    uint8_t nonces[32 + 256], skeyseed[32];
    memcpy(nonces, ike->ni, 32);
    memcpy(nonces + 32, ike->nr, ike->nr_len);
    size_t nn = 32 + ike->nr_len;
    prf(nonces, nn, shared, shared_len, skeyseed);
    uint8_t seed[32 + 256 + 16];
    memcpy(seed, nonces, nn);
    memcpy(seed + nn, ike->spi_i, 8);
    memcpy(seed + nn + 8, ike->spi_r, 8);
    size_t ek = ike->ike_key_len + (ike->ike_gcm ? 4 : 0), ak = ike->ike_gcm ? 0 : 32;
    uint8_t keys[32 * 5 + 2 * 36];
    size_t total = 32 + 2 * ak + 2 * ek + 64;
    prf_plus(skeyseed, 32, seed, nn + 16, keys, total);
    const uint8_t *k = keys;
    memcpy(ike->sk_d, k, 32);
    k += 32;
    memcpy(ike->sk_ai, k, ak);
    k += ak;
    memcpy(ike->sk_ar, k, ak);
    k += ak;
    if (ike->ike_gcm) {
        gcm_init(&ike->gcm_i, k, ike->ike_key_len);
        memcpy(ike->salt_i, k + ike->ike_key_len, 4);
        k += ek;
        gcm_init(&ike->gcm_r, k, ike->ike_key_len);
        memcpy(ike->salt_r, k + ike->ike_key_len, 4);
        k += ek;
    } else {
        aes_init(&ike->aes_i, k, ike->ike_key_len);
        k += ek;
        aes_init(&ike->aes_r, k, ike->ike_key_len);
        k += ek;
    }
    memcpy(ike->sk_pi, k, 32);
    memcpy(ike->sk_pr, k + 32, 32);
    memset(keys, 0, sizeof(keys));
    memset(skeyseed, 0, sizeof(skeyseed));
    return true;
}

static int fail(const char *fmt, const char *arg, int err);

static const char *notify_text(int type)
{
    switch (type) {
    case N_NO_PROPOSAL_CHOSEN:
        return "the server accepts none of the offered ciphers";
    case N_AUTHENTICATION_FAILED:
        return "the user name, password or key was not accepted";
    case N_TS_UNACCEPTABLE:
        return "the server refused the traffic selectors";
    case 36:
        return "the server has no address to give";
    case 37:
        return "the server requires a configuration request";
    default:
        return "the server refused the connection";
    }
}

static int sa_init(void)
{
    static const int groups[] = {DH_X25519, DH_P256, DH_P384};
    ike->group = DH_X25519;
    random_bytes(ike->ni, 32);
    for (int attempt = 0; attempt < 5; attempt++) {
        build_init(&ike->out);
        send_request(&ike->out);
        if (!await(12000))
            return fail("%s did not answer (check the address and the port)", ike->host, E_TIMEDOUT);
        struct payload pl[16];
        uint8_t *p = ike->in.b;
        int count = parse_chain(p[16], p + 28, ike->in.n - 28, pl, 16);
        const struct payload *cookie = NULL, *ke_hint = NULL;
        for (int i = 0; i < count; i++)
            if (pl[i].type == PL_NOTIFY && pl[i].n >= 4) {
                int t = (int)get16(pl[i].p + 2);
                if (t == N_COOKIE)
                    cookie = &pl[i];
                else if (t == N_INVALID_KE_PAYLOAD)
                    ke_hint = &pl[i];
                else if (t == N_FRAGMENTATION)
                    ike->fragments = true;
            }
        if (cookie && cookie->n - 4 <= sizeof(ike->cookie)) {
            ike->cookie_len = cookie->n - 4;
            memcpy(ike->cookie, cookie->p + 4, ike->cookie_len);
            continue;
        }
        if (ke_hint && ke_hint->n >= 6) {
            int g = (int)get16(ke_hint->p + 4);
            bool known = false;
            for (size_t i = 0; i < ARRAY_SIZE(groups); i++)
                known |= groups[i] == g;
            if (!known || g == ike->group)
                return fail("%s", "the server wants a key exchange this client does not do", E_NOSYS);
            ike->group = g;
            continue;
        }
        int err = error_notify(pl, count);
        if (err)
            return fail("%s", notify_text(err), E_PERM);
        const struct payload *ke = find(pl, count, PL_KE), *nr = find(pl, count, PL_NONCE);
        if (!take_ike_sa(find(pl, count, PL_SA)) || !ke || ke->n < 4 || !nr || nr->n < 16 || nr->n > 256)
            return fail("%s", "the server's answer to the key exchange did not make sense", E_IO);
        memcpy(ike->spi_r, p + 8, 8);
        memcpy(ike->nr, nr->p, nr->n);
        ike->nr_len = nr->n;
        uint8_t shared[48];
        size_t sl = dh_shared(ke->p + 4, ke->n - 4, shared);
        if (!sl)
            return fail("%s", "the server's key share is not valid", E_IO);
        derive_ike_keys(shared, sl);
        memset(shared, 0, sizeof(shared));
        ike->init_req = kmalloc(ike->out.n);
        ike->init_resp = kmalloc(ike->in.n);
        if (!ike->init_req || !ike->init_resp)
            return fail("%s", "out of memory", E_NOMEM);
        memcpy(ike->init_req, ike->out.b, ike->out.n);
        ike->init_req_len = ike->out.n;
        memcpy(ike->init_resp, ike->in.b, ike->in.n);
        ike->init_resp_len = ike->in.n;
        ike->next_id = 1;
        return 0;
    }
    return fail("%s", "the key exchange did not settle", E_IO);
}

/* ---- authentication ---- */

static size_t id_payload(struct msg *m, int type, const char *id)
{
    size_t at = pl_begin(m, type);
    uint32_t ip;
    if (net_parse_ip(id, &ip)) {
        msg_byte(m, 1); /* ID_IPV4_ADDR */
        msg_byte(m, 0);
        msg_16(m, 0);
        msg_32(m, ip);
    } else {
        msg_byte(m, strchr(id, '@') ? 3 : 2); /* ID_RFC822_ADDR / ID_FQDN */
        msg_byte(m, 0);
        msg_16(m, 0);
        msg_put(m, id, strlen(id));
    }
    pl_end(m, at);
    return at;
}

/* the octets an AUTH payload covers: the sender's first message, the other side's nonce, prf(SK_p, ID) */
static void signed_octets(bool ours, const uint8_t *id_body, size_t id_len, uint8_t **out, size_t *out_len)
{
    const uint8_t *first = ours ? ike->init_req : ike->init_resp;
    size_t first_len = ours ? ike->init_req_len : ike->init_resp_len;
    const uint8_t *nonce = ours ? ike->nr : ike->ni;
    size_t nonce_len = ours ? ike->nr_len : 32;
    *out_len = first_len + nonce_len + 32;
    *out = kmalloc(*out_len);
    if (!*out)
        return;
    memcpy(*out, first, first_len);
    memcpy(*out + first_len, nonce, nonce_len);
    prf(ours ? ike->sk_pi : ike->sk_pr, 32, id_body, id_len, *out + first_len + nonce_len);
}

static void shared_key_auth(const uint8_t *key, size_t key_len, const uint8_t *octets, size_t n, uint8_t out[32])
{
    uint8_t padded[32];
    prf(key, key_len, "Key Pad for IKEv2", 17, padded);
    prf(padded, 32, octets, n, out);
}

static void esp_proposals(struct msg *m)
{
    size_t sa = pl_begin(m, PL_SA);
    for (int k = 0; k < 2; k++) {
        msg_byte(m, k ? 0 : 2);
        msg_byte(m, 0);
        size_t at = m->n;
        msg_16(m, 0);
        msg_byte(m, k + 1);
        msg_byte(m, 3); /* ESP */
        msg_byte(m, 4);
        msg_byte(m, k ? 4 : 3);
        msg_put(m, ike->esp.spi_in, 4);
        if (k == 0) {
            transform(m, false, T_ENCR, ENCR_AES_GCM16, 256);
            transform(m, false, T_ENCR, ENCR_AES_GCM16, 128);
        } else {
            transform(m, false, T_ENCR, ENCR_AES_CBC, 256);
            transform(m, false, T_ENCR, ENCR_AES_CBC, 128);
            transform(m, false, T_INTEG, INTEG_SHA256, 0);
        }
        transform(m, true, T_ESN, 0, 0);
        put16(m->b + at, (uint32_t)(m->n - at + 2));
    }
    pl_end(m, sa);
}

static void selectors(struct msg *m, int type)
{
    static const uint8_t v4[16] = {7, 0, 0, 16, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
    uint8_t v6[40] = {8, 0, 0, 40, 0, 0, 0xff, 0xff};
    memset(v6 + 24, 0xff, 16);
    size_t at = pl_begin(m, type);
    msg_byte(m, 2);
    msg_16(m, 0);
    msg_byte(m, 0);
    msg_put(m, v4, 16);
    msg_put(m, v6, 40);
    pl_end(m, at);
}

static void config_request(struct msg *m)
{
    size_t at = pl_begin(m, PL_CP);
    msg_byte(m, 1); /* CFG_REQUEST */
    msg_byte(m, 0);
    msg_16(m, 0);
    static const int attrs[] = {1, 3, 8, 10}; /* IPv4 address and DNS, IPv6 address and DNS */
    for (size_t i = 0; i < ARRAY_SIZE(attrs); i++) {
        msg_16(m, (uint32_t)attrs[i]);
        msg_16(m, 0);
    }
    pl_end(m, at);
}

/* the certificates the server sent: the first is its own, then intermediates; checked up to a trusted root */
struct certs {
    uint8_t *der[6];
    size_t len[6];
    struct x509 cert[6];
    int count;
    uint8_t *ca_der[4];
    size_t ca_len[4];
    struct x509 ca[4];
    int nca;
};

static void add_ca(const uint8_t *der, size_t len, void *ctx)
{
    struct certs *c = ctx;
    if (c->nca >= 4 || !(c->ca_der[c->nca] = kmalloc(len)))
        return;
    memcpy(c->ca_der[c->nca], der, len);
    c->ca_len[c->nca] = len;
    if (x509_parse(c->ca_der[c->nca], len, &c->ca[c->nca]))
        c->nca++;
    else
        kfree(c->ca_der[c->nca]);
}

static void free_certs(struct certs *c)
{
    for (int i = 0; i < c->count; i++)
        kfree(c->der[i]);
    for (int i = 0; i < c->nca; i++)
        kfree(c->ca_der[i]);
}

static const struct x509 *find_issuer(struct certs *c, const struct x509 *child)
{
    for (int i = 0; i < c->nca; i++)
        if (c->ca[i].subject_len == child->issuer_len && !memcmp(c->ca[i].subject, child->issuer, child->issuer_len))
            return &c->ca[i];
    if (!ike->ca)
        return tls_find_root(child->issuer, child->issuer_len, child);
    return NULL;
}

static const char *check_chain(struct certs *c)
{
    if (!c->count)
        return "the server sent no certificate";
    if (ike->ca)
        x509_load_pem(ike->ca, ike->ca_len, add_ca, c);
    else
        tls_roots_load();
    int64_t now = x509_now();
    const struct x509 *cert = &c->cert[0];
    const char *name = ike->remote_id[0] ? ike->remote_id : ike->host;
    if (!x509_matches_host(cert, name))
        return "the server's certificate is for another name";
    for (int depth = 0; depth < 6; depth++) {
        if (now > 0 && (now < cert->not_before || now > cert->not_after))
            return "a certificate of the server has expired";
        const struct x509 *issuer = find_issuer(c, cert);
        if (issuer)
            return x509_verify_signature(cert, issuer) ? NULL : "the server's certificate has a bad signature";
        const struct x509 *next = NULL;
        for (int i = 1; i < c->count && !next; i++)
            if (c->cert[i].subject_len == cert->issuer_len && !memcmp(c->cert[i].subject, cert->issuer, cert->issuer_len))
                next = &c->cert[i];
        if (!next || !x509_verify_signature(cert, next))
            return "the server's certificate is not trusted";
        cert = next;
    }
    return "the server's certificate chain is too long";
}

/* reads a DER INTEGER at *p (for ECDSA signatures in method 14) */
static bool der_int(const uint8_t **p, const uint8_t *end, const uint8_t **v, size_t *n)
{
    if (end - *p < 2 || (*p)[0] != 2 || (*p)[1] & 0x80 || (size_t)(end - *p - 2) < (*p)[1])
        return false;
    *n = (*p)[1];
    *v = *p + 2;
    *p += 2 + *n;
    while (*n > 1 && !**v) {
        (*v)++;
        (*n)--;
    }
    return true;
}

static bool oid_in(const uint8_t *alg, size_t len, const uint8_t *oid, size_t oid_len)
{
    for (size_t i = 0; i + oid_len <= len; i++)
        if (!memcmp(alg + i, oid, oid_len))
            return true;
    return false;
}

/* checks the server's AUTH payload with its certificate */
static bool verify_signature(const struct x509 *cert, const uint8_t *auth, size_t n, const uint8_t *octets,
                             size_t octets_len)
{
    int method = auth[0];
    const uint8_t *sig = auth + 4;
    size_t sig_len = n - 4;
    const struct hash_alg *h = &hash_sha256;
    bool pss = false, der = false;
    if (method == 1) {
        h = &hash_sha1;
    } else if (method == 9 || method == 10 || method == 11) {
        h = method == 9 ? &hash_sha256 : method == 10 ? &hash_sha384 : &hash_sha512;
    } else if (method == 14) {
        if (sig_len < 1 || sig[0] + 1u > sig_len)
            return false;
        const uint8_t *alg = sig + 1;
        size_t alg_len = sig[0];
        static const uint8_t pkcs1[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01};
        static const uint8_t ecdsa[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03};
        static const uint8_t sha2[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02};
        if (oid_in(alg, alg_len, pkcs1, sizeof(pkcs1)) && alg_len > 12) {
            int which = -1;
            for (size_t i = 0; i + sizeof(pkcs1) < alg_len; i++)
                if (!memcmp(alg + i, pkcs1, sizeof(pkcs1)))
                    which = alg[i + sizeof(pkcs1)];
            pss = which == 10;
            h = which == 12 ? &hash_sha384 : which == 13 ? &hash_sha512 : &hash_sha256;
            if (pss) {
                for (size_t i = 0; i + sizeof(sha2) < alg_len; i++)
                    if (!memcmp(alg + i, sha2, sizeof(sha2)))
                        h = alg[i + sizeof(sha2)] == 2 ? &hash_sha384 : alg[i + sizeof(sha2)] == 3 ? &hash_sha512 : &hash_sha256;
            }
        } else if (oid_in(alg, alg_len, ecdsa, sizeof(ecdsa))) {
            der = true;
            for (size_t i = 0; i + sizeof(ecdsa) < alg_len; i++)
                if (!memcmp(alg + i, ecdsa, sizeof(ecdsa)))
                    h = alg[i + sizeof(ecdsa)] == 3 ? &hash_sha384 : alg[i + sizeof(ecdsa)] == 4 ? &hash_sha512 : &hash_sha256;
        } else {
            return false;
        }
        sig += 1 + alg_len;
        sig_len -= 1 + alg_len;
    } else {
        return false;
    }
    uint8_t digest[64];
    hash_digest(h, octets, octets_len, digest);
    if (cert->key == PK_RSA)
        return pss ? rsa_verify_pss(cert->rsa_n, cert->rsa_n_len, cert->rsa_e, cert->rsa_e_len, h, digest, sig, sig_len)
                   : rsa_verify_pkcs1(cert->rsa_n, cert->rsa_n_len, cert->rsa_e, cert->rsa_e_len, h, digest, sig,
                                      sig_len);
    if (cert->key != PK_EC_P256 && cert->key != PK_EC_P384)
        return false;
    enum ec_curve c = cert->key == PK_EC_P256 ? EC_P256 : EC_P384;
    const uint8_t *r = sig, *s = sig + sig_len / 2;
    size_t rl = sig_len / 2, sl = sig_len / 2;
    if (der) {
        const uint8_t *p = sig, *end = sig + sig_len;
        if (sig_len < 2 || p[0] != 0x30)
            return false;
        p += 2;
        if (!der_int(&p, end, &r, &rl) || !der_int(&p, end, &s, &sl))
            return false;
    }
    return ecdsa_verify(c, cert->ec_point, cert->ec_point_len, digest, h->digest, r, rl, s, sl);
}

/* checks the server's identity and AUTH in a reply; key is the PSK or the EAP MSK, or NULL for a signature */
static const char *check_server(const struct payload *pl, int count, const uint8_t *key, size_t key_len)
{
    const struct payload *idr = find(pl, count, PL_IDR), *auth = find(pl, count, PL_AUTH);
    if (idr && idr->n <= sizeof(ike->idr)) { /* the last EAP round's reply does not repeat it */
        memcpy(ike->idr, idr->p, idr->n);
        ike->idr_len = idr->n;
    }
    if (!ike->idr_len || !auth || auth->n < 5)
        return "the server did not prove who it is";
    uint8_t *octets;
    size_t n;
    signed_octets(false, ike->idr, ike->idr_len, &octets, &n);
    if (!octets)
        return "out of memory";
    const char *why = NULL;
    if (key) {
        uint8_t mac[32];
        shared_key_auth(key, key_len, octets, n, mac);
        if (auth->p[0] != 2 || auth->n - 4 != 32 || !crypto_equal(mac, auth->p + 4, 32))
            why = ike->use_psk ? "the server does not know the pre-shared key" : "the server did not confirm the login";
    } else {
        struct certs *c = kzalloc(sizeof(*c));
        if (!c) {
            kfree(octets);
            return "out of memory";
        }
        for (int i = 0; i < count && c->count < 6; i++)
            if (pl[i].type == PL_CERT && pl[i].n > 1 && pl[i].p[0] == 4 && (c->der[c->count] = kmalloc(pl[i].n - 1))) {
                memcpy(c->der[c->count], pl[i].p + 1, pl[i].n - 1);
                c->len[c->count] = pl[i].n - 1;
                if (x509_parse(c->der[c->count], c->len[c->count], &c->cert[c->count]))
                    c->count++;
                else
                    kfree(c->der[c->count]);
            }
        why = check_chain(c);
        if (!why && !verify_signature(&c->cert[0], auth->p, auth->n, octets, n))
            why = "the server's signature does not check out";
        free_certs(c);
        kfree(c);
    }
    kfree(octets);
    return why;
}

/* ---- the child SA ---- */

static bool take_esp(const struct payload *sa, struct esp *e)
{
    if (!sa || sa->n < 12)
        return false;
    const uint8_t *p = sa->p;
    size_t len = get16(p + 2);
    if (len > sa->n || p[5] != 3 || p[6] != 4)
        return false;
    memcpy(e->spi_out, p + 8, 4);
    int count = p[7];
    size_t at = 12;
    e->key_len = 0;
    bool integ = false;
    for (int i = 0; i < count && at + 8 <= len; i++) {
        size_t tl = get16(p + at + 2);
        int type = p[at + 4], id = (int)get16(p + at + 6);
        int bits = tl >= 12 && get16(p + at + 8) == 0x800e ? (int)get16(p + at + 10) : 0;
        if (type == T_ENCR && (id == ENCR_AES_GCM16 || id == ENCR_AES_CBC) && (bits == 128 || bits == 256)) {
            e->gcm = id == ENCR_AES_GCM16;
            e->key_len = (size_t)bits / 8;
        } else if (type == T_INTEG && id == INTEG_SHA256) {
            integ = true;
        } else if (type == T_ESN && id != 0) {
            return false;
        }
        if (tl < 8)
            return false;
        at += tl;
    }
    return e->key_len && (e->gcm || integ);
}

/* KEYMAT = prf+(SK_d, Ni | Nr): first the initiator's direction. we_initiated: this side began the exchange. */
static void esp_keys(struct esp *e, const uint8_t *ni, size_t ni_len, const uint8_t *nr, size_t nr_len, bool we_initiated)
{
    uint8_t seed[512], keys[2 * (36 + 32)];
    memcpy(seed, ni, ni_len);
    memcpy(seed + ni_len, nr, nr_len);
    size_t ek = e->key_len + (e->gcm ? 4 : 0), ak = e->gcm ? 0 : 32;
    prf_plus(ike->sk_d, 32, seed, ni_len + nr_len, keys, 2 * (ek + ak));
    const uint8_t *first = keys, *second = keys + ek + ak;
    const uint8_t *out = we_initiated ? first : second, *in = we_initiated ? second : first;
    if (e->gcm) {
        gcm_init(&e->gcm_out, out, e->key_len);
        memcpy(e->salt_out, out + e->key_len, 4);
        gcm_init(&e->gcm_in, in, e->key_len);
        memcpy(e->salt_in, in + e->key_len, 4);
    } else {
        aes_init(&e->aes_out, out, e->key_len);
        memcpy(e->int_out, out + ek, 32);
        aes_init(&e->aes_in, in, e->key_len);
        memcpy(e->int_in, in + ek, 32);
    }
    memset(keys, 0, sizeof(keys));
}

static void take_selectors(const struct payload *tsr)
{
    ike->nts = ike->nts6 = 0;
    if (!tsr || tsr->n < 4)
        return;
    const uint8_t *p = tsr->p + 4, *end = tsr->p + tsr->n;
    for (int i = 0; i < tsr->p[0] && end - p >= 8; i++) {
        size_t len = get16(p + 2);
        if (len < 8 || (size_t)(end - p) < len)
            break;
        if (p[0] == 7 && len == 16 && ike->nts < MAX_TS)
            ike->ts[ike->nts++] = (struct range){get32(p + 8), get32(p + 12)};
        else if (p[0] == 8 && len == 40 && ike->nts6 < 4) {
            memcpy(ike->ts6[ike->nts6].lo, p + 8, 16);
            memcpy(ike->ts6[ike->nts6++].hi, p + 24, 16);
        }
        p += len;
    }
}

static void take_config(const struct payload *cp)
{
    if (!cp || cp->n < 4 || cp->p[0] != 2)
        return;
    const uint8_t *p = cp->p + 4, *end = cp->p + cp->n;
    while (end - p >= 4) {
        int type = (int)(get16(p) & 0x7fff);
        size_t len = get16(p + 2);
        if ((size_t)(end - p - 4) < len)
            break;
        const uint8_t *v = p + 4;
        if (type == 1 && len == 4 && !ike->addr)
            ike->addr = get32(v);
        else if (type == 3 && len == 4 && !ike->dns)
            ike->dns = get32(v);
        else if (type == 8 && len == 17 && !ike->has6) {
            memcpy(ike->addr6, v, 16);
            ike->has6 = true;
        }
        p += 4 + len;
    }
}

/* ---- ESP ---- */

static void esp_output(const uint8_t *packet, size_t n)
{
    struct esp *e = ike->has_old && !ike->new_confirmed ? &ike->old : &ike->esp;
    bool v6 = n && packet[0] >> 4 == 6;
    size_t block = e->gcm ? 4 : 16, iv_len = e->gcm ? 8 : 16;
    size_t pad = (block - (n + 2) % block) % block;
    size_t body = n + pad + 2;
    uint8_t *out = kmalloc(8 + iv_len + body + 16);
    if (!out)
        return;
    memcpy(out, e->spi_out, 4);
    uint32_t seq = ++e->seq_out;
    put32(out + 4, seq);
    uint8_t *iv = out + 8, *ct = out + 8 + iv_len;
    memcpy(ct, packet, n);
    for (size_t i = 0; i < pad; i++)
        ct[n + i] = (uint8_t)(i + 1);
    ct[n + pad] = (uint8_t)pad;
    ct[n + pad + 1] = v6 ? 41 : 4;
    if (e->gcm) {
        memset(iv, 0, 4);
        put32(iv + 4, seq); /* a counter: never repeats under one key */
        uint8_t nonce[12];
        memcpy(nonce, e->salt_out, 4);
        memcpy(nonce + 4, iv, 8);
        gcm_seal(&e->gcm_out, nonce, out, 8, ct, body, ct, ct + body);
    } else {
        random_bytes(iv, 16);
        cbc_encrypt(&e->aes_out, iv, ct, body);
        uint8_t mac[32];
        hmac(&hash_sha256, e->int_out, 32, out, 8 + iv_len + body, mac);
        memcpy(ct + body, mac, 16);
    }
    net_sendto(ike->sock, out, 8 + iv_len + body + 16, ike->server, ike->port);
    ike->last_tx = hal_ticks_ms();
    ike->tx_bytes += n;
    kfree(out);
}

static bool esp_replay_ok(struct esp *e, uint32_t seq)
{
    if (seq > e->recv_max) {
        uint32_t shift = seq - e->recv_max;
        e->bitmap = shift >= 64 ? 0 : e->bitmap << shift;
        e->bitmap |= 1;
        e->recv_max = seq;
        return true;
    }
    uint32_t back = e->recv_max - seq;
    if (back >= 64 || e->bitmap >> back & 1)
        return false;
    e->bitmap |= 1ull << back;
    return true;
}

static void esp_input(const uint8_t *p, size_t n)
{
    struct esp *e = &ike->esp;
    if (ike->has_old && !memcmp(p, ike->old.spi_in, 4))
        e = &ike->old;
    if (!ike->up || memcmp(p, e->spi_in, 4))
        return;
    size_t iv_len = e->gcm ? 8 : 16;
    if (n < 8 + iv_len + 2 + 16)
        return;
    size_t body = n - 8 - iv_len - 16;
    uint32_t seq = get32(p + 4);
    uint8_t *plain = kmalloc(body + 1);
    if (!plain)
        return;
    bool ok;
    if (e->gcm) {
        uint8_t nonce[12];
        memcpy(nonce, e->salt_in, 4);
        memcpy(nonce + 4, p + 8, 8);
        ok = gcm_open(&e->gcm_in, nonce, p, 8, p + 8 + iv_len, body, plain, p + n - 16);
    } else {
        uint8_t mac[32];
        hmac(&hash_sha256, e->int_in, 32, p, n - 16, mac);
        ok = crypto_equal(mac, p + n - 16, 16) && body % 16 == 0;
        if (ok) {
            memcpy(plain, p + 8 + iv_len, body);
            cbc_decrypt(&e->aes_in, p + 8, plain, body);
        }
    }
    if (ok && esp_replay_ok(e, seq)) {
        size_t pad = plain[body - 2];
        int next = plain[body - 1];
        if (e == &ike->esp)
            ike->new_confirmed = true; /* the server sends on the new SA: so can we */
        if (pad + 2 <= body && (next == 4 || next == 41)) {
            ike->rx_bytes += body - pad - 2;
            vpn_tunnel_input(plain, body - pad - 2);
        }
    }
    kfree(plain);
}

/* ---- requests from the server ---- */

static void respond(int exchange, uint32_t id, struct msg *inner)
{
    header(&ike->out, exchange, id, true);
    seal(&ike->out, inner);
    transmit_ike(ike->out.b, ike->out.n);
}

/* the server rekeys the ESP SA: the new SA replaces the old at once */
static void rekey_child(uint32_t id, const struct payload *pl, int count)
{
    struct msg *r = &ike->inner;
    inner_begin(r);
    const struct payload *sa = find(pl, count, PL_SA), *ni = find(pl, count, PL_NONCE);
    struct esp *e = kzalloc(sizeof(*e));
    if (!e || find(pl, count, PL_KE) || !ni || ni->n < 16 || ni->n > 256 || !take_esp(sa, e)) {
        notify(r, 0, N_NO_PROPOSAL_CHOSEN, NULL, 0);
        respond(CREATE_CHILD_SA, id, r);
        kfree(e);
        return;
    }
    random_bytes(e->spi_in, 4);
    size_t at = pl_begin(r, PL_SA);
    msg_byte(r, 0);
    msg_byte(r, 0);
    size_t pa = r->n;
    msg_16(r, 0);
    msg_byte(r, sa->p[4]);
    msg_byte(r, 3);
    msg_byte(r, 4);
    msg_byte(r, e->gcm ? 2 : 3);
    msg_put(r, e->spi_in, 4);
    transform(r, e->gcm, T_ENCR, e->gcm ? ENCR_AES_GCM16 : ENCR_AES_CBC, (int)e->key_len * 8);
    if (!e->gcm)
        transform(r, false, T_INTEG, INTEG_SHA256, 0);
    transform(r, true, T_ESN, 0, 0);
    put16(r->b + pa, (uint32_t)(r->n - pa + 2));
    pl_end(r, at);
    uint8_t nr[32];
    random_bytes(nr, 32);
    at = pl_begin(r, PL_NONCE);
    msg_put(r, nr, 32);
    pl_end(r, at);
    const struct payload *tsi = find(pl, count, PL_TSI), *tsr = find(pl, count, PL_TSR);
    if (tsi && tsr) { /* the same selectors: the server began the exchange, so its TSi is its own side */
        at = pl_begin(r, PL_TSI);
        msg_put(r, tsi->p, tsi->n);
        pl_end(r, at);
        at = pl_begin(r, PL_TSR);
        msg_put(r, tsr->p, tsr->n);
        pl_end(r, at);
    }
    respond(CREATE_CHILD_SA, id, r);
    esp_keys(e, ni->p, ni->n, nr, 32, false);
    ike->old = ike->esp;
    ike->has_old = true;
    ike->new_confirmed = false;
    ike->esp = *e;
    ike->rekeys++;
    kfree(e);
}

static void peer_request(uint8_t *p, size_t n)
{
    int exchange = p[18];
    uint32_t id = get32(p + 20);
    if (!ike->up || !decrypt_message(p, n))
        return;
    struct payload pl[16];
    int count = parse_chain(ike->in.b[0], ike->in.b + 1, ike->in.n - 1, pl, 16);
    if (exchange == CREATE_CHILD_SA) {
        rekey_child(id, pl, count);
        return;
    }
    if (exchange != INFORMATIONAL)
        return;
    struct msg *r = &ike->inner;
    inner_begin(r);
    for (int i = 0; i < count; i++)
        if (pl[i].type == PL_DELETE && pl[i].n >= 4) {
            if (pl[i].p[0] == 1) { /* the whole IKE SA */
                ike->lost = true;
                str_copy(ike->error, "the server ended the connection", sizeof(ike->error));
            } else if (pl[i].p[0] == 3 && pl[i].p[1] == 4 && pl[i].n >= 8) {
                /* deleting an ESP SA (the server's inbound SPI): answer with our side of the same SA */
                bool current = !memcmp(pl[i].p + 4, ike->esp.spi_out, 4);
                if (current) {
                    ike->lost = true;
                    str_copy(ike->error, "the server closed the tunnel", sizeof(ike->error));
                }
                size_t at = pl_begin(r, PL_DELETE);
                msg_byte(r, 3);
                msg_byte(r, 4);
                msg_16(r, 1);
                msg_put(r, current ? ike->esp.spi_in : ike->old.spi_in, 4);
                pl_end(r, at);
                if (!current && ike->has_old && !memcmp(pl[i].p + 4, ike->old.spi_out, 4)) {
                    ike->has_old = false;
                    ike->new_confirmed = true;
                }
            }
        }
    respond(INFORMATIONAL, id, r); /* also the answer to dead-peer detection */
}

/* ---- connecting ---- */

static int auth_exchange(void)
{
    struct msg *m = &ike->inner;
    inner_begin(m);
    const char *my_id = ike->use_psk ? (ike->id[0] ? ike->id : "synthos") : ike->user;
    size_t id_at = id_payload(m, PL_IDI, my_id);
    size_t id_len = m->n - id_at - 4;
    uint8_t id_body[256];
    memcpy(id_body, m->b + id_at + 4, MIN(id_len, sizeof(id_body)));
    if (!ike->use_psk) {
        size_t at = pl_begin(m, PL_CERTREQ);
        msg_byte(m, 4);
        pl_end(m, at);
    }
    if (ike->remote_id[0])
        id_payload(m, PL_IDR, ike->remote_id);
    uint8_t *octets;
    size_t n;
    signed_octets(true, id_body, id_len, &octets, &n);
    if (!octets)
        return fail("%s", "out of memory", E_NOMEM);
    if (ike->use_psk) {
        uint8_t mac[32];
        shared_key_auth((const uint8_t *)ike->psk, strlen(ike->psk), octets, n, mac);
        size_t at = pl_begin(m, PL_AUTH);
        msg_byte(m, 2);
        msg_byte(m, 0);
        msg_16(m, 0);
        msg_put(m, mac, 32);
        pl_end(m, at);
    }
    config_request(m);
    random_bytes(ike->esp.spi_in, 4);
    esp_proposals(m);
    selectors(m, PL_TSI);
    selectors(m, PL_TSR);
    request(IKE_AUTH, m);
    struct payload pl[24];
    int count;
    uint8_t msk[32];
    bool eap_done = false;
    for (int round = 0;; round++) {
        if (!await(15000)) {
            kfree(octets);
            return fail("%s", "the server stopped answering during the login", E_TIMEDOUT);
        }
        count = parse_chain(ike->in.b[0], ike->in.b + 1, ike->in.n - 1, pl, 24);
        int err = error_notify(pl, count);
        if (err) {
            kfree(octets);
            return fail("%s", notify_text(err), E_PERM);
        }
        if (ike->use_psk || eap_done)
            break;
        if (round == 0) { /* the server's certificate and signature come with its first EAP request */
            const char *why = check_server(pl, count, NULL, 0);
            if (why) {
                kfree(octets);
                return fail("%s", why, E_PERM);
            }
        }
        const struct payload *eap = find(pl, count, PL_EAP);
        if (!eap || eap->n < 4 || round > 6) {
            kfree(octets);
            return fail("%s", "the server did not start the EAP login", E_IO);
        }
        const uint8_t *e = eap->p;
        int code = e[0], eid = e[1];
        inner_begin(m);
        if (code == 3) { /* EAP success: the AUTH payload with the MSK as key */
            uint8_t mac[32];
            shared_key_auth(msk, 32, octets, n, mac);
            size_t at = pl_begin(m, PL_AUTH);
            msg_byte(m, 2);
            msg_byte(m, 0);
            msg_16(m, 0);
            msg_put(m, mac, 32);
            pl_end(m, at);
            request(IKE_AUTH, m);
            eap_done = true;
            continue;
        }
        if (code != 1 || eap->n < 5) {
            kfree(octets);
            return fail("%s", "the user name or password was not accepted", E_PERM);
        }
        int type = e[4];
        size_t at = pl_begin(m, PL_EAP);
        msg_byte(m, 2);
        msg_byte(m, eid);
        size_t len_at = m->n;
        msg_16(m, 0);
        if (type == 1) { /* identity */
            msg_byte(m, 1);
            msg_put(m, ike->user, strlen(ike->user));
        } else if (type == 26 && eap->n >= 10 && e[5] == 1 && e[9] == 16 && eap->n >= 26) { /* MS-CHAPv2 challenge */
            static uint8_t auth_ch[16], peer_ch[16], nt[24];
            memcpy(auth_ch, e + 10, 16);
            random_bytes(peer_ch, 16);
            nt_response(peer_ch, auth_ch, ike->user, ike->pass, nt);
            mschap_msk(ike->pass, nt, msk);
            msg_byte(m, 26);
            msg_byte(m, 2);
            msg_byte(m, e[6]);
            msg_16(m, (uint32_t)(4 + 1 + 49 + strlen(ike->user)));
            msg_byte(m, 49);
            msg_put(m, peer_ch, 16);
            uint8_t zero[8] = {0};
            msg_put(m, zero, 8);
            msg_put(m, nt, 24);
            msg_byte(m, 0);
            msg_put(m, ike->user, strlen(ike->user));
            /* remember what the server's success message must carry */
            authenticator_response(ike->pass, nt, peer_ch, auth_ch, ike->user, ike->expect_s);
        } else if (type == 26 && eap->n >= 6 && e[5] == 3) { /* MS-CHAPv2 success */
            const char *s = NULL;
            for (size_t i = 9; i + 42 <= eap->n; i++)
                if (e[i] == 'S' && e[i + 1] == '=') {
                    s = (const char *)e + i + 2;
                    break;
                }
            if (!s || memcmp(s, ike->expect_s, 40)) {
                kfree(octets);
                return fail("%s", "the server could not prove it knows the password", E_PERM);
            }
            msg_byte(m, 26);
            msg_byte(m, 3);
        } else if (type == 26 && eap->n >= 6 && e[5] == 4) { /* MS-CHAPv2 failure */
            kfree(octets);
            return fail("%s", "the user name or password was not accepted", E_PERM);
        } else { /* another method: ask for MS-CHAPv2 */
            msg_byte(m, 3);
            msg_byte(m, 26);
        }
        put16(m->b + len_at, (uint32_t)(m->n - at - 4));
        pl_end(m, at);
        request(IKE_AUTH, m);
    }
    kfree(octets);
    const char *why = check_server(pl, count, ike->use_psk ? (const uint8_t *)ike->psk : msk,
                                   ike->use_psk ? strlen(ike->psk) : 32);
    if (why)
        return fail("%s", why, E_PERM);
    if (!take_esp(find(pl, count, PL_SA), &ike->esp))
        return fail("%s", "the server chose no tunnel cipher this client knows", E_IO);
    esp_keys(&ike->esp, ike->ni, 32, ike->nr, ike->nr_len, true);
    take_config(find(pl, count, PL_CP));
    take_selectors(find(pl, count, PL_TSR));
    if (!ike->addr)
        return fail("%s", "the server gave no address", E_IO);
    return 0;
}

/* ---- routing ---- */

static bool routes(uint32_t dst)
{
    if (!ike || !ike->up || dst == ike->server)
        return false;
    for (int i = 0; i < ike->nts; i++)
        if (dst >= ike->ts[i].lo && dst <= ike->ts[i].hi)
            return true;
    return false;
}

static bool routes6(const uint8_t *d)
{
    if (!ike || !ike->up || !ike->has6)
        return false;
    for (int i = 0; i < ike->nts6; i++)
        if (memcmp(d, ike->ts6[i].lo, 16) >= 0 && memcmp(d, ike->ts6[i].hi, 16) <= 0)
            return true;
    return false;
}

static int output(const uint8_t *packet, size_t n)
{
    if (!ike || !ike->up)
        return E_UNREACH;
    if (n > TUN_MTU)
        return E_RANGE;
    esp_output(packet, n);
    return 0;
}

static void tunnel_tick(uint64_t now)
{
    UNUSED(now);
    if (!ike || !ike->up)
        return;
    pump();
    uint64_t t = hal_ticks_ms();
    if (t - ike->last_tx > 20000) {
        uint8_t keepalive = 0xff;
        net_sendto(ike->sock, &keepalive, 1, ike->server, ike->port);
        ike->last_tx = t;
    }
    if (!ike->req && t - ike->last_ike_rx > 30000 && !ike->lost) { /* dead-peer detection */
        inner_begin(&ike->inner);
        request(INFORMATIONAL, &ike->inner);
    }
    if (ike->req && ike->req_tries >= 6 && t - ike->req_at > 4000 && !ike->lost) {
        ike->lost = true;
        str_copy(ike->error, "the server stopped answering", sizeof(ike->error));
    }
}

static struct vpn_tunnel tunnel = {.routes = routes, .routes6 = routes6, .output = output, .tick = tunnel_tick};

static void teardown(void)
{
    if (!ike)
        return;
    vpn_tunnel_set(NULL);
    if (ike->up && !ike->lost) { /* tell the server */
        inner_begin(&ike->inner);
        size_t at = pl_begin(&ike->inner, PL_DELETE);
        msg_byte(&ike->inner, 1);
        msg_byte(&ike->inner, 0);
        msg_16(&ike->inner, 0);
        pl_end(&ike->inner, at);
        request(INFORMATIONAL, &ike->inner);
        await(1500);
    }
    if (ike->sock >= 0)
        net_close(ike->sock);
    kfree(ike->req);
    kfree(ike->init_req);
    kfree(ike->init_resp);
    kfree(ike->ca);
    drop_fragments();
    memset(ike, 0, sizeof(*ike));
    kfree(ike);
    ike = NULL;
}

static int fail(const char *fmt, const char *arg, int err)
{
    ksnprintf(last_error, sizeof(last_error), fmt, arg);
    if (ike)
        ike->up = false;
    teardown();
    return err;
}

static int connect(const char *server, const char *user, const char *pass, const char *psk, const char *id,
                   const char *remote_id, const char *ca_path)
{
    teardown();
    vpn_down(); /* one VPN at a time */
    ike = kzalloc(sizeof(*ike));
    if (!ike)
        return E_NOMEM;
    ike->sock = -1;
    str_copy(ike->host, server, sizeof(ike->host));
    char *colon = strchr(ike->host, ':');
    ike->port = 4500;
    if (colon) {
        *colon = 0;
        ike->port = (uint16_t)str_to_long(colon + 1, NULL, 10);
    }
    ike->use_psk = psk != NULL;
    str_copy(ike->user, user ? user : "", sizeof(ike->user));
    str_copy(ike->pass, pass ? pass : "", sizeof(ike->pass));
    str_copy(ike->psk, psk ? psk : "", sizeof(ike->psk));
    str_copy(ike->id, id ? id : "", sizeof(ike->id));
    str_copy(ike->remote_id, remote_id ? remote_id : "", sizeof(ike->remote_id));
    if (ca_path && vfs_read_all(ca_path, &ike->ca, &ike->ca_len) != 0)
        return fail("can't read %s", ca_path, E_NOENT);
    int err;
    if ((err = net_resolve(ike->host, &ike->server, 5000)))
        return fail("can't find %s", ike->host, err);
    ike->sock = net_socket(NET_UDP);
    if (ike->sock < 0)
        return fail("%s", "no socket", ike->sock);
    random_bytes(ike->spi_i, 8);
    if ((err = sa_init()) || (err = auth_exchange()))
        return err;
    ike->up = true;
    ike->up_at = ike->last_rx = ike->last_ike_rx = hal_ticks_ms();
    ksnprintf(ike->cipher_desc, sizeof(ike->cipher_desc), "ESP AES-%u-%s, IKE AES-%u-%s %s, %s",
              (unsigned)ike->esp.key_len * 8, ike->esp.gcm ? "GCM" : "CBC/SHA-256", (unsigned)ike->ike_key_len * 8,
              ike->ike_gcm ? "GCM" : "CBC/SHA-256",
              ike->group == DH_X25519 ? "Curve25519" : ike->group == DH_P256 ? "P-256" : "P-384",
              ike->use_psk ? "pre-shared key" : "EAP-MSCHAPv2");
    tunnel.address = ike->addr;
    tunnel.dns = ike->dns;
    tunnel.has6 = ike->has6;
    memcpy(tunnel.address6, ike->addr6, 16);
    bool all = false;
    for (int i = 0; i < ike->nts; i++)
        all |= ike->ts[i].lo == 0 && ike->ts[i].hi == 0xffffffffu;
    tunnel.block6 = all && !ike->has6;
    tunnel.mtu = TUN_MTU;
    vpn_tunnel_set(&tunnel);
    last_error[0] = 0;
    return 0;
}

/* ---- the command ---- */

static void status(struct shell *sh)
{
    if (!ike) {
        sh_printf(sh, "IKEv2 is off%s%s\n", last_error[0] ? ": " : "", last_error);
        return;
    }
    char addr[20], dns[20], a6[48];
    net_format_ip(ike->addr, addr, sizeof(addr));
    net_format_ip(ike->dns, dns, sizeof(dns));
    sh_printf(sh, "IKEv2 %s: %s\n", ike->host, ike->lost ? ike->error : "connected");
    sh_printf(sh, "  address %s", addr);
    if (ike->has6) {
        net_format_ip6(ike->addr6, a6, sizeof(a6));
        sh_printf(sh, " %s", a6);
    }
    sh_printf(sh, ", dns %s, server port %u/udp\n", ike->dns ? dns : "-", ike->port);
    for (int i = 0; i < ike->nts; i++) {
        char lo[20], hi[20];
        net_format_ip(ike->ts[i].lo, lo, sizeof(lo));
        net_format_ip(ike->ts[i].hi, hi, sizeof(hi));
        sh_printf(sh, "  through the tunnel: %s - %s\n", lo, hi);
    }
    sh_printf(sh, "  %s; ESP keys renewed: %u; received %u KB, sent %u KB\n", ike->cipher_desc, ike->rekeys,
              (unsigned)(ike->rx_bytes / 1024), (unsigned)(ike->tx_bytes / 1024));
}

COMMAND(ikev2, NULL, "net",
        "ikev2 up SERVER[:PORT] USER PASSWORD [--ca FILE] [--remote-id ID] | up SERVER[:PORT] --psk KEY [--id ID] "
        "[--remote-id ID] | down | status",
        "connect to an IKEv2 VPN server")
{
    if (argc >= 3 && !strcmp(argv[1], "up")) {
        const char *pos[2] = {NULL, NULL}, *psk = NULL, *id = NULL, *remote = NULL, *ca = NULL;
        int npos = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--psk") && i + 1 < argc)
                psk = argv[++i];
            else if (!strcmp(argv[i], "--id") && i + 1 < argc)
                id = argv[++i];
            else if (!strcmp(argv[i], "--remote-id") && i + 1 < argc)
                remote = argv[++i];
            else if (!strcmp(argv[i], "--ca") && i + 1 < argc)
                ca = argv[++i];
            else if (npos < 2)
                pos[npos++] = argv[i];
            else
                return command_usage(sh, command_find("ikev2"));
        }
        if (!psk && npos < 2)
            return command_usage(sh, command_find("ikev2"));
        char ca_path[256];
        if (ca)
            path_resolve(sh->cwd, ca, ca_path, sizeof(ca_path));
        int err = connect(argv[2], pos[0], pos[1], psk, id, remote, ca ? ca_path : NULL);
        if (err)
            return sh_error(sh, "ikev2", "%s", last_error);
        status(sh);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "down")) {
        bool was = ike != NULL;
        teardown();
        last_error[0] = 0;
        sh_printf(sh, was ? "IKEv2 disconnected\n" : "IKEv2 is off\n");
        return 0;
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        status(sh);
        return 0;
    }
    return command_usage(sh, command_find("ikev2"));
}

static const struct command *const commands[] = {&cmddef_ikev2};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(NULL, NULL, &ext);
