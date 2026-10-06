#include <synth/error.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vfs.h>

#define RAMDISK_MAGIC "SRD1"

enum {
    RD_FILE = 1,
    RD_DIR = 2,
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

int ramdisk_unpack(const void *data, size_t size, const char *dest)
{
    const uint8_t *p = data;
    if (size < 8 || memcmp(p, RAMDISK_MAGIC, 4) != 0)
        return E_INVAL;
    uint32_t count = rd32(p + 4);
    size_t off = 8;
    int files = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 8 > size)
            return E_IO;
        uint16_t name_len = rd16(p + off);
        uint8_t type = p[off + 2];
        uint32_t len = rd32(p + off + 4);
        off += 8;
        if (off + name_len + len > size || name_len >= PATH_MAX_LEN)
            return E_IO;
        char rel[PATH_MAX_LEN], path[PATH_MAX_LEN];
        memcpy(rel, p + off, name_len);
        rel[name_len] = 0;
        off += name_len;
        if (path_resolve(dest, rel, path, sizeof(path)) == 0) {
            if (type == RD_DIR) {
                vfs_mkdirs(path);
            } else if (type == RD_FILE) {
                char dir[PATH_MAX_LEN];
                path_dirname(path, dir, sizeof(dir));
                vfs_mkdirs(dir);
                int err = vfs_write_all(path, p + off, len);
                if (err)
                    klog("ramdisk: %s: %s", path, err_str(err));
                else
                    files++;
            }
        }
        off = ALIGN_UP(off + len, 4u);
    }
    klog("ramdisk: unpacked %d files into %s", files, dest);
    return files;
}
