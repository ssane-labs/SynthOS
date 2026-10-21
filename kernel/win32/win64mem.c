#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "win64.h"

/* Addresses above 4 GB for 64-bit programs (Chromium reserves 16 GB pools there). A reservation is bookkeeping;
   its committed pages live in 2 MB shadow slots of one low arena, so 32-bit kernel code reaches them through
   w64_ptr, and a fault on the high address maps the shadow page's frame there as well. */

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_RESET 0x80000u
#define MEM_RESET_UNDO 0x1000000u
#define PAGE_NOACCESS 0x01u
#define ERROR_INVALID_ADDRESS 487
#define SLOT_SHIFT 21
#define SLOT (1u << SLOT_SHIFT)
#define GRAN 0x10000ull
#define HIGH_START 0x100000000ull
#define HIGH_AUTO 0x10000000000ull
#define HIGH_END 0x7fff00000000ull

struct hregion {
    uint64_t base, size;
    uint32_t alloc_protect, protect;
    uint16_t *slot;
};

struct high_mem {
    struct hregion *regions;
    int count, cap;
    uint64_t next;
    uint32_t arena, slots, hint;
    uint64_t *slot_va;
    uint16_t *slot_pages;
};

#define P(v) w64_ptr(v)
#define U(v) ((uint32_t)(uintptr_t)(v))

static W32_STATE struct high_mem *hm;

static const uint32_t arena_sizes[] = {1024u << 20, 768u << 20, 512u << 20, 256u << 20, 64u << 20};

static struct high_mem *high(void)
{
    if (!hm && (hm = kzalloc(sizeof(*hm))))
        hm->next = HIGH_AUTO;
    return hm;
}

static struct hregion *region_at(uint64_t a)
{
    struct high_mem *h = hm;
    int lo = 0, hi = h ? h->count - 1 : -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        struct hregion *r = &h->regions[mid];
        if (a < r->base)
            hi = mid - 1;
        else if (a >= r->base + r->size)
            lo = mid + 1;
        else
            return r;
    }
    return NULL;
}

static uint32_t chunk_of(const struct hregion *r, uint64_t a)
{
    return (uint32_t)((a >> SLOT_SHIFT) - (r->base >> SLOT_SHIFT));
}

static uint32_t shadow(const struct hregion *r, uint64_t a)
{
    uint16_t s = r->slot[chunk_of(r, a)];
    return s ? hm->arena + (uint32_t)(s - 1) * SLOT + (uint32_t)(a & (SLOT - 1)) : 0;
}

void *w64_high_ptr(uint64_t v)
{
    struct hregion *r = region_at(v);
    uint32_t low = r ? shadow(r, v) : 0;
    return (void *)(uintptr_t)(low ? low : (uint32_t)v);
}

uint64_t w64_high_back(uint64_t v)
{
    struct high_mem *h = hm;
    if (!h || !h->arena || v < h->arena || v >= h->arena + h->slots * SLOT)
        return v;
    uint32_t s = (uint32_t)(v - h->arena) >> SLOT_SHIFT;
    return h->slot_va[s] ? h->slot_va[s] + (v & (SLOT - 1)) : v;
}

bool w64_high_fault(uint64_t a)
{
    struct hregion *r = w32 && hm ? region_at(a) : NULL;
    uint32_t low = r ? shadow(r, a) & ~4095u : 0;
    if (!low || !w32_vm_committed(low) || (!vmm_mapped(low) && vmm_commit_range(low, 4096) != 0))
        return false;
    return vmm_high_alias(a & ~4095ull, low) == 0;
}

static bool arena_init(struct high_mem *h)
{
    if (h->arena)
        return true;
    for (size_t i = 0; i < ARRAY_SIZE(arena_sizes) && !h->arena; i++) {
        uint32_t base = w32_vm_reserve(arena_sizes[i]);
        if (!base)
            continue;
        h->slots = arena_sizes[i] / SLOT;
        h->slot_va = kzalloc(h->slots * sizeof(*h->slot_va));
        h->slot_pages = kzalloc(h->slots * sizeof(*h->slot_pages));
        h->arena = base;
    }
    return h->arena && h->slot_va && h->slot_pages;
}

static bool slot_free(const struct high_mem *h, int s)
{
    return s >= 0 && (uint32_t)s < h->slots && !h->slot_va[s];
}

/* neighbours of a chunk get neighbouring slots where possible, so buffers that cross 2 MB stay contiguous */
static int slot_pick(struct high_mem *h, const struct hregion *r, uint32_t c, uint32_t chunks)
{
    if (c && r->slot[c - 1] && slot_free(h, r->slot[c - 1]))
        return r->slot[c - 1];
    if (c + 1 < chunks && r->slot[c + 1] && slot_free(h, r->slot[c + 1] - 2))
        return r->slot[c + 1] - 2;
    for (uint32_t k = 0; k < h->slots; k++) {
        uint32_t s = (h->hint + k) % h->slots;
        if (slot_free(h, (int)s) && (s + 1 == h->slots || slot_free(h, (int)s + 1))) {
            h->hint = s + 2;
            return (int)s;
        }
    }
    for (uint32_t s = 0; s < h->slots; s++)
        if (slot_free(h, (int)s))
            return (int)s;
    return -1;
}

static uint32_t chunk_count(const struct hregion *r)
{
    return (uint32_t)(((r->base + r->size - 1) >> SLOT_SHIFT) - (r->base >> SLOT_SHIFT) + 1);
}

static bool commit(struct hregion *r, uint64_t start, uint64_t end)
{
    struct high_mem *h = hm;
    if (!arena_init(h))
        return false;
    uint32_t chunks = chunk_count(r);
    for (uint64_t a = start; a < end;) {
        uint32_t c = chunk_of(r, a);
        uint64_t stop = MIN(end, ((a >> SLOT_SHIFT) + 1) << SLOT_SHIFT);
        if (!r->slot[c]) {
            int s = slot_pick(h, r, c, chunks);
            if (s < 0)
                return false;
            h->slot_va[s] = a & ~(uint64_t)(SLOT - 1);
            r->slot[c] = (uint16_t)(s + 1);
        }
        uint32_t low = shadow(r, a), n = 0;
        for (uint32_t p = low; p < low + (uint32_t)(stop - a); p += 4096u)
            n += !w32_vm_committed(p);
        h->slot_pages[r->slot[c] - 1] += (uint16_t)n;
        w32_vm_set_committed(low, (uint32_t)(stop - a), true);
        a = stop;
    }
    return true;
}

static void decommit(struct hregion *r, uint64_t start, uint64_t end)
{
    struct high_mem *h = hm;
    for (uint64_t a = start; a < end;) {
        uint32_t c = chunk_of(r, a);
        uint64_t stop = MIN(end, ((a >> SLOT_SHIFT) + 1) << SLOT_SHIFT);
        if (r->slot[c]) {
            uint32_t s = r->slot[c] - 1u, low = shadow(r, a), n = 0;
            for (uint32_t p = low; p < low + (uint32_t)(stop - a); p += 4096u)
                n += w32_vm_committed(p);
            w32_vm_set_committed(low, (uint32_t)(stop - a), false);
            h->slot_pages[s] -= (uint16_t)MIN(n, h->slot_pages[s]);
            if (!h->slot_pages[s]) {
                h->slot_va[s] = 0;
                r->slot[c] = 0;
            }
        }
        a = stop;
    }
    vmm_high_unmap(start, end - start);
}

static bool range_free(uint64_t base, uint64_t size)
{
    if (base < HIGH_START || base + size > HIGH_END || base + size < base)
        return false;
    for (int i = 0; hm && i < hm->count; i++)
        if (base < hm->regions[i].base + hm->regions[i].size && hm->regions[i].base < base + size)
            return false;
    return true;
}

static uint64_t find_free(struct high_mem *h, uint64_t size)
{
    uint64_t align = size >= (4ull << 30) ? (4ull << 30) : GRAN;
    for (int tries = 0; tries < 2; tries++) {
        uint64_t base = ALIGN_UP(h->next, align);
        while (base + size <= HIGH_END) {
            if (range_free(base, size)) {
                h->next = base + size;
                return base;
            }
            base = ALIGN_UP(base + GRAN, align);
            for (int i = 0; i < h->count; i++)
                if (base < h->regions[i].base + h->regions[i].size && h->regions[i].base < base + size)
                    base = ALIGN_UP(h->regions[i].base + h->regions[i].size, align);
        }
        h->next = HIGH_AUTO;
    }
    return 0;
}

static struct hregion *add_region(struct high_mem *h, uint64_t base, uint64_t size, uint32_t protect)
{
    if (h->count == h->cap) {
        int cap = h->cap ? h->cap * 2 : 16;
        struct hregion *grown = krealloc(h->regions, sizeof(*grown) * (size_t)cap);
        if (!grown)
            return NULL;
        h->regions = grown;
        h->cap = cap;
    }
    struct hregion r = {base, size, protect, protect, NULL};
    if (!(r.slot = kzalloc(chunk_count(&r) * sizeof(uint16_t))))
        return NULL;
    int at = 0;
    while (at < h->count && h->regions[at].base < base)
        at++;
    memmove(&h->regions[at + 1], &h->regions[at], sizeof(*h->regions) * (size_t)(h->count - at));
    h->regions[at] = r;
    h->count++;
    return &h->regions[at];
}

static void remove_region(struct hregion *r)
{
    decommit(r, r->base, r->base + r->size);
    kfree(r->slot);
    int i = (int)(r - hm->regions);
    memmove(r, r + 1, sizeof(*r) * (size_t)(hm->count - i - 1));
    hm->count--;
}

void w64_mem_cleanup(void)
{
    if (!hm)
        return;
    for (int i = 0; i < hm->count; i++)
        kfree(hm->regions[i].slot);
    kfree(hm->regions);
    kfree(hm->slot_va);
    kfree(hm->slot_pages);
    kfree(hm);
    hm = NULL;
}

static uint64_t high_alloc(uint64_t addr, uint64_t size, uint32_t type, uint32_t protect)
{
    struct high_mem *h = high();
    if (!h || !size) {
        w32_set_error(!h ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (type & (MEM_RESET | MEM_RESET_UNDO))
        return region_at(addr) ? addr : 0;
    struct hregion *r;
    if ((type & MEM_RESERVE) || !addr) {
        uint64_t base, length;
        if (addr) {
            base = addr & ~(GRAN - 1);
            length = ALIGN_UP(addr + size, 4096ull) - base;
            if (!range_free(base, length)) {
                w32_set_error(ERROR_INVALID_ADDRESS);
                return 0;
            }
        } else {
            length = ALIGN_UP(size, 4096ull);
            if (!(base = find_free(h, length))) {
                w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
        }
        if (!(r = add_region(h, base, length, protect))) {
            w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if ((type & MEM_COMMIT) && !commit(r, base, base + length)) {
            remove_region(r);
            w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        return base;
    }
    uint64_t start = addr & ~4095ull, end = ALIGN_UP(addr + size, 4096ull);
    if (!(r = region_at(addr)) || end > r->base + r->size) {
        w32_set_error(ERROR_INVALID_ADDRESS);
        return 0;
    }
    if (!commit(r, start, end)) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    r->protect = protect;
    return start;
}

static uint64_t h_virtual_alloc(uint64_t *a, const uint64_t *f, void *t)
{
    if (!(a[0] >> 32) && !(a[1] >> 32) && !region_at(a[0])) {
        uint64_t low = w64_ret((uint32_t)T32(t, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3]));
        if (low || a[0])
            return low;
    }
    return high_alloc(a[0], a[1], (uint32_t)a[2], (uint32_t)a[3]);
}

static uint64_t h_virtual_free(uint64_t *a, const uint64_t *f, void *t)
{
    struct hregion *r = a[0] >> 32 ? region_at(a[0]) : NULL;
    if (!(a[0] >> 32))
        return T32(t, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
    if (!r) {
        w32_set_error(ERROR_INVALID_ADDRESS);
        return 0;
    }
    uint32_t type = (uint32_t)a[2];
    if (type & MEM_RELEASE) {
        if (a[0] != r->base || a[1]) {
            w32_set_error(ERROR_INVALID_PARAMETER);
            return 0;
        }
        remove_region(r);
        return 1;
    }
    if (type & MEM_DECOMMIT) {
        uint64_t start = a[0] & ~4095ull, end = a[1] ? ALIGN_UP(a[0] + a[1], 4096ull) : r->base + r->size;
        decommit(r, start, MIN(end, r->base + r->size));
        return 1;
    }
    w32_set_error(ERROR_INVALID_PARAMETER);
    return 0;
}

static uint64_t h_virtual_protect(uint64_t *a, const uint64_t *f, void *t)
{
    if (!(a[0] >> 32))
        return T32(t, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], U(P(a[3])));
    struct hregion *r = region_at(a[0]);
    if (!r) {
        w32_set_error(ERROR_INVALID_ADDRESS);
        return 0;
    }
    if (a[3])
        *(uint32_t *)P(a[3]) = r->protect;
    r->protect = (uint32_t)a[2];
    return 1;
}

static void put_mbi(uint8_t *o, uint64_t base, uint64_t alloc, uint32_t alloc_protect, uint64_t size, uint32_t state,
                    uint32_t protect, uint32_t type)
{
    memset(o, 0, 48);
    memcpy(o, &base, 8);
    memcpy(o + 8, &alloc, 8);
    memcpy(o + 16, &alloc_protect, 4);
    memcpy(o + 24, &size, 8);
    memcpy(o + 32, &state, 4);
    memcpy(o + 36, &protect, 4);
    memcpy(o + 40, &type, 4);
}

static bool page_on(const struct hregion *r, uint64_t a)
{
    uint32_t low = shadow(r, a);
    return low && w32_vm_committed(low);
}

static uint64_t high_query(uint64_t addr, uint8_t *o)
{
    uint64_t a = addr & ~4095ull;
    struct hregion *r = region_at(a);
    if (r) {
        bool on = page_on(r, a);
        uint64_t end = a + 4096;
        while (end < r->base + r->size) {
            if (!r->slot[chunk_of(r, end)] && !on) {
                end = MIN(((end >> SLOT_SHIFT) + 1) << SLOT_SHIFT, r->base + r->size);
                continue;
            }
            if (page_on(r, end) != on)
                break;
            end += 4096;
        }
        put_mbi(o, a, r->base, r->alloc_protect, end - a, on ? MEM_COMMIT : MEM_RESERVE, on ? r->protect : 0,
                MEM_PRIVATE);
        return 48;
    }
    uint64_t next = HIGH_END;
    for (int i = 0; hm && i < hm->count; i++)
        if (hm->regions[i].base > a) {
            next = hm->regions[i].base;
            break;
        }
    if (a >= HIGH_END)
        return 0;
    put_mbi(o, a, 0, 0, next - a, MEM_FREE, PAGE_NOACCESS, 0);
    return 48;
}

static uint64_t h_virtual_query(uint64_t *a, const uint64_t *f, void *t)
{
    uint8_t *o = P(a[1]);
    if (a[0] >> 32)
        return a[2] >= 48 ? high_query(a[0], o) : 0;
    uint32_t m[7];
    if (!(uint32_t)T32(t, (uint32_t)a[0], U(m), sizeof(m)))
        return 0;
    put_mbi(o, m[0], m[1], m[2], m[3], m[4], m[5], m[6]);
    return 48;
}

static const struct w64_export mem_exports[] = {
    W64_FN("VirtualAlloc", h_virtual_alloc),
    W64_FN("VirtualFree", h_virtual_free),
    W64_FN("VirtualProtect", h_virtual_protect),
    W64_FN("VirtualQuery", h_virtual_query),
};
W64_EXPORTS(mem_module, "kernel32.dll", mem_exports);
