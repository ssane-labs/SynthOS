#ifndef SYNTH_EXEC_H
#define SYNTH_EXEC_H

#include <synth/shell.h>

struct exec_ctx {
    struct shell *sh;
    const char *path;
    int argc;
    char **argv;
    struct stream *in;
    struct stream *out;
    struct stream *err;
    const char *env;
    const char *cwd;
    void *handles;
};

struct binfmt {
    const char *name;
    const char *description;
    const char *extensions;
    bool (*probe)(const char *path, const uint8_t *head, size_t n);
    int (*run)(struct exec_ctx *ctx, uint8_t *image, size_t size);
};

#define BINFMT(object) SYNTH_REGISTER(synth_binfmts, struct binfmt, object)

int exec_program(struct exec_ctx *ctx);
int w32_start(struct shell *sh, const char *path, int argc, char **argv, const char *cwd);
void w32_background(void);
void w32_host_sleep(uint32_t ms);
int exec_find(struct shell *sh, const char *name, char *out, size_t cap);
const struct binfmt *exec_identify(const char *path, const uint8_t *head, size_t n);
void exec_foreach_format(void (*fn)(const struct binfmt *fmt, void *ctx), void *ctx);
bool exec_is_executable_name(const char *name);

#endif
