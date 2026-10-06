#ifndef SYNTH_HEAP_H
#define SYNTH_HEAP_H

#include <synth/types.h>

struct heap_stats {
    size_t total;
    size_t used;
    size_t free;
    size_t largest_free;
    size_t blocks;
    size_t peak;
};

#define HEAP_BINS 28

struct heap {
    uint8_t *start;
    uint8_t *end;
    void *bins[HEAP_BINS];
    size_t used;
    size_t peak;
    bool strict;
};

void heap_create(struct heap *h, void *base, size_t size, bool strict);
void *heap_alloc(struct heap *h, size_t size);
void *heap_realloc(struct heap *h, void *ptr, size_t size);
void heap_free(struct heap *h, void *ptr);
size_t heap_size_of(struct heap *h, void *ptr);
void *heap_alloc_aligned(struct heap *h, size_t size, size_t align);
void heap_free_aligned(struct heap *h, void *ptr);
void heap_stats_of(struct heap *h, struct heap_stats *out);

void heap_init(void *base, size_t size);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
char *kstrdup(const char *s);
char *kstrndup(const char *s, size_t n);
void heap_get_stats(struct heap_stats *out);

#endif
