#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

/* Window previews over the Dock, as on the Windows taskbar: rest the pointer on an app that has windows open and
   a picture of each one shows above it; a click brings that window forward, the cross closes it. */

#define PEEK_MAX 8
#define PEEK_W 200
#define PEEK_H 128
#define PEEK_HEAD 28
#define OPEN_MS 420
#define LEAVE_MS 260

struct peek {
    struct window *wins[PEEK_MAX];
    struct canvas thumbs[PEEK_MAX];
    int count, hover;
    bool close_hot;
};

static bool previews = true;
static const struct gui_app *want_app, *shown_app, *suppressed;
static struct rect want_icon;
static bool want_beside;
static uint64_t want_at, leave_at;
static struct window *peek_win;
static bool peek_hot;

static struct rect card_rect(int i)
{
    return (struct rect){10 + i * (PEEK_W + 8), 10, PEEK_W, PEEK_HEAD + PEEK_H};
}

static struct rect close_rect(int i)
{
    struct rect c = card_rect(i);
    return (struct rect){c.x + c.w - 26, c.y + 4, 20, 20};
}

static void peek_paint(struct window *w, struct canvas *c)
{
    struct peek *p = w->data;
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    for (int i = 0; i < p->count; i++) {
        struct rect card = card_rect(i);
        if (i == p->hover)
            gfx_round(c, card, 12, style.dark ? ALPHA(0xffffffffu, 34) : ALPHA(0, 22));
        struct window *win = p->wins[i];
        gfx_icon(c, card.x + 8, card.y + 6, win->icon, 16);
        gfx_text_box(c, (struct rect){card.x + 30, card.y + 4, card.w - 62, 20}, &font_sans, win->title, style.text,
                     ALIGN_LEFT);
        if (i == p->hover) {
            struct rect x = close_rect(i);
            if (p->close_hot)
                gfx_round(c, x, 5, RGB(232, 17, 35));
            gfx_symbol(c, x.x + 2, x.y + 2, SYM_CLOSE, 16, p->close_hot ? RGB(255, 255, 255) : style.text);
        }
        struct canvas *t = &p->thumbs[i];
        if (t->px) {
            struct rect area = {card.x + 8, card.y + PEEK_HEAD, card.w - 16, PEEK_H - 8};
            gfx_blit_alpha(c, area.x + (area.w - t->w) / 2, area.y + (area.h - t->h) / 2, t,
                           (struct rect){0, 0, t->w, t->h}, 255);
        }
    }
}

static void peek_close(void)
{
    if (peek_win && wm_window_alive(peek_win))
        wm_destroy(peek_win);
    peek_win = NULL;
    shown_app = NULL;
    peek_hot = false;
}

static int card_at(struct peek *p, int x, int y)
{
    for (int i = 0; i < p->count; i++)
        if (rect_contains(card_rect(i), x, y))
            return i;
    return -1;
}

static void peek_open(const struct gui_app *app);

static void peek_event(struct window *w, struct gui_event *e)
{
    struct peek *p = w->data;
    switch (e->type) {
    case EV_MOUSE_MOVE: {
        peek_hot = true;
        int h = card_at(p, e->x, e->y);
        bool ch = h >= 0 && rect_contains(close_rect(h), e->x, e->y);
        if (h != p->hover || ch != p->close_hot) {
            p->hover = h;
            p->close_hot = ch;
            wm_invalidate(w);
        }
        break;
    }
    case EV_LEAVE:
        peek_hot = false;
        leave_at = hal_ticks_ms() + LEAVE_MS;
        p->hover = -1;
        wm_invalidate(w);
        break;
    case EV_MOUSE_DOWN: {
        e->handled = true;
        int i = card_at(p, e->x, e->y);
        if (i < 0)
            break;
        struct window *target = p->wins[i];
        const struct gui_app *app = shown_app;
        if (rect_contains(close_rect(i), e->x, e->y)) {
            peek_close();
            wm_close(target);
            if (app && wm_app_window_count(app) > 0)
                peek_open(app);
            return;
        }
        peek_close();
        if (wm_window_alive(target)) {
            if (target->flags & WIN_MINIMIZED)
                wm_show(target);
            else
                wm_focus(target);
        }
        return;
    }
    case EV_KEY:
        if (KEY_BASE(e->key) == KEY_ESC)
            peek_close();
        break;
    default: break;
    }
}

static void peek_destroy(struct window *w)
{
    struct peek *p = w->data;
    for (int i = 0; i < p->count; i++)
        canvas_free(&p->thumbs[i]);
    kfree(p);
    if (peek_win == w) {
        peek_win = NULL;
        shown_app = NULL;
    }
}

static const struct window_ops peek_ops = {peek_paint, peek_event, NULL, peek_destroy};

static void peek_open(const struct gui_app *app)
{
    struct peek *p = kzalloc(sizeof(*p));
    if (!p)
        return;
    struct window *list[32];
    int n = wm_window_list(list, 32);
    for (int i = n - 1; i >= 0 && p->count < PEEK_MAX; i--) {
        if (list[i]->app != app)
            continue;
        int k = p->count;
        p->wins[k] = list[i];
        struct canvas full = {0};
        const struct canvas *src = NULL;
        if ((list[i]->flags & WIN_MINIMIZED) && list[i]->thumb.px)
            src = &list[i]->thumb;
        else if (wm_window_image(list[i], &full))
            src = &full;
        if (src) {
            int scale = MIN((PEEK_W - 16) * 1024 / MAX(src->w, 1), (PEEK_H - 8) * 1024 / MAX(src->h, 1));
            int tw = MAX(src->w * scale / 1024, 1), th = MAX(src->h * scale / 1024, 1);
            if (canvas_alloc(&p->thumbs[k], tw, th)) {
                memset(p->thumbs[k].px, 0, (size_t)tw * (size_t)th * 4);
                gfx_scale_smooth(&p->thumbs[k], (struct rect){0, 0, tw, th}, src, 255);
            }
        }
        canvas_free(&full);
        p->count++;
    }
    if (!p->count) {
        kfree(p);
        return;
    }
    p->hover = -1;
    int w = p->count * (PEEK_W + 8) + 12, h = PEEK_HEAD + PEEK_H + 20;
    struct rect s = wm_screen_rect(0);
    int x = want_beside ? want_icon.x + want_icon.w + 14 : want_icon.x + want_icon.w / 2 - w / 2;
    int y = want_beside ? want_icon.y + want_icon.h / 2 - h / 2 : want_icon.y - h - 16;
    x = CLAMP(x, s.x + 8, s.x + s.w - w - 8);
    y = CLAMP(y, s.y + menubar_height() + 4, s.y + s.h - h - 8);
    peek_win = wm_popup(x, y, w, h, &peek_ops, p, NULL);
    if (!peek_win) {
        for (int i = 0; i < p->count; i++)
            canvas_free(&p->thumbs[i]);
        kfree(p);
        return;
    }
    peek_win->radius = 18;
    peek_win->flags |= WIN_NOACTIVATE;
    shown_app = app;
}

/* A click on the Dock: the preview goes away until the pointer moves to another app. */
void dock_peek_cancel(void)
{
    peek_close();
    suppressed = want_app;
}

/* The Dock reports which running app the pointer rests on (NULL when none). */
void dock_peek_hover(const struct gui_app *app, struct rect icon, bool beside)
{
    if (!previews || app == want_app)
        return;
    if (app != suppressed)
        suppressed = NULL;
    uint64_t now = hal_ticks_ms();
    want_app = app;
    if (app) {
        want_icon = icon;
        want_beside = beside;
        want_at = now + (peek_win ? 60 : OPEN_MS);
    } else {
        leave_at = now + LEAVE_MS;
    }
}

void dock_peek_tick(void)
{
    if (peek_win && !wm_window_alive(peek_win)) {
        peek_win = NULL;
        shown_app = NULL;
    }
    uint64_t now = hal_ticks_ms();
    if (want_app && want_app != shown_app && want_app != suppressed && now >= want_at && !(hal_key_modifiers() & KEY_MOD_SUPER)) {
        peek_close();
        if (wm_app_window_count(want_app) > 0)
            peek_open(want_app);
        else
            want_app = NULL;
    } else if (!want_app && peek_win && !peek_hot && now >= leave_at) {
        peek_close();
    }
}

static void get_previews(char *buf, size_t cap)
{
    setting_flag_get(previews, buf, cap);
}

static int set_previews(struct shell *sh, const char *value)
{
    UNUSED(sh);
    int err = setting_flag(value, &previews);
    if (!previews)
        want_app = NULL;
    return err;
}

static const struct setting s_previews = {"dockpreviews", "on|off",
                                          "pictures of an app's open windows when the pointer rests on it in the Dock",
                                          get_previews, set_previews};
SETTING(s_previews);
