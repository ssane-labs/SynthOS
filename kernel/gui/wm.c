#include <synth/fbcon.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "internal.h"

#define MAX_DIRTY 48
#define RADIUS 8
#define SHADOW 14
#define BUTTON_W 40
#define DOUBLE_CLICK_MS 400
#define CURSOR_W 12
#define CURSOR_H 19

static struct framebuffer fb;
static struct canvas back;
static int scale = 1;
static int screen_w, screen_h;
static struct window *bottom, *top, *focus;
static struct rect dirty[MAX_DIRTY];
static int dirty_count;
static bool rendering;

int mouse_x, mouse_y;
static unsigned mouse_buttons;
static bool cursor_visible = true;

enum drag_mode {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE,
    DRAG_CLIENT,
    DRAG_TITLE_BUTTON,
};

static enum drag_mode drag;
static struct window *drag_win;
static int drag_x, drag_y, drag_edges;
static struct rect drag_frame;
static struct window *hover_win;
static int hover_button = -1, pressed_button = -1;
static uint64_t last_click_time;
static int last_click_x, last_click_y, click_count;
static struct window *last_click_win;

static const char *const cursor_shape[CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ",
    "      XXXX  ",
};

static uint32_t pack(uint32_t c)
{
    uint32_t r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    if (fb.bpp == 16)
        return (r >> 3) << fb.red_shift | (g >> 2) << fb.green_shift | (b >> 3) << fb.blue_shift;
    return r << fb.red_shift | g << fb.green_shift | b << fb.blue_shift;
}

static void flush(struct rect r)
{
    r = rect_intersect(r, (struct rect){0, 0, screen_w, screen_h});
    if (rect_empty(r))
        return;
    bool native = fb.bpp == 32 && fb.red_shift == 16 && fb.green_shift == 8 && fb.blue_shift == 0;
    uint32_t bytes = fb.bpp / 8;
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = back.px + y * back.stride + r.x;
        for (int sy = 0; sy < scale; sy++) {
            uint8_t *line = fb.base + (size_t)(y * scale + sy) * fb.pitch + (size_t)r.x * scale * bytes;
            if (native && scale == 1) {
                memcpy(line, src, (size_t)r.w * 4);
                continue;
            }
            for (int x = 0; x < r.w; x++) {
                uint32_t p = native ? src[x] : pack(src[x]);
                for (int sx = 0; sx < scale; sx++) {
                    uint8_t *d = line + (size_t)(x * scale + sx) * bytes;
                    if (bytes == 4)
                        *(uint32_t *)d = p;
                    else if (bytes == 3) {
                        d[0] = (uint8_t)p;
                        d[1] = (uint8_t)(p >> 8);
                        d[2] = (uint8_t)(p >> 16);
                    } else {
                        *(uint16_t *)d = (uint16_t)p;
                    }
                }
            }
        }
    }
}

void wm_damage(struct rect r)
{
    r = rect_intersect(r, (struct rect){0, 0, screen_w, screen_h});
    if (rect_empty(r))
        return;
    for (int i = 0; i < dirty_count; i++) {
        struct rect u = rect_union(dirty[i], r);
        if (u.w * u.h <= dirty[i].w * dirty[i].h + r.w * r.h + 4096) {
            dirty[i] = u;
            return;
        }
    }
    if (dirty_count == MAX_DIRTY) {
        for (int i = 1; i < dirty_count; i++)
            dirty[0] = rect_union(dirty[0], dirty[i]);
        dirty_count = 1;
        dirty[0] = rect_union(dirty[0], r);
        return;
    }
    dirty[dirty_count++] = r;
}

void wm_screen_size(int *w, int *h)
{
    if (w)
        *w = screen_w;
    if (h)
        *h = screen_h;
}

static bool visible(const struct window *w)
{
    return !(w->flags & (WIN_HIDDEN | WIN_MINIMIZED));
}

static bool framed(const struct window *w)
{
    return !(w->flags & WIN_POPUP);
}

struct rect wm_client_rect(const struct window *w)
{
    if (!framed(w))
        return w->frame;
    return (struct rect){w->frame.x + BORDER, w->frame.y + TITLE_HEIGHT, w->frame.w - 2 * BORDER,
                         w->frame.h - TITLE_HEIGHT - BORDER};
}

static struct rect outer_rect(const struct window *w)
{
    if (!framed(w))
        return rect_union(w->frame, (struct rect){w->frame.x - 8, w->frame.y - 4, w->frame.w + 16, w->frame.h + 16});
    if (w->flags & WIN_MAXIMIZED)
        return w->frame;
    return (struct rect){w->frame.x - SHADOW, w->frame.y - SHADOW, w->frame.w + 2 * SHADOW, w->frame.h + 2 * SHADOW};
}

static void damage_window(struct window *w)
{
    wm_damage(outer_rect(w));
}

bool wm_window_alive(const struct window *w)
{
    for (struct window *it = bottom; it; it = it->above)
        if (it == w)
            return true;
    return false;
}

static void unlink(struct window *w)
{
    if (w->below)
        w->below->above = w->above;
    else
        bottom = w->above;
    if (w->above)
        w->above->below = w->below;
    else
        top = w->below;
    w->above = w->below = NULL;
}

static void link_top(struct window *w)
{
    w->below = top;
    w->above = NULL;
    if (top)
        top->above = w;
    else
        bottom = w;
    top = w;
}

static void raise(struct window *w)
{
    if (top == w)
        return;
    unlink(w);
    link_top(w);
    struct window *popup_owner = w;
    for (struct window *it = bottom; it;) {
        struct window *next = it->above;
        if (it != w && (it->flags & WIN_POPUP) && it->owner == popup_owner) {
            unlink(it);
            link_top(it);
        }
        it = next;
    }
    damage_window(w);
}

static void send(struct window *w, struct gui_event *e)
{
    if (w && w->ops && w->ops->event)
        w->ops->event(w, e);
}

struct window *wm_focused(void)
{
    return focus;
}

void wm_focus(struct window *w)
{
    if (w && !(w->flags & WIN_POPUP)) {
        raise(w);
    }
    if (focus == w)
        return;
    struct window *old = focus;
    focus = w;
    if (old && wm_window_alive(old)) {
        send(old, &(struct gui_event){.type = EV_BLUR});
        damage_window(old);
    }
    if (w) {
        send(w, &(struct gui_event){.type = EV_FOCUS});
        damage_window(w);
    }
    taskbar_changed();
}

static bool alloc_surface(struct window *w)
{
    struct rect c = wm_client_rect(w);
    struct canvas fresh;
    if (!canvas_alloc(&fresh, MAX(c.w, 1), MAX(c.h, 1)))
        return false;
    if (w->surface.px)
        canvas_free(&w->surface);
    w->surface = fresh;
    w->dirty = true;
    return true;
}

static void place(struct window *w)
{
    static int cascade;
    int work_h = screen_h - TASKBAR_HEIGHT;
    int x = (screen_w - w->frame.w) / 2 + (cascade % 6) * 26 - 65;
    int y = (work_h - w->frame.h) / 2 + (cascade % 6) * 26 - 65;
    cascade++;
    w->frame.x = CLAMP(MAX(x, 110), 0, MAX(0, screen_w - w->frame.w));
    w->frame.y = CLAMP(y, 0, MAX(0, work_h - w->frame.h));
}

struct window *wm_open(const char *title, enum icon_id icon, int cw, int ch, const struct window_ops *ops, void *data,
                       unsigned flags)
{
    struct window *w = kzalloc(sizeof(*w));
    if (!w)
        return NULL;
    str_copy(w->title, title, sizeof(w->title));
    w->icon = icon;
    w->ops = ops;
    w->data = data;
    w->flags = flags;
    w->min_w = 200;
    w->min_h = 120;
    int max_w = screen_w - 2 * BORDER, max_h = screen_h - TASKBAR_HEIGHT - TITLE_HEIGHT - BORDER;
    w->frame.w = MIN(cw, max_w) + 2 * BORDER;
    w->frame.h = MIN(ch, max_h) + TITLE_HEIGHT + BORDER;
    place(w);
    if (!alloc_surface(w)) {
        kfree(w);
        return NULL;
    }
    link_top(w);
    if (!(flags & WIN_HIDDEN))
        wm_focus(w);
    damage_window(w);
    taskbar_changed();
    return w;
}

struct window *wm_popup(int x, int y, int pw, int ph, const struct window_ops *ops, void *data, struct window *owner)
{
    struct window *w = kzalloc(sizeof(*w));
    if (!w)
        return NULL;
    w->ops = ops;
    w->data = data;
    w->flags = WIN_POPUP;
    w->owner = owner;
    w->frame = (struct rect){CLAMP(x, 0, MAX(0, screen_w - pw)), CLAMP(y, 0, MAX(0, screen_h - ph)), pw, ph};
    if (!alloc_surface(w)) {
        kfree(w);
        return NULL;
    }
    link_top(w);
    damage_window(w);
    return w;
}

void wm_destroy(struct window *w)
{
    if (!wm_window_alive(w))
        return;
    for (struct window *it = bottom; it;) {
        struct window *next = it->above;
        if (it->owner == w)
            wm_destroy(it);
        it = next;
    }
    if (!wm_window_alive(w))
        return;
    damage_window(w);
    unlink(w);
    if (drag_win == w) {
        drag = DRAG_NONE;
        drag_win = NULL;
    }
    if (hover_win == w)
        hover_win = NULL;
    if (last_click_win == w)
        last_click_win = NULL;
    if (w->ops && w->ops->destroy)
        w->ops->destroy(w);
    canvas_free(&w->surface);
    if (focus == w) {
        focus = NULL;
        struct window *next = w->owner && wm_window_alive(w->owner) ? w->owner : NULL;
        for (struct window *it = top; it && !next; it = it->below)
            if (visible(it) && !(it->flags & WIN_POPUP))
                next = it;
        if (next)
            wm_focus(next);
    }
    kfree(w);
    taskbar_changed();
}

void wm_close(struct window *w)
{
    if (!wm_window_alive(w))
        return;
    if (w->ops && w->ops->close && !w->ops->close(w))
        return;
    if (w->flags & WIN_KEEP) {
        wm_hide(w);
        return;
    }
    wm_destroy(w);
}

void wm_hide(struct window *w)
{
    w->flags |= WIN_HIDDEN;
    damage_window(w);
    if (focus == w) {
        focus = NULL;
        for (struct window *it = top; it; it = it->below)
            if (visible(it) && !(it->flags & WIN_POPUP)) {
                wm_focus(it);
                break;
            }
    }
    taskbar_changed();
}

void wm_show(struct window *w)
{
    w->flags &= ~(WIN_HIDDEN | WIN_MINIMIZED);
    wm_focus(w);
    damage_window(w);
    taskbar_changed();
}

void wm_minimize(struct window *w)
{
    w->flags |= WIN_MINIMIZED;
    damage_window(w);
    if (focus == w) {
        focus = NULL;
        for (struct window *it = top; it; it = it->below)
            if (visible(it) && !(it->flags & WIN_POPUP)) {
                wm_focus(it);
                break;
            }
    }
    taskbar_changed();
}

void wm_invalidate(struct window *w)
{
    if (w && wm_window_alive(w))
        w->dirty = true;
}

void wm_set_title(struct window *w, const char *title)
{
    str_copy(w->title, title, sizeof(w->title));
    wm_damage((struct rect){w->frame.x, w->frame.y, w->frame.w, TITLE_HEIGHT});
    taskbar_changed();
}

void wm_set_timer(struct window *w, uint32_t ms)
{
    w->timer_ms = ms;
    w->next_timer = hal_ticks_ms() + ms;
}

void wm_move(struct window *w, int x, int y)
{
    damage_window(w);
    w->frame.x = x;
    w->frame.y = y;
    damage_window(w);
}

static void set_frame(struct window *w, struct rect frame)
{
    damage_window(w);
    bool resized = frame.w != w->frame.w || frame.h != w->frame.h;
    w->frame = frame;
    if (resized) {
        alloc_surface(w);
        send(w, &(struct gui_event){.type = EV_RESIZE});
    }
    damage_window(w);
}

void wm_resize_client(struct window *w, int cw, int ch)
{
    struct rect f = w->frame;
    f.w = cw + 2 * BORDER;
    f.h = ch + TITLE_HEIGHT + BORDER;
    set_frame(w, f);
}

void wm_toggle_maximize(struct window *w)
{
    if (!(w->flags & WIN_RESIZABLE))
        return;
    if (w->flags & WIN_MAXIMIZED) {
        w->flags &= ~WIN_MAXIMIZED;
        set_frame(w, w->restore);
    } else {
        w->restore = w->frame;
        w->flags |= WIN_MAXIMIZED;
        set_frame(w, (struct rect){0, 0, screen_w, screen_h - TASKBAR_HEIGHT});
    }
}

void wm_foreach(void (*fn)(struct window *w, void *ctx), void *ctx)
{
    for (struct window *it = bottom; it;) {
        struct window *next = it->above;
        fn(it, ctx);
        it = next;
    }
}

static struct rect title_button(const struct window *w, int index)
{
    return (struct rect){w->frame.x + w->frame.w - BORDER - (3 - index) * BUTTON_W, w->frame.y + BORDER, BUTTON_W,
                         TITLE_HEIGHT - BORDER};
}

static void draw_button_glyph(struct canvas *c, struct rect b, int index, uint32_t color)
{
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (index == 0) {
        gfx_fill(c, (struct rect){cx - 5, cy, 11, 1}, color);
    } else if (index == 1) {
        gfx_frame(c, (struct rect){cx - 5, cy - 5, 11, 11}, color);
    } else {
        for (int i = -5; i <= 5; i++) {
            gfx_pixel(c, cx + i, cy + i, color);
            gfx_pixel(c, cx + i, cy - i, color);
            gfx_pixel(c, cx + i + 1, cy + i, ALPHA(color, 110));
            gfx_pixel(c, cx + i + 1, cy - i, ALPHA(color, 110));
        }
    }
}

static void draw_frame(struct canvas *c, struct window *w)
{
    bool active = w == focus;
    bool max = w->flags & WIN_MAXIMIZED;
    int radius = max ? 0 : RADIUS;
    if (!max)
        gfx_shadow(c, w->frame, radius, SHADOW, active ? 120 : 70);
    gfx_round(c, w->frame, radius, style.window);
    struct rect title = {w->frame.x, w->frame.y, w->frame.w, TITLE_HEIGHT + radius};
    if (active)
        gfx_round_gradient(c, title, radius, style.title_active_top, style.title_active_bottom);
    else
        gfx_round(c, title, radius, style.title_inactive);
    gfx_icon(c, w->frame.x + 10, w->frame.y + (TITLE_HEIGHT - 16) / 2, w->icon, 16);
    int buttons = w->flags & WIN_DIALOG ? 1 : 3;
    struct rect text = {w->frame.x + 34, w->frame.y, w->frame.w - 34 - buttons * BUTTON_W - 8, TITLE_HEIGHT};
    gfx_text_box(c, text, &font_bold, w->title, active ? style.title_text : ALPHA(style.title_text, 150), ALIGN_LEFT);
    for (int i = 3 - buttons; i < 3; i++) {
        if (i == 1 && !(w->flags & WIN_RESIZABLE))
            continue;
        struct rect b = title_button(w, i);
        bool hot = hover_win == w && hover_button == i;
        if (hot)
            gfx_fill(c, b, i == 2 ? RGB(232, 17, 35) : ALPHA(0xffffffffu, pressed_button == i ? 70 : 40));
        draw_button_glyph(c, b, i, hot && i == 2 ? 0xffffffffu : style.title_text);
    }
}

static void draw_client(struct canvas *c, struct window *w)
{
    struct rect cr = wm_client_rect(w);
    if (!framed(w) || (w->flags & WIN_MAXIMIZED)) {
        gfx_blit(c, cr.x, cr.y, &w->surface, (struct rect){0, 0, w->surface.w, w->surface.h});
        return;
    }
    int body_h = MAX(0, MIN(cr.h, w->surface.h) - RADIUS);
    gfx_blit(c, cr.x, cr.y, &w->surface, (struct rect){0, 0, MIN(cr.w, w->surface.w), body_h});
    struct rect bottom_part = rect_intersect((struct rect){cr.x, cr.y + body_h, cr.w, cr.h - body_h}, c->clip);
    struct rect shape = {w->frame.x, w->frame.y, w->frame.w, w->frame.h};
    for (int y = bottom_part.y; y < bottom_part.y + bottom_part.h; y++)
        for (int x = bottom_part.x; x < bottom_part.x + bottom_part.w; x++) {
            int sx = x - cr.x, sy = y - cr.y;
            if (sx >= w->surface.w || sy >= w->surface.h)
                continue;
            int dx = x < shape.x + RADIUS ? shape.x + RADIUS - x : x >= shape.x + shape.w - RADIUS ? x - (shape.x + shape.w - RADIUS - 1) : 0;
            int dy = y - (shape.y + shape.h - RADIUS - 1);
            if (dx && dy > 0 && dx * dx + dy * dy > RADIUS * RADIUS)
                continue;
            c->px[y * c->stride + x] = w->surface.px[sy * w->surface.stride + sx];
        }
    if (!(w->flags & WIN_MAXIMIZED))
        gfx_round_frame(c, w->frame, RADIUS, w == focus ? ALPHA(style.accent, 150) : style.border);
}

static void draw_window(struct canvas *c, struct window *w)
{
    if (!framed(w)) {
        gfx_shadow(c, w->frame, 6, 10, 110);
        draw_client(c, w);
        return;
    }
    draw_frame(c, w);
    draw_client(c, w);
}

static void draw_cursor(struct canvas *c)
{
    if (!cursor_visible)
        return;
    for (int y = 0; y < CURSOR_H; y++)
        for (int x = 0; x < CURSOR_W; x++) {
            char p = cursor_shape[y][x];
            if (p == 'X')
                gfx_pixel(c, mouse_x + x, mouse_y + y, RGB(20, 20, 30));
            else if (p == '.')
                gfx_pixel(c, mouse_x + x, mouse_y + y, RGB(255, 255, 255));
        }
}

static void compose(struct rect area)
{
    canvas_clip(&back, area);
    desktop_paint(&back);
    for (struct window *w = bottom; w; w = w->above)
        if (visible(w) && !(w->flags & WIN_POPUP) && !rect_empty(rect_intersect(outer_rect(w), area)))
            draw_window(&back, w);
    taskbar_paint(&back);
    for (struct window *w = bottom; w; w = w->above)
        if (visible(w) && (w->flags & WIN_POPUP) && !rect_empty(rect_intersect(outer_rect(w), area)))
            draw_window(&back, w);
    draw_cursor(&back);
}

void wm_render(void)
{
    if (rendering || !back.px)
        return;
    rendering = true;
    for (struct window *w = bottom; w; w = w->above)
        if (w->dirty && visible(w) && w->ops && w->ops->paint) {
            w->dirty = false;
            struct canvas c = w->surface;
            c.clip = (struct rect){0, 0, c.w, c.h};
            w->ops->paint(w, &c);
            wm_damage(wm_client_rect(w));
        }
    int count = dirty_count;
    struct rect list[MAX_DIRTY];
    memcpy(list, dirty, sizeof(struct rect) * (size_t)count);
    dirty_count = 0;
    for (int i = 0; i < count; i++) {
        compose(list[i]);
        flush(list[i]);
    }
    rendering = false;
}

static struct window *window_at(int x, int y)
{
    for (struct window *w = top; w; w = w->below)
        if (visible(w) && rect_contains(w->frame, x, y))
            return w;
    return NULL;
}

static int edges_at(struct window *w, int x, int y)
{
    if (!(w->flags & WIN_RESIZABLE) || (w->flags & WIN_MAXIMIZED) || !framed(w))
        return 0;
    int edges = 0;
    if (x >= w->frame.x + w->frame.w - 6)
        edges |= 1;
    if (y >= w->frame.y + w->frame.h - 6)
        edges |= 2;
    if (x < w->frame.x + 4)
        edges |= 4;
    return edges;
}

static int button_at(struct window *w, int x, int y)
{
    if (!framed(w) || y >= w->frame.y + TITLE_HEIGHT)
        return -1;
    int first = w->flags & WIN_DIALOG ? 2 : 0;
    for (int i = first; i < 3; i++) {
        if (i == 1 && !(w->flags & WIN_RESIZABLE))
            continue;
        if (rect_contains(title_button(w, i), x, y))
            return i;
    }
    return -1;
}

static void client_event(struct window *w, enum gui_event_type type, unsigned button, int wheel, int clicks)
{
    struct rect cr = wm_client_rect(w);
    struct gui_event e = {.type = type, .x = mouse_x - cr.x, .y = mouse_y - cr.y, .buttons = mouse_buttons,
                          .button = button, .wheel = wheel, .clicks = clicks};
    send(w, &e);
}

static void set_hover(struct window *w, int button)
{
    if (hover_win != w || hover_button != button) {
        if (hover_win && wm_window_alive(hover_win)) {
            wm_damage((struct rect){hover_win->frame.x, hover_win->frame.y, hover_win->frame.w, TITLE_HEIGHT});
            if (hover_win != w)
                send(hover_win, &(struct gui_event){.type = EV_LEAVE});
        }
        hover_win = w;
        hover_button = button;
        if (w)
            wm_damage((struct rect){w->frame.x, w->frame.y, w->frame.w, TITLE_HEIGHT});
    }
}

static void close_popups(struct window *except)
{
    for (struct window *w = bottom; w;) {
        struct window *next = w->above;
        if ((w->flags & WIN_POPUP) && w != except && !(except && except->owner == w))
            wm_destroy(w);
        w = next;
    }
}

static void mouse_down(unsigned button)
{
    struct window *w = window_at(mouse_x, mouse_y);
    if (!w || !(w->flags & WIN_POPUP))
        close_popups(w);
    if (!w || (!(w->flags & WIN_POPUP) && taskbar_hit(mouse_x, mouse_y))) {
        if (taskbar_hit(mouse_x, mouse_y)) {
            taskbar_mouse(mouse_x, mouse_y, button, true);
            return;
        }
        wm_focus(NULL);
        desktop_mouse(mouse_x, mouse_y, button, true, click_count);
        return;
    }
    if (!(w->flags & WIN_POPUP))
        wm_focus(w);
    int b = button == BUTTON_LEFT ? button_at(w, mouse_x, mouse_y) : -1;
    if (b >= 0) {
        drag = DRAG_TITLE_BUTTON;
        drag_win = w;
        pressed_button = b;
        set_hover(w, b);
        return;
    }
    int edges = button == BUTTON_LEFT ? edges_at(w, mouse_x, mouse_y) : 0;
    if (edges) {
        drag = DRAG_RESIZE;
        drag_win = w;
        drag_edges = edges;
        drag_x = mouse_x;
        drag_y = mouse_y;
        drag_frame = w->frame;
        return;
    }
    if (framed(w) && mouse_y < w->frame.y + TITLE_HEIGHT) {
        if (button != BUTTON_LEFT)
            return;
        if (click_count >= 2) {
            wm_toggle_maximize(w);
            return;
        }
        drag = DRAG_MOVE;
        drag_win = w;
        drag_x = mouse_x - w->frame.x;
        drag_y = mouse_y - w->frame.y;
        return;
    }
    drag = DRAG_CLIENT;
    drag_win = w;
    client_event(w, EV_MOUSE_DOWN, button, 0, click_count);
}

static void mouse_up(unsigned button)
{
    struct window *w = drag_win;
    enum drag_mode mode = drag;
    drag = DRAG_NONE;
    drag_win = NULL;
    if (!w || !wm_window_alive(w)) {
        if (taskbar_hit(mouse_x, mouse_y))
            taskbar_mouse(mouse_x, mouse_y, button, false);
        else if (!window_at(mouse_x, mouse_y))
            desktop_mouse(mouse_x, mouse_y, button, false, click_count);
        return;
    }
    if (mode == DRAG_TITLE_BUTTON) {
        int b = pressed_button;
        pressed_button = -1;
        wm_damage((struct rect){w->frame.x, w->frame.y, w->frame.w, TITLE_HEIGHT});
        if (button_at(w, mouse_x, mouse_y) != b)
            return;
        if (b == 0)
            wm_minimize(w);
        else if (b == 1)
            wm_toggle_maximize(w);
        else
            wm_close(w);
        return;
    }
    if (mode == DRAG_CLIENT)
        client_event(w, EV_MOUSE_UP, button, 0, click_count);
}

static void mouse_moved(void)
{
    if (drag == DRAG_MOVE && drag_win) {
        if (drag_win->flags & WIN_MAXIMIZED) {
            int rel = drag_x * drag_win->restore.w / MAX(drag_win->frame.w, 1);
            wm_toggle_maximize(drag_win);
            drag_x = rel;
        }
        int y = CLAMP(mouse_y - drag_y, 0, screen_h - TASKBAR_HEIGHT - TITLE_HEIGHT);
        wm_move(drag_win, mouse_x - drag_x, y);
        return;
    }
    if (drag == DRAG_RESIZE && drag_win) {
        struct rect f = drag_frame;
        int dx = mouse_x - drag_x, dy = mouse_y - drag_y;
        if (drag_edges & 1)
            f.w = MAX(drag_win->min_w, drag_frame.w + dx);
        if (drag_edges & 2)
            f.h = MAX(drag_win->min_h, drag_frame.h + dy);
        if (drag_edges & 4) {
            int w = MAX(drag_win->min_w, drag_frame.w - dx);
            f.x = drag_frame.x + drag_frame.w - w;
            f.w = w;
        }
        set_frame(drag_win, f);
        return;
    }
    if (drag == DRAG_CLIENT && drag_win) {
        client_event(drag_win, EV_MOUSE_MOVE, 0, 0, 0);
        return;
    }
    struct window *w = window_at(mouse_x, mouse_y);
    if (w && drag != DRAG_TITLE_BUTTON) {
        set_hover(w, button_at(w, mouse_x, mouse_y));
        struct rect cr = wm_client_rect(w);
        if (rect_contains(cr, mouse_x, mouse_y))
            client_event(w, EV_MOUSE_MOVE, 0, 0, 0);
    } else if (!w) {
        set_hover(NULL, -1);
    }
    if (taskbar_hit(mouse_x, mouse_y) || !w)
        taskbar_hover(mouse_x, mouse_y);
}

void wm_pointer(const struct pointer_event *ev)
{
    int old_x = mouse_x, old_y = mouse_y;
    mouse_x = CLAMP(mouse_x + ev->dx, 0, screen_w - 1);
    mouse_y = CLAMP(mouse_y + ev->dy, 0, screen_h - 1);
    if (old_x != mouse_x || old_y != mouse_y) {
        wm_damage((struct rect){old_x, old_y, CURSOR_W, CURSOR_H});
        wm_damage((struct rect){mouse_x, mouse_y, CURSOR_W, CURSOR_H});
        mouse_moved();
    }
    unsigned changed = ev->buttons ^ mouse_buttons;
    for (unsigned b = 1; b <= 4; b <<= 1) {
        if (!(changed & b))
            continue;
        if (ev->buttons & b) {
            mouse_buttons |= b;
            uint64_t now = hal_ticks_ms();
            int dist = MAX(mouse_x > last_click_x ? mouse_x - last_click_x : last_click_x - mouse_x,
                           mouse_y > last_click_y ? mouse_y - last_click_y : last_click_y - mouse_y);
            struct window *at = window_at(mouse_x, mouse_y);
            if (b == BUTTON_LEFT && now - last_click_time < DOUBLE_CLICK_MS && dist < 5 && at == last_click_win)
                click_count++;
            else
                click_count = 1;
            if (b == BUTTON_LEFT) {
                last_click_time = now;
                last_click_x = mouse_x;
                last_click_y = mouse_y;
                last_click_win = at;
            }
            mouse_down(b);
        } else {
            mouse_buttons &= ~b;
            mouse_up(b);
        }
    }
    if (ev->wheel) {
        struct window *w = window_at(mouse_x, mouse_y);
        if (w)
            client_event(w, EV_WHEEL, 0, ev->wheel, 0);
    }
}

void wm_key(int key)
{
    if (focus) {
        struct gui_event e = {.type = EV_KEY, .key = key};
        send(focus, &e);
    }
}

void wm_cycle(void)
{
    for (struct window *w = bottom; w; w = w->above)
        if (w != focus && !(w->flags & (WIN_POPUP | WIN_HIDDEN))) {
            w->flags &= ~WIN_MINIMIZED;
            wm_focus(w);
            return;
        }
}

void wm_timers(void)
{
    uint64_t now = hal_ticks_ms();
    for (struct window *w = bottom; w;) {
        struct window *next = w->above;
        if (w->timer_ms && now >= w->next_timer) {
            w->next_timer = now + w->timer_ms;
            send(w, &(struct gui_event){.type = EV_TIMER});
        }
        w = next;
    }
}

void wm_close_popups(void)
{
    close_popups(NULL);
}

struct window *wm_top_popup(void)
{
    for (struct window *w = top; w; w = w->below)
        if ((w->flags & WIN_POPUP) && visible(w))
            return w;
    return NULL;
}

void wm_set_cursor_visible(bool on)
{
    cursor_visible = on;
    wm_damage((struct rect){mouse_x, mouse_y, CURSOR_W, CURSOR_H});
}

bool wm_start(const struct framebuffer *info)
{
    fb = *info;
    scale = fb.width >= 2400 ? 2 : 1;
    screen_w = (int)fb.width / scale;
    screen_h = (int)fb.height / scale;
    if (!canvas_alloc(&back, screen_w, screen_h))
        return false;
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    mouse_buttons = 0;
    dirty_count = 0;
    wm_damage((struct rect){0, 0, screen_w, screen_h});
    return true;
}

void wm_stop(void)
{
    while (bottom)
        wm_destroy(bottom);
    focus = NULL;
    canvas_free(&back);
}

void wm_damage_all(void)
{
    wm_damage((struct rect){0, 0, screen_w, screen_h});
}

int wm_window_list(struct window **out, int max)
{
    int n = 0;
    for (struct window *w = bottom; w && n < max; w = w->above)
        if (!(w->flags & (WIN_POPUP | WIN_HIDDEN | WIN_DIALOG)) || ((w->flags & WIN_DIALOG) && !(w->flags & WIN_HIDDEN) && !w->owner))
            out[n++] = w;
    return n;
}
