#include <synth/block.h>
#include <synth/bundle.h>
#include <synth/error.h>
#include <synth/hal.h>
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

static int write_whole(const char *path, const uint8_t *data, size_t n)
{
    char dir[PATH_MAX_LEN];
    path_dirname(path, dir, sizeof(dir));
    vfs_mkdirs(dir);
    return vfs_write_all(path, data, n);
}

/* The unpacking is a job done in short steps, so the desktop (and the shell under it) keeps running between them;
   the program's start file `last` is kept in memory and written at the very end. */
struct bundle_job {
    struct bundle_info info;
    struct reader r;
    char dest[PATH_MAX_LEN], last[NAME_MAX_LEN];
    uint32_t index;
    uint64_t done;
    bool in_record, deferred;
    uint32_t crc, csize, usize;
    struct sink sink;
    struct gunzip_stream *z;
    char path[PATH_MAX_LEN], last_path[PATH_MAX_LEN];
    uint8_t *last_data;
    size_t last_len;
};

struct bundle_job *bundle_job_start(const char *dest, const char *last, int *err)
{
    struct bundle_job *j = kzalloc(sizeof(*j));
    *err = E_NOMEM;
    if (!j)
        return NULL;
    *err = E_NOENT;
    if (!bundle_info(&j->info)) {
        kfree(j);
        return NULL;
    }
    j->r = (struct reader){.dev = block_find("bundle0"), .lba = 0, .end = j->info.sectors};
    str_copy(j->dest, dest, sizeof(j->dest));
    str_copy(j->last, last ? last : "", sizeof(j->last));
    uint8_t header[BLOCK_SIZE];
    *err = E_NOMEM;
    if (!(j->r.buf = kmalloc(CHUNK)) || (*err = read_bytes(&j->r, header, sizeof(header))) != 0) {
        kfree(j->r.buf);
        kfree(j);
        return NULL;
    }
    *err = 0;
    return j;
}

static int open_record(struct bundle_job *j)
{
    uint8_t rec[20];
    char name[PATH_MAX_LEN];
    int err = read_bytes(&j->r, rec, sizeof(rec));
    if (err)
        return err;
    uint32_t name_len = (uint32_t)rec[4] | (uint32_t)rec[5] << 8;
    if (memcmp(rec, "SBE1", 4) || name_len >= sizeof(name) || (rec[6] != 0 && rec[6] != 8))
        return E_INVAL;
    j->crc = rd32(rec + 8);
    j->csize = rd32(rec + 12);
    j->usize = rd32(rec + 16);
    if ((err = read_bytes(&j->r, name, name_len)))
        return err;
    name[name_len] = 0;
    size_t prefix = strlen(j->info.prefix);
    const char *rel = !strncmp(name, j->info.prefix, prefix) ? name + prefix : name;
    path_join(j->dest, rel, j->path, sizeof(j->path));
    j->sink = (struct sink){.crc = 0};
    j->deferred = j->last[0] && !str_casecmp(rel, j->last);
    if (j->deferred) {
        if (!(j->sink.keep = kmalloc(MAX(j->usize, 1u))))
            return E_NOMEM;
    } else {
        char dir[PATH_MAX_LEN];
        path_dirname(j->path, dir, sizeof(dir));
        vfs_mkdirs(dir);
        if ((err = vfs_open(j->path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &j->sink.f)))
            return err;
    }
    if (rec[6] == 8 && !(j->z = inflate_stream_new(to_file, &j->sink)))
        return E_NOMEM;
    j->in_record = true;
    return 0;
}

static int close_record(struct bundle_job *j)
{
    int err = 0;
    if (j->z) {
        err = gunzip_stream_end(j->z);
        gunzip_stream_free(j->z);
        j->z = NULL;
    }
    if (j->sink.f)
        vfs_close(j->sink.f);
    j->sink.f = NULL;
    if (!err && (j->sink.crc != j->crc || j->sink.written != j->usize))
        err = E_INVAL;
    if (j->deferred && !err) {
        kfree(j->last_data);
        j->last_data = j->sink.keep;
        j->last_len = j->sink.kept;
        str_copy(j->last_path, j->path, sizeof(j->last_path));
    } else {
        kfree(j->sink.keep);
    }
    j->sink.keep = NULL;
    j->in_record = false;
    j->done += j->usize;
    j->index++;
    return err;
}

/* Works for about budget_ms; returns 1 while there is more to do, 0 when everything is in place, or an error. */
int bundle_job_step(struct bundle_job *j, uint32_t budget_ms)
{
    uint64_t end = hal_ticks_ms() + budget_ms;
    do {
        int err;
        if (!j->in_record) {
            if (j->index == j->info.files) {
                err = j->last_data ? write_whole(j->last_path, j->last_data, j->last_len) : 0;
                kfree(j->last_data);
                j->last_data = NULL;
                return err;
            }
            if ((err = open_record(j)))
                return err;
        }
        while (j->csize && hal_ticks_ms() < end) {
            if ((err = refill(&j->r)))
                return err;
            size_t k = MIN(MIN((size_t)j->csize, j->r.len - j->r.pos), (size_t)65536);
            err = j->z ? gunzip_stream_push(j->z, j->r.buf + j->r.pos, k) : to_file(&j->sink, j->r.buf + j->r.pos, k);
            if (err)
                return err;
            j->r.pos += k;
            j->csize -= (uint32_t)k;
        }
        if (!j->csize && (err = close_record(j)))
            return err;
    } while (hal_ticks_ms() < end);
    return 1;
}

void bundle_job_progress(const struct bundle_job *j, uint64_t *done, uint64_t *total)
{
    *done = j->done + j->sink.written;
    *total = j->info.bytes;
}

void bundle_job_free(struct bundle_job *j)
{
    if (!j)
        return;
    if (j->z)
        gunzip_stream_free(j->z);
    if (j->sink.f)
        vfs_close(j->sink.f);
    kfree(j->sink.keep);
    kfree(j->last_data);
    kfree(j->r.buf);
    kfree(j);
}

int bundle_extract(const char *dest, const char *last, void (*progress)(void *ctx, uint64_t done, uint64_t total),
                   void *ctx)
{
    int err;
    struct bundle_job *j = bundle_job_start(dest, last, &err);
    if (!j)
        return err;
    while ((err = bundle_job_step(j, 100)) > 0)
        if (progress) {
            uint64_t done, total;
            bundle_job_progress(j, &done, &total);
            progress(ctx, done, total);
        }
    bundle_job_free(j);
    return err;
}
