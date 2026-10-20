#include <synth/block.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

#define SFS_MAGIC "SYNTHFS1"
#define SFS_VERSION 1
#define SFS_DIRECT 10
#define SFS_PTRS (BLOCK_SIZE / 4)
#define SFS_INODES_PER_BLOCK (BLOCK_SIZE / sizeof(struct sfs_inode))
#define SFS_DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(struct sfs_dirent))
#define SFS_NAME_MAX 255
#define SFS_PART 58
#define SFS_ROOT 1
#define SFS_CACHE 128
#define SFS_WAYS 4

enum {
    SFS_FREE = 0,
    SFS_FILE = 1,
    SFS_DIR = 2,
    SFS_CONT = 3,
};

struct sfs_super {
    char magic[8];
    uint32_t version;
    uint32_t blocks;
    uint32_t inodes;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t data_start;
    uint32_t created;
};

struct sfs_inode {
    uint16_t type;
    uint16_t reserved;
    uint32_t size;
    uint32_t mtime;
    uint32_t direct[SFS_DIRECT];
    uint32_t indirect;
    uint32_t double_indirect;
    uint32_t triple_indirect;
};

struct sfs_dirent {
    uint32_t ino;
    uint8_t type;
    uint8_t len;
    char name[58];
};

_Static_assert(sizeof(struct sfs_inode) == 64, "inode layout");
_Static_assert(sizeof(struct sfs_dirent) == 64, "dirent layout");

struct cache_line {
    uint32_t block, used;
    bool valid, dirty;
    uint8_t data[BLOCK_SIZE];
};

struct sfs {
    struct blockdev *dev;
    struct sfs_super sb;
    uint8_t *bitmap;
    uint32_t free_blocks;
    uint32_t hint;
    bool batch, zero_data;
    uint32_t bitmap_lo, bitmap_hi;
    uint8_t *ra;
    uint32_t ra_start, ra_count, clock, inode_hint;
    struct cache_line cache[SFS_CACHE];
};

struct sfs_node {
    struct vnode vn;
    struct sfs *fs;
    uint32_t ino;
};

static const struct vnode_ops sfs_ops;

/* Metadata written during one file_io (indirect tables) stays in the cache marked dirty and goes to the disk
   once at the end; a line is written back before another block takes its place. */
static int write_back(struct sfs *fs, struct cache_line *c)
{
    if (!c->dirty)
        return 0;
    c->dirty = false;
    return block_write(fs->dev, c->block, 1, c->data);
}

/* The cache is set-associative: a big file's tables at different levels often fall on the same set, and with one
   line per set they pushed each other out on every block. */
static struct cache_line *cached(struct sfs *fs, uint32_t blk)
{
    struct cache_line *set = &fs->cache[blk % (SFS_CACHE / SFS_WAYS) * SFS_WAYS];
    for (int i = 0; i < SFS_WAYS; i++)
        if (set[i].valid && set[i].block == blk)
            return &set[i];
    return NULL;
}

static struct cache_line *victim(struct sfs *fs, uint32_t blk)
{
    struct cache_line *set = &fs->cache[blk % (SFS_CACHE / SFS_WAYS) * SFS_WAYS], *v = set;
    for (int i = 0; i < SFS_WAYS; i++) {
        if (!set[i].valid)
            return &set[i];
        if (set[i].used < v->used)
            v = &set[i];
    }
    return v;
}

static int bread(struct sfs *fs, uint32_t blk, void *buf)
{
    struct cache_line *c = cached(fs, blk);
    if (!c) {
        c = victim(fs, blk);
        int err = write_back(fs, c);
        if (!err)
            err = block_read(fs->dev, blk, 1, c->data);
        if (err) {
            c->valid = false;
            return err;
        }
        c->block = blk;
        c->valid = true;
    }
    c->used = ++fs->clock;
    memcpy(buf, c->data, BLOCK_SIZE);
    return 0;
}

/* Read-ahead: a small read brings up to 64 KB of the file's next neighbouring blocks in one command (a command costs
   about the same whatever its size); any write or allocation forgets it. */
#define RA_BLOCKS 128

static void ra_forget(struct sfs *fs, uint32_t first, uint32_t count)
{
    if (first < fs->ra_start + fs->ra_count && fs->ra_start < first + count)
        fs->ra_count = 0;
}

static int bwrite(struct sfs *fs, uint32_t blk, const void *buf)
{
    ra_forget(fs, blk, 1);
    struct cache_line *c = cached(fs, blk);
    if (!c) {
        c = victim(fs, blk);
        int err = write_back(fs, c);
        if (err)
            return err;
    }
    c->used = ++fs->clock;
    if (fs->batch) {
        memcpy(c->data, buf, BLOCK_SIZE);
        c->block = blk;
        c->valid = c->dirty = true;
        return 0;
    }
    int err = block_write(fs->dev, blk, 1, buf);
    if (err) {
        c->valid = c->dirty = false;
        return err;
    }
    memcpy(c->data, buf, BLOCK_SIZE);
    c->block = blk;
    c->valid = true;
    c->dirty = false;
    return 0;
}

/* Data blocks written straight to the disk must not leave an older copy in the cache. */
static void forget(struct sfs *fs, uint32_t first, uint32_t count)
{
    for (uint32_t b = first; b < first + count; b++) {
        struct cache_line *c = cached(fs, b);
        if (c)
            c->valid = c->dirty = false;
    }
}

static int bitmap_sync(struct sfs *fs, uint32_t blk)
{
    uint32_t index = blk / (BLOCK_SIZE * 8);
    if (fs->batch) {
        fs->bitmap_lo = MIN(fs->bitmap_lo, index);
        fs->bitmap_hi = MAX(fs->bitmap_hi, index + 1);
        return 0;
    }
    return bwrite(fs, fs->sb.bitmap_start + index, fs->bitmap + index * BLOCK_SIZE);
}

static void batch_begin(struct sfs *fs)
{
    fs->batch = true;
    fs->bitmap_lo = UINT32_MAX;
    fs->bitmap_hi = 0;
}

static int batch_end(struct sfs *fs)
{
    fs->batch = false;
    int err = 0;
    for (uint32_t i = fs->bitmap_lo; i < fs->bitmap_hi && !err; i++)
        err = block_write(fs->dev, fs->sb.bitmap_start + i, 1, fs->bitmap + i * BLOCK_SIZE);
    for (int i = 0; i < SFS_CACHE && !err; i++)
        err = write_back(fs, &fs->cache[i]);
    return err;
}

static int balloc_raw(struct sfs *fs, uint32_t *out, bool zero)
{
    uint32_t total = fs->sb.blocks;
    for (uint32_t n = 0; n < total; n++) {
        uint32_t b = (fs->hint + n) % total;
        if (b < fs->sb.data_start)
            continue;
        if (fs->bitmap[b / 8] & (1u << (b % 8)))
            continue;
        fs->bitmap[b / 8] |= (uint8_t)(1u << (b % 8));
        int err = bitmap_sync(fs, b);
        if (err)
            return err;
        static const uint8_t zeros[BLOCK_SIZE];
        if (zero && (err = bwrite(fs, b, zeros)) != 0)
            return err;
        ra_forget(fs, b, 1);
        fs->free_blocks--;
        fs->hint = b + 1;
        *out = b;
        return 0;
    }
    return E_NOSPC;
}

static int balloc(struct sfs *fs, uint32_t *out)
{
    return balloc_raw(fs, out, true);
}

static void bfree(struct sfs *fs, uint32_t b)
{
    if (b < fs->sb.data_start || b >= fs->sb.blocks)
        return;
    if (!(fs->bitmap[b / 8] & (1u << (b % 8))))
        return;
    fs->bitmap[b / 8] &= (uint8_t) ~(1u << (b % 8));
    fs->free_blocks++;
    ra_forget(fs, b, 1);
    bitmap_sync(fs, b);
}

static int iget(struct sfs *fs, uint32_t ino, struct sfs_inode *out)
{
    if (ino == 0 || ino > fs->sb.inodes)
        return E_INVAL;
    uint8_t buf[BLOCK_SIZE];
    uint32_t idx = ino - 1;
    int err = bread(fs, fs->sb.inode_start + idx / SFS_INODES_PER_BLOCK, buf);
    if (err)
        return err;
    memcpy(out, buf + (idx % SFS_INODES_PER_BLOCK) * sizeof(*out), sizeof(*out));
    return 0;
}

static int iput(struct sfs *fs, uint32_t ino, const struct sfs_inode *in)
{
    uint8_t buf[BLOCK_SIZE];
    uint32_t idx = ino - 1;
    uint32_t blk = fs->sb.inode_start + idx / SFS_INODES_PER_BLOCK;
    int err = bread(fs, blk, buf);
    if (err)
        return err;
    memcpy(buf + (idx % SFS_INODES_PER_BLOCK) * sizeof(*in), in, sizeof(*in));
    if (in->type == SFS_FREE)
        fs->inode_hint = MIN(fs->inode_hint, idx / (uint32_t)SFS_INODES_PER_BLOCK);
    return bwrite(fs, blk, buf);
}

/* The blocks before inode_hint have no free inode, so creating a file does not read the whole table again. */
static int ialloc(struct sfs *fs, uint16_t type, uint32_t *out)
{
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = fs->inode_hint; b < fs->sb.inode_blocks; b++) {
        fs->inode_hint = b;
        int err = bread(fs, fs->sb.inode_start + b, buf);
        if (err)
            return err;
        for (uint32_t i = 0; i < SFS_INODES_PER_BLOCK; i++) {
            uint32_t ino = b * SFS_INODES_PER_BLOCK + i + 1;
            struct sfs_inode *in = (struct sfs_inode *)(buf + i * sizeof(struct sfs_inode));
            if (ino > fs->sb.inodes)
                return E_NOSPC;
            if (in->type != SFS_FREE)
                continue;
            memset(in, 0, sizeof(*in));
            in->type = type;
            in->mtime = time_now();
            err = bwrite(fs, fs->sb.inode_start + b, buf);
            if (err)
                return err;
            *out = ino;
            return 0;
        }
    }
    return E_NOSPC;
}

static int table_entry(struct sfs *fs, uint32_t *table_blk, uint32_t index, bool alloc, uint32_t *out, bool *dirty,
                       bool leaf)
{
    uint32_t table[SFS_PTRS];
    if (!*table_blk) {
        if (!alloc) {
            *out = 0;
            return 0;
        }
        int err = balloc(fs, table_blk);
        if (err)
            return err;
        *dirty = true;
    }
    int err = bread(fs, *table_blk, table);
    if (err)
        return err;
    if (!table[index] && alloc) {
        err = balloc_raw(fs, &table[index], fs->zero_data || !leaf);
        if (err)
            return err;
        err = bwrite(fs, *table_blk, table);
        if (err)
            return err;
    }
    *out = table[index];
    return 0;
}

static int bmap(struct sfs *fs, struct sfs_inode *in, uint32_t index, bool alloc, uint32_t *out, bool *dirty)
{
    if (index < SFS_DIRECT) {
        if (!in->direct[index] && alloc) {
            int err = balloc_raw(fs, &in->direct[index], fs->zero_data);
            if (err)
                return err;
            *dirty = true;
        }
        *out = in->direct[index];
        return 0;
    }
    index -= SFS_DIRECT;
    if (index < SFS_PTRS)
        return table_entry(fs, &in->indirect, index, alloc, out, dirty, true);
    index -= SFS_PTRS;
    uint32_t *top = &in->double_indirect, span = SFS_PTRS;
    if (index >= SFS_PTRS * SFS_PTRS) {
        index -= SFS_PTRS * SFS_PTRS;
        if (index >= SFS_PTRS * SFS_PTRS * SFS_PTRS)
            return E_NOSPC;
        top = &in->triple_indirect;
        span = SFS_PTRS * SFS_PTRS;
    }
    uint32_t table;
    int err = table_entry(fs, top, index / span, alloc, &table, dirty, false);
    bool unused = false;
    for (index %= span, span /= SFS_PTRS; !err && table && span; index %= span, span /= SFS_PTRS)
        err = table_entry(fs, &table, index / span, alloc, &table, &unused, span == 1);
    *out = err ? 0 : table;
    return err;
}

static void free_table(struct sfs *fs, uint32_t *table_blk, uint32_t start)
{
    if (!*table_blk)
        return;
    uint32_t table[SFS_PTRS];
    if (bread(fs, *table_blk, table) != 0)
        return;
    for (uint32_t i = start; i < SFS_PTRS; i++) {
        bfree(fs, table[i]);
        table[i] = 0;
    }
    if (start == 0) {
        bfree(fs, *table_blk);
        *table_blk = 0;
    } else {
        bwrite(fs, *table_blk, table);
    }
}

/* Frees the blocks of a tree of tables at and after `start` (counted in data blocks below it). */
static void free_tree(struct sfs *fs, uint32_t *table_blk, uint32_t start, uint32_t span)
{
    if (!*table_blk)
        return;
    if (span == 1) {
        free_table(fs, table_blk, start);
        return;
    }
    uint32_t table[SFS_PTRS];
    if (bread(fs, *table_blk, table) != 0)
        return;
    for (uint32_t a = 0; a < SFS_PTRS; a++) {
        uint32_t lo = a * span;
        if (!table[a] || lo + span <= start)
            continue;
        free_tree(fs, &table[a], start > lo ? start - lo : 0, span / SFS_PTRS);
    }
    if (start == 0) {
        bfree(fs, *table_blk);
        *table_blk = 0;
    } else {
        bwrite(fs, *table_blk, table);
    }
}

static void free_blocks(struct sfs *fs, struct sfs_inode *in, uint32_t keep)
{
    for (uint32_t i = keep; i < SFS_DIRECT; i++) {
        bfree(fs, in->direct[i]);
        in->direct[i] = 0;
    }
    uint32_t start = keep > SFS_DIRECT ? keep - SFS_DIRECT : 0;
    if (start < SFS_PTRS)
        free_table(fs, &in->indirect, start);
    uint32_t base = SFS_DIRECT + SFS_PTRS;
    free_tree(fs, &in->double_indirect, keep > base ? keep - base : 0, SFS_PTRS);
    base += SFS_PTRS * SFS_PTRS;
    free_tree(fs, &in->triple_indirect, keep > base ? keep - base : 0, SFS_PTRS * SFS_PTRS);
}

#define RUN_MAX 256

/* Whole blocks move in runs of neighbouring disk blocks, one command each; a block that is about to be
   overwritten completely is not zeroed first; the bitmap and the tables go to the disk once, at the end. */
static long file_io(struct sfs *fs, uint32_t ino, struct sfs_inode *in, uint32_t off, void *buf, size_t n, bool write)
{
    uint8_t block[BLOCK_SIZE];
    bool dirty = false;
    size_t done = 0;
    if (!write) {
        if (off >= in->size)
            return 0;
        n = MIN(n, (size_t)(in->size - off));
    }
    batch_begin(fs);
    int err = 0;
    while (done < n) {
        uint32_t index = (off + (uint32_t)done) / BLOCK_SIZE;
        uint32_t within = (off + (uint32_t)done) % BLOCK_SIZE;
        size_t chunk = MIN(n - done, (size_t)(BLOCK_SIZE - within));
        uint8_t *at = (uint8_t *)buf + done;
        uint32_t blk;
        bool whole = chunk == BLOCK_SIZE;
        fs->zero_data = !whole;
        if ((err = bmap(fs, in, index, write, &blk, &dirty)) != 0)
            break;
        if (!write && blk && blk - fs->ra_start < fs->ra_count) {
            memcpy(at, fs->ra + (size_t)(blk - fs->ra_start) * BLOCK_SIZE + within, chunk);
            done += chunk;
            continue;
        }
        if (!write && blk && fs->ra && in->type == SFS_FILE && n - done < (size_t)RA_BLOCKS * BLOCK_SIZE) {
            uint32_t count = 1, blocks = (in->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
            for (uint32_t next; count < RA_BLOCKS && index + count < blocks; count++)
                if (bmap(fs, in, index + count, false, &next, &dirty) != 0 || next != blk + count)
                    break;
            if (count > 1) {
                fs->ra_count = 0;
                if ((err = block_read(fs->dev, blk, count, fs->ra)) != 0)
                    break;
                fs->ra_start = blk;
                fs->ra_count = count;
                continue;
            }
        }
        if (whole && blk) {
            uint32_t count = 1;
            while (count < RUN_MAX && n - done >= (size_t)(count + 1) * BLOCK_SIZE) {
                uint32_t next;
                if ((err = bmap(fs, in, index + count, write, &next, &dirty)) != 0 || next != blk + count)
                    break;
                count++;
            }
            if (err)
                break;
            for (uint32_t k = 0; k < count && !write; k++) {
                struct cache_line *c = cached(fs, blk + k);
                if (c && c->dirty)
                    write_back(fs, c);
            }
            if (write)
                ra_forget(fs, blk, count);
            err = write ? block_write(fs->dev, blk, count, at) : block_read(fs->dev, blk, count, at);
            if (err)
                break;
            if (write)
                forget(fs, blk, count);
            done += (size_t)count * BLOCK_SIZE;
            continue;
        }
        if (!blk) {
            memset(at, 0, chunk);
        } else {
            if ((err = bread(fs, blk, block)) != 0)
                break;
            if (write) {
                memcpy(block + within, at, chunk);
                if ((err = bwrite(fs, blk, block)) != 0)
                    break;
            } else {
                memcpy(at, block + within, chunk);
            }
        }
        done += chunk;
    }
    fs->zero_data = false;
    int flushed = batch_end(fs);
    if (!err)
        err = flushed;
    if (err && !done)
        return err;
    if (write && done) {
        if (off + done > in->size)
            in->size = off + (uint32_t)done;
        in->mtime = time_now();
        dirty = true;
    }
    if (dirty) {
        int e = iput(fs, ino, in);
        if (e)
            return e;
    }
    return (long)done;
}

static struct sfs_node *node(struct vnode *vn)
{
    return (struct sfs_node *)vn;
}

static int node_new(struct sfs *fs, uint32_t ino, struct mount *mnt, struct vnode **out)
{
    struct sfs_inode in;
    int err = iget(fs, ino, &in);
    if (err)
        return err;
    if (in.type == SFS_FREE)
        return E_NOENT;
    struct sfs_node *n = kzalloc(sizeof(*n));
    if (!n)
        return E_NOMEM;
    n->vn.ops = &sfs_ops;
    n->vn.type = in.type == SFS_DIR ? VN_DIR : VN_FILE;
    n->vn.mount = mnt;
    n->vn.refs = 1;
    n->vn.priv = n;
    n->fs = fs;
    n->ino = ino;
    *out = &n->vn;
    return 0;
}

/* A name longer than one entry holds goes on in the slots right after it: SFS_CONT entries with no inode, which
   disks from before long names never have and which older kernels pass over as free. */
static uint32_t name_slots(size_t len)
{
    return len <= SFS_PART ? 1 : 1 + (uint32_t)((len - SFS_PART + SFS_PART - 1) / SFS_PART);
}

static int entry_read(struct sfs *fs, uint32_t dir, struct sfs_inode *in, uint32_t slot, struct sfs_dirent *ent)
{
    long n = file_io(fs, dir, in, slot * (uint32_t)sizeof(*ent), ent, sizeof(*ent), false);
    return n == (long)sizeof(*ent) ? 0 : n < 0 ? (int)n : E_IO;
}

static int entry_name(struct sfs *fs, uint32_t dir, struct sfs_inode *in, uint32_t slot, const struct sfs_dirent *ent,
                      char *out)
{
    size_t len = ent->len, at = MIN(len, (size_t)SFS_PART);
    memcpy(out, ent->name, at);
    for (uint32_t s = slot + 1; at < len; s++) {
        struct sfs_dirent c;
        int err = entry_read(fs, dir, in, s, &c);
        if (err)
            return err;
        if (c.ino || c.type != SFS_CONT)
            return E_IO;
        size_t part = MIN(len - at, (size_t)SFS_PART);
        memcpy(out + at, c.name, part);
        at += part;
    }
    out[len] = 0;
    return 0;
}

static int dir_find(struct sfs *fs, uint32_t dir, const char *name, uint32_t *slot, struct sfs_dirent *out)
{
    struct sfs_inode in;
    int err = iget(fs, dir, &in);
    if (err)
        return err;
    struct sfs_dirent ent;
    size_t len = strlen(name);
    uint32_t count = in.size / sizeof(ent);
    for (uint32_t i = 0; i < count; i++) {
        if ((err = entry_read(fs, dir, &in, i, &ent)) != 0)
            return err;
        if (!ent.ino || ent.len != len || memcmp(ent.name, name, MIN(len, (size_t)SFS_PART)) != 0)
            continue;
        if (len > SFS_PART) {
            char full[SFS_NAME_MAX + 1];
            if (entry_name(fs, dir, &in, i, &ent, full) != 0 || strcmp(full, name) != 0)
                continue;
        }
        if (slot)
            *slot = i;
        if (out)
            *out = ent;
        return 0;
    }
    return E_NOENT;
}

static int dir_add(struct sfs *fs, uint32_t dir, const char *name, uint32_t ino, uint8_t type)
{
    struct sfs_inode in;
    int err = iget(fs, dir, &in);
    if (err)
        return err;
    size_t len = strlen(name);
    uint32_t need = name_slots(len), count = in.size / sizeof(struct sfs_dirent), slot = count, run = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct sfs_dirent ent;
        bool free = entry_read(fs, dir, &in, i, &ent) == 0 && !ent.ino && ent.type != SFS_CONT;
        run = free ? run + 1 : 0;
        if (run == need) {
            slot = i + 1 - need;
            break;
        }
    }
    struct sfs_dirent ents[5];
    memset(ents, 0, sizeof(ents));
    ents[0].ino = ino;
    ents[0].type = type;
    ents[0].len = (uint8_t)len;
    for (uint32_t k = 0, at = 0; k < need; k++, at += SFS_PART) {
        if (k)
            ents[k].type = SFS_CONT;
        memcpy(ents[k].name, name + at, MIN(len - at, (size_t)SFS_PART));
    }
    size_t bytes = need * sizeof(ents[0]);
    long n = file_io(fs, dir, &in, slot * (uint32_t)sizeof(ents[0]), ents, bytes, true);
    return n == (long)bytes ? 0 : n < 0 ? (int)n : E_IO;
}

static int dir_clear(struct sfs *fs, uint32_t dir, uint32_t slot)
{
    struct sfs_inode in;
    int err = iget(fs, dir, &in);
    if (err)
        return err;
    struct sfs_dirent ents[5];
    if ((err = entry_read(fs, dir, &in, slot, &ents[0])) != 0)
        return err;
    uint32_t need = name_slots(ents[0].len);
    memset(ents, 0, sizeof(ents));
    size_t bytes = need * sizeof(ents[0]);
    long n = file_io(fs, dir, &in, slot * (uint32_t)sizeof(ents[0]), ents, bytes, true);
    return n == (long)bytes ? 0 : E_IO;
}

static bool dir_empty(struct sfs *fs, uint32_t dir)
{
    struct sfs_inode in;
    if (iget(fs, dir, &in) != 0)
        return false;
    struct sfs_dirent ent;
    for (uint32_t i = 0; i < in.size / sizeof(ent); i++)
        if (file_io(fs, dir, &in, i * (uint32_t)sizeof(ent), &ent, sizeof(ent), false) == (long)sizeof(ent) && ent.ino)
            return false;
    return true;
}

static int s_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct sfs_node *d = node(dir);
    struct sfs_dirent ent;
    int err = dir_find(d->fs, d->ino, name, NULL, &ent);
    if (err)
        return err;
    return node_new(d->fs, ent.ino, dir->mount, out);
}

static int s_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    struct sfs_node *d = node(dir);
    if (strlen(name) > SFS_NAME_MAX)
        return E_NAMETOOLONG;
    if (dir_find(d->fs, d->ino, name, NULL, NULL) == 0)
        return E_EXIST;
    uint32_t ino;
    uint16_t t = type == VN_DIR ? SFS_DIR : SFS_FILE;
    int err = ialloc(d->fs, t, &ino);
    if (err)
        return err;
    err = dir_add(d->fs, d->ino, name, ino, (uint8_t)t);
    if (err) {
        struct sfs_inode in = {0};
        iput(d->fs, ino, &in);
        return err;
    }
    return node_new(d->fs, ino, dir->mount, out);
}

static int release_inode(struct sfs *fs, uint32_t ino)
{
    struct sfs_inode in;
    int err = iget(fs, ino, &in);
    if (err)
        return err;
    free_blocks(fs, &in, 0);
    memset(&in, 0, sizeof(in));
    return iput(fs, ino, &in);
}

static int s_unlink(struct vnode *dir, const char *name)
{
    struct sfs_node *d = node(dir);
    uint32_t slot;
    struct sfs_dirent ent;
    int err = dir_find(d->fs, d->ino, name, &slot, &ent);
    if (err)
        return err;
    if (ent.type == SFS_DIR && !dir_empty(d->fs, ent.ino))
        return E_NOTEMPTY;
    err = dir_clear(d->fs, d->ino, slot);
    if (err)
        return err;
    return release_inode(d->fs, ent.ino);
}

static int s_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname)
{
    struct sfs_node *o = node(odir), *n = node(ndir);
    struct sfs *fs = o->fs;
    if (strlen(nname) > SFS_NAME_MAX)
        return E_NAMETOOLONG;
    uint32_t slot;
    struct sfs_dirent src, dst;
    int err = dir_find(fs, o->ino, oname, &slot, &src);
    if (err)
        return err;
    if (dir_find(fs, n->ino, nname, NULL, &dst) == 0) {
        if (dst.ino == src.ino)
            return 0;
        if (dst.type == SFS_DIR)
            return E_ISDIR;
        if ((err = s_unlink(ndir, nname)) != 0)
            return err;
        if (o->ino == n->ino && (err = dir_find(fs, o->ino, oname, &slot, &src)) != 0)
            return err;
    }
    if ((err = dir_add(fs, n->ino, nname, src.ino, src.type)) != 0)
        return err;
    if (o->ino == n->ino && (err = dir_find(fs, o->ino, oname, &slot, &src)) != 0)
        return err;
    return dir_clear(fs, o->ino, slot);
}

static int s_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct sfs_node *d = node(dir);
    struct sfs_inode in;
    int err = iget(d->fs, d->ino, &in);
    if (err)
        return err;
    struct sfs_dirent ent;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < in.size / sizeof(ent); i++) {
        if (entry_read(d->fs, d->ino, &in, i, &ent) != 0)
            return E_IO;
        if (!ent.ino || seen++ != index)
            continue;
        if ((err = entry_name(d->fs, d->ino, &in, i, &ent, out->name)) != 0)
            return err;
        out->type = ent.type == SFS_DIR ? VN_DIR : VN_FILE;
        struct sfs_inode child;
        out->size = iget(d->fs, ent.ino, &child) == 0 ? child.size : 0;
        return 0;
    }
    return E_NOENT;
}

static long s_read(struct vnode *vn, uint32_t off, void *buf, size_t n)
{
    struct sfs_node *f = node(vn);
    struct sfs_inode in;
    int err = iget(f->fs, f->ino, &in);
    return err ? err : file_io(f->fs, f->ino, &in, off, buf, n, false);
}

static long s_write(struct vnode *vn, uint32_t off, const void *buf, size_t n)
{
    struct sfs_node *f = node(vn);
    struct sfs_inode in;
    int err = iget(f->fs, f->ino, &in);
    return err ? err : file_io(f->fs, f->ino, &in, off, (void *)buf, n, true);
}

static int s_truncate(struct vnode *vn, uint32_t size)
{
    struct sfs_node *f = node(vn);
    struct sfs_inode in;
    int err = iget(f->fs, f->ino, &in);
    if (err)
        return err;
    if (size < in.size) {
        uint32_t keep = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        free_blocks(f->fs, &in, keep);
        if (size % BLOCK_SIZE) {
            uint32_t blk;
            bool dirty = false;
            uint8_t block[BLOCK_SIZE];
            if (bmap(f->fs, &in, size / BLOCK_SIZE, false, &blk, &dirty) == 0 && blk &&
                bread(f->fs, blk, block) == 0) {
                memset(block + size % BLOCK_SIZE, 0, BLOCK_SIZE - size % BLOCK_SIZE);
                bwrite(f->fs, blk, block);
            }
        }
    }
    in.size = size;
    in.mtime = time_now();
    return iput(f->fs, f->ino, &in);
}

static int s_stat(struct vnode *vn, struct vstat *st)
{
    struct sfs_node *f = node(vn);
    struct sfs_inode in;
    int err = iget(f->fs, f->ino, &in);
    if (err)
        return err;
    st->size = in.size;
    st->mtime = in.mtime;
    st->inode = f->ino;
    return 0;
}

static void s_release(struct vnode *vn)
{
    kfree(node(vn));
}

static const struct vnode_ops sfs_ops = {
    .lookup = s_lookup,
    .create = s_create,
    .unlink = s_unlink,
    .rename = s_rename,
    .readdir = s_readdir,
    .read = s_read,
    .write = s_write,
    .truncate = s_truncate,
    .stat = s_stat,
    .release = s_release,
};

static bool read_super(struct blockdev *dev, struct sfs_super *sb)
{
    uint8_t buf[BLOCK_SIZE];
    if (block_read(dev, 0, 1, buf) != 0)
        return false;
    memcpy(sb, buf, sizeof(*sb));
    return memcmp(sb->magic, SFS_MAGIC, 8) == 0 && sb->version == SFS_VERSION && sb->blocks <= dev->sectors;
}

bool sfs_detect(struct blockdev *dev)
{
    struct sfs_super sb;
    return read_super(dev, &sb);
}

int sfs_format(struct blockdev *dev)
{
    struct sfs_super sb = {0};
    memcpy(sb.magic, SFS_MAGIC, 8);
    sb.version = SFS_VERSION;
    sb.blocks = dev->sectors;
    sb.inodes = CLAMP(dev->sectors / 8, 64u, 65536u);
    sb.bitmap_start = 1;
    sb.bitmap_blocks = (sb.blocks + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    sb.inode_start = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_blocks = (sb.inodes + (uint32_t)SFS_INODES_PER_BLOCK - 1) / (uint32_t)SFS_INODES_PER_BLOCK;
    sb.data_start = sb.inode_start + sb.inode_blocks;
    sb.created = time_now();
    if (sb.data_start + 16 >= sb.blocks)
        return E_NOSPC;

    /* Bitmap and inode table go out 64 KB at a time: a fresh 2 GB disk is ~9000 blocks of them. */
    enum { CHUNK = 128 };
    uint8_t *chunk = kmalloc(CHUNK * BLOCK_SIZE);
    if (!chunk)
        return E_NOMEM;
    int err = 0;
    for (uint32_t b = 0; b < sb.bitmap_blocks + sb.inode_blocks && !err; b += CHUNK) {
        uint32_t count = MIN((uint32_t)CHUNK, sb.bitmap_blocks + sb.inode_blocks - b);
        memset(chunk, 0, (size_t)count * BLOCK_SIZE);
        for (uint32_t k = 0; k < count; k++) {
            uint8_t *blk = chunk + (size_t)k * BLOCK_SIZE;
            uint32_t at = b + k;
            if (at >= sb.bitmap_blocks) {
                if (at == sb.bitmap_blocks) {
                    struct sfs_inode root = {.type = SFS_DIR, .mtime = sb.created};
                    memcpy(blk, &root, sizeof(root));
                }
                continue;
            }
            for (uint32_t j = 0; j < BLOCK_SIZE; j++) {
                uint32_t n = (at * BLOCK_SIZE + j) * 8;
                if (n >= sb.data_start && n + 8 <= sb.blocks)
                    continue;
                for (uint32_t i = 0; i < 8; i++)
                    if (n + i < sb.data_start || n + i >= sb.blocks)
                        blk[j] |= (uint8_t)(1u << i);
            }
        }
        err = block_write(dev, sb.bitmap_start + b, count, chunk);
    }
    kfree(chunk);
    if (err)
        return err;
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(sb));
    err = block_write(dev, 0, 1, buf);
    if (err == 0)
        klog("sfs: formatted %s (%u blocks, %u inodes)", dev->name, (unsigned)sb.blocks, (unsigned)sb.inodes);
    return err;
}

static int sfs_mount(const char *source, struct vnode **root)
{
    struct blockdev *dev = source ? block_find(source) : NULL;
    if (!dev)
        return E_NOENT;
    struct sfs *fs = kzalloc(sizeof(*fs));
    if (!fs)
        return E_NOMEM;
    fs->dev = dev;
    if (!read_super(dev, &fs->sb)) {
        kfree(fs);
        return E_INVAL;
    }
    fs->bitmap = kmalloc(fs->sb.bitmap_blocks * BLOCK_SIZE);
    if (!fs->bitmap) {
        kfree(fs);
        return E_NOMEM;
    }
    for (uint32_t b = 0; b < fs->sb.bitmap_blocks; b += RA_BLOCKS) {
        uint32_t count = MIN((uint32_t)RA_BLOCKS, fs->sb.bitmap_blocks - b);
        if (block_read(dev, fs->sb.bitmap_start + b, count, fs->bitmap + b * BLOCK_SIZE) != 0) {
            kfree(fs->bitmap);
            kfree(fs);
            return E_IO;
        }
    }
    fs->ra = kmalloc(RA_BLOCKS * BLOCK_SIZE);
    for (uint32_t b = fs->sb.data_start; b < fs->sb.blocks; b++)
        if (!(fs->bitmap[b / 8] & (1u << (b % 8))))
            fs->free_blocks++;
    fs->hint = fs->sb.data_start;
    int err = node_new(fs, SFS_ROOT, NULL, root);
    if (err) {
        kfree(fs->bitmap);
        kfree(fs);
    }
    return err;
}

static int sfs_statfs(struct vnode *root, uint64_t *total, uint64_t *free)
{
    struct sfs *fs = node(root)->fs;
    *total = (uint64_t)(fs->sb.blocks - fs->sb.data_start) * BLOCK_SIZE;
    *free = (uint64_t)fs->free_blocks * BLOCK_SIZE;
    return 0;
}

static const struct fs_type sfs_type = {
    .name = "sfs",
    .description = "SynthFS persistent disk filesystem",
    .mount = sfs_mount,
    .statfs = sfs_statfs,
};
FS_TYPE(sfs_type);
