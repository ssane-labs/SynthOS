#include <synth/config.h>
#include <synth/console.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/lineedit.h>
#include <synth/string.h>

struct editor {
    char *buf;
    size_t cap;
    size_t len;
    size_t pos;
    const struct lineedit_hooks *h;
    int sx, sy;
    size_t drawn;
    const char *suggestion;
    size_t hist;
    char *stash;
    bool tabbed;
    uint8_t *attrs;
};

void completion_add(struct completion *c, const char *item)
{
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i], item) == 0)
            return;
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        char **items = krealloc(c->items, cap * sizeof(char *));
        if (!items)
            return;
        c->items = items;
        c->cap = cap;
    }
    c->items[c->count++] = kstrdup(item);
}

void completion_free(struct completion *c)
{
    for (size_t i = 0; i < c->count; i++)
        kfree(c->items[i]);
    kfree(c->items);
    c->items = NULL;
    c->count = c->cap = 0;
}

static void find_suggestion(struct editor *e)
{
    e->suggestion = NULL;
    if (!e->h || !e->h->suggest || !e->h->history || e->len == 0 || e->pos != e->len)
        return;
    const struct history *h = e->h->history;
    for (size_t i = h->count; i-- > 0;) {
        const char *item = h->items[i];
        if (strlen(item) > e->len && strncmp(item, e->buf, e->len) == 0) {
            e->suggestion = item;
            return;
        }
    }
}

static void render(struct editor *e, bool with_suggestion)
{
    int cols = con_cols(), rows = con_rows();
    uint32_t scrolls = con_scroll_count();
    con_hold(true);
    con_goto(e->sx, e->sy < 0 ? 0 : e->sy);

    uint8_t text = theme_attr(ROLE_TEXT);
    memset(e->attrs, text, e->len);
    if (e->h && e->h->highlight)
        e->h->highlight(e->h->ctx, e->buf, e->len, e->attrs);
    for (size_t i = 0; i < e->len; i++) {
        con_set_attr(e->attrs[i]);
        con_putc(e->buf[i]);
    }
    size_t total = e->len;
    if (with_suggestion && e->suggestion) {
        con_role(ROLE_SUGGESTION);
        const char *rest = e->suggestion + e->len;
        con_puts(rest);
        total += strlen(rest);
    }
    con_reset();
    for (size_t i = total; i < e->drawn; i++)
        con_putc(' ');
    e->drawn = total;
    e->sy -= (int)(con_scroll_count() - scrolls);

    size_t off = (size_t)e->sx + e->pos;
    int y = e->sy + (int)(off / (size_t)cols);
    int x = (int)(off % (size_t)cols);
    while (y >= rows) {
        con_goto(cols - 1, rows - 1);
        con_putc('\n');
        e->sy--;
        y--;
    }
    con_goto(x, y);
    con_hold(false);
}

static void set_text(struct editor *e, const char *s)
{
    e->len = str_copy(e->buf, s, e->cap);
    if (e->len >= e->cap)
        e->len = e->cap - 1;
    e->pos = e->len;
}

static void insert(struct editor *e, const char *s, size_t n)
{
    if (e->len + n + 1 > e->cap)
        n = e->cap - 1 - e->len;
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos + 1);
    memcpy(e->buf + e->pos, s, n);
    e->len += n;
    e->pos += n;
}

static void erase(struct editor *e, size_t from, size_t to)
{
    if (to <= from)
        return;
    memmove(e->buf + from, e->buf + to, e->len - to + 1);
    e->len -= to - from;
    if (e->pos > to)
        e->pos -= to - from;
    else if (e->pos > from)
        e->pos = from;
}

static size_t word_left(struct editor *e)
{
    size_t p = e->pos;
    while (p > 0 && e->buf[p - 1] == ' ')
        p--;
    while (p > 0 && e->buf[p - 1] != ' ' && e->buf[p - 1] != '/')
        p--;
    return p;
}

static size_t word_right(struct editor *e)
{
    size_t p = e->pos;
    while (p < e->len && e->buf[p] == ' ')
        p++;
    while (p < e->len && e->buf[p] != ' ' && e->buf[p] != '/')
        p++;
    if (p < e->len && e->buf[p] == '/')
        p++;
    return p;
}

static void restart(struct editor *e)
{
    if (e->h && e->h->prompt)
        e->h->prompt(e->h->ctx);
    con_cursor(&e->sx, &e->sy);
    if (e->sx >= con_cols()) {
        con_putc('\n');
        con_cursor(&e->sx, &e->sy);
    }
    e->drawn = 0;
}

static void finish_line(struct editor *e)
{
    e->pos = e->len;
    e->suggestion = NULL;
    render(e, false);
    int x;
    con_cursor(&x, NULL);
    if (!(e->len > 0 && x == 0 && ((size_t)e->sx + e->len) % (size_t)con_cols() == 0))
        con_putc('\n');
}

static void list_completions(struct editor *e, struct completion *c)
{
    e->pos = e->len;
    render(e, false);
    con_putc('\n');
    size_t width = 0;
    for (size_t i = 0; i < c->count; i++)
        width = MAX(width, strlen(c->items[i]));
    width += 2;
    size_t per_row = MAX((size_t)1, (size_t)con_cols() / width);
    for (size_t i = 0; i < c->count; i++) {
        const char *item = c->items[i];
        con_role(str_ends(item, "/") ? ROLE_DIR : ROLE_TEXT);
        con_puts(item);
        con_reset();
        if ((i + 1) % per_row == 0 || i + 1 == c->count)
            con_putc('\n');
        else
            for (size_t k = strlen(item); k < width; k++)
                con_putc(' ');
    }
    restart(e);
}

static void complete(struct editor *e)
{
    if (!e->h || !e->h->complete)
        return;
    struct completion c = {0};
    e->h->complete(e->h->ctx, e->buf, e->pos, &c);
    if (c.count == 0) {
        completion_free(&c);
        return;
    }
    size_t word_len = e->pos - c.start;
    size_t common = strlen(c.items[0]);
    for (size_t i = 1; i < c.count; i++) {
        size_t k = 0;
        while (k < common && c.items[i][k] == c.items[0][k])
            k++;
        common = k;
    }
    if (c.count == 1) {
        erase(e, c.start, e->pos);
        insert(e, c.items[0], strlen(c.items[0]));
        if (!str_ends(c.items[0], "/"))
            insert(e, " ", 1);
    } else if (common > word_len) {
        erase(e, c.start, e->pos);
        insert(e, c.items[0], common);
    } else if (e->tabbed) {
        list_completions(e, &c);
    }
    e->tabbed = c.count > 1;
    completion_free(&c);
}

static void history_move(struct editor *e, int dir)
{
    const struct history *h = e->h ? e->h->history : NULL;
    if (!h || h->count == 0)
        return;
    if (dir < 0 && e->hist > 0) {
        if (e->hist == h->count) {
            kfree(e->stash);
            e->stash = kstrdup(e->buf);
        }
        set_text(e, h->items[--e->hist]);
    } else if (dir > 0 && e->hist < h->count) {
        e->hist++;
        set_text(e, e->hist == h->count ? (e->stash ? e->stash : "") : h->items[e->hist]);
    }
}

static int read_stream(char *buf, size_t cap)
{
    size_t len = 0;
    for (;;) {
        int key = input_getkey(-1);
        if (key == KEY_EOF)
            return len ? (int)len : LE_EOF;
        if (key == KEY_ENTER) {
            con_putc('\n');
            break;
        }
        if (key == KEY_CTRL('c')) {
            con_puts("^C\n");
            return LE_CANCEL;
        }
        if (key == KEY_BACKSPACE) {
            if (len) {
                len--;
                con_puts("\b \b");
            }
            continue;
        }
        if (key >= 0x20 && key < 0x100 && key != 0x7f && len + 1 < cap) {
            buf[len++] = (char)key;
            con_putc((char)key);
        }
    }
    buf[len] = 0;
    return (int)len;
}

int lineedit_read(char *buf, size_t cap, const struct lineedit_hooks *hooks)
{
    buf[0] = 0;
    if (!con_is_grid()) {
        if (hooks && hooks->prompt)
            hooks->prompt(hooks->ctx);
        return read_stream(buf, cap);
    }
    struct editor e = {.buf = buf, .cap = cap, .h = hooks};
    e.attrs = kmalloc(cap);
    if (!e.attrs)
        return LE_CANCEL;
    e.hist = hooks && hooks->history ? hooks->history->count : 0;
    restart(&e);

    int result = LE_CANCEL;
    for (;;) {
        find_suggestion(&e);
        render(&e, true);
        int key = input_getkey(1000);
        if (key == KEY_NONE) {
            con_update_status();
            con_flush();
            continue;
        }
        bool was_tab = e.tabbed;
        e.tabbed = false;
        int base = KEY_BASE(key);
        bool ctrl = key & KEY_MOD_CTRL, alt = key & KEY_MOD_ALT;

        if (key == KEY_ENTER) {
            finish_line(&e);
            result = (int)e.len;
            break;
        } else if (key == KEY_EOF || (key == KEY_CTRL('d') && e.len == 0)) {
            finish_line(&e);
            result = LE_EOF;
            break;
        } else if (key == KEY_CTRL('c')) {
            e.pos = e.len;
            render(&e, false);
            con_role(ROLE_MUTED);
            con_puts("^C");
            con_reset();
            con_putc('\n');
            result = LE_CANCEL;
            break;
        } else if (key == KEY_TAB) {
            e.tabbed = was_tab;
            complete(&e);
        } else if (key == KEY_BACKSPACE && alt) {
            erase(&e, word_left(&e), e.pos);
        } else if (key == KEY_BACKSPACE || key == KEY_CTRL('h')) {
            if (e.pos > 0)
                erase(&e, e.pos - 1, e.pos);
        } else if (base == KEY_DELETE || key == KEY_CTRL('d')) {
            if (e.pos < e.len)
                erase(&e, e.pos, e.pos + 1);
        } else if (base == KEY_LEFT || key == KEY_CTRL('b')) {
            if (ctrl || alt)
                e.pos = word_left(&e);
            else if (e.pos > 0)
                e.pos--;
        } else if (base == KEY_RIGHT || key == KEY_CTRL('f')) {
            if (e.pos == e.len && e.suggestion)
                set_text(&e, e.suggestion);
            else if (ctrl || alt)
                e.pos = word_right(&e);
            else if (e.pos < e.len)
                e.pos++;
        } else if (base == KEY_HOME || key == KEY_CTRL('a')) {
            e.pos = 0;
        } else if (base == KEY_END || key == KEY_CTRL('e')) {
            if (e.pos == e.len && e.suggestion)
                set_text(&e, e.suggestion);
            e.pos = e.len;
        } else if (base == KEY_UP || key == KEY_CTRL('p')) {
            history_move(&e, -1);
        } else if (base == KEY_DOWN || key == KEY_CTRL('n')) {
            history_move(&e, 1);
        } else if (key == KEY_CTRL('u')) {
            erase(&e, 0, e.pos);
        } else if (key == KEY_CTRL('k')) {
            erase(&e, e.pos, e.len);
        } else if (key == KEY_CTRL('w')) {
            erase(&e, word_left(&e), e.pos);
        } else if (key == KEY_ESC) {
            set_text(&e, "");
        } else if (key == KEY_CTRL('l')) {
            con_clear();
            con_update_status();
            restart(&e);
        } else if (key == KEY_RESIZE) {
            e.sx = MIN(e.sx, con_cols() - 1);
            e.sy = MIN(e.sy, con_rows() - 1);
        } else if (key >= 0x20 && key < 0x100 && key != 0x7f) {
            char ch = (char)key;
            insert(&e, &ch, 1);
        }
    }
    kfree(e.stash);
    kfree(e.attrs);
    return result;
}

int lineedit_simple(char *buf, size_t cap)
{
    struct lineedit_hooks hooks = {0};
    int n = lineedit_read(buf, cap, &hooks);
    return n == LE_CANCEL ? LE_EOF : n;
}
