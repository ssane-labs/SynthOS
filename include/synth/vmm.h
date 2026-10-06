#ifndef SYNTH_VMM_H
#define SYNTH_VMM_H

#include <synth/types.h>

#define VMM_PAGE 4096u
#define VMM_USER_START 0x00400000u
#define VMM_USER_END 0xbf000000u

bool vmm_available(void);
int vmm_map(uintptr_t vaddr, void *kernel_memory, size_t size);
void vmm_unmap(uintptr_t vaddr, size_t size);
bool vmm_range_free(uintptr_t vaddr, size_t size);

void *kmalloc_pages(size_t size);
void kfree_pages(void *ptr);

#endif
