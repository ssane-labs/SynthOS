#include <synth/command.h>
#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/rules.h>
#include <synth/shell.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/timeline.h>
#include <synth/versions.h>
#include <synth/vfs.h>

/* Rewind: before a file of the user's is overwritten, the old content is kept in /home/.versions/<hash>/<time>
   (at most one copy a minute, the last 20, files up to 4 MB). Folders and files whose name starts with a dot and
   AppData are left out: programs keep their own state there. */

#define VERSIONS_DIR "/home/.versions"
#define KEEP 20
#define MAX_SIZE (4u << 20)
#define GAP 60

static bool enabled = true, busy;

bool versions_ready;

/* the user's own files: not programs (/home/apps, .exe, .dll), not what the system lays out while it starts */
bool versions_tracked(const char *path)
{
    if (!versions_ready || strncmp(path, "/home/", 6) || strstr(path, "/AppData/") || !strncmp(path, "/home/apps/", 11))
        return false;
    const char *ext = path_ext(path);
    if (ext && (!str_casecmp(ext, ".exe") || !str_casecmp(ext, ".dll") || !str_casecmp(ext, ".sys")))
        return false;
    for (const char *p = path + 5; *p; p++)
        if (*p == '/' && p[1] == '.')
            return false;
    return true;
}

static void dir_of(const char *path, char *out, size_t cap)
{
    uint32_t a = 2166136261u, b = 5381;
    for (const char *p = path; *p; p++) {
        a = (a ^ (uint8_t)*p) * 16777619u;
        b = b * 33 + (uint8_t)*p;
    }
    ksnprintf(out, cap, VERSIONS_DIR "/%08x%08x", a, b);
}

/* Version times, newest first. */
static int list(const char *dir, uint32_t *times, int max)
{
    struct vdir d;
    if (vfs_opendir(dir, &d))
        return 0;
    struct vfs_dirent e;
    int n = 0;
    while (vfs_readdir(&d, &e) == 0) {
        if (e.name[0] != 'v')
            continue;
        uint32_t t = (uint32_t)str_to_long(e.name + 1, NULL, 10);
        int k = n < max ? n++ : max - 1;
        if (k == max - 1 && n == max && times[k] >= t)
            continue;
        while (k > 0 && times[k - 1] < t) {
            times[k] = times[k - 1];
            k--;
        }
        times[k] = t;
    }
    vfs_closedir(&d);
    return n;
}

/* a file of the user's was written: Timeline and the rules hear of it */
void versions_saved(const char *path)
{
    if (busy)
        return;
    timeline_add('s', path_basename(path), path);
    rules_event("save", path);
}

void versions_keep(const char *path, struct vnode *vn)
{
    struct vstat st;
    if (!enabled || busy || !versions_tracked(path) || !vn->ops->stat || !vn->ops->read || vn->ops->stat(vn, &st) || !st.size ||
        st.size > MAX_SIZE)
        return;
    busy = true;
    char dir[64], file[96];
    dir_of(path, dir, sizeof(dir));
    uint32_t now = time_now(), times[KEEP + 8];
    int n = list(dir, times, KEEP + 8);
    char *buf = n && now - times[0] < GAP ? NULL : kmalloc(st.size);
    long got = 0;
    while (buf && got < (long)st.size) {
        long r = vn->ops->read(vn, (uint32_t)got, buf + got, st.size - (size_t)got);
        if (r <= 0)
            break;
        got += r;
    }
    if (buf && got == (long)st.size) {
        vfs_mkdirs(dir);
        ksnprintf(file, sizeof(file), "%s/path", dir);
        if (!vfs_exists(file))
            vfs_write_all(file, path, strlen(path));
        ksnprintf(file, sizeof(file), "%s/v%010u", dir, now);
        if (!vfs_write_all(file, buf, st.size))
            for (int i = KEEP - 1; i < n; i++) {
                ksnprintf(file, sizeof(file), "%s/v%010u", dir, times[i]);
                vfs_unlink(file);
            }
    }
    kfree(buf);
    busy = false;
}

int versions_list(const char *path, uint32_t *times, uint32_t *sizes, int max)
{
    char dir[64], file[96];
    dir_of(path, dir, sizeof(dir));
    int n = list(dir, times, max);
    for (int i = 0; sizes && i < n; i++) {
        struct vstat st = {0};
        ksnprintf(file, sizeof(file), "%s/v%010u", dir, times[i]);
        vfs_stat(file, &st);
        sizes[i] = st.size;
    }
    return n;
}

int versions_restore(const char *path, uint32_t time)
{
    char dir[64], file[96];
    dir_of(path, dir, sizeof(dir));
    ksnprintf(file, sizeof(file), "%s/v%010u", dir, time);
    char *data;
    size_t size;
    int err = vfs_read_all(file, &data, &size);
    if (err)
        return err;
    busy = true;
    char keep[96];
    ksnprintf(keep, sizeof(keep), "%s/v%010u", dir, time_now());
    if (strcmp(keep, file))
        vfs_copy(path, keep);
    busy = false;
    err = vfs_write_all(path, data, size);
    kfree(data);
    return err;
}

static void get_versions(char *buf, size_t cap)
{
    str_copy(buf, enabled ? "on" : "off", cap);
}

static int set_versions(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    enabled = !strcmp(value, "on");
    return 0;
}

static const struct setting s_versions = {"versions", "on|off",
                                          "keep earlier versions of your files when they are saved over (Rewind)",
                                          get_versions, set_versions};
SETTING(s_versions);

COMMAND(versions, "rewind", "files", "versions <file> [show <n> | restore <n>]",
        "earlier versions of a file, kept each time it is saved over; restore brings one back")
{
    if (argc < 2)
        return command_usage(sh, command_find("versions"));
    char path[PATH_MAX_LEN];
    sh_resolve(sh, argv[1], path, sizeof(path));
    uint32_t times[32], sizes[32];
    int n = versions_list(path, times, sizes, 32);
    if (argc < 4) {
        if (!n) {
            sh_printf(sh, "%s has no earlier versions\n", argv[1]);
            return 0;
        }
        for (int i = 0; i < n; i++) {
            char when[24], size[16];
            time_format(times[i] + (uint32_t)(time_zone() * 60), when, sizeof(when));
            fmt_size(size, sizeof(size), sizes[i]);
            sh_printf(sh, "%3d  %s  %s\n", i + 1, when, size);
        }
        return 0;
    }
    int k = (int)str_to_long(argv[3], NULL, 10) - 1;
    if (k < 0 || k >= n)
        return sh_error(sh, "versions", "no version %s of %s", argv[3], argv[1]);
    if (!strcmp(argv[2], "restore")) {
        int err = versions_restore(path, times[k]);
        if (err)
            return sh_error(sh, "versions", "%s", err_str(err));
        sh_printf(sh, "%s is back to version %d\n", argv[1], k + 1);
        return 0;
    }
    if (!strcmp(argv[2], "show")) {
        char dir[64], file[96], *data;
        size_t size;
        dir_of(path, dir, sizeof(dir));
        ksnprintf(file, sizeof(file), "%s/v%010u", dir, times[k]);
        if (vfs_read_all(file, &data, &size) == 0) {
            stream_write(sh->out, data, size);
            kfree(data);
        }
        return 0;
    }
    return command_usage(sh, command_find("versions"));
}
