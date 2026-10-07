#include <synth/config.h>
#include <synth/console.h>
#include <synth/font.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/string.h>
#include <synth/theme.h>

#include "internal.h"

#define PAD 6
#define FRAME_MS 33

struct term {
    struct window *win;
    struct display display;
    const struct cell *cells;
    int cell_cols;
    struct cell shown[CONFIG_MAX_COLS * CONFIG_MAX_ROWS];
    uint32_t colors[16];
    int cursor_x, cursor_y;
    bool cursor_on;
    enum cursor_shape shape;
    bool pending;
    uint64_t last_frame;
};

static struct term *term;

static uint32_t cell_color(int index)
{
    return term->colors[index & 15];
}

static void draw_cell(int x, int y, struct cell c, bool cursor)
{
    struct canvas *s = &term->win->surface;
    uint32_t fg = cell_color(c.attr & 15), bg = cell_color(c.attr >> 4);
    int px = PAD + x * FONT_WIDTH, py = PAD + y * FONT_HEIGHT;
    if (px + FONT_WIDTH > s->w || py + FONT_HEIGHT > s->h)
        return;
    struct canvas c2 = *s;
    c2.clip = (struct rect){0, 0, s->w, s->h};
    gfx_cell(&c2, px, py, c.ch, fg, bg);
    if (cursor) {
        if (term->shape == CURSOR_BLOCK)
            gfx_fill(&c2, (struct rect){px, py, FONT_WIDTH, FONT_HEIGHT}, ALPHA(fg, 160));
        else if (term->shape == CURSOR_BAR)
            gfx_fill(&c2, (struct rect){px, py, 2, FONT_HEIGHT}, fg);
        else
            gfx_fill(&c2, (struct rect){px, py + 13, FONT_WIDTH, 2}, fg);
    }
}

static void render(void)
{
    if (!term || !term->cells)
        return;
    int cols = MIN(term->display.cols, term->cell_cols);
    struct rect changed = {0, 0, 0, 0};
    for (int y = 0; y < term->display.rows; y++)
        for (int x = 0; x < cols; x++) {
            struct cell c = term->cells[y * term->cell_cols + x];
            struct cell *old = &term->shown[y * term->display.cols + x];
            bool cur = term->cursor_on && x == term->cursor_x && y == term->cursor_y;
            if (old->ch == c.ch && old->attr == c.attr && !cur)
                continue;
            *old = c;
            if (cur)
                old->attr ^= 0xff;
            draw_cell(x, y, c, cur);
            changed = rect_union(changed, (struct rect){PAD + x * FONT_WIDTH, PAD + y * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT});
        }
    term->pending = false;
    term->last_frame = hal_ticks_ms();
    if (!rect_empty(changed)) {
        struct rect cr = wm_client_rect(term->win);
        wm_damage((struct rect){cr.x + changed.x, cr.y + changed.y, changed.w, changed.h});
    }
}

void terminal_flush(void)
{
    if (term && term->pending)
        render();
}

static void request(void)
{
    term->pending = true;
    if (hal_ticks_ms() - term->last_frame >= FRAME_MS) {
        render();
        wm_render();
    }
}

static void t_present(const struct cell *cells, int cols, int y0, int y1)
{
    UNUSED(y0);
    UNUSED(y1);
    term->cells = cells;
    term->cell_cols = cols;
    request();
}

static void t_cursor(int x, int y, bool visible)
{
    if (x == term->cursor_x && y == term->cursor_y && visible == term->cursor_on)
        return;
    if (term->cursor_x >= 0 && term->cursor_y >= 0 && term->cursor_x < term->display.cols &&
        term->cursor_y < term->display.rows)
        term->shown[term->cursor_y * term->display.cols + term->cursor_x].attr ^= 0x5a;
    term->cursor_x = x;
    term->cursor_y = y;
    term->cursor_on = visible;
    if (visible && x >= 0 && y >= 0 && x < term->display.cols && y < term->display.rows)
        term->shown[y * term->display.cols + x].attr ^= 0xa5;
    request();
}

static void invalidate_all(void)
{
    for (int i = 0; i < term->display.cols * term->display.rows; i++)
        term->shown[i] = (struct cell){0, 0xee};
}

static void t_palette(const uint32_t rgb[16])
{
    for (int i = 0; i < 16; i++)
        term->colors[i] = 0xff000000u | rgb[i];
    invalidate_all();
    wm_invalidate(term->win);
    gui_style_refresh();
    gui_wallpaper_refresh();
    request();
}

static void t_shape(enum cursor_shape shape)
{
    term->shape = shape;
    invalidate_all();
    request();
}

static void paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_fill(c, (struct rect){0, 0, c->w, c->h}, term->colors[ATTR_BG(theme_attr(ROLE_TEXT))]);
    invalidate_all();
    render();
}

static void grid_for(struct window *w, int *cols, int *rows)
{
    struct rect cr = wm_client_rect(w);
    *cols = CLAMP((cr.w - 2 * PAD) / FONT_WIDTH, 20, CONFIG_MAX_COLS);
    *rows = CLAMP((cr.h - 2 * PAD) / FONT_HEIGHT, 5, CONFIG_MAX_ROWS);
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_KEY) {
        input_inject(e->key);
    } else if (e->type == EV_RESIZE) {
        int cols, rows;
        grid_for(w, &cols, &rows);
        term->display.cols = cols;
        term->display.rows = rows;
        invalidate_all();
        con_resize(cols, rows);
        con_redraw();
    } else if (e->type == EV_MOUSE_DOWN && e->button == BUTTON_RIGHT) {
        for (const char *p = gui_clipboard(); *p; p++)
            input_inject(*p == '\n' ? KEY_ENTER : (uint8_t)*p);
    }
}

static bool on_close(struct window *w)
{
    UNUSED(w);
    return true;
}

static const struct window_ops ops = {paint, event, on_close, NULL};

struct window *terminal_open(void)
{
    term = kzalloc(sizeof(*term));
    if (!term)
        return NULL;
    term->cursor_x = term->cursor_y = -1;
    term->shape = con_cursor_shape();
    int sw, sh;
    wm_screen_size(&sw, &sh);
    int cols = MIN(100, (sw - 80) / FONT_WIDTH), rows = MIN(30, (sh - TASKBAR_HEIGHT - 120) / FONT_HEIGHT);
    term->win = wm_open("Terminal", ICON_TERMINAL, cols * FONT_WIDTH + 2 * PAD, rows * FONT_HEIGHT + 2 * PAD, &ops,
                        term, WIN_RESIZABLE | WIN_KEEP);
    if (!term->win) {
        kfree(term);
        term = NULL;
        return NULL;
    }
    term->win->min_w = 30 * FONT_WIDTH;
    term->win->min_h = 8 * FONT_HEIGHT + TITLE_HEIGHT;
    grid_for(term->win, &cols, &rows);
    term->display = (struct display){
        .name = "desktop terminal",
        .cols = cols,
        .rows = rows,
        .caps = DISPLAY_GRID | DISPLAY_PALETTE | DISPLAY_CURSOR_SHAPE,
        .present = t_present,
        .cursor = t_cursor,
        .palette = t_palette,
        .cursor_shape = t_shape,
    };
    invalidate_all();
    con_init(&term->display);
    return term->win;
}

void terminal_close(void)
{
    kfree(term);
    term = NULL;
}

struct window *terminal_window(void)
{
    return term ? term->win : NULL;
}

static void launch_terminal(const char *arg)
{
    UNUSED(arg);
    gui_terminal_show();
}

static const struct gui_app terminal_app = {"Terminal", "Shell with all SynthOS commands", ICON_TERMINAL,
                                            launch_terminal, NULL};
GUI_APP(terminal_app);
