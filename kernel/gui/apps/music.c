#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

#define MAX_FILES 64
#define ROW 40
#define TOP 236

enum {
    CMD_PLAY,
    CMD_NEXT,
    CMD_PREV,
    CMD_OPEN,
    CMD_FOLDER,
};

static const struct menu_item play_items[] = {
    {"Play / Pause", KEY_COMMAND(CMD_PLAY), "Space"}, {"Next", KEY_COMMAND(CMD_NEXT), "→"},
    {"Previous", KEY_COMMAND(CMD_PREV), "←"},         {"", 0, NULL},
    {"Open File…", KEY_COMMAND(CMD_OPEN), "⌘O"},     {"Show Music Folder", KEY_COMMAND(CMD_FOLDER), NULL},
};

static const struct app_menu menus[] = {{"Music", play_items, ARRAY_SIZE(play_items)}};

struct music {
    char files[MAX_FILES][NAME_MAX_LEN];
    int file_count, hover, pressed_btn, hover_btn;
    char dir[PATH_MAX_LEN];
    struct spring pulse, hl, hl_alpha, pos;
    int levels[MEDIA_BANDS];
    uint64_t tick;
    int scroll;
};

static int total(struct music *m)
{
    return media_count() + m->file_count;
}

static void scan(struct music *m)
{
    m->file_count = 0;
    path_join(user_home(), "Music", m->dir, sizeof(m->dir));
    struct vdir d;
    if (vfs_opendir(m->dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (m->file_count < MAX_FILES && vfs_readdir(&d, &ent) == 0) {
        const char *ext = path_ext(ent.name);
        if (ent.type != VN_DIR && !str_casecmp(ext, ".wav"))
            str_copy(m->files[m->file_count++], ent.name, NAME_MAX_LEN);
    }
    vfs_closedir(&d);
}

static void play_index(struct music *m, int i)
{
    if (i < media_count()) {
        media_play(i);
        return;
    }
    char path[PATH_MAX_LEN];
    path_join(m->dir, m->files[i - media_count()], path, sizeof(path));
    if (media_open(path) != 0)
        gui_message("Cannot play this file", "Music plays PCM WAV files (8 or 16 bit, mono or stereo).");
}

static void art(struct canvas *c, struct rect r, int index, int pulse)
{
    static const uint32_t tops[3] = {RGB(255, 120, 120), RGB(250, 196, 140), RGB(110, 150, 230)};
    static const uint32_t bottoms[3] = {RGB(90, 30, 120), RGB(150, 90, 120), RGB(30, 40, 90)};
    int i = index < 0 ? 2 : index % 3;
    int grow = pulse * 6 / 1024;
    r = rect_inset(r, -grow);
    gfx_drop_shadow(c, r, 22, 10, 60, 3);
    gfx_round_gradient(c, r, 22, tops[i], bottoms[i]);
    struct canvas clip = *c;
    clip.clip = rect_intersect(c->clip, r);
    int cx = r.x + r.w / 2, cy = r.y + r.h * 58 / 100;
    gfx_glow(&clip, (struct rect){cx - r.w / 4, cy - r.w / 4, r.w / 2, r.w / 2}, r.w / 4, r.w / 6, 120, RGB(255, 220, 140));
    gfx_circle(&clip, cx, cy, r.w / 4, ALPHA(RGB(255, 236, 160), 240));
    for (int k = 0; k < 5; k++)
        gfx_fill(&clip, (struct rect){r.x, cy + k * 7 - 2, r.w, 2 + k}, ALPHA(bottoms[i], 210));
    gfx_fill(&clip, (struct rect){r.x, cy + 30, r.w, r.h}, ALPHA(bottoms[i], 230));
}

static struct rect button(int W, int i)
{
    int cx = 196 + 24 + i * 56;
    UNUSED(W);
    return (struct rect){cx - 22, 156, 44, 44};
}

static void paint(struct window *w, struct canvas *c)
{
    struct music *m = w->data;
    int W = c->w, H = c->h;
    gfx_clear(c, (struct rect){0, 0, W, H}, style.content);
    const struct media_track *t = media_track();
    art(c, (struct rect){24, 24, 152, 152}, t->index, CLAMP(m->pulse.pos, 0, 1024));
    gfx_text_box(c, (struct rect){196, 34, W - 216, 28}, &font_title, t->title, style.text, ALIGN_LEFT);
    gfx_text_box(c, (struct rect){196, 64, W - 216, 20}, &font_sans, t->artist, style.secondary, ALIGN_LEFT);
    struct rect bar = {196, 110, W - 220, 5};
    gfx_round(c, bar, 2, ALPHA(style.text, 40));
    int fill = bar.w * CLAMP(m->pos.pos, 0, 1024) / 1024;
    gfx_round(c, (struct rect){bar.x, bar.y, fill, 5}, 2, style.accent);
    gfx_circle(c, bar.x + fill, bar.y + 2, 6, style.accent);
    char a[16], b[16];
    ksnprintf(a, sizeof(a), "%d:%02d", t->position_ms / 60000, t->position_ms / 1000 % 60);
    ksnprintf(b, sizeof(b), "%d:%02d", t->length_ms / 60000, t->length_ms / 1000 % 60);
    gfx_text_box(c, (struct rect){bar.x, bar.y + 10, 80, 16}, &font_small, a, style.secondary, ALIGN_LEFT);
    gfx_text_box(c, (struct rect){bar.x + bar.w - 80, bar.y + 10, 80, 16}, &font_small, b, style.secondary, ALIGN_RIGHT);
    static const enum symbol_id syms[3] = {SYM_PREV, SYM_PLAY, SYM_NEXT};
    for (int i = 0; i < 3; i++) {
        struct rect r = button(W, i);
        int cx = r.x + 22, cy = r.y + 22;
        bool main = i == 1, hot = m->hover_btn == i;
        int rad = main ? 22 : 18;
        if (m->pressed_btn == i)
            rad -= 2;
        if (main)
            gfx_circle(c, cx, cy, rad, hot ? color_shade(style.accent, 24) : style.accent);
        else if (hot)
            gfx_circle(c, cx, cy, rad, style.hover);
        enum symbol_id s = main && t->playing ? SYM_PAUSE : syms[i];
        gfx_symbol(c, cx - 8, cy - 8, s, 16, main ? style.accent_text : style.text);
    }
    int vx = 196 + 3 * 56 + 30, vw = W - vx - 24;
    if (vw > 60) {
        int bw = MAX(2, (vw - (MEDIA_BANDS - 1) * 4) / MEDIA_BANDS);
        for (int i = 0; i < MEDIA_BANDS; i++) {
            int h = 3 + 38 * CLAMP(m->levels[i], 0, 1100) / 1100;
            gfx_round(c, (struct rect){vx + i * (bw + 4), 196 - h, bw, h}, 2, ALPHA(style.accent, 200));
        }
    }
    gfx_fill(c, (struct rect){24, TOP - 14, W - 48, 1}, style.separator);
    if (m->hl_alpha.pos > 8)
        gfx_round(c, (struct rect){16, m->hl.pos, W - 32, ROW - 4}, 12, ALPHA(style.text, CLAMP(m->hl_alpha.pos, 0, 1024) * 18 / 1024));
    struct canvas list = *c;
    list.clip = rect_intersect(c->clip, (struct rect){0, TOP - 6, W, H - TOP + 6});
    for (int i = 0; i < total(m); i++) {
        int y = TOP + i * ROW - m->scroll;
        if (y > H)
            break;
        bool current = (i < media_count() && t->index == i) ||
                       (i >= media_count() && t->index < 0 && !strncmp(t->title, m->files[i - media_count()], strlen(t->title)));
        const char *title = i < media_count() ? media_song_title(i) : m->files[i - media_count()];
        const char *sub = i < media_count() ? media_song_artist(i) : "WAV file";
        if (current && t->playing)
            gfx_symbol(&list, 30, y + 10, SYM_MUSIC, 16, style.accent);
        else {
            char n[8];
            ksnprintf(n, sizeof(n), "%d", i + 1);
            gfx_text_box(&list, (struct rect){24, y, 28, ROW - 4}, &font_small, n, style.tertiary, ALIGN_CENTER);
        }
        gfx_text_box(&list, (struct rect){60, y, W / 2, ROW - 4}, current ? &font_bold : &font_sans, title,
                     current ? style.accent : style.text, ALIGN_LEFT);
        gfx_text_box(&list, (struct rect){W / 2 + 40, y, W / 2 - 80, ROW - 4}, &font_small, sub, style.secondary, ALIGN_RIGHT);
    }
}

static int row_at(struct music *m, int y)
{
    if (y < TOP - 4)
        return -1;
    int i = (y - TOP + 4 + m->scroll) / ROW;
    return i < total(m) ? i : -1;
}

static int button_at(struct window *w, int x, int y)
{
    for (int i = 0; i < 3; i++)
        if (rect_contains(button(w->surface.w, i), x, y))
            return i;
    return -1;
}

static void opened(struct window *owner, const char *value)
{
    UNUSED(owner);
    if (value && value[0] && media_open(value) != 0)
        gui_message("Cannot play this file", "Music plays PCM WAV files (8 or 16 bit, mono or stereo).");
}

static void command(struct window *w, struct music *m, int cmd)
{
    switch (cmd) {
    case CMD_PLAY: media_toggle(); break;
    case CMD_NEXT: media_next(); break;
    case CMD_PREV: media_prev(); break;
    case CMD_OPEN: ui_prompt(w, "Open music", "Path to a WAV file:", m->dir, opened); break;
    case CMD_FOLDER:
        vfs_mkdirs(m->dir);
        gui_app_launch(gui_app_find("Files"), m->dir);
        break;
    }
    spring_set(&m->pulse, 0);
    m->pulse.vel = 9000;
    wm_invalidate(w);
}

static void event(struct window *w, struct gui_event *e)
{
    struct music *m = w->data;
    switch (e->type) {
    case EV_KEY:
        if (e->key >= KEY_COMMAND(0) && e->key <= KEY_COMMAND(CMD_FOLDER))
            command(w, m, e->key - KEY_COMMAND(0));
        else if (e->key == ' ')
            command(w, m, CMD_PLAY);
        else if (e->key == KEY_RIGHT)
            command(w, m, CMD_NEXT);
        else if (e->key == KEY_LEFT)
            command(w, m, CMD_PREV);
        break;
    case EV_MOUSE_MOVE: {
        int r = row_at(m, e->y), b = button_at(w, e->x, e->y);
        if (r != m->hover) {
            if (r >= 0) {
                int y = TOP + r * ROW - m->scroll - 4;
                if (m->hl_alpha.target == 0)
                    spring_set(&m->hl, y);
                m->hl.target = y;
            }
            m->hl_alpha.target = r >= 0 ? 1024 : 0;
            m->hover = r;
        }
        if (b != m->hover_btn) {
            m->hover_btn = b;
            wm_invalidate(w);
        }
        break;
    }
    case EV_LEAVE:
        m->hover = m->hover_btn = -1;
        m->hl_alpha.target = 0;
        break;
    case EV_MOUSE_DOWN: {
        e->handled = true;
        m->pressed_btn = button_at(w, e->x, e->y);
        struct rect bar = {196, 104, w->surface.w - 220, 18};
        if (rect_contains(bar, e->x, e->y)) {
            media_seek((int)((int64_t)(e->x - bar.x) * media_track()->length_ms / MAX(bar.w, 1)));
            if (!media_track()->playing)
                media_resume();
        }
        wm_invalidate(w);
        break;
    }
    case EV_MOUSE_UP: {
        int b = button_at(w, e->x, e->y);
        if (b >= 0 && b == m->pressed_btn)
            command(w, m, b == 0 ? CMD_PREV : b == 1 ? CMD_PLAY : CMD_NEXT);
        else if (m->pressed_btn < 0 && e->button == BUTTON_LEFT) {
            int r = row_at(m, e->y);
            if (r >= 0) {
                play_index(m, r);
                spring_set(&m->pulse, 0);
                m->pulse.vel = 9000;
            }
        }
        m->pressed_btn = -1;
        wm_invalidate(w);
        break;
    }
    case EV_WHEEL: {
        int max = MAX(0, total(m) * ROW - (w->surface.h - TOP));
        m->scroll = CLAMP(m->scroll + e->wheel * ROW, 0, max);
        wm_invalidate(w);
        break;
    }
    case EV_TIMER: {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - m->tick, 64u);
        m->tick = now;
        const struct media_track *t = media_track();
        m->pos.target = t->length_ms ? (int)((int64_t)t->position_ms * 1024 / t->length_ms) : 0;
        bool busy = spring_step(&m->pos, 300, 34, dt);
        busy |= spring_step(&m->pulse, 300, 14, dt);
        busy |= spring_step(&m->hl, 520, 40, dt);
        busy |= spring_step(&m->hl_alpha, 400, 40, dt);
        int lv[MEDIA_BANDS];
        media_levels(lv, MEDIA_BANDS);
        for (int i = 0; i < MEDIA_BANDS; i++) {
            if (lv[i] != m->levels[i])
                busy = true;
            m->levels[i] = (m->levels[i] + lv[i] * 3) / 4;
        }
        if (busy || t->playing)
            wm_invalidate(w);
        break;
    }
    case EV_FOCUS: scan(m); break;
    default: break;
    }
}

static struct window *music_win;

static void destroy(struct window *w)
{
    if (music_win == w)
        music_win = NULL;
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (music_win && wm_window_alive(music_win) && !(arg && *arg)) {
        wm_show(music_win);
        return;
    }
    struct music *m = kzalloc(sizeof(*m));
    if (!m)
        return;
    m->hover = m->pressed_btn = m->hover_btn = -1;
    m->tick = hal_ticks_ms();
    scan(m);
    struct window *w = wm_open("Music", ICON_MUSIC, 560, TOP + 5 * ROW + 16, &ops, m, WIN_RESIZABLE);
    if (!w) {
        kfree(m);
        return;
    }
    music_win = w;
    w->min_w = 460;
    w->min_h = TOP + 2 * ROW;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    wm_set_timer(w, 30);
    if (arg && *arg && media_open(arg) != 0)
        gui_message("Cannot play this file", "Music plays PCM WAV files (8 or 16 bit, mono or stereo).");
}

static const struct gui_app music_app = {"Music", "Play the built-in songs and WAV files", ICON_MUSIC, launch, ".wav", NULL};
GUI_APP(music_app);
