#include <synth/config.h>
#include <synth/heap.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/vfs.h>

static struct strmap_entry *find(const struct strmap *m, const char *key)
{
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->items[i].key, key) == 0)
            return &m->items[i];
    return NULL;
}

void strmap_set(struct strmap *m, const char *key, const char *value)
{
    struct strmap_entry *e = find(m, key);
    char *v = kstrdup(value ? value : "");
    if (!v)
        return;
    if (e) {
        kfree(e->value);
        e->value = v;
        return;
    }
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 16;
        struct strmap_entry *items = krealloc(m->items, cap * sizeof(*items));
        if (!items) {
            kfree(v);
            return;
        }
        m->items = items;
        m->cap = cap;
    }
    m->items[m->count].key = kstrdup(key);
    m->items[m->count].value = v;
    m->count++;
}

const char *strmap_get(const struct strmap *m, const char *key)
{
    struct strmap_entry *e = find(m, key);
    return e ? e->value : NULL;
}

bool strmap_remove(struct strmap *m, const char *key)
{
    struct strmap_entry *e = find(m, key);
    if (!e)
        return false;
    kfree(e->key);
    kfree(e->value);
    *e = m->items[--m->count];
    return true;
}

void strmap_sort(struct strmap *m)
{
    for (size_t i = 1; i < m->count; i++) {
        struct strmap_entry e = m->items[i];
        size_t j = i;
        while (j > 0 && strcmp(m->items[j - 1].key, e.key) > 0) {
            m->items[j] = m->items[j - 1];
            j--;
        }
        m->items[j] = e;
    }
}

void history_add(struct history *h, const char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;
    if (h->count && strcmp(h->items[h->count - 1], line) == 0)
        return;
    if (!h->items) {
        h->max = CONFIG_HISTORY;
        h->items = kzalloc(h->max * sizeof(char *));
        if (!h->items)
            return;
    }
    if (h->count == h->max) {
        kfree(h->items[0]);
        memmove(h->items, h->items + 1, (h->max - 1) * sizeof(char *));
        h->count--;
    }
    h->items[h->count++] = kstrdup(line);
}

const char *history_get(const struct history *h, size_t index)
{
    return index < h->count ? h->items[index] : NULL;
}

int history_load(struct history *h, const char *path)
{
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    char *line = data;
    for (char *p = data;; p++) {
        if (*p == '\n' || *p == 0) {
            bool end = *p == 0;
            *p = 0;
            history_add(h, line);
            if (end)
                break;
            line = p + 1;
        }
    }
    kfree(data);
    return 0;
}

int history_save(const struct history *h, const char *path)
{
    struct stream *s = stream_file(path, VFS_WRITE | VFS_CREATE | VFS_TRUNC, NULL);
    if (!s)
        return -1;
    for (size_t i = 0; i < h->count; i++) {
        stream_puts(s, h->items[i]);
        stream_putc(s, '\n');
    }
    stream_close(s);
    return 0;
}
