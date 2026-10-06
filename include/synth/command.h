#ifndef SYNTH_COMMAND_H
#define SYNTH_COMMAND_H

#include <synth/shell.h>

typedef int (*command_fn)(struct shell *sh, int argc, char **argv);

struct command {
    const char *name;
    const char *aliases;
    const char *category;
    const char *usage;
    const char *summary;
    command_fn run;
};

#define COMMAND(id, ...)                                                           \
    static int cmd_##id(struct shell *sh, int argc, char **argv);                  \
    static const struct command cmddef_##id = {#id, __VA_ARGS__, cmd_##id};        \
    SYNTH_REGISTER(synth_commands, struct command, cmddef_##id);                  \
    static int cmd_##id(struct shell *sh, int argc, char **argv)

const struct command *command_find(const char *name);
void command_foreach(void (*fn)(const struct command *cmd, void *ctx), void *ctx);
int command_usage(struct shell *sh, const struct command *cmd);

struct getopt {
    int argc;
    char **argv;
    int index;
    int pos;
    int opt;
    const char *arg;
};

void getopt_init(struct getopt *g, int argc, char **argv);
int getopt_next(struct getopt *g, const char *spec);
int getopt_flags(struct shell *sh, int argc, char **argv, const char *spec, uint64_t *flags);

#define OPT(c) (UINT64_C(1) << ((c) & 63))

#endif
