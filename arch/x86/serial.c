#include <synth/codepage.h>
#include <synth/hal.h>
#include <synth/keys.h>

#include "io.h"

#define COM1 0x3f8

static bool present;
static int pending = -1;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xc7);
    outb(COM1 + 4, 0x1e);
    outb(COM1 + 0, 0xae);
    if (inb(COM1 + 0) != 0xae)
        return;
    outb(COM1 + 4, 0x0f);
    present = true;
}

bool serial_ready(void)
{
    return present && (inb(COM1 + 5) & 1);
}

static int raw_getc(uint32_t wait_ms)
{
    if (pending >= 0) {
        int c = pending;
        pending = -1;
        return c;
    }
    uint64_t end = hal_ticks_ms() + wait_ms;
    while (!serial_ready()) {
        if (hal_ticks_ms() >= end)
            return -1;
        cpu_halt();
    }
    return inb(COM1);
}

int serial_getc(void)
{
    return serial_ready() || pending >= 0 ? raw_getc(0) : -1;
}

static void put(char c)
{
    if (!present)
        return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)s[i];
        if (ch == '\n') {
            put('\r');
            put('\n');
        } else if (ch >= 0x80) {
            char utf[4];
            size_t len = utf8_encode(codepage_to_unicode(ch), utf);
            for (size_t k = 0; k < len; k++)
                put(utf[k]);
        } else if (ch >= 0x20 || ch == '\r' || ch == '\b' || ch == '\t') {
            put((char)ch);
        }
    }
}

static int decode_escape(void)
{
    int c = raw_getc(30);
    if (c < 0)
        return KEY_ESC;
    if (c != '[' && c != 'O')
        return c | KEY_MOD_ALT;
    int n = 0, f;
    while ((f = raw_getc(30)) >= '0' && f <= '9')
        n = n * 10 + (f - '0');
    switch (f) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
    case 'S': return KEY_F4;
    case '~':
        switch (n) {
        case 1: return KEY_HOME;
        case 2: return KEY_INSERT;
        case 3: return KEY_DELETE;
        case 4: return KEY_END;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        default: return KEY_NONE;
        }
    default:
        return KEY_NONE;
    }
}

int serial_key(void)
{
    int c = serial_getc();
    if (c < 0)
        return KEY_NONE;
    if (c == '\r') {
        int n = raw_getc(2);
        if (n >= 0 && n != '\n')
            pending = n;
        return KEY_ENTER;
    }
    if (c == '\n')
        return KEY_ENTER;
    if (c == 0x7f || c == 0x08)
        return KEY_BACKSPACE;
    if (c == 0x1b)
        return decode_escape();
    if (c == 0x04)
        return KEY_EOF;
    if (c >= 0x80) {
        int extra = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
        uint32_t cp = (uint32_t)(c & (0x3f >> extra));
        for (int i = 0; i < extra; i++) {
            int b = raw_getc(30);
            if (b < 0)
                return '?';
            cp = (cp << 6) | (uint32_t)(b & 0x3f);
        }
        int ch = codepage_from_unicode(cp);
        return ch < 0 ? '?' : ch;
    }
    return c;
}

static struct display serial_disp = {
    .name = "serial",
    .cols = 80,
    .rows = 25,
    .caps = 0,
    .write = serial_write,
};

struct display *serial_display(void)
{
    return &serial_disp;
}
