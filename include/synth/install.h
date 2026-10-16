#ifndef SYNTH_INSTALL_H
#define SYNTH_INSTALL_H

#include <synth/types.h>

struct blockdev;

enum install_stage {
    INSTALL_COPY,
    INSTALL_PARTITIONS,
    INSTALL_FORMAT,
    INSTALL_HOME,
    INSTALL_DONE,
};

struct install_job {
    struct blockdev *source, *target, *data;
    bool copy_home;
    enum install_stage stage;
    uint32_t lba, total;
    int percent;
    int error;
    char message[96];
};

int install_targets(struct blockdev **out, int max);
int install_begin(struct install_job *job, struct blockdev *target, bool copy_home);
int install_step(struct install_job *job);
const char *install_stage_name(enum install_stage stage);

#endif
