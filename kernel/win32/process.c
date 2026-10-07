#include <synth/codepage.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>
#include <synth/winpath.h>

#include "win32.h"

#define ARENA_BASE 0x20000000u
#define ARENA_MAX (512u * 1024 * 1024)
#define ARENA_MIN (1024u * 1024)
#define ARENA_HIGH_MAX (1024u * 1024 * 1024)
#define STACK_SIZE (1024u * 1024)
#define HANDLE_BASE 0x100u

struct w32_process *w32;

bool w32_is_pe(const uint8_t *file, size_t size);
struct w32_image *w32_load_main(const char *path, int *error);

void w32_set_error(DWORD error)
{
    if (w32 && w32->teb)
        memcpy(w32->teb + 0x34, &error, 4);
    if (w32 && w32->teb64)
        memcpy(w32->teb64 + 0x68, &error, 4);
}

DWORD w32_error(void)
{
    DWORD e = 0;
    if (w32 && w32->teb)
        memcpy(&e, w32->teb + 0x34, 4);
    return e;
}

DWORD w32_error_from_vfs(int err)
{
    switch (err) {
    case 0: return ERROR_SUCCESS;
    case E_NOENT: return ERROR_FILE_NOT_FOUND;
    case E_NOTDIR: return ERROR_PATH_NOT_FOUND;
    case E_EXIST: return ERROR_ALREADY_EXISTS;
    case E_NOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case E_NOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case E_NAMETOOLONG: return ERROR_INVALID_NAME;
    case E_INVAL: return ERROR_INVALID_PARAMETER;
    case E_BADF: return ERROR_INVALID_HANDLE;
    case E_MFILE: return ERROR_TOO_MANY_OPEN_FILES;
    default: return ERROR_ACCESS_DENIED;
    }
}

#define FRESH_ZERO (64u * 1024)

void *w32_alloc(size_t size)
{
    void *p = heap_alloc(&w32->heap, size);
    if (p && size >= FRESH_ZERO)
        memset(p, 0, size);
    return p;
}

void *w32_zalloc(size_t size)
{
    void *p = heap_alloc(&w32->heap, size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *w32_realloc(void *ptr, size_t size)
{
    return heap_realloc(&w32->heap, ptr, size);
}

void w32_free(void *ptr)
{
    heap_free(&w32->heap, ptr);
}

size_t w32_msize(void *ptr)
{
    return heap_size_of(&w32->heap, ptr);
}

char *w32_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = w32_alloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

HANDLE w32_handle_new(enum w32_object_type type)
{
    for (int i = 3; i < W32_MAX_HANDLES; i++) {
        if (w32->handles[i].type == WH_FREE) {
            memset(&w32->handles[i], 0, sizeof(w32->handles[i]));
            w32->handles[i].type = type;
            w32->handles[i].refs = 1;
            return (HANDLE)(uintptr_t)(HANDLE_BASE + i * 4);
        }
    }
    w32_set_error(ERROR_TOO_MANY_OPEN_FILES);
    return NULL;
}

struct w32_object *w32_handle_get(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (v < HANDLE_BASE || (v - HANDLE_BASE) % 4)
        return NULL;
    uintptr_t i = (v - HANDLE_BASE) / 4;
    if (i >= W32_MAX_HANDLES || w32->handles[i].type == WH_FREE)
        return NULL;
    return &w32->handles[i];
}

HANDLE w32_std_handle(int which)
{
    return (HANDLE)(uintptr_t)(HANDLE_BASE + (uintptr_t)which * 4);
}

bool w32_handle_close(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return false;
    if (--o->refs > 0)
        return true;
    if (o->file)
        vfs_close(o->file);
    if (o->find)
        kfree(o->find);
    o->type = WH_FREE;
    o->file = NULL;
    o->find = NULL;
    return true;
}

int w32_path(const char *win, char *out, size_t cap)
{
    return winpath_to_vfs(w32->cwd, win, out, cap);
}

void w32_path_to_win(const char *vfs_path, char *out, size_t cap)
{
    winpath_from_vfs(vfs_path, out, cap);
}

void w32_console_write_raw(int kind, const char *s, size_t n)
{
    struct stream *out = kind == WH_STDERR ? w32->ctx->err : w32->ctx->out;
    if (stream_is_console(out)) {
        uint8_t saved = con_attr();
        con_set_attr(w32->console_attr);
        con_write(s, n);
        con_set_attr(saved);
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            stream_write(out, s + start, i - start);
            start = i + 1;
        }
    }
    stream_write(out, s + start, n - start);
}

void w32_console_write(int kind, const char *s, size_t n)
{
    struct stream *out = kind == WH_STDERR ? w32->ctx->err : w32->ctx->out;
    int cp = w32->output_cp;
    if (cp == codepage_current() || !stream_is_console(out)) {
        w32_console_write_raw(kind, s, n);
        return;
    }
    char buf[256];
    WCHAR wide[128];
    while (n) {
        size_t chunk = MIN(n, ARRAY_SIZE(wide));
        if (cp == 65001)
            while (chunk < n && chunk > 1 && ((uint8_t)s[chunk] & 0xc0) == 0x80)
                chunk--;
        size_t wn = w32_cp_to_wide(s, (int)chunk, wide, ARRAY_SIZE(wide), cp);
        size_t bn = w32_wide_to_cp(wide, (int)MIN(wn, ARRAY_SIZE(wide)), buf, sizeof(buf), 1);
        w32_console_write_raw(kind, buf, MIN(bn, sizeof(buf)));
        s += chunk;
        n -= chunk;
    }
}

static W32_STATE char line_buffer[1024];
static W32_STATE size_t line_len, line_pos;

long w32_console_read(char *buf, size_t n)
{
    struct stream *in = w32->ctx->in;
    if (!stream_is_console(in))
        return stream_read(in, buf, n);
    if (line_pos >= line_len) {
        long got = stream_getline(in, line_buffer, sizeof(line_buffer) - 2);
        if (input_break_pending()) {
            w32->exiting = true;
            w32_exit(STATUS_CONTROL_C_EXIT);
        }
        if (got < 0)
            return 0;
        line_buffer[got++] = '\r';
        line_buffer[got++] = '\n';
        line_len = (size_t)got;
        line_pos = 0;
    }
    size_t k = MIN(n, line_len - line_pos);
    memcpy(buf, line_buffer + line_pos, k);
    line_pos += k;
    return (long)k;
}

uint32_t w32_call(void *fn, int argc, const uint32_t *args)
{
    if (!w32->jump.active)
        return arch_w32_call(fn, argc, args, w32->stack + w32->stack_size - (w32->win64 ? 4096 : 16), &w32->jump);
    typedef uint32_t WINAPI (*f0)(void);
    typedef uint32_t WINAPI (*f1)(uint32_t);
    typedef uint32_t WINAPI (*f2)(uint32_t, uint32_t);
    typedef uint32_t WINAPI (*f3)(uint32_t, uint32_t, uint32_t);
    typedef uint32_t WINAPI (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
    switch (argc) {
    case 0: return ((f0)fn)();
    case 1: return ((f1)fn)(args[0]);
    case 2: return ((f2)fn)(args[0], args[1]);
    case 3: return ((f3)fn)(args[0], args[1], args[2]);
    default: return ((f4)fn)(args[0], args[1], args[2], args[3]);
    }
}

void w32_run_atexit(void)
{
    while (w32->atexit_count > 0) {
        void (*fn)(void) = w32->atexit_fns[--w32->atexit_count];
        fn();
    }
}

void w32_exit(uint32_t code)
{
    w32->exiting = true;
    w32->exit_code = code;
    if (w32->jump.active)
        arch_w32_abort(&w32->jump, code);
    panic("win32: exit outside of program context");
}

static const char *fault_name(int vector)
{
    switch (vector) {
    case 0: return "integer division by zero";
    case 3: return "breakpoint";
    case 6: return "illegal instruction";
    case 13: return "general protection fault";
    case 14: return "access violation";
    case 16: return "floating point error";
    case 19: return "SIMD floating point error";
    default: return "CPU exception";
    }
}

static void on_fault(int vector, uint32_t eip, uint32_t address)
{
    if (!w32 || !w32->jump.active)
        panic("CPU exception %d at %08x (address %08x)", vector, eip, address);
    if (vector == 14)
        ksnprintf(w32->message, sizeof(w32->message), "crashed: %s at 0x%08x (address 0x%08x)", fault_name(vector),
                  eip, address);
    else
        ksnprintf(w32->message, sizeof(w32->message), "crashed: %s at 0x%08x", fault_name(vector), eip);
    uint32_t code = vector == 0 ? 0xc0000094u : vector == 6 ? 0xc000001du : vector == 3 ? 0x80000003u
                  : vector == 16 || vector == 19 ? 0xc0000090u : STATUS_ACCESS_VIOLATION;
    w32->exiting = true;
    w32->exit_code = code;
    arch_w32_abort(&w32->jump, code);
}

static void on_tick(uint32_t eip)
{
    static unsigned ticks;
    if (!w32 || !w32->jump.active || eip < VMM_USER_START || eip >= 0xc0000000u)
        return;
    ticks++;
    if (ticks % 32 == 0) {
        input_poll();
        if (input_break_pending()) {
            w32->exiting = true;
            w32->exit_code = STATUS_CONTROL_C_EXIT;
            arch_w32_abort(&w32->jump, STATUS_CONTROL_C_EXIT);
        }
    }
    if (ticks % 10 == 0)
        w32_preempt();
}

static bool needs_quotes(const char *s)
{
    if (!*s)
        return true;
    for (; *s; s++)
        if (*s == ' ' || *s == '\t' || *s == '"')
            return true;
    return false;
}

static void build_command_line(struct exec_ctx *ctx)
{
    size_t cap = PATH_MAX_LEN + 16;
    for (int i = 1; i < ctx->argc; i++)
        cap += strlen(ctx->argv[i]) * 2 + 3;
    char *line = w32_alloc(cap);
    char exe[PATH_MAX_LEN];
    w32_path_to_win(ctx->path, exe, sizeof(exe));
    w32->argc = ctx->argc > 0 ? ctx->argc : 1;
    w32->argv = w32_zalloc(sizeof(char *) * (size_t)(w32->argc + 1));
    w32->argv[0] = w32_strdup(exe);
    ksnprintf(line, cap, "\"%s\"", exe);
    for (int i = 1; i < ctx->argc; i++) {
        w32->argv[i] = w32_strdup(ctx->argv[i]);
        str_append(line, " ", cap);
        if (!needs_quotes(ctx->argv[i])) {
            str_append(line, ctx->argv[i], cap);
            continue;
        }
        str_append(line, "\"", cap);
        for (const char *p = ctx->argv[i]; *p; p++) {
            if (*p == '"')
                str_append(line, "\\", cap);
            char ch[2] = {*p, 0};
            str_append(line, ch, cap);
        }
        str_append(line, "\"", cap);
    }
    w32->cmdline = line;
    w32->cmdline_w = w32_widen(line);
    w32->wargv = w32_zalloc(sizeof(WCHAR *) * (size_t)(w32->argc + 1));
    for (int i = 0; i < w32->argc; i++)
        w32->wargv[i] = w32_widen(w32->argv[i]);
}

static void build_environment(struct exec_ctx *ctx)
{
    struct shell *sh = ctx->sh;
    const char *user = sh_get(sh, "USER") ? sh_get(sh, "USER") : "root";
    const char *host = sh_get(sh, "HOSTNAME") ? sh_get(sh, "HOSTNAME") : "synth";
    char defaults[1024];
    ksnprintf(defaults, sizeof(defaults),
              "ALLUSERSPROFILE=C:\\ProgramData\n"
              "APPDATA=C:\\Users\\%s\\AppData\\Roaming\n"
              "COMPUTERNAME=%s\n"
              "ComSpec=C:\\Windows\\System32\\cmd.exe\n"
              "HOMEDRIVE=C:\n"
              "HOMEPATH=\\Users\\%s\n"
              "LOCALAPPDATA=C:\\Users\\%s\\AppData\\Local\n"
              "NUMBER_OF_PROCESSORS=1\n"
              "OS=Windows_NT\n"
              "PATH=C:\\Windows\\System32;C:\\Windows;C:\\Program Files\n"
              "PATHEXT=.COM;.EXE;.BAT;.CMD\n"
              "PROCESSOR_ARCHITECTURE=x86\n"
              "ProgramData=C:\\ProgramData\n"
              "ProgramFiles=C:\\Program Files\n"
              "SystemDrive=C:\n"
              "SystemRoot=C:\\Windows\n"
              "TEMP=C:\\Windows\\Temp\n"
              "TMP=C:\\Windows\\Temp\n"
              "USERNAME=%s\n"
              "USERPROFILE=C:\\Users\\%s\n"
              "windir=C:\\Windows\n"
              "SYNTHOS=%s\n",
              user, host, user, user, user, user, SYNTH_VERSION);
    size_t cap = strlen(defaults) + 2;
    for (size_t i = 0; i < sh->vars.count; i++)
        cap += strlen(sh->vars.items[i].key) + strlen(sh->vars.items[i].value) + 2;
    char *block = w32_zalloc(cap);
    size_t len = 0;
    int count = 0;
    for (const char *p = defaults; *p;) {
        const char *nl = strchr(p, '\n');
        memcpy(block + len, p, (size_t)(nl - p));
        len += (size_t)(nl - p) + 1;
        count++;
        p = nl + 1;
    }
    static const char *const skip = "PATH,PATHEXT,PROMPT,PROMPT2,HOME,USER,HOSTNAME,PWD,OLDPWD,SHELL,OS";
    for (size_t i = 0; i < sh->vars.count; i++) {
        const char *k = sh->vars.items[i].key;
        if (str_in_list(k, skip) || strchr(k, '='))
            continue;
        len += (size_t)ksnprintf(block + len, cap - len, "%s=%s", k, sh->vars.items[i].value) + 1;
        count++;
    }
    block[len] = 0;
    w32->env_block = block;
    w32->environ = w32_zalloc(sizeof(char *) * (size_t)(count + 1));
    w32->wenviron = w32_zalloc(sizeof(WCHAR *) * (size_t)(count + 1));
    int n = 0;
    for (char *p = block; *p && n < count; p += strlen(p) + 1, n++) {
        w32->environ[n] = p;
        w32->wenviron[n] = w32_widen(p);
    }
    size_t wn = w32_cp_to_wide(block, (int)len + 1, NULL, 0, 1);
    w32->env_block_w = w32_alloc((wn + 1) * sizeof(WCHAR));
    w32_cp_to_wide(block, (int)len + 1, w32->env_block_w, wn, 1);
    w32->env_block_w[wn] = 0;
}

static void put32(uint8_t *base, uint32_t off, uint32_t v)
{
    memcpy(base + off, &v, 4);
}

static void put_unicode_string(uint8_t *base, uint32_t off, const WCHAR *s)
{
    uint16_t len = (uint16_t)(w32_wcslen(s) * 2);
    memcpy(base + off, &len, 2);
    uint16_t max = (uint16_t)(len + 2);
    memcpy(base + off + 2, &max, 2);
    put32(base, off + 4, (uint32_t)s);
}

static bool build_teb_peb(void)
{
    w32->teb = heap_alloc_aligned(&w32->heap, 4096, 4096);
    w32->peb = heap_alloc_aligned(&w32->heap, 4096, 4096);
    uint8_t *params = w32_zalloc(0x300);
    uint8_t *ldr = w32_zalloc(0x40);
    if (!w32->teb || !w32->peb || !params || !ldr)
        return false;
    uint32_t stack_top = (uint32_t)w32->stack + w32->stack_size;
    put32(w32->teb, 0x00, 0xffffffffu);
    put32(w32->teb, 0x04, stack_top);
    put32(w32->teb, 0x08, (uint32_t)w32->stack);
    put32(w32->teb, 0x18, (uint32_t)w32->teb);
    put32(w32->teb, 0x20, 0x100);
    put32(w32->teb, 0x24, 0x104);
    put32(w32->teb, 0x30, (uint32_t)w32->peb);

    for (uint32_t off = 0x0c; off <= 0x1c; off += 8) {
        put32(ldr, off, (uint32_t)(ldr + off));
        put32(ldr, off + 4, (uint32_t)(ldr + off));
    }
    put32(ldr, 0x00, 0x30);
    put32(ldr, 0x04, 1);
    put32(w32->peb, 0x0c, (uint32_t)ldr);
    put32(w32->peb, 0x10, (uint32_t)params);
    put32(w32->peb, 0x18, (uint32_t)&w32->heap);
    put32(w32->peb, 0x64, 1);
    put32(w32->peb, 0xa4, 10);
    put32(w32->peb, 0xa8, 0);
    put32(w32->peb, 0xac, 19045);
    put32(w32->peb, 0xb0, 2);

    put32(params, 0x00, 0x300);
    put32(params, 0x04, 0x300);
    put32(params, 0x08, 1);
    char exe[PATH_MAX_LEN];
    w32_path_to_win(w32->ctx->path, exe, sizeof(exe));
    put_unicode_string(params, 0x38, w32_widen(exe));
    put_unicode_string(params, 0x40, w32->cmdline_w);
    put32(params, 0x48, (uint32_t)w32->env_block_w);
    arch_w32_set_teb((uint32_t)w32->teb);
    return true;
}

static void setup_tls(struct w32_image *img)
{
    const uint8_t *mem = (const uint8_t *)img->base;
    uint32_t opt = *(const uint32_t *)(mem + 0x3c) + 24;
    uint32_t rva = *(const uint32_t *)(mem + opt + 96 + 9 * 8);
    if (!rva)
        return;
    const uint32_t *dir = (const uint32_t *)(mem + rva);
    uint32_t start = dir[0], end = dir[1], index_addr = dir[2], callbacks = dir[3], zero = dir[4];
    uint32_t size = end - start;
    uint8_t *block = w32_zalloc(size + zero + 16);
    void **slots = w32_zalloc(sizeof(void *) * 64);
    if (!block || !slots)
        return;
    memcpy(block, (const void *)start, size);
    slots[0] = block;
    put32(w32->teb, 0x2c, (uint32_t)slots);
    if (index_addr)
        *(uint32_t *)index_addr = 0;
    w32->tls_start = start;
    w32->tls_size = size;
    w32->tls_zero = zero;
    w32->tls_callbacks = callbacks;
    w32->tls_index = 0;
    for (const uint32_t *cb = (const uint32_t *)callbacks; cb && *cb; cb++) {
        uint32_t args[3] = {img->base, 1, 0};
        w32_call((void *)*cb, 3, args);
        if (w32->exiting)
            return;
    }
}

static size_t image_reserve;
static bool image_win64;

static size_t choose_arena(void)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    size_t size = hs.largest_free / 4 * 3;
    if (hs.largest_free > image_reserve && hs.largest_free - image_reserve < size)
        size = hs.largest_free - image_reserve;
    if (size > ARENA_MAX)
        size = ARENA_MAX;
    return size & ~(size_t)0xffff;
}

static bool high_arena(void)
{
    size_t pages = vmm_high_pages() / 4 * 3;
    uint32_t size = (uint32_t)MIN((uint64_t)pages * 4096, (uint64_t)ARENA_HIGH_MAX) & ~0xffffu;
    if (size < 64u * 1024 * 1024)
        return false;
    uint32_t *frames = kmalloc(size / 4096 * sizeof(uint32_t));
    if (!frames)
        return false;
    if (!vmm_frames_alloc(frames, size / 4096)) {
        kfree(frames);
        return false;
    }
    if (vmm_map_frames(ARENA_BASE, frames, size / 4096) != 0) {
        vmm_frames_free(frames, size / 4096);
        kfree(frames);
        return false;
    }
    w32->arena_frames = frames;
    w32->arena_size = size;
    return true;
}

static void cleanup(void)
{
    w32_winsock_cleanup();
    w32_inet_cleanup();
#if CONFIG_GUI
    w32_gui_cleanup();
#endif
    for (int i = 0; i < W32_MAX_HANDLES; i++)
        if (w32->handles[i].type != WH_FREE)
            w32_handle_close((HANDLE)(uintptr_t)(HANDLE_BASE + i * 4));
    w32_unload_all();
    if (w32->arena) {
        vmm_unmap(w32->arena_base, w32->arena_size);
        kfree_pages(w32->arena);
    }
    if (w32->arena_frames) {
        vmm_unmap(w32->arena_base, w32->arena_size);
        vmm_frames_free(w32->arena_frames, w32->arena_size / 4096);
        kfree(w32->arena_frames);
    }
}

static int run(struct exec_ctx *ctx)
{
    w32->ctx = ctx;
    w32->win64 = image_win64;
    if (w32->win64 && !arch_long_supported()) {
        ksnprintf(w32->message, sizeof(w32->message), "is a 64-bit program and this processor has no 64-bit mode");
        return 1;
    }
    str_copy(w32->cwd, ctx->sh->cwd, sizeof(w32->cwd));
    w32->default_attr = theme_attr(ROLE_TEXT);
    w32->console_attr = w32->default_attr;
    w32->output_cp = w32->input_cp = codepage_current();
    w32->input_mode = 0x1f7;
    w32->output_mode = 0x7;
    w32->arena_base = ARENA_BASE;
    if (!high_arena()) {
        w32->arena_size = (uint32_t)choose_arena();
        if (w32->arena_size < ARENA_MIN)
            return E_NOMEM;
        w32->arena = kmalloc_pages_uninit(w32->arena_size);
        if (!w32->arena || vmm_map(ARENA_BASE, w32->arena, w32->arena_size) != 0)
            return E_NOMEM;
    }
    heap_create(&w32->heap, (void *)ARENA_BASE, w32->arena_size, false);

    w32->stack_size = MIN(STACK_SIZE, w32->arena_size / 4 & ~0xffffu);
    w32->stack = heap_alloc_aligned(&w32->heap, w32->stack_size, 4096);
    for (int i = 0; i < 3; i++) {
        w32->handles[i].type = (enum w32_object_type)(WH_STDIN + i);
        w32->handles[i].refs = 1 << 20;
    }
    build_command_line(ctx);
    build_environment(ctx);
    if (!w32->stack || !build_teb_peb() || !w32_threads_start() || (w32->win64 && !w64_setup()))
        return E_NOMEM;

    int error = 0;
    struct w32_image *main = w32_load_main(ctx->path, &error);
    if (!main) {
        if (!w32->message[0])
            ksnprintf(w32->message, sizeof(w32->message), "cannot load program (Windows error %d)", error);
        return 1;
    }
    w32->main = main;
    put32(w32->peb, 0x08, main->base);
    bool entered_long = false;
    if (w32->win64) {
        if (!main->pe64) {
            ksnprintf(w32->message, sizeof(w32->message), "cannot mix 32-bit and 64-bit code");
            return 1;
        }
        w64_set_image(main->base);
        entered_long = !arch_long_active();
        if (!arch_long_enter()) {
            ksnprintf(w32->message, sizeof(w32->message), "cannot switch the processor to 64-bit mode");
            return 1;
        }
    }

    arch_set_fault_handler(on_fault);
    arch_set_tick_hook(on_tick);
    if (w32_init_modules() == 0 && !w32->exiting) {
        if (w32->win64)
            w64_setup_tls(main);
        else
            setup_tls(main);
    }
    uint32_t code = w32->exit_code;
    if (!w32->exiting) {
        code = w32->win64 ? w64_run_main(main->entry) : w32_call((void *)main->entry, 0, NULL);
        if (!w32->exiting)
            w32->exit_code = code;
    }
    arch_set_tick_hook(NULL);
    arch_set_fault_handler(NULL);
    if (w32->win64)
        w64_cleanup();
    if (entered_long)
        arch_long_leave();
    if (w32->threads)
        arch_w32_set_teb((uint32_t)w32->threads->teb);
    w32_threads_end();
    return (int)w32->exit_code;
}

static bool pe_probe(const char *path, const uint8_t *head, size_t n)
{
    UNUSED(path);
    return w32_is_pe(head, n);
}

extern uint8_t __start_w32state[], __stop_w32state[];

#define KERNEL_STACK (64u * 1024)

struct kernel_call {
    uint32_t (*fn)(void *);
    void *arg;
};

uint32_t w32_kernel_call(uint32_t (*fn)(void *), void *arg)
{
    uint8_t *stack = kmalloc(KERNEL_STACK);
    if (!stack)
        return (uint32_t)E_NOMEM;
    uint32_t r = arch_call_on_stack(fn, arg, stack + KERNEL_STACK);
    kfree(stack);
    return r;
}

static void map_process(struct w32_process *p, bool on)
{
    for (struct w32_image *img = p->images; img; img = img->next)
        if (img->memory) {
            if (on)
                vmm_map(img->base, img->memory, img->size);
            else
                vmm_unmap(img->base, img->size);
        }
    if (on && p->arena_frames)
        vmm_map_frames(p->arena_base, p->arena_frames, p->arena_size / 4096);
    else if (on)
        vmm_map(p->arena_base, p->arena, p->arena_size);
    else
        vmm_unmap(p->arena_base, p->arena_size);
}

static int run_process(struct exec_ctx *ctx)
{
    w32 = kzalloc(sizeof(*w32));
    if (!w32)
        return E_NOMEM;
    uint8_t saved_attr = con_attr();
    int rc = run(ctx);
    bool started = w32->main != NULL;
    con_set_attr(saved_attr);
    con_show_cursor(true);
    con_flush();
    if (w32->message[0])
        sh_error(ctx->sh, path_basename(ctx->path), "%s", w32->message);
    cleanup();
    kfree(w32);
    w32 = NULL;
    line_len = line_pos = 0;
    if (!started)
        return rc;
    uint32_t code = (uint32_t)rc;
    return code <= 255 ? (int)code : (code & 0xff) ? (int)(code & 0xff) : 255;
}

static int run_nested(struct exec_ctx *ctx)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (sp >= VMM_USER_START && sp < VMM_USER_END)
        return sh_error(ctx->sh, path_basename(ctx->path), "cannot start a Windows program from here");
    struct w32_process *parent = w32;
    size_t state_size = (size_t)(__stop_w32state - __start_w32state);
    uint8_t *state = kmalloc(state_size);
    if (!state)
        return E_NOMEM;
    memcpy(state, __start_w32state, state_size);
    uint16_t fpu_cw;
    __asm__ volatile("fnstcw %0" : "=m"(fpu_cw));
    map_process(parent, false);
    int rc = run_process(ctx);
    map_process(parent, true);
    memcpy(__start_w32state, state, state_size);
    kfree(state);
    __asm__ volatile("fldcw %0" : : "m"(fpu_cw));
    w32 = parent;
    arch_w32_set_teb((uint32_t)parent->teb);
    if (parent->teb64)
        arch_w64_set_teb((uint32_t)parent->teb64);
    arch_set_fault_handler(on_fault);
    arch_set_tick_hook(on_tick);
    return rc;
}

static int pe_run(struct exec_ctx *ctx, uint8_t *image, size_t size)
{
    UNUSED(image);
    if (!vmm_available())
        return E_NOSYS;
    image_reserve = size * 3 + 256 * 1024;
    image_win64 = w32_is_pe64(image, size);
    return w32 ? run_nested(ctx) : run_process(ctx);
}

static const struct binfmt pe_format = {
    .name = "pe",
    .description = "Windows program (Win32/Win64 PE)",
    .extensions = ".exe",
    .probe = pe_probe,
    .run = pe_run,
};
BINFMT(pe_format);
