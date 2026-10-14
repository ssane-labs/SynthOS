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

static struct heap kernel_heap;

static struct block *next_phys(struct heap *h, struct block *b)
{
    uint8_t *n = (uint8_t *)b + HEADER_SIZE + b->size;
    return n < h->end ? (struct block *)n : NULL;
}

static int bin_of(size_t size)
{
    int bin = 0;
    for (size >>= 5; size && bin < HEAP_BINS - 1; size >>= 1)
        bin++;
    return bin;
}

static void free_insert(struct heap *h, struct block *b)
{
    struct block **head = (struct block **)&h->bins[bin_of(b->size)];
    b->prev_free = NULL;
    b->next_free = *head;
    if (*head)
        (*head)->prev_free = b;
    *head = b;
}

static void free_remove(struct heap *h, struct block *b)
{
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
        h->bins[bin_of(b->size)] = b->next_free;
    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
}

void heap_create(struct heap *h, void *base, size_t size, bool strict)
{
    memset(h, 0, sizeof(*h));
    uintptr_t start = ALIGN_UP((uintptr_t)base, HEAP_ALIGN);
    uintptr_t end = ((uintptr_t)base + size) & ~(uintptr_t)(HEAP_ALIGN - 1);
    h->start = (uint8_t *)start;
    h->end = (uint8_t *)end;
    h->strict = strict;
    struct block *b = (struct block *)h->start;
    b->size = (size_t)(end - start) - HEADER_SIZE;
    b->prev_phys = NULL;
    b->magic = HEAP_MAGIC;
    b->used = 0;
    free_insert(h, b);
}

void heap_init(void *base, size_t size)
{
    heap_create(&kernel_heap, base, size, true);
}

static void split(struct heap *h, struct block *b, size_t size)
{
    if (b->size < size + MIN_SPLIT)
        return;
    struct block *rest = (struct block *)((uint8_t *)b + HEADER_SIZE + size);
    rest->size = b->size - size - HEADER_SIZE;
    rest->prev_phys = b;
    rest->magic = HEAP_MAGIC;
    rest->used = 0;
    b->size = size;
    struct block *after = next_phys(h, rest);
    if (after)
        after->prev_phys = rest;
    free_insert(h, rest);
}

void *heap_alloc(struct heap *h, size_t size)
{
    if (size == 0)
        size = 1;
    if (size > (size_t)(h->end - h->start))
        return NULL;
    size = ALIGN_UP(size, HEAP_ALIGN);
    struct block *best = NULL;
    int bin = bin_of(size);
    for (struct block *b = h->bins[bin]; b && !best; b = b->next_free)
        if (b->size >= size)
            best = b;
    while (!best && ++bin < HEAP_BINS)
        best = h->bins[bin];
    if (!best)
        return NULL;
    free_remove(h, best);
    split(h, best, size);
    best->used = 1;
    h->used += best->size + HEADER_SIZE;
    if (h->used > h->peak)
        h->peak = h->used;
    return (uint8_t *)best + HEADER_SIZE;
}

static struct block *header_of(struct heap *h, void *ptr)
{
    struct block *b = (struct block *)((uint8_t *)ptr - HEADER_SIZE);
    if ((uint8_t *)b < h->start || (uint8_t *)b >= h->end || b->magic != HEAP_MAGIC || !b->used) {
        if (h->strict)
            panic("heap: invalid pointer %p", ptr);
        return NULL;
    }
    return b;
}

static struct block *merge(struct heap *h, struct block *a, struct block *b)
{
    a->size += HEADER_SIZE + b->size;
    b->magic = 0;
    struct block *after = next_phys(h, a);
    if (after)
        after->prev_phys = a;
    return a;
}

void heap_free(struct heap *h, void *ptr)
{
    if (!ptr)
        return;
    struct block *b = header_of(h, ptr);
    if (!b)
        return;
    h->used -= b->size + HEADER_SIZE;
    b->used = 0;
    struct block *n = next_phys(h, b);
    if (n && !n->used) {
        free_remove(h, n);
        merge(h, b, n);
    }
    struct block *p = b->prev_phys;
    if (p && !p->used) {
        free_remove(h, p);
        b = merge(h, p, b);
    }
    free_insert(h, b);
}

size_t heap_size_of(struct heap *h, void *ptr)
{
    struct block *b = ptr ? header_of(h, ptr) : NULL;
    return b ? b->size : 0;
}

void *heap_realloc(struct heap *h, void *ptr, size_t size)
{
    if (!ptr)
        return heap_alloc(h, size);
    if (size == 0) {
        heap_free(h, ptr);
        return NULL;
    }
    struct block *b = header_of(h, ptr);
    if (!b)
        return NULL;
    size_t want = ALIGN_UP(size, HEAP_ALIGN);
    if (b->size >= want)
        return ptr;
    struct block *n = next_phys(h, b);
    if (n && !n->used && b->size + HEADER_SIZE + n->size >= want) {
        h->used -= b->size;
        free_remove(h, n);
        merge(h, b, n);
        split(h, b, want);
        h->used += b->size;
        if (h->used > h->peak)
            h->peak = h->used;
        return ptr;
    }
    void *fresh = heap_alloc(h, size);
    if (!fresh)
        return NULL;
    memcpy(fresh, ptr, b->size);
    heap_free(h, ptr);
    return fresh;
}

static void *alloc_aligned(struct heap *h, size_t size, size_t align, bool zero)
{
    uint8_t *raw = heap_alloc(h, size + align + sizeof(void *));
    if (!raw)
        return NULL;
    uint8_t *aligned = (uint8_t *)ALIGN_UP((uintptr_t)raw + sizeof(void *), (uintptr_t)align);
    ((void **)aligned)[-1] = raw;
    if (zero)
        memset(aligned, 0, size);
    return aligned;
}

void *heap_alloc_aligned(struct heap *h, size_t size, size_t align)
{
    return alloc_aligned(h, size, align, true);
}

void heap_free_aligned(struct heap *h, void *ptr)
{
    if (ptr)
        heap_free(h, ((void **)ptr)[-1]);
}

void heap_stats_of(struct heap *h, struct heap_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->total = (size_t)(h->end - h->start);
    out->used = h->used;
    out->free = out->total - h->used;
    out->peak = h->peak;
    for (struct block *b = (struct block *)h->start; b; b = next_phys(h, b)) {
        out->blocks++;
        if (!b->used && b->size > out->largest_free)
            out->largest_free = b->size;
    }
}

void *kmalloc(size_t size)
{
    return heap_alloc(&kernel_heap, size);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    heap_free(&kernel_heap, ptr);
}

void *krealloc(void *ptr, size_t size)
{
    return heap_realloc(&kernel_heap, ptr, size);
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

size_t heap_free_bytes(void)
{
    return (size_t)(kernel_heap.end - kernel_heap.start) - kernel_heap.used;
}

uintptr_t heap_check(void)
{
    struct heap *h = &kernel_heap;
    struct block *prev = NULL;
    for (struct block *b = (struct block *)h->start; b;) {
        if (b->magic != HEAP_MAGIC || b->prev_phys != prev || (uint8_t *)b + HEADER_SIZE + b->size > h->end)
            return (uintptr_t)b;
        prev = b;
        b = next_phys(h, b);
    }
    return 0;
}

void heap_get_stats(struct heap_stats *out)
{
    heap_stats_of(&kernel_heap, out);
}

#define POOL_CHUNK_PAGES 256u
#define POOL_MAX_CHUNKS 2048

struct page_chunk {
    uint8_t *base;
    void *free_list;
    uint32_t free_count;
    struct page_chunk *next_partial;
};

static struct page_chunk *pool_chunks[POOL_MAX_CHUNKS];
static int pool_count;
static struct page_chunk *pool_partial;

static int pool_find(const void *ptr)
{
    int lo = 0, hi = pool_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *b = pool_chunks[mid]->base;
        if ((const uint8_t *)ptr < b)
            hi = mid - 1;
        else if ((const uint8_t *)ptr >= b + POOL_CHUNK_PAGES * 4096u)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

static struct page_chunk *pool_grow(void)
{
    if (pool_count == POOL_MAX_CHUNKS)
        return NULL;
    struct page_chunk *c = heap_alloc(&kernel_heap, sizeof(*c));
    uint8_t *base = c ? alloc_aligned(&kernel_heap, POOL_CHUNK_PAGES * 4096u, 4096, false) : NULL;
    if (!base) {
        heap_free(&kernel_heap, c);
        return NULL;
    }
    c->base = base;
    c->free_list = NULL;
    for (uint32_t i = POOL_CHUNK_PAGES; i-- > 0;) {
        void **page = (void **)(base + i * 4096u);
        *page = c->free_list;
        c->free_list = page;
    }
    c->free_count = POOL_CHUNK_PAGES;
    int at = pool_count;
    while (at > 0 && pool_chunks[at - 1]->base > base) {
        pool_chunks[at] = pool_chunks[at - 1];
        at--;
    }
    pool_chunks[at] = c;
    pool_count++;
    c->next_partial = pool_partial;
    pool_partial = c;
    return c;
}

static void *page_alloc(void)
{
    struct page_chunk *c = pool_partial;
    if (!c && !(c = pool_grow()))
        return NULL;
    void **page = c->free_list;
    c->free_list = *page;
    if (--c->free_count == 0)
        pool_partial = c->next_partial;
    return page;
}

static void partial_remove(struct page_chunk *c)
{
    for (struct page_chunk **pp = &pool_partial; *pp; pp = &(*pp)->next_partial)
        if (*pp == c) {
            *pp = c->next_partial;
            return;
        }
}

static void page_free(int index, void *ptr)
{
    struct page_chunk *c = pool_chunks[index];
    *(void **)ptr = c->free_list;
    c->free_list = ptr;
    if (c->free_count++ == 0) {
        c->next_partial = pool_partial;
        pool_partial = c;
    }
    if (c->free_count == POOL_CHUNK_PAGES && pool_partial != c) {
        partial_remove(c);
        memmove(&pool_chunks[index], &pool_chunks[index + 1], (size_t)(pool_count - index - 1) * sizeof(c));
        pool_count--;
        heap_free_aligned(&kernel_heap, c->base);
        heap_free(&kernel_heap, c);
    }
}

void *kmalloc_pages(size_t size)
{
    if (size == 4096) {
        void *p = page_alloc();
        if (p)
            memset(p, 0, 4096);
        return p;
    }
    return heap_alloc_aligned(&kernel_heap, size, 4096);
}

void *kmalloc_pages_uninit(size_t size)
{
    if (size == 4096)
        return page_alloc();
    return alloc_aligned(&kernel_heap, size, 4096, false);
}

void kfree_pages(void *ptr)
{
    int index = ptr ? pool_find(ptr) : -1;
    if (index >= 0)
        page_free(index, ptr);
    else
        heap_free_aligned(&kernel_heap, ptr);
}
