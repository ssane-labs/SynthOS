#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/lnk.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "internal.h"

/* Programs installed the Windows way: whatever an installer puts in the Start Menu (the user's or everyone's)
   becomes an app in Launchpad and Spotlight, with the program's own icon, and the windows of that program group
   under it. Nothing here knows any particular program. */

#define MAX_INSTALLED 96
#define RESCAN_MS 2000

struct installed {
    struct gui_app app;
    char name[64];
    char lnk[PATH_MAX_LEN];
    char target[PATH_MAX_LEN];
    char icon[PATH_MAX_LEN];
    struct canvas picture;
    bool seen;
};

static struct installed *list[MAX_INSTALLED];

/* opening goes through the shortcut (gui_app_launch asks installed_shortcut first) */
static void no_launch(const char *arg)
{
    UNUSED(arg);
}
static int count;
static uint64_t scanned_at;
static bool scanned;

static bool skip_name(const char *name)
{
    static const char *const words[] = {"uninstall", "uninst", "remove ", "readme", "help", "license", "manual",
                                        "release notes", "website", "on the web"};
    for (size_t i = 0; i < ARRAY_SIZE(words); i++)
        for (const char *p = name; *p; p++)
            if (!str_ncasecmp(p, words[i], strlen(words[i])))
                return true;
    return false;
}

static struct installed *find_named(const char *name)
{
    for (int i = 0; i < count; i++)
        if (!str_casecmp(list[i]->name, name))
            return list[i];
    return NULL;
}

static void consider(const char *file)
{
    const char *base = path_basename(file);
    const char *ext = path_ext(base);
    if (!ext || str_casecmp(ext, ".lnk"))
        return;
    char name[64];
    str_copy(name, base, MIN(sizeof(name), (size_t)(ext - base) + 1));
    if (!name[0] || skip_name(name))
        return;
    struct installed *have = find_named(name);
    if (have && !strcmp(have->lnk, file)) {
        have->seen = true;
        return;
    }
    if (have)
        return;
    struct lnk *l = kmalloc(sizeof(*l));
    char target[PATH_MAX_LEN], icon[PATH_MAX_LEN] = "";
    bool ok = l && lnk_read(file, l) == 0 && winpath_to_vfs("/", l->target, target, sizeof(target)) == 0;
    ok = ok && path_ext(target) && !str_casecmp(path_ext(target), ".exe") && vfs_exists(target);
    if (ok && l->icon[0] && winpath_to_vfs("/", l->icon, icon, sizeof(icon)) != 0)
        icon[0] = 0;
    kfree(l);
    /* a program that already has a built-in app (the bundled Chromium, Chrome) keeps that one */
    if (!ok || count >= MAX_INSTALLED || gui_builtin_app_for_program(target))
        return;
    struct installed *in = kzalloc(sizeof(*in));
    if (!in)
        return;
    str_copy(in->name, name, sizeof(in->name));
    str_copy(in->lnk, file, sizeof(in->lnk));
    str_copy(in->target, target, sizeof(in->target));
    str_copy(in->icon, icon[0] && vfs_exists(icon) ? icon : target, sizeof(in->icon));
    in->app = (struct gui_app){in->name, "Windows program", ICON_APP, no_launch, NULL, NULL};
    if (gui_program_icon(in->icon, 64, &in->picture) || gui_program_icon(target, 64, &in->picture))
        in->app.picture = &in->picture;
    in->seen = true;
    list[count++] = in;
}

static void scan(const char *dir, int depth)
{
    struct vdir d;
    if (depth > 2 || vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (ent.name[0] == '.')
            continue;
        char path[PATH_MAX_LEN];
        if (path_join(dir, ent.name, path, sizeof(path)) != 0)
            continue;
        if (ent.type == VN_DIR)
            scan(path, depth + 1);
        else
            consider(path);
    }
    vfs_closedir(&d);
}

/* entries go when their shortcut does; a window of the program keeps its app until it closes */
static void rescan(void)
{
    for (int i = 0; i < count; i++)
        list[i]->seen = false;
    char win[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    ksnprintf(win, sizeof(win), "C:\\Users\\%s\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs", gui_user());
    if (winpath_to_vfs("/", win, dir, sizeof(dir)) == 0)
        scan(dir, 0);
    if (winpath_to_vfs("/", "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs", dir, sizeof(dir)) == 0)
        scan(dir, 0);
    for (int i = 0; i < count;) {
        if (list[i]->seen || wm_app_window_count(&list[i]->app) > 0) {
            i++;
            continue;
        }
        kfree(list[i]->picture.px);
        kfree(list[i]);
        list[i] = list[--count];
    }
}

void installed_refresh(void)
{
    uint64_t now = hal_ticks_ms();
    if (scanned && now - scanned_at < RESCAN_MS)
        return;
    scanned = true;
    scanned_at = now;
    rescan();
}

void installed_foreach(void (*fn)(const struct gui_app *app, void *ctx), void *ctx)
{
    installed_refresh();
    for (int i = 0; i < count; i++)
        fn(&list[i]->app, ctx);
}

const struct gui_app *installed_find(const char *name)
{
    installed_refresh();
    struct installed *in = find_named(name);
    return in ? &in->app : NULL;
}

const char *installed_shortcut(const struct gui_app *app)
{
    for (int i = 0; app && i < count; i++)
        if (&list[i]->app == app)
            return list[i]->lnk;
    return NULL;
}

const struct gui_app *installed_for_program(const char *path)
{
    installed_refresh();
    for (int i = 0; i < count; i++)
        if (!str_casecmp(list[i]->target, path))
            return &list[i]->app;
    return NULL;
}
