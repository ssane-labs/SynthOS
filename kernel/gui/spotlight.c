#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

#define SPOT_W 680
#define SPOT_BAR 58
#define ROW_H 40
#define MAX_RESULTS 9

enum result_kind {
    R_APP,
    R_FILE,
    R_RUN,
};

struct result {
    enum result_kind kind;
    const struct gui_app *app;
    char path[PATH_MAX_LEN];
    char label[80];
    char detail[96];
    enum icon_id icon;
};

struct spot {
    struct ui_state st;
    struct ui_text query;
    char last[256];
    struct result res[MAX_RESULTS];
    int count;
    int selected;
};

static struct window *spot_win, *launch_win, *control_win;

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return false;
    for (const char *p = hay; *p; p++)
        if (!str_ncasecmp(p, needle, n))
            return true;
    return false;
}

struct app_search {
    struct spot *s;
};

static void match_app(const struct gui_app *app, void *ctx)
{
    struct spot *s = ctx;
    if (s->count >= MAX_RESULTS - 1 || !contains_ci(app->name, s->query.buf))
        return;
    struct result *r = &s->res[s->count++];
    *r = (struct result){.kind = R_APP, .app = app, .icon = app->icon};
    str_copy(r->label, app->name, sizeof(r->label));
    str_copy(r->detail, "Application", sizeof(r->detail));
}

static enum icon_id file_icon(const char *name, bool dir)
{
    if (dir)
        return ICON_FOLDER;
    const char *ext = path_ext(name);
    if (ext && !str_casecmp(ext, ".exe"))
        return ICON_APP;
    if (picture_is_image(name))
        return ICON_IMAGE;
    if (ext && (!str_casecmp(ext, ".sh") || !str_casecmp(ext, ".bat")))
        return ICON_SCRIPT;
    if (ext && !str_casecmp(ext, ".com"))
        return ICON_DOS;
    return ICON_TEXT;
}

static void search_dir(struct spot *s, const char *dir, int depth)
{
    if (depth > 4 || s->count >= MAX_RESULTS - 1)
        return;
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    char subdirs[16][NAME_MAX_LEN];
    int nsub = 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0 && s->count < MAX_RESULTS - 1) {
        if (ent.name[0] == '.')
            continue;
        bool is_dir = ent.type == VN_DIR;
        if (contains_ci(ent.name, s->query.buf)) {
            struct result *r = &s->res[s->count++];
            *r = (struct result){.kind = R_FILE, .icon = file_icon(ent.name, is_dir)};
            path_join(dir, ent.name, r->path, sizeof(r->path));
            str_copy(r->label, ent.name, sizeof(r->label));
            str_copy(r->detail, dir, sizeof(r->detail));
        }
        if (is_dir && nsub < 16)
            str_copy(subdirs[nsub++], ent.name, NAME_MAX_LEN);
    }
    vfs_closedir(&d);
    for (int i = 0; i < nsub; i++) {
        char sub[PATH_MAX_LEN];
        path_join(dir, subdirs[i], sub, sizeof(sub));
        search_dir(s, sub, depth + 1);
    }
}

static void update(struct spot *s)
{
    if (!strcmp(s->last, s->query.buf))
        return;
    str_copy(s->last, s->query.buf, sizeof(s->last));
    s->count = 0;
    s->selected = 0;
    if (!s->query.len)
        return;
    gui_app_foreach(match_app, s);
    search_dir(s, "/home", 0);
    search_dir(s, "/apps", 0);
    search_dir(s, "/docs", 0);
    struct result *r = &s->res[s->count++];
    *r = (struct result){.kind = R_RUN, .icon = ICON_TERMINAL};
    ksnprintf(r->label, sizeof(r->label), "Run \"%s\"", s->query.buf);
    str_copy(r->detail, "Terminal", sizeof(r->detail));
}

static int spot_height(struct spot *s)
{
    return SPOT_BAR + (s->count ? s->count * ROW_H + 14 : 0);
}

static void open_result(struct spot *s, int i)
{
    if (i < 0 || i >= s->count)
        return;
    struct result r = s->res[i];
    char query[256];
    str_copy(query, s->query.buf, sizeof(query));
    wm_destroy(spot_win);
    if (r.kind == R_APP)
        gui_app_launch(r.app, NULL);
    else if (r.kind == R_FILE)
        gui_open_file(r.path);
    else
        gui_terminal_run(query);
}

static void spot_frame(struct ui *ui, void *data)
{
    struct spot *s = data;
    int W = ui->win->surface.w;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){0, 0, ui->c->w, ui->c->h}, 0);
        gfx_symbol(ui->c, 20, (SPOT_BAR - 24) / 2, SYM_SEARCH, 24, style.secondary);
        int ty = (SPOT_BAR - font_large.height) / 2;
        if (s->query.len) {
            gfx_text(ui->c, 58, ty, &font_large, s->query.buf, style.text);
            int caret = gfx_text_width_n(&font_large, s->query.buf, (size_t)s->query.cursor);
            gfx_fill(ui->c, (struct rect){58 + caret, ty + 4, 2, font_large.height - 8}, style.accent);
        } else {
            gfx_text(ui->c, 58, ty, &font_large, "Spotlight Search", style.tertiary);
            gfx_fill(ui->c, (struct rect){58, ty + 4, 2, font_large.height - 8}, style.accent);
        }
        if (s->count)
            gfx_fill(ui->c, (struct rect){0, SPOT_BAR, W, 1}, style.separator);
        for (int i = 0; i < s->count; i++) {
            struct rect row = {8, SPOT_BAR + 7 + i * ROW_H, W - 16, ROW_H};
            bool sel = i == s->selected;
            if (sel)
                gfx_round(ui->c, row, 8, style.accent);
            gfx_icon(ui->c, row.x + 10, row.y + 6, s->res[i].icon, 28);
            uint32_t ink = sel ? RGB(255, 255, 255) : style.text;
            gfx_text_box(ui->c, (struct rect){row.x + 50, row.y, row.w - 260, row.h}, &font_sans, s->res[i].label, ink,
                         ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){row.x + row.w - 210, row.y, 200, row.h}, &font_sans, s->res[i].detail,
                         sel ? ALPHA(0xffffffffu, 200) : style.secondary, ALIGN_RIGHT);
        }
    }
    struct gui_event *e = ui->e;
    if (!e)
        return;
    if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN) {
        int i = e->y >= SPOT_BAR + 7 ? (e->y - SPOT_BAR - 7) / ROW_H : -1;
        if (i >= 0 && i < s->count && i != s->selected) {
            s->selected = i;
            ui->changed = true;
        }
        if (e->type == EV_MOUSE_DOWN) {
            ui->consumed = true;
            if (i >= 0 && i < s->count) {
                ui->win = NULL;
                open_result(s, i);
            }
        }
        return;
    }
    if (e->type != EV_KEY)
        return;
    int key = e->key, base = KEY_BASE(key);
    ui->consumed = true;
    if (base == KEY_ESC) {
        ui->win = NULL;
        wm_destroy(spot_win);
        return;
    }
    if (base == KEY_DOWN && s->count)
        s->selected = (s->selected + 1) % s->count;
    else if (base == KEY_UP && s->count)
        s->selected = (s->selected + s->count - 1) % s->count;
    else if (base == KEY_ENTER || key == '\r') {
        ui->win = NULL;
        open_result(s, s->selected);
        return;
    } else {
        ui_text_edit(&s->query, key);
    }
    update(s);
    int h = spot_height(s);
    if (h != ui->win->frame.h)
        wm_resize_client(ui->win, W, h);
}

static void spot_paint(struct window *w, struct canvas *c)
{
    struct spot *s = w->data;
    ui_run(w, &s->st, c, NULL, spot_frame, s);
}

static void spot_event(struct window *w, struct gui_event *e)
{
    struct spot *s = w->data;
    ui_run(w, &s->st, NULL, e, spot_frame, s);
}

static void spot_destroy(struct window *w)
{
    kfree(w->data);
    if (spot_win == w)
        spot_win = NULL;
}

static const struct window_ops spot_ops = {spot_paint, spot_event, NULL, spot_destroy};

void spotlight_toggle(void)
{
    if (spot_win) {
        wm_destroy(spot_win);
        return;
    }
    wm_close_popups();
    struct spot *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    struct rect scr = wm_screen_rect(0);
    spot_win = wm_popup(scr.x + (scr.w - SPOT_W) / 2, scr.y + scr.h / 4, SPOT_W, SPOT_BAR, &spot_ops, s, NULL);
    if (!spot_win) {
        kfree(s);
        return;
    }
    spot_win->radius = 22;
}

struct pad {
    struct ui_state st;
    struct ui_text filter;
    const struct gui_app *apps[48];
    int count;
    int hover;
};

static void collect_app(const struct gui_app *app, void *ctx)
{
    struct pad *p = ctx;
    if (p->count >= 48 || (p->filter.len && !contains_ci(app->name, p->filter.buf)))
        return;
    int i = p->count++;
    while (i > 0 && strcmp(p->apps[i - 1]->name, app->name) > 0) {
        p->apps[i] = p->apps[i - 1];
        i--;
    }
    p->apps[i] = app;
}

static void pad_collect(struct pad *p)
{
    p->count = 0;
    gui_app_foreach(collect_app, p);
}

#define PAD_CELL_W 150
#define PAD_CELL_H 130
#define PAD_ICON 80

static int pad_columns(int W)
{
    return CLAMP((W - 160) / PAD_CELL_W, 3, 7);
}

static struct rect pad_cell(struct window *w, struct pad *p, int i)
{
    int W = w->surface.w, H = w->surface.h;
    int cols = pad_columns(W), rows = (p->count + cols - 1) / MAX(cols, 1);
    int gx = (W - cols * PAD_CELL_W) / 2, gy = MAX(110, (H - rows * PAD_CELL_H) / 2 - 20);
    return (struct rect){gx + (i % cols) * PAD_CELL_W, gy + (i / cols) * PAD_CELL_H, PAD_CELL_W, PAD_CELL_H};
}

static void pad_frame(struct ui *ui, void *data)
{
    struct pad *p = data;
    struct window *w = ui->win;
    int W = w->surface.w;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){0, 0, ui->c->w, ui->c->h}, 0);
        struct rect field = {(W - 260) / 2, 48, 260, 30};
        gfx_round(ui->c, field, 15, ALPHA(0xffffffffu, 46));
        gfx_round_frame(ui->c, field, 15, ALPHA(0xffffffffu, 70));
        gfx_symbol(ui->c, field.x + 10, field.y + 7, SYM_SEARCH, 16, ALPHA(0xffffffffu, 200));
        gfx_text_box(ui->c, (struct rect){field.x + 32, field.y, field.w - 40, field.h}, &font_sans,
                     p->filter.len ? p->filter.buf : "Search", p->filter.len ? RGB(255, 255, 255) : ALPHA(0xffffffffu, 170),
                     ALIGN_LEFT);
        for (int i = 0; i < p->count; i++) {
            struct rect cell = pad_cell(w, p, i);
            int size = i == p->hover ? PAD_ICON + 6 : PAD_ICON;
            gfx_icon(ui->c, cell.x + (cell.w - size) / 2, cell.y + 8 + (PAD_ICON - size) / 2, p->apps[i]->icon, size);
            struct rect label = {cell.x, cell.y + PAD_ICON + 16, cell.w, 18};
            gfx_text_box(ui->c, (struct rect){label.x + 1, label.y + 1, label.w, label.h}, &font_sans, p->apps[i]->name,
                         ALPHA(0, 140), ALIGN_CENTER);
            gfx_text_box(ui->c, label, &font_sans, p->apps[i]->name, RGB(255, 255, 255), ALIGN_CENTER);
        }
    }
    struct gui_event *e = ui->e;
    if (!e)
        return;
    int at = -1;
    for (int i = 0; i < p->count; i++) {
        struct rect cell = pad_cell(w, p, i);
        struct rect hit = {cell.x + (cell.w - PAD_ICON) / 2 - 10, cell.y, PAD_ICON + 20, PAD_ICON + 40};
        if (rect_contains(hit, e->x, e->y))
            at = i;
    }
    if (e->type == EV_MOUSE_MOVE && at != p->hover) {
        p->hover = at;
        ui->changed = true;
    } else if (e->type == EV_MOUSE_DOWN) {
        ui->consumed = true;
        const struct gui_app *app = at >= 0 ? p->apps[at] : NULL;
        ui->win = NULL;
        wm_destroy(w);
        if (app)
            gui_app_launch(app, NULL);
    } else if (e->type == EV_KEY) {
        ui->consumed = true;
        int base = KEY_BASE(e->key);
        if (base == KEY_ESC || e->key == KEY_SUPER) {
            ui->win = NULL;
            wm_destroy(w);
            return;
        }
        if ((base == KEY_ENTER || e->key == '\r') && p->count) {
            const struct gui_app *app = p->apps[0];
            ui->win = NULL;
            wm_destroy(w);
            gui_app_launch(app, NULL);
            return;
        }
        ui_text_edit(&p->filter, e->key);
        pad_collect(p);
        p->hover = -1;
    }
}

static void pad_paint(struct window *w, struct canvas *c)
{
    struct pad *p = w->data;
    ui_run(w, &p->st, c, NULL, pad_frame, p);
}

static void pad_event(struct window *w, struct gui_event *e)
{
    struct pad *p = w->data;
    ui_run(w, &p->st, NULL, e, pad_frame, p);
}

static void pad_destroy(struct window *w)
{
    kfree(w->data);
    if (launch_win == w)
        launch_win = NULL;
}

static const struct window_ops pad_ops = {pad_paint, pad_event, NULL, pad_destroy};

void launchpad_toggle(void)
{
    if (launch_win) {
        wm_destroy(launch_win);
        return;
    }
    wm_close_popups();
    struct pad *p = kzalloc(sizeof(*p));
    if (!p)
        return;
    p->hover = -1;
    pad_collect(p);
    struct rect s = wm_screen_rect(0);
    launch_win = wm_popup(s.x, s.y, s.w, s.h, &pad_ops, p, NULL);
    if (!launch_win) {
        kfree(p);
        return;
    }
    launch_win->flags |= WIN_OVERLAY | WIN_NOSHADOW;
    launch_win->radius = 0;
    wm_move(launch_win, s.x, s.y);
}

struct control {
    struct ui_state st;
};

static void apply(const char *name, const char *value)
{
    setting_apply(shell_current(), name, value);
}

static void current(const char *name, char *buf, size_t cap)
{
    const struct setting *st = setting_find(name);
    buf[0] = 0;
    if (st && st->get)
        st->get(buf, cap);
}

static struct rect tile(struct ui *ui, int x, int y, int w, int h)
{
    struct rect r = {x, y, w, h};
    if (ui->c)
        gfx_round(ui->c, r, 14, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 150));
    return r;
}

static void control_frame(struct ui *ui, void *data)
{
    UNUSED(data);
    int W = ui->win->surface.w;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, ui->c->w, ui->c->h}, 0);
    char value[32];
    int y = 12;
    struct rect t = tile(ui, 12, y, W - 24, 64);
    ui_label(ui, (struct rect){t.x + 12, t.y + 6, 200, 20}, "Appearance", &font_bold, style.text, ALIGN_LEFT);
    current("appearance", value, sizeof(value));
    static const char *const modes[] = {"Light", "Dark", "Auto"};
    static const char *const mode_values[] = {"light", "dark", "auto"};
    int mode = !strcmp(value, "dark") ? 1 : !strcmp(value, "auto") ? 2 : 0;
    if (ui_segmented(ui, (struct rect){t.x + 12, t.y + 30, t.w - 24, 24}, modes, 3, &mode))
        apply("appearance", mode_values[mode]);
    y += 74;
    t = tile(ui, 12, y, W - 24, 64);
    ui_label(ui, (struct rect){t.x + 12, t.y + 6, 200, 20}, "Transparency", &font_bold, style.text, ALIGN_LEFT);
    current("glass", value, sizeof(value));
    static const char *const glass_names[] = {"Liquid Glass", "Frosted", "Solid"};
    static const char *const glass_values[] = {"liquid", "frosted", "solid"};
    int g = !strcmp(value, "frosted") ? 1 : !strcmp(value, "solid") ? 2 : 0;
    if (ui_segmented(ui, (struct rect){t.x + 12, t.y + 30, t.w - 24, 24}, glass_names, 3, &g))
        apply("glass", glass_values[g]);
    y += 74;
    t = tile(ui, 12, y, W - 24, 64);
    ui_label(ui, (struct rect){t.x + 12, t.y + 6, 200, 20}, "Accent color", &font_bold, style.text, ALIGN_LEFT);
    static const char *const accents[] = {"blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite"};
    static const uint32_t swatches[] = {RGB(0, 122, 255), RGB(149, 61, 150), RGB(247, 79, 158), RGB(224, 56, 62),
                                        RGB(247, 130, 27), RGB(230, 168, 0), RGB(98, 186, 70), RGB(140, 140, 140)};
    current("accent", value, sizeof(value));
    for (int i = 0; i < 8; i++) {
        struct rect dot = {t.x + 14 + i * 34, t.y + 32, 22, 22};
        if (ui->c) {
            gfx_circle(ui->c, dot.x + 11, dot.y + 11, 10, swatches[i]);
            if (!strcmp(value, accents[i]))
                gfx_circle(ui->c, dot.x + 11, dot.y + 11, 3, RGB(255, 255, 255));
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, dot)) {
            ui->consumed = true;
            apply("accent", accents[i]);
        }
    }
    y += 74;
    t = tile(ui, 12, y, W - 24, 64);
    ui_label(ui, (struct rect){t.x + 12, t.y + 6, 200, 20}, "Wallpaper", &font_bold, style.text, ALIGN_LEFT);
    static const char *const walls[] = {"Waves", "Hills", "Aurora", "Synthwave"};
    static const char *const wall_values[] = {"waves", "hills", "aurora", "synthwave"};
    current("wallpaper", value, sizeof(value));
    int wall = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(value, wall_values[i]))
            wall = i;
    if (ui_segmented(ui, (struct rect){t.x + 12, t.y + 30, t.w - 24, 24}, walls, 4, &wall))
        apply("wallpaper", wall_values[wall]);
    y += 74;
    int bw = (W - 36) / 2;
    if (ui_button(ui, (struct rect){12, y, bw, 30}, "Lock Screen")) {
        ui->win = NULL;
        wm_close_popups();
        gui_lock();
        return;
    }
    if (ui_button(ui, (struct rect){24 + bw, y, bw, 30}, "System Settings…")) {
        ui->win = NULL;
        wm_close_popups();
        gui_app_launch(gui_app_find("System Settings"), NULL);
        return;
    }
}

static void control_paint(struct window *w, struct canvas *c)
{
    struct control *cc = w->data;
    ui_run(w, &cc->st, c, NULL, control_frame, cc);
}

static void control_event(struct window *w, struct gui_event *e)
{
    struct control *cc = w->data;
    if (e->type == EV_KEY && KEY_BASE(e->key) == KEY_ESC) {
        wm_destroy(w);
        return;
    }
    ui_run(w, &cc->st, NULL, e, control_frame, cc);
}

static void control_destroy(struct window *w)
{
    kfree(w->data);
    if (control_win == w)
        control_win = NULL;
}

static const struct window_ops control_ops = {control_paint, control_event, NULL, control_destroy};

void control_center_toggle(int right)
{
    if (control_win) {
        wm_destroy(control_win);
        return;
    }
    struct control *cc = kzalloc(sizeof(*cc));
    if (!cc)
        return;
    int w = 330, h = 12 + 74 * 4 + 42;
    control_win = wm_popup(right - w, menubar_height() + 6, w, h, &control_ops, cc, NULL);
    if (!control_win) {
        kfree(cc);
        return;
    }
    control_win->radius = 22;
}
