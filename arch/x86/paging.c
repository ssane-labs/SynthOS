#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define KERNEL_HEAP_VIRT 0xc0000000u
#define HEAP_VIRT_LIMIT 0x3f000000u
#define IDENTITY_LIMIT 0x400000u
#define LARGE (2u << 20)
#define PTE_PRESENT 0x001u
#define PTE_WRITE 0x002u
#define PTE_USER 0x004u
#define PTE_LARGE 0x080u
#define MAX_REGIONS 8

static bool enabled, pae;
static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint64_t pdpt[4] __attribute__((aligned(32)));
static uint64_t pdirs[4][512] __attribute__((aligned(4096)));
static void *tables[2048];
static uint32_t heap_phys;
static uint32_t heap_bytes;
static uint32_t early_tables[4][1024] __attribute__((aligned(4096)));
static int early_used;
static uint64_t *pml4, *long_pdpt;

struct region {
    uint32_t first_pfn, count, free, hint;
    uint32_t *bitmap;
};

static struct region regions[MAX_REGIONS];
static int region_count;
static bool high_ready;

static uint32_t phys_of(const void *ptr)
{
    uint32_t v = (uint32_t)ptr;
    if (v >= KERNEL_HEAP_VIRT && v - KERNEL_HEAP_VIRT < heap_bytes)
        return v - KERNEL_HEAP_VIRT + heap_phys;
    return v;
}

uint32_t vmm_phys(const void *ptr)
{
    return phys_of(ptr);
}

bool vmm_pae(void)
{
    return pae;
}

static inline void invlpg(uint32_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static void flush_range(uint32_t vaddr, size_t size)
{
    if (size > 64u * 4096u) {
        uint32_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        return;
    }
    for (uint32_t off = 0; off < size; off += 4096u)
        invlpg(vaddr + off);
}

static int shift(void)
{
    return pae ? 21 : 22;
}

static void set_pde(uint32_t index, uint64_t value)
{
    if (pae)
        pdirs[index >> 9][index & 511] = value;
    else
        page_dir[index] = (uint32_t)value;
}

static void set_pte(uint32_t v, uint64_t phys, uint32_t flags)
{
    void *t = tables[v >> shift()];
    if (pae)
        ((uint64_t *)t)[(v >> 12) & 511] = phys | flags;
    else
        ((uint32_t *)t)[(v >> 12) & 1023] = (uint32_t)phys | flags;
}

static bool pte_present(uint32_t v)
{
    void *t = tables[v >> shift()];
    if (!t)
        return false;
    return pae ? (((uint64_t *)t)[(v >> 12) & 511] & PTE_PRESENT) : (((uint32_t *)t)[(v >> 12) & 1023] & PTE_PRESENT);
}

static bool ensure_table(uint32_t v, uint32_t flags)
{
    uint32_t index = v >> shift();
    if (tables[index])
        return true;
    void *t = early_used < (int)ARRAY_SIZE(early_tables) ? early_tables[early_used++] : kmalloc_pages(4096);
    if (!t)
        return false;
    memset(t, 0, 4096);
    tables[index] = t;
    set_pde(index, phys_of(t) | PTE_PRESENT | PTE_WRITE | flags);
    return true;
}

static void *legacy_init(uint32_t table_start, uint32_t table_limit, uint32_t start, uint32_t end, size_t *size)
{
    uint32_t bytes = MIN(end - start, HEAP_VIRT_LIMIT);
    uint32_t count = (bytes + 0x3fffffu) / 0x400000u + 1;
    uint32_t limit = MIN(IDENTITY_LIMIT, table_limit);
    if (table_start + 2 * 4096u > limit)
        return NULL;
    count = MIN(count, (limit - table_start) / 4096u);
    bytes = MIN(bytes, (count - 1) * 0x400000u);
    uint32_t tables_end = table_start + count * 4096u;
    uint32_t *carve = (uint32_t *)table_start;
    memset(carve, 0, count * 4096u);
    if (start < tables_end && end > table_start)
        start = tables_end;
    heap_phys = start;
    heap_bytes = MIN(end - heap_phys, bytes) & ~4095u;
    tables[0] = carve;
    for (uint32_t i = 0; i < 1024; i++)
        carve[i] = (i * 4096u) | PTE_PRESENT | PTE_WRITE;
    carve[0] = 0;
    page_dir[0] = (uint32_t)carve | PTE_PRESENT | PTE_WRITE;
    for (uint32_t off = 0; off < heap_bytes; off += 4096u) {
        uint32_t virt = KERNEL_HEAP_VIRT + off;
        uint32_t pde = virt >> 22;
        if (!tables[pde]) {
            tables[pde] = carve + 1024u * (1 + (pde - (KERNEL_HEAP_VIRT >> 22)));
            page_dir[pde] = (uint32_t)tables[pde] | PTE_PRESENT | PTE_WRITE;
        }
        ((uint32_t *)tables[pde])[(virt >> 12) & 1023] = (heap_phys + off) | PTE_PRESENT | PTE_WRITE;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_dir) : "memory");
    *size = heap_bytes;
    return (void *)KERNEL_HEAP_VIRT;
}

static void *pae_init(uint32_t table_start, uint32_t table_limit, uint32_t start, uint32_t end, size_t *size)
{
    uint32_t tables_end = table_start + 2 * 4096u;
    if (tables_end > IDENTITY_LIMIT || tables_end > table_limit)
        return NULL;
    uint64_t *low = (uint64_t *)table_start;
    memset(low, 0, 2 * 4096u);
    for (uint32_t i = 0; i < 1024; i++)
        low[i] = (uint64_t)(i * 4096u) | PTE_PRESENT | PTE_WRITE;
    low[0] = 0;
    tables[0] = low;
    tables[1] = low + 512;
    pdirs[0][0] = (uint32_t)low | PTE_PRESENT | PTE_WRITE;
    pdirs[0][1] = (uint32_t)(low + 512) | PTE_PRESENT | PTE_WRITE;
    if (start < tables_end && end > table_start)
        start = tables_end;
    heap_phys = ALIGN_UP(start, LARGE);
    heap_bytes = heap_phys < end ? MIN(end - heap_phys, HEAP_VIRT_LIMIT) & ~(LARGE - 1) : 0;
    for (uint32_t off = 0; off < heap_bytes; off += LARGE) {
        uint32_t index = (KERNEL_HEAP_VIRT + off) >> 21;
        pdirs[index >> 9][index & 511] = (uint64_t)(heap_phys + off) | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
    }
    for (int i = 0; i < 4; i++)
        pdpt[i] = (uint32_t)pdirs[i] | PTE_PRESENT;
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1u << 5;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    __asm__ volatile("mov %0, %%cr3" : : "r"(pdpt) : "memory");
    *size = heap_bytes;
    return (void *)KERNEL_HEAP_VIRT;
}

void *paging_init(uint32_t table_start, uint32_t table_limit, uint32_t start, uint32_t end, size_t *size)
{
    table_start = ALIGN_UP(table_start, 4096u);
    start = ALIGN_UP(start, 4096u);
    end &= ~4095u;
    pae = cpu_info()->pae && end - start >= 2 * LARGE;
    void *heap = pae ? pae_init(table_start, table_limit, start, end, size)
                     : legacy_init(table_start, table_limit, start, end, size);
    if (!heap)
        return NULL;
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    enabled = true;
    return heap;
}

bool vmm_available(void)
{
    return enabled;
}

bool vmm_range_free(uintptr_t vaddr, size_t size)
{
    if (vaddr < VMM_USER_START || vaddr + size > VMM_USER_END || vaddr + size < vaddr)
        return false;
    for (uintptr_t v = vaddr & ~4095u; v < vaddr + size; v += 4096u)
        if (pte_present(v))
            return false;
    return true;
}

int vmm_map(uintptr_t vaddr, void *memory, size_t size)
{
    if (!enabled || (vaddr & 4095u) || ((uintptr_t)memory & 4095u))
        return -1;
    for (uintptr_t off = 0; off < size; off += 4096u) {
        uint32_t v = vaddr + off;
        if (!ensure_table(v, PTE_USER)) {
            flush_range(vaddr, off);
            return -1;
        }
        set_pte(v, phys_of((uint8_t *)memory + off), PTE_PRESENT | PTE_WRITE | PTE_USER);
    }
    flush_range(vaddr, size);
    return 0;
}

void vmm_unmap(uintptr_t vaddr, size_t size)
{
    for (uintptr_t off = 0; off < size; off += 4096u) {
        uint32_t v = vaddr + off;
        if (tables[v >> shift()])
            set_pte(v, 0, 0);
    }
    flush_range(vaddr, size);
}

void *vmm_map_phys64(uint64_t phys, size_t size)
{
    static uint32_t next = VMM_USER_END;
    uint32_t offset = (uint32_t)(phys & 4095u);
    uint64_t base = phys - offset;
    size = ALIGN_UP(size + offset, 4096u);
    if (!enabled)
        return phys >> 32 ? NULL : (void *)(uintptr_t)phys;
    if ((phys >> 32) && !pae)
        return NULL;
    if (next + size > KERNEL_HEAP_VIRT || next + size < next)
        return NULL;
    uint32_t virt = next;
    for (uint32_t off = 0; off < size; off += 4096u) {
        uint32_t v = virt + off;
        if (!ensure_table(v, 0))
            return NULL;
        set_pte(v, base + off, PTE_PRESENT | PTE_WRITE);
        invlpg(v);
    }
    next += size;
    return (void *)(uintptr_t)(virt + offset);
}

void *vmm_map_phys(uint32_t phys, size_t size)
{
    return vmm_map_phys64(phys, size);
}

static void high_init(void)
{
    high_ready = true;
    if (!pae)
        return;
    const struct x86_bootinfo *bi = x86_boot_info();
    const struct e820_entry *map = (const struct e820_entry *)bi->mmap_addr;
    for (uint32_t i = 0; i < bi->mmap_count && region_count < MAX_REGIONS; i++) {
        if (map[i].type != 1)
            continue;
        uint64_t start = MAX(map[i].base, 0x100000000ull), end = map[i].base + map[i].length;
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
        klog("memory: %u MB above 4 GB available for programs", (unsigned)(total >> 20));
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
    if (!enabled || !pae || (vaddr & 4095u))
        return -1;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = vaddr + (uint32_t)i * 4096u;
        if (!ensure_table(v, PTE_USER)) {
            flush_range(vaddr, i * 4096u);
            return -1;
        }
        set_pte(v, (uint64_t)pfns[i] << 12, PTE_PRESENT | PTE_WRITE | PTE_USER);
    }
    flush_range(vaddr, n * 4096u);
    return 0;
}

uint32_t vmm_long_mode_root(void)
{
    if (!pae)
        return 0;
    if (!pml4) {
        pml4 = kmalloc_pages(4096);
        long_pdpt = kmalloc_pages(4096);
        if (!pml4 || !long_pdpt)
            return 0;
        memset(pml4, 0, 4096);
        memset(long_pdpt, 0, 4096);
        for (int i = 0; i < 4; i++)
            long_pdpt[i] = (uint32_t)pdirs[i] | PTE_PRESENT | PTE_WRITE | PTE_USER;
        pml4[0] = phys_of(long_pdpt) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return phys_of(pml4);
}

uint32_t vmm_pae_root(void)
{
    return pae ? (uint32_t)pdpt : (uint32_t)page_dir;
}

#define PTE_OWNED 0x200u

struct vmm_space {
    void *tables[2048];
    uint32_t commit_start, commit_end;
};

static struct vmm_space boot_space;
static struct vmm_space *current_space = &boot_space;

static uint32_t user_first(void)
{
    return VMM_USER_START >> shift();
}

static uint32_t user_last(void)
{
    return VMM_USER_END >> shift();
}

static void reload_cr3(void)
{
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

struct vmm_space *vmm_space_create(void)
{
    return kzalloc(sizeof(struct vmm_space));
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
    for (uint32_t i = user_first(); i < user_last(); i++) {
        current_space->tables[i] = tables[i];
        tables[i] = s->tables[i];
        set_pde(i, tables[i] ? phys_of(tables[i]) | PTE_PRESENT | PTE_WRITE | PTE_USER : 0);
    }
    current_space = s;
    reload_cr3();
}

static uint64_t pte_get(void *t, uint32_t v)
{
    return pae ? ((uint64_t *)t)[(v >> 12) & 511] : ((uint32_t *)t)[(v >> 12) & 1023];
}

static void free_frame(uint64_t pte)
{
    uint64_t phys = pte & 0x000ffffffffff000ull;
    if (phys >> 32 || phys >= (uint64_t)heap_phys + heap_bytes || phys < heap_phys) {
        uint32_t pfn = (uint32_t)(phys >> 12);
        vmm_frames_free(&pfn, 1);
    } else {
        kfree_pages((void *)(uintptr_t)(KERNEL_HEAP_VIRT + (uint32_t)phys - heap_phys));
    }
}

static void release_tables(void **set, uint32_t start, uint32_t end, bool drop_tables)
{
    uint32_t span = 1u << shift();
    for (uint32_t i = start >> shift(); i < (end + span - 1) >> shift() && i < user_last(); i++) {
        void *t = set[i];
        if (!t)
            continue;
        uint32_t base = i << shift();
        for (uint32_t v = MAX(base, start); v < MIN(base + span, end) && v >= base; v += 4096u) {
            uint64_t e = pte_get(t, v);
            if ((e & PTE_PRESENT) && (e & PTE_OWNED)) {
                free_frame(e);
                if (pae)
                    ((uint64_t *)t)[(v >> 12) & 511] = 0;
                else
                    ((uint32_t *)t)[(v >> 12) & 1023] = 0;
            }
        }
        bool early = (uint8_t *)t >= (uint8_t *)early_tables && (uint8_t *)t < (uint8_t *)early_tables + sizeof(early_tables);
        if (drop_tables && !early) {
            kfree_pages(t);
            set[i] = NULL;
        }
    }
}

void vmm_space_destroy(struct vmm_space *s)
{
    if (!s || s == &boot_space || s == current_space)
        return;
    release_tables(s->tables, VMM_USER_START, VMM_USER_END, true);
    kfree(s);
}

void vmm_space_set_commit(uintptr_t start, size_t size)
{
    current_space->commit_start = (uint32_t)start;
    current_space->commit_end = (uint32_t)(start + size);
}

void vmm_release(uintptr_t start, size_t size)
{
    release_tables(tables, (uint32_t)start, (uint32_t)(start + size), false);
    flush_range((uint32_t)start, size);
}

static bool (*commit_hook)(uintptr_t address);

void vmm_set_commit_hook(bool (*fn)(uintptr_t address))
{
    commit_hook = fn;
}

bool vmm_commit_fault(uintptr_t address)
{
    uint32_t v = (uint32_t)address & ~4095u;
    if (!enabled || pte_present(v))
        return false;
    if ((v < current_space->commit_start || v >= current_space->commit_end) && !(commit_hook && commit_hook(v)))
        return false;
    if (!ensure_table(v, PTE_USER))
        return false;
    uint64_t phys;
    uint32_t pfn;
    if (pae && vmm_frames_alloc(&pfn, 1)) {
        phys = (uint64_t)pfn << 12;
    } else {
        void *page = kmalloc_pages_uninit(4096);
        if (!page)
            return false;
        phys = phys_of(page);
    }
    set_pte(v, phys, PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_OWNED);
    invlpg(v);
    memset((void *)(uintptr_t)v, 0, 4096);
    return true;
}

int vmm_commit_range(uintptr_t start, size_t size)
{
    uint32_t v0 = (uint32_t)start & ~4095u;
    for (uint32_t v = v0; v < start + size; v += 4096u) {
        if (pte_present(v))
            continue;
        if (!ensure_table(v, PTE_USER)) {
            vmm_release(v0, v - v0);
            return -1;
        }
        uint64_t phys;
        uint32_t pfn;
        if (pae && vmm_frames_alloc(&pfn, 1)) {
            phys = (uint64_t)pfn << 12;
        } else {
            void *page = kmalloc_pages_uninit(4096);
            if (!page) {
                vmm_release(v0, v - v0);
                return -1;
            }
            phys = phys_of(page);
        }
        set_pte(v, phys, PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_OWNED);
        invlpg(v);
        memset((void *)(uintptr_t)v, 0, 4096);
    }
    return 0;
}
