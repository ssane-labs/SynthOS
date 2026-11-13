#include <synth/command.h>
#include <synth/error.h>
#include <synth/lnk.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/picture.h>
#include <synth/vfs.h>
#include <synth/winpath.h>
#include <synth/zip.h>

#include "systool.h"

/* App Installer: MSIX and AppX packages. A package is a ZIP with AppxManifest.xml at the top; a bundle
   (.msixbundle) holds one package per processor and AppxMetadata/AppxBundleManifest.xml naming them. Desktop
   programs packaged this way (the manifest's "full trust" entry point) are unpacked into
   C:\Program Files\WindowsApps\<name>_<version> and get a Start Menu shortcut per application, so they show up in
   Launchpad like any installed program. Universal Windows apps need WinRT and are turned down with a clear
   reason. 'appx' installs, lists and removes; opening a package file shows the installer window. */

#define APPS_DIR "C:\\Program Files\\WindowsApps"
#define MENU_DIR "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"
#define RECORD ".synth-appx"
#define MAX_APPS 8

struct app_entry {
    char id[64], exe[192], name[96];
};

struct package {
    const uint8_t *data; /* the package itself (inside a bundle: the chosen inner package) */
    size_t len;
    uint8_t *inner;      /* owned copy of the inner package of a bundle */
    char name[96], version[32], arch[16], publisher[160], display[96], logo[160];
    struct app_entry apps[MAX_APPS];
    int napps;
    bool uwp; /* has applications none of which is a desktop program */
};

struct find {
    const char *want;
    bool prefix;
    struct zip_entry hit;
    bool found;
};

/* OPC part names are percent-encoded ("My%20App.exe") */
static void unescape(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (in[i] == '%' && i + 2 < n && is_xdigit((uint8_t)in[i + 1]) && is_xdigit((uint8_t)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)str_to_long(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i] == '\\' ? '/' : in[i];
        }
    }
    out[o] = 0;
}

static int find_fn(const struct zip_entry *e, void *ctx)
{
    struct find *f = ctx;
    char name[PATH_MAX_LEN];
    unescape(e->name, e->name_len, name, sizeof(name));
    size_t k = strlen(f->want);
    if (f->prefix ? !str_ncasecmp(name, f->want, k) : !str_casecmp(name, f->want)) {
        f->hit = *e;
        f->found = true;
        return 1;
    }
    return 0;
}

/* the named part, NUL-terminated; kfree it */
static char *part(const uint8_t *data, size_t len, const char *name, bool prefix, size_t *size)
{
    struct find f = {name, prefix, {0}, false};
    zip_each(data, len, find_fn, &f);
    uint8_t *out;
    if (!f.found || f.hit.usize > (64u << 20) || zip_extract(data, len, &f.hit, &out) != 0)
        return NULL;
    char *text = kmalloc(f.hit.usize + 1);
    if (text) {
        memcpy(text, out, f.hit.usize);
        text[f.hit.usize] = 0;
    }
    kfree(out);
    if (size)
        *size = f.hit.usize;
    return text;
}

/* the next <tag ...> or <prefix:tag ...> at or after p, before end */
static const char *tag(const char *p, const char *end, const char *name)
{
    size_t k = strlen(name);
    for (; p && p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL; p++) {
        const char *n = p + 1;
        const char *colon = n;
        while (colon < end && (is_alnum((uint8_t)*colon) || *colon == '_'))
            colon++;
        if (colon < end && *colon == ':')
            n = colon + 1;
        if (n + k < end && !strncmp(n, name, k) && (n[k] == ' ' || n[k] == '>' || n[k] == '/' || n[k] == '\n' ||
                                                     n[k] == '\r' || n[k] == '\t'))
            return p;
    }
    return NULL;
}

/* the few entities a name can carry */
static void entities(char *out)
{
    static const char *const ents[][2] = {{"&amp;", "&"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"},
                                           {"&gt;", ">"}};
    for (char *a = out; *a; a++)
        for (size_t i = 0; i < ARRAY_SIZE(ents); i++)
            if (!strncmp(a, ents[i][0], strlen(ents[i][0]))) {
                size_t l = strlen(ents[i][0]);
                *a = ents[i][1][0];
                memmove(a + 1, a + l, strlen(a + l) + 1);
            }
}

/* attribute of the tag at t (any namespace prefix on the attribute is allowed) */
static bool attr(const char *t, const char *name, char *out, size_t cap)
{
    out[0] = 0;
    if (!t)
        return false;
    const char *close = strchr(t, '>');
    size_t k = strlen(name);
    for (const char *p = t; close && p < close; p++) {
        if (p[-1] != ' ' && p[-1] != ':' && p[-1] != '\n' && p[-1] != '\t' && p[-1] != '\r')
            continue;
        if (strncmp(p, name, k) || p[k] != '=' || (p[k + 1] != '"' && p[k + 1] != '\''))
            continue;
        if (p[-1] == ':') { /* only accept a prefix when it is a whole word before the colon */
            const char *s = p - 1;
            while (s > t && is_alnum((uint8_t)s[-1]))
                s--;
            if (s[-1] != ' ' && s[-1] != '\n' && s[-1] != '\t')
                continue;
        }
        char q = p[k + 1];
        const char *v = p + k + 2, *e = strchr(v, q);
        if (!e)
            return false;
        str_copy(out, v, MIN(cap, (size_t)(e - v) + 1));
        entities(out);
        return true;
    }
    return false;
}

/* the text between <tag> and </ */
static void text_of(const char *t, char *out, size_t cap)
{
    out[0] = 0;
    const char *s = t ? strchr(t, '>') : NULL;
    const char *e = s ? strstr(s, "</") : NULL;
    if (s && e && s[-1] != '/')
        str_copy(out, s + 1, MIN(cap, (size_t)(e - s)));
    entities(out);
}

static const char *my_arch(void)
{
#ifdef __x86_64__
    return "x64";
#else
    return "x86";
#endif
}

static bool arch_ok(const char *a)
{
    return !a[0] || !str_casecmp(a, "neutral") || !str_casecmp(a, my_arch()) ||
           (!str_casecmp(a, "x86") && !strcmp(my_arch(), "x64"));
}

/* a bundle: pick the inner package for this processor (x64 first, then x86, then neutral) */
static int open_bundle(struct package *p, const char *manifest)
{
    const char *end = manifest + strlen(manifest), *best = NULL;
    int best_rank = 0;
    for (const char *t = tag(manifest, end, "Package"); t; t = tag(t + 1, end, "Package")) {
        char type[24], a[16];
        attr(t, "Type", type, sizeof(type));
        attr(t, "Architecture", a, sizeof(a));
        if (str_casecmp(type, "application") || !arch_ok(a))
            continue;
        int rank = !str_casecmp(a, my_arch()) ? 3 : !str_casecmp(a, "x86") ? 2 : 1;
        if (rank > best_rank)
            best = t, best_rank = rank;
    }
    char file[160];
    if (!best || !attr(best, "FileName", file, sizeof(file)))
        return E_NOENT;
    size_t n;
    char *inner = part(p->data, p->len, file, false, &n);
    if (!inner)
        return E_NOENT;
    p->inner = (uint8_t *)inner;
    p->data = p->inner;
    p->len = n;
    return 0;
}

static void package_free(struct package *p)
{
    kfree(p->inner);
    p->inner = NULL;
}

/* reads the manifest; E_NOENT: no manifest, E_NOSYS: nothing for this processor */
static int package_read(struct package *p, const uint8_t *data, size_t len)
{
    memset(p, 0, sizeof(*p));
    p->data = data;
    p->len = len;
    char *m = part(data, len, "AppxManifest.xml", false, NULL);
    if (!m) {
        char *b = part(data, len, "AppxMetadata/AppxBundleManifest.xml", false, NULL);
        int err = b ? open_bundle(p, b) : E_NOENT;
        kfree(b);
        if (err)
            return b ? E_NOSYS : E_NOENT;
        m = part(p->data, p->len, "AppxManifest.xml", false, NULL);
        if (!m)
            return E_NOENT;
    }
    const char *end = m + strlen(m);
    const char *id = tag(m, end, "Identity");
    attr(id, "Name", p->name, sizeof(p->name));
    attr(id, "Version", p->version, sizeof(p->version));
    attr(id, "ProcessorArchitecture", p->arch, sizeof(p->arch));
    attr(id, "Publisher", p->publisher, sizeof(p->publisher));
    const char *props = tag(m, end, "Properties");
    const char *props_end = props ? strstr(props + 12, "Properties>") : NULL;
    if (props && props_end) {
        text_of(tag(props, props_end, "DisplayName"), p->display, sizeof(p->display));
        text_of(tag(props, props_end, "Logo"), p->logo, sizeof(p->logo));
        char pub[96];
        text_of(tag(props, props_end, "PublisherDisplayName"), pub, sizeof(pub));
        if (pub[0] && strncmp(pub, "ms-resource:", 12))
            str_copy(p->publisher, pub, sizeof(p->publisher));
    }
    if (!p->display[0] || !strncmp(p->display, "ms-resource:", 12))
        str_copy(p->display, p->name, sizeof(p->display));
    int found = 0;
    for (const char *t = tag(m, end, "Application"); t && p->napps < MAX_APPS; t = tag(t + 1, end, "Application")) {
        found++;
        struct app_entry *a = &p->apps[p->napps];
        char entry[96], behavior[48], trust[24];
        attr(t, "Id", a->id, sizeof(a->id));
        attr(t, "Executable", a->exe, sizeof(a->exe));
        attr(t, "EntryPoint", entry, sizeof(entry));
        attr(t, "RuntimeBehavior", behavior, sizeof(behavior));
        attr(t, "TrustLevel", trust, sizeof(trust));
        bool desktop = !str_casecmp(entry, "Windows.FullTrustApplication") || !str_casecmp(behavior, "win32App") ||
                       !str_casecmp(behavior, "packagedClassicApp") || !str_casecmp(trust, "mediumIL");
        if (!desktop || !a->exe[0])
            continue;
        for (char *c = a->exe; *c; c++)
            if (*c == '/')
                *c = '\\';
        const char *ve = tag(t, end, "VisualElements");
        const char *next = tag(t + 1, end, "Application");
        if (ve && (!next || ve < next))
            attr(ve, "DisplayName", a->name, sizeof(a->name));
        if (!a->name[0] || !strncmp(a->name, "ms-resource:", 12))
            str_copy(a->name, p->display, sizeof(a->name));
        p->napps++;
    }
    p->uwp = found && !p->napps;
    kfree(m);
    return p->name[0] ? 0 : E_INVAL;
}

static void install_dir(const struct package *p, char *out, size_t cap)
{
    char win[PATH_MAX_LEN];
    ksnprintf(win, sizeof(win), APPS_DIR "\\%s_%s", p->name, p->version);
    if (winpath_to_vfs("/", win, out, cap) != 0)
        out[0] = 0;
}

static bool menu_dir(char *out, size_t cap)
{
    return winpath_to_vfs("/", MENU_DIR, out, cap) == 0;
}

/* a part name that stays inside the install folder */
static bool safe(const char *name)
{
    if (!name[0] || name[0] == '/' || strchr(name, ':'))
        return false;
    for (const char *p = name; *p;) {
        const char *s = strchr(p, '/');
        size_t n = s ? (size_t)(s - p) : strlen(p);
        if ((n == 2 && !strncmp(p, "..", 2)) || (n == 1 && *p == '.'))
            return false;
        p += n + (s ? 1 : 0);
    }
    return true;
}

struct unpack {
    const struct package *p;
    const char *dir;
    int files, err;
};

static int unpack_fn(const struct zip_entry *e, void *ctx)
{
    struct unpack *u = ctx;
    char name[PATH_MAX_LEN], path[PATH_MAX_LEN];
    unescape(e->name, e->name_len, name, sizeof(name));
    if (!safe(name) || !strcmp(name, "[Content_Types].xml") || !strcmp(name, "AppxSignature.p7x"))
        return 0;
    path_join(u->dir, name, path, sizeof(path));
    if (name[strlen(name) - 1] == '/') {
        vfs_mkdirs(path);
        return 0;
    }
    char parent[PATH_MAX_LEN];
    path_dirname(path, parent, sizeof(parent));
    if (!vfs_is_dir(parent))
        vfs_mkdirs(parent);
    uint8_t *data;
    int err = zip_extract(u->p->data, u->p->len, e, &data);
    if (!err) {
        err = vfs_write_all(path, data, e->usize);
        kfree(data);
    }
    if (err) {
        u->err = err;
        return err;
    }
    u->files++;
    return 0;
}

static int write_link(const struct package *p, const struct app_entry *a, const char *dir, char *file, size_t cap)
{
    char menu[PATH_MAX_LEN], target[PATH_MAX_LEN];
    if (!menu_dir(menu, sizeof(menu)))
        return E_NOENT;
    vfs_mkdirs(menu);
    struct lnk *l = kzalloc(sizeof(*l));
    uint8_t *buf = kmalloc(8192);
    int err = l && buf ? 0 : E_NOMEM;
    if (!err) {
        winpath_from_vfs(dir, l->dir, sizeof(l->dir));
        ksnprintf(l->target, sizeof(l->target), "%s\\%s", l->dir, a->exe);
        str_copy(l->desc, p->display, sizeof(l->desc));
        char name[NAME_MAX_LEN];
        str_copy(name, a->name, sizeof(name) - 4);
        for (char *c = name; *c; c++)
            if (strchr("\\/:*?\"<>|", *c))
                *c = ' ';
        str_append(name, ".lnk", sizeof(name));
        path_join(menu, name, file, cap);
        err = winpath_to_vfs("/", l->target, target, sizeof(target)) == 0 && vfs_exists(target) ? 0 : E_NOENT;
        size_t n = err ? 0 : lnk_encode(l, buf, 8192);
        if (!err)
            err = n ? vfs_write_all(file, buf, n) : E_INVAL;
    }
    kfree(l);
    kfree(buf);
    return err;
}

static void say(struct shell *sh, char *msg, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, cap, fmt, ap);
    va_end(ap);
    if (sh)
        sh_printf(sh, "%s\n", msg);
}

/* installs; msg gets what to tell the person (also printed when sh is set) */
static int install(struct shell *sh, const uint8_t *data, size_t len, char *msg, size_t cap)
{
    struct package *p = kmalloc(sizeof(*p));
    if (!p)
        return E_NOMEM;
    int err = package_read(p, data, len);
    if (err == E_NOENT)
        say(sh, msg, cap, "not an MSIX or AppX package (no AppxManifest.xml)");
    else if (err == E_NOSYS)
        say(sh, msg, cap, "the bundle has no package for this computer (%s)", my_arch());
    else if (err)
        say(sh, msg, cap, "the package manifest is damaged");
    else if (!arch_ok(p->arch))
        say(sh, msg, cap, "%s is made for %s computers, this one is %s", p->display, p->arch, my_arch()), err = E_NOSYS;
    else if (p->uwp)
        say(sh, msg, cap, "%s is a Universal Windows app; SynthOS runs desktop programs packaged as MSIX, not UWP apps",
            p->display), err = E_NOSYS;
    else if (!p->napps)
        say(sh, msg, cap, "%s has no program to start (a framework or resource package)", p->display), err = E_NOSYS;
    if (err) {
        package_free(p);
        kfree(p);
        return err;
    }
    char dir[PATH_MAX_LEN];
    install_dir(p, dir, sizeof(dir));
    if (vfs_is_dir(dir))
        vfs_remove_tree(dir);
    err = dir[0] ? vfs_mkdirs(dir) : E_NOENT;
    struct unpack u = {p, dir, 0, 0};
    if (!err)
        err = zip_each(p->data, p->len, unpack_fn, &u);
    if (!err)
        err = u.err;
    char record[1024], rpath[PATH_MAX_LEN];
    ksnprintf(record, sizeof(record), "name %s\ndisplay %s\nversion %s\npublisher %s\n", p->name, p->display,
              p->version, p->publisher);
    int links = 0;
    for (int i = 0; !err && i < p->napps; i++) {
        char file[PATH_MAX_LEN];
        if (write_link(p, &p->apps[i], dir, file, sizeof(file)) == 0) {
            links++;
            str_append(record, "link ", sizeof(record));
            str_append(record, file, sizeof(record));
            str_append(record, "\n", sizeof(record));
        }
    }
    if (!err && !links)
        err = E_NOENT;
    if (!err) {
        path_join(dir, RECORD, rpath, sizeof(rpath));
        err = vfs_write_all(rpath, record, strlen(record));
    }
    if (err) {
        if (dir[0])
            vfs_remove_tree(dir);
        say(sh, msg, cap, "could not install %s: %s", p->display, err_str(err));
    } else {
        say(sh, msg, cap, "Installed %s %s (%d files) - find it in Launchpad", p->display, p->version, u.files);
    }
    package_free(p);
    kfree(p);
    return err;
}

/* calls fn for each installed package folder with its record */
static int each_installed(void (*fn)(const char *dir, const char *record, void *ctx), void *ctx)
{
    char root[PATH_MAX_LEN];
    struct vdir d;
    if (winpath_to_vfs("/", APPS_DIR, root, sizeof(root)) != 0 || vfs_opendir(root, &d) != 0)
        return 0;
    int n = 0;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        char dir[PATH_MAX_LEN], rpath[PATH_MAX_LEN], *rec;
        size_t size;
        path_join(root, ent.name, dir, sizeof(dir));
        path_join(dir, RECORD, rpath, sizeof(rpath));
        if (ent.name[0] == '.' || vfs_read_all(rpath, &rec, &size) != 0)
            continue;
        char *text = krealloc(rec, size + 1);
        if (!text) {
            kfree(rec);
            continue;
        }
        text[size] = 0;
        fn(dir, text, ctx);
        kfree(text);
        n++;
    }
    vfs_closedir(&d);
    return n;
}

static void field(const char *record, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    size_t k = strlen(key);
    for (const char *p = record; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, k) && p[k] == ' ') {
            const char *e = strchr(p, '\n');
            str_copy(out, p + k + 1, MIN(cap, e ? (size_t)(e - p - k) : cap));
            return;
        }
}

static void print_fn(const char *dir, const char *record, void *ctx)
{
    UNUSED(dir);
    char name[96], display[96], version[32];
    field(record, "name", name, sizeof(name));
    field(record, "display", display, sizeof(display));
    field(record, "version", version, sizeof(version));
    sh_printf(ctx, "%-28s %-12s %s\n", display, version, name);
}

struct removal {
    const char *want;
    char dir[PATH_MAX_LEN], record[1024];
    bool found;
};

static void match_fn(const char *dir, const char *record, void *ctx)
{
    struct removal *r = ctx;
    char name[96], display[96];
    field(record, "name", name, sizeof(name));
    field(record, "display", display, sizeof(display));
    if (!r->found && (!str_casecmp(name, r->want) || !str_casecmp(display, r->want))) {
        str_copy(r->dir, dir, sizeof(r->dir));
        str_copy(r->record, record, sizeof(r->record));
        r->found = true;
    }
}

static int uninstall(const char *which, char *display, size_t cap)
{
    struct removal *r = kzalloc(sizeof(*r));
    if (!r)
        return E_NOMEM;
    r->want = which;
    each_installed(match_fn, r);
    int err = r->found ? 0 : E_NOENT;
    if (!err) {
        field(r->record, "display", display, cap);
        for (const char *p = r->record; (p = strstr(p, "link ")) != NULL; p += 5) {
            char file[PATH_MAX_LEN];
            const char *e = strchr(p, '\n');
            str_copy(file, p + 5, MIN(sizeof(file), e ? (size_t)(e - p - 4) : sizeof(file)));
            vfs_unlink(file);
        }
        err = vfs_remove_tree(r->dir);
    }
    kfree(r);
    return err;
}

/* ---- the installer window, opened for a package file ---- */

struct installer {
    struct ui_state st;
    char path[PATH_MAX_LEN];
    struct package p;
    char *data;
    size_t len;
    int state; /* 0 ready, 1 installed, 2 cannot */
    char msg[200];
    struct canvas logo;
};

static struct window *open_win;

static void load_logo(struct installer *in)
{
    if (!in->p.logo[0])
        return;
    char want[160];
    unescape(in->p.logo, strlen(in->p.logo), want, sizeof(want));
    char *dot = strrchr(want, '.');
    size_t n = 0;
    char *png = part(in->p.data, in->p.len, want, false, &n);
    if (!png && dot) { /* the scale-qualified file: Assets/Logo.scale-200.png */
        *dot = 0;
        png = part(in->p.data, in->p.len, want, true, &n);
    }
    struct picture pic;
    if (png && png_decode((const uint8_t *)png, n, &pic) == 0) {
        struct canvas src = {pic.px, pic.w, pic.h, pic.w, {0, 0, pic.w, pic.h}};
        if (canvas_alloc(&in->logo, 72, 72))
            gfx_scale_smooth(&in->logo, (struct rect){0, 0, 72, 72}, &src, 255);
        picture_free(&pic);
    }
    kfree(png);
}

static void frame(struct ui *ui, void *data)
{
    struct installer *in = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    if (ui->c)
        gfx_clear(ui->c, (struct rect){0, 0, W, H}, style.content);
    struct rect box = {28, 44, 72, 72};
    if (ui->c) {
        if (in->logo.px)
            gfx_blit_round(ui->c, box, &in->logo, 14, true);
        else
            gfx_icon(ui->c, box.x, box.y, ICON_APP, 72);
    }
    int x = 120, w = W - x - 28;
    ui_label(ui, (struct rect){x, 48, w, 28}, in->p.display[0] ? in->p.display : path_basename(in->path), &font_bold,
             style.text, ALIGN_LEFT);
    char line[200];
    ksnprintf(line, sizeof(line), "Publisher: %s", in->p.publisher[0] ? in->p.publisher : "unknown");
    ui_label(ui, (struct rect){x, 78, w, 20}, line, &font_small, style.secondary, ALIGN_LEFT);
    ksnprintf(line, sizeof(line), "Version: %s   %s", in->p.version[0] ? in->p.version : "?",
              in->p.arch[0] ? in->p.arch : "");
    ui_label(ui, (struct rect){x, 98, w, 20}, line, &font_small, style.secondary, ALIGN_LEFT);
    struct rect msg = {28, 140, W - 56, H - 200};
    if (ui->c)
        ui_wrap_text(ui->c, msg, &font_sans, in->msg, in->state == 2 ? style.error : style.text);
    struct rect b = {W - 128, H - 48, 108, 30};
    if (in->state == 0 && ui_button_primary(ui, b, "Install")) {
        in->state = install(NULL, (const uint8_t *)in->data, in->len, in->msg, sizeof(in->msg)) == 0 ? 1 : 2;
        gui_notify("App Installer", in->msg);
    } else if (in->state == 1 && ui_button_primary(ui, b, "Launch")) {
        tool_launch(in->p.apps[0].name, NULL);
        wm_destroy(ui->win);
        return;
    }
    if (ui_button(ui, (struct rect){b.x - 116, b.y, 108, 30}, in->state == 0 ? "Cancel" : "Close"))
        wm_destroy(ui->win);
}

static void paint(struct window *win, struct canvas *c)
{
    struct installer *in = win->data;
    ui_run(win, &in->st, c, NULL, frame, in);
}

static void event(struct window *win, struct gui_event *e)
{
    struct installer *in = win->data;
    ui_run(win, &in->st, NULL, e, frame, in);
}

static void destroy(struct window *win)
{
    struct installer *in = win->data;
    package_free(&in->p);
    kfree(in->data);
    canvas_free(&in->logo);
    kfree(in);
    if (open_win == win)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (!arg || !arg[0]) {
        gui_terminal_run("appx list");
        return;
    }
    if (open_win && wm_window_alive(open_win))
        wm_destroy(open_win);
    struct installer *in = kzalloc(sizeof(*in));
    if (!in)
        return;
    str_copy(in->path, arg, sizeof(in->path));
    if (vfs_read_all(arg, &in->data, &in->len) != 0) {
        kfree(in);
        gui_notify("App Installer", "Could not read the package.");
        return;
    }
    int err = package_read(&in->p, (const uint8_t *)in->data, in->len);
    if (err) {
        in->state = 2;
        str_copy(in->msg, err == E_NOSYS ? "This bundle has no package for this computer." :
                          "This file is not an MSIX or AppX package, or it is damaged.", sizeof(in->msg));
    } else if (in->p.uwp) {
        in->state = 2;
        str_copy(in->msg, "This is a Universal Windows app. SynthOS runs desktop programs packaged as MSIX, "
                          "but not UWP apps.", sizeof(in->msg));
    } else if (!arch_ok(in->p.arch) || !in->p.napps) {
        in->state = 2;
        str_copy(in->msg, in->p.napps ? "This package is made for a different kind of processor." :
                                        "This package has no program to start.", sizeof(in->msg));
    } else {
        ksnprintf(in->msg, sizeof(in->msg), "Install %s? It adds %d app%s to Launchpad.", in->p.display, in->p.napps,
                  in->p.napps == 1 ? "" : "s");
        load_logo(in);
    }
    open_win = wm_open("App Installer", ICON_APP, 520, 300, &ops, in, 0);
    if (!open_win) {
        package_free(&in->p);
        kfree(in->data);
        canvas_free(&in->logo);
        kfree(in);
    }
}

static const struct gui_app appx_app = {"App Installer", "Install MSIX and AppX packages", ICON_APP, launch,
                                        ".msix;.appx;.msixbundle;.appxbundle", NULL};

COMMAND(appx, "msix", "system", "appx install <file.msix> | list | remove <name> | info <file.msix>",
        "install, list and remove MSIX/AppX packaged desktop programs")
{
    if (argc == 2 && !strcmp(argv[1], "list")) {
        if (!each_installed(print_fn, sh))
            sh_printf(sh, "no MSIX packages installed\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "remove")) {
        char display[96];
        int err = uninstall(argv[2], display, sizeof(display));
        if (err == E_NOENT)
            return sh_error(sh, "appx", "no installed package called %s", argv[2]);
        if (err)
            return sh_error(sh, "appx", "%s", err_str(err));
        sh_printf(sh, "Removed %s\n", display);
        return 0;
    }
    if (argc != 3 || (strcmp(argv[1], "install") && strcmp(argv[1], "info")))
        return command_usage(sh, command_find("appx"));
    char path[PATH_MAX_LEN], *data;
    size_t len;
    int err = sh_resolve(sh, argv[2], path, sizeof(path));
    if (!err)
        err = vfs_read_all(path, &data, &len);
    if (err)
        return sh_fs_error(sh, "appx", argv[2], err);
    char msg[200];
    if (!strcmp(argv[1], "install")) {
        err = install(sh, (const uint8_t *)data, len, msg, sizeof(msg));
        kfree(data);
        return err ? 1 : 0;
    }
    struct package *p = kmalloc(sizeof(*p));
    err = p ? package_read(p, (const uint8_t *)data, len) : E_NOMEM;
    if (!err) {
        sh_printf(sh, "name: %s\ndisplay name: %s\nversion: %s\nprocessor: %s\npublisher: %s\n", p->name, p->display,
                  p->version, p->arch[0] ? p->arch : "neutral", p->publisher);
        for (int i = 0; i < p->napps; i++)
            sh_printf(sh, "app: %s (%s)\n", p->apps[i].name, p->apps[i].exe);
        if (p->uwp)
            sh_printf(sh, "kind: Universal Windows app (not supported)\n");
        package_free(p);
    }
    kfree(p);
    kfree(data);
    return err ? sh_error(sh, "appx", "not an MSIX or AppX package") : 0;
}

static const struct command *const commands[] = {&cmddef_appx};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&appx_app, NULL, &ext);
