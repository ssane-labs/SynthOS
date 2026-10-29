#include <synth/config.h>
#if CONFIG_WIN32
#include <synth/exec.h>
#include <synth/inflate.h>
#include <synth/string.h>
#include <synth/winshell.h>
#include <synth/vfs.h>
#include <synth/vmm.h>

/* The Windows subsystem is x32 code: 4-byte pointers, size_t and long. The kernel functions it calls that write a
   pointer or a size_t through a pointer, read an array of pointers or take a signed long are reached through these
   (arch/x86/64/x32.syms renames its references), and the kernel reaches its functions that take an argv or write a
   pointer through these. */

int x32k_vfs_open(const char *path, unsigned flags, uint32_t *out)
{
    struct file *f = (struct file *)(uintptr_t)*out;
    int r = vfs_open(path, flags, &f);
    *out = (uint32_t)(uintptr_t)f;
    return r;
}

int x32k_vfs_read_all(const char *path, uint32_t *data, uint32_t *size)
{
    char *d = (char *)(uintptr_t)*data;
    size_t n = *size;
    int r = vfs_read_all(path, &d, &n);
    *data = (uint32_t)(uintptr_t)d;
    *size = (uint32_t)n;
    return r;
}

long x32k_vfs_seek(struct file *f, int32_t off, int whence)
{
    return vfs_seek(f, off, whence);
}

int32_t x32k_str_to_long(const char *s, uint32_t *end, int base)
{
    const char *e = end ? (const char *)(uintptr_t)*end : NULL;
    long v = str_to_long(s, end ? &e : NULL, base);
    if (end)
        *end = (uint32_t)(uintptr_t)e;
    return (int32_t)v;
}

bool x32k_str_parse_long(const char *s, int32_t *out)
{
    long v = *out;
    bool ok = str_parse_long(s, &v);
    *out = (int32_t)v;
    return ok;
}

int x32k_inflate_into(const void *in, uint32_t in_len, void *out, uint32_t out_cap, uint32_t *out_len, uint32_t *used)
{
    size_t n = out_len ? *out_len : 0, u = used ? *used : 0;
    int r = inflate_into(in, in_len, out, out_cap, out_len ? &n : NULL, used ? &u : NULL);
    if (out_len)
        *out_len = (uint32_t)n;
    if (used)
        *used = (uint32_t)u;
    return r;
}

int x32k_vmm_map_pages(uintptr_t vaddr, const uint32_t *pages, size_t count, bool cow)
{
    for (size_t i = 0; i < count; i++)
        if (vmm_map_pages(vaddr + i * 4096, (void *const[]){(void *)(uintptr_t)pages[i]}, 1, cow)) {
            vmm_unmap(vaddr, i * 4096);
            return -1;
        }
    return 0;
}

int x32_w32_start(struct shell *sh, const char *path, int argc, const uint32_t *argv, const char *cwd);
int x32_msiexec_main(struct shell *sh, int argc, const uint32_t *argv);
bool x32_winshell_split(const char *cmdline, char *program, size_t cap, uint32_t *args);

static void narrow_argv(uint32_t *out, int argc, char **argv)
{
    for (int i = 0; i < argc; i++)
        out[i] = (uint32_t)(uintptr_t)argv[i];
    out[argc] = 0;
}

int w32_start(struct shell *sh, const char *path, int argc, char **argv, const char *cwd)
{
    uint32_t v[argc + 1];
    narrow_argv(v, argc, argv);
    return x32_w32_start(sh, path, argc, v, cwd);
}

int msiexec_main(struct shell *sh, int argc, char **argv)
{
    uint32_t v[argc + 1];
    narrow_argv(v, argc, argv);
    return x32_msiexec_main(sh, argc, v);
}

bool winshell_split(const char *cmdline, char *program, size_t cap, const char **args)
{
    uint32_t a = args ? (uint32_t)(uintptr_t)*args : 0;
    bool ok = x32_winshell_split(cmdline, program, cap, args ? &a : NULL);
    if (args)
        *args = (const char *)(uintptr_t)a;
    return ok;
}
#endif
