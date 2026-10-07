#ifndef SYNTH_GUI_INTERNAL_H
#define SYNTH_GUI_INTERNAL_H

#include <synth/gui.h>
#include <synth/hal.h>

#define MAX_SCREENS 4
#define Z_DESKTOP 1
#define Z_MENUBAR 1000
#define Z_DOCK 1001
#define Z_CURSOR 0x40000000

struct framebuffer;

extern int mouse_x, mouse_y;

struct glass {
    struct rect frame;
    int z;
    bool valid;
    bool registered;
    struct canvas small;
    struct rect src;
    struct glass *next;
};

enum glass_kind {
    GLASS_MENUBAR,
    GLASS_DOCK,
    GLASS_MENU,
    GLASS_PANEL,
    GLASS_WINDOW,
    GLASS_OVERLAY,
};

struct glass_look {
    uint32_t tint;
    uint32_t solid_color;
    uint32_t outline;
    int radius;
    int blur;
    int saturation;
    int band;
    int rim;
    bool liquid;
    bool solid;
};

void glass_register(struct glass *g);
void glass_unregister(struct glass *g);
void glass_set_frame(struct glass *g, struct rect frame);
void glass_invalidate(struct glass *g);
void glass_damage(struct rect r, int z);
void glass_draw(struct canvas *c, struct glass *g, const struct glass_look *look);
void glass_look_for(struct glass_look *look, enum glass_kind kind, int radius);
int glass_margin(void);
int gfx_corner_coverage(struct rect r, int radius, int x, int y);

bool wm_start(const struct framebuffer *fbs, const struct rect *origins, int count);
bool wm_reconfigure(const struct framebuffer *fbs, const struct rect *origins, int count);
void gui_displays_changed(void);
int gui_display_hal(int screen);
void wm_stop(void);
void wm_render(void);
void wm_pointer(const struct pointer_event *ev);
void wm_key(int key);
void wm_cycle(void);
void wm_timers(void);
void wm_close_popups(void);
struct window *wm_top_popup(void);
void wm_damage_all(void);
void wm_damage_z(struct rect r, int z);
void wm_set_cursor_visible(bool on);
int wm_window_list(struct window **out, int max);
struct rect wm_compose_screen(void);
void wm_restore(struct window *w);
void wm_set_app(const struct gui_app *app);
struct window *wm_app_window(const struct gui_app *app);
int wm_app_window_count(const struct gui_app *app);
void wm_focus_app(const struct gui_app *app);
void wm_bring_all(void);

void desktop_paint(struct canvas *c, int screen, struct rect area);
void desktop_mouse(int x, int y, unsigned button, bool down, int clicks);

void menubar_paint(struct canvas *c, int screen);
void dock_paint(struct canvas *c, int screen);
bool chrome_hit(int x, int y);
void chrome_mouse(int x, int y, unsigned button, bool down, int clicks);
void chrome_hover(int x, int y);
void chrome_changed(void);
void chrome_start(void);
void chrome_stop(void);
int dock_reserved(void);
bool dock_hit(int x, int y);
void dock_mouse(int x, int y, unsigned button, bool down, int clicks);
void dock_hover(int x, int y);
void dock_changed(void);
void dock_start(void);
void dock_stop(void);
bool dock_hidden(void);
void dock_minimize_target(struct window *w);
void menubar_open_app_menu(void);

void spotlight_toggle(void);
void launchpad_toggle(void);
void control_center_toggle(int x);
void notify_tick(void);

void wallpaper_paint(struct canvas *c, const char *kind, bool dark);

struct window *terminal_open(void);
void terminal_close(void);
void terminal_flush(void);
struct window *terminal_window(void);
void terminal_retitle(void);

void gui_lock(void);
void gui_login(void);
bool gui_locked(void);
void gui_session_login(const char *name);
const struct canvas *desktop_wallpaper(int screen);
void desktop_dir(char *out, size_t cap);

#endif
