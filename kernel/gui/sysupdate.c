#include "internal.h"
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/net.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/update.h>

/* Software Update: Settings asks for a check or an install, the desktop loop runs it (a download blocks, so a small
   card keeps drawing its progress) and a notification tells the result. Once a day the desktop checks by itself. */

enum { ASK_NONE, ASK_CHECK, ASK_INSTALL };

static int asked;
static bool quiet;
static int state = SYSUPDATE_IDLE;
static struct update_status status;
static char note[160];
static bool auto_check = true;
static bool auto_done;
static uint64_t desktop_since;

static struct window *card;
static const char *card_title;
static const char *card_stage;
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

static void card_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    UNUSED(ctx);
    UNUSED(stage);
    card_permille = total ? (int)((uint64_t)done * 1000 / total) : -1;
    card_redraw(false);
}

static void card_open(const char *title, const char *stage)
{
    card_title = title;
    card_stage = stage;
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

static void card_close(void)
{
    if (card && wm_window_alive(card))
        wm_destroy(card);
    card = NULL;
}

static void day_stamp(char *out, size_t cap)
{
    struct rtc_time t;
    time_local(&t);
    ksnprintf(out, cap, "%04d-%02d-%02d", t.year, t.month, t.day);
}

static void run_check(void)
{
    if (!quiet)
        card_open("Software Update", "Looking for a newer SynthOS");
    state = SYSUPDATE_BUSY;
    int err = update_check(&status, quiet ? NULL : card_progress, NULL);
    card_close();
    str_copy(note, status.message, sizeof(note));
    state = err ? SYSUPDATE_FAILED : status.available ? SYSUPDATE_AVAILABLE : SYSUPDATE_CURRENT;
    if (!err) {
        char day[16];
        day_stamp(day, sizeof(day));
        app_state_set("update", "checked", day);
    }
    if (state == SYSUPDATE_AVAILABLE)
        gui_notify("Software Update", "A new SynthOS is ready. Open Settings > General > Software Update.");
    else if (!quiet)
        gui_notify("Software Update", note);
}

static void run_install(void)
{
    card_open("Software Update", "Looking for a newer SynthOS");
    state = SYSUPDATE_BUSY;
    int err = update_check(&status, card_progress, NULL);
    if (!err && !status.available) {
        card_close();
        str_copy(note, status.message, sizeof(note));
        state = SYSUPDATE_CURRENT;
        gui_notify("Software Update", note);
        return;
    }
    uint8_t *data = NULL;
    size_t size = 0;
    if (!err) {
        card_open("Software Update", "Downloading the update");
        err = update_download(&status, &data, &size, card_progress, NULL, note, sizeof(note));
    } else {
        str_copy(note, status.message, sizeof(note));
    }
    if (!err) {
        card_open("Software Update", "Installing, do not turn off the computer");
        err = update_install(data, size, note, sizeof(note));
    }
    kfree(data);
    card_close();
    state = err ? SYSUPDATE_FAILED : SYSUPDATE_INSTALLED;
    gui_notify(err ? "Update failed" : "Update installed", err ? note : "Restart the computer to use the new SynthOS.");
}

void sysupdate_request(bool install)
{
    asked = install ? ASK_INSTALL : ASK_CHECK;
    quiet = false;
}

void sysupdate_run(void)
{
    if (!desktop_since)
        desktop_since = hal_ticks_ms();
    if (!asked && auto_check && !auto_done && state == SYSUPDATE_IDLE && hal_ticks_ms() - desktop_since > 45000 &&
        !gui_locked()) {
        auto_done = true;
        char day[16];
        day_stamp(day, sizeof(day));
        if (net_interface_count() && net_interface(0)->configured && strcmp(app_state("update", "checked", ""), day)) {
            asked = ASK_CHECK;
            quiet = true;
        }
    }
    if (!asked || state == SYSUPDATE_BUSY)
        return;
    int op = asked;
    asked = ASK_NONE;
    if (op == ASK_INSTALL)
        run_install();
    else
        run_check();
    quiet = false;
}

int sysupdate_state(void)
{
    return state;
}

const char *sysupdate_note(void)
{
    return state == SYSUPDATE_IDLE ? "" : note;
}

static void get_auto(char *buf, size_t cap)
{
    str_copy(buf, auto_check ? "on" : "off", cap);
}

static int set_auto(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    auto_check = !strcmp(value, "on");
    return 0;
}

static const struct setting s_autoupdate = {"autoupdate", "on|off", "look for a system update once a day", get_auto,
                                            set_auto};
SETTING(s_autoupdate);
