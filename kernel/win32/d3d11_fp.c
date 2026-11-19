/* Direct3D 11 and DXGI for 64-bit programs: COM objects over the graphics device (include/synth/gd.h). The device
   creates resources, views, shaders and states in kernel memory; the immediate context sets them on a gd context
   whose draws run the program's shaders on the software pipeline; a swap chain's Present puts the back buffer into
   the window. Map gives the program the resource's own pages, so nothing is copied.
   Chromium and the programs built on it (they ship ANGLE's libGLESv2.dll) keep the old answer, a PC without a
   graphics driver, and draw with SwiftShader as before. */

#include <synth/types.h>

#ifdef SYNTH_X32

#include <synth/gd.h>
#include <synth/gui.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "w32gui.h"
#include "win64.h"

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define DXGI_ERROR_INVALID_CALL 0x887a0001u
#define DXGI_ERROR_NOT_FOUND 0x887a0002u
#define DXGI_ERROR_MORE_DATA 0x887a0003u
#define DXGI_ERROR_UNSUPPORTED 0x887a0004u
#define DXGI_ERROR_SDK_COMPONENT_MISSING 0x887a002du
#define D3D11_ERROR_FILE_NOT_FOUND 0x887c0002u
#define CLASS_NOT_REGISTERED 0x80040154u

#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                                      \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),               \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), a, b, c, d, e, f, g, h}

/* ---- interface ids */

static const uint8_t IID_Unknown[16] = GUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t IID_DeviceChild[16] =
    GUID(0x1841e5c8, 0x16b0, 0x489b, 0xbc, 0xc8, 0x44, 0xcf, 0xb0, 0xd5, 0xde, 0xae);
static const uint8_t IID_Device[16] = GUID(0xdb6f6ddb, 0xac77, 0x4e88, 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40);
static const uint8_t IID_Device1[16] = GUID(0xa04bfb29, 0x08ef, 0x43d6, 0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86);
static const uint8_t IID_Device2[16] = GUID(0x9d06dffa, 0xd1e5, 0x4d07, 0x83, 0xa8, 0x1b, 0xb1, 0x23, 0xf2, 0xf8, 0x41);
static const uint8_t IID_Context[16] = GUID(0xc0bfa96c, 0xe089, 0x44fb, 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda);
static const uint8_t IID_Context1[16] =
    GUID(0xbb2c6faa, 0xb5fb, 0x4082, 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1);
static const uint8_t IID_Context2[16] =
    GUID(0x420d5b32, 0xb90c, 0x4da4, 0xbe, 0xf0, 0x35, 0x9f, 0x6a, 0x24, 0xa8, 0x3a);
static const uint8_t IID_Resource[16] = GUID(0xdc8e63f3, 0xd12b, 0x4952, 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d);
static const uint8_t IID_Buffer[16] = GUID(0x48570b85, 0xd1ee, 0x4fcd, 0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37);
static const uint8_t IID_Texture1D[16] =
    GUID(0xf8fb5c27, 0xc6b3, 0x4f75, 0xa4, 0xc8, 0x43, 0x9a, 0xf2, 0xef, 0x56, 0x4c);
static const uint8_t IID_Texture2D[16] =
    GUID(0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);
static const uint8_t IID_Texture3D[16] =
    GUID(0x037e866e, 0xf56d, 0x4357, 0xa8, 0xaf, 0x9d, 0xab, 0xbe, 0x6e, 0x25, 0x0e);
static const uint8_t IID_View[16] = GUID(0x839d1216, 0xbb2e, 0x412b, 0xb7, 0xf4, 0xa9, 0xdb, 0xeb, 0xe0, 0x8e, 0xd1);
static const uint8_t IID_SRV[16] = GUID(0xb0e06fe0, 0x8192, 0x4e1a, 0xb1, 0xca, 0x36, 0xd7, 0x41, 0x47, 0x10, 0xb2);
static const uint8_t IID_RTV[16] = GUID(0xdfdba067, 0x0b8d, 0x4865, 0x87, 0x5b, 0xd7, 0xb4, 0x51, 0x6c, 0xc1, 0x64);
static const uint8_t IID_DSV[16] = GUID(0x9fdac92a, 0x1876, 0x48c3, 0xaf, 0xad, 0x25, 0xb9, 0x4f, 0x84, 0xa9, 0xb6);
static const uint8_t IID_UAV[16] = GUID(0x28acf509, 0x7f5c, 0x48f6, 0x86, 0x11, 0xf3, 0x16, 0x01, 0x0a, 0x63, 0x80);
static const uint8_t IID_VS[16] = GUID(0x3b301d64, 0xd678, 0x4289, 0x88, 0x97, 0x22, 0xf8, 0x92, 0x8b, 0x72, 0xf3);
static const uint8_t IID_PS[16] = GUID(0xea82e40d, 0x51dc, 0x4f33, 0x93, 0xd4, 0xdb, 0x7c, 0x91, 0x25, 0xae, 0x8c);
static const uint8_t IID_GS[16] = GUID(0x38325b96, 0xeffb, 0x4022, 0xba, 0x02, 0x2e, 0x79, 0x5b, 0x70, 0x27, 0x5c);
static const uint8_t IID_HS[16] = GUID(0x8e5c6061, 0x628a, 0x4c8e, 0x82, 0x64, 0xbb, 0xe4, 0x5c, 0xb3, 0xd5, 0xdd);
static const uint8_t IID_DS[16] = GUID(0xf582c508, 0x0f36, 0x490c, 0x99, 0x77, 0x31, 0xee, 0xce, 0x26, 0x8c, 0xfa);
static const uint8_t IID_CS[16] = GUID(0x4f5b196e, 0xc2bd, 0x495e, 0xbd, 0x01, 0x1f, 0xde, 0xd3, 0x8e, 0x49, 0x69);
static const uint8_t IID_Layout[16] = GUID(0xe4819ddc, 0x4cf0, 0x4025, 0xbd, 0x26, 0x5d, 0xe8, 0x2a, 0x3e, 0x07, 0xb7);
static const uint8_t IID_Blend[16] = GUID(0x75b68faa, 0x347d, 0x4159, 0x8f, 0x45, 0xa0, 0x64, 0x0f, 0x01, 0xcd, 0x9a);
static const uint8_t IID_Depth[16] = GUID(0x03823efb, 0x8d8f, 0x4e1c, 0x9a, 0xa2, 0xf6, 0x4b, 0xb2, 0xcb, 0xfd, 0xf1);
static const uint8_t IID_Raster[16] = GUID(0x9bb4ab81, 0xab1a, 0x4d8f, 0xb5, 0x06, 0xfc, 0x04, 0x20, 0x0b, 0x6e, 0xe7);
static const uint8_t IID_Sampler[16] = GUID(0xda6fea51, 0x564c, 0x4487, 0x98, 0x10, 0xf0, 0xd0, 0xf9, 0xb4, 0xe3, 0xa5);
static const uint8_t IID_Async[16] = GUID(0x4b35d0cd, 0x1e15, 0x4258, 0x9c, 0x98, 0x1b, 0x13, 0x33, 0xf6, 0xdd, 0x3b);
static const uint8_t IID_CommandList[16] = GUID(0xa24bc4d1, 0x769e, 0x43f7, 0x80, 0x13, 0x98, 0xff, 0x56, 0x6c, 0x18, 0xe2);
static const uint8_t IID_Query[16] = GUID(0xd6c00747, 0x87b7, 0x425e, 0xb8, 0x4d, 0x44, 0xd1, 0x08, 0x56, 0x0a, 0xfd);
static const uint8_t IID_Predicate[16] =
    GUID(0x9eb576dd, 0x9f77, 0x4d86, 0x81, 0xaa, 0x8b, 0xab, 0x5f, 0xe4, 0x90, 0xe2);
static const uint8_t IID_Object[16] = GUID(0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e);
static const uint8_t IID_SubObject[16] =
    GUID(0x3d3e0379, 0xf9de, 0x4d58, 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6);
static const uint8_t IID_DXGIDevice[16] =
    GUID(0x54ec77fa, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c);
static const uint8_t IID_DXGIDevice1[16] =
    GUID(0x77db970f, 0x6276, 0x48ba, 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c);
static const uint8_t IID_DXGIDevice2[16] =
    GUID(0x05008617, 0xfbfd, 0x4051, 0xa7, 0x90, 0x14, 0x48, 0x84, 0xb4, 0xf6, 0xa9);
static const uint8_t IID_DXGIDevice3[16] =
    GUID(0x6007896c, 0x3244, 0x4afd, 0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23);
static const uint8_t IID_DXGIResource[16] =
    GUID(0x035f3ab4, 0x482e, 0x4e50, 0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b);
static const uint8_t IID_DXGIResource1[16] =
    GUID(0x30961379, 0x4609, 0x4a41, 0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e, 0xe0, 0xc1);
static const uint8_t IID_Surface[16] = GUID(0xcafcb56c, 0x6ac3, 0x4889, 0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60, 0xec);
static const uint8_t IID_Surface1[16] =
    GUID(0x4ae63092, 0x6327, 0x4c1b, 0x80, 0xae, 0xbf, 0xe1, 0x2e, 0xa3, 0x2b, 0x86);
static const uint8_t IID_Factory[16] = GUID(0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69);
static const uint8_t IID_Factory1[16] =
    GUID(0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87);
static const uint8_t IID_Factory2[16] =
    GUID(0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0);
static const uint8_t IID_Factory3[16] =
    GUID(0x25483823, 0xcd46, 0x4c7d, 0x86, 0xca, 0x47, 0xaa, 0x95, 0xb8, 0x37, 0xbd);
static const uint8_t IID_Factory4[16] =
    GUID(0x1bc6ea02, 0xef36, 0x464f, 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a);
static const uint8_t IID_Factory5[16] =
    GUID(0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d);
static const uint8_t IID_Factory6[16] =
    GUID(0xc1b6694f, 0xff09, 0x44a9, 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17);
static const uint8_t IID_Adapter[16] = GUID(0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0);
static const uint8_t IID_Adapter1[16] =
    GUID(0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05);
static const uint8_t IID_Adapter2[16] =
    GUID(0x0aa1ae0a, 0xfa0e, 0x4b84, 0x86, 0x44, 0xe0, 0x5f, 0xf8, 0xe5, 0xac, 0xb5);
static const uint8_t IID_Adapter3[16] =
    GUID(0x645967a4, 0x1392, 0x4310, 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd);
static const uint8_t IID_Adapter4[16] =
    GUID(0x3c8d99d1, 0x4fbf, 0x4181, 0xa8, 0x2c, 0xaf, 0x66, 0xbf, 0x7b, 0xd2, 0x4e);
static const uint8_t IID_Output[16] = GUID(0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa);
static const uint8_t IID_Output1[16] = GUID(0x00cddea8, 0x939b, 0x4b83, 0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc);
static const uint8_t IID_SwapChain[16] =
    GUID(0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a);
static const uint8_t IID_SwapChain1[16] =
    GUID(0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa);
static const uint8_t IID_SwapChain2[16] =
    GUID(0xa8be2ac4, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7);
static const uint8_t IID_SwapChain3[16] =
    GUID(0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1);

/* ---- objects */

enum kind {
    K_DEVICE, K_CONTEXT, K_BUFFER, K_TEX1D, K_TEX2D, K_TEX3D, K_SRV, K_RTV, K_DSV, K_UAV, K_LAYOUT,
    K_PS, K_VS, K_GS, K_HS, K_DS, K_CS, /* the DXBC program types, in order from K_PS */
    K_BLEND, K_DEPTH, K_RASTER, K_SAMPLER, K_QUERY, K_PREDICATE, K_FACTORY, K_ADAPTER, K_OUTPUT, K_SWAPCHAIN,
    K_CLASSLINKAGE, K_CMDLIST,
};

struct pdata {
    struct pdata *next;
    uint8_t guid[16];
    uint32_t size;
    uint64_t iface; /* SetPrivateDataInterface: a reference held */
    uint8_t data[];
};

struct context;
struct swapchain;
struct call;

/* A COM object as the program sees it: up to three interface pointers (this points at one of vt[]), then ours.
   It lives in the program's memory; what it stands for lives in the kernel's. */
struct cobj {
    uint64_t vt[3];
    int32_t refs, irefs; /* the program's references; the context's and other objects' */
    uint32_t kind;
    struct cobj *device;
    struct cobj *next, *prev;
    struct pdata *priv;
    union {
        struct {
            struct gd_resource *r;
            uint32_t user, user_size; /* where Map shows it in the program's memory */
            uint32_t samples, quality, usage, bind, cpu, misc;
            struct cobj *swapchain;
        } res;
        struct {
            struct gd_view *v;
            struct cobj *res;
            uint8_t desc[44];
        } view;
        struct gd_shader *shader;
        struct gd_input_layout *layout;
        struct {
            uint32_t size;
            uint8_t desc[332]; /* the description as the program gave it, for GetDesc */
            union {
                struct gd_blend blend;
                struct gd_depth depth;
                struct gd_raster raster;
                struct gd_sampler sampler;
            } s;
        } state;
        struct {
            uint32_t type, misc, state; /* state: 0 new, 1 begun, 2 ended */
            uint64_t start, result[11];
        } query;
        struct context *ctx;
        struct swapchain *sc;
        struct call *calls; /* a command list */
        struct {
            uint32_t level, flags;
            struct cobj *context;
        } dev;
    };
};

static W32_STATE struct cobj *live;
static W32_STATE struct cobj *factory_obj, *adapter_obj, *output_obj;

static void put64(uint64_t where, uint64_t v)
{
    if (where)
        memcpy(w64_ptr(where), &v, 8);
}

static void put32(uint64_t where, uint32_t v)
{
    if (where)
        memcpy(w64_ptr(where), &v, 4);
}

static uint64_t get64(uint64_t where)
{
    uint64_t v = 0;
    if (where)
        memcpy(&v, w64_ptr(where), 8);
    return v;
}

static uint32_t get32(uint64_t where)
{
    uint32_t v = 0;
    if (where)
        memcpy(&v, w64_ptr(where), 4);
    return v;
}

static float argf(const uint64_t *f, int i)
{
    float v;
    memcpy(&v, &f[i], 4);
    return v;
}

static uint64_t iface(struct cobj *o, int i)
{
    return o ? (uint64_t)(uintptr_t)&o->vt[i] : 0;
}

/* ---- vtables: every slot a stub that finds the object from this and calls the method by its number */

typedef uint64_t (*method)(struct cobj *o, uint64_t *a, const uint64_t *f);

struct vspec {
    const char *name;
    int index; /* which of vt[] this interface is */
    int count;
    const method *fns;
};

static const struct vspec *spec_of(int s);
enum {
    SP_DEVICE, SP_DXGIDEVICE, SP_CONTEXT, SP_BUFFER, SP_TEX1D, SP_TEX2D, SP_TEX3D, SP_SURFACE, SP_DXGIRES, SP_SRV,
    SP_RTV, SP_DSV, SP_UAV, SP_CHILD, SP_BLEND, SP_DEPTH, SP_RASTER, SP_SAMPLER, SP_QUERY, SP_FACTORY, SP_ADAPTER,
    SP_OUTPUT, SP_SWAPCHAIN, SP_CMDLIST, SP_COUNT
};

static bool deferred(const struct cobj *o);
static uint64_t defer(struct cobj *o, uint32_t slot, method fn, uint64_t *a, const uint64_t *f);

static W32_STATE uint64_t *vtables[SP_COUNT];
static W32_STATE uint8_t missing_seen[SP_COUNT][32];

static uint64_t h_slot(uint64_t *a, const uint64_t *f, void *t)
{
    uint32_t code = (uint32_t)(uintptr_t)t, s = code >> 8, slot = code & 255;
    const struct vspec *v = spec_of((int)s);
    method fn = (int)slot < v->count ? v->fns[slot] : NULL;
    if (!fn) {
        if (slot < 256 && !(missing_seen[s][slot / 8] & (1u << (slot % 8)))) {
            missing_seen[s][slot / 8] |= (uint8_t)(1u << (slot % 8));
            klog("d3d11: %s method %u is not done yet", v->name, slot);
        }
        return E_NOTIMPL;
    }
    struct cobj *o = (struct cobj *)(uintptr_t)(a[0] - (uint64_t)v->index * 8);
    if (s == SP_CONTEXT && deferred(o))
        return defer(o, slot, fn, a, f);
    return fn(o, a, f);
}

static uint64_t vtable(int s)
{
    if (vtables[s])
        return (uintptr_t)vtables[s];
    const struct vspec *v = spec_of(s);
    int n = v->count < 4 ? 4 : v->count;
    uint64_t *t = w32_zalloc(sizeof(uint64_t) * (size_t)n);
    if (!t)
        return 0;
    for (int i = 0; i < n; i++) {
        void *stub = w64_target_stub(h_slot, (void *)(uintptr_t)((uint32_t)s << 8 | (uint32_t)i));
        if (!stub)
            return 0;
        t[i] = (uintptr_t)stub;
    }
    vtables[s] = t;
    return (uintptr_t)t;
}

/* the object behind an interface pointer the program passes, or NULL for one that is not ours */
static struct cobj *unwrap(uint64_t p)
{
    if (!p || p >> 32)
        return NULL;
    uint64_t vt;
    memcpy(&vt, (const void *)(uintptr_t)p, 8);
    for (int s = 0; s < SP_COUNT; s++)
        if (vtables[s] && vt == (uintptr_t)vtables[s])
            return (struct cobj *)(uintptr_t)(p - (uint64_t)spec_of(s)->index * 8);
    return NULL;
}

static struct cobj *unwrap_kind(uint64_t p, uint32_t kind)
{
    struct cobj *o = unwrap(p);
    return o && o->kind == kind ? o : NULL;
}

static bool is_resource(const struct cobj *o)
{
    return o && o->kind >= K_BUFFER && o->kind <= K_TEX3D;
}

static struct cobj *new_obj(uint32_t kind, struct cobj *device, int s0, int s1, int s2)
{
    struct cobj *o = w32_zalloc(sizeof(*o));
    if (!o)
        return NULL;
    int specs[3] = {s0, s1, s2};
    for (int i = 0; i < 3; i++)
        if (specs[i] >= 0 && !(o->vt[i] = vtable(specs[i]))) {
            w32_free(o);
            return NULL;
        }
    o->refs = 1;
    o->kind = kind;
    o->device = device;
    o->next = live;
    if (live)
        live->prev = o;
    live = o;
    return o;
}

static void release_guest(uint64_t iface_ptr)
{
    if (!iface_ptr)
        return;
    uint64_t vt = get64(iface_ptr);
    w64_call(get64(vt + 2 * 8), 1, (uint64_t[]){iface_ptr});
}

static void addref_guest(uint64_t iface_ptr)
{
    if (!iface_ptr)
        return;
    uint64_t vt = get64(iface_ptr);
    w64_call(get64(vt + 1 * 8), 1, (uint64_t[]){iface_ptr});
}

static void destroy(struct cobj *o);
static void calls_free(struct call *c, bool release);

static void hold(struct cobj *o)
{
    if (o)
        __atomic_add_fetch(&o->irefs, 1, __ATOMIC_RELAXED);
}

static void maybe_destroy(struct cobj *o)
{
    if (!__atomic_load_n(&o->refs, __ATOMIC_ACQUIRE) && !__atomic_load_n(&o->irefs, __ATOMIC_ACQUIRE))
        destroy(o);
}

static void drop(struct cobj *o)
{
    if (o && !__atomic_sub_fetch(&o->irefs, 1, __ATOMIC_ACQ_REL) && !__atomic_load_n(&o->refs, __ATOMIC_ACQUIRE))
        destroy(o);
}

/* a binding: holds the new object, lets go of the old */
static void bind(struct cobj **slot, struct cobj *o)
{
    if (*slot == o)
        return;
    hold(o);
    struct cobj *old = *slot;
    *slot = o;
    drop(old);
}

static uint64_t m_addref(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return (uint32_t)__atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}

static uint64_t m_release(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    int32_t n = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    if (n < 0) {
        o->refs = 0;
        return 0;
    }
    if (!n)
        maybe_destroy(o);
    return (uint32_t)n;
}

/* ---- QueryInterface: which interface ids each kind answers to, and on which of its vtables */

struct iid_map {
    const uint8_t *iid;
    int index;
};

static const struct iid_map *iids_of(uint32_t kind);

static uint64_t m_query(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    uint64_t out = a[2];
    if (!out)
        return E_POINTER;
    const uint8_t *iid = w64_ptr(a[1]);
    for (const struct iid_map *m = iids_of(o->kind); m && m->iid; m++)
        if (!memcmp(iid, m->iid, 16)) {
            __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
            put64(out, iface(o, m->index));
            return S_OK;
        }
    put64(out, 0);
    return E_NOINTERFACE;
}

static uint64_t query_out(struct cobj *o, const uint8_t *iid, uint64_t out)
{
    uint64_t a[3] = {iface(o, 0), (uintptr_t)iid, out};
    return m_query(o, a, NULL);
}

/* ---- private data (debug names and the like) */

static void priv_free(struct pdata *p)
{
    release_guest(p->iface);
    w32_free(p);
}

static uint64_t set_private(struct cobj *o, const uint8_t *guid, uint32_t size, uint64_t data, uint64_t iface_ptr)
{
    for (struct pdata **pp = &o->priv; *pp; pp = &(*pp)->next)
        if (!memcmp((*pp)->guid, guid, 16)) {
            struct pdata *old = *pp;
            *pp = old->next;
            priv_free(old);
            break;
        }
    if (!data && !iface_ptr)
        return S_OK;
    struct pdata *p = w32_zalloc(sizeof(*p) + size);
    if (!p)
        return E_OUTOFMEMORY;
    memcpy(p->guid, guid, 16);
    p->size = size;
    if (iface_ptr) {
        p->iface = iface_ptr;
        p->size = 8;
        addref_guest(iface_ptr);
    } else {
        memcpy(p->data, w64_ptr(data), size);
    }
    p->next = o->priv;
    o->priv = p;
    return S_OK;
}

/* D3D11 order: (guid, size, data) for Set and (guid, *size, data) for Get */
static uint64_t m_set_private(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return a[1] ? set_private(o, w64_ptr(a[1]), (uint32_t)a[2], a[3], 0) : E_INVALIDARG;
}

static uint64_t m_set_private_iface(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return a[1] ? set_private(o, w64_ptr(a[1]), 0, 0, a[2]) : E_INVALIDARG;
}

static uint64_t m_get_private(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1] || !a[2])
        return E_INVALIDARG;
    for (struct pdata *p = o->priv; p; p = p->next)
        if (!memcmp(p->guid, w64_ptr(a[1]), 16)) {
            uint32_t room = get32(a[2]);
            put32(a[2], p->size);
            if (!a[3])
                return S_OK;
            if (room < p->size)
                return DXGI_ERROR_MORE_DATA;
            if (p->iface) {
                addref_guest(p->iface);
                put64(a[3], p->iface);
            } else {
                memcpy(w64_ptr(a[3]), p->data, p->size);
            }
            return S_OK;
        }
    put32(a[2], 0);
    return DXGI_ERROR_NOT_FOUND;
}

/* GetDevice(ID3D11Device **) */
static uint64_t m_get_device(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *d = o->kind == K_DEVICE ? o : o->device;
    if (d)
        __atomic_add_fetch(&d->refs, 1, __ATOMIC_RELAXED);
    put64(a[1], iface(d, 0));
    return 0;
}

static uint64_t m_zero(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return 0;
}

static uint64_t m_ok(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return S_OK;
}

static uint64_t m_one(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return 1;
}

static uint64_t m_unsupported(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return DXGI_ERROR_UNSUPPORTED;
}

static uint64_t m_notimpl(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return E_NOTIMPL;
}

#define IUNKNOWN [0] = m_query, [1] = m_addref, [2] = m_release
/* ID3D11DeviceChild: GetDevice, GetPrivateData, SetPrivateData, SetPrivateDataInterface */
#define DEVICE_CHILD IUNKNOWN, [3] = m_get_device, [4] = m_get_private, [5] = m_set_private, [6] = m_set_private_iface
/* IDXGIObject: SetPrivateData, SetPrivateDataInterface, GetPrivateData, GetParent */
#define DXGI_OBJECT IUNKNOWN, [3] = m_set_private, [4] = m_set_private_iface, [5] = m_get_private

/* ---- the immediate context's bindings, kept for the Get methods; every one holds its object */

#define STAGES 6

struct context {
    struct gd_context *gd;
    struct cobj *shader[STAGES];
    struct cobj *cb[STAGES][GD_SLOTS_CB];
    uint32_t cb_first[STAGES][GD_SLOTS_CB], cb_count[STAGES][GD_SLOTS_CB];
    struct cobj *srv[STAGES][GD_SLOTS_SRV];
    struct cobj *sampler[STAGES][GD_SLOTS_SAMPLER];
    struct cobj *vb[GD_SLOTS_VB];
    uint32_t vb_stride[GD_SLOTS_VB], vb_offset[GD_SLOTS_VB];
    struct cobj *ib;
    uint32_t ib_format, ib_offset;
    struct cobj *layout;
    uint32_t topology;
    struct cobj *rtv[8], *dsv, *uav[8], *csuav[8];
    struct cobj *so[GD_SLOTS_SO]; /* stream output buffers */
    struct cobj *blend, *depth, *raster;
    float blend_factor[4];
    uint32_t sample_mask, stencil_ref;
    struct gd_viewport vp[16];
    struct gd_rect sc[16];
    uint32_t nvp, nsc;
    struct cobj *predicate;
    uint32_t predicate_value;
    bool deferred;            /* a deferred context: it keeps its calls for a command list */
    struct call *first, *last; /* what it kept so far */
};

struct swapchain {
    HANDLE window;
    struct cobj *buffer;
    uint32_t width, height, format, count, usage, flags, effect, scaling, alpha, windowed;
    uint32_t presents;
    uint64_t last_present;
    bool scanout; /* full screen: the screen shows the back buffer straight from the host's graphics card */
};

static void context_unbind_all(struct context *c)
{
    for (int s = 0; s < STAGES; s++) {
        bind(&c->shader[s], NULL);
        for (int i = 0; i < GD_SLOTS_CB; i++)
            bind(&c->cb[s][i], NULL);
        for (int i = 0; i < GD_SLOTS_SRV; i++)
            bind(&c->srv[s][i], NULL);
        for (int i = 0; i < GD_SLOTS_SAMPLER; i++)
            bind(&c->sampler[s][i], NULL);
    }
    for (int i = 0; i < GD_SLOTS_VB; i++)
        bind(&c->vb[i], NULL);
    for (int i = 0; i < 8; i++) {
        bind(&c->rtv[i], NULL);
        bind(&c->uav[i], NULL);
        bind(&c->csuav[i], NULL);
    }
    for (int i = 0; i < GD_SLOTS_SO; i++)
        bind(&c->so[i], NULL);
    bind(&c->ib, NULL);
    bind(&c->layout, NULL);
    bind(&c->dsv, NULL);
    bind(&c->blend, NULL);
    bind(&c->depth, NULL);
    bind(&c->raster, NULL);
    bind(&c->predicate, NULL);
    memset(c->vb_stride, 0, sizeof(c->vb_stride));
    memset(c->vb_offset, 0, sizeof(c->vb_offset));
    memset(c->cb_first, 0, sizeof(c->cb_first));
    memset(c->cb_count, 0, sizeof(c->cb_count));
    c->ib_format = c->ib_offset = c->topology = 0;
    for (int i = 0; i < 4; i++)
        c->blend_factor[i] = 1.0f;
    c->sample_mask = 0xffffffffu;
    c->stencil_ref = 0;
    c->nvp = c->nsc = 0;
    c->predicate_value = 0;
}

static void unmap_user(struct cobj *o)
{
    if (o->res.user) {
        w32_vm_unmap_shared(o->res.user);
        o->res.user = 0;
    }
}

static void destroy(struct cobj *o)
{
    if (o->kind == K_DEVICE || o->kind == K_FACTORY || o->kind == K_ADAPTER || o->kind == K_OUTPUT)
        return; /* they live as long as the process */
    o->refs = o->irefs = 1; /* nothing below may come back here */
    while (o->priv) {
        struct pdata *p = o->priv;
        o->priv = p->next;
        priv_free(p);
    }
    switch (o->kind) {
    case K_CONTEXT:
        if (o->ctx) {
            calls_free(o->ctx->first, true);
            context_unbind_all(o->ctx);
            gd_context_destroy(o->ctx->gd);
            kfree(o->ctx);
        }
        break;
    case K_CMDLIST:
        calls_free(o->calls, true);
        break;
    case K_BUFFER:
    case K_TEX1D:
    case K_TEX2D:
    case K_TEX3D:
        unmap_user(o);
        gd_resource_unref(o->res.r);
        break;
    case K_SRV:
    case K_RTV:
    case K_DSV:
    case K_UAV:
        gd_view_unref(o->view.v);
        drop(o->view.res);
        break;
    case K_PS:
    case K_VS:
    case K_GS:
    case K_HS:
    case K_DS:
    case K_CS:
        gd_shader_unref(o->shader);
        break;
    case K_LAYOUT:
        kfree(o->layout);
        break;
    case K_SWAPCHAIN:
        if (o->sc) {
            if (o->sc->scanout)
                gd_scanout(NULL, 0, 0);
            drop(o->sc->buffer);
            kfree(o->sc);
        }
        break;
    default:
        break;
    }
    if (o->device && o->kind != K_CONTEXT)
        drop(o->device);
    if (o->prev)
        o->prev->next = o->next;
    else
        live = o->next;
    if (o->next)
        o->next->prev = o->prev;
    w32_free(o);
}

/* the process is ending: every object goes, with what it holds in the kernel */
void w32_d3d_cleanup(void)
{
    for (struct cobj *o = live; o; o = o->next)
        if (o->kind == K_CONTEXT && o->ctx)
            context_unbind_all(o->ctx);
    while (live) {
        struct cobj *o = live;
        live = o->next;
        switch (o->kind) {
        case K_CONTEXT:
            if (o->ctx) {
                calls_free(o->ctx->first, false);
                gd_context_destroy(o->ctx->gd);
                kfree(o->ctx);
            }
            break;
        case K_CMDLIST:
            calls_free(o->calls, false);
            break;
        case K_BUFFER:
        case K_TEX1D:
        case K_TEX2D:
        case K_TEX3D:
            gd_resource_unref(o->res.r);
            break;
        case K_SRV:
        case K_RTV:
        case K_DSV:
        case K_UAV:
            gd_view_unref(o->view.v);
            break;
        case K_PS:
        case K_VS:
        case K_GS:
        case K_HS:
        case K_DS:
        case K_CS:
            gd_shader_unref(o->shader);
            break;
        case K_LAYOUT:
            kfree(o->layout);
            break;
        case K_SWAPCHAIN:
            if (o->sc && o->sc->scanout)
                gd_scanout(NULL, 0, 0);
            kfree(o->sc);
            break;
        default:
            break;
        }
    }
    factory_obj = adapter_obj = output_obj = NULL;
}

/* a new object for the program: one reference, and one on the device it came from */
static struct cobj *child(uint32_t kind, struct cobj *dev, int s0, int s1, int s2)
{
    struct cobj *o = new_obj(kind, dev, s0, s1, s2);
    if (o && dev)
        hold(dev);
    return o;
}

/* ---- resources */

static uint32_t mip_count(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t m = 1, x = MAX(w, MAX(h, d));
    while (x > 1)
        x >>= 1, m++;
    return m;
}

/* where the program sees subresource sub (Map): the resource's pages, put in its memory the first time */
static uint64_t map_sub(struct cobj *o, uint32_t sub, struct gd_layout *l)
{
    uint8_t *p = gd_subresource(o->res.r, sub, l);
    if (!p)
        return 0;
    uint32_t bytes;
    uint8_t *mem = gd_resource_memory(o->res.r, &bytes);
    if (!o->res.user) {
        uint32_t n = bytes / 4096u;
        uint8_t **pages = kmalloc(sizeof(*pages) * n);
        if (!pages)
            return 0;
        for (uint32_t i = 0; i < n; i++)
            pages[i] = mem + (size_t)i * 4096u;
        o->res.user = w32_vm_map_shared(pages, n, 0);
        o->res.user_size = bytes;
        kfree(pages);
        if (!o->res.user)
            return 0;
    }
    return o->res.user + (uint32_t)(p - mem);
}

static uint64_t create_resource(struct cobj *dev, uint32_t kind, struct gd_res_desc *d, uint64_t init, uint64_t out)
{
    uint32_t samples = d->samples ? d->samples : 1;
    d->samples = 1; /* multisampling is drawn with one sample */
    if (d->dim != GD_BUFFER && !d->mips)
        d->mips = mip_count(d->width, d->height, d->dim == GD_TEX3D ? d->depth : 1);
    uint32_t subs = d->dim == GD_BUFFER ? 1 : d->mips * (d->dim == GD_TEX3D ? 1 : MAX(d->array, 1u));
    struct gd_init *in = NULL;
    if (init) {
        in = kzalloc(sizeof(*in) * subs);
        if (!in)
            return E_OUTOFMEMORY;
        for (uint32_t i = 0; i < subs; i++) {
            uint64_t data = get64(init + i * 16ull);
            in[i].data = data ? (uintptr_t)w64_ptr(data) : 0;
            in[i].row_pitch = get32(init + i * 16ull + 8);
            in[i].slice_pitch = get32(init + i * 16ull + 12);
        }
    }
    uint32_t error = 0;
    struct gd_resource *r = gd_resource_create(d, in, &error);
    kfree(in);
    if (!r)
        return error ? error : E_INVALIDARG;
    if (!out) {
        gd_resource_unref(r);
        return S_FALSE;
    }
    int sp = kind == K_BUFFER ? SP_BUFFER : kind == K_TEX1D ? SP_TEX1D : kind == K_TEX2D ? SP_TEX2D : SP_TEX3D;
    struct cobj *o = child(kind, dev, sp, kind == K_BUFFER ? -1 : SP_SURFACE, SP_DXGIRES);
    if (!o) {
        gd_resource_unref(r);
        return E_OUTOFMEMORY;
    }
    o->res.r = r;
    o->res.samples = samples;
    put64(out, iface(o, 0));
    return S_OK;
}

/* CreateBuffer(desc, init, out) */
static uint64_t m_create_buffer(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    const uint32_t *b = w64_ptr(a[1]);
    struct gd_res_desc d = {.dim = GD_BUFFER, .width = b[0], .usage = b[1], .bind = b[2], .cpu = b[3], .misc = b[4],
                            .stride = b[5]};
    put64(a[3], 0);
    return create_resource(dev, K_BUFFER, &d, a[2], a[3]);
}

static uint64_t m_create_tex1d(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    const uint32_t *t = w64_ptr(a[1]);
    struct gd_res_desc d = {.dim = GD_TEX1D, .width = t[0], .mips = t[1], .array = t[2], .format = t[3],
                            .usage = t[4], .bind = t[5], .cpu = t[6], .misc = t[7]};
    put64(a[3], 0);
    return create_resource(dev, K_TEX1D, &d, a[2], a[3]);
}

static uint64_t m_create_tex2d(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    const uint32_t *t = w64_ptr(a[1]);
    struct gd_res_desc d = {.dim = GD_TEX2D, .width = t[0], .height = t[1], .mips = t[2], .array = t[3],
                            .format = t[4], .samples = t[5], .usage = t[7], .bind = t[8], .cpu = t[9], .misc = t[10]};
    put64(a[3], 0);
    return create_resource(dev, K_TEX2D, &d, a[2], a[3]);
}

static uint64_t m_create_tex3d(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    const uint32_t *t = w64_ptr(a[1]);
    struct gd_res_desc d = {.dim = GD_TEX3D, .width = t[0], .height = t[1], .depth = t[2], .mips = t[3],
                            .format = t[4], .usage = t[5], .bind = t[6], .cpu = t[7], .misc = t[8]};
    put64(a[3], 0);
    return create_resource(dev, K_TEX3D, &d, a[2], a[3]);
}

/* ID3D11Resource: GetType, SetEvictionPriority, GetEvictionPriority, GetDesc */
static uint64_t m_res_type(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->kind - K_BUFFER + 1);
    return 0;
}

static uint64_t m_res_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct gd_res_desc d;
    gd_resource_desc(o->res.r, &d);
    uint32_t v[11];
    int n;
    switch (o->kind) {
    case K_BUFFER:
        n = 6, v[0] = d.width, v[1] = d.usage, v[2] = d.bind, v[3] = d.cpu, v[4] = d.misc, v[5] = d.stride;
        break;
    case K_TEX1D:
        n = 8, v[0] = d.width, v[1] = d.mips, v[2] = d.array, v[3] = d.format, v[4] = d.usage, v[5] = d.bind,
        v[6] = d.cpu, v[7] = d.misc;
        break;
    case K_TEX2D:
        n = 11, v[0] = d.width, v[1] = d.height, v[2] = d.mips, v[3] = d.array, v[4] = d.format,
        v[5] = o->res.samples, v[6] = 0, v[7] = d.usage, v[8] = d.bind, v[9] = d.cpu, v[10] = d.misc;
        break;
    default:
        n = 9, v[0] = d.width, v[1] = d.height, v[2] = d.depth, v[3] = d.mips, v[4] = d.format, v[5] = d.usage,
        v[6] = d.bind, v[7] = d.cpu, v[8] = d.misc;
        break;
    }
    if (a[1])
        memcpy(w64_ptr(a[1]), v, (size_t)n * 4);
    return 0;
}

/* IDXGISurface1 and IDXGIResource1 on a resource */
static uint64_t m_dxgi_parent(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!o->device) {
        put64(a[2], 0);
        return E_NOINTERFACE;
    }
    return query_out(o->device, w64_ptr(a[1]), a[2]);
}

static uint64_t m_surface_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct gd_res_desc d;
    gd_resource_desc(o->res.r, &d);
    uint32_t v[5] = {d.width, d.height, d.format, o->res.samples, 0};
    if (a[1])
        memcpy(w64_ptr(a[1]), v, sizeof(v));
    return S_OK;
}

static uint64_t m_surface_map(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct gd_layout l;
    uint64_t p = map_sub(o, 0, &l);
    if (!p)
        return E_OUTOFMEMORY;
    put32(a[1], l.row_pitch);
    put64(a[1] + 8, p);
    return S_OK;
}

static uint64_t m_shared_handle(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[1], 0);
    return E_NOTIMPL;
}

static uint64_t m_usage(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct gd_res_desc d;
    gd_resource_desc(o->res.r, &d);
    uint32_t u = (d.bind & GD_BIND_SHADER_RESOURCE ? 0x10u : 0) | (d.bind & GD_BIND_RENDER_TARGET ? 0x20u : 0) |
                 (o->res.swapchain ? 0x40u : 0);
    put32(a[1], u);
    return S_OK;
}

static uint64_t m_eviction(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0x50000000u); /* DXGI_RESOURCE_PRIORITY_NORMAL */
    return S_OK;
}

static uint64_t m_eviction_value(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return 0x50000000u;
}

/* ---- views */

static const uint32_t view_desc_size[] = {[K_SRV] = 24, [K_RTV] = 20, [K_DSV] = 24, [K_UAV] = 20};

/* the D3D11 view description from what the view looks at */
static void view_desc_out(uint32_t kind, const struct gd_view_desc *g, uint32_t *v)
{
    memset(v, 0, 24);
    uint32_t dim = g->dim;
    v[0] = g->format;
    uint32_t *u = v + 2;
    if (kind == K_DSV) {
        v[1] = dim >= 2 && dim <= 7 ? dim - 1 : 3;
        v[2] = g->flags;
        u = v + 3;
    } else {
        v[1] = dim;
    }
    bool mips = kind == K_SRV;
    switch (dim) {
    case 1: /* buffer */
        u[0] = g->first_element, u[1] = g->elements;
        break;
    case 11:
        u[0] = g->first_element, u[1] = g->elements, u[2] = g->flags;
        break;
    case 2:
    case 4:
    case 9:
        u[0] = g->first_mip, u[1] = mips ? g->mips : 0;
        break;
    case 8:
        u[0] = g->first_mip;
        if (mips)
            u[1] = g->mips;
        else
            u[1] = g->first_slice, u[2] = g->slices;
        break;
    case 3:
    case 5:
        if (mips)
            u[0] = g->first_mip, u[1] = g->mips, u[2] = g->first_slice, u[3] = g->slices;
        else
            u[0] = g->first_mip, u[1] = g->first_slice, u[2] = g->slices;
        break;
    case 7:
        u[0] = g->first_slice, u[1] = g->slices;
        break;
    case 10:
        u[0] = g->first_mip, u[1] = g->mips, u[2] = g->first_slice, u[3] = g->slices / 6;
        break;
    default:
        break;
    }
}

/* the program's view description as the device takes it; false for one that is not allowed */
static bool view_desc_in(uint32_t kind, const uint32_t *v, struct gd_view_desc *g)
{
    uint32_t dim = v[1];
    const uint32_t *u = v + 2;
    g->format = v[0];
    if (kind == K_DSV) {
        g->flags = v[2];
        u = v + 3;
        if (!dim || dim > 6)
            return false;
        dim++; /* to the SRV numbering */
    }
    g->dim = dim;
    bool srv = kind == K_SRV;
    uint32_t all = 0xffffffffu;
    switch (dim) {
    case 1:
        g->first_element = u[0], g->elements = u[1];
        if (kind == K_UAV)
            g->flags = u[2];
        break;
    case 11:
        if (!srv)
            return false;
        g->first_element = u[0], g->elements = u[1], g->flags = u[2];
        break;
    case 2:
    case 4:
    case 9:
        if (dim == 9 && !srv)
            return false;
        g->first_mip = u[0], g->mips = srv ? (u[1] == all ? 0 : u[1]) : 1;
        break;
    case 8:
        g->first_mip = u[0];
        if (srv)
            g->mips = u[1] == all ? 0 : u[1];
        else
            g->mips = 1, g->first_slice = u[1], g->slices = u[2] == all ? 0 : u[2];
        break;
    case 3:
    case 5:
        if (srv)
            g->first_mip = u[0], g->mips = u[1] == all ? 0 : u[1], g->first_slice = u[2], g->slices = u[3];
        else
            g->first_mip = u[0], g->mips = 1, g->first_slice = u[1], g->slices = u[2];
        break;
    case 6:
        g->mips = 1;
        break;
    case 7:
        g->mips = 1, g->first_slice = u[0], g->slices = u[1];
        break;
    case 10:
        if (!srv)
            return false;
        g->first_mip = u[0], g->mips = u[1] == all ? 0 : u[1], g->first_slice = u[2], g->slices = u[3] * 6;
        break;
    default:
        return false;
    }
    if (dim == 6 || dim == 7) /* multisampled: one sample here, so a plain 2D view */
        g->dim = dim == 6 ? 4 : 5;
    return true;
}

/* Create*View(resource, desc, out) */
static uint64_t create_view(struct cobj *dev, uint32_t kind, uint64_t *a)
{
    put64(a[3], 0);
    struct cobj *res = unwrap(a[1]);
    if (!is_resource(res))
        return E_INVALIDARG;
    static const uint32_t gk[] = {[K_SRV] = GD_VIEW_SRV, [K_RTV] = GD_VIEW_RTV, [K_DSV] = GD_VIEW_DSV,
                                  [K_UAV] = GD_VIEW_UAV};
    struct gd_view_desc g = {.kind = gk[kind]};
    if (a[2] && !view_desc_in(kind, w64_ptr(a[2]), &g))
        return E_INVALIDARG;
    if (!a[2] && kind != K_SRV)
        g.mips = 1;
    uint32_t error = 0;
    struct gd_view *v = gd_view_create(res->res.r, &g, &error);
    if (!v)
        return error ? error : E_INVALIDARG;
    if (!a[3]) {
        gd_view_unref(v);
        return S_FALSE;
    }
    static const int sp[] = {[K_SRV] = SP_SRV, [K_RTV] = SP_RTV, [K_DSV] = SP_DSV, [K_UAV] = SP_UAV};
    struct cobj *o = child(kind, dev, sp[kind], -1, -1);
    if (!o) {
        gd_view_unref(v);
        return E_OUTOFMEMORY;
    }
    o->view.v = v;
    o->view.res = res;
    hold(res);
    struct gd_view_desc filled;
    gd_view_desc(v, &filled);
    if (!filled.format)
        filled.format = g.format;
    view_desc_out(kind, &filled, (uint32_t *)o->view.desc);
    put64(a[3], iface(o, 0));
    return S_OK;
}

static uint64_t m_create_srv(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_view(dev, K_SRV, a);
}

static uint64_t m_create_uav(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_view(dev, K_UAV, a);
}

static uint64_t m_create_rtv(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_view(dev, K_RTV, a);
}

static uint64_t m_create_dsv(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_view(dev, K_DSV, a);
}

/* ID3D11View: GetResource, GetDesc */
static uint64_t m_view_resource(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    __atomic_add_fetch(&o->view.res->refs, 1, __ATOMIC_RELAXED);
    put64(a[1], iface(o->view.res, 0));
    return 0;
}

static uint64_t m_view_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1])
        memcpy(w64_ptr(a[1]), o->view.desc, view_desc_size[o->kind]);
    return 0;
}

/* ---- shaders and input layouts */

static uint64_t create_shader(struct cobj *dev, uint32_t kind, uint64_t blob, uint64_t size, uint64_t out)
{
    put64(out, 0);
    if (!blob || !size)
        return E_INVALIDARG;
    char error[64] = "";
    struct gd_shader *s = gd_shader_create(w64_ptr(blob), (uint32_t)size, error);
    if (!s) {
        klog("d3d11: shader not taken: %s", error);
        return E_INVALIDARG;
    }
    if (gd_shader_stage(s) != (int)(kind - K_PS)) {
        gd_shader_unref(s);
        return E_INVALIDARG;
    }
    if (!out) {
        gd_shader_unref(s);
        return S_FALSE;
    }
    struct cobj *o = child(kind, dev, SP_CHILD, -1, -1);
    if (!o) {
        gd_shader_unref(s);
        return E_OUTOFMEMORY;
    }
    o->shader = s;
    put64(out, iface(o, 0));
    return S_OK;
}

/* Create*Shader(bytecode, length, class linkage, out) */
static uint64_t m_create_vs(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_shader(dev, K_VS, a[1], a[2], a[4]);
}

static uint64_t m_create_gs(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_shader(dev, K_GS, a[1], a[2], a[4]);
}

/* CreateGeometryShaderWithStreamOutput(bytecode, length, entries, count, strides, n, stream, linkage, out): the
   bytecode is a geometry shader, or a vertex shader whose outputs are streamed out with no geometry shader */
static uint64_t m_create_gs_so(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    put64(a[9], 0);
    uint32_t count = (uint32_t)a[4], nstrides = (uint32_t)a[6];
    if (count > 64 || (count && !a[3]) || nstrides > GD_SLOTS_SO || (nstrides && !a[5]))
        return E_INVALIDARG;
    if (!a[1]) { /* stream output alone, without a shader */
        struct cobj *o = child(K_GS, dev, SP_CHILD, -1, -1);
        put64(a[9], iface(o, 0));
        return o ? S_OK : E_OUTOFMEMORY;
    }
    char error[64] = "";
    struct gd_shader *s = gd_shader_create(w64_ptr(a[1]), (uint32_t)a[2], error);
    if (!s || (gd_shader_stage(s) != 2 && gd_shader_stage(s) != 1)) { /* geometry or vertex */
        klog("d3d11: stream output shader not taken: %s", error);
        gd_shader_unref(s);
        return E_INVALIDARG;
    }
    struct gd_so_entry *e = kzalloc(sizeof(*e) * (count ? count : 1));
    uint32_t strides[GD_SLOTS_SO];
    if (!e) {
        gd_shader_unref(s);
        return E_OUTOFMEMORY;
    }
    for (uint32_t i = 0; i < count; i++) { /* D3D11_SO_DECLARATION_ENTRY: 24 bytes */
        uint64_t p = a[3] + i * 24ull, name = get64(p + 8);
        e[i].stream = get32(p);
        str_copy(e[i].name, name ? w64_ptr(name) : "", sizeof(e[i].name));
        e[i].index = get32(p + 16);
        uint32_t packed = get32(p + 20);
        e[i].start = (uint8_t)packed, e[i].count = (uint8_t)(packed >> 8), e[i].slot = (uint8_t)(packed >> 16);
    }
    for (uint32_t i = 0; i < nstrides; i++)
        strides[i] = get32(a[5] + i * 4ull);
    bool ok = gd_shader_set_so(s, e, count, strides, nstrides, (uint32_t)a[7]);
    kfree(e);
    if (!ok) {
        klog("d3d11: stream output declaration not taken");
        gd_shader_unref(s);
        return E_INVALIDARG;
    }
    if (!a[9]) {
        gd_shader_unref(s);
        return S_FALSE;
    }
    struct cobj *o = child(K_GS, dev, SP_CHILD, -1, -1);
    if (!o) {
        gd_shader_unref(s);
        return E_OUTOFMEMORY;
    }
    o->shader = s;
    put64(a[9], iface(o, 0));
    return S_OK;
}

static uint64_t m_create_ps(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_shader(dev, K_PS, a[1], a[2], a[4]);
}

static uint64_t m_create_hs(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_shader(dev, K_HS, a[1], a[2], a[4]);
}

static uint64_t m_create_ds(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_shader(dev, K_DS, a[1], a[2], a[4]);
}

static uint64_t m_create_cs(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_shader(dev, K_CS, a[1], a[2], a[4]);
}

static uint64_t m_create_linkage(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    struct cobj *o = child(K_CLASSLINKAGE, dev, SP_CHILD, -1, -1);
    put64(a[1], iface(o, 0));
    return o ? S_OK : E_OUTOFMEMORY;
}

/* CreateInputLayout(elements, count, bytecode, length, out) */
static uint64_t m_create_layout(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    put64(a[5], 0);
    uint32_t count = (uint32_t)a[2];
    if (count > GD_MAX_ELEMENTS || (count && !a[1]))
        return E_INVALIDARG;
    struct gd_input_layout *l = kzalloc(sizeof(*l));
    if (!l)
        return E_OUTOFMEMORY;
    l->count = count;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t e = a[1] + i * 32ull;
        uint64_t name = get64(e);
        const char *s = name ? w64_ptr(name) : "";
        str_copy(l->e[i].name, s, sizeof(l->e[i].name));
        l->e[i].index = get32(e + 8);
        l->e[i].format = get32(e + 12);
        l->e[i].slot = get32(e + 16);
        l->e[i].offset = get32(e + 20);
        l->e[i].per_instance = get32(e + 24);
        l->e[i].step = get32(e + 28);
        if (l->e[i].slot >= GD_SLOTS_VB) {
            kfree(l);
            return E_INVALIDARG;
        }
    }
    if (!a[5]) {
        kfree(l);
        return S_FALSE;
    }
    struct cobj *o = child(K_LAYOUT, dev, SP_CHILD, -1, -1);
    if (!o) {
        kfree(l);
        return E_OUTOFMEMORY;
    }
    o->layout = l;
    put64(a[5], iface(o, 0));
    return S_OK;
}

/* ---- pipeline states */

static struct cobj *new_state(struct cobj *dev, uint32_t kind, int sp, const void *desc, uint32_t size)
{
    struct cobj *o = child(kind, dev, sp, -1, -1);
    if (o) {
        o->state.size = size;
        memcpy(o->state.desc, desc, size);
    }
    return o;
}

static void blend_target_in(struct gd_blend_target *t, const uint32_t *v, bool v1)
{
    if (v1) /* BlendEnable, LogicOpEnable, then as before with LogicOp before the mask */
        *t = (struct gd_blend_target){v[0], v[2], v[3], v[4], v[5], v[6], v[7], v[9] & 0xff};
    else
        *t = (struct gd_blend_target){v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] & 0xff};
}

static uint64_t create_blend(struct cobj *dev, uint64_t *a, bool v1)
{
    put64(a[2], 0);
    if (!a[1])
        return E_INVALIDARG;
    const uint32_t *v = w64_ptr(a[1]);
    uint32_t per = v1 ? 10 : 8;
    struct gd_blend b = {.alpha_to_coverage = v[0], .independent = v[1]};
    for (int i = 0; i < 8; i++)
        blend_target_in(&b.rt[i], v + 2 + (b.independent ? i : 0) * per, v1);
    if (!a[2])
        return S_FALSE;
    struct cobj *o = new_state(dev, K_BLEND, SP_BLEND, v, (2 + 8 * per) * 4);
    if (!o)
        return E_OUTOFMEMORY;
    o->state.s.blend = b;
    put64(a[2], iface(o, 0));
    return S_OK;
}

static uint64_t m_create_blend(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_blend(dev, a, false);
}

static uint64_t m_create_blend1(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_blend(dev, a, true);
}

/* GetDesc and GetDesc1 of a blend state, whichever way it was made */
static void blend_desc_out(const struct gd_blend *b, uint64_t out, bool v1)
{
    if (!out)
        return;
    uint32_t v[82] = {b->alpha_to_coverage, b->independent};
    uint32_t per = v1 ? 10 : 8;
    for (int i = 0; i < 8; i++) {
        const struct gd_blend_target *t = &b->rt[i];
        uint32_t *o = v + 2 + i * per;
        if (v1)
            o[0] = t->enable, o[1] = 0, o[2] = t->src, o[3] = t->dst, o[4] = t->op, o[5] = t->src_alpha,
            o[6] = t->dst_alpha, o[7] = t->op_alpha, o[8] = 1, o[9] = t->write_mask;
        else
            o[0] = t->enable, o[1] = t->src, o[2] = t->dst, o[3] = t->op, o[4] = t->src_alpha, o[5] = t->dst_alpha,
            o[6] = t->op_alpha, o[7] = t->write_mask;
    }
    memcpy(w64_ptr(out), v, (2 + 8 * per) * 4);
}

static uint64_t m_blend_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    blend_desc_out(&o->state.s.blend, a[1], false);
    return 0;
}

static uint64_t m_blend_desc1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    blend_desc_out(&o->state.s.blend, a[1], true);
    return 0;
}

static uint64_t m_create_depth(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    put64(a[2], 0);
    if (!a[1])
        return E_INVALIDARG;
    const uint8_t *p = w64_ptr(a[1]);
    uint32_t v[13];
    memcpy(v, p, sizeof(v));
    struct gd_depth d = {.depth_enable = v[0], .write_mask = v[1], .func = v[2], .stencil_enable = v[3],
                         .read_mask = p[16], .write_mask_stencil = p[17]};
    memcpy(&d.front, &v[5], 16);
    memcpy(&d.back, &v[9], 16);
    if (!a[2])
        return S_FALSE;
    struct cobj *o = new_state(dev, K_DEPTH, SP_DEPTH, p, 52);
    if (!o)
        return E_OUTOFMEMORY;
    o->state.s.depth = d;
    put64(a[2], iface(o, 0));
    return S_OK;
}

/* CreateRasterizerState, 1 and 2: the same fields first */
static uint64_t m_create_raster(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    put64(a[2], 0);
    if (!a[1])
        return E_INVALIDARG;
    if (!a[2])
        return S_FALSE;
    struct cobj *o = new_state(dev, K_RASTER, SP_RASTER, w64_ptr(a[1]), 40);
    if (!o)
        return E_OUTOFMEMORY;
    memcpy(&o->state.s.raster, o->state.desc, sizeof(struct gd_raster));
    put64(a[2], iface(o, 0));
    return S_OK;
}

static uint64_t m_create_sampler(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    put64(a[2], 0);
    if (!a[1])
        return E_INVALIDARG;
    if (!a[2])
        return S_FALSE;
    struct cobj *o = new_state(dev, K_SAMPLER, SP_SAMPLER, w64_ptr(a[1]), 52);
    if (!o)
        return E_OUTOFMEMORY;
    memcpy(&o->state.s.sampler, o->state.desc, sizeof(struct gd_sampler));
    put64(a[2], iface(o, 0));
    return S_OK;
}

/* GetDesc of the other states: what the program gave */
static uint64_t m_state_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1]) {
        memset(w64_ptr(a[1]), 0, o->kind == K_RASTER ? 40 : o->state.size);
        memcpy(w64_ptr(a[1]), o->state.desc, o->state.size);
    }
    return 0;
}

/* ---- queries */

static uint32_t query_size(uint32_t type)
{
    static const uint32_t sizes[] = {4, 8, 8, 16, 88, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4};
    return type < ARRAY_SIZE(sizes) ? sizes[type] : 0;
}

static uint64_t create_query(struct cobj *dev, uint64_t *a, uint32_t kind)
{
    put64(a[2], 0);
    if (!a[1])
        return E_INVALIDARG;
    uint32_t type = get32(a[1]), misc = get32(a[1] + 4);
    if (!query_size(type) || (kind == K_PREDICATE && type != 5 && type != 7))
        return E_INVALIDARG;
    if (!a[2])
        return S_FALSE;
    struct cobj *o = child(kind, dev, SP_QUERY, -1, -1);
    if (!o)
        return E_OUTOFMEMORY;
    o->query.type = type;
    o->query.misc = misc;
    put64(a[2], iface(o, 0));
    return S_OK;
}

static uint64_t m_create_query(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_query(dev, a, K_QUERY);
}

static uint64_t m_create_predicate(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return create_query(dev, a, K_PREDICATE);
}

static uint64_t m_query_size(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return query_size(o->query.type);
}

static uint64_t m_query_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->query.type);
    put32(a[1] + 4, o->query.misc);
    return 0;
}

/* ---- the device: what the hardware can do */

static uint64_t m_format_support(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    struct gd_format fm;
    uint32_t s = gd_format((uint32_t)a[1], &fm) ? fm.support : 0;
    put32(a[2], s);
    return s ? S_OK : E_FAIL;
}

/* multisampled targets are drawn with one sample, so a program that asks for 2, 4 or 8 gets them */
static uint64_t m_ms_levels(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    struct gd_format fm;
    uint32_t count = (uint32_t)a[2], levels = 0;
    if (gd_format((uint32_t)a[1], &fm) && (count == 1 || ((count == 2 || count == 4 || count == 8) &&
                                                          fm.support & (GD_SUPPORT_RENDER_TARGET | GD_SUPPORT_DEPTH_STENCIL))))
        levels = 1;
    put32(a[3], levels);
    return S_OK;
}

static uint64_t m_ms_levels1(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    uint64_t b[4] = {a[0], a[1], a[2], a[4]};
    return m_ms_levels(dev, b, f);
}

static uint64_t m_counter_info(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    if (a[1])
        memset(w64_ptr(a[1]), 0, 12);
    return 0;
}

/* CheckFeatureSupport(feature, data, size) */
static uint64_t m_feature_support(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    uint32_t feature = (uint32_t)a[1], size = (uint32_t)a[3];
    if (!a[2] || feature > 40)
        return E_INVALIDARG;
    uint32_t *d = w64_ptr(a[2]);
    uint32_t keep = feature == 2 || feature == 3 ? d[0] : 0;
    memset(d, 0, size);
    switch (feature) {
    case 0: /* threading: objects may be made on any thread; command lists are the program's runtime's */
        if (size >= 4)
            d[0] = 1;
        break;
    case 2: /* format support */
    case 3: {
        struct gd_format fm;
        if (size < 8)
            return E_INVALIDARG;
        d[0] = keep;
        d[1] = feature == 2 && gd_format(keep, &fm) ? fm.support : 0;
        break;
    }
    case 5: /* D3D11 options: discards, partial constant buffer updates, no-overwrite maps */
        for (uint32_t i = 2; i <= 9 && (i + 1) * 4 <= size; i++)
            d[i] = i == 2 || i >= 6;
        break;
    case 9:  /* D3D9 shadows */
    case 11: /* D3D9 simple instancing */
        if (size >= 4)
            d[0] = 1;
        break;
    default:
        break;
    }
    return S_OK;
}

static uint64_t m_feature_level(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return dev->dev.level;
}

static uint64_t m_creation_flags(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    return dev->dev.flags;
}

static uint64_t m_immediate_context(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    struct cobj *c = dev->dev.context;
    __atomic_add_fetch(&c->refs, 1, __ATOMIC_RELAXED);
    put64(a[1], iface(c, 0));
    return 0;
}

static uint64_t m_deferred_context(struct cobj *dev, uint64_t *a, const uint64_t *f);

/* the device's GetPrivateData comes first, SetPrivateData second */
static const method device_fns[] = {
    IUNKNOWN,
    [3] = m_create_buffer, m_create_tex1d, m_create_tex2d, m_create_tex3d, m_create_srv, m_create_uav, m_create_rtv,
    m_create_dsv, m_create_layout, m_create_vs, m_create_gs, m_create_gs_so, m_create_ps, m_create_hs, m_create_ds,
    m_create_cs, m_create_linkage, m_create_blend, m_create_depth, m_create_raster, m_create_sampler,
    m_create_query, m_create_predicate, m_unsupported, m_deferred_context, m_notimpl, m_format_support,
    m_ms_levels, m_counter_info, m_notimpl, m_feature_support,
    [34] = m_get_private, m_set_private, m_set_private_iface, m_feature_level, m_creation_flags, m_ok,
    m_immediate_context, m_ok, m_zero,
    /* ID3D11Device1 */
    [43] = m_immediate_context, m_deferred_context, m_create_blend1, m_create_raster, m_notimpl, m_notimpl,
    m_notimpl,
    /* ID3D11Device2 */
    [50] = m_immediate_context, m_deferred_context, m_zero, m_ms_levels1,
};

/* ---- the immediate context */

static void put_obj(uint64_t where, struct cobj *o)
{
    if (!where)
        return;
    if (o)
        __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
    put64(where, iface(o, 0));
}

static struct cobj *arg_obj(uint64_t array, uint32_t i, uint32_t kind)
{
    return array ? unwrap_kind(get64(array + i * 8ull), kind) : NULL;
}

static void set_cbs(struct context *c, int s, uint64_t *a, uint64_t firsts, uint64_t counts)
{
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[1] + i < GD_SLOTS_CB; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        struct cobj *o = arg_obj(a[3], i, K_BUFFER);
        bind(&c->cb[s][slot], o);
        c->cb_first[s][slot] = firsts ? get32(firsts + i * 4ull) : 0;
        c->cb_count[s][slot] = counts ? get32(counts + i * 4ull) : 0;
        gd_set_constant_buffer(c->gd, (uint32_t)s, slot, o ? o->res.r : NULL, c->cb_first[s][slot],
                               c->cb_count[s][slot]);
    }
}

static void get_cbs(struct context *c, int s, uint64_t *a, uint64_t firsts, uint64_t counts)
{
    for (uint32_t i = 0; i < (uint32_t)a[2]; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        bool in = slot < GD_SLOTS_CB;
        if (a[3])
            put_obj(a[3] + i * 8ull, in ? c->cb[s][slot] : NULL);
        if (firsts)
            put32(firsts + i * 4ull, in ? c->cb_first[s][slot] : 0);
        if (counts)
            put32(counts + i * 4ull, in ? c->cb_count[s][slot] : 0);
    }
}

static void set_srvs(struct context *c, int s, uint64_t *a)
{
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[1] + i < GD_SLOTS_SRV; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        struct cobj *o = arg_obj(a[3], i, K_SRV);
        bind(&c->srv[s][slot], o);
        gd_set_shader_resource(c->gd, (uint32_t)s, slot, o ? o->view.v : NULL);
    }
}

static void get_srvs(struct context *c, int s, uint64_t *a)
{
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[3]; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        put_obj(a[3] + i * 8ull, slot < GD_SLOTS_SRV ? c->srv[s][slot] : NULL);
    }
}

static void set_samplers(struct context *c, int s, uint64_t *a)
{
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[1] + i < GD_SLOTS_SAMPLER; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        struct cobj *o = arg_obj(a[3], i, K_SAMPLER);
        bind(&c->sampler[s][slot], o);
        gd_set_sampler(c->gd, (uint32_t)s, slot, o ? &o->state.s.sampler : NULL);
    }
}

static void get_samplers(struct context *c, int s, uint64_t *a)
{
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[3]; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        put_obj(a[3] + i * 8ull, slot < GD_SLOTS_SAMPLER ? c->sampler[s][slot] : NULL);
    }
}

static void set_shader(struct context *c, int s, uint64_t p)
{
    struct cobj *o = unwrap_kind(p, K_PS + (uint32_t)s);
    bind(&c->shader[s], o);
    gd_set_shader(c->gd, (uint32_t)s, o ? o->shader : NULL);
}

static void get_shader(struct context *c, int s, uint64_t *a)
{
    put_obj(a[1], c->shader[s]);
    if (a[3])
        put32(a[3], 0);
}

#define STAGE_METHODS(P, S)                                                                                        \
    static uint64_t m_##P##_set_cbs(struct cobj *o, uint64_t *a, const uint64_t *f)                               \
    {                                                                                                              \
        set_cbs(o->ctx, S, a, 0, 0);                                                                               \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_set_cbs1(struct cobj *o, uint64_t *a, const uint64_t *f)                              \
    {                                                                                                              \
        set_cbs(o->ctx, S, a, a[4], a[5]);                                                                         \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_get_cbs(struct cobj *o, uint64_t *a, const uint64_t *f)                               \
    {                                                                                                              \
        get_cbs(o->ctx, S, a, 0, 0);                                                                               \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_get_cbs1(struct cobj *o, uint64_t *a, const uint64_t *f)                              \
    {                                                                                                              \
        get_cbs(o->ctx, S, a, a[4], a[5]);                                                                         \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_set_srvs(struct cobj *o, uint64_t *a, const uint64_t *f)                              \
    {                                                                                                              \
        set_srvs(o->ctx, S, a);                                                                                    \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_get_srvs(struct cobj *o, uint64_t *a, const uint64_t *f)                              \
    {                                                                                                              \
        get_srvs(o->ctx, S, a);                                                                                    \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_set_samplers(struct cobj *o, uint64_t *a, const uint64_t *f)                          \
    {                                                                                                              \
        set_samplers(o->ctx, S, a);                                                                                \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_get_samplers(struct cobj *o, uint64_t *a, const uint64_t *f)                          \
    {                                                                                                              \
        get_samplers(o->ctx, S, a);                                                                                \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_set_shader(struct cobj *o, uint64_t *a, const uint64_t *f)                            \
    {                                                                                                              \
        set_shader(o->ctx, S, a[1]);                                                                               \
        return 0;                                                                                                  \
    }                                                                                                              \
    static uint64_t m_##P##_get_shader(struct cobj *o, uint64_t *a, const uint64_t *f)                            \
    {                                                                                                              \
        get_shader(o->ctx, S, a);                                                                                  \
        return 0;                                                                                                  \
    }

STAGE_METHODS(ps, 0)
STAGE_METHODS(vs, 1)
STAGE_METHODS(gs, 2)
STAGE_METHODS(hs, 3)
STAGE_METHODS(ds, 4)
STAGE_METHODS(cs, 5)

/* SetPredication: draws are skipped while the predicate's answer equals the value given */
static bool predicated_off(struct context *c)
{
    struct cobj *p = c->predicate;
    return p && p->query.state == 2 && (p->query.result[0] != 0) == (c->predicate_value != 0);
}

static uint64_t m_draw_indexed(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!predicated_off(o->ctx))
        gd_draw(o->ctx->gd, true, (uint32_t)a[1], (uint32_t)a[2], (int32_t)a[3], 1, 0);
    return 0;
}

static uint64_t m_draw(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!predicated_off(o->ctx))
        gd_draw(o->ctx->gd, false, (uint32_t)a[1], (uint32_t)a[2], 0, 1, 0);
    return 0;
}

static uint64_t m_draw_indexed_instanced(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!predicated_off(o->ctx))
        gd_draw(o->ctx->gd, true, (uint32_t)a[1], (uint32_t)a[3], (int32_t)a[4], (uint32_t)a[2], (uint32_t)a[5]);
    return 0;
}

static uint64_t m_draw_instanced(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!predicated_off(o->ctx))
        gd_draw(o->ctx->gd, false, (uint32_t)a[1], (uint32_t)a[3], 0, (uint32_t)a[2], (uint32_t)a[4]);
    return 0;
}

/* the arguments of an indirect draw, from a buffer */
static bool indirect_args(uint64_t buffer, uint64_t offset, uint32_t *out, uint32_t n)
{
    struct cobj *b = unwrap_kind(buffer, K_BUFFER);
    struct gd_layout l;
    uint8_t *p = b ? gd_subresource(b->res.r, 0, &l) : NULL;
    if (!p || offset + n * 4 > l.size)
        return false;
    memcpy(out, p + offset, n * 4);
    return true;
}

static uint64_t m_draw_indexed_indirect(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t v[5];
    if (indirect_args(a[1], (uint32_t)a[2], v, 5) && !predicated_off(o->ctx))
        gd_draw(o->ctx->gd, true, v[0], v[2], (int32_t)v[3], v[1], v[4]);
    return 0;
}

static uint64_t m_draw_indirect(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t v[4];
    if (indirect_args(a[1], (uint32_t)a[2], v, 4) && !predicated_off(o->ctx))
        gd_draw(o->ctx->gd, false, v[0], v[2], 0, v[1], v[3]);
    return 0;
}

static uint64_t m_dispatch(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!predicated_off(o->ctx))
        gd_dispatch(o->ctx->gd, (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3]);
    return 0;
}

static uint64_t m_dispatch_indirect(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    uint32_t v[3];
    if (indirect_args(a[1], (uint32_t)a[2], v, 3) && !predicated_off(o->ctx))
        gd_dispatch(o->ctx->gd, v[0], v[1], v[2]);
    return 0;
}

/* Map(resource, subresource, type, flags, D3D11_MAPPED_SUBRESOURCE *): the resource's own pages */
static uint64_t m_map(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *r = unwrap(a[1]);
    if (!is_resource(r) || (uint32_t)a[2] >= gd_subresources(r->res.r))
        return E_INVALIDARG;
    struct gd_layout l;
    uint64_t p = map_sub(r, (uint32_t)a[2], &l);
    if (!p)
        return E_OUTOFMEMORY;
    if (a[5]) {
        put64(a[5], p);
        put32(a[5] + 8, l.row_pitch);
        put32(a[5] + 12, l.slice_pitch);
    }
    return S_OK;
}

static uint64_t m_ia_set_layout(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *l = unwrap_kind(a[1], K_LAYOUT);
    bind(&o->ctx->layout, l);
    gd_set_input_layout(o->ctx->gd, l ? l->layout : NULL);
    return 0;
}

/* IASetVertexBuffers(start, count, buffers, strides, offsets) */
static uint64_t m_ia_set_vbs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[1] + i < GD_SLOTS_VB; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        struct cobj *b = arg_obj(a[3], i, K_BUFFER);
        bind(&c->vb[slot], b);
        c->vb_stride[slot] = a[4] ? get32(a[4] + i * 4ull) : 0;
        c->vb_offset[slot] = a[5] ? get32(a[5] + i * 4ull) : 0;
        gd_set_vertex_buffer(c->gd, slot, b ? b->res.r : NULL, c->vb_stride[slot], c->vb_offset[slot]);
    }
    return 0;
}

static uint64_t m_ia_set_ib(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    struct cobj *b = unwrap_kind(a[1], K_BUFFER);
    bind(&c->ib, b);
    c->ib_format = (uint32_t)a[2];
    c->ib_offset = (uint32_t)a[3];
    gd_set_index_buffer(c->gd, b ? b->res.r : NULL, c->ib_format, c->ib_offset);
    return 0;
}

static uint64_t m_ia_set_topology(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    o->ctx->topology = (uint32_t)a[1];
    gd_set_topology(o->ctx->gd, (uint32_t)a[1]);
    return 0;
}

static uint64_t m_ia_get_layout(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], o->ctx->layout);
    return 0;
}

static uint64_t m_ia_get_vbs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    for (uint32_t i = 0; i < (uint32_t)a[2]; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        bool in = slot < GD_SLOTS_VB;
        if (a[3])
            put_obj(a[3] + i * 8ull, in ? c->vb[slot] : NULL);
        if (a[4])
            put32(a[4] + i * 4ull, in ? c->vb_stride[slot] : 0);
        if (a[5])
            put32(a[5] + i * 4ull, in ? c->vb_offset[slot] : 0);
    }
    return 0;
}

static uint64_t m_ia_get_ib(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], o->ctx->ib);
    put32(a[2], o->ctx->ib_format);
    put32(a[3], o->ctx->ib_offset);
    return 0;
}

static uint64_t m_ia_get_topology(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->ctx->topology);
    return 0;
}

/* ---- queries on the context */

static uint64_t now_ms(void)
{
    return hal_ticks_ms();
}

static uint64_t m_begin(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *q = unwrap(a[1]);
    if (!q || (q->kind != K_QUERY && q->kind != K_PREDICATE))
        return 0;
    struct gd_stats st;
    gd_get_stats(&st);
    q->query.state = 1;
    q->query.start = q->query.type == 1 || q->query.type == 5 ? st.pixels : now_ms();
    memset(q->query.result, 0, sizeof(q->query.result));
    if (q->query.type == 4) { /* pipeline statistics: begin values kept in result, made differences at End */
        q->query.result[0] = st.triangles * 3, q->query.result[1] = st.triangles;
        q->query.result[7] = st.pixels;
    }
    return 0;
}

static uint64_t m_end(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *q = unwrap(a[1]);
    if (!q || (q->kind != K_QUERY && q->kind != K_PREDICATE))
        return 0;
    struct gd_stats st;
    gd_get_stats(&st);
    uint64_t *r = q->query.result;
    switch (q->query.type) {
    case 0: /* event: everything before it is done, the draws being synchronous */
        r[0] = 1;
        break;
    case 1:
        r[0] = q->query.state == 1 ? st.pixels - q->query.start : 0;
        break;
    case 2:
        r[0] = now_ms();
        break;
    case 3: /* timestamps count milliseconds */
        r[0] = 1000, r[1] = 0;
        break;
    case 4: {
        uint64_t tris = st.triangles - r[1], px = st.pixels - r[7];
        memset(r, 0, sizeof(q->query.result));
        r[0] = r[2] = tris * 3, r[1] = r[5] = r[6] = tris, r[7] = px;
        break;
    }
    case 5:
        r[0] = q->query.state == 1 && st.pixels != q->query.start;
        break;
    default:
        memset(r, 0, sizeof(q->query.result));
        break;
    }
    q->query.state = 2;
    return 0;
}

/* GetData(async, data, size, flags) */
static uint64_t m_get_data(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *q = unwrap(a[1]);
    if (!q || (q->kind != K_QUERY && q->kind != K_PREDICATE))
        return E_INVALIDARG;
    if (q->query.state != 2)
        return S_FALSE;
    uint32_t size = query_size(q->query.type);
    if (a[2] && a[3]) {
        uint8_t out[88] = {0};
        uint32_t t = q->query.type;
        if (t == 0 || t == 5 || t == 7 || (t >= 9 && t % 2))
            memcpy(out, &(uint32_t){(uint32_t)q->query.result[0]}, 4);
        else if (t == 3)
            memcpy(out, &q->query.result[0], 8), memcpy(out + 8, &(uint32_t){(uint32_t)q->query.result[1]}, 4);
        else
            memcpy(out, q->query.result, size);
        memcpy(w64_ptr(a[2]), out, MIN(size, (uint32_t)a[3]));
    }
    return S_OK;
}

static uint64_t m_set_predication(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    bind(&o->ctx->predicate, unwrap_kind(a[1], K_PREDICATE));
    o->ctx->predicate_value = (uint32_t)a[2];
    return 0;
}

static uint64_t m_get_predication(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], o->ctx->predicate);
    put32(a[2], o->ctx->predicate_value);
    return 0;
}

/* ---- output merger and rasterizer */

static void set_targets(struct context *c, uint32_t count, uint64_t rtvs, uint64_t dsv)
{
    for (uint32_t i = 0; i < 8; i++) {
        struct cobj *v = i < count ? arg_obj(rtvs, i, K_RTV) : NULL;
        bind(&c->rtv[i], v);
        gd_set_render_target(c->gd, i, v ? v->view.v : NULL);
    }
    struct cobj *d = unwrap_kind(dsv, K_DSV);
    bind(&c->dsv, d);
    gd_set_depth_target(c->gd, d ? d->view.v : NULL);
}

static uint64_t m_om_set_targets(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    set_targets(o->ctx, (uint32_t)a[1], a[2], a[3]);
    return 0;
}

/* OMSetRenderTargetsAndUnorderedAccessViews(nrtv, rtvs, dsv, uav start, nuav, uavs, counts) */
static uint64_t m_om_set_targets_uavs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    if ((uint32_t)a[1] != 0xffffffffu)
        set_targets(c, (uint32_t)a[1], a[2], a[3]);
    if ((uint32_t)a[5] != 0xffffffffu)
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t start = (uint32_t)a[4];
            bool in = i >= start && i - start < (uint32_t)a[5];
            bind(&c->uav[i], in ? arg_obj(a[6], i - start, K_UAV) : NULL);
            uint32_t count = in && a[7] ? ((const uint32_t *)w64_ptr(a[7]))[i - start] : 0xffffffffu;
            gd_set_uav(c->gd, 0, i, c->uav[i] ? c->uav[i]->view.v : NULL, count);
        }
    return 0;
}

/* CSSetUnorderedAccessViews(start, count, uavs, initial counts) */
static uint64_t m_cs_set_uavs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    for (uint32_t i = 0; i < (uint32_t)a[2]; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        if (slot >= 8)
            break;
        bind(&c->csuav[slot], a[3] ? arg_obj(a[3], i, K_UAV) : NULL);
        uint32_t count = a[4] ? ((const uint32_t *)w64_ptr(a[4]))[i] : 0xffffffffu;
        gd_set_uav(c->gd, 5, slot, c->csuav[slot] ? c->csuav[slot]->view.v : NULL, count);
    }
    return 0;
}

static uint64_t m_cs_get_uavs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    for (uint32_t i = 0; i < (uint32_t)a[2] && a[3]; i++) {
        uint32_t slot = (uint32_t)a[1] + i;
        put_obj(a[3] + i * 8ull, slot < 8 ? o->ctx->csuav[slot] : NULL);
    }
    return 0;
}

static uint64_t m_om_set_blend(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    struct cobj *b = unwrap_kind(a[1], K_BLEND);
    bind(&c->blend, b);
    if (a[2])
        memcpy(c->blend_factor, w64_ptr(a[2]), 16);
    else
        for (int i = 0; i < 4; i++)
            c->blend_factor[i] = 1.0f;
    c->sample_mask = (uint32_t)a[3];
    gd_set_blend(c->gd, b ? &b->state.s.blend : NULL, c->blend_factor, c->sample_mask);
    return 0;
}

static uint64_t m_om_set_depth(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    struct cobj *d = unwrap_kind(a[1], K_DEPTH);
    bind(&c->depth, d);
    c->stencil_ref = (uint32_t)a[2];
    gd_set_depth(c->gd, d ? &d->state.s.depth : NULL, c->stencil_ref);
    return 0;
}

static uint64_t m_om_get_targets(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    for (uint32_t i = 0; i < (uint32_t)a[1] && a[2]; i++)
        put_obj(a[2] + i * 8ull, i < 8 ? o->ctx->rtv[i] : NULL);
    put_obj(a[3], o->ctx->dsv);
    return 0;
}

static uint64_t m_om_get_targets_uavs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    for (uint32_t i = 0; i < (uint32_t)a[1] && a[2]; i++)
        put_obj(a[2] + i * 8ull, i < 8 ? o->ctx->rtv[i] : NULL);
    put_obj(a[3], o->ctx->dsv);
    for (uint32_t i = 0; i < (uint32_t)a[5] && a[6]; i++) {
        uint32_t slot = (uint32_t)a[4] + i;
        put_obj(a[6] + i * 8ull, slot < 8 ? o->ctx->uav[slot] : NULL);
    }
    return 0;
}

static uint64_t m_om_get_blend(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], o->ctx->blend);
    if (a[2])
        memcpy(w64_ptr(a[2]), o->ctx->blend_factor, 16);
    put32(a[3], o->ctx->sample_mask);
    return 0;
}

static uint64_t m_om_get_depth(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], o->ctx->depth);
    put32(a[2], o->ctx->stencil_ref);
    return 0;
}

static uint64_t m_rs_set_state(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *r = unwrap_kind(a[1], K_RASTER);
    bind(&o->ctx->raster, r);
    gd_set_raster(o->ctx->gd, r ? &r->state.s.raster : NULL);
    return 0;
}

static uint64_t m_rs_set_viewports(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    c->nvp = MIN((uint32_t)a[1], 16u);
    if (c->nvp && a[2])
        memcpy(c->vp, w64_ptr(a[2]), sizeof(struct gd_viewport) * c->nvp);
    else
        c->nvp = 0;
    gd_set_viewports(c->gd, c->nvp, c->vp);
    return 0;
}

static uint64_t m_rs_set_scissors(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    c->nsc = MIN((uint32_t)a[1], 16u);
    if (c->nsc && a[2])
        memcpy(c->sc, w64_ptr(a[2]), sizeof(struct gd_rect) * c->nsc);
    else
        c->nsc = 0;
    gd_set_scissors(c->gd, c->nsc, c->sc);
    return 0;
}

static uint64_t m_rs_get_state(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], o->ctx->raster);
    return 0;
}

/* RSGetViewports(UINT *count, D3D11_VIEWPORT *): count first, or the copy */
static uint64_t m_rs_get_viewports(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    if (!a[1])
        return 0;
    if (a[2]) {
        uint32_t n = get32(a[1]);
        memset(w64_ptr(a[2]), 0, sizeof(struct gd_viewport) * n);
        memcpy(w64_ptr(a[2]), c->vp, sizeof(struct gd_viewport) * MIN(n, c->nvp));
    } else {
        put32(a[1], c->nvp);
    }
    return 0;
}

static uint64_t m_rs_get_scissors(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    if (!a[1])
        return 0;
    if (a[2]) {
        uint32_t n = get32(a[1]);
        memset(w64_ptr(a[2]), 0, sizeof(struct gd_rect) * n);
        memcpy(w64_ptr(a[2]), c->sc, sizeof(struct gd_rect) * MIN(n, c->nsc));
    } else {
        put32(a[1], c->nsc);
    }
    return 0;
}

/* ---- copies and clears */

/* CopySubresourceRegion(dst, dst sub, x, y, z, src, src sub, box) */
static uint64_t m_copy_region(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *d = unwrap(a[1]), *s = unwrap(a[6]);
    if (is_resource(d) && is_resource(s))
        gd_copy_region(d->res.r, (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4], (uint32_t)a[5], s->res.r,
                       (uint32_t)a[7], a[8] ? w64_ptr(a[8]) : NULL);
    return 0;
}

static uint64_t m_copy_resource(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *d = unwrap(a[1]), *s = unwrap(a[2]);
    if (is_resource(d) && is_resource(s))
        gd_copy_resource(d->res.r, s->res.r);
    return 0;
}

/* UpdateSubresource(dst, sub, box, data, row pitch, depth pitch) */
static uint64_t m_update(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *d = unwrap(a[1]);
    if (is_resource(d) && a[4])
        gd_update(d->res.r, (uint32_t)a[2], a[3] ? w64_ptr(a[3]) : NULL, w64_ptr(a[4]), (uint32_t)a[5],
                  (uint32_t)a[6]);
    return 0;
}

static uint64_t m_clear_rtv(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *v = unwrap_kind(a[1], K_RTV);
    if (v && a[2]) {
        float c[4];
        memcpy(c, w64_ptr(a[2]), 16);
        gd_clear_rtv(v->view.v, c);
    }
    return 0;
}

/* ClearUnorderedAccessViewUint and ...Float(view, values[4]) */
static uint64_t m_clear_uav_uint(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *v = unwrap_kind(a[1], K_UAV);
    if (v && a[2])
        gd_clear_uav(v->view.v, w64_ptr(a[2]), false);
    return 0;
}

static uint64_t m_clear_uav_float(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *v = unwrap_kind(a[1], K_UAV);
    if (v && a[2])
        gd_clear_uav(v->view.v, w64_ptr(a[2]), true);
    return 0;
}

/* CopyStructureCount(buffer, offset, view): the view's hidden counter into the buffer */
static uint64_t m_copy_structure_count(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *b = unwrap_kind(a[1], K_BUFFER), *v = unwrap_kind(a[3], K_UAV);
    struct gd_layout l;
    uint8_t *p = b && v ? gd_subresource(b->res.r, 0, &l) : NULL;
    uint32_t n = v ? gd_uav_counter(v->view.v) : 0;
    if (p && (uint32_t)a[2] + 4 <= l.size)
        memcpy(p + (uint32_t)a[2], &n, 4);
    return 0;
}

static uint64_t m_clear_dsv(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *v = unwrap_kind(a[1], K_DSV);
    if (v)
        gd_clear_dsv(v->view.v, (uint32_t)a[2] & 3, argf(f, 3), (uint32_t)a[4] & 0xff);
    return 0;
}

static uint64_t m_generate_mips(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *v = unwrap_kind(a[1], K_SRV);
    if (v)
        gd_generate_mips(v->view.v);
    return 0;
}

/* ResolveSubresource(dst, dst sub, src, src sub, format): one sample, so a copy */
static uint64_t m_resolve(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *d = unwrap(a[1]), *s = unwrap(a[3]);
    if (is_resource(d) && is_resource(s))
        gd_copy_region(d->res.r, (uint32_t)a[2], 0, 0, 0, s->res.r, (uint32_t)a[4], NULL);
    return 0;
}

/* ClearView(view, color, rects, count): render targets */
static uint64_t m_clear_view(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *v = unwrap(a[1]);
    if (v && v->kind == K_RTV && a[2] && !a[4]) {
        float c[4];
        memcpy(c, w64_ptr(a[2]), 16);
        gd_clear_rtv(v->view.v, c);
    }
    return 0;
}

static uint64_t m_clear_state(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    context_unbind_all(o->ctx);
    gd_context_clear(o->ctx->gd);
    return 0;
}

static uint64_t m_get_min_lod(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return 0; /* 0.0f */
}

/* SOGetTargets(count, buffers) */
static uint64_t m_so_get_targets(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    for (uint32_t i = 0; i < (uint32_t)a[1] && a[2]; i++)
        put_obj(a[2] + i * 8ull, i < GD_SLOTS_SO ? o->ctx->so[i] : NULL);
    return 0;
}

/* SOSetTargets(count, buffers, offsets): the slots after count are unbound */
static uint64_t m_so_set_targets(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *c = o->ctx;
    for (uint32_t i = 0; i < GD_SLOTS_SO; i++) {
        struct cobj *b = i < (uint32_t)a[1] && a[2] ? arg_obj(a[2], i, K_BUFFER) : NULL;
        uint32_t off = b && a[3] ? get32(a[3] + i * 4ull) : 0xffffffffu;
        bind(&c->so[i], b);
        gd_set_so_target(c->gd, i, b ? b->res.r : NULL, off);
    }
    return 0;
}

static uint64_t m_draw_auto(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!predicated_off(o->ctx))
        gd_draw_auto(o->ctx->gd);
    return 0;
}

/* D3D11_DEVICE_CONTEXT_IMMEDIATE */
static void drop_program_ref(struct cobj *o);

static bool deferred(const struct cobj *o)
{
    return o->ctx && o->ctx->deferred;
}

/* ---- deferred contexts

   A deferred context keeps every call that draws, clears, copies or sets state, with a copy of what its pointers
   point at and a reference on every object it names; the state calls also change its own state, so its Get calls
   answer as they should. FinishCommandList hands the calls over as a command list; ExecuteCommandList makes them
   again, in order, on the immediate context, starting from the default state and then putting the immediate
   context's own state back (or leaving the default state). */

struct call {
    struct call *next;
    uint16_t slot, nheld;
    uint64_t a[10], f[8];
    struct cobj **held;
    uint8_t *guest;  /* Map on a deferred context: the program wrote here (its memory) */
    uint32_t guest_size;
    uint8_t data[]; /* the copies the pointers in a[] point at, then held[] */
};

enum { P_END, P_OBJ, P_OBJS, P_BYTES, P_ARR, P_NONE, P_UPDATE };

struct pdesc {
    uint8_t type, arg, count, size; /* count: the argument giving how many; size: of one element or in all */
};

#define OBJ(i) {P_OBJ, i, 0, 0}
#define OBJS(i, n) {P_OBJS, i, n, 8}
#define BYTES(i, n) {P_BYTES, i, 0, n}
#define ARR(i, n, sz) {P_ARR, i, n, sz}
#define NONE(i) {P_NONE, i, 0, 0}

/* what each kept call points at; KEEP: recorded only, STATE: recorded and made on the deferred context too */
enum { DIRECT, KEEP, STATE };

struct dcall {
    uint8_t how;
    struct pdesc p[4];
};

#define SLOTS3 {STATE, {OBJS(3, 2)}}
#define SHADER {STATE, {OBJ(1), NONE(2)}}
#define CBS1 {STATE, {OBJS(3, 2), ARR(4, 2, 4), ARR(5, 2, 4)}}

static const struct dcall dcalls[134] = {
    [7] = SLOTS3, [8] = SLOTS3, [9] = SHADER, [10] = SLOTS3, [11] = SHADER, [12] = {KEEP, {{0}}},
    [13] = {KEEP, {{0}}}, [16] = SLOTS3, [17] = {STATE, {OBJ(1)}},
    [18] = {STATE, {OBJS(3, 2), ARR(4, 2, 4), ARR(5, 2, 4)}}, [19] = {STATE, {OBJ(1)}}, [20] = {KEEP, {{0}}},
    [21] = {KEEP, {{0}}}, [22] = SLOTS3, [23] = SHADER, [24] = {STATE, {{0}}}, [25] = SLOTS3, [26] = SLOTS3,
    [27] = {KEEP, {OBJ(1)}}, [28] = {KEEP, {OBJ(1)}}, [30] = {STATE, {OBJ(1)}}, [31] = SLOTS3, [32] = SLOTS3,
    [33] = {STATE, {OBJS(2, 1), OBJ(3)}}, [34] = {STATE, {OBJS(2, 1), OBJ(3), OBJS(6, 5), ARR(7, 5, 4)}},
    [35] = {STATE, {OBJ(1), BYTES(2, 16)}}, [36] = {STATE, {OBJ(1)}}, [37] = {STATE, {OBJS(2, 1), ARR(3, 1, 4)}},
    [38] = {KEEP, {{0}}}, [39] = {KEEP, {OBJ(1)}}, [40] = {KEEP, {OBJ(1)}}, [41] = {KEEP, {{0}}},
    [42] = {KEEP, {OBJ(1)}}, [43] = {STATE, {OBJ(1)}}, [44] = {STATE, {ARR(2, 1, 24)}},
    [45] = {STATE, {ARR(2, 1, 16)}}, [46] = {KEEP, {OBJ(1), OBJ(6), BYTES(7, 24)}}, [47] = {KEEP, {OBJ(1), OBJ(2)}},
    [48] = {KEEP, {OBJ(1), BYTES(3, 24), {P_UPDATE, 4, 0, 0}}}, [49] = {KEEP, {OBJ(1), OBJ(3)}},
    [50] = {KEEP, {OBJ(1), BYTES(2, 16)}}, [51] = {KEEP, {OBJ(1), BYTES(2, 16)}},
    [52] = {KEEP, {OBJ(1), BYTES(2, 16)}}, [53] = {KEEP, {OBJ(1)}}, [54] = {KEEP, {OBJ(1)}},
    [55] = {KEEP, {OBJ(1)}}, [57] = {KEEP, {OBJ(1), OBJ(3)}}, [58] = {KEEP, {OBJ(1)}}, [59] = SLOTS3,
    [60] = SHADER, [61] = SLOTS3, [62] = SLOTS3, [63] = SLOTS3, [64] = SHADER, [65] = SLOTS3, [66] = SLOTS3,
    [67] = SLOTS3, [68] = {STATE, {OBJS(3, 2), ARR(4, 2, 4)}}, [69] = SHADER, [70] = SLOTS3, [71] = SLOTS3,
    [110] = {STATE, {{0}}}, [115] = {KEEP, {OBJ(1), OBJ(6), BYTES(7, 24)}},
    [116] = {KEEP, {OBJ(1), BYTES(3, 24), {P_UPDATE, 4, 0, 0}}}, [117] = {KEEP, {OBJ(1)}}, [118] = {KEEP, {OBJ(1)}},
    [119] = CBS1, [120] = CBS1, [121] = CBS1, [122] = CBS1, [123] = CBS1, [124] = CBS1,
    [132] = {KEEP, {OBJ(1), BYTES(2, 16), ARR(3, 4, 16)}}, [133] = {KEEP, {OBJ(1), ARR(2, 3, 16)}},
};

/* how many elements an array argument has: none for D3D11_KEEP_* (0xffffffff) and the like */
static uint32_t elements(const uint64_t *a, const struct pdesc *p)
{
    uint32_t n = (uint32_t)a[p->count];
    return n > 4096 ? 0 : n;
}

static size_t payload(const struct pdesc *p, const uint64_t *a, uint32_t *objs)
{
    if (!a[p->arg])
        return 0;
    switch (p->type) {
    case P_OBJ:
        (*objs)++;
        return 0;
    case P_OBJS:
        *objs += elements(a, p);
        return (size_t)elements(a, p) * 8;
    case P_BYTES:
        return p->size;
    case P_ARR:
        return (size_t)elements(a, p) * p->size;
    case P_UPDATE: {
        struct cobj *r = unwrap(a[1]);
        struct gd_box box;
        if (a[3])
            memcpy(&box, w64_ptr(a[3]), sizeof(box));
        return is_resource(r) ? gd_update_size(r->res.r, (uint32_t)a[2], a[3] ? &box : NULL, (uint32_t)a[5],
                                               (uint32_t)a[6])
                              : 0;
    }
    default:
        return 0;
    }
}

static void keep_obj(struct call *c, uint64_t p)
{
    struct cobj *o = unwrap(p);
    if (o) {
        hold(o);
        c->held[c->nheld++] = o;
    }
}

static struct call *keep(struct context *ctx, uint32_t slot, const uint64_t *a, const uint64_t *f)
{
    const struct dcall *d = &dcalls[slot];
    size_t bytes = 0;
    uint32_t objs = 0;
    for (const struct pdesc *p = d->p; p < d->p + 4 && p->type; p++)
        bytes += (payload(p, a, &objs) + 7) & ~(size_t)7;
    struct call *c = kzalloc(sizeof(*c) + bytes + objs * sizeof(struct cobj *));
    if (!c)
        return NULL;
    c->slot = (uint16_t)slot;
    memcpy(c->a, a, sizeof(c->a));
    memcpy(c->f, f, sizeof(c->f));
    c->held = (struct cobj **)(c->data + bytes);
    uint8_t *at = c->data;
    for (const struct pdesc *p = d->p; p < d->p + 4 && p->type; p++) {
        uint64_t src = a[p->arg];
        if (!src)
            continue;
        if (p->type == P_NONE) {
            c->a[p->arg] = 0;
            continue;
        }
        if (p->type == P_OBJ) {
            keep_obj(c, src);
            continue;
        }
        size_t n = payload(p, a, &(uint32_t){0});
        if (n)
            memcpy(at, w64_ptr(src), n);
        if (p->type == P_OBJS)
            for (uint32_t i = 0; i < elements(a, p); i++)
                keep_obj(c, get64((uintptr_t)at + i * 8ull));
        c->a[p->arg] = (uintptr_t)at;
        at += (n + 7) & ~(size_t)7;
    }
    if (ctx->last)
        ctx->last->next = c;
    else
        ctx->first = c;
    ctx->last = c;
    return c;
}

static void calls_free(struct call *c, bool release)
{
    while (c) {
        struct call *next = c->next;
        if (release)
            for (uint32_t i = 0; i < c->nheld; i++)
                drop(c->held[i]);
        if (c->guest)
            w32_free(c->guest);
        kfree(c);
        c = next;
    }
}

/* Map on a deferred context (WRITE_DISCARD, or NO_OVERWRITE on a buffer): the program writes into memory of its
   own; Unmap keeps it as a call that copies it into the resource. NO_OVERWRITE starts from what the resource will
   hold by then: the last copy kept for it, else what it holds now. */
#define SLOT_FILL 255

static struct call *pending_map(struct context *ctx, struct cobj *r, uint32_t sub)
{
    for (struct call *c = ctx->first; c; c = c->next)
        if (c->slot == SLOT_FILL && c->held[0] == r && (uint32_t)c->a[2] == sub && c->a[3])
            return c;
    return NULL;
}

static uint64_t deferred_map(struct context *ctx, uint64_t *a)
{
    struct cobj *r = unwrap(a[1]);
    uint32_t sub = (uint32_t)a[2], type = (uint32_t)a[3];
    struct gd_layout l;
    uint8_t *now = is_resource(r) ? gd_subresource(r->res.r, sub, &l) : NULL;
    if (!now || (type != 4 && type != 5) || !a[5])
        return E_INVALIDARG;
    struct call *c = kzalloc(sizeof(*c) + sizeof(struct cobj *));
    uint8_t *mem = c ? w32_zalloc(l.size) : NULL;
    if (!mem) {
        kfree(c);
        return E_OUTOFMEMORY;
    }
    if (type == 5) { /* NO_OVERWRITE */
        struct call *last = NULL;
        for (struct call *k = ctx->first; k; k = k->next)
            if (k->slot == SLOT_FILL && k->held[0] == r && (uint32_t)k->a[2] == sub)
                last = k;
        memcpy(mem, last ? last->guest : now, l.size);
    }
    c->slot = SLOT_FILL;
    c->held = (struct cobj **)c->data;
    c->held[0] = r;
    c->nheld = 1;
    hold(r);
    c->a[2] = sub;
    c->a[3] = 1; /* still mapped */
    c->guest = mem;
    c->guest_size = l.size;
    if (ctx->last)
        ctx->last->next = c;
    else
        ctx->first = c;
    ctx->last = c;
    put64(a[5], (uintptr_t)mem);
    put32(a[5] + 8, l.row_pitch);
    put32(a[5] + 12, l.slice_pitch);
    return S_OK;
}

static uint64_t deferred_unmap(struct context *ctx, uint64_t *a)
{
    struct cobj *r = unwrap(a[1]);
    struct call *c = r ? pending_map(ctx, r, (uint32_t)a[2]) : NULL;
    if (c)
        c->a[3] = 0;
    return 0;
}

static uint64_t m_context_type(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return o->ctx->deferred ? 1 : 0; /* D3D11_DEVICE_CONTEXT_DEFERRED */
}

/* FinishCommandList(restore state, ID3D11CommandList **) */
static uint64_t m_finish(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct context *ctx = o->ctx;
    if (!ctx->deferred)
        return DXGI_ERROR_INVALID_CALL;
    struct cobj *l = child(K_CMDLIST, o->device, SP_CMDLIST, -1, -1);
    if (!l)
        return E_OUTOFMEMORY;
    l->calls = ctx->first;
    ctx->first = ctx->last = NULL;
    if (!(uint32_t)a[1]) {
        context_unbind_all(ctx);
        gd_context_clear(ctx->gd);
    }
    if (a[2])
        put64(a[2], iface(l, 0));
    else
        drop_program_ref(l);
    return S_OK;
}

static uint64_t defer(struct cobj *o, uint32_t slot, method fn, uint64_t *a, const uint64_t *f)
{
    struct context *ctx = o->ctx;
    if (slot == 14)
        return deferred_map(ctx, a);
    if (slot == 15)
        return deferred_unmap(ctx, a);
    if (slot == 29) /* GetData */
        return DXGI_ERROR_INVALID_CALL;
    uint8_t how = slot < ARRAY_SIZE(dcalls) ? dcalls[slot].how : DIRECT;
    if (how == DIRECT)
        return fn(o, a, f);
    keep(ctx, slot, a, f);
    return how == STATE ? fn(o, a, f) : 0;
}

/* the immediate context's state, kept while a command list runs */
static void context_hold_all(struct context *c, bool take)
{
    struct cobj **one[] = {&c->ib, &c->layout, &c->dsv, &c->blend, &c->depth, &c->raster, &c->predicate};
    for (size_t i = 0; i < ARRAY_SIZE(one); i++)
        if (*one[i])
            take ? hold(*one[i]) : drop(*one[i]);
    for (int s = 0; s < STAGES; s++) {
        struct cobj **lists[] = {&c->shader[s], c->cb[s], c->srv[s], c->sampler[s]};
        int counts[] = {1, GD_SLOTS_CB, GD_SLOTS_SRV, GD_SLOTS_SAMPLER};
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < counts[k]; i++)
                if (lists[k][i])
                    take ? hold(lists[k][i]) : drop(lists[k][i]);
    }
    for (int i = 0; i < GD_SLOTS_VB; i++)
        if (c->vb[i])
            take ? hold(c->vb[i]) : drop(c->vb[i]);
    for (int i = 0; i < 8; i++) {
        if (c->rtv[i])
            take ? hold(c->rtv[i]) : drop(c->rtv[i]);
        if (c->uav[i])
            take ? hold(c->uav[i]) : drop(c->uav[i]);
        if (c->csuav[i])
            take ? hold(c->csuav[i]) : drop(c->csuav[i]);
    }
    for (int i = 0; i < GD_SLOTS_SO; i++)
        if (c->so[i])
            take ? hold(c->so[i]) : drop(c->so[i]);
}

/* every binding of c told to its graphics context again */
static void context_push(struct context *c)
{
    gd_context_clear(c->gd);
    for (int s = 0; s < STAGES; s++) {
        gd_set_shader(c->gd, (uint32_t)s, c->shader[s] ? c->shader[s]->shader : NULL);
        for (uint32_t i = 0; i < GD_SLOTS_CB; i++)
            if (c->cb[s][i])
                gd_set_constant_buffer(c->gd, (uint32_t)s, i, c->cb[s][i]->res.r, c->cb_first[s][i], c->cb_count[s][i]);
        for (uint32_t i = 0; i < GD_SLOTS_SRV; i++)
            if (c->srv[s][i])
                gd_set_shader_resource(c->gd, (uint32_t)s, i, c->srv[s][i]->view.v);
        for (uint32_t i = 0; i < GD_SLOTS_SAMPLER; i++)
            if (c->sampler[s][i])
                gd_set_sampler(c->gd, (uint32_t)s, i, &c->sampler[s][i]->state.s.sampler);
    }
    for (uint32_t i = 0; i < GD_SLOTS_VB; i++)
        if (c->vb[i])
            gd_set_vertex_buffer(c->gd, i, c->vb[i]->res.r, c->vb_stride[i], c->vb_offset[i]);
    gd_set_index_buffer(c->gd, c->ib ? c->ib->res.r : NULL, c->ib_format, c->ib_offset);
    gd_set_input_layout(c->gd, c->layout ? c->layout->layout : NULL);
    gd_set_topology(c->gd, c->topology);
    for (uint32_t i = 0; i < 8; i++) {
        gd_set_render_target(c->gd, i, c->rtv[i] ? c->rtv[i]->view.v : NULL);
        gd_set_uav(c->gd, 0, i, c->uav[i] ? c->uav[i]->view.v : NULL, 0xffffffffu);
        gd_set_uav(c->gd, 5, i, c->csuav[i] ? c->csuav[i]->view.v : NULL, 0xffffffffu);
    }
    for (uint32_t i = 0; i < GD_SLOTS_SO; i++) /* where the buffers were filled to stays */
        gd_set_so_target(c->gd, i, c->so[i] ? c->so[i]->res.r : NULL, 0xffffffffu);
    gd_set_depth_target(c->gd, c->dsv ? c->dsv->view.v : NULL);
    gd_set_blend(c->gd, c->blend ? &c->blend->state.s.blend : NULL, c->blend_factor, c->sample_mask);
    gd_set_depth(c->gd, c->depth ? &c->depth->state.s.depth : NULL, c->stencil_ref);
    gd_set_raster(c->gd, c->raster ? &c->raster->state.s.raster : NULL);
    gd_set_viewports(c->gd, c->nvp, c->vp);
    gd_set_scissors(c->gd, c->nsc, c->sc);
}

static uint64_t m_execute(struct cobj *o, uint64_t *a, const uint64_t *f);

static const method context_fns[] = {
    IUNKNOWN, [3] = m_get_device, m_get_private, m_set_private, m_set_private_iface,
    [7] = m_vs_set_cbs, m_ps_set_srvs, m_ps_set_shader, m_ps_set_samplers, m_vs_set_shader, m_draw_indexed, m_draw,
    m_map, m_zero, m_ps_set_cbs, m_ia_set_layout, m_ia_set_vbs, m_ia_set_ib, m_draw_indexed_instanced,
    m_draw_instanced, m_gs_set_cbs, m_gs_set_shader, m_ia_set_topology, m_vs_set_srvs, m_vs_set_samplers, m_begin,
    m_end, m_get_data, m_set_predication, m_gs_set_srvs, m_gs_set_samplers, m_om_set_targets,
    m_om_set_targets_uavs, m_om_set_blend, m_om_set_depth, m_so_set_targets, m_draw_auto,
    m_draw_indexed_indirect, m_draw_indirect, m_dispatch, m_dispatch_indirect,
    m_rs_set_state, m_rs_set_viewports, m_rs_set_scissors, m_copy_region, m_copy_resource, m_update,
    m_copy_structure_count, m_clear_rtv, m_clear_uav_uint, m_clear_uav_float, m_clear_dsv, m_generate_mips, m_zero /* SetResourceMinLOD */,
    m_get_min_lod, m_resolve, m_execute, m_hs_set_srvs, m_hs_set_shader, m_hs_set_samplers,
    m_hs_set_cbs, m_ds_set_srvs, m_ds_set_shader, m_ds_set_samplers, m_ds_set_cbs, m_cs_set_srvs,
    m_cs_set_uavs, m_cs_set_shader, m_cs_set_samplers, m_cs_set_cbs,
    [72] = m_vs_get_cbs, m_ps_get_srvs, m_ps_get_shader, m_ps_get_samplers, m_vs_get_shader, m_ps_get_cbs,
    m_ia_get_layout, m_ia_get_vbs, m_ia_get_ib, m_gs_get_cbs, m_gs_get_shader, m_ia_get_topology, m_vs_get_srvs,
    m_vs_get_samplers, m_get_predication, m_gs_get_srvs, m_gs_get_samplers, m_om_get_targets,
    m_om_get_targets_uavs, m_om_get_blend, m_om_get_depth, m_so_get_targets, m_rs_get_state, m_rs_get_viewports,
    m_rs_get_scissors, m_hs_get_srvs, m_hs_get_shader, m_hs_get_samplers, m_hs_get_cbs, m_ds_get_srvs,
    m_ds_get_shader, m_ds_get_samplers, m_ds_get_cbs, m_cs_get_srvs, m_cs_get_uavs,
    m_cs_get_shader, m_cs_get_samplers, m_cs_get_cbs,
    [110] = m_clear_state, m_zero /* Flush */, m_context_type, m_zero /* GetContextFlags */, m_finish,
    /* ID3D11DeviceContext1 */
    [115] = m_copy_region, m_update, m_zero /* DiscardResource */, m_zero /* DiscardView */, m_vs_set_cbs1,
    m_hs_set_cbs1, m_ds_set_cbs1, m_gs_set_cbs1, m_ps_set_cbs1, m_cs_set_cbs1, m_vs_get_cbs1, m_hs_get_cbs1,
    m_ds_get_cbs1, m_gs_get_cbs1, m_ps_get_cbs1, m_cs_get_cbs1, m_zero /* SwapDeviceContextState */, m_clear_view,
    m_zero /* DiscardView1 */,
    /* ID3D11DeviceContext2 */
    [134] = m_notimpl, m_notimpl, m_zero, m_zero, m_notimpl, m_zero, m_zero /* IsAnnotationEnabled */, m_zero,
    m_zero, m_zero,
};

static void drop_program_ref(struct cobj *o)
{
    o->refs = 1;
    m_release(o, NULL, NULL);
}

static void replay(struct cobj *o, struct call *list)
{
    for (struct call *c = list; c; c = c->next) {
        if (c->slot == SLOT_FILL) {
            struct gd_layout l;
            uint8_t *dst = gd_subresource(c->held[0]->res.r, (uint32_t)c->a[2], &l);
            if (dst && c->guest)
                memcpy(dst, c->guest, MIN(l.size, c->guest_size));
            continue;
        }
        method fn = c->slot < ARRAY_SIZE(context_fns) ? context_fns[c->slot] : NULL;
        if (!fn)
            continue;
        uint64_t a[10];
        memcpy(a, c->a, sizeof(a));
        a[0] = iface(o, 0);
        fn(o, a, c->f);
    }
}

/* ExecuteCommandList(list, restore context state) */
static uint64_t m_execute(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *l = unwrap_kind(a[1], K_CMDLIST);
    if (!l)
        return 0;
    struct context *c = o->ctx;
    struct context *saved = (uint32_t)a[2] ? kmalloc(sizeof(*saved)) : NULL;
    if (saved) {
        memcpy(saved, c, sizeof(*saved));
        context_hold_all(saved, true);
    }
    context_unbind_all(c);
    gd_context_clear(c->gd);
    replay(o, l->calls);
    context_unbind_all(c);
    if (saved) {
        struct gd_context *gd = c->gd;
        struct call *first = c->first, *last = c->last;
        memcpy(c, saved, sizeof(*c)); /* the references saved are the context's again */
        c->gd = gd;
        c->first = first;
        c->last = last;
        kfree(saved);
        context_push(c);
    } else {
        gd_context_clear(c->gd);
    }
    return 0;
}

static uint64_t m_deferred_context(struct cobj *dev, uint64_t *a, const uint64_t *f)
{
    if (!a[2])
        return E_INVALIDARG;
    put64(a[2], 0);
    struct cobj *o = child(K_CONTEXT, dev, SP_CONTEXT, -1, -1);
    struct context *c = o ? kzalloc(sizeof(*c)) : NULL;
    if (c && !(c->gd = gd_context_create())) {
        kfree(c);
        c = NULL;
    }
    if (!c) {
        if (o)
            drop_program_ref(o);
        return E_OUTOFMEMORY;
    }
    context_unbind_all(c);
    c->deferred = true;
    o->ctx = c;
    put64(a[2], iface(o, 0));
    return S_OK;
}

static const method cmdlist_fns[] = {DEVICE_CHILD, [7] = m_zero /* GetContextFlags */};


/* ---- who asks: Chromium keeps a PC without a graphics driver */

static W32_STATE int chromium_answer; /* 0 not looked yet, 1 no, 2 yes */
static bool chromium(void)
{
    if (w32_d3d11_chromium())
        return false;
    if (w32_d3d11_for)
        return true;
    if (!chromium_answer) {
        bool yes = w32->cmdline && strstr(w32->cmdline, "--type=");
        if (!yes && w32->main) {
            char p[PATH_MAX_LEN];
            str_copy(p, w32->main->path, sizeof(p));
            char *slash = strrchr(p, '/');
            if (slash && (size_t)(slash - p) + 16 < sizeof(p)) {
                str_copy(slash + 1, "libGLESv2.dll", sizeof(p) - (size_t)(slash + 1 - p));
                yes = vfs_exists(p);
            }
        }
        chromium_answer = yes ? 2 : 1;
    }
    return chromium_answer == 2;
}

static void *user32(const char *name)
{
    return w32_builtin_lookup("user32.dll", name);
}

static void screen_size(uint32_t *w, uint32_t *h)
{
    int(WINAPI * metrics)(int) = (int(WINAPI *)(int))user32("GetSystemMetrics");
    int x = metrics ? metrics(0) : 0, y = metrics ? metrics(1) : 0;
    *w = x > 0 ? (uint32_t)x : 1024;
    *h = y > 0 ? (uint32_t)y : 768;
}

static bool client_size(HANDLE window, uint32_t *w, uint32_t *h)
{
    BOOL(WINAPI * get)(HANDLE, RECT *) = (BOOL(WINAPI *)(HANDLE, RECT *))user32("GetClientRect");
    RECT r = {0};
    if (!get || !get(window, &r) || r.right <= r.left || r.bottom <= r.top)
        return false;
    *w = (uint32_t)(r.right - r.left);
    *h = (uint32_t)(r.bottom - r.top);
    return true;
}

/* ---- DXGI: the factory, the adapter and its one output */

static struct cobj *dxgi_object(struct cobj **slot, uint32_t kind, int sp)
{
    if (!*slot && (*slot = new_obj(kind, NULL, sp, -1, -1)))
        (*slot)->irefs = 1;
    return *slot;
}

static struct cobj *factory(void)
{
    return dxgi_object(&factory_obj, K_FACTORY, SP_FACTORY);
}

static struct cobj *adapter(void)
{
    return dxgi_object(&adapter_obj, K_ADAPTER, SP_ADAPTER);
}

static struct cobj *output(void)
{
    return dxgi_object(&output_obj, K_OUTPUT, SP_OUTPUT);
}

/* DXGI_ADAPTER_DESC, 1, 2 and 3: the name, the ids, memory, the LUID, then flags (version 1 on) */
static void adapter_desc(uint64_t out, int version)
{
    if (!out)
        return;
    uint8_t d[320] = {0};
    bool basic = chromium();
    const char *name = basic ? "Microsoft Basic Render Driver" : "SynthOS Graphics";
    for (size_t i = 0; name[i]; i++)
        d[i * 2] = (uint8_t)name[i];
    const uint32_t ids[4] = {basic ? 0x1414u : 0x1af4u, basic ? 0x8cu : 0x1050u, 0, 1};
    memcpy(d + 256, ids, sizeof(ids));
    uint64_t dedicated = basic ? 0 : 2048ull << 20, shared = basic ? 256ull << 20 : 4096ull << 20;
    memcpy(d + 272, &dedicated, 8);
    memcpy(d + 288, &shared, 8);
    const uint32_t luid[2] = {0x5359, 0};
    memcpy(d + 296, luid, 8);
    uint32_t flags = basic ? 2 : 0;
    memcpy(d + 304, &flags, 4);
    size_t size = version == 0 ? 304 : version == 1 ? 312 : 320;
    memcpy(w64_ptr(out), d, size);
}

static uint64_t dxgi_parent(struct cobj *parent, uint64_t *a)
{
    if (!parent) {
        put64(a[2], 0);
        return E_NOINTERFACE;
    }
    return query_out(parent, w64_ptr(a[1]), a[2]);
}

static uint64_t m_factory_parent(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[2], 0);
    return E_NOINTERFACE;
}

/* EnumAdapters(index, out) and EnumAdapters1 */
static uint64_t m_enum_adapters(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[2])
        return E_POINTER;
    if ((uint32_t)a[1]) {
        put64(a[2], 0);
        return DXGI_ERROR_NOT_FOUND;
    }
    struct cobj *ad = adapter();
    if (!ad)
        return E_OUTOFMEMORY;
    __atomic_add_fetch(&ad->refs, 1, __ATOMIC_RELAXED);
    put64(a[2], iface(ad, 0));
    return S_OK;
}

/* EnumAdapterByGpuPreference(index, preference, iid, out) */
static uint64_t m_enum_by_preference(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if ((uint32_t)a[1]) {
        put64(a[4], 0);
        return DXGI_ERROR_NOT_FOUND;
    }
    struct cobj *ad = adapter();
    return ad ? query_out(ad, w64_ptr(a[3]), a[4]) : E_OUTOFMEMORY;
}

/* EnumAdapterByLuid(luid, iid, out) */
static uint64_t m_enum_by_luid(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *ad = adapter();
    return ad ? query_out(ad, w64_ptr(a[2]), a[3]) : E_OUTOFMEMORY;
}

/* EnumWarpAdapter(iid, out) */
static uint64_t m_enum_warp(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *ad = adapter();
    return ad ? query_out(ad, w64_ptr(a[1]), a[2]) : E_OUTOFMEMORY;
}

static uint64_t m_get_window_assoc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[1], 0);
    return S_OK;
}

static uint64_t m_software_adapter(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[2], 0);
    return DXGI_ERROR_UNSUPPORTED;
}

static uint64_t m_cookie(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[2], 1); /* Register...(window or event, cookie *) */
    return S_OK;
}

static uint64_t m_shared_luid(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[2], 0x5359);
    return S_OK;
}

/* CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, BOOL *, size) */
static uint64_t m_factory_feature(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if ((uint32_t)a[1] || (uint32_t)a[3] < 4)
        return E_INVALIDARG;
    put32(a[2], 1);
    return S_OK;
}

static uint64_t create_swapchain(struct cobj *dev, HANDLE window, const uint32_t *d1, uint32_t windowed,
                                 uint64_t out);

/* CreateSwapChain(device, DXGI_SWAP_CHAIN_DESC *, out) */
static uint64_t m_create_swapchain(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[3], 0);
    struct cobj *dev = unwrap_kind(a[1], K_DEVICE);
    if (!dev || !a[2])
        return DXGI_ERROR_INVALID_CALL;
    const uint32_t *d = w64_ptr(a[2]);
    uint64_t window;
    memcpy(&window, d + 12, 8);
    /* as DXGI_SWAP_CHAIN_DESC1: width, height, format, stereo, samples, quality, usage, count, scaling, effect,
       alpha, flags */
    uint32_t d1[12] = {d[0], d[1], d[4], 0, d[7], d[8], d[9], d[10], 0, d[15], 0, d[16]};
    return create_swapchain(dev, (HANDLE)(uintptr_t)window, d1, d[14], a[3]);
}

/* CreateSwapChainForHwnd(device, window, DXGI_SWAP_CHAIN_DESC1 *, fullscreen desc, output, out) */
static uint64_t m_create_swapchain_hwnd(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[6], 0);
    struct cobj *dev = unwrap_kind(a[1], K_DEVICE);
    if (!dev || !a[3])
        return DXGI_ERROR_INVALID_CALL;
    uint32_t windowed = a[4] ? get32(a[4] + 16) : 1;
    return create_swapchain(dev, (HANDLE)(uintptr_t)a[2], w64_ptr(a[3]), windowed, a[6]);
}

static const method factory_fns[] = {
    DXGI_OBJECT, [6] = m_factory_parent, m_enum_adapters, m_ok /* MakeWindowAssociation */, m_get_window_assoc,
    m_create_swapchain, m_software_adapter,
    /* IDXGIFactory1 */
    [12] = m_enum_adapters, m_one /* IsCurrent */,
    /* IDXGIFactory2 */
    [14] = m_zero /* IsWindowedStereoEnabled */, m_create_swapchain_hwnd, m_notimpl /* ForCoreWindow */,
    m_shared_luid, m_cookie, m_cookie, m_zero, m_cookie, m_cookie, m_zero, m_notimpl /* ForComposition */,
    /* IDXGIFactory3 to 6 */
    [25] = m_zero /* GetCreationFlags */, m_enum_by_luid, m_enum_warp, m_factory_feature, m_enum_by_preference,
};

/* ---- the adapter */

static uint64_t m_adapter_parent(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return dxgi_parent(factory(), a);
}

static uint64_t m_enum_outputs(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[2])
        return E_POINTER;
    struct cobj *out = (uint32_t)a[1] ? NULL : output();
    if (!out) {
        put64(a[2], 0);
        return DXGI_ERROR_NOT_FOUND;
    }
    __atomic_add_fetch(&out->refs, 1, __ATOMIC_RELAXED);
    put64(a[2], iface(out, 0));
    return S_OK;
}

static uint64_t m_adapter_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    adapter_desc(a[1], 0);
    return a[1] ? S_OK : E_INVALIDARG;
}

static uint64_t m_adapter_desc1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    adapter_desc(a[1], 1);
    return a[1] ? S_OK : E_INVALIDARG;
}

static uint64_t m_adapter_desc2(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    adapter_desc(a[1], 2);
    return a[1] ? S_OK : E_INVALIDARG;
}

/* CheckInterfaceSupport(iid, LARGE_INTEGER *version): the user-mode driver of a Windows 10 install */
static uint64_t m_interface_support(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[2], 19041u << 16 | 1);
    put32(a[2] + 4, 10u << 16);
    return S_OK;
}

/* QueryVideoMemoryInfo(node, group, info *): budget, usage, available for reservation, reservation */
static uint64_t m_video_memory(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[3])
        return E_INVALIDARG;
    uint64_t v[4] = {(uint32_t)a[2] ? 2048ull << 20 : 1536ull << 20, 0, 1024ull << 20, 0};
    memcpy(w64_ptr(a[3]), v, sizeof(v));
    return S_OK;
}

static const method adapter_fns[] = {
    DXGI_OBJECT, [6] = m_adapter_parent, m_enum_outputs, m_adapter_desc, m_interface_support,
    [10] = m_adapter_desc1, m_adapter_desc2, m_cookie, m_zero, m_video_memory, m_ok, m_cookie, m_zero,
    [18] = m_adapter_desc2,
};

/* ---- the output: the screen */

static bool display_format(uint32_t f)
{
    return f == GD_R8G8B8A8_UNORM || f == GD_R8G8B8A8_UNORM_SRGB || f == GD_B8G8R8A8_UNORM ||
           f == GD_B8G8R8A8_UNORM_SRGB || f == GD_R10G10B10A2_UNORM || f == GD_R16G16B16A16_FLOAT;
}

/* the modes the screen offers: common sizes up to its own, and its own */
static uint32_t modes(uint32_t (*out)[2])
{
    static const uint32_t common[][2] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 720},
                                         {1280, 800},  {1366, 768},  {1600, 900},  {1920, 1080},
                                         {2560, 1440}, {3840, 2160}};
    uint32_t sw, sh, n = 0;
    screen_size(&sw, &sh);
    bool native = false;
    for (size_t i = 0; i < ARRAY_SIZE(common); i++)
        if (common[i][0] <= sw && common[i][1] <= sh) {
            out[n][0] = common[i][0], out[n][1] = common[i][1], n++;
            native |= common[i][0] == sw && common[i][1] == sh;
        }
    if (!native)
        out[n][0] = sw, out[n][1] = sh, n++;
    return n;
}

static void mode_out(uint8_t *p, uint32_t w, uint32_t h, uint32_t format, bool v1)
{
    uint32_t m[8] = {w, h, 60, 1, format, 1 /* progressive */, 0, 0};
    memcpy(p, m, v1 ? 32 : 28);
}

/* GetDisplayModeList(format, flags, UINT *count, DXGI_MODE_DESC *) and the 1 version */
static uint64_t mode_list(uint64_t *a, bool v1)
{
    if (!a[3])
        return E_INVALIDARG;
    uint32_t list[16][2], n = display_format((uint32_t)a[1]) ? modes(list) : 0;
    if (!a[4]) {
        put32(a[3], n);
        return S_OK;
    }
    uint32_t room = get32(a[3]), size = v1 ? 32 : 28;
    uint8_t *p = w64_ptr(a[4]);
    for (uint32_t i = 0; i < MIN(room, n); i++)
        mode_out(p + i * size, list[i][0], list[i][1], (uint32_t)a[1], v1);
    put32(a[3], MIN(room, n));
    return room < n ? DXGI_ERROR_MORE_DATA : S_OK;
}

static uint64_t m_mode_list(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return mode_list(a, false);
}

static uint64_t m_mode_list1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return mode_list(a, true);
}

/* FindClosestMatchingMode(mode, closest, device): the nearest size the screen has */
static uint64_t closest_mode(uint64_t *a, bool v1)
{
    if (!a[1] || !a[2])
        return E_INVALIDARG;
    const uint32_t *want = w64_ptr(a[1]);
    uint32_t list[16][2], n = modes(list), best = n - 1;
    uint64_t best_d = ~0ull;
    for (uint32_t i = 0; i < n && want[0] && want[1]; i++) {
        int64_t dx = (int64_t)list[i][0] - want[0], dy = (int64_t)list[i][1] - want[1];
        uint64_t d = (uint64_t)(dx * dx + dy * dy);
        if (d < best_d)
            best_d = d, best = i;
    }
    mode_out(w64_ptr(a[2]), list[best][0], list[best][1],
             display_format(want[4]) ? want[4] : GD_B8G8R8A8_UNORM, v1);
    return S_OK;
}

static uint64_t m_closest_mode(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return closest_mode(a, false);
}

static uint64_t m_closest_mode1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return closest_mode(a, true);
}

/* DXGI_OUTPUT_DESC: the name, the desktop rectangle, attached, rotation, the monitor */
static uint64_t m_output_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    uint8_t d[96] = {0};
    static const char name[] = "\\\\.\\DISPLAY1";
    for (size_t i = 0; name[i]; i++)
        d[i * 2] = (uint8_t)name[i];
    uint32_t w, h;
    screen_size(&w, &h);
    uint32_t r[4] = {0, 0, w, h}, attached = 1, rotation = 1;
    memcpy(d + 64, r, 16);
    memcpy(d + 80, &attached, 4);
    memcpy(d + 84, &rotation, 4);
    HANDLE(WINAPI * monitor)(HANDLE, DWORD) = (HANDLE(WINAPI *)(HANDLE, DWORD))user32("MonitorFromWindow");
    uint64_t m = monitor ? (uintptr_t)monitor(NULL, 1) : 0;
    memcpy(d + 88, &m, 8);
    memcpy(w64_ptr(a[1]), d, sizeof(d));
    return S_OK;
}

static uint64_t m_output_parent(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return dxgi_parent(adapter(), a);
}

static uint64_t m_gamma_caps(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1])
        memset(w64_ptr(a[1]), 0, 16);
    return S_OK;
}

static uint64_t m_frame_stats(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1])
        memset(w64_ptr(a[1]), 0, 32);
    return S_OK;
}

static const method output_fns[] = {
    DXGI_OBJECT, [6] = m_output_parent, m_output_desc, m_mode_list, m_closest_mode, m_ok /* WaitForVBlank */,
    m_ok /* TakeOwnership */, m_zero /* ReleaseOwnership */, m_gamma_caps, m_ok, m_notimpl, m_ok, m_notimpl,
    m_frame_stats,
    /* IDXGIOutput1 */
    [19] = m_mode_list1, m_closest_mode1, m_notimpl, m_unsupported /* DuplicateOutput */,
};

/* ---- the swap chain */

static struct cobj *swap_buffer(struct cobj *dev, struct swapchain *s)
{
    struct gd_res_desc d = {.dim = GD_TEX2D, .width = s->width, .height = s->height, .mips = 1, .array = 1,
                            .format = s->format, .samples = 1,
                            .bind = GD_BIND_RENDER_TARGET | GD_BIND_SHADER_RESOURCE};
    uint64_t got = 0;
    if (create_resource(dev, K_TEX2D, &d, 0, (uintptr_t)&got) != S_OK)
        return NULL;
    struct cobj *b = unwrap(got);
    if (!b)
        return NULL;
    b->refs = 0; /* the swap chain's: the program's references come from GetBuffer */
    b->irefs = 1;
    return b;
}

static void fit_window(struct swapchain *s)
{
    uint32_t w, h;
    bool known = s->window && client_size(s->window, &w, &h);
    if (!s->width)
        s->width = known ? w : 640;
    if (!s->height)
        s->height = known ? h : 480;
}

static uint64_t create_swapchain(struct cobj *dev, HANDLE window, const uint32_t *d1, uint32_t windowed, uint64_t out)
{
    if (!window || !out)
        return DXGI_ERROR_INVALID_CALL;
    uint32_t format = d1[2];
    if (!display_format(format) && format != GD_B8G8R8X8_UNORM)
        return DXGI_ERROR_INVALID_CALL;
    struct swapchain *s = kzalloc(sizeof(*s));
    if (!s)
        return E_OUTOFMEMORY;
    *s = (struct swapchain){window, NULL, d1[0], d1[1], format, d1[7] ? d1[7] : 1, d1[6], d1[11], d1[9], d1[8],
                            d1[10], windowed, 0, 0, false};
    fit_window(s);
    struct cobj *o = new_obj(K_SWAPCHAIN, dev, SP_SWAPCHAIN, -1, -1);
    if (!o || !(s->buffer = swap_buffer(dev, s))) {
        if (o)
            w32_free(o); /* never seen by the program */
        kfree(s);
        return E_OUTOFMEMORY;
    }
    hold(dev);
    o->sc = s;
    s->buffer->res.swapchain = o;
    put64(out, iface(o, 0));
    return S_OK;
}

/* Present: the back buffer into the window's client area, stretched to it */
static uint64_t present(struct cobj *o, uint32_t sync, uint32_t flags)
{
    struct swapchain *s = o->sc;
    if (flags & 1) /* DXGI_PRESENT_TEST */
        return S_OK;
    uint32_t sw, sh;
    screen_size(&sw, &sh);
    bool shown = !s->windowed && gd_scanout(s->buffer->res.r, sw, sh);
    if (s->scanout && !shown)
        gd_scanout(NULL, 0, 0);
    s->scanout = shown;
    HANDLE(WINAPI * get_dc)(HANDLE) = (HANDLE(WINAPI *)(HANDLE))user32("GetDC");
    int(WINAPI * release_dc)(HANDLE, HANDLE) = (int(WINAPI *)(HANDLE, HANDLE))user32("ReleaseDC");
    uint32_t w, h;
    HANDLE dc = !shown && get_dc && client_size(s->window, &w, &h) ? get_dc(s->window) : NULL;
    if (dc) {
        int ox, oy;
        struct rect clip;
        struct canvas *c = w32_dc_target(dc, &ox, &oy, &clip);
        if (c) {
            gd_present(s->buffer->res.r, 0, c->px, c->stride, ox, oy, (int)w, (int)h, clip.x, clip.y, clip.w,
                       clip.h);
            w32_dc_damage(dc, (struct rect){ox, oy, (int)w, (int)h});
        }
        if (release_dc)
            release_dc(s->window, dc);
    }
    s->presents++;
    uint64_t now = now_ms();
    if (sync && now - s->last_present < 16 * sync) {
        void(WINAPI * sleep)(DWORD) = (void(WINAPI *)(DWORD))w32_builtin_lookup("kernel32.dll", "Sleep");
        if (sleep)
            sleep((DWORD)(16 * sync - (now - s->last_present)));
        now = now_ms();
    } else {
        w32_desktop_turn();
    }
    s->last_present = now;
    return S_OK;
}

static uint64_t m_present(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return present(o, (uint32_t)a[1], (uint32_t)a[2]);
}

static uint64_t m_sc_device(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return dxgi_parent(o->device, a);
}

static uint64_t m_sc_parent(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return dxgi_parent(factory(), a);
}

/* GetBuffer(index, iid, out): one buffer, renamed by Present as Direct3D 11 does */
static uint64_t m_get_buffer(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[3])
        return E_POINTER;
    return query_out(o->sc->buffer, w64_ptr(a[2]), a[3]);
}

static uint64_t m_set_fullscreen(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    o->sc->windowed = !(uint32_t)a[1];
    if (o->sc->windowed && o->sc->scanout)
        gd_scanout(NULL, 0, 0);
    o->sc->scanout = o->sc->scanout && !o->sc->windowed;
    return S_OK;
}

static uint64_t m_get_fullscreen(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], !o->sc->windowed);
    if (a[2]) {
        struct cobj *out = o->sc->windowed ? NULL : output();
        put_obj(a[2], out);
    }
    return S_OK;
}

static uint64_t m_sc_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    struct swapchain *s = o->sc;
    uint32_t d[18] = {s->width, s->height, 60, 1, s->format, 0, 0, 1, 0, s->usage, s->count, 0};
    uint64_t window = (uintptr_t)s->window;
    memcpy(d + 12, &window, 8);
    d[14] = s->windowed, d[15] = s->effect, d[16] = s->flags;
    memcpy(w64_ptr(a[1]), d, 72);
    return S_OK;
}

static uint64_t m_sc_desc1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    struct swapchain *s = o->sc;
    uint32_t d[12] = {s->width, s->height, s->format, 0, 1, 0, s->usage, s->count, s->scaling, s->effect, s->alpha,
                      s->flags};
    memcpy(w64_ptr(a[1]), d, sizeof(d));
    return S_OK;
}

static uint64_t m_sc_fullscreen_desc(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (!a[1])
        return E_INVALIDARG;
    uint32_t d[5] = {60, 1, 0, 0, o->sc->windowed};
    memcpy(w64_ptr(a[1]), d, sizeof(d));
    return S_OK;
}

/* ResizeBuffers(count, width, height, format, flags): a new back buffer */
static uint64_t m_resize_buffers(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct swapchain *s = o->sc;
    struct swapchain t = *s;
    if ((uint32_t)a[1])
        t.count = (uint32_t)a[1];
    t.width = (uint32_t)a[2];
    t.height = (uint32_t)a[3];
    if ((uint32_t)a[4])
        t.format = (uint32_t)a[4];
    t.flags = (uint32_t)a[5];
    fit_window(&t);
    struct cobj *b = swap_buffer(o->device, &t);
    if (!b)
        return E_OUTOFMEMORY;
    b->res.swapchain = o;
    s->buffer->res.swapchain = NULL;
    drop(s->buffer);
    t.buffer = b;
    *s = t;
    return S_OK;
}

static uint64_t m_containing_output(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put_obj(a[1], output());
    return a[1] ? S_OK : E_INVALIDARG;
}

static uint64_t m_present_count(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->sc->presents);
    return S_OK;
}

static uint64_t m_sc_hwnd(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[1], (uintptr_t)o->sc->window);
    return S_OK;
}

static uint64_t m_present1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return present(o, (uint32_t)a[1], (uint32_t)a[2]);
}

static uint64_t m_restrict_output(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put64(a[1], 0);
    return S_OK;
}

static uint64_t m_background(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    if (a[1])
        memset(w64_ptr(a[1]), 0, 16);
    return S_OK;
}

static uint64_t m_rotation(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 1);
    return S_OK;
}

static uint64_t m_source_size(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], o->sc->width);
    put32(a[2], o->sc->height);
    return S_OK;
}

static uint64_t m_frame_latency(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 1);
    return S_OK;
}

/* GetFrameLatencyWaitableObject: an event that is always set, Present being synchronous */
static uint64_t m_latency_object(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    HANDLE(WINAPI * make)(void *, BOOL, BOOL, const void *) =
        (HANDLE(WINAPI *)(void *, BOOL, BOOL, const void *))w32_builtin_lookup("kernel32.dll", "CreateEventW");
    return make ? (uintptr_t)make(NULL, TRUE, TRUE, NULL) : 0;
}

static uint64_t m_color_space(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[2], (uint32_t)a[1] == 0 ? 1 : 0); /* sRGB, full range */
    return S_OK;
}

static uint64_t m_resize_buffers1(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return m_resize_buffers(o, a, f);
}

static const method swapchain_fns[] = {
    DXGI_OBJECT, [6] = m_sc_parent, m_sc_device, m_present, m_get_buffer, m_set_fullscreen, m_get_fullscreen,
    m_sc_desc, m_resize_buffers, m_ok /* ResizeTarget */, m_containing_output, m_frame_stats, m_present_count,
    /* IDXGISwapChain1 */
    [18] = m_sc_desc1, m_sc_fullscreen_desc, m_sc_hwnd, m_notimpl /* GetCoreWindow */, m_present1,
    m_zero /* IsTemporaryMonoSupported */, m_restrict_output, m_ok, m_background, m_ok, m_rotation,
    /* IDXGISwapChain2 */
    [29] = m_ok /* SetSourceSize */, m_source_size, m_ok, m_frame_latency, m_latency_object, m_notimpl, m_notimpl,
    /* IDXGISwapChain3 and 4 */
    [36] = m_zero /* GetCurrentBackBufferIndex */, m_color_space, m_ok, m_resize_buffers1, m_ok,
};

/* ---- IDXGIDevice on the device */

static uint64_t m_dxgi_device_parent(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    return dxgi_parent(adapter(), a);
}

static uint64_t m_get_adapter(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    struct cobj *ad = adapter();
    put_obj(a[1], ad);
    return ad ? S_OK : E_OUTOFMEMORY;
}

/* QueryResourceResidency(resources, status, count): all of it is in memory */
static uint64_t m_residency(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    for (uint32_t i = 0; i < (uint32_t)a[3] && a[2]; i++)
        put32(a[2] + i * 4ull, 1);
    return S_OK;
}

static uint64_t m_thread_priority(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 0);
    return S_OK;
}

static uint64_t m_max_latency(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    put32(a[1], 3);
    return S_OK;
}

static uint64_t m_reclaim(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    for (uint32_t i = 0; i < (uint32_t)a[1] && a[3]; i++)
        put32(a[3] + i * 4ull, 0);
    return S_OK;
}

static uint64_t m_set_event(struct cobj *o, uint64_t *a, const uint64_t *f)
{
    BOOL(WINAPI * set)(HANDLE) = (BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("kernel32.dll", "SetEvent");
    if (set && a[1])
        set((HANDLE)(uintptr_t)a[1]);
    return S_OK;
}

static const method dxgi_device_fns[] = {
    DXGI_OBJECT, [6] = m_dxgi_device_parent, m_get_adapter, m_notimpl /* CreateSurface */, m_residency, m_ok,
    m_thread_priority, m_ok /* SetMaximumFrameLatency */, m_max_latency, m_ok /* OfferResources */, m_reclaim,
    m_set_event, m_zero /* Trim */,
};

/* ---- the other interfaces' tables */

static const method child_fns[] = {DEVICE_CHILD};

static const method buffer_fns[] = {
    DEVICE_CHILD, [7] = m_res_type, m_zero /* SetEvictionPriority */, m_eviction_value, m_res_desc,
};

static const method surface_fns[] = {
    DXGI_OBJECT, [6] = m_dxgi_parent, m_dxgi_parent, m_surface_desc, m_surface_map, m_ok /* Unmap */, m_notimpl,
    m_ok /* ReleaseDC */,
};

static const method dxgi_resource_fns[] = {
    DXGI_OBJECT, [6] = m_dxgi_parent, m_dxgi_parent, m_shared_handle, m_usage, m_ok, m_eviction, m_notimpl,
    m_notimpl,
};

static const method view_fns[] = {DEVICE_CHILD, [7] = m_view_resource, m_view_desc};
static const method blend_fns[] = {DEVICE_CHILD, [7] = m_blend_desc, m_blend_desc1};
static const method state_fns[] = {DEVICE_CHILD, [7] = m_state_desc, m_state_desc, m_state_desc};
static const method query_fns[] = {DEVICE_CHILD, [7] = m_query_size, m_query_desc};

#define SPEC(n, i, t) {n, i, ARRAY_SIZE(t), t}

static const struct vspec specs[SP_COUNT] = {
    [SP_DEVICE] = SPEC("ID3D11Device", 0, device_fns),
    [SP_DXGIDEVICE] = SPEC("IDXGIDevice", 1, dxgi_device_fns),
    [SP_CONTEXT] = SPEC("ID3D11DeviceContext", 0, context_fns),
    [SP_BUFFER] = SPEC("ID3D11Buffer", 0, buffer_fns),
    [SP_TEX1D] = SPEC("ID3D11Texture1D", 0, buffer_fns),
    [SP_TEX2D] = SPEC("ID3D11Texture2D", 0, buffer_fns),
    [SP_TEX3D] = SPEC("ID3D11Texture3D", 0, buffer_fns),
    [SP_SURFACE] = SPEC("IDXGISurface", 1, surface_fns),
    [SP_DXGIRES] = SPEC("IDXGIResource", 2, dxgi_resource_fns),
    [SP_SRV] = SPEC("ID3D11ShaderResourceView", 0, view_fns),
    [SP_RTV] = SPEC("ID3D11RenderTargetView", 0, view_fns),
    [SP_DSV] = SPEC("ID3D11DepthStencilView", 0, view_fns),
    [SP_UAV] = SPEC("ID3D11UnorderedAccessView", 0, view_fns),
    [SP_CHILD] = SPEC("ID3D11DeviceChild", 0, child_fns),
    [SP_BLEND] = SPEC("ID3D11BlendState", 0, blend_fns),
    [SP_DEPTH] = SPEC("ID3D11DepthStencilState", 0, state_fns),
    [SP_RASTER] = SPEC("ID3D11RasterizerState", 0, state_fns),
    [SP_SAMPLER] = SPEC("ID3D11SamplerState", 0, state_fns),
    [SP_QUERY] = SPEC("ID3D11Query", 0, query_fns),
    [SP_FACTORY] = SPEC("IDXGIFactory", 0, factory_fns),
    [SP_ADAPTER] = SPEC("IDXGIAdapter", 0, adapter_fns),
    [SP_OUTPUT] = SPEC("IDXGIOutput", 0, output_fns),
    [SP_SWAPCHAIN] = SPEC("IDXGISwapChain", 0, swapchain_fns),
    [SP_CMDLIST] = SPEC("ID3D11CommandList", 0, cmdlist_fns),
};

static const struct vspec *spec_of(int s)
{
    return &specs[s];
}

/* ---- interface ids each kind answers to */

#define CHILD_IIDS {IID_Unknown, 0}, {IID_DeviceChild, 0}
#define RES_IIDS                                                                                                   \
    CHILD_IIDS, {IID_Resource, 0}, {IID_Object, 2}, {IID_SubObject, 2}, {IID_DXGIResource, 2},                    \
        {IID_DXGIResource1, 2}

static const struct iid_map device_iids[] = {
    {IID_Unknown, 0}, {IID_Device, 0}, {IID_Device1, 0}, {IID_Device2, 0}, {IID_Object, 1}, {IID_DXGIDevice, 1},
    {IID_DXGIDevice1, 1}, {IID_DXGIDevice2, 1}, {IID_DXGIDevice3, 1}, {NULL, 0},
};
static const struct iid_map context_iids[] = {
    CHILD_IIDS, {IID_Context, 0}, {IID_Context1, 0}, {IID_Context2, 0}, {NULL, 0},
};
static const struct iid_map buffer_iids[] = {RES_IIDS, {IID_Buffer, 0}, {NULL, 0}};
static const struct iid_map tex1d_iids[] = {RES_IIDS, {IID_Texture1D, 0}, {NULL, 0}};
static const struct iid_map tex2d_iids[] = {
    RES_IIDS, {IID_Texture2D, 0}, {IID_Surface, 1}, {IID_Surface1, 1}, {NULL, 0},
};
static const struct iid_map tex3d_iids[] = {RES_IIDS, {IID_Texture3D, 0}, {NULL, 0}};
static const struct iid_map srv_iids[] = {CHILD_IIDS, {IID_View, 0}, {IID_SRV, 0}, {NULL, 0}};
static const struct iid_map rtv_iids[] = {CHILD_IIDS, {IID_View, 0}, {IID_RTV, 0}, {NULL, 0}};
static const struct iid_map dsv_iids[] = {CHILD_IIDS, {IID_View, 0}, {IID_DSV, 0}, {NULL, 0}};
static const struct iid_map uav_iids[] = {CHILD_IIDS, {IID_View, 0}, {IID_UAV, 0}, {NULL, 0}};
static const struct iid_map layout_iids[] = {CHILD_IIDS, {IID_Layout, 0}, {NULL, 0}};
static const struct iid_map ps_iids[] = {CHILD_IIDS, {IID_PS, 0}, {NULL, 0}};
static const struct iid_map vs_iids[] = {CHILD_IIDS, {IID_VS, 0}, {NULL, 0}};
static const struct iid_map gs_iids[] = {CHILD_IIDS, {IID_GS, 0}, {NULL, 0}};
static const struct iid_map hs_iids[] = {CHILD_IIDS, {IID_HS, 0}, {NULL, 0}};
static const struct iid_map ds_iids[] = {CHILD_IIDS, {IID_DS, 0}, {NULL, 0}};
static const struct iid_map cs_iids[] = {CHILD_IIDS, {IID_CS, 0}, {NULL, 0}};
static const struct iid_map blend_iids[] = {CHILD_IIDS, {IID_Blend, 0}, {NULL, 0}};
static const struct iid_map depth_iids[] = {CHILD_IIDS, {IID_Depth, 0}, {NULL, 0}};
static const struct iid_map raster_iids[] = {CHILD_IIDS, {IID_Raster, 0}, {NULL, 0}};
static const struct iid_map sampler_iids[] = {CHILD_IIDS, {IID_Sampler, 0}, {NULL, 0}};
static const struct iid_map query_iids[] = {CHILD_IIDS, {IID_Async, 0}, {IID_Query, 0}, {NULL, 0}};
static const struct iid_map predicate_iids[] = {
    CHILD_IIDS, {IID_Async, 0}, {IID_Query, 0}, {IID_Predicate, 0}, {NULL, 0},
};
static const struct iid_map factory_iids[] = {
    {IID_Unknown, 0}, {IID_Object, 0}, {IID_Factory, 0}, {IID_Factory1, 0}, {IID_Factory2, 0}, {IID_Factory3, 0},
    {IID_Factory4, 0}, {IID_Factory5, 0}, {IID_Factory6, 0}, {NULL, 0},
};
static const struct iid_map adapter_iids[] = {
    {IID_Unknown, 0}, {IID_Object, 0}, {IID_Adapter, 0}, {IID_Adapter1, 0}, {IID_Adapter2, 0}, {IID_Adapter3, 0},
    {IID_Adapter4, 0}, {NULL, 0},
};
static const struct iid_map output_iids[] = {
    {IID_Unknown, 0}, {IID_Object, 0}, {IID_Output, 0}, {IID_Output1, 0}, {NULL, 0},
};
static const struct iid_map swapchain_iids[] = {
    {IID_Unknown, 0}, {IID_Object, 0}, {IID_SubObject, 0}, {IID_SwapChain, 0}, {IID_SwapChain1, 0},
    {IID_SwapChain2, 0}, {IID_SwapChain3, 0}, {NULL, 0},
};
static const struct iid_map plain_iids[] = {CHILD_IIDS, {NULL, 0}};
static const struct iid_map cmdlist_iids[] = {CHILD_IIDS, {IID_CommandList, 0}, {NULL, 0}};

static const struct iid_map *iids_of(uint32_t kind)
{
    static const struct iid_map *const by_kind[] = {
        [K_DEVICE] = device_iids,   [K_CONTEXT] = context_iids, [K_BUFFER] = buffer_iids,
        [K_TEX1D] = tex1d_iids,     [K_TEX2D] = tex2d_iids,     [K_TEX3D] = tex3d_iids,
        [K_SRV] = srv_iids,         [K_RTV] = rtv_iids,         [K_DSV] = dsv_iids,
        [K_UAV] = uav_iids,         [K_LAYOUT] = layout_iids,   [K_PS] = ps_iids,
        [K_VS] = vs_iids,           [K_GS] = gs_iids,           [K_HS] = hs_iids,
        [K_DS] = ds_iids,           [K_CS] = cs_iids,           [K_BLEND] = blend_iids,
        [K_DEPTH] = depth_iids,     [K_RASTER] = raster_iids,   [K_SAMPLER] = sampler_iids,
        [K_QUERY] = query_iids,     [K_PREDICATE] = predicate_iids, [K_FACTORY] = factory_iids,
        [K_ADAPTER] = adapter_iids, [K_OUTPUT] = output_iids,   [K_SWAPCHAIN] = swapchain_iids,
        [K_CLASSLINKAGE] = plain_iids, [K_CMDLIST] = cmdlist_iids,
    };
    return kind < ARRAY_SIZE(by_kind) ? by_kind[kind] : NULL;
}

/* ---- the exported functions */

/* the highest feature level asked for that the device has: 11_1 down to 9_1 */
static uint32_t pick_level(uint64_t levels, uint32_t count)
{
    if (!levels || !count)
        return 0xb000;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t l = get32(levels + i * 4ull);
        if (l == 0xb100 || l == 0xb000 || l == 0xa100 || l == 0xa000 || l == 0x9300 || l == 0x9200 || l == 0x9100)
            return l;
    }
    return 0;
}

static struct cobj *create_device(uint32_t level, uint32_t flags)
{
    struct cobj *dev = new_obj(K_DEVICE, NULL, SP_DEVICE, SP_DXGIDEVICE, -1);
    struct cobj *ctx = dev ? new_obj(K_CONTEXT, dev, SP_CONTEXT, -1, -1) : NULL;
    struct context *c = ctx ? kzalloc(sizeof(*c)) : NULL;
    if (c && !(c->gd = gd_context_create())) {
        kfree(c);
        c = NULL;
    }
    if (!c)
        return NULL; /* the bits made so far go with the process */
    context_unbind_all(c);
    ctx->ctx = c;
    ctx->refs = 0;
    ctx->irefs = 1; /* the device's */
    dev->dev.level = level;
    dev->dev.flags = flags;
    dev->dev.context = ctx;
    return dev;
}

/* the answer to a program that may not have a device: no device and no context */
static uint64_t no_device(uint64_t dev_out, uint64_t ctx_out, uint64_t hr)
{
    put64(dev_out, 0);
    put64(ctx_out, 0);
    return hr;
}

static uint64_t device_for(uint64_t *a, uint64_t dev_out, uint64_t level_out, uint64_t ctx_out, struct cobj **made)
{
    *made = NULL;
    if (chromium())
        return no_device(dev_out, ctx_out, CLASS_NOT_REGISTERED);
    uint32_t level = pick_level(a[4], (uint32_t)a[5]);
    if (!level)
        return no_device(dev_out, ctx_out, E_INVALIDARG);
    put32(level_out, level);
    if (!dev_out && !ctx_out)
        return S_FALSE;
    struct cobj *dev = create_device(level, (uint32_t)a[3]);
    if (!dev)
        return no_device(dev_out, ctx_out, E_OUTOFMEMORY);
    if (ctx_out) {
        __atomic_add_fetch(&dev->dev.context->refs, 1, __ATOMIC_RELAXED);
        put64(ctx_out, iface(dev->dev.context, 0));
    }
    if (dev_out)
        put64(dev_out, iface(dev, 0));
    else
        dev->refs = 0; /* the context's alone: it lives with the process */
    *made = dev;
    return S_OK;
}

/* D3D11CreateDevice(adapter, driver type, software, flags, levels, count, sdk, device, level, context) */
static uint64_t h_create_device(uint64_t *a, const uint64_t *f, void *t)
{
    struct cobj *dev;
    return device_for(a, a[7], a[8], a[9], &dev);
}

/* D3D11CreateDeviceAndSwapChain(adapter, type, software, flags, levels, count, sdk, desc, swap chain, device,
   level, context) */
static uint64_t h_create_device_swapchain(uint64_t *a, const uint64_t *f, void *t)
{
    put64(a[8], 0);
    struct cobj *dev;
    uint64_t hr = device_for(a, a[9], a[10], a[11], &dev);
    if (hr != S_OK || !a[8] || !a[7])
        return hr;
    struct cobj *fac = factory();
    if (!fac)
        return E_OUTOFMEMORY;
    uint64_t b[4] = {iface(fac, 0), iface(dev, 0), a[7], a[8]};
    return m_create_swapchain(fac, b, f);
}

static uint64_t h_no_d3d11on12(uint64_t *a, const uint64_t *f, void *t)
{
    put64(a[8], 0);
    put64(a[10], 0);
    return E_NOTIMPL;
}

static const struct w64_export d3d11_exports[] = {
    W64_FN("D3D11CreateDevice", h_create_device),
    W64_FN("D3D11CreateDeviceAndSwapChain", h_create_device_swapchain),
    W64_FN("D3D11On12CreateDevice", h_no_d3d11on12),
};

W64_EXPORTS(d3d11_module, "d3d11.dll", d3d11_exports);

static uint64_t create_factory(const uint8_t *iid, uint64_t out)
{
    if (!out)
        return E_POINTER;
    struct cobj *fac = factory();
    if (!fac) {
        put64(out, 0);
        return DXGI_ERROR_UNSUPPORTED;
    }
    return query_out(fac, iid, out);
}

/* CreateDXGIFactory(iid, out), CreateDXGIFactory1 */
static uint64_t h_create_factory(uint64_t *a, const uint64_t *f, void *t)
{
    return create_factory(w64_ptr(a[0]), a[1]);
}

/* CreateDXGIFactory2(flags, iid, out) */
static uint64_t h_create_factory2(uint64_t *a, const uint64_t *f, void *t)
{
    return create_factory(w64_ptr(a[1]), a[2]);
}

static uint64_t h_debug_interface(uint64_t *a, const uint64_t *f, void *t)
{
    put64(a[2], 0);
    return E_NOINTERFACE;
}

static uint64_t h_s_ok(uint64_t *a, const uint64_t *f, void *t)
{
    return S_OK;
}

static const struct w64_export dxgi64_exports[] = {
    W64_FN("CreateDXGIFactory", h_create_factory),
    W64_FN("CreateDXGIFactory1", h_create_factory),
    W64_FN("CreateDXGIFactory2", h_create_factory2),
    W64_FN("DXGIGetDebugInterface1", h_debug_interface),
    W64_FN("DXGIDeclareAdapterRemovalSupport", h_s_ok),
};

W64_EXPORTS(dxgi64_module, "dxgi.dll", dxgi64_exports);

#endif
