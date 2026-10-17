#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

#define MAX_SIDE 2400
#define MAX_FILES 256

enum {
    CMD_OPEN,
    CMD_PREV,
    CMD_NEXT,
    CMD_ZOOM_IN,
    CMD_ZOOM_OUT,
    CMD_FIT,
    CMD_ACTUAL,
    CMD_ROTATE,
    CMD_WALLPAPER,
    CMD_EDIT,
    CMD_SHOW,
    CMD_SLIDESHOW,
};

static const struct menu_item file_items[] = {
    {"Open…", KEY_COMMAND(CMD_OPEN), "⌘O"},
    {"Show in Files", KEY_COMMAND(CMD_SHOW), NULL},
    {"Edit in Paint", KEY_COMMAND(CMD_EDIT), NULL},
    {"Set as Wallpaper", KEY_COMMAND(CMD_WALLPAPER), NULL},
};

static const struct menu_item view_items[] = {
    {"Previous", KEY_COMMAND(CMD_PREV), "←"},       {"Next", KEY_COMMAND(CMD_NEXT), "→"},
    {"", 0, NULL},                                   {"Zoom In", KEY_COMMAND(CMD_ZOOM_IN), "+"},
    {"Zoom Out", KEY_COMMAND(CMD_ZOOM_OUT), "-"},    {"Fit to Window", KEY_COMMAND(CMD_FIT), "0"},
    {"Actual Size", KEY_COMMAND(CMD_ACTUAL), "1"},   {"Rotate", KEY_COMMAND(CMD_ROTATE), "R"},
    {"", 0, NULL},                                   {"Slideshow", KEY_COMMAND(CMD_SLIDESHOW), "Space"},
};

static const struct app_menu menus[] = {
    {"Picture", file_items, ARRAY_SIZE(file_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
};

struct photos {
    char path[PATH_MAX_LEN];
    char dir[PATH_MAX_LEN];
    char names[MAX_FILES][NAME_MAX_LEN];
    int count, index;
    struct picture pic;
    int frame;
    uint64_t frame_due;
    struct canvas fitted;
    int fitted_w, fitted_h;
    struct spring zoom, slide, fade;
    int zoom_target;
    bool fit;
    int pan_x, pan_y;
    bool panning;
    int drag_x, drag_y;
    uint64_t tick;
    bool slideshow;
    uint64_t slide_due;
    char error[96];
};

static void shrink(struct picture *p)
{
    int side = MAX(p->w, p->h);
    if (side <= MAX_SIDE || p->frames != 1)
        return;
    int w = (int)((int64_t)p->w * MAX_SIDE / side), h = (int)((int64_t)p->h * MAX_SIDE / side);
    uint32_t *px = kmalloc((size_t)w * (size_t)h * 4);
    if (!px)
        return;
    struct canvas dst = canvas_make(px, w, h), src = canvas_make(p->px, p->w, p->h);
    gfx_clear(&dst, (struct rect){0, 0, w, h}, 0);
    gfx_scale(&dst, (struct rect){0, 0, w, h}, &src, 255);
    kfree(p->px);
    p->px = px;
    p->w = w;
    p->h = h;
}

static void rotate(struct photos *ph)
{
    struct picture *p = &ph->pic;
    size_t n = (size_t)p->w * (size_t)p->h;
    uint32_t *px = kmalloc(n * 4 * (size_t)p->frames);
    if (!px)
        return;
    for (int f = 0; f < p->frames; f++)
        for (int y = 0; y < p->h; y++)
            for (int x = 0; x < p->w; x++)
                px[f * n + (size_t)x * (size_t)p->h + (size_t)(p->h - 1 - y)] = p->px[f * n + (size_t)y * (size_t)p->w + (size_t)x];
    kfree(p->px);
    p->px = px;
    int t = p->w;
    p->w = p->h;
    p->h = t;
    canvas_free(&ph->fitted);
}

static void scan_dir(struct photos *ph)
{
    ph->count = 0;
    struct vdir d;
    if (vfs_opendir(ph->dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (ph->count < MAX_FILES && vfs_readdir(&d, &ent) == 0)
        if (ent.type != VN_DIR && ent.name[0] != '.' && picture_is_image(ent.name)) {
            int i = ph->count++;
            while (i > 0 && str_casecmp(ph->names[i - 1], ent.name) > 0) {
                str_copy(ph->names[i], ph->names[i - 1], NAME_MAX_LEN);
                i--;
            }
            str_copy(ph->names[i], ent.name, NAME_MAX_LEN);
        }
    vfs_closedir(&d);
}

static void show(struct window *w, struct photos *ph, const char *path, int direction)
{
    picture_free(&ph->pic);
    canvas_free(&ph->fitted);
    ph->error[0] = 0;
    str_copy(ph->path, path, sizeof(ph->path));
    int err = picture_load(path, &ph->pic);
    if (err) {
        ksnprintf(ph->error, sizeof(ph->error), "Cannot open this picture (%s)",
                  err == E_INVAL ? "unknown format" : err == E_NOSYS ? "unsupported kind of JPEG" : err_str(err));
    } else {
        shrink(&ph->pic);
    }
    ph->frame = 0;
    ph->frame_due = hal_ticks_ms() + (ph->pic.delays ? (uint64_t)ph->pic.delays[0] : 0);
    ph->fit = true;
    ph->pan_x = ph->pan_y = 0;
    spring_set(&ph->zoom, 1024);
    spring_set(&ph->slide, direction * 160);
    ph->slide.target = 0;
    spring_set(&ph->fade, direction ? 200 : 0);
    ph->fade.target = 1024;
    char title[96];
    str_copy(title, path_basename(path), sizeof(title));
    wm_set_title(w, title);
    wm_invalidate(w);
}

static void open_index(struct window *w, struct photos *ph, int i, int direction)
{
    if (!ph->count)
        return;
    ph->index = (i + ph->count) % ph->count;
    char path[PATH_MAX_LEN];
    path_join(ph->dir, ph->names[ph->index], path, sizeof(path));
    show(w, ph, path, direction);
}

static void open_path(struct window *w, struct photos *ph, const char *path)
{
    if (vfs_is_dir(path)) {
        str_copy(ph->dir, path, sizeof(ph->dir));
        scan_dir(ph);
        if (ph->count)
            open_index(w, ph, 0, 0);
        else
            wm_invalidate(w);
        return;
    }
    path_dirname(path, ph->dir, sizeof(ph->dir));
    scan_dir(ph);
    ph->index = 0;
    for (int i = 0; i < ph->count; i++)
        if (!strcmp(ph->names[i], path_basename(path)))
            ph->index = i;
    show(w, ph, path, 0);
}

static struct rect image_rect(struct photos *ph, int W, int H, int *scale)
{
    int pw = ph->pic.w, ph_ = ph->pic.h;
    int fit = MIN(W * 1024 / MAX(pw, 1), H * 1024 / MAX(ph_, 1));
    fit = MIN(fit, 1024 * 4);
    if (pw <= W && ph_ <= H)
        fit = MIN(fit, 1024);
    int s = (int)((int64_t)fit * ph->zoom.pos / 1024);
    *scale = s;
    int w = (int)((int64_t)pw * s / 1024), h = (int)((int64_t)ph_ * s / 1024);
    return (struct rect){(W - w) / 2 + ph->pan_x + ph->slide.pos * W / 1024 / 4, (H - h) / 2 + ph->pan_y, w, h};
}

static void paint(struct window *w, struct canvas *c)
{
    struct photos *ph = w->data;
    int W = c->w, H = c->h;
    gfx_clear(c, (struct rect){0, 0, W, H}, style.dark ? RGB(12, 12, 14) : color_mix(RGB(24, 24, 28), style.tint | 0xff000000u, 20));
    if (!ph->pic.px) {
        const char *msg = ph->error[0] ? ph->error : "No pictures here";
        gfx_symbol(c, W / 2 - 16, H / 2 - 40, SYM_PHOTO, 32, ALPHA(0xffffffffu, 120));
        gfx_text_box(c, (struct rect){0, H / 2, W, 22}, &font_bold, msg, ALPHA(0xffffffffu, 200), ALIGN_CENTER);
        gfx_text_box(c, (struct rect){0, H / 2 + 24, W, 18}, &font_small, "Open a PNG, JPEG, GIF or BMP file from the ⋯ menu",
                     ALPHA(0xffffffffu, 120), ALIGN_CENTER);
        return;
    }
    int scale;
    struct rect r = image_rect(ph, W, H, &scale);
    struct canvas src = canvas_make(ph->pic.px + (size_t)ph->frame * (size_t)ph->pic.w * (size_t)ph->pic.h, ph->pic.w,
                                    ph->pic.h);
    int alpha = CLAMP(ph->fade.pos, 0, 1024) * 255 / 1024;
    bool still = !spring_busy(&ph->zoom) && !spring_busy(&ph->slide) && ph->pic.frames == 1;
    if (still && r.w <= ph->pic.w) {
        if (!ph->fitted.px || ph->fitted_w != r.w || ph->fitted_h != r.h) {
            canvas_free(&ph->fitted);
            if (canvas_alloc(&ph->fitted, MAX(r.w, 1), MAX(r.h, 1))) {
                gfx_clear(&ph->fitted, (struct rect){0, 0, r.w, r.h}, 0);
                gfx_scale(&ph->fitted, (struct rect){0, 0, r.w, r.h}, &src, 255);
                ph->fitted_w = r.w;
                ph->fitted_h = r.h;
            }
        }
        if (ph->fitted.px) {
            gfx_drop_shadow(c, r, 4, 8, 60 * alpha / 255, 2);
            gfx_blit_alpha(c, r.x, r.y, &ph->fitted, (struct rect){0, 0, r.w, r.h}, alpha);
            return;
        }
    }
    gfx_drop_shadow(c, r, 4, 8, 60 * alpha / 255, 2);
    gfx_scale_fast(c, r, &src, alpha);
}

static void command(struct window *w, struct photos *ph, int cmd);

static void picked(struct window *owner, const char *value)
{
    if (wm_window_alive(owner) && value && value[0])
        open_path(owner, owner->data, value);
}

static void zoom_to(struct photos *ph, int z)
{
    ph->zoom.target = CLAMP(z, 256, 8192);
    if (ph->zoom.target == 1024) {
        ph->pan_x = ph->pan_y = 0;
    }
}

static void command(struct window *w, struct photos *ph, int cmd)
{
    switch (cmd) {
    case CMD_OPEN: ui_prompt(w, "Open picture", "Path to a picture or a folder:", ph->dir, picked); break;
    case CMD_PREV: open_index(w, ph, ph->index - 1, -1); break;
    case CMD_NEXT: open_index(w, ph, ph->index + 1, 1); break;
    case CMD_ZOOM_IN: zoom_to(ph, ph->zoom.target * 3 / 2); break;
    case CMD_ZOOM_OUT: zoom_to(ph, ph->zoom.target * 2 / 3); break;
    case CMD_FIT: zoom_to(ph, 1024); break;
    case CMD_ACTUAL: {
        int scale;
        image_rect(ph, w->surface.w, w->surface.h, &scale);
        int fit = scale * 1024 / MAX(ph->zoom.pos, 1);
        zoom_to(ph, fit ? 1024 * 1024 / fit : 1024);
        break;
    }
    case CMD_ROTATE:
        if (ph->pic.px)
            rotate(ph);
        break;
    case CMD_WALLPAPER:
        if (ph->pic.px) {
            char line[PATH_MAX_LEN + 24];
            ksnprintf(line, sizeof(line), "config wallpaper '%s'", ph->path);
            gui_terminal_run(line);
        }
        break;
    case CMD_EDIT: gui_app_launch(gui_app_find("Paint"), ph->path); break;
    case CMD_SHOW: gui_app_launch(gui_app_find("Files"), ph->dir); break;
    case CMD_SLIDESHOW:
        ph->slideshow = !ph->slideshow;
        ph->slide_due = hal_ticks_ms() + 3000;
        break;
    }
    wm_invalidate(w);
}

static void event(struct window *w, struct gui_event *e)
{
    struct photos *ph = w->data;
    switch (e->type) {
    case EV_KEY: {
        int k = e->key;
        if (k >= KEY_COMMAND(0) && k <= KEY_COMMAND(CMD_SLIDESHOW))
            command(w, ph, k - KEY_COMMAND(0));
        else if (k == KEY_LEFT)
            command(w, ph, CMD_PREV);
        else if (k == KEY_RIGHT)
            command(w, ph, CMD_NEXT);
        else if (k == '+' || k == '=')
            command(w, ph, CMD_ZOOM_IN);
        else if (k == '-')
            command(w, ph, CMD_ZOOM_OUT);
        else if (k == '0')
            command(w, ph, CMD_FIT);
        else if (k == '1')
            command(w, ph, CMD_ACTUAL);
        else if (k == 'r' || k == 'R')
            command(w, ph, CMD_ROTATE);
        else if (k == ' ')
            command(w, ph, CMD_SLIDESHOW);
        else if (k == KEY_CTRL('o'))
            command(w, ph, CMD_OPEN);
        else if (k == KEY_ESC && ph->slideshow)
            ph->slideshow = false;
        break;
    }
    case EV_WHEEL: command(w, ph, e->wheel < 0 ? CMD_ZOOM_IN : CMD_ZOOM_OUT); break;
    case EV_MOUSE_DOWN:
        if (e->button == BUTTON_LEFT && e->clicks >= 2) {
            command(w, ph, ph->zoom.target == 1024 ? CMD_ACTUAL : CMD_FIT);
        } else if (e->button == BUTTON_LEFT) {
            ph->panning = true;
            ph->drag_x = e->x;
            ph->drag_y = e->y;
        } else if (e->button == BUTTON_RIGHT) {
            struct rect cr = wm_client_rect(w);
            wm_window_menu(w, cr.x + e->x, cr.y + e->y);
        }
        e->handled = true;
        break;
    case EV_MOUSE_MOVE:
        if (ph->panning && ph->zoom.target > 1024) {
            ph->pan_x += e->x - ph->drag_x;
            ph->pan_y += e->y - ph->drag_y;
            wm_invalidate(w);
        }
        ph->drag_x = e->x;
        ph->drag_y = e->y;
        break;
    case EV_MOUSE_UP: ph->panning = false; break;
    case EV_TIMER: {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - ph->tick, 64u);
        ph->tick = now;
        bool busy = spring_step(&ph->zoom, 360, 34, dt);
        busy |= spring_step(&ph->slide, 260, 30, dt);
        busy |= spring_step(&ph->fade, 300, 40, dt);
        if (ph->pic.frames > 1 && now >= ph->frame_due) {
            ph->frame = (ph->frame + 1) % ph->pic.frames;
            ph->frame_due = now + (uint64_t)ph->pic.delays[ph->frame];
            busy = true;
        }
        if (ph->slideshow && now >= ph->slide_due) {
            ph->slide_due = now + 3000;
            command(w, ph, CMD_NEXT);
        }
        if (busy)
            wm_invalidate(w);
        break;
    }
    case EV_RESIZE: canvas_free(&ph->fitted); break;
    default: break;
    }
}

static void destroy(struct window *w)
{
    struct photos *ph = w->data;
    picture_free(&ph->pic);
    canvas_free(&ph->fitted);
    kfree(ph);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    struct photos *ph = kzalloc(sizeof(*ph));
    if (!ph)
        return;
    ph->tick = hal_ticks_ms();
    spring_set(&ph->zoom, 1024);
    spring_set(&ph->fade, 1024);
    struct window *w = wm_open("Photos", ICON_PHOTOS, 760, 520, &ops, ph, WIN_RESIZABLE);
    if (!w) {
        kfree(ph);
        return;
    }
    w->min_w = 300;
    w->min_h = 220;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    wm_set_timer(w, 16);
    char pictures[PATH_MAX_LEN];
    path_join(user_home(), "Pictures", pictures, sizeof(pictures));
    open_path(w, ph, arg && *arg ? arg : pictures);
}

static const struct gui_app photos_app = {"Photos", "View pictures: PNG, JPEG, GIF and BMP", ICON_PHOTOS, launch,
                                          ".png;.jpg;.jpeg;.gif;.bmp", NULL};
GUI_APP(photos_app);
