#include <synth/command.h>
#include <synth/error.h>
#include <synth/heap.h>
#include <synth/lnk.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

static void print_rule(const struct winpath_rule *rule, void *ctx)
{
    struct shell *sh = ctx;
    sh_role(sh, ROLE_ACCENT2);
    sh_printf(sh, "  %-26s", rule->windows);
    sh_role(sh, ROLE_MUTED);
    sh_puts(sh, " \x1a ");
    sh_role(sh, ROLE_DIR);
    sh_printf(sh, "%s\n", rule->synth);
    sh_reset(sh);
}

COMMAND(drives, "winpaths", "files", "drives", "show how Windows and DOS paths map onto SynthOS folders")
{
    UNUSED(argc);
    UNUSED(argv);
    sh_printf(sh, "Windows view of the SynthOS file system (names are case-insensitive):\n");
    winpath_rules(print_rule, sh);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "  other letters D: .. Y:  \x1a /mnt/<letter>      Z:  \x1a /\n");
    sh_reset(sh);
    return 0;
}

COMMAND(winpath, NULL, "files", "winpath path...", "convert between Windows paths and SynthOS paths")
{
    if (argc < 2)
        return command_usage(sh, command_find("winpath"));
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char out[PATH_MAX_LEN];
        const char *arg = argv[i];
        bool windows = (is_alpha((uint8_t)arg[0]) && arg[1] == ':') || strchr(arg, '\\');
        if (windows) {
            int err = winpath_to_vfs(sh->cwd, arg, out, sizeof(out));
            if (err) {
                rc = sh_fs_error(sh, "winpath", arg, err);
                continue;
            }
        } else {
            char abs[PATH_MAX_LEN];
            int err = sh_resolve(sh, arg, abs, sizeof(abs));
            if (err) {
                rc = sh_fs_error(sh, "winpath", arg, err);
                continue;
            }
            winpath_from_vfs(abs, out, sizeof(out));
        }
        sh_printf(sh, "%s\n", out);
    }
    return rc;
}

static int show_shortcut(struct shell *sh, const char *file)
{
    char path[PATH_MAX_LEN];
    struct lnk *l = kmalloc(sizeof(*l));
    int err = l ? sh_resolve(sh, file, path, sizeof(path)) : E_NOMEM;
    if (!err)
        err = lnk_read(path, l);
    if (!err) {
        sh_printf(sh, "target: %s\n", l->target);
        if (l->args[0])
            sh_printf(sh, "arguments: %s\n", l->args);
        if (l->dir[0])
            sh_printf(sh, "start in: %s\n", l->dir);
        if (l->desc[0])
            sh_printf(sh, "comment: %s\n", l->desc);
    }
    kfree(l);
    return err ? sh_fs_error(sh, "shortcut", file, err) : 0;
}

COMMAND(shortcut, NULL, "files", "shortcut program [name] [arguments...] | shortcut -l file.lnk",
        "put a Windows shortcut (.lnk) to a program on the desktop, or show where one points")
{
    if (argc == 3 && !strcmp(argv[1], "-l"))
        return show_shortcut(sh, argv[2]);
    if (argc < 2)
        return command_usage(sh, command_find("shortcut"));
    char target[PATH_MAX_LEN], dir[PATH_MAX_LEN], file[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[1], target, sizeof(target));
    if (err || !vfs_exists(target))
        return sh_fs_error(sh, "shortcut", argv[1], err ? err : E_NOENT);
    struct lnk *l = kzalloc(sizeof(*l));
    uint8_t *buf = kmalloc(8192);
    if (!l || !buf) {
        kfree(l);
        kfree(buf);
        return sh_error(sh, "shortcut", "out of memory");
    }
    winpath_from_vfs(target, l->target, sizeof(l->target));
    path_dirname(target, dir, sizeof(dir));
    winpath_from_vfs(dir, l->dir, sizeof(l->dir));
    for (int i = 3; i < argc; i++) {
        if (i > 3)
            str_append(l->args, " ", sizeof(l->args));
        str_append(l->args, argv[i], sizeof(l->args));
    }
    char name[NAME_MAX_LEN];
    str_copy(name, argc > 2 ? argv[2] : path_basename(target), sizeof(name));
    char *dot = strrchr(name, '.');
    if (argc <= 2 && dot && dot != name)
        *dot = 0;
    str_append(name, ".lnk", sizeof(name));
    path_join(user_home(), "Desktop", dir, sizeof(dir));
    vfs_mkdirs(dir);
    path_join(dir, name, file, sizeof(file));
    size_t n = lnk_encode(l, buf, 8192);
    err = n ? vfs_write_all(file, buf, n) : E_INVAL;
    kfree(l);
    kfree(buf);
    if (err)
        return sh_fs_error(sh, "shortcut", file, err);
    sh_printf(sh, "Created %s\n", file);
    return 0;
}
