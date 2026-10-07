#ifndef SYNTH_KEYS_H
#define SYNTH_KEYS_H

#define KEY_CTRL(c) ((c) & 0x1f)

enum key_code {
    KEY_NONE = 0,
    KEY_BACKSPACE = 0x08,
    KEY_TAB = 0x09,
    KEY_ENTER = 0x0a,
    KEY_ESC = 0x1b,
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_RESIZE,
    KEY_EOF,
    KEY_SUPER,
};

#define KEY_MOD_SHIFT 0x1000
#define KEY_MOD_CTRL 0x2000
#define KEY_MOD_ALT 0x4000
#define KEY_MOD_MASK 0x7000
#define KEY_BASE(k) ((k) & 0x0fff)

const char *key_name(int key, char *buf, int cap);

#endif
