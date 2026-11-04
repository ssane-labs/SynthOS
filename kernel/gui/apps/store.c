#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/net.h>
#include <synth/path.h>
#include <synth/store.h>
#include <synth/string.h>
#include <synth/time.h>

#include "../internal.h"

/* App Store: the catalog of /system/store with a Discover page, categories, what is installed and a page per app.
   Installing blocks, so the window only asks and the desktop loop does it (store_run) under the progress card. */

#define SIDEBAR_W 220
#define SIDE_ROW 28
#define MAX_CATS 8
#define TILE_H 78

enum { ASK_NONE, ASK_GET, ASK_REMOVE, ASK_REFRESH };

struct store_win {
    struct ui_state st;
    struct ui_text search;
    int page; /* 0 Discover, 1..cats categories, cats + 1 Installed */
    int detail;
    int scroll, height;
    int cats;
    const char *cat[MAX_CATS];
    enum store_state state[64];
    bool states_ok;
};

static struct window *instance;
static int ask;
static bool ask_quiet;
static char ask_id[24], busy_id[24], waiting_id[24];
static uint64_t waiting_since, desktop_since;
static char card_title[64];
static bool auto_done;

static bool contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!str_ncasecmp(hay, needle, n))
            return true;
    return false;
}

static enum icon_id app_icon(const struct store_app *a)
{
    return !strcmp(a->icon, "browser") ? ICON_BROWSER
         : !strcmp(a->icon, "code")    ? ICON_CODE
         : !strcmp(a->icon, "games")   ? ICON_GAMES
         : !strcmp(a->icon, "terminal") ? ICON_TERMINAL
                                       : ICON_APP;
}

static enum symbol_id cat_symbol(const char *name)
{
    return contains(name, "browser") ? SYM_GLOBE
         : contains(name, "develop") ? SYM_TERMINAL
         : contains(name, "game")    ? SYM_PLAY
         : contains(name, "music") || contains(name, "video") ? SYM_MUSIC
                                     : SYM_APPS;
}

static void refresh_states(struct store_win *s)
{
    s->cats = store_categories(s->cat, MAX_CATS);
    for (int i = 0; i < store_count() && i < (int)ARRAY_SIZE(s->state); i++)
        s->state[i] = store_state(store_app(i));
    if (waiting_id[0]) {
        const struct store_app *a = store_find(waiting_id);
        if (!a || store_state(a) == STORE_INSTALLED || hal_ticks_ms() - waiting_since > 600000)
            waiting_id[0] = 0;
    }
    s->states_ok = true;
}

static int index_of(const struct store_app *a)
{
    for (int i = 0; i < store_count(); i++)
        if (store_app(i) == a)
            return i;
    return -1;
}

static void go(struct store_win *s, int page)
{
    s->page = page;
    s->detail = -1;
    s->scroll = 0;
    app_state_set_number("store", "page", page);
}

static void open_app(const struct store_app *a)
{
    char path[PATH_MAX_LEN];
    if (a->bin[0] && !a->open[0]) {
        gui_app_launch(gui_app_find("Terminal"), NULL);
        return;
    }
    if (!store_program(a, path, sizeof(path))) {
        gui_notify(a->name, "The program is not on the disk.");
        return;
    }
    const struct gui_app *app = gui_app_for_program(path);
    if (app)
        gui_app_launch(app, NULL);
    else if (!gui_start_program(path, NULL))
        gui_notify(a->name, "The program did not start.");
}

static void request(const struct store_app *a, int op)
{
    if (ask || busy_id[0])
        return;
    if (a && a->kind == STORE_CHROME && op == ASK_GET) {
        gui_app_launch(gui_app_find("Chrome"), NULL);
        return;
    }
    str_copy(ask_id, a ? a->id : "", sizeof(ask_id));
    str_copy(busy_id, a ? a->id : "-", sizeof(busy_id));
    ask = op;
    ask_quiet = false;
}

static void remove_done(struct window *owner, const char *value)
{
    UNUSED(value);
    struct store_win *s = owner ? owner->data : NULL;
    if (s && s->detail >= 0 && s->detail < store_count())
        request(store_app(s->detail), ASK_REMOVE);
}

/* The rounded button of an app: GET, OPEN or UPDATE. */
static bool action(struct ui *ui, struct rect r, const struct store_app *a, enum store_state st)
{
    bool busy = !strcmp(busy_id, a->id) || !strcmp(waiting_id, a->id);
    const char *label = busy                      ? "..."
                      : st == STORE_INSTALLED     ? "OPEN"
                      : st == STORE_UPDATE        ? "UPDATE"
                      : st == STORE_UNAVAILABLE   ? (a->kind == STORE_BUILTIN ? "SOON" : "64-BIT")
                                                  : "GET";
    bool hot = ui_hover(ui, r) && st != STORE_UNAVAILABLE && !busy;
    if (ui->c) {
        uint32_t back = st == STORE_UNAVAILABLE ? ALPHA(style.text, 12) : ALPHA(style.text, hot ? 34 : 22);
        gfx_round(ui->c, r, r.h / 2, back);
        if (busy) {
            int w = r.w / 2, t = (int)(hal_ticks_ms() / 6 % (uint64_t)(w + 1));
            struct rect bar = {r.x + (r.w - w) / 2, r.y + r.h / 2 - 2, w, 4};
            gfx_round(ui->c, bar, 2, ALPHA(style.accent, 70));
            gfx_round(ui->c, (struct rect){bar.x, bar.y, MAX(t, 4), 4}, 2, style.accent);
        } else {
            gfx_text_box(ui->c, r, &font_bold, label, st == STORE_UNAVAILABLE ? style.tertiary : style.accent,
                         ALIGN_CENTER);
        }
    }
    if (hot && ui->e && ui->e->type == EV_MOUSE_DOWN) {
        ui->consumed = true;
        return true;
    }
    return false;
}

static void press(const struct store_app *a, enum store_state st)
{
    if (st == STORE_INSTALLED)
        open_app(a);
    else if (st == STORE_GET || st == STORE_UPDATE)
        request(a, ASK_GET);
}

static void tile(struct ui *ui, struct store_win *s, struct rect r, int i, bool line)
{
    const struct store_app *a = store_app(i);
    enum store_state st = s->state[i];
    struct rect btn = {r.x + r.w - 82, r.y + (r.h - 28) / 2, 76, 28};
    if (action(ui, btn, a, st))
        press(a, st);
    if (ui->c) {
        gfx_icon(ui->c, r.x + 4, r.y + (r.h - 56) / 2, app_icon(a), 56);
        int tx = r.x + 74, tw = btn.x - tx - 10;
        gfx_text_box(ui->c, (struct rect){tx, r.y + 16, tw, 20}, &font_bold, a->name, style.text, ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){tx, r.y + 36, tw, 16}, &font_small, a->summary, style.secondary,
                     ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){tx, r.y + 52, tw, 16}, &font_small, a->category, style.tertiary,
                     ALIGN_LEFT);
        if (line)
            gfx_fill(ui->c, (struct rect){tx, r.y + r.h - 1, r.w - (tx - r.x), 1}, style.separator);
    }
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && !ui->consumed && ui_hover(ui, r)) {
        ui->consumed = true;
        s->detail = i;
        s->scroll = 0;
    }
}

/* Lays out the apps that pass want() in columns; returns the height used. */
static int grid(struct ui *ui, struct store_win *s, int x, int y, int w, bool (*want)(struct store_win *, int, void *),
                void *ctx, int limit)
{
    int cols = w >= 900 ? 3 : w >= 560 ? 2 : 1, gap = 24, cw = (w - gap * (cols - 1)) / cols;
    int shown[64], n = 0;
    for (int i = 0; i < store_count() && n < limit && n < 64; i++)
        if (want(s, i, ctx))
            shown[n++] = i;
    int rows = (n + cols - 1) / cols;
    for (int k = 0; k < n; k++) {
        int col = k % cols, row = k / cols;
        tile(ui, s, (struct rect){x + col * (cw + gap), y + row * TILE_H, cw, TILE_H}, shown[k], row < rows - 1);
    }
    return rows * TILE_H;
}

static bool in_category(struct store_win *s, int i, void *ctx)
{
    UNUSED(s);
    return !strcmp(store_app(i)->category, ctx);
}

static bool installed(struct store_win *s, int i, void *ctx)
{
    UNUSED(ctx);
    return s->state[i] == STORE_INSTALLED || s->state[i] == STORE_UPDATE;
}

static bool updates(struct store_win *s, int i, void *ctx)
{
    UNUSED(ctx);
    return s->state[i] == STORE_UPDATE;
}

static bool matches(struct store_win *s, int i, void *ctx)
{
    UNUSED(s);
    const struct store_app *a = store_app(i);
    const char *q = ctx;
    return contains(a->name, q) || contains(a->summary, q) || contains(a->developer, q) || contains(a->category, q);
}

static int heading(struct ui *ui, int x, int y, int w, const char *text, const char *more, bool *clicked)
{
    ui_label(ui, (struct rect){x, y, w - 90, 28}, text, &font_title, style.text, ALIGN_LEFT);
    if (more) {
        struct rect r = {x + w - 80, y + 4, 80, 22};
        ui_label(ui, r, more, &font_sans, style.accent, ALIGN_RIGHT);
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, r)) {
            ui->consumed = true;
            *clicked = true;
        }
    }
    return 36;
}

static int hero(struct ui *ui, struct store_win *s, int x, int y, int w)
{
    int pick = -1;
    for (int i = 0; i < store_count() && pick < 0; i++)
        if (s->state[i] == STORE_GET)
            pick = i;
    if (pick < 0)
        pick = 0;
    const struct store_app *a = store_app(pick);
    struct rect card = {x, y, w, 188};
    if (ui->c) {
        gfx_round(ui->c, card, 22, style.group);
        gfx_icon(ui->c, card.x + card.w - 170, card.y + 24, app_icon(a), 140);
        gfx_text_box(ui->c, (struct rect){card.x + 28, card.y + 24, 300, 16}, &font_small,
                     s->state[pick] == STORE_GET ? "FEATURED" : "ON YOUR COMPUTER", style.accent, ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){card.x + 28, card.y + 44, card.w - 230, 34}, &font_large, a->name,
                     style.text, ALIGN_LEFT);
        ui_wrap_text(ui->c, (struct rect){card.x + 28, card.y + 84, MIN(card.w - 230, 420), 40}, &font_sans,
                     a->summary, style.secondary);
    }
    struct rect btn = {card.x + 28, card.y + card.h - 50, 92, 30};
    if (action(ui, btn, a, s->state[pick]))
        press(a, s->state[pick]);
    if (ui->e && ui->e->type == EV_MOUSE_DOWN && !ui->consumed && ui_hover(ui, card)) {
        ui->consumed = true;
        s->detail = pick;
        s->scroll = 0;
    }
    return card.h + 28;
}

static int info_row(struct ui *ui, struct rect group, int row, const char *label, const char *value, bool last)
{
    struct rect r = {group.x, group.y + row * 36, group.w, 36};
    ui_group_label(ui, r, label, value, last);
    return row + 1;
}

static int detail(struct ui *ui, struct store_win *s, int x, int y, int w)
{
    const struct store_app *a = store_app(s->detail);
    enum store_state st = s->state[s->detail];
    int y0 = y;
    if (ui->c) {
        gfx_icon(ui->c, x, y, app_icon(a), 112);
        gfx_text_box(ui->c, (struct rect){x + 132, y + 10, w - 132, 34}, &font_large, a->name, style.text, ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){x + 132, y + 46, w - 132, 18}, &font_sans, a->developer, style.secondary,
                     ALIGN_LEFT);
    }
    struct rect btn = {x + 132, y + 76, 92, 30};
    if (action(ui, btn, a, st))
        press(a, st);
    if ((st == STORE_INSTALLED || st == STORE_UPDATE) && a->kind != STORE_BUILTIN && !busy_id[0] &&
        ui_button(ui, (struct rect){btn.x + btn.w + 12, btn.y + 1, 92, 28}, "Remove")) {
        char text[160];
        ksnprintf(text, sizeof(text), "%s will be removed from this computer.", a->name);
        ui_confirm(ui->win, "Remove the app?", text, remove_done);
    }
    y += 136;
    if (a->note[0]) {
        struct rect n = {x, y, w, 40};
        if (ui->c) {
            gfx_round(ui->c, n, 12, ALPHA(style.warning, 36));
            gfx_symbol(ui->c, n.x + 14, n.y + 12, SYM_INFO, 16, style.warning);
            gfx_text_box(ui->c, (struct rect){n.x + 40, n.y, n.w - 50, n.h}, &font_sans, a->note, style.text,
                         ALIGN_LEFT);
        }
        y += 56;
    }
    y += heading(ui, x, y, w, "About", NULL, NULL);
    y += ui_wrap_text(ui->c, (struct rect){x, y, MIN(w, 640), 1000}, &font_sans, a->about[0] ? a->about : a->summary,
                      style.text) + 24;
    y += heading(ui, x, y, w, "Information", NULL, NULL);
    char where[96];
    if (a->kind == STORE_ZIP)
        ksnprintf(where, sizeof(where), "/home/apps/%s", a->id);
    else if (a->kind == STORE_INSTALLER)
        str_copy(where, "The developer's own installer", sizeof(where));
    else if (a->kind == STORE_CHROME)
        str_copy(where, "Google's update server", sizeof(where));
    else
        str_copy(where, "Comes with SynthOS", sizeof(where));
    int rows = 6;
    struct rect g = ui_group(ui, x, y, MIN(w, 640), rows, 36);
    int r = 0;
    r = info_row(ui, g, r, "Developer", a->developer, false);
    r = info_row(ui, g, r, "Category", a->category, false);
    r = info_row(ui, g, r, "Version", a->version[0] ? a->version : "-", false);
    r = info_row(ui, g, r, "Size", a->size[0] ? a->size : "-", false);
    r = info_row(ui, g, r, "Source", where, false);
    info_row(ui, g, r, "Compatibility", a->bits == 64 ? "64-bit SynthOS" : "SynthOS, 32 and 64-bit", true);
    y += rows * 36 + 24;
    return y - y0;
}

static void sidebar(struct ui *ui, struct store_win *s, int H)
{
    if (ui_search(ui, (struct rect){16, 44, SIDEBAR_W - 28, 28}, &s->search, "Search")) {
        s->detail = -1;
        s->scroll = 0;
    }
    int y = 88;
    int count = 0;
    for (int i = 0; i < store_count(); i++)
        count += s->state[i] == STORE_UPDATE;
    struct {
        enum symbol_id sym;
        const char *name;
    } items[MAX_CATS + 2];
    int n = 0;
    items[n++] = (typeof(items[0])){SYM_STAR, "Discover"};
    for (int i = 0; i < s->cats; i++)
        items[n++] = (typeof(items[0])){cat_symbol(s->cat[i]), s->cat[i]};
    items[n++] = (typeof(items[0])){count ? SYM_DOWNLOAD : SYM_CHECK, count ? "Updates" : "Installed"};
    for (int i = 0; i < n && y < H - SIDE_ROW; i++) {
        if (i == n - 1) {
            ui_sidebar_header(ui, (struct rect){20, y + 4, SIDEBAR_W - 30, 20}, "Library");
            y += 28;
        }
        if (ui_sidebar_item(ui, (struct rect){12, y, SIDEBAR_W - 24, SIDE_ROW}, items[i].sym, items[i].name,
                            !s->search.len && s->page == i && s->detail < 0)) {
            ui_text_set(&s->search, "");
            go(s, i);
        }
        y += SIDE_ROW + 2;
    }
}

static void toolbar(struct ui *ui, struct store_win *s, int cx, int W)
{
    const char *title = s->search.len ? "Search" : s->page == 0 ? "Discover"
                      : s->page <= s->cats ? s->cat[s->page - 1] : "Installed";
    int tx = cx + 20;
    if (s->detail >= 0) {
        struct rect back = {cx + 14, (TOOLBAR_HEIGHT - 28) / 2, 28, 28};
        if (ui->c)
            gfx_symbol(ui->c, back.x + 6, back.y + 6, SYM_LEFT, 16, style.text);
        if (ui->e && ui->e->type == EV_MOUSE_DOWN && ui_hover(ui, back)) {
            ui->consumed = true;
            s->detail = -1;
            s->scroll = 0;
        }
        tx = cx + 50;
    }
    ui_label(ui, (struct rect){tx, 0, W - tx - 60, TOOLBAR_HEIGHT}, title, &font_bold, style.text, ALIGN_LEFT);
    struct rect reload = {W - 84, (TOOLBAR_HEIGHT - 28) / 2, 28, 28};
    if (ui_symbol_button(ui, reload, SYM_RESTART, false))
        request(NULL, ASK_REFRESH);
}

static void body(struct ui *ui, struct store_win *s, int x, int y, int w)
{
    int y0 = y;
    bool more = false;
    if (s->detail >= 0 && s->detail < store_count()) {
        y += detail(ui, s, x, y, w);
    } else if (s->search.len) {
        int h = grid(ui, s, x, y, w, matches, s->search.buf, 64);
        if (!h)
            ui_label(ui, (struct rect){x, y + 40, w, 24}, "No Results", &font_title, style.secondary, ALIGN_CENTER);
        y += MAX(h, 100);
    } else if (s->page == 0) {
        y += hero(ui, s, x, y, w);
        for (int c = 0; c < s->cats; c++) {
            more = false;
            y += heading(ui, x, y, w, s->cat[c], "See All", &more);
            if (more)
                go(s, c + 1);
            y += grid(ui, s, x, y, w, in_category, (void *)s->cat[c], w >= 900 ? 6 : 4) + 24;
        }
    } else if (s->page <= s->cats) {
        y += grid(ui, s, x, y, w, in_category, (void *)s->cat[s->page - 1], 64);
    } else {
        int h = grid(ui, s, x, y, w, updates, NULL, 64);
        if (h) {
            y += heading(ui, x, y, w, "Updates", NULL, NULL);
            y += grid(ui, s, x, y, w, updates, NULL, 64) + 24;
            y += heading(ui, x, y, w, "Installed", NULL, NULL);
        }
        int n = grid(ui, s, x, y, w, installed, NULL, 64);
        if (!n)
            ui_label(ui, (struct rect){x, y + 40, w, 24}, "Nothing installed from the App Store yet", &font_sans,
                     style.secondary, ALIGN_CENTER);
        y += MAX(n, 100);
    }
    s->height = y - y0;
}

static void frame(struct ui *ui, void *data)
{
    struct store_win *s = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (!s->states_ok || (ui->e && ui->e->type == EV_TIMER))
        refresh_states(s);
    if (s->page > s->cats + 1)
        s->page = 0;
    int cx = SIDEBAR_W + 1;
    if (ui->c) {
        gfx_clear(ui->c, (struct rect){SIDEBAR_W, 0, W - SIDEBAR_W, H}, style.window);
        ui_sidebar_panel(ui, SIDEBAR_W, H, style.window);
    }
    sidebar(ui, s, H);
    if (!ui->win)
        return;
    toolbar(ui, s, cx, W);
    struct rect content = {cx, TOOLBAR_HEIGHT, W - cx, H - TOOLBAR_HEIGHT};
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains(content, ui->e->x, ui->e->y)) {
        s->scroll += ui->e->wheel * 40;
        ui->changed = true;
    }
    if (ui->e && ui->e->type == EV_KEY && !s->search.len && s->st.focus == 0) {
        int k = ui->e->key;
        if (k == KEY_DOWN || k == KEY_PGDN)
            s->scroll += k == KEY_DOWN ? 40 : content.h - 60;
        else if (k == KEY_UP || k == KEY_PGUP)
            s->scroll -= k == KEY_UP ? 40 : content.h - 60;
        else if ((k == KEY_BACKSPACE || k == KEY_ESC) && s->detail >= 0)
            s->detail = -1;
        else
            k = 0;
        if (k) {
            ui->consumed = true;
            ui->changed = true;
        }
    }
    s->scroll = CLAMP(s->scroll, 0, MAX(0, s->height + 40 - content.h));
    int w = MIN(content.w - 56, 1100), x = cx + (content.w - w) / 2;
    struct canvas *saved = ui->c, clipped;
    if (ui->c) {
        clipped = *ui->c;
        clipped.clip = rect_intersect(ui->c->clip, content);
        ui->c = &clipped;
    }
    if (ui->e && !rect_contains(content, ui->e->x, ui->e->y) && ui->e->type == EV_MOUSE_DOWN) {
        struct gui_event *e = ui->e;
        ui->e = NULL;
        body(ui, s, x, TOOLBAR_HEIGHT + 12 - s->scroll, w);
        ui->e = e;
    } else {
        body(ui, s, x, TOOLBAR_HEIGHT + 12 - s->scroll, w);
    }
    if (ui->win && saved)
        ui->c = saved;
    bool busy = busy_id[0] || waiting_id[0];
    uint32_t tick = busy ? 60 : 2000;
    if (ui->win->timer_ms != tick)
        wm_set_timer(ui->win, tick);
}

static void paint(struct window *w, struct canvas *c)
{
    struct store_win *s = w->data;
    ui_run(w, &s->st, c, NULL, frame, s);
}

static void event(struct window *w, struct gui_event *e)
{
    struct store_win *s = w->data;
    ui_run(w, &s->st, NULL, e, frame, s);
}

static void destroy(struct window *w)
{
    if (instance == w)
        instance = NULL;
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void show_app(struct store_win *s, const char *id)
{
    const struct store_app *a = id && *id ? store_find(path_basename(id)) : NULL;
    if (a)
        s->detail = index_of(a);
}

static void launch(const char *arg)
{
    if (instance) {
        if (arg && *arg) {
            show_app(instance->data, arg);
            wm_invalidate(instance);
        }
        wm_show(instance);
        return;
    }
    store_load();
    struct store_win *s = kzalloc(sizeof(*s));
    if (!s)
        return;
    s->detail = -1;
    s->page = (int)app_state_number("store", "page", 0);
    if (s->page < 0)
        s->page = 0;
    show_app(s, arg);
    struct rect a = wm_screen_rect(0);
    int w = CLAMP(a.w - 160, 720, 1080), h = CLAMP(a.h - 140, 480, 760);
    instance = wm_open("App Store", ICON_STORE, w, h, &ops, s, WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!instance) {
        kfree(s);
        return;
    }
    instance->toolbar_h = TOOLBAR_HEIGHT;
    instance->min_w = 640;
    instance->min_h = 420;
}

static const struct gui_app store_app_entry = {"App Store", "Browsers, developer tools and games for SynthOS",
                                               ICON_STORE, launch, NULL, NULL};
GUI_APP(store_app_entry);

/* --- the work, from the desktop loop --- */

static void card_say(void *ctx, const char *text)
{
    UNUSED(ctx);
    progress_open(card_title, text);
}

static bool start_installer(void *ctx, const char *path, const char *args)
{
    UNUSED(ctx);
    return gui_start_program(path, args);
}

static void day_stamp(char *out, size_t cap)
{
    struct rtc_time t;
    time_local(&t);
    ksnprintf(out, cap, "%04d-%02d-%02d", t.year, t.month, t.day);
}

static void finish(void)
{
    busy_id[0] = 0;
    if (instance) {
        ((struct store_win *)instance->data)->states_ok = false;
        wm_invalidate(instance);
    }
}

void store_run(void)
{
    if (!desktop_since)
        desktop_since = hal_ticks_ms();
    if (!ask && !auto_done && hal_ticks_ms() - desktop_since > 90000 && !gui_locked()) {
        auto_done = true;
        char day[16];
        day_stamp(day, sizeof(day));
        if (net_interface_count() && net_interface(0)->configured && strcmp(app_state("store", "refreshed", ""), day)) {
            ask = ASK_REFRESH;
            ask_quiet = true;
        }
    }
    if (!ask)
        return;
    int op = ask;
    ask = ASK_NONE;
    char msg[160];
    struct store_ui u = {.progress = progress_update, .say = card_say, .run = start_installer};
    if (op == ASK_REFRESH) {
        if (!ask_quiet)
            progress_open("App Store", "Looking for new apps");
        int err = store_refresh(ask_quiet ? NULL : &u, msg, sizeof(msg));
        progress_close();
        if (!err) {
            char day[16];
            day_stamp(day, sizeof(day));
            app_state_set("store", "refreshed", day);
        }
        if (!ask_quiet)
            gui_notify("App Store", err ? msg : "The catalog is up to date.");
        ask_quiet = false;
        finish();
        return;
    }
    const struct store_app *a = store_find(ask_id);
    if (!a) {
        finish();
        return;
    }
    if (op == ASK_REMOVE) {
        char uninstall[PATH_MAX_LEN];
        int err = store_remove(a, uninstall, sizeof(uninstall), msg, sizeof(msg));
        if (!err && uninstall[0] && !gui_start_command(uninstall))
            ksnprintf(msg, sizeof(msg), "The uninstaller of %s did not start", a->name);
        gui_notify(a->name, msg);
        finish();
        return;
    }
    ksnprintf(card_title, sizeof(card_title), "%s", a->name);
    progress_open(card_title, "Downloading");
    int err = store_install(a, &u, msg, sizeof(msg));
    progress_close();
    if (!err && a->kind == STORE_INSTALLER) {
        str_copy(waiting_id, a->id, sizeof(waiting_id));
        waiting_since = hal_ticks_ms();
    }
    gui_notify(err ? "Could not install" : a->name, err ? msg : a->kind == STORE_INSTALLER ? msg : "Installed");
    finish();
}
