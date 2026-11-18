#include <synth/block.h>
#include <synth/command.h>
#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* The first start of an installed system walks through the languages, region, the user's account, the look, the
   desktop, the Dock and menu bar and the system's behaviour; every choice takes effect behind the card right away.
   Started from a USB stick, the system first asks whether to try it or to install it. */

#define SETUP_DONE "/home/.setup-done"
#define LIVE_DONE "/home/.live-welcome"
#define SETTINGS_FILE "/home/.synth.conf"
#define CARD_W 680
#define CARD_H 540
#define WALLS 11
#define THUMB_W 84
#define THUMB_H 52
#define ROW_H 36

enum page {
    SP_LIVE,
    SP_HELLO,
    SP_LANGUAGE,
    SP_REGION,
    SP_ACCOUNT,
    SP_LOOK,
    SP_DESKTOP,
    SP_DOCK,
    SP_SYSTEM,
    SP_DONE,
};

struct zone {
    const char *city, *offset;
};

static const struct zone zones[] = {
    {"Honolulu", "-10:00"},       {"Anchorage", "-09:00"},     {"Los Angeles", "-08:00"},  {"Denver", "-07:00"},
    {"Chicago, Mexico City", "-06:00"}, {"New York, Toronto", "-05:00"}, {"Halifax", "-04:00"},
    {"Sao Paulo, Buenos Aires", "-03:00"}, {"London, Lisbon (UTC)", "UTC"}, {"Berlin, Paris, Madrid", "+01:00"},
    {"Kaliningrad, Kyiv, Athens", "+02:00"}, {"Moscow, Istanbul, Minsk", "+03:00"}, {"Samara, Dubai", "+04:00"},
    {"Yekaterinburg, Tashkent", "+05:00"}, {"Delhi, Mumbai", "+05:30"},  {"Omsk, Dhaka", "+06:00"},
    {"Novosibirsk, Bangkok", "+07:00"}, {"Irkutsk, Beijing, Singapore", "+08:00"}, {"Yakutsk, Tokyo, Seoul", "+09:00"},
    {"Vladivostok, Sydney", "+10:00"}, {"Magadan", "+11:00"},     {"Kamchatka, Auckland", "+12:00"},
};

static const char *const accents[] = {"auto", "blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite"};
static const uint32_t accent_colors[] = {0,          0xff007aff, 0xffaf52de, 0xffff2d55, 0xffff3b30,
                                         0xffff9500, 0xffffcc00, 0xff34c759, 0xff8e8e93};

struct setup {
    struct ui_state st;
    enum page page;
    struct spring slide;
    uint64_t tick;
    struct canvas backdrop;
    bool layouts[32];
    int order[32], norder;
    int zone;
    struct ui_text full, name, pass, pass2;
    bool name_edited, autologin, want_focus;
    char error[96];
    int look, appearance, accent;
    struct canvas thumbs[WALLS];
    const struct gui_look *thumbs_look;
    bool thumbs_dark;
};

static const char *const wall_names[WALLS] = {"Look", "Mono", "Halo", "Sunrise", "Earthset", "Aurora",
                                              "Milky Way", "Dunes", "Glacier", "Saturn", "Solid"};
static const char *const wall_values[WALLS] = {"auto", "mono", "halo", "sunrise", "earthset", "aurora",
                                               "milkyway", "dunes", "glacier", "saturn", "solid"};
static const char *const widget_names[8] = {"clock", "analog", "weather", "calendar", "worldclock", "music",
                                            "notes", "system"};
static const char *const widget_titles[8] = {"Clock", "Analog Clock", "Weather", "Calendar", "World Clock", "Music",
                                             "Notes", "System"};

static struct window *setup_win;

bool gui_setup_active(void)
{
    return setup_win != NULL;
}

static bool live_stick(void)
{
    struct blockdev *d = hal_boot_disk();
    return d && !hal_installed() && !strncmp(d->name, "usb", 3);
}

bool gui_setup_needed(void)
{
    if (hal_installed())
        return !vfs_exists(SETUP_DONE);
    return live_stick() && !vfs_exists(LIVE_DONE);
}

static void apply(const char *name, const char *value)
{
    setting_apply(shell_current(), name, value);
}

static void current(const char *name, char *buf, size_t cap)
{
    const struct setting *st = setting_find(name);
    buf[0] = 0;
    if (st && st->get)
        st->get(buf, cap);
}

static bool is_on(const char *name)
{
    char v[16];
    current(name, v, sizeof(v));
    return !strcmp(v, "on");
}

static void refresh_backdrop(struct setup *s)
{
    canvas_free(&s->backdrop);
    gui_blurred_wallpaper(&s->backdrop);
}

static void go(struct setup *s, enum page page)
{
    int dir = page > s->page ? 1 : -1;
    s->page = page;
    s->error[0] = 0;
    s->st.focus = 0;
    s->want_focus = page == SP_ACCOUNT;
    spring_set(&s->slide, dir * 1024);
    s->slide.target = 0;
    wm_set_timer(setup_win, 16);
}

static struct rect card_rect(struct ui *ui)
{
    int W = ui->win->surface.w, H = ui->win->surface.h;
    return (struct rect){(W - CARD_W) / 2, (H - CARD_H) / 2, CARD_W, CARD_H};
}

static struct rect body(struct ui *ui, struct setup *s)
{
    struct rect c = card_rect(ui);
    int dx = s->slide.pos * c.w / 1024 / 4;
    return (struct rect){c.x + 44 + dx, c.y + 40, c.w - 88, c.h - 120};
}

static bool enter_key(struct ui *ui)
{
    int key = ui_key(ui);
    if (KEY_BASE(key) == KEY_ENTER || key == '\r') {
        ui->consumed = true;
        return true;
    }
    return false;
}

static bool nav(struct ui *ui, struct setup *s, const char *next, bool *back)
{
    struct rect c = card_rect(ui);
    *back = s->page > SP_HELLO && s->page < SP_DONE &&
            ui_button(ui, (struct rect){c.x + 32, c.y + c.h - 60, 110, 32}, "Back");
    if (ui->c) {
        int dots = SP_DONE - SP_HELLO + 1, x0 = c.x + c.w / 2 - dots * 8;
        for (int i = 0; i < dots && s->page >= SP_HELLO; i++)
            gfx_circle(ui->c, x0 + i * 16 + 4, c.y + c.h - 44, 3,
                       i == (int)s->page - SP_HELLO ? style.accent : ALPHA(style.text, 50));
    }
    return ui_button_primary(ui, (struct rect){c.x + c.w - 32 - 150, c.y + c.h - 60, 150, 32}, next) || enter_key(ui);
}

static void title(struct ui *ui, struct rect r, const char *text, const char *sub)
{
    ui_label(ui, (struct rect){r.x, r.y, r.w, 30}, text, &font_title, style.text, ALIGN_CENTER);
    if (sub)
        ui_label(ui, (struct rect){r.x, r.y + 32, r.w, 18}, sub, &font_sans, style.secondary, ALIGN_CENTER);
}

static void finish_live(struct setup *s, bool install)
{
    UNUSED(s);
    vfs_write_all(LIVE_DONE, "1\n", 2);
    wm_destroy(setup_win);
    gui_app_launch(gui_app_find("Install SynthOS"), install ? NULL : "hidden");
}

static bool app_card(struct ui *ui, struct rect r, const struct gui_app *app, enum icon_id icon, const char *head,
                     const char *text, bool on)
{
    bool hot = ui_hover(ui, r);
    if (ui->c) {
        gfx_round(ui->c, r, 18, on ? ALPHA(style.accent, 46) : hot ? style.hover : style.group);
        gfx_round_frame(ui->c, r, 18, on ? style.accent : style.separator);
        if (app && app->picture && app->picture->px)
            gfx_scale(ui->c, (struct rect){r.x + (r.w - 64) / 2, r.y + 20, 64, 64}, app->picture, 255);
        else
            gfx_icon(ui->c, r.x + (r.w - 64) / 2, r.y + 20, app ? app->icon : icon, 64);
        gfx_text_box(ui->c, (struct rect){r.x + 10, r.y + 96, r.w - 20, 22}, &font_bold, head, style.text, ALIGN_CENTER);
        ui_wrap_text(ui->c, (struct rect){r.x + 18, r.y + 122, r.w - 36, r.h - 128}, &font_small, text, style.secondary);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
        ui->consumed = true;
        return true;
    }
    return false;
}

static bool choice_card(struct ui *ui, struct rect r, enum icon_id icon, const char *head, const char *text, bool on)
{
    return app_card(ui, r, NULL, icon, head, text, on);
}

static void live_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    if (ui->c)
        gfx_icon(ui->c, r.x + (r.w - 72) / 2, r.y, ICON_LOGO, 72);
    title(ui, (struct rect){r.x, r.y + 86, r.w, 60}, "Welcome to SynthOS " SYNTH_VERSION,
          "It runs from the USB stick. What would you like to do?");
    int cw = (r.w - 20) / 2;
    if (choice_card(ui, (struct rect){r.x, r.y + 160, cw, 210}, ICON_USB, "Try SynthOS",
                    "Use it straight from the stick. Nothing on this computer changes.", false))
        finish_live(s, false);
    if (choice_card(ui, (struct rect){r.x + cw + 20, r.y + 160, cw, 210}, ICON_INSTALLER, "Install SynthOS",
                    "Put it on a disk of this computer, next to Windows or on its own disk.", true))
        finish_live(s, true);
    int key = ui_key(ui);
    if (key == KEY_ESC) {
        ui->consumed = true;
        finish_live(s, false);
    } else if (enter_key(ui)) {
        finish_live(s, true);
    }
}

static void hello_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    if (ui->c)
        gfx_icon(ui->c, r.x + (r.w - 96) / 2, r.y + 30, ICON_LOGO, 96);
    ui_label(ui, (struct rect){r.x, r.y + 150, r.w, 40}, "Hello", &font_large, style.text, ALIGN_CENTER);
    ui_label(ui, (struct rect){r.x, r.y + 196, r.w, 20}, "Привет  ·  Hallo  ·  Bonjour  ·  Hola", &font_title,
             style.accent, ALIGN_CENTER);
    ui_wrap_text(ui->c, (struct rect){r.x + 40, r.y + 246, r.w - 80, 80}, &font_sans,
                 "SynthOS is installed. Let's make it yours: languages and region, your account, the look, the "
                 "desktop and the Dock, and how the system behaves. Everything can be changed later in System "
                 "Settings.",
                 style.secondary);
    bool back;
    if (nav(ui, s, "Continue", &back))
        go(s, SP_LANGUAGE);
}

static void order_remove(struct setup *s, int k)
{
    for (int i = 0; i < s->norder; i++)
        if (s->order[i] == k) {
            memmove(&s->order[i], &s->order[i + 1], (size_t)(s->norder - i - 1) * sizeof(int));
            s->norder--;
            return;
        }
}

static void language_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "Languages", "The system speaks English; tick the languages you type in.");
    int n = MIN(keymap_count(), 32), cols = 3, y = r.y + 62;
    for (int i = 0; i < n; i++) {
        const char *name;
        keymap_layout_name(i, &name);
        struct rect cb = {r.x + (i % cols) * (r.w / cols), y + (i / cols) * 25, r.w / cols, 22};
        if (ui_checkbox(ui, cb, name, &s->layouts[i])) {
            if (!i)
                s->layouts[0] = true;
            else if (s->layouts[i] && s->norder < 32)
                s->order[s->norder++] = i;
            else if (!s->layouts[i])
                order_remove(s, i);
        }
    }
    y += ((n + cols - 1) / cols) * 25 + 10;
    ui_label(ui, (struct rect){r.x, y, r.w, 16}, "Order: the first language is used at login; click one to put it first.",
             &font_small, style.tertiary, ALIGN_LEFT);
    y += 22;
    int x = r.x;
    for (int i = 0; i < s->norder; i++) {
        const char *name, *code = keymap_layout_name(s->order[i], &name);
        int w = gfx_text_width(&font_sans, name) + 24;
        if (x + w > r.x + r.w)
            break;
        struct rect chip = {x, y, w, 26};
        if (ui->c) {
            gfx_round(ui->c, chip, 13, i ? style.group : ALPHA(style.accent, 60));
            gfx_round_frame(ui->c, chip, 13, i ? style.separator : style.accent);
            gfx_text_box(ui->c, chip, i ? &font_sans : &font_bold, name, style.text, ALIGN_CENTER);
        }
        if (i && ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, chip)) {
            ui->consumed = true;
            int k = s->order[i];
            memmove(&s->order[1], &s->order[0], (size_t)i * sizeof(int));
            s->order[0] = k;
        }
        UNUSED(code);
        x += w + 8;
    }
    bool back;
    if (nav(ui, s, "Continue", &back))
        go(s, SP_REGION);
    if (back)
        go(s, SP_HELLO);
}

static void switch_row(struct ui *ui, struct rect g, int i, int n, const char *label, const char *setting)
{
    struct rect row = {g.x, g.y + i * ROW_H, g.w, ROW_H};
    ui_group_label(ui, row, label, "", i == n - 1);
    bool on = is_on(setting);
    if (ui_switch(ui, (struct rect){row.x + row.w - 50, row.y + (ROW_H - 22) / 2, 36, 22}, &on))
        apply(setting, on ? "on" : "off");
}

static void choice_row(struct ui *ui, struct rect r, int y, const char *label, const char *setting,
                       const char *const *labels, const char *const *values, int n)
{
    char v[24];
    current(setting, v, sizeof(v));
    int sel = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(v, values[i]))
            sel = i;
    ui_label(ui, (struct rect){r.x, y + 6, 150, 20}, label, &font_sans, style.text, ALIGN_LEFT);
    if (ui_segmented(ui, (struct rect){r.x + 160, y, r.w - 160, 30}, labels, n, &sel))
        apply(setting, values[sel]);
}

static void region_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "Region", "Where you are and how the time is shown.");
    int y = r.y + 70;
    ui_label(ui, (struct rect){r.x, y, r.w, 18}, "Time zone", &font_bold, style.text, ALIGN_LEFT);
    struct rect picker = {r.x, y + 24, r.w, 36};
    if (ui->c) {
        gfx_round(ui->c, picker, 12, style.group);
        gfx_round_frame(ui->c, picker, 12, style.separator);
        char text[80];
        const struct zone *z = &zones[s->zone];
        ksnprintf(text, sizeof(text), "%s  (%s%s)", z->city, strcmp(z->offset, "UTC") ? "UTC" : "", z->offset);
        gfx_text_box(ui->c, picker, &font_sans, text, style.text, ALIGN_CENTER);
    }
    int nz = (int)ARRAY_SIZE(zones), was = s->zone;
    if (ui_symbol_button(ui, (struct rect){picker.x + 4, picker.y + 4, 28, 28}, SYM_LEFT, false))
        s->zone = (s->zone + nz - 1) % nz;
    if (ui_symbol_button(ui, (struct rect){picker.x + picker.w - 32, picker.y + 4, 28, 28}, SYM_RIGHT, false))
        s->zone = (s->zone + 1) % nz;
    int key = KEY_BASE(ui_key(ui));
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        ui->consumed = true;
        s->zone = (s->zone + (key == KEY_LEFT ? nz - 1 : 1)) % nz;
    }
    if (s->zone != was) {
        apply("timezone", zones[s->zone].offset);
        chrome_changed();
    }
    y += 84;
    ui_label(ui, (struct rect){r.x, y, r.w, 18}, "Clock", &font_bold, style.text, ALIGN_LEFT);
    struct rect g = ui_group(ui, r.x, y + 24, r.w, 2, ROW_H);
    switch_row(ui, g, 0, 2, "24-hour time", "clock24");
    switch_row(ui, g, 1, 2, "Show the date in the menu bar", "clockdate");
    bool back;
    if (nav(ui, s, "Continue", &back)) {
        apply("timezone", zones[s->zone].offset);
        chrome_changed();
        go(s, SP_ACCOUNT);
    }
    if (back)
        go(s, SP_LANGUAGE);
}

/* The account name follows the full name until it is edited: "Anna Smith" becomes "anna". */
static void derive_name(struct setup *s)
{
    char name[USER_NAME_MAX];
    int n = 0;
    for (const char *p = s->full.buf; *p && *p != ' ' && n < (int)sizeof(name) - 1; p++) {
        char c = (char)to_lower(*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9' && n))
            name[n++] = c;
    }
    name[n] = 0;
    ui_text_set(&s->name, name);
}

static bool account_ok(struct setup *s)
{
    if (!s->full.len)
        str_copy(s->error, "Type your name", sizeof(s->error));
    else if (!user_name_valid(s->name.buf))
        str_copy(s->error, "The account name takes small Latin letters and digits", sizeof(s->error));
    else if (!strcmp(s->name.buf, "root") || user_find(s->name.buf))
        str_copy(s->error, "This account name is taken", sizeof(s->error));
    else if (strcmp(s->pass.buf, s->pass2.buf))
        str_copy(s->error, "The passwords do not match", sizeof(s->error));
    else
        return true;
    return false;
}

static void account_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "Your account", "You use it to log in and keep your files.");
    if (ui->c)
        gfx_icon(ui->c, r.x + (r.w - 56) / 2, r.y + 62, ICON_USER, 56);
    int y = r.y + 132, lw = 150, fw = r.w - lw;
    static const char *const labels[4] = {"Full name", "Account name", "Password", "Repeat password"};
    struct ui_text *fields[4] = {&s->full, &s->name, &s->pass, &s->pass2};
    s->full.complete = s->name.complete = COMPLETE_OFF; /* Tab moves between these */
    static const char *const hints[4] = {"Anna Smith", "anna", "optional", ""};
    int ids[4], tab = KEY_BASE(ui_key(ui)) == KEY_TAB;
    bool submit = false;
    for (int i = 0; i < 4; i++) {
        ui_label(ui, (struct rect){r.x, y + i * 42 + 6, lw, 20}, labels[i], &font_sans, style.text, ALIGN_LEFT);
        char before[sizeof(s->full.buf)];
        str_copy(before, fields[i]->buf, sizeof(before));
        submit |= ui_textbox_hint(ui, (struct rect){r.x + lw, y + i * 42, fw, 30}, fields[i], hints[i]);
        ids[i] = ui->id;
        if (strcmp(before, fields[i]->buf)) {
            if (i == 0 && !s->name_edited)
                derive_name(s);
            if (i == 1)
                s->name_edited = true;
            s->error[0] = 0;
        }
    }
    if (s->want_focus) {
        s->want_focus = false;
        s->st.focus = ids[0];
    }
    if (tab) {
        ui->consumed = true;
        int at = 0;
        for (int i = 0; i < 4; i++)
            if (s->st.focus == ids[i])
                at = i + 1;
        s->st.focus = ids[at % 4];
    }
    y += 4 * 42 + 6;
    ui_checkbox(ui, (struct rect){r.x + lw, y, fw, 24}, "Log in automatically", &s->autologin);
    if (s->error[0])
        ui_label(ui, (struct rect){r.x, y + 30, r.w, 18}, s->error, &font_sans, style.error, ALIGN_CENTER);
    bool back;
    if ((nav(ui, s, "Continue", &back) || submit) && account_ok(s))
        go(s, SP_LOOK);
    if (back)
        go(s, SP_REGION);
}

static void look_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "Choose your look", "It changes behind this card as you pick.");
    int n = MIN(look_count(), 4), cw = (r.w - (n - 1) * 16) / MAX(n, 1);
    for (int i = 0; i < n; i++) {
        const struct gui_look *l = look_get(i);
        struct rect card = {r.x + i * (cw + 16), r.y + 64, cw, 168};
        bool on = s->look == i, hot = ui_hover(ui, card);
        if (ui->c) {
            struct rect pv = {card.x + 8, card.y + 8, card.w - 16, 92};
            uint32_t a = l->tint | 0xff000000u, b = l->dark ? RGB(20, 20, 28) : RGB(250, 248, 246);
            gfx_gradient(ui->c, pv, a, b);
            gfx_round_frame(ui->c, pv, 10, ALPHA(0, 30));
            gfx_round(ui->c, (struct rect){pv.x + 12, pv.y + 14, pv.w / 2, 40}, 8, l->dark ? ALPHA(0, 120) : ALPHA(0xffffffffu, 190));
            gfx_circle(ui->c, pv.x + pv.w - 20, pv.y + pv.h - 18, 8, l->accent | 0xff000000u);
            gfx_round_frame(ui->c, card, 14, on ? style.accent : hot ? style.tertiary : style.separator);
            if (on)
                gfx_round_frame(ui->c, rect_inset(card, -1), 15, style.accent);
            gfx_text_box(ui->c, (struct rect){card.x, card.y + 106, card.w, 20}, &font_bold, l->title, style.text,
                         ALIGN_CENTER);
            ui_wrap_text(ui->c, (struct rect){card.x + 10, card.y + 126, card.w - 20, 36}, &font_small, l->description,
                         style.secondary);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
            ui->consumed = true;
            s->look = i;
            apply("look", l->name);
            refresh_backdrop(s);
        }
    }
    int y = r.y + 250;
    ui_label(ui, (struct rect){r.x, y + 6, 120, 20}, "Appearance", &font_sans, style.text, ALIGN_LEFT);
    static const char *const modes[3] = {"Auto", "Light", "Dark"};
    static const char *const mode_values[3] = {"auto", "light", "dark"};
    if (ui_segmented(ui, (struct rect){r.x + 130, y, r.w - 130, 30}, modes, 3, &s->appearance)) {
        apply("appearance", mode_values[s->appearance]);
        refresh_backdrop(s);
    }
    y += 48;
    ui_label(ui, (struct rect){r.x, y + 4, 120, 20}, "Accent color", &font_sans, style.text, ALIGN_LEFT);
    for (int i = 0; i < (int)ARRAY_SIZE(accents); i++) {
        int cx = r.x + 142 + i * 34, cy = y + 14;
        struct rect hit = {cx - 14, cy - 14, 28, 28};
        if (ui->c) {
            uint32_t col = i ? accent_colors[i] : (look_get(s->look)->accent | 0xff000000u);
            if (s->accent == i)
                gfx_ring(ui->c, cx, cy, 13, style.text);
            gfx_circle(ui->c, cx, cy, 10, col);
            if (!i)
                gfx_text_box(ui->c, (struct rect){cx - 10, cy - 8, 20, 16}, &font_small, "A", RGB(255, 255, 255),
                             ALIGN_CENTER);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, hit)) {
            ui->consumed = true;
            s->accent = i;
            apply("accent", accents[i]);
        }
    }
    static const char *const glass[] = {"Liquid Glass", "Frosted", "Solid"};
    static const char *const glass_values[] = {"liquid", "frosted", "solid"};
    choice_row(ui, r, y + 46, "Transparency", "glass", glass, glass_values, 3);
    bool back;
    if (nav(ui, s, "Continue", &back))
        go(s, SP_DESKTOP);
    if (back)
        go(s, SP_ACCOUNT);
}

static void make_thumbs(struct setup *s)
{
    if (s->thumbs[0].px && s->thumbs_dark == style.dark && s->thumbs_look == look_current())
        return;
    for (int i = 0; i < WALLS; i++) {
        if (!s->thumbs[i].px && !canvas_alloc(&s->thumbs[i], THUMB_W, THUMB_H))
            continue;
        wallpaper_paint(&s->thumbs[i], i ? wall_values[i] : look_current()->wallpaper, style.dark);
    }
    s->thumbs_dark = style.dark;
    s->thumbs_look = look_current();
}

static bool widget_on(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, name, n))
            return true;
        p += len + (e ? 1 : 0);
    }
    return false;
}

static void widget_toggle(const char *name, bool on)
{
    char list[200], out[220] = "";
    current("widgets", list, sizeof(list));
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char kind[24];
        str_copy(kind, p, MIN(len + 1, sizeof(kind)));
        if (strcmp(kind, "none") && strcmp(kind, name)) {
            if (out[0])
                str_append(out, ",", sizeof(out));
            str_append(out, kind, sizeof(out));
        }
        p += len + (e ? 1 : 0);
    }
    if (on) {
        if (out[0])
            str_append(out, ",", sizeof(out));
        str_append(out, name, sizeof(out));
    }
    apply("widgets", out[0] ? out : "none");
}

static void desktop_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "Your desktop", "A wallpaper and the widgets on it.");
    char wall[PATH_MAX_LEN];
    current("wallpaper", wall, sizeof(wall));
    make_thumbs(s);
    int cols = 6, gap = (r.w - cols * THUMB_W) / (cols - 1), y = r.y + 62;
    for (int i = 0; i < WALLS; i++) {
        struct rect t = {r.x + (i % cols) * (THUMB_W + gap), y + (i / cols) * (THUMB_H + 24), THUMB_W, THUMB_H};
        bool sel = !strcmp(wall, wall_values[i]);
        if (ui->c && s->thumbs[i].px) {
            gfx_blit_round(ui->c, t, &s->thumbs[i], 8, false);
            gfx_round_frame(ui->c, t, 8, ALPHA(0, 30));
            if (sel)
                gfx_round_frame(ui->c, rect_inset(t, -3), 11, style.accent);
            gfx_text_box(ui->c, (struct rect){t.x - 6, t.y + t.h + 3, t.w + 12, 16}, &font_small, wall_names[i],
                         sel ? style.text : style.secondary, ALIGN_CENTER);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, t)) {
            ui->consumed = true;
            apply("wallpaper", wall_values[i]);
            refresh_backdrop(s);
        }
    }
    y += 2 * (THUMB_H + 24) + 8;
    ui_label(ui, (struct rect){r.x, y, r.w, 18}, "Widgets", &font_bold, style.text, ALIGN_LEFT);
    char kinds[200];
    current("widgets", kinds, sizeof(kinds));
    for (int i = 0; i < 8; i++) {
        bool on = widget_on(kinds, widget_names[i]);
        struct rect cb = {r.x + (i % 4) * (r.w / 4), y + 24 + (i / 4) * 26, r.w / 4, 22};
        if (ui_checkbox(ui, cb, widget_titles[i], &on))
            widget_toggle(widget_names[i], on);
    }
    y += 24 + 2 * 26 + 6;
    bool disk = is_on("desktopdisk");
    if (ui_checkbox(ui, (struct rect){r.x, y, r.w, 22}, "Show SynthOS HD and other disks on the desktop", &disk))
        apply("desktopdisk", disk ? "on" : "off");
    bool back;
    if (nav(ui, s, "Continue", &back))
        go(s, SP_DOCK);
    if (back)
        go(s, SP_LOOK);
}

static void dock_preview(struct ui *ui, struct rect pv)
{
    if (!ui->c)
        return;
    const struct canvas *wp = desktop_wallpaper(0);
    if (wp && wp->px) {
        struct canvas shot;
        if (canvas_alloc(&shot, pv.w, pv.h)) {
            gfx_scale(&shot, (struct rect){0, 0, pv.w, pv.h}, wp, 255);
            gfx_blit_round(ui->c, pv, &shot, 10, false);
            canvas_free(&shot);
        }
    } else {
        gfx_round(ui->c, pv, 10, style.desktop);
    }
    gfx_round_frame(ui->c, pv, 10, ALPHA(0, 40));
    gfx_fill(ui->c, (struct rect){pv.x + 4, pv.y + 3, pv.w - 8, 12}, ALPHA(style.dark ? 0 : 0xffffffffu, 120));
    char when[40];
    ksnprintf(when, sizeof(when), "%s%s", is_on("clockdate") ? "Thu 12 Nov  " : "",
              is_on("clock24") ? (is_on("clockseconds") ? "21:41:07" : "21:41")
                               : (is_on("clockseconds") ? "9:41:07 PM" : "9:41 PM"));
    gfx_text_box(ui->c, (struct rect){pv.x + 8, pv.y + 2, pv.w - 16, 14}, &font_small, when, style.text, ALIGN_RIGHT);
    char mode[16], size[16];
    current("dockmode", mode, sizeof(mode));
    current("docksize", size, sizeof(size));
    if (!strcmp(mode, "off"))
        return;
    int icon = !strcmp(size, "small") ? 12 : !strcmp(size, "large") ? 20 : 16, n = 7;
    int dw = n * (icon + 4) + 10, dh = icon + 10;
    struct rect dock = {pv.x + (pv.w - dw) / 2, pv.y + pv.h - dh - 6, dw, dh};
    if (!strcmp(mode, "autohide")) {
        gfx_fill(ui->c, (struct rect){pv.x + pv.w / 2 - 20, pv.y + pv.h - 4, 40, 2}, ALPHA(0xffffffffu, 160));
        return;
    }
    gfx_round(ui->c, dock, dh / 3, ALPHA(style.dark ? 0 : 0xffffffffu, 140));
    static const uint32_t colors[] = {0xff0a84ff, 0xff30d158, 0xffff9f0a, 0xffff375f, 0xffbf5af2, 0xff64d2ff,
                                      0xff8e8e93};
    for (int i = 0; i < n; i++) {
        int sz = icon;
        if (is_on("magnify") && i == 3)
            sz = icon * 3 / 2;
        gfx_round(ui->c, (struct rect){dock.x + 7 + i * (icon + 4) - (sz - icon) / 2, dock.y + dock.h - 5 - sz, sz, sz},
                  sz / 4, colors[i]);
    }
}

static void dock_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "Dock and menu bar", "Where your apps wait and what the menu bar shows.");
    dock_preview(ui, (struct rect){r.x + (r.w - 300) / 2, r.y + 60, 300, 130});
    int y = r.y + 206;
    static const char *const modes[] = {"Always shown", "Hide automatically", "Off"};
    static const char *const mode_values[] = {"show", "autohide", "off"};
    choice_row(ui, r, y, "Dock", "dockmode", modes, mode_values, 3);
    static const char *const sizes[] = {"Small", "Medium", "Large"};
    static const char *const size_values[] = {"small", "medium", "large"};
    choice_row(ui, r, y + 40, "Icon size", "docksize", sizes, size_values, 3);
    struct rect g = ui_group(ui, r.x, y + 84, r.w, 2, ROW_H);
    switch_row(ui, g, 0, 2, "Magnify icons under the pointer", "magnify");
    switch_row(ui, g, 1, 2, "Show seconds in the menu bar clock", "clockseconds");
    bool back;
    if (nav(ui, s, "Continue", &back))
        go(s, SP_SYSTEM);
    if (back)
        go(s, SP_DESKTOP);
}

static void system_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    title(ui, r, "How it works", "Shortcuts, comfort and privacy.");
    int y = r.y + 64;
    static const char *const keys[] = {"Windows + Mac", "Windows", "Mac"};
    static const char *const key_values[] = {"both", "windows", "mac"};
    choice_row(ui, r, y, "Keyboard shortcuts", "shortcuts", keys, key_values, 3);
    static const char *const nights[] = {"Off", "Evenings", "Always"};
    static const char *const night_values[] = {"off", "auto", "on"};
    choice_row(ui, r, y + 40, "Night Shift", "nightshift", nights, night_values, 3);
    static const char *const locks[] = {"Never", "5 min", "15 min", "1 hour"};
    static const char *const lock_values[] = {"0", "5", "15", "60"};
    choice_row(ui, r, y + 80, "Lock when idle", "locktime", locks, lock_values, 4);
    struct rect g = ui_group(ui, r.x, y + 126, r.w, 4, ROW_H);
    switch_row(ui, g, 0, 4, "Animations", "animations");
    switch_row(ui, g, 1, 4, "Natural scrolling", "naturalscroll");
    switch_row(ui, g, 2, 4, "Look for updates once a day", "autoupdate");
    switch_row(ui, g, 3, 4, "Do Not Disturb", "dnd");
    bool back;
    if (nav(ui, s, "Continue", &back))
        go(s, SP_DONE);
    if (back)
        go(s, SP_DOCK);
}

static bool finish(struct setup *s)
{
    int err = user_add(s->name.buf, s->full.buf, s->pass.buf, true);
    if (err) {
        ksnprintf(s->error, sizeof(s->error), "The account cannot be made: %s", err_str(err));
        return false;
    }
    char list[160] = "";
    for (int i = 0; i < s->norder; i++) {
        if (list[0])
            str_append(list, ",", sizeof(list));
        str_append(list, keymap_layout_name(s->order[i], NULL), sizeof(list));
    }
    if (list[0])
        apply("keyboard", list);
    apply("autologin", s->autologin ? s->name.buf : "off");
    settings_save(SETTINGS_FILE);
    vfs_write_all(SETUP_DONE, "1\n", 2);
    char name[USER_NAME_MAX];
    str_copy(name, s->name.buf, sizeof(name));
    wm_destroy(setup_win);
    gui_session_login(name);
    return true;
}

static void summary_row(struct ui *ui, struct rect r, int i, const char *label, const char *value)
{
    if (!ui->c)
        return;
    int y = r.y + i * 22;
    gfx_text_box(ui->c, (struct rect){r.x, y, r.w / 2 - 10, 20}, &font_sans, label, style.secondary, ALIGN_RIGHT);
    gfx_text_box(ui->c, (struct rect){r.x + r.w / 2 + 10, y, r.w / 2 - 10, 20}, &font_bold, value, style.text,
                 ALIGN_LEFT);
}

static void done_page(struct ui *ui, struct setup *s)
{
    struct rect r = body(ui, s);
    if (ui->c) {
        int cx = r.x + r.w / 2, cy = r.y + 34;
        gfx_circle(ui->c, cx, cy, 30, style.ok);
        gfx_symbol(ui->c, cx - 15, cy - 15, SYM_CHECK, 30, RGB(255, 255, 255));
    }
    char hello[96];
    ksnprintf(hello, sizeof(hello), "All set, %s", s->full.buf);
    ui_label(ui, (struct rect){r.x, r.y + 76, r.w, 30}, hello, &font_title, style.text, ALIGN_CENTER);
    struct rect list = {r.x, r.y + 120, r.w, 200};
    char langs[120] = "", v[PATH_MAX_LEN], line[160];
    for (int i = 0; i < s->norder; i++) {
        const char *name;
        keymap_layout_name(s->order[i], &name);
        if (langs[0])
            str_append(langs, ", ", sizeof(langs));
        str_append(langs, name, sizeof(langs));
    }
    summary_row(ui, list, 0, "Languages", langs);
    ksnprintf(line, sizeof(line), "%s, %s clock", zones[s->zone].city, is_on("clock24") ? "24-hour" : "12-hour");
    summary_row(ui, list, 1, "Region", line);
    ksnprintf(line, sizeof(line), "%s (%s)%s", s->name.buf, s->pass.len ? "password" : "no password",
              s->autologin ? ", logs in automatically" : "");
    summary_row(ui, list, 2, "Account", line);
    static const char *const modes[3] = {"Auto", "Light", "Dark"};
    ksnprintf(line, sizeof(line), "%s, %s, accent %s", look_get(s->look)->title, modes[s->appearance],
              accents[s->accent]);
    summary_row(ui, list, 3, "Look", line);
    current("wallpaper", v, sizeof(v));
    const char *wall = v;
    for (int i = 0; i < WALLS; i++)
        if (!strcmp(v, wall_values[i]))
            wall = i ? wall_names[i] : "From the look";
    summary_row(ui, list, 4, "Wallpaper", wall);
    char mode[16], size[16];
    current("dockmode", mode, sizeof(mode));
    current("docksize", size, sizeof(size));
    ksnprintf(line, sizeof(line), "%s, %s icons", !strcmp(mode, "show") ? "always shown" : !strcmp(mode, "off") ? "off" : "hides itself",
              size);
    summary_row(ui, list, 5, "Dock", line);
    current("shortcuts", v, sizeof(v));
    summary_row(ui, list, 6, "Shortcuts", !strcmp(v, "both") ? "Windows and Mac" : !strcmp(v, "mac") ? "Mac" : "Windows");
    ui_wrap_text(ui->c, (struct rect){r.x + 30, r.y + 290, r.w - 60, 60}, &font_sans,
                 "Get more apps in the App Store, change anything in System Settings, and find every app in Launchpad.",
                 style.secondary);
    if (s->error[0])
        ui_label(ui, (struct rect){r.x, r.y + 350, r.w, 18}, s->error, &font_sans, style.error, ALIGN_CENTER);
    bool back;
    if (nav(ui, s, "Start", &back))
        finish(s);
    if (back)
        go(s, SP_SYSTEM);
}

static void frame(struct ui *ui, void *data)
{
    struct setup *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    struct rect card = card_rect(ui);
    if (ui->c) {
        if (s->backdrop.px)
            gfx_scale(ui->c, (struct rect){0, 0, W, H}, &s->backdrop, 255);
        else
            gfx_clear(ui->c, (struct rect){0, 0, W, H}, style.desktop);
        gfx_fill(ui->c, (struct rect){0, 0, W, H}, ALPHA(0, 40));
        gfx_drop_shadow(ui->c, card, 22, 30, 90, 10);
        gfx_round(ui->c, card, 22, style.content);
    }
    switch (s->page) {
    case SP_LIVE: live_page(ui, s); break;
    case SP_HELLO: hello_page(ui, s); break;
    case SP_REGION: region_page(ui, s); break;
    case SP_ACCOUNT: account_page(ui, s); break;
    case SP_LANGUAGE: language_page(ui, s); break;
    case SP_LOOK: look_page(ui, s); break;
    case SP_DESKTOP: desktop_page(ui, s); break;
    case SP_DOCK: dock_page(ui, s); break;
    case SP_SYSTEM: system_page(ui, s); break;
    default: done_page(ui, s); break;
    }
}

static void paint(struct window *w, struct canvas *c)
{
    struct setup *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct setup *s = w->data;
    if (e->type == EV_TIMER) {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - s->tick, 64u);
        s->tick = now;
        if (!spring_step(&s->slide, 300, 30, dt))
            wm_set_timer(w, 0);
        wm_invalidate(w);
        return;
    }
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    struct setup *s = w->data;
    canvas_free(&s->backdrop);
    for (int i = 0; i < WALLS; i++)
        canvas_free(&s->thumbs[i]);
    memset(&s->pass, 0, sizeof(s->pass));
    memset(&s->pass2, 0, sizeof(s->pass2));
    kfree(s);
    if (setup_win == w)
        setup_win = NULL;
    chrome_changed();
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static int current_zone(void)
{
    const struct setting *st = setting_find("timezone");
    char value[16] = "";
    if (st)
        st->get(value, sizeof(value));
    for (int i = 0; i < (int)ARRAY_SIZE(zones); i++)
        if (!strcmp(value, zones[i].offset))
            return i;
    return 11;
}

static void open_setup(enum page first)
{
    if (setup_win)
        return;
    wm_close_popups();
    struct setup *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    s->page = first;
    s->norder = keymap_order(s->order, 32);
    for (int i = 0; i < s->norder; i++)
        if (s->order[i] >= 0 && s->order[i] < 32)
            s->layouts[s->order[i]] = true;
    if (!s->layouts[0] && s->norder < 32) {
        s->layouts[0] = true;
        s->order[s->norder++] = 0;
    }
    s->zone = current_zone();
    s->tick = hal_ticks_ms();
    s->pass.password = s->pass2.password = true;
    for (int i = 0; i < look_count(); i++)
        if (look_get(i) == look_current())
            s->look = i;
    gui_blurred_wallpaper(&s->backdrop);
    struct rect sr = wm_screen_rect(0);
    setup_win = wm_popup(sr.x, sr.y, sr.w, sr.h, &ops, s, NULL);
    if (!setup_win) {
        canvas_free(&s->backdrop);
        kfree(s);
        return;
    }
    setup_win->flags &= ~WIN_POPUP;
    setup_win->flags |= WIN_OVERLAY | WIN_NOSHADOW;
    if (setup_win->glass) {
        glass_unregister(setup_win->glass);
        kfree(setup_win->glass);
        setup_win->glass = NULL;
    }
    setup_win->radius = 0;
    wm_move(setup_win, sr.x, sr.y);
}

void gui_setup(void)
{
    if (hal_installed())
        apply("clock24", "on");
    open_setup(hal_installed() ? SP_HELLO : SP_LIVE);
}

COMMAND(welcome, NULL, "system", "welcome [live]", "run the first-start assistant: languages, region, account, look, desktop, Dock, behaviour")
{
    if (!gui_active())
        return sh_error(sh, "welcome", "start the desktop first");
    open_setup(argc > 1 && !strcmp(argv[1], "live") ? SP_LIVE : SP_HELLO);
    return 0;
}
