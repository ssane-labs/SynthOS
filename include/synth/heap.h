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

void heap_init(void *base, size_t size);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
char *kstrdup(const char *s);
char *kstrndup(const char *s, size_t n);
void heap_get_stats(struct heap_stats *out);

#endif
