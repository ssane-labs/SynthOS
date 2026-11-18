/* Read-only exFAT (USB drives, SD cards) and NTFS (Windows disks). Files can be opened, listed and copied; nothing
   is ever written to these volumes, so a Windows disk is never put at risk. 64-bit kernel only (size). */
#ifdef __x86_64__
#include <synth/block.h>
#include <synth/codepage.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

#define SS 512u
#define LIST_LIMIT (64u << 20)

/* ---- shared: byte-addressed volume reads, names, directory listings ---- */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

static int vread(struct blockdev *dev, uint64_t off, void *buf, size_t n)
{
    uint8_t *o = buf, tmp[SS];
    while (n) {
        uint64_t lba = off / SS;
        uint32_t in = (uint32_t)(off % SS);
        if (lba >= dev->sectors)
            return E_IO;
        if (!in && n >= SS) {
            uint32_t cnt = (uint32_t)MIN(n / SS, (size_t)256);
            cnt = (uint32_t)MIN((uint64_t)cnt, dev->sectors - lba);
            int err = block_read(dev, (uint32_t)lba, cnt, o);
            if (err)
                return err;
            o += cnt * SS;
            off += cnt * SS;
            n -= cnt * SS;
            continue;
        }
        int err = block_read(dev, (uint32_t)lba, 1, tmp);
        if (err)
            return err;
        size_t k = MIN(n, (size_t)(SS - in));
        memcpy(o, tmp + in, k);
        o += k;
        off += k;
        n -= k;
    }
    return 0;
}

/* UTF-16LE to UTF-8 */
static void name16(const uint8_t *p, int chars, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < chars; i++) {
        uint32_t c = le16(p + i * 2);
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < chars) {
            uint32_t lo = le16(p + i * 2 + 2);
            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        char u[4];
        size_t k = utf8_encode(c ? c : '_', u);
        if (n + k >= cap)
            break;
        memcpy(out + n, u, k);
        n += k;
    }
    out[n] = 0;
}

struct item {
    uint32_t name; /* offset in the pool */
    uint8_t type, flags;
    uint32_t mtime;
    uint64_t key, size, valid;
};

struct listing {
    struct item *items;
    uint32_t count, cap;
    char *pool;
    uint32_t used, room;
};

static bool add(struct listing *l, const char *name, const struct item *it)
{
    size_t len = strlen(name) + 1;
    if (!name[0] || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
        return true;
    if (l->count == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 32;
        struct item *n = krealloc(l->items, cap * sizeof(*n));
        if (!n)
            return false;
        l->items = n;
        l->cap = cap;
    }
    if (l->used + len > l->room) {
        uint32_t room = MAX(l->room * 2, 4096u);
        while (room < l->used + len)
            room *= 2;
        char *p = krealloc(l->pool, room);
        if (!p)
            return false;
        l->pool = p;
        l->room = room;
    }
    memcpy(l->pool + l->used, name, len);
    l->items[l->count] = *it;
    l->items[l->count++].name = l->used;
    l->used += (uint32_t)len;
    return true;
}

static void listing_free(struct listing *l)
{
    if (l) {
        kfree(l->items);
        kfree(l->pool);
        kfree(l);
    }
}

static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

struct fvol;
struct fnode;

struct fs_impl {
    int (*list)(struct fnode *dir, struct listing *out);
    int (*open)(struct fnode *f);
    long (*read)(struct fnode *f, uint64_t off, uint8_t *buf, size_t n);
    void (*drop)(struct fnode *f);
};

struct fvol {
    struct blockdev *dev;
    const struct fs_impl *impl;
    uint32_t cluster;   /* bytes */
    uint64_t total, free;
    bool free_known;
    char label[64];
    /* exFAT */
    uint64_t fat_off, heap_off;
    uint32_t clusters, root, bitmap_first;
    uint64_t bitmap_len;
    uint64_t fat_lba;
    uint8_t fat_cache[SS];
    bool fat_valid;
    /* NTFS */
    uint32_t record, index, sector;
    struct run *mft;
    uint32_t mft_runs;
};

struct fnode {
    struct vnode vn;
    struct fvol *v;
    uint64_t key, size, valid;
    uint32_t mtime;
    uint8_t flags;
    bool opened;
    struct listing *list;
    uint64_t walk_i, walk_c; /* exFAT: the last cluster reached in a chain */
    void *data;              /* NTFS: resident bytes or runs */
    uint32_t runs;
    bool resident, unreadable;
};

static const struct vnode_ops fops;

static struct fnode *fnode(struct vnode *vn)
{
    return vn->priv;
}

static struct fnode *node_make(struct fvol *v, struct mount *m, const struct item *it)
{
    struct fnode *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    n->vn.ops = &fops;
    n->vn.type = it->type;
    n->vn.mount = m;
    n->vn.refs = 1;
    n->vn.priv = n;
    n->v = v;
    n->key = it->key;
    n->size = it->size;
    n->valid = it->valid;
    n->mtime = it->mtime;
    n->flags = it->flags;
    return n;
}

static int listed(struct fnode *d)
{
    if (d->list)
        return 0;
    struct listing *l = kzalloc(sizeof(*l));
    if (!l)
        return E_NOMEM;
    int err = d->v->impl->list(d, l);
    if (err) {
        listing_free(l);
        return err;
    }
    d->list = l;
    return 0;
}

static const struct item *find(struct fnode *d, const char *name, int *err)
{
    *err = listed(d);
    if (*err)
        return NULL;
    for (uint32_t i = 0; i < d->list->count; i++)
        if (same_name(d->list->pool + d->list->items[i].name, name))
            return &d->list->items[i];
    *err = E_NOENT;
    return NULL;
}

static int f_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct fnode *d = fnode(dir);
    int err;
    const struct item *it = find(d, name, &err);
    if (!it)
        return err;
    struct fnode *n = node_make(d->v, dir->mount, it);
    if (!n)
        return E_NOMEM;
    *out = &n->vn;
    return 0;
}

static int f_lookup_nocase(struct vnode *dir, const char *name, char *real, size_t cap)
{
    struct fnode *d = fnode(dir);
    int err;
    const struct item *it = find(d, name, &err);
    if (it)
        str_copy(real, d->list->pool + it->name, cap);
    return it ? 0 : err;
}

static int f_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct fnode *d = fnode(dir);
    int err = listed(d);
    if (err)
        return err;
    if (index >= d->list->count)
        return E_NOENT;
    const struct item *it = &d->list->items[index];
    str_copy(out->name, d->list->pool + it->name, sizeof(out->name));
    out->type = it->type;
    out->size = it->type == VN_DIR ? 0 : (uint32_t)MIN(it->size, (uint64_t)0xffffffffu);
    return 0;
}

static int opened(struct fnode *f)
{
    if (f->opened || !f->v->impl->open)
        return 0;
    int err = f->v->impl->open(f);
    if (!err)
        f->opened = true;
    return err;
}

static long f_read(struct vnode *vn, uint32_t off, void *buf, size_t n)
{
    struct fnode *f = fnode(vn);
    if (vn->type != VN_FILE)
        return E_ISDIR;
    int err = opened(f);
    if (err)
        return err;
    if (off >= f->size)
        return 0;
    n = (size_t)MIN((uint64_t)n, f->size - off);
    size_t real = off >= f->valid ? 0 : (size_t)MIN((uint64_t)n, f->valid - off);
    long got = real ? f->v->impl->read(f, off, buf, real) : 0;
    if (got < 0)
        return got;
    if ((size_t)got == real && real < n) {
        memset((uint8_t *)buf + real, 0, n - real);
        got = (long)n;
    }
    return got;
}

static int f_stat(struct vnode *vn, struct vstat *st)
{
    struct fnode *f = fnode(vn);
    if (vn->type == VN_FILE)
        opened(f);
    st->size = vn->type == VN_DIR ? 0 : (uint32_t)MIN(f->size, (uint64_t)0xffffffffu);
    st->mtime = f->mtime;
    st->inode = (uint32_t)f->key;
    return 0;
}

static void f_release(struct vnode *vn)
{
    struct fnode *f = fnode(vn);
    listing_free(f->list);
    if (f->v->impl->drop)
        f->v->impl->drop(f);
    kfree(f);
}

static const struct vnode_ops fops = {
    .lookup = f_lookup,
    .readdir = f_readdir,
    .lookup_nocase = f_lookup_nocase,
    .read = f_read,
    .stat = f_stat,
    .release = f_release,
};

static int mount_with(const char *source, struct vnode **root, struct fvol *(*open)(struct blockdev *dev),
                      const struct item *top, const char *what)
{
    struct blockdev *dev = source ? block_find(source) : NULL;
    if (!dev)
        return E_NOENT;
    struct fvol *v = open(dev);
    if (!v)
        return E_INVAL;
    struct item it = *top;
    if (!it.key)
        it.key = v->root;
    struct fnode *n = node_make(v, NULL, &it);
    if (!n) {
        kfree(v);
        return E_NOMEM;
    }
    *root = &n->vn;
    klog("%s: %s, clusters of %u bytes, read-only", what, dev->name, (unsigned)v->cluster);
    return 0;
}

static int label_of(struct vnode *root, char *out, size_t cap)
{
    str_copy(out, fnode(root)->v->label, cap);
    return 0;
}

static uint32_t bits_clear(const uint8_t *p, size_t n)
{
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t b = (uint8_t)~p[i]; b; b &= (uint8_t)(b - 1))
            c++;
    return c;
}

/* ---- exFAT ---- */

static uint64_t cluster_at(struct fvol *v, uint64_t c)
{
    return v->heap_off + (c - 2) * v->cluster;
}

static int fat_next(struct fvol *v, uint64_t c, uint64_t *next)
{
    uint64_t off = v->fat_off + c * 4, lba = off / SS;
    if (!v->fat_valid || v->fat_lba != lba) {
        int err = vread(v->dev, lba * SS, v->fat_cache, SS);
        if (err)
            return err;
        v->fat_lba = lba;
        v->fat_valid = true;
    }
    *next = le32(v->fat_cache + off % SS);
    return 0;
}

static bool good(struct fvol *v, uint64_t c)
{
    return c >= 2 && c < (uint64_t)v->clusters + 2;
}

/* the cluster holding byte `off` of a stream, and how many clusters run on contiguously from it (at most `want`) */
static int ex_map(struct fnode *f, uint64_t first, bool contiguous, uint64_t off, uint64_t want, uint64_t *c,
                  uint64_t *run)
{
    struct fvol *v = f->v;
    uint64_t i = off / v->cluster;
    if (contiguous) {
        *c = first + i;
        *run = want;
        return good(v, *c) ? 0 : E_IO;
    }
    uint64_t at = first, k = 0;
    if (f->walk_c && f->walk_i <= i) {
        at = f->walk_c;
        k = f->walk_i;
    }
    while (k < i) {
        if (!good(v, at))
            return E_IO;
        int err = fat_next(v, at, &at);
        if (err)
            return err;
        k++;
    }
    if (!good(v, at))
        return E_IO;
    f->walk_i = i;
    f->walk_c = at;
    *c = at;
    *run = 1;
    uint64_t cur = at, nx;
    while (*run < want && !fat_next(v, cur, &nx) && nx == cur + 1) {
        cur = nx;
        (*run)++;
    }
    return 0;
}

static long ex_read_stream(struct fnode *f, uint64_t first, bool contiguous, uint64_t off, uint8_t *buf, size_t n)
{
    struct fvol *v = f->v;
    size_t done = 0;
    while (done < n) {
        uint64_t pos = off + done, in = pos % v->cluster, c, run;
        uint64_t want = (in + (n - done) + v->cluster - 1) / v->cluster;
        int err = ex_map(f, first, contiguous, pos, want, &c, &run);
        if (err)
            return done ? (long)done : err;
        size_t k = (size_t)MIN((uint64_t)(n - done), run * v->cluster - in);
        err = vread(v->dev, cluster_at(v, c) + in, buf + done, k);
        if (err)
            return done ? (long)done : err;
        done += k;
    }
    return (long)done;
}

static long ex_read(struct fnode *f, uint64_t off, uint8_t *buf, size_t n)
{
    return ex_read_stream(f, f->key, f->flags & 2, off, buf, n);
}

static uint32_t ex_time(uint32_t t)
{
    struct rtc_time r = {
        .year = (uint16_t)(1980 + (t >> 25)),
        .month = (uint8_t)MAX((t >> 21) & 15, 1u),
        .day = (uint8_t)MAX((t >> 16) & 31, 1u),
        .hour = (uint8_t)((t >> 11) & 31),
        .minute = (uint8_t)((t >> 5) & 63),
        .second = (uint8_t)((t & 31) * 2),
    };
    return time_from_rtc(&r);
}

/* a directory's raw entries: the root follows its chain, other directories have a known length */
static int ex_dir_bytes(struct fnode *d, uint8_t **out, size_t *len)
{
    struct fvol *v = d->v;
    uint64_t size = d->size;
    if (!size) {
        uint64_t c = d->key;
        while (good(v, c) && size < LIST_LIMIT) {
            size += v->cluster;
            int err = fat_next(v, c, &c);
            if (err)
                return err;
        }
    }
    if (!size || size > LIST_LIMIT)
        return E_IO;
    uint8_t *b = kmalloc((size_t)size);
    if (!b)
        return E_NOMEM;
    long got = ex_read_stream(d, d->key, d->flags & 2, 0, b, (size_t)size);
    if (got < 0) {
        kfree(b);
        return (int)got;
    }
    *out = b;
    *len = (size_t)got;
    return 0;
}

static int ex_list(struct fnode *d, struct listing *l)
{
    uint8_t *b;
    size_t len;
    int err = ex_dir_bytes(d, &b, &len);
    if (err)
        return err;
    for (size_t i = 0; i + 32 <= len; i += 32) {
        const uint8_t *e = b + i;
        if (!e[0])
            break;
        if (e[0] != 0x85)
            continue;
        int count = e[1];
        if (count < 2 || i + 32 * (size_t)(count + 1) > len || b[i + 32] != 0xc0)
            continue;
        const uint8_t *s = e + 32;
        int chars = s[3];
        uint8_t wide[255 * 2];
        int got = 0;
        for (int k = 2; k <= count && got < chars; k++) {
            const uint8_t *nm = e + 32 * k;
            if (nm[0] != 0xc1)
                break;
            int take = MIN(15, chars - got);
            memcpy(wide + got * 2, nm + 2, (size_t)take * 2);
            got += take;
        }
        char name[NAME_MAX_LEN];
        name16(wide, got, name, sizeof(name));
        struct item it = {
            .type = (le16(e + 4) & 0x10) ? VN_DIR : VN_FILE,
            .flags = s[1],
            .mtime = ex_time(le32(e + 12)),
            .key = le32(s + 20),
            .size = le64(s + 24),
            .valid = le64(s + 8),
        };
        if (!add(l, name, &it)) {
            kfree(b);
            return E_NOMEM;
        }
        i += 32 * (size_t)count;
    }
    kfree(b);
    return 0;
}

static const struct fs_impl exfat_impl = {.list = ex_list, .read = ex_read};

static bool ex_boot(struct blockdev *dev, uint8_t *b)
{
    if (vread(dev, 0, b, SS) || memcmp(b + 3, "EXFAT   ", 8) || b[510] != 0x55 || b[511] != 0xaa)
        return false;
    return b[108] >= 9 && b[108] <= 12 && b[108] + b[109] <= 25 && le32(b + 92) >= 1;
}

bool exfat_detect(struct blockdev *dev)
{
    uint8_t b[SS];
    return ex_boot(dev, b);
}

static struct fvol *ex_open(struct blockdev *dev)
{
    uint8_t b[SS];
    if (!ex_boot(dev, b))
        return NULL;
    struct fvol *v = kzalloc(sizeof(*v));
    if (!v)
        return NULL;
    uint32_t ss = 1u << b[108];
    v->dev = dev;
    v->impl = &exfat_impl;
    v->cluster = ss << b[109];
    v->fat_off = (uint64_t)le32(b + 80) * ss;
    v->heap_off = (uint64_t)le32(b + 88) * ss;
    v->clusters = le32(b + 92);
    v->root = le32(b + 96);
    v->total = (uint64_t)v->clusters * v->cluster;
    /* the label and the allocation bitmap are entries of the root */
    struct fnode tmp = {.v = v, .key = v->root};
    uint8_t *raw;
    size_t len;
    if (!ex_dir_bytes(&tmp, &raw, &len)) {
        for (size_t i = 0; i + 32 <= len && raw[i]; i += 32) {
            if (raw[i] == 0x83)
                name16(raw + i + 2, MIN(raw[i + 1], 11), v->label, sizeof(v->label));
            else if (raw[i] == 0x81 && !(raw[i + 1] & 1)) {
                v->bitmap_first = le32(raw + i + 20);
                v->bitmap_len = le64(raw + i + 24);
            }
        }
        kfree(raw);
    }
    return v;
}

static int exfat_mount(const char *source, struct vnode **root)
{
    static const struct item top = {.type = VN_DIR};
    return mount_with(source, root, ex_open, &top, "exfat");
}

static int exfat_statfs(struct vnode *root, uint64_t *total, uint64_t *free)
{
    struct fvol *v = fnode(root)->v;
    if (!v->free_known && v->bitmap_first && v->bitmap_len <= LIST_LIMIT) {
        uint8_t *b = kmalloc((size_t)v->bitmap_len);
        struct fnode tmp = {.v = v};
        if (b && ex_read_stream(&tmp, v->bitmap_first, false, 0, b, (size_t)v->bitmap_len) ==
                     (long)v->bitmap_len) {
            size_t bytes = MIN((size_t)v->bitmap_len, ((size_t)v->clusters + 7) / 8);
            uint32_t clear = bits_clear(b, bytes);
            uint32_t pad = (uint32_t)(bytes * 8 - v->clusters);
            v->free = (uint64_t)(clear - MIN(clear, pad)) * v->cluster;
            v->free_known = true;
        }
        kfree(b);
    }
    *total = v->total;
    *free = v->free;
    return 0;
}

static const struct fs_type exfat_type = {
    .name = "exfat",
    .description = "exFAT, read-only (USB drives, SD cards)",
    .mount = exfat_mount,
    .statfs = exfat_statfs,
    .label = label_of,
};
FS_TYPE(exfat_type);

/* ---- NTFS ---- */

struct run {
    uint64_t vcn, lcn, len; /* lcn ~0: sparse */
};

#define SPARSE (~(uint64_t)0)

static int fixup(struct fvol *v, uint8_t *rec, uint32_t size)
{
    uint16_t at = le16(rec + 4), count = le16(rec + 6);
    if (!count || at + count * 2u > size || (count - 1u) * v->sector > size)
        return E_IO;
    for (uint32_t i = 1; i < count; i++) {
        uint8_t *end = rec + i * v->sector - 2;
        if (end[0] != rec[at] || end[1] != rec[at + 1])
            return E_IO;
        end[0] = rec[at + i * 2];
        end[1] = rec[at + i * 2 + 1];
    }
    return 0;
}

static bool runs_add(struct run **runs, uint32_t *count, struct run r)
{
    if (!(*count & 15)) {
        struct run *n = krealloc(*runs, (*count + 16) * sizeof(*n));
        if (!n)
            return false;
        *runs = n;
    }
    (*runs)[(*count)++] = r;
    return true;
}

/* the data runs of a non-resident attribute */
static int decode_runs(const uint8_t *attr, uint32_t alen, struct run **runs, uint32_t *count)
{
    uint64_t vcn = le64(attr + 16);
    int64_t lcn = 0;
    uint32_t p = le16(attr + 32);
    while (p < alen && attr[p]) {
        uint8_t h = attr[p], ls = h & 15, os = h >> 4;
        if (!ls || ls > 8 || os > 8 || p + 1 + ls + os > alen)
            return E_IO;
        uint64_t len = 0;
        for (int i = ls - 1; i >= 0; i--)
            len = len << 8 | attr[p + 1 + i];
        struct run r = {.vcn = vcn, .len = len, .lcn = SPARSE};
        if (os) {
            int64_t d = (int8_t)attr[p + ls + os];
            for (int i = os - 2; i >= 0; i--)
                d = d * 256 + attr[p + 1 + ls + i];
            lcn += d;
            r.lcn = (uint64_t)lcn;
        }
        if (!runs_add(runs, count, r))
            return E_NOMEM;
        vcn += len;
        p += 1u + ls + os;
    }
    return 0;
}

static long runs_read(struct fvol *v, const struct run *runs, uint32_t count, uint64_t off, uint8_t *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        uint64_t pos = off + done, vcn = pos / v->cluster;
        const struct run *r = NULL;
        for (uint32_t i = 0; i < count; i++)
            if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len) {
                r = &runs[i];
                break;
            }
        if (!r)
            return done ? (long)done : E_IO;
        uint64_t end = (r->vcn + r->len) * v->cluster;
        size_t k = (size_t)MIN((uint64_t)(n - done), end - pos);
        if (r->lcn == SPARSE) {
            memset(buf + done, 0, k);
        } else {
            int err = vread(v->dev, (r->lcn + vcn - r->vcn) * v->cluster + pos % v->cluster, buf + done, k);
            if (err)
                return done ? (long)done : err;
        }
        done += k;
    }
    return (long)done;
}

static int read_record(struct fvol *v, uint64_t n, uint8_t *rec)
{
    if (runs_read(v, v->mft, v->mft_runs, n * v->record, rec, v->record) != (long)v->record)
        return E_IO;
    if (memcmp(rec, "FILE", 4) || !(le16(rec + 22) & 1))
        return E_NOENT;
    return fixup(v, rec, v->record);
}

/* the first attribute of a type in a record (with no name, or the directory index "$I30") */
static const uint8_t *attr_find(struct fvol *v, const uint8_t *rec, uint32_t type, bool named, uint32_t *len)
{
    uint32_t p = le16(rec + 20);
    while (p + 16 <= v->record) {
        uint32_t t = le32(rec + p), l = le32(rec + p + 4);
        if (t == 0xffffffffu || l < 16 || p + l > v->record)
            break;
        if (t == type && (named || !rec[p + 9])) {
            *len = l;
            return rec + p;
        }
        p += l;
    }
    return NULL;
}

/* the runs of an attribute, gathered from every record its attribute list names when it is split */
static int attr_runs(struct fvol *v, uint64_t ref, const uint8_t *rec, uint32_t type, bool named, struct run **runs,
                     uint32_t *count, uint64_t *size, uint64_t *init, const uint8_t **resident, uint32_t *rlen,
                     uint16_t *flags)
{
    uint32_t len;
    const uint8_t *a = attr_find(v, rec, type, named, &len);
    if (a) {
        *flags = le16(a + 12);
        if (!a[8]) {
            *resident = a + le16(a + 20);
            *rlen = le32(a + 16);
            *size = *init = *rlen;
            return 0;
        }
        *size = le64(a + 48);
        *init = le64(a + 56);
        int err = decode_runs(a, len, runs, count);
        if (err || le64(a + 24) + 1 >= (*size + v->cluster - 1) / v->cluster)
            return err;
    }
    const uint8_t *list = attr_find(v, rec, 0x20, true, &len);
    if (!list || list[8])
        return a ? 0 : E_NOENT; /* a non-resident attribute list is not read */
    const uint8_t *e = list + le16(list + 20), *end = e + le32(list + 16);
    uint8_t *other = kmalloc(v->record);
    if (!other)
        return E_NOMEM;
    bool found = a != NULL;
    for (; e + 26 <= end && le16(e + 4) >= 26; e += le16(e + 4)) {
        uint64_t at = le64(e + 16) & 0xffffffffffffull;
        if (le32(e) != type || (!named && e[6]) || at == ref)
            continue;
        if (read_record(v, at, other))
            continue;
        uint32_t olen;
        const uint8_t *oa = attr_find(v, other, type, named, &olen);
        if (!oa || !oa[8])
            continue;
        if (!found || !le64(oa + 16)) {
            *size = le64(oa + 48) ? le64(oa + 48) : *size;
            *init = le64(oa + 56) ? le64(oa + 56) : *init;
            *flags = le16(oa + 12);
        }
        found = true;
        decode_runs(oa, olen, runs, count);
    }
    kfree(other);
    return found ? 0 : E_NOENT;
}

static uint32_t nt_time(uint64_t ft)
{
    uint64_t s = ft / 10000000u;
    return s > 11644473600ull ? (uint32_t)(s - 11644473600ull) : 0;
}

static int nt_index_entries(struct listing *l, const uint8_t *p, const uint8_t *end)
{
    while (p + 16 <= end) {
        uint16_t len = le16(p + 8), klen = le16(p + 10), fl = le16(p + 12);
        if (fl & 2 || len < 16)
            break;
        const uint8_t *k = p + 16;
        if (klen >= 66 && k + 66 + k[64] * 2 <= end) {
            uint64_t ref = le64(p) & 0xffffffffffffull;
            if (k[65] != 2 && ref >= 24) {
                char name[NAME_MAX_LEN];
                name16(k + 66, k[64], name, sizeof(name));
                struct item it = {
                    .type = (le32(k + 56) & 0x10000000u) ? VN_DIR : VN_FILE,
                    .mtime = nt_time(le64(k + 16)),
                    .key = ref,
                    .size = le64(k + 48),
                };
                it.valid = it.size;
                if (!add(l, name, &it))
                    return E_NOMEM;
            }
        }
        p += len;
    }
    return 0;
}

static int nt_list(struct fnode *d, struct listing *l)
{
    struct fvol *v = d->v;
    uint8_t *rec = kmalloc(v->record);
    if (!rec)
        return E_NOMEM;
    int err = read_record(v, d->key, rec);
    uint32_t len;
    const uint8_t *root = err ? NULL : attr_find(v, rec, 0x90, true, &len);
    if (!root || root[8]) {
        kfree(rec);
        return err ? err : 0; /* a reparse point (junction) lists as empty */
    }
    const uint8_t *val = root + le16(root + 20), *hdr = val + 16;
    err = nt_index_entries(l, hdr + le32(hdr), hdr + le32(hdr + 4));
    struct run *runs = NULL;
    uint32_t count = 0;
    uint64_t size = 0, init = 0;
    const uint8_t *res = NULL, *bres = NULL;
    uint32_t rlen = 0, blen = 0;
    uint16_t flags;
    if (!err && (le32(hdr + 12) & 1) &&
        !attr_runs(v, d->key, rec, 0xa0, true, &runs, &count, &size, &init, &res, &rlen, &flags) && size &&
        size <= LIST_LIMIT) {
        uint8_t *bits = NULL;
        uint64_t bsize = 0, binit;
        struct run *bruns = NULL;
        uint32_t bcount = 0;
        if (!attr_runs(v, d->key, rec, 0xb0, true, &bruns, &bcount, &bsize, &binit, &bres, &blen, &flags)) {
            if (bres) {
                bits = kmalloc(blen ? blen : 1);
                if (bits)
                    memcpy(bits, bres, blen);
            } else if (bsize <= (1u << 20) && (bits = kmalloc((size_t)bsize + 1))) {
                runs_read(v, bruns, bcount, 0, bits, (size_t)bsize);
            }
        }
        kfree(bruns);
        uint8_t *block = kmalloc(v->index);
        for (uint64_t at = 0, i = 0; block && !err && at + v->index <= size; at += v->index, i++) {
            if (bits && i / 8 >= (bres ? blen : bsize))
                break;
            if (bits && !(bits[i / 8] >> (i % 8) & 1))
                continue;
            if (runs_read(v, runs, count, at, block, v->index) != (long)v->index || memcmp(block, "INDX", 4) ||
                fixup(v, block, v->index))
                continue;
            const uint8_t *h = block + 24;
            if (le32(h + 4) > v->index - 24)
                continue;
            err = nt_index_entries(l, h + le32(h), h + le32(h + 4));
        }
        kfree(block);
        kfree(bits);
    }
    kfree(runs);
    kfree(rec);
    return err;
}

static int nt_open(struct fnode *f)
{
    struct fvol *v = f->v;
    uint8_t *rec = kmalloc(v->record);
    if (!rec)
        return E_NOMEM;
    int err = read_record(v, f->key, rec);
    struct run *runs = NULL;
    uint32_t count = 0, rlen = 0;
    uint64_t size = 0, init = 0;
    const uint8_t *res = NULL;
    uint16_t flags = 0;
    if (!err)
        err = attr_runs(v, f->key, rec, 0x80, false, &runs, &count, &size, &init, &res, &rlen, &flags);
    if (!err) {
        f->size = size;
        f->valid = MIN(init, size);
        f->unreadable = (flags & 0x4001) != 0; /* compressed or encrypted */
        if (res) {
            f->data = kmalloc(rlen ? rlen : 1);
            if (!f->data)
                err = E_NOMEM;
            else
                memcpy(f->data, res, rlen);
            f->resident = true;
            kfree(runs);
        } else {
            f->data = runs;
            f->runs = count;
        }
    } else {
        kfree(runs);
    }
    if (err == E_NOENT) { /* no data stream: an empty file */
        f->size = f->valid = 0;
        err = 0;
    }
    kfree(rec);
    return err;
}

static long nt_read(struct fnode *f, uint64_t off, uint8_t *buf, size_t n)
{
    if (f->unreadable)
        return E_IO;
    if (f->resident) {
        memcpy(buf, (uint8_t *)f->data + off, n);
        return (long)n;
    }
    return runs_read(f->v, f->data, f->runs, off, buf, n);
}

static void nt_drop(struct fnode *f)
{
    kfree(f->data);
}

static const struct fs_impl ntfs_impl = {.list = nt_list, .open = nt_open, .read = nt_read, .drop = nt_drop};

static bool nt_boot(struct blockdev *dev, uint8_t *b)
{
    if (vread(dev, 0, b, SS) || memcmp(b + 3, "NTFS    ", 8) || b[510] != 0x55 || b[511] != 0xaa)
        return false;
    uint16_t bps = le16(b + 11);
    return bps >= 512 && bps <= 4096 && !(bps & (bps - 1)) && b[13];
}

bool ntfs_detect(struct blockdev *dev)
{
    uint8_t b[SS];
    return nt_boot(dev, b);
}

static uint32_t nt_size(int8_t x, uint32_t cluster)
{
    return x > 0 ? (uint32_t)x * cluster : 1u << -x;
}

static struct fvol *nt_open_volume(struct blockdev *dev)
{
    uint8_t b[SS];
    if (!nt_boot(dev, b))
        return NULL;
    struct fvol *v = kzalloc(sizeof(*v));
    if (!v)
        return NULL;
    uint32_t spc = b[13] > 0x80 ? 1u << (256 - b[13]) : b[13];
    v->dev = dev;
    v->impl = &ntfs_impl;
    v->sector = le16(b + 11);
    v->cluster = v->sector * spc;
    v->record = nt_size((int8_t)b[64], v->cluster);
    v->index = nt_size((int8_t)b[68], v->cluster);
    v->total = le64(b + 40) * v->sector;
    v->root = 5;
    uint8_t *rec = v->record >= 1024 && v->record <= 4096 && v->index >= 1024 && v->index <= 65536
                       ? kmalloc(v->record)
                       : NULL;
    /* record 0 is the MFT itself: its runs say where every other record is */
    struct run first = {.vcn = 0, .lcn = le64(b + 48), .len = (v->record + v->cluster - 1) / v->cluster};
    v->mft = &first;
    v->mft_runs = 1;
    bool ok = rec && read_record(v, 0, rec) == 0;
    v->mft = NULL;
    v->mft_runs = 0;
    uint32_t len;
    const uint8_t *data = ok ? attr_find(v, rec, 0x80, false, &len) : NULL;
    ok = data && data[8] && decode_runs(data, len, &v->mft, &v->mft_runs) == 0 && v->mft_runs;
    if (ok && read_record(v, 3, rec) == 0) {
        const uint8_t *nm = attr_find(v, rec, 0x60, true, &len);
        if (nm && !nm[8])
            name16(nm + le16(nm + 20), (int)(le32(nm + 16) / 2), v->label, sizeof(v->label));
    }
    kfree(rec);
    if (!ok) {
        kfree(v->mft);
        kfree(v);
        return NULL;
    }
    return v;
}

static int ntfs_mount(const char *source, struct vnode **root)
{
    static const struct item top = {.type = VN_DIR, .key = 5};
    return mount_with(source, root, nt_open_volume, &top, "ntfs");
}

static int ntfs_statfs(struct vnode *root, uint64_t *total, uint64_t *free)
{
    struct fvol *v = fnode(root)->v;
    if (!v->free_known) {
        struct fnode f = {.v = v, .key = 6};
        if (!nt_open(&f) && !f.resident && f.size <= LIST_LIMIT) {
            uint8_t *bits = kmalloc((size_t)f.size + 1);
            uint64_t clusters = v->total / v->cluster;
            if (bits && runs_read(v, f.data, f.runs, 0, bits, (size_t)f.size) == (long)f.size) {
                size_t bytes = (size_t)MIN(f.size, (clusters + 7) / 8);
                uint32_t clear = bits_clear(bits, bytes);
                uint32_t pad = (uint32_t)(bytes * 8 - MIN((uint64_t)bytes * 8, clusters));
                v->free = (uint64_t)(clear - MIN(clear, pad)) * v->cluster;
            }
            kfree(bits);
        }
        nt_drop(&f);
        v->free_known = true;
    }
    *total = v->total;
    *free = v->free;
    return 0;
}

static const struct fs_type ntfs_type = {
    .name = "ntfs",
    .description = "NTFS, read-only (Windows disks)",
    .mount = ntfs_mount,
    .statfs = ntfs_statfs,
    .label = label_of,
};
FS_TYPE(ntfs_type);
#endif
