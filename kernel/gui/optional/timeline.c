#include <synth/timeline.h>
#include <synth/versions.h>
#include <synth/vfs.h>

#include "systool.h"

/* Timeline: the apps, files and web pages of the last days, newest first, searchable; a click opens the thing
   again. A saved file shows its earlier versions (Rewind), and one click brings a version back. */

#define ROW_H 40
#define MAX_ENTRIES 600

struct entry {
    uint32_t time;
    char kind;
    char title[64];
    char target[160];
};

static const struct tool_item items[] = {
    {SYM_CLOCK, "Today", "Time"},  {SYM_CALENDAR, "Yesterday", NULL}, {SYM_CALENDAR, "Last 7 Days", NULL},
    {SYM_LIST, "Everything", NULL}, {SYM_APPS, "Apps", "Kinds"},       {SYM_DOC, "Files", NULL},
    {SYM_PENCIL, "Saved", NULL},   {SYM_GLOBE, "Web", NULL},
};
#define ITEMS ((int)ARRAY_SIZE(items))

struct timeline {
    struct ui_state st;
    struct ui_text find;
    struct entry *list;
    int count, page, scroll;
    char versions_of[PATH_MAX_LEN];
    uint32_t vtimes[24], vsizes[24];
    int vcount;
    char note[96];
};

static struct window *open_win;

static void reload(struct timeline *t)
{
    t->count = 0;
    char *data;
    size_t size;
    if (!t->list || vfs_read_all(TIMELINE_FILE, &data, &size))
        return;
    for (char *line = data; line < data + size;) {
        char *end = memchr(line, '\n', (size_t)(data + size - line));
        if (!end)
            break;
        *end = 0;
        char *f[4] = {line};
        for (int i = 1; i < 4 && f[i - 1]; i++) {
            f[i] = strchr(f[i - 1], '\t');
            if (f[i])
                *f[i]++ = 0;
        }
        if (f[3]) {
            if (t->count == MAX_ENTRIES) {
                memmove(t->list, t->list + 1, sizeof(*t->list) * (MAX_ENTRIES - 1));
                t->count--;
            }
            struct entry *e = &t->list[t->count++];
            e->time = (uint32_t)str_to_long(f[0], NULL, 10);
            e->kind = f[1][0];
            str_copy(e->title, f[2], sizeof(e->title));
            str_copy(e->target, f[3], sizeof(e->target));
        }
        line = end + 1;
    }
    kfree(data);
}

static bool shown(struct timeline *t, const struct entry *e)
{
    uint32_t zone = (uint32_t)(time_zone() * 60), now = time_now() + zone, day = now - now % 86400, at = e->time + zone;
    switch (t->page) {
    case 0: if (at < day) return false; break;
    case 1: if (at < day - 86400 || at >= day) return false; break;
    case 2: if (at < day - 6 * 86400) return false; break;
    case 4: if (e->kind != 'a') return false; break;
    case 5: if (e->kind != 'f') return false; break;
    case 6: if (e->kind != 's') return false; break;
    case 7: if (e->kind != 'w') return false; break;
    default: break;
    }
    return tool_has(e->title, strlen(e->title), t->find.buf) || tool_has(e->target, strlen(e->target), t->find.buf);
}

static void reopen(const struct entry *e)
{
    if (e->kind == 'a')
        gui_app_launch(gui_app_find(e->target), NULL);
    else if (e->kind == 'w')
        gui_open_url(e->target);
    else if (vfs_exists(e->target))
        gui_open_file(e->target);
}

static void show_versions(struct timeline *t, const char *path)
{
    str_copy(t->versions_of, path, sizeof(t->versions_of));
    t->vcount = versions_list(path, t->vtimes, t->vsizes, 24);
    t->note[0] = 0;
}

static void versions_view(struct ui *ui, struct timeline *t, struct rect r)
{
    char head[PATH_MAX_LEN + 32];
    ksnprintf(head, sizeof(head), "Earlier versions of %s", path_basename(t->versions_of));
    ui_label(ui, (struct rect){r.x + 20, r.y + 14, r.w - 140, 22}, head, &font_title, style.text, ALIGN_LEFT);
    if (ui_button(ui, (struct rect){r.x + r.w - 110, r.y + 12, 90, 26}, "Done"))
        t->versions_of[0] = 0;
    ui_label(ui, (struct rect){r.x + 20, r.y + 40, r.w - 40, 18}, t->note[0] ? t->note : t->versions_of, &font_small,
             style.secondary, ALIGN_LEFT);
    if (!t->vcount)
        ui_label(ui, (struct rect){r.x, r.y + 120, r.w, 20}, "No earlier versions are kept for this file.", &font_sans,
                 style.tertiary, ALIGN_CENTER);
    int y = r.y + 70;
    for (int i = 0; i < t->vcount && y + ROW_H < r.y + r.h; i++, y += ROW_H) {
        char when[24], size[16], line[64];
        time_format(t->vtimes[i] + (uint32_t)(time_zone() * 60), when, sizeof(when));
        fmt_size(size, sizeof(size), t->vsizes[i]);
        ksnprintf(line, sizeof(line), "%s   %s", when, size);
        struct rect row = {r.x + 16, y, r.w - 32, ROW_H - 4};
        if (ui->c)
            gfx_round(ui->c, row, 8, i % 2 ? style.content : style.group);
        if (ui->c)
            gfx_symbol(ui->c, row.x + 10, row.y + 8, SYM_UNDO, 20, style.accent);
        ui_label(ui, (struct rect){row.x + 40, row.y, row.w - 160, row.h}, line, &font_sans, style.text, ALIGN_LEFT);
        if (ui_button(ui, (struct rect){row.x + row.w - 100, row.y + 6, 90, 24}, "Restore")) {
            int err = versions_restore(t->versions_of, t->vtimes[i]);
            show_versions(t, t->versions_of);
            str_copy(t->note, err ? "Could not restore it" : "Restored; the replaced content is kept as a version",
                     sizeof(t->note));
        }
    }
}

static void list_view(struct ui *ui, struct timeline *t, struct rect r)
{
    int shown_n = 0;
    for (int i = 0; i < t->count; i++)
        shown_n += shown(t, &t->list[i]);
    int rows = MAX(1, (r.h - 8) / ROW_H);
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(r, ui->e->x, ui->e->y)) {
        t->scroll -= ui->e->wheel * 2;
        ui->consumed = true;
    }
    t->scroll = CLAMP(t->scroll, 0, MAX(0, shown_n - rows));
    if (!shown_n) {
        ui_label(ui, r, t->count ? "Nothing here" : "What you open, save and visit shows up here", &font_sans,
                 style.tertiary, ALIGN_CENTER);
        return;
    }
    int index = 0, y = r.y + 4;
    uint32_t zone = (uint32_t)(time_zone() * 60);
    for (int i = t->count - 1; i >= 0 && y + ROW_H <= r.y + r.h; i--) {
        const struct entry *e = &t->list[i];
        if (!shown(t, e) || index++ < t->scroll)
            continue;
        struct rect row = {r.x + 12, y, r.w - 24, ROW_H - 2};
        bool hot = ui_hover(ui, row);
        char when[24];
        time_format(e->time + zone, when, sizeof(when));
        if (ui->c) {
            if (hot)
                gfx_round(ui->c, row, 8, style.hover);
            enum symbol_id sym = e->kind == 'a' ? SYM_APPS : e->kind == 'w' ? SYM_GLOBE : e->kind == 's' ? SYM_PENCIL
                                                                                                       : SYM_DOC;
            const struct gui_app *app = e->kind == 'a' ? gui_app_find(e->target) : NULL;
            if (app)
                gfx_icon(ui->c, row.x + 8, row.y + 5, app->icon, 28);
            else
                gfx_symbol(ui->c, row.x + 12, row.y + 9, sym, 20, style.accent);
            gfx_text_box(ui->c, (struct rect){row.x + 46, row.y + 3, row.w - 200, 18}, &font_bold, e->title,
                         style.text, ALIGN_LEFT);
            const char *what = e->kind == 'a' ? "App" : e->kind == 's' ? "Saved" : e->kind == 'w' ? "Web page" : "Opened";
            char sub[200];
            ksnprintf(sub, sizeof(sub), "%s  %s", what, e->kind == 'a' ? "" : e->target);
            gfx_text_box(ui->c, (struct rect){row.x + 46, row.y + 20, row.w - 200, 16}, &font_small, sub,
                         style.secondary, ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){row.x + row.w - 150, row.y, 140, row.h}, &font_small,
                         t->page < 2 ? when + 11 : when, style.tertiary, ALIGN_RIGHT);
        }
        if (e->kind == 's' && hot &&
            ui_button(ui, (struct rect){row.x + row.w - 250, row.y + 7, 90, 24}, "Versions")) {
            show_versions(t, e->target);
        } else if (ui->e && ui->e->type == EV_MOUSE_UP && hot && !ui->consumed) {
            ui->consumed = true;
            reopen(e);
        }
        y += ROW_H;
    }
}

static void frame(struct ui *ui, void *data)
{
    struct timeline *t = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (tool_sidebar(ui, H, items, ITEMS, &t->page))
        t->scroll = 0;
    struct rect r = tool_page(ui, W, H, items[t->page].label, "Everything you opened, saved and visited");
    if (ui_search(ui, (struct rect){W - 230, (TOOLBAR_HEIGHT - 28) / 2, 210, 28}, &t->find, "Search"))
        t->scroll = 0;
    if (t->versions_of[0])
        versions_view(ui, t, r);
    else
        list_view(ui, t, r);
}

static void paint(struct window *w, struct canvas *c)
{
    struct timeline *t = w->data;
    ui_run(w, &t->st, c, NULL, frame, t);
}

static void event(struct window *w, struct gui_event *e)
{
    struct timeline *t = w->data;
    if (e->type == EV_TIMER) {
        reload(t);
        wm_invalidate(w);
        return;
    }
    ui_run(w, &t->st, NULL, e, frame, t);
}

static void destroy(struct window *w)
{
    struct timeline *t = w->data;
    kfree(t->list);
    kfree(t);
    if (open_win == w)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct timeline *t = kzalloc(sizeof(*t));
    if (!t)
        return;
    t->list = kmalloc(sizeof(*t->list) * MAX_ENTRIES);
    reload(t);
    if (arg && vfs_exists(arg))
        show_versions(t, arg);
    open_win = tool_open("Timeline", ICON_TIMELINE, 820, 560, &ops, t);
    if (!open_win) {
        kfree(t->list);
        kfree(t);
        return;
    }
    wm_set_timer(open_win, 5000);
}

static const struct gui_app timeline_app = {
    .name = "Timeline", .description = "Everything you opened, saved and visited, and earlier versions of your files",
    .icon = ICON_TIMELINE, .launch = launch};
GUI_APP(timeline_app);
