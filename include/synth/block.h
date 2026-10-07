#ifndef SYNTH_BLOCK_H
#define SYNTH_BLOCK_H

#include <synth/types.h>

#define BLOCK_SIZE 512

struct blockdev {
    char name[16];
    char model[41];
    uint32_t sectors;
    uint32_t offset;
    struct blockdev *parent;
    int (*read)(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf);
    void *priv;
    struct blockdev *next;
};

void block_register(struct blockdev *dev);
struct blockdev *block_add_partition(struct blockdev *parent, const char *name, uint32_t offset, uint32_t sectors);
struct blockdev *block_find(const char *name);
struct blockdev *block_list(void);
int block_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf);
int block_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf);

void block_unregister(struct blockdev *dev);
void block_set_listener(void (*listener)(struct blockdev *dev, bool added));

int sfs_format(struct blockdev *dev);
bool fat_detect(struct blockdev *dev);
void automount_init(void);
bool sfs_detect(struct blockdev *dev);

#endif
