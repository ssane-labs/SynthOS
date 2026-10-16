#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* Login items: ~/.autostart lists apps, files and "!commands" that start when a user logs in to the desktop. */

static void list_path(char *out, size_t cap)
{
    path_join(user_home(), ".autostart", out, cap);
}

int autostart_items(char (*out)[AUTOSTART_LEN], int max)
{
    char path[PATH_MAX_LEN], *data;
    size_t size;
    list_path(path, sizeof(path));
    if (vfs_read_all(path, &data, &size) != 0)
        return 0;
    int n = 0;
    for (char *p = data, *end = data + size; p < end && n < max;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[AUTOSTART_LEN];
        str_copy(line, p, MIN(len + 1, sizeof(line)));
        char *t = str_trim(line);
        if (*t && *t != '#')
            str_copy(out[n++], t, AUTOSTART_LEN);
        p += len + 1;
    }
    kfree(data);
    return n;
}

static int save(char (*items)[AUTOSTART_LEN], int n)
{
    char path[PATH_MAX_LEN];
    list_path(path, sizeof(path));
    char *buf = kmalloc((size_t)n * (AUTOSTART_LEN + 1) + 1);
    if (!buf)
        return E_NOMEM;
    size_t len = 0;
    for (int i = 0; i < n; i++)
        len += (size_t)ksnprintf(buf + len, AUTOSTART_LEN + 2, "%s\n", items[i]);
    int err = vfs_write_all(path, buf, len);
    kfree(buf);
    return err;
}

int autostart_add(const char *item)
{
    static char items[AUTOSTART_MAX][AUTOSTART_LEN];
    int n = autostart_items(items, AUTOSTART_MAX);
    if (n >= AUTOSTART_MAX)
        return E_NOSPC;
    for (int i = 0; i < n; i++)
        if (!strcmp(items[i], item))
            return 0;
    str_copy(items[n++], item, AUTOSTART_LEN);
    return save(items, n);
}

int autostart_remove(int index)
{
    static char items[AUTOSTART_MAX][AUTOSTART_LEN];
    int n = autostart_items(items, AUTOSTART_MAX);
    if (index < 0 || index >= n)
        return E_INVAL;
    for (int i = index; i + 1 < n; i++)
        str_copy(items[i], items[i + 1], AUTOSTART_LEN);
    return save(items, n - 1);
}

void autostart_run(void)
{
    static char items[AUTOSTART_MAX][AUTOSTART_LEN];
    int n = autostart_items(items, AUTOSTART_MAX);
    for (int i = 0; i < n; i++) {
        const char *it = items[i];
        if (it[0] == '!') {
            gui_terminal_run(it + 1);
            continue;
        }
        const struct gui_app *app = gui_app_find(it);
        if (app && !str_casecmp(app->name, it))
            gui_app_launch(app, NULL);
        else if (vfs_exists(it))
            gui_open_file(it);
    }
}

COMMAND(autostart, NULL, "system", "autostart [list|add <app|file|!command>|remove <n>]",
        "programs that start when you log in to the desktop")
{
    static char items[AUTOSTART_MAX][AUTOSTART_LEN];
    const char *op = argc > 1 ? argv[1] : "list";
    if (!strcmp(op, "add") && argc > 2) {
        char item[AUTOSTART_LEN] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(item, " ", sizeof(item));
            str_append(item, argv[i], sizeof(item));
        }
        int err = autostart_add(item);
        return err ? sh_error(sh, "autostart", "%s", err_str(err)) : 0;
    }
    if (!strcmp(op, "remove") && argc > 2) {
        long n;
        if (!str_parse_long(argv[2], &n) || autostart_remove((int)n - 1) != 0)
            return sh_error(sh, "autostart", "no item %s", argv[2]);
        return 0;
    }
    if (strcmp(op, "list"))
        return command_usage(sh, command_find("autostart"));
    int n = autostart_items(items, AUTOSTART_MAX);
    if (!n)
        sh_puts(sh, "nothing starts at login\n");
    for (int i = 0; i < n; i++)
        sh_printf(sh, "%d  %s\n", i + 1, items[i]);
    return 0;
}
