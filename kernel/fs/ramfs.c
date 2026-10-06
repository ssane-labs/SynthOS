#include <synth/error.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

struct ram_node {
    struct vnode vn;
    char name[NAME_MAX_LEN];
    struct ram_node *parent;
    struct ram_node *children;
    struct ram_node *next;
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    uint32_t mtime;
    uint32_t ino;
    bool unlinked;
};

static const struct vnode_ops ram_ops;
static uint32_t next_ino = 1;
static size_t ram_bytes;

static struct ram_node *node_of(struct vnode *vn)
{
    return (struct ram_node *)vn;
}

static struct ram_node *node_new(const char *name, int type)
{
    struct ram_node *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    n->vn.ops = &ram_ops;
    n->vn.type = (uint8_t)type;
    n->vn.priv = n;
    n->ino = next_ino++;
    n->mtime = time_now();
    str_copy(n->name, name, sizeof(n->name));
    return n;
}

static void node_free(struct ram_node *n)
{
    ram_bytes -= n->cap;
    kfree(n->data);
    kfree(n);
}

static struct ram_node *child(struct ram_node *dir, const char *name)
{
    for (struct ram_node *c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

static void attach(struct ram_node *dir, struct ram_node *n)
{
    struct ram_node **link = &dir->children;
    while (*link && strcmp((*link)->name, n->name) < 0)
        link = &(*link)->next;
    n->next = *link;
    *link = n;
    n->parent = dir;
    dir->mtime = time_now();
}

static void detach(struct ram_node *n)
{
    struct ram_node **link = &n->parent->children;
    while (*link != n)
        link = &(*link)->next;
    *link = n->next;
    n->parent->mtime = time_now();
    n->parent = NULL;
    n->next = NULL;
}

static int ram_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct ram_node *c = child(node_of(dir), name);
    if (!c)
        return E_NOENT;
    *out = vnode_get(&c->vn);
    return 0;
}

static int ram_create(struct vnode *dir, const char *name, int type, struct vnode **out)
{
    if (child(node_of(dir), name))
        return E_EXIST;
    struct ram_node *n = node_new(name, type);
    if (!n)
        return E_NOMEM;
    n->vn.mount = dir->mount;
    attach(node_of(dir), n);
    *out = vnode_get(&n->vn);
    return 0;
}

static int ram_unlink(struct vnode *dir, const char *name)
{
    struct ram_node *c = child(node_of(dir), name);
    if (!c)
        return E_NOENT;
    if (c->children)
        return E_NOTEMPTY;
    detach(c);
    c->unlinked = true;
    if (c->vn.refs <= 0)
        node_free(c);
    return 0;
}

static int ram_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname)
{
    struct ram_node *src = child(node_of(odir), oname);
    if (!src)
        return E_NOENT;
    struct ram_node *dst = child(node_of(ndir), nname);
    if (dst == src)
        return 0;
    for (struct ram_node *p = node_of(ndir); p; p = p->parent)
        if (p == src)
            return E_INVAL;
    if (dst) {
        if (dst->vn.type == VN_DIR)
            return dst->children ? E_NOTEMPTY : E_ISDIR;
        int err = ram_unlink(ndir, nname);
        if (err)
            return err;
    }
    detach(src);
    str_copy(src->name, nname, sizeof(src->name));
    attach(node_of(ndir), src);
    return 0;
}

static int ram_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct ram_node *c = node_of(dir)->children;
    while (c && index--)
        c = c->next;
    if (!c)
        return E_NOENT;
    str_copy(out->name, c->name, sizeof(out->name));
    out->type = c->vn.type;
    out->size = c->size;
    return 0;
}

static long ram_read(struct vnode *vn, uint32_t off, void *buf, size_t n)
{
    struct ram_node *f = node_of(vn);
    if (off >= f->size)
        return 0;
    size_t len = MIN(n, (size_t)(f->size - off));
    memcpy(buf, f->data + off, len);
    return (long)len;
}

static int reserve(struct ram_node *f, uint32_t size)
{
    if (size <= f->cap)
        return 0;
    uint32_t cap = f->cap ? f->cap : 64;
    while (cap < size)
        cap *= 2;
    uint8_t *data = krealloc(f->data, cap);
    if (!data)
        return E_NOSPC;
    ram_bytes += cap - f->cap;
    f->data = data;
    f->cap = cap;
    return 0;
}

static long ram_write(struct vnode *vn, uint32_t off, const void *buf, size_t n)
{
    struct ram_node *f = node_of(vn);
    uint32_t end = off + (uint32_t)n;
    int err = reserve(f, end);
    if (err)
        return err;
    if (off > f->size)
        memset(f->data + f->size, 0, off - f->size);
    memcpy(f->data + off, buf, n);
    if (end > f->size)
        f->size = end;
    f->mtime = time_now();
    return (long)n;
}

static int ram_truncate(struct vnode *vn, uint32_t size)
{
    struct ram_node *f = node_of(vn);
    int err = reserve(f, size);
    if (err)
        return err;
    if (size > f->size)
        memset(f->data + f->size, 0, size - f->size);
    f->size = size;
    f->mtime = time_now();
    return 0;
}

static int ram_stat(struct vnode *vn, struct vstat *st)
{
    struct ram_node *n = node_of(vn);
    st->size = n->size;
    st->mtime = n->mtime;
    st->inode = n->ino;
    return 0;
}

static void ram_release(struct vnode *vn)
{
    struct ram_node *n = node_of(vn);
    if (n->unlinked)
        node_free(n);
}

static const struct vnode_ops ram_ops = {
    .lookup = ram_lookup,
    .create = ram_create,
    .unlink = ram_unlink,
    .rename = ram_rename,
    .readdir = ram_readdir,
    .read = ram_read,
    .write = ram_write,
    .truncate = ram_truncate,
    .stat = ram_stat,
    .release = ram_release,
};

static int ram_mount(const char *source, struct vnode **root)
{
    UNUSED(source);
    struct ram_node *n = node_new("/", VN_DIR);
    if (!n)
        return E_NOMEM;
    n->vn.refs = 1;
    *root = &n->vn;
    return 0;
}

static int ram_statfs(struct vnode *root, uint64_t *total, uint64_t *free)
{
    UNUSED(root);
    struct heap_stats hs;
    heap_get_stats(&hs);
    *total = ram_bytes + hs.free;
    *free = hs.free;
    return 0;
}

static const struct fs_type ramfs = {
    .name = "ramfs",
    .description = "fast in-memory filesystem",
    .mount = ram_mount,
    .statfs = ram_statfs,
};
FS_TYPE(ramfs);
