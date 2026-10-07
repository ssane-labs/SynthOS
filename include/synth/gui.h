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
    const uint8_t *alpha;
};

extern const struct ui_font font_sans, font_bold, font_title, font_mono;

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
    ICON_COUNT,
};

struct icon {
    const uint32_t *large;
    const uint32_t *small;
};

extern const struct icon icons[ICON_COUNT];

struct rect rect_make(int x, int y, int w, int h);
bool rect_empty(struct rect r);
bool rect_contains(struct rect r, int x, int y);
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
struct rect rect_inset(struct rect r, int d);

uint32_t color_mix(uint32_t a, uint32_t b, int t);
uint32_t color_shade(uint32_t c, int amount);

struct canvas canvas_make(uint32_t *px, int w, int h);
bool canvas_alloc(struct canvas *c, int w, int h);
void canvas_free(struct canvas *c);
void canvas_clip(struct canvas *c, struct rect r);

void gfx_fill(struct canvas *c, struct rect r, uint32_t color);
void gfx_frame(struct canvas *c, struct rect r, uint32_t color);
void gfx_gradient(struct canvas *c, struct rect r, uint32_t top, uint32_t bottom);
void gfx_round(struct canvas *c, struct rect r, int radius, uint32_t color);
void gfx_round_frame(struct canvas *c, struct rect r, int radius, uint32_t color);
void gfx_round_gradient(struct canvas *c, struct rect r, int radius, uint32_t top, uint32_t bottom);
void gfx_shadow(struct canvas *c, struct rect r, int radius, int size, int strength);
void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_thick_line(struct canvas *c, int x0, int y0, int x1, int y1, int width, uint32_t color);
void gfx_circle(struct canvas *c, int cx, int cy, int r, uint32_t color);
void gfx_pixel(struct canvas *c, int x, int y, uint32_t color);
void gfx_blit(struct canvas *c, int x, int y, const struct canvas *src, struct rect from);
void gfx_image(struct canvas *c, int x, int y, const uint32_t *argb, int w, int h);
void gfx_icon(struct canvas *c, int x, int y, enum icon_id id, int size);

enum text_align {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT,
};

uint32_t utf8_next(const char **s);
int gfx_text(struct canvas *c, int x, int y, const struct ui_font *f, const char *s, uint32_t color);
int gfx_text_n(struct canvas *c, int x, int y, const struct ui_font *f, const char *s, size_t n, uint32_t color);
int gfx_text_width(const struct ui_font *f, const char *s);
int gfx_text_width_n(const struct ui_font *f, const char *s, size_t n);
void gfx_text_box(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color,
                  enum text_align align);
void gfx_cell(struct canvas *c, int x, int y, uint8_t ch, uint32_t fg, uint32_t bg);

struct gui_style {
    uint32_t desktop;
    uint32_t window;
    uint32_t panel;
    uint32_t border;
    uint32_t title_active_top;
    uint32_t title_active_bottom;
    uint32_t title_inactive;
    uint32_t title_text;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t accent2;
    uint32_t selection;
    uint32_t button;
    uint32_t button_hover;
    uint32_t field;
    uint32_t taskbar;
    uint32_t error;
    uint32_t ok;
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
};

enum window_flags {
    WIN_RESIZABLE = 1u << 0,
    WIN_MAXIMIZED = 1u << 1,
    WIN_MINIMIZED = 1u << 2,
    WIN_HIDDEN = 1u << 3,
    WIN_POPUP = 1u << 4,
    WIN_DIALOG = 1u << 5,
    WIN_KEEP = 1u << 6,
};

struct window;

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
};

#define TITLE_HEIGHT 30
#define BORDER 1

struct window *wm_open(const char *title, enum icon_id icon, int w, int h, const struct window_ops *ops, void *data,
                       unsigned flags);
void wm_close(struct window *w);
void wm_destroy(struct window *w);
void wm_show(struct window *w);
void wm_hide(struct window *w);
void wm_focus(struct window *w);
struct window *wm_focused(void);
void wm_invalidate(struct window *w);
void wm_set_title(struct window *w, const char *title);
void wm_set_timer(struct window *w, uint32_t ms);
void wm_move(struct window *w, int x, int y);
void wm_resize_client(struct window *w, int cw, int ch);
void wm_toggle_maximize(struct window *w);
void wm_minimize(struct window *w);
struct rect wm_client_rect(const struct window *w);
struct window *wm_popup(int x, int y, int w, int h, const struct window_ops *ops, void *data, struct window *owner);
void wm_screen_size(int *w, int *h);
void wm_damage(struct rect r);
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
};

struct ui_list {
    int count;
    int selected;
    int scroll;
    int row_h;
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
bool ui_tool(struct ui *ui, struct rect r, enum icon_id icon, const char *label, bool toggled);
bool ui_checkbox(struct ui *ui, struct rect r, const char *label, bool *value);
bool ui_radio(struct ui *ui, struct rect r, const char *label, bool selected);
bool ui_slider(struct ui *ui, struct rect r, int *value, int min, int max);
void ui_label(struct ui *ui, struct rect r, const char *text, const struct ui_font *f, uint32_t color,
              enum text_align align);
void ui_panel(struct ui *ui, struct rect r, uint32_t color);
void ui_text_set(struct ui_text *t, const char *s);
bool ui_textbox(struct ui *ui, struct rect r, struct ui_text *t);
int ui_list(struct ui *ui, struct rect r, struct ui_list *l, ui_list_draw_fn draw, void *ctx);
bool ui_scrollbar(struct ui *ui, struct rect r, int *pos, int content, int view);
int ui_key(struct ui *ui);
void ui_menu(struct window *owner, int x, int y, const char *const *items, int count, ui_pick_fn pick);
void ui_confirm(struct window *owner, const char *title, const char *text, ui_prompt_fn done);
void ui_prompt(struct window *owner, const char *title, const char *label, const char *initial, ui_prompt_fn done);
int ui_wrap_text(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color);

struct gui_app {
    const char *name;
    const char *description;
    enum icon_id icon;
    void (*launch)(const char *arg);
    const char *extensions;
};

#define GUI_APP(object) SYNTH_REGISTER(synth_gui_apps, struct gui_app, object)

const struct gui_app *gui_app_find(const char *name);
void gui_app_foreach(void (*fn)(const struct gui_app *app, void *ctx), void *ctx);
bool gui_open_file(const char *path);
void gui_message(const char *title, const char *text);
void gui_terminal_run(const char *command);
void gui_terminal_show(void);
bool gui_active(void);
void gui_style_refresh(void);
void gui_wallpaper_refresh(void);
const char *gui_clipboard(void);
void gui_set_clipboard(const char *text, size_t n);
void gui_quit(int action);

enum {
    QUIT_TEXT,
    QUIT_REBOOT,
    QUIT_POWEROFF,
};

struct shell;
int gui_run(struct shell *sh);

#endif
