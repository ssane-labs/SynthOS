#ifndef SYNTH_SYSTOOL_H
#define SYNTH_SYSTOOL_H

/* Shared by the system tools (Console, System Information, Network Utility, Disk Utility): a window with the glass
   sidebar, a toolbar, the output of shell commands and a scrolling monospace text view with a filter. Each module
   gets its own copy, so everything here is static. */

#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>

#include "../internal.h"

#define TOOL __attribute__((unused)) static
#define TOOL_SIDE 210
#define TOOL_ROW 30

struct tool_item {
    enum symbol_id sym;
    const char *label; /* NULL: label is a section header */
    const char *header;
};

struct tool_text {
    char *text;
    int scroll, lines;
    bool follow; /* keep the end in view */
};

TOOL struct window *tool_open(const char *title, enum icon_id icon, int w, int h, const struct window_ops *ops,
                              void *data)
{
    struct window *win = wm_open(title, icon, w, h, ops, data, WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (win) {
        win->toolbar_h = TOOLBAR_HEIGHT;
        win->min_w = 560;
        win->min_h = 360;
    }
    return win;
}

/* the sidebar with its items; true when the selection changed */
TOOL bool tool_sidebar(struct ui *ui, int H, const struct tool_item *items, int n, int *selected)
{
    ui_sidebar_panel(ui, TOOL_SIDE, H, style.window);
    int y = 46, index = 0;
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (items[i].header) {
            ui_sidebar_header(ui, (struct rect){22, y + (i ? 6 : 0), TOOL_SIDE - 32, 20}, items[i].header);
            y += i ? 28 : 22;
        }
        if (!items[i].label)
            continue;
        if (y + TOOL_ROW < H - 8 &&
            ui_sidebar_item(ui, (struct rect){14, y, TOOL_SIDE - 26, TOOL_ROW - 2}, items[i].sym, items[i].label,
                            *selected == index) &&
            *selected != index) {
            *selected = index;
            changed = true;
        }
        y += TOOL_ROW;
        index++;
    }
    return changed;
}

/* the content side: background, separator under the toolbar and the title; returns the content area */
TOOL struct rect tool_page(struct ui *ui, int W, int H, const char *title, const char *subtitle)
{
    int x = TOOL_SIDE + 1;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){x, 0, W - x, H}, style.content);
        gfx_fill(ui->c, (struct rect){x, TOOLBAR_HEIGHT - 1, W - x, 1}, style.separator);
    }
    int tw = gfx_text_width(&font_bold, title);
    ui_label(ui, (struct rect){x + 18, 0, tw + 4, TOOLBAR_HEIGHT}, title, &font_bold, style.text, ALIGN_LEFT);
    if (subtitle)
        ui_label(ui, (struct rect){x + 30 + tw, 0, W - x - tw - 60, TOOLBAR_HEIGHT}, subtitle, &font_small,
                 style.tertiary, ALIGN_LEFT);
    return (struct rect){x, TOOLBAR_HEIGHT, W - x, H - TOOLBAR_HEIGHT};
}

/* runs a shell command and returns what it printed (errors too); kfree it */
TOOL char *tool_run(const char *command)
{
    static struct shell *sh;
    if (!sh && !(sh = shell_create()))
        return kstrdup("no shell\n");
    char line[512], *out = NULL;
    ksnprintf(line, sizeof(line), "%s 2>&1", command);
    shell_capture(sh, line, &out);
    return out ? out : kstrdup("");
}

/* commands draw bars with CP437 blocks for the text console; the desktop fonts are UTF-8 */
TOOL void tool_clean(char *text)
{
    for (unsigned char *p = (unsigned char *)text; p && *p; p++) {
        if (*p < 0x80)
            continue;
        int more = *p >= 0xc2 && *p <= 0xdf ? 1 : *p >= 0xe0 && *p <= 0xef ? 2 : *p >= 0xf0 && *p <= 0xf4 ? 3 : -1;
        bool ok = more > 0;
        for (int i = 1; ok && i <= more; i++)
            ok = (p[i] & 0xc0) == 0x80;
        if (ok) {
            p += more;
            continue;
        }
        *p = *p == 0xb0 || *p == 0xb1 ? '.' : *p == 0xb2 || *p == 0xdb || *p == 0xdc || *p == 0xdf ? '#'
           : *p == 0xfa ? '.' : '?';
    }
}

TOOL void tool_text_set(struct tool_text *t, char *text)
{
    tool_clean(text);
    kfree(t->text);
    t->text = text;
    t->lines = 0;
    for (const char *p = text; p && *p; p++)
        t->lines += *p == '\n';
}

TOOL bool tool_has(const char *line, size_t n, const char *find)
{
    size_t k = strlen(find);
    for (size_t i = 0; k && i + k <= n; i++)
        if (!str_ncasecmp(line + i, find, k))
            return true;
    return !k;
}

TOOL uint32_t tool_line_color(const char *line, size_t n)
{
    if (tool_has(line, n, "error") || tool_has(line, n, "fail") || tool_has(line, n, "panic") ||
        tool_has(line, n, "fault") || tool_has(line, n, "denied"))
        return RGB(255, 105, 97);
    if (tool_has(line, n, "warn") || tool_has(line, n, "timeout") || tool_has(line, n, "not found"))
        return RGB(255, 196, 70);
    return style.text;
}

/* the text, one line per row, only lines containing filter (case-insensitive) */
TOOL void tool_text_view(struct ui *ui, struct rect r, struct tool_text *t, const char *filter)
{
    const struct ui_font *f = &font_mono;
    int lh = f->height + 2, rows = MAX(1, (r.h - 16) / lh);
    if (ui->c)
        gfx_round(ui->c, r, 10, style.dark ? RGB(20, 20, 23) : RGB(250, 250, 252));
    if (!t->text)
        return;
    int shown = 0;
    for (const char *p = t->text; *p;) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        shown += tool_has(p, n, filter);
        p += n + (end ? 1 : 0);
    }
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(r, ui->e->x, ui->e->y)) {
        t->scroll -= ui->e->wheel * 3;
        t->follow = t->scroll >= shown - rows;
        ui->consumed = true;
    }
    int key = ui_key(ui);
    if (key == KEY_PGUP || key == KEY_PGDN || key == KEY_HOME || key == KEY_END) {
        t->scroll += key == KEY_PGUP ? -rows : key == KEY_PGDN ? rows : key == KEY_HOME ? -1000000 : 1000000;
        t->follow = key == KEY_END;
        ui->consumed = true;
    }
    if (t->follow)
        t->scroll = shown - rows;
    t->scroll = CLAMP(t->scroll, 0, MAX(0, shown - rows));
    int bar = t->scroll * lh;
    if (ui_scrollbar(ui, (struct rect){r.x + r.w - 12, r.y + 6, 10, r.h - 12}, &bar, shown * lh, rows * lh) &&
        bar / lh != t->scroll) {
        t->scroll = bar / lh;
        t->follow = false;
    }
    if (!ui->c)
        return;
    struct canvas clip = *ui->c;
    clip.clip = rect_intersect(ui->c->clip, rect_inset(r, 8));
    int index = 0, y = r.y + 8;
    for (const char *p = t->text; *p && y < r.y + r.h - 8;) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (tool_has(p, n, filter) && index++ >= t->scroll) {
            gfx_text_n(&clip, r.x + 12, y, f, p, n, tool_line_color(p, n));
            y += lh;
        }
        p += n + (end ? 1 : 0);
    }
    if (!shown)
        gfx_text_box(ui->c, r, &font_sans, *filter ? "Nothing matches" : "Nothing to show", style.tertiary,
                     ALIGN_CENTER);
}

TOOL void tool_launch(const char *name, const char *arg)
{
    const struct gui_app *app = gui_app_find(name);
    if (app)
        gui_app_launch(app, arg);
}

#endif
