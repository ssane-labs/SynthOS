#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "../io.h"

/* The 64-bit kernel's address space, the same in every process:
     0 - 4 MiB                      low memory, one to one (BIOS data, start-up code)
     4 MiB - 2.75 GiB               32-bit programs
     0xb0000000 (KERNEL_OFFSET)     the kernel image (the first 256 MiB of physical memory)
     0xc0000000 - 4 GiB             the kernel heap
     DIRECT (0xffff800000000000)    all physical memory, for devices and page tables
   so every pointer a 32-bit program may see (heap objects, the kernel's static data, call gate stubs) is below
   4 GiB. The boot space also maps 4 MiB - 2.75 GiB one to one, which the start-up code uses. */

#define P_PRESENT 0x001ull
#define P_WRITE 0x002ull
#define P_PWT 0x008ull
#define P_PCD 0x010ull
#define P_LARGE 0x080ull
#define P_ADDR 0x000ffffffffff000ull
#define LARGE (2ull << 20)
#define DIRECT 0xffff800000000000ull
#define HEAP_VIRT 0xc0000000ull
#define HEAP_LIMIT 0x3f000000ull

extern uint64_t boot_pml4[512];

static bool enabled;
static uint64_t heap_phys, heap_bytes;

/* tables needed before the kernel heap exists come from here */
static uint64_t early_tables[24][512] __attribute__((aligned(4096)));
static int early_used;

static inline uint64_t *table_at(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(DIRECT + (entry & P_ADDR));
}

static void *table_alloc(void)
{
    void *t = early_used < (int)ARRAY_SIZE(early_tables) ? early_tables[early_used++] : kmalloc_pages(4096);
    if (t)
        memset(t, 0, 4096);
    return t;
}

static inline void invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static uint64_t *next_level(uint64_t *entry, bool create)
{
    if (*entry & P_PRESENT)
        return table_at(*entry);
    if (!create)
        return NULL;
    uint64_t *t = table_alloc();
    if (!t)
        return NULL;
    *entry = vmm_phys(t) | P_PRESENT | P_WRITE;
    return t;
}

static uint64_t *pd_entry(uint64_t va, bool create)
{
    uint64_t *pdpt = next_level(&boot_pml4[(va >> 39) & 511], create);
    if (!pdpt)
        return NULL;
    uint64_t *pd = next_level(&pdpt[(va >> 30) & 511], create);
    return pd ? &pd[(va >> 21) & 511] : NULL;
}

/* the 4 KiB entry for an address, splitting a 2 MiB page when needed */
static uint64_t *pte(uint64_t va, bool create)
{
    uint64_t *pde = pd_entry(va, create);
    if (!pde)
        return NULL;
    if ((*pde & P_PRESENT) && (*pde & P_LARGE)) {
        if (!create)
            return NULL;
        uint64_t *t = table_alloc();
        if (!t)
            return NULL;
        uint64_t base = *pde & P_ADDR & ~(LARGE - 1), flags = *pde & (P_WRITE | P_PWT | P_PCD | 0x4ull);
        if (*pde & (1ull << 12))
            flags |= 0x80ull;
        for (int i = 0; i < 512; i++)
            t[i] = (base + (uint64_t)i * 4096) | flags | P_PRESENT;
        *pde = vmm_phys(t) | P_PRESENT | P_WRITE;
        uint64_t va_base = va & ~(LARGE - 1);
        for (int i = 0; i < 512; i++)
            invlpg(va_base + (uint64_t)i * 4096);
    }
    uint64_t *t = next_level(pde, create);
    return t ? &t[(va >> 12) & 511] : NULL;
}

/* maps [va, va + size) to [phys, ...) with 2 MiB pages where both line up */
static bool map_range(uint64_t va, uint64_t phys, uint64_t size, uint64_t cache)
{
    uint64_t end = va + size;
    while (va < end) {
        if (!(va & (LARGE - 1)) && !(phys & (LARGE - 1)) && end - va >= LARGE) {
            uint64_t *pde = pd_entry(va, true);
            if (!pde)
                return false;
            if (!(*pde & P_PRESENT) || (*pde & P_LARGE)) {
                *pde = phys | P_PRESENT | P_WRITE | P_LARGE | cache;
                invlpg(va);
                va += LARGE;
                phys += LARGE;
                continue;
            }
        }
        uint64_t *e = pte(va, true);
        if (!e)
            return false;
        *e = phys | P_PRESENT | P_WRITE | cache;
        invlpg(va);
        va += 4096;
        phys += 4096;
    }
    return true;
}

void *paging_init(uint32_t table_start, uint32_t table_limit, uint32_t start, uint32_t end, size_t *size)
{
    UNUSED(table_start);
    UNUSED(table_limit);
    heap_phys = ALIGN_UP((uint64_t)start, 4096u);
    heap_bytes = MIN(((uint64_t)end - heap_phys) & ~4095ull, HEAP_LIMIT);
    if (!map_range(HEAP_VIRT, heap_phys, heap_bytes, 0))
        return NULL;
    enabled = true;
    *size = (size_t)heap_bytes;
    return (void *)(uintptr_t)HEAP_VIRT;
}

bool vmm_available(void)
{
    return enabled;
}

uint32_t vmm_phys(const void *ptr)
{
    uint64_t v = (uintptr_t)ptr;
    if (v >= HEAP_VIRT && v - HEAP_VIRT < heap_bytes)
        return (uint32_t)(v - HEAP_VIRT + heap_phys);
    if (v >= KERNEL_OFFSET && v < KERNEL_OFFSET + 0x10000000u)
        return (uint32_t)(v - KERNEL_OFFSET);
    if (v >= DIRECT)
        return (uint32_t)(v - DIRECT);
    return (uint32_t)v;
}

bool vmm_pae(void)
{
    return true;
}

static void *map_direct(uint64_t phys, size_t size, uint64_t cache)
{
    uint64_t first = phys & ~4095ull, last = (phys + size + 4095) & ~4095ull;
    if (!map_range(DIRECT + first, first, last - first, cache))
        return NULL;
    return (void *)(uintptr_t)(DIRECT + phys);
}

void *vmm_map_phys64(uint64_t phys, size_t size)
{
    return map_direct(phys, size, 0);
}

void *vmm_map_phys(uint32_t phys, size_t size)
{
    return map_direct(phys, size, 0);
}

void *vmm_map_uncached(uint64_t phys, size_t size)
{
    return map_direct(phys, size, P_PWT | P_PCD);
}

void *vmm_map_framebuffer(uint64_t phys, size_t size)
{
    return map_direct(phys, size, cpu_info()->pat ? P_PWT : 0);
}

int vmm_map(uintptr_t vaddr, void *memory, size_t size)
{
    return map_range(vaddr, vmm_phys(memory), ALIGN_UP(size, 4096u), 0) ? 0 : -1;
}

void vmm_unmap(uintptr_t vaddr, size_t size)
{
    for (size_t off = 0; off < size; off += 4096) {
        uint64_t *e = pte(vaddr + off, false);
        if (e) {
            *e = 0;
            invlpg(vaddr + off);
        }
    }
}

bool vmm_mapped(uintptr_t address)
{
    uint64_t *pde = pd_entry(address, false);
    if (!pde || !(*pde & P_PRESENT))
        return false;
    if (*pde & P_LARGE)
        return true;
    uint64_t *e = pte(address, false);
    return e && (*e & P_PRESENT);
}

bool vmm_range_free(uintptr_t vaddr, size_t size)
{
    UNUSED(vaddr);
    UNUSED(size);
    return false;
}

/* address spaces, frames above 4 GiB, copy-on-write and committed regions belong to the Windows subsystem: one
   address space and no extra frames until it runs on this kernel */
size_t vmm_high_pages(void)
{
    return 0;
}

size_t vmm_high_total_pages(void)
{
    return 0;
}

bool vmm_frames_alloc(uint32_t *pfns, size_t n)
{
    UNUSED(pfns);
    UNUSED(n);
    return false;
}

void vmm_frames_free(const uint32_t *pfns, size_t n)
{
    UNUSED(pfns);
    UNUSED(n);
}

int vmm_map_frames(uintptr_t vaddr, const uint32_t *pfns, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!map_range(vaddr + i * 4096, (uint64_t)pfns[i] << 12, 4096, 0))
            return -1;
    return 0;
}

uint32_t vmm_long_mode_root(void)
{
    return vmm_phys(boot_pml4);
}

uint32_t vmm_pae_root(void)
{
    return vmm_phys(boot_pml4);
}

struct vmm_space {
    int unused;
};

static struct vmm_space only_space;

struct vmm_space *vmm_space_create(void)
{
    return &only_space;
}

struct vmm_space *vmm_space_current(void)
{
    return &only_space;
}

void vmm_space_switch(struct vmm_space *s)
{
    UNUSED(s);
}

void vmm_space_destroy(struct vmm_space *s)
{
    UNUSED(s);
}

void vmm_space_set_commit(uintptr_t start, size_t size)
{
    UNUSED(start);
    UNUSED(size);
}

void vmm_release(uintptr_t start, size_t size)
{
    UNUSED(start);
    UNUSED(size);
}

static bool (*commit_hook)(uintptr_t address);
static bool (*map_hook)(uintptr_t address);
static bool (*high_hook)(uint64_t address);

void vmm_set_commit_hook(bool (*fn)(uintptr_t address))
{
    commit_hook = fn;
}

void vmm_set_map_hook(bool (*fn)(uintptr_t address))
{
    map_hook = fn;
}

void vmm_set_high_hook(bool (*fn)(uint64_t address))
{
    high_hook = fn;
}

bool vmm_commit_fault(uintptr_t address)
{
    return (commit_hook && commit_hook(address)) || (map_hook && map_hook(address));
}

bool vmm_high_fault(uint64_t address)
{
    return high_hook && high_hook(address);
}

int vmm_commit_range(uintptr_t start, size_t size)
{
    UNUSED(start);
    UNUSED(size);
    return 0;
}

int vmm_map_pages(uintptr_t vaddr, void *const *pages, size_t count, bool cow)
{
    UNUSED(cow);
    for (size_t i = 0; i < count; i++)
        if (vmm_map(vaddr + i * 4096, pages[i], 4096))
            return -1;
    return 0;
}

void vmm_make_cow(uintptr_t vaddr, size_t size)
{
    UNUSED(vaddr);
    UNUSED(size);
}

bool vmm_cow_fault(uintptr_t address)
{
    UNUSED(address);
    return false;
}

int vmm_high_alias(uint64_t vaddr, uintptr_t low)
{
    return vmm_map((uintptr_t)vaddr, (void *)low, 4096);
}

void vmm_high_unmap(uint64_t vaddr, uint64_t size)
{
    vmm_unmap((uintptr_t)vaddr, (size_t)size);
}
