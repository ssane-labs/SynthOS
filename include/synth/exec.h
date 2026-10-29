#ifndef SYNTH_EXEC_H
#define SYNTH_EXEC_H

#include <synth/shell.h>

struct exec_ctx {
    K64(struct shell *sh);
    K64(const char *path);
    int argc;
    K64(KARR(char *) *argv);
    K64(struct stream *in);
    K64(struct stream *out);
    K64(struct stream *err);
    K64(const char *env);
    K64(const char *cwd);
    K64(void *handles);
};

struct binfmt {
    K64(const char *name);
    K64(const char *description);
    K64(const char *extensions);
    K64(bool (*probe)(const char *path, const uint8_t *head, size_t n));
    K64(int (*run)(struct exec_ctx *ctx, uint8_t *image, size_t size));
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
