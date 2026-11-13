#include <synth/fmt.h>
#include <synth/gui.h>
#include <synth/heap.h>
#include <synth/module.h>
#include <synth/string.h>
#include <synth/users.h>

#include "../../win32/msiapi.h"

/* Custom actions, launch conditions and rollback for msiexec, which asks for the "msi-actions" service.
   The InstallExecuteSequence runs in order around msiexec's own work: the actions before InstallFiles first, then
   the files, shortcuts and registry, then the rest. Supported: set a property (51) or a folder (35), stop with an
   error (19), and run a program from the package's Binary table (2), from an installed file (18), from a folder (34)
   or named by a property (50). DLL and script actions need the Windows Installer's own API and are skipped with a
   note. When an action fails, the package's rollback actions run and msiexec takes everything written back out. */

#define MAX_STEPS 192
#define MAX_ACTIONS 128
#define TEXT 512

struct step {
    char action[72], cond[256];
    int seq;
};

struct action {
    char name[72], source[72], target[TEXT];
    int type;
};

struct state {
    struct msi_run *r;
    struct step steps[MAX_STEPS];
    struct action actions[MAX_ACTIONS];
    int nsteps, nactions, split;
    int rollback[16], commit[16], nrollback, ncommit;
    int ran, skipped;
};

static struct state *st; /* one installation at a time */

static void say(const char *fmt, ...)
{
    char line[TEXT];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    msi_api_say(st->r, line);
}

/* ---- conditions: the Windows Installer grammar ---- */

struct cond {
    const char *p;
};

static void space(struct cond *c)
{
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')
        c->p++;
}

static bool word(struct cond *c, const char *w)
{
    space(c);
    size_t n = strlen(w);
    if (str_ncasecmp(c->p, w, n) || is_alnum((uint8_t)c->p[n]) || c->p[n] == '_')
        return false;
    c->p += n;
    return true;
}

static bool cond_or(struct cond *c);

/* an operand: "text", a number, a property, %ENV, or $comp ?comp &feature !feature (3 installed here, 2 absent) */
static void value(struct cond *c, char *out, size_t cap)
{
    space(c);
    out[0] = 0;
    if (*c->p == '"') {
        const char *e = strchr(c->p + 1, '"');
        size_t n = e ? (size_t)(e - c->p - 1) : strlen(c->p + 1);
        str_copy(out, c->p + 1, MIN(cap, n + 1));
        c->p += n + 1 + (e ? 1 : 0);
        return;
    }
    char sigil = *c->p && strchr("%$?&!", *c->p) ? *c->p++ : 0;
    const char *s = c->p;
    while (is_alnum((uint8_t)*c->p) || *c->p == '_' || *c->p == '.' || (*c->p == '-' && c->p == s))
        c->p++;
    char name[80];
    str_copy(name, s, MIN(sizeof(name), (size_t)(c->p - s) + 1));
    if (!sigil) {
        if (is_digit((uint8_t)name[0]) || name[0] == '-')
            str_copy(out, name, cap);
        else
            msi_api_get(st->r, 0, name, out, (int)cap);
    } else if (sigil == '%') {
        str_copy(out, !strcmp(name, "USERNAME") ? user_current()->name : "", cap);
    } else if (sigil == '?' || sigil == '!') {
        str_copy(out, "2", cap);
    } else if (sigil == '$') {
        str_copy(out, msi_api_component(st->r, name) ? "3" : "2", cap);
    } else {
        bool on = false;
        char buf[80];
        for (int f = 0, n = msi_api_rows(st->r, "Feature"); f < n; f++)
            if (msi_api_str(st->r, "Feature", f, 0, buf, sizeof(buf)) && !strcmp(buf, name))
                on = msi_api_int(st->r, "Feature", f, 5) > 0;
        str_copy(out, on ? "3" : "2", cap);
    }
}

static bool number(const char *s, long *v)
{
    return s[0] && str_parse_long(s, v);
}

static bool term(struct cond *c)
{
    space(c);
    if (word(c, "NOT"))
        return !term(c);
    if (*c->p == '(') {
        c->p++;
        bool v = cond_or(c);
        space(c);
        if (*c->p == ')')
            c->p++;
        return v;
    }
    char a[TEXT], b[TEXT];
    value(c, a, sizeof(a));
    space(c);
    bool fold = *c->p == '~';
    if (fold)
        c->p++;
    static const char *const ops[] = {"<>", "><", "<<", ">>", ">=", "<=", "=", "<", ">"};
    int op = -1;
    for (int i = 0; i < (int)ARRAY_SIZE(ops) && op < 0; i++)
        if (!strncmp(c->p, ops[i], strlen(ops[i]))) {
            op = i;
            c->p += strlen(ops[i]);
        }
    long x, y;
    if (op < 0)
        return number(a, &x) ? x != 0 : a[0] != 0;
    value(c, b, sizeof(b));
    int cmp;
    if (op != 1 && op != 2 && op != 3 && number(a, &x) && number(b, &y))
        cmp = x < y ? -1 : x > y;
    else
        cmp = fold ? str_casecmp(a, b) : strcmp(a, b);
    size_t la = strlen(a), lb = strlen(b);
    switch (op) {
    case 0: return cmp != 0;
    case 1: /* contains */
        for (size_t i = 0; i + lb <= la; i++)
            if (!(fold ? str_ncasecmp(a + i, b, lb) : strncmp(a + i, b, lb)))
                return true;
        return false;
    case 2: return la >= lb && !(fold ? str_ncasecmp(a, b, lb) : strncmp(a, b, lb));
    case 3: return la >= lb && !(fold ? str_ncasecmp(a + la - lb, b, lb) : strncmp(a + la - lb, b, lb));
    case 4: return cmp >= 0;
    case 5: return cmp <= 0;
    case 6: return cmp == 0;
    case 7: return cmp < 0;
    default: return cmp > 0;
    }
}

static bool cond_and(struct cond *c)
{
    bool v = term(c);
    while (word(c, "AND"))
        v = term(c) && v;
    return v;
}

static bool cond_or(struct cond *c)
{
    bool v = cond_and(c);
    for (;;) {
        if (word(c, "OR"))
            v = cond_and(c) || v;
        else if (word(c, "XOR"))
            v = cond_and(c) != v;
        else if (word(c, "EQV"))
            v = cond_and(c) == v;
        else if (word(c, "IMP"))
            v = cond_and(c) || !v;
        else
            return v;
    }
}

/* an empty condition is true */
static bool holds(const char *text)
{
    struct cond c = {text};
    space(&c);
    return !*c.p || cond_or(&c);
}

/* ---- the tables ---- */

static void load(void)
{
    struct msi_run *r = st->r;
    for (int i = 0, n = msi_api_rows(r, "CustomAction"); i < n && st->nactions < MAX_ACTIONS; i++) {
        struct action *a = &st->actions[st->nactions++];
        msi_api_str(r, "CustomAction", i, 0, a->name, sizeof(a->name));
        a->type = msi_api_int(r, "CustomAction", i, 1);
        msi_api_str(r, "CustomAction", i, 2, a->source, sizeof(a->source));
        msi_api_str(r, "CustomAction", i, 3, a->target, sizeof(a->target));
    }
    for (int i = 0, n = msi_api_rows(r, "InstallExecuteSequence"); i < n && st->nsteps < MAX_STEPS; i++) {
        struct step *s = &st->steps[st->nsteps++];
        msi_api_str(r, "InstallExecuteSequence", i, 0, s->action, sizeof(s->action));
        msi_api_str(r, "InstallExecuteSequence", i, 1, s->cond, sizeof(s->cond));
        s->seq = msi_api_int(r, "InstallExecuteSequence", i, 2);
    }
    for (int i = 1; i < st->nsteps; i++) /* by sequence number */
        for (int j = i; j > 0 && st->steps[j].seq < st->steps[j - 1].seq; j--) {
            struct step tmp = st->steps[j];
            st->steps[j] = st->steps[j - 1];
            st->steps[j - 1] = tmp;
        }
    st->split = st->nsteps;
    for (int i = 0; i < st->nsteps && st->split == st->nsteps; i++)
        if (!strcmp(st->steps[i].action, "InstallFiles") || st->steps[i].seq >= 4000)
            st->split = i;
}

/* every LaunchCondition must hold, or the package says why it cannot install */
static bool launch_conditions(void)
{
    char cond[TEXT], text[TEXT], msg[TEXT];
    for (int i = 0, n = msi_api_rows(st->r, "LaunchCondition"); i < n; i++) {
        msi_api_str(st->r, "LaunchCondition", i, 0, cond, sizeof(cond));
        if (holds(cond))
            continue;
        msi_api_str(st->r, "LaunchCondition", i, 1, text, sizeof(text));
        msi_api_format(st->r, text, msg, sizeof(msg));
        msi_api_say(st->r, msg);
        return false;
    }
    return true;
}

static int find_action(const char *name)
{
    for (int i = 0; i < st->nactions; i++)
        if (!strcmp(st->actions[i].name, name))
            return i;
    return -1;
}

/* ---- running the actions ---- */

enum { CA_OK, CA_FAILED, CA_STOP };

static int run(int index)
{
    struct msi_run *r = st->r;
    const struct action *a = &st->actions[index];
    char target[TEXT], exe[260] = "", dir[260] = "";
    msi_api_format(r, a->target, target, sizeof(target));
    int base = a->type & 7, from = a->type & 0x30;
    bool ignore = a->type & 0xc0; /* "continue on error", or not waited for on Windows */
    if (base == 3 && (from == 0x30 || from == 0x20)) { /* 51 property, 35 folder = formatted text */
        msi_api_set(r, a->source, target, from == 0x20);
        st->ran++;
        return CA_OK;
    }
    if (base == 3 && from == 0x10) { /* 19: stop with a message */
        say("%s", target[0] ? target : "The package stopped the installation.");
        return CA_STOP;
    }
    if (base != 2) {
        static const char *const kinds[8] = {"an unknown", "a DLL", "an unknown", "an unknown", "an unknown",
                                             "a JScript", "a VBScript", "a nested install"};
        say("Skipped custom action %s (%s action needs the Windows Installer service).", a->name, kinds[base]);
        st->skipped++;
        return CA_OK;
    }
    const char *args = target;
    if (from == 0x00) { /* a program kept in the Binary table, written to the temp folder first */
        char stream[96];
        ksnprintf(stream, sizeof(stream), "Binary.%s", a->source);
        ksnprintf(exe, sizeof(exe), "C:\\Windows\\Temp\\msi-%s.exe", a->source);
        if (!msi_api_save(r, stream, exe))
            exe[0] = 0;
    } else if (from == 0x10) {
        msi_api_get(r, 2, a->source, exe, sizeof(exe));
        msi_api_get(r, 1, "INSTALLDIR", dir, sizeof(dir));
    } else if (from == 0x30) {
        msi_api_get(r, 0, a->source, exe, sizeof(exe));
    } else { /* 34: Source is the folder, Target the whole command line */
        msi_api_get(r, 1, a->source, dir, sizeof(dir));
        const char *s = target;
        char quote = *s == '"' ? *s++ : ' ';
        const char *e = strchr(s, quote);
        size_t n = e ? (size_t)(e - s) : strlen(s);
        str_copy(exe, s, MIN(sizeof(exe), n + 1));
        args = e ? e + 1 : "";
        if (!strchr(exe, '\\') && dir[0]) {
            char full[260];
            ksnprintf(full, sizeof(full), "%s%s%s", dir, dir[strlen(dir) - 1] == '\\' ? "" : "\\", exe);
            str_copy(exe, full, sizeof(exe));
        }
    }
    say("Running custom action %s", a->name);
    int rc = exe[0] ? msi_api_run(r, exe, args, dir) : -1;
    st->ran++;
    if (rc == 0 || ignore)
        return CA_OK;
    if (rc < 0)
        say("Custom action %s could not start %s.", a->name, exe[0] ? exe : a->source);
    else
        say("Custom action %s failed (exit code %d).", a->name, rc);
    return CA_FAILED;
}

/* the steps in [from, to) that are custom actions; rollback and commit ones are kept for later */
static int run_steps(int from, int to)
{
    for (int i = from; i < to; i++) {
        int index = find_action(st->steps[i].action);
        if (index < 0 || !holds(st->steps[i].cond))
            continue;
        int type = st->actions[index].type;
        if (type & 0x100) {
            if (st->nrollback < (int)ARRAY_SIZE(st->rollback))
                st->rollback[st->nrollback++] = index;
        } else if (type & 0x200) {
            if (st->ncommit < (int)ARRAY_SIZE(st->commit))
                st->commit[st->ncommit++] = index;
        } else {
            int rc = run(index);
            if (rc != CA_OK)
                return rc;
        }
    }
    return CA_OK;
}

static void done(void)
{
    kfree(st);
    st = NULL;
}

static int before(struct msi_run *r)
{
    done();
    if (!(st = kzalloc(sizeof(*st))))
        return 1601;
    st->r = r;
    /* what the Windows Installer tells packages about the machine */
    static const char *const facts[][2] = {{"VersionNT", "603"}, {"WindowsBuild", "9600"}, {"Privileged", "1"},
                                           {"AdminUser", "1"}, {"UILevel", "2"}, {"UserLanguageID", "1033"},
                                           {"SystemLanguageID", "1033"}};
    char have[16];
    for (size_t i = 0; i < ARRAY_SIZE(facts); i++)
        if (!msi_api_get(r, 0, facts[i][0], have, sizeof(have)))
            msi_api_set(r, facts[i][0], facts[i][1], 0);
#ifdef __x86_64__
    if (!msi_api_get(r, 0, "VersionNT64", have, sizeof(have)))
        msi_api_set(r, "VersionNT64", "603", 0);
#endif
    if (!msi_api_get(r, 0, "LogonUser", have, sizeof(have)))
        msi_api_set(r, "LogonUser", user_current()->name, 0);
    load();
    if (!launch_conditions() || run_steps(0, st->split) != CA_OK) {
        msi_api_say(r, "Nothing was installed.");
        done();
        return 1603;
    }
    return 0;
}

static int after(struct msi_run *r)
{
    if (!st || st->r != r)
        return 0;
    int rc = 0;
    if (run_steps(st->split, st->nsteps) != CA_OK) {
        msi_api_say(r, "Rolling back the installation.");
        while (st->nrollback) /* the package's own undo steps, newest first; msiexec removes what was written */
            run(st->rollback[--st->nrollback]);
        rc = 1603;
    } else {
        for (int i = 0; i < st->ncommit; i++)
            run(st->commit[i]);
        if (st->ran || st->skipped)
            say("Ran %d custom actions, skipped %d.", st->ran, st->skipped);
    }
    done();
    return rc;
}

static const struct msi_actions actions = {before, after};
static const struct module_ext ext = {NULL, 0, NULL, NULL, NULL, NULL, NULL, "msi-actions", &actions};
GUI_APP_FULL(NULL, NULL, &ext);
