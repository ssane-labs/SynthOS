/* The graphics device's texel formats and texture unit (gd_internal.h): reading and writing every DXGI format
   the device knows, and sampling with filtering, mip levels, address modes, cubes and comparison. */

#if defined(__x86_64__) || defined(RS_HOST)

#include "gd_internal.h"
#include "gmath.h"

#ifdef RS_HOST
#include <string.h>
#else
#include <synth/string.h>
#endif

/* ---- formats */

uint32_t gd_format_bytes(uint32_t f, uint32_t *block)
{
    *block = 1;
    if (f >= GD_BC1_TYPELESS && f <= GD_BC5_SNORM) {
        *block = 4;
        return (f >= GD_BC1_TYPELESS && f <= GD_BC1_UNORM_SRGB) || (f >= GD_BC4_TYPELESS && f <= GD_BC4_SNORM) ? 8 : 16;
    }
    if (f >= GD_BC6H_TYPELESS && f <= GD_BC7_UNORM_SRGB) {
        *block = 4;
        return 16;
    }
    if (f >= 1 && f <= 4)
        return 16;
    if (f >= 5 && f <= 8)
        return 12;
    if (f >= 9 && f <= 22)
        return 8;
    if ((f >= 23 && f <= 47) || f == GD_R9G9B9E5_SHAREDEXP || (f >= 87 && f <= 93))
        return 4;
    if ((f >= 48 && f <= 59) || f == GD_B5G6R5_UNORM || f == GD_B5G5R5A1_UNORM || f == GD_B4G4R4A4_UNORM)
        return 2;
    if (f >= 60 && f <= 65)
        return 1;
    return 0;
}

int gd_format_class(uint32_t f)
{
    switch (f) {
    case GD_R32G32B32A32_UINT: case GD_R32G32B32_UINT: case GD_R16G16B16A16_UINT: case GD_R32G32_UINT:
    case GD_X32_TYPELESS_G8X24_UINT: case GD_R10G10B10A2_UINT: case GD_R8G8B8A8_UINT: case GD_R16G16_UINT:
    case GD_R32_UINT: case GD_X24_TYPELESS_G8_UINT: case GD_R8G8_UINT: case GD_R16_UINT: case GD_R8_UINT:
        return FC_UINT;
    case GD_R32G32B32A32_SINT: case GD_R32G32B32_SINT: case GD_R16G16B16A16_SINT: case GD_R32G32_SINT:
    case GD_R8G8B8A8_SINT: case GD_R16G16_SINT: case GD_R32_SINT: case GD_R8G8_SINT: case GD_R16_SINT:
    case GD_R8_SINT:
        return FC_SINT;
    default: {
        uint32_t block;
        return gd_format_bytes(f, &block) ? FC_FLOAT : FC_NONE;
    }
    }
}

bool gd_format_srgb(uint32_t f)
{
    return f == GD_R8G8B8A8_UNORM_SRGB || f == GD_B8G8R8A8_UNORM_SRGB || f == GD_B8G8R8X8_UNORM_SRGB ||
           f == GD_BC1_UNORM_SRGB || f == GD_BC2_UNORM_SRGB || f == GD_BC3_UNORM_SRGB || f == GD_BC7_UNORM_SRGB;
}

bool gd_format_depth(uint32_t f)
{
    return f == GD_D32_FLOAT || f == GD_D24_UNORM_S8_UINT || f == GD_D16_UNORM || f == GD_D32_FLOAT_S8X24_UINT;
}

uint32_t gd_format_typed(uint32_t f)
{
    switch (f) {
    case GD_R32G32B32A32_TYPELESS: return GD_R32G32B32A32_FLOAT;
    case GD_R32G32B32_TYPELESS: return GD_R32G32B32_FLOAT;
    case GD_R16G16B16A16_TYPELESS: return GD_R16G16B16A16_FLOAT;
    case GD_R32G32_TYPELESS: return GD_R32G32_FLOAT;
    case GD_R32G8X24_TYPELESS: return GD_R32_FLOAT_X8X24_TYPELESS;
    case GD_R10G10B10A2_TYPELESS: return GD_R10G10B10A2_UNORM;
    case GD_R8G8B8A8_TYPELESS: return GD_R8G8B8A8_UNORM;
    case GD_R16G16_TYPELESS: return GD_R16G16_FLOAT;
    case GD_R32_TYPELESS: return GD_R32_FLOAT;
    case GD_R24G8_TYPELESS: return GD_R24_UNORM_X8_TYPELESS;
    case GD_R8G8_TYPELESS: return GD_R8G8_UNORM;
    case GD_R16_TYPELESS: return GD_R16_FLOAT;
    case GD_R8_TYPELESS: return GD_R8_UNORM;
    case GD_BC1_TYPELESS: return GD_BC1_UNORM;
    case GD_BC2_TYPELESS: return GD_BC2_UNORM;
    case GD_BC3_TYPELESS: return GD_BC3_UNORM;
    case GD_BC4_TYPELESS: return GD_BC4_UNORM;
    case GD_BC5_TYPELESS: return GD_BC5_UNORM;
    case GD_B8G8R8A8_TYPELESS: return GD_B8G8R8A8_UNORM;
    case GD_B8G8R8X8_TYPELESS: return GD_B8G8R8X8_UNORM;
    case GD_BC6H_TYPELESS: return GD_BC6H_UF16;
    case GD_BC7_TYPELESS: return GD_BC7_UNORM;
    case GD_D32_FLOAT: return GD_R32_FLOAT;
    case GD_D24_UNORM_S8_UINT: return GD_R24_UNORM_X8_TYPELESS;
    case GD_D16_UNORM: return GD_R16_UNORM;
    case GD_D32_FLOAT_S8X24_UINT: return GD_R32_FLOAT_X8X24_TYPELESS;
    default: return f;
    }
}

bool gd_format(uint32_t f, struct gd_format *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t block, bytes = gd_format_bytes(f, &block);
    if (!bytes || f == GD_R8G8_B8G8_UNORM || f == GD_G8R8_G8B8_UNORM || f == GD_R10G10B10_XR_BIAS_A2_UNORM)
        return false;
    out->block_bytes = bytes, out->block = block;
    int cls = gd_format_class(f);
    bool typeless = gd_format_typed(f) != f && !gd_format_depth(f);
    uint32_t s = GD_SUPPORT_TEX1D | GD_SUPPORT_TEX2D | GD_SUPPORT_TEX3D | GD_SUPPORT_CUBE | GD_SUPPORT_MIP |
                 GD_SUPPORT_CPU_LOCKABLE | GD_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
    if (typeless) {
        out->support = s;
        return true;
    }
    if (gd_format_depth(f)) {
        out->support = GD_SUPPORT_TEX1D | GD_SUPPORT_TEX2D | GD_SUPPORT_CUBE | GD_SUPPORT_MIP | GD_SUPPORT_DEPTH_STENCIL;
        return true;
    }
    s |= GD_SUPPORT_LOAD | GD_SUPPORT_BUFFER;
    if (block == 1 && f != GD_R9G9B9E5_SHAREDEXP && f != GD_X24_TYPELESS_G8_UINT && f != GD_R24_UNORM_X8_TYPELESS &&
        f != GD_X32_TYPELESS_G8X24_UINT && f != GD_R32_FLOAT_X8X24_TYPELESS)
        s |= GD_SUPPORT_IA_VERTEX | GD_SUPPORT_RENDER_TARGET | GD_SUPPORT_MULTISAMPLE_RENDERTARGET |
             GD_SUPPORT_MULTISAMPLE_LOAD | GD_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (cls == FC_FLOAT) {
        s |= GD_SUPPORT_SAMPLE | GD_SUPPORT_SHADER_GATHER;
        if (block == 1 && (s & GD_SUPPORT_RENDER_TARGET)) {
            s |= GD_SUPPORT_BLENDABLE | GD_SUPPORT_MIP_AUTOGEN | GD_SUPPORT_MULTISAMPLE_RESOLVE;
        }
    }
    if (f == GD_R32_FLOAT || f == GD_R24_UNORM_X8_TYPELESS || f == GD_R16_UNORM || f == GD_R32_FLOAT_X8X24_TYPELESS)
        s |= GD_SUPPORT_SAMPLE_COMPARISON | GD_SUPPORT_SHADER_GATHER_COMPARISON;
    if (f == GD_R16_UINT || f == GD_R32_UINT)
        s |= GD_SUPPORT_IA_INDEX;
    if (f == GD_R8G8B8A8_UNORM || f == GD_R8G8B8A8_UNORM_SRGB || f == GD_B8G8R8A8_UNORM ||
        f == GD_B8G8R8A8_UNORM_SRGB || f == GD_R16G16B16A16_FLOAT || f == GD_R10G10B10A2_UNORM)
        s |= GD_SUPPORT_DISPLAY;
    out->support = s;
    return true;
}

/* ---- conversions */

static float srgb_table[256];
static bool srgb_ready;

static void tables(void)
{
    if (srgb_ready)
        return;
    for (int i = 0; i < 256; i++) {
        float c = (float)i / 255.0f;
        srgb_table[i] = c <= 0.04045f ? c / 12.92f : fpow((c + 0.055f) / 1.055f, 2.4f);
    }
    __atomic_store_n(&srgb_ready, true, __ATOMIC_RELEASE);
}

float gd_srgb_to_linear(float c)
{
    if (c <= 0.04045f)
        return c / 12.92f;
    return fpow((c + 0.055f) / 1.055f, 2.4f);
}

float gd_linear_to_srgb(float c)
{
    c = fclamp(c, 0, 1);
    if (c <= 0.0031308f)
        return c * 12.92f;
    return 1.055f * fpow(c, 1.0f / 2.4f) - 0.055f;
}

static uint32_t unorm(uint32_t v, int bits)
{
    return U((float)v / (float)((1u << bits) - 1));
}

static uint32_t snorm(uint32_t v, int bits)
{
    int32_t s = (int32_t)(v << (32 - bits)) >> (32 - bits);
    float f = (float)s / (float)((1 << (bits - 1)) - 1);
    return U(f < -1 ? -1 : f);
}

static uint32_t sext(uint32_t v, int bits)
{
    return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits));
}

static uint32_t to_unorm(uint32_t bits_f, int bits)
{
    float f = fclamp(F(bits_f), 0, 1);
    return (uint32_t)(f * (float)((1u << bits) - 1) + 0.5f);
}

static uint32_t to_snorm(uint32_t bits_f, int bits)
{
    float f = fclamp(F(bits_f), -1, 1) * (float)((1 << (bits - 1)) - 1);
    int32_t i = (int32_t)(f + (f >= 0 ? 0.5f : -0.5f));
    return (uint32_t)i & ((1u << bits) - 1);
}

static uint32_t half(uint32_t h)
{
    return U(f16_to_f32(h));
}

/* the small unsigned floats of R11G11B10: 5 exponent bits, 6 or 5 mantissa bits */
static float small_float(uint32_t v, int mbits)
{
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 31)
        return m ? F(0x7fc00000u) : F(0x7f800000u);
    if (!e)
        return (float)m / (float)(1u << mbits) / 16384.0f;
    return F((e + 112) << 23 | m << (23 - mbits));
}

static uint32_t to_small_float(float f, int mbits)
{
    if (f != f)
        return 31u << mbits | 1;
    if (f <= 0)
        return 0;
    uint32_t u = U(f);
    int e = (int)((u >> 23) & 255) - 127 + 15;
    if (e >= 31)
        return 31u << mbits;
    uint32_t m = (u & 0x7fffffu) | 0x800000u;
    if (e <= 0) {
        int shift = 24 - mbits - e;
        return shift < 32 ? (m >> shift) : 0;
    }
    return (uint32_t)e << mbits | ((m >> (23 - mbits)) & ((1u << mbits) - 1));
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

void gd_texel_load(uint32_t f, const uint8_t *p, int bx, int by, uint32_t o[4])
{
    o[0] = o[1] = o[2] = 0;
    o[3] = gd_format_class(f) == FC_FLOAT ? U(1.0f) : 1;
    switch (f) {
    case GD_R32G32B32A32_FLOAT: case GD_R32G32B32A32_UINT: case GD_R32G32B32A32_SINT: case GD_R32G32B32A32_TYPELESS:
        memcpy(o, p, 16);
        break;
    case GD_R32G32B32_FLOAT: case GD_R32G32B32_UINT: case GD_R32G32B32_SINT: case GD_R32G32B32_TYPELESS:
        memcpy(o, p, 12);
        break;
    case GD_R32G32_FLOAT: case GD_R32G32_UINT: case GD_R32G32_SINT: case GD_R32G32_TYPELESS:
        memcpy(o, p, 8);
        break;
    case GD_R32_FLOAT: case GD_R32_UINT: case GD_R32_SINT: case GD_D32_FLOAT: case GD_R32_TYPELESS:
    case GD_R32_FLOAT_X8X24_TYPELESS: case GD_D32_FLOAT_S8X24_UINT: case GD_R32G8X24_TYPELESS:
        o[0] = rd32(p);
        break;
    case GD_X32_TYPELESS_G8X24_UINT:
        o[1] = p[4];
        break;
    case GD_R16G16B16A16_FLOAT: case GD_R16G16B16A16_TYPELESS:
        for (int i = 0; i < 4; i++)
            o[i] = half(rd16(p + 2 * i));
        break;
    case GD_R16G16B16A16_UNORM:
        for (int i = 0; i < 4; i++)
            o[i] = unorm(rd16(p + 2 * i), 16);
        break;
    case GD_R16G16B16A16_SNORM:
        for (int i = 0; i < 4; i++)
            o[i] = snorm(rd16(p + 2 * i), 16);
        break;
    case GD_R16G16B16A16_UINT:
        for (int i = 0; i < 4; i++)
            o[i] = rd16(p + 2 * i);
        break;
    case GD_R16G16B16A16_SINT:
        for (int i = 0; i < 4; i++)
            o[i] = sext(rd16(p + 2 * i), 16);
        break;
    case GD_R16G16_FLOAT: case GD_R16G16_TYPELESS:
        o[0] = half(rd16(p)), o[1] = half(rd16(p + 2));
        break;
    case GD_R16G16_UNORM: o[0] = unorm(rd16(p), 16), o[1] = unorm(rd16(p + 2), 16); break;
    case GD_R16G16_SNORM: o[0] = snorm(rd16(p), 16), o[1] = snorm(rd16(p + 2), 16); break;
    case GD_R16G16_UINT: o[0] = rd16(p), o[1] = rd16(p + 2); break;
    case GD_R16G16_SINT: o[0] = sext(rd16(p), 16), o[1] = sext(rd16(p + 2), 16); break;
    case GD_R16_FLOAT: case GD_R16_TYPELESS: o[0] = half(rd16(p)); break;
    case GD_R16_UNORM: case GD_D16_UNORM: o[0] = unorm(rd16(p), 16); break;
    case GD_R16_SNORM: o[0] = snorm(rd16(p), 16); break;
    case GD_R16_UINT: o[0] = rd16(p); break;
    case GD_R16_SINT: o[0] = sext(rd16(p), 16); break;
    case GD_R10G10B10A2_UNORM: case GD_R10G10B10A2_TYPELESS: {
        uint32_t v = rd32(p);
        o[0] = unorm(v & 1023, 10), o[1] = unorm(v >> 10 & 1023, 10), o[2] = unorm(v >> 20 & 1023, 10);
        o[3] = unorm(v >> 30, 2);
        break;
    }
    case GD_R10G10B10A2_UINT: {
        uint32_t v = rd32(p);
        o[0] = v & 1023, o[1] = v >> 10 & 1023, o[2] = v >> 20 & 1023, o[3] = v >> 30;
        break;
    }
    case GD_R11G11B10_FLOAT: {
        uint32_t v = rd32(p);
        o[0] = U(small_float(v & 2047, 6)), o[1] = U(small_float(v >> 11 & 2047, 6));
        o[2] = U(small_float(v >> 22, 5));
        break;
    }
    case GD_R9G9B9E5_SHAREDEXP: {
        uint32_t v = rd32(p);
        float scale = F((uint32_t)((int)(v >> 27) - 15 - 9 + 127) << 23);
        o[0] = U((float)(v & 511) * scale), o[1] = U((float)(v >> 9 & 511) * scale);
        o[2] = U((float)(v >> 18 & 511) * scale);
        break;
    }
    case GD_R8G8B8A8_UNORM: case GD_R8G8B8A8_UNORM_SRGB: case GD_R8G8B8A8_TYPELESS:
        for (int i = 0; i < 4; i++)
            o[i] = unorm(p[i], 8);
        break;
    case GD_R8G8B8A8_SNORM:
        for (int i = 0; i < 4; i++)
            o[i] = snorm(p[i], 8);
        break;
    case GD_R8G8B8A8_UINT:
        for (int i = 0; i < 4; i++)
            o[i] = p[i];
        break;
    case GD_R8G8B8A8_SINT:
        for (int i = 0; i < 4; i++)
            o[i] = sext(p[i], 8);
        break;
    case GD_B8G8R8A8_UNORM: case GD_B8G8R8A8_UNORM_SRGB: case GD_B8G8R8A8_TYPELESS:
        o[0] = unorm(p[2], 8), o[1] = unorm(p[1], 8), o[2] = unorm(p[0], 8), o[3] = unorm(p[3], 8);
        break;
    case GD_B8G8R8X8_UNORM: case GD_B8G8R8X8_UNORM_SRGB: case GD_B8G8R8X8_TYPELESS:
        o[0] = unorm(p[2], 8), o[1] = unorm(p[1], 8), o[2] = unorm(p[0], 8);
        break;
    case GD_R8G8_UNORM: case GD_R8G8_TYPELESS: o[0] = unorm(p[0], 8), o[1] = unorm(p[1], 8); break;
    case GD_R8G8_SNORM: o[0] = snorm(p[0], 8), o[1] = snorm(p[1], 8); break;
    case GD_R8G8_UINT: o[0] = p[0], o[1] = p[1]; break;
    case GD_R8G8_SINT: o[0] = sext(p[0], 8), o[1] = sext(p[1], 8); break;
    case GD_R8_UNORM: case GD_R8_TYPELESS: o[0] = unorm(p[0], 8); break;
    case GD_R8_SNORM: o[0] = snorm(p[0], 8); break;
    case GD_R8_UINT: o[0] = p[0]; break;
    case GD_R8_SINT: o[0] = sext(p[0], 8); break;
    case GD_A8_UNORM: o[3] = unorm(p[0], 8); break;
    case GD_R24_UNORM_X8_TYPELESS: case GD_D24_UNORM_S8_UINT: case GD_R24G8_TYPELESS:
        o[0] = U((float)(rd32(p) & 0xffffff) / 16777215.0f);
        break;
    case GD_X24_TYPELESS_G8_UINT: o[1] = p[3]; break;
    case GD_B5G6R5_UNORM: {
        uint32_t v = rd16(p);
        o[0] = unorm(v >> 11, 5), o[1] = unorm(v >> 5 & 63, 6), o[2] = unorm(v & 31, 5);
        break;
    }
    case GD_B5G5R5A1_UNORM: {
        uint32_t v = rd16(p);
        o[0] = unorm(v >> 10 & 31, 5), o[1] = unorm(v >> 5 & 31, 5), o[2] = unorm(v & 31, 5), o[3] = unorm(v >> 15, 1);
        break;
    }
    case GD_B4G4R4A4_UNORM: {
        uint32_t v = rd16(p);
        o[0] = unorm(v >> 8 & 15, 4), o[1] = unorm(v >> 4 & 15, 4), o[2] = unorm(v & 15, 4), o[3] = unorm(v >> 12, 4);
        break;
    }
    default: {
        uint32_t block;
        if (gd_format_bytes(f, &block) && block == 4) {
            float c[4];
            gd_bc_texel(gd_format_typed(f), p, bx, by, c);
            for (int i = 0; i < 4; i++)
                o[i] = U(c[i]);
        }
        break;
    }
    }
    if (gd_format_srgb(f))
        for (int i = 0; i < 3; i++) {
            float c = F(o[i]);
            if (srgb_ready) {
                int k = (int)(c * 255.0f + 0.5f);
                o[i] = U(srgb_table[k < 0 ? 0 : k > 255 ? 255 : k]);
            } else {
                o[i] = U(gd_srgb_to_linear(c));
            }
        }
}

void gd_texel_store(uint32_t f, uint8_t *p, const uint32_t vin[4], unsigned mask)
{
    uint32_t v[4] = {vin[0], vin[1], vin[2], vin[3]};
    if (gd_format_srgb(f))
        for (int i = 0; i < 3; i++)
            v[i] = U(gd_linear_to_srgb(F(v[i])));
    uint32_t block;
    uint32_t bytes = gd_format_bytes(f, &block);
    if (!bytes || block != 1)
        return;
    uint8_t px[16];
    memcpy(px, p, bytes); /* the components not written keep their value */
    switch (f) {
    case GD_R32G32B32A32_FLOAT: case GD_R32G32B32A32_UINT: case GD_R32G32B32A32_SINT: case GD_R32G32B32A32_TYPELESS:
    case GD_R32G32B32_FLOAT: case GD_R32G32B32_UINT: case GD_R32G32B32_SINT: case GD_R32G32B32_TYPELESS:
    case GD_R32G32_FLOAT: case GD_R32G32_UINT: case GD_R32G32_SINT: case GD_R32G32_TYPELESS:
    case GD_R32_FLOAT: case GD_R32_UINT: case GD_R32_SINT: case GD_R32_TYPELESS: case GD_D32_FLOAT:
        for (uint32_t i = 0; i < bytes / 4; i++)
            if (mask & (1u << i))
                memcpy(px + 4 * i, &v[i], 4);
        break;
    case GD_R16G16B16A16_FLOAT: case GD_R16G16B16A16_TYPELESS: case GD_R16G16_FLOAT: case GD_R16G16_TYPELESS:
    case GD_R16_FLOAT: case GD_R16_TYPELESS:
        for (uint32_t i = 0; i < bytes / 2; i++)
            if (mask & (1u << i))
                wr16(px + 2 * i, f32_to_f16(F(v[i])));
        break;
    case GD_R16G16B16A16_UNORM: case GD_R16G16_UNORM: case GD_R16_UNORM: case GD_D16_UNORM:
        for (uint32_t i = 0; i < bytes / 2; i++)
            if (mask & (1u << i))
                wr16(px + 2 * i, to_unorm(v[i], 16));
        break;
    case GD_R16G16B16A16_SNORM: case GD_R16G16_SNORM: case GD_R16_SNORM:
        for (uint32_t i = 0; i < bytes / 2; i++)
            if (mask & (1u << i))
                wr16(px + 2 * i, to_snorm(v[i], 16));
        break;
    case GD_R16G16B16A16_UINT: case GD_R16G16_UINT: case GD_R16_UINT:
    case GD_R16G16B16A16_SINT: case GD_R16G16_SINT: case GD_R16_SINT:
        for (uint32_t i = 0; i < bytes / 2; i++)
            if (mask & (1u << i))
                wr16(px + 2 * i, v[i]);
        break;
    case GD_R8G8B8A8_UNORM: case GD_R8G8B8A8_UNORM_SRGB: case GD_R8G8B8A8_TYPELESS: case GD_R8G8_UNORM:
    case GD_R8G8_TYPELESS: case GD_R8_UNORM: case GD_R8_TYPELESS:
        for (uint32_t i = 0; i < bytes; i++)
            if (mask & (1u << i))
                px[i] = (uint8_t)to_unorm(v[i], 8);
        break;
    case GD_R8G8B8A8_SNORM: case GD_R8G8_SNORM: case GD_R8_SNORM:
        for (uint32_t i = 0; i < bytes; i++)
            if (mask & (1u << i))
                px[i] = (uint8_t)to_snorm(v[i], 8);
        break;
    case GD_R8G8B8A8_UINT: case GD_R8G8_UINT: case GD_R8_UINT: case GD_R8G8B8A8_SINT: case GD_R8G8_SINT:
    case GD_R8_SINT:
        for (uint32_t i = 0; i < bytes; i++)
            if (mask & (1u << i))
                px[i] = (uint8_t)v[i];
        break;
    case GD_A8_UNORM:
        if (mask & 8)
            px[0] = (uint8_t)to_unorm(v[3], 8);
        break;
    case GD_B8G8R8A8_UNORM: case GD_B8G8R8A8_UNORM_SRGB: case GD_B8G8R8A8_TYPELESS:
    case GD_B8G8R8X8_UNORM: case GD_B8G8R8X8_UNORM_SRGB: case GD_B8G8R8X8_TYPELESS: {
        static const int at[4] = {2, 1, 0, 3};
        for (int i = 0; i < 4; i++)
            if (mask & (1u << i))
                px[at[i]] = (uint8_t)to_unorm(v[i], 8);
        if (f == GD_B8G8R8X8_UNORM || f == GD_B8G8R8X8_UNORM_SRGB || f == GD_B8G8R8X8_TYPELESS)
            px[3] = 255;
        break;
    }
    case GD_R10G10B10A2_UNORM: case GD_R10G10B10A2_TYPELESS: case GD_R10G10B10A2_UINT: {
        bool ints = f == GD_R10G10B10A2_UINT;
        uint32_t w = rd32(px);
        static const int shift[4] = {0, 10, 20, 30}, bits[4] = {10, 10, 10, 2};
        for (int i = 0; i < 4; i++)
            if (mask & (1u << i)) {
                uint32_t m = (1u << bits[i]) - 1, c = ints ? v[i] & m : to_unorm(v[i], bits[i]);
                w = (w & ~(m << shift[i])) | c << shift[i];
            }
        memcpy(px, &w, 4);
        break;
    }
    case GD_R11G11B10_FLOAT: {
        uint32_t w = rd32(px);
        if (mask & 1)
            w = (w & ~2047u) | to_small_float(F(v[0]), 6);
        if (mask & 2)
            w = (w & ~(2047u << 11)) | to_small_float(F(v[1]), 6) << 11;
        if (mask & 4)
            w = (w & ~(1023u << 22)) | to_small_float(F(v[2]), 5) << 22;
        memcpy(px, &w, 4);
        break;
    }
    case GD_B5G6R5_UNORM: {
        uint32_t w = rd16(px);
        if (mask & 1)
            w = (w & ~0xf800u) | to_unorm(v[0], 5) << 11;
        if (mask & 2)
            w = (w & ~0x07e0u) | to_unorm(v[1], 6) << 5;
        if (mask & 4)
            w = (w & ~0x1fu) | to_unorm(v[2], 5);
        wr16(px, w);
        break;
    }
    case GD_B5G5R5A1_UNORM: {
        uint32_t w = rd16(px);
        if (mask & 1)
            w = (w & ~(31u << 10)) | to_unorm(v[0], 5) << 10;
        if (mask & 2)
            w = (w & ~(31u << 5)) | to_unorm(v[1], 5) << 5;
        if (mask & 4)
            w = (w & ~31u) | to_unorm(v[2], 5);
        if (mask & 8)
            w = (w & 0x7fffu) | to_unorm(v[3], 1) << 15;
        wr16(px, w);
        break;
    }
    case GD_B4G4R4A4_UNORM: {
        uint32_t w = rd16(px);
        static const int shift[4] = {8, 4, 0, 12};
        for (int i = 0; i < 4; i++)
            if (mask & (1u << i))
                w = (w & ~(15u << shift[i])) | to_unorm(v[i], 4) << shift[i];
        wr16(px, w);
        break;
    }
    default: return;
    }
    memcpy(p, px, bytes);
}

/* ---- depth and stencil */

float gd_depth_read(uint32_t f, const uint8_t *p)
{
    switch (f) {
    case GD_D32_FLOAT: case GD_D32_FLOAT_S8X24_UINT: return F(rd32(p));
    case GD_D24_UNORM_S8_UINT: return (float)(rd32(p) & 0xffffff) / 16777215.0f;
    case GD_D16_UNORM: return (float)rd16(p) / 65535.0f;
    default: return 0;
    }
}

void gd_depth_write(uint32_t f, uint8_t *p, float d)
{
    d = fclamp(d, 0, 1);
    switch (f) {
    case GD_D32_FLOAT: case GD_D32_FLOAT_S8X24_UINT: memcpy(p, &d, 4); break;
    case GD_D24_UNORM_S8_UINT: {
        uint32_t z = (uint32_t)((double)d * 16777215.0 + 0.5);
        uint32_t v = (rd32(p) & 0xff000000u) | (z > 0xffffffu ? 0xffffffu : z);
        memcpy(p, &v, 4);
        break;
    }
    case GD_D16_UNORM: wr16(p, (uint32_t)(d * 65535.0f + 0.5f)); break;
    default: break;
    }
}

uint32_t gd_stencil_read(uint32_t f, const uint8_t *p)
{
    return f == GD_D24_UNORM_S8_UINT ? p[3] : f == GD_D32_FLOAT_S8X24_UINT ? p[4] : 0;
}

void gd_stencil_write(uint32_t f, uint8_t *p, uint32_t s)
{
    if (f == GD_D24_UNORM_S8_UINT)
        p[3] = (uint8_t)s;
    else if (f == GD_D32_FLOAT_S8X24_UINT)
        p[4] = (uint8_t)s;
}

/* ---- the texture unit */

static int address(int i, int n, uint32_t mode, bool *border)
{
    switch (mode) {
    case 1: /* wrap */
        i %= n;
        return i < 0 ? i + n : i;
    case 2: { /* mirror */
        int m = i % (2 * n);
        m = m < 0 ? m + 2 * n : m;
        return m < n ? m : 2 * n - 1 - m;
    }
    case 4: /* border */
        if (i < 0 || i >= n)
            *border = true;
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    case 5: /* mirror once */
        i = i < 0 ? -1 - i : i;
        return i >= n ? n - 1 : i;
    default: /* clamp */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

struct level {
    const struct gd_view *v;
    const uint8_t *base;
    const struct gd_layout *lay;
    uint32_t format, block;
    int w, h, d;
};

static bool level_of(const struct gd_view *v, uint32_t mip, uint32_t slice, struct level *l)
{
    const struct gd_resource *r = v->r;
    uint32_t mips = r->d.mips ? r->d.mips : 1;
    uint32_t m = v->d.first_mip + mip, s = v->d.first_slice + slice;
    if (m >= mips)
        return false;
    uint32_t sub = r->d.dim == GD_TEX3D ? m : m + s * mips;
    if (sub >= r->subs)
        return false;
    l->v = v;
    l->lay = &r->lay[sub];
    l->base = r->data + r->off[sub];
    l->format = v->format;
    gd_format_bytes(v->format, &l->block);
    l->w = (int)l->lay->width, l->h = (int)l->lay->height, l->d = (int)l->lay->depth;
    return true;
}

static void fetch(const struct level *l, int x, int y, int z, float out[4])
{
    uint32_t bytes, block, raw[4];
    bytes = gd_format_bytes(l->format, &block);
    const uint8_t *p = l->base + (size_t)z * l->lay->slice_pitch + (size_t)(y / (int)block) * l->lay->row_pitch +
                       (size_t)(x / (int)block) * bytes;
    gd_texel_load(l->format, p, x & 3, y & 3, raw);
    int cls = gd_format_class(l->format);
    for (int i = 0; i < 4; i++)
        out[i] = cls == FC_FLOAT ? F(raw[i]) : cls == FC_SINT ? (float)(int32_t)raw[i] : (float)raw[i];
}

static bool compare(uint32_t func, float ref, float texel)
{
    switch (func) {
    case 1: return false;
    case 2: return ref < texel;
    case 3: return ref == texel;
    case 4: return ref <= texel;
    case 5: return ref > texel;
    case 6: return ref != texel;
    case 7: return ref >= texel;
    default: return true;
    }
}

static void texel(const struct level *l, const struct gd_sampler *s, int x, int y, int z, const float *ref,
                  float out[4])
{
    bool border = false;
    x = address(x, l->w, s->address_u, &border);
    y = address(y, l->h, s->address_v, &border);
    z = address(z, l->d, s->address_w, &border);
    if (border)
        memcpy(out, s->border, sizeof(float) * 4);
    else
        fetch(l, x, y, z, out);
    if (ref) {
        float c = compare(s->compare, *ref, out[0]) ? 1.0f : 0.0f;
        out[0] = out[1] = out[2] = out[3] = c;
    }
}

/* one mip level: point or (bi/tri)linear */
static void sample_level(const struct level *l, const struct gd_sampler *s, float u, float v, float w, bool linear,
                         int dims, const int offset[3], const float *ref, float out[4])
{
    float fx = u * (float)l->w, fy = v * (float)l->h, fz = w * (float)l->d;
    if (!linear) {
        int x = (int)ffloor(fx) + offset[0], y = dims > 1 ? (int)ffloor(fy) + offset[1] : 0;
        int z = dims > 2 ? (int)ffloor(fz) + offset[2] : 0;
        texel(l, s, x, y, z, ref, out);
        return;
    }
    fx -= 0.5f, fy -= 0.5f, fz -= 0.5f;
    float bx = ffloor(fx), by = ffloor(fy), bz = ffloor(fz);
    float ax = fx - bx, ay = dims > 1 ? fy - by : 0, az = dims > 2 ? fz - bz : 0;
    int x0 = (int)bx + offset[0], y0 = dims > 1 ? (int)by + offset[1] : 0, z0 = dims > 2 ? (int)bz + offset[2] : 0;
    for (int i = 0; i < 4; i++)
        out[i] = 0;
    int nz = dims > 2 ? 2 : 1, ny = dims > 1 ? 2 : 1;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < 2; i++) {
                float wgt = (i ? ax : 1 - ax) * (dims > 1 ? (j ? ay : 1 - ay) : 1) * (dims > 2 ? (k ? az : 1 - az) : 1);
                if (wgt == 0)
                    continue;
                float t[4];
                texel(l, s, x0 + i, y0 + j, z0 + k, ref, t);
                for (int c = 0; c < 4; c++)
                    out[c] += t[c] * wgt;
            }
}

static int view_dims(const struct gd_view *v)
{
    switch (v->vdim) {
    case VD_1D: case VD_1D_ARRAY: return 1;
    case VD_3D: return 3;
    default: return 2;
    }
}

/* a cube direction to a face and its coordinates */
static int cube_face(const float d[3], float *u, float *v, float *ma)
{
    float ax = d[0] < 0 ? -d[0] : d[0], ay = d[1] < 0 ? -d[1] : d[1], az = d[2] < 0 ? -d[2] : d[2];
    int face;
    float sc, tc, m;
    if (ax >= ay && ax >= az) {
        face = d[0] >= 0 ? 0 : 1, m = ax;
        sc = d[0] >= 0 ? -d[2] : d[2], tc = -d[1];
    } else if (ay >= az) {
        face = d[1] >= 0 ? 2 : 3, m = ay;
        sc = d[0], tc = d[1] >= 0 ? d[2] : -d[2];
    } else {
        face = d[2] >= 0 ? 4 : 5, m = az;
        sc = d[2] >= 0 ? d[0] : -d[0], tc = -d[1];
    }
    if (m == 0)
        m = 1;
    *u = (sc / m + 1) * 0.5f, *v = (tc / m + 1) * 0.5f, *ma = m;
    return face;
}

static uint32_t view_mips(const struct gd_view *v)
{
    uint32_t mips = v->r->d.mips ? v->r->d.mips : 1;
    uint32_t n = v->d.mips ? v->d.mips : mips - v->d.first_mip;
    return n ? n : 1;
}

static int slice_of(float c, uint32_t slices)
{
    int s = (int)ffloor(c + 0.5f);
    return s < 0 ? 0 : s >= (int)slices ? (int)slices - 1 : s;
}

/* the level the derivatives of the coordinates select on a level-0 footprint of l0 (before bias and clamping) */
static float derived_lod(const struct gd_sampler *s, int dims, const struct level *l0, const float du[2],
                         const float dv[2], const float dw[2])
{
    bool aniso = (s->filter & 0x40) != 0;
    float x0 = du[0] * (float)l0->w, y0 = dv[0] * (float)l0->h, z0 = dw[0] * (float)l0->d;
    float x1 = du[1] * (float)l0->w, y1 = dv[1] * (float)l0->h, z1 = dw[1] * (float)l0->d;
    float a = x0 * x0 + (dims > 1 ? y0 * y0 : 0) + (dims > 2 ? z0 * z0 : 0);
    float b = x1 * x1 + (dims > 1 ? y1 * y1 : 0) + (dims > 2 ? z1 * z1 : 0);
    float big = a > b ? a : b, small = a > b ? b : a;
    if (aniso && small > 0) { /* the anisotropic footprint: sharper along the long axis, at most max_anisotropy */
        float ratio = big / small, max = (float)(s->max_anisotropy ? s->max_anisotropy : 1);
        if (ratio > max * max)
            ratio = max * max;
        big /= ratio;
    }
    return big > 0 ? 0.5f * flog2(big) : -32.0f;
}

/* the derivatives on a cube's face, roughly */
static void cube_derivatives(const float coord[4], const float ddx[4], const float ddy[4], float du[2], float dv[2])
{
    float u, vv, ma;
    cube_face(coord, &u, &vv, &ma);
    float k = 0.5f / ma;
    for (int i = 0; i < 2; i++) {
        const float *d = i ? ddy : ddx;
        float m0 = d[0] < 0 ? -d[0] : d[0], m1 = d[1] < 0 ? -d[1] : d[1], m2 = d[2] < 0 ? -d[2] : d[2];
        float big = m0 > m1 ? (m0 > m2 ? m0 : m2) : (m1 > m2 ? m1 : m2);
        du[i] = big * k, dv[i] = big * k;
    }
}

void gd_lod(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const float ddx[4],
            const float ddy[4], float out[2])
{
    out[0] = out[1] = 0;
    struct level l0;
    if (!v || !v->r || v->vdim == VD_BUFFER || !s || !level_of(v, 0, 0, &l0))
        return;
    int dims = view_dims(v);
    float du[2] = {ddx[0], ddy[0]}, dv[2] = {ddx[1], ddy[1]}, dw[2] = {ddx[2], ddy[2]};
    if (v->vdim == VD_CUBE || v->vdim == VD_CUBE_ARRAY)
        cube_derivatives(coord, ddx, ddy, du, dv), dims = 2;
    float lod = derived_lod(s, dims, &l0, du, dv, dw) + s->mip_lod_bias;
    out[1] = lod;
    lod = lod < s->min_lod ? s->min_lod : lod > s->max_lod ? s->max_lod : lod;
    float last = (float)(view_mips(v) - 1);
    out[0] = lod < 0 ? 0 : lod > last ? last : lod;
}

void gd_sample(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const float ddx[4],
               const float ddy[4], float lod, float bias, const int offset[3], const float *ref, float out[4])
{
    out[0] = out[1] = out[2] = 0, out[3] = 1;
    if (!v || !v->r || v->vdim == VD_BUFFER)
        return;
    static const struct gd_sampler point = {.filter = 0, .address_u = 3, .address_v = 3, .address_w = 3,
                                            .max_lod = 3.4e38f};
    if (!s)
        s = &point;
    int dims = view_dims(v);
    float u = coord[0], vv = dims > 1 ? coord[1] : 0, w = dims > 2 ? coord[2] : 0;
    float du[2] = {ddx[0], ddy[0]}, dv[2] = {ddx[1], ddy[1]}, dw[2] = {ddx[2], ddy[2]};
    uint32_t slice = 0, slices = v->d.slices ? v->d.slices : 1;
    if (v->vdim == VD_CUBE || v->vdim == VD_CUBE_ARRAY) {
        float ma;
        int face = cube_face(coord, &u, &vv, &ma);
        cube_derivatives(coord, ddx, ddy, du, dv);
        slice = (uint32_t)face;
        if (v->vdim == VD_CUBE_ARRAY)
            slice += 6u * (uint32_t)slice_of(coord[3], slices / 6 ? slices / 6 : 1);
        dims = 2;
    } else if (v->vdim == VD_1D_ARRAY) {
        slice = (uint32_t)slice_of(coord[1], slices);
    } else if (v->vdim == VD_2D_ARRAY) {
        slice = (uint32_t)slice_of(coord[2], slices);
    }
    struct level l0;
    if (!level_of(v, 0, slice, &l0))
        return;
    uint32_t f = s->filter;
    bool aniso = (f & 0x40) != 0;
    bool mip_lin = aniso || (f & 1), mag_lin = aniso || (f & 4), min_lin = aniso || (f & 0x10);
    if (lod < -1e30f)
        lod = derived_lod(s, dims, &l0, du, dv, dw);
    lod += bias + s->mip_lod_bias;
    float maxl = s->max_lod, minl = s->min_lod;
    lod = lod < minl ? minl : lod > maxl ? maxl : lod;
    uint32_t mips = view_mips(v);
    bool linear = lod > 0 ? min_lin : mag_lin;
    if (lod < 0)
        lod = 0;
    if (lod > (float)(mips - 1))
        lod = (float)(mips - 1);
    if (!mip_lin) {
        uint32_t m = (uint32_t)ffloor(lod + 0.5f);
        struct level l;
        if (level_of(v, m, slice, &l))
            sample_level(&l, s, u, vv, w, linear, dims, offset, ref, out);
        return;
    }
    uint32_t m0 = (uint32_t)ffloor(lod);
    float t = lod - (float)m0;
    struct level a, b;
    if (!level_of(v, m0, slice, &a))
        return;
    sample_level(&a, s, u, vv, w, linear, dims, offset, ref, out);
    if (t > 0 && m0 + 1 < mips && level_of(v, m0 + 1, slice, &b)) {
        float o2[4];
        sample_level(&b, s, u, vv, w, linear, dims, offset, ref, o2);
        for (int c = 0; c < 4; c++)
            out[c] += (o2[c] - out[c]) * t;
    }
}

void gd_gather(const struct gd_view *v, const struct gd_sampler *s, const float coord[4], const int offset[3],
               int component, const float *ref, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!v || !v->r || v->vdim == VD_BUFFER || !s)
        return;
    float u = coord[0], vv = coord[1];
    uint32_t slice = 0, slices = v->d.slices ? v->d.slices : 1;
    if (v->vdim == VD_CUBE || v->vdim == VD_CUBE_ARRAY) {
        float ma;
        slice = (uint32_t)cube_face(coord, &u, &vv, &ma);
        if (v->vdim == VD_CUBE_ARRAY)
            slice += 6u * (uint32_t)slice_of(coord[3], slices / 6 ? slices / 6 : 1);
    } else if (v->vdim == VD_2D_ARRAY) {
        slice = (uint32_t)slice_of(coord[2], slices);
    }
    struct level l;
    if (!level_of(v, 0, slice, &l))
        return;
    float fx = u * (float)l.w - 0.5f, fy = vv * (float)l.h - 0.5f;
    int x0 = (int)ffloor(fx) + offset[0], y0 = (int)ffloor(fy) + offset[1];
    static const int at[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int i = 0; i < 4; i++) {
        float t[4];
        texel(&l, s, x0 + at[i][0], y0 + at[i][1], 0, ref, t);
        out[i] = t[component & 3];
    }
}

void gd_texel_fetch(const struct gd_view *v, const int32_t coord[4], const int offset[3], uint32_t out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!v || !v->r)
        return;
    const struct gd_resource *r = v->r;
    if (v->vdim == VD_BUFFER) {
        uint32_t bytes, block;
        bytes = gd_format_bytes(v->format, &block);
        int64_t e = (int64_t)coord[0];
        if (!bytes || e < 0 || (uint64_t)e >= v->d.elements)
            return;
        size_t at = ((size_t)v->d.first_element + (size_t)e) * bytes;
        if (at + bytes <= r->size)
            gd_texel_load(v->format, r->data + at, 0, 0, out);
        return;
    }
    int dims = view_dims(v);
    uint32_t slice = 0;
    int x = coord[0] + offset[0], y = dims > 1 ? coord[1] + offset[1] : 0, z = dims > 2 ? coord[2] + offset[2] : 0;
    if (v->vdim == VD_1D_ARRAY)
        slice = (uint32_t)coord[1], y = 0;
    else if (v->vdim == VD_2D_ARRAY || v->vdim == VD_CUBE || v->vdim == VD_CUBE_ARRAY)
        slice = (uint32_t)coord[2], z = 0;
    struct level l;
    uint32_t slices = v->d.slices ? v->d.slices : 1;
    if ((uint32_t)coord[3] >= view_mips(v) || slice >= slices || !level_of(v, (uint32_t)coord[3], slice, &l))
        return;
    if (x < 0 || y < 0 || z < 0 || x >= l.w || y >= l.h || z >= l.d)
        return;
    uint32_t block, bytes = gd_format_bytes(l.format, &block);
    const uint8_t *p = l.base + (size_t)z * l.lay->slice_pitch + (size_t)(y / (int)block) * l.lay->row_pitch +
                       (size_t)(x / (int)block) * bytes;
    gd_texel_load(l.format, p, x & 3, y & 3, out);
}

void gd_view_size(const struct gd_view *v, uint32_t mip, uint32_t out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!v || !v->r)
        return;
    if (v->vdim == VD_BUFFER) {
        out[0] = v->d.elements;
        return;
    }
    uint32_t mips = view_mips(v);
    out[3] = mips;
    if (mip >= mips)
        return;
    const struct gd_res_desc *d = &v->r->d;
    uint32_t m = v->d.first_mip + mip;
    uint32_t w = d->width >> m, h = d->height >> m, dd = d->depth >> m;
    w = w ? w : 1, h = h ? h : 1, dd = dd ? dd : 1;
    uint32_t slices = v->d.slices ? v->d.slices : 1;
    switch (v->vdim) {
    case VD_1D: out[0] = w; break;
    case VD_1D_ARRAY: out[0] = w, out[1] = slices; break;
    case VD_2D: case VD_CUBE: out[0] = w, out[1] = h; break;
    case VD_2D_ARRAY: out[0] = w, out[1] = h, out[2] = slices; break;
    case VD_CUBE_ARRAY: out[0] = w, out[1] = h, out[2] = slices / 6; break;
    case VD_3D: out[0] = w, out[1] = h, out[2] = dd; break;
    default: break;
    }
}

/* the tables the texel reads use, filled before drawing starts (never on helper processors) */
void gd_texture_init(void);
void gd_texture_init(void)
{
    tables();
}

#endif
