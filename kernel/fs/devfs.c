#include <synth/console.h>
#include <synth/error.h>
#include <synth/hal.h>
#include <synth/string.h>
#include <synth/vfs.h>

struct device {
    const char *name;
    long (*read)(void *buf, size_t n);
    long (*write)(const void *buf, size_t n);
};

static long null_read(void *buf, size_t n)
{
    UNUSED(buf);
    UNUSED(n);
    return 0;
}

static long sink_write(const void *buf, size_t n)
{
    UNUSED(buf);
    return (long)n;
}

static long zero_read(void *buf, size_t n)
{
    memset(buf, 0, n);
    return (long)n;
}

static long random_read(void *buf, size_t n)
{
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(hal_entropy() >> 8);
    return (long)n;
}

static long console_write(const void *buf, size_t n)
{
    con_write(buf, n);
    return (long)n;
}

static const struct device devices[] = {
    {"console", null_read, console_write},
    {"null", null_read, sink_write},
    {"random", random_read, sink_write},
    {"zero", zero_read, sink_write},
};

static const struct vnode_ops dev_ops;
static const struct vnode_ops dir_ops;
static struct vnode dev_nodes[ARRAY_SIZE(devices)];
static struct vnode dev_root;

static int dev_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    UNUSED(dir);
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        if (strcmp(devices[i].name, name) == 0) {
            *out = vnode_get(&dev_nodes[i]);
            return 0;
        }
    }
    return E_NOENT;
}

static int dev_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out)
{
    UNUSED(dir);
    if (index >= ARRAY_SIZE(devices))
        return E_NOENT;
    str_copy(out->name, devices[index].name, sizeof(out->name));
    out->type = VN_DEV;
    return 0;
}

static long dev_read(struct vnode *vn, uint32_t off, void *buf, size_t n)
{
    UNUSED(off);
    return ((const struct device *)vn->priv)->read(buf, n);
}

static long dev_write(struct vnode *vn, uint32_t off, const void *buf, size_t n)
{
    UNUSED(off);
    return ((const struct device *)vn->priv)->write(buf, n);
}

static int dev_truncate(struct vnode *vn, uint32_t size)
{
    UNUSED(vn);
    UNUSED(size);
    return 0;
}

static const struct vnode_ops dir_ops = {
    .lookup = dev_lookup,
    .readdir = dev_readdir,
};

static const struct vnode_ops dev_ops = {
    .read = dev_read,
    .write = dev_write,
    .truncate = dev_truncate,
};

static int devfs_mount(const char *source, struct vnode **root)
{
    UNUSED(source);
    dev_root = (struct vnode){.ops = &dir_ops, .type = VN_DIR, .refs = 1};
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++)
        dev_nodes[i] = (struct vnode){.ops = &dev_ops, .type = VN_DEV, .refs = 1, .priv = (void *)&devices[i]};
    *root = &dev_root;
    return 0;
}

static const struct fs_type devfs = {
    .name = "devfs",
    .description = "device nodes",
    .mount = devfs_mount,
};
FS_TYPE(devfs);
