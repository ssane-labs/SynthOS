#ifndef SYNTH_INFLATE_H
#define SYNTH_INFLATE_H

#include <synth/types.h>

int inflate_into(const void *in, size_t in_len, void *out, size_t out_cap, size_t *out_len, size_t *used);
int inflate_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len, size_t *used);
int gunzip_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len);
int zlib_inflate_alloc(const void *in, size_t in_len, size_t limit, uint8_t **out, size_t *out_len);
int deflate_alloc(const void *in, size_t n, uint8_t **out, size_t *out_len, size_t reserve);
int zlib_deflate_alloc(const void *in, size_t n, uint8_t **out, size_t *out_len);

struct gunzip_stream;
struct gunzip_stream *gunzip_stream_new(int (*out)(void *ctx, const void *data, size_t n), void *ctx);
struct gunzip_stream *inflate_stream_new(int (*out)(void *ctx, const void *data, size_t n), void *ctx);
int gunzip_stream_push(struct gunzip_stream *z, const void *data, size_t n);
int gunzip_stream_end(struct gunzip_stream *z);
void gunzip_stream_free(struct gunzip_stream *z);

#endif
