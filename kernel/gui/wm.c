#include <synth/command.h>
#include <synth/error.h>
#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

#define MAX_DIRTY 32
#define RADIUS 20
#define EDGE 6
#define SHADOW 12
#define SHADOW_DY 3
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
static bool natural_scroll, left_handed;
static int double_click_ms = 400, scroll_lines = 1;
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
static struct spring lights_grow;
static struct window *more_hover_win, *more_target;
static uint64_t chrome_tick;
static struct menu_item more_items[64];
static int more_action[64];
static struct canvas fade_snap;
static uint64_t stat_frames, stat_pixels, stat_ms;
static uint64_t fade_start;
#define FADE_MS 520
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

/* Brightness and Night Shift: a table per channel applied while a row goes to the screen; the composed picture and
   the windows' surfaces keep the true colors. */
static uint8_t tint[3][256];
static volatile bool tinted;

void wm_set_tint(int brightness, int warmth)
{
    brightness = CLAMP(brightness, 10, 100);
    warmth = CLAMP(warmth, 0, 100);
    static const int keep[3] = {100, 78, 42};
    for (int ch = 0; ch < 3; ch++) {
        int scale = brightness * (10000 - warmth * (100 - keep[ch])) / 10000;
        for (int v = 0; v < 256; v++)
            tint[ch][v] = (uint8_t)(v * scale / 100);
    }
    bool was = tinted;
    tinted = brightness < 100 || warmth > 0;
    if (tinted || was)
        wm_damage_all();
}

static void fb_put(struct screen *s, int x, int y, const uint32_t *src, int w);

static void fb_row(struct screen *s, int x, int y, const uint32_t *src, int w)
{
    if (!tinted) {
        fb_put(s, x, y, src, w);
        return;
    }
    uint32_t row[128];
    for (int i = 0; i < w; i += 128) {
        int n = MIN(128, w - i);
        for (int k = 0; k < n; k++) {
            uint32_t p = src[i + k];
            row[k] = (uint32_t)tint[0][p >> 16 & 0xff] << 16 | (uint32_t)tint[1][p >> 8 & 0xff] << 8 |
                     tint[2][p & 0xff];
        }
        fb_put(s, x + i, y, row, n);
    }
}

static void fb_put(struct screen *s, int x, int y, const uint32_t *src, int w)
{
    const struct framebuffer *fb = &s->fb;
    bool native = fb->bpp == 32 && fb->red_shift == 16 && fb->green_shift == 8 && fb->blue_shift == 0;
    uint32_t bytes = fb->bpp / 8;
    int lx = x - s->area.x, ly = y - s->area.y, scale = s->scale;
    for (int sy = 0; sy < scale; sy++) {
        uint8_t *line = fb->base + (size_t)(ly * scale + sy) * fb->pitch + (size_t)lx * scale * bytes;
        if (native && scale == 1) {
            memcpy(line, src, (size_t)w * 4);
            continue;
        }
        for (int i = 0; i < w; i++) {
            uint32_t p = native ? src[i] : pack(fb, src[i]);
            for (int sx = 0; sx < scale; sx++) {
                uint8_t *d = line + (size_t)(i * scale + sx) * bytes;
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

/* The pointer is not part of the composed picture: it is drawn on the screen itself, and the interrupt that brings
   a movement moves it at once, so it glides even while a program or a long job keeps the desktop loop away. What
   it covers is put back from the back buffer. fb_busy keeps the interrupt off the screen while a frame is copied;
   the move it wanted is done right after. */
static volatile int hw_x, hw_y;
static volatile bool fb_busy, hw_dirty, hw_live;
static struct rect hw_drawn;

static void fb_compose(struct screen *s, struct rect r)
{
    r = rect_intersect(r, s->area);
    if (rect_empty(r) || r.w > 64)
        return;
    uint32_t row[64];
    const uint32_t *sprite = cursor_px[cursor_kind];
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = s->back.px + y * s->back.stride + r.x;
        for (int i = 0; i < r.w; i++) {
            int cx = r.x + i - hw_drawn.x, cy = y - hw_drawn.y;
            uint32_t c = sprite && cx >= 0 && cy >= 0 && cx < hw_drawn.w && cy < hw_drawn.h ? sprite[cy * CURSOR_SIZE + cx] : 0;
            row[i] = c >> 24 ? color_mix(src[i], c, (int)(c >> 24)) : src[i];
        }
        fb_row(s, r.x, y, row, r.w);
    }
}

/* Puts the pointer where hw_x, hw_y say: one pass over the old and new places (they overlap while it moves). */
static void hw_cursor_move(void)
{
    struct rect old = hw_drawn;
    hw_drawn = hw_live && cursor_visible && cursor_px[cursor_kind]
                   ? (struct rect){hw_x - cursor_hot[cursor_kind][0], hw_y - cursor_hot[cursor_kind][1], CURSOR_SIZE,
                                   CURSOR_SIZE}
                   : (struct rect){0, 0, 0, 0};
    struct rect both = rect_union(old, hw_drawn);
    bool joined = !rect_empty(old) && !rect_empty(hw_drawn) && both.w <= 64 && both.h <= 64;
    for (int i = 0; i < screen_count; i++) {
        if (joined) {
            fb_compose(&screens[i], both);
            continue;
        }
        if (!rect_empty(old))
            fb_compose(&screens[i], old);
        if (!rect_empty(hw_drawn))
            fb_compose(&screens[i], hw_drawn);
    }
}

static void hw_cursor_sync(void)
{
    uint32_t flags = hal_irq_save();
    hw_dirty = false;
    hw_cursor_move();
    hal_irq_restore(flags);
}

static void flush(struct screen *s, struct rect r)
{
    r = rect_intersect(r, s->area);
    if (rect_empty(r))
        return;
    for (int y = 0; y < r.h; y++)
        fb_row(s, r.x, r.y + y, s->back.px + (r.y + y) * s->back.stride + r.x, r.w);
    struct rect c = rect_intersect(r, hw_drawn);
    if (!rect_empty(c))
        fb_compose(s, c);
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

void wm_damage_client(struct window *w, struct rect r)
{
    struct rect cr = wm_client_rect(w);
    r = rect_intersect((struct rect){cr.x + r.x, cr.y + r.y, r.w, r.h}, cr);
    if (w->flags & (WIN_HIDDEN | WIN_MINIMIZED) || rect_empty(r))
        return;
    wm_damage_z(r, w->z);
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
    int top = menubar_height() ? menubar_height() : look_current()->status == STATUS_PILL ? 54 : 8;
    a.y += top;
    a.h -= top;
    if (index == 0) {
        int left = shell_left_reserved();
        a.x += left;
        a.w -= left;
        a.h -= dock_reserved();
    }
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

/* WIN_BARE: a program's own popup (menu, bubble, tooltip) — no frame, no buttons, no shadow, square corners; the
   program draws and closes it. WIN_TRANSLUCENT: its surface carries alpha (layered windows). WIN_NOACTIVATE: a click
   does not move the focus to it (WS_EX_NOACTIVATE menus and lists). */
static bool framed(const struct window *w)
{
    return !(w->flags & (WIN_POPUP | WIN_DIALOG | WIN_OVERLAY | WIN_BARE));
}

/* a window that fills its area has square corners, so it meets the menu bar and the screen edges with no gap */
static int corner(const struct window *w)
{
    return w->flags & (WIN_MAXIMIZED | WIN_FULLSCREEN) ? 0 : w->radius;
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
    int size = floating(w) ? 8 : SHADOW, dy = floating(w) ? 2 : SHADOW_DY;
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

/* Popups float over everything, windows kept on top over the rest; the order inside each layer stays. */
static int layer(const struct window *w)
{
    if (floating(w))
        return 2;
    if (w->flags & WIN_PINNED)
        return 1;
    return w->owner && w->owner != w && wm_window_alive(w->owner) && (w->owner->flags & WIN_PINNED) ? 1 : 0;
}

static void order_layers(void)
{
    struct window *was = bottom, *heads[3] = {0}, *tails[3] = {0};
    int layers[256], n = 0;
    for (struct window *w = was; w && n < 256; w = w->above)
        layers[n++] = layer(w);
    if (n == 256)
        return;
    n = 0;
    for (struct window *w = was, *next; w; w = next) {
        next = w->above;
        int l = layers[n++];
        w->below = tails[l];
        w->above = NULL;
        if (tails[l])
            tails[l]->above = w;
        else
            heads[l] = w;
        tails[l] = w;
    }
    bottom = top = NULL;
    for (int l = 0; l < 3; l++) {
        if (!heads[l])
            continue;
        if (top) {
            top->above = heads[l];
            heads[l]->below = top;
        } else {
            bottom = heads[l];
        }
        top = tails[l];
    }
}

static void restack(void)
{
    order_layers();
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

enum anim_kind {
    ANIM_OPEN,
    ANIM_CLOSE,
    ANIM_MINIMIZE,
    ANIM_RESTORE,
    ANIM_MORPH,
};

struct ghost {
    enum anim_kind kind;
    struct canvas snap;
    struct rect frame, target, drawn;
    int margin, ms;
    unsigned flags;
    uint64_t start;
    struct window *win;
    struct ghost *next;
};

static struct ghost *ghosts;
static bool animations = true;

static void draw_window(struct canvas *c, struct window *w);

static bool snapshot(struct window *w, struct canvas *snap, int margin)
{
    struct rect f = w->frame;
    struct rect r = {f.x - margin, f.y - margin, f.w + 2 * margin, f.h + 2 * margin};
    if (!canvas_alloc(snap, r.w, r.h))
        return false;
    memset(snap->px, 0, (size_t)r.w * (size_t)r.h * 4);
    struct canvas c = *snap;
    c.px = snap->px - (r.x + r.y * snap->stride);
    c.clip = r;
    unsigned saved = w->flags;
    if (!margin)
        w->flags |= WIN_NOSHADOW;
    glass_freeze(true);
    draw_window(&c, w);
    glass_freeze(false);
    w->flags = saved;
    gfx_bleed(snap);
    return true;
}

static int ghost_margin(const struct window *w)
{
    return w->flags & WIN_NOSHADOW ? 0 : SHADOW + SHADOW_DY;
}

/* How long each animation lasts at normal speed; "animspeed" scales them all. */
static int anim_speed = 100;

static int ghost_ms(enum anim_kind kind, unsigned flags)
{
    bool small = flags & WIN_POPUP && !(flags & WIN_OVERLAY);
    int ms = 200;
    switch (kind) {
    case ANIM_OPEN: ms = small ? 190 : 300; break;
    case ANIM_CLOSE: ms = small ? 120 : 180; break;
    case ANIM_MINIMIZE: ms = 380; break;
    case ANIM_RESTORE: ms = 340; break;
    case ANIM_MORPH: ms = 260; break;
    }
    return ms * 100 / anim_speed;
}

static struct ghost *ghost_new(enum anim_kind kind, struct window *w)
{
    if (!animations || !screen_count || (w->flags & WIN_BARE))
        return NULL;
    struct ghost *g = kzalloc(sizeof(*g));
    if (!g)
        return NULL;
    g->kind = kind;
    g->frame = w->frame;
    g->flags = w->flags;
    g->margin = kind == ANIM_MINIMIZE || kind == ANIM_RESTORE ? 0 : ghost_margin(w);
    g->ms = ghost_ms(kind, w->flags);
    g->start = hal_ticks_ms();
    g->next = ghosts;
    ghosts = g;
    return g;
}

static void ghost_free(struct ghost *g)
{
    if (g->win && wm_window_alive(g->win)) {
        g->win->anim_hold = false;
        wm_damage_z(outer_rect(g->win), g->win->z);
    }
    wm_damage_z(g->drawn, Z_CURSOR);
    canvas_free(&g->snap);
    kfree(g);
}

static void ghost_forget(struct window *w)
{
    for (struct ghost *g = ghosts; g; g = g->next)
        if (g->win == w) {
            g->win = NULL;
            g->start = 0;
        }
}

static void hold_window(struct window *w, enum anim_kind kind, struct rect target)
{
    if (w->anim_hold)
        return;
    struct ghost *g = ghost_new(kind, w);
    if (!g)
        return;
    g->win = w;
    g->target = target;
    w->anim_hold = true;
    int m = MAX(g->margin, glass_margin());
    wm_damage_z((struct rect){w->frame.x - m, w->frame.y - m, w->frame.w + 2 * m, w->frame.h + 2 * m}, Z_CURSOR);
}

static void release_window(struct window *w, enum anim_kind kind, struct rect target)
{
    if (w->anim_hold || !visible(w))
        return;
    struct ghost *g = ghost_new(kind, w);
    if (!g)
        return;
    g->target = target;
    if (!snapshot(w, &g->snap, g->margin)) {
        ghosts = g->next;
        kfree(g);
    }
}

static struct rect ghost_rect(const struct ghost *g, int t, int *alpha)
{
    struct rect f = g->frame;
    int m = g->margin;
    struct rect full = {f.x - m, f.y - m, f.w + 2 * m, f.h + 2 * m};
    if (g->kind == ANIM_MORPH) {
        int e = ease_spring(t);
        struct rect a = g->target;
        struct rect now = {lerp_i(a.x, f.x, e), lerp_i(a.y, f.y, e), lerp_i(a.w, f.w, e), lerp_i(a.h, f.h, e)};
        *alpha = 255;
        return (struct rect){now.x - m, now.y - m, now.w + 2 * m, now.h + 2 * m};
    }
    bool overlay = g->flags & WIN_OVERLAY, popup = g->flags & WIN_POPUP && !overlay;
    bool open = g->kind == ANIM_OPEN;
    /* Opening moves on a spring and fades in during its first part; closing speeds up as it goes, like a card let
       go of. Neither changes color, and the last opening frame is the window itself. */
    int fade = open ? ease_out(MIN(1024, t * 2)) : ease_in_out(t);
    *alpha = open ? fade * 255 / 1024 : 255 - fade * 255 / 1024;
    if (popup && (g->flags & WIN_PASSIVE)) {
        int e = open ? ease_spring(t) : ease_in_out(t);
        int shift = open ? 40 * (1024 - e) / 1024 : 40 * e / 1024;
        return (struct rect){full.x + shift, full.y, full.w, full.h};
    }
    int e = open ? ease_spring(t) : ease_in_out(t);
    int scale = open ? lerp_i(overlay ? 1060 : popup ? 940 : 920, 1024, e) : lerp_i(1024, overlay ? 1040 : 960, e);
    int cx = f.x + f.w / 2, cy = popup ? f.y : f.y + f.h / 2;
    int w = full.w * scale / 1024, h = full.h * scale / 1024;
    int x = cx - (cx - full.x) * scale / 1024, y = cy - (cy - full.y) * scale / 1024;
    if (!popup && !overlay)
        y += open ? (1024 - e) * 18 / 1024 : e * 10 / 1024;
    return (struct rect){x, y, w, h};
}

static void genie_edges(const struct ghost *g, int t, int *top, int *bottom)
{
    struct rect f = g->frame, d = g->target;
    if (g->kind == ANIM_RESTORE)
        t = 1024 - t;
    int eb = ease_in_out(MIN(1024, t * 8 / 5));
    int et = ease_in_out(t < 300 ? 0 : (t - 300) * 1024 / 724);
    *top = lerp_i(f.y, d.y, et);
    *bottom = lerp_i(f.y + f.h, d.y + d.h, eb);
}

static struct rect ghost_bounds(const struct ghost *g)
{
    int t = anim_progress(g->start, g->ms);
    if (g->kind == ANIM_MINIMIZE || g->kind == ANIM_RESTORE) {
        int top, bottom;
        genie_edges(g, t, &top, &bottom);
        int x0 = MIN(g->frame.x, g->target.x), x1 = MAX(g->frame.x + g->frame.w, g->target.x + g->target.w);
        return (struct rect){x0, top, x1 - x0, MAX(bottom - top, 1)};
    }
    int alpha;
    return ghost_rect(g, t, &alpha);
}

static void draw_genie(struct canvas *c, struct ghost *g, int t)
{
    struct rect f = g->frame, d = g->target;
    int top, bottom;
    genie_edges(g, t, &top, &bottom);
    int tt = g->kind == ANIM_RESTORE ? 1024 - t : t;
    int span = MAX(d.y + d.h - f.y, 1);
    int h = MAX(bottom - top, 1);
    int alpha = 255 - tt * 60 / 1024;
    int y0 = MAX(top, c->clip.y), y1 = MIN(bottom, c->clip.y + c->clip.h);
    for (int y = y0; y < y1; y++) {
        int u = (y - f.y) * 1024 / span;
        int k = ease_in_out(CLAMP(u + tt * 2 - 1024, 0, 1024));
        int left = lerp_i(f.x, d.x, k), right = lerp_i(f.x + f.w, d.x + d.w, k);
        int sy = (y - top) * g->snap.h / h;
        gfx_row_scaled(c, y, left, right, &g->snap, sy, alpha);
    }
}

/* Opening and closing windows are drawn in their own layer, under the Dock and the menu bar like the window itself;
   menus, overlays and windows flying into the Dock go on top. */
static bool ghost_above(const struct ghost *g)
{
    return g->kind == ANIM_MINIMIZE || g->kind == ANIM_RESTORE || (g->flags & (WIN_POPUP | WIN_OVERLAY));
}

static void draw_ghosts(struct canvas *c, bool above)
{
    for (struct ghost *g = ghosts; g; g = g->next) {
        if (ghost_above(g) != above)
            continue;
        if (!g->snap.px) {
            struct window *w = g->win;
            if (!w || !wm_window_alive(w))
                continue;
            g->frame = w->frame;
            g->flags = w->flags;
            if (w->glass && style.glass != GLASS_SOLID) {
                struct glass_look look;
                glass_look_for(&look, w->flags & WIN_OVERLAY ? GLASS_OVERLAY : w->flags & WIN_POPUP ? GLASS_MENU : GLASS_WINDOW,
                               corner(w));
                glass_prepare(c, w->glass, &look);
            }
            if (!snapshot(w, &g->snap, g->margin))
                continue;
            g->start = hal_ticks_ms();
        }
        int t = anim_progress(g->start, g->ms);
        if (g->kind == ANIM_MINIMIZE || g->kind == ANIM_RESTORE) {
            draw_genie(c, g, t);
        } else {
            int alpha;
            struct rect r = ghost_rect(g, t, &alpha);
            gfx_scale_smooth(c, r, &g->snap, alpha);
        }
    }
}

static bool animating;

/* Something is moving: the desktop loop then draws frames at an even pace instead of waiting for input. */
bool wm_animating(void)
{
    return animating;
}

static void wm_animate(void)
{
    for (struct ghost **pp = &ghosts; *pp;) {
        struct ghost *g = *pp;
        bool waiting = !g->snap.px && g->win && wm_window_alive(g->win);
        if (!g->snap.px && !waiting) {
            *pp = g->next;
            ghost_free(g);
            continue;
        }
        struct rect now = ghost_bounds(g);
        wm_damage_z(rect_union(g->drawn, now), Z_CURSOR);
        g->drawn = now;
        if (!waiting && anim_progress(g->start, g->ms) >= 1024) {
            *pp = g->next;
            ghost_free(g);
            continue;
        }
        pp = &g->next;
    }
    bool busy = ghosts != NULL;
    busy |= dock_animate();
    busy |= panel_animate();
    busy |= widgets_animate();
    busy |= desktop_animate();
    uint64_t now = hal_ticks_ms();
    int dt = chrome_tick ? (int)MIN(now - chrome_tick, 64u) : 16;
    chrome_tick = now;
    if (spring_step(&lights_grow, 500, 22, dt) && traffic_win && wm_window_alive(traffic_win)) {
        damage_title(traffic_win);
        busy = true;
    }
    animating = busy || fade_start;
    if (fade_start) {
        wm_damage_all();
        if (now >= fade_start + FADE_MS) {
            fade_start = 0;
            canvas_free(&fade_snap);
        }
    }
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
    struct rect obstacles[8];
    int n = widgets_obstacles(obstacles, 8);
    for (int i = 0; i < n; i++) {
        struct rect o = rect_intersect(r, obstacles[i]);
        if (!rect_empty(o))
            cost += (int64_t)o.w * o.h * 2;
    }
    struct rect dock = rect_intersect(r, dock_obstacle());
    if (!rect_empty(dock))
        cost += (int64_t)dock.w * dock.h * 4;
    for (struct window *it = bottom; it; it = it->above) {
        if (it == self || !visible(it) || floating(it))
            continue;
        struct rect o = rect_intersect(r, it->frame);
        if (!rect_empty(o))
            cost += (int64_t)o.w * o.h;
    }
    return cost;
}

static void best_spot(struct window *w, struct rect work, int cx, int cy);

static void place(struct window *w)
{
    int screen = focus ? wm_screen_at(focus->frame.x + focus->frame.w / 2, focus->frame.y + 10) : 0;
    struct rect work = wm_work_area(screen);
    if (w->flags & WIN_DIALOG) {
        struct rect ref = w->owner && wm_window_alive(w->owner) ? w->owner->frame : work;
        w->frame.x = CLAMP(ref.x + (ref.w - w->frame.w) / 2, work.x + 8, MAX(work.x + 8, work.x + work.w - w->frame.w - 8));
        w->frame.y = CLAMP(ref.y + MIN(60, (ref.h - w->frame.h) / 3), work.y + 8,
                           MAX(work.y + 8, work.y + work.h - w->frame.h - 8));
        return;
    }
    best_spot(w, work, work.x + (work.w - w->frame.w) / 2, work.y + (work.h - w->frame.h) / 3);
}

static void best_spot(struct window *w, struct rect work, int cx, int cy)
{
    int min_x = work.x, max_x = MAX(min_x, work.x + work.w - w->frame.w);
    int min_y = work.y + 4, max_y = MAX(min_y, work.y + work.h - w->frame.h);
    cx = CLAMP(cx, min_x, max_x);
    cy = CLAMP(cy, min_y, max_y);
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
    w->radius = flags & WIN_BARE ? 0 : flags & WIN_DIALOG ? 16 : RADIUS;
    if (flags & WIN_BARE)
        w->min_w = w->min_h = 1;
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
    if (!(flags & WIN_HIDDEN)) {
        wm_focus(w);
        hold_window(w, ANIM_OPEN, w->frame);
    }
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
    w->radius = 12;
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
    hold_window(w, ANIM_OPEN, w->frame);
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
    release_window(w, ANIM_CLOSE, w->frame);
    ghost_forget(w);
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
    if (more_hover_win == w)
        more_hover_win = NULL;
    if (more_target == w)
        more_target = NULL;
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
    release_window(w, ANIM_CLOSE, w->frame);
    w->flags |= WIN_HIDDEN;
    wm_damage(outer_rect(w));
    lose_focus(w);
    chrome_changed();
}

static void show_window(struct window *w, bool activate)
{
    bool minimized = w->flags & WIN_MINIMIZED, hidden = w->flags & WIN_HIDDEN;
    struct rect from = minimized ? dock_window_rect(w) : w->frame;
    w->flags &= ~(WIN_HIDDEN | WIN_MINIMIZED);
    canvas_free(&w->thumb);
    if (activate)
        wm_focus(w);
    else if (!floating(w))
        raise(w);
    if (minimized && !rect_empty(from))
        hold_window(w, ANIM_RESTORE, from);
    else if (hidden)
        hold_window(w, ANIM_OPEN, w->frame);
    wm_damage(outer_rect(w));
    chrome_changed();
}

void wm_show(struct window *w)
{
    show_window(w, true);
}

/* Shown on top without taking the focus, like SW_SHOWNOACTIVATE: a program's menu must not deactivate its window. */
void wm_show_passive(struct window *w)
{
    show_window(w, false);
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
    struct canvas snap = {0};
    bool animate = animations && !w->anim_hold && snapshot(w, &snap, 0);
    w->flags |= WIN_MINIMIZED;
    wm_damage(outer_rect(w));
    lose_focus(w);
    chrome_changed();
    struct rect to = animate ? dock_window_rect(w) : (struct rect){0, 0, 0, 0};
    struct ghost *g = !rect_empty(to) ? ghost_new(ANIM_MINIMIZE, w) : NULL;
    if (g) {
        g->margin = 0;
        g->snap = snap;
        g->target = to;
        g->frame = w->frame;
    } else {
        canvas_free(&snap);
    }
}

/* Steps every window aside (they shrink away) to show the desktop, and brings them back. */
void wm_peek_desktop(bool on)
{
    static struct window *had_focus;
    if (on)
        had_focus = focus;
    for (struct window *w = bottom; w; w = w->above) {
        if (on && framed(w) && visible(w)) {
            release_window(w, ANIM_CLOSE, w->frame);
            w->flags |= WIN_HIDDEN | WIN_PEEKED;
            wm_damage(outer_rect(w));
        } else if (!on && (w->flags & WIN_PEEKED)) {
            w->flags &= ~(WIN_HIDDEN | WIN_PEEKED);
            hold_window(w, ANIM_OPEN, w->frame);
            wm_damage(outer_rect(w));
        }
    }
    if (on && focus && !visible(focus))
        wm_focus(NULL);
    if (!on && had_focus && wm_window_alive(had_focus) && visible(had_focus))
        wm_focus(had_focus);
    chrome_changed();
}

void wm_set_pinned(struct window *w, bool on)
{
    if (!w || !wm_window_alive(w) || !framed(w) || !(w->flags & WIN_PINNED) == !on)
        return;
    w->flags = on ? w->flags | WIN_PINNED : w->flags & ~WIN_PINNED;
    restack();
    damage_window(w);
    for (struct window *it = bottom; it; it = it->above)
        if (it->owner == w)
            damage_window(it);
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

static void morph(struct window *w, struct rect from)
{
    if (!animations || w->anim_hold || !visible(w))
        return;
    struct ghost *g = ghost_new(ANIM_MORPH, w);
    if (!g)
        return;
    g->margin = 0;
    if (!snapshot(w, &g->snap, 0)) {
        ghosts = g->next;
        kfree(g);
        return;
    }
    g->target = from;
    g->win = w;
    w->anim_hold = true;
}

void wm_toggle_maximize(struct window *w)
{
    if (!(w->flags & WIN_RESIZABLE))
        return;
    struct rect from = w->frame;
    struct ghost *g = ghosts;
    morph(w, from);
    if (ghosts != g && ghosts->kind == ANIM_MORPH)
        g = ghosts;
    else
        g = NULL;
    if (w->flags & WIN_MAXIMIZED) {
        w->flags &= ~WIN_MAXIMIZED;
        set_frame(w, w->restore);
    } else {
        w->restore = w->frame;
        w->flags |= WIN_MAXIMIZED;
        set_frame(w, wm_work_area(wm_screen_at(w->frame.x + w->frame.w / 2, w->frame.y + 8)));
    }
    if (g)
        g->frame = w->frame;
}

/* Full screen: the window takes its whole screen, and while it is in front the menu bar and the Dock step aside
   (the menu bar comes back while the pointer is at the top edge). */
/* Snap: halves and quarters of the work area, as on Windows. The size before the first snap comes back when the
   window is restored or dragged away. */
static bool snapping = true;
static enum snap_zone snap_zone;
static struct rect snap_preview;

static struct rect zone_rect(int screen, enum snap_zone z)
{
    struct rect a = wm_work_area(screen);
    int hw = a.w / 2, hh = a.h / 2;
    switch (z) {
    case SNAP_LEFT: return (struct rect){a.x, a.y, hw, a.h};
    case SNAP_RIGHT: return (struct rect){a.x + hw, a.y, a.w - hw, a.h};
    case SNAP_TOP_LEFT: return (struct rect){a.x, a.y, hw, hh};
    case SNAP_TOP_RIGHT: return (struct rect){a.x + hw, a.y, a.w - hw, hh};
    case SNAP_BOTTOM_LEFT: return (struct rect){a.x, a.y + hh, hw, a.h - hh};
    case SNAP_BOTTOM_RIGHT: return (struct rect){a.x + hw, a.y + hh, a.w - hw, a.h - hh};
    default: return a;
    }
}

static int window_screen(const struct window *w)
{
    return wm_screen_at(w->frame.x + w->frame.w / 2, w->frame.y + 8);
}

enum snap_zone wm_snapped(const struct window *w)
{
    if (w->flags & WIN_MAXIMIZED)
        return SNAP_FILL;
    if (!(w->flags & WIN_TILED))
        return SNAP_NONE;
    for (int z = SNAP_LEFT; z <= SNAP_BOTTOM_RIGHT; z++) {
        struct rect r = zone_rect(window_screen(w), (enum snap_zone)z);
        if (z != SNAP_FILL && r.x == w->frame.x && r.y == w->frame.y && r.w == w->frame.w && r.h == w->frame.h)
            return (enum snap_zone)z;
    }
    return SNAP_NONE;
}

static void reshape(struct window *w, struct rect to)
{
    struct ghost *g = ghosts;
    morph(w, w->frame);
    g = ghosts != g && ghosts->kind == ANIM_MORPH ? ghosts : NULL;
    set_frame(w, to);
    if (g)
        g->frame = w->frame;
}

void wm_snap(struct window *w, enum snap_zone zone)
{
    if (!w || !(w->flags & WIN_RESIZABLE) || (w->flags & WIN_FULLSCREEN))
        return;
    if (w->flags & WIN_MINIMIZED)
        wm_restore(w);
    if (zone == SNAP_FILL) {
        if (!(w->flags & WIN_MAXIMIZED)) {
            if (w->flags & WIN_TILED) {
                w->flags &= ~WIN_TILED;
                w->flags |= WIN_MAXIMIZED;
                reshape(w, wm_work_area(window_screen(w)));
            } else {
                wm_toggle_maximize(w);
            }
        }
        return;
    }
    if (zone == SNAP_RESTORE || zone == SNAP_NONE) {
        if (w->flags & (WIN_TILED | WIN_MAXIMIZED)) {
            w->flags &= ~(WIN_TILED | WIN_MAXIMIZED);
            reshape(w, w->restore);
        }
        return;
    }
    if (!(w->flags & (WIN_TILED | WIN_MAXIMIZED)))
        w->restore = w->frame;
    w->flags &= ~WIN_MAXIMIZED;
    w->flags |= WIN_TILED;
    reshape(w, zone_rect(window_screen(w), zone));
}

static enum snap_zone zone_at(int x, int y)
{
    struct rect s = wm_screen_rect(wm_screen_at(x, y));
    int corner = MAX(60, s.h / 8);
    bool left = x <= s.x + 2, right = x >= s.x + s.w - 3;
    if (left || right) {
        if (y < s.y + corner)
            return left ? SNAP_TOP_LEFT : SNAP_TOP_RIGHT;
        if (y > s.y + s.h - corner)
            return left ? SNAP_BOTTOM_LEFT : SNAP_BOTTOM_RIGHT;
        return left ? SNAP_LEFT : SNAP_RIGHT;
    }
    return y <= s.y + 2 ? SNAP_FILL : SNAP_NONE;
}

static void set_snap_zone(enum snap_zone z)
{
    if (z == snap_zone)
        return;
    if (snap_zone)
        wm_damage(rect_inset(snap_preview, -4));
    snap_zone = z;
    if (z) {
        snap_preview = zone_rect(wm_screen_at(mouse_x, mouse_y), z);
        wm_damage(rect_inset(snap_preview, -4));
    }
}

static void draw_snap_preview(struct canvas *c)
{
    struct rect r = rect_inset(snap_preview, 6);
    gfx_round(c, r, 18, style.dark ? ALPHA(0xffffffffu, 46) : ALPHA(0xffffffffu, 90));
    gfx_round(c, r, 18, ALPHA(style.accent, 36));
    gfx_round_frame(c, r, 18, ALPHA(0xffffffffu, 170));
}

static void get_snap(char *buf, size_t cap)
{
    str_copy(buf, snapping ? "on" : "off", cap);
}

static int set_snap(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    snapping = !strcmp(value, "on");
    return 0;
}

static const struct setting s_snap = {"snap", "on|off", "drag windows to the screen edges to tile them", get_snap,
                                      set_snap};
SETTING(s_snap);

void wm_toggle_fullscreen(struct window *w)
{
    if (!framed(w) || (!(w->flags & WIN_RESIZABLE) && !(w->flags & WIN_FULLSCREEN)))
        return;
    struct rect from = w->frame;
    struct ghost *g = ghosts;
    morph(w, from);
    if (ghosts != g && ghosts->kind == ANIM_MORPH)
        g = ghosts;
    else
        g = NULL;
    if (w->flags & WIN_FULLSCREEN) {
        w->flags &= ~WIN_FULLSCREEN;
        set_frame(w, w->restore_full);
    } else {
        w->restore_full = w->frame;
        w->flags |= WIN_FULLSCREEN;
        set_frame(w, wm_screen_rect(wm_screen_at(w->frame.x + w->frame.w / 2, w->frame.y + 8)));
        wm_focus(w);
    }
    if (g)
        g->frame = w->frame;
    wm_damage_all();
}

bool wm_fullscreen_front(void)
{
    for (struct window *w = top; w; w = w->below)
        if (visible(w) && !w->anim_hold && framed(w))
            return (w->flags & WIN_FULLSCREEN) &&
                   rect_contains(screens[0].area, w->frame.x + w->frame.w / 2, w->frame.y + w->frame.h / 2);
    return false;
}

static bool full_menu;

bool wm_fullscreen_menu(void)
{
    return full_menu;
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


static bool has_more(const struct window *w)
{
    return framed(w) && (w->menus || w->app);
}

static struct rect more_rect(const struct window *w)
{
    int cy = traffic_cy(w);
    return (struct rect){w->frame.x + w->frame.w - 36, cy - 11, 28, 22};
}

static void draw_more(struct canvas *c, struct window *w)
{
    struct rect r = more_rect(w);
    if (w->flags & WIN_PINNED)
        gfx_symbol(c, r.x - 18, r.y + 4, SYM_PIN, 14, style.accent);
    bool hot = more_hover_win == w;
    if (hot)
        gfx_round(c, r, 11, style.hover);
    gfx_symbol(c, r.x + 6, r.y + 3, SYM_MORE, 16, hot || w == focus ? style.text : style.tertiary);
}

static void draw_lights(struct canvas *c, struct window *w)
{
    static const uint32_t colors[3] = {RGB(255, 95, 87), RGB(254, 188, 46), RGB(40, 200, 64)};
    static const uint32_t glyphs[3] = {RGBA(77, 0, 0, 220), RGBA(152, 87, 18, 230), RGBA(0, 100, 0, 220)};
    bool active = w == focus;
    bool hover = traffic_win == w;
    int cy = traffic_cy(w);
    int grow = hover ? CLAMP(lights_grow.pos, 0, 1200) : 0;
    uint32_t idle = color_mix(active ? style.titlebar : style.titlebar_inactive, style.text, active ? 70 : 40);
    for (int i = 0; i < 3; i++) {
        int cx = traffic_cx(w, i);
        bool on = light_enabled(w, i);
        int rad = 5;
        uint32_t color = hover && on ? color_mix(idle, colors[i], MIN(grow, 1024) * 255 / 1024) : idle;
        if (!on)
            color = ALPHA(idle, 90);
        if (hover && pressed_light == i)
            color = color_shade(color, -50);
        gfx_circle(c, cx, cy, rad, color);
        if (!hover || !on || grow < 500)
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
            gfx_drop_shadow(c, f, corner(w), 8, style.dark ? 90 : 34, 2);
        else
            gfx_drop_shadow(c, f, corner(w), active ? SHADOW : 8, style.dark ? (active ? 130 : 80) : (active ? 46 : 26),
                            active ? SHADOW_DY : 2);
    }
    struct rect cr = wm_client_rect(w);
    bool titled = framed(w) && !(w->flags & WIN_FULLSIZE);
    if (w->glass && style.glass != GLASS_SOLID) {
        struct glass_look look;
        glass_look_for(&look, w->flags & WIN_OVERLAY ? GLASS_OVERLAY : w->flags & WIN_POPUP ? GLASS_MENU : GLASS_WINDOW,
                       corner(w));
        glass_draw(c, w->glass, &look);
    } else if (w->glass) {
        gfx_round(c, f, corner(w), style.sidebar | 0xff000000u);
    }
    struct canvas src = w->surface, body = *c;
    if (titled) {
        struct canvas t = *c;
        t.clip = rect_intersect(c->clip, (struct rect){f.x, f.y, f.w, TITLE_HEIGHT});
        gfx_round(&t, f, corner(w), active ? style.titlebar : style.titlebar_inactive);
        gfx_fill(c, (struct rect){f.x, f.y + TITLE_HEIGHT - 1, f.w, 1}, style.separator);
        src.px -= TITLE_HEIGHT * src.stride;
        src.h += TITLE_HEIGHT;
        body.clip = rect_intersect(c->clip, cr);
    }
    gfx_blit_round(&body, f, &src, corner(w), w->glass != NULL || (w->flags & WIN_TRANSLUCENT));
    if (framed(w)) {
        if (!(w->flags & WIN_NOTITLE)) {
            struct rect tr = {f.x + 76, f.y, f.w - 152, title_band(w)};
            if (w->flags & WIN_FULLSIZE)
                tr.w = 0;
            gfx_text_box(c, tr, &font_bold, w->title, active ? ALPHA(style.text, 220) : style.tertiary, ALIGN_CENTER);
        }
        draw_lights(c, w);
        if (has_more(w))
            draw_more(c, w);
    }
    if (!(w->flags & (WIN_OVERLAY | WIN_BARE))) {
        gfx_round_frame(c, f, corner(w), style.dark ? ALPHA(0, 230) : ALPHA(0, 44));
        if (style.dark)
            gfx_round_frame_gradient(c, rect_inset(f, 1), MAX(corner(w) - 1, 0), ALPHA(0xffffffffu, 40), ALPHA(0xffffffffu, 12));
    }
}

void wm_crossfade(void)
{
    if (!animations || !screen_count)
        return;
    struct screen *s = &screens[0];
    if (!fade_snap.px || fade_snap.w != s->area.w || fade_snap.h != s->area.h) {
        canvas_free(&fade_snap);
        if (!canvas_alloc(&fade_snap, s->area.w, s->area.h))
            return;
    }
    for (int y = 0; y < s->area.h; y++)
        memcpy(fade_snap.px + y * fade_snap.stride, s->back.px + (s->area.y + y) * s->back.stride + s->area.x,
               (size_t)s->area.w * 4);
    fade_start = hal_ticks_ms();
    wm_damage_all();
}

static void draw_fade(struct canvas *c, int index)
{
    if (!fade_start || index != 0)
        return;
    int t = anim_progress(fade_start, FADE_MS);
    int alpha = 255 - ease_in_out(t) * 255 / 1024;
    if (alpha <= 0)
        return;
    struct rect a = screens[0].area;
    gfx_blit_alpha(c, a.x, a.y, &fade_snap, (struct rect){0, 0, fade_snap.w, fade_snap.h}, alpha);
}

/* An opaque window that hides all of the area: nothing below it needs drawing. */
static bool covers(const struct window *w, struct rect area)
{
    if (!visible(w) || w->anim_hold || floating(w) || w->glass || (w->flags & WIN_TRANSLUCENT))
        return false;
    struct rect inner = rect_intersect(rect_inset(w->frame, MAX(corner(w), 1)), area);
    return inner.x == area.x && inner.y == area.y && inner.w == area.w && inner.h == area.h;
}

static void compose(int index, struct rect area)
{
    struct screen *s = &screens[index];
    compose_area = s->area;
    struct canvas *c = &s->back;
    c->clip = rect_intersect(area, s->area);
    struct window *from = NULL;
    for (struct window *w = top; w && !from; w = w->below)
        if (covers(w, c->clip))
            from = w;
    if (!from)
        desktop_paint(c, index, c->clip);
    for (struct window *w = from ? from : bottom; w; w = w->above) {
        if (snap_zone && w == drag_win)
            draw_snap_preview(c);
        if (visible(w) && !w->anim_hold && !floating(w) && !rect_empty(rect_intersect(outer_rect(w), c->clip)))
            draw_window(c, w);
    }
    if (ghosts)
        draw_ghosts(c, false);
    bool full = index == 0 && wm_fullscreen_front();
    if (!full || full_menu)
        menubar_paint(c, index);
    if (!full)
        dock_paint(c, index);
    if (index == 0)
        dock_paint_drag(c);
    for (struct window *w = bottom; w; w = w->above)
        if (visible(w) && !w->anim_hold && floating(w) && !rect_empty(rect_intersect(outer_rect(w), c->clip)))
            draw_window(c, w);
    if (ghosts)
        draw_ghosts(c, true);
    draw_fade(c, index);
}

void wm_render(void)
{
    if (rendering || !screen_count)
        return;
    rendering = true;
    glass_defer(drag == DRAG_MOVE || drag == DRAG_RESIZE || ghosts);
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
        uint64_t t0 = hal_ticks_ms();
        for (int k = 0; k < count; k++) {
            compose(i, list[k]);
            stat_pixels += (uint64_t)MAX(list[k].w, 0) * (uint64_t)MAX(list[k].h, 0);
        }
        if (count)
            stat_frames++;
        stat_ms += hal_ticks_ms() - t0;
        fb_busy = true;
        for (int k = 0; k < count; k++)
            flush(s, list[k]);
        fb_busy = false;
    }
    if (hw_dirty)
        hw_cursor_sync();
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

static void hw_cursor_refresh(void)
{
    if (rendering || fb_busy)
        hw_dirty = true;
    else
        hw_cursor_sync();
}

static void set_cursor(int kind)
{
    if (kind == cursor_kind)
        return;
    cursor_kind = kind;
    hw_cursor_refresh();
}

static void set_traffic(struct window *w)
{
    if (traffic_win == w)
        return;
    lights_grow.target = w ? 1024 : 0;
    if (w && !traffic_win)
        spring_set(&lights_grow, 0), lights_grow.target = 1024;
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


enum {
    ACT_NONE,
    ACT_KEY,
    ACT_MINIMIZE,
    ACT_ZOOM,
    ACT_FULL,
    ACT_PIN,
    ACT_CLOSE,
    ACT_SNAP,
};

static void more_pick(struct window *owner, int index)
{
    UNUSED(owner);
    struct window *w = more_target;
    if (!w || !wm_window_alive(w) || index < 0 || index >= 64)
        return;
    int act = more_action[index];
    if (act == ACT_MINIMIZE)
        wm_minimize(w);
    else if (act == ACT_ZOOM)
        wm_toggle_maximize(w);
    else if (act == ACT_FULL)
        wm_toggle_fullscreen(w);
    else if (act == ACT_PIN)
        wm_set_pinned(w, !(w->flags & WIN_PINNED));
    else if (act == ACT_CLOSE)
        wm_close(w);
    else if (act >= ACT_SNAP)
        wm_snap(w, (enum snap_zone)(act - ACT_SNAP));
    else if (act == ACT_KEY && more_items[index].key > 0) {
        wm_focus(w);
        wm_key(more_items[index].key);
    }
}

void wm_window_menu(struct window *w, int x, int y)
{
    int n = 0;
    more_target = w;
    for (int i = 0; w->menus && i < w->menu_count; i++) {
        const struct app_menu *m = &w->menus[i];
        if (n > 40)
            break;
        if (n)
            more_action[n] = ACT_NONE, more_items[n++] = (struct menu_item){"", 0, NULL};
        more_action[n] = ACT_NONE;
        more_items[n++] = (struct menu_item){m->title, MENU_HEADER, NULL};
        for (int k = 0; k < m->count && n < 42; k++) {
            more_action[n] = m->items[k].label[0] ? ACT_KEY : ACT_NONE;
            more_items[n++] = m->items[k];
        }
    }
    if (n)
        more_action[n] = ACT_NONE, more_items[n++] = (struct menu_item){"", 0, NULL};
    more_action[n] = ACT_PIN;
    more_items[n++] = (struct menu_item){w->flags & WIN_PINNED ? "Stop Keeping on Top" : "Keep on Top", 0, NULL};
    more_action[n] = ACT_MINIMIZE;
    more_items[n++] = (struct menu_item){"Minimize", 0, "\xe2\x8c\x98M"};
    if (w->flags & WIN_RESIZABLE) {
        more_action[n] = ACT_ZOOM;
        more_items[n++] = (struct menu_item){w->flags & WIN_MAXIMIZED ? "Restore Size" : "Fill Screen", 0, NULL};
    }
    if ((w->flags & WIN_RESIZABLE) && !(w->flags & WIN_FULLSCREEN)) {
        static const struct {
            const char *label, *keys;
            enum snap_zone z;
        } tiles[] = {{"Tile Left", "\xe2\x8c\x98\xe2\x86\x90", SNAP_LEFT},
                     {"Tile Right", "\xe2\x8c\x98\xe2\x86\x92", SNAP_RIGHT},
                     {"Top Left Quarter", NULL, SNAP_TOP_LEFT},
                     {"Top Right Quarter", NULL, SNAP_TOP_RIGHT},
                     {"Bottom Left Quarter", NULL, SNAP_BOTTOM_LEFT},
                     {"Bottom Right Quarter", NULL, SNAP_BOTTOM_RIGHT}};
        more_action[n] = ACT_NONE, more_items[n++] = (struct menu_item){"", 0, NULL};
        for (size_t i = 0; i < ARRAY_SIZE(tiles); i++) {
            more_action[n] = ACT_SNAP + tiles[i].z;
            more_items[n++] = (struct menu_item){tiles[i].label, 0, tiles[i].keys};
        }
        if (w->flags & WIN_TILED) {
            more_action[n] = ACT_SNAP + SNAP_RESTORE;
            more_items[n++] = (struct menu_item){"Return to Previous Size", 0, NULL};
        }
        more_action[n] = ACT_NONE, more_items[n++] = (struct menu_item){"", 0, NULL};
    }
    if (w->flags & (WIN_RESIZABLE | WIN_FULLSCREEN)) {
        more_action[n] = ACT_FULL;
        more_items[n++] = (struct menu_item){w->flags & WIN_FULLSCREEN ? "Exit Full Screen" : "Enter Full Screen", 0, "F11"};
    }
    more_action[n] = ACT_CLOSE;
    more_items[n++] = (struct menu_item){"Close", 0, "\xe2\x8c\x98W"};
    ui_menu_items(NULL, x, y, more_items, n, more_pick);
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
    if (!floating(w) && !(w->flags & WIN_NOACTIVATE))
        wm_focus(w);
    else if (w->flags & WIN_NOACTIVATE)
        raise(w);
    if (has_more(w) && rect_contains(more_rect(w), mouse_x, mouse_y)) {
        struct rect r = more_rect(w);
        wm_window_menu(w, r.x + r.w - 230, r.y + r.h + 6);
        return;
    }
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
    if ((mode == DRAG_MOVE || mode == DRAG_CLIENT) && snap_zone) {
        enum snap_zone z = snap_zone;
        set_snap_zone(SNAP_NONE);
        wm_snap(w, z);
    }
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
        if (drag_win->flags & (WIN_MAXIMIZED | WIN_TILED)) {
            int rel = drag_x * drag_win->restore.w / MAX(drag_win->frame.w, 1);
            drag_win->flags &= ~(WIN_MAXIMIZED | WIN_TILED);
            set_frame(drag_win, (struct rect){mouse_x - rel, drag_win->frame.y, drag_win->restore.w, drag_win->restore.h});
            drag_x = rel;
        }
        struct rect s = wm_screen_rect(wm_screen_at(mouse_x, mouse_y));
        int y = MAX(mouse_y - drag_y, s.y + menubar_height());
        wm_move(drag_win, mouse_x - drag_x, y);
        set_snap_zone(snapping && (drag_win->flags & WIN_RESIZABLE) ? zone_at(mouse_x, mouse_y) : SNAP_NONE);
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
            f.y = MAX(drag_frame.y + drag_frame.h - h, menubar_height());
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
    desktop_hover(mouse_x, mouse_y, hit == HIT_DESKTOP);
    if (hit != HIT_WINDOW || !w) {
        set_hover(NULL);
        set_traffic(NULL);
        if (more_hover_win && wm_window_alive(more_hover_win))
            damage_title(more_hover_win);
        more_hover_win = NULL;
        set_cursor(CUR_ARROW);
        return;
    }
    if (drag != DRAG_TRAFFIC)
        set_traffic(in_traffic_zone(w, mouse_x, mouse_y) ? w : NULL);
    struct window *mh = has_more(w) && rect_contains(more_rect(w), mouse_x, mouse_y) ? w : NULL;
    if (mh != more_hover_win) {
        if (more_hover_win && wm_window_alive(more_hover_win))
            damage_title(more_hover_win);
        more_hover_win = mh;
        if (mh)
            damage_title(mh);
    }
    set_cursor(cursor_for_edges(edges_at(w, mouse_x, mouse_y)));
    set_hover(w);
    if (rect_contains(wm_client_rect(w), mouse_x, mouse_y))
        client_event(w, EV_MOUSE_MOVE, 0, 0, 0);
}

static void nearest_point(int *px, int *py)
{
    int x = *px, y = *py;
    for (int i = 0; i < screen_count; i++)
        if (rect_contains(screens[i].area, x, y))
            return;
    int best = -1, best_d = 0, bx = x, by = y;
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
    *px = bx;
    *py = by;
}

static void clamp_mouse(int x, int y)
{
    nearest_point(&x, &y);
    mouse_x = x;
    mouse_y = y;
    struct rect a = screens[0].area;
    bool want = wm_fullscreen_front() && rect_contains(a, x, y) &&
                y < a.y + (full_menu ? MAX(menubar_height(), 2) : 2);
    if (want != full_menu) {
        full_menu = want;
        wm_damage_all();
    }
}

static void absolute_point(const struct pointer_event *ev, int *x, int *y)
{
    struct rect all = screens[0].area;
    for (int i = 1; i < screen_count; i++)
        all = rect_union(all, screens[i].area);
    *x = all.x + (int)((int64_t)ev->x * (all.w - 1) / 65535);
    *y = all.y + (int)((int64_t)ev->y * (all.h - 1) / 65535);
}

/* Runs in the mouse interrupt: places the pointer and moves it on the screen right away. */
static int irq_accel_x, irq_accel_y;

static void pointer_irq(struct pointer_event *ev)
{
    if (!hw_live || !screen_count)
        return;
    int x, y;
    if (ev->absolute) {
        absolute_point(ev, &x, &y);
    } else {
        irq_accel_x += ev->dx * mouse_speed;
        irq_accel_y += ev->dy * mouse_speed;
        int dx = irq_accel_x / 5, dy = irq_accel_y / 5;
        irq_accel_x -= dx * 5;
        irq_accel_y -= dy * 5;
        x = hw_x + dx;
        y = hw_y + dy;
    }
    nearest_point(&x, &y);
    ev->x = x;
    ev->y = y;
    ev->placed = true;
    if (x == hw_x && y == hw_y)
        return;
    hw_x = x;
    hw_y = y;
    if (fb_busy)
        hw_dirty = true;
    else
        hw_cursor_move();
}

void wm_pointer(const struct pointer_event *ev)
{
    int old_x = mouse_x, old_y = mouse_y;
    if (ev->placed) {
        clamp_mouse(ev->x, ev->y);
    } else if (ev->absolute) {
        int x, y;
        absolute_point(ev, &x, &y);
        clamp_mouse(x, y);
    } else {
        accel_x += ev->dx * mouse_speed;
        accel_y += ev->dy * mouse_speed;
        int dx = accel_x / 5, dy = accel_y / 5;
        accel_x -= dx * 5;
        accel_y -= dy * 5;
        clamp_mouse(mouse_x + dx, mouse_y + dy);
    }
    if (old_x != mouse_x || old_y != mouse_y) {
        if (!ev->placed) {
            hw_x = mouse_x;
            hw_y = mouse_y;
            hw_cursor_refresh();
        }
        mouse_moved();
    }
    unsigned buttons = ev->buttons;
    if (left_handed)
        buttons = (buttons & ~(unsigned)(BUTTON_LEFT | BUTTON_RIGHT)) | (buttons & BUTTON_LEFT ? BUTTON_RIGHT : 0) |
                  (buttons & BUTTON_RIGHT ? BUTTON_LEFT : 0);
    unsigned changed = buttons ^ mouse_buttons;
    for (unsigned b = 1; b <= 4; b <<= 1) {
        if (!(changed & b))
            continue;
        if (buttons & b) {
            mouse_buttons |= b;
            uint64_t now = hal_ticks_ms();
            int dist = MAX(mouse_x > last_click_x ? mouse_x - last_click_x : last_click_x - mouse_x,
                           mouse_y > last_click_y ? mouse_y - last_click_y : last_click_y - mouse_y);
            struct window *at;
            hit_test(mouse_x, mouse_y, &at);
            if (b == BUTTON_LEFT && now - last_click_time < (uint64_t)double_click_ms && dist < 5 && at == last_click_win)
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
            client_event(w, EV_WHEEL, 0, (natural_scroll ? -ev->wheel : ev->wheel) * scroll_lines, 0);
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

struct window *wm_window_at(int x, int y)
{
    for (struct window *w = top; w; w = w->below)
        if (visible(w) && !w->anim_hold && rect_contains(w->frame, x, y))
            return w;
    return NULL;
}

void wm_animate_ghosts(void)
{
    if (ghosts)
        wm_animate();
}

void wm_timers(void)
{
    if (ghosts || screen_count)
        wm_animate();
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
    hw_cursor_refresh();
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
    hw_x = mouse_x;
    hw_y = mouse_y;
    hw_drawn = (struct rect){0, 0, 0, 0};
    hw_live = true;
    hw_dirty = true;
    hal_pointer_hook(pointer_irq);
    wm_damage_all();
    return true;
}

static void hw_cursor_off(void)
{
    uint32_t flags = hal_irq_save();
    hw_live = false;
    hw_drawn = (struct rect){0, 0, 0, 0};
    hal_irq_restore(flags);
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
    if (w->flags & WIN_FULLSCREEN) {
        set_frame(w, wm_screen_rect(wm_screen_at(cx, ty)));
        return;
    }
    if (w->flags & WIN_MAXIMIZED) {
        set_frame(w, wm_work_area(wm_screen_at(cx, ty)));
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

static void avoid_widgets(struct window *w, void *ctx)
{
    UNUSED(ctx);
    if (!framed(w) || w->flags & (WIN_MAXIMIZED | WIN_DIALOG | WIN_OVERLAY | WIN_HIDDEN | WIN_MINIMIZED))
        return;
    struct rect obstacles[8];
    int n = widgets_obstacles(obstacles, 8);
    bool hit = false;
    for (int i = 0; i < n && !hit; i++)
        hit = !rect_empty(rect_intersect(w->frame, obstacles[i]));
    if (!hit)
        return;
    struct rect f = w->frame;
    best_spot(w, wm_work_area(wm_screen_at(f.x + f.w / 2, f.y + 10)), f.x, f.y);
    struct rect to = w->frame;
    w->frame = f;
    set_frame(w, to);
}

void wm_reflow(void)
{
    wm_foreach(keep_visible, NULL);
    wm_foreach(avoid_widgets, NULL);
    wm_damage_all();
}

bool wm_reconfigure(const struct framebuffer *fbs, const struct rect *origins, int count)
{
    hw_cursor_off();
    free_screens();
    if (!setup_screens(fbs, origins, count))
        return false;
    clamp_mouse(mouse_x, mouse_y);
    hw_x = mouse_x;
    hw_y = mouse_y;
    hw_live = true;
    hw_dirty = true;
    wm_foreach(keep_visible, NULL);
    wm_damage_all();
    return true;
}

void wm_stop(void)
{
    hal_pointer_hook(NULL);
    hw_cursor_off();
    while (bottom)
        wm_destroy(bottom);
    focus = NULL;
    free_screens();
}

unsigned wm_mouse_buttons(void)
{
    return mouse_buttons;
}

/* No window is still flying open or closed. */
bool wm_settled(void)
{
    return !ghosts;
}

/* What the screens show (without the pointer), for screenshots. */
bool wm_capture(struct rect r, struct canvas *out)
{
    if (rect_empty(r) || !canvas_alloc(out, r.w, r.h))
        return false;
    memset(out->px, 0, (size_t)r.w * (size_t)r.h * 4);
    for (int i = 0; i < screen_count; i++) {
        struct screen *s = &screens[i];
        struct rect c = rect_intersect(r, s->area);
        for (int y = c.y; y < c.y + c.h; y++)
            memcpy(out->px + (y - r.y) * out->stride + (c.x - r.x), s->back.px + y * s->back.stride + c.x,
                   (size_t)c.w * 4);
    }
    return true;
}

/* A window as if nothing covered it; its rounded corners stay transparent. */
bool wm_window_image(struct window *w, struct canvas *out)
{
    return snapshot(w, out, 0);
}

struct window *wm_window_at_point(int x, int y)
{
    for (struct window *w = top; w; w = w->below)
        if (framed(w) && visible(w) && rect_contains(w->frame, x, y))
            return w;
    return NULL;
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

static void get_double(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", double_click_ms);
}

static int set_double(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 150 || v > 1500)
        return E_INVAL;
    double_click_ms = (int)v;
    return 0;
}

static void get_scroll(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", scroll_lines);
}

static int set_scroll(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 1 || v > 5)
        return E_INVAL;
    scroll_lines = (int)v;
    return 0;
}

static void get_primary(char *buf, size_t cap)
{
    str_copy(buf, left_handed ? "right" : "left", cap);
}

static int set_primary(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "left") && strcmp(value, "right"))
        return E_INVAL;
    left_handed = !strcmp(value, "right");
    return 0;
}

static const struct setting s_double = {"doubleclick", "150..1500",
                                        "longest pause between the clicks of a double-click, ms", get_double,
                                        set_double};
static const struct setting s_scroll = {"scrollspeed", "1..5", "how far one notch of the wheel scrolls", get_scroll,
                                        set_scroll};
static const struct setting s_primary = {"mouseprimary", "left|right", "the button that clicks (right for left hands)",
                                         get_primary, set_primary};
SETTING(s_double);
SETTING(s_scroll);
SETTING(s_primary);

static void get_animations(char *buf, size_t cap)
{
    str_copy(buf, animations ? "on" : "off", cap);
}

static int set_animations(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    animations = !strcmp(value, "on");
    return 0;
}

static const struct setting s_animations = {"animations", "on|off", "animate windows, menus and the Dock",
                                            get_animations, set_animations};
SETTING(s_animations);

static void get_speed_anim(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", anim_speed);
}

static int set_speed_anim(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 50 || v > 200)
        return E_INVAL;
    anim_speed = (int)v;
    return 0;
}

static const struct setting s_anim_speed = {"animspeed", "50..200", "speed of window animations in percent",
                                            get_speed_anim, set_speed_anim};
SETTING(s_anim_speed);

bool wm_animations(void)
{
    return animations;
}

COMMAND(windows, NULL, "system", "windows [pin|unpin|max|full <n>] [frame <n>]",
        "list the open windows; keep one on top, fill the screen, go full screen or show where it is")
{
    if (!screen_count)
        return sh_error(sh, "windows", "the desktop is not running");
    struct window *list[64];
    int n = wm_window_list(list, 64);
    if (argc == 1) {
        for (int i = 0; i < n; i++) {
            sh_printf(sh, "%2d  %s", i + 1, list[i]->title);
            if (list[i]->flags & (WIN_PINNED | WIN_MINIMIZED)) {
                sh_role(sh, ROLE_MUTED);
                sh_printf(sh, "%s%s", list[i]->flags & WIN_PINNED ? "  (on top)" : "",
                          list[i]->flags & WIN_MINIMIZED ? "  (minimized)" : "");
                sh_role(sh, ROLE_TEXT);
            }
            sh_puts(sh, "\n");
        }
        return 0;
    }
    long k;
    const char *op = argv[1];
    if (argc != 3 || !str_parse_long(argv[2], &k))
        return command_usage(sh, command_find("windows"));
    if (k < 1 || k > n)
        return sh_error(sh, "windows", "there is no window %ld", k);
    struct window *w = list[k - 1];
    if (!strcmp(op, "pin") || !strcmp(op, "unpin"))
        wm_set_pinned(w, !strcmp(op, "pin"));
    else if (!strcmp(op, "max"))
        wm_toggle_maximize(w);
    else if (!strcmp(op, "full"))
        wm_toggle_fullscreen(w);
    else if (!strcmp(op, "frame"))
        sh_printf(sh, "%d %d %d %d%s%s\n", w->frame.x, w->frame.y, w->frame.w, w->frame.h,
                  w->flags & WIN_MAXIMIZED ? " max" : "", w->flags & WIN_FULLSCREEN ? " full" : "");
    else
        return command_usage(sh, command_find("windows"));
    return 0;
}

COMMAND(wmstat, NULL, "dev", "wmstat [seconds]", "count desktop frames while idle (frames, pixels, glass, time)")
{
    if (!screen_count)
        return sh_error(sh, "wmstat", "the desktop is not running");
    long secs = 3;
    if (argc > 1 && (!str_parse_long(argv[1], &secs) || secs < 1))
        return command_usage(sh, command_find("wmstat"));
    uint64_t f0 = stat_frames, p0 = stat_pixels, m0 = stat_ms, g0 = glass_stat_renders, gp0 = glass_stat_pixels;
    uint64_t start = hal_ticks_ms();
    while (hal_ticks_ms() - start < (uint64_t)secs * 1000) {
        int k = input_getkey(50);
        if (k > 0)
            break;
    }
    uint64_t el = MAX(hal_ticks_ms() - start, 1u);
    sh_printf(sh, "%u frames/s, %u Kpx/s composed, %u ms/s composing, %u glass renders/s (%u Kpx/s)\n",
              (unsigned)((stat_frames - f0) * 1000 / el), (unsigned)((stat_pixels - p0) / el),
              (unsigned)((stat_ms - m0) * 1000 / el), (unsigned)((glass_stat_renders - g0) * 1000 / el),
              (unsigned)((glass_stat_pixels - gp0) / el));
    return 0;
}

COMMAND(wmbench, NULL, "dev", "wmbench [frames]", "time full-screen desktop redraws (and an open/close animation)")
{
    if (!screen_count)
        return sh_error(sh, "wmbench", "the desktop is not running");
    long frames = 20;
    if (argc > 1 && (!str_parse_long(argv[1], &frames) || frames < 1))
        return command_usage(sh, command_find("wmbench"));
    uint64_t t0 = hal_ticks_ms();
    for (long i = 0; i < frames; i++) {
        wm_damage_all();
        wm_render();
    }
    uint64_t full = hal_ticks_ms() - t0;
    t0 = hal_ticks_ms();
    long anim = 0;
    struct window *w = top && framed(top) ? top : NULL;
    if (w) {
        struct rect f = w->frame;
        for (long i = 0; i < frames; i++) {
            wm_damage_z((struct rect){f.x - 46, f.y - 46, f.w + 92, f.h + 92}, Z_CURSOR);
            wm_render();
            anim++;
        }
    }
    uint64_t part = hal_ticks_ms() - t0;
    struct canvas snap;
    uint64_t zoom = 0;
    if (w && snapshot(w, &snap, 0)) {
        struct canvas c = screens[0].back;
        c.clip = screens[0].area;
        struct rect f = w->frame;
        t0 = hal_ticks_ms();
        for (long i = 0; i < frames; i++) {
            int k = 940 + (int)(i % 8) * 10;
            gfx_scale_smooth(&c, (struct rect){f.x + f.w * (1024 - k) / 2048, f.y + f.h * (1024 - k) / 2048,
                                               f.w * k / 1024, f.h * k / 1024}, &snap, 230);
        }
        zoom = hal_ticks_ms() - t0;
        canvas_free(&snap);
        wm_damage_all();
        wm_render();
    }
    sh_printf(sh, "full screen %dx%d: %u ms per frame\n", wm_screen_rect(0).w, wm_screen_rect(0).h,
              (unsigned)(full / (uint64_t)frames));
    if (anim)
        sh_printf(sh, "front window area: %u ms per frame\nfront window zoom: %u ms per frame\n",
                  (unsigned)(part / (uint64_t)anim), (unsigned)(zoom / (uint64_t)anim));
    return 0;
}
