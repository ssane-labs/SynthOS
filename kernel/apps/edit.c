#include <synth/command.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/string.h>
#include <synth/vfs.h>

#if CONFIG_EDITOR

#define TAB_WIDTH 4

enum syntax {
    SYNTAX_PLAIN,
    SYNTAX_SHELL,
    SYNTAX_ASM,
    SYNTAX_CONFIG,
};

struct line {
    char *text;
    size_t len;
    size_t cap;
};

struct editor {
    struct shell *sh;
    char path[PATH_MAX_LEN];
    struct line *lines;
    size_t count;
    size_t cap;
    size_t cx, cy;
    size_t top, left;
    bool modified;
    bool numbers;
    enum syntax syntax;
    char message[96];
    bool message_error;
    char search[64];
    char *clip;
    int cols, rows;
};

static bool line_reserve(struct line *l, size_t need)
{
    if (need + 1 <= l->cap)
        return true;
    size_t cap = l->cap ? l->cap : 32;
    while (cap < need + 1)
        cap *= 2;
    char *t = krealloc(l->text, cap);
    if (!t)
        return false;
    l->text = t;
    l->cap = cap;
    return true;
}

static bool line_set(struct line *l, const char *s, size_t n)
{
    if (!line_reserve(l, n))
        return false;
    memcpy(l->text, s, n);
    l->len = n;
    l->text[n] = 0;
    return true;
}

static bool insert_line(struct editor *e, size_t at, const char *s, size_t n)
{
    if (e->count == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 64;
        struct line *lines = krealloc(e->lines, cap * sizeof(*lines));
        if (!lines)
            return false;
        e->lines = lines;
        e->cap = cap;
    }
    memmove(&e->lines[at + 1], &e->lines[at], (e->count - at) * sizeof(struct line));
    e->lines[at] = (struct line){0};
    e->count++;
    return line_set(&e->lines[at], s, n);
}

static void delete_line(struct editor *e, size_t at)
{
    kfree(e->lines[at].text);
    memmove(&e->lines[at], &e->lines[at + 1], (e->count - at - 1) * sizeof(struct line));
    e->count--;
    if (e->count == 0)
        insert_line(e, 0, "", 0);
}

static void set_message(struct editor *e, bool error, const char *fmt, ...) PRINTF_LIKE(3, 4);

static void set_message(struct editor *e, bool error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    e->message_error = error;
}

static enum syntax detect_syntax(const char *path)
{
    const char *ext = path_ext(path);
    if (str_casecmp(ext, ".sh") == 0 || str_casecmp(ext, ".syn") == 0 || str_ends(path, "synthrc"))
        return SYNTAX_SHELL;
    if (str_casecmp(ext, ".sasm") == 0 || str_casecmp(ext, ".asm") == 0 || str_casecmp(ext, ".inc") == 0)
        return SYNTAX_ASM;
    if (str_casecmp(ext, ".theme") == 0 || str_casecmp(ext, ".conf") == 0)
        return SYNTAX_CONFIG;
    return SYNTAX_PLAIN;
}

static int load(struct editor *e)
{
    char *data;
    size_t size;
    int err = vfs_read_all(e->path, &data, &size);
    if (err == E_NOENT) {
        insert_line(e, 0, "", 0);
        set_message(e, false, "new file");
        return 0;
    }
    if (err)
        return err;
    char *expanded = kmalloc(CONFIG_LINE_MAX * 4);
    size_t start = 0;
    for (size_t i = 0; i <= size && expanded; i++) {
        if (i < size && data[i] != '\n')
            continue;
        size_t n = 0;
        for (size_t k = start; k < i && n + TAB_WIDTH < CONFIG_LINE_MAX * 4; k++) {
            if (data[k] == '\r')
                continue;
            if (data[k] == '\t') {
                do
                    expanded[n++] = ' ';
                while (n % TAB_WIDTH);
            } else {
                expanded[n++] = data[k];
            }
        }
        if (i < size || n > 0 || e->count == 0)
            insert_line(e, e->count, expanded, n);
        start = i + 1;
    }
    kfree(expanded);
    kfree(data);
    set_message(e, false, "%u lines", (unsigned)e->count);
    return 0;
}

static int save(struct editor *e)
{
    size_t total = 0;
    for (size_t i = 0; i < e->count; i++)
        total += e->lines[i].len + 1;
    char *buf = kmalloc(total + 1);
    if (!buf)
        return E_NOMEM;
    size_t pos = 0;
    for (size_t i = 0; i < e->count; i++) {
        memcpy(buf + pos, e->lines[i].text, e->lines[i].len);
        pos += e->lines[i].len;
        if (i + 1 < e->count || e->lines[i].len)
            buf[pos++] = '\n';
    }
    int err = vfs_write_all(e->path, buf, pos);
    kfree(buf);
    if (err)
        return err;
    e->modified = false;
    set_message(e, false, "saved %s (%u bytes)", e->path, (unsigned)pos);
    return 0;
}

static bool is_keyword(const char *s, size_t n)
{
    static const char *const words[] = {"if", "elif", "else", "end", "fi", "done", "while", "for", "in",
                                        "func", "function", "return", "break", "continue", "exit"};
    for (size_t i = 0; i < ARRAY_SIZE(words); i++)
        if (strlen(words[i]) == n && memcmp(words[i], s, n) == 0)
            return true;
    return false;
}

static void highlight(struct editor *e, const struct line *l, uint8_t *attrs)
{
    uint8_t text = theme_attr(ROLE_TEXT), comment = theme_attr(ROLE_MUTED);
    uint8_t string = theme_attr(ROLE_SYNTAX_STRING), keyword = theme_attr(ROLE_SYNTAX_OPERATOR);
    uint8_t number = theme_attr(ROLE_ACCENT2), variable = theme_attr(ROLE_SYNTAX_VARIABLE);
    uint8_t label = theme_attr(ROLE_SYNTAX_COMMAND);
    memset(attrs, text, l->len);
    if (e->syntax == SYNTAX_PLAIN)
        return;
    char comment_char = e->syntax == SYNTAX_ASM ? ';' : '#';
    char quote = 0;
    bool word_start = true;
    for (size_t i = 0; i < l->len; i++) {
        char c = l->text[i];
        if (quote) {
            attrs[i] = string;
            if (c == '\\' && i + 1 < l->len)
                attrs[++i] = string;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == comment_char || (e->syntax == SYNTAX_ASM && c == '#')) {
            memset(attrs + i, comment, l->len - i);
            return;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            attrs[i] = string;
        } else if (c == '$' && e->syntax == SYNTAX_SHELL) {
            attrs[i] = variable;
            while (i + 1 < l->len && (is_ident((uint8_t)l->text[i + 1]) || l->text[i + 1] == '{' || l->text[i + 1] == '}' || l->text[i + 1] == '?'))
                attrs[++i] = variable;
        } else if (word_start && (is_alpha((uint8_t)c) || c == '.' || c == '_')) {
            size_t j = i;
            while (j < l->len && (is_ident((uint8_t)l->text[j]) || l->text[j] == '.'))
                j++;
            uint8_t a = text;
            if (e->syntax == SYNTAX_SHELL && is_keyword(l->text + i, j - i))
                a = keyword;
            else if (e->syntax == SYNTAX_ASM && c == '.')
                a = keyword;
            else if (e->syntax == SYNTAX_ASM && j < l->len && l->text[j] == ':')
                a = label, j++;
            else if (e->syntax == SYNTAX_CONFIG && j < l->len)
                a = label;
            memset(attrs + i, a, j - i);
            i = j - 1;
        } else if (word_start && is_digit((uint8_t)c)) {
            size_t j = i;
            while (j < l->len && is_alnum((uint8_t)l->text[j]))
                j++;
            memset(attrs + i, number, j - i);
            i = j - 1;
        }
        word_start = !is_ident((uint8_t)c) && c != '.';
    }
}

static int gutter_width(struct editor *e)
{
    if (!e->numbers)
        return 0;
    int digits = 1;
    for (size_t n = e->count; n >= 10; n /= 10)
        digits++;
    return MAX(digits, 3) + 2;
}

static void scroll_into_view(struct editor *e)
{
    int text_rows = e->rows - 2;
    int text_cols = e->cols - gutter_width(e);
    if (e->cy < e->top)
        e->top = e->cy;
    if (e->cy >= e->top + (size_t)text_rows)
        e->top = e->cy - (size_t)text_rows + 1;
    if (e->cx < e->left)
        e->left = e->cx;
    if (e->cx >= e->left + (size_t)text_cols)
        e->left = e->cx - (size_t)text_cols + 1;
}

static void draw_bar(int y, const char *left, const char *right, uint8_t attr, int cols)
{
    con_fill(0, y, cols, 1, ' ', attr);
    con_put_text(0, y, left, attr);
    if (right)
        con_put_text(MAX(0, cols - (int)strlen(right)), y, right, attr);
}

static void draw_help(struct editor *e, int y)
{
    static const char *const items[][2] = {
        {"^S", "Save"}, {"^Q", "Quit"}, {"^F", "Find"}, {"F3", "Next"}, {"^G", "Line"},
        {"^K", "Cut"},  {"^U", "Paste"}, {"^D", "Dup"}, {"^N", "Numbers"},
    };
    uint8_t base = theme_attr(ROLE_STATUS), key = theme_attr(ROLE_STATUS_ACCENT);
    con_fill(0, y, e->cols, 1, ' ', base);
    if (e->message[0]) {
        con_put_text(1, y, e->message, e->message_error ? ATTR(ATTR_FG(theme_attr(ROLE_ERROR)), ATTR_BG(base)) : base);
        return;
    }
    int x = 0;
    for (size_t i = 0; i < ARRAY_SIZE(items) && x + 10 < e->cols; i++) {
        con_put_text(x, y, items[i][0], key);
        x += (int)strlen(items[i][0]);
        con_put_cell(x++, y, ' ', base);
        con_put_text(x, y, items[i][1], base);
        x += (int)strlen(items[i][1]) + 2;
    }
}

static void render(struct editor *e)
{
    e->cols = con_cols();
    e->rows = con_rows();
    scroll_into_view(e);
    char left[PATH_MAX_LEN + 32], right[48];
    ksnprintf(left, sizeof(left), " \x04 Edit  %s%s", e->path, e->modified ? " [modified]" : "");
    ksnprintf(right, sizeof(right), "Ln %u/%u  Col %u ", (unsigned)e->cy + 1, (unsigned)e->count,
              (unsigned)e->cx + 1);
    draw_bar(0, left, right, theme_attr(ROLE_STATUS), e->cols);

    int gutter = gutter_width(e);
    uint8_t *attrs = kmalloc(CONFIG_LINE_MAX * 4);
    for (int y = 1; y < e->rows - 1; y++) {
        size_t idx = e->top + (size_t)(y - 1);
        con_fill(0, y, e->cols, 1, ' ', theme_attr(ROLE_TEXT));
        if (idx >= e->count) {
            con_put_cell(gutter, y, '~', theme_attr(ROLE_MUTED));
            continue;
        }
        if (gutter) {
            char num[16];
            ksnprintf(num, sizeof(num), "%*u ", gutter - 2, (unsigned)idx + 1);
            con_put_text(0, y, num, theme_attr(idx == e->cy ? ROLE_ACCENT : ROLE_MUTED));
            con_put_cell(gutter - 1, y, 0xb3, theme_attr(ROLE_MUTED));
        }
        struct line *l = &e->lines[idx];
        if (attrs && l->len <= CONFIG_LINE_MAX * 4)
            highlight(e, l, attrs);
        for (int x = gutter; x < e->cols; x++) {
            size_t col = e->left + (size_t)(x - gutter);
            if (col >= l->len)
                break;
            con_put_cell(x, y, (uint8_t)l->text[col], attrs && l->len <= CONFIG_LINE_MAX * 4 ? attrs[col] : theme_attr(ROLE_TEXT));
        }
    }
    kfree(attrs);
    draw_help(e, e->rows - 1);
    con_goto(gutter + (int)(e->cx - e->left), 1 + (int)(e->cy - e->top));
    con_flush();
}

static bool prompt(struct editor *e, const char *label, char *buf, size_t cap)
{
    size_t len = strlen(buf);
    for (;;) {
        uint8_t base = theme_attr(ROLE_STATUS);
        con_fill(0, e->rows - 1, e->cols, 1, ' ', base);
        con_put_text(1, e->rows - 1, label, theme_attr(ROLE_STATUS_ACCENT));
        int x = (int)strlen(label) + 2;
        con_put_text(x, e->rows - 1, buf, base);
        con_goto(x + (int)len, e->rows - 1);
        con_flush();
        int key = input_getkey(-1);
        if (key == KEY_ENTER)
            return true;
        if (key == KEY_ESC || key == KEY_CTRL('c') || key == KEY_CTRL('q') || key == KEY_EOF)
            return false;
        if (key == KEY_BACKSPACE && len > 0)
            buf[--len] = 0;
        else if (key >= 0x20 && key < 0x100 && key != 0x7f && len + 1 < cap) {
            buf[len++] = (char)key;
            buf[len] = 0;
        }
    }
}

static void clamp_cursor(struct editor *e)
{
    if (e->cy >= e->count)
        e->cy = e->count - 1;
    if (e->cx > e->lines[e->cy].len)
        e->cx = e->lines[e->cy].len;
}

static void insert_char(struct editor *e, char c)
{
    struct line *l = &e->lines[e->cy];
    if (!line_reserve(l, l->len + 1))
        return;
    memmove(l->text + e->cx + 1, l->text + e->cx, l->len - e->cx + 1);
    l->text[e->cx++] = c;
    l->len++;
    e->modified = true;
}

static void newline(struct editor *e)
{
    struct line *l = &e->lines[e->cy];
    size_t indent = 0;
    while (indent < l->len && indent < e->cx && l->text[indent] == ' ')
        indent++;
    char *rest = kmalloc(l->len - e->cx + indent + 1);
    if (!rest)
        return;
    memset(rest, ' ', indent);
    memcpy(rest + indent, l->text + e->cx, l->len - e->cx);
    size_t rest_len = l->len - e->cx + indent;
    l->len = e->cx;
    l->text[l->len] = 0;
    insert_line(e, e->cy + 1, rest, rest_len);
    kfree(rest);
    e->cy++;
    e->cx = indent;
    e->modified = true;
}

static void backspace(struct editor *e)
{
    if (e->cx > 0) {
        struct line *l = &e->lines[e->cy];
        size_t remove = 1;
        if (e->cx % TAB_WIDTH == 0 && e->cx >= TAB_WIDTH) {
            bool spaces = true;
            for (size_t i = e->cx - TAB_WIDTH; i < e->cx; i++)
                spaces &= l->text[i] == ' ';
            bool leading = true;
            for (size_t i = 0; i < e->cx; i++)
                leading &= l->text[i] == ' ';
            if (spaces && leading)
                remove = TAB_WIDTH;
        }
        memmove(l->text + e->cx - remove, l->text + e->cx, l->len - e->cx + 1);
        l->len -= remove;
        e->cx -= remove;
    } else if (e->cy > 0) {
        struct line *prev = &e->lines[e->cy - 1], *cur = &e->lines[e->cy];
        size_t at = prev->len;
        if (!line_reserve(prev, prev->len + cur->len))
            return;
        memcpy(prev->text + prev->len, cur->text, cur->len + 1);
        prev->len += cur->len;
        delete_line(e, e->cy);
        e->cy--;
        e->cx = at;
    } else {
        return;
    }
    e->modified = true;
}

static void delete_char(struct editor *e)
{
    struct line *l = &e->lines[e->cy];
    if (e->cx < l->len) {
        memmove(l->text + e->cx, l->text + e->cx + 1, l->len - e->cx);
        l->len--;
        e->modified = true;
    } else if (e->cy + 1 < e->count) {
        e->cy++;
        e->cx = 0;
        backspace(e);
    }
}

static void find_next(struct editor *e)
{
    if (!e->search[0])
        return;
    size_t n = strlen(e->search);
    for (size_t k = 0; k <= e->count; k++) {
        size_t idx = (e->cy + k) % e->count;
        struct line *l = &e->lines[idx];
        size_t from = k == 0 ? e->cx + 1 : 0;
        for (size_t i = from; i + n <= l->len; i++) {
            if (str_ncasecmp(l->text + i, e->search, n) == 0) {
                e->cy = idx;
                e->cx = i;
                set_message(e, false, "found \"%s\"", e->search);
                return;
            }
        }
    }
    set_message(e, true, "\"%s\" not found", e->search);
}

static bool confirm_quit(struct editor *e)
{
    if (!e->modified)
        return true;
    char answer[4] = "";
    if (!prompt(e, "Save changes? (y/n)", answer, sizeof(answer)))
        return false;
    if (to_lower((uint8_t)answer[0]) == 'y') {
        int err = save(e);
        if (err) {
            set_message(e, true, "save failed: %s", err_str(err));
            return false;
        }
        return true;
    }
    return to_lower((uint8_t)answer[0]) == 'n';
}

static void run(struct editor *e)
{
    for (;;) {
        render(e);
        int key = input_getkey(-1);
        int base = KEY_BASE(key);
        bool ctrl = key & KEY_MOD_CTRL;
        if (key != KEY_NONE)
            e->message[0] = 0;
        size_t page = (size_t)MAX(1, e->rows - 3);
        if (key == KEY_CTRL('q') || key == KEY_EOF || key == KEY_CTRL('x')) {
            if (confirm_quit(e))
                return;
        } else if (key == KEY_CTRL('s')) {
            int err = save(e);
            if (err)
                set_message(e, true, "cannot save: %s", err_str(err));
        } else if (key == KEY_CTRL('f')) {
            if (prompt(e, "Find:", e->search, sizeof(e->search)))
                find_next(e);
        } else if (base == KEY_F3) {
            find_next(e);
        } else if (key == KEY_CTRL('g')) {
            char buf[16] = "";
            long n;
            if (prompt(e, "Go to line:", buf, sizeof(buf)) && str_parse_long(buf, &n) && n > 0) {
                e->cy = (size_t)n - 1;
                e->cx = 0;
            }
        } else if (key == KEY_CTRL('k')) {
            kfree(e->clip);
            e->clip = kstrdup(e->lines[e->cy].text);
            delete_line(e, e->cy);
            e->modified = true;
        } else if (key == KEY_CTRL('u')) {
            if (e->clip) {
                insert_line(e, e->cy, e->clip, strlen(e->clip));
                e->cy++;
                e->modified = true;
            }
        } else if (key == KEY_CTRL('d')) {
            insert_line(e, e->cy + 1, e->lines[e->cy].text, e->lines[e->cy].len);
            e->cy++;
            e->modified = true;
        } else if (key == KEY_CTRL('n')) {
            e->numbers = !e->numbers;
        } else if (base == KEY_UP) {
            if (e->cy > 0)
                e->cy--;
        } else if (base == KEY_DOWN) {
            if (e->cy + 1 < e->count)
                e->cy++;
        } else if (base == KEY_LEFT) {
            if (e->cx > 0)
                e->cx--;
            else if (e->cy > 0)
                e->cy--, e->cx = e->lines[e->cy].len;
        } else if (base == KEY_RIGHT) {
            if (e->cx < e->lines[e->cy].len)
                e->cx++;
            else if (e->cy + 1 < e->count)
                e->cy++, e->cx = 0;
        } else if (base == KEY_HOME) {
            if (ctrl)
                e->cy = 0;
            e->cx = 0;
        } else if (base == KEY_END) {
            if (ctrl)
                e->cy = e->count - 1;
            e->cx = e->lines[e->cy].len;
        } else if (base == KEY_PGUP) {
            e->cy = e->cy > page ? e->cy - page : 0;
        } else if (base == KEY_PGDN) {
            e->cy = MIN(e->cy + page, e->count - 1);
        } else if (key == KEY_ENTER) {
            newline(e);
        } else if (key == KEY_BACKSPACE) {
            backspace(e);
        } else if (base == KEY_DELETE) {
            delete_char(e);
        } else if (key == KEY_TAB) {
            do
                insert_char(e, ' ');
            while (e->cx % TAB_WIDTH);
        } else if (key >= 0x20 && key < 0x100 && key != 0x7f) {
            insert_char(e, (char)key);
        }
        clamp_cursor(e);
    }
}

COMMAND(edit, "nano,notepad", "files", "edit file", "full-screen text editor with syntax highlighting")
{
    if (argc != 2)
        return command_usage(sh, command_find("edit"));
    if (!con_is_grid())
        return sh_error(sh, "edit", "needs a full-screen display");
    struct editor *e = kzalloc(sizeof(*e));
    if (!e)
        return 1;
    e->sh = sh;
    e->numbers = true;
    int err = sh_resolve(sh, argv[1], e->path, sizeof(e->path));
    if (!err && vfs_is_dir(e->path))
        err = E_ISDIR;
    if (!err)
        err = load(e);
    if (err) {
        kfree(e);
        return sh_fs_error(sh, "edit", argv[1], err);
    }
    e->syntax = detect_syntax(e->path);
    input_set_break(false);
    run(e);
    input_set_break(true);
    for (size_t i = 0; i < e->count; i++)
        kfree(e->lines[i].text);
    kfree(e->lines);
    kfree(e->clip);
    kfree(e);
    con_clear();
    con_update_status();
    con_flush();
    return 0;
}

#endif
