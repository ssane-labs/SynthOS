#include <synth/keys.h>

#include "io.h"

#define BUFFER_SIZE 128

static volatile int buffer[BUFFER_SIZE];
static volatile unsigned head, tail;
static bool shift_l, shift_r, ctrl, alt, caps, extended;

static const char normal_map[0x3a] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

static const char shift_map[0x3a] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static void push(int key)
{
    if (tail - head < BUFFER_SIZE)
        buffer[tail++ % BUFFER_SIZE] = key;
}

static int modifiers(void)
{
    return (ctrl ? KEY_MOD_CTRL : 0) | (alt ? KEY_MOD_ALT : 0) | (shift_l || shift_r ? KEY_MOD_SHIFT : 0);
}

static int extended_key(uint8_t code)
{
    switch (code) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4f: return KEY_END;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x1c: return KEY_ENTER;
    case 0x35: return '/';
    case 0x5b:
    case 0x5c: return KEY_SUPER;
    default: return KEY_NONE;
    }
}

static int keypad_key(uint8_t code)
{
    switch (code) {
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x4f: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x4a: return '-';
    case 0x4e: return '+';
    default: return KEY_NONE;
    }
}

static void handle(uint8_t sc)
{
    if (sc == 0xe0) {
        extended = true;
        return;
    }
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7f;
    bool ext = extended;
    extended = false;

    switch (code) {
    case 0x2a: shift_l = !release; return;
    case 0x36: shift_r = !release; return;
    case 0x1d: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3a:
        if (!release)
            caps = !caps;
        return;
    }
    if (release)
        return;

    int key = KEY_NONE;
    int mods = modifiers();
    if (ext) {
        key = extended_key(code);
        if (key >= KEY_UP)
            key |= mods;
    } else if (code >= 0x3b && code <= 0x44) {
        key = (KEY_F1 + (code - 0x3b)) | mods;
    } else if (code == 0x57 || code == 0x58) {
        key = (KEY_F11 + (code - 0x57)) | mods;
    } else if (code >= 0x47 && code <= 0x53) {
        key = keypad_key(code);
        if (key >= KEY_UP)
            key |= mods;
    } else if (code < sizeof(normal_map)) {
        bool shifted = shift_l || shift_r;
        char c = shifted ? shift_map[code] : normal_map[code];
        if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c = shifted ? (char)(c + 32) : (char)(c - 32);
        if (!c)
            return;
        key = (uint8_t)c;
        if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '[' || c == ']' || c == '\\'))
            key = KEY_CTRL(c);
        else if (ctrl && (key == KEY_BACKSPACE || key == KEY_TAB))
            key |= KEY_MOD_CTRL;
        if (alt)
            key |= KEY_MOD_ALT;
        if (key == KEY_TAB && shifted)
            key |= KEY_MOD_SHIFT;
    }
    if (key != KEY_NONE)
        push(key);
}

void i8042_drain(void)
{
    uint8_t status;
    while ((status = inb(0x64)) & 1) {
        uint8_t data = inb(0x60);
        if (status & 0x20)
            mouse_byte(data);
        else
            handle(data);
    }
}

static void keyboard_irq(struct regs *r)
{
    UNUSED(r);
    i8042_drain();
}

void keyboard_init(void)
{
    while (inb(0x64) & 1)
        inb(0x60);
    irq_install(1, keyboard_irq);
}

int keyboard_read(void)
{
    if (head == tail)
        return KEY_NONE;
    return buffer[head++ % BUFFER_SIZE];
}
