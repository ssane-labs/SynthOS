#include <synth/command.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

/* Spaces: several desktops, each with its own windows. Switching pushes the old desktop aside and slides the new
   one in; Mission Control shows them in a row at the top, where windows can be dropped on another desktop. */

#define SLIDE_MS 320

static int space_count = 1;

struct slide {
    struct canvas from, to;
    int dir;
    uint64_t start;
};

static struct window *slide_win;

int wm_space_count(void)
{
    return space_count;
}

static void slide_paint(struct window *w, struct canvas *c)
{
    struct slide *s = w->data;
    int t = ease_out(anim_progress(s->start, SLIDE_MS * 100 / MAX(gui_setting_number("animspeed", 100), 1)));
    int W = c->w, off = W * t / 1024;
    gfx_blit(c, -s->dir * off, 0, &s->from, (struct rect){0, 0, s->from.w, s->from.h});
    gfx_blit(c, s->dir * (W - off), 0, &s->to, (struct rect){0, 0, s->to.w, s->to.h});
}

static void slide_event(struct window *w, struct gui_event *e)
{
    struct slide *s = w->data;
    if (e->type == EV_TIMER) {
        if (anim_progress(s->start, SLIDE_MS * 100 / MAX(gui_setting_number("animspeed", 100), 1)) >= 1024)
            wm_destroy(w);
        else
            wm_invalidate(w);
    } else if (e->type == EV_MOUSE_DOWN) {
        e->handled = true;
    }
}

static void slide_destroy(struct window *w)
{
    struct slide *s = w->data;
    canvas_free(&s->from);
    canvas_free(&s->to);
    kfree(s);
    if (slide_win == w)
        slide_win = NULL;
}

static const struct window_ops slide_ops = {slide_paint, slide_event, NULL, slide_destroy};

void wm_space_switch(int space)
{
    if (space < 0 || space >= space_count || space == wm_space_current())
        return;
    if (slide_win && wm_window_alive(slide_win))
        wm_destroy(slide_win);
    int dir = space > wm_space_current() ? 1 : -1;
    wm_render();
    struct rect s = wm_screen_rect(0);
    struct rect area = {s.x, s.y + menubar_height(), s.w, s.h - menubar_height()};
    struct slide *sl = kzalloc(sizeof(*sl));
    bool animate = sl && gui_setting_on("animations") && wm_capture(area, &sl->from);
    wm_activate_space(space);
    if (!animate) {
        if (sl)
            canvas_free(&sl->from);
        kfree(sl);
        return;
    }
    wm_render();
    if (!wm_capture(area, &sl->to)) {
        canvas_free(&sl->from);
        kfree(sl);
        return;
    }
    sl->dir = dir;
    sl->start = hal_ticks_ms();
    slide_win = wm_popup(area.x, area.y, area.w, area.h, &slide_ops, sl, NULL);
    if (!slide_win) {
        canvas_free(&sl->from);
        canvas_free(&sl->to);
        kfree(sl);
        return;
    }
    slide_win->flags |= WIN_OVERLAY | WIN_NOSHADOW | WIN_PASSIVE;
    slide_win->radius = 0;
    wm_move(slide_win, area.x, area.y);
    wm_set_timer(slide_win, 16);
}

int wm_space_add(void)
{
    if (space_count >= MAX_SPACES)
        return -1;
    return space_count++;
}

void wm_space_remove(int space)
{
    if (space_count <= 1 || space < 0 || space >= space_count)
        return;
    wm_spaces_merge(space, space > 0 ? space - 1 : 0);
    space_count--;
    chrome_changed();
}

struct space_list {
    struct shell *sh;
    int space, count;
};

static void list_window(struct window *w, void *ctx)
{
    struct space_list *l = ctx;
    if (w->space != l->space || (w->flags & (WIN_POPUP | WIN_OVERLAY | WIN_DIALOG | WIN_BARE | WIN_HIDDEN)))
        return;
    sh_printf(l->sh, "%s%s", l->count++ ? ", " : "  ", w->title);
}

COMMAND(desktops, "spaces", "system", "desktops [add|go <n>|remove <n>|move <window> <n>]",
        "list the desktops (Spaces) with their windows, add one, switch, remove or move a window to another")
{
    if (!wm_screen_count())
        return sh_error(sh, "desktops", "the desktop is not running");
    long a = 0, b = 0;
    if (argc == 1) {
        for (int i = 0; i < space_count; i++) {
            struct space_list l = {sh, i, 0};
            sh_printf(sh, "%c Desktop %d", i == wm_space_current() ? '*' : ' ', i + 1);
            wm_foreach(list_window, &l);
            sh_puts(sh, "\n");
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "add")) {
        int n = wm_space_add();
        if (n < 0)
            return sh_error(sh, "desktops", "there can be at most %d desktops", MAX_SPACES);
        sh_printf(sh, "Desktop %d\n", n + 1);
        return 0;
    }
    if (argc == 3 && str_parse_long(argv[2], &a) && (a < 1 || a > space_count))
        return sh_error(sh, "desktops", "there is no desktop %ld", a);
    if (argc == 3 && a && !strcmp(argv[1], "go")) {
        wm_space_switch(a - 1);
        return 0;
    }
    if (argc == 3 && a && !strcmp(argv[1], "remove")) {
        if (space_count == 1)
            return sh_error(sh, "desktops", "the last desktop stays");
        wm_space_remove(a - 1);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "move") && str_parse_long(argv[2], &a) && str_parse_long(argv[3], &b)) {
        struct window *list[64];
        int n = wm_window_list(list, 64);
        if (a < 1 || a > n)
            return sh_error(sh, "desktops", "there is no window %ld", a);
        if (b < 1 || b > space_count)
            return sh_error(sh, "desktops", "there is no desktop %ld", b);
        wm_window_to_space(list[a - 1], b - 1);
        return 0;
    }
    return command_usage(sh, command_find("desktops"));
}
