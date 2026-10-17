#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

static const struct gui_look defaults[] = {
    {"dune", "Dune", "Warm desert glass, places on the left, launcher in the corner", false, RGB(214, 150, 96),
     RGB(212, 116, 46), DOCK_CORNER, STATUS_PILL, true, 1u << WIDGET_CLOCK | 1u << WIDGET_WEATHER | 1u << WIDGET_MUSIC},
    {"mist", "Mist", "Quiet morning fog, menu bar and a floating dock", false, RGB(120, 140, 170), RGB(78, 112, 214),
     DOCK_CENTER, STATUS_MENUBAR, false, 1u << WIDGET_CALENDAR | 1u << WIDGET_TASKS},
    {"night", "Night", "Deep dark sky, an app rail and music on the desktop", true, RGB(110, 92, 200),
     RGB(150, 128, 255), DOCK_RAIL, STATUS_PILL, false,
     1u << WIDGET_CLOCK | 1u << WIDGET_MUSIC | 1u << WIDGET_VISUALIZER},
};

#define LOOKS ARRAY_SIZE(defaults)

/* The user's copy of every look: the dock style, the status bar and the places panel can be changed per look and
   are kept in ~/.looks ("dune dock=corner status=pill places=on"). */
static struct gui_look looks[LOOKS];
static bool loaded;
static int current = 0;

static const char *const dock_names[] = {"bottom", "corner", "side"};
static const char *const status_names[] = {"menubar", "pill"};

static void looks_path(char *out, size_t cap)
{
    path_join(user_home(), ".looks", out, cap);
}

static int name_index(const char *const *names, int n, const char *value, size_t len)
{
    for (int i = 0; i < n; i++)
        if (strlen(names[i]) == len && !strncmp(names[i], value, len))
            return i;
    return -1;
}

static void apply_pair(struct gui_look *l, const char *key, size_t klen, const char *value, size_t vlen)
{
    int i;
    if (klen == 4 && !strncmp(key, "dock", 4) && (i = name_index(dock_names, 3, value, vlen)) >= 0)
        l->dock = (enum dock_kind)i;
    else if (klen == 6 && !strncmp(key, "status", 6) && (i = name_index(status_names, 2, value, vlen)) >= 0)
        l->status = (enum status_kind)i;
    else if (klen == 6 && !strncmp(key, "places", 6))
        l->places = vlen == 2 && !strncmp(value, "on", 2);
}

static void looks_load(void)
{
    loaded = true;
    memcpy(looks, defaults, sizeof(looks));
    char path[PATH_MAX_LEN], *data;
    size_t size;
    looks_path(path, sizeof(path));
    if (vfs_read_all(path, &data, &size) != 0)
        return;
    char *line = data, *end = data + size;
    while (line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        char *stop = nl ? nl : end;
        struct gui_look *l = NULL;
        for (char *p = line; p < stop;) {
            char *sp = memchr(p, ' ', (size_t)(stop - p));
            char *word_end = sp ? sp : stop;
            char *eq = memchr(p, '=', (size_t)(word_end - p));
            if (!l && !eq) {
                for (size_t i = 0; i < LOOKS; i++)
                    if (strlen(looks[i].name) == (size_t)(word_end - p) && !strncmp(looks[i].name, p, (size_t)(word_end - p)))
                        l = &looks[i];
            } else if (l && eq) {
                apply_pair(l, p, (size_t)(eq - p), eq + 1, (size_t)(word_end - eq - 1));
            }
            p = word_end + 1;
        }
        line = stop + 1;
    }
    kfree(data);
}

static void looks_save(void)
{
    char path[PATH_MAX_LEN], buf[LOOKS * 80];
    size_t n = 0;
    for (size_t i = 0; i < LOOKS; i++)
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "%s dock=%s status=%s places=%s\n", looks[i].name,
                               dock_names[looks[i].dock], status_names[looks[i].status], looks[i].places ? "on" : "off");
    looks_path(path, sizeof(path));
    vfs_write_all(path, buf, n);
}

void look_user_changed(void)
{
    loaded = false;
}

const struct gui_look *look_current(void)
{
    if (!loaded)
        looks_load();
    return &looks[current];
}

int look_count(void)
{
    return (int)LOOKS;
}

void look_reset(void)
{
    looks[current] = defaults[current];
    looks_save();
    gui_look_changed();
}

const struct gui_look *look_get(int index)
{
    if (!loaded)
        looks_load();
    return index >= 0 && index < look_count() ? &looks[index] : NULL;
}

int menubar_height(void)
{
    return look_current()->status == STATUS_MENUBAR ? MENUBAR_HEIGHT : 0;
}

static void get_look(char *buf, size_t cap)
{
    str_copy(buf, look_current()->name, cap);
}

static int set_look(struct shell *sh, const char *value)
{
    UNUSED(sh);
    for (int i = 0; i < look_count(); i++)
        if (!str_casecmp(value, look_get(i)->name)) {
            if (i != current) {
                current = i;
                gui_look_changed();
            }
            return 0;
        }
    return E_INVAL;
}

static const struct setting s_look = {"look", "dune|mist|night", "desktop design: colors, wallpaper and layout",
                                      get_look, set_look};
SETTING(s_look);

static void get_part(char *buf, size_t cap, int which)
{
    const struct gui_look *l = look_current();
    str_copy(buf, which == 0 ? dock_names[l->dock] : which == 1 ? status_names[l->status] : l->places ? "on" : "off",
             cap);
}

static int set_part(const char *value, int which)
{
    look_current();
    struct gui_look *l = &looks[current];
    int i = which == 0 ? name_index(dock_names, 3, value, strlen(value))
            : which == 1 ? name_index(status_names, 2, value, strlen(value))
                         : name_index((const char *const[]){"off", "on"}, 2, value, strlen(value));
    if (i < 0)
        return E_INVAL;
    if (which == 0)
        l->dock = (enum dock_kind)i;
    else if (which == 1)
        l->status = (enum status_kind)i;
    else
        l->places = i;
    looks_save();
    gui_look_changed();
    return 0;
}

static void get_dock(char *buf, size_t cap)
{
    get_part(buf, cap, 0);
}

static int set_dock(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return set_part(value, 0);
}

static void get_status(char *buf, size_t cap)
{
    get_part(buf, cap, 1);
}

static int set_status(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return set_part(value, 1);
}

static void get_places(char *buf, size_t cap)
{
    get_part(buf, cap, 2);
}

static int set_places(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return set_part(value, 2);
}

static const struct setting s_dock = {"dockstyle", "bottom|corner|side", "the dock of the current look", get_dock,
                                      set_dock};
static const struct setting s_status = {"statusbar", "menubar|pill", "menu bar or floating status pill (current look)",
                                        get_status, set_status};
static const struct setting s_places = {"places", "on|off", "the places panel on the left (current look)", get_places,
                                        set_places};
SETTING(s_dock);
SETTING(s_status);
SETTING(s_places);
