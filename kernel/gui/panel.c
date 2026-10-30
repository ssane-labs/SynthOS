#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/net.h>
#include <synth/string.h>
#include <synth/time.h>

#include "internal.h"

#define PILL_H 34
#define PILL_ICON 34

enum pill_part {
    PART_SEARCH,
    PART_WIFI,
    PART_VOLUME,
    PART_CONTROL,
    PART_LANG,
    PART_CLOCK,
    PART_COUNT,
};

static bool started;
static enum status_kind status;
static struct glass pill_glass;
static struct rect pill, part_r[PART_COUNT], zone_pill;
static int part_hover = -1;
static struct spring pill_grow, intro;
static uint64_t last_tick;

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

int status_bottom(void)
{
    struct rect s = wm_screen_rect(0);
    switch (look_current()->status) {
    case STATUS_PILL: return s.y + 12 + PILL_H;
    default: return s.y + menubar_height();
    }
}

static void clock_text(char *buf, size_t cap)
{
    gui_clock_text(buf, cap, false);
}

static void layout(void)
{
    struct rect s = wm_screen_rect(0);
    struct rect old_pill = zone_pill;
    int dy = (1024 - CLAMP(intro.pos, 0, 1024)) * 60 / 1024;
    if (status == STATUS_PILL) {
        char clock[16];
        clock_text(clock, sizeof(clock));
        int g = CLAMP(pill_grow.pos, -100, 1124);
        int cw = gfx_text_width(&font_bold, clock) + 22;
        int lw = keymap_multi() ? gfx_text_width(&font_bold, keymap_label()) + 14 : 0;
        int w = 4 * PILL_ICON + lw + cw + 10 + g * 8 / 1024;
        int h = PILL_H + g * 4 / 1024;
        pill = (struct rect){s.x + s.w - w - 12, s.y + 12 - dy - g * 2 / 1024, w, h};
        int x = pill.x + 5;
        for (int i = 0; i < PART_CONTROL + 1; i++, x += PILL_ICON)
            part_r[i] = (struct rect){x, pill.y, PILL_ICON, pill.h};
        part_r[PART_LANG] = (struct rect){x, pill.y, lw, pill.h};
        x += lw;
        part_r[PART_CLOCK] = (struct rect){x, pill.y, cw, pill.h};
        zone_pill = rect_inset(pill, -30);
        glass_set_frame(&pill_glass, pill);
    }
    wm_damage_z(rect_union(old_pill, zone_pill), Z_MENUBAR);
}

bool panel_animate(void)
{
    if (!started)
        return false;
    uint64_t now = hal_ticks_ms();
    int dt = last_tick ? (int)MIN(now - last_tick, 64u) : 16;
    last_tick = now;
    bool busy = spring_step(&pill_grow, 420, 26, dt);
    busy |= spring_step(&intro, 160, 20, dt);
    if (busy)
        layout();
    return busy;
}

static void paint_part_icon(struct canvas *c, int i, struct rect r)
{
    bool silent = gui_setting_on("mute") || !gui_setting_number("volume", 100);
    enum symbol_id sym = i == PART_SEARCH   ? SYM_SEARCH
                         : i == PART_WIFI   ? (net_enabled() ? SYM_WIFI : SYM_WIFI_OFF)
                         : i == PART_VOLUME ? (silent ? SYM_MUTE : SYM_VOLUME)
                                            : SYM_CONTROL;
    bool online = !net_enabled() || (net_interface_count() > 0 && net_interface(0)->configured);
    uint32_t col = i == PART_WIFI && !online ? ink_soft() : ink();
    if (i == part_hover)
        gfx_circle(c, r.x + r.w / 2, r.y + r.h / 2, MIN(r.w, r.h) / 2 - 3, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 26));
    gfx_symbol(c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, sym, 16, col);
}

static void paint_pill(struct canvas *c)
{
    struct glass_look look;
    glass_look_for(&look, GLASS_PANEL, pill.h / 2);
    glass_draw(c, &pill_glass, &look);
    gfx_round_frame(c, pill, pill.h / 2, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 90));
    for (int i = 0; i < PART_LANG; i++)
        paint_part_icon(c, i, part_r[i]);
    if (part_r[PART_LANG].w)
        gfx_text_box(c, part_r[PART_LANG], &font_bold, keymap_label(), ink(), ALIGN_CENTER);
    char clock[16];
    clock_text(clock, sizeof(clock));
    if (part_hover == PART_CLOCK)
        gfx_round(c, rect_inset(part_r[PART_CLOCK], 4), (pill.h - 8) / 2, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 22));
    gfx_text_box(c, part_r[PART_CLOCK], &font_bold, clock, ink(), ALIGN_CENTER);
}

void panel_paint(struct canvas *c, int screen)
{
    if (!started || screen != 0)
        return;
    if (rect_empty(rect_intersect(zone_pill, c->clip)))
        return;
    if (status == STATUS_PILL)
        paint_pill(c);
}

static bool in_status(int x, int y)
{
    if (status == STATUS_PILL)
        return rect_contains(pill, x, y);
    return false;
}

bool panel_hit(int x, int y)
{
    if (!started)
        return false;
    return in_status(x, y);
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

static void open_part(int part)
{
    struct rect s = wm_screen_rect(0);
    switch (part) {
    case PART_SEARCH: spotlight_toggle(); break;
    case PART_CLOCK: notification_center_toggle(); break;
    case PART_WIFI: control_module_toggle(CONTROL_NETWORK, part_r[part].x + part_r[part].w + 60, status_bottom() + 8); break;
    case PART_VOLUME: control_module_toggle(CONTROL_SOUND, part_r[part].x + part_r[part].w + 60, status_bottom() + 8); break;
    case PART_LANG:
        keymap_next();
        chrome_changed();
        break;
    default: control_center_toggle(s.x + s.w - 12); break;
    }
}

void panel_mouse(int x, int y, unsigned button, bool down)
{
    if (down) {
        wm_close_popups();
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
    pill_grow.target = 0;
    if (part != part_hover) {
        part_hover = part;
        wm_damage_z(zone_pill, Z_MENUBAR);
    }
}

void panel_changed(void)
{
    if (!started)
        return;
    layout();
}

void panel_start(void)
{
    const struct gui_look *l = look_current();
    status = l->status;
    pill_glass = (struct glass){0};
    pill_glass.z = Z_MENUBAR;
    part_hover = -1;
    spring_set(&pill_grow, 0);
    spring_set(&intro, 0);
    intro.target = 1024;
    last_tick = 0;
    zone_pill = (struct rect){0, 0, 0, 0};
    started = true;
    layout();
    if (status != STATUS_MENUBAR) {
        pill_glass.frame = pill;
        glass_register(&pill_glass);
    }
}

void panel_stop(void)
{
    if (!started)
        return;
    started = false;
    glass_unregister(&pill_glass);
    wm_damage_z(zone_pill, Z_MENUBAR);
}
