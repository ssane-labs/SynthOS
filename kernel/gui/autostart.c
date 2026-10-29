#include <synth/command.h>
#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/lnk.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>
#include <synth/winshell.h>

#include "internal.h"

/* Login items are what Windows has: what lies in the Startup folders (the user's and everyone's) and, for Windows
   programs, the Run and RunOnce values installers write. An app is kept as "Name.app", a file as a shortcut, a
   command as "name.command" holding the command line. */

#define USER_STARTUP "C:\\Users\\%s\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"
#define COMMON_STARTUP "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"

enum { ITEM_FILE, ITEM_RUN };

struct item {
    char label[AUTOSTART_LEN];
    char path[PATH_MAX_LEN];
    int kind;
#if CONFIG_WIN32
    struct winshell_run run;
#endif
};

static struct item items[AUTOSTART_MAX];
static int item_count;

static void startup_win(bool common, char *out, size_t cap)
{
    if (common)
        str_copy(out, COMMON_STARTUP, cap);
    else
        ksnprintf(out, cap, USER_STARTUP, user_current()->name);
}

static bool startup_dir(bool common, char *out, size_t cap)
{
    char win[PATH_MAX_LEN];
    startup_win(common, win, sizeof(win));
    return winpath_to_vfs("/", win, out, cap) == 0;
}

static char *read_text(const char *path)
{
    char *data;
    size_t size;
    if (vfs_read_all(path, &data, &size) != 0)
        return NULL;
    char *text = kmalloc(size + 1);
    if (text) {
        memcpy(text, data, size);
        text[size] = 0;
    }
    kfree(data);
    return text;
}

static void label_of(const char *path, char *out, size_t cap)
{
    const char *ext = path_ext(path);
    str_copy(out, path, cap);
    if (ext && (!str_casecmp(ext, ".app") || !str_casecmp(ext, ".command"))) {
        char *text = read_text(path);
        if (text) {
            if (!str_casecmp(ext, ".command"))
                ksnprintf(out, cap, "!%s", str_trim(text));
            else
                str_copy(out, str_trim(text), cap);
            kfree(text);
        }
    } else if (ext && !str_casecmp(ext, ".lnk")) {
        struct lnk *l = kmalloc(sizeof(*l));
        char vfs[PATH_MAX_LEN];
        if (l && lnk_read(path, l) == 0)
            str_copy(out, winpath_to_vfs("/", l->target, vfs, sizeof(vfs)) == 0 ? vfs : l->target, cap);
        kfree(l);
    }
}

static void scan_folder(bool common)
{
    char dir[PATH_MAX_LEN];
    struct vdir d;
    if (!startup_dir(common, dir, sizeof(dir)) || vfs_opendir(dir, &d) != 0)
        return;
    int first = item_count;
    struct vfs_dirent ent;
    while (item_count < AUTOSTART_MAX && vfs_readdir(&d, &ent) == 0) {
        if (ent.name[0] == '.' || !str_casecmp(ent.name, "desktop.ini"))
            continue;
        struct item *it = &items[item_count++];
        it->kind = ITEM_FILE;
        path_join(dir, ent.name, it->path, sizeof(it->path));
    }
    vfs_closedir(&d);
    for (int i = first + 1; i < item_count; i++)
        for (int j = i; j > first && strcmp(items[j - 1].path, items[j].path) > 0; j--) {
            struct item t = items[j - 1];
            items[j - 1] = items[j];
            items[j] = t;
        }
    for (int i = first; i < item_count; i++)
        label_of(items[i].path, items[i].label, sizeof(items[i].label));
}

#if CONFIG_WIN32
static void add_run(const struct winshell_run *r, void *ctx)
{
    UNUSED(ctx);
    if (item_count >= AUTOSTART_MAX)
        return;
    struct item *it = &items[item_count++];
    it->kind = ITEM_RUN;
    it->run = *r;
    it->path[0] = 0;
    ksnprintf(it->label, sizeof(it->label), "%s", r->name);
}
#endif

static int create(const char *item);

/* the list older versions kept in ~/.autostart moves into the Startup folder */
static void migrate(void)
{
    char path[PATH_MAX_LEN];
    path_join(user_home(), ".autostart", path, sizeof(path));
    char *text = read_text(path);
    if (!text)
        return;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *t = str_trim(line);
        if (*t && *t != '#')
            create(t);
    }
    kfree(text);
    vfs_unlink(path);
}

static void scan(void)
{
    migrate();
    item_count = 0;
    scan_folder(false);
    scan_folder(true);
#if CONFIG_WIN32
    winshell_run_entries(add_run, NULL);
#endif
}

int autostart_items(char (*out)[AUTOSTART_LEN], int max)
{
    scan();
    int n = MIN(item_count, max);
    for (int i = 0; i < n; i++)
        str_copy(out[i], items[i].label, AUTOSTART_LEN);
    return n;
}

static int write_unique(const char *dir, const char *base, const char *ext, const void *data, size_t size)
{
    char name[NAME_MAX_LEN], path[PATH_MAX_LEN];
    for (int i = 1; i < 100; i++) {
        if (i == 1)
            ksnprintf(name, sizeof(name), "%s%s", base, ext);
        else
            ksnprintf(name, sizeof(name), "%s %d%s", base, i, ext);
        path_join(dir, name, path, sizeof(path));
        if (!vfs_exists(path))
            return vfs_write_all(path, data, size);
    }
    return E_EXIST;
}

static int create(const char *item)
{
    char dir[PATH_MAX_LEN];
    if (!startup_dir(false, dir, sizeof(dir)))
        return E_NOENT;
    vfs_mkdirs(dir);
    if (item[0] == '!') {
        const char *cmd = item + 1;
        while (*cmd == ' ')
            cmd++;
        char base[48];
        size_t n = 0;
        for (const char *p = cmd; *p && *p != ' ' && n < sizeof(base) - 1; p++)
            base[n++] = *p == '/' || *p == '\\' || *p == ':' ? '_' : *p;
        base[n] = 0;
        return write_unique(dir, n ? base : "command", ".command", cmd, strlen(cmd));
    }
    const struct gui_app *app = gui_app_find(item);
    if (app && !str_casecmp(app->name, item))
        return write_unique(dir, app->name, ".app", app->name, strlen(app->name));
    if (!vfs_exists(item))
        return E_NOENT;
    struct lnk *l = kzalloc(sizeof(*l));
    uint8_t *blob = kmalloc(4096);
    int err = E_NOMEM;
    if (l && blob) {
        winpath_from_vfs(item, l->target, sizeof(l->target));
        l->show = 1;
        size_t size = lnk_encode(l, blob, 4096);
        char base[NAME_MAX_LEN];
        str_copy(base, path_basename(item), sizeof(base));
        char *dot = strrchr(base, '.');
        if (dot && dot != base)
            *dot = 0;
        err = size ? write_unique(dir, base, ".lnk", blob, size) : E_INVAL;
    }
    kfree(l);
    kfree(blob);
    return err;
}

int autostart_add(const char *item)
{
    scan();
    if (item_count >= AUTOSTART_MAX)
        return E_NOSPC;
    for (int i = 0; i < item_count; i++)
        if (!strcmp(items[i].label, item))
            return 0;
    return create(item);
}

int autostart_remove(int index)
{
    scan();
    if (index < 0 || index >= item_count)
        return E_INVAL;
#if CONFIG_WIN32
    if (items[index].kind == ITEM_RUN)
        return winshell_run_remove(&items[index].run) ? 0 : E_IO;
#endif
    return vfs_unlink(items[index].path);
}

void autostart_run(void)
{
    scan();
    for (int i = 0; i < item_count; i++) {
        const struct item *it = &items[i];
#if CONFIG_WIN32
        if (it->kind == ITEM_RUN) {
            gui_start_command(it->run.command);
            if (it->run.once)
                winshell_run_remove(&it->run);
            continue;
        }
#endif
        const char *ext = path_ext(it->path);
        if (ext && !str_casecmp(ext, ".command"))
            gui_terminal_run(it->label + 1);
        else
            gui_open_file(it->path);
    }
}

COMMAND(autostart, NULL, "system", "autostart [list|folder|add <app|file|!command>|remove <n>]",
        "programs that start when you log in: the Startup folder and the Run key")
{
    static char list[AUTOSTART_MAX][AUTOSTART_LEN];
    const char *op = argc > 1 ? argv[1] : "list";
    if (!strcmp(op, "add") && argc > 2) {
        char item[AUTOSTART_LEN] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(item, " ", sizeof(item));
            str_append(item, argv[i], sizeof(item));
        }
        if (item[0] != '!' && !gui_app_find(item)) {
            char path[PATH_MAX_LEN];
            if (sh_resolve(sh, item, path, sizeof(path)) == 0)
                str_copy(item, path, sizeof(item));
        }
        int err = autostart_add(item);
        return err ? sh_error(sh, "autostart", "%s: %s", item, err_str(err)) : 0;
    }
    if (!strcmp(op, "remove") && argc > 2) {
        long n;
        if (!str_parse_long(argv[2], &n) || autostart_remove((int)n - 1) != 0)
            return sh_error(sh, "autostart", "no item %s", argv[2]);
        return 0;
    }
    if (!strcmp(op, "folder")) {
        char win[PATH_MAX_LEN];
        startup_win(false, win, sizeof(win));
        sh_printf(sh, "%s\n", win);
        startup_win(true, win, sizeof(win));
        sh_printf(sh, "%s\n", win);
        return 0;
    }
    if (strcmp(op, "list"))
        return command_usage(sh, command_find("autostart"));
    int n = autostart_items(list, AUTOSTART_MAX);
    if (!n)
        sh_puts(sh, "nothing starts at login\n");
    for (int i = 0; i < n; i++)
        sh_printf(sh, "%d  %s\n", i + 1, list[i]);
    return 0;
}
