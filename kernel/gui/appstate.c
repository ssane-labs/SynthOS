#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/shell.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* What apps remember between runs and reboots (a sidebar left open, list or icon view, the last tab):
   "app.key=value" lines in .appstate in the user's home, written the moment something changes. */

#define STATE_MAX 128
#define KEY_LEN 40
#define VALUE_LEN 64

struct state_entry {
    char key[KEY_LEN];
    char value[VALUE_LEN];
};

static struct state_entry *table;
static int count;
static char loaded_from[PATH_MAX_LEN];

static void state_path(char *buf, size_t cap)
{
    const char *home = user_home();
    ksnprintf(buf, cap, "%s/.appstate", home && home[0] ? home : "/home");
}

static void state_load(void)
{
    char path[PATH_MAX_LEN];
    state_path(path, sizeof(path));
    if (table && !strcmp(path, loaded_from))
        return;
    if (!table && !(table = kzalloc(sizeof(*table) * STATE_MAX)))
        return;
    count = 0;
    str_copy(loaded_from, path, sizeof(loaded_from));
    char *data;
    size_t size;
    if (vfs_read_all(path, &data, &size) < 0)
        return;
    for (char *line = data, *end = data + size; line < end && count < STATE_MAX;) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        char *stop = nl ? nl : end;
        char *eq = memchr(line, '=', (size_t)(stop - line));
        if (eq && eq > line && eq - line < KEY_LEN && stop - eq - 1 < VALUE_LEN) {
            struct state_entry *e = &table[count++];
            memcpy(e->key, line, (size_t)(eq - line));
            e->key[eq - line] = 0;
            memcpy(e->value, eq + 1, (size_t)(stop - eq - 1));
            e->value[stop - eq - 1] = 0;
        }
        line = stop + 1;
    }
    kfree(data);
}

static void state_save(void)
{
    int err;
    struct stream *s = stream_file(loaded_from, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &err);
    if (!s)
        return;
    for (int i = 0; i < count; i++)
        stream_printf(s, "%s=%s\n", table[i].key, table[i].value);
    stream_close(s);
}

static struct state_entry *state_find(const char *app, const char *key)
{
    char full[KEY_LEN];
    ksnprintf(full, sizeof(full), "%s.%s", app, key);
    state_load();
    for (int i = 0; table && i < count; i++)
        if (!strcmp(table[i].key, full))
            return &table[i];
    return NULL;
}

const char *app_state(const char *app, const char *key, const char *fallback)
{
    struct state_entry *e = state_find(app, key);
    return e ? e->value : fallback;
}

long app_state_number(const char *app, const char *key, long fallback)
{
    long v;
    const char *s = app_state(app, key, NULL);
    return s && str_parse_long(s, &v) ? v : fallback;
}

bool app_state_on(const char *app, const char *key, bool fallback)
{
    const char *s = app_state(app, key, NULL);
    return s ? !strcmp(s, "on") : fallback;
}

void app_state_set(const char *app, const char *key, const char *value)
{
    struct state_entry *e = state_find(app, key);
    if (e && !strcmp(e->value, value))
        return;
    if (!e) {
        if (!table || count >= STATE_MAX)
            return;
        e = &table[count++];
        ksnprintf(e->key, sizeof(e->key), "%s.%s", app, key);
    }
    str_copy(e->value, value, sizeof(e->value));
    state_save();
}

void app_state_set_number(const char *app, const char *key, long value)
{
    char buf[24];
    ksnprintf(buf, sizeof(buf), "%ld", value);
    app_state_set(app, key, buf);
}

void app_state_set_on(const char *app, const char *key, bool on)
{
    app_state_set(app, key, on ? "on" : "off");
}

COMMAND(appstate, NULL, "system", "appstate [<app> <key> [value]|clear]",
        "what apps remember between runs: sidebars, views, tabs")
{
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        state_load();
        count = 0;
        state_save();
        return 0;
    }
    if (argc == 3 || argc == 4) {
        if (argc == 4)
            app_state_set(argv[1], argv[2], argv[3]);
        const char *v = app_state(argv[1], argv[2], NULL);
        if (!v) {
            sh_printf(sh, "%s.%s is not set\n", argv[1], argv[2]);
            return E_NOENT;
        }
        sh_printf(sh, "%s\n", v);
        return 0;
    }
    if (argc != 1) {
        sh_puts(sh, "usage: appstate [<app> <key> [value]|clear]\n");
        return E_INVAL;
    }
    state_load();
    for (int i = 0; table && i < count; i++)
        sh_printf(sh, "%-28s %s\n", table[i].key, table[i].value);
    return 0;
}
