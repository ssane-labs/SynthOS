#include <synth/codepage.h>
#include <synth/command.h>
#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/input.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/stream.h>
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

static bool log_errors;

bool w32_logging_errors(void)
{
    return log_errors;
}

void w32_set_error(DWORD error)
{
    if (log_errors && error && w32 && w32->current) {
        const char *api = w32->current->recent[(w32->current->recent_pos + 7) % 8];
        klog("win32: pid %u tid %u %s failed with %u", w32->child ? w32->child->pid : 0, w32->current->id, api ? api : "?",
             (unsigned)error);
    }
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

void w32_object_release(struct w32_object *o)
{
    if (o->file)
        vfs_close(o->file);
    if (o->delete_on_close && o->path) {
        w32_fcache_forget(o->path);
        vfs_unlink(o->path);
    }
    o->delete_on_close = false;
    if (o->find)
        kfree(o->find);
    kfree(o->path);
    o->path = NULL;
    if (o->type == WH_PIPE)
        w32_pipe_release(o);
    if (o->section)
        w32_section_release(o->section);
    o->section = NULL;
    if (o->child)
        w32_child_release(o->child);
    o->child = NULL;
    kfree(o->port);
    o->port = NULL;
    o->due = 0;
    o->type = WH_FREE;
    o->file = NULL;
    o->find = NULL;
}

bool w32_handle_close(HANDLE h)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return false;
    if (--o->refs > 0)
        return true;
    if (o->type == WH_PIPE)
        w32_io_drop(h);
    w32_object_release(o);
    return true;
}

bool w32_object_clone(struct w32_object *dst, const struct w32_object *src)
{
    if (src->type == WH_FIND || src->type == WH_PORT || src->type == WH_THREAD || src->type == WH_FREE ||
        src->type <= WH_STDERR)
        return false;
    *dst = *src;
    dst->refs = 1;
    dst->delete_on_close = false;
    dst->path = src->path ? kstrdup(src->path) : NULL;
    dst->file = NULL;
    if (src->file) {
        if (!src->path || vfs_open(src->path, src->file->flags & ~(unsigned)(VFS_CREATE | VFS_TRUNC), &dst->file)) {
            kfree(dst->path);
            return false;
        }
        vfs_seek(dst->file, (long)src->file->pos, VFS_SEEK_SET);
    }
    if (src->type == WH_PIPE && !w32_pipe_clone(dst)) {
        if (dst->file)
            vfs_close(dst->file);
        kfree(dst->path);
        return false;
    }
    if (dst->section)
        w32_section_retain(dst->section);
    if (dst->child)
        dst->child->refs++;
    return true;
}

struct w32_inherit *w32_inherit_collect(const HANDLE *list, int count)
{
    int n = 0;
    for (int i = 3; i < W32_MAX_HANDLES; i++)
        if (w32->handles[i].type != WH_FREE)
            n++;
    struct w32_inherit *in = kzalloc(sizeof(*in) + (size_t)n * sizeof(in->items[0]));
    if (!in)
        return NULL;
    for (int i = 3; i < W32_MAX_HANDLES && in->count < n; i++) {
        HANDLE h = (HANDLE)(uintptr_t)(HANDLE_BASE + (uintptr_t)i * 4);
        const struct w32_object *o = &w32->handles[i];
        bool wanted = list ? false : o->inherit;
        for (int k = 0; list && k < count; k++)
            wanted |= list[k] == h;
        if (wanted && o->type != WH_FREE && w32_object_clone(&in->items[in->count].object, o))
            in->items[in->count++].index = (uint32_t)i;
    }
    return in;
}

HANDLE w32_inheritable(HANDLE h, const void *sa)
{
    struct w32_object *o = w32_handle_get(h);
    if (o && sa)
        o->inherit = ((const uint32_t *)sa)[2] != 0;
    return h;
}

void w32_inherit_free(struct w32_inherit *in)
{
    for (int i = 0; in && i < in->count; i++)
        if (in->items[i].object.type != WH_FREE)
            w32_object_release(&in->items[i].object);
    kfree(in);
}

static void install_inherited(struct w32_inherit *in)
{
    for (int i = 0; in && i < in->count; i++) {
        uint32_t index = in->items[i].index;
        if (index < W32_MAX_HANDLES && w32->handles[index].type == WH_FREE) {
            w32->handles[index] = in->items[i].object;
            memset(&in->items[i].object, 0, sizeof(in->items[i].object));
        }
    }
    w32_inherit_free(in);
}

HANDLE w32_handle_put(struct w32_process *p, const struct w32_object *o)
{
    for (int i = 3; i < W32_MAX_HANDLES; i++)
        if (p->handles[i].type == WH_FREE) {
            if (!w32_object_clone(&p->handles[i], o))
                return NULL;
            return (HANDLE)(uintptr_t)(HANDLE_BASE + i * 4);
        }
    return NULL;
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
        if (input_break_pending() && !w32->detached) {
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
    w32_trace_dump();
    uint32_t r[8];
    arch_fault_registers(r);
    klog("win32: fault %d in %s thread %u at %08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
         vector, w32->parent ? "child" : "main", w32->current ? w32->current->id : 0, eip, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    uint32_t fp = r[6];
    for (int i = 0; i < 12 && fp >= VMM_USER_START && (fp >= 0xc0000000u ? fp < 0xfff00000u : vmm_mapped(fp + 4)); i++) {
        klog("win32:   called from %08x", ((uint32_t *)fp)[1]);
        fp = ((uint32_t *)fp)[0];
    }
    char where[96] = "";
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (!img->builtin && eip >= img->base && eip - img->base < img->size)
            ksnprintf(where, sizeof(where), " (%s+0x%x)", img->name, eip - img->base);
    if (vector == 14)
        ksnprintf(w32->message, sizeof(w32->message), "crashed: %s at 0x%08x%s (address 0x%08x)", fault_name(vector),
                  eip, where, address);
    else
        ksnprintf(w32->message, sizeof(w32->message), "crashed: %s at 0x%08x%s", fault_name(vector), eip, where);
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
    if (w32->current)
        w32->current->user_eip = eip;
    if (ticks % 32 == 0) {
        input_poll();
        if (input_break_pending()) {
            w32->exiting = true;
            w32->exit_code = STATUS_CONTROL_C_EXIT;
            arch_w32_abort(&w32->jump, STATUS_CONTROL_C_EXIT);
        }
    }
    if (ticks % 4 == 0)
        w32_signal_tick();
    if (ticks % 10 == 0)
        w32_preempt();
    else if (ticks % 2 == 0 && hal_input_pending())
        w32_input_boost();
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

/* Chromium's sandbox needs restricted tokens and job objects, its GPU process needs Direct3D: chrome.exe started as
   a browser gets the switches that run it without them. */
static const char *const chromium_switches[] = {"--no-sandbox", "--test-type", "--in-process-gpu", "--disable-gpu",
                                                "--disable-software-rasterizer", "--no-first-run",
                                                "--no-default-browser-check"};

/* Its child processes quit when the browser has not connected to them within 15 seconds; a busy machine (or one
   without hardware virtualization) needs longer, or the network service dies and pages never load. */
static const char *const chromium_child_switches[] = {"--ipc-connection-timeout=600"};

static bool has_arg(struct exec_ctx *ctx, const char *arg, size_t n)
{
    for (int i = 1; i < ctx->argc; i++)
        if (!strncmp(ctx->argv[i], arg, n))
            return true;
    return false;
}

static int command_args(struct exec_ctx *ctx, const char **list)
{
    int n = 0;
    bool chrome = !str_casecmp(path_basename(ctx->path), "chrome.exe"), child = has_arg(ctx, "--type=", 7);
    if (chrome && !child)
        for (size_t i = 0; i < ARRAY_SIZE(chromium_switches); i++)
            if (!has_arg(ctx, chromium_switches[i], strlen(chromium_switches[i]) + 1))
                list[n++] = chromium_switches[i];
    if (chrome && child && !has_arg(ctx, "--ipc-connection-timeout", 24))
        for (size_t i = 0; i < ARRAY_SIZE(chromium_child_switches); i++)
            list[n++] = chromium_child_switches[i];
    for (int i = 1; i < ctx->argc; i++)
        list[n++] = ctx->argv[i];
    return n;
}

static void build_command_line(struct exec_ctx *ctx)
{
    const char **list = w32_zalloc(sizeof(char *) * (size_t)(ctx->argc + ARRAY_SIZE(chromium_switches) + 1));
    int count = list ? command_args(ctx, list) : 0;
    size_t cap = PATH_MAX_LEN + 16;
    for (int i = 0; i < count; i++)
        cap += strlen(list[i]) * 2 + 3;
    char *line = w32_alloc(cap);
    char exe[PATH_MAX_LEN];
    w32_path_to_win(ctx->path, exe, sizeof(exe));
    w32->argc = count + 1;
    w32->argv = w32_zalloc(sizeof(char *) * (size_t)(w32->argc + 1));
    w32->argv[0] = w32_strdup(exe);
    ksnprintf(line, cap, "\"%s\"", exe);
    for (int i = 0; i < count; i++) {
        const char *arg = list[i];
        w32->argv[i + 1] = w32_strdup(arg);
        str_append(line, " ", cap);
        if (!needs_quotes(arg)) {
            str_append(line, arg, cap);
            continue;
        }
        str_append(line, "\"", cap);
        for (const char *p = arg; *p; p++) {
            if (*p == '"')
                str_append(line, "\\", cap);
            char ch[2] = {*p, 0};
            str_append(line, ch, cap);
        }
        str_append(line, "\"", cap);
    }
    w32_free(list);
    w32->cmdline = line;
    w32->cmdline_w = w32_widen(line);
    const char *type = strstr(line, "--type=");
    if (type) {
        str_copy(w32->tag, type + 7, sizeof(w32->tag));
        char *end = strchr(w32->tag, ' ');
        if (end)
            *end = 0;
    }
    w32->wargv = w32_zalloc(sizeof(WCHAR *) * (size_t)(w32->argc + 1));
    for (int i = 0; i < w32->argc; i++)
        w32->wargv[i] = w32_widen(w32->argv[i]);
}

static void inherit_environment(const char *src)
{
    size_t len = 0;
    int count = 0;
    while (src[len]) {
        len += strlen(src + len) + 1;
        count++;
    }
    char *block = w32_zalloc(len + 2);
    memcpy(block, src, len);
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

static void build_environment(struct exec_ctx *ctx)
{
    if (ctx->env && ctx->env[0]) {
        inherit_environment(ctx->env);
        return;
    }
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
              "GOOGLE_API_KEY=no\n"
              "GOOGLE_DEFAULT_CLIENT_ID=no\n"
              "GOOGLE_DEFAULT_CLIENT_SECRET=no\n"
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
    uint8_t *loader_lock = w32_zalloc(0x18);
    uint32_t *heaps = w32_zalloc(16 * 4);
    if (!w32->teb || !w32->peb || !params || !ldr || !loader_lock || !heaps)
        return false;
    memset(w32->teb, 0, 4096);
    memset(w32->peb, 0, 4096);
    put32(loader_lock, 0x04, 0xffffffffu);
    heaps[0] = (uint32_t)&w32->heap;
    uint32_t stack_top = (uint32_t)w32->stack + w32->stack_size;
    put32(w32->teb, 0x00, 0xffffffffu);
    put32(w32->teb, 0x04, stack_top);
    put32(w32->teb, 0x08, (uint32_t)w32->stack);
    put32(w32->teb, 0x18, (uint32_t)w32->teb);
    put32(w32->teb, 0x20, w32_current_pid());
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
    put32(w32->peb, 0x88, 1);
    put32(w32->peb, 0x8c, 16);
    put32(w32->peb, 0x90, (uint32_t)heaps);
    put32(w32->peb, 0xa0, (uint32_t)loader_lock);
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


static bool image_win64;

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
    w32_vm_cleanup();
    w32_unload_all();
    vmm_release(w32->arena_base, w32->arena_size);
    vmm_space_set_commit(0, 0);
    kfree_pages(w32->stack);
    w32->stack = NULL;
}

static int run(struct exec_ctx *ctx)
{
    w32->ctx = ctx;
    w32->win64 = image_win64;
    if (w32->win64 && !arch_long_supported()) {
        ksnprintf(w32->message, sizeof(w32->message), "is a 64-bit program and this processor has no 64-bit mode");
        return 1;
    }
    str_copy(w32->cwd, ctx->cwd ? ctx->cwd : ctx->sh->cwd, sizeof(w32->cwd));
    w32->default_attr = theme_attr(ROLE_TEXT);
    w32->console_attr = w32->default_attr;
    w32->output_cp = w32->input_cp = codepage_current();
    w32->input_mode = 0x1f7;
    w32->output_mode = 0x7;
    w32->arena_base = ARENA_BASE;
    w32->arena_size = ARENA_HIGH_MAX;
    vmm_space_set_commit(ARENA_BASE, w32->arena_size);
    heap_create(&w32->heap, (void *)ARENA_BASE, w32->arena_size, false);

    w32->stack_size = STACK_SIZE;
    w32->stack = kmalloc_pages_uninit(w32->stack_size);
    for (int i = 0; i < 3; i++) {
        w32->handles[i].type = (enum w32_object_type)(WH_STDIN + i);
        w32->handles[i].refs = 1 << 20;
    }
    install_inherited(ctx->handles);
    ctx->handles = NULL;
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
    vmm_set_commit_hook(w32_vm_committed);
    vmm_set_map_hook(w32_vm_fault);
    if (w32_init_modules() == 0 && !w32->exiting) {
        if (w32->win64)
            w64_setup_tls(main);
        else
            w32_tls_callbacks(main->base, 1);
    }
    uint32_t code = w32->exit_code;
    if (!w32->exiting) {
        code = w32->win64 ? w64_run_main(main->entry) : w32_call((void *)main->entry, 0, NULL);
        if (!w32->exiting)
            w32->exit_code = code;
    }
    if (!w32->ring_next || w32->ring_next == w32) {
        arch_set_tick_hook(NULL);
        arch_set_fault_handler(NULL);
    }
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

/* Programs started by the shell see it as their parent process. */
#define SHELL_PID 0x1000

static struct w32_process *ring;
static uint32_t next_pid = 0x2000;

static size_t state_size(void)
{
    return (size_t)(__stop_w32state - __start_w32state);
}

struct w32_object *w32_handle_get_in(struct w32_process *p, HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (!p || v < HANDLE_BASE || (v - HANDLE_BASE) % 4)
        return NULL;
    uintptr_t i = (v - HANDLE_BASE) / 4;
    if (i >= W32_MAX_HANDLES || p->handles[i].type == WH_FREE)
        return NULL;
    return &p->handles[i];
}

struct w32_process *w32_process_by_pid(uint32_t pid)
{
    struct w32_process *p = w32;
    do {
        if (p->child && p->child->pid == pid)
            return p;
        p = p->ring_next;
    } while (p && p != w32);
    return NULL;
}

uint32_t w32_current_pid(void)
{
    return w32 && w32->child ? w32->child->pid : 0x100;
}

static void where(struct w32_process *p, uint32_t a, char *out, size_t cap)
{
    for (struct w32_image *img = p->images; img; img = img->next)
        if (a >= img->base && a < img->base + img->size) {
            ksnprintf(out, cap, "%s+0x%x", img->name, a - img->base);
            return;
        }
    ksnprintf(out, cap, "0x%08x", a);
}

static const struct w32_image *code_image(struct w32_process *p, uint32_t a)
{
    for (const struct w32_image *img = p->images; img; img = img->next)
        if (!img->builtin && a >= img->base + 8 && a < img->base + img->size)
            return img;
    return NULL;
}

static bool after_call(struct w32_process *p, uint32_t a)
{
    if (!code_image(p, a) || !vmm_mapped(a - 8) || !vmm_mapped(a - 1))
        return false;
    const uint8_t *c = (const uint8_t *)(uintptr_t)a;
    return c[-5] == 0xe8 || (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) || (c[-2] == 0xff && (c[-1] & 0xf8) == 0xd0) ||
           (c[-3] == 0xff && (c[-2] & 0x38) == 0x10);
}

/* Likely return addresses above where a waiting thread left program code: its call chain, read on demand. */
static int call_trail(struct w32_process *p, const struct w32_thread *t, uint32_t *out, int cap)
{
    if (!t->wait_caller || !t->wait_sp || t->done)
        return 0;
    struct vmm_space *old = vmm_space_current();
    if (p->space && p->space != old)
        vmm_space_switch(p->space);
    const uint32_t *sp = (const uint32_t *)(uintptr_t)t->wait_sp;
    const uint32_t *end = t->stack ? (const uint32_t *)(t->stack + t->stack_size) : sp + 4096;
    int k = 0;
    for (int n = 0; n < 4096 && sp < end && k < cap; n++, sp++)
        if (after_call(p, *sp))
            out[k++] = *sp;
    if (vmm_space_current() != old)
        vmm_space_switch(old);
    return k;
}

static struct shell *dump_sh;

static void dump_line(const char *line)
{
    if (dump_sh) {
        sh_puts(dump_sh, line);
        size_t n = strlen(line);
        klog("w32ps: %.*s", (int)(n && line[n - 1] == '\n' ? n - 1 : n), line);
    }
    else
        w32_console_write(WH_STDERR, line, strlen(line));
}

void w32_dump_status(void)
{
    char line[160], at[96];
    struct w32_process *p = ring;
    if (!p)
        return;
    do {
        ksnprintf(line, sizeof(line), "pid %u %s %s: %d live threads%s%s\n", p->child ? p->child->pid : 0,
                  p->main ? p->main->name : "?", p->tag, p->live_threads, p->frozen ? " frozen" : "",
                  p->dead ? " dead" : "");
        dump_line(line);
        struct w32_thread *t = p->threads;
        if (t)
            do {
                where(p, t->wait_caller, at, sizeof(at));
                ksnprintf(line, sizeof(line), "  tid %u %s%s from %s\n", t->id,
                          t->done ? "done" : t->waiting ? "waiting" : "running", t->suspended ? " suspended" : "", at);
                dump_line(line);
                if (!t->waiting && t->user_eip) {
                    where(p, t->user_eip, at, sizeof(at));
                    ksnprintf(line, sizeof(line), "      at %s\n", at);
                    dump_line(line);
                }
                uint32_t trail[16];
                int n = t->waiting ? call_trail(p, t, trail, 16) : 0;
                for (int k = 0; k < n; k++) {
                    where(p, trail[k], at, sizeof(at));
                    ksnprintf(line, sizeof(line), "      <- %s\n", at);
                    dump_line(line);
                }
                for (unsigned r = 0; r < 8; r++) {
                    const char *name = t->recent[(t->recent_pos + r) % 8];
                    if (name && !t->done) {
                        ksnprintf(line, sizeof(line), "      %s\n", name);
                        dump_line(line);
                    }
                }
            } while ((t = t->next) != p->threads);
    } while ((p = p->ring_next) && p != ring);
    w32_pipe_dump();
}

COMMAND(w32ps, NULL, "system", "w32ps", "list the running Windows processes and what each thread waits for")
{
    UNUSED(argc);
    UNUSED(argv);
    if (!ring) {
        sh_puts(sh, "no Windows programs are running\n");
        return 0;
    }
    dump_sh = sh;
    w32_dump_status();
    dump_sh = NULL;
    return 0;
}

struct w32_process *w32_ring_next(struct w32_process *p)
{
    return p ? p->ring_next : NULL;
}

static void ring_add(struct w32_process *p)
{
    if (!ring) {
        p->ring_next = p;
        ring = p;
        return;
    }
    p->ring_next = ring->ring_next;
    ring->ring_next = p;
}

static void ring_remove(struct w32_process *p)
{
    if (!ring || !p->ring_next)
        return;
    if (p->ring_next == p) {
        ring = NULL;
    } else {
        struct w32_process *q = ring;
        while (q->ring_next != p)
            q = q->ring_next;
        q->ring_next = p->ring_next;
        if (ring == p)
            ring = q;
    }
    p->ring_next = NULL;
}

void w32_process_enter(struct w32_process *p)
{
    if (p == w32)
        return;
    if (w32) {
        if (!w32->state)
            w32->state = kmalloc(state_size());
        if (w32->state)
            memcpy(w32->state, __start_w32state, state_size());
    }
    vmm_space_switch(p->space);
    if (p->state)
        memcpy(__start_w32state, p->state, state_size());
    w32 = p;
}

void w32_child_release(struct w32_child *c)
{
    if (c && --c->refs <= 0)
        kfree(c);
}

static void finish_child(struct w32_process *p, uint32_t code)
{
    w32_kick_all();
    if (p->child)
        klog("win32: pid %u %s exited with 0x%08x%s%s", p->child->pid, p->tag, code, p->message[0] ? ": " : "",
             p->message);
    if (p->child) {
        p->child->done = true;
        p->child->exit_code = code;
        p->child->proc = NULL;
        w32_child_release(p->child);
        p->child = NULL;
    }
    if (p->spawn_ctx) {
        struct exec_ctx *c = p->spawn_ctx;
        if (p->own_streams & 1)
            stream_close(c->in);
        if (p->own_streams & 2)
            stream_close(c->out);
        if (p->own_streams & 4)
            stream_close(c->err);
        p->own_streams = 0;
    }
    p->dead = true;
}

static void free_process(struct w32_process *p)
{
    w32_io_forget(p);
    ring_remove(p);
    vmm_space_destroy(p->space);
    kfree(p->boot_stack);
    kfree(p->boot);
    kfree(p->state);
    if (p->spawn_ctx) {
        for (int i = 0; i < p->spawn_ctx->argc; i++)
            kfree(p->spawn_ctx->argv[i]);
        kfree(p->spawn_ctx->argv);
        kfree((char *)p->spawn_ctx->path);
        kfree((char *)p->spawn_ctx->env);
        kfree((char *)p->spawn_ctx->cwd);
        kfree(p->spawn_ctx);
    }
    kfree(p);
}

void w32_reap(void)
{
    bool again = true;
    while (again && ring) {
        again = false;
        struct w32_process *p = ring;
        do {
            if (p->dead && p != w32) {
                free_process(p);
                again = true;
                break;
            }
            p = p->ring_next;
        } while (p != ring);
    }
}

static void kill_process(struct w32_process *p, uint32_t code)
{
    struct w32_process *self = w32;
    w32_process_enter(p);
    w32->exiting = true;
    cleanup();
    w32_threads_end();
    finish_child(p, code);
    w32_process_enter(self);
}

static void kill_children(struct w32_process *parent)
{
    bool again = true;
    while (again) {
        again = false;
        for (struct w32_process *p = ring; p; p = p->ring_next) {
            if (p != parent && !p->dead && p->parent == parent) {
                kill_children(p);
                kill_process(p, 1);
                again = true;
                break;
            }
            if (p->ring_next == ring)
                break;
        }
    }
}

void w32_terminate_child(struct w32_child *c, uint32_t code)
{
    if (!c || c->done || !c->proc)
        return;
    struct w32_process *p = c->proc;
    if (p == w32) {
        w32_exit(code);
        return;
    }
    kill_children(p);
    kill_process(p, code);
}

static int exit_status(int rc, bool started)
{
    if (!started)
        return rc;
    uint32_t code = (uint32_t)rc;
    return code <= 255 ? (int)code : (code & 0xff) ? (int)(code & 0xff) : 255;
}

static uint8_t *pristine_state;

static int run_process(struct exec_ctx *ctx)
{
    struct w32_process *parent = w32;
    struct w32_process *p = kzalloc(sizeof(*p));
    if (!p)
        return E_NOMEM;
    if (pristine_state)
        memcpy(__start_w32state, pristine_state, state_size());
    p->space = vmm_space_create();
    if (!p->space) {
        kfree(p);
        return E_NOMEM;
    }
    struct vmm_space *outer = vmm_space_current();
    p->parent = parent;
    p->parent_pid = parent && parent->child ? parent->child->pid : SHELL_PID;
    if ((p->child = kzalloc(sizeof(*p->child)))) {
        next_pid += 8;
        p->child->refs = 1;
        p->child->pid = next_pid;
        p->child->proc = p;
    }
    w32 = p;
    ring_add(p);
    vmm_space_switch(p->space);
    uint8_t saved_attr = con_attr();
    int rc = run(ctx);
    bool started = w32->main != NULL;
    con_set_attr(saved_attr);
    con_show_cursor(true);
    con_flush();
    if (w32->message[0])
        sh_error(ctx->sh, path_basename(ctx->path), "%s", w32->message);
    cleanup();
    kill_children(p);
    if (p->child) {
        p->child->done = true;
        p->child->exit_code = (uint32_t)w32->exit_code;
        p->child->proc = NULL;
        w32_child_release(p->child);
        p->child = NULL;
    }
    vmm_space_switch(outer);
    w32_io_forget(p);
    ring_remove(p);
    w32 = parent;
    w32_reap();
    vmm_space_destroy(p->space);
    kfree(p->state);
    kfree(p);
    line_len = line_pos = 0;
    return exit_status(rc, started);
}

static int run_nested(struct exec_ctx *ctx)
{
    struct w32_process *parent = w32;
    uint8_t *state = kmalloc(state_size());
    if (!state)
        return E_NOMEM;
    memcpy(state, __start_w32state, state_size());
    uint16_t fpu_cw;
    __asm__ volatile("fnstcw %0" : "=m"(fpu_cw));
    parent->frozen = true;
    int rc = run_process(ctx);
    parent->frozen = false;
    memcpy(__start_w32state, state, state_size());
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

#define BOOT_STACK (64u * 1024)

static void child_entry(void)
{
    struct w32_process *p = w32;
    arch_set_irq_depth(0);
    image_win64 = false;
    int rc = run(p->spawn_ctx);
    bool started = w32->main != NULL;
    if (w32->message[0])
        stream_printf(p->spawn_ctx->err, "%s: %s\n", path_basename(p->spawn_ctx->path), w32->message);
    cleanup();
    kill_children(p);
    finish_child(p, (uint32_t)exit_status(rc, started));
    w32_leave_forever();
}

struct w32_child *w32_spawn_async(struct exec_ctx *ctx, unsigned own)
{
    struct w32_process *p = kzalloc(sizeof(*p));
    struct w32_child *c = kzalloc(sizeof(*c));
    struct w32_thread *boot = kzalloc(sizeof(*boot));
    uint8_t *stack = kmalloc(BOOT_STACK);
    uint8_t *state = kmalloc(state_size());
    struct vmm_space *space = vmm_space_create();
    if (!p || !c || !boot || !stack || !state || !space) {
        kfree(p);
        kfree(c);
        kfree(boot);
        kfree(stack);
        kfree(state);
        vmm_space_destroy(space);
        return NULL;
    }
    memcpy(state, pristine_state ? pristine_state : __start_w32state, state_size());
    next_pid += 8;
    c->refs = 2;
    c->pid = next_pid;
    c->proc = p;
    p->space = space;
    p->state = state;
    p->boot_stack = stack;
    p->boot = boot;
    p->child = c;
    p->parent = w32;
    p->spawn_ctx = ctx;
    p->own_streams = own;
    p->detached = w32 && (w32_is_host(w32) || w32->detached);
    p->parent_pid = w32 && w32->child ? w32->child->pid : SHELL_PID;
    boot->proc = p;
    boot->boot = true;
    boot->id = 0x104;
    boot->next = boot;
    w32_thread_frame(boot, child_entry, stack + BOOT_STACK);
    p->threads = p->current = boot;
    p->live_threads = 1;
    ring_add(p);
    w32_switch_thread(boot);
    return c;
}

/* Programs started from the desktop run without a terminal: the shell lends them its idle time through a host
   process that sits in the ring only while it waits for them. */
static struct w32_process *host;
static struct w32_thread host_thread;

bool w32_is_host(const struct w32_process *p)
{
    return p && p == host;
}

struct w32_thread *w32_host_thread(void)
{
    return host && host->ring_next && w32 != host ? &host_thread : NULL;
}

static bool host_enter(void)
{
    if (!host && !(host = kzalloc(sizeof(*host))))
        return false;
    host->space = vmm_space_current();
    host->threads = host->current = &host_thread;
    host->live_threads = 1;
    host_thread.next = &host_thread;
    host_thread.proc = host;
    host_thread.boot = true;
    host_thread.id = 0x100;
    host_thread.waiting = false;
    w32 = host;
    ring_add(host);
    return true;
}

static void host_leave(void)
{
    ring_remove(host);
    w32 = NULL;
}

void w32_background(void)
{
    if (w32 || !ring || hal_in_interrupt() || !host_enter())
        return;
    w32_kick_all();
    w32_yield();
    host_leave();
}

int w32_start(struct shell *sh, const char *path, int argc, char **argv, const char *cwd)
{
    if (w32)
        return E_BUSY;
    if (!vmm_available())
        return E_NOSYS;
    struct file *f;
    uint8_t head[512];
    long n = vfs_open(path, VFS_READ, &f) == 0 ? vfs_read(f, head, sizeof(head)) : -1;
    if (n >= 0)
        vfs_close(f);
    if (n < 64 || !w32_is_pe(head, (size_t)n) || w32_is_pe64(head, (size_t)n))
        return E_NOEXEC;
    uint32_t pe = (uint32_t)head[0x3c] | (uint32_t)head[0x3d] << 8;
    if (pe + 24 + 70 > (uint32_t)n || head[pe + 24 + 68] != 2)
        return E_NOEXEC;
    if (!pristine_state && (pristine_state = kmalloc(state_size())))
        memcpy(pristine_state, __start_w32state, state_size());
    struct exec_ctx *c = kzalloc(sizeof(*c));
    char **copy = kzalloc(sizeof(char *) * (size_t)(argc + 1));
    if (!c || !copy) {
        kfree(c);
        kfree(copy);
        return E_NOMEM;
    }
    for (int i = 0; i < argc; i++)
        copy[i] = kstrdup(argv[i]);
    *c = (struct exec_ctx){sh, kstrdup(path), argc, copy, stream_null(), stream_null(), stream_log(), NULL,
                           kstrdup(cwd ? cwd : sh->cwd), NULL};
    struct w32_child *child = NULL;
    if (host_enter()) {
        child = w32_spawn_async(c, 0);
        host_leave();
    }
    if (!child) {
        for (int i = 0; i < argc; i++)
            kfree(copy[i]);
        kfree(copy);
        kfree((char *)c->path);
        kfree((char *)c->cwd);
        kfree(c);
        return E_NOMEM;
    }
    w32_child_release(child);
    return 0;
}

static int pe_run(struct exec_ctx *ctx, uint8_t *image, size_t size)
{
    UNUSED(image);
    if (!vmm_available())
        return E_NOSYS;
    if (!pristine_state && !w32 && (pristine_state = kmalloc(state_size())))
        memcpy(pristine_state, __start_w32state, state_size());
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

static void get_trace(char *buf, size_t cap)
{
    str_copy(buf, w32_trace ? "on" : "off", cap);
}

static int set_trace(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    w32_trace = !strcmp(value, "on");
    return 0;
}

static const struct setting s_trace = {"w32trace", "on|off",
                                       "remember the last Windows API calls and log them when a program crashes",
                                       get_trace, set_trace};
SETTING(s_trace);

/* config w32errors on: every API call that sets an error code is logged with its name (it turns w32trace on). */
static void get_errors(char *buf, size_t cap)
{
    str_copy(buf, log_errors ? "on" : "off", cap);
}

static int set_errors(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    static bool traced;
    bool on = !strcmp(value, "on");
    if (on && !log_errors)
        traced = w32_trace;
    if (log_errors && !on)
        w32_trace = traced;
    log_errors = on;
    if (on)
        w32_trace = true;
    return 0;
}

static const struct setting s_errors = {"w32errors", "on|off", "log every Windows API call that fails, with its name",
                                        get_errors, set_errors};
SETTING(s_errors);

bool w32_headless;

static void get_headless(char *buf, size_t cap)
{
    str_copy(buf, w32_headless ? "on" : "off", cap);
}

static int set_headless(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    w32_headless = !strcmp(value, "on");
    return 0;
}

static const struct setting s_headless = {"w32headless", "on|off",
                                          "let Windows programs create windows without the desktop (headless mode)",
                                          get_headless, set_headless};
SETTING(s_headless);
