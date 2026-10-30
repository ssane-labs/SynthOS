#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "widgets.h"

/* Desktop widgets live on the desktop layer under the windows. They sit on a grid of cells: a drag moves a widget
   freely and it settles into the nearest free cells when it is let go. Each look keeps its own widgets in
   ~/.widgets, one per line: "<look> <kind> <size> <shape> <style> <col> <row> <options>". */

#define S 1u << WS_SMALL
#define M 1u << WS_MEDIUM
#define L 1u << WS_LARGE
#define T 1u << WS_SLIM

const struct widget_kind_info widget_kinds[WK_COUNT] = {
    {"clock", "Clock", SYM_CLOCK, WS_MEDIUM, S | M | L | T},
    {"weather", "Weather", SYM_WEATHER, WS_MEDIUM, S | M | L | T},
    {"music", "Music", SYM_MUSIC, WS_MEDIUM, S | M | T},
    {"calendar", "Calendar", SYM_CALENDAR, WS_MEDIUM, S | M | L},
    {"visualizer", "Visualizer", SYM_VOLUME, WS_MEDIUM, S | M | L | T},
    {"tasks", "Tasks", SYM_LIST, WS_MEDIUM, S | M | L},
    {"analog", "Analog Clock", SYM_CLOCK, WS_SMALL, S | L},
    {"worldclock", "World Clock", SYM_GLOBE, WS_SMALL, S | M | T},
    {"photo", "Photo", SYM_PHOTO, WS_SMALL, S | M | L},
    {"notes", "Notes", SYM_PENCIL, WS_SMALL, S | M | L},
    {"system", "System", SYM_CHIP, WS_SMALL, S | M | T},
    {"apps", "Apps", SYM_APPS, WS_SMALL, S | M | T},
};

#undef S
#undef M
#undef L
#undef T

static const char *const size_names[WS_COUNT] = {"small", "medium", "large", "slim"};
static const char *const size_titles[WS_COUNT] = {"Small", "Medium", "Large", "Slim"};
static const char *const shape_names[SH_COUNT] = {"rounded", "round", "square"};
static const char *const shape_titles[SH_COUNT] = {"Rounded", "Round", "Square"};
static const char *const style_names[ST_COUNT] = {"glass", "tinted", "solid", "clear"};
static const char *const style_titles[ST_COUNT] = {"Glass", "Tinted", "Solid", "Clear"};
static const int size_cells[WS_COUNT][2] = {{2, 2}, {4, 2}, {4, 4}, {4, 1}};

static struct widget ws[MAX_WIDGETS];
static int loaded_look = -1;
static bool started, editing;
static struct spring edit_pop;
static struct window *gallery, *inspector;
static int drag = -1, grab_x, grab_y, press_x, press_y, drop_col, drop_row;
static bool dragging;
static int pressed = -1, hover = -1;
static uint64_t last_tick;

int widget_hover_part = -1, widget_pressed_part = -1;
const struct widget *widget_hover_w, *widget_pressed_w;

/* ---- storage */

static int look_index(void)
{
    return CLAMP((int)(look_current() - look_get(0)), 0, 7);
}

static void file_path(char *out, size_t cap)
{
    path_join(user_home(), ".widgets", out, cap);
}

static int find_name(const char *const *names, int n, const char *s, size_t len)
{
    for (int i = 0; i < n; i++)
        if (strlen(names[i]) == len && !strncmp(names[i], s, len))
            return i;
    return -1;
}

static int kind_named(const char *s, size_t len)
{
    for (int i = 0; i < WK_COUNT; i++)
        if (strlen(widget_kinds[i].name) == len && !strncmp(widget_kinds[i].name, s, len))
            return i;
    return -1;
}

static struct widget *add_widget(int kind)
{
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (!ws[i].used) {
            struct widget *w = &ws[i];
            memset(w, 0, sizeof(*w));
            w->used = true;
            w->kind = kind;
            w->size = widget_kinds[kind].size;
            w->shape = kind == WK_ANALOG ? SH_ROUND : SH_ROUNDED;
            w->style = kind == WK_CLOCK ? ST_CLEAR : ST_GLASS;
            w->col = w->row = -1;
            widget_default_opt(w);
            return w;
        }
    return NULL;
}

static void drop_widget(struct widget *w)
{
    if (w->glass.registered)
        glass_unregister(&w->glass);
    widget_free_kind(w);
    w->used = false;
}

static void clear_all(void)
{
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (ws[i].used)
            drop_widget(&ws[i]);
}

static char *next_word(char **p, char *end, size_t *len)
{
    while (*p < end && **p == ' ')
        (*p)++;
    char *s = *p;
    while (*p < end && **p != ' ')
        (*p)++;
    *len = (size_t)(*p - s);
    return s;
}

/* One line of ~/.widgets for the current look; the old form "dune clock,weather" still reads. */
static void parse_line(char *p, char *end)
{
    size_t n;
    char *kind = next_word(&p, end, &n);
    if (memchr(kind, ',', n) || (p >= end && kind_named(kind, n) >= 0)) {
        for (char *k = kind, *kend = kind + n; k < kend;) {
            char *comma = memchr(k, ',', (size_t)(kend - k));
            size_t kn = comma ? (size_t)(comma - k) : (size_t)(kend - k);
            int id = kind_named(k, kn);
            if (id >= 0)
                add_widget(id);
            k += kn + 1;
        }
        return;
    }
    int id = kind_named(kind, n);
    if (id < 0)
        return;
    struct widget *w = add_widget(id);
    if (!w)
        return;
    char *s = next_word(&p, end, &n);
    int v = find_name(size_names, WS_COUNT, s, n);
    if (v >= 0 && (widget_kinds[id].sizes >> v & 1))
        w->size = v;
    s = next_word(&p, end, &n);
    if ((v = find_name(shape_names, SH_COUNT, s, n)) >= 0)
        w->shape = v;
    s = next_word(&p, end, &n);
    if ((v = find_name(style_names, ST_COUNT, s, n)) >= 0)
        w->style = v;
    long col = -1, row = -1;
    char num[12];
    s = next_word(&p, end, &n);
    memcpy(num, s, MIN(n, sizeof(num) - 1));
    num[MIN(n, sizeof(num) - 1)] = 0;
    str_parse_long(num, &col);
    s = next_word(&p, end, &n);
    memcpy(num, s, MIN(n, sizeof(num) - 1));
    num[MIN(n, sizeof(num) - 1)] = 0;
    str_parse_long(num, &row);
    w->col = (int)col;
    w->row = (int)row;
    while (p < end && *p == ' ')
        p++;
    if (p < end) {
        size_t k = MIN((size_t)(end - p), sizeof(w->opt) - 1);
        memcpy(w->opt, p, k);
        w->opt[k] = 0;
    }
}

static void load(void)
{
    clear_all();
    loaded_look = look_index();
    const char *look = look_current()->name;
    char path[PATH_MAX_LEN], *data;
    size_t size;
    file_path(path, sizeof(path));
    if (vfs_read_all(path, &data, &size) != 0)
        return;
    size_t ln = strlen(look);
    for (char *p = data, *end = data + size; p < end;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *le = nl ? nl : end;
        if ((size_t)(le - p) > ln && !strncmp(p, look, ln) && p[ln] == ' ')
            parse_line(p + ln + 1, le);
        p = le + 1;
    }
    kfree(data);
}

static void ensure_loaded(void)
{
    if (loaded_look != look_index())
        load();
}

static void save(void)
{
    char path[PATH_MAX_LEN], *data = NULL;
    size_t size = 0;
    file_path(path, sizeof(path));
    vfs_read_all(path, &data, &size);
    size_t cap = size + MAX_WIDGETS * (WIDGET_OPT + 96) + 64, n = 0;
    char *out = kmalloc(cap);
    if (!out) {
        kfree(data);
        return;
    }
    const char *look = look_current()->name;
    size_t ln = strlen(look);
    for (char *p = data, *end = data + size; p && p < end;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *le = nl ? nl : end;
        bool mine = (size_t)(le - p) > ln && !strncmp(p, look, ln) && p[ln] == ' ';
        if (!mine && le > p) {
            memcpy(out + n, p, (size_t)(le - p));
            n += (size_t)(le - p);
            out[n++] = '\n';
        }
        p = le + 1;
    }
    bool any = false;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        struct widget *w = &ws[i];
        if (!w->used || w->leaving)
            continue;
        any = true;
        n += (size_t)ksnprintf(out + n, cap - n, "%s %s %s %s %s %d %d%s%s\n", look, widget_kinds[w->kind].name,
                               size_names[w->size], shape_names[w->shape], style_names[w->style], w->col, w->row,
                               w->opt[0] ? " " : "", w->opt);
    }
    if (!any)
        n += (size_t)ksnprintf(out + n, cap - n, "%s none\n", look);
    vfs_write_all(path, out, n);
    kfree(out);
    kfree(data);
}

struct widget *widget_list(int *n)
{
    *n = MAX_WIDGETS;
    return ws;
}

void widgets_changed(void)
{
    save();
    widgets_layout();
    gui_desktop_changed();
}

/* ---- the grid */

static struct rect grid_area(void)
{
    struct rect a = wm_work_area(0);
    return (struct rect){a.x + 20, a.y + 14, a.w - 40, a.h - 24};
}

static int grid_cols(void)
{
    return MAX(1, (grid_area().w + CELL_GAP) / (CELL + CELL_GAP));
}

static int grid_rows(void)
{
    return MAX(1, (grid_area().h + CELL_GAP) / (CELL + CELL_GAP));
}

static struct rect cell_rect(int col, int row, int size)
{
    struct rect a = grid_area();
    int cw = size_cells[size][0], ch = size_cells[size][1];
    return (struct rect){a.x + col * (CELL + CELL_GAP), a.y + row * (CELL + CELL_GAP), cw * CELL + (cw - 1) * CELL_GAP,
                         ch * CELL + (ch - 1) * CELL_GAP};
}

static bool fits(int col, int row, int size, const struct widget *except)
{
    int cw = size_cells[size][0], ch = size_cells[size][1];
    if (col < 0 || row < 0 || col + cw > grid_cols() || row + ch > grid_rows())
        return false;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        const struct widget *o = &ws[i];
        if (!o->used || o == except || o->leaving || o->col < 0)
            continue;
        int ow = size_cells[o->size][0], oh = size_cells[o->size][1];
        if (col < o->col + ow && o->col < col + cw && row < o->row + oh && o->row < row + ch)
            return false;
    }
    return true;
}

/* The free place nearest to (col, row); new widgets start from the top right corner. */
static bool nearest_free(int col, int row, int size, const struct widget *except, int *out_col, int *out_row)
{
    int best = -1, bc = 0, br = 0;
    for (int r = 0; r < grid_rows(); r++)
        for (int c = 0; c < grid_cols(); c++) {
            if (!fits(c, r, size, except))
                continue;
            int d = (c - col) * (c - col) * 4 + (r - row) * (r - row) * 5;
            if (best < 0 || d < best)
                best = d, bc = c, br = r;
        }
    if (best < 0)
        return false;
    *out_col = bc;
    *out_row = br;
    return true;
}

static bool place(struct widget *w)
{
    int cw = size_cells[w->size][0];
    int want_col = w->col >= 0 ? w->col : grid_cols() - cw, want_row = w->row >= 0 ? w->row : 0;
    int c, r;
    if (fits(want_col, want_row, w->size, w)) {
        w->col = want_col;
        w->row = want_row;
    } else if (nearest_free(want_col, want_row, w->size, w, &c, &r)) {
        w->col = c;
        w->row = r;
    } else {
        w->col = MAX(0, grid_cols() - cw);
        w->row = 0;
        return false;
    }
    return true;
}

/* A new widget takes the free cells nearest the top right corner, or is not added when there are none. */
static bool place_new(struct widget *w)
{
    if (place(w)) {
        spring_set(&w->intro, -500);
        w->intro.target = 1024;
        return true;
    }
    drop_widget(w);
    gui_notify("Widgets", "There is no room on the desktop for another widget of this size.");
    return false;
}

int widgets_obstacles(struct rect *out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_WIDGETS && n < max; i++)
        if (started && ws[i].used && !ws[i].leaving)
            out[n++] = ws[i].r;
    return n;
}

int widgets_right_reserved(void)
{
    if (!started)
        return 0;
    struct rect s = wm_screen_rect(0);
    int reserved = 0;
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (ws[i].used && !ws[i].leaving && ws[i].r.x > s.x + s.w / 2)
            reserved = MAX(reserved, s.x + s.w - ws[i].r.x + 8);
    return reserved;
}

void widget_damage(const struct widget *w)
{
    if (w && w->used)
        wm_damage_z(rect_inset(w->r, -30), Z_DESKTOP);
}

void widgets_layout(void)
{
    for (int i = 0; i < MAX_WIDGETS; i++) {
        struct widget *w = &ws[i];
        if (!w->used)
            continue;
        if (w->col < 0 && !w->leaving)
            place(w);
        struct rect old = w->r, cell = cell_rect(w->col, w->row, w->size);
        if (!w->placed) {
            spring_set(&w->ax, cell.x);
            spring_set(&w->ay, cell.y);
            w->placed = true;
        }
        if (i != drag || !dragging) {
            w->ax.target = cell.x;
            w->ay.target = cell.y;
        }
        int rise = (1024 - CLAMP(w->intro.pos, -400, 1024)) * 24 / 1024;
        w->r = (struct rect){w->ax.pos, w->ay.pos + rise, cell.w, cell.h};
        bool glassy = w->style == ST_GLASS || w->style == ST_TINTED;
        if (glassy) {
            w->glass.z = Z_DESKTOP + 1;
            glass_set_frame(&w->glass, w->r);
            if (!w->glass.registered)
                glass_register(&w->glass);
        } else if (w->glass.registered) {
            glass_unregister(&w->glass);
        }
        wm_damage_z(rect_union(rect_inset(old, -30), rect_inset(w->r, -30)), Z_DESKTOP);
    }
}

/* ---- painting */

uint32_t widget_ink(const struct widget *w)
{
    if (w->style == ST_SOLID)
        return style.text;
    if (w->style == ST_TINTED)
        return RGB(255, 255, 255);
    if (w->style == ST_CLEAR)
        return style.dark || look_current()->dock == DOCK_CORNER ? RGB(255, 255, 255) : style.text;
    return style.dark ? RGB(246, 244, 250) : color_mix(RGB(30, 26, 24), style.tint | 0xff000000u, 60);
}

uint32_t widget_soft(const struct widget *w)
{
    return ALPHA(widget_ink(w), 165);
}

void widget_text(struct canvas *c, const struct widget *w, struct rect r, const struct ui_font *f, const char *s,
                 uint32_t col, enum text_align a)
{
    if (w->style == ST_CLEAR)
        gfx_text_box(c, (struct rect){r.x + 1, r.y + 2, r.w, r.h}, f, s, ALPHA(0, style.dark ? 140 : 70), a);
    gfx_text_box(c, r, f, s, col, a);
}

static int radius_of(const struct widget *w)
{
    int m = MIN(w->r.w, w->r.h);
    switch (w->shape) {
    case SH_ROUND: return m / 2;
    case SH_SQUARE: return 10;
    default: return MIN(28, m / 4);
    }
}

static struct rect content_rect(const struct widget *w)
{
    if (w->shape != SH_ROUND)
        return w->r;
    if (w->r.w == w->r.h)
        return rect_inset(w->r, w->r.w * 3 / 20);
    return (struct rect){w->r.x + w->r.h / 3, w->r.y, w->r.w - w->r.h * 2 / 3, w->r.h};
}

static void card(struct canvas *c, struct widget *w, bool lifted)
{
    int rad = radius_of(w);
    if (lifted)
        gfx_drop_shadow(c, w->r, rad, 18, style.dark ? 90 : 50, 8);
    switch (w->style) {
    case ST_CLEAR:
        if (lifted || editing)
            gfx_round(c, w->r, rad, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 18));
        return;
    case ST_SOLID:
        gfx_round(c, w->r, rad, style.dark ? RGB(38, 38, 42) : RGB(250, 250, 252));
        gfx_round_frame(c, w->r, rad, style.dark ? ALPHA(0xffffffffu, 26) : ALPHA(0, 26));
        return;
    default: {
        struct glass_look look;
        glass_look_for(&look, GLASS_PANEL, rad);
        glass_draw(c, &w->glass, &look);
        if (w->style == ST_TINTED)
            gfx_round(c, w->r, rad, ALPHA(style.accent, 150));
        gfx_round_frame(c, w->r, rad, style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 90));
    }
    }
}

void widgets_paint(struct canvas *c, struct rect area)
{
    if (!started)
        return;
    if (dragging && drag >= 0) {
        struct rect g = cell_rect(drop_col, drop_row, ws[drag].size);
        if (!rect_empty(rect_intersect(rect_inset(g, -4), area))) {
            int rad = radius_of(&ws[drag]);
            gfx_round(c, g, rad, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 26));
            gfx_round_frame(c, g, rad, ALPHA(0xffffffffu, 120));
        }
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MAX_WIDGETS; i++) {
            struct widget *w = &ws[i];
            bool top = i == drag && dragging;
            if (!w->used || top != (pass == 1) || rect_empty(rect_intersect(rect_inset(w->r, -30), area)))
                continue;
            if (CLAMP(w->intro.pos, 0, 1024) < 16)
                continue;
            card(c, w, top);
            struct canvas clip = *c;
            clip.clip = rect_intersect(c->clip, rect_inset(w->r, -2));
            widget_paint_kind(&clip, w, content_rect(w));
            int k = CLAMP(edit_pop.pos, 0, 1200);
            if (k > 16 && !w->leaving) {
                int br = 11 * k / 1024, cx = w->r.x + 10, cy = w->r.y + 10;
                gfx_circle(c, cx, cy, br, style.dark ? RGB(70, 68, 80) : RGB(60, 58, 64));
                gfx_fill(c, (struct rect){cx - br / 2, cy - 1, br, 2}, RGB(255, 255, 255));
            }
        }
}

/* ---- input */

static int widget_at(int x, int y)
{
    for (int i = MAX_WIDGETS - 1; i >= 0; i--)
        if (ws[i].used && !ws[i].leaving && rect_contains(ws[i].r, x, y))
            return i;
    return -1;
}

static int part_at(int id, int x, int y)
{
    struct rect parts[16];
    int n = widget_parts(&ws[id], content_rect(&ws[id]), parts, 16);
    for (int i = 0; i < n; i++)
        if (rect_contains(rect_inset(parts[i], -3), x, y))
            return i;
    return -1;
}

static void remove_widget(struct widget *w)
{
    if (!started) {
        drop_widget(w);
        save();
        return;
    }
    w->leaving = true;
    w->intro.target = -400;
    save();
    gui_desktop_changed();
}

static struct widget *menu_target;
static struct menu_item menu_items[12];
static int menu_size[12];

static void inspector_open(struct widget *w);

static void menu_pick(struct window *owner, int index)
{
    UNUSED(owner);
    struct widget *w = menu_target;
    if (!w || !w->used || index < 0 || index >= 12)
        return;
    int act = menu_size[index];
    if (act >= 0 && act < WS_COUNT) {
        w->size = act;
        if (!fits(w->col, w->row, w->size, w)) {
            int c, r;
            if (nearest_free(w->col, w->row, w->size, w, &c, &r))
                w->col = c, w->row = r;
        }
        save();
        widgets_layout();
        gui_desktop_changed();
    } else if (act == 100) {
        inspector_open(w);
    } else if (act == 101) {
        remove_widget(w);
    } else if (act == 102) {
        widgets_edit(true);
    }
}

static void context_menu(struct widget *w, int x, int y)
{
    static char labels[WS_COUNT][24];
    int n = 0;
    menu_target = w;
    menu_size[n] = 100;
    menu_items[n++] = (struct menu_item){"Edit Widget…", 0, NULL};
    menu_size[n] = -1;
    menu_items[n++] = (struct menu_item){"", 0, NULL};
    for (int s = 0; s < WS_COUNT; s++)
        if (widget_kinds[w->kind].sizes >> s & 1) {
            ksnprintf(labels[s], sizeof(labels[s]), "%s%s", w->size == s ? "✓ " : "    ", size_titles[s]);
            menu_size[n] = s;
            menu_items[n++] = (struct menu_item){labels[s], 0, NULL};
        }
    menu_size[n] = -1;
    menu_items[n++] = (struct menu_item){"", 0, NULL};
    menu_size[n] = 101;
    menu_items[n++] = (struct menu_item){"Remove Widget", 0, NULL};
    menu_size[n] = 102;
    menu_items[n++] = (struct menu_item){"Edit Widgets…", 0, NULL};
    ui_menu_items(NULL, x, y, menu_items, n, menu_pick);
}

bool widgets_mouse(int x, int y, unsigned button, bool down, int clicks)
{
    if (!started)
        return false;
    if (down) {
        int id = widget_at(x, y);
        if (editing && button == BUTTON_LEFT) {
            if (id < 0) {
                widgets_edit(false);
                return true;
            }
            int dx = x - (ws[id].r.x + 10), dy = y - (ws[id].r.y + 10);
            if (dx * dx + dy * dy <= 15 * 15) {
                remove_widget(&ws[id]);
                return true;
            }
        }
        if (id >= 0 && button == BUTTON_RIGHT) {
            context_menu(&ws[id], x, y);
            return true;
        }
        if (id < 0 || button != BUTTON_LEFT)
            return id >= 0;
        pressed = id;
        widget_pressed_w = &ws[id];
        widget_pressed_part = editing ? -1 : part_at(id, x, y);
        drag = widget_pressed_part < 0 ? id : -1;
        grab_x = x - ws[id].r.x;
        grab_y = y - ws[id].r.y;
        press_x = x;
        press_y = y;
        dragging = false;
        widget_damage(&ws[id]);
        return true;
    }
    int id = pressed;
    if (id < 0)
        return widget_at(x, y) >= 0;
    struct widget *w = &ws[id];
    pressed = -1;
    if (dragging) {
        dragging = false;
        drag = -1;
        w->col = drop_col;
        w->row = drop_row;
        save();
        widgets_layout();
        gui_desktop_changed();
    } else {
        drag = -1;
        if (widget_at(x, y) == id && part_at(id, x, y) == widget_pressed_part && !editing)
            widget_click(w, widget_pressed_part, clicks);
    }
    widget_pressed_w = NULL;
    widget_pressed_part = -1;
    widget_damage(w);
    return true;
}

void desktop_hover(int x, int y, bool over)
{
    if (desktop_icon_drag(x, y))
        return;
    desktop_icon_hover(x, over ? y : -1);
    widgets_hover(over || drag >= 0 ? x : -1, y);
}

void widgets_hover(int x, int y)
{
    if (!started)
        return;
    if (drag >= 0 && pressed == drag) {
        struct widget *w = &ws[drag];
        int mx = x - press_x, my = y - press_y;
        if (!dragging && mx * mx + my * my <= 25)
            return;
        dragging = true;
        struct rect a = grid_area();
        int nx = x - grab_x, ny = y - grab_y;
        spring_set(&w->ax, nx);
        spring_set(&w->ay, ny);
        int col = (nx - a.x + (CELL + CELL_GAP) / 2) / (CELL + CELL_GAP);
        int row = (ny - a.y + (CELL + CELL_GAP) / 2) / (CELL + CELL_GAP);
        col = CLAMP(col, 0, grid_cols() - size_cells[w->size][0]);
        row = CLAMP(row, 0, grid_rows() - size_cells[w->size][1]);
        int c = col, r = row;
        if (!fits(col, row, w->size, w) && !nearest_free(col, row, w->size, w, &c, &r))
            c = w->col, r = w->row;
        if (c != drop_col || r != drop_row) {
            wm_damage_z(rect_inset(cell_rect(drop_col, drop_row, w->size), -6), Z_DESKTOP);
            drop_col = c;
            drop_row = r;
            wm_damage_z(rect_inset(cell_rect(c, r, w->size), -6), Z_DESKTOP);
        }
        widgets_layout();
        return;
    }
    int id = x >= 0 ? widget_at(x, y) : -1;
    int part = id >= 0 && !editing ? part_at(id, x, y) : -1;
    const struct widget *hw = id >= 0 ? &ws[id] : NULL;
    if (hw != widget_hover_w || part != widget_hover_part) {
        if (widget_hover_w)
            widget_damage(widget_hover_w);
        widget_hover_w = hw;
        widget_hover_part = part;
        hover = id;
        widget_damage(hw);
    }
}

bool widgets_animate(void)
{
    if (!started)
        return false;
    uint64_t now = hal_ticks_ms();
    int dt = last_tick ? (int)MIN(now - last_tick, 64u) : 16;
    last_tick = now;
    bool moved = false;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        struct widget *w = &ws[i];
        if (!w->used)
            continue;
        moved |= spring_step(&w->intro, w->leaving ? 300 : 180, 0, dt);
        if (!(i == drag && dragging)) {
            moved |= spring_step(&w->ax, 420, 0, dt);
            moved |= spring_step(&w->ay, 420, 0, dt);
        }
        if (w->leaving && w->intro.pos <= -300) {
            widget_damage(w);
            drop_widget(w);
            moved = true;
        }
    }
    if (spring_step(&edit_pop, 420, 0, dt)) {
        moved = true;
        for (int i = 0; i < MAX_WIDGETS; i++)
            widget_damage(&ws[i]);
    }
    if (moved)
        widgets_layout();
    widget_kinds_tick();
    return moved || widget_kind_animating();
}

void widgets_start(void)
{
    if (started)
        widgets_stop();
    load();
    started = true;
    last_tick = 0;
    pressed = drag = hover = -1;
    dragging = false;
    widget_hover_w = widget_pressed_w = NULL;
    int k = 0;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        struct widget *w = &ws[i];
        if (!w->used)
            continue;
        w->glass = (struct glass){0};
        w->placed = w->leaving = false;
        spring_set(&w->intro, -600 - k++ * 200);
        w->intro.target = 1024;
    }
    widgets_layout();
}

void widgets_stop(void)
{
    if (!started)
        return;
    widgets_edit(false);
    if (inspector && wm_window_alive(inspector))
        wm_destroy(inspector);
    started = false;
    for (int i = 0; i < MAX_WIDGETS; i++) {
        if (ws[i].glass.registered)
            glass_unregister(&ws[i].glass);
        if (ws[i].used)
            wm_damage_z(rect_inset(ws[i].r, -30), Z_DESKTOP);
    }
}

void widgets_user_changed(void)
{
    loaded_look = -1;
    if (started)
        widgets_start();
}

/* ---- adding: the gallery of edit mode */

static void add_kind(int kind)
{
    ensure_loaded();
    struct widget *w = add_widget(kind);
    if (!w)
        return;
    if (started) {
        if (!place_new(w))
            return;
        widgets_layout();
    }
    save();
    gui_desktop_changed();
}

#define TILE_W 92
#define TILE_H 84
#define GALLERY_COLS 6
#define GALLERY_H (2 * TILE_H + 24)
#define DONE_W 88

static int gallery_hover = -1;

static struct rect tile_rect(int i)
{
    return (struct rect){16 + (i % GALLERY_COLS) * TILE_W, 12 + (i / GALLERY_COLS) * TILE_H, TILE_W - 6, TILE_H - 6};
}

static struct rect done_rect(struct window *w)
{
    return (struct rect){w->frame.w - DONE_W - 18, (GALLERY_H - 36) / 2, DONE_W, 36};
}

static void gallery_paint(struct window *w, struct canvas *c)
{
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    for (int i = 0; i < WK_COUNT; i++) {
        struct rect t = tile_rect(i);
        if (gallery_hover == i)
            gfx_round(c, t, 16, style.hover);
        int cx = t.x + t.w / 2, cy = t.y + 28;
        gfx_circle(c, cx, cy, 22, ALPHA(style.dark ? 0xffffffffu : 0xff000000u, 22));
        gfx_symbol(c, cx - 8, cy - 8, widget_kinds[i].sym, 16, style.text);
        int bx = cx + 17, by = cy - 16;
        gfx_circle(c, bx, by, 8, style.accent);
        gfx_symbol(c, bx - 7, by - 7, SYM_PLUS, 14, style.accent_text);
        gfx_text_box(c, (struct rect){t.x, t.y + 54, t.w, 18}, &font_small, widget_kinds[i].title, style.text,
                     ALIGN_CENTER);
    }
    struct rect d = done_rect(w);
    gfx_round(c, d, 18, gallery_hover == 100 ? color_shade(style.accent, 24) : style.accent);
    gfx_text_box(c, d, &font_bold, "Done", style.accent_text, ALIGN_CENTER);
}

static int gallery_at(struct window *w, int x, int y)
{
    if (rect_contains(done_rect(w), x, y))
        return 100;
    for (int i = 0; i < WK_COUNT; i++)
        if (rect_contains(tile_rect(i), x, y))
            return i;
    return -1;
}

static void gallery_event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_MOUSE_MOVE) {
        int h = gallery_at(w, e->x, e->y);
        if (h != gallery_hover) {
            gallery_hover = h;
            wm_invalidate(w);
        }
    } else if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
    } else if (e->type == EV_MOUSE_UP && e->button == BUTTON_LEFT) {
        e->handled = true;
        int h = gallery_at(w, e->x, e->y);
        if (h == 100)
            widgets_edit(false);
        else if (h >= 0)
            add_kind(h);
    } else if (e->type == EV_KEY && KEY_BASE(e->key) == KEY_ESC) {
        widgets_edit(false);
    }
}

static void gallery_destroy(struct window *w)
{
    if (gallery == w)
        gallery = NULL;
    if (editing) {
        editing = false;
        edit_pop.target = 0;
        wm_peek_desktop(false);
    }
}

static const struct window_ops gallery_ops = {gallery_paint, gallery_event, NULL, gallery_destroy};

void widgets_edit(bool on_)
{
    if (on_ == editing || (on_ && !started))
        return;
    editing = on_;
    if (on_ && edit_pop.pos <= 0)
        spring_set(&edit_pop, 0);
    edit_pop.target = on_ ? 1024 : 0;
    wm_peek_desktop(on_);
    if (!on_) {
        if (gallery && wm_window_alive(gallery))
            wm_destroy(gallery);
        gallery = NULL;
        return;
    }
    struct rect s = wm_screen_rect(0);
    int w = GALLERY_COLS * TILE_W + 32 + DONE_W + 20;
    gallery_hover = -1;
    wm_close_popups();
    gallery = wm_popup(s.x + 24, s.y + s.h - GALLERY_H - 110, w, GALLERY_H, &gallery_ops, NULL, NULL);
    if (gallery) {
        gallery->radius = 28;
        gallery->flags |= WIN_PASSIVE;
    }
}

bool widgets_editing(void)
{
    return editing;
}

/* ---- the inspector: everything about one widget */

struct inspector {
    struct ui_state st;
    struct widget *w;
    struct ui_text text, city;
};

static const char *const world_labels[] = {"Los Angeles",  "New York", "Sao Paulo", "London",    "Berlin",
                                           "Moscow",       "Dubai",    "Delhi",     "Novosibirsk", "Beijing",
                                           "Tokyo",        "Sydney"};
static const char *const world_values[] = {"Los Angeles|-480", "New York|-300", "Sao Paulo|-180", "London|0",
                                           "Berlin|60",        "Moscow|180",    "Dubai|240",      "Delhi|330",
                                           "Novosibirsk|420",  "Beijing|480",   "Tokyo|540",      "Sydney|600"};

static void changed(struct widget *w)
{
    if (!fits(w->col, w->row, w->size, w)) {
        int c, r;
        if (nearest_free(w->col, w->row, w->size, w, &c, &r))
            w->col = c, w->row = r;
    }
    widget_free_kind(w);
    save();
    widgets_layout();
    widget_damage(w);
    gui_desktop_changed();
}

static int segmented_row(struct ui *ui, int y, int W, const char *label, const char *const *titles, int n, int sel,
                         unsigned allowed)
{
    ui_label(ui, (struct rect){18, y, 90, 26}, label, &font_sans, style.text, ALIGN_LEFT);
    const char *shown[WS_COUNT + 1];
    int map[WS_COUNT + 1], k = 0, cur = -1;
    for (int i = 0; i < n; i++)
        if (allowed >> i & 1) {
            if (i == sel)
                cur = k;
            map[k] = i;
            shown[k++] = titles[i];
        }
    int before = cur;
    ui_segmented(ui, (struct rect){100, y, W - 118, 26}, shown, k, &cur);
    return cur != before && cur >= 0 ? map[cur] : -1;
}

static void apps_toggle(struct widget *w, const char *name)
{
    char out[WIDGET_OPT] = "";
    bool had = false;
    for (const char *p = w->opt; *p;) {
        const char *e = strchr(p, '|');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == strlen(name) && !strncmp(p, name, n))
            had = true;
        else if (n) {
            if (out[0])
                str_append(out, "|", sizeof(out));
            size_t len = strlen(out);
            if (len + n < sizeof(out)) {
                memcpy(out + len, p, n);
                out[len + n] = 0;
            }
        }
        p += n + (e ? 1 : 0);
    }
    if (!had) {
        if (out[0])
            str_append(out, "|", sizeof(out));
        str_append(out, name, sizeof(out));
    }
    str_copy(w->opt, out, sizeof(w->opt));
}

struct app_rows {
    struct ui *ui;
    struct widget *w;
    int y, W, count;
};

static void app_row(const struct gui_app *app, void *ctx)
{
    struct app_rows *a = ctx;
    if (!app->launch || a->count >= 12 || !strcmp(app->name, "Launchpad"))
        return;
    int col = a->count % 2, row = a->count / 2;
    struct rect r = {18 + col * (a->W - 36) / 2, a->y + row * 26, (a->W - 36) / 2 - 6, 24};
    bool on = strstr(a->w->opt, app->name) != NULL;
    if (ui_checkbox(a->ui, r, app->name, &on)) {
        apps_toggle(a->w, app->name);
        changed(a->w);
    }
    a->count++;
}

static int inspector_height(const struct widget *w)
{
    int extra = 0;
    switch (w->kind) {
    case WK_NOTES:
    case WK_PHOTO:
    case WK_WEATHER: extra = 44; break;
    case WK_WORLD: extra = 44; break;
    case WK_APPS: extra = 26 * 6 + 10; break;
    case WK_TASKS: extra = 40; break;
    default: break;
    }
    return 52 + 3 * 36 + extra + 56;
}

static void city_picked(struct window *owner, int index);
static struct menu_item city_items[ARRAY_SIZE(world_labels)];

static void inspector_frame(struct ui *ui, void *data)
{
    struct inspector *in = data;
    struct widget *w = in->w;
    int W = ui->win->surface.w;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, ui->c->w, ui->c->h}, 0);
    if (!w->used || w->leaving) {
        ui->win = NULL;
        wm_destroy(inspector);
        return;
    }
    if (ui->c) {
        gfx_symbol(ui->c, 18, 16, widget_kinds[w->kind].sym, 16, style.text);
        gfx_text_box(ui->c, (struct rect){42, 12, W - 60, 24}, &font_bold, widget_kinds[w->kind].title, style.text,
                     ALIGN_LEFT);
    }
    int y = 52, v;
    if ((v = segmented_row(ui, y, W, "Size", size_titles, WS_COUNT, w->size, widget_kinds[w->kind].sizes)) >= 0) {
        w->size = v;
        changed(w);
    }
    y += 36;
    if ((v = segmented_row(ui, y, W, "Shape", shape_titles, SH_COUNT, w->shape, (1u << SH_COUNT) - 1)) >= 0) {
        w->shape = v;
        changed(w);
    }
    y += 36;
    if ((v = segmented_row(ui, y, W, "Style", style_titles, ST_COUNT, w->style, (1u << ST_COUNT) - 1)) >= 0) {
        w->style = v;
        changed(w);
    }
    y += 44;
    switch (w->kind) {
    case WK_NOTES:
        ui_label(ui, (struct rect){18, y, 90, 26}, "Note", &font_sans, style.text, ALIGN_LEFT);
        if (ui_textbox_hint(ui, (struct rect){100, y, W - 118, 26}, &in->text, "Write something")) {
            str_copy(w->opt, in->text.buf, sizeof(w->opt));
            changed(w);
        }
        break;
    case WK_PHOTO:
        ui_label(ui, (struct rect){18, y, 90, 26}, "Folder", &font_sans, style.text, ALIGN_LEFT);
        if (ui_textbox_hint(ui, (struct rect){100, y, W - 118, 26}, &in->text, "~/Pictures")) {
            str_copy(w->opt, in->text.buf, sizeof(w->opt));
            w->pic_index = 0;
            changed(w);
        }
        break;
    case WK_WEATHER:
        ui_label(ui, (struct rect){18, y, 90, 26}, "City", &font_sans, style.text, ALIGN_LEFT);
        if (ui_textbox_hint(ui, (struct rect){100, y, W - 118, 26}, &in->city, "By address"))
            gui_setting("weathercity", in->city.len ? in->city.buf : "auto");
        break;
    case WK_WORLD: {
        ui_label(ui, (struct rect){18, y, 90, 26}, "City", &font_sans, style.text, ALIGN_LEFT);
        const char *bar = strchr(w->opt, '|');
        char city[48];
        size_t n = bar ? MIN((size_t)(bar - w->opt), sizeof(city) - 1) : 0;
        memcpy(city, w->opt, n);
        city[n] = 0;
        if (ui_button(ui, (struct rect){100, y, W - 118, 26}, n ? city : "Choose…")) {
            for (size_t i = 0; i < ARRAY_SIZE(world_labels); i++)
                city_items[i] = (struct menu_item){world_labels[i], 0, NULL};
            ui_menu_items(ui->win, 100, y + 28, city_items, (int)ARRAY_SIZE(world_labels), city_picked);
        }
        break;
    }
    case WK_APPS: {
        ui_label(ui, (struct rect){18, y - 6, W - 36, 18}, "Apps in the widget", &font_small, style.secondary,
                 ALIGN_LEFT);
        struct app_rows a = {ui, w, y + 14, W, 0};
        gui_app_foreach(app_row, &a);
        break;
    }
    case WK_TASKS:
        if (ui_button(ui, (struct rect){18, y, W - 36, 26}, "Remove Finished Tasks"))
            widget_click(w, 200, 1);
        break;
    default: break;
    }
    if (!ui->win)
        return;
    int H = ui->win->surface.h;
    if (ui_button(ui, (struct rect){18, H - 44, (W - 48) / 2, 28}, "Remove")) {
        ui->win = NULL;
        wm_destroy(inspector);
        remove_widget(w);
        return;
    }
    if (ui_button_primary(ui, (struct rect){30 + (W - 48) / 2, H - 44, (W - 48) / 2, 28}, "Done")) {
        ui->win = NULL;
        wm_destroy(inspector);
    }
}

static void city_picked(struct window *owner, int index)
{
    if (!owner || !wm_window_alive(owner) || index < 0 || index >= (int)ARRAY_SIZE(world_values))
        return;
    struct inspector *in = owner->data;
    str_copy(in->w->opt, world_values[index], sizeof(in->w->opt));
    changed(in->w);
    wm_invalidate(owner);
}

static void inspector_paint(struct window *w, struct canvas *c)
{
    struct inspector *in = w->data;
    ui_run(w, &in->st, c, NULL, inspector_frame, in);
}

static void inspector_event(struct window *w, struct gui_event *e)
{
    struct inspector *in = w->data;
    if (e->type == EV_KEY && KEY_BASE(e->key) == KEY_ESC && !in->st.focus) {
        wm_destroy(w);
        return;
    }
    ui_run(w, &in->st, NULL, e, inspector_frame, in);
}

static void inspector_destroy(struct window *w)
{
    kfree(w->data);
    if (inspector == w)
        inspector = NULL;
}

static const struct window_ops inspector_ops = {inspector_paint, inspector_event, NULL, inspector_destroy};

static void inspector_open(struct widget *w)
{
    if (inspector && wm_window_alive(inspector))
        wm_destroy(inspector);
    struct inspector *in = kzalloc(sizeof(*in));
    if (!in)
        return;
    in->w = w;
    if (w->kind == WK_NOTES || w->kind == WK_PHOTO)
        ui_text_set(&in->text, w->opt);
    char city[48];
    gui_setting_get("weathercity", city, sizeof(city));
    if (strcmp(city, "auto"))
        ui_text_set(&in->city, city);
    int W = 340, H = inspector_height(w);
    struct rect s = wm_screen_rect(0);
    int x = w->r.x - W - 16 >= s.x + 8 ? w->r.x - W - 16 : w->r.x + w->r.w + 16;
    int y = CLAMP(w->r.y, s.y + menubar_height() + 8, s.y + s.h - H - 8);
    inspector = wm_popup(CLAMP(x, s.x + 8, s.x + s.w - W - 8), y, W, H, &inspector_ops, in, NULL);
    if (!inspector) {
        kfree(in);
        return;
    }
    inspector->radius = 22;
}

/* ---- the command and the settings */

static void kinds_text(char *buf, size_t cap)
{
    ensure_loaded();
    size_t n = 0;
    buf[0] = 0;
    for (int k = 0; k < WK_COUNT; k++)
        for (int i = 0; i < MAX_WIDGETS; i++)
            if (ws[i].used && !ws[i].leaving && ws[i].kind == k) {
                n += (size_t)ksnprintf(buf + n, cap - n, "%s%s", n ? "," : "", widget_kinds[k].name);
                break;
            }
    if (!n)
        str_copy(buf, "none", cap);
}

static bool has_kind(int k)
{
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (ws[i].used && !ws[i].leaving && ws[i].kind == k)
            return true;
    return false;
}

/* Makes the set of kinds on the desktop match the list: kinds that stay keep their place and options. */
static int set_kinds(const char *value)
{
    unsigned want = 0;
    if (!strcmp(value, "all"))
        want = (1u << WK_COUNT) - 1;
    else if (strcmp(value, "none"))
        for (const char *p = value; *p;) {
            const char *e = strchr(p, ',');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            int k = kind_named(p, n);
            if (k < 0)
                return E_INVAL;
            want |= 1u << k;
            p += n + (e ? 1 : 0);
        }
    ensure_loaded();
    for (int i = 0; i < MAX_WIDGETS; i++)
        if (ws[i].used && !ws[i].leaving && !(want >> ws[i].kind & 1)) {
            if (started)
                ws[i].leaving = true, ws[i].intro.target = -400;
            else
                drop_widget(&ws[i]);
        }
    for (int k = 0; k < WK_COUNT; k++)
        if ((want >> k & 1) && !has_kind(k)) {
            struct widget *w = add_widget(k);
            if (w && started)
                place_new(w);
        }
    save();
    if (started) {
        widgets_layout();
        gui_desktop_changed();
    }
    return 0;
}

COMMAND(widgets, NULL, "system", "widgets [list|add <kind>|remove <kind>|edit|done]",
        "add and remove desktop widgets (each look keeps its own)")
{
    if (argc == 1 || !strcmp(argv[1], "list")) {
        ensure_loaded();
        for (int k = 0; k < WK_COUNT; k++) {
            int n = 0;
            for (int i = 0; i < MAX_WIDGETS; i++)
                n += ws[i].used && !ws[i].leaving && ws[i].kind == k;
            sh_printf(sh, "%-11s", widget_kinds[k].name);
            sh_role(sh, n ? ROLE_OK : ROLE_MUTED);
            if (n > 1)
                sh_printf(sh, "%d on the desktop\n", n);
            else
                sh_puts(sh, n ? "on the desktop\n" : "off\n");
            sh_role(sh, ROLE_TEXT);
        }
        return 0;
    }
    if (!strcmp(argv[1], "edit") || !strcmp(argv[1], "done")) {
        if (!started)
            return sh_error(sh, "widgets", "the desktop is not running");
        widgets_edit(!strcmp(argv[1], "edit"));
        return 0;
    }
    bool add = !strcmp(argv[1], "add");
    if (argc != 3 || (!add && strcmp(argv[1], "remove")))
        return command_usage(sh, command_find("widgets"));
    int k = kind_named(argv[2], strlen(argv[2]));
    if (k < 0) {
        sh_error(sh, "widgets", "no widget called %s; there are:", argv[2]);
        for (int i = 0; i < WK_COUNT; i++)
            sh_printf(sh, "%s%s", i ? ", " : "  ", widget_kinds[i].name);
        sh_puts(sh, "\n");
        return 1;
    }
    if (add) {
        add_kind(k);
        return 0;
    }
    ensure_loaded();
    for (int i = MAX_WIDGETS - 1; i >= 0; i--)
        if (ws[i].used && !ws[i].leaving && ws[i].kind == k) {
            if (started)
                remove_widget(&ws[i]);
            else {
                drop_widget(&ws[i]);
                save();
            }
            break;
        }
    return 0;
}

static void get_widgets(char *buf, size_t cap)
{
    kinds_text(buf, cap);
}

static int set_widgets(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return set_kinds(value);
}

static const struct setting s_widgets = {"widgets", "all|none|<kind>,...",
                                         "widgets on the desktop of the current look (see 'widgets list')", get_widgets,
                                         set_widgets};
SETTING(s_widgets);
