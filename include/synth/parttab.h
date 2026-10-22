#ifndef SYNTH_PARTTAB_H
#define SYNTH_PARTTAB_H

#include <synth/types.h>

struct blockdev;

#define PART_ALIGN 2048u
#define PART_MAX 40

enum part_kind {
    PART_FREE,
    PART_EFI,
    PART_SYNTHOS,
    PART_WINDOWS,
    PART_MSR,
    PART_RECOVERY,
    PART_LINUX,
    PART_DATA,
    PART_EXTENDED,
    PART_OTHER,
};

enum part_scheme {
    SCHEME_NONE,
    SCHEME_MBR,
    SCHEME_GPT,
};

struct part {
    uint32_t start, count;
    int slot;
    enum part_kind kind;
    char fs[12];
    char label[40];
};

struct disk_map {
    struct blockdev *dev;
    enum part_scheme scheme;
    bool read_only;
    int count, free_slots;
    struct part parts[PART_MAX];
};

int disk_map_read(struct blockdev *dev, struct disk_map *map);
int disk_map_add(struct disk_map *map, uint32_t start, uint32_t count, enum part_kind kind, const char *name);
int disk_map_remove(struct disk_map *map, int slot);
int disk_map_init(struct blockdev *dev, enum part_scheme scheme);
const char *part_kind_name(enum part_kind kind);
const char *part_scheme_name(enum part_scheme scheme);

#endif
