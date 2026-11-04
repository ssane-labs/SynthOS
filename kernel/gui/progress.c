#include "internal.h"
#include <synth/hal.h>
#include <synth/string.h>

/* A small card in the top right corner for work that blocks the desktop (a download, an update): it keeps
   drawing its progress bar while the work runs. */

static struct window *card;
static char card_title[64], card_stage[96];
static int card_permille = -1;
static uint64_t card_drawn;

static void card_paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    gfx_symbol(c, 18, 18, SYM_DOWNLOAD, 22, style.accent);
    gfx_text_box(c, (struct rect){52, 12, c->w - 64, 18}, &font_bold, card_title, style.text, ALIGN_LEFT);
    gfx_text_box(c, (struct rect){52, 30, c->w - 64, 16}, &font_small, card_stage, style.secondary, ALIGN_LEFT);
    struct rect bar = {52, c->h - 18, c->w - 70, 5};
    gfx_round(c, bar, 2, style.separator);
    int fill = card_permille >= 0 ? bar.w * card_permille / 1000
                                  : bar.w / 6 + (int)(hal_ticks_ms() / 4 % (uint64_t)(bar.w / 2));
    gfx_round(c, (struct rect){bar.x, bar.y, MIN(fill, bar.w), bar.h}, 2, style.accent);
}

static void card_event(struct window *w, struct gui_event *e)
{
    UNUSED(w);
    if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
}

static void card_destroy(struct window *w)
{
    if (card == w)
        card = NULL;
}

static const struct window_ops card_ops = {card_paint, card_event, NULL, card_destroy};

static void card_redraw(bool now)
{
    uint64_t t = hal_ticks_ms();
    if (!card || (!now && t - card_drawn < 100))
        return;
    card_drawn = t;
    wm_invalidate(card);
    wm_animate_ghosts();
    wm_render();
}

void progress_update(void *ctx, const char *stage, size_t done, size_t total)
{
    UNUSED(ctx);
    UNUSED(stage);
    card_permille = total ? (int)((uint64_t)done * 1000 / total) : -1;
    card_redraw(false);
}

void progress_open(const char *title, const char *stage)
{
    str_copy(card_title, title, sizeof(card_title));
    str_copy(card_stage, stage, sizeof(card_stage));
    card_permille = -1;
    if (!card) {
        struct rect s = wm_screen_rect(0);
        card = wm_popup(s.x + s.w - 300, status_bottom() + 10, 288, 64, &card_ops, NULL, NULL);
        if (card) {
            card->radius = 20;
            card->flags |= WIN_PASSIVE;
        }
    }
    card_redraw(true);
}

void progress_close(void)
{
    if (card && wm_window_alive(card))
        wm_destroy(card);
    card = NULL;
}
