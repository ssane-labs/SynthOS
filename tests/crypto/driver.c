#include <synth/sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <synth/crypto.h>
uint32_t hal_entropy(void)
{
    return 0;
}
static size_t unhex(const char *s, uint8_t *o)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        o[i] = v;
    }
    return n;
}
static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}
static const struct hash_alg *alg(const char *s)
{
    return !strcmp(s, "sha256") ? &hash_sha256 : !strcmp(s, "sha384") ? &hash_sha384 : !strcmp(s, "sha1") ? &hash_sha1
           : !strcmp(s, "md5")    ? &hash_md5
                                  : &hash_sha512;
}
static uint8_t a[4096], b[4096], c[4096], d[4096], e[4096], o[8192];
int main(int argc, char **argv)
{
    const char *cmd = argv[1];
    if (!strcmp(cmd, "sha1")) {
        size_t n = unhex(argv[2], a);
        sha1(a, n, o);
        hex(o, 20);
    } else if (!strcmp(cmd, "hash")) {
        size_t n = unhex(argv[3], a);
        const struct hash_alg *h = alg(argv[2]);
        hash_digest(h, a, n, o);
        hex(o, h->digest);
    } else if (!strcmp(cmd, "aes")) {
        size_t kl = unhex(argv[2], a);
        unhex(argv[3], b);
        struct aes k;
        aes_init(&k, a, kl);
        aes_encrypt(&k, b, o);
        aes_decrypt(&k, o, o + 16);
        hex(o, 16);
        hex(o + 16, 16);
    } else if (!strcmp(cmd, "hkdf")) {
        const struct hash_alg *h = alg(argv[2]);
        size_t sl = unhex(argv[3], a), il = unhex(argv[4], b), fl = unhex(argv[5], c);
        int L = atoi(argv[6]);
        uint8_t prk[64];
        hkdf_extract(h, a, sl, b, il, prk);
        hkdf_expand(h, prk, c, fl, o, L);
        hex(o, L);
    } else if (!strcmp(cmd, "gcm")) {
        size_t kl = unhex(argv[2], a);
        unhex(argv[3], b);
        size_t al = unhex(argv[4], c), pl = unhex(argv[5], d);
        struct gcm g;
        gcm_init(&g, a, kl);
        uint8_t tag[16];
        gcm_seal(&g, b, c, al, d, pl, o, tag);
        memcpy(o + pl, tag, 16);
        hex(o, pl + 16);
        uint8_t back[4096];
        printf("%s\n",
               gcm_open(&g, b, c, al, o, pl, back, tag) && !memcmp(back, d, pl) ? "open-ok" : "open-bad");
    } else if (!strcmp(cmd, "chacha")) {
        unhex(argv[2], a);
        unhex(argv[3], b);
        size_t al = unhex(argv[4], c), pl = unhex(argv[5], d);
        uint8_t tag[16];
        chacha20_poly1305_seal(a, b, c, al, d, pl, o, tag);
        memcpy(o + pl, tag, 16);
        hex(o, pl + 16);
        uint8_t back[4096];
        printf("%s\n", chacha20_poly1305_open(a, b, c, al, o, pl, back, tag) && !memcmp(back, d, pl)
                           ? "open-ok"
                           : "open-bad");
    } else if (!strcmp(cmd, "x25519")) {
        unhex(argv[2], a);
        unhex(argv[3], b);
        x25519(o, a, b);
        hex(o, 32);
    } else if (!strcmp(cmd, "ecpub")) {
        int cv = atoi(argv[2]);
        unhex(argv[3], a);
        if (!ec_public_key(cv, a, o))
            printf("fail\n");
        else
            hex(o, 1 + 2 * ec_size(cv));
    } else if (!strcmp(cmd, "ecdh")) {
        int cv = atoi(argv[2]);
        unhex(argv[3], a);
        size_t pl = unhex(argv[4], b);
        if (!ec_shared_secret(cv, a, b, pl, o))
            printf("fail\n");
        else
            hex(o, ec_size(cv));
    } else if (!strcmp(cmd, "ecdsa")) {
        int cv = atoi(argv[2]);
        size_t pl = unhex(argv[3], a), hl = unhex(argv[4], b), rl = unhex(argv[5], c), sl = unhex(argv[6], d);
        printf("%s\n", ecdsa_verify(cv, a, pl, b, hl, c, rl, d, sl) ? "valid" : "invalid");
    } else if (!strcmp(cmd, "rsa") || !strcmp(cmd, "pss")) {
        size_t nl = unhex(argv[2], a), el = unhex(argv[3], b);
        const struct hash_alg *h = alg(argv[4]);
        unhex(argv[5], c);
        size_t sl = unhex(argv[6], d);
        bool ok = !strcmp(cmd, "rsa") ? rsa_verify_pkcs1(a, nl, b, el, h, c, d, sl)
                                      : rsa_verify_pss(a, nl, b, el, h, c, d, sl);
        printf("%s\n", ok ? "valid" : "invalid");
    }
    return 0;
}
