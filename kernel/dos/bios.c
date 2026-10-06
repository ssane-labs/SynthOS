#include <synth/console.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/string.h>

#include "dos.h"

#define VRAM(d) ((d)->mem + ((uint32_t)DOS_VIDEO_SEG << 4))
#define CELLS (DOS_COLS * DOS_ROWS)

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
    0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static const char scan_chars[] = "\0\0" "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char scan_shift[] = "\0\0" "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static uint8_t scan_of(int ch)
{
    for (int i = 2; i < (int)sizeof(scan_chars) - 1; i++)
        if (scan_chars[i] == ch || scan_shift[i] == ch)
            return (uint8_t)i;
    return ch == ' ' ? 0x39 : 0;
}

uint16_t dos_key_code(int key)
{
    int base = KEY_BASE(key);
    bool ctrl = key & KEY_MOD_CTRL, alt = key & KEY_MOD_ALT, shift = key & KEY_MOD_SHIFT;
    switch (base) {
    case KEY_ENTER: return 0x1c0d;
    case KEY_BACKSPACE: return 0x0e08;
    case KEY_TAB: return shift ? 0x0f00 : 0x0f09;
    case KEY_ESC: return 0x011b;
    case KEY_UP: return ctrl ? 0x8d00 : 0x4800;
    case KEY_DOWN: return ctrl ? 0x9100 : 0x5000;
    case KEY_LEFT: return ctrl ? 0x7300 : 0x4b00;
    case KEY_RIGHT: return ctrl ? 0x7400 : 0x4d00;
    case KEY_HOME: return ctrl ? 0x7700 : 0x4700;
    case KEY_END: return ctrl ? 0x7500 : 0x4f00;
    case KEY_PGUP: return ctrl ? 0x8400 : 0x4900;
    case KEY_PGDN: return ctrl ? 0x7600 : 0x5100;
    case KEY_INSERT: return 0x5200;
    case KEY_DELETE: return 0x5300;
    case KEY_F11: return 0x8500;
    case KEY_F12: return 0x8600;
    default: break;
    }
    if (base >= KEY_F1 && base <= KEY_F10) {
        int n = base - KEY_F1;
        if (alt)
            return (uint16_t)((0x68 + n) << 8);
        if (ctrl)
            return (uint16_t)((0x5e + n) << 8);
        if (shift)
            return (uint16_t)((0x54 + n) << 8);
        return (uint16_t)((0x3b + n) << 8);
    }
    if (base >= 1 && base <= 26)
        return (uint16_t)(scan_of('a' + base - 1) << 8 | base);
    if (base < 0x100) {
        uint8_t scan = scan_of(base);
        if (alt)
            return (uint16_t)(scan << 8);
        return (uint16_t)(scan << 8 | base);
    }
    return 0;
}

static void fill_keys(struct dos *d)
{
    while (d->ktail - d->khead < ARRAY_SIZE(d->keys) && input_poll()) {
        int key = input_getkey(0);
        if (key == KEY_NONE || key == KEY_RESIZE)
            continue;
        uint16_t code = dos_key_code(key);
        if (code) {
            d->keys[d->ktail++ % ARRAY_SIZE(d->keys)] = code;
            d->last_scancode = (uint8_t)(code >> 8);
        }
    }
}

void dos_flush_keys(struct dos *d)
{
    fill_keys(d);
    d->khead = d->ktail;
    d->pending_scan = -1;
}

int dos_read_key(struct dos *d, bool wait, bool peek)
{
    for (;;) {
        fill_keys(d);
        if (d->khead != d->ktail) {
            uint16_t code = d->keys[d->khead % ARRAY_SIZE(d->keys)];
            if (!peek)
                d->khead++;
            return code;
        }
        if (!wait || !d->running || input_break_pending())
            return -1;
        dos_video_sync(d, false);
        int key = input_getkey(20);
        if (key != KEY_NONE && key != KEY_RESIZE) {
            uint16_t code = dos_key_code(key);
            if (code && d->ktail - d->khead < ARRAY_SIZE(d->keys))
                d->keys[d->ktail++ % ARRAY_SIZE(d->keys)] = code;
        }
        uint32_t t = dos_ticks(d);
        if (t != d->delivered)
            d->delivered = t;
    }
}

static uint8_t *cell(struct dos *d, int x, int y)
{
    return VRAM(d) + (y * DOS_COLS + x) * 2;
}

static void cursor_get(struct dos *d, int *x, int *y)
{
    *x = d->mem[0x450];
    *y = d->mem[0x451];
}

static void cursor_set(struct dos *d, int x, int y)
{
    d->mem[0x450] = (uint8_t)CLAMP(x, 0, DOS_COLS - 1);
    d->mem[0x451] = (uint8_t)CLAMP(y, 0, DOS_ROWS - 1);
}

static void blank(struct dos *d, uint8_t attr)
{
    for (int i = 0; i < CELLS; i++) {
        VRAM(d)[i * 2] = ' ';
        VRAM(d)[i * 2 + 1] = attr;
    }
}

void dos_video_init(struct dos *d)
{
    blank(d, 0x07);
    memcpy(d->baseline, VRAM(d), sizeof(d->baseline));
    memcpy(d->shadow, VRAM(d), sizeof(d->shadow));
    d->video_mode = 3;
    d->cpu.video_touched = false;
}

static void scroll(struct dos *d, int lines, uint8_t attr, int x0, int y0, int x1, int y1, bool up)
{
    x1 = MIN(x1, DOS_COLS - 1);
    y1 = MIN(y1, DOS_ROWS - 1);
    if (x0 > x1 || y0 > y1)
        return;
    int height = y1 - y0 + 1;
    if (lines <= 0 || lines > height)
        lines = height;
    for (int i = 0; i < height; i++) {
        int y = up ? y0 + i : y1 - i;
        int src = up ? y + lines : y - lines;
        for (int x = x0; x <= x1; x++) {
            uint8_t *dst = cell(d, x, y);
            if (src >= y0 && src <= y1) {
                memcpy(dst, cell(d, x, src), 2);
            } else {
                dst[0] = ' ';
                dst[1] = attr;
            }
        }
    }
}

static void teletype(struct dos *d, uint8_t ch, int attr)
{
    int x, y;
    cursor_get(d, &x, &y);
    switch (ch) {
    case '\r':
        x = 0;
        break;
    case '\n':
        y++;
        break;
    case '\b':
        if (x > 0)
            x--;
        break;
    case 7:
        hal_beep(880, 60);
        break;
    default:
        cell(d, x, y)[0] = ch;
        if (attr >= 0)
            cell(d, x, y)[1] = (uint8_t)attr;
        if (++x >= DOS_COLS) {
            x = 0;
            y++;
        }
        break;
    }
    if (y >= DOS_ROWS) {
        scroll(d, 1, 0x07, 0, 0, DOS_COLS - 1, DOS_ROWS - 1, true);
        y = DOS_ROWS - 1;
    }
    cursor_set(d, x, y);
}

void dos_enter_screen(struct dos *d, bool clear)
{
    if (!con_is_grid())
        return;
    if (!d->screen) {
        d->screen = true;
        struct display *disp = hal_display();
        if (disp->palette)
            disp->palette(vga_palette);
        int cx, cy;
        con_cursor(&cx, &cy);
        if (cx >= con_cols()) {
            cx = 0;
            cy++;
        }
        d->ox = 0;
        d->oy = CLAMP(cy - (DOS_ROWS - 1), 0, MAX(0, con_rows() - DOS_ROWS));
        if (!clear) {
            for (int y = 0; y < DOS_ROWS; y++)
                for (int x = 0; x < DOS_COLS; x++) {
                    int i = (y * DOS_COLS + x) * 2;
                    if (memcmp(VRAM(d) + i, d->baseline + i, 2) != 0)
                        continue;
                    struct cell c = con_get_cell(d->ox + x, d->oy + y);
                    bool inside = x < con_cols() && d->oy + y < con_rows();
                    VRAM(d)[i] = inside ? c.ch : ' ';
                    VRAM(d)[i + 1] = inside ? c.attr : 0x07;
                }
            cursor_set(d, MIN(cx, DOS_COLS - 1), cy - d->oy);
        }
        memset(d->shadow, 0xff, sizeof(d->shadow));
        d->cpu.video_touched = false;
    }
    if (clear) {
        con_set_attr(0x07);
        con_clear();
        d->oy = 0;
        blank(d, 0x07);
        cursor_set(d, 0, 0);
        memset(d->shadow, 0xff, sizeof(d->shadow));
    }
    dos_video_sync(d, true);
}

void dos_video_sync(struct dos *d, bool force)
{
    d->last_sync = hal_ticks_ms();
    if (!d->screen) {
        if (d->cpu.video_touched) {
            d->cpu.video_touched = false;
            if (memcmp(VRAM(d), d->baseline, sizeof(d->baseline)) != 0)
                dos_enter_screen(d, false);
        }
        return;
    }
    const uint8_t *v = VRAM(d);
    bool changed = force;
    for (int i = 0; i < CELLS; i++) {
        if (!force && v[i * 2] == d->shadow[i * 2] && v[i * 2 + 1] == d->shadow[i * 2 + 1])
            continue;
        d->shadow[i * 2] = v[i * 2];
        d->shadow[i * 2 + 1] = v[i * 2 + 1];
        con_put_cell(d->ox + i % DOS_COLS, d->oy + i / DOS_COLS, v[i * 2], v[i * 2 + 1]);
        changed = true;
    }
    int x, y;
    cursor_get(d, &x, &y);
    bool visible = !(d->mem[0x461] & 0x20) && (d->mem[0x461] & 0x1f) <= (d->mem[0x460] & 0x1f);
    static int last_x = -1, last_y = -1;
    if (changed || x != last_x || y != last_y) {
        con_goto(d->ox + x, d->oy + y);
        con_show_cursor(visible);
        con_flush();
        last_x = x;
        last_y = y;
    }
}

void dos_video_finish(struct dos *d)
{
    if (!d->screen)
        return;
    dos_video_sync(d, false);
    int x, y;
    cursor_get(d, &x, &y);
    con_goto(d->ox + x, MIN(d->oy + y, con_rows() - 1));
    con_show_cursor(true);
    con_apply_palette();
    con_reset();
    con_flush();
}

void dos_putc(struct dos *d, uint8_t ch)
{
    if (d->screen) {
        teletype(d, ch, -1);
        return;
    }
    if (ch == 7) {
        hal_beep(880, 60);
        return;
    }
    char c = (char)ch;
    con_write(&c, 1);
}

void dos_puts(struct dos *d, const char *s)
{
    while (*s)
        dos_putc(d, (uint8_t)*s++);
}

void dos_out(struct dos *d, int kind, uint8_t ch)
{
    struct stream *s = kind == H_STDERR ? d->ctx->err : d->ctx->out;
    if (stream_is_console(s)) {
        dos_putc(d, ch);
    } else if (ch != '\r') {
        char c = (char)ch;
        stream_write(s, &c, 1);
    }
}

int dos_line_input(struct dos *d, char *buf, int max)
{
    int len = 0;
    for (;;) {
        int code = dos_read_key(d, true, false);
        if (code < 0)
            return len;
        uint8_t ch = (uint8_t)code;
        if (ch == '\r') {
            dos_putc(d, '\r');
            dos_putc(d, '\n');
            return len;
        }
        if (ch == 8) {
            if (len > 0) {
                len--;
                dos_puts(d, "\b \b");
            }
        } else if (ch == 0x1b) {
            while (len > 0) {
                len--;
                dos_puts(d, "\b \b");
            }
        } else if (ch >= 0x20 || ch == 0x1a) {
            if (len < max - 1) {
                buf[len++] = (char)ch;
                dos_putc(d, ch >= 0x20 ? ch : '^');
            } else {
                hal_beep(1200, 20);
            }
        }
    }
}

static uint8_t bcd(int v)
{
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

static void int10(struct dos *d)
{
    struct cpu *c = &d->cpu;
    int x, y;
    switch (AH(c)) {
    case 0x00: {
        uint8_t mode = AL(c) & 0x7f;
        d->video_mode = mode;
        d->mem[0x449] = mode;
        if (mode > 3 && mode != 7 && !d->graphics_warned) {
            d->graphics_warned = true;
            str_copy(d->message, "graphics video modes are not supported yet", sizeof(d->message));
        }
        if (!(AL(c) & 0x80))
            dos_enter_screen(d, true);
        break;
    }
    case 0x01:
        d->mem[0x460] = CL(c);
        d->mem[0x461] = CH(c);
        break;
    case 0x02:
        dos_enter_screen(d, false);
        cursor_set(d, DL(c), DH(c));
        break;
    case 0x03:
        if (d->screen) {
            cursor_get(d, &x, &y);
        } else {
            con_cursor(&x, &y);
            x = MIN(x, DOS_COLS - 1);
            y = MIN(y, DOS_ROWS - 1);
        }
        cpu_set_reg8(c, 6, (uint8_t)y);
        cpu_set_reg8(c, 2, (uint8_t)x);
        cpu_set_reg8(c, 5, d->mem[0x461]);
        cpu_set_reg8(c, 1, d->mem[0x460]);
        break;
    case 0x05:
        break;
    case 0x06:
    case 0x07:
        dos_enter_screen(d, false);
        scroll(d, AL(c), BH(c), CL(c), CH(c), DL(c), DH(c), AH(c) == 0x06);
        break;
    case 0x08:
        dos_enter_screen(d, false);
        cursor_get(d, &x, &y);
        c->r[AX] = (uint16_t)(cell(d, x, y)[1] << 8 | cell(d, x, y)[0]);
        break;
    case 0x09:
    case 0x0a: {
        dos_enter_screen(d, false);
        cursor_get(d, &x, &y);
        int pos = y * DOS_COLS + x;
        for (int i = 0; i < c->r[CX] && pos + i < CELLS; i++) {
            VRAM(d)[(pos + i) * 2] = AL(c);
            if (AH(c) == 0x09)
                VRAM(d)[(pos + i) * 2 + 1] = BL(c);
        }
        break;
    }
    case 0x0e:
        dos_putc(d, AL(c));
        break;
    case 0x0f:
        cpu_set_reg8(c, 0, d->video_mode);
        cpu_set_reg8(c, 4, DOS_COLS);
        cpu_set_reg8(c, 7, 0);
        break;
    case 0x11:
        if (AL(c) == 0x30) {
            c->r[CX] = 16;
            cpu_set_reg8(c, 2, DOS_ROWS - 1);
        }
        break;
    case 0x12:
        if (BL(c) == 0x10) {
            cpu_set_reg8(c, 7, 0);
            cpu_set_reg8(c, 3, 3);
            c->r[CX] = 0x0009;
        }
        break;
    case 0x13: {
        dos_enter_screen(d, false);
        int saved_x, saved_y;
        cursor_get(d, &saved_x, &saved_y);
        cursor_set(d, DL(c), DH(c));
        bool attrs = AL(c) & 2;
        uint16_t off = c->r[BP];
        for (int i = 0; i < c->r[CX]; i++) {
            uint8_t ch = cpu_rb(c, cpu_lin(c->s[ES], off++));
            uint8_t attr = attrs ? cpu_rb(c, cpu_lin(c->s[ES], off++)) : BL(c);
            teletype(d, ch, attr);
        }
        if (!(AL(c) & 1))
            cursor_set(d, saved_x, saved_y);
        break;
    }
    case 0x1a:
        if (AL(c) == 0) {
            cpu_set_reg8(c, 0, 0x1a);
            cpu_set_reg8(c, 3, 0x08);
            cpu_set_reg8(c, 7, 0);
        }
        break;
    default:
        break;
    }
}

static void int16(struct dos *d)
{
    struct cpu *c = &d->cpu;
    int code;
    switch (AH(c)) {
    case 0x00:
    case 0x10:
        code = dos_read_key(d, true, false);
        c->r[AX] = code < 0 ? 0 : (uint16_t)code;
        break;
    case 0x01:
    case 0x11:
        code = dos_read_key(d, false, true);
        if (code < 0) {
            dos_set_zf(d, true);
            dos_idle(d);
        } else {
            c->r[AX] = (uint16_t)code;
            dos_set_zf(d, false);
        }
        break;
    case 0x02:
    case 0x12:
        cpu_set_reg8(c, 0, 0);
        break;
    case 0x05:
        if (d->ktail - d->khead < ARRAY_SIZE(d->keys)) {
            d->keys[d->ktail++ % ARRAY_SIZE(d->keys)] = c->r[CX];
            cpu_set_reg8(c, 0, 0);
        } else {
            cpu_set_reg8(c, 0, 1);
        }
        break;
    default:
        break;
    }
}

static void int1a(struct dos *d)
{
    struct cpu *c = &d->cpu;
    struct rtc_time t;
    switch (AH(c)) {
    case 0x00: {
        uint32_t ticks = dos_ticks(d);
        c->r[CX] = (uint16_t)(ticks >> 16);
        c->r[DX] = (uint16_t)ticks;
        cpu_set_reg8(c, 0, 0);
        break;
    }
    case 0x02:
        hal_rtc(&t);
        cpu_set_reg8(c, 5, bcd(t.hour));
        cpu_set_reg8(c, 1, bcd(t.minute));
        cpu_set_reg8(c, 6, bcd(t.second));
        cpu_set_reg8(c, 2, 0);
        dos_set_cf(d, false);
        break;
    case 0x04:
        hal_rtc(&t);
        cpu_set_reg8(c, 5, bcd(t.year / 100));
        cpu_set_reg8(c, 1, bcd(t.year % 100));
        cpu_set_reg8(c, 6, bcd(t.month));
        cpu_set_reg8(c, 2, bcd(t.day));
        dos_set_cf(d, false);
        break;
    default:
        break;
    }
}

static void int15(struct dos *d)
{
    struct cpu *c = &d->cpu;
    switch (AH(c)) {
    case 0x86: {
        uint32_t us = (uint32_t)c->r[CX] << 16 | c->r[DX];
        uint64_t end = hal_ticks_ms() + us / 1000;
        while (hal_ticks_ms() < end && !input_break_pending())
            dos_idle(d);
        dos_set_cf(d, false);
        break;
    }
    case 0x88:
        c->r[AX] = 0;
        dos_set_cf(d, false);
        break;
    case 0x4f:
        dos_set_cf(d, true);
        break;
    default:
        cpu_set_reg8(c, 4, 0x86);
        dos_set_cf(d, true);
        break;
    }
}

void dos_bios(struct dos *d, uint8_t n)
{
    struct cpu *c = &d->cpu;
    switch (n) {
    case 0x10:
        int10(d);
        break;
    case 0x11:
        c->r[AX] = (uint16_t)(d->mem[0x410] | d->mem[0x411] << 8);
        break;
    case 0x12:
        c->r[AX] = 640;
        break;
    case 0x15:
        int15(d);
        break;
    case 0x16:
        int16(d);
        break;
    case 0x1a:
        int1a(d);
        break;
    case 0x28:
        dos_idle(d);
        break;
    case 0x29:
        dos_putc(d, AL(c));
        break;
    case 0x2f:
        if (AL(c) == 0)
            cpu_set_reg8(c, 0, 0);
        break;
    case 0x33:
        if (c->r[AX] == 0)
            c->r[BX] = 0;
        break;
    default:
        break;
    }
}
