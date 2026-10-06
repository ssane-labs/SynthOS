#include <dirent.h>
#include <stdint.h>
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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mkrd <directory> <output>\n");
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
    return 0;
}
