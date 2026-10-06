#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/theme.h>
#include <synth/vfs.h>

#define DEFAULT_ROLES                                                                   \
    [ROLE_TEXT] = ATTR(7, 0), [ROLE_MUTED] = ATTR(8, 0), [ROLE_ACCENT] = ATTR(13, 0),   \
    [ROLE_ACCENT2] = ATTR(11, 0), [ROLE_HIGHLIGHT] = ATTR(15, 0), [ROLE_OK] = ATTR(10, 0), \
    [ROLE_WARN] = ATTR(14, 0), [ROLE_ERROR] = ATTR(12, 0), [ROLE_DIR] = ATTR(9, 0),     \
    [ROLE_EXEC] = ATTR(10, 0), [ROLE_DEVICE] = ATTR(14, 0),                             \
    [ROLE_PROMPT_USER] = ATTR(13, 0), [ROLE_PROMPT_HOST] = ATTR(5, 0),                  \
    [ROLE_PROMPT_PATH] = ATTR(11, 0), [ROLE_PROMPT_SYMBOL] = ATTR(13, 0),               \
    [ROLE_STATUS] = ATTR(7, 1), [ROLE_STATUS_ACCENT] = ATTR(0, 13),                     \
    [ROLE_SELECTION] = ATTR(0, 11), [ROLE_SYNTAX_COMMAND] = ATTR(10, 0),                \
    [ROLE_SYNTAX_STRING] = ATTR(14, 0), [ROLE_SYNTAX_VARIABLE] = ATTR(11, 0),           \
    [ROLE_SYNTAX_OPERATOR] = ATTR(13, 0), [ROLE_SYNTAX_ERROR] = ATTR(12, 0),            \
    [ROLE_SUGGESTION] = ATTR(8, 0), [ROLE_LOGO1] = ATTR(13, 0), [ROLE_LOGO2] = ATTR(12, 0), \
    [ROLE_LOGO3] = ATTR(11, 0)

static const struct theme builtin_themes[] = {
    {
        .name = "synthwave",
        .description = "Neon sunset over a retro grid (default)",
        .palette = {0x140c26, 0x2d1b69, 0x1b998b, 0x2de2e6, 0xc71e5a, 0x7b2cbf, 0xf6a21e, 0xd7cdf2,
                    0x5f4d85, 0x6d7cff, 0x3df5a6, 0x3fe8ff, 0xff3864, 0xff6ad5, 0xffd319, 0xffffff},
        .roles = {DEFAULT_ROLES, [ROLE_STATUS] = ATTR(15, 1), [ROLE_LOGO2] = ATTR(14, 0)},
    },
    {
        .name = "classic",
        .description = "Authentic VGA text mode colors",
        .palette = {0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
                    0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(14, 0), [ROLE_PROMPT_USER] = ATTR(15, 0),
                  [ROLE_PROMPT_HOST] = ATTR(7, 0), [ROLE_PROMPT_SYMBOL] = ATTR(14, 0),
                  [ROLE_STATUS] = ATTR(0, 3), [ROLE_STATUS_ACCENT] = ATTR(15, 1),
                  [ROLE_LOGO1] = ATTR(11, 0), [ROLE_LOGO2] = ATTR(9, 0), [ROLE_LOGO3] = ATTR(1, 0)},
    },
    {
        .name = "commander",
        .description = "Blue panels in the spirit of classic file managers",
        .palette = {0x000000, 0x0000a8, 0x00a800, 0x00a8a8, 0xa80000, 0xa800a8, 0xa85400, 0xa8a8a8,
                    0x545454, 0x5454fc, 0x54fc54, 0x54fcfc, 0xfc5454, 0xfc54fc, 0xfcfc54, 0xfcfcfc},
        .roles = {[ROLE_TEXT] = ATTR(11, 1), [ROLE_MUTED] = ATTR(3, 1), [ROLE_ACCENT] = ATTR(14, 1),
                  [ROLE_ACCENT2] = ATTR(15, 1), [ROLE_HIGHLIGHT] = ATTR(15, 1),
                  [ROLE_OK] = ATTR(10, 1), [ROLE_WARN] = ATTR(14, 1), [ROLE_ERROR] = ATTR(12, 1),
                  [ROLE_DIR] = ATTR(15, 1), [ROLE_EXEC] = ATTR(10, 1), [ROLE_DEVICE] = ATTR(13, 1),
                  [ROLE_PROMPT_USER] = ATTR(14, 1), [ROLE_PROMPT_HOST] = ATTR(11, 1),
                  [ROLE_PROMPT_PATH] = ATTR(15, 1), [ROLE_PROMPT_SYMBOL] = ATTR(14, 1),
                  [ROLE_STATUS] = ATTR(0, 3), [ROLE_STATUS_ACCENT] = ATTR(15, 0),
                  [ROLE_SELECTION] = ATTR(0, 3), [ROLE_SYNTAX_COMMAND] = ATTR(15, 1),
                  [ROLE_SYNTAX_STRING] = ATTR(14, 1), [ROLE_SYNTAX_VARIABLE] = ATTR(10, 1),
                  [ROLE_SYNTAX_OPERATOR] = ATTR(13, 1), [ROLE_SYNTAX_ERROR] = ATTR(12, 1),
                  [ROLE_SUGGESTION] = ATTR(3, 1), [ROLE_LOGO1] = ATTR(15, 1),
                  [ROLE_LOGO2] = ATTR(14, 1), [ROLE_LOGO3] = ATTR(11, 1)},
    },
    {
        .name = "matrix",
        .description = "Green phosphor rain",
        .palette = {0x000a02, 0x0b3d1a, 0x0f7a2e, 0x1fa34a, 0x2f6b2f, 0x1d5c3a, 0x6b8f23, 0x4ce06b,
                    0x23492f, 0x3bbf63, 0x7dff9b, 0x9effb8, 0xb6ff5c, 0x5cff8a, 0xd6ffb0, 0xeaffef},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(10, 0), [ROLE_ACCENT2] = ATTR(11, 0),
                  [ROLE_PROMPT_USER] = ATTR(10, 0), [ROLE_PROMPT_HOST] = ATTR(3, 0),
                  [ROLE_PROMPT_SYMBOL] = ATTR(14, 0), [ROLE_STATUS] = ATTR(10, 1),
                  [ROLE_STATUS_ACCENT] = ATTR(0, 10), [ROLE_LOGO1] = ATTR(15, 0),
                  [ROLE_LOGO2] = ATTR(10, 0), [ROLE_LOGO3] = ATTR(2, 0)},
    },
    {
        .name = "amber",
        .description = "Warm monochrome amber terminal",
        .palette = {0x120b00, 0x3a2400, 0x8a5a00, 0xb87800, 0x9c4a00, 0x7a4300, 0xc77d00, 0xffb000,
                    0x5c3d00, 0xd99400, 0xffc640, 0xffd36b, 0xff8c1a, 0xffa94d, 0xffe08a, 0xfff2cc},
        .roles = {DEFAULT_ROLES, [ROLE_MUTED] = ATTR(8, 0), [ROLE_ACCENT] = ATTR(14, 0),
                  [ROLE_ACCENT2] = ATTR(11, 0), [ROLE_PROMPT_USER] = ATTR(15, 0),
                  [ROLE_PROMPT_HOST] = ATTR(6, 0), [ROLE_PROMPT_SYMBOL] = ATTR(14, 0),
                  [ROLE_STATUS] = ATTR(0, 7), [ROLE_STATUS_ACCENT] = ATTR(15, 1),
                  [ROLE_LOGO1] = ATTR(15, 0), [ROLE_LOGO2] = ATTR(14, 0), [ROLE_LOGO3] = ATTR(6, 0)},
    },
    {
        .name = "nord",
        .description = "Arctic, north-bluish calm",
        .palette = {0x2e3440, 0x5e81ac, 0xa3be8c, 0x88c0d0, 0xbf616a, 0xb48ead, 0xd08770, 0xd8dee9,
                    0x4c566a, 0x81a1c1, 0xb5d19c, 0x8fbcbb, 0xd57780, 0xc895bf, 0xebcb8b, 0xeceff4},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(11, 0), [ROLE_ACCENT2] = ATTR(9, 0),
                  [ROLE_PROMPT_USER] = ATTR(3, 0), [ROLE_PROMPT_HOST] = ATTR(9, 0),
                  [ROLE_PROMPT_PATH] = ATTR(14, 0), [ROLE_PROMPT_SYMBOL] = ATTR(11, 0),
                  [ROLE_STATUS] = ATTR(15, 8), [ROLE_STATUS_ACCENT] = ATTR(0, 3),
                  [ROLE_LOGO1] = ATTR(3, 0), [ROLE_LOGO2] = ATTR(9, 0), [ROLE_LOGO3] = ATTR(1, 0)},
    },
    {
        .name = "dracula",
        .description = "Dark theme for vampires",
        .palette = {0x282a36, 0x44475a, 0x50fa7b, 0x8be9fd, 0xff5555, 0xbd93f9, 0xffb86c, 0xf8f8f2,
                    0x6272a4, 0x9aa5ff, 0x69ff94, 0xa4ffff, 0xff6e6e, 0xff79c6, 0xf1fa8c, 0xffffff},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(13, 0), [ROLE_ACCENT2] = ATTR(5, 0),
                  [ROLE_PROMPT_USER] = ATTR(5, 0), [ROLE_PROMPT_HOST] = ATTR(13, 0),
                  [ROLE_PROMPT_PATH] = ATTR(3, 0), [ROLE_PROMPT_SYMBOL] = ATTR(2, 0),
                  [ROLE_STATUS] = ATTR(15, 1), [ROLE_STATUS_ACCENT] = ATTR(0, 5),
                  [ROLE_LOGO1] = ATTR(5, 0), [ROLE_LOGO2] = ATTR(13, 0), [ROLE_LOGO3] = ATTR(3, 0)},
    },
    {
        .name = "gruvbox",
        .description = "Retro groove with warm contrast",
        .palette = {0x282828, 0x458588, 0x98971a, 0x689d6a, 0xcc241d, 0xb16286, 0xd79921, 0xebdbb2,
                    0x928374, 0x83a598, 0xb8bb26, 0x8ec07c, 0xfb4934, 0xd3869b, 0xfabd2f, 0xfbf1c7},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(14, 0), [ROLE_ACCENT2] = ATTR(11, 0),
                  [ROLE_PROMPT_USER] = ATTR(10, 0), [ROLE_PROMPT_HOST] = ATTR(6, 0),
                  [ROLE_PROMPT_PATH] = ATTR(9, 0), [ROLE_PROMPT_SYMBOL] = ATTR(14, 0),
                  [ROLE_STATUS] = ATTR(15, 8), [ROLE_STATUS_ACCENT] = ATTR(0, 14),
                  [ROLE_LOGO1] = ATTR(12, 0), [ROLE_LOGO2] = ATTR(14, 0), [ROLE_LOGO3] = ATTR(10, 0)},
    },
    {
        .name = "paper",
        .description = "Light theme, ink on warm paper",
        .palette = {0xf7f3e8, 0x2b59c3, 0x2e7d32, 0x00838f, 0xc62828, 0x8e24aa, 0xb26a00, 0x37352f,
                    0x9e9a8f, 0x1e88e5, 0x43a047, 0x0097a7, 0xe53935, 0xab47bc, 0xef6c00, 0x111111},
        .roles = {DEFAULT_ROLES, [ROLE_TEXT] = ATTR(7, 0), [ROLE_HIGHLIGHT] = ATTR(15, 0),
                  [ROLE_ACCENT] = ATTR(5, 0), [ROLE_ACCENT2] = ATTR(1, 0), [ROLE_OK] = ATTR(2, 0),
                  [ROLE_WARN] = ATTR(6, 0), [ROLE_ERROR] = ATTR(4, 0), [ROLE_DIR] = ATTR(1, 0),
                  [ROLE_EXEC] = ATTR(2, 0), [ROLE_DEVICE] = ATTR(6, 0),
                  [ROLE_PROMPT_USER] = ATTR(5, 0), [ROLE_PROMPT_HOST] = ATTR(8, 0),
                  [ROLE_PROMPT_PATH] = ATTR(1, 0), [ROLE_PROMPT_SYMBOL] = ATTR(5, 0),
                  [ROLE_STATUS] = ATTR(0, 7), [ROLE_STATUS_ACCENT] = ATTR(0, 5),
                  [ROLE_SELECTION] = ATTR(0, 1), [ROLE_SYNTAX_COMMAND] = ATTR(2, 0),
                  [ROLE_SYNTAX_STRING] = ATTR(6, 0), [ROLE_SYNTAX_VARIABLE] = ATTR(3, 0),
                  [ROLE_SYNTAX_OPERATOR] = ATTR(5, 0), [ROLE_SYNTAX_ERROR] = ATTR(4, 0),
                  [ROLE_SUGGESTION] = ATTR(8, 0), [ROLE_LOGO1] = ATTR(5, 0),
                  [ROLE_LOGO2] = ATTR(4, 0), [ROLE_LOGO3] = ATTR(1, 0)},
    },
    {
        .name = "mono",
        .description = "Pure grayscale minimalism",
        .palette = {0x0c0c0c, 0x2a2a2a, 0x4a4a4a, 0x6a6a6a, 0x5a5a5a, 0x3a3a3a, 0x7a7a7a, 0xb8b8b8,
                    0x5c5c5c, 0x8a8a8a, 0xa0a0a0, 0xc8c8c8, 0xe0e0e0, 0xd0d0d0, 0xececec, 0xffffff},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(15, 0), [ROLE_ACCENT2] = ATTR(12, 0),
                  [ROLE_OK] = ATTR(15, 0), [ROLE_WARN] = ATTR(14, 0), [ROLE_ERROR] = ATTR(15, 0),
                  [ROLE_DIR] = ATTR(15, 0), [ROLE_EXEC] = ATTR(11, 0),
                  [ROLE_PROMPT_USER] = ATTR(15, 0), [ROLE_PROMPT_HOST] = ATTR(8, 0),
                  [ROLE_PROMPT_PATH] = ATTR(11, 0), [ROLE_PROMPT_SYMBOL] = ATTR(15, 0),
                  [ROLE_STATUS] = ATTR(0, 7), [ROLE_STATUS_ACCENT] = ATTR(15, 0),
                  [ROLE_LOGO1] = ATTR(15, 0), [ROLE_LOGO2] = ATTR(11, 0), [ROLE_LOGO3] = ATTR(8, 0)},
    },
    {
        .name = "ocean",
        .description = "Deep sea blues with coral highlights",
        .palette = {0x0b1622, 0x1b3a5c, 0x2bb38f, 0x2a9fd6, 0xe0605c, 0x9b6fd8, 0xe7a35c, 0xc5d8e8,
                    0x3f5872, 0x4fa8ff, 0x5ee3b5, 0x6ad9ff, 0xff7f78, 0xc79bff, 0xffd27f, 0xf2f9ff},
        .roles = {DEFAULT_ROLES, [ROLE_ACCENT] = ATTR(11, 0), [ROLE_ACCENT2] = ATTR(12, 0),
                  [ROLE_PROMPT_USER] = ATTR(11, 0), [ROLE_PROMPT_HOST] = ATTR(3, 0),
                  [ROLE_PROMPT_PATH] = ATTR(14, 0), [ROLE_PROMPT_SYMBOL] = ATTR(12, 0),
                  [ROLE_STATUS] = ATTR(15, 1), [ROLE_STATUS_ACCENT] = ATTR(0, 11),
                  [ROLE_LOGO1] = ATTR(11, 0), [ROLE_LOGO2] = ATTR(9, 0), [ROLE_LOGO3] = ATTR(12, 0)},
    },
};

static const char *const role_names[ROLE_COUNT] = {
    [ROLE_TEXT] = "text",
    [ROLE_MUTED] = "muted",
    [ROLE_ACCENT] = "accent",
    [ROLE_ACCENT2] = "accent2",
    [ROLE_HIGHLIGHT] = "highlight",
    [ROLE_OK] = "ok",
    [ROLE_WARN] = "warn",
    [ROLE_ERROR] = "error",
    [ROLE_DIR] = "dir",
    [ROLE_EXEC] = "exec",
    [ROLE_DEVICE] = "device",
    [ROLE_PROMPT_USER] = "prompt.user",
    [ROLE_PROMPT_HOST] = "prompt.host",
    [ROLE_PROMPT_PATH] = "prompt.path",
    [ROLE_PROMPT_SYMBOL] = "prompt.symbol",
    [ROLE_STATUS] = "status",
    [ROLE_STATUS_ACCENT] = "status.accent",
    [ROLE_SELECTION] = "selection",
    [ROLE_SYNTAX_COMMAND] = "syntax.command",
    [ROLE_SYNTAX_STRING] = "syntax.string",
    [ROLE_SYNTAX_VARIABLE] = "syntax.variable",
    [ROLE_SYNTAX_OPERATOR] = "syntax.operator",
    [ROLE_SYNTAX_ERROR] = "syntax.error",
    [ROLE_SUGGESTION] = "suggestion",
    [ROLE_LOGO1] = "logo1",
    [ROLE_LOGO2] = "logo2",
    [ROLE_LOGO3] = "logo3",
};

static const char *const color_names[16] = {
    "black",    "blue",      "green",      "cyan",      "red",      "magenta",
    "brown",    "lightgray", "darkgray",   "lightblue", "lightgreen", "lightcyan",
    "lightred", "pink",      "yellow",     "white",
};

static struct theme current = {0};
static bool initialized;

const struct theme *theme_current(void)
{
    if (!initialized) {
        current = builtin_themes[0];
        initialized = true;
    }
    return &current;
}

uint8_t theme_attr(enum theme_role role)
{
    return theme_current()->roles[role < ROLE_COUNT ? role : ROLE_TEXT];
}

uint8_t theme_background(void)
{
    return ATTR_BG(theme_attr(ROLE_TEXT));
}

void theme_apply(const struct theme *t)
{
    current = *t;
    initialized = true;
    con_reset();
    con_apply_palette();
}

void theme_set_role(enum theme_role role, uint8_t attr)
{
    theme_current();
    if (role < ROLE_COUNT)
        current.roles[role] = attr;
}

void theme_set_color(int index, uint32_t rgb)
{
    theme_current();
    if (index >= 0 && index < 16) {
        current.palette[index] = rgb & 0xffffff;
        con_apply_palette();
    }
}

size_t theme_builtin_count(void)
{
    return ARRAY_SIZE(builtin_themes);
}

const struct theme *theme_builtin(size_t index)
{
    return index < ARRAY_SIZE(builtin_themes) ? &builtin_themes[index] : NULL;
}

const char *theme_role_name(enum theme_role role)
{
    return role < ROLE_COUNT ? role_names[role] : "?";
}

int theme_role_lookup(const char *name)
{
    for (int i = 0; i < ROLE_COUNT; i++)
        if (str_casecmp(role_names[i], name) == 0)
            return i;
    return -1;
}

const char *theme_color_name(int color)
{
    return color >= 0 && color < 16 ? color_names[color] : "?";
}

int theme_color_lookup(const char *name)
{
    long n;
    if (str_parse_long(name, &n))
        return n >= 0 && n < 16 ? (int)n : -1;
    for (int i = 0; i < 16; i++)
        if (str_casecmp(color_names[i], name) == 0)
            return i;
    if (str_casecmp(name, "gray") == 0 || str_casecmp(name, "grey") == 0)
        return COLOR_LIGHT_GRAY;
    if (str_casecmp(name, "lightmagenta") == 0)
        return COLOR_LIGHT_MAGENTA;
    return -1;
}

static bool parse_rgb(const char *s, uint32_t *out)
{
    if (*s == '#')
        s++;
    if (strlen(s) != 6)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        if (!is_xdigit((uint8_t)s[i]))
            return false;
        v = v * 16 + (uint32_t)(is_digit((uint8_t)s[i]) ? s[i] - '0' : to_lower((uint8_t)s[i]) - 'a' + 10);
    }
    *out = v;
    return true;
}

static bool parse_attr(char *value, uint8_t fallback_bg, uint8_t *out)
{
    char *bg = strstr(value, " on ");
    if (bg) {
        *bg = 0;
        bg += 4;
    } else if ((bg = strchr(value, '/'))) {
        *bg++ = 0;
    }
    int f = theme_color_lookup(str_trim(value));
    int b = bg ? theme_color_lookup(str_trim(bg)) : fallback_bg;
    if (f < 0 || b < 0)
        return false;
    *out = ATTR(f, b);
    return true;
}

int theme_find(const char *name, struct theme *out)
{
    for (size_t i = 0; i < ARRAY_SIZE(builtin_themes); i++) {
        if (str_casecmp(builtin_themes[i].name, name) == 0) {
            *out = builtin_themes[i];
            return 0;
        }
    }
    char path[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), "/etc/themes/%s.theme", name);
    char *text;
    size_t size;
    int err = vfs_read_all(name[0] == '/' ? name : path, &text, &size);
    if (err)
        return err;
    char msg[64];
    err = theme_parse(text, size, out, msg, sizeof(msg));
    kfree(text);
    return err;
}

int theme_load(const char *name)
{
    struct theme t;
    int err = theme_find(name, &t);
    if (err == 0)
        theme_apply(&t);
    return err;
}

int theme_parse(const char *text, size_t len, struct theme *out, char *err, size_t errcap)
{
    *out = builtin_themes[0];
    int lineno = 0;
    size_t pos = 0;
    while (pos < len) {
        char line[160];
        size_t n = 0;
        while (pos < len && text[pos] != '\n') {
            if (n + 1 < sizeof(line))
                line[n++] = text[pos];
            pos++;
        }
        pos++;
        line[n] = 0;
        lineno++;
        char *s = str_trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            ksnprintf(err, errcap, "line %d: expected key = value", lineno);
            return E_INVAL;
        }
        *eq = 0;
        char *key = str_trim(s);
        char *value = str_trim(eq + 1);
        int role;
        uint32_t rgb;
        if (strcmp(key, "name") == 0) {
            str_copy(out->name, value, sizeof(out->name));
        } else if (strcmp(key, "description") == 0) {
            str_copy(out->description, value, sizeof(out->description));
        } else if (strcmp(key, "base") == 0) {
            char name[sizeof(out->name)], desc[sizeof(out->description)];
            memcpy(name, out->name, sizeof(name));
            memcpy(desc, out->description, sizeof(desc));
            bool found = false;
            for (size_t i = 0; i < ARRAY_SIZE(builtin_themes); i++)
                if (str_casecmp(builtin_themes[i].name, value) == 0)
                    *out = builtin_themes[i], found = true;
            if (!found) {
                ksnprintf(err, errcap, "line %d: unknown base theme '%s'", lineno, value);
                return E_INVAL;
            }
            memcpy(out->name, name, sizeof(name));
            memcpy(out->description, desc, sizeof(desc));
        } else if (str_starts(key, "color") && is_digit((uint8_t)key[5])) {
            long idx;
            if (!str_parse_long(key + 5, &idx) || idx < 0 || idx > 15 || !parse_rgb(value, &rgb)) {
                ksnprintf(err, errcap, "line %d: bad color definition", lineno);
                return E_INVAL;
            }
            out->palette[idx] = rgb;
        } else if ((role = theme_role_lookup(key)) >= 0) {
            if (!parse_attr(value, ATTR_BG(out->roles[ROLE_TEXT]), &out->roles[role])) {
                ksnprintf(err, errcap, "line %d: bad color for role '%s'", lineno, key);
                return E_INVAL;
            }
        } else {
            ksnprintf(err, errcap, "line %d: unknown key '%s'", lineno, key);
            return E_INVAL;
        }
    }
    return 0;
}

size_t theme_serialize(const struct theme *t, char *buf, size_t cap)
{
    size_t len = 0;
    char line[96];
#define PUT(...)                                                                        \
    do {                                                                                \
        ksnprintf(line, sizeof(line), __VA_ARGS__);                                     \
        len = str_append(buf, line, cap);                                               \
    } while (0)
    if (cap)
        buf[0] = 0;
    PUT("# SynthOS theme\nname = %s\ndescription = %s\n\n", t->name, t->description);
    for (int i = 0; i < 16; i++)
        PUT("color%d = #%06x\n", i, (unsigned)t->palette[i]);
    PUT("\n");
    for (int r = 0; r < ROLE_COUNT; r++)
        PUT("%s = %s on %s\n", role_names[r], color_names[ATTR_FG(t->roles[r])],
            color_names[ATTR_BG(t->roles[r])]);
#undef PUT
    return len;
}
