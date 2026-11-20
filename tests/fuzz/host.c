/* What the kernel code under test calls, on the host: the heap is malloc, so AddressSanitizer sees every block. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

void *kzalloc(size_t size)
{
    return calloc(1, size ? size : 1);
}

void *krealloc(void *p, size_t size)
{
    return realloc(p, size ? size : 1);
}

void kfree(void *p)
{
    free(p);
}

char *kstrdup(const char *s)
{
    return strdup(s);
}

char *kstrndup(const char *s, size_t n)
{
    return strndup(s, n);
}

void *kmalloc_pages_uninit(size_t size)
{
    return malloc(size);
}

void kfree_pages(void *p)
{
    free(p);
}

void klog(const char *fmt, ...)
{
    (void)fmt;
}

/* the shell, files and input the archive and document modules know of: nothing is there on the host */
#include <synth/command.h>
#include <synth/error.h>
#include <synth/hal.h>
#include <synth/input.h>
#include <synth/shell.h>
#include <synth/time.h>
#include <synth/vfs.h>

void hal_parallel(void (*fn)(void *ctx, int from, int to), void *ctx, int count, int grain)
{
    (void)grain;
    fn(ctx, 0, count);
}

bool input_break_pending(void)
{
    return false;
}

void input_yield(void)
{
}

void sh_puts(struct shell *sh, const char *s)
{
    (void)sh, (void)s;
}

int sh_printf(struct shell *sh, const char *fmt, ...)
{
    (void)sh;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return n;
}

int sh_error(struct shell *sh, const char *who, const char *fmt, ...)
{
    (void)sh, (void)who, (void)fmt;
    return 1;
}

int sh_fs_error(struct shell *sh, const char *who, const char *path, int err)
{
    (void)sh, (void)who, (void)path;
    return err;
}

int sh_resolve(struct shell *sh, const char *path, char *out, size_t cap)
{
    (void)sh;
    snprintf(out, cap, "%s", path);
    return 0;
}

const struct command *command_find(const char *name)
{
    (void)name;
    return NULL;
}

int command_usage(struct shell *sh, const struct command *cmd)
{
    (void)sh, (void)cmd;
    return 1;
}

const char *err_str(int code)
{
    (void)code;
    return "error";
}

void time_local(struct rtc_time *t)
{
    memset(t, 0, sizeof(*t));
}

int vfs_mkdirs(const char *path)
{
    (void)path;
    return E_INVAL;
}

bool vfs_exists(const char *path)
{
    (void)path;
    return false;
}

bool vfs_is_dir(const char *path)
{
    (void)path;
    return false;
}

int vfs_opendir(const char *path, struct vdir *dir)
{
    (void)path, (void)dir;
    return E_INVAL;
}

int vfs_readdir(struct vdir *dir, struct vfs_dirent *out)
{
    (void)dir, (void)out;
    return E_INVAL;
}

void vfs_closedir(struct vdir *dir)
{
    (void)dir;
}

int vfs_read_all(const char *path, char **data, size_t *size)
{
    (void)path, (void)data, (void)size;
    return E_INVAL;
}

int vfs_write_all(const char *path, const void *data, size_t size)
{
    (void)path, (void)data, (void)size;
    return E_INVAL;
}
