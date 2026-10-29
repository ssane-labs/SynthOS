#include "gfx.h"

enum { K_NONE, K_RAW, K_HALF, K_UNORM, K_SNORM, K_UINT, K_SINT, K_SRGB, K_SPECIAL };
enum { O_RGBA, O_BGRA, O_ALPHA };
enum {
    S_NONE, S_RGB10A2, S_RGB10A2_UINT, S_R11G11B10, S_RGB9E5, S_B5G6R5, S_B5G5R5A1, S_B4G4R4A4, S_D24S8, S_R24X8,
    S_X24G8, S_D32S8, S_R32X32, S_X32G8, S_D16, S_BGRX, S_BGRX_SRGB
};

struct entry {
    struct gfx_format info;
    uint8_t chans, bits, kind, order, special, is_int;
};

#define E(bytes, chans, bits, kind, order, is_int) {{bytes, 0, 0, kind == K_SRGB}, chans, bits, kind, order, 0, is_int}
#define SP(bytes, sp, depth, is_int) {{bytes, 0, depth, 0}, 0, 0, K_SPECIAL, 0, sp, is_int}
#define BC(bytes, srgb) {{bytes, 1, 0, srgb}, 0, 0, K_NONE, 0, 0, 0}

static const struct entry formats[] = {
    [DXGI_FORMAT_R32G32B32A32_TYPELESS] = E(16, 4, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32G32B32A32_FLOAT] = E(16, 4, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32G32B32A32_UINT] = E(16, 4, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R32G32B32A32_SINT] = E(16, 4, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R32G32B32_TYPELESS] = E(12, 3, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32G32B32_FLOAT] = E(12, 3, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32G32B32_UINT] = E(12, 3, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R32G32B32_SINT] = E(12, 3, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R16G16B16A16_TYPELESS] = E(8, 4, 16, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R16G16B16A16_FLOAT] = E(8, 4, 16, K_HALF, O_RGBA, 0),
    [DXGI_FORMAT_R16G16B16A16_UNORM] = E(8, 4, 16, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16G16B16A16_UINT] = E(8, 4, 16, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R16G16B16A16_SNORM] = E(8, 4, 16, K_SNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16G16B16A16_SINT] = E(8, 4, 16, K_SINT, O_RGBA, 1),
    [DXGI_FORMAT_R32G32_TYPELESS] = E(8, 2, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32G32_FLOAT] = E(8, 2, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32G32_UINT] = E(8, 2, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R32G32_SINT] = E(8, 2, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R32G8X24_TYPELESS] = SP(8, S_D32S8, 0, 0),
    [DXGI_FORMAT_D32_FLOAT_S8X24_UINT] = SP(8, S_D32S8, 1, 0),
    [DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS] = SP(8, S_R32X32, 0, 0),
    [DXGI_FORMAT_X32_TYPELESS_G8X24_UINT] = SP(8, S_X32G8, 0, 1),
    [DXGI_FORMAT_R10G10B10A2_TYPELESS] = SP(4, S_RGB10A2, 0, 0),
    [DXGI_FORMAT_R10G10B10A2_UNORM] = SP(4, S_RGB10A2, 0, 0),
    [DXGI_FORMAT_R10G10B10A2_UINT] = SP(4, S_RGB10A2_UINT, 0, 1),
    [DXGI_FORMAT_R11G11B10_FLOAT] = SP(4, S_R11G11B10, 0, 0),
    [DXGI_FORMAT_R8G8B8A8_TYPELESS] = E(4, 4, 8, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8G8B8A8_UNORM] = E(4, 4, 8, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8G8B8A8_UNORM_SRGB] = E(4, 4, 8, K_SRGB, O_RGBA, 0),
    [DXGI_FORMAT_R8G8B8A8_UINT] = E(4, 4, 8, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R8G8B8A8_SNORM] = E(4, 4, 8, K_SNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8G8B8A8_SINT] = E(4, 4, 8, K_SINT, O_RGBA, 1),
    [DXGI_FORMAT_R16G16_TYPELESS] = E(4, 2, 16, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R16G16_FLOAT] = E(4, 2, 16, K_HALF, O_RGBA, 0),
    [DXGI_FORMAT_R16G16_UNORM] = E(4, 2, 16, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16G16_UINT] = E(4, 2, 16, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R16G16_SNORM] = E(4, 2, 16, K_SNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16G16_SINT] = E(4, 2, 16, K_SINT, O_RGBA, 1),
    [DXGI_FORMAT_R32_TYPELESS] = E(4, 1, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_D32_FLOAT] = {{4, 0, 1, 0}, 1, 32, K_RAW, O_RGBA, 0, 0},
    [DXGI_FORMAT_R32_FLOAT] = E(4, 1, 32, K_RAW, O_RGBA, 0),
    [DXGI_FORMAT_R32_UINT] = E(4, 1, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R32_SINT] = E(4, 1, 32, K_RAW, O_RGBA, 1),
    [DXGI_FORMAT_R24G8_TYPELESS] = SP(4, S_D24S8, 0, 0),
    [DXGI_FORMAT_D24_UNORM_S8_UINT] = SP(4, S_D24S8, 1, 0),
    [DXGI_FORMAT_R24_UNORM_X8_TYPELESS] = SP(4, S_R24X8, 0, 0),
    [DXGI_FORMAT_X24_TYPELESS_G8_UINT] = SP(4, S_X24G8, 0, 1),
    [DXGI_FORMAT_R8G8_TYPELESS] = E(2, 2, 8, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8G8_UNORM] = E(2, 2, 8, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8G8_UINT] = E(2, 2, 8, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R8G8_SNORM] = E(2, 2, 8, K_SNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8G8_SINT] = E(2, 2, 8, K_SINT, O_RGBA, 1),
    [DXGI_FORMAT_R16_TYPELESS] = E(2, 1, 16, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16_FLOAT] = E(2, 1, 16, K_HALF, O_RGBA, 0),
    [DXGI_FORMAT_D16_UNORM] = {{2, 0, 1, 0}, 1, 16, K_UNORM, O_RGBA, 0, 0},
    [DXGI_FORMAT_R16_UNORM] = E(2, 1, 16, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16_UINT] = E(2, 1, 16, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R16_SNORM] = E(2, 1, 16, K_SNORM, O_RGBA, 0),
    [DXGI_FORMAT_R16_SINT] = E(2, 1, 16, K_SINT, O_RGBA, 1),
    [DXGI_FORMAT_R8_TYPELESS] = E(1, 1, 8, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8_UNORM] = E(1, 1, 8, K_UNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8_UINT] = E(1, 1, 8, K_UINT, O_RGBA, 1),
    [DXGI_FORMAT_R8_SNORM] = E(1, 1, 8, K_SNORM, O_RGBA, 0),
    [DXGI_FORMAT_R8_SINT] = E(1, 1, 8, K_SINT, O_RGBA, 1),
    [DXGI_FORMAT_A8_UNORM] = E(1, 1, 8, K_UNORM, O_ALPHA, 0),
    [DXGI_FORMAT_R9G9B9E5_SHAREDEXP] = SP(4, S_RGB9E5, 0, 0),
    [DXGI_FORMAT_BC1_TYPELESS] = BC(8, 0),
    [DXGI_FORMAT_BC1_UNORM] = BC(8, 0),
    [DXGI_FORMAT_BC1_UNORM_SRGB] = BC(8, 1),
    [DXGI_FORMAT_BC2_TYPELESS] = BC(16, 0),
    [DXGI_FORMAT_BC2_UNORM] = BC(16, 0),
    [DXGI_FORMAT_BC2_UNORM_SRGB] = BC(16, 1),
    [DXGI_FORMAT_BC3_TYPELESS] = BC(16, 0),
    [DXGI_FORMAT_BC3_UNORM] = BC(16, 0),
    [DXGI_FORMAT_BC3_UNORM_SRGB] = BC(16, 1),
    [DXGI_FORMAT_BC4_TYPELESS] = BC(8, 0),
    [DXGI_FORMAT_BC4_UNORM] = BC(8, 0),
    [DXGI_FORMAT_BC4_SNORM] = BC(8, 0),
    [DXGI_FORMAT_BC5_TYPELESS] = BC(16, 0),
    [DXGI_FORMAT_BC5_UNORM] = BC(16, 0),
    [DXGI_FORMAT_BC5_SNORM] = BC(16, 0),
    [DXGI_FORMAT_B5G6R5_UNORM] = SP(2, S_B5G6R5, 0, 0),
    [DXGI_FORMAT_B5G5R5A1_UNORM] = SP(2, S_B5G5R5A1, 0, 0),
    [DXGI_FORMAT_B8G8R8A8_UNORM] = E(4, 4, 8, K_UNORM, O_BGRA, 0),
    [DXGI_FORMAT_B8G8R8X8_UNORM] = SP(4, S_BGRX, 0, 0),
    [DXGI_FORMAT_B8G8R8A8_TYPELESS] = E(4, 4, 8, K_UNORM, O_BGRA, 0),
    [DXGI_FORMAT_B8G8R8A8_UNORM_SRGB] = E(4, 4, 8, K_SRGB, O_BGRA, 0),
    [DXGI_FORMAT_B8G8R8X8_TYPELESS] = SP(4, S_BGRX, 0, 0),
    [DXGI_FORMAT_B8G8R8X8_UNORM_SRGB] = {{4, 0, 0, 1}, 0, 0, K_SPECIAL, 0, S_BGRX_SRGB, 0},
    [DXGI_FORMAT_BC6H_TYPELESS] = BC(16, 0),
    [DXGI_FORMAT_BC6H_UF16] = BC(16, 0),
    [DXGI_FORMAT_BC6H_SF16] = BC(16, 0),
    [DXGI_FORMAT_BC7_TYPELESS] = BC(16, 0),
    [DXGI_FORMAT_BC7_UNORM] = BC(16, 0),
    [DXGI_FORMAT_BC7_UNORM_SRGB] = BC(16, 1),
    [DXGI_FORMAT_B4G4R4A4_UNORM] = SP(2, S_B4G4R4A4, 0, 0),
};

static const struct entry *entry_of(DXGI_FORMAT f)
{
    if ((unsigned)f >= GFX_COUNT(formats) || !formats[f].info.bytes)
        return NULL;
    return &formats[f];
}

const struct gfx_format *gfx_format_info(DXGI_FORMAT f)
{
    const struct entry *e = entry_of(f);
    return e ? &e->info : NULL;
}

bool gfx_format_supported(DXGI_FORMAT f)
{
    return entry_of(f) != NULL;
}

bool gfx_format_is_int(DXGI_FORMAT f)
{
    const struct entry *e = entry_of(f);
    return e && e->is_int;
}

static inline uint32_t fu(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float uf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline float sat(float v)
{
    return v != v ? 0.0f : GFX_CLAMP(v, 0.0f, 1.0f);
}

static inline uint32_t to_unorm(float v, uint32_t max)
{
    return (uint32_t)(sat(v) * (float)max + 0.5f);
}

static inline uint32_t to_snorm(float v, uint32_t bits)
{
    float max = (float)((1u << (bits - 1)) - 1);
    v = v != v ? 0.0f : GFX_CLAMP(v, -1.0f, 1.0f) * max;
    int32_t i = (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
    return (uint32_t)i & ((bits == 32) ? 0xffffffffu : ((1u << bits) - 1));
}

static float from_snorm(uint32_t v, uint32_t bits)
{
    int32_t i = (int32_t)(v << (32 - bits)) >> (32 - bits);
    float f = (float)i / (float)((1u << (bits - 1)) - 1);
    return f < -1.0f ? -1.0f : f;
}

static float small_float(uint32_t v, int mbits)
{
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0)
        return (float)m / (float)(1u << mbits) * (1.0f / 16384.0f);
    if (e == 31)
        return m ? uf(0x7fc00000) : uf(0x7f800000);
    return uf(((e + 112) << 23) | (m << (23 - mbits)));
}

static uint32_t to_small_float(float f, int mbits)
{
    if (!(f > 0.0f))
        return 0;
    uint16_t h = gfx_float_to_half(f);
    return (uint32_t)(h >> (10 - mbits)) & ((1u << (mbits + 5)) - 1);
}

static uint32_t rd(const uint8_t *p, int bytes)
{
    uint32_t v = 0;
    memcpy(&v, p, (size_t)bytes);
    return v;
}

static void wr(uint8_t *p, uint32_t v, int bytes)
{
    memcpy(p, &v, (size_t)bytes);
}

static void load_special(const struct entry *e, const uint8_t *p, uint32_t o[4])
{
    uint32_t v = rd(p, e->info.bytes > 4 ? 4 : e->info.bytes);
    float f[4] = {0, 0, 0, 1};
    switch (e->special) {
    case S_RGB10A2:
        f[0] = (float)(v & 1023) / 1023.0f;
        f[1] = (float)((v >> 10) & 1023) / 1023.0f;
        f[2] = (float)((v >> 20) & 1023) / 1023.0f;
        f[3] = (float)(v >> 30) / 3.0f;
        break;
    case S_RGB10A2_UINT:
        o[0] = v & 1023;
        o[1] = (v >> 10) & 1023;
        o[2] = (v >> 20) & 1023;
        o[3] = v >> 30;
        return;
    case S_R11G11B10:
        f[0] = small_float(v & 0x7ff, 6);
        f[1] = small_float((v >> 11) & 0x7ff, 6);
        f[2] = small_float(v >> 22, 5);
        break;
    case S_RGB9E5: {
        float scale = gfx_exp2((float)((int)(v >> 27) - 15 - 9));
        f[0] = (float)(v & 511) * scale;
        f[1] = (float)((v >> 9) & 511) * scale;
        f[2] = (float)((v >> 18) & 511) * scale;
        break;
    }
    case S_B5G6R5:
        f[0] = (float)((v >> 11) & 31) / 31.0f;
        f[1] = (float)((v >> 5) & 63) / 63.0f;
        f[2] = (float)(v & 31) / 31.0f;
        break;
    case S_B5G5R5A1:
        f[0] = (float)((v >> 10) & 31) / 31.0f;
        f[1] = (float)((v >> 5) & 31) / 31.0f;
        f[2] = (float)(v & 31) / 31.0f;
        f[3] = (float)(v >> 15);
        break;
    case S_B4G4R4A4:
        f[0] = (float)((v >> 8) & 15) / 15.0f;
        f[1] = (float)((v >> 4) & 15) / 15.0f;
        f[2] = (float)(v & 15) / 15.0f;
        f[3] = (float)((v >> 12) & 15) / 15.0f;
        break;
    case S_D24S8:
    case S_R24X8:
        f[0] = (float)(v & 0xffffff) / 16777215.0f;
        if (e->special == S_D24S8)
            f[1] = (float)(v >> 24);
        break;
    case S_X24G8:
        o[0] = 0;
        o[1] = v >> 24;
        o[2] = 0;
        o[3] = 1;
        return;
    case S_D32S8:
    case S_R32X32:
        f[0] = uf(v);
        if (e->special == S_D32S8)
            f[1] = (float)p[4];
        break;
    case S_X32G8:
        o[0] = 0;
        o[1] = p[4];
        o[2] = 0;
        o[3] = 1;
        return;
    case S_BGRX:
    case S_BGRX_SRGB:
        for (int i = 0; i < 3; i++) {
            f[i] = (float)((v >> (16 - 8 * i)) & 0xff) / 255.0f;
            if (e->special == S_BGRX_SRGB)
                f[i] = gfx_srgb_to_linear(f[i]);
        }
        break;
    }
    for (int i = 0; i < 4; i++)
        o[i] = fu(f[i]);
}

static float srgb8[256];

void gfx_texel_load(DXGI_FORMAT format, const uint8_t *p, uint32_t o[4])
{
    const struct entry *e = entry_of(format);
    o[0] = o[1] = o[2] = 0;
    o[3] = e && e->is_int ? 1 : 0x3f800000;
    if (!e)
        return;
    if (e->kind == K_SPECIAL) {
        load_special(e, p, o);
        return;
    }
    if (e->info.block)
        return;
    uint32_t c[4] = {0, 0, 0, 0};
    int bytes = e->bits / 8;
    for (int i = 0; i < e->chans; i++)
        c[i] = rd(p + i * bytes, bytes);
    uint32_t max = e->bits == 32 ? 0xffffffffu : (1u << e->bits) - 1;
    for (int i = 0; i < e->chans; i++) {
        int dst = e->order == O_ALPHA ? 3 : (e->order == O_BGRA && i < 3) ? 2 - i : i;
        uint32_t v = c[i];
        switch (e->kind) {
        case K_RAW: o[dst] = v; break;
        case K_HALF: o[dst] = fu(gfx_half_to_float((uint16_t)v)); break;
        case K_UNORM: o[dst] = fu((float)v / (float)max); break;
        case K_SNORM: o[dst] = fu(from_snorm(v, e->bits)); break;
        case K_UINT: o[dst] = v; break;
        case K_SINT: o[dst] = (uint32_t)((int32_t)(v << (32 - e->bits)) >> (32 - e->bits)); break;
        case K_SRGB:
            if (dst == 3) {
                o[dst] = fu((float)v / 255.0f);
            } else {
                if (srgb8[255] == 0.0f)
                    for (int k = 0; k < 256; k++)
                        srgb8[k] = gfx_srgb_to_linear((float)k / 255.0f);
                o[dst] = fu(srgb8[v & 255]);
            }
            break;
        }
    }
}

static void store_special(const struct entry *e, uint8_t *p, const uint32_t in[4])
{
    float f[4] = {uf(in[0]), uf(in[1]), uf(in[2]), uf(in[3])};
    uint32_t v = 0;
    switch (e->special) {
    case S_RGB10A2:
        v = to_unorm(f[0], 1023) | to_unorm(f[1], 1023) << 10 | to_unorm(f[2], 1023) << 20 | to_unorm(f[3], 3) << 30;
        break;
    case S_RGB10A2_UINT:
        v = GFX_MIN(in[0], 1023u) | GFX_MIN(in[1], 1023u) << 10 | GFX_MIN(in[2], 1023u) << 20 | GFX_MIN(in[3], 3u) << 30;
        break;
    case S_R11G11B10:
        v = to_small_float(f[0], 6) | to_small_float(f[1], 6) << 11 | to_small_float(f[2], 5) << 22;
        break;
    case S_RGB9E5: {
        float m = GFX_MAX(GFX_MAX(f[0], f[1]), f[2]);
        m = GFX_CLAMP(m, 0.0f, 65408.0f);
        int e2 = m > 0 ? (int)gfx_floor(gfx_log2(m)) + 1 : -15;
        e2 = GFX_CLAMP(e2, -15, 16);
        float scale = gfx_exp2((float)(9 - e2));
        for (int i = 0; i < 3; i++)
            v |= (uint32_t)GFX_CLAMP((int)(GFX_MAX(f[i], 0.0f) * scale + 0.5f), 0, 511) << (9 * i);
        v |= (uint32_t)(e2 + 15) << 27;
        break;
    }
    case S_B5G6R5: v = to_unorm(f[2], 31) | to_unorm(f[1], 63) << 5 | to_unorm(f[0], 31) << 11; break;
    case S_B5G5R5A1:
        v = to_unorm(f[2], 31) | to_unorm(f[1], 31) << 5 | to_unorm(f[0], 31) << 10 | to_unorm(f[3], 1) << 15;
        break;
    case S_B4G4R4A4:
        v = to_unorm(f[2], 15) | to_unorm(f[1], 15) << 4 | to_unorm(f[0], 15) << 8 | to_unorm(f[3], 15) << 12;
        break;
    case S_D24S8:
    case S_R24X8:
        v = (rd(p, 4) & 0xff000000u) | to_unorm(f[0], 0xffffff);
        break;
    case S_X24G8: v = (rd(p, 4) & 0xffffff) | (in[1] & 255) << 24; break;
    case S_D32S8:
    case S_R32X32: v = in[0]; break;
    case S_X32G8:
        p[4] = (uint8_t)in[1];
        return;
    case S_BGRX:
    case S_BGRX_SRGB:
        for (int i = 0; i < 3; i++)
            v |= to_unorm(e->special == S_BGRX_SRGB ? gfx_linear_to_srgb(f[i]) : f[i], 255) << (16 - 8 * i);
        v |= 0xff000000u;
        break;
    }
    wr(p, v, e->info.bytes > 4 ? 4 : e->info.bytes);
}

void gfx_texel_store(DXGI_FORMAT format, uint8_t *p, const uint32_t in[4])
{
    const struct entry *e = entry_of(format);
    if (!e || e->info.block)
        return;
    if (e->kind == K_SPECIAL) {
        store_special(e, p, in);
        return;
    }
    int bytes = e->bits / 8;
    uint32_t max = e->bits == 32 ? 0xffffffffu : (1u << e->bits) - 1;
    for (int i = 0; i < e->chans; i++) {
        int src = e->order == O_ALPHA ? 3 : (e->order == O_BGRA && i < 3) ? 2 - i : i;
        uint32_t v = in[src];
        float f = uf(v);
        switch (e->kind) {
        case K_RAW: break;
        case K_HALF: v = gfx_float_to_half(f); break;
        case K_UNORM: v = to_unorm(f, max); break;
        case K_SNORM: v = to_snorm(f, e->bits); break;
        case K_UINT: v = GFX_MIN(v, max); break;
        case K_SINT: {
            int32_t lo = -(int32_t)(max >> 1) - 1, hi = (int32_t)(max >> 1), s = (int32_t)v;
            v = (uint32_t)GFX_CLAMP(s, lo, hi) & max;
            break;
        }
        case K_SRGB: v = to_unorm(src == 3 ? f : gfx_linear_to_srgb(f), 255); break;
        }
        wr(p + i * bytes, v, bytes);
    }
}

/* BC1-BC5 (S3TC and RGTC) */
static void color_block(const uint8_t *b, int x, int y, bool bc1, float out[4])
{
    uint32_t c0 = rd(b, 2), c1 = rd(b + 2, 2), bits = rd(b + 4, 4);
    float c[4][4];
    for (int i = 0; i < 2; i++) {
        uint32_t v = i ? c1 : c0;
        c[i][0] = (float)((v >> 11) & 31) / 31.0f;
        c[i][1] = (float)((v >> 5) & 63) / 63.0f;
        c[i][2] = (float)(v & 31) / 31.0f;
        c[i][3] = 1.0f;
    }
    for (int k = 0; k < 4; k++) {
        if (c0 > c1 || !bc1) {
            c[2][k] = (2 * c[0][k] + c[1][k]) / 3.0f;
            c[3][k] = (c[0][k] + 2 * c[1][k]) / 3.0f;
        } else {
            c[2][k] = (c[0][k] + c[1][k]) * 0.5f;
            c[3][k] = 0.0f;
        }
    }
    if (!(c0 > c1 || !bc1))
        c[2][3] = 1.0f;
    int idx = (int)((bits >> (2 * (y * 4 + x))) & 3);
    for (int k = 0; k < 4; k++)
        out[k] = c[idx][k];
}

static float alpha_block(const uint8_t *b, int x, int y, bool snorm)
{
    float a[8];
    float a0 = snorm ? GFX_MAX((float)(int8_t)b[0] / 127.0f, -1.0f) : (float)b[0] / 255.0f;
    float a1 = snorm ? GFX_MAX((float)(int8_t)b[1] / 127.0f, -1.0f) : (float)b[1] / 255.0f;
    bool six = snorm ? (int8_t)b[0] > (int8_t)b[1] : b[0] > b[1];
    a[0] = a0;
    a[1] = a1;
    if (six) {
        for (int i = 1; i < 7; i++)
            a[i + 1] = ((7 - i) * a0 + i * a1) / 7.0f;
    } else {
        for (int i = 1; i < 5; i++)
            a[i + 1] = ((5 - i) * a0 + i * a1) / 5.0f;
        a[6] = snorm ? -1.0f : 0.0f;
        a[7] = 1.0f;
    }
    uint64_t bits = 0;
    memcpy(&bits, b + 2, 6);
    return a[(bits >> (3 * (y * 4 + x))) & 7];
}

void gfx_block_decode(DXGI_FORMAT f, const uint8_t *b, int x, int y, uint32_t o[4])
{
    float c[4] = {0, 0, 0, 1};
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB: color_block(b, x, y, true, c); break;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        color_block(b + 8, x, y, false, c);
        c[3] = (float)((b[(y * 4 + x) / 2] >> (((y * 4 + x) & 1) * 4)) & 15) / 15.0f;
        break;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        color_block(b + 8, x, y, false, c);
        c[3] = alpha_block(b, x, y, false);
        break;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM: c[0] = alpha_block(b, x, y, f == DXGI_FORMAT_BC4_SNORM); break;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        c[0] = alpha_block(b, x, y, f == DXGI_FORMAT_BC5_SNORM);
        c[1] = alpha_block(b + 8, x, y, f == DXGI_FORMAT_BC5_SNORM);
        break;
    default: {
        extern bool gfx_bc67_decode(DXGI_FORMAT f, const uint8_t *b, int x, int y, float out[4]);
        if (!gfx_bc67_decode(f, b, x, y, c))
            c[0] = c[1] = c[2] = 1.0f;
        break;
    }
    }
    if (gfx_format_info(f) && gfx_format_info(f)->srgb)
        for (int i = 0; i < 3; i++)
            c[i] = gfx_srgb_to_linear(c[i]);
    for (int i = 0; i < 4; i++)
        o[i] = fu(c[i]);
}

uint32_t gfx_mip_count(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t m = GFX_MAX(GFX_MAX(w, h), d), n = 1;
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

bool gfx_image_init(struct gfx_image *img, DXGI_FORMAT format, uint32_t w, uint32_t h, uint32_t d, uint32_t mips,
                    uint32_t layers)
{
    const struct gfx_format *fi = gfx_format_info(format);
    if (!fi || !w || !h || !d || !layers)
        return false;
    if (!mips)
        mips = gfx_mip_count(w, h, d);
    memset(img, 0, sizeof(*img));
    img->format = format;
    img->width = w;
    img->height = h;
    img->depth = d;
    img->mips = mips;
    img->layers = layers;
    img->subs = mips * layers;
    img->sub = gfx_alloc(sizeof(*img->sub) * img->subs);
    if (!img->sub)
        return false;
    size_t off = 0;
    for (uint32_t l = 0; l < layers; l++)
        for (uint32_t m = 0; m < mips; m++) {
            struct gfx_sub *s = &img->sub[l * mips + m];
            s->width = GFX_MAX(w >> m, 1u);
            s->height = GFX_MAX(h >> m, 1u);
            s->depth = GFX_MAX(d >> m, 1u);
            uint32_t bw = fi->block ? (s->width + 3) / 4 : s->width, bh = fi->block ? (s->height + 3) / 4 : s->height;
            s->row_pitch = bw * fi->bytes;
            s->depth_pitch = s->row_pitch * bh;
            s->offset = (uint32_t)off;
            off += ((size_t)s->depth_pitch * s->depth + 15) & ~(size_t)15;
        }
    img->size = off;
    img->data = gfx_alloc(off);
    if (!img->data) {
        gfx_free(img->sub);
        return false;
    }
    return true;
}

void gfx_image_free(struct gfx_image *img)
{
    gfx_free(img->data);
    gfx_free(img->sub);
    memset(img, 0, sizeof(*img));
}
