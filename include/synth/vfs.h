#ifndef SYNTH_VFS_H
#define SYNTH_VFS_H

#include <synth/path.h>
#include <synth/types.h>

enum vnode_type {
    VN_FILE = 1,
    VN_DIR = 2,
    VN_DEV = 3,
};

enum {
    VFS_READ = 1u << 0,
    VFS_WRITE = 1u << 1,
    VFS_RDWR = VFS_READ | VFS_WRITE,
    VFS_CREATE = 1u << 2,
    VFS_TRUNC = 1u << 3,
    VFS_APPEND = 1u << 4,
    VFS_EXCL = 1u << 5,
};

enum {
    VFS_SEEK_SET = 0,
    VFS_SEEK_CUR = 1,
    VFS_SEEK_END = 2,
};

struct vnode;
struct mount;

struct vstat {
    uint8_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t inode;
};

struct vfs_dirent {
    char name[NAME_MAX_LEN];
    uint8_t type;
    uint32_t size;
};

struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, int type, struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*rename)(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname);
    int (*readdir)(struct vnode *dir, uint32_t index, struct vfs_dirent *out);
    long (*read)(struct vnode *vn, uint32_t off, void *buf, size_t n);
    long (*write)(struct vnode *vn, uint32_t off, const void *buf, size_t n);
    int (*truncate)(struct vnode *vn, uint32_t size);
    int (*stat)(struct vnode *vn, struct vstat *st);
    void (*release)(struct vnode *vn);
};

struct vnode {
    const struct vnode_ops *ops;
    struct mount *mount;
    void *priv;
    int refs;
    uint8_t type;
};

struct fs_type {
    const char *name;
    const char *description;
    int (*mount)(const char *source, struct vnode **root);
    int (*statfs)(struct vnode *root, uint64_t *total, uint64_t *free);
    int (*label)(struct vnode *root, char *out, size_t cap);
};

#define FS_TYPE(object) SYNTH_REGISTER(synth_fs, struct fs_type, object)

struct mount {
    char path[PATH_MAX_LEN];
    char source[PATH_MAX_LEN];
    const struct fs_type *fs;
    struct vnode *root;
    struct mount *next;
};

struct file {
    struct vnode *vn;
    uint32_t pos;
    unsigned flags;
};

struct vdir {
    struct vnode *vn;
    uint32_t index;
};

void vfs_init(void);
const struct fs_type *vfs_find_fs(const char *name);
void vfs_foreach_fs(void (*fn)(const struct fs_type *fs, void *ctx), void *ctx);
int vfs_mount(const char *fsname, const char *source, const char *path);
int vfs_umount(const char *path);
struct mount *vfs_mounts(void);
int vfs_free_space(const char *path, uint64_t *free);

struct vnode *vnode_get(struct vnode *vn);
void vnode_put(struct vnode *vn);
int vfs_lookup(const char *path, struct vnode **out);
int vfs_lookup_parent(const char *path, struct vnode **dir, char *name);

int vfs_open(const char *path, unsigned flags, struct file **out);
long vfs_read(struct file *f, void *buf, size_t n);
long vfs_write(struct file *f, const void *buf, size_t n);
long vfs_seek(struct file *f, long off, int whence);
int vfs_fstat(struct file *f, struct vstat *st);
void vfs_close(struct file *f);

int vfs_stat(const char *path, struct vstat *st);
int vfs_mkdir(const char *path);
int vfs_mkdirs(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *from, const char *to);
int vfs_truncate(const char *path, uint32_t size);
bool vfs_exists(const char *path);
bool vfs_is_dir(const char *path);

int vfs_opendir(const char *path, struct vdir *dir);
int vfs_readdir(struct vdir *dir, struct vfs_dirent *out);
void vfs_closedir(struct vdir *dir);

int vfs_read_all(const char *path, char **data, size_t *size);
int vfs_write_all(const char *path, const void *data, size_t size);
int vfs_copy(const char *from, const char *to);
int vfs_remove_tree(const char *path);

struct volume {
    char label[48];
    char path[PATH_MAX_LEN];
    char source[16];
};

int vfs_volumes(struct volume *out, int max);
int vfs_eject(const char *path);

int ramdisk_unpack(const void *data, size_t size, const char *dest);

#endif
