#include <synth/config.h>
#include <synth/console.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>

enum esc_state {
    ESC_NONE,
    ESC_START,
    ESC_CSI,
};

static struct display *disp;
static struct cell *cells;
static int cols, rows, text_rows;
static int cx, cy, saved_x, saved_y;
static bool wrap_pending;
static bool cursor_on = true;
static uint8_t attr;
static int dirty_y0 = 0, dirty_y1 = 0;
static uint32_t scrolls;
static bool statusbar;
static status_render_fn status_fn;
static enum cursor_shape shape = CURSOR_BLOCK;
static int hold_depth;

static enum esc_state esc;
static int esc_params[8];
static int esc_count;
static bool esc_private;

static const uint8_t ansi_to_vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

static inline struct cell *at(int x, int y)
{
    return &cells[y * cols + x];
}

static void mark(int y0, int y1)
{
    if (dirty_y0 >= dirty_y1) {
        dirty_y0 = y0;
        dirty_y1 = y1;
    } else {
        dirty_y0 = MIN(dirty_y0, y0);
        dirty_y1 = MAX(dirty_y1, y1);
    }
}

static uint8_t blank_attr(void)
{
    return ATTR(ATTR_FG(theme_attr(ROLE_TEXT)), ATTR_BG(attr));
}

static void layout(void)
{
    text_rows = statusbar && rows >= 4 ? rows - 1 : rows;
    if (cy >= text_rows)
        cy = text_rows - 1;
}

static bool alloc_cells(int c, int r)
{
    struct cell *fresh = kmalloc((size_t)c * (size_t)r * sizeof(struct cell));
    if (!fresh)
        return false;
    uint8_t a = theme_attr(ROLE_TEXT);
    for (int i = 0; i < c * r; i++)
        fresh[i] = (struct cell){' ', a};
    if (cells) {
        int keep_top = MAX(0, cy - (r - 1));
        for (int y = 0; y < MIN(rows - keep_top, r); y++)
            memcpy(&fresh[y * c], &cells[(y + keep_top) * cols], (size_t)MIN(cols, c) * sizeof(struct cell));
        cy -= keep_top;
        kfree(cells);
    }
    cells = fresh;
    cols = c;
    rows = r;
    return true;
}

struct display *con_display(void)
{
    return disp;
}

void con_init(struct display *d)
{
    disp = d;
    attr = theme_attr(ROLE_TEXT);
    if (d->caps & DISPLAY_GRID) {
        alloc_cells(MIN(d->cols, CONFIG_MAX_COLS), MIN(d->rows, CONFIG_MAX_ROWS));
        layout();
        con_apply_palette();
        con_clear();
    } else {
        cols = d->cols ? d->cols : 80;
        rows = text_rows = d->rows ? d->rows : 25;
    }
}

void con_resize(int c, int r)
{
    if (!con_is_grid())
        return;
    c = CLAMP(c, 8, CONFIG_MAX_COLS);
    r = CLAMP(r, 2, CONFIG_MAX_ROWS);
    if (c == cols && r == rows)
        return;
    if (!alloc_cells(c, r))
        return;
    cx = MIN(cx, cols - 1);
    layout();
    con_redraw();
}

bool con_is_grid(void)
{
    return disp && (disp->caps & DISPLAY_GRID);
}

int con_cols(void)
{
    return cols;
}

int con_rows(void)
{
    return text_rows;
}

uint32_t con_scroll_count(void)
{
    return scrolls;
}

static void present_dirty(void)
{
    if (dirty_y0 < dirty_y1 && disp->present)
        disp->present(cells, cols, dirty_y0, dirty_y1);
    dirty_y0 = dirty_y1 = 0;
}

static void scroll_up(void)
{
    uint8_t a = blank_attr();
    if (disp->scroll) {
        present_dirty();
        disp->scroll(0, text_rows, 1);
    } else {
        mark(0, text_rows);
    }
    memmove(cells, cells + cols, (size_t)cols * (size_t)(text_rows - 1) * sizeof(struct cell));
    for (int x = 0; x < cols; x++)
        *at(x, text_rows - 1) = (struct cell){' ', a};
    mark(text_rows - 1, text_rows);
    scrolls++;
}

static void newline(void)
{
    cx = 0;
    wrap_pending = false;
    if (++cy >= text_rows) {
        cy = text_rows - 1;
        scroll_up();
    }
}

static void put_glyph(uint8_t ch)
{
    if (wrap_pending)
        newline();
    *at(cx, cy) = (struct cell){ch, attr};
    mark(cy, cy + 1);
    if (++cx >= cols) {
        cx = cols - 1;
        wrap_pending = true;
    }
}

static void sgr(void)
{
    if (esc_count == 0)
        esc_params[esc_count++] = 0;
    uint8_t fg = ATTR_FG(attr), bg = ATTR_BG(attr);
    for (int i = 0; i < esc_count; i++) {
        int p = esc_params[i];
        if (p == 0) {
            fg = ATTR_FG(theme_attr(ROLE_TEXT));
            bg = ATTR_BG(theme_attr(ROLE_TEXT));
        } else if (p == 1) {
            fg |= 8;
        } else if (p == 22) {
            fg &= 7;
        } else if (p == 7) {
            uint8_t t = fg;
            fg = bg;
            bg = t;
        } else if (p >= 30 && p <= 37) {
            fg = (uint8_t)((fg & 8) | ansi_to_vga[p - 30]);
        } else if (p >= 90 && p <= 97) {
            fg = (uint8_t)(8 | ansi_to_vga[p - 90]);
        } else if (p >= 40 && p <= 47) {
            bg = ansi_to_vga[p - 40];
        } else if (p >= 100 && p <= 107) {
            bg = (uint8_t)(8 | ansi_to_vga[p - 100]);
        } else if (p == 39) {
            fg = ATTR_FG(theme_attr(ROLE_TEXT));
        } else if (p == 49) {
            bg = ATTR_BG(theme_attr(ROLE_TEXT));
        } else if ((p == 38 || p == 48) && i + 2 < esc_count && esc_params[i + 1] == 5) {
            int n = esc_params[i + 2] & 15;
            uint8_t c = (uint8_t)((n & 8) | ansi_to_vga[n & 7]);
            if (p == 38)
                fg = c;
            else
                bg = c;
            i += 2;
        }
    }
    attr = ATTR(fg, bg);
}

static void csi(char final)
{
    int p0 = esc_count > 0 ? esc_params[0] : 0;
    int n = p0 > 0 ? p0 : 1;
    if (!con_is_grid() && final != 'm')
        return;
    wrap_pending = false;
    switch (final) {
    case 'm':
        sgr();
        break;
    case 'A':
        cy = MAX(0, cy - n);
        break;
    case 'B':
        cy = MIN(text_rows - 1, cy + n);
        break;
    case 'C':
        cx = MIN(cols - 1, cx + n);
        break;
    case 'D':
        cx = MAX(0, cx - n);
        break;
    case 'G':
        cx = CLAMP(n - 1, 0, cols - 1);
        break;
    case 'H':
    case 'f':
        cy = CLAMP(n - 1, 0, text_rows - 1);
        cx = CLAMP((esc_count > 1 && esc_params[1] > 0 ? esc_params[1] : 1) - 1, 0, cols - 1);
        break;
    case 'J':
        if (p0 == 2 || p0 == 3) {
            con_clear();
        } else if (p0 == 0) {
            con_clear_line_from(cx, cy);
            con_clear_below(cy + 1);
        }
        break;
    case 'K':
        con_clear_line_from(p0 == 2 ? 0 : cx, cy);
        break;
    case 's':
        saved_x = cx, saved_y = cy;
        break;
    case 'u':
        cx = saved_x, cy = saved_y;
        break;
    case 'h':
    case 'l':
        if (esc_private && p0 == 25)
            cursor_on = final == 'h';
        break;
    default:
        break;
    }
}

static void feed(uint8_t ch)
{
    if (esc == ESC_START) {
        if (ch == '[') {
            esc = ESC_CSI;
            esc_count = 0;
            esc_private = false;
            memset(esc_params, 0, sizeof(esc_params));
            return;
        }
        esc = ESC_NONE;
        return;
    }
    if (esc == ESC_CSI) {
        if (is_digit(ch)) {
            if (esc_count == 0)
                esc_count = 1;
            esc_params[esc_count - 1] = esc_params[esc_count - 1] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (esc_count == 0)
                esc_count = 1;
            if (esc_count < (int)ARRAY_SIZE(esc_params))
                esc_params[esc_count++] = 0;
        } else if (ch == '?') {
            esc_private = true;
        } else if (ch >= 0x40 && ch <= 0x7e) {
            esc = ESC_NONE;
            csi((char)ch);
        }
        return;
    }

    switch (ch) {
    case 0x1b:
        esc = ESC_START;
        break;
    case '\n':
        newline();
        break;
    case '\r':
        cx = 0;
        wrap_pending = false;
        break;
    case '\t': {
        int stop = (cx / 8 + 1) * 8;
        if (wrap_pending)
            stop = 8;
        do
            put_glyph(' ');
        while (cx < stop && cx < cols - 1 && !wrap_pending);
        break;
    }
    case '\b':
        if (wrap_pending)
            wrap_pending = false;
        else if (cx > 0)
            cx--;
        break;
    case '\a':
        hal_beep(880, 40);
        break;
    case 0:
        break;
    default:
        put_glyph(ch);
        break;
    }
}

static void stream_feed(const char *s, size_t n)
{
    char out[256];
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (esc != ESC_NONE || ch == 0x1b) {
            if (esc == ESC_CSI && ch == 'm') {
                esc = ESC_NONE;
                continue;
            }
            feed(ch);
            continue;
        }
        if (ch == '\r' || ch == '\n')
            cx = 0;
        else if (ch == '\b')
            cx = cx > 0 ? cx - 1 : 0;
        else
            cx++;
        out[len++] = (char)ch;
        if (len == sizeof(out)) {
            disp->write(out, len);
            len = 0;
        }
    }
    if (len)
        disp->write(out, len);
}

void con_write(const char *s, size_t n)
{
    if (!disp)
        return;
    if (!con_is_grid()) {
        stream_feed(s, n);
        return;
    }
    for (size_t i = 0; i < n; i++)
        feed((uint8_t)s[i]);
    if (!hold_depth)
        con_flush();
}

void con_hold(bool hold)
{
    hold_depth += hold ? 1 : -1;
    if (hold_depth < 0)
        hold_depth = 0;
    if (!hold_depth)
        con_flush();
}

void con_puts(const char *s)
{
    con_write(s, strlen(s));
}

void con_putc(char c)
{
    con_write(&c, 1);
}

static void con_sink(void *ctx, const char *s, size_t n)
{
    UNUSED(ctx);
    con_write(s, n);
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = fmt_format(con_sink, NULL, fmt, ap);
    va_end(ap);
    return n;
}

static int markup_tag(const char *s, size_t *consumed)
{
    const char *end = strchr(s + 1, '}');
    if (!end || end - s - 1 >= 24)
        return -2;
    char name[24];
    size_t n = (size_t)(end - s - 1);
    memcpy(name, s + 1, n);
    name[n] = 0;
    *consumed = n + 2;
    if (strcmp(name, "/") == 0 || strcmp(name, "reset") == 0)
        return -1;
    return theme_role_lookup(name) >= 0 ? theme_role_lookup(name) : -2;
}

void con_markup(const char *s)
{
    while (*s) {
        const char *start = s;
        while (*s && *s != '{')
            s++;
        con_write(start, (size_t)(s - start));
        if (!*s)
            break;
        if (s[1] == '{') {
            con_putc('{');
            s += 2;
            continue;
        }
        size_t used = 0;
        int role = markup_tag(s, &used);
        if (role == -2) {
            con_putc('{');
            s++;
        } else {
            if (role == -1)
                con_reset();
            else
                con_role((enum theme_role)role);
            s += used;
        }
    }
}

size_t markup_strip(const char *s, char *out, size_t cap)
{
    size_t len = 0;
    while (*s) {
        if (*s == '{') {
            size_t used = 0;
            if (s[1] == '{') {
                s += 2;
                if (len + 1 < cap)
                    out[len++] = '{';
                continue;
            }
            if (markup_tag(s, &used) != -2) {
                s += used;
                continue;
            }
        }
        if (len + 1 < cap)
            out[len++] = *s;
        s++;
    }
    if (cap)
        out[MIN(len, cap - 1)] = 0;
    return len;
}

void con_set_attr(uint8_t a)
{
    attr = a;
}

uint8_t con_attr(void)
{
    return attr;
}

void con_role(enum theme_role role)
{
    uint8_t a = theme_attr(role);
    attr = a;
}

void con_reset(void)
{
    attr = theme_attr(ROLE_TEXT);
}

void con_goto(int x, int y)
{
    if (!con_is_grid())
        return;
    cx = CLAMP(x, 0, cols - 1);
    cy = CLAMP(y, 0, text_rows - 1);
    wrap_pending = false;
}

void con_cursor(int *x, int *y)
{
    if (x)
        *x = wrap_pending ? cols : cx;
    if (y)
        *y = cy;
}

void con_show_cursor(bool visible)
{
    cursor_on = visible;
}

bool con_cursor_visible(void)
{
    return cursor_on;
}

void con_fill(int x, int y, int w, int h, uint8_t ch, uint8_t a)
{
    if (!con_is_grid())
        return;
    int x1 = MIN(cols, x + w), y1 = MIN(rows, y + h);
    x = MAX(0, x);
    y = MAX(0, y);
    for (int r = y; r < y1; r++)
        for (int c = x; c < x1; c++)
            *at(c, r) = (struct cell){ch, a};
    if (y < y1)
        mark(y, y1);
}

void con_clear(void)
{
    attr = theme_attr(ROLE_TEXT);
    if (!con_is_grid()) {
        if (disp)
            disp->write("\n", 1);
        return;
    }
    con_fill(0, 0, cols, text_rows, ' ', attr);
    cx = cy = 0;
    wrap_pending = false;
    con_flush();
}

void con_clear_line_from(int x, int y)
{
    con_fill(x, y, cols - x, 1, ' ', blank_attr());
}

void con_clear_below(int y)
{
    if (y < text_rows)
        con_fill(0, y, cols, text_rows - y, ' ', blank_attr());
}

void con_put_cell(int x, int y, uint8_t ch, uint8_t a)
{
    if (!con_is_grid() || x < 0 || y < 0 || x >= cols || y >= rows)
        return;
    struct cell *c = at(x, y);
    if (c->ch != ch || c->attr != a) {
        *c = (struct cell){ch, a};
        mark(y, y + 1);
    }
}

struct cell con_get_cell(int x, int y)
{
    if (!con_is_grid() || x < 0 || y < 0 || x >= cols || y >= rows)
        return (struct cell){' ', theme_attr(ROLE_TEXT)};
    return *at(x, y);
}

void con_put_text(int x, int y, const char *s, uint8_t a)
{
    for (; *s && x < cols; s++, x++)
        con_put_cell(x, y, (uint8_t)*s, a);
}

void con_flush(void)
{
    if (!con_is_grid())
        return;
    present_dirty();
    if (disp->cursor)
        disp->cursor(MIN(cx, cols - 1), cy, cursor_on);
}

void con_redraw(void)
{
    if (!con_is_grid())
        return;
    if (statusbar)
        con_update_status();
    mark(0, rows);
    con_flush();
}

void con_apply_palette(void)
{
    if (disp && disp->palette)
        disp->palette(theme_current()->palette);
    if (con_is_grid())
        con_redraw();
}

void con_set_cursor_shape(enum cursor_shape s)
{
    shape = s;
    if (disp && disp->cursor_shape)
        disp->cursor_shape(s);
}

enum cursor_shape con_cursor_shape(void)
{
    return shape;
}

void con_set_statusbar(bool on)
{
    if (!con_is_grid() || on == statusbar || rows < 4)
        return;
    statusbar = on;
    if (on && cy >= rows - 1) {
        memmove(cells, cells + cols, (size_t)cols * (size_t)(rows - 1) * sizeof(struct cell));
        cy = rows - 2;
    }
    layout();
    if (!on)
        con_fill(0, rows - 1, cols, 1, ' ', blank_attr());
    con_redraw();
}

bool con_statusbar(void)
{
    return statusbar;
}

void con_set_status_renderer(status_render_fn fn)
{
    status_fn = fn;
}

void con_status_put(int x, const char *s, uint8_t a)
{
    if (!statusbar)
        return;
    con_put_text(x, rows - 1, s, a);
}

void con_update_status(void)
{
    if (!statusbar || !con_is_grid())
        return;
    con_fill(0, rows - 1, cols, 1, ' ', theme_attr(ROLE_STATUS));
    if (status_fn)
        status_fn(cols);
}
