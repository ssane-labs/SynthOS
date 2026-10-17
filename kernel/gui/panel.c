#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

#define PILL_H 34
#define PILL_ICON 34
#define PLACE_ROW 36
#define MAX_PLACES 12
#define CORNER_R0 116
#define CORNER_R1 140

enum pill_part {
    PART_SEARCH,
    PART_WIFI,
    PART_VOLUME,
    PART_CONTROL,
    PART_CLOCK,
    PART_COUNT,
};

struct place {
    char label[40];
    char path[PATH_MAX_LEN];
    enum symbol_id sym;
};

struct bubble {
    int cx, cy, r;
};

static bool started;
static enum status_kind status;
static bool places_on;
static struct glass pill_glass, places_glass;
static struct rect pill, part_r[PART_COUNT], places_r, zone_pill, zone_places;
static struct bubble bubble;
static struct place places[MAX_PLACES];
static int place_count, place_hover = -1, part_hover = -1, place_pressed = -1;
static struct spring pill_grow, place_hl, place_alpha, bubble_grow, intro;
static uint64_t last_tick;
static struct window *launcher;

static const char *const day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const month_names[12] = {"January", "February", "March",     "April",   "May",      "June",
                                            "July",    "August",   "September", "October", "November", "December"};

static uint32_t ink(void)
{
    return style.dark ? RGB(246, 244, 250) : color_mix(RGB(30, 26, 24), style.tint | 0xff000000u, 60);
}

static uint32_t ink_soft(void)
{
    return ALPHA(ink(), 150);
}

int panel_weekday(int y, int m, int d)
{
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

const char *panel_day_name(int i)
{
    return day_names[CLAMP(i, 0, 6)];
}

const char *panel_month_name(int i)
{
    return month_names[CLAMP(i, 0, 11)];
}

static void add_place(const char *label, const char *path, enum symbol_id sym)
{
    if (place_count >= MAX_PLACES)
        return;
    struct place *p = &places[place_count++];
    str_copy(p->label, label, sizeof(p->label));
    str_copy(p->path, path, sizeof(p->path));
    p->sym = sym;
}

static void home_place(const char *label, const char *dir, enum symbol_id sym)
{
    char path[PATH_MAX_LEN];
    path_join(user_home(), dir, path, sizeof(path));
    add_place(label, path, sym);
}

static void build_places(void)
{
    place_count = 0;
    add_place("Home", user_home(), SYM_HOME);
    home_place("Desktop", "Desktop", SYM_DESKTOP);
    home_place("Documents", "Documents", SYM_DOC);
    home_place("Downloads", "Downloads", SYM_DOWNLOAD);
    home_place("Pictures", "Pictures", SYM_PHOTO);
    home_place("Music", "Music", SYM_MUSIC);
    home_place("Videos", "Videos", SYM_VIDEO);
    add_place("Trash", "/home/.Trash", SYM_TRASH);
    struct volume vols[4];
    int n = vfs_volumes(vols, 4);
    for (int i = 0; i < n; i++)
        add_place(vols[i].label, vols[i].path, SYM_USB);
}

int status_bottom(void)
{
    struct rect s = wm_screen_rect(0);
    switch (look_current()->status) {
    case STATUS_PILL: return s.y + 12 + PILL_H;
    case STATUS_CORNER: return s.y + CORNER_R0 + 8;
    default: return s.y + menubar_height();
    }
}

static int bubble_cover(int x, int y, const void *ctx)
{
    const struct bubble *b = ctx;
    return gfx_cover(b->r * 16 - gfx_dist16((x - b->cx) * 16 + 8, (y - b->cy) * 16 + 8));
}

static void clock_text(char *buf, size_t cap)
{
    struct rtc_time t;
    hal_rtc(&t);
    ksnprintf(buf, cap, "%02u:%02u", t.hour, t.minute);
}

static void layout(void)
{
    struct rect s = wm_screen_rect(0);
    struct rect old_pill = zone_pill, old_places = zone_places;
    int dy = (1024 - CLAMP(intro.pos, 0, 1024)) * 60 / 1024;
    if (status == STATUS_PILL) {
        char clock[16];
        clock_text(clock, sizeof(clock));
        int g = CLAMP(pill_grow.pos, -100, 1124);
        int cw = gfx_text_width(&font_bold, clock) + 22;
        int w = 4 * PILL_ICON + cw + 10 + g * 8 / 1024;
        int h = PILL_H + g * 4 / 1024;
        pill = (struct rect){s.x + s.w - w - 12, s.y + 12 - dy - g * 2 / 1024, w, h};
        int x = pill.x + 5;
        for (int i = 0; i < PART_CONTROL + 1; i++, x += PILL_ICON)
            part_r[i] = (struct rect){x, pill.y, PILL_ICON, pill.h};
        part_r[PART_CLOCK] = (struct rect){x, pill.y, cw, pill.h};
        zone_pill = rect_inset(pill, -30);
        glass_set_frame(&pill_glass, pill);
    } else if (status == STATUS_CORNER) {
        int r = lerp_i(CORNER_R0, CORNER_R1, CLAMP(bubble_grow.pos, -100, 1124)) - dy * 2;
        bubble = (struct bubble){s.x + s.w, s.y, MAX(r, 4)};
        pill = (struct rect){s.x + s.w - bubble.r, s.y, bubble.r, bubble.r};
        int bx = s.x + s.w - bubble.r * 70 / 100;
        part_r[PART_CLOCK] = (struct rect){bx - 10, s.y + bubble.r * 18 / 100, bubble.r * 66 / 100, 34};
        part_r[PART_SEARCH] = (struct rect){bx - 6, s.y + bubble.r * 52 / 100, 28, 28};
        part_r[PART_WIFI] = (struct rect){bx + 24, s.y + bubble.r * 52 / 100, 28, 28};
        part_r[PART_VOLUME] = (struct rect){0, 0, 0, 0};
        part_r[PART_CONTROL] = (struct rect){bx + 54, s.y + bubble.r * 52 / 100, 28, 28};
        zone_pill = (struct rect){s.x + s.w - CORNER_R1 - 70, s.y, CORNER_R1 + 70, CORNER_R1 + 70};
        glass_set_frame(&pill_glass, pill);
    }
    if (places_on) {
        int bottom = s.y + s.h - (44 + 2 * 15) - 22 - 16;
        places_r = (struct rect){s.x + 10 - dy, s.y + 12, PLACES_W, MIN(bottom - s.y - 12, 50 + place_count * PLACE_ROW + 6)};
        zone_places = rect_inset(places_r, -40);
        glass_set_frame(&places_glass, places_r);
    }
    wm_damage_z(rect_union(old_pill, zone_pill), Z_MENUBAR);
    if (places_on)
        wm_damage_z(rect_union(old_places, zone_places), Z_MENUBAR);
}

static struct rect place_row(int i)
{
    return (struct rect){places_r.x + 8, places_r.y + 46 + i * PLACE_ROW, places_r.w - 16, PLACE_ROW - 4};
}

bool panel_animate(void)
{
    if (!started)
        return false;
    uint64_t now = hal_ticks_ms();
    int dt = last_tick ? (int)MIN(now - last_tick, 64u) : 16;
    last_tick = now;
    bool busy = spring_step(&pill_grow, 420, 26, dt);
    busy |= spring_step(&bubble_grow, 300, 20, dt);
    busy |= spring_step(&intro, 160, 20, dt);
    bool hl = spring_step(&place_hl, 520, 38, dt);
    hl |= spring_step(&place_alpha, 400, 40, dt);
    if (busy)
        layout();
    if (hl && places_on)
        wm_damage_z(zone_places, Z_MENUBAR);
    return busy || hl;
}

static void paint_part_icon(struct canvas *c, int i, struct rect r)
{
    enum symbol_id sym = i == PART_SEARCH ? SYM_SEARCH : i == PART_WIFI ? SYM_WIFI : i == PART_VOLUME ? SYM_VOLUME : SYM_CONTROL;
    bool online = net_interface_count() > 0 && net_interface(0)->configured;
    uint32_t col = i == PART_WIFI && !online ? ink_soft() : ink();
    if (i == part_hover)
        gfx_circle(c, r.x + r.w / 2, r.y + r.h / 2, MIN(r.w, r.h) / 2 - 3, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 26));
    gfx_symbol(c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, sym, 16, col);
}

static void paint_pill(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, pill.h / 2);
    gfx_drop_shadow(c, pill, pill.h / 2, 16, style.dark ? 70 : 26, 4);
    glass_draw(c, &pill_glass, &look);
    gfx_round_frame(c, pill, pill.h / 2, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 90));
    for (int i = 0; i < PART_CLOCK; i++)
        paint_part_icon(c, i, part_r[i]);
    char clock[16];
    clock_text(clock, sizeof(clock));
    if (part_hover == PART_CLOCK)
        gfx_round(c, rect_inset(part_r[PART_CLOCK], 4), (pill.h - 8) / 2, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 22));
    gfx_text_box(c, part_r[PART_CLOCK], &font_bold, clock, ink(), ALIGN_CENTER);
}

static void paint_bubble(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, 0);
    look.shape = bubble_cover;
    look.shape_ctx = &bubble;
    look.shape_key = bubble.r;
    gfx_glow(c, (struct rect){bubble.cx - bubble.r, bubble.cy - bubble.r, bubble.r * 2, bubble.r * 2}, bubble.r, 26,
             50 + CLAMP(bubble_grow.pos, 0, 1024) * 50 / 1024, style.accent);
    glass_draw(c, &pill_glass, &look);
    char clock[16];
    clock_text(clock, sizeof(clock));
    struct rtc_time t;
    hal_rtc(&t);
    struct rect cr = part_r[PART_CLOCK];
    gfx_text_box(c, cr, &font_title, clock, ink(), ALIGN_LEFT);
    char date[32];
    int m = CLAMP((int)t.month, 1, 12);
    ksnprintf(date, sizeof(date), "%.3s, %u %.3s", day_names[panel_weekday((int)t.year, m, (int)t.day)], t.day,
              month_names[m - 1]);
    gfx_text_box(c, (struct rect){cr.x, cr.y + 30, cr.w + 40, 16}, &font_small, date, ink_soft(), ALIGN_LEFT);
    for (int i = 0; i < PART_CLOCK; i++)
        if (!rect_empty(part_r[i]))
            paint_part_icon(c, i, part_r[i]);
}

static void paint_places(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, PANEL_RADIUS);
    gfx_drop_shadow(c, places_r, PANEL_RADIUS, 22, style.dark ? 70 : 26, 6);
    glass_draw(c, &places_glass, &look);
    gfx_round_frame(c, places_r, PANEL_RADIUS, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 90));
    gfx_text_box(c, (struct rect){places_r.x + 18, places_r.y + 14, places_r.w - 30, 20}, &font_bold, "Places", ink_soft(),
                 ALIGN_LEFT);
    if (place_alpha.pos > 8) {
        struct rect hr = {places_r.x + 8, place_hl.pos, places_r.w - 16, PLACE_ROW - 4};
        gfx_round(c, hr, 14, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, CLAMP(place_alpha.pos, 0, 1024) * 22 / 1024));
    }
    struct canvas clip = *c;
    clip.clip = rect_intersect(c->clip, places_r);
    for (int i = 0; i < place_count; i++) {
        struct rect r = place_row(i);
        if (r.y + r.h > places_r.y + places_r.h - 6)
            break;
        int push = i == place_hover ? 6 : 0;
        uint32_t col = i == place_pressed ? style.accent : ink();
        gfx_symbol(&clip, r.x + 10 + push, r.y + (r.h - 16) / 2, places[i].sym, 16, i == place_hover ? style.accent : ink_soft());
        gfx_text_box(&clip, (struct rect){r.x + 38 + push, r.y, r.w - 44, r.h}, i == place_hover ? &font_bold : &font_sans,
                     places[i].label, col, ALIGN_LEFT);
    }
}

void panel_paint(struct canvas *c, int screen)
{
    if (!started || screen != 0)
        return;
    if (places_on && !rect_empty(rect_intersect(zone_places, c->clip)))
        paint_places(c);
    if (rect_empty(rect_intersect(zone_pill, c->clip)))
        return;
    if (status == STATUS_PILL)
        paint_pill(c);
    else if (status == STATUS_CORNER)
        paint_bubble(c);
}

static bool in_status(int x, int y)
{
    if (status == STATUS_PILL)
        return rect_contains(pill, x, y);
    if (status == STATUS_CORNER)
        return bubble_cover(x, y, &bubble) > 0;
    return false;
}

bool panel_hit(int x, int y)
{
    if (!started)
        return false;
    return in_status(x, y) || (places_on && rect_contains(places_r, x, y));
}

static int part_at(int x, int y)
{
    if (!in_status(x, y))
        return -1;
    for (int i = 0; i < PART_COUNT; i++)
        if (rect_contains(part_r[i], x, y))
            return i;
    return -1;
}

static int place_at(int x, int y)
{
    if (!places_on || !rect_contains(places_r, x, y))
        return -1;
    for (int i = 0; i < place_count; i++)
        if (rect_contains(place_row(i), x, y))
            return i;
    return -1;
}

static void open_part(int part)
{
    struct rect s = wm_screen_rect(0);
    switch (part) {
    case PART_SEARCH: spotlight_toggle(); break;
    case PART_CLOCK: calendar_toggle(s.x + s.w - 12, status_bottom() + 8); break;
    case PART_WIFI: gui_app_launch(gui_app_find("System Settings"), "network"); break;
    default: control_center_toggle(s.x + s.w - 12); break;
    }
}

void panel_mouse(int x, int y, unsigned button, bool down)
{
    if (down) {
        wm_close_popups();
        place_pressed = button == BUTTON_LEFT ? place_at(x, y) : -1;
        if (place_pressed >= 0)
            wm_damage_z(zone_places, Z_MENUBAR);
        return;
    }
    int pressed = place_pressed;
    place_pressed = -1;
    int p = place_at(x, y);
    if (pressed >= 0) {
        wm_damage_z(zone_places, Z_MENUBAR);
        if (pressed == p) {
            if (!vfs_exists(places[p].path))
                vfs_mkdirs(places[p].path);
            gui_open_file(places[p].path);
        }
        return;
    }
    int part = part_at(x, y);
    if (part >= 0 && button == BUTTON_LEFT)
        open_part(part);
}

void panel_hover(int x, int y)
{
    if (!started)
        return;
    bool on_status = y >= 0 && in_status(x, y);
    int part = on_status ? part_at(x, y) : -1;
    pill_grow.target = status == STATUS_PILL && on_status ? 1024 : 0;
    bubble_grow.target = status == STATUS_CORNER && on_status ? 1024 : 0;
    if (part != part_hover) {
        part_hover = part;
        wm_damage_z(zone_pill, Z_MENUBAR);
    }
    int p = y >= 0 ? place_at(x, y) : -1;
    if (p != place_hover) {
        if (p >= 0) {
            int ty = place_row(p).y;
            if (place_alpha.target == 0)
                spring_set(&place_hl, ty);
            place_hl.target = ty;
        }
        place_alpha.target = p >= 0 ? 1024 : 0;
        place_hover = p;
        wm_damage_z(zone_places, Z_MENUBAR);
    }
}

void panel_changed(void)
{
    if (!started)
        return;
    if (places_on)
        build_places();
    layout();
}

void panel_start(void)
{
    const struct gui_look *l = look_current();
    status = l->status;
    places_on = l->places;
    pill_glass = (struct glass){0};
    places_glass = (struct glass){0};
    pill_glass.z = places_glass.z = Z_MENUBAR;
    part_hover = place_hover = place_pressed = -1;
    spring_set(&pill_grow, 0);
    spring_set(&bubble_grow, 0);
    spring_set(&place_hl, 0);
    spring_set(&place_alpha, 0);
    spring_set(&intro, 0);
    intro.target = 1024;
    last_tick = 0;
    zone_pill = zone_places = (struct rect){0, 0, 0, 0};
    started = true;
    build_places();
    layout();
    if (status != STATUS_MENUBAR) {
        pill_glass.frame = pill;
        glass_register(&pill_glass);
    }
    if (places_on) {
        places_glass.frame = places_r;
        glass_register(&places_glass);
    }
}

void panel_stop(void)
{
    if (!started)
        return;
    started = false;
    glass_unregister(&pill_glass);
    glass_unregister(&places_glass);
    wm_damage_z(zone_pill, Z_MENUBAR);
    wm_damage_z(zone_places, Z_MENUBAR);
    if (launcher && wm_window_alive(launcher))
        wm_destroy(launcher);
    launcher = NULL;
}

/* The floating launcher of the Sunset look: a glowing list of apps and power actions. */

struct launch_menu {
    const struct gui_app *apps[10];
    int count, hover;
    struct spring hl;
    uint64_t tick;
};

#define LAUNCH_ROW 44
#define LAUNCH_W 250

static const char *const launch_names[] = {"Browser", "Files", "Notepad", "Music", "Photos", "Terminal",
                                           "System Settings"};

static void launch_paint(struct window *w, struct canvas *c)
{
    struct launch_menu *m = w->data;
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    gfx_text_box(c, (struct rect){18, 10, c->w - 36, 20}, &font_bold, "SynthOS", style.secondary, ALIGN_LEFT);
    if (m->hover >= 0)
        gfx_round(c, (struct rect){8, m->hl.pos, c->w - 16, LAUNCH_ROW - 4}, 14, ALPHA(style.accent, 70));
    for (int i = 0; i < m->count; i++) {
        int y = 36 + i * LAUNCH_ROW;
        gfx_icon(c, 18, y + 6, m->apps[i]->icon, 30);
        gfx_text_box(c, (struct rect){60, y, c->w - 70, LAUNCH_ROW - 4}, i == m->hover ? &font_bold : &font_sans,
                     m->apps[i]->name, style.text, ALIGN_LEFT);
    }
    int y = 36 + m->count * LAUNCH_ROW + 8;
    gfx_fill(c, (struct rect){16, y - 6, c->w - 32, 1}, style.separator);
    static const enum symbol_id syms[3] = {SYM_LOCK, SYM_RESTART, SYM_POWER};
    for (int i = 0; i < 3; i++) {
        int cx = c->w / 2 + (i - 1) * 56;
        bool hot = m->hover == 100 + i;
        gfx_circle(c, cx, y + 20, 17, hot ? ALPHA(style.accent, 200) : ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 20));
        gfx_symbol(c, cx - 8, y + 12, syms[i], 16, hot ? RGB(255, 255, 255) : style.text);
    }
}

static int launch_at(struct window *w, int x, int y)
{
    struct launch_menu *m = w->data;
    if (y >= 36 && y < 36 + m->count * LAUNCH_ROW && x > 6 && x < w->frame.w - 6)
        return (y - 36) / LAUNCH_ROW;
    int by = 36 + m->count * LAUNCH_ROW + 8;
    for (int i = 0; i < 3; i++) {
        int cx = w->frame.w / 2 + (i - 1) * 56;
        if ((x - cx) * (x - cx) + (y - by - 20) * (y - by - 20) <= 18 * 18)
            return 100 + i;
    }
    return -1;
}

static void launch_event(struct window *w, struct gui_event *e)
{
    struct launch_menu *m = w->data;
    if (e->type == EV_MOUSE_MOVE) {
        int h = launch_at(w, e->x, e->y);
        if (h != m->hover) {
            if (h >= 0 && h < 100) {
                int ty = 36 + h * LAUNCH_ROW;
                if (m->hover < 0 || m->hover >= 100)
                    spring_set(&m->hl, ty);
                m->hl.target = ty;
            }
            m->hover = h;
            wm_invalidate(w);
        }
    } else if (e->type == EV_TIMER) {
        uint64_t now = hal_ticks_ms();
        if (spring_step(&m->hl, 520, 38, (int)MIN(now - m->tick, 64u)))
            wm_invalidate(w);
        m->tick = now;
    } else if (e->type == EV_MOUSE_UP && e->button == BUTTON_LEFT) {
        int h = launch_at(w, e->x, e->y);
        e->handled = true;
        if (h < 0)
            return;
        const struct gui_app *app = h < 100 ? m->apps[h] : NULL;
        wm_destroy(w);
        if (app)
            gui_app_launch(app, NULL);
        else if (h == 100)
            gui_lock();
        else
            shell_power_menu(h == 101);
    } else if (e->type == EV_KEY && e->key == 27) {
        wm_destroy(w);
    } else if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
    }
}

static void launch_destroy(struct window *w)
{
    if (launcher == w)
        launcher = NULL;
    kfree(w->data);
}

static const struct window_ops launch_ops = {launch_paint, launch_event, NULL, launch_destroy};

void launcher_toggle(int x, int y)
{
    if (launcher && wm_window_alive(launcher)) {
        wm_destroy(launcher);
        return;
    }
    wm_close_popups();
    struct launch_menu *m = kzalloc(sizeof(*m));
    if (!m)
        return;
    for (size_t i = 0; i < ARRAY_SIZE(launch_names); i++) {
        const struct gui_app *app = gui_app_find(launch_names[i]);
        if (app && !strcmp(app->name, launch_names[i]))
            m->apps[m->count++] = app;
    }
    m->hover = -1;
    m->tick = hal_ticks_ms();
    int h = 36 + m->count * LAUNCH_ROW + 60;
    launcher = wm_popup(x, y - h, LAUNCH_W, h, &launch_ops, m, NULL);
    if (!launcher) {
        kfree(m);
        return;
    }
    launcher->radius = 22;
    launcher->flags |= WIN_GLOW;
    wm_set_timer(launcher, 16);
}
