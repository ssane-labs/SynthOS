#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#define SETTINGS_FILE "/home/.synth.conf"

SYNTH_TABLE_DECLARE(synth_settings, struct setting);

static void swatches(struct shell *sh, const struct theme *t)
{
    if (!stream_is_console(sh->out))
        return;
    uint8_t bg = ATTR_BG(t->roles[ROLE_TEXT]);
    for (int c = 0; c < 16; c++) {
        con_set_attr(ATTR(c == bg ? (c + 8) % 16 : c, c));
        sh_puts(sh, c == bg ? "\xfe" : " ");
    }
    con_reset();
}

static void theme_row(struct shell *sh, const struct theme *t, bool current)
{
    sh_role(sh, current ? ROLE_ACCENT : ROLE_MUTED);
    sh_puts(sh, current ? " \x10 " : "   ");
    sh_role(sh, current ? ROLE_HIGHLIGHT : ROLE_TEXT);
    sh_printf(sh, "%-12s", t->name);
    sh_reset(sh);
    swatches(sh, t);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "  %s\n", t->description);
    sh_reset(sh);
}

static int theme_list(struct shell *sh)
{
    const char *active = theme_current()->name;
    for (size_t i = 0; i < theme_builtin_count(); i++)
        theme_row(sh, theme_builtin(i), strcmp(active, theme_builtin(i)->name) == 0);
    struct vdir d;
    if (vfs_opendir("/etc/themes", &d) == 0) {
        struct vfs_dirent ent;
        while (vfs_readdir(&d, &ent) == 0) {
            if (!str_ends(ent.name, ".theme"))
                continue;
            ent.name[strlen(ent.name) - 6] = 0;
            struct theme t;
            if (theme_find(ent.name, &t) == 0)
                theme_row(sh, &t, strcmp(active, t.name) == 0);
        }
        vfs_closedir(&d);
    }
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "\n'theme <name>' to switch, 'theme show' to preview, 'config save' to keep it.\n");
    sh_reset(sh);
    return 0;
}

static int theme_show(struct shell *sh, const struct theme *t)
{
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%s", t->name);
    sh_reset(sh);
    sh_printf(sh, " - %s\n\n", t->description);
    if (!stream_is_console(sh->out)) {
        char *buf = kmalloc(4096);
        if (buf) {
            theme_serialize(t, buf, 4096);
            sh_puts(sh, buf);
            kfree(buf);
        }
        return 0;
    }
    for (int c = 0; c < 16; c++) {
        con_set_attr(ATTR(c, ATTR_BG(t->roles[ROLE_TEXT])));
        sh_printf(sh, " %2d ", c);
        con_set_attr(ATTR(c, c));
        sh_puts(sh, "      ");
        con_reset();
        sh_printf(sh, " #%06x %-12s", (unsigned)t->palette[c], theme_color_name(c));
        if (c % 2 == 1)
            sh_puts(sh, "\n");
    }
    sh_puts(sh, "\n");
    for (int r = 0; r < ROLE_COUNT; r++) {
        con_set_attr(t->roles[r]);
        sh_printf(sh, " %-16s", theme_role_name((enum theme_role)r));
        con_reset();
        if (r % 4 == 3 || r + 1 == ROLE_COUNT)
            sh_puts(sh, "\n");
    }
    return 0;
}

COMMAND(theme, NULL, "custom", "theme [list | <name> | show [name] | save <name> | reset]", "color themes")
{
    if (argc == 1 || strcmp(argv[1], "list") == 0)
        return theme_list(sh);
    if (strcmp(argv[1], "show") == 0) {
        struct theme t = *theme_current();
        if (argc > 2 && theme_find(argv[2], &t) != 0)
            return sh_error(sh, "theme", "unknown theme '%s'", argv[2]);
        return theme_show(sh, &t);
    }
    if (strcmp(argv[1], "reset") == 0) {
        theme_apply(theme_builtin(0));
        return 0;
    }
    if (strcmp(argv[1], "save") == 0) {
        if (argc < 3)
            return command_usage(sh, command_find("theme"));
        struct theme t = *theme_current();
        str_copy(t.name, argv[2], sizeof(t.name));
        if (strcmp(t.description, theme_current()->description) == 0)
            str_copy(t.description, "Custom theme", sizeof(t.description));
        char *buf = kmalloc(4096), path[PATH_MAX_LEN];
        if (!buf)
            return 1;
        size_t n = theme_serialize(&t, buf, 4096);
        ksnprintf(path, sizeof(path), "/etc/themes/%s.theme", argv[2]);
        vfs_mkdirs("/etc/themes");
        int err = vfs_write_all(path, buf, n);
        kfree(buf);
        if (err)
            return sh_fs_error(sh, "theme", path, err);
        theme_apply(&t);
        sh_printf(sh, "saved %s - edit it with 'edit %s'\n", path, path);
        return 0;
    }
    int err = theme_load(argv[1]);
    if (err == E_NOENT)
        return sh_error(sh, "theme", "unknown theme '%s' (try 'theme list')", argv[1]);
    if (err)
        return sh_error(sh, "theme", "%s: %s", argv[1], err_str(err));
    return 0;
}

COMMAND(color, "colour", "custom", "color <role> <fg> [on <bg>] | color <0-15> #rrggbb", "recolor a theme role or palette entry")
{
    if (argc == 1) {
        for (int r = 0; r < ROLE_COUNT; r++) {
            uint8_t a = theme_attr((enum theme_role)r);
            con_set_attr(a);
            sh_printf(sh, "%-16s", theme_role_name((enum theme_role)r));
            con_reset();
            sh_printf(sh, " %s on %s\n", theme_color_name(ATTR_FG(a)), theme_color_name(ATTR_BG(a)));
        }
        return 0;
    }
    long index;
    if (argc == 3 && str_parse_long(argv[1], &index)) {
        const char *hex = argv[2][0] == '#' ? argv[2] + 1 : argv[2];
        const char *end;
        uint32_t rgb = (uint32_t)str_to_long(hex, &end, 16);
        if (index < 0 || index > 15 || *end || strlen(hex) != 6)
            return command_usage(sh, command_find("color"));
        theme_set_color((int)index, rgb);
        return 0;
    }
    int role = theme_role_lookup(argv[1]);
    if (role < 0)
        return sh_error(sh, "color", "unknown role '%s' (run 'color' to list roles)", argv[1]);
    if (argc < 3)
        return command_usage(sh, command_find("color"));
    int fg = theme_color_lookup(argv[2]);
    int bg = ATTR_BG(theme_attr((enum theme_role)role));
    if (argc >= 5 && strcmp(argv[3], "on") == 0)
        bg = theme_color_lookup(argv[4]);
    if (fg < 0 || bg < 0)
        return sh_error(sh, "color", "unknown color (use 0-15 or a name like pink, lightcyan)");
    theme_set_role((enum theme_role)role, ATTR(fg, bg));
    if (role == ROLE_TEXT)
        con_reset();
    return 0;
}

COMMAND(palette, "colors", "custom", "palette", "show the 16 colors of the current theme")
{
    return theme_show(sh, theme_current());
}

struct preset_ctx {
    struct shell *sh;
};

static void show_preset(const char *name, const char *format, void *ctx)
{
    struct shell *sh = ((struct preset_ctx *)ctx)->sh;
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%-9s", name);
    sh_reset(sh);
    if (!stream_is_console(sh->out)) {
        sh_printf(sh, "%s\n", format);
        return;
    }
    char *saved = kstrdup(sh_get(sh, "PROMPT") ? sh_get(sh, "PROMPT") : "");
    sh_set(sh, "PROMPT", format);
    prompt_render(sh);
    sh_set(sh, "PROMPT", saved ? saved : "");
    kfree(saved);
    sh_puts(sh, "\n");
}

COMMAND(prompt, NULL, "custom", "prompt [preset | \"format\" | tokens]", "choose or design the prompt")
{
    if (argc == 1) {
        struct preset_ctx ctx = {sh};
        prompt_presets(show_preset, &ctx);
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "\n'prompt <name>' to switch, 'prompt tokens' for building your own.\n");
        sh_reset(sh);
        return 0;
    }
    if (strcmp(argv[1], "tokens") == 0) {
        sh_printf(sh, "  %%u user      %%h host       %%w path (~)   %%W folder    %%D DOS path\n"
                      "  %%t HH:MM     %%T HH:MM:SS   %%d date       %%? status    %%e status if failed\n"
                      "  %%> arrow     %%n new line   %%%% percent    %%v version\n"
                      "  %%{role} color from the theme, e.g. %%{accent} %%{prompt.path} %%{/} resets\n"
                      "example: prompt \"%%{accent}%%u %%{prompt.path}%%W %%{ok}%%>%%{/} \"\n");
        return 0;
    }
    const char *preset = prompt_preset(argv[1]);
    sh_set(sh, "PROMPT", preset ? preset : argv[1]);
    return 0;
}

COMMAND(config, "setup", "custom", "config [name [value] | save | reset]", "view and change system settings")
{
    if (argc == 1) {
        SYNTH_TABLE_FOREACH(synth_settings, struct setting, st) {
            char value[256];
            st->get(value, sizeof(value));
            if (strlen(value) > 34)
                str_copy(value + 31, "...", 4);
            sh_role(sh, ROLE_ACCENT2);
            sh_printf(sh, "  %-10s", st->name);
            sh_role(sh, ROLE_HIGHLIGHT);
            sh_printf(sh, " %-34s", value);
            sh_role(sh, ROLE_MUTED);
            sh_printf(sh, " %s\n", st->summary);
            sh_reset(sh);
        }
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "\n'config <name> <value>' to change, 'config save' to keep after reboot.\n");
        sh_reset(sh);
        return 0;
    }
    if (strcmp(argv[1], "save") == 0) {
        int err = settings_save(SETTINGS_FILE);
        if (err)
            return sh_fs_error(sh, "config", SETTINGS_FILE, err);
        sh_role(sh, ROLE_OK);
        sh_printf(sh, "settings saved to %s\n", SETTINGS_FILE);
        sh_reset(sh);
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        vfs_unlink(SETTINGS_FILE);
        theme_apply(theme_builtin(0));
        setting_apply(sh, "prompt", "neon");
        return 0;
    }
    const struct setting *st = setting_find(argv[1]);
    if (!st)
        return sh_error(sh, "config", "unknown setting '%s'", argv[1]);
    if (argc == 2) {
        char value[256];
        st->get(value, sizeof(value));
        sh_printf(sh, "%s = %s\n", st->name, value);
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "values: %s - %s\n", st->values, st->summary);
        sh_reset(sh);
        return 0;
    }
    int err = st->set(sh, argv[2]);
    if (err)
        return sh_error(sh, "config", "%s: invalid value '%s' (expected %s)", st->name, argv[2], st->values);
    return 0;
}

COMMAND(banner, "logo", "custom", "banner", "show the SynthOS logo")
{
    UNUSED(argc);
    UNUSED(argv);
    if (!stream_is_console(sh->out)) {
        sh_printf(sh, "SynthOS\n");
        return 0;
    }
    shell_banner();
    return 0;
}
