#include <synth/command.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/path.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>
#include <synth/winshell.h>

#include "win32.h"

#define CLIENTS "SOFTWARE\\Clients\\StartMenuInternet"
#define URL_CHOICE "HKCU\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\%s\\UserChoice"
#define FILE_CHOICE "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice"

static bool reg_text(char *out, size_t cap, const char *name, const char *fmt, ...)
{
    char path[384];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    return w32_reg_read(path, name, out, cap) && out[0];
}

bool winshell_is_url(const char *t)
{
    size_t n = 0;
    while ((t[n] >= 'a' && t[n] <= 'z') || (t[n] >= 'A' && t[n] <= 'Z') ||
           (n && ((t[n] >= '0' && t[n] <= '9') || t[n] == '+' || t[n] == '-' || t[n] == '.')))
        n++;
    return n >= 2 && t[n] == ':';
}

static const char *variable(const char *name, size_t n, char *buf, size_t cap)
{
    static const char *const fixed[][2] = {
        {"SystemRoot", "C:\\Windows"},
        {"windir", "C:\\Windows"},
        {"SystemDrive", "C:"},
        {"ProgramFiles", "C:\\Program Files"},
        {"ProgramW6432", "C:\\Program Files"},
        {"ProgramFiles(x86)", "C:\\Program Files (x86)"},
        {"CommonProgramFiles", "C:\\Program Files\\Common Files"},
        {"ProgramData", "C:\\ProgramData"},
        {"ALLUSERSPROFILE", "C:\\ProgramData"},
        {"PUBLIC", "C:\\Users\\Public"},
    };
    static const char *const profile[][2] = {
        {"USERPROFILE", ""},
        {"APPDATA", "\\AppData\\Roaming"},
        {"LOCALAPPDATA", "\\AppData\\Local"},
        {"TEMP", "\\AppData\\Local\\Temp"},
        {"TMP", "\\AppData\\Local\\Temp"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(fixed); i++)
        if (strlen(fixed[i][0]) == n && !str_ncasecmp(name, fixed[i][0], n))
            return fixed[i][1];
    const char *user = user_current()->name;
    for (size_t i = 0; i < ARRAY_SIZE(profile); i++)
        if (strlen(profile[i][0]) == n && !str_ncasecmp(name, profile[i][0], n)) {
            ksnprintf(buf, cap, "C:\\Users\\%s%s", user, profile[i][1]);
            return buf;
        }
    if (n == 8 && !str_ncasecmp(name, "USERNAME", n))
        return user;
    return NULL;
}

static void put(char *out, size_t *o, size_t cap, const char *s)
{
    *o = MIN(*o + str_copy(out + *o, s, cap - *o), cap - 1);
}

/* a registered command with its placeholders filled: %1 %L %V the file or link, %* the parameters, %NAME% the
   environment; a command that never names the file gets it at the end, as Windows does */
static void fill(const char *command, const char *target, const char *params, char *out, size_t cap)
{
    size_t o = 0;
    bool named = false;
    out[0] = 0;
    for (const char *p = command; *p && o + 1 < cap;) {
        if (*p != '%') {
            out[o++] = *p++;
            continue;
        }
        const char *end = strchr(p + 1, '%');
        char buf[PATH_MAX_LEN];
        const char *value = end && end - p > 2 ? variable(p + 1, (size_t)(end - p - 1), buf, sizeof(buf)) : NULL;
        if (value) {
            put(out, &o, cap, value);
            p = end + 1;
            continue;
        }
        char c = p[1];
        if (c == '1' || c == 'L' || c == 'l' || c == 'V' || c == 'v') {
            put(out, &o, cap, target);
            named = true;
        } else if (c == '*') {
            put(out, &o, cap, params ? params : "");
        } else if (c == '%') {
            out[o++] = '%';
        } else if (!c || !strchr("023456789IiDdHhSsWwUu~", c)) {
            out[o++] = '%';
            p++;
            continue;
        }
        p += c ? 2 : 1;
    }
    out[MIN(o, cap - 1)] = 0;
    if (!named)
        ksnprintf(out + strlen(out), cap - strlen(out), " \"%s\"", target);
}

static bool key_exists(const char *cls)
{
    char dummy[8];
    return reg_text(dummy, sizeof(dummy), NULL, "HKCR\\%s\\shell\\open\\command", cls);
}

static bool class_command(const char *cls, const char *verb, char *out, size_t cap, int depth)
{
    char cur[128];
    if (depth < 3 && reg_text(cur, sizeof(cur), NULL, "HKCR\\%s\\CurVer", cls) && str_casecmp(cur, cls) &&
        class_command(cur, verb, out, cap, depth + 1))
        return true;
    if (reg_text(out, cap, NULL, "HKCR\\%s\\shell\\%s\\command", cls, verb))
        return true;
    char def[64];
    if (!str_casecmp(verb, "open") && reg_text(def, sizeof(def), NULL, "HKCR\\%s\\shell", cls) &&
        reg_text(out, cap, NULL, "HKCR\\%s\\shell\\%s\\command", cls, def))
        return true;
    return false;
}

static const char *plain_verb(const char *verb)
{
    return !verb || !*verb || !str_casecmp(verb, "runas") || !str_casecmp(verb, "opennew") ? "open" : verb;
}

/* the class (ProgID) behind ".ext" or a URL scheme: the user's choice first, then what HKCR says */
bool winshell_class(const char *assoc, char *cls, size_t cap)
{
    if (assoc[0] == '.')
        return reg_text(cls, cap, "ProgId", FILE_CHOICE, assoc) || reg_text(cls, cap, NULL, "HKCR\\%s", assoc) ||
               (reg_text(cls, cap, NULL, "HKCR\\SystemFileAssociations\\%s", assoc));
    char scheme[32];
    str_copy(scheme, assoc, sizeof(scheme));
    str_lower(scheme);
    if (reg_text(cls, cap, "ProgId", URL_CHOICE, scheme))
        return true;
    str_copy(cls, scheme, cap);
    return reg_text(scheme, sizeof(scheme), "URL Protocol", "HKCR\\%s", cls) || key_exists(cls);
}

/* the registered command line of ".ext", a scheme or a ProgID, placeholders unfilled */
bool winshell_assoc_command(const char *assoc, const char *verb, char *out, size_t cap)
{
    verb = plain_verb(verb);
    char cls[128];
    if (winshell_class(assoc, cls, sizeof(cls)) && class_command(cls, verb, out, cap, 0))
        return true;
    return class_command(assoc, verb, out, cap, 0);
}

/* The command line Windows would run to open a file (a SynthOS path) or a link with a verb, false when nothing is
   registered for it. */
bool winshell_command(const char *verb, const char *target, const char *params, char *out, size_t cap)
{
    char command[512], what[PATH_MAX_LEN];
    bool url = winshell_is_url(target);
    char key[32];
    const char *ext = url ? NULL : path_ext(target);
    if (url) {
        size_t n = (size_t)(strchr(target, ':') - target);
        str_copy(key, target, MIN(n + 1, sizeof(key)));
    }
    if ((!url && !ext) || !winshell_assoc_command(url ? key : ext, verb, command, sizeof(command)))
        return false;
    if (url)
        str_copy(what, target, sizeof(what));
    else
        winpath_from_vfs(target, what, sizeof(what));
    fill(command, what, params, out, cap);
    return true;
}

/* built-in tools have no file on disk: the shell runs them itself */
static bool builtin_program(const char *win)
{
    return !str_casecmp(path_basename(win), "msiexec.exe");
}

/* The program a Windows command line starts, as a SynthOS path, and where its arguments begin. An unquoted program
   path with spaces is tried a word at a time, as CreateProcess does. */
bool winshell_split(const char *cmdline, char *program, size_t cap, const char **args)
{
    while (*cmdline == ' ' || *cmdline == '\t')
        cmdline++;
    char win[PATH_MAX_LEN];
    const char *rest;
    if (*cmdline == '"') {
        const char *end = strchr(cmdline + 1, '"');
        if (!end)
            return false;
        str_copy(win, cmdline + 1, MIN((size_t)(end - cmdline), sizeof(win)));
        rest = end + 1;
    } else {
        rest = cmdline;
        for (;;) {
            const char *sp = strchr(rest, ' ');
            size_t n = sp ? (size_t)(sp - cmdline) : strlen(cmdline);
            str_copy(win, cmdline, MIN(n + 1, sizeof(win)));
            rest = cmdline + n;
            if (!sp || (winpath_to_vfs("/", win, program, cap) == 0 &&
                        (builtin_program(win) || (vfs_exists(program) && !vfs_is_dir(program)))))
                break;
            rest = sp + 1;
        }
    }
    while (*rest == ' ' || *rest == '\t')
        rest++;
    if (args)
        *args = rest;
    return winpath_to_vfs("/", win, program, cap) == 0 && (vfs_exists(program) || builtin_program(win));
}

static bool browser_at(const char *root, const char *key, struct winshell_browser *b)
{
    char command[512];
    if (!reg_text(command, sizeof(command), NULL, "%s\\%s\\%s\\shell\\open\\command", root, CLIENTS, key) ||
        !winshell_split(command, b->program, sizeof(b->program), NULL))
        return false;
    str_copy(b->key, key, sizeof(b->key));
    if (!reg_text(b->name, sizeof(b->name), "ApplicationName", "%s\\%s\\%s\\Capabilities", root, CLIENTS, key) &&
        !reg_text(b->name, sizeof(b->name), NULL, "%s\\%s\\%s", root, CLIENTS, key))
        str_copy(b->name, key, sizeof(b->name));
    return true;
}

bool winshell_browser(const char *key, struct winshell_browser *out)
{
    return key && key[0] && (browser_at("HKCU", key, out) || browser_at("HKLM", key, out));
}

struct listing {
    void (*fn)(const struct winshell_browser *b, void *ctx);
    void *ctx;
    const char *root;
    char seen[16][64];
    int count;
};

static void list_one(const char *key, void *ctx)
{
    struct listing *l = ctx;
    for (int i = 0; i < l->count; i++)
        if (!str_casecmp(l->seen[i], key))
            return;
    struct winshell_browser b;
    if (l->count >= (int)ARRAY_SIZE(l->seen) || !browser_at(l->root, key, &b))
        return;
    str_copy(l->seen[l->count++], key, sizeof(l->seen[0]));
    l->fn(&b, l->ctx);
}

/* every browser an installer registered, the user's own (per-user installs) first */
void winshell_browsers(void (*fn)(const struct winshell_browser *b, void *ctx), void *ctx)
{
    struct listing *l = kzalloc(sizeof(*l));
    if (!l)
        return;
    l->fn = fn;
    l->ctx = ctx;
    l->root = "HKCU";
    w32_reg_subkeys("HKCU\\" CLIENTS, list_one, l);
    l->root = "HKLM";
    w32_reg_subkeys("HKLM\\" CLIENTS, list_one, l);
    kfree(l);
}

static bool browser_class(const char *key, const char *kind, const char *name, char *cls, size_t cap)
{
    return reg_text(cls, cap, name, "HKCU\\%s\\%s\\Capabilities\\%s", CLIENTS, key, kind) ||
           reg_text(cls, cap, name, "HKLM\\%s\\%s\\Capabilities\\%s", CLIENTS, key, kind);
}

/* how a browser opens a page or a link: through the class it registered for it, else its own command line */
bool winshell_browser_command(const char *key, const char *target, char *out, size_t cap)
{
    bool url = winshell_is_url(target);
    char cls[128], command[512], what[PATH_MAX_LEN];
    bool found = url ? browser_class(key, "URLAssociations", "https", cls, sizeof(cls)) ||
                           browser_class(key, "URLAssociations", "http", cls, sizeof(cls))
                     : browser_class(key, "FileAssociations", ".html", cls, sizeof(cls)) ||
                           browser_class(key, "FileAssociations", ".htm", cls, sizeof(cls));
    found = found && class_command(cls, "open", command, sizeof(command), 0);
    if (!found && !reg_text(command, sizeof(command), NULL, "HKCU\\%s\\%s\\shell\\open\\command", CLIENTS, key) &&
        !reg_text(command, sizeof(command), NULL, "HKLM\\%s\\%s\\shell\\open\\command", CLIENTS, key))
        return false;
    if (url)
        str_copy(what, target, sizeof(what));
    else
        winpath_from_vfs(target, what, sizeof(what));
    fill(command, what, NULL, out, cap);
    return true;
}

struct finder {
    const char *progid;
    char *key;
    size_t cap;
    bool found;
};

static void match_default(const struct winshell_browser *b, void *ctx)
{
    struct finder *f = ctx;
    char cls[128];
    if (!f->found && browser_class(b->key, "URLAssociations", "http", cls, sizeof(cls)) && !str_casecmp(cls, f->progid)) {
        str_copy(f->key, b->key, f->cap);
        f->found = true;
    }
}

/* the browser the user chose for web links (HKCU UserChoice for http), when it is a registered one */
bool winshell_default_browser(char *key, size_t cap)
{
    char progid[128];
    if (!reg_text(progid, sizeof(progid), "ProgId", URL_CHOICE, "http"))
        return false;
    struct finder f = {progid, key, cap, false};
    winshell_browsers(match_default, &f);
    return f.found;
}

/* key NULL gives links back to SynthOS's own browser */
bool winshell_set_default_browser(const char *key)
{
    static const char *const schemes[] = {"http", "https"};
    static const char *const exts[] = {".html", ".htm"};
    char path[256], cls[128];
    for (size_t i = 0; i < ARRAY_SIZE(schemes); i++) {
        if (key && !browser_class(key, "URLAssociations", schemes[i], cls, sizeof(cls)))
            return false;
        ksnprintf(path, sizeof(path), URL_CHOICE, schemes[i]);
        w32_reg_write(path, "ProgId", key ? cls : "");
    }
    for (size_t i = 0; i < ARRAY_SIZE(exts); i++) {
        ksnprintf(path, sizeof(path), FILE_CHOICE, exts[i]);
        w32_reg_write(path, "ProgId", key && browser_class(key, "FileAssociations", exts[i], cls, sizeof(cls)) ? cls : "");
    }
    return true;
}

#define UNINSTALL "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"

struct programs {
    void (*fn)(const struct winshell_program *p, void *ctx);
    void *ctx;
    const char *root;
    struct winshell_program p;
};

static void program_one(const char *key, void *ctx)
{
    struct programs *l = ctx;
    struct winshell_program *p = &l->p;
    char hidden[8];
    memset(p, 0, sizeof(*p));
    if (!reg_text(p->name, sizeof(p->name), "DisplayName", "%s\\%s\\%s", l->root, UNINSTALL, key) ||
        reg_text(hidden, sizeof(hidden), "SystemComponent", "%s\\%s\\%s", l->root, UNINSTALL, key))
        return;
    str_copy(p->key, key, sizeof(p->key));
    reg_text(p->version, sizeof(p->version), "DisplayVersion", "%s\\%s\\%s", l->root, UNINSTALL, key);
    reg_text(p->publisher, sizeof(p->publisher), "Publisher", "%s\\%s\\%s", l->root, UNINSTALL, key);
    reg_text(p->uninstall, sizeof(p->uninstall), "UninstallString", "%s\\%s\\%s", l->root, UNINSTALL, key);
    l->fn(p, l->ctx);
}

void winshell_programs(void (*fn)(const struct winshell_program *p, void *ctx), void *ctx)
{
    struct programs *l = kzalloc(sizeof(*l));
    if (!l)
        return;
    l->fn = fn;
    l->ctx = ctx;
    l->root = "HKCU";
    w32_reg_subkeys("HKCU\\" UNINSTALL, program_one, l);
    l->root = "HKLM";
    w32_reg_subkeys("HKLM\\" UNINSTALL, program_one, l);
    kfree(l);
}

#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

struct run_names {
    char names[32][64];
    int n;
};

static void run_name(const char *name, void *ctx)
{
    struct run_names *l = ctx;
    if (l->n < 32 && name[0])
        str_copy(l->names[l->n++], name, sizeof(l->names[0]));
}

void winshell_run_entries(void (*fn)(const struct winshell_run *r, void *ctx), void *ctx)
{
    static const char *const keys[] = {"HKCU\\" RUN_KEY, "HKLM\\" RUN_KEY, "HKCU\\" RUN_KEY "Once", "HKLM\\" RUN_KEY "Once"};
    struct run_names *l = kmalloc(sizeof(*l));
    struct winshell_run *r = kmalloc(sizeof(*r));
    for (size_t i = 0; l && r && i < ARRAY_SIZE(keys); i++) {
        l->n = 0;
        w32_reg_values(keys[i], run_name, l);
        for (int k = 0; k < l->n; k++) {
            if (!w32_reg_read(keys[i], l->names[k], r->command, sizeof(r->command)) || !r->command[0])
                continue;
            str_copy(r->key, keys[i], sizeof(r->key));
            str_copy(r->name, l->names[k], sizeof(r->name));
            r->once = i >= 2;
            fn(r, ctx);
        }
    }
    kfree(l);
    kfree(r);
}

bool winshell_run_remove(const struct winshell_run *r)
{
    return w32_reg_delete_value(r->key, r->name);
}

/* start, as in Windows: a file or a link opens in the program its association names */
COMMAND(start, NULL, "system", "start <file|url>", "open a file or a link in the program registered for it")
{
    if (argc != 2)
        return command_usage(sh, command_find("start"));
    char path[PATH_MAX_LEN], program[PATH_MAX_LEN];
    const char *target = argv[1], *rest;
    if (!winshell_is_url(target)) {
        if (sh_resolve(sh, target, path, sizeof(path)) != 0 || !vfs_exists(path))
            return sh_error(sh, "start", "no such file: %s", target);
        target = path;
    }
    char *line = kmalloc(1024), *buf = kmalloc(1024), *args[64];
    int rc;
    if (!line || !buf) {
        rc = sh_error(sh, "start", "out of memory");
    } else if (!winshell_command("open", target, NULL, line, 1024)) {
        rc = sh_error(sh, "start", "no program is registered for %s", argv[1]);
    } else if (!winshell_split(line, program, sizeof(program), &rest)) {
        rc = sh_error(sh, "start", "the registered program is missing: %s", line);
    } else {
        args[0] = program;
        int n = 1 + w32_split_args(rest, buf, args + 1, 62);
        args[n] = NULL;
        if (builtin_program(program)) {
            rc = msiexec_main(sh, n, args);
        } else {
            KARR(char *) kargs[64];
            for (int i = 0; i <= n; i++)
                kargs[i] = (KARR(char *))(uintptr_t)args[i];
            struct exec_ctx ctx = {sh, program, n, kargs, sh->in, sh->out, sh->err, NULL, NULL, NULL};
            rc = exec_program(&ctx);
        }
    }
    kfree(line);
    kfree(buf);
    return rc;
}
