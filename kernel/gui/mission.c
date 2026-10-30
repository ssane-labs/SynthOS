#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

/* Mission Control (F3, Ctrl+Up, a hot corner): every open window shrinks into a grid over the wallpaper; a click
   brings one forward. And the window switcher of Alt+Tab and Super+Tab: cards with a picture of each window,
   Tab moves along them and letting go of the modifier switches. */

#define MC_MAX 24
#define MC_MS 280

struct mission {
    struct window *wins[MC_MAX];
    struct canvas snaps[MC_MAX], fitted[MC_MAX];
    struct rect from[MC_MAX], to[MC_MAX];
    int count, hover;
    struct canvas back;
    uint64_t start;
    bool closing;
    struct window *pick;
    int pressed, strip_hover;
    bool dragging;
    int px, py, mx, my;
};

/* The row of desktops at the top: a picture of each with its windows outlined, then the + button. */
#define STRIP_H 132
#define STRIP_PLUS 1000
#define STRIP_REMOVE 2000

static struct rect space_rect(int i, int W, int H)
{
    int n = wm_space_count(), tw = MIN(190, (W - 160) / MAX(n, 1) - 24), th = tw * H / MAX(W, 1);
    int total = n * (tw + 24) - 24 + 70;
    return (struct rect){(W - total) / 2 + i * (tw + 24), 22, tw, th};
}

static struct rect plus_rect(int W, int H)
{
    struct rect last = space_rect(wm_space_count() - 1, W, H);
    return (struct rect){last.x + last.w + 26, last.y + last.h / 2 - 22, 44, 44};
}

static int strip_at(int x, int y, int W, int H)
{
    if (wm_space_count() < MAX_SPACES && rect_contains(plus_rect(W, H), x, y))
        return STRIP_PLUS;
    for (int i = 0; i < wm_space_count(); i++) {
        struct rect r = space_rect(i, W, H);
        int dx = x - r.x, dy = y - r.y;
        if (wm_space_count() > 1 && dx * dx + dy * dy <= 13 * 13)
            return STRIP_REMOVE + i;
        if (rect_contains(rect_inset(r, -6), x, y))
            return i;
    }
    return -1;
}

struct outline_ctx {
    struct canvas *c;
    struct rect thumb;
    int space, sw, sh;
};

static void outline_window(struct window *w, void *ctx)
{
    struct outline_ctx *o = ctx;
    if (w->space != o->space || (w->flags & (WIN_POPUP | WIN_OVERLAY | WIN_DIALOG | WIN_BARE | WIN_HIDDEN |
                                              WIN_MINIMIZED)))
        return;
    struct rect f = w->frame, t = o->thumb;
    struct rect r = {t.x + f.x * t.w / MAX(o->sw, 1), t.y + f.y * t.h / MAX(o->sh, 1), f.w * t.w / MAX(o->sw, 1),
                     f.h * t.h / MAX(o->sh, 1)};
    struct canvas k = *o->c;
    k.clip = rect_intersect(o->c->clip, t);
    gfx_round(&k, r, 3, ALPHA(0xffffffffu, 120));
    gfx_round(&k, (struct rect){r.x, r.y, r.w, MIN(r.h, 4)}, 2, ALPHA(0xffffffffu, 200));
}

static void paint_strip(struct canvas *c, struct mission *m, int alpha)
{
    int W = c->w, H = c->h;
    gfx_fill(c, (struct rect){0, 0, W, STRIP_H}, ALPHA(0, alpha * 60 / 255));
    for (int i = 0; i < wm_space_count(); i++) {
        struct rect r = space_rect(i, W, H);
        gfx_scale_fast(c, r, &m->back, alpha);
        struct outline_ctx o = {c, r, i, W, H};
        wm_foreach(outline_window, &o);
        bool cur = i == wm_space_current(), hot = m->strip_hover == i || m->strip_hover == STRIP_REMOVE + i;
        gfx_round_frame(c, rect_inset(r, -2), 6, cur ? style.accent : ALPHA(0xffffffffu, hot ? 160 : 60));
        if (cur)
            gfx_round_frame(c, rect_inset(r, -3), 7, style.accent);
        char label[24];
        ksnprintf(label, sizeof(label), "Desktop %d", i + 1);
        gfx_text_box(c, (struct rect){r.x, r.y + r.h + 6, r.w, 18}, &font_bold, label, ALPHA(0xffffffffu, alpha),
                     ALIGN_CENTER);
        if (hot && wm_space_count() > 1) {
            gfx_circle(c, r.x, r.y, 11, RGB(60, 60, 66));
            gfx_symbol(c, r.x - 8, r.y - 8, SYM_CLOSE, 16, RGB(255, 255, 255));
        }
    }
    if (wm_space_count() < MAX_SPACES) {
        struct rect p = plus_rect(W, H);
        gfx_circle(c, p.x + 22, p.y + 22, 22, ALPHA(0xffffffffu, m->strip_hover == STRIP_PLUS ? 90 : 50));
        gfx_symbol(c, p.x + 14, p.y + 14, SYM_PLUS, 16, RGB(255, 255, 255));
    }
}

static struct window *mission_win;

static int ease(uint64_t start, int ms)
{
    int t = (int)MIN((hal_ticks_ms() - start) * 1024 / (uint64_t)MAX(ms, 1), 1024u);
    int u = 1024 - t;
    return 1024 - (int)((int64_t)u * u / 1024 * u / 1024);
}

static struct rect lerp(struct rect a, struct rect b, int t)
{
    return (struct rect){a.x + (b.x - a.x) * t / 1024, a.y + (b.y - a.y) * t / 1024, a.w + (b.w - a.w) * t / 1024,
                         a.h + (b.h - a.h) * t / 1024};
}

static void layout(struct mission *m, struct rect s)
{
    struct rect area = {s.x + 48, s.y + STRIP_H + 20, s.w - 96, s.h - STRIP_H - 110};
    int n = m->count, cols = 1;
    while (cols * cols * area.h < n * area.w * 3 / 4 && cols < n)
        cols++;
    int rows = (n + cols - 1) / cols;
    int cw = area.w / cols, ch = area.h / MAX(rows, 1);
    for (int i = 0; i < n; i++) {
        struct rect f = m->wins[i]->frame;
        int row = i / cols, in_row = MIN(cols, n - row * cols), col = i % cols;
        int box_w = cw - 36, box_h = ch - 46;
        int scale = MIN(840, MIN(box_w * 1024 / MAX(f.w, 1), box_h * 1024 / MAX(f.h, 1)));
        int w = f.w * scale / 1024, h = f.h * scale / 1024;
        int x0 = area.x + (area.w - in_row * cw) / 2;
        m->to[i] = (struct rect){x0 + col * cw + (cw - w) / 2 - s.x, area.y + row * ch + (ch - 28 - h) / 2 - s.y, w, h};
        m->from[i] = (struct rect){f.x - s.x, f.y - s.y, f.w, f.h};
    }
}

static void mission_close(struct window *w, struct mission *m, struct window *pick)
{
    if (m->closing)
        return;
    m->closing = true;
    m->pick = pick;
    m->start = hal_ticks_ms();
    wm_invalidate(w);
}

static int thumb_at(struct mission *m, int x, int y)
{
    for (int i = m->count - 1; i >= 0; i--)
        if (rect_contains(rect_inset(m->to[i], -8), x, y))
            return i;
    return -1;
}

static void mission_paint(struct window *w, struct canvas *c)
{
    struct mission *m = w->data;
    int t = ease(m->start, MC_MS);
    if (m->closing)
        t = 1024 - t;
    gfx_blit(c, 0, 0, &m->back, (struct rect){0, 0, c->w, c->h});
    gfx_fill(c, (struct rect){0, 0, c->w, c->h}, ALPHA(0, 70 * t / 1024));
    paint_strip(c, m, 255 * t / 1024);
    bool settled = t == 1024;
    for (int i = 0; i < m->count; i++) {
        if (!m->snaps[i].px || (m->dragging && i == m->pressed))
            continue;
        struct rect r = lerp(m->from[i], m->to[i], t);
        if (settled) {
            if (!m->fitted[i].px && canvas_alloc(&m->fitted[i], r.w, r.h)) {
                memset(m->fitted[i].px, 0, (size_t)r.w * (size_t)r.h * 4);
                gfx_scale_smooth(&m->fitted[i], (struct rect){0, 0, r.w, r.h}, &m->snaps[i], 255);
            }
            gfx_drop_shadow(c, r, 10, 18, 70, 6);
            if (m->fitted[i].px)
                gfx_blit_alpha(c, r.x, r.y, &m->fitted[i], (struct rect){0, 0, r.w, r.h}, 255);
            if (i == m->hover) {
                gfx_round_frame(c, rect_inset(r, -3), 12, style.accent);
                gfx_round_frame(c, rect_inset(r, -4), 13, style.accent);
            }
            const char *title = m->wins[i]->title;
            int tw = MIN(gfx_text_width(&font_bold, title) + 24, r.w + 40);
            struct rect pill = {r.x + (r.w - tw) / 2, r.y + r.h + 10, tw, 24};
            if (i == m->hover || m->count <= 6) {
                gfx_round(c, pill, 12, ALPHA(0, i == m->hover ? 170 : 110));
                gfx_text_box(c, rect_inset(pill, 6), &font_bold, title, RGB(255, 255, 255), ALIGN_CENTER);
            }
        } else {
            gfx_scale_fast(c, r, &m->snaps[i], 255);
        }
    }
    if (!m->count && settled)
        gfx_text_box(c, (struct rect){0, c->h / 2 - 20, c->w, 40}, &font_title, "No Open Windows",
                     RGB(255, 255, 255), ALIGN_CENTER);
    if (m->dragging && m->pressed >= 0 && m->fitted[m->pressed].px) {
        struct canvas *f = &m->fitted[m->pressed];
        int w = f->w * 6 / 10, h = f->h * 6 / 10;
        gfx_scale(c, (struct rect){m->mx - w / 2, m->my - h / 2, w, h}, f, 220);
    }
}

static void mission_event(struct window *w, struct gui_event *e)
{
    struct mission *m = w->data;
    switch (e->type) {
    case EV_TIMER:
        if (m->closing && hal_ticks_ms() - m->start >= MC_MS) {
            struct window *pick = m->pick;
            wm_destroy(w);
            if (pick && wm_window_alive(pick)) {
                if (pick->flags & WIN_MINIMIZED)
                    wm_show(pick);
                else
                    wm_focus(pick);
            }
            return;
        }
        if (hal_ticks_ms() - m->start < MC_MS + 40)
            wm_invalidate(w);
        break;
    case EV_MOUSE_MOVE: {
        int h = m->closing ? -1 : thumb_at(m, e->x, e->y);
        int sh = m->closing ? -1 : strip_at(e->x, e->y, w->surface.w, w->surface.h);
        m->mx = e->x;
        m->my = e->y;
        if (m->pressed >= 0 && !m->dragging && (ABS(e->x - m->px) > 6 || ABS(e->y - m->py) > 6))
            m->dragging = true;
        if (h != m->hover || sh != m->strip_hover || m->dragging) {
            m->hover = h;
            m->strip_hover = sh;
            wm_invalidate(w);
        }
        break;
    }
    case EV_MOUSE_DOWN: {
        e->handled = true;
        if (m->closing)
            break;
        int sh = strip_at(e->x, e->y, w->surface.w, w->surface.h);
        if (sh == STRIP_PLUS) {
            wm_space_add();
            wm_invalidate(w);
        } else if (sh >= STRIP_REMOVE) {
            wm_destroy(w);
            wm_space_remove(sh - STRIP_REMOVE);
            mission_toggle();
        } else if (sh >= 0) {
            wm_destroy(w);
            wm_space_switch(sh);
        } else {
            m->pressed = thumb_at(m, e->x, e->y);
            m->px = m->mx = e->x;
            m->py = m->my = e->y;
            m->dragging = false;
            if (m->pressed < 0)
                mission_close(w, m, NULL);
        }
        return;
    }
    case EV_MOUSE_UP: {
        int i = m->pressed;
        bool dragged = m->dragging;
        m->pressed = -1;
        m->dragging = false;
        if (i < 0 || m->closing)
            break;
        if (!dragged) {
            mission_close(w, m, m->wins[i]);
            break;
        }
        int sh = strip_at(e->x, e->y, w->surface.w, w->surface.h);
        if (sh >= 0 && sh < MAX_SPACES && sh != wm_space_current()) {
            wm_window_to_space(m->wins[i], sh);
            canvas_free(&m->snaps[i]);
            canvas_free(&m->fitted[i]);
            for (int k = i; k + 1 < m->count; k++) {
                m->wins[k] = m->wins[k + 1];
                m->snaps[k] = m->snaps[k + 1];
                m->fitted[k] = m->fitted[k + 1];
            }
            m->count--;
            m->snaps[m->count] = m->fitted[m->count] = (struct canvas){0};
            for (int k = 0; k < m->count; k++)
                canvas_free(&m->fitted[k]);
            layout(m, wm_screen_rect(0));
            for (int k = 0; k < m->count; k++)
                m->from[k] = m->to[k];
        }
        wm_invalidate(w);
        break;
    }
    case EV_KEY: {
        int k = KEY_BASE(e->key);
        if (k == KEY_ESC || k == KEY_F3 || (k == KEY_UP && (e->key & KEY_MOD_CTRL)))
            mission_close(w, m, NULL);
        else if (k == KEY_ENTER && m->hover >= 0)
            mission_close(w, m, m->wins[m->hover]);
        else if ((k == KEY_RIGHT || k == KEY_TAB) && m->count) {
            m->hover = (m->hover + 1) % m->count;
            wm_invalidate(w);
        } else if (k == KEY_LEFT && m->count) {
            m->hover = (m->hover + m->count - 1) % m->count;
            wm_invalidate(w);
        }
        break;
    }
    default: break;
    }
}

static void mission_destroy(struct window *w)
{
    struct mission *m = w->data;
    for (int i = 0; i < m->count; i++) {
        canvas_free(&m->snaps[i]);
        canvas_free(&m->fitted[i]);
    }
    canvas_free(&m->back);
    kfree(m);
    if (mission_win == w)
        mission_win = NULL;
}

static const struct window_ops mission_ops = {mission_paint, mission_event, NULL, mission_destroy};

void mission_toggle(void)
{
    if (mission_win && wm_window_alive(mission_win)) {
        mission_close(mission_win, mission_win->data, NULL);
        return;
    }
    wm_close_popups();
    struct mission *m = kzalloc(sizeof(*m));
    if (!m)
        return;
    struct rect s = wm_screen_rect(0);
    struct window *list[MC_MAX];
    int n = wm_window_list(list, MC_MAX);
    for (int i = 0; i < n; i++) {
        if (list[i]->flags & WIN_MINIMIZED)
            continue;
        if (wm_window_image(list[i], &m->snaps[m->count]))
            m->wins[m->count++] = list[i];
    }
    if (!canvas_alloc(&m->back, s.w, s.h)) {
        for (int i = 0; i < m->count; i++)
            canvas_free(&m->snaps[i]);
        kfree(m);
        return;
    }
    const struct canvas *wp = desktop_wallpaper(0);
    if (wp)
        gfx_scale(&m->back, (struct rect){0, 0, s.w, s.h}, wp, 255);
    else
        gfx_clear(&m->back, (struct rect){0, 0, s.w, s.h}, RGB(40, 40, 46));
    layout(m, s);
    m->hover = m->pressed = m->strip_hover = -1;
    m->start = hal_ticks_ms();
    mission_win = wm_popup(s.x, s.y, s.w, s.h, &mission_ops, m, NULL);
    if (!mission_win) {
        m->count = 0;
        canvas_free(&m->back);
        kfree(m);
        return;
    }
    mission_win->flags |= WIN_OVERLAY | WIN_NOSHADOW;
    mission_win->radius = 0;
    wm_move(mission_win, s.x, s.y);
    wm_set_timer(mission_win, 16);
}

bool mission_active(void)
{
    return mission_win && wm_window_alive(mission_win);
}

/* ---- the window switcher */

#define SW_MAX 12
#define CARD_W 196
#define CARD_H 150

struct switcher {
    struct window *wins[SW_MAX];
    struct canvas thumbs[SW_MAX];
    int count, sel, held;
};

static struct window *switch_win;

static void switch_commit(struct window *w, struct switcher *sw)
{
    struct window *pick = sw->sel >= 0 && sw->sel < sw->count ? sw->wins[sw->sel] : NULL;
    wm_destroy(w);
    if (pick && wm_window_alive(pick)) {
        if (pick->flags & WIN_MINIMIZED)
            wm_show(pick);
        else
            wm_focus(pick);
    }
}

static void switch_paint(struct window *w, struct canvas *c)
{
    struct switcher *sw = w->data;
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    for (int i = 0; i < sw->count; i++) {
        struct rect card = {16 + i * (CARD_W + 8), 16, CARD_W, CARD_H};
        if (i == sw->sel)
            gfx_round(c, card, 14, style.dark ? ALPHA(0xffffffffu, 46) : ALPHA(0, 30));
        struct rect pic = {card.x + 10, card.y + 10, card.w - 20, card.h - 46};
        struct canvas *t = &sw->thumbs[i];
        if (t->px) {
            int scale = MIN(pic.w * 1024 / MAX(t->w, 1), pic.h * 1024 / MAX(t->h, 1));
            int tw = t->w * scale / 1024, th = t->h * scale / 1024;
            gfx_blit_alpha(c, pic.x + (pic.w - tw) / 2, pic.y + (pic.h - th) / 2, t, (struct rect){0, 0, tw, th},
                           255);
        }
        struct window *win = sw->wins[i];
        gfx_icon(c, card.x + 10, card.y + card.h - 32, win->icon, 24);
        gfx_text_box(c, (struct rect){card.x + 40, card.y + card.h - 32, card.w - 48, 24},
                     i == sw->sel ? &font_bold : &font_sans, win->title, style.text, ALIGN_LEFT);
    }
}

static void switch_step(struct switcher *sw, int dir)
{
    if (sw->count)
        sw->sel = (sw->sel + dir + sw->count) % sw->count;
}

static void switch_event(struct window *w, struct gui_event *e)
{
    struct switcher *sw = w->data;
    if (e->type == EV_TIMER) {
        if (!(hal_key_modifiers() & sw->held))
            switch_commit(w, sw);
        return;
    }
    if (e->type == EV_KEY) {
        int k = KEY_BASE(e->key);
        if (k == KEY_ESC) {
            wm_destroy(w);
        } else if (k == KEY_ENTER) {
            switch_commit(w, sw);
        } else if (k == KEY_TAB || k == KEY_RIGHT || k == KEY_LEFT) {
            switch_step(sw, k == KEY_LEFT || (e->key & KEY_MOD_SHIFT) ? -1 : 1);
            wm_invalidate(w);
        }
        return;
    }
    if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
        int i = (e->x - 16) / (CARD_W + 8);
        if (e->x >= 16 && i >= 0 && i < sw->count) {
            sw->sel = i;
            switch_commit(w, sw);
        }
    }
}

static void switch_destroy(struct window *w)
{
    struct switcher *sw = w->data;
    for (int i = 0; i < sw->count; i++)
        canvas_free(&sw->thumbs[i]);
    kfree(sw);
    if (switch_win == w)
        switch_win = NULL;
}

static const struct window_ops switch_ops = {switch_paint, switch_event, NULL, switch_destroy};

/* held is the modifier (KEY_MOD_ALT or KEY_MOD_SUPER) whose release picks the window. */
void switcher_next(int held, bool back)
{
    if (switch_win && wm_window_alive(switch_win)) {
        struct switcher *sw = switch_win->data;
        switch_step(sw, back ? -1 : 1);
        wm_invalidate(switch_win);
        return;
    }
    struct window *list[SW_MAX];
    int n = wm_window_list(list, SW_MAX);
    if (n < 2) {
        wm_cycle();
        return;
    }
    struct switcher *sw = kzalloc(sizeof(*sw));
    if (!sw)
        return;
    struct window *focused = wm_focused();
    for (int i = n - 1; i >= 0; i--) {
        if (list[i] == focused && sw->count) {
            memmove(sw->wins + 1, sw->wins, sizeof(sw->wins[0]) * (size_t)sw->count);
            sw->wins[0] = list[i];
        } else {
            sw->wins[sw->count] = list[i];
        }
        sw->count++;
    }
    for (int i = 0; i < sw->count; i++) {
        struct canvas full;
        if (!wm_window_image(sw->wins[i], &full))
            continue;
        int scale = MIN((CARD_W - 20) * 1024 / MAX(full.w, 1), (CARD_H - 46) * 1024 / MAX(full.h, 1));
        int tw = MAX(full.w * scale / 1024, 1), th = MAX(full.h * scale / 1024, 1);
        if (canvas_alloc(&sw->thumbs[i], tw, th)) {
            memset(sw->thumbs[i].px, 0, (size_t)tw * (size_t)th * 4);
            gfx_scale_smooth(&sw->thumbs[i], (struct rect){0, 0, tw, th}, &full, 255);
        }
        canvas_free(&full);
    }
    sw->sel = back ? sw->count - 1 : 1;
    sw->held = held;
    struct rect s = wm_screen_rect(0);
    int w = MIN(sw->count * (CARD_W + 8) + 24, s.w - 40), h = CARD_H + 32;
    switch_win = wm_popup(s.x + (s.w - w) / 2, s.y + (s.h - h) / 2, w, h, &switch_ops, sw, NULL);
    if (!switch_win) {
        for (int i = 0; i < sw->count; i++)
            canvas_free(&sw->thumbs[i]);
        kfree(sw);
        return;
    }
    switch_win->radius = 24;
    wm_set_timer(switch_win, 30);
}

/* ---- hot corners */

enum corner_action {
    HC_OFF,
    HC_MISSION,
    HC_DESKTOP,
    HC_LAUNCHPAD,
    HC_NOTIFY,
    HC_LOCK,
    HC_SHOT,
    HC_COUNT,
};

static const char *const action_names[HC_COUNT] = {"off",           "mission", "desktop",   "launchpad",
                                                   "notifications", "lock",    "screenshot"};
static int corners[4];
static int in_corner = -1;
static bool fired;
static uint64_t corner_since;

void hotcorner_tick(void)
{
    if (!gui_active() || gui_locked())
        return;
    struct rect s = wm_screen_rect(0);
    int x = mouse_x - s.x, y = mouse_y - s.y, c = -1;
    bool left = x <= 1, right = x >= s.w - 2, top = y <= 1, bottom = y >= s.h - 2;
    if (top && left)
        c = 0;
    else if (top && right)
        c = 1;
    else if (bottom && left)
        c = 2;
    else if (bottom && right)
        c = 3;
    uint64_t now = hal_ticks_ms();
    if (c != in_corner) {
        bool away = x > 24 && y > 24 && x < s.w - 24 && y < s.h - 24;
        if (c >= 0 || away) {
            in_corner = c;
            fired = false;
            corner_since = now;
        }
        return;
    }
    if (c < 0 || fired || !corners[c] || wm_mouse_buttons() || now - corner_since < 180)
        return;
    fired = true;
    switch (corners[c]) {
    case HC_MISSION: mission_toggle(); break;
    case HC_DESKTOP: desktop_show_toggle(); break;
    case HC_LAUNCHPAD: launchpad_toggle(); break;
    case HC_NOTIFY: notification_center_toggle(); break;
    case HC_LOCK: gui_lock(); break;
    case HC_SHOT: screenshot_start(SHOT_PANEL); break;
    }
}

static void corner_get(int i, char *buf, size_t cap)
{
    str_copy(buf, action_names[corners[i]], cap);
}

static int corner_set(int i, const char *value)
{
    for (int k = 0; k < HC_COUNT; k++)
        if (!strcmp(value, action_names[k])) {
            corners[i] = k;
            return 0;
        }
    return E_INVAL;
}

#define CORNER(n, i, name, help)                                                                                    \
    static void get_##n(char *buf, size_t cap) { corner_get(i, buf, cap); }                                        \
    static int set_##n(struct shell *sh, const char *value)                                                        \
    {                                                                                                               \
        UNUSED(sh);                                                                                                 \
        return corner_set(i, value);                                                                                \
    }                                                                                                               \
    static const struct setting s_##n = {name, "off|mission|desktop|launchpad|notifications|lock|screenshot", help, \
                                         get_##n, set_##n};                                                         \
    SETTING(s_##n)

CORNER(tl, 0, "cornertl", "what the top left hot corner does");
CORNER(tr, 1, "cornertr", "what the top right hot corner does");
CORNER(bl, 2, "cornerbl", "what the bottom left hot corner does");
CORNER(br, 3, "cornerbr", "what the bottom right hot corner does");
