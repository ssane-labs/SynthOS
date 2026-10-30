#include <synth/bundle.h>
#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/theme.h>
#include <synth/time.h>
#include <synth/tls.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winshell.h>

#include <synth/keys.h>

#include "../internal.h"

/* System Settings: a sidebar of panes and, on the right, groups of rows - a label (and a line of explanation) on the
   left, the control on the right. Each pane is drawn twice per frame: a measuring pass finds how tall its groups are
   (and, while searching, which rows match), then the real pass draws them and handles the input. */

#define SIDEBAR_W 236
#define SIDE_ROW 26
#define ROW 40
#define ROW_DESC 54
#define GAP 24
#define SECTIONS 32
#define HISTORY 16
#define WALLS 11
#define LOOK_W 140
#define LOOK_H 88
#define THUMB_W 124
#define THUMB_H 78

struct settings;

struct page {
    struct ui *ui;
    struct settings *s;
    int x, y, w;
    bool measure;
    const char *query;
    int hits;
    int sec, rows, sec_y;
    int *sec_h, *sec_rows;
};

typedef void (*pane_fn)(struct page *p);

enum pane {
    P_NETWORK,
    P_NOTIFY,
    P_SOUND,
    P_GENERAL,
    P_ACCESS,
    P_APPEARANCE,
    P_CONTROL,
    P_DOCK,
    P_DISPLAYS,
    P_WALLPAPER,
    P_LOCK,
    P_PRIVACY,
    P_USERS,
    P_KEYBOARD,
    P_MOUSE,
    P_APPS,
    P_TERMINAL,
    P_ABOUT,
    P_STORAGE,
    P_DATETIME,
    P_LOGIN,
    P_COUNT,
};

static void pane_network(struct page *p);
static void pane_notify(struct page *p);
static void pane_sound(struct page *p);
static void pane_general(struct page *p);
static void pane_access(struct page *p);
static void pane_appearance(struct page *p);
static void pane_control(struct page *p);
static void pane_dock(struct page *p);
static void pane_displays(struct page *p);
static void pane_wallpaper(struct page *p);
static void pane_lock(struct page *p);
static void pane_privacy(struct page *p);
static void pane_users(struct page *p);
static void pane_keyboard(struct page *p);
static void pane_mouse(struct page *p);
static void pane_apps(struct page *p);
static void pane_terminal(struct page *p);
static void pane_about(struct page *p);
static void pane_storage(struct page *p);
static void pane_datetime(struct page *p);
static void pane_login(struct page *p);
static void browser_popup(struct page *p);

/* Every setting has one home, the way macOS and Windows arrange them; other panes only link to it. */
static const struct {
    const char *name;
    const char *key;
    enum symbol_id sym;
    uint32_t color;
    int group;
    int parent;
    pane_fn fn;
} panes[P_COUNT] = {
    {"Network", "network", SYM_WIFI, RGB(28, 118, 240), 0, -1, pane_network},
    {"Notifications", "notifications", SYM_BELL, RGB(240, 64, 64), 1, -1, pane_notify},
    {"Sound", "sound", SYM_VOLUME, RGB(236, 64, 112), 1, -1, pane_sound},
    {"General", "general", SYM_GEAR, RGB(142, 142, 148), 2, -1, pane_general},
    {"Accessibility", "accessibility", SYM_ACCESS, RGB(28, 118, 240), 2, -1, pane_access},
    {"Appearance", "appearance", SYM_APPEARANCE, RGB(44, 44, 48), 2, -1, pane_appearance},
    {"Control Center", "control", SYM_CONTROL, RGB(142, 142, 148), 2, -1, pane_control},
    {"Desktop & Dock", "dock", SYM_DOCK, RGB(44, 44, 48), 2, -1, pane_dock},
    {"Displays", "displays", SYM_DISPLAY, RGB(28, 118, 240), 2, -1, pane_displays},
    {"Wallpaper", "wallpaper", SYM_PHOTO, RGB(36, 168, 220), 2, -1, pane_wallpaper},
    {"Lock Screen", "lock", SYM_LOCK, RGB(44, 44, 48), 3, -1, pane_lock},
    {"Privacy & Security", "privacy", SYM_HAND, RGB(28, 118, 240), 3, -1, pane_privacy},
    {"Users & Groups", "users", SYM_USERS, RGB(28, 118, 240), 3, -1, pane_users},
    {"Keyboard", "keyboard", SYM_KEYBOARD, RGB(142, 142, 148), 4, -1, pane_keyboard},
    {"Mouse", "mouse", SYM_MOUSE, RGB(142, 142, 148), 4, -1, pane_mouse},
    {"Apps", "apps", SYM_APPS, RGB(40, 140, 220), 4, -1, pane_apps},
    {"Terminal", "terminal", SYM_TERMINAL, RGB(60, 60, 64), 4, -1, pane_terminal},
    {"About", "about", SYM_INFO, RGB(142, 142, 148), 2, P_GENERAL, pane_about},
    {"Storage", "storage", SYM_DISK, RGB(142, 142, 148), 2, P_GENERAL, pane_storage},
    {"Date & Time", "datetime", SYM_CLOCK, RGB(28, 118, 240), 2, P_GENERAL, pane_datetime},
    {"Login Items", "login", SYM_LIST, RGB(142, 142, 148), 2, P_GENERAL, pane_login},
};

static const char *const wall_names[WALLS] = {"From the look", "Mono", "Dune", "Mist", "Night", "Sunset",
                                              "Waves", "Hills", "Aurora", "Synthwave", "Solid"};
static const char *const wall_values[WALLS] = {"auto", "mono", "dune", "mist", "night", "sunset",
                                               "waves", "hills", "aurora", "synthwave", "solid"};

struct settings {
    struct ui_state st;
    int pane;
    int scroll, side_scroll, height;
    int back[HISTORY], back_n, forward[HISTORY], forward_n;
    struct ui_text search;
    int sec_h[SECTIONS], sec_rows[SECTIONS];
    char themes[48][24];
    int theme_count;
    struct ui_list theme_list;
    struct ui_text wallpaper_file, wallpaper_url, login_item, computer;
    struct canvas thumbs[WALLS];
    bool thumbs_dark;
    const struct gui_look *thumbs_look;
    struct canvas look_thumbs[8];
    char pending_user[USER_NAME_MAX];
    char popup_key[24];
    const char *const *popup_values;
};

static struct window *instance;

static void apply(const char *name, const char *value)
{
    gui_setting(name, value);
}

static void current(const char *name, char *buf, size_t cap)
{
    gui_setting_get(name, buf, cap);
}

static bool is_on(const char *name)
{
    return gui_setting_on(name);
}

static long number(const char *name, long fallback)
{
    return gui_setting_number(name, fallback);
}

static void apply_number(const char *name, int v)
{
    char buf[12];
    ksnprintf(buf, sizeof(buf), "%d", v);
    apply(name, buf);
}

static bool found(const char *query, const char *text)
{
    size_t n = strlen(query);
    if (!n || !text)
        return false;
    for (const char *p = text; *p; p++)
        if (!str_ncasecmp(p, query, n))
            return true;
    return false;
}

/* Text that wraps at word boundaries; draws when c is set and returns the number of lines. */
static int wrap(struct canvas *c, struct rect r, const struct ui_font *f, const char *text, uint32_t color,
                enum text_align align)
{
    int lines = 0;
    const char *p = text;
    while (*p) {
        char line[160];
        size_t len = 0, fit = 0;
        while (p[len] && len < sizeof(line) - 1) {
            size_t end = len;
            while (p[end] && p[end] != ' ' && end < sizeof(line) - 1)
                end++;
            memcpy(line, p, end);
            line[end] = 0;
            if (fit && gfx_text_width(f, line) > r.w)
                break;
            fit = len = end;
            if (p[len] == ' ')
                len++;
            if (!p[end])
                break;
        }
        if (!fit)
            fit = len = MAX(len, (size_t)1);
        memcpy(line, p, fit);
        line[fit] = 0;
        if (c)
            gfx_text_box(c, (struct rect){r.x, r.y + lines * (f->height + 3), r.w, f->height + 3}, f, line, color,
                         align);
        lines++;
        p += len;
        while (*p == ' ')
            p++;
    }
    return lines;
}

/* ---- layout: groups and rows */

static void section(struct page *p, const char *title)
{
    if (title) {
        if (!p->measure)
            ui_label(p->ui, (struct rect){p->x + 2, p->y, p->w, 20}, title, &font_bold, style.text, ALIGN_LEFT);
        if (p->query && found(p->query, title))
            p->hits++;
        p->y += 26;
    }
    p->sec_y = p->y;
    p->rows = 0;
    if (p->measure || !p->ui->c || p->sec >= SECTIONS || p->sec_h[p->sec] <= 0)
        return;
    struct rect g = {p->x, p->y, p->w, p->sec_h[p->sec]};
    gfx_round(p->ui->c, g, 10, style.group);
    gfx_round_frame(p->ui->c, g, 10, style.dark ? ALPHA(0xffffffffu, 16) : ALPHA(0, 16));
}

static void end(struct page *p, const char *footer)
{
    if (p->measure && p->sec < SECTIONS) {
        p->sec_h[p->sec] = p->y - p->sec_y;
        p->sec_rows[p->sec] = p->rows;
    }
    p->sec++;
    if (footer) {
        p->y += 6;
        struct rect r = {p->x + 4, p->y, p->w - 8, 0};
        int lines = wrap(p->measure ? NULL : p->ui->c, r, &font_small, footer, style.secondary, ALIGN_LEFT);
        p->y += lines * (font_small.height + 3);
    }
    p->y += GAP;
}

static struct rect ctl(struct rect r, int w, int h)
{
    return (struct rect){r.x + r.w - 14 - w, r.y + (r.h - h) / 2, w, h};
}

/* A row of the current group; cw is how wide its control is, so the label stops before it. */
static struct rect row(struct page *p, const char *label, const char *desc, int h, int cw)
{
    if (!h)
        h = desc ? ROW_DESC : ROW;
    struct rect r = {p->x, p->y, p->w, h};
    bool last = p->sec < SECTIONS && p->rows + 1 >= p->sec_rows[p->sec];
    p->rows++;
    p->y += h;
    bool hit = p->query && (found(p->query, label) || found(p->query, desc));
    if (p->measure) {
        p->hits += hit;
        return r;
    }
    struct canvas *c = p->ui->c;
    if (!c)
        return r;
    if (hit)
        gfx_round(c, rect_inset(r, 3), 8, ALPHA(style.accent, style.dark ? 70 : 46));
    int tw = r.w - 28 - (cw ? cw + 14 : 0);
    if (label && desc) {
        gfx_text_box(c, (struct rect){r.x + 14, r.y + 9, tw, 18}, &font_sans, label, style.text, ALIGN_LEFT);
        wrap(c, (struct rect){r.x + 14, r.y + 28, tw, 0}, &font_small, desc, style.secondary, ALIGN_LEFT);
    } else if (label) {
        gfx_text_box(c, (struct rect){r.x + 14, r.y, tw, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
    }
    if (!last)
        gfx_fill(c, (struct rect){r.x + 14, r.y + r.h - 1, r.w - 28, 1}, style.separator);
    return r;
}

/* A row the pane draws itself; words make it show up in searches. */
static struct rect custom(struct page *p, int h, const char *words)
{
    struct rect r = row(p, NULL, NULL, h, 0);
    if (p->query && found(p->query, words)) {
        if (p->measure)
            p->hits++;
        else if (p->ui->c)
            gfx_round(p->ui->c, rect_inset(r, 3), 8, ALPHA(style.accent, style.dark ? 40 : 26));
    }
    return r;
}

static bool toggle(struct page *p, const char *label, const char *desc, bool *value)
{
    struct rect r = row(p, label, desc, 0, 36);
    return !p->measure && ui_switch(p->ui, ctl(r, 36, 21), value);
}

static void toggle_setting(struct page *p, const char *label, const char *desc, const char *key)
{
    bool on = is_on(key);
    if (toggle(p, label, desc, &on))
        apply(key, on ? "on" : "off");
}

static void choice(struct page *p, const char *label, const char *desc, const char *key,
                   const char *const *labels, const char *const *values, int n, int width)
{
    struct rect r = row(p, label, desc, 0, width);
    if (p->measure)
        return;
    char v[48];
    current(key, v, sizeof(v));
    int sel = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(v, values[i]))
            sel = i;
    if (ui_segmented(p->ui, ctl(r, width, 24), labels, n, &sel))
        apply(key, values[sel]);
}

static struct menu_item popup_items[24];

static void popup_pick(struct window *owner, int index)
{
    struct settings *s = wm_window_alive(owner) ? owner->data : NULL;
    if (s && s->popup_values && s->popup_key[0])
        apply(s->popup_key, s->popup_values[index]);
    if (s)
        wm_invalidate(owner);
}

/* A pop-up button: the current choice and a chevron; a click opens the list of choices under it. */
static void popup(struct page *p, const char *label, const char *desc, const char *key, const char *const *labels,
                  const char *const *values, int n)
{
    int width = 0;
    for (int i = 0; i < n; i++)
        width = MAX(width, gfx_text_width(&font_sans, labels[i]));
    width = MIN(width + 40, p->w / 2);
    struct rect r = row(p, label, desc, 0, width);
    if (p->measure)
        return;
    char v[48];
    current(key, v, sizeof(v));
    const char *shown = v;
    for (int i = 0; i < n; i++)
        if (!strcmp(v, values[i]))
            shown = labels[i];
    struct rect b = ctl(r, width, 24);
    bool hot = ui_hover(p->ui, b);
    if (p->ui->c) {
        gfx_round(p->ui->c, b, 6, style.dark ? ALPHA(0xffffffffu, hot ? 34 : 22) : ALPHA(0, hot ? 18 : 10));
        gfx_text_box(p->ui->c, (struct rect){b.x + 10, b.y, b.w - 34, b.h}, &font_sans, shown, style.text, ALIGN_LEFT);
        gfx_symbol(p->ui->c, b.x + b.w - 20, b.y + 4, SYM_DOWN, 16, style.secondary);
    }
    if (p->ui->e && p->ui->e->type == EV_MOUSE_DOWN && hot) {
        p->ui->consumed = true;
        n = MIN(n, (int)ARRAY_SIZE(popup_items));
        for (int i = 0; i < n; i++)
            popup_items[i] = (struct menu_item){labels[i], 0, NULL};
        str_copy(p->s->popup_key, key, sizeof(p->s->popup_key));
        p->s->popup_values = values;
        ui_menu_items(p->ui->win, b.x, b.y + b.h + 2, popup_items, n, popup_pick);
    }
}

static bool slider(struct page *p, const char *label, const char *desc, int *value, int min, int max,
                   const char *lo, const char *hi)
{
    int width = MIN(240, p->w / 2 - 20);
    struct rect r = row(p, label, desc, lo ? MAX(desc ? ROW_DESC : 0, 56) : 0, width);
    if (p->measure)
        return false;
    struct rect s = ctl(r, width, 20);
    if (lo)
        s.y -= 7;
    bool changed = ui_slider(p->ui, s, value, min, max);
    if (lo && p->ui->c) {
        gfx_text_box(p->ui->c, (struct rect){s.x - 4, s.y + 20, width / 2, 14}, &font_small, lo, style.secondary,
                     ALIGN_LEFT);
        gfx_text_box(p->ui->c, (struct rect){s.x + width / 2, s.y + 20, width / 2 + 4, 14}, &font_small, hi,
                     style.secondary, ALIGN_RIGHT);
    }
    return changed;
}

static void info(struct page *p, const char *label, const char *value)
{
    int width = MIN(gfx_text_width(&font_sans, value), p->w - 200);
    struct rect r = row(p, label, NULL, 0, width);
    if (p->query && found(p->query, value))
        p->hits += p->measure;
    if (!p->measure && p->ui->c)
        gfx_text_box(p->ui->c, ctl(r, width, r.h), &font_sans, value, style.secondary, ALIGN_RIGHT);
}

static bool button(struct page *p, const char *label, const char *desc, const char *text)
{
    int width = gfx_text_width(&font_sans, text) + 28;
    struct rect r = row(p, label, desc, 0, width);
    return !p->measure && ui_button(p->ui, ctl(r, width, 26), text);
}

static bool field(struct page *p, const char *label, const char *desc, struct ui_text *t, const char *hint, int width)
{
    struct rect r = row(p, label, desc, 0, width);
    return !p->measure && ui_textbox_hint(p->ui, ctl(r, width, 26), t, hint);
}

/* A row that leads somewhere else: a small icon, the name, a detail and a chevron. */
static bool link(struct page *p, enum symbol_id sym, uint32_t color, const char *label, const char *detail)
{
    struct rect r = row(p, NULL, NULL, 44, 0);
    if (p->query && found(p->query, label))
        p->hits += p->measure;
    if (p->measure)
        return false;
    bool hot = ui_hover(p->ui, r);
    if (p->ui->c) {
        if (p->query && found(p->query, label))
            gfx_round(p->ui->c, rect_inset(r, 3), 8, ALPHA(style.accent, style.dark ? 70 : 46));
        ui_settings_icon(p->ui->c, r.x + 12, r.y + 10, 24, sym, color);
        gfx_text_box(p->ui->c, (struct rect){r.x + 48, r.y, r.w / 2, r.h}, &font_sans, label, style.text, ALIGN_LEFT);
        if (detail)
            gfx_text_box(p->ui->c, (struct rect){r.x + r.w / 2, r.y, r.w / 2 - 36, r.h}, &font_sans, detail,
                         style.secondary, ALIGN_RIGHT);
        gfx_symbol(p->ui->c, r.x + r.w - 26, r.y + 14, SYM_RIGHT, 16, style.tertiary);
    }
    if (p->ui->e && p->ui->e->type == EV_MOUSE_DOWN && hot) {
        p->ui->consumed = true;
        return true;
    }
    return false;
}

/* The card at the top of some panes: a big icon, the name and what the pane is for. */
static void header(struct page *p, enum symbol_id sym, uint32_t color, const char *title, const char *text)
{
    section(p, NULL);
    int lines = wrap(NULL, (struct rect){0, 0, p->w - 80, 0}, &font_small, text, 0, ALIGN_CENTER);
    struct rect r = row(p, NULL, NULL, 128 + lines * (font_small.height + 3), 0);
    if (p->query && (found(p->query, title) || found(p->query, text)))
        p->hits += p->measure;
    if (!p->measure && p->ui->c) {
        ui_settings_icon(p->ui->c, r.x + (r.w - 56) / 2, r.y + 18, 56, sym, color);
        gfx_text_box(p->ui->c, (struct rect){r.x, r.y + 84, r.w, 24}, &font_title, title, style.text, ALIGN_CENTER);
        wrap(p->ui->c, (struct rect){r.x + 40, r.y + 112, r.w - 80, 0}, &font_small, text, style.secondary,
             ALIGN_CENTER);
    }
    end(p, NULL);
}

static void go(struct settings *s, int pane)
{
    if (pane == s->pane)
        return;
    if (s->back_n == HISTORY)
        memmove(s->back, s->back + 1, sizeof(s->back[0]) * (HISTORY - 1)), s->back_n--;
    s->back[s->back_n++] = s->pane;
    s->forward_n = 0;
    s->pane = pane;
    s->scroll = 0;
}

/* ---- Network */

static void pane_network(struct page *p)
{
    if (!net_interface_count()) {
        header(p, SYM_WIFI, panes[P_NETWORK].color, "Network",
               "No network hardware was found. SynthOS works with Intel e1000/e1000e and Realtek RTL8139/RTL8169 "
               "cards (QEMU's default card is an e1000).");
        return;
    }
    for (int i = 0; i < net_interface_count(); i++) {
        struct net_iface *ifc = net_interface(i);
        struct netdev *d = ifc->dev;
        bool link_up = !d->link || d->link(d);
        char title[48], ip[16], mask[16], gw[16], dns[16], mac[24];
        ksnprintf(title, sizeof(title), "Ethernet %d", i + 1);
        net_format_ip(ifc->ip, ip, sizeof(ip));
        net_format_ip(ifc->mask, mask, sizeof(mask));
        net_format_ip(ifc->gateway, gw, sizeof(gw));
        net_format_ip(ifc->dns, dns, sizeof(dns));
        ksnprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", d->mac[0], d->mac[1], d->mac[2], d->mac[3],
                  d->mac[4], d->mac[5]);
        const char *status = !link_up ? "Cable unplugged" : ifc->configured ? "Connected" : "Getting an address";
        section(p, NULL);
        struct rect r = row(p, title, status, 60, 0);
        if (!p->measure && p->ui->c) {
            uint32_t dot = ifc->configured && link_up ? RGB(52, 199, 89)
                           : link_up                    ? RGB(255, 179, 0)
                                                        : RGB(255, 69, 58);
            gfx_circle(p->ui->c, r.x + r.w - 26, r.y + r.h / 2, 5, dot);
            char drv[48];
            ksnprintf(drv, sizeof(drv), "%s · %s", d->name, d->driver);
            gfx_text_box(p->ui->c, (struct rect){r.x + r.w / 2, r.y, r.w / 2 - 44, r.h}, &font_small, drv,
                         style.tertiary, ALIGN_RIGHT);
        }
        end(p, NULL);
        section(p, "TCP/IP");
        info(p, "Configure IPv4", ifc->dhcp ? "Using DHCP" : "Manually");
        info(p, "IP address", ifc->configured ? ip : "-");
        info(p, "Subnet mask", mask);
        info(p, "Router", gw);
        info(p, "DNS server", dns);
        info(p, "Hardware address", mac);
        if (button(p, "Ask the network for a new address", NULL, "Renew DHCP Lease")) {
            net_dhcp_start(ifc);
            gui_notify(d->name, "Asking the network for a new address.");
        }
        end(p, NULL);
    }
}

/* ---- Notifications and Sound */

static void pane_notify(struct page *p)
{
    header(p, SYM_BELL, panes[P_NOTIFY].color, "Notifications",
           "Banners appear in the top right corner for a few seconds; every notification stays in Notification "
           "Center, which opens with a click on the clock.");
    section(p, "Focus");
    toggle_setting(p, "Do Not Disturb", "Banners stay hidden; they still land in Notification Center", "dnd");
    end(p, "Control Center and the moon in the menu bar turn it on and off too.");
    section(p, NULL);
    if (button(p, "See how a notification looks", NULL, "Show Example"))
        gui_notify("Notifications", "This is how a notification looks.");
    end(p, NULL);
}

static void pane_sound(struct page *p)
{
    section(p, "Output");
    int v = (int)number("volume", 100);
    if (slider(p, "Output volume", NULL, &v, 0, 100, NULL, NULL))
        apply_number("volume", v);
    toggle_setting(p, "Mute", NULL, "mute");
    const char *dev = hal_audio_name();
    info(p, "Output device", dev && *dev ? dev : "Built-in sound (AC'97)");
    end(p, "Music, Video and the built-in songs play through the AC'97 sound card QEMU and many PCs have.");
}

/* ---- General and its pages */

static void pane_general(struct page *p)
{
    struct settings *s = p->s;
    header(p, SYM_GEAR, panes[P_GENERAL].color, "General",
           "The name of this computer, its storage, date and time, and what opens when you log in.");
    section(p, NULL);
    static const int pages[] = {P_ABOUT, P_STORAGE, P_DATETIME, P_LOGIN};
    for (size_t i = 0; i < ARRAY_SIZE(pages); i++)
        if (link(p, panes[pages[i]].sym, panes[pages[i]].color, panes[pages[i]].name, NULL))
            go(s, pages[i]);
    end(p, NULL);
    section(p, NULL);
    if (!p->measure && !s->computer.len && p->ui->st->focus == 0) {
        char name[48];
        current("hostname", name, sizeof(name));
        ui_text_set(&s->computer, name);
    }
    if (field(p, "Computer name", "Shown in the Terminal prompt and in About", &s->computer, "synth", 180) &&
        s->computer.len)
        apply("hostname", s->computer.buf);
    toggle_setting(p, "Start the desktop when SynthOS starts", NULL, "desktop");
    end(p, NULL);
    section(p, NULL);
    if (button(p, "Install SynthOS", "Copy SynthOS onto another disk of this computer", "Install..."))
        gui_app_launch(gui_app_find("Install SynthOS"), NULL);
    end(p, NULL);
}

static void pane_about(struct page *p)
{
    section(p, NULL);
    struct rect r = custom(p, 168, "SynthOS version");
    if (!p->measure && p->ui->c) {
        gfx_icon(p->ui->c, r.x + (r.w - 80) / 2, r.y + 16, ICON_LOGO, 80);
        gfx_text_box(p->ui->c, (struct rect){r.x, r.y + 104, r.w, 30}, &font_large, "SynthOS", style.text,
                     ALIGN_CENTER);
        gfx_text_box(p->ui->c, (struct rect){r.x, r.y + 138, r.w, 18}, &font_sans,
                     "Version " SYNTH_VERSION " \"" SYNTH_CODENAME "\"", style.secondary, ALIGN_CENTER);
    }
    end(p, NULL);
    struct heap_stats hs;
    heap_get_stats(&hs);
    char mem[48], used[16], total[16], screen[48], name[48], ip[16] = "-", up[32];
    fmt_size(used, sizeof(used), hs.used);
    fmt_size(total, sizeof(total), hs.total);
    ksnprintf(mem, sizeof(mem), "%s (%s in use)", total, used);
    struct rect a = wm_screen_rect(0);
    ksnprintf(screen, sizeof(screen), "%d x %d%s", a.w, a.h, wm_screen_count() > 1 ? " and more" : "");
    current("hostname", name, sizeof(name));
    if (net_interface_count() && net_interface(0)->configured)
        net_format_ip(net_interface(0)->ip, ip, sizeof(ip));
    uint64_t sec = hal_ticks_ms() / 1000;
    if (sec < 3600)
        ksnprintf(up, sizeof(up), "%u min", (unsigned)(sec / 60));
    else
        ksnprintf(up, sizeof(up), "%u h %u min", (unsigned)(sec / 3600), (unsigned)(sec % 3600 / 60));
    section(p, NULL);
    info(p, "Name", name);
    info(p, "Processor", hal_machine());
    info(p, "Architecture", hal_arch());
    info(p, "Memory", mem);
    info(p, "Display", screen);
    info(p, "Network address", ip);
    info(p, "Up for", up);
    info(p, "User", user_current()->full);
    end(p, NULL);
    section(p, NULL);
    if (button(p, "Memory, disks, windows and the kernel log", NULL, "System Report..."))
        gui_app_launch(gui_app_find("System Monitor"), NULL);
    end(p, NULL);
}

static void pane_storage(struct page *p)
{
    section(p, NULL);
    int n = 0;
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        uint64_t total = 0, free = 0;
        if (!m->fs->statfs || m->fs->statfs(m->root, &total, &free) != 0 || !total)
            continue;
        n++;
        const char *title = !strcmp(m->path, "/") ? "System (in memory)" : !strcmp(m->path, "/home") ? "SynthOS HD"
                                                                                                   : m->path;
        char line[96], a[16], b[16];
        fmt_size(a, sizeof(a), free);
        fmt_size(b, sizeof(b), total);
        ksnprintf(line, sizeof(line), "%s available of %s  ·  %s  ·  %s", a, b, m->fs->name, m->path);
        struct rect r = row(p, NULL, NULL, 72, 0);
        if (p->query && found(p->query, title))
            p->hits += p->measure;
        if (p->measure || !p->ui->c)
            continue;
        gfx_symbol(p->ui->c, r.x + 16, r.y + 14, SYM_DISK, 32, style.secondary);
        gfx_text_box(p->ui->c, (struct rect){r.x + 60, r.y + 10, r.w - 80, 18}, &font_bold, title, style.text,
                     ALIGN_LEFT);
        struct rect bar = {r.x + 60, r.y + 34, r.w - 80, 8};
        gfx_round(p->ui->c, bar, 4, style.separator);
        int used = (int)((total - free) * (uint64_t)bar.w / total);
        gfx_round(p->ui->c, (struct rect){bar.x, bar.y, MAX(used, 8), bar.h}, 4, style.accent);
        gfx_text_box(p->ui->c, (struct rect){r.x + 60, r.y + 48, r.w - 80, 16}, &font_small, line, style.secondary,
                     ALIGN_LEFT);
    }
    if (!n)
        info(p, "Disks", "None");
    end(p, "SynthOS HD keeps /home. Files in other places live in memory until the computer turns off.");
}

static const char *const zone_values[] = {"-08:00", "-07:00", "-06:00", "-05:00", "-03:00", "+00:00", "+01:00",
                                          "+02:00", "+03:00", "+04:00", "+05:00", "+05:30", "+06:00", "+07:00",
                                          "+08:00", "+09:00", "+10:00", "+11:00", "+12:00"};
static const char *const zone_labels[] = {
    "UTC-08:00  Los Angeles",    "UTC-07:00  Denver",       "UTC-06:00  Chicago, Mexico City",
    "UTC-05:00  New York",       "UTC-03:00  Sao Paulo",    "UTC  London, Lisbon",
    "UTC+01:00  Berlin, Paris",  "UTC+02:00  Athens, Kaliningrad", "UTC+03:00  Moscow, Istanbul",
    "UTC+04:00  Dubai, Samara",  "UTC+05:00  Yekaterinburg", "UTC+05:30  Delhi",
    "UTC+06:00  Omsk, Almaty",   "UTC+07:00  Novosibirsk, Bangkok", "UTC+08:00  Beijing, Irkutsk",
    "UTC+09:00  Tokyo, Yakutsk", "UTC+10:00  Vladivostok, Sydney", "UTC+11:00  Magadan",
    "UTC+12:00  Kamchatka, Auckland",
};

static void pane_datetime(struct page *p)
{
    struct rtc_time t;
    time_local(&t);
    char now[48], time[24];
    gui_clock_time(&t, time, sizeof(time), false, true);
    ksnprintf(now, sizeof(now), "%s %u %s %u, %s", time_weekday_name(time_weekday(&t)), t.day,
              time_month_name(t.month), t.year, time);
    section(p, NULL);
    info(p, "Date and time", now);
    popup(p, "Time zone", NULL, "timezone", zone_labels, zone_values, (int)ARRAY_SIZE(zone_values));
    end(p, "The computer's clock keeps universal time (UTC); clocks you read, date and cal add the time zone.");
    section(p, "Clock");
    toggle_setting(p, "24-hour time", NULL, "clock24");
    toggle_setting(p, "Show seconds", "In the menu bar and the status pill", "clockseconds");
    toggle_setting(p, "Show the date", "In the menu bar, before the time", "clockdate");
    end(p, NULL);
}

static void pane_login(struct page *p)
{
    struct settings *s = p->s;
    static char items[AUTOSTART_MAX][AUTOSTART_LEN];
    int n = autostart_items(items, AUTOSTART_MAX);
    section(p, "Open at Login");
    if (!n)
        row(p, "Nothing opens at login yet", NULL, 0, 0);
    for (int i = 0; i < n; i++) {
        const struct gui_app *app = gui_app_find(items[i]);
        bool is_app = app && !str_casecmp(app->name, items[i]);
        const char *name = items[i][0] == '!' ? items[i] + 1 : items[i];
        struct rect r = row(p, NULL, NULL, 44, 0);
        if (p->query && found(p->query, name))
            p->hits += p->measure;
        if (p->measure)
            continue;
        if (p->ui->c) {
            gfx_icon(p->ui->c, r.x + 12, r.y + 8, is_app ? app->icon : items[i][0] == '!' ? ICON_TERMINAL : ICON_FILE,
                     28);
            gfx_text_box(p->ui->c, (struct rect){r.x + 50, r.y, r.w - 110, r.h}, &font_sans, name, style.text,
                         ALIGN_LEFT);
        }
        if (ui_symbol_button(p->ui, ctl(r, 26, 26), SYM_MINUS, false))
            autostart_remove(i);
    }
    end(p, NULL);
    section(p, "Add");
    bool go_add = field(p, "App, file or !command", NULL, &s->login_item, "Music, /home/notes.txt, !date", 220);
    if (go_add && s->login_item.len) {
        autostart_add(s->login_item.buf);
        ui_text_set(&s->login_item, "");
    }
    static const char *const quick[] = {"Music", "Notepad", "Files", "Photos", "Terminal"};
    struct rect r = row(p, NULL, NULL, 48, 0);
    for (size_t i = 0; i < ARRAY_SIZE(quick) && !p->measure; i++) {
        struct rect b = {r.x + 14 + (int)i * 94, r.y + 10, 86, 28};
        if (b.x + b.w > r.x + r.w - 10)
            break;
        if (ui_button(p->ui, b, quick[i]))
            autostart_add(quick[i]);
    }
    end(p, "These open one after another a moment after you log in.");
}

/* ---- Appearance */

static void make_look_thumbs(struct settings *s)
{
    for (int i = 0; i < look_count() && i < 8; i++) {
        if (s->look_thumbs[i].px || !canvas_alloc(&s->look_thumbs[i], LOOK_W, LOOK_H))
            continue;
        const struct gui_look *l = look_get(i);
        wallpaper_paint(&s->look_thumbs[i], l->name, l->dark);
    }
}

static void draw_layout(struct canvas *c, struct rect r, const struct gui_look *l)
{
    uint32_t glass = l->dark ? RGBA(70, 64, 90, 200) : RGBA(255, 255, 255, 190);
    struct canvas k = *c;
    k.clip = rect_intersect(c->clip, r);
    int u = r.w / 44;
    struct rect win = {r.x + r.w / 4, r.y + r.h / 4, r.w / 2, r.h / 2};
    switch (l->dock) {
    case DOCK_CENTER:
        gfx_fill(&k, (struct rect){r.x, r.y, r.w, 4 * u}, glass);
        gfx_round(&k, (struct rect){r.x + r.w / 4, r.y + r.h - 9 * u, r.w / 2, 6 * u}, 3 * u, glass);
        break;
    case DOCK_RAIL:
        gfx_round(&k, (struct rect){r.x + 2 * u, r.y + 2 * u, 7 * u, r.h - 4 * u}, 3 * u, glass);
        win.x += 4 * u;
        break;
    case DOCK_CORNER:
        gfx_round(&k, (struct rect){r.x - 4 * u, r.y + r.h - 9 * u, 22 * u, 14 * u}, 4 * u, glass);
        win.x += 4 * u;
        break;
    }
    if (l->status == STATUS_PILL)
        gfx_round(&k, (struct rect){r.x + r.w - 15 * u, r.y + 2 * u, 13 * u, 3 * u}, (3 * u) / 2, glass);
    gfx_round(&k, win, 2 * u, l->dark ? RGB(32, 30, 40) : RGB(250, 250, 252));
    gfx_round(&k, (struct rect){win.x, win.y, win.w, 3 * u}, 2 * u, l->dark ? RGB(46, 44, 56) : RGB(236, 236, 240));
}

static void pane_appearance(struct page *p)
{
    struct settings *s = p->s;
    section(p, "Look");
    struct rect r = custom(p, LOOK_H + 52, "Look Dune Mist Night");
    if (!p->measure) {
        make_look_thumbs(s);
        int n = look_count(), gap = CLAMP((r.w - 28 - n * LOOK_W) / MAX(n - 1, 1), 8, 40);
        int x0 = r.x + (r.w - n * LOOK_W - (n - 1) * gap) / 2;
        for (int i = 0; i < n && i < 8; i++) {
            const struct gui_look *l = look_get(i);
            struct rect t = {x0 + i * (LOOK_W + gap), r.y + 14, LOOK_W, LOOK_H};
            bool sel = l == look_current();
            if (p->ui->c && s->look_thumbs[i].px) {
                gfx_blit_round(p->ui->c, t, &s->look_thumbs[i], 10, false);
                draw_layout(p->ui->c, t, l);
                gfx_round_frame(p->ui->c, t, 10, ALPHA(0, 30));
                if (sel)
                    gfx_round_frame(p->ui->c, rect_inset(t, -3), 13, style.accent);
                gfx_text_box(p->ui->c, (struct rect){t.x, t.y + t.h + 8, t.w, 18}, sel ? &font_bold : &font_sans,
                             l->title, style.text, ALIGN_CENTER);
            }
            struct rect hit = {t.x, t.y, t.w, t.h + 30};
            if (p->ui->e && p->ui->e->type == EV_MOUSE_DOWN && ui_hover(p->ui, hit)) {
                p->ui->consumed = true;
                apply("look", l->name);
            }
        }
    }
    end(p, look_current()->description);
    section(p, NULL);
    static const char *const modes[] = {"Auto", "Light", "Dark"};
    static const char *const mode_values[] = {"auto", "light", "dark"};
    choice(p, "Appearance", "Auto follows the look", "appearance", modes, mode_values, 3, 240);
    static const char *const accents[] = {"auto", "blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite"};
    static const uint32_t swatches[] = {0, RGB(0, 122, 255), RGB(149, 61, 150), RGB(247, 79, 158), RGB(224, 56, 62),
                                        RGB(247, 130, 27), RGB(230, 168, 0), RGB(98, 186, 70), RGB(140, 140, 140)};
    r = row(p, "Accent color", NULL, 0, 9 * 24);
    if (!p->measure) {
        char value[16];
        current("accent", value, sizeof(value));
        for (int i = 0; i < 9; i++) {
            struct rect dot = ctl(r, 9 * 24, 18);
            dot.x += i * 24;
            dot.w = 18;
            uint32_t col = i ? swatches[i] : look_current()->accent;
            if (p->ui->c) {
                gfx_circle(p->ui->c, dot.x + 9, dot.y + 9, 8, col);
                gfx_ring(p->ui->c, dot.x + 9, dot.y + 9, 8, ALPHA(0, 40));
                if (!i)
                    gfx_ring(p->ui->c, dot.x + 9, dot.y + 9, 6, ALPHA(0xffffffffu, 200));
                if (!strcmp(value, accents[i]))
                    gfx_circle(p->ui->c, dot.x + 9, dot.y + 9, 3, RGB(255, 255, 255));
            }
            if (p->ui->e && p->ui->e->type == EV_MOUSE_DOWN && ui_hover(p->ui, dot)) {
                p->ui->consumed = true;
                apply("accent", accents[i]);
            }
        }
    }
    end(p, NULL);
    section(p, "Glass");
    static const char *const glass[] = {"Liquid Glass", "Frosted"};
    static const char *const glass_values[] = {"liquid", "frosted"};
    choice(p, "Material", "Of menus, the Dock, widgets and sidebars", "glass", glass, glass_values, 2, 240);
    char material[16];
    current("glass", material, sizeof(material));
    if (!strcmp(material, "liquid")) {
        int k = (int)number("glassstrength", 70);
        if (slider(p, "Liquid Glass effect", "How strongly the glass bends and shows what lies behind it", &k, 0,
                   100, "Subtle", "Strong"))
            apply_number("glassstrength", k);
    }
    end(p, !strcmp(material, "solid") ? "Reduce transparency is on in Accessibility, so menus and the Dock are solid."
                                      : NULL);
}

static void pane_access(struct page *p)
{
    struct settings *s = p->s;
    header(p, SYM_ACCESS, panes[P_ACCESS].color, "Accessibility",
           "Make SynthOS calmer and easier to read and to use with the keyboard and the mouse.");
    section(p, "Vision");
    bool still = !is_on("animations");
    if (toggle(p, "Reduce motion", "Windows, menus and the Dock appear without moving", &still))
        apply("animations", still ? "off" : "on");
    if (!still) {
        static const char *const speeds[] = {"Relaxed", "Normal", "Fast", "Fastest"};
        static const char *const speed_values[] = {"70", "100", "140", "200"};
        choice(p, "Animation speed", "How quickly windows open, close and move", "animspeed", speeds, speed_values,
               4, 300);
    }
    char glass[16];
    current("glass", glass, sizeof(glass));
    bool solid = !strcmp(glass, "solid");
    if (toggle(p, "Reduce transparency", "Solid colors instead of glass behind menus, the Dock and sidebars", &solid))
        apply("glass", solid ? "solid" : "liquid");
    end(p, NULL);
    section(p, "Motor");
    if (link(p, panes[P_MOUSE].sym, panes[P_MOUSE].color, "Pointer speed and buttons", "Mouse"))
        go(s, P_MOUSE);
    if (link(p, panes[P_KEYBOARD].sym, panes[P_KEYBOARD].color, "Key repeat and shortcuts", "Keyboard"))
        go(s, P_KEYBOARD);
    if (link(p, panes[P_DISPLAYS].sym, panes[P_DISPLAYS].color, "Brightness and Night Shift", "Displays"))
        go(s, P_DISPLAYS);
    end(p, NULL);
}

/* ---- Control Center */

static void pane_control(struct page *p)
{
    struct settings *s = p->s;
    header(p, SYM_CONTROL, panes[P_CONTROL].color, "Control Center",
           "Control Center sits in the menu bar; the modules you pick here sit next to it, each with its own menu.");
    static const char *const names[] = {"sound", "network", "display", "focus", "search"};
    static const char *const labels[] = {"Sound", "Network", "Display", "Focus", "Spotlight"};
    static const char *const descs[] = {"Volume, mute and the output", "Turn the network on or off, see the address",
                                        "Brightness, Night Shift and Dark Mode", "Do Not Disturb (shows while it is on)",
                                        "Search apps and files"};
    char list[64];
    current("menuextras", list, sizeof(list));
    section(p, "Show in Menu Bar");
    for (int i = 0; i < 5; i++) {
        bool on = strstr(list, names[i]) != NULL;
        if (toggle(p, labels[i], descs[i], &on)) {
            char out[64] = "";
            for (int k = 0; k < 5; k++) {
                bool want = k == i ? on : strstr(list, names[k]) != NULL;
                if (want) {
                    if (out[0])
                        str_append(out, ",", sizeof(out));
                    str_append(out, names[k], sizeof(out));
                }
            }
            apply("menuextras", out[0] ? out : "none");
        }
    }
    end(p, NULL);
    section(p, "Menu Bar");
    static const char *const bars[] = {"Menu Bar", "Floating Pill"};
    static const char *const bar_values[] = {"menubar", "pill"};
    choice(p, "Status at the top", "The floating pill belongs to the current look", "statusbar", bars, bar_values,
           2, 240);
    if (link(p, panes[P_DATETIME].sym, panes[P_DATETIME].color, "Clock", "Date & Time"))
        go(s, P_DATETIME);
    end(p, NULL);
}

/* ---- Desktop & Dock */

static void pane_dock(struct page *p)
{
    section(p, "Dock");
    static const char *const sizes[] = {"Small", "Medium", "Large"};
    static const char *const size_values[] = {"small", "medium", "large"};
    choice(p, "Size", NULL, "docksize", sizes, size_values, 3, 240);
    toggle_setting(p, "Magnification", "Icons grow under the pointer", "magnify");
    static const char *const docks[] = {"Bottom", "Corner", "Side"};
    static const char *const dock_values[] = {"bottom", "corner", "side"};
    choice(p, "Position on screen", NULL, "dockstyle", docks, dock_values, 3, 240);
    static const char *const modes[] = {"Always", "Hide Automatically", "Off"};
    static const char *const mode_values[] = {"show", "autohide", "off"};
    choice(p, "Show the Dock", NULL, "dockmode", modes, mode_values, 3, 240);
    toggle_setting(p, "Animate opening applications", NULL, "dockbounce");
    toggle_setting(p, "Show indicators for open applications", NULL, "dockdots");
    toggle_setting(p, "Show previews of open windows", "Rest the pointer on an app to see and pick its windows",
                   "dockpreviews");
    if (button(p, "Icons in the Dock", "Drag icons onto the Dock, along it or off it", "Reset Icons"))
        apply("dockapps", "default");
    char title[64];
    ksnprintf(title, sizeof(title), "Start over with the layout of %s", look_current()->title);
    if (button(p, title, "Each look keeps its own Dock and menu bar", "Reset Layout"))
        look_reset();
    end(p, NULL);
    section(p, "Windows");
    toggle_setting(p, "Tile windows at the screen edges", "Drag a window to a side, a corner or the top; Super+arrows "
                                                          "do it from the keyboard", "snap");
    end(p, "The ··· button of every window has Tile Left, Tile Right and the quarters too.");
    section(p, "Mission Control");
    static const char *const corners[] = {"Off", "Mission Control", "Desktop", "Launchpad", "Notifications",
                                          "Lock Screen", "Screenshot"};
    static const char *const corner_values[] = {"off", "mission", "desktop", "launchpad", "notifications", "lock",
                                                "screenshot"};
    static const char *const corner_keys[] = {"cornertl", "cornertr", "cornerbl", "cornerbr"};
    static const char *const corner_names[] = {"Top left hot corner", "Top right hot corner",
                                               "Bottom left hot corner", "Bottom right hot corner"};
    for (int i = 0; i < 4; i++)
        popup(p, corner_names[i], NULL, corner_keys[i], corners, corner_values, 7);
    info(p, "Show all windows", "F3 or Ctrl+Up");
    end(p, "Push the pointer into a corner of the screen to run what it is set to.");
}

/* ---- Displays */

static int menu_display;
static struct display_mode menu_modes[24];
static char menu_labels[24][24];
static struct menu_item menu_items[24];

static void pick_mode(struct window *owner, int index)
{
    UNUSED(owner);
    char value[24];
    ksnprintf(value, sizeof(value), "%ux%u", menu_modes[index].width, menu_modes[index].height);
    apply(menu_display ? "resolution2" : "resolution", value);
}

static void pane_displays(struct page *p)
{
    int n = wm_screen_count();
    section(p, "Brightness");
    int b = (int)number("brightness", 100);
    if (slider(p, "Brightness", "Dims the picture when the screen has no control of its own", &b, 10, 100, NULL,
               NULL))
        apply_number("brightness", b);
    static const char *const nights[] = {"Off", "On", "Scheduled"};
    static const char *const night_values[] = {"off", "on", "auto"};
    choice(p, "Night Shift", "Warmer colors in the evening (21:00 to 7:00)", "nightshift", nights, night_values, 3,
           300);
    int warm = (int)number("nightwarmth", 50);
    if (slider(p, "Color temperature", NULL, &warm, 0, 100, "Less Warm", "More Warm"))
        apply_number("nightwarmth", warm);
    end(p, NULL);
    section(p, NULL);
    struct rect g = custom(p, 176, "Arrangement");
    if (!p->measure && p->ui->c) {
        struct rect bounds = wm_screen_rect(0);
        for (int i = 1; i < n; i++)
            bounds = rect_union(bounds, wm_screen_rect(i));
        int scale = MAX(1, MAX(bounds.w / (g.w - 80) + 1, bounds.h / 120 + 1));
        int ox = g.x + (g.w - bounds.w / scale) / 2, oy = g.y + 16 + (120 - bounds.h / scale) / 2;
        for (int i = 0; i < n; i++) {
            struct rect a = wm_screen_rect(i);
            struct rect r = {ox + (a.x - bounds.x) / scale + 2, oy + (a.y - bounds.y) / scale + 2, a.w / scale - 4,
                             a.h / scale - 4};
            const struct canvas *wp = desktop_wallpaper(i);
            struct canvas shot;
            if (wp && canvas_alloc(&shot, MAX(r.w, 1), MAX(r.h, 1))) {
                gfx_scale(&shot, (struct rect){0, 0, r.w, r.h}, wp, 255);
                gfx_blit_round(p->ui->c, r, &shot, 5, false);
                canvas_free(&shot);
            } else {
                gfx_round(p->ui->c, r, 5, style.control);
            }
            gfx_round_frame(p->ui->c, r, 5, i == 0 ? style.accent : ALPHA(0, 60));
            gfx_fill(p->ui->c, (struct rect){r.x + 4, r.y + 3, r.w - 8, 3}, ALPHA(0xffffffffu, 210));
            char label[8];
            ksnprintf(label, sizeof(label), "%d", i + 1);
            gfx_text_box(p->ui->c, r, &font_title, label, RGB(255, 255, 255), ALIGN_CENTER);
        }
        gfx_text_box(p->ui->c, (struct rect){g.x, g.y + 148, g.w, 18}, &font_small,
                     n > 1 ? "Display 1 is the main display with the Dock" : "One display in use", style.secondary,
                     ALIGN_CENTER);
    }
    if (hal_screen_count() > 1) {
        static const char *const labels[] = {"Right", "Left", "Above", "Below", "Off"};
        static const char *const values[] = {"right", "left", "above", "below", "off"};
        choice(p, "Second display", NULL, "arrange", labels, values, 5, 320);
    }
    end(p, NULL);
    for (int i = 0; i < n; i++) {
        int hal = gui_display_hal(i);
        char title[64], res[32];
        ksnprintf(title, sizeof(title), "Display %d - %s", i + 1, hal_screen_name(hal));
        section(p, title);
        struct rect a = wm_screen_rect(i);
        int factor = a.w >= 2400 ? 2 : 1;
        ksnprintf(res, sizeof(res), "%d x %d", a.w * factor, a.h * factor);
        struct rect r = row(p, "Resolution", NULL, 0, 160);
        if (!p->measure) {
            struct rect b = ctl(r, 160, 24);
            bool hot = ui_hover(p->ui, b);
            if (p->ui->c) {
                gfx_round(p->ui->c, b, 6, style.dark ? ALPHA(0xffffffffu, hot ? 34 : 22) : ALPHA(0, hot ? 18 : 10));
                gfx_text_box(p->ui->c, (struct rect){b.x + 10, b.y, b.w - 34, b.h}, &font_sans, res, style.text,
                             ALIGN_LEFT);
                gfx_symbol(p->ui->c, b.x + b.w - 20, b.y + 4, SYM_DOWN, 16, style.secondary);
            }
            if (p->ui->e && p->ui->e->type == EV_MOUSE_DOWN && hot && i < 2) {
                p->ui->consumed = true;
                int count = hal_screen_modes(hal, menu_modes, 24);
                for (int k = 0; k < count; k++) {
                    ksnprintf(menu_labels[k], sizeof(menu_labels[k]), "%u x %u", menu_modes[k].width,
                              menu_modes[k].height);
                    menu_items[k] = (struct menu_item){menu_labels[k], 0, NULL};
                }
                menu_display = i;
                if (count)
                    ui_menu_items(p->ui->win, b.x, b.y + b.h + 2, menu_items, count, pick_mode);
            }
        }
        info(p, "Scale", factor == 2 ? "200%" : "100%");
        end(p, NULL);
    }
}

/* ---- Wallpaper */

static void make_thumbs(struct settings *s)
{
    if (s->thumbs[0].px && s->thumbs_dark == style.dark && s->thumbs_look == look_current())
        return;
    for (int i = 0; i < WALLS; i++) {
        if (!s->thumbs[i].px && !canvas_alloc(&s->thumbs[i], THUMB_W, THUMB_H))
            continue;
        wallpaper_paint(&s->thumbs[i], i ? wall_values[i] : look_current()->name, style.dark);
    }
    s->thumbs_dark = style.dark;
    s->thumbs_look = look_current();
}

static void pane_wallpaper(struct page *p)
{
    struct settings *s = p->s;
    char value[PATH_MAX_LEN];
    current("wallpaper", value, sizeof(value));
    const char *shown = value;
    for (int i = 0; i < WALLS; i++)
        if (!strcmp(value, wall_values[i]))
            shown = wall_names[i];
    section(p, NULL);
    struct rect r = custom(p, 132, "Current wallpaper");
    if (!p->measure && p->ui->c) {
        const struct canvas *wp = desktop_wallpaper(0);
        struct rect big = {r.x + 16, r.y + 16, 160, 100};
        struct canvas shot;
        if (wp && canvas_alloc(&shot, big.w, big.h)) {
            gfx_scale(&shot, (struct rect){0, 0, big.w, big.h}, wp, 255);
            gfx_blit_round(p->ui->c, big, &shot, 8, false);
            canvas_free(&shot);
            gfx_round_frame(p->ui->c, big, 8, ALPHA(0, 30));
        }
        gfx_text_box(p->ui->c, (struct rect){big.x + big.w + 20, big.y + 26, r.w - 220, 24}, &font_title, shown,
                     style.text, ALIGN_LEFT);
        gfx_text_box(p->ui->c, (struct rect){big.x + big.w + 20, big.y + 54, r.w - 220, 18}, &font_sans,
                     style.dark ? "Dark variant" : "Light variant", style.secondary, ALIGN_LEFT);
    }
    end(p, NULL);
    section(p, "Dynamic Wallpapers");
    int cols = MAX(1, (p->w - 28 + 16) / (THUMB_W + 16));
    int rows = (WALLS + cols - 1) / cols;
    r = custom(p, rows * (THUMB_H + 34) + 18, "Dune Mist Night Sunset Waves Hills Aurora Synthwave Solid");
    if (!p->measure) {
        make_thumbs(s);
        int x0 = r.x + (r.w - cols * THUMB_W - (cols - 1) * 16) / 2;
        for (int i = 0; i < WALLS; i++) {
            struct rect t = {x0 + (i % cols) * (THUMB_W + 16), r.y + 16 + (i / cols) * (THUMB_H + 34), THUMB_W,
                             THUMB_H};
            bool sel = !strcmp(value, wall_values[i]);
            if (p->ui->c && s->thumbs[i].px) {
                gfx_blit_round(p->ui->c, t, &s->thumbs[i], 8, false);
                gfx_round_frame(p->ui->c, t, 8, ALPHA(0, 30));
                if (sel)
                    gfx_round_frame(p->ui->c, rect_inset(t, -3), 11, style.accent);
                gfx_text_box(p->ui->c, (struct rect){t.x, t.y + t.h + 6, t.w, 18}, &font_sans, wall_names[i],
                             style.text, ALIGN_CENTER);
            }
            if (p->ui->e && p->ui->e->type == EV_MOUSE_DOWN && ui_hover(p->ui, t)) {
                p->ui->consumed = true;
                apply("wallpaper", wall_values[i]);
            }
        }
    }
    end(p, NULL);
    section(p, "From the Internet");
    if (button(p, "Photo of the day", "Bing's picture of the day, saved in ~/Pictures/Wallpapers", "Download"))
        wallpaper_fetch("bing");
    if (button(p, "Random photo", "A new photo from picsum.photos", "Download"))
        wallpaper_fetch("random");
    bool fetch = field(p, "From a link", NULL, &s->wallpaper_url, "https://example.com/picture.jpg", 280);
    if (fetch && s->wallpaper_url.len)
        wallpaper_fetch(s->wallpaper_url.buf);
    end(p, NULL);
    section(p, "Your Picture");
    if (field(p, "Picture file", "PNG, JPEG, GIF or BMP", &s->wallpaper_file, "/home/Pictures/photo.jpg", 280) &&
        s->wallpaper_file.len)
        apply("wallpaper", s->wallpaper_file.buf);
    end(p, "In Photos, ... > Set as Wallpaper does the same for the picture you are looking at.");
}

/* ---- Lock Screen, Privacy, Users */

static void pane_lock(struct page *p)
{
    struct settings *s = p->s;
    static const char *const labels[] = {"Never",      "After 1 minute",   "After 2 minutes", "After 5 minutes",
                                         "After 10 minutes", "After 15 minutes", "After 30 minutes", "After 1 hour"};
    static const char *const values[] = {"0", "1", "2", "5", "10", "15", "30", "60"};
    section(p, NULL);
    popup(p, "Lock the screen when idle", NULL, "locktime", labels, values, 8);
    end(p, NULL);
    const struct user *me = user_current();
    char value[USER_NAME_MAX + 4], title[96];
    current("autologin", value, sizeof(value));
    bool automatic = !strcmp(value, me->name);
    section(p, "When SynthOS Starts");
    ksnprintf(title, sizeof(title), "Log in automatically as %s", me->full);
    if (toggle(p, title, "Skip the login screen at boot", &automatic))
        apply("autologin", automatic ? me->name : "off");
    if (link(p, panes[P_USERS].sym, panes[P_USERS].color, "Password", me->has_password ? "Set" : "Not set"))
        go(s, P_USERS);
    end(p, NULL);
    section(p, NULL);
    if (button(p, "Lock the screen now", "Super+L does it too", "Lock Screen")) {
        p->ui->win = NULL;
        gui_lock();
    }
    end(p, NULL);
}

static void pane_privacy(struct page *p)
{
    struct settings *s = p->s;
    header(p, SYM_HAND, panes[P_PRIVACY].color, "Privacy & Security",
           "HTTPS checks every server against trusted root certificates; sites keep cookies only until you turn "
           "the computer off.");
    char roots[32], cookies[32];
    ksnprintf(roots, sizeof(roots), "%d trusted", tls_roots_count());
    ksnprintf(cookies, sizeof(cookies), "%d stored", http_cookie_count());
    section(p, "Internet");
    struct rect r = row(p, "Root certificates", roots, 0, 90);
    if (!p->measure && ui_button(p->ui, ctl(r, 90, 26), "Reload")) {
        tls_roots_load();
        gui_notify("Certificates", "Reloaded /etc/certs and /home/certs.");
    }
    r = row(p, "Cookies", cookies, 0, 90);
    if (!p->measure && ui_button(p->ui, ctl(r, 90, 26), "Clear"))
        http_cookies_clear();
    end(p, "To trust another root, put its .pem file in /home/certs and press Reload.");
    section(p, "Security");
    if (link(p, panes[P_LOCK].sym, panes[P_LOCK].color, "Screen lock", number("locktime", 0) ? "When idle" : "Manual"))
        go(s, P_LOCK);
    if (link(p, panes[P_USERS].sym, panes[P_USERS].color, "Passwords", NULL))
        go(s, P_USERS);
    end(p, NULL);
}

static struct settings *dialog_settings(struct window *w)
{
    return wm_window_alive(w) ? w->data : NULL;
}

static void done_password(struct window *w, const char *value)
{
    struct settings *s = dialog_settings(w);
    if (!s || !s->pending_user[0])
        return;
    int err = user_set_password(s->pending_user, value);
    if (err)
        gui_message("The password could not be changed", err_str(err));
    else
        gui_notify("Password changed", *value ? "The new password is in effect." : "The password was removed.");
    s->pending_user[0] = 0;
    wm_invalidate(w);
}

static void done_new_user_password(struct window *w, const char *value)
{
    struct settings *s = dialog_settings(w);
    if (!s || !s->pending_user[0])
        return;
    int err = user_add(s->pending_user, s->pending_user, value, false);
    if (err)
        gui_message("The user could not be created",
                    err == E_EXIST ? "A user with this name already exists." : err == E_INVAL
                                                                                 ? "Use lowercase letters, digits, - and _."
                                                                                 : err_str(err));
    s->pending_user[0] = 0;
    wm_invalidate(w);
}

static void done_new_user(struct window *w, const char *value)
{
    struct settings *s = dialog_settings(w);
    if (!s || !*value)
        return;
    if (!user_name_valid(value)) {
        gui_message("Invalid user name", "Use lowercase letters, digits, - and _.");
        return;
    }
    str_copy(s->pending_user, value, sizeof(s->pending_user));
    ui_prompt_secret(w, "Password", "Choose a password for the new user (leave empty for none):",
                     done_new_user_password);
}

static void done_delete(struct window *w, const char *value)
{
    UNUSED(value);
    struct settings *s = dialog_settings(w);
    if (!s || !s->pending_user[0])
        return;
    int err = user_delete(s->pending_user);
    if (err)
        gui_message("The user could not be deleted", err == E_BUSY ? "You can't delete root or yourself." : err_str(err));
    s->pending_user[0] = 0;
    wm_invalidate(w);
}

static void avatar(struct canvas *c, int x, int y, int size, const char *name, bool me)
{
    uint32_t col = me ? style.accent : RGB(142, 142, 150);
    struct rect r = {x, y, size, size};
    gfx_round_gradient(c, r, size / 2, color_shade(col, 30), col);
    char initial[2] = {name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0};
    gfx_text_box(c, r, size >= 36 ? &font_title : &font_bold, initial, RGB(255, 255, 255), ALIGN_CENTER);
}

static void pane_users(struct page *p)
{
    struct settings *s = p->s;
    const struct user *me = user_current();
    int n = users_count();
    section(p, NULL);
    for (int i = 0; i < n; i++) {
        const struct user *u = users_get(i);
        char name[96], sub[96];
        ksnprintf(name, sizeof(name), "%s%s", u->full, u == me ? "  (you)" : "");
        ksnprintf(sub, sizeof(sub), "%s  ·  %s  ·  %s", u->name, u->admin ? "Admin" : "Standard",
                  u->has_password ? "Password set" : "No password");
        struct rect r = row(p, NULL, NULL, 60, 0);
        if (p->query && (found(p->query, u->full) || found(p->query, u->name)))
            p->hits += p->measure;
        if (p->measure)
            continue;
        if (p->ui->c) {
            avatar(p->ui->c, r.x + 14, r.y + 10, 40, u->full, u == me);
            gfx_text_box(p->ui->c, (struct rect){r.x + 66, r.y + 12, r.w / 2, 18}, &font_bold, name, style.text,
                         ALIGN_LEFT);
            gfx_text_box(p->ui->c, (struct rect){r.x + 66, r.y + 32, r.w / 2, 16}, &font_small, sub, style.secondary,
                         ALIGN_LEFT);
        }
        bool may = me->admin || u == me;
        if (may && ui_button(p->ui, (struct rect){r.x + r.w - (i && u != me ? 222 : 142), r.y + 16, 128, 28},
                             "Password...")) {
            str_copy(s->pending_user, u->name, sizeof(s->pending_user));
            ui_prompt_secret(p->ui->win, "Change Password", "Enter a new password (leave empty to remove it):",
                             done_password);
        }
        if (me->admin && i && u != me && ui_button(p->ui, (struct rect){r.x + r.w - 86, r.y + 16, 72, 28}, "Delete")) {
            char msg[128];
            ksnprintf(msg, sizeof(msg), "Delete the user \"%s\"? The home folder %s is kept.", u->name, u->home);
            str_copy(s->pending_user, u->name, sizeof(s->pending_user));
            ui_confirm(p->ui->win, "Delete User", msg, done_delete);
        }
    }
    end(p, NULL);
    if (!me->admin)
        return;
    section(p, NULL);
    if (button(p, "Add a person who uses this computer", NULL, "Add User..."))
        ui_prompt(p->ui->win, "New User", "Short name for the new account (lowercase):", "", done_new_user);
    end(p, "Every user has their own /home folder, desktop, Dock and settings.");
}

/* ---- Keyboard, Mouse, Terminal */

static void pane_keyboard(struct page *p)
{
    section(p, NULL);
    int rate = (int)number("keyrate", 30);
    if (slider(p, "Key repeat rate", NULL, &rate, 2, 30, "Slow", "Fast")) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", rate);
        apply("keyrate", buf);
    }
    static const int delays[] = {1000, 750, 500, 250};
    long d = number("keydelay", 500);
    int pos = 0;
    for (int i = 0; i < 4; i++)
        if (d <= delays[i])
            pos = i;
    if (slider(p, "Delay until repeat", NULL, &pos, 0, 3, "Long", "Short")) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", delays[pos]);
        apply("keydelay", buf);
    }
    end(p, NULL);
    section(p, "Input Sources");
    char list[64] = "";
    bool changed = false;
    for (int i = 0; i < keymap_count(); i++) {
        const char *title, *name = keymap_layout_name(i, &title);
        bool on = keymap_enabled(i);
        if (i && toggle(p, title, NULL, &on))
            changed = true;
        if (on || !i) {
            if (list[0])
                str_append(list, ",", sizeof(list));
            str_append(list, name, sizeof(list));
        }
    }
    if (changed) {
        apply("keyboard", list);
        chrome_changed();
    }
    end(p, "English (US) is always on. With more than one layout, Super+Space or the letters in the status bar switch "
           "between them.");
    section(p, "Text Input");
    static const char *const sets[] = {"437 Western", "866 Cyrillic"};
    static const char *const set_values[] = {"437", "866"};
    choice(p, "Character set", "Of the text console and the Terminal", "codepage", sets, set_values, 2, 240);
    toggle_setting(p, "Suggest from history", "Accept with the Right arrow", "suggest");
    end(p, NULL);
    section(p, "Keyboard Shortcuts");
    static const char *const keys[][2] = {
        {"Search and open apps", "Ctrl+Space"},
        {"Switch the keyboard layout", "Super+Space"},
        {"Switch windows", "Alt+Tab, Super+Tab"},
        {"Mission Control", "F3, Ctrl+Up"},
        {"Show the desktop", "Super+D"},
        {"Tile the window", "Super+arrows"},
        {"Close the window", "Super+W"},
        {"Minimize the window", "Super+M"},
        {"Quit the app", "Super+Q"},
        {"Open Files", "Super+E"},
        {"Lock the screen", "Super+L"},
        {"Open System Settings", "Super+,"},
        {"Copy, paste", "Ctrl+C, Ctrl+V"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(keys); i++)
        info(p, keys[i][0], keys[i][1]);
    end(p, "Super is the Windows key (Command on a Mac keyboard).");
    section(p, "Screenshots");
    info(p, "Whole screen", "Super+Shift+3, Super+Print Screen");
    info(p, "An area", "Super+Shift+4, Super+Shift+S");
    info(p, "Screenshot bar", "Print Screen, Super+Shift+5");
    static const char *const places[] = {"Desktop", "Pictures"};
    static const char *const place_values[] = {"desktop", "pictures"};
    choice(p, "Save to", NULL, "shotfolder", places, place_values, 2, 200);
    static const char *const timers[] = {"None", "5 s", "10 s"};
    static const char *const timer_values[] = {"0", "5", "10"};
    choice(p, "Timer", NULL, "shotdelay", timers, timer_values, 3, 200);
    toggle_setting(p, "Show floating thumbnail", "A click on it opens the editor: crop and mark up", "shotthumb");
    toggle_setting(p, "Open the editor after each screenshot", NULL, "shotedit");
    end(p, NULL);
}

static void pane_mouse(struct page *p)
{
    section(p, NULL);
    int v = (int)number("mousespeed", 5);
    if (slider(p, "Tracking speed", NULL, &v, 1, 10, "Slow", "Fast")) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", v);
        apply("mousespeed", buf);
    }
    static const int clicks[] = {900, 750, 620, 520, 440, 370, 310, 260, 200};
    long ms = number("doubleclick", 400);
    int pos = 0;
    for (int i = 0; i < 9; i++)
        if (ms <= clicks[i])
            pos = i;
    if (slider(p, "Double-click speed", NULL, &pos, 0, 8, "Slow", "Fast")) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", clicks[pos]);
        apply("doubleclick", buf);
    }
    v = (int)number("scrollspeed", 1);
    if (slider(p, "Scrolling speed", NULL, &v, 1, 5, "Slow", "Fast")) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", v);
        apply("scrollspeed", buf);
    }
    end(p, NULL);
    section(p, NULL);
    toggle_setting(p, "Natural scrolling", "Content moves the way the wheel turns", "naturalscroll");
    static const char *const sides[] = {"Left", "Right"};
    static const char *const side_values[] = {"left", "right"};
    choice(p, "Primary mouse button", "Right suits the left hand", "mouseprimary", sides, side_values, 2, 180);
    end(p, NULL);
}

static void draw_theme(struct canvas *c, struct rect row, int index, bool sel, void *ctx)
{
    struct settings *s = ctx;
    struct theme t;
    if (theme_find(s->themes[index], &t) == 0) {
        for (int i = 0; i < 8; i++)
            gfx_fill(c, (struct rect){row.x + 20 + i * 12, row.y + 8, 12, row.h - 16}, 0xff000000u | t.palette[i + 8]);
        gfx_round_frame(c, (struct rect){row.x + 19, row.y + 7, 98, row.h - 14}, 3, ALPHA(0, 50));
    }
    gfx_text_box(c, (struct rect){row.x + 130, row.y, row.w - 140, row.h}, &font_sans, s->themes[index], ui_row_text(sel),
                 ALIGN_LEFT);
}

static void pane_terminal(struct page *p)
{
    struct settings *s = p->s;
    section(p, "Color Theme");
    struct rect r = custom(p, 6 * 32 + 12, "Color theme");
    if (!p->measure) {
        s->theme_list.row_h = 32;
        int res = ui_list(p->ui, (struct rect){r.x + 2, r.y + 6, r.w - 4, 6 * 32}, &s->theme_list, draw_theme, s);
        if ((res == LIST_SELECT || res == LIST_ACTIVATE) && s->theme_list.selected >= 0)
            apply("theme", s->themes[s->theme_list.selected]);
    }
    end(p, "Themes are files in /etc/themes and /home/themes.");
    section(p, NULL);
    static const char *const shapes[] = {"Block", "Underline", "Bar"};
    static const char *const shape_values[] = {"block", "underline", "bar"};
    choice(p, "Cursor", NULL, "cursor", shapes, shape_values, 3, 240);
    toggle_setting(p, "Status bar", "Path, theme, memory and clock at the bottom", "termstatus");
    toggle_setting(p, "Syntax highlighting while typing", NULL, "highlight");
    toggle_setting(p, "Show the logo at startup", NULL, "banner");
    end(p, NULL);
}

/* the built-in browsers and every browser an installer registered (Firefox, Brave, Opera, ...) */
#define MAX_BROWSERS 16
static const char *browser_labels[MAX_BROWSERS], *browser_values[MAX_BROWSERS];
static int browser_count;
#if CONFIG_WIN32
static char browser_names[MAX_BROWSERS][64], browser_keys[MAX_BROWSERS][64];
#endif

#if CONFIG_WIN32
static void add_browser(const struct winshell_browser *b, void *ctx)
{
    UNUSED(ctx);
    if (browser_count >= MAX_BROWSERS || gui_builtin_app_for_program(b->program))
        return;
    str_copy(browser_names[browser_count], b->name, sizeof(browser_names[0]));
    str_copy(browser_keys[browser_count], b->key, sizeof(browser_keys[0]));
    browser_labels[browser_count] = browser_names[browser_count];
    browser_values[browser_count] = browser_keys[browser_count];
    browser_count++;
}
#endif

static void browser_popup(struct page *p)
{
    browser_labels[0] = "Chromium";
    browser_values[0] = "chromium";
    browser_labels[1] = "Google Chrome";
    browser_values[1] = "chrome";
    browser_count = 2;
#if CONFIG_WIN32
    winshell_browsers(add_browser, NULL);
#endif
    popup(p, "Default web browser", NULL, "browser", browser_labels, browser_values, browser_count);
}

/* ---- Apps */

#if CONFIG_WIN32
static void program_row(const struct winshell_program *prog, void *ctx)
{
    struct page *p = ctx;
    char desc[128];
    ksnprintf(desc, sizeof(desc), "%s%s%s", prog->version, prog->version[0] && prog->publisher[0] ? " - " : "",
              prog->publisher);
    if (button(p, prog->name, desc[0] ? desc : NULL, "Uninstall") && prog->uninstall[0] &&
        !gui_start_command(prog->uninstall))
        gui_message("Cannot uninstall", "The program's uninstaller is missing.");
}
#endif

static void pane_apps(struct page *p)
{
    header(p, SYM_APPS, panes[P_APPS].color, "Apps",
           "Default apps, the programs you installed and how 32- and 64-bit Windows programs run: double-click an "
           ".exe, or start it from the Terminal.");
    section(p, "Default Apps");
    browser_popup(p);
    end(p, "Links from every app open in the default browser.");
    struct bundle_info bi;
    const char *chromium = vfs_exists("/home/apps/chromium/chrome.exe") ? "Installed"
                           : bundle_info(&bi)                             ? "Unpacks after login"
                                                                          : "Not in this image";
    section(p, "Browsers");
    info(p, "Chromium", chromium);
    info(p, "Google Chrome", vfs_exists("/home/apps/chrome/chrome.exe") ? "Installed" : "Not installed");
    toggle_setting(p, "Download Google Chrome by itself", "On an image without Chromium, after the first login",
                   "chromeauto");
    end(p, NULL);
#if CONFIG_WIN32
    section(p, "Installed Programs");
    winshell_programs(program_row, p);
    end(p, "Programs installed with their own installers, as in Apps & features on Windows.");
#endif
    section(p, "For Developers");
    toggle_setting(p, "Run programs without windows", "Their output goes to the Terminal (headless)", "w32headless");
    toggle_setting(p, "Log Windows API calls", "Every call goes to the kernel log", "w32trace");
    end(p, "Ctrl+T while a Windows program runs lists its processes and threads.");
}

/* ---- the window */

static void load_themes(struct settings *s);

static int search_hits(struct ui *ui, struct settings *s, int pane, int w)
{
    static int h[SECTIONS], r[SECTIONS];
    struct ui quiet = *ui;
    quiet.c = NULL;
    quiet.e = NULL;
    struct page m = {&quiet, s, 0, 0, w, true, s->search.buf, 0, 0, 0, 0, h, r};
    panes[pane].fn(&m);
    return m.hits + (found(s->search.buf, panes[pane].name) ? 1 : 0);
}

static bool side_item(struct ui *ui, struct rect r, int pane, bool selected)
{
    bool hot = ui_hover(ui, r);
    if (ui->c) {
        if (selected)
            gfx_round(ui->c, r, 6, style.accent);
        ui_settings_icon(ui->c, r.x + 6, r.y + 4, 20, panes[pane].sym, panes[pane].color);
        gfx_text_box(ui->c, (struct rect){r.x + 34, r.y, r.w - 38, r.h}, &font_sans, panes[pane].name,
                     selected ? RGB(255, 255, 255) : style.text, ALIGN_LEFT);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && hot) {
        ui->consumed = true;
        return true;
    }
    return false;
}

static void sidebar(struct ui *ui, struct settings *s, int H, int w)
{
    if (ui_search(ui, (struct rect){18, 44, SIDEBAR_W - 30, 28}, &s->search, "Search")) {
        int best = -1, most = 0;
        for (int i = 0; i < P_COUNT; i++) {
            int hits = search_hits(ui, s, i, w);
            if (hits > most)
                best = i, most = hits;
        }
        if (best >= 0)
            go(s, best);
    }
    struct rect list = {0, 82, SIDEBAR_W, H - 90};
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(list, ui->e->x, ui->e->y)) {
        s->side_scroll = MAX(0, s->side_scroll + ui->e->wheel * 30);
        ui->changed = true;
        ui->consumed = true;
    }
    struct canvas *saved = ui->c, clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, list);
        ui->c = &clipped;
    }
    int y = 86 - s->side_scroll;
    if (s->search.len) {
        ui_label(ui, (struct rect){24, y, SIDEBAR_W - 40, 18}, "Results", &font_small, style.secondary, ALIGN_LEFT);
        y += 24;
        int shown = 0;
        for (int i = 0; i < P_COUNT; i++) {
            if (!search_hits(ui, s, i, w))
                continue;
            if (side_item(ui, (struct rect){14, y, SIDEBAR_W - 26, SIDE_ROW}, i, s->pane == i))
                go(s, i);
            y += SIDE_ROW + 2;
            shown++;
        }
        if (!shown)
            ui_label(ui, (struct rect){24, y, SIDEBAR_W - 40, 20}, "No Results", &font_sans, style.secondary,
                     ALIGN_LEFT);
    } else {
        const struct user *me = user_current();
        struct rect card = {14, y, SIDEBAR_W - 26, 50};
        if (ui->c) {
            if (s->pane == P_USERS)
                gfx_round(ui->c, card, 8, style.dark ? ALPHA(0xffffffffu, 30) : ALPHA(0, 20));
            avatar(ui->c, card.x + 6, card.y + 7, 36, me->full, true);
            gfx_text_box(ui->c, (struct rect){card.x + 52, card.y + 8, card.w - 56, 18}, &font_bold, me->full,
                         style.text, ALIGN_LEFT);
            gfx_text_box(ui->c, (struct rect){card.x + 52, card.y + 26, card.w - 56, 16}, &font_small,
                         "SynthOS Account", style.secondary, ALIGN_LEFT);
        }
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, card)) {
            ui->consumed = true;
            go(s, P_USERS);
        }
        y += 58;
        int group = 0;
        int shown = panes[s->pane].parent >= 0 ? panes[s->pane].parent : s->pane;
        for (int i = 0; i < P_COUNT; i++) {
            if (panes[i].parent >= 0)
                continue;
            if (panes[i].group != group) {
                group = panes[i].group;
                y += 8;
            }
            if (side_item(ui, (struct rect){14, y, SIDEBAR_W - 26, SIDE_ROW}, i, shown == i))
                go(s, i);
            y += SIDE_ROW + 1;
        }
    }
    int content = y + s->side_scroll - 86;
    s->side_scroll = CLAMP(s->side_scroll, 0, MAX(0, content - list.h + 12));
    if (ui->win && saved)
        ui->c = saved;
}

static void toolbar(struct ui *ui, struct settings *s, int cx, int W)
{
    struct rect back = {cx + 14, (TOOLBAR_HEIGHT - 28) / 2, 28, 28}, fwd = {cx + 44, back.y, 28, 28};
    bool can_back = s->back_n > 0 || panes[s->pane].parent >= 0, can_fwd = s->forward_n > 0;
    if (ui->c) {
        gfx_symbol(ui->c, back.x + 6, back.y + 6, SYM_LEFT, 16, can_back ? style.text : style.tertiary);
        gfx_symbol(ui->c, fwd.x + 6, fwd.y + 6, SYM_RIGHT, 16, can_fwd ? style.text : style.tertiary);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN) {
        if (ui_hover(ui, back) && can_back) {
            ui->consumed = true;
            int to = s->back_n ? s->back[--s->back_n] : panes[s->pane].parent;
            if (s->forward_n < HISTORY)
                s->forward[s->forward_n++] = s->pane;
            s->pane = to;
            s->scroll = 0;
        } else if (ui_hover(ui, fwd) && can_fwd) {
            ui->consumed = true;
            if (s->back_n < HISTORY)
                s->back[s->back_n++] = s->pane;
            s->pane = s->forward[--s->forward_n];
            s->scroll = 0;
        }
    }
    ui_label(ui, (struct rect){cx + 82, 0, W - cx - 100, TOOLBAR_HEIGHT}, panes[s->pane].name, &font_bold, style.text,
             ALIGN_LEFT);
}

static void frame(struct ui *ui, void *data)
{
    struct settings *s = data;
    struct window *w = ui->win;
    int W = w->surface.w, H = w->surface.h;
    int cx = SIDEBAR_W + 1, wdt = MIN(W - cx - 56, 660), x = cx + (W - cx - wdt) / 2;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){SIDEBAR_W, 0, W - SIDEBAR_W, H}, style.window);
        ui_sidebar_panel(ui, SIDEBAR_W, H, style.window);
    }
    sidebar(ui, s, H, wdt);
    if (!ui->win)
        return;
    toolbar(ui, s, cx, W);
    struct rect content = {cx, TOOLBAR_HEIGHT, W - cx, H - TOOLBAR_HEIGHT};
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(content, ui->e->x, ui->e->y)) {
        s->scroll += ui->e->wheel * 40;
        ui->changed = true;
    }
    const char *query = s->search.len ? s->search.buf : NULL;
    struct ui quiet = *ui;
    quiet.c = NULL;
    quiet.e = NULL;
    struct page m = {&quiet, s, x, 0, wdt, true, query, 0, 0, 0, 0, s->sec_h, s->sec_rows};
    panes[s->pane].fn(&m);
    s->height = m.y;
    s->scroll = CLAMP(s->scroll, 0, MAX(0, s->height + 16 - content.h));
    struct canvas *saved = ui->c, clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, content);
        ui->c = &clipped;
    }
    struct page p = {ui, s, x, TOOLBAR_HEIGHT + 6 - s->scroll, wdt, false, query, 0, 0, 0, 0, s->sec_h, s->sec_rows};
    panes[s->pane].fn(&p);
    if (ui->win && saved)
        ui->c = saved;
}

static void paint(struct window *w, struct canvas *c)
{
    struct settings *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct settings *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    struct settings *s = w->data;
    for (int i = 0; i < WALLS; i++)
        canvas_free(&s->thumbs[i]);
    for (int i = 0; i < 8; i++)
        canvas_free(&s->look_thumbs[i]);
    if (instance == w)
        instance = NULL;
    kfree(s);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void add_theme(struct settings *s, const char *name)
{
    for (int i = 0; i < s->theme_count; i++)
        if (!strcmp(s->themes[i], name))
            return;
    if (s->theme_count < (int)ARRAY_SIZE(s->themes))
        str_copy(s->themes[s->theme_count++], name, sizeof(s->themes[0]));
}

static void scan_themes(struct settings *s, const char *dir)
{
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        const char *ext = path_ext(ent.name);
        if (ext && !strcmp(ext, ".theme")) {
            char name[24];
            size_t n = MIN((size_t)(ext - ent.name), sizeof(name) - 1);
            memcpy(name, ent.name, n);
            name[n] = 0;
            add_theme(s, name);
        }
    }
    vfs_closedir(&d);
}

static void load_themes(struct settings *s)
{
    s->theme_count = 0;
    for (size_t i = 0; i < theme_builtin_count(); i++)
        add_theme(s, theme_builtin(i)->name);
    scan_themes(s, "/etc/themes");
    scan_themes(s, "/home/themes");
    s->theme_list.count = s->theme_count;
    s->theme_list.selected = -1;
    for (int i = 0; i < s->theme_count; i++)
        if (!strcmp(s->themes[i], theme_current()->name))
            s->theme_list.selected = i;
}

static void select_pane(struct settings *s, const char *arg)
{
    if (!arg)
        return;
    arg = path_basename(arg);
    for (int i = 0; i < P_COUNT; i++)
        if (!str_casecmp(arg, panes[i].key) || !str_casecmp(arg, panes[i].name))
            go(s, i);
    if (!str_casecmp(arg, "windows"))
        go(s, P_APPS);
    if (!str_casecmp(arg, "focus"))
        go(s, P_NOTIFY);
}

static void launch(const char *arg)
{
    if (instance && wm_window_alive(instance)) {
        select_pane(instance->data, arg);
        wm_show(instance);
        wm_invalidate(instance);
        return;
    }
    struct settings *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    load_themes(s);
    s->pane = P_APPEARANCE;
    select_pane(s, arg);
    s->back_n = 0;
    char wp[PATH_MAX_LEN];
    current("wallpaper", wp, sizeof(wp));
    if (strchr(wp, '/'))
        ui_text_set(&s->wallpaper_file, wp);
    struct rect a = wm_screen_rect(0);
    int w = CLAMP(a.w - 120, 700, 920), h = CLAMP(a.h - 130, 460, 680);
    instance = wm_open("System Settings", ICON_SETTINGS, w, h, &ops, s,
                       WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!instance) {
        kfree(s);
        return;
    }
    instance->toolbar_h = TOOLBAR_HEIGHT;
    instance->min_w = 680;
    instance->min_h = 440;
}

static const struct gui_app settings_app = {"System Settings", "Appearance, wallpaper, users, displays and more",
                                            ICON_SETTINGS, launch, NULL, NULL};
GUI_APP(settings_app);
