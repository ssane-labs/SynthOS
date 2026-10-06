#ifndef SYNTH_THEME_H
#define SYNTH_THEME_H

#include <synth/types.h>

#define ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0f)))
#define ATTR_FG(a) ((a) & 0x0f)
#define ATTR_BG(a) (((a) >> 4) & 0x0f)

enum color {
    COLOR_BLACK,
    COLOR_BLUE,
    COLOR_GREEN,
    COLOR_CYAN,
    COLOR_RED,
    COLOR_MAGENTA,
    COLOR_BROWN,
    COLOR_LIGHT_GRAY,
    COLOR_DARK_GRAY,
    COLOR_LIGHT_BLUE,
    COLOR_LIGHT_GREEN,
    COLOR_LIGHT_CYAN,
    COLOR_LIGHT_RED,
    COLOR_LIGHT_MAGENTA,
    COLOR_YELLOW,
    COLOR_WHITE,
};

enum theme_role {
    ROLE_TEXT,
    ROLE_MUTED,
    ROLE_ACCENT,
    ROLE_ACCENT2,
    ROLE_HIGHLIGHT,
    ROLE_OK,
    ROLE_WARN,
    ROLE_ERROR,
    ROLE_DIR,
    ROLE_EXEC,
    ROLE_DEVICE,
    ROLE_PROMPT_USER,
    ROLE_PROMPT_HOST,
    ROLE_PROMPT_PATH,
    ROLE_PROMPT_SYMBOL,
    ROLE_STATUS,
    ROLE_STATUS_ACCENT,
    ROLE_SELECTION,
    ROLE_SYNTAX_COMMAND,
    ROLE_SYNTAX_STRING,
    ROLE_SYNTAX_VARIABLE,
    ROLE_SYNTAX_OPERATOR,
    ROLE_SYNTAX_ERROR,
    ROLE_SUGGESTION,
    ROLE_LOGO1,
    ROLE_LOGO2,
    ROLE_LOGO3,
    ROLE_COUNT,
};

struct theme {
    char name[24];
    char description[64];
    uint32_t palette[16];
    uint8_t roles[ROLE_COUNT];
};

const struct theme *theme_current(void);
uint8_t theme_attr(enum theme_role role);
uint8_t theme_background(void);
void theme_apply(const struct theme *t);
int theme_load(const char *name);
int theme_find(const char *name, struct theme *out);
int theme_parse(const char *text, size_t len, struct theme *out, char *err, size_t errcap);
size_t theme_serialize(const struct theme *t, char *buf, size_t cap);
size_t theme_builtin_count(void);
const struct theme *theme_builtin(size_t index);
const char *theme_role_name(enum theme_role role);
int theme_role_lookup(const char *name);
const char *theme_color_name(int color);
int theme_color_lookup(const char *name);
void theme_set_role(enum theme_role role, uint8_t attr);
void theme_set_color(int index, uint32_t rgb);

#endif
