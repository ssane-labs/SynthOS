#include <synth/config.h>
#if CONFIG_GUI
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/vmm.h>

#include "internal.h"

/* Apps that are not part of the kernel image: /system/apps/<name>.sxm are position-independent ELF objects linked
   against this very kernel (its symbols are absolute, so only RELATIVE relocations remain) and shipped in the
   ramdisk. The entry point is the address of the module's struct gui_module_entry. A module is loaded once and then
   shown or hidden: the App Store turns them on and off, and code that may still run in a window is never freed. */

#define MODULE_DIR "/system/apps"
#define MAX_MODULES 48

#if defined(__x86_64__)
#define ELF_CLASS 2
#define ELF_MACHINE 62
#define REL_RELATIVE 8
#define DT_RELOCS 7  /* DT_RELA */
#define DT_RELOCSZ 8 /* DT_RELASZ */
#define DT_RELOCENT 9
typedef uint64_t elf_word;
struct reloc {
    uint64_t offset, info;
    int64_t addend;
};
#define REL_TYPE(i) ((uint32_t)(i))
#else
#define ELF_CLASS 1
#define ELF_MACHINE 3
#define REL_RELATIVE 8
#define DT_RELOCS 17  /* DT_REL */
#define DT_RELOCSZ 18 /* DT_RELSZ */
#define DT_RELOCENT 19
typedef uint32_t elf_word;
struct reloc {
    uint32_t offset, info;
};
#define REL_TYPE(i) ((i) & 0xff)
#endif

struct elf_header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    elf_word entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_phdr {
    uint32_t type;
#if defined(__x86_64__)
    uint32_t flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
#else
    uint32_t offset, vaddr, paddr, filesz, memsz, flags, align;
#endif
};

struct elf_dyn {
    elf_word tag, value;
};

struct module {
    char name[32];
    const struct gui_app *app;
    void (*tick)(void);
    const struct module_ext *ext;
    bool shown;
};

static struct module modules[MAX_MODULES];
static int module_count;

static int relocate(uint8_t *base, size_t span, const struct elf_dyn *dyn)
{
    elf_word at = 0, size = 0;
    for (; dyn->tag; dyn++) {
        if (dyn->tag == DT_RELOCS)
            at = dyn->value;
        else if (dyn->tag == DT_RELOCSZ)
            size = dyn->value;
    }
    if (at + size > span)
        return E_INVAL;
    const struct reloc *r = (const struct reloc *)(base + at);
    for (size_t i = 0; i < size / sizeof(*r); i++) {
        if (REL_TYPE(r[i].info) != REL_RELATIVE || r[i].offset + sizeof(elf_word) > span)
            return E_INVAL;
        /* ld marks GOT slots of the kernel's (absolute) symbols RELATIVE too: those values lie past the module, at
           the kernel's addresses (1 MiB and up), and stay as they are */
        elf_word *p = (elf_word *)(base + r[i].offset);
#if defined(__x86_64__)
        elf_word v = (elf_word)r[i].addend;
#else
        elf_word v = *p;
#endif
        *p = v > span ? v : (elf_word)(uintptr_t)base + v;
    }
    return 0;
}

static int load(const char *path, struct gui_module_entry *out)
{
    char *data;
    size_t len;
    int err = vfs_read_all(path, &data, &len);
    if (err)
        return err;
    const struct elf_header *h = (const struct elf_header *)data;
    err = E_INVAL;
    if (len < sizeof(*h) || memcmp(h->ident, "\177ELF", 4) || h->ident[4] != ELF_CLASS ||
        h->machine != ELF_MACHINE || h->type != 3 || h->phentsize != sizeof(struct elf_phdr) ||
        h->phoff + (size_t)h->phnum * sizeof(struct elf_phdr) > len)
        goto done;
    const struct elf_phdr *ph = (const struct elf_phdr *)(data + h->phoff);
    size_t span = 0;
    for (int i = 0; i < h->phnum; i++)
        if (ph[i].type == 1) {
            if (ph[i].offset + ph[i].filesz > len || ph[i].filesz > ph[i].memsz)
                goto done;
            span = MAX(span, (size_t)(ph[i].vaddr + ph[i].memsz));
        }
    if (!span || span >= (1u << 20) || h->entry + sizeof(*out) > span)
        goto done;
    uint8_t *base = kmalloc_pages(ALIGN_UP(span, 4096u));
    err = E_NOMEM;
    if (!base)
        goto done;
    const struct elf_dyn *dyn = NULL;
    for (int i = 0; i < h->phnum; i++) {
        if (ph[i].type == 1)
            memcpy(base + ph[i].vaddr, data + ph[i].offset, ph[i].filesz);
        else if (ph[i].type == 2)
            dyn = (const struct elf_dyn *)(base + ph[i].vaddr);
    }
    err = dyn ? relocate(base, span, dyn) : 0;
    if (err) {
        kfree_pages(base);
        goto done;
    }
    *out = *(const struct gui_module_entry *)(base + h->entry);
    klog("module: %s at %p", path, (void *)base);
done:
    kfree(data);
    return err;
}

static void module_name(const char *file, char *out, size_t cap)
{
    const char *base = path_basename(file);
    str_copy(out, base, cap);
    char *dot = strrchr(out, '.');
    if (dot)
        *dot = 0;
}

static struct module *find(const char *name)
{
    char key[32];
    module_name(name, key, sizeof(key));
    for (int i = 0; i < module_count; i++)
        if (!str_casecmp(modules[i].name, key))
            return &modules[i];
    return NULL;
}

static int open_module(const char *name, struct module **out)
{
    struct module *m = find(name);
    if (!m) {
        if (module_count == MAX_MODULES)
            return E_NOSPC;
        char key[32], path[PATH_MAX_LEN];
        module_name(name, key, sizeof(key));
        ksnprintf(path, sizeof(path), MODULE_DIR "/%s.sxm", key);
        struct gui_module_entry entry;
        int err = load(path, &entry);
        if (err) {
            klog("module: %s: %s", path, err_str(err));
            return err;
        }
        m = &modules[module_count++];
        str_copy(m->name, key, sizeof(m->name));
        m->app = entry.app;
        m->tick = entry.tick;
        m->ext = entry.ext;
    }
    *out = m;
    return 0;
}

int gui_module_show(const char *name, bool shown)
{
    struct module *m = find(name);
    int err = shown ? open_module(name, &m) : 0;
    if (err)
        return err;
    if (m)
        m->shown = shown;
    return 0;
}

/* every module, so their commands and decoders are known (apps stay as the App Store left them) */
void gui_module_load_all(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    struct vdir dir;
    if (vfs_opendir(MODULE_DIR, &dir))
        return;
    struct vfs_dirent e;
    while (vfs_readdir(&dir, &e) == 0) {
        const char *dot = strrchr(e.name, '.');
        struct module *m;
        if (dot && !strcmp(dot, ".sxm"))
            open_module(e.name, &m);
    }
    vfs_closedir(&dir);
}

const struct command *module_command_find(const char *name)
{
    gui_module_load_all();
    for (int i = 0; i < module_count; i++)
        for (int k = 0; modules[i].ext && k < modules[i].ext->command_count; k++) {
            const struct command *c = modules[i].ext->commands[k];
            if (!strcmp(c->name, name) || (c->aliases && str_in_list(name, c->aliases)))
                return c;
        }
    return NULL;
}

void module_command_foreach(void (*fn)(const struct command *cmd, void *ctx), void *ctx)
{
    gui_module_load_all();
    for (int i = 0; i < module_count; i++)
        for (int k = 0; modules[i].ext && k < modules[i].ext->command_count; k++)
            fn(modules[i].ext->commands[k], ctx);
}

static bool has_ext(const char *list, const char *name)
{
    const char *dot = strrchr(name, '.');
    size_t n = dot ? strlen(dot) : 0;
    for (const char *p = list; p && n && *p; p++)
        if ((p == list || p[-1] == ' ') && !str_ncasecmp(p, dot, n) && (!p[n] || p[n] == ' '))
            return true;
    return false;
}

bool module_handles(const char *name, bool audio)
{
    gui_module_load_all();
    for (int i = 0; i < module_count; i++)
        if (modules[i].ext && has_ext(audio ? modules[i].ext->audio_exts : modules[i].ext->picture_exts, name))
            return true;
    return false;
}

int module_picture_decode(const uint8_t *data, size_t size, struct picture *out)
{
    gui_module_load_all();
    for (int i = 0; i < module_count; i++)
        if (modules[i].ext && modules[i].ext->picture_decode) {
            int err = modules[i].ext->picture_decode(data, size, out);
            if (err != E_INVAL)
                return err;
        }
    return E_INVAL;
}

int module_audio_decode(const uint8_t *data, size_t size, int16_t **frames, uint32_t *count, uint32_t *rate)
{
    gui_module_load_all();
    for (int i = 0; i < module_count; i++)
        if (modules[i].ext && modules[i].ext->audio_decode) {
            int err = modules[i].ext->audio_decode(data, size, frames, count, rate);
            if (err != E_INVAL)
                return err;
        }
    return E_INVAL;
}

int module_content_search(const char *query, void (*hit)(const char *path, void *ctx), void *ctx)
{
    gui_module_load_all();
    for (int i = 0; i < module_count; i++)
        if (modules[i].ext && modules[i].ext->content_search)
            return modules[i].ext->content_search(query, hit, ctx);
    return 0;
}

bool gui_module_shown(const char *name)
{
    const struct module *m = find(name);
    return m && m->shown;
}

bool gui_module_exists(const char *name)
{
    char key[32], path[PATH_MAX_LEN];
    module_name(name, key, sizeof(key));
    ksnprintf(path, sizeof(path), MODULE_DIR "/%s.sxm", key);
    return vfs_exists(path);
}

/* the shown module apps after the built-in ones */
const struct gui_app *gui_module_app(int index)
{
    for (int i = 0; i < module_count; i++)
        if (modules[i].shown && modules[i].app && !index--)
            return modules[i].app;
    return NULL;
}

bool gui_module_owns(const struct gui_app *app)
{
    for (int i = 0; i < module_count; i++)
        if (app && modules[i].app == app)
            return true;
    return false;
}

void gui_module_tick(void)
{
    for (int i = 0; i < module_count; i++)
        if (modules[i].shown && modules[i].tick)
            modules[i].tick();
}

COMMAND(modules, NULL, "system", "modules [load|hide <name>]", "list the app modules loaded from /system/apps")
{
    if (argc == 3 && (!strcmp(argv[1], "load") || !strcmp(argv[1], "hide"))) {
        int err = gui_module_show(argv[2], argv[1][0] == 'l');
        return err ? sh_error(sh, "modules", "%s: %s", argv[2], err_str(err)) : 0;
    }
    if (argc != 1)
        return sh_error(sh, "modules", "usage: modules [load|hide <name>]");
    for (int i = 0; i < module_count; i++)
        sh_printf(sh, "%-12s %-20s %s\n", modules[i].name, modules[i].app ? modules[i].app->name : "-",
                  !modules[i].app ? "extension" : modules[i].shown ? "shown" : "hidden");
    return 0;
}

#endif
