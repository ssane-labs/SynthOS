#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/vmm.h>

#include "win32.h"

struct file_cache {
    struct file_cache *next;
    char *path;
    struct file *file;
    uint8_t **pages;
    uint32_t count;
    int refs;
    bool stale;
};

static struct file_cache *caches;

static bool grow(struct file_cache *c, uint32_t size)
{
    uint32_t count = (size + 4095u) / 4096u;
    if (count <= c->count)
        return true;
    uint8_t **pages = krealloc(c->pages, sizeof(*pages) * count);
    if (!pages)
        return false;
    memset(pages + c->count, 0, sizeof(*pages) * (count - c->count));
    c->pages = pages;
    c->count = count;
    return true;
}

struct file_cache *w32_fcache_get(const char *path, uint32_t size)
{
    struct file_cache *c = caches;
    while (c && (c->stale || strcmp(c->path, path)))
        c = c->next;
    if (!c) {
        c = kzalloc(sizeof(*c));
        if (!c || !(c->path = kstrdup(path)) || vfs_open(path, VFS_READ, &c->file) != 0) {
            if (c)
                kfree(c->path);
            kfree(c);
            return NULL;
        }
        c->next = caches;
        caches = c;
    }
    c->refs++;
    if (!grow(c, size)) {
        w32_fcache_put(c);
        return NULL;
    }
    return c;
}

void w32_fcache_put(struct file_cache *c)
{
    if (!c || --c->refs > 0)
        return;
    struct file_cache **p = &caches;
    while (*p && *p != c)
        p = &(*p)->next;
    if (*p)
        *p = c->next;
    for (uint32_t i = 0; i < c->count; i++)
        kfree_pages(c->pages[i]);
    kfree(c->pages);
    vfs_close(c->file);
    kfree(c->path);
    kfree(c);
}

void *w32_fcache_page(struct file_cache *c, uint32_t index)
{
    if (index >= c->count)
        return NULL;
    if (c->pages[index])
        return c->pages[index];
    uint8_t *page = kmalloc_pages_uninit(4096);
    if (!page)
        return NULL;
    vfs_seek(c->file, (long)index * 4096, VFS_SEEK_SET);
    long got = vfs_read(c->file, page, 4096);
    if (got < 0)
        got = 0;
    memset(page + got, 0, 4096u - (uint32_t)got);
    if (c->pages[index]) {
        kfree_pages(page);
        return c->pages[index];
    }
    c->pages[index] = page;
    return page;
}

void w32_fcache_write(const char *path, uint32_t offset, const void *data, uint32_t len)
{
    for (struct file_cache *c = caches; c; c = c->next) {
        if (c->stale || strcmp(c->path, path))
            continue;
        for (uint32_t done = 0; done < len;) {
            uint32_t at = offset + done, index = at / 4096u, in = at % 4096u, n = MIN(len - done, 4096u - in);
            if (index < c->count && c->pages[index])
                memcpy(c->pages[index] + in, (const uint8_t *)data + done, n);
            done += n;
        }
    }
}

void w32_fcache_truncate(const char *path, uint32_t size)
{
    for (struct file_cache *c = caches; c; c = c->next) {
        if (c->stale || strcmp(c->path, path))
            continue;
        for (uint32_t i = size / 4096u; i < c->count; i++)
            if (c->pages[i]) {
                uint32_t from = i == size / 4096u ? size % 4096u : 0;
                memset(c->pages[i] + from, 0, 4096u - from);
            }
    }
}

void w32_fcache_forget(const char *path)
{
    w32_image_detach(path);
    for (struct file_cache *c = caches; c; c = c->next)
        if (!strcmp(c->path, path))
            c->stale = true;
}
