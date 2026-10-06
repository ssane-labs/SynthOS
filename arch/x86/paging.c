#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define KERNEL_HEAP_VIRT 0xc0000000u
#define HEAP_VIRT_LIMIT 0x3f000000u
#define IDENTITY_LIMIT 0x400000u
#define PTE_PRESENT 0x001u
#define PTE_WRITE 0x002u
#define PTE_USER 0x004u

static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t *tables[1024];
static uint32_t heap_phys;
static uint32_t heap_bytes;
static bool enabled;

static uint32_t phys_of(const void *ptr)
{
    uint32_t v = (uint32_t)ptr;
    if (v >= KERNEL_HEAP_VIRT && v - KERNEL_HEAP_VIRT < heap_bytes)
        return v - KERNEL_HEAP_VIRT + heap_phys;
    return v;
}

static inline void invlpg(uint32_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

void *paging_init(uint32_t start, uint32_t end, size_t *size)
{
    start = ALIGN_UP(start, 4096u);
    end &= ~4095u;
    uint32_t bytes = MIN(end - start, HEAP_VIRT_LIMIT);
    uint32_t count = (bytes + 0x3fffffu) / 0x400000u + 1;
    if (start + count * 4096u > IDENTITY_LIMIT)
        return NULL;
    uint32_t *carve = (uint32_t *)start;
    memset(carve, 0, count * 4096u);
    heap_phys = start + count * 4096u;
    heap_bytes = MIN(end - heap_phys, HEAP_VIRT_LIMIT) & ~4095u;

    tables[0] = carve;
    for (uint32_t i = 0; i < 1024; i++)
        tables[0][i] = (i * 4096u) | PTE_PRESENT | PTE_WRITE;
    page_dir[0] = (uint32_t)tables[0] | PTE_PRESENT | PTE_WRITE;

    for (uint32_t off = 0; off < heap_bytes; off += 4096u) {
        uint32_t virt = KERNEL_HEAP_VIRT + off;
        uint32_t pde = virt >> 22;
        if (!tables[pde]) {
            tables[pde] = carve + 1024u * (1 + (pde - (KERNEL_HEAP_VIRT >> 22)));
            page_dir[pde] = (uint32_t)tables[pde] | PTE_PRESENT | PTE_WRITE;
        }
        tables[pde][(virt >> 12) & 1023] = (heap_phys + off) | PTE_PRESENT | PTE_WRITE;
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(page_dir) : "memory");
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    enabled = true;
    *size = heap_bytes;
    return (void *)KERNEL_HEAP_VIRT;
}

bool vmm_available(void)
{
    return enabled;
}

bool vmm_range_free(uintptr_t vaddr, size_t size)
{
    if (vaddr < VMM_USER_START || vaddr + size > VMM_USER_END || vaddr + size < vaddr)
        return false;
    for (uintptr_t v = vaddr & ~4095u; v < vaddr + size; v += 4096u) {
        uint32_t *t = tables[v >> 22];
        if (t && (t[(v >> 12) & 1023] & PTE_PRESENT))
            return false;
    }
    return true;
}

int vmm_map(uintptr_t vaddr, void *memory, size_t size)
{
    if (!enabled || (vaddr & 4095u) || ((uintptr_t)memory & 4095u))
        return -1;
    for (uintptr_t off = 0; off < size; off += 4096u) {
        uint32_t v = vaddr + off;
        uint32_t pde = v >> 22;
        if (!tables[pde]) {
            uint32_t *t = kmalloc_pages(4096);
            if (!t)
                return -1;
            tables[pde] = t;
            page_dir[pde] = phys_of(t) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        tables[pde][(v >> 12) & 1023] = phys_of((uint8_t *)memory + off) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        invlpg(v);
    }
    return 0;
}

void vmm_unmap(uintptr_t vaddr, size_t size)
{
    for (uintptr_t off = 0; off < size; off += 4096u) {
        uint32_t v = vaddr + off;
        uint32_t *t = tables[v >> 22];
        if (t) {
            t[(v >> 12) & 1023] = 0;
            invlpg(v);
        }
    }
}
