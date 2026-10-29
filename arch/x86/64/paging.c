#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "../io.h"

/* The 64-bit kernel's address space:
     0 - 4 MiB                      low memory, one to one (BIOS data, start-up code)
     4 MiB - 2.75 GiB               32-bit programs, different in every address space
     0xb0000000 (KERNEL_OFFSET)     the kernel image (the first 256 MiB of physical memory)
     0xc0000000 - 4 GiB             the kernel heap
     4 GiB - 128 TiB                64-bit programs, different in every address space
     DIRECT (0xffff800000000000)    physical memory, for devices and page tables
   so every pointer a 32-bit program may see (heap objects, the kernel's static data, call gate stubs) is below
   4 GiB. Every space has its own PML4, the table below its first entry and the page directories for 0-3 GiB;
   the heap's directory and DIRECT are shared, and the kernel entries of the first three directories are copied
   into every space. */

#define P_PRESENT 0x001ull
#define P_WRITE 0x002ull
#define P_USER 0x004ull
#define P_PWT 0x008ull
#define P_PCD 0x010ull
#define P_LARGE 0x080ull
#define P_OWNED 0x200ull
#define P_COW 0x400ull
#define P_ADDR 0x000ffffffffff000ull
#define LARGE (2ull << 20)
#define DIRECT 0xffff800000000000ull
#define HEAP_VIRT 0xc0000000ull
#define HEAP_LIMIT 0x3f000000ull
#define MAX_REGIONS 8

extern uint64_t boot_pml4[512], boot_pdpt_low[512], boot_pd_low[4][512];

static bool enabled;
static uint64_t heap_phys, heap_bytes;

/* tables needed before the kernel heap exists come from here */
static uint64_t early_tables[24][512] __attribute__((aligned(4096)));
static int early_used;

struct vmm_space {
    uint64_t *pml4, *pdpt, *dirs[3];
    uintptr_t commit_start, commit_end;
    struct vmm_space *next;
};

static struct vmm_space boot_space = {.pml4 = boot_pml4, .pdpt = boot_pdpt_low,
                                      .dirs = {boot_pd_low[0], boot_pd_low[1], boot_pd_low[2]}};
static struct vmm_space *current_space = &boot_space;
static struct vmm_space *spaces;

struct region {
    uint32_t first_pfn, count, free, hint;
    uint32_t *bitmap;
};

static struct region regions[MAX_REGIONS];
static int region_count;
static bool high_ready;

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

/* a table or page from the heap, by its physical address */
static void *heap_page(uint64_t phys)
{
    return (void *)(uintptr_t)(HEAP_VIRT + phys - heap_phys);
}

static bool heap_frame(uint64_t phys)
{
    return phys >= heap_phys && phys - heap_phys < heap_bytes;
}

static void table_free(uint64_t entry)
{
    uint64_t phys = entry & P_ADDR;
    if (heap_frame(phys))
        kfree_pages(heap_page(phys));
}

static inline void invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static void flush_all(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

static void flush_range(uint64_t va, uint64_t size)
{
    if (size > 64ull * 4096) {
        flush_all();
        return;
    }
    for (uint64_t off = 0; off < size; off += 4096)
        invlpg(va + off);
}

static bool user_low(uint64_t va)
{
    return va >= VMM_USER_START && va < VMM_USER_END;
}

static uint64_t *next_level(uint64_t *entry, bool create, uint64_t flags)
{
    if (*entry & P_PRESENT)
        return table_at(*entry);
    if (!create)
        return NULL;
    uint64_t *t = table_alloc();
    if (!t)
        return NULL;
    *entry = vmm_phys(t) | P_PRESENT | P_WRITE | flags;
    return t;
}

static uint64_t *pd_entry(uint64_t va, bool create)
{
    uint64_t *pdpt = next_level(&current_space->pml4[(va >> 39) & 511], create, P_USER);
    if (!pdpt)
        return NULL;
    uint64_t *pd = next_level(&pdpt[(va >> 30) & 511], create, P_USER);
    return pd ? &pd[(va >> 21) & 511] : NULL;
}

/* a kernel entry of the first three directories changed: every space has its own copy */
static void kernel_pde_sync(uint64_t va, uint64_t value)
{
    if (va >= 0xc0000000ull || user_low(va))
        return;
    uint32_t dir = (uint32_t)(va >> 30), index = (uint32_t)(va >> 21) & 511;
    boot_space.dirs[dir][index] = value;
    for (struct vmm_space *s = spaces; s; s = s->next)
        s->dirs[dir][index] = value;
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
        uint64_t base = *pde & P_ADDR & ~(LARGE - 1), flags = *pde & (P_WRITE | P_PWT | P_PCD | P_USER);
        if (*pde & (1ull << 12))
            flags |= 0x80ull;
        for (int i = 0; i < 512; i++)
            t[i] = (base + (uint64_t)i * 4096) | flags | P_PRESENT;
        *pde = vmm_phys(t) | P_PRESENT | P_WRITE | P_USER;
        kernel_pde_sync(va, *pde);
        uint64_t va_base = va & ~(LARGE - 1);
        for (int i = 0; i < 512; i++)
            invlpg(va_base + (uint64_t)i * 4096);
    }
    uint64_t *t = next_level(pde, create, P_USER);
    if (t && (*pde & P_PRESENT))
        kernel_pde_sync(va, *pde);
    return t ? &t[(va >> 12) & 511] : NULL;
}

static uint64_t pte_value(uint64_t va)
{
    uint64_t *pde = pd_entry(va, false);
    if (!pde || !(*pde & P_PRESENT))
        return 0;
    if (*pde & P_LARGE)
        return (*pde & P_ADDR & ~(LARGE - 1)) + (va & (LARGE - 1) & ~4095ull) + (*pde & 0xfffull & ~P_LARGE);
    uint64_t *e = pte(va, false);
    return e ? *e : 0;
}

/* maps [va, va + size) to [phys, ...) with 2 MiB pages where both line up */
static bool map_range(uint64_t va, uint64_t phys, uint64_t size, uint64_t flags)
{
    uint64_t end = va + size;
    while (va < end) {
        if (!(va & (LARGE - 1)) && !(phys & (LARGE - 1)) && end - va >= LARGE && !user_low(va)) {
            uint64_t *pde = pd_entry(va, true);
            if (!pde)
                return false;
            if (!(*pde & P_PRESENT) || (*pde & P_LARGE)) {
                *pde = phys | P_PRESENT | P_WRITE | P_LARGE | flags;
                kernel_pde_sync(va, *pde);
                invlpg(va);
                va += LARGE;
                phys += LARGE;
                continue;
            }
        }
        uint64_t *e = pte(va, true);
        if (!e)
            return false;
        *e = phys | P_PRESENT | P_WRITE | flags;
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
    /* the start-up code is done with the one-to-one map of the programs' range */
    for (uint64_t va = VMM_USER_START; va < VMM_USER_END; va += LARGE)
        boot_pd_low[va >> 30][(va >> 21) & 511] = 0;
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | 0x10000ull) : "memory");
    flush_all();
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
    if (enabled && user_low(v))
        return (uint32_t)(pte_value(v & ~4095ull) & P_ADDR) | (uint32_t)(v & 4095);
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
    if (!enabled || (vaddr & 4095) || ((uintptr_t)memory & 4095))
        return -1;
    for (size_t off = 0; off < size; off += 4096) {
        uint64_t *e = pte(vaddr + off, true);
        if (!e) {
            flush_range(vaddr, off);
            return -1;
        }
        *e = vmm_phys((const uint8_t *)memory + off) | P_PRESENT | P_WRITE | P_USER;
    }
    flush_range(vaddr, size);
    return 0;
}

void vmm_unmap(uintptr_t vaddr, size_t size)
{
    for (size_t off = 0; off < size; off += 4096) {
        uint64_t *e = pte(vaddr + off, false);
        if (e)
            *e = 0;
    }
    flush_range(vaddr, size);
}

bool vmm_mapped(uintptr_t address)
{
    return enabled && (pte_value(address & ~4095ull) & P_PRESENT);
}

bool vmm_range_free(uintptr_t vaddr, size_t size)
{
    if (vaddr < VMM_USER_START || vaddr + size > VMM_USER_END || vaddr + size < vaddr)
        return false;
    for (uintptr_t v = vaddr & ~4095ull; v < vaddr + size;) {
        uint64_t *pde = pd_entry(v, false);
        if (!pde || !(*pde & P_PRESENT)) {
            v = (v + LARGE) & ~(LARGE - 1);
            continue;
        }
        if (pte_value(v) & P_PRESENT)
            return false;
        v += 4096;
    }
    return true;
}

/* frames above the kernel heap, below and above 4 GiB, hold the pages of programs */
static void high_init(void)
{
    high_ready = true;
    const struct x86_bootinfo *bi = x86_boot_info();
    const struct e820_entry *map = (const struct e820_entry *)(uintptr_t)bi->mmap_addr;
    for (uint32_t i = 0; i < bi->mmap_count && region_count < MAX_REGIONS; i++) {
        if (map[i].type != 1)
            continue;
        uint64_t start = MAX(map[i].base, heap_phys + heap_bytes), end = map[i].base + map[i].length;
        start = (start + 4095) & ~4095ull;
        end &= ~4095ull;
        if (end <= start || (start >> 12) > 0xffffffffull)
            continue;
        uint64_t count = MIN((end - start) >> 12, 0xffffffffull - (start >> 12));
        struct region *r = &regions[region_count];
        r->bitmap = kzalloc((size_t)((count + 31) / 32) * 4);
        if (!r->bitmap)
            continue;
        r->first_pfn = (uint32_t)(start >> 12);
        r->count = (uint32_t)count;
        r->free = r->count;
        region_count++;
    }
    uint64_t total = 0;
    for (int i = 0; i < region_count; i++)
        total += (uint64_t)regions[i].count * 4096;
    if (total)
        klog("memory: %u MB above the kernel heap available for programs", (unsigned)(total >> 20));
}

size_t vmm_high_pages(void)
{
    if (!high_ready)
        high_init();
    size_t free = 0;
    for (int i = 0; i < region_count; i++)
        free += regions[i].free;
    return free;
}

size_t vmm_high_total_pages(void)
{
    if (!high_ready)
        high_init();
    size_t total = 0;
    for (int i = 0; i < region_count; i++)
        total += regions[i].count;
    return total;
}

bool vmm_frames_alloc(uint32_t *pfns, size_t n)
{
    if (vmm_high_pages() < n)
        return false;
    size_t got = 0;
    for (int i = 0; i < region_count && got < n; i++) {
        struct region *r = &regions[i];
        for (uint32_t k = 0; k < r->count && got < n && r->free; k++) {
            uint32_t bit = (r->hint + k) % r->count;
            if (r->bitmap[bit / 32] & (1u << (bit % 32)))
                continue;
            r->bitmap[bit / 32] |= 1u << (bit % 32);
            r->free--;
            r->hint = bit + 1;
            pfns[got++] = r->first_pfn + bit;
        }
    }
    if (got < n) {
        vmm_frames_free(pfns, got);
        return false;
    }
    return true;
}

void vmm_frames_free(const uint32_t *pfns, size_t n)
{
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < region_count; k++) {
            struct region *r = &regions[k];
            if (pfns[i] < r->first_pfn || pfns[i] - r->first_pfn >= r->count)
                continue;
            uint32_t bit = pfns[i] - r->first_pfn;
            if (r->bitmap[bit / 32] & (1u << (bit % 32))) {
                r->bitmap[bit / 32] &= ~(1u << (bit % 32));
                r->free++;
            }
            break;
        }
}

int vmm_map_frames(uintptr_t vaddr, const uint32_t *pfns, size_t n)
{
    if (!enabled || (vaddr & 4095))
        return -1;
    for (size_t i = 0; i < n; i++) {
        uint64_t *e = pte(vaddr + i * 4096, true);
        if (!e) {
            flush_range(vaddr, i * 4096);
            return -1;
        }
        *e = ((uint64_t)pfns[i] << 12) | P_PRESENT | P_WRITE | P_USER;
    }
    flush_range(vaddr, n * 4096);
    return 0;
}

uint32_t vmm_long_mode_root(void)
{
    return vmm_phys(current_space->pml4);
}

uint32_t vmm_pae_root(void)
{
    return vmm_phys(current_space->pml4);
}

/* addresses above 4 GiB, for 64-bit programs: their pages are frames of low pages, which own them */
static uint64_t *high_pte(uint64_t va, bool create)
{
    if (va >> 47 || !(va >> 32))
        return NULL;
    uint64_t *t = current_space->pml4;
    for (int s = 39; s >= 21; s -= 9) {
        t = next_level(&t[(va >> s) & 511], create, P_USER);
        if (!t)
            return NULL;
    }
    return &t[(va >> 12) & 511];
}

static bool (*high_hook)(uint64_t address);

void vmm_set_high_hook(bool (*fn)(uint64_t address))
{
    high_hook = fn;
}

bool vmm_high_fault(uint64_t address)
{
    return enabled && high_hook && high_hook(address);
}

int vmm_high_alias(uint64_t vaddr, uintptr_t low)
{
    uint64_t e = pte_value(low & ~4095ull);
    uint64_t *p = (e & P_PRESENT) ? high_pte(vaddr, true) : NULL;
    if (!p)
        return -1;
    *p = (e & P_ADDR) | P_PRESENT | P_WRITE | P_USER;
    return 0;
}

void vmm_high_unmap(uint64_t vaddr, uint64_t size)
{
    bool any = false;
    for (uint64_t v = vaddr & ~4095ull; v < vaddr + size;) {
        uint64_t *p = high_pte(v, false);
        if (!p) {
            v = (v + LARGE) & ~(LARGE - 1);
            continue;
        }
        any |= *p != 0;
        *p = 0;
        v += 4096;
    }
    if (any)
        flush_all();
}

static void free_frame(uint64_t e)
{
    uint64_t phys = e & P_ADDR;
    if (heap_frame(phys)) {
        kfree_pages(heap_page(phys));
    } else {
        uint32_t pfn = (uint32_t)(phys >> 12);
        vmm_frames_free(&pfn, 1);
    }
}

/* drops the pages a range owns; with drop_tables also the page tables (and directory entries) under it */
static void release_range(struct vmm_space *s, uint64_t start, uint64_t end, bool drop_tables)
{
    for (uint64_t base = start & ~(LARGE - 1); base < end && base < VMM_USER_END; base += LARGE) {
        uint64_t *pde = &s->dirs[base >> 30][(base >> 21) & 511];
        if (!(*pde & P_PRESENT) || (*pde & P_LARGE) || !user_low(base))
            continue;
        uint64_t *t = table_at(*pde);
        for (uint64_t v = MAX(base, start); v < MIN(base + LARGE, end); v += 4096) {
            uint64_t *e = &t[(v >> 12) & 511];
            if ((*e & P_PRESENT) && (*e & P_OWNED))
                free_frame(*e);
            if (*e & P_OWNED)
                *e = 0;
        }
        if (drop_tables) {
            table_free(*pde);
            *pde = 0;
        }
    }
}

static void high_tables_free(struct vmm_space *s)
{
    for (int i = 0; i < 256; i++) {
        if (!(s->pml4[i] & P_PRESENT))
            continue;
        uint64_t *pdpt = table_at(s->pml4[i]);
        for (int j = i ? 0 : 4; j < 512; j++) {
            if (!(pdpt[j] & P_PRESENT))
                continue;
            uint64_t *pd = table_at(pdpt[j]);
            for (int k = 0; k < 512; k++)
                if (pd[k] & P_PRESENT)
                    table_free(pd[k]);
            table_free(pdpt[j]);
            pdpt[j] = 0;
        }
        if (i)
            table_free(s->pml4[i]);
    }
}

static void space_free(struct vmm_space *s)
{
    if (s->pml4 && s->pdpt)
        high_tables_free(s);
    if (s->pml4)
        kfree_pages(s->pml4);
    if (s->pdpt)
        kfree_pages(s->pdpt);
    for (int i = 0; i < 3; i++)
        if (s->dirs[i])
            kfree_pages(s->dirs[i]);
    kfree(s);
}

static uint64_t *zeroed_page(void)
{
    uint64_t *t = kmalloc_pages(4096);
    if (t)
        memset(t, 0, 4096);
    return t;
}

struct vmm_space *vmm_space_create(void)
{
    struct vmm_space *s = kzalloc(sizeof(struct vmm_space));
    if (!s)
        return NULL;
    s->pml4 = zeroed_page();
    s->pdpt = zeroed_page();
    for (int i = 0; i < 3; i++)
        s->dirs[i] = zeroed_page();
    if (!s->pml4 || !s->pdpt || !s->dirs[0] || !s->dirs[1] || !s->dirs[2]) {
        space_free(s);
        return NULL;
    }
    for (int i = 256; i < 512; i++)
        s->pml4[i] = boot_pml4[i];
    for (int i = 0; i < 3; i++) {
        memcpy(s->dirs[i], boot_space.dirs[i], 4096);
        s->pdpt[i] = vmm_phys(s->dirs[i]) | P_PRESENT | P_WRITE | P_USER;
    }
    s->pdpt[3] = boot_pdpt_low[3];
    s->pml4[0] = vmm_phys(s->pdpt) | P_PRESENT | P_WRITE | P_USER;
    for (uint64_t va = VMM_USER_START; va < VMM_USER_END; va += LARGE)
        s->dirs[va >> 30][(va >> 21) & 511] = 0;
    s->next = spaces;
    spaces = s;
    return s;
}

struct vmm_space *vmm_space_current(void)
{
    return current_space;
}

void vmm_space_switch(struct vmm_space *s)
{
    if (!s)
        s = &boot_space;
    if (!enabled || s == current_space)
        return;
    current_space = s;
    uint64_t root = vmm_phys(s->pml4);
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}

void vmm_space_destroy(struct vmm_space *s)
{
    if (!s || s == &boot_space || s == current_space)
        return;
    release_range(s, VMM_USER_START, VMM_USER_END, true);
    for (struct vmm_space **pp = &spaces; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    space_free(s);
}

void vmm_space_set_commit(uintptr_t start, size_t size)
{
    current_space->commit_start = start;
    current_space->commit_end = start + size;
}

void vmm_release(uintptr_t start, size_t size)
{
    release_range(current_space, start, start + size, false);
    flush_range(start, size);
}

int vmm_map_pages(uintptr_t vaddr, void *const *pages, size_t count, bool cow)
{
    if (!enabled || (vaddr & 4095))
        return -1;
    for (size_t i = 0; i < count; i++) {
        uint64_t *e = pte(vaddr + i * 4096, true);
        if (!e) {
            vmm_unmap(vaddr, i * 4096);
            return -1;
        }
        *e = vmm_phys(pages[i]) | P_PRESENT | P_USER | (cow ? P_COW : P_WRITE);
    }
    flush_range(vaddr, count * 4096);
    return 0;
}

void vmm_make_cow(uintptr_t vaddr, size_t size)
{
    for (uintptr_t v = vaddr & ~4095ull; v < vaddr + size; v += 4096) {
        uint64_t *e = pte(v, false);
        if (e && (*e & P_PRESENT) && !(*e & P_OWNED))
            *e = (*e & ~P_WRITE) | P_COW;
    }
    flush_range(vaddr, size);
}

bool vmm_cow_fault(uintptr_t address)
{
    uintptr_t v = address & ~4095ull;
    uint64_t *e = enabled ? pte(v, false) : NULL;
    if (!e || !(*e & P_PRESENT) || !(*e & P_COW))
        return false;
    void *page = kmalloc_pages_uninit(4096);
    if (!page)
        return false;
    memcpy(page, (const void *)v, 4096);
    *e = vmm_phys(page) | P_PRESENT | P_WRITE | P_USER | P_OWNED;
    invlpg(v);
    return true;
}

static bool (*commit_hook)(uintptr_t address);
static bool (*map_hook)(uintptr_t address);

void vmm_set_commit_hook(bool (*fn)(uintptr_t address))
{
    commit_hook = fn;
}

void vmm_set_map_hook(bool (*fn)(uintptr_t address))
{
    map_hook = fn;
}

/* a fresh zeroed page owned by the space at v */
static bool commit_page(uintptr_t v)
{
    uint64_t *e = pte(v, true);
    if (!e)
        return false;
    uint64_t phys;
    uint32_t pfn;
    if (vmm_frames_alloc(&pfn, 1)) {
        phys = (uint64_t)pfn << 12;
    } else {
        void *page = kmalloc_pages_uninit(4096);
        if (!page)
            return false;
        phys = vmm_phys(page);
    }
    *e = phys | P_PRESENT | P_WRITE | P_USER | P_OWNED;
    invlpg(v);
    memset((void *)v, 0, 4096);
    return true;
}

bool vmm_commit_fault(uintptr_t address)
{
    uintptr_t v = address & ~4095ull;
    if (!enabled || !user_low(v) || (pte_value(v) & P_PRESENT))
        return false;
    if (map_hook && map_hook(v))
        return true;
    if ((v < current_space->commit_start || v >= current_space->commit_end) && !(commit_hook && commit_hook(v)))
        return false;
    return commit_page(v);
}

int vmm_commit_range(uintptr_t start, size_t size)
{
    uintptr_t v0 = start & ~4095ull;
    for (uintptr_t v = v0; v < start + size; v += 4096) {
        if (pte_value(v) & P_PRESENT)
            continue;
        if (!commit_page(v)) {
            vmm_release(v0, v - v0);
            return -1;
        }
    }
    return 0;
}
