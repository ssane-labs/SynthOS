#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/input.h>
#include <synth/omaha.h>
#include <synth/path.h>
#include <synth/sha256.h>
#include <synth/shell.h>
#include <synth/store.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>
#include <synth/zip.h>
#if CONFIG_WIN32
#include <synth/winshell.h>
#endif

/* The catalog is a text file of [id] blocks with key = value lines. The one in the image is the fallback; a
   refreshed copy from the SynthOS releases lives in /home/etc/store. What the store installed is listed in
   /home/etc/store/installed (id and version per line), so it can offer updates. */

#define BUILTIN_CATALOG "/system/store/catalog.txt"
#define STORE_DIR "/home/etc/store"
#define CACHED_CATALOG STORE_DIR "/catalog.txt"
#define INSTALLED STORE_DIR "/installed"
#define APPS_DIR "/home/apps"
#define MAX_APPS 48
#define DEFAULT_URL "https://github.com/ssane-labs/synthos/releases/latest/download/"

static struct store_app *apps;
static int count;
static char store_url[256] = DEFAULT_URL;

static char *trim(char *s)
{
    while (is_space(*s))
        s++;
    size_t n = strlen(s);
    while (n && is_space(s[n - 1]))
        s[--n] = 0;
    return s;
}

static void set_field(struct store_app *a, const char *key, const char *v)
{
    static const struct {
        const char *key;
        size_t offset, size;
    } fields[] = {
#define F(name) {#name, offsetof(struct store_app, name), sizeof(((struct store_app *)0)->name)}
        F(name),    F(developer), F(category), F(icon), F(version), F(size),  F(summary), F(about),
        F(note),    F(sha256),    F(file),     F(args), F(check),   F(open),  F(bin),     F(post),
        F(strip),   F(uninstall), F(inner),     F(extra), F(extra_to),
#undef F
    };
    for (size_t i = 0; i < ARRAY_SIZE(fields); i++)
        if (!strcmp(key, fields[i].key)) {
            str_copy((char *)a + fields[i].offset, v, fields[i].size);
            return;
        }
    if (!strcmp(key, "url"))
        str_copy(a->url[a->url[0][0] ? 1 : 0], v, sizeof(a->url[0]));
    else if (!strcmp(key, "kind"))
        a->kind = !strcmp(v, "installer") ? STORE_INSTALLER
                  : !strcmp(v, "chrome")  ? STORE_CHROME
                  : !strcmp(v, "builtin") ? STORE_BUILTIN
                                          : STORE_ZIP;
    else if (!strcmp(key, "bits"))
        a->bits = (int)str_to_long(v, NULL, 10);
}

/* Parses a catalog into out (up to max apps); returns how many it has. */
static int parse(char *text, struct store_app *out, int max)
{
    int n = 0;
    struct store_app *a = NULL;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *hash = line[0] == '#' ? line : NULL;
        if (hash)
            *hash = 0;
        char *l = trim(line);
        line = next;
        if (*l == '[') {
            char *end = strchr(l, ']');
            a = NULL;
            if (!end || n == max)
                continue;
            *end = 0;
            a = &out[n++];
            memset(a, 0, sizeof(*a));
            str_copy(a->id, l + 1, sizeof(a->id));
            continue;
        }
        char *eq = strchr(l, '=');
        if (!a || !eq)
            continue;
        *eq = 0;
        char *v = trim(eq + 1);
        for (char *p = v; (p = strstr(p, "\\n"));)
            p[0] = '\n', memmove(p + 1, p + 2, strlen(p + 2) + 1);
        set_field(a, trim(l), v);
    }
    int kept = 0;
    for (int i = 0; i < n; i++)
        if (out[i].id[0] && out[i].name[0] && (out[i].kind == STORE_BUILTIN || out[i].kind == STORE_CHROME ||
                                               out[i].url[0][0]))
            out[kept++] = out[i];
    return kept;
}

static int load_file(const char *path)
{
    char *text;
    size_t size;
    if (vfs_read_all(path, &text, &size))
        return 0;
    int n = parse(text, apps, MAX_APPS);
    kfree(text);
    return n;
}

int store_load(void)
{
    if (!apps && !(apps = kmalloc(sizeof(*apps) * MAX_APPS)))
        return E_NOMEM;
    count = load_file(CACHED_CATALOG);
    if (!count)
        count = load_file(BUILTIN_CATALOG);
    return count;
}

int store_count(void)
{
    if (!apps)
        store_load();
    return count;
}

const struct store_app *store_app(int index)
{
    return index >= 0 && index < store_count() ? &apps[index] : NULL;
}

const struct store_app *store_find(const char *id)
{
    for (int i = 0; i < store_count(); i++)
        if (!str_casecmp(apps[i].id, id))
            return &apps[i];
    return NULL;
}

int store_categories(const char *out[], int max)
{
    int n = 0;
    for (int i = 0; i < store_count(); i++) {
        bool seen = false;
        for (int k = 0; k < n; k++)
            seen |= !strcmp(out[k], apps[i].category);
        if (!seen && n < max && apps[i].category[0])
            out[n++] = apps[i].category;
    }
    return n;
}

/* --- what is installed --- */

static void app_dir(const struct store_app *a, char *out, size_t cap)
{
    path_join(APPS_DIR, a->id, out, cap);
}

/* The program that starts the app, as a SynthOS path. */
bool store_program(const struct store_app *a, char *out, size_t cap)
{
    const char *p = a->open[0] ? a->open : a->check;
    if (!p[0])
        return false;
    if (a->kind == STORE_ZIP && !strchr(p, ':') && p[0] != '/') {
        char dir[PATH_MAX_LEN];
        app_dir(a, dir, sizeof(dir));
        path_join(dir, p, out, cap);
    } else if (strchr(p, ':')) {
        if (winpath_to_vfs("/", p, out, cap) != 0)
            return false;
    } else {
        str_copy(out, p, cap);
    }
    return vfs_exists(out);
}

static bool present(const struct store_app *a)
{
    char path[PATH_MAX_LEN];
    if (a->check[0]) {
        if (strchr(a->check, ':'))
            return winpath_to_vfs("/", a->check, path, sizeof(path)) == 0 && vfs_exists(path);
        if (a->check[0] == '/')
            return vfs_exists(a->check);
        app_dir(a, path, sizeof(path));
        char full[PATH_MAX_LEN];
        path_join(path, a->check, full, sizeof(full));
        return vfs_exists(full);
    }
    app_dir(a, path, sizeof(path));
    return vfs_is_dir(path);
}

static bool recorded(const struct store_app *a, char *version, size_t cap)
{
    char *text;
    size_t size;
    if (vfs_read_all(INSTALLED, &text, &size))
        return false;
    bool found = false;
    size_t n = strlen(a->id);
    for (char *l = text; l && *l && !found;) {
        char *next = strchr(l, '\n');
        if (next)
            *next++ = 0;
        if (!strncmp(l, a->id, n) && l[n] == ' ') {
            str_copy(version, l + n + 1, cap);
            found = true;
        }
        l = next;
    }
    kfree(text);
    return found;
}

static void record(const struct store_app *a, bool installed)
{
    char *text = NULL;
    size_t size = 0;
    vfs_read_all(INSTALLED, &text, &size);
    struct stream *out = stream_buffer();
    if (!out) {
        kfree(text);
        return;
    }
    size_t n = strlen(a->id);
    for (char *l = text; l && *l;) {
        char *next = strchr(l, '\n');
        if (next)
            *next++ = 0;
        if (*l && !(!strncmp(l, a->id, n) && l[n] == ' '))
            stream_printf(out, "%s\n", l);
        l = next;
    }
    if (installed)
        stream_printf(out, "%s %s\n", a->id, a->version[0] ? a->version : "-");
    size_t len;
    const char *data = stream_buffer_data(out, &len);
    vfs_mkdirs(STORE_DIR);
    vfs_write_all(INSTALLED, data, len);
    stream_close(out);
    kfree(text);
}

static bool fits(const struct store_app *a)
{
#if defined(__x86_64__)
    UNUSED(a);
    return true;
#else
    return a->bits != 64;
#endif
}

enum store_state store_state(const struct store_app *a)
{
    if (!present(a))
        return a->kind == STORE_BUILTIN || !fits(a) ? STORE_UNAVAILABLE : STORE_GET;
    char version[24];
    if (a->kind == STORE_ZIP && a->version[0] && recorded(a, version, sizeof(version)) && strcmp(version, a->version))
        return STORE_UPDATE;
    return STORE_INSTALLED;
}

/* --- downloads --- */

struct fetch {
    const struct store_ui *ui;
    struct stream *file;
    struct sha256 hash;
    int err;
};

static int to_file(void *ctx, const void *data, size_t n)
{
    struct fetch *f = ctx;
    sha256_update(&f->hash, data, n);
    if (stream_write(f->file, data, n) != (long)n)
        return f->err = E_NOSPC;
    return 0;
}

static void file_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    struct fetch *f = ctx;
    f->ui->progress(f->ui->ctx, stage, done, total);
}

static void say(const struct store_ui *ui, const char *fmt, const char *arg)
{
    if (ui && ui->say) {
        char text[200];
        ksnprintf(text, sizeof(text), fmt, arg);
        ui->say(ui->ctx, text);
    }
}

static bool hash_ok(const struct store_app *a, const uint8_t digest[32])
{
    if (!a->sha256[0])
        return true;
    char hex[65];
    for (int i = 0; i < 32; i++)
        ksnprintf(hex + i * 2, 3, "%02x", digest[i]);
    return !str_casecmp(hex, a->sha256);
}

/* Downloads the app, into memory (body) or into the file path; checks its SHA-256 when the catalog gives one. */
static int download(const struct store_app *a, const struct store_ui *ui, const char *path, struct http_response *resp,
                    char *msg, size_t cap)
{
    int err = E_NOENT;
    for (int i = 0; i < 2 && a->url[i][0]; i++) {
        say(ui, "Downloading %s", a->url[i]);
        struct fetch f = {.ui = ui};
        sha256_init(&f.hash);
        if (path && !(f.file = stream_file(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &err))) {
            ksnprintf(msg, cap, "%s: %s", path, err_str(err));
            return err;
        }
        struct http_request req = {.url = a->url[i], .max_size = 768u << 20, .flags = HTTP_NO_COOKIES,
                                   .cancelled = ui ? ui->cancelled : NULL, .progress = ui ? ui->progress : NULL,
                                   .ctx = ui ? ui->ctx : NULL, .sink = path ? to_file : NULL};
        if (path)
            req.ctx = &f, req.progress = ui && ui->progress ? file_progress : NULL;
        err = http_fetch(&req, resp);
        if (f.file)
            stream_close(f.file);
        if (!err && resp->status == 200 && !f.err) {
            uint8_t digest[32];
            if (!path)
                sha256(resp->body, resp->length, digest);
            else
                sha256_final(&f.hash, digest);
            if (hash_ok(a, digest))
                return 0;
            http_free(resp);
            if (path)
                vfs_unlink(path);
            ksnprintf(msg, cap, "the download of %s is damaged (its checksum does not match)", a->name);
            return E_IO;
        }
        if (!err) {
            ksnprintf(msg, cap, "the server answered %d", resp->status);
            err = f.err ? f.err : E_NOENT;
            http_free(resp);
        } else {
            ksnprintf(msg, cap, "%s", tls_strerror(err));
        }
        if (path)
            vfs_unlink(path);
        if (ui && ui->cancelled && ui->cancelled())
            break;
    }
    return err;
}

struct unpack {
    const struct store_app *a;
    const struct store_ui *ui;
    const uint8_t *data;
    size_t len;
    const char *dir;
    int files, total, err;
};

static bool safe_name(const char *name)
{
    if (name[0] == '/' || strchr(name, ':'))
        return false;
    for (const char *p = name; *p; p++)
        if ((p == name || p[-1] == '/') && p[0] == '.' && p[1] == '.' && (p[2] == '/' || !p[2]))
            return false;
    return true;
}

static int count_entry(const struct zip_entry *e, void *ctx)
{
    UNUSED(e);
    ((struct unpack *)ctx)->total++;
    return 0;
}

static int unpack_entry(const struct zip_entry *e, void *ctx)
{
    struct unpack *u = ctx;
    char name[PATH_MAX_LEN];
    size_t n = MIN(e->name_len, sizeof(name) - 1);
    memcpy(name, e->name, n);
    name[n] = 0;
    for (char *p = name; *p; p++)
        if (*p == '\\')
            *p = '/';
    const char *rel = name;
    if (u->a->strip[0] && str_starts(rel, u->a->strip))
        rel += strlen(u->a->strip);
    while (*rel == '/')
        rel++;
    u->files++;
    if (u->ui && u->ui->progress && (u->files % 16 == 0 || u->files == u->total))
        u->ui->progress(u->ui->ctx, "Unpacking", (size_t)u->files, (size_t)u->total);
    if (!*rel || !safe_name(rel))
        return 0;
    char path[PATH_MAX_LEN];
    path_join(u->dir, rel, path, sizeof(path));
    if (rel[strlen(rel) - 1] == '/')
        return vfs_mkdirs(path) == 0 || vfs_is_dir(path) ? 0 : E_IO;
    char parent[PATH_MAX_LEN];
    str_copy(parent, path, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (!vfs_is_dir(parent))
            vfs_mkdirs(parent);
    }
    uint8_t *data;
    int err = zip_extract(u->data, u->len, e, &data);
    if (!err) {
        err = vfs_write_all(path, data, e->usize);
        kfree(data);
    }
    if (err)
        u->err = err;
    if (u->ui && u->ui->cancelled && u->ui->cancelled())
        return E_INTR;
    return err;
}

/* extra = a second zip the program needs (OpenTTD's free graphics), unpacked as it is into extra_to */
static int install_extra(const struct store_app *a, const struct store_ui *ui, const char *dir, char *msg, size_t cap)
{
    struct store_app *x = kmalloc(sizeof(*x));
    if (!x)
        return E_NOMEM;
    *x = *a;
    str_copy(x->url[0], a->extra, sizeof(x->url[0]));
    x->url[1][0] = x->sha256[0] = x->strip[0] = 0;
    struct http_response resp;
    int err = download(x, ui, NULL, &resp, msg, cap);
    if (!err) {
        char sub[PATH_MAX_LEN];
        path_join(dir, a->extra_to[0] ? a->extra_to : ".", sub, sizeof(sub));
        vfs_mkdirs(sub);
        struct unpack u = {x, ui, (const uint8_t *)resp.body, resp.length, sub, 0, 0, 0};
        zip_each(u.data, u.len, count_entry, &u);
        err = u.total ? zip_each(u.data, u.len, unpack_entry, &u) : E_INVAL;
        http_free(&resp);
    }
    kfree(x);
    return err;
}

static int install_zip(const struct store_app *a, const struct store_ui *ui, char *msg, size_t cap)
{
    struct http_response resp;
    int err = download(a, ui, NULL, &resp, msg, cap);
    if (err)
        return err;
    char dir[PATH_MAX_LEN];
    app_dir(a, dir, sizeof(dir));
    if (vfs_is_dir(dir))
        vfs_remove_tree(dir);
    vfs_mkdirs(dir);
    say(ui, "Unpacking into %s", dir);
    if (a->file[0] && (resp.length < 2 || memcmp(resp.body, "PK", 2))) {
        /* a single program, not an archive: kept under its own name */
        char path[PATH_MAX_LEN];
        path_join(dir, a->file, path, sizeof(path));
        err = vfs_write_all(path, resp.body, resp.length);
    } else {
        struct unpack u = {a, ui, (const uint8_t *)resp.body, resp.length, dir, 0, 0, 0};
        zip_each(u.data, u.len, count_entry, &u);
        err = u.total ? zip_each(u.data, u.len, unpack_entry, &u) : E_INVAL;
    }
    http_free(&resp);
    msg[0] = 0;
    if (!err && a->extra[0])
        err = install_extra(a, ui, dir, msg, cap);
    if (err) {
        if (!msg[0])
            ksnprintf(msg, cap, "%s", err == E_INVAL ? "the download is not a zip archive" : err_str(err));
        vfs_remove_tree(dir);
        return err;
    }
    if (!strcmp(a->post, "python"))
        pkg_python_site(NULL, dir);
    char bin[PATH_MAX_LEN];
    if (a->bin[0]) {
        path_join(dir, a->bin, bin, sizeof(bin));
        pkg_add_path(shell_current(), !strcmp(a->bin, ".") ? dir : bin);
    }
    record(a, true);
    ksnprintf(msg, cap, "%s %s is installed in %s", a->name, a->version, dir);
    return 0;
}

struct inner_find {
    const char *name;
    const struct zip_entry *found;
    struct zip_entry entry;
};

static int find_inner(const struct zip_entry *e, void *ctx)
{
    struct inner_find *f = ctx;
    size_t n = strlen(f->name);
    const char *base = e->name;
    for (size_t i = 0; i < e->name_len; i++)
        if (e->name[i] == '/' || e->name[i] == '\\')
            base = e->name + i + 1;
    if ((size_t)(e->name + e->name_len - base) == n && !str_ncasecmp(base, f->name, n)) {
        f->entry = *e;
        f->found = &f->entry;
        return 1;
    }
    return 0;
}

/* the vendor ships its installer inside a zip: unpack that one file next to the downloads */
static int unpack_inner(const struct store_app *a, const struct store_ui *ui, const char *path, char *msg, size_t cap)
{
    struct http_response resp;
    int err = download(a, ui, NULL, &resp, msg, cap);
    if (err)
        return err;
    struct inner_find f = {a->inner, NULL, {0}};
    zip_each((const uint8_t *)resp.body, resp.length, find_inner, &f);
    uint8_t *data = NULL;
    err = f.found ? zip_extract((const uint8_t *)resp.body, resp.length, f.found, &data) : E_NOENT;
    if (!err) {
        err = vfs_write_all(path, data, f.found->usize);
        kfree(data);
    }
    http_free(&resp);
    if (err)
        ksnprintf(msg, cap, "%s is not in the download of %s", a->inner, a->name);
    return err;
}

static int install_runner(const struct store_app *a, const struct store_ui *ui, char *msg, size_t cap)
{
    char dir[PATH_MAX_LEN], path[PATH_MAX_LEN];
    path_join(user_home(), "Downloads", dir, sizeof(dir));
    vfs_mkdirs(dir);
    path_join(dir, a->inner[0] ? a->inner : a->file[0] ? a->file : "setup.exe", path, sizeof(path));
    int err;
    if (a->inner[0]) {
        err = unpack_inner(a, ui, path, msg, cap);
    } else {
        struct http_response resp;
        err = download(a, ui, path, &resp, msg, cap);
        if (!err)
            http_free(&resp);
    }
    if (err)
        return err;
    record(a, true);
    if (ui && ui->run) {
        if (!ui->run(ui->ctx, path, a->args)) {
            ksnprintf(msg, cap, "the installer of %s did not start", a->name);
            return E_IO;
        }
        ksnprintf(msg, cap, "The installer of %s is running", a->name);
    } else {
        str_copy(msg, path, cap);
    }
    return 0;
}

static void omaha_say(void *ctx, const char *text)
{
    const struct store_ui *ui = ctx;
    if (ui->say)
        ui->say(ui->ctx, text);
}

static void omaha_progress(void *ctx, const char *stage, uint64_t done, uint64_t total)
{
    const struct store_ui *ui = ctx;
    if (ui->progress)
        ui->progress(ui->ctx, stage, (size_t)MIN(done, (uint64_t)SIZE_MAX), (size_t)MIN(total, (uint64_t)SIZE_MAX));
}

static bool omaha_cancelled(void *ctx)
{
    const struct store_ui *ui = ctx;
    return ui->cancelled && ui->cancelled();
}

int store_install(const struct store_app *a, const struct store_ui *ui, char *msg, size_t cap)
{
    msg[0] = 0;
    if (!fits(a)) {
        ksnprintf(msg, cap, "%s needs the 64-bit SynthOS", a->name);
        return E_NOSYS;
    }
    if (a->kind == STORE_BUILTIN) {
        ksnprintf(msg, cap, "%s comes with SynthOS", a->name);
        return present(a) ? 0 : E_NOENT;
    }
    if (a->kind == STORE_CHROME) {
        static const struct store_ui none;
        struct omaha_ui o = {.say = omaha_say, .progress = omaha_progress, .cancelled = omaha_cancelled,
                             .ctx = (void *)(ui ? ui : &none)};
        int err = chrome_install(&o);
        str_copy(msg, err ? o.error : "Google Chrome is installed; the shortcut is on the desktop", cap);
        if (!err)
            record(a, true);
        return err;
    }
    return a->kind == STORE_INSTALLER ? install_runner(a, ui, msg, cap) : install_zip(a, ui, msg, cap);
}

#if CONFIG_WIN32
struct find_program {
    const char *name;
    char *out;
    size_t cap;
};

static void find_uninstall(const struct winshell_program *p, void *ctx)
{
    struct find_program *f = ctx;
    if (!f->out[0] && p->uninstall[0] && str_starts(p->name, f->name))
        str_copy(f->out, p->uninstall, f->cap);
}
#endif

/* Removes what the store put in /home/apps; for an app with its own installer, gives back the command of its
   uninstaller in uninstall (the caller runs it). */
int store_remove(const struct store_app *a, char *uninstall, size_t ucap, char *msg, size_t cap)
{
    uninstall[0] = 0;
    if (a->kind == STORE_BUILTIN) {
        ksnprintf(msg, cap, "%s is part of SynthOS", a->name);
        return E_PERM;
    }
    if (a->kind == STORE_INSTALLER) {
#if CONFIG_WIN32
        struct find_program f = {a->uninstall[0] ? a->uninstall : a->name, uninstall, ucap};
        winshell_programs(find_uninstall, &f);
#endif
        record(a, false);
        if (!uninstall[0]) {
            ksnprintf(msg, cap, "%s has no uninstaller; remove it in Settings > Apps", a->name);
            return E_NOENT;
        }
        ksnprintf(msg, cap, "Starting the uninstaller of %s", a->name);
        return 0;
    }
    char dir[PATH_MAX_LEN];
    if (a->kind == STORE_CHROME)
        str_copy(dir, APPS_DIR "/chrome", sizeof(dir));
    else
        app_dir(a, dir, sizeof(dir));
    int err = vfs_is_dir(dir) ? vfs_remove_tree(dir) : E_NOENT;
    record(a, false);
    if (err)
        ksnprintf(msg, cap, "%s: %s", dir, err_str(err));
    else
        ksnprintf(msg, cap, "Removed %s", a->name);
    return err;
}

int store_refresh(const struct store_ui *ui, char *msg, size_t cap)
{
    char url[320];
    ksnprintf(url, sizeof(url), "%s%sstore-catalog.txt", store_url,
              store_url[0] && store_url[strlen(store_url) - 1] == '/' ? "" : "/");
    struct http_request req = {.url = url, .max_size = 1u << 20, .flags = HTTP_NO_COOKIES,
                               .cancelled = ui ? ui->cancelled : NULL, .progress = ui ? ui->progress : NULL,
                               .ctx = ui ? ui->ctx : NULL};
    struct http_response resp;
    int err = http_fetch(&req, &resp);
    if (err) {
        ksnprintf(msg, cap, "%s", tls_strerror(err));
        return err;
    }
    if (resp.status != 200) {
        ksnprintf(msg, cap, "the store answered %d", resp.status);
        http_free(&resp);
        return E_NOENT;
    }
    struct store_app *check = kmalloc(sizeof(*check) * MAX_APPS);
    char *copy = kmalloc(resp.length + 1);
    int n = 0;
    if (check && copy) {
        memcpy(copy, resp.body, resp.length);
        copy[resp.length] = 0;
        n = parse(copy, check, MAX_APPS);
    }
    kfree(check);
    kfree(copy);
    if (n <= 0) {
        http_free(&resp);
        ksnprintf(msg, cap, "the catalog from the store is empty or damaged");
        return E_INVAL;
    }
    vfs_mkdirs(STORE_DIR);
    err = vfs_write_all(CACHED_CATALOG, resp.body, resp.length);
    http_free(&resp);
    if (err) {
        ksnprintf(msg, cap, "%s: %s", CACHED_CATALOG, err_str(err));
        return err;
    }
    store_load();
    ksnprintf(msg, cap, "The catalog has %d apps", count);
    return 0;
}

static void get_url(char *buf, size_t cap)
{
    str_copy(buf, strcmp(store_url, DEFAULT_URL) ? store_url : "default", cap);
}

static int set_url(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "default") && !str_starts(value, "https://") && !str_starts(value, "http://"))
        return E_INVAL;
    str_copy(store_url, strcmp(value, "default") ? value : DEFAULT_URL, sizeof(store_url));
    return 0;
}

static const struct setting s_storeurl = {"storeurl", "default|<url>", "where the App Store gets its catalog", get_url,
                                          set_url};
SETTING(s_storeurl);

/* --- the store command --- */

static void cmd_say(void *ctx, const char *text)
{
    sh_printf(ctx, "%s\n", text);
}

static void cmd_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    static size_t last;
    if (!done || !total) {
        last = 0;
        return;
    }
    if (done - last < (4u << 20) && done != total && strcmp(stage, "Unpacking"))
        return;
    if (!strcmp(stage, "Unpacking") && done != total)
        return;
    last = done;
    char a[16], b[16];
    if (!strcmp(stage, "Unpacking")) {
        sh_printf(ctx, "  unpacked %u files\n", (unsigned)total);
        return;
    }
    fmt_size(a, sizeof(a), done);
    fmt_size(b, sizeof(b), total);
    sh_printf(ctx, "\r  downloaded %s of %s (%u%%)%s", a, b, (unsigned)((uint64_t)done * 100 / total),
              done == total ? "\n" : "");
}

static bool cmd_cancelled(void)
{
    return input_break_pending();
}

static const char *state_name(enum store_state s)
{
    return s == STORE_INSTALLED ? "installed" : s == STORE_UPDATE ? "update" : s == STORE_UNAVAILABLE ? "-" : "get";
}

COMMAND(store, "appstore", "system",
        "store [list | info <app> | get <app> | remove <app> | open <app> | refresh]",
        "the App Store: programs that run in SynthOS, installed in one step")
{
    const char *op = argc > 1 ? argv[1] : "list";
    store_load();
    if (!strcmp(op, "list")) {
        int n = store_count();
        for (int i = 0; i < n; i++) {
            const struct store_app *a = store_app(i);
            sh_role(sh, ROLE_ACCENT);
            sh_printf(sh, "%-10s", a->id);
            sh_role(sh, ROLE_MUTED);
            sh_printf(sh, " %-9s %-16s ", state_name(store_state(a)), a->category);
            sh_role(sh, ROLE_TEXT);
            sh_printf(sh, "%s\n", a->summary);
        }
        if (!n)
            sh_puts(sh, "the catalog is empty (store refresh gets it)\n");
        return 0;
    }
    if (!strcmp(op, "refresh")) {
        char msg[160];
        struct store_ui ui = {.cancelled = cmd_cancelled, .ctx = sh};
        int err = store_refresh(&ui, msg, sizeof(msg));
        if (err)
            return sh_error(sh, "store", "%s", msg);
        sh_printf(sh, "%s\n", msg);
        return 0;
    }
    if (argc != 3)
        return command_usage(sh, command_find("store"));
    const struct store_app *a = store_find(argv[2]);
    if (!a)
        return sh_error(sh, "store", "no app called %s (store list shows them)", argv[2]);
    char msg[200];
    if (!strcmp(op, "info")) {
        sh_role(sh, ROLE_ACCENT);
        sh_printf(sh, "%s", a->name);
        sh_role(sh, ROLE_TEXT);
        sh_printf(sh, " %s by %s (%s, %s)\n%s\n", a->version, a->developer, a->category, state_name(store_state(a)),
                  a->about[0] ? a->about : a->summary);
        if (a->note[0])
            sh_printf(sh, "Note: %s\n", a->note);
        return 0;
    }
    if (!strcmp(op, "get") || !strcmp(op, "install")) {
        struct store_ui ui = {.progress = cmd_progress, .say = cmd_say, .cancelled = cmd_cancelled, .ctx = sh};
        int err = store_install(a, &ui, msg, sizeof(msg));
        if (err)
            return sh_error(sh, "store", "%s", msg);
        if (a->kind == STORE_INSTALLER) {
            char line[PATH_MAX_LEN + 96];
            sh_printf(sh, "Running the installer %s %s\n", msg, a->args);
            ksnprintf(line, sizeof(line), "'%s' %s", msg, a->args);
            err = shell_exec(sh, line);
            if (err || !present(a))
                return sh_error(sh, "store", "the installer of %s did not finish", a->name);
            ksnprintf(msg, sizeof(msg), "%s is installed", a->name);
        }
        sh_role(sh, ROLE_OK);
        sh_printf(sh, "%s\n", msg);
        sh_role(sh, ROLE_TEXT);
        return 0;
    }
    if (!strcmp(op, "remove")) {
        char uninstall[384];
        int err = store_remove(a, uninstall, sizeof(uninstall), msg, sizeof(msg));
        if (err)
            return sh_error(sh, "store", "%s", msg);
        sh_printf(sh, "%s\n", msg);
        if (uninstall[0]) {
            char path[PATH_MAX_LEN], line[640];
            const char *rest = uninstall;
            if (uninstall[0] == '"') {
                const char *end = strchr(uninstall + 1, '"');
                size_t n = end ? (size_t)(end - uninstall - 1) : strlen(uninstall + 1);
                memcpy(line, uninstall + 1, MIN(n, sizeof(line) - 1));
                line[MIN(n, sizeof(line) - 1)] = 0;
                rest = end ? end + 1 : "";
            } else {
                str_copy(line, uninstall, sizeof(line));
                char *sp = strchr(line, ' ');
                rest = "";
                if (sp) {
                    *sp = 0;
                    rest = uninstall + (sp - line);
                }
            }
            if (winpath_to_vfs("/", line, path, sizeof(path)) != 0)
                str_copy(path, line, sizeof(path));
            ksnprintf(line, sizeof(line), "'%s'%s", path, rest);
            return shell_exec(sh, line);
        }
        return 0;
    }
    if (!strcmp(op, "open")) {
        char path[PATH_MAX_LEN];
        if (!store_program(a, path, sizeof(path)))
            return sh_error(sh, "store", "%s is not installed", a->name);
        char line[PATH_MAX_LEN + 8];
        ksnprintf(line, sizeof(line), "'%s'", path);
        return shell_exec(sh, line);
    }
    return command_usage(sh, command_find("store"));
}
