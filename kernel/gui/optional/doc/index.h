/* The search index: the words of every file in /home (text files, and documents through the readers above), kept
   in /home/.index as one line per file, "path<TAB>size<TAB>mtime<TAB> word word … ". Only files whose size or
   time changed are read again. Spotlight and mdfind ask it for files that hold every word of a query. */

#define INDEX_FILE "/home/.index"
#define INDEX_MAX_FILES 4000
#define INDEX_WORDS_CAP 32768
#define INDEX_TEXT_MAX (4u << 20)
#define INDEX_EVERY_MS 10000

struct ix_entry {
    char *path, *words; /* words: " a b c " */
    uint32_t size, mtime;
};

static struct ix_entry *ix;
static int ix_count;
static bool ix_loaded;
static uint64_t ix_checked;

static bool ix_word_char(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

/* the distinct words of a text, lower case, as " a b c " */
static char *ix_words(const char *text, size_t len)
{
    char *out = kmalloc(INDEX_WORDS_CAP);
    uint32_t *seen = kzalloc(4096 * sizeof(uint32_t)); /* offset + 1 of each word in out, by hash */
    if (!out || !seen) {
        kfree(out);
        kfree(seen);
        return NULL;
    }
    size_t o = 0;
    out[o++] = ' ';
    char w[41];
    for (size_t i = 0; i < len;) {
        int n = 0;
        while (i < len && ix_word_char((uint8_t)to_lower((uint8_t)text[i]))) {
            if (n < 40)
                w[n++] = (char)to_lower((uint8_t)text[i]);
            i++;
        }
        if (n < 2) {
            i++;
            continue;
        }
        w[n] = 0;
        uint32_t h = 2166136261u;
        for (int k = 0; k < n; k++)
            h = (h ^ (uint8_t)w[k]) * 16777619u;
        for (uint32_t s = h & 4095;; s = (s + 1) & 4095) {
            if (!seen[s]) {
                if (o + (size_t)n + 2 >= INDEX_WORDS_CAP)
                    goto full;
                seen[s] = (uint32_t)o + 1;
                memcpy(out + o, w, (size_t)n);
                o += (size_t)n;
                out[o++] = ' ';
                break;
            }
            const char *at = out + seen[s] - 1;
            if (!memcmp(at, w, (size_t)n) && at[n] == ' ')
                break;
        }
    }
full:
    out[o] = 0;
    kfree(seen);
    return out;
}

static bool ix_is_document(const char *name)
{
    const char *ext = path_ext(name);
    char want[16];
    if (!ext || strlen(ext) > 10)
        return false;
    ksnprintf(want, sizeof(want), "%s;", ext);
    str_lower(want);
    return strstr(EXTS ";", want) != NULL;
}

static char *ix_file_words(const char *path, uint32_t size)
{
    if (size > INDEX_TEXT_MAX)
        return NULL;
    if (ix_is_document(path)) {
        struct doc d;
        if (doc_load(&d, path) != 0)
            return NULL;
        size_t len;
        char *t = doc_plain(&d, &len);
        doc_free(&d);
        char *w = t ? ix_words(t, len) : NULL;
        kfree(t);
        return w;
    }
    char *data;
    size_t len;
    if (vfs_read_all(path, &data, &len) != 0)
        return NULL;
    for (size_t i = 0; i < MIN(len, 2048u); i++)
        if (!data[i]) { /* not text */
            kfree(data);
            return NULL;
        }
    char *w = ix_words(data, len);
    kfree(data);
    return w;
}

static void ix_clear(struct ix_entry *e, int n)
{
    for (int i = 0; i < n; i++) {
        kfree(e[i].path);
        kfree(e[i].words);
    }
    kfree(e);
}

static void ix_load(void)
{
    ix_loaded = true;
    char *data;
    size_t len;
    if (vfs_read_all(INDEX_FILE, &data, &len) != 0)
        return;
    ix = kzalloc(INDEX_MAX_FILES * sizeof(*ix));
    for (char *p = data, *end = data + len; ix && p < end && ix_count < INDEX_MAX_FILES;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            break;
        *nl = 0;
        char *f[4] = {p, NULL, NULL, NULL};
        for (int k = 1; k < 4 && f[k - 1]; k++) {
            f[k] = strchr(f[k - 1], '\t');
            if (f[k])
                *f[k]++ = 0;
        }
        if (f[3]) {
            struct ix_entry *e = &ix[ix_count++];
            e->path = kstrdup(f[0]);
            e->size = (uint32_t)str_to_long(f[1], NULL, 10);
            e->mtime = (uint32_t)str_to_long(f[2], NULL, 10);
            e->words = kstrdup(f[3]);
        }
        p = nl + 1;
    }
    kfree(data);
}

static int ix_save(void)
{
    size_t cap = 64;
    for (int i = 0; i < ix_count; i++)
        cap += strlen(ix[i].path) + strlen(ix[i].words) + 32;
    char *out = kmalloc(cap);
    if (!out)
        return E_NOMEM;
    size_t o = 0;
    for (int i = 0; i < ix_count; i++)
        o += (size_t)ksnprintf(out + o, cap - o, "%s\t%u\t%u\t%s\n", ix[i].path, ix[i].size, ix[i].mtime, ix[i].words);
    int err = vfs_write_all(INDEX_FILE, out, o);
    kfree(out);
    return err;
}

struct ix_walk {
    struct ix_entry *old, *now;
    int old_count, count, hint, changed;
};

static void ix_walk_dir(struct ix_walk *w, const char *dir, int depth)
{
    struct vdir d;
    if (depth > 8 || vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0 && w->count < INDEX_MAX_FILES) {
        if (ent.name[0] == '.')
            continue;
        char path[PATH_MAX_LEN];
        path_join(dir, ent.name, path, sizeof(path));
        if (ent.type == VN_DIR) {
            ix_walk_dir(w, path, depth + 1);
            continue;
        }
        struct vstat st;
        if (vfs_stat(path, &st) != 0 || strchr(path, '\t'))
            continue;
        struct ix_entry *old = NULL;
        for (int k = 0; k < w->old_count && !old; k++) {
            int j = (w->hint + k) % w->old_count;
            if (w->old[j].path && !strcmp(w->old[j].path, path)) {
                old = &w->old[j];
                w->hint = j + 1;
            }
        }
        struct ix_entry *e = &w->now[w->count];
        if (old && old->size == st.size && old->mtime == st.mtime) {
            *e = *old;
            old->path = old->words = NULL;
        } else {
            char *words = ix_file_words(path, st.size);
            if (!words)
                continue;
            *e = (struct ix_entry){kstrdup(path), words, st.size, st.mtime};
            w->changed++;
        }
        w->count++;
    }
    vfs_closedir(&d);
}

/* returns how many files were read again; *removed: how many left the index */
static int ix_update(bool rebuild, int *removed)
{
    if (!ix_loaded)
        ix_load();
    struct ix_walk w = {rebuild ? NULL : ix, kzalloc(INDEX_MAX_FILES * sizeof(struct ix_entry)), rebuild ? 0 : ix_count,
                        0, 0, 0};
    if (!w.now)
        return 0;
    ix_walk_dir(&w, "/home", 0);
    int gone = 0;
    for (int i = 0; i < w.old_count; i++)
        gone += w.old[i].path != NULL;
    ix_clear(ix, ix_count);
    ix = w.now;
    ix_count = w.count;
    if (w.changed || gone || rebuild)
        ix_save();
    ix_checked = hal_ticks_ms();
    if (removed)
        *removed = gone;
    return w.changed;
}

static int ix_search(const char *query, void (*hit)(const char *path, void *ctx), void *ctx)
{
    if (!ix_loaded || !ix_checked || hal_ticks_ms() - ix_checked > INDEX_EVERY_MS)
        ix_update(false, NULL);
    char words[8][42];
    int nw = 0;
    for (const char *p = query; *p && nw < 8;) {
        int n = 0;
        words[nw][n++] = ' ';
        while (*p && ix_word_char((uint8_t)to_lower((uint8_t)*p))) {
            if (n < 41)
                words[nw][n++] = (char)to_lower((uint8_t)*p);
            p++;
        }
        words[nw][n] = 0;
        if (n > 2)
            nw++;
        if (*p)
            p++;
    }
    int found = 0;
    for (int i = 0; nw && i < ix_count; i++) {
        bool all = true;
        for (int k = 0; k < nw && all; k++)
            all = strstr(ix[i].words, words[k]) != NULL;
        if (all) {
            hit(ix[i].path, ctx);
            found++;
        }
    }
    return found;
}

static void print_hit(const char *path, void *ctx)
{
    sh_printf(ctx, "%s\n", path);
}

COMMAND(mdfind, "findtext", "files", "mdfind <words…>", "list the files in /home whose text holds every word")
{
    char q[256] = "";
    for (int i = 1; i < argc; i++) {
        str_append(q, " ", sizeof(q));
        str_append(q, argv[i], sizeof(q));
    }
    if (argc < 2)
        return sh_error(sh, "mdfind", "usage: mdfind <words…>");
    ix_update(false, NULL);
    if (!ix_search(q, print_hit, sh))
        sh_printf(sh, "no file holds%s\n", q);
    return 0;
}

COMMAND(index, NULL, "files", "index [status|update|rebuild]", "the word index of /home that Spotlight searches")
{
    const char *op = argc > 1 ? argv[1] : "status";
    if (!strcmp(op, "update") || !strcmp(op, "rebuild")) {
        int removed;
        int read = ix_update(!strcmp(op, "rebuild"), &removed);
        sh_printf(sh, "indexed %d files: %d read, %d removed\n", ix_count, read, removed);
    } else if (!strcmp(op, "status")) {
        if (!ix_loaded)
            ix_load();
        size_t bytes = 0;
        for (int i = 0; i < ix_count; i++)
            bytes += strlen(ix[i].words);
        sh_printf(sh, "%d files, %u KB of words in %s\n", ix_count, (unsigned)(bytes / 1024), INDEX_FILE);
    } else {
        return sh_error(sh, "index", "usage: index [status|update|rebuild]");
    }
    return 0;
}
