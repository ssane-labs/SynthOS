#include <synth/command.h>
#include <synth/error.h>
#include <synth/gui.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/picture.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

/* The clipboard of the whole desktop: text or a picture (what Windows calls CF_UNICODETEXT and CF_DIB). Putting
   one in takes the other out, as on Windows; the sequence number tells readers it changed. */

#define TEXT_MAX (1u << 20)
#define HISTORY 25
#define HISTORY_TEXT_MAX (64u << 10)

static char empty[1];
static char *text;
static uint32_t *image;
static int image_w, image_h;
static uint32_t seq = 1;
static char *history[HISTORY]; /* the texts copied last, newest first (Win+V) */
static int history_count;

static void remember(const char *s, size_t n)
{
    if (!n || n > HISTORY_TEXT_MAX)
        return;
    int at = -1;
    for (int i = 0; i < history_count && at < 0; i++)
        if (strlen(history[i]) == n && !memcmp(history[i], s, n))
            at = i;
    char *entry;
    if (at >= 0) {
        entry = history[at];
    } else {
        if (!(entry = kmalloc(n + 1)))
            return;
        memcpy(entry, s, n);
        entry[n] = 0;
        if (history_count == HISTORY)
            kfree(history[--history_count]);
        at = history_count++;
    }
    memmove(&history[1], &history[0], (size_t)at * sizeof(history[0]));
    history[0] = entry;
}

/* the i-th text of the history (0: the newest), NULL past the end */
const char *gui_clipboard_history(int i)
{
    return i >= 0 && i < history_count ? history[i] : NULL;
}

void gui_clipboard_forget(void)
{
    for (int i = 0; i < history_count; i++)
        kfree(history[i]);
    history_count = 0;
}

static void clear(void)
{
    kfree(text);
    kfree(image);
    text = NULL;
    image = NULL;
    image_w = image_h = 0;
    seq++;
}

const char *gui_clipboard(void)
{
    return text ? text : empty;
}

void gui_set_clipboard(const char *s, size_t n)
{
    clear();
    n = MIN(n, (size_t)TEXT_MAX);
    text = kmalloc(n + 1);
    if (!text)
        return;
    memcpy(text, s, n);
    text[n] = 0;
    remember(text, n);
}

void gui_clipboard_set_image(const uint32_t *px, int w, int h, int stride)
{
    clear();
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > (32u << 20))
        return;
    image = kmalloc((size_t)w * (size_t)h * 4);
    if (!image)
        return;
    for (int y = 0; y < h; y++)
        memcpy(image + (size_t)y * (size_t)w, px + (size_t)y * (size_t)stride, (size_t)w * 4);
    image_w = w;
    image_h = h;
}

/* The size of the picture on the clipboard (false when it holds none); copying it out leaves it there. No
   pointer goes out, so the 32-bit Windows code can call these as they are. */
bool gui_clipboard_image(int *w, int *h)
{
    if (!image)
        return false;
    *w = image_w;
    *h = image_h;
    return true;
}

void gui_clipboard_copy_image(uint32_t *dst, int stride)
{
    for (int y = 0; image && y < image_h; y++)
        memcpy(dst + (size_t)y * (size_t)stride, image + (size_t)y * (size_t)image_w, (size_t)image_w * 4);
}

uint32_t gui_clipboard_seq(void)
{
    return seq;
}

/* Win+V: the history as a menu; the text chosen goes back on the clipboard and is pasted where the focus is */
static char history_labels[HISTORY][60];

static void history_pick(struct window *owner, int index)
{
    UNUSED(owner);
    if (index == history_count + 2) {
        gui_clipboard_forget();
        return;
    }
    char *copy = index >= 1 && index <= history_count ? kstrdup(history[index - 1]) : NULL;
    if (!copy)
        return;
    gui_set_clipboard(copy, strlen(copy));
    kfree(copy);
    wm_key(KEY_CTRL('v'));
}

void gui_clipboard_history_show(void)
{
    static struct menu_item items[HISTORY + 3];
    int n = 0;
    items[n++] = (struct menu_item){"Clipboard", MENU_HEADER, NULL};
    for (int i = 0; i < history_count; i++) {
        char *l = history_labels[i];
        const char *t = history[i];
        while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t')
            t++;
        str_copy(l, t, 52);
        for (char *c = l; *c; c++)
            if (*c == '\n' || *c == '\r' || *c == '\t')
                *c = ' ';
        if (strlen(t) > 51)
            str_append(l, "...", sizeof(history_labels[i]));
        items[n++] = (struct menu_item){l, 0, NULL};
    }
    if (!history_count)
        items[n++] = (struct menu_item){"Text you copy shows here", MENU_HEADER, NULL};
    else {
        items[n++] = (struct menu_item){"", 0, NULL};
        items[n++] = (struct menu_item){"Clear All", 0, NULL};
    }
    struct rect s = wm_screen_rect(0);
    ui_menu_items(NULL, s.x + s.w / 2 - 170, s.y + s.h / 4, items, n, history_pick);
}

COMMAND(clip, "pbcopy", "system", "clip [info|history|pick <n>|forget|set <text>|image <picture>|save <file.png>]",
        "show or change the clipboard (text or a picture)")
{
    const char *op = argc > 1 ? argv[1] : "show";
    if (!strcmp(op, "show")) {
        sh_puts(sh, gui_clipboard());
        if (text && *text)
            sh_puts(sh, "\n");
        return 0;
    }
    if (!strcmp(op, "info")) {
        if (image)
            sh_printf(sh, "image %dx%d\n", image_w, image_h);
        else if (text && *text)
            sh_printf(sh, "text %u bytes\n", (unsigned)strlen(text));
        else
            sh_puts(sh, "empty\n");
        return 0;
    }
    if (!strcmp(op, "history")) {
        for (int i = 0; i < history_count; i++) {
            char line[72];
            str_copy(line, history[i], sizeof(line));
            for (char *c = line; *c; c++)
                if (*c == '\n' || *c == '\r' || *c == '\t')
                    *c = ' ';
            sh_printf(sh, "%2d  %s\n", i + 1, line);
        }
        if (!history_count)
            sh_puts(sh, "nothing copied yet\n");
        return 0;
    }
    if (!strcmp(op, "forget")) {
        gui_clipboard_forget();
        return 0;
    }
    if (argc < 3)
        return command_usage(sh, command_find("clip"));
    if (!strcmp(op, "pick")) {
        const char *t = gui_clipboard_history((int)str_to_long(argv[2], NULL, 10) - 1);
        if (!t)
            return sh_error(sh, "clip", "no text %s in the history", argv[2]);
        char *copy = kstrdup(t);
        if (copy)
            gui_set_clipboard(copy, strlen(copy));
        kfree(copy);
        return 0;
    }
    if (!strcmp(op, "set")) {
        char buf[1024] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                str_append(buf, " ", sizeof(buf));
            str_append(buf, argv[i], sizeof(buf));
        }
        gui_set_clipboard(buf, strlen(buf));
        return 0;
    }
    char path[PATH_MAX_LEN];
    if (sh_resolve(sh, argv[2], path, sizeof(path)) != 0)
        return sh_error(sh, "clip", "bad path: %s", argv[2]);
    if (!strcmp(op, "image")) {
        struct picture p;
        int err = picture_load(path, &p);
        if (err)
            return sh_error(sh, "clip", "%s: %s", argv[2], err == E_INVAL ? "not a picture" : err_str(err));
        gui_clipboard_set_image(p.px, p.w, p.h, p.w);
        picture_free(&p);
        return 0;
    }
    if (!strcmp(op, "save")) {
        if (!image)
            return sh_error(sh, "clip", "the clipboard holds no picture");
        uint8_t *png;
        size_t len;
        int err = png_encode(image, image_w, image_h, image_w, &png, &len);
        if (!err) {
            err = vfs_write_all(path, png, len);
            kfree(png);
        }
        return err ? sh_error(sh, "clip", "%s: %s", argv[2], err_str(err)) : 0;
    }
    return command_usage(sh, command_find("clip"));
}
