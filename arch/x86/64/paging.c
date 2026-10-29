#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "../io.h"

/* The 64-bit kernel sees physical memory where it is: the entry code maps 0-4 GiB one to one with 2 MiB pages,
   device memory above that is added the same way when a driver asks for it. Pages that need other caching or a
   different target are split into 4 KiB entries. */

#define P_PRESENT 0x001ull
#define P_WRITE 0x002ull
#define P_PWT 0x008ull
#define P_PCD 0x010ull
#define P_LARGE 0x080ull
#define P_ADDR 0x000ffffffffff000ull
#define LARGE (2ull << 20)

extern uint64_t boot_pml4[512];

static bool enabled;

/* tables needed before the kernel heap exists (the boot framebuffer) come from here */
static uint64_t early_tables[16][512] __attribute__((aligned(4096)));
static int early_used;

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
        return (uint64_t *)(uintptr_t)(*entry & P_ADDR);
    if (!create)
        return NULL;
    uint64_t *t = table_alloc();
    if (!t)
        return NULL;
    *entry = (uint64_t)(uintptr_t)t | P_PRESENT | P_WRITE;
    return t;
}

/* the page directory entry for a 2 MiB region */
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
        uint64_t *t = table_alloc();
        if (!t)
            return NULL;
        uint64_t base = *pde & P_ADDR & ~(LARGE - 1), flags = *pde & (P_WRITE | P_PWT | P_PCD | 0x4ull);
        if (*pde & (1ull << 12))
            flags |= 0x80ull;
        for (int i = 0; i < 512; i++)
            t[i] = (base + (uint64_t)i * 4096) | flags | P_PRESENT;
        *pde = (uint64_t)(uintptr_t)t | P_PRESENT | P_WRITE;
        for (int i = 0; i < 512; i++)
            invlpg(base + (uint64_t)i * 4096);
    }
    uint64_t *t = next_level(pde, create);
    return t ? &t[(va >> 12) & 511] : NULL;
}

void *paging_init(uint32_t table_start, uint32_t table_limit, uint32_t start, uint32_t end, size_t *size)
{
    UNUSED(table_start);
    UNUSED(table_limit);
    enabled = true;
    *size = end - start;
    return (void *)(uintptr_t)start;
}

bool vmm_available(void)
{
    return enabled;
}

uint32_t vmm_phys(const void *ptr)
{
    return (uint32_t)(uintptr_t)ptr;
}

bool vmm_pae(void)
{
    return true;
}

static void *map_identity(uint64_t phys, size_t size, uint64_t cache)
{
    uint64_t first = phys & ~4095ull, last = (phys + size + 4095) & ~4095ull;
    for (uint64_t a = first; a < last;) {
        uint64_t *pde = pd_entry(a, true);
        if (!pde)
            return NULL;
        bool whole = !(a & (LARGE - 1)) && last - a >= LARGE;
        if (whole && (!(*pde & P_PRESENT) || (*pde & P_LARGE))) {
            uint64_t want = a | P_PRESENT | P_WRITE | P_LARGE | cache;
            if (*pde != want) {
                *pde = want;
                invlpg(a);
            }
            a += LARGE;
            continue;
        }
        uint64_t *e = pte(a, true);
        if (!e)
            return NULL;
        uint64_t want = a | P_PRESENT | P_WRITE | cache;
        if (*e != want) {
            *e = want;
            invlpg(a);
        }
        a += 4096;
    }
    return (void *)(uintptr_t)phys;
}

void *vmm_map_phys64(uint64_t phys, size_t size)
{
    return map_identity(phys, size, 0);
}

void *vmm_map_phys(uint32_t phys, size_t size)
{
    return map_identity(phys, size, 0);
}

void *vmm_map_uncached(uint64_t phys, size_t size)
{
    return map_identity(phys, size, P_PWT | P_PCD);
}

void *vmm_map_framebuffer(uint64_t phys, size_t size)
{
    return map_identity(phys, size, cpu_info()->pat ? P_PWT : 0);
}

int vmm_map(uintptr_t vaddr, void *memory, size_t size)
{
    uintptr_t phys = (uintptr_t)memory;
    for (size_t off = 0; off < size; off += 4096) {
        uint64_t *e = pte(vaddr + off, true);
        if (!e)
            return -1;
        *e = (phys + off) | P_PRESENT | P_WRITE;
        invlpg(vaddr + off);
    }
    return 0;
}

/* addresses taken away from the identity map read as unmapped until mapped again */
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

/* address spaces, frames above 4 GiB, copy-on-write and committed regions belong to the Windows subsystem, which
   the 64-bit kernel does not run yet: one address space, no extra frames */
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
        if (vmm_map(vaddr + i * 4096, (void *)((uintptr_t)pfns[i] << 12), 4096))
            return -1;
    return 0;
}

uint32_t vmm_long_mode_root(void)
{
    return (uint32_t)(uintptr_t)boot_pml4;
}

uint32_t vmm_pae_root(void)
{
    return (uint32_t)(uintptr_t)boot_pml4;
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
