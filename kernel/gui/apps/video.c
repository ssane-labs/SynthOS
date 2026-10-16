#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "../internal.h"

/* Video: AVI files with Motion JPEG pictures and PCM sound. The sound goes through the media engine, the
   picture follows its clock. */

#define MAX_FRAMES 20000
#define BAR_H 46

enum {
    CMD_OPEN,
    CMD_PLAY,
    CMD_RESTART,
    CMD_LOOP,
    CMD_SHOW,
};

static const struct menu_item video_items[] = {
    {"Open…", KEY_COMMAND(CMD_OPEN), "⌘O"},       {"Show in Files", KEY_COMMAND(CMD_SHOW), NULL},
    {"", 0, NULL},                                  {"Play / Pause", KEY_COMMAND(CMD_PLAY), "Space"},
    {"Play from Start", KEY_COMMAND(CMD_RESTART), NULL}, {"Loop", KEY_COMMAND(CMD_LOOP), NULL},
};

static const struct app_menu menus[] = {{"Video", video_items, ARRAY_SIZE(video_items)}};

struct video {
    char path[PATH_MAX_LEN];
    uint8_t *file;
    size_t size;
    uint32_t *offsets, *lengths;
    int frames, shown;
    int w, h;
    uint32_t us_per_frame;
    bool sound, playing, loop;
    struct picture pic;
    uint64_t start, paused_at, tick, last_move;
    struct spring bar;
    char error[96];
    bool dragging;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

struct sound {
    int channels, bits;
    uint32_t rate;
    int16_t *pcm;
    uint32_t count, cap;
};

static void add_sound(struct sound *s, const uint8_t *p, uint32_t len)
{
    if (!s->channels || (s->bits != 8 && s->bits != 16))
        return;
    uint32_t n = len / (uint32_t)(s->channels * s->bits / 8);
    if (s->count + n > s->cap) {
        uint32_t cap = MAX(s->cap * 2, s->count + n + 65536);
        int16_t *grown = krealloc(s->pcm, (size_t)cap * 4);
        if (!grown)
            return;
        s->pcm = grown;
        s->cap = cap;
    }
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            int ch = MIN(c, s->channels - 1);
            int v = s->bits == 16 ? (int16_t)rd16(p + (i * (uint32_t)s->channels + (uint32_t)ch) * 2)
                                  : (p[i * (uint32_t)s->channels + (uint32_t)ch] - 128) << 8;
            s->pcm[(s->count + i) * 2 + c] = (int16_t)v;
        }
    s->count += n;
}

static void walk(struct video *v, struct sound *snd, const uint8_t *p, const uint8_t *end, int *video_stream, int *stream)
{
    while (p + 8 <= end) {
        uint32_t len = rd32(p + 4);
        const uint8_t *body = p + 8;
        if (len > (size_t)(end - body))
            len = (uint32_t)(end - body);
        if (!memcmp(p, "LIST", 4) && len >= 4) {
            walk(v, snd, body + 4, body + len, video_stream, stream);
        } else if (!memcmp(p, "avih", 4) && len >= 40) {
            v->us_per_frame = rd32(body);
            v->w = (int)rd32(body + 32);
            v->h = (int)rd32(body + 36);
        } else if (!memcmp(p, "strh", 4) && len >= 8) {
            (*stream)++;
            if (!memcmp(body, "vids", 4) && *video_stream < 0)
                *video_stream = *stream;
        } else if (!memcmp(p, "strf", 4) && *stream >= 0 && *stream != *video_stream && len >= 16) {
            if (rd16(body) == 1) {
                snd->channels = rd16(body + 2);
                snd->rate = rd32(body + 4);
                snd->bits = rd16(body + 14);
            }
        } else if (p[2] == 'd' && (p[3] == 'c' || p[3] == 'b') && v->frames < MAX_FRAMES && len > 0) {
            v->offsets[v->frames] = (uint32_t)(body - v->file);
            v->lengths[v->frames++] = len;
        } else if (p[2] == 'w' && p[3] == 'b') {
            add_sound(snd, body, len);
        }
        p = body + len + (len & 1);
    }
}

static void stop_sound(struct video *v)
{
    if (v->sound && media_is_file())
        media_stop();
}

static void unload(struct video *v)
{
    stop_sound(v);
    kfree(v->file);
    kfree(v->offsets);
    kfree(v->lengths);
    picture_free(&v->pic);
    v->file = NULL;
    v->offsets = v->lengths = NULL;
    v->frames = 0;
    v->sound = false;
}

static int now_ms(struct video *v)
{
    if (v->sound) {
        const struct media_track *t = media_track();
        return t->position_ms;
    }
    uint64_t at = v->playing ? hal_ticks_ms() : v->paused_at;
    return (int)(at - v->start);
}

static int length_ms(struct video *v)
{
    return (int)((uint64_t)v->frames * v->us_per_frame / 1000);
}

static void play(struct video *v, bool on)
{
    if (on == v->playing)
        return;
    v->playing = on;
    if (v->sound) {
        if (on)
            media_resume();
        else
            media_stop();
    } else if (on) {
        v->start = hal_ticks_ms() - (v->paused_at - v->start);
    } else {
        v->paused_at = hal_ticks_ms();
    }
}

static void seek(struct video *v, int ms)
{
    ms = CLAMP(ms, 0, length_ms(v));
    if (v->sound)
        media_seek(ms);
    uint64_t now = hal_ticks_ms();
    v->start = now - (uint64_t)ms;
    v->paused_at = now;
    v->shown = -1;
}

static void open_file(struct window *w, struct video *v, const char *path)
{
    unload(v);
    v->error[0] = 0;
    str_copy(v->path, path, sizeof(v->path));
    wm_set_title(w, path_basename(path));
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err) {
        ksnprintf(v->error, sizeof(v->error), "Cannot read the file (%s)", err_str(err));
        return;
    }
    v->file = (uint8_t *)data;
    v->size = size;
    v->offsets = kmalloc(MAX_FRAMES * 4);
    v->lengths = kmalloc(MAX_FRAMES * 4);
    if (!v->offsets || !v->lengths || size < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "AVI ", 4)) {
        str_copy(v->error, "This is not an AVI video", sizeof(v->error));
        unload(v);
        return;
    }
    struct sound snd = {0};
    int video_stream = -1, stream = -1;
    walk(v, &snd, v->file + 12, v->file + size, &video_stream, &stream);
    if (!v->frames) {
        str_copy(v->error, "The video has no pictures", sizeof(v->error));
        kfree(snd.pcm);
        unload(v);
        return;
    }
    if (!v->us_per_frame)
        v->us_per_frame = 40000;
    if (snd.count && snd.rate) {
        char title[64];
        str_copy(title, path_basename(path), sizeof(title));
        media_play_pcm(snd.pcm, snd.count, snd.rate, title, "Video");
        v->sound = true;
    } else {
        kfree(snd.pcm);
    }
    v->playing = true;
    v->start = hal_ticks_ms();
    v->shown = -1;
    v->last_move = hal_ticks_ms();
}

static void show_frame(struct video *v, int i)
{
    if (i == v->shown || i < 0 || i >= v->frames)
        return;
    struct picture p;
    if (picture_decode(v->file + v->offsets[i], v->lengths[i], &p) == 0) {
        picture_free(&v->pic);
        v->pic = p;
    }
    v->shown = i;
}

static struct rect bar_rect(int W, int H, int show)
{
    int w = MIN(W - 40, 520);
    return (struct rect){(W - w) / 2, H - 16 - BAR_H + (1024 - show) * (BAR_H + 20) / 1024, w, BAR_H};
}

static void paint(struct window *w, struct canvas *c)
{
    struct video *v = w->data;
    int W = c->w, H = c->h;
    gfx_clear(c, (struct rect){0, 0, W, H}, RGB(0, 0, 0));
    if (!v->file) {
        gfx_symbol(c, W / 2 - 16, H / 2 - 40, SYM_VIDEO, 32, ALPHA(0xffffffffu, 120));
        gfx_text_box(c, (struct rect){0, H / 2, W, 22}, &font_bold, v->error[0] ? v->error : "No video open",
                     ALPHA(0xffffffffu, 200), ALIGN_CENTER);
        gfx_text_box(c, (struct rect){0, H / 2 + 24, W, 18}, &font_small, "Open an AVI file (Motion JPEG) from the ⋯ menu",
                     ALPHA(0xffffffffu, 120), ALIGN_CENTER);
        return;
    }
    if (v->pic.px) {
        int pw = v->pic.w, ph = v->pic.h;
        int s = MIN(W * 1024 / MAX(pw, 1), H * 1024 / MAX(ph, 1));
        int dw = pw * s / 1024, dh = ph * s / 1024;
        struct canvas src = canvas_make(v->pic.px, pw, ph);
        gfx_scale_fast(c, (struct rect){(W - dw) / 2, (H - dh) / 2, dw, dh}, &src, 255);
    }
    int show = CLAMP(v->bar.pos, 0, 1024);
    if (show < 16)
        return;
    struct rect b = bar_rect(W, H, show);
    gfx_drop_shadow(c, b, b.h / 2, 16, 120, 4);
    gfx_round(c, b, b.h / 2, RGBA(24, 24, 30, 210));
    gfx_round_frame(c, b, b.h / 2, ALPHA(0xffffffffu, 40));
    int cx = b.x + 28, cy = b.y + b.h / 2;
    gfx_circle(c, cx, cy, 16, style.accent);
    gfx_symbol(c, cx - 8, cy - 8, v->playing ? SYM_PAUSE : SYM_PLAY, 16, style.accent_text);
    int len = MAX(length_ms(v), 1), pos = CLAMP(now_ms(v), 0, len);
    struct rect track = {b.x + 56, cy - 2, b.w - 56 - 100, 4};
    gfx_round(c, track, 2, ALPHA(0xffffffffu, 60));
    int fill = (int)((int64_t)track.w * pos / len);
    gfx_round(c, (struct rect){track.x, track.y, fill, 4}, 2, style.accent);
    gfx_circle(c, track.x + fill, cy, 5, RGB(255, 255, 255));
    char t[24];
    ksnprintf(t, sizeof(t), "%d:%02d / %d:%02d", pos / 60000, pos / 1000 % 60, len / 60000, len / 1000 % 60);
    gfx_text_box(c, (struct rect){track.x + track.w + 10, b.y, 90, b.h}, &font_small, t, ALPHA(0xffffffffu, 220),
                 ALIGN_LEFT);
}

static void picked(struct window *owner, const char *value)
{
    if (wm_window_alive(owner) && value && value[0]) {
        open_file(owner, owner->data, value);
        wm_invalidate(owner);
    }
}

static void command(struct window *w, struct video *v, int cmd)
{
    switch (cmd) {
    case CMD_OPEN: {
        char dir[PATH_MAX_LEN];
        path_join(user_home(), "Videos", dir, sizeof(dir));
        ui_prompt(w, "Open video", "Path to an AVI file:", dir, picked);
        break;
    }
    case CMD_PLAY: play(v, !v->playing); break;
    case CMD_RESTART:
        seek(v, 0);
        play(v, true);
        break;
    case CMD_LOOP: v->loop = !v->loop; break;
    case CMD_SHOW: {
        char dir[PATH_MAX_LEN];
        path_dirname(v->path, dir, sizeof(dir));
        gui_app_launch(gui_app_find("Files"), dir);
        break;
    }
    }
    v->last_move = hal_ticks_ms();
    wm_invalidate(w);
}

static bool on_track(struct window *w, struct video *v, int x, int y, int *ms)
{
    struct rect b = bar_rect(w->surface.w, w->surface.h, 1024);
    struct rect track = {b.x + 56, b.y, b.w - 56 - 100, b.h};
    if (!rect_contains(track, x, y))
        return false;
    *ms = (int)((int64_t)(x - track.x) * length_ms(v) / MAX(track.w, 1));
    return true;
}

static void event(struct window *w, struct gui_event *e)
{
    struct video *v = w->data;
    switch (e->type) {
    case EV_KEY:
        if (e->key >= KEY_COMMAND(0) && e->key <= KEY_COMMAND(CMD_SHOW))
            command(w, v, e->key - KEY_COMMAND(0));
        else if (e->key == ' ')
            command(w, v, CMD_PLAY);
        else if (e->key == KEY_LEFT || e->key == KEY_RIGHT)
            seek(v, now_ms(v) + (e->key == KEY_LEFT ? -5000 : 5000));
        else if (e->key == KEY_CTRL('o'))
            command(w, v, CMD_OPEN);
        break;
    case EV_MOUSE_MOVE:
        v->last_move = hal_ticks_ms();
        if (v->dragging) {
            int ms;
            if (on_track(w, v, e->x, e->y, &ms))
                seek(v, ms);
        }
        break;
    case EV_MOUSE_DOWN: {
        e->handled = true;
        v->last_move = hal_ticks_ms();
        if (!v->file)
            break;
        struct rect b = bar_rect(w->surface.w, w->surface.h, 1024);
        int ms;
        if (rect_contains((struct rect){b.x, b.y, 56, b.h}, e->x, e->y))
            command(w, v, CMD_PLAY);
        else if (on_track(w, v, e->x, e->y, &ms)) {
            seek(v, ms);
            v->dragging = true;
        } else if (e->button == BUTTON_LEFT && e->clicks >= 2)
            wm_toggle_maximize(w);
        else if (e->button == BUTTON_LEFT)
            command(w, v, CMD_PLAY);
        break;
    }
    case EV_MOUSE_UP: v->dragging = false; break;
    case EV_TIMER: {
        uint64_t now = hal_ticks_ms();
        int dt = (int)MIN(now - v->tick, 64u);
        v->tick = now;
        bool show = !v->playing || now - v->last_move < 2200;
        v->bar.target = show ? 1024 : 0;
        bool busy = spring_step(&v->bar, 320, 30, dt);
        if (v->file && v->frames) {
            int pos = now_ms(v);
            if (v->sound && v->playing && !media_track()->playing && media_is_file()) {
                v->playing = false;
                if (v->loop)
                    command(w, v, CMD_RESTART);
            }
            if (!v->sound && v->playing && pos >= length_ms(v)) {
                if (v->loop)
                    seek(v, 0);
                else
                    play(v, false), v->paused_at = v->start + (uint64_t)length_ms(v);
            }
            int idx = (int)((int64_t)CLAMP(pos, 0, length_ms(v)) * 1000 / v->us_per_frame);
            idx = MIN(idx, v->frames - 1);
            if (idx != v->shown) {
                show_frame(v, idx);
                busy = true;
            }
            if (v->bar.pos > 16 && now / 250 != (now - (uint64_t)dt) / 250)
                busy = true;
        }
        if (busy)
            wm_invalidate(w);
        break;
    }
    default: break;
    }
}

static void destroy(struct window *w)
{
    struct video *v = w->data;
    unload(v);
    kfree(v);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    struct video *v = kzalloc(sizeof(*v));
    if (!v)
        return;
    v->tick = hal_ticks_ms();
    spring_set(&v->bar, 1024);
    struct window *w = wm_open("Video", ICON_VIDEO, 640, 380, &ops, v, WIN_RESIZABLE);
    if (!w) {
        kfree(v);
        return;
    }
    w->min_w = 320;
    w->min_h = 200;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    wm_set_timer(w, 15);
    if (arg && *arg)
        open_file(w, v, arg);
}

static const struct gui_app video_app = {"Video", "Watch AVI videos (Motion JPEG with PCM sound)", ICON_VIDEO, launch,
                                         ".avi", NULL};
GUI_APP(video_app);
