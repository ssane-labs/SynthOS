#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "../internal.h"

#define DISPLAY_H 92
#define GAP 6

struct calc {
    struct ui_state st;
    char entry[32];
    double acc;
    char op;
    bool fresh;
    bool error;
    char history[64];
};

static void format(double v, char *out, size_t cap)
{
    if (v != v || v > 1e300 || v < -1e300) {
        str_copy(out, "Error", cap);
        return;
    }
    char buf[48];
    size_t n = 0;
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    if (v >= 1e15 || (v > 0 && v < 1e-6)) {
        int e = 0;
        while (v >= 10) {
            v /= 10;
            e++;
        }
        while (v < 1) {
            v *= 10;
            e--;
        }
        long long digits = (long long)(v * 1e9 + 0.5);
        buf[n++] = (char)('0' + digits / 1000000000);
        buf[n++] = '.';
        long long rest = digits % 1000000000;
        char tmp[16];
        for (int i = 8; i >= 0; i--) {
            tmp[i] = (char)('0' + rest % 10);
            rest /= 10;
        }
        int keep = 9;
        while (keep > 1 && tmp[keep - 1] == '0')
            keep--;
        memcpy(buf + n, tmp, (size_t)keep);
        n += (size_t)keep;
        buf[n++] = 'e';
        if (e < 0) {
            buf[n++] = '-';
            e = -e;
        }
        char ebuf[8];
        int en = 0;
        do
            ebuf[en++] = (char)('0' + e % 10);
        while ((e /= 10));
        while (en)
            buf[n++] = ebuf[--en];
        buf[n] = 0;
        str_copy(out, buf, cap);
        return;
    }
    long long whole = (long long)v;
    double frac = v - (double)whole;
    int int_digits = 1;
    for (long long t = whole; t >= 10; t /= 10)
        int_digits++;
    int decimals = MAX(0, 12 - int_digits);
    long long scale = 1;
    for (int i = 0; i < decimals; i++)
        scale *= 10;
    long long f = (long long)(frac * (double)scale + 0.5);
    if (f >= scale) {
        whole++;
        f -= scale;
    }
    char ibuf[24];
    int in = 0;
    do
        ibuf[in++] = (char)('0' + whole % 10);
    while ((whole /= 10));
    while (in)
        buf[n++] = ibuf[--in];
    if (f) {
        char fbuf[16];
        for (int i = decimals - 1; i >= 0; i--) {
            fbuf[i] = (char)('0' + f % 10);
            f /= 10;
        }
        int keep = decimals;
        while (keep && fbuf[keep - 1] == '0')
            keep--;
        buf[n++] = '.';
        memcpy(buf + n, fbuf, (size_t)keep);
        n += (size_t)keep;
    }
    buf[n] = 0;
    str_copy(out, buf, cap);
}

static double parse(const char *s)
{
    double v = 0, scale = 1;
    bool neg = *s == '-', point = false;
    if (neg)
        s++;
    for (; *s; s++) {
        if (*s == '.') {
            point = true;
            continue;
        }
        if (*s < '0' || *s > '9')
            break;
        if (point) {
            scale /= 10;
            v += (*s - '0') * scale;
        } else {
            v = v * 10 + (*s - '0');
        }
    }
    return neg ? -v : v;
}

static double apply(double a, double b, char op, bool *error)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0) {
            *error = true;
            return 0;
        }
        return a / b;
    default: return b;
    }
}

static void clear(struct calc *c)
{
    str_copy(c->entry, "0", sizeof(c->entry));
    c->acc = 0;
    c->op = 0;
    c->fresh = true;
    c->error = false;
    c->history[0] = 0;
}

static void digit(struct calc *c, char d)
{
    if (c->error)
        clear(c);
    if (c->fresh) {
        str_copy(c->entry, d == '.' ? "0." : "0", sizeof(c->entry));
        c->fresh = false;
        if (d == '.')
            return;
    }
    if (d == '.' && strchr(c->entry, '.'))
        return;
    size_t n = strlen(c->entry);
    if (n >= 16)
        return;
    if (!strcmp(c->entry, "0") && d != '.')
        c->entry[0] = d;
    else {
        c->entry[n] = d;
        c->entry[n + 1] = 0;
    }
}

static const char *op_symbol(char op)
{
    return op == '*' ? "×" : op == '/' ? "÷" : op == '-' ? "−" : "+";
}

static void operator(struct calc *c, char op)
{
    if (c->error)
        return;
    double value = parse(c->entry);
    if (c->op && !c->fresh)
        c->acc = apply(c->acc, value, c->op, &c->error);
    else if (!c->op)
        c->acc = value;
    c->op = op;
    c->fresh = true;
    format(c->acc, c->entry, sizeof(c->entry));
    char a[32];
    format(c->acc, a, sizeof(a));
    str_copy(c->history, a, sizeof(c->history));
    str_append(c->history, " ", sizeof(c->history));
    str_append(c->history, op_symbol(op), sizeof(c->history));
    if (c->error)
        str_copy(c->entry, "Cannot divide by zero", sizeof(c->entry));
}

static void equals(struct calc *c)
{
    if (c->error || !c->op)
        return;
    double value = parse(c->entry);
    char a[32], b[32];
    format(c->acc, a, sizeof(a));
    format(value, b, sizeof(b));
    double result = apply(c->acc, value, c->op, &c->error);
    str_copy(c->history, a, sizeof(c->history));
    str_append(c->history, " ", sizeof(c->history));
    str_append(c->history, op_symbol(c->op), sizeof(c->history));
    str_append(c->history, " ", sizeof(c->history));
    str_append(c->history, b, sizeof(c->history));
    str_append(c->history, " =", sizeof(c->history));
    c->op = 0;
    c->fresh = true;
    if (c->error)
        str_copy(c->entry, "Cannot divide by zero", sizeof(c->entry));
    else
        format(result, c->entry, sizeof(c->entry));
}

static void unary(struct calc *c, char kind)
{
    if (c->error)
        return;
    double v = parse(c->entry);
    if (kind == 'n')
        v = -v;
    else if (kind == '%')
        v = c->op ? c->acc * v / 100 : v / 100;
    else if (kind == 'r') {
        if (v < 0) {
            c->error = true;
            str_copy(c->entry, "Invalid input", sizeof(c->entry));
            return;
        }
        double x = v > 1 ? v / 2 : 1;
        for (int i = 0; i < 60; i++)
            x = (x + v / x) / 2;
        v = v ? x : 0;
    } else if (kind == 'q') {
        v = v * v;
    } else if (kind == 'i') {
        if (v == 0) {
            c->error = true;
            str_copy(c->entry, "Cannot divide by zero", sizeof(c->entry));
            return;
        }
        v = 1 / v;
    }
    format(v, c->entry, sizeof(c->entry));
    c->fresh = kind != 'n';
}

static void backspace(struct calc *c)
{
    if (c->fresh || c->error)
        return;
    size_t n = strlen(c->entry);
    if (n <= 1 || (n == 2 && c->entry[0] == '-'))
        str_copy(c->entry, "0", sizeof(c->entry));
    else
        c->entry[n - 1] = 0;
}

static const char *const labels[6][4] = {
    {"%", "CE", "C", "⌫"},  {"1/x", "x²", "√x", "÷"}, {"7", "8", "9", "×"},
    {"4", "5", "6", "−"},    {"1", "2", "3", "+"},    {"±", "0", ".", "="},
};

static void press(struct calc *c, int row, int col)
{
    const char *l = labels[row][col];
    if (l[0] >= '0' && l[0] <= '9' && !l[1])
        digit(c, l[0]);
    else if (!strcmp(l, "."))
        digit(c, '.');
    else if (!strcmp(l, "+"))
        operator(c, '+');
    else if (!strcmp(l, "−"))
        operator(c, '-');
    else if (!strcmp(l, "×"))
        operator(c, '*');
    else if (!strcmp(l, "÷"))
        operator(c, '/');
    else if (!strcmp(l, "="))
        equals(c);
    else if (!strcmp(l, "C"))
        clear(c);
    else if (!strcmp(l, "CE")) {
        str_copy(c->entry, "0", sizeof(c->entry));
        c->error = false;
    } else if (!strcmp(l, "⌫"))
        backspace(c);
    else if (!strcmp(l, "±"))
        unary(c, 'n');
    else if (!strcmp(l, "%"))
        unary(c, '%');
    else if (!strcmp(l, "√x"))
        unary(c, 'r');
    else if (!strcmp(l, "x²"))
        unary(c, 'q');
    else if (!strcmp(l, "1/x"))
        unary(c, 'i');
}

static void frame(struct ui *ui, void *data)
{
    struct calc *c = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    ui_panel(ui, (struct rect){0, 0, W, H}, style.window);
    ui_label(ui, (struct rect){12, 8, W - 24, 22}, c->history, &font_sans, style.muted, ALIGN_RIGHT);
    if (ui->c) {
        const struct ui_font *f = gfx_text_width(&font_title, c->entry) > W - 24 ? &font_bold : &font_title;
        gfx_text_box(ui->c, (struct rect){12, 32, W - 24, 48}, f, c->entry, style.text, ALIGN_RIGHT);
    }
    int bw = (W - GAP * 5) / 4, bh = (H - DISPLAY_H - GAP * 7) / 6;
    for (int r = 0; r < 6; r++)
        for (int col = 0; col < 4; col++) {
            struct rect b = {GAP + col * (bw + GAP), DISPLAY_H + GAP + r * (bh + GAP), bw, bh};
            if (ui_button(ui, b, labels[r][col]))
                press(c, r, col);
            if (ui->c && col == 3 && r == 5)
                gfx_round_frame(ui->c, b, 6, style.accent);
        }
    int k = ui_key(ui);
    if (k == KEY_NONE)
        return;
    int base = KEY_BASE(k);
    if ((k >= '0' && k <= '9') || k == '.' || k == ',')
        digit(c, k == ',' ? '.' : (char)k);
    else if (k == '+' || k == '-' || k == '*' || k == '/')
        operator(c, (char)k);
    else if (k == '=' || base == KEY_ENTER || k == '\r')
        equals(c);
    else if (k == KEY_BACKSPACE)
        backspace(c);
    else if (base == KEY_ESC)
        clear(c);
    else if (k == '%')
        unary(c, '%');
    else if (k == KEY_CTRL('c'))
        gui_set_clipboard(c->entry, strlen(c->entry));
    else if (k == KEY_CTRL('v')) {
        double v = parse(gui_clipboard());
        format(v, c->entry, sizeof(c->entry));
        c->fresh = false;
    }
}

static void paint(struct window *w, struct canvas *cv)
{
    struct calc *c = w->data;
    ui_run(w, &c->st, cv, NULL, frame, c);
}

static void event(struct window *w, struct gui_event *e)
{
    struct calc *c = w->data;
    ui_run(w, &c->st, NULL, e, frame, c);
}

static void destroy(struct window *w)
{
    kfree(w->data);
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    struct calc *c = kzalloc(sizeof(*c));
    if (!c)
        return;
    clear(c);
    struct window *w = wm_open("Calculator", ICON_CALC, 320, 440, &ops, c, WIN_RESIZABLE);
    if (!w) {
        kfree(c);
        return;
    }
    w->min_w = 260;
    w->min_h = 360;
}

static const struct gui_app calc_app = {"Calculator", "Arithmetic with the keyboard or mouse", ICON_CALC, launch,
                                        NULL};
GUI_APP(calc_app);
