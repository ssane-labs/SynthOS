#ifndef SYNTH_GUI_H
#define SYNTH_GUI_H

#include <synth/types.h>

#define RGB(r, g, b) (0xff000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define RGBA(r, g, b, a) ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
#define ALPHA(c, a) (((c) & 0x00ffffffu) | (uint32_t)(a) << 24)

struct rect {
    int x, y, w, h;
};

struct canvas {
    uint32_t *px;
    int w, h, stride;
    struct rect clip;
};

struct ui_glyph {
    uint16_t code;
    uint8_t advance, w, h;
    int8_t x, y;
    uint32_t offset;
};

struct ui_font {
    int height;
    int ascent;
    int count;
    const struct ui_glyph *glyphs;
    const uint8_t *lz;
    uint32_t lz_size, size;
    const uint8_t **alpha;
};

extern const struct ui_font font_sans, font_bold, font_small, font_title, font_large, font_mono, font_clock, font_display;

enum icon_id {
    ICON_FOLDER,
    ICON_FILE,
    ICON_TEXT,
    ICON_APP,
    ICON_DLL,
    ICON_IMAGE,
    ICON_TERMINAL,
    ICON_SETTINGS,
    ICON_CALC,
    ICON_NOTEPAD,
    ICON_PAINT,
    ICON_COMPUTER,
    ICON_HOME,
    ICON_MONITOR,
    ICON_MINES,
    ICON_DOS,
    ICON_SX,
    ICON_SCRIPT,
    ICON_POWER,
    ICON_LOGO,
    ICON_INFO,
    ICON_FILES,
    ICON_LAUNCHPAD,
    ICON_WARNING,
    ICON_USER,
    ICON_TRASH,
    ICON_TRASH_FULL,
    ICON_USB,
    ICON_FOLDER_APPS,
    ICON_FOLDER_DOCS,
    ICON_FOLDER_DESKTOP,
    ICON_FOLDER_DOWNLOADS,
    ICON_BROWSER,
    ICON_PHOTOS,
    ICON_MUSIC,
    ICON_VIDEO,
    ICON_VIEWER,
    ICON_INSTALLER,
    ICON_COUNT,
};

enum symbol_id {
    SYM_LOGO,
    SYM_SEARCH,
    SYM_CONTROL,
    SYM_WIFI,
    SYM_HOME,
    SYM_DESKTOP,
    SYM_DOC,
    SYM_APPS,
    SYM_DOWNLOAD,
    SYM_DISK,
    SYM_TRASH,
    SYM_FOLDER,
    SYM_CLOCK,
    SYM_GEAR,
    SYM_USER,
    SYM_USERS,
    SYM_LOCK,
    SYM_POWER,
    SYM_RESTART,
    SYM_MOON,
    SYM_SUN,
    SYM_DISPLAY,
    SYM_DOCK,
    SYM_PHOTO,
    SYM_APPEARANCE,
    SYM_KEYBOARD,
    SYM_MOUSE,
    SYM_INFO,
    SYM_LEFT,
    SYM_RIGHT,
    SYM_DOWN,
    SYM_UP,
    SYM_GRID,
    SYM_LIST,
    SYM_PLUS,
    SYM_MINUS,
    SYM_CHECK,
    SYM_CLOSE,
    SYM_PENCIL,
    SYM_BRUSH,
    SYM_ERASER,
    SYM_LINE,
    SYM_RECT,
    SYM_ELLIPSE,
    SYM_BUCKET,
    SYM_DROPPER,
    SYM_UNDO,
    SYM_TERMINAL,
    SYM_CHIP,
    SYM_MEMORY,
    SYM_NETWORK,
    SYM_SIDEBAR,
    SYM_STAR,
    SYM_NEWFOLDER,
    SYM_USB,
    SYM_BATTERY,
    SYM_EJECT,
    SYM_MUSIC,
    SYM_VIDEO,
    SYM_PLAY,
    SYM_PAUSE,
    SYM_PREV,
    SYM_NEXT,
    SYM_VOLUME,
    SYM_MORE,
    SYM_WEATHER,
    SYM_CALENDAR,
    SYM_PIN,
    SYM_GLOBE,
    SYM_COUNT,
};

struct packed_image {
    uint16_t size;
    uint32_t length;
    const uint8_t *data;
};

extern const struct packed_image icon_pack[ICON_COUNT], symbol_pack[SYM_COUNT];

struct rect rect_make(int x, int y, int w, int h);
bool rect_empty(struct rect r);
bool rect_contains(struct rect r, int x, int y);
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
struct rect rect_inset(struct rect r, int d);
bool rect_inside(struct rect inner, struct rect outer);

uint32_t color_mix(uint32_t a, uint32_t b, int t);
uint32_t color_shade(uint32_t c, int amount);
int color_luma(uint32_t c);

struct canvas canvas_make(uint32_t *px, int w, int h);
bool canvas_alloc(struct canvas *c, int w, int h);
void canvas_free(struct canvas *c);
void canvas_clip(struct canvas *c, struct rect r);

void gfx_fill(struct canvas *c, struct rect r, uint32_t color);
void gfx_clear(struct canvas *c, struct rect r, uint32_t color);
void gfx_frame(struct canvas *c, struct rect r, uint32_t color);
void gfx_gradient(struct canvas *c, struct rect r, uint32_t top, uint32_t bottom);
void gfx_round(struct canvas *c, struct rect r, int radius, uint32_t color);
void gfx_round_clear(struct canvas *c, struct rect r, int radius, uint32_t color);
void gfx_round_frame(struct canvas *c, struct rect r, int radius, uint32_t color);
void gfx_round_gradient(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom);
void gfx_round_frame_gradient(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom);
void gfx_shadow(struct canvas *c, struct rect r, int radius, int size, int strength);
void gfx_drop_shadow(struct canvas *c, struct rect r, int radius, int size, int strength, int dy);
void gfx_glow(struct canvas *c, struct rect r, int radius, int size, int strength, uint32_t color);
typedef int (*gfx_cover_fn)(int x, int y, const void *ctx);
void gfx_shape(struct canvas *c, struct rect bounds, gfx_cover_fn cover, const void *ctx, uint32_t color);
int gfx_dist16(int dx16, int dy16);
int gfx_cover(int d16);
void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_thick_line(struct canvas *c, int x0, int y0, int x1, int y1, int width, uint32_t color);
void gfx_circle(struct canvas *c, int cx, int cy, int r, uint32_t color);
void gfx_ring(struct canvas *c, int cx, int cy, int r, uint32_t color);
void gfx_pixel(struct canvas *c, int x, int y, uint32_t color);
void gfx_blit(struct canvas *c, int x, int y, const struct canvas *src, struct rect from);
void gfx_blit_alpha(struct canvas *c, int x, int y, const struct canvas *src, struct rect from, int alpha);
void gfx_blit_round(struct canvas *c, struct rect dst, const struct canvas *src, int radius, bool blend_alpha);
void gfx_scale(struct canvas *c, struct rect dst, const struct canvas *src, int alpha);
void gfx_scale_fast(struct canvas *c, struct rect dst, const struct canvas *src, int alpha);
void gfx_scale_smooth(struct canvas *c, struct rect dst, const struct canvas *src, int alpha);
void gfx_bleed(struct canvas *c);
void gfx_row_scaled(struct canvas *c, int y, int x0, int x1, const struct canvas *src, int sy, int alpha);
void gfx_image(struct canvas *c, int x, int y, const uint32_t *argb, int w, int h);
void gfx_icon(struct canvas *c, int x, int y, enum icon_id id, int size);
void gfx_icon_alpha(struct canvas *c, int x, int y, enum icon_id id, int size, int alpha);
void gfx_symbol(struct canvas *c, int x, int y, enum symbol_id id, int size, uint32_t color);
const uint32_t *gfx_icon_pixels(enum icon_id id);

enum text_align {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT,
};

uint32_t utf8_next(const char **s);
bool gfx_has_glyph(const struct ui_font *f, uint32_t code);
int gfx_glyph(struct canvas *c, int x, int y, const struct ui_font *f, uint32_t code, uint32_t color);
int gfx_text(struct canvas *c, int x, int y, const struct ui_font *f, const char *s, uint32_t color);
int gfx_text_n(struct canvas *c, int x, int y, const struct ui_font *f, const char *s, size_t n, uint32_t color);
int gfx_text_width(const struct ui_font *f, const char *s);
int gfx_text_width_n(const struct ui_font *f, const char *s, size_t n);
void gfx_text_box(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color,
                  enum text_align align);
void gfx_cell(struct canvas *c, int x, int y, uint8_t ch, uint32_t fg, uint32_t bg);

enum glass_level {
    GLASS_LIQUID,
    GLASS_FROSTED,
    GLASS_SOLID,
};

struct gui_style {
    bool dark;
    int glass;
    uint32_t tint;
    uint32_t window;
    uint32_t content;
    uint32_t sidebar;
    uint32_t titlebar;
    uint32_t titlebar_inactive;
    uint32_t separator;
    uint32_t text;
    uint32_t secondary;
    uint32_t tertiary;
    uint32_t accent;
    uint32_t accent_text;
    uint32_t selection;
    uint32_t selection_inactive;
    uint32_t hover;
    uint32_t control;
    uint32_t control_pressed;
    uint32_t control_border;
    uint32_t field;
    uint32_t field_border;
    uint32_t zebra;
    uint32_t group;
    uint32_t menu;
    uint32_t error;
    uint32_t ok;
    uint32_t warning;
    uint32_t desktop;
    uint32_t panel;
    uint32_t border;
    uint32_t muted;
    uint32_t button;
    uint32_t button_hover;
    uint32_t accent2;
    uint32_t palette[16];
};

extern struct gui_style style;

enum gui_event_type {
    EV_NONE,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,
    EV_WHEEL,
    EV_KEY,
    EV_FOCUS,
    EV_BLUR,
    EV_RESIZE,
    EV_TIMER,
    EV_LEAVE,
};

enum {
    BUTTON_LEFT = 1,
    BUTTON_RIGHT = 2,
    BUTTON_MIDDLE = 4,
};

struct gui_event {
    enum gui_event_type type;
    int x, y;
    unsigned buttons;
    unsigned button;
    int wheel;
    int key;
    int clicks;
    bool handled;
};

enum window_flags {
    WIN_RESIZABLE = 1u << 0,
    WIN_MAXIMIZED = 1u << 1,
    WIN_MINIMIZED = 1u << 2,
    WIN_HIDDEN = 1u << 3,
    WIN_POPUP = 1u << 4,
    WIN_DIALOG = 1u << 5,
    WIN_KEEP = 1u << 6,
    WIN_FULLSIZE = 1u << 7,
    WIN_GLASS = 1u << 8,
    WIN_NOTITLE = 1u << 9,
    WIN_OVERLAY = 1u << 10,
    WIN_NOSHADOW = 1u << 11,
    WIN_PASSIVE = 1u << 12,
    WIN_PINNED = 1u << 13,
    WIN_PEEKED = 1u << 14,
    WIN_BARE = 1u << 15,
    WIN_TRANSLUCENT = 1u << 16,
};

#define MENU_HEADER (-2)

struct menu_item {
    const char *label;
    int key;
    const char *shortcut;
};

struct app_menu {
    const char *title;
    const struct menu_item *items;
    int count;
};

struct window;
struct gui_app;
struct glass;

struct window_ops {
    void (*paint)(struct window *w, struct canvas *c);
    void (*event)(struct window *w, struct gui_event *e);
    bool (*close)(struct window *w);
    void (*destroy)(struct window *w);
};

struct window {
    char title[64];
    enum icon_id icon;
    struct rect frame;
    struct rect restore;
    unsigned flags;
    int min_w, min_h;
    struct canvas surface;
    bool dirty;
    const struct window_ops *ops;
    void *data;
    uint32_t timer_ms;
    uint64_t next_timer;
    struct window *owner;
    struct window *below;
    struct window *above;
    const struct gui_app *app;
    const struct app_menu *menus;
    int menu_count;
    int toolbar_h;
    int radius;
    int z;
    struct glass *glass;
    struct canvas thumb;
    bool anim_hold;
};

#define TITLE_HEIGHT 28
#define TOOLBAR_HEIGHT 52
#define BORDER 0
#define MENUBAR_HEIGHT 24

struct window *wm_open(const char *title, enum icon_id icon, int w, int h, const struct window_ops *ops, void *data,
                       unsigned flags);
void wm_close(struct window *w);
void wm_destroy(struct window *w);
void wm_show(struct window *w);
void wm_hide(struct window *w);
void wm_focus(struct window *w);
struct window *wm_focused(void);
void wm_invalidate(struct window *w);
void wm_set_pinned(struct window *w, bool on);
void wm_peek_desktop(bool on);
void wm_set_title(struct window *w, const char *title);
void wm_set_timer(struct window *w, uint32_t ms);
void wm_set_menus(struct window *w, const struct app_menu *menus, int count);
void wm_move(struct window *w, int x, int y);
void wm_resize_client(struct window *w, int cw, int ch);
void wm_toggle_maximize(struct window *w);
void wm_minimize(struct window *w);
struct rect wm_client_rect(const struct window *w);
struct window *wm_popup(int x, int y, int w, int h, const struct window_ops *ops, void *data, struct window *owner);
void wm_screen_size(int *w, int *h);
int wm_screen_count(void);
struct rect wm_screen_rect(int index);
struct rect wm_work_area(int index);
int wm_screen_at(int x, int y);
void wm_damage(struct rect r);
void wm_damage_client(struct window *w, struct rect r);
bool wm_window_alive(const struct window *w);
void wm_foreach(void (*fn)(struct window *w, void *ctx), void *ctx);

struct ui_state {
    int hot;
    int active;
    int focus;
    int drag_value;
};

struct ui {
    struct window *win;
    struct canvas *c;
    struct gui_event *e;
    struct ui_state *st;
    int id;
    int hot;
    bool changed;
    bool consumed;
};

struct ui_text {
    char buf[256];
    int len;
    int cursor;
    int scroll;
    bool password;
};

struct ui_list {
    int count;
    int selected;
    int scroll;
    int row_h;
    bool zebra;
};

enum ui_list_result {
    LIST_NONE,
    LIST_SELECT,
    LIST_ACTIVATE,
    LIST_CONTEXT,
};

typedef void (*ui_frame_fn)(struct ui *ui, void *data);
typedef void (*ui_list_draw_fn)(struct canvas *c, struct rect row, int index, bool selected, void *ctx);
typedef void (*ui_pick_fn)(struct window *owner, int index);
typedef void (*ui_prompt_fn)(struct window *owner, const char *value);

void ui_run(struct window *w, struct ui_state *st, struct canvas *c, struct gui_event *e, ui_frame_fn frame,
            void *data);
int ui_next_id(struct ui *ui);
bool ui_hover(struct ui *ui, struct rect r);
bool ui_button(struct ui *ui, struct rect r, const char *label);
bool ui_button_primary(struct ui *ui, struct rect r, const char *label);
bool ui_tool(struct ui *ui, struct rect r, enum icon_id icon, const char *label, bool toggled);
bool ui_symbol_button(struct ui *ui, struct rect r, enum symbol_id sym, bool toggled);
bool ui_checkbox(struct ui *ui, struct rect r, const char *label, bool *value);
bool ui_switch(struct ui *ui, struct rect r, bool *value);
bool ui_radio(struct ui *ui, struct rect r, const char *label, bool selected);
bool ui_slider(struct ui *ui, struct rect r, int *value, int min, int max);
bool ui_segmented(struct ui *ui, struct rect r, const char *const *labels, int count, int *selected);
void ui_label(struct ui *ui, struct rect r, const char *text, const struct ui_font *f, uint32_t color,
              enum text_align align);
void ui_panel(struct ui *ui, struct rect r, uint32_t color);
void ui_separator(struct ui *ui, int x, int y, int w);
void ui_text_set(struct ui_text *t, const char *s);
bool ui_text_edit(struct ui_text *t, int key);
bool ui_textbox(struct ui *ui, struct rect r, struct ui_text *t);
bool ui_textbox_hint(struct ui *ui, struct rect r, struct ui_text *t, const char *hint);
bool ui_search(struct ui *ui, struct rect r, struct ui_text *t, const char *hint);
int ui_list(struct ui *ui, struct rect r, struct ui_list *l, ui_list_draw_fn draw, void *ctx);
uint32_t ui_row_text(bool selected);
bool ui_scrollbar(struct ui *ui, struct rect r, int *pos, int content, int view);
bool ui_sidebar_item(struct ui *ui, struct rect r, enum symbol_id sym, const char *label, bool selected);
void ui_sidebar_header(struct ui *ui, struct rect r, const char *label);
void ui_sidebar_panel(struct ui *ui, int width, int height, uint32_t back);
struct rect ui_group(struct ui *ui, int x, int y, int w, int rows, int row_h);
void ui_group_label(struct ui *ui, struct rect row, const char *label, const char *detail, bool last);
void ui_settings_icon(struct canvas *c, int x, int y, int size, enum symbol_id sym, uint32_t color);
int ui_key(struct ui *ui);
void ui_menu(struct window *owner, int x, int y, const char *const *items, int count, ui_pick_fn pick);
struct window *ui_menu_items(struct window *owner, int x, int y, const struct menu_item *items, int count,
                             ui_pick_fn pick);
void ui_confirm(struct window *owner, const char *title, const char *text, ui_prompt_fn done);
bool ui_alert(struct window *owner, const char *title, const char *text, enum icon_id icon,
              const char *const *buttons, int count, ui_pick_fn pick);
void ui_prompt(struct window *owner, const char *title, const char *label, const char *initial, ui_prompt_fn done);
void ui_prompt_secret(struct window *owner, const char *title, const char *label, ui_prompt_fn done);
int ui_wrap_text(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color);

struct gui_app {
    const char *name;
    const char *description;
    enum icon_id icon;
    void (*launch)(const char *arg);
    const char *extensions;
    const struct canvas *picture;
};

#define GUI_APP(object) SYNTH_REGISTER(synth_gui_apps, struct gui_app, object)

const struct gui_app *gui_app_find(const char *name);
const struct gui_app *gui_app_for_program(const char *path);
void gui_app_foreach(void (*fn)(const struct gui_app *app, void *ctx), void *ctx);
void gui_app_launch(const struct gui_app *app, const char *arg);
bool gui_open_file(const char *path);
bool gui_program_icon(const char *path, int size, struct canvas *out);
void gui_message(const char *title, const char *text);
void gui_notify(const char *title, const char *text);
bool gui_eject(const char *path);
void gui_terminal_run(const char *command);
void gui_terminal_show(void);
bool gui_active(void);
void gui_style_refresh(void);
void gui_wallpaper_refresh(void);
const char *gui_clipboard(void);
void gui_set_clipboard(const char *text, size_t n);
void gui_quit(int action);
const char *gui_user(void);
void gui_lock(void);

enum {
    QUIT_TEXT,
    QUIT_REBOOT,
    QUIT_POWEROFF,
    QUIT_LOGOUT,
};

struct shell;
int gui_run(struct shell *sh);

#endif
