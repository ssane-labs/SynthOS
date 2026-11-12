#include <synth/path.h>
#include <synth/vfs.h>

#include "systool.h"

/* Disk Utility: every mounted volume with its use, opened in Files or ejected, and the disks with their
   partitions as the 'part' command reports them. */

#define MAX_VOLS 12

struct vol {
    char path[PATH_MAX_LEN], source[48], fs[16], label[48];
    uint64_t total, free;
    bool removable;
};

struct diskutil {
    struct ui_state st;
    struct vol vols[MAX_VOLS];
    int count, selected;
    struct tool_text view;
};

static struct window *open_win;

static void scan(struct diskutil *d)
{
    struct volume removable[8];
    int nr = vfs_volumes(removable, 8);
    d->count = 0;
    for (struct mount *mt = vfs_mounts(); mt && d->count < MAX_VOLS; mt = mt->next) {
        uint64_t t = 0, f = 0;
        if (!mt->fs->statfs || mt->fs->statfs(mt->root, &t, &f) != 0 || !t || !strcmp(mt->fs->name, "devfs"))
            continue;
        struct vol *v = &d->vols[d->count++];
        str_copy(v->path, mt->path, sizeof(v->path));
        str_copy(v->source, mt->source[0] ? mt->source : "memory", sizeof(v->source));
        str_copy(v->fs, mt->fs->name, sizeof(v->fs));
        v->total = t;
        v->free = f;
        v->removable = false;
        const char *name = !strcmp(mt->path, "/") ? "System (in memory)" : !strcmp(mt->path, "/home") ? "SynthOS HD"
                                                                                                      : path_basename(mt->path);
        str_copy(v->label, name, sizeof(v->label));
        for (int i = 0; i < nr; i++)
            if (!strcmp(removable[i].path, mt->path)) {
                str_copy(v->label, removable[i].label, sizeof(v->label));
                v->removable = true;
            }
    }
    d->selected = CLAMP(d->selected, 0, d->count); /* count = the Disks page */
}

static void legend(struct ui *ui, int x, int y, uint32_t color, const char *name, uint64_t bytes)
{
    char size[16];
    fmt_size(size, sizeof(size), bytes);
    if (ui->c)
        gfx_round(ui->c, (struct rect){x, y + 4, 12, 12}, 3, color);
    ui_label(ui, (struct rect){x + 20, y, 70, 20}, name, &font_small, style.secondary, ALIGN_LEFT);
    ui_label(ui, (struct rect){x + 20, y + 18, 120, 22}, size, &font_bold, style.text, ALIGN_LEFT);
}

static void volume(struct ui *ui, struct diskutil *d, struct rect area)
{
    struct vol *v = &d->vols[d->selected];
    int x = area.x + 28, w = area.w - 56, y = area.y + 22;
    if (ui->c)
        gfx_icon(ui->c, x, y, v->removable ? ICON_USB : ICON_DISKUTIL, 64);
    ui_label(ui, (struct rect){x + 82, y + 4, w - 82, 30}, v->label, &font_title, style.text, ALIGN_LEFT);
    char line[128], total[16];
    fmt_size(total, sizeof(total), v->total);
    ksnprintf(line, sizeof(line), "%s volume  -  %s  -  mounted at %s", v->fs, total, v->path);
    ui_label(ui, (struct rect){x + 82, y + 36, w - 82, 20}, line, &font_sans, style.secondary, ALIGN_LEFT);
    y += 96;
    uint64_t used = v->total - v->free;
    struct rect bar = {x, y, w, 22};
    if (ui->c) {
        gfx_round(ui->c, bar, 7, style.dark ? RGB(64, 64, 70) : RGB(222, 222, 228));
        int uw = (int)((uint64_t)bar.w * used / v->total);
        if (uw > 0)
            gfx_round_gradient(ui->c, (struct rect){bar.x, bar.y, MAX(uw, 14), bar.h}, 7, RGB(90, 170, 255),
                               RGB(30, 110, 230));
    }
    y += 36;
    legend(ui, x, y, RGB(60, 140, 245), "Used", used);
    legend(ui, x + 160, y, style.dark ? RGB(64, 64, 70) : RGB(222, 222, 228), "Free", v->free);
    legend(ui, x + 320, y, style.separator, "Capacity", v->total);
    y += 64;
    struct rect info = {x, y, w, 4 * 34};
    if (ui->c)
        gfx_round(ui->c, info, 12, style.group);
    static const char *const keys[] = {"Mount Point", "Device", "Format", "Kind"};
    const char *vals[] = {v->path, v->source, v->fs, v->removable ? "Removable" : "Internal"};
    for (int i = 0; i < 4; i++) {
        struct rect row = {x + 16, y + i * 34, w - 32, 34};
        ui_label(ui, row, keys[i], &font_sans, style.secondary, ALIGN_LEFT);
        ui_label(ui, row, vals[i], &font_sans, style.text, ALIGN_RIGHT);
        if (i && ui->c)
            gfx_fill(ui->c, (struct rect){x + 16, row.y, w - 32, 1}, style.separator);
    }
    y += info.h + 18;
    if (ui_button(ui, (struct rect){x, y, 130, 28}, "Open in Files"))
        tool_launch("Files", v->path);
    if (v->removable && ui_button(ui, (struct rect){x + 140, y, 100, 28}, "Eject")) {
        gui_eject(v->path);
        scan(d);
    }
}

static void frame(struct ui *ui, void *data)
{
    struct diskutil *d = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->e && ui->e->type == EV_TIMER) {
        scan(d);
        ui->consumed = true;
    }
    struct tool_item items[MAX_VOLS + 1];
    int n = 0;
    for (int i = 0; i < d->count; i++)
        items[n++] = (struct tool_item){d->vols[i].removable ? SYM_USB : SYM_DISK, d->vols[i].label,
                                        i ? NULL : "Volumes"};
    items[n++] = (struct tool_item){SYM_CHIP, "Disks & Partitions", "Devices"};
    if (tool_sidebar(ui, H, items, n, &d->selected) && d->selected == d->count) {
        tool_text_set(&d->view, tool_run("part"));
        d->view.scroll = 0;
    }
    bool disks = d->selected >= d->count;
    struct rect area = tool_page(ui, W, H, disks ? "Disks & Partitions" : d->vols[d->selected].label,
                                 disks ? "from the 'part' command" : NULL);
    if (ui_symbol_button(ui, (struct rect){W - 76, 12, 28, 28}, SYM_RESTART, false)) {
        scan(d);
        if (disks)
            tool_text_set(&d->view, tool_run("part"));
    }
    if (disks)
        tool_text_view(ui, rect_inset(area, 12), &d->view, "");
    else
        volume(ui, d, area);
}

static void paint(struct window *w, struct canvas *c)
{
    struct diskutil *d = w->data;
    ui_run(w, &d->st, c, NULL, frame, d);
}

static void event(struct window *w, struct gui_event *e)
{
    struct diskutil *d = w->data;
    ui_run(w, &d->st, NULL, e, frame, d);
}

static void destroy(struct window *w)
{
    struct diskutil *d = w->data;
    if (open_win == w)
        open_win = NULL;
    kfree(d->view.text);
    kfree(d);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct diskutil *d = kzalloc(sizeof(*d));
    if (!d)
        return;
    scan(d);
    for (int i = 0; i < d->count; i++)
        if (!strcmp(d->vols[i].path, "/home"))
            d->selected = i;
    open_win = tool_open("Disk Utility", ICON_DISKUTIL, 820, 560, &ops, d);
    if (!open_win) {
        kfree(d);
        return;
    }
    wm_set_timer(open_win, 3000);
}

static const struct gui_app diskutil_app = {
    .name = "Disk Utility", .description = "Volumes, their use, disks and partitions", .icon = ICON_DISKUTIL,
    .launch = launch};
GUI_APP(diskutil_app);
