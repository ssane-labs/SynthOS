#include <synth/command.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/gui.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/stream.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>

/* Who may do what with files. Administrators may do anything; a standard account's commands (and the programs
   they start) and what it does in Files are held to these rules by the file system (vfs_guard):
     - its own home, /tmp, /home/Public, the Trash and removable drives (/mnt) are its own: read and write;
     - the rest of /home (other people's homes, the administrator's files, the account list) is closed, except
       shared program data (/home/programdata), which can be read;
     - the system (/system, /apps, /etc, Program Files, Windows, ...) can be read, not changed.
   Rules in /etc/acl change that for a path and everything under it: "allow|deny <user|everyone> read|write <path>";
   the most specific path wins, deny before allow. 'acl' edits them (administrators only), and 'sudo' (runas) runs one
   command with an administrator's rights once that administrator's password is given, as UAC does. */

#define ACL_FILE "/etc/acl"
#define MAX_RULES 64
#define RELOAD_MS 2000

struct rule {
    bool allow, write;
    char who[USER_NAME_MAX];
    char path[PATH_MAX_LEN];
};

static struct rule rules[MAX_RULES];
static int nrules;
static uint64_t loaded_at;
static bool loaded;

/* the next space-separated word of *p */
static bool next_word(char **p, char *out, size_t cap)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
    char *s = *p;
    while (**p && **p != ' ' && **p != '\t')
        (*p)++;
    if (*p == s)
        return false;
    str_copy(out, s, MIN(cap, (size_t)(*p - s) + 1));
    return true;
}

static bool parse(char *line, struct rule *r)
{
    char verb[8], what[8];
    if (line[0] == '#' || !next_word(&line, verb, sizeof(verb)) || !next_word(&line, r->who, sizeof(r->who)) ||
        !next_word(&line, what, sizeof(what)))
        return false;
    while (*line == ' ' || *line == '\t')
        line++;
    if (strcmp(verb, "allow") && strcmp(verb, "deny"))
        return false;
    if (strcmp(what, "read") && strcmp(what, "write"))
        return false;
    if (line[0] != '/')
        return false;
    r->allow = !strcmp(verb, "allow");
    r->write = !strcmp(what, "write");
    str_copy(r->path, line, sizeof(r->path));
    size_t n = strlen(r->path);
    while (n > 1 && r->path[n - 1] == '/')
        r->path[--n] = 0;
    return true;
}

static void load(void)
{
    nrules = 0;
    loaded = true;
    loaded_at = hal_ticks_ms();
    char *text;
    size_t size;
    if (vfs_read_all(ACL_FILE, &text, &size) != 0)
        return;
    for (char *line = text; line < text + size && nrules < MAX_RULES;) {
        char *end = memchr(line, '\n', (size_t)(text + size - line));
        if (end)
            *end = 0;
        if (parse(line, &rules[nrules]))
            nrules++;
        line = end ? end + 1 : text + size;
    }
    kfree(text);
}

static bool save(void)
{
    char *text = kmalloc((size_t)nrules * (PATH_MAX_LEN + 64) + 128);
    if (!text)
        return false;
    size_t n = (size_t)ksnprintf(text, 128, "# file access rules: allow|deny <user|everyone> read|write <path>\n");
    for (int i = 0; i < nrules; i++)
        n += (size_t)ksnprintf(text + n, PATH_MAX_LEN + 64, "%s %s %s %s\n", rules[i].allow ? "allow" : "deny",
                               rules[i].who, rules[i].write ? "write" : "read", rules[i].path);
    bool ok = vfs_write_all(ACL_FILE, text, n) == 0;
    kfree(text);
    loaded_at = hal_ticks_ms();
    return ok;
}

/* path is path or below it */
static bool under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/')
        return true;
    return !strncmp(path, dir, n) && (path[n] == '/' || !path[n]);
}

static bool by_default(const char *home, const char *path, bool write)
{
    if (under(path, home) && strcmp(home, "/home"))
        return true;
    static const char *const own[] = {"/tmp", "/home/Public", "/home/.Trash", "/mnt"};
    for (size_t i = 0; i < ARRAY_SIZE(own); i++)
        if (under(path, own[i]))
            return true;
    if (!strcmp(path, "/home")) /* the list of homes can be seen */
        return !write;
    if (under(path, "/home"))
        return !write && under(path, "/home/programdata");
    return !write;
}

static bool allowed(const char *user, const char *home, const char *path, bool write)
{
    if (!loaded || hal_ticks_ms() - loaded_at > RELOAD_MS)
        load();
    const struct rule *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < nrules; i++) {
        const struct rule *r = &rules[i];
        if ((strcmp(r->who, "everyone") && strcmp(r->who, user)) || !under(path, r->path))
            continue;
        if (write && !r->write && r->allow) /* allowing reading says nothing about writing */
            continue;
        if (!write && r->write && !r->allow) /* refusing writing says nothing about reading */
            continue;
        size_t n = strlen(r->path);
        if (!best || n > best_len || (n == best_len && !r->allow)) {
            best = r;
            best_len = n;
        }
    }
    return best ? best->allow : by_default(home, path, write);
}

static const struct file_access access_rules = {allowed};

/* ---- commands ---- */

static bool need_admin(struct shell *sh, const char *who)
{
    if (user_current()->admin || sh->elevated)
        return true;
    sh_error(sh, who, "only an administrator can change file access (try sudo)");
    return false;
}

COMMAND(acl, "icacls", "system",
        "acl [list] | acl allow|deny <user|everyone> read|write <path> | acl remove <n> | acl check <user> <path>",
        "who may read and change files: the rules over the defaults")
{
    load();
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "list"))) {
        if (!nrules)
            sh_printf(sh, "no rules: the defaults apply (own home and /tmp writable, the system read-only, other "
                          "homes closed)\n");
        for (int i = 0; i < nrules; i++)
            sh_printf(sh, "%2d  %-5s %-12s %-5s %s\n", i + 1, rules[i].allow ? "allow" : "deny", rules[i].who,
                      rules[i].write ? "write" : "read", rules[i].path);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "check")) {
        const struct user *u = user_find(argv[2]);
        if (!u)
            return sh_error(sh, "acl", "no user called %s", argv[2]);
        char path[PATH_MAX_LEN];
        if (sh_resolve(sh, argv[3], path, sizeof(path)) != 0)
            return sh_error(sh, "acl", "bad path: %s", argv[3]);
        bool r = u->admin || allowed(u->name, u->home, path, false);
        bool w = u->admin || allowed(u->name, u->home, path, true);
        sh_printf(sh, "%s: %s\n", u->name, w ? "read and write" : r ? "read only" : "no access");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "remove")) {
        long n;
        if (!need_admin(sh, "acl"))
            return 1;
        if (!str_parse_long(argv[2], &n) || n < 1 || n > nrules)
            return sh_error(sh, "acl", "no rule %s", argv[2]);
        memmove(&rules[n - 1], &rules[n], sizeof(rules[0]) * (size_t)(nrules - n));
        nrules--;
        return save() ? 0 : sh_error(sh, "acl", "could not write %s", ACL_FILE);
    }
    if (argc >= 5 && (!strcmp(argv[1], "allow") || !strcmp(argv[1], "deny"))) {
        if (!need_admin(sh, "acl"))
            return 1;
        if (strcmp(argv[2], "everyone") && !user_find(argv[2]))
            return sh_error(sh, "acl", "no user called %s", argv[2]);
        if (strcmp(argv[3], "read") && strcmp(argv[3], "write"))
            return command_usage(sh, command_find("acl"));
        if (nrules >= MAX_RULES)
            return sh_error(sh, "acl", "too many rules");
        char path[PATH_MAX_LEN], line[PATH_MAX_LEN + 64];
        if (sh_resolve(sh, argv[4], path, sizeof(path)) != 0)
            return sh_error(sh, "acl", "bad path: %s", argv[4]);
        ksnprintf(line, sizeof(line), "%s %s %s %s", argv[1], argv[2], argv[3], path);
        if (!parse(line, &rules[nrules]))
            return command_usage(sh, command_find("acl"));
        nrules++;
        if (!save())
            return sh_error(sh, "acl", "could not write %s", ACL_FILE);
        sh_printf(sh, "%s %s %s %s\n", argv[1], argv[2], argv[3], path);
        return 0;
    }
    return command_usage(sh, command_find("acl"));
}

/* the administrator to ask: the one named, else the first one with a password, else the first one */
static const struct user *pick_admin(const char *name)
{
    if (name)
        return user_find(name);
    const struct user *first = NULL;
    for (int i = 0; i < users_count(); i++) {
        const struct user *u = users_get(i);
        if (!u->admin)
            continue;
        if (u->has_password)
            return u;
        if (!first)
            first = u;
    }
    return first;
}

COMMAND(sudo, "runas", "system", "sudo [-S] [-u administrator] command...",
        "run one command with an administrator's rights (-S: read the password from the input)")
{
    bool from_input = false;
    const char *who = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-S"))
            from_input = true;
        else if (!strcmp(argv[i], "-u") && i + 1 < argc)
            who = argv[++i];
        else
            break;
    }
    if (i >= argc)
        return command_usage(sh, command_find("sudo"));
    const struct user *me = user_current();
    if (!me->admin && !sh->elevated) {
        const struct user *admin = pick_admin(who);
        if (!admin || !admin->admin)
            return sh_error(sh, "sudo", who ? "%s is not an administrator" : "there is no administrator%s",
                            who ? who : "");
        if (admin->has_password) {
            char secret[128] = "", prompt[80];
            int err = 0;
            if (from_input) {
                long n = stream_getline(sh->in, secret, sizeof(secret));
                err = n < 0 ? E_IO : 0;
                size_t len = strlen(secret);
                while (len && (secret[len - 1] == '\n' || secret[len - 1] == '\r'))
                    secret[--len] = 0;
            } else {
                ksnprintf(prompt, sizeof(prompt), "Password for %s: ", admin->name);
                err = user_read_password(prompt, secret, sizeof(secret));
            }
            bool ok = !err && user_check(admin->name, secret);
            memset(secret, 0, sizeof(secret));
            if (!ok)
                return sh_error(sh, "sudo", "the password for %s is not right", admin->name);
        }
    }
    char *line = kmalloc(2048);
    if (!line)
        return sh_error(sh, "sudo", "out of memory");
    line[0] = 0;
    for (int k = i; k < argc; k++) { /* quote each word again so the command sees what was typed */
        if (k > i)
            str_append(line, " ", 2048);
        str_append(line, "'", 2048);
        for (const char *p = argv[k]; *p; p++)
            str_append(line, *p == '\'' ? "'\\''" : (char[]){*p, 0}, 2048);
        str_append(line, "'", 2048);
    }
    bool was = sh->elevated;
    sh->elevated = true;
    int rc = shell_exec(sh, line);
    sh->elevated = was;
    kfree(line);
    return rc;
}

static const struct command *const commands[] = {&cmddef_acl, &cmddef_sudo};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, "file-access",
                                      &access_rules};
GUI_APP_FULL(NULL, NULL, &ext);
