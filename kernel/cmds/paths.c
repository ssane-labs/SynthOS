#include <synth/command.h>
#include <synth/error.h>
#include <synth/string.h>
#include <synth/winpath.h>

static void print_rule(const struct winpath_rule *rule, void *ctx)
{
    struct shell *sh = ctx;
    sh_role(sh, ROLE_ACCENT2);
    sh_printf(sh, "  %-26s", rule->windows);
    sh_role(sh, ROLE_MUTED);
    sh_puts(sh, " \x1a ");
    sh_role(sh, ROLE_DIR);
    sh_printf(sh, "%s\n", rule->synth);
    sh_reset(sh);
}

COMMAND(drives, "winpaths", "files", "drives", "show how Windows and DOS paths map onto SynthOS folders")
{
    UNUSED(argc);
    UNUSED(argv);
    sh_printf(sh, "Windows view of the SynthOS file system (names are case-insensitive):\n");
    winpath_rules(print_rule, sh);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "  other letters D: .. Y:  \x1a /mnt/<letter>      Z:  \x1a /\n");
    sh_reset(sh);
    return 0;
}

COMMAND(winpath, NULL, "files", "winpath path...", "convert between Windows paths and SynthOS paths")
{
    if (argc < 2)
        return command_usage(sh, command_find("winpath"));
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char out[PATH_MAX_LEN];
        const char *arg = argv[i];
        bool windows = (is_alpha((uint8_t)arg[0]) && arg[1] == ':') || strchr(arg, '\\');
        if (windows) {
            int err = winpath_to_vfs(sh->cwd, arg, out, sizeof(out));
            if (err) {
                rc = sh_fs_error(sh, "winpath", arg, err);
                continue;
            }
        } else {
            char abs[PATH_MAX_LEN];
            int err = sh_resolve(sh, arg, abs, sizeof(abs));
            if (err) {
                rc = sh_fs_error(sh, "winpath", arg, err);
                continue;
            }
            winpath_from_vfs(abs, out, sizeof(out));
        }
        sh_printf(sh, "%s\n", out);
    }
    return rc;
}
