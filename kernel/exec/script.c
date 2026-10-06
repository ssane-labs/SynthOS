#include <synth/exec.h>
#include <synth/string.h>

static bool script_probe(const char *path, const uint8_t *head, size_t n)
{
    if (n >= 2 && head[0] == '#' && head[1] == '!')
        return true;
    return str_iends(path, ".sh") || str_iends(path, ".syn");
}

static int script_run(struct exec_ctx *ctx, uint8_t *image, size_t size)
{
    UNUSED(size);
    return shell_exec_script(ctx->sh, (const char *)image, ctx->argc, ctx->argv);
}

static const struct binfmt script_format = {
    .name = "script",
    .description = "SynthOS shell script",
    .extensions = ".sh,.syn",
    .probe = script_probe,
    .run = script_run,
};
BINFMT(script_format);
