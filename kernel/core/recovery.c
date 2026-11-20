#include <synth/block.h>
#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/parttab.h>
#include <synth/recovery.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

/* The UEFI loader counts starts in \SYNTHOS\BOOT.TRY ("tries=N nonce=XXXXXXXX") and picks safe mode after two that
   did not get far. Once the desktop or the shell is up, the counter goes back to 0 here; the nonce tells which EFI
   partition this start came from. */
#define TRY_LEN 23

static bool safe;
static bool confirmed;

bool safe_mode(void)
{
    return safe;
}

void recovery_init(const char *cmdline)
{
    safe = hal_safe_mode();
    for (const char *p = cmdline; p && (p = strstr(p, "safe")); p += 4)
        if ((p == cmdline || p[-1] == ' ') && (p[4] == 0 || p[4] == ' '))
            safe = true;
    if (safe)
        klog("recovery: safe mode (basic drivers, one processor, no autostart)");
}

/* Looks at \SYNTHOS\BOOT.TRY on every EFI partition: the one with this start's nonce is the loader's own. The
   tries value found goes to *found; tries >= 0 is written back in place (the file keeps its cluster, so a power cut
   cannot leave the FAT half changed). */
static int visit(const char *root, uint32_t nonce, int tries, int *found)
{
    char path[PATH_MAX_LEN], text[TRY_LEN + 1], want[9];
    ksnprintf(want, sizeof(want), "%08x", nonce);
    ksnprintf(path, sizeof(path), "%s/SYNTHOS/BOOT.TRY", root);
    struct file *f;
    if (vfs_open(path, VFS_READ, &f))
        return E_NOENT;
    long n = vfs_read(f, text, TRY_LEN);
    vfs_close(f);
    if (n != TRY_LEN || memcmp(text, "tries=", 6) || memcmp(text + 14, want, 8))
        return E_NOENT;
    *found = text[6] >= '0' && text[6] <= '9' ? text[6] - '0' : 0;
    if (tries < 0)
        return 0;
    ksnprintf(text, sizeof(text), "tries=%d nonce=%s\n", MIN(tries, 9), want);
    int err = vfs_open(path, VFS_WRITE, &f);
    if (err)
        return err;
    n = vfs_write(f, text, TRY_LEN);
    vfs_close(f);
    return n == TRY_LEN ? 0 : n < 0 ? (int)n : E_IO;
}

static const char *mounted_at(struct blockdev *dev)
{
    for (struct mount *m = vfs_mounts(); m; m = m->next)
        if (!strcmp(m->source, dev->name))
            return m->path;
    return NULL;
}

typedef int (*esp_fn)(const char *root, void *ctx);

/* Calls fn for each EFI partition until one answers other than E_NOENT; mounts the ones nobody has mounted. */
static int each_esp(esp_fn fn, void *ctx)
{
    struct disk_map *map = kmalloc(sizeof(*map)); /* from the heap: the 32-bit kernel's image has no room */
    if (!map)
        return E_NOMEM;
    int err = E_NOENT;
    for (struct blockdev *disk = block_list(); disk && err == E_NOENT; disk = disk->next) {
        if (disk->parent || disk_map_read(disk, map))
            continue;
        for (int i = 0; i < map->count && err == E_NOENT; i++) {
            if (map->parts[i].kind != PART_EFI)
                continue;
            struct blockdev *esp = block_add_partition(disk, "recesp", map->parts[i].start, map->parts[i].count);
            if (!esp)
                continue;
            const char *root = mounted_at(esp);
            bool own = false;
            if (!root && vfs_mkdirs("/mnt/recesp") == 0 && vfs_mount("fat", esp->name, "/mnt/recesp") == 0) {
                root = "/mnt/recesp";
                own = true;
            }
            if (root)
                err = fn(root, ctx);
            if (own) {
                vfs_umount("/mnt/recesp");
                vfs_unlink("/mnt/recesp");
            }
            block_unregister(esp);
        }
    }
    kfree(map);
    return err;
}

struct counter {
    uint32_t nonce;
    int tries, *found;
    esp_fn then;
    void *ctx;
};

static int counter_visit(const char *root, void *ctx)
{
    struct counter *c = ctx;
    int found = 0, err = visit(root, c->nonce, c->tries, &found);
    if (!err && c->found)
        *c->found = found;
    return err || !c->then ? err : c->then(root, c->ctx);
}

/* fn runs on the EFI partition this start came from (its BOOT.TRY carries this start's nonce) */
static int own_esp(int tries, int *found, esp_fn fn, void *ctx)
{
    uint32_t nonce = hal_boot_nonce();
    if (!nonce)
        return E_NOENT;
    struct counter c = {nonce, tries, found, fn, ctx};
    return each_esp(counter_visit, &c);
}

int recovery_own_esp(esp_fn fn, void *ctx)
{
    return own_esp(-1, NULL, fn, ctx);
}

static int try_counter(int tries, int *found)
{
    return own_esp(tries, found, NULL, NULL);
}

static int write_tries(int tries)
{
    int found;
    return try_counter(tries, &found);
}

void recovery_boot_ok(void)
{
    if (confirmed)
        return;
    confirmed = true;
    int err = write_tries(0);
    if (err && err != E_NOENT)
        klog("recovery: cannot reset the start counter: %s", err_str(err));
}

/* Keys of SYSTEM.CFG the UEFI boot menu reads: default= (the system it starts by itself), menu= (auto, always,
   never), timeout= (seconds) and hide= (words of systems it leaves out). A value equal to the fallback is removed. */
struct boot_key {
    const char *key, *fallback, *set;
    char *out;
    size_t cap;
};

static int boot_key_visit(const char *root, void *ctx)
{
    struct boot_key *b = ctx;
    char path[PATH_MAX_LEN];
    ksnprintf(path, sizeof(path), "%s/SYNTHOS/SYSTEM.CFG", root);
    struct file *f;
    int err = vfs_open(path, VFS_READ, &f);
    if (err)
        return err;
    char text[1024], next[1100];
    long n = vfs_read(f, text, sizeof(text) - 1);
    vfs_close(f);
    if (n < 0)
        return (int)n;
    text[n] = 0;
    size_t len = 0, klen = strlen(b->key);
    if (b->out)
        str_copy(b->out, b->fallback, b->cap);
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t l = end ? (size_t)(end - line) + 1 : strlen(line);
        if (!strncmp(line, b->key, klen) && line[klen] == '=') {
            if (b->out) {
                size_t k = 0;
                for (const char *v = line + klen + 1; k + 1 < b->cap && *v && *v != '\n' && *v != '\r'; v++)
                    b->out[k++] = *v;
                b->out[k] = 0;
            }
        } else if (len + l < sizeof(next)) {
            memcpy(next + len, line, l);
            len += l;
        }
        line += l;
    }
    if (!b->set)
        return 0;
    if (len && next[len - 1] != '\n')
        next[len++] = '\n';
    if (strcmp(b->set, b->fallback))
        len += (size_t)ksnprintf(next + len, sizeof(next) - len, "%s=%s\n", b->key, b->set);
    if ((err = vfs_open(path, VFS_WRITE | VFS_TRUNC, &f)))
        return err;
    n = vfs_write(f, next, len);
    vfs_close(f);
    return n == (long)len ? 0 : n < 0 ? (int)n : E_IO;
}

static void boot_key_get(const char *key, const char *fallback, char *buf, size_t cap)
{
    struct boot_key b = {key, fallback, NULL, buf, cap};
    if (own_esp(-1, NULL, boot_key_visit, &b))
        str_copy(buf, fallback, cap);
}

/* Small letters and digits; with `list`, words separated by commas. */
static int boot_key_set(const char *key, const char *fallback, const char *value, bool list)
{
    char v[96];
    size_t k = 0;
    for (; value[k] && k + 1 < sizeof(v); k++) {
        char c = value[k];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (list && c == ',' && k && value[k + 1])))
            return E_INVAL;
        v[k] = c;
    }
    v[k] = 0;
    if (!k || value[k])
        return E_INVAL;
    struct boot_key b = {key, fallback, v, NULL, 0};
    return own_esp(-1, NULL, boot_key_visit, &b);
}

static void get_bootdefault(char *buf, size_t cap)
{
    boot_key_get("default", "synthos", buf, cap);
}

static int set_bootdefault(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return boot_key_set("default", "synthos", value, false);
}

static const struct setting s_bootdefault = {"bootdefault", "synthos|windows|other|<word of a name>",
                                             "the system the boot menu starts by itself (UEFI starts)",
                                             get_bootdefault, set_bootdefault};
SETTING(s_bootdefault);

static void get_bootmenu(char *buf, size_t cap)
{
    boot_key_get("menu", "auto", buf, cap);
}

static int set_bootmenu(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "auto") && strcmp(value, "always") && strcmp(value, "never"))
        return E_INVAL;
    return boot_key_set("menu", "auto", value, false);
}

static const struct setting s_bootmenu = {"bootmenu", "auto|always|never",
                                          "when the boot menu shows (auto: when other systems are found)",
                                          get_bootmenu, set_bootmenu};
SETTING(s_bootmenu);

static void get_boottimeout(char *buf, size_t cap)
{
    boot_key_get("timeout", "3", buf, cap);
}

static int set_boottimeout(struct shell *sh, const char *value)
{
    UNUSED(sh);
    const char *end;
    long v = str_to_long(value, &end, 10);
    if (!*value || *end || v < 0 || v > 60)
        return E_INVAL;
    return boot_key_set("timeout", "3", value, false);
}

static const struct setting s_boottimeout = {"boottimeout", "<seconds 0-60>",
                                             "how long the boot menu waits before it starts the default system",
                                             get_boottimeout, set_boottimeout};
SETTING(s_boottimeout);

static void get_boothide(char *buf, size_t cap)
{
    boot_key_get("hide", "none", buf, cap);
}

static int set_boothide(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return boot_key_set("hide", "none", value, true);
}

static const struct setting s_boothide = {"boothide", "none|<word>,<word>,...",
                                          "systems the boot menu leaves out (a word of their name)", get_boothide,
                                          set_boothide};
SETTING(s_boothide);

struct systems_read {
    char (*names)[BOOT_NAME_MAX];
    int max, n;
};

static int systems_visit(const char *root, void *ctx)
{
    struct systems_read *r = ctx;
    char path[PATH_MAX_LEN], text[1024];
    ksnprintf(path, sizeof(path), "%s/SYNTHOS/SYSTEMS.TXT", root);
    struct file *f;
    int err = vfs_open(path, VFS_READ, &f);
    if (err)
        return err;
    long n = vfs_read(f, text, sizeof(text) - 1);
    vfs_close(f);
    if (n < 0)
        return (int)n;
    text[n] = 0;
    for (char *line = text; *line && r->n < r->max;) {
        char *end = strchr(line, '\n');
        size_t l = end ? (size_t)(end - line) : strlen(line);
        if (l) {
            str_copy(r->names[r->n], line, MIN(l + 1, (size_t)BOOT_NAME_MAX));
            r->n++;
        }
        line += l + (end ? 1 : 0);
    }
    return 0;
}

int boot_systems(char (*names)[BOOT_NAME_MAX], int max)
{
    struct systems_read r = {names, max, 0};
    own_esp(-1, NULL, systems_visit, &r);
    return r.n;
}

/* Whether a word of hide= (or default=) is part of the name, in any case. */
bool boot_name_has(const char *name, const char *words)
{
    for (const char *p = words; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        for (const char *q = name; len && *q; q++) {
            size_t k = 0;
            while (k < len && to_lower(q[k]) == p[k])
                k++;
            if (k == len)
                return true;
        }
        p += len + (e ? 1 : 0);
    }
    return false;
}

COMMAND(bootmenu, NULL, "system", "bootmenu", "the UEFI boot menu: when it shows, what it starts and its systems")
{
    UNUSED(argc);
    UNUSED(argv);
    char menu[16], timeout[8], def[32], hide[96];
    get_bootmenu(menu, sizeof(menu));
    get_boottimeout(timeout, sizeof(timeout));
    get_bootdefault(def, sizeof(def));
    get_boothide(hide, sizeof(hide));
    sh_printf(sh, "Shows: %s, waits %s s, starts %s by itself\n",
              !strcmp(menu, "auto") ? "when other systems are found" : menu, timeout, def);
    if (!hal_boot_nonce()) {
        sh_puts(sh, "This start did not come from the SynthOS UEFI loader: the menu settings cannot be read here.\n");
        return 0;
    }
    char names[8][BOOT_NAME_MAX];
    int n = boot_systems(names, 8);
    sh_printf(sh, "  1  SynthOS\n  2  SynthOS (safe mode)\n");
    for (int i = 0; i < n; i++)
        sh_printf(sh, "  %d  %s%s\n", i + 3, names[i],
                  strcmp(hide, "none") && boot_name_has(names[i], hide) ? "  (hidden)" : "");
    sh_puts(sh, "Change with: config bootmenu|boottimeout|bootdefault|boothide <value>\n");
    return 0;
}

COMMAND(recovery, "safemode", "system", "recovery [safe|normal]",
        "safe mode state; 'safe' makes the next start use safe mode, 'normal' a normal one")
{
    if (argc > 1) {
        int tries = !strcmp(argv[1], "safe") ? 2 : !strcmp(argv[1], "normal") ? 0 : -1;
        if (tries < 0) {
            sh_printf(sh, "usage: recovery [safe|normal]\n");
            return 1;
        }
        int err = write_tries(tries);
        if (err) {
            sh_printf(sh, "recovery: %s\n", err == E_NOENT ? "this start did not come from the UEFI loader" : err_str(err));
            return 1;
        }
        sh_printf(sh, "The next start is %s.\n", tries ? "in safe mode" : "a normal one");
        return 0;
    }
    int now = 0;
    sh_printf(sh, "Safe mode: %s\n", safe ? "on (basic drivers, one processor, no autostart)" : "off");
    if (try_counter(-1, &now) == 0)
        sh_printf(sh, "Start counter: %d (0 once a start gets to the desktop or the shell)\n", now);
    else
        sh_printf(sh, "Start counter: none (BIOS start)\n");
    return 0;
}

static volatile uintptr_t nowhere = 0x10;

static void bad_access(void *ctx)
{
    UNUSED(ctx);
    *(volatile uint32_t *)nowhere = 1;
}

/* placed with the desktop apps' code, so the hang watchdog takes it for a stuck app */
__attribute__((section(".text.apps"), noinline)) static void endless_loop(void *ctx)
{
    volatile uint32_t *n = ctx;
    for (;;)
        (*n)++;
}

COMMAND(faulttest, NULL, "system", "faulttest [hang]",
        "check that a crash (or an endless loop) in desktop app code closes only that app")
{
    if (argc > 1 && !strcmp(argv[1], "hang")) {
        volatile uint32_t spins = 0;
        uint64_t t0 = hal_ticks_ms();
        int r = hal_guard(endless_loop, (void *)&spins);
        sh_printf(sh, r == HAL_GUARD_HUNG ? "contained: the stuck app was stopped after %u s\n" : "not stopped\n",
                  (unsigned)((hal_ticks_ms() - t0) / 1000));
        return r == HAL_GUARD_HUNG ? 0 : 1;
    }
    int vector = hal_guard(bad_access, sh);
    sh_printf(sh, vector ? "contained: CPU exception %d, the system goes on\n" : "no fault\n", vector);
    return vector ? 0 : 1;
}

struct par_check {
    uint32_t *out;
};

static void par_fill(void *ctx, int from, int to)
{
    const struct par_check *p = ctx;
    for (int i = from; i < to; i++)
        p->out[i] += (uint32_t)i * 2654435761u + 1;
}

COMMAND(parcheck, NULL, "system", "parcheck", "check that work shared between the processors is done once")
{
    UNUSED(argc);
    UNUSED(argv);
    enum { N = 200000 };
    uint32_t *out = kzalloc(N * sizeof(uint32_t));
    if (!out)
        return sh_error(sh, "parcheck", "out of memory");
    struct par_check p = {out};
    int bad = 0;
    for (int round = 0; round < 50 && !bad; round++) {
        hal_parallel(par_fill, &p, N, 1000 + round * 37);
        for (int i = 0; i < N; i++)
            if (out[i] != ((uint32_t)i * 2654435761u + 1) * (uint32_t)(round + 1)) {
                bad = i + 1;
                break;
            }
    }
    kfree(out);
    if (bad)
        return sh_error(sh, "parcheck", "part %d was lost or done twice", bad - 1);
    sh_printf(sh, "parallel work: ok on %d processor%s\n", hal_parallel_cpus(), hal_parallel_cpus() == 1 ? "" : "s");
    return 0;
}
