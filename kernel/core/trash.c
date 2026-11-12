#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/trash.h>
#include <synth/vfs.h>

/* The Trash keeps where each item came from and when it was thrown away in /home/.Trash/.info/<name>
   ("time<TAB>path"): Put Back returns it there, and what is older than trashdays (30) goes for good. */

#define INFO_DIR TRASH_DIR "/.info"

static int days = 30;

static void info_path(const char *name, char *out, size_t cap)
{
    ksnprintf(out, cap, INFO_DIR "/%s", name);
}

static bool read_info(const char *name, uint32_t *when, char *where, size_t cap)
{
    char path[PATH_MAX_LEN], *data;
    size_t size;
    info_path(name, path, sizeof(path));
    if (vfs_read_all(path, &data, &size))
        return false;
    char *tab = memchr(data, '\t', size);
    bool ok = tab != NULL;
    if (ok) {
        *tab = 0;
        *when = (uint32_t)str_to_long(data, NULL, 10);
        size_t n = MIN((size_t)(data + size - tab - 1), cap - 1);
        memcpy(where, tab + 1, n);
        where[n] = 0;
        char *nl = strchr(where, '\n');
        if (nl)
            *nl = 0;
    }
    kfree(data);
    return ok;
}

bool trash_origin(const char *name, char *where, size_t cap)
{
    uint32_t when;
    return read_info(name, &when, where, cap);
}

int trash_put(const char *path, char *out, size_t cap)
{
    char full[PATH_MAX_LEN], to[PATH_MAX_LEN], name[NAME_MAX_LEN];
    int err = path_resolve("/", path, full, sizeof(full));
    if (err)
        return err;
    if (str_starts(full, TRASH_DIR "/") || !strcmp(full, TRASH_DIR))
        return E_INVAL;
    vfs_mkdirs(INFO_DIR);
    str_copy(name, path_basename(full), sizeof(name));
    path_join(TRASH_DIR, name, to, sizeof(to));
    for (int i = 2; vfs_exists(to) && i < 1000; i++) {
        char *dot = strrchr(path_basename(full), '.');
        size_t stem = dot && dot != path_basename(full) ? (size_t)(dot - path_basename(full)) : strlen(path_basename(full));
        ksnprintf(name, sizeof(name), "%.*s %d%s", (int)stem, path_basename(full), i, dot && stem ? dot : "");
        path_join(TRASH_DIR, name, to, sizeof(to));
    }
    if ((err = vfs_move(full, to)) != 0)
        return err;
    char info[PATH_MAX_LEN + 16], ipath[PATH_MAX_LEN];
    int n = ksnprintf(info, sizeof(info), "%u\t%s\n", time_now(), full);
    info_path(name, ipath, sizeof(ipath));
    vfs_write_all(ipath, info, (size_t)n);
    if (out)
        str_copy(out, name, cap);
    return 0;
}

int trash_restore(const char *name, char *where, size_t cap)
{
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN], ipath[PATH_MAX_LEN];
    path_join(TRASH_DIR, name, from, sizeof(from));
    if (!vfs_exists(from))
        return E_NOENT;
    uint32_t when;
    if (!read_info(name, &when, to, sizeof(to)))
        path_join("/home", name, to, sizeof(to));
    if (vfs_exists(to))
        return E_EXIST;
    char dir[PATH_MAX_LEN];
    str_copy(dir, to, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        vfs_mkdirs(dir);
    }
    int err = vfs_move(from, to);
    if (err)
        return err;
    info_path(name, ipath, sizeof(ipath));
    vfs_unlink(ipath);
    if (where)
        str_copy(where, to, cap);
    return 0;
}

int trash_clean(void)
{
    if (days <= 0)
        return 0;
    uint32_t now = time_now(), limit = (uint32_t)days * 86400u;
    int removed = 0;
    for (bool again = true; again;) {
        again = false;
        struct vdir d;
        if (vfs_opendir(TRASH_DIR, &d))
            return removed;
        struct vfs_dirent e;
        char victim[NAME_MAX_LEN] = "";
        while (!victim[0] && vfs_readdir(&d, &e) == 0) {
            uint32_t when;
            char where[PATH_MAX_LEN];
            if (e.name[0] != '.' && read_info(e.name, &when, where, sizeof(where)) && now > when &&
                now - when > limit)
                str_copy(victim, e.name, sizeof(victim));
        }
        vfs_closedir(&d);
        if (victim[0]) {
            char path[PATH_MAX_LEN];
            path_join(TRASH_DIR, victim, path, sizeof(path));
            vfs_remove_tree(path);
            info_path(victim, path, sizeof(path));
            vfs_unlink(path);
            removed++;
            again = true;
        }
    }
    return removed;
}

static void get_days(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", days);
}

static int set_days(struct shell *sh, const char *value)
{
    UNUSED(sh);
    const char *end;
    long v = str_to_long(value, &end, 10);
    if (*end || v < 0 || v > 3650)
        return E_INVAL;
    days = (int)v;
    return 0;
}

static const struct setting s_trashdays = {"trashdays", "<days>|0",
                                           "empty what has been in the Trash this long (0: never)", get_days,
                                           set_days};
SETTING(s_trashdays);

COMMAND(trash, "rm-safe", "files", "trash [<file>...|list|restore <name>|empty|clean]",
        "move files to the Trash, list it, put items back")
{
    if (argc < 2 || !strcmp(argv[1], "list")) {
        struct vdir d;
        int n = 0;
        if (vfs_opendir(TRASH_DIR, &d) == 0) {
            struct vfs_dirent e;
            while (vfs_readdir(&d, &e) == 0) {
                if (e.name[0] == '.')
                    continue;
                uint32_t when = 0;
                char where[PATH_MAX_LEN] = "?", date[32] = "";
                read_info(e.name, &when, where, sizeof(where));
                if (when)
                    time_format(when, date, sizeof(date));
                sh_printf(sh, "%-24s %-20s %s\n", e.name, date, where);
                n++;
            }
            vfs_closedir(&d);
        }
        if (!n)
            sh_puts(sh, "the Trash is empty\n");
        return 0;
    }
    if (!strcmp(argv[1], "restore") && argc == 3) {
        char where[PATH_MAX_LEN];
        int err = trash_restore(argv[2], where, sizeof(where));
        if (err)
            return sh_error(sh, "trash", "%s: %s", argv[2], err_str(err));
        sh_printf(sh, "put back: %s\n", where);
        return 0;
    }
    if (!strcmp(argv[1], "empty") && argc == 2) {
        vfs_remove_tree(TRASH_DIR);
        vfs_mkdirs(TRASH_DIR);
        sh_puts(sh, "the Trash is empty\n");
        return 0;
    }
    if (!strcmp(argv[1], "clean") && argc == 2) {
        sh_printf(sh, "removed %d old item(s)\n", trash_clean());
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        char name[NAME_MAX_LEN], full[PATH_MAX_LEN];
        int err = sh_resolve(sh, argv[i], full, sizeof(full));
        if (!err)
            err = trash_put(full, name, sizeof(name));
        if (err)
            status = sh_error(sh, "trash", "%s: %s", argv[i], err_str(err));
    }
    return status;
}
