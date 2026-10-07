#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "../internal.h"

#define TOOLBAR_H 44
#define SIDEBAR_W 150
#define STATUS_H 26
#define HEADER_H 24
#define ROW_H 24
#define HISTORY 16

struct entry {
    char name[NAME_MAX_LEN];
    uint8_t type;
    uint32_t size;
};

struct files {
    struct ui_state st;
    char path[PATH_MAX_LEN];
    struct entry *entries;
    int count;
    struct ui_list list;
    struct ui_text location;
    char history[HISTORY][PATH_MAX_LEN];
    int history_len;
    int menu_x, menu_y;
};

static char file_clip[PATH_MAX_LEN];
static bool file_clip_cut;

static const struct {
    const char *label;
    const char *path;
    enum icon_id icon;
} places[] = {
    {"Computer", "/", ICON_COMPUTER},   {"Home", "/home", ICON_HOME},      {"Programs", "/apps", ICON_FOLDER},
    {"Windows", "/system", ICON_FOLDER}, {"DOS", "/dos", ICON_DOS},         {"Commands", "/bin", ICON_SX},
    {"Temp", "/tmp", ICON_FOLDER},
};

static enum icon_id icon_for(const struct entry *e)
{
    if (e->type == VN_DIR)
        return ICON_FOLDER;
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
    if (!str_casecmp(ext, ".sh"))
        return ICON_SCRIPT;
    if (!str_casecmp(ext, ".bmp"))
        return ICON_IMAGE;
    if (str_in_list(ext + 1, "txt,md,c,h,cpp,asm,sasm,inc,conf,theme,log,ini,cfg,json,xml,html,css,js,py,bat"))
        return ICON_TEXT;
    return ICON_FILE;
}

static const char *type_name(const struct entry *e)
{
    switch (icon_for(e)) {
    case ICON_FOLDER: return "Folder";
    case ICON_APP: return "Windows program";
    case ICON_DLL: return "Windows library";
    case ICON_DOS: return "DOS program";
    case ICON_SX: return "SX program";
    case ICON_SCRIPT: return "Shell script";
    case ICON_IMAGE: return "Bitmap image";
    case ICON_TEXT: return "Text document";
    default: return e->type == VN_DEV ? "Device" : "File";
    }
}

static int compare(const struct entry *a, const struct entry *b)
{
    if ((a->type == VN_DIR) != (b->type == VN_DIR))
        return a->type == VN_DIR ? -1 : 1;
    return str_casecmp(a->name, b->name);
}

static void load(struct files *f)
{
    kfree(f->entries);
    f->entries = NULL;
    f->count = 0;
    struct vdir d;
    if (vfs_opendir(f->path, &d) != 0)
        return;
    int cap = 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (f->count == cap) {
            cap = cap ? cap * 2 : 32;
            struct entry *grown = krealloc(f->entries, sizeof(struct entry) * (size_t)cap);
            if (!grown)
                break;
            f->entries = grown;
        }
        struct entry *e = &f->entries[f->count++];
        str_copy(e->name, ent.name, sizeof(e->name));
        e->type = ent.type;
        e->size = ent.size;
    }
    vfs_closedir(&d);
    for (int i = 1; i < f->count; i++)
        for (int j = i; j > 0 && compare(&f->entries[j - 1], &f->entries[j]) > 0; j--) {
            struct entry t = f->entries[j];
            f->entries[j] = f->entries[j - 1];
            f->entries[j - 1] = t;
        }
    f->list.count = f->count;
    f->list.selected = f->count ? 0 : -1;
    f->list.scroll = 0;
}

static void update_title(struct window *w, struct files *f)
{
    char title[64];
    const char *base = strcmp(f->path, "/") ? path_basename(f->path) : "Computer";
    ksnprintf(title, sizeof(title), "%s - Files", base);
    wm_set_title(w, title);
    ui_text_set(&f->location, f->path);
}

static void navigate(struct window *w, struct files *f, const char *path, bool remember)
{
    char resolved[PATH_MAX_LEN];
    int err = (path[0] && path[1] == ':') ? winpath_to_vfs(f->path, path, resolved, sizeof(resolved))
                                          : path_resolve(f->path, path, resolved, sizeof(resolved));
    if (err || !vfs_is_dir(resolved)) {
        if (!err && vfs_exists(resolved)) {
            gui_open_file(resolved);
            return;
        }
        gui_message("Files", "That folder does not exist.");
        return;
    }
    if (remember && strcmp(resolved, f->path) != 0) {
        if (f->history_len == HISTORY) {
            memmove(f->history[0], f->history[1], sizeof(f->history[0]) * (HISTORY - 1));
            f->history_len--;
        }
        str_copy(f->history[f->history_len++], f->path, PATH_MAX_LEN);
    }
    str_copy(f->path, resolved, sizeof(f->path));
    load(f);
    update_title(w, f);
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
    while (vfs_readdir(&d, &ent) == 0) {
        char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
        path_join(from, ent.name, a, sizeof(a));
        path_join(to, ent.name, b, sizeof(b));
        if ((err = copy_tree(a, b)) != 0)
            break;
    }
    vfs_closedir(&d);
    return err;
}

static void report(int err, const char *what)
{
    if (err) {
        char msg[160];
        ksnprintf(msg, sizeof(msg), "Could not %s: %s.", what, err_str(err));
        gui_message("Files", msg);
    }
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
    report(vfs_rename(a, b), "rename");
    load(f);
    wm_invalidate(w);
}

static void do_mkdir(struct window *w, const char *value)
{
    struct files *f = w->data;
    if (!*value)
        return;
    char p[PATH_MAX_LEN];
    child_path(f, value, p);
    report(vfs_mkdir(p), "create the folder");
    load(f);
    wm_invalidate(w);
}

static void do_newfile(struct window *w, const char *value)
{
    struct files *f = w->data;
    if (!*value)
        return;
    char p[PATH_MAX_LEN];
    child_path(f, value, p);
    if (vfs_exists(p))
        report(E_EXIST, "create the file");
    else
        report(vfs_write_all(p, "", 0), "create the file");
    load(f);
    wm_invalidate(w);
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
    report(remove_tree(p), "delete");
    load(f);
    wm_invalidate(w);
}

static void ask_delete(struct window *w, struct files *f)
{
    struct entry *e = selected(f);
    if (!e)
        return;
    char msg[128];
    ksnprintf(msg, sizeof(msg), "Delete \"%s\"%s?", e->name, e->type == VN_DIR ? " and everything inside" : "");
    ui_confirm(w, "Delete", msg, do_delete);
}

static void paste(struct window *w, struct files *f)
{
    if (!file_clip[0])
        return;
    char target[PATH_MAX_LEN];
    child_path(f, path_basename(file_clip), target);
    if (strcmp(target, file_clip) == 0)
        return;
    int err = file_clip_cut ? vfs_rename(file_clip, target) : copy_tree(file_clip, target);
    if (err == E_XDEV && file_clip_cut && (err = copy_tree(file_clip, target)) == 0)
        err = remove_tree(file_clip);
    report(err, file_clip_cut ? "move" : "copy");
    if (file_clip_cut && !err)
        file_clip[0] = 0;
    load(f);
    wm_invalidate(w);
}

enum {
    M_OPEN,
    M_EDIT,
    M_RUN,
    M_SEP1,
    M_COPY,
    M_CUT,
    M_RENAME,
    M_DELETE,
};

static const char *const item_menu[] = {"Open", "Edit in Notepad", "Run in Terminal", "", "Copy", "Cut",
                                        "Rename", "Delete"};
static const char *const space_menu[] = {"New folder", "New text file", "Paste", "", "Open terminal here",
                                         "Refresh"};

static void pick_item(struct window *w, int index)
{
    struct files *f = w->data;
    struct entry *e = selected(f);
    if (!e)
        return;
    char p[PATH_MAX_LEN];
    child_path(f, e->name, p);
    switch (index) {
    case M_OPEN:
        open_entry(w, f, e);
        break;
    case M_EDIT: {
        const struct gui_app *app = gui_app_find("Notepad");
        if (app)
            app->launch(p);
        break;
    }
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
    case M_RENAME:
        ui_prompt(w, "Rename", "New name:", e->name, do_rename);
        break;
    case M_DELETE:
        ask_delete(w, f);
        break;
    }
    wm_invalidate(w);
}

static void pick_space(struct window *w, int index)
{
    struct files *f = w->data;
    if (index == 0)
        ui_prompt(w, "New folder", "Folder name:", "New folder", do_mkdir);
    else if (index == 1)
        ui_prompt(w, "New file", "File name:", "notes.txt", do_newfile);
    else if (index == 2)
        paste(w, f);
    else if (index == 4) {
        char line[PATH_MAX_LEN + 8];
        ksnprintf(line, sizeof(line), "cd '%s'", f->path);
        gui_terminal_run(line);
    } else if (index == 5) {
        load(f);
    }
    wm_invalidate(w);
}

static void draw_row(struct canvas *c, struct rect row, int index, bool sel, void *ctx)
{
    UNUSED(sel);
    struct files *f = ctx;
    struct entry *e = &f->entries[index];
    int name_w = row.w - 230;
    gfx_icon(c, row.x + 8, row.y + (row.h - 16) / 2, icon_for(e), 16);
    gfx_text_box(c, (struct rect){row.x + 32, row.y, name_w - 32, row.h}, &font_sans, e->name, style.text,
                 ALIGN_LEFT);
    if (e->type != VN_DIR) {
        char size[16];
        fmt_size(size, sizeof(size), e->size);
        gfx_text_box(c, (struct rect){row.x + name_w, row.y, 80, row.h}, &font_sans, size, style.muted, ALIGN_RIGHT);
    }
    gfx_text_box(c, (struct rect){row.x + name_w + 100, row.y, 130, row.h}, &font_sans, type_name(e), style.muted,
                 ALIGN_LEFT);
}

static void frame(struct ui *ui, void *data)
{
    struct files *f = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    ui_panel(ui, (struct rect){0, 0, W, H}, style.window);
    ui_panel(ui, (struct rect){0, 0, W, TOOLBAR_H}, style.panel);

    if (ui_button(ui, (struct rect){8, 8, 30, 28}, "←") && f->history_len)
        navigate(w, f, f->history[--f->history_len], false);
    if (ui_button(ui, (struct rect){42, 8, 30, 28}, "↑"))
        navigate(w, f, "..", true);
    if (ui_tool(ui, (struct rect){76, 8, 30, 28}, ICON_HOME, NULL, false))
        navigate(w, f, "/home", true);
    if (ui_textbox(ui, (struct rect){114, 7, W - 122, 30}, &f->location)) {
        char target[PATH_MAX_LEN];
        str_copy(target, f->location.buf, sizeof(target));
        navigate(w, f, target, true);
    }

    struct rect side = {0, TOOLBAR_H, SIDEBAR_W, H - TOOLBAR_H - STATUS_H};
    ui_panel(ui, side, color_mix(style.window, style.panel, 128));
    for (int i = 0; i < (int)ARRAY_SIZE(places); i++) {
        struct rect r = {6, side.y + 8 + i * 30, SIDEBAR_W - 12, 28};
        if (ui_tool(ui, r, places[i].icon, places[i].label, strcmp(f->path, places[i].path) == 0))
            navigate(w, f, places[i].path, true);
    }

    struct rect list = {SIDEBAR_W + 4, TOOLBAR_H + HEADER_H, W - SIDEBAR_W - 8, H - TOOLBAR_H - HEADER_H - STATUS_H};
    if (ui->c) {
        int name_w = list.w - 12 - 230;
        gfx_text_box(ui->c, (struct rect){list.x + 32, TOOLBAR_H, 100, HEADER_H}, &font_bold, "Name", style.muted,
                     ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){list.x + name_w, TOOLBAR_H, 80, HEADER_H}, &font_bold, "Size", style.muted,
                     ALIGN_RIGHT);
        gfx_text_box(ui->c, (struct rect){list.x + name_w + 100, TOOLBAR_H, 100, HEADER_H}, &font_bold, "Type",
                     style.muted, ALIGN_LEFT);
        gfx_fill(ui->c, (struct rect){list.x, TOOLBAR_H + HEADER_H - 1, list.w, 1}, style.border);
    }
    f->list.row_h = ROW_H;
    if (!ui->st->focus)
        ui->st->focus = ui->id + 1;
    int result = ui_list(ui, list, &f->list, draw_row, f);
    if (result == LIST_ACTIVATE && selected(f))
        open_entry(w, f, selected(f));
    if (result == LIST_CONTEXT && ui->e) {
        if (selected(f))
            ui_menu(w, ui->e->x, ui->e->y, item_menu, (int)ARRAY_SIZE(item_menu), pick_item);
        else
            ui_menu(w, ui->e->x, ui->e->y, space_menu, (int)ARRAY_SIZE(space_menu), pick_space);
        return;
    }

    int key = ui_key(ui);
    if (key == KEY_BACKSPACE)
        navigate(w, f, "..", true);
    else if (KEY_BASE(key) == KEY_DELETE)
        ask_delete(w, f);
    else if (KEY_BASE(key) == KEY_F2 && selected(f))
        ui_prompt(w, "Rename", "New name:", selected(f)->name, do_rename);
    else if (KEY_BASE(key) == KEY_F5)
        load(f);
    else if (key == KEY_CTRL('c') || key == KEY_CTRL('x')) {
        if (selected(f)) {
            child_path(f, selected(f)->name, file_clip);
            file_clip_cut = key == KEY_CTRL('x');
        }
    } else if (key == KEY_CTRL('v'))
        paste(w, f);

    if (ui->c) {
        struct rect status = {0, H - STATUS_H, W, STATUS_H};
        gfx_fill(ui->c, status, style.panel);
        char text[128];
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
        ksnprintf(text, sizeof(text), "%d items%s%s", f->count, total ? "    free space: " : "", total ? free_text : "");
        gfx_text_box(ui->c, (struct rect){10, status.y, W - 20, STATUS_H}, &font_sans, text, style.muted, ALIGN_LEFT);
    }
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
    kfree(f->entries);
    kfree(f);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    struct files *f = kzalloc(sizeof(*f));
    if (!f)
        return;
    str_copy(f->path, "/", sizeof(f->path));
    struct window *w = wm_open("Files", ICON_FOLDER, 720, 460, &ops, f, WIN_RESIZABLE);
    if (!w) {
        kfree(f);
        return;
    }
    w->min_w = 420;
    w->min_h = 260;
    navigate(w, f, arg && *arg ? arg : "/home", false);
}

static const struct gui_app files_app = {"Files", "Browse folders and open files", ICON_FOLDER, launch, NULL};
GUI_APP(files_app);
