#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "internal.h"

/* Wallpapers from the internet: Bing's photo of the day, a random photo from picsum.photos or any picture URL,
   checked by its signature and kept in ~/Pictures/Wallpapers. */

#define BING_JSON "https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=en-US"
#define PICSUM_URL "https://picsum.photos/1920/1200"
#define MAX_PICTURE (24u << 20)

struct fetch_ui {
    void (*progress)(void *ctx, const char *stage, size_t done, size_t total);
    void *ctx;
};

static bool json_value(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[32];
    ksnprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p)
        return false;
    p += strlen(pattern);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (p[0] == '\\' && p[1] == 'u' && p[2] && p[3] && p[4] && p[5]) {
            char hex[5] = {p[2], p[3], p[4], p[5], 0};
            long c = str_to_long(hex, NULL, 16);
            out[n++] = c < 128 ? (char)c : '?';
            p += 6;
        } else if (p[0] == '\\' && p[1]) {
            out[n++] = p[1];
            p += 2;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return *p == '"';
}

static int fetch(const char *url, size_t max, const struct fetch_ui *ui, struct http_response *resp)
{
    struct http_request req = {.url = url, .max_size = max, .flags = HTTP_NO_COOKIES,
                               .progress = ui ? ui->progress : NULL, .ctx = ui ? ui->ctx : NULL};
    int err = http_fetch(&req, resp);
    if (err)
        return err;
    if (resp->status != 200 || !resp->body) {
        http_free(resp);
        return E_NOENT;
    }
    return 0;
}

static const char *picture_ext(const uint8_t *d, size_t n)
{
    if (n > 3 && d[0] == 0xff && d[1] == 0xd8)
        return ".jpg";
    if (n > 8 && !memcmp(d, "\x89PNG", 4))
        return ".png";
    if (n > 6 && !memcmp(d, "GIF8", 4))
        return ".gif";
    if (n > 2 && d[0] == 'B' && d[1] == 'M')
        return ".bmp";
    return NULL;
}

static void clean_name(const char *from, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = from; *p && n + 1 < cap; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                  c == '_';
        if (ok && !(c == ' ' && (!n || out[n - 1] == ' ')))
            out[n++] = c;
        else if (n && out[n - 1] != ' ')
            out[n++] = ' ';
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
    if (!n)
        str_copy(out, "Wallpaper", cap);
}

int wallpaper_download(const char *source, char *saved, size_t cap, char *title, size_t title_cap,
                       void (*progress)(void *ctx, const char *stage, size_t done, size_t total), void *ctx)
{
    struct fetch_ui ui = {progress, ctx};
    char url[HTTP_URL_MAX], name[96];
    struct http_response resp;
    int err;
    if (!strcmp(source, "bing")) {
        if ((err = fetch(BING_JSON, 1u << 18, NULL, &resp)) != 0)
            return err;
        char rel[512] = "", caption[96] = "";
        bool ok = json_value(resp.body, "url", rel, sizeof(rel)) && rel[0] == '/';
        if (!json_value(resp.body, "title", caption, sizeof(caption)))
            str_copy(caption, "Photo of the Day", sizeof(caption));
        http_free(&resp);
        if (!ok)
            return E_INVAL;
        ksnprintf(url, sizeof(url), "https://www.bing.com%s", rel);
        clean_name(caption, name, sizeof(name));
    } else if (!strcmp(source, "picsum") || !strcmp(source, "random")) {
        str_copy(url, PICSUM_URL, sizeof(url));
        ksnprintf(name, sizeof(name), "Photo %u", (unsigned)(hal_ticks_ms() % 100000));
    } else if (str_starts(source, "http://") || str_starts(source, "https://")) {
        str_copy(url, source, sizeof(url));
        char base[96];
        str_copy(base, path_basename(source), sizeof(base));
        char *q = strchr(base, '?');
        if (q)
            *q = 0;
        char *dot = strrchr(base, '.');
        if (dot)
            *dot = 0;
        clean_name(base, name, sizeof(name));
    } else {
        return E_INVAL;
    }
    if ((err = fetch(url, MAX_PICTURE, &ui, &resp)) != 0)
        return err;
    const char *ext = picture_ext((const uint8_t *)resp.body, resp.length);
    if (!ext) {
        http_free(&resp);
        return E_NOEXEC;
    }
    char dir[PATH_MAX_LEN], file[128];
    path_join(user_home(), "Pictures/Wallpapers", dir, sizeof(dir));
    vfs_mkdirs(dir);
    ksnprintf(file, sizeof(file), "%s%s", name, ext);
    path_join(dir, file, saved, cap);
    err = vfs_write_all(saved, resp.body, resp.length);
    http_free(&resp);
    if (title)
        str_copy(title, name, title_cap);
    return err;
}

static const char *fetch_error(int err)
{
    return err == E_NOEXEC ? "that is not a picture" : err == E_INVAL ? "the address is not a picture link"
                                                                       : tls_strerror(err);
}

/* The desktop runs a download from its loop (not inside an app's event) and keeps a small progress card drawn. */

static char pending[HTTP_URL_MAX];
static struct window *card;
static int card_permille = -1;
static uint64_t card_drawn;

static void card_paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    gfx_clear(c, (struct rect){0, 0, c->w, c->h}, 0);
    gfx_symbol(c, 18, 18, SYM_GLOBE, 22, style.accent);
    gfx_text_box(c, (struct rect){52, 12, c->w - 64, 18}, &font_bold, "Getting a wallpaper", style.text, ALIGN_LEFT);
    gfx_text_box(c, (struct rect){52, 30, c->w - 64, 16}, &font_small,
                 card_permille >= 0 ? "Downloading the picture" : "Asking the server", style.secondary, ALIGN_LEFT);
    struct rect bar = {52, c->h - 18, c->w - 70, 5};
    gfx_round(c, bar, 2, style.separator);
    int fill = card_permille >= 0 ? bar.w * card_permille / 1000 : bar.w / 6 + (int)(hal_ticks_ms() / 4 % (uint64_t)(bar.w / 2));
    gfx_round(c, (struct rect){bar.x, bar.y, MIN(fill, bar.w), bar.h}, 2, style.accent);
}

static void card_event(struct window *w, struct gui_event *e)
{
    UNUSED(w);
    if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
}

static void card_destroy(struct window *w)
{
    if (card == w)
        card = NULL;
}

static const struct window_ops card_ops = {card_paint, card_event, NULL, card_destroy};

static void card_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    UNUSED(ctx);
    UNUSED(stage);
    card_permille = total ? (int)((uint64_t)done * 1000 / total) : -1;
    uint64_t now = hal_ticks_ms();
    if (!card || now - card_drawn < 100)
        return;
    card_drawn = now;
    wm_invalidate(card);
    wm_animate_ghosts();
    wm_render();
}

void wallpaper_fetch(const char *source)
{
    str_copy(pending, source, sizeof(pending));
}

void wallpaper_fetch_run(void)
{
    if (!pending[0])
        return;
    char source[HTTP_URL_MAX], saved[PATH_MAX_LEN], title[96];
    str_copy(source, pending, sizeof(source));
    pending[0] = 0;
    struct rect s = wm_screen_rect(0);
    card_permille = -1;
    card = wm_popup(s.x + s.w - 300, status_bottom() + 10, 288, 64, &card_ops, NULL, NULL);
    if (card) {
        card->radius = 20;
        card->flags |= WIN_PASSIVE;
    }
    card_drawn = 0;
    card_progress(NULL, "", 0, 0);
    int err = wallpaper_download(source, saved, sizeof(saved), title, sizeof(title), card_progress, NULL);
    if (card && wm_window_alive(card))
        wm_destroy(card);
    card = NULL;
    if (err) {
        gui_notify("No wallpaper", fetch_error(err));
        return;
    }
    gui_wallpaper_use(saved);
    gui_notify("New wallpaper", title);
}

COMMAND(wallpaper, NULL, "system", "wallpaper [bing|random|<picture url>]",
        "download a wallpaper (photo of the day, a random photo or a link) and use it")
{
    if (argc != 2)
        return command_usage(sh, command_find("wallpaper"));
    char saved[PATH_MAX_LEN], title[96];
    sh_printf(sh, "Downloading %s\n", argv[1]);
    int err = wallpaper_download(argv[1], saved, sizeof(saved), title, sizeof(title), NULL, NULL);
    if (err)
        return sh_error(sh, "wallpaper", "%s", fetch_error(err));
    gui_wallpaper_use(saved);
    sh_printf(sh, "Saved %s and set it as the wallpaper\n", saved);
    return 0;
}
