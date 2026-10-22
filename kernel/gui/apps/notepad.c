#include <synth/codepage.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

#define TOOLBAR_H 0
#define STATUS_H 0
#define GUTTER_W 46
#define LINE_H 18
#define TEXT_PAD 18

enum {
    CMD_NEW,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_NUMBERS,
    CMD_SELECT_ALL,
};

static const struct menu_item file_items[] = {
    {"New", KEY_COMMAND(CMD_NEW), "⌘N"},
    {"Open…", KEY_COMMAND(CMD_OPEN), "⌘O"},
    {"", 0, NULL},
    {"Save", KEY_COMMAND(CMD_SAVE), "⌘S"},
    {"Save As…", KEY_COMMAND(CMD_SAVE_AS), "⇧⌘S"},
};

static const struct menu_item edit_items[] = {
    {"Cut", KEY_CTRL('x'), "⌘X"},
    {"Copy", KEY_CTRL('c'), "⌘C"},
    {"Paste", KEY_CTRL('v'), "⌘V"},
    {"", 0, NULL},
    {"Select All", KEY_COMMAND(CMD_SELECT_ALL), "⌘A"},
};

static const struct menu_item view_items[] = {
    {"Line Numbers", KEY_COMMAND(CMD_NUMBERS), NULL},
};

static const struct app_menu menus[] = {
    {"File", file_items, ARRAY_SIZE(file_items)},
    {"Edit", edit_items, ARRAY_SIZE(edit_items)},
    {"View", view_items, ARRAY_SIZE(view_items)},
};

struct notepad {
    struct ui_state st;
    char *text;
    size_t len, cap;
    size_t cursor, anchor;
    int scroll_y, scroll_x;
    int goal_x;
    bool selecting;
    bool modified;
    char path[PATH_MAX_LEN];
    size_t *lines;
    int line_count, line_cap;
    bool numbers;
};

static void index_lines(struct notepad *n)
{
    n->line_count = 0;
    for (size_t i = 0; i <= n->len; i++) {
        if (i == 0 || n->text[i - 1] == '\n') {
            if (n->line_count == n->line_cap) {
                int cap = n->line_cap ? n->line_cap * 2 : 256;
                size_t *grown = krealloc(n->lines, sizeof(size_t) * (size_t)cap);
                if (!grown)
                    return;
                n->lines = grown;
                n->line_cap = cap;
            }
            n->lines[n->line_count++] = i;
        }
    }
}

static int line_of(struct notepad *n, size_t pos)
{
    int lo = 0, hi = n->line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (n->lines[mid] <= pos)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static size_t line_end(struct notepad *n, int line)
{
    return line + 1 < n->line_count ? n->lines[line + 1] - 1 : n->len;
}

static bool reserve(struct notepad *n, size_t extra)
{
    if (n->len + extra + 1 <= n->cap)
        return true;
    size_t cap = MAX(n->cap * 2, n->len + extra + 256);
    char *grown = krealloc(n->text, cap);
    if (!grown)
        return false;
    n->text = grown;
    n->cap = cap;
    return true;
}

static void title(struct window *w, struct notepad *n)
{
    char t[64];
    ksnprintf(t, sizeof(t), "%s%s", n->path[0] ? path_basename(n->path) : "Untitled", n->modified ? " — Edited" : "");
    wm_set_title(w, t);
}

static bool has_selection(struct notepad *n)
{
    return n->cursor != n->anchor;
}

static void selection(struct notepad *n, size_t *a, size_t *b)
{
    *a = MIN(n->cursor, n->anchor);
    *b = MAX(n->cursor, n->anchor);
}

static void erase(struct notepad *n, size_t a, size_t b)
{
    memmove(n->text + a, n->text + b, n->len - b + 1);
    n->len -= b - a;
    n->cursor = n->anchor = a;
    n->modified = true;
}

static void delete_selection(struct notepad *n)
{
    size_t a, b;
    selection(n, &a, &b);
    if (a != b)
        erase(n, a, b);
}

static void insert(struct notepad *n, const char *s, size_t len)
{
    delete_selection(n);
    if (!reserve(n, len))
        return;
    memmove(n->text + n->cursor + len, n->text + n->cursor, n->len - n->cursor + 1);
    memcpy(n->text + n->cursor, s, len);
    n->len += len;
    n->cursor += len;
    n->anchor = n->cursor;
    n->modified = true;
}

static size_t prev_char(struct notepad *n, size_t pos)
{
    if (!pos)
        return 0;
    pos--;
    while (pos && ((uint8_t)n->text[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
}

static size_t next_char(struct notepad *n, size_t pos)
{
    if (pos >= n->len)
        return n->len;
    pos++;
    while (pos < n->len && ((uint8_t)n->text[pos] & 0xc0) == 0x80)
        pos++;
    return pos;
}

static int x_of(struct notepad *n, size_t pos)
{
    int line = line_of(n, pos);
    return gfx_text_width_n(&font_mono, n->text + n->lines[line], pos - n->lines[line]);
}

static size_t pos_at(struct notepad *n, int line, int x)
{
    line = CLAMP(line, 0, n->line_count - 1);
    size_t p = n->lines[line], end = line_end(n, line);
    while (p < end) {
        size_t next = next_char(n, p);
        int w0 = gfx_text_width_n(&font_mono, n->text + n->lines[line], p - n->lines[line]);
        int w1 = gfx_text_width_n(&font_mono, n->text + n->lines[line], next - n->lines[line]);
        if (x < (w0 + w1) / 2)
            break;
        p = next;
    }
    return p;
}

static void load_text(struct notepad *n, const char *data, size_t size)
{
    n->len = 0;
    if (reserve(n, size)) {
        size_t out = 0;
        for (size_t i = 0; i < size; i++)
            if (data[i] != '\r')
                n->text[out++] = data[i];
        n->len = out;
        n->text[out] = 0;
    }
    n->cursor = n->anchor = 0;
    n->scroll_y = n->scroll_x = 0;
    n->modified = false;
    index_lines(n);
}

static bool open_path(struct window *w, struct notepad *n, const char *path)
{
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err && err != E_NOENT) {
        gui_message("The document could not be opened", "The file can't be read.");
        return false;
    }
    load_text(n, err ? "" : data, err ? 0 : size);
    if (!err)
        kfree(data);
    str_copy(n->path, path, sizeof(n->path));
    title(w, n);
    return true;
}

static void save_to(struct window *w, struct notepad *n, const char *path)
{
    int err = vfs_write_all(path, n->text, n->len);
    if (err) {
        char msg[128];
        ksnprintf(msg, sizeof(msg), "The document could not be saved: %s.", err_str(err));
        gui_message("Save failed", msg);
        return;
    }
    str_copy(n->path, path, sizeof(n->path));
    n->modified = false;
    title(w, n);
}

static void picked_save(void *ctx, const char *path)
{
    struct window *w = ctx;
    if (path && wm_window_alive(w))
        save_to(w, w->data, path);
}

static void picked_open(void *ctx, const char *path)
{
    struct window *w = ctx;
    if (!path || !wm_window_alive(w))
        return;
    open_path(w, w->data, path);
    wm_invalidate(w);
}

static void ask(struct window *w, struct notepad *n, bool saving)
{
    char dir[PATH_MAX_LEN];
    if (n->path[0])
        path_dirname(n->path, dir, sizeof(dir));
    else
        path_join(user_home(), "Documents", dir, sizeof(dir));
    struct gui_picker o = {.save = saving, .dir = dir, .name = n->path[0] ? path_basename(n->path) : "Untitled.txt"};
    gui_file_picker(w, &o, saving ? picked_save : picked_open, w);
}

static void save(struct window *w, struct notepad *n)
{
    if (n->path[0])
        save_to(w, n, n->path);
    else
        ask(w, n, true);
}

static void copy_selection(struct notepad *n)
{
    size_t a, b;
    selection(n, &a, &b);
    if (a != b)
        gui_set_clipboard(n->text + a, b - a);
}

static void ensure_visible(struct window *w, struct notepad *n)
{
    int view_h = w->surface.h - TOOLBAR_H - STATUS_H - 2 * TEXT_PAD;
    int view_w = w->surface.w - (n->numbers ? GUTTER_W : 0) - 2 * TEXT_PAD - 14;
    int line = line_of(n, n->cursor);
    if (line * LINE_H < n->scroll_y)
        n->scroll_y = line * LINE_H;
    if ((line + 1) * LINE_H > n->scroll_y + view_h)
        n->scroll_y = (line + 1) * LINE_H - view_h;
    int x = x_of(n, n->cursor);
    if (x < n->scroll_x)
        n->scroll_x = MAX(0, x - 40);
    if (x > n->scroll_x + view_w)
        n->scroll_x = x - view_w + 40;
}

static void key(struct window *w, struct notepad *n, int k)
{
    int base = KEY_BASE(k);
    bool shift = k & KEY_MOD_SHIFT;
    bool ctrl = k & KEY_MOD_CTRL;
    int line = line_of(n, n->cursor);
    int page = MAX(1, (w->surface.h - TOOLBAR_H - STATUS_H) / LINE_H - 1);
    size_t target = n->cursor;
    bool move = true, vertical = false;
    switch (base) {
    case KEY_LEFT:
        target = has_selection(n) && !shift ? MIN(n->cursor, n->anchor) : prev_char(n, n->cursor);
        break;
    case KEY_RIGHT:
        target = has_selection(n) && !shift ? MAX(n->cursor, n->anchor) : next_char(n, n->cursor);
        break;
    case KEY_UP:
        target = line > 0 ? pos_at(n, line - 1, n->goal_x) : 0;
        vertical = true;
        break;
    case KEY_DOWN:
        target = line + 1 < n->line_count ? pos_at(n, line + 1, n->goal_x) : n->len;
        vertical = true;
        break;
    case KEY_PGUP:
        target = pos_at(n, line - page, n->goal_x);
        vertical = true;
        break;
    case KEY_PGDN:
        target = pos_at(n, line + page, n->goal_x);
        vertical = true;
        break;
    case KEY_HOME:
        target = ctrl ? 0 : n->lines[line];
        break;
    case KEY_END:
        target = ctrl ? n->len : line_end(n, line);
        break;
    default:
        move = false;
    }
    if (move) {
        n->cursor = target;
        if (!shift)
            n->anchor = target;
        if (!vertical)
            n->goal_x = x_of(n, n->cursor);
        ensure_visible(w, n);
        return;
    }
    if (k == KEY_CTRL('a') || k == KEY_COMMAND(CMD_SELECT_ALL)) {
        n->anchor = 0;
        n->cursor = n->len;
    } else if (k == KEY_CTRL('c')) {
        copy_selection(n);
    } else if (k == KEY_CTRL('x')) {
        copy_selection(n);
        delete_selection(n);
    } else if (k == KEY_CTRL('v')) {
        const char *clip = gui_clipboard();
        insert(n, clip, strlen(clip));
    } else if (k == (KEY_CTRL('s') | KEY_MOD_SHIFT) || k == KEY_COMMAND(CMD_SAVE_AS)) {
        ask(w, n, true);
        return;
    } else if (k == KEY_CTRL('s') || k == KEY_COMMAND(CMD_SAVE)) {
        save(w, n);
    } else if (k == KEY_CTRL('o') || k == KEY_COMMAND(CMD_OPEN)) {
        ask(w, n, false);
        return;
    } else if (k == KEY_CTRL('n') || k == KEY_COMMAND(CMD_NEW)) {
        gui_app_launch(w->app, NULL);
        return;
    } else if (k == KEY_COMMAND(CMD_NUMBERS)) {
        n->numbers = !n->numbers;
        return;
    } else if (k == KEY_BACKSPACE) {
        if (has_selection(n))
            delete_selection(n);
        else if (n->cursor)
            erase(n, prev_char(n, n->cursor), n->cursor);
    } else if (base == KEY_DELETE) {
        if (has_selection(n))
            delete_selection(n);
        else if (n->cursor < n->len)
            erase(n, n->cursor, next_char(n, n->cursor));
    } else if (base == KEY_ENTER || k == '\r') {
        size_t start = n->lines[line], indent = 0;
        while (start + indent < n->cursor && (n->text[start + indent] == ' ' || n->text[start + indent] == '\t'))
            indent++;
        char buf[64] = "\n";
        indent = MIN(indent, sizeof(buf) - 2);
        memcpy(buf + 1, n->text + start, indent);
        insert(n, buf, indent + 1);
    } else if (base == KEY_TAB) {
        insert(n, "    ", 4);
    } else if (KEY_IS_TEXT(k) || (k >= 0x20 && k < 0x100 && k != 0x7f)) {
        char enc[4];
        insert(n, enc, utf8_encode(KEY_IS_TEXT(k) ? (uint32_t)KEY_TEXT_CP(k) : (uint32_t)k, enc));
    } else {
        return;
    }
    index_lines(n);
    n->goal_x = x_of(n, n->cursor);
    ensure_visible(w, n);
    title(w, n);
}

static void pick(struct window *w, int index)
{
    static const int keys[] = {KEY_CTRL('x'), KEY_CTRL('c'), KEY_CTRL('v'), 0, KEY_CTRL('a')};
    if (index >= 0 && index < 5 && keys[index]) {
        key(w, w->data, keys[index]);
        wm_invalidate(w);
    }
}

static void frame(struct ui *ui, void *data)
{
    struct notepad *n = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    int gutter = n->numbers ? GUTTER_W : 0;
    struct rect area = {0, TOOLBAR_H, W, H - TOOLBAR_H - STATUS_H};
    struct rect text = {gutter + TEXT_PAD, TOOLBAR_H + TEXT_PAD, area.w - gutter - 2 * TEXT_PAD - 10,
                        area.h - 2 * TEXT_PAD};
    int content = n->line_count * LINE_H + 2 * TEXT_PAD;

    struct gui_event *e = ui->e;
    if (e && (e->type == EV_MOUSE_DOWN || (e->type == EV_MOUSE_MOVE && n->selecting)) &&
        (n->selecting || rect_contains(area, e->x, e->y)) && (e->type != EV_MOUSE_DOWN || e->button == BUTTON_LEFT)) {
        int line = (e->y - text.y + n->scroll_y) / LINE_H;
        size_t p = pos_at(n, line, e->x - text.x + n->scroll_x);
        n->cursor = p;
        if (e->type == EV_MOUSE_DOWN) {
            n->anchor = p;
            n->selecting = true;
            ui->st->focus = -1;
            ui->consumed = true;
        }
        n->goal_x = x_of(n, n->cursor);
        ui->changed = true;
    }
    if (e && e->type == EV_MOUSE_UP)
        n->selecting = false;
    if (e && e->type == EV_WHEEL && rect_contains(area, e->x, e->y)) {
        n->scroll_y = CLAMP(n->scroll_y + e->wheel * LINE_H * 3, 0, MAX(0, content - text.h));
        ui->changed = true;
    }
    if (e && e->type == EV_MOUSE_DOWN && e->button == BUTTON_RIGHT && rect_contains(area, e->x, e->y)) {
        static const char *const menu[] = {"Cut", "Copy", "Paste", "", "Select all"};
        ui_menu(w, e->x, e->y, menu, 5, pick);
    }
    int k = ui_key(ui);
    if (k != KEY_NONE && (ui->st->focus == -1 || ui->st->focus == 0)) {
        key(w, n, k);
        ui->consumed = true;
    }
    ui_scrollbar(ui, (struct rect){W - 10, TOOLBAR_H, 10, area.h}, &n->scroll_y, content, area.h);

    if (!ui->c)
        return;
    struct canvas *c = ui->c;
    gfx_clear(c, area, style.content);
    if (gutter) {
        gfx_clear(c, (struct rect){0, area.y, gutter, area.h}, color_mix(style.content, style.window, 110));
        gfx_fill(c, (struct rect){gutter - 1, area.y, 1, area.h}, style.separator);
    }
    struct canvas inner = *c;
    inner.clip = rect_intersect(c->clip, (struct rect){0, area.y, area.w, area.h});
    int first = n->scroll_y / LINE_H;
    size_t sa, sb;
    selection(n, &sa, &sb);
    int cursor_line = line_of(n, n->cursor);
    for (int line = first; line < n->line_count; line++) {
        int y = text.y + line * LINE_H - n->scroll_y;
        if (y > area.y + area.h)
            break;
        if (gutter) {
            char num[12];
            ksnprintf(num, sizeof(num), "%d", line + 1);
            gfx_text_box(&inner, (struct rect){0, y, gutter - 10, LINE_H}, &font_mono, num,
                         line == cursor_line ? style.text : style.tertiary, ALIGN_RIGHT);
        }
        size_t start = n->lines[line], end = line_end(n, line);
        struct canvas tc = inner;
        tc.clip = rect_intersect(inner.clip, (struct rect){gutter + 2, area.y, area.w - gutter - 2, area.h});
        if (sa < sb && sa <= end && sb >= start) {
            size_t a = MAX(sa, start), b = MIN(sb, end);
            int x0 = gfx_text_width_n(&font_mono, n->text + start, a - start);
            int x1 = gfx_text_width_n(&font_mono, n->text + start, b - start) + (sb > end ? 6 : 0);
            gfx_fill(&tc, (struct rect){text.x + x0 - n->scroll_x, y, x1 - x0, LINE_H},
                     w == wm_focused() ? ALPHA(style.accent, 70) : style.selection_inactive);
        }
        gfx_text_n(&tc, text.x - n->scroll_x, y + 1, &font_mono, n->text + start, end - start, style.text);
        if (line == cursor_line && w == wm_focused()) {
            int cx = text.x + x_of(n, n->cursor) - n->scroll_x;
            gfx_fill(&tc, (struct rect){cx, y + 1, 2, LINE_H - 2}, style.text);
        }
    }
    UNUSED(H);
}

static void paint(struct window *w, struct canvas *c)
{
    struct notepad *n = w->data;
    ui_run(w, &n->st, c, NULL, frame, n);
}

static void event(struct window *w, struct gui_event *e)
{
    struct notepad *n = w->data;
    ui_run(w, &n->st, NULL, e, frame, n);
}

static void discard(struct window *w, const char *value)
{
    UNUSED(value);
    struct notepad *n = w->data;
    n->modified = false;
    wm_close(w);
}

static bool on_close(struct window *w)
{
    struct notepad *n = w->data;
    if (!n->modified)
        return true;
    ui_confirm(w, "Do you want to discard the changes?", "Your changes will be lost if you don't save them.", discard);
    return false;
}

static void destroy(struct window *w)
{
    struct notepad *n = w->data;
    kfree(n->text);
    kfree(n->lines);
    kfree(n);
}

static const struct window_ops ops = {paint, event, on_close, destroy};

static void launch(const char *arg)
{
    struct notepad *n = kzalloc(sizeof(*n));
    if (!n)
        return;
    n->st.focus = -1;
    load_text(n, "", 0);
    struct window *w = wm_open("Untitled", ICON_NOTEPAD, 680, 460, &ops, n, WIN_RESIZABLE);
    if (!w) {
        kfree(n->text);
        kfree(n);
        return;
    }
    w->min_w = 360;
    w->min_h = 200;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    if (arg && *arg)
        open_path(w, n, arg);
}

static const struct gui_app notepad_app = {
    "Notepad", "Edit text files", ICON_NOTEPAD, launch,
    ".txt;.md;.c;.h;.cpp;.asm;.sasm;.inc;.conf;.theme;.log;.ini;.cfg;.json;.xml;.css;.js;.py;.bat;.synthrc", NULL};
GUI_APP(notepad_app);
