#include <synth/block.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

static bool quiet;

static void boot_step(bool ok, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void boot_step(bool ok, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    klog("%s", msg);
    if (quiet && ok)
        return;
    con_role(ROLE_MUTED);
    kprintf(" [");
    con_role(ok ? ROLE_OK : ROLE_ERROR);
    kprintf(ok ? " OK " : "FAIL");
    con_role(ROLE_MUTED);
    kprintf("] ");
    con_reset();
    kprintf("%s\n", msg);
}

static const char *cmdline_value(const char *cmdline, const char *key, char *buf, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = cmdline; p && *p; p++) {
        if ((p == cmdline || p[-1] == ' ') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            bool rest = strcmp(key, "run") == 0;
            while (p[n] && (rest || p[n] != ' ') && n + 1 < cap)
                n++;
            memcpy(buf, p, n);
            buf[n] = 0;
            return buf;
        }
    }
    return NULL;
}

static bool cmdline_flag(const char *cmdline, const char *flag)
{
    size_t n = strlen(flag);
    for (const char *p = cmdline; p && (p = strstr(p, flag)); p += n)
        if ((p == cmdline || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
            return true;
    return false;
}

#if CONFIG_WIN32
void win32_install(void);
#endif
#if CONFIG_GUI
bool gui_autostart(void);
int gui_run(struct shell *sh);
#endif

static void mount_storage(void)
{
    struct blockdev *data = block_find("data0");
    if (!data) {
        boot_step(true, "No persistent disk: /home lives in memory");
        return;
    }
    bool fresh = !sfs_detect(data);
    if (fresh) {
        int err = sfs_format(data);
        if (err) {
            boot_step(false, "Cannot format %s: %s", data->name, err_str(err));
            return;
        }
        vfs_mkdirs("/mnt/setup");
        if (vfs_mount("sfs", data->name, "/mnt/setup") == 0) {
            vfs_copy("/home", "/mnt/setup");
            vfs_umount("/mnt/setup");
        }
        vfs_unlink("/mnt/setup");
    }
    int err = vfs_mount("sfs", data->name, "/home");
    char size[16];
    fmt_size(size, sizeof(size), (uint64_t)data->sectors * BLOCK_SIZE);
    boot_step(err == 0, "Persistent disk: %s mounted at /home%s", size, fresh ? " (freshly formatted)" : "");
}

static void mount_filesystems(const struct boot_info *bi)
{
    int err = vfs_mount("ramfs", NULL, "/");
    if (err)
        panic("cannot mount root filesystem: %s", err_str(err));
    boot_step(true, "Root filesystem: ramfs");

    if (bi->initrd && bi->initrd_size) {
        int files = ramdisk_unpack(bi->initrd, bi->initrd_size, "/");
        boot_step(files >= 0, "Ramdisk: %d files (%u KB)", files, (unsigned)(bi->initrd_size / 1024));
    }

    static const char *const dirs[] = {"/apps", "/bin", "/dev", "/dos", "/etc", "/home", "/mnt", "/system/win32", "/system/data", "/tmp"};
    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++)
        vfs_mkdirs(dirs[i]);
    err = vfs_mount("devfs", NULL, "/dev");
    boot_step(err == 0, "Devices mounted at /dev");
    hal_fs_init();
    mount_storage();
#if CONFIG_WIN32
    win32_install();
#endif
}

static void run_startup(struct shell *sh)
{
    static const char *const scripts[] = {"/etc/synthrc", "/etc/synth.conf", "/home/.synthrc", "/home/.synth.conf"};
    for (size_t i = 0; i < ARRAY_SIZE(scripts); i++)
        if (vfs_exists(scripts[i]))
            shell_source(sh, scripts[i], 0, NULL);
}

static void show_welcome(struct shell *sh)
{
    const char *banner = sh_get(sh, "BANNER");
    if (!banner || strcmp(banner, "off") != 0)
        shell_banner();
    char *motd;
    if (vfs_read_all("/etc/motd", &motd, NULL) == 0) {
        con_markup(motd);
        con_reset();
        kfree(motd);
    }
}

void kmain(const struct boot_info *bi)
{
    heap_init(bi->heap, bi->heap_size);
    struct display *d = hal_display();
    con_init(d);
    quiet = cmdline_flag(bi->cmdline, "quiet");

    if (!quiet) {
        con_role(ROLE_ACCENT);
        kprintf("%s %s", SYNTH_NAME, SYNTH_VERSION);
        con_role(ROLE_MUTED);
        kprintf(" booting on %s\n", hal_machine());
        con_reset();
    }
    char size[16];
    fmt_size(size, sizeof(size), bi->heap_size);
    boot_step(true, "Memory: %s available to the kernel heap", size);
    if (con_is_grid())
        boot_step(true, "Display: %s %dx%d, 16-color palette", d->name, d->cols, d->rows);
    else
        boot_step(true, "Display: %s (line mode)", d->name);

    vfs_init();
    mount_filesystems(bi);

    struct shell *sh = shell_create();
    if (!sh)
        panic("cannot create shell");
    boot_step(true, "Shell ready");
    run_startup(sh);

    char command[CONFIG_LINE_MAX];
    if (cmdline_value(bi->cmdline, "run", command, sizeof(command))) {
        int rc = shell_exec(sh, command);
        con_flush();
        UNUSED(rc);
        hal_poweroff();
    }

    if (con_is_grid())
        con_clear();
    show_welcome(sh);
#if CONFIG_GUI
    if (gui_autostart() && con_is_grid())
        gui_run(sh);
#endif
    shell_repl(sh);

    con_role(ROLE_MUTED);
    kprintf("System halted.\n");
    con_reset();
    con_flush();
    hal_poweroff();
}
