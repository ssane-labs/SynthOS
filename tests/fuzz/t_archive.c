/* the archive module: gzip, xz and lzma streams, and tar listings (names made safe, GNU long names, pax paths) */
#define SYNTH_MODULE
#define synth_module_app fuzz_archive_app
#include "../../kernel/gui/optional/archive.c"
#include "targets.h"

void fuzz_archive(const uint8_t *d, size_t n)
{
    uint8_t *out = NULL;
    size_t len = 0;
    if (!unpack(d, n, &out, &len, "fuzz.lzma")) {
        if (is_tar(out, len)) {
            struct tar_result res = {0};
            untar(NULL, out, len, "/x", true, &res);
        }
        kfree(out);
    }
    if (is_tar(d, n)) {
        struct tar_result res = {0};
        untar(NULL, d, n, "/x", true, &res);
    }
}
