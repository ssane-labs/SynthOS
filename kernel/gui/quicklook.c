#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

/* Quick Look: Space in Files shows the selected file without opening an app - a picture fitted to the screen, the
   start of a text file, or the icon, kind and size of anything else. Space or Esc closes it, Enter opens the file. */

#define QL_HEAD 44
#define QL_TEXT_MAX 6000

struct quicklook {
    struct ui_state st;
    char path[PATH_MAX_LEN];
    struct canvas pic;
    char *text;
    size_t size;
    int pic_w, pic_h;
    bool dir;
};

static struct window *ql_win;

static bool is_text(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0 || (c < 8) || (c > 13 && c < 32 && c != 27))
            return false;
    }
    return true;
}

static void ql_frame(struct ui *ui, void *data)
{
    struct quicklook *q = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, 0);
    char info[64], sz[16];
    fmt_size(sz, sizeof(sz), q->size);
    ksnprintf(info, sizeof(info), q->dir ? "Folder" : q->pic.px ? "%s · %d × %d" : "%s", sz, q->pic_w, q->pic_h);
    if (ui->c) {
        gfx_text_box(ui->c, (struct rect){20, 8, W - 160, 20}, &font_bold, path_basename(q->path), style.text,
                     ALIGN_LEFT);
        gfx_text_box(ui->c, (struct rect){20, 26, W - 160, 14}, &font_small, info, style.secondary, ALIGN_LEFT);
    }
    if (ui_button(ui, (struct rect){W - 132, 10, 112, 26}, "Open")) {
        char path[PATH_MAX_LEN];
        str_copy(path, q->path, sizeof(path));
        ui->win = NULL;
        wm_destroy(ql_win);
        gui_open_file(path);
        return;
    }
    struct rect body = {12, QL_HEAD + 4, W - 24, H - QL_HEAD - 16};
    if (!ui->c)
        return;
    if (q->pic.px) {
        gfx_blit_round(ui->c, (struct rect){body.x + (body.w - q->pic.w) / 2, body.y + (body.h - q->pic.h) / 2,
                                            q->pic.w, q->pic.h},
                       &q->pic, 12, true);
    } else if (q->text) {
        gfx_round(ui->c, body, 12, style.dark ? RGB(28, 28, 30) : RGB(255, 255, 255));
        struct canvas clip = *ui->c;
        clip.clip = rect_intersect(ui->c->clip, rect_inset(body, 8));
        int y = body.y + 12;
        for (const char *p = q->text; *p && y < body.y + body.h - 8; y += font_mono.height + 2) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            gfx_text_n(&clip, body.x + 14, y, &font_mono, p, MIN(n, (size_t)200), style.text);
            p += n + (nl ? 1 : 0);
        }
    } else {
        int s = 128;
        gui_draw_file_icon(ui->c, q->path, (struct rect){body.x + (body.w - s) / 2, body.y + body.h / 2 - s / 2 - 20, s, s},
                           255);
        const char *ext = path_ext(q->path);
        char kind[48];
        ksnprintf(kind, sizeof(kind), q->dir ? "Folder" : ext ? "%s file" : "File", ext ? ext + 1 : "");
        gfx_text_box(ui->c, (struct rect){body.x, body.y + body.h / 2 + s / 2 - 8, body.w, 22}, &font_title, kind,
                     style.text, ALIGN_CENTER);
    }
}

static void ql_paint(struct window *w, struct canvas *c)
{
    struct quicklook *q = w->data;
    ui_run(w, &q->st, c, NULL, ql_frame, q);
}

static void ql_event(struct window *w, struct gui_event *e)
{
    struct quicklook *q = w->data;
    if (e->type == EV_KEY) {
        int k = KEY_BASE(e->key);
        if (k == KEY_ESC || k == ' ') {
            wm_destroy(w);
        } else if (k == KEY_ENTER) {
            char path[PATH_MAX_LEN];
            str_copy(path, q->path, sizeof(path));
            wm_destroy(w);
            gui_open_file(path);
        }
        return;
    }
    ui_run(w, &q->st, NULL, e, ql_frame, q);
    if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
}

static void ql_destroy(struct window *w)
{
    struct quicklook *q = w->data;
    canvas_free(&q->pic);
    kfree(q->text);
    kfree(q);
    if (ql_win == w)
        ql_win = NULL;
}

static const struct window_ops ql_ops = {ql_paint, ql_event, NULL, ql_destroy};

bool quicklook_open(void)
{
    return ql_win && wm_window_alive(ql_win);
}

void quicklook_toggle(const char *path)
{
    if (quicklook_open()) {
        wm_destroy(ql_win);
        return;
    }
    struct quicklook *q = kzalloc(sizeof(*q));
    if (!q)
        return;
    str_copy(q->path, path, sizeof(q->path));
    struct vstat st;
    if (vfs_stat(path, &st) == 0) {
        q->size = st.size;
        q->dir = st.type == VN_DIR;
    }
    struct rect s = wm_screen_rect(0);
    int maxw = s.w * 3 / 4, maxh = s.h * 3 / 4 - QL_HEAD;
    int W = 520, H = 420;
    struct picture p;
    if (!q->dir && picture_is_image(path) && picture_load(path, &p) == 0) {
        int scale = MIN(1024, MIN(maxw * 1024 / MAX(p.w, 1), maxh * 1024 / MAX(p.h, 1)));
        int w = MAX(p.w * scale / 1024, 1), h = MAX(p.h * scale / 1024, 1);
        struct canvas src = {p.px, p.w, p.h, p.w, {0, 0, p.w, p.h}};
        q->pic_w = p.w;
        q->pic_h = p.h;
        if (canvas_alloc(&q->pic, w, h)) {
            gfx_scale_smooth(&q->pic, (struct rect){0, 0, w, h}, &src, 255);
            q->pic.w = w;
            q->pic.h = h;
            W = MAX(w + 24, 360);
            H = h + QL_HEAD + 20;
        }
        picture_free(&p);
    } else if (!q->dir) {
        char *data;
        size_t n;
        if (vfs_read_all(path, &data, &n) == 0) {
            size_t k = MIN(n, (size_t)QL_TEXT_MAX);
            if (k && is_text(data, k)) {
                q->text = kmalloc(k + 1);
                if (q->text) {
                    memcpy(q->text, data, k);
                    q->text[k] = 0;
                    W = MIN(760, maxw);
                    H = MIN(560, maxh + QL_HEAD);
                }
            }
            kfree(data);
        }
    }
    ql_win = wm_popup(s.x + (s.w - W) / 2, s.y + (s.h - H) / 2, W, H, &ql_ops, q, NULL);
    if (!ql_win) {
        canvas_free(&q->pic);
        kfree(q->text);
        kfree(q);
        return;
    }
    ql_win->radius = 22;
}
