#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "image.h"
#include "internal.h"

/* Screenshots, as Snipping Tool and macOS's Screenshot do them: the whole screen, one window or a dragged area, a
   bar of choices (Super+Shift+5, Print Screen), an optional timer, a floating thumbnail and an editor that crops
   and marks up. The screen is frozen when a capture starts, so menus and the pointer stay out of the picture. */

#define BAR_W 432
#define BAR_H 54
#define THUMB_W 180
#define UNDO 4

/* ---- options */

static bool to_pictures, show_thumb = true, open_editor, copy_shot = true;
static int delay_s;

static void get_folder(char *buf, size_t cap)
{
    str_copy(buf, to_pictures ? "pictures" : "desktop", cap);
}

static int set_folder(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "desktop") && strcmp(value, "pictures"))
        return E_INVAL;
    to_pictures = !strcmp(value, "pictures");
    return 0;
}

static void get_thumb(char *buf, size_t cap)
{
    setting_flag_get(show_thumb, buf, cap);
}

static int set_thumb(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return setting_flag(value, &show_thumb);
}

static void get_edit(char *buf, size_t cap)
{
    setting_flag_get(open_editor, buf, cap);
}

static int set_edit(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return setting_flag(value, &open_editor);
}

static void get_delay(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", delay_s);
}

static int set_delay(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || (v != 0 && v != 5 && v != 10))
        return E_INVAL;
    delay_s = (int)v;
    return 0;
}

static void get_copy(char *buf, size_t cap)
{
    setting_flag_get(copy_shot, buf, cap);
}

static int set_copy(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return setting_flag(value, &copy_shot);
}

static const struct setting s_copy = {"shotcopy", "on|off", "put every screenshot on the clipboard too", get_copy,
                                      set_copy};
SETTING(s_copy);

static const struct setting s_folder = {"shotfolder", "desktop|pictures", "where screenshots are saved", get_folder,
                                        set_folder};
static const struct setting s_thumb = {"shotthumb", "on|off", "show a thumbnail after a screenshot", get_thumb,
                                       set_thumb};
static const struct setting s_edit = {"shotedit", "on|off", "open the editor after a screenshot", get_edit, set_edit};
static const struct setting s_delay = {"shotdelay", "0|5|10", "seconds before a screenshot is taken", get_delay,
                                       set_delay};
SETTING(s_folder);
SETTING(s_thumb);
SETTING(s_edit);
SETTING(s_delay);

/* ---- saving */

static int save_shot(const struct canvas *img, char *path, size_t cap)
{
    char dir[PATH_MAX_LEN], name[80];
    if (to_pictures)
        path_join(user_home(), "Pictures", dir, sizeof(dir));
    else
        desktop_dir(dir, sizeof(dir));
    vfs_mkdirs(dir);
    struct rtc_time t;
    time_local(&t);
    ksnprintf(name, sizeof(name), "Screenshot %04u-%02u-%02u at %02u.%02u.%02u.png", t.year, t.month, t.day, t.hour,
              t.minute, t.second);
    path_join(dir, name, path, cap);
    for (int n = 2; vfs_exists(path) && n < 100; n++) {
        char again[96];
        ksnprintf(again, sizeof(again), "Screenshot %04u-%02u-%02u at %02u.%02u.%02u (%d).png", t.year, t.month,
                  t.day, t.hour, t.minute, t.second, n);
        path_join(dir, again, path, cap);
    }
    return image_save(path, img->px, img->w, img->h, img->stride);
}

static void editor_open(const char *path);
static void thumbnail_show(const struct canvas *img, const char *path);

static void finish(struct canvas *img)
{
    if (copy_shot)
        gui_clipboard_set_image(img->px, img->w, img->h, img->stride);
    char path[PATH_MAX_LEN];
    int err = save_shot(img, path, sizeof(path));
    if (err) {
        gui_message("The screenshot could not be saved", err_str(err));
    } else if (open_editor) {
        editor_open(path);
    } else if (show_thumb) {
        thumbnail_show(img, path);
    } else {
        gui_notify("Screenshot", path_basename(path));
    }
    canvas_free(img);
}

static bool crop(const struct canvas *from, struct rect r, struct canvas *out)
{
    r = rect_intersect(r, (struct rect){0, 0, from->w, from->h});
    if (rect_empty(r) || !canvas_alloc(out, r.w, r.h))
        return false;
    for (int y = 0; y < r.h; y++)
        memcpy(out->px + y * out->stride, from->px + (r.y + y) * from->stride + r.x, (size_t)r.w * 4);
    return true;
}

/* ---- the overlay: frozen screen, selection and the bar */

struct overlay {
    struct ui_state st;
    enum shot_mode mode;
    bool bar;
    struct canvas frozen;
    struct rect screen;
    bool dragging;
    int sx, sy, ex, ey;
    struct window *hover;
};

static struct window *overlay_win;

static struct rect selection(const struct overlay *o)
{
    return (struct rect){MIN(o->sx, o->ex), MIN(o->sy, o->ey), ABS(o->ex - o->sx), ABS(o->ey - o->sy)};
}

static struct rect bar_rect(int W, int H)
{
    return (struct rect){(W - BAR_W) / 2, H - BAR_H - 90, BAR_W, BAR_H};
}

static void capture_window(struct overlay *o, struct window *w)
{
    struct rect f = w->frame;
    struct canvas img, win;
    if (!crop(&o->frozen, (struct rect){f.x - o->screen.x, f.y - o->screen.y, f.w, f.h}, &img))
        return;
    if (wm_window_image(w, &win)) {
        gfx_blit_alpha(&img, MIN(0, f.x - o->screen.x), MIN(0, f.y - o->screen.y), &win,
                       (struct rect){0, 0, win.w, win.h}, 255);
        canvas_free(&win);
    }
    finish(&img);
}

static void take(struct window *ow, struct overlay *o, struct rect r)
{
    struct canvas img;
    bool ok = crop(&o->frozen, r, &img);
    wm_destroy(ow);
    if (ok)
        finish(&img);
}

static struct menu_item option_items[8];
static const char *const option_labels[] = {"Save to Desktop", "Save to Pictures", "",
                                            "No Timer", "5 Seconds", "10 Seconds", "",
                                            "Show Floating Thumbnail"};

static void pick_option(struct window *owner, int index)
{
    UNUSED(owner);
    switch (index) {
    case 0: gui_setting("shotfolder", "desktop"); break;
    case 1: gui_setting("shotfolder", "pictures"); break;
    case 3: gui_setting("shotdelay", "0"); break;
    case 4: gui_setting("shotdelay", "5"); break;
    case 5: gui_setting("shotdelay", "10"); break;
    case 7: gui_setting("shotthumb", show_thumb ? "off" : "on"); break;
    }
    if (overlay_win)
        wm_invalidate(overlay_win);
}

static void open_options(struct ui *ui, struct rect at)
{
    static char labels[8][40];
    bool checked[8] = {!to_pictures, to_pictures, false, delay_s == 0, delay_s == 5, delay_s == 10, false, show_thumb};
    for (int i = 0; i < 8; i++) {
        if (option_labels[i][0])
            ksnprintf(labels[i], sizeof(labels[i]), "%s%s", checked[i] ? "✓ " : "    ", option_labels[i]);
        else
            labels[i][0] = 0;
        option_items[i] = (struct menu_item){labels[i], 0, NULL};
    }
    ui_menu_items(ui->win, at.x, at.y - 8 * 26 - 12, option_items, 8, pick_option);
}

static void start_now(enum shot_mode mode, bool bar);
static enum shot_mode pending_mode;
static bool pending, pending_bar;
static uint64_t pending_due;
static int settled;

static void schedule(enum shot_mode mode, bool bar, int seconds)
{
    pending = true;
    pending_mode = mode;
    pending_bar = bar;
    pending_due = hal_ticks_ms() + (uint64_t)seconds * 1000;
    settled = 0;
}

static bool bar_button(struct ui *ui, struct rect r, enum symbol_id sym, bool on)
{
    bool hot = ui_hover(ui, r);
    if (ui->c) {
        if (on || hot)
            gfx_round(ui->c, r, 9, ALPHA(0xffffffffu, on ? 70 : 34));
        gfx_symbol(ui->c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, sym, 16, RGB(255, 255, 255));
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
        ui->consumed = true;
        return true;
    }
    return false;
}

static void draw_bar(struct ui *ui, struct overlay *o, struct rect b)
{
    if (ui->c) {
        gfx_drop_shadow(ui->c, b, 14, 18, 90, 6);
        gfx_round(ui->c, b, 14, RGBA(36, 36, 40, 236));
        gfx_round_frame(ui->c, b, 14, ALPHA(0xffffffffu, 40));
    }
    int x = b.x + 8, y = b.y + 7;
    if (bar_button(ui, (struct rect){x, y, 40, 40}, SYM_CLOSE, false)) {
        ui->win = NULL;
        wm_destroy(overlay_win);
        return;
    }
    x += 48;
    if (ui->c)
        gfx_fill(ui->c, (struct rect){x - 4, y + 8, 1, 24}, ALPHA(0xffffffffu, 50));
    static const enum symbol_id syms[3] = {SYM_DISPLAY, SYM_WINDOW, SYM_SCREENSHOT};
    static const enum shot_mode modes[3] = {SHOT_SCREEN, SHOT_WINDOW, SHOT_REGION};
    for (int i = 0; i < 3; i++, x += 44)
        if (bar_button(ui, (struct rect){x, y, 40, 40}, syms[i], o->mode == modes[i])) {
            o->mode = modes[i];
            o->dragging = false;
            o->sx = o->ex = o->sy = o->ey = 0;
        }
    x += 6;
    if (ui->c)
        gfx_fill(ui->c, (struct rect){x - 4, y + 8, 1, 24}, ALPHA(0xffffffffu, 50));
    struct rect opt = {x + 4, y + 6, 96, 28};
    bool hot = ui_hover(ui, opt);
    if (ui->c) {
        if (hot)
            gfx_round(ui->c, opt, 8, ALPHA(0xffffffffu, 34));
        gfx_text_box(ui->c, (struct rect){opt.x + 8, opt.y, 64, opt.h}, &font_sans, "Options", RGB(255, 255, 255),
                     ALIGN_LEFT);
        gfx_symbol(ui->c, opt.x + opt.w - 22, opt.y + 6, SYM_DOWN, 16, RGB(255, 255, 255));
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
        ui->consumed = true;
        open_options(ui, (struct rect){opt.x, b.y, opt.w, opt.h});
        return;
    }
    struct rect cap = {b.x + b.w - 104, y + 6, 92, 28};
    hot = ui_hover(ui, cap);
    if (ui->c) {
        gfx_round(ui->c, cap, 8, hot ? color_shade(style.accent, 20) : style.accent);
        gfx_text_box(ui->c, cap, &font_bold, "Capture", RGB(255, 255, 255), ALIGN_CENTER);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
        ui->consumed = true;
        struct rect sel = selection(o);
        if (delay_s && o->mode == SHOT_SCREEN) {
            ui->win = NULL;
            wm_destroy(overlay_win);
            schedule(SHOT_SCREEN, false, delay_s);
        } else if (o->mode == SHOT_SCREEN || (o->mode == SHOT_REGION && sel.w > 4 && sel.h > 4)) {
            ui->win = NULL;
            take(overlay_win, o, o->mode == SHOT_SCREEN ? (struct rect){0, 0, o->frozen.w, o->frozen.h} : sel);
        }
    }
}

static void dim_outside(struct canvas *c, struct rect all, struct rect keep, uint32_t shade)
{
    gfx_fill(c, (struct rect){all.x, all.y, all.w, keep.y - all.y}, shade);
    gfx_fill(c, (struct rect){all.x, keep.y + keep.h, all.w, all.y + all.h - keep.y - keep.h}, shade);
    gfx_fill(c, (struct rect){all.x, keep.y, keep.x - all.x, keep.h}, shade);
    gfx_fill(c, (struct rect){keep.x + keep.w, keep.y, all.x + all.w - keep.x - keep.w, keep.h}, shade);
}

static void overlay_frame(struct ui *ui, void *data)
{
    struct overlay *o = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    struct rect all = {0, 0, W, H}, b = bar_rect(W, H);
    struct rect sel = selection(o);
    uint32_t shade = ALPHA(0, 96);
    if (ui->c) {
        gfx_blit(ui->c, 0, 0, &o->frozen, all);
        if (o->mode == SHOT_REGION && (o->dragging || sel.w)) {
            dim_outside(ui->c, all, sel, shade);
            gfx_frame(ui->c, rect_inset(sel, -1), ALPHA(0xffffffffu, 230));
            char size[32];
            ksnprintf(size, sizeof(size), "%d × %d", sel.w, sel.h);
            int tw = gfx_text_width(&font_small, size) + 14;
            struct rect tag = {sel.x + sel.w - tw, sel.y + sel.h + 6, tw, 20};
            if (tag.y + tag.h > H)
                tag.y = sel.y + sel.h - 26;
            gfx_round(ui->c, tag, 6, ALPHA(0, 170));
            gfx_text_box(ui->c, tag, &font_small, size, RGB(255, 255, 255), ALIGN_CENTER);
        } else if (o->mode == SHOT_WINDOW && o->hover) {
            struct rect f = o->hover->frame;
            f.x -= o->screen.x;
            f.y -= o->screen.y;
            dim_outside(ui->c, all, f, shade);
            gfx_round(ui->c, f, 12, ALPHA(style.accent, 70));
        } else {
            gfx_fill(ui->c, all, o->mode == SHOT_SCREEN ? ALPHA(0, 40) : shade);
        }
        if (!o->bar && o->mode != SHOT_SCREEN && !o->dragging) {
            const char *hint = o->mode == SHOT_REGION ? "Drag to select an area  ·  Esc to cancel"
                                                      : "Click a window  ·  Esc to cancel";
            int tw = gfx_text_width(&font_sans, hint) + 32;
            struct rect pill = {(W - tw) / 2, 60, tw, 32};
            gfx_round(ui->c, pill, 16, ALPHA(0, 170));
            gfx_text_box(ui->c, pill, &font_sans, hint, RGB(255, 255, 255), ALIGN_CENTER);
        }
    }
    if (o->bar && !o->dragging) {
        draw_bar(ui, o, b);
        if (!ui->win)
            return;
    }
    struct gui_event *e = ui->e;
    if (!e || ui->consumed)
        return;
    if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_RIGHT) {
        ui->win = NULL;
        wm_destroy(overlay_win);
        return;
    }
    if (o->mode == SHOT_REGION) {
        if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT) {
            o->dragging = true;
            o->sx = o->ex = e->x;
            o->sy = o->ey = e->y;
            ui->changed = true;
        } else if (e->type == EV_MOUSE_MOVE && o->dragging) {
            o->ex = CLAMP(e->x, 0, W);
            o->ey = CLAMP(e->y, 0, H);
            ui->changed = true;
        } else if (e->type == EV_MOUSE_UP && o->dragging) {
            o->dragging = false;
            sel = selection(o);
            if (sel.w > 4 && sel.h > 4) {
                ui->win = NULL;
                take(overlay_win, o, sel);
                return;
            }
            ui->changed = true;
        }
    } else if (o->mode == SHOT_WINDOW) {
        if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN) {
            struct window *h = wm_window_at_point(o->screen.x + e->x, o->screen.y + e->y);
            if (h != o->hover) {
                o->hover = h;
                ui->changed = true;
            }
        }
        if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT && o->hover) {
            struct window *target = o->hover;
            ui->win = NULL;
            struct overlay copy = *o;
            o->frozen.px = NULL;
            wm_destroy(overlay_win);
            capture_window(&copy, target);
            canvas_free(&copy.frozen);
            return;
        }
    } else if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT) {
        ui->win = NULL;
        take(overlay_win, o, all);
        return;
    }
    if (e->type == EV_MOUSE_DOWN)
        ui->consumed = true;
}

static void overlay_paint(struct window *w, struct canvas *c)
{
    struct overlay *o = w->data;
    ui_run(w, &o->st, c, NULL, overlay_frame, o);
}

static void overlay_event(struct window *w, struct gui_event *e)
{
    struct overlay *o = w->data;
    if (e->type == EV_KEY) {
        int k = KEY_BASE(e->key);
        if (k == KEY_ESC) {
            wm_destroy(w);
        } else if (k == KEY_ENTER) {
            struct rect sel = selection(o);
            if (o->mode == SHOT_SCREEN)
                take(w, o, (struct rect){0, 0, o->frozen.w, o->frozen.h});
            else if (o->mode == SHOT_REGION && sel.w > 4 && sel.h > 4)
                take(w, o, sel);
        }
        return;
    }
    ui_run(w, &o->st, NULL, e, overlay_frame, o);
    if (e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP)
        e->handled = true;
}

static void overlay_destroy(struct window *w)
{
    struct overlay *o = w->data;
    canvas_free(&o->frozen);
    kfree(o);
    if (overlay_win == w)
        overlay_win = NULL;
}

static const struct window_ops overlay_ops = {overlay_paint, overlay_event, NULL, overlay_destroy};

static void start_now(enum shot_mode mode, bool bar)
{
    struct rect s = wm_screen_rect(0);
    struct canvas frozen;
    if (!wm_capture(s, &frozen))
        return;
    if (mode == SHOT_SCREEN && !bar) {
        finish(&frozen);
        return;
    }
    struct overlay *o = kzalloc(sizeof(*o));
    if (!o) {
        canvas_free(&frozen);
        return;
    }
    o->mode = mode;
    o->bar = bar;
    o->frozen = frozen;
    o->screen = s;
    overlay_win = wm_popup(s.x, s.y, s.w, s.h, &overlay_ops, o, NULL);
    if (!overlay_win) {
        canvas_free(&o->frozen);
        kfree(o);
        return;
    }
    overlay_win->flags |= WIN_OVERLAY | WIN_NOSHADOW;
    overlay_win->radius = 0;
    wm_move(overlay_win, s.x, s.y);
}

/* Waits until menus and Control Center have finished closing, so the frozen screen shows only what was under
   them. */
void screenshot_tick(void)
{
    if (!pending)
        return;
    settled = wm_settled() ? settled + 1 : 0;
    if (settled < 3 || hal_ticks_ms() < pending_due)
        return;
    pending = false;
    start_now(pending_mode, pending_bar);
}

void screenshot_start(enum shot_mode mode)
{
    if (overlay_win && wm_window_alive(overlay_win)) {
        wm_destroy(overlay_win);
        return;
    }
    wm_close_popups();
    bool bar = mode == SHOT_PANEL;
    schedule(bar ? SHOT_REGION : mode, bar, !bar ? delay_s : 0);
}

/* ---- the floating thumbnail */

struct thumb {
    struct canvas img;
    char path[PATH_MAX_LEN];
    uint64_t until;
};

static void thumb_paint(struct window *w, struct canvas *c)
{
    struct thumb *t = w->data;
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    gfx_round(c, (struct rect){0, 0, c->w, c->h}, 10, RGB(255, 255, 255));
    struct rect in = {4, 4, c->w - 8, c->h - 8};
    struct canvas fit;
    if (canvas_alloc(&fit, in.w, in.h)) {
        gfx_scale_smooth(&fit, (struct rect){0, 0, in.w, in.h}, &t->img, 255);
        gfx_blit_round(c, in, &fit, 7, false);
        canvas_free(&fit);
    }
}

static void thumb_event(struct window *w, struct gui_event *e)
{
    struct thumb *t = w->data;
    if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
        char path[PATH_MAX_LEN];
        str_copy(path, t->path, sizeof(path));
        wm_destroy(w);
        if (e->button == BUTTON_LEFT)
            editor_open(path);
    } else if (e->type == EV_TIMER && hal_ticks_ms() >= t->until) {
        wm_destroy(w);
    }
}

static void thumb_destroy(struct window *w)
{
    struct thumb *t = w->data;
    canvas_free(&t->img);
    kfree(t);
}

static const struct window_ops thumb_ops = {thumb_paint, thumb_event, NULL, thumb_destroy};

static void thumbnail_show(const struct canvas *img, const char *path)
{
    struct thumb *t = kzalloc(sizeof(*t));
    if (!t)
        return;
    int w = THUMB_W, h = CLAMP(THUMB_W * img->h / MAX(img->w, 1), 50, 150);
    if (!canvas_alloc(&t->img, w * 2, h * 2)) {
        kfree(t);
        return;
    }
    gfx_scale_smooth(&t->img, (struct rect){0, 0, w * 2, h * 2}, img, 255);
    str_copy(t->path, path, sizeof(t->path));
    t->until = hal_ticks_ms() + 6000;
    struct rect s = wm_screen_rect(0);
    struct window *win = wm_popup(s.x + s.w - w - 24, s.y + s.h - h - 110, w + 8, h + 8, &thumb_ops, t, NULL);
    if (!win) {
        canvas_free(&t->img);
        kfree(t);
        return;
    }
    win->radius = 10;
    win->flags |= WIN_PASSIVE;
    wm_set_timer(win, 250);
}

/* ---- the editor: crop and mark up */

enum tool {
    TOOL_NONE,
    TOOL_CROP,
    TOOL_PEN,
    TOOL_MARKER,
};

enum {
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_SHOW,
    CMD_PAINT,
    CMD_DELETE,
    CMD_UNDO,
    CMD_CROP,
    CMD_COPY,
    CMD_NEW,
};

static const struct menu_item file_items[] = {
    {"New Screenshot", KEY_COMMAND(CMD_NEW), "⇧⌘5"}, {"", 0, NULL},
    {"Save", KEY_COMMAND(CMD_SAVE), "⌘S"},            {"Save As…", KEY_COMMAND(CMD_SAVE_AS), NULL},
    {"Show in Files", KEY_COMMAND(CMD_SHOW), NULL},   {"Edit in Paint", KEY_COMMAND(CMD_PAINT), NULL},
    {"", 0, NULL},                                    {"Move to Trash", KEY_COMMAND(CMD_DELETE), NULL},
};

static const struct menu_item edit_items[] = {
    {"Undo", KEY_COMMAND(CMD_UNDO), "⌘Z"},
    {"Copy", KEY_COMMAND(CMD_COPY), "⌘C"},
    {"Crop", KEY_COMMAND(CMD_CROP), "⌘K"},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"Edit", edit_items, ARRAY_SIZE(edit_items)},
};

static const uint32_t pens[4] = {RGB(255, 59, 48), RGB(255, 204, 0), RGB(0, 122, 255), RGB(28, 28, 30)};

struct editor {
    struct ui_state st;
    char path[PATH_MAX_LEN];
    struct canvas img, fitted, undo[UNDO];
    int undo_n;
    enum tool tool;
    int color;
    bool modified, stroke;
    struct rect crop_r;
    int handle;
    int last_x, last_y;
};

static struct rect view_rect(int W, int H)
{
    return (struct rect){16, TOOLBAR_HEIGHT + 10, W - 32, H - TOOLBAR_HEIGHT - 26};
}

/* Where the picture is drawn and how big: scale is 1024 for 1:1. */
static struct rect placed(struct editor *ed, int W, int H, int *scale)
{
    struct rect v = view_rect(W, H);
    int s = MIN(1024, MIN(v.w * 1024 / MAX(ed->img.w, 1), v.h * 1024 / MAX(ed->img.h, 1)));
    s = MAX(s, 1);
    *scale = s;
    int w = ed->img.w * s / 1024, h = ed->img.h * s / 1024;
    return (struct rect){v.x + (v.w - w) / 2, v.y + (v.h - h) / 2, w, h};
}

static void to_image(struct rect p, int scale, int x, int y, int *ix, int *iy)
{
    *ix = (x - p.x) * 1024 / scale;
    *iy = (y - p.y) * 1024 / scale;
}

static void push_undo(struct editor *ed)
{
    if (ed->undo_n == UNDO) {
        canvas_free(&ed->undo[0]);
        memmove(ed->undo, ed->undo + 1, sizeof(ed->undo[0]) * (UNDO - 1));
        ed->undo_n--;
    }
    struct canvas *u = &ed->undo[ed->undo_n];
    if (!canvas_alloc(u, ed->img.w, ed->img.h))
        return;
    for (int y = 0; y < ed->img.h; y++)
        memcpy(u->px + y * u->stride, ed->img.px + y * ed->img.stride, (size_t)ed->img.w * 4);
    ed->undo_n++;
}

static void changed(struct editor *ed)
{
    canvas_free(&ed->fitted);
    ed->modified = true;
}

static void undo(struct editor *ed)
{
    if (!ed->undo_n)
        return;
    canvas_free(&ed->img);
    ed->img = ed->undo[--ed->undo_n];
    ed->undo[ed->undo_n].px = NULL;
    ed->crop_r = (struct rect){0, 0, ed->img.w, ed->img.h};
    changed(ed);
}

static void apply_crop(struct editor *ed)
{
    struct rect r = ed->crop_r;
    if (r.w < 4 || r.h < 4 || (r.x == 0 && r.y == 0 && r.w == ed->img.w && r.h == ed->img.h)) {
        ed->tool = TOOL_NONE;
        return;
    }
    struct canvas out;
    if (!crop(&ed->img, r, &out))
        return;
    push_undo(ed);
    canvas_free(&ed->img);
    ed->img = out;
    ed->crop_r = (struct rect){0, 0, out.w, out.h};
    ed->tool = TOOL_NONE;
    changed(ed);
}

static bool editor_save(struct editor *ed)
{
    int err = image_save(ed->path, ed->img.px, ed->img.w, ed->img.h, ed->img.stride);
    if (err) {
        gui_message("The screenshot could not be saved", err_str(err));
        return false;
    }
    ed->modified = false;
    return true;
}

static void saved_as(struct window *w, const char *value)
{
    if (!wm_window_alive(w) || !*value)
        return;
    struct editor *ed = w->data;
    str_copy(ed->path, value, sizeof(ed->path));
    if (editor_save(ed))
        wm_set_title(w, path_basename(ed->path));
}

static void trashed(struct window *w, const char *value)
{
    UNUSED(value);
    if (!wm_window_alive(w))
        return;
    struct editor *ed = w->data;
    vfs_unlink(ed->path);
    ed->modified = false;
    wm_close(w);
}

static void command(struct window *w, struct editor *ed, int cmd)
{
    switch (cmd) {
    case CMD_SAVE: editor_save(ed); break;
    case CMD_SAVE_AS: ui_prompt(w, "Save Screenshot", "Save as (PNG or BMP):", ed->path, saved_as); break;
    case CMD_SHOW: {
        char dir[PATH_MAX_LEN];
        path_dirname(ed->path, dir, sizeof(dir));
        gui_app_launch(gui_app_find("Files"), dir);
        break;
    }
    case CMD_PAINT:
        if (ed->modified)
            editor_save(ed);
        gui_app_launch(gui_app_find("Paint"), ed->path);
        break;
    case CMD_DELETE: ui_confirm(w, "Move to Trash", "Delete this screenshot?", trashed); break;
    case CMD_UNDO: undo(ed); break;
    case CMD_CROP:
        ed->tool = ed->tool == TOOL_CROP ? TOOL_NONE : TOOL_CROP;
        ed->crop_r = (struct rect){0, 0, ed->img.w, ed->img.h};
        break;
    case CMD_NEW: screenshot_start(SHOT_PANEL); break;
    case CMD_COPY:
        gui_clipboard_set_image(ed->img.px, ed->img.w, ed->img.h, ed->img.stride);
        gui_notify("Screenshot", "Copied to the clipboard.");
        break;
    }
    wm_invalidate(w);
}

static struct rect handle_rect(struct rect c, int i)
{
    int hx = i % 3, hy = i / 3;
    return (struct rect){c.x + hx * c.w / 2 - 6, c.y + hy * c.h / 2 - 6, 12, 12};
}

static void editor_frame(struct ui *ui, void *data)
{
    struct editor *ed = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, style.dark ? RGB(30, 30, 32) : RGB(236, 236, 240));
    int x = (W - 8 * 36 - 40) / 2, y = (TOOLBAR_HEIGHT - 28) / 2;
    if (ui_symbol_button(ui, (struct rect){x, y, 28, 28}, SYM_CROP, ed->tool == TOOL_CROP))
        command(w, ed, CMD_CROP);
    x += 36;
    if (ui_symbol_button(ui, (struct rect){x, y, 28, 28}, SYM_PENCIL, ed->tool == TOOL_PEN))
        ed->tool = ed->tool == TOOL_PEN ? TOOL_NONE : TOOL_PEN;
    x += 36;
    if (ui_symbol_button(ui, (struct rect){x, y, 28, 28}, SYM_BRUSH, ed->tool == TOOL_MARKER))
        ed->tool = ed->tool == TOOL_MARKER ? TOOL_NONE : TOOL_MARKER;
    x += 44;
    for (int i = 0; i < 4; i++, x += 26) {
        struct rect dot = {x, y + 5, 18, 18};
        if (ui->c) {
            gfx_circle(ui->c, dot.x + 9, dot.y + 9, 8, pens[i]);
            if (ed->color == i)
                gfx_ring(ui->c, dot.x + 9, dot.y + 9, 11, style.text);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, dot)) {
            ui->consumed = true;
            ed->color = i;
            if (ed->tool != TOOL_MARKER)
                ed->tool = TOOL_PEN;
        }
    }
    x += 10;
    if (ui_symbol_button(ui, (struct rect){x, y, 28, 28}, SYM_UNDO, false))
        undo(ed);
    x += 36;
    if (ui_symbol_button(ui, (struct rect){x, y, 28, 28}, SYM_COPY, false))
        command(w, ed, CMD_COPY);
    if (ed->tool == TOOL_CROP) {
        if (ui_button_primary(ui, (struct rect){W - 196, y, 90, 28}, "Crop"))
            apply_crop(ed);
        if (ui_button(ui, (struct rect){W - 100, y, 84, 28}, "Cancel"))
            ed->tool = TOOL_NONE;
    } else if (ui_button_primary(ui, (struct rect){W - 100, y, 84, 28}, "Done")) {
        if (!ed->modified || editor_save(ed)) {
            ui->win = NULL;
            wm_close(w);
            return;
        }
    }
    int scale;
    struct rect p = placed(ed, W, H, &scale);
    if (ui->c) {
        if (!ed->fitted.px || ed->fitted.w != p.w || ed->fitted.h != p.h) {
            canvas_free(&ed->fitted);
            if (canvas_alloc(&ed->fitted, MAX(p.w, 1), MAX(p.h, 1)))
                gfx_scale_smooth(&ed->fitted, (struct rect){0, 0, p.w, p.h}, &ed->img, 255);
        }
        gfx_drop_shadow(ui->c, p, 2, 14, 60, 4);
        if (ed->fitted.px)
            gfx_blit(ui->c, p.x, p.y, &ed->fitted, (struct rect){0, 0, p.w, p.h});
        if (ed->tool == TOOL_CROP) {
            struct rect c = {p.x + ed->crop_r.x * scale / 1024, p.y + ed->crop_r.y * scale / 1024,
                             ed->crop_r.w * scale / 1024, ed->crop_r.h * scale / 1024};
            struct canvas k = *ui->c;
            k.clip = rect_intersect(ui->c->clip, p);
            dim_outside(&k, p, c, ALPHA(0, 120));
            gfx_frame(ui->c, c, RGB(255, 255, 255));
            for (int i = 1; i < 3; i++) {
                gfx_fill(ui->c, (struct rect){c.x + c.w * i / 3, c.y, 1, c.h}, ALPHA(0xffffffffu, 90));
                gfx_fill(ui->c, (struct rect){c.x, c.y + c.h * i / 3, c.w, 1}, ALPHA(0xffffffffu, 90));
            }
            for (int i = 0; i < 9; i++)
                if (i != 4) {
                    struct rect hr = handle_rect(c, i);
                    gfx_round(ui->c, hr, 3, RGB(255, 255, 255));
                    gfx_round_frame(ui->c, hr, 3, ALPHA(0, 80));
                }
        }
    }
    struct gui_event *e = ui->e;
    if (!e || ui->consumed)
        return;
    int ix, iy;
    to_image(p, scale, e->x, e->y, &ix, &iy);
    if (ed->tool == TOOL_CROP) {
        struct rect c = {p.x + ed->crop_r.x * scale / 1024, p.y + ed->crop_r.y * scale / 1024,
                         ed->crop_r.w * scale / 1024, ed->crop_r.h * scale / 1024};
        if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT) {
            ed->handle = -1;
            for (int i = 0; i < 9; i++)
                if (i != 4 && rect_contains(rect_inset(handle_rect(c, i), -6), e->x, e->y))
                    ed->handle = i;
            if (ed->handle < 0 && rect_contains(c, e->x, e->y))
                ed->handle = 4;
            if (ed->handle < 0 && rect_contains(p, e->x, e->y)) {
                ed->handle = 8;
                ed->crop_r = (struct rect){ix, iy, 0, 0};
            }
            ed->last_x = ix;
            ed->last_y = iy;
            ui->consumed = true;
        } else if (e->type == EV_MOUSE_MOVE && ui->st->active == 0 && ed->handle >= 0 &&
                   (e->buttons & BUTTON_LEFT)) {
            struct rect r = ed->crop_r;
            int dx = ix - ed->last_x, dy = iy - ed->last_y;
            ed->last_x = ix;
            ed->last_y = iy;
            int x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
            if (ed->handle == 4) {
                dx = CLAMP(dx, -x0, ed->img.w - x1);
                dy = CLAMP(dy, -y0, ed->img.h - y1);
                x0 += dx, x1 += dx, y0 += dy, y1 += dy;
            } else {
                int hx = ed->handle % 3, hy = ed->handle / 3;
                if (hx == 0)
                    x0 += dx;
                if (hx == 2)
                    x1 += dx;
                if (hy == 0)
                    y0 += dy;
                if (hy == 2)
                    y1 += dy;
            }
            x0 = CLAMP(x0, 0, ed->img.w);
            x1 = CLAMP(x1, 0, ed->img.w);
            y0 = CLAMP(y0, 0, ed->img.h);
            y1 = CLAMP(y1, 0, ed->img.h);
            ed->crop_r = (struct rect){MIN(x0, x1), MIN(y0, y1), ABS(x1 - x0), ABS(y1 - y0)};
            if (x1 < x0 && ed->handle % 3 != 1)
                ed->handle += ed->handle % 3 == 0 ? 2 : -2;
            if (y1 < y0 && ed->handle / 3 != 1)
                ed->handle += ed->handle / 3 == 0 ? 6 : -6;
            ui->changed = true;
        } else if (e->type == EV_MOUSE_UP) {
            ed->handle = -1;
        }
        return;
    }
    if (ed->tool == TOOL_PEN || ed->tool == TOOL_MARKER) {
        int width = ed->tool == TOOL_MARKER ? 18 : 4;
        uint32_t col = ed->tool == TOOL_MARKER ? ALPHA(pens[ed->color], 90) : pens[ed->color];
        if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT && rect_contains(p, e->x, e->y)) {
            push_undo(ed);
            ed->stroke = true;
            ed->last_x = ix;
            ed->last_y = iy;
            gfx_circle(&ed->img, ix, iy, width / 2, col);
            changed(ed);
            ui->consumed = true;
        } else if (e->type == EV_MOUSE_MOVE && ed->stroke) {
            gfx_thick_line(&ed->img, ed->last_x, ed->last_y, ix, iy, width, col);
            ed->last_x = ix;
            ed->last_y = iy;
            changed(ed);
            ui->changed = true;
        } else if (e->type == EV_MOUSE_UP) {
            ed->stroke = false;
        }
    }
}

static void editor_paint(struct window *w, struct canvas *c)
{
    struct editor *ed = w->data;
    ui_run(w, &ed->st, c, NULL, editor_frame, ed);
}

static void editor_event(struct window *w, struct gui_event *e)
{
    struct editor *ed = w->data;
    if (e->type == EV_KEY) {
        int k = e->key;
        if (k >= KEY_COMMAND(0) && k <= KEY_COMMAND(CMD_NEW)) {
            command(w, ed, k - KEY_COMMAND(0));
            return;
        }
        if (k == KEY_CTRL('s')) {
            command(w, ed, CMD_SAVE);
            return;
        }
        if (k == KEY_CTRL('z')) {
            command(w, ed, CMD_UNDO);
            return;
        }
        if (k == KEY_CTRL('c')) {
            command(w, ed, CMD_COPY);
            return;
        }
        if (k == KEY_CTRL('k')) {
            command(w, ed, CMD_CROP);
            return;
        }
        if (k == KEY_ENTER && ed->tool == TOOL_CROP) {
            apply_crop(ed);
            wm_invalidate(w);
            return;
        }
        if (k == KEY_ESC && ed->tool != TOOL_NONE) {
            ed->tool = TOOL_NONE;
            wm_invalidate(w);
            return;
        }
    }
    if (e->type == EV_RESIZE)
        canvas_free(&ed->fitted);
    ui_run(w, &ed->st, NULL, e, editor_frame, ed);
}

static bool editor_close(struct window *w)
{
    struct editor *ed = w->data;
    if (ed->modified)
        editor_save(ed);
    return true;
}

static void editor_destroy(struct window *w)
{
    struct editor *ed = w->data;
    canvas_free(&ed->img);
    canvas_free(&ed->fitted);
    for (int i = 0; i < ed->undo_n; i++)
        canvas_free(&ed->undo[i]);
    kfree(ed);
}

static const struct window_ops editor_ops = {editor_paint, editor_event, editor_close, editor_destroy};

static void editor_open(const char *path)
{
    struct editor *ed = kzalloc(sizeof(*ed));
    if (!ed)
        return;
    struct image im;
    if (!image_load(path, &im)) {
        kfree(ed);
        gui_message("Screenshot", "The picture could not be opened.");
        return;
    }
    ed->img = (struct canvas){im.px, im.w, im.h, im.w, {0, 0, im.w, im.h}};
    ed->crop_r = (struct rect){0, 0, im.w, im.h};
    ed->handle = -1;
    str_copy(ed->path, path, sizeof(ed->path));
    struct rect s = wm_screen_rect(0);
    int w = CLAMP(im.w + 40, 560, s.w - 160), h = CLAMP(im.h + TOOLBAR_HEIGHT + 40, 380, s.h - 140);
    struct window *win = wm_open(path_basename(path), ICON_SCREENSHOT, w, h, &editor_ops, ed,
                                 WIN_RESIZABLE | WIN_FULLSIZE | WIN_NOTITLE);
    if (!win) {
        canvas_free(&ed->img);
        kfree(ed);
        return;
    }
    win->toolbar_h = TOOLBAR_HEIGHT;
    win->min_w = 520;
    win->min_h = 320;
    wm_set_menus(win, menus, ARRAY_SIZE(menus));
}

static void launch(const char *arg)
{
    if (arg && *arg)
        editor_open(arg);
    else
        screenshot_start(SHOT_PANEL);
}

static const struct gui_app screenshot_app = {"Screenshot", "Capture the screen, a window or an area; crop and mark up",
                                              ICON_SCREENSHOT, launch, NULL, NULL};
GUI_APP(screenshot_app);

COMMAND(screenshot, NULL, "system", "screenshot [-w|-r|-p]", "capture the screen (or a window, an area, the bar)")
{
    if (!gui_active())
        return sh_error(sh, "screenshot", "the desktop is not running");
    enum shot_mode mode = SHOT_SCREEN;
    if (argc > 1 && !strcmp(argv[1], "-w"))
        mode = SHOT_WINDOW;
    else if (argc > 1 && !strcmp(argv[1], "-r"))
        mode = SHOT_REGION;
    else if (argc > 1 && !strcmp(argv[1], "-p"))
        mode = SHOT_PANEL;
    screenshot_start(mode);
    return 0;
}
