#include <synth/keys.h>

#include "io.h"
#include "usb.h"

#define BUFFER_SIZE 128

static volatile int buffer[BUFFER_SIZE];
static volatile unsigned head, tail;
static bool shift_l, shift_r, ctrl, alt, caps, extended, super, super_used;

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
    return (ctrl ? KEY_MOD_CTRL : 0) | (alt ? KEY_MOD_ALT : 0) | (shift_l || shift_r ? KEY_MOD_SHIFT : 0) |
           (super ? KEY_MOD_SUPER : 0);
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

    if (ext && (code == 0x5b || code == 0x5c)) {
        if (!release && !super) {
            super = true;
            super_used = false;
        } else if (release) {
            super = false;
            if (!super_used)
                push(KEY_SUPER);
        }
        return;
    }
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
        else if (ctrl && (key == KEY_BACKSPACE || key == KEY_TAB || key == ' ' || key == ',' || key == '.'))
            key |= KEY_MOD_CTRL;
        if (alt)
            key |= KEY_MOD_ALT;
        if (super)
            key |= KEY_MOD_SUPER;
        if (key == KEY_TAB && shifted)
            key |= KEY_MOD_SHIFT;
    }
    if (key != KEY_NONE) {
        super_used |= super;
        push(key);
    }
}

void keyboard_scancode(uint8_t code)
{
    uint32_t flags = cpu_irq_save();
    handle(code);
    cpu_irq_restore(flags);
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

static bool ps2_send(uint8_t v)
{
    for (int i = 0; i < 20000 && (inb(0x64) & 2); i++)
        ;
    outb(0x60, v);
    for (int i = 0; i < 20000; i++)
        if (inb(0x64) & 1)
            return inb(0x60) == 0xfa;
    return false;
}

/* USB keyboards repeat in software; a PS/2 keyboard gets the nearest typematic setting
   (delay 250..1000 ms, period (8 + A) * 2^B * 4.17 ms). */
void hal_key_repeat(int delay_ms, int rate)
{
    int period = 1000 / MAX(rate, 1);
    usbhid_repeat(delay_ms, period);
    int best = 0, best_err = 1 << 30;
    for (int code = 0; code < 32; code++) {
        int p = (8 + (code & 7)) * (1 << (code >> 3)) * 417 / 100, err = p > period ? p - period : period - p;
        if (err < best_err) {
            best = code;
            best_err = err;
        }
    }
    uint8_t typematic = (uint8_t)(CLAMP((delay_ms + 125) / 250 - 1, 0, 3) << 5 | best);
    uint32_t flags = cpu_irq_save();
    if (ps2_send(0xf3))
        ps2_send(typematic);
    cpu_irq_restore(flags);
}

bool keyboard_pending(void)
{
    return head != tail;
}

int keyboard_read(void)
{
    if (head == tail)
        return KEY_NONE;
    return buffer[head++ % BUFFER_SIZE];
}
