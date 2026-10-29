#ifndef SYNTH_SHELL_H
#define SYNTH_SHELL_H

#include <synth/path.h>
#include <synth/stream.h>
#include <synth/theme.h>
#include <synth/types.h>

#define SH_MAX_ARGS 256
#define SH_MAX_DEPTH 32

enum sh_control {
    CTL_NONE,
    CTL_BREAK,
    CTL_CONTINUE,
    CTL_RETURN,
    CTL_EXIT,
};

struct strmap_entry {
    K64(char *key);
    K64(char *value);
};

struct strmap {
    K64(struct strmap_entry *items);
    K64(size_t count);
    K64(size_t cap);
};

struct sh_function {
    K64(char *name);
    K64(char *body);
};

struct history {
    K64(KARR(char *) *items);
    K64(size_t count);
    K64(size_t max);
};

struct shell {
    char cwd[PATH_MAX_LEN];
    K64(struct stream *in);
    K64(struct stream *out);
    K64(struct stream *err);
    int status;
    enum sh_control ctl;
    int loop_depth;
    int call_depth;
    bool interactive;
    bool idle;
    struct strmap vars;
    struct strmap aliases;
    K64(struct sh_function *funcs);
    K64(size_t func_count);
    struct history history;
    int argc;
    K64(KARR(char *) *argv);
};

struct setting {
    K64(const char *name);
    K64(const char *values);
    K64(const char *summary);
    K64(void (*get)(char *buf, size_t cap));
    K64(int (*set)(struct shell *sh, const char *value));
};

#define SETTING(object) SYNTH_REGISTER(synth_settings, struct setting, object)

void strmap_set(struct strmap *m, const char *key, const char *value);
const char *strmap_get(const struct strmap *m, const char *key);
bool strmap_remove(struct strmap *m, const char *key);
void strmap_sort(struct strmap *m);

struct shell *shell_create(void);
struct shell *shell_current(void);
void shell_repl(struct shell *sh);
int shell_exec(struct shell *sh, const char *line);
int shell_exec_script(struct shell *sh, const char *text, int argc, char **argv);
int shell_source(struct shell *sh, const char *path, int argc, char **argv);
int shell_capture(struct shell *sh, const char *line, char **out);

const char *sh_get(struct shell *sh, const char *name);
void sh_set(struct shell *sh, const char *name, const char *value);
void sh_unset(struct shell *sh, const char *name);
int sh_resolve(struct shell *sh, const char *path, char *out, size_t cap);
int sh_chdir(struct shell *sh, const char *path);

int sh_printf(struct shell *sh, const char *fmt, ...) PRINTF_LIKE(2, 3);
void sh_puts(struct shell *sh, const char *s);
void sh_role(struct shell *sh, enum theme_role role);
void sh_reset(struct shell *sh);
int sh_error(struct shell *sh, const char *who, const char *fmt, ...) PRINTF_LIKE(3, 4);
int sh_fail(struct shell *sh, int code, const char *who, const char *fmt, ...) PRINTF_LIKE(4, 5);
int sh_fs_error(struct shell *sh, const char *who, const char *path, int err);

struct sh_function *sh_function_find(struct shell *sh, const char *name);
void sh_function_define(struct shell *sh, const char *name, const char *body);
int sh_function_call(struct shell *sh, struct sh_function *fn, int argc, char **argv);

void history_add(struct history *h, const char *line);
const char *history_get(const struct history *h, size_t index);
int history_load(struct history *h, const char *path);
int history_save(const struct history *h, const char *path);

const struct setting *setting_find(const char *name);
int setting_apply(struct shell *sh, const char *name, const char *value);
int settings_save(const char *path);
int setting_flag(const char *value, bool *out);
void setting_flag_get(bool value, char *buf, size_t cap);

void prompt_render(struct shell *sh);
const char *prompt_preset(const char *name);
void prompt_presets(void (*fn)(const char *name, const char *format, void *ctx), void *ctx);

bool sh_interrupted(void);
int calc_eval(struct shell *sh, const char *expr, long *out, char *err, size_t errcap);
void shell_banner(void);

#endif
