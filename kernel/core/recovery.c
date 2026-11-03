#include <synth/block.h>
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/parttab.h>
#include <synth/recovery.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

/* The UEFI loader counts starts in \SYNTHOS\BOOT.TRY ("tries=N nonce=XXXXXXXX") and picks safe mode after two that
   did not get far. Once the desktop or the shell is up, the counter goes back to 0 here; the nonce tells which EFI
   partition this start came from. */
#define TRY_LEN 23

static bool safe;
static bool confirmed;

bool safe_mode(void)
{
    return safe;
}

void recovery_init(const char *cmdline)
{
    safe = hal_safe_mode();
    for (const char *p = cmdline; p && (p = strstr(p, "safe")); p += 4)
        if ((p == cmdline || p[-1] == ' ') && (p[4] == 0 || p[4] == ' '))
            safe = true;
    if (safe)
        klog("recovery: safe mode (basic drivers, one processor, no autostart)");
}

/* Looks at \SYNTHOS\BOOT.TRY on every EFI partition: the one with this start's nonce is the loader's own. The
   tries value found goes to *found; tries >= 0 is written back in place (the file keeps its cluster, so a power cut
   cannot leave the FAT half changed). */
static int visit(const char *root, uint32_t nonce, int tries, int *found)
{
    char path[PATH_MAX_LEN], text[TRY_LEN + 1], want[9];
    ksnprintf(want, sizeof(want), "%08x", nonce);
    ksnprintf(path, sizeof(path), "%s/SYNTHOS/BOOT.TRY", root);
    struct file *f;
    if (vfs_open(path, VFS_READ, &f))
        return E_NOENT;
    long n = vfs_read(f, text, TRY_LEN);
    vfs_close(f);
    if (n != TRY_LEN || memcmp(text, "tries=", 6) || memcmp(text + 14, want, 8))
        return E_NOENT;
    *found = text[6] >= '0' && text[6] <= '9' ? text[6] - '0' : 0;
    if (tries < 0)
        return 0;
    ksnprintf(text, sizeof(text), "tries=%d nonce=%s\n", MIN(tries, 9), want);
    int err = vfs_open(path, VFS_WRITE, &f);
    if (err)
        return err;
    n = vfs_write(f, text, TRY_LEN);
    vfs_close(f);
    return n == TRY_LEN ? 0 : n < 0 ? (int)n : E_IO;
}

static const char *mounted_at(struct blockdev *dev)
{
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        if (!strcmp(m->source, dev->name))
            return m->path;
    return NULL;
}

static int try_counter(int tries, int *found)
{
    uint32_t nonce = hal_boot_nonce();
    if (!nonce)
        return E_NOENT;
    static struct disk_map map;
    int err = E_NOENT;
    for (struct blockdev *disk = block_list(); disk && err == E_NOENT; disk = disk->next) {
        if (disk->parent || disk_map_read(disk, &map))
            continue;
        for (int i = 0; i < map.count && err == E_NOENT; i++) {
            if (map.parts[i].kind != PART_EFI)
                continue;
            struct blockdev *esp = block_add_partition(disk, "recesp", map.parts[i].start, map.parts[i].count);
            if (!esp)
                continue;
            const char *root = mounted_at(esp);
            bool own = false;
            if (!root && vfs_mkdirs("/mnt/recesp") == 0 && vfs_mount("fat", esp->name, "/mnt/recesp") == 0) {
                root = "/mnt/recesp";
                own = true;
            }
            if (root)
                err = visit(root, nonce, tries, found);
            if (own) {
                vfs_umount("/mnt/recesp");
                vfs_unlink("/mnt/recesp");
            }
            block_unregister(esp);
        }
    }
    return err;
}

static int write_tries(int tries)
{
    int found;
    return try_counter(tries, &found);
}

void recovery_boot_ok(void)
{
    if (confirmed)
        return;
    confirmed = true;
    int err = write_tries(0);
    if (err && err != E_NOENT)
        klog("recovery: cannot reset the start counter: %s", err_str(err));
}

COMMAND(recovery, "safemode", "system", "recovery [safe|normal]",
        "safe mode state; 'safe' makes the next start use safe mode, 'normal' a normal one")
{
    if (argc > 1) {
        int tries = !strcmp(argv[1], "safe") ? 2 : !strcmp(argv[1], "normal") ? 0 : -1;
        if (tries < 0) {
            sh_printf(sh, "usage: recovery [safe|normal]\n");
            return 1;
        }
        int err = write_tries(tries);
        if (err) {
            sh_printf(sh, "recovery: %s\n", err == E_NOENT ? "this start did not come from the UEFI loader" : err_str(err));
            return 1;
        }
        sh_printf(sh, "The next start is %s.\n", tries ? "in safe mode" : "a normal one");
        return 0;
    }
    int now = 0;
    sh_printf(sh, "Safe mode: %s\n", safe ? "on (basic drivers, one processor, no autostart)" : "off");
    if (try_counter(-1, &now) == 0)
        sh_printf(sh, "Start counter: %d (0 once a start gets to the desktop or the shell)\n", now);
    else
        sh_printf(sh, "Start counter: none (BIOS start)\n");
    return 0;
}

static volatile uintptr_t nowhere = 0x10;

static void bad_access(void *ctx)
{
    UNUSED(ctx);
    *(volatile uint32_t *)nowhere = 1;
}

COMMAND(faulttest, NULL, "system", "faulttest", "check that a crash in desktop app code closes only that app")
{
    UNUSED(argc);
    UNUSED(argv);
    int vector = hal_guard(bad_access, sh);
    sh_printf(sh, vector ? "contained: CPU exception %d, the system goes on\n" : "no fault\n", vector);
    return vector ? 0 : 1;
}
