#include <synth/block.h>
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/parttab.h>
#include <synth/sha256.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/update.h>
#include <synth/vfs.h>

/* System updates without a reinstall. A package (tools/mkupdate.py) carries the boot sectors and the files of the
   EFI partition. The boot code of sector 0 is replaced but its install record and partition table stay; stage2 and
   the kernel with its ramdisk go to the sectors after it; the EFI files go to the EFI partition of this copy (inside
   the system area, or the one right before the SynthOS partition), whose SYSTEM.CFG stays. The new system starts
   with the next restart; /home and the settings are not touched. */

#define HEADER 64
#define ENTRY 64
#define MAX_ENTRIES 32
#define KERNEL_LBA 17
#define STAGE2_SECTORS 16
#define ESP_LBA 8192
#define ESP_SECTORS 24576
#define ESP_MOUNT "/mnt/update-esp"
#define RECORD 416
#define DEFAULT_URL "https://github.com/ssane-labs/synthos/releases/latest/download/"

static char update_url[HTTP_URL_MAX] = DEFAULT_URL;

static void get_url(char *buf, size_t cap)
{
    str_copy(buf, update_url, cap);
}

static int set_url(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!strcmp(value, "default"))
        value = DEFAULT_URL;
    else if (strncmp(value, "http://", 7) && strncmp(value, "https://", 8))
        return E_INVAL;
    str_copy(update_url, value, sizeof(update_url));
    return 0;
}

static const struct setting s_update_url = {"updateurl", "<url>|default", "where system updates come from (a folder URL)",
                                            get_url, set_url};
SETTING(s_update_url);

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *arch_suffix(void)
{
    return sizeof(void *) == 8 ? "" : "32";
}

static int arch_bits(void)
{
    return sizeof(void *) == 8 ? 64 : 32;
}

int update_parse(const uint8_t *data, size_t size, struct update_pkg *pkg, char *why, size_t cap)
{
    memset(pkg, 0, sizeof(*pkg));
    if (size < HEADER + 32 || memcmp(data, "SYNUPD01", 8)) {
        str_copy(why, "not a SynthOS update", cap);
        return E_NOEXEC;
    }
    uint8_t digest[32];
    sha256(data, size - 32, digest);
    if (memcmp(digest, data + size - 32, 32)) {
        str_copy(why, "the package is damaged (checksum mismatch)", cap);
        return E_IO;
    }
    pkg->arch = (int)rd32(data + 8);
    pkg->count = (int)rd32(data + 12);
    memcpy(pkg->build, data + 16, sizeof(pkg->build) - 1);
    if (pkg->count <= 0 || pkg->count > MAX_ENTRIES || HEADER + (size_t)pkg->count * ENTRY > size - 32) {
        str_copy(why, "the package is damaged", cap);
        return E_IO;
    }
    size_t at = HEADER + (size_t)pkg->count * ENTRY;
    for (int i = 0; i < pkg->count; i++) {
        const uint8_t *e = data + HEADER + i * ENTRY;
        struct update_file *f = &pkg->files[i];
        memcpy(f->name, e, 55);
        f->name[55] = 0;
        f->size = rd32(e + 56);
        uint32_t same = rd32(e + 60);
        if (same) {
            if (same > (uint32_t)i || pkg->files[same - 1].size != f->size) {
                str_copy(why, "the package is damaged", cap);
                return E_IO;
            }
            f->data = pkg->files[same - 1].data;
            continue;
        }
        if (f->size > size - 32 - at) {
            str_copy(why, "the package is damaged", cap);
            return E_IO;
        }
        f->data = data + at;
        at += f->size;
    }
    if (pkg->arch != arch_bits()) {
        ksnprintf(why, cap, "this update is for the %d-bit system, this one is %d-bit", pkg->arch, arch_bits());
        return E_INVAL;
    }
    const struct update_file *k = update_file(pkg, "boot/kernel");
    const struct update_file *s2 = update_file(pkg, "boot/stage2");
    const struct update_file *s1 = update_file(pkg, "boot/stage1");
    if (!k || !s1 || !s2 || s1->size != 512 || s2->size > STAGE2_SECTORS * 512 ||
        KERNEL_LBA + (k->size + 511) / 512 > ESP_LBA || k->size < 512 || memcmp(k->data + 8, "SYNK", 4)) {
        str_copy(why, "the package has no usable boot files", cap);
        return E_NOEXEC;
    }
    return 0;
}

const struct update_file *update_file(const struct update_pkg *pkg, const char *name)
{
    for (int i = 0; i < pkg->count; i++)
        if (!strcmp(pkg->files[i].name, name))
            return &pkg->files[i];
    return NULL;
}

/* The SHA-256 of the kernel now on the system disk, `size` bytes from LBA 17 (the size of the newer one). */
int update_installed_kernel(uint32_t size, uint8_t out[32])
{
    struct blockdev *sys = hal_system_area();
    if (!sys)
        return E_NODEV;
    uint8_t *buf = kmalloc(64 * 512);
    if (!buf)
        return E_NOMEM;
    struct sha256 h;
    sha256_init(&h);
    int err = 0;
    for (uint32_t done = 0; done < size && !err;) {
        uint32_t n = MIN(64u * 512, (size - done + 511) & ~511u);
        err = block_read(sys, KERNEL_LBA + done / 512, n / 512, buf);
        uint32_t take = MIN(n, size - done);
        sha256_update(&h, buf, take);
        done += take;
    }
    kfree(buf);
    if (!err)
        sha256_final(&h, out);
    return err;
}

static uint32_t abs_start(struct blockdev *dev, struct blockdev **root)
{
    uint32_t at = 0;
    while (dev->parent) {
        at += dev->offset;
        dev = dev->parent;
    }
    *root = dev;
    return at;
}

/* Where this partition is already mounted (by automount), if it is. */
static const char *mounted_at(struct blockdev *root, uint32_t start)
{
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        struct blockdev *dev = block_find(m->source), *r;
        if (dev && abs_start(dev, &r) == start && r == root)
            return m->path;
    }
    return NULL;
}

static bool exists(const char *root, const char *rel)
{
    char path[PATH_MAX_LEN];
    struct vstat st;
    ksnprintf(path, sizeof(path), "%s/%s", root, rel);
    return vfs_stat(path, &st) == 0;
}

/* SYSTEM.CFG names the system this EFI partition starts: it must be this one. */
static bool starts_us(const char *root, uint32_t system_lba)
{
    char path[PATH_MAX_LEN], *text;
    size_t size;
    ksnprintf(path, sizeof(path), "%s/SYNTHOS/SYSTEM.CFG", root);
    if (vfs_read_all(path, &text, &size) < 0)
        return false;
    bool match = false;
    for (const char *line = text; line && line < text + size;) {
        if (!strncmp(line, "system=", 7)) {
            long v;
            char num[16];
            size_t n = 0;
            while (n < sizeof(num) - 1 && line[7 + n] >= '0' && line[7 + n] <= '9')
                num[n] = line[7 + n], n++;
            num[n] = 0;
            match = str_parse_long(num, &v) && (uint32_t)v == system_lba;
        }
        line = memchr(line, '\n', (size_t)(text + size - line));
        if (line)
            line++;
    }
    kfree(text);
    return match;
}

static int put(const char *root, const char *rel, const struct update_file *f)
{
    char file[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    ksnprintf(file, sizeof(file), "%s/%s", root, rel);
    str_copy(dir, file, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        vfs_mkdirs(dir);
    }
    return vfs_write_all(file, f->data, f->size);
}

/* The EFI files go where this kind of installation keeps them. Its own EFI partition (the stick, a whole-disk or a
   partition install) has them in \EFI\BOOT; next to Windows they are in \EFI\SynthOS, and \EFI\BOOT there belongs
   to Windows. */
static int write_files(const char *root, const struct update_pkg *pkg)
{
    bool windows = exists(root, "EFI/SynthOS");
    const struct update_file *shim = update_file(pkg, "esp/EFI/BOOT/BOOTX64.EFI");
    const struct update_file *loader = update_file(pkg, "esp/EFI/BOOT/grubx64.efi");
    bool has_shim = loader != NULL;
    if (!has_shim)
        loader = shim;
    int err = 0;
    for (int i = 0; i < pkg->count && !err; i++) {
        const struct update_file *f = &pkg->files[i];
        if (strncmp(f->name, "esp/", 4))
            continue;
        const char *rel = f->name + 4;
        if (!strncmp(rel, "EFI/BOOT/", 9)) {
            if (!windows)
                err = put(root, rel, f);
            continue;
        }
        err = put(root, rel, f);
    }
    if (err || !windows)
        return err;
    if (exists(root, "EFI/SynthOS/synthos.efi"))
        err = put(root, "EFI/SynthOS/synthos.efi", loader);
    if (!err && has_shim && exists(root, "EFI/SynthOS/grubx64.efi")) {
        err = put(root, "EFI/SynthOS/grubx64.efi", loader);
        if (!err)
            err = put(root, "EFI/SynthOS/shimx64.efi", shim);
        const struct update_file *mm = update_file(pkg, "esp/EFI/BOOT/mmx64.efi");
        if (!err && mm)
            err = put(root, "EFI/SynthOS/mmx64.efi", mm);
    }
    return err;
}

/* Updates the FAT volume at [start, start + count) of `root` if it is an EFI partition of this system. */
static int try_esp(const struct update_pkg *pkg, struct blockdev *root, uint32_t start, uint32_t count,
                   uint32_t system_lba, bool inside, int *written)
{
    const char *path = mounted_at(root, start);
    struct blockdev *dev = NULL;
    char where[PATH_MAX_LEN];
    if (!path) {
        if (start + count > root->sectors || !(dev = block_add_partition(root, "update-esp", start, count)))
            return 0;
        if (!fat_detect(dev)) {
            block_unregister(dev);
            return 0;
        }
        if (!(path = mounted_at(root, start))) {
            vfs_mkdirs(ESP_MOUNT);
            if (vfs_mount("fat", dev->name, ESP_MOUNT)) {
                block_unregister(dev);
                return 0;
            }
            path = ESP_MOUNT;
        }
    }
    str_copy(where, path, sizeof(where));
    path = where;
    int err = 0;
    bool ours = inside ? exists(path, "SYNTHOS/KERNEL.BIN") && (!exists(path, "SYNTHOS/SYSTEM.CFG") ||
                                                                 starts_us(path, system_lba))
                       : starts_us(path, system_lba);
    if (ours) {
        err = write_files(path, pkg);
        if (!err)
            (*written)++;
    }
    if (dev) {
        int e2 = vfs_umount(path);
        if (!err)
            err = e2;
        if (strcmp(path, ESP_MOUNT))
            vfs_unlink(path);
        block_unregister(dev);
    }
    return err;
}

static int write_esps(const struct update_pkg *pkg, struct blockdev *sys, int *written)
{
    struct blockdev *root;
    uint32_t base = abs_start(sys, &root);
    int err = try_esp(pkg, root, base + ESP_LBA, ESP_SECTORS, base, true, written);
    if (err || !sys->parent)
        return err;
    struct disk_map *map = kmalloc(sizeof(*map));
    if (!map)
        return E_NOMEM;
    if (disk_map_read(root, map) == 0)
        for (int i = 0; i < map->count && !err; i++)
            if (map->parts[i].kind == PART_EFI || !strncmp(map->parts[i].fs, "FAT", 3))
                err = try_esp(pkg, root, map->parts[i].start, map->parts[i].count, base, false, written);
    kfree(map);
    return err;
}

static int write_boot(struct blockdev *sys, const struct update_pkg *pkg)
{
    const struct update_file *s1 = update_file(pkg, "boot/stage1"), *s2 = update_file(pkg, "boot/stage2"),
                             *k = update_file(pkg, "boot/kernel");
    uint32_t sectors = (k->size + 511) / 512;
    uint8_t *buf = kzalloc((size_t)(STAGE2_SECTORS + sectors) * 512);
    if (!buf)
        return E_NOMEM;
    memcpy(buf, s2->data, s2->size);
    memcpy(buf + STAGE2_SECTORS * 512, k->data, k->size);
    int err = block_write(sys, 1, STAGE2_SECTORS + sectors, buf);
    kfree(buf);
    if (err)
        return err;
    uint8_t sector[512];
    if ((err = block_read(sys, 0, 1, sector)) != 0)
        return err;
    memcpy(sector, s1->data, RECORD);
    return block_write(sys, 0, 1, sector);
}

int update_apply(const struct update_pkg *pkg, char *msg, size_t cap)
{
    struct blockdev *sys = hal_system_area();
    if (!sys) {
        str_copy(msg, "SynthOS did not start from a disk it can update", cap);
        return E_NODEV;
    }
    int esps = 0;
    int err = write_esps(pkg, sys, &esps);
    if (err) {
        ksnprintf(msg, cap, "writing the EFI partition failed: %s", err_str(err));
        return err;
    }
    if ((err = write_boot(sys, pkg)) != 0) {
        ksnprintf(msg, cap, "writing the boot sectors failed: %s", err_str(err));
        return err;
    }
    ksnprintf(msg, cap, "Updated to %s (boot sectors%s). Restart to use it.", pkg->build,
              esps ? ", EFI partition" : "");
    return 0;
}

static bool manifest_value(const char *text, const char *key, char *out, size_t cap)
{
    size_t n = strlen(key);
    for (const char *line = text; line && *line;) {
        if (!strncmp(line, key, n) && line[n] == '=') {
            const char *v = line + n + 1;
            size_t len = 0;
            while (v[len] && v[len] != '\n' && v[len] != '\r')
                len++;
            str_copy(out, v, MIN(cap, len + 1));
            return true;
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return false;
}

static void hex(const uint8_t *d, char *out)
{
    for (int i = 0; i < 32; i++)
        ksnprintf(out + i * 2, 3, "%02x", d[i]);
}

int update_check(struct update_status *st, void (*progress)(void *, const char *, size_t, size_t), void *ctx)
{
    memset(st, 0, sizeof(*st));
    char url[HTTP_URL_MAX];
    ksnprintf(url, sizeof(url), "%s%ssynthos-update%s.txt", update_url,
              update_url[strlen(update_url) - 1] == '/' ? "" : "/", arch_suffix());
    struct http_request req = {.method = "GET", .url = url, .max_size = 4096, .progress = progress, .ctx = ctx};
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    if (err) {
        ksnprintf(st->message, sizeof(st->message), "Could not reach the update server: %s", err_str(err));
        return err;
    }
    if (resp.status != 200 || !resp.body) {
        ksnprintf(st->message, sizeof(st->message), "No update information (HTTP %d)", resp.status);
        http_free(&resp);
        return E_NOENT;
    }
    char size[16], want[80];
    bool ok = manifest_value(resp.body, "build", st->build, sizeof(st->build)) &&
              manifest_value(resp.body, "kernel-size", size, sizeof(size)) &&
              manifest_value(resp.body, "kernel-sha256", want, sizeof(want)) &&
              manifest_value(resp.body, "sha256", st->sha256, sizeof(st->sha256));
    char total[16];
    if (manifest_value(resp.body, "size", total, sizeof(total))) {
        long v;
        st->size = str_parse_long(total, &v) ? (size_t)v : 0;
    }
    http_free(&resp);
    long ksize;
    if (!ok || !str_parse_long(size, &ksize) || ksize <= 0) {
        str_copy(st->message, "The update information is not readable", sizeof(st->message));
        return E_INVAL;
    }
    uint8_t now[32];
    char now_hex[65];
    if ((err = update_installed_kernel((uint32_t)ksize, now)) != 0) {
        str_copy(st->message, "SynthOS did not start from a disk it can update", sizeof(st->message));
        return err;
    }
    hex(now, now_hex);
    st->available = strcmp(now_hex, want) != 0;
    ksnprintf(st->message, sizeof(st->message), st->available ? "SynthOS %s is available" : "SynthOS is up to date (%s)",
              st->build);
    return 0;
}

int update_download(const struct update_status *st, uint8_t **data, size_t *size,
                    void (*progress)(void *, const char *, size_t, size_t), void *ctx, char *msg, size_t cap)
{
    char url[HTTP_URL_MAX];
    ksnprintf(url, sizeof(url), "%s%ssynthos-update%s.sup", update_url,
              update_url[strlen(update_url) - 1] == '/' ? "" : "/", arch_suffix());
    struct http_request req = {.method = "GET", .url = url, .max_size = 64u << 20, .progress = progress, .ctx = ctx};
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    if (err) {
        ksnprintf(msg, cap, "Download failed: %s", err_str(err));
        return err;
    }
    if (resp.status != 200 || !resp.body) {
        ksnprintf(msg, cap, "Download failed (HTTP %d)", resp.status);
        http_free(&resp);
        return E_NOENT;
    }
    if (st && st->sha256[0]) {
        uint8_t d[32];
        char got[65];
        sha256(resp.body, resp.length, d);
        hex(d, got);
        if (strcmp(got, st->sha256)) {
            str_copy(msg, "The download does not match the update information", cap);
            http_free(&resp);
            return E_IO;
        }
    }
    *data = (uint8_t *)resp.body;
    *size = resp.length;
    resp.body = NULL;
    http_free(&resp);
    return 0;
}

int update_install(const uint8_t *data, size_t size, char *msg, size_t cap)
{
    struct update_pkg *pkg = kmalloc(sizeof(*pkg));
    if (!pkg) {
        str_copy(msg, "Not enough memory for the update", cap);
        return E_NOMEM;
    }
    int err = update_parse(data, size, pkg, msg, cap);
    if (!err)
        err = update_apply(pkg, msg, cap);
    kfree(pkg);
    return err;
}

static int apply_bytes(struct shell *sh, uint8_t *data, size_t size, bool dry)
{
    struct update_pkg *pkg = kmalloc(sizeof(*pkg));
    char msg[160];
    if (!pkg)
        return E_NOMEM;
    int err = update_parse(data, size, pkg, msg, sizeof(msg));
    if (!err) {
        sh_printf(sh, "SynthOS update %s for %d-bit: %d files, checksum verified\n", pkg->build, pkg->arch, pkg->count);
        if (!dry) {
            err = update_apply(pkg, msg, sizeof(msg));
            sh_printf(sh, "%s\n", msg);
        }
    } else {
        sh_error(sh, "update", "%s", msg);
    }
    kfree(pkg);
    return err;
}

COMMAND(update, "softwareupdate", "system", "update [check | install | verify <file.sup> | apply <file.sup>]",
        "update SynthOS without a reinstall: from the update server or a package file")
{
    const char *op = argc > 1 ? argv[1] : "check";
    if ((!strcmp(op, "verify") || !strcmp(op, "apply")) && argc == 3) {
        char *data;
        size_t size;
        int err = vfs_read_all(argv[2], &data, &size);
        if (err)
            return sh_error(sh, "update", "%s: %s", argv[2], err_str(err));
        err = apply_bytes(sh, (uint8_t *)data, size, !strcmp(op, "verify"));
        kfree(data);
        return err;
    }
    if (!strcmp(op, "check") || !strcmp(op, "install")) {
        struct update_status st;
        int err = update_check(&st, NULL, NULL);
        sh_printf(sh, "%s\n", st.message);
        if (err || !st.available || strcmp(op, "install"))
            return err;
        uint8_t *data;
        size_t size;
        char msg[160];
        sh_printf(sh, "Downloading %u KB\n", (unsigned)(st.size / 1024));
        if ((err = update_download(&st, &data, &size, NULL, NULL, msg, sizeof(msg))) != 0)
            return sh_error(sh, "update", "%s", msg);
        err = apply_bytes(sh, data, size, false);
        kfree(data);
        return err;
    }
    return command_usage(sh, command_find("update"));
}
