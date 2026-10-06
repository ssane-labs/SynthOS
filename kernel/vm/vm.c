#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/lineedit.h>
#include <synth/string.h>
#include <synth/sx.h>
#include <synth/vfs.h>

#define VM_MAX_FILES 16
#define VM_POLL_INTERVAL 8192
#define VM_EXIT_FAULT 139
#define VM_EXIT_BREAK 130

struct vm {
    uint32_t r[SX_REG_COUNT];
    uint32_t pc;
    bool zf, lf, bf;
    uint8_t *mem;
    uint32_t size;
    uint32_t brk;
    uint32_t brk_limit;
    struct exec_ctx *ctx;
    struct file *files[VM_MAX_FILES];
    bool running;
    int status;
    char fault[96];
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void vm_fault(struct vm *vm, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void vm_fault(struct vm *vm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(vm->fault, sizeof(vm->fault), fmt, ap);
    va_end(ap);
    vm->running = false;
    vm->status = VM_EXIT_FAULT;
}

static inline bool range_ok(struct vm *vm, uint32_t addr, uint32_t n)
{
    if (UNLIKELY(addr < SX_LOAD_BASE || addr > vm->size || n > vm->size - addr)) {
        vm_fault(vm, "invalid memory access at 0x%x (pc 0x%x)", addr, vm->pc);
        return false;
    }
    return true;
}

static uint8_t *ptr(struct vm *vm, uint32_t addr, uint32_t n)
{
    return range_ok(vm, addr, n) ? vm->mem + addr : NULL;
}

static const char *vm_str(struct vm *vm, uint32_t addr)
{
    if (!range_ok(vm, addr, 1))
        return NULL;
    const uint8_t *p = vm->mem + addr;
    if (!memchr(p, 0, vm->size - addr)) {
        vm_fault(vm, "unterminated string at 0x%x", addr);
        return NULL;
    }
    return (const char *)p;
}

static void push(struct vm *vm, uint32_t v)
{
    vm->r[SX_REG_SP] -= 4;
    uint8_t *p = ptr(vm, vm->r[SX_REG_SP], 4);
    if (p && vm->r[SX_REG_SP] >= vm->brk)
        wr32(p, v);
    else if (p)
        vm_fault(vm, "stack overflow");
}

static uint32_t pop(struct vm *vm)
{
    uint8_t *p = ptr(vm, vm->r[SX_REG_SP], 4);
    vm->r[SX_REG_SP] += 4;
    return p ? rd32(p) : 0;
}

static struct stream *fd_stream(struct vm *vm, uint32_t fd)
{
    return fd == 0 ? vm->ctx->in : fd == 1 ? vm->ctx->out : fd == 2 ? vm->ctx->err : NULL;
}

static bool console_out(struct vm *vm)
{
    return stream_is_console(vm->ctx->out);
}

static int resolve(struct vm *vm, uint32_t addr, char *out)
{
    const char *path = vm_str(vm, addr);
    if (!path)
        return E_FAULT;
    return sh_resolve(vm->ctx->sh, path, out, PATH_MAX_LEN);
}

static void syscall(struct vm *vm, uint32_t num)
{
    uint32_t *r = vm->r;
    uint32_t a1 = r[1], a2 = r[2], a3 = r[3];
    char path[PATH_MAX_LEN];
    char tmp[64];
    uint8_t *p;
    const char *s;
    struct stream *st;
    int err;
    switch (num) {
    case SYS_EXIT:
        vm->status = (int)a1;
        vm->running = false;
        break;
    case SYS_PUTC: {
        char c = (char)a1;
        stream_write(vm->ctx->out, &c, 1);
        break;
    }
    case SYS_PUTS:
        if ((s = vm_str(vm, a1)))
            stream_puts(vm->ctx->out, s);
        break;
    case SYS_WRITE:
        if (!(p = ptr(vm, a2, a3)))
            break;
        if ((st = fd_stream(vm, a1)))
            r[0] = (uint32_t)stream_write(st, p, a3);
        else if (a1 < VM_MAX_FILES && vm->files[a1])
            r[0] = (uint32_t)vfs_write(vm->files[a1], p, a3);
        else
            r[0] = (uint32_t)E_BADF;
        break;
    case SYS_READ:
        if (!(p = ptr(vm, a2, a3)))
            break;
        if ((st = fd_stream(vm, a1)))
            r[0] = (uint32_t)stream_read(st, p, a3);
        else if (a1 < VM_MAX_FILES && vm->files[a1])
            r[0] = (uint32_t)vfs_read(vm->files[a1], p, a3);
        else
            r[0] = (uint32_t)E_BADF;
        break;
    case SYS_OPEN: {
        static const unsigned modes[] = {VFS_READ, VFS_WRITE | VFS_CREATE | VFS_TRUNC,
                                         VFS_WRITE | VFS_CREATE | VFS_APPEND, VFS_RDWR | VFS_CREATE};
        if ((err = resolve(vm, a1, path)) != 0 || a2 > 3) {
            r[0] = (uint32_t)(err ? err : E_INVAL);
            break;
        }
        int fd = 3;
        while (fd < VM_MAX_FILES && vm->files[fd])
            fd++;
        if (fd == VM_MAX_FILES) {
            r[0] = (uint32_t)E_MFILE;
            break;
        }
        err = vfs_open(path, modes[a2], &vm->files[fd]);
        r[0] = err ? (uint32_t)err : (uint32_t)fd;
        break;
    }
    case SYS_CLOSE:
        if (a1 >= 3 && a1 < VM_MAX_FILES && vm->files[a1]) {
            vfs_close(vm->files[a1]);
            vm->files[a1] = NULL;
            r[0] = 0;
        } else {
            r[0] = (uint32_t)E_BADF;
        }
        break;
    case SYS_GETKEY:
        con_flush();
        r[0] = (uint32_t)input_getkey(-1);
        break;
    case SYS_POLLKEY:
        con_flush();
        r[0] = input_poll() ? (uint32_t)input_getkey(0) : 0;
        break;
    case SYS_PRINTI:
        stream_printf(vm->ctx->out, "%d", (int32_t)a1);
        break;
    case SYS_PRINTX:
        stream_printf(vm->ctx->out, "%x", a1);
        break;
    case SYS_COLOR:
        if (console_out(vm)) {
            uint8_t cur = con_attr();
            con_set_attr(ATTR((int32_t)a1 < 0 ? ATTR_FG(cur) : a1 & 15, (int32_t)a2 < 0 ? ATTR_BG(cur) : a2 & 15));
        }
        break;
    case SYS_ROLE:
        if (console_out(vm))
            con_role(a1 < ROLE_COUNT ? (enum theme_role)a1 : ROLE_TEXT);
        break;
    case SYS_GOTOXY:
        if (console_out(vm))
            con_goto((int)a1, (int)a2);
        break;
    case SYS_CLEAR:
        if (console_out(vm))
            con_clear();
        break;
    case SYS_SCREEN:
        r[0] = (uint32_t)con_cols();
        r[1] = (uint32_t)con_rows();
        break;
    case SYS_TICKS:
        r[0] = (uint32_t)hal_ticks_ms();
        break;
    case SYS_SLEEP: {
        con_flush();
        uint64_t end = hal_ticks_ms() + a1;
        while (hal_ticks_ms() < end && !input_break_pending()) {
            hal_sleep_ms((uint32_t)MIN(end - hal_ticks_ms(), (uint64_t)10));
            input_poll();
        }
        break;
    }
    case SYS_RAND:
        r[0] = hal_entropy();
        break;
    case SYS_SBRK: {
        uint32_t old = vm->brk;
        if ((int32_t)a1 < 0 || vm->brk + a1 > vm->brk_limit || vm->brk + a1 > vm->r[SX_REG_SP] - 1024) {
            r[0] = 0;
            break;
        }
        vm->brk += a1;
        memset(vm->mem + old, 0, a1);
        r[0] = old;
        break;
    }
    case SYS_READLINE: {
        if (!(p = ptr(vm, a1, a2)) || a2 == 0)
            break;
        long n;
        if (stream_is_console(vm->ctx->in)) {
            n = lineedit_simple((char *)p, a2);
        } else {
            n = stream_getline(vm->ctx->in, (char *)p, a2);
        }
        r[0] = (uint32_t)(n < 0 ? -1 : n);
        break;
    }
    case SYS_PUTCELL:
        con_put_cell((int)a1, (int)a2, (uint8_t)a3, (uint8_t)r[4]);
        break;
    case SYS_FLUSH:
        con_flush();
        break;
    case SYS_CURSOR:
        con_show_cursor(a1 != 0);
        con_flush();
        break;
    case SYS_EXEC:
        if ((s = vm_str(vm, a1))) {
            char *line = kstrdup(s);
            r[0] = line ? (uint32_t)shell_exec(vm->ctx->sh, line) : 1;
            kfree(line);
        }
        break;
    case SYS_TIME: {
        if (!(p = ptr(vm, a1, 24)))
            break;
        struct rtc_time t;
        hal_rtc(&t);
        uint32_t v[6] = {t.year, t.month, t.day, t.hour, t.minute, t.second};
        for (int i = 0; i < 6; i++)
            wr32(p + i * 4, v[i]);
        break;
    }
    case SYS_GETCWD:
        if ((p = ptr(vm, a1, a2)) && a2)
            r[0] = (uint32_t)str_copy((char *)p, vm->ctx->sh->cwd, a2);
        break;
    case SYS_CHDIR:
        if ((s = vm_str(vm, a1)))
            r[0] = (uint32_t)sh_chdir(vm->ctx->sh, s);
        break;
    case SYS_MKDIR:
        r[0] = (uint32_t)((err = resolve(vm, a1, path)) ? err : vfs_mkdir(path));
        break;
    case SYS_REMOVE:
        r[0] = (uint32_t)((err = resolve(vm, a1, path)) ? err : vfs_unlink(path));
        break;
    case SYS_GETENV:
        if ((s = vm_str(vm, a1)) && (p = ptr(vm, a2, a3)) && a3) {
            const char *v = sh_get(vm->ctx->sh, s);
            r[0] = v ? (uint32_t)str_copy((char *)p, v, a3) : (uint32_t)-1;
        }
        break;
    case SYS_BEEP:
        hal_beep(a1, a2);
        break;
    case SYS_MEMCPY:
        if (ptr(vm, a1, a3) && ptr(vm, a2, a3))
            memmove(vm->mem + a1, vm->mem + a2, a3);
        break;
    case SYS_MEMSET:
        if ((p = ptr(vm, a1, a3)))
            memset(p, (int)a2, a3);
        break;
    case SYS_ITOA: {
        if (a3 == 10)
            ksnprintf(tmp, sizeof(tmp), "%d", (int32_t)a1);
        else if (a3 == 16)
            ksnprintf(tmp, sizeof(tmp), "%x", a1);
        else if (a3 == 2)
            ksnprintf(tmp, sizeof(tmp), "%b", a1);
        else
            ksnprintf(tmp, sizeof(tmp), "%u", a1);
        size_t n = strlen(tmp) + 1;
        if ((p = ptr(vm, a2, (uint32_t)n)))
            memcpy(p, tmp, n);
        r[0] = (uint32_t)(n - 1);
        break;
    }
    case SYS_ATOI:
        if ((s = vm_str(vm, a1)))
            r[0] = (uint32_t)str_to_long(s, NULL, 0);
        break;
    case SYS_SEEK:
        r[0] = a1 >= 3 && a1 < VM_MAX_FILES && vm->files[a1] ? (uint32_t)vfs_seek(vm->files[a1], (int32_t)a2, (int)a3)
                                                             : (uint32_t)E_BADF;
        break;
    case SYS_STAT: {
        struct vstat vs;
        if ((err = resolve(vm, a1, path)) == 0 && (err = vfs_stat(path, &vs)) == 0 && (p = ptr(vm, a2, 12))) {
            wr32(p, vs.type);
            wr32(p + 4, vs.size);
            wr32(p + 8, vs.mtime);
        }
        r[0] = (uint32_t)err;
        break;
    }
    case SYS_READDIR: {
        struct vdir d;
        struct vfs_dirent ent;
        if ((err = resolve(vm, a1, path)) == 0 && (err = vfs_opendir(path, &d)) == 0) {
            for (uint32_t i = 0; i <= a2 && err == 0; i++)
                err = vfs_readdir(&d, &ent);
            vfs_closedir(&d);
            if (err == 0 && (p = ptr(vm, a3, NAME_MAX_LEN)))
                str_copy((char *)p, ent.name, NAME_MAX_LEN);
        }
        r[0] = err ? (uint32_t)err : ent.type;
        break;
    }
    default:
        vm_fault(vm, "unknown system call %u", num);
        break;
    }
}

static bool cond(struct vm *vm, uint8_t op)
{
    switch (op) {
    case SX_JMP: return true;
    case SX_JZ: return vm->zf;
    case SX_JNZ: return !vm->zf;
    case SX_JL: return vm->lf;
    case SX_JGE: return !vm->lf;
    case SX_JG: return !vm->lf && !vm->zf;
    case SX_JLE: return vm->lf || vm->zf;
    case SX_JB: return vm->bf;
    case SX_JAE: return !vm->bf;
    case SX_JA: return !vm->bf && !vm->zf;
    case SX_JBE: return vm->bf || vm->zf;
    default: return false;
    }
}

static void run(struct vm *vm)
{
    uint32_t *r = vm->r;
    uint32_t budget = VM_POLL_INTERVAL;
    while (vm->running) {
        if (UNLIKELY(--budget == 0)) {
            budget = VM_POLL_INTERVAL;
            input_poll();
            if (input_break_pending()) {
                vm->status = VM_EXIT_BREAK;
                break;
            }
        }
        if (UNLIKELY(vm->pc < SX_LOAD_BASE || vm->pc > vm->size - 4)) {
            vm_fault(vm, "jump outside program memory (pc 0x%x)", vm->pc);
            break;
        }
        const uint8_t *code = vm->mem + vm->pc;
        uint8_t op = code[0] & 0x7f;
        bool imm_flag = code[0] & SX_IMM_FLAG;
        uint8_t a = code[1] & 15, b = code[2], c = code[3] & 15;
        uint32_t size = 4, imm = 0;
        enum sx_shape shape = op < SX_OPCODE_COUNT ? sx_ops[op].shape : SHAPE_NONE;
        if (imm_flag || shape == SHAPE_LOAD || shape == SHAPE_STORE || shape == SHAPE_SYS) {
            if (UNLIKELY(vm->pc > vm->size - 8)) {
                vm_fault(vm, "truncated instruction at 0x%x", vm->pc);
                break;
            }
            imm = rd32(code + 4);
            size = 8;
        }
        uint32_t next = vm->pc + size;
        uint32_t src = imm_flag ? imm : r[c];
        uint32_t base = b == SX_REG_NONE ? 0 : r[b & 15];
        uint8_t *m;
        switch (op) {
        case SX_NOP: break;
        case SX_HALT: vm->running = false; vm->status = (int)r[0]; break;
        case SX_MOV: r[a] = imm_flag ? imm : r[b & 15]; break;
        case SX_ADD: r[a] = r[b & 15] + src; break;
        case SX_SUB: r[a] = r[b & 15] - src; break;
        case SX_MUL: r[a] = r[b & 15] * src; break;
        case SX_DIV:
        case SX_MOD:
            if (src == 0 || ((int32_t)src == -1 && r[b & 15] == 0x80000000u)) {
                vm_fault(vm, "division by zero at 0x%x", vm->pc);
                break;
            }
            r[a] = op == SX_DIV ? (uint32_t)((int32_t)r[b & 15] / (int32_t)src)
                                : (uint32_t)((int32_t)r[b & 15] % (int32_t)src);
            break;
        case SX_DIVU:
        case SX_MODU:
            if (src == 0) {
                vm_fault(vm, "division by zero at 0x%x", vm->pc);
                break;
            }
            r[a] = op == SX_DIVU ? r[b & 15] / src : r[b & 15] % src;
            break;
        case SX_AND: r[a] = r[b & 15] & src; break;
        case SX_OR: r[a] = r[b & 15] | src; break;
        case SX_XOR: r[a] = r[b & 15] ^ src; break;
        case SX_SHL: r[a] = r[b & 15] << (src & 31); break;
        case SX_SHR: r[a] = r[b & 15] >> (src & 31); break;
        case SX_SAR: r[a] = (uint32_t)((int32_t)r[b & 15] >> (src & 31)); break;
        case SX_NOT: r[a] = ~r[b & 15]; break;
        case SX_NEG: r[a] = -r[b & 15]; break;
        case SX_SXB: r[a] = (uint32_t)(int32_t)(int8_t)r[b & 15]; break;
        case SX_SXH: r[a] = (uint32_t)(int32_t)(int16_t)r[b & 15]; break;
        case SX_CMP: {
            uint32_t x = r[a], y = imm_flag ? imm : r[b & 15];
            vm->zf = x == y;
            vm->lf = (int32_t)x < (int32_t)y;
            vm->bf = x < y;
            break;
        }
        case SX_JMP: case SX_JZ: case SX_JNZ: case SX_JL: case SX_JGE: case SX_JG:
        case SX_JLE: case SX_JB: case SX_JAE: case SX_JA: case SX_JBE:
            if (cond(vm, op))
                next = imm_flag ? imm : r[a];
            break;
        case SX_CALL:
            push(vm, next);
            next = imm_flag ? imm : r[a];
            break;
        case SX_RET: next = pop(vm); break;
        case SX_PUSH: push(vm, imm_flag ? imm : r[a]); break;
        case SX_POP: r[a] = pop(vm); break;
        case SX_LD: if ((m = ptr(vm, base + imm, 4))) r[a] = rd32(m); break;
        case SX_LDH: if ((m = ptr(vm, base + imm, 2))) r[a] = (uint32_t)(m[0] | m[1] << 8); break;
        case SX_LDB: if ((m = ptr(vm, base + imm, 1))) r[a] = m[0]; break;
        case SX_ST: if ((m = ptr(vm, base + imm, 4))) wr32(m, r[a]); break;
        case SX_STH: if ((m = ptr(vm, base + imm, 2))) m[0] = (uint8_t)r[a], m[1] = (uint8_t)(r[a] >> 8); break;
        case SX_STB: if ((m = ptr(vm, base + imm, 1))) m[0] = (uint8_t)r[a]; break;
        case SX_SYS: syscall(vm, imm); break;
        default:
            vm_fault(vm, "illegal instruction 0x%02x at 0x%x", code[0], vm->pc);
            break;
        }
        vm->pc = next;
    }
}

static bool sx_probe(const char *path, const uint8_t *head, size_t n)
{
    UNUSED(path);
    return n >= SX_HEADER_SIZE && memcmp(head, SX_MAGIC, 4) == 0;
}

static uint32_t setup_args(struct vm *vm, int argc, char **argv)
{
    uint32_t sp = vm->size;
    uint32_t ptrs[64];
    int n = MIN(argc, 64);
    for (int i = n - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        if (sp - len < vm->brk + 4096)
            break;
        sp -= len;
        memcpy(vm->mem + sp, argv[i], len);
        ptrs[i] = sp;
    }
    sp &= ~3u;
    sp -= 4 * (uint32_t)(n + 1);
    for (int i = 0; i < n; i++)
        wr32(vm->mem + sp + 4 * (uint32_t)i, ptrs[i]);
    wr32(vm->mem + sp + 4 * (uint32_t)n, 0);
    vm->r[1] = (uint32_t)n;
    vm->r[2] = sp;
    return sp & ~15u;
}

static int sx_run(struct exec_ctx *ctx, uint8_t *image, size_t size)
{
    uint32_t entry = rd32(image + 8), length = rd32(image + 12), mem = rd32(image + 16);
    if (image[4] != SX_VERSION || length > size - SX_HEADER_SIZE)
        return E_NOEXEC;
    if (mem < SX_LOAD_BASE + length + SX_MIN_STACK)
        mem = ALIGN_UP(SX_LOAD_BASE + length + SX_MIN_STACK, 4096u);
    if (mem > 64u * 1024 * 1024)
        return E_NOMEM;
    struct vm *vm = kzalloc(sizeof(*vm));
    if (!vm)
        return E_NOMEM;
    vm->mem = kzalloc(mem);
    if (!vm->mem) {
        kfree(vm);
        return E_NOMEM;
    }
    vm->size = mem;
    vm->ctx = ctx;
    memcpy(vm->mem + SX_LOAD_BASE, image + SX_HEADER_SIZE, length);
    vm->brk = ALIGN_UP(SX_LOAD_BASE + length, 16u);
    vm->r[SX_REG_SP] = setup_args(vm, ctx->argc, ctx->argv);
    vm->brk_limit = vm->r[SX_REG_SP] > SX_MIN_STACK ? vm->r[SX_REG_SP] - SX_MIN_STACK / 2 : vm->brk;
    vm->r[SX_REG_FP] = vm->r[SX_REG_SP];
    vm->pc = entry;
    vm->running = true;

    uint8_t saved_attr = con_attr();
    run(vm);
    con_show_cursor(true);
    con_set_attr(saved_attr);
    con_flush();

    if (vm->fault[0])
        sh_error(ctx->sh, path_basename(ctx->path), "%s", vm->fault);
    for (int i = 0; i < VM_MAX_FILES; i++)
        if (vm->files[i])
            vfs_close(vm->files[i]);
    int status = vm->status;
    kfree(vm->mem);
    kfree(vm);
    return status < 0 ? 255 : status;
}

static const struct binfmt sx_format = {
    .name = "sx",
    .description = "SynthOS executable (SX VM)",
    .extensions = ".sx",
    .probe = sx_probe,
    .run = sx_run,
};
BINFMT(sx_format);
