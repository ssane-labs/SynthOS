#include <synth/block.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

#define FAT_CACHE 64
#define FAT_EOC 0x0ffffff8u
#define ENTRY 32
#define ATTR_READONLY 0x01
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN 0x0f

struct cache_line {
    uint32_t lba, used;
    bool valid, dirty;
    uint8_t data[BLOCK_SIZE];
};

struct fat {
    struct blockdev *dev;
    int bits;
    uint32_t spc, cluster_bytes;
    uint32_t fat_start, fat_sectors, fats;
    uint32_t root_start, root_entries;
    uint32_t root_cluster;
    uint32_t data_start;
    uint32_t clusters;
    uint32_t hint;
    uint32_t free_count;
    bool free_known;
    uint8_t *wb;
    uint32_t wb_lba, wb_count;
    uint32_t clock;
    struct cache_line *last;
    struct cache_line cache[FAT_CACHE];
};

struct fat_node {
    struct vnode vn;
    struct fat *fs;
    uint32_t cluster;
    uint32_t size;
    bool root;
    uint32_t parent;
    uint32_t entry;
    uint32_t walk_index, walk_cluster;
};

struct found {
    uint32_t first, index;
    uint8_t raw[ENTRY];
    char name[NAME_MAX_LEN];
};

static const struct vnode_ops fat_ops;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

#define WB_SECTORS 128

static int wb_flush(struct fat *fs)
{
    if (!fs->wb_count)
        return 0;
    int err = block_write(fs->dev, fs->wb_lba, fs->wb_count, fs->wb);
    fs->wb_count = 0;
    return err;
}

static int wb_sync(struct fat *fs, uint32_t lba, uint32_t count)
{
    if (fs->wb_count && lba < fs->wb_lba + fs->wb_count && fs->wb_lba < lba + count)
        return wb_flush(fs);
    return 0;
}

static int data_write(struct fat *fs, uint32_t lba, uint32_t count, const uint8_t *buf)
{
    int err = 0;
    while (count && !err) {
        if (fs->wb_count && lba != fs->wb_lba + fs->wb_count && (err = wb_flush(fs)))
            return err;
        if (!fs->wb_count && count >= WB_SECTORS)
            return block_write(fs->dev, lba, count, buf);
        if (!fs->wb && !(fs->wb = kmalloc(WB_SECTORS * BLOCK_SIZE)))
            return block_write(fs->dev, lba, count, buf);
        if (!fs->wb_count)
            fs->wb_lba = lba;
        uint32_t n = MIN(count, WB_SECTORS - fs->wb_count);
        memcpy(fs->wb + fs->wb_count * BLOCK_SIZE, buf, n * BLOCK_SIZE);
        fs->wb_count += n;
        lba += n;
        buf += n * BLOCK_SIZE;
        count -= n;
        if (fs->wb_count == WB_SECTORS)
            err = wb_flush(fs);
    }
    return err;
}

static int write_back(struct fat *fs, struct cache_line *c)
{
    if (!c->dirty)
        return 0;
    c->dirty = false;
    int err = block_write(fs->dev, c->lba, 1, c->data);
    if (err)
        c->valid = false;
    return err;
}

static int fat_flush(struct fat *fs)
{
    int first = wb_flush(fs);
    for (int i = 0; i < FAT_CACHE; i++) {
        int err = write_back(fs, &fs->cache[i]);
        if (err && !first)
            first = err;
    }
    return first;
}

static struct cache_line *lookup(struct fat *fs, uint32_t lba)
{
    if (fs->last && fs->last->valid && fs->last->lba == lba)
        return fs->last;
    for (int i = 0; i < FAT_CACHE; i++)
        if (fs->cache[i].valid && fs->cache[i].lba == lba)
            return fs->last = &fs->cache[i];
    return NULL;
}

static void mark_dirty(struct fat *fs, uint32_t lba)
{
    struct cache_line *c = lookup(fs, lba);
    if (c)
        c->dirty = true;
}

static uint8_t *sector(struct fat *fs, uint32_t lba, int *err)
{
    struct cache_line *c = lookup(fs, lba);
    if (!c) {
        c = &fs->cache[0];
        for (int i = 0; i < FAT_CACHE; i++) {
            struct cache_line *l = &fs->cache[i];
            if (!l->valid) {
                c = l;
                break;
            }
            if (l->used < c->used)
                c = l;
        }
        if (c->valid && (*err = write_back(fs, c)))
            return NULL;
        if ((*err = wb_sync(fs, lba, 1)))
            return NULL;
        *err = block_read(fs->dev, lba, 1, c->data);
        if (*err) {
            c->valid = false;
            return NULL;
        }
        c->lba = lba;
        c->valid = true;
        fs->last = c;
    }
    c->used = ++fs->clock;
    *err = 0;
    return c->data;
}

static int sector_sync(struct fat *fs, uint32_t lba)
{
    struct cache_line *c = lookup(fs, lba);
    if (!c)
        return E_IO;
    c->dirty = false;
    int err = wb_sync(fs, lba, 1);
    if (!err)
        err = block_write(fs->dev, lba, 1, c->data);
    if (err)
        c->valid = false;
    return err;
}

static void cache_drop(struct fat *fs, uint32_t lba, uint32_t count)
{
    for (int i = 0; i < FAT_CACHE; i++)
        if (fs->cache[i].lba >= lba && fs->cache[i].lba < lba + count)
            fs->cache[i].valid = fs->cache[i].dirty = false;
}

static uint32_t cluster_lba(struct fat *fs, uint32_t c)
{
    return fs->data_start + (c - 2) * fs->spc;
}

static bool valid_cluster(struct fat *fs, uint32_t c)
{
    return c >= 2 && c < fs->clusters + 2;
}

static int fat_get(struct fat *fs, uint32_t c, uint32_t *out)
{
    uint32_t off = fs->bits == 12 ? c + c / 2 : c * (uint32_t)(fs->bits / 8);
    uint32_t lba = fs->fat_start + off / BLOCK_SIZE, at = off % BLOCK_SIZE;
    int err;
    uint8_t *p = sector(fs, lba, &err);
    if (!p)
        return err;
    uint32_t v;
    if (fs->bits == 12) {
        uint32_t lo = p[at], hi;
        if (at == BLOCK_SIZE - 1) {
            uint8_t *q = sector(fs, lba + 1, &err);
            if (!q)
                return err;
            hi = q[0];
        } else {
            hi = p[at + 1];
        }
        v = (c & 1) ? (lo | hi << 8) >> 4 : (lo | hi << 8) & 0xfff;
        if (v >= 0xff8)
            v = 0x0fffffff;
    } else if (fs->bits == 16) {
        v = le16(p + at);
        if (v >= 0xfff8)
            v = 0x0fffffff;
    } else {
        v = le32(p + at) & 0x0fffffff;
    }
    *out = v;
    return 0;
}

static int fat_set(struct fat *fs, uint32_t c, uint32_t v)
{
    uint32_t off = fs->bits == 12 ? c + c / 2 : c * (uint32_t)(fs->bits / 8);
    for (uint32_t copy = 0; copy < fs->fats; copy++) {
        uint32_t lba = fs->fat_start + copy * fs->fat_sectors + off / BLOCK_SIZE, at = off % BLOCK_SIZE;
        int err;
        uint8_t *p = sector(fs, lba, &err);
        if (!p)
            return err;
        if (fs->bits == 12) {
            uint8_t *q = at == BLOCK_SIZE - 1 ? sector(fs, lba + 1, &err) : p;
            if (!q)
                return err;
            uint8_t *hi = at == BLOCK_SIZE - 1 ? q : p + at + 1;
            uint32_t value = v & 0xfff;
            if (c & 1) {
                p[at] = (uint8_t)((p[at] & 0x0f) | (value << 4 & 0xf0));
                *hi = (uint8_t)(value >> 4);
            } else {
                p[at] = (uint8_t)value;
                *hi = (uint8_t)((*hi & 0xf0) | (value >> 8));
            }
            mark_dirty(fs, lba);
            if (q != p)
                mark_dirty(fs, lba + 1);
            continue;
        }
        if (fs->bits == 16)
            put16(p + at, v);
        else
            put32(p + at, (le32(p + at) & 0xf0000000u) | (v & 0x0fffffff));
        mark_dirty(fs, lba);
    }
    return 0;
}

static int zero_cluster(struct fat *fs, uint32_t c)
{
    uint8_t *zero = kzalloc(fs->cluster_bytes);
    if (!zero)
        return E_NOMEM;
    uint32_t lba = cluster_lba(fs, c);
    int err = wb_sync(fs, lba, fs->spc);
    if (!err)
        err = block_write(fs->dev, lba, fs->spc, zero);
    cache_drop(fs, lba, fs->spc);
    kfree(zero);
    return err;
}

static int alloc_cluster(struct fat *fs, uint32_t prev, bool zero, uint32_t *out)
{
    for (uint32_t i = 0; i < fs->clusters; i++) {
        uint32_t c = 2 + (fs->hint - 2 + i) % fs->clusters, v;
        int err = fat_get(fs, c, &v);
        if (err)
            return err;
        if (v)
            continue;
        if ((err = fat_set(fs, c, 0x0fffffff)))
            return err;
        if (zero && (err = zero_cluster(fs, c)))
            return err;
        if (prev && (err = fat_set(fs, prev, c)))
            return err;
        fs->hint = c + 1 < fs->clusters + 2 ? c + 1 : 2;
        if (fs->free_known && fs->free_count)
            fs->free_count--;
        *out = c;
        return 0;
    }
    return E_NOSPC;
}

static void free_chain(struct fat *fs, uint32_t c)
{
    for (uint32_t guard = 0; valid_cluster(fs, c) && guard < fs->clusters; guard++) {
        uint32_t next;
        if (fat_get(fs, c, &next) || fat_set(fs, c, 0))
            return;
        if (fs->free_known)
            fs->free_count++;
        c = next;
    }
}

static int chain_at(struct fat *fs, uint32_t start, uint32_t index, uint32_t *out)
{
    uint32_t c = start;
    for (uint32_t i = 0; i < index; i++) {
        int err = fat_get(fs, c, &c);
        if (err)
            return err;
        if (!valid_cluster(fs, c))
            return E_NOENT;
    }
    *out = c;
    return 0;
}

static int dir_entry(struct fat *fs, uint32_t dir, uint32_t index, uint8_t **out, uint32_t *lba_out)
{
    uint32_t lba;
    if (!dir) {
        if (index >= fs->root_entries)
            return E_NOENT;
        lba = fs->root_start + index * ENTRY / BLOCK_SIZE;
    } else {
        uint32_t per = fs->cluster_bytes / ENTRY, c;
        int err = chain_at(fs, dir, index / per, &c);
        if (err)
            return err;
        lba = cluster_lba(fs, c) + (index % per) * ENTRY / BLOCK_SIZE;
    }
    int err;
    uint8_t *p = sector(fs, lba, &err);
    if (!p)
        return err;
    *out = p + index * ENTRY % BLOCK_SIZE;
    *lba_out = lba;
    return 0;
}

static int dir_extend(struct fat *fs, uint32_t dir)
{
    if (!dir)
        return E_NOSPC;
    uint32_t c = dir, next;
    for (uint32_t guard = 0; guard < fs->clusters; guard++) {
        int err = fat_get(fs, c, &next);
        if (err)
            return err;
        if (!valid_cluster(fs, next))
            return alloc_cluster(fs, c, true, &next);
        c = next;
    }
    return E_IO;
}

static void append_utf8(char *out, size_t *n, uint16_t ch)
{
    if (ch < 0x80) {
        if (*n + 1 < NAME_MAX_LEN)
            out[(*n)++] = (char)ch;
    } else if (ch < 0x800) {
        if (*n + 2 < NAME_MAX_LEN) {
            out[(*n)++] = (char)(0xc0 | ch >> 6);
            out[(*n)++] = (char)(0x80 | (ch & 0x3f));
        }
    } else if (*n + 3 < NAME_MAX_LEN) {
        out[(*n)++] = (char)(0xe0 | ch >> 12);
        out[(*n)++] = (char)(0x80 | (ch >> 6 & 0x3f));
        out[(*n)++] = (char)(0x80 | (ch & 0x3f));
    }
}

static void short_name(const uint8_t *e, char *out)
{
    size_t n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xe5 : e[i]);
        out[n++] = (e[12] & 0x08) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (e[12] & 0x10) && e[i] >= 'A' && e[i] <= 'Z' ? (char)(e[i] + 32) : (char)e[i];
    }
    out[n] = 0;
}

static uint8_t checksum(const uint8_t *name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static int dir_next(struct fat *fs, uint32_t dir, uint32_t *index, struct found *out)
{
    static const uint8_t slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint16_t lfn[260];
    int lfn_count = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_first = 0;
    for (uint32_t i = *index;; i++) {
        uint8_t *e;
        uint32_t lba;
        int err = dir_entry(fs, dir, i, &e, &lba);
        if (err)
            return err == E_NOENT ? E_NOENT : err;
        if (e[0] == 0)
            return E_NOENT;
        if (e[0] == 0xe5) {
            lfn_count = 0;
            continue;
        }
        if ((e[11] & 0x3f) == ATTR_LFN) {
            int seq = e[0] & 0x1f;
            if (e[0] & 0x40) {
                lfn_count = seq;
                lfn_sum = e[13];
                lfn_first = i;
                memset(lfn, 0, sizeof(lfn));
            }
            if (seq < 1 || seq > 20 || seq > lfn_count || e[13] != lfn_sum) {
                lfn_count = 0;
                continue;
            }
            for (int k = 0; k < 13; k++)
                lfn[(seq - 1) * 13 + k] = le16(e + slots[k]);
            continue;
        }
        if (e[11] & ATTR_VOLUME) {
            lfn_count = 0;
            continue;
        }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
            lfn_count = 0;
            continue;
        }
        memcpy(out->raw, e, ENTRY);
        out->index = i;
        out->first = i;
        if (lfn_count && checksum(e) == lfn_sum) {
            size_t n = 0;
            for (int k = 0; k < lfn_count * 13 && lfn[k] && lfn[k] != 0xffff; k++)
                append_utf8(out->name, &n, lfn[k]);
            out->name[n] = 0;
            out->first = lfn_first;
        } else {
            short_name(e, out->name);
        }
        *index = i + 1;
        return 0;
    }
}

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

static int dir_find(struct fat *fs, uint32_t dir, const char *name, struct found *out)
{
    uint32_t index = 0;
    int err;
    while ((err = dir_next(fs, dir, &index, out)) == 0)
        if (same_name(out->name, name))
            return 0;
    return err;
}

static uint32_t entry_cluster(struct fat *fs, const uint8_t *e)
{
    uint32_t c = le16(e + 26) | (fs->bits == 32 ? (uint32_t)le16(e + 20) << 16 : 0);
    return c;
}

static uint32_t dir_key(struct fat *fs, uint32_t cluster)
{
    return fs->bits != 32 && cluster == 0 ? 0 : cluster ? cluster : fs->root_cluster;
}

static struct fat_node *node(struct vnode *vn)
{
    return (struct fat_node *)vn;
}

static int node_new(struct fat *fs, struct mount *mnt, uint32_t parent, const struct found *f, struct vnode **out)
{
    struct fat_node *n = kzalloc(sizeof(*n));
    if (!n)
        return E_NOMEM;
    bool dir = f->raw[11] & ATTR_DIR;
    n->vn.ops = &fat_ops;
    n->vn.type = dir ? VN_DIR : VN_FILE;
    n->vn.mount = mnt;
    n->vn.refs = 1;
    n->vn.priv = n;
    n->fs = fs;
    n->cluster = entry_cluster(fs, f->raw);
    n->size = dir ? 0 : le32(f->raw + 28);
    n->parent = parent;
    n->entry = f->index;
    if (dir)
        n->cluster = dir_key(fs, n->cluster);
    *out = &n->vn;
    return 0;
}

static void fat_time(uint32_t epoch, uint16_t *date, uint16_t *clock)
{
    struct rtc_time t;
    time_to_rtc(epoch, &t);
    int year = t.year < 1980 ? 1980 : t.year;
    *date = (uint16_t)((year - 1980) << 9 | t.month << 5 | t.day);
    *clock = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
}

static uint32_t epoch_of(uint16_t date, uint16_t clock)
{
    struct rtc_time t = {
        .year = (uint16_t)(1980 + (date >> 9)),
        .month = (uint8_t)MAX((date >> 5) & 15, 1),
        .day = (uint8_t)MAX(date & 31, 1),
        .hour = (uint8_t)(clock >> 11),
        .minute = (uint8_t)((clock >> 5) & 63),
        .second = (uint8_t)((clock & 31) * 2),
    };
    return time_from_rtc(&t);
}

static int refresh(struct fat_node *n)
{
    if (n->root)
        return 0;
    uint8_t *e;
    uint32_t lba;
    int err = dir_entry(n->fs, n->parent, n->entry, &e, &lba);
    if (err)
        return err;
    if (n->vn.type == VN_FILE) {
        uint32_t c = entry_cluster(n->fs, e);
        if (c != n->cluster)
            n->walk_index = n->walk_cluster = 0;
        n->cluster = c;
        n->size = le32(e + 28);
    }
    return 0;
}

static int update_entry(struct fat_node *n, bool touch)
{
    if (n->root)
        return 0;
    uint8_t *e;
    uint32_t lba;
    int err = dir_entry(n->fs, n->parent, n->entry, &e, &lba);
    if (err)
        return err;
    if (n->vn.type == VN_FILE) {
        put16(e + 26, n->cluster);
        if (n->fs->bits == 32)
            put16(e + 20, n->cluster >> 16);
        put32(e + 28, n->size);
    }
    if (touch) {
        uint16_t date, clock;
        fat_time(time_now(), &date, &clock);
        put16(e + 22, clock);
        put16(e + 24, date);
        put16(e + 18, date);
        e[11] |= n->vn.type == VN_FILE ? ATTR_ARCHIVE : 0;
    }
    mark_dirty(n->fs, lba);
    return 0;
}

static int node_cluster(struct fat_node *n, uint32_t index, bool extend, uint32_t *out)
{
    struct fat *fs = n->fs;
    if (!valid_cluster(fs, n->cluster)) {
        if (!extend)
            return E_IO;
        int err = alloc_cluster(fs, 0, false, &n->cluster);
        if (err)
            return err;
        n->walk_index = n->walk_cluster = 0;
        if ((err = update_entry(n, false)))
            return err;
    }
    uint32_t i = 0, c = n->cluster;
    if (n->walk_cluster && n->walk_index <= index) {
        i = n->walk_index;
        c = n->walk_cluster;
    }
    for (; i < index; i++) {
        uint32_t next;
        int err = fat_get(fs, c, &next);
        if (err)
            return err;
        if (!valid_cluster(fs, next)) {
            if (!extend)
                return E_IO;
            if ((err = alloc_cluster(fs, c, false, &next)))
                return err;
        }
        c = next;
    }
    n->walk_index = index;
    n->walk_cluster = c;
    *out = c;
    return 0;
}

static long file_io(struct fat_node *n, uint32_t off, uint8_t *buf, size_t len, bool write)
{
    struct fat *fs = n->fs;
    if (!write) {
        if (off >= n->size)
            return 0;
        len = MIN(len, n->size - off);
    }
    size_t done = 0;
    while (done < len) {
        uint32_t pos = off + (uint32_t)done, within = pos % fs->cluster_bytes, c;
        uint32_t index = pos / fs->cluster_bytes;
        int err = node_cluster(n, index, write, &c);
        if (err)
            return done ? (long)done : err;
        uint32_t span = fs->cluster_bytes - within;
        for (uint32_t k = 1; within % BLOCK_SIZE == 0 && span < len - done && span < (1u << 20); k++) {
            uint32_t next;
            if (node_cluster(n, index + k, write, &next) || next != c + k)
                break;
            span += fs->cluster_bytes;
        }
        uint32_t chunk = (uint32_t)MIN(len - done, span);
        uint32_t lba = cluster_lba(fs, c) + within / BLOCK_SIZE, at = within % BLOCK_SIZE;
        if (at == 0 && chunk >= BLOCK_SIZE) {
            uint32_t count = chunk / BLOCK_SIZE;
            chunk = count * BLOCK_SIZE;
            if (write) {
                err = data_write(fs, lba, count, buf + done);
                cache_drop(fs, lba, count);
            } else if (!(err = wb_sync(fs, lba, count))) {
                err = block_read(fs->dev, lba, count, buf + done);
            }
        } else {
            chunk = MIN(chunk, BLOCK_SIZE - at);
            uint8_t *p = sector(fs, lba, &err);
            if (p) {
                if (write) {
                    memcpy(p + at, buf + done, chunk);
                    err = sector_sync(fs, lba);
                } else {
                    memcpy(buf + done, p + at, chunk);
                }
            }
        }
        if (err)
            return done ? (long)done : err;
        done += chunk;
    }
    if (write) {
        if (off + done > n->size)
            n->size = off + (uint32_t)done;
        int err = update_entry(n, true);
        if (err)
            return err;
    }
    return (long)done;
}

static int f_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct fat_node *d = node(dir);
    struct found f;
    int err = dir_find(d->fs, d->cluster, name, &f);
    if (err)
        return err;
    return node_new(d->fs, dir->mount, d->cluster, &f, out);
}

static bool short_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

static bool make_short(const char *name, uint8_t out[11], uint8_t *flags)
{
    memset(out, ' ', 11);
    *flags = 0;
    const char *dot = strrchr(name, '.');
    if (dot == name)
        dot = NULL;
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    bool lossy = base_len > 8 || (dot && strlen(dot + 1) > 3) || base_len == 0;
    bool lower[2] = {false, false}, upper[2] = {false, false};
    int n = 0;
    for (size_t i = 0; i < base_len; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') {
            lossy = true;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            lower[0] = true;
            c = (char)(c - 32);
        } else if (c >= 'A' && c <= 'Z') {
            upper[0] = true;
        }
        if (!short_char(c)) {
            lossy = true;
            c = '_';
        }
        if (n < 8)
            out[n++] = (uint8_t)c;
    }
    if (dot) {
        n = 8;
        for (const char *p = dot + 1; *p; p++) {
            char c = *p;
            if (c == ' ') {
                lossy = true;
                continue;
            }
            if (c >= 'a' && c <= 'z') {
                lower[1] = true;
                c = (char)(c - 32);
            } else if (c >= 'A' && c <= 'Z') {
                upper[1] = true;
            }
            if (!short_char(c)) {
                lossy = true;
                c = '_';
            }
            if (n < 11)
                out[n++] = (uint8_t)c;
        }
    }
    if (out[0] == 0xe5)
        out[0] = 0x05;
    if ((lower[0] && upper[0]) || (lower[1] && upper[1]))
        lossy = true;
    if (!lossy)
        *flags = (uint8_t)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return lossy;
}

static bool short_exists(struct fat *fs, uint32_t dir, const uint8_t *name)
{
    for (uint32_t i = 0;; i++) {
        uint8_t *e;
        uint32_t lba;
        if (dir_entry(fs, dir, i, &e, &lba) || e[0] == 0)
            return false;
        if (e[0] != 0xe5 && (e[11] & 0x3f) != ATTR_LFN && memcmp(e, name, 11) == 0)
            return true;
    }
}

static int unique_short(struct fat *fs, uint32_t dir, uint8_t *name)
{
    int base = 0;
    while (base < 8 && name[base] != ' ')
        base++;
    for (uint32_t k = 1; k < 1000000; k++) {
        char tail[8];
        int len = 0;
        for (uint32_t v = k; v; v /= 10)
            tail[len++] = (char)('0' + v % 10);
        int keep = MIN(base, 7 - len);
        uint8_t candidate[11];
        memcpy(candidate, name, 11);
        memset(candidate + keep, ' ', (size_t)(8 - keep));
        candidate[keep] = '~';
        for (int i = 0; i < len; i++)
            candidate[keep + 1 + i] = (uint8_t)tail[len - 1 - i];
        if (!short_exists(fs, dir, candidate)) {
            memcpy(name, candidate, 11);
            return 0;
        }
    }
    return E_EXIST;
}

static int dir_alloc(struct fat *fs, uint32_t dir, int count, uint32_t *start)
{
    int run = 0;
    for (uint32_t i = 0;; i++) {
        uint8_t *e;
        uint32_t lba;
        int err = dir_entry(fs, dir, i, &e, &lba);
        if (err == E_NOENT) {
            if ((err = dir_extend(fs, dir)))
                return err;
            err = dir_entry(fs, dir, i, &e, &lba);
        }
        if (err)
            return err;
        if (e[0] == 0 || e[0] == 0xe5) {
            if (run++ == 0)
                *start = i;
            if (run == count)
                return 0;
        } else {
            run = 0;
        }
    }
}

static int decode_utf8(const char *s, uint16_t *out, int cap)
{
    int n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && n < cap;) {
        uint32_t c = *p++;
        if (c >= 0xe0 && p[0] && p[1]) {
            c = (c & 0x0f) << 12 | (uint32_t)(p[0] & 0x3f) << 6 | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0 && p[0]) {
            c = (c & 0x1f) << 6 | (p[0] & 0x3f);
            p++;
        }
        out[n++] = (uint16_t)c;
    }
    return n;
}

static int write_entries(struct fat *fs, uint32_t dir, const char *name, const uint8_t *templ, uint32_t *index)
{
    uint8_t entry[ENTRY], flags;
    memcpy(entry, templ, ENTRY);
    bool lossy = make_short(name, entry, &flags);
    uint16_t wide[260];
    int len = decode_utf8(name, wide, 255);
    for (int i = 0; i < len; i++)
        if (wide[i] >= 0x80)
            lossy = true;
    if (lossy) {
        int err = unique_short(fs, dir, entry);
        if (err)
            return err;
        entry[12] = 0;
    } else {
        entry[12] = flags;
    }
    int lfn = lossy ? (len + 12) / 13 : 0;
    uint32_t start;
    int err = dir_alloc(fs, dir, lfn + 1, &start);
    if (err)
        return err;
    uint8_t sum = checksum(entry);
    static const uint8_t slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int k = 0; k < lfn; k++) {
        int seq = lfn - k;
        uint8_t *e;
        uint32_t lba;
        if ((err = dir_entry(fs, dir, start + (uint32_t)k, &e, &lba)))
            return err;
        memset(e, 0, ENTRY);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        for (int c = 0; c < 13; c++) {
            int at = (seq - 1) * 13 + c;
            uint16_t ch = at < len ? wide[at] : at == len ? 0 : 0xffff;
            put16(e + slots[c], ch);
        }
        if ((err = sector_sync(fs, lba)))
            return err;
    }
    uint8_t *e;
    uint32_t lba;
    if ((err = dir_entry(fs, dir, start + (uint32_t)lfn, &e, &lba)))
        return err;
    memcpy(e, entry, ENTRY);
    *index = start + (uint32_t)lfn;
    return sector_sync(fs, lba);
}

static bool name_ok(const char *name)
{
    if (!*name || strlen(name) > 255 || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (const char *p = name; *p; p++)
        if ((uint8_t)*p < 32 || strchr("\\/:*?\"<>|", *p))
            return false;
    return true;
}

static int f_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    struct fat_node *d = node(dir);
    struct fat *fs = d->fs;
    if (!name_ok(name))
        return E_INVAL;
    struct found f;
    if (dir_find(fs, d->cluster, name, &f) == 0)
        return E_EXIST;
    uint8_t entry[ENTRY] = {0};
    uint16_t date, clock;
    fat_time(time_now(), &date, &clock);
    put16(entry + 14, clock);
    put16(entry + 16, date);
    put16(entry + 18, date);
    put16(entry + 22, clock);
    put16(entry + 24, date);
    entry[11] = type == VN_DIR ? ATTR_DIR : ATTR_ARCHIVE;
    uint32_t cluster = 0;
    int err;
    if (type == VN_DIR) {
        if ((err = alloc_cluster(fs, 0, true, &cluster)))
            return err;
        put16(entry + 26, cluster);
        put16(entry + 20, fs->bits == 32 ? cluster >> 16 : 0);
        uint8_t *e;
        uint32_t lba;
        if ((err = dir_entry(fs, cluster, 0, &e, &lba)))
            return err;
        memcpy(e, entry, ENTRY);
        memset(e, ' ', 11);
        e[0] = '.';
        memcpy(e + ENTRY, entry, ENTRY);
        memset(e + ENTRY, ' ', 11);
        e[ENTRY] = e[ENTRY + 1] = '.';
        uint32_t up = d->root ? 0 : d->cluster;
        put16(e + ENTRY + 26, up);
        put16(e + ENTRY + 20, fs->bits == 32 ? up >> 16 : 0);
        if ((err = sector_sync(fs, lba)))
            return err;
    }
    if ((err = write_entries(fs, d->cluster, name, entry, &f.index))) {
        if (cluster)
            free_chain(fs, cluster);
        return err;
    }
    memcpy(f.raw, entry, ENTRY);
    return node_new(fs, dir->mount, d->cluster, &f, out);
}

static int clear_entries(struct fat *fs, uint32_t dir, uint32_t first, uint32_t last)
{
    for (uint32_t i = first; i <= last; i++) {
        uint8_t *e;
        uint32_t lba;
        int err = dir_entry(fs, dir, i, &e, &lba);
        if (err)
            return err;
        e[0] = 0xe5;
        if ((err = sector_sync(fs, lba)))
            return err;
    }
    return 0;
}

static bool dir_empty(struct fat *fs, uint32_t dir)
{
    uint32_t index = 0;
    struct found f;
    return dir_next(fs, dir, &index, &f) == E_NOENT;
}

static int f_unlink(struct vnode *dir, const char *name)
{
    struct fat_node *d = node(dir);
    struct fat *fs = d->fs;
    struct found f;
    int err = dir_find(fs, d->cluster, name, &f);
    if (err)
        return err;
    uint32_t cluster = entry_cluster(fs, f.raw);
    if ((f.raw[11] & ATTR_DIR) && valid_cluster(fs, cluster) && !dir_empty(fs, cluster))
        return E_NOTEMPTY;
    if ((err = clear_entries(fs, d->cluster, f.first, f.index)))
        return err;
    free_chain(fs, cluster);
    return 0;
}

static int f_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname)
{
    struct fat_node *o = node(odir), *n = node(ndir);
    struct fat *fs = o->fs;
    if (!name_ok(nname))
        return E_INVAL;
    struct found src, dst;
    int err = dir_find(fs, o->cluster, oname, &src);
    if (err)
        return err;
    if (dir_find(fs, n->cluster, nname, &dst) == 0) {
        if (o->cluster == n->cluster && dst.index == src.index) {
            if (!strcmp(oname, nname))
                return 0;
        } else {
            if (dst.raw[11] & ATTR_DIR)
                return E_ISDIR;
            if ((err = f_unlink(ndir, nname)) || (err = dir_find(fs, o->cluster, oname, &src)))
                return err;
        }
    }
    uint32_t index;
    if ((err = write_entries(fs, n->cluster, nname, src.raw, &index)))
        return err;
    if ((err = clear_entries(fs, o->cluster, src.first, src.index)))
        return err;
    uint32_t cluster = entry_cluster(fs, src.raw);
    if ((src.raw[11] & ATTR_DIR) && o->cluster != n->cluster && valid_cluster(fs, cluster)) {
        uint8_t *e;
        uint32_t lba;
        if (dir_entry(fs, cluster, 1, &e, &lba) == 0 && e[0] == '.' && e[1] == '.') {
            uint32_t up = n->root ? 0 : n->cluster;
            put16(e + 26, up);
            put16(e + 20, fs->bits == 32 ? up >> 16 : 0);
            sector_sync(fs, lba);
        }
    }
    return 0;
}

static int f_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct fat_node *d = node(dir);
    struct found f;
    uint32_t at = 0;
    for (uint32_t seen = 0;; seen++) {
        int err = dir_next(d->fs, d->cluster, &at, &f);
        if (err)
            return err;
        if (seen == index)
            break;
    }
    str_copy(out->name, f.name, sizeof(out->name));
    out->type = f.raw[11] & ATTR_DIR ? VN_DIR : VN_FILE;
    out->size = out->type == VN_DIR ? 0 : le32(f.raw + 28);
    return 0;
}

static long f_read(struct vnode *vn, uint32_t off, void *buf, size_t n)
{
    struct fat_node *f = node(vn);
    int err = refresh(f);
    return err ? err : file_io(f, off, buf, n, false);
}

static long f_write(struct vnode *vn, uint32_t off, const void *buf, size_t n)
{
    struct fat_node *f = node(vn);
    int err = refresh(f);
    if (err)
        return err;
    if (off > f->size) {
        static const uint8_t zeros[BLOCK_SIZE];
        while (f->size < off) {
            long k = file_io(f, f->size, (uint8_t *)zeros, MIN(off - f->size, BLOCK_SIZE), true);
            if (k <= 0)
                return k < 0 ? k : E_IO;
        }
    }
    return file_io(f, off, (uint8_t *)buf, n, true);
}

static int f_truncate(struct vnode *vn, uint32_t size)
{
    struct fat_node *f = node(vn);
    struct fat *fs = f->fs;
    int err = refresh(f);
    if (err)
        return err;
    if (size > f->size) {
        long n = f_write(vn, size, "", 0);
        return n < 0 ? (int)n : 0;
    }
    uint32_t keep = (size + fs->cluster_bytes - 1) / fs->cluster_bytes;
    if (valid_cluster(fs, f->cluster)) {
        if (!keep) {
            free_chain(fs, f->cluster);
            f->cluster = 0;
        } else {
            uint32_t last, next;
            if ((err = chain_at(fs, f->cluster, keep - 1, &last)) == 0 && (err = fat_get(fs, last, &next)) == 0 &&
                valid_cluster(fs, next)) {
                fat_set(fs, last, 0x0fffffff);
                free_chain(fs, next);
            }
        }
    }
    f->walk_index = f->walk_cluster = 0;
    f->size = size;
    return update_entry(f, true);
}

static int f_stat(struct vnode *vn, struct vstat *st)
{
    struct fat_node *f = node(vn);
    int err = refresh(f);
    if (err)
        return err;
    st->size = f->size;
    st->inode = f->cluster;
    st->mtime = 0;
    if (!f->root) {
        uint8_t *e;
        uint32_t lba;
        if (dir_entry(f->fs, f->parent, f->entry, &e, &lba) == 0)
            st->mtime = epoch_of(le16(e + 24), le16(e + 22));
    }
    return 0;
}

static void f_release(struct vnode *vn)
{
    fat_flush(node(vn)->fs);
    kfree(node(vn));
}

static int flushed(struct vnode *vn, int err)
{
    int f = fat_flush(node(vn)->fs);
    return err < 0 ? err : f ? f : err;
}

static int fs_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    return flushed(dir, f_create(dir, name, type, out));
}

static int fs_unlink(struct vnode *dir, const char *name)
{
    return flushed(dir, f_unlink(dir, name));
}

static int fs_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname)
{
    return flushed(odir, f_rename(odir, oname, ndir, nname));
}

static int fs_truncate(struct vnode *vn, uint32_t size)
{
    return flushed(vn, f_truncate(vn, size));
}

static const struct vnode_ops fat_ops = {
    .lookup = f_lookup,
    .create = fs_create,
    .unlink = fs_unlink,
    .rename = fs_rename,
    .readdir = f_readdir,
    .read = f_read,
    .write = f_write,
    .truncate = fs_truncate,
    .stat = f_stat,
    .release = f_release,
};

static bool parse_bpb(struct blockdev *dev, struct fat *fs)
{
    uint8_t b[BLOCK_SIZE];
    if (block_read(dev, 0, 1, b) != 0 || b[510] != 0x55 || b[511] != 0xaa)
        return false;
    uint32_t bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16];
    uint32_t root_entries = le16(b + 17), total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    uint32_t fat_size = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if (bps != BLOCK_SIZE || !spc || (spc & (spc - 1)) || !reserved || !fats || fats > 4 || !fat_size || !total ||
        total > dev->sectors || (b[0] != 0xeb && b[0] != 0xe9))
        return false;
    uint32_t root_sectors = (root_entries * ENTRY + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t meta = reserved + fats * fat_size + root_sectors;
    if (meta >= total)
        return false;
    fs->dev = dev;
    fs->spc = spc;
    fs->cluster_bytes = spc * BLOCK_SIZE;
    fs->fat_start = reserved;
    fs->fat_sectors = fat_size;
    fs->fats = fats;
    fs->root_start = reserved + fats * fat_size;
    fs->root_entries = root_entries;
    fs->data_start = meta;
    fs->clusters = (total - meta) / spc;
    fs->bits = fs->clusters < 4085 ? 12 : fs->clusters < 65525 ? 16 : 32;
    if (fs->bits == 32) {
        if (root_entries)
            return false;
        fs->root_cluster = le32(b + 44);
        if (!valid_cluster(fs, fs->root_cluster))
            return false;
    } else if (!root_entries) {
        return false;
    }
    fs->hint = 2;
    return true;
}

bool fat_detect(struct blockdev *dev)
{
    struct fat *fs = kzalloc(sizeof(*fs));
    bool ok = fs && parse_bpb(dev, fs);
    kfree(fs);
    return ok;
}

static int fat_mount(const char *source, struct vnode **root)
{
    struct blockdev *dev = source ? block_find(source) : NULL;
    if (!dev)
        return E_NOENT;
    struct fat *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return E_NOMEM;
    if (!parse_bpb(dev, fs)) {
        kfree(fs);
        return E_INVAL;
    }
    struct fat_node *n = kzalloc(sizeof(*n));
    if (!n) {
        kfree(fs);
        return E_NOMEM;
    }
    n->vn.ops = &fat_ops;
    n->vn.type = VN_DIR;
    n->vn.refs = 1;
    n->vn.priv = n;
    n->fs = fs;
    n->root = true;
    n->cluster = fs->bits == 32 ? fs->root_cluster : 0;
    *root = &n->vn;
    klog("fat: %s FAT%d, %u clusters of %u bytes", dev->name, fs->bits, (unsigned)fs->clusters,
         (unsigned)fs->cluster_bytes);
    return 0;
}

static int fat_statfs(struct vnode *root, uint64_t *total, uint64_t *free)
{
    struct fat *fs = node(root)->fs;
    if (!fs->free_known) {
        uint32_t count = 0;
        for (uint32_t c = 2; c < fs->clusters + 2; c++) {
            uint32_t v;
            if (fat_get(fs, c, &v))
                return E_IO;
            count += v == 0;
        }
        fs->free_count = count;
        fs->free_known = true;
    }
    *total = (uint64_t)fs->clusters * fs->cluster_bytes;
    *free = (uint64_t)fs->free_count * fs->cluster_bytes;
    return 0;
}

static void copy_label(const uint8_t *raw, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < 11 && n + 1 < cap; i++)
        out[n++] = raw[i] >= 32 && raw[i] < 127 ? (char)raw[i] : '_';
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    if (!strcmp(out, "NO NAME"))
        out[0] = 0;
}

static int fat_label(struct vnode *root, char *out, size_t cap)
{
    struct fat_node *r = node(root);
    struct fat *fs = r->fs;
    out[0] = 0;
    for (uint32_t i = 0; i < 64; i++) {
        uint8_t *e;
        uint32_t lba;
        if (dir_entry(fs, r->cluster, i, &e, &lba) || e[0] == 0)
            break;
        if (e[0] != 0xe5 && (e[11] & 0x3f) != ATTR_LFN && (e[11] & ATTR_VOLUME)) {
            copy_label(e, out, cap);
            return 0;
        }
    }
    int err;
    uint8_t *b = sector(fs, 0, &err);
    if (b && b[fs->bits == 32 ? 66 : 38] == 0x29)
        copy_label(b + (fs->bits == 32 ? 71 : 43), out, cap);
    return 0;
}

static const struct fs_type fat_type = {
    .name = "fat",
    .description = "FAT12/16/32 with long names (USB drives, SD cards)",
    .mount = fat_mount,
    .statfs = fat_statfs,
    .label = fat_label,
};
FS_TYPE(fat_type);
