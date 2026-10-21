#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "crt/crt.h"
#include "win64.h"

SYNTH_TABLE_DECLARE(synth_win64, struct w64_module);

#define MAX_ARGS 16
#define ONEXIT_MAX 128
#define SIGNALS 32
#define TEB64_SIZE 8192
#define TLS_SLOTS 64
#define FILES 64

extern void *w64_dispatch_fn;
extern char w64_gate[], w64_setjmp[], w64_longjmp[], w64_memcpy[], w64_memmove[], w64_memset[], w64_strlen[];
uint64_t w64_strtod(uint64_t *a, const uint64_t *f, void *target, bool wide, bool single);
uint64_t w64_atof(void *target, uint64_t s);

struct w64_stub {
    uint8_t code[16];
    w64_handler fn;
    void *target;
    uint32_t flags;
    const char *module;
    const char *name;
    struct w64_stub *next;
};

struct lconv64 {
    uint64_t text[10];
    char values[8];
    uint64_t wide[8];
};

struct file64 {
    uint8_t file[48];
    uint8_t lock[40];
    uint32_t real;
    uint32_t used;
};

struct w64_state {
    struct w64_stub *stubs;
    uint8_t iob[3][48];
    struct file64 files[FILES];
    uint64_t argv, wargv, environ, wenviron, initenv, winitenv, acmdln, wcmdln, pgmptr, wpgmptr, pctype, pwctype;
    uint64_t tzname[2];
    uint64_t onexit[ONEXIT_MAX];
    int onexit_count;
    uint64_t signals[SIGNALS];
    uint64_t filter;
    struct lconv64 lconv;
    struct tls64_module {
        uint64_t start, end, callbacks, base;
        uint32_t zero;
    } tls[TLS_SLOTS];
    int tls_count;
    uint64_t image_base;
};

struct thread_start {
    uint64_t fn;
    uint64_t param;
    bool void_result;
};

#define S ((struct w64_state *)w32->w64)
#define P(v) w64_ptr(v)
#define U(v) ((uint32_t)(uintptr_t)(v))

static uint32_t rd32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void put16(void *p, uint16_t v)
{
    memcpy(p, &v, 2);
}

static void put32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static void out64(uint64_t addr, uint64_t v)
{
    if (addr)
        put64(P(addr), v);
}

static uint32_t narrow(uint64_t v)
{
    uintptr_t base = (uintptr_t)S->iob;
    if (v >= base && v < base + sizeof(S->iob))
        return U(&crt_iob[(v - base) / 48]);
    base = (uintptr_t)S->files;
    if (v >= base && v < base + sizeof(S->files))
        return S->files[(v - base) / sizeof(struct file64)].real;
    return U(P(v));
}

static uint64_t wrap_file(uint32_t real)
{
    if (!real)
        return 0;
    for (int i = 0; i < 3; i++)
        if (real == U(&crt_iob[i]))
            return U(S->iob[i]);
    struct file64 *free_slot = NULL;
    for (int i = 0; i < FILES; i++) {
        if (S->files[i].used && S->files[i].real == real)
            return U(&S->files[i]);
        if (!S->files[i].used && !free_slot)
            free_slot = &S->files[i];
    }
    if (!free_slot)
        return real;
    const CRT_FILE *f = w64_ptr(real);
    memset(free_slot, 0, sizeof(*free_slot));
    put32(free_slot->file + 0x18, (uint32_t)f->flag);
    put32(free_slot->file + 0x1c, (uint32_t)f->file);
    free_slot->real = real;
    free_slot->used = 1;
    return U(free_slot);
}

static void release_file(uint64_t v)
{
    uintptr_t base = (uintptr_t)S->files;
    if (v >= base && v < base + sizeof(S->files))
        S->files[(v - base) / sizeof(struct file64)].used = 0;
}

#define OUT_UNSET 0xfeedf00du

static NORETURN void missing(struct w64_stub *s)
{
    ksnprintf(w32->message, sizeof(w32->message), "%s!%s is not supported yet (64-bit)", s->module, s->name);
    w32_exit(STATUS_ENTRYPOINT_NOT_FOUND);
}

static uint64_t dispatch(struct w64_stub *s, uint64_t *a, const uint64_t *f)
{
    if (w32->current)
        w32->current->recent[w32->current->recent_pos++ % 8] = s->name;
    if (s->fn)
        return s->fn(a, f, s->target);
    if (!s->target)
        missing(s);
    if (s->flags & W64F_MATH)
        return w64_math(s->target, (int)(s->flags >> 8 & 0xff), a, f);
    uint32_t args[MAX_ARGS];
    int count = MAX_ARGS;
    for (int i = 0; i < MAX_ARGS; i++)
        args[i] = narrow(a[i]);
    if (s->flags & W64F_SPLIT0) {
        args[0] = (uint32_t)a[0];
        args[1] = (uint32_t)(a[0] >> 32);
        for (int i = 1; i < MAX_ARGS - 1; i++)
            args[i + 1] = narrow(a[i]);
    }
    if (s->flags & W64F_VA) {
        int fmt = (int)(s->flags >> 8 & 0xff);
        args[fmt + 1] = U(&a[fmt + 1]);
        count = fmt + 2;
    }
    uint32_t outs = s->flags >> W64_OUT_SHIFT, slot[12];
    for (int i = 0; outs && i < 12; i++)
        if ((outs >> i & 1) && a[i]) {
            slot[i] = s->flags & W64F_INOUT ? rd32(P(a[i])) : OUT_UNSET;
            args[i] = U(&slot[i]);
        }
    int saved = crt_slots64;
    if (s->flags & (W64F_VA | W64F_FMT))
        crt_slots64 = 1;
    uint64_t r = w64_call32(s->target, args, count);
    crt_slots64 = saved;
    for (int i = 0; outs && i < 12; i++)
        if ((outs >> i & 1) && a[i] && slot[i] != (s->flags & W64F_INOUT ? rd32(P(a[i])) : OUT_UNSET))
            put64(P(a[i]), w64_high_back(slot[i]));
    if (s->flags & W64F_FILE)
        return wrap_file((uint32_t)r);
    return s->flags & W64F_RET64 ? r : w64_high_back(w64_ret((uint32_t)r));
}

static const struct w64_export *find_export(const char *module, const char *name)
{
    SYNTH_TABLE_FOREACH(synth_win64, struct w64_module, m) {
        if (strcmp(m->name, module) != 0)
            continue;
        for (size_t i = 0; i < m->count; i++)
            if (strcmp(m->exports[i].name, name) == 0)
                return &m->exports[i];
    }
    return strcmp(module, "*") ? find_export("*", name) : NULL;
}

static struct w64_stub *new_stub(const char *module, const char *name, const struct w64_export *e, void *target)
{
    struct w64_stub *s = heap_alloc_aligned(&w32->heap, sizeof(*s), 16);
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    uint32_t self = U(s), gate = U(w64_gate);
    s->code[0] = 0x41;
    s->code[1] = 0xbb;
    memcpy(s->code + 2, &self, 4);
    s->code[6] = 0xb8;
    memcpy(s->code + 7, &gate, 4);
    s->code[11] = 0xff;
    s->code[12] = 0xe0;
    s->fn = e ? e->fn : NULL;
    s->flags = e ? e->flags : 0;
    s->target = target;
    s->module = w32_strdup(module);
    s->name = w32_strdup(name);
    s->next = S->stubs;
    S->stubs = s;
    return s;
}

void *w64_lookup(const char *module, const char *symbol, uint32_t ordinal, bool create_missing)
{
    if (!w32 || !w32->w64)
        return NULL;
    char tmp[16];
    if (!symbol)
        symbol = w32_builtin_ordinal_name(module, ordinal);
    if (!symbol) {
        ksnprintf(tmp, sizeof(tmp), "#%u", ordinal);
        symbol = tmp;
    }
    for (struct w64_stub *s = S->stubs; s; s = s->next)
        if (strcmp(s->name, symbol) == 0 && strcmp(s->module, module) == 0)
            return s;
    const struct w64_export *e = find_export(module, symbol);
    void *target = w32_builtin_lookup(module, e && e->target ? e->target : symbol);
    if (e && (e->flags & W64F_NATIVE))
        return (void *)e->data;
    if (e && (e->flags & W64F_DATA))
        return ((void *(*)(void))e->data)();
    if (e && (e->flags & W64F_INTDATA))
        return target;
    if (!target && !(e && e->fn) && !create_missing)
        return NULL;
    return new_stub(module, symbol, e, target);
}

void *w64_target_stub(w64_handler fn, void *target)
{
    for (struct w64_stub *s = S->stubs; s; s = s->next)
        if (s->fn == fn && s->target == target)
            return s;
    struct w64_export e = {"callback", fn, 0, NULL, NULL};
    return new_stub("user32.dll", "callback", &e, target);
}

bool w64_stub_info(uint64_t addr, w64_handler fn, void **target)
{
    for (struct w64_stub *s = S->stubs; s; s = s->next) {
        if (U(s) != addr)
            continue;
        if (s->fn != fn)
            return false;
        *target = s->target;
        return true;
    }
    return false;
}

bool w64_is_code64(uint64_t addr)
{
    if (addr >> 32)
        return true;
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (img->pe64 && !img->builtin && addr >= img->base && addr < (uint64_t)img->base + img->size)
            return true;
    for (struct w64_stub *s = w32->w64 ? S->stubs : NULL; s; s = s->next)
        if (U(s) == addr)
            return true;
    return false;
}

void *w64_kernel_stub(w64_handler fn, const char *name)
{
    struct w64_stub *s = kmalloc(sizeof(*s) + 16);
    if (!s)
        return NULL;
    s = (struct w64_stub *)ALIGN_UP((uintptr_t)s, 16u);
    memset(s, 0, sizeof(*s));
    uint32_t self = U(s), gate = U(w64_gate);
    s->code[0] = 0x41;
    s->code[1] = 0xbb;
    memcpy(s->code + 2, &self, 4);
    s->code[6] = 0xb8;
    memcpy(s->code + 7, &gate, 4);
    s->code[11] = 0xff;
    s->code[12] = 0xe0;
    s->fn = fn;
    s->module = "kernel32.dll";
    s->name = name;
    return s;
}

void *w64_stub(const char *module, const char *symbol, uint32_t ordinal)
{
    return w64_lookup(module, symbol, ordinal, true);
}

struct call_request {
    uint64_t fn;
    int argc;
    const uint64_t *args;
    uint64_t result;
};

static uint32_t WINAPI call_on_program_stack(struct call_request *r)
{
    r->result = arch_w64_call((uint32_t)r->fn, r->argc, r->args);
    return (uint32_t)r->result;
}

uint64_t w64_call(uint64_t fn, int argc, const uint64_t *args)
{
    if (w32->jump.active)
        return arch_w64_call((uint32_t)fn, argc, args);
    struct call_request r = {fn, argc, args, 0};
    uint32_t arg = U(&r);
    w32_call((void *)call_on_program_stack, 1, &arg);
    return r.result;
}

uint32_t w64_run_main(uint32_t entry)
{
    return (uint32_t)w64_call(entry, 0, NULL);
}

static uint64_t ptr_array(void *const *v, int n)
{
    if (n < 0)
        for (n = 0; v && v[n]; n++)
            ;
    uint64_t *arr = w32_zalloc(sizeof(uint64_t) * (size_t)(n + 1));
    if (!arr)
        return 0;
    for (int i = 0; i < n; i++)
        arr[i] = U(v[i]);
    return U(arr);
}

static void refresh_environ(void)
{
    S->environ = S->initenv = ptr_array((void *const *)w32->environ, -1);
    S->wenviron = S->winitenv = ptr_array((void *const *)w32->wenviron, -1);
}

static void put_ustr(uint8_t *at, const WCHAR *s)
{
    uint16_t len = (uint16_t)(w32_wcslen(s) * 2);
    put16(at, len);
    put16(at + 2, (uint16_t)(len + 2));
    put64(at + 8, U(s));
}

/* Implicit TLS: every module with a TLS directory gets an index, and each thread a copy of its template in the
   ThreadLocalStoragePointer array (TEB+0x58), DLLs loaded later included, as since Windows Vista. */
static uint64_t *tls_slots(uint8_t *teb)
{
    uint64_t slots = rd64(teb + 0x58);
    if (slots)
        return P(slots);
    uint64_t *fresh = w32_zalloc(sizeof(uint64_t) * TLS_SLOTS);
    if (fresh)
        put64(teb + 0x58, U(fresh));
    return fresh;
}

static void tls_fill(uint8_t *teb, int index)
{
    const struct tls64_module *m = &S->tls[index];
    uint32_t size = (uint32_t)(m->end - m->start);
    uint64_t *slots = tls_slots(teb);
    uint8_t *block = w32_zalloc(size + m->zero + 16);
    if (!slots || !block)
        return;
    if (size)
        memcpy(block, P(m->start), size);
    slots[index] = U(block);
}

static void tls_block(uint8_t *teb)
{
    for (int i = 0; i < S->tls_count; i++)
        tls_fill(teb, i);
}

static int tls_add(struct w32_image *img)
{
    const uint8_t *mem = (const uint8_t *)img->base;
    uint32_t opt = rd32(mem + 0x3c) + 24;
    uint32_t rva = rd32(mem + opt + 112 + 9 * 8);
    if (!rva || S->tls_count >= TLS_SLOTS)
        return -1;
    const uint8_t *dir = mem + rva;
    int index = S->tls_count++;
    S->tls[index] = (struct tls64_module){rd64(dir), rd64(dir + 8), rd64(dir + 24), img->base, rd32(dir + 32)};
    if (rd64(dir + 16))
        put32(P(rd64(dir + 16)), (uint32_t)index);
    return index;
}

static void tls_run(const struct tls64_module *m, uint32_t reason)
{
    for (const uint64_t *cb = m->callbacks ? P(m->callbacks) : NULL; cb && *cb && !w32->exiting; cb++) {
        uint64_t args[3] = {m->base, reason, 0};
        w64_call(*cb, 3, args);
    }
}

void w64_tls_register(struct w32_image *img)
{
    if (!w32->w64)
        return;
    int index = tls_add(img);
    if (index < 0)
        return;
    struct w32_thread *t = w32->threads;
    do {
        if (t && t->teb64)
            tls_fill(t->teb64, index);
    } while (t && (t = t->next) != w32->threads);
}

void w64_tls_attach(uint32_t base)
{
    for (int i = 0; w32->w64 && i < S->tls_count; i++)
        if (S->tls[i].base == base)
            tls_run(&S->tls[i], 1);
}

uint8_t *w64_make_teb(uint32_t stack, uint32_t size, uint32_t tid)
{
    uint8_t *teb = heap_alloc_aligned(&w32->heap, TEB64_SIZE, 4096);
    if (!teb)
        return NULL;
    memset(teb, 0, TEB64_SIZE);
    put64(teb + 0x08, stack + size);
    put64(teb + 0x10, stack);
    put64(teb + 0x30, U(teb));
    put64(teb + 0x40, 0x100);
    put64(teb + 0x48, tid);
    put64(teb + 0x60, U(w32->peb64));
    tls_block(teb);
    return teb;
}

static bool build_peb64(void)
{
    uint8_t *peb = heap_alloc_aligned(&w32->heap, 4096, 4096);
    uint8_t *params = w32_zalloc(0x400);
    uint8_t *ldr = w32_zalloc(0x60);
    if (!peb || !params || !ldr)
        return false;
    memset(peb, 0, 4096);
    put32(ldr, 0x58);
    ldr[4] = 1;
    for (uint32_t off = 0x10; off <= 0x30; off += 0x10) {
        put64(ldr + off, U(ldr + off));
        put64(ldr + off + 8, U(ldr + off));
    }
    put64(peb + 0x18, U(ldr));
    put64(peb + 0x20, U(params));
    put64(peb + 0x30, U(&w32->heap));
    put32(peb + 0xb8, 1);
    put32(peb + 0x118, 10);
    put16(peb + 0x120, 19045);
    put32(peb + 0x124, 2);
    put32(params, 0x400);
    put32(params + 4, 0x400);
    put32(params + 8, 1);
    for (int i = 0; i < 3; i++)
        put64(params + 0x20 + i * 8, U(w32_std_handle(i)));
    put_ustr(params + 0x60, w32->wargv[0]);
    put_ustr(params + 0x70, w32->cmdline_w);
    put64(params + 0x80, U(w32->env_block_w));
    w32->peb64 = peb;
    return true;
}

bool w64_setup(void)
{
    w64_dispatch_fn = (void *)dispatch;
    vmm_set_high_hook(w64_high_fault);
    w64_seh_setup();
    crt_slots64 = 0;
    struct w64_state *st = w32_zalloc(sizeof(*st));
    if (!st)
        return false;
    w32->w64 = st;
    for (int i = 0; i < 3; i++) {
        put32(st->iob[i] + 0x18, i == 0 ? 1 : 2);
        put32(st->iob[i] + 0x1c, (uint32_t)i);
    }
    st->argv = ptr_array((void *const *)w32->argv, w32->argc);
    st->wargv = ptr_array((void *const *)w32->wargv, w32->argc);
    refresh_environ();
    st->acmdln = U(w32->cmdline);
    st->wcmdln = U(w32->cmdline_w);
    st->pgmptr = U(w32->argv[0]);
    st->wpgmptr = U(w32->wargv[0]);
    static const char dot[] = ".", empty[] = "";
    st->lconv.text[0] = U(dot);
    for (int i = 1; i < 10; i++)
        st->lconv.text[i] = U(empty);
    memset(st->lconv.values, 127, sizeof(st->lconv.values));
    if (!build_peb64())
        return false;
    w32->teb64 = w64_make_teb(U(w32->stack), w32->stack_size, w32_thread_id());
    if (!w32->teb64)
        return false;
    w32->threads->teb64 = w32->teb64;
    arch_w64_set_teb(U(w32->teb64));
    return true;
}

void w64_set_image(uint32_t base)
{
    S->image_base = base;
    put64(w32->peb64 + 0x10, base);
}

void w64_setup_tls(struct w32_image *img)
{
    w64_tls_register(img);
    w64_tls_attach(img->base);
}

/* thread attach (2) and detach (3) go to every module */
void w64_tls_callbacks(uint32_t reason)
{
    for (int i = 0; w32->w64 && i < S->tls_count && !w32->exiting; i++)
        tls_run(&S->tls[i], reason);
}

void w64_cleanup(void)
{
    crt_slots64 = 0;
    w64_mem_cleanup();
}

static uint64_t h_zero(uint64_t *a, const uint64_t *f, void *t)
{
    return 0;
}

static uint64_t h_one(uint64_t *a, const uint64_t *f, void *t)
{
    return 1;
}

static uint64_t h_get_system_info(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t *o = P(a[0]);
    memset(o, 0, 48);
    put16(o, 9);
    put32(o + 4, 4096);
    put64(o + 8, 0x10000);
    put64(o + 16, VMM_USER_END - 0x10001u);
    put64(o + 24, 1);
    put32(o + 32, 1);
    put32(o + 36, 8664);
    put32(o + 40, 0x10000);
    put16(o + 44, 6);
    return 0;
}

static void startup_to64(const uint8_t *s, uint8_t *o)
{
    memset(o, 0, 104);
    put32(o, 104);
    for (int i = 0; i < 3; i++)
        put64(o + 8 + i * 8, rd32(s + 4 + i * 4));
    memcpy(o + 32, s + 16, 32);
    memcpy(o + 64, s + 48, 4);
    put64(o + 72, rd32(s + 52));
    for (int i = 0; i < 3; i++)
        put64(o + 80 + i * 8, w64_sx(rd32(s + 56 + i * 4)));
}

static void startup_to32(const uint8_t *o, uint8_t *s)
{
    memset(s, 0, 68);
    put32(s, 68);
    for (int i = 0; i < 3; i++)
        put32(s + 4 + i * 4, (uint32_t)rd64(o + 8 + i * 8));
    memcpy(s + 16, o + 32, 32);
    memcpy(s + 48, o + 64, 4);
    put32(s + 52, (uint32_t)rd64(o + 72));
    for (int i = 0; i < 3; i++)
        put32(s + 56 + i * 4, (uint32_t)rd64(o + 80 + i * 8));
}

static uint64_t h_get_startup_info(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t s[68];
    T32(t, U(s));
    startup_to64(s, P(a[0]));
    return 0;
}

static uint64_t h_create_process(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t si[68], pi[16] = {0};
    if (a[8])
        startup_to32(P(a[8]), si);
    uint32_t r = (uint32_t)T32(t, narrow(a[0]), narrow(a[1]), narrow(a[2]), narrow(a[3]), narrow(a[4]),
                               narrow(a[5]), narrow(a[6]), narrow(a[7]), a[8] ? U(si) : 0, U(pi));
    if (r && a[9]) {
        uint8_t *o = P(a[9]);
        put64(o, rd32(pi));
        put64(o + 8, rd32(pi + 4));
        put32(o + 16, rd32(pi + 8));
        put32(o + 20, rd32(pi + 12));
    }
    return r;
}

static uint64_t h_get_proc_address(uint64_t *a, const uint64_t *f, void *t)
{
    const char *name = a[1] >> 16 ? P(a[1]) : NULL;
    uint32_t ordinal = a[1] >> 16 ? 0 : (uint32_t)a[1];
    for (struct w32_image *img = w32->images; img; img = img->next) {
        if (img->base != (uint32_t)a[0])
            continue;
        void *fn = img->builtin ? w64_lookup(img->name, name, ordinal, false) : w32_get_proc(img, name, ordinal);
        if (!fn)
            w32_set_error(ERROR_PROC_NOT_FOUND);
        return U(fn);
    }
    w32_set_error(ERROR_MOD_NOT_FOUND);
    return 0;
}

static uint64_t h_module_handle_ex(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t h = 0;
    uint32_t r = (uint32_t)T32(t, narrow(a[0]), narrow(a[1]), U(&h));
    out64(a[2], h);
    return r;
}

static DWORD WINAPI thread_main(void *p)
{
    struct thread_start *s = p;
    w64_tls_callbacks(2);
    uint64_t arg = s->param;
    uint64_t r = arch_w64_call((uint32_t)s->fn, 1, &arg);
    return s->void_result ? 0 : (DWORD)r;
}

static uint64_t start_thread(uint64_t stack, uint64_t fn, uint64_t param, uint64_t flags, uint64_t id_out,
                             bool void_result)
{
    struct thread_start *s = w32_alloc(sizeof(*s));
    if (!s)
        return 0;
    s->fn = fn;
    s->param = param;
    s->void_result = void_result;
    DWORD id = 0;
    HANDLE h = w32_create_thread((SIZE_T)stack, (void *)thread_main, s, (DWORD)flags, &id);
    if (id_out)
        put32(P(id_out), id);
    return U(h);
}

static uint64_t h_create_thread(uint64_t *a, const uint64_t *f, void *t)
{
    return start_thread(a[1], a[2], a[3], a[4], a[5], false);
}

static uint64_t h_beginthreadex(uint64_t *a, const uint64_t *f, void *t)
{
    return start_thread(a[1], a[2], a[3], a[4], a[5], false);
}

static uint64_t h_beginthread(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t h = start_thread(a[1], a[0], a[2], 0, 0, true);
    return h ? h : (uint64_t)-1;
}

static uint64_t h_set_filter(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t old = S->filter;
    S->filter = a[0];
    return old;
}

static uint64_t h_wait_multiple(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t n = (uint32_t)MIN(a[0], 64u), handles[64];
    const uint64_t *src = P(a[1]);
    for (uint32_t i = 0; i < n; i++)
        handles[i] = (uint32_t)src[i];
    return w64_sx((uint32_t)T32(t, n, U(handles), narrow(a[2]), narrow(a[3]), narrow(a[4])));
}

static uint64_t h_duplicate_handle(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t h = 0;
    uint32_t r = (uint32_t)T32(t, narrow(a[0]), narrow(a[1]), narrow(a[2]), a[3] ? U(&h) : 0,
                               narrow(a[4]), narrow(a[5]), narrow(a[6]));
    out64(a[3], h);
    return r;
}

static uint64_t h_format_message(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t flags = (uint32_t)a[0] & ~0x2000u, buf = 0;
    bool alloc = flags & 0x100;
    uint32_t r = (uint32_t)T32(t, flags | 0x200, narrow(a[1]), narrow(a[2]), narrow(a[3]),
                               alloc ? U(&buf) : narrow(a[4]), narrow(a[5]), 0);
    if (alloc)
        out64(a[4], buf);
    return r;
}

static uint64_t h_full_path(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t part = 0;
    uint32_t r = (uint32_t)T32(t, narrow(a[0]), narrow(a[1]), narrow(a[2]), a[3] ? U(&part) : 0);
    out64(a[3], w64_high_back(part));
    return r;
}

static uint64_t h_init_once(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t *once = P(a[0]);
    if (*once == 0) {
        *once = 1;
        uint64_t args[3] = {a[0], a[2], a[3]};
        uint32_t ok = (uint32_t)w64_call(a[1], 3, args);
        *once = ok ? 2 : 0;
        return ok != 0;
    }
    return 1;
}

static uint64_t h_fls_alloc(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_sx((uint32_t)T32(t, 0));
}

static uint64_t h_set_file_pointer_ex(uint64_t *a, const uint64_t *f, void *t)
{
    return (uint32_t)T32(t, narrow(a[0]), (uint32_t)a[1], (uint32_t)(a[1] >> 32), narrow(a[2]), narrow(a[3]));
}

static uint64_t h_c_specific_handler(uint64_t *a, const uint64_t *f, void *t)
{
    return 1;
}

static uint64_t h_getmainargs(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t argc, argv, env;
    T32(t, U(&argc), U(&argv), U(&env), narrow(a[3]), narrow(a[4]));
    put32(P(a[0]), (uint32_t)w32->argc);
    out64(a[1], S->argv);
    out64(a[2], S->environ);
    return 0;
}

static uint64_t h_wgetmainargs(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t argc, argv, env;
    T32(t, U(&argc), U(&argv), U(&env), narrow(a[3]), narrow(a[4]));
    put32(P(a[0]), (uint32_t)w32->argc);
    out64(a[1], S->wargv);
    out64(a[2], S->wenviron);
    return 0;
}

static void *d_argv(void) { return &S->argv; }
static void *d_wargv(void) { return &S->wargv; }
static void *d_environ(void) { return &S->environ; }
static void *d_wenviron(void) { return &S->wenviron; }
static void *d_initenv(void) { return &S->initenv; }
static void *d_winitenv(void) { return &S->winitenv; }
static void *d_acmdln(void) { return &S->acmdln; }
static void *d_wcmdln(void) { return &S->wcmdln; }
static void *d_pgmptr(void) { return &S->pgmptr; }
static void *d_wpgmptr(void) { return &S->wpgmptr; }
static void *d_iob(void) { return S->iob; }

static void *d_pctype(void)
{
    const uint32_t *p = w32_builtin_lookup("msvcrt.dll", "_pctype");
    S->pctype = p ? *p : 0;
    return &S->pctype;
}

static void *d_pwctype(void)
{
    const uint32_t *p = w32_builtin_lookup("msvcrt.dll", "_pwctype");
    S->pwctype = p ? *p : 0;
    return &S->pwctype;
}

static void *d_tzname(void)
{
    const uint32_t *p = w32_builtin_lookup("msvcrt.dll", "_tzname");
    for (int i = 0; i < 2; i++)
        S->tzname[i] = p ? p[i] : 0;
    return S->tzname;
}

static uint64_t h_p_argv(uint64_t *a, const uint64_t *f, void *t) { return U(d_argv()); }
static uint64_t h_p_wargv(uint64_t *a, const uint64_t *f, void *t) { return U(d_wargv()); }
static uint64_t h_p_environ(uint64_t *a, const uint64_t *f, void *t) { return U(d_environ()); }
static uint64_t h_p_wenviron(uint64_t *a, const uint64_t *f, void *t) { return U(d_wenviron()); }
static uint64_t h_p_initenv(uint64_t *a, const uint64_t *f, void *t) { return U(d_initenv()); }
static uint64_t h_p_winitenv(uint64_t *a, const uint64_t *f, void *t) { return U(d_winitenv()); }
static uint64_t h_p_acmdln(uint64_t *a, const uint64_t *f, void *t) { return U(d_acmdln()); }
static uint64_t h_p_wcmdln(uint64_t *a, const uint64_t *f, void *t) { return U(d_wcmdln()); }
static uint64_t h_p_pgmptr(uint64_t *a, const uint64_t *f, void *t) { return U(d_pgmptr()); }
static uint64_t h_p_wpgmptr(uint64_t *a, const uint64_t *f, void *t) { return U(d_wpgmptr()); }
static uint64_t h_p_pctype(uint64_t *a, const uint64_t *f, void *t) { return U(d_pctype()); }
static uint64_t h_p_pwctype(uint64_t *a, const uint64_t *f, void *t) { return U(d_pwctype()); }
static uint64_t h_p_tzname(uint64_t *a, const uint64_t *f, void *t) { return U(d_tzname()); }
static uint64_t h_iob_func(uint64_t *a, const uint64_t *f, void *t) { return U(S->iob); }

static uint64_t h_acrt_iob_func(uint64_t *a, const uint64_t *f, void *t)
{
    return U(S->iob[MIN((uint32_t)a[0], 2u)]);
}

static uint64_t h_get_pgmptr(uint64_t *a, const uint64_t *f, void *t)
{
    out64(a[0], S->pgmptr);
    return 0;
}

static uint64_t h_get_wpgmptr(uint64_t *a, const uint64_t *f, void *t)
{
    out64(a[0], S->wpgmptr);
    return 0;
}

static uint64_t h_putenv(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t r = w64_sx((uint32_t)T32(t, narrow(a[0]), narrow(a[1])));
    refresh_environ();
    return r;
}

static uint64_t h_fclose(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t r = w64_sx((uint32_t)T32(t, narrow(a[0])));
    release_file(a[0]);
    return r;
}

static uint64_t h_fcloseall(uint64_t *a, const uint64_t *f, void *t)
{
    for (int i = 0; i < FILES; i++)
        S->files[i].used = 0;
    return w64_sx((uint32_t)w64_call32(t, NULL, 0));
}

static uint64_t h_fopen_s(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t file = 0;
    uint32_t r = (uint32_t)T32(t, U(&file), narrow(a[1]), narrow(a[2]));
    out64(a[0], wrap_file(file));
    return r;
}

static uint64_t h_initterm(uint64_t *a, const uint64_t *f, void *t)
{
    for (uint64_t p = a[0]; p < a[1] && !w32->exiting; p += 8) {
        uint64_t fn = rd64(P(p));
        if (fn)
            w64_call(fn, 0, NULL);
    }
    return 0;
}

static uint64_t h_initterm_e(uint64_t *a, const uint64_t *f, void *t)
{
    for (uint64_t p = a[0]; p < a[1] && !w32->exiting; p += 8) {
        uint64_t fn = rd64(P(p));
        if (fn) {
            uint32_t r = (uint32_t)w64_call(fn, 0, NULL);
            if (r)
                return r;
        }
    }
    return 0;
}

static bool add_onexit(uint64_t fn)
{
    if (!fn || S->onexit_count >= ONEXIT_MAX)
        return false;
    S->onexit[S->onexit_count++] = fn;
    return true;
}

static void run_onexit(void)
{
    while (S->onexit_count > 0 && !w32->exiting) {
        uint64_t fn = S->onexit[--S->onexit_count];
        w64_call(fn, 0, NULL);
    }
}

static uint64_t h_onexit(uint64_t *a, const uint64_t *f, void *t)
{
    return add_onexit(a[0]) ? a[0] : 0;
}

static uint64_t h_atexit(uint64_t *a, const uint64_t *f, void *t)
{
    return add_onexit(a[0]) ? 0 : (uint64_t)-1;
}

static uint64_t h_exit(uint64_t *a, const uint64_t *f, void *t)
{
    run_onexit();
    T32(t, narrow(a[0]));
    return 0;
}

static uint64_t h_cexit(uint64_t *a, const uint64_t *f, void *t)
{
    run_onexit();
    w64_call32(t, NULL, 0);
    return 0;
}

static uint64_t h_init_onexit_table(uint64_t *a, const uint64_t *f, void *t)
{
    if (!a[0])
        return (uint64_t)-1;
    memset(P(a[0]), 0, 24);
    return 0;
}

static uint64_t h_register_onexit(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t *table = P(a[0]);
    if (!table)
        return (uint64_t)-1;
    if (table[1] == table[2]) {
        size_t used = (size_t)(table[1] - table[0]) / 8, cap = used ? used * 2 : 16;
        uint64_t *grown = w32_realloc(P(table[0]), cap * 8);
        if (!grown)
            return (uint64_t)-1;
        table[0] = U(grown);
        table[1] = U(grown + used);
        table[2] = U(grown + cap);
    }
    put64(P(table[1]), a[1]);
    table[1] += 8;
    return 0;
}

static uint64_t h_execute_onexit(uint64_t *a, const uint64_t *f, void *t)
{
    uint64_t *table = P(a[0]);
    if (!table)
        return (uint64_t)-1;
    while (table[1] > table[0] && !w32->exiting) {
        table[1] -= 8;
        uint64_t fn = rd64(P(table[1]));
        if (fn)
            w64_call(fn, 0, NULL);
    }
    w32_free(P(table[0]));
    memset(table, 0, 24);
    return 0;
}

static uint64_t h_initial_environment(uint64_t *a, const uint64_t *f, void *t)
{
    return S->environ;
}

static uint64_t h_initial_wide_environment(uint64_t *a, const uint64_t *f, void *t)
{
    return S->wenviron;
}

static uint64_t h_signal(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t sig = (uint32_t)a[0];
    if (sig >= SIGNALS)
        return (uint64_t)-1;
    uint64_t old = S->signals[sig];
    S->signals[sig] = a[1];
    return old;
}

static uint64_t h_raise(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t sig = (uint32_t)a[0];
    if (sig < SIGNALS && S->signals[sig] > 1) {
        uint64_t arg = sig;
        w64_call(S->signals[sig], 1, &arg);
        return 0;
    }
    return w64_sx((uint32_t)T32(t, sig));
}

static uint64_t h_localeconv(uint64_t *a, const uint64_t *f, void *t)
{
    return U(&S->lconv);
}

static uint64_t h_strto(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t end = 0;
    uint64_t r = T32(t, narrow(a[0]), a[1] ? U(&end) : 0, narrow(a[2]));
    out64(a[1], w64_high_back(end));
    return w64_sx((uint32_t)r);
}

static uint64_t h_strto64(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t end = 0;
    uint64_t r = T32(t, narrow(a[0]), a[1] ? U(&end) : 0, narrow(a[2]));
    out64(a[1], w64_high_back(end));
    return r;
}

static uint64_t h_strtod(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_strtod(a, f, t, false, false);
}

static uint64_t h_strtof(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_strtod(a, f, t, false, true);
}

static uint64_t h_wcstod(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_strtod(a, f, t, true, false);
}

static uint64_t h_atof(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_atof(t, a[0]);
}

static uint64_t h_i64to(uint64_t *a, const uint64_t *f, void *t)
{
    return T32(t, (uint32_t)a[0], (uint32_t)(a[0] >> 32), narrow(a[1]), narrow(a[2]));
}

static uint64_t h_lseek64(uint64_t *a, const uint64_t *f, void *t)
{
    return T32(t, narrow(a[0]), (uint32_t)a[1], (uint32_t)(a[1] >> 32), narrow(a[2]));
}

static uint64_t h_fseek64(uint64_t *a, const uint64_t *f, void *t)
{
    return w64_sx((uint32_t)T32(t, narrow(a[0]), (uint32_t)a[1], (uint32_t)(a[1] >> 32), narrow(a[2])));
}

static uint64_t h_stat64i32(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t st[56];
    uint32_t r = (uint32_t)T32(t, narrow(a[0]), U(st));
    if (r == 0) {
        uint8_t *o = P(a[1]);
        memset(o, 0, 48);
        memcpy(o, st, 20);
        memcpy(o + 20, st + 24, 4);
        memcpy(o + 24, st + 32, 24);
    }
    return w64_sx(r);
}

static uint64_t h_div(uint64_t *a, const uint64_t *f, void *t)
{
    int32_t x = (int32_t)a[0], y = (int32_t)a[1];
    return (uint32_t)(x / y) | (uint64_t)(uint32_t)(x % y) << 32;
}

static uint64_t h_lldiv(uint64_t *a, const uint64_t *f, void *t)
{
    int64_t x = (int64_t)a[1], y = (int64_t)a[2];
    put64(P(a[0]), (uint64_t)(x / y));
    put64(P(a[0]) + 8, (uint64_t)(x % y));
    return a[0];
}

static void swap_bytes(uint8_t *x, uint8_t *y, size_t n)
{
    while (n--) {
        uint8_t c = *x;
        *x++ = *y;
        *y++ = c;
    }
}

static int compare(uint64_t fn, uint64_t ctx, bool with_ctx, const void *x, const void *y)
{
    uint64_t args[3] = {ctx, U(x), U(y)};
    return (int)(uint32_t)w64_call(fn, with_ctx ? 3 : 2, with_ctx ? args : args + 1);
}

static void sift(uint8_t *base, size_t size, size_t root, size_t n, uint64_t fn, uint64_t ctx, bool with_ctx)
{
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= n)
            return;
        if (child + 1 < n && compare(fn, ctx, with_ctx, base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compare(fn, ctx, with_ctx, base + root * size, base + child * size) >= 0)
            return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

static void sort64(uint8_t *base, size_t n, size_t size, uint64_t fn, uint64_t ctx, bool with_ctx)
{
    if (n < 2 || !size)
        return;
    for (size_t i = n / 2; i-- > 0;)
        sift(base, size, i, n, fn, ctx, with_ctx);
    for (size_t end = n - 1; end > 0 && !w32->exiting; end--) {
        swap_bytes(base, base + end * size, size);
        sift(base, size, 0, end, fn, ctx, with_ctx);
    }
}

static uint64_t h_qsort(uint64_t *a, const uint64_t *f, void *t)
{
    sort64(P(a[0]), (size_t)a[1], (size_t)a[2], a[3], 0, false);
    return 0;
}

static uint64_t h_qsort_s(uint64_t *a, const uint64_t *f, void *t)
{
    sort64(P(a[0]), (size_t)a[1], (size_t)a[2], a[3], a[4], true);
    return 0;
}

static uint64_t h_bsearch(uint64_t *a, const uint64_t *f, void *t)
{
    const uint8_t *base = P(a[1]);
    size_t lo = 0, hi = (size_t)a[2], size = (size_t)a[3];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = compare(a[4], 0, false, P(a[0]), base + mid * size);
        if (c == 0)
            return U(base + mid * size);
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

static const struct w64_export kernel32_exports[] = {
    W64_FN("GetSystemInfo", h_get_system_info),
    W64_FN("GetNativeSystemInfo", h_get_system_info),
    W64_FN("GetStartupInfoA", h_get_startup_info),
    W64_FN("GetStartupInfoW", h_get_startup_info),
    W64_FN("CreateProcessA", h_create_process),
    W64_FN("CreateProcessW", h_create_process),
    W64_FN("GetProcAddress", h_get_proc_address),
    W64_FN("GetModuleHandleExA", h_module_handle_ex),
    W64_FN("GetModuleHandleExW", h_module_handle_ex),
    W64_FN("CreateThread", h_create_thread),
    W64_FN("SetUnhandledExceptionFilter", h_set_filter),
    W64_FN("AddVectoredExceptionHandler", h_one),
    W64_FN("RemoveVectoredExceptionHandler", h_one),
    W64_FN("AddVectoredContinueHandler", h_one),
    W64_FN("RemoveVectoredContinueHandler", h_one),
    W64_FN("RtlAddFunctionTable", h_one),
    W64_FN("RtlDeleteFunctionTable", h_one),
    W64_FN("RtlInstallFunctionTableCallback", h_one),
    W64_FN("SetConsoleCtrlHandler", h_one),
    W64_FN("WaitForMultipleObjects", h_wait_multiple),
    W64_FN("WaitForMultipleObjectsEx", h_wait_multiple),
    W64_FN("DuplicateHandle", h_duplicate_handle),
    W64_FN("FormatMessageA", h_format_message),
    W64_FN("FormatMessageW", h_format_message),
    W64_FN("GetFullPathNameA", h_full_path),
    W64_FN("GetFullPathNameW", h_full_path),
    W64_FN("InitOnceExecuteOnce", h_init_once),
    W64_FN("FlsAlloc", h_fls_alloc),
    W64_FN("SetFilePointerEx", h_set_file_pointer_ex),
    W64_FLAG("GetTickCount64", W64F_RET64),
};
W64_EXPORTS(kernel32_module, "kernel32.dll", kernel32_exports);

/* one block, as LocalFree expects: 8-byte pointers, then the strings */
static uint64_t h_argv_w(uint64_t *a, const uint64_t *f, void *t)
{
    int n = 0;
    uint32_t *argv = (uint32_t *)(uintptr_t)T32(t, U(P(a[0])), U(&n));
    if (!argv)
        return 0;
    size_t chars = 0;
    for (int i = 0; i < n; i++)
        chars += w32_wcslen((WCHAR *)(uintptr_t)argv[i]) + 1;
    uint64_t *out = w32_alloc(sizeof(uint64_t) * (size_t)(n + 1) + chars * sizeof(WCHAR));
    if (out) {
        WCHAR *text = (WCHAR *)(out + n + 1);
        for (int i = 0; i < n; i++) {
            size_t len = w32_wcslen((WCHAR *)(uintptr_t)argv[i]) + 1;
            memcpy(text, (WCHAR *)(uintptr_t)argv[i], len * sizeof(WCHAR));
            out[i] = U(text);
            text += len;
        }
        out[n] = 0;
        if (a[1])
            put32(P(a[1]), (uint32_t)n);
    }
    w32_free(argv);
    return U(out);
}

/* UNICODE_STRING: lengths, then the buffer pointer at offset 8 */
static uint64_t h_init_unicode(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t *s = P(a[0]);
    uint16_t len = (uint16_t)MIN((a[1] ? w32_wcslen(P(a[1])) : 0) * 2, 0xfffcu);
    memset(s, 0, 16);
    put16(s, len);
    put16(s + 2, a[1] ? (uint16_t)(len + 2) : 0);
    put64(s + 8, a[1]);
    return 0;
}

static uint64_t h_free_unicode(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t *s = a[0] ? P(a[0]) : NULL;
    if (s && rd64(s + 8))
        w32_free(P(rd64(s + 8)));
    if (s)
        memset(s, 0, 16);
    return 0;
}

static uint64_t h_user_key_path(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t m[8];
    uint32_t status = (uint32_t)T32(t, U(m));
    uint8_t *o = P(a[0]);
    memset(o, 0, 16);
    if (!status) {
        memcpy(o, m, 4);
        put64(o + 8, rd32(m + 4));
    }
    return status;
}

static uint64_t h_out_handle(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t h = 0;
    uint32_t r = (uint32_t)T32(t, a[0] ? U(&h) : 0, narrow(a[1]), narrow(a[2]), narrow(a[3]), narrow(a[4]),
                               narrow(a[5]), narrow(a[6]));
    out64(a[0], h);
    return r;
}

static const struct w64_export shell32_exports[] = {
    W64_FN("CommandLineToArgvW", h_argv_w),
};
W64_EXPORTS(shell32_module, "shell32.dll", shell32_exports);

static const struct w64_export ntdll_exports[] = {
    W64_FN("RtlInitUnicodeString", h_init_unicode),
    W64_FN("RtlFreeUnicodeString", h_free_unicode),
    W64_FN("RtlFormatCurrentUserKeyPath", h_user_key_path),
    W64_FN("NtOpenKey", h_out_handle),
    W64_FN("NtOpenKeyEx", h_out_handle),
    W64_FN("NtCreateKey", h_out_handle),
    W64_FN("RtlAddFunctionTable", h_one),
    W64_FN("RtlDeleteFunctionTable", h_one),
    W64_NATIVE("memcpy", w64_memcpy),
    W64_NATIVE("memmove", w64_memmove),
    W64_NATIVE("memset", w64_memset),
    W64_NATIVE("strlen", w64_strlen),
};
W64_EXPORTS(ntdll_module, "ntdll.dll", ntdll_exports);

static const struct w64_export msvcrt_exports[] = {
    W64_FN("__C_specific_handler", h_c_specific_handler),
    W64_FN("__getmainargs", h_getmainargs),
    W64_FN("__wgetmainargs", h_wgetmainargs),
    W64_FN("__p___argv", h_p_argv),
    W64_FN("__p___wargv", h_p_wargv),
    W64_FN("__p__environ", h_p_environ),
    W64_FN("__p__wenviron", h_p_wenviron),
    W64_FN("__p___initenv", h_p_initenv),
    W64_FN("__p___winitenv", h_p_winitenv),
    W64_FN("__p__acmdln", h_p_acmdln),
    W64_FN("__p__wcmdln", h_p_wcmdln),
    W64_FN("__p__pgmptr", h_p_pgmptr),
    W64_FN("__p__wpgmptr", h_p_wpgmptr),
    W64_FN("__p__pctype", h_p_pctype),
    W64_FN("__p__pwctype", h_p_pwctype),
    W64_FN("__p__tzname", h_p_tzname),
    W64_FN("_get_pgmptr", h_get_pgmptr),
    W64_FN("_get_wpgmptr", h_get_wpgmptr),
    W64_FN("__iob_func", h_iob_func),
    W64_FN("__acrt_iob_func", h_acrt_iob_func),
    W64_FLAG("fopen", W64F_FILE),
    W64_FLAG("_wfopen", W64F_FILE),
    W64_FLAG("_fsopen", W64F_FILE),
    W64_FLAG("_wfsopen", W64F_FILE),
    W64_FLAG("_fdopen", W64F_FILE),
    W64_FLAG("_wfdopen", W64F_FILE),
    W64_FLAG("freopen", W64F_FILE),
    W64_FLAG("_wfreopen", W64F_FILE),
    W64_FLAG("tmpfile", W64F_FILE),
    W64_FLAG("_popen", W64F_FILE),
    W64_FLAG("_wpopen", W64F_FILE),
    W64_FN("fopen_s", h_fopen_s),
    W64_FN("_wfopen_s", h_fopen_s),
    W64_FN("fclose", h_fclose),
    W64_FN("_pclose", h_fclose),
    W64_FN("_fcloseall", h_fcloseall),
    W64_FN("_putenv", h_putenv),
    W64_FN("_wputenv", h_putenv),
    W64_FN("_putenv_s", h_putenv),
    W64_FN("_wputenv_s", h_putenv),
    W64_DATA("__argv", d_argv),
    W64_DATA("__wargv", d_wargv),
    W64_DATA("_environ", d_environ),
    W64_DATA("_wenviron", d_wenviron),
    W64_DATA("__initenv", d_initenv),
    W64_DATA("__winitenv", d_winitenv),
    W64_DATA("_acmdln", d_acmdln),
    W64_DATA("_wcmdln", d_wcmdln),
    W64_DATA("_pgmptr", d_pgmptr),
    W64_DATA("_wpgmptr", d_wpgmptr),
    W64_DATA("_pctype", d_pctype),
    W64_DATA("_pwctype", d_pwctype),
    W64_DATA("_tzname", d_tzname),
    W64_DATA("_iob", d_iob),
    W64_INTDATA("__argc"),
    W64_INTDATA("_fmode"),
    W64_INTDATA("_commode"),
    W64_INTDATA("__mb_cur_max"),
    W64_INTDATA("_osver"),
    W64_INTDATA("_winver"),
    W64_INTDATA("_winmajor"),
    W64_INTDATA("_winminor"),
    W64_INTDATA("_timezone"),
    W64_INTDATA("_daylight"),
    W64_INTDATA("_HUGE"),
    W64_FN("_initterm", h_initterm),
    W64_FN("_initterm_e", h_initterm_e),
    W64_FN("_onexit", h_onexit),
    W64_FN("__dllonexit", h_onexit),
    W64_FN("atexit", h_atexit),
    W64_FN("_crt_atexit", h_atexit),
    W64_FN("exit", h_exit),
    W64_FN("_cexit", h_cexit),
    W64_FN("_initialize_onexit_table", h_init_onexit_table),
    W64_FN("_register_onexit_function", h_register_onexit),
    W64_FN("_execute_onexit_table", h_execute_onexit),
    W64_FN("_get_initial_narrow_environment", h_initial_environment),
    W64_FN("_get_initial_wide_environment", h_initial_wide_environment),
    W64_FN("__tzname", h_p_tzname),
    W64_FLAG("__stdio_common_vfprintf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vfprintf_s", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vfwprintf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vfwprintf_s", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vsprintf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vswprintf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vsprintf_s", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vswprintf_s", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vsnprintf_s", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vsscanf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vswscanf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vfscanf", W64F_FMT | W64F_SPLIT0),
    W64_FLAG("__stdio_common_vfwscanf", W64F_FMT | W64F_SPLIT0),
    W64_FN("signal", h_signal),
    W64_FN("raise", h_raise),
    W64_FN("__setusermatherr", h_zero),
    W64_FN("_set_invalid_parameter_handler", h_zero),
    W64_FN("_set_purecall_handler", h_zero),
    W64_FN("localeconv", h_localeconv),
    W64_FN("_beginthreadex", h_beginthreadex),
    W64_FN("_beginthread", h_beginthread),
    W64_FN("strtol", h_strto),
    W64_FN("strtoul", h_strto),
    W64_FN("wcstol", h_strto),
    W64_FN("wcstoul", h_strto),
    W64_FN("strtoll", h_strto64),
    W64_FN("strtoull", h_strto64),
    W64_FN("_strtoi64", h_strto64),
    W64_FN("_strtoui64", h_strto64),
    W64_FN("strtoimax", h_strto64),
    W64_FN("strtoumax", h_strto64),
    W64_FN("wcstoll", h_strto64),
    W64_FN("wcstoull", h_strto64),
    W64_FN("_wcstoi64", h_strto64),
    W64_FN("_wcstoui64", h_strto64),
    W64_FN("strtod", h_strtod),
    W64_FN("strtof", h_strtof),
    W64_FN("wcstod", h_wcstod),
    W64_FN("atof", h_atof),
    W64_FN("_i64toa", h_i64to),
    W64_FN("_ui64toa", h_i64to),
    W64_FN("_i64tow", h_i64to),
    W64_FN("_ui64tow", h_i64to),
    W64_FN("_lseeki64", h_lseek64),
    W64_FN("_fseeki64", h_fseek64),
    W64_FLAG("_telli64", W64F_RET64),
    W64_FLAG("_ftelli64", W64F_RET64),
    W64_FLAG("_atoi64", W64F_RET64),
    W64_FLAG("_wtoi64", W64F_RET64),
    W64_FLAG("atoll", W64F_RET64),
    W64_FLAG("_abs64", W64F_RET64),
    W64_FLAG("llabs", W64F_RET64),
    W64_FLAG("_time64", W64F_RET64),
    W64_FLAG("_mktime64", W64F_RET64),
    W64_TO("time", "_time64", W64F_RET64),
    W64_TO("mktime", "_mktime64", W64F_RET64),
    W64_TO("localtime", "_localtime64", 0),
    W64_TO("gmtime", "_gmtime64", 0),
    W64_TO("ctime", "_ctime64", 0),
    W64_TO("localtime_s", "_localtime64_s", 0),
    W64_TO("gmtime_s", "_gmtime64_s", 0),
    W64_TO("_ftime", "_ftime64", 0),
    W64_TO("_utime", "_utime64", 0),
    W64_TO("_findfirst", "_findfirst64i32", 0),
    W64_TO("_findnext", "_findnext64i32", 0),
    W64_TO("_findfirsti64", "_findfirst64", 0),
    W64_TO("_findnexti64", "_findnext64", 0),
    W64_TO("_stati64", "_stat64", 0),
    W64_TO("_wstati64", "_wstat64", 0),
    W64_TO("_fstati64", "_fstat64", 0),
    {"_stat", h_stat64i32, 0, NULL, "_stat64"},
    {"_wstat", h_stat64i32, 0, NULL, "_wstat64"},
    {"_fstat", h_stat64i32, 0, NULL, "_fstat64"},
    {"_stat64i32", h_stat64i32, 0, NULL, "_stat64"},
    {"_wstat64i32", h_stat64i32, 0, NULL, "_wstat64"},
    {"_fstat64i32", h_stat64i32, 0, NULL, "_fstat64"},
    {"difftime", NULL, W64F_MATH | M_D_LL << 8, NULL, "_difftime64"},
    W64_FN("div", h_div),
    W64_FN("ldiv", h_div),
    W64_FN("lldiv", h_lldiv),
    W64_FN("qsort", h_qsort),
    W64_FN("qsort_s", h_qsort_s),
    W64_FN("bsearch", h_bsearch),
    W64_NATIVE("_setjmp", w64_setjmp),
    W64_NATIVE("_setjmpex", w64_setjmp),
    W64_NATIVE("setjmp", w64_setjmp),
    W64_NATIVE("__intrinsic_setjmp", w64_setjmp),
    W64_NATIVE("__intrinsic_setjmpex", w64_setjmp),
    W64_NATIVE("longjmp", w64_longjmp),
    W64_NATIVE("memcpy", w64_memcpy),
    W64_NATIVE("memmove", w64_memmove),
    W64_NATIVE("memset", w64_memset),
    W64_NATIVE("strlen", w64_strlen),
    W64_VA("printf", 0, "vprintf"),
    W64_VA("wprintf", 0, "vwprintf"),
    W64_VA("fprintf", 1, "vfprintf"),
    W64_VA("fwprintf", 1, "vfwprintf"),
    W64_VA("sprintf", 1, "vsprintf"),
    W64_VA("swprintf", 1, "_vswprintf"),
    W64_VA("_swprintf", 1, "_vswprintf"),
    W64_VA("_snprintf", 2, "_vsnprintf"),
    W64_VA("snprintf", 2, "vsnprintf"),
    W64_VA("_snwprintf", 2, "_vsnwprintf"),
    W64_VA("sprintf_s", 2, "vsprintf_s"),
    W64_VA("swprintf_s", 2, "vswprintf_s"),
    W64_VA("_snprintf_s", 3, "_vsnprintf_s"),
    W64_VA("_scprintf", 0, "_vscprintf"),
    W64_VA("_scwprintf", 0, "_vscwprintf"),
    W64_VA("scanf", 0, "vscanf"),
    W64_VA("fscanf", 1, "vfscanf"),
    W64_VA("sscanf", 1, "vsscanf"),
    W64_VA("swscanf", 1, "vswscanf"),
    W64_VA("fwscanf", 1, "vfwscanf"),
    W64_FLAG("vprintf", W64F_FMT),
    W64_FLAG("vwprintf", W64F_FMT),
    W64_FLAG("vfprintf", W64F_FMT),
    W64_FLAG("vfwprintf", W64F_FMT),
    W64_FLAG("vsprintf", W64F_FMT),
    W64_FLAG("vswprintf", W64F_FMT),
    W64_FLAG("_vswprintf", W64F_FMT),
    W64_FLAG("_vsnprintf", W64F_FMT),
    W64_FLAG("vsnprintf", W64F_FMT),
    W64_FLAG("_vsnwprintf", W64F_FMT),
    W64_FLAG("vsprintf_s", W64F_FMT),
    W64_FLAG("vswprintf_s", W64F_FMT),
    W64_FLAG("_vsnprintf_s", W64F_FMT),
    W64_FLAG("_vscprintf", W64F_FMT),
    W64_FLAG("_vscwprintf", W64F_FMT),
    W64_FLAG("vscanf", W64F_FMT),
    W64_FLAG("vfscanf", W64F_FMT),
    W64_FLAG("vsscanf", W64F_FMT),
    W64_FLAG("vswscanf", W64F_FMT),
    W64_FLAG("vfwscanf", W64F_FMT),
    W64_MATH("sqrt", M_D_D),
    W64_MATH("sin", M_D_D),
    W64_MATH("cos", M_D_D),
    W64_MATH("tan", M_D_D),
    W64_MATH("atan", M_D_D),
    W64_MATH("asin", M_D_D),
    W64_MATH("acos", M_D_D),
    W64_MATH("exp", M_D_D),
    W64_MATH("log", M_D_D),
    W64_MATH("log10", M_D_D),
    W64_MATH("log2", M_D_D),
    W64_MATH("exp2", M_D_D),
    W64_MATH("floor", M_D_D),
    W64_MATH("ceil", M_D_D),
    W64_MATH("trunc", M_D_D),
    W64_MATH("round", M_D_D),
    W64_MATH("rint", M_D_D),
    W64_MATH("nearbyint", M_D_D),
    W64_MATH("fabs", M_D_D),
    W64_MATH("sinh", M_D_D),
    W64_MATH("cosh", M_D_D),
    W64_MATH("tanh", M_D_D),
    W64_MATH("cbrt", M_D_D),
    W64_MATH("_chgsign", M_D_D),
    W64_MATH("_logb", M_D_D),
    W64_MATH("atan2", M_D_DD),
    W64_MATH("pow", M_D_DD),
    W64_MATH("fmod", M_D_DD),
    W64_MATH("hypot", M_D_DD),
    W64_MATH("_hypot", M_D_DD),
    W64_MATH("copysign", M_D_DD),
    W64_MATH("_copysign", M_D_DD),
    W64_MATH("nextafter", M_D_DD),
    W64_MATH("_nextafter", M_D_DD),
    W64_MATH("ldexp", M_D_DI),
    W64_MATH("_scalb", M_D_DI),
    W64_MATH("frexp", M_D_DP),
    W64_MATH("modf", M_D_DP),
    W64_MATH("lround", M_I_D),
    W64_MATH("_finite", M_I_D),
    W64_MATH("_isnan", M_I_D),
    W64_MATH("_fpclass", M_I_D),
    W64_MATH("sqrtf", M_F_F),
    W64_MATH("sinf", M_F_F),
    W64_MATH("cosf", M_F_F),
    W64_MATH("tanf", M_F_F),
    W64_MATH("atanf", M_F_F),
    W64_MATH("expf", M_F_F),
    W64_MATH("logf", M_F_F),
    W64_MATH("log10f", M_F_F),
    W64_MATH("floorf", M_F_F),
    W64_MATH("ceilf", M_F_F),
    W64_MATH("fabsf", M_F_F),
    W64_MATH("atan2f", M_F_FF),
    W64_MATH("powf", M_F_FF),
    W64_MATH("fmodf", M_F_FF),
};
W64_EXPORTS(msvcrt_module, "msvcrt.dll", msvcrt_exports);
