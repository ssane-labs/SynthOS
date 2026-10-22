#include <synth/block.h>
#include <synth/command.h>
#include <synth/crypto.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/install.h>
#include <synth/parttab.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

/* Installs SynthOS from the disk it started from (usually the USB stick) onto another disk: over the whole disk,
   into free space or into the space of a partition the user gives up. The boot area, the kernel, the EFI files and
   the program bundle are copied sector by sector; an install record with a fresh id tells the loaders and the
   kernel which copy is which. Each step does a small piece of work so a window can show the progress. */

#define CHUNK 256
#define MIN_DATA 8192
#define RECORD 416
#define MOUNT_POINT "/mnt/install"
#define ESP_MOUNT "/mnt/install-esp"

static uint32_t bundle(void)
{
    return hal_bundle_sectors();
}

uint32_t install_min_sectors(bool whole)
{
    return (whole ? 0 : INSTALL_ESP_SECTORS) + hal_system_sectors() + bundle() + MIN_DATA;
}

int install_targets(struct blockdev **out, int max)
{
    struct blockdev *boot = hal_boot_disk();
    int n = 0;
    for (struct blockdev *d = block_list(); d && n < max; d = d->next)
        if (!d->parent && d != boot && d->sectors >= install_min_sectors(false))
            out[n++] = d;
    return n;
}

const char *install_stage_name(enum install_stage stage)
{
    switch (stage) {
    case INSTALL_PARTITIONS: return "Making room on the disk";
    case INSTALL_COPY: return "Copying the system";
    case INSTALL_RECORD: return "Writing the boot records";
    case INSTALL_FORMAT: return "Preparing the data partition";
    case INSTALL_HOME: return "Copying your files";
    case INSTALL_BOOT: return "Setting up the boot menu";
    default: return "Done";
    }
}

static int why(char *buf, size_t cap, int err, const char *text)
{
    if (buf)
        str_copy(buf, text, cap);
    return err;
}

int install_check(const struct install_plan *plan, char *buf, size_t cap)
{
    struct blockdev *d = plan->disk;
    if (!hal_system_area())
        return why(buf, cap, E_NODEV, "SynthOS did not start from a disk it can copy");
    if (!d || d->parent || d == hal_boot_disk())
        return why(buf, cap, E_INVAL, "SynthOS cannot be installed on the disk it runs from");
    if (plan->whole)
        return d->sectors < install_min_sectors(true) ? why(buf, cap, E_NOSPC, "The disk is too small") : 0;
    struct disk_map *map = kmalloc(sizeof(*map));
    if (!map)
        return E_NOMEM;
    int err = disk_map_read(d, map);
    if (err) {
        kfree(map);
        return why(buf, cap, err, "The disk cannot be read");
    }
    uint32_t end = plan->start + plan->count;
    bool inside = false;
    const struct part *replaced = NULL;
    for (int i = 0; i < map->count; i++) {
        const struct part *p = &map->parts[i];
        if (plan->replace >= 0 && p->slot == plan->replace && p->kind != PART_FREE)
            replaced = p;
        if (p->kind == PART_FREE && plan->start >= p->start && end <= p->start + p->count)
            inside = true;
    }
    if (replaced)
        inside = plan->start >= replaced->start && end <= replaced->start + replaced->count;
    if (map->scheme == SCHEME_NONE)
        err = why(buf, cap, E_INVAL, "The disk has no partition table: install on the whole disk");
    else if (map->read_only)
        err = why(buf, cap, E_RANGE, "The partition table cannot be changed (damaged, or a disk over 2 TB)");
    else if (plan->replace >= 0 && !replaced)
        err = why(buf, cap, E_INVAL, "No such partition");
    else if (replaced && (replaced->kind == PART_EFI || replaced->kind == PART_MSR || replaced->kind == PART_EXTENDED))
        err = why(buf, cap, E_INVAL, "This partition is needed by other systems and cannot be replaced");
    else if (!inside || plan->start % PART_ALIGN)
        err = why(buf, cap, E_RANGE, "The chosen space is not free");
    else if (plan->count < install_min_sectors(false))
        err = why(buf, cap, E_NOSPC, "Not enough space for SynthOS");
    else if (map->scheme == SCHEME_MBR && map->free_slots + (replaced ? 1 : 0) < 2)
        err = why(buf, cap, E_NOSPC, "The disk has no room for two more partitions (MBR holds four)");
    kfree(map);
    return err;
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

static void add_range(struct install_job *job, uint32_t from, uint32_t to, uint32_t count)
{
    if (!count)
        return;
    job->ranges[job->range_count++] = (struct install_range){from, to, count};
    job->total += count;
}

int install_begin(struct install_job *job, const struct install_plan *plan)
{
    memset(job, 0, sizeof(*job));
    int err = install_check(plan, job->message, sizeof(job->message));
    if (err)
        return err;
    job->plan = *plan;
    job->source = hal_system_area();
    uint32_t sys = hal_system_sectors();
    if (plan->whole) {
        job->system_lba = 0;
        job->system_sectors = plan->disk->sectors;
        job->esp_lba = sys - INSTALL_ESP_SECTORS;
        add_range(job, 0, 0, sys + bundle());
    } else {
        job->esp_lba = plan->start;
        job->system_lba = plan->start + INSTALL_ESP_SECTORS;
        job->system_sectors = plan->count - INSTALL_ESP_SECTORS;
        add_range(job, sys - INSTALL_ESP_SECTORS, job->esp_lba, INSTALL_ESP_SECTORS);
        add_range(job, 0, job->system_lba, sys - INSTALL_ESP_SECTORS);
        add_range(job, sys, job->system_lba + sys, bundle());
    }
    do
        random_bytes(job->id, sizeof(job->id));
    while (!memcmp(job->id, "\0\0\0\0\0\0\0\0", 8));
    release_target(plan->disk);
    job->stage = plan->whole ? INSTALL_COPY : INSTALL_PARTITIONS;
    str_copy(job->message, install_stage_name(job->stage), sizeof(job->message));
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
    if (job->esp) {
        vfs_umount(ESP_MOUNT);
        block_unregister(job->esp);
        job->esp = NULL;
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

static uint32_t data_lba(struct install_job *job)
{
    return job->system_lba + hal_system_sectors() + bundle();
}

static uint32_t data_end(struct install_job *job)
{
    return job->plan.whole ? job->plan.disk->sectors : job->system_lba + job->system_sectors;
}

static int make_room(struct install_job *job)
{
    struct disk_map *map = kmalloc(sizeof(*map));
    if (!map)
        return E_NOMEM;
    int err = disk_map_read(job->plan.disk, map);
    if (!err && job->plan.replace >= 0)
        err = disk_map_remove(map, job->plan.replace);
    if (!err)
        err = disk_map_add(map, job->esp_lba, INSTALL_ESP_SECTORS, PART_EFI, "SynthOS EFI");
    if (!err)
        err = disk_map_add(map, job->system_lba, job->system_sectors, PART_SYNTHOS, "SynthOS");
    kfree(map);
    return err;
}

static int copy_chunk(struct install_job *job)
{
    struct install_range *r = &job->ranges[job->range];
    uint8_t *buf = kmalloc(CHUNK * BLOCK_SIZE);
    if (!buf)
        return E_NOMEM;
    uint32_t n = MIN((uint32_t)CHUNK, r->count - job->lba);
    int err = block_read(job->source, r->from + job->lba, n, buf);
    if (!err)
        err = block_write(job->plan.disk, r->to + job->lba, n, buf);
    kfree(buf);
    if (err)
        return err;
    job->lba += n;
    job->copied += n;
    if (job->lba >= r->count) {
        job->lba = 0;
        job->range++;
    }
    return 0;
}

static int write_record(struct install_job *job)
{
    uint8_t sector[BLOCK_SIZE];
    struct blockdev *disk = job->plan.disk;
    int err = block_read(disk, job->system_lba, 1, sector);
    if (err)
        return err;
    memcpy(sector + RECORD, "SYID", 4);
    memcpy(sector + RECORD + 4, job->id, 8);
    wr32(sector + RECORD + 12, 1);
    if (job->plan.whole) {
        uint8_t *entry = sector + 446 + 2 * 16;
        entry[4] = 0x7f;
        wr32(entry + 8, data_lba(job));
        wr32(entry + 12, disk->sectors - data_lba(job));
    } else {
        memset(sector + 446, 0, 64);
    }
    if ((err = block_write(disk, job->system_lba, 1, sector)) != 0)
        return err;
    static uint8_t zero[BLOCK_SIZE * 16];
    memset(zero, 0, sizeof(zero));
    return block_write(disk, data_lba(job), 16, zero);
}

static const char *mounted_at(struct blockdev *dev)
{
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        if (!strcmp(m->source, dev->name))
            return m->path;
    return NULL;
}

/* The loader reads SYSTEM.CFG from its own EFI partition: where the system lives, its id, and that it still has
   to add itself to the firmware boot menu. */
static int write_boot_config(struct install_job *job)
{
    job->esp = block_add_partition(job->plan.disk, "inst-esp", job->esp_lba, INSTALL_ESP_SECTORS);
    if (!job->esp)
        return E_NOMEM;
    const char *root = mounted_at(job->esp);
    bool own = false;
    if (!root) {
        vfs_mkdirs(ESP_MOUNT);
        int err = vfs_mount("fat", job->esp->name, ESP_MOUNT);
        if (err)
            return err;
        root = ESP_MOUNT;
        own = true;
    }
    char path[PATH_MAX_LEN], text[256];
    ksnprintf(text, sizeof(text),
              "# SynthOS installation: the loader reads this file\nsystem=%u\nsectors=%u\n"
              "id=%02x%02x%02x%02x%02x%02x%02x%02x\nregister=1\ntimeout=3\n",
              (unsigned)job->system_lba, (unsigned)(job->plan.whole ? 0 : job->system_sectors), job->id[0], job->id[1],
              job->id[2], job->id[3], job->id[4], job->id[5], job->id[6], job->id[7]);
    ksnprintf(path, sizeof(path), "%s/SYNTHOS/SYSTEM.CFG", root);
    int err = vfs_write_all(path, text, strlen(text));
    if (own) {
        int e2 = vfs_umount(ESP_MOUNT);
        vfs_unlink(ESP_MOUNT);
        if (!err)
            err = e2;
    }
    block_unregister(job->esp);
    job->esp = NULL;
    return err;
}

int install_step(struct install_job *job)
{
    if (job->error)
        return job->error;
    int err = 0;
    switch (job->stage) {
    case INSTALL_PARTITIONS:
        if ((err = make_room(job)) != 0)
            return fail(job, err, "Partition table");
        job->percent = 2;
        next(job, INSTALL_COPY);
        return 1;
    case INSTALL_COPY:
        if ((err = copy_chunk(job)) != 0)
            return fail(job, err, "Copying");
        job->percent = 2 + (int)((uint64_t)job->copied * 80 / job->total);
        if (job->range >= job->range_count)
            next(job, INSTALL_RECORD);
        return 1;
    case INSTALL_RECORD:
        if ((err = write_record(job)) != 0)
            return fail(job, err, "Boot records");
        job->percent = 84;
        next(job, job->plan.copy_home ? INSTALL_FORMAT : INSTALL_BOOT);
        return 1;
    case INSTALL_FORMAT:
        job->data = block_add_partition(job->plan.disk, "inst-data", data_lba(job), data_end(job) - data_lba(job));
        if (!job->data)
            return fail(job, E_NOMEM, "Data partition");
        if ((err = sfs_format(job->data)) != 0)
            return fail(job, err, "Formatting");
        job->percent = 88;
        next(job, INSTALL_HOME);
        return 1;
    case INSTALL_HOME:
        vfs_mkdirs(MOUNT_POINT);
        if ((err = vfs_mount("sfs", job->data->name, MOUNT_POINT)) != 0)
            return fail(job, err, "Mounting the new disk");
        err = vfs_copy("/home", MOUNT_POINT);
        vfs_umount(MOUNT_POINT);
        vfs_unlink(MOUNT_POINT);
        block_unregister(job->data);
        job->data = NULL;
        if (err)
            return fail(job, err, "Copying your files");
        job->percent = 96;
        next(job, INSTALL_BOOT);
        return 1;
    case INSTALL_BOOT:
        if ((err = write_boot_config(job)) != 0)
            return fail(job, err, "Boot menu");
        job->percent = 100;
        next(job, INSTALL_DONE);
        return 0;
    default: return 0;
    }
}

static void print_map(struct shell *sh, struct blockdev *d)
{
    char size[16];
    fmt_size(size, sizeof(size), (uint64_t)d->sectors * BLOCK_SIZE);
    struct disk_map *map = kmalloc(sizeof(*map));
    if (!map)
        return;
    int err = disk_map_read(d, map);
    sh_printf(sh, "%s  %s  %s  (%s%s)\n", d->name, size, d->model[0] ? d->model : "disk",
              err ? err_str(err) : part_scheme_name(map->scheme), map->read_only ? ", read-only" : "");
    int free_index = 0;
    for (int i = 0; !err && i < map->count; i++) {
        const struct part *p = &map->parts[i];
        fmt_size(size, sizeof(size), (uint64_t)p->count * BLOCK_SIZE);
        if (p->slot < 0 && p->kind != PART_FREE)
            sh_printf(sh, "    whole disk %-18s %5s  %s %s\n", "", size, p->fs, p->label);
        else if (p->kind == PART_FREE)
            sh_printf(sh, "    free %-3d %-18s %9s  at %u\n", ++free_index, "", size, (unsigned)p->start);
        else
            sh_printf(sh, "    part %-3d %-18s %9s  at %u  %s %s\n", p->slot + 1, part_kind_name(p->kind), size,
                      (unsigned)p->start, p->fs, p->label);
    }
    kfree(map);
}

static int find_free(struct blockdev *d, int index, struct part *out)
{
    struct disk_map *map = kmalloc(sizeof(*map));
    if (!map)
        return E_NOMEM;
    int err = disk_map_read(d, map), seen = 0;
    if (!err)
        err = E_NOENT;
    for (int i = 0; err == E_NOENT && i < map->count; i++)
        if (map->parts[i].kind == PART_FREE && ++seen == index) {
            *out = map->parts[i];
            err = 0;
        }
    for (int i = 0; err == E_NOENT && i < map->count && index < 0; i++)
        if (map->parts[i].kind != PART_FREE && map->parts[i].slot == -index - 1) {
            *out = map->parts[i];
            err = 0;
        }
    kfree(map);
    return err;
}

static int run_job(struct shell *sh, struct install_job *job)
{
    int shown = -1;
    for (;;) {
        int r = install_step(job);
        if (job->percent / 10 != shown) {
            shown = job->percent / 10;
            sh_printf(sh, "%3d%%  %s\n", job->percent, job->message);
        }
        if (r < 0)
            return sh_error(sh, "install", "%s", job->message);
        if (r == 0 && job->stage == INSTALL_DONE)
            return 0;
    }
}

COMMAND(install, "setup", "system",
        "install [<disk> --whole | --free <n> [--size <MB>] | --replace <part> [--size <MB>]] [--copy-home] --yes",
        "install SynthOS on a disk, next to other systems or over the whole disk")
{
    const char *name = NULL;
    bool yes = false;
    struct install_plan plan = {.replace = -1};
    int free_index = 0;
    uint32_t size_mb = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))
            yes = true;
        else if (!strcmp(argv[i], "--copy-home"))
            plan.copy_home = true;
        else if (!strcmp(argv[i], "--whole"))
            plan.whole = true;
        else if (!strcmp(argv[i], "--free") && i + 1 < argc)
            free_index = (int)(uint32_t)str_to_long(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--replace") && i + 1 < argc)
            plan.replace = (int)(uint32_t)str_to_long(argv[++i], NULL, 10) - 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            size_mb = (uint32_t)str_to_long(argv[++i], NULL, 10);
        else
            name = argv[i];
    }
    if (!hal_system_area())
        return sh_error(sh, "install", "SynthOS did not start from a disk it can copy");
    if (!name) {
        struct blockdev *list[16];
        int n = install_targets(list, 16);
        if (!n)
            sh_puts(sh, "no disk to install on (the boot disk itself cannot be used)\n");
        for (int i = 0; i < n; i++)
            print_map(sh, list[i]);
        char size[16];
        fmt_size(size, sizeof(size), (uint64_t)install_min_sectors(false) * BLOCK_SIZE);
        sh_printf(sh, "SynthOS needs at least %s next to other systems.\n", size);
        return 0;
    }
    plan.disk = block_find(name);
    if (!plan.disk)
        return sh_error(sh, "install", "no disk %s", name);
    if (!plan.whole) {
        if (!free_index && plan.replace < 0)
            return sh_error(sh, "install", "say where: --whole, --free <n> or --replace <part>");
        struct part space;
        int err = find_free(plan.disk, plan.replace >= 0 ? -(plan.replace + 1) : free_index, &space);
        if (err)
            return sh_error(sh, "install", "%s: no such %s", name, plan.replace >= 0 ? "partition" : "free space");
        plan.start = (space.start + PART_ALIGN - 1) / PART_ALIGN * PART_ALIGN;
        plan.count = space.start + space.count - plan.start;
        if (size_mb && (uint64_t)size_mb * 2048 < plan.count)
            plan.count = size_mb * 2048;
    }
    char reason[96];
    int err = install_check(&plan, reason, sizeof(reason));
    if (err)
        return sh_error(sh, "install", "%s", reason);
    if (!yes)
        return sh_error(sh, "install", plan.whole ? "this erases everything on %s; add --yes to go on"
                                                  : "this changes the partitions of %s; add --yes to go on",
                        name);
    struct install_job *job = kmalloc(sizeof(*job));
    if (!job)
        return sh_error(sh, "install", "out of memory");
    err = install_begin(job, &plan);
    if (!err)
        err = run_job(sh, job);
    else
        sh_error(sh, "install", "%s", job->message);
    if (!err)
        sh_printf(sh, "SynthOS is installed on %s (system at sector %u). Restart with the USB stick still in: it "
                      "adds SynthOS to the boot menu and starts it.\n",
                  name, (unsigned)job->system_lba);
    kfree(job);
    return err ? 1 : 0;
}

static enum part_kind kind_from(const char *s)
{
    if (!strcmp(s, "efi"))
        return PART_EFI;
    if (!strcmp(s, "synthos"))
        return PART_SYNTHOS;
    if (!strcmp(s, "linux"))
        return PART_LINUX;
    return PART_DATA;
}

COMMAND(part, "setup", "system", "part [<disk> [init gpt|mbr | add <MB> [efi|data|linux|synthos] [<name>] | del <n>] --yes]",
        "show and change partition tables")
{
    bool yes = false;
    int n = 0;
    const char *args[8];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))
            yes = true;
        else if (n < 8)
            args[n++] = argv[i];
    }
    if (!n) {
        for (struct blockdev *d = block_list(); d; d = d->next)
            if (!d->parent)
                print_map(sh, d);
        return 0;
    }
    struct blockdev *d = block_find(args[0]);
    if (!d || d->parent)
        return sh_error(sh, "part", "no disk %s", args[0]);
    if (n == 1) {
        print_map(sh, d);
        return 0;
    }
    if (d == hal_boot_disk())
        return sh_error(sh, "part", "%s holds the running system", d->name);
    if (!yes)
        return sh_error(sh, "part", "this changes the partitions of %s; add --yes to go on", d->name);
    release_target(d);
    int err = E_INVAL;
    if (!strcmp(args[1], "init") && n >= 3) {
        err = disk_map_init(d, !strcmp(args[2], "gpt") ? SCHEME_GPT : SCHEME_MBR);
    } else if (!strcmp(args[1], "add") && n >= 3) {
        uint32_t want = (uint32_t)str_to_long(args[2], NULL, 10) * 2048;
        struct disk_map *map = kmalloc(sizeof(*map));
        if (!map)
            return sh_error(sh, "part", "out of memory");
        err = disk_map_read(d, map);
        int k = 0;
        while (!err && k < map->count && !(map->parts[k].kind == PART_FREE && map->parts[k].count >= want))
            k++;
        if (!err)
            err = k < map->count ? disk_map_add(map, map->parts[k].start, want, n >= 4 ? kind_from(args[3]) : PART_DATA,
                                                n >= 5 ? args[4] : "")
                                 : E_NOSPC;
        kfree(map);
    } else if (!strcmp(args[1], "del") && n >= 3) {
        struct disk_map *map = kmalloc(sizeof(*map));
        if (!map)
            return sh_error(sh, "part", "out of memory");
        err = disk_map_read(d, map);
        if (!err)
            err = disk_map_remove(map, (int)(uint32_t)str_to_long(args[2], NULL, 10) - 1);
        kfree(map);
    }
    if (err)
        return sh_error(sh, "part", "%s: %s", d->name, err_str(err));
    print_map(sh, d);
    return 0;
}
