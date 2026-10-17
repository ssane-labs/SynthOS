/* Installs an application the way Google Update does: ask the update server (Omaha protocol 3.1) for the current
   package, download it and check its SHA-256. The package is a CRX3 (a zip) holding the offline installer, and that
   installer carries the program as a 7z archive, which is unpacked straight from the downloaded file. */
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/input.h>
#include <synth/lnk.h>
#include <synth/omaha.h>
#include <synth/path.h>
#include <synth/sevenzip.h>
#include <synth/sha256.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

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
    struct omaha_ui *ui;
};

static void fail(struct omaha_ui *ui, const char *fmt, const char *a, const char *b)
{
    ksnprintf(ui->error, sizeof(ui->error), fmt, a, b);
}

static void say(struct omaha_ui *ui, const char *text)
{
    if (ui->say)
        ui->say(ui->ctx, text);
}

static int to_file(void *ctx, const void *data, size_t n)
{
    struct sink *s = ctx;
    sha256_update(&s->hash, data, n);
    return vfs_write(s->f, data, n) == (long)n ? 0 : E_NOSPC;
}

static void sink_progress(void *ctx, const char *stage, size_t done, size_t total)
{
    UNUSED(stage);
    struct omaha_ui *ui = ((struct sink *)ctx)->ui;
    if (ui->progress)
        ui->progress(ui->ctx, "Downloading", done, total);
}

static struct omaha_ui *current_ui;

static bool cancelled(void)
{
    return current_ui && current_ui->cancelled ? current_ui->cancelled(current_ui->ctx) : input_break_pending();
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

/* Where the installer that the CRX3 package holds starts: "Cr24", version 3, a header, then a zip archive. */
static int installer_offset(const char *crx, const char *name, uint32_t *at)
{
    struct file *f;
    int err = vfs_open(crx, VFS_READ, &f);
    if (err)
        return err;
    uint8_t head[12];
    struct vstat st;
    uint32_t size = vfs_stat(crx, &st) == 0 ? st.size : 0, len = 0;
    if (!read_at(f, 0, head, sizeof(head)) || memcmp(head, "Cr24", 4) || rd32(head + 4) != 3 ||
        12 + rd32(head + 8) >= size)
        err = E_INVAL;
    else
        err = find_stored(f, 12 + rd32(head + 8), size, name, at, &len);
    vfs_close(f);
    return err;
}

struct update {
    char codebase[256], name[192], run[128], hash[72], version[32];
    uint64_t size;
};

static uint64_t json_number(const char *from, const char *key)
{
    char pattern[48];
    ksnprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = from ? strstr(from, pattern) : NULL;
    uint64_t v = 0;
    for (p = p ? p + strlen(pattern) : NULL; p && *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (uint64_t)(*p - '0');
    return v;
}

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
    u->size = json_number(packages, "size");
    return u->codebase[0] && packages && json_string(packages, "name", u->name, sizeof(u->name)) &&
           json_string(packages, "hash_sha256", u->hash, sizeof(u->hash)) &&
           json_string(check, "run", u->run, sizeof(u->run)) && strchr(u->run, '/') == NULL &&
           strchr(u->name, '/') == NULL && strstr(u->run, "..") == NULL && strstr(u->name, "..") == NULL &&
           json_string(check, "version", u->version, sizeof(u->version));
}

static int ask_server(struct omaha_ui *ui, const char *appid, const char *ap, struct update *u)
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
    say(ui, "Asking Google's update server for the current version");
    int err = http_fetch(&req, &resp);
    if (err) {
        fail(ui, "The update server cannot be reached: %s%s", tls_strerror(err), "");
        return err;
    }
    bool ok = resp.status == 200 && resp.body && parse_update(resp.body, u);
    http_free(&resp);
    if (!ok)
        fail(ui, "The update server did not offer a package%s%s", "", "");
    return ok ? 0 : E_NOENT;
}

static int download(struct omaha_ui *ui, const struct update *u, const char *path)
{
    char url[HTTP_URL_MAX];
    ksnprintf(url, sizeof(url), "%s%s", u->codebase, u->name);
    struct sink s = {.ui = ui};
    int err = vfs_open(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &s.f);
    if (err) {
        fail(ui, "Cannot write %s: %s", path, err_str(err));
        return err;
    }
    sha256_init(&s.hash);
    say(ui, url);
    struct http_request req = {.url = url, .cancelled = cancelled, .progress = sink_progress, .ctx = &s,
                               .flags = HTTP_NO_COOKIES | HTTP_GZIP, .sink = to_file};
    struct http_response resp;
    err = http_fetch(&req, &resp);
    int status = err ? 0 : resp.status;
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
        if (err == E_INVAL)
            fail(ui, "The download is damaged (SHA-256 does not match)%s%s", "", "");
        else
            fail(ui, "Could not download %s: %s", u->name, tls_strerror(err));
    }
    return err;
}

static void unpack_progress(void *ctx, const char *name, uint64_t done, uint64_t total)
{
    UNUSED(name);
    struct omaha_ui *ui = ctx;
    if (ui->progress)
        ui->progress(ui->ctx, "Unpacking", done, total);
}

int omaha_install(const char *appid, const char *ap, const char *dest, const char *strip, struct omaha_ui *ui)
{
    current_ui = ui;
    ui->error[0] = 0;
    struct update u = {0};
    int err = ask_server(ui, appid, ap, &u);
    char work[PATH_MAX_LEN], crx[PATH_MAX_LEN] = "";
    path_join(user_home(), "Downloads", work, sizeof(work));
    vfs_mkdirs(work);
    uint64_t free = 0;
    if (!err && u.size && vfs_free_space(work, &free) == 0 && free < u.size * 2 + (64u << 20)) {
        char need[16], have[16];
        fmt_size(need, sizeof(need), u.size * 2 + (64u << 20));
        fmt_size(have, sizeof(have), free);
        fail(ui, "This needs %s of free disk space; %s is free", need, have);
        err = E_NOSPC;
    }
    if (!err) {
        char line[96];
        ksnprintf(line, sizeof(line), "Version %s", u.version);
        say(ui, line);
        char file[NAME_MAX_LEN];
        ksnprintf(file, sizeof(file), "package-%s.crx3", u.version);
        path_join(work, file, crx, sizeof(crx));
        err = download(ui, &u, crx);
    }
    uint32_t at = 0;
    if (!err && (err = installer_offset(crx, u.run, &at)) != 0)
        fail(ui, "The package has no %s inside%s", u.run, "");
    int64_t base = err ? -1 : sevenzip_find(crx, at);
    if (!err && base < 0) {
        err = E_INVAL;
        fail(ui, "%s carries no 7z archive%s", u.run, "");
    }
    if (!err) {
        say(ui, "Unpacking");
        vfs_mkdirs(dest);
        struct sevenzip_result r;
        err = sevenzip_extract(crx, base, dest, strip, unpack_progress, ui, &r);
        if (!err && !r.files)
            err = E_NOENT;
        if (err)
            fail(ui, "Unpacking failed: %s%s", err_str(err), "");
    }
    if (u.name[0] && crx[0])
        vfs_unlink(crx);
    current_ui = NULL;
    return err;
}

/* Google Chrome: the 32-bit stable channel, unpacked into /home/apps/chrome. */

#define CHROME_APPID "{8A69D345-D564-463C-AFF1-A69D9E530F96}"
#define CHROME_AP "stable-arch_x86"
#define CHROME_DIR "/home/apps/chrome"

bool chrome_path(char *out, size_t cap)
{
    path_join(CHROME_DIR, "chrome.exe", out, cap);
    return vfs_exists(out);
}

static void chrome_shortcut(const char *exe)
{
    struct lnk *l = kzalloc(sizeof(*l));
    uint8_t *buf = kmalloc(8192);
    char dir[PATH_MAX_LEN], file[PATH_MAX_LEN];
    if (l && buf) {
        winpath_from_vfs(exe, l->target, sizeof(l->target));
        winpath_from_vfs(CHROME_DIR, l->dir, sizeof(l->dir));
        path_join(user_home(), "Desktop", dir, sizeof(dir));
        vfs_mkdirs(dir);
        path_join(dir, "Google Chrome.lnk", file, sizeof(file));
        size_t n = lnk_encode(l, buf, 8192);
        if (n)
            vfs_write_all(file, buf, n);
    }
    kfree(l);
    kfree(buf);
}

int chrome_install(struct omaha_ui *ui)
{
    int err = omaha_install(CHROME_APPID, CHROME_AP, CHROME_DIR, "Chrome-bin/", ui);
    char exe[PATH_MAX_LEN];
    if (!err && !chrome_path(exe, sizeof(exe))) {
        fail(ui, "chrome.exe did not come out of the package%s%s", "", "");
        err = E_NOENT;
    }
    if (!err)
        chrome_shortcut(exe);
    return err;
}
