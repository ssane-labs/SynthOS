#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/path.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "../internal.h"

/* Viewer: a quick read-only look at any file, as text or as a hex dump. */

#define LINE_H 18
#define PAD 16
#define MAX_BYTES (4u << 20)

enum {
    CMD_TEXT,
    CMD_HEX,
    CMD_EDIT,
    CMD_SHOW,
    CMD_COPY_PATH,
};

static const struct menu_item file_items[] = {
    {"Show as Text", KEY_COMMAND(CMD_TEXT), NULL},    {"Show as Hex", KEY_COMMAND(CMD_HEX), NULL},
    {"", 0, NULL},                                    {"Open in Notepad", KEY_COMMAND(CMD_EDIT), NULL},
    {"Show in Files", KEY_COMMAND(CMD_SHOW), NULL},   {"Copy Path", KEY_COMMAND(CMD_COPY_PATH), NULL},
};

static const struct app_menu menus[] = {{"File", file_items, ARRAY_SIZE(file_items)}};

struct viewer {
    char path[PATH_MAX_LEN];
    char *data;
    size_t size, real_size;
    bool hex;
    size_t *lines;
    int line_count;
    int scroll;
    struct spring smooth;
    uint64_t tick;
};

static bool looks_binary(const char *d, size_t n)
{
    n = MIN(n, (size_t)4096);
    for (size_t i = 0; i < n; i++)
        if (!d[i])
            return true;
    return false;
}

bool viewer_is_binary(const char *path)
{
    struct file *f;
    if (vfs_open(path, VFS_READ, &f) != 0)
        return false;
    char buf[512];
    long n = vfs_read(f, buf, sizeof(buf));
    vfs_close(f);
    return n > 0 && looks_binary(buf, (size_t)n);
}

static void index_lines(struct viewer *v)
{
    kfree(v->lines);
    v->line_count = 0;
    if (v->hex) {
        v->line_count = (int)((v->size + 15) / 16);
        v->lines = NULL;
        return;
    }
    int cap = 1024;
    v->lines = kmalloc(sizeof(size_t) * (size_t)cap);
    if (!v->lines)
        return;
    v->lines[v->line_count++] = 0;
    for (size_t i = 0; i < v->size; i++)
        if (v->data[i] == '\n') {
            if (v->line_count == cap) {
                size_t *grown = krealloc(v->lines, sizeof(size_t) * (size_t)cap * 2);
                if (!grown)
                    return;
                v->lines = grown;
                cap *= 2;
            }
            v->lines[v->line_count++] = i + 1;
        }
}

static int visible_lines(struct window *w)
{
    return MAX(1, (w->surface.h - 2 * PAD) / LINE_H);
}

static void scroll_to(struct window *w, struct viewer *v, int line)
{
    v->scroll = CLAMP(line, 0, MAX(0, v->line_count - visible_lines(w)));
    v->smooth.target = v->scroll * LINE_H;
    wm_invalidate(w);
}

static void paint(struct window *w, struct canvas *c)
{
    struct viewer *v = w->data;
    int W = c->w, H = c->h;
    gfx_clear(c, (struct rect){0, 0, W, H}, style.content);
    if (!v->data) {
        gfx_text_box(c, (struct rect){0, H / 2 - 10, W, 20}, &font_sans, "This file cannot be read", style.secondary,
                     ALIGN_CENTER);
        return;
    }
    int y0 = PAD - v->smooth.pos % LINE_H, first = v->smooth.pos / LINE_H;
    for (int i = first; i < v->line_count && y0 + (i - first) * LINE_H < H; i++) {
        int y = y0 + (i - first) * LINE_H;
        char buf[256];
        if (v->hex) {
            size_t off = (size_t)i * 16;
            int n = ksnprintf(buf, sizeof(buf), "%08x  ", (unsigned)off);
            for (int k = 0; k < 16; k++)
                n += off + (size_t)k < v->size ? ksnprintf(buf + n, sizeof(buf) - (size_t)n, "%02x%s", (uint8_t)v->data[off + (size_t)k], k == 7 ? "  " : " ")
                                               : ksnprintf(buf + n, sizeof(buf) - (size_t)n, "   %s", k == 7 ? " " : "");
            n += ksnprintf(buf + n, sizeof(buf) - (size_t)n, " ");
            for (int k = 0; k < 16 && off + (size_t)k < v->size; k++) {
                uint8_t ch = (uint8_t)v->data[off + (size_t)k];
                buf[n++] = ch >= 32 && ch < 127 ? (char)ch : '.';
            }
            buf[n] = 0;
            gfx_text_n(c, PAD, y, &font_mono, buf, 10, style.tertiary);
            gfx_text(c, PAD + gfx_text_width_n(&font_mono, buf, 10), y, &font_mono, buf + 10, style.text);
        } else {
            size_t start = v->lines[i], end = i + 1 < v->line_count ? v->lines[i + 1] - 1 : v->size;
            size_t n = MIN(end - start, sizeof(buf) - 1);
            memcpy(buf, v->data + start, n);
            buf[n] = 0;
            for (size_t k = 0; k < n; k++)
                if (buf[k] == '\t' || buf[k] == '\r')
                    buf[k] = ' ';
            gfx_text(c, PAD, y, &font_mono, buf, style.text);
        }
    }
    if (v->real_size > v->size)
        gfx_text_box(c, (struct rect){0, H - 22, W - PAD, 18}, &font_small, "Showing the first 4 MB", style.tertiary,
                     ALIGN_RIGHT);
}

static void event(struct window *w, struct gui_event *e)
{
    struct viewer *v = w->data;
    switch (e->type) {
    case EV_KEY: {
        int k = e->key, page = visible_lines(w) - 1;
        if (k == KEY_COMMAND(CMD_TEXT) || k == KEY_COMMAND(CMD_HEX)) {
            v->hex = k == KEY_COMMAND(CMD_HEX);
            index_lines(v);
            spring_set(&v->smooth, 0);
            scroll_to(w, v, 0);
        } else if (k == KEY_COMMAND(CMD_EDIT)) {
            gui_app_launch(gui_app_find("Notepad"), v->path);
        } else if (k == KEY_COMMAND(CMD_SHOW)) {
            char dir[PATH_MAX_LEN];
            path_dirname(v->path, dir, sizeof(dir));
            gui_app_launch(gui_app_find("Files"), dir);
        } else if (k == KEY_COMMAND(CMD_COPY_PATH)) {
            gui_set_clipboard(v->path, strlen(v->path));
        } else if (k == KEY_DOWN) {
            scroll_to(w, v, v->scroll + 1);
        } else if (k == KEY_UP) {
            scroll_to(w, v, v->scroll - 1);
        } else if (k == KEY_PGDN || k == ' ') {
            scroll_to(w, v, v->scroll + page);
        } else if (k == KEY_PGUP) {
            scroll_to(w, v, v->scroll - page);
        } else if (k == KEY_HOME) {
            scroll_to(w, v, 0);
        } else if (k == KEY_END) {
            scroll_to(w, v, v->line_count);
        }
        break;
    }
    case EV_WHEEL: scroll_to(w, v, v->scroll + e->wheel * 3); break;
    case EV_MOUSE_DOWN:
        e->handled = true;
        if (e->button == BUTTON_RIGHT) {
            struct rect cr = wm_client_rect(w);
            wm_window_menu(w, cr.x + e->x, cr.y + e->y);
        }
        break;
    case EV_TIMER: {
        uint64_t now = hal_ticks_ms();
        if (spring_step(&v->smooth, 500, 44, (int)MIN(now - v->tick, 64u)))
            wm_invalidate(w);
        v->tick = now;
        break;
    }
    case EV_RESIZE: scroll_to(w, v, v->scroll); break;
    default: break;
    }
}

static void destroy(struct window *w)
{
    struct viewer *v = w->data;
    kfree(v->data);
    kfree(v->lines);
    kfree(v);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (!arg || !*arg)
        return;
    struct viewer *v = kzalloc(sizeof(*v));
    if (!v)
        return;
    str_copy(v->path, arg, sizeof(v->path));
    struct file *f;
    if (vfs_open(arg, VFS_READ, &f) == 0) {
        struct vstat st;
        v->real_size = vfs_stat(arg, &st) == 0 ? (size_t)st.size : 0;
        size_t want = MIN(v->real_size ? v->real_size : MAX_BYTES, (size_t)MAX_BYTES);
        v->data = kmalloc(want + 1);
        long n = v->data ? vfs_read(f, v->data, want) : -1;
        vfs_close(f);
        if (n < 0) {
            kfree(v->data);
            v->data = NULL;
        } else {
            v->size = (size_t)n;
        }
    }
    v->hex = v->data && looks_binary(v->data, v->size);
    index_lines(v);
    v->tick = hal_ticks_ms();
    struct window *w = wm_open(path_basename(arg), ICON_VIEWER, v->hex ? 640 : 620, 440, &ops, v, WIN_RESIZABLE);
    if (!w) {
        kfree(v->data);
        kfree(v->lines);
        kfree(v);
        return;
    }
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    wm_set_timer(w, 16);
}

static const struct gui_app viewer_app = {"Viewer", "Look inside any file as text or hex", ICON_VIEWER, launch, NULL, NULL};
GUI_APP(viewer_app);
