#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/string.h>

#include "dos.h"

#define MZ_HEADER_SIZE 28

static void trap(struct cpu *c, uint8_t n);
static uint8_t port_in(struct cpu *c, uint16_t port);
static void port_out(struct cpu *c, uint16_t port, uint8_t v);
static void halt(struct cpu *c);
static void invalid(struct cpu *c, uint8_t op);

static const struct cpu_hooks hooks = {trap, port_in, port_out, halt, invalid};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

void dos_set_cf(struct dos *d, bool on)
{
    struct cpu *c = &d->cpu;
    uint16_t off = (uint16_t)(c->r[SP] + 4);
    uint16_t f = cpu_rw(c, c->s[SS], off);
    cpu_ww(c, c->s[SS], off, on ? (uint16_t)(f | F_CF) : (uint16_t)(f & ~F_CF));
}

void dos_set_zf(struct dos *d, bool on)
{
    struct cpu *c = &d->cpu;
    uint16_t off = (uint16_t)(c->r[SP] + 4);
    uint16_t f = cpu_rw(c, c->s[SS], off);
    cpu_ww(c, c->s[SS], off, on ? (uint16_t)(f | F_ZF) : (uint16_t)(f & ~F_ZF));
}

void dos_fail(struct dos *d, uint16_t error)
{
    d->cpu.r[AX] = error;
    d->last_error = error;
    dos_set_cf(d, true);
}

void dos_ok(struct dos *d)
{
    dos_set_cf(d, false);
}

uint32_t dos_ticks(struct dos *d)
{
    uint64_t elapsed = hal_ticks_ms() - d->start_ms;
    return (uint32_t)((d->base_ticks + elapsed * 1193182u / 65536000u) % 0x1800b0u);
}

static void mcb_write(struct dos *d, uint16_t seg, char type, uint16_t owner, uint16_t size)
{
    uint8_t *m = d->mem + ((uint32_t)seg << 4);
    m[0] = (uint8_t)type;
    m[1] = (uint8_t)owner;
    m[2] = (uint8_t)(owner >> 8);
    m[3] = (uint8_t)size;
    m[4] = (uint8_t)(size >> 8);
}

static char mcb_type(struct dos *d, uint16_t seg)
{
    return (char)d->mem[(uint32_t)seg << 4];
}

static uint16_t mcb_owner(struct dos *d, uint16_t seg)
{
    return rd16(d->mem + ((uint32_t)seg << 4) + 1);
}

static uint16_t mcb_size(struct dos *d, uint16_t seg)
{
    return rd16(d->mem + ((uint32_t)seg << 4) + 3);
}

static void mcb_merge(struct dos *d)
{
    uint16_t seg = DOS_FIRST_MCB;
    while (mcb_type(d, seg) == 'M') {
        uint16_t next = (uint16_t)(seg + 1 + mcb_size(d, seg));
        if (mcb_owner(d, seg) == 0 && mcb_owner(d, next) == 0 &&
            (mcb_type(d, next) == 'M' || mcb_type(d, next) == 'Z')) {
            mcb_write(d, seg, mcb_type(d, next), 0, (uint16_t)(mcb_size(d, seg) + 1 + mcb_size(d, next)));
            continue;
        }
        seg = next;
    }
}

static void mcb_split(struct dos *d, uint16_t seg, uint16_t size)
{
    uint16_t total = mcb_size(d, seg);
    if (total <= size)
        return;
    char type = mcb_type(d, seg);
    uint16_t rest = (uint16_t)(seg + 1 + size);
    mcb_write(d, rest, type, 0, (uint16_t)(total - size - 1));
    mcb_write(d, seg, 'M', mcb_owner(d, seg), size);
}

uint16_t mcb_alloc(struct dos *d, uint16_t paragraphs, uint16_t *largest)
{
    mcb_merge(d);
    uint16_t seg = DOS_FIRST_MCB, best = 0;
    *largest = 0;
    for (;;) {
        char type = mcb_type(d, seg);
        if (type != 'M' && type != 'Z')
            return 0;
        uint16_t size = mcb_size(d, seg);
        if (mcb_owner(d, seg) == 0) {
            if (size > *largest)
                *largest = size;
            if (size >= paragraphs && !best)
                best = seg;
        }
        if (type == 'Z')
            break;
        seg = (uint16_t)(seg + 1 + size);
    }
    if (!best)
        return 0;
    if (mcb_size(d, best) > paragraphs)
        mcb_split(d, best, paragraphs);
    mcb_write(d, best, mcb_type(d, best), d->psp ? d->psp : 8, mcb_size(d, best));
    return (uint16_t)(best + 1);
}

static bool valid_block(struct dos *d, uint16_t seg)
{
    for (uint16_t m = DOS_FIRST_MCB;;) {
        char type = mcb_type(d, m);
        if (type != 'M' && type != 'Z')
            return false;
        if ((uint16_t)(m + 1) == seg)
            return true;
        if (type == 'Z')
            return false;
        m = (uint16_t)(m + 1 + mcb_size(d, m));
    }
}

bool mcb_free(struct dos *d, uint16_t seg)
{
    if (!valid_block(d, seg))
        return false;
    uint16_t m = (uint16_t)(seg - 1);
    mcb_write(d, m, mcb_type(d, m), 0, mcb_size(d, m));
    mcb_merge(d);
    return true;
}

bool mcb_resize(struct dos *d, uint16_t seg, uint16_t paragraphs, uint16_t *largest)
{
    if (!valid_block(d, seg))
        return false;
    mcb_merge(d);
    uint16_t m = (uint16_t)(seg - 1);
    uint16_t size = mcb_size(d, m);
    if (paragraphs <= size) {
        mcb_split(d, m, paragraphs);
        mcb_merge(d);
        return true;
    }
    uint16_t avail = size;
    if (mcb_type(d, m) == 'M') {
        uint16_t next = (uint16_t)(m + 1 + size);
        if (mcb_owner(d, next) == 0)
            avail = (uint16_t)(size + 1 + mcb_size(d, next));
        if (avail >= paragraphs) {
            mcb_write(d, m, mcb_type(d, next), mcb_owner(d, m), avail);
            mcb_split(d, m, paragraphs);
            return true;
        }
    }
    *largest = avail;
    return false;
}

void mcb_free_owner(struct dos *d, uint16_t owner)
{
    for (uint16_t m = DOS_FIRST_MCB;;) {
        char type = mcb_type(d, m);
        if (type != 'M' && type != 'Z')
            break;
        if (mcb_owner(d, m) == owner)
            mcb_write(d, m, type, 0, mcb_size(d, m));
        if (type == 'Z')
            break;
        m = (uint16_t)(m + 1 + mcb_size(d, m));
    }
    mcb_merge(d);
}

static void init_memory(struct dos *d)
{
    for (int n = 0; n < 256; n++) {
        uint8_t *stub = d->mem + ((uint32_t)DOS_BIOS_SEG << 4) + (uint32_t)n * 4;
        stub[0] = 0xf1;
        stub[1] = (uint8_t)n;
        stub[2] = 0xcf;
        stub[3] = 0x90;
        uint16_t off = n == 8 ? DOS_TIMER_STUB : (uint16_t)(n * 4);
        d->mem[n * 4] = (uint8_t)off;
        d->mem[n * 4 + 1] = (uint8_t)(off >> 8);
        d->mem[n * 4 + 2] = (uint8_t)DOS_BIOS_SEG;
        d->mem[n * 4 + 3] = (uint8_t)(DOS_BIOS_SEG >> 8);
    }
    static const uint8_t timer[] = {0xf1, 0x08, 0xcd, 0x1c, 0xcf};
    memcpy(d->mem + ((uint32_t)DOS_BIOS_SEG << 4) + DOS_TIMER_STUB, timer, sizeof(timer));
    memcpy(d->mem + 0xffff5, "10/06/26", 8);
    d->mem[0xffffe] = 0xfc;

    uint8_t *bda = d->mem + 0x400;
    bda[0x10] = 0x21;
    bda[0x13] = 0x80;
    bda[0x14] = 0x02;
    bda[0x49] = 3;
    bda[0x4a] = DOS_COLS;
    bda[0x4c] = 0xa0;
    bda[0x4d] = 0x0f;
    bda[0x60] = 0x07;
    bda[0x61] = 0x06;
    bda[0x63] = 0xd4;
    bda[0x64] = 0x03;
    bda[0x84] = DOS_ROWS - 1;
    bda[0x85] = 16;

    uint8_t *lol = d->mem + 0x500;
    lol[0x0e] = (uint8_t)DOS_FIRST_MCB;
    lol[0x0f] = (uint8_t)(DOS_FIRST_MCB >> 8);

    mcb_write(d, DOS_FIRST_MCB, 'Z', 0, (uint16_t)(DOS_MEM_TOP - DOS_FIRST_MCB - 1));

    struct rtc_time t;
    hal_rtc(&t);
    uint32_t seconds = t.hour * 3600u + t.minute * 60u + t.second;
    d->start_ms = hal_ticks_ms();
    d->base_ticks = (uint32_t)((uint64_t)seconds * 1193182u / 65536u);
    d->delivered = dos_ticks(d);
}

static void to_dos_path(const char *vfs_path, char *out, size_t cap)
{
    ksnprintf(out, cap, "C:%s", vfs_path);
    for (char *p = out; *p; p++)
        *p = *p == '/' ? '\\' : (char)to_upper((uint8_t)*p);
}

static uint16_t build_env(struct dos *d, uint16_t source, const char *program)
{
    char buf[1024];
    size_t len = 0;
    if (source) {
        const uint8_t *p = d->mem + ((uint32_t)source << 4);
        while (len + 2 < 768 && (p[len] || p[len + 1])) {
            buf[len] = (char)p[len];
            len++;
        }
        buf[len++] = 0;
    } else {
        static const char defaults[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\DOS;C:\\BIN\0PROMPT=$P$G\0OS=SynthOS\0";
        memcpy(buf, defaults, sizeof(defaults) - 1);
        len = sizeof(defaults) - 1;
    }
    buf[len++] = 0;
    buf[len++] = 1;
    buf[len++] = 0;
    char path[PATH_MAX_LEN];
    to_dos_path(program, path, sizeof(path));
    size_t pl = strlen(path) + 1;
    if (len + pl > sizeof(buf))
        pl = sizeof(buf) - len;
    memcpy(buf + len, path, pl);
    len += pl;
    uint16_t paras = (uint16_t)((len + 15) / 16), largest;
    uint16_t seg = mcb_alloc(d, paras, &largest);
    if (seg)
        memcpy(d->mem + ((uint32_t)seg << 4), buf, len);
    return seg;
}

static void fill_fcb(uint8_t *fcb, const char *arg)
{
    memset(fcb, 0, 16);
    memset(fcb + 1, ' ', 11);
    if (!arg)
        return;
    if (arg[0] && arg[1] == ':') {
        fcb[0] = (uint8_t)(to_upper((uint8_t)arg[0]) - 'A' + 1);
        arg += 2;
    }
    int i = 0;
    while (*arg && *arg != '.' && *arg != ' ' && *arg != '/' && i < 8)
        fcb[1 + i++] = (uint8_t)to_upper((uint8_t)*arg++);
    while (*arg && *arg != '.' && *arg != ' ')
        arg++;
    if (*arg == '.') {
        arg++;
        for (i = 0; *arg && *arg != ' ' && i < 3; i++)
            fcb[9 + i] = (uint8_t)to_upper((uint8_t)*arg++);
    }
}

static void build_psp(struct dos *d, uint16_t psp, uint16_t end, uint16_t env, uint16_t parent, const char *tail)
{
    uint8_t *p = d->mem + ((uint32_t)psp << 4);
    memset(p, 0, 256);
    p[0] = 0xcd;
    p[1] = 0x20;
    p[2] = (uint8_t)end;
    p[3] = (uint8_t)(end >> 8);
    p[0x16] = (uint8_t)parent;
    p[0x17] = (uint8_t)(parent >> 8);
    memset(p + 0x18, 0xff, 20);
    for (int i = 0; i < 5; i++)
        p[0x18 + i] = (uint8_t)i;
    p[0x2c] = (uint8_t)env;
    p[0x2d] = (uint8_t)(env >> 8);
    p[0x32] = 20;
    p[0x34] = 0x18;
    p[0x36] = (uint8_t)psp;
    p[0x37] = (uint8_t)(psp >> 8);
    p[0x50] = 0xcd;
    p[0x51] = 0x21;
    p[0x52] = 0xcb;

    size_t n = MIN(strlen(tail), (size_t)126);
    p[0x80] = (uint8_t)n;
    for (size_t i = 0; i < n; i++)
        p[0x81 + i] = (uint8_t)tail[i];
    p[0x81 + n] = 0x0d;

    const char *a1 = tail;
    while (*a1 == ' ')
        a1++;
    const char *a2 = a1;
    while (*a2 && *a2 != ' ')
        a2++;
    while (*a2 == ' ')
        a2++;
    fill_fcb(p + 0x5c, *a1 ? a1 : NULL);
    fill_fcb(p + 0x6c, *a2 ? a2 : NULL);
}

static void set_owner_name(struct dos *d, uint16_t seg, uint16_t owner, const char *path)
{
    uint8_t *m = d->mem + ((uint32_t)(seg - 1) << 4);
    m[1] = (uint8_t)owner;
    m[2] = (uint8_t)(owner >> 8);
    if (!path)
        return;
    const char *base = path_basename(path);
    memset(m + 8, 0, 8);
    for (int i = 0; i < 8 && base[i] && base[i] != '.'; i++)
        m[8 + i] = (uint8_t)to_upper((uint8_t)base[i]);
}

static bool is_mz(const uint8_t *data, size_t size)
{
    return size >= MZ_HEADER_SIZE && ((data[0] == 'M' && data[1] == 'Z') || (data[0] == 'Z' && data[1] == 'M'));
}

static void save_state(struct dos *d)
{
    struct dos_saved *s = &d->stack[d->depth++];
    memcpy(s->r, d->cpu.r, sizeof(s->r));
    memcpy(s->s, d->cpu.s, sizeof(s->s));
    s->ip = d->cpu.ip;
    s->flags = d->cpu.flags;
    s->psp = d->psp;
    s->dta_seg = d->dta_seg;
    s->dta_off = d->dta_off;
}

int dos_load_overlay(struct dos *d, const uint8_t *data, size_t size, uint16_t seg, uint16_t reloc)
{
    if (!is_mz(data, size)) {
        if (((uint32_t)seg << 4) + size > MEM_SIZE)
            return 8;
        memcpy(d->mem + ((uint32_t)seg << 4), data, size);
        return 0;
    }
    uint32_t pages = rd16(data + 4), last = rd16(data + 2), header = rd16(data + 8) * 16u;
    uint32_t image = pages * 512u - (last ? 512u - last : 0);
    if (image > size)
        image = (uint32_t)size;
    if (header > image)
        return 11;
    uint32_t length = image - header;
    if (((uint32_t)seg << 4) + length > MEM_SIZE)
        return 8;
    memcpy(d->mem + ((uint32_t)seg << 4), data + header, length);
    uint32_t count = rd16(data + 6), table = rd16(data + 24);
    for (uint32_t i = 0; i < count && table + i * 4 + 4 <= size; i++) {
        uint16_t off = rd16(data + table + i * 4), s = rd16(data + table + i * 4 + 2);
        uint32_t addr = cpu_lin((uint16_t)(seg + s), off);
        uint16_t v = (uint16_t)(rd16(d->mem + addr) + reloc);
        d->mem[addr] = (uint8_t)v;
        d->mem[addr + 1] = (uint8_t)(v >> 8);
    }
    return 0;
}

int dos_exec(struct dos *d, const char *path, const char *tail, uint16_t env_source, bool child)
{
    char *raw;
    size_t size;
    int err = vfs_read_all(path, &raw, &size);
    if (err)
        return err == E_NOENT ? 2 : 5;
    const uint8_t *data = (const uint8_t *)raw;
    bool exe = is_mz(data, size);
    if (!exe && size > 0xff00) {
        kfree(raw);
        return 11;
    }

    uint16_t env = build_env(d, env_source, path);
    if (!env) {
        kfree(raw);
        return 8;
    }
    uint32_t header = 0, length = (uint32_t)size, min_extra = 0x1000, max_extra = 0xffff;
    if (exe) {
        uint32_t pages = rd16(data + 4), last = rd16(data + 2);
        header = rd16(data + 8) * 16u;
        uint32_t image = pages * 512u - (last ? 512u - last : 0);
        if (image > size)
            image = (uint32_t)size;
        length = image > header ? image - header : 0;
        min_extra = rd16(data + 10);
        max_extra = rd16(data + 12);
    }
    uint32_t need = 0x10 + (length + 15) / 16 + min_extra;
    uint16_t largest = 0;
    mcb_alloc(d, 0xffff, &largest);
    uint32_t want = exe ? MIN(0x10 + (length + 15) / 16 + max_extra, (uint32_t)largest) : largest;
    if (need > 0xffff || largest < need || want < need) {
        mcb_free(d, env);
        kfree(raw);
        return 8;
    }
    uint16_t saved_psp = d->psp;
    uint16_t psp = mcb_alloc(d, (uint16_t)want, &largest);
    if (!psp) {
        mcb_free(d, env);
        kfree(raw);
        return 8;
    }
    if (child)
        save_state(d);
    set_owner_name(d, psp, psp, path);
    set_owner_name(d, env, psp, NULL);
    build_psp(d, psp, (uint16_t)(psp + want), env, saved_psp ? saved_psp : psp, tail);

    struct cpu *c = &d->cpu;
    if (exe) {
        uint16_t load = (uint16_t)(psp + 0x10);
        dos_load_overlay(d, data, size, load, load);
        c->s[SS] = (uint16_t)(load + rd16(data + 14));
        c->r[SP] = rd16(data + 16);
        c->s[CS] = (uint16_t)(load + rd16(data + 22));
        c->ip = rd16(data + 20);
    } else {
        memcpy(d->mem + ((uint32_t)psp << 4) + 0x100, data, size);
        c->s[CS] = c->s[SS] = psp;
        c->ip = 0x100;
        c->r[SP] = want >= 0x1000 ? 0xfffe : (uint16_t)(want * 16 - 2);
        cpu_ww(c, psp, c->r[SP], 0);
    }
    c->s[DS] = c->s[ES] = psp;
    memset(c->r, 0, sizeof(uint16_t) * 4);
    c->r[BP] = c->r[SI] = c->r[DI] = 0;
    c->flags = 0xf202;
    d->psp = psp;
    d->dta_seg = psp;
    d->dta_off = 0x80;
    kfree(raw);
    return 0;
}

void dos_terminate(struct dos *d, uint8_t code, uint8_t type)
{
    d->return_code = (uint16_t)(type << 8 | code);
    dos_handles_close_owner(d, d->psp);
    mcb_free_owner(d, d->psp);
    if (d->depth == 0) {
        d->exit_code = code;
        d->running = false;
        d->cpu.stop = true;
        return;
    }
    struct dos_saved *s = &d->stack[--d->depth];
    memcpy(d->cpu.r, s->r, sizeof(s->r));
    memcpy(d->cpu.s, s->s, sizeof(s->s));
    d->cpu.ip = s->ip;
    d->cpu.flags = s->flags;
    d->psp = s->psp;
    d->dta_seg = s->dta_seg;
    d->dta_off = s->dta_off;
    dos_set_cf(d, false);
}

static void report(struct dos *d, const char *msg)
{
    dos_puts(d, "\r\n");
    dos_puts(d, msg);
    dos_puts(d, "\r\n");
}

static void trap(struct cpu *c, uint8_t n)
{
    struct dos *d = c->user;
    switch (n) {
    case 0x00:
        report(d, "Divide overflow");
        dos_terminate(d, 0xff, 0);
        break;
    case 0x06:
        report(d, "Invalid opcode");
        dos_terminate(d, 0xff, 0);
        break;
    case 0x08: {
        uint32_t t = dos_ticks(d);
        memcpy(d->mem + 0x46c, &t, 4);
        break;
    }
    case 0x20:
        dos_terminate(d, 0, 0);
        break;
    case 0x21:
        dos_int21(d);
        break;
    case 0x23:
        dos_terminate(d, 0, 1);
        break;
    case 0x27:
        dos_terminate(d, 0, 3);
        break;
    default:
        dos_bios(d, n);
        break;
    }
}

static uint8_t port_in(struct cpu *c, uint16_t port)
{
    struct dos *d = c->user;
    switch (port) {
    case 0x60:
        return d->last_scancode;
    case 0x61:
        return 0x20;
    case 0x64:
        return 0x14;
    case 0x3da:
    case 0x3ba:
        return (d->retrace++ & 1) ? 0x09 : 0x00;
    case 0x40: {
        uint16_t counter = (uint16_t)-(uint16_t)(hal_ticks_ms() * 1193u);
        return (d->retrace++ & 1) ? (uint8_t)(counter >> 8) : (uint8_t)counter;
    }
    case 0x3d5: {
        uint16_t pos = (uint16_t)(d->mem[0x451] * DOS_COLS + d->mem[0x450]);
        return d->crtc_index == 0x0e ? (uint8_t)(pos >> 8) : d->crtc_index == 0x0f ? (uint8_t)pos : 0;
    }
    default:
        return 0xff;
    }
}

static void port_out(struct cpu *c, uint16_t port, uint8_t v)
{
    struct dos *d = c->user;
    if (port == 0x3d4) {
        d->crtc_index = v;
    } else if (port == 0x3d5 && (d->crtc_index == 0x0e || d->crtc_index == 0x0f)) {
        uint16_t pos = (uint16_t)(d->mem[0x451] * DOS_COLS + d->mem[0x450]);
        pos = d->crtc_index == 0x0e ? (uint16_t)((pos & 0xff) | v << 8) : (uint16_t)((pos & 0xff00) | v);
        d->mem[0x450] = (uint8_t)(pos % DOS_COLS);
        d->mem[0x451] = (uint8_t)(pos / DOS_COLS);
    }
}

static void halt(struct cpu *c)
{
    struct dos *d = c->user;
    if (c->flags & F_IF) {
        dos_idle(d);
        return;
    }
    report(d, "System halted by program");
    dos_terminate(d, 0xff, 0);
}

static void invalid(struct cpu *c, uint8_t op)
{
    struct dos *d = c->user;
    ksnprintf(d->message, sizeof(d->message), "unsupported instruction 0x%02x at %04x:%04x", op, c->op_cs, c->op_ip);
    c->stop = true;
    d->running = false;
    d->exit_code = 255;
}

void dos_idle(struct dos *d)
{
    dos_video_sync(d, false);
    hal_sleep_ms(1);
    uint32_t t = dos_ticks(d);
    if (d->delivered != t && cpu_irq(&d->cpu, 8))
        d->delivered = t;
}

static void join_args(struct exec_ctx *ctx, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 1; i < ctx->argc; i++) {
        str_append(out, " ", cap);
        str_append(out, ctx->argv[i], cap);
    }
}

static int dos_run(struct exec_ctx *ctx)
{
    struct dos *d = kzalloc(sizeof(*d));
    if (!d)
        return E_NOMEM;
    d->mem = kzalloc(MEM_SIZE);
    if (!d->mem) {
        kfree(d);
        return E_NOMEM;
    }
    d->ctx = ctx;
    d->cpu.mem = d->mem;
    d->cpu.hooks = &hooks;
    d->cpu.user = d;
    d->pending_scan = -1;
    cpu_reset(&d->cpu);
    init_memory(d);
    dos_handles_init(d);
    dos_video_init(d);
    str_copy(d->cwd, ctx->sh->cwd, sizeof(d->cwd));

    char tail[128];
    join_args(ctx, tail, sizeof(tail));
    int err = dos_exec(d, ctx->path, tail, 0, false);
    if (err) {
        kfree(d->mem);
        kfree(d);
        return err == 8 ? E_NOMEM : err == 2 ? E_NOENT : E_NOEXEC;
    }
    d->running = true;
    uint8_t saved_attr = con_attr();
    while (d->running) {
        cpu_run(&d->cpu, 4096);
        if (!d->running)
            break;
        uint32_t t = dos_ticks(d);
        if (t != d->delivered && cpu_irq(&d->cpu, 8))
            d->delivered = t;
        input_poll();
        if (input_break_pending()) {
            report(d, "^C");
            d->exit_code = 130;
            break;
        }
        if (hal_ticks_ms() - d->last_sync >= 25)
            dos_video_sync(d, false);
    }
    dos_video_finish(d);
    con_set_attr(saved_attr);
    if (d->message[0])
        sh_error(ctx->sh, path_basename(ctx->path), "%s", d->message);
    for (int i = 0; i < DOS_MAX_HANDLES; i++)
        if (d->handles[i].kind != H_FREE)
            dos_handle_release(d, i);
    int code = d->exit_code;
    kfree(d->mem);
    kfree(d);
    return code;
}

bool w32_is_pe(const uint8_t *file, size_t size);

static bool mz_probe(const char *path, const uint8_t *head, size_t n)
{
    UNUSED(path);
#if CONFIG_WIN32
    if (w32_is_pe(head, n))
        return false;
#endif
    return is_mz(head, n);
}

static bool com_probe(const char *path, const uint8_t *head, size_t n)
{
    return str_iends(path, ".com") && !is_mz(head, n);
}

static int dos_binfmt_run(struct exec_ctx *ctx, uint8_t *image, size_t size)
{
    UNUSED(image);
    UNUSED(size);
    return dos_run(ctx);
}

static const struct binfmt mz_format = {
    .name = "mz",
    .description = "MS-DOS executable (8086 emulation)",
    .extensions = ".exe",
    .probe = mz_probe,
    .run = dos_binfmt_run,
};
BINFMT(mz_format);

static const struct binfmt com_format = {
    .name = "com",
    .description = "MS-DOS COM program (8086 emulation)",
    .extensions = ".com",
    .probe = com_probe,
    .run = dos_binfmt_run,
};
BINFMT(com_format);
