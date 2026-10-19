#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* The open and save panel: places on the left, the folder on the right, a name field when saving. Windows programs
   reach it through IFileOpenDialog/IFileSaveDialog and GetOpenFileName/GetSaveFileName. */

#define PICK_W 700
#define PICK_H 460
#define SIDE_W 176
#define TOP_H 48
#define BOTTOM_H 56
#define ROW_H 26
#define MAX_ENTRIES 2048

struct pick_entry {
    char name[NAME_MAX_LEN];
    bool dir, allowed;
    uint32_t size;
};

struct picker {
    struct ui_state st;
    bool save, folders, finished;
    char title[80], exts[200], button[32];
    char dir[PATH_MAX_LEN];
    char history[16][PATH_MAX_LEN];
    int depth;
    struct pick_entry *entries;
    int count;
    struct ui_list list;
    struct ui_text name;
    gui_picker_fn done;
    void *ctx;
};

struct place {
    const char *label, *sub;
    enum symbol_id sym;
};

static const struct place places[] = {
    {"Home", "", SYM_HOME},          {"Desktop", "Desktop", SYM_DESKTOP}, {"Documents", "Documents", SYM_DOC},
    {"Downloads", "Downloads", SYM_DOWNLOAD}, {"Pictures", "Pictures", SYM_PHOTO},
};

static bool allowed(const struct picker *p, const char *name)
{
    if (!p->exts[0])
        return true;
    const char *ext = path_ext(name);
    if (!ext || !*ext)
        return false;
    size_t n = strlen(ext);
    for (const char *s = p->exts; *s;) {
        const char *end = strchr(s, ';');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len == n && !str_ncasecmp(s, ext, n))
            return true;
        if (len == 1 && *s == '*')
            return true;
        s += len + (end ? 1 : 0);
    }
    return false;
}

static bool before(const struct pick_entry *a, const struct pick_entry *b)
{
    if (a->dir != b->dir)
        return a->dir;
    return str_casecmp(a->name, b->name) < 0;
}

static void load(struct picker *p)
{
    p->count = 0;
    if (!p->entries && !(p->entries = kmalloc(sizeof(struct pick_entry) * MAX_ENTRIES)))
        return;
    struct vdir d;
    if (vfs_opendir(p->dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (p->count < MAX_ENTRIES && vfs_readdir(&d, &ent) == 0) {
        if (ent.name[0] == '.')
            continue;
        struct pick_entry e = {.dir = ent.type == VN_DIR, .size = ent.size};
        str_copy(e.name, ent.name, sizeof(e.name));
        e.allowed = e.dir || (!p->folders && allowed(p, e.name));
        int i = p->count++;
        while (i > 0 && before(&e, &p->entries[i - 1])) {
            p->entries[i] = p->entries[i - 1];
            i--;
        }
        p->entries[i] = e;
    }
    vfs_closedir(&d);
}

static void go(struct picker *p, const char *path, bool remember)
{
    if (!vfs_is_dir(path) || !strcmp(path, p->dir))
        return;
    if (remember && p->dir[0]) {
        if (p->depth == (int)ARRAY_SIZE(p->history)) {
            memmove(p->history[0], p->history[1], sizeof(p->history[0]) * (ARRAY_SIZE(p->history) - 1));
            p->depth--;
        }
        str_copy(p->history[p->depth++], p->dir, PATH_MAX_LEN);
    }
    str_copy(p->dir, path, sizeof(p->dir));
    load(p);
    p->list.selected = -1;
    p->list.scroll = 0;
}

static void finish(struct window *w, struct picker *p, const char *path)
{
    p->finished = true;
    gui_picker_fn done = p->done;
    void *ctx = p->ctx;
    wm_destroy(w);
    if (done)
        done(ctx, path);
}

static const struct pick_entry *selection(const struct picker *p)
{
    return p->list.selected >= 0 && p->list.selected < p->count ? &p->entries[p->list.selected] : NULL;
}

static void draw_row(struct canvas *c, struct rect row, int index, bool selected, void *ctx)
{
    struct picker *p = ctx;
    const struct pick_entry *e = &p->entries[index];
    char path[PATH_MAX_LEN];
    path_join(p->dir, e->name, path, sizeof(path));
    gui_draw_file_icon(c, path, (struct rect){row.x + 10, row.y + 3, 20, 20}, e->allowed ? 255 : 110);
    uint32_t text = selected ? ui_row_text(true) : e->allowed ? style.text : style.tertiary;
    int size_w = 90;
    gfx_text_box(c, (struct rect){row.x + 38, row.y, row.w - 48 - size_w, row.h}, &font_sans, e->name, text,
                 ALIGN_LEFT);
    if (!e->dir) {
        char size[16];
        fmt_size(size, sizeof(size), e->size);
        gfx_text_box(c, (struct rect){row.x + row.w - size_w - 12, row.y, size_w, row.h}, &font_small, size,
                     selected ? text : style.secondary, ALIGN_RIGHT);
    } else {
        gfx_symbol(c, row.x + row.w - 24, row.y + (row.h - 12) / 2, SYM_RIGHT, 12, selected ? text : style.tertiary);
    }
}

static void submit(struct window *w, struct picker *p)
{
    const struct pick_entry *e = selection(p);
    char path[PATH_MAX_LEN];
    if (p->save) {
        if (e && e->dir && !p->name.len) {
            path_join(p->dir, e->name, path, sizeof(path));
            go(p, path, true);
            return;
        }
        if (!p->name.len)
            return;
        path_join(p->dir, p->name.buf, path, sizeof(path));
        finish(w, p, path);
        return;
    }
    if (p->folders) {
        if (e && e->dir)
            path_join(p->dir, e->name, path, sizeof(path));
        else
            str_copy(path, p->dir, sizeof(path));
        finish(w, p, path);
        return;
    }
    if (!e)
        return;
    path_join(p->dir, e->name, path, sizeof(path));
    if (e->dir)
        go(p, path, true);
    else if (e->allowed)
        finish(w, p, path);
}

static void frame(struct ui *ui, void *data)
{
    struct picker *p = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, style.content);
    ui_sidebar_panel(ui, SIDE_W, H, style.content);
    int y = 20;
    ui_sidebar_header(ui, (struct rect){24, y, SIDE_W - 34, 20}, "Favorites");
    y += 22;
    char path[PATH_MAX_LEN];
    for (size_t i = 0; i < ARRAY_SIZE(places); i++) {
        path_join(user_home(), places[i].sub, path, sizeof(path));
        if (ui_sidebar_item(ui, (struct rect){14, y, SIDE_W - 26, 28}, places[i].sym, places[i].label,
                            !strcmp(p->dir, path)))
            go(p, path, true);
        y += 28;
    }
    struct volume volumes[6];
    int n = vfs_volumes(volumes, (int)ARRAY_SIZE(volumes));
    y += 10;
    ui_sidebar_header(ui, (struct rect){24, y, SIDE_W - 34, 20}, "Locations");
    y += 22;
    if (ui_sidebar_item(ui, (struct rect){14, y, SIDE_W - 26, 28}, SYM_DISK, "SynthOS HD", !strcmp(p->dir, "/")))
        go(p, "/", true);
    y += 28;
    for (int i = 0; i < n && y < H - 40; i++, y += 28)
        if (ui_sidebar_item(ui, (struct rect){14, y, SIDE_W - 26, 28}, SYM_USB, volumes[i].label,
                            !strcmp(p->dir, volumes[i].path)))
            go(p, volumes[i].path, true);

    int x = SIDE_W + 8, cw = W - x - 16;
    if (ui_symbol_button(ui, (struct rect){x, 10, 28, 26}, SYM_LEFT, false) && p->depth > 0) {
        char back[PATH_MAX_LEN];
        str_copy(back, p->history[--p->depth], sizeof(back));
        go(p, back, false);
    }
    if (ui_symbol_button(ui, (struct rect){x + 30, 10, 28, 26}, SYM_UP, false) && strcmp(p->dir, "/")) {
        path_dirname(p->dir, path, sizeof(path));
        go(p, path, true);
    }
    const char *here = !strcmp(p->dir, user_home()) ? "Home" : !strcmp(p->dir, "/") ? "SynthOS HD" : path_basename(p->dir);
    if (ui->c)
        gfx_text_box(ui->c, (struct rect){x + 68, 10, cw - 68, 26}, &font_bold, here, style.text, ALIGN_LEFT);
    int list_y = TOP_H, list_h = H - TOP_H - BOTTOM_H;
    if (p->save) {
        if (ui->c)
            gfx_text_box(ui->c, (struct rect){x + 4, TOP_H, 70, 28}, &font_sans, "Save As:", style.text, ALIGN_LEFT);
        if (!ui->st->focus)
            ui->st->focus = ui->id + 1;
        if (ui_textbox(ui, (struct rect){x + 76, TOP_H, cw - 76, 28}, &p->name))
            submit(w, p);
        if (!wm_window_alive(w) || p->finished)
            return;
        list_y += 38;
        list_h -= 38;
    }
    p->list.count = p->count;
    p->list.row_h = ROW_H;
    struct rect lr = {x, list_y, cw, list_h};
    if (ui->c) {
        gfx_round(ui->c, lr, 10, style.group);
        gfx_round_frame(ui->c, lr, 10, style.dark ? ALPHA(0xffffffffu, 16) : ALPHA(0, 18));
    }
    int r = ui_list(ui, rect_inset(lr, 4), &p->list, draw_row, p);
    const struct pick_entry *e = selection(p);
    if (r == LIST_SELECT && p->save && e && !e->dir)
        ui_text_set(&p->name, e->name);
    if (r == LIST_ACTIVATE && e) {
        submit(w, p);
        if (!wm_window_alive(w) || p->finished)
            return;
    }
    if (ui->c && !p->count)
        gfx_text_box(ui->c, lr, &font_sans, "This folder is empty", style.tertiary, ALIGN_CENTER);

    int bw = 96, by = H - BOTTOM_H + 14;
    bool cancel = ui_button(ui, (struct rect){W - 16 - bw * 2 - 10, by, bw, 28}, "Cancel");
    bool ok = ui_button_primary(ui, (struct rect){W - 16 - bw, by, bw, 28}, p->button);
    if (p->folders && ui_button(ui, (struct rect){x, by, 110, 28}, "New Folder")) {
        path_join(p->dir, "untitled folder", path, sizeof(path));
        for (int i = 2; vfs_exists(path) && i < 100; i++) {
            char name[40];
            ksnprintf(name, sizeof(name), "untitled folder %d", i);
            path_join(p->dir, name, path, sizeof(path));
        }
        vfs_mkdir(path);
        load(p);
    }
    int key = ui_key(ui);
    if (key == KEY_ESC)
        cancel = true;
    else if (key == KEY_ENTER || key == '\r')
        ok = true;
    else if (key == KEY_BACKSPACE && !p->save && strcmp(p->dir, "/")) {
        path_dirname(p->dir, path, sizeof(path));
        go(p, path, true);
    }
    if (cancel)
        finish(w, p, NULL);
    else if (ok)
        submit(w, p);
}

static void picker_paint(struct window *w, struct canvas *c)
{
    struct picker *p = w->data;
    ui_run(w, &p->st, c, NULL, frame, p);
}

static void picker_event(struct window *w, struct gui_event *e)
{
    struct picker *p = w->data;
    ui_run(w, &p->st, NULL, e, frame, p);
}

static void picker_destroy(struct window *w)
{
    struct picker *p = w->data;
    if (!p->finished && p->done)
        p->done(p->ctx, NULL);
    kfree(p->entries);
    kfree(p);
}

static const struct window_ops picker_ops = {picker_paint, picker_event, NULL, picker_destroy};

struct window *gui_file_picker(struct window *owner, const struct gui_picker *o, gui_picker_fn done, void *ctx)
{
    struct picker *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    p->save = o->save;
    p->folders = o->folders;
    str_copy(p->title, o->title ? o->title : p->save ? "Save" : p->folders ? "Choose a Folder" : "Open",
             sizeof(p->title));
    str_copy(p->exts, o->exts ? o->exts : "", sizeof(p->exts));
    str_copy(p->button, o->button ? o->button : p->save ? "Save" : p->folders ? "Choose" : "Open", sizeof(p->button));
    p->done = done;
    p->ctx = ctx;
    p->list.selected = -1;
    if (o->name)
        ui_text_set(&p->name, o->name);
    char start[PATH_MAX_LEN];
    if (o->dir && vfs_is_dir(o->dir))
        str_copy(start, o->dir, sizeof(start));
    else
        path_join(user_home(), p->save ? "Downloads" : "", start, sizeof(start));
    go(p, vfs_is_dir(start) ? start : user_home(), false);
    if (owner && wm_window_alive(owner))
        wm_focus(owner);
    struct window *w = wm_open(p->title, ICON_FOLDER, PICK_W, PICK_H, &picker_ops, p, WIN_DIALOG);
    if (!w) {
        kfree(p->entries);
        kfree(p);
        return NULL;
    }
    if (owner && wm_window_alive(owner)) {
        w->owner = owner;
        w->app = owner->app;
    }
    return w;
}
