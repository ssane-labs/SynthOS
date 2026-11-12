#include <synth/bundle.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "../internal.h"

/* The image carries a browser as a bundle (tools/mkbundle.py): Chromium's own 32-bit Windows snapshot, or on the
   64-bit system the Obscura engine. A few seconds after the first login it is unpacked into /home/apps/<name>
   without any network; the Dock icon shows how far it got. */

#define DIR "/home/apps/chromium"
#define EXE DIR "/chrome.exe"
#define OBSCURA_DIR "/home/apps/obscura"

static struct canvas picture;
static bool picture_tried;

static void load_picture(void)
{
    if (picture.px || picture_tried || !vfs_exists(EXE))
        return;
    picture_tried = true;
    gui_program_icon(EXE, 64, &picture);
}

static void launch(const char *arg);

static const struct gui_app chromium_app = {"Chromium", "The web browser", ICON_BROWSER, launch,
                                            ".html;.htm;.xhtml;.url", &picture};
GUI_APP(chromium_app);

const struct gui_app *gui_builtin_app_for_program(const char *path)
{
    if (str_starts(path, DIR "/") && !str_casecmp(path_basename(path), "chrome.exe")) {
        load_picture();
        return &chromium_app;
    }
    if (str_starts(path, OBSCURA_DIR "/") && !str_casecmp(path_basename(path), "obscura.exe"))
        return gui_app_find("Obscura");
    return chrome_app_for(path);
}

static struct {
    struct window *win;
    bool working, pending, failed, open_after;
    uint64_t done, total, drawn, auto_after;
    char arg[PATH_MAX_LEN];
    const struct gui_app *app;
} job;

/* which browser the bundle is, where it goes and its start file, which is unpacked last */
static const struct gui_app *bundle_app(char *dir, size_t cap, const char **exe)
{
    struct bundle_info info;
    if (!bundle_info(&info))
        return NULL;
    bool obscura = !strcmp(info.title, "Obscura");
    if (dir)
        str_copy(dir, obscura ? OBSCURA_DIR : DIR, cap);
    if (exe)
        *exe = obscura ? "obscura.exe" : "chrome.exe";
    return obscura ? gui_app_find("Obscura") : &chromium_app;
}

int gui_app_progress(const struct gui_app *app)
{
    if (job.working && app == job.app)
        return (int)(job.total ? job.done * 1024 / job.total : 0);
    if (app == &chromium_app || app == gui_app_find("Obscura"))
        return -1;
    return chrome_app_progress(app);
}

bool gui_bundle_has(const struct gui_app *app)
{
    return app && bundle_app(NULL, 0, NULL) == app;
}

#define WIN_W 420
#define WIN_H 200

static void paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, style.content);
    gfx_icon(c, (c->w - 56) / 2, 22, ICON_BROWSER, 56);
    char line[96];
    const char *name = job.app ? job.app->name : "the browser";
    if (job.failed)
        ksnprintf(line, sizeof(line), "%s could not be set up: the disk may be full.", name);
    else
        ksnprintf(line, sizeof(line), "Setting up %s", name);
    gfx_text_box(c, (struct rect){0, 92, c->w, 24}, &font_title, line, job.failed ? style.error : style.text,
                 ALIGN_CENTER);
    struct rect bar = {48, 136, WIN_W - 96, 8};
    gfx_round(c, bar, 4, style.separator);
    int fill = job.total ? (int)((uint64_t)bar.w * job.done / job.total) : 0;
    gfx_round(c, (struct rect){bar.x, bar.y, MAX(fill, 8), bar.h}, 4, style.accent);
    char a[16], b[16], text[48];
    fmt_size(a, sizeof(a), job.done);
    fmt_size(b, sizeof(b), job.total);
    ksnprintf(text, sizeof(text), "%s of %s", a, b);
    gfx_text_box(c, (struct rect){0, 152, c->w, 16}, &font_small, text, style.tertiary, ALIGN_CENTER);
}

static void event(struct window *w, struct gui_event *e)
{
    if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
    else if (e->type == EV_KEY && e->key == KEY_ESC)
        wm_close(w);
}

static void destroy(struct window *w)
{
    if (job.win == w)
        job.win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

void chromium_auto_arm(void)
{
    job.auto_after = hal_ticks_ms() + 4000;
}

static void start(bool open_after)
{
    struct bundle_info info;
    char dir[PATH_MAX_LEN], exe[PATH_MAX_LEN];
    const char *last;
    if (job.working || job.pending || !bundle_info(&info) || !(job.app = bundle_app(dir, sizeof(dir), &last)))
        return;
    path_join(dir, last, exe, sizeof(exe));
    if (vfs_exists(exe))
        return;
    job.pending = true;
    job.failed = false;
    job.done = 0;
    job.total = info.bytes;
    job.open_after |= open_after;
}

static struct bundle_job *unpacking;

static void unpack_finished(int err)
{
    bundle_job_free(unpacking);
    unpacking = NULL;
    job.working = false;
    dock_redraw();
    if (err) {
        job.failed = true;
        if (job.win)
            wm_invalidate(job.win);
        return;
    }
    picture_tried = false;
    load_picture();
    gui_desktop_changed();
    if (job.win)
        wm_close(job.win);
    if (job.open_after) {
        job.open_after = false;
        gui_app_launch(job.app, job.arg[0] ? job.arg : NULL);
    }
}

/* Called from the desktop's event pump: unpacks a short step at a time, so the desktop keeps answering. */
void chromium_run(void)
{
    if (job.auto_after && hal_ticks_ms() >= job.auto_after) {
        job.auto_after = 0;
        start(false);
    }
    if (job.pending) {
        job.pending = false;
        char dir[PATH_MAX_LEN];
        const char *last;
        bundle_app(dir, sizeof(dir), &last);
        vfs_mkdirs(dir);
        int err;
        if (!(unpacking = bundle_job_start(dir, last, &err))) {
            job.failed = true;
            if (job.win)
                wm_invalidate(job.win);
            return;
        }
        job.working = true;
        dock_redraw();
    }
    if (!unpacking)
        return;
    int r = bundle_job_step(unpacking, 20);
    bundle_job_progress(unpacking, &job.done, &job.total);
    uint64_t now = hal_ticks_ms();
    if (now - job.drawn >= 100) {
        job.drawn = now;
        if (job.win)
            wm_invalidate(job.win);
        dock_redraw();
    }
    if (r <= 0)
        unpack_finished(r);
}

static void launch(const char *arg)
{
    if (vfs_exists(EXE)) {
        load_picture();
        dock_bounce(&chromium_app);
        if (!gui_start_program(EXE, arg))
            gui_message("Chromium did not start", "Windows programs cannot run right now.");
        return;
    }
    if (bundle_app(NULL, 0, NULL) != &chromium_app) {
        /* An image without Chromium: Google Chrome comes from the network instead. */
        gui_app_launch(gui_app_find("Chrome"), arg);
        return;
    }
    gui_bundle_unpack(arg);
}

/* unpack the bundled browser now, in a window with a progress bar, and open it with arg afterwards */
void gui_bundle_unpack(const char *arg)
{
    str_copy(job.arg, arg ? arg : "", sizeof(job.arg));
    start(true);
    job.open_after = true;
    if (job.win && wm_window_alive(job.win)) {
        wm_focus(job.win);
        return;
    }
    job.win = wm_open(job.app ? job.app->name : "Browser", ICON_BROWSER, WIN_W, WIN_H, &ops, NULL, 0);
}
