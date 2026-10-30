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
};

enum status_kind {
    STATUS_MENUBAR,
    STATUS_PILL,
};

struct gui_look {
    const char *name;
    const char *title;
    const char *description;
    bool dark;
    uint32_t tint, accent;
    enum dock_kind dock;
    enum status_kind status;
    unsigned widgets;
};

const struct gui_look *look_current(void);
const struct gui_look *look_get(int index);
int look_count(void);
void look_user_changed(void);
void look_reset(void);
int menubar_height(void);
void gui_look_changed(void);

struct spring {
    int pos, vel, target;
    int64_t x, v;
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
    bool edge_line;
    bool liquid;
    bool solid;
    gfx_cover_fn shape;
    const void *shape_ctx;
    bool (*shape_span)(int y, const void *ctx, int *x0, int *x1);
    int (*shape_sdf)(int x, int y, const void *ctx, int *nx, int *ny);
    int shape_key;
};

struct glass {
    struct rect frame;
    int z;
    bool valid;
    bool registered;
    struct canvas small, sharp;
    struct rect src;
    int scale;
    void *geo;
    int geo_w, geo_h, geo_r;
    unsigned stamp, out_stamp;
    int luma;
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
void wm_set_tint(int brightness, int warmth);
int gui_glass_strength(void);
#ifndef ABS
#define ABS(v) ((v) < 0 ? -(v) : (v))
#endif
void glass_invalidate_all(void);
void display_tick(void);
int display_brightness(void);
bool display_night(void);
void wm_damage_z(struct rect r, int z);
void wm_set_cursor_visible(bool on);
int wm_window_list(struct window **out, int max);
bool wm_capture(struct rect r, struct canvas *out);
bool wm_settled(void);
uint32_t wm_top_color(int screen);
bool wm_animating(void);
unsigned wm_mouse_buttons(void);
void hotcorner_tick(void);
void screenshot_tick(void);
bool wm_window_image(struct window *w, struct canvas *out);
struct window *wm_window_at_point(int x, int y);
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
struct rect dock_obstacle(void);
int dock_rail_reserved(void);

#define PANEL_RADIUS 44

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
void calendar_toggle(int right, int y);
void calendar_draw(struct canvas *c, struct rect at);
void notify_record(const char *title, const char *text);
int notify_unseen(void);
void notification_center_toggle(void);
enum shot_mode {
    SHOT_PANEL,
    SHOT_SCREEN,
    SHOT_WINDOW,
    SHOT_REGION,
};
void screenshot_start(enum shot_mode mode);
void mission_toggle(void);
void quicklook_toggle(const char *path);
bool quicklook_open(void);
bool mission_active(void);
void switcher_next(int held, bool back);
void desktop_show_toggle(void);
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
int media_volume(void);
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
void widgets_edit(bool on);
void wm_animate_ghosts(void);
struct window *wm_window_at(int x, int y);
int wallpaper_download(const char *source, char *saved, size_t cap, char *title, size_t title_cap,
                       void (*progress)(void *ctx, const char *stage, size_t done, size_t total), void *ctx);
void wallpaper_fetch(const char *source);
void wallpaper_fetch_run(void);
void gui_wallpaper_use(const char *path);
void gui_draw_file_icon(struct canvas *c, const char *path, struct rect r, int alpha);
void gui_file_label(const char *path, char *out, size_t cap);
void gui_make_app_shortcut(const char *name);
bool gui_start_program(const char *path, const char *args);
void chrome_install_run(void);
void chrome_auto_arm(void);
const struct gui_app *chrome_app_for(const char *path);
int chrome_app_progress(const struct gui_app *app);
void chromium_auto_arm(void);
void chromium_run(void);
int gui_app_progress(const struct gui_app *app);
void dock_redraw(void);
void dock_drag_move(const char *entry, int x, int y);
bool dock_drag_drop(int x, int y);
bool dock_dragging(void);
bool desktop_icon_drag(int x, int y);
void desktop_icon_hover(int x, int y);
bool desktop_animate(void);
void dock_paint_drag(struct canvas *c);
bool widgets_editing(void);
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
void gui_clock_time(const struct rtc_time *t, char *buf, size_t cap, bool seconds, bool suffix);
void gui_clock_text(char *buf, size_t cap, bool date);
bool gui_clock_seconds(void);
const struct gui_app *gui_default_browser(void);
void chrome_start(void);
void chrome_stop(void);
int dock_reserved(void);
void dock_peek_hover(const struct gui_app *app, struct rect icon, bool beside);
void dock_peek_tick(void);
void dock_peek_cancel(void);
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
void control_center_toggle(int right);
enum control_module {
    CONTROL_FULL,
    CONTROL_SOUND,
    CONTROL_NETWORK,
    CONTROL_DISPLAY,
    CONTROL_FOCUS,
};
void control_module_toggle(enum control_module m, int right, int top);
int gui_setting(const char *name, const char *value);
void gui_setting_get(const char *name, char *buf, size_t cap);
bool gui_setting_on(const char *name);
long gui_setting_number(const char *name, long fallback);
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
void gui_blurred_wallpaper(struct canvas *out);
bool gui_setup_needed(void);
bool gui_setup_active(void);
void gui_setup(void);
bool gui_locked(void);
void gui_session_login(const char *name);
const struct canvas *desktop_wallpaper(int screen);
void desktop_dir(char *out, size_t cap);

#endif
