/* Installs an application the way Google Update does: ask the update server (Omaha protocol 3.1) for the current
   package, download it, check its SHA-256, take the installer out of the CRX3 package and run it. */
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/input.h>
#include <synth/omaha.h>
#include <synth/path.h>
#include <synth/sha256.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>

#define OMAHA_URL "https://update.googleapis.com/service/update2/json"
#define CHUNK (1u << 20)

/* Copies the JSON string value that follows "key": after `from`, decoding \uXXXX escapes of ASCII. */
static bool json_string(const char *from, const char *key, char *out, size_t cap)
{
    char pattern[48];
    ksnprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = from ? strstr(from, pattern) : NULL;
    if (!p)
        return false;
    p += strlen(pattern);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (p[0] == '\\' && p[1] == 'u' && p[2] && p[3] && p[4] && p[5]) {
            char hex[5] = {p[2], p[3], p[4], p[5], 0};
            out[n++] = (char)str_to_long(hex, NULL, 16);
            p += 6;
        } else if (p[0] == '\\' && p[1]) {
            out[n++] = p[1];
            p += 2;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return *p == '"';
}

struct sink {
    struct file *f;
    struct sha256 hash;
    struct shell *sh;
};

static int to_file(void *ctx, const void *data, size_t n)
{
    struct sink *s = ctx;
    sha256_update(&s->hash, data, n);
    return vfs_write(s->f, data, n) == (long)n ? 0 : E_NOSPC;
}

static void progress(void *ctx, const char *stage, size_t done, size_t total)
{
    struct shell *sh = ctx;
    static size_t last;
    if (!done) {
        last = 0;
        return;
    }
    if (done - last < (4u << 20) && done != total)
        return;
    last = done;
    char a[16], b[16];
    fmt_size(a, sizeof(a), done);
    fmt_size(b, sizeof(b), total);
    if (total)
        sh_printf(sh, "\r  %s %s of %s (%u%%)  ", stage, a, b, (unsigned)((uint64_t)done * 100 / total));
    else
        sh_printf(sh, "\r  %s %s  ", stage, a);
}

static void sink_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    progress(((struct sink *)ctx)->sh, stage, done, total);
}

static bool cancelled(void)
{
    return input_break_pending();
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static bool read_at(struct file *f, uint32_t off, void *buf, size_t n)
{
    return vfs_seek(f, (long)off, VFS_SEEK_SET) >= 0 && vfs_read(f, buf, n) == (long)n;
}

/* Finds a stored (uncompressed) member of the zip archive that starts at `zip` and gives its data range. */
static int find_stored(struct file *f, uint32_t zip, uint32_t size, const char *name, uint32_t *at, uint32_t *len)
{
    uint8_t tail[1024];
    uint32_t back = MIN(size - zip, (uint32_t)sizeof(tail));
    if (!read_at(f, size - back, tail, back))
        return E_IO;
    int eocd = -1;
    for (int i = (int)back - 22; i >= 0 && eocd < 0; i--)
        if (rd32(tail + i) == 0x06054b50)
            eocd = i;
    if (eocd < 0)
        return E_INVAL;
    uint32_t count = rd16(tail + eocd + 10), dir_size = rd32(tail + eocd + 12), dir = zip + rd32(tail + eocd + 16);
    if (dir_size > (1u << 20) || dir + dir_size > size)
        return E_INVAL;
    uint8_t *cd = kmalloc(dir_size);
    if (!cd)
        return E_NOMEM;
    int err = read_at(f, dir, cd, dir_size) ? E_NOENT : E_IO;
    for (uint32_t i = 0, p = 0; err == E_NOENT && i < count && p + 46 <= dir_size; i++) {
        uint32_t name_len = rd16(cd + p + 28), extra = rd16(cd + p + 30), comment = rd16(cd + p + 32);
        if (p + 46 + name_len > dir_size)
            break;
        if (name_len == strlen(name) && !memcmp(cd + p + 46, name, name_len)) {
            uint8_t local[30];
            uint32_t header = zip + rd32(cd + p + 42);
            if (rd16(cd + p + 10) != 0)
                err = E_NOSYS;
            else if (!read_at(f, header, local, sizeof(local)) || rd32(local) != 0x04034b50)
                err = E_INVAL;
            else {
                *at = header + 30 + rd16(local + 26) + rd16(local + 28);
                *len = rd32(cd + p + 24);
                err = *at + *len <= size ? 0 : E_INVAL;
            }
        }
        p += 46 + name_len + extra + comment;
    }
    kfree(cd);
    return err;
}

/* A CRX3 file is "Cr24", version 3, a header of the given length and then a zip archive. */
static int extract_from_crx(struct shell *sh, const char *crx, const char *name, const char *out)
{
    struct file *f, *o = NULL;
    int err = vfs_open(crx, VFS_READ, &f);
    if (err)
        return err;
    uint8_t head[12];
    struct vstat st;
    uint32_t size = vfs_stat(crx, &st) == 0 ? st.size : 0, at = 0, len = 0;
    if (!read_at(f, 0, head, sizeof(head)) || memcmp(head, "Cr24", 4) || rd32(head + 4) != 3 ||
        12 + rd32(head + 8) >= size)
        err = E_INVAL;
    else
        err = find_stored(f, 12 + rd32(head + 8), size, name, &at, &len);
    uint8_t *buf = err ? NULL : kmalloc(CHUNK);
    if (!err && !buf)
        err = E_NOMEM;
    if (!err)
        err = vfs_open(out, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &o);
    for (uint32_t done = 0; !err && done < len;) {
        uint32_t n = MIN(CHUNK, len - done);
        if (!read_at(f, at + done, buf, n))
            err = E_IO;
        else if (vfs_write(o, buf, n) != (long)n)
            err = E_NOSPC;
        done += n;
        progress(sh, "unpacked", done, len);
    }
    sh_puts(sh, "\n");
    if (o)
        vfs_close(o);
    vfs_close(f);
    kfree(buf);
    if (err)
        vfs_unlink(out);
    return err;
}

struct update {
    char codebase[256], name[192], run[128], args[256], hash[72], version[32];
};

/* The first https:// download location, preferring dl.google.com, and what the manifest says to run. */
static bool parse_update(const char *json, struct update *u)
{
    const char *check = strstr(json, "\"updatecheck\"");
    char status[16];
    if (!check || !json_string(check, "status", status, sizeof(status)) || strcmp(status, "ok"))
        return false;
    u->codebase[0] = 0;
    for (const char *p = check; (p = strstr(p, "\"codebase\":\"")); p++) {
        char url[256];
        if (json_string(p, "codebase", url, sizeof(url)) && str_starts(url, "https://") &&
            (!u->codebase[0] || strstr(url, "//dl.google.com/")))
            str_copy(u->codebase, url, sizeof(u->codebase));
    }
    const char *packages = strstr(check, "\"package\"");
    return u->codebase[0] && packages && json_string(packages, "name", u->name, sizeof(u->name)) &&
           json_string(packages, "hash_sha256", u->hash, sizeof(u->hash)) &&
           json_string(check, "run", u->run, sizeof(u->run)) && strchr(u->run, '/') == NULL &&
           strchr(u->name, '/') == NULL && strstr(u->run, "..") == NULL &&
           (json_string(check, "arguments", u->args, sizeof(u->args)) || (u->args[0] = 0, true)) &&
           json_string(check, "version", u->version, sizeof(u->version));
}

static int ask_server(struct shell *sh, const char *appid, const char *ap, struct update *u)
{
    char body[640];
    int n = ksnprintf(body, sizeof(body),
                      "{\"request\":{\"protocol\":\"3.1\",\"updater\":\"SynthOS\",\"updaterversion\":\"1.0\","
                      "\"acceptformat\":\"crx3,download,run\",\"ismachine\":false,\"arch\":\"x86\","
                      "\"nacl_arch\":\"x86\",\"os\":{\"platform\":\"win\",\"version\":\"10.0.19045.0\","
                      "\"arch\":\"x86\"},\"app\":[{\"appid\":\"%s\",\"version\":\"0.0.0.0\",\"ap\":\"%s\","
                      "\"updatecheck\":{}}]}}",
                      appid, ap);
    struct http_request req = {.method = "POST", .url = OMAHA_URL, .headers = "Content-Type: application/json\r\n",
                               .body = body, .body_len = (size_t)n, .max_size = 1u << 20, .cancelled = cancelled,
                               .flags = HTTP_NO_COOKIES};
    struct http_response resp;
    sh_printf(sh, "Asking %s for the current version\n", OMAHA_URL);
    int err = http_fetch(&req, &resp);
    if (err)
        return sh_error(sh, "pkg", "the update server cannot be reached: %s", tls_strerror(err));
    bool ok = resp.status == 200 && resp.body && parse_update(resp.body, u);
    http_free(&resp);
    return ok ? 0 : sh_error(sh, "pkg", "the update server did not offer a package");
}

static int download(struct shell *sh, const struct update *u, const char *path)
{
    char url[HTTP_URL_MAX];
    ksnprintf(url, sizeof(url), "%s%s", u->codebase, u->name);
    struct sink s = {.sh = sh};
    int err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &s.f);
    if (err)
        return sh_fs_error(sh, "pkg", path, err);
    sha256_init(&s.hash);
    sh_printf(sh, "Downloading %s\n", url);
    struct http_request req = {.url = url, .cancelled = cancelled, .progress = sink_progress, .ctx = &s,
                               .flags = HTTP_NO_COOKIES, .sink = to_file};
    struct http_response resp;
    progress(sh, "downloaded", 0, 0);
    err = http_fetch(&req, &resp);
    sh_puts(sh, "\n");
    int status = err ? 0 : resp.status;
    size_t got = err ? 0 : resp.length;
    if (!err)
        http_free(&resp);
    vfs_close(s.f);
    uint8_t digest[32];
    sha256_final(&s.hash, digest);
    char hex[65];
    for (int i = 0; i < 32; i++)
        ksnprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (!err && status != 200)
        err = E_NOENT;
    if (!err && str_casecmp(hex, u->hash))
        err = E_INVAL;
    if (err) {
        vfs_unlink(path);
        return sh_error(sh, "pkg", err == E_INVAL ? "the download is damaged (SHA-256 does not match)"
                                                  : "could not download %s: %s", u->name, tls_strerror(err));
    }
    char size[16];
    fmt_size(size, sizeof(size), got);
    sh_printf(sh, "Downloaded %s, SHA-256 matches\n", size);
    return 0;
}

int omaha_install(struct shell *sh, const char *appid, const char *ap, const char *dir)
{
    struct update u = {0};
    int err = ask_server(sh, appid, ap, &u);
    if (err)
        return err;
    sh_printf(sh, "Version %s: %s\n", u.version, u.run);
    vfs_mkdirs(dir);
    char crx[PATH_MAX_LEN], exe[PATH_MAX_LEN];
    path_join(dir, u.name, crx, sizeof(crx));
    path_join(dir, u.run, exe, sizeof(exe));
    if ((err = download(sh, &u, crx)) != 0)
        return err;
    err = extract_from_crx(sh, crx, u.run, exe);
    vfs_unlink(crx);
    if (err)
        return sh_fs_error(sh, "pkg", u.run, err);
    char cmd[PATH_MAX_LEN + 300];
    ksnprintf(cmd, sizeof(cmd), "'%s' %s", exe, u.args);
    sh_printf(sh, "Running the installer: %s\n", cmd);
    err = shell_exec(sh, cmd);
    vfs_unlink(exe);
    return err;
}
