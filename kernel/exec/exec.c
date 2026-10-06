#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

SYNTH_TABLE_DECLARE(synth_binfmts, struct binfmt);

const struct binfmt *exec_identify(const char *path, const uint8_t *head, size_t n)
{
    SYNTH_TABLE_FOREACH(synth_binfmts, struct binfmt, fmt)
        if (fmt->probe(path, head, n))
            return fmt;
    return NULL;
}

void exec_foreach_format(void (*fn)(const struct binfmt *fmt, void *ctx), void *ctx)
{
    SYNTH_TABLE_FOREACH(synth_binfmts, struct binfmt, fmt)
        fn(fmt, ctx);
}

bool exec_is_executable_name(const char *name)
{
    const char *ext = path_ext(name);
    if (!*ext)
        return false;
    SYNTH_TABLE_FOREACH(synth_binfmts, struct binfmt, fmt) {
        const char *list = fmt->extensions;
        while (list && *list) {
            const char *comma = strchr(list, ',');
            size_t n = comma ? (size_t)(comma - list) : strlen(list);
            if (strlen(ext) == n && str_ncasecmp(ext, list, n) == 0)
                return true;
            list = comma ? comma + 1 : NULL;
        }
    }
    return false;
}

static bool is_file(const char *path)
{
    struct vstat st;
    return vfs_stat(path, &st) == 0 && st.type == VN_FILE;
}

static bool try_candidates(struct shell *sh, const char *dir, const char *name, char *out, size_t cap)
{
    char base[PATH_MAX_LEN];
    if (path_resolve(dir, name, base, sizeof(base)) != 0)
        return false;
    if (exec_is_executable_name(base) && is_file(base)) {
        str_copy(out, base, cap);
        return true;
    }
    const char *exts = sh_get(sh, "PATHEXT");
    if (!exts)
        exts = ".sx;.com;.exe;.sh";
    while (*exts) {
        const char *sep = strchr(exts, ';');
        size_t n = sep ? (size_t)(sep - exts) : strlen(exts);
        char candidate[PATH_MAX_LEN];
        ksnprintf(candidate, sizeof(candidate), "%s%.*s", base, (int)n, exts);
        if (is_file(candidate)) {
            str_copy(out, candidate, cap);
            return true;
        }
        if (!sep)
            break;
        exts = sep + 1;
    }
    return false;
}

int exec_find(struct shell *sh, const char *name, char *out, size_t cap)
{
    if (!*name)
        return E_NOENT;
    if (strchr(name, '/')) {
        char path[PATH_MAX_LEN];
        int err = sh_resolve(sh, name, path, sizeof(path));
        if (err)
            return err;
        if (is_file(path)) {
            str_copy(out, path, cap);
            return 0;
        }
        return try_candidates(sh, "/", path, out, cap) ? 0 : E_NOENT;
    }
    if (try_candidates(sh, sh->cwd, name, out, cap))
        return 0;
    const char *path = sh_get(sh, "PATH");
    while (path && *path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        char dir[PATH_MAX_LEN];
        if (n && n < sizeof(dir)) {
            memcpy(dir, path, n);
            dir[n] = 0;
            if (try_candidates(sh, dir, name, out, cap))
                return 0;
        }
        path = sep ? sep + 1 : NULL;
    }
    return E_NOENT;
}

int exec_program(struct exec_ctx *ctx)
{
    char *image;
    size_t size;
    int err = vfs_read_all(ctx->path, &image, &size);
    if (err)
        return err;
    const struct binfmt *fmt = exec_identify(ctx->path, (const uint8_t *)image, size);
    int rc = fmt ? fmt->run(ctx, (uint8_t *)image, size) : E_NOEXEC;
    kfree(image);
    return rc;
}
