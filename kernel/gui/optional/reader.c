#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/keys.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/zip.h>

#include "../internal.h"

/* Reader: PDF, Word, Excel, PowerPoint and OpenDocument files as pages (the readers in doc/ fill one model), with
   zoom, find, copying the text, and the documents of the home folder when nothing is open. doctext prints the
   text of a document in the terminal. */

#include "doc/model.h"

static int doc_text_width(int style, int size, const char *s, int n)
{
    const struct ui_font *f = style == ST_BOLD ? &font_bold : style == ST_MONO ? &font_mono : &font_sans;
    return (int)((int64_t)gfx_text_width_n(f, s, (size_t)n) * size / 13);
}

#include "doc/office.h"
#include "doc/pdf.h"

#define EXTS ".pdf;.docx;.xlsx;.pptx;.odt;.ods;.odp"
#define MAX_RECENT 24
#define GAP 18

struct reader {
    struct ui_state st;
    struct doc doc;
    char path[PATH_MAX_LEN], title[NAME_MAX_LEN];
    int scroll, zoom; /* zoom: percent of the fitting width */
    struct ui_text find;
    char last_find[64];
    int match, matches;
    char recent[MAX_RECENT][PATH_MAX_LEN];
    int recent_count;
};

static int doc_load(struct doc *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    const uint8_t *p = (const uint8_t *)data;
    if (size > 4 && !memcmp(p, "%PDF", 4))
        err = pdf_read(d, p, size);
    else if (size > 4 && p[0] == 'P' && p[1] == 'K')
        err = office_read(d, p, size);
    else
        err = E_INVAL;
    kfree(data);
    if (err)
        doc_free(d);
    return err;
}

/* ---- drawing ---- */

static const struct ui_font *font_at(int style, int px)
{
    if (style == ST_MONO && px < 18)
        return &font_mono;
    if (px < 12)
        return &font_small;
    if (px < 17)
        return style == ST_BOLD ? &font_bold : &font_sans;
    if (px < 26)
        return &font_title;
    return &font_large;
}

static const struct ui_font *smaller(const struct ui_font *f)
{
    return f == &font_large ? &font_title : f == &font_title ? &font_sans : f == &font_sans || f == &font_bold ? &font_small : NULL;
}

/* z: pixels per point in 1/256 */
static int scale(int32_t v, int z)
{
    return (int)((int64_t)v * z / (PT * 256));
}

static bool item_matches(const struct doc *d, const struct item *it, const char *q)
{
    size_t n = strlen(q);
    if (!n || it->kind != IT_TEXT || it->len < n)
        return false;
    for (uint32_t i = 0; i + n <= it->len; i++)
        if (!str_ncasecmp(d->text + it->text + i, q, n))
            return true;
    return false;
}

static void draw_page(struct canvas *c, const struct doc *d, const struct doc_page *pg, int ox, int oy, int z,
                      const char *find)
{
    struct rect page = {ox, oy, scale(pg->w, z), scale(pg->h, z)};
    gfx_drop_shadow(c, page, 2, 12, 60, 3);
    gfx_fill(c, page, 0xffffffff);
    struct canvas clip = *c;
    clip.clip = rect_intersect(c->clip, page);
    if (clip.clip.w <= 0 || clip.clip.h <= 0)
        return;
    for (int i = pg->first; i < pg->first + pg->count; i++) {
        const struct item *it = &d->items[i];
        int x = ox + scale(it->x, z), y = oy + scale(it->y, z);
        switch (it->kind) {
        case IT_RECT:
            gfx_fill(&clip, (struct rect){x, y, MAX(1, scale(it->w, z)), MAX(1, scale(it->h, z))}, it->color);
            break;
        case IT_FRAME:
            gfx_frame(&clip, (struct rect){x, y, MAX(1, scale(it->w, z)), MAX(1, scale(it->h, z))}, it->color);
            break;
        case IT_LINE:
            gfx_line(&clip, x, y, x + scale(it->w, z), y + scale(it->h, z), it->color);
            break;
        case IT_IMAGE: {
            const struct picture *pic = &d->pics[it->text];
            struct canvas src = {pic->px, pic->w, pic->h, pic->w, {0, 0, pic->w, pic->h}};
            struct rect dst = {x, y, MAX(1, scale(it->w, z)), MAX(1, scale(it->h, z))};
            if (rect_intersect(dst, clip.clip).w > 0)
                gfx_scale(&clip, dst, &src, 255);
            break;
        }
        case IT_TEXT: {
            int px = scale(it->size, z);
            if (px < 4) {
                gfx_fill(&clip, (struct rect){x, y - MAX(1, px / 2), MAX(1, scale(it->w, z)), 1}, 0xffb0b4bc);
                break;
            }
            const struct ui_font *f = font_at(it->style, px);
            int room = scale(it->w, z);
            const char *s = d->text + it->text;
            while (room > 0 && f && smaller(f) && gfx_text_width_n(f, s, it->len) > room * 5 / 4)
                f = smaller(f);
            if (y - f->ascent > clip.clip.y + clip.clip.h || y + f->height < clip.clip.y)
                break;
            if (find[0] && item_matches(d, it, find))
                gfx_fill(&clip, (struct rect){x - 1, y - f->ascent - 1, MAX(room, gfx_text_width_n(f, s, it->len)) + 2,
                                              f->height + 2}, 0xffffe066);
            gfx_text_n(&clip, x, y - f->ascent, f, s, it->len, it->color);
            break;
        }
        }
    }
}

static int fit_zoom(const struct reader *r, int view_w)
{
    int32_t widest = A4_W;
    for (int i = 0; i < r->doc.page_count; i++)
        widest = MAX(widest, r->doc.pages[i].w);
    int fit = (int)((int64_t)MAX(200, view_w - 2 * GAP) * PT * 256 / widest);
    return fit * r->zoom / 100;
}

static int total_height(const struct reader *r, int z)
{
    int h = GAP;
    for (int i = 0; i < r->doc.page_count; i++)
        h += scale(r->doc.pages[i].h, z) + GAP;
    return h;
}

static int page_top(const struct reader *r, int z, int page)
{
    int y = GAP;
    for (int i = 0; i < page && i < r->doc.page_count; i++)
        y += scale(r->doc.pages[i].h, z) + GAP;
    return y;
}

static int page_at(const struct reader *r, int z, int y)
{
    int top = GAP;
    for (int i = 0; i < r->doc.page_count; i++) {
        top += scale(r->doc.pages[i].h, z) + GAP;
        if (y < top)
            return i;
    }
    return MAX(0, r->doc.page_count - 1);
}

/* ---- opening ---- */

static void open_path(struct window *w, struct reader *r, const char *path)
{
    struct doc d;
    int err = doc_load(&d, path);
    if (err) {
        char msg[200];
        ksnprintf(msg, sizeof(msg), "\"%s\" could not be read: %s.", path_basename(path),
                  err == E_INVAL  ? "the file is damaged or of a kind Reader does not know"
                  : err == E_PERM ? "it is protected with a password"
                                  : err_str(err));
        gui_message("Reader", msg);
        return;
    }
    doc_free(&r->doc);
    r->doc = d;
    str_copy(r->path, path, sizeof(r->path));
    str_copy(r->title, path_basename(path), sizeof(r->title));
    r->scroll = 0;
    r->zoom = 100;
    r->match = -1;
    if (w) {
        wm_set_title(w, r->title);
        wm_invalidate(w);
    }
}

static void picked(void *ctx, const char *path)
{
    struct window *w = ctx;
    if (path && wm_window_alive(w))
        open_path(w, w->data, path);
}

static void scan_recent(struct reader *r)
{
    static const char *const dirs[] = {"Documents", "Downloads", "Desktop"};
    r->recent_count = 0;
    for (size_t k = 0; k < ARRAY_SIZE(dirs); k++) {
        char dir[PATH_MAX_LEN];
        path_join(user_home(), dirs[k], dir, sizeof(dir));
        struct vdir vd;
        if (vfs_opendir(dir, &vd))
            continue;
        struct vfs_dirent e;
        while (r->recent_count < MAX_RECENT && vfs_readdir(&vd, &e) == 0)
            if (e.type != VN_DIR && path_ext(e.name) && strstr(EXTS, path_ext(e.name)))
                path_join(dir, e.name, r->recent[r->recent_count++], PATH_MAX_LEN);
        vfs_closedir(&vd);
    }
}

static void copy_text(struct reader *r)
{
    size_t len;
    char *t = doc_plain(&r->doc, &len);
    if (t) {
        gui_set_clipboard(t, len);
        kfree(t);
        gui_notify("Reader", "The text of the document is on the clipboard.");
    }
}

/* the next page with a match after the current one, and how many items match */
static void find_next(struct reader *r, int z, int view_h)
{
    const char *q = r->find.buf;
    r->matches = 0;
    int first = -1, after = -1, cur = page_at(r, z, r->scroll + view_h / 3);
    for (int p = 0; p < r->doc.page_count; p++) {
        const struct doc_page *pg = &r->doc.pages[p];
        bool hit = false;
        for (int i = pg->first; i < pg->first + pg->count; i++)
            if (item_matches(&r->doc, &r->doc.items[i], q)) {
                r->matches++;
                hit = true;
            }
        if (hit && first < 0)
            first = p;
        if (hit && p > cur && after < 0)
            after = p;
    }
    int go = after >= 0 ? after : first;
    if (go >= 0)
        r->scroll = page_top(r, z, go) - GAP;
}

enum { CMD_OPEN = 1, CMD_COPY, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_FIT, CMD_DELETE_PAGE };

static int current_page(struct window *w, const struct reader *r)
{
    int z = fit_zoom(r, w->surface.w);
    return page_at(r, z, r->scroll + (w->surface.h - TOOLBAR_HEIGHT) / 3);
}

static void delete_page(struct window *w, const char *value)
{
    UNUSED(value);
    struct reader *r = w->data;
    int page = current_page(w, r), scroll = r->scroll;
    int err = pdf_delete_page(r->path, &r->doc, page);
    if (err) {
        gui_message("Reader", err == E_INVAL ? "This page cannot be deleted from this document."
                                             : "The document could not be saved.");
        return;
    }
    char path[PATH_MAX_LEN];
    str_copy(path, r->path, sizeof(path));
    open_path(w, r, path);
    r->scroll = scroll;
}

static void frame(struct ui *ui, void *data)
{
    struct reader *r = data;
    int W = ui->win->surface.w, H = ui->win->surface.h, T = TOOLBAR_HEIGHT;
    struct rect view = {0, T, W, H - T};
    ui_panel(ui, view, style.window);
    int key = ui_key(ui);
    int cmd = key >= KEY_COMMAND(0) && key < KEY_COMMAND(16) ? key - KEY_COMMAND(0) : 0;
    if (key == KEY_CTRL('o'))
        cmd = CMD_OPEN;
    if (cmd == CMD_OPEN) {
        struct gui_picker o = {.title = "Open a Document", .exts = EXTS, .button = "Open"};
        gui_file_picker(ui->win, &o, picked, ui->win);
        ui->consumed = true;
    }
    if (!r->doc.page_count) {
        ui_label(ui, (struct rect){0, 0, W, T}, "Reader", &font_bold, style.text, ALIGN_CENTER);
        ui_label(ui, (struct rect){0, T + 30, W, 28}, "Open a PDF, Word, Excel, PowerPoint or OpenDocument file",
                 &font_title, style.text, ALIGN_CENTER);
        if (ui_button_primary(ui, (struct rect){W / 2 - 60, T + 70, 120, 30}, "Open…")) {
            struct gui_picker o = {.title = "Open a Document", .exts = EXTS, .button = "Open"};
            gui_file_picker(ui->win, &o, picked, ui->win);
        }
        if (r->recent_count)
            ui_label(ui, (struct rect){40, T + 124, W - 80, 20}, "In your folders", &font_bold, style.secondary,
                     ALIGN_LEFT);
        for (int i = 0; i < r->recent_count; i++) {
            struct rect row = {40, T + 150 + i * 34, W - 80, 30};
            if (row.y + row.h > H - 8)
                break;
            bool hot = ui_hover(ui, row);
            if (hot)
                ui_panel(ui, row, style.hover);
            if (ui->c)
                gfx_icon(ui->c, row.x + 6, row.y + 3, ICON_READER, 24);
            ui_label(ui, (struct rect){row.x + 40, row.y, row.w - 40, row.h}, path_basename(r->recent[i]), &font_sans,
                     style.text, ALIGN_LEFT);
            if (hot && ui->e && ui->e->type == EV_MOUSE_DOWN) {
                open_path(ui->win, r, r->recent[i]);
                ui->consumed = true;
                return;
            }
        }
        return;
    }
    int z = fit_zoom(r, W), total = total_height(r, z), page_h = H - T;
    r->scroll = CLAMP(r->scroll, 0, MAX(0, total - page_h));
    if (ui->c) {
        struct canvas clip = *ui->c;
        clip.clip = rect_intersect(ui->c->clip, view);
        int y = T + GAP - r->scroll;
        for (int i = 0; i < r->doc.page_count; i++) {
            const struct doc_page *pg = &r->doc.pages[i];
            int ph = scale(pg->h, z), pw = scale(pg->w, z);
            if (y + ph >= T && y <= H)
                draw_page(&clip, &r->doc, pg, MAX(GAP, (W - pw) / 2), y, z, r->find.buf);
            y += ph + GAP;
        }
    }
    int sb = r->scroll, old_zoom = r->zoom;
    if (ui_scrollbar(ui, (struct rect){W - 12, T + 2, 10, H - T - 4}, &sb, total, page_h))
        r->scroll = sb;
    /* toolbar */
    char head[NAME_MAX_LEN + 48];
    ksnprintf(head, sizeof(head), "%s  -  page %d of %d", r->title, page_at(r, z, r->scroll + page_h / 3) + 1,
              r->doc.page_count);
    ui_label(ui, (struct rect){90, 0, W - 420, T}, head, &font_bold, style.text, ALIGN_LEFT);
    int bx = W - 330;
    if (ui_button(ui, (struct rect){bx, (T - 26) / 2, 30, 26}, "-"))
        r->zoom = MAX(25, r->zoom - 25);
    char zl[16];
    ksnprintf(zl, sizeof(zl), "%d%%", r->zoom);
    ui_label(ui, (struct rect){bx + 30, 0, 50, T}, zl, &font_small, style.secondary, ALIGN_CENTER);
    if (ui_button(ui, (struct rect){bx + 80, (T - 26) / 2, 30, 26}, "+"))
        r->zoom = MIN(400, r->zoom + 25);
    bool enter = ui_search(ui, (struct rect){W - 200, (T - 28) / 2, 186, 28}, &r->find, "Find");
    if (enter || strcmp(r->last_find, r->find.buf)) {
        if (r->find.buf[0])
            find_next(r, z, page_h);
        str_copy(r->last_find, r->find.buf, sizeof(r->last_find));
    }
    key = ui_key(ui); /* what the find field did not take */
    if (cmd == CMD_DELETE_PAGE) {
        if (r->doc.page_obj && r->doc.page_count > 1) {
            char msg[96];
            ksnprintf(msg, sizeof(msg), "Page %d will be removed from \"%s\". Earlier versions stay in Rewind.",
                      current_page(ui->win, r) + 1, r->title);
            ui_confirm(ui->win, "Delete this page?", msg, delete_page);
        } else {
            gui_message("Reader", "Only pages of a PDF with more than one page can be deleted.");
        }
        ui->consumed = true;
    }
    if (cmd == CMD_COPY || key == KEY_CTRL('c')) {
        copy_text(r);
        ui->consumed = true;
    }
    int zoom_before = r->zoom;
    if (cmd == CMD_ZOOM_IN || key == '+' || key == '=')
        r->zoom = MIN(400, r->zoom + 25);
    if (cmd == CMD_ZOOM_OUT || key == '-')
        r->zoom = MAX(25, r->zoom - 25);
    if (cmd == CMD_FIT || key == '0')
        r->zoom = 100;
    if (key == KEY_PGDN || key == ' ')
        r->scroll += page_h - 40;
    if (key == KEY_PGUP)
        r->scroll -= page_h - 40;
    if (key == KEY_DOWN)
        r->scroll += 40;
    if (key == KEY_UP)
        r->scroll -= 40;
    if (key == KEY_HOME)
        r->scroll = 0;
    if (key == KEY_END)
        r->scroll = total;
    if (ui->e && ui->e->type == EV_WHEEL) {
        r->scroll -= ui->e->wheel * 60;
        ui->consumed = true;
    }
    if (r->zoom != zoom_before) {
        int page = page_at(r, z, r->scroll + page_h / 3);
        z = fit_zoom(r, W);
        total = total_height(r, z);
        r->scroll = page_top(r, z, page) - GAP;
        ui->consumed = true;
    }
    r->scroll = CLAMP(r->scroll, 0, MAX(0, total - page_h));
    if (r->scroll != sb || r->zoom != old_zoom)
        wm_invalidate(ui->win);
}

static void paint(struct window *w, struct canvas *c)
{
    struct reader *r = w->data;
    ui_run(w, &r->st, c, NULL, frame, r);
}

static void event(struct window *w, struct gui_event *e)
{
    struct reader *r = w->data;
    ui_run(w, &r->st, NULL, e, frame, r);
}

static void destroy(struct window *w)
{
    struct reader *r = w->data;
    doc_free(&r->doc);
    kfree(r);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static const struct menu_item file_menu[] = {
    {"Open…", KEY_COMMAND(CMD_OPEN), "Ctrl+O"}, {"Copy Text", KEY_COMMAND(CMD_COPY), "Ctrl+C"}, {"", 0, NULL},
    {"Delete Page…", KEY_COMMAND(CMD_DELETE_PAGE), NULL}};
static const struct menu_item view_menu[] = {
    {"Zoom In", KEY_COMMAND(CMD_ZOOM_IN), "+"}, {"Zoom Out", KEY_COMMAND(CMD_ZOOM_OUT), "-"},
    {"Actual Width", KEY_COMMAND(CMD_FIT), "0"}};
static const struct app_menu menus[] = {{"File", file_menu, ARRAY_SIZE(file_menu)},
                                        {"View", view_menu, ARRAY_SIZE(view_menu)}};

static void launch(const char *arg)
{
    struct reader *r = kzalloc(sizeof(*r));
    if (!r)
        return;
    r->zoom = 100;
    r->match = -1;
    scan_recent(r);
    struct window *w = wm_open("Reader", ICON_READER, 820, 640, &ops, r,
                               WIN_RESIZABLE | WIN_FULLSIZE | WIN_GLASS | WIN_NOTITLE);
    if (!w) {
        kfree(r);
        return;
    }
    w->toolbar_h = TOOLBAR_HEIGHT;
    w->min_w = 520;
    w->min_h = 360;
    wm_set_menus(w, menus, ARRAY_SIZE(menus));
    if (arg && *arg)
        open_path(w, r, arg);
}

COMMAND(doctext, "pdftotext", "files", "doctext [-i] <file>",
        "print the text of a PDF, Word, Excel, PowerPoint or OpenDocument file (-i: what it is)")
{
    bool info = argc == 3 && !strcmp(argv[1], "-i");
    if (argc != 2 + info)
        return sh_error(sh, "doctext", "usage: doctext [-i] <file>");
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[1 + info], path, sizeof(path));
    struct doc d;
    if (!err)
        err = doc_load(&d, path);
    if (err)
        return sh_error(sh, "doctext", "%s: %s", argv[1 + info], err == E_INVAL ? "not a document Reader knows" : err_str(err));
    if (info) {
        sh_printf(sh, "%s, %d page%s, %d pictures\n", d.kind, d.page_count, d.page_count == 1 ? "" : "s", d.pic_count);
    } else {
        size_t len;
        char *t = doc_plain(&d, &len);
        if (t) {
            for (size_t i = 0; i < len; i++)
                if (t[i] == '\f')
                    t[i] = '\n';
            sh_puts(sh, t);
            kfree(t);
        }
    }
    doc_free(&d);
    return 0;
}

COMMAND(pdfpage, NULL, "files", "pdfpage <file.pdf> delete <page>", "delete a page of a PDF (the old file stays in Rewind)")
{
    if (argc != 4 || strcmp(argv[2], "delete"))
        return sh_error(sh, "pdfpage", "usage: pdfpage <file.pdf> delete <page>");
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[1], path, sizeof(path));
    struct doc d;
    if (!err)
        err = doc_load(&d, path);
    if (err)
        return sh_error(sh, "pdfpage", "%s: %s", argv[1], err_str(err));
    long page = str_to_long(argv[3], NULL, 10);
    err = d.page_obj ? pdf_delete_page(path, &d, (int)page - 1) : E_INVAL;
    int count = d.page_count;
    doc_free(&d);
    if (err)
        return sh_error(sh, "pdfpage", "page %ld cannot be deleted from %s", page, argv[1]);
    sh_printf(sh, "deleted page %ld, %d left\n", page, count - 1);
    return 0;
}

static const struct command *const commands[] = {&cmddef_doctext, &cmddef_pdfpage};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL};
static const struct gui_app reader_app = {.name = "Reader",
                                          .description = "PDF, Word, Excel, PowerPoint and OpenDocument files",
                                          .icon = ICON_READER,
                                          .launch = launch,
                                          .extensions = EXTS};
GUI_APP_FULL(&reader_app, NULL, &ext);
