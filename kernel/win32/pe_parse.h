/* The parts of the PE loader that read what a file says: headers, sections, base relocations, imports and exports.
   A file can say anything, so every offset is checked against the buffer it points into before it is used. They
   use nothing of the kernel, so tests/fuzz builds them on the host and feeds them broken files. */
#ifndef SYNTH_PE_PARSE_H
#define SYNTH_PE_PARSE_H

#include <synth/string.h>
#include <synth/types.h>

#define DIR_EXPORT 0
#define DIR_IMPORT 1
#define DIR_RELOC 5
#define DIR_TLS 9

struct pe_info {
    const uint8_t *file;
    size_t file_size;
    uint32_t coff;
    uint32_t opt;
    uint16_t sections;
    uint16_t characteristics;
    uint32_t entry;
    uint64_t image_base;
    bool pe64;
    uint32_t image_size;
    uint32_t headers_size;
    uint32_t dirs;
    uint32_t dir_count;
};

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

/* n bytes at off lie inside size bytes */
static inline bool pe_fits(uint64_t size, uint64_t off, uint64_t n)
{
    return off <= size && n <= size - off;
}

/* the string at off when it ends inside size bytes */
static inline const char *pe_str(const uint8_t *mem, uint32_t size, uint32_t off)
{
    if (off >= size || !memchr(mem + off, 0, size - off))
        return NULL;
    return (const char *)mem + off;
}

static bool parse_headers(const uint8_t *file, size_t size, struct pe_info *pe)
{
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z')
        return false;
    uint32_t lfanew = rd32(file + 0x3c);
    if (!pe_fits(size, lfanew, 24 + 96) || memcmp(file + lfanew, "PE\0\0", 4) != 0)
        return false;
    pe->file = file;
    pe->file_size = size;
    pe->coff = lfanew + 4;
    pe->opt = pe->coff + 20;
    uint16_t machine = rd16(file + pe->coff), magic = rd16(file + pe->opt);
    pe->pe64 = machine == 0x8664 && magic == 0x20b;
    if (!pe->pe64 && (machine != 0x14c || magic != 0x10b))
        return false;
    if (pe->pe64 && !pe_fits(size, lfanew, 24 + 112))
        return false;
    pe->sections = rd16(file + pe->coff + 2);
    pe->characteristics = rd16(file + pe->coff + 18);
    pe->entry = rd32(file + pe->opt + 16);
    pe->image_base = pe->pe64 ? rd64(file + pe->opt + 24) : rd32(file + pe->opt + 28);
    uint32_t image_size = rd32(file + pe->opt + 56);
    if (!image_size || image_size >= 512u * 1024 * 1024)
        return false;
    pe->image_size = ALIGN_UP(image_size, 4096u);
    pe->headers_size = rd32(file + pe->opt + 60);
    pe->dirs = pe->opt + (pe->pe64 ? 112 : 96);
    /* only the directory entries the buffer holds */
    pe->dir_count = MIN(MIN(rd32(file + pe->opt + (pe->pe64 ? 108 : 92)), 16u), (uint32_t)((size - pe->dirs) / 8));
    return true;
}

static void dir_of(const struct pe_info *pe, int index, uint32_t *rva, uint32_t *size)
{
    *rva = *size = 0;
    if ((uint32_t)index < pe->dir_count) {
        *rva = rd32(pe->file + pe->dirs + (uint32_t)index * 8);
        *size = rd32(pe->file + pe->dirs + (uint32_t)index * 8 + 4);
    }
}

/* Section i of a file of file_size bytes, of which the first head_size are in pe->file: where it goes in the image
   and how much of the file to copy there. False for a section with nothing to copy, or past the headers. */
struct pe_section {
    uint32_t va, ptr, copy;
};

static bool pe_section(const struct pe_info *pe, size_t head_size, uint64_t file_size, uint16_t i,
                       struct pe_section *s)
{
    uint64_t sec = (uint64_t)pe->opt + rd16(pe->file + pe->coff + 16) + (uint64_t)i * 40;
    if (i >= pe->sections || !pe_fits(head_size, sec, 40))
        return false;
    const uint8_t *h = pe->file + sec;
    uint32_t vsize = rd32(h + 8), va = rd32(h + 12), raw = rd32(h + 16), ptr = rd32(h + 20);
    uint32_t copy = vsize && vsize < raw ? vsize : raw;
    if (va >= pe->image_size || ptr >= file_size)
        return false;
    s->va = va;
    s->ptr = ptr;
    s->copy = (uint32_t)MIN((uint64_t)copy, MIN((uint64_t)(pe->image_size - va), file_size - ptr));
    return s->copy > 0;
}

/* one fixup of the given type at target, inside an image of size bytes */
static void pe_fixup(uint8_t *mem, uint32_t size, uint32_t target, unsigned type, uint64_t delta)
{
    switch (type) {
    case 3:
        if (pe_fits(size, target, 4)) {
            uint32_t v = rd32(mem + target) + (uint32_t)delta;
            memcpy(mem + target, &v, 4);
        }
        break;
    case 10:
        if (pe_fits(size, target, 8)) {
            uint64_t v = rd64(mem + target) + delta;
            memcpy(mem + target, &v, 8);
        }
        break;
    case 2:
    case 1:
        if (pe_fits(size, target, 2)) {
            uint16_t v = (uint16_t)(rd16(mem + target) + (type == 2 ? (delta & 0xffff) : (delta >> 16 & 0xffff)));
            memcpy(mem + target, &v, 2);
        }
        break;
    default:
        break;
    }
}

/* the base relocations of an image mapped at mem (pe->image_size bytes), moved by delta */
static bool apply_relocations(const struct pe_info *pe, uint8_t *mem, uint64_t delta)
{
    uint32_t rva, size;
    dir_of(pe, DIR_RELOC, &rva, &size);
    if (!delta)
        return true;
    if (pe->characteristics & 0x0001)
        return false;
    if (!rva || !size || !pe_fits(pe->image_size, rva, size))
        return true;
    const uint8_t *rel = mem + rva;
    for (uint32_t off = 0; pe_fits(size, off, 8);) {
        uint32_t page = rd32(rel + off), block = rd32(rel + off + 4);
        if (block < 8 || !pe_fits(size, off, block))
            break;
        for (uint32_t i = 8; i + 2 <= block; i += 2) {
            uint16_t entry = rd16(rel + off + i);
            uint64_t target = (uint64_t)page + (entry & 0xfff);
            if (target < pe->image_size)
                pe_fixup(mem, pe->image_size, (uint32_t)target, entry >> 12, delta);
        }
        off += block;
    }
    return true;
}

/* Import descriptors of an image mapped at mem (size bytes): pe_import_next fills the next one, then pe_thunk reads
   its entries one by one and pe_thunk_set writes the address an entry resolved to. Each returns 1 for one more,
   0 at the end and -1 when the file points outside the image. */
struct pe_import {
    uint32_t next;
    const char *dll;
    uint32_t lookup, iat;
};

static int pe_import_next(const struct pe_info *pe, const uint8_t *mem, uint32_t size, struct pe_import *imp)
{
    if (!imp->next) {
        uint32_t rva, dsize;
        dir_of(pe, DIR_IMPORT, &rva, &dsize);
        if (!rva)
            return 0;
        imp->next = rva;
    }
    if (!pe_fits(size, imp->next, 20))
        return 0;
    const uint8_t *d = mem + imp->next;
    uint32_t lookup = rd32(d), name_rva = rd32(d + 12), iat = rd32(d + 16);
    if (!name_rva && !iat)
        return 0;
    imp->next += 20;
    imp->dll = pe_str(mem, size, name_rva);
    if (!imp->dll || iat >= size)
        return -1;
    imp->lookup = lookup ? lookup : iat;
    imp->iat = iat;
    return 1;
}

static int pe_thunk(const uint8_t *mem, uint32_t size, const struct pe_import *imp, bool pe64, uint32_t i,
                    const char **symbol, uint32_t *ordinal)
{
    uint32_t step = pe64 ? 8 : 4;
    uint64_t at = (uint64_t)imp->lookup + (uint64_t)i * step;
    if (!pe_fits(size, at, step) || !pe_fits(size, (uint64_t)imp->iat + (uint64_t)i * step, step))
        return -1;
    uint64_t entry = pe64 ? rd64(mem + at) : rd32(mem + at);
    if (!entry)
        return 0;
    *symbol = NULL;
    *ordinal = 0;
    if (pe64 ? entry >> 63 : entry >> 31) {
        *ordinal = (uint32_t)entry & 0xffff;
        return 1;
    }
    uint32_t hint = (uint32_t)entry & 0x7fffffffu;
    *symbol = hint < 0xfffffff0u ? pe_str(mem, size, hint + 2) : NULL;
    return *symbol ? 1 : -1;
}

static void pe_thunk_set(uint8_t *mem, const struct pe_import *imp, bool pe64, uint32_t i, uint64_t addr)
{
    memcpy(mem + imp->iat + i * (pe64 ? 8u : 4u), &addr, pe64 ? 8 : 4);
}

/* The export of an image mapped at mem (size bytes) by name, or by ordinal when name is NULL: its RVA, or 0. When
   the export forwards to another DLL ("KERNEL32.Sleep"), *forward is that string and the RVA is not to be used. */
static uint32_t pe_export(const uint8_t *mem, uint32_t size, bool pe64, const char *name, uint32_t ordinal,
                          const char **forward)
{
    *forward = NULL;
    if (size < 0x40)
        return 0;
    uint64_t dirs = (uint64_t)rd32(mem + 0x3c) + 24 + (pe64 ? 112 : 96);
    if (!pe_fits(size, dirs, 8))
        return 0;
    uint32_t rva = rd32(mem + dirs), dsize = rd32(mem + dirs + 4);
    if (!rva || !pe_fits(size, rva, 40))
        return 0;
    const uint8_t *dir = mem + rva;
    uint32_t base = rd32(dir + 16), nfuncs = rd32(dir + 20), nnames = rd32(dir + 24);
    uint32_t funcs = rd32(dir + 28), names = rd32(dir + 32), ordinals = rd32(dir + 36);
    uint32_t index = 0xffffffffu;
    if (name) {
        if (!pe_fits(size, names, (uint64_t)nnames * 4) || !pe_fits(size, ordinals, (uint64_t)nnames * 2))
            return 0;
        for (uint32_t i = 0; i < nnames; i++) {
            const char *n = pe_str(mem, size, rd32(mem + names + i * 4));
            if (n && strcmp(n, name) == 0) {
                index = rd16(mem + ordinals + i * 2);
                break;
            }
        }
    } else if (ordinal >= base) {
        index = ordinal - base;
    }
    if (index >= nfuncs || !pe_fits(size, funcs + (uint64_t)index * 4, 4))
        return 0;
    uint32_t fn = rd32(mem + funcs + index * 4);
    if (!fn || fn >= size)
        return 0;
    if (fn >= rva && fn - rva < dsize) {
        *forward = pe_str(mem, size, fn);
        return *forward ? fn : 0;
    }
    return fn;
}

#endif
