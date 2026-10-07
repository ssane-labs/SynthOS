#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "../image.h"
#include "../internal.h"

#define TOOLBAR_H TOOLBAR_HEIGHT
#define TOOLBOX_W 0
#define PALETTE_H 48
#define STATUS_H 0
#define BAR 12
#define MARGIN 8
#define IMG_W 640
#define IMG_H 400
#define WHITE RGB(255, 255, 255)

enum tool {
    T_PENCIL,
    T_BRUSH,
    T_ERASER,
    T_LINE,
    T_RECT,
    T_ELLIPSE,
    T_FILL,
    T_PICK,
    T_COUNT,
};

static const char *const tool_names[T_COUNT] = {"Pencil", "Brush", "Eraser", "Line",
                                                "Rectangle", "Ellipse", "Fill", "Color picker"};

static const uint32_t colors[] = {
    RGB(0, 0, 0),       RGB(64, 64, 64),    RGB(128, 128, 128), RGB(136, 0, 21),   RGB(237, 28, 36),
    RGB(255, 127, 39),  RGB(255, 242, 0),   RGB(34, 177, 76),   RGB(0, 162, 232),  RGB(63, 72, 204),
    RGB(163, 73, 164),  RGB(255, 0, 170),   RGB(0, 255, 213),   RGB(20, 16, 60),   RGB(255, 255, 255),
    RGB(195, 195, 195), RGB(220, 220, 220), RGB(185, 122, 87),  RGB(255, 174, 201), RGB(255, 201, 14),
    RGB(239, 228, 176), RGB(181, 230, 29),  RGB(153, 217, 234), RGB(112, 146, 190), RGB(200, 191, 231),
    RGB(255, 120, 220), RGB(120, 255, 240), RGB(90, 60, 160),
};

struct paint {
    struct ui_state st;
    struct canvas img;
    struct canvas undo;
    struct canvas snap;
    bool has_undo;
    int tool, prev_tool;
    uint32_t fg, bg, stroke;
    int size;
    bool filled;
    bool drawing;
    int sx, sy, lx, ly;
    int view_x, view_y;
    int cx, cy;
    char path[PATH_MAX_LEN];
    bool modified;
};

static int isqrt(int v)
{
    if (v <= 0)
        return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

static void ellipse_rows(struct canvas *c, int x0, int y0, int x1, int y1, int thick, bool fill, uint32_t color)
{
    if (x1 < x0) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    int a2 = x1 - x0, b2 = y1 - y0;
    int cx2 = x0 + x1, cy2 = y0 + y1;
    int ia2 = a2 - 2 * thick, ib2 = b2 - 2 * thick;
    for (int y = y0; y <= y1; y++) {
        int dy = 2 * y - cy2;
        if (b2 == 0) {
            gfx_fill(c, (struct rect){x0, y, a2 + 1, 1}, color);
            continue;
        }
        int outer = (int)((int64_t)a2 * isqrt(b2 * b2 - dy * dy) / b2);
        int left = (cx2 - outer + 1) / 2, right = (cx2 + outer) / 2;
        int inner = -1;
        if (!fill && ia2 > 0 && ib2 > 0 && dy * dy < ib2 * ib2)
            inner = (int)((int64_t)ia2 * isqrt(ib2 * ib2 - dy * dy) / ib2);
        if (inner < 0) {
            gfx_fill(c, (struct rect){left, y, right - left + 1, 1}, color);
        } else {
            int il = (cx2 - inner + 1) / 2, ir = (cx2 + inner) / 2;
            gfx_fill(c, (struct rect){left, y, MAX(1, il - left), 1}, color);
            gfx_fill(c, (struct rect){ir + 1, y, MAX(1, right - ir), 1}, color);
        }
    }
}

static void rect_shape(struct canvas *c, int x0, int y0, int x1, int y1, int t, bool fill, uint32_t color)
{
    struct rect r = {MIN(x0, x1), MIN(y0, y1), (x0 < x1 ? x1 - x0 : x0 - x1) + 1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1};
    if (fill) {
        gfx_fill(c, r, color);
        return;
    }
    gfx_fill(c, (struct rect){r.x, r.y, r.w, t}, color);
    gfx_fill(c, (struct rect){r.x, r.y + r.h - t, r.w, t}, color);
    gfx_fill(c, (struct rect){r.x, r.y, t, r.h}, color);
    gfx_fill(c, (struct rect){r.x + r.w - t, r.y, t, r.h}, color);
}

static void dab(struct canvas *c, int x, int y, int size, bool square, uint32_t color)
{
    if (square)
        gfx_fill(c, (struct rect){x - size / 2, y - size / 2, size, size}, color);
    else
        gfx_circle(c, x, y, size / 2, color);
}

static void stroke(struct paint *p, int x0, int y0, int x1, int y1)
{
    if (p->tool == T_PENCIL) {
        gfx_line(&p->img, x0, y0, x1, y1, p->stroke);
        return;
    }
    bool eraser = p->tool == T_ERASER;
    int size = eraser ? MAX(4, p->size * 2) : p->size;
    int dx = x1 - x0, dy = y1 - y0;
    int steps = MAX(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
    for (int i = 0; i <= steps; i++)
        dab(&p->img, steps ? x0 + dx * i / steps : x0, steps ? y0 + dy * i / steps : y0, size, eraser, p->stroke);
}

static void shape(struct paint *p, int x, int y)
{
    memcpy(p->img.px, p->snap.px, (size_t)p->img.w * (size_t)p->img.h * 4);
    if (p->tool == T_LINE)
        gfx_thick_line(&p->img, p->sx, p->sy, x, y, p->size, p->stroke);
    else if (p->tool == T_RECT)
        rect_shape(&p->img, p->sx, p->sy, x, y, p->size, p->filled, p->stroke);
    else
        ellipse_rows(&p->img, p->sx, p->sy, x, y, p->size, p->filled, p->stroke);
}

static void flood(struct canvas *c, int x, int y, uint32_t color)
{
    uint32_t target = c->px[y * c->stride + x];
    if (target == color)
        return;
    size_t cap = (size_t)c->w * (size_t)c->h;
    uint32_t *stack = kmalloc(cap * sizeof(uint32_t));
    if (!stack)
        return;
    size_t top = 0;
    stack[top++] = (uint32_t)(y * c->w + x);
    while (top) {
        uint32_t v = stack[--top];
        int px = (int)(v % (uint32_t)c->w), py = (int)(v / (uint32_t)c->w);
        uint32_t *row = c->px + py * c->stride;
        if (row[px] != target)
            continue;
        int l = px, r = px;
        while (l > 0 && row[l - 1] == target)
            l--;
        while (r < c->w - 1 && row[r + 1] == target)
            r++;
        for (int i = l; i <= r; i++)
            row[i] = color;
        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = py + dir;
            if (ny < 0 || ny >= c->h)
                continue;
            uint32_t *nrow = c->px + ny * c->stride;
            bool open = false;
            for (int i = l; i <= r; i++) {
                bool match = nrow[i] == target;
                if (match && !open && top < cap)
                    stack[top++] = (uint32_t)(ny * c->w + i);
                open = match;
            }
        }
    }
    kfree(stack);
}

static bool same_size(struct canvas *a, const struct canvas *b)
{
    if (a->px && a->w == b->w && a->h == b->h)
        return true;
    canvas_free(a);
    return canvas_alloc(a, b->w, b->h);
}

static void save_undo(struct paint *p)
{
    if (!same_size(&p->undo, &p->img))
        return;
    memcpy(p->undo.px, p->img.px, (size_t)p->img.w * (size_t)p->img.h * 4);
    p->has_undo = true;
}

static void undo(struct paint *p)
{
    if (!p->has_undo)
        return;
    struct canvas t = p->img;
    p->img = p->undo;
    p->undo = t;
    p->modified = true;
}

static void update_title(struct window *w, struct paint *p)
{
    char title[64];
    const char *name = p->path[0] ? path_basename(p->path) : "Untitled";
    str_copy(title, name, sizeof(title));
    wm_set_title(w, title);
}

static void new_image(struct paint *p, int w, int h)
{
    struct canvas c;
    if (!canvas_alloc(&c, w, h))
        return;
    gfx_fill(&c, (struct rect){0, 0, w, h}, WHITE);
    canvas_free(&p->img);
    p->img = c;
    p->has_undo = false;
    p->modified = false;
    p->view_x = p->view_y = 0;
    p->path[0] = 0;
}

static bool load(struct window *w, struct paint *p, const char *path)
{
    struct image im;
    if (!image_load(path, &im)) {
        gui_message("The picture could not be opened", "Only BMP images with 24 or 32 bits per pixel are supported.");
        return false;
    }
    canvas_free(&p->img);
    p->img = canvas_make(im.px, im.w, im.h);
    for (int i = 0; i < im.w * im.h; i++)
        im.px[i] |= 0xff000000u;
    p->has_undo = false;
    p->modified = false;
    p->view_x = p->view_y = 0;
    str_copy(p->path, path, sizeof(p->path));
    update_title(w, p);
    return true;
}

static void save(struct window *w, struct paint *p)
{
    if (image_save(p->path, p->img.px, p->img.w, p->img.h, p->img.stride) != 0) {
        gui_message("The picture could not be saved", "Check the file name and that the disk is not full.");
        return;
    }
    p->modified = false;
    update_title(w, p);
}

static void on_open(struct window *w, const char *value)
{
    if (wm_window_alive(w) && value && *value)
        load(w, w->data, value);
}

static void on_save_as(struct window *w, const char *value)
{
    if (!wm_window_alive(w) || !value || !*value)
        return;
    struct paint *p = w->data;
    str_copy(p->path, value, sizeof(p->path));
    if (!path_ext(p->path))
        str_append(p->path, ".bmp", sizeof(p->path));
    save(w, p);
}

static void save_or_prompt(struct window *w, struct paint *p)
{
    if (p->path[0])
        save(w, p);
    else
        ui_prompt(w, "Save picture", "Save as BMP file:", "/home/picture.bmp", on_save_as);
}

static void hscroll(struct ui *ui, struct rect r, int *pos, int content, int view)
{
    int max = MAX(0, content - view);
    *pos = CLAMP(*pos, 0, max);
    int id = ui_next_id(ui);
    int thumb = content > 0 ? MAX(24, r.w * view / MAX(content, 1)) : r.w;
    thumb = MIN(thumb, r.w);
    int travel = r.w - thumb;
    if (ui->e) {
        bool in = ui_hover(ui, r);
        if (in)
            ui->hot = id;
        if (ui->e->type == EV_MOUSE_DOWN && in && ui->e->button == BUTTON_LEFT) {
            ui->st->active = id;
            ui->consumed = true;
        }
        if (ui->st->active == id && max > 0 && (ui->e->type == EV_MOUSE_DOWN || ui->e->type == EV_MOUSE_MOVE)) {
            int v = travel > 0 ? CLAMP(ui->e->x - r.x - thumb / 2, 0, travel) * max / travel : 0;
            if (v != *pos) {
                *pos = v;
                ui->changed = true;
            }
        }
    }
    if (ui->c && max > 0) {
        int tx = r.x + (travel > 0 ? *pos * travel / max : 0);
        gfx_round(ui->c, (struct rect){tx + 2, r.y + 3, thumb - 4, r.h - 6}, 3,
                  ui->st->active == id || ui->st->hot == id ? style.button_hover : style.button);
    }
}

static void begin(struct paint *p, int x, int y, unsigned button)
{
    p->stroke = button == BUTTON_RIGHT ? p->bg : p->fg;
    if (p->tool == T_PICK) {
        uint32_t c = p->img.px[y * p->img.stride + x] | 0xff000000u;
        if (button == BUTTON_RIGHT)
            p->bg = c;
        else
            p->fg = c;
        p->tool = p->prev_tool;
        return;
    }
    save_undo(p);
    p->modified = true;
    if (p->tool == T_FILL) {
        flood(&p->img, x, y, p->stroke);
        return;
    }
    if (p->tool == T_ERASER)
        p->stroke = button == BUTTON_RIGHT ? p->fg : p->bg;
    p->drawing = true;
    p->sx = p->lx = x;
    p->sy = p->ly = y;
    if (p->tool >= T_LINE) {
        if (!same_size(&p->snap, &p->img)) {
            p->drawing = false;
            return;
        }
        memcpy(p->snap.px, p->img.px, (size_t)p->img.w * (size_t)p->img.h * 4);
        shape(p, x, y);
    } else {
        stroke(p, x, y, x, y);
    }
}

static void canvas_input(struct ui *ui, struct paint *p, struct rect view, int ox, int oy)
{
    struct gui_event *e = ui->e;
    if (!e)
        return;
    int x = e->x - ox, y = e->y - oy;
    bool over = rect_contains(view, e->x, e->y) && x >= 0 && y >= 0 && x < p->img.w && y < p->img.h;
    if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN || e->type == EV_LEAVE) {
        int ncx = over && e->type != EV_LEAVE ? x : -1, ncy = over && e->type != EV_LEAVE ? y : -1;
        if (ncx != p->cx || ncy != p->cy) {
            p->cx = ncx;
            p->cy = ncy;
            ui->changed = true;
        }
    }
    if (e->type == EV_WHEEL && rect_contains(view, e->x, e->y)) {
        p->view_y += e->wheel * 40;
        ui->changed = true;
    } else if (e->type == EV_MOUSE_DOWN && over && (e->button == BUTTON_LEFT || e->button == BUTTON_RIGHT) &&
               !p->drawing) {
        ui->consumed = true;
        begin(p, x, y, e->button);
    } else if (e->type == EV_MOUSE_MOVE && p->drawing) {
        x = CLAMP(x, -1, p->img.w);
        y = CLAMP(y, -1, p->img.h);
        if (p->tool >= T_LINE)
            shape(p, x, y);
        else
            stroke(p, p->lx, p->ly, x, y);
        p->lx = x;
        p->ly = y;
        ui->changed = true;
    } else if (e->type == EV_MOUSE_UP && p->drawing) {
        p->drawing = false;
    }
}

enum {
    CMD_NEW,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_UNDO,
    CMD_TOOL,
};

static const struct menu_item file_items[] = {
    {"New", KEY_COMMAND(CMD_NEW), "⌘N"},
    {"Open…", KEY_COMMAND(CMD_OPEN), "⌘O"},
    {"", 0, NULL},
    {"Save", KEY_COMMAND(CMD_SAVE), "⌘S"},
    {"Save As…", KEY_COMMAND(CMD_SAVE_AS), "⇧⌘S"},
};

static const struct menu_item edit_items[] = {
    {"Undo", KEY_COMMAND(CMD_UNDO), "⌘Z"},
};

static const struct menu_item tool_items[] = {
    {"Pencil", KEY_COMMAND(CMD_TOOL + T_PENCIL), "P"},     {"Brush", KEY_COMMAND(CMD_TOOL + T_BRUSH), "B"},
    {"Eraser", KEY_COMMAND(CMD_TOOL + T_ERASER), "E"},     {"Line", KEY_COMMAND(CMD_TOOL + T_LINE), "L"},
    {"Rectangle", KEY_COMMAND(CMD_TOOL + T_RECT), "R"},    {"Ellipse", KEY_COMMAND(CMD_TOOL + T_ELLIPSE), "O"},
    {"Fill", KEY_COMMAND(CMD_TOOL + T_FILL), "F"},         {"Color Picker", KEY_COMMAND(CMD_TOOL + T_PICK), "I"},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"Edit", edit_items, ARRAY_SIZE(edit_items)},
    {"Tools", tool_items, ARRAY_SIZE(tool_items)},
};

static const enum symbol_id tool_symbols[T_COUNT] = {SYM_PENCIL, SYM_BRUSH, SYM_ERASER, SYM_LINE,
                                                     SYM_RECT,   SYM_ELLIPSE, SYM_BUCKET, SYM_DROPPER};

static void select_tool(struct paint *p, int tool)
{
    if (tool == T_PICK && p->tool != T_PICK)
        p->prev_tool = p->tool;
    p->tool = tool;
}

static void toolbar(struct ui *ui, struct paint *p, int W)
{
    ui_panel(ui, (struct rect){0, 0, W, TOOLBAR_H}, style.window);
    ui_separator(ui, 0, TOOLBAR_H - 1, W);
    char title[64];
    str_copy(title, p->path[0] ? path_basename(p->path) : "Untitled", sizeof(title));
    if (p->modified)
        str_append(title, " — Edited", sizeof(title));
    ui_label(ui, (struct rect){84, 8, 200, 20}, title, &font_bold, style.text, ALIGN_LEFT);
    char sub[32];
    ksnprintf(sub, sizeof(sub), "%d × %d", p->img.w, p->img.h);
    ui_label(ui, (struct rect){84, 26, 200, 16}, sub, &font_small, style.secondary, ALIGN_LEFT);
    int group_w = T_COUNT * 32 + 8, x = MAX(250, (W - group_w) / 2 - 40);
    if (ui->c)
        gfx_round(ui->c, (struct rect){x, 10, group_w, 32}, 16, style.dark ? ALPHA(0xffffffffu, 20) : ALPHA(0, 12));
    for (int i = 0; i < T_COUNT; i++)
        if (ui_symbol_button(ui, (struct rect){x + 4 + i * 32, 12, 30, 28}, tool_symbols[i], p->tool == i))
            select_tool(p, i);
    x += group_w + 16;
    ui_slider(ui, (struct rect){x, 16, 90, 20}, &p->size, 1, 32);
    x += 104;
    ui_label(ui, (struct rect){x, 0, 40, TOOLBAR_H}, "Fill", &font_sans, style.text, ALIGN_LEFT);
    ui_switch(ui, (struct rect){x + 30, 16, 32, 20}, &p->filled);
    if (ui_symbol_button(ui, (struct rect){W - 44, 12, 30, 28}, SYM_UNDO, false))
        undo(p);
}

static void wells(struct ui *ui, struct paint *p, int y)
{
    int x = 14;
    if (ui->c) {
        struct rect back = {x + 14, y + 14, 24, 24}, front = {x, y + 6, 24, 24};
        gfx_round(ui->c, back, 6, p->bg);
        gfx_round_frame(ui->c, back, 6, style.separator);
        gfx_drop_shadow(ui->c, front, 6, 4, 60, 1);
        gfx_round(ui->c, front, 6, p->fg);
        gfx_round_frame(ui->c, front, 6, ALPHA(0, 60));
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, (struct rect){x, y, 40, 40})) {
        uint32_t t = p->fg;
        p->fg = p->bg;
        p->bg = t;
        ui->consumed = true;
    }
}

static void palette(struct ui *ui, struct paint *p, int y, int W)
{
    ui_panel(ui, (struct rect){0, y, W, PALETTE_H}, style.window);
    ui_separator(ui, 0, y, W);
    wells(ui, p, y);
    int per_row = (int)ARRAY_SIZE(colors) / 2;
    for (int i = 0; i < (int)ARRAY_SIZE(colors); i++) {
        struct rect r = {64 + (i % per_row) * 22, y + 4 + (i / per_row) * 20, 18, 18};
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, r)) {
            if (ui->e->button == BUTTON_RIGHT)
                p->bg = colors[i];
            else
                p->fg = colors[i];
            ui->consumed = true;
        }
        if (ui->c) {
            gfx_circle(ui->c, r.x + 9, r.y + 9, 8, colors[i]);
            gfx_ring(ui->c, r.x + 9, r.y + 9, 8, ALPHA(0, 50));
        }
    }
    char pos[32];
    if (p->cx >= 0)
        ksnprintf(pos, sizeof(pos), "%s   ·   %d, %d", tool_names[p->tool], p->cx, p->cy);
    else
        str_copy(pos, tool_names[p->tool], sizeof(pos));
    ui_label(ui, (struct rect){W - 220, y, 206, PALETTE_H}, pos, &font_small, style.secondary, ALIGN_RIGHT);
}

static void keys(struct ui *ui, struct paint *p)
{
    int k = ui_key(ui);
    static const char tool_keys[T_COUNT] = {'p', 'b', 'e', 'l', 'r', 'o', 'f', 'i'};
    if (k >= KEY_COMMAND(CMD_TOOL) && k < KEY_COMMAND(CMD_TOOL + T_COUNT)) {
        select_tool(p, k - KEY_COMMAND(CMD_TOOL));
        return;
    }
    for (int i = 0; i < T_COUNT; i++)
        if (k == tool_keys[i]) {
            select_tool(p, i);
            return;
        }
    if (k == KEY_CTRL('z') || k == KEY_COMMAND(CMD_UNDO))
        undo(p);
    else if (k == KEY_CTRL('s') || k == KEY_COMMAND(CMD_SAVE))
        save_or_prompt(ui->win, p);
    else if (k == (KEY_CTRL('s') | KEY_MOD_SHIFT) || k == KEY_COMMAND(CMD_SAVE_AS))
        ui_prompt(ui->win, "Save As", "Save the picture as a BMP file:",
                  p->path[0] ? p->path : "/home/Pictures/Untitled.bmp", on_save_as);
    else if (k == KEY_CTRL('o') || k == KEY_COMMAND(CMD_OPEN))
        ui_prompt(ui->win, "Open", "Enter the path of a BMP image:", p->path[0] ? p->path : "/home/Pictures/", on_open);
    else if (k == KEY_CTRL('n') || k == KEY_COMMAND(CMD_NEW))
        gui_app_launch(ui->win->app, NULL);
    else if (k == '[')
        p->size = MAX(1, p->size - 1);
    else if (k == ']')
        p->size = MIN(32, p->size + 1);
}

static void frame(struct ui *ui, void *data)
{
    struct paint *p = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    int bottom = H - PALETTE_H;
    struct rect view = {0, TOOLBAR_H, W - BAR, bottom - TOOLBAR_H - BAR};
    int content_w = p->img.w + 2 * MARGIN, content_h = p->img.h + 2 * MARGIN;
    p->view_x = CLAMP(p->view_x, 0, MAX(0, content_w - view.w));
    p->view_y = CLAMP(p->view_y, 0, MAX(0, content_h - view.h));
    int ox = view.x + MAX(MARGIN, (view.w - p->img.w) / 2) - p->view_x;
    int oy = view.y + MAX(MARGIN, (view.h - p->img.h) / 2) - p->view_y;
    canvas_input(ui, p, view, ox, oy);
    if (ui->c) {
        struct canvas vc = *ui->c;
        vc.clip = rect_intersect(ui->c->clip, (struct rect){0, view.y, W, view.h + BAR});
        gfx_clear(&vc, vc.clip, style.dark ? RGB(30, 30, 32) : RGB(222, 222, 226));
        vc.clip = rect_intersect(ui->c->clip, view);
        gfx_drop_shadow(&vc, (struct rect){ox, oy, p->img.w, p->img.h}, 0, 10, 70, 3);
        gfx_blit(&vc, ox, oy, &p->img, (struct rect){0, 0, p->img.w, p->img.h});
    }
    ui_scrollbar(ui, (struct rect){view.x + view.w, view.y, BAR, view.h}, &p->view_y, content_h, view.h);
    hscroll(ui, (struct rect){view.x, view.y + view.h, view.w, BAR}, &p->view_x, content_w, view.w);
    toolbar(ui, p, W);
    palette(ui, p, bottom, W);
    keys(ui, p);
}

static void paint(struct window *w, struct canvas *c)
{
    struct paint *p = w->data;
    ui_run(w, &p->st, c, NULL, frame, p);
}

static void event(struct window *w, struct gui_event *e)
{
    struct paint *p = w->data;
    ui_run(w, &p->st, NULL, e, frame, p);
}

static void discard(struct window *w, const char *value)
{
    UNUSED(value);
    struct paint *p = w->data;
    p->modified = false;
    wm_close(w);
}

static bool on_close(struct window *w)
{
    struct paint *p = w->data;
    if (!p->modified)
        return true;
    ui_confirm(w, "Do you want to discard the changes?", "Your changes to this picture will be lost.", discard);
    return false;
}

static void destroy(struct window *w)
{
    struct paint *p = w->data;
    canvas_free(&p->img);
    canvas_free(&p->undo);
    canvas_free(&p->snap);
    kfree(p);
}

static const struct window_ops ops = {paint, event, on_close, destroy};

static void launch(const char *arg)
{
    struct paint *p = kzalloc(sizeof(*p));
    if (!p)
        return;
    new_image(p, IMG_W, IMG_H);
    if (!p->img.px) {
        kfree(p);
        return;
    }
    p->fg = colors[0];
    p->bg = WHITE;
    p->size = 4;
    p->cx = p->cy = -1;
    int sw, sh;
    wm_screen_size(&sw, &sh);
    int cw = MIN(MAX(820, IMG_W + 2 * MARGIN + BAR), sw - 40);
    int ch = MIN(TOOLBAR_H + IMG_H + 2 * MARGIN + BAR + PALETTE_H, sh - MENUBAR_HEIGHT - dock_reserved() - 16);
    struct window *w = wm_open("Paint", ICON_PAINT, cw, ch, &ops, p, WIN_RESIZABLE | WIN_FULLSIZE | WIN_NOTITLE);
    if (!w) {
        canvas_free(&p->img);
        kfree(p);
        return;
    }
    w->toolbar_h = TOOLBAR_H;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    w->min_w = 640;
    w->min_h = 360;
    if (arg && *arg)
        load(w, p, arg);
}

static const struct gui_app paint_app = {"Paint", "Draw pictures and edit BMP images", ICON_PAINT, launch, ".bmp"};
GUI_APP(paint_app);
