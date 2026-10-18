#include <synth/block.h>
#include <synth/bundle.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/zip.h>

#define CHUNK (1u << 20)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool bundle_info(struct bundle_info *out)
{
    struct blockdev *dev = block_find("bundle0");
    uint8_t sector[BLOCK_SIZE];
    if (!dev || block_read(dev, 0, 1, sector) || memcmp(sector, "SYNTHBDL", 8) || rd32(sector + 8) != 1)
        return false;
    out->sectors = rd32(sector + 12);
    out->files = rd32(sector + 16);
    out->bytes = (uint64_t)rd32(sector + 20) | (uint64_t)rd32(sector + 24) << 32;
    memcpy(out->title, sector + 28, 63);
    out->title[63] = 0;
    memcpy(out->prefix, sector + 92, 63);
    out->prefix[63] = 0;
    return out->sectors <= dev->sectors;
}

struct reader {
    struct blockdev *dev;
    uint32_t lba, end;
    uint8_t *buf;
    size_t pos, len;
};

static int refill(struct reader *r)
{
    if (r->pos < r->len)
        return 0;
    if (r->lba >= r->end)
        return E_INVAL;
    uint32_t count = MIN(CHUNK / BLOCK_SIZE, r->end - r->lba);
    int err = block_read(r->dev, r->lba, count, r->buf);
    if (err)
        return err;
    r->lba += count;
    r->pos = 0;
    r->len = (size_t)count * BLOCK_SIZE;
    return 0;
}

static int read_bytes(struct reader *r, void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n) {
        int err = refill(r);
        if (err)
            return err;
        size_t k = MIN(n, r->len - r->pos);
        memcpy(d, r->buf + r->pos, k);
        r->pos += k;
        d += k;
        n -= k;
    }
    return 0;
}

struct sink {
    struct file *f;
    uint8_t *keep;
    size_t kept;
    uint32_t crc;
    uint64_t written;
};

static int to_file(void *ctx, const void *data, size_t n)
{
    struct sink *s = ctx;
    s->crc = crc32_update(s->crc, data, n);
    s->written += n;
    if (s->keep) {
        memcpy(s->keep + s->kept, data, n);
        s->kept += n;
        return 0;
    }
    return vfs_write(s->f, data, n) == (long)n ? 0 : E_NOSPC;
}

/* Copies (or inflates) `csize` bytes of the record body into the sink. */
static int unpack(struct reader *r, uint8_t method, uint32_t csize, struct sink *s)
{
    struct gunzip_stream *z = method == 8 ? inflate_stream_new(to_file, s) : NULL;
    if (method == 8 && !z)
        return E_NOMEM;
    int err = 0;
    while (csize && !err) {
        if ((err = refill(r)))
            break;
        size_t k = MIN((size_t)csize, r->len - r->pos);
        err = z ? gunzip_stream_push(z, r->buf + r->pos, k) : to_file(s, r->buf + r->pos, k);
        r->pos += k;
        csize -= (uint32_t)k;
    }
    if (z) {
        if (!err)
            err = gunzip_stream_end(z);
        gunzip_stream_free(z);
    }
    return err;
}

static int write_whole(const char *path, const uint8_t *data, size_t n)
{
    char dir[PATH_MAX_LEN];
    path_dirname(path, dir, sizeof(dir));
    vfs_mkdirs(dir);
    return vfs_write_all(path, data, n);
}

int bundle_extract(const char *dest, const char *last, void (*progress)(void *ctx, uint64_t done, uint64_t total),
                   void *ctx)
{
    struct bundle_info info;
    if (!bundle_info(&info))
        return E_NOENT;
    struct reader r = {.dev = block_find("bundle0"), .lba = 0, .end = info.sectors};
    r.buf = kmalloc(CHUNK);
    if (!r.buf)
        return E_NOMEM;
    uint8_t header[BLOCK_SIZE];
    int err = read_bytes(&r, header, sizeof(header));
    size_t prefix = strlen(info.prefix);
    uint64_t done = 0;
    char path[PATH_MAX_LEN], last_path[PATH_MAX_LEN] = "";
    uint8_t *last_data = NULL;
    size_t last_len = 0;
    for (uint32_t i = 0; i < info.files && !err; i++) {
        uint8_t rec[20];
        char name[PATH_MAX_LEN];
        if ((err = read_bytes(&r, rec, sizeof(rec))))
            break;
        uint32_t name_len = (uint32_t)rec[4] | (uint32_t)rec[5] << 8, crc = rd32(rec + 8), csize = rd32(rec + 12),
                 usize = rd32(rec + 16);
        if (memcmp(rec, "SBE1", 4) || name_len >= sizeof(name) || (rec[6] != 0 && rec[6] != 8)) {
            err = E_INVAL;
            break;
        }
        if ((err = read_bytes(&r, name, name_len)))
            break;
        name[name_len] = 0;
        const char *rel = !strncmp(name, info.prefix, prefix) ? name + prefix : name;
        path_join(dest, rel, path, sizeof(path));
        struct sink s = {.crc = 0};
        bool deferred = last && !str_casecmp(rel, last);
        if (deferred) {
            s.keep = kmalloc(MAX(usize, 1u));
            if (!s.keep) {
                err = E_NOMEM;
                break;
            }
        } else {
            char dir[PATH_MAX_LEN];
            path_dirname(path, dir, sizeof(dir));
            vfs_mkdirs(dir);
            if ((err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &s.f)))
                break;
        }
        err = unpack(&r, rec[6], csize, &s);
        if (s.f)
            vfs_close(s.f);
        if (!err && (s.crc != crc || s.written != usize))
            err = E_INVAL;
        if (deferred && !err) {
            kfree(last_data);
            last_data = s.keep;
            last_len = s.kept;
            str_copy(last_path, path, sizeof(last_path));
        } else {
            kfree(s.keep);
        }
        done += usize;
        if (progress)
            progress(ctx, done, info.bytes);
    }
    if (!err && last_data)
        err = write_whole(last_path, last_data, last_len);
    kfree(last_data);
    kfree(r.buf);
    return err;
}
