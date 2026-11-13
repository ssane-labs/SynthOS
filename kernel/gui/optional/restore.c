#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "systool.h"

/* System Restore: a restore point keeps a copy of what makes up the system's state — the settings, /etc, the
   Windows registry, the Dock, widgets and login items — and the list of installed apps, in /home/.restore/<n>.
   Going back to a point puts those files back and removes the apps installed after it (a point is made first, so
   the step can be undone). "Reset This PC" removes all of it and keeps the documents. */

#define POINTS_DIR "/home/.restore"
#define APPS_DIR "/home/apps"
#define MAX_POINTS 32
#define KEEP_POINTS 12

static const char *const tracked[] = {"/etc",         "/home/.synth.conf",  "/home/.synthrc",
                                      "/home/.dock",  "/home/.widgets",     "/home/.autostart",
                                      "/home/.looks", "/home/.rules",       "/home/.desktop-layout",
                                      "/home/.paths", "/home/.tasks",       "/home/programdata/synthos/registry"};

struct point {
    int id;
    uint32_t time;
    char name[64];
    int apps;
};

static void point_dir(int id, char *out, size_t cap)
{
    ksnprintf(out, cap, POINTS_DIR "/%d", id);
}

/* the copy of a tracked path inside a point: <point>/files/etc, <point>/files/home/.dock, ... */
static void point_copy(int id, const char *path, char *out, size_t cap)
{
    ksnprintf(out, cap, POINTS_DIR "/%d/files%s", id, path);
}

static int read_point(int id, struct point *p)
{
    char path[PATH_MAX_LEN], *data;
    size_t len;
    ksnprintf(path, sizeof(path), POINTS_DIR "/%d/info", id);
    if (vfs_read_all(path, &data, &len) != 0)
        return E_NOENT;
    *p = (struct point){id, 0, "", 0};
    for (char *line = data; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!strncmp(line, "time ", 5))
            p->time = (uint32_t)str_to_long(line + 5, NULL, 10);
        else if (!strncmp(line, "name ", 5))
            str_copy(p->name, line + 5, sizeof(p->name));
        else if (!strncmp(line, "app ", 4))
            p->apps++;
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    return 0;
}

/* the points, newest first */
static int list_points(struct point *out, int max)
{
    struct vdir d;
    int n = 0;
    if (vfs_opendir(POINTS_DIR, &d) != 0)
        return 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0 && n < max) {
        long id;
        if (str_parse_long(ent.name, &id) && read_point((int)id, &out[n]) == 0)
            n++;
    }
    vfs_closedir(&d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j].id > out[j - 1].id; j--) {
            struct point t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

static bool point_has_app(const char *info, const char *app)
{
    char want[80];
    ksnprintf(want, sizeof(want), "\napp %s\n", app);
    return strstr(info, want) != NULL;
}

static int create_point(const char *name, int *id_out)
{
    struct point *all = kmalloc(sizeof(struct point) * MAX_POINTS);
    if (!all)
        return E_NOMEM;
    int n = list_points(all, MAX_POINTS), id = n ? all[0].id + 1 : 1;
    for (int i = KEEP_POINTS - 1; i < n; i++) { /* the oldest go */
        char dir[PATH_MAX_LEN];
        point_dir(all[i].id, dir, sizeof(dir));
        vfs_remove_tree(dir);
    }
    kfree(all);
    char dir[PATH_MAX_LEN], path[PATH_MAX_LEN];
    point_dir(id, dir, sizeof(dir));
    int err = vfs_mkdirs(dir);
    if (err)
        return err;
    size_t cap = 4096, len = 0;
    char *info = kmalloc(cap);
    if (!info)
        return E_NOMEM;
    len += (size_t)ksnprintf(info, cap, "time %u\nname %s\n", time_now(), name);
    for (size_t i = 0; i < ARRAY_SIZE(tracked) && !err; i++) {
        if (!vfs_exists(tracked[i]))
            continue;
        point_copy(id, tracked[i], path, sizeof(path));
        char parent[PATH_MAX_LEN];
        str_copy(parent, path, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash)
            *slash = 0;
        vfs_mkdirs(parent);
        err = vfs_copy(tracked[i], path);
        if (!err && len + PATH_MAX_LEN + 16 < cap)
            len += (size_t)ksnprintf(info + len, cap - len, "has %s\n", tracked[i]);
    }
    struct vdir d;
    if (!err && vfs_opendir(APPS_DIR, &d) == 0) {
        struct vfs_dirent ent;
        while (vfs_readdir(&d, &ent) == 0) {
            if (ent.type != VN_DIR || ent.name[0] == '.')
                continue;
            if (len + 128 >= cap) {
                char *grown = krealloc(info, cap * 2);
                if (!grown)
                    break;
                info = grown;
                cap *= 2;
            }
            len += (size_t)ksnprintf(info + len, cap - len, "app %s\n", ent.name);
        }
        vfs_closedir(&d);
    }
    if (!err) {
        ksnprintf(path, sizeof(path), "%s/info", dir);
        err = vfs_write_all(path, info, len);
    }
    kfree(info);
    if (err)
        vfs_remove_tree(dir);
    else if (id_out)
        *id_out = id;
    return err;
}

/* puts the point back; *removed: apps taken away, *missing: apps of the point that are gone (Store has them) */
static int restore_point(int id, int *removed, int *missing)
{
    struct point p;
    if (read_point(id, &p) != 0)
        return E_NOENT;
    char path[PATH_MAX_LEN], *info;
    size_t len;
    ksnprintf(path, sizeof(path), POINTS_DIR "/%d/info", id);
    if (vfs_read_all(path, &info, &len) != 0)
        return E_IO;
    char name[96];
    ksnprintf(name, sizeof(name), "Before going back to \"%s\"", p.name);
    int err = create_point(name, NULL);
    if (err) {
        kfree(info);
        return err;
    }
    for (size_t i = 0; i < ARRAY_SIZE(tracked) && !err; i++) {
        char has[PATH_MAX_LEN + 8];
        ksnprintf(has, sizeof(has), "has %s\n", tracked[i]);
        if (vfs_exists(tracked[i]))
            vfs_remove_tree(tracked[i]);
        if (strstr(info, has)) {
            point_copy(id, tracked[i], path, sizeof(path));
            err = vfs_copy(path, tracked[i]);
        }
    }
    *removed = *missing = 0;
    struct vdir d;
    char (*gone)[64] = kmalloc(64 * 64); /* names first: removing while reading the folder skips entries */
    int count = 0;
    if (!err && gone && vfs_opendir(APPS_DIR, &d) == 0) {
        struct vfs_dirent ent;
        while (vfs_readdir(&d, &ent) == 0 && count < 64)
            if (ent.type == VN_DIR && ent.name[0] != '.' && !point_has_app(info, ent.name))
                str_copy(gone[count++], ent.name, 64);
        vfs_closedir(&d);
    }
    for (int i = 0; i < count; i++) {
        path_join(APPS_DIR, gone[i], path, sizeof(path));
        if (vfs_remove_tree(path) == 0)
            ++*removed;
    }
    kfree(gone);
    for (char *line = info; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!strncmp(line, "app ", 4)) {
            path_join(APPS_DIR, line + 4, path, sizeof(path));
            *missing += !vfs_exists(path);
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(info);
    char *conf; /* the settings take effect now; the rest after a restart */
    if (!err && vfs_read_all("/home/.synth.conf", &conf, &len) == 0) {
        shell_exec_script(shell_current(), conf, 0, NULL);
        kfree(conf);
    }
    return err;
}

/* Reset This PC: the settings, apps and registry go, the documents stay */
static int reset_pc(void)
{
    int err = create_point("Before resetting this PC", NULL);
    if (err)
        return err;
    for (size_t i = 0; i < ARRAY_SIZE(tracked); i++)
        if (strcmp(tracked[i], "/etc") && vfs_exists(tracked[i]))
            vfs_remove_tree(tracked[i]);
    vfs_remove_tree("/etc/themes");
    vfs_remove_tree(APPS_DIR);
    vfs_unlink("/home/.setup-done"); /* the welcome setup runs again */
    vfs_unlink("/home/.session");
    return 0;
}

static void describe(const struct point *p, char *out, size_t cap)
{
    char when[24];
    time_format(p->time + (uint32_t)(time_zone() * 60), when, sizeof(when));
    ksnprintf(out, cap, "%s  %s", when, p->name);
}

/* ---- the window ---- */

enum { PAGE_POINTS, PAGE_RESET };

static const struct tool_item items[] = {
    {SYM_RESTART, "Restore Points", "System Restore"},
    {SYM_POWER, "Reset This PC", NULL},
};

struct restore_win {
    struct ui_state st;
    struct point points[MAX_POINTS];
    int count, page, selected;
    char status[128];
};

static struct window *open_win;

static void reload(struct restore_win *r)
{
    r->count = list_points(r->points, MAX_POINTS);
    r->selected = CLAMP(r->selected, 0, MAX(r->count - 1, 0));
}

static void made(struct window *owner, const char *name)
{
    struct restore_win *r = owner && wm_window_alive(owner) ? owner->data : NULL;
    if (!r || !name)
        return;
    int id, err = create_point(*name ? name : "Restore point", &id);
    if (err)
        ksnprintf(r->status, sizeof(r->status), "Could not make the point: %s", err_str(err));
    else
        ksnprintf(r->status, sizeof(r->status), "Restore point %d made", id);
    reload(r);
    wm_invalidate(owner);
}

static void restore_pick(struct window *owner, int index)
{
    struct restore_win *r = owner && wm_window_alive(owner) ? owner->data : NULL;
    if (!r || index != 1 || r->selected >= r->count)
        return;
    int removed, missing, err = restore_point(r->points[r->selected].id, &removed, &missing);
    if (err)
        ksnprintf(r->status, sizeof(r->status), "Could not go back: %s", err_str(err));
    else
        ksnprintf(r->status, sizeof(r->status), "Done: %d apps removed%s. Restart to finish.", removed,
                  missing ? ", some apps must be installed again from the Store" : "");
    reload(r);
    wm_invalidate(owner);
}

static void reset_pick(struct window *owner, int index)
{
    struct restore_win *r = owner && wm_window_alive(owner) ? owner->data : NULL;
    if (!r || index != 1)
        return;
    int err = reset_pc();
    if (err) {
        ksnprintf(r->status, sizeof(r->status), "Could not reset: %s", err_str(err));
        wm_invalidate(owner);
        return;
    }
    hal_reboot();
}

static void draw_point(struct canvas *c, struct rect row, int index, bool selected, void *ctx)
{
    struct restore_win *r = ctx;
    const struct point *p = &r->points[index];
    char line[160], apps[32];
    describe(p, line, sizeof(line));
    ksnprintf(apps, sizeof(apps), p->apps == 1 ? "%d app" : "%d apps", p->apps);
    gfx_text_box(c, (struct rect){row.x + 12, row.y, row.w - 100, row.h}, &font_sans, line, ui_row_text(selected),
                 ALIGN_LEFT);
    gfx_text_box(c, (struct rect){row.x + row.w - 90, row.y, 78, row.h}, &font_small, apps,
                 selected ? ui_row_text(true) : style.secondary, ALIGN_RIGHT);
}

static void frame(struct ui *ui, void *data)
{
    struct restore_win *r = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (tool_sidebar(ui, H, items, ARRAY_SIZE(items), &r->page))
        r->status[0] = 0;
    if (r->page == PAGE_POINTS) {
        struct rect a = tool_page(ui, W, H, "Restore Points", "Settings, registry, Dock and installed apps");
        int x = a.x + 20, w = a.w - 40, y = a.y + 16;
        if (ui->c)
            ui_wrap_text(ui->c, (struct rect){x, y, w, 40}, &font_small,
                         "Going back to a point puts back the settings it kept and removes the apps installed after "
                         "it. Your documents are not touched, and a point is made first so you can undo it.",
                         style.secondary);
        y += 46;
        int list_h = H - y - 84;
        static struct ui_list list = {0, 0, 0, 30, true};
        list.count = r->count;
        list.selected = r->selected;
        ui_list(ui, (struct rect){x, y, w, list_h}, &list, draw_point, r);
        r->selected = list.selected;
        if (!r->count)
            ui_label(ui, (struct rect){x, y + list_h / 2 - 10, w, 20}, "No restore points yet", &font_sans,
                     style.tertiary, ALIGN_CENTER);
        int by = H - 46;
        if (ui_button(ui, (struct rect){x, by, 150, 30}, "Create Point…"))
            ui_prompt(ui->win, "Create a Restore Point", "Name:", "Restore point", made);
        if (r->count && ui_button_primary(ui, (struct rect){x + w - 130, by, 130, 30}, "Go Back…")) {
            static const char *const buttons[] = {"Cancel", "Go Back"};
            char text[200], what[160];
            describe(&r->points[r->selected], what, sizeof(what));
            ksnprintf(text, sizeof(text), "The system goes back to %s. Apps installed later are removed.", what);
            ui_alert(ui->win, "Go back to this point?", text, ICON_WARNING, buttons, 2, restore_pick);
        }
        if (r->count && ui_button(ui, (struct rect){x + w - 240, by, 100, 30}, "Delete")) {
            char dir[PATH_MAX_LEN];
            point_dir(r->points[r->selected].id, dir, sizeof(dir));
            vfs_remove_tree(dir);
            reload(r);
        }
        ui_label(ui, (struct rect){x, by - 26, w, 20}, r->status, &font_small, style.secondary, ALIGN_LEFT);
    } else {
        struct rect a = tool_page(ui, W, H, "Reset This PC", NULL);
        int x = a.x + 20, w = a.w - 40, y = a.y + 20;
        if (ui->c)
            ui_wrap_text(ui->c, (struct rect){x, y, w, 120}, &font_sans,
                         "Removes all settings, installed apps, the Windows registry and themes, then restarts "
                         "with the welcome setup. Documents, pictures, music and everything else in your folders "
                         "stay. A restore point is made first.",
                         style.text);
        if (ui_button_primary(ui, (struct rect){x, y + 130, 150, 30}, "Reset…")) {
            static const char *const buttons[] = {"Cancel", "Reset"};
            ui_alert(ui->win, "Reset this PC?", "Settings and apps are removed and the computer restarts.",
                     ICON_WARNING, buttons, 2, reset_pick);
        }
        ui_label(ui, (struct rect){x, y + 170, w, 20}, r->status, &font_small, style.secondary, ALIGN_LEFT);
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct restore_win *r = w->data;
    ui_run(w, &r->st, c, NULL, frame, r);
}

static void event(struct window *w, struct gui_event *e)
{
    struct restore_win *r = w->data;
    ui_run(w, &r->st, NULL, e, frame, r);
}

static void destroy(struct window *w)
{
    kfree(w->data);
    if (open_win == w)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct restore_win *r = kzalloc(sizeof(*r));
    if (!r)
        return;
    reload(r);
    open_win = tool_open("System Restore", ICON_SETTINGS, 760, 480, &ops, r);
    if (!open_win)
        kfree(r);
}

static const struct gui_app restore_app = {
    .name = "System Restore", .description = "Restore points, going back to one and resetting this PC",
    .icon = ICON_SETTINGS, .launch = launch};

COMMAND(restore, "rstrui", "system", "restore [list|point [name]|to <n>|delete <n>|reset --yes]",
        "restore points of the settings, registry and apps; go back to one or reset this PC")
{
    const char *op = argc > 1 ? argv[1] : "list";
    if (!strcmp(op, "list")) {
        struct point *all = kmalloc(sizeof(struct point) * MAX_POINTS);
        if (!all)
            return sh_error(sh, "restore", "%s", err_str(E_NOMEM));
        int n = list_points(all, MAX_POINTS);
        for (int i = 0; i < n; i++) {
            char line[160];
            describe(&all[i], line, sizeof(line));
            sh_printf(sh, "%3d  %s (%d apps)\n", all[i].id, line, all[i].apps);
        }
        if (!n)
            sh_puts(sh, "no restore points\n");
        kfree(all);
    } else if (!strcmp(op, "point")) {
        char name[64] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(name, " ", sizeof(name));
            str_append(name, argv[i], sizeof(name));
        }
        int id, err = create_point(*name ? name : "Restore point", &id);
        if (err)
            return sh_error(sh, "restore", "%s", err_str(err));
        sh_printf(sh, "made restore point %d\n", id);
    } else if ((!strcmp(op, "to") || !strcmp(op, "delete")) && argc == 3) {
        long id;
        struct point p;
        if (!str_parse_long(argv[2], &id) || read_point((int)id, &p) != 0)
            return sh_error(sh, "restore", "no restore point %s", argv[2]);
        if (!strcmp(op, "delete")) {
            char dir[PATH_MAX_LEN];
            point_dir((int)id, dir, sizeof(dir));
            return vfs_remove_tree(dir) ? sh_error(sh, "restore", "cannot delete it") : 0;
        }
        int removed, missing, err = restore_point((int)id, &removed, &missing);
        if (err)
            return sh_error(sh, "restore", "%s", err_str(err));
        sh_printf(sh, "back to \"%s\": %d apps removed, %d to install again; restart to finish\n", p.name, removed,
                  missing);
    } else if (!strcmp(op, "reset")) {
        if (argc < 3 || strcmp(argv[2], "--yes"))
            return sh_error(sh, "restore", "removes the settings and apps, keeps documents; run 'restore reset --yes'");
        int err = reset_pc();
        if (err)
            return sh_error(sh, "restore", "%s", err_str(err));
        sh_puts(sh, "reset: restart the computer to finish\n");
    } else {
        return command_usage(sh, command_find("restore"));
    }
    return 0;
}

static const struct command *const commands[] = {&cmddef_restore};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&restore_app, NULL, &ext);
