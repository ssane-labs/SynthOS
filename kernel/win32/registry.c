#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

#include "win32.h"

/* The registry: one tree shared by every process, the machine part (HKLM) and each user's part (HKCU) kept on the
   persistent disk. Installers record what they installed here, browsers register themselves and their protocols,
   and programs keep their settings; everything a Windows program writes, it can read back. Names are UTF-8 and
   compared without case; string data is kept as UTF-16, as programs store it. */

#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7
#define REG_QWORD 11
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_KEY_DELETED 1018
#define ERROR_UNSUPPORTED_TYPE 1630
#define ERROR_BADKEY 1010
#define KEY_ROOT_CLASSES 0x80000000u
#define KEY_ROOT_USER 0x80000001u
#define KEY_ROOT_MACHINE 0x80000002u
#define KEY_ROOT_USERS 0x80000003u
#define KEY_ROOT_CONFIG 0x80000005u
#define KEY_ROOT_LOCAL 0x80000007u
#define CP_UTF8 65001
#define USER_SID "S-1-5-21-1000-2000-3000-1001"
#define MACHINE_FILE "/home/programdata/synthos/registry/machine.reg"

struct reg_value {
    char *name;
    uint32_t type, size;
    uint8_t *data;
    struct reg_value *next;
};

struct reg_key {
    char *name;
    struct reg_key *parent, *child, *next;
    struct reg_value *values;
    int refs;
    bool deleted;
    uint64_t written;
};

struct hive {
    struct reg_key *root;
    char file[PATH_MAX_LEN];
    char owner[USER_NAME_MAX];
    bool loaded, dirty;
};

static struct hive machine, user;
/* reading a hive back from disk is not a change worth writing again */
static bool loading;

static uint64_t filetime_now(void)
{
    return hal_ticks_ms() * 10000ull + 132000000000000000ull;
}

static struct reg_key *key_new(const char *name, struct reg_key *parent)
{
    struct reg_key *k = kzalloc(sizeof(*k));
    if (!k || !(k->name = kstrdup(name))) {
        kfree(k);
        return NULL;
    }
    k->parent = parent;
    k->written = filetime_now();
    if (parent) {
        struct reg_key **at = &parent->child;
        while (*at && str_casecmp((*at)->name, name) < 0)
            at = &(*at)->next;
        k->next = *at;
        *at = k;
    }
    return k;
}

static void value_free(struct reg_value *v)
{
    kfree(v->name);
    kfree(v->data);
    kfree(v);
}

/* a key goes when nothing refers to it any more; one deleted while open stays until its last handle closes */
static void key_destroy(struct reg_key *k)
{
    while (k->child) {
        struct reg_key *c = k->child;
        k->child = c->next;
        c->parent = NULL;
        c->deleted = true;
        if (!c->refs)
            key_destroy(c);
    }
    while (k->values) {
        struct reg_value *v = k->values;
        k->values = v->next;
        value_free(v);
    }
    kfree(k->name);
    kfree(k);
}

static struct hive *hive_of(struct reg_key *k)
{
    while (k && k->parent)
        k = k->parent;
    return k == user.root ? &user : &machine;
}

static void touch(struct reg_key *k)
{
    k->written = filetime_now();
    if (!loading)
        hive_of(k)->dirty = true;
}

static struct reg_key *find_child(struct reg_key *k, const char *name, size_t n)
{
    for (struct reg_key *c = k->child; c; c = c->next)
        if (strlen(c->name) == n && !str_ncasecmp(c->name, name, n))
            return c;
    return NULL;
}

/* 32-bit programs on 64-bit Windows see Software through WOW6432Node: here both views are one */
static bool skipped(const char *name, size_t n)
{
    return n == 11 && !str_ncasecmp(name, "WOW6432Node", 11);
}

static struct reg_key *walk(struct reg_key *k, const char *path, bool create, bool *created)
{
    while (k && path && *path) {
        while (*path == '\\')
            path++;
        if (!*path)
            break;
        const char *end = strchr(path, '\\');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        if (n > 255)
            return NULL;
        if (!skipped(path, n)) {
            struct reg_key *c = find_child(k, path, n);
            if (!c && create) {
                char name[256];
                memcpy(name, path, n);
                name[n] = 0;
                if ((c = key_new(name, k))) {
                    touch(k);
                    if (created)
                        *created = true;
                }
            }
            k = c;
        }
        path += n;
    }
    return k;
}

static struct reg_value *find_value(struct reg_key *k, const char *name)
{
    for (struct reg_value *v = k->values; v; v = v->next)
        if (!str_casecmp(v->name, name ? name : ""))
            return v;
    return NULL;
}

static int set_value(struct reg_key *k, const char *name, uint32_t type, const void *data, uint32_t size)
{
    struct reg_value *v = find_value(k, name);
    uint8_t *copy = kmalloc(MAX(size, 1u));
    if (!copy)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (size)
        memcpy(copy, data, size);
    if (!v) {
        if (!(v = kzalloc(sizeof(*v))) || !(v->name = kstrdup(name ? name : ""))) {
            kfree(v);
            kfree(copy);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        struct reg_value **at = &k->values;
        while (*at)
            at = &(*at)->next;
        *at = v;
    }
    kfree(v->data);
    v->data = copy;
    v->type = type;
    v->size = size;
    touch(k);
    return 0;
}

static bool delete_value(struct reg_key *k, const char *name)
{
    for (struct reg_value **at = &k->values; *at; at = &(*at)->next)
        if (!str_casecmp((*at)->name, name ? name : "")) {
            struct reg_value *v = *at;
            *at = v->next;
            value_free(v);
            touch(k);
            return true;
        }
    return false;
}

static void unlink_key(struct reg_key *k)
{
    struct reg_key *p = k->parent;
    if (!p)
        return;
    for (struct reg_key **at = &p->child; *at; at = &(*at)->next)
        if (*at == k) {
            *at = k->next;
            break;
        }
    touch(p);
    k->parent = NULL;
    k->deleted = true;
    if (!k->refs)
        key_destroy(k);
}

/* Persistence: a text file per hive, "[path]" lines for keys and name=type:hex lines for values. */

struct buf {
    char *data;
    size_t len, cap;
};

static void put(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->len + n + 4096);
        char *grown = krealloc(b->data, cap);
        if (!grown)
            return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void puts_(struct buf *b, const char *s)
{
    put(b, s, strlen(s));
}

static void put_escaped(struct buf *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            put(b, "\\", 1);
        put(b, s, 1);
    }
}

static void key_path(struct reg_key *k, char *out, size_t cap)
{
    const char *parts[64];
    int n = 0;
    for (; k && k->parent && n < 64; k = k->parent)
        parts[n++] = k->name;
    out[0] = 0;
    while (n-- > 0) {
        if (out[0])
            str_append(out, "\\", cap);
        str_append(out, parts[n], cap);
    }
}

static void save_key(struct buf *b, struct reg_key *k)
{
    if (k->parent && (k->values || !k->child)) {
        char path[1024];
        key_path(k, path, sizeof(path));
        puts_(b, "[");
        puts_(b, path);
        puts_(b, "]\n");
        for (struct reg_value *v = k->values; v; v = v->next) {
            char head[16];
            puts_(b, "\"");
            put_escaped(b, v->name);
            ksnprintf(head, sizeof(head), "\"=%x:", v->type);
            puts_(b, head);
            for (uint32_t i = 0; i < v->size; i++) {
                char hx[3];
                ksnprintf(hx, sizeof(hx), "%02x", v->data[i]);
                put(b, hx, 2);
            }
            puts_(b, "\n");
        }
    }
    for (struct reg_key *c = k->child; c; c = c->next)
        save_key(b, c);
}

static void hive_save(struct hive *h)
{
    if (!h->loaded || !h->dirty || !h->file[0])
        return;
    struct buf b = {0};
    puts_(&b, "SYNTHOS REGISTRY 1\n");
    save_key(&b, h->root);
    char dir[PATH_MAX_LEN];
    str_copy(dir, h->file, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        vfs_mkdirs(dir);
    }
    if (b.data && vfs_write_all(h->file, b.data, b.len) == 0)
        h->dirty = false;
    kfree(b.data);
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
}

static void hive_parse(struct hive *h, char *text)
{
    struct reg_key *k = NULL;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r')
            line[--n] = 0;
        if (line[0] == '[' && n > 1 && line[n - 1] == ']') {
            line[n - 1] = 0;
            k = walk(h->root, line + 1, true, NULL);
            continue;
        }
        if (!k || line[0] != '"')
            continue;
        char name[1024];
        size_t o = 0;
        char *p = line + 1;
        for (; *p && *p != '"' && o + 1 < sizeof(name); p++) {
            if (*p == '\\' && p[1])
                p++;
            name[o++] = *p;
        }
        name[o] = 0;
        if (p[0] != '"' || p[1] != '=')
            continue;
        p += 2;
        uint32_t type = 0;
        for (; hexval(*p) >= 0; p++)
            type = type * 16 + (uint32_t)hexval(*p);
        if (*p++ != ':')
            continue;
        size_t len = strlen(p) / 2;
        uint8_t *data = kmalloc(MAX(len, (size_t)1));
        if (!data)
            continue;
        for (size_t i = 0; i < len; i++)
            data[i] = (uint8_t)(hexval(p[2 * i]) << 4 | hexval(p[2 * i + 1]));
        set_value(k, name, type, data, (uint32_t)len);
        kfree(data);
    }
}

static void set_string(struct reg_key *k, const char *name, uint32_t type, const char *text, bool keep)
{
    if (keep && find_value(k, name))
        return;
    size_t n = strlen(text);
    WCHAR *w = kmalloc((n + 1) * sizeof(WCHAR));
    if (!w)
        return;
    size_t wn = w32_cp_to_wide(text, (int)n, w, n, CP_UTF8);
    w[wn] = 0;
    set_value(k, name, type, w, (uint32_t)(wn + 1) * 2);
    kfree(w);
}

static void set_dword(struct reg_key *k, const char *name, uint32_t v)
{
    if (!find_value(k, name))
        set_value(k, name, REG_DWORD, &v, 4);
}

/* what a fresh Windows 10 has and programs look for; missing values come back on every start */
static void seed_machine(struct reg_key *m)
{
    struct reg_key *nt = walk(m, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", true, NULL);
    if (nt) {
        set_string(nt, "ProductName", REG_SZ, "Windows 10 Pro", true);
        set_string(nt, "EditionID", REG_SZ, "Professional", true);
        set_string(nt, "InstallationType", REG_SZ, "Client", true);
        set_string(nt, "CurrentVersion", REG_SZ, "6.3", true);
        set_string(nt, "CurrentBuild", REG_SZ, "19045", true);
        set_string(nt, "CurrentBuildNumber", REG_SZ, "19045", true);
        set_string(nt, "ReleaseId", REG_SZ, "2009", true);
        set_string(nt, "DisplayVersion", REG_SZ, "22H2", true);
        set_string(nt, "RegisteredOwner", REG_SZ, "SynthOS", true);
        set_string(nt, "SystemRoot", REG_SZ, "C:\\Windows", true);
        set_dword(nt, "CurrentMajorVersionNumber", 10);
        set_dword(nt, "CurrentMinorVersionNumber", 0);
        set_dword(nt, "UBR", 3448);
    }
    struct reg_key *fonts = walk(m, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", true, NULL);
    if (fonts)
        set_string(fonts, "DejaVu Sans (TrueType)", REG_SZ, "DejaVuSans.ttf", true);
    struct reg_key *cv = walk(m, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", true, NULL);
    if (cv) {
        set_string(cv, "ProgramFilesDir", REG_SZ, "C:\\Program Files", true);
        set_string(cv, "ProgramFilesDir (x86)", REG_SZ, "C:\\Program Files (x86)", true);
        set_string(cv, "ProgramW6432Dir", REG_SZ, "C:\\Program Files", true);
        set_string(cv, "CommonFilesDir", REG_SZ, "C:\\Program Files\\Common Files", true);
        set_string(cv, "ProgramFilesPath", REG_EXPAND_SZ, "%ProgramFiles%", true);
    }
    struct reg_key *sf = walk(m, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", true, NULL);
    if (sf) {
        set_string(sf, "Common Desktop", REG_SZ, "C:\\Users\\Public\\Desktop", true);
        set_string(sf, "Common Programs", REG_SZ, "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs", true);
        set_string(sf, "Common Start Menu", REG_SZ, "C:\\ProgramData\\Microsoft\\Windows\\Start Menu", true);
        set_string(sf, "Common Startup", REG_SZ,
                   "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", true);
        set_string(sf, "Common AppData", REG_SZ, "C:\\ProgramData", true);
    }
    walk(m, "SOFTWARE\\Classes", true, NULL);
    walk(m, "SOFTWARE\\Policies\\Microsoft", true, NULL);
    walk(m, "SOFTWARE\\Clients\\StartMenuInternet", true, NULL);
    walk(m, "SOFTWARE\\RegisteredApplications", true, NULL);
    walk(m, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", true, NULL);
    walk(m, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths", true, NULL);
    struct reg_key *env = walk(m, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", true, NULL);
    if (env) {
        set_string(env, "OS", REG_SZ, "Windows_NT", true);
        set_string(env, "windir", REG_EXPAND_SZ, "%SystemRoot%", true);
    }
}

static void seed_user(struct reg_key *u, const char *name)
{
    char profile[96];
    ksnprintf(profile, sizeof(profile), "C:\\Users\\%s", name);
    static const char *const folders[][2] = {
        {"Desktop", "\\Desktop"},
        {"Personal", "\\Documents"},
        {"AppData", "\\AppData\\Roaming"},
        {"Local AppData", "\\AppData\\Local"},
        {"Programs", "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs"},
        {"Start Menu", "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu"},
        {"Startup", "\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"},
        {"My Pictures", "\\Pictures"},
        {"My Music", "\\Music"},
        {"My Video", "\\Videos"},
        {"{374DE290-123F-4565-9164-39C4925E467B}", "\\Downloads"},
    };
    struct reg_key *sf = walk(u, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", true, NULL);
    struct reg_key *usf =
        walk(u, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", true, NULL);
    for (size_t i = 0; i < ARRAY_SIZE(folders); i++) {
        char path[192];
        ksnprintf(path, sizeof(path), "%s%s", profile, folders[i][1]);
        if (sf)
            set_string(sf, folders[i][0], REG_SZ, path, true);
        ksnprintf(path, sizeof(path), "%%USERPROFILE%%%s", folders[i][1]);
        if (usf)
            set_string(usf, folders[i][0], REG_EXPAND_SZ, path, true);
    }
    struct reg_key *intl = walk(u, "Control Panel\\International", true, NULL);
    if (intl) {
        set_string(intl, "LocaleName", REG_SZ, "en-US", true);
        set_string(intl, "sDecimal", REG_SZ, ".", true);
        set_string(intl, "sShortDate", REG_SZ, "M/d/yyyy", true);
    }
    struct reg_key *env = walk(u, "Environment", true, NULL);
    if (env) {
        set_string(env, "TEMP", REG_EXPAND_SZ, "%USERPROFILE%\\AppData\\Local\\Temp", true);
        set_string(env, "TMP", REG_EXPAND_SZ, "%USERPROFILE%\\AppData\\Local\\Temp", true);
    }
    walk(u, "Software\\Classes", true, NULL);
    walk(u, "Software\\Policies\\Microsoft", true, NULL);
    walk(u, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", true, NULL);
}

static void hive_load(struct hive *h, const char *file, void (*seed)(struct reg_key *, const char *), const char *who)
{
    if (!h->root && !(h->root = key_new(h == &machine ? "MACHINE" : "USER", NULL)))
        return;
    str_copy(h->file, file, sizeof(h->file));
    str_copy(h->owner, who, sizeof(h->owner));
    char *text = NULL;
    size_t size = 0;
    loading = true;
    if (vfs_read_all(file, &text, &size) == 0) {
        char *z = krealloc(text, size + 1);
        if (z) {
            z[size] = 0;
            hive_parse(h, z);
            text = z;
        }
        kfree(text);
    }
    loading = false;
    h->loaded = true;
    bool before = h->dirty;
    seed(h->root, who);
    h->dirty = before || h->dirty;
}

static void seed_machine_adapter(struct reg_key *m, const char *who)
{
    UNUSED(who);
    seed_machine(m);
}

static const char *current_user(void)
{
    const struct user *u = user_current();
    return u && u->name[0] ? u->name : "root";
}

/* the machine hive once; the user hive again whenever another user is logged in */
static void ensure_loaded(void)
{
    if (!machine.loaded)
        hive_load(&machine, MACHINE_FILE, seed_machine_adapter, "");
    const char *who = current_user();
    if (user.loaded && !strcmp(user.owner, who))
        return;
    if (user.loaded) {
        hive_save(&user);
        key_destroy(user.root);
        user = (struct hive){0};
    }
    char file[PATH_MAX_LEN];
    ksnprintf(file, sizeof(file), "/home/programdata/synthos/registry/user-%s.reg", who);
    hive_load(&user, file, seed_user, who);
}

void w32_registry_flush(void)
{
    hive_save(&machine);
    hive_save(&user);
}

/* Handles: predefined roots are pseudo-handles; opened keys are handle objects holding a reference. */

static bool is_classes(HANDLE h)
{
    return (uint32_t)(uintptr_t)h == KEY_ROOT_CLASSES;
}

static struct reg_key *key_of(HANDLE h, LONG *err)
{
    ensure_loaded();
    *err = 0;
    switch ((uint32_t)(uintptr_t)h) {
    case KEY_ROOT_USER:
    case KEY_ROOT_LOCAL: return user.root;
    case KEY_ROOT_MACHINE: return machine.root;
    case KEY_ROOT_CONFIG:
        return walk(machine.root, "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current", true, NULL);
    case KEY_ROOT_CLASSES: return walk(machine.root, "SOFTWARE\\Classes", true, NULL);
    default: break;
    }
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_KEY || !o->key) {
        *err = ERROR_INVALID_HANDLE;
        return NULL;
    }
    if (o->key->deleted) {
        *err = ERROR_KEY_DELETED;
        return NULL;
    }
    return o->key;
}

void w32_reg_key_ref(struct reg_key *k)
{
    if (k)
        k->refs++;
}

void w32_reg_key_unref(struct reg_key *k)
{
    if (k && --k->refs <= 0 && k->deleted)
        key_destroy(k);
}

static HANDLE new_handle(struct reg_key *k)
{
    HANDLE h = w32_handle_new(WH_KEY);
    struct w32_object *o = w32_handle_get(h);
    if (!o)
        return NULL;
    o->key = k;
    k->refs++;
    return h;
}

/* HKEY_USERS: the user's SID (and .DEFAULT) lead to the user's hive */
static struct reg_key *users_path(const char *path, const char **rest)
{
    while (*path == '\\')
        path++;
    const char *end = strchr(path, '\\');
    size_t n = end ? (size_t)(end - path) : strlen(path);
    if ((n == strlen(USER_SID) && !str_ncasecmp(path, USER_SID, n)) || (n == 8 && !str_ncasecmp(path, ".DEFAULT", 8))) {
        *rest = path + n;
        return user.root;
    }
    return NULL;
}

/* HKEY_CLASSES_ROOT is the user's classes over the machine's */
static struct reg_key *open_path(HANDLE base, const char *path, bool create, bool *created, LONG *err)
{
    ensure_loaded();
    if ((uint32_t)(uintptr_t)base == KEY_ROOT_USERS) {
        const char *rest = path ? path : "";
        struct reg_key *r = users_path(rest, &rest);
        if (!r) {
            *err = ERROR_FILE_NOT_FOUND;
            return NULL;
        }
        struct reg_key *k = walk(r, rest, create, created);
        *err = k ? 0 : ERROR_FILE_NOT_FOUND;
        return k;
    }
    if (is_classes(base) && path && *path) {
        struct reg_key *mine = walk(user.root, "Software\\Classes", true, NULL);
        struct reg_key *k = mine ? walk(mine, path, false, NULL) : NULL;
        if (k) {
            *err = 0;
            return k;
        }
    }
    struct reg_key *k = key_of(base, err);
    if (!k)
        return NULL;
    k = walk(k, path, create, created);
    if (!k && !*err)
        *err = ERROR_FILE_NOT_FOUND;
    return k;
}

/* text conversions: names come in as UTF-16 (W) or the ANSI code page (A) */

static char *name_from_w(const WCHAR *w)
{
    if (!w)
        return NULL;
    size_t n = w32_wide_to_cp(w, -1, NULL, 0, CP_UTF8);
    char *s = kmalloc(n + 1);
    if (s) {
        w32_wide_to_cp(w, -1, s, n, CP_UTF8);
        s[n] = 0;
    }
    return s;
}

static char *name_from_a(const char *a)
{
    if (!a)
        return NULL;
    WCHAR *w = w32_widen(a);
    char *s = name_from_w(w);
    w32_free(w);
    return s;
}

static bool string_type(uint32_t t)
{
    return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ;
}

/* Core operations on UTF-8 names. */

static LONG reg_open(HANDLE base, const char *sub, HANDLE *out)
{
    LONG err;
    if (!out)
        return ERROR_INVALID_PARAMETER;
    *out = NULL;
    struct reg_key *k = open_path(base, sub, false, NULL, &err);
    if (!k)
        return err;
    *out = new_handle(k);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}

static LONG reg_create(HANDLE base, const char *sub, HANDLE *out, DWORD *disposition)
{
    LONG err;
    bool created = false;
    if (!out)
        return ERROR_INVALID_PARAMETER;
    *out = NULL;
    struct reg_key *k = open_path(base, sub, true, &created, &err);
    if (!k)
        return err ? err : ERROR_ACCESS_DENIED;
    if (disposition)
        *disposition = created ? 1 : 2;
    *out = new_handle(k);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}

static LONG reg_set(HANDLE base, const char *sub, const char *name, uint32_t type, const void *data, uint32_t size)
{
    LONG err;
    struct reg_key *k = open_path(base, sub, sub && *sub, NULL, &err);
    if (!k)
        return err;
    if (size && !data)
        return ERROR_INVALID_PARAMETER;
    return set_value(k, name, type, data, size);
}

/* a value as the caller wants it: wide strings kept as they are, or turned into the ANSI code page */
static LONG reg_get(HANDLE base, const char *sub, const char *name, DWORD *type, void *data, DWORD *size, bool ansi)
{
    LONG err;
    struct reg_key *k = open_path(base, sub, false, NULL, &err);
    if (!k)
        return err;
    struct reg_value *v = find_value(k, name);
    if (!v)
        return ERROR_FILE_NOT_FOUND;
    if (type)
        *type = v->type;
    const uint8_t *src = v->data;
    uint32_t need = v->size;
    char *conv = NULL;
    if (ansi && string_type(v->type)) {
        int wn = (int)(v->size / 2);
        size_t n = w32_wide_to_cp((const WCHAR *)v->data, wn, NULL, 0, 0);
        if (!(conv = kmalloc(n + 1)))
            return ERROR_NOT_ENOUGH_MEMORY;
        w32_wide_to_cp((const WCHAR *)v->data, wn, conv, n, 0);
        conv[n] = 0;
        src = (const uint8_t *)conv;
        need = (uint32_t)n;
    }
    LONG r = 0;
    if (data && size) {
        if (*size < need)
            r = ERROR_MORE_DATA;
        else
            memcpy(data, src, need);
    } else if (data && !size) {
        r = ERROR_INVALID_PARAMETER;
    }
    if (size)
        *size = need;
    kfree(conv);
    return r;
}

static LONG reg_delete_value(HANDLE base, const char *sub, const char *name)
{
    LONG err;
    struct reg_key *k = open_path(base, sub, false, NULL, &err);
    if (!k)
        return err;
    return delete_value(k, name) ? 0 : ERROR_FILE_NOT_FOUND;
}

static LONG reg_delete_key(HANDLE base, const char *sub, bool tree)
{
    LONG err;
    struct reg_key *k = open_path(base, sub, false, NULL, &err);
    if (!k)
        return err;
    if (!sub || !*sub) {
        if (!tree)
            return ERROR_ACCESS_DENIED;
        while (k->child)
            unlink_key(k->child);
        while (k->values)
            delete_value(k, k->values->name);
        return 0;
    }
    if (!k->parent || (k->child && !tree))
        return ERROR_ACCESS_DENIED;
    unlink_key(k);
    return 0;
}

static struct reg_key *nth_child(struct reg_key *k, DWORD index)
{
    struct reg_key *c = k->child;
    while (c && index--)
        c = c->next;
    return c;
}

static struct reg_value *nth_value(struct reg_key *k, DWORD index)
{
    struct reg_value *v = k->values;
    while (v && index--)
        v = v->next;
    return v;
}

/* names handed back: UTF-16 or ANSI, len in characters without the terminator */
static LONG give_name(const char *utf8, void *out, DWORD *len, bool ansi)
{
    if (!len)
        return ERROR_INVALID_PARAMETER;
    size_t n = strlen(utf8);
    WCHAR wide[256];
    size_t wn = w32_cp_to_wide(utf8, (int)n, wide, ARRAY_SIZE(wide) - 1, CP_UTF8);
    wide[wn] = 0;
    size_t chars = ansi ? w32_wide_to_cp(wide, (int)wn, NULL, 0, 0) : wn;
    if (!out || *len <= chars) {
        *len = (DWORD)chars + (out ? 1 : 0);
        return ERROR_MORE_DATA;
    }
    if (ansi) {
        w32_wide_to_cp(wide, (int)wn, out, chars, 0);
        ((char *)out)[chars] = 0;
    } else {
        memcpy(out, wide, (wn + 1) * sizeof(WCHAR));
    }
    *len = (DWORD)chars;
    return 0;
}

static LONG reg_enum_key(HANDLE h, DWORD index, void *name, DWORD *len, uint64_t *time, bool ansi)
{
    LONG err;
    struct reg_key *k = is_classes(h) ? open_path(h, NULL, false, NULL, &err) : key_of(h, &err);
    if ((uint32_t)(uintptr_t)h == KEY_ROOT_USERS) {
        static const char *const names[] = {".DEFAULT", USER_SID};
        if (index >= ARRAY_SIZE(names))
            return ERROR_NO_MORE_ITEMS;
        return give_name(names[index], name, len, ansi);
    }
    if (!k)
        return err;
    struct reg_key *c = nth_child(k, index);
    if (!c)
        return ERROR_NO_MORE_ITEMS;
    if (time)
        *time = c->written;
    return give_name(c->name, name, len, ansi);
}

static LONG reg_enum_value(HANDLE h, DWORD index, void *name, DWORD *len, DWORD *type, void *data, DWORD *size,
                           bool ansi)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    if (!k)
        return err;
    struct reg_value *v = nth_value(k, index);
    if (!v)
        return ERROR_NO_MORE_ITEMS;
    LONG r = give_name(v->name, name, len, ansi);
    if (r)
        return r;
    if (!data && !size && !type)
        return 0;
    char *vname = kstrdup(v->name);
    r = reg_get(h, NULL, vname, type, data, size, ansi);
    kfree(vname);
    return r;
}

/* Win32 API */

typedef HANDLE HKEY;

static LONG WINAPI RegOpenKeyExW(HKEY key, const WCHAR *sub, DWORD opts, DWORD sam, HKEY *out)
{
    UNUSED(opts);
    UNUSED(sam);
    char *s = name_from_w(sub);
    LONG r = reg_open(key, s, out);
    kfree(s);
    return r;
}

static LONG WINAPI RegOpenKeyExA(HKEY key, const char *sub, DWORD opts, DWORD sam, HKEY *out)
{
    UNUSED(opts);
    UNUSED(sam);
    char *s = name_from_a(sub);
    LONG r = reg_open(key, s, out);
    kfree(s);
    return r;
}

static LONG WINAPI RegOpenKeyW(HKEY key, const WCHAR *sub, HKEY *out)
{
    return RegOpenKeyExW(key, sub, 0, 0, out);
}

static LONG WINAPI RegOpenKeyA(HKEY key, const char *sub, HKEY *out)
{
    return RegOpenKeyExA(key, sub, 0, 0, out);
}

static LONG WINAPI RegOpenKeyTransactedW(HKEY key, const WCHAR *sub, DWORD opts, DWORD sam, HKEY *out, HANDLE tx,
                                         void *param)
{
    UNUSED(tx);
    UNUSED(param);
    return RegOpenKeyExW(key, sub, opts, sam, out);
}

static LONG WINAPI RegCreateKeyExW(HKEY key, const WCHAR *sub, DWORD reserved, WCHAR *cls, DWORD opts, DWORD sam,
                                   void *sa, HKEY *out, DWORD *disposition)
{
    UNUSED(reserved);
    UNUSED(cls);
    UNUSED(opts);
    UNUSED(sam);
    UNUSED(sa);
    char *s = name_from_w(sub);
    LONG r = reg_create(key, s, out, disposition);
    kfree(s);
    return r;
}

static LONG WINAPI RegCreateKeyExA(HKEY key, const char *sub, DWORD reserved, char *cls, DWORD opts, DWORD sam,
                                   void *sa, HKEY *out, DWORD *disposition)
{
    UNUSED(reserved);
    UNUSED(cls);
    UNUSED(opts);
    UNUSED(sam);
    UNUSED(sa);
    char *s = name_from_a(sub);
    LONG r = reg_create(key, s, out, disposition);
    kfree(s);
    return r;
}

static LONG WINAPI RegCreateKeyW(HKEY key, const WCHAR *sub, HKEY *out)
{
    return RegCreateKeyExW(key, sub, 0, NULL, 0, 0, NULL, out, NULL);
}

static LONG WINAPI RegCreateKeyA(HKEY key, const char *sub, HKEY *out)
{
    return RegCreateKeyExA(key, sub, 0, NULL, 0, 0, NULL, out, NULL);
}

static LONG WINAPI RegCreateKeyTransactedW(HKEY key, const WCHAR *sub, DWORD reserved, WCHAR *cls, DWORD opts,
                                           DWORD sam, void *sa, HKEY *out, DWORD *disposition, HANDLE tx, void *param)
{
    UNUSED(tx);
    UNUSED(param);
    return RegCreateKeyExW(key, sub, reserved, cls, opts, sam, sa, out, disposition);
}

static LONG WINAPI RegCloseKey(HKEY key)
{
    if ((uint32_t)(uintptr_t)key >= KEY_ROOT_CLASSES && (uint32_t)(uintptr_t)key <= KEY_ROOT_LOCAL)
        return 0;
    struct w32_object *o = w32_handle_get(key);
    if (!o || o->type != WH_KEY)
        return ERROR_INVALID_HANDLE;
    w32_handle_close(key);
    return 0;
}

static LONG WINAPI RegFlushKey(HKEY key)
{
    UNUSED(key);
    w32_registry_flush();
    return 0;
}

static LONG WINAPI RegOpenCurrentUser(DWORD sam, HKEY *out)
{
    UNUSED(sam);
    *out = (HKEY)(uintptr_t)KEY_ROOT_USER;
    return 0;
}

static LONG WINAPI RegQueryValueExW(HKEY key, const WCHAR *name, DWORD *reserved, DWORD *type, BYTE *data,
                                    DWORD *size)
{
    UNUSED(reserved);
    char *n = name_from_w(name);
    LONG r = reg_get(key, NULL, n ? n : "", type, data, size, false);
    kfree(n);
    return r;
}

static LONG WINAPI RegQueryValueExA(HKEY key, const char *name, DWORD *reserved, DWORD *type, BYTE *data,
                                    DWORD *size)
{
    UNUSED(reserved);
    char *n = name_from_a(name);
    LONG r = reg_get(key, NULL, n ? n : "", type, data, size, true);
    kfree(n);
    return r;
}

/* the old API: the default value of a subkey, always a string */
static LONG WINAPI RegQueryValueW(HKEY key, const WCHAR *sub, WCHAR *data, LONG *size)
{
    char *s = name_from_w(sub);
    LONG r = reg_get(key, s, "", NULL, data, (DWORD *)size, false);
    kfree(s);
    return r;
}

static LONG WINAPI RegQueryValueA(HKEY key, const char *sub, char *data, LONG *size)
{
    char *s = name_from_a(sub);
    LONG r = reg_get(key, s, "", NULL, data, (DWORD *)size, true);
    kfree(s);
    return r;
}

#define RRF_RT_ANY 0xffffu
#define RRF_NOEXPAND 0x10000000u
#define RRF_ZEROONFAILURE 0x20000000u

static bool type_allowed(uint32_t type, DWORD flags)
{
    uint32_t bit = type == REG_SZ ? 0x02 : type == REG_EXPAND_SZ ? 0x04 : type == REG_BINARY ? 0x08
                 : type == REG_DWORD ? 0x10 : type == REG_MULTI_SZ ? 0x20 : type == REG_QWORD ? 0x40 : 0x01;
    if (type == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND) && (flags & 0x02))
        return true;
    return (flags & RRF_RT_ANY) == RRF_RT_ANY || (flags & bit);
}

/* REG_EXPAND_SZ asked for as a string: %VARIABLES% replaced, the size that of the expanded text */
static LONG get_expanded(HKEY key, const char *sub, const char *name, void *data, DWORD *size, bool ansi)
{
    DWORD raw_size = 0;
    LONG r = reg_get(key, sub, name, NULL, NULL, &raw_size, false);
    if (r)
        return r;
    WCHAR *raw = kzalloc(raw_size + 2), *out = NULL;
    if (!raw || (r = reg_get(key, sub, name, NULL, raw, &raw_size, false))) {
        kfree(raw);
        return r ? r : ERROR_NOT_ENOUGH_MEMORY;
    }
    DWORD(WINAPI * expand)(const WCHAR *, WCHAR *, DWORD) =
        (DWORD(WINAPI *)(const WCHAR *, WCHAR *, DWORD))w32_builtin_lookup("kernel32.dll", "ExpandEnvironmentStringsW");
    DWORD chars = expand ? expand(raw, NULL, 0) : 0;
    if (chars && (out = kmalloc(chars * sizeof(WCHAR))))
        expand(raw, out, chars);
    const WCHAR *text = out ? out : raw;
    size_t wn = w32_wcslen(text) + 1;
    DWORD need = ansi ? (DWORD)w32_wide_to_cp(text, (int)wn, NULL, 0, 0) : (DWORD)wn * 2;
    if (data && *size >= need) {
        if (ansi)
            w32_wide_to_cp(text, (int)wn, data, need, 0);
        else
            memcpy(data, text, need);
    } else if (data) {
        r = ERROR_MORE_DATA;
    }
    *size = need;
    kfree(raw);
    kfree(out);
    return r;
}

static LONG get_value(HKEY key, const char *sub, const char *name, DWORD flags, DWORD *type, void *data, DWORD *size,
                      bool ansi)
{
    DWORD t = 0, cap = size ? *size : 0;
    LONG r = reg_get(key, sub, name ? name : "", &t, NULL, NULL, ansi);
    if (!r && t == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND) && size) {
        if (type)
            *type = REG_SZ;
        if (!(flags & 0x02))
            return ERROR_UNSUPPORTED_TYPE;
        return get_expanded(key, sub, name ? name : "", data, size, ansi);
    }
    if (!r)
        r = reg_get(key, sub, name ? name : "", &t, data, size, ansi);
    if (type)
        *type = t;
    if (!r || r == ERROR_MORE_DATA) {
        if (!type_allowed(t, flags))
            r = ERROR_UNSUPPORTED_TYPE;
        else if (t == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND) && type)
            *type = REG_SZ;
    }
    /* strings come back terminated, even when stored without the terminator */
    if (!r && data && size && string_type(t)) {
        DWORD unit = ansi ? 1 : 2;
        if (*size + unit <= cap && (*size < unit || ((uint8_t *)data)[*size - 1] || (!ansi && ((uint8_t *)data)[*size - 2]))) {
            memset((uint8_t *)data + *size, 0, unit);
            *size += unit;
        }
    }
    if (r && r != ERROR_MORE_DATA && data && (flags & RRF_ZEROONFAILURE) && cap)
        memset(data, 0, cap);
    return r;
}

static LONG WINAPI RegGetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, DWORD flags, DWORD *type, void *data,
                                DWORD *size)
{
    char *s = name_from_w(sub), *v = name_from_w(value);
    LONG r = get_value(key, s, v, flags, type, data, size, false);
    kfree(s);
    kfree(v);
    return r;
}

static LONG WINAPI RegGetValueA(HKEY key, const char *sub, const char *value, DWORD flags, DWORD *type, void *data,
                                DWORD *size)
{
    char *s = name_from_a(sub), *v = name_from_a(value);
    LONG r = get_value(key, s, v, flags, type, data, size, true);
    kfree(s);
    kfree(v);
    return r;
}

/* ANSI string data is kept as UTF-16, like every other string */
static LONG set_ansi(HKEY key, const char *sub, const char *name, DWORD type, const BYTE *data, DWORD size)
{
    if (!string_type(type) || !data)
        return reg_set(key, sub, name ? name : "", type, data, size);
    size_t wn = w32_cp_to_wide((const char *)data, (int)size, NULL, 0, 0);
    WCHAR *w = kmalloc((wn + 1) * sizeof(WCHAR));
    if (!w)
        return ERROR_NOT_ENOUGH_MEMORY;
    w32_cp_to_wide((const char *)data, (int)size, w, wn, 0);
    LONG r = reg_set(key, sub, name ? name : "", type, w, (uint32_t)wn * 2);
    kfree(w);
    return r;
}

static LONG WINAPI RegSetValueExW(HKEY key, const WCHAR *name, DWORD reserved, DWORD type, const BYTE *data,
                                  DWORD size)
{
    UNUSED(reserved);
    char *n = name_from_w(name);
    LONG r = reg_set(key, NULL, n ? n : "", type, data, size);
    kfree(n);
    return r;
}

static LONG WINAPI RegSetValueExA(HKEY key, const char *name, DWORD reserved, DWORD type, const BYTE *data,
                                  DWORD size)
{
    UNUSED(reserved);
    char *n = name_from_a(name);
    LONG r = set_ansi(key, NULL, n, type, data, size);
    kfree(n);
    return r;
}

static LONG WINAPI RegSetValueW(HKEY key, const WCHAR *sub, DWORD type, const WCHAR *data, DWORD len)
{
    UNUSED(len);
    char *s = name_from_w(sub);
    LONG r = reg_set(key, s, "", type, data, data ? (DWORD)(w32_wcslen(data) + 1) * 2 : 0);
    kfree(s);
    return r;
}

static LONG WINAPI RegSetValueA(HKEY key, const char *sub, DWORD type, const char *data, DWORD len)
{
    UNUSED(len);
    char *s = name_from_a(sub);
    LONG r = set_ansi(key, s, "", type, (const BYTE *)data, data ? (DWORD)strlen(data) + 1 : 0);
    kfree(s);
    return r;
}

static LONG WINAPI RegSetKeyValueW(HKEY key, const WCHAR *sub, const WCHAR *name, DWORD type, const void *data,
                                   DWORD size)
{
    char *s = name_from_w(sub), *n = name_from_w(name);
    LONG r = reg_set(key, s, n ? n : "", type, data, size);
    kfree(s);
    kfree(n);
    return r;
}

static LONG WINAPI RegSetKeyValueA(HKEY key, const char *sub, const char *name, DWORD type, const void *data,
                                   DWORD size)
{
    char *s = name_from_a(sub), *n = name_from_a(name);
    LONG r = set_ansi(key, s, n, type, data, size);
    kfree(s);
    kfree(n);
    return r;
}

static LONG WINAPI RegDeleteValueW(HKEY key, const WCHAR *name)
{
    char *n = name_from_w(name);
    LONG r = reg_delete_value(key, NULL, n ? n : "");
    kfree(n);
    return r;
}

static LONG WINAPI RegDeleteValueA(HKEY key, const char *name)
{
    char *n = name_from_a(name);
    LONG r = reg_delete_value(key, NULL, n ? n : "");
    kfree(n);
    return r;
}

static LONG WINAPI RegDeleteKeyValueW(HKEY key, const WCHAR *sub, const WCHAR *name)
{
    char *s = name_from_w(sub), *n = name_from_w(name);
    LONG r = reg_delete_value(key, s, n ? n : "");
    kfree(s);
    kfree(n);
    return r;
}

static LONG WINAPI RegDeleteKeyValueA(HKEY key, const char *sub, const char *name)
{
    char *s = name_from_a(sub), *n = name_from_a(name);
    LONG r = reg_delete_value(key, s, n ? n : "");
    kfree(s);
    kfree(n);
    return r;
}

static LONG WINAPI RegDeleteKeyW(HKEY key, const WCHAR *sub)
{
    char *s = name_from_w(sub);
    LONG r = s && *s ? reg_delete_key(key, s, false) : ERROR_INVALID_PARAMETER;
    kfree(s);
    return r;
}

static LONG WINAPI RegDeleteKeyA(HKEY key, const char *sub)
{
    char *s = name_from_a(sub);
    LONG r = s && *s ? reg_delete_key(key, s, false) : ERROR_INVALID_PARAMETER;
    kfree(s);
    return r;
}

static LONG WINAPI RegDeleteKeyExW(HKEY key, const WCHAR *sub, DWORD sam, DWORD reserved)
{
    UNUSED(sam);
    UNUSED(reserved);
    return RegDeleteKeyW(key, sub);
}

static LONG WINAPI RegDeleteKeyExA(HKEY key, const char *sub, DWORD sam, DWORD reserved)
{
    UNUSED(sam);
    UNUSED(reserved);
    return RegDeleteKeyA(key, sub);
}

static LONG WINAPI RegDeleteTreeW(HKEY key, const WCHAR *sub)
{
    char *s = name_from_w(sub);
    LONG r = reg_delete_key(key, s, true);
    kfree(s);
    return r;
}

static LONG WINAPI RegDeleteTreeA(HKEY key, const char *sub)
{
    char *s = name_from_a(sub);
    LONG r = reg_delete_key(key, s, true);
    kfree(s);
    return r;
}

static LONG WINAPI RegEnumKeyExW(HKEY key, DWORD index, WCHAR *name, DWORD *len, DWORD *reserved, WCHAR *cls,
                                 DWORD *cls_len, uint64_t *time)
{
    UNUSED(reserved);
    if (cls && cls_len && *cls_len)
        cls[0] = 0;
    if (cls_len)
        *cls_len = 0;
    return reg_enum_key(key, index, name, len, time, false);
}

static LONG WINAPI RegEnumKeyExA(HKEY key, DWORD index, char *name, DWORD *len, DWORD *reserved, char *cls,
                                 DWORD *cls_len, uint64_t *time)
{
    UNUSED(reserved);
    if (cls && cls_len && *cls_len)
        cls[0] = 0;
    if (cls_len)
        *cls_len = 0;
    return reg_enum_key(key, index, name, len, time, true);
}

static LONG WINAPI RegEnumKeyW(HKEY key, DWORD index, WCHAR *name, DWORD len)
{
    return reg_enum_key(key, index, name, &len, NULL, false);
}

static LONG WINAPI RegEnumKeyA(HKEY key, DWORD index, char *name, DWORD len)
{
    return reg_enum_key(key, index, name, &len, NULL, true);
}

static LONG WINAPI RegEnumValueW(HKEY key, DWORD index, WCHAR *name, DWORD *len, DWORD *reserved, DWORD *type,
                                 BYTE *data, DWORD *size)
{
    UNUSED(reserved);
    return reg_enum_value(key, index, name, len, type, data, size, false);
}

static LONG WINAPI RegEnumValueA(HKEY key, DWORD index, char *name, DWORD *len, DWORD *reserved, DWORD *type,
                                 BYTE *data, DWORD *size)
{
    UNUSED(reserved);
    return reg_enum_value(key, index, name, len, type, data, size, true);
}

static LONG query_info(HKEY h, DWORD *subkeys, DWORD *max_sub, DWORD *max_cls, DWORD *values, DWORD *max_name,
                       DWORD *max_value, DWORD *sec, uint64_t *time)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    if (!k)
        return err;
    DWORD ns = 0, ms = 0, nv = 0, mn = 0, mv = 0;
    for (struct reg_key *c = k->child; c; c = c->next, ns++)
        ms = MAX(ms, (DWORD)strlen(c->name));
    for (struct reg_value *v = k->values; v; v = v->next, nv++) {
        mn = MAX(mn, (DWORD)strlen(v->name));
        mv = MAX(mv, v->size);
    }
    if (subkeys)
        *subkeys = ns;
    if (max_sub)
        *max_sub = ms;
    if (max_cls)
        *max_cls = 0;
    if (values)
        *values = nv;
    if (max_name)
        *max_name = mn;
    if (max_value)
        *max_value = mv;
    if (sec)
        *sec = 0;
    if (time)
        *time = k->written;
    return 0;
}

static LONG WINAPI RegQueryInfoKeyW(HKEY key, WCHAR *cls, DWORD *cls_len, DWORD *reserved, DWORD *subkeys,
                                    DWORD *max_sub, DWORD *max_cls, DWORD *values, DWORD *max_name, DWORD *max_value,
                                    DWORD *sec, uint64_t *time)
{
    UNUSED(reserved);
    if (cls && cls_len && *cls_len)
        cls[0] = 0;
    if (cls_len)
        *cls_len = 0;
    return query_info(key, subkeys, max_sub, max_cls, values, max_name, max_value, sec, time);
}

static LONG WINAPI RegQueryInfoKeyA(HKEY key, char *cls, DWORD *cls_len, DWORD *reserved, DWORD *subkeys,
                                    DWORD *max_sub, DWORD *max_cls, DWORD *values, DWORD *max_name, DWORD *max_value,
                                    DWORD *sec, uint64_t *time)
{
    UNUSED(reserved);
    if (cls && cls_len && *cls_len)
        cls[0] = 0;
    if (cls_len)
        *cls_len = 0;
    return query_info(key, subkeys, max_sub, max_cls, values, max_name, max_value, sec, time);
}

static void copy_tree(struct reg_key *from, struct reg_key *to)
{
    for (struct reg_value *v = from->values; v; v = v->next)
        set_value(to, v->name, v->type, v->data, v->size);
    for (struct reg_key *c = from->child; c; c = c->next) {
        struct reg_key *d = walk(to, c->name, true, NULL);
        if (d)
            copy_tree(c, d);
    }
}

static LONG WINAPI RegCopyTreeW(HKEY src, const WCHAR *sub, HKEY dst)
{
    LONG err;
    char *s = name_from_w(sub);
    struct reg_key *from = open_path(src, s, false, NULL, &err);
    kfree(s);
    if (!from)
        return err;
    struct reg_key *to = key_of(dst, &err);
    if (!to)
        return err;
    copy_tree(from, to);
    return 0;
}

static LONG WINAPI RegRenameKey(HKEY key, const WCHAR *sub, const WCHAR *name)
{
    LONG err;
    char *s = name_from_w(sub), *n = name_from_w(name);
    struct reg_key *k = open_path(key, s, false, NULL, &err);
    kfree(s);
    if (!k || !n || !*n || !k->parent || find_child(k->parent, n, strlen(n))) {
        kfree(n);
        return k ? ERROR_ACCESS_DENIED : err;
    }
    struct reg_key *parent = k->parent, *copy = key_new(n, parent);
    kfree(n);
    if (!copy)
        return ERROR_NOT_ENOUGH_MEMORY;
    copy_tree(k, copy);
    unlink_key(k);
    return 0;
}

/* an indirect string ("@file.dll,-id") would need the resource: only plain strings are given */
static LONG WINAPI RegLoadMUIStringW(HKEY key, const WCHAR *value, WCHAR *out, DWORD size, DWORD *need, DWORD flags,
                                     const WCHAR *dir)
{
    UNUSED(flags);
    UNUSED(dir);
    DWORD type = 0, bytes = size;
    char *v = name_from_w(value);
    LONG r = reg_get(key, NULL, v ? v : "", &type, out, &bytes, false);
    kfree(v);
    if (need)
        *need = bytes;
    if (!r && (type != REG_SZ || (out && out[0] == '@')))
        r = ERROR_FILE_NOT_FOUND;
    return r;
}

static LONG WINAPI RegNotifyChangeKeyValue(HKEY key, BOOL subtree, DWORD filter, HANDLE event, BOOL async)
{
    UNUSED(subtree);
    UNUSED(filter);
    UNUSED(event);
    UNUSED(async);
    LONG err;
    return key_of(key, &err) ? 0 : err;
}

static LONG WINAPI RegOverridePredefKey(HKEY key, HKEY with)
{
    UNUSED(key);
    UNUSED(with);
    return 0;
}

static const struct w32_export registry_exports[] = {
    W32_FN(RegOpenKeyExW),      W32_FN(RegOpenKeyExA),       W32_FN(RegOpenKeyW),         W32_FN(RegOpenKeyA),
    W32_FN(RegOpenKeyTransactedW), W32_FN(RegCreateKeyExW),  W32_FN(RegCreateKeyExA),     W32_FN(RegCreateKeyW),
    W32_FN(RegCreateKeyA),      W32_FN(RegCreateKeyTransactedW), W32_FN(RegCloseKey),     W32_FN(RegFlushKey),
    W32_FN(RegOpenCurrentUser), W32_FN(RegQueryValueExW),    W32_FN(RegQueryValueExA),    W32_FN(RegQueryValueW),
    W32_FN(RegQueryValueA),     W32_FN(RegGetValueW),        W32_FN(RegGetValueA),        W32_FN(RegSetValueExW),
    W32_FN(RegSetValueExA),     W32_FN(RegSetValueW),        W32_FN(RegSetValueA),        W32_FN(RegSetKeyValueW),
    W32_FN(RegSetKeyValueA),    W32_FN(RegDeleteValueW),     W32_FN(RegDeleteValueA),     W32_FN(RegDeleteKeyValueW),
    W32_FN(RegDeleteKeyValueA), W32_FN(RegDeleteKeyW),       W32_FN(RegDeleteKeyA),       W32_FN(RegDeleteKeyExW),
    W32_FN(RegDeleteKeyExA),    W32_FN(RegDeleteTreeW),      W32_FN(RegDeleteTreeA),      W32_FN(RegEnumKeyExW),
    W32_FN(RegEnumKeyExA),      W32_FN(RegEnumKeyW),         W32_FN(RegEnumKeyA),         W32_FN(RegEnumValueW),
    W32_FN(RegEnumValueA),      W32_FN(RegQueryInfoKeyW),    W32_FN(RegQueryInfoKeyA),    W32_FN(RegCopyTreeW),
    W32_FN(RegRenameKey),       W32_FN(RegLoadMUIStringW),   W32_FN(RegNotifyChangeKeyValue),
    W32_FN(RegOverridePredefKey),
};

W32_EXPORTS(registry_module, "advapi32.dll", registry_exports);

/* shlwapi's shortcuts over the same keys */

static LONG WINAPI SHGetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, DWORD *type, void *data, DWORD *size)
{
    return RegGetValueW(key, sub, value, RRF_RT_ANY | RRF_NOEXPAND, type, data, size);
}

static LONG WINAPI SHGetValueA(HKEY key, const char *sub, const char *value, DWORD *type, void *data, DWORD *size)
{
    return RegGetValueA(key, sub, value, RRF_RT_ANY | RRF_NOEXPAND, type, data, size);
}

static LONG WINAPI SHSetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, DWORD type, const void *data,
                               DWORD size)
{
    return RegSetKeyValueW(key, sub, value, type, data, size);
}

static LONG WINAPI SHSetValueA(HKEY key, const char *sub, const char *value, DWORD type, const void *data, DWORD size)
{
    return RegSetKeyValueA(key, sub, value, type, data, size);
}

static LONG WINAPI SHRegGetValueW(HKEY key, const WCHAR *sub, const WCHAR *value, int flags, DWORD *type, void *data,
                                  DWORD *size)
{
    return RegGetValueW(key, sub, value, (DWORD)flags, type, data, size);
}

static LONG WINAPI SHQueryValueExW(HKEY key, const WCHAR *value, DWORD *reserved, DWORD *type, void *data,
                                   DWORD *size)
{
    return RegQueryValueExW(key, value, reserved, type, data, size);
}

static LONG WINAPI SHDeleteKeyW(HKEY key, const WCHAR *sub)
{
    return RegDeleteTreeW(key, sub);
}

static LONG WINAPI SHDeleteKeyA(HKEY key, const char *sub)
{
    return RegDeleteTreeA(key, sub);
}

static LONG WINAPI SHDeleteValueW(HKEY key, const WCHAR *sub, const WCHAR *value)
{
    return RegDeleteKeyValueW(key, sub, value);
}

static const struct w32_export shreg_exports[] = {
    W32_FN(SHGetValueW),     W32_FN(SHGetValueA),  W32_FN(SHSetValueW),  W32_FN(SHSetValueA),
    W32_FN(SHRegGetValueW),  W32_FN(SHQueryValueExW), W32_FN(SHDeleteKeyW), W32_FN(SHDeleteKeyA),
    W32_FN(SHDeleteValueW),
};

W32_EXPORTS(shreg_module, "shlwapi.dll", shreg_exports);

/* ntdll's view: "\Registry\Machine\..." and "\Registry\User\<sid>\..." */

struct unicode_string {
    uint16_t length, max_length;
    WCHAR *buffer;
};

struct object_attributes {
    ULONG length;
    HANDLE root;
    struct unicode_string *name;
    ULONG attributes;
    void *security, *qos;
};

#define STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034u
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_BUFFER_TOO_SMALL 0xc0000023u
#define STATUS_NO_MORE_ENTRIES 0x8000001au
#define STATUS_INVALID_HANDLE 0xc0000008u
#define STATUS_INVALID_PARAMETER 0xc000000du

static DWORD status_of(LONG err)
{
    switch (err) {
    case 0: return 0;
    case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_MORE_DATA: return STATUS_BUFFER_OVERFLOW;
    case ERROR_NO_MORE_ITEMS: return STATUS_NO_MORE_ENTRIES;
    case ERROR_INVALID_HANDLE: return STATUS_INVALID_HANDLE;
    case ERROR_ACCESS_DENIED: return 0xc0000022u;
    default: return STATUS_INVALID_PARAMETER;
    }
}

static char *us_name(const struct unicode_string *u)
{
    if (!u || !u->buffer)
        return kstrdup("");
    size_t n = u->length / 2;
    WCHAR *w = kmalloc((n + 1) * sizeof(WCHAR));
    if (!w)
        return NULL;
    memcpy(w, u->buffer, n * 2);
    w[n] = 0;
    char *s = name_from_w(w);
    kfree(w);
    return s;
}

/* the base handle and the rest of the path an object name stands for */
static HKEY nt_base(const struct object_attributes *attr, char *name, const char **rest)
{
    *rest = name;
    if (attr->root)
        return attr->root;
    static const char *const prefixes[][2] = {
        {"\\Registry\\Machine", (const char *)(uintptr_t)KEY_ROOT_MACHINE},
        {"\\Registry\\User", (const char *)(uintptr_t)KEY_ROOT_USERS},
    };
    for (size_t i = 0; i < ARRAY_SIZE(prefixes); i++) {
        size_t n = strlen(prefixes[i][0]);
        if (!str_ncasecmp(name, prefixes[i][0], n) && (!name[n] || name[n] == '\\')) {
            *rest = name + n;
            return (HKEY)prefixes[i][1];
        }
    }
    return NULL;
}

static DWORD nt_open(HANDLE *out, const struct object_attributes *attr, bool create, DWORD *disposition)
{
    if (!out || !attr)
        return STATUS_INVALID_PARAMETER;
    *out = NULL;
    char *name = us_name(attr->name);
    if (!name)
        return 0xc0000017u;
    const char *rest;
    HKEY base = nt_base(attr, name, &rest);
    LONG r = !base ? ERROR_FILE_NOT_FOUND : create ? reg_create(base, rest, out, disposition) : reg_open(base, rest, out);
    kfree(name);
    return status_of(r);
}

static DWORD WINAPI NtOpenKey(HANDLE *out, DWORD access, const struct object_attributes *attr)
{
    UNUSED(access);
    return nt_open(out, attr, false, NULL);
}

static DWORD WINAPI NtOpenKeyEx(HANDLE *out, DWORD access, const struct object_attributes *attr, DWORD options)
{
    UNUSED(options);
    return NtOpenKey(out, access, attr);
}

static DWORD WINAPI NtCreateKey(HANDLE *out, DWORD access, const struct object_attributes *attr, DWORD index,
                                void *cls, DWORD options, DWORD *disposition)
{
    UNUSED(access);
    UNUSED(index);
    UNUSED(cls);
    UNUSED(options);
    return nt_open(out, attr, true, disposition);
}

/* KEY_VALUE_BASIC (0), FULL (1) and PARTIAL (2) information */
static DWORD value_info(struct reg_value *v, int cls, uint8_t *info, DWORD len, DWORD *result)
{
    WCHAR wname[256];
    size_t wn = w32_cp_to_wide(v->name, (int)strlen(v->name), wname, ARRAY_SIZE(wname) - 1, CP_UTF8);
    DWORD head = cls == 0 ? 12 : cls == 1 ? 20 : cls == 2 ? 12 : 0;
    if (!head)
        return STATUS_INVALID_PARAMETER;
    DWORD name_bytes = cls == 2 ? 0 : (DWORD)wn * 2, data_bytes = cls == 0 ? 0 : v->size;
    DWORD need = head + name_bytes + data_bytes;
    if (result)
        *result = need;
    if (len < head)
        return STATUS_BUFFER_TOO_SMALL;
    uint32_t *w = (uint32_t *)info;
    w[0] = 0;
    w[1] = v->type;
    if (cls == 0)
        w[2] = name_bytes;
    else if (cls == 1)
        w[2] = head + name_bytes, w[3] = v->size, w[4] = name_bytes;
    else
        w[2] = v->size;
    if (len < need)
        return STATUS_BUFFER_OVERFLOW;
    memcpy(info + head, wname, name_bytes);
    memcpy(info + head + name_bytes, v->data, data_bytes);
    return 0;
}

static DWORD WINAPI NtQueryValueKey(HANDLE h, const struct unicode_string *name, int cls, void *info, DWORD len,
                                    DWORD *result)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    if (!k)
        return status_of(err);
    char *n = us_name(name);
    struct reg_value *v = n ? find_value(k, n) : NULL;
    kfree(n);
    if (!v)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    return value_info(v, cls, info, len, result);
}

static DWORD WINAPI NtEnumerateValueKey(HANDLE h, DWORD index, int cls, void *info, DWORD len, DWORD *result)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    if (!k)
        return status_of(err);
    struct reg_value *v = nth_value(k, index);
    return v ? value_info(v, cls, info, len, result) : STATUS_NO_MORE_ENTRIES;
}

/* KEY_BASIC_INFORMATION: write time, title index, name */
static DWORD key_info(struct reg_key *k, int cls, uint8_t *info, DWORD len, DWORD *result)
{
    if (cls != 0 && cls != 3)
        return STATUS_INVALID_PARAMETER;
    WCHAR wname[256];
    size_t wn = w32_cp_to_wide(k->name, (int)strlen(k->name), wname, ARRAY_SIZE(wname) - 1, CP_UTF8);
    DWORD head = cls == 0 ? 16 : 4, need = head + (DWORD)wn * 2;
    if (result)
        *result = need;
    if (len < need)
        return len < head ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;
    if (cls == 0) {
        memcpy(info, &k->written, 8);
        memset(info + 8, 0, 4);
        memcpy(info + 12, &(uint32_t){(uint32_t)wn * 2}, 4);
    } else {
        memcpy(info, &(uint32_t){(uint32_t)wn * 2}, 4);
    }
    memcpy(info + head, wname, wn * 2);
    return 0;
}

static DWORD WINAPI NtEnumerateKey(HANDLE h, DWORD index, int cls, void *info, DWORD len, DWORD *result)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    if (!k)
        return status_of(err);
    struct reg_key *c = nth_child(k, index);
    return c ? key_info(c, cls, info, len, result) : STATUS_NO_MORE_ENTRIES;
}

static DWORD WINAPI NtQueryKey(HANDLE h, int cls, void *info, DWORD len, DWORD *result)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    return k ? key_info(k, cls, info, len, result) : status_of(err);
}

static DWORD WINAPI NtSetValueKey(HANDLE h, const struct unicode_string *name, DWORD index, DWORD type,
                                  const void *data, DWORD size)
{
    UNUSED(index);
    char *n = us_name(name);
    LONG r = n ? reg_set(h, NULL, n, type, data, size) : ERROR_NOT_ENOUGH_MEMORY;
    kfree(n);
    return status_of(r);
}

static DWORD WINAPI NtDeleteValueKey(HANDLE h, const struct unicode_string *name)
{
    char *n = us_name(name);
    LONG r = n ? reg_delete_value(h, NULL, n) : ERROR_NOT_ENOUGH_MEMORY;
    kfree(n);
    return status_of(r);
}

static DWORD WINAPI NtDeleteKey(HANDLE h)
{
    LONG err;
    struct reg_key *k = key_of(h, &err);
    if (!k)
        return status_of(err);
    if (k->child || !k->parent)
        return 0xc0000022u;
    unlink_key(k);
    return 0;
}

static DWORD WINAPI NtFlushKey(HANDLE h)
{
    UNUSED(h);
    w32_registry_flush();
    return 0;
}

static DWORD WINAPI RtlOpenCurrentUser(DWORD access, HANDLE *out)
{
    UNUSED(access);
    *out = (HANDLE)(uintptr_t)KEY_ROOT_USER;
    return 0;
}

static const struct w32_export ntreg_exports[] = {
    W32_FN(NtOpenKey),       W32_FN(NtOpenKeyEx),  W32_FN(NtCreateKey),         W32_FN(NtQueryValueKey),
    W32_FN(NtEnumerateValueKey), W32_FN(NtEnumerateKey), W32_FN(NtQueryKey),   W32_FN(NtSetValueKey),
    W32_FN(NtDeleteValueKey), W32_FN(NtDeleteKey), W32_FN(NtFlushKey),         W32_FN(RtlOpenCurrentUser),
    W32_AS("ZwOpenKey", NtOpenKey), W32_AS("ZwQueryValueKey", NtQueryValueKey), W32_AS("ZwSetValueKey", NtSetValueKey),
    W32_AS("ZwCreateKey", NtCreateKey), W32_AS("ZwEnumerateKey", NtEnumerateKey),
};

W32_EXPORTS(ntreg_module, "ntdll.dll", ntreg_exports);

/* Kernel side: the same tree for the desktop and the shell. */

static HKEY root_named(const char *name, const char **rest)
{
    static const struct {
        const char *name;
        uint32_t key;
    } roots[] = {
        {"HKEY_LOCAL_MACHINE", KEY_ROOT_MACHINE}, {"HKLM", KEY_ROOT_MACHINE}, {"HKEY_CURRENT_USER", KEY_ROOT_USER},
        {"HKCU", KEY_ROOT_USER},                  {"HKEY_CLASSES_ROOT", KEY_ROOT_CLASSES}, {"HKCR", KEY_ROOT_CLASSES},
        {"HKEY_USERS", KEY_ROOT_USERS},           {"HKU", KEY_ROOT_USERS},  {"HKEY_CURRENT_CONFIG", KEY_ROOT_CONFIG},
        {"HKCC", KEY_ROOT_CONFIG},
    };
    for (size_t i = 0; i < ARRAY_SIZE(roots); i++) {
        size_t n = strlen(roots[i].name);
        if (!str_ncasecmp(name, roots[i].name, n) && (!name[n] || name[n] == '\\')) {
            *rest = name + n;
            return (HKEY)(uintptr_t)roots[i].key;
        }
    }
    return NULL;
}

/* "HKCU\Software\X" and a value name: the value as UTF-8 text (strings) or a number, false when absent */
bool w32_reg_read(const char *path, const char *name, char *out, size_t cap)
{
    const char *rest;
    HKEY base = root_named(path, &rest);
    LONG err;
    struct reg_key *k = base ? open_path(base, rest, false, NULL, &err) : NULL;
    struct reg_value *v = k ? find_value(k, name ? name : "") : NULL;
    if (!v)
        return false;
    if (string_type(v->type)) {
        size_t n = w32_wide_to_cp((const WCHAR *)v->data, (int)(v->size / 2), out, cap - 1, CP_UTF8);
        out[n] = 0;
    } else if (v->type == REG_DWORD && v->size >= 4) {
        ksnprintf(out, cap, "%u", *(const uint32_t *)v->data);
    } else {
        out[0] = 0;
    }
    return true;
}

bool w32_reg_write(const char *path, const char *name, const char *text)
{
    const char *rest;
    HKEY base = root_named(path, &rest);
    LONG err;
    struct reg_key *k = base ? open_path(base, rest, true, NULL, &err) : NULL;
    if (!k)
        return false;
    set_string(k, name ? name : "", REG_SZ, text, false);
    w32_registry_flush();
    return true;
}

/* the names of a key's subkeys, one call per name */
int w32_reg_subkeys(const char *path, void (*fn)(const char *name, void *ctx), void *ctx)
{
    const char *rest;
    HKEY base = root_named(path, &rest);
    LONG err;
    struct reg_key *k = base ? open_path(base, rest, false, NULL, &err) : NULL;
    if (!k)
        return E_NOENT;
    int n = 0;
    for (struct reg_key *c = k->child; c; c = c->next, n++)
        fn(c->name, ctx);
    return n;
}

static const char *type_name(uint32_t t)
{
    switch (t) {
    case REG_SZ: return "REG_SZ";
    case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
    case REG_BINARY: return "REG_BINARY";
    case REG_DWORD: return "REG_DWORD";
    case REG_MULTI_SZ: return "REG_MULTI_SZ";
    case REG_QWORD: return "REG_QWORD";
    default: return "REG_NONE";
    }
}

static void show_value(struct shell *sh, const struct reg_value *v)
{
    char text[512];
    if (string_type(v->type)) {
        size_t n = w32_wide_to_cp((const WCHAR *)v->data, (int)(v->size / 2), text, sizeof(text) - 1, CP_UTF8);
        text[n] = 0;
        for (size_t i = 0; i + 1 < n; i++)
            if (!text[i])
                text[i] = ' ';
    } else if (v->type == REG_DWORD && v->size >= 4) {
        ksnprintf(text, sizeof(text), "0x%x", *(const uint32_t *)v->data);
    } else {
        size_t o = 0;
        for (uint32_t i = 0; i < v->size && o + 3 < sizeof(text); i++)
            o += (size_t)ksnprintf(text + o, sizeof(text) - o, "%02x", v->data[i]);
        text[o] = 0;
    }
    sh_printf(sh, "    %s    %s    %s\n", v->name[0] ? v->name : "(Default)", type_name(v->type), text);
}

static void show_key(struct shell *sh, const char *path, struct reg_key *k, bool recurse)
{
    sh_role(sh, ROLE_ACCENT);
    sh_printf(sh, "%s\n", path);
    sh_role(sh, ROLE_TEXT);
    for (struct reg_value *v = k->values; v; v = v->next)
        show_value(sh, v);
    for (struct reg_key *c = k->child; c; c = c->next) {
        char sub[1024];
        ksnprintf(sub, sizeof(sub), "%s\\%s", path, c->name);
        if (recurse)
            show_key(sh, sub, c, true);
        else
            sh_printf(sh, "%s\n", sub);
    }
}

static const char *opt(int argc, char **argv, const char *flag)
{
    for (int i = 2; i + 1 < argc; i++)
        if (!str_casecmp(argv[i], flag))
            return argv[i + 1];
    return NULL;
}

static bool has(int argc, char **argv, const char *flag)
{
    for (int i = 2; i < argc; i++)
        if (!str_casecmp(argv[i], flag))
            return true;
    return false;
}

COMMAND(reg, NULL, "system", "reg query|add|delete KEY [/v NAME] [/t TYPE] [/d DATA] [/s] [/f]",
        "read and change the Windows registry, as reg.exe does")
{
    if (argc < 3)
        return command_usage(sh, command_find("reg"));
    /* the shell takes backslashes as escapes: HKCU/Software/X works as well as 'HKCU\Software\X' */
    for (char *p = argv[2]; *p; p++)
        if (*p == '/')
            *p = '\\';
    const char *rest;
    HKEY base = root_named(argv[2], &rest);
    if (!base) {
        sh_printf(sh, "reg: unknown root in %s\n", argv[2]);
        return E_INVAL;
    }
    const char *name = has(argc, argv, "/ve") ? "" : opt(argc, argv, "/v");
    LONG err;
    if (!str_casecmp(argv[1], "query")) {
        struct reg_key *k = open_path(base, rest, false, NULL, &err);
        if (!k) {
            sh_printf(sh, "reg: the key %s was not found\n", argv[2]);
            return E_NOENT;
        }
        if (name) {
            struct reg_value *v = find_value(k, name);
            if (!v) {
                sh_printf(sh, "reg: no value %s\n", name[0] ? name : "(Default)");
                return E_NOENT;
            }
            sh_printf(sh, "%s\n", argv[2]);
            show_value(sh, v);
            return 0;
        }
        show_key(sh, argv[2], k, has(argc, argv, "/s"));
        return 0;
    }
    if (!str_casecmp(argv[1], "add")) {
        struct reg_key *k = open_path(base, rest, true, NULL, &err);
        if (!k)
            return E_INVAL;
        if (name) {
            const char *t = opt(argc, argv, "/t"), *d = opt(argc, argv, "/d");
            if (t && !str_casecmp(t, "REG_DWORD")) {
                long v = 0;
                str_parse_long(d ? d : "0", &v);
                uint32_t dw = (uint32_t)v;
                set_value(k, name, REG_DWORD, &dw, 4);
            } else {
                set_string(k, name, t && !str_casecmp(t, "REG_EXPAND_SZ") ? REG_EXPAND_SZ : REG_SZ, d ? d : "", false);
            }
        }
        w32_registry_flush();
        sh_puts(sh, "The operation completed successfully.\n");
        return 0;
    }
    if (!str_casecmp(argv[1], "delete")) {
        err = name ? reg_delete_value(base, rest, name) : reg_delete_key(base, rest, true);
        if (err) {
            sh_printf(sh, "reg: %s was not found\n", name ? name : argv[2]);
            return E_NOENT;
        }
        w32_registry_flush();
        sh_puts(sh, "The operation completed successfully.\n");
        return 0;
    }
    return command_usage(sh, command_find("reg"));
}
