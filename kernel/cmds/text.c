#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#define LINE_CAP 1024

struct lines {
    char **items;
    size_t count;
    size_t cap;
};

static void lines_push(struct lines *l, const char *s)
{
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **items = krealloc(l->items, cap * sizeof(char *));
        if (!items)
            return;
        l->items = items;
        l->cap = cap;
    }
    l->items[l->count++] = kstrdup(s);
}

static void lines_free(struct lines *l)
{
    for (size_t i = 0; i < l->count; i++)
        kfree(l->items[i]);
    kfree(l->items);
}

static struct stream *open_input(struct shell *sh, const char *who, const char *arg)
{
    if (!arg || strcmp(arg, "-") == 0)
        return sh->in;
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, arg, path, sizeof(path));
    struct stream *s = err ? NULL : stream_file(path, VFS_READ, &err);
    if (!s)
        sh_fs_error(sh, who, arg, err);
    return s;
}

static void close_input(struct shell *sh, struct stream *s)
{
    if (s && s != sh->in)
        stream_close(s);
}

static int read_lines(struct shell *sh, const char *who, int argc, char **argv, int first, struct lines *out)
{
    char line[LINE_CAP];
    int rc = 0;
    for (int i = first; i < argc || i == first; i++) {
        struct stream *s = open_input(sh, who, i < argc ? argv[i] : NULL);
        if (!s) {
            rc = 1;
            continue;
        }
        while (stream_getline(s, line, sizeof(line)) >= 0)
            lines_push(out, line);
        close_input(sh, s);
    }
    return rc;
}

static void put_escaped(struct shell *sh, const char *s)
{
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            stream_putc(sh->out, *s);
            continue;
        }
        s++;
        switch (*s) {
        case 'n': stream_putc(sh->out, '\n'); break;
        case 't': stream_putc(sh->out, '\t'); break;
        case 'r': stream_putc(sh->out, '\r'); break;
        case 'a': stream_putc(sh->out, '\a'); break;
        case 'b': stream_putc(sh->out, '\b'); break;
        case 'e': stream_putc(sh->out, 0x1b); break;
        case '\\': stream_putc(sh->out, '\\'); break;
        case 'x': {
            int v = 0, n = 0;
            while (n < 2 && is_xdigit((uint8_t)s[1])) {
                s++;
                v = v * 16 + (is_digit((uint8_t)*s) ? *s - '0' : to_lower((uint8_t)*s) - 'a' + 10);
                n++;
            }
            stream_putc(sh->out, (char)v);
            break;
        }
        default:
            stream_putc(sh->out, '\\');
            stream_putc(sh->out, *s);
            break;
        }
    }
}

COMMAND(echo, NULL, "text", "echo [-n] [-e] [-m] text...", "print text (-e escapes, -m {role} colors)")
{
    bool newline = true, escapes = false, markup = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        bool valid = true;
        for (const char *p = f; *p; p++)
            valid &= *p == 'n' || *p == 'e' || *p == 'm';
        if (!valid)
            break;
        for (const char *p = f; *p; p++) {
            newline &= *p != 'n';
            escapes |= *p == 'e';
            markup |= *p == 'm';
        }
    }
    for (int first = i; i < argc; i++) {
        if (i > first)
            stream_putc(sh->out, ' ');
        if (markup && stream_is_console(sh->out)) {
            con_markup(argv[i]);
        } else if (markup) {
            size_t n = strlen(argv[i]) + 1;
            char *plain = kmalloc(n);
            if (plain) {
                markup_strip(argv[i], plain, n);
                sh_puts(sh, plain);
                kfree(plain);
            }
        } else if (escapes) {
            put_escaped(sh, argv[i]);
        } else {
            sh_puts(sh, argv[i]);
        }
    }
    if (markup)
        sh_reset(sh);
    if (newline)
        stream_putc(sh->out, '\n');
    return 0;
}

COMMAND(printf, NULL, "text", "printf format [args...]", "formatted output (%s %d %x %c %%)")
{
    if (argc < 2)
        return command_usage(sh, command_find("printf"));
    int arg = 2;
    for (const char *p = argv[1]; *p; p++) {
        if (*p == '\\' && p[1]) {
            char esc[3] = {'\\', p[1], 0};
            put_escaped(sh, esc);
            p++;
            continue;
        }
        if (*p != '%' || !p[1]) {
            stream_putc(sh->out, *p);
            continue;
        }
        char spec[16];
        size_t n = 0;
        spec[n++] = *p++;
        while (*p && strchr("-0123456789.", *p) && n < sizeof(spec) - 2)
            spec[n++] = *p++;
        spec[n++] = *p;
        spec[n] = 0;
        const char *value = arg < argc ? argv[arg++] : "";
        long num = 0;
        switch (*p) {
        case 'd':
        case 'i':
        case 'x':
        case 'X':
        case 'u':
        case 'o':
            str_parse_long(value, &num);
            spec[n - 1] = 'l';
            spec[n] = *p;
            spec[n + 1] = 0;
            sh_printf(sh, spec, num);
            break;
        case 'c':
            stream_putc(sh->out, value[0]);
            break;
        case 's':
            sh_printf(sh, spec, value);
            break;
        case '%':
            stream_putc(sh->out, '%');
            arg--;
            break;
        default:
            sh_puts(sh, spec);
            break;
        }
        if (!*p)
            break;
    }
    return 0;
}

static int head_tail(struct shell *sh, int argc, char **argv, bool tail)
{
    long count = 10;
    struct getopt g;
    getopt_init(&g, argc, argv);
    int c;
    while ((c = getopt_next(&g, "n:")) != -1) {
        if (c != 'n' || !str_parse_long(g.arg, &count) || count < 0)
            return command_usage(sh, command_find(tail ? "tail" : "head"));
    }
    struct lines l = {0};
    int rc = read_lines(sh, argv[0], argc, argv, g.index, &l);
    size_t start = tail && l.count > (size_t)count ? l.count - (size_t)count : 0;
    size_t end = tail ? l.count : MIN(l.count, (size_t)count);
    for (size_t i = start; i < end; i++)
        sh_printf(sh, "%s\n", l.items[i]);
    lines_free(&l);
    return rc;
}

COMMAND(head, NULL, "text", "head [-n N] [file...]", "print the first lines")
{
    return head_tail(sh, argc, argv, false);
}

COMMAND(tail, NULL, "text", "tail [-n N] [file...]", "print the last lines")
{
    return head_tail(sh, argc, argv, true);
}

COMMAND(wc, NULL, "text", "wc [-l] [-w] [-c] [file...]", "count lines, words and bytes")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "lwc", &flags);
    if (first < 0)
        return 2;
    if (!flags)
        flags = OPT('l') | OPT('w') | OPT('c');
    unsigned long lines = 0, words = 0, bytes = 0;
    int rc = 0;
    for (int i = first; i < argc || i == first; i++) {
        struct stream *s = open_input(sh, "wc", i < argc ? argv[i] : NULL);
        if (!s) {
            rc = 1;
            continue;
        }
        bool in_word = false;
        int ch;
        while ((ch = stream_getc(s)) >= 0) {
            bytes++;
            if (ch == '\n')
                lines++;
            if (is_space(ch))
                in_word = false;
            else if (!in_word)
                in_word = true, words++;
        }
        close_input(sh, s);
    }
    if (flags & OPT('l'))
        sh_printf(sh, "%8lu", lines);
    if (flags & OPT('w'))
        sh_printf(sh, "%8lu", words);
    if (flags & OPT('c'))
        sh_printf(sh, "%8lu", bytes);
    sh_puts(sh, "\n");
    return rc;
}

static const char *find_match(const char *line, const char *pattern, bool icase, size_t *len)
{
    size_t n = strlen(pattern);
    *len = n;
    for (const char *p = line; *p; p++)
        if ((icase ? str_ncasecmp(p, pattern, n) : strncmp(p, pattern, n)) == 0)
            return p;
    return NULL;
}

COMMAND(grep, NULL, "text", "grep [-i] [-n] [-v] [-c] pattern [file...]", "search text for a pattern")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "invc", &flags);
    if (first < 0 || first >= argc)
        return command_usage(sh, command_find("grep"));
    const char *pattern = argv[first++];
    bool icase = flags & OPT('i'), invert = flags & OPT('v');
    bool wild = has_wildcard(pattern);
    bool multi = argc - first > 1;
    char line[LINE_CAP];
    unsigned long matches = 0;
    for (int i = first; i < argc || i == first; i++) {
        const char *name = i < argc ? argv[i] : NULL;
        struct stream *s = open_input(sh, "grep", name);
        if (!s)
            continue;
        long lineno = 0;
        while (stream_getline(s, line, sizeof(line)) >= 0) {
            lineno++;
            size_t mlen = 0;
            const char *hit = wild ? NULL : find_match(line, pattern, icase, &mlen);
            bool matched = wild ? wildcard_match(pattern, line, icase) : hit != NULL;
            if (matched == invert)
                continue;
            matches++;
            if (flags & OPT('c'))
                continue;
            if (multi) {
                sh_role(sh, ROLE_ACCENT);
                sh_printf(sh, "%s:", name);
                sh_reset(sh);
            }
            if (flags & OPT('n')) {
                sh_role(sh, ROLE_OK);
                sh_printf(sh, "%ld:", lineno);
                sh_reset(sh);
            }
            const char *p = line;
            while (hit && !invert) {
                stream_write(sh->out, p, (size_t)(hit - p));
                sh_role(sh, ROLE_ERROR);
                stream_write(sh->out, hit, mlen);
                sh_reset(sh);
                p = hit + mlen;
                hit = mlen ? find_match(p, pattern, icase, &mlen) : NULL;
            }
            sh_printf(sh, "%s\n", p);
        }
        close_input(sh, s);
    }
    if (flags & OPT('c'))
        sh_printf(sh, "%lu\n", matches);
    return matches ? 0 : 1;
}

static bool sort_numeric;
static bool sort_reverse;
static bool sort_icase;

static int compare_lines(const char *a, const char *b)
{
    int r;
    if (sort_numeric) {
        long x = str_to_long(a, NULL, 10), y = str_to_long(b, NULL, 10);
        r = x < y ? -1 : x > y;
    } else {
        r = sort_icase ? str_casecmp(a, b) : strcmp(a, b);
    }
    return sort_reverse ? -r : r;
}

static void merge_sort(char **items, char **tmp, size_t n)
{
    if (n < 2)
        return;
    size_t mid = n / 2;
    merge_sort(items, tmp, mid);
    merge_sort(items + mid, tmp, n - mid);
    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n)
        tmp[k++] = compare_lines(items[i], items[j]) <= 0 ? items[i++] : items[j++];
    while (i < mid)
        tmp[k++] = items[i++];
    while (j < n)
        tmp[k++] = items[j++];
    memcpy(items, tmp, n * sizeof(char *));
}

COMMAND(sort, NULL, "text", "sort [-r] [-n] [-u] [-f] [file...]", "sort lines")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "rnuf", &flags);
    if (first < 0)
        return 2;
    struct lines l = {0};
    int rc = read_lines(sh, "sort", argc, argv, first, &l);
    sort_numeric = flags & OPT('n');
    sort_reverse = flags & OPT('r');
    sort_icase = flags & OPT('f');
    char **tmp = kmalloc(MAX(l.count, (size_t)1) * sizeof(char *));
    if (tmp)
        merge_sort(l.items, tmp, l.count);
    kfree(tmp);
    for (size_t i = 0; i < l.count; i++) {
        if ((flags & OPT('u')) && i > 0 && strcmp(l.items[i], l.items[i - 1]) == 0)
            continue;
        sh_printf(sh, "%s\n", l.items[i]);
    }
    lines_free(&l);
    return rc;
}

COMMAND(uniq, NULL, "text", "uniq [-c] [file]", "collapse repeated lines")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "c", &flags);
    if (first < 0)
        return 2;
    struct lines l = {0};
    int rc = read_lines(sh, "uniq", argc, argv, first, &l);
    for (size_t i = 0; i < l.count;) {
        size_t j = i + 1;
        while (j < l.count && strcmp(l.items[j], l.items[i]) == 0)
            j++;
        if (flags & OPT('c'))
            sh_printf(sh, "%7u %s\n", (unsigned)(j - i), l.items[i]);
        else
            sh_printf(sh, "%s\n", l.items[i]);
        i = j;
    }
    lines_free(&l);
    return rc;
}

COMMAND(rev, NULL, "text", "rev [file...]", "reverse each line")
{
    struct lines l = {0};
    int rc = read_lines(sh, "rev", argc, argv, 1, &l);
    for (size_t i = 0; i < l.count; i++) {
        size_t n = strlen(l.items[i]);
        while (n--)
            stream_putc(sh->out, l.items[i][n]);
        stream_putc(sh->out, '\n');
    }
    lines_free(&l);
    return rc;
}

COMMAND(tee, NULL, "text", "tee [-a] file", "copy input to a file and to output")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "a", &flags);
    if (first < 0 || first >= argc)
        return command_usage(sh, command_find("tee"));
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[first], path, sizeof(path));
    unsigned mode = VFS_WRITE | VFS_CREATE | ((flags & OPT('a')) ? VFS_APPEND : VFS_TRUNC);
    struct stream *f = err ? NULL : stream_file(path, mode, &err);
    if (!f)
        return sh_fs_error(sh, "tee", argv[first], err);
    char buf[512];
    long n;
    while ((n = stream_read(sh->in, buf, sizeof(buf))) > 0) {
        stream_write(f, buf, (size_t)n);
        stream_write(sh->out, buf, (size_t)n);
    }
    stream_close(f);
    return 0;
}

COMMAND(seq, NULL, "text", "seq [first [step]] last", "print a sequence of numbers")
{
    long first = 1, step = 1, last;
    bool ok = argc >= 2 && argc <= 4 && str_parse_long(argv[argc - 1], &last);
    if (ok && argc >= 3)
        ok = str_parse_long(argv[1], &first);
    if (ok && argc == 4)
        ok = str_parse_long(argv[2], &step) && step != 0;
    if (!ok)
        return command_usage(sh, command_find("seq"));
    for (long v = first; step > 0 ? v <= last : v >= last; v += step) {
        sh_printf(sh, "%ld\n", v);
        if (sh_interrupted())
            return 130;
    }
    return 0;
}

COMMAND(yes, NULL, "text", "yes [text]", "repeat text until interrupted (Ctrl+C)")
{
    const char *text = argc > 1 ? argv[1] : "y";
    for (unsigned long i = 0; !sh_interrupted() && (stream_is_console(sh->out) || i < 100000); i++)
        sh_printf(sh, "%s\n", text);
    return 0;
}

COMMAND(basename, NULL, "text", "basename path [suffix]", "strip directory (and suffix) from a path")
{
    if (argc < 2)
        return command_usage(sh, command_find("basename"));
    char name[NAME_MAX_LEN];
    str_copy(name, path_basename(argv[1]), sizeof(name));
    if (argc > 2 && str_ends(name, argv[2]) && strcmp(name, argv[2]) != 0)
        name[strlen(name) - strlen(argv[2])] = 0;
    sh_printf(sh, "%s\n", name);
    return 0;
}

COMMAND(dirname, NULL, "text", "dirname path", "strip the last component of a path")
{
    if (argc < 2)
        return command_usage(sh, command_find("dirname"));
    char dir[PATH_MAX_LEN];
    path_dirname(argv[1], dir, sizeof(dir));
    sh_printf(sh, "%s\n", dir);
    return 0;
}

static int change_case(struct shell *sh, bool upper)
{
    int ch;
    while ((ch = stream_getc(sh->in)) >= 0)
        stream_putc(sh->out, (char)(upper ? to_upper(ch) : to_lower(ch)));
    return 0;
}

COMMAND(upper, NULL, "text", "upper", "convert input to upper case")
{
    return change_case(sh, true);
}

COMMAND(lower, NULL, "text", "lower", "convert input to lower case")
{
    return change_case(sh, false);
}

COMMAND(write, NULL, "text", "write file text...", "write text into a file")
{
    if (argc < 2)
        return command_usage(sh, command_find("write"));
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[1], path, sizeof(path));
    struct stream *f = err ? NULL : stream_file(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &err);
    if (!f)
        return sh_fs_error(sh, "write", argv[1], err);
    for (int i = 2; i < argc; i++) {
        if (i > 2)
            stream_putc(f, ' ');
        stream_puts(f, argv[i]);
    }
    if (argc > 2)
        stream_putc(f, '\n');
    stream_close(f);
    return 0;
}
