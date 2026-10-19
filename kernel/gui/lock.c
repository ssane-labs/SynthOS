#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/users.h>

#include "internal.h"

#define AVATAR 84
#define FIELD_W 220
#define FIELD_H 32

struct lockscreen {
    struct ui_state st;
    struct ui_text pass;
    int user;
    bool choosing;
    bool login;
    bool error;
    uint64_t shake_until;
    struct canvas backdrop;
};

static struct window *lock_win;

static const char *const day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const month_names[12] = {"January", "February", "March",     "April",   "May",      "June",
                                            "July",    "August",   "September", "October", "November", "December"};

static int weekday(int y, int m, int d)
{
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

bool gui_locked(void)
{
    return lock_win != NULL;
}

static void make_backdrop(struct lockscreen *l)
{
    const struct canvas *wp = desktop_wallpaper(0);
    if (!wp || !wp->px)
        return;
    int w = MAX(8, wp->w / 12), h = MAX(8, wp->h / 12);
    if (!canvas_alloc(&l->backdrop, w, h))
        return;
    l->backdrop.clip = (struct rect){0, 0, w, h};
    gfx_scale(&l->backdrop, (struct rect){0, 0, w, h}, wp, 255);
    for (int pass = 0; pass < 2; pass++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int sx = CLAMP(x + dx, 0, w - 1), sy = CLAMP(y + dy, 0, h - 1);
                        uint32_t p = l->backdrop.px[sy * w + sx];
                        r += p >> 16 & 0xff;
                        g += p >> 8 & 0xff;
                        b += p & 0xff;
                        n++;
                    }
                l->backdrop.px[y * w + x] = 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
}

static struct rect user_slot(int W, int i, int n)
{
    int slot = 140, x0 = (W - n * slot) / 2;
    return (struct rect){x0 + i * slot, 0, slot, AVATAR + 40};
}

static bool attempt(struct window *w, struct lockscreen *l)
{
    const struct user *u = users_get(l->user);
    if (!u)
        return false;
    if (!user_check(u->name, l->pass.buf)) {
        ui_text_set(&l->pass, "");
        l->error = true;
        l->shake_until = hal_ticks_ms() + 450;
        wm_set_timer(w, 30);
        return false;
    }
    ui_text_set(&l->pass, "");
    char name[USER_NAME_MAX];
    str_copy(name, u->name, sizeof(name));
    bool login = l->login;
    wm_destroy(w);
    if (login)
        gui_session_login(name);
    return true;
}

static void frame(struct ui *ui, void *data)
{
    struct lockscreen *l = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    if (ui->c) {
        if (l->backdrop.px)
            gfx_scale(ui->c, (struct rect){0, 0, W, H}, &l->backdrop, 255);
        else
            gfx_clear(ui->c, (struct rect){0, 0, W, H}, RGB(30, 30, 40));
        gfx_fill(ui->c, (struct rect){0, 0, W, H}, ALPHA(0, 70));
        struct rtc_time t;
        time_local(&t);
        int m = CLAMP((int)t.month, 1, 12);
        char date[64], clock[16];
        ksnprintf(date, sizeof(date), "%s %u %s", day_names[weekday((int)t.year, m, (int)t.day)], t.day, month_names[m - 1]);
        gui_clock_time(&t, clock, sizeof(clock), false, false);
        gfx_text_box(ui->c, (struct rect){0, H / 10, W, 30}, &font_title, date, ALPHA(0xffffffffu, 230), ALIGN_CENTER);
        gfx_text_box(ui->c, (struct rect){0, H / 10 + 26, W, font_clock.height}, &font_clock, clock,
                     ALPHA(0xffffffffu, 235), ALIGN_CENTER);
    }
    int count = users_count();
    int base_y = H - 340;
    if (l->choosing) {
        for (int i = 0; i < count; i++) {
            struct rect slot = user_slot(W, i, count);
            slot.y = base_y;
            const struct user *u = users_get(i);
            if (ui->c) {
                gfx_drop_shadow(ui->c, (struct rect){slot.x + (slot.w - AVATAR) / 2, slot.y, AVATAR, AVATAR}, AVATAR / 2, 14, 90, 4);
                gfx_icon(ui->c, slot.x + (slot.w - AVATAR) / 2, slot.y, ICON_USER, AVATAR);
                gfx_text_box(ui->c, (struct rect){slot.x, slot.y + AVATAR + 10, slot.w, 20}, &font_bold, u->full,
                             RGB(255, 255, 255), ALIGN_CENTER);
            }
            if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, slot)) {
                ui->consumed = true;
                l->user = i;
                l->choosing = false;
                l->error = false;
                ui->st->focus = 0;
                return;
            }
        }
    } else {
        const struct user *u = users_get(l->user);
        int cx = W / 2;
        if (ui->c && u) {
            gfx_drop_shadow(ui->c, (struct rect){cx - AVATAR / 2, base_y, AVATAR, AVATAR}, AVATAR / 2, 14, 90, 4);
            gfx_icon(ui->c, cx - AVATAR / 2, base_y, ICON_USER, AVATAR);
            gfx_text_box(ui->c, (struct rect){0, base_y + AVATAR + 10, W, 22}, &font_title, u->full, RGB(255, 255, 255),
                         ALIGN_CENTER);
        }
        int shake = 0;
        uint64_t now = hal_ticks_ms();
        if (now < l->shake_until) {
            int phase = (int)(l->shake_until - now) / 30;
            shake = (phase & 1 ? 8 : -8) * (int)(l->shake_until - now) / 450;
        } else if (l->shake_until) {
            l->shake_until = 0;
            wm_set_timer(w, 1000);
        }
        struct rect field = {cx - FIELD_W / 2 + shake, base_y + AVATAR + 44, FIELD_W, FIELD_H};
        if (ui->c) {
            gfx_round(ui->c, field, FIELD_H / 2, ALPHA(0xffffffffu, 46));
            gfx_round_frame(ui->c, field, FIELD_H / 2, ALPHA(0xffffffffu, 90));
            const char *hint = u && u->has_password ? "Enter Password" : "Press Enter to log in";
            if (!l->pass.len) {
                gfx_text_box(ui->c, rect_inset(field, 4), &font_sans, hint, ALPHA(0xffffffffu, 170), ALIGN_CENTER);
            } else {
                char dots[3 * 64 + 1];
                int n = MIN(l->pass.len, 64);
                for (int i = 0; i < n; i++)
                    memcpy(dots + i * 3, "•", 3);
                dots[n * 3] = 0;
                gfx_text_box(ui->c, (struct rect){field.x + 14, field.y, field.w - 48, field.h}, &font_bold, dots,
                             RGB(255, 255, 255), ALIGN_LEFT);
                gfx_circle(ui->c, field.x + field.w - 17, field.y + field.h / 2, 11, ALPHA(0xffffffffu, 80));
                gfx_symbol(ui->c, field.x + field.w - 25, field.y + field.h / 2 - 8, SYM_RIGHT, 16, RGB(255, 255, 255));
            }
            if (l->error)
                gfx_text_box(ui->c, (struct rect){0, field.y + FIELD_H + 10, W, 20}, &font_sans, "Incorrect password",
                             ALPHA(0xffffffffu, 220), ALIGN_CENTER);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN &&
            ui_hover(ui, (struct rect){field.x + field.w - 30, field.y, 30, field.h})) {
            ui->consumed = true;
            if (attempt(w, l))
                ui->win = NULL;
            return;
        }
        if (count > 1 && l->login) {
            struct rect back = {cx - 60, field.y + FIELD_H + 34, 120, 24};
            if (ui->c)
                gfx_text_box(ui->c, back, &font_sans, "‹  Other users", ALPHA(0xffffffffu, 200), ALIGN_CENTER);
            if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, back)) {
                ui->consumed = true;
                l->choosing = true;
                return;
            }
        }
    }
    if (l->login) {
        static const char *const labels[2] = {"Restart", "Shut Down"};
        static const enum symbol_id syms[2] = {SYM_RESTART, SYM_POWER};
        for (int i = 0; i < 2; i++) {
            struct rect b = {W / 2 - 110 + i * 120, H - 44, 100, 40};
            if (ui->c) {
                gfx_circle(ui->c, b.x + b.w / 2, b.y - 14, 16, ALPHA(0xffffffffu, 50));
                gfx_symbol(ui->c, b.x + b.w / 2 - 8, b.y - 22, syms[i], 16, RGB(255, 255, 255));
                gfx_text_box(ui->c, (struct rect){b.x, b.y + 6, b.w, 16}, &font_small, labels[i], ALPHA(0xffffffffu, 210),
                             ALIGN_CENTER);
            }
            if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, (struct rect){b.x, b.y - 32, b.w, 56})) {
                ui->consumed = true;
                gui_quit(i ? QUIT_POWEROFF : QUIT_REBOOT);
                return;
            }
        }
    }
    int key = ui_key(ui);
    if (key == KEY_NONE)
        return;
    ui->consumed = true;
    int base = KEY_BASE(key);
    if (l->choosing) {
        if (base == KEY_RIGHT)
            l->user = (l->user + 1) % count;
        else if (base == KEY_LEFT)
            l->user = (l->user + count - 1) % count;
        else if (base == KEY_ENTER || key == '\r')
            l->choosing = false;
        return;
    }
    if (base == KEY_ENTER || key == '\r') {
        if (attempt(w, l))
            ui->win = NULL;
        return;
    }
    if (base == KEY_ESC) {
        ui_text_set(&l->pass, "");
        if (count > 1 && l->login)
            l->choosing = true;
        return;
    }
    l->error = false;
    ui_text_edit(&l->pass, key);
}

static void paint(struct window *w, struct canvas *c)
{
    struct lockscreen *l = w->data;
    ui_run(w, &l->st, c, NULL, frame, l);
}

static void event(struct window *w, struct gui_event *e)
{
    struct lockscreen *l = w->data;
    ui_run(w, &l->st, NULL, e, frame, l);
}

static void destroy(struct window *w)
{
    struct lockscreen *l = w->data;
    canvas_free(&l->backdrop);
    memset(&l->pass, 0, sizeof(l->pass));
    kfree(l);
    if (lock_win == w)
        lock_win = NULL;
    wm_set_cursor_visible(true);
    chrome_changed();
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void show(bool login)
{
    if (lock_win)
        return;
    wm_close_popups();
    struct lockscreen *l = kzalloc(sizeof(*l));
    if (!l)
        return;
    l->login = login;
    const struct user *me = user_current();
    for (int i = 0; i < users_count(); i++)
        if (users_get(i) == me)
            l->user = i;
    l->choosing = login && users_count() > 1;
    make_backdrop(l);
    struct rect s = wm_screen_rect(0);
    lock_win = wm_popup(s.x, s.y, s.w, s.h, &ops, l, NULL);
    if (!lock_win) {
        canvas_free(&l->backdrop);
        kfree(l);
        return;
    }
    lock_win->flags &= ~WIN_POPUP;
    lock_win->flags |= WIN_OVERLAY | WIN_NOSHADOW;
    if (lock_win->glass) {
        glass_unregister(lock_win->glass);
        kfree(lock_win->glass);
        lock_win->glass = NULL;
    }
    lock_win->radius = 0;
    wm_move(lock_win, s.x, s.y);
    wm_set_timer(lock_win, 1000);
}

void gui_lock(void)
{
    show(false);
}

void gui_login(void)
{
    show(true);
}
