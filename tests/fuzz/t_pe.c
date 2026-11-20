/* PE: the headers, then the image as the loader maps it (headers and sections), its base relocations, its imports
   and its exports, as kernel/win32/pe.c does with pe_parse.h */
#include <stdlib.h>

#include "../../kernel/win32/pe_parse.h"
#include "targets.h"

void fuzz_pe(const uint8_t *d, size_t n)
{
    struct pe_info pe;
    if (!parse_headers(d, n, &pe) || pe.image_size > (64u << 20))
        return;
    uint8_t *mem = calloc(1, pe.image_size);
    if (!mem)
        return;
    memcpy(mem, d, MIN((size_t)pe.headers_size, MIN(n, (size_t)pe.image_size)));
    for (uint16_t i = 0; i < pe.sections; i++) {
        struct pe_section s;
        if (pe_section(&pe, n, n, i, &s))
            memcpy(mem + s.va, d + s.ptr, s.copy);
    }
    pe.file = mem;
    apply_relocations(&pe, mem, 0x10000);
    apply_relocations(&pe, mem, (uint64_t)-0x400000);
    struct pe_import imp = {0};
    while (pe_import_next(&pe, mem, pe.image_size, &imp) > 0) {
        const char *symbol, *fwd;
        uint32_t ordinal, i = 0;
        while (i < 100000 && pe_thunk(mem, pe.image_size, &imp, pe.pe64, i, &symbol, &ordinal) > 0) {
            pe_export(mem, pe.image_size, pe.pe64, symbol, ordinal, &fwd);
            pe_thunk_set(mem, &imp, pe.pe64, i++, 0x1234);
        }
    }
    const char *fwd;
    pe_export(mem, pe.image_size, pe.pe64, "DllMain", 0, &fwd);
    for (uint32_t o = 0; o < 4; o++)
        pe_export(mem, pe.image_size, pe.pe64, NULL, o, &fwd);
    free(mem);
}
