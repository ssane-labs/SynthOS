#include <synth/block.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vfs.h>

static void mount_volume(struct blockdev *dev)
{
    if (!fat_detect(dev))
        return;
    char path[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), "/mnt/%s", dev->name);
    vfs_mkdirs(path);
    int err = vfs_mount("fat", dev->name, path);
    if (err)
        klog("automount: %s: %s", dev->name, err_str(err));
}

static bool has_children(struct blockdev *dev)
{
    for (struct blockdev *d = block_list(); d; d = d->next)
        if (d->parent == dev)
            return true;
    return false;
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void add_partition(struct blockdev *dev, int number, uint64_t start, uint64_t count)
{
    if (!start || !count || start >= dev->sectors)
        return;
    char name[16];
    ksnprintf(name, sizeof(name), "%sp%d", dev->name, number);
    block_add_partition(dev, name, (uint32_t)start, (uint32_t)MIN(count, (uint64_t)dev->sectors - start));
}

static void scan_gpt(struct blockdev *dev)
{
    static uint8_t buf[BLOCK_SIZE];
    if (block_read(dev, 1, 1, buf) || memcmp(buf, "EFI PART", 8))
        return;
    uint64_t table = le64(buf + 72);
    uint32_t entries = MIN(buf[80] | buf[81] << 8 | (uint32_t)buf[82] << 16, 128u), size = buf[84] | buf[85] << 8;
    if (size < 128 || size > BLOCK_SIZE || BLOCK_SIZE % size)
        return;
    int number = 1;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t per = BLOCK_SIZE / size;
        if (i % per == 0 && block_read(dev, (uint32_t)(table + i / per), 1, buf))
            return;
        const uint8_t *e = buf + (i % per) * size;
        bool used = false;
        for (int k = 0; k < 16; k++)
            used |= e[k] != 0;
        if (used) {
            uint64_t first = le64(e + 32), last = le64(e + 40);
            add_partition(dev, number, first, last >= first ? last - first + 1 : 0);
        }
        number++;
    }
}

static void scan(struct blockdev *dev)
{
    static uint8_t mbr[BLOCK_SIZE];
    if (dev->parent || has_children(dev))
        return;
    if (fat_detect(dev)) {
        mount_volume(dev);
        return;
    }
    if (block_read(dev, 0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa)
        return;
    for (int i = 0; i < 4; i++)
        if (mbr[446 + i * 16 + 4] == 0xee) {
            scan_gpt(dev);
            return;
        }
    uint8_t entries[64];
    memcpy(entries, mbr + 446, sizeof(entries));
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = entries + i * 16;
        uint32_t start = e[8] | e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        uint32_t count = e[12] | e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
        if ((e[0] == 0 || e[0] == 0x80) && e[4] && e[4] != 0x05 && e[4] != 0x0f)
            add_partition(dev, i + 1, start, count);
    }
}

static void changed(struct blockdev *dev, bool added)
{
    if (added) {
        if (dev->parent)
            mount_volume(dev);
        else
            scan(dev);
        return;
    }
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        if (!strcmp(m->source, dev->name)) {
            char path[PATH_MAX_LEN];
            str_copy(path, m->path, sizeof(path));
            if (vfs_umount(path) == 0)
                vfs_unlink(path);
            break;
        }
}

void automount_init(void)
{
    block_set_listener(changed);
    for (struct blockdev *d = block_list(); d; d = d->next)
        if (!d->parent)
            scan(d);
}
