#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "../internal.h"

#define CELL 26
#define HEADER 92
#define MIN_W 360
#define PAD 10
#define MAX_W 30
#define MAX_H 16

enum {
    C_MINE = 1 << 0,
    C_OPEN = 1 << 1,
    C_FLAG = 1 << 2,
};

enum game_state {
    G_READY,
    G_PLAYING,
    G_WON,
    G_LOST,
};

struct level {
    const char *name;
    int w, h, mines;
};

static const struct level levels[] = {
    {"Beginner", 9, 9, 10},
    {"Intermediate", 16, 16, 40},
    {"Expert", 30, 16, 99},
};

struct mines {
    struct ui_state st;
    int level;
    int w, h, count;
    uint8_t cell[MAX_H][MAX_W];
    enum game_state state;
    int opened, flags;
    uint64_t started, finished;
    int press_x, press_y;
    int boom_x, boom_y;
    uint32_t seed;
};

static const uint32_t number_colors[9] = {
    0,
    RGB(80, 160, 255),
    RGB(80, 200, 120),
    RGB(255, 90, 90),
    RGB(170, 110, 255),
    RGB(255, 160, 60),
    RGB(60, 210, 210),
    RGB(240, 240, 240),
    RGB(160, 160, 160),
};

static uint32_t rnd(struct mines *m)
{
    m->seed ^= m->seed << 13;
    m->seed ^= m->seed >> 17;
    m->seed ^= m->seed << 5;
    return m->seed;
}

static int neighbours(struct mines *m, int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < m->w && ny < m->h && (m->cell[ny][nx] & C_MINE))
                n++;
        }
    return n;
}

static void reset(struct mines *m)
{
    const struct level *l = &levels[m->level];
    m->w = l->w;
    m->h = l->h;
    m->count = l->mines;
    memset(m->cell, 0, sizeof(m->cell));
    m->state = G_READY;
    m->opened = m->flags = 0;
    m->press_x = m->press_y = -1;
    m->boom_x = m->boom_y = -1;
}

static void place_mines(struct mines *m, int sx, int sy)
{
    m->seed ^= (uint32_t)hal_ticks_ms() * 2654435761u;
    if (!m->seed)
        m->seed = 0x9e3779b9u;
    int placed = 0;
    while (placed < m->count) {
        int x = (int)(rnd(m) % (uint32_t)m->w), y = (int)(rnd(m) % (uint32_t)m->h);
        int dx = x - sx, dy = y - sy;
        if ((m->cell[y][x] & C_MINE) || (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1))
            continue;
        m->cell[y][x] |= C_MINE;
        placed++;
    }
    m->state = G_PLAYING;
    m->started = hal_ticks_ms();
}

static void finish(struct mines *m, enum game_state s)
{
    m->state = s;
    m->finished = hal_ticks_ms();
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++)
            if (s == G_WON && (m->cell[y][x] & C_MINE))
                m->cell[y][x] |= C_FLAG;
    if (s == G_WON)
        m->flags = m->count;
}

static void open_cell(struct mines *m, int x, int y)
{
    static int16_t stack[MAX_W * MAX_H * 2];
    int top = 0;
    stack[top++] = (int16_t)x;
    stack[top++] = (int16_t)y;
    while (top) {
        int cy = stack[--top], cx = stack[--top];
        uint8_t *c = &m->cell[cy][cx];
        if (*c & (C_OPEN | C_FLAG))
            continue;
        *c |= C_OPEN;
        m->opened++;
        if (*c & C_MINE) {
            m->boom_x = cx;
            m->boom_y = cy;
            finish(m, G_LOST);
            return;
        }
        if (neighbours(m, cx, cy))
            continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (nx >= 0 && ny >= 0 && nx < m->w && ny < m->h && !(m->cell[ny][nx] & C_OPEN) &&
                    top < (int)ARRAY_SIZE(stack) - 1) {
                    stack[top++] = (int16_t)nx;
                    stack[top++] = (int16_t)ny;
                }
            }
    }
    if (m->opened == m->w * m->h - m->count)
        finish(m, G_WON);
}

static void reveal(struct mines *m, int x, int y)
{
    if (m->state == G_WON || m->state == G_LOST)
        return;
    if (m->state == G_READY)
        place_mines(m, x, y);
    uint8_t c = m->cell[y][x];
    if (!(c & C_OPEN)) {
        open_cell(m, x, y);
        return;
    }
    int n = neighbours(m, x, y), flagged = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < m->w && ny < m->h && (m->cell[ny][nx] & C_FLAG))
                flagged++;
        }
    if (!n || flagged != n)
        return;
    for (int dy = -1; dy <= 1 && m->state == G_PLAYING; dy++)
        for (int dx = -1; dx <= 1 && m->state == G_PLAYING; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < m->w && ny < m->h)
                open_cell(m, nx, ny);
        }
}

static void toggle_flag(struct mines *m, int x, int y)
{
    if (m->state == G_WON || m->state == G_LOST || (m->cell[y][x] & C_OPEN))
        return;
    m->cell[y][x] ^= C_FLAG;
    m->flags += (m->cell[y][x] & C_FLAG) ? 1 : -1;
}

static void draw_mine(struct canvas *c, int cx, int cy)
{
    gfx_thick_line(c, cx - 8, cy, cx + 8, cy, 2, RGB(20, 20, 30));
    gfx_thick_line(c, cx, cy - 8, cx, cy + 8, 2, RGB(20, 20, 30));
    gfx_thick_line(c, cx - 6, cy - 6, cx + 6, cy + 6, 2, RGB(20, 20, 30));
    gfx_thick_line(c, cx - 6, cy + 6, cx + 6, cy - 6, 2, RGB(20, 20, 30));
    gfx_circle(c, cx, cy, 6, RGB(20, 20, 30));
    gfx_circle(c, cx - 2, cy - 2, 2, RGB(230, 230, 240));
}

static void draw_flag(struct canvas *c, int cx, int cy)
{
    gfx_fill(c, (struct rect){cx - 1, cy - 8, 2, 16}, RGB(30, 30, 40));
    gfx_fill(c, (struct rect){cx - 6, cy + 6, 12, 3}, RGB(30, 30, 40));
    for (int i = 0; i < 6; i++)
        gfx_fill(c, (struct rect){cx + 1, cy - 8 + i, 9 - i * 3 / 2, 1}, RGB(240, 60, 70));
    for (int i = 0; i < 5; i++)
        gfx_fill(c, (struct rect){cx + 1, cy - 2 + i, 1 + i * 2, 1}, RGB(240, 60, 70));
}

static void draw_cell(struct canvas *c, struct mines *m, int x, int y, struct rect r)
{
    uint8_t v = m->cell[y][x];
    bool over = m->state == G_LOST || m->state == G_WON;
    bool shown = (v & C_OPEN) || (over && (v & C_MINE) && !(v & C_FLAG));
    bool pressed = m->press_x == x && m->press_y == y && !(v & (C_OPEN | C_FLAG));
    struct rect in = rect_inset(r, 1);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (!shown && !pressed) {
        gfx_round_gradient(c, in, 4, color_shade(style.button, 18), style.button);
        gfx_round_frame(c, in, 4, ALPHA(0xffffffffu, 28));
        if (v & C_FLAG) {
            draw_flag(c, cx, cy);
            if (over && !(v & C_MINE)) {
                gfx_thick_line(c, cx - 8, cy - 8, cx + 8, cy + 8, 2, style.error);
                gfx_thick_line(c, cx - 8, cy + 8, cx + 8, cy - 8, 2, style.error);
            }
        }
        return;
    }
    uint32_t bg = x == m->boom_x && y == m->boom_y ? style.error : style.field;
    gfx_round(c, in, 3, bg);
    if (!shown)
        return;
    if (v & C_MINE) {
        draw_mine(c, cx, cy);
        return;
    }
    int n = neighbours(m, x, y);
    if (n) {
        char s[2] = {(char)('0' + n), 0};
        gfx_text_box(c, r, &font_title, s, number_colors[n], ALIGN_CENTER);
    }
}

static struct rect board_rect(struct window *w, struct mines *m)
{
    int bw = m->w * CELL, bh = m->h * CELL;
    return (struct rect){(w->surface.w - bw) / 2, HEADER + (w->surface.h - HEADER - PAD - bh) / 2, bw, bh};
}

static void fit(struct window *w, struct mines *m)
{
    wm_resize_client(w, MAX(m->w * CELL + 2 * PAD, MIN_W), HEADER + m->h * CELL + PAD * 2);
}

static void counter(struct canvas *c, struct rect r, int value)
{
    char s[8];
    ksnprintf(s, sizeof(s), "%03d", CLAMP(value, -99, 999));
    gfx_round(c, r, 6, RGB(16, 12, 24));
    gfx_text_box(c, r, &font_title, s, RGB(255, 70, 90), ALIGN_CENTER);
}

static void face(struct canvas *c, struct rect r, enum game_state s, bool tense)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    gfx_circle(c, cx, cy, 13, RGB(255, 214, 60));
    if (s == G_LOST) {
        gfx_line(c, cx - 7, cy - 6, cx - 3, cy - 2, RGB(40, 30, 10));
        gfx_line(c, cx - 7, cy - 2, cx - 3, cy - 6, RGB(40, 30, 10));
        gfx_line(c, cx + 3, cy - 6, cx + 7, cy - 2, RGB(40, 30, 10));
        gfx_line(c, cx + 3, cy - 2, cx + 7, cy - 6, RGB(40, 30, 10));
        gfx_fill(c, (struct rect){cx - 5, cy + 6, 10, 2}, RGB(40, 30, 10));
        return;
    }
    if (s == G_WON) {
        gfx_fill(c, (struct rect){cx - 10, cy - 6, 20, 3}, RGB(20, 20, 30));
        gfx_fill(c, (struct rect){cx - 9, cy - 5, 7, 5}, RGB(20, 20, 30));
        gfx_fill(c, (struct rect){cx + 2, cy - 5, 7, 5}, RGB(20, 20, 30));
    } else {
        gfx_circle(c, cx - 5, cy - 4, 2, RGB(40, 30, 10));
        gfx_circle(c, cx + 5, cy - 4, 2, RGB(40, 30, 10));
    }
    if (tense)
        gfx_circle(c, cx, cy + 6, 3, RGB(40, 30, 10));
    else
        for (int i = -6; i <= 6; i++)
            gfx_fill(c, (struct rect){cx + i, cy + 5 + (36 - i * i) / 14, 1, 2}, RGB(40, 30, 10));
}

static void frame(struct ui *ui, void *data)
{
    struct mines *m = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    ui_panel(ui, (struct rect){0, 0, W, H}, style.window);
    ui_panel(ui, (struct rect){0, 0, W, HEADER - 6}, style.panel);
    int lw = (W - 2 * PAD - 12) / 3;
    for (int i = 0; i < (int)ARRAY_SIZE(levels); i++)
        if (ui_tool(ui, (struct rect){PAD + i * (lw + 6), 52, lw, 28}, ICON_MINES, levels[i].name, m->level == i)) {
            m->level = i;
            reset(m);
            wm_set_timer(w, 0);
            fit(w, m);
            return;
        }
    struct rect smiley = {(W - 44) / 2, 6, 44, 40};
    if (ui_button(ui, smiley, "")) {
        reset(m);
        wm_set_timer(w, 0);
    }
    uint64_t now = m->state == G_PLAYING ? hal_ticks_ms() : m->finished;
    int secs = m->state == G_READY ? 0 : (int)((now - m->started) / 1000);
    if (ui->c) {
        counter(ui->c, (struct rect){PAD, 8, 64, 36}, m->count - m->flags);
        counter(ui->c, (struct rect){W - PAD - 64, 8, 64, 36}, secs);
        face(ui->c, smiley, m->state, m->press_x >= 0);
    }
    struct rect board = board_rect(w, m);
    struct gui_event *e = ui->e;
    if (e && (e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP || e->type == EV_MOUSE_MOVE || e->type == EV_LEAVE)) {
        int x = (e->x - board.x) / CELL, y = (e->y - board.y) / CELL;
        bool in = rect_contains(board, e->x, e->y);
        if (e->type == EV_MOUSE_DOWN && in) {
            ui->consumed = true;
            if (e->button == BUTTON_RIGHT)
                toggle_flag(m, x, y);
            else if (e->button == BUTTON_LEFT && m->state != G_WON && m->state != G_LOST) {
                m->press_x = x;
                m->press_y = y;
            }
        } else if (e->type == EV_MOUSE_MOVE && m->press_x >= 0) {
            int nx = in ? x : -2, ny = in ? y : -2;
            if (nx != m->press_x || ny != m->press_y) {
                m->press_x = nx;
                m->press_y = ny;
                ui->changed = true;
            }
        } else if (e->type == EV_MOUSE_UP && m->press_x != -1) {
            if (in && e->button == BUTTON_LEFT && m->press_x >= 0) {
                reveal(m, x, y);
                if (m->state == G_PLAYING)
                    wm_set_timer(w, 1000);
                else
                    wm_set_timer(w, 0);
            }
            m->press_x = m->press_y = -1;
        } else if (e->type == EV_LEAVE && m->press_x >= 0) {
            m->press_x = m->press_y = -2;
        }
    }
    if (ui->c)
        for (int y = 0; y < m->h; y++)
            for (int x = 0; x < m->w; x++)
                draw_cell(ui->c, m, x, y, (struct rect){board.x + x * CELL, board.y + y * CELL, CELL, CELL});
    int k = ui_key(ui);
    if (k == KEY_F2 || k == 'n' || k == 'N') {
        reset(m);
        wm_set_timer(w, 0);
    } else if (k >= '1' && k <= '3') {
        m->level = k - '1';
        reset(m);
        wm_set_timer(w, 0);
        fit(w, m);
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct mines *m = w->data;
    ui_run(w, &m->st, c, NULL, frame, m);
}

static void event(struct window *w, struct gui_event *e)
{
    struct mines *m = w->data;
    ui_run(w, &m->st, NULL, e, frame, m);
}

static void destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    struct mines *m = kzalloc(sizeof(*m));
    if (!m)
        return;
    m->seed = (uint32_t)hal_ticks_ms() | 1;
    reset(m);
    struct window *w = wm_open("Minesweeper", ICON_MINES, MAX(m->w * CELL + 2 * PAD, MIN_W), HEADER + m->h * CELL + PAD * 2,
                               &ops, m, 0);
    if (!w)
        kfree(m);
}

static const struct gui_app mines_app = {"Minesweeper", "Clear the field without hitting a mine", ICON_MINES, launch,
                                         NULL};
GUI_APP(mines_app);
