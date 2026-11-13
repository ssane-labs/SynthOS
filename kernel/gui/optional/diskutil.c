#include <synth/parttab.h>
#include <synth/module.h>
#include <synth/hal.h>
#include <synth/error.h>
#include <synth/command.h>
#include <synth/block.h>
#include <synth/path.h>
#include <synth/vfs.h>

#include "systool.h"

/* Disk Utility: every mounted volume with its use, opened in Files or ejected, and the disks with their partition
   maps: partitions are added in free space, deleted and formatted (FAT32 or SynthOS), a disk gets a new GPT or MBR
   table. The disk of the running system is only shown. */

#define MAX_VOLS 12

struct vol {
    char path[PATH_MAX_LEN], source[48], fs[16], label[48];
    uint64_t total, free;
    bool removable;
};

struct diskutil {
    struct ui_state st;
    struct vol vols[MAX_VOLS];
    int count, selected;
    struct tool_text view;
};

static struct window *open_win;

static void scan(struct diskutil *d)
{
    struct volume removable[8];
    int nr = vfs_volumes(removable, 8);
    d->count = 0;
    for (struct mount *mt = vfs_mounts(); mt && d->count < MAX_VOLS; mt = mt->next) {
        uint64_t t = 0, f = 0;
        if (!mt->fs->statfs || mt->fs->statfs(mt->root, &t, &f) != 0 || !t || !strcmp(mt->fs->name, "devfs"))
            continue;
        struct vol *v = &d->vols[d->count++];
        str_copy(v->path, mt->path, sizeof(v->path));
        str_copy(v->source, mt->source[0] ? mt->source : "memory", sizeof(v->source));
        str_copy(v->fs, mt->fs->name, sizeof(v->fs));
        v->total = t;
        v->free = f;
        v->removable = false;
        const char *name = !strcmp(mt->path, "/") ? "System (in memory)" : !strcmp(mt->path, "/home") ? "SynthOS HD"
                                                                                                      : path_basename(mt->path);
        str_copy(v->label, name, sizeof(v->label));
        for (int i = 0; i < nr; i++)
            if (!strcmp(removable[i].path, mt->path)) {
                str_copy(v->label, removable[i].label, sizeof(v->label));
                v->removable = true;
            }
    }
    d->selected = CLAMP(d->selected, 0, d->count); /* count = the Disks page */
}

static void legend(struct ui *ui, int x, int y, uint32_t color, const char *name, uint64_t bytes)
{
    char size[16];
    fmt_size(size, sizeof(size), bytes);
    if (ui->c)
        gfx_round(ui->c, (struct rect){x, y + 4, 12, 12}, 3, color);
    ui_label(ui, (struct rect){x + 20, y, 70, 20}, name, &font_small, style.secondary, ALIGN_LEFT);
    ui_label(ui, (struct rect){x + 20, y + 18, 120, 22}, size, &font_bold, style.text, ALIGN_LEFT);
}

static void volume(struct ui *ui, struct diskutil *d, struct rect area)
{
    struct vol *v = &d->vols[d->selected];
    int x = area.x + 28, w = area.w - 56, y = area.y + 22;
    if (ui->c)
        gfx_icon(ui->c, x, y, v->removable ? ICON_USB : ICON_DISKUTIL, 64);
    ui_label(ui, (struct rect){x + 82, y + 4, w - 82, 30}, v->label, &font_title, style.text, ALIGN_LEFT);
    char line[128], total[16];
    fmt_size(total, sizeof(total), v->total);
    ksnprintf(line, sizeof(line), "%s volume  -  %s  -  mounted at %s", v->fs, total, v->path);
    ui_label(ui, (struct rect){x + 82, y + 36, w - 82, 20}, line, &font_sans, style.secondary, ALIGN_LEFT);
    y += 96;
    uint64_t used = v->total - v->free;
    struct rect bar = {x, y, w, 22};
    if (ui->c) {
        gfx_round(ui->c, bar, 7, style.dark ? RGB(64, 64, 70) : RGB(222, 222, 228));
        int uw = (int)((uint64_t)bar.w * used / v->total);
        if (uw > 0)
            gfx_round_gradient(ui->c, (struct rect){bar.x, bar.y, MAX(uw, 14), bar.h}, 7, RGB(90, 170, 255),
                               RGB(30, 110, 230));
    }
    y += 36;
    legend(ui, x, y, RGB(60, 140, 245), "Used", used);
    legend(ui, x + 160, y, style.dark ? RGB(64, 64, 70) : RGB(222, 222, 228), "Free", v->free);
    legend(ui, x + 320, y, style.separator, "Capacity", v->total);
    y += 64;
    struct rect info = {x, y, w, 4 * 34};
    if (ui->c)
        gfx_round(ui->c, info, 12, style.group);
    static const char *const keys[] = {"Mount Point", "Device", "Format", "Kind"};
    const char *vals[] = {v->path, v->source, v->fs, v->removable ? "Removable" : "Internal"};
    for (int i = 0; i < 4; i++) {
        struct rect row = {x + 16, y + i * 34, w - 32, 34};
        ui_label(ui, row, keys[i], &font_sans, style.secondary, ALIGN_LEFT);
        ui_label(ui, row, vals[i], &font_sans, style.text, ALIGN_RIGHT);
        if (i && ui->c)
            gfx_fill(ui->c, (struct rect){x + 16, row.y, w - 32, 1}, style.separator);
    }
    y += info.h + 18;
    if (ui_button(ui, (struct rect){x, y, 130, 28}, "Open in Files"))
        tool_launch("Files", v->path);
    if (v->removable && ui_button(ui, (struct rect){x + 140, y, 100, 28}, "Eject")) {
        gui_eject(v->path);
        scan(d);
    }
}

/* ---- disks and partitions ---- */

#define MAX_DISKS 8

static void le16w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32w(uint8_t *p, uint32_t v)
{
    le16w(p, v);
    le16w(p + 2, v >> 16);
}

/* A FAT32 file system over the whole device: 32 reserved sectors (boot sector, FSInfo, backups at 6 and 7), two
   FATs and the root folder in cluster 2. */
static int format_fat32(struct blockdev *dev, const char *label)
{
    uint32_t n = dev->sectors, spc = n <= 532480 ? 1 : n <= 16777216 ? 8 : n <= 67108864 ? 16 : 32;
    uint32_t reserved = 32, per = (256 * spc + 2) / 2;
    uint32_t fat = (n - reserved + per - 1) / per, clusters = (n - reserved - 2 * fat) / spc;
    if (clusters < 65525)
        return E_RANGE; /* too small for FAT32 */
    uint8_t *buf = kzalloc(64 * BLOCK_SIZE);
    if (!buf)
        return E_NOMEM;
    int err = 0;
    for (uint32_t s = 0; s < reserved && !err; s += 32) /* the reserved area */
        err = block_write(dev, s, 32, buf);
    for (uint32_t s = 0; s < 2 * fat && !err; s += 64) /* both FATs */
        err = block_write(dev, reserved + s, MIN(64u, 2 * fat - s), buf);
    if (!err) /* the root folder */
        err = block_write(dev, reserved + 2 * fat, spc, buf);
    uint8_t *b = buf;
    static const uint8_t jump[3] = {0xeb, 0x58, 0x90};
    memcpy(b, jump, 3);
    memcpy(b + 3, "SYNTHOS ", 8);
    le16w(b + 11, BLOCK_SIZE);
    b[13] = (uint8_t)spc;
    le16w(b + 14, reserved);
    b[16] = 2;
    b[21] = 0xf8;
    le16w(b + 24, 63);
    le16w(b + 26, 255);
    le32w(b + 28, dev->offset);
    le32w(b + 32, n);
    le32w(b + 36, fat);
    le32w(b + 44, 2);
    le16w(b + 48, 1);
    le16w(b + 50, 6);
    b[64] = 0x80;
    b[66] = 0x29;
    le32w(b + 67, (uint32_t)hal_ticks_ms() * 2654435761u);
    char name[12];
    ksnprintf(name, sizeof(name), "%-11s", label && *label ? label : "NO NAME");
    for (int i = 0; i < 11; i++)
        b[71 + i] = (uint8_t)to_upper((uint8_t)name[i]);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xaa;
    uint8_t *info = buf + BLOCK_SIZE;
    le32w(info, 0x41615252);
    le32w(info + 484, 0x61417272);
    le32w(info + 488, clusters - 1);
    le32w(info + 492, 3);
    le32w(info + 508, 0xaa550000);
    if (!err)
        err = block_write(dev, 0, 2, buf);
    if (!err)
        err = block_write(dev, 6, 2, buf);
    memset(buf, 0, 2 * BLOCK_SIZE);
    le32w(buf, 0x0ffffff8);
    le32w(buf + 4, 0x0fffffff);
    le32w(buf + 8, 0x0fffffff); /* the root folder's one cluster */
    for (int f = 0; f < 2 && !err; f++)
        err = block_write(dev, reserved + (uint32_t)f * fat, 1, buf);
    memset(buf, 0, BLOCK_SIZE);
    for (int i = 0; i < 11; i++)
        buf[i] = (uint8_t)to_upper((uint8_t)name[i]);
    buf[11] = 0x08; /* the volume label */
    if (!err)
        err = block_write(dev, reserved + 2 * fat, 1, buf);
    kfree(buf);
    return err;
}

/* takes away the mounts of a disk and of its partitions */
static void release(struct blockdev *disk)
{
    for (struct mount *m = vfs_mounts(); m;) {
        struct mount *next = m->next;
        struct blockdev *dev = block_find(m->source);
        if (dev && (dev == disk || dev->parent == disk))
            vfs_eject(m->path);
        m = next;
    }
}

/* after the table changed: the partitions are made again from it and their volumes mounted */
static void rescan(struct blockdev *disk)
{
    release(disk);
    for (bool again = true; again;) {
        again = false;
        for (struct blockdev *d = block_list(); d; d = d->next)
            if (d->parent == disk) {
                block_unregister(d);
                kfree(d);
                again = true;
                break;
            }
    }
    struct disk_map *map = kmalloc(sizeof(*map));
    if (!map || disk_map_read(disk, map) != 0) {
        kfree(map);
        return;
    }
    for (int i = 0; i < map->count; i++) {
        const struct part *p = &map->parts[i];
        if (p->kind == PART_FREE || p->slot < 0)
            continue;
        char name[16];
        ksnprintf(name, sizeof(name), "%sp%d", disk->name, p->slot + 1);
        struct blockdev *dev = block_add_partition(disk, name, p->start, p->count); /* FAT mounts by itself */
        if (dev && sfs_detect(dev)) {
            char path[PATH_MAX_LEN];
            ksnprintf(path, sizeof(path), "/mnt/%s", name);
            vfs_mkdirs(path);
            vfs_mount("sfs", name, path);
        }
    }
    kfree(map);
}

static struct blockdev *part_device(struct blockdev *disk, const struct part *p)
{
    for (struct blockdev *d = block_list(); d; d = d->next)
        if (d->parent == disk && d->offset == p->start)
            return d;
    return p->slot < 0 && p->kind != PART_FREE ? disk : NULL;
}

/* formats a partition (or a disk without a table): fat32 or synthos */
static int format_device(struct blockdev *disk, struct blockdev *dev, const char *kind, const char *label)
{
    if (disk == hal_boot_disk())
        return E_BUSY;
    release(disk);
    int err = !strcmp(kind, "fat32") ? format_fat32(dev, label) : !strcmp(kind, "synthos") ? sfs_format(dev) : E_INVAL;
    rescan(disk);
    if (dev == disk && !err) {
        char path[PATH_MAX_LEN];
        ksnprintf(path, sizeof(path), "/mnt/%s", disk->name);
        vfs_mkdirs(path);
        vfs_mount(!strcmp(kind, "fat32") ? "fat" : "sfs", disk->name, path);
    }
    return err;
}

struct disks {
    struct blockdev *list[MAX_DISKS];
    struct disk_map *map;
    int count, disk, part;
    char status[128];
};

static struct disks dk;

static void disks_scan(void)
{
    dk.count = 0;
    for (struct blockdev *d = block_list(); d && dk.count < MAX_DISKS; d = d->next)
        if (!d->parent)
            dk.list[dk.count++] = d;
    dk.disk = CLAMP(dk.disk, 0, MAX(dk.count - 1, 0));
    if (!dk.map)
        dk.map = kmalloc(sizeof(*dk.map));
    if (dk.map && (!dk.count || disk_map_read(dk.list[dk.disk], dk.map) != 0))
        dk.map->count = 0;
    dk.part = dk.map ? CLAMP(dk.part, 0, MAX(dk.map->count - 1, 0)) : 0;
}

static uint32_t part_color(const struct part *p)
{
    static const uint32_t colors[] = {RGB(255, 159, 10), RGB(10, 132, 255), RGB(48, 209, 88), RGB(94, 92, 230),
                                      RGB(191, 90, 242), RGB(255, 69, 58), RGB(100, 210, 255), RGB(255, 214, 10)};
    if (p->kind == PART_FREE)
        return style.dark ? RGB(64, 64, 70) : RGB(222, 222, 228);
    return colors[(p->kind * 3 + (p->slot < 0 ? 0 : p->slot)) % 8];
}

static void part_name(const struct part *p, char *out, size_t cap)
{
    if (p->kind == PART_FREE)
        str_copy(out, "Free space", cap);
    else if (p->label[0])
        ksnprintf(out, cap, "%s", p->label);
    else
        ksnprintf(out, cap, "%s %d", part_kind_name(p->kind), p->slot + 1);
}

static void added(struct window *owner, const char *value)
{
    if (!value || !dk.map || dk.part >= dk.map->count)
        return;
    struct blockdev *disk = dk.list[dk.disk];
    const struct part *p = &dk.map->parts[dk.part];
    long mb = str_to_long(value, NULL, 10);
    uint32_t count = mb > 0 ? (uint32_t)MIN((uint64_t)mb * 2048, (uint64_t)p->count) : p->count;
    release(disk);
    int err = disk_map_add(dk.map, p->start, count, PART_DATA, "Data");
    rescan(disk);
    ksnprintf(dk.status, sizeof(dk.status), err ? "Could not add it: %s" : "Partition added", err_str(err));
    disks_scan();
    wm_invalidate(owner);
}

static void format_pick(struct window *owner, int index)
{
    if (index < 1 || !dk.map || dk.part >= dk.map->count)
        return;
    struct blockdev *disk = dk.list[dk.disk], *dev = part_device(disk, &dk.map->parts[dk.part]);
    int err = dev ? format_device(disk, dev, index == 1 ? "fat32" : "synthos", "UNTITLED") : E_NOENT;
    ksnprintf(dk.status, sizeof(dk.status), err == E_RANGE ? "Too small for FAT32: use SynthOS"
                                            : err ? "Could not format: %s" : "Formatted",
              err_str(err));
    disks_scan();
    wm_invalidate(owner);
}

static void delete_pick(struct window *owner, int index)
{
    if (index != 1 || !dk.map || dk.part >= dk.map->count)
        return;
    struct blockdev *disk = dk.list[dk.disk];
    release(disk);
    int err = disk_map_remove(dk.map, dk.map->parts[dk.part].slot);
    rescan(disk);
    ksnprintf(dk.status, sizeof(dk.status), err ? "Could not delete it: %s" : "Partition deleted", err_str(err));
    disks_scan();
    wm_invalidate(owner);
}

static void table_pick(struct window *owner, int index)
{
    if (index < 1 || !dk.count)
        return;
    struct blockdev *disk = dk.list[dk.disk];
    release(disk);
    int err = disk_map_init(disk, index == 1 ? SCHEME_GPT : SCHEME_MBR);
    rescan(disk);
    ksnprintf(dk.status, sizeof(dk.status), err ? "Could not erase it: %s" : "New partition table", err_str(err));
    disks_scan();
    wm_invalidate(owner);
}

static void disks_page(struct ui *ui, struct rect area)
{
    if (!dk.count || !dk.map) {
        ui_label(ui, area, "No disks", &font_sans, style.tertiary, ALIGN_CENTER);
        return;
    }
    int x = area.x + 24, w = area.w - 48, y = area.y + 14;
    for (int i = 0; i < dk.count; i++) { /* the disks as tabs */
        struct rect r = {x + i * 92, y, 86, 26};
        if (ui_button(ui, r, dk.list[i]->name) && dk.disk != i) {
            dk.disk = i;
            dk.part = 0;
            dk.status[0] = 0;
            disks_scan();
        }
        if (dk.disk == i && ui->c)
            gfx_fill(ui->c, (struct rect){r.x + 10, r.y + r.h + 2, r.w - 20, 2}, style.accent);
    }
    y += 40;
    struct blockdev *disk = dk.list[dk.disk];
    bool boot = disk == hal_boot_disk();
    char line[160], size[16];
    fmt_size(size, sizeof(size), (uint64_t)disk->sectors * BLOCK_SIZE);
    ksnprintf(line, sizeof(line), "%s  -  %s  -  %s%s%s", disk->model[0] ? disk->model : "Disk", size,
              part_scheme_name(dk.map->scheme), dk.map->read_only ? ", read-only" : "",
              boot ? ", holds the running system" : "");
    ui_label(ui, (struct rect){x, y, w, 20}, line, &font_sans, style.secondary, ALIGN_LEFT);
    y += 28;
    struct rect bar = {x, y, w, 34};
    uint64_t total = MAX(disk->sectors, 1u);
    int at = 0;
    for (int i = 0; i < dk.map->count; i++) { /* the map: every partition its share, at least a sliver */
        const struct part *p = &dk.map->parts[i];
        int pw = i == dk.map->count - 1 ? bar.w - at : MAX(6, (int)((uint64_t)bar.w * p->count / total));
        struct rect r = {bar.x + at, bar.y, MIN(pw, bar.w - at), bar.h};
        at += r.w;
        if (r.w <= 0)
            continue;
        if (ui->c) {
            gfx_round(ui->c, rect_inset(r, 1), 5, part_color(p));
            if (i == dk.part)
                gfx_round_frame(ui->c, r, 6, style.text);
        }
        if (ui_hover(ui, r) && ui->e && ui->e->type == EV_MOUSE_DOWN) {
            dk.part = i;
            dk.status[0] = 0;
        }
    }
    y += 50;
    int rows = MIN(dk.map->count, 8);
    for (int i = 0; i < rows; i++) {
        const struct part *p = &dk.map->parts[i];
        struct rect r = {x, y + i * 30, w, 30};
        if (ui->c) {
            if (i == dk.part)
                gfx_round(ui->c, r, 7, style.selection);
            gfx_round(ui->c, (struct rect){r.x + 10, r.y + 9, 12, 12}, 3, part_color(p));
        }
        char name[48], detail[64];
        part_name(p, name, sizeof(name));
        fmt_size(size, sizeof(size), (uint64_t)p->count * BLOCK_SIZE);
        struct blockdev *dev = part_device(disk, p);
        ksnprintf(detail, sizeof(detail), "%s  %s  %s", p->fs[0] ? p->fs : p->kind == PART_FREE ? "" : "unformatted",
                  dev ? dev->name : "", size);
        ui_label(ui, (struct rect){r.x + 32, r.y, r.w / 2, r.h}, name, &font_sans, ui_row_text(i == dk.part),
                 ALIGN_LEFT);
        ui_label(ui, (struct rect){r.x + r.w / 2, r.y, r.w / 2 - 12, r.h}, detail, &font_small,
                 i == dk.part ? ui_row_text(true) : style.secondary, ALIGN_RIGHT);
        if (ui_hover(ui, r) && ui->e && ui->e->type == EV_MOUSE_DOWN) {
            dk.part = i;
            dk.status[0] = 0;
        }
    }
    int by = area.y + area.h - 46;
    ui_label(ui, (struct rect){x, by - 26, w, 20}, boot ? "The disk of the running system cannot be changed here"
                                                        : dk.status,
             &font_small, style.secondary, ALIGN_LEFT);
    if (boot || dk.map->read_only || dk.part >= dk.map->count)
        return;
    const struct part *p = &dk.map->parts[dk.part];
    if (ui_button(ui, (struct rect){x, by, 150, 30}, "Erase Disk…")) {
        static const char *const b[] = {"Cancel", "GPT", "MBR"};
        ui_alert(ui->win, "Erase the whole disk?",
                 "A new, empty partition table is written. Everything on the disk is lost.", ICON_WARNING, b, 3,
                 table_pick);
    }
    if (p->kind == PART_FREE) {
        if (dk.map->scheme != SCHEME_NONE && ui_button_primary(ui, (struct rect){x + w - 160, by, 160, 30},
                                                                "Add Partition…")) {
            char all[16];
            ksnprintf(all, sizeof(all), "%u", p->count / 2048);
            ui_prompt(ui->win, "Add a Partition", "Size in MB:", all, added);
        }
        return;
    }
    if (ui_button_primary(ui, (struct rect){x + w - 120, by, 120, 30}, "Format…")) {
        static const char *const b[] = {"Cancel", "FAT32", "SynthOS"};
        ui_alert(ui->win, "Format this partition?",
                 "FAT32 works with Windows, macOS and cameras; SynthOS keeps owners, links and long files. "
                 "What is on it now is lost.",
                 ICON_WARNING, b, 3, format_pick);
    }
    if (p->slot >= 0 && ui_button(ui, (struct rect){x + w - 230, by, 100, 30}, "Delete…")) {
        static const char *const b[] = {"Cancel", "Delete"};
        ui_alert(ui->win, "Delete this partition?", "Its files are lost.", ICON_WARNING, b, 2, delete_pick);
    }
}

COMMAND(format, "mkfs", "system", "format <device> fat32|synthos [label] --yes",
        "make a new FAT32 or SynthOS file system on a partition or disk (part lists them)")
{
    bool yes = argc > 1 && !strcmp(argv[argc - 1], "--yes");
    int n = yes ? argc - 1 : argc;
    if (n < 3)
        return command_usage(sh, command_find("format"));
    struct blockdev *dev = block_find(argv[1]);
    for (struct blockdev *d = block_list(); !dev && d; d = d->next) /* a partition 'part' just made */
        if (!d->parent && !strncmp(argv[1], d->name, strlen(d->name))) {
            rescan(d);
            dev = block_find(argv[1]);
        }
    if (!dev)
        return sh_error(sh, "format", "no device %s", argv[1]);
    if (!yes)
        return sh_error(sh, "format", "everything on %s is lost; add --yes to go on", dev->name);
    struct blockdev *disk = dev->parent ? dev->parent : dev;
    int err = format_device(disk, dev, argv[2], n > 3 ? argv[3] : "");
    if (err)
        return sh_error(sh, "format", "%s: %s", dev->name,
                        err == E_BUSY ? "holds the running system" : err == E_RANGE ? "too small for FAT32" : err_str(err));
    sh_printf(sh, "%s: new %s file system\n", dev->name, argv[2]);
    return 0;
}

static void frame(struct ui *ui, void *data)
{
    struct diskutil *d = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->e && ui->e->type == EV_TIMER) {
        scan(d);
        ui->consumed = true;
    }
    struct tool_item items[MAX_VOLS + 1];
    int n = 0;
    for (int i = 0; i < d->count; i++)
        items[n++] = (struct tool_item){d->vols[i].removable ? SYM_USB : SYM_DISK, d->vols[i].label,
                                        i ? NULL : "Volumes"};
    items[n++] = (struct tool_item){SYM_CHIP, "Disks & Partitions", "Devices"};
    if (tool_sidebar(ui, H, items, n, &d->selected) && d->selected == d->count)
        disks_scan();
    bool disks = d->selected >= d->count;
    struct rect area = tool_page(ui, W, H, disks ? "Disks & Partitions" : d->vols[d->selected].label,
                                 disks ? "Partition maps, adding, deleting and formatting" : NULL);
    if (ui_symbol_button(ui, (struct rect){W - 76, 12, 28, 28}, SYM_RESTART, false)) {
        scan(d);
        if (disks)
            disks_scan();
    }
    if (disks)
        disks_page(ui, area);
    else
        volume(ui, d, area);
}

static void paint(struct window *w, struct canvas *c)
{
    struct diskutil *d = w->data;
    ui_run(w, &d->st, c, NULL, frame, d);
}

static void event(struct window *w, struct gui_event *e)
{
    struct diskutil *d = w->data;
    ui_run(w, &d->st, NULL, e, frame, d);
}

static void destroy(struct window *w)
{
    struct diskutil *d = w->data;
    if (open_win == w)
        open_win = NULL;
    kfree(d->view.text);
    kfree(d);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct diskutil *d = kzalloc(sizeof(*d));
    if (!d)
        return;
    scan(d);
    for (int i = 0; i < d->count; i++)
        if (!strcmp(d->vols[i].path, "/home"))
            d->selected = i;
    open_win = tool_open("Disk Utility", ICON_DISKUTIL, 820, 560, &ops, d);
    if (!open_win) {
        kfree(d);
        return;
    }
    wm_set_timer(open_win, 3000);
}

static const struct gui_app diskutil_app = {
    .name = "Disk Utility", .description = "Volumes, their use, disks and partitions", .icon = ICON_DISKUTIL,
    .launch = launch};
static const struct command *const commands[] = {&cmddef_format};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&diskutil_app, NULL, &ext);
