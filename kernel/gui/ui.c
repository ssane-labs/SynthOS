#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "internal.h"

#define SCROLL_W 12

void ui_run(struct window *w, struct ui_state *st, struct canvas *c, struct gui_event *e, ui_frame_fn frame,
            void *data)
{
    struct ui ui = {.win = w, .c = c, .e = e, .st = st, .id = 0, .hot = 0};
    frame(&ui, data);
    if (!e || !ui.win)
        return;
    if (e->type == EV_MOUSE_MOVE || e->type == EV_LEAVE) {
        int hot = e->type == EV_LEAVE ? 0 : ui.hot;
        if (hot != st->hot) {
            st->hot = hot;
            ui.changed = true;
        }
    }
    if (e->type == EV_MOUSE_UP && st->active) {
        st->active = 0;
        ui.changed = true;
    }
    if (e->type == EV_MOUSE_DOWN && !ui.consumed && st->focus) {
        st->focus = 0;
        ui.changed = true;
    }
    if (ui.changed || e->type == EV_KEY || e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP ||
        e->type == EV_RESIZE || e->type == EV_TIMER || e->type == EV_WHEEL)
        wm_invalidate(w);
}

int ui_next_id(struct ui *ui)
{
    return ++ui->id;
}

static bool mouse_in(struct ui *ui, struct rect r)
{
    return ui->e && (ui->e->type == EV_MOUSE_MOVE || ui->e->type == EV_MOUSE_DOWN || ui->e->type == EV_MOUSE_UP ||
                     ui->e->type == EV_WHEEL) &&
           rect_contains(r, ui->e->x, ui->e->y);
}

bool ui_hover(struct ui *ui, struct rect r)
{
    return mouse_in(ui, r);
}

static int track(struct ui *ui, struct rect r, bool *clicked)
{
    int id = ui_next_id(ui);
    *clicked = false;
    if (!ui->e)
        return id;
    bool in = mouse_in(ui, r);
    if (in)
        ui->hot = id;
    if (ui->e->type == EV_MOUSE_DOWN && in && ui->e->button == BUTTON_LEFT) {
        ui->st->active = id;
        ui->consumed = true;
        ui->changed = true;
    } else if (ui->e->type == EV_MOUSE_UP && ui->st->active == id) {
        *clicked = in;
        ui->consumed = true;
    }
    return id;
}

static bool is_hot(struct ui *ui, int id)
{
    return ui->st->hot == id;
}

static bool is_active(struct ui *ui, int id)
{
    return ui->st->active == id;
}

void ui_panel(struct ui *ui, struct rect r, uint32_t color)
{
    if (ui->c)
        gfx_fill(ui->c, r, color);
}

void ui_label(struct ui *ui, struct rect r, const char *text, const struct ui_font *f, uint32_t color,
              enum text_align align)
{
    if (ui->c)
        gfx_text_box(ui->c, r, f ? f : &font_sans, text, color, align);
}

bool ui_button(struct ui *ui, struct rect r, const char *label)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        uint32_t bg = is_active(ui, id) ? style.accent : is_hot(ui, id) ? style.button_hover : style.button;
        gfx_round(ui->c, r, 6, bg);
        gfx_round_frame(ui->c, r, 6, ALPHA(0xffffffffu, 30));
        gfx_text_box(ui->c, rect_inset(r, 4), &font_bold, label, is_active(ui, id) ? RGB(255, 255, 255) : style.text,
                     ALIGN_CENTER);
    }
    return clicked;
}

bool ui_tool(struct ui *ui, struct rect r, enum icon_id icon, const char *label, bool toggled)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        if (toggled || is_active(ui, id))
            gfx_round(ui->c, r, 6, style.selection);
        else if (is_hot(ui, id))
            gfx_round(ui->c, r, 6, style.button);
        int x = r.x + 6;
        if (label && *label) {
            gfx_icon(ui->c, x, r.y + (r.h - 16) / 2, icon, 16);
            gfx_text_box(ui->c, (struct rect){x + 22, r.y, r.w - 30, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
        } else {
            gfx_icon(ui->c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, icon, 16);
        }
    }
    return clicked;
}

bool ui_checkbox(struct ui *ui, struct rect r, const char *label, bool *value)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (clicked)
        *value = !*value;
    if (ui->c) {
        struct rect box = {r.x, r.y + (r.h - 18) / 2, 18, 18};
        gfx_round(ui->c, box, 4, *value ? style.accent : style.field);
        gfx_round_frame(ui->c, box, 4, is_hot(ui, id) ? style.accent : style.border);
        if (*value) {
            gfx_thick_line(ui->c, box.x + 4, box.y + 9, box.x + 7, box.y + 12, 2, RGB(255, 255, 255));
            gfx_thick_line(ui->c, box.x + 7, box.y + 12, box.x + 13, box.y + 5, 2, RGB(255, 255, 255));
        }
        gfx_text_box(ui->c, (struct rect){r.x + 26, r.y, r.w - 26, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
    }
    return clicked;
}

bool ui_radio(struct ui *ui, struct rect r, const char *label, bool selected)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        int cx = r.x + 9, cy = r.y + r.h / 2;
        gfx_circle(ui->c, cx, cy, 9, is_hot(ui, id) ? style.accent : style.border);
        gfx_circle(ui->c, cx, cy, 8, style.field);
        if (selected)
            gfx_circle(ui->c, cx, cy, 4, style.accent);
        gfx_text_box(ui->c, (struct rect){r.x + 26, r.y, r.w - 26, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
    }
    return clicked;
}

bool ui_slider(struct ui *ui, struct rect r, int *value, int min, int max)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    bool changed = false;
    if (ui->e && is_active(ui, id) && (ui->e->type == EV_MOUSE_DOWN || ui->e->type == EV_MOUSE_MOVE)) {
        int v = min + (ui->e->x - r.x) * (max - min) / MAX(r.w, 1);
        v = CLAMP(v, min, max);
        if (v != *value) {
            *value = v;
            changed = true;
            ui->changed = true;
        }
    }
    if (ui->c) {
        int cy = r.y + r.h / 2;
        int pos = r.x + (*value - min) * r.w / MAX(max - min, 1);
        gfx_round(ui->c, (struct rect){r.x, cy - 2, r.w, 4}, 2, style.field);
        gfx_round(ui->c, (struct rect){r.x, cy - 2, pos - r.x, 4}, 2, style.accent);
        gfx_circle(ui->c, pos, cy, 8, is_hot(ui, id) || is_active(ui, id) ? style.accent : style.button_hover);
    }
    return changed;
}

void ui_text_set(struct ui_text *t, const char *s)
{
    str_copy(t->buf, s, sizeof(t->buf));
    t->len = (int)strlen(t->buf);
    t->cursor = t->len;
    t->scroll = 0;
}

int ui_key(struct ui *ui)
{
    if (!ui->e || ui->e->type != EV_KEY || ui->consumed)
        return KEY_NONE;
    return ui->e->key;
}

static bool edit_text(struct ui_text *t, int key)
{
    int base = KEY_BASE(key);
    if (base == KEY_LEFT && t->cursor > 0)
        t->cursor--;
    else if (base == KEY_RIGHT && t->cursor < t->len)
        t->cursor++;
    else if (base == KEY_HOME)
        t->cursor = 0;
    else if (base == KEY_END)
        t->cursor = t->len;
    else if (key == KEY_BACKSPACE && t->cursor > 0) {
        memmove(t->buf + t->cursor - 1, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1));
        t->cursor--;
        t->len--;
    } else if (base == KEY_DELETE && t->cursor < t->len) {
        memmove(t->buf + t->cursor, t->buf + t->cursor + 1, (size_t)(t->len - t->cursor));
        t->len--;
    } else if (key == KEY_CTRL('v')) {
        for (const char *p = gui_clipboard(); *p && *p != '\n' && t->len < (int)sizeof(t->buf) - 1; p++) {
            memmove(t->buf + t->cursor + 1, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1));
            t->buf[t->cursor++] = *p;
            t->len++;
        }
    } else if (key >= 0x20 && key < 0x100 && key != 0x7f && t->len < (int)sizeof(t->buf) - 1) {
        memmove(t->buf + t->cursor + 1, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1));
        t->buf[t->cursor++] = (char)key;
        t->len++;
    } else {
        return false;
    }
    return true;
}

bool ui_textbox(struct ui *ui, struct rect r, struct ui_text *t)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    bool enter = false;
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && mouse_in(ui, r)) {
        ui->st->focus = id;
        int x = ui->e->x - r.x - 8 + t->scroll;
        t->cursor = 0;
        while (t->cursor < t->len && gfx_text_width_n(&font_sans, t->buf, (size_t)t->cursor + 1) <= x)
            t->cursor++;
    }
    if (ui->st->focus == id) {
        int key = ui_key(ui);
        if (key == KEY_ENTER || key == '\r') {
            enter = true;
            ui->consumed = true;
        } else if (key != KEY_NONE && edit_text(t, key)) {
            ui->consumed = true;
        }
    }
    int caret = gfx_text_width_n(&font_sans, t->buf, (size_t)t->cursor);
    if (caret - t->scroll > r.w - 16)
        t->scroll = caret - (r.w - 16);
    if (caret < t->scroll)
        t->scroll = caret;
    if (ui->c) {
        bool focused = ui->st->focus == id;
        gfx_round(ui->c, r, 6, style.field);
        gfx_round_frame(ui->c, r, 6, focused ? style.accent : is_hot(ui, id) ? style.button_hover : style.border);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, rect_inset(r, 2));
        int ty = r.y + (r.h - font_sans.height) / 2;
        gfx_text(&inner, r.x + 8 - t->scroll, ty, &font_sans, t->buf, style.text);
        if (focused)
            gfx_fill(&inner, (struct rect){r.x + 8 - t->scroll + caret, ty, 1, font_sans.height}, style.accent);
    }
    return enter;
}

bool ui_scrollbar(struct ui *ui, struct rect r, int *pos, int content, int view)
{
    int max = MAX(0, content - view);
    *pos = CLAMP(*pos, 0, max);
    bool clicked;
    int id = track(ui, r, &clicked);
    bool changed = false;
    int thumb_h = content > 0 ? MAX(24, r.h * view / MAX(content, 1)) : r.h;
    thumb_h = MIN(thumb_h, r.h);
    int travel = r.h - thumb_h;
    if (ui->e && is_active(ui, id) && max > 0 && (ui->e->type == EV_MOUSE_DOWN || ui->e->type == EV_MOUSE_MOVE)) {
        int y = ui->e->y - r.y - thumb_h / 2;
        int v = travel > 0 ? CLAMP(y, 0, travel) * max / travel : 0;
        if (v != *pos) {
            *pos = v;
            changed = true;
            ui->changed = true;
        }
    }
    if (ui->c && max > 0) {
        int ty = r.y + (travel > 0 ? *pos * travel / max : 0);
        gfx_round(ui->c, (struct rect){r.x + 3, ty + 2, r.w - 6, thumb_h - 4}, 3,
                  is_active(ui, id) || is_hot(ui, id) ? style.button_hover : style.button);
    }
    return changed;
}

int ui_list(struct ui *ui, struct rect r, struct ui_list *l, ui_list_draw_fn draw, void *ctx)
{
    int row_h = l->row_h ? l->row_h : 24;
    int content = l->count * row_h;
    struct rect area = {r.x, r.y, r.w - SCROLL_W, r.h};
    int result = LIST_NONE;
    int id = ui_next_id(ui);
    if (ui->e) {
        bool in = mouse_in(ui, area);
        if (in)
            ui->hot = id;
        if (ui->e->type == EV_WHEEL && rect_contains(r, ui->e->x, ui->e->y)) {
            l->scroll += ui->e->wheel * row_h * 3;
            ui->changed = true;
        } else if (ui->e->type == EV_MOUSE_DOWN && in) {
            int index = (ui->e->y - r.y + l->scroll) / row_h;
            ui->st->focus = id;
            ui->consumed = true;
            if (index >= 0 && index < l->count) {
                l->selected = index;
                result = ui->e->button == BUTTON_RIGHT ? LIST_CONTEXT : ui->e->clicks >= 2 ? LIST_ACTIVATE : LIST_SELECT;
            } else if (ui->e->button == BUTTON_RIGHT) {
                l->selected = -1;
                result = LIST_CONTEXT;
            } else {
                l->selected = -1;
                result = LIST_SELECT;
            }
        } else if (ui->st->focus == id && ui->e->type == EV_KEY && !ui->consumed && l->count) {
            int key = KEY_BASE(ui->e->key);
            int page = MAX(1, r.h / row_h - 1);
            int old = l->selected;
            if (key == KEY_DOWN)
                l->selected = MIN(l->count - 1, l->selected + 1);
            else if (key == KEY_UP)
                l->selected = MAX(0, l->selected - 1);
            else if (key == KEY_PGDN)
                l->selected = MIN(l->count - 1, l->selected + page);
            else if (key == KEY_PGUP)
                l->selected = MAX(0, l->selected - page);
            else if (key == KEY_HOME)
                l->selected = 0;
            else if (key == KEY_END)
                l->selected = l->count - 1;
            else if (key == KEY_ENTER || key == '\r')
                result = LIST_ACTIVATE;
            if (old != l->selected) {
                result = LIST_SELECT;
                if (l->selected * row_h < l->scroll)
                    l->scroll = l->selected * row_h;
                if ((l->selected + 1) * row_h > l->scroll + r.h)
                    l->scroll = (l->selected + 1) * row_h - r.h;
            }
            if (result != LIST_NONE)
                ui->consumed = true;
        }
    }
    l->scroll = CLAMP(l->scroll, 0, MAX(0, content - r.h));
    ui_scrollbar(ui, (struct rect){r.x + r.w - SCROLL_W, r.y, SCROLL_W, r.h}, &l->scroll, content, r.h);
    if (ui->c) {
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, area);
        int first = l->scroll / row_h;
        for (int i = first; i < l->count; i++) {
            struct rect row = {area.x, area.y + i * row_h - l->scroll, area.w, row_h};
            if (row.y >= area.y + area.h)
                break;
            bool sel = i == l->selected;
            if (sel)
                gfx_round(&inner, rect_inset(row, 1), 5, ui->st->focus == id ? style.selection : ALPHA(style.accent, 45));
            draw(&inner, row, i, sel, ctx);
        }
    }
    return result;
}

int ui_wrap_text(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color)
{
    int y = r.y;
    const char *p = s;
    while (*p) {
        const char *line_end = p, *last_space = NULL;
        while (*line_end && *line_end != '\n') {
            const char *next = line_end;
            utf8_next(&next);
            if (gfx_text_width_n(f, p, (size_t)(next - p)) > r.w)
                break;
            if (*line_end == ' ')
                last_space = line_end;
            line_end = next;
        }
        if (*line_end && *line_end != '\n' && last_space)
            line_end = last_space;
        if (line_end == p && *p && *p != '\n')
            utf8_next(&line_end);
        if (c)
            gfx_text_n(c, r.x, y, f, p, (size_t)(line_end - p), color);
        y += f->height + 2;
        p = line_end;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    return y - r.y;
}

struct menu {
    const char *const *items;
    int count;
    int hover;
    ui_pick_fn pick;
    struct window *owner;
};

#define MENU_ITEM_H 28

static void menu_paint(struct window *w, struct canvas *c)
{
    struct menu *m = w->data;
    gfx_fill(c, (struct rect){0, 0, c->w, c->h}, style.panel);
    gfx_frame(c, (struct rect){0, 0, c->w, c->h}, style.border);
    for (int i = 0; i < m->count; i++) {
        struct rect row = {4, 4 + i * MENU_ITEM_H, c->w - 8, MENU_ITEM_H};
        if (!m->items[i][0]) {
            gfx_fill(c, (struct rect){row.x + 6, row.y + MENU_ITEM_H / 2, row.w - 12, 1}, style.border);
            continue;
        }
        if (i == m->hover)
            gfx_round(c, row, 5, style.selection);
        gfx_text_box(c, (struct rect){row.x + 12, row.y, row.w - 16, row.h}, &font_sans, m->items[i], style.text,
                     ALIGN_LEFT);
    }
}

static void menu_event(struct window *w, struct gui_event *e)
{
    struct menu *m = w->data;
    int old = m->hover;
    int index = e->y >= 4 ? (e->y - 4) / MENU_ITEM_H : -1;
    if (index >= m->count || (index >= 0 && !m->items[index][0]))
        index = -1;
    if (e->type == EV_MOUSE_MOVE)
        m->hover = index;
    else if (e->type == EV_LEAVE)
        m->hover = -1;
    else if (e->type == EV_KEY) {
        int key = KEY_BASE(e->key);
        if (key == KEY_ESC) {
            wm_destroy(w);
            return;
        }
        if (key == KEY_DOWN || key == KEY_UP) {
            int step = key == KEY_DOWN ? 1 : -1;
            int h = m->hover;
            for (int n = 0; n < m->count; n++) {
                h = (h + step + m->count) % m->count;
                if (m->items[h][0])
                    break;
            }
            m->hover = h;
        } else if ((key == KEY_ENTER || key == '\r') && m->hover >= 0) {
            index = m->hover;
            e->type = EV_MOUSE_UP;
            e->button = BUTTON_LEFT;
        }
    }
    if (e->type == EV_MOUSE_UP && e->button == BUTTON_LEFT && index >= 0) {
        ui_pick_fn pick = m->pick;
        struct window *owner = m->owner;
        wm_destroy(w);
        if (pick && wm_window_alive(owner))
            pick(owner, index);
        return;
    }
    if (old != m->hover)
        wm_invalidate(w);
}

static void menu_destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops menu_ops = {menu_paint, menu_event, NULL, menu_destroy};

void ui_menu(struct window *owner, int x, int y, const char *const *items, int count, ui_pick_fn pick)
{
    wm_close_popups();
    struct menu *m = kzalloc(sizeof(*m));
    if (!m)
        return;
    *m = (struct menu){items, count, -1, pick, owner};
    int width = 120;
    for (int i = 0; i < count; i++)
        width = MAX(width, gfx_text_width(&font_sans, items[i]) + 40);
    struct rect cr = wm_client_rect(owner);
    if (!wm_popup(cr.x + x, cr.y + y, width, count * MENU_ITEM_H + 8, &menu_ops, m, owner))
        kfree(m);
}

struct dialog {
    struct ui_state st;
    char text[512];
    struct ui_text input;
    bool has_input;
    ui_prompt_fn done;
    struct window *owner;
};

static void dialog_frame(struct ui *ui, void *data)
{
    struct dialog *d = data;
    struct rect area = {0, 0, ui->win->surface.w, ui->win->surface.h};
    ui_panel(ui, area, style.window);
    int y = 16;
    if (ui->c) {
        gfx_icon(ui->c, 18, y, d->has_input ? ICON_TEXT : ICON_INFO, 32);
        y += ui_wrap_text(ui->c, (struct rect){64, y + 4, area.w - 84, area.h}, &font_sans, d->text, style.text);
    } else {
        y += ui_wrap_text(NULL, (struct rect){64, y + 4, area.w - 84, area.h}, &font_sans, d->text, style.text);
    }
    bool submit = false;
    if (d->has_input) {
        if (!ui->st->focus)
            ui->st->focus = ui->id + 1;
        submit = ui_textbox(ui, (struct rect){64, MAX(y + 10, 56), area.w - 84, 30}, &d->input);
    }
    bool two = d->has_input || d->done;
    struct rect ok = {area.w - (two ? 200 : 104), area.h - 46, 88, 32};
    if (ui_button(ui, ok, "OK"))
        submit = true;
    bool cancel = false;
    if (two && ui_button(ui, (struct rect){area.w - 104, area.h - 46, 88, 32}, "Cancel"))
        cancel = true;
    int key = ui_key(ui);
    if (key == KEY_ESC)
        cancel = true;
    if (!d->has_input && (key == KEY_ENTER || key == '\r'))
        submit = true;
    if (submit || cancel) {
        struct window *w = ui->win;
        ui_prompt_fn done = d->done;
        struct window *owner = d->owner;
        char value[256];
        str_copy(value, d->input.buf, sizeof(value));
        ui->win = NULL;
        wm_destroy(w);
        if (submit && done && wm_window_alive(owner))
            done(owner, value);
    }
}

static void dialog_paint(struct window *w, struct canvas *c)
{
    struct dialog *d = w->data;
    ui_run(w, &d->st, c, NULL, dialog_frame, d);
}

static void dialog_event(struct window *w, struct gui_event *e)
{
    struct dialog *d = w->data;
    ui_run(w, &d->st, NULL, e, dialog_frame, d);
}

static void dialog_destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops dialog_ops = {dialog_paint, dialog_event, NULL, dialog_destroy};

static void open_dialog(struct window *owner, const char *title, const char *text, const char *initial,
                        ui_prompt_fn done)
{
    struct dialog *d = kzalloc(sizeof(*d));
    if (!d)
        return;
    str_copy(d->text, text, sizeof(d->text));
    d->has_input = initial != NULL;
    if (initial)
        ui_text_set(&d->input, initial);
    d->done = done;
    d->owner = owner;
    int text_h = ui_wrap_text(NULL, (struct rect){0, 0, 380 - 84, 1000}, &font_sans, text, 0);
    int h = MAX(120, 16 + text_h + (initial ? 50 : 10) + 60);
    struct window *w = wm_open(title, d->has_input ? ICON_TEXT : ICON_INFO, 380, h, &dialog_ops, d, WIN_DIALOG);
    if (!w) {
        kfree(d);
        return;
    }
    w->owner = owner;
}

void gui_message(const char *title, const char *text)
{
    open_dialog(NULL, title, text, NULL, NULL);
}

void ui_confirm(struct window *owner, const char *title, const char *text, ui_prompt_fn done)
{
    open_dialog(owner, title, text, NULL, done);
}

void ui_prompt(struct window *owner, const char *title, const char *label, const char *initial, ui_prompt_fn done)
{
    open_dialog(owner, title, label, initial ? initial : "", done);
}
