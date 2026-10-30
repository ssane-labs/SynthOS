#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "internal.h"

/* Notification Center: a click on the clock slides in the notifications of this session (banners hidden by Do Not
   Disturb land here too) above the calendar. */

#define NC_W 360
#define HISTORY 24
#define CARD_H 70

struct past_note {
    char title[64];
    char text[160];
    uint64_t when;
};

static struct past_note history[HISTORY];
static int history_count, unseen;
static struct window *center;
static struct ui_state center_st;
static int center_scroll;

void notify_record(const char *title, const char *text)
{
    if (history_count == HISTORY)
        memmove(history, history + 1, sizeof(history[0]) * (HISTORY - 1)), history_count--;
    struct past_note *n = &history[history_count++];
    str_copy(n->title, title, sizeof(n->title));
    str_copy(n->text, text, sizeof(n->text));
    n->when = hal_ticks_ms();
    unseen++;
    if (center && wm_window_alive(center))
        wm_invalidate(center);
}

int notify_unseen(void)
{
    return unseen;
}

static void ago(uint64_t when, char *buf, size_t cap)
{
    uint64_t s = (hal_ticks_ms() - when) / 1000;
    if (s < 60)
        str_copy(buf, "now", cap);
    else if (s < 3600)
        ksnprintf(buf, cap, "%um ago", (unsigned)(s / 60));
    else
        ksnprintf(buf, cap, "%uh ago", (unsigned)(s / 3600));
}

static void center_frame(struct ui *ui, void *data)
{
    UNUSED(data);
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, 0);
    uint32_t card = style.dark ? ALPHA(0xffffffffu, 22) : ALPHA(0xffffffffu, 150);
    ui_label(ui, (struct rect){20, 12, 200, 24}, "Notifications", &font_title, style.text, ALIGN_LEFT);
    if (history_count && ui_button(ui, (struct rect){W - 98, 12, 80, 24}, "Clear All")) {
        history_count = 0;
        center_scroll = 0;
    }
    int cal_h = 66 + 6 * 30 + 12;
    struct rect list = {0, 46, W, H - 46 - cal_h - 20};
    int total = MAX(history_count, 1) * (CARD_H + 8);
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(list, ui->e->x, ui->e->y)) {
        center_scroll = CLAMP(center_scroll + ui->e->wheel * 30, 0, MAX(0, total - list.h));
        ui->changed = true;
        ui->consumed = true;
    }
    struct canvas *saved = ui->c, clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, list);
        ui->c = &clipped;
    }
    int y = list.y - center_scroll;
    if (!history_count && ui->c) {
        struct rect r = {12, y, W - 24, CARD_H};
        gfx_round(ui->c, r, 16, card);
        gfx_text_box(ui->c, r, &font_sans, "No Notifications", style.secondary, ALIGN_CENTER);
    }
    for (int i = history_count - 1; i >= 0; i--, y += CARD_H + 8) {
        struct past_note *n = &history[i];
        struct rect r = {12, y, W - 24, CARD_H};
        if (ui->c && r.y + r.h > list.y && r.y < list.y + list.h) {
            char when[16];
            ago(n->when, when, sizeof(when));
            gfx_round(ui->c, r, 16, card);
            gfx_icon(ui->c, r.x + 12, r.y + 17, ICON_LOGO, 36);
            gfx_text_box(ui->c, (struct rect){r.x + 58, r.y + 10, r.w - 130, 18}, &font_bold, n->title, style.text,
                         ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){r.x + r.w - 72, r.y + 10, 60, 18}, &font_small, when, style.secondary,
                         ALIGN_RIGHT);
            ui_wrap_text(ui->c, (struct rect){r.x + 58, r.y + 30, r.w - 70, 36}, &font_small, n->text,
                         style.text);
        }
        struct rect x = {r.x + r.w - 26, r.y + 36, 20, 20};
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, x) && rect_contains(list, ui->e->x, ui->e->y)) {
            ui->consumed = true;
            memmove(history + i, history + i + 1, sizeof(history[0]) * (size_t)(history_count - i - 1));
            history_count--;
            break;
        }
        if (ui->c && ui_hover(ui, r))
            gfx_symbol(ui->c, x.x + 2, x.y + 2, SYM_CLOSE, 16, style.secondary);
    }
    if (saved)
        ui->c = saved;
    struct rect cal = {12, H - cal_h - 12, W - 24, cal_h};
    if (ui->c) {
        gfx_round(ui->c, cal, 18, card);
        calendar_draw(ui->c, cal);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN)
        ui->consumed = true;
}

static void center_paint(struct window *w, struct canvas *c)
{
    ui_run(w, &center_st, c, NULL, center_frame, NULL);
}

static void center_event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_KEY && KEY_BASE(e->key) == KEY_ESC) {
        wm_destroy(w);
        return;
    }
    if (e->type == EV_TIMER) {
        wm_invalidate(w);
        return;
    }
    ui_run(w, &center_st, NULL, e, center_frame, NULL);
}

static void center_destroy(struct window *w)
{
    if (center == w)
        center = NULL;
}

static const struct window_ops center_ops = {center_paint, center_event, NULL, center_destroy};

void notification_center_toggle(void)
{
    if (center && wm_window_alive(center)) {
        wm_destroy(center);
        return;
    }
    wm_close_popups();
    struct rect s = wm_screen_rect(0);
    int top = status_bottom() + 6;
    int h = MIN(s.y + s.h - top - 12, 46 + 5 * (CARD_H + 8) + 66 + 6 * 30 + 12 + 20);
    center_scroll = 0;
    unseen = 0;
    center = wm_popup(s.x + s.w - NC_W - 8, top, NC_W, h, &center_ops, NULL, NULL);
    if (!center)
        return;
    center->radius = 24;
    wm_set_timer(center, 30000);
    chrome_changed();
}
