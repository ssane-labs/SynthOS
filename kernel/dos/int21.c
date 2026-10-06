#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/winpath.h>

#include "dos.h"

enum {
    DOS_OK = 0,
    DOS_EINVAL = 1,
    DOS_ENOFILE = 2,
    DOS_ENOPATH = 3,
    DOS_EMFILE = 4,
    DOS_EACCES = 5,
    DOS_EBADF = 6,
    DOS_ENOMEM = 8,
    DOS_EBADBLOCK = 9,
    DOS_EFORMAT = 11,
    DOS_ECURDIR = 16,
    DOS_ENOMORE = 18,
    DOS_EEXIST = 80,
};

enum {
    DEV_NONE,
    DEV_CON,
    DEV_NUL,
};

static uint16_t map_error(int err, bool path)
{
    switch (err) {
    case E_NOENT: return path ? DOS_ENOPATH : DOS_ENOFILE;
    case E_NOTDIR: return DOS_ENOPATH;
    case E_EXIST: return DOS_EEXIST;
    case E_MFILE: return DOS_EMFILE;
    case E_BADF: return DOS_EBADF;
    case E_NOMEM: return DOS_ENOMEM;
    case E_BUSY: return DOS_ECURDIR;
    default: return DOS_EACCES;
    }
}

void dos_read_string(struct dos *d, uint16_t seg, uint16_t off, char *out, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char ch = (char)cpu_rb(&d->cpu, cpu_lin(seg, (uint16_t)(off + n)));
        if (!ch)
            break;
        out[n++] = ch;
    }
    out[n] = 0;
}

static void write_string(struct dos *d, uint16_t seg, uint16_t off, const char *s)
{
    do
        cpu_wb(&d->cpu, cpu_lin(seg, off++), (uint8_t)*s);
    while (*s++);
}

static int device_of(const char *path)
{
    char base[16];
    str_copy(base, path_basename(path), sizeof(base));
    char *dot = strchr(base, '.');
    if (dot)
        *dot = 0;
    if (str_casecmp(base, "CON") == 0)
        return DEV_CON;
    if (str_casecmp(base, "NUL") == 0 || str_casecmp(base, "PRN") == 0 || str_casecmp(base, "AUX") == 0 ||
        str_casecmp(base, "LPT1") == 0 || str_casecmp(base, "COM1") == 0)
        return DEV_NUL;
    return DEV_NONE;
}

int dos_resolve(struct dos *d, const char *dos_path, char *out, size_t cap)
{
    return winpath_to_vfs(d->cwd, dos_path, out, cap);
}

static int read_path(struct dos *d, uint16_t seg, uint16_t off, char *out, int *device)
{
    char raw[PATH_MAX_LEN];
    dos_read_string(d, seg, off, raw, sizeof(raw));
    if (device)
        *device = device_of(raw);
    return dos_resolve(d, raw, out, PATH_MAX_LEN);
}

void dos_handles_init(struct dos *d)
{
    static const enum dos_handle_kind std[5] = {H_STDIN, H_STDOUT, H_STDERR, H_NUL, H_NUL};
    for (int i = 0; i < 5; i++)
        d->handles[i] = (struct dos_handle){std[i], NULL, 0};
}

void dos_handle_release(struct dos *d, int index)
{
    struct dos_handle *h = &d->handles[index];
    struct file *f = h->kind == H_FILE ? h->file : NULL;
    h->kind = H_FREE;
    h->file = NULL;
    if (!f)
        return;
    for (int i = 0; i < DOS_MAX_HANDLES; i++)
        if (d->handles[i].kind == H_FILE && d->handles[i].file == f)
            return;
    vfs_close(f);
}

void dos_handles_close_owner(struct dos *d, uint16_t owner)
{
    for (int i = 5; i < DOS_MAX_HANDLES; i++)
        if (d->handles[i].kind != H_FREE && d->handles[i].owner == owner)
            dos_handle_release(d, i);
}

static int new_handle(struct dos *d, enum dos_handle_kind kind, struct file *f)
{
    for (int i = 5; i < DOS_MAX_HANDLES; i++) {
        if (d->handles[i].kind == H_FREE) {
            d->handles[i] = (struct dos_handle){kind, f, d->psp};
            return i;
        }
    }
    return -1;
}

static struct dos_handle *handle(struct dos *d, uint16_t h)
{
    if (h >= DOS_MAX_HANDLES || d->handles[h].kind == H_FREE)
        return NULL;
    return &d->handles[h];
}

static void open_file(struct dos *d, uint16_t seg, uint16_t off, unsigned mode, bool create, bool exclusive)
{
    struct cpu *c = &d->cpu;
    char path[PATH_MAX_LEN];
    int device;
    int err = read_path(d, seg, off, path, &device);
    if (err) {
        dos_fail(d, DOS_ENOPATH);
        return;
    }
    if (device) {
        int h = new_handle(d, device == DEV_CON ? H_STDOUT : H_NUL, NULL);
        if (h < 0) {
            dos_fail(d, DOS_EMFILE);
            return;
        }
        c->r[AX] = (uint16_t)h;
        dos_ok(d);
        return;
    }
    unsigned flags = (mode & 3) == 0 ? VFS_READ : (mode & 3) == 1 ? VFS_WRITE : VFS_RDWR;
    if (create)
        flags = VFS_RDWR | VFS_CREATE | (exclusive ? VFS_EXCL : VFS_TRUNC);
    if (vfs_is_dir(path)) {
        dos_fail(d, DOS_EACCES);
        return;
    }
    struct file *f;
    err = vfs_open(path, flags, &f);
    if (err) {
        dos_fail(d, map_error(err, false));
        return;
    }
    int h = new_handle(d, H_FILE, f);
    if (h < 0) {
        vfs_close(f);
        dos_fail(d, DOS_EMFILE);
        return;
    }
    c->r[AX] = (uint16_t)h;
    dos_ok(d);
}

static long read_stdin(struct dos *d, uint8_t *buf, uint16_t n)
{
    if (!stream_is_console(d->ctx->in))
        return stream_read(d->ctx->in, buf, n);
    if (d->inpos >= d->inlen) {
        int len = dos_line_input(d, d->inbuf, (int)sizeof(d->inbuf) - 2);
        if (len == 1 && d->inbuf[0] == 0x1a)
            return 0;
        d->inbuf[len++] = '\r';
        d->inbuf[len++] = '\n';
        d->inlen = len;
        d->inpos = 0;
    }
    long k = MIN((long)n, (long)(d->inlen - d->inpos));
    memcpy(buf, d->inbuf + d->inpos, (size_t)k);
    d->inpos += (int)k;
    return k;
}

static uint8_t *linear(struct dos *d, uint16_t seg, uint16_t off, uint16_t n)
{
    uint32_t addr = cpu_lin(seg, off);
    if (addr + n > MEM_SIZE)
        return NULL;
    return d->mem + addr;
}

static void do_read(struct dos *d)
{
    struct cpu *c = &d->cpu;
    struct dos_handle *h = handle(d, c->r[BX]);
    uint8_t *buf = linear(d, c->s[DS], c->r[DX], c->r[CX]);
    if (!h || !buf) {
        dos_fail(d, DOS_EBADF);
        return;
    }
    long n = 0;
    if (h->kind == H_STDIN)
        n = read_stdin(d, buf, c->r[CX]);
    else if (h->kind == H_FILE)
        n = vfs_read(h->file, buf, c->r[CX]);
    if (n < 0) {
        dos_fail(d, DOS_EACCES);
        return;
    }
    c->r[AX] = (uint16_t)n;
    dos_ok(d);
}

static void do_write(struct dos *d)
{
    struct cpu *c = &d->cpu;
    struct dos_handle *h = handle(d, c->r[BX]);
    uint8_t *buf = linear(d, c->s[DS], c->r[DX], c->r[CX]);
    if (!h || !buf) {
        dos_fail(d, DOS_EBADF);
        return;
    }
    long n = c->r[CX];
    if (h->kind == H_STDOUT || h->kind == H_STDERR || h->kind == H_STDIN) {
        for (long i = 0; i < n; i++)
            dos_out(d, h->kind == H_STDERR ? H_STDERR : H_STDOUT, buf[i]);
    } else if (h->kind == H_FILE) {
        if (n == 0) {
            struct vstat st;
            vfs_fstat(h->file, &st);
            if (h->file->vn->ops->truncate)
                h->file->vn->ops->truncate(h->file->vn, h->file->pos);
        } else {
            n = vfs_write(h->file, buf, (size_t)n);
        }
    }
    if (n < 0) {
        dos_fail(d, DOS_EACCES);
        return;
    }
    c->r[AX] = (uint16_t)n;
    dos_ok(d);
}

static void dos_time(uint32_t epoch, uint16_t *time, uint16_t *date)
{
    struct rtc_time t;
    time_to_rtc(epoch, &t);
    *time = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
    *date = (uint16_t)((t.year < 1980 ? 0 : t.year - 1980) << 9 | t.month << 5 | t.day);
}

static bool to_83(const char *name, char *out)
{
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base == 0 || base > 8 || ext > 3 || (dot && strchr(name, '.') != dot))
        return false;
    for (const char *p = name; *p; p++)
        if (*p == ' ' || (uint8_t)*p < 0x20)
            return false;
    str_copy(out, name, 13);
    str_upper(out);
    return true;
}

static bool dos_match(const char *pattern, const char *name)
{
    if (strcmp(pattern, "*.*") == 0 || strcmp(pattern, "*") == 0)
        return true;
    if (wildcard_match(pattern, name, true))
        return true;
    size_t n = strlen(pattern);
    if (!strchr(name, '.') && n >= 2 && strcmp(pattern + n - 2, ".*") == 0) {
        char base[16];
        str_copy(base, pattern, MIN(sizeof(base), n - 1));
        return wildcard_match(base, name, true);
    }
    return false;
}

static void find_next(struct dos *d, int slot)
{
    struct dos_find *f = &d->finds[slot];
    uint8_t *dta = linear(d, d->dta_seg, d->dta_off, 43);
    struct vdir vd;
    if (!dta || !f->used || vfs_opendir(f->dir, &vd) != 0) {
        dos_fail(d, DOS_ENOMORE);
        return;
    }
    struct vfs_dirent ent;
    uint32_t index = 0;
    bool found = false;
    while (vfs_readdir(&vd, &ent) == 0) {
        if (index++ < f->index)
            continue;
        char name[13];
        if (!to_83(ent.name, name) || !dos_match(f->pattern, name))
            continue;
        if (ent.type == VN_DIR && !(f->attr & 0x10))
            continue;
        char full[PATH_MAX_LEN];
        struct vstat st = {0};
        path_join(f->dir, ent.name, full, sizeof(full));
        vfs_stat(full, &st);
        uint16_t time, date;
        dos_time(st.mtime, &time, &date);
        dta[0] = (uint8_t)(0x80 | slot);
        memcpy(dta + 1, &index, 4);
        dta[0x15] = ent.type == VN_DIR ? 0x10 : 0x20;
        dta[0x16] = (uint8_t)time;
        dta[0x17] = (uint8_t)(time >> 8);
        dta[0x18] = (uint8_t)date;
        dta[0x19] = (uint8_t)(date >> 8);
        uint32_t size = ent.type == VN_DIR ? 0 : st.size;
        memcpy(dta + 0x1a, &size, 4);
        memset(dta + 0x1e, 0, 13);
        memcpy(dta + 0x1e, name, strlen(name));
        f->index = index;
        found = true;
        break;
    }
    vfs_closedir(&vd);
    if (!found) {
        f->used = false;
        dos_fail(d, DOS_ENOMORE);
        return;
    }
    d->cpu.r[AX] = 0;
    dos_ok(d);
}

static void find_first(struct dos *d)
{
    static int next_slot;
    struct cpu *c = &d->cpu;
    char raw[PATH_MAX_LEN];
    dos_read_string(d, c->s[DS], c->r[DX], raw, sizeof(raw));
    for (char *p = raw; *p; p++)
        if (*p == '\\')
            *p = '/';
    const char *slash = strrchr(raw, '/');
    const char *pattern = slash ? slash + 1 : raw[0] && raw[1] == ':' ? raw + 2 : raw;
    char dir_part[PATH_MAX_LEN] = ".";
    if (slash) {
        size_t n = (size_t)(slash - raw);
        memcpy(dir_part, raw, n);
        dir_part[n] = 0;
        if (n == 0 || (n == 2 && raw[1] == ':'))
            str_copy(dir_part, "/", sizeof(dir_part));
    }
    if ((c->r[CX] & 0x08) && !(c->r[CX] & 0x10)) {
        dos_fail(d, DOS_ENOMORE);
        return;
    }
    int slot = next_slot++ % DOS_MAX_FINDS;
    struct dos_find *f = &d->finds[slot];
    if (dos_resolve(d, dir_part, f->dir, sizeof(f->dir)) != 0 || !vfs_is_dir(f->dir)) {
        dos_fail(d, DOS_ENOPATH);
        return;
    }
    str_copy(f->pattern, *pattern ? pattern : "*.*", sizeof(f->pattern));
    str_upper(f->pattern);
    f->attr = (uint8_t)c->r[CX];
    f->index = 0;
    f->used = true;
    find_next(d, slot);
}

static void dos_exec_call(struct dos *d)
{
    struct cpu *c = &d->cpu;
    uint8_t mode = AL(c);
    char path[PATH_MAX_LEN], raw[PATH_MAX_LEN];
    dos_read_string(d, c->s[DS], c->r[DX], raw, sizeof(raw));
    if (dos_resolve(d, raw, path, sizeof(path)) != 0) {
        dos_fail(d, DOS_ENOFILE);
        return;
    }
    uint16_t pb = c->r[BX], pbs = c->s[ES];
    if (mode == 3) {
        char *data;
        size_t size;
        if (vfs_read_all(path, &data, &size) != 0) {
            dos_fail(d, DOS_ENOFILE);
            return;
        }
        uint16_t seg = cpu_rw(c, pbs, pb), reloc = cpu_rw(c, pbs, (uint16_t)(pb + 2));
        int err = dos_load_overlay(d, (const uint8_t *)data, size, seg, reloc);
        kfree(data);
        if (err)
            dos_fail(d, (uint16_t)err);
        else
            dos_ok(d);
        return;
    }
    if (mode != 0) {
        dos_fail(d, DOS_EINVAL);
        return;
    }
    uint16_t env = cpu_rw(c, pbs, pb);
    uint16_t tail_off = cpu_rw(c, pbs, (uint16_t)(pb + 2)), tail_seg = cpu_rw(c, pbs, (uint16_t)(pb + 4));
    char tail[130];
    uint8_t len = MIN(cpu_rb(c, cpu_lin(tail_seg, tail_off)), (uint8_t)126);
    for (int i = 0; i < len; i++)
        tail[i] = (char)cpu_rb(c, cpu_lin(tail_seg, (uint16_t)(tail_off + 1 + i)));
    tail[len] = 0;
    char *cr = strchr(tail, '\r');
    if (cr)
        *cr = 0;

    if (str_casecmp(path_basename(path), "COMMAND.COM") == 0) {
        const char *cmd = tail;
        while (*cmd == ' ')
            cmd++;
        if ((cmd[0] == '/' || cmd[0] == '-') && (cmd[1] == 'c' || cmd[1] == 'C'))
            cmd += 2;
        int rc = 0;
        if (*cmd) {
            bool screen = d->screen;
            if (screen)
                dos_video_finish(d);
            rc = shell_exec(d->ctx->sh, cmd);
            if (screen) {
                d->screen = false;
                dos_enter_screen(d, false);
            }
        }
        d->return_code = (uint16_t)rc;
        dos_ok(d);
        return;
    }
    if (d->depth >= DOS_MAX_DEPTH) {
        dos_fail(d, DOS_ENOMEM);
        return;
    }
    int err = dos_exec(d, path, tail, env ? env : cpu_rw(c, d->psp, 0x2c), true);
    if (err)
        dos_fail(d, (uint16_t)err);
}

static void get_date_time(struct dos *d, bool date)
{
    struct cpu *c = &d->cpu;
    struct rtc_time t;
    hal_rtc(&t);
    if (date) {
        c->r[CX] = t.year;
        cpu_set_reg8(c, 6, t.month);
        cpu_set_reg8(c, 2, t.day);
        cpu_set_reg8(c, 0, (uint8_t)time_weekday(&t));
    } else {
        cpu_set_reg8(c, 5, t.hour);
        cpu_set_reg8(c, 1, t.minute);
        cpu_set_reg8(c, 6, t.second);
        cpu_set_reg8(c, 2, (uint8_t)(hal_ticks_ms() / 10 % 100));
    }
}

static int read_char(struct dos *d, bool wait, bool echo)
{
    if (d->pending_scan >= 0) {
        int scan = d->pending_scan;
        d->pending_scan = -1;
        return scan;
    }
    if (!stream_is_console(d->ctx->in)) {
        int ch = stream_getc(d->ctx->in);
        if (ch == '\n')
            ch = '\r';
        return ch < 0 ? 0x1a : ch;
    }
    int code = dos_read_key(d, wait, false);
    if (code < 0)
        return -1;
    uint8_t ascii = (uint8_t)code;
    if (ascii == 0 || ascii == 0xe0)
        d->pending_scan = code >> 8;
    if (ascii == 3 && d->running) {
        dos_puts(d, "^C\r\n");
        dos_terminate(d, 0, 1);
        return 3;
    }
    if (echo && ascii)
        dos_out(d, H_STDOUT, ascii);
    return ascii;
}

static void buffered_input(struct dos *d)
{
    struct cpu *c = &d->cpu;
    uint8_t max = cpu_rb(c, cpu_lin(c->s[DS], c->r[DX]));
    if (max == 0)
        return;
    char line[256];
    int len;
    if (stream_is_console(d->ctx->in)) {
        len = dos_line_input(d, line, max);
    } else {
        long n = stream_getline(d->ctx->in, line, max);
        len = n < 0 ? 0 : (int)n;
    }
    cpu_wb(c, cpu_lin(c->s[DS], (uint16_t)(c->r[DX] + 1)), (uint8_t)len);
    for (int i = 0; i < len; i++)
        cpu_wb(c, cpu_lin(c->s[DS], (uint16_t)(c->r[DX] + 2 + i)), (uint8_t)line[i]);
    cpu_wb(c, cpu_lin(c->s[DS], (uint16_t)(c->r[DX] + 2 + len)), '\r');
}

static void get_cwd(struct dos *d)
{
    struct cpu *c = &d->cpu;
    char buf[PATH_MAX_LEN];
    str_copy(buf, d->cwd + 1, sizeof(buf));
    for (char *p = buf; *p; p++)
        *p = *p == '/' ? '\\' : (char)to_upper((uint8_t)*p);
    buf[63] = 0;
    write_string(d, c->s[DS], c->r[SI], buf);
    c->r[AX] = 0x0100;
    dos_ok(d);
}

static void ioctl(struct dos *d)
{
    struct cpu *c = &d->cpu;
    struct dos_handle *h;
    switch (AL(c)) {
    case 0x00:
        if (!(h = handle(d, c->r[BX]))) {
            dos_fail(d, DOS_EBADF);
            return;
        }
        c->r[DX] = h->kind == H_FILE ? 0x0002 : h->kind == H_NUL ? 0x8084 : 0x80d3;
        if (h->kind == H_STDIN && !stream_is_console(d->ctx->in))
            c->r[DX] = 0x0002;
        if (h->kind == H_STDOUT && !stream_is_console(d->ctx->out))
            c->r[DX] = 0x0002;
        dos_ok(d);
        break;
    case 0x01:
    case 0x07:
        c->r[AX] = 0xff;
        dos_ok(d);
        break;
    case 0x06:
        h = handle(d, c->r[BX]);
        cpu_set_reg8(c, 0, h && h->kind == H_STDIN ? (dos_read_key(d, false, true) >= 0 ? 0xff : 0) : 0xff);
        dos_ok(d);
        break;
    case 0x08:
        c->r[AX] = 1;
        dos_ok(d);
        break;
    case 0x09:
        c->r[DX] = 0;
        dos_ok(d);
        break;
    case 0x0e:
        cpu_set_reg8(c, 0, 0);
        dos_ok(d);
        break;
    default:
        dos_fail(d, DOS_EINVAL);
        break;
    }
}

static void extended_open(struct dos *d)
{
    struct cpu *c = &d->cpu;
    char path[PATH_MAX_LEN];
    int device;
    if (read_path(d, c->s[DS], c->r[SI], path, &device) != 0) {
        dos_fail(d, DOS_ENOPATH);
        return;
    }
    bool exists = vfs_exists(path) || device;
    uint16_t action = c->r[DX], mode = c->r[BX];
    uint16_t taken;
    if (exists && (action & 0x0f) == 0) {
        dos_fail(d, DOS_EEXIST);
        return;
    }
    if (!exists && !(action & 0x10)) {
        dos_fail(d, DOS_ENOFILE);
        return;
    }
    uint16_t saved_dx = c->r[DX];
    if (!exists) {
        open_file(d, c->s[DS], c->r[SI], 2, true, false);
        taken = 2;
    } else if ((action & 0x0f) == 2) {
        open_file(d, c->s[DS], c->r[SI], 2, true, false);
        taken = 3;
    } else {
        open_file(d, c->s[DS], c->r[SI], mode & 3, false, false);
        taken = 1;
    }
    c->r[DX] = saved_dx;
    c->r[CX] = taken;
}

static void file_ops(struct dos *d, uint8_t fn)
{
    struct cpu *c = &d->cpu;
    char path[PATH_MAX_LEN], other[PATH_MAX_LEN];
    int err;
    switch (fn) {
    case 0x39:
        err = read_path(d, c->s[DS], c->r[DX], path, NULL);
        if (!err)
            err = vfs_mkdir(path);
        break;
    case 0x3a:
        err = read_path(d, c->s[DS], c->r[DX], path, NULL);
        if (!err && strcmp(path, d->cwd) == 0)
            err = E_BUSY;
        if (!err)
            err = vfs_is_dir(path) ? vfs_unlink(path) : E_NOENT;
        break;
    case 0x3b:
        err = read_path(d, c->s[DS], c->r[DX], path, NULL);
        if (!err && !vfs_is_dir(path))
            err = E_NOENT;
        if (!err)
            str_copy(d->cwd, path, sizeof(d->cwd));
        if (err) {
            dos_fail(d, DOS_ENOPATH);
            return;
        }
        break;
    case 0x41:
        err = read_path(d, c->s[DS], c->r[DX], path, NULL);
        if (!err && vfs_is_dir(path))
            err = E_PERM;
        if (!err)
            err = vfs_unlink(path);
        break;
    case 0x56:
        err = read_path(d, c->s[DS], c->r[DX], path, NULL);
        if (!err)
            err = read_path(d, c->s[ES], c->r[DI], other, NULL);
        if (!err && vfs_exists(other))
            err = E_EXIST;
        if (!err)
            err = vfs_rename(path, other);
        break;
    default:
        err = E_INVAL;
        break;
    }
    if (err)
        dos_fail(d, map_error(err, fn != 0x41));
    else
        dos_ok(d);
}

void dos_int21(struct dos *d)
{
    struct cpu *c = &d->cpu;
    uint8_t fn = AH(c);
    struct dos_handle *h;
    int ch;
    switch (fn) {
    case 0x00:
        dos_terminate(d, 0, 0);
        break;
    case 0x01:
        ch = read_char(d, true, true);
        cpu_set_reg8(c, 0, (uint8_t)(ch < 0 ? 0 : ch));
        break;
    case 0x02:
        dos_out(d, H_STDOUT, DL(c));
        cpu_set_reg8(c, 0, DL(c));
        break;
    case 0x03:
        cpu_set_reg8(c, 0, 0);
        break;
    case 0x04:
    case 0x05:
        break;
    case 0x06:
        if (DL(c) == 0xff) {
            ch = read_char(d, false, false);
            if (ch < 0) {
                cpu_set_reg8(c, 0, 0);
                dos_set_zf(d, true);
                dos_idle(d);
            } else {
                cpu_set_reg8(c, 0, (uint8_t)ch);
                dos_set_zf(d, false);
            }
        } else {
            dos_out(d, H_STDOUT, DL(c));
            cpu_set_reg8(c, 0, DL(c));
        }
        break;
    case 0x07:
    case 0x08:
        ch = read_char(d, true, false);
        cpu_set_reg8(c, 0, (uint8_t)(ch < 0 ? 0 : ch));
        break;
    case 0x09: {
        uint16_t off = c->r[DX];
        for (int i = 0; i < 65535; i++) {
            uint8_t b = cpu_rb(c, cpu_lin(c->s[DS], off++));
            if (b == '$')
                break;
            dos_out(d, H_STDOUT, b);
        }
        cpu_set_reg8(c, 0, '$');
        break;
    }
    case 0x0a:
        buffered_input(d);
        break;
    case 0x0b:
        cpu_set_reg8(c, 0, d->pending_scan >= 0 || dos_read_key(d, false, true) >= 0 ? 0xff : 0x00);
        if (AL(c) == 0)
            dos_idle(d);
        break;
    case 0x0c: {
        dos_flush_keys(d);
        uint8_t sub = AL(c);
        if (sub == 0x01 || sub == 0x06 || sub == 0x07 || sub == 0x08 || sub == 0x0a) {
            cpu_set_reg8(c, 4, sub);
            dos_int21(d);
        }
        break;
    }
    case 0x0d:
        break;
    case 0x0e:
        cpu_set_reg8(c, 0, 26);
        break;
    case 0x19:
        cpu_set_reg8(c, 0, 2);
        break;
    case 0x1a:
        d->dta_seg = c->s[DS];
        d->dta_off = c->r[DX];
        break;
    case 0x25:
        cpu_ww(c, 0, (uint16_t)(AL(c) * 4), c->r[DX]);
        cpu_ww(c, 0, (uint16_t)(AL(c) * 4 + 2), c->s[DS]);
        break;
    case 0x2a:
        get_date_time(d, true);
        break;
    case 0x2b:
    case 0x2d:
        cpu_set_reg8(c, 0, 0);
        break;
    case 0x2c:
        get_date_time(d, false);
        break;
    case 0x2f:
        c->s[ES] = d->dta_seg;
        c->r[BX] = d->dta_off;
        break;
    case 0x30:
        c->r[AX] = 0x0005;
        c->r[BX] = 0xff00;
        c->r[CX] = 0;
        break;
    case 0x33:
        if (AL(c) == 0x00)
            cpu_set_reg8(c, 2, 0);
        else if (AL(c) == 0x05)
            cpu_set_reg8(c, 2, 3);
        else if (AL(c) == 0x06)
            c->r[BX] = 0x0005, c->r[DX] = 0;
        break;
    case 0x35:
        c->r[BX] = cpu_rw(c, 0, (uint16_t)(AL(c) * 4));
        c->s[ES] = cpu_rw(c, 0, (uint16_t)(AL(c) * 4 + 2));
        break;
    case 0x36:
        if (DL(c) > 3) {
            c->r[AX] = 0xffff;
            break;
        }
        c->r[AX] = 8;
        c->r[BX] = 0x4000;
        c->r[CX] = 512;
        c->r[DX] = 0x8000;
        break;
    case 0x37:
        cpu_set_reg8(c, 0, 0);
        cpu_set_reg8(c, 2, '/');
        break;
    case 0x38: {
        uint8_t *info = linear(d, c->s[DS], c->r[DX], 34);
        if (info) {
            memset(info, 0, 34);
            info[2] = '$';
            info[7] = ',';
            info[9] = '.';
            info[11] = '/';
            info[13] = ':';
        }
        c->r[BX] = 1;
        dos_ok(d);
        break;
    }
    case 0x39:
    case 0x3a:
    case 0x3b:
    case 0x41:
    case 0x56:
        file_ops(d, fn);
        break;
    case 0x3c:
        open_file(d, c->s[DS], c->r[DX], 2, true, false);
        break;
    case 0x3d:
        open_file(d, c->s[DS], c->r[DX], AL(c), false, false);
        break;
    case 0x3e:
        if (!(h = handle(d, c->r[BX]))) {
            dos_fail(d, DOS_EBADF);
            break;
        }
        dos_handle_release(d, c->r[BX]);
        dos_ok(d);
        break;
    case 0x3f:
        do_read(d);
        break;
    case 0x40:
        do_write(d);
        break;
    case 0x42: {
        if (!(h = handle(d, c->r[BX]))) {
            dos_fail(d, DOS_EBADF);
            break;
        }
        long pos = 0;
        if (h->kind == H_FILE) {
            int32_t off = (int32_t)((uint32_t)c->r[CX] << 16 | c->r[DX]);
            pos = vfs_seek(h->file, off, AL(c));
            if (pos < 0) {
                dos_fail(d, DOS_EINVAL);
                break;
            }
        }
        c->r[AX] = (uint16_t)pos;
        c->r[DX] = (uint16_t)((uint32_t)pos >> 16);
        dos_ok(d);
        break;
    }
    case 0x43: {
        char path[PATH_MAX_LEN];
        struct vstat st;
        if (read_path(d, c->s[DS], c->r[DX], path, NULL) != 0 || vfs_stat(path, &st) != 0) {
            dos_fail(d, DOS_ENOFILE);
            break;
        }
        if (AL(c) == 0)
            c->r[CX] = st.type == VN_DIR ? 0x10 : 0x20;
        dos_ok(d);
        break;
    }
    case 0x44:
        ioctl(d);
        break;
    case 0x45:
    case 0x46: {
        if (!(h = handle(d, c->r[BX]))) {
            dos_fail(d, DOS_EBADF);
            break;
        }
        int target;
        if (fn == 0x45) {
            target = new_handle(d, h->kind, h->file);
            if (target < 0) {
                dos_fail(d, DOS_EMFILE);
                break;
            }
        } else {
            target = c->r[CX];
            if (target >= DOS_MAX_HANDLES) {
                dos_fail(d, DOS_EBADF);
                break;
            }
            if (target == c->r[BX]) {
                c->r[AX] = (uint16_t)target;
                dos_ok(d);
                break;
            }
            if (d->handles[target].kind != H_FREE)
                dos_handle_release(d, target);
        }
        d->handles[target] = *h;
        d->handles[target].owner = d->psp;
        c->r[AX] = (uint16_t)target;
        dos_ok(d);
        break;
    }
    case 0x47:
        get_cwd(d);
        break;
    case 0x48: {
        uint16_t largest;
        uint16_t seg = mcb_alloc(d, c->r[BX], &largest);
        if (!seg) {
            c->r[BX] = largest;
            dos_fail(d, DOS_ENOMEM);
            break;
        }
        c->r[AX] = seg;
        dos_ok(d);
        break;
    }
    case 0x49:
        if (mcb_free(d, c->s[ES]))
            dos_ok(d);
        else
            dos_fail(d, DOS_EBADBLOCK);
        break;
    case 0x4a: {
        uint16_t largest = 0;
        if (mcb_resize(d, c->s[ES], c->r[BX], &largest)) {
            dos_ok(d);
        } else {
            c->r[BX] = largest;
            dos_fail(d, largest ? DOS_ENOMEM : DOS_EBADBLOCK);
        }
        break;
    }
    case 0x4b:
        dos_exec_call(d);
        break;
    case 0x4c:
        dos_terminate(d, AL(c), 0);
        break;
    case 0x4d:
        c->r[AX] = d->return_code;
        break;
    case 0x4e:
        find_first(d);
        break;
    case 0x4f: {
        uint8_t *dta = linear(d, d->dta_seg, d->dta_off, 43);
        if (!dta || !(dta[0] & 0x80)) {
            dos_fail(d, DOS_ENOMORE);
            break;
        }
        int slot = dta[0] & 0x7f;
        if (slot >= DOS_MAX_FINDS || !d->finds[slot].used) {
            dos_fail(d, DOS_ENOMORE);
            break;
        }
        memcpy(&d->finds[slot].index, dta + 1, 4);
        find_next(d, slot);
        break;
    }
    case 0x50:
        d->psp = c->r[BX];
        break;
    case 0x51:
    case 0x62:
        c->r[BX] = d->psp;
        break;
    case 0x52:
        c->s[ES] = 0x0050;
        c->r[BX] = 0x0010;
        break;
    case 0x54:
        cpu_set_reg8(c, 0, 0);
        break;
    case 0x57: {
        if (!(h = handle(d, c->r[BX]))) {
            dos_fail(d, DOS_EBADF);
            break;
        }
        if (AL(c) == 0) {
            struct vstat st = {0};
            if (h->kind == H_FILE)
                vfs_fstat(h->file, &st);
            uint16_t time, date;
            dos_time(st.mtime ? st.mtime : time_now(), &time, &date);
            c->r[CX] = time;
            c->r[DX] = date;
        }
        dos_ok(d);
        break;
    }
    case 0x58:
        if (AL(c) == 0)
            c->r[AX] = 0;
        dos_ok(d);
        break;
    case 0x59:
        c->r[AX] = d->last_error;
        c->r[BX] = 0x0101;
        cpu_set_reg8(c, 5, 1);
        break;
    case 0x5a: {
        char base[PATH_MAX_LEN];
        dos_read_string(d, c->s[DS], c->r[DX], base, sizeof(base) - 16);
        size_t n = strlen(base);
        for (uint32_t i = 0; i < 1000; i++) {
            char name[16];
            ksnprintf(name, sizeof(name), "TMP%05u.$$$", (unsigned)((hal_entropy() + i) % 100000));
            str_copy(base + n, name, sizeof(base) - n);
            char path[PATH_MAX_LEN];
            if (dos_resolve(d, base, path, sizeof(path)) == 0 && !vfs_exists(path)) {
                write_string(d, c->s[DS], c->r[DX], base);
                open_file(d, c->s[DS], c->r[DX], 2, true, true);
                return;
            }
        }
        dos_fail(d, DOS_EACCES);
        break;
    }
    case 0x5b:
        open_file(d, c->s[DS], c->r[DX], 2, true, true);
        break;
    case 0x60: {
        char path[PATH_MAX_LEN], out[PATH_MAX_LEN];
        if (read_path(d, c->s[DS], c->r[SI], path, NULL) != 0) {
            dos_fail(d, DOS_ENOPATH);
            break;
        }
        ksnprintf(out, sizeof(out), "C:%s", path);
        for (char *p = out; *p; p++)
            *p = *p == '/' ? '\\' : (char)to_upper((uint8_t)*p);
        write_string(d, c->s[ES], c->r[DI], out);
        dos_ok(d);
        break;
    }
    case 0x66:
        c->r[BX] = c->r[DX] = 437;
        dos_ok(d);
        break;
    case 0x67:
    case 0x68:
    case 0x6a:
        dos_ok(d);
        break;
    case 0x6c:
        extended_open(d);
        break;
    default:
        cpu_set_reg8(c, 0, 0);
        dos_fail(d, DOS_EINVAL);
        break;
    }
}
