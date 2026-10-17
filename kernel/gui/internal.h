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

enum dock_kind {
    DOCK_CENTER,
    DOCK_CORNER,
    DOCK_RAIL,
    DOCK_ARC,
};

enum status_kind {
    STATUS_MENUBAR,
    STATUS_PILL,
    STATUS_CORNER,
};

enum widget_id {
    WIDGET_CLOCK,
    WIDGET_WEATHER,
    WIDGET_MUSIC,
    WIDGET_CALENDAR,
    WIDGET_VISUALIZER,
    WIDGET_COUNT,
};

struct gui_look {
    const char *name;
    const char *title;
    const char *description;
    bool dark;
    uint32_t tint, accent;
    enum dock_kind dock;
    enum status_kind status;
    bool places;
    unsigned widgets;
};

const struct gui_look *look_current(void);
const struct gui_look *look_get(int index);
int look_count(void);
int menubar_height(void);
void gui_look_changed(void);

struct spring {
    int pos, vel, target, rem;
};

void spring_set(struct spring *s, int value);
bool spring_step(struct spring *s, int stiffness, int damping, int dt);
bool spring_busy(const struct spring *s);

extern int mouse_x, mouse_y;


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
    int scale;
    int refract;
    int brightness;
    int pointer_light;
    bool dispersion;
    bool liquid;
    bool solid;
    gfx_cover_fn shape;
    const void *shape_ctx;
    bool (*shape_span)(int y, const void *ctx, int *x0, int *x1);
    int shape_key;
};

struct glass {
    struct rect frame;
    int z;
    bool valid;
    bool registered;
    struct canvas small;
    struct rect src;
    int scale;
    void *geo;
    int geo_w, geo_h, geo_r;
    unsigned stamp, out_stamp;
    struct canvas out;
    struct glass_look out_look;
    int out_x, out_y, out_mx, out_my;
    bool out_near;
    bool stale;
    struct glass *next;
};

extern uint64_t glass_stat_renders, glass_stat_pixels;
void glass_register(struct glass *g);
void glass_unregister(struct glass *g);
void glass_set_frame(struct glass *g, struct rect frame);
void glass_invalidate(struct glass *g);
void glass_damage(struct rect r, int z);
void glass_draw(struct canvas *c, struct glass *g, const struct glass_look *look);
void glass_freeze(bool on);
void glass_defer(bool on);
void glass_prepare(struct canvas *c, struct glass *g, const struct glass_look *look);
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
void wm_reflow(void);
void wm_crossfade(void);
void wm_window_menu(struct window *w, int x, int y);
int shell_left_reserved(void);

#define PLACES_W 190
#define PANEL_RADIUS 34

void panel_paint(struct canvas *c, int screen);
bool panel_hit(int x, int y);
void panel_mouse(int x, int y, unsigned button, bool down);
void panel_hover(int x, int y);
void panel_changed(void);
void panel_start(void);
void panel_stop(void);
bool panel_animate(void);
int status_bottom(void);
int panel_weekday(int y, int m, int d);
const char *panel_day_name(int i);
const char *panel_month_name(int i);
void launcher_toggle(int x, int y);
void calendar_toggle(int right, int y);
void shell_system_menu(int x, int y);
void shell_power_menu(bool restart);

#define MEDIA_BANDS 12

struct media_track {
    const char *title, *artist;
    int length_ms, position_ms, index;
    bool playing;
};

const struct media_track *media_track(void);
int media_count(void);
const char *media_song_title(int i);
const char *media_song_artist(int i);
void media_play(int index);
int media_open(const char *path);
void media_toggle(void);
void media_next(void);
void media_prev(void);
void media_stop(void);
void media_resume(void);
void media_seek(int ms);
bool media_is_file(void);
int media_play_pcm(int16_t *frames, uint32_t count, uint32_t rate, const char *title, const char *artist);
int media_levels(int *out, int n);
bool media_tick(void);

void widgets_paint(struct canvas *c, struct rect area);
bool widgets_mouse(int x, int y, unsigned button, bool down, int clicks);
void widgets_hover(int x, int y);
bool widgets_animate(void);
void widgets_start(void);
void widgets_stop(void);
void widgets_user_changed(void);
int widgets_right_reserved(void);
int widgets_obstacles(struct rect *out, int max);
void gui_desktop_changed(void);
void desktop_hover(int x, int y, bool over);
bool viewer_is_binary(const char *path);

#define AUTOSTART_MAX 16
#define AUTOSTART_LEN 128

int autostart_items(char (*out)[AUTOSTART_LEN], int max);
int autostart_add(const char *item);
int autostart_remove(int index);
void autostart_run(void);

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
void menubar_open_app_menu(void);

void spotlight_toggle(void);
void launchpad_toggle(void);
void control_center_toggle(int x);
void notify_tick(void);

int anim_progress(uint64_t start, int ms);
int ease_out(int t);
int ease_in_out(int t);
int ease_spring(int t);
int lerp_i(int a, int b, int t);
bool dock_animate(void);
struct rect dock_window_rect(struct window *w);
void dock_bounce(const struct gui_app *app);

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
