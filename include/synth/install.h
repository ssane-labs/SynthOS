#ifndef SYNTH_INSTALL_H
#define SYNTH_INSTALL_H

#include <synth/types.h>

struct blockdev;

#define INSTALL_BOOT_SECTORS 8192u
#define INSTALL_ESP_SECTORS (hal_system_sectors() - INSTALL_BOOT_SECTORS)

enum install_stage {
    INSTALL_PARTITIONS,
    INSTALL_COPY,
    INSTALL_RECORD,
    INSTALL_FORMAT,
    INSTALL_HOME,
    INSTALL_BOOT,
    INSTALL_DONE,
};

/* Whole disk: the disk is erased and laid out like the boot image. Otherwise SynthOS takes [start, start + count)
   (free space, or the space of the partition in `replace`, which is deleted): a small EFI partition for its loader
   and a SynthOS partition with the system and the data. */
struct install_plan {
    struct blockdev *disk;
    bool whole;
    int replace;
    uint32_t start, count;
    bool copy_home;
};

struct install_range {
    uint32_t from, to, count;
};

struct install_job {
    struct install_plan plan;
    struct blockdev *source, *data, *esp;
    enum install_stage stage;
    struct install_range ranges[3];
    int range_count, range;
    uint32_t lba, copied, total;
    uint32_t esp_lba, system_lba, system_sectors;
    uint8_t id[8];
    int percent;
    int error;
    char message[96];
};

int install_targets(struct blockdev **out, int max);
uint32_t install_min_sectors(bool whole);
int install_check(const struct install_plan *plan, char *why, size_t cap);
int install_begin(struct install_job *job, const struct install_plan *plan);
int install_step(struct install_job *job);
const char *install_stage_name(enum install_stage stage);

#endif
