#include <synth/error.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vfs.h>

SYNTH_TABLE_DECLARE(synth_fs, struct fs_type);

static struct mount *mounts;

void vfs_init(void)
{
    mounts = NULL;
}

const struct fs_type *vfs_find_fs(const char *name)
{
    SYNTH_TABLE_FOREACH(synth_fs, struct fs_type, fs)
        if (strcmp(fs->name, name) == 0)
            return fs;
    return NULL;
}

void vfs_foreach_fs(void (*fn)(const struct fs_type *fs, void *ctx), void *ctx)
{
    SYNTH_TABLE_FOREACH(synth_fs, struct fs_type, fs)
        fn(fs, ctx);
}

struct mount *vfs_mounts(void)
{
    return mounts;
}

struct vnode *vnode_get(struct vnode *vn)
{
    if (vn)
        vn->refs++;
    return vn;
}

void vnode_put(struct vnode *vn)
{
    if (vn && --vn->refs <= 0 && vn->ops->release)
        vn->ops->release(vn);
}

static bool mount_covers(const struct mount *m, const char *path)
{
    size_t n = strlen(m->path);
    if (n == 1)
        return true;
    return strncmp(path, m->path, n) == 0 && (path[n] == 0 || path[n] == '/');
}

static struct mount *mount_for(const char *path, const char **rest)
{
    for (struct mount *m = mounts; m; m = m->next) {
        if (mount_covers(m, path)) {
            size_t n = strlen(m->path);
            *rest = path + (n == 1 ? 0 : n);
            return m;
        }
    }
    return NULL;
}

int vfs_mount(const char *fsname, const char *source, const char *path)
{
    const struct fs_type *fs = vfs_find_fs(fsname);
    if (!fs)
        return E_NOSYS;
    if (path[0] != '/')
        return E_INVAL;
    if (strcmp(path, "/") != 0 && mounts && !vfs_is_dir(path))
        return E_NOTDIR;
    for (struct mount *m = mounts; m; m = m->next)
        if (strcmp(m->path, path) == 0 && strcmp(path, "/") != 0)
            return E_BUSY;

    struct mount *m = kzalloc(sizeof(*m));
    if (!m)
        return E_NOMEM;
    int err = fs->mount(source, &m->root);
    if (err) {
        kfree(m);
        return err;
    }
    m->fs = fs;
    m->root->mount = m;
    str_copy(m->path, path, sizeof(m->path));
    str_copy(m->source, source ? source : fsname, sizeof(m->source));

    struct mount **link = &mounts;
    while (*link && strlen((*link)->path) >= strlen(path))
        link = &(*link)->next;
    m->next = *link;
    *link = m;
    klog("vfs: mounted %s (%s) at %s", m->source, fsname, path);
    return 0;
}

int vfs_umount(const char *path)
{
    for (struct mount **link = &mounts; *link; link = &(*link)->next) {
        struct mount *m = *link;
        if (strcmp(m->path, path) != 0)
            continue;
        if (strcmp(path, "/") == 0)
            return E_BUSY;
        *link = m->next;
        vnode_put(m->root);
        kfree(m);
        return 0;
    }
    return E_INVAL;
}

static int walk(struct vnode *start, const char *rest, struct vnode **out)
{
    struct vnode *vn = vnode_get(start);
    char name[NAME_MAX_LEN];
    while (*rest) {
        while (*rest == '/')
            rest++;
        if (!*rest)
            break;
        size_t n = 0;
        while (rest[n] && rest[n] != '/')
            n++;
        if (n >= sizeof(name)) {
            vnode_put(vn);
            return E_NAMETOOLONG;
        }
        memcpy(name, rest, n);
        name[n] = 0;
        rest += n;
        if (vn->type != VN_DIR) {
            vnode_put(vn);
            return E_NOTDIR;
        }
        struct vnode *next = NULL;
        int err = vn->ops->lookup(vn, name, &next);
        vnode_put(vn);
        if (err)
            return err;
        vn = next;
    }
    *out = vn;
    return 0;
}

int vfs_lookup(const char *path, struct vnode **out)
{
    const char *rest;
    struct mount *m = mount_for(path, &rest);
    if (!m)
        return E_NOENT;
    return walk(m->root, rest, out);
}

int vfs_lookup_parent(const char *path, struct vnode **dir, char *name)
{
    const char *base = path_basename(path);
    if (!*base || strcmp(path, "/") == 0)
        return E_INVAL;
    if (strlen(base) >= NAME_MAX_LEN)
        return E_NAMETOOLONG;
    char parent[PATH_MAX_LEN];
    path_dirname(path, parent, sizeof(parent));
    struct vnode *vn;
    int err = vfs_lookup(parent, &vn);
    if (err)
        return err;
    if (vn->type != VN_DIR) {
        vnode_put(vn);
        return E_NOTDIR;
    }
    str_copy(name, base, NAME_MAX_LEN);
    *dir = vn;
    return 0;
}

static bool is_mount_point(const char *path)
{
    for (struct mount *m = mounts; m; m = m->next)
        if (strcmp(m->path, path) == 0)
            return true;
    return false;
}

int vfs_open(const char *path, unsigned flags, struct file **out)
{
    struct vnode *vn = NULL;
    int err = vfs_lookup(path, &vn);
    if (err == 0 && (flags & VFS_CREATE) && (flags & VFS_EXCL)) {
        vnode_put(vn);
        return E_EXIST;
    }
    if (err == E_NOENT && (flags & VFS_CREATE)) {
        struct vnode *dir;
        char name[NAME_MAX_LEN];
        err = vfs_lookup_parent(path, &dir, name);
        if (err)
            return err;
        err = dir->ops->create ? dir->ops->create(dir, name, VN_FILE, &vn) : E_PERM;
        vnode_put(dir);
    }
    if (err)
        return err;
    if (vn->type == VN_DIR && (flags & VFS_WRITE)) {
        vnode_put(vn);
        return E_ISDIR;
    }
    if ((flags & VFS_TRUNC) && (flags & VFS_WRITE) && vn->type == VN_FILE) {
        err = vn->ops->truncate ? vn->ops->truncate(vn, 0) : E_PERM;
        if (err) {
            vnode_put(vn);
            return err;
        }
    }
    struct file *f = kzalloc(sizeof(*f));
    if (!f) {
        vnode_put(vn);
        return E_NOMEM;
    }
    f->vn = vn;
    f->flags = flags;
    *out = f;
    return 0;
}

long vfs_read(struct file *f, void *buf, size_t n)
{
    if (!(f->flags & VFS_READ))
        return E_BADF;
    if (f->vn->type == VN_DIR)
        return E_ISDIR;
    if (!f->vn->ops->read)
        return E_PERM;
    long got = f->vn->ops->read(f->vn, f->pos, buf, n);
    if (got > 0)
        f->pos += (uint32_t)got;
    return got;
}

long vfs_write(struct file *f, const void *buf, size_t n)
{
    if (!(f->flags & VFS_WRITE))
        return E_BADF;
    if (!f->vn->ops->write)
        return E_PERM;
    if (f->flags & VFS_APPEND) {
        struct vstat st;
        if (vfs_fstat(f, &st) == 0)
            f->pos = st.size;
    }
    long put = f->vn->ops->write(f->vn, f->pos, buf, n);
    if (put > 0)
        f->pos += (uint32_t)put;
    return put;
}

long vfs_seek(struct file *f, long off, int whence)
{
    long base = 0;
    if (whence == VFS_SEEK_CUR) {
        base = f->pos;
    } else if (whence == VFS_SEEK_END) {
        struct vstat st;
        int err = vfs_fstat(f, &st);
        if (err)
            return err;
        base = st.size;
    }
    if (base + off < 0)
        return E_INVAL;
    f->pos = (uint32_t)(base + off);
    return f->pos;
}

int vfs_fstat(struct file *f, struct vstat *st)
{
    memset(st, 0, sizeof(*st));
    st->type = f->vn->type;
    return f->vn->ops->stat ? f->vn->ops->stat(f->vn, st) : 0;
}

void vfs_close(struct file *f)
{
    if (!f)
        return;
    vnode_put(f->vn);
    kfree(f);
}

int vfs_stat(const char *path, struct vstat *st)
{
    struct vnode *vn;
    int err = vfs_lookup(path, &vn);
    if (err)
        return err;
    memset(st, 0, sizeof(*st));
    st->type = vn->type;
    if (vn->ops->stat)
        err = vn->ops->stat(vn, st);
    vnode_put(vn);
    return err;
}

bool vfs_exists(const char *path)
{
    struct vstat st;
    return vfs_stat(path, &st) == 0;
}

bool vfs_is_dir(const char *path)
{
    struct vstat st;
    return vfs_stat(path, &st) == 0 && st.type == VN_DIR;
}

int vfs_mkdir(const char *path)
{
    struct vnode *dir, *vn;
    char name[NAME_MAX_LEN];
    if (vfs_exists(path))
        return E_EXIST;
    int err = vfs_lookup_parent(path, &dir, name);
    if (err)
        return err;
    err = dir->ops->create ? dir->ops->create(dir, name, VN_DIR, &vn) : E_PERM;
    vnode_put(dir);
    if (err == 0)
        vnode_put(vn);
    return err;
}

int vfs_mkdirs(const char *path)
{
    char partial[PATH_MAX_LEN];
    size_t len = 0;
    for (const char *p = path;; p++) {
        if ((*p == '/' || !*p) && len > 0) {
            partial[len] = 0;
            if (!vfs_is_dir(partial)) {
                int err = vfs_mkdir(partial);
                if (err && err != E_EXIST)
                    return err;
            }
        }
        if (!*p)
            break;
        if (len + 1 >= sizeof(partial))
            return E_NAMETOOLONG;
        partial[len++] = *p;
    }
    return 0;
}

int vfs_unlink(const char *path)
{
    if (is_mount_point(path))
        return E_BUSY;
    struct vnode *dir;
    char name[NAME_MAX_LEN];
    int err = vfs_lookup_parent(path, &dir, name);
    if (err)
        return err;
    err = dir->ops->unlink ? dir->ops->unlink(dir, name) : E_PERM;
    vnode_put(dir);
    return err;
}

int vfs_rename(const char *from, const char *to)
{
    if (is_mount_point(from) || is_mount_point(to))
        return E_BUSY;
    if (str_starts(to, from) && (to[strlen(from)] == '/'))
        return E_INVAL;
    struct vnode *odir, *ndir;
    char oname[NAME_MAX_LEN], nname[NAME_MAX_LEN];
    int err = vfs_lookup_parent(from, &odir, oname);
    if (err)
        return err;
    err = vfs_lookup_parent(to, &ndir, nname);
    if (err) {
        vnode_put(odir);
        return err;
    }
    if (odir->mount != ndir->mount)
        err = E_XDEV;
    else
        err = odir->ops->rename ? odir->ops->rename(odir, oname, ndir, nname) : E_PERM;
    vnode_put(odir);
    vnode_put(ndir);
    return err;
}

int vfs_truncate(const char *path, uint32_t size)
{
    struct vnode *vn;
    int err = vfs_lookup(path, &vn);
    if (err)
        return err;
    err = vn->type != VN_FILE ? E_ISDIR : vn->ops->truncate ? vn->ops->truncate(vn, size) : E_PERM;
    vnode_put(vn);
    return err;
}

int vfs_opendir(const char *path, struct vdir *dir)
{
    struct vnode *vn;
    int err = vfs_lookup(path, &vn);
    if (err)
        return err;
    if (vn->type != VN_DIR) {
        vnode_put(vn);
        return E_NOTDIR;
    }
    dir->vn = vn;
    dir->index = 0;
    return 0;
}

int vfs_readdir(struct vdir *dir, struct vfs_dirent *out)
{
    if (!dir->vn->ops->readdir)
        return E_NOENT;
    memset(out, 0, sizeof(*out));
    int err = dir->vn->ops->readdir(dir->vn, dir->index, out);
    if (err == 0)
        dir->index++;
    return err;
}

void vfs_closedir(struct vdir *dir)
{
    vnode_put(dir->vn);
    dir->vn = NULL;
}

int vfs_read_all(const char *path, char **data, size_t *size)
{
    struct file *f;
    int err = vfs_open(path, VFS_READ, &f);
    if (err)
        return err;
    struct vstat st;
    vfs_fstat(f, &st);
    size_t cap = st.size + 1, len = 0;
    char *buf = kmalloc(cap);
    for (;;) {
        if (!buf) {
            vfs_close(f);
            return E_NOMEM;
        }
        long n = vfs_read(f, buf + len, cap - len - 1);
        if (n < 0) {
            kfree(buf);
            vfs_close(f);
            return (int)n;
        }
        if (n == 0)
            break;
        len += (size_t)n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = krealloc(buf, cap);
            if (!grown)
                kfree(buf);
            buf = grown;
        }
    }
    vfs_close(f);
    buf[len] = 0;
    *data = buf;
    if (size)
        *size = len;
    return 0;
}

int vfs_write_all(const char *path, const void *data, size_t size)
{
    struct file *f;
    int err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &f);
    if (err)
        return err;
    long n = size ? vfs_write(f, data, size) : 0;
    vfs_close(f);
    if (n < 0)
        return (int)n;
    return (size_t)n == size ? 0 : E_NOSPC;
}

static int copy_file(const char *from, const char *to)
{
    struct file *src, *dst;
    int err = vfs_open(from, VFS_READ, &src);
    if (err)
        return err;
    err = vfs_open(to, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &dst);
    if (err) {
        vfs_close(src);
        return err;
    }
    char *buf = kmalloc(4096);
    long n = buf ? 0 : E_NOMEM;
    while (buf && (n = vfs_read(src, buf, 4096)) > 0) {
        long w = vfs_write(dst, buf, (size_t)n);
        if (w != n) {
            n = w < 0 ? w : E_NOSPC;
            break;
        }
    }
    kfree(buf);
    vfs_close(src);
    vfs_close(dst);
    return n < 0 ? (int)n : 0;
}

int vfs_copy(const char *from, const char *to)
{
    if (!vfs_is_dir(from))
        return copy_file(from, to);
    int err = vfs_mkdir(to);
    if (err && err != E_EXIST)
        return err;
    struct vdir d;
    if ((err = vfs_opendir(from, &d)) != 0)
        return err;
    struct vfs_dirent ent;
    uint32_t index = 0;
    for (;;) {
        d.index = index++;
        if (vfs_readdir(&d, &ent) != 0)
            break;
        char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
        if ((err = path_join(from, ent.name, a, sizeof(a))) != 0 || (err = path_join(to, ent.name, b, sizeof(b))) != 0)
            break;
        if ((err = vfs_copy(a, b)) != 0)
            break;
    }
    vfs_closedir(&d);
    return err;
}
