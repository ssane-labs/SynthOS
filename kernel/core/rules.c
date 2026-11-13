#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/rules.h>
#include <synth/shell.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/vfs.h>

/* Rules: "when this happens, run that command", one per line of /home/.rules ("when<TAB>command"). A rule's
   output goes to the system log. */

#define RULES_FILE "/home/.rules"
#define MAX_RULES 32

struct rule {
    char when[64], cmd[192];
};

static struct rule *rules;
static int count = -1;
static bool busy;
static struct shell *runner;

static void load(void)
{
    if (count >= 0)
        return;
    count = 0;
    if (!rules && !(rules = kzalloc(sizeof(*rules) * MAX_RULES)))
        return;
    char *data;
    size_t size;
    if (vfs_read_all(RULES_FILE, &data, &size))
        return;
    for (char *line = data; line < data + size && count < MAX_RULES;) {
        char *end = memchr(line, '\n', (size_t)(data + size - line));
        if (end)
            *end = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            str_copy(rules[count].when, line, sizeof(rules[count].when));
            str_copy(rules[count].cmd, tab + 1, sizeof(rules[count].cmd));
            count++;
        }
        line = end ? end + 1 : data + size;
    }
    kfree(data);
}

static int save(void)
{
    char *text = kmalloc(MAX_RULES * 260 + 1);
    if (!text)
        return E_NOMEM;
    size_t n = 0;
    for (int i = 0; i < count; i++)
        n += (size_t)ksnprintf(text + n, MAX_RULES * 260 + 1 - n, "%s\t%s\n", rules[i].when, rules[i].cmd);
    int err = count ? vfs_write_all(RULES_FILE, text, n) : vfs_unlink(RULES_FILE);
    kfree(text);
    return err == E_NOENT ? 0 : err;
}

/* "at" events carry "HH:MM d" (d: weekday, 0 Sunday); rules say "at HH:MM", "at HH:MM mon,fri" or "every N"
   (minutes) */
static bool on_time(const char *when, const char *subject)
{
    if (!subject || strlen(subject) < 5)
        return false;
    int minute = (int)str_to_long(subject, NULL, 10) * 60 + (int)str_to_long(subject + 3, NULL, 10);
    if (!strncmp(when, "every ", 6)) {
        long n = str_to_long(when + 6, NULL, 10);
        return n > 0 && minute % n == 0;
    }
    if (strncmp(when, "at ", 3) || strncmp(when + 3, subject, 5))
        return false;
    static const char days[] = "sunmontuewedthufrisat";
    return !when[8] || (subject[5] && subject[6] >= '0' && subject[6] <= '6' && strstr(when + 9, (char[4]){days[(subject[6] - '0') * 3], days[(subject[6] - '0') * 3 + 1],
                                                    days[(subject[6] - '0') * 3 + 2], 0}) != NULL);
}

static bool matches(const char *when, const char *kind, const char *subject)
{
    if (!strcmp(kind, "at"))
        return on_time(when, subject);
    size_t k = strlen(kind);
    if (strncmp(when, kind, k) || (when[k] && when[k] != ' '))
        return false;
    const char *arg = when[k] ? when + k + 1 : "";
    if (!strcmp(kind, "save"))
        return subject && !strncmp(subject, arg, strlen(arg));
    return !*arg || (subject && !str_casecmp(arg, subject));
}

static int run(struct shell *sh, const char *kind, const char *subject)
{
    load();
    int fired = 0;
    for (int i = 0; i < count; i++)
        if (matches(rules[i].when, kind, subject)) {
            klog("rules: %s -> %s", rules[i].when, rules[i].cmd);
            shell_exec(sh, rules[i].cmd);
            fired++;
        }
    return fired;
}

/* Events wait in a small queue: they can come from inside a file operation, and the commands run later, from the
   desktop's loop or before the shell's next prompt. */
static struct {
    char kind[8], subject[160];
} queue[8];
static int queued;

void rules_event(const char *kind, const char *subject)
{
    if (busy || queued == 8 || !vfs_is_dir("/home"))
        return;
    load();
    if (!count)
        return;
    bool fits = false;
    for (int i = 0; i < count && !fits; i++)
        fits = matches(rules[i].when, kind, subject);
    if (!fits)
        return;
    str_copy(queue[queued].kind, kind, sizeof(queue[queued].kind));
    str_copy(queue[queued].subject, subject ? subject : "", sizeof(queue[queued].subject));
    queued++;
}

void rules_run_pending(void)
{
    if (!queued || busy)
        return;
    if (!runner && (runner = shell_create()) != NULL)
        runner->out = runner->err = stream_log();
    if (!runner)
        return;
    busy = true;
    while (queued) {
        char kind[8], subject[160];
        str_copy(kind, queue[0].kind, sizeof(kind));
        str_copy(subject, queue[0].subject, sizeof(subject));
        memmove(queue, queue + 1, sizeof(queue[0]) * (size_t)--queued);
        run(runner, kind, subject);
    }
    busy = false;
}

static bool valid_when(const char *w)
{
    if (!strncmp(w, "at ", 3))
        return strlen(w) >= 8 && w[5] == ':' && w[3] >= '0' && w[3] <= '2' && w[6] >= '0' && w[6] <= '5' &&
               (!w[8] || w[8] == ' ');
    if (!strncmp(w, "every ", 6))
        return str_to_long(w + 6, NULL, 10) > 0;
    return !strncmp(w, "open ", 5) || !strncmp(w, "save ", 5) || !strcmp(w, "login") || !strcmp(w, "start");
}

COMMAND(rules, "when", "system",
        "rules [add <when> -- <command> | remove <n> | run <when>]",
        "run commands by themselves: at HH:MM [days], every <minutes>, open <app>, save <folder>, login, start")
{
    load();
    if (argc == 1) {
        if (!count)
            sh_puts(sh, "no rules; e.g. rules add at 22:00 -- config nightshift on\n");
        for (int i = 0; i < count; i++)
            sh_printf(sh, "%2d  when %-24s  %s\n", i + 1, rules[i].when, rules[i].cmd);
        return 0;
    }
    if (!strcmp(argv[1], "add") || !strcmp(argv[1], "run")) {
        char when[64] = "", cmd[192] = "";
        int i = 2;
        for (; i < argc && strcmp(argv[i], "--"); i++) {
            if (when[0])
                str_append(when, " ", sizeof(when));
            str_append(when, argv[i], sizeof(when));
        }
        for (i++; i < argc; i++) {
            if (cmd[0])
                str_append(cmd, " ", sizeof(cmd));
            str_append(cmd, argv[i], sizeof(cmd));
        }
        if (!valid_when(when))
            return sh_error(sh, "rules", "when: at HH:MM [mon,tue…], every <minutes>, open <app>, save <folder>, login or start");
        if (!strcmp(argv[1], "run")) {
            char *space = strchr(when, ' ');
            if (space)
                *space++ = 0;
            if (!run(sh, when, space))
                sh_puts(sh, "no rule fits\n");
            return 0;
        }
        if (!cmd[0])
            return sh_error(sh, "rules", "say what to run after --");
        if (!rules || count >= MAX_RULES)
            return sh_error(sh, "rules", "there are %d rules already", MAX_RULES);
        str_copy(rules[count].when, when, sizeof(rules[count].when));
        str_copy(rules[count].cmd, cmd, sizeof(rules[count].cmd));
        count++;
        int err = save();
        if (err)
            return sh_error(sh, "rules", "%s", err_str(err));
        sh_printf(sh, "rule %d: when %s, %s\n", count, when, cmd);
        return 0;
    }
    if (!strcmp(argv[1], "remove") && argc == 3) {
        int k = (int)str_to_long(argv[2], NULL, 10) - 1;
        if (k < 0 || k >= count)
            return sh_error(sh, "rules", "no rule %s", argv[2]);
        memmove(&rules[k], &rules[k + 1], (size_t)(count - k - 1) * sizeof(*rules));
        count--;
        return save();
    }
    return command_usage(sh, command_find("rules"));
}
