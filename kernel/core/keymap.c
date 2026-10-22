#include <synth/error.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

/* Keyboard layouts on top of the US key codes the drivers deliver: a typed character is looked up and comes back
   as a text key with its code point. Win+Space moves to the next enabled layout; shortcuts are never translated. */

struct pair {
    char from;
    uint16_t to;
};

struct layout {
    const char *name, *label, *title;
    const struct pair *pairs;
    int count;
};

static const struct pair ru[] = {
    {'q', 0x439}, {'w', 0x446}, {'e', 0x443}, {'r', 0x43a}, {'t', 0x435}, {'y', 0x43d}, {'u', 0x433}, {'i', 0x448},
    {'o', 0x449}, {'p', 0x437}, {'[', 0x445}, {']', 0x44a}, {'a', 0x444}, {'s', 0x44b}, {'d', 0x432}, {'f', 0x430},
    {'g', 0x43f}, {'h', 0x440}, {'j', 0x43e}, {'k', 0x43b}, {'l', 0x434}, {';', 0x436}, {'\'', 0x44d}, {'z', 0x44f},
    {'x', 0x447}, {'c', 0x441}, {'v', 0x43c}, {'b', 0x438}, {'n', 0x442}, {'m', 0x44c}, {',', 0x431}, {'.', 0x44e},
    {'`', 0x451}, {'Q', 0x419}, {'W', 0x426}, {'E', 0x423}, {'R', 0x41a}, {'T', 0x415}, {'Y', 0x41d}, {'U', 0x413},
    {'I', 0x428}, {'O', 0x429}, {'P', 0x417}, {'{', 0x425}, {'}', 0x42a}, {'A', 0x424}, {'S', 0x42b}, {'D', 0x412},
    {'F', 0x410}, {'G', 0x41f}, {'H', 0x420}, {'J', 0x41e}, {'K', 0x41b}, {'L', 0x414}, {':', 0x416}, {'"', 0x42d},
    {'Z', 0x42f}, {'X', 0x427}, {'C', 0x421}, {'V', 0x41c}, {'B', 0x418}, {'N', 0x422}, {'M', 0x42c}, {'<', 0x411},
    {'>', 0x42e}, {'~', 0x401}, {'/', '.'},   {'?', ','},   {'@', '"'},   {'$', ';'},   {'^', ':'},   {'&', '?'},
    {'|', '/'},
};

static const struct pair de[] = {
    {'y', 'z'},  {'z', 'y'},  {'Y', 'Z'},  {'Z', 'Y'},  {';', 0xf6}, {':', 0xd6}, {'\'', 0xe4}, {'"', 0xc4},
    {'[', 0xfc}, {'{', 0xdc}, {'-', 0xdf}, {']', '+'},  {'}', '*'},  {'/', '-'},  {'?', '_'},   {'@', '"'},
    {'#', 0xa7}, {'^', '&'},  {'&', '/'},  {'*', '('},  {'(', ')'},  {')', '='},  {'_', '?'},   {'<', ';'},
    {'>', ':'},  {'`', '^'},  {'~', 0xb0}, {'\\', '#'}, {'|', '\''},
};

static const struct pair fr[] = {
    {'q', 'a'},  {'a', 'q'},  {'w', 'z'},  {'z', 'w'},  {'Q', 'A'},  {'A', 'Q'},  {'W', 'Z'},  {'Z', 'W'},
    {';', 'm'},  {':', 'M'},  {'m', ','},  {'M', '?'},  {',', ';'},  {'<', '.'},  {'.', ':'},  {'>', '/'},
    {'/', '!'},  {'?', 0xa7}, {'1', '&'},  {'2', 0xe9}, {'3', '"'},  {'4', '\''}, {'5', '('},  {'6', '-'},
    {'7', 0xe8}, {'8', '_'},  {'9', 0xe7}, {'0', 0xe0}, {'-', ')'},  {'!', '1'},  {'@', '2'},  {'#', '3'},
    {'$', '4'},  {'%', '5'},  {'^', '6'},  {'&', '7'},  {'*', '8'},  {'(', '9'},  {')', '0'},  {'_', 0xb0},
    {']', '$'},  {'}', 0xa3}, {'\'', 0xf9}, {'"', '%'}, {'\\', '*'}, {'|', 0xb5}, {'`', 0xb2},
};

static const struct pair es[] = {
    {';', 0xf1}, {':', 0xd1}, {'-', '\''}, {'_', '?'},  {'=', 0xa1}, {'+', 0xbf}, {'/', '-'},  {'?', '_'},
    {'`', 0xba}, {'~', 0xaa}, {'@', '"'},  {'#', 0xb7}, {'^', '&'},  {'&', '/'},  {'*', '('},  {'(', ')'},
    {')', '='},  {'<', ';'},  {'>', ':'},  {']', '+'},  {'}', '*'},  {'\\', 0xe7}, {'|', 0xc7},
};

static const struct layout layouts[] = {
    {"us", "EN", "English (US)", NULL, 0},
    {"ru", "RU", "Russian", ru, (int)ARRAY_SIZE(ru)},
    {"de", "DE", "German", de, (int)ARRAY_SIZE(de)},
    {"fr", "FR", "French", fr, (int)ARRAY_SIZE(fr)},
    {"es", "ES", "Spanish", es, (int)ARRAY_SIZE(es)},
};

static uint32_t enabled = 1;
static int active;

int keymap_count(void)
{
    return (int)ARRAY_SIZE(layouts);
}

const char *keymap_layout_name(int index, const char **title)
{
    if (index < 0 || index >= keymap_count())
        return NULL;
    if (title)
        *title = layouts[index].title;
    return layouts[index].name;
}

bool keymap_enabled(int index)
{
    return index >= 0 && index < keymap_count() && (enabled & 1u << index);
}

bool keymap_multi(void)
{
    return (enabled & (enabled - 1)) != 0;
}

const char *keymap_label(void)
{
    return layouts[active].label;
}

void keymap_next(void)
{
    for (int i = 1; i <= keymap_count(); i++) {
        int next = (active + i) % keymap_count();
        if (enabled & 1u << next) {
            active = next;
            return;
        }
    }
}

int keymap_translate(int key)
{
    if (KEY_IS_TEXT(key) || (key & (KEY_MOD_CTRL | KEY_MOD_ALT | KEY_MOD_SUPER)) || !active)
        return key;
    int base = KEY_BASE(key);
    if (base < 0x20 || base > 0x7e)
        return key;
    const struct layout *l = &layouts[active];
    for (int i = 0; i < l->count; i++)
        if (l->pairs[i].from == base)
            return KEY_TEXT_CHAR(l->pairs[i].to, base);
    return key;
}

int keymap_plain(int key)
{
    return KEY_IS_TEXT(key) ? KEY_TEXT_PHYS(key) : key;
}

int keymap_set_enabled(const char *list)
{
    uint32_t mask = 0;
    const char *p = list;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != ',')
            n++;
        int found = -1;
        for (int i = 0; i < keymap_count(); i++)
            if (strlen(layouts[i].name) == n && !strncmp(layouts[i].name, p, n))
                found = i;
        if (found < 0)
            return E_INVAL;
        mask |= 1u << found;
        p += n + (p[n] == ',');
    }
    enabled = mask | 1;
    if (!(enabled & 1u << active))
        active = 0;
    return 0;
}

static void get_keyboard(char *buf, size_t cap)
{
    buf[0] = 0;
    for (int i = 0; i < keymap_count(); i++)
        if (enabled & 1u << i) {
            if (buf[0])
                str_append(buf, ",", cap);
            str_append(buf, layouts[i].name, cap);
        }
}

static int set_keyboard(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return keymap_set_enabled(value);
}

static const struct setting s_keyboard = {"keyboard", "us|us,ru|us,de|us,fr|us,es|...",
                                          "keyboard layouts; Win+Space switches between them", get_keyboard,
                                          set_keyboard};
SETTING(s_keyboard);
