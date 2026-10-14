#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>
#include <synth/winpath.h>

#include "win32.h"

#define DIR_EXPORT 0
#define DIR_IMPORT 1
#define DIR_RELOC 5
#define DIR_TLS 9

SYNTH_TABLE_DECLARE(synth_win32, struct w32_module_def);

struct pe_info {
    const uint8_t *file;
    size_t file_size;
    uint32_t coff;
    uint32_t opt;
    uint16_t sections;
    uint16_t characteristics;
    uint32_t entry;
    uint64_t image_base;
    bool pe64;
    uint32_t image_size;
    uint32_t headers_size;
    uint32_t dirs;
    uint32_t dir_count;
};

static W32_STATE struct w32_image *init_order[64];
static W32_STATE int init_count;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static bool parse_headers(const uint8_t *file, size_t size, struct pe_info *pe)
{
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z')
        return false;
    uint32_t lfanew = rd32(file + 0x3c);
    if (lfanew + 24 + 96 > size || memcmp(file + lfanew, "PE\0\0", 4) != 0)
        return false;
    pe->file = file;
    pe->file_size = size;
    pe->coff = lfanew + 4;
    pe->opt = pe->coff + 20;
    uint16_t machine = rd16(file + pe->coff), magic = rd16(file + pe->opt);
    pe->pe64 = machine == 0x8664 && magic == 0x20b;
    if (!pe->pe64 && (machine != 0x14c || magic != 0x10b))
        return false;
    if (pe->pe64 && lfanew + 24 + 112 > size)
        return false;
    pe->sections = rd16(file + pe->coff + 2);
    pe->characteristics = rd16(file + pe->coff + 18);
    pe->entry = rd32(file + pe->opt + 16);
    pe->image_base = pe->pe64 ? rd64(file + pe->opt + 24) : rd32(file + pe->opt + 28);
    pe->image_size = ALIGN_UP(rd32(file + pe->opt + 56), 4096u);
    pe->headers_size = rd32(file + pe->opt + 60);
    pe->dir_count = MIN(rd32(file + pe->opt + (pe->pe64 ? 108 : 92)), 16u);
    pe->dirs = pe->opt + (pe->pe64 ? 112 : 96);
    return pe->image_size > 0 && pe->image_size < 512u * 1024 * 1024;
}

bool w32_is_pe(const uint8_t *file, size_t size)
{
    struct pe_info pe;
    return parse_headers(file, size, &pe);
}

bool w32_is_pe64(const uint8_t *file, size_t size)
{
    struct pe_info pe;
    return parse_headers(file, size, &pe) && pe.pe64;
}

static void dir_of(const struct pe_info *pe, int index, uint32_t *rva, uint32_t *size)
{
    *rva = *size = 0;
    if ((uint32_t)index < pe->dir_count) {
        *rva = rd32(pe->file + pe->dirs + (uint32_t)index * 8);
        *size = rd32(pe->file + pe->dirs + (uint32_t)index * 8 + 4);
    }
}

static void normalize(const char *name, char *out, size_t cap)
{
    str_copy(out, path_basename(name), cap);
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    str_copy(out, path_basename(out), cap);
    str_lower(out);
    if (!strchr(out, '.'))
        str_append(out, ".dll", cap);
}

static const char *builtin_alias(const char *name)
{
    if (str_starts(name, "api-ms-win-crt-") || strcmp(name, "ucrtbase.dll") == 0)
        return "msvcrt.dll";
    if (str_starts(name, "api-ms-win-core-") || strcmp(name, "kernelbase.dll") == 0)
        return "kernel32.dll";
    if (str_starts(name, "msvcr") || str_starts(name, "vcruntime") || strcmp(name, "crtdll.dll") == 0)
        return "msvcrt.dll";
    static const char *const sets[][2] = {{"api-ms-win-shcore-", "shcore.dll"},
                                          {"api-ms-win-downlevel-shell32-", "shell32.dll"},
                                          {"api-ms-win-downlevel-shlwapi-", "shlwapi.dll"},
                                          {"api-ms-win-downlevel-user32-", "user32.dll"},
                                          {"api-ms-win-downlevel-advapi32-", "advapi32.dll"},
                                          {"api-ms-win-security-", "advapi32.dll"},
                                          {"api-ms-win-power-", "powrprof.dll"},
                                          {"api-ms-win-appmodel-", "kernel32.dll"}};
    for (size_t i = 0; i < ARRAY_SIZE(sets); i++)
        if (str_starts(name, sets[i][0]))
            return sets[i][1];
    return name;
}

static const struct w32_module_def *builtin_find(const char *name)
{
    SYNTH_TABLE_FOREACH(synth_win32, struct w32_module_def, def)
        if (strcmp(def->name, name) == 0)
            return def;
    return NULL;
}

static void *builtin_lookup(const char *module, const char *symbol)
{
    SYNTH_TABLE_FOREACH(synth_win32, struct w32_module_def, def) {
        if (strcmp(def->name, module) != 0)
            continue;
        for (size_t i = 0; i < def->count; i++)
            if (strcmp(def->exports[i].name, symbol) == 0)
                return def->exports[i].address;
    }
    return NULL;
}

static void *builtin_ordinal(const char *module, uint32_t ordinal)
{
    SYNTH_TABLE_FOREACH(synth_win32, struct w32_module_def, def)
        if (!strcmp(def->name, module) && def->ordinals && ordinal < def->ordinal_count && def->ordinals[ordinal])
            return builtin_lookup(module, def->ordinals[ordinal]);
    return NULL;
}

void *w32_builtin_lookup(const char *module, const char *symbol)
{
    return builtin_lookup(module, symbol);
}

const char *w32_builtin_ordinal_name(const char *module, uint32_t ordinal)
{
    SYNTH_TABLE_FOREACH(synth_win32, struct w32_module_def, def)
        if (!strcmp(def->name, module) && def->ordinals && ordinal < def->ordinal_count)
            return def->ordinals[ordinal];
    return NULL;
}

static void w32_missing(const char *what)
{
    ksnprintf(w32->message, sizeof(w32->message), "%s is not supported yet", what);
    w32_exit(0xc0000139u);
}

static void *missing_stub(const char *module, const char *symbol)
{
    char name[128];
    ksnprintf(name, sizeof(name), "%s!%s", module, symbol);
    char *saved = w32_strdup(name);
    uint8_t *code = w32_alloc(12);
    if (!saved || !code)
        return NULL;
    code[0] = 0x68;
    memcpy(code + 1, &saved, 4);
    code[5] = 0xe8;
    int32_t rel = (int32_t)((uintptr_t)w32_missing - (uintptr_t)(code + 10));
    memcpy(code + 6, &rel, 4);
    code[10] = 0xcc;
    return code;
}

struct w32_image *w32_find_module(const char *name)
{
    char norm[64];
    normalize(name, norm, sizeof(norm));
    const char *alias = builtin_alias(norm);
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (str_casecmp(img->name, norm) == 0 || str_casecmp(img->name, alias) == 0)
            return img;
    return NULL;
}

static struct w32_image *image_new(const char *name)
{
    struct w32_image *img = kzalloc(sizeof(*img));
    if (!img)
        return NULL;
    str_copy(img->name, name, sizeof(img->name));
    img->refs = 1;
    img->next = w32->images;
    w32->images = img;
    return img;
}

static struct w32_image *load_builtin(const char *name)
{
    struct w32_image *img = image_new(name);
    if (!img)
        return NULL;
    uint8_t *header = heap_alloc_aligned(&w32->heap, 4096, 4096);
    if (!header)
        return img;
    header[0] = 'M';
    header[1] = 'Z';
    header[0x3c] = 0x40;
    memcpy(header + 0x40, "PE\0\0", 4);
    header[0x44] = 0x4c;
    header[0x45] = 0x01;
    header[0x54] = 0xe0;
    header[0x56] = 0x02;
    header[0x57] = 0x21;
    header[0x58] = 0x0b;
    header[0x59] = 0x01;
    uint32_t size = 4096;
    memcpy(header + 0x58 + 56, &size, 4);
    img->base = (uint32_t)header;
    img->size = 4096;
    img->dll = true;
    img->initialized = true;
    ksnprintf(img->path, sizeof(img->path), "/system/win32/%s", name);
    return img;
}

static uint32_t pick_base(uint64_t preferred, uint32_t size, bool relocatable)
{
    bool clear = preferred + size <= w32->arena_base || preferred >= (uint64_t)w32->arena_base + w32->arena_size;
    if (clear && preferred >= VMM_USER_START && preferred + size <= VMM_USER_END &&
        vmm_range_free((uint32_t)preferred, size))
        return (uint32_t)preferred;
    if (!relocatable)
        return 0;
    for (uint32_t base = 0x10000000u; base < 0xb0000000u; base += 0x10000u)
        if ((base + size <= w32->arena_base || base >= w32->arena_base + w32->arena_size) &&
            vmm_range_free(base, size))
            return base;
    return 0;
}

static bool apply_relocations(const struct pe_info *pe, uint8_t *mem, uint64_t delta)
{
    uint32_t rva, size;
    dir_of(pe, DIR_RELOC, &rva, &size);
    if (!delta)
        return true;
    if (!rva || !size || (pe->characteristics & 0x0001))
        return false;
    uint32_t off = 0;
    while (off + 8 <= size && rva + off + 8 <= pe->image_size) {
        uint32_t page = rd32(mem + rva + off), block = rd32(mem + rva + off + 4);
        if (block < 8)
            break;
        for (uint32_t i = 8; i + 2 <= block; i += 2) {
            uint16_t entry = rd16(mem + rva + off + i);
            uint32_t target = page + (entry & 0xfff);
            if (target + 4 > pe->image_size)
                continue;
            switch (entry >> 12) {
            case 3: {
                uint32_t v = rd32(mem + target) + (uint32_t)delta;
                memcpy(mem + target, &v, 4);
                break;
            }
            case 10: {
                if (target + 8 > pe->image_size)
                    break;
                uint64_t v = rd64(mem + target) + delta;
                memcpy(mem + target, &v, 8);
                break;
            }
            case 2: {
                uint16_t v = (uint16_t)(rd16(mem + target) + (delta & 0xffff));
                memcpy(mem + target, &v, 2);
                break;
            }
            case 1: {
                uint16_t v = (uint16_t)(rd16(mem + target) + (uint16_t)(delta >> 16));
                memcpy(mem + target, &v, 2);
                break;
            }
            default:
                break;
            }
        }
        off += block;
    }
    return true;
}

static void *export_lookup(struct w32_image *img, const char *name, uint32_t ordinal, int depth);

bool w32_trace;

#define TRACE_RING 96

static char (*trace_ring)[48];
static unsigned trace_pos;

static void trace_record(const char *name)
{
    if (!trace_ring && !(trace_ring = kzalloc(TRACE_RING * sizeof(*trace_ring))))
        return;
    ksnprintf(trace_ring[trace_pos++ % TRACE_RING], sizeof(*trace_ring), "%s%s", w32 && w32->parent ? "child " : "",
              name);
}

static void trace_call(const char *name)
{
    trace_record(name);
    if (w32 && w32->current)
        w32->current->recent[w32->current->recent_pos++ % 8] = name;
}

void w32_trace_note(const char *module, const char *name)
{
    char label[64];
    if (!w32_trace)
        return;
    ksnprintf(label, sizeof(label), "GetProcAddress %s!%s", module, name);
    trace_record(label);
}

void w32_trace_dump(void)
{
    if (!w32_trace || !trace_ring)
        return;
    klog("win32: last API calls, oldest first:");
    for (unsigned i = 0; i < TRACE_RING; i++) {
        const char *name = trace_ring[(trace_pos + i) % TRACE_RING];
        if (*name)
            klog("  %s", name);
    }
}

static void *relay(void *fn, const char *module, const char *symbol)
{
    static const char *const noisy[] = {"SRWLock", "CriticalSection", "LastError", "TlsGetValue", "TlsSetValue",
                                        "FlsGetValue", "Interlocked", "HeapAlloc", "HeapFree", "HeapReAlloc"};
    if (!w32_trace || !fn)
        return fn;
    for (size_t i = 0; i < ARRAY_SIZE(noisy); i++)
        if (strstr(symbol, noisy[i]))
            return fn;
    char label[96];
    ksnprintf(label, sizeof(label), "%s!%s", module, symbol);
    char *name = kstrdup(label);
    uint8_t *code = w32_alloc(24);
    if (!name || !code)
        return fn;
    code[0] = 0x60;
    code[1] = 0x9c;
    code[2] = 0x68;
    memcpy(code + 3, &name, 4);
    code[7] = 0xe8;
    int32_t rel = (int32_t)((uintptr_t)trace_call - (uintptr_t)(code + 12));
    memcpy(code + 8, &rel, 4);
    code[12] = 0x83;
    code[13] = 0xc4;
    code[14] = 0x04;
    code[15] = 0x9d;
    code[16] = 0x61;
    code[17] = 0xe9;
    rel = (int32_t)((uintptr_t)fn - (uintptr_t)(code + 22));
    memcpy(code + 18, &rel, 4);
    return code;
}

static void *resolve(struct w32_image *dll, const char *module, const char *symbol, uint32_t ordinal, bool pe64)
{
    void *fn = NULL;
    if (pe64 && dll->builtin)
        return w64_stub(dll->name, symbol, ordinal);
    if (pe64 != dll->pe64 && !dll->builtin)
        return pe64 ? w64_stub(module, symbol, ordinal) : missing_stub(module, symbol ? symbol : "?");
    if (dll->builtin) {
        fn = symbol ? builtin_lookup(dll->name, symbol) : builtin_ordinal(dll->name, ordinal);
        if (fn && symbol)
            return relay(fn, dll->name, symbol);
    } else {
        fn = export_lookup(dll, symbol, ordinal, 0);
    }
    if (fn)
        return fn;
    char sym[32];
    if (!symbol) {
        ksnprintf(sym, sizeof(sym), "#%u", ordinal);
        symbol = sym;
    }
    klog("win32: unresolved %s!%s", module, symbol);
    return missing_stub(module, symbol);
}

static void *export_lookup(struct w32_image *img, const char *name, uint32_t ordinal, int depth)
{
    if (img->builtin)
        return name ? builtin_lookup(img->name, name) : builtin_ordinal(img->name, ordinal);
    const uint8_t *mem = (const uint8_t *)img->base;
    uint32_t lfanew = rd32(mem + 0x3c);
    uint32_t opt = lfanew + 24;
    uint32_t dirs = opt + (img->pe64 ? 112 : 96);
    uint32_t rva = rd32(mem + dirs), size = rd32(mem + dirs + 4);
    if (!rva || rva + 40 > img->size)
        return NULL;
    const uint8_t *dir = mem + rva;
    uint32_t base = rd32(dir + 16), nfuncs = rd32(dir + 20), nnames = rd32(dir + 24);
    uint32_t funcs = rd32(dir + 28), names = rd32(dir + 32), ordinals = rd32(dir + 36);
    uint32_t index = 0xffffffffu;
    if (name) {
        for (uint32_t i = 0; i < nnames; i++) {
            const char *n = (const char *)mem + rd32(mem + names + i * 4);
            if (strcmp(n, name) == 0) {
                index = rd16(mem + ordinals + i * 2);
                break;
            }
        }
    } else if (ordinal >= base) {
        index = ordinal - base;
    }
    if (index >= nfuncs)
        return NULL;
    uint32_t fn = rd32(mem + funcs + index * 4);
    if (!fn)
        return NULL;
    if (fn >= rva && fn < rva + size && depth < 4) {
        char forward[128];
        str_copy(forward, (const char *)mem + fn, sizeof(forward));
        char *dot = strchr(forward, '.');
        if (!dot)
            return NULL;
        *dot = 0;
        int err;
        struct w32_image *target = w32_load(forward, NULL, &err);
        if (!target)
            return NULL;
        if (img->pe64 && target->builtin)
            return w64_stub(target->name, dot[1] == '#' ? NULL : dot + 1,
                            dot[1] == '#' ? (uint32_t)str_to_long(dot + 2, NULL, 10) : 0);
        return dot[1] == '#' ? export_lookup(target, NULL, (uint32_t)str_to_long(dot + 2, NULL, 10), depth + 1)
                             : (target->builtin ? builtin_lookup(target->name, dot + 1)
                                                : export_lookup(target, dot + 1, 0, depth + 1));
    }
    return (void *)(img->base + fn);
}

static bool system_dll(const char *name)
{
    static const char *const known =
        "cfgmgr32.dll,comctl32.dll,comdlg32.dll,cryptui.dll,dwrite.dll,esent.dll,hid.dll,imm32.dll,mf.dll,"
        "mfplat.dll,mfreadwrite.dll,mmdevapi.dll,ndfapi.dll,netapi32.dll,propsys.dll,setupapi.dll,"
        "shlwapi.dll,userenv.dll,uxtheme.dll,winmm.dll,winspool.drv,wintrust.dll,winusb.dll,"
        "wtsapi32.dll,bcryptprimitives.dll,bthprops.cpl,credui.dll,d3d11.dll,d3d12.dll,d3d9.dll,dbghelp.dll,"
        "dcomp.dll,dhcpcsvc.dll,dwmapi.dll,dxgi.dll,ncrypt.dll,pdh.dll,tbs.dll,wevtapi.dll,"
        "shcore.dll,dinput8.dll,xinput1_3.dll,xinput1_4.dll,xinput9_1_0.dll,dsound.dll,opengl32.dll,glu32.dll,"
        "msimg32.dll,version.dll,psapi.dll,mpr.dll,urlmon.dll,wininet.dll,winhttp.dll,crypt32.dll,secur32.dll,"
        "dnsapi.dll,mswsock.dll,fontsub.dll,usp10.dll,msvcp140.dll,d2d1.dll,windowscodecs.dll,dxva2.dll,"
        "avrt.dll,ksuser.dll,mscms.dll,sensapi.dll,wlanapi.dll,dbgcore.dll,ntdsapi.dll,authz.dll";
    char norm[64];
    normalize(name, norm, sizeof(norm));
    return str_starts(norm, "api-ms-win-") || str_starts(norm, "ext-ms-") || str_in_list(norm, known);
}

bool w32_is_system_dll(const char *name)
{
    char norm[64];
    normalize(name, norm, sizeof(norm));
    return builtin_find(builtin_alias(norm)) || system_dll(norm) || !strcmp(norm, "kernelbase.dll");
}

static struct w32_image *load_phantom(const char *name)
{
    static const struct w32_module_def phantom = {.name = "phantom"};
    char norm[64];
    normalize(name, norm, sizeof(norm));
    struct w32_image *img = load_builtin(norm);
    if (img)
        img->builtin = &phantom;
    klog("win32: %s is not available, its functions will report themselves when called", norm);
    return img;
}

static int resolve_imports(struct w32_image *img, const struct pe_info *pe, const char *search_dir)
{
    uint8_t *mem = (uint8_t *)img->base;
    uint32_t rva, size;
    dir_of(pe, DIR_IMPORT, &rva, &size);
    if (!rva)
        return 0;
    for (uint32_t desc = rva; desc + 20 <= img->size; desc += 20) {
        uint32_t lookup = rd32(mem + desc), name_rva = rd32(mem + desc + 12), iat = rd32(mem + desc + 16);
        if (!name_rva && !iat)
            break;
        if (name_rva >= img->size || iat >= img->size)
            return ERROR_INVALID_DATA;
        const char *dll_name = (const char *)mem + name_rva;
        int err = 0;
        struct w32_image *dll = w32_load(dll_name, search_dir, &err);
        if (!dll && system_dll(dll_name))
            dll = load_phantom(dll_name);
        if (!dll) {
            if (!w32->message[0])
                ksnprintf(w32->message, sizeof(w32->message), "required library %s was not found", dll_name);
            return ERROR_MOD_NOT_FOUND;
        }
        if (!lookup)
            lookup = iat;
        uint32_t step = pe->pe64 ? 8 : 4;
        for (uint32_t i = 0;; i++) {
            uint64_t entry = pe->pe64 ? rd64(mem + lookup + i * step) : rd32(mem + lookup + i * step);
            if (!entry)
                break;
            bool by_ordinal = pe->pe64 ? entry >> 63 : entry >> 31;
            void *fn;
            if (by_ordinal) {
                fn = resolve(dll, dll_name, NULL, (uint32_t)entry & 0xffff, pe->pe64);
            } else {
                uint32_t hint = (uint32_t)entry & 0x7fffffffu;
                if (hint + 2 >= img->size)
                    return ERROR_INVALID_DATA;
                fn = resolve(dll, dll_name, (const char *)mem + hint + 2, 0, pe->pe64);
            }
            uint64_t addr = (uint32_t)fn;
            memcpy(mem + iat + i * step, &addr, step);
        }
    }
    return 0;
}

#define CACHE_MIN (4u * 1024 * 1024)

struct image_cache {
    char path[PATH_MAX_LEN];
    uint32_t base, size, file_size;
    void **pages;
    int users;
    struct image_cache *next;
};

static struct image_cache *image_caches;

static struct image_cache *cache_find(const char *path, uint32_t base, uint32_t size, uint32_t file_size)
{
    for (struct image_cache *c = image_caches; c; c = c->next)
        if (c->base == base && c->size == size && c->file_size == file_size && !strcmp(c->path, path))
            return c;
    return NULL;
}

static void cache_free(struct image_cache *c)
{
    for (uint32_t i = 0; c->pages && i < c->size / 4096u; i++)
        kfree_pages(c->pages[i]);
    kfree(c->pages);
    kfree(c);
}

static struct image_cache *cache_new(const char *path, uint32_t base, uint32_t size, uint32_t file_size)
{
    struct image_cache *c = kzalloc(sizeof(*c));
    uint32_t count = ALIGN_UP(size, 4096u) / 4096u;
    if (!c || !(c->pages = kzalloc(count * sizeof(*c->pages)))) {
        kfree(c);
        return NULL;
    }
    str_copy(c->path, path, sizeof(c->path));
    c->base = base;
    c->size = count * 4096u;
    c->file_size = file_size;
    for (uint32_t i = 0; i < count; i++)
        if (!(c->pages[i] = kmalloc_pages(4096))) {
            cache_free(c);
            return NULL;
        }
    return c;
}

static void cache_put(struct image_cache *c, uint32_t base)
{
    vmm_release(base, c->size);
    vmm_unmap(base, c->size);
    if (--c->users > 0)
        return;
    for (struct image_cache **pp = &image_caches; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    cache_free(c);
}

static struct w32_image *load_file(const char *path, const char *name, int *error)
{
    struct file *f;
    struct vstat st;
    if (vfs_open(path, VFS_READ, &f) != 0) {
        *error = ERROR_FILE_NOT_FOUND;
        return NULL;
    }
    if (vfs_fstat(f, &st) != 0) {
        vfs_close(f);
        *error = ERROR_FILE_NOT_FOUND;
        return NULL;
    }
    size_t size = st.size, head_size = MIN(size, (size_t)65536);
    uint8_t *data = kmalloc(MAX(head_size, (size_t)1));
    if (!data || vfs_read(f, data, head_size) != (long)head_size) {
        kfree(data);
        vfs_close(f);
        *error = ERROR_FILE_NOT_FOUND;
        return NULL;
    }
    struct pe_info pe;
    if (!parse_headers(data, head_size, &pe)) {
        kfree(data);
        vfs_close(f);
        *error = 193;
        return NULL;
    }
    uint32_t reloc_rva, reloc_size;
    dir_of(&pe, DIR_RELOC, &reloc_rva, &reloc_size);
    bool relocatable = reloc_rva && reloc_size && !(pe.characteristics & 0x0001);
    uint32_t base = pick_base(pe.image_base, pe.image_size, relocatable);
    struct image_cache *cache = NULL;
    bool cached = false;
    if (base && pe.image_size >= CACHE_MIN) {
        if ((cache = cache_find(path, base, ALIGN_UP(pe.image_size, 4096u), (uint32_t)size)) &&
            vmm_map_pages(base, cache->pages, cache->size / 4096u, true) == 0) {
            cache->users++;
            cached = true;
        } else if ((cache = cache_new(path, base, pe.image_size, (uint32_t)size)) &&
                   vmm_map_pages(base, cache->pages, cache->size / 4096u, false) != 0) {
            cache_free(cache);
            cache = NULL;
        }
    }
    if (!base || (!cache && vmm_commit_range(base, pe.image_size) != 0)) {
        kfree(data);
        vfs_close(f);
        *error = base ? ERROR_NOT_ENOUGH_MEMORY : 193;
        return NULL;
    }
    uint8_t *mem = (uint8_t *)base;
    if (!cached)
        memcpy(mem, data, MIN((size_t)pe.headers_size, MIN(head_size, (size_t)pe.image_size)));
    uint32_t sec = pe.opt + rd16(pe.file + pe.coff + 16);
    for (uint16_t i = 0; !cached && i < pe.sections && sec + 40 <= head_size; i++, sec += 40) {
        uint32_t vsize = rd32(pe.file + sec + 8), va = rd32(pe.file + sec + 12);
        uint32_t raw = rd32(pe.file + sec + 16), ptr = rd32(pe.file + sec + 20);
        uint32_t copy = vsize && vsize < raw ? vsize : raw;
        if (va >= pe.image_size || ptr >= size)
            continue;
        copy = MIN(copy, MIN(pe.image_size - va, (uint32_t)(size - ptr)));
        vfs_seek(f, (long)ptr, VFS_SEEK_SET);
        if (vfs_read(f, mem + va, copy) != (long)copy) {
            if (cache) {
                vmm_unmap(base, cache->size);
                cache_free(cache);
            }
            vmm_release(base, pe.image_size);
            kfree(data);
            vfs_close(f);
            *error = ERROR_FILE_NOT_FOUND;
            return NULL;
        }
    }
    kfree(data);
    vfs_close(f);
    uint8_t *memory = NULL;

    struct w32_image *img = image_new(name);
    if (!img) {
        vmm_release(base, pe.image_size);
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    str_copy(img->path, path, sizeof(img->path));
    img->pe64 = pe.pe64;
    img->base = base;
    img->size = pe.image_size;
    img->memory = memory;
    img->entry = pe.entry ? base + pe.entry : 0;
    img->dll = pe.characteristics & 0x2000;
    if (base != pe.image_base && !cached) {
        uint64_t delta = base - pe.image_base, base64 = base;
        if (pe.pe64)
            memcpy(mem + pe.opt + 24, &base64, 8);
        else
            memcpy(mem + pe.opt + 28, &base, 4);
        apply_relocations(&pe, mem, delta);
        klog("win32: %s relocated to 0x%08x", name, base);
    }
    if (cache && !cached) {
        vmm_make_cow(base, cache->size);
        cache->users = 1;
        cache->next = image_caches;
        image_caches = cache;
    }
    img->cache = cache;
    pe.file = mem;
    char dir[PATH_MAX_LEN];
    path_dirname(path, dir, sizeof(dir));
    int err2 = resolve_imports(img, &pe, dir);
    if (err2) {
        *error = err2;
        return NULL;
    }
    if (!pe.pe64)
        w32_tls_register(img);
    if (img->dll && init_count < (int)ARRAY_SIZE(init_order))
        init_order[init_count++] = img;
    else if (!img->dll)
        img->initialized = true;
    return img;
}

static bool find_in_assemblies(const char *name, const char *dir, char *out)
{
    struct vdir d;
    if (!dir || vfs_opendir(dir, &d) != 0)
        return false;
    struct vfs_dirent ent;
    bool found = false;
    while (!found && vfs_readdir(&d, &ent) == 0) {
        if (ent.type != VN_DIR || ent.name[0] == '.')
            continue;
        char manifest[PATH_MAX_LEN], file[NAME_MAX_LEN + 16], candidate[PATH_MAX_LEN];
        ksnprintf(file, sizeof(file), "%s/%s.manifest", ent.name, ent.name);
        if (path_join(dir, file, manifest, sizeof(manifest)) != 0 || !vfs_exists(manifest))
            continue;
        ksnprintf(file, sizeof(file), "%s/%s", ent.name, name);
        if (path_join(dir, file, candidate, sizeof(candidate)) == 0 &&
            vfs_resolve_nocase(candidate, out, PATH_MAX_LEN) == 0 && vfs_exists(out) && !vfs_is_dir(out))
            found = true;
    }
    vfs_closedir(&d);
    return found;
}

static bool find_on_disk(const char *name, const char *search_dir, char *out)
{
    const char *dirs[4] = {search_dir, w32->cwd, "/system/win32", "/apps"};
    for (int i = 0; i < 4; i++) {
        if (!dirs[i])
            continue;
        char candidate[PATH_MAX_LEN];
        if (path_join(dirs[i], name, candidate, sizeof(candidate)) != 0)
            continue;
        if (vfs_resolve_nocase(candidate, out, PATH_MAX_LEN) == 0 && vfs_exists(out) && !vfs_is_dir(out))
            return true;
    }
    return find_in_assemblies(name, search_dir, out);
}

struct w32_image *w32_load(const char *name, const char *search_dir, int *error)
{
    struct w32_image *img = w32_find_module(name);
    if (img) {
        img->refs++;
        return img;
    }
    char norm[64];
    normalize(name, norm, sizeof(norm));
    char path[PATH_MAX_LEN];
    const char *alias = builtin_alias(norm);
    const struct w32_module_def *def = builtin_find(alias);
    if (!def && (strchr(name, '\\') || strchr(name, '/') || (name[0] && name[1] == ':'))) {
        if (w32_path(name, path, sizeof(path)) == 0 && vfs_exists(path))
            return load_file(path, norm, error);
    }
    if (def) {
        img = load_builtin(alias);
        if (img) {
            img->builtin = def;
            SYNTH_TABLE_FOREACH(synth_win32, struct w32_module_def, d)
                if (d->attach && strcmp(d->name, alias) == 0)
                    d->attach();
        }
        return img;
    }
    if (find_on_disk(norm, search_dir, path))
        return load_file(path, norm, error);
    *error = ERROR_MOD_NOT_FOUND;
    return NULL;
}

void win32_install(void)
{
    static const char marker[] = "SynthOS built-in Windows module\n";
    SYNTH_TABLE_FOREACH(synth_win32, struct w32_module_def, def) {
        char path[PATH_MAX_LEN];
        ksnprintf(path, sizeof(path), "/system/win32/%s", def->name);
        if (!vfs_exists(path))
            vfs_write_all(path, marker, sizeof(marker) - 1);
    }
}

struct w32_image *w32_load_main(const char *path, int *error)
{
    init_count = 0;
    struct w32_image *img = load_file(path, path_basename(path), error);
    if (img)
        img->dll = false;
    return img;
}

void *w32_get_proc(struct w32_image *image, const char *name, uint32_t ordinal)
{
    return export_lookup(image, name, ordinal, 0);
}

int w32_init_modules(void)
{
    for (int i = 0; i < init_count; i++) {
        struct w32_image *img = init_order[i];
        if (img->initialized)
            continue;
        img->initialized = true;
        if (!img->pe64)
            w32_tls_callbacks(img->base, 1);
        if (!img->entry || w32->exiting)
            continue;
        uint32_t ok;
        if (img->pe64) {
            uint64_t args[3] = {img->base, 1, 0};
            ok = (uint32_t)arch_w64_call(img->entry, 3, args);
        } else {
            uint32_t args[3] = {img->base, 1, 0};
            ok = w32_call((void *)img->entry, 3, args);
        }
        if (w32->exiting)
            return -1;
        if (!ok) {
            ksnprintf(w32->message, sizeof(w32->message), "%s failed to initialize", img->name);
            return -1;
        }
    }
    init_count = 0;
    return 0;
}

void w32_unload_all(void)
{
    struct w32_image *img = w32->images;
    while (img) {
        struct w32_image *next = img->next;
        if (img->memory) {
            vmm_unmap(img->base, img->size);
            kfree_pages(img->memory);
        } else if (img->cache) {
            cache_put(img->cache, img->base);
        } else if (!img->builtin && img->base) {
            vmm_release(img->base, img->size);
        }
        kfree(img);
        img = next;
    }
    w32->images = NULL;
}
