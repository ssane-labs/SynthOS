#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/net.h>
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
        gfx_text_box(ui->c, (struct rect){x + 38, y + 1, w - 40, 18}, &font_bold, label, style.text, ALIGN_LEFT);
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

/* ---- Control Center */

static int full_height(void)
{
    return 12 + 136 + 10 + 66 + 10 + 66 + 10 + 64 + 10 + 30 + 12;
}

static void now_playing(struct ui *ui, struct rect t)
{
    const struct media_track *m = media_track();
    if (ui->c) {
        struct rect art = {t.x + 12, t.y + 12, 40, 40};
        gfx_round_gradient(ui->c, art, 9, RGB(252, 92, 125), RGB(106, 130, 251));
        gfx_symbol(ui->c, art.x + 12, art.y + 12, SYM_MUSIC, 16, RGB(255, 255, 255));
        gfx_text_box(ui->c, (struct rect){t.x + 62, t.y + 14, t.w - 140, 18}, &font_bold,
                     m && m->title ? m->title : "Not Playing", style.text, ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){t.x + 62, t.y + 32, t.w - 140, 16}, &font_small,
                     m && m->artist ? m->artist : "Music", style.secondary, ALIGN_LEFT);
    }
    bool playing = m && m->playing;
    struct rect prev = {t.x + t.w - 92, t.y + 18, 28, 28}, play = {prev.x + 30, prev.y, 28, 28},
                next = {play.x + 30, prev.y, 28, 28};
    if (ui->c) {
        gfx_symbol(ui->c, prev.x + 6, prev.y + 6, SYM_PREV, 16, style.text);
        gfx_symbol(ui->c, play.x + 6, play.y + 6, playing ? SYM_PAUSE : SYM_PLAY, 16, style.text);
        gfx_symbol(ui->c, next.x + 6, next.y + 6, SYM_NEXT, 16, style.text);
    }
    if (pressed(ui, prev))
        media_prev();
    else if (pressed(ui, play))
        media_toggle();
    else if (pressed(ui, next))
        media_next();
    else if (pressed(ui, (struct rect){t.x, t.y, t.w - 100, t.h})) {
        ui->win = NULL;
        wm_close_popups();
        gui_app_launch(gui_app_find("Music"), NULL);
    }
}

static void square_tile(struct ui *ui, struct rect t, enum symbol_id sym, const char *label, int action)
{
    if (ui->c) {
        gfx_symbol(ui->c, t.x + (t.w - 16) / 2, t.y + 12, sym, 16, style.text);
        gfx_text_box(ui->c, (struct rect){t.x + 2, t.y + 34, t.w - 4, 16}, &font_small, label, style.text,
                     ALIGN_CENTER);
    }
    if (!pressed(ui, t))
        return;
    ui->win = NULL;
    wm_close_popups();
    if (action == 0)
        screenshot_start(SHOT_PANEL);
    else
        mission_toggle();
}

static void full_frame(struct ui *ui)
{
    int W = ui->win->surface.w, half = (W - 36) / 2;
    char buf[48];
    int y = 12;
    struct rect t = tile(ui, 12, y, half, 136);
    if (circle_toggle(ui, t.x + 8, t.y + 8, t.w - 12, net_enabled() ? SYM_WIFI : SYM_WIFI_OFF, net_enabled(),
                      "Network", network_state(buf, sizeof(buf))))
        gui_setting("network", net_enabled() ? "off" : "on");
    if (circle_toggle(ui, t.x + 8, t.y + 50, t.w - 12, SYM_APPEARANCE, style.dark, "Dark Mode",
                      style.dark ? "On" : "Off"))
        gui_setting("appearance", style.dark ? "light" : "dark");
    if (circle_toggle(ui, t.x + 8, t.y + 92, t.w - 12, SYM_SUN, night_on(), "Night Shift", night_state()))
        gui_setting("nightshift", night_on() ? "off" : "on");
    int rx = 24 + half;
    t = tile(ui, rx, y, half, 62);
    bool dnd = gui_setting_on("dnd");
    if (circle_toggle(ui, t.x + 8, t.y + 13, t.w - 12, SYM_MOON, dnd, "Focus", dnd ? "Do Not Disturb" : "Off"))
        gui_setting("dnd", dnd ? "off" : "on");
    int sq = (half - 12) / 2;
    square_tile(ui, tile(ui, rx, y + 74, sq, 62), SYM_SCREENSHOT, "Screenshot", 0);
    square_tile(ui, tile(ui, rx + sq + 12, y + 74, half - sq - 12, 62), SYM_MISSION, "Mission Control", 1);
    y += 146;
    t = tile(ui, 12, y, W - 24, 66);
    ui_label(ui, (struct rect){t.x + 14, t.y + 8, 200, 18}, "Display", &font_bold, style.text, ALIGN_LEFT);
    brightness_slider(ui, (struct rect){t.x + 12, t.y + 32, t.w - 24, 24});
    y += 76;
    t = tile(ui, 12, y, W - 24, 66);
    ui_label(ui, (struct rect){t.x + 14, t.y + 8, 200, 18}, "Sound", &font_bold, style.text, ALIGN_LEFT);
    sound_slider(ui, (struct rect){t.x + 12, t.y + 32, t.w - 24, 24});
    y += 76;
    now_playing(ui, tile(ui, 12, y, W - 24, 64));
    if (!ui->win)
        return;
    y += 74;
    int bw = (W - 36) / 2;
    if (ui_button(ui, (struct rect){12, y, bw, 30}, "Lock Screen")) {
        ui->win = NULL;
        wm_close_popups();
        gui_lock();
        return;
    }
    if (ui_button(ui, (struct rect){24 + bw, y, bw, 30}, "System Settings…"))
        open_settings(ui, NULL);
}

/* ---- the modules of the menu bar */

static int module_height(enum control_module m)
{
    switch (m) {
    case CONTROL_SOUND: return 214;
    case CONTROL_NETWORK: return 100 + MAX(net_interface_count(), 1) * 44;
    case CONTROL_DISPLAY: return 196;
    case CONTROL_FOCUS: return 122;
    default: return full_height();
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
    default: full_frame(ui); break;
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
    int w = m == CONTROL_FULL ? CC_W : MODULE_W, h = module_height(m);
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
