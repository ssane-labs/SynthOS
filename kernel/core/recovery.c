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
    static struct disk_map map;
    int err = E_NOENT;
    for (struct blockdev *disk = block_list(); disk && err == E_NOENT; disk = disk->next) {
        if (disk->parent || disk_map_read(disk, &map))
            continue;
        for (int i = 0; i < map.count && err == E_NOENT; i++) {
            if (map.parts[i].kind != PART_EFI)
                continue;
            struct blockdev *esp = block_add_partition(disk, "recesp", map.parts[i].start, map.parts[i].count);
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

/* default= in SYSTEM.CFG: which system the UEFI boot menu starts when nobody picks one */
struct boot_default {
    const char *set;
    char *out;
    size_t cap;
};

static int boot_default_visit(const char *root, void *ctx)
{
    struct boot_default *b = ctx;
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
    size_t len = 0;
    if (b->out)
        str_copy(b->out, "synthos", b->cap);
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        size_t l = end ? (size_t)(end - line) + 1 : strlen(line);
        if (!strncmp(line, "default=", 8)) {
            if (b->out) {
                size_t k = 0;
                for (const char *v = line + 8; k + 1 < b->cap && *v && *v != '\n' && *v != '\r'; v++)
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
    if (strcmp(b->set, "synthos"))
        len += (size_t)ksnprintf(next + len, sizeof(next) - len, "default=%s\n", b->set);
    if ((err = vfs_open(path, VFS_WRITE | VFS_TRUNC, &f)))
        return err;
    n = vfs_write(f, next, len);
    vfs_close(f);
    return n == (long)len ? 0 : n < 0 ? (int)n : E_IO;
}

static void get_bootdefault(char *buf, size_t cap)
{
    struct boot_default b = {NULL, buf, cap};
    if (own_esp(-1, NULL, boot_default_visit, &b))
        str_copy(buf, "synthos", cap);
}

static int set_bootdefault(struct shell *sh, const char *value)
{
    UNUSED(sh);
    char v[32];
    size_t k = 0;
    for (; value[k] && k + 1 < sizeof(v); k++) {
        char c = value[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return E_INVAL;
        v[k] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    v[k] = 0;
    if (!k)
        return E_INVAL;
    struct boot_default b = {v, NULL, 0};
    return own_esp(-1, NULL, boot_default_visit, &b);
}

static const struct setting s_bootdefault = {"bootdefault", "synthos|windows|other|<word of a name>",
                                             "the system the boot menu starts by itself (UEFI starts)",
                                             get_bootdefault, set_bootdefault};
SETTING(s_bootdefault);

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
