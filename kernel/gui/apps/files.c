#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "../internal.h"

#define SIDEBAR_W 190
#define BAR_H TOOLBAR_HEIGHT
#define STATUS_H 26
#define HEADER_H 26
#define ROW_H 24
#define CELL_W 104
#define CELL_H 104
#define HISTORY 16
#define TRASH "/home/.Trash"

enum {
    CMD_NEW_WINDOW,
    CMD_NEW_FOLDER,
    CMD_OPEN,
    CMD_RENAME,
    CMD_TRASH,
    CMD_BACK,
    CMD_FORWARD,
    CMD_UP,
    CMD_GO_HOME,
    CMD_GO_DESKTOP,
    CMD_GO_DOCS,
    CMD_GO_DOWNLOADS,
    CMD_GO_APPS,
    CMD_GO_COMPUTER,
    CMD_GO_TO,
    CMD_ICONS,
    CMD_LIST,
    CMD_HIDDEN,
    CMD_TERMINAL,
    CMD_EMPTY_TRASH,
};

struct entry {
    char name[NAME_MAX_LEN];
    uint8_t type;
    uint32_t size;
};

struct files {
    struct ui_state st;
    char path[PATH_MAX_LEN];
    struct entry *all;
    int all_count;
    struct entry *entries;
    int count;
    struct ui_list list;
    struct ui_text search;
    char last_search[256];
    char back[HISTORY][PATH_MAX_LEN];
    int back_len;
    char forward[HISTORY][PATH_MAX_LEN];
    int forward_len;
    bool icons;
    bool hidden;
    int scroll;
};

static char file_clip[PATH_MAX_LEN];
static bool file_clip_cut;

struct place {
    const char *label;
    const char *path;
    enum symbol_id sym;
};

static char fav_paths[5][PATH_MAX_LEN];
static struct place favorites[6] = {
    {"Home", fav_paths[0], SYM_HOME},          {"Desktop", fav_paths[1], SYM_DESKTOP},
    {"Documents", fav_paths[2], SYM_DOC},      {"Downloads", fav_paths[3], SYM_DOWNLOAD},
    {"Pictures", fav_paths[4], SYM_PHOTO},     {"Applications", "/apps", SYM_APPS},
};

static void update_favorites(void)
{
    static const char *const sub[5] = {"", "Desktop", "Documents", "Downloads", "Pictures"};
    for (int i = 0; i < 5; i++)
        if (i)
            path_join(user_home(), sub[i], fav_paths[i], PATH_MAX_LEN);
        else
            str_copy(fav_paths[0], user_home(), PATH_MAX_LEN);
}

static const struct place locations[] = {
    {"SynthOS HD", "/", SYM_DISK}, {"Windows", "/system", SYM_FOLDER}, {"DOS", "/dos", SYM_FOLDER},
    {"Commands", "/bin", SYM_TERMINAL}, {"Temp", "/tmp", SYM_CLOCK},  {"Trash", TRASH, SYM_TRASH},
};

static const struct menu_item file_items[] = {
    {"New Window", KEY_COMMAND(CMD_NEW_WINDOW), "⌘N"}, {"New Folder", KEY_COMMAND(CMD_NEW_FOLDER), "⇧⌘N"},
    {"Open", KEY_COMMAND(CMD_OPEN), "⌘O"},             {"", 0, NULL},
    {"Rename…", KEY_COMMAND(CMD_RENAME), "F2"},        {"Move to Trash", KEY_COMMAND(CMD_TRASH), "Del"},
    {"", 0, NULL},                                     {"Open in Terminal", KEY_COMMAND(CMD_TERMINAL), NULL},
    {"Empty Trash", KEY_COMMAND(CMD_EMPTY_TRASH), NULL},
};

static const struct menu_item edit_items[] = {
    {"Copy", KEY_CTRL('c'), "⌘C"},
    {"Cut", KEY_CTRL('x'), "⌘X"},
    {"Paste", KEY_CTRL('v'), "⌘V"},
};

static const struct menu_item view_items[] = {
    {"as Icons", KEY_COMMAND(CMD_ICONS), "⌘1"},
    {"as List", KEY_COMMAND(CMD_LIST), "⌘2"},
    {"", 0, NULL},
    {"Show Hidden Files", KEY_COMMAND(CMD_HIDDEN), NULL},
};

static const struct menu_item go_items[] = {
    {"Back", KEY_COMMAND(CMD_BACK), "⌘["},
    {"Forward", KEY_COMMAND(CMD_FORWARD), "⌘]"},
    {"Enclosing Folder", KEY_COMMAND(CMD_UP), "⌫"},
    {"", 0, NULL},
    {"Computer", KEY_COMMAND(CMD_GO_COMPUTER), NULL},
    {"Home", KEY_COMMAND(CMD_GO_HOME), NULL},
    {"Desktop", KEY_COMMAND(CMD_GO_DESKTOP), NULL},
    {"Documents", KEY_COMMAND(CMD_GO_DOCS), NULL},
    {"Downloads", KEY_COMMAND(CMD_GO_DOWNLOADS), NULL},
    {"Applications", KEY_COMMAND(CMD_GO_APPS), NULL},
    {"", 0, NULL},
    {"Go to Folder…", KEY_COMMAND(CMD_GO_TO), "⌘L"},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"Edit", edit_items, ARRAY_SIZE(edit_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
    {"Go", go_items, ARRAY_SIZE(go_items)},
};

static enum icon_id icon_for(const char *dir, const struct entry *e)
{
    if (e->type == VN_DIR) {
        if (!strcmp(dir, "/home")) {
            if (!strcmp(e->name, "Desktop"))
                return ICON_FOLDER_DESKTOP;
            if (!strcmp(e->name, "Documents"))
                return ICON_FOLDER_DOCS;
            if (!strcmp(e->name, "Downloads"))
                return ICON_FOLDER_DOWNLOADS;
        }
        if (!strcmp(dir, "/") && !strcmp(e->name, "home"))
            return ICON_HOME;
        if (!strcmp(dir, "/") && !strcmp(e->name, "apps"))
            return ICON_FOLDER_APPS;
        return ICON_FOLDER;
    }
    const char *ext = path_ext(e->name);
    if (!ext)
        return ICON_FILE;
    if (!str_casecmp(ext, ".exe"))
        return ICON_APP;
    if (!str_casecmp(ext, ".dll"))
        return ICON_DLL;
    if (!str_casecmp(ext, ".com"))
        return ICON_DOS;
    if (!str_casecmp(ext, ".sx"))
        return ICON_SX;
    if (!str_casecmp(ext, ".sh") || !str_casecmp(ext, ".bat"))
        return ICON_SCRIPT;
    if (!str_casecmp(ext, ".bmp"))
        return ICON_IMAGE;
    if (str_in_list(ext + 1, "txt,md,c,h,cpp,asm,sasm,inc,conf,theme,log,ini,cfg,json,xml,html,css,js,py"))
        return ICON_TEXT;
    return ICON_FILE;
}

static const char *kind_name(const char *dir, const struct entry *e)
{
    switch (icon_for(dir, e)) {
    case ICON_FOLDER:
    case ICON_HOME:
    case ICON_FOLDER_APPS:
    case ICON_FOLDER_DOCS:
    case ICON_FOLDER_DESKTOP:
    case ICON_FOLDER_DOWNLOADS: return "Folder";
    case ICON_APP: return "Windows Application";
    case ICON_DLL: return "Windows Library";
    case ICON_DOS: return "DOS Program";
    case ICON_SX: return "SX Program";
    case ICON_SCRIPT: return "Shell Script";
    case ICON_IMAGE: return "BMP Image";
    case ICON_TEXT: return "Plain Text";
    default: return e->type == VN_DEV ? "Device" : "Document";
    }
}

static int compare(const struct entry *a, const struct entry *b)
{
    if ((a->type == VN_DIR) != (b->type == VN_DIR))
        return a->type == VN_DIR ? -1 : 1;
    return str_casecmp(a->name, b->name);
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!str_ncasecmp(p, needle, n))
            return true;
    return !n;
}

static void filter(struct files *f)
{
    kfree(f->entries);
    f->entries = kmalloc(sizeof(struct entry) * (size_t)MAX(f->all_count, 1));
    f->count = 0;
    if (!f->entries)
        return;
    for (int i = 0; i < f->all_count; i++) {
        struct entry *e = &f->all[i];
        if (!f->hidden && e->name[0] == '.')
            continue;
        if (!contains_ci(e->name, f->search.buf))
            continue;
        f->entries[f->count++] = *e;
    }
    f->list.count = f->count;
    if (f->list.selected >= f->count)
        f->list.selected = -1;
}

static void load(struct files *f)
{
    kfree(f->all);
    f->all = NULL;
    f->all_count = 0;
    struct vdir d;
    if (vfs_opendir(f->path, &d) == 0) {
        int cap = 0;
        struct vfs_dirent ent;
        while (vfs_readdir(&d, &ent) == 0) {
            if (!strcmp(ent.name, ".") || !strcmp(ent.name, ".."))
                continue;
            if (f->all_count == cap) {
                cap = cap ? cap * 2 : 32;
                struct entry *grown = krealloc(f->all, sizeof(struct entry) * (size_t)cap);
                if (!grown)
                    break;
                f->all = grown;
            }
            struct entry *e = &f->all[f->all_count++];
            str_copy(e->name, ent.name, sizeof(e->name));
            e->type = ent.type;
            e->size = ent.size;
        }
        vfs_closedir(&d);
    }
    for (int i = 1; i < f->all_count; i++)
        for (int j = i; j > 0 && compare(&f->all[j - 1], &f->all[j]) > 0; j--) {
            struct entry t = f->all[j];
            f->all[j] = f->all[j - 1];
            f->all[j - 1] = t;
        }
    filter(f);
}

static const char *display_name(const char *path)
{
    if (!strcmp(path, "/"))
        return "SynthOS HD";
    if (!strcmp(path, TRASH))
        return "Trash";
    static struct volume volumes[6];
    int n = vfs_volumes(volumes, (int)ARRAY_SIZE(volumes));
    for (int i = 0; i < n; i++)
        if (!strcmp(path, volumes[i].path))
            return volumes[i].label;
    return path_basename(path);
}

static void navigate(struct window *w, struct files *f, const char *path, bool remember)
{
    char resolved[PATH_MAX_LEN];
    int err = (path[0] && path[1] == ':') ? winpath_to_vfs(f->path, path, resolved, sizeof(resolved))
                                          : path_resolve(f->path, path, resolved, sizeof(resolved));
    if (!err && !strcmp(resolved, TRASH))
        vfs_mkdirs(TRASH);
    if (err || !vfs_is_dir(resolved)) {
        if (!err && vfs_exists(resolved)) {
            gui_open_file(resolved);
            return;
        }
        gui_message("Folder not found", "The folder can't be found.");
        return;
    }
    if (remember && f->path[0] && strcmp(resolved, f->path) != 0) {
        if (f->back_len == HISTORY) {
            memmove(f->back[0], f->back[1], sizeof(f->back[0]) * (HISTORY - 1));
            f->back_len--;
        }
        str_copy(f->back[f->back_len++], f->path, PATH_MAX_LEN);
        f->forward_len = 0;
    }
    str_copy(f->path, resolved, sizeof(f->path));
    ui_text_set(&f->search, "");
    f->last_search[0] = 0;
    f->list.selected = -1;
    f->list.scroll = 0;
    f->scroll = 0;
    load(f);
    wm_set_title(w, display_name(f->path));
}

static void go_back(struct window *w, struct files *f)
{
    if (!f->back_len)
        return;
    if (f->forward_len < HISTORY)
        str_copy(f->forward[f->forward_len++], f->path, PATH_MAX_LEN);
    char target[PATH_MAX_LEN];
    str_copy(target, f->back[--f->back_len], sizeof(target));
    navigate(w, f, target, false);
}

static void go_forward(struct window *w, struct files *f)
{
    if (!f->forward_len)
        return;
    if (f->back_len < HISTORY)
        str_copy(f->back[f->back_len++], f->path, PATH_MAX_LEN);
    char target[PATH_MAX_LEN];
    str_copy(target, f->forward[--f->forward_len], sizeof(target));
    navigate(w, f, target, false);
}

static void child_path(struct files *f, const char *name, char *out)
{
    path_join(f->path, name, out, PATH_MAX_LEN);
}

static struct entry *selected(struct files *f)
{
    return f->list.selected >= 0 && f->list.selected < f->count ? &f->entries[f->list.selected] : NULL;
}

static void open_entry(struct window *w, struct files *f, struct entry *e)
{
    char path[PATH_MAX_LEN];
    child_path(f, e->name, path);
    if (e->type == VN_DIR)
        navigate(w, f, path, true);
    else
        gui_open_file(path);
}

static int remove_tree(const char *path)
{
    if (vfs_is_dir(path)) {
        struct vdir d;
        if (vfs_opendir(path, &d) == 0) {
            struct vfs_dirent ent;
            char names[64][NAME_MAX_LEN];
            int n = 0;
            while (n < 64 && vfs_readdir(&d, &ent) == 0)
                if (strcmp(ent.name, ".") && strcmp(ent.name, ".."))
                    str_copy(names[n++], ent.name, NAME_MAX_LEN);
            vfs_closedir(&d);
            for (int i = 0; i < n; i++) {
                char child[PATH_MAX_LEN];
                path_join(path, names[i], child, sizeof(child));
                int err = remove_tree(child);
                if (err)
                    return err;
            }
        }
    }
    return vfs_unlink(path);
}

static int copy_tree(const char *from, const char *to)
{
    if (!vfs_is_dir(from))
        return vfs_copy(from, to);
    int err = vfs_mkdir(to);
    if (err && err != E_EXIST)
        return err;
    struct vdir d;
    if (vfs_opendir(from, &d) != 0)
        return E_IO;
    struct vfs_dirent ent;
    err = 0;
    while (vfs_readdir(&d, &ent) == 0) {
        if (!strcmp(ent.name, ".") || !strcmp(ent.name, ".."))
            continue;
        char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
        path_join(from, ent.name, a, sizeof(a));
        path_join(to, ent.name, b, sizeof(b));
        if ((err = copy_tree(a, b)) != 0)
            break;
    }
    vfs_closedir(&d);
    return err;
}

static int move_tree(const char *from, const char *to)
{
    int err = vfs_rename(from, to);
    if (err == E_XDEV && (err = copy_tree(from, to)) == 0)
        err = remove_tree(from);
    return err;
}

static void report(int err, const char *what)
{
    if (err) {
        char msg[160];
        ksnprintf(msg, sizeof(msg), "The item could not be %s: %s.", what, err_str(err));
        gui_message("The operation can't be completed", msg);
    }
}

static void refresh(struct window *w, struct files *f)
{
    load(f);
    wm_invalidate(w);
}

static void do_rename(struct window *w, const char *value)
{
    struct files *f = w->data;
    struct entry *e = selected(f);
    if (!e || !*value)
        return;
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    child_path(f, e->name, a);
    child_path(f, value, b);
    report(vfs_rename(a, b), "renamed");
    refresh(w, f);
}

static void new_folder(struct window *w, struct files *f)
{
    char p[PATH_MAX_LEN];
    for (int i = 1; i < 100; i++) {
        char name[32];
        if (i == 1)
            str_copy(name, "untitled folder", sizeof(name));
        else
            ksnprintf(name, sizeof(name), "untitled folder %d", i);
        child_path(f, name, p);
        if (!vfs_exists(p)) {
            report(vfs_mkdir(p), "created");
            refresh(w, f);
            for (int k = 0; k < f->count; k++)
                if (!strcmp(f->entries[k].name, name))
                    f->list.selected = k;
            return;
        }
    }
}

static void do_delete(struct window *w, const char *value)
{
    UNUSED(value);
    struct files *f = w->data;
    struct entry *e = selected(f);
    if (!e)
        return;
    char p[PATH_MAX_LEN];
    child_path(f, e->name, p);
    report(remove_tree(p), "deleted");
    refresh(w, f);
}

static void trash(struct window *w, struct files *f)
{
    struct entry *e = selected(f);
    if (!e)
        return;
    if (str_starts(f->path, TRASH)) {
        char msg[160];
        ksnprintf(msg, sizeof(msg), "\"%s\" will be deleted immediately. You can't undo this action.", e->name);
        ui_confirm(w, "Delete immediately?", msg, do_delete);
        return;
    }
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    child_path(f, e->name, from);
    vfs_mkdirs(TRASH);
    path_join(TRASH, e->name, to, sizeof(to));
    for (int i = 2; vfs_exists(to) && i < 100; i++) {
        char name[NAME_MAX_LEN + 8];
        ksnprintf(name, sizeof(name), "%s %d", e->name, i);
        path_join(TRASH, name, to, sizeof(to));
    }
    report(move_tree(from, to), "moved to the Trash");
    refresh(w, f);
    chrome_changed();
}

static void empty_trash(struct window *w, const char *value)
{
    UNUSED(value);
    struct files *f = w->data;
    remove_tree(TRASH);
    vfs_mkdirs(TRASH);
    refresh(w, f);
    chrome_changed();
}

static void paste(struct window *w, struct files *f)
{
    if (!file_clip[0])
        return;
    char target[PATH_MAX_LEN];
    child_path(f, path_basename(file_clip), target);
    if (strcmp(target, file_clip) == 0)
        return;
    int err = file_clip_cut ? move_tree(file_clip, target) : copy_tree(file_clip, target);
    report(err, file_clip_cut ? "moved" : "copied");
    if (file_clip_cut && !err)
        file_clip[0] = 0;
    refresh(w, f);
}

static void go_to(struct window *w, const char *value)
{
    struct files *f = w->data;
    if (*value)
        navigate(w, f, value, true);
}

static void open_terminal_here(struct files *f)
{
    char line[PATH_MAX_LEN + 8];
    ksnprintf(line, sizeof(line), "cd '%s'", f->path);
    gui_terminal_run(line);
}

enum {
    M_OPEN,
    M_EDIT,
    M_RUN,
    M_SEP1,
    M_COPY,
    M_CUT,
    M_RENAME,
    M_TRASH,
};

static const char *const item_menu[] = {"Open", "Open with Notepad", "Run in Terminal", "", "Copy", "Cut",
                                        "Rename…", "Move to Trash"};
static const char *const space_menu[] = {"New Folder", "Paste", "", "Open in Terminal", "View as Icons",
                                         "View as List"};

static void pick_item(struct window *w, int index)
{
    struct files *f = w->data;
    struct entry *e = selected(f);
    if (!e)
        return;
    char p[PATH_MAX_LEN];
    child_path(f, e->name, p);
    switch (index) {
    case M_OPEN: open_entry(w, f, e); break;
    case M_EDIT: gui_app_launch(gui_app_find("Notepad"), p); break;
    case M_RUN: {
        char line[PATH_MAX_LEN + 8];
        ksnprintf(line, sizeof(line), "'%s'", p);
        gui_terminal_run(line);
        break;
    }
    case M_COPY:
    case M_CUT:
        str_copy(file_clip, p, sizeof(file_clip));
        file_clip_cut = index == M_CUT;
        break;
    case M_RENAME: ui_prompt(w, "Rename", "Enter a new name:", e->name, do_rename); break;
    case M_TRASH: trash(w, f); break;
    }
    wm_invalidate(w);
}

static void pick_space(struct window *w, int index)
{
    struct files *f = w->data;
    if (index == 0)
        new_folder(w, f);
    else if (index == 1)
        paste(w, f);
    else if (index == 3)
        open_terminal_here(f);
    else if (index == 4 || index == 5)
        f->icons = index == 4;
    wm_invalidate(w);
}

static void command(struct window *w, struct files *f, int cmd)
{
    switch (cmd) {
    case CMD_NEW_WINDOW: gui_app_launch(w->app, f->path); break;
    case CMD_NEW_FOLDER: new_folder(w, f); break;
    case CMD_OPEN:
        if (selected(f))
            open_entry(w, f, selected(f));
        break;
    case CMD_RENAME:
        if (selected(f))
            ui_prompt(w, "Rename", "Enter a new name:", selected(f)->name, do_rename);
        break;
    case CMD_TRASH: trash(w, f); break;
    case CMD_BACK: go_back(w, f); break;
    case CMD_FORWARD: go_forward(w, f); break;
    case CMD_UP: navigate(w, f, "..", true); break;
    case CMD_GO_HOME: navigate(w, f, user_home(), true); break;
    case CMD_GO_DESKTOP: update_favorites(); navigate(w, f, fav_paths[1], true); break;
    case CMD_GO_DOCS: update_favorites(); navigate(w, f, fav_paths[2], true); break;
    case CMD_GO_DOWNLOADS: update_favorites(); navigate(w, f, fav_paths[3], true); break;
    case CMD_GO_APPS: navigate(w, f, "/apps", true); break;
    case CMD_GO_COMPUTER: navigate(w, f, "/", true); break;
    case CMD_GO_TO: ui_prompt(w, "Go to Folder", "Type a path, for example C:\\Windows or /home:", f->path, go_to); break;
    case CMD_ICONS: f->icons = true; break;
    case CMD_LIST: f->icons = false; break;
    case CMD_HIDDEN:
        f->hidden = !f->hidden;
        filter(f);
        break;
    case CMD_TERMINAL: open_terminal_here(f); break;
    case CMD_EMPTY_TRASH:
        ui_confirm(w, "Empty the Trash?", "You can't undo this action.", empty_trash);
        break;
    }
}

static void draw_row(struct canvas *c, struct rect row, int index, bool sel, void *ctx)
{
    struct files *f = ctx;
    struct entry *e = &f->entries[index];
    uint32_t ink = ui_row_text(sel), dim = sel && ink != style.text ? ALPHA(0xffffffffu, 200) : style.secondary;
    int name_w = row.w - 280;
    gfx_icon(c, row.x + 14, row.y + (row.h - 16) / 2, icon_for(f->path, e), 16);
    gfx_text_box(c, (struct rect){row.x + 38, row.y, name_w - 38, row.h}, &font_sans, e->name, ink, ALIGN_LEFT);
    char size[16];
    if (e->type == VN_DIR)
        str_copy(size, "--", sizeof(size));
    else
        fmt_size(size, sizeof(size), e->size);
    gfx_text_box(c, (struct rect){row.x + name_w, row.y, 90, row.h}, &font_sans, size, dim, ALIGN_RIGHT);
    gfx_text_box(c, (struct rect){row.x + name_w + 110, row.y, 160, row.h}, &font_sans, kind_name(f->path, e), dim,
                 ALIGN_LEFT);
}

static void sidebar_section(struct ui *ui, struct window *w, struct files *f, int *y, const char *title,
                            const struct place *places, int n)
{
    ui_sidebar_header(ui, (struct rect){24, *y, SIDEBAR_W - 34, 20}, title);
    *y += 22;
    for (int i = 0; i < n; i++) {
        struct rect r = {14, *y, SIDEBAR_W - 26, 28};
        if (ui_sidebar_item(ui, r, places[i].sym, places[i].label, !strcmp(f->path, places[i].path)))
            navigate(w, f, places[i].path, true);
        *y += 28;
    }
    *y += 10;
}

static void icon_label(struct canvas *c, struct rect r, const char *name, bool sel, bool focused)
{
    const struct ui_font *font = &font_small;
    size_t n = strlen(name), first = n;
    while (first > 0 && gfx_text_width_n(font, name, first) > r.w - 8)
        first--;
    if (first < n) {
        size_t cut = first;
        while (cut > 0 && name[cut] != ' ' && name[cut] != '.' && name[cut] != '_')
            cut--;
        if (cut > first / 2)
            first = cut;
    }
    char line1[NAME_MAX_LEN], line2[NAME_MAX_LEN];
    memcpy(line1, name, first);
    line1[first] = 0;
    str_copy(line2, name + first, sizeof(line2));
    while (line2[0] == ' ')
        memmove(line2, line2 + 1, strlen(line2));
    int lines = line2[0] ? 2 : 1;
    int w1 = gfx_text_width(font, line1), w2 = MIN(gfx_text_width(font, line2), r.w - 8);
    if (sel) {
        uint32_t bg = focused ? style.accent : style.selection_inactive;
        gfx_round(c, (struct rect){r.x + (r.w - w1) / 2 - 4, r.y, w1 + 8, 16}, 4, bg);
        if (lines == 2)
            gfx_round(c, (struct rect){r.x + (r.w - w2) / 2 - 4, r.y + 15, w2 + 8, 16}, 4, bg);
    }
    uint32_t ink = sel && focused ? RGB(255, 255, 255) : style.text;
    gfx_text_box(c, (struct rect){r.x, r.y, r.w, 16}, font, line1, ink, ALIGN_CENTER);
    if (lines == 2)
        gfx_text_box(c, (struct rect){r.x + 4, r.y + 15, r.w - 8, 16}, font, line2, ink, ALIGN_CENTER);
}

static int grid_columns(struct rect area)
{
    return MAX(1, (area.w - 16) / CELL_W);
}

static struct rect grid_cell(struct files *f, struct rect area, int i)
{
    int cols = grid_columns(area);
    int x0 = area.x + (area.w - cols * CELL_W) / 2;
    return (struct rect){x0 + (i % cols) * CELL_W, area.y + 10 + (i / cols) * CELL_H - f->scroll, CELL_W, CELL_H};
}

static int icon_grid(struct ui *ui, struct files *f, struct rect area)
{
    int result = LIST_NONE;
    int id = ui_next_id(ui);
    int cols = grid_columns(area), rows = (f->count + cols - 1) / cols;
    int content = rows * CELL_H + 20;
    struct gui_event *e = ui->e;
    if (e && rect_contains(area, e->x, e->y)) {
        if (e->type == EV_WHEEL) {
            f->scroll += e->wheel * 40;
            ui->changed = true;
        } else if (e->type == EV_MOUSE_DOWN) {
            ui->consumed = true;
            ui->st->focus = id;
            int hit = -1;
            for (int i = 0; i < f->count; i++) {
                struct rect cell = grid_cell(f, area, i);
                struct rect body = {cell.x + 12, cell.y, cell.w - 24, cell.h - 8};
                if (rect_contains(body, e->x, e->y))
                    hit = i;
            }
            f->list.selected = hit;
            result = e->button == BUTTON_RIGHT ? LIST_CONTEXT : hit >= 0 && e->clicks >= 2 ? LIST_ACTIVATE : LIST_SELECT;
        }
    }
    if (e && e->type == EV_KEY && ui->st->focus == id && !ui->consumed && f->count) {
        int key = KEY_BASE(e->key), s = f->list.selected;
        int old = s;
        if (key == KEY_RIGHT)
            s = MIN(f->count - 1, s + 1);
        else if (key == KEY_LEFT)
            s = MAX(0, s - 1);
        else if (key == KEY_DOWN)
            s = s < 0 ? 0 : MIN(f->count - 1, s + cols);
        else if (key == KEY_UP)
            s = MAX(0, s - cols);
        else if (key == KEY_ENTER || e->key == '\r')
            result = LIST_ACTIVATE;
        if (s != old) {
            f->list.selected = s;
            result = LIST_SELECT;
            struct rect cell = grid_cell(f, area, s);
            if (cell.y < area.y)
                f->scroll -= area.y - cell.y + 10;
            if (cell.y + cell.h > area.y + area.h)
                f->scroll += cell.y + cell.h - area.y - area.h;
        }
        if (result != LIST_NONE)
            ui->consumed = true;
    }
    f->scroll = CLAMP(f->scroll, 0, MAX(0, content - area.h));
    if (ui->c) {
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, area);
        bool focused = ui->win == wm_focused();
        for (int i = 0; i < f->count; i++) {
            struct rect cell = grid_cell(f, area, i);
            if (cell.y + cell.h < area.y || cell.y > area.y + area.h)
                continue;
            bool sel = i == f->list.selected;
            if (sel)
                gfx_round(&inner, (struct rect){cell.x + (cell.w - 72) / 2, cell.y - 2, 72, 70}, 8,
                          style.dark ? ALPHA(0xffffffffu, 36) : ALPHA(0, 26));
            gfx_icon(&inner, cell.x + (cell.w - 64) / 2, cell.y + 1, icon_for(f->path, &f->entries[i]), 64);
            icon_label(&inner, (struct rect){cell.x + 2, cell.y + 70, cell.w - 4, 34}, f->entries[i].name, sel, focused);
        }
    }
    ui_scrollbar(ui, (struct rect){area.x + area.w - 10, area.y, 10, area.h}, &f->scroll, content, area.h);
    return result;
}

static void status_line(struct files *f, char *text, size_t cap)
{
    uint64_t total = 0, free = 0;
    struct mount *best = NULL;
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        size_t n = strlen(m->path);
        bool under = str_starts(f->path, m->path) && (n == 1 || f->path[n] == 0 || f->path[n] == '/');
        if (under && (!best || n > strlen(best->path)))
            best = m;
    }
    if (best && best->fs->statfs)
        best->fs->statfs(best->root, &total, &free);
    char free_text[16];
    fmt_size(free_text, sizeof(free_text), free);
    if (total)
        ksnprintf(text, cap, "%d item%s, %s available", f->count, f->count == 1 ? "" : "s", free_text);
    else
        ksnprintf(text, cap, "%d item%s", f->count, f->count == 1 ? "" : "s");
}

static void frame(struct ui *ui, void *data)
{
    struct files *f = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    int mx = SIDEBAR_W;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){mx, 0, W - mx, H}, style.content);
        ui_sidebar_panel(ui, mx, H, style.content);
        gfx_fill(ui->c, (struct rect){mx + 8, BAR_H - 1, W - mx - 8, 1}, style.separator);
    }
    int y = BAR_H + 4;
    update_favorites();
    sidebar_section(ui, w, f, &y, "Favorites", favorites, ARRAY_SIZE(favorites));
    sidebar_section(ui, w, f, &y, "Locations", locations, 1);
    y -= 10;
    struct volume volumes[6];
    int volume_count = vfs_volumes(volumes, (int)ARRAY_SIZE(volumes));
    for (int i = 0; i < volume_count; i++) {
        struct rect r = {14, y, SIDEBAR_W - 26, 28};
        bool here = str_starts(f->path, volumes[i].path) &&
                    (f->path[strlen(volumes[i].path)] == 0 || f->path[strlen(volumes[i].path)] == '/');
        if (ui_sidebar_item(ui, (struct rect){r.x, r.y, r.w - 26, r.h}, SYM_USB, volumes[i].label,
                            !strcmp(f->path, volumes[i].path)))
            navigate(w, f, volumes[i].path, true);
        if (ui_symbol_button(ui, (struct rect){r.x + r.w - 24, r.y + 4, 20, 20}, SYM_EJECT, false)) {
            char path[PATH_MAX_LEN];
            str_copy(path, volumes[i].path, sizeof(path));
            if (here)
                navigate(w, f, user_home(), true);
            gui_eject(path);
        }
        y += 28;
    }
    for (size_t i = 1; i < ARRAY_SIZE(locations); i++) {
        struct rect r = {14, y, SIDEBAR_W - 26, 28};
        if (ui_sidebar_item(ui, r, locations[i].sym, locations[i].label, !strcmp(f->path, locations[i].path)))
            navigate(w, f, locations[i].path, true);
        y += 28;
    }
    y += 10;

    int bx = mx + 12;
    if (ui_symbol_button(ui, (struct rect){bx, 12, 28, 28}, SYM_LEFT, false))
        go_back(w, f);
    if (ui_symbol_button(ui, (struct rect){bx + 30, 12, 28, 28}, SYM_RIGHT, false))
        go_forward(w, f);
    ui_label(ui, (struct rect){bx + 70, 0, W - bx - 70 - 280, BAR_H}, display_name(f->path), &font_bold, style.text,
             ALIGN_LEFT);
    int rx = W - 12 - 170;
    struct rect search = {rx, 12, 170, 28};
    ui_search(ui, search, &f->search, "Search");
    if (strcmp(f->search.buf, f->last_search)) {
        str_copy(f->last_search, f->search.buf, sizeof(f->last_search));
        f->list.selected = -1;
        filter(f);
    }
    if (ui_symbol_button(ui, (struct rect){rx - 40, 12, 28, 28}, SYM_NEWFOLDER, false))
        new_folder(w, f);
    if (ui_symbol_button(ui, (struct rect){rx - 106, 12, 28, 28}, SYM_GRID, f->icons))
        f->icons = true;
    if (ui_symbol_button(ui, (struct rect){rx - 76, 12, 28, 28}, SYM_LIST, !f->icons))
        f->icons = false;

    struct rect area = {mx + 1, BAR_H, W - mx - 1, H - BAR_H - STATUS_H};
    int result;
    if (f->icons) {
        result = icon_grid(ui, f, area);
    } else {
        if (ui->c) {
            int name_w = area.w - 280;
            gfx_text_box(ui->c, (struct rect){area.x + 38, area.y, 100, HEADER_H}, &font_small, "Name", style.secondary,
                         ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){area.x + name_w, area.y, 90, HEADER_H}, &font_small, "Size",
                         style.secondary, ALIGN_RIGHT);
            gfx_text_box(ui->c, (struct rect){area.x + name_w + 110, area.y, 100, HEADER_H}, &font_small, "Kind",
                         style.secondary, ALIGN_LEFT);
            gfx_fill(ui->c, (struct rect){area.x, area.y + HEADER_H - 1, area.w, 1}, style.separator);
        }
        f->list.row_h = ROW_H;
        f->list.zebra = true;
        result = ui_list(ui, (struct rect){area.x, area.y + HEADER_H, area.w, area.h - HEADER_H}, &f->list, draw_row, f);
    }
    if (result == LIST_ACTIVATE && selected(f)) {
        open_entry(w, f, selected(f));
        return;
    }
    if (result == LIST_CONTEXT && ui->e) {
        if (selected(f))
            ui_menu(w, ui->e->x, ui->e->y, item_menu, (int)ARRAY_SIZE(item_menu), pick_item);
        else
            ui_menu(w, ui->e->x, ui->e->y, space_menu, (int)ARRAY_SIZE(space_menu), pick_space);
        return;
    }

    if (ui->c) {
        gfx_fill(ui->c, (struct rect){mx + 1, H - STATUS_H, W - mx - 1, 1}, style.separator);
        char text[128];
        status_line(f, text, sizeof(text));
        if (!strcmp(f->path, TRASH) && f->count)
            str_append(text, "   —   choose Empty Trash in the File menu to delete", sizeof(text));
        gfx_text_box(ui->c, (struct rect){mx, H - STATUS_H, W - mx, STATUS_H}, &font_small, text, style.secondary,
                     ALIGN_CENTER);
    }

    int key = ui_key(ui);
    if (key >= KEY_COMMAND(0) && key < KEY_COMMAND(64)) {
        command(w, f, key - KEY_COMMAND(0));
        return;
    }
    int base = KEY_BASE(key);
    if (key == KEY_BACKSPACE)
        navigate(w, f, "..", true);
    else if (base == KEY_DELETE)
        trash(w, f);
    else if (base == KEY_F2 && selected(f))
        ui_prompt(w, "Rename", "Enter a new name:", selected(f)->name, do_rename);
    else if (base == KEY_F5)
        refresh(w, f);
    else if (key == KEY_CTRL('c') || key == KEY_CTRL('x')) {
        if (selected(f)) {
            child_path(f, selected(f)->name, file_clip);
            file_clip_cut = key == KEY_CTRL('x');
        }
    } else if (key == KEY_CTRL('v'))
        paste(w, f);
    else if (key == KEY_CTRL('n'))
        command(w, f, CMD_NEW_WINDOW);
    else if (key == (KEY_CTRL('n') | KEY_MOD_SHIFT))
        command(w, f, CMD_NEW_FOLDER);
    else if (key == KEY_CTRL('o'))
        command(w, f, CMD_OPEN);
    else if (key == KEY_CTRL('l'))
        command(w, f, CMD_GO_TO);
    else if (key == KEY_CTRL('['))
        go_back(w, f);
    else if (key == KEY_CTRL(']'))
        go_forward(w, f);
    else if (key == ('1' | KEY_MOD_SUPER) || key == ('1' | KEY_MOD_CTRL))
        f->icons = true;
    else if (key == ('2' | KEY_MOD_SUPER) || key == ('2' | KEY_MOD_CTRL))
        f->icons = false;
}

static void paint(struct window *w, struct canvas *c)
{
    struct files *f = w->data;
    ui_run(w, &f->st, c, NULL, frame, f);
}

static void event(struct window *w, struct gui_event *e)
{
    struct files *f = w->data;
    ui_run(w, &f->st, NULL, e, frame, f);
}

static void destroy(struct window *w)
{
    struct files *f = w->data;
    kfree(f->all);
    kfree(f->entries);
    kfree(f);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    struct files *f = kzalloc(sizeof(*f));
    if (!f)
        return;
    f->icons = true;
    f->list.selected = -1;
    struct window *w = wm_open("Files", ICON_FILES, 860, 520, &ops, f, WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!w) {
        kfree(f);
        return;
    }
    w->toolbar_h = BAR_H;
    w->min_w = 560;
    w->min_h = 320;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    navigate(w, f, arg && *arg ? arg : user_home(), false);
}

static const struct gui_app files_app = {"Files", "Browse folders and open files", ICON_FILES, launch, NULL};
GUI_APP(files_app);
