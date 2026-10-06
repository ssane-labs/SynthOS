#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/sx.h>
#include <synth/vfs.h>

struct include_ctx {
    char dir[PATH_MAX_LEN];
};

static int load_include(void *ctx, const char *path, char **text)
{
    struct include_ctx *ic = ctx;
    char full[PATH_MAX_LEN];
    int err = path_resolve(ic->dir, path, full, sizeof(full));
    if (err)
        return err;
    return vfs_read_all(full, text, NULL);
}

COMMAND(asm, "sasm", "dev", "asm source.sasm [-o output.sx]", "assemble a program for the SX virtual machine")
{
    const char *src = NULL, *dst = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            dst = argv[++i];
        else
            src = argv[i];
    }
    if (!src)
        return command_usage(sh, command_find("asm"));
    char in_path[PATH_MAX_LEN], out_path[PATH_MAX_LEN];
    int err = sh_resolve(sh, src, in_path, sizeof(in_path));
    char *text = NULL;
    if (!err)
        err = vfs_read_all(in_path, &text, NULL);
    if (err)
        return sh_fs_error(sh, "asm", src, err);
    if (dst) {
        err = sh_resolve(sh, dst, out_path, sizeof(out_path));
    } else {
        str_copy(out_path, in_path, sizeof(out_path));
        char *dot = strrchr(out_path, '.');
        if (dot && !strchr(dot, '/'))
            *dot = 0;
        str_append(out_path, ".sx", sizeof(out_path));
    }
    struct include_ctx ic;
    path_dirname(in_path, ic.dir, sizeof(ic.dir));
    struct sasm_options opts = {load_include, &ic};
    struct sasm_output out;
    int rc = sasm_assemble(path_basename(in_path), text, &opts, &out);
    kfree(text);
    if (rc != 0)
        return sh_error(sh, "asm", "%s", out.error);
    err = vfs_write_all(out_path, out.data, out.size);
    kfree(out.data);
    if (err)
        return sh_fs_error(sh, "asm", out_path, err);
    sh_role(sh, ROLE_OK);
    sh_printf(sh, "assembled %s", out_path);
    sh_reset(sh);
    sh_printf(sh, " (%u bytes, entry 0x%x)\n", (unsigned)out.size, out.entry);
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

COMMAND(dis, "disasm", "dev", "dis program.sx", "disassemble an SX program")
{
    if (argc != 2)
        return command_usage(sh, command_find("dis"));
    char path[PATH_MAX_LEN];
    char *data;
    size_t size;
    int err = sh_resolve(sh, argv[1], path, sizeof(path));
    if (!err)
        err = vfs_read_all(path, &data, &size);
    if (err)
        return sh_fs_error(sh, "dis", argv[1], err);
    const uint8_t *img = (const uint8_t *)data;
    if (size < SX_HEADER_SIZE || memcmp(img, SX_MAGIC, 4) != 0) {
        kfree(data);
        return sh_error(sh, "dis", "%s: not an SX program", argv[1]);
    }
    uint32_t entry = rd32(img + 8), length = MIN(rd32(img + 12), (uint32_t)(size - SX_HEADER_SIZE));
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "; SX v%u  entry 0x%x  code %u bytes  memory %u KB\n", img[4], entry, length,
              rd32(img + 16) / 1024);
    sh_reset(sh);
    const uint8_t *code = img + SX_HEADER_SIZE;
    for (uint32_t off = 0; off < length && !sh_interrupted();) {
        char text[64];
        uint32_t addr = SX_LOAD_BASE + off;
        size_t run = 0;
        while (off + run < length && code[off + run] >= 0x20 && code[off + run] < 0x7f)
            run++;
        if (run >= 4 && off + run < length && code[off + run] == 0) {
            sh_role(sh, ROLE_MUTED);
            sh_printf(sh, "%06x  %-16s  ", addr, "");
            sh_role(sh, ROLE_SYNTAX_STRING);
            sh_printf(sh, ".asciz \"%.*s\"\n", (int)MIN(run, (size_t)60), (const char *)code + off);
            sh_reset(sh);
            off += (uint32_t)run + 1;
            continue;
        }
        size_t n = sx_disasm(code + off, length - off, addr, text, sizeof(text));
        if (n == 0)
            break;
        sh_role(sh, addr == entry ? ROLE_ACCENT : ROLE_MUTED);
        sh_printf(sh, "%06x  ", addr);
        for (size_t i = 0; i < 8; i++)
            sh_printf(sh, i < n ? "%02x" : "  ", code[off + i < length ? off + i : 0]);
        sh_reset(sh);
        sh_printf(sh, "  %s\n", text);
        off += (uint32_t)n;
    }
    kfree(data);
    return 0;
}
