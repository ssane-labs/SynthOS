#include <synth/bundle.h>
#include <synth/config.h>
#include <synth/exec.h>
#include <synth/shell.h>
#include <synth/net.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/omaha.h>
#include <synth/path.h>
#include <synth/string.h>

#include "../internal.h"

/* Chrome is the browser of SynthOS: this app starts the real Google Chrome, and the first time, when it is not on
   the disk yet, fetches it from Google's update server (the offline installer's program is unpacked directly). */

static struct canvas picture;
static bool picture_tried;

static void load_picture(void)
{
    char exe[PATH_MAX_LEN];
    if (picture.px || picture_tried || !chrome_path(exe, sizeof(exe)))
        return;
    picture_tried = true;
    gui_program_icon(exe, 64, &picture);
}

static void launch(const char *arg);

static const struct gui_app chrome_app = {"Chrome", "Google's web browser", ICON_BROWSER, launch, ".html;.htm;.xhtml;.url",
                                          &picture};
GUI_APP(chrome_app);

const struct gui_app *chrome_app_for(const char *path)
{
    if (str_casecmp(path_basename(path), "chrome.exe"))
        return NULL;
    load_picture();
    return &chrome_app;
}

/* The window shown until Chrome is installed: one sentence, one button, then a progress bar. */

enum stage {
    ST_OFFER,
    ST_WORKING,
    ST_FAILED,
};

static struct {
    struct window *win;
    enum stage stage;
    bool pending, cancel, quiet;
    char status[160], arg[PATH_MAX_LEN];
    const char *step;
    uint64_t done, total;
    uint64_t drawn;
    struct spring bar;
} job;

#define WIN_W 460
#define WIN_H 250

static struct rect button_rect(void)
{
    return (struct rect){(WIN_W - 180) / 2, WIN_H - 62, 180, 36};
}

static void paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, style.content);
    gfx_icon(c, (c->w - 64) / 2, 18, ICON_BROWSER, 64);
    gfx_text_box(c, (struct rect){0, 92, c->w, 26}, &font_title, "Google Chrome", style.text, ALIGN_CENTER);
    const char *line = job.stage == ST_FAILED    ? job.status
                       : job.stage == ST_WORKING ? job.status
                                                 : "Chrome is not on this computer yet. SynthOS gets it from Google "
                                                   "(about 420 MB) and opens it.";
    ui_wrap_text(c, (struct rect){36, 124, c->w - 72, 44}, &font_sans, line,
                 job.stage == ST_FAILED ? style.error : style.secondary);
    if (job.stage == ST_WORKING) {
        struct rect bar = {48, WIN_H - 52, WIN_W - 96, 8};
        gfx_round(c, bar, 4, style.separator);
        int fill = job.total ? bar.w * CLAMP(job.bar.pos, 0, 1024) / 1024 : bar.w / 5;
        int x = job.total ? bar.x : bar.x + (int)(hal_ticks_ms() / 3 % (uint64_t)(bar.w - fill));
        gfx_round(c, (struct rect){x, bar.y, MAX(fill, 8), bar.h}, 4, style.accent);
        char text[64] = "";
        if (job.total) {
            char a[16], b[16];
            fmt_size(a, sizeof(a), job.done);
            fmt_size(b, sizeof(b), job.total);
            ksnprintf(text, sizeof(text), "%s %s of %s", job.step ? job.step : "", a, b);
        }
        gfx_text_box(c, (struct rect){0, bar.y + 14, c->w, 16}, &font_small, text, style.tertiary, ALIGN_CENTER);
        return;
    }
    struct rect b = button_rect();
    gfx_round(c, b, 18, style.accent);
    gfx_text_box(c, b, &font_bold, job.stage == ST_FAILED ? "Try Again" : "Get Chrome", style.accent_text,
                 ALIGN_CENTER);
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
    } else if (e->type == EV_MOUSE_UP && e->button == BUTTON_LEFT && job.stage != ST_WORKING &&
               rect_contains(button_rect(), e->x, e->y)) {
        job.stage = ST_WORKING;
        job.cancel = false;
        job.total = job.done = 0;
        spring_set(&job.bar, 0);
        str_copy(job.status, "Asking Google's update server", sizeof(job.status));
        job.pending = true;
        wm_invalidate(w);
    } else if (e->type == EV_KEY && e->key == 27 && job.stage != ST_WORKING) {
        wm_close(w);
    } else if (e->type == EV_TIMER && job.stage == ST_WORKING) {
        wm_invalidate(w);
    }
}

static void destroy(struct window *w)
{
    if (job.win == w) {
        job.win = NULL;
        job.cancel = job.stage == ST_WORKING;
    }
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void say(void *ctx, const char *text)
{
    UNUSED(ctx);
    if (str_starts(text, "https://"))
        text = "Downloading Chrome from dl.google.com";
    str_copy(job.status, text, sizeof(job.status));
}

/* Runs inside the desktop loop: keeping the desktop alive here lets you go on working while Chrome arrives. */
static void progress(void *ctx, const char *stage, uint64_t done, uint64_t total)
{
    UNUSED(ctx);
    job.step = stage;
    job.done = done;
    job.total = total;
    job.bar.target = total ? (int)(done * 1024 / total) : 0;
    uint64_t now = hal_ticks_ms();
    if (now - job.drawn < 40)
        return;
    spring_step(&job.bar, 300, 30, (int)MIN(now - job.drawn, 64u));
    job.drawn = now;
    if (job.win)
        wm_invalidate(job.win);
    dock_redraw();
    input_poll();
#if CONFIG_WIN32
    w32_background();
#endif
}

static bool stopped(void *ctx)
{
    UNUSED(ctx);
    return job.cancel;
}

/* Chrome comes by itself: a little while after the first login, with a network and room on the disk, SynthOS
   fetches it in the background (the Dock icon shows how far it got). config chromeauto off turns this off. */
static bool auto_get = true;
static uint64_t auto_after;

void chrome_auto_arm(void)
{
    auto_after = hal_ticks_ms() + 15000;
}

static void auto_start(void)
{
    char exe[PATH_MAX_LEN];
    struct bundle_info bundle;
    if (!auto_get || !auto_after || hal_ticks_ms() < auto_after || job.stage == ST_WORKING || job.pending)
        return;
    auto_after = 0;
    if (bundle_info(&bundle))
        return;
    if (chrome_path(exe, sizeof(exe)) || job.stage == ST_FAILED || net_interface_count() == 0 ||
        !net_interface(0)->configured)
        return;
    job.stage = ST_WORKING;
    job.cancel = false;
    job.quiet = true;
    job.total = job.done = 0;
    spring_set(&job.bar, 0);
    str_copy(job.status, "Asking Google's update server", sizeof(job.status));
    job.pending = true;
}

int chrome_app_progress(const struct gui_app *app)
{
    if (app != &chrome_app || job.stage != ST_WORKING)
        return -1;
    return job.total ? (int)(job.done * 1024 / job.total) : 0;
}

void chrome_install_run(void)
{
    auto_start();
    if (!job.pending)
        return;
    job.pending = false;
    struct omaha_ui ui = {.say = say, .progress = progress, .cancelled = stopped};
    int err = chrome_install(&ui);
    bool quiet = job.quiet && !job.win;
    job.quiet = false;
    dock_redraw();
    if (err) {
        job.stage = ST_FAILED;
        str_copy(job.status, job.cancel ? "Stopped." : ui.error, sizeof(job.status));
        if (job.win)
            wm_invalidate(job.win);
        return;
    }
    picture_tried = false;
    load_picture();
    gui_desktop_changed();
    if (job.win)
        wm_close(job.win);
    gui_notify("Chrome is ready", "Google Chrome is installed; it is in the Dock and on the desktop.");
    job.stage = ST_OFFER;
    if (!quiet)
        launch(job.arg[0] ? job.arg : NULL);
}

static void get_auto(char *buf, size_t cap)
{
    str_copy(buf, auto_get ? "on" : "off", cap);
}

static int set_auto(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    auto_get = !strcmp(value, "on");
    return 0;
}

static const struct setting s_auto = {"chromeauto", "on|off", "fetch Google Chrome in the background after login",
                                      get_auto, set_auto};
SETTING(s_auto);

static void launch(const char *arg)
{
    char exe[PATH_MAX_LEN];
    if (chrome_path(exe, sizeof(exe))) {
        load_picture();
        dock_bounce(&chrome_app);
        if (!gui_start_program(exe, arg))
            gui_message("Chrome did not start", "Windows programs cannot run right now.");
        return;
    }
    str_copy(job.arg, arg ? arg : "", sizeof(job.arg));
    if (job.win && wm_window_alive(job.win)) {
        wm_focus(job.win);
        return;
    }
    if (job.stage != ST_WORKING)
        job.stage = ST_OFFER;
    job.win = wm_open("Chrome", ICON_BROWSER, WIN_W, WIN_H, &ops, NULL, 0);
    if (job.win)
        wm_set_timer(job.win, 100);
}
