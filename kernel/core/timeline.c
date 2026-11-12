#include <synth/command.h>
#include <synth/config.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/timeline.h>
#include <synth/vfs.h>

/* Timeline: what was opened, saved and visited, one line each in /home/.timeline ("time kind title target",
   separated by tabs). The same thing again within ten minutes is not written twice; past 256 KB the older half goes. */

#define LIMIT (256u << 10)
#define REPEAT 600

static bool enabled = true, busy;
static struct {
    char kind;
    uint32_t hash, time;
} recent[8];
static int recent_at;

static uint32_t hash_of(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void trim(void)
{
    char *data;
    size_t size;
    if (vfs_read_all(TIMELINE_FILE, &data, &size))
        return;
    size_t from = size / 2;
    while (from < size && data[from - 1] != '\n')
        from++;
    vfs_write_all(TIMELINE_FILE, data + from, size - from);
    kfree(data);
}

static void clean(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    for (; s && *s && k + 1 < cap; s++)
        out[k++] = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
    out[k] = 0;
}

void timeline_add(char kind, const char *title, const char *target)
{
    if (!enabled || busy || !target || !*target || !vfs_is_dir("/home"))
        return;
    uint32_t now = time_now(), h = hash_of(target);
    for (int i = 0; i < 8; i++)
        if (recent[i].kind == kind && recent[i].hash == h && now - recent[i].time < REPEAT)
            return;
    recent[recent_at].kind = kind;
    recent[recent_at].hash = h;
    recent[recent_at].time = now;
    recent_at = (recent_at + 1) % 8;
    busy = true;
    char t[96], p[PATH_MAX_LEN], line[PATH_MAX_LEN + 128];
    clean(t, sizeof(t), title && *title ? title : path_basename(target));
    clean(p, sizeof(p), target);
    int n = ksnprintf(line, sizeof(line), "%u\t%c\t%s\t%s\n", now, kind, t, p);
    struct file *f;
    if (vfs_open(TIMELINE_FILE, VFS_WRITE | VFS_CREATE, &f) == 0) {
        struct vstat st = {0};
        vfs_fstat(f, &st);
        vfs_seek(f, 0, VFS_SEEK_END);
        vfs_write(f, line, (size_t)n);
        vfs_close(f);
        if (st.size > LIMIT)
            trim();
    }
    busy = false;
}

static void get_timeline(char *buf, size_t cap)
{
    str_copy(buf, enabled ? "on" : "off", cap);
}

static int set_timeline(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    enabled = !strcmp(value, "on");
    return 0;
}

static const struct setting s_timeline = {"timeline", "on|off",
                                          "remember the apps, files and web pages you open (Timeline)", get_timeline,
                                          set_timeline};
SETTING(s_timeline);

static const char *kind_name(char k)
{
    return k == 'a' ? "app" : k == 'f' ? "opened" : k == 's' ? "saved" : k == 'w' ? "web" : "?";
}

COMMAND(timeline, NULL, "files", "timeline [today | <days>] [<word>] | clear",
        "what you opened, saved and visited, newest last; a word keeps only the lines with it")
{
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        vfs_unlink(TIMELINE_FILE);
        return 0;
    }
    int days = 1, a = 1;
    if (argc > a && !strcmp(argv[a], "today"))
        a++;
    else if (argc > a && argv[a][0] >= '0' && argv[a][0] <= '9')
        days = (int)str_to_long(argv[a++], NULL, 10);
    const char *word = argc > a ? argv[a] : NULL;
    char *data;
    size_t size;
    if (vfs_read_all(TIMELINE_FILE, &data, &size)) {
        sh_puts(sh, "nothing yet\n");
        return 0;
    }
    int zone = time_zone() * 60;
    uint32_t now = time_now() + (uint32_t)zone, from = now - now % 86400 - (uint32_t)(days - 1) * 86400;
    int shown = 0;
    for (char *line = data; line < data + size;) {
        char *end = memchr(line, '\n', (size_t)(data + size - line));
        if (!end)
            break;
        *end = 0;
        char *f[4] = {line};
        for (int i = 1; i < 4 && f[i - 1]; i++) {
            f[i] = strchr(f[i - 1], '\t');
            if (f[i])
                *f[i]++ = 0;
        }
        uint32_t t = (uint32_t)str_to_long(f[0], NULL, 10) + (uint32_t)zone;
        if (f[3] && t >= from && (!word || strstr(f[2], word) || strstr(f[3], word))) {
            char when[24];
            time_format(t, when, sizeof(when));
            sh_printf(sh, "%s  %-6s  %s", when + 11, kind_name(f[1][0]), f[2]);
            if (f[1][0] != 'a')
                sh_printf(sh, "  (%s)", f[3]);
            sh_puts(sh, "\n");
            shown++;
        }
        line = end + 1;
    }
    kfree(data);
    if (!shown)
        sh_puts(sh, "nothing yet\n");
    return 0;
}
