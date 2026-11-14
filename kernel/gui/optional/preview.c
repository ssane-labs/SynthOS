/* The pictures at the top of every Settings pane and before every setting that changes how the system looks: a small
   copy of this computer (its own wallpaper, menu bar, a window, a widget and the Dock, made of the same glass as the
   real ones) that acts out what the pane or the setting does, the setting going from before to after and back. */

#include <synth/gui.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/fmt.h>
#include <synth/string.h>
#include <synth/time.h>

#include "../internal.h"

/* ---- arithmetic without floating point */

static int smooth(int t) /* 0..1024 eased in and out */
{
    t = CLAMP(t, 0, 1024);
    return t * t / 1024 * (3072 - 2 * t) / 1024;
}

static int wave(uint32_t ms, uint32_t period) /* 0..1024..0, a smooth swing */
{
    int t = (int)(ms % period * 2048 / period);
    return smooth(t < 1024 ? t : 2048 - t);
}

static int mix(int a, int b, int t)
{
    return a + (b - a) * t / 1024;
}

/* ---- what a small screen shows */

struct mini {
    bool dark;
    int glass;    /* enum glass_level */
    int effect;   /* enum glass_effect */
    int strength, edge, frost;
    int dock;     /* icon size 0 small, 1 medium, 2 large */
    int dock_at;  /* 0 bottom, 1 corner, 2 side */
    int dock_show; /* 0..1024: hidden .. shown */
    int magnify;  /* 0..1024 how far the icon under the pointer grows */
    int window;   /* 0..1024: a window opening */
    int bounce;   /* 0..1024: an icon jumping */
    bool dots;
    int bright;   /* 100: full */
    int warm;     /* 0..100 */
    int px, py;   /* the pointer, in 1/1024 of the screen; px < 0: none */
    int lock;     /* 0..1024: the lock screen coming down */
    int orb;      /* 0..1024: the assistant's drop coming out */
    int hud;      /* 0..1024: the volume bubble */
    int zoom;     /* 0..1024: the magnifier */
};

static int effect_named(const char *v)
{
    static const char *const names[] = {"regular", "clear", "tinted", "smoky", "prism", "halo"};
    for (int i = 0; i < 6; i++)
        if (!strcmp(v, names[i]))
            return i;
    return GLASS_FX_HALO;
}

static int number_or(const char *v, int fallback)
{
    if (!v || !*v || *v < '0' || *v > '9')
        return fallback;
    int n = 0;
    while (*v >= '0' && *v <= '9')
        n = n * 10 + (*v++ - '0');
    return n;
}

static int setting_number(const char *key, int fallback)
{
    char v[24];
    gui_setting_get(key, v, sizeof(v));
    return number_or(v, fallback);
}

static bool setting_is(const char *key, const char *value)
{
    char v[32];
    gui_setting_get(key, v, sizeof(v));
    return !strcmp(v, value);
}

/* the computer as it is set up now */
static void mini_now(struct mini *m)
{
    memset(m, 0, sizeof(*m));
    char v[32];
    m->dark = style.dark;
    m->glass = style.glass;
    m->effect = gui_glass_effect();
    m->strength = gui_glass_strength();
    m->edge = gui_glass_edge();
    m->frost = gui_glass_frost();
    gui_setting_get("docksize", v, sizeof(v));
    m->dock = !strcmp(v, "small") ? 0 : !strcmp(v, "large") ? 2 : 1;
    gui_setting_get("dockstyle", v, sizeof(v));
    m->dock_at = !strcmp(v, "corner") ? 1 : !strcmp(v, "side") ? 2 : 0;
    m->dock_show = setting_is("dockmode", "off") ? 0 : 1024;
    m->dots = !setting_is("dockdots", "off");
    m->bright = setting_number("brightness", 100);
    m->warm = 0;
    m->px = -1;
}

/* the same with one setting set to value */
static void mini_with(struct mini *m, const char *key, const char *value)
{
    if (!strcmp(key, "appearance"))
        m->dark = !strcmp(value, "dark") || (!strcmp(value, "auto") && look_current()->dark);
    else if (!strcmp(key, "glass"))
        m->glass = !strcmp(value, "frosted") ? GLASS_FROSTED : !strcmp(value, "solid") ? GLASS_SOLID : GLASS_LIQUID;
    else if (!strcmp(key, "glasseffect"))
        m->effect = effect_named(value);
    else if (!strcmp(key, "glassstrength"))
        m->strength = number_or(value, 70);
    else if (!strcmp(key, "glassedge"))
        m->edge = number_or(value, 50);
    else if (!strcmp(key, "glassfrost"))
        m->frost = number_or(value, 50);
    else if (!strcmp(key, "docksize"))
        m->dock = !strcmp(value, "small") ? 0 : !strcmp(value, "large") ? 2 : 1;
    else if (!strcmp(key, "dockstyle"))
        m->dock_at = !strcmp(value, "corner") ? 1 : !strcmp(value, "side") ? 2 : 0;
    else if (!strcmp(key, "dockmode"))
        m->dock_show = !strcmp(value, "off") ? 0 : !strcmp(value, "autohide") ? 300 : 1024;
    else if (!strcmp(key, "magnify"))
        m->magnify = !strcmp(value, "on") ? 1024 : 0;
    else if (!strcmp(key, "dockbounce"))
        m->bounce = !strcmp(value, "on") ? 1024 : 0;
    else if (!strcmp(key, "dockdots"))
        m->dots = !strcmp(value, "on");
    else if (!strcmp(key, "brightness"))
        m->bright = number_or(value, 100);
    else if (!strcmp(key, "nightshift") || !strcmp(key, "nightwarmth"))
        m->warm = !strcmp(value, "off") ? 0 : !strcmp(key, "nightwarmth") ? number_or(value, 50) : 60;
    else if (!strcmp(key, "animations"))
        m->window = !strcmp(value, "on") ? -1 : 1024; /* -1: animated by the caller */
}

/* ---- the wallpaper, at the size of the small screen */

#define WALLS 6 /* the hero's screen, the small ones and the zoomed ones, light and dark */
static struct canvas walls[WALLS];
static bool walls_dark[WALLS];
static char walls_kind[WALLS][48];
static int walls_next;

static const struct canvas *wallpaper_for(bool dark, int w, int h)
{
    char kind[48];
    gui_setting_get("wallpaper", kind, sizeof(kind));
    if (!kind[0] || !strcmp(kind, "auto"))
        str_copy(kind, look_current()->wallpaper ? look_current()->wallpaper : "mono", sizeof(kind));
    int slot = -1;
    for (int i = 0; i < WALLS; i++)
        if (walls[i].px && walls[i].w == w && walls[i].h == h && walls_dark[i] == dark && !strcmp(walls_kind[i], kind))
            return &walls[i];
    for (int i = 0; i < WALLS && slot < 0; i++) /* one of the same size and look is out of date: replace it */
        if (walls[i].px && walls[i].w == w && walls[i].h == h && walls_dark[i] == dark)
            slot = i;
    if (slot < 0) {
        slot = walls_next;
        walls_next = (walls_next + 1) % WALLS;
    }
    struct canvas *c = &walls[slot];
    if (dark == style.dark) { /* the real one, shrunk */
        const struct canvas *real = desktop_wallpaper(0);
        if (real && real->px) {
            if (!c->px || c->w != w || c->h != h) {
                canvas_free(c);
                if (!canvas_alloc(c, w, h))
                    return NULL;
            }
            c->clip = (struct rect){0, 0, w, h};
            gfx_scale_smooth(c, (struct rect){0, 0, w, h}, real, 255);
            walls_dark[slot] = dark;
            str_copy(walls_kind[slot], kind, sizeof(walls_kind[slot]));
            return c;
        }
    }
    struct canvas big;
    if (!canvas_alloc(&big, w * 2, h * 2))
        return NULL;
    wallpaper_paint(&big, kind, dark);
    if (!c->px || c->w != w || c->h != h) {
        canvas_free(c);
        if (!canvas_alloc(c, w, h)) {
            canvas_free(&big);
            return NULL;
        }
    }
    c->clip = (struct rect){0, 0, w, h};
    gfx_scale_smooth(c, (struct rect){0, 0, w, h}, &big, 255);
    canvas_free(&big);
    walls_dark[slot] = dark;
    str_copy(walls_kind[slot], kind, sizeof(walls_kind[slot]));
    return c;
}

/* ---- drawing the small screen */

static void pointer(struct canvas *c, int x, int y, int size)
{
    for (int i = 0; i < size; i++) { /* an arrow: a white wedge with a dark edge */
        int w = i * 2 / 3 + 1;
        gfx_fill(c, (struct rect){x - 1, y + i, w + 2, 1}, RGB(20, 20, 24));
        gfx_fill(c, (struct rect){x, y + i, w, 1}, RGB(255, 255, 255));
    }
}

static void glass_with(const struct mini *m, struct canvas *c, struct rect r, int radius, enum glass_kind kind,
                       uint32_t tint)
{
    glass_control(c, r, radius, kind, tint, 0);
    UNUSED(m);
}

static const enum icon_id dock_icons[] = {ICON_FILES, ICON_BROWSER, ICON_TERMINAL, ICON_PHOTOS, ICON_MUSIC,
                                          ICON_SETTINGS};

static void draw_dock(const struct mini *m, struct canvas *c, struct rect s, uint32_t ms)
{
    if (m->dock_show <= 0)
        return;
    int n = (int)ARRAY_SIZE(dock_icons);
    int icon = MAX(6, s.h * (m->dock == 0 ? 7 : m->dock == 2 ? 11 : 9) / 100), gap = MAX(2, icon / 5);
    bool side = m->dock_at == 2;
    int len = n * icon + (n + 1) * gap, thick = icon + 2 * gap;
    int hide = (1024 - m->dock_show) * (thick + 4) / 1024;
    struct rect d = side ? (struct rect){s.x + 4 - hide, s.y + (s.h - len) / 2, thick, len}
                  : m->dock_at == 1 ? (struct rect){s.x + s.w - len - 6, s.y + s.h - thick - 4 + hide, len, thick}
                                    : (struct rect){s.x + (s.w - len) / 2, s.y + s.h - thick - 4 + hide, len, thick};
    glass_with(m, c, d, MIN(thick / 2, icon / 2 + gap), GLASS_DOCK, 0);
    int under = m->magnify ? (int)(ms / 700 % (uint32_t)n) : -1;
    for (int i = 0; i < n; i++) {
        int grow = 0;
        if (under >= 0) {
            int dist = ABS(i - under);
            grow = dist == 0 ? icon * 6 / 10 : dist == 1 ? icon * 3 / 10 : 0;
            grow = grow * m->magnify / 1024;
        }
        int lift = i == 1 && m->bounce ? wave(ms, 600) * icon / 2 / 1024 * m->bounce / 1024 : 0;
        int size = icon + grow;
        int x = side ? d.x + gap - grow / 2 + grow : d.x + gap + i * (icon + gap) - grow / 2;
        int y = side ? d.y + gap + i * (icon + gap) - grow / 2 : d.y + gap - grow - lift;
        if (side)
            x = d.x + gap;
        gfx_icon(c, x, y, dock_icons[i], size);
        if (m->dots && i < 3)
            gfx_circle(c, side ? d.x + 1 : x + size / 2, side ? y + size / 2 : d.y + d.h - 2, 1,
                       m->dark ? RGB(230, 230, 236) : RGB(40, 40, 46));
    }
}

static void draw_window(const struct mini *m, struct canvas *c, struct rect s, int open)
{
    if (open <= 0)
        return;
    struct rect full = {s.x + s.w * 22 / 100, s.y + s.h * 18 / 100, s.w * 56 / 100, s.h * 54 / 100};
    int k = smooth(open);
    struct rect w = {mix(full.x + full.w / 2, full.x, k), mix(s.y + s.h - 6, full.y, k), mix(4, full.w, k),
                     mix(3, full.h, k)};
    if (w.w < 6 || w.h < 6)
        return;
    int rad = MAX(2, s.h / 40);
    uint32_t back = m->dark ? RGB(32, 32, 36) : RGB(248, 248, 250);
    gfx_drop_shadow(c, w, rad, MAX(3, s.h / 30), m->dark ? 110 : 60, 1);
    gfx_round(c, w, rad, back);
    struct rect side = {w.x + 2, w.y + 2, w.w * 28 / 100, w.h - 4};
    gfx_round(c, side, rad, m->dark ? RGB(44, 44, 50) : RGB(234, 234, 238));
    if (k > 900) {
        glass_lit_edge(c, side, rad, GLASS_PANEL);
        int line = MAX(2, s.h / 60);
        for (int i = 0; i < 4; i++) {
            gfx_round(c, (struct rect){side.x + 3, side.y + 6 + i * line * 3, side.w - 8, line}, line / 2,
                      i == 1 ? style.accent : m->dark ? RGB(90, 90, 98) : RGB(190, 190, 198));
            gfx_round(c, (struct rect){side.x + side.w + 6, side.y + 6 + i * line * 3, (w.w - side.w) * (60 + i * 7) / 100,
                                       line},
                      line / 2, m->dark ? RGB(70, 70, 76) : RGB(214, 214, 220));
        }
        struct rect b = {w.x + w.w - w.w / 4 - 4, w.y + w.h - s.h / 12 - 4, w.w / 4, s.h / 12};
        glass_control(c, b, b.h / 2, GLASS_CONTROL, style.accent, 0);
    }
}

static void tint_screen(struct canvas *c, struct rect s, int bright, int warm)
{
    if (bright >= 100 && warm <= 0)
        return;
    struct rect a = rect_intersect(s, c->clip);
    int k = 40 + CLAMP(bright, 0, 100) * 60 / 100;
    for (int y = a.y; y < a.y + a.h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = a.x; x < a.x + a.w; x++) {
            uint32_t p = row[x];
            int r = (int)(p >> 16 & 0xff), g = (int)(p >> 8 & 0xff), b = (int)(p & 0xff);
            g = g * (1000 - warm * 2) / 1000;
            b = b * (1000 - warm * 6) / 1000;
            row[x] = (p & 0xff000000u) | (uint32_t)(r * k / 100) << 16 | (uint32_t)(g * k / 100) << 8 |
                     (uint32_t)(b * k / 100);
        }
    }
}

static void draw_screen(const struct mini *m, struct canvas *c, struct rect s, uint32_t ms)
{
    struct canvas clip = *c;
    clip.clip = rect_intersect(c->clip, s);
    if (rect_empty(clip.clip))
        return;
    c = &clip;
    /* the glass of this picture: its choices, not necessarily the ones in use */
    struct glass_preview gp = {m->effect, m->strength, m->edge, m->frost};
    const struct glass_preview *saved_gp = glass_preview;
    bool saved_dark = style.dark;
    int saved_glass = style.glass;
    glass_preview = &gp;
    style.dark = m->dark;
    style.glass = m->glass;
    const struct canvas *wp = wallpaper_for(m->dark, s.w, s.h);
    if (wp)
        gfx_blit(c, s.x, s.y, wp, (struct rect){0, 0, wp->w, wp->h});
    else
        gfx_clear(c, s, m->dark ? RGB(30, 32, 40) : RGB(120, 150, 200));
    if (m->lock > 0) { /* the lock screen: a big clock over the picture */
        struct rtc_time t;
        time_local(&t);
        char clock[8];
        ksnprintf(clock, sizeof(clock), "%u:%02u", t.hour, t.minute);
        gfx_fill(c, s, ALPHA(0, 40 * m->lock / 1024));
        gfx_text_box(c, (struct rect){s.x, s.y + s.h / 6 - (1024 - m->lock) * s.h / 4096, s.w, s.h / 4}, &font_title,
                     clock, ALPHA(0xffffffffu, 255 * m->lock / 1024), ALIGN_CENTER);
    }
    if (m->lock < 1024) {
        int bar = MAX(5, s.h / 22);
        struct rect mb = {s.x, s.y, s.w, bar};
        gfx_fill(c, mb, m->dark ? ALPHA(0, 70) : ALPHA(0xffffffffu, 90));
        uint32_t ink = m->dark ? RGB(236, 236, 240) : RGB(30, 30, 34);
        for (int i = 0; i < 3; i++)
            gfx_round(c, (struct rect){s.x + 6 + i * bar * 2, s.y + bar / 3, bar + (i == 0 ? 0 : bar / 2), bar / 3},
                      1, ALPHA(ink, i ? 150 : 230));
        gfx_round(c, (struct rect){s.x + s.w - bar * 4, s.y + bar / 3, bar * 3, bar / 3}, 1, ALPHA(ink, 170));
        if (m->window != 0) /* a widget: the same glass the desktop's are made of */
            glass_with(m, c, (struct rect){s.x + s.w * 5 / 100, s.y + s.h * 16 / 100, s.w * 16 / 100, s.w * 16 / 100},
                       s.w / 40, GLASS_PANEL, 0);
        draw_window(m, c, s, m->window);
        if (m->orb > 0) { /* the assistant: a drop that stretches into a pill under the menu bar */
            int k = smooth(m->orb), d = MAX(8, s.h / 9), w = mix(d, s.w * 44 / 100, k);
            struct rect o = {s.x + (s.w - w) / 2, s.y + bar + 3 - (1024 - k) * d / 1024, w, d};
            glass_with(m, c, o, d / 2, GLASS_MENU, 0);
            for (int i = 0; i < 3; i++)
                gfx_circle(c, o.x + d / 2, o.y + d / 2, d / 3 - i * 2,
                           ALPHA(gfx_halo_tone((int)((ms / 4 + (uint32_t)i * 300) % 1024)), 200));
        }
        if (m->hud > 0) { /* the volume: a bubble with a bar */
            int k = smooth(m->hud);
            struct rect h = {s.x + s.w - s.w / 3 - 8, s.y + bar + 6, s.w / 3, s.h / 9};
            glass_with(m, c, h, h.h / 2, GLASS_MENU, 0);
            gfx_round(c, (struct rect){h.x + h.h / 2, h.y + h.h / 2 - 2, (h.w - h.h) * k / 1024, 4}, 2,
                      m->dark ? RGB(240, 240, 244) : RGB(40, 40, 46));
        }
        draw_dock(m, c, s, ms);
    }
    if (m->zoom > 0) { /* the magnifier: twice the size under a round lens */
        int r = s.h / 4, cx = s.x + s.w / 2 + (m->px >= 0 ? (m->px - 512) * s.w / 2048 : 0), cy = s.y + s.h / 2;
        struct canvas lens;
        if (canvas_alloc(&lens, r, r)) {
            for (int y = 0; y < r; y++)
                for (int x = 0; x < r; x++) {
                    int sx = CLAMP(cx - r / 2 + x, c->clip.x, c->clip.x + c->clip.w - 1);
                    int sy = CLAMP(cy - r / 2 + y, c->clip.y, c->clip.y + c->clip.h - 1);
                    lens.px[y * lens.stride + x] = c->px[sy * c->stride + sx] | 0xff000000u;
                }
            int R = r * m->zoom / 1024;
            struct canvas round = *c;
            round.clip = rect_intersect(c->clip, (struct rect){cx - R, cy - R, 2 * R, 2 * R});
            gfx_scale_smooth(&round, (struct rect){cx - R, cy - R, 2 * R, 2 * R}, &lens, 255);
            canvas_free(&lens);
            gfx_ring(c, cx, cy, R, RGB(255, 255, 255));
        }
    }
    tint_screen(c, s, m->bright, m->warm);
    if (m->px >= 0)
        pointer(c, s.x + m->px * s.w / 1024, s.y + m->py * s.h / 1024, MAX(6, s.h / 14));
    style.dark = saved_dark;
    style.glass = saved_glass;
    glass_preview = saved_gp;
}

/* ---- the computer around it */

static void draw_computer(struct canvas *c, struct rect r, struct rect *screen)
{
    int sh = r.h * 76 / 100, sw = sh * 16 / 10;
    if (sw > r.w - 20) {
        sw = r.w - 20;
        sh = sw * 10 / 16;
    }
    int bez = MAX(4, sh / 28);
    struct rect body = {r.x + (r.w - sw) / 2 - bez, r.y + 6, sw + 2 * bez, sh + 2 * bez};
    gfx_drop_shadow(c, body, 12, 18, style.dark ? 140 : 70, 6);
    gfx_round_gradient(c, body, 12, RGB(58, 58, 64), RGB(22, 22, 26)); /* aluminium edge, black glass */
    gfx_round(c, rect_inset(body, 2), 10, RGB(12, 12, 14));
    gfx_round_frame_gradient(c, body, 12, ALPHA(0xffffffffu, 120), ALPHA(0xffffffffu, 20));
    *screen = (struct rect){body.x + bez, body.y + bez, sw, sh};
    int neck_w = sw / 6, neck_h = r.y + r.h - (body.y + body.h) - 8;
    if (neck_h > 2) {
        struct rect neck = {body.x + (body.w - neck_w) / 2, body.y + body.h, neck_w, neck_h};
        gfx_round_gradient(c, neck, 2, RGB(170, 172, 180), RGB(110, 112, 120));
        struct rect foot = {neck.x - neck_w / 3, neck.y + neck.h - 2, neck_w + neck_w * 2 / 3, 8};
        gfx_round_gradient(c, foot, 4, RGB(200, 202, 210), RGB(130, 132, 140));
    }
}

/* ---- the stories each pane tells */

static void act(struct mini *m, const char *pane, uint32_t ms)
{
    int w = wave(ms, 6000);
    m->window = 1024;
    if (!strcmp(pane, "appearance")) { /* light to dark and back, the glass changing its effect */
        m->dark = w > 512;
        m->effect = (int)(ms / 6000 % 6);
    } else if (!strcmp(pane, "dock")) {
        m->magnify = 1024;
        m->bounce = 1024;
        m->px = 340 + (int)(ms / 700 % 6) * 60;
        m->py = 950;
    } else if (!strcmp(pane, "displays")) {
        m->bright = mix(100, 45, w);
        m->warm = mix(0, 70, wave(ms + 3000, 6000));
    } else if (!strcmp(pane, "lock")) {
        m->lock = w;
    } else if (!strcmp(pane, "ai")) {
        m->orb = w;
        m->window = 0;
    } else if (!strcmp(pane, "sound")) {
        m->hud = w;
    } else if (!strcmp(pane, "accessibility")) {
        m->zoom = w;
        m->px = 300 + w / 3;
        m->py = 500;
    } else { /* a window opening, the pointer going to it */
        m->window = MIN(1024, (int)(ms % 5000) * 1024 / 1400);
        m->px = mix(800, 520, smooth((int)(ms % 5000) * 1024 / 2500));
        m->py = mix(900, 520, smooth((int)(ms % 5000) * 1024 / 2500));
    }
}

static void hero(struct canvas *c, struct rect r, const char *pane, uint32_t ms)
{
    /* a stage: soft light from above on the group's colour */
    gfx_round_gradient(c, r, 10, color_shade(style.group, style.dark ? 10 : 6), style.group);
    struct rect screen;
    draw_computer(c, rect_inset(r, 8), &screen);
    struct mini m;
    mini_now(&m);
    act(&m, pane, ms);
    draw_screen(&m, c, screen, ms);
    gfx_round_frame_gradient(c, screen, 2, ALPHA(0xffffffffu, 40), ALPHA(0xffffffffu, 0)); /* the glass of the panel */
}

static const char *const shown_keys[] = {"appearance", "glass", "glasseffect", "glassstrength", "glassedge",
                                         "glassfrost", "docksize", "dockstyle", "dockmode", "magnify",
                                         "dockbounce", "dockdots", "brightness", "nightshift", "nightwarmth",
                                         "animations"};

static bool shows(const char *key)
{
    for (size_t i = 0; i < ARRAY_SIZE(shown_keys); i++)
        if (!strcmp(key, shown_keys[i]))
            return true;
    return false;
}

/* the glass and the Dock are small on a small screen: show the bottom of it, bigger */
static bool zoomed(const char *key)
{
    return !strncmp(key, "glass", 5) || !strcmp(key, "docksize") || !strcmp(key, "magnify") ||
           !strcmp(key, "dockbounce") || !strcmp(key, "dockdots");
}

/* before, then after, then back: 1.4 s each with a crossfade between */
static void setting(struct canvas *c, struct rect r, const char *key, const char *before, const char *after,
                    uint32_t ms)
{
    uint32_t t = ms % 4000;
    int k = t < 1300 ? 0 : t < 1800 ? smooth((int)(t - 1300) * 1024 / 500) : t < 3500 ? 1024
                                                                                     : 1024 - smooth((int)(t - 3500) * 1024 / 500);
    struct canvas tile;
    if (!canvas_alloc(&tile, r.w, r.h))
        return;
    struct rect local = {0, 0, r.w, r.h};
    tile.clip = local;
    struct mini m;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0 && k >= 1024) || (pass == 1 && k <= 0))
            continue;
        mini_now(&m);
        m.window = 1024;
        mini_with(&m, key, pass ? after : before);
        if (m.window < 0) /* animations on: the window grows */
            m.window = (int)(ms % 2000) * 1024 / 900;
        if (!strcmp(key, "magnify") || !strcmp(key, "dockbounce"))
            m.px = 360 + (int)(ms / 700 % 6) * 56, m.py = 940;
        draw_screen(&m, &tile, zoomed(key) ? (struct rect){-r.w * 3 / 4, -r.h * 3 / 2, r.w * 5 / 2, r.h * 5 / 2} : local,
                    ms);
        struct canvas out = *c;
        out.clip = rect_intersect(c->clip, r);
        int alpha = pass ? k * 255 / 1024 : 255;
        if (alpha >= 255)
            gfx_blit(&out, r.x, r.y, &tile, local);
        else
            gfx_blit_alpha(&out, r.x, r.y, &tile, local, alpha);
    }
    canvas_free(&tile);
    /* rounded like a screen, with a dot telling before from after */
    gfx_round_frame(c, r, 6, ALPHA(0, 90));
    gfx_round_frame_gradient(c, rect_inset(r, 1), 5, ALPHA(0xffffffffu, 70), ALPHA(0xffffffffu, 10));
    int dot = 5;
    gfx_circle(c, r.x + r.w - 7, r.y + r.h - 7, dot / 2 + 1, ALPHA(0, 120));
    gfx_circle(c, r.x + r.w - 7, r.y + r.h - 7, dot / 2, k > 512 ? style.accent : RGB(200, 200, 206));
}

static const struct settings_preview service = {hero, shows, setting};
static const struct module_ext ext = {NULL, 0, NULL, NULL, NULL, NULL, NULL, "settings-preview", &service};
GUI_APP_FULL(NULL, NULL, &ext);
