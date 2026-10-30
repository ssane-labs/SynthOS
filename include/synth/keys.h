#ifndef SYNTH_KEYS_H
#define SYNTH_KEYS_H

#include <synth/types.h>

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
    KEY_PRINT,
};

#define KEY_MOD_SHIFT 0x1000
#define KEY_MOD_CTRL 0x2000
#define KEY_MOD_ALT 0x4000
#define KEY_MOD_SUPER 0x8000
#define KEY_MOD_MASK 0xf000
#define KEY_COMMAND(n) (0x10800 + (n))
#define KEY_BASE(k) ((k) & 0x0fff)

/* A character from a keyboard layout: the code point (below U+1000) and the US key that was pressed. */
#define KEY_TEXT 0x1000000
#define KEY_TEXT_CHAR(cp, phys) (KEY_TEXT | (phys) << 16 | (cp))
#define KEY_IS_TEXT(k) (((k) & KEY_TEXT) != 0)
#define KEY_TEXT_CP(k) ((k) & 0xfff)
#define KEY_TEXT_PHYS(k) (((k) >> 16) & 0x7f)

int keymap_translate(int key);
int keymap_plain(int key);
void keymap_next(void);
bool keymap_multi(void);
const char *keymap_label(void);
int keymap_count(void);
const char *keymap_layout_name(int index, const char **title);
bool keymap_enabled(int index);
int keymap_set_enabled(const char *list);

const char *key_name(int key, char *buf, int cap);

#endif
