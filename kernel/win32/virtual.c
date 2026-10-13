#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "win32.h"

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_IMAGE 0x1000000u
#define MEM_RESET 0x80000u
#define MEM_TOP_DOWN 0x100000u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READWRITE 0x40u
#define GRANULARITY 0x10000u
#define VM_LOW 0x01000000u
#define VM_HIGH 0xb0000000u
#define ERROR_INVALID_ADDRESS 487

struct vregion {
    uint32_t base, size;
    uint32_t alloc_protect, protect;
    uint8_t *committed;
};

struct w32_vm {
    struct vregion *regions;
    int count, cap;
};

static struct w32_vm *vm(void)
{
    if (!w32->vm)
        w32->vm = kzalloc(sizeof(struct w32_vm));
    return w32->vm;
}

static int find_index(uint32_t a)
{
    struct w32_vm *v = vm();
    int lo = 0, hi = v ? v->count - 1 : -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        struct vregion *r = &v->regions[mid];
        if (a < r->base)
            hi = mid - 1;
        else if (a >= r->base + r->size)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

static struct vregion *region_at(uint32_t a)
{
    int i = find_index(a);
    return i >= 0 ? &w32->vm->regions[i] : NULL;
}

static bool page_committed(const struct vregion *r, uint32_t a)
{
    uint32_t page = (a - r->base) >> 12;
    return r->committed[page >> 3] & (1u << (page & 7));
}

static void mark(struct vregion *r, uint32_t start, uint32_t end, bool on)
{
    for (uint32_t a = start; a < end; a += 4096u) {
        uint32_t page = (a - r->base) >> 12;
        if (on)
            r->committed[page >> 3] |= (uint8_t)(1u << (page & 7));
        else
            r->committed[page >> 3] &= (uint8_t)~(1u << (page & 7));
    }
}

bool w32_vm_committed(uintptr_t address)
{
    if (!w32 || !w32->vm)
        return false;
    struct vregion *r = region_at((uint32_t)address);
    return r && page_committed(r, (uint32_t)address);
}

static bool overlaps_image(uint32_t base, uint32_t size)
{
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (!img->builtin && base < img->base + img->size && img->base < base + size)
            return true;
    return false;
}

static bool range_free(uint32_t base, uint32_t size)
{
    if (base < VMM_USER_START || base + size > VM_HIGH || base + size < base)
        return false;
    if (base < w32->arena_base + w32->arena_size && w32->arena_base < base + size)
        return false;
    if (overlaps_image(base, size))
        return false;
    struct w32_vm *v = vm();
    for (int i = 0; i < v->count; i++)
        if (base < v->regions[i].base + v->regions[i].size && v->regions[i].base < base + size)
            return false;
    return vmm_range_free(base, size);
}

static uint32_t find_free(uint32_t size, bool top_down)
{
    size = ALIGN_UP(size, GRANULARITY);
    if (top_down) {
        for (uint32_t base = (VM_HIGH - size) & ~(GRANULARITY - 1); base >= VM_LOW; base -= GRANULARITY)
            if (range_free(base, size))
                return base;
        return 0;
    }
    for (uint32_t base = VM_LOW; base + size <= VM_HIGH; base += GRANULARITY)
        if (range_free(base, size))
            return base;
    return 0;
}

static struct vregion *add_region(uint32_t base, uint32_t size, uint32_t protect)
{
    struct w32_vm *v = vm();
    if (!v)
        return NULL;
    if (v->count == v->cap) {
        int cap = v->cap ? v->cap * 2 : 64;
        struct vregion *grown = krealloc(v->regions, sizeof(*grown) * (size_t)cap);
        if (!grown)
            return NULL;
        v->regions = grown;
        v->cap = cap;
    }
    uint8_t *bits = kzalloc((size / 4096u + 7) / 8 + 1);
    if (!bits)
        return NULL;
    int at = 0;
    while (at < v->count && v->regions[at].base < base)
        at++;
    memmove(&v->regions[at + 1], &v->regions[at], sizeof(*v->regions) * (size_t)(v->count - at));
    v->regions[at] = (struct vregion){base, size, protect, protect, bits};
    v->count++;
    return &v->regions[at];
}

static void remove_region(int i)
{
    struct w32_vm *v = w32->vm;
    struct vregion *r = &v->regions[i];
    vmm_release(r->base, r->size);
    kfree(r->committed);
    memmove(&v->regions[i], &v->regions[i + 1], sizeof(*v->regions) * (size_t)(v->count - i - 1));
    v->count--;
}

void w32_vm_cleanup(void)
{
    if (!w32->vm)
        return;
    while (w32->vm->count)
        remove_region(w32->vm->count - 1);
    kfree(w32->vm->regions);
    kfree(w32->vm);
    w32->vm = NULL;
}

static void *WINAPI VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect)
{
    uint32_t a = (uint32_t)addr;
    if (!size || !vm()) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (type & MEM_RESET)
        return addr;
    if ((type & MEM_RESERVE) || !addr) {
        uint32_t base, length;
        if (a) {
            base = a & ~(GRANULARITY - 1);
            length = ALIGN_UP(a + (uint32_t)size, 4096u) - base;
            if (!range_free(base, length)) {
                w32_set_error(ERROR_INVALID_ADDRESS);
                return NULL;
            }
        } else {
            length = ALIGN_UP((uint32_t)size, 4096u);
            base = find_free(length, (type & MEM_TOP_DOWN) != 0);
            if (!base) {
                w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
        }
        struct vregion *r = add_region(base, length, protect);
        if (!r) {
            w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        if (type & MEM_COMMIT)
            mark(r, base, base + length, true);
        return (void *)(uintptr_t)(a ? a : base);
    }
    struct vregion *r = region_at(a);
    if (!r) {
        if (a >= w32->arena_base && a < w32->arena_base + w32->arena_size)
            return addr;
        for (struct w32_image *img = w32->images; img; img = img->next)
            if (a >= img->base && a < img->base + img->size)
                return addr;
        w32_set_error(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    uint32_t start = a & ~4095u, end = ALIGN_UP(a + (uint32_t)size, 4096u);
    if (end > r->base + r->size) {
        w32_set_error(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    mark(r, start, end, true);
    r->protect = protect;
    return (void *)(uintptr_t)start;
}

static BOOL WINAPI VirtualFree(void *addr, SIZE_T size, DWORD type)
{
    uint32_t a = (uint32_t)addr;
    int i = find_index(a);
    if (i < 0) {
        if (a >= w32->arena_base && a < w32->arena_base + w32->arena_size && (type & MEM_RELEASE))
            heap_free_aligned(&w32->heap, addr);
        return TRUE;
    }
    struct vregion *r = &w32->vm->regions[i];
    if (type & MEM_RELEASE) {
        if (a != r->base || size) {
            w32_set_error(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        remove_region(i);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) {
        uint32_t start = a & ~4095u, end = size ? ALIGN_UP(a + (uint32_t)size, 4096u) : r->base + r->size;
        end = MIN(end, r->base + r->size);
        mark(r, start, end, false);
        vmm_release(start, end - start);
        return TRUE;
    }
    w32_set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static BOOL WINAPI VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old)
{
    UNUSED(size);
    struct vregion *r = region_at((uint32_t)addr);
    if (old)
        *old = r ? r->protect : PAGE_EXECUTE_READWRITE;
    if (r)
        r->protect = protect;
    return TRUE;
}

struct mbi {
    uint32_t base, allocation_base, allocation_protect, size, state, protect, type;
};

static uint32_t next_used(uint32_t a)
{
    uint32_t next = VM_HIGH;
    if (w32->arena_base > a)
        next = MIN(next, w32->arena_base);
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (!img->builtin && img->base > a)
            next = MIN(next, img->base);
    struct w32_vm *v = vm();
    for (int i = 0; v && i < v->count; i++)
        if (v->regions[i].base > a)
            next = MIN(next, v->regions[i].base);
    return next;
}

static SIZE_T WINAPI VirtualQuery(const void *addr, struct mbi *info, SIZE_T len)
{
    if (len < sizeof(*info))
        return 0;
    uint32_t a = (uint32_t)addr & ~4095u;
    memset(info, 0, sizeof(*info));
    info->base = a;
    struct vregion *r = region_at(a);
    if (r) {
        bool on = page_committed(r, a);
        uint32_t end = a;
        while (end < r->base + r->size && page_committed(r, end) == on)
            end += 4096u;
        *info = (struct mbi){a, r->base, r->alloc_protect, end - a, on ? MEM_COMMIT : MEM_RESERVE,
                             on ? r->protect : 0, MEM_PRIVATE};
        return sizeof(*info);
    }
    for (struct w32_image *img = w32->images; img; img = img->next)
        if (a >= img->base && a < img->base + img->size) {
            *info = (struct mbi){a, img->base, PAGE_EXECUTE_READWRITE, img->base + img->size - a, MEM_COMMIT,
                                 PAGE_EXECUTE_READWRITE, MEM_IMAGE};
            return sizeof(*info);
        }
    if (a >= w32->arena_base && a < w32->arena_base + w32->arena_size) {
        *info = (struct mbi){a, w32->arena_base, PAGE_READWRITE, w32->arena_base + w32->arena_size - a, MEM_COMMIT,
                             PAGE_READWRITE, MEM_PRIVATE};
        return sizeof(*info);
    }
    if (a >= VM_HIGH && a < 0xc0000000u) {
        *info = (struct mbi){a, a, PAGE_READWRITE, 4096, MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE};
        return sizeof(*info);
    }
    info->size = next_used(a) - a;
    info->state = MEM_FREE;
    info->protect = 1;
    return sizeof(*info);
}

static void *WINAPI VirtualAllocEx(HANDLE process, void *addr, SIZE_T size, DWORD type, DWORD protect)
{
    UNUSED(process);
    return VirtualAlloc(addr, size, type, protect);
}

static BOOL WINAPI VirtualFreeEx(HANDLE process, void *addr, SIZE_T size, DWORD type)
{
    UNUSED(process);
    return VirtualFree(addr, size, type);
}

static BOOL WINAPI VirtualProtectEx(HANDLE process, void *addr, SIZE_T size, DWORD protect, DWORD *old)
{
    UNUSED(process);
    return VirtualProtect(addr, size, protect, old);
}

static SIZE_T WINAPI VirtualQueryEx(HANDLE process, const void *addr, struct mbi *info, SIZE_T len)
{
    UNUSED(process);
    return VirtualQuery(addr, info, len);
}

static const struct w32_export virtual_exports[] = {
    W32_FN(VirtualAlloc),   W32_FN(VirtualFree),   W32_FN(VirtualProtect),   W32_FN(VirtualQuery),
    W32_FN(VirtualAllocEx), W32_FN(VirtualFreeEx), W32_FN(VirtualProtectEx), W32_FN(VirtualQueryEx),
};

W32_EXPORTS(virtual_module, "kernel32.dll", virtual_exports);
