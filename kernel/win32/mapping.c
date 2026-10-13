#include <synth/error.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/vmm.h>

#include "win32.h"

#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE_READWRITE 0x40u
#define FILE_MAP_WRITE 0x0002u
#define FILE_MAP_COPY 0x0001u

struct w32_section {
    int refs;
    char *path;
    uint8_t **pages;
    uint32_t page_count;
    uint32_t size;
    bool writable;
};

struct view {
    uint8_t *base;
    uint32_t offset, length;
    bool write_back, owned, shared;
    struct w32_section *section;
};

#define MAX_VIEWS 256

static W32_STATE struct view views[MAX_VIEWS];

void w32_section_retain(struct w32_section *s)
{
    s->refs++;
}

void w32_section_release(struct w32_section *s)
{
    if (--s->refs > 0)
        return;
    kfree(s->path);
    for (uint32_t i = 0; s->pages && i < s->page_count; i++)
        kfree_pages(s->pages[i]);
    kfree(s->pages);
    kfree(s);
}

static HANDLE create_mapping(HANDLE file, DWORD protect, DWORD size_hi, DWORD size_lo)
{
    if (size_hi) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    struct w32_section *s = kzalloc(sizeof(*s));
    if (!s) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    s->refs = 1;
    s->writable = (protect & 0xff) == PAGE_READWRITE || (protect & 0xff) == PAGE_EXECUTE_READWRITE;
    if (file == INVALID_HANDLE_VALUE || !file) {
        s->size = size_lo;
        s->page_count = (size_lo + 4095u) / 4096u;
        s->pages = s->page_count ? kzalloc(s->page_count * sizeof(*s->pages)) : NULL;
        for (uint32_t i = 0; s->pages && i < s->page_count; i++)
            if (!(s->pages[i] = kmalloc_pages(4096))) {
                s->refs = 1;
                w32_section_release(s);
                s = NULL;
                break;
            }
        if (!s || !s->pages) {
            if (s)
                kfree(s);
            w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    } else {
        struct w32_object *f = w32_handle_get(file);
        if (!f || f->type != WH_FILE || !f->path) {
            kfree(s);
            w32_set_error(ERROR_INVALID_HANDLE);
            return NULL;
        }
        struct vstat st;
        if (vfs_fstat(f->file, &st) != 0) {
            kfree(s);
            w32_set_error(ERROR_INVALID_HANDLE);
            return NULL;
        }
        s->size = size_lo ? size_lo : st.size;
        if (!s->size) {
            kfree(s);
            w32_set_error(1006);
            return NULL;
        }
        if (s->writable && size_lo > st.size)
            vfs_truncate(f->path, size_lo);
        s->path = kstrdup(f->path);
    }
    HANDLE h = w32_handle_new(WH_SECTION);
    struct w32_object *o = w32_handle_get(h);
    if (!o) {
        w32_section_release(s);
        return NULL;
    }
    o->section = s;
    return h;
}

static HANDLE WINAPI CreateFileMappingA(HANDLE file, void *sa, DWORD protect, DWORD size_hi, DWORD size_lo,
                                        LPCSTR name)
{
    UNUSED(name);
    return w32_inheritable(create_mapping(file, protect, size_hi, size_lo), sa);
}

static HANDLE WINAPI CreateFileMappingW(HANDLE file, void *sa, DWORD protect, DWORD size_hi, DWORD size_lo,
                                        LPCWSTR name)
{
    UNUSED(name);
    return w32_inheritable(create_mapping(file, protect, size_hi, size_lo), sa);
}

static void *map_view(HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T bytes)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_SECTION || !o->section || off_hi || off_lo > o->section->size) {
        w32_set_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    struct w32_section *s = o->section;
    if (o->access && (access & FILE_MAP_WRITE) && !(o->access & FILE_MAP_WRITE)) {
        w32_set_error(ERROR_ACCESS_DENIED);
        return NULL;
    }
    uint32_t length = bytes ? (uint32_t)bytes : s->size - off_lo;
    if (off_lo + length > s->size) {
        w32_set_error(ERROR_ACCESS_DENIED);
        return NULL;
    }
    struct view *v = NULL;
    for (int i = 0; i < MAX_VIEWS && !v; i++)
        if (!views[i].base)
            v = &views[i];
    if (!v) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (s->pages) {
        uint32_t first = off_lo / 4096u, count = (off_lo % 4096u + length + 4095u) / 4096u;
        uint32_t base = w32_vm_map_shared(s->pages + first, count);
        if (!base) {
            w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        *v = (struct view){(uint8_t *)(uintptr_t)base + off_lo % 4096u, off_lo, length, false, false, true, s};
        s->refs++;
        return v->base;
    }
    uint8_t *mem = w32_alloc(MAX(length, 1u));
    if (!mem) {
        w32_set_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    struct file *f;
    if (vfs_open(s->path, VFS_READ, &f) != 0) {
        w32_free(mem);
        w32_set_error(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    vfs_seek(f, (long)off_lo, VFS_SEEK_SET);
    long got = vfs_read(f, mem, length);
    vfs_close(f);
    if (got < 0)
        got = 0;
    if ((uint32_t)got < length)
        memset(mem + got, 0, length - (uint32_t)got);
    bool write_back = s->writable && (access & FILE_MAP_WRITE) && !(access & FILE_MAP_COPY);
    *v = (struct view){mem, off_lo, length, write_back, true, false, s};
    s->refs++;
    return mem;
}

static void *WINAPI MapViewOfFile(HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T bytes)
{
    return map_view(h, access, off_hi, off_lo, bytes);
}

static void *WINAPI MapViewOfFileEx(HANDLE h, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T bytes, void *base)
{
    UNUSED(base);
    return map_view(h, access, off_hi, off_lo, bytes);
}

static struct view *find_view(const void *p)
{
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i].base && (const uint8_t *)p >= views[i].base &&
            (const uint8_t *)p < views[i].base + MAX(views[i].length, 1u))
            return &views[i];
    return NULL;
}

static void flush(struct view *v)
{
    if (!v->write_back || !v->section->path)
        return;
    struct file *f;
    if (vfs_open(v->section->path, VFS_RDWR, &f) != 0)
        return;
    vfs_seek(f, (long)v->offset, VFS_SEEK_SET);
    vfs_write(f, v->base, v->length);
    vfs_close(f);
}

static BOOL WINAPI FlushViewOfFile(const void *p, SIZE_T n)
{
    UNUSED(n);
    struct view *v = find_view(p);
    if (v)
        flush(v);
    return TRUE;
}

static BOOL WINAPI UnmapViewOfFile(const void *p)
{
    struct view *v = find_view(p);
    if (!v) {
        w32_set_error(487);
        return FALSE;
    }
    flush(v);
    if (v->owned)
        w32_free(v->base);
    if (v->shared)
        w32_vm_unmap_shared((uint32_t)(uintptr_t)v->base & ~4095u);
    w32_section_release(v->section);
    memset(v, 0, sizeof(*v));
    return TRUE;
}

static DWORD WINAPI NtQuerySection(HANDLE h, int cls, uint32_t *info, ULONG len, ULONG *ret)
{
    struct w32_object *o = w32_handle_get(h);
    if (!o || o->type != WH_SECTION || !o->section)
        return 0xc0000008u;
    if (cls != 0)
        return 0xc0000003u;
    if (len < 16)
        return 0xc0000004u;
    info[0] = 0;
    info[1] = 0x08000000u;
    info[2] = o->section->size;
    info[3] = 0;
    if (ret)
        *ret = 16;
    return 0;
}

static const struct w32_export mapping_ntdll[] = {
    W32_FN(NtQuerySection),
};

static const struct w32_export mapping_exports[] = {
    W32_FN(CreateFileMappingA), W32_FN(CreateFileMappingW), W32_FN(MapViewOfFile),
    W32_FN(MapViewOfFileEx),    W32_FN(UnmapViewOfFile),    W32_FN(FlushViewOfFile),
};

W32_EXPORTS(mapping_module, "kernel32.dll", mapping_exports);
W32_EXPORTS(mapping_ntdll_module, "ntdll.dll", mapping_ntdll);
