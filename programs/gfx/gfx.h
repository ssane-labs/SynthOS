/* SynthOS graphics: Direct3D 11 and DXGI drawn by the CPU. Built as real Windows DLLs for 32- and 64-bit programs. */
#ifndef GFX_H
#define GFX_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#define GFX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define GFX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define GFX_CLAMP(v, lo, hi) GFX_MIN(GFX_MAX(v, lo), hi)
#define GFX_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CONTAINER(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))

void *gfx_alloc(size_t n);
void *gfx_realloc(void *p, size_t n);
void gfx_free(void *p);
void gfx_log(const char *fmt, ...);

/* math without the C runtime */
float gfx_floor(float x);
float gfx_ceil(float x);
float gfx_trunc(float x);
float gfx_round_even(float x);
float gfx_exp2(float x);
float gfx_log2(float x);
float gfx_sin(float x);
float gfx_cos(float x);
float gfx_pow(float x, float y);
static inline float gfx_sqrt(float x) { return __builtin_sqrtf(x); }
static inline float gfx_abs(float x) { return __builtin_fabsf(x); }
float gfx_half_to_float(uint16_t h);
uint16_t gfx_float_to_half(float f);
float gfx_srgb_to_linear(float c);
float gfx_linear_to_srgb(float c);

/* pixel formats */
struct gfx_format {
    uint8_t bytes;     /* per texel, or per 4x4 block when block is set */
    uint8_t block;     /* BC compressed */
    uint8_t depth;     /* depth/stencil format */
    uint8_t srgb;
};
const struct gfx_format *gfx_format_info(DXGI_FORMAT f);
bool gfx_format_supported(DXGI_FORMAT f);
/* texels move as raw 32-bit words: float bits for float/normalized formats, integers for UINT/SINT ones */
void gfx_texel_load(DXGI_FORMAT f, const uint8_t *p, uint32_t out[4]);
void gfx_texel_store(DXGI_FORMAT f, uint8_t *p, const uint32_t in[4]);
bool gfx_format_is_int(DXGI_FORMAT f);
void gfx_block_decode(DXGI_FORMAT f, const uint8_t *block, int x, int y, uint32_t out[4]);

/* memory of a resource: one or more subresources laid out one after another */
struct gfx_sub {
    uint32_t offset, row_pitch, depth_pitch;
    uint32_t width, height, depth;
};

struct gfx_image {
    DXGI_FORMAT format;
    uint32_t width, height, depth, mips, layers;
    uint32_t subs;
    struct gfx_sub *sub; /* layer * mips + mip */
    uint8_t *data;
    size_t size;
};

bool gfx_image_init(struct gfx_image *img, DXGI_FORMAT format, uint32_t w, uint32_t h, uint32_t d, uint32_t mips,
                    uint32_t layers);
void gfx_image_free(struct gfx_image *img);
uint32_t gfx_mip_count(uint32_t w, uint32_t h, uint32_t d);

#endif
