#ifndef SYNTH_VMM_H
#define SYNTH_VMM_H

#include <synth/types.h>

#define VMM_PAGE 4096u
#define VMM_USER_START 0x00400000u
#define VMM_USER_END 0xb0000000u

bool vmm_available(void);
int vmm_map(uintptr_t vaddr, void *kernel_memory, size_t size);
void vmm_unmap(uintptr_t vaddr, size_t size);
bool vmm_range_free(uintptr_t vaddr, size_t size);
void *vmm_map_phys(uint32_t phys, size_t size);
void *vmm_map_phys64(uint64_t phys, size_t size);
bool vmm_pae(void);
size_t vmm_high_pages(void);
size_t vmm_high_total_pages(void);
bool vmm_frames_alloc(uint32_t *pfns, size_t n);
void vmm_frames_free(const uint32_t *pfns, size_t n);
int vmm_map_frames(uintptr_t vaddr, const uint32_t *pfns, size_t n);
uint32_t vmm_long_mode_root(void);
uint32_t vmm_pae_root(void);
uint32_t vmm_phys(const void *ptr);

struct vmm_space;
struct vmm_space *vmm_space_create(void);
struct vmm_space *vmm_space_current(void);
void vmm_space_switch(struct vmm_space *s);
void vmm_space_destroy(struct vmm_space *s);
void vmm_space_set_commit(uintptr_t start, size_t size);
void vmm_release(uintptr_t start, size_t size);
bool vmm_commit_fault(uintptr_t address);
bool vmm_mapped(uintptr_t address);
int vmm_map_pages(uintptr_t vaddr, void *const *pages, size_t count, bool cow);
void vmm_make_cow(uintptr_t vaddr, size_t size);
bool vmm_cow_fault(uintptr_t address);
void vmm_set_commit_hook(bool (*fn)(uintptr_t address));
void vmm_set_map_hook(bool (*fn)(uintptr_t address));
int vmm_commit_range(uintptr_t start, size_t size);

void *kmalloc_pages(size_t size);
void *kmalloc_pages_uninit(size_t size);
void kfree_pages(void *ptr);

#endif
