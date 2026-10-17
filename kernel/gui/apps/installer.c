#include <synth/block.h>
#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/install.h>
#include <synth/string.h>

#include "../internal.h"

enum page {
    PAGE_WELCOME,
    PAGE_DISK,
    PAGE_OPTIONS,
    PAGE_INSTALL,
    PAGE_DONE,
    PAGE_ERROR,
};

struct installer {
    struct ui_state st;
    enum page page;
    struct spring slide, bar;
    uint64_t tick;
    struct blockdev *disks[8];
    int disk_count, chosen;
    bool copy_home;
    struct install_job job;
};

static void go(struct installer *in, enum page page)
{
    int dir = page > in->page ? 1 : -1;
    in->page = page;
    spring_set(&in->slide, dir * 1024);
    in->slide.target = 0;
}

static void scan(struct installer *in)
{
    in->disk_count = install_targets(in->disks, 8);
    if (in->chosen >= in->disk_count)
        in->chosen = -1;
}

static void size_text(struct blockdev *d, char *buf, size_t cap)
{
    fmt_size(buf, cap, (uint64_t)d->sectors * BLOCK_SIZE);
}

static struct rect content(struct ui *ui, struct installer *in)
{
    int W = ui->win->surface.w, H = ui->win->surface.h;
    int dx = in->slide.pos * W / 1024 / 3;
    return (struct rect){40 + dx, 30, W - 80, H - 100};
}

static bool nav(struct ui *ui, const char *back, const char *next, bool next_enabled, bool *went_back)
{
    int W = ui->win->surface.w, H = ui->win->surface.h;
    *went_back = back && ui_button(ui, (struct rect){W - 40 - 260, H - 58, 120, 32}, back);
    if (!next)
        return false;
    struct rect r = {W - 40 - 130, H - 58, 130, 32};
    if (!next_enabled) {
        if (ui->c) {
            gfx_round(ui->c, r, 16, style.control);
            gfx_text_box(ui->c, r, &font_bold, next, style.tertiary, ALIGN_CENTER);
        }
        return false;
    }
    return ui_button_primary(ui, r, next);
}

static void welcome(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    if (ui->c) {
        struct rect logo = {r.x + (r.w - 96) / 2, r.y + 20, 96, 96};
        gfx_icon(ui->c, logo.x, logo.y, ICON_LOGO, 96);
    }
    ui_label(ui, (struct rect){r.x, r.y + 134, r.w, 34}, "SynthOS " SYNTH_VERSION, &font_large, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 172, r.w, 20}, "\"" SYNTH_CODENAME "\"", &font_title, style.accent, ALIGN_CENTER);
    ui_wrap_text(ui->c, (struct rect){r.x + 40, r.y + 214, r.w - 80, 80}, &font_sans,
                 "Install SynthOS on a disk of this computer to keep it there: the loaders, the kernel and your own "
                 "partition for files. Booting works with BIOS and UEFI.",
                 style.secondary);
    bool back;
    if (nav(ui, NULL, "Continue", true, &back)) {
        scan(in);
        go(in, PAGE_DISK);
    }
}

static void disk_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, "Where should SynthOS go?", &font_title, style.text, ALIGN_LEFT);
    ui_label(ui, (struct rect){r.x, r.y + 30, r.w, 18}, "Everything on the chosen disk will be erased.", &font_sans,
             style.secondary, ALIGN_LEFT);
    if (!in->disk_count) {
        ui_wrap_text(ui->c, (struct rect){r.x, r.y + 80, r.w, 60}, &font_sans,
                     "No other disk was found. Connect a disk or a USB stick of at least 12 MB.", style.text);
        if (ui_button(ui, (struct rect){r.x, r.y + 130, 120, 30}, "Look again"))
            scan(in);
    }
    for (int i = 0; i < in->disk_count; i++) {
        struct rect card = {r.x, r.y + 70 + i * 70, r.w, 60};
        bool sel = in->chosen == i, hot = ui_hover(ui, card);
        if (ui->c) {
            gfx_round(ui->c, card, 16, sel ? ALPHA(style.accent, 50) : hot ? style.hover : style.group);
            gfx_round_frame(ui->c, card, 16, sel ? style.accent : style.separator);
            gfx_icon(ui->c, card.x + 14, card.y + 10, strncmp(in->disks[i]->name, "usb", 3) ? ICON_COMPUTER : ICON_USB, 40);
            char size[16], title[64];
            size_text(in->disks[i], size, sizeof(size));
            ksnprintf(title, sizeof(title), "%s  ·  %s", in->disks[i]->model[0] ? in->disks[i]->model : "Disk", size);
            gfx_text_box(ui->c, (struct rect){card.x + 70, card.y + 10, card.w - 90, 20}, &font_bold, title, style.text,
                         ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){card.x + 70, card.y + 30, card.w - 90, 18}, &font_small, in->disks[i]->name,
                         style.secondary, ALIGN_LEFT);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
            ui->consumed = true;
            in->chosen = i;
        }
    }
    bool back;
    if (nav(ui, "Back", "Continue", in->chosen >= 0, &back))
        go(in, PAGE_OPTIONS);
    if (back)
        go(in, PAGE_WELCOME);
}

static void options_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    struct blockdev *d = in->disks[CLAMP(in->chosen, 0, 7)];
    char size[16], text[160];
    size_text(d, size, sizeof(size));
    ui_label(ui, (struct rect){r.x, r.y, r.w, 26}, "Ready to install", &font_title, style.text, ALIGN_LEFT);
    ksnprintf(text, sizeof(text), "SynthOS will be installed on %s (%s). Everything on it will be erased.", d->name, size);
    ui_wrap_text(ui->c, (struct rect){r.x, r.y + 34, r.w, 40}, &font_sans, text, style.secondary);
    struct rect g = ui_group(ui, r.x, r.y + 86, r.w, 1, 52);
    ui_group_label(ui, (struct rect){g.x, g.y, g.w, 52}, "Bring my files and settings", "everything in /home", true);
    ui_switch(ui, (struct rect){g.x + g.w - 50, g.y + 15, 36, 22}, &in->copy_home);
    bool back;
    if (nav(ui, "Back", "Install", true, &back)) {
        if (install_begin(&in->job, d, in->copy_home) == 0)
            go(in, PAGE_INSTALL);
        else {
            str_copy(in->job.message, "This disk cannot be used", sizeof(in->job.message));
            go(in, PAGE_ERROR);
        }
    }
    if (back)
        go(in, PAGE_DISK);
}

static void install_page(struct ui *ui, struct installer *in)
{
    struct rect r = content(ui, in);
    if (ui->c) {
        struct rect logo = {r.x + (r.w - 72) / 2, r.y + 30, 72, 72};
        gfx_icon(ui->c, logo.x, logo.y, ICON_LOGO, 72);
    }
    ui_label(ui, (struct rect){r.x, r.y + 120, r.w, 26}, "Installing SynthOS", &font_title, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 150, r.w, 18}, in->job.message, &font_sans, style.secondary, ALIGN_CENTER);
    struct rect bar = {r.x + 40, r.y + 190, r.w - 80, 8};
    if (ui->c) {
        gfx_round(ui->c, bar, 4, ALPHA(style.text, 30));
        int fill = bar.w * CLAMP(in->bar.pos, 0, 1024) / 1024;
        gfx_round(ui->c, (struct rect){bar.x, bar.y, fill, bar.h}, 4, style.accent);
    }
    char pct[8];
    ksnprintf(pct, sizeof(pct), "%d%%", in->job.percent);
    ui_label(ui, (struct rect){r.x, r.y + 206, r.w, 18}, pct, &font_small, style.tertiary, ALIGN_CENTER);
}

static void done_page(struct ui *ui, struct installer *in, bool ok)
{
    struct rect r = content(ui, in);
    if (ui->c) {
        int cx = r.x + r.w / 2, cy = r.y + 70;
        gfx_circle(ui->c, cx, cy, 34, ok ? style.ok : style.error);
        gfx_symbol(ui->c, cx - 16, cy - 16, ok ? SYM_CHECK : SYM_CLOSE, 32, RGB(255, 255, 255));
    }
    ui_label(ui, (struct rect){r.x, r.y + 124, r.w, 26}, ok ? "SynthOS is installed" : "The installation stopped",
             &font_title, style.text, ALIGN_CENTER);
    ui_wrap_text(ui->c, (struct rect){r.x + 40, r.y + 160, r.w - 80, 60}, &font_sans,
                 ok ? "Remove the current boot disk, or choose the new one in the firmware boot menu, and restart."
                    : in->job.message,
                 style.secondary);
    bool back;
    if (ok) {
        if (nav(ui, "Close", "Restart", true, &back))
            shell_power_menu(true);
        if (back)
            wm_close(ui->win);
    } else if (nav(ui, NULL, "Back", true, &back)) {
        go(in, PAGE_DISK);
    }
}

static void frame(struct ui *ui, void *data)
{
    struct installer *in = data;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, ui->win->surface.w, ui->win->surface.h}, style.content);
    struct canvas *saved = ui->c, faded;
    if (ui->c) {
        faded = *ui->c;
        ui->c = &faded;
    }
    switch (in->page) {
    case PAGE_WELCOME: welcome(ui, in); break;
    case PAGE_DISK: disk_page(ui, in); break;
    case PAGE_OPTIONS: options_page(ui, in); break;
    case PAGE_INSTALL: install_page(ui, in); break;
    case PAGE_DONE: done_page(ui, in, true); break;
    default: done_page(ui, in, false); break;
    }
    if (saved)
        ui->c = saved;
}

static void paint(struct window *w, struct canvas *c)
{
    struct installer *in = w->data;
    ui_run(w, &in->st, c, NULL, frame, in);
}

static void event(struct window *w, struct gui_event *e)
{
    struct installer *in = w->data;
    if (e->type == EV_TIMER) {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - in->tick, 64u);
        in->tick = now;
        bool busy = spring_step(&in->slide, 300, 30, dt);
        if (in->page == PAGE_INSTALL) {
            uint64_t until = now + 40;
            int r = 1;
            while (r > 0 && hal_ticks_ms() < until)
                r = install_step(&in->job);
            in->bar.target = in->job.percent * 1024 / 100;
            if (r < 0)
                go(in, PAGE_ERROR);
            else if (r == 0)
                go(in, PAGE_DONE);
            busy = true;
        }
        busy |= spring_step(&in->bar, 200, 26, dt);
        if (busy)
            wm_invalidate(w);
        return;
    }
    ui_run(w, &in->st, NULL, e, frame, in);
}

static bool on_close(struct window *w)
{
    struct installer *in = w->data;
    return in->page != PAGE_INSTALL;
}

static void destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, on_close, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    struct installer *in = kzalloc(sizeof(*in));
    if (!in)
        return;
    in->chosen = -1;
    in->copy_home = true;
    in->tick = hal_ticks_ms();
    struct window *w = wm_open("Install SynthOS", ICON_INSTALLER, 600, 440, &ops, in, 0);
    if (!w) {
        kfree(in);
        return;
    }
    wm_set_timer(w, 16);
}

static const struct gui_app installer_app = {"Install SynthOS", "Put SynthOS on a disk of this computer", ICON_INSTALLER,
                                             launch, NULL, NULL};
GUI_APP(installer_app);
