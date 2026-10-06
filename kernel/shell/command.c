#include <synth/command.h>
#include <synth/string.h>

SYNTH_TABLE_DECLARE(synth_commands, struct command);

const struct command *command_find(const char *name)
{
    SYNTH_TABLE_FOREACH(synth_commands, struct command, cmd)
        if (strcmp(cmd->name, name) == 0)
            return cmd;
    SYNTH_TABLE_FOREACH(synth_commands, struct command, cmd)
        if (cmd->aliases && str_in_list(name, cmd->aliases))
            return cmd;
    return NULL;
}

void command_foreach(void (*fn)(const struct command *cmd, void *ctx), void *ctx)
{
    SYNTH_TABLE_FOREACH(synth_commands, struct command, cmd)
        fn(cmd, ctx);
}

int command_usage(struct shell *sh, const struct command *cmd)
{
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "usage: ");
    sh_reset(sh);
    sh_printf(sh, "%s\n", cmd->usage);
    return 2;
}

void getopt_init(struct getopt *g, int argc, char **argv)
{
    *g = (struct getopt){.argc = argc, .argv = argv, .index = 1, .pos = 0};
}

int getopt_next(struct getopt *g, const char *spec)
{
    g->arg = NULL;
    if (g->pos == 0) {
        if (g->index >= g->argc)
            return -1;
        const char *a = g->argv[g->index];
        if (a[0] != '-' || a[1] == 0 || is_digit((uint8_t)a[1]))
            return -1;
        if (strcmp(a, "--") == 0) {
            g->index++;
            return -1;
        }
        g->pos = 1;
    }
    const char *a = g->argv[g->index];
    char c = a[g->pos++];
    const char *s = strchr(spec, c);
    g->opt = c;
    if (!s || c == ':') {
        if (!a[g->pos]) {
            g->index++;
            g->pos = 0;
        }
        return '?';
    }
    if (s[1] == ':') {
        if (a[g->pos]) {
            g->arg = a + g->pos;
        } else if (g->index + 1 < g->argc) {
            g->arg = g->argv[++g->index];
        } else {
            g->index++;
            g->pos = 0;
            return ':';
        }
        g->index++;
        g->pos = 0;
        return c;
    }
    if (!a[g->pos]) {
        g->index++;
        g->pos = 0;
    }
    return c;
}

int getopt_flags(struct shell *sh, int argc, char **argv, const char *spec, uint64_t *flags)
{
    struct getopt g;
    getopt_init(&g, argc, argv);
    *flags = 0;
    int c;
    while ((c = getopt_next(&g, spec)) != -1) {
        if (c == '?' || c == ':') {
            sh_error(sh, argv[0], "invalid option -- '%c'", g.opt);
            return -1;
        }
        *flags |= OPT(c);
    }
    return g.index;
}
