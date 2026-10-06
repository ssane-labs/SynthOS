#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>

#define HEAP_ALIGN 16
#define HEAP_MAGIC 0x5e17u

struct block {
    size_t size;
    struct block *prev_phys;
    struct block *next_free;
    struct block *prev_free;
    uint32_t magic;
    uint32_t used;
};

#define HEADER_SIZE ALIGN_UP(sizeof(struct block), HEAP_ALIGN)
#define MIN_SPLIT (HEADER_SIZE + 32)

static uint8_t *heap_start;
static uint8_t *heap_end;
static struct block *free_list;
static size_t used_bytes;
static size_t peak_bytes;

static struct block *next_phys(struct block *b)
{
    uint8_t *n = (uint8_t *)b + HEADER_SIZE + b->size;
    return n < heap_end ? (struct block *)n : NULL;
}

static void free_insert(struct block *b)
{
    b->prev_free = NULL;
    b->next_free = free_list;
    if (free_list)
        free_list->prev_free = b;
    free_list = b;
}

static void free_remove(struct block *b)
{
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
        free_list = b->next_free;
    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
}

void heap_init(void *base, size_t size)
{
    uintptr_t start = ALIGN_UP((uintptr_t)base, HEAP_ALIGN);
    uintptr_t end = ((uintptr_t)base + size) & ~(uintptr_t)(HEAP_ALIGN - 1);
    heap_start = (uint8_t *)start;
    heap_end = (uint8_t *)end;
    struct block *b = (struct block *)heap_start;
    b->size = (size_t)(end - start) - HEADER_SIZE;
    b->prev_phys = NULL;
    b->magic = HEAP_MAGIC;
    b->used = 0;
    free_list = NULL;
    free_insert(b);
}

static void split(struct block *b, size_t size)
{
    if (b->size < size + MIN_SPLIT)
        return;
    struct block *rest = (struct block *)((uint8_t *)b + HEADER_SIZE + size);
    rest->size = b->size - size - HEADER_SIZE;
    rest->prev_phys = b;
    rest->magic = HEAP_MAGIC;
    rest->used = 0;
    b->size = size;
    struct block *after = next_phys(rest);
    if (after)
        after->prev_phys = rest;
    free_insert(rest);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        size = 1;
    size = ALIGN_UP(size, HEAP_ALIGN);
    struct block *best = NULL;
    for (struct block *b = free_list; b; b = b->next_free) {
        if (b->size >= size && (!best || b->size < best->size)) {
            best = b;
            if (b->size - size < MIN_SPLIT)
                break;
        }
    }
    if (!best)
        return NULL;
    free_remove(best);
    split(best, size);
    best->used = 1;
    used_bytes += best->size + HEADER_SIZE;
    if (used_bytes > peak_bytes)
        peak_bytes = used_bytes;
    return (uint8_t *)best + HEADER_SIZE;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

static struct block *header_of(void *ptr)
{
    struct block *b = (struct block *)((uint8_t *)ptr - HEADER_SIZE);
    if ((uint8_t *)b < heap_start || (uint8_t *)b >= heap_end || b->magic != HEAP_MAGIC || !b->used)
        panic("heap: invalid pointer %p", ptr);
    return b;
}

static struct block *merge(struct block *a, struct block *b)
{
    a->size += HEADER_SIZE + b->size;
    b->magic = 0;
    struct block *after = next_phys(a);
    if (after)
        after->prev_phys = a;
    return a;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct block *b = header_of(ptr);
    used_bytes -= b->size + HEADER_SIZE;
    b->used = 0;
    struct block *n = next_phys(b);
    if (n && !n->used) {
        free_remove(n);
        merge(b, n);
    }
    struct block *p = b->prev_phys;
    if (p && !p->used) {
        free_remove(p);
        b = merge(p, b);
    }
    free_insert(b);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct block *b = header_of(ptr);
    size_t want = ALIGN_UP(size, HEAP_ALIGN);
    if (b->size >= want)
        return ptr;
    struct block *n = next_phys(b);
    if (n && !n->used && b->size + HEADER_SIZE + n->size >= want) {
        used_bytes -= b->size;
        free_remove(n);
        merge(b, n);
        split(b, want);
        used_bytes += b->size;
        if (used_bytes > peak_bytes)
            peak_bytes = used_bytes;
        return ptr;
    }
    void *fresh = kmalloc(size);
    if (!fresh)
        return NULL;
    memcpy(fresh, ptr, b->size);
    kfree(ptr);
    return fresh;
}

char *kstrndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = kmalloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

char *kstrdup(const char *s)
{
    return kstrndup(s, (size_t)-1);
}

void heap_get_stats(struct heap_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->total = (size_t)(heap_end - heap_start);
    out->used = used_bytes;
    out->free = out->total - used_bytes;
    out->peak = peak_bytes;
    for (struct block *b = (struct block *)heap_start; b; b = next_phys(b)) {
        out->blocks++;
        if (!b->used && b->size > out->largest_free)
            out->largest_free = b->size;
    }
}
