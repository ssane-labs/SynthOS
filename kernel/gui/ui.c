#include <synth/codepage.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "internal.h"

#define SCROLL_W 10

static bool list_focused;

void ui_run(struct window *w, struct ui_state *st, struct canvas *c, struct gui_event *e, ui_frame_fn frame,
            void *data)
{
    struct ui ui = {.win = w, .c = c, .e = e, .st = st, .id = 0, .hot = 0};
    frame(&ui, data);
    if (e && ui.consumed)
        e->handled = true;
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
        e->type == EV_RESIZE || e->type == EV_TIMER || e->type == EV_WHEEL || e->type == EV_FOCUS ||
        e->type == EV_BLUR)
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

static bool window_active(struct ui *ui)
{
    return ui->win && (ui->win == wm_focused() || (ui->win->flags & (WIN_POPUP | WIN_OVERLAY)));
}

void ui_panel(struct ui *ui, struct rect r, uint32_t color)
{
    if (ui->c)
        gfx_clear(ui->c, rect_intersect(r, ui->c->clip), color);
}

void ui_separator(struct ui *ui, int x, int y, int w)
{
    if (ui->c)
        gfx_fill(ui->c, (struct rect){x, y, w, 1}, style.separator);
}

void ui_label(struct ui *ui, struct rect r, const char *text, const struct ui_font *f, uint32_t color,
              enum text_align align)
{
    if (ui->c)
        gfx_text_box(ui->c, r, f ? f : &font_sans, text, color, align);
}

static int control_radius(struct rect r)
{
    return MIN(r.h / 2, r.h >= 30 ? 9 : 7);
}

static void control_box(struct canvas *c, struct rect r, int radius, bool pressed)
{
    if (style.dark) {
        gfx_round(c, r, radius, pressed ? style.control_pressed : style.control);
        gfx_round_frame_gradient(c, r, radius, ALPHA(0xffffffffu, 46), ALPHA(0xffffffffu, 8));
    } else {
        gfx_round(c, (struct rect){r.x, r.y + 1, r.w, r.h}, radius, ALPHA(0, 22));
        gfx_round(c, r, radius, pressed ? style.control_pressed : style.control);
        gfx_round_frame(c, r, radius, style.control_border);
    }
}

bool ui_button(struct ui *ui, struct rect r, const char *label)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        control_box(ui->c, r, control_radius(r), is_active(ui, id) && is_hot(ui, id));
        gfx_text_box(ui->c, rect_inset(r, 4), &font_sans, label, style.text, ALIGN_CENTER);
    }
    return clicked;
}

bool ui_button_primary(struct ui *ui, struct rect r, const char *label)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        int radius = control_radius(r);
        bool pressed = is_active(ui, id) && is_hot(ui, id);
        uint32_t base = window_active(ui) ? style.accent : style.control;
        if (pressed)
            base = color_shade(base, -40);
        if (!style.dark)
            gfx_round(ui->c, (struct rect){r.x, r.y + 1, r.w, r.h}, radius, ALPHA(0, 26));
        gfx_round_gradient(ui->c, r, radius, color_shade(base, 22), base);
        gfx_round_frame_gradient(ui->c, r, radius, ALPHA(0xffffffffu, 50), ALPHA(0, 20));
        gfx_text_box(ui->c, rect_inset(r, 4), &font_sans, label, window_active(ui) ? RGB(255, 255, 255) : style.text,
                     ALIGN_CENTER);
    }
    return clicked;
}

static void tool_back(struct ui *ui, int id, struct rect r, bool toggled)
{
    if (toggled)
        gfx_round(ui->c, r, 7, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 22));
    else if (is_active(ui, id))
        gfx_round(ui->c, r, 7, style.dark ? ALPHA(0xffffffffu, 40) : ALPHA(0, 30));
    else if (is_hot(ui, id))
        gfx_round(ui->c, r, 7, style.hover);
}

bool ui_tool(struct ui *ui, struct rect r, enum icon_id icon, const char *label, bool toggled)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        tool_back(ui, id, r, toggled);
        if (label && *label) {
            gfx_icon(ui->c, r.x + 8, r.y + (r.h - 16) / 2, icon, 16);
            gfx_text_box(ui->c, (struct rect){r.x + 30, r.y, r.w - 36, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
        } else {
            gfx_icon(ui->c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, icon, 16);
        }
    }
    return clicked;
}

bool ui_symbol_button(struct ui *ui, struct rect r, enum symbol_id sym, bool toggled)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        tool_back(ui, id, r, toggled);
        gfx_symbol(ui->c, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, sym, 16,
                   toggled ? style.accent : ALPHA(style.text, 210));
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
        struct rect box = {r.x, r.y + (r.h - 16) / 2, 16, 16};
        if (*value) {
            gfx_round_gradient(ui->c, box, 4, color_shade(style.accent, 20), style.accent);
            gfx_symbol(ui->c, box.x + 2, box.y + 2, SYM_CHECK, 12, RGB(255, 255, 255));
        } else {
            control_box(ui->c, box, 4, is_active(ui, id));
        }
        gfx_text_box(ui->c, (struct rect){r.x + 24, r.y, r.w - 24, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
    }
    return clicked;
}

bool ui_switch(struct ui *ui, struct rect r, bool *value)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    UNUSED(id);
    if (clicked)
        *value = !*value;
    if (ui->c) {
        int h = r.h, radius = h / 2;
        uint32_t track_color = *value ? style.accent : style.dark ? RGB(72, 72, 76) : RGB(226, 226, 230);
        gfx_round(ui->c, r, radius, track_color);
        if (!style.dark && !*value)
            gfx_round_frame(ui->c, r, radius, ALPHA(0, 20));
        int knob = h - 4, kx = *value ? r.x + r.w - knob - 2 : r.x + 2;
        struct rect k = {kx, r.y + 2, knob, knob};
        gfx_drop_shadow(ui->c, k, knob / 2, 4, 70, 1);
        gfx_round(ui->c, k, knob / 2, RGB(255, 255, 255));
    }
    return clicked;
}

bool ui_radio(struct ui *ui, struct rect r, const char *label, bool selected)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        int cx = r.x + 8, cy = r.y + r.h / 2;
        if (selected) {
            gfx_circle(ui->c, cx, cy, 8, style.accent);
            gfx_circle(ui->c, cx, cy, 3, RGB(255, 255, 255));
        } else {
            gfx_circle(ui->c, cx, cy + (style.dark ? 0 : 1), 8, style.dark ? ALPHA(0, 0) : ALPHA(0, 22));
            gfx_circle(ui->c, cx, cy, 8, is_active(ui, id) ? style.control_pressed : style.control);
            gfx_ring(ui->c, cx, cy, 8, style.control_border);
        }
        gfx_text_box(ui->c, (struct rect){r.x + 24, r.y, r.w - 24, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
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
        gfx_round(ui->c, (struct rect){r.x, cy - 2, r.w, 4}, 2, style.dark ? RGB(80, 80, 84) : RGB(216, 216, 220));
        gfx_round(ui->c, (struct rect){r.x, cy - 2, pos - r.x, 4}, 2, style.accent);
        struct rect knob = {pos - 9, cy - 9, 18, 18};
        gfx_drop_shadow(ui->c, knob, 9, 5, 80, 1);
        gfx_circle(ui->c, pos, cy, 9, RGB(255, 255, 255));
        gfx_ring(ui->c, pos, cy, 9, ALPHA(0, 30));
    }
    return changed;
}

/* The wide slider of Control Center: a capsule that fills from the left, with its symbol inside. */
bool ui_capsule_slider(struct ui *ui, struct rect r, enum symbol_id sym, int *value, int min, int max)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    bool changed = false;
    int rad = r.h / 2, span = MAX(r.w - r.h, 1);
    if (ui->e && is_active(ui, id) && (ui->e->type == EV_MOUSE_DOWN || ui->e->type == EV_MOUSE_MOVE)) {
        int v = min + ((ui->e->x - r.x - rad) * (max - min) + span / 2) / span;
        v = CLAMP(v, min, max);
        if (v != *value) {
            *value = v;
            changed = true;
            ui->changed = true;
        }
    }
    if (ui->c) {
        int fill = r.h + (*value - min) * span / MAX(max - min, 1);
        gfx_round(ui->c, r, rad, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 22));
        uint32_t knob = style.dark ? RGB(236, 236, 240) : RGB(255, 255, 255);
        gfx_round(ui->c, (struct rect){r.x, r.y, fill, r.h}, rad, knob);
        struct rect k = {r.x + fill - r.h, r.y, r.h, r.h};
        gfx_drop_shadow(ui->c, k, rad, 4, 60, 1);
        gfx_circle(ui->c, k.x + rad, k.y + rad, rad, knob);
        gfx_ring(ui->c, k.x + rad, k.y + rad, rad, ALPHA(0, 26));
        gfx_symbol(ui->c, r.x + (r.h - 16) / 2 + 1, r.y + (r.h - 16) / 2, sym, 16, RGB(118, 118, 124));
    }
    return changed;
}

bool ui_segmented(struct ui *ui, struct rect r, const char *const *labels, int count, int *selected)
{
    bool changed = false;
    int sw = r.w / MAX(count, 1);
    if (ui->c)
        gfx_round(ui->c, r, 7, style.dark ? ALPHA(0xffffffffu, 24) : ALPHA(0, 16));
    for (int i = 0; i < count; i++) {
        struct rect s = {r.x + i * sw, r.y, i == count - 1 ? r.w - i * sw : sw, r.h};
        bool clicked;
        int id = track(ui, s, &clicked);
        if (clicked && *selected != i) {
            *selected = i;
            changed = true;
        }
        if (!ui->c)
            continue;
        if (i == *selected) {
            struct rect k = rect_inset(s, 2);
            if (!style.dark)
                gfx_drop_shadow(ui->c, k, 6, 4, 50, 1);
            gfx_round(ui->c, k, 6, style.dark ? RGB(100, 100, 104) : RGB(255, 255, 255));
        } else if (is_active(ui, id)) {
            gfx_round(ui->c, rect_inset(s, 2), 6, style.hover);
        } else if (i > 0 && i - 1 != *selected) {
            gfx_fill(ui->c, (struct rect){s.x, s.y + 6, 1, s.h - 12}, style.separator);
        }
        gfx_text_box(ui->c, rect_inset(s, 4), i == *selected ? &font_bold : &font_sans, labels[i], style.text,
                     ALIGN_CENTER);
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

static int text_back(const char *s, int i)
{
    while (i > 0 && ((uint8_t)s[--i] & 0xc0) == 0x80)
        ;
    return i;
}

static int text_next(const char *s, int i, int len)
{
    while (i < len && ((uint8_t)s[++i] & 0xc0) == 0x80)
        ;
    return MIN(i, len);
}

static void text_insert(struct ui_text *t, const char *p, int n)
{
    if (t->len + n >= (int)sizeof(t->buf))
        return;
    memmove(t->buf + t->cursor + n, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1));
    memcpy(t->buf + t->cursor, p, (size_t)n);
    t->cursor += n;
    t->len += n;
}

bool ui_text_edit(struct ui_text *t, int key)
{
    int base = KEY_BASE(key);
    if (KEY_IS_TEXT(key)) {
        char enc[4];
        text_insert(t, enc, (int)utf8_encode((uint32_t)KEY_TEXT_CP(key), enc));
    } else if (base == KEY_LEFT && t->cursor > 0)
        t->cursor = text_back(t->buf, t->cursor);
    else if (base == KEY_RIGHT && t->cursor < t->len)
        t->cursor = text_next(t->buf, t->cursor, t->len);
    else if (base == KEY_HOME)
        t->cursor = 0;
    else if (base == KEY_END)
        t->cursor = t->len;
    else if (key == KEY_BACKSPACE && t->cursor > 0) {
        int from = text_back(t->buf, t->cursor);
        memmove(t->buf + from, t->buf + t->cursor, (size_t)(t->len - t->cursor + 1));
        t->len -= t->cursor - from;
        t->cursor = from;
    } else if (base == KEY_DELETE && t->cursor < t->len) {
        int to = text_next(t->buf, t->cursor, t->len);
        memmove(t->buf + t->cursor, t->buf + to, (size_t)(t->len - to + 1));
        t->len -= to - t->cursor;
    } else if (key == KEY_CTRL('u')) {
        t->buf[0] = 0;
        t->len = t->cursor = 0;
    } else if (key == KEY_CTRL('v')) {
        const char *clip = gui_clipboard();
        int n = 0;
        while (clip[n] && clip[n] != '\n')
            n++;
        text_insert(t, clip, MIN(n, (int)sizeof(t->buf) - 1 - t->len));
    } else if (key >= 0x20 && key < 0x100 && key != 0x7f) {
        char enc[4];
        text_insert(t, enc, (int)utf8_encode((uint32_t)key, enc));
    } else {
        return false;
    }
    return true;
}

static void shown_text(const struct ui_text *t, char *out, size_t cap, int *caret_chars)
{
    if (!t->password) {
        str_copy(out, t->buf, cap);
        *caret_chars = t->cursor;
        return;
    }
    size_t n = 0;
    *caret_chars = 0;
    for (int i = 0; i < t->len && n + 4 < cap; i = text_next(t->buf, i, t->len)) {
        if (i < t->cursor)
            *caret_chars += 3;
        memcpy(out + n, "•", 3);
        n += 3;
    }
    out[n] = 0;
}

static void focus_ring(struct canvas *c, struct rect r, int radius)
{
    gfx_round_frame(c, (struct rect){r.x - 3, r.y - 3, r.w + 6, r.h + 6}, radius + 3, ALPHA(style.accent, 70));
    gfx_round_frame(c, (struct rect){r.x - 2, r.y - 2, r.w + 4, r.h + 4}, radius + 2, ALPHA(style.accent, 140));
    gfx_round_frame(c, (struct rect){r.x - 1, r.y - 1, r.w + 2, r.h + 2}, radius + 1, ALPHA(style.accent, 200));
}

static bool text_field(struct ui *ui, struct rect r, struct ui_text *t, const char *hint, int radius, int inset,
                       bool search)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    bool enter = false;
    const struct ui_font *f = &font_sans;
    char shown[sizeof(t->buf) * 3];
    int caret_chars;
    shown_text(t, shown, sizeof(shown), &caret_chars);
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && mouse_in(ui, r)) {
        ui->st->focus = id;
        int x = ui->e->x - r.x - inset + t->scroll;
        t->cursor = 0;
        while (t->cursor < t->len && gfx_text_width_n(f, t->buf, (size_t)text_next(t->buf, t->cursor, t->len)) <= x)
            t->cursor = text_next(t->buf, t->cursor, t->len);
    }
    if (ui->st->focus == id) {
        int key = ui_key(ui);
        if (key == KEY_ENTER || key == '\r') {
            enter = true;
            ui->consumed = true;
        } else if (key != KEY_NONE && ui_text_edit(t, key)) {
            ui->consumed = true;
        }
        shown_text(t, shown, sizeof(shown), &caret_chars);
    }
    int caret = gfx_text_width_n(f, shown, (size_t)caret_chars);
    int room = r.w - inset - 8;
    if (caret - t->scroll > room)
        t->scroll = caret - room;
    if (caret < t->scroll)
        t->scroll = caret;
    if (ui->c) {
        bool focused = ui->st->focus == id;
        if (search) {
            gfx_round(ui->c, r, radius, style.dark ? ALPHA(0xffffffffu, 26) : ALPHA(0, 14));
            gfx_symbol(ui->c, r.x + 8, r.y + (r.h - 14) / 2, SYM_SEARCH, 14, style.secondary);
        } else {
            gfx_round(ui->c, r, radius, style.field);
            gfx_round_frame(ui->c, r, radius, style.field_border);
        }
        if (focused)
            focus_ring(ui->c, r, radius);
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, (struct rect){r.x + inset - 2, r.y + 2, r.w - inset - 2, r.h - 4});
        int ty = r.y + (r.h - f->height) / 2;
        if (!t->len && hint)
            gfx_text(&inner, r.x + inset, ty, f, hint, style.secondary);
        else
            gfx_text(&inner, r.x + inset - t->scroll, ty, f, shown, style.text);
        if (focused)
            gfx_fill(&inner, (struct rect){r.x + inset - t->scroll + caret, ty + 2, 1, f->height - 3}, style.accent);
    }
    return enter;
}

bool ui_textbox(struct ui *ui, struct rect r, struct ui_text *t)
{
    return text_field(ui, r, t, NULL, 6, 8, false);
}

bool ui_textbox_hint(struct ui *ui, struct rect r, struct ui_text *t, const char *hint)
{
    return text_field(ui, r, t, hint, 6, 8, false);
}

bool ui_search(struct ui *ui, struct rect r, struct ui_text *t, const char *hint)
{
    return text_field(ui, r, t, hint ? hint : "Search", r.h / 2, 28, true);
}

bool ui_scrollbar(struct ui *ui, struct rect r, int *pos, int content, int view)
{
    int max = MAX(0, content - view);
    *pos = CLAMP(*pos, 0, max);
    bool clicked;
    int id = track(ui, r, &clicked);
    bool changed = false;
    int thumb_h = content > 0 ? MAX(28, r.h * view / MAX(content, 1)) : r.h;
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
        bool strong = is_active(ui, id) || is_hot(ui, id);
        int w = strong ? 8 : 6;
        gfx_round(ui->c, (struct rect){r.x + r.w - w - 2, ty + 2, w, thumb_h - 4}, w / 2,
                  ALPHA(style.dark ? 0xffffffffu : 0, strong ? 130 : 90));
    }
    return changed;
}

uint32_t ui_row_text(bool selected)
{
    return selected && list_focused ? RGB(255, 255, 255) : style.text;
}

int ui_list(struct ui *ui, struct rect r, struct ui_list *l, ui_list_draw_fn draw, void *ctx)
{
    int row_h = l->row_h ? l->row_h : 24;
    int content = l->count * row_h;
    struct rect area = r;
    int result = LIST_NONE;
    int id = ui_next_id(ui);
    if (ui->e) {
        bool in = mouse_in(ui, area) && ui->e->x < r.x + r.w - SCROLL_W;
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
    if (ui->c) {
        struct canvas inner = *ui->c;
        inner.clip = rect_intersect(ui->c->clip, area);
        list_focused = ui->st->focus == id && window_active(ui);
        int first = l->scroll / row_h;
        for (int i = first; i < l->count; i++) {
            struct rect row = {area.x, area.y + i * row_h - l->scroll, area.w, row_h};
            if (row.y >= area.y + area.h)
                break;
            bool sel = i == l->selected;
            if (sel)
                gfx_round(&inner, (struct rect){row.x + 6, row.y, row.w - 12, row.h}, 6,
                          list_focused ? style.selection : style.selection_inactive);
            else if (l->zebra && (i & 1))
                gfx_round(&inner, (struct rect){row.x + 6, row.y, row.w - 12, row.h}, 6, style.zebra);
            draw(&inner, row, i, sel, ctx);
        }
        list_focused = false;
    }
    ui_scrollbar(ui, (struct rect){r.x + r.w - SCROLL_W, r.y, SCROLL_W, r.h}, &l->scroll, content, r.h);
    return result;
}

bool ui_sidebar_item(struct ui *ui, struct rect r, enum symbol_id sym, const char *label, bool selected)
{
    bool clicked;
    int id = track(ui, r, &clicked);
    if (ui->c) {
        if (selected)
            gfx_round(ui->c, r, 7, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 24));
        else if (is_active(ui, id))
            gfx_round(ui->c, r, 7, style.hover);
        gfx_symbol(ui->c, r.x + 8, r.y + (r.h - 16) / 2, sym, 16, style.accent);
        gfx_text_box(ui->c, (struct rect){r.x + 32, r.y, r.w - 36, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
    }
    return clicked;
}

void ui_sidebar_panel(struct ui *ui, int width, int height, uint32_t back)
{
    if (!ui->c)
        return;
    struct canvas *c = ui->c;
    gfx_clear(c, (struct rect){0, 0, width, height}, back);
    struct rect r = {8, 8, width - 8, height - 16};
    int radius = 12;
    uint32_t panel = style.sidebar;
    unsigned pa = panel >> 24;
    struct rect area = rect_intersect(r, c->clip);
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t *row = c->px + y * c->stride;
        bool corner_row = y < r.y + radius || y >= r.y + r.h - radius;
        for (int x = area.x; x < area.x + area.w; x++) {
            int cov = corner_row && (x < r.x + radius || x >= r.x + r.w - radius) ? gfx_corner_coverage(r, radius, x, y)
                                                                                 : 256;
            if (cov >= 256) {
                row[x] = panel;
            } else if (cov > 0) {
                unsigned a = (pa * (unsigned)cov + 255u * (256u - (unsigned)cov)) >> 8;
                uint32_t mix = color_mix(back | 0xff000000u, panel | 0xff000000u, cov * 255 / 256);
                row[x] = (mix & 0x00ffffffu) | a << 24;
            }
        }
    }
    gfx_round_frame_gradient(c, r, radius, ALPHA(0xffffffffu, style.dark ? 70 : 200),
                             ALPHA(0xffffffffu, style.dark ? 20 : 90));
    gfx_round_frame(c, (struct rect){r.x - 1, r.y - 1, r.w + 2, r.h + 2}, radius + 1,
                    style.dark ? ALPHA(0, 120) : ALPHA(0, 22));
}

void ui_sidebar_header(struct ui *ui, struct rect r, const char *label)
{
    if (ui->c)
        gfx_text_box(ui->c, r, &font_small, label, style.secondary, ALIGN_LEFT);
}

struct rect ui_group(struct ui *ui, int x, int y, int w, int rows, int row_h)
{
    struct rect r = {x, y, w, rows * row_h};
    if (ui->c) {
        gfx_round(ui->c, r, 10, style.group);
        gfx_round_frame(ui->c, r, 10, style.dark ? ALPHA(0xffffffffu, 16) : ALPHA(0, 18));
    }
    return r;
}

void ui_group_label(struct ui *ui, struct rect row, const char *label, const char *detail, bool last)
{
    if (!ui->c)
        return;
    gfx_text_box(ui->c, (struct rect){row.x + 14, row.y, row.w / 2, row.h}, &font_sans, label, style.text, ALIGN_LEFT);
    if (detail)
        gfx_text_box(ui->c, (struct rect){row.x + row.w / 2, row.y, row.w / 2 - 14, row.h}, &font_sans, detail,
                     style.secondary, ALIGN_RIGHT);
    if (!last)
        gfx_fill(ui->c, (struct rect){row.x + 14, row.y + row.h - 1, row.w - 28, 1}, style.separator);
}

void ui_settings_icon(struct canvas *c, int x, int y, int size, enum symbol_id sym, uint32_t color)
{
    struct rect r = {x, y, size, size};
    gfx_round_gradient(c, r, size * 2 / 9, color_shade(color, 30), color);
    int s = size * 5 / 8;
    gfx_symbol(c, x + (size - s) / 2, y + (size - s) / 2, sym, s, RGB(255, 255, 255));
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

static int wrap_centered(struct canvas *c, struct rect r, const struct ui_font *f, const char *s, uint32_t color)
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
        if (c) {
            int w = gfx_text_width_n(f, p, (size_t)(line_end - p));
            gfx_text_n(c, r.x + (r.w - w) / 2, y, f, p, (size_t)(line_end - p), color);
        }
        y += f->height + 2;
        p = line_end;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    return y - r.y;
}

#define MENU_PAD 6
#define MENU_ITEM_H 26
#define MENU_SEP_H 11
#define MENU_MAX 48

struct menu {
    struct menu_item items[MENU_MAX];
    int count;
    int hover;
    ui_pick_fn pick;
    struct window *owner;
    struct spring hl, fade;
    uint64_t tick;
};

static int menu_item_y(struct menu *m, int index)
{
    int y = MENU_PAD;
    for (int i = 0; i < index; i++)
        y += m->items[i].label[0] ? MENU_ITEM_H : MENU_SEP_H;
    return y;
}

static int menu_index_at(struct menu *m, int y)
{
    int top = MENU_PAD;
    for (int i = 0; i < m->count; i++) {
        int h = m->items[i].label[0] ? MENU_ITEM_H : MENU_SEP_H;
        if (y >= top && y < top + h)
            return m->items[i].label[0] && m->items[i].key >= 0 ? i : -1;
        top += h;
    }
    return -1;
}

static void menu_paint(struct window *w, struct canvas *c)
{
    struct menu *m = w->data;
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    if (m->fade.pos > 8) {
        struct rect row = {MENU_PAD, m->hl.pos, c->w - 2 * MENU_PAD, MENU_ITEM_H};
        gfx_round(c, row, 9, ALPHA(style.accent, CLAMP(m->fade.pos, 0, 1024) * 255 / 1024));
    }
    for (int i = 0; i < m->count; i++) {
        int y = menu_item_y(m, i);
        if (!m->items[i].label[0]) {
            gfx_fill(c, (struct rect){MENU_PAD + 10, y + MENU_SEP_H / 2, c->w - 2 * MENU_PAD - 20, 1}, style.separator);
            continue;
        }
        struct rect row = {MENU_PAD, y, c->w - 2 * MENU_PAD, MENU_ITEM_H};
        if (m->items[i].key == MENU_HEADER) {
            gfx_text_box(c, (struct rect){row.x + 12, row.y + 4, row.w - 24, row.h - 4}, &font_small, m->items[i].label,
                         style.secondary, ALIGN_LEFT);
            continue;
        }
        bool hot = i == m->hover && m->fade.pos > 512;
        bool disabled = m->items[i].key < 0;
        uint32_t ink = disabled ? style.tertiary : hot ? style.accent_text : style.text;
        int push = i == m->hover ? 2 : 0;
        gfx_text_box(c, (struct rect){row.x + 12 + push, row.y, row.w - 24, row.h}, &font_sans, m->items[i].label, ink,
                     ALIGN_LEFT);
        if (m->items[i].shortcut)
            gfx_text_box(c, (struct rect){row.x + 12, row.y, row.w - 24, row.h}, &font_sans, m->items[i].shortcut,
                         hot ? ALPHA(style.accent_text, 200) : style.tertiary, ALIGN_RIGHT);
    }
}

static void menu_event(struct window *w, struct gui_event *e)
{
    struct menu *m = w->data;
    int old = m->hover;
    int index = menu_index_at(m, e->y);
    if (e->type == EV_TIMER) {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - m->tick, 64u);
        m->tick = now;
        bool busy = spring_step(&m->hl, 700, 46, dt);
        busy |= spring_step(&m->fade, 600, 50, dt);
        if (busy)
            wm_invalidate(w);
        return;
    }
    if (e->type == EV_MOUSE_MOVE)
        m->hover = index;
    else if (e->type == EV_LEAVE)
        m->hover = -1;
    else if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
    else if (e->type == EV_KEY) {
        int key = KEY_BASE(e->key);
        if (key == KEY_ESC) {
            wm_destroy(w);
            return;
        }
        if (key == KEY_DOWN || key == KEY_UP) {
            int step = key == KEY_DOWN ? 1 : -1;
            int h = m->hover < 0 && step < 0 ? 0 : m->hover;
            for (int n = 0; n < m->count; n++) {
                h = (h + step + m->count) % m->count;
                if (m->items[h].label[0] && m->items[h].key >= 0)
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
        bool owned = owner != NULL;
        wm_destroy(w);
        if (pick && (!owned || wm_window_alive(owner)))
            pick(owner, index);
        return;
    }
    if (old != m->hover) {
        if (m->hover >= 0) {
            int y = menu_item_y(m, m->hover);
            if (m->fade.target == 0)
                spring_set(&m->hl, y);
            m->hl.target = y;
        }
        m->fade.target = m->hover >= 0 ? 1024 : 0;
        wm_invalidate(w);
    }
}

static void menu_destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops menu_ops = {menu_paint, menu_event, NULL, menu_destroy};

struct window *ui_menu_items(struct window *owner, int x, int y, const struct menu_item *items, int count,
                             ui_pick_fn pick)
{
    wm_close_popups();
    struct menu *m = kzalloc(sizeof(*m));
    if (!m)
        return NULL;
    m->count = MIN(count, MENU_MAX);
    memcpy(m->items, items, sizeof(*items) * (size_t)m->count);
    m->hover = -1;
    m->pick = pick;
    m->owner = owner;
    int width = 200, height = 2 * MENU_PAD;
    for (int i = 0; i < m->count; i++) {
        int w = gfx_text_width(&font_sans, items[i].label) + 40;
        if (items[i].shortcut)
            w += gfx_text_width(&font_sans, items[i].shortcut) + 30;
        width = MAX(width, w);
        height += items[i].label[0] ? MENU_ITEM_H : MENU_SEP_H;
    }
    if (owner) {
        struct rect cr = wm_client_rect(owner);
        x += cr.x;
        y += cr.y;
    }
    m->tick = hal_ticks_ms();
    struct window *w = wm_popup(x, y, width, height, &menu_ops, m, owner);
    if (!w)
        kfree(m);
    else {
        w->radius = 14;
        wm_set_timer(w, 16);
    }
    return w;
}

void ui_menu(struct window *owner, int x, int y, const char *const *items, int count, ui_pick_fn pick)
{
    struct menu_item list[MENU_MAX];
    count = MIN(count, MENU_MAX);
    for (int i = 0; i < count; i++)
        list[i] = (struct menu_item){items[i], 0, NULL};
    ui_menu_items(owner, x, y, list, count, pick);
}

struct dialog {
    struct ui_state st;
    char title[64];
    char text[512];
    struct ui_text input;
    bool has_input;
    ui_prompt_fn done;
    struct window *owner;
    enum icon_id icon;
    char buttons[3][16];
    int button_count;
    ui_pick_fn pick;
};

#define DIALOG_W 300

static void dialog_frame(struct ui *ui, void *data)
{
    struct dialog *d = data;
    struct rect area = {0, 0, ui->win->surface.w, ui->win->surface.h};
    if (ui->c)
        gfx_clear(ui->c, area, ALPHA(style.window, style.glass == GLASS_SOLID ? 255 : 200));
    int y = 22;
    if (ui->c)
        gfx_icon(ui->c, (area.w - 64) / 2, y, d->icon, 64);
    y += 76;
    y += wrap_centered(ui->c, (struct rect){20, y, area.w - 40, 200}, &font_bold, d->title, style.text) + 4;
    y += wrap_centered(ui->c, (struct rect){20, y, area.w - 40, 400}, &font_small, d->text, style.text) + 8;
    bool submit = false;
    if (d->has_input) {
        if (!ui->st->focus)
            ui->st->focus = ui->id + 1;
        submit = ui_textbox(ui, (struct rect){20, y, area.w - 40, 26}, &d->input);
    }
    if (d->button_count) {
        int n = d->button_count, bw = (area.w - 40 - 10 * (n - 1)) / n, chosen = -2;
        for (int i = 0; i < n; i++) {
            struct rect r = {20 + i * (bw + 10), area.h - 46, bw, 28};
            if (i == n - 1 ? ui_button_primary(ui, r, d->buttons[i]) : ui_button(ui, r, d->buttons[i]))
                chosen = i;
        }
        int key = ui_key(ui);
        if (key == KEY_ENTER || key == '\r')
            chosen = n - 1;
        else if (key == KEY_ESC)
            chosen = n > 1 ? 0 : n - 1;
        if (chosen >= 0) {
            struct window *w = ui->win;
            ui_pick_fn pick = d->pick;
            struct window *owner = d->owner;
            ui->win = NULL;
            wm_destroy(w);
            if (pick)
                pick(owner, chosen);
        }
        return;
    }
    bool two = d->has_input || d->done;
    int bw = two ? (area.w - 50) / 2 : area.w - 40;
    bool cancel = false;
    if (two && ui_button(ui, (struct rect){20, area.h - 46, bw, 28}, "Cancel"))
        cancel = true;
    if (ui_button_primary(ui, (struct rect){two ? 30 + bw : 20, area.h - 46, bw, 28}, "OK"))
        submit = true;
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
        if (submit && done && (!owner || wm_window_alive(owner)))
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

static struct dialog *open_dialog(struct window *owner, const char *title, const char *text, const char *initial,
                                  ui_prompt_fn done, enum icon_id icon)
{
    struct dialog *d = kzalloc(sizeof(*d));
    if (!d)
        return NULL;
    str_copy(d->title, title, sizeof(d->title));
    str_copy(d->text, text, sizeof(d->text));
    d->has_input = initial != NULL;
    if (initial)
        ui_text_set(&d->input, initial);
    d->done = done;
    d->owner = owner;
    d->icon = icon;
    int h = 22 + 76 + wrap_centered(NULL, (struct rect){0, 0, DIALOG_W - 40, 200}, &font_bold, title, 0) + 4 +
            wrap_centered(NULL, (struct rect){0, 0, DIALOG_W - 40, 400}, &font_small, text, 0) + 8 +
            (initial ? 40 : 6) + 52;
    if (owner)
        wm_focus(owner);
    struct window *w = wm_open(title, icon, DIALOG_W, h, &dialog_ops, d, WIN_DIALOG | WIN_GLASS);
    if (!w) {
        kfree(d);
        return NULL;
    }
    w->owner = owner;
    if (owner)
        w->app = owner->app;
    return d;
}

void gui_message(const char *title, const char *text)
{
    open_dialog(NULL, title, text, NULL, NULL, ICON_INFO);
}

bool ui_alert(struct window *owner, const char *title, const char *text, enum icon_id icon,
              const char *const *buttons, int count, ui_pick_fn pick)
{
    struct dialog *d = open_dialog(owner, title, text, NULL, NULL, icon);
    if (!d)
        return false;
    d->button_count = MIN(count, 3);
    for (int i = 0; i < d->button_count; i++)
        str_copy(d->buttons[i], buttons[i], sizeof(d->buttons[i]));
    d->pick = pick;
    return true;
}

void ui_confirm(struct window *owner, const char *title, const char *text, ui_prompt_fn done)
{
    open_dialog(owner, title, text, NULL, done, ICON_WARNING);
}

void ui_prompt(struct window *owner, const char *title, const char *label, const char *initial, ui_prompt_fn done)
{
    open_dialog(owner, title, label, initial ? initial : "", done, owner ? owner->icon : ICON_TEXT);
}

void ui_prompt_secret(struct window *owner, const char *title, const char *label, ui_prompt_fn done)
{
    struct dialog *d = open_dialog(owner, title, label, "", done, ICON_USER);
    if (d)
        d->input.password = true;
}
