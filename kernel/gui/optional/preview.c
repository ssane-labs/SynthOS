/* The pictures at the top of every Settings pane and before every setting that changes how the system looks. They
   are recordings of this system (/system/previews.pak, made by tools/previews): each pane has its own scene, each
   value of a setting its own clip, the setting going from before to after and back. Without a recording a small
   drawn copy of the computer acts it out instead. */

#include <synth/gui.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/fmt.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/shell.h>
#include <synth/config.h>
#include <synth/picture.h>
#include <synth/vfs.h>

#include "../internal.h"

/* ---- the recordings */

#define CLIP_SLOTS 20

struct clip {
    char name[24];
    const uint8_t *data;
    uint32_t size;
    int w, h, fps, frames;
    int shown;   /* the frame in the canvas, -1 before the first */
    uint32_t at; /* where the next frame starts */
    struct canvas cv;
    uint64_t used;
};

static uint8_t *pak;
static uint32_t pak_size;
static struct clip clips[CLIP_SLOTS];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int rd16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static bool pak_open(void)
{
    static bool tried;
    if (!pak && !tried) {
        tried = true;
        char *buf;
        size_t n;
        if (vfs_read_all("/system/previews.pak", &buf, &n) == 0) {
            if (n >= 8 && !memcmp(buf, "SPV1", 4) && n < 0x40000000u) {
                pak = (uint8_t *)buf;
                pak_size = (uint32_t)n;
            } else {
                kfree(buf);
            }
        }
    }
    return pak != NULL;
}

static bool clip_find(const char *name, const uint8_t **data, uint32_t *size)
{
    if (!pak_open())
        return false;
    uint32_t count = rd32(pak + 4);
    if (count > (pak_size - 8) / 32)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = pak + 8 + i * 32;
        if (strncmp((const char *)e, name, 24))
            continue;
        uint32_t off = rd32(e + 24), len = rd32(e + 28);
        if (off > pak_size || len > pak_size - off || len < 8)
            return false;
        *data = pak + off;
        *size = len;
        return true;
    }
    return false;
}

/* The clip in a slot of its own, the least recently used slot given up for it. */
static struct clip *clip_get(const char *name)
{
    struct clip *slot = NULL;
    for (int i = 0; i < CLIP_SLOTS; i++) {
        if (clips[i].data && !strcmp(clips[i].name, name)) {
            clips[i].used = hal_ticks_ms();
            return &clips[i];
        }
        if (!slot || (slot->data && (!clips[i].data || clips[i].used < slot->used)))
            slot = &clips[i];
    }
    const uint8_t *data;
    uint32_t size;
    if (!clip_find(name, &data, &size))
        return NULL;
    int w = rd16(data), h = rd16(data + 2), fps = rd16(data + 4), frames = rd16(data + 6);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || fps <= 0 || frames <= 0)
        return NULL;
    struct clip *c = slot;
    canvas_free(&c->cv);
    memset(c, 0, sizeof(*c));
    if (!canvas_alloc(&c->cv, w, h))
        return NULL;
    c->cv.clip = (struct rect){0, 0, w, h};
    str_copy(c->name, name, sizeof(c->name));
    c->data = data;
    c->size = size;
    c->w = w, c->h = h, c->fps = fps, c->frames = frames;
    c->shown = -1;
    c->at = 8;
    c->used = hal_ticks_ms();
    return c;
}

/* Frame i in the clip's canvas: the changed rectangles of every frame since the one shown, from the start when
   the clip went round. */
static bool clip_seek(struct clip *c, int i)
{
    if (i < c->shown) {
        c->shown = -1;
        c->at = 8;
    }
    while (c->shown < i) {
        if (c->at + 12 > c->size)
            return false;
        const uint8_t *f = c->data + c->at;
        int x = rd16(f), y = rd16(f + 2), w = rd16(f + 4), h = rd16(f + 6);
        uint32_t len = rd32(f + 8);
        if (len > c->size - c->at - 12)
            return false;
        if (len) {
            struct picture p;
            if (picture_decode(f + 12, len, &p) == 0) {
                if (p.w == w && p.h == h && x + w <= c->w && y + h <= c->h) {
                    struct canvas src = canvas_make(p.px, p.w, p.h);
                    gfx_blit(&c->cv, x, y, &src, (struct rect){0, 0, w, h});
                }
                picture_free(&p);
            }
        }
        c->at += 12 + len;
        c->shown++;
    }
    return true;
}

/* The clip at this moment over r, filling it (cut at the sides or at the top and bottom), with round corners. */
static bool clip_draw(struct canvas *out, struct rect r, const char *name, uint32_t ms, int radius, int alpha)
{
    struct clip *c = clip_get(name);
    if (!c || r.w <= 0 || r.h <= 0)
        return false;
    int i = (int)((uint64_t)ms * (uint64_t)c->fps / 1000 % (uint64_t)c->frames);
    if (!clip_seek(c, i))
        return false;
    struct rect from = {0, 0, c->w, c->h};
    if ((int64_t)c->w * r.h > (int64_t)c->h * r.w) {
        from.w = (int)((int64_t)c->h * r.w / r.h);
        from.x = (c->w - from.w) / 2;
    } else {
        from.h = (int)((int64_t)c->w * r.h / r.w);
        from.y = (c->h - from.h) / 2;
    }
    struct canvas view = canvas_make(c->cv.px + from.y * c->cv.stride + from.x, from.w, from.h);
    view.stride = c->cv.stride;
    view.clip = (struct rect){0, 0, from.w, from.h};
    struct canvas tile;
    if (!canvas_alloc(&tile, r.w, r.h))
        return false;
    tile.clip = (struct rect){0, 0, r.w, r.h};
    gfx_scale_smooth(&tile, tile.clip, &view, 255);
    struct canvas cl = *out;
    cl.clip = rect_intersect(out->clip, r);
    if (alpha >= 255) {
        gfx_blit_round(&cl, r, &tile, radius, false);
    } else {
        struct canvas mix;
        if (canvas_alloc(&mix, r.w, r.h)) {
            mix.clip = tile.clip;
            gfx_blit(&mix, 0, 0, out, r); /* what lies under it now */
            gfx_blit_alpha(&mix, 0, 0, &tile, tile.clip, alpha);
            gfx_blit_round(&cl, r, &mix, radius, false);
            canvas_free(&mix);
        }
    }
    canvas_free(&tile);
    return true;
}

/* The recording of one value of a setting: "s_<key>_<value>"; numbers fall to "lo" or "hi". */
static void value_clip(char *name, size_t cap, const char *key, const char *value)
{
    long n;
    if (str_parse_long(value, &n))
        ksnprintf(name, cap, "s_%s_%s", key, n >= 50 ? "hi" : "lo");
    else
        ksnprintf(name, cap, "s_%s_%s", key, value);
}

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
    bool halo, glow, spot; /* the Dock's effects */
    int fx;       /* their strength, percent */
    int filter;   /* 0 none, 1 grayscale, 2 inverted */
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
    m->halo = !setting_is("dockhalo", "off");
    m->glow = !setting_is("dockglow", "off");
    m->spot = !setting_is("dockspot", "off");
    m->fx = setting_is("dockfx", "subtle") ? 55 : setting_is("dockfx", "vivid") ? 160 : 100;
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
    else if (!strcmp(key, "dockhalo"))
        m->halo = !strcmp(value, "on");
    else if (!strcmp(key, "dockglow"))
        m->glow = !strcmp(value, "on");
    else if (!strcmp(key, "dockspot"))
        m->spot = !strcmp(value, "on");
    else if (!strcmp(key, "dockfx"))
        m->fx = !strcmp(value, "subtle") ? 55 : !strcmp(value, "vivid") ? 160 : 100;
    else if (!strcmp(key, "colorfilter"))
        m->filter = !strcmp(value, "grayscale") ? 1 : !strcmp(value, "invert") ? 2 : 0;
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

/* an icon's own color: the average of its saturated pixels, worked out once */
static uint32_t icon_tone(enum icon_id id)
{
    static uint32_t tones[ICON_COUNT];
    if ((unsigned)id >= ICON_COUNT)
        return style.accent;
    if (tones[id])
        return tones[id];
    struct canvas t;
    uint32_t tone = style.accent;
    if (canvas_alloc(&t, 24, 24)) {
        t.clip = (struct rect){0, 0, 24, 24};
        gfx_fill(&t, t.clip, 0);
        gfx_icon(&t, 0, 0, id, 24);
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (int i = 0; i < 24 * 24; i++) {
            uint32_t p = t.px[(i / 24) * t.stride + i % 24];
            int pr = p >> 16 & 255, pg = p >> 8 & 255, pb = p & 255;
            int hi = MAX(pr, MAX(pg, pb)), lo = MIN(pr, MIN(pg, pb));
            if ((p >> 24) > 128 && hi - lo > 40)
                r += (uint32_t)pr, g += (uint32_t)pg, b += (uint32_t)pb, n++;
        }
        if (n)
            tone = RGB(r / n, g / n, b / n);
        canvas_free(&t);
    }
    return tones[id] = tone | 0xff000000u;
}

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
    int under = m->magnify || m->spot ? (int)(ms / 700 % (uint32_t)n) : -1;
    int fx = CLAMP(m->fx, 0, 200);
    for (int i = 0; i < n && !side; i++) { /* light from the icons, under them */
        int cx = d.x + gap + i * (icon + gap) + icon / 2;
        uint32_t tone = icon_tone(dock_icons[i]) & 0xffffffu;
        if (m->halo)
            gfx_glow(c, (struct rect){cx - icon / 3, d.y + d.h - 3, icon * 2 / 3, 2}, 1, icon / 2,
                     MIN(255, 70 * fx / 100), tone | 0xff000000u);
        if (m->glow)
            gfx_glow(c, (struct rect){cx - icon / 4, d.y + gap + icon / 4, icon / 2, icon / 2}, icon / 4, icon / 2,
                     MIN(255, 90 * fx / 100), tone | 0xff000000u);
    }
    if (m->spot && under >= 0 && !side) {
        int cx = d.x + gap + under * (icon + gap) + icon / 2;
        gfx_glow(c, (struct rect){cx - 2, d.y + d.h / 2 - 2, 4, 4}, 2, d.h, MIN(255, 80 * fx / 100), 0xffffffffu);
    }
    if (!m->magnify)
        under = -1;
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

/* ---- the scenes at the top of each pane: the desktop itself, edge to edge, and something happening on it */

static uint32_t ink_of(const struct mini *m, int alpha)
{
    return ALPHA(m->dark ? 0xfff2f2f6u : 0xff1c1c22u, alpha);
}

/* a glass card with an icon, a title and a line, sliding in from the right as k goes 0..1024 */
static void toast(const struct mini *m, struct canvas *c, struct rect s, int slot, int k, enum icon_id icon,
                  const char *title, const char *text)
{
    if (k <= 0)
        return;
    int w = MIN(s.w * 44 / 100, 260), h = 52;
    int x = s.x + s.w - 12 - w + (1024 - smooth(k)) * (w + 20) / 1024;
    struct rect r = {x, s.y + 30 + slot * (h + 8), w, h};
    glass_with(m, c, r, 16, GLASS_MENU, 0);
    gfx_icon(c, r.x + 10, r.y + 10, icon, 32);
    gfx_text_box(c, (struct rect){r.x + 50, r.y + 9, r.w - 58, 16}, &font_bold, title, ink_of(m, 240), ALIGN_LEFT);
    gfx_text_box(c, (struct rect){r.x + 50, r.y + 28, r.w - 58, 16}, &font_small, text, ink_of(m, 180), ALIGN_LEFT);
}

static struct rect centered(struct rect s, int w, int h)
{
    return (struct rect){s.x + (s.w - w) / 2, s.y + (s.h - h) / 2, w, h};
}

static void caption(const struct mini *m, struct canvas *c, struct rect r, const char *text)
{
    gfx_text_box(c, r, &font_title, text, ink_of(m, 235), ALIGN_CENTER);
}

static void ripple(struct canvas *c, int cx, int cy, uint32_t ms, uint32_t period, int max_r, uint32_t color)
{
    for (int i = 0; i < 3; i++) {
        int t = (int)((ms + (uint32_t)i * period / 3) % period * 1024 / period);
        int r = max_r * t / 1024;
        int a = (1024 - t) * ((color >> 24) & 255) / 1024;
        if (r > 2)
            gfx_ring(c, cx, cy, r, (color & 0xffffffu) | (uint32_t)a << 24);
    }
}

/* the screen's pixels changed in place: gray, inverted */
static void filter_rect(struct canvas *c, struct rect r, int mode)
{
    struct rect a = rect_intersect(r, c->clip);
    for (int y = a.y; y < a.y + a.h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = a.x; x < a.x + a.w; x++) {
            uint32_t p = row[x];
            uint32_t rr = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
            if (mode == 1) {
                uint32_t l = (rr * 77 + g * 150 + b * 29) >> 8;
                rr = g = b = l;
            } else {
                rr = 255 - rr, g = 255 - g, b = 255 - b;
            }
            row[x] = (p & 0xff000000u) | rr << 16 | g << 8 | b;
        }
    }
}

static void scene(struct mini *m, struct canvas *c, struct rect s, const char *pane, uint32_t ms)
{
    int w = wave(ms, 6000);
    uint32_t loop = ms % 6000;
    int t = (int)(loop * 1024 / 6000);
    if (!strcmp(pane, "network")) {
        struct rect card = centered(s, 300, 120);
        glass_with(m, c, card, 22, GLASS_PANEL, 0);
        gfx_icon(c, card.x + 20, card.y + 34, ICON_COMPUTER, 52);
        gfx_icon(c, card.x + card.w - 72, card.y + 34, ICON_NETUTIL, 52);
        int x0 = card.x + 80, x1 = card.x + card.w - 80, y = card.y + 60;
        gfx_thick_line(c, x0, y, x1, y, 3, ink_of(m, 60));
        for (int i = 0; i < 4; i++) {
            int k = (int)((ms / 3 + (uint32_t)i * 160) % 640);
            int px = i % 2 ? x1 - (x1 - x0) * k / 640 : x0 + (x1 - x0) * k / 640;
            gfx_circle(c, px, y, 4, i % 2 ? RGB(77, 214, 195) : style.accent);
        }
        ripple(c, card.x + card.w - 46, card.y + 60, ms, 1800, 70, ALPHA(style.accent, 160));
    } else if (!strcmp(pane, "sound")) {
        struct rect hud = {s.x + 24, s.y + s.h / 2 - 34, s.w * 40 / 100, 68};
        glass_with(m, c, hud, 26, GLASS_MENU, 0);
        for (int i = 0; i < 18; i++) {
            int h = 8 + (wave(ms + (uint32_t)i * 170, 900 + (uint32_t)(i % 5) * 140) * 40 / 1024);
            int bx = hud.x + 20 + i * (hud.w - 40) / 18;
            gfx_round(c, (struct rect){bx, hud.y + (hud.h - h) / 2, 5, h}, 2, ALPHA(gfx_halo_tone(i * 56), 230));
        }
        toast(m, c, s, 0, mix(0, 1024, MIN(1024, t * 4)), ICON_MUSIC, "Now playing", "Your music, on every output");
        toast(m, c, s, 1, mix(0, 1024, CLAMP((t - 300) * 4, 0, 1024)), ICON_CLOCK, "Clock", "The timer is done");
    } else if (!strcmp(pane, "general")) {
        m->window = MIN(1024, (int)(ms % 5000) * 1024 / 1400);
        m->px = mix(800, 520, smooth((int)(ms % 5000) * 1024 / 2500));
        m->py = mix(900, 520, smooth((int)(ms % 5000) * 1024 / 2500));
    } else if (!strcmp(pane, "accessibility")) {
        int mode = (int)(ms / 3000 % 3);
        if (mode)
            filter_rect(c, (struct rect){s.x, s.y, s.w * t / 1024 + 1, s.h}, mode);
        m->zoom = mode ? 0 : w;
        m->px = 300 + w / 3;
        m->py = 500;
    } else if (!strcmp(pane, "appearance")) {
        struct rect half = {s.x, s.y, s.w * smooth(w) / 1024, s.h};
        struct canvas cl = *c;
        cl.clip = rect_intersect(c->clip, half);
        struct mini other = *m;
        other.dark = !m->dark;
        other.window = 1024;
        draw_screen(&other, &cl, s, ms);
        gfx_fill(c, (struct rect){s.x + half.w - 1, s.y, 2, s.h}, ALPHA(0xffffffffu, 200));
    } else if (!strcmp(pane, "lock")) {
        toast(m, c, s, 0, CLAMP((t - 200) * 3, 0, 1024), ICON_WEATHER, "Weather", "Clear tonight, 4 degrees");
        toast(m, c, s, 1, CLAMP((t - 450) * 3, 0, 1024), ICON_STORE, "App Store", "2 updates are ready");
    } else if (!strcmp(pane, "privacy")) {
        struct rect r = centered(s, 120, 140);
        int pulse = wave(ms, 2400);
        gfx_glow(c, r, 50, 30 + pulse / 40, 120, style.accent);
        gfx_icon(c, r.x + 10, r.y + 10, ICON_USER, 100);
        gfx_round(c, (struct rect){r.x + 72, r.y + 80, 44, 52}, 10, ALPHA(0xff20242c, 230));
        gfx_ring(c, r.x + 94, r.y + 82, 12, RGB(255, 255, 255));
        gfx_circle(c, r.x + 94, r.y + 108, 5, RGB(255, 210, 90));
    } else if (!strcmp(pane, "users")) {
        static const char *const names[] = {"You", "Family", "Guest"};
        for (int i = 0; i < 3; i++) {
            int k = CLAMP((t - i * 120) * 4, 0, 1024);
            int cx = s.x + s.w * (i + 1) / 4, y = s.y + s.h / 2 - 50 + (1024 - smooth(k)) * 30 / 1024;
            struct rect card = {cx - 56, y - 10, 112, 130};
            glass_with(m, c, card, 24, GLASS_PANEL, 0);
            gfx_icon_alpha(c, cx - 36, y, ICON_USER, 72, k * 255 / 1024);
            gfx_text_box(c, (struct rect){card.x, y + 82, card.w, 20}, &font_bold, names[i], ink_of(m, 230 * k / 1024),
                         ALIGN_CENTER);
        }
    } else if (!strcmp(pane, "devices")) {
        int k = smooth(CLAMP(t * 3, 0, 1024));
        int x = mix(s.x + 40, s.x + s.w / 2 - 110, k);
        gfx_icon(c, x, s.y + s.h / 2 - 32, ICON_USB, 64);
        gfx_icon(c, s.x + s.w / 2 - 20, s.y + s.h / 2 - 40, ICON_COMPUTER, 80);
        toast(m, c, s, 0, CLAMP((t - 380) * 4, 0, 1024), ICON_USB, "Device Connected", "USB drive, 64 GB");
    } else if (!strcmp(pane, "language")) {
        static const char *const hello[] = {"Hello", "Привет", "Hola", "Bonjour", "Hallo", "Ciao", "Olá", "Merhaba"};
        int i = (int)(ms / 1200 % ARRAY_SIZE(hello)), k = (int)(ms % 1200);
        int a = k < 200 ? k * 255 / 200 : k > 1000 ? (1200 - k) * 255 / 200 : 255;
        struct rect b = centered(s, 260, 90);
        glass_with(m, c, b, 45, GLASS_MENU, 0);
        gfx_text_box(c, (struct rect){b.x, b.y + 22 - (255 - a) / 20, b.w, 46}, &font_display, hello[i],
                     ink_of(m, a), ALIGN_CENTER);
    } else if (!strcmp(pane, "apps")) {
        static const enum icon_id grid[] = {ICON_FILES, ICON_BROWSER, ICON_PHOTOS, ICON_MUSIC, ICON_VIDEO, ICON_NOTES,
                                            ICON_CALC, ICON_CLOCK, ICON_WEATHER, ICON_STORE, ICON_CODE, ICON_GAMES};
        int cols = 6, size = MIN(56, s.h / 3 - 18), gap = size / 2;
        int gw = cols * size + (cols - 1) * gap, x0 = s.x + (s.w - gw) / 2, y0 = s.y + (s.h - 2 * size - gap) / 2;
        gfx_fill(c, s, ALPHA(0, 70));
        for (int i = 0; i < 12; i++) {
            int k = smooth(CLAMP((t - i * 30) * 5, 0, 1024));
            int sz = size * (600 + k * 424 / 1024) / 1024;
            int x = x0 + (i % cols) * (size + gap) + (size - sz) / 2, y = y0 + (i / cols) * (size + gap) + (size - sz) / 2;
            gfx_icon_alpha(c, x, y, grid[i], sz, k * 255 / 1024);
        }
    } else if (!strcmp(pane, "about")) {
        struct rect r = centered(s, 110, 110);
        gfx_glow(c, r, 40, 26 + wave(ms, 3000) / 64, 120, RGB(255, 160, 100));
        gfx_icon(c, r.x, r.y - 14, ICON_LOGO, 110);
        caption(m, c, (struct rect){s.x, r.y + r.h, s.w, 30}, "SynthOS");
    } else if (!strcmp(pane, "storage")) {
        struct rect bar = {s.x + 40, s.y + s.h / 2 - 14, s.w - 80, 28};
        glass_with(m, c, rect_inset(bar, -8), 22, GLASS_PANEL, 0);
        static const uint32_t parts[] = {0xff6a8dff, 0xff4dd6c3, 0xffffb347, 0xffff6b9a, 0xffa78bfa};
        static const int share[] = {26, 18, 12, 9, 7};
        int x = bar.x, k = smooth(CLAMP(t * 2, 0, 1024));
        for (int i = 0; i < 5; i++) {
            int wpart = bar.w * share[i] / 100 * k / 1024;
            gfx_round(c, (struct rect){x, bar.y, wpart, bar.h}, 6, parts[i]);
            x += wpart;
        }
        gfx_round_frame(c, bar, 8, ink_of(m, 80));
    } else if (!strcmp(pane, "datetime")) {
        struct rtc_time tm;
        time_local(&tm);
        int r = s.h * 36 / 100, cx = s.x + s.w / 2, cy = s.y + s.h / 2;
        glass_with(m, c, (struct rect){cx - r - 10, cy - r - 10, 2 * r + 20, 2 * r + 20}, r + 10, GLASS_PANEL, 0);
        int sec = (int)(ms / 1000 % 60);
        for (int i = 0; i <= sec; i++) { /* the seconds go round as dots */
            static const int sin60[15] = {0, 107, 213, 316, 416, 512, 602, 685, 761, 828, 887, 935, 974, 1002, 1018};
            int q = i % 15, quad = i / 15 % 4;
            int sx = q ? sin60[q] : 0, cxq = q ? sin60[15 - q] : 1024;
            int dx = quad == 0 ? sx : quad == 1 ? cxq : quad == 2 ? -sx : -cxq;
            int dy = quad == 0 ? -cxq : quad == 1 ? sx : quad == 2 ? cxq : -sx;
            gfx_circle(c, cx + dx * (r - 6) / 1024, cy + dy * (r - 6) / 1024, i == sec ? 4 : 2,
                       i == sec ? style.accent : ink_of(m, 120));
        }
        char hm[8];
        ksnprintf(hm, sizeof(hm), "%u:%02u", tm.hour, tm.minute);
        gfx_text_box(c, (struct rect){cx - r, cy - 24, 2 * r, 48}, &font_display, hm, ink_of(m, 240), ALIGN_CENTER);
    } else if (!strcmp(pane, "update")) {
        int cx = s.x + s.w / 2, cy = s.y + s.h / 2, r = s.h / 4;
        glass_with(m, c, (struct rect){cx - r - 18, cy - r - 18, 2 * r + 36, 2 * r + 36}, r + 18, GLASS_PANEL, 0);
        int k = smooth(t);
        for (int i = 0; i < 48 * k / 1024; i++) {
            static const int sin12[13] = {0, 133, 265, 392, 512, 623, 724, 812, 887, 946, 989, 1015, 1024};
            int q = i % 12, quad = i / 12 % 4, sx = sin12[q], co = sin12[12 - q];
            int dx = quad == 0 ? sx : quad == 1 ? co : quad == 2 ? -sx : -co;
            int dy = quad == 0 ? -co : quad == 1 ? sx : quad == 2 ? co : -sx;
            gfx_circle(c, cx + dx * r / 1024, cy + dy * r / 1024, 4, gfx_halo_tone(i * 21));
        }
        gfx_icon(c, cx - 26, cy - 26, ICON_INSTALLER, 52);
    } else if (!strcmp(pane, "keyboard")) {
        static const char *const rows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        static const char typed[] = "SYNTHOS";
        char lit = typed[ms / 450 % (sizeof(typed) - 1)];
        int key = MIN(34, (s.w - 60) / 11), y = s.y + (s.h - 3 * key - 16) / 2;
        for (int rI = 0; rI < 3; rI++) {
            int n = (int)strlen(rows[rI]), x = s.x + (s.w - n * (key + 6)) / 2;
            for (int i = 0; i < n; i++) {
                struct rect k = {x + i * (key + 6), y + rI * (key + 8), key, key};
                bool on = rows[rI][i] == lit;
                glass_control(c, on ? rect_inset(k, 1) : k, 8, GLASS_CONTROL, on ? style.accent : 0, on ? 0 : 1);
                char ch[2] = {rows[rI][i], 0};
                gfx_text_box(c, k, &font_bold, ch, on ? RGB(255, 255, 255) : ink_of(m, 220), ALIGN_CENTER);
            }
        }
    } else if (!strcmp(pane, "mouse")) {
        static const int path[][2] = {{200, 300}, {700, 260}, {820, 620}, {380, 720}, {200, 300}};
        int seg = (int)(ms / 1200 % 4), k = smooth((int)(ms % 1200) * 1024 / 1200);
        m->px = mix(path[seg][0], path[seg + 1][0], k);
        m->py = mix(path[seg][1], path[seg + 1][1], k);
        if (k > 900)
            ripple(c, s.x + m->px * s.w / 1024, s.y + m->py * s.h / 1024, ms, 600, 26, ALPHA(style.accent, 200));
    } else if (!strcmp(pane, "terminal")) {
        struct rect win = centered(s, s.w * 70 / 100, s.h * 70 / 100);
        gfx_drop_shadow(c, win, 14, 16, 90, 6);
        gfx_round(c, win, 14, ALPHA(0xff10131a, 240));
        static const char *const lines[] = {"root@synth ~ > uname", "SynthOS 1.2 Nebula x86-64", "root@synth ~ > ls /mnt",
                                            "hdcp1/  hdcp2/", "root@synth ~ > _"};
        int shown = (int)(ms / 700 % 7);
        for (int i = 0; i < 5 && i < shown; i++)
            gfx_text(c, win.x + 16, win.y + 18 + i * 22, &font_mono, lines[i],
                     i % 2 ? RGB(200, 206, 220) : RGB(77, 255, 176));
    } else if (!strcmp(pane, "screenshot")) {
        int k = smooth(CLAMP(t * 2, 0, 1024));
        struct rect sel = {s.x + s.w / 5, s.y + s.h / 5, (s.w * 3 / 5) * k / 1024, (s.h * 3 / 5) * k / 1024};
        gfx_fill(c, s, ALPHA(0, 80));
        gfx_frame(c, sel, RGB(255, 255, 255));
        m->px = (sel.x + sel.w - s.x) * 1024 / MAX(1, s.w);
        m->py = (sel.y + sel.h - s.y) * 1024 / MAX(1, s.h);
        if (t > 560 && t < 640)
            gfx_fill(c, sel, ALPHA(0xffffffffu, 160));
        if (t >= 640)
            toast(m, c, s, 0, CLAMP((t - 640) * 5, 0, 1024), ICON_SCREENSHOT, "Screenshot", "Saved to Pictures");
    } else if (!strcmp(pane, "ai")) {
        int cx = s.x + s.w / 2, cy = s.y + s.h / 2 - 8, r = MIN(s.h / 4, 46);
        int pulse = wave(ms, 1600);
        gfx_glow(c, (struct rect){cx - r, cy - r, 2 * r, 2 * r}, r, r / 2 + pulse * r / 2048, 150,
                 gfx_halo_tone((int)(ms / 8)));
        for (int i = 0; i < 6; i++) { /* colors chasing each other inside the drop */
            int a = (int)(ms / 6 + (uint32_t)i * 171) % 1024;
            int q = a / 256, f = a % 256, sx = f * 4, co = 1024 - sx;
            int dx = q == 0 ? sx : q == 1 ? co : q == 2 ? -sx : -co, dy = q == 0 ? -co : q == 1 ? sx : q == 2 ? co : -sx;
            gfx_circle(c, cx + dx * r / 2048, cy + dy * r / 2048, r * 2 / 3, ALPHA(gfx_halo_tone(i * 170), 120));
        }
        gfx_ring(c, cx, cy, r, ALPHA(0xffffffffu, 140));
        gfx_circle(c, cx - r / 3, cy - r / 3, r / 5, ALPHA(0xffffffffu, 110));
        static const char *const asks[] = {"How can I help?", "Make the screen warmer", "Open my photos from May",
                                           "What is on the calendar?"};
        const char *ask = asks[ms / 2400 % 4];
        int k = (int)(ms % 2400), a = k < 300 ? k * 255 / 300 : k > 2100 ? (2400 - k) * 255 / 300 : 255;
        int bw = gfx_text_width(&font_sans, ask) + 32;
        struct rect b = {cx - bw / 2, cy + r + 12, bw, 30};
        glass_with(m, c, b, 15, GLASS_MENU, 0);
        gfx_text_box(c, b, &font_sans, ask, ink_of(m, a), ALIGN_CENTER);
    } else if (!strcmp(pane, "dock") || !strcmp(pane, "displays")) {
        /* acted on the screen itself below */
    }
}

static void hero(struct canvas *c, struct rect r, const char *pane, uint32_t ms)
{
    char name[24];
    ksnprintf(name, sizeof(name), "h_%s", pane);
    if (clip_draw(c, r, name, ms, 16, 255)) {
        gfx_round_frame(c, r, 16, ALPHA(0xffffffffu, style.dark ? 30 : 60));
        return;
    }
    struct mini m;
    mini_now(&m);
    m.window = 1024;
    bool quiet = strcmp(pane, "general") && strcmp(pane, "appearance");
    if (quiet) /* the scene is the subject: no window in front of it */
        m.window = 0;
    struct rect s = r;
    if (!strcmp(pane, "dock")) { /* closer to the Dock, the pointer running along it */
        m.magnify = 1024;
        m.bounce = 1024;
        m.px = 340 + (int)(ms / 700 % 6) * 60;
        m.py = 950;
        s = (struct rect){r.x - r.w / 2, r.y - r.h * 3 / 2, r.w * 2, r.h * 5 / 2};
    } else if (!strcmp(pane, "displays")) {
        m.bright = mix(100, 45, wave(ms, 6000));
        m.warm = mix(0, 70, wave(ms + 3000, 6000));
    } else if (!strcmp(pane, "lock")) {
        m.lock = 1024;
    }
    struct canvas cl = *c;
    cl.clip = rect_intersect(c->clip, r);
    struct canvas tile;
    if (!canvas_alloc(&tile, r.w, r.h))
        return;
    tile.clip = (struct rect){0, 0, r.w, r.h};
    gfx_fill(&tile, tile.clip, RGB(0, 0, 0));
    struct rect local = {s.x - r.x, s.y - r.y, s.w, s.h};
    struct rect whole = {0, 0, r.w, r.h};
    /* the scene's own pointer is drawn after it */
    int px = m.px, py = m.py;
    m.px = -1;
    draw_screen(&m, &tile, local, ms);
    scene(&m, &tile, whole, pane, ms);
    if (m.zoom > 0 || m.orb > 0 || m.window > 0 || m.px >= 0) { /* what the scene asked of the screen */
        if (m.px < 0)
            m.px = px, m.py = py;
        if (m.zoom > 0 || m.orb > 0 || (m.window > 0 && m.window < 1024))
            draw_screen(&m, &tile, local, ms);
        else if (m.px >= 0)
            pointer(&tile, m.px * r.w / 1024, m.py * r.h / 1024, MAX(8, r.h / 14));
    } else if (px >= 0) {
        pointer(&tile, (local.x + px * local.w / 1024), (local.y + py * local.h / 1024), MAX(8, r.h / 14));
    }
    gfx_blit_round(&cl, r, &tile, 16, false);
    canvas_free(&tile);
    gfx_round_frame(c, r, 16, ALPHA(0xffffffffu, style.dark ? 30 : 60));
}

static const char *const shown_keys[] = {"dockhalo", "iconshape", "dockspot", "dockfx", "colorfilter", "appearance", "glass", "glasseffect", "glassstrength", "glassedge",
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
           !strcmp(key, "dockbounce") || !strcmp(key, "dockdots") || !strncmp(key, "dock", 4);
}

/* before, then after, then back: 1.4 s each with a crossfade between */
static void setting(struct canvas *c, struct rect r, const char *key, const char *before, const char *after,
                    uint32_t ms)
{
    uint32_t t = ms % 4000;
    int k = t < 1300 ? 0 : t < 1800 ? smooth((int)(t - 1300) * 1024 / 500) : t < 3500 ? 1024
                                                                                     : 1024 - smooth((int)(t - 3500) * 1024 / 500);
    char was[24], now[24];
    value_clip(was, sizeof(was), key, before);
    value_clip(now, sizeof(now), key, after);
    if (clip_find(now, &(const uint8_t *){0}, &(uint32_t){0})) {
        bool drawn = k < 1024 && clip_draw(c, r, was, ms, 6, 255);
        if (k > 0 || !drawn)
            clip_draw(c, r, now, ms, 6, drawn ? k * 255 / 1024 : 255);
        gfx_round_frame(c, r, 6, ALPHA(0, 90));
        gfx_circle(c, r.x + r.w - 7, r.y + r.h - 7, 3, ALPHA(0, 120));
        gfx_circle(c, r.x + r.w - 7, r.y + r.h - 7, 2, k > 512 ? style.accent : RGB(200, 200, 206));
        return;
    }
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
        if (!strcmp(key, "magnify") || !strcmp(key, "dockbounce") || !strcmp(key, "dockspot"))
            m.px = 360 + (int)(ms / 700 % 6) * 56, m.py = 940;
        draw_screen(&m, &tile, zoomed(key) ? (struct rect){-r.w * 3 / 4, -r.h * 3 / 2, r.w * 5 / 2, r.h * 5 / 2} : local,
                    ms);
        struct canvas out = *c;
        out.clip = rect_intersect(c->clip, r);
        if (m.filter)
            filter_rect(&tile, local, m.filter);
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

/* ---- the first-start assistant's own pages */

static bool setting_on(const char *name)
{
    char v[16] = "";
    gui_setting_get(name, v, sizeof(v));
    return !strcmp(v, "on");
}

static void put(const char *name, const char *value)
{
    setting_apply(shell_current(), name, value);
}

static void row_switch(struct ui *ui, struct rect g, int i, int n, const char *label, const char *detail,
                       const char *name)
{
    struct rect row = {g.x, g.y + i * 36, g.w, 36};
    ui_group_label(ui, row, label, detail ? detail : "", i == n - 1);
    bool on = setting_on(name);
    if (ui_switch(ui, (struct rect){row.x + row.w - 50, row.y + 7, 36, 22}, &on))
        put(name, on ? "on" : "off");
}

static void row_choice(struct ui *ui, struct rect r, int y, const char *label, const char *name,
                       const char *const *labels, const char *const *values, int n)
{
    char v[24] = "";
    gui_setting_get(name, v, sizeof(v));
    int sel = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(v, values[i]))
            sel = i;
    ui_label(ui, (struct rect){r.x, y + 6, 150, 20}, label, &font_sans, style.text, ALIGN_LEFT);
    if (ui_segmented(ui, (struct rect){r.x + 160, y, r.w - 160, 30}, labels, n, &sel))
        put(name, values[sel]);
}

static void heading(struct ui *ui, struct rect r, const char *text, const char *sub)
{
    ui_label(ui, (struct rect){r.x, r.y, r.w, 30}, text, &font_title, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 32, r.w, 18}, sub, &font_sans, style.secondary, ALIGN_CENTER);
}

static void setup_page(struct ui *ui, struct rect r, const char *page)
{
    int y = r.y + 62;
    if (!strcmp(page, "effects")) {
        heading(ui, r, "Effects", "The glass, the menu bar and the light of the Dock.");
        static const char *const fx[] = {"Regular", "Clear", "Tinted", "Smoky", "Prism", "Halo"};
        static const char *const fx_values[] = {"regular", "clear", "tinted", "smoky", "prism", "halo"};
        row_choice(ui, r, y, "Glass", "glasseffect", fx, fx_values, 6);
        static const char *const bars[] = {"Menu bar", "Status pill", "Islands"};
        static const char *const bar_values[] = {"menubar", "pill", "islands"};
        row_choice(ui, r, y + 40, "Top of the screen", "statusbar", bars, bar_values, 3);
        struct rect g = ui_group(ui, r.x, y + 84, r.w, 3, 36);
        row_switch(ui, g, 0, 3, "Icon colors under the icons", NULL, "dockhalo");
        row_switch(ui, g, 1, 3, "Magnification", NULL, "magnify");
        row_switch(ui, g, 2, 3, "Light follows the pointer", NULL, "dockspot");
        static const char *const st[] = {"Subtle", "Normal", "Vivid"};
        static const char *const st_values[] = {"subtle", "normal", "vivid"};
        row_choice(ui, r, y + 84 + 3 * 36 + 12, "Dock effects", "dockfx", st, st_values, 3);
        static const char *const sh[] = {"Squircle", "Rounded", "Circle", "Square", "Leaf"};
        static const char *const sh_values[] = {"squircle", "rounded", "circle", "square", "leaf"};
        row_choice(ui, r, y + 84 + 3 * 36 + 52, "Icon shape", "iconshape", sh, sh_values, 5);
    } else if (!strcmp(page, "ai")) {
        heading(ui, r, "Assistant", "It runs on this computer: nothing you say or type leaves it.");
        struct rect g = ui_group(ui, r.x, y, r.w, 3, 36);
        row_switch(ui, g, 0, 3, "Use AI: the assistant and Tab completion", NULL, "ai");
        row_switch(ui, g, 1, 3, "Listen for \"Hey Synth\"", NULL, "aiwake");
        row_switch(ui, g, 2, 3, "Say answers aloud (an 860 MB voice)", NULL, "aispeak");
        y += 3 * 36 + 14;
        static const char *const in[] = {"Smart", "Voice", "Text", "Both"};
        static const char *const in_values[] = {"smart", "voice", "text", "both"};
        row_choice(ui, r, y, "Ask by", "aiinput", in, in_values, 4);
        static const char *const st[] = {"Friendly", "Brief", "Detailed"};
        static const char *const st_values[] = {"friendly", "brief", "detailed"};
        row_choice(ui, r, y + 40, "Style", "aistyle", st, st_values, 3);
        static const char *const al[] = {"Ask first", "Look around", "Anything"};
        static const char *const al_values[] = {"ask", "look", "all"};
        row_choice(ui, r, y + 80, "May do alone", "aiallow", al, al_values, 3);
    }
}

static const struct settings_preview service = {hero, shows, setting, setup_page};
static const struct module_ext ext = {NULL, 0, NULL, NULL, NULL, NULL, NULL, "settings-preview", &service};
GUI_APP_FULL(NULL, NULL, &ext);
