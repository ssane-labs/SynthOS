/* the parsers tests/fuzz feeds: each takes one whole file */
#include <stddef.h>
#include <stdint.h>

struct target {
    const char *name;
    void (*fn)(const uint8_t *d, size_t n);
};

void fuzz_pe(const uint8_t *d, size_t n);
void fuzz_picture(const uint8_t *d, size_t n);
void fuzz_codecs(const uint8_t *d, size_t n);
void fuzz_media(const uint8_t *d, size_t n);
void fuzz_doc(const uint8_t *d, size_t n);
void fuzz_archive(const uint8_t *d, size_t n);
void fuzz_zip(const uint8_t *d, size_t n);
void fuzz_lnk(const uint8_t *d, size_t n);

static const struct target targets[] = {
    {"pe", fuzz_pe},         {"picture", fuzz_picture}, {"codecs", fuzz_codecs}, {"media", fuzz_media},
    {"doc", fuzz_doc},       {"archive", fuzz_archive}, {"zip", fuzz_zip},       {"lnk", fuzz_lnk},
    {NULL, NULL},
};
