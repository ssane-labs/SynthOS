#include <dirent.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct entry {
    char path[512];
    char host[1024];
    int type;
    uint32_t size;
};

static struct entry *entries;
static size_t count, cap;

static int compare(const void *a, const void *b)
{
    return strcmp(((const struct entry *)a)->path, ((const struct entry *)b)->path);
}

static void add(const char *path, const char *host, int type, uint32_t size)
{
    if (count == cap) {
        cap = cap ? cap * 2 : 64;
        entries = realloc(entries, cap * sizeof(*entries));
        if (!entries)
            exit(1);
    }
    struct entry *e = &entries[count++];
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->host, sizeof(e->host), "%s", host);
    e->type = type;
    e->size = size;
}

static void walk(const char *host, const char *rel)
{
    DIR *d = opendir(host);
    if (!d) {
        perror(host);
        exit(1);
    }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.' && (!ent->d_name[1] || (ent->d_name[1] == '.' && !ent->d_name[2])))
            continue;
        if (strcmp(ent->d_name, ".gitkeep") == 0)
            continue;
        char hpath[1024], rpath[512];
        struct stat st;
        snprintf(hpath, sizeof(hpath), "%s/%s", host, ent->d_name);
        snprintf(rpath, sizeof(rpath), "%s/%s", rel, ent->d_name);
        if (stat(hpath, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            add(rpath, hpath, 2, 0);
            walk(hpath, rpath);
        } else if (S_ISREG(st.st_mode)) {
            add(rpath, hpath, 1, (uint32_t)st.st_size);
        }
    }
    closedir(d);
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
    fwrite(b, 1, 4, f);
}

static uint8_t *emit_length(uint8_t *o, size_t n)
{
    while (n >= 255) {
        *o++ = 255;
        n -= 255;
    }
    *o++ = (uint8_t)n;
    return o;
}

static uint8_t *emit_sequence(uint8_t *o, const uint8_t *lit, size_t lit_len, size_t match_len, size_t offset)
{
    uint8_t *token = o++;
    *token = (uint8_t)((lit_len >= 15 ? 15 : lit_len) << 4);
    if (lit_len >= 15)
        o = emit_length(o, lit_len - 15);
    memcpy(o, lit, lit_len);
    o += lit_len;
    if (!match_len)
        return o;
    *o++ = (uint8_t)offset;
    *o++ = (uint8_t)(offset >> 8);
    size_t m = match_len - 4;
    *token |= (uint8_t)(m >= 15 ? 15 : m);
    if (m >= 15)
        o = emit_length(o, m - 15);
    return o;
}

/* hash chains over the 64 KiB window with one step of lazy matching: the plain LZ4 format, packed tighter */
#define CHAIN_DEPTH 1024
static uint32_t chain_head[1 << 16], chain_prev[1 << 16];

static uint32_t hash4(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> 16;
}

static void chain_insert(const uint8_t *in, size_t i)
{
    uint32_t h = hash4(in + i);
    chain_prev[i & 0xffff] = chain_head[h];
    chain_head[h] = (uint32_t)i;
}

static size_t chain_best(const uint8_t *in, size_t n, size_t i, size_t *dist)
{
    size_t best = 0;
    uint32_t cand = chain_head[hash4(in + i)];
    for (int depth = 0; depth < CHAIN_DEPTH && cand != 0xffffffffu && cand < i && i - cand <= 65535; depth++) {
        if (in[cand + best] == in[i + best] && !memcmp(in + cand, in + i, 4)) {
            size_t len = 4;
            while (i + len + 5 < n && in[cand + len] == in[i + len])
                len++;
            if (len > best) {
                best = len;
                *dist = i - cand;
            }
        }
        uint32_t next = chain_prev[cand & 0xffff];
        if (next >= cand)
            break;
        cand = next;
    }
    return best;
}

static size_t lz4_compress(const uint8_t *in, size_t n, uint8_t *out)
{
    memset(chain_head, 0xff, sizeof(chain_head));
    memset(chain_prev, 0xff, sizeof(chain_prev));
    uint8_t *o = out;
    size_t anchor = 0, i = 0;
    while (n >= 13 && i + 12 < n) {
        size_t dist = 0, len = chain_best(in, n, i, &dist);
        if (len >= 4 && i + 13 < n) {
            chain_insert(in, i);
            size_t dist2 = 0, len2 = chain_best(in, n, i + 1, &dist2);
            if (len2 > len + 1) {
                i++;
                len = len2;
                dist = dist2;
            }
        }
        if (len < 4) {
            chain_insert(in, i);
            i++;
            continue;
        }
        o = emit_sequence(o, in + anchor, i - anchor, len, dist);
        size_t end = i + len;
        for (size_t k = i; k < end && k + 4 <= n; k++)
            if (chain_head[hash4(in + k)] != (uint32_t)k)
                chain_insert(in, k);
        i = end;
        anchor = i;
    }
    o = emit_sequence(o, in + anchor, n - anchor, 0, 0);
    return (size_t)(o - out);
}

static int compress_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = malloc((size_t)size), *packed = malloc((size_t)size + (size_t)size / 200 + 64);
    if (!raw || !packed || fread(raw, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);
    size_t n = lz4_compress(raw, (size_t)size, packed);
    f = fopen(path, "wb");
    if (!f)
        return 1;
    fwrite("SRDZ", 1, 4, f);
    put32(f, (uint32_t)size);
    fwrite(packed, 1, n, f);
    fclose(f);
    printf("  LZ4     %ld -> %zu bytes\n", size, n + 8);
    return 0;
}

static uint8_t *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)*size + 1);
    if (buf && fread(buf, 1, (size_t)*size, f) != (size_t)*size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* the kernel image, then "SYPK", the pack size and the pack: one ramdisk, or "SRDC" and several ramdisks, each with
   its size in front and padded to 4 bytes (the system files, then the app modules built for this kernel) */
static int pack_kernel(const char *image, const char *const *disks, int ndisks, const char *path)
{
    long n, m = ndisks > 1 ? 4 : 0;
    uint8_t *img = slurp(image, &n), *part[4];
    long len[4];
    if (!img || n < 16 || ndisks > 4)
        return 1;
    for (int i = 0; i < ndisks; i++) {
        if (!(part[i] = slurp(disks[i], &len[i])))
            return 1;
        m += ndisks > 1 ? 4 + (len[i] + 3) / 4 * 4 : len[i];
    }
    long total = (n + 15) / 16 * 16 + 8 + m;
    uint8_t *buf = calloc(1, (size_t)total);
    if (!buf)
        return 1;
    memcpy(buf, img, (size_t)n);
    uint8_t *pack = buf + (n + 15) / 16 * 16, *at = pack + 8;
    memcpy(pack, "SYPK", 4);
    uint32_t v = (uint32_t)m;
    memcpy(pack + 4, &v, 4);
    if (ndisks > 1) {
        memcpy(at, "SRDC", 4);
        at += 4;
    }
    for (int i = 0; i < ndisks; i++) {
        if (ndisks > 1) {
            v = (uint32_t)len[i];
            memcpy(at, &v, 4);
            at += 4;
        }
        memcpy(at, part[i], (size_t)len[i]);
        at += ndisks > 1 ? (len[i] + 3) / 4 * 4 : len[i];
    }
    v = (uint32_t)((total + 511) / 512);
    memcpy(buf + 12, &v, 4);
    FILE *out = fopen(path, "wb");
    if (!out || fwrite(buf, 1, (size_t)total, out) != (size_t)total)
        return 1;
    fclose(out);
    return 0;
}

int main(int argc, char **argv)
{
    /* --kernel <image> <output> <ramdisk>...: the old order (image, ramdisk, output) is kept for one ramdisk */
    if (argc == 5 && strcmp(argv[1], "--kernel") == 0)
        return pack_kernel(argv[2], (const char *const *)&argv[3], 1, argv[4]);
    if (argc > 5 && strcmp(argv[1], "--kernel") == 0)
        return pack_kernel(argv[2], (const char *const *)&argv[4], argc - 4, argv[3]);
    /* --raw leaves the pack uncompressed for tools/lzmapack.py */
    bool raw = argc == 4 && strcmp(argv[1], "--raw") == 0;
    if (raw) {
        argv++;
        argc--;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: mkrd [--raw] <directory> <output>\n");
        return 1;
    }
    walk(argv[1], "");
    qsort(entries, count, sizeof(*entries), compare);
    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    fwrite("SRD1", 1, 4, out);
    put32(out, (uint32_t)count);
    long total = 8;
    for (size_t i = 0; i < count; i++) {
        struct entry *e = &entries[i];
        uint16_t n = (uint16_t)strlen(e->path);
        uint8_t hdr[4] = {n, n >> 8, (uint8_t)e->type, 0};
        fwrite(hdr, 1, 4, out);
        put32(out, e->size);
        fwrite(e->path, 1, n, out);
        total += 8 + n;
        if (e->type == 1) {
            FILE *in = fopen(e->host, "rb");
            if (!in) {
                perror(e->host);
                return 1;
            }
            char buf[8192];
            size_t r;
            while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
                fwrite(buf, 1, r, out);
            fclose(in);
            total += e->size;
        }
        while (total % 4) {
            fputc(0, out);
            total++;
        }
    }
    fclose(out);
    printf("  RAMDISK %s: %zu entries, %ld bytes\n", argv[2], count, total);
    return raw ? 0 : compress_file(argv[2]);
}
