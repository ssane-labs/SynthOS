#include <synth/codepage.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "internal.h"

SYNTH_TABLE_DECLARE(synth_settings, struct setting);

const struct setting *setting_find(const char *name)
{
    SYNTH_TABLE_FOREACH(synth_settings, struct setting, st)
        if (strcmp(st->name, name) == 0)
            return st;
    return NULL;
}

int setting_apply(struct shell *sh, const char *name, const char *value)
{
    const struct setting *st = setting_find(name);
    if (!st)
        return E_NOENT;
    return st->set(sh, value);
}

int settings_save(const char *path)
{
    int err;
    struct stream *s = stream_file(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &err);
    if (!s)
        return err;
    stream_puts(s, "# SynthOS settings - written by 'config save'\n");
    SYNTH_TABLE_FOREACH(synth_settings, struct setting, st) {
        char value[256];
        st->get(value, sizeof(value));
        stream_printf(s, "config %s '", st->name);
        for (const char *p = value; *p; p++) {
            if (*p == '\'')
                stream_puts(s, "'\"'\"'");
            else
                stream_putc(s, *p);
        }
        stream_puts(s, "'\n");
    }
    stream_close(s);
    return 0;
}

static int parse_bool(const char *v)
{
    if (str_casecmp(v, "on") == 0 || str_casecmp(v, "yes") == 0 || strcmp(v, "1") == 0 ||
        str_casecmp(v, "true") == 0)
        return 1;
    if (str_casecmp(v, "off") == 0 || str_casecmp(v, "no") == 0 || strcmp(v, "0") == 0 ||
        str_casecmp(v, "false") == 0)
        return 0;
    return -1;
}

int setting_flag(const char *value, bool *out)
{
    int b = parse_bool(value);
    if (b < 0)
        return E_INVAL;
    *out = b;
    return 0;
}

void setting_flag_get(bool value, char *buf, size_t cap)
{
    str_copy(buf, value ? "on" : "off", cap);
}

static int key_delay = 500, key_rate = 30;

static void get_key_delay(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", key_delay);
}

static int set_key_delay(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 250 || v > 1000)
        return E_INVAL;
    key_delay = (int)v;
    hal_key_repeat(key_delay, key_rate);
    return 0;
}

static void get_key_rate(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", key_rate);
}

static int set_key_rate(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long v;
    if (!str_parse_long(value, &v) || v < 2 || v > 30)
        return E_INVAL;
    key_rate = (int)v;
    hal_key_repeat(key_delay, key_rate);
    return 0;
}

static const struct setting s_key_delay = {"keydelay", "250..1000", "how long a held key waits before it repeats, ms",
                                           get_key_delay, set_key_delay};
static const struct setting s_key_rate = {"keyrate", "2..30", "how many times a second a held key repeats",
                                          get_key_rate, set_key_rate};
SETTING(s_key_delay);
SETTING(s_key_rate);

static void get_var(const char *name, const char *fallback, char *buf, size_t cap)
{
    struct shell *sh = shell_current();
    const char *v = sh ? sh_get(sh, name) : NULL;
    str_copy(buf, v ? v : fallback, cap);
}

static int set_flag_var(struct shell *sh, const char *name, const char *value)
{
    int b = parse_bool(value);
    if (b < 0)
        return E_INVAL;
    sh_set(sh, name, b ? "on" : "off");
    return 0;
}

static void theme_get(char *buf, size_t cap)
{
    str_copy(buf, theme_current()->name, cap);
}

static int theme_set(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return theme_load(value);
}

static const struct setting s_theme = {"theme", "<name>", "color theme (see 'theme list')", theme_get, theme_set};
SETTING(s_theme);

static void prompt_get(char *buf, size_t cap)
{
    get_var("PROMPT", "", buf, cap);
}

static int prompt_set(struct shell *sh, const char *value)
{
    const char *preset = prompt_preset(value);
    sh_set(sh, "PROMPT", preset ? preset : value);
    return 0;
}

static const struct setting s_prompt = {"prompt", "<preset>|<format>", "prompt preset or format string", prompt_get, prompt_set};
SETTING(s_prompt);

static void statusbar_get(char *buf, size_t cap)
{
    str_copy(buf, con_statusbar() ? "on" : "off", cap);
}

static int statusbar_set(struct shell *sh, const char *value)
{
    UNUSED(sh);
    int b = parse_bool(value);
    if (b < 0)
        return E_INVAL;
    con_set_statusbar(b);
    return 0;
}

static const struct setting s_statusbar = {"termstatus", "on|off", "terminal status bar with clock at the bottom",
                                           statusbar_get, statusbar_set};
SETTING(s_statusbar);

static void highlight_get(char *buf, size_t cap)
{
    get_var("HIGHLIGHT", "on", buf, cap);
}

static int highlight_set(struct shell *sh, const char *value)
{
    return set_flag_var(sh, "HIGHLIGHT", value);
}

static const struct setting s_highlight = {"highlight", "on|off", "live syntax highlighting while typing", highlight_get, highlight_set};
SETTING(s_highlight);

static void suggest_get(char *buf, size_t cap)
{
    get_var("SUGGEST", "on", buf, cap);
}

static int suggest_set(struct shell *sh, const char *value)
{
    return set_flag_var(sh, "SUGGEST", value);
}

static const struct setting s_suggest = {"suggest", "on|off", "history autosuggestions (accept with Right)", suggest_get, suggest_set};
SETTING(s_suggest);

static const char *const cursor_names[] = {"block", "underline", "bar"};

static void cursor_get(char *buf, size_t cap)
{
    str_copy(buf, cursor_names[con_cursor_shape()], cap);
}

static int cursor_set(struct shell *sh, const char *value)
{
    UNUSED(sh);
    for (int i = 0; i < 3; i++) {
        if (str_casecmp(value, cursor_names[i]) == 0) {
            con_set_cursor_shape((enum cursor_shape)i);
            return 0;
        }
    }
    return E_INVAL;
}

static const struct setting s_cursor = {"cursor", "block|underline|bar", "text cursor shape", cursor_get, cursor_set};
SETTING(s_cursor);

static void codepage_get(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "%d", codepage_current());
}

static int codepage_set_value(struct shell *sh, const char *value)
{
    UNUSED(sh);
    long cp;
    if (!str_parse_long(value, &cp) || !codepage_set((int)cp))
        return E_INVAL;
    con_redraw();
    return 0;
}

static const struct setting s_codepage = {"codepage", "437|866", "character set (866 = Cyrillic)", codepage_get, codepage_set_value};
SETTING(s_codepage);

static void banner_get(char *buf, size_t cap)
{
    get_var("BANNER", "on", buf, cap);
}

static int banner_set(struct shell *sh, const char *value)
{
    return set_flag_var(sh, "BANNER", value);
}

static const struct setting s_banner = {"banner", "on|off", "show the logo at startup", banner_get, banner_set};
SETTING(s_banner);

static void hostname_get(char *buf, size_t cap)
{
    get_var("HOSTNAME", "synth", buf, cap);
}

static int hostname_set(struct shell *sh, const char *value)
{
    if (!*value)
        return E_INVAL;
    sh_set(sh, "HOSTNAME", value);
    return 0;
}

static const struct setting s_hostname = {"hostname", "<name>", "machine name shown in the prompt", hostname_get, hostname_set};
SETTING(s_hostname);

static void user_get(char *buf, size_t cap)
{
    get_var("USER", "root", buf, cap);
}

static int user_set(struct shell *sh, const char *value)
{
    if (!*value)
        return E_INVAL;
    sh_set(sh, "USER", value);
    return 0;
}

static const struct setting s_user = {"user", "<name>", "user name shown in the prompt", user_get, user_set};
SETTING(s_user);
