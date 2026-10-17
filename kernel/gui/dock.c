#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

#define MAX_ITEMS 28
#define PAD 8
#define GAP 6
#define BOTTOM 6
#define TOOLTIP_H 30
#define SEPARATOR_W 13
#define TRASH_DIR "/home/.Trash"
#define RAIL_X 10
#define RAIL_W0 64
#define RAIL_W1 212
#define RAIL_ROW 50
#define CORNER_ICON 44
#define CORNER_PAD 15
#define CORNER_GAP 12
#define CORNER_ROW_MAX 7
#define GRID_COLS 4
#define GRID_CELL 86
#define GRID_MAX 24
#define FILLET 22
#define ARC_R0 150
#define ARC_R1 178
#define ARC_MAX 8
#define ORB_R 54

enum item_kind {
    DI_APP,
    DI_LAUNCHPAD,
    DI_SEPARATOR,
    DI_WINDOW,
    DI_TRASH,
    DI_SYSTEM,
};

struct item {
    enum item_kind kind;
    const struct gui_app *app;
    struct window *win;
    int size, x;
    struct rect r;
    bool running;
};

static const char *const pinned[] = {"Files",       "Launchpad",  "Browser", "Terminal",       "Notepad", "Photos",
                                     "Music",       "Calculator", "Paint",   "System Monitor", "Minesweeper",
                                     "System Settings"};
static const char *const short_pinned[] = {"Files", "Browser", "Terminal", "Notepad", "Music", "System Settings"};

static struct item items[MAX_ITEMS];
static int count;
static struct spring lift[MAX_ITEMS];
static const struct gui_app *grid[GRID_MAX];
static struct rect grid_r[GRID_MAX];
static int grid_count, grid_hover = -1, grid_pressed = -1;
static int base_size = 52;
static bool magnify = true;
static bool autohide;
static int hover_x = -1, hover_item = -1, pressed_item = -1;
static bool revealed, over;
static int mag_x = -1;
static struct spring grow, hl, hl_alpha;
static uint64_t last_tick;
static const struct gui_app *bounce_app;
static uint64_t bounce_start;
static struct glass glass;
static struct rect dock_rect, zone;
static bool started;
static enum dock_kind kind;

struct corner_shape {
    int x, y, w, h, radius, fillet, bottom;
};

struct arc_shape {
    int cx, cy, inner, outer, orb;
};

static struct corner_shape corner;
static struct arc_shape arc;

static const int16_t sin_deg[91] = {
    0,    18,   36,   54,   71,   89,   107,  125,  143,  160,  178,  195,  213,  230,  248,  265,  282,  299,  316,
    333,  350,  367,  384,  400,  416,  433,  449,  465,  481,  496,  512,  527,  543,  558,  573,  587,  602,  616,
    630,  644,  658,  672,  685,  698,  711,  724,  737,  749,  761,  773,  784,  796,  807,  818,  828,  839,  849,
    859,  868,  878,  887,  896,  904,  912,  920,  928,  935,  943,  949,  956,  962,  968,  974,  979,  984,  989,
    994,  998,  1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024};

static int sin_d(int deg)
{
    return sin_deg[CLAMP(deg, 0, 90)];
}

static int cos_d(int deg)
{
    return sin_deg[CLAMP(90 - deg, 0, 90)];
}

static int max_size(void)
{
    return magnify ? base_size * 3 / 2 : base_size;
}

static bool is_pinned(const struct gui_app *app)
{
    for (size_t i = 0; i < ARRAY_SIZE(pinned); i++)
        if (!strcmp(pinned[i], app->name))
            return true;
    return false;
}

static void add_running(const struct gui_app *app, void *ctx)
{
    UNUSED(ctx);
    if (count < MAX_ITEMS - 4 && !is_pinned(app) && wm_app_window_count(app) > 0)
        items[count++] = (struct item){.kind = DI_APP, .app = app, .running = true};
}

static void add_grid(const struct gui_app *app, void *ctx)
{
    UNUSED(ctx);
    if (grid_count < GRID_MAX && app->launch && strcmp(app->name, "Launchpad"))
        grid[grid_count++] = app;
}

static void add_app(const char *name)
{
    if (!strcmp(name, "Launchpad")) {
        if (kind == DOCK_CENTER || kind == DOCK_RAIL)
            items[count++] = (struct item){.kind = DI_LAUNCHPAD};
        return;
    }
    const struct gui_app *app = gui_app_find(name);
    if (app && !strcmp(app->name, name))
        items[count++] = (struct item){.kind = DI_APP, .app = app, .running = wm_app_window_count(app) > 0};
}

static void build(void)
{
    count = 0;
    if (kind != DOCK_CENTER && kind != DOCK_ARC)
        items[count++] = (struct item){.kind = DI_SYSTEM};
    if (kind == DOCK_CORNER)
        items[count++] = (struct item){.kind = DI_LAUNCHPAD};
    if (kind == DOCK_CENTER || kind == DOCK_RAIL) {
        for (size_t i = 0; i < ARRAY_SIZE(pinned) && count < MAX_ITEMS; i++)
            add_app(pinned[i]);
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(short_pinned) && count < MAX_ITEMS; i++)
            add_app(short_pinned[i]);
    }
    int pinned_end = count;
    gui_app_foreach(add_running, NULL);
    struct window *list[32];
    int n = wm_window_list(list, 32);
    for (int i = 0; i < n && count < MAX_ITEMS - 4; i++) {
        bool seen = !list[i]->app;
        for (int k = 0; k < count && !seen; k++)
            seen = items[k].kind == DI_APP && items[k].app == list[i]->app;
        if (!seen)
            items[count++] = (struct item){.kind = DI_APP, .app = list[i]->app, .running = true};
    }
    for (int k = pinned_end; k < count; k++)
        items[k].running = true;
    for (int k = 0; k < count; k++)
        if (items[k].kind == DI_APP && !items[k].running)
            items[k].running = wm_app_window_count(items[k].app) > 0;
    if (kind != DOCK_ARC)
        items[count++] = (struct item){.kind = DI_SEPARATOR};
    for (int i = 0; i < n && count < MAX_ITEMS - 1; i++)
        if ((list[i]->flags & WIN_MINIMIZED) && list[i]->thumb.px)
            items[count++] = (struct item){.kind = DI_WINDOW, .win = list[i]};
    items[count++] = (struct item){.kind = DI_TRASH};
    if (kind == DOCK_ARC && count > ARC_MAX) {
        items[ARC_MAX - 1] = items[count - 1];
        count = ARC_MAX;
    }
    grid_count = 0;
    gui_app_foreach(add_grid, NULL);
}

static bool visible_now(void)
{
    return kind != DOCK_CENTER || !autohide || revealed;
}

int dock_reserved(void)
{
    if (!started && look_current()->dock != DOCK_CENTER)
        return 0;
    if ((started ? kind : look_current()->dock) != DOCK_CENTER)
        return 0;
    return autohide ? 0 : base_size + 2 * PAD + BOTTOM + 4;
}

int shell_left_reserved(void)
{
    const struct gui_look *l = look_current();
    if (l->dock == DOCK_RAIL)
        return RAIL_X + RAIL_W0 + 8;
    if (l->places)
        return PLACES_W + 14;
    return 0;
}

bool dock_hidden(void)
{
    return !visible_now();
}

static int item_width(const struct item *it)
{
    return it->kind == DI_SEPARATOR ? SEPARATOR_W : it->size;
}

static int bounce_offset(const struct item *it)
{
    if (!bounce_app || it->kind != DI_APP || it->app != bounce_app)
        return 0;
    uint64_t t = hal_ticks_ms() - bounce_start;
    if (t >= 1000)
        return 0;
    int phase = (int)(t % 500) * 1024 / 500, height = t < 500 ? 26 : 13;
    return height * 4 * phase / 1024 * (1024 - phase) / 1024;
}

static void layout_center(void)
{
    struct rect s = wm_screen_rect(0);
    int base_total = 0;
    for (int i = 0; i < count; i++)
        base_total += (items[i].kind == DI_SEPARATOR ? SEPARATOR_W : base_size) + (i ? GAP : 0);
    int x0 = s.x + (s.w - base_total - 2 * PAD) / 2 + PAD;
    int big = max_size(), mag = grow.pos / 4;
    for (int i = 0, x = x0; i < count; i++) {
        int w = items[i].kind == DI_SEPARATOR ? SEPARATOR_W : base_size;
        int center = x + w / 2;
        int size = base_size;
        if (mag > 0 && mag_x >= 0 && items[i].kind != DI_SEPARATOR && big > base_size) {
            int d = (mag_x > center ? mag_x - center : center - mag_x) * 256 / (base_size * 5 / 2);
            if (d < 256)
                size = base_size + (big - base_size) * (65536 - d * d) / 65536 * CLAMP(mag, -64, 320) / 256;
        }
        items[i].size = MAX(size, base_size / 2);
        x += w + GAP;
    }
    int total = 0;
    for (int i = 0; i < count; i++)
        total += item_width(&items[i]) + (i ? GAP : 0);
    int x = s.x + (s.w - total - 2 * PAD) / 2;
    int h = base_size + 2 * PAD;
    int y = visible_now() ? s.y + s.h - BOTTOM - h : s.y + s.h + 2;
    dock_rect = (struct rect){x, y, total + 2 * PAD, h};
    x += PAD;
    for (int i = 0; i < count; i++) {
        items[i].x = x;
        int bottom = dock_rect.y + PAD + base_size - bounce_offset(&items[i]);
        items[i].r = (struct rect){x, bottom - items[i].size, items[i].size, items[i].size};
        x += item_width(&items[i]) + GAP;
    }
    int extra = big - base_size + TOOLTIP_H + 14;
    zone = (struct rect){dock_rect.x - 60, dock_rect.y - extra, dock_rect.w + 120, dock_rect.h + extra + BOTTOM};
}

static void layout_rail(void)
{
    struct rect s = wm_screen_rect(0);
    int width = lerp_i(RAIL_W0, RAIL_W1, CLAMP(grow.pos, -100, 1124));
    dock_rect = (struct rect){s.x + RAIL_X, s.y + 10, width, s.h - 20};
    int y = dock_rect.y + 10;
    int trash_y = dock_rect.y + dock_rect.h - RAIL_ROW - 6;
    for (int i = 0; i < count; i++) {
        struct item *it = &items[i];
        if (it->kind == DI_SEPARATOR) {
            it->r = (struct rect){dock_rect.x + 12, y + 4, width - 24, 1};
            y += 10;
            continue;
        }
        int ry = it->kind == DI_TRASH ? trash_y : y;
        int size = 34 + lift[i].pos * 10 / 1024;
        int bx = dock_rect.x + 15 + bounce_offset(it) / 2;
        it->size = size;
        it->r = (struct rect){bx + (34 - size) / 2, ry + (RAIL_ROW - size) / 2, size, size};
        if (it->kind != DI_TRASH)
            y += RAIL_ROW;
        if (y > trash_y - RAIL_ROW && i < count - 2) {
            for (int k = i + 1; k < count - 1; k++)
                items[k].r = (struct rect){0, 0, 0, 0};
            items[count - 1].size = 34;
            items[count - 1].r = (struct rect){dock_rect.x + 15, trash_y + (RAIL_ROW - 34) / 2, 34, 34};
            break;
        }
    }
    zone = (struct rect){dock_rect.x - 40, dock_rect.y - 40, RAIL_W1 + 100, dock_rect.h + 80};
}

static int corner_row_count(void)
{
    int n = 0;
    for (int i = 0; i < count && n < CORNER_ROW_MAX + 2; i++)
        if (items[i].kind != DI_SEPARATOR)
            n++;
    return n;
}

static int corner_cover(int x, int y, const void *ctx)
{
    const struct corner_shape *k = ctx;
    int l = k->x, r = k->x + k->w, t = k->y, b = k->bottom;
    int px = x * 16 + 8, py = y * 16 + 8;
    if (x >= l && x < r && y >= t && y < b) {
        int cx = r - k->radius, cy = t + k->radius;
        if (x >= cx && y < cy)
            return gfx_cover(k->radius * 16 - gfx_dist16(px - cx * 16, py - cy * 16));
        return gfx_cover(MIN(r * 16 - px, MIN(py - t * 16, (b - y) * 16)));
    }
    int f = k->fillet;
    if (x >= l && x < l + f && y >= t - f && y < t) {
        int d = gfx_dist16(px - (l + f) * 16, py - (t - f) * 16) - f * 16;
        return gfx_cover(d);
    }
    if (x >= r && x < r + f && y >= b - f && y < b) {
        int d = gfx_dist16(px - (r + f) * 16, py - (b - f) * 16) - f * 16;
        return gfx_cover(d);
    }
    return 0;
}

static bool corner_span(int y, const void *ctx, int *x0, int *x1)
{
    const struct corner_shape *k = ctx;
    if (y < k->y || y >= k->bottom - 1)
        return false;
    *x0 = k->x;
    *x1 = y >= k->y + k->radius ? k->x + k->w - 1 : k->x + k->w - k->radius;
    return true;
}

static void layout_corner(void)
{
    struct rect s = wm_screen_rect(0);
    int n = corner_row_count();
    int row_w = 2 * CORNER_PAD + n * CORNER_ICON + (n - 1) * CORNER_GAP;
    int rows = (grid_count + GRID_COLS - 1) / GRID_COLS;
    int grid_h = rows * GRID_CELL + 44;
    int g = CLAMP(grow.pos, 0, 1100);
    int ph = CORNER_ICON + 2 * CORNER_PAD + grid_h * g / 1024;
    int pw = MAX(row_w, lerp_i(row_w, GRID_COLS * GRID_CELL + 2 * CORNER_PAD, MIN(g, 1024)));
    corner = (struct corner_shape){s.x, s.y + s.h - ph, pw, ph, PANEL_RADIUS, FILLET, s.y + s.h};
    dock_rect = (struct rect){s.x, s.y + s.h - ph - FILLET, pw + FILLET, ph + FILLET};
    int x = s.x + CORNER_PAD, y = s.y + s.h - CORNER_PAD - CORNER_ICON, shown = 0;
    for (int i = 0; i < count; i++) {
        struct item *it = &items[i];
        if (it->kind == DI_SEPARATOR || shown >= n) {
            it->r = (struct rect){0, 0, 0, 0};
            continue;
        }
        int grow_px = lift[i].pos * 16 / 1024;
        it->size = CORNER_ICON + grow_px;
        it->r = (struct rect){x - grow_px / 2, y - grow_px - bounce_offset(it), it->size, it->size};
        x += CORNER_ICON + CORNER_GAP;
        shown++;
    }
    int gx = s.x + CORNER_PAD + (pw - 2 * CORNER_PAD - GRID_COLS * GRID_CELL) / 2;
    int gy = s.y + s.h - ph + 40;
    for (int i = 0; i < grid_count; i++)
        grid_r[i] = (struct rect){gx + (i % GRID_COLS) * GRID_CELL, gy + (i / GRID_COLS) * GRID_CELL, GRID_CELL, GRID_CELL};
    int full_h = CORNER_ICON + 2 * CORNER_PAD + grid_h + FILLET;
    zone = (struct rect){s.x, s.y + s.h - full_h - TOOLTIP_H - 30, GRID_COLS * GRID_CELL + 2 * CORNER_PAD + FILLET + 80,
                         full_h + TOOLTIP_H + 30};
}

static int arc_cover(int x, int y, const void *ctx)
{
    const struct arc_shape *a = ctx;
    if (x < a->cx || y > a->cy)
        return 0;
    int d = gfx_dist16((x - a->cx) * 16 + 8, (y - a->cy) * 16 + 8);
    int band = MIN(d - a->inner * 16, a->outer * 16 - d);
    int orb = a->orb * 16 - d;
    return MAX(gfx_cover(band), gfx_cover(orb));
}

static void layout_arc(void)
{
    struct rect s = wm_screen_rect(0);
    int g = CLAMP(grow.pos, -80, 1150);
    int R = lerp_i(ARC_R0, ARC_R1, g), half = lerp_i(32, 38, g);
    arc = (struct arc_shape){s.x, s.y + s.h, R - half, R + half, ORB_R + g * 6 / 1024};
    int outer = R + half;
    dock_rect = (struct rect){s.x, s.y + s.h - outer - 2, outer + 2, outer + 2};
    int n = count;
    for (int i = 0; i < n; i++) {
        struct item *it = &items[i];
        int deg = n > 1 ? 10 + 70 * i / (n - 1) : 45;
        int size = 38 + g * 6 / 1024 + lift[i].pos * 18 / 1024;
        int rr = R + bounce_offset(it) / 2;
        int cx = arc.cx + rr * cos_d(deg) / 1024, cy = arc.cy - rr * sin_d(deg) / 1024;
        it->size = size;
        it->r = (struct rect){cx - size / 2, cy - size / 2, size, size};
    }
    int reach = ARC_R1 + 60 + TOOLTIP_H + 40;
    zone = (struct rect){s.x, s.y + s.h - reach, reach + 60, reach};
}

static const char *item_label(const struct item *it);

static struct rect tip_rect(int i)
{
    const char *label = i >= 0 && i < count ? item_label(&items[i]) : NULL;
    if (!label)
        return (struct rect){0, 0, 0, 0};
    struct rect r = items[i].r;
    int w = gfx_text_width(&font_sans, label) + 22;
    bool beside = kind == DOCK_RAIL || kind == DOCK_ARC;
    struct rect tip = beside ? (struct rect){r.x + r.w + 12, r.y + r.h / 2 - 12, w, 24}
                             : (struct rect){r.x + r.w / 2 - w / 2, r.y - TOOLTIP_H, w, 24};
    return rect_inset(tip, -14);
}

static struct rect item_extent(int i)
{
    return rect_empty(items[i].r) ? items[i].r : rect_inset(items[i].r, -26);
}

/* Redraw only what moved: the panel when its shape changes, otherwise the icons and tooltip that did. */
static void damage_changes(struct rect old_zone)
{
    static struct rect body_was, items_was[MAX_ITEMS], tip_was;
    static int count_was = -1, grid_hover_was = -1, hl_was, hl_alpha_was;
    struct rect body = rect_inset(dock_rect, -36);
    struct rect tip = tip_rect(hover_item);
    bool full = count != count_was || memcmp(&body, &body_was, sizeof(body)) || kind == DOCK_CENTER;
    if (!full && kind == DOCK_RAIL && (hl.pos != hl_was || hl_alpha.pos != hl_alpha_was))
        full = true;
    if (full) {
        struct rect r = rect_union(body, body_was);
        for (int i = 0; i < count; i++)
            r = rect_union(r, item_extent(i));
        for (int i = 0; i < count_was; i++)
            r = rect_union(r, items_was[i]);
        r = rect_union(r, rect_union(tip, tip_was));
        if (kind == DOCK_CENTER || count_was < 0)
            r = rect_union(r, rect_union(old_zone, zone));
        wm_damage_z(r, Z_DOCK);
    } else {
        for (int i = 0; i < count; i++) {
            struct rect now = item_extent(i);
            if (memcmp(&now, &items_was[i], sizeof(now)))
                wm_damage_z(rect_union(now, items_was[i]), Z_DOCK);
        }
        if (memcmp(&tip, &tip_was, sizeof(tip))) {
            wm_damage_z(tip_was, Z_DOCK);
            wm_damage_z(tip, Z_DOCK);
        }
        if (grid_hover != grid_hover_was) {
            if (grid_hover_was >= 0 && grid_hover_was < grid_count)
                wm_damage_z(grid_r[grid_hover_was], Z_DOCK);
            if (grid_hover >= 0 && grid_hover < grid_count)
                wm_damage_z(grid_r[grid_hover], Z_DOCK);
        }
    }
    body_was = body;
    for (int i = 0; i < count; i++)
        items_was[i] = item_extent(i);
    count_was = count;
    tip_was = tip;
    grid_hover_was = grid_hover;
    hl_was = hl.pos;
    hl_alpha_was = hl_alpha.pos;
}

static void layout(void)
{
    struct rect old = zone;
    switch (kind) {
    case DOCK_RAIL: layout_rail(); break;
    case DOCK_CORNER: layout_corner(); break;
    case DOCK_ARC: layout_arc(); break;
    default: layout_center(); break;
    }
    glass_set_frame(&glass, dock_rect);
    damage_changes(old);
}

static bool trash_full(void)
{
    struct vdir d;
    bool full = false;
    if (vfs_opendir(TRASH_DIR, &d) == 0) {
        struct vfs_dirent ent;
        while (!full && vfs_readdir(&d, &ent) == 0)
            full = ent.name[0] != '.';
        vfs_closedir(&d);
    }
    return full;
}

static enum icon_id item_icon(const struct item *it)
{
    switch (it->kind) {
    case DI_APP: return it->app->icon;
    case DI_LAUNCHPAD: return ICON_LAUNCHPAD;
    case DI_TRASH: return trash_full() ? ICON_TRASH_FULL : ICON_TRASH;
    case DI_SYSTEM: return ICON_LOGO;
    default: return ICON_APP;
    }
}

static const char *item_label(const struct item *it)
{
    switch (it->kind) {
    case DI_APP: return it->app->name;
    case DI_LAUNCHPAD: return kind == DOCK_CENTER ? "Launchpad" : "Apps";
    case DI_TRASH: return "Trash";
    case DI_WINDOW: return it->win->title;
    case DI_SYSTEM: return "SynthOS";
    default: return NULL;
    }
}

struct rect dock_window_rect(struct window *w)
{
    if (!started)
        return (struct rect){0, 0, 0, 0};
    for (int i = 0; i < count; i++)
        if (items[i].kind == DI_WINDOW && items[i].win == w)
            return items[i].r;
    return (struct rect){0, 0, 0, 0};
}

void dock_bounce(const struct gui_app *app)
{
    bounce_app = app;
    bounce_start = hal_ticks_ms();
}

bool dock_animate(void)
{
    if (!started)
        return false;
    uint64_t now = hal_ticks_ms();
    int dt = last_tick ? (int)MIN(now - last_tick, 64u) : 16;
    last_tick = now;
    bool busy = false;
    int k = kind == DOCK_CENTER ? 420 : 260, d = kind == DOCK_CENTER ? 34 : 24;
    busy |= spring_step(&grow, k, d, dt);
    busy |= spring_step(&hl, 520, 40, dt);
    busy |= spring_step(&hl_alpha, 400, 40, dt);
    for (int i = 0; i < count; i++) {
        lift[i].target = i == hover_item && kind != DOCK_CENTER ? 1024 : 0;
        busy |= spring_step(&lift[i], 600, 26, dt);
    }
    if (bounce_app) {
        if (now - bounce_start >= 1000)
            bounce_app = NULL;
        busy = true;
    }
    if (busy)
        layout();
    return busy;
}

static void draw_icon(struct canvas *c, const struct item *it, struct rect r, int alpha)
{
    if (it->kind == DI_WINDOW) {
        struct canvas *t = &it->win->thumb;
        int tw = r.w, th = r.w * t->h / MAX(t->w, 1);
        if (th > r.h) {
            th = r.h;
            tw = r.h * t->w / MAX(t->h, 1);
        }
        struct rect dst = {r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2, tw, th};
        gfx_drop_shadow(c, dst, 4, 6, 70, 2);
        gfx_scale(c, dst, t, alpha);
        gfx_icon(c, r.x + r.w - 18, r.y + r.h - 18, it->win->icon, 18);
    } else if (it->kind == DI_APP && it->app->picture) {
        gfx_scale(c, (struct rect){r.x + r.w / 16, r.y + r.w / 16, r.w - r.w / 8, r.h - r.w / 8}, it->app->picture, alpha);
    } else {
        gfx_icon_alpha(c, r.x, r.y, item_icon(it), r.w, alpha);
    }
}

static uint32_t ink(void)
{
    return style.dark ? RGB(244, 242, 248) : color_mix(RGB(30, 26, 24), style.tint | 0xff000000u, 60);
}

static void tooltip(struct canvas *c, struct rect r, const char *label, bool beside)
{
    int w = gfx_text_width(&font_sans, label) + 22;
    struct rect tip = beside ? (struct rect){r.x + r.w + 12, r.y + r.h / 2 - 12, w, 24}
                             : (struct rect){r.x + r.w / 2 - w / 2, r.y - TOOLTIP_H, w, 24};
    gfx_drop_shadow(c, tip, 12, 10, 50, 3);
    gfx_round(c, tip, 12, style.dark ? RGBA(46, 44, 54, 235) : RGBA(252, 252, 254, 235));
    gfx_round_frame_gradient(c, tip, 12, style.dark ? ALPHA(0xffffffffu, 60) : ALPHA(0xffffffffu, 255),
                             style.dark ? ALPHA(0xffffffffu, 16) : ALPHA(0, 26));
    gfx_text_box(c, tip, &font_sans, label, style.text, ALIGN_CENTER);
}

static void running_dot(struct canvas *c, struct rect r)
{
    uint32_t col = style.dark ? RGB(240, 240, 246) : RGB(40, 34, 30);
    switch (kind) {
    case DOCK_RAIL:
        gfx_round(c, (struct rect){dock_rect.x + 4, r.y + r.h / 2 - 7, 3, 14}, 1, style.accent);
        break;
    case DOCK_ARC: {
        int cx = r.x + r.w / 2 - arc.cx, cy = arc.cy - (r.y + r.h / 2);
        int len = MAX(gfx_dist16(cx * 16, cy * 16) / 16, 1);
        int off = r.w / 2 + 6;
        gfx_circle(c, r.x + r.w / 2 + cx * off / len, r.y + r.h / 2 - cy * off / len, 2, col);
        break;
    }
    default:
        gfx_circle(c, r.x + r.w / 2, (kind == DOCK_CENTER ? dock_rect.y + dock_rect.h : r.y + r.h + 9) - 4, 2, col);
        break;
    }
}

static void paint_items(struct canvas *c)
{
    for (int i = 0; i < count; i++) {
        struct item *it = &items[i];
        if (it->kind == DI_SEPARATOR) {
            if (kind == DOCK_CENTER)
                gfx_fill(c, (struct rect){it->x + SEPARATOR_W / 2, dock_rect.y + 10, 1, dock_rect.h - 20},
                         style.dark ? ALPHA(0xffffffffu, 60) : ALPHA(0, 50));
            else if (kind == DOCK_RAIL && it->r.w)
                gfx_fill(c, it->r, style.dark ? ALPHA(0xffffffffu, 40) : ALPHA(0, 34));
            continue;
        }
        struct rect r = it->r;
        if (rect_empty(r) || rect_empty(rect_intersect(rect_inset(r, -12), c->clip)))
            continue;
        int alpha = pressed_item == i ? 160 : 255;
        if (kind == DOCK_ARC || kind == DOCK_CORNER) {
            int glow = lift[i].pos * 90 / 1024;
            if (glow > 0)
                gfx_glow(c, r, r.w / 3, 14, glow, style.accent);
        }
        draw_icon(c, it, r, alpha);
        if (it->running)
            running_dot(c, r);
        if (kind == DOCK_RAIL) {
            int a = CLAMP((grow.pos - 300) * 255 / 600, 0, 255);
            const char *label = item_label(it);
            if (a > 0 && label) {
                struct rect lr = {dock_rect.x + 62 + (255 - a) / 16, r.y + r.h / 2 - 9, dock_rect.w - 72, 18};
                struct canvas clip = *c;
                clip.clip = rect_intersect(c->clip, rect_inset(dock_rect, 2));
                gfx_text_box(&clip, lr, i == hover_item ? &font_bold : &font_sans, label, ALPHA(ink(), a), ALIGN_LEFT);
            }
        }
    }
}

static void paint_center(struct canvas *c)
{
    struct glass_look look;
    int radius = dock_rect.h * 2 / 5;
    glass_look_for(&look, GLASS_DOCK, radius);
    if (style.glass == GLASS_LIQUID)
        gfx_drop_shadow(c, dock_rect, radius, 16, style.dark ? 90 : 40, 5);
    glass_draw(c, &glass, &look);
    paint_items(c);
    if (hover_item >= 0 && hover_item < count && pressed_item < 0) {
        const char *label = item_label(&items[hover_item]);
        if (label)
            tooltip(c, items[hover_item].r, label, false);
    }
}

static void paint_rail(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, PANEL_RADIUS);
    gfx_drop_shadow(c, dock_rect, PANEL_RADIUS, 22, style.dark ? 80 : 30, 6);
    glass_draw(c, &glass, &look);
    gfx_round_frame(c, dock_rect, PANEL_RADIUS, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 90));
    if (hl_alpha.pos > 8) {
        struct rect pill = {dock_rect.x + 8, hl.pos, dock_rect.w - 16, RAIL_ROW - 6};
        gfx_round(c, pill, 14, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, CLAMP(hl_alpha.pos, 0, 1024) * 24 / 1024));
    }
    paint_items(c);
    if (hover_item >= 0 && grow.pos < 300 && pressed_item < 0) {
        const char *label = item_label(&items[hover_item]);
        if (label)
            tooltip(c, items[hover_item].r, label, true);
    }
}

static void paint_corner(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, 0);
    look.shape = corner_cover;
    look.shape_span = corner_span;
    look.shape_ctx = &corner;
    look.shape_key = corner.w * 4096 + corner.h;
    glass_draw(c, &glass, &look);
    int g = CLAMP(grow.pos, 0, 1024);
    if (g > 40) {
        int a = CLAMP((g - 200) * 255 / 700, 0, 255);
        struct rect head = {corner.x + CORNER_PAD + 4, corner.y + 12, 200, 20};
        gfx_text_box(c, head, &font_bold, "All apps", ALPHA(ink(), a), ALIGN_LEFT);
        for (int i = 0; i < grid_count; i++) {
            struct rect cell = grid_r[i];
            int stagger = CLAMP((g - 250 - i * 22) * 255 / 400, 0, 255);
            if (!stagger || cell.y < corner.y + 30)
                continue;
            int size = 40 + stagger * 4 / 255;
            int dy = (255 - stagger) / 12;
            if (i == grid_hover)
                gfx_round(c, rect_inset(cell, 4), 16, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 22 * stagger / 255));
            struct rect ir = {cell.x + (cell.w - size) / 2, cell.y + 10 + dy, size, size};
            if (grid[i]->picture)
                gfx_scale(c, ir, grid[i]->picture, stagger);
            else
                gfx_icon_alpha(c, ir.x, ir.y, grid[i]->icon, size, grid_pressed == i ? stagger * 2 / 3 : stagger);
            gfx_text_box(c, (struct rect){cell.x + 2, cell.y + 58 + dy, cell.w - 4, 16}, &font_small, grid[i]->name,
                         ALPHA(ink(), stagger), ALIGN_CENTER);
        }
        gfx_fill(c, (struct rect){corner.x + CORNER_PAD, corner.y + corner.h - CORNER_ICON - 2 * CORNER_PAD + 4,
                                  corner.w - 2 * CORNER_PAD, 1},
                 ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 30 * a / 255));
    }
    paint_items(c);
    if (hover_item >= 0 && pressed_item < 0 && g < 200) {
        const char *label = item_label(&items[hover_item]);
        if (label)
            tooltip(c, items[hover_item].r, label, false);
    }
}

static void paint_arc(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, 0);
    look.shape = arc_cover;
    look.shape_ctx = &arc;
    look.shape_key = arc.inner * 65536 + arc.outer * 256 + arc.orb;
    int glow = 60 + CLAMP(grow.pos, 0, 1024) * 60 / 1024;
    gfx_glow(c, (struct rect){arc.cx - arc.orb, arc.cy - arc.orb, arc.orb * 2, arc.orb * 2}, arc.orb, 30, glow, style.accent);
    glass_draw(c, &glass, &look);
    int cx = arc.cx + arc.orb * 42 / 100, cy = arc.cy - arc.orb * 42 / 100;
    gfx_circle(c, cx, cy, 22, ALPHA(style.accent, 230));
    gfx_symbol(c, cx - 8, cy - 8, SYM_GRID, 16, RGB(255, 255, 255));
    paint_items(c);
    if (hover_item >= 0 && pressed_item < 0) {
        const char *label = item_label(&items[hover_item]);
        if (label)
            tooltip(c, items[hover_item].r, label, true);
    }
}

void dock_paint(struct canvas *c, int screen)
{
    if (!started || screen != 0 || rect_empty(rect_intersect(zone, c->clip)))
        return;
    switch (kind) {
    case DOCK_RAIL: paint_rail(c); break;
    case DOCK_CORNER: paint_corner(c); break;
    case DOCK_ARC: paint_arc(c); break;
    default: paint_center(c); break;
    }
}

static int item_at(int x, int y)
{
    for (int i = 0; i < count; i++) {
        if (items[i].kind == DI_SEPARATOR || rect_empty(items[i].r))
            continue;
        struct rect r = items[i].r;
        if (kind == DOCK_CENTER)
            r = (struct rect){r.x - GAP / 2, MIN(r.y, dock_rect.y), r.w + GAP,
                              dock_rect.y + dock_rect.h - MIN(r.y, dock_rect.y)};
        else if (kind == DOCK_RAIL)
            r = (struct rect){dock_rect.x, r.y - (RAIL_ROW - r.h) / 2, dock_rect.w, RAIL_ROW};
        else
            r = rect_inset(r, -CORNER_GAP / 2);
        if (rect_contains(r, x, y))
            return i;
    }
    return -1;
}

static int grid_at(int x, int y)
{
    if (kind != DOCK_CORNER || grow.pos < 600)
        return -1;
    for (int i = 0; i < grid_count; i++)
        if (rect_contains(grid_r[i], x, y) && grid_r[i].y > corner.y + 30)
            return i;
    return -1;
}

static bool in_orb(int x, int y)
{
    if (kind != DOCK_ARC)
        return false;
    int dx = x - arc.cx, dy = arc.cy - y;
    return dx >= 0 && dy >= 0 && dx * dx + dy * dy <= arc.orb * arc.orb;
}

static bool inside(int x, int y)
{
    switch (kind) {
    case DOCK_CORNER: return corner_cover(x, y, &corner) > 0;
    case DOCK_ARC: return arc_cover(x, y, &arc) > 0;
    default: return rect_contains(dock_rect, x, y);
    }
}

bool dock_hit(int x, int y)
{
    if (!started)
        return false;
    if (!visible_now()) {
        struct rect s = wm_screen_rect(0);
        return autohide && y >= s.y + s.h - 2 && wm_screen_at(x, y) == 0;
    }
    return inside(x, y) || item_at(x, y) >= 0;
}

static void empty_trash(void)
{
    struct vdir d;
    if (vfs_opendir(TRASH_DIR, &d) != 0)
        return;
    char names[64][NAME_MAX_LEN];
    int n = 0;
    struct vfs_dirent ent;
    while (n < 64 && vfs_readdir(&d, &ent) == 0)
        if (strcmp(ent.name, ".") && strcmp(ent.name, ".."))
            str_copy(names[n++], ent.name, NAME_MAX_LEN);
    vfs_closedir(&d);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX_LEN];
        path_join(TRASH_DIR, names[i], path, sizeof(path));
        if (vfs_is_dir(path)) {
            char cmd[PATH_MAX_LEN + 16];
            ksnprintf(cmd, sizeof(cmd), "rm -r '%s'", path);
            gui_terminal_run(cmd);
        } else {
            vfs_unlink(path);
        }
    }
    dock_changed();
}

static int menu_item;

static void pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (menu_item < 0 || menu_item >= count)
        return;
    struct item *it = &items[menu_item];
    if (it->kind == DI_TRASH) {
        if (index == 0)
            gui_open_file(TRASH_DIR);
        else
            empty_trash();
        return;
    }
    if (it->kind != DI_APP)
        return;
    if (index == 0)
        gui_app_launch(it->app, NULL);
    else if (index == 1)
        wm_focus_app(it->app);
    else if (index == 3)
        for (struct window *w = wm_app_window(it->app); w; w = wm_app_window(it->app)) {
            wm_close(w);
            if (wm_window_alive(w))
                break;
        }
}

static void activate(int i)
{
    struct item *it = &items[i];
    switch (it->kind) {
    case DI_APP:
        if (wm_app_window_count(it->app) > 0) {
            wm_focus_app(it->app);
        } else {
            dock_bounce(it->app);
            gui_app_launch(it->app, NULL);
        }
        break;
    case DI_LAUNCHPAD: launchpad_toggle(); break;
    case DI_SYSTEM: shell_system_menu(it->r.x + it->r.w + 8, it->r.y); break;
    case DI_WINDOW:
        if (wm_window_alive(it->win))
            wm_show(it->win);
        break;
    case DI_TRASH:
        vfs_mkdirs(TRASH_DIR);
        gui_open_file(TRASH_DIR);
        break;
    default: break;
    }
}

static void context_menu(int i, int x, int y)
{
    menu_item = i;
    int above = kind == DOCK_RAIL ? 0 : 1;
    int mx = kind == DOCK_RAIL ? items[i].r.x + items[i].r.w + 12 : x;
    if (items[i].kind == DI_TRASH) {
        static const struct menu_item trash_items[] = {{"Open", 0, NULL}, {"Empty Trash", 0, NULL}};
        ui_menu_items(NULL, mx, above ? y - 80 : y, trash_items, 2, pick);
    } else if (items[i].kind == DI_APP) {
        bool running = wm_app_window_count(items[i].app) > 0;
        static const struct menu_item app_items[] = {
            {"New Window", 0, NULL}, {"Show All Windows", 0, NULL}, {"", 0, NULL}, {"Quit", 0, NULL}};
        static const struct menu_item open_items[] = {{"Open", 0, NULL}};
        if (running)
            ui_menu_items(NULL, mx, above ? y - 120 : y, app_items, 4, pick);
        else
            ui_menu_items(NULL, mx, above ? y - 44 : y, open_items, 1, pick);
    }
}

void dock_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    UNUSED(clicks);
    int i = item_at(x, y), g = grid_at(x, y);
    if (down) {
        pressed_item = button == BUTTON_LEFT ? i : -1;
        grid_pressed = button == BUTTON_LEFT ? g : -1;
        wm_damage_z(zone, Z_DOCK);
        if (button == BUTTON_RIGHT && i >= 0)
            context_menu(i, x, y);
        return;
    }
    int pressed = pressed_item, gp = grid_pressed;
    pressed_item = grid_pressed = -1;
    wm_damage_z(zone, Z_DOCK);
    if (pressed >= 0 && pressed == i) {
        activate(i);
    } else if (gp >= 0 && gp == g) {
        const struct gui_app *app = grid[g];
        if (wm_app_window_count(app) > 0)
            wm_focus_app(app);
        else
            gui_app_launch(app, NULL);
        grow.target = 0;
    } else if (i < 0 && g < 0 && in_orb(x, y)) {
        launcher_toggle(arc.cx + arc.orb + 20, arc.cy - arc.orb - 20);
    }
}

void dock_hover(int x, int y)
{
    if (!started)
        return;
    struct rect s = wm_screen_rect(0);
    bool now_over = y >= 0 && wm_screen_at(x, y) == 0 && (inside(x, y) || item_at(x, y) >= 0);
    if (kind == DOCK_CENTER)
        now_over = y >= 0 && wm_screen_at(x, y) == 0 && rect_contains(zone, x, y) &&
                   y >= dock_rect.y - (max_size() - base_size);
    if (autohide && kind == DOCK_CENTER) {
        bool want = revealed ? (y >= 0 && y >= dock_rect.y - (max_size() - base_size) - 10) : (y >= s.y + s.h - 2);
        if (want != revealed) {
            revealed = want;
            layout();
        }
    }
    int item = now_over ? item_at(x, y) : -1;
    int gi = now_over ? grid_at(x, y) : -1;
    if (now_over != over) {
        over = now_over;
        grow.target = over ? 1024 : 0;
    }
    if (kind == DOCK_RAIL && item >= 0) {
        int ty = items[item].r.y - (RAIL_ROW - items[item].r.h) / 2 + 3;
        if (hl_alpha.target == 0)
            spring_set(&hl, ty);
        hl.target = ty;
        hl_alpha.target = 1024;
    } else {
        hl_alpha.target = 0;
    }
    int nx = now_over ? x : -1;
    if (nx == hover_x && item == hover_item && gi == grid_hover)
        return;
    hover_x = nx;
    hover_item = item;
    grid_hover = gi;
    if (nx >= 0)
        mag_x = nx;
    layout();
}

void dock_changed(void)
{
    if (!started)
        return;
    build();
    if (hover_item >= count)
        hover_item = -1;
    layout();
}

void dock_start(void)
{
    glass = (struct glass){0};
    glass.z = Z_DOCK;
    kind = look_current()->dock;
    hover_x = hover_item = pressed_item = grid_hover = grid_pressed = -1;
    spring_set(&grow, kind == DOCK_CORNER || kind == DOCK_ARC ? -900 : 0);
    grow.target = 0;
    spring_set(&hl, 0);
    spring_set(&hl_alpha, 0);
    for (int i = 0; i < MAX_ITEMS; i++)
        spring_set(&lift[i], 0);
    over = false;
    last_tick = 0;
    mag_x = -1;
    bounce_app = NULL;
    revealed = false;
    started = true;
    build();
    layout();
    glass.frame = dock_rect;
    glass_register(&glass);
}

void dock_stop(void)
{
    if (!started)
        return;
    started = false;
    glass_unregister(&glass);
    wm_damage_z(zone, Z_DOCK);
}

static void get_size(char *buf, size_t cap)
{
    str_copy(buf, base_size <= 40 ? "small" : base_size >= 64 ? "large" : "medium", cap);
}

static int set_size(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (!strcmp(value, "small"))
        base_size = 40;
    else if (!strcmp(value, "medium"))
        base_size = 52;
    else if (!strcmp(value, "large"))
        base_size = 64;
    else
        return E_INVAL;
    dock_changed();
    if (gui_active())
        wm_damage_all();
    return 0;
}

static const struct setting s_docksize = {"docksize", "small|medium|large", "size of the dock icons", get_size,
                                          set_size};
SETTING(s_docksize);

static void get_magnify(char *buf, size_t cap)
{
    str_copy(buf, magnify ? "on" : "off", cap);
}

static int set_magnify(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    magnify = !strcmp(value, "on");
    dock_changed();
    return 0;
}

static const struct setting s_magnify = {"magnify", "on|off", "enlarge dock icons under the pointer", get_magnify,
                                         set_magnify};
SETTING(s_magnify);

static void get_hide(char *buf, size_t cap)
{
    str_copy(buf, autohide ? "on" : "off", cap);
}

static int set_hide(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    autohide = !strcmp(value, "on");
    revealed = false;
    dock_changed();
    if (gui_active())
        wm_damage_all();
    return 0;
}

static const struct setting s_dockhide = {"dockhide", "on|off", "hide the dock until the pointer reaches the bottom",
                                          get_hide, set_hide};
SETTING(s_dockhide);
