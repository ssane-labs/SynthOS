#include <synth/arch_win32.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "win64.h"

/* ntdll.dll for 64-bit programs as a real image: PE32+ headers, an export directory, and every function as a
   Windows 10 system-call stub (mov r10,rcx; mov eax,N; ...; syscall). Programs that read ntdll's exports
   themselves, or copy and patch its stubs (Chromium's sandbox), see what they expect; the syscall instruction
   arrives at w64_syscall, which jumps to the built-in through the service table. The image sits at the same address
   in every process, as on Windows; process.c maps KUSER_SHARED_DATA next to it. */

#ifdef __x86_64__

SYNTH_TABLE_DECLARE(synth_win64, struct w64_module);

#define NTDLL_BASE 0x7ff00000u
#define SERVICES 0x7fc00000u
#define MAX_SERVICES 65536u
#define STUB 32u
#define MAX_NAMES 4096

/* the service table: per entry the target (0 until first called), the export's name and its DLL's name */
struct service {
    uint64_t target;
    uint32_t name, module;
};

uint32_t w64_service_table = SERVICES, w64_service_count, w64_service_resolve_fn;
static W32_STATE uint32_t services_used;

struct name_list {
    const char *names[MAX_NAMES];
    int count;
};

static void put16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, 2);
}

static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

static void collect(const char *name, void *arg)
{
    struct name_list *l = arg;
    if (l->count < MAX_NAMES && name[0] != '#' && name[0])
        l->names[l->count++] = name;
}

static void sort_names(const char **v, int n)
{
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++)
            for (int j = i; j >= gap && strcmp(v[j - gap], v[j]) > 0; j -= gap) {
                const char *t = v[j];
                v[j] = v[j - gap];
                v[j - gap] = t;
            }
}

static void collect64(const char *module, struct name_list *l)
{
    SYNTH_TABLE_FOREACH(synth_win64, struct w64_module, m)
        if (!strcmp(m->name, module))
            for (size_t i = 0; i < m->count; i++)
                collect(m->exports[i].name, l);
}

/* called by w64_syscall the first time a service runs */
static uint32_t service_resolve(uint32_t index)
{
    struct service *s = (struct service *)(uintptr_t)SERVICES + index;
    if (index >= services_used || !s->name)
        return 0;
    if (!s->target)
        s->target = (uint32_t)(uintptr_t)w64_lookup((const char *)(uintptr_t)s->module,
                                                    (const char *)(uintptr_t)s->name, 0, true);
    return (uint32_t)s->target;
}

static bool services_ready(void)
{
    if (services_used)
        return true;
    if (!w32_vm_alloc_at(SERVICES, MAX_SERVICES * sizeof(struct service)))
        return false;
    services_used = 1;
    w64_service_count = MAX_SERVICES;
    w64_service_resolve_fn = (uint32_t)(uintptr_t)service_resolve;
    arch_w64_syscall_init();
    return true;
}

static void stub_write(uint8_t *p, uint32_t id)
{
    static const uint8_t code[24] = {0x4c, 0x8b, 0xd1, 0xb8, 0, 0, 0, 0, 0xf6, 0x04, 0x25, 0x08,
                                     0x03, 0xfe, 0x7f, 0x01, 0x75, 0x03, 0x0f, 0x05, 0xc3, 0xcd, 0x2e, 0xc3};
    memcpy(p, code, sizeof(code));
    put32(p + 4, id);
    memset(p + 24, 0xcc, STUB - 24);
}

static void headers_write(uint8_t *m, uint32_t base, uint32_t size, uint32_t exports, uint32_t exports_size)
{
    m[0] = 'M';
    m[1] = 'Z';
    put32(m + 0x3c, 0x40);
    memcpy(m + 0x40, "PE\0\0", 4);
    uint8_t *fh = m + 0x44, *oh = fh + 20;
    put16(fh, 0x8664);
    put16(fh + 2, 1);
    put16(fh + 16, 0xf0);
    put16(fh + 18, 0x2022);
    put16(oh, 0x20b);
    put64(oh + 24, base);
    put32(oh + 32, 0x1000);
    put32(oh + 36, 0x200);
    put16(oh + 40, 10);
    put16(oh + 48, 10);
    put32(oh + 56, size);
    put32(oh + 60, 0x400);
    put16(oh + 68, 3);
    put16(oh + 70, 0x160);
    put32(oh + 108, 16);
    put32(oh + 112, exports);
    put32(oh + 116, exports_size);
    uint8_t *sec = oh + 0xf0;
    memcpy(sec, ".text", 5);
    put32(sec + 8, size - 0x1000);
    put32(sec + 12, 0x1000);
    put32(sec + 16, size - 0x1000);
    put32(sec + 20, 0x1000);
    put32(sec + 36, 0xe0000020u);
}

uint32_t w64_builtin_image(const char *module, uint32_t *size_out)
{
    if (!services_ready())
        return 0;
    struct name_list *l = kmalloc(sizeof(*l));
    if (!l)
        return 0;
    l->count = 0;
    w32_builtin_names(module, collect, l);
    collect64(module, l);
    sort_names(l->names, l->count);
    int n = 0;
    for (int i = 0; i < l->count; i++)
        if (!n || strcmp(l->names[n - 1], l->names[i]))
            l->names[n++] = l->names[i];
    if (!n || services_used + (uint32_t)n > MAX_SERVICES) {
        kfree(l);
        return 0;
    }
    uint32_t names_size = 0;
    for (int i = 0; i < n; i++)
        names_size += (uint32_t)strlen(l->names[i]) + 1;
    size_t module_len = strlen(module) + 1;
    uint32_t stubs = 0x1000, exports = ALIGN_UP(stubs + (uint32_t)n * STUB, 16u);
    uint32_t functions = exports + 40, name_ptrs = functions + (uint32_t)n * 4, ordinals = name_ptrs + (uint32_t)n * 4;
    uint32_t dll_name = ordinals + (uint32_t)n * 2, strings = ALIGN_UP(dll_name + (uint32_t)module_len, 4u);
    uint32_t end = strings + names_size, size = ALIGN_UP(end, 4096u);
    uint32_t base = !strcmp(module, "ntdll.dll") ? w32_vm_alloc_at(NTDLL_BASE, size) : 0;
    if (!base)
        base = w32_vm_alloc_at(0, size);
    if (!base) {
        kfree(l);
        return 0;
    }
    uint8_t *m = (uint8_t *)(uintptr_t)base;
    memset(m, 0, size);
    headers_write(m, base, size, exports, end - exports);
    uint8_t *dir = m + exports;
    put32(dir + 12, dll_name);
    put32(dir + 16, 1);
    put32(dir + 20, (uint32_t)n);
    put32(dir + 24, (uint32_t)n);
    put32(dir + 28, functions);
    put32(dir + 32, name_ptrs);
    put32(dir + 36, ordinals);
    memcpy(m + dll_name, module, module_len);
    struct service *table = (struct service *)(uintptr_t)SERVICES;
    uint32_t at = strings;
    for (int i = 0; i < n; i++) {
        uint32_t id = services_used++, rva = stubs + (uint32_t)i * STUB;
        size_t len = strlen(l->names[i]) + 1;
        memcpy(m + at, l->names[i], len);
        stub_write(m + rva, id);
        table[id] = (struct service){0, base + at, base + dll_name};
        uint32_t data = w64_export_data(module, l->names[i]);
        if (data > base)
            rva = data - base;
        put32(m + functions + (uint32_t)i * 4, rva);
        put32(m + name_ptrs + (uint32_t)i * 4, at);
        put16(m + ordinals + (uint32_t)i * 2, (uint16_t)i);
        at += (uint32_t)len;
    }
    kfree(l);
    *size_out = size;
    return base;
}

#endif
