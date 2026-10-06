#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/exec.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/time.h>
#include <synth/vfs.h>

struct entry {
    char name[NAME_MAX_LEN];
    uint8_t type;
    uint32_t size;
    uint32_t mtime;
};

struct listing {
    struct entry *items;
    size_t count;
    size_t cap;
};

static enum theme_role entry_role(const struct entry *e)
{
    if (e->type == VN_DIR)
        return ROLE_DIR;
    if (e->type == VN_DEV)
        return ROLE_DEVICE;
    if (exec_is_executable_name(e->name))
        return ROLE_EXEC;
    return ROLE_TEXT;
}

static int listing_load(const char *path, bool all, struct listing *out)
{
    struct vdir d;
    int err = vfs_opendir(path, &d);
    if (err)
        return err;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0) {
        if (!all && ent.name[0] == '.')
            continue;
        if (out->count == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 32;
            struct entry *items = krealloc(out->items, cap * sizeof(*items));
            if (!items)
                break;
            out->items = items;
            out->cap = cap;
        }
        struct entry *e = &out->items[out->count++];
        str_copy(e->name, ent.name, sizeof(e->name));
        e->type = ent.type;
        e->size = ent.size;
        char full[PATH_MAX_LEN];
        struct vstat st = {0};
        e->mtime = path_join(path, ent.name, full, sizeof(full)) == 0 && vfs_stat(full, &st) == 0 ? st.mtime : 0;
        if (e->type == VN_FILE)
            e->size = st.size;
    }
    vfs_closedir(&d);
    for (size_t i = 1; i < out->count; i++) {
        struct entry e = out->items[i];
        size_t j = i;
        while (j > 0 && str_casecmp(out->items[j - 1].name, e.name) > 0) {
            out->items[j] = out->items[j - 1];
            j--;
        }
        out->items[j] = e;
    }
    return 0;
}

static void print_entry_name(struct shell *sh, const struct entry *e, bool decorate)
{
    sh_role(sh, entry_role(e));
    sh_puts(sh, e->name);
    sh_reset(sh);
    if (decorate && e->type == VN_DIR)
        sh_puts(sh, "/");
}

static void list_long(struct shell *sh, struct listing *l)
{
    uint64_t total = 0;
    for (size_t i = 0; i < l->count; i++) {
        struct entry *e = &l->items[i];
        char size[16], date[24];
        if (e->type == VN_DIR)
            str_copy(size, "<DIR>", sizeof(size));
        else if (e->type == VN_DEV)
            str_copy(size, "<DEV>", sizeof(size));
        else
            fmt_size(size, sizeof(size), e->size);
        total += e->type == VN_FILE ? e->size : 0;
        time_format(e->mtime, date, sizeof(date));
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%c ", e->type == VN_DIR ? 'd' : e->type == VN_DEV ? 'c' : '-');
        sh_reset(sh);
        sh_role(sh, e->type == VN_FILE ? ROLE_ACCENT2 : ROLE_MUTED);
        sh_printf(sh, "%7s ", size);
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%s  ", date);
        sh_reset(sh);
        print_entry_name(sh, e, true);
        sh_puts(sh, "\n");
    }
    char sz[16];
    fmt_size(sz, sizeof(sz), total);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "%u entries, %s\n", (unsigned)l->count, sz);
    sh_reset(sh);
}

static void list_columns(struct shell *sh, struct listing *l)
{
    if (!stream_is_console(sh->out)) {
        for (size_t i = 0; i < l->count; i++)
            sh_printf(sh, "%s\n", l->items[i].name);
        return;
    }
    size_t width = 1;
    for (size_t i = 0; i < l->count; i++)
        width = MAX(width, strlen(l->items[i].name) + (l->items[i].type == VN_DIR ? 1 : 0));
    width += 2;
    size_t cols = MAX((size_t)1, (size_t)con_cols() / width);
    size_t rows = (l->count + cols - 1) / cols;
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t i = c * rows + r;
            if (i >= l->count)
                break;
            struct entry *e = &l->items[i];
            print_entry_name(sh, e, true);
            size_t used = strlen(e->name) + (e->type == VN_DIR ? 1 : 0);
            if ((c + 1) * rows + r < l->count)
                for (; used < width; used++)
                    sh_puts(sh, " ");
        }
        sh_puts(sh, "\n");
    }
}

COMMAND(ls, "dir", "files", "ls [-l] [-a] [-1] [path...]", "list directory contents")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "la1", &flags);
    if (first < 0)
        return 2;
    bool long_fmt = flags & OPT('l') || strcmp(argv[0], "dir") == 0;
    int rc = 0;
    int count = argc - first;
    for (int i = first; i < argc || (count == 0 && i == first); i++) {
        const char *arg = count ? argv[i] : ".";
        char path[PATH_MAX_LEN];
        struct vstat st;
        int err = sh_resolve(sh, arg, path, sizeof(path));
        if (!err)
            err = vfs_stat(path, &st);
        if (err) {
            rc = sh_fs_error(sh, "ls", arg, err);
            continue;
        }
        if (st.type != VN_DIR) {
            struct entry e = {.type = st.type, .size = st.size, .mtime = st.mtime};
            str_copy(e.name, arg, sizeof(e.name));
            struct listing one = {&e, 1, 1};
            if (long_fmt)
                list_long(sh, &one);
            else
                list_columns(sh, &one);
            continue;
        }
        if (count > 1) {
            sh_role(sh, ROLE_ACCENT);
            sh_printf(sh, "%s:\n", arg);
            sh_reset(sh);
        }
        struct listing l = {0};
        listing_load(path, flags & OPT('a'), &l);
        if (long_fmt)
            list_long(sh, &l);
        else if (flags & OPT('1'))
            for (size_t k = 0; k < l.count; k++)
                print_entry_name(sh, &l.items[k], true), sh_puts(sh, "\n");
        else
            list_columns(sh, &l);
        kfree(l.items);
    }
    return rc;
}

COMMAND(cd, "chdir", "files", "cd [dir|-]", "change the current directory")
{
    const char *target = argc > 1 ? argv[1] : sh_get(sh, "HOME");
    if (argc > 1 && strcmp(argv[1], "-") == 0)
        target = sh_get(sh, "OLDPWD");
    if (!target)
        target = "/";
    int err = sh_chdir(sh, target);
    return err ? sh_fs_error(sh, "cd", target, err) : 0;
}

COMMAND(pwd, NULL, "files", "pwd", "print the current directory")
{
    sh_printf(sh, "%s\n", sh->cwd);
    return 0;
}

COMMAND(mkdir, "md", "files", "mkdir [-p] dir...", "create directories")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "p", &flags);
    if (first < 0 || first >= argc)
        return command_usage(sh, command_find("mkdir"));
    int rc = 0;
    for (int i = first; i < argc; i++) {
        char path[PATH_MAX_LEN];
        int err = sh_resolve(sh, argv[i], path, sizeof(path));
        if (!err)
            err = (flags & OPT('p')) ? vfs_mkdirs(path) : vfs_mkdir(path);
        if (err)
            rc = sh_fs_error(sh, "mkdir", argv[i], err);
    }
    return rc;
}

COMMAND(rmdir, "rd", "files", "rmdir dir...", "remove empty directories")
{
    if (argc < 2)
        return command_usage(sh, command_find("rmdir"));
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX_LEN];
        int err = sh_resolve(sh, argv[i], path, sizeof(path));
        if (!err && !vfs_is_dir(path))
            err = E_NOTDIR;
        if (!err)
            err = vfs_unlink(path);
        if (err)
            rc = sh_fs_error(sh, "rmdir", argv[i], err);
    }
    return rc;
}

static int remove_tree(const char *path)
{
    struct vstat st;
    int err = vfs_stat(path, &st);
    if (err)
        return err;
    if (st.type == VN_DIR) {
        struct vdir d;
        struct vfs_dirent ent;
        for (;;) {
            if ((err = vfs_opendir(path, &d)) != 0)
                return err;
            int r = vfs_readdir(&d, &ent);
            vfs_closedir(&d);
            if (r != 0)
                break;
            char child[PATH_MAX_LEN];
            if ((err = path_join(path, ent.name, child, sizeof(child))) != 0)
                return err;
            if ((err = remove_tree(child)) != 0)
                return err;
        }
    }
    return vfs_unlink(path);
}

COMMAND(rm, "del,erase", "files", "rm [-r] [-f] path...", "remove files (and directories with -r)")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "rRf", &flags);
    if (first < 0 || first >= argc)
        return command_usage(sh, command_find("rm"));
    bool recursive = flags & (OPT('r') | OPT('R'));
    bool force = flags & OPT('f');
    int rc = 0;
    for (int i = first; i < argc; i++) {
        char path[PATH_MAX_LEN];
        int err = sh_resolve(sh, argv[i], path, sizeof(path));
        if (!err && strcmp(path, "/") == 0)
            err = E_PERM;
        if (!err && vfs_is_dir(path) && !recursive)
            err = E_ISDIR;
        if (!err)
            err = recursive ? remove_tree(path) : vfs_unlink(path);
        if (err && !(force && err == E_NOENT))
            rc = sh_fs_error(sh, "rm", argv[i], err);
    }
    return rc;
}

static int copy_file(const char *from, const char *to)
{
    struct file *src, *dst;
    int err = vfs_open(from, VFS_READ, &src);
    if (err)
        return err;
    err = vfs_open(to, VFS_WRITE | VFS_CREATE | VFS_TRUNC, &dst);
    if (err) {
        vfs_close(src);
        return err;
    }
    char *buf = kmalloc(4096);
    long n = buf ? 0 : E_NOMEM;
    while (buf && (n = vfs_read(src, buf, 4096)) > 0) {
        long w = vfs_write(dst, buf, (size_t)n);
        if (w != n) {
            n = w < 0 ? w : E_NOSPC;
            break;
        }
    }
    kfree(buf);
    vfs_close(src);
    vfs_close(dst);
    return n < 0 ? (int)n : 0;
}

static int copy_tree(const char *from, const char *to)
{
    if (!vfs_is_dir(from))
        return copy_file(from, to);
    int err = vfs_mkdir(to);
    if (err && err != E_EXIST)
        return err;
    struct listing l = {0};
    listing_load(from, true, &l);
    for (size_t i = 0; i < l.count && !err; i++) {
        char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
        if ((err = path_join(from, l.items[i].name, a, sizeof(a))) == 0 &&
            (err = path_join(to, l.items[i].name, b, sizeof(b))) == 0)
            err = copy_tree(a, b);
    }
    kfree(l.items);
    return err;
}

static int transfer(struct shell *sh, int argc, char **argv, bool move)
{
    const char *name = move ? "mv" : "cp";
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "rR", &flags);
    if (first < 0 || argc - first < 2)
        return command_usage(sh, command_find(name));
    char dest[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[argc - 1], dest, sizeof(dest));
    if (err)
        return sh_fs_error(sh, name, argv[argc - 1], err);
    bool dest_dir = vfs_is_dir(dest);
    if (argc - first > 2 && !dest_dir)
        return sh_fs_error(sh, name, argv[argc - 1], E_NOTDIR);
    int rc = 0;
    for (int i = first; i < argc - 1; i++) {
        char src[PATH_MAX_LEN], target[PATH_MAX_LEN];
        if ((err = sh_resolve(sh, argv[i], src, sizeof(src))) != 0 || !vfs_exists(src)) {
            rc = sh_fs_error(sh, name, argv[i], err ? err : E_NOENT);
            continue;
        }
        if (dest_dir)
            err = path_join(dest, path_basename(src), target, sizeof(target));
        else
            str_copy(target, dest, sizeof(target));
        if (!err && strcmp(src, target) == 0)
            continue;
        if (!err && !move && vfs_is_dir(src) && !(flags & (OPT('r') | OPT('R')))) {
            rc = sh_error(sh, name, "%s: is a directory (use -r)", argv[i]);
            continue;
        }
        if (!err && move) {
            err = vfs_rename(src, target);
            if (err == E_XDEV && (err = copy_tree(src, target)) == 0)
                err = remove_tree(src);
        } else if (!err) {
            err = copy_tree(src, target);
        }
        if (err)
            rc = sh_fs_error(sh, name, argv[i], err);
    }
    return rc;
}

COMMAND(cp, "copy", "files", "cp [-r] source... dest", "copy files and directories")
{
    return transfer(sh, argc, argv, false);
}

COMMAND(mv, "move,ren,rename", "files", "mv source... dest", "move or rename files")
{
    return transfer(sh, argc, argv, true);
}

COMMAND(touch, NULL, "files", "touch file...", "create empty files")
{
    if (argc < 2)
        return command_usage(sh, command_find("touch"));
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX_LEN];
        struct file *f;
        int err = sh_resolve(sh, argv[i], path, sizeof(path));
        if (!err && (err = vfs_open(path, VFS_WRITE | VFS_CREATE, &f)) == 0)
            vfs_close(f);
        if (err)
            rc = sh_fs_error(sh, "touch", argv[i], err);
    }
    return rc;
}

static int pump(struct shell *sh, struct stream *in, bool number)
{
    char buf[512];
    long n;
    if (!number) {
        while ((n = stream_read(in, buf, sizeof(buf))) > 0) {
            stream_write(sh->out, buf, (size_t)n);
            if (sh_interrupted())
                return 130;
        }
        return 0;
    }
    int line = 1;
    while (stream_getline(in, buf, sizeof(buf)) >= 0) {
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%5d  ", line++);
        sh_reset(sh);
        sh_printf(sh, "%s\n", buf);
    }
    return 0;
}

COMMAND(cat, "type", "files", "cat [-n] [file...]", "print files (or standard input)")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "n", &flags);
    if (first < 0)
        return 2;
    if (first >= argc)
        return pump(sh, sh->in, flags & OPT('n'));
    int rc = 0;
    for (int i = first; i < argc; i++) {
        char path[PATH_MAX_LEN];
        int err = sh_resolve(sh, argv[i], path, sizeof(path));
        struct stream *s = err ? NULL : stream_file(path, VFS_READ, &err);
        if (!s) {
            rc = sh_fs_error(sh, "cat", argv[i], err);
            continue;
        }
        int r = pump(sh, s, flags & OPT('n'));
        stream_close(s);
        if (r)
            return r;
    }
    return rc;
}

COMMAND(stat, NULL, "files", "stat path...", "show file details")
{
    if (argc < 2)
        return command_usage(sh, command_find("stat"));
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX_LEN], date[24];
        struct vstat st;
        int err = sh_resolve(sh, argv[i], path, sizeof(path));
        if (!err)
            err = vfs_stat(path, &st);
        if (err) {
            rc = sh_fs_error(sh, "stat", argv[i], err);
            continue;
        }
        time_format(st.mtime, date, sizeof(date));
        const char *type = st.type == VN_DIR ? "directory" : st.type == VN_DEV ? "device" : "file";
        sh_role(sh, ROLE_ACCENT);
        sh_printf(sh, "%s\n", path);
        sh_reset(sh);
        sh_printf(sh, "  type      %s\n  size      %u bytes\n  modified  %s\n  inode     %u\n", type,
                  (unsigned)st.size, date, (unsigned)st.inode);
    }
    return rc;
}

static void tree_walk(struct shell *sh, const char *path, char *prefix, size_t plen, int depth, bool all,
                      unsigned *dirs, unsigned *files)
{
    struct listing l = {0};
    if (listing_load(path, all, &l) != 0 || depth > 32)
        return;
    for (size_t i = 0; i < l.count && !sh_interrupted(); i++) {
        bool last = i + 1 == l.count;
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%s%s", prefix, last ? "\xc0\xc4\xc4 " : "\xc3\xc4\xc4 ");
        sh_reset(sh);
        print_entry_name(sh, &l.items[i], false);
        sh_puts(sh, "\n");
        if (l.items[i].type == VN_DIR) {
            (*dirs)++;
            char child[PATH_MAX_LEN];
            if (path_join(path, l.items[i].name, child, sizeof(child)) == 0 && plen + 5 < 128) {
                str_copy(prefix + plen, last ? "    " : "\xb3   ", 128 - plen);
                tree_walk(sh, child, prefix, plen + 4, depth + 1, all, dirs, files);
                prefix[plen] = 0;
            }
        } else {
            (*files)++;
        }
    }
    kfree(l.items);
}

COMMAND(tree, NULL, "files", "tree [-a] [dir]", "show a directory tree")
{
    uint64_t flags;
    int first = getopt_flags(sh, argc, argv, "a", &flags);
    if (first < 0)
        return 2;
    const char *arg = first < argc ? argv[first] : ".";
    char path[PATH_MAX_LEN], prefix[128] = "";
    int err = sh_resolve(sh, arg, path, sizeof(path));
    if (!err && !vfs_is_dir(path))
        err = E_NOTDIR;
    if (err)
        return sh_fs_error(sh, "tree", arg, err);
    unsigned dirs = 0, files = 0;
    sh_role(sh, ROLE_DIR);
    sh_printf(sh, "%s\n", arg);
    sh_reset(sh);
    tree_walk(sh, path, prefix, 0, 0, flags & OPT('a'), &dirs, &files);
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "\n%u directories, %u files\n", dirs, files);
    sh_reset(sh);
    return 0;
}

static void find_walk(struct shell *sh, const char *path, const char *shown, const char *pattern, int type,
                      int depth)
{
    struct listing l = {0};
    if (depth > 32 || listing_load(path, true, &l) != 0)
        return;
    for (size_t i = 0; i < l.count && !sh_interrupted(); i++) {
        char child[PATH_MAX_LEN], display[PATH_MAX_LEN];
        if (path_join(path, l.items[i].name, child, sizeof(child)) != 0)
            continue;
        ksnprintf(display, sizeof(display), "%s%s%s", shown, str_ends(shown, "/") ? "" : "/", l.items[i].name);
        bool type_ok = type == 0 || l.items[i].type == type;
        if (type_ok && (!pattern || wildcard_match(pattern, l.items[i].name, true))) {
            sh_role(sh, entry_role(&l.items[i]));
            sh_printf(sh, "%s\n", display);
            sh_reset(sh);
        }
        if (l.items[i].type == VN_DIR)
            find_walk(sh, child, display, pattern, type, depth + 1);
    }
    kfree(l.items);
}

COMMAND(find, NULL, "files", "find [dir] [-name pattern] [-type f|d]", "search for files by name")
{
    const char *start = ".", *pattern = NULL;
    int type = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc)
            pattern = argv[++i];
        else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc)
            type = argv[++i][0] == 'd' ? VN_DIR : VN_FILE;
        else if (argv[i][0] != '-')
            start = argv[i];
        else
            return command_usage(sh, command_find("find"));
    }
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, start, path, sizeof(path));
    if (!err && !vfs_is_dir(path))
        err = E_NOTDIR;
    if (err)
        return sh_fs_error(sh, "find", start, err);
    find_walk(sh, path, start, pattern, type, 0);
    return 0;
}

static void print_mount(const struct fs_type *fs, void *ctx)
{
    struct shell *sh = ctx;
    sh_printf(sh, "  %-8s %s\n", fs->name, fs->description);
}

COMMAND(mount, NULL, "files", "mount [fstype source dir]", "list or attach filesystems")
{
    if (argc == 1) {
        for (struct mount *m = vfs_mounts(); m; m = m->next) {
            sh_role(sh, ROLE_ACCENT2);
            sh_printf(sh, "%-10s", m->path);
            sh_reset(sh);
            sh_printf(sh, " %-8s %s\n", m->fs->name, m->source);
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-t") == 0) {
        vfs_foreach_fs(print_mount, sh);
        return 0;
    }
    if (argc != 4)
        return command_usage(sh, command_find("mount"));
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[3], path, sizeof(path));
    if (!err)
        err = vfs_mount(argv[1], argv[2], path);
    return err ? sh_fs_error(sh, "mount", argv[3], err) : 0;
}

COMMAND(umount, NULL, "files", "umount dir", "detach a filesystem")
{
    if (argc != 2)
        return command_usage(sh, command_find("umount"));
    char path[PATH_MAX_LEN];
    int err = sh_resolve(sh, argv[1], path, sizeof(path));
    if (!err)
        err = vfs_umount(path);
    return err ? sh_fs_error(sh, "umount", argv[1], err) : 0;
}

COMMAND(df, NULL, "files", "df", "show filesystem usage")
{
    sh_role(sh, ROLE_MUTED);
    sh_printf(sh, "%-10s %-8s %8s %8s\n", "MOUNT", "TYPE", "SIZE", "FREE");
    sh_reset(sh);
    for (struct mount *m = vfs_mounts(); m; m = m->next) {
        uint64_t total = 0, free = 0;
        char a[16] = "-", b[16] = "-";
        if (m->fs->statfs && m->fs->statfs(m->root, &total, &free) == 0) {
            fmt_size(a, sizeof(a), total);
            fmt_size(b, sizeof(b), free);
        }
        sh_printf(sh, "%-10s %-8s %8s %8s\n", m->path, m->fs->name, a, b);
    }
    return 0;
}

COMMAND(hexdump, "hd,xxd", "files", "hexdump [file]", "show bytes in hex and text")
{
    struct stream *s = sh->in;
    int err = 0;
    if (argc > 1) {
        char path[PATH_MAX_LEN];
        err = sh_resolve(sh, argv[1], path, sizeof(path));
        s = err ? NULL : stream_file(path, VFS_READ, &err);
        if (!s)
            return sh_fs_error(sh, "hexdump", argv[1], err);
    }
    uint8_t row[16];
    uint32_t off = 0;
    long n;
    while ((n = stream_read(s, row, sizeof(row))) > 0 && !sh_interrupted()) {
        while (n < 16) {
            long more = stream_read(s, row + n, (size_t)(16 - n));
            if (more <= 0)
                break;
            n += more;
        }
        sh_role(sh, ROLE_MUTED);
        sh_printf(sh, "%08x  ", off);
        sh_reset(sh);
        for (int i = 0; i < 16; i++) {
            if (i < n) {
                sh_role(sh, row[i] == 0 ? ROLE_MUTED : ROLE_TEXT);
                sh_printf(sh, "%02x ", row[i]);
            } else {
                sh_puts(sh, "   ");
            }
            if (i == 7)
                sh_puts(sh, " ");
        }
        sh_role(sh, ROLE_ACCENT2);
        sh_puts(sh, " ");
        for (int i = 0; i < n; i++)
            stream_putc(sh->out, row[i] >= 0x20 && row[i] < 0x7f ? (char)row[i] : '.');
        sh_reset(sh);
        sh_puts(sh, "\n");
        off += (uint32_t)n;
    }
    if (s != sh->in)
        stream_close(s);
    return 0;
}
