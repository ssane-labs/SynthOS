#include <synth/block.h>
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/install.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

/* Installs SynthOS on another disk: the boot area (loaders, kernel and the EFI partition) is copied sector by
   sector, the partition table is resized to the new disk and the data partition is formatted or left for the
   first boot to set up. Each step does a small piece of work so a window can show the progress. */

#define CHUNK 256
#define MIN_DATA 8192
#define MOUNT_POINT "/mnt/install"

int install_targets(struct blockdev **out, int max)
{
    struct blockdev *boot = hal_boot_disk();
    int n = 0;
    for (struct blockdev *d = block_list(); d && n < max; d = d->next)
        if (!d->parent && d != boot && d->sectors >= hal_system_sectors() + MIN_DATA)
            out[n++] = d;
    return n;
}

const char *install_stage_name(enum install_stage stage)
{
    switch (stage) {
    case INSTALL_COPY: return "Copying the system";
    case INSTALL_PARTITIONS: return "Writing the partition table";
    case INSTALL_FORMAT: return "Preparing the data partition";
    case INSTALL_HOME: return "Copying your files";
    default: return "Done";
    }
}

static void release_target(struct blockdev *target)
{
    for (struct mount *m = vfs_mounts(); m;) {
        struct mount *next = m->next;
        struct blockdev *dev = block_find(m->source);
        if (dev && (dev == target || dev->parent == target))
            vfs_eject(m->path);
        m = next;
    }
}

int install_begin(struct install_job *job, struct blockdev *target, bool copy_home)
{
    memset(job, 0, sizeof(*job));
    job->source = hal_boot_disk();
    if (!job->source)
        return E_NODEV;
    if (!target || target == job->source || target->parent || target->sectors < hal_system_sectors() + MIN_DATA)
        return E_INVAL;
    release_target(target);
    job->target = target;
    job->copy_home = copy_home;
    job->total = hal_system_sectors();
    job->stage = INSTALL_COPY;
    str_copy(job->message, install_stage_name(INSTALL_COPY), sizeof(job->message));
    return 0;
}

static int fail(struct install_job *job, int err, const char *what)
{
    job->error = err;
    ksnprintf(job->message, sizeof(job->message), "%s: %s", what, err_str(err));
    if (job->data) {
        vfs_umount(MOUNT_POINT);
        block_unregister(job->data);
        job->data = NULL;
    }
    return err;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void next(struct install_job *job, enum install_stage stage)
{
    job->stage = stage;
    str_copy(job->message, install_stage_name(stage), sizeof(job->message));
}

int install_step(struct install_job *job)
{
    if (job->error)
        return job->error;
    uint32_t sys = hal_system_sectors();
    switch (job->stage) {
    case INSTALL_COPY: {
        uint8_t *buf = kmalloc(CHUNK * BLOCK_SIZE);
        if (!buf)
            return fail(job, E_NOMEM, "Copying");
        uint32_t n = MIN((uint32_t)CHUNK, sys - job->lba);
        int err = block_read(job->source, job->lba, n, buf);
        if (!err)
            err = block_write(job->target, job->lba, n, buf);
        kfree(buf);
        if (err)
            return fail(job, err, "Copying");
        job->lba += n;
        job->percent = (int)((uint64_t)job->lba * 80 / sys);
        if (job->lba >= sys)
            next(job, INSTALL_PARTITIONS);
        return 1;
    }
    case INSTALL_PARTITIONS: {
        uint8_t mbr[BLOCK_SIZE];
        int err = block_read(job->target, 0, 1, mbr);
        if (err)
            return fail(job, err, "Partition table");
        uint8_t *entry = mbr + 446 + 2 * 16;
        entry[4] = 0x7f;
        wr32(entry + 8, sys);
        wr32(entry + 12, job->target->sectors - sys);
        if ((err = block_write(job->target, 0, 1, mbr)) != 0)
            return fail(job, err, "Partition table");
        job->percent = 84;
        next(job, INSTALL_FORMAT);
        return 1;
    }
    case INSTALL_FORMAT: {
        static uint8_t zero[BLOCK_SIZE * 16];
        memset(zero, 0, sizeof(zero));
        int err = block_write(job->target, sys, 16, zero);
        if (err)
            return fail(job, err, "Data partition");
        if (!job->copy_home) {
            job->percent = 100;
            next(job, INSTALL_DONE);
            return 0;
        }
        job->data = block_add_partition(job->target, "install0", sys, job->target->sectors - sys);
        if (!job->data)
            return fail(job, E_NOMEM, "Data partition");
        if ((err = sfs_format(job->data)) != 0)
            return fail(job, err, "Formatting");
        job->percent = 90;
        next(job, INSTALL_HOME);
        return 1;
    }
    case INSTALL_HOME: {
        vfs_mkdirs(MOUNT_POINT);
        int err = vfs_mount("sfs", job->data->name, MOUNT_POINT);
        if (err)
            return fail(job, err, "Mounting the new disk");
        err = vfs_copy("/home", MOUNT_POINT);
        vfs_umount(MOUNT_POINT);
        vfs_unlink(MOUNT_POINT);
        block_unregister(job->data);
        job->data = NULL;
        if (err)
            return fail(job, err, "Copying your files");
        job->percent = 100;
        next(job, INSTALL_DONE);
        return 0;
    }
    default: return 0;
    }
}

static void list_targets(struct shell *sh)
{
    struct blockdev *list[16];
    int n = install_targets(list, 16);
    if (!n) {
        sh_puts(sh, "no disk to install on (it needs at least 12 MB and must not be the boot disk)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        char size[16];
        fmt_size(size, sizeof(size), (uint64_t)list[i]->sectors * BLOCK_SIZE);
        sh_printf(sh, "  %-8s %8s  %s\n", list[i]->name, size, list[i]->model[0] ? list[i]->model : "");
    }
}

COMMAND(install, "setup", "system", "install [<disk> [--copy-home] --yes]", "install SynthOS on another disk")
{
    const char *name = NULL;
    bool yes = false, home = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))
            yes = true;
        else if (!strcmp(argv[i], "--copy-home"))
            home = true;
        else
            name = argv[i];
    }
    if (!hal_boot_disk())
        return sh_error(sh, "install", "SynthOS did not start from a disk it can copy");
    if (!name) {
        list_targets(sh);
        return 0;
    }
    struct blockdev *target = block_find(name);
    struct install_job job;
    if (!target)
        return sh_error(sh, "install", "no disk %s", name);
    if (!yes)
        return sh_error(sh, "install", "this erases everything on %s; add --yes to go on", name);
    int err = install_begin(&job, target, home);
    if (err)
        return sh_error(sh, "install", "%s: %s", name, err == E_INVAL ? "not a disk you can install on" : err_str(err));
    int shown = -1;
    for (;;) {
        int r = install_step(&job);
        if (job.percent / 10 != shown) {
            shown = job.percent / 10;
            sh_printf(sh, "%3d%%  %s\n", job.percent, job.message);
        }
        if (r < 0)
            return sh_error(sh, "install", "%s", job.message);
        if (r == 0 && job.stage == INSTALL_DONE)
            break;
    }
    sh_printf(sh, "SynthOS is installed on %s. Remove the current boot disk and restart.\n", name);
    return 0;
}
