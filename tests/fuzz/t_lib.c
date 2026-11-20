/* the decoders of the kernel library: pictures (PNG, JPEG, GIF, BMP), ZIP, gzip and zlib, Windows shell links */
#include <stdlib.h>

#include <synth/inflate.h>
#include <synth/lnk.h>
#include <synth/picture.h>
#include <synth/zip.h>

#include "targets.h"

void kfree(void *p);

void fuzz_picture(const uint8_t *d, size_t n)
{
    struct picture p = {0};
    if (!picture_decode(d, n, &p))
        picture_free(&p);
    if (!jpeg_decode_fit(d, n, 16, 16, &p))
        picture_free(&p);
}

struct zbuf {
    const uint8_t *d;
    size_t n;
};

static int each(const struct zip_entry *e, void *ctx)
{
    const struct zbuf *z = ctx;
    uint8_t *out = NULL;
    if (zip_extract(z->d, z->n, e, &out) >= 0)
        kfree(out);
    return 0;
}

void fuzz_zip(const uint8_t *d, size_t n)
{
    struct zbuf z = {d, n};
    zip_each(d, n, each, &z);
    uint8_t *out = NULL;
    size_t len;
    if (!gunzip_alloc(d, n, 1u << 24, &out, &len))
        kfree(out);
    out = NULL;
    if (!zlib_inflate_alloc(d, n, 1u << 24, &out, &len))
        kfree(out);
}

void fuzz_lnk(const uint8_t *d, size_t n)
{
    struct lnk l;
    lnk_decode(d, n, &l);
}
