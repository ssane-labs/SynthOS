#ifndef SYNTH_GUI_WIDGETS_H
#define SYNTH_GUI_WIDGETS_H

#include "internal.h"

/* Desktop widgets: instances on a grid of cells, each with a kind, a size, a shape, a style and its own options. */

#define MAX_WIDGETS 16
#define WIDGET_OPT 160
#define CELL 78
#define CELL_GAP 14

enum widget_kind {
    WK_CLOCK,
    WK_WEATHER,
    WK_MUSIC,
    WK_CALENDAR,
    WK_VISUALIZER,
    WK_TASKS,
    WK_ANALOG,
    WK_WORLD,
    WK_PHOTO,
    WK_NOTES,
    WK_SYSTEM,
    WK_APPS,
    WK_COUNT,
};

enum widget_size {
    WS_SMALL,
    WS_MEDIUM,
    WS_LARGE,
    WS_SLIM,
    WS_COUNT,
};

enum widget_shape {
    SH_ROUNDED,
    SH_ROUND,
    SH_SQUARE,
    SH_COUNT,
};

enum widget_style {
    ST_GLASS,
    ST_TINTED,
    ST_SOLID,
    ST_CLEAR,
    ST_COUNT,
};

struct widget {
    int kind, size, shape, style;
    int cw, ch;
    int col, row;
    char opt[WIDGET_OPT];
    struct rect r;
    struct glass glass;
    struct spring ax, ay, intro;
    bool placed, leaving, used;
    struct canvas pic;
    int pic_index;
    uint64_t pic_next;
};

struct widget_kind_info {
    const char *name, *title;
    enum symbol_id sym;
    int cw, ch;
    int min_w, min_h, max_w, max_h;
};

extern const struct widget_kind_info widget_kinds[WK_COUNT];
extern int widget_hover_part, widget_pressed_part;
extern const struct widget *widget_hover_w, *widget_pressed_w;

uint32_t widget_ink(const struct widget *w);
uint32_t widget_soft(const struct widget *w);
void widget_text(struct canvas *c, const struct widget *w, struct rect r, const struct ui_font *f, const char *s,
                 uint32_t col, enum text_align a);
void widget_paint_kind(struct canvas *c, struct widget *w, struct rect r);
int widget_parts(const struct widget *w, struct rect r, struct rect *out, int max);
void widget_click(struct widget *w, int part, int clicks);
void widget_damage(const struct widget *w);
void widgets_layout(void);
void widget_kinds_tick(void);
void widget_free_kind(struct widget *w);
void widget_default_opt(struct widget *w);
bool widget_kind_animating(void);
struct widget *widget_list(int *n);
int widget_opt_int(const struct widget *w, const char *key, int fallback);
void widget_opt_set(struct widget *w, const char *key, int value);
#define CLOCK_FONTS 5
#define CLOCK_COLORS 12
extern const struct ui_font *const clock_fonts[CLOCK_FONTS];
extern const uint32_t clock_colors[CLOCK_COLORS];
void widgets_changed(void);

#endif
