#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/input.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>
#include <synth/zip.h>

#define PATHS_FILE "/home/.paths"
#define DEFAULT_ROOT "/home/apps"

struct unzip_job {
    struct shell *sh;
    const uint8_t *data;
    size_t len;
    const char *dir;
    const char *strip;
    bool list, quiet;
    int files;
    uint64_t bytes;
    int err;
};

static bool safe_name(const char *name)
{
    if (name[0] == '/' || name[0] == '\\' || strchr(name, ':'))
        return false;
    bool start = true;
    for (const char *p = name; *p; p++) {
        if (start && p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\' || !p[2]))
            return false;
        start = *p == '/' || *p == '\\';
    }
    return true;
}

static int unzip_entry(const struct zip_entry *e, void *ctx)
{
    struct unzip_job *job = ctx;
    char name[PATH_MAX_LEN];
    size_t n = MIN(e->name_len, sizeof(name) - 1);
    memcpy(name, e->name, n);
    name[n] = 0;
    for (char *p = name; *p; p++)
        if (*p == '\\')
            *p = '/';
    const char *rel = name;
    if (job->strip && str_starts(rel, job->strip))
        rel += strlen(job->strip);
    if (!*rel)
        return 0;
    if (!safe_name(rel)) {
        sh_printf(job->sh, "skipping unsafe name %s\n", name);
        return 0;
    }
    bool dir = rel[strlen(rel) - 1] == '/';
    if (job->list) {
        char size[16];
        fmt_size(size, sizeof(size), e->usize);
        sh_printf(job->sh, "%8s  %s\n", dir ? "" : size, rel);
        job->files += !dir;
        job->bytes += e->usize;
        return 0;
    }
    char path[PATH_MAX_LEN];
    path_join(job->dir, rel, path, sizeof(path));
    if (dir)
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
    int err = zip_extract(job->data, job->len, e, &data);
    if (!err) {
        err = vfs_write_all(path, data, e->usize);
        kfree(data);
    }
    if (err) {
        job->err = err;
        sh_fs_error(job->sh, "unzip", path, err);
        return err;
    }
    job->files++;
    job->bytes += e->usize;
    if (!job->quiet && job->files % 50 == 0) {
        sh_role(job->sh, ROLE_MUTED);
        sh_printf(job->sh, "  %d files...\n", job->files);
        sh_role(job->sh, ROLE_TEXT);
    }
    if (input_break_pending())
        return E_INTR;
    return 0;
}

static int unzip_data(struct shell *sh, const uint8_t *data, size_t len, const char *dir, const char *strip, bool list,
                      bool quiet)
{
    struct unzip_job job = {sh, data, len, dir, strip, list, quiet, 0, 0, 0};
    if (!list && !vfs_is_dir(dir)) {
        int err = vfs_mkdirs(dir);
        if (err)
            return sh_fs_error(sh, "unzip", dir, err);
    }
    int err = zip_each(data, len, unzip_entry, &job);
    if (err == E_INVAL)
        return sh_error(sh, "unzip", "not a zip archive (or a damaged one)");
    if (err)
        return job.err ? 1 : sh_error(sh, "unzip", "%s", err_str(err));
    char size[16];
    fmt_size(size, sizeof(size), (size_t)MIN(job.bytes, (uint64_t)0xffffffffu));
    if (!quiet)
        sh_printf(sh, "%s %d files (%s)%s%s\n", list ? "Listed" : "Extracted", job.files, size, list ? "" : " to ",
                  list ? "" : dir);
    return 0;
}

COMMAND(unzip, NULL, "files", "unzip [-l] [-q] [-d dir] file.zip", "list or extract a zip archive")
{
    const char *dir = NULL;
    bool list = false, quiet = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-l"))
            list = true;
        else if (!strcmp(argv[i], "-q"))
            quiet = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dir = argv[++i];
        else
            return command_usage(sh, command_find("unzip"));
    }
    if (i != argc - 1)
        return command_usage(sh, command_find("unzip"));
    char path[PATH_MAX_LEN], target[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[i], path, sizeof(path));
    if (!err)
        err = sh_resolve(sh, dir ? dir : ".", target, sizeof(target));
    char *data;
    size_t len;
    if (!err)
        err = vfs_read_all(path, &data, &len);
    if (err)
        return sh_fs_error(sh, "unzip", argv[i], err);
    int rc = unzip_data(sh, (const uint8_t *)data, len, target, NULL, list, quiet);
    kfree(data);
    return rc;
}

enum post {
    POST_NONE,
    POST_PYTHON,
};

struct package {
    const char *name;
    const char *version;
    const char *summary;
    const char *urls[3];
    const char *bin;
    enum post post;
};

static const struct package packages[] = {
    {"git", "2.56.0", "Git for Windows (MinGit, 32-bit): clone, commit, push over HTTPS",
     {"https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.2/MinGit-2.56.0.2-32-bit.zip",
      "https://registry.npmmirror.com/-/binary/git-for-windows/v2.56.0.windows.2/MinGit-2.56.0.2-32-bit.zip", NULL},
     "mingw32/bin", POST_NONE},
    {"python", "3.12.10", "Python for Windows (official embeddable build, 32-bit) with site-packages",
     {"https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-win32.zip", NULL, NULL}, "", POST_PYTHON},
    {"vscode", "latest", "Visual Studio Code (64-bit Electron; experimental, does not start yet)",
     {"https://update.code.visualstudio.com/latest/win32-x64-archive/stable", NULL, NULL}, "", POST_NONE},
};

static const struct package *find_package(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(packages); i++)
        if (!str_casecmp(packages[i].name, name))
            return &packages[i];
    return NULL;
}

static void show_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    struct shell *sh = ctx;
    static size_t last;
    if (!done) {
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "  %s...\n", stage);
        sh_role(sh, ROLE_TEXT);
        last = 0;
        return;
    }
    if (done - last < (1u << 20) && done != total)
        return;
    last = done;
    char a[16], b[16];
    fmt_size(a, sizeof(a), done);
    if (total) {
        fmt_size(b, sizeof(b), total);
        sh_printf(sh, "\r  downloaded %s of %s (%u%%)  ", a, b, (unsigned)((uint64_t)done * 100 / total));
    } else {
        sh_printf(sh, "\r  downloaded %s  ", a);
    }
}

static bool cancelled(void)
{
    return input_break_pending();
}

static int download(struct shell *sh, const struct package *p, struct http_response *resp)
{
    int err = E_NOENT;
    for (int i = 0; i < 3 && p->urls[i]; i++) {
        sh_printf(sh, "Downloading %s\n", p->urls[i]);
        struct http_request req = {.url = p->urls[i], .max_size = 512u << 20, .cancelled = cancelled,
                                   .progress = show_progress, .ctx = sh, .flags = HTTP_NO_COOKIES};
        err = http_fetch(&req, resp);
        sh_puts(sh, "\n");
        if (!err && resp->status == 200)
            return 0;
        if (!err) {
            sh_printf(sh, "  server answered %d\n", resp->status);
            http_free(resp);
            err = E_NOENT;
        } else {
            sh_printf(sh, "  failed: %s\n", tls_strerror(err));
        }
        if (cancelled())
            break;
    }
    return err;
}

static void add_path(struct shell *sh, const char *dir)
{
    const char *cur = sh_get(sh, "PATH");
    if (!cur || !strstr(cur, dir)) {
        char *grown = kmalloc(strlen(cur ? cur : "") + strlen(dir) + 2);
        if (grown) {
            ksnprintf(grown, strlen(cur ? cur : "") + strlen(dir) + 2, "%s:%s", cur ? cur : "", dir);
            sh_set(sh, "PATH", grown);
            kfree(grown);
        }
    }
    char *old = NULL;
    size_t len = 0;
    vfs_read_all(PATHS_FILE, &old, &len);
    if (old && strstr(old, dir)) {
        kfree(old);
        return;
    }
    char *next = kmalloc(len + strlen(dir) + 2);
    if (next) {
        if (old)
            memcpy(next, old, len);
        memcpy(next + len, dir, strlen(dir));
        next[len + strlen(dir)] = '\n';
        vfs_write_all(PATHS_FILE, next, len + strlen(dir) + 1);
        kfree(next);
    }
    kfree(old);
}

static int post_python(struct shell *sh, const char *root)
{
    struct vdir d;
    if (vfs_opendir(root, &d) != 0)
        return 0;
    struct vfs_dirent ent;
    char pth[PATH_MAX_LEN] = "";
    while (vfs_readdir(&d, &ent) == 0)
        if (str_ends(ent.name, "._pth"))
            path_join(root, ent.name, pth, sizeof(pth));
    vfs_closedir(&d);
    if (!pth[0])
        return 0;
    char zipname[NAME_MAX_LEN];
    str_copy(zipname, path_basename(pth), sizeof(zipname));
    char *dot = strrchr(zipname, '.');
    if (dot)
        str_copy(dot, ".zip", sizeof(zipname) - (size_t)(dot - zipname));
    char text[256];
    int n = ksnprintf(text, sizeof(text), "%s\r\n.\r\nLib\\site-packages\r\nimport site\r\n", zipname);
    vfs_write_all(pth, text, (size_t)n);
    char site[PATH_MAX_LEN];
    path_join(root, "Lib/site-packages", site, sizeof(site));
    vfs_mkdirs(site);
    sh_puts(sh, "Enabled site-packages (pip can be installed with: python get-pip.py)\n");
    return 0;
}

static int install(struct shell *sh, const struct package *p, const char *base)
{
    char root[PATH_MAX_LEN];
    path_join(base, p->name, root, sizeof(root));
    struct http_response resp;
    int err = download(sh, p, &resp);
    if (err)
        return sh_error(sh, "pkg", "could not download %s", p->name);
    char size[16];
    fmt_size(size, sizeof(size), resp.length);
    sh_printf(sh, "Unpacking %s (%s) into %s\n", p->name, size, root);
    int rc = unzip_data(sh, (const uint8_t *)resp.body, resp.length, root, NULL, false, false);
    http_free(&resp);
    if (rc)
        return rc;
    if (p->post == POST_PYTHON)
        post_python(sh, root);
    char bin[PATH_MAX_LEN];
    if (p->bin[0])
        path_join(root, p->bin, bin, sizeof(bin));
    else
        str_copy(bin, root, sizeof(bin));
    add_path(sh, bin);
    sh_role(sh, ROLE_OK);
    sh_printf(sh, "Installed %s %s\n", p->name, p->version);
    sh_role(sh, ROLE_TEXT);
    sh_printf(sh, "Programs in %s are now on the PATH.\n", bin);
    return 0;
}

COMMAND(pkg, "install", "system", "pkg [list | install name [--to dir] | remove name [--from dir]]",
        "download and install programs (git, python, ...)")
{
    if (argc < 2 || !strcmp(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(packages); i++) {
            sh_role(sh, ROLE_ACCENT);
            sh_printf(sh, "%-8s", packages[i].name);
            sh_role(sh, ROLE_MUTED);
            sh_printf(sh, " %-8s ", packages[i].version);
            sh_role(sh, ROLE_TEXT);
            sh_printf(sh, "%s\n", packages[i].summary);
        }
        return 0;
    }
    if (argc < 3)
        return command_usage(sh, command_find("pkg"));
    const struct package *p = find_package(argv[2]);
    if (!p)
        return sh_error(sh, "pkg", "unknown package %s (pkg list shows them)", argv[2]);
    char base[PATH_MAX_LEN];
    str_copy(base, DEFAULT_ROOT, sizeof(base));
    if (argc >= 5 && (!strcmp(argv[3], "--to") || !strcmp(argv[3], "--from"))) {
        int err = sh_resolve(sh, argv[4], base, sizeof(base));
        if (err)
            return sh_fs_error(sh, "pkg", argv[4], err);
    }
    if (!strcmp(argv[1], "install"))
        return install(sh, p, base);
    if (!strcmp(argv[1], "remove")) {
        char root[PATH_MAX_LEN], cmd[PATH_MAX_LEN + 16];
        path_join(base, p->name, root, sizeof(root));
        if (!vfs_is_dir(root))
            return sh_error(sh, "pkg", "%s is not installed in %s", p->name, base);
        ksnprintf(cmd, sizeof(cmd), "rm -r '%s'", root);
        return shell_exec(sh, cmd);
    }
    return command_usage(sh, command_find("pkg"));
}
