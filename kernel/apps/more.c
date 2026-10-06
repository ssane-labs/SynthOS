#include <synth/command.h>
#include <synth/console.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/string.h>
#include <synth/vfs.h>

struct pager {
    char **lines;
    size_t count;
    size_t cap;
};

static void add_line(struct pager *p, const char *s, size_t n)
{
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 256;
        char **lines = krealloc(p->lines, cap * sizeof(char *));
        if (!lines)
            return;
        p->lines = lines;
        p->cap = cap;
    }
    p->lines[p->count++] = kstrndup(s, n);
}

static void wrap(struct pager *p, const char *text, size_t len, int width)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && text[i] != '\n' && (int)(i - start) < width)
            continue;
        if (i < len && text[i] != '\n') {
            add_line(p, text + start, i - start);
            start = i;
            continue;
        }
        if (i < len || i > start)
            add_line(p, text + start, i - start);
        start = i + 1;
    }
}

static void draw(struct pager *p, size_t top, const char *name)
{
    int rows = con_rows() - 1, cols = con_cols();
    for (int y = 0; y < rows; y++) {
        con_fill(0, y, cols, 1, ' ', theme_attr(ROLE_TEXT));
        if (top + (size_t)y < p->count)
            con_put_text(0, y, p->lines[top + (size_t)y], theme_attr(ROLE_TEXT));
    }
    char status[128];
    size_t last = MIN(top + (size_t)rows, p->count);
    unsigned percent = p->count ? (unsigned)(last * 100 / p->count) : 100;
    ksnprintf(status, sizeof(status), " %s  lines %u-%u of %u (%u%%)  Space/PgDn next, b back, q quit ", name,
              (unsigned)(top + 1), (unsigned)last, (unsigned)p->count, percent);
    con_fill(0, rows, cols, 1, ' ', theme_attr(ROLE_STATUS));
    con_put_text(0, rows, status, theme_attr(ROLE_STATUS));
    con_goto(cols - 1, rows);
    con_flush();
}

static int page(struct shell *sh, const char *text, size_t len, const char *name)
{
    if (!con_is_grid() || !stream_is_console(sh->out)) {
        stream_write(sh->out, text, len);
        return 0;
    }
    struct pager p = {0};
    wrap(&p, text, len, con_cols());
    int rows = con_rows() - 1;
    if ((int)p.count <= rows) {
        stream_write(sh->out, text, len);
        if (len && text[len - 1] != '\n')
            sh_puts(sh, "\n");
    } else {
        size_t top = 0, max_top = p.count - (size_t)rows;
        input_set_break(false);
        con_show_cursor(false);
        for (;;) {
            draw(&p, top, name);
            int key = input_getkey(-1);
            int base = KEY_BASE(key);
            if (key == 'q' || key == KEY_ESC || key == KEY_CTRL('c') || key == KEY_EOF)
                break;
            if (key == ' ' || base == KEY_PGDN || key == 'f')
                top = MIN(top + (size_t)rows, max_top);
            else if (key == 'b' || base == KEY_PGUP)
                top = top > (size_t)rows ? top - (size_t)rows : 0;
            else if (key == KEY_ENTER || base == KEY_DOWN || key == 'j')
                top = MIN(top + 1, max_top);
            else if (base == KEY_UP || key == 'k')
                top = top ? top - 1 : 0;
            else if (base == KEY_HOME || key == 'g')
                top = 0;
            else if (base == KEY_END || key == 'G')
                top = max_top;
        }
        con_show_cursor(true);
        input_set_break(true);
        con_clear();
        con_update_status();
        con_flush();
    }
    for (size_t i = 0; i < p.count; i++)
        kfree(p.lines[i]);
    kfree(p.lines);
    return 0;
}

COMMAND(more, "less,pager", "text", "more [file]", "page through text (Space, b, arrows, q)")
{
    char *text;
    long len;
    const char *name = "(input)";
    if (argc > 1) {
        char path[PATH_MAX_LEN];
        size_t size;
        int err = sh_resolve(sh, argv[1], path, sizeof(path));
        if (!err)
            err = vfs_read_all(path, &text, &size);
        if (err)
            return sh_fs_error(sh, "more", argv[1], err);
        len = (long)size;
        name = argv[1];
    } else {
        len = stream_read_all(sh->in, &text);
        if (len < 0)
            return 1;
    }
    int rc = page(sh, text, (size_t)len, name);
    kfree(text);
    return rc;
}
