#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/net.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

/* Control Center and the menu bar modules (Sound, Network, Display, Focus): the switches people reach for often.
   Everything here goes through gui_setting, the same settings System Settings shows. */

#define CC_W 340
#define MODULE_W 290
#define TILE_R 16

struct control {
    struct ui_state st;
    enum control_module mode;
    bool editing;
    int drag, grab_x, grab_y, drag_x, drag_y;
};

static struct window *control_win;
static enum control_module control_mode;

static uint32_t tile_color(void)
{
    return style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 150);
}

static struct rect tile(struct ui *ui, int x, int y, int w, int h)
{
    struct rect r = {x, y, w, h};
    if (ui->c)
        gfx_round(ui->c, r, TILE_R, tile_color());
    return r;
}

static bool pressed(struct ui *ui, struct rect r)
{
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui->e->button == BUTTON_LEFT && ui_hover(ui, r)) {
        ui->consumed = true;
        return true;
    }
    return false;
}

/* A round button that is filled with the accent color while its feature is on, a name and a line of state. */
static bool circle_toggle(struct ui *ui, int x, int y, int w, enum symbol_id sym, bool on, const char *label,
                          const char *detail)
{
    struct rect hit = {x, y, w, 36};
    if (ui->c) {
        int cx = x + 16, cy = y + 18;
        bool hot = ui_hover(ui, hit);
        uint32_t back = on ? style.accent : style.dark ? ALPHA(0xffffffffu, hot ? 60 : 40) : ALPHA(0, hot ? 34 : 22);
        gfx_circle(ui->c, cx, cy, 15, back);
        gfx_symbol(ui->c, cx - 8, cy - 8, sym, 16, on ? RGB(255, 255, 255) : style.text);
        gfx_text_box(ui->c, (struct rect){x + 38, detail ? y + 1 : y + 9, w - 40, 18}, &font_bold, label, style.text,
                     ALIGN_LEFT);
        if (detail)
            gfx_text_box(ui->c, (struct rect){x + 38, y + 18, w - 40, 16}, &font_small, detail, style.secondary,
                         ALIGN_LEFT);
    }
    return pressed(ui, hit);
}

/* A row like a menu item at the bottom of a module: it opens a pane of System Settings. */
static bool settings_row(struct ui *ui, int y, int w, const char *label)
{
    struct rect r = {8, y, w - 16, 28};
    if (ui->c) {
        if (ui_hover(ui, r))
            gfx_round(ui->c, r, 7, style.dark ? ALPHA(0xffffffffu, 30) : ALPHA(0, 18));
        gfx_text_box(ui->c, (struct rect){r.x + 10, r.y, r.w - 20, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
    }
    return pressed(ui, r);
}

static void separator(struct ui *ui, int y, int w)
{
    if (ui->c)
        gfx_fill(ui->c, (struct rect){16, y, w - 32, 1}, style.separator);
}

static void open_settings(struct ui *ui, const char *pane)
{
    ui->win = NULL;
    wm_close_popups();
    gui_app_launch(gui_app_find("System Settings"), pane);
}

static void set_number(const char *name, int v)
{
    char buf[12];
    ksnprintf(buf, sizeof(buf), "%d", v);
    gui_setting(name, buf);
}

/* ---- state shown in several places */

static const char *network_state(char *buf, size_t cap)
{
    if (!net_enabled())
        return "Off";
    if (!net_interface_count())
        return "No hardware";
    struct net_iface *ifc = net_interface(0);
    struct netdev *d = ifc->dev;
    if (d->link && !d->link(d))
        return "Cable unplugged";
    if (!ifc->configured)
        return "Connecting…";
    char ip[16];
    net_format_ip(ifc->ip, ip, sizeof(ip));
    ksnprintf(buf, cap, "Ethernet · %s", ip);
    return buf;
}

static const char *night_state(void)
{
    char v[8];
    gui_setting_get("nightshift", v, sizeof(v));
    return !strcmp(v, "auto") ? (display_night() ? "On until 7:00" : "Off until 21:00") : !strcmp(v, "on") ? "On"
                                                                                                          : "Off";
}

static bool night_on(void)
{
    char v[8];
    gui_setting_get("nightshift", v, sizeof(v));
    return strcmp(v, "off") != 0;
}

static void sound_slider(struct ui *ui, struct rect r)
{
    int v = (int)gui_setting_number("volume", 100);
    bool muted = gui_setting_on("mute");
    if (muted)
        v = 0;
    if (ui_capsule_slider(ui, r, muted || !v ? SYM_MUTE : SYM_VOLUME, &v, 0, 100)) {
        if (muted && v)
            gui_setting("mute", "off");
        set_number("volume", v);
    }
}

static void brightness_slider(struct ui *ui, struct rect r)
{
    int v = display_brightness();
    if (ui_capsule_slider(ui, r, SYM_SUN, &v, 10, 100))
        set_number("brightness", v);
}

/* ---- Control Center: tiles on a grid of four columns, in the order and sizes the ccitems setting keeps */

#define CELL_W 70
#define CELL_H 62
#define GAP 12
#define MAX_ITEMS 16
#define GRID_ROWS 24

enum cc_kind { K_CONNECT, K_NETWORK, K_DARK, K_NIGHT, K_FOCUS, K_SCREENSHOT, K_MISSION, K_LOCK, K_DISPLAY,
               K_SOUND, K_MEDIA, K_COUNT };

static const struct {
    const char *key, *label, *wide;
    uint8_t sizes[3]; /* 0xWH, the first is the size a new tile gets */
} kinds[K_COUNT] = {
    [K_CONNECT] = {"connect", "Connectivity", "Connectivity", {0x22, 0x41}},
    [K_NETWORK] = {"network", "Network", "Network", {0x21, 0x11}},
    [K_DARK] = {"dark", "Dark", "Dark Mode", {0x11, 0x21}},
    [K_NIGHT] = {"night", "Night", "Night Shift", {0x11, 0x21}},
    [K_FOCUS] = {"focus", "Focus", "Focus", {0x21, 0x11}},
    [K_SCREENSHOT] = {"screenshot", "Screenshot", "Screenshot", {0x11, 0x21}},
    [K_MISSION] = {"mission", "Mission", "Mission Control", {0x11, 0x21}},
    [K_LOCK] = {"lock", "Lock", "Lock Screen", {0x11, 0x21}},
    [K_DISPLAY] = {"display", "Display", "Display", {0x41, 0x21}},
    [K_SOUND] = {"sound", "Sound", "Sound", {0x41, 0x21}},
    [K_MEDIA] = {"media", "Music", "Now Playing", {0x41, 0x21}},
};

static const char default_items[] =
    "connect:2x2,focus:2x1,screenshot:1x1,mission:1x1,display:4x1,sound:4x1,media:4x1";

struct cc_item {
    uint8_t kind, w, h;
};

static struct cc_item items[MAX_ITEMS];
static int item_count = -1;
static bool hide_labels;

static bool size_allowed(int kind, int w, int h)
{
    for (int i = 0; i < 3 && kinds[kind].sizes[i]; i++)
        if (kinds[kind].sizes[i] == (w << 4 | h))
            return true;
    return false;
}

static int parse_items(const char *v, struct cc_item *out)
{
    int n = 0;
    if (!strcmp(v, "none"))
        return 0;
    for (const char *p = v; *p;) {
        const char *e = p, *colon;
        while (*e && *e != ',')
            e++;
        for (colon = p; colon < e && *colon != ':'; colon++)
            ;
        size_t len = (size_t)(colon - p);
        int k = 0;
        while (k < K_COUNT && (strlen(kinds[k].key) != len || strncmp(kinds[k].key, p, len)))
            k++;
        if (k == K_COUNT || n == MAX_ITEMS)
            return E_INVAL;
        int w = kinds[k].sizes[0] >> 4, h = kinds[k].sizes[0] & 15;
        if (colon < e) {
            if (e - colon != 4 || colon[2] != 'x')
                return E_INVAL;
            w = colon[1] - '0';
            h = colon[3] - '0';
        }
        if (!size_allowed(k, w, h))
            return E_INVAL;
        for (int i = 0; i < n; i++)
            if (out[i].kind == k)
                return E_INVAL;
        out[n++] = (struct cc_item){(uint8_t)k, (uint8_t)w, (uint8_t)h};
        p = *e ? e + 1 : e;
    }
    return n;
}

static void ensure_items(void)
{
    if (item_count < 0)
        item_count = parse_items(default_items, items);
}

static void get_items(char *buf, size_t cap)
{
    ensure_items();
    buf[0] = 0;
    for (int i = 0; i < item_count; i++) {
        char one[24];
        ksnprintf(one, sizeof(one), "%s%s:%dx%d", i ? "," : "", kinds[items[i].kind].key, items[i].w, items[i].h);
        str_append(buf, one, cap);
    }
    if (!buf[0])
        str_copy(buf, "none", cap);
}

static void relayout(void);

static int set_items(struct shell *sh, const char *value)
{
    UNUSED(sh);
    struct cc_item tmp[MAX_ITEMS];
    int n = !strcmp(value, "default") ? parse_items(default_items, tmp) : parse_items(value, tmp);
    if (n < 0)
        return n;
    memcpy(items, tmp, sizeof(tmp));
    item_count = n;
    relayout();
    return 0;
}

static const struct setting s_items = {"ccitems", "default|none|<tile>:<w>x<h>,...",
                                       "Control Center tiles: connect, network, dark, night, focus, screenshot, "
                                       "mission, lock, display, sound, media",
                                       get_items, set_items};
SETTING(s_items);

static void save_items(void)
{
    char buf[256];
    item_count = item_count < 0 ? 0 : item_count;
    get_items(buf, sizeof(buf));
    gui_setting("ccitems", buf);
}

/* Each tile takes the first free place, row by row, like icons on a phone's home screen. */
static int layout(struct rect *out)
{
    uint8_t used[GRID_ROWS] = {0};
    int rows = 0;
    ensure_items();
    for (int i = 0; i < item_count; i++) {
        int w = items[i].w, h = items[i].h;
        unsigned mask = (1u << w) - 1;
        out[i] = (struct rect){0, 0, 0, 0};
        for (int r = 0; r + h <= GRID_ROWS && !out[i].w; r++)
            for (int c = 0; c + w <= 4 && !out[i].w; c++) {
                bool free = true;
                for (int y = r; y < r + h; y++)
                    free = free && !(used[y] & (mask << c));
                if (!free)
                    continue;
                for (int y = r; y < r + h; y++)
                    used[y] |= (uint8_t)(mask << c);
                out[i] = (struct rect){12 + c * (CELL_W + GAP), 12 + r * (CELL_H + GAP), w * CELL_W + (w - 1) * GAP,
                                       h * CELL_H + (h - 1) * GAP};
                rows = MAX(rows, r + h);
            }
    }
    return rows;
}

static bool has_kind(int k)
{
    for (int i = 0; i < item_count; i++)
        if (items[i].kind == k)
            return true;
    return false;
}

static int missing_kinds(void)
{
    int n = 0;
    for (int k = 0; k < K_COUNT; k++)
        n += !has_kind(k);
    return n;
}

static int full_height(bool editing)
{
    struct rect r[MAX_ITEMS];
    int h = 12 + layout(r) * (CELL_H + GAP) + 30 + 12;
    int missing = missing_kinds();
    if (editing && missing)
        h += 26 + (missing + 1) / 2 * 38;
    return h;
}

static void now_playing(struct ui *ui, struct rect t)
{
    const struct media_track *m = media_track();
    bool wide = t.w > 200, playing = m && m->playing;
    int art = wide ? 40 : 36;
    if (ui->c) {
        struct rect a = {t.x + 12, t.y + (t.h - art) / 2, art, art};
        gfx_round_gradient(ui->c, a, 9, RGB(252, 92, 125), RGB(106, 130, 251));
        gfx_symbol(ui->c, a.x + (art - 16) / 2, a.y + (art - 16) / 2, SYM_MUSIC, 16, RGB(255, 255, 255));
        int tx = a.x + art + 10, tw = t.x + t.w - tx - (wide ? 100 : 40);
        gfx_text_box(ui->c, (struct rect){tx, t.y + t.h / 2 - 16, tw, 18}, &font_bold,
                     m && m->title ? m->title : "Not Playing", style.text, ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){tx, t.y + t.h / 2 + 2, tw, 16}, &font_small,
                     m && m->artist ? m->artist : "Music", style.secondary, ALIGN_LEFT);
    }
    struct rect play = {t.x + t.w - (wide ? 62 : 38), t.y + (t.h - 28) / 2, 28, 28};
    struct rect prev = {play.x - 30, play.y, 28, 28}, next = {play.x + 30, play.y, 28, 28};
    if (ui->c) {
        gfx_symbol(ui->c, play.x + 6, play.y + 6, playing ? SYM_PAUSE : SYM_PLAY, 16, style.text);
        if (wide) {
            gfx_symbol(ui->c, prev.x + 6, prev.y + 6, SYM_PREV, 16, style.text);
            gfx_symbol(ui->c, next.x + 6, next.y + 6, SYM_NEXT, 16, style.text);
        }
    }
    if (pressed(ui, play))
        media_toggle();
    else if (wide && pressed(ui, prev))
        media_prev();
    else if (wide && pressed(ui, next))
        media_next();
    else if (pressed(ui, (struct rect){t.x, t.y, play.x - t.x - (wide ? 32 : 2), t.h})) {
        ui->win = NULL;
        wm_close_popups();
        gui_app_launch(gui_app_find("Music"), NULL);
    }
}

/* A one-cell tile: the symbol in a circle (filled while the feature is on) over its name. */
static bool small_tile(struct ui *ui, struct rect t, enum symbol_id sym, bool round, bool on, const char *label)
{
    if (ui->c) {
        int cx = t.x + t.w / 2, cy = t.y + 22;
        if (round) {
            bool hot = ui_hover(ui, t);
            gfx_circle(ui->c, cx, cy, 15,
                       on ? style.accent : style.dark ? ALPHA(0xffffffffu, hot ? 60 : 40) : ALPHA(0, hot ? 34 : 22));
        }
        gfx_symbol(ui->c, cx - 8, cy - 8, sym, 16, on ? RGB(255, 255, 255) : style.text);
        if (!hide_labels)
            gfx_text_box(ui->c, (struct rect){t.x + 2, t.y + 41, t.w - 4, 16}, &font_small, label, style.text,
                         ALIGN_CENTER);
    }
    return pressed(ui, t);
}

static bool toggle_tile(struct ui *ui, struct rect t, int kind, enum symbol_id sym, bool on, const char *detail)
{
    if (t.w < 100)
        return small_tile(ui, t, sym, true, on, kinds[kind].label);
    return circle_toggle(ui, t.x + 8, t.y + (t.h - 36) / 2, t.w - 12, sym, on, kinds[kind].wide, detail);
}

static void run_action(struct ui *ui, int kind)
{
    ui->win = NULL;
    wm_close_popups();
    if (kind == K_SCREENSHOT)
        screenshot_start(SHOT_PANEL);
    else if (kind == K_MISSION)
        mission_toggle();
    else
        gui_lock();
}

static void slider_tile(struct ui *ui, struct rect t, int kind)
{
    struct rect s = {t.x + 12, t.y + 30, t.w - 24, 24};
    if (t.w > 200)
        ui_label(ui, (struct rect){t.x + 14, t.y + 6, 200, 18}, kinds[kind].label, &font_bold, style.text,
                 ALIGN_LEFT);
    else
        s.y = t.y + (t.h - 24) / 2;
    if (kind == K_DISPLAY)
        brightness_slider(ui, s);
    else
        sound_slider(ui, s);
}

static void draw_tile(struct ui *ui, struct rect r, int kind)
{
    char buf[48];
    struct rect t = tile(ui, r.x, r.y, r.w, r.h);
    bool net = net_enabled(), dnd = gui_setting_on("dnd");
    switch (kind) {
    case K_CONNECT:
        if (t.h > CELL_H) {
            if (circle_toggle(ui, t.x + 8, t.y + 8, t.w - 12, net ? SYM_WIFI : SYM_WIFI_OFF, net, "Network",
                              network_state(buf, sizeof(buf))))
                gui_setting("network", net ? "off" : "on");
            if (circle_toggle(ui, t.x + 8, t.y + 50, t.w - 12, SYM_APPEARANCE, style.dark, "Dark Mode",
                              style.dark ? "On" : "Off"))
                gui_setting("appearance", style.dark ? "light" : "dark");
            if (circle_toggle(ui, t.x + 8, t.y + 92, t.w - 12, SYM_SUN, night_on(), "Night Shift", night_state()))
                gui_setting("nightshift", night_on() ? "off" : "on");
        } else {
            int w3 = t.w / 3;
            if (small_tile(ui, (struct rect){t.x, t.y, w3, t.h}, net ? SYM_WIFI : SYM_WIFI_OFF, true, net,
                           "Network"))
                gui_setting("network", net ? "off" : "on");
            if (small_tile(ui, (struct rect){t.x + w3, t.y, w3, t.h}, SYM_APPEARANCE, true, style.dark, "Dark"))
                gui_setting("appearance", style.dark ? "light" : "dark");
            if (small_tile(ui, (struct rect){t.x + 2 * w3, t.y, t.w - 2 * w3, t.h}, SYM_SUN, true, night_on(),
                           "Night Shift"))
                gui_setting("nightshift", night_on() ? "off" : "on");
        }
        break;
    case K_NETWORK:
        if (toggle_tile(ui, t, kind, net ? SYM_WIFI : SYM_WIFI_OFF, net, network_state(buf, sizeof(buf))))
            gui_setting("network", net ? "off" : "on");
        break;
    case K_DARK:
        if (toggle_tile(ui, t, kind, SYM_APPEARANCE, style.dark, style.dark ? "On" : "Off"))
            gui_setting("appearance", style.dark ? "light" : "dark");
        break;
    case K_NIGHT:
        if (toggle_tile(ui, t, kind, SYM_SUN, night_on(), night_state()))
            gui_setting("nightshift", night_on() ? "off" : "on");
        break;
    case K_FOCUS:
        if (toggle_tile(ui, t, kind, SYM_MOON, dnd, dnd ? "Do Not Disturb" : "Off"))
            gui_setting("dnd", dnd ? "off" : "on");
        break;
    case K_SCREENSHOT:
    case K_MISSION:
    case K_LOCK: {
        enum symbol_id sym = kind == K_SCREENSHOT ? SYM_SCREENSHOT : kind == K_MISSION ? SYM_MISSION : SYM_LOCK;
        bool hit = t.w < 100 ? small_tile(ui, t, sym, false, false, kinds[kind].label)
                             : circle_toggle(ui, t.x + 8, t.y + (t.h - 36) / 2, t.w - 12, sym, false,
                                             kinds[kind].wide, NULL);
        if (hit)
            run_action(ui, kind);
        break;
    }
    case K_DISPLAY:
    case K_SOUND: slider_tile(ui, t, kind); break;
    case K_MEDIA: now_playing(ui, t); break;
    }
}

static struct rect remove_badge(struct rect t)
{
    return (struct rect){t.x - 4, t.y - 4, 20, 20};
}

static struct rect size_badge(struct rect t)
{
    if (t.w < 100)
        return (struct rect){t.x + (t.w - 32) / 2, t.y + t.h - 20, 32, 16};
    return (struct rect){t.x + t.w - 36, t.y + 4, 32, 16};
}

static void cycle_size(int i)
{
    int k = items[i].kind, cur = 0;
    while (cur < 3 && kinds[k].sizes[cur] != (items[i].w << 4 | items[i].h))
        cur++;
    int next = cur + 1 < 3 && kinds[k].sizes[cur + 1] ? cur + 1 : 0;
    items[i].w = kinds[k].sizes[next] >> 4;
    items[i].h = kinds[k].sizes[next] & 15;
}

static void move_item(int from, int to)
{
    struct cc_item it = items[from];
    if (to > from)
        memmove(&items[from], &items[from + 1], (size_t)(to - from) * sizeof(it));
    else
        memmove(&items[to + 1], &items[to], (size_t)(from - to) * sizeof(it));
    items[to] = it;
}

static void edit_badges(struct ui *ui, struct rect t, int i)
{
    if (!ui->c)
        return;
    gfx_round_frame(ui->c, t, TILE_R, ALPHA(style.accent, 160));
    struct rect m = remove_badge(t);
    gfx_circle(ui->c, m.x + 10, m.y + 10, 9, style.error);
    gfx_fill(ui->c, (struct rect){m.x + 6, m.y + 9, 8, 2}, RGB(255, 255, 255));
    int k = items[i].kind;
    if (kinds[k].sizes[1]) {
        char s[8];
        ksnprintf(s, sizeof(s), "%dx%d", items[i].w, items[i].h);
        struct rect b = size_badge(t);
        gfx_round(ui->c, b, 8, style.accent);
        gfx_text_box(ui->c, b, &font_small, s, RGB(255, 255, 255), ALIGN_CENTER);
    }
}

/* Edit mode: tiles do nothing when clicked; the red badge removes one, the size badge cycles its sizes, dragging
   moves it to where it is dropped, and the list below adds what is missing. */
static void edit_events(struct ui *ui, struct control *cc, struct rect *r)
{
    struct gui_event *e = ui->e;
    if (!e)
        return;
    if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_LEFT) {
        cc->drag = -1;
        for (int i = item_count - 1; i >= 0; i--) {
            if (!r[i].w)
                continue;
            if (rect_contains(remove_badge(r[i]), e->x, e->y)) {
                memmove(&items[i], &items[i + 1], (size_t)(item_count - i - 1) * sizeof(items[0]));
                item_count--;
                ui->consumed = true;
                save_items();
                return;
            }
            if (kinds[items[i].kind].sizes[1] && rect_contains(size_badge(r[i]), e->x, e->y)) {
                cycle_size(i);
                ui->consumed = true;
                save_items();
                return;
            }
        }
        for (int i = 0; i < item_count; i++)
            if (rect_contains(r[i], e->x, e->y)) {
                cc->drag = i;
                cc->grab_x = e->x - r[i].x;
                cc->grab_y = e->y - r[i].y;
                cc->drag_x = e->x;
                cc->drag_y = e->y;
                ui->consumed = true;
                return;
            }
    }
    if (cc->drag < 0)
        return;
    if (e->type == EV_MOUSE_MOVE) {
        cc->drag_x = e->x;
        cc->drag_y = e->y;
        wm_invalidate(ui->win);
    } else if (e->type == EV_MOUSE_UP) {
        int to = -1, last = 0;
        for (int i = 0; i < item_count; i++) {
            if (r[i].w && rect_contains(r[i], e->x, e->y))
                to = i;
            last = MAX(last, r[i].y + r[i].h);
        }
        if (to < 0 && e->y >= last)
            to = item_count - 1;
        int from = cc->drag;
        cc->drag = -1;
        if (to >= 0 && to != from) {
            move_item(from, to);
            save_items();
        } else {
            wm_invalidate(ui->win);
        }
    }
}

static void add_list(struct ui *ui, int y, int W)
{
    ui_label(ui, (struct rect){16, y, W - 32, 18}, "Add Controls", &font_bold, style.text, ALIGN_LEFT);
    y += 26;
    int cw = (W - 36) / 2, n = 0;
    for (int k = 0; k < K_COUNT; k++) {
        if (has_kind(k))
            continue;
        struct rect c = {12 + (n % 2) * (cw + 12), y + n / 2 * 38, cw, 30};
        n++;
        if (ui->c) {
            gfx_round(ui->c, c, 10, ui_hover(ui, c) ? style.button_hover : tile_color());
            gfx_circle(ui->c, c.x + 15, c.y + 15, 8, style.ok);
            gfx_fill(ui->c, (struct rect){c.x + 11, c.y + 14, 8, 2}, RGB(255, 255, 255));
            gfx_fill(ui->c, (struct rect){c.x + 14, c.y + 11, 2, 8}, RGB(255, 255, 255));
            gfx_text_box(ui->c, (struct rect){c.x + 30, c.y, c.w - 34, c.h}, &font_sans, kinds[k].wide, style.text,
                         ALIGN_LEFT);
        }
        if (pressed(ui, c) && item_count < MAX_ITEMS) {
            items[item_count++] = (struct cc_item){(uint8_t)k, (uint8_t)(kinds[k].sizes[0] >> 4),
                                                   (uint8_t)(kinds[k].sizes[0] & 15)};
            save_items();
            return;
        }
    }
}

static void full_frame(struct ui *ui, struct control *cc)
{
    int W = ui->win->surface.w;
    struct rect r[MAX_ITEMS];
    int rows = layout(r);
    if (cc->drag >= item_count)
        cc->drag = -1;
    struct ui view = *ui, *u = cc->editing ? &view : ui;
    view.e = NULL;
    hide_labels = cc->editing;
    for (int i = 0; i < item_count; i++) {
        if (!r[i].w || i == cc->drag)
            continue;
        draw_tile(u, r[i], items[i].kind);
        if (!u->win)
            return;
        if (cc->editing)
            edit_badges(ui, r[i], i);
    }
    if (cc->editing && cc->drag >= 0 && ui->c) {
        struct rect d = r[cc->drag];
        gfx_round_frame(ui->c, d, TILE_R, ALPHA(style.accent, 90));
        d.x = cc->drag_x - cc->grab_x;
        d.y = cc->drag_y - cc->grab_y;
        draw_tile(u, d, items[cc->drag].kind);
        edit_badges(ui, d, cc->drag);
    }
    ui->id = u->id;
    hide_labels = false;
    int y = 12 + rows * (CELL_H + GAP);
    if (cc->editing) {
        edit_events(ui, cc, r);
        if (missing_kinds()) {
            add_list(ui, y, W);
            y += 26 + (missing_kinds() + 1) / 2 * 38;
        }
    }
    int bw = (W - 36) / 2;
    if (cc->editing) {
        if (ui_button(ui, (struct rect){12, y, bw, 30}, "Reset")) {
            gui_setting("ccitems", "default");
            return;
        }
        if (ui_button_primary(ui, (struct rect){24 + bw, y, bw, 30}, "Done")) {
            cc->editing = false;
            cc->drag = -1;
            relayout();
        }
        return;
    }
    if (ui_button(ui, (struct rect){12, y, bw, 30}, "Edit Controls")) {
        cc->editing = true;
        relayout();
        return;
    }
    if (ui_button(ui, (struct rect){24 + bw, y, bw, 30}, "System Settings…"))
        open_settings(ui, NULL);
}

/* ---- the modules of the menu bar */

static int module_height(enum control_module m, bool editing)
{
    switch (m) {
    case CONTROL_SOUND: return 214;
    case CONTROL_NETWORK: return 100 + MAX(net_interface_count(), 1) * 44;
    case CONTROL_DISPLAY: return 196;
    case CONTROL_FOCUS: return 122;
    default: return full_height(editing);
    }
}

static void title(struct ui *ui, const char *text)
{
    ui_label(ui, (struct rect){18, 10, 200, 22}, text, &font_bold, style.text, ALIGN_LEFT);
}

static void sound_frame(struct ui *ui, int W)
{
    title(ui, "Sound");
    sound_slider(ui, (struct rect){16, 40, W - 32, 24});
    separator(ui, 78, W);
    ui_label(ui, (struct rect){18, 86, 200, 16}, "Output", &font_small, style.secondary, ALIGN_LEFT);
    const char *dev = hal_audio_name();
    if (ui->c) {
        gfx_circle(ui->c, 34, 120, 13, style.accent);
        gfx_symbol(ui->c, 26, 112, SYM_VOLUME, 16, RGB(255, 255, 255));
        gfx_text_box(ui->c, (struct rect){56, 106, W - 72, 28}, &font_sans, dev && *dev ? dev : "Built-in Output",
                     style.text, ALIGN_LEFT);
    }
    bool muted = gui_setting_on("mute");
    ui_label(ui, (struct rect){18, 140, 120, 28}, "Mute", &font_sans, style.text, ALIGN_LEFT);
    if (ui_switch(ui, (struct rect){W - 52, 144, 36, 21}, &muted))
        gui_setting("mute", muted ? "on" : "off");
    separator(ui, 176, W);
    if (settings_row(ui, 182, W, "Sound Settings…"))
        open_settings(ui, "sound");
}

static void network_frame(struct ui *ui, int W)
{
    title(ui, "Network");
    bool on = net_enabled();
    if (ui_switch(ui, (struct rect){W - 52, 12, 36, 21}, &on))
        gui_setting("network", on ? "on" : "off");
    separator(ui, 44, W);
    int y = 52;
    char buf[48];
    if (!net_interface_count()) {
        ui_label(ui, (struct rect){18, y, W - 36, 36}, "No network hardware", &font_sans, style.secondary,
                 ALIGN_LEFT);
        y += 44;
    }
    for (int i = 0; i < net_interface_count(); i++, y += 44) {
        char name[24];
        ksnprintf(name, sizeof(name), "Ethernet %d", i + 1);
        bool up = net_enabled() && net_interface(i)->configured;
        const char *state = i ? (up ? "Connected" : "Not connected") : network_state(buf, sizeof(buf));
        if (circle_toggle(ui, 10, y + 2, W - 20, net_enabled() ? SYM_NETWORK : SYM_WIFI_OFF, up, name, state))
            open_settings(ui, "network");
        if (!ui->win)
            return;
    }
    separator(ui, y + 2, W);
    if (settings_row(ui, y + 8, W, "Network Settings…"))
        open_settings(ui, "network");
}

static void display_frame(struct ui *ui, int W)
{
    title(ui, "Display");
    brightness_slider(ui, (struct rect){16, 40, W - 32, 24});
    separator(ui, 78, W);
    bool night = night_on(), dark = style.dark;
    ui_label(ui, (struct rect){18, 86, 160, 28}, "Night Shift", &font_sans, style.text, ALIGN_LEFT);
    if (ui_switch(ui, (struct rect){W - 52, 90, 36, 21}, &night))
        gui_setting("nightshift", night ? "on" : "off");
    ui_label(ui, (struct rect){18, 118, 160, 28}, "Dark Mode", &font_sans, style.text, ALIGN_LEFT);
    if (ui_switch(ui, (struct rect){W - 52, 122, 36, 21}, &dark))
        gui_setting("appearance", dark ? "dark" : "light");
    separator(ui, 156, W);
    if (settings_row(ui, 162, W, "Displays Settings…"))
        open_settings(ui, "displays");
}

static void focus_frame(struct ui *ui, int W)
{
    title(ui, "Focus");
    bool dnd = gui_setting_on("dnd");
    if (circle_toggle(ui, 10, 40, W - 20, SYM_MOON, dnd, "Do Not Disturb", dnd ? "On" : "Off"))
        gui_setting("dnd", dnd ? "off" : "on");
    separator(ui, 84, W);
    if (settings_row(ui, 90, W, "Focus Settings…"))
        open_settings(ui, "notifications");
}

static void control_frame(struct ui *ui, void *data)
{
    struct control *cc = data;
    int W = ui->win->surface.w;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, ui->c->w, ui->c->h}, 0);
    switch (cc->mode) {
    case CONTROL_SOUND: sound_frame(ui, W); break;
    case CONTROL_NETWORK: network_frame(ui, W); break;
    case CONTROL_DISPLAY: display_frame(ui, W); break;
    case CONTROL_FOCUS: focus_frame(ui, W); break;
    default: full_frame(ui, cc); break;
    }
    if (ui->win && ui->e && ui->e->type == EV_MOUSE_UP)
        chrome_changed();
}

static void control_paint(struct window *w, struct canvas *c)
{
    struct control *cc = w->data;
    ui_run(w, &cc->st, c, NULL, control_frame, cc);
}

static void control_event(struct window *w, struct gui_event *e)
{
    struct control *cc = w->data;
    if (e->type == EV_KEY && KEY_BASE(e->key) == KEY_ESC) {
        wm_destroy(w);
        return;
    }
    if (e->type == EV_TIMER) {
        wm_invalidate(w);
        return;
    }
    ui_run(w, &cc->st, NULL, e, control_frame, cc);
}

static void control_destroy(struct window *w)
{
    kfree(w->data);
    if (control_win == w)
        control_win = NULL;
}

static const struct window_ops control_ops = {control_paint, control_event, NULL, control_destroy};

void control_module_toggle(enum control_module m, int right, int top)
{
    if (control_win && wm_window_alive(control_win)) {
        bool same = control_mode == m;
        wm_destroy(control_win);
        if (same)
            return;
    }
    wm_close_popups();
    struct control *cc = kzalloc(sizeof(*cc));
    if (!cc)
        return;
    cc->mode = m;
    cc->drag = -1;
    int w = m == CONTROL_FULL ? CC_W : MODULE_W, h = module_height(m, false);
    struct rect s = wm_screen_rect(0);
    int x = CLAMP(right - w, s.x + 8, s.x + s.w - w - 8);
    control_win = wm_popup(x, top, w, h, &control_ops, cc, NULL);
    if (!control_win) {
        kfree(cc);
        return;
    }
    control_mode = m;
    control_win->radius = m == CONTROL_FULL ? 24 : 16;
    wm_set_timer(control_win, 1000);
}

void control_center_toggle(int right)
{
    control_module_toggle(CONTROL_FULL, right, status_bottom() + 6);
}

/* System Settings opens the Control Center right in edit mode. */
void control_center_edit(void)
{
    if (!control_win || !wm_window_alive(control_win) || control_mode != CONTROL_FULL) {
        struct rect s = wm_screen_rect(0);
        control_center_toggle(s.x + s.w - 8);
    }
    if (!control_win)
        return;
    struct control *cc = control_win->data;
    cc->editing = true;
    relayout();
}

static void relayout(void)
{
    if (!control_win || !wm_window_alive(control_win) || control_mode != CONTROL_FULL)
        return;
    struct control *cc = control_win->data;
    wm_resize_client(control_win, CC_W, module_height(CONTROL_FULL, cc->editing));
    wm_invalidate(control_win);
}
