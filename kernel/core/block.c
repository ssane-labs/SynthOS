#include <synth/block.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>

static struct blockdev *devices;
static void (*listener)(struct blockdev *dev, bool added);

void block_set_listener(void (*fn)(struct blockdev *dev, bool added))
{
    listener = fn;
}

void block_unregister(struct blockdev *dev)
{
    for (struct blockdev *d = devices; d; d = d->next)
        if ((d == dev || d->parent == dev) && listener)
            listener(d, false);
    for (struct blockdev **link = &devices; *link;) {
        if (*link == dev || (*link)->parent == dev)
            *link = (*link)->next;
        else
            link = &(*link)->next;
    }
    klog("block: %s removed", dev->name);
}

void block_register(struct blockdev *dev)
{
    struct blockdev **link = &devices;
    while (*link)
        link = &(*link)->next;
    dev->next = NULL;
    *link = dev;
    klog("block: %s, %u sectors%s%s", dev->name, (unsigned)dev->sectors, dev->model[0] ? ", " : "", dev->model);
    if (listener)
        listener(dev, true);
}

struct blockdev *block_add_partition(struct blockdev *parent, const char *name, uint32_t offset, uint32_t sectors)
{
    if (offset >= parent->sectors)
        return NULL;
    struct blockdev *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    str_copy(p->name, name, sizeof(p->name));
    str_copy(p->model, parent->model, sizeof(p->model));
    p->parent = parent;
    p->offset = offset;
    p->sectors = MIN(sectors, parent->sectors - offset);
    block_register(p);
    return p;
}

struct blockdev *block_find(const char *name)
{
    for (struct blockdev *d = devices; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

struct blockdev *block_list(void)
{
    return devices;
}

int block_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    if (lba + count > dev->sectors || lba + count < lba)
        return E_RANGE;
    if (dev->parent)
        return block_read(dev->parent, lba + dev->offset, count, buf);
    return dev->read(dev, lba, count, buf);
}

int block_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    if (lba + count > dev->sectors || lba + count < lba)
        return E_RANGE;
    if (dev->parent)
        return block_write(dev->parent, lba + dev->offset, count, buf);
    return dev->write ? dev->write(dev, lba, count, buf) : E_PERM;
}
