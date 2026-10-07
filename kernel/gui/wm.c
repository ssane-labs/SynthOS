#include <synth/error.h>
#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

#define MAX_DIRTY 32
#define RADIUS 12
#define DOUBLE_CLICK_MS 400
#define EDGE 6
#define SHADOW 34
#define SHADOW_DY 12
#define CURSOR_SIZE 28
#define THUMB_MAX 128

struct screen {
    struct framebuffer fb;
    struct rect area;
    int scale;
    uint32_t *mem;
    struct canvas back;
    struct rect dirty[MAX_DIRTY];
    int dirty_count;
};

static struct screen screens[MAX_SCREENS];
static int screen_count;
static struct rect compose_area;
static struct window *bottom, *top, *focus;
static bool rendering;
static const struct gui_app *launching_app;

int mouse_x, mouse_y;
static unsigned mouse_buttons;
static int mouse_speed = 5, accel_x, accel_y;
static bool natural_scroll;
static bool cursor_visible = true;

enum cursor_kind {
    CUR_ARROW,
    CUR_EW,
    CUR_NS,
    CUR_NWSE,
    CUR_NESW,
    CUR_COUNT,
};

static uint32_t *cursor_px[CUR_COUNT];
static int cursor_hot[CUR_COUNT][2];
static int cursor_kind;

enum drag_mode {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE,
    DRAG_CLIENT,
    DRAG_TRAFFIC,
    DRAG_CHROME,
};

static enum drag_mode drag;
static struct window *drag_win;
static int drag_x, drag_y, drag_edges;
static struct rect drag_frame;
static struct window *hover_win, *traffic_win;
static int pressed_light = -1;
static uint64_t last_click_time;
static int last_click_x, last_click_y, click_count;
static struct window *last_click_win;

static uint32_t pack(const struct framebuffer *fb, uint32_t c)
{
    uint32_t r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    if (fb->bpp == 16)
        return (r >> 3) << fb->red_shift | (g >> 2) << fb->green_shift | (b >> 3) << fb->blue_shift;
    return r << fb->red_shift | g << fb->green_shift | b << fb->blue_shift;
}

static void flush(struct screen *s, struct rect r)
{
    r = rect_intersect(r, s->area);
    if (rect_empty(r))
        return;
    const struct framebuffer *fb = &s->fb;
    bool native = fb->bpp == 32 && fb->red_shift == 16 && fb->green_shift == 8 && fb->blue_shift == 0;
    uint32_t bytes = fb->bpp / 8;
    int lx = r.x - s->area.x, ly = r.y - s->area.y, scale = s->scale;
    for (int y = 0; y < r.h; y++) {
        const uint32_t *src = s->back.px + (r.y + y) * s->back.stride + r.x;
        for (int sy = 0; sy < scale; sy++) {
            uint8_t *line = fb->base + (size_t)((ly + y) * scale + sy) * fb->pitch + (size_t)lx * scale * bytes;
            if (native && scale == 1) {
                memcpy(line, src, (size_t)r.w * 4);
                continue;
            }
            for (int x = 0; x < r.w; x++) {
                uint32_t p = native ? src[x] : pack(fb, src[x]);
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

static void add_dirty(struct screen *s, struct rect r)
{
    for (int i = 0; i < s->dirty_count; i++) {
        if (rect_inside(r, s->dirty[i]))
            return;
        struct rect u = rect_union(s->dirty[i], r);
        if (u.w * u.h <= s->dirty[i].w * s->dirty[i].h + r.w * r.h + 4096) {
            s->dirty[i] = u;
            return;
        }
    }
    if (s->dirty_count == MAX_DIRTY) {
        for (int i = 1; i < s->dirty_count; i++)
            s->dirty[0] = rect_union(s->dirty[0], s->dirty[i]);
        s->dirty_count = 1;
        s->dirty[0] = rect_union(s->dirty[0], r);
        return;
    }
    s->dirty[s->dirty_count++] = r;
}

void wm_damage_z(struct rect r, int z)
{
    if (rect_empty(r))
        return;
    for (int i = 0; i < screen_count; i++) {
        struct rect a = rect_intersect(r, screens[i].area);
        if (!rect_empty(a))
            add_dirty(&screens[i], a);
    }
    glass_damage(r, z);
}

void wm_damage(struct rect r)
{
    wm_damage_z(r, 0);
}

void wm_damage_all(void)
{
    for (int i = 0; i < screen_count; i++)
        wm_damage(screens[i].area);
}

struct rect wm_compose_screen(void)
{
    return compose_area;
}

int wm_screen_count(void)
{
    return screen_count;
}

struct rect wm_screen_rect(int index)
{
    if (index < 0 || index >= screen_count)
        return (struct rect){0, 0, 0, 0};
    return screens[index].area;
}

int wm_screen_at(int x, int y)
{
    for (int i = 0; i < screen_count; i++)
        if (rect_contains(screens[i].area, x, y))
            return i;
    return 0;
}

void wm_screen_size(int *w, int *h)
{
    struct rect a = wm_screen_rect(0);
    if (w)
        *w = a.w;
    if (h)
        *h = a.h;
}

struct rect wm_work_area(int index)
{
    struct rect a = wm_screen_rect(index);
    a.y += MENUBAR_HEIGHT;
    a.h -= MENUBAR_HEIGHT;
    if (index == 0)
        a.h -= dock_reserved();
    return a;
}

static bool visible(const struct window *w)
{
    return !(w->flags & (WIN_HIDDEN | WIN_MINIMIZED));
}

static bool floating(const struct window *w)
{
    return w->flags & (WIN_POPUP | WIN_OVERLAY);
}

static bool framed(const struct window *w)
{
    return !(w->flags & (WIN_POPUP | WIN_DIALOG | WIN_OVERLAY));
}

struct rect wm_client_rect(const struct window *w)
{
    if (!framed(w) || (w->flags & WIN_FULLSIZE))
        return w->frame;
    return (struct rect){w->frame.x, w->frame.y + TITLE_HEIGHT, w->frame.w, w->frame.h - TITLE_HEIGHT};
}

static struct rect outer_rect(const struct window *w)
{
    if (w->flags & WIN_NOSHADOW)
        return w->frame;
    int size = floating(w) ? 18 : SHADOW, dy = floating(w) ? 6 : SHADOW_DY;
    struct rect s = {w->frame.x - size, w->frame.y + dy - size, w->frame.w + 2 * size, w->frame.h + 2 * size};
    return rect_union(s, w->frame);
}

static void damage_window(struct window *w)
{
    wm_damage_z(outer_rect(w), w->z);
}

bool wm_window_alive(const struct window *w)
{
    for (struct window *it = bottom; it; it = it->above)
        if (it == w)
            return true;
    return false;
}

static void restack(void)
{
    int normal = 0, high = 0;
    for (struct window *w = bottom; w; w = w->above) {
        w->z = floating(w) ? 2000 + 2 * high++ : 10 + 2 * normal++;
        if (w->glass) {
            w->glass->z = w->z;
            glass_set_frame(w->glass, w->frame);
        }
    }
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
    for (struct window *it = bottom; it;) {
        struct window *next = it->above;
        if (it != w && it->owner == w) {
            unlink(it);
            link_top(it);
        }
        it = next;
    }
    restack();
    wm_damage(outer_rect(w));
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

static void damage_title(struct window *w)
{
    int h = w->flags & WIN_FULLSIZE ? (w->toolbar_h ? w->toolbar_h : TITLE_HEIGHT) : TITLE_HEIGHT;
    wm_damage_z((struct rect){w->frame.x, w->frame.y, w->frame.w, h}, w->z);
}

void wm_focus(struct window *w)
{
    if (w && !floating(w))
        raise(w);
    if (focus == w)
        return;
    struct window *old = focus;
    focus = w;
    if (old && wm_window_alive(old)) {
        send(old, &(struct gui_event){.type = EV_BLUR});
        damage_window(old);
        old->dirty = true;
    }
    if (w) {
        send(w, &(struct gui_event){.type = EV_FOCUS});
        damage_window(w);
        w->dirty = true;
    }
    chrome_changed();
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
    gfx_clear(&w->surface, (struct rect){0, 0, fresh.w, fresh.h}, style.window);
    w->dirty = true;
    return true;
}

static int64_t overlap_cost(struct rect r, const struct window *self)
{
    int64_t cost = 0;
    for (struct window *it = bottom; it; it = it->above) {
        if (it == self || !visible(it) || floating(it))
            continue;
        struct rect o = rect_intersect(r, it->frame);
        if (!rect_empty(o))
            cost += (int64_t)o.w * o.h;
    }
    return cost;
}

static void place(struct window *w)
{
    int screen = focus ? wm_screen_at(focus->frame.x + focus->frame.w / 2, focus->frame.y + 10) : 0;
    struct rect work = wm_work_area(screen);
    if (w->flags & WIN_DIALOG) {
        struct rect ref = w->owner && wm_window_alive(w->owner) ? w->owner->frame : work;
        w->frame.x = ref.x + (ref.w - w->frame.w) / 2;
        w->frame.y = MAX(work.y + 8, ref.y + MIN(60, (ref.h - w->frame.h) / 3));
        return;
    }
    int min_x = work.x, max_x = MAX(min_x, work.x + work.w - w->frame.w);
    int min_y = work.y + 4, max_y = MAX(min_y, work.y + work.h - w->frame.h);
    int cx = CLAMP(work.x + (work.w - w->frame.w) / 2, min_x, max_x), cy = CLAMP(work.y + (work.h - w->frame.h) / 3, min_y, max_y);
    int64_t best = -1, best_dist = 0;
    int bx = cx, by = cy;
    for (int y = min_y; y <= max_y; y += 16)
        for (int x = min_x; x <= max_x; x += 16) {
            int64_t cost = overlap_cost((struct rect){x, y, w->frame.w, w->frame.h}, w);
            int64_t dist = (int64_t)(x - cx) * (x - cx) + (int64_t)(y - cy) * (y - cy);
            if (best < 0 || cost < best || (cost == best && dist < best_dist)) {
                best = cost;
                best_dist = dist;
                bx = x;
                by = y;
            }
        }
    w->frame.x = bx;
    w->frame.y = by;
}

void wm_set_app(const struct gui_app *app)
{
    launching_app = app;
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
    w->min_w = 220;
    w->min_h = 120;
    w->radius = flags & WIN_DIALOG ? 16 : RADIUS;
    w->app = launching_app;
    w->toolbar_h = flags & WIN_FULLSIZE ? TITLE_HEIGHT : 0;
    struct rect work = wm_work_area(0);
    int title_h = framed(w) && !(flags & WIN_FULLSIZE) ? TITLE_HEIGHT : 0;
    w->frame.w = MIN(cw, work.w);
    w->frame.h = MIN(ch + title_h, work.h - 8);
    if (flags & WIN_DIALOG)
        w->owner = focus;
    place(w);
    if (!alloc_surface(w)) {
        kfree(w);
        return NULL;
    }
    if (flags & WIN_GLASS) {
        w->glass = kzalloc(sizeof(*w->glass));
        if (w->glass) {
            w->glass->frame = w->frame;
            glass_register(w->glass);
        }
    }
    link_top(w);
    restack();
    if (!(flags & WIN_HIDDEN))
        wm_focus(w);
    damage_window(w);
    chrome_changed();
    return w;
}

struct window *wm_popup(int x, int y, int pw, int ph, const struct window_ops *ops, void *data, struct window *owner)
{
    struct window *w = kzalloc(sizeof(*w));
    if (!w)
        return NULL;
    w->ops = ops;
    w->data = data;
    w->flags = WIN_POPUP | WIN_GLASS;
    w->owner = owner;
    w->radius = 10;
    struct rect s = wm_screen_rect(wm_screen_at(x, y));
    w->frame = (struct rect){CLAMP(x, s.x + 4, MAX(s.x + 4, s.x + s.w - pw - 4)),
                             CLAMP(y, s.y + 2, MAX(s.y + 2, s.y + s.h - ph - 4)), pw, ph};
    if (!alloc_surface(w)) {
        kfree(w);
        return NULL;
    }
    w->glass = kzalloc(sizeof(*w->glass));
    if (w->glass) {
        w->glass->frame = w->frame;
        glass_register(w->glass);
    }
    link_top(w);
    restack();
    damage_window(w);
    return w;
}

static void free_glass(struct window *w)
{
    if (!w->glass)
        return;
    glass_unregister(w->glass);
    kfree(w->glass);
    w->glass = NULL;
}

static struct window *next_focus(struct window *except)
{
    for (struct window *it = top; it; it = it->below)
        if (it != except && visible(it) && !floating(it))
            return it;
    return NULL;
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
    wm_damage(outer_rect(w));
    unlink(w);
    restack();
    if (drag_win == w) {
        drag = DRAG_NONE;
        drag_win = NULL;
    }
    if (hover_win == w)
        hover_win = NULL;
    if (traffic_win == w)
        traffic_win = NULL;
    if (last_click_win == w)
        last_click_win = NULL;
    if (w->ops && w->ops->destroy)
        w->ops->destroy(w);
    canvas_free(&w->surface);
    canvas_free(&w->thumb);
    free_glass(w);
    if (focus == w) {
        focus = NULL;
        struct window *next = w->owner && wm_window_alive(w->owner) ? w->owner : next_focus(w);
        if (next)
            wm_focus(next);
    }
    kfree(w);
    chrome_changed();
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

static void lose_focus(struct window *w)
{
    if (focus != w)
        return;
    focus = NULL;
    struct window *next = next_focus(w);
    if (next)
        wm_focus(next);
}

void wm_hide(struct window *w)
{
    w->flags |= WIN_HIDDEN;
    wm_damage(outer_rect(w));
    lose_focus(w);
    chrome_changed();
}

void wm_show(struct window *w)
{
    w->flags &= ~(WIN_HIDDEN | WIN_MINIMIZED);
    canvas_free(&w->thumb);
    wm_focus(w);
    wm_damage(outer_rect(w));
    chrome_changed();
}

void wm_restore(struct window *w)
{
    wm_show(w);
}

static void make_thumb(struct window *w)
{
    canvas_free(&w->thumb);
    int tw = THUMB_MAX, th = THUMB_MAX;
    if (w->frame.w > w->frame.h)
        th = MAX(8, THUMB_MAX * w->frame.h / MAX(w->frame.w, 1));
    else
        tw = MAX(8, THUMB_MAX * w->frame.w / MAX(w->frame.h, 1));
    if (!canvas_alloc(&w->thumb, tw, th))
        return;
    gfx_clear(&w->thumb, (struct rect){0, 0, tw, th}, 0);
    struct rect cr = wm_client_rect(w);
    int title = cr.y - w->frame.y;
    int title_h = th * title / MAX(w->frame.h, 1);
    gfx_round_clear(&w->thumb, (struct rect){0, 0, tw, th}, 6, style.titlebar);
    gfx_scale(&w->thumb, (struct rect){0, title_h, tw, th - title_h}, &w->surface, 255);
}

void wm_minimize(struct window *w)
{
    make_thumb(w);
    w->flags |= WIN_MINIMIZED;
    wm_damage(outer_rect(w));
    lose_focus(w);
    chrome_changed();
}

void wm_invalidate(struct window *w)
{
    if (w && wm_window_alive(w))
        w->dirty = true;
}

void wm_set_title(struct window *w, const char *title)
{
    str_copy(w->title, title, sizeof(w->title));
    damage_title(w);
    chrome_changed();
}

void wm_set_menus(struct window *w, const struct app_menu *menus, int count)
{
    w->menus = menus;
    w->menu_count = count;
    chrome_changed();
}

void wm_set_timer(struct window *w, uint32_t ms)
{
    w->timer_ms = ms;
    w->next_timer = hal_ticks_ms() + ms;
}

static void set_frame(struct window *w, struct rect frame)
{
    wm_damage_z(outer_rect(w), w->z);
    bool resized = frame.w != w->frame.w || frame.h != w->frame.h;
    w->frame = frame;
    if (w->glass)
        glass_set_frame(w->glass, frame);
    if (resized) {
        alloc_surface(w);
        send(w, &(struct gui_event){.type = EV_RESIZE});
    }
    damage_window(w);
}

void wm_move(struct window *w, int x, int y)
{
    set_frame(w, (struct rect){x, y, w->frame.w, w->frame.h});
}

void wm_resize_client(struct window *w, int cw, int ch)
{
    struct rect f = w->frame;
    struct rect cr = wm_client_rect(w);
    f.w = cw + (f.w - cr.w);
    f.h = ch + (f.h - cr.h);
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
        struct rect work = wm_work_area(wm_screen_at(w->frame.x + w->frame.w / 2, w->frame.y + 8));
        set_frame(w, (struct rect){work.x + 4, work.y + 4, work.w - 8, work.h - 8});
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

struct window *wm_app_window(const struct gui_app *app)
{
    for (struct window *w = top; w; w = w->below)
        if (w->app == app && framed(w) && !(w->flags & WIN_HIDDEN))
            return w;
    return NULL;
}

int wm_app_window_count(const struct gui_app *app)
{
    int n = 0;
    for (struct window *w = bottom; w; w = w->above)
        if (w->app == app && framed(w) && !(w->flags & WIN_HIDDEN))
            n++;
    return n;
}

void wm_focus_app(const struct gui_app *app)
{
    struct window *last = NULL, *minimized = NULL;
    for (struct window *w = bottom; w;) {
        struct window *next = w->above;
        if (w->app == app && framed(w) && !(w->flags & WIN_HIDDEN)) {
            if (w->flags & WIN_MINIMIZED)
                minimized = w;
            else {
                raise(w);
                last = w;
            }
        }
        w = next;
    }
    if (last)
        wm_focus(last);
    else if (minimized)
        wm_show(minimized);
}

void wm_bring_all(void)
{
    for (struct window *w = bottom; w; w = w->above)
        if (w->flags & WIN_MINIMIZED) {
            w->flags &= ~WIN_MINIMIZED;
            canvas_free(&w->thumb);
        }
    wm_damage_all();
    chrome_changed();
}

static int title_band(const struct window *w)
{
    if (!framed(w))
        return 0;
    return w->flags & WIN_FULLSIZE ? (w->toolbar_h ? w->toolbar_h : TITLE_HEIGHT) : TITLE_HEIGHT;
}

static int traffic_cy(const struct window *w)
{
    return w->frame.y + title_band(w) / 2;
}

static int traffic_cx(const struct window *w, int i)
{
    return w->frame.x + 20 + i * 20;
}

static bool light_enabled(const struct window *w, int i)
{
    return i == 0 || i == 1 || (w->flags & WIN_RESIZABLE);
}

static void draw_lights(struct canvas *c, struct window *w)
{
    static const uint32_t colors[3] = {RGB(255, 95, 87), RGB(254, 188, 46), RGB(40, 200, 64)};
    static const uint32_t glyphs[3] = {RGBA(77, 0, 0, 220), RGBA(152, 87, 18, 230), RGBA(0, 100, 0, 220)};
    bool active = w == focus;
    bool hover = traffic_win == w;
    int cy = traffic_cy(w);
    for (int i = 0; i < 3; i++) {
        int cx = traffic_cx(w, i);
        bool on = light_enabled(w, i);
        uint32_t color = (active || hover) && on ? colors[i] : style.dark ? RGB(76, 76, 80) : RGB(208, 208, 210);
        if (hover && pressed_light == i)
            color = color_shade(color, -50);
        gfx_circle(c, cx, cy, 6, color);
        gfx_ring(c, cx, cy, 6, ALPHA(0, style.dark ? 70 : 34));
        if (!hover || !on)
            continue;
        uint32_t g = glyphs[i];
        if (i == 0) {
            gfx_thick_line(c, cx - 3, cy - 3, cx + 3, cy + 3, 2, g);
            gfx_thick_line(c, cx - 3, cy + 3, cx + 3, cy - 3, 2, g);
        } else if (i == 1) {
            gfx_fill(c, (struct rect){cx - 4, cy - 1, 8, 2}, g);
        } else {
            for (int k = 0; k < 5; k++) {
                gfx_fill(c, (struct rect){cx - 4, cy - 4 + k, 5 - k, 1}, g);
                gfx_fill(c, (struct rect){cx + k, cy + 4 - k, 4 - k + 1, 1}, g);
            }
        }
    }
}

static void draw_window(struct canvas *c, struct window *w)
{
    struct rect f = w->frame;
    bool active = w == focus || floating(w);
    if (!(w->flags & WIN_NOSHADOW)) {
        if (floating(w))
            gfx_drop_shadow(c, f, w->radius, 18, style.dark ? 140 : 70, 6);
        else
            gfx_drop_shadow(c, f, w->radius, active ? SHADOW : 24, style.dark ? (active ? 200 : 140) : (active ? 105 : 60),
                            active ? SHADOW_DY : 8);
    }
    struct rect cr = wm_client_rect(w);
    bool titled = framed(w) && !(w->flags & WIN_FULLSIZE);
    if (w->glass && style.glass != GLASS_SOLID) {
        struct glass_look look;
        glass_look_for(&look, w->flags & WIN_OVERLAY ? GLASS_OVERLAY : w->flags & WIN_POPUP ? GLASS_MENU : GLASS_WINDOW,
                       w->radius);
        glass_draw(c, w->glass, &look);
    } else if (w->glass) {
        gfx_round(c, f, w->radius, style.sidebar | 0xff000000u);
    }
    struct canvas src = w->surface, body = *c;
    if (titled) {
        struct canvas t = *c;
        t.clip = rect_intersect(c->clip, (struct rect){f.x, f.y, f.w, TITLE_HEIGHT});
        gfx_round(&t, f, w->radius, active ? style.titlebar : style.titlebar_inactive);
        gfx_fill(c, (struct rect){f.x, f.y + TITLE_HEIGHT - 1, f.w, 1}, style.separator);
        src.px -= TITLE_HEIGHT * src.stride;
        src.h += TITLE_HEIGHT;
        body.clip = rect_intersect(c->clip, cr);
    }
    gfx_blit_round(&body, f, &src, w->radius, w->glass != NULL);
    if (framed(w)) {
        if (!(w->flags & WIN_NOTITLE)) {
            struct rect tr = {f.x + 76, f.y, f.w - 152, title_band(w)};
            gfx_text_box(c, tr, &font_bold, w->title, active ? ALPHA(style.text, 220) : style.tertiary, ALIGN_CENTER);
        }
        draw_lights(c, w);
    }
    if (!(w->flags & WIN_OVERLAY)) {
        gfx_round_frame(c, f, w->radius, style.dark ? ALPHA(0, 230) : ALPHA(0, 44));
        if (style.dark)
            gfx_round_frame_gradient(c, rect_inset(f, 1), w->radius - 1, ALPHA(0xffffffffu, 40), ALPHA(0xffffffffu, 12));
    }
}

static void draw_cursor(struct canvas *c)
{
    if (!cursor_visible || !cursor_px[cursor_kind])
        return;
    gfx_image(c, mouse_x - cursor_hot[cursor_kind][0], mouse_y - cursor_hot[cursor_kind][1], cursor_px[cursor_kind],
              CURSOR_SIZE, CURSOR_SIZE);
}

static struct rect cursor_rect(void)
{
    return (struct rect){mouse_x - cursor_hot[cursor_kind][0], mouse_y - cursor_hot[cursor_kind][1], CURSOR_SIZE,
                         CURSOR_SIZE};
}

static void compose(int index, struct rect area)
{
    struct screen *s = &screens[index];
    compose_area = s->area;
    struct canvas *c = &s->back;
    c->clip = rect_intersect(area, s->area);
    desktop_paint(c, index, c->clip);
    for (struct window *w = bottom; w; w = w->above)
        if (visible(w) && !floating(w) && !rect_empty(rect_intersect(outer_rect(w), c->clip)))
            draw_window(c, w);
    menubar_paint(c, index);
    dock_paint(c, index);
    for (struct window *w = bottom; w; w = w->above)
        if (visible(w) && floating(w) && !rect_empty(rect_intersect(outer_rect(w), c->clip)))
            draw_window(c, w);
    draw_cursor(c);
}

void wm_render(void)
{
    if (rendering || !screen_count)
        return;
    rendering = true;
    for (struct window *w = bottom; w; w = w->above)
        if (w->dirty && visible(w) && w->ops && w->ops->paint) {
            w->dirty = false;
            struct canvas c = w->surface;
            c.clip = (struct rect){0, 0, c.w, c.h};
            w->ops->paint(w, &c);
            wm_damage_z(wm_client_rect(w), w->z);
        }
    for (int i = 0; i < screen_count; i++) {
        struct screen *s = &screens[i];
        struct rect list[MAX_DIRTY];
        int count = s->dirty_count;
        memcpy(list, s->dirty, sizeof(struct rect) * (size_t)count);
        s->dirty_count = 0;
        for (int k = 0; k < count; k++)
            compose(i, list[k]);
        for (int k = 0; k < count; k++)
            flush(s, list[k]);
    }
    rendering = false;
}

enum hit_kind {
    HIT_DESKTOP,
    HIT_WINDOW,
    HIT_CHROME,
};

static enum hit_kind hit_test(int x, int y, struct window **out)
{
    *out = NULL;
    for (struct window *w = top; w; w = w->below)
        if (visible(w) && floating(w) && rect_contains(w->frame, x, y)) {
            *out = w;
            return HIT_WINDOW;
        }
    if (chrome_hit(x, y))
        return HIT_CHROME;
    for (struct window *w = top; w; w = w->below)
        if (visible(w) && !floating(w)) {
            struct rect grab = w->frame;
            if ((w->flags & WIN_RESIZABLE) && framed(w))
                grab = (struct rect){grab.x - 3, grab.y - 3, grab.w + 6, grab.h + 6};
            if (rect_contains(grab, x, y)) {
                *out = w;
                return HIT_WINDOW;
            }
        }
    return HIT_DESKTOP;
}

static int edges_at(struct window *w, int x, int y)
{
    if (!(w->flags & WIN_RESIZABLE) || !framed(w))
        return 0;
    struct rect f = w->frame;
    int edges = 0;
    if (x >= f.x + f.w - EDGE)
        edges |= 1;
    if (y >= f.y + f.h - EDGE)
        edges |= 2;
    if (x < f.x + EDGE)
        edges |= 4;
    if (y < f.y + 4)
        edges |= 8;
    return edges;
}

static int light_at(struct window *w, int x, int y)
{
    if (!framed(w))
        return -1;
    int cy = traffic_cy(w);
    for (int i = 0; i < 3; i++) {
        int dx = x - traffic_cx(w, i), dy = y - cy;
        if (dx * dx + dy * dy <= 8 * 8)
            return light_enabled(w, i) ? i : -1;
    }
    return -1;
}

static bool in_traffic_zone(struct window *w, int x, int y)
{
    if (!framed(w))
        return false;
    int cy = traffic_cy(w);
    return x >= w->frame.x + 8 && x < w->frame.x + 72 && y >= cy - 9 && y <= cy + 9;
}

static bool client_event(struct window *w, enum gui_event_type type, unsigned button, int wheel, int clicks)
{
    struct rect cr = wm_client_rect(w);
    struct gui_event e = {.type = type, .x = mouse_x - cr.x, .y = mouse_y - cr.y, .buttons = mouse_buttons,
                          .button = button, .wheel = wheel, .clicks = clicks};
    send(w, &e);
    return e.handled;
}

static void set_cursor(int kind)
{
    if (kind == cursor_kind)
        return;
    wm_damage_z(cursor_rect(), Z_CURSOR);
    cursor_kind = kind;
    wm_damage_z(cursor_rect(), Z_CURSOR);
}

static void set_traffic(struct window *w)
{
    if (traffic_win == w)
        return;
    if (traffic_win && wm_window_alive(traffic_win))
        damage_title(traffic_win);
    traffic_win = w;
    if (w)
        damage_title(w);
}

static void set_hover(struct window *w)
{
    if (hover_win == w)
        return;
    if (hover_win && wm_window_alive(hover_win))
        send(hover_win, &(struct gui_event){.type = EV_LEAVE});
    hover_win = w;
}

static void close_popups(struct window *except)
{
    for (struct window *w = bottom; w;) {
        struct window *next = w->above;
        if ((w->flags & WIN_POPUP) && !(w->flags & WIN_PASSIVE) && w != except && !(except && except->owner == w))
            wm_destroy(w);
        w = next;
    }
}

static void mouse_down(unsigned button)
{
    struct window *w;
    enum hit_kind hit = hit_test(mouse_x, mouse_y, &w);
    if (hit == HIT_CHROME) {
        drag = DRAG_CHROME;
        chrome_mouse(mouse_x, mouse_y, button, true, click_count);
        return;
    }
    if (!w || !(w->flags & WIN_POPUP))
        close_popups(w);
    if (hit == HIT_DESKTOP || !w || !wm_window_alive(w)) {
        wm_focus(NULL);
        desktop_mouse(mouse_x, mouse_y, button, true, click_count);
        return;
    }
    if (!floating(w))
        wm_focus(w);
    int light = button == BUTTON_LEFT ? light_at(w, mouse_x, mouse_y) : -1;
    if (light >= 0) {
        drag = DRAG_TRAFFIC;
        drag_win = w;
        pressed_light = light;
        set_traffic(w);
        damage_title(w);
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
    bool in_title = framed(w) && mouse_y < w->frame.y + title_band(w);
    if (in_title && !(w->flags & WIN_FULLSIZE)) {
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
    bool handled = client_event(w, EV_MOUSE_DOWN, button, 0, click_count);
    if (!handled && in_title && button == BUTTON_LEFT && wm_window_alive(w)) {
        if (click_count >= 2) {
            drag = DRAG_NONE;
            drag_win = NULL;
            wm_toggle_maximize(w);
            return;
        }
        drag = DRAG_MOVE;
        drag_x = mouse_x - w->frame.x;
        drag_y = mouse_y - w->frame.y;
    }
}

static void mouse_up(unsigned button)
{
    struct window *w = drag_win;
    enum drag_mode mode = drag;
    drag = DRAG_NONE;
    drag_win = NULL;
    if (mode == DRAG_CHROME) {
        chrome_mouse(mouse_x, mouse_y, button, false, click_count);
        return;
    }
    if (!w || !wm_window_alive(w)) {
        struct window *at;
        if (hit_test(mouse_x, mouse_y, &at) == HIT_DESKTOP)
            desktop_mouse(mouse_x, mouse_y, button, false, click_count);
        return;
    }
    if (mode == DRAG_TRAFFIC) {
        int b = pressed_light;
        pressed_light = -1;
        damage_title(w);
        if (light_at(w, mouse_x, mouse_y) != b)
            return;
        if (b == 0)
            wm_close(w);
        else if (b == 1)
            wm_minimize(w);
        else
            wm_toggle_maximize(w);
        return;
    }
    if (mode == DRAG_CLIENT)
        client_event(w, EV_MOUSE_UP, button, 0, click_count);
}

static int cursor_for_edges(int edges)
{
    if ((edges & 1 && edges & 2) || (edges & 4 && edges & 8))
        return CUR_NWSE;
    if ((edges & 1 && edges & 8) || (edges & 4 && edges & 2))
        return CUR_NESW;
    if (edges & 5)
        return CUR_EW;
    if (edges & 10)
        return CUR_NS;
    return CUR_ARROW;
}

static void mouse_moved(void)
{
    if (drag == DRAG_MOVE && drag_win) {
        if (drag_win->flags & WIN_MAXIMIZED) {
            int rel = drag_x * drag_win->restore.w / MAX(drag_win->frame.w, 1);
            drag_win->flags &= ~WIN_MAXIMIZED;
            set_frame(drag_win, (struct rect){mouse_x - rel, drag_win->frame.y, drag_win->restore.w, drag_win->restore.h});
            drag_x = rel;
        }
        struct rect s = wm_screen_rect(wm_screen_at(mouse_x, mouse_y));
        int y = MAX(mouse_y - drag_y, s.y + MENUBAR_HEIGHT);
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
        if (drag_edges & 8) {
            int h = MAX(drag_win->min_h, drag_frame.h - dy);
            f.y = MAX(drag_frame.y + drag_frame.h - h, MENUBAR_HEIGHT);
            f.h = drag_frame.y + drag_frame.h - f.y;
        }
        set_frame(drag_win, f);
        return;
    }
    if (drag == DRAG_CLIENT && drag_win) {
        client_event(drag_win, EV_MOUSE_MOVE, 0, 0, 0);
        return;
    }
    if (drag == DRAG_CHROME) {
        chrome_hover(mouse_x, mouse_y);
        return;
    }
    struct window *w;
    enum hit_kind hit = hit_test(mouse_x, mouse_y, &w);
    chrome_hover(mouse_x, hit == HIT_CHROME ? mouse_y : -1);
    if (hit != HIT_WINDOW || !w) {
        set_hover(NULL);
        set_traffic(NULL);
        set_cursor(CUR_ARROW);
        return;
    }
    if (drag != DRAG_TRAFFIC)
        set_traffic(in_traffic_zone(w, mouse_x, mouse_y) ? w : NULL);
    set_cursor(cursor_for_edges(edges_at(w, mouse_x, mouse_y)));
    set_hover(w);
    if (rect_contains(wm_client_rect(w), mouse_x, mouse_y))
        client_event(w, EV_MOUSE_MOVE, 0, 0, 0);
}

static void clamp_mouse(int x, int y)
{
    for (int i = 0; i < screen_count; i++)
        if (rect_contains(screens[i].area, x, y)) {
            mouse_x = x;
            mouse_y = y;
            return;
        }
    int best = -1, best_d = 0, bx = mouse_x, by = mouse_y;
    for (int i = 0; i < screen_count; i++) {
        struct rect a = screens[i].area;
        int cx = CLAMP(x, a.x, a.x + a.w - 1), cy = CLAMP(y, a.y, a.y + a.h - 1);
        int d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
            bx = cx;
            by = cy;
        }
    }
    mouse_x = bx;
    mouse_y = by;
}

void wm_pointer(const struct pointer_event *ev)
{
    int old_x = mouse_x, old_y = mouse_y;
    struct rect old = cursor_rect();
    accel_x += ev->dx * mouse_speed;
    accel_y += ev->dy * mouse_speed;
    int dx = accel_x / 5, dy = accel_y / 5;
    accel_x -= dx * 5;
    accel_y -= dy * 5;
    clamp_mouse(mouse_x + dx, mouse_y + dy);
    if (old_x != mouse_x || old_y != mouse_y) {
        wm_damage_z(old, Z_CURSOR);
        wm_damage_z(cursor_rect(), Z_CURSOR);
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
            struct window *at;
            hit_test(mouse_x, mouse_y, &at);
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
        struct window *w;
        if (hit_test(mouse_x, mouse_y, &w) == HIT_WINDOW && w)
            client_event(w, EV_WHEEL, 0, natural_scroll ? -ev->wheel : ev->wheel, 0);
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
        if (w != focus && framed(w) && !(w->flags & WIN_HIDDEN)) {
            if (w->flags & WIN_MINIMIZED)
                wm_show(w);
            else
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
        if (floating(w) && visible(w) && !(w->flags & WIN_PASSIVE))
            return w;
    return NULL;
}

void wm_set_cursor_visible(bool on)
{
    cursor_visible = on;
    wm_damage_z(cursor_rect(), Z_CURSOR);
}

static bool inside_poly(const int *pts, int n, int x, int y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        int xi = pts[i * 2], yi = pts[i * 2 + 1], xj = pts[j * 2], yj = pts[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < xj + (int)((int64_t)(xi - xj) * (y - yj) / (yi - yj)))
            in = !in;
    }
    return in;
}

static uint32_t *render_cursor(const int *pts, int n)
{
    uint32_t *px = kzalloc(CURSOR_SIZE * CURSOR_SIZE * 4);
    uint8_t *fill = kzalloc(CURSOR_SIZE * CURSOR_SIZE), *outline = kzalloc(CURSOR_SIZE * CURSOR_SIZE);
    if (!px || !fill || !outline) {
        kfree(fill);
        kfree(outline);
        return px;
    }
    static const int ring[8][2] = {{24, 0}, {-24, 0}, {0, 24}, {0, -24}, {17, 17}, {-17, 17}, {17, -17}, {-17, -17}};
    for (int y = 0; y < CURSOR_SIZE; y++)
        for (int x = 0; x < CURSOR_SIZE; x++) {
            int f = 0, o = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px16 = x * 16 + sx * 4 + 2, py16 = y * 16 + sy * 4 + 2;
                    bool in = inside_poly(pts, n, px16, py16);
                    f += in;
                    bool near = in;
                    for (int k = 0; k < 8 && !near; k++)
                        near = inside_poly(pts, n, px16 + ring[k][0], py16 + ring[k][1]);
                    o += near;
                }
            fill[y * CURSOR_SIZE + x] = (uint8_t)(f * 255 / 16);
            outline[y * CURSOR_SIZE + x] = (uint8_t)(o * 255 / 16);
        }
    for (int y = 0; y < CURSOR_SIZE; y++)
        for (int x = 0; x < CURSOR_SIZE; x++) {
            unsigned shadow = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int sx = x - 1 + dx, sy = y - 2 + dy;
                    if (sx >= 0 && sy >= 0 && sx < CURSOR_SIZE && sy < CURSOR_SIZE)
                        shadow += outline[sy * CURSOR_SIZE + sx];
                }
            shadow = shadow / 9 * 90 / 255;
            uint32_t p = shadow ? ALPHA(0, shadow) : 0;
            unsigned o = outline[y * CURSOR_SIZE + x], f = fill[y * CURSOR_SIZE + x];
            if (o) {
                unsigned a = o + (p >> 24) * (255 - o) / 255;
                p = ALPHA(0xffffffu, a);
            }
            if (f)
                p = (color_mix(p | 0xff000000u, RGB(0, 0, 0), (int)f) & 0x00ffffffu) | (uint32_t)MAX(f, p >> 24) << 24;
            px[y * CURSOR_SIZE + x] = p;
        }
    kfree(fill);
    kfree(outline);
    return px;
}

static void make_cursors(void)
{
    static const int arrow[] = {2, 2, 2, 18, 6, 14, 9, 21, 11, 20, 8, 13, 14, 13};
    int pts[16 * 2];
    for (int i = 0; i < 7 * 2; i++)
        pts[i] = arrow[i] * 16 + 24;
    cursor_px[CUR_ARROW] = render_cursor(pts, 7);
    cursor_hot[CUR_ARROW][0] = 3;
    cursor_hot[CUR_ARROW][1] = 3;
    static const int dbl[] = {-9, 0, -4, -5, -4, -2, 4, -2, 4, -5, 9, 0, 4, 5, 4, 2, -4, 2, -4, 5};
    static const int rot[4][4] = {{256, 0, 0, 256}, {0, -256, 256, 0}, {181, -181, 181, 181}, {181, 181, -181, 181}};
    static const int kinds[4] = {CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW};
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 10; i++) {
            int x = dbl[i * 2] * 16, y = dbl[i * 2 + 1] * 16;
            pts[i * 2] = (x * rot[k][0] + y * rot[k][1]) / 256 + 14 * 16;
            pts[i * 2 + 1] = (x * rot[k][2] + y * rot[k][3]) / 256 + 14 * 16;
        }
        cursor_px[kinds[k]] = render_cursor(pts, 10);
        cursor_hot[kinds[k]][0] = 14;
        cursor_hot[kinds[k]][1] = 14;
    }
}

static bool setup_screens(const struct framebuffer *fbs, const struct rect *origins, int count)
{
    int made = 0;
    for (int i = 0; i < count && i < MAX_SCREENS; i++) {
        struct screen *s = &screens[made];
        s->fb = fbs[i];
        s->scale = fbs[i].width >= 2400 ? 2 : 1;
        int w = (int)fbs[i].width / s->scale, h = (int)fbs[i].height / s->scale;
        s->mem = kmalloc((size_t)w * (size_t)h * 4);
        if (!s->mem)
            break;
        int x = origins ? origins[i].x : 0, y = origins ? origins[i].y : 0;
        s->area = (struct rect){x, y, w, h};
        s->back.stride = w;
        s->back.px = s->mem - (x + y * w);
        s->back.w = x + w;
        s->back.h = y + h;
        s->dirty_count = 0;
        made++;
    }
    screen_count = made;
    return made > 0;
}

static void free_screens(void)
{
    for (int i = 0; i < screen_count; i++) {
        kfree(screens[i].mem);
        screens[i].mem = NULL;
    }
    screen_count = 0;
}

bool wm_start(const struct framebuffer *fbs, const struct rect *origins, int count)
{
    if (!setup_screens(fbs, origins, count))
        return false;
    if (!cursor_px[CUR_ARROW])
        make_cursors();
    mouse_x = screens[0].area.x + screens[0].area.w / 2;
    mouse_y = screens[0].area.y + screens[0].area.h / 2;
    mouse_buttons = 0;
    cursor_kind = CUR_ARROW;
    wm_damage_all();
    return true;
}

static void keep_visible(struct window *w, void *ctx)
{
    UNUSED(ctx);
    if (w->flags & WIN_OVERLAY) {
        struct rect s = wm_screen_rect(0);
        set_frame(w, s);
        return;
    }
    int cx = w->frame.x + w->frame.w / 2, ty = w->frame.y + 10;
    bool visible_somewhere = false;
    for (int i = 0; i < screen_count; i++)
        visible_somewhere |= rect_contains(screens[i].area, cx, ty);
    if (w->flags & WIN_MAXIMIZED) {
        struct rect work = wm_work_area(wm_screen_at(cx, ty));
        set_frame(w, (struct rect){work.x + 4, work.y + 4, work.w - 8, work.h - 8});
        return;
    }
    struct rect work = wm_work_area(visible_somewhere ? wm_screen_at(cx, ty) : 0);
    struct rect f = w->frame;
    f.w = MIN(f.w, work.w);
    f.h = MIN(f.h, work.h);
    f.x = CLAMP(f.x, work.x, work.x + work.w - f.w);
    f.y = CLAMP(f.y, work.y, work.y + work.h - f.h);
    if (f.x != w->frame.x || f.y != w->frame.y || f.w != w->frame.w || f.h != w->frame.h)
        set_frame(w, f);
}

bool wm_reconfigure(const struct framebuffer *fbs, const struct rect *origins, int count)
{
    free_screens();
    if (!setup_screens(fbs, origins, count))
        return false;
    clamp_mouse(mouse_x, mouse_y);
    wm_foreach(keep_visible, NULL);
    wm_damage_all();
    return true;
}

void wm_stop(void)
{
    while (bottom)
        wm_destroy(bottom);
    focus = NULL;
    free_screens();
}

int wm_window_list(struct window **out, int max)
{
    int n = 0;
    for (struct window *w = bottom; w && n < max; w = w->above)
        if (framed(w) && !(w->flags & WIN_HIDDEN))
            out[n++] = w;
    return n;
}

static void get_speed(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", mouse_speed);
}

static int set_speed(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 1 || v > 10)
        return E_INVAL;
    mouse_speed = (int)v;
    return 0;
}

static const struct setting s_speed = {"mousespeed", "1..10", "pointer tracking speed (5 is normal)", get_speed,
                                       set_speed};
SETTING(s_speed);

static void get_natural(char *buf, size_t cap)
{
    str_copy(buf, natural_scroll ? "on" : "off", cap);
}

static int set_natural(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    natural_scroll = !strcmp(value, "on");
    return 0;
}

static const struct setting s_natural = {"naturalscroll", "on|off", "invert the scroll wheel", get_natural,
                                         set_natural};
SETTING(s_natural);
