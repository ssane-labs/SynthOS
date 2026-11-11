/* gdiplus.dll: images (the kernel's PNG/JPEG/GIF/BMP decoders), bitmaps a program locks and fills, and graphics
   that fill, outline and draw them onto a DC or another bitmap. Coordinates are integers here; the REAL variants in
   gdiplus_fp.c round and come here. Transforms are translation and scale (16.16 fixed point). */
#include <synth/config.h>
#if CONFIG_GUI
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "gdiplus.h"

#define GP_MAGIC 0x47506c73u
#define ONE 65536

enum { Ok, GenericError, InvalidParameter, OutOfMemory, ObjectBusy, InsufficientBuffer, NotImplemented, Win32Error,
       WrongState, Aborted, FileNotFound, ValueOverflow, AccessDenied, UnknownImageFormat, FontFamilyNotFound,
       FontStyleNotFound, NotTrueTypeFont, UnsupportedGdiplusVersion, GdiplusNotInitialized, PropertyNotFound };

enum { FMT_24RGB = 0x21808, FMT_32RGB = 0x22009, FMT_32ARGB = 0x26200a, FMT_32PARGB = 0xe200b };
enum { RAW_MEMORY, RAW_BMP, RAW_JPEG = 4, RAW_PNG = 5, RAW_GIF = 6, RAW_ICON = 11 };

struct gp {
    uint32_t magic;
    int kind;
};

struct gp_image {
    struct gp base;
    int w, h;
    uint32_t *px, *all;
    int stride, frames, frame;
    int *delays;
    uint32_t format;
    int raw;
    bool borrowed;
    struct gp_lock {
        int x, y, w, h;
        uint32_t mode, format;
        uint8_t *buf;
        int stride;
    } lock;
};

struct gp_state {
    int tx, ty, sx, sy;
    struct rect clip;
    bool clipped;
};

struct gp_graphics {
    struct gp base;
    HANDLE dc, window, temp_dc, temp_bmp;
    struct gp_image *img;
    struct gp_state s, saved[8];
    int depth;
    int modes[8];
};

struct gp_brush {
    struct gp base;
    uint32_t color, color2;
    bool gradient;
    int x1, y1, x2, y2;
};

struct gp_pen {
    struct gp base;
    uint32_t color;
    int width, dash;
};

struct gp_font {
    struct gp base;
    char face[32];
    int px, style;
};

struct gp_format {
    struct gp base;
    int align, line_align, flags, nranges;
    int ranges[8][2];
};

struct gp_region {
    struct gp base;
    struct rect r;
    bool infinite;
};

/* figures of points; a figure starts where starts[] says */
struct gp_path {
    struct gp base;
    int *pts, n, cap, *starts, figures, open;
};

struct gp_matrix {
    struct gp base;
    int sx, sy, tx, ty;
};

static void *gp_new(int kind, size_t size)
{
    struct gp *o = w32_zalloc(size);
    if (o) {
        o->magic = GP_MAGIC;
        o->kind = kind;
    }
    return o;
}

static void *gp_of(const void *p, int kind)
{
    const struct gp *o = p;
    return (uintptr_t)p >= 0x10000 && o->magic == GP_MAGIC && o->kind == kind ? (void *)o : NULL;
}

static void gp_free(void *p)
{
    struct gp *o = p;
    o->magic = 0;
    w32_free(o);
}

#define IMG(p) ((struct gp_image *)gp_of(p, GP_IMAGE))
#define GFX(p) ((struct gp_graphics *)gp_of(p, GP_GRAPHICS))
#define BRUSH(p) ((struct gp_brush *)gp_of(p, GP_BRUSH))
#define PEN(p) ((struct gp_pen *)gp_of(p, GP_PEN))
#define FONT(p) ((struct gp_font *)gp_of(p, GP_FONT))
#define FAMILY(p) ((struct gp_font *)gp_of(p, GP_FAMILY))
#define FORMAT(p) ((struct gp_format *)gp_of(p, GP_FORMAT))
#define MATRIX(p) ((struct gp_matrix *)gp_of(p, GP_MATRIX))

/* startup */

static int WINAPI GdipNotificationHook(uintptr_t *token)
{
    if (token)
        *token = 1;
    return Ok;
}

static void WINAPI GdipNotificationUnhook(uintptr_t token)
{
    UNUSED(token);
}

static int WINAPI GdiplusStartup(uintptr_t *token, const uint32_t *input, uint32_t *output)
{
    if (!token || !input)
        return InvalidParameter;
    if (input[0] < 1 || input[0] > 3)
        return UnsupportedGdiplusVersion;
    *token = 1;
    if (output) {
        output[0] = (uint32_t)(uintptr_t)w32_guest_addr((const void *)GdipNotificationHook);
        output[1] = (uint32_t)(uintptr_t)w32_guest_addr((const void *)GdipNotificationUnhook);
    }
    return Ok;
}

static void WINAPI GdiplusShutdown(uintptr_t token)
{
    UNUSED(token);
}

static void *WINAPI GdipAlloc(SIZE_T size)
{
    return w32_alloc(size);
}

static void WINAPI GdipFree(void *p)
{
    w32_free(p);
}

/* images */

static struct gp_image *image_new(int w, int h, int frames)
{
    if (w <= 0 || h <= 0 || frames <= 0 || (uint64_t)w * (uint64_t)h * (uint64_t)frames > PICTURE_MAX_PIXELS)
        return NULL;
    struct gp_image *im = gp_new(GP_IMAGE, sizeof(*im));
    if (!im)
        return NULL;
    im->all = w32_zalloc((size_t)w * (size_t)h * (size_t)frames * 4);
    if (!im->all) {
        gp_free(im);
        return NULL;
    }
    im->w = w;
    im->h = h;
    im->stride = w;
    im->px = im->all;
    im->frames = frames;
    im->format = FMT_32ARGB;
    return im;
}

static int raw_kind(const uint8_t *d, size_t n)
{
    if (n >= 8 && !memcmp(d, "\x89PNG", 4))
        return RAW_PNG;
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8)
        return RAW_JPEG;
    if (n >= 6 && !memcmp(d, "GIF8", 4))
        return RAW_GIF;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
        return RAW_BMP;
    return -1;
}

static int image_decode(const uint8_t *data, size_t size, struct gp_image **out)
{
    struct picture pic;
    int raw = raw_kind(data, size);
    if (raw < 0 || picture_decode(data, size, &pic) != 0)
        return raw < 0 ? UnknownImageFormat : OutOfMemory;
    int frames = MAX(pic.frames, 1);
    struct gp_image *im = image_new(pic.w, pic.h, frames);
    if (!im) {
        picture_free(&pic);
        return OutOfMemory;
    }
    memcpy(im->all, pic.px, (size_t)pic.w * (size_t)pic.h * (size_t)frames * 4);
    if (frames > 1 && pic.delays && (im->delays = w32_zalloc(sizeof(int) * (size_t)frames)))
        memcpy(im->delays, pic.delays, sizeof(int) * (size_t)frames);
    im->raw = raw;
    im->format = raw == RAW_JPEG || raw == RAW_BMP ? FMT_24RGB : FMT_32ARGB;
    picture_free(&pic);
    *out = im;
    return Ok;
}

static int load_file(const WCHAR *name, void **out)
{
    if (!name || !out)
        return InvalidParameter;
    char *win = w32_narrow(name), path[PATH_MAX_LEN];
    char *data = NULL;
    size_t size = 0;
    int r = win && w32_path(win, path, sizeof(path)) == 0 && vfs_read_all(path, &data, &size) == 0 ? Ok : FileNotFound;
    w32_free(win);
    if (r == Ok) {
        r = image_decode((const uint8_t *)data, size, (struct gp_image **)out);
        kfree(data);
    }
    return r;
}

static int WINAPI GdipLoadImageFromFile(const WCHAR *name, void **out)
{
    return load_file(name, out);
}

static int load_stream(void *stream, void **out)
{
    if (!stream || !out)
        return InvalidParameter;
    size_t cap = 65536, used = 0;
    uint8_t *data = kmalloc(cap), *chunk = w32_alloc(65536);
    uint32_t *got = w32_alloc(4);
    int r = data && chunk && got ? Ok : OutOfMemory;
    while (r == Ok) {
        *got = 0;
        uint32_t hr = w32_stream_call(stream, 3, chunk, 65536, got);
        if (hr > 1 || !*got)
            break;
        if (used + *got > cap) {
            uint8_t *bigger = used + *got <= (64u << 20) ? krealloc(data, cap * 2) : NULL;
            if (!bigger) {
                r = OutOfMemory;
                break;
            }
            data = bigger;
            cap *= 2;
        }
        memcpy(data + used, chunk, *got);
        used += *got;
        if (*got < 65536)
            break;
    }
    if (r == Ok)
        r = image_decode(data, used, (struct gp_image **)out);
    kfree(data);
    w32_free(chunk);
    w32_free(got);
    return r;
}

static int WINAPI GdipLoadImageFromStream(void *stream, void **out)
{
    return load_stream(stream, out);
}

static uint32_t unpremultiply(uint32_t c)
{
    uint32_t a = c >> 24;
    if (a == 255 || !a)
        return a ? c : 0;
    uint32_t r = MIN(((c >> 16) & 0xff) * 255 / a, 255u), g = MIN(((c >> 8) & 0xff) * 255 / a, 255u),
             b = MIN((c & 0xff) * 255 / a, 255u);
    return a << 24 | r << 16 | g << 8 | b;
}

static uint32_t premultiply(uint32_t c)
{
    uint32_t a = c >> 24;
    if (a == 255)
        return c;
    return a << 24 | (((c >> 16) & 0xff) * a / 255) << 16 | (((c >> 8) & 0xff) * a / 255) << 8 | (c & 0xff) * a / 255;
}

/* one row of pixels in a program's format, to and from straight ARGB */
static bool row_in(uint32_t format, const uint8_t *src, uint32_t *out, int n)
{
    for (int x = 0; x < n; x++) {
        switch (format) {
        case FMT_32ARGB: out[x] = ((const uint32_t *)src)[x]; break;
        case FMT_32RGB: out[x] = ((const uint32_t *)src)[x] | 0xff000000u; break;
        case FMT_32PARGB: out[x] = unpremultiply(((const uint32_t *)src)[x]); break;
        case FMT_24RGB:
            out[x] = 0xff000000u | (uint32_t)src[x * 3 + 2] << 16 | (uint32_t)src[x * 3 + 1] << 8 | src[x * 3];
            break;
        default: return false;
        }
    }
    return true;
}

static bool row_out(uint32_t format, const uint32_t *in, uint8_t *dst, int n)
{
    for (int x = 0; x < n; x++) {
        switch (format) {
        case FMT_32ARGB: ((uint32_t *)dst)[x] = in[x]; break;
        case FMT_32RGB: ((uint32_t *)dst)[x] = in[x] | 0xff000000u; break;
        case FMT_32PARGB: ((uint32_t *)dst)[x] = premultiply(in[x]); break;
        case FMT_24RGB:
            dst[x * 3] = (uint8_t)in[x];
            dst[x * 3 + 1] = (uint8_t)(in[x] >> 8);
            dst[x * 3 + 2] = (uint8_t)(in[x] >> 16);
            break;
        default: return false;
        }
    }
    return true;
}

static int format_bytes(uint32_t format)
{
    return format == FMT_24RGB ? 3 : format == FMT_32ARGB || format == FMT_32RGB || format == FMT_32PARGB ? 4 : 0;
}

static int WINAPI GdipCreateBitmapFromScan0(int w, int h, int stride, uint32_t format, uint8_t *scan0, void **out)
{
    if (!out || !format_bytes(format))
        return InvalidParameter;
    /* a 32-bit ARGB buffer the program keeps drawing into stays the bitmap's own memory */
    if (scan0 && (format == FMT_32ARGB || format == FMT_32RGB) && stride > 0 && stride % 4 == 0 && w > 0 && h > 0) {
        struct gp_image *im = gp_new(GP_IMAGE, sizeof(*im));
        if (!im)
            return OutOfMemory;
        im->w = w;
        im->h = h;
        im->px = im->all = (uint32_t *)scan0;
        im->stride = stride / 4;
        im->frames = 1;
        im->format = format;
        im->borrowed = true;
        *out = im;
        return Ok;
    }
    struct gp_image *im = image_new(w, h, 1);
    if (!im)
        return w <= 0 || h <= 0 ? InvalidParameter : OutOfMemory;
    im->format = format;
    if (scan0)
        for (int y = 0; y < h; y++) {
            const uint8_t *row = stride < 0 ? scan0 - (ptrdiff_t)y * -stride : scan0 + (size_t)y * (size_t)stride;
            row_in(format, row, im->px + (size_t)y * (size_t)w, w);
        }
    else if (format != FMT_32ARGB && format != FMT_32PARGB)
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            im->px[i] = 0xff000000u;
    *out = im;
    return Ok;
}

static int WINAPI GdipCreateBitmapFromHBITMAP(HANDLE bitmap, HANDLE palette, void **out)
{
    UNUSED(palette);
    struct canvas *c = w32_bitmap_canvas(bitmap);
    if (!c || !out)
        return InvalidParameter;
    struct gp_image *im = image_new(c->w, c->h, 1);
    if (!im)
        return OutOfMemory;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            im->px[y * c->w + x] = c->px[y * c->stride + x] | 0xff000000u;
    im->format = FMT_32RGB;
    *out = im;
    return Ok;
}

static uint32_t over(uint32_t s, uint32_t d)
{
    uint32_t a = s >> 24;
    if (a == 255)
        return s;
    if (!a)
        return d;
    uint32_t da = d >> 24;
    if (da == 255) {
        uint32_t na = 255 - a;
        return 0xff000000u | ((((s >> 16) & 0xff) * a + ((d >> 16) & 0xff) * na) / 255) << 16 |
               ((((s >> 8) & 0xff) * a + ((d >> 8) & 0xff) * na) / 255) << 8 | ((s & 0xff) * a + (d & 0xff) * na) / 255;
    }
    uint32_t oa = a + da * (255 - a) / 255, dw = da * (255 - a) / 255;
    if (!oa)
        return 0;
    return oa << 24 | ((((s >> 16) & 0xff) * a + ((d >> 16) & 0xff) * dw) / oa) << 16 |
           ((((s >> 8) & 0xff) * a + ((d >> 8) & 0xff) * dw) / oa) << 8 | ((s & 0xff) * a + (d & 0xff) * dw) / oa;
}

static int WINAPI GdipCreateHBITMAPFromBitmap(void *image, HANDLE *out, uint32_t background)
{
    struct gp_image *im = IMG(image);
    if (!im || !out)
        return InvalidParameter;
    uint32_t *row = kmalloc((size_t)im->w * 4);
    HANDLE b = row ? w32_bitmap_argb(im->w, im->h, NULL, 0) : NULL;
    struct canvas *c = b ? w32_bitmap_canvas(b) : NULL;
    if (!c) {
        kfree(row);
        return OutOfMemory;
    }
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            uint32_t p = im->px[y * im->stride + x];
            c->px[y * c->stride + x] = background >> 24 ? over(p, background) : p;
        }
    kfree(row);
    *out = b;
    return Ok;
}

static struct gp_image *image_copy(const struct gp_image *src, int x0, int y0, int w, int h, bool all_frames)
{
    int frames = all_frames ? src->frames : 1;
    struct gp_image *im = image_new(w, h, frames);
    if (!im)
        return NULL;
    for (int f = 0; f < frames; f++) {
        const uint32_t *from = all_frames ? src->all + (size_t)f * (size_t)src->w * (size_t)src->h : src->px;
        for (int y = 0; y < h; y++)
            memcpy(im->all + ((size_t)f * (size_t)h + (size_t)y) * (size_t)w,
                   from + (size_t)(y0 + y) * (size_t)src->stride + (size_t)x0, (size_t)w * 4);
    }
    if (all_frames && src->delays && (im->delays = w32_zalloc(sizeof(int) * (size_t)frames)))
        memcpy(im->delays, src->delays, sizeof(int) * (size_t)frames);
    im->px = im->all + (all_frames ? (size_t)src->frame * (size_t)w * (size_t)h : 0);
    im->frame = all_frames ? src->frame : 0;
    im->format = src->format;
    im->raw = src->raw;
    return im;
}

static int WINAPI GdipCloneImage(void *image, void **out)
{
    struct gp_image *im = IMG(image);
    if (!im || !out)
        return InvalidParameter;
    *out = image_copy(im, 0, 0, im->w, im->h, !im->borrowed);
    return *out ? Ok : OutOfMemory;
}

static int WINAPI GdipCloneBitmapAreaI(int x, int y, int w, int h, uint32_t format, void *image, void **out)
{
    struct gp_image *im = IMG(image);
    if (!im || !out || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > im->w || y + h > im->h)
        return InvalidParameter;
    struct gp_image *c = image_copy(im, x, y, w, h, false);
    if (!c)
        return OutOfMemory;
    if (format_bytes(format))
        c->format = format;
    *out = c;
    return Ok;
}

int w32_gp_clone_area(int x, int y, int w, int h, uint32_t format, void *image, void **out)
{
    return GdipCloneBitmapAreaI(x, y, w, h, format, image, out);
}

static int WINAPI GdipDisposeImage(void *image)
{
    struct gp_image *im = IMG(image);
    if (!im)
        return InvalidParameter;
    if (!im->borrowed)
        w32_free(im->all);
    w32_free(im->delays);
    w32_free(im->lock.buf);
    gp_free(im);
    return Ok;
}

static int WINAPI GdipGetImageWidth(void *image, UINT *w)
{
    struct gp_image *im = IMG(image);
    if (!im || !w)
        return InvalidParameter;
    *w = (UINT)im->w;
    return Ok;
}

static int WINAPI GdipGetImageHeight(void *image, UINT *h)
{
    struct gp_image *im = IMG(image);
    if (!im || !h)
        return InvalidParameter;
    *h = (UINT)im->h;
    return Ok;
}

bool w32_gp_image_size(void *image, int *w, int *h)
{
    struct gp_image *im = IMG(image);
    if (!im)
        return false;
    *w = im->w;
    *h = im->h;
    return true;
}

static int WINAPI GdipGetImagePixelFormat(void *image, uint32_t *format)
{
    struct gp_image *im = IMG(image);
    if (!im || !format)
        return InvalidParameter;
    *format = im->format;
    return Ok;
}

static int WINAPI GdipGetImageType(void *image, int *type)
{
    if (!IMG(image) || !type)
        return InvalidParameter;
    *type = 1;
    return Ok;
}

static int WINAPI GdipGetImageFlags(void *image, UINT *flags)
{
    struct gp_image *im = IMG(image);
    if (!im || !flags)
        return InvalidParameter;
    *flags = (im->format & 0x40000 ? 2 : 0) | 0x2000 | 0x10000;
    return Ok;
}

static const GUID raw_guid = {0xb96b3caa, 0x0728, 0x11d3, {0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

static int WINAPI GdipGetImageRawFormat(void *image, GUID *out)
{
    struct gp_image *im = IMG(image);
    if (!im || !out)
        return InvalidParameter;
    *out = raw_guid;
    out->Data1 += (uint32_t)im->raw;
    return Ok;
}

/* frames: GIF animations have FrameDimensionTime, everything else one page */
static const GUID dim_time = {0x6aedbd6d, 0x3fb5, 0x418a, {0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72}};
static const GUID dim_page = {0x7462dc86, 0x6180, 0x4c7e, {0x8e, 0x3f, 0xee, 0x73, 0x33, 0xa7, 0xa4, 0x83}};

static int WINAPI GdipImageGetFrameDimensionsCount(void *image, UINT *n)
{
    if (!IMG(image) || !n)
        return InvalidParameter;
    *n = 1;
    return Ok;
}

static int WINAPI GdipImageGetFrameDimensionsList(void *image, GUID *list, UINT n)
{
    struct gp_image *im = IMG(image);
    if (!im || !list || n < 1)
        return InvalidParameter;
    list[0] = im->raw == RAW_GIF ? dim_time : dim_page;
    return Ok;
}

static int WINAPI GdipImageGetFrameCount(void *image, const GUID *dim, UINT *n)
{
    UNUSED(dim);
    struct gp_image *im = IMG(image);
    if (!im || !n)
        return InvalidParameter;
    *n = (UINT)im->frames;
    return Ok;
}

static int WINAPI GdipImageSelectActiveFrame(void *image, const GUID *dim, UINT index)
{
    UNUSED(dim);
    struct gp_image *im = IMG(image);
    if (!im || (int)index >= im->frames)
        return InvalidParameter;
    im->frame = (int)index;
    im->px = im->all + (size_t)index * (size_t)im->w * (size_t)im->h;
    return Ok;
}

/* property items: what GIF players ask for (0x5100 frame delays, 0x5101 loop count) */
static int WINAPI GdipGetPropertyItemSize(void *image, uint32_t id, UINT *size)
{
    struct gp_image *im = IMG(image);
    if (!im || !size)
        return InvalidParameter;
    if (id == 0x5100 && im->delays)
        *size = 16 + (UINT)im->frames * 4;
    else if (id == 0x5101 && im->frames > 1)
        *size = 16 + 2;
    else
        return PropertyNotFound;
    return Ok;
}

static int WINAPI GdipGetPropertyItem(void *image, uint32_t id, UINT size, uint8_t *item)
{
    UINT need;
    int r = GdipGetPropertyItemSize(image, id, &need);
    if (r != Ok)
        return r;
    if (!item || size < need)
        return InvalidParameter;
    struct gp_image *im = image;
    uint32_t head[4] = {id, need - 16, id == 0x5100 ? 4u : 3u, (uint32_t)(uintptr_t)(item + 16)};
    memcpy(item, head, 16);
    if (id == 0x5100)
        for (int i = 0; i < im->frames; i++) {
            uint32_t cs = (uint32_t)MAX(im->delays[i], 0) / 10;
            memcpy(item + 16 + i * 4, &cs, 4);
        }
    else
        memset(item + 16, 0, 2);
    return Ok;
}

static int WINAPI GdipGetPropertyCount(void *image, UINT *n)
{
    struct gp_image *im = IMG(image);
    if (!im || !n)
        return InvalidParameter;
    *n = im->delays ? 2 : 0;
    return Ok;
}

static int WINAPI GdipGetPropertyIdList(void *image, UINT n, uint32_t *list)
{
    struct gp_image *im = IMG(image);
    if (!im || !list || n < (im->delays ? 2u : 0u))
        return InvalidParameter;
    if (im->delays) {
        list[0] = 0x5100;
        list[1] = 0x5101;
    }
    return Ok;
}

static int WINAPI GdipBitmapLockBits(void *image, const int *rect, UINT mode, uint32_t format, uint32_t *data)
{
    struct gp_image *im = IMG(image);
    int bytes = format_bytes(format);
    if (!im || !data || !bytes)
        return InvalidParameter;
    if (im->lock.mode)
        return WrongState;
    int x = rect ? rect[0] : 0, y = rect ? rect[1] : 0, w = rect ? rect[2] : im->w, h = rect ? rect[3] : im->h;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > im->w || y + h > im->h)
        return InvalidParameter;
    int stride = (w * bytes + 3) & ~3;
    uint8_t *buf;
    if (mode & 4) {
        buf = (uint8_t *)(uintptr_t)data[4];
        stride = (int)data[2];
    } else {
        buf = w32_alloc((size_t)stride * (size_t)h);
        if (!buf)
            return OutOfMemory;
        im->lock.buf = buf;
    }
    if (mode & 1)
        for (int r = 0; r < h; r++)
            row_out(format, im->px + (size_t)(y + r) * (size_t)im->stride + (size_t)x, buf + (ptrdiff_t)r * stride, w);
    im->lock = (struct gp_lock){x, y, w, h, mode | 0x100, format, im->lock.buf, stride};
    data[0] = (uint32_t)w;
    data[1] = (uint32_t)h;
    data[2] = (uint32_t)stride;
    data[3] = format;
    data[4] = (uint32_t)(uintptr_t)buf;
    data[5] = 0;
    return Ok;
}

static int WINAPI GdipBitmapUnlockBits(void *image, uint32_t *data)
{
    struct gp_image *im = IMG(image);
    if (!im || !data || !im->lock.mode)
        return InvalidParameter;
    struct gp_lock *l = &im->lock;
    if (l->mode & 2) {
        const uint8_t *buf = (const uint8_t *)(uintptr_t)data[4];
        for (int r = 0; r < l->h; r++)
            row_in(l->format, buf + (ptrdiff_t)r * l->stride, im->px + (size_t)(l->y + r) * (size_t)im->stride +
                   (size_t)l->x, l->w);
        if (l->format == FMT_24RGB || l->format == FMT_32RGB)
            im->format = im->format == FMT_32ARGB ? im->format : l->format;
    }
    w32_free(l->buf);
    *l = (struct gp_lock){0};
    return Ok;
}

static int WINAPI GdipBitmapGetPixel(void *image, int x, int y, uint32_t *color)
{
    struct gp_image *im = IMG(image);
    if (!im || !color || x < 0 || y < 0 || x >= im->w || y >= im->h)
        return InvalidParameter;
    *color = im->px[y * im->stride + x];
    return Ok;
}

static int WINAPI GdipBitmapSetPixel(void *image, int x, int y, uint32_t color)
{
    struct gp_image *im = IMG(image);
    if (!im || x < 0 || y < 0 || x >= im->w || y >= im->h)
        return InvalidParameter;
    im->px[y * im->stride + x] = color;
    return Ok;
}

/* RotateNoneFlipNone..Rotate270FlipX: bit 0 and 1 the quarter turns, bit 2 a horizontal flip after them */
static int WINAPI GdipImageRotateFlip(void *image, int type)
{
    struct gp_image *im = IMG(image);
    if (!im || type < 0 || type > 7 || im->borrowed)
        return InvalidParameter;
    int turns = type & 3, w = turns & 1 ? im->h : im->w, h = turns & 1 ? im->w : im->h;
    bool flip = type & 4;
    size_t frame = (size_t)im->w * (size_t)im->h;
    uint32_t *all = w32_alloc(frame * (size_t)im->frames * 4);
    if (!all)
        return OutOfMemory;
    for (int f = 0; f < im->frames; f++) {
        const uint32_t *src = im->all + frame * (size_t)f;
        uint32_t *dst = all + frame * (size_t)f;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int ux = flip ? w - 1 - x : x, sx, sy;
                switch (turns) {
                case 1: sx = y, sy = im->h - 1 - ux; break;
                case 2: sx = im->w - 1 - ux, sy = im->h - 1 - y; break;
                case 3: sx = im->w - 1 - y, sy = ux; break;
                default: sx = ux, sy = y; break;
                }
                dst[y * w + x] = src[sy * im->w + sx];
            }
    }
    w32_free(im->all);
    im->all = all;
    im->w = w;
    im->h = h;
    im->stride = w;
    im->px = all + frame * (size_t)im->frame;
    return Ok;
}

static void scale_into(const struct gp_image *src, struct gp_image *dst)
{
    for (int y = 0; y < dst->h; y++) {
        const uint32_t *row = src->px + (size_t)(y * src->h / dst->h) * (size_t)src->stride;
        for (int x = 0; x < dst->w; x++)
            dst->px[y * dst->w + x] = row[x * src->w / dst->w];
    }
}

static int WINAPI GdipGetImageThumbnail(void *image, UINT w, UINT h, void **out, void *abort, void *data)
{
    UNUSED(abort);
    UNUSED(data);
    struct gp_image *im = IMG(image);
    if (!im || !out)
        return InvalidParameter;
    struct gp_image *t = image_new(w ? (int)w : 120, h ? (int)h : 120, 1);
    if (!t)
        return OutOfMemory;
    scale_into(im, t);
    *out = t;
    return Ok;
}

/* encoders and decoders: the ImageCodecInfo records (strings follow the array) */
static const char *const codec_names[][4] = {
    {"Built-in BMP Codec", "BMP", "*.BMP;*.DIB;*.RLE", "image/bmp"},
    {"Built-in JPEG Codec", "JPEG", "*.JPG;*.JPEG;*.JPE;*.JFIF", "image/jpeg"},
    {"Built-in GIF Codec", "GIF", "*.GIF", "image/gif"},
    {"Built-in PNG Codec", "PNG", "*.PNG", "image/png"},
};
static const uint8_t codec_clsid[] = {0x00, 0x01, 0x02, 0x06};
static const uint8_t codec_format[] = {RAW_BMP, RAW_JPEG, RAW_GIF, RAW_PNG};

static UINT codec_size(bool encoders, UINT *count)
{
    UINT n = 0, size = 0;
    for (int i = 0; i < 4; i++) {
        if (encoders && (i == 1 || i == 2))
            continue;
        n++;
        size += 76;
        for (int k = 0; k < 4; k++)
            size += (UINT)(strlen(codec_names[i][k]) + 1) * 2;
        size += (UINT)(strlen("") + 1) * 2;
    }
    *count = n;
    return size;
}

static int codec_list(bool encoders, UINT count, UINT size, uint8_t *out)
{
    UINT n;
    if (!out || size < codec_size(encoders, &n) || count < n)
        return InvalidParameter;
    uint8_t *strings = out + n * 76;
    UINT at = 0;
    for (int i = 0; i < 4; i++) {
        if (encoders && (i == 1 || i == 2))
            continue;
        uint8_t *e = out + at++ * 76;
        memset(e, 0, 76);
        GUID clsid = {0x557cf400u + codec_clsid[i], 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        GUID fmt = raw_guid;
        fmt.Data1 += codec_format[i];
        memcpy(e, &clsid, 16);
        memcpy(e + 16, &fmt, 16);
        static const int slots[5] = {32, 40, 44, 48, 36};
        const char *texts[5] = {codec_names[i][0], codec_names[i][1], codec_names[i][2], codec_names[i][3], ""};
        for (int k = 0; k < 5; k++) {
            uint32_t ptr = (uint32_t)(uintptr_t)strings;
            memcpy(e + slots[k], &ptr, 4);
            size_t len = strlen(texts[k]);
            for (size_t c = 0; c <= len; c++)
                ((WCHAR *)strings)[c] = (WCHAR)(uint8_t)texts[k][c];
            strings += (len + 1) * 2;
        }
        uint32_t flags = encoders ? 0x10001u : 0x10002u, version = 1;
        memcpy(e + 52, &flags, 4);
        memcpy(e + 56, &version, 4);
    }
    return Ok;
}

static int WINAPI GdipGetImageEncodersSize(UINT *count, UINT *size)
{
    if (!count || !size)
        return InvalidParameter;
    *size = codec_size(true, count);
    return Ok;
}

static int WINAPI GdipGetImageEncoders(UINT count, UINT size, uint8_t *out)
{
    return codec_list(true, count, size, out);
}

static int WINAPI GdipGetImageDecodersSize(UINT *count, UINT *size)
{
    if (!count || !size)
        return InvalidParameter;
    *size = codec_size(false, count);
    return Ok;
}

static int WINAPI GdipGetImageDecoders(UINT count, UINT size, uint8_t *out)
{
    return codec_list(false, count, size, out);
}

/* PNG or BMP bytes of the current frame */
static int encode(const struct gp_image *im, const GUID *clsid, uint8_t **out, size_t *len)
{
    if (!clsid || clsid->Data2 != 0x1a04 || (clsid->Data1 != 0x557cf400u && clsid->Data1 != 0x557cf406u))
        return clsid ? NotImplemented : InvalidParameter;
    if (clsid->Data1 == 0x557cf406u)
        return png_encode(im->px, im->w, im->h, im->stride, out, len) == 0 ? Ok : OutOfMemory;
    size_t bytes = (size_t)im->w * (size_t)im->h * 4;
    uint8_t *b = kmalloc(54 + bytes);
    if (!b)
        return OutOfMemory;
    memset(b, 0, 54);
    uint32_t v[] = {(uint32_t)(54 + bytes), 0, 54, 40, (uint32_t)im->w, (uint32_t)-im->h};
    b[0] = 'B';
    b[1] = 'M';
    memcpy(b + 2, v, 24);
    b[26] = 1;
    b[28] = 32;
    for (int y = 0; y < im->h; y++)
        memcpy(b + 54 + (size_t)y * (size_t)im->w * 4, im->px + (size_t)y * (size_t)im->stride, (size_t)im->w * 4);
    *out = b;
    *len = 54 + bytes;
    return Ok;
}

static int WINAPI GdipSaveImageToFile(void *image, const WCHAR *name, const GUID *clsid, const void *params)
{
    UNUSED(params);
    struct gp_image *im = IMG(image);
    if (!im || !name)
        return InvalidParameter;
    uint8_t *data;
    size_t len;
    int r = encode(im, clsid, &data, &len);
    if (r != Ok)
        return r;
    char *win = w32_narrow(name), path[PATH_MAX_LEN];
    if (!win || w32_path(win, path, sizeof(path)) != 0 || vfs_write_all(path, data, len) != 0)
        r = Win32Error;
    w32_free(win);
    kfree(data);
    return r;
}

static int WINAPI GdipSaveImageToStream(void *image, void *stream, const GUID *clsid, const void *params)
{
    UNUSED(params);
    struct gp_image *im = IMG(image);
    if (!im || !stream)
        return InvalidParameter;
    uint8_t *data, *copy;
    size_t len;
    int r = encode(im, clsid, &data, &len);
    if (r != Ok)
        return r;
    copy = w32_alloc(len);
    if (copy) {
        memcpy(copy, data, len);
        r = w32_stream_call(stream, 4, copy, (uint32_t)len, NULL) == 0 ? Ok : Win32Error;
        w32_free(copy);
    } else {
        r = OutOfMemory;
    }
    kfree(data);
    return r;
}

/* graphics: a DC (a window's or a memory one) or a bitmap */

struct surf {
    struct canvas *c, own;
    HANDLE dc;
    int ox, oy;
    struct rect clip, drawn;
    const struct gp_state *s;
};

static bool surf_of(struct gp_graphics *g, struct surf *s)
{
    s->dc = g->dc;
    s->s = &g->s;
    s->drawn = (struct rect){0, 0, 0, 0};
    if (g->img) {
        struct gp_image *im = g->img;
        s->own = (struct canvas){im->px, im->w, im->h, im->stride, {0, 0, im->w, im->h}};
        s->c = &s->own;
        s->ox = s->oy = 0;
        s->clip = s->own.clip;
    } else {
        s->c = w32_dc_target(g->dc, &s->ox, &s->oy, &s->clip);
        if (!s->c)
            return false;
    }
    if (g->s.clipped)
        s->clip = rect_intersect(s->clip, (struct rect){s->ox + g->s.clip.x, s->oy + g->s.clip.y, g->s.clip.w,
                                                         g->s.clip.h});
    return true;
}

static void surf_done(struct surf *s)
{
    if (s->dc && !rect_empty(s->drawn))
        w32_dc_damage(s->dc, s->drawn);
}

/* world coordinates to the canvas */
static int dev_x(const struct surf *s, int x)
{
    return s->ox + s->s->tx + (int)((int64_t)x * s->s->sx / ONE);
}

static int dev_y(const struct surf *s, int y)
{
    return s->oy + s->s->ty + (int)((int64_t)y * s->s->sy / ONE);
}

static struct rect dev_rect(const struct surf *s, int x, int y, int w, int h)
{
    int x0 = dev_x(s, x), y0 = dev_y(s, y), x1 = dev_x(s, x + w), y1 = dev_y(s, y + h);
    return (struct rect){MIN(x0, x1), MIN(y0, y1), ABS(x1 - x0), ABS(y1 - y0)};
}

static void mark(struct surf *s, struct rect r)
{
    r = rect_intersect(r, s->clip);
    if (rect_empty(r))
        return;
    if (rect_empty(s->drawn)) {
        s->drawn = r;
        return;
    }
    int x0 = MIN(s->drawn.x, r.x), y0 = MIN(s->drawn.y, r.y);
    int x1 = MAX(s->drawn.x + s->drawn.w, r.x + r.w), y1 = MAX(s->drawn.y + s->drawn.h, r.y + r.h);
    s->drawn = (struct rect){x0, y0, x1 - x0, y1 - y0};
}

static uint32_t brush_color(const struct gp_brush *b, const struct surf *s, int x, int y)
{
    if (!b->gradient)
        return b->color;
    int64_t dx = dev_x(s, b->x2) - dev_x(s, b->x1), dy = dev_y(s, b->y2) - dev_y(s, b->y1);
    int64_t len = dx * dx + dy * dy, t = len ? ((x - dev_x(s, b->x1)) * dx + (y - dev_y(s, b->y1)) * dy) * 256 / len : 0;
    t = t < 0 ? 0 : t > 256 ? 256 : t;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int64_t a = (b->color >> sh) & 0xff, c = (b->color2 >> sh) & 0xff;
        out |= (uint32_t)((a * (256 - t) + c * t) >> 8) << sh;
    }
    return out;
}

static void span(struct surf *s, int y, int x0, int x1, const struct gp_brush *b, uint32_t color)
{
    if (y < s->clip.y || y >= s->clip.y + s->clip.h)
        return;
    x0 = MAX(x0, s->clip.x);
    x1 = MIN(x1, s->clip.x + s->clip.w);
    uint32_t *row = s->c->px + y * s->c->stride;
    for (int x = x0; x < x1; x++)
        row[x] = over(b ? brush_color(b, s, x, y) : color, row[x]);
}

static void fill_dev(struct surf *s, struct rect r, const struct gp_brush *b, uint32_t color)
{
    for (int y = r.y; y < r.y + r.h; y++)
        span(s, y, r.x, r.x + r.w, b, color);
    mark(s, r);
}

static int fill_rects(void *graphics, void *brush, const int *rects, int n)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_brush *b = BRUSH(brush);
    struct surf s;
    if (!g || !b || !rects || n < 0)
        return InvalidParameter;
    if (!surf_of(g, &s))
        return Ok;
    for (int i = 0; i < n; i++)
        fill_dev(&s, dev_rect(&s, rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]), b, 0);
    surf_done(&s);
    return Ok;
}

int w32_gp_fill_rect(void *graphics, void *brush, int x, int y, int w, int h)
{
    return fill_rects(graphics, brush, (const int[]){x, y, w, h}, 1);
}

static int WINAPI GdipFillRectangleI(void *graphics, void *brush, int x, int y, int w, int h)
{
    return w32_gp_fill_rect(graphics, brush, x, y, w, h);
}

static int WINAPI GdipFillRectanglesI(void *graphics, void *brush, const int *rects, int n)
{
    return fill_rects(graphics, brush, rects, n);
}

/* polygons, filled with the even-odd rule along each pixel row's centre */
int w32_gp_fill_poly(void *graphics, void *brush, const int *pts, int n)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_brush *b = BRUSH(brush);
    struct surf s;
    if (!g || !b || !pts || n < 3)
        return n < 3 && g && b ? Ok : InvalidParameter;
    if (!surf_of(g, &s))
        return Ok;
    int *dx = kmalloc(sizeof(int) * (size_t)n * 3), *xs = dx ? dx + n * 2 : NULL;
    if (!dx)
        return OutOfMemory;
    int top = 1 << 30, bottom = -(1 << 30);
    for (int i = 0; i < n; i++) {
        dx[i * 2] = dev_x(&s, pts[i * 2]);
        dx[i * 2 + 1] = dev_y(&s, pts[i * 2 + 1]);
        top = MIN(top, dx[i * 2 + 1]);
        bottom = MAX(bottom, dx[i * 2 + 1]);
    }
    top = MAX(top, s.clip.y);
    bottom = MIN(bottom, s.clip.y + s.clip.h);
    for (int y = top; y < bottom; y++) {
        int count = 0;
        for (int i = 0; i < n; i++) {
            int x0 = dx[i * 2], y0 = dx[i * 2 + 1], x1 = dx[(i + 1) % n * 2], y1 = dx[(i + 1) % n * 2 + 1];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y))
                xs[count++] = x0 + (int)((int64_t)(y - y0) * (x1 - x0) / (y1 - y0));
        }
        for (int i = 1; i < count; i++)
            for (int k = i; k > 0 && xs[k - 1] > xs[k]; k--) {
                int t = xs[k];
                xs[k] = xs[k - 1];
                xs[k - 1] = t;
            }
        for (int i = 0; i + 1 < count; i += 2)
            span(&s, y, xs[i], xs[i + 1], b, 0);
    }
    int x0 = 1 << 30, x1 = -(1 << 30);
    for (int i = 0; i < n; i++) {
        x0 = MIN(x0, dx[i * 2]);
        x1 = MAX(x1, dx[i * 2]);
    }
    mark(&s, (struct rect){x0, top, x1 - x0 + 1, bottom - top});
    kfree(dx);
    surf_done(&s);
    return Ok;
}

static int WINAPI GdipFillPolygonI(void *graphics, void *brush, const int *pts, int n, int mode)
{
    UNUSED(mode);
    return w32_gp_fill_poly(graphics, brush, pts, n);
}

static int WINAPI GdipFillPolygon2I(void *graphics, void *brush, const int *pts, int n)
{
    return w32_gp_fill_poly(graphics, brush, pts, n);
}

int w32_gp_fill_ellipse(void *graphics, void *brush, int x, int y, int w, int h)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_brush *b = BRUSH(brush);
    struct surf s;
    if (!g || !b)
        return InvalidParameter;
    if (!surf_of(g, &s))
        return Ok;
    struct rect r = dev_rect(&s, x, y, w, h);
    int64_t a = r.w, c = r.h;
    for (int row = 0; row < r.h; row++) {
        int64_t yy = 2 * row + 1 - c, half = a * a - (a * a * yy * yy) / (c * c > 0 ? c * c : 1);
        int hw = 0;
        while ((int64_t)(2 * hw + 2) * (2 * hw + 2) <= half)
            hw++;
        int cx = r.x + r.w / 2;
        span(&s, r.y + row, cx - hw - (r.w & 1), cx + hw + 1, b, 0);
    }
    mark(&s, r);
    surf_done(&s);
    return Ok;
}

static int WINAPI GdipFillEllipseI(void *graphics, void *brush, int x, int y, int w, int h)
{
    return w32_gp_fill_ellipse(graphics, brush, x, y, w, h);
}

static void dot(struct surf *s, int x, int y, int width, uint32_t color)
{
    int h = width / 2;
    for (int yy = y - h; yy < y - h + width; yy++)
        span(s, yy, x - h, x - h + width, NULL, color);
}

static void line_dev(struct surf *s, int x0, int y0, int x1, int y1, const struct gp_pen *p)
{
    int dx = ABS(x1 - x0), dy = -ABS(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy, step = 0;
    int width = MAX(p->width, 1);
    for (;;) {
        if (!p->dash || (step / (width * 3)) % 2 == 0)
            dot(s, x0, y0, width, p->color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
        step++;
    }
}

int w32_gp_draw_lines(void *graphics, void *pen, const int *pts, int n, bool closed)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_pen *p = PEN(pen);
    struct surf s;
    if (!g || !p || !pts || n < 2)
        return InvalidParameter;
    if (!surf_of(g, &s))
        return Ok;
    int w = MAX(p->width, 1);
    for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        const int *a = pts + i * 2, *b = pts + (i + 1) % n * 2;
        int x0 = dev_x(&s, a[0]), y0 = dev_y(&s, a[1]), x1 = dev_x(&s, b[0]), y1 = dev_y(&s, b[1]);
        line_dev(&s, x0, y0, x1, y1, p);
        mark(&s, (struct rect){MIN(x0, x1) - w, MIN(y0, y1) - w, ABS(x1 - x0) + 2 * w + 1, ABS(y1 - y0) + 2 * w + 1});
    }
    surf_done(&s);
    return Ok;
}

static int WINAPI GdipDrawLineI(void *graphics, void *pen, int x0, int y0, int x1, int y1)
{
    return w32_gp_draw_lines(graphics, pen, (const int[]){x0, y0, x1, y1}, 2, false);
}

static int WINAPI GdipDrawLinesI(void *graphics, void *pen, const int *pts, int n)
{
    return w32_gp_draw_lines(graphics, pen, pts, n, false);
}

static int WINAPI GdipDrawPolygonI(void *graphics, void *pen, const int *pts, int n)
{
    return w32_gp_draw_lines(graphics, pen, pts, n, true);
}

int w32_gp_draw_rect(void *graphics, void *pen, int x, int y, int w, int h)
{
    return w32_gp_draw_lines(graphics, pen, (const int[]){x, y, x + w, y, x + w, y + h, x, y + h}, 4, true);
}

static int WINAPI GdipDrawRectangleI(void *graphics, void *pen, int x, int y, int w, int h)
{
    return w32_gp_draw_rect(graphics, pen, x, y, w, h);
}

static int WINAPI GdipDrawRectanglesI(void *graphics, void *pen, const int *rects, int n)
{
    for (int i = 0; i < n; i++) {
        int r = w32_gp_draw_rect(graphics, pen, rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]);
        if (r != Ok)
            return r;
    }
    return Ok;
}

/* an ellipse outline as a polygon of 32 points (integer sine table, quarter turn in 8 steps) */
int w32_gp_draw_ellipse(void *graphics, void *pen, int x, int y, int w, int h)
{
    static const int sine[9] = {0, 12785, 25080, 36410, 46341, 54491, 60547, 64277, 65536};
    int pts[64];
    for (int i = 0; i < 32; i++) {
        int q = i / 8, k = i % 8, sn = q % 2 ? sine[8 - k] : sine[k], cs = q % 2 ? sine[k] : sine[8 - k];
        int sx = q == 1 || q == 2 ? -cs : cs, sy = q >= 2 ? -sn : sn;
        pts[i * 2] = x + w / 2 + (int)((int64_t)sx * w / 2 / ONE);
        pts[i * 2 + 1] = y + h / 2 + (int)((int64_t)sy * h / 2 / ONE);
    }
    return w32_gp_draw_lines(graphics, pen, pts, 32, true);
}

static int WINAPI GdipDrawEllipseI(void *graphics, void *pen, int x, int y, int w, int h)
{
    return w32_gp_draw_ellipse(graphics, pen, x, y, w, h);
}

static int WINAPI GdipGraphicsClear(void *graphics, uint32_t color)
{
    struct gp_graphics *g = GFX(graphics);
    struct surf s;
    if (!g)
        return InvalidParameter;
    if (!surf_of(g, &s))
        return Ok;
    for (int y = s.clip.y; y < s.clip.y + s.clip.h; y++)
        for (int x = s.clip.x; x < s.clip.x + s.clip.w; x++)
            s.c->px[y * s.c->stride + x] = g->img ? color : color | 0xff000000u;
    mark(&s, s.clip);
    surf_done(&s);
    return Ok;
}

/* an image (or part of it) into a rectangle, nearest pixel */
int w32_gp_draw_image(void *graphics, void *image, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_image *im = IMG(image);
    struct surf s;
    if (!g || !im)
        return InvalidParameter;
    if (!dw || !dh || !sw || !sh || !surf_of(g, &s))
        return Ok;
    struct rect r = dev_rect(&s, dx, dy, dw, dh), vis = rect_intersect(r, s.clip);
    bool opaque = im->format == FMT_24RGB || im->format == FMT_32RGB;
    for (int y = vis.y; y < vis.y + vis.h; y++) {
        int py = sy + (int)((int64_t)(y - r.y) * sh / r.h);
        if (py < 0 || py >= im->h)
            continue;
        const uint32_t *in = im->px + (size_t)py * (size_t)im->stride;
        uint32_t *out = s.c->px + y * s.c->stride;
        for (int x = vis.x; x < vis.x + vis.w; x++) {
            int px = sx + (int)((int64_t)(x - r.x) * sw / r.w);
            if (px < 0 || px >= im->w)
                continue;
            out[x] = opaque ? in[px] | 0xff000000u : over(in[px], out[x]);
        }
    }
    mark(&s, r);
    surf_done(&s);
    return Ok;
}

static int WINAPI GdipDrawImageI(void *graphics, void *image, int x, int y)
{
    struct gp_image *im = IMG(image);
    return im ? w32_gp_draw_image(graphics, image, x, y, im->w, im->h, 0, 0, im->w, im->h) : InvalidParameter;
}

static int WINAPI GdipDrawImageRectI(void *graphics, void *image, int x, int y, int w, int h)
{
    struct gp_image *im = IMG(image);
    return im ? w32_gp_draw_image(graphics, image, x, y, w, h, 0, 0, im->w, im->h) : InvalidParameter;
}

static int WINAPI GdipDrawImageRectRectI(void *graphics, void *image, int dx, int dy, int dw, int dh, int sx, int sy,
                                         int sw, int sh, int unit, void *attrs, void *cb, void *data)
{
    UNUSED(unit);
    UNUSED(attrs);
    UNUSED(cb);
    UNUSED(data);
    return w32_gp_draw_image(graphics, image, dx, dy, dw, dh, sx, sy, sw, sh);
}

static int WINAPI GdipDrawImagePointRectI(void *graphics, void *image, int x, int y, int sx, int sy, int sw, int sh,
                                          int unit)
{
    UNUSED(unit);
    return w32_gp_draw_image(graphics, image, x, y, sw, sh, sx, sy, sw, sh);
}

static struct gp_graphics *graphics_new(void)
{
    struct gp_graphics *g = gp_new(GP_GRAPHICS, sizeof(*g));
    if (g) {
        g->s.sx = g->s.sy = ONE;
        g->modes[5] = 2;
    }
    return g;
}

static int WINAPI GdipCreateFromHDC(HANDLE dc, void **out)
{
    if (!dc || !out)
        return InvalidParameter;
    struct gp_graphics *g = graphics_new();
    if (!g)
        return OutOfMemory;
    g->dc = dc;
    *out = g;
    return Ok;
}

static int WINAPI GdipCreateFromHDC2(HANDLE dc, HANDLE device, void **out)
{
    UNUSED(device);
    return GdipCreateFromHDC(dc, out);
}

static int WINAPI GdipCreateFromHWND(HANDLE window, void **out)
{
    HANDLE(WINAPI * get_dc)(HANDLE) = (HANDLE(WINAPI *)(HANDLE))w32_builtin_lookup("user32.dll", "GetDC");
    HANDLE dc = get_dc ? get_dc(window) : NULL;
    if (!dc)
        return InvalidParameter;
    int r = GdipCreateFromHDC(dc, out);
    if (r == Ok)
        ((struct gp_graphics *)*out)->window = window;
    return r;
}

static int WINAPI GdipGetImageGraphicsContext(void *image, void **out)
{
    struct gp_image *im = IMG(image);
    if (!im || !out)
        return InvalidParameter;
    struct gp_graphics *g = graphics_new();
    if (!g)
        return OutOfMemory;
    g->img = im;
    *out = g;
    return Ok;
}

static int WINAPI GdipCreateBitmapFromGraphics(int w, int h, void *graphics, void **out)
{
    if (!GFX(graphics))
        return InvalidParameter;
    return GdipCreateBitmapFromScan0(w, h, 0, FMT_32PARGB, NULL, out);
}

static int WINAPI GdipDeleteGraphics(void *graphics)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    if (g->window) {
        int(WINAPI * release)(HANDLE, HANDLE) = (int(WINAPI *)(HANDLE, HANDLE))w32_builtin_lookup("user32.dll", "ReleaseDC");
        if (release)
            release(g->window, g->dc);
    }
    gp_free(g);
    return Ok;
}

/* GetDC on a bitmap's graphics: a memory DC with a copy, copied back on ReleaseDC */
static int WINAPI GdipGetDC(void *graphics, HANDLE *out)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g || !out)
        return InvalidParameter;
    if (!g->img) {
        *out = g->dc;
        return Ok;
    }
    HANDLE(WINAPI * create)(HANDLE) = (HANDLE(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "CreateCompatibleDC");
    HANDLE(WINAPI * select)(HANDLE, HANDLE) = (HANDLE(WINAPI *)(HANDLE, HANDLE))w32_builtin_lookup("gdi32.dll", "SelectObject");
    g->temp_bmp = w32_bitmap_argb(g->img->w, g->img->h, g->img->px, g->img->stride);
    g->temp_dc = create ? create(NULL) : NULL;
    if (!g->temp_bmp || !g->temp_dc || !select)
        return OutOfMemory;
    select(g->temp_dc, g->temp_bmp);
    *out = g->temp_dc;
    return Ok;
}

static int WINAPI GdipReleaseDC(void *graphics, HANDLE dc)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    if (g->temp_dc && dc == g->temp_dc) {
        struct canvas *c = w32_bitmap_canvas(g->temp_bmp);
        for (int y = 0; c && y < c->h && y < g->img->h; y++)
            for (int x = 0; x < c->w && x < g->img->w; x++) {
                uint32_t p = c->px[y * c->stride + x];
                g->img->px[y * g->img->stride + x] = p >> 24 ? p : p | 0xff000000u;
            }
        BOOL(WINAPI * del_dc)(HANDLE) = (BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "DeleteDC");
        BOOL(WINAPI * del)(HANDLE) = (BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "DeleteObject");
        if (del_dc)
            del_dc(g->temp_dc);
        if (del)
            del(g->temp_bmp);
        g->temp_dc = g->temp_bmp = NULL;
    }
    return Ok;
}

/* rendering modes are kept and read back; drawing is the same for all of them */
static int set_mode(void *graphics, int slot, int value)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    g->modes[slot] = value;
    return Ok;
}

static int get_mode(void *graphics, int slot, int *value)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g || !value)
        return InvalidParameter;
    *value = g->modes[slot];
    return Ok;
}

#define MODE(n, slot)                                                                                                  \
    static int WINAPI GdipSet##n(void *g, int v)                                                                       \
    {                                                                                                                  \
        return set_mode(g, slot, v);                                                                                   \
    }                                                                                                                  \
    static int WINAPI GdipGet##n(void *g, int *v)                                                                      \
    {                                                                                                                  \
        return get_mode(g, slot, v);                                                                                   \
    }
MODE(SmoothingMode, 0)
MODE(InterpolationMode, 1)
MODE(PixelOffsetMode, 2)
MODE(CompositingMode, 3)
MODE(CompositingQuality, 4)
MODE(PageUnit, 5)
MODE(TextRenderingHint, 6)
MODE(TextContrast, 7)

static int WINAPI GdipResetWorldTransform(void *graphics)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    g->s.tx = g->s.ty = 0;
    g->s.sx = g->s.sy = ONE;
    return Ok;
}

/* order 0 (prepend) applies the new step first, in world units; 1 (append) after the current transform */
int w32_gp_transform(void *graphics, int tx, int ty, int sx, int sy, int order)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    struct gp_state *s = &g->s;
    if (order == 1) {
        s->tx = (int)((int64_t)s->tx * sx / ONE) + tx;
        s->ty = (int)((int64_t)s->ty * sy / ONE) + ty;
    } else {
        s->tx += (int)((int64_t)tx * s->sx / ONE);
        s->ty += (int)((int64_t)ty * s->sy / ONE);
    }
    s->sx = (int)((int64_t)s->sx * sx / ONE);
    s->sy = (int)((int64_t)s->sy * sy / ONE);
    return Ok;
}

static int WINAPI GdipCreateMatrix(void **out)
{
    struct gp_matrix *m = out ? gp_new(GP_MATRIX, sizeof(*m)) : NULL;
    if (!m)
        return out ? OutOfMemory : InvalidParameter;
    m->sx = m->sy = ONE;
    *out = m;
    return Ok;
}

int w32_gp_matrix_new(int sx, int sy, int tx, int ty, void **out)
{
    int r = GdipCreateMatrix(out);
    if (r == Ok) {
        struct gp_matrix *m = *out;
        m->sx = sx;
        m->sy = sy;
        m->tx = tx;
        m->ty = ty;
    }
    return r;
}

static int WINAPI GdipDeleteMatrix(void *matrix)
{
    struct gp_matrix *m = MATRIX(matrix);
    if (!m)
        return InvalidParameter;
    gp_free(m);
    return Ok;
}

int w32_gp_matrix_get(void *matrix, int *v)
{
    struct gp_matrix *m = MATRIX(matrix);
    if (!m)
        return InvalidParameter;
    v[0] = m->sx;
    v[1] = m->sy;
    v[2] = m->tx;
    v[3] = m->ty;
    return Ok;
}

int w32_gp_matrix_set(void *matrix, const int *v)
{
    struct gp_matrix *m = MATRIX(matrix);
    if (!m)
        return InvalidParameter;
    m->sx = v[0];
    m->sy = v[1];
    m->tx = v[2];
    m->ty = v[3];
    return Ok;
}

static int WINAPI GdipSetWorldTransform(void *graphics, void *matrix)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_matrix *m = MATRIX(matrix);
    if (!g || !m)
        return InvalidParameter;
    g->s.sx = m->sx;
    g->s.sy = m->sy;
    g->s.tx = m->tx;
    g->s.ty = m->ty;
    return Ok;
}

static int WINAPI GdipGetWorldTransform(void *graphics, void *matrix)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_matrix *m = MATRIX(matrix);
    if (!g || !m)
        return InvalidParameter;
    m->sx = g->s.sx;
    m->sy = g->s.sy;
    m->tx = g->s.tx;
    m->ty = g->s.ty;
    return Ok;
}

static int WINAPI GdipSetClipRectI(void *graphics, int x, int y, int w, int h, int mode)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    struct rect r = {g->s.tx + (int)((int64_t)x * g->s.sx / ONE), g->s.ty + (int)((int64_t)y * g->s.sy / ONE),
                     (int)((int64_t)w * g->s.sx / ONE), (int)((int64_t)h * g->s.sy / ONE)};
    g->s.clip = mode == 1 && g->s.clipped ? rect_intersect(g->s.clip, r) : r;
    g->s.clipped = true;
    return Ok;
}

int w32_gp_clip(void *graphics, int x, int y, int w, int h, int mode)
{
    return GdipSetClipRectI(graphics, x, y, w, h, mode);
}

static int WINAPI GdipResetClip(void *graphics)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g)
        return InvalidParameter;
    g->s.clipped = false;
    return Ok;
}

static int WINAPI GdipGetClipBoundsI(void *graphics, int *rect)
{
    struct gp_graphics *g = GFX(graphics);
    struct surf s;
    if (!g || !rect)
        return InvalidParameter;
    struct rect r = g->s.clip;
    if (!g->s.clipped)
        r = surf_of(g, &s) ? (struct rect){s.clip.x - s.ox, s.clip.y - s.oy, s.clip.w, s.clip.h}
                           : (struct rect){0, 0, 0, 0};
    rect[0] = (int)((int64_t)(r.x - g->s.tx) * ONE / MAX(g->s.sx, 1));
    rect[1] = (int)((int64_t)(r.y - g->s.ty) * ONE / MAX(g->s.sy, 1));
    rect[2] = (int)((int64_t)r.w * ONE / MAX(g->s.sx, 1));
    rect[3] = (int)((int64_t)r.h * ONE / MAX(g->s.sy, 1));
    return Ok;
}

static int WINAPI GdipSaveGraphics(void *graphics, UINT *state)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g || !state)
        return InvalidParameter;
    if (g->depth < 8)
        g->saved[g->depth] = g->s;
    *state = (UINT)++g->depth;
    return Ok;
}

static int WINAPI GdipRestoreGraphics(void *graphics, UINT state)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g || !state || (int)state > g->depth)
        return InvalidParameter;
    if (state <= 8)
        g->s = g->saved[state - 1];
    g->depth = (int)state - 1;
    return Ok;
}

/* brushes and pens */

static int WINAPI GdipCreateSolidFill(uint32_t color, void **out)
{
    struct gp_brush *b = out ? gp_new(GP_BRUSH, sizeof(*b)) : NULL;
    if (!b)
        return out ? OutOfMemory : InvalidParameter;
    b->color = color;
    *out = b;
    return Ok;
}

int w32_gp_gradient(int x1, int y1, int x2, int y2, uint32_t c1, uint32_t c2, void **out)
{
    int r = GdipCreateSolidFill(c1, out);
    if (r == Ok) {
        struct gp_brush *b = *out;
        *b = (struct gp_brush){b->base, c1, c2, true, x1, y1, x2, y2};
    }
    return r;
}

static int WINAPI GdipCreateLineBrushI(const int *p1, const int *p2, uint32_t c1, uint32_t c2, int wrap, void **out)
{
    UNUSED(wrap);
    if (!p1 || !p2)
        return InvalidParameter;
    return w32_gp_gradient(p1[0], p1[1], p2[0], p2[1], c1, c2, out);
}

/* LinearGradientModeHorizontal, Vertical, ForwardDiagonal, BackwardDiagonal */
int w32_gp_gradient_rect(const int *r, uint32_t c1, uint32_t c2, int mode, void **out)
{
    if (!r)
        return InvalidParameter;
    int x2 = mode == 1 ? r[0] : r[0] + r[2], y2 = mode == 0 ? r[1] : r[1] + r[3];
    return mode == 3 ? w32_gp_gradient(r[0] + r[2], r[1], r[0], r[1] + r[3], c1, c2, out)
                     : w32_gp_gradient(r[0], r[1], x2, y2, c1, c2, out);
}

static int WINAPI GdipCreateLineBrushFromRectI(const int *r, uint32_t c1, uint32_t c2, int mode, int wrap, void **out)
{
    UNUSED(wrap);
    return w32_gp_gradient_rect(r, c1, c2, mode, out);
}

static int WINAPI GdipSetSolidFillColor(void *brush, uint32_t color)
{
    struct gp_brush *b = BRUSH(brush);
    if (!b)
        return InvalidParameter;
    b->color = color;
    b->gradient = false;
    return Ok;
}

static int WINAPI GdipGetSolidFillColor(void *brush, uint32_t *color)
{
    struct gp_brush *b = BRUSH(brush);
    if (!b || !color)
        return InvalidParameter;
    *color = b->color;
    return Ok;
}

static int WINAPI GdipCloneBrush(void *brush, void **out)
{
    struct gp_brush *b = BRUSH(brush), *c = b && out ? gp_new(GP_BRUSH, sizeof(*c)) : NULL;
    if (!c)
        return b && out ? OutOfMemory : InvalidParameter;
    *c = *b;
    *out = c;
    return Ok;
}

static int WINAPI GdipDeleteBrush(void *brush)
{
    struct gp_brush *b = BRUSH(brush);
    if (!b)
        return InvalidParameter;
    gp_free(b);
    return Ok;
}

static int WINAPI GdipGetBrushType(void *brush, int *type)
{
    struct gp_brush *b = BRUSH(brush);
    if (!b || !type)
        return InvalidParameter;
    *type = b->gradient ? 4 : 0;
    return Ok;
}

int w32_gp_pen(uint32_t color, int width, void *brush, void **out)
{
    struct gp_brush *b = brush ? BRUSH(brush) : NULL;
    struct gp_pen *p = out && (!brush || b) ? gp_new(GP_PEN, sizeof(*p)) : NULL;
    if (!p)
        return out && (!brush || b) ? OutOfMemory : InvalidParameter;
    p->color = b ? b->color : color;
    p->width = width;
    *out = p;
    return Ok;
}

int w32_gp_pen_width(void *pen, int width, int *get)
{
    struct gp_pen *p = PEN(pen);
    if (!p)
        return InvalidParameter;
    if (get)
        *get = p->width;
    else
        p->width = width;
    return Ok;
}

static int WINAPI GdipDeletePen(void *pen)
{
    struct gp_pen *p = PEN(pen);
    if (!p)
        return InvalidParameter;
    gp_free(p);
    return Ok;
}

static int WINAPI GdipClonePen(void *pen, void **out)
{
    struct gp_pen *p = PEN(pen), *c = p && out ? gp_new(GP_PEN, sizeof(*c)) : NULL;
    if (!c)
        return p && out ? OutOfMemory : InvalidParameter;
    *c = *p;
    *out = c;
    return Ok;
}

static int WINAPI GdipSetPenColor(void *pen, uint32_t color)
{
    struct gp_pen *p = PEN(pen);
    if (!p)
        return InvalidParameter;
    p->color = color;
    return Ok;
}

static int WINAPI GdipGetPenColor(void *pen, uint32_t *color)
{
    struct gp_pen *p = PEN(pen);
    if (!p || !color)
        return InvalidParameter;
    *color = p->color;
    return Ok;
}

static int WINAPI GdipSetPenDashStyle(void *pen, int dash)
{
    struct gp_pen *p = PEN(pen);
    if (!p)
        return InvalidParameter;
    p->dash = dash;
    return Ok;
}

static int WINAPI GdipSetPenBrushFill(void *pen, void *brush)
{
    struct gp_pen *p = PEN(pen);
    struct gp_brush *b = BRUSH(brush);
    if (!p || !b)
        return InvalidParameter;
    p->color = b->color;
    return Ok;
}

/* caps and joins: the pen draws square dots, which already covers the flat and square ones */
static int WINAPI pen_shape(void *pen, int value)
{
    UNUSED(value);
    return PEN(pen) ? Ok : InvalidParameter;
}

static int WINAPI pen_caps(void *pen, int start, int end, int dash)
{
    UNUSED(start);
    UNUSED(end);
    UNUSED(dash);
    return PEN(pen) ? Ok : InvalidParameter;
}

/* fonts and text: GDI's font choice for the size, the kernel's text renderer */

static int family_new(const char *face, void **out)
{
    struct gp_font *f = out ? gp_new(GP_FAMILY, sizeof(*f)) : NULL;
    if (!f)
        return out ? OutOfMemory : InvalidParameter;
    str_copy(f->face, face, sizeof(f->face));
    *out = f;
    return Ok;
}

static int WINAPI GdipCreateFontFamilyFromName(const WCHAR *name, void *collection, void **out)
{
    UNUSED(collection);
    char *face = w32_narrow(name);
    int r = family_new(face ? face : "Segoe UI", out);
    w32_free(face);
    return r;
}

static int WINAPI GdipGetGenericFontFamilySansSerif(void **out)
{
    return family_new("Segoe UI", out);
}

static int WINAPI GdipGetGenericFontFamilySerif(void **out)
{
    return family_new("Times New Roman", out);
}

static int WINAPI GdipGetGenericFontFamilyMonospace(void **out)
{
    return family_new("Courier New", out);
}

static int WINAPI GdipCloneFontFamily(void *family, void **out)
{
    struct gp_font *f = FAMILY(family);
    return f ? family_new(f->face, out) : InvalidParameter;
}

static int WINAPI GdipDeleteFontFamily(void *family)
{
    struct gp_font *f = FAMILY(family);
    if (!f)
        return InvalidParameter;
    gp_free(f);
    return Ok;
}

static int WINAPI GdipGetFamilyName(void *family, WCHAR *name, WORD lang)
{
    UNUSED(lang);
    struct gp_font *f = FAMILY(family) ? FAMILY(family) : FONT(family);
    if (!f || !name)
        return InvalidParameter;
    w32_cp_to_wide(f->face, -1, name, 32, 1);
    return Ok;
}

int w32_gp_font(void *family, int px, int style, void **out)
{
    struct gp_font *fam = FAMILY(family), *f = fam && out ? gp_new(GP_FONT, sizeof(*f)) : NULL;
    if (!f)
        return fam && out ? OutOfMemory : InvalidParameter;
    str_copy(f->face, fam->face, sizeof(f->face));
    f->px = MAX(px, 1);
    f->style = style;
    *out = f;
    return Ok;
}

int w32_gp_font_px(void *font)
{
    struct gp_font *f = FONT(font);
    return f ? f->px : -1;
}

static int font_from_logfont(const LONG *lf, const char *face, void **out)
{
    struct gp_font *f = out ? gp_new(GP_FONT, sizeof(*f)) : NULL;
    if (!f)
        return out ? OutOfMemory : InvalidParameter;
    str_copy(f->face, face && *face ? face : "Segoe UI", sizeof(f->face));
    f->px = lf[0] < 0 ? -lf[0] : lf[0] ? lf[0] * 3 / 4 : 12;
    f->style = (lf[4] >= 600 ? 1 : 0) | (((const uint8_t *)lf)[20] ? 2 : 0);
    *out = f;
    return Ok;
}

static int WINAPI GdipCreateFontFromLogfontW(HANDLE dc, const LONG *lf, void **out)
{
    UNUSED(dc);
    if (!lf)
        return InvalidParameter;
    char *face = w32_narrow((const WCHAR *)((const uint8_t *)lf + 28));
    int r = font_from_logfont(lf, face, out);
    w32_free(face);
    return r;
}

static int WINAPI GdipCreateFontFromLogfontA(HANDLE dc, const LONG *lf, void **out)
{
    UNUSED(dc);
    return lf ? font_from_logfont(lf, (const char *)lf + 28, out) : InvalidParameter;
}

static int WINAPI GdipCreateFontFromDC(HANDLE dc, void **out)
{
    UNUSED(dc);
    static const LONG lf[7] = {-12, 0, 0, 0, 400, 0, 0};
    return font_from_logfont(lf, "Segoe UI", out);
}

static int WINAPI GdipCloneFont(void *font, void **out)
{
    struct gp_font *f = FONT(font), *c = f && out ? gp_new(GP_FONT, sizeof(*c)) : NULL;
    if (!c)
        return f && out ? OutOfMemory : InvalidParameter;
    *c = *f;
    *out = c;
    return Ok;
}

static int WINAPI GdipDeleteFont(void *font)
{
    struct gp_font *f = FONT(font);
    if (!f)
        return InvalidParameter;
    gp_free(f);
    return Ok;
}

static int WINAPI GdipGetFontStyle(void *font, int *style)
{
    struct gp_font *f = FONT(font);
    if (!f || !style)
        return InvalidParameter;
    *style = f->style;
    return Ok;
}

static int WINAPI GdipGetFamily(void *font, void **out)
{
    struct gp_font *f = FONT(font);
    return f ? family_new(f->face, out) : InvalidParameter;
}

static int WINAPI GdipGetLogFontW(void *font, void *graphics, LONG *lf)
{
    UNUSED(graphics);
    struct gp_font *f = FONT(font);
    if (!f || !lf)
        return InvalidParameter;
    memset(lf, 0, 92);
    lf[0] = -f->px;
    lf[4] = f->style & 1 ? 700 : 400;
    ((uint8_t *)lf)[20] = f->style & 2 ? 1 : 0;
    w32_cp_to_wide(f->face, -1, (WCHAR *)((uint8_t *)lf + 28), 32, 1);
    return Ok;
}

static int WINAPI GdipCreateStringFormat(int flags, WORD lang, void **out)
{
    UNUSED(lang);
    struct gp_format *f = out ? gp_new(GP_FORMAT, sizeof(*f)) : NULL;
    if (!f)
        return out ? OutOfMemory : InvalidParameter;
    f->flags = flags;
    *out = f;
    return Ok;
}

static int WINAPI GdipStringFormatGetGenericDefault(void **out)
{
    return GdipCreateStringFormat(0, 0, out);
}

static int WINAPI GdipStringFormatGetGenericTypographic(void **out)
{
    return GdipCreateStringFormat(0x4004, 0, out);
}

static int WINAPI GdipCloneStringFormat(void *format, void **out)
{
    struct gp_format *f = FORMAT(format), *c = f && out ? gp_new(GP_FORMAT, sizeof(*c)) : NULL;
    if (!c)
        return f && out ? OutOfMemory : InvalidParameter;
    *c = *f;
    *out = c;
    return Ok;
}

static int WINAPI GdipDeleteStringFormat(void *format)
{
    struct gp_format *f = FORMAT(format);
    if (!f)
        return InvalidParameter;
    gp_free(f);
    return Ok;
}

static int format_set(void *format, int slot, int value)
{
    struct gp_format *f = FORMAT(format);
    if (!f)
        return InvalidParameter;
    if (slot == 0)
        f->align = value;
    else if (slot == 1)
        f->line_align = value;
    else if (slot == 2)
        f->flags = value;
    return Ok;
}

static int WINAPI GdipSetStringFormatAlign(void *f, int v)
{
    return format_set(f, 0, v);
}

static int WINAPI GdipSetStringFormatLineAlign(void *f, int v)
{
    return format_set(f, 1, v);
}

static int WINAPI GdipSetStringFormatFlags(void *f, int v)
{
    return format_set(f, 2, v);
}

static int WINAPI GdipSetStringFormatTrimming(void *f, int v)
{
    return format_set(f, 3, v);
}

static int WINAPI GdipSetStringFormatHotkeyPrefix(void *f, int v)
{
    return format_set(f, 3, v);
}

static int WINAPI GdipGetStringFormatAlign(void *format, int *v)
{
    struct gp_format *f = FORMAT(format);
    if (!f || !v)
        return InvalidParameter;
    *v = f->align;
    return Ok;
}

static int WINAPI GdipGetStringFormatLineAlign(void *format, int *v)
{
    struct gp_format *f = FORMAT(format);
    if (!f || !v)
        return InvalidParameter;
    *v = f->line_align;
    return Ok;
}

static const struct ui_font *ui_font_of(const struct gp_font *f)
{
    return w32_pick_font(-f->px, f->style & 1 ? 700 : 400, f->face, 0);
}

/* the text's size in pixels on one line, or wrapped to width when it is given */
int w32_gp_measure(const WCHAR *text, int len, void *font, int width, int *w, int *h)
{
    struct gp_font *f = FONT(font);
    if (!f || !text)
        return InvalidParameter;
    const struct ui_font *uf = ui_font_of(f);
    size_t n = len < 0 ? w32_wcslen(text) : (size_t)len;
    char *s = kmalloc(n * 3 + 1);
    if (!s)
        return OutOfMemory;
    size_t k = w32_wide_to_cp(text, (int)n, s, n * 3, 1);
    s[k] = 0;
    int tw = gfx_text_width(uf, s), lines = 1;
    if (width > 0 && tw > width) {
        lines = (tw + width - 1) / width;
        tw = width;
    }
    kfree(s);
    *w = tw;
    *h = lines * uf->height;
    return Ok;
}

int w32_gp_draw_text(void *graphics, const WCHAR *text, int len, void *font, const int *box, void *format,
                     void *brush)
{
    struct gp_graphics *g = GFX(graphics);
    struct gp_font *f = FONT(font);
    struct gp_brush *b = BRUSH(brush);
    struct gp_format *fmt = format ? FORMAT(format) : NULL;
    struct surf s;
    if (!g || !f || !b || !text)
        return InvalidParameter;
    if (!surf_of(g, &s))
        return Ok;
    const struct ui_font *uf = ui_font_of(f);
    size_t n = len < 0 ? w32_wcslen(text) : (size_t)len;
    char *str = kmalloc(n * 3 + 1);
    if (!str)
        return OutOfMemory;
    size_t k = w32_wide_to_cp(text, (int)n, str, n * 3, 1);
    str[k] = 0;
    for (char *p = str; *p; p++)
        if (*p == '&' && (!fmt || fmt->flags != 0))
            memmove(p, p + 1, strlen(p));
    int tw = gfx_text_width(uf, str), x = dev_x(&s, box[0]), y = dev_y(&s, box[1]);
    int bw = box[2] ? (int)((int64_t)box[2] * s.s->sx / ONE) : 0, bh = box[3] ? (int)((int64_t)box[3] * s.s->sy / ONE) : 0;
    if (fmt && bw)
        x += fmt->align == 1 ? (bw - tw) / 2 : fmt->align == 2 ? bw - tw : 0;
    if (fmt && bh)
        y += fmt->line_align == 1 ? (bh - uf->height) / 2 : fmt->line_align == 2 ? bh - uf->height : 0;
    struct rect old = s.c->clip, clip = s.clip;
    if (bw && bh && !(fmt && (fmt->flags & 0x4000)))
        clip = rect_intersect(clip, dev_rect(&s, box[0], box[1], box[2], box[3]));
    s.c->clip = clip;
    if (bw && tw > bw && !(fmt && (fmt->flags & 0x1000)))
        ui_wrap_text(s.c, (struct rect){x, y, bw, bh ? bh : 4096}, uf, str, b->color | 0xff000000u);
    else
        gfx_text(s.c, x, y, uf, str, b->color | 0xff000000u);
    s.c->clip = old;
    mark(&s, (struct rect){x, y, MAX(tw, bw), MAX(uf->height, bh)});
    kfree(str);
    surf_done(&s);
    return Ok;
}


/* regions: one rectangle or everything */

static int WINAPI GdipCreateRegion(void **out)
{
    struct gp_region *r = out ? gp_new(GP_REGION, sizeof(*r)) : NULL;
    if (!r)
        return out ? OutOfMemory : InvalidParameter;
    r->infinite = true;
    *out = r;
    return Ok;
}

int w32_gp_region_rect(int x, int y, int w, int h, void **out)
{
    int r = GdipCreateRegion(out);
    if (r == Ok) {
        struct gp_region *g = *out;
        g->r = (struct rect){x, y, w, h};
        g->infinite = false;
    }
    return r;
}

static int WINAPI GdipCreateRegionRectI(const int *rect, void **out)
{
    return rect ? w32_gp_region_rect(rect[0], rect[1], rect[2], rect[3], out) : InvalidParameter;
}

static int WINAPI GdipDeleteRegion(void *region)
{
    struct gp_region *r = gp_of(region, GP_REGION);
    if (!r)
        return InvalidParameter;
    gp_free(r);
    return Ok;
}

int w32_gp_region_set(void *region, int x, int y, int w, int h, bool infinite)
{
    struct gp_region *r = gp_of(region, GP_REGION);
    if (!r)
        return InvalidParameter;
    r->r = (struct rect){x, y, w, h};
    r->infinite = infinite;
    return Ok;
}

int w32_gp_region_get(void *region, int *rect)
{
    struct gp_region *r = gp_of(region, GP_REGION);
    if (!r)
        return InvalidParameter;
    struct rect v = r->infinite ? (struct rect){-(1 << 22), -(1 << 22), 1 << 23, 1 << 23} : r->r;
    rect[0] = v.x;
    rect[1] = v.y;
    rect[2] = v.w;
    rect[3] = v.h;
    return Ok;
}

static int WINAPI GdipGetRegionBoundsI(void *region, void *graphics, int *rect)
{
    UNUSED(graphics);
    return rect ? w32_gp_region_get(region, rect) : InvalidParameter;
}

static int WINAPI GdipIsInfiniteRegion(void *region, void *graphics, BOOL *out)
{
    UNUSED(graphics);
    struct gp_region *r = gp_of(region, GP_REGION);
    if (!r || !out)
        return InvalidParameter;
    *out = r->infinite;
    return Ok;
}

static int WINAPI GdipGetClip(void *graphics, void *region)
{
    struct gp_graphics *g = GFX(graphics);
    if (!g || !gp_of(region, GP_REGION))
        return InvalidParameter;
    if (!g->s.clipped)
        return w32_gp_region_set(region, 0, 0, 0, 0, true);
    int rect[4];
    GdipGetClipBoundsI(graphics, rect);
    return w32_gp_region_set(region, rect[0], rect[1], rect[2], rect[3], false);
}

static int WINAPI GdipSetClipRegion(void *graphics, void *region, int mode)
{
    struct gp_region *r = gp_of(region, GP_REGION);
    if (!GFX(graphics) || !r)
        return InvalidParameter;
    if (r->infinite)
        return mode == 1 ? Ok : GdipResetClip(graphics);
    return GdipSetClipRectI(graphics, r->r.x, r->r.y, r->r.w, r->r.h, mode);
}

static int WINAPI GdipGetRegionHRgn(void *region, void *graphics, HANDLE *out)
{
    UNUSED(graphics);
    struct gp_region *r = gp_of(region, GP_REGION);
    if (!r || !out)
        return InvalidParameter;
    HANDLE(WINAPI * make)(int, int, int, int) =
        (HANDLE(WINAPI *)(int, int, int, int))w32_builtin_lookup("gdi32.dll", "CreateRectRgn");
    *out = r->infinite || !make ? NULL : make(r->r.x, r->r.y, r->r.x + r->r.w, r->r.y + r->r.h);
    return Ok;
}

/* paths: polygons that fill and outline like the polygon calls */

static int WINAPI GdipCreatePath(int fill, void **out)
{
    UNUSED(fill);
    struct gp_path *p = out ? gp_new(GP_PATH, sizeof(*p)) : NULL;
    if (!p)
        return out ? OutOfMemory : InvalidParameter;
    *out = p;
    return Ok;
}

static int WINAPI GdipDeletePath(void *path)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p)
        return InvalidParameter;
    kfree(p->pts);
    kfree(p->starts);
    gp_free(p);
    return Ok;
}

static int WINAPI GdipResetPath(void *path)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p)
        return InvalidParameter;
    p->n = p->figures = p->open = 0;
    return Ok;
}

static int WINAPI GdipStartPathFigure(void *path)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p)
        return InvalidParameter;
    p->open = 0;
    return Ok;
}

/* points join the open figure, or start a new one */
int w32_gp_path_add(void *path, const int *pts, int n, bool close)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p || !pts || n <= 0)
        return InvalidParameter;
    if (p->n + n > p->cap || p->figures + 1 > p->cap) {
        int cap = MAX(p->cap * 2, p->n + n + 16);
        int *pts2 = krealloc(p->pts, sizeof(int) * 2 * (size_t)cap), *st2 = pts2 ? krealloc(p->starts, sizeof(int) * (size_t)cap) : NULL;
        if (pts2)
            p->pts = pts2;
        if (!st2)
            return OutOfMemory;
        p->starts = st2;
        p->cap = cap;
    }
    if (!p->open)
        p->starts[p->figures++] = p->n;
    memcpy(p->pts + p->n * 2, pts, sizeof(int) * 2 * (size_t)n);
    p->n += n;
    p->open = !close;
    return Ok;
}

static int WINAPI GdipClosePathFigure(void *path)
{
    return GdipStartPathFigure(path);
}

static int WINAPI GdipAddPathRectangleI(void *path, int x, int y, int w, int h)
{
    GdipStartPathFigure(path);
    return w32_gp_path_add(path, (const int[]){x, y, x + w, y, x + w, y + h, x, y + h}, 4, true);
}

static int WINAPI GdipAddPathLineI(void *path, int x0, int y0, int x1, int y1)
{
    return w32_gp_path_add(path, (const int[]){x0, y0, x1, y1}, 2, false);
}

static int WINAPI GdipAddPathPolygonI(void *path, const int *pts, int n)
{
    GdipStartPathFigure(path);
    return w32_gp_path_add(path, pts, n, true);
}

static int WINAPI GdipWindingModeOutline(void *path, void *matrix, int flatness)
{
    UNUSED(matrix);
    UNUSED(flatness);
    return gp_of(path, GP_PATH) ? Ok : InvalidParameter;
}

static int WINAPI GdipGetPointCount(void *path, int *n)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p || !n)
        return InvalidParameter;
    *n = p->n;
    return Ok;
}

static int WINAPI GdipFillPath(void *graphics, void *brush, void *path)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p)
        return InvalidParameter;
    for (int f = 0; f < p->figures; f++) {
        int end = f + 1 < p->figures ? p->starts[f + 1] : p->n, r = Ok;
        if (end - p->starts[f] >= 3)
            r = w32_gp_fill_poly(graphics, brush, p->pts + p->starts[f] * 2, end - p->starts[f]);
        if (r != Ok)
            return r;
    }
    return Ok;
}

static int WINAPI GdipDrawPath(void *graphics, void *pen, void *path)
{
    struct gp_path *p = gp_of(path, GP_PATH);
    if (!p)
        return InvalidParameter;
    for (int f = 0; f < p->figures; f++) {
        int end = f + 1 < p->figures ? p->starts[f + 1] : p->n, r = Ok;
        if (end - p->starts[f] >= 2)
            r = w32_gp_draw_lines(graphics, pen, p->pts + p->starts[f] * 2, end - p->starts[f], true);
        if (r != Ok)
            return r;
    }
    return Ok;
}

/* a packed DIB: 24 or 32 bits per pixel, either row order */
static int WINAPI GdipCreateBitmapFromGdiDib(const uint8_t *info, const uint8_t *bits, void **out)
{
    if (!info || !bits || !out)
        return InvalidParameter;
    int32_t w, h;
    uint16_t bpp;
    uint32_t compression;
    memcpy(&w, info + 4, 4);
    memcpy(&h, info + 8, 4);
    memcpy(&bpp, info + 14, 2);
    memcpy(&compression, info + 16, 4);
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3) || w <= 0 || !h)
        return NotImplemented;
    int rows = h < 0 ? -h : h, stride = (w * bpp / 8 + 3) & ~3;
    struct gp_image *im = image_new(w, rows, 1);
    if (!im)
        return OutOfMemory;
    for (int y = 0; y < rows; y++)
        row_in(bpp == 24 ? FMT_24RGB : FMT_32RGB, bits + (size_t)(h < 0 ? y : rows - 1 - y) * (size_t)stride,
               im->px + (size_t)y * (size_t)w, w);
    im->format = bpp == 24 ? FMT_24RGB : FMT_32RGB;
    *out = im;
    return Ok;
}

static int WINAPI GdipCreateHatchBrush(int style, uint32_t fore, uint32_t back, void **out)
{
    UNUSED(style);
    UNUSED(back);
    return GdipCreateSolidFill(fore, out);
}

static int WINAPI GdipSetPropertyItem(void *image, const void *item)
{
    return IMG(image) && item ? Ok : InvalidParameter;
}

static int WINAPI GdipGetStringFormatFlags(void *format, int *flags)
{
    struct gp_format *f = FORMAT(format);
    if (!f || !flags)
        return InvalidParameter;
    *flags = f->flags;
    return Ok;
}

static int WINAPI GdipSetStringFormatMeasurableCharacterRanges(void *format, int n, const int *ranges)
{
    struct gp_format *f = FORMAT(format);
    if (!f || n < 0 || n > 32 || (n && !ranges))
        return InvalidParameter;
    f->nranges = MIN(n, 8);
    memcpy(f->ranges, ranges, sizeof(int) * 2 * (size_t)f->nranges);
    return Ok;
}

int w32_gp_ranges(void *format, int (**ranges)[2])
{
    struct gp_format *f = FORMAT(format);
    if (!f)
        return -1;
    *ranges = f->ranges;
    return f->nranges;
}

/* the pixel offset and width of text[first, first + count) on one line */
int w32_gp_text_span(const WCHAR *text, int len, void *font, int first, int count, int *x, int *w)
{
    struct gp_font *f = FONT(font);
    if (!f || !text)
        return InvalidParameter;
    const struct ui_font *uf = ui_font_of(f);
    size_t n = len < 0 ? w32_wcslen(text) : (size_t)len;
    first = MIN(MAX(first, 0), (int)n);
    count = MIN(MAX(count, 0), (int)n - first);
    char *s = kmalloc(n * 3 + 1);
    if (!s)
        return OutOfMemory;
    size_t a = w32_wide_to_cp(text, first, s, n * 3, 1);
    s[a] = 0;
    *x = gfx_text_width(uf, s);
    size_t b = w32_wide_to_cp(text + first, count, s, n * 3, 1);
    s[b] = 0;
    *w = gfx_text_width(uf, s);
    kfree(s);
    return Ok;
}

int w32_gp_clip_bounds(void *graphics, int *rect)
{
    return GdipGetClipBoundsI(graphics, rect);
}

int w32_gp_image_res(void *image)
{
    return IMG(image) ? 96 : -1;
}

int w32_gp_font_height(void *font)
{
    struct gp_font *f = FONT(font);
    return f ? ui_font_of(f)->height : -1;
}

/* image attributes, cached bitmaps */

static int WINAPI GdipCreateImageAttributes(void **out)
{
    if (!out)
        return InvalidParameter;
    *out = gp_new(GP_ATTR, sizeof(struct gp));
    return *out ? Ok : OutOfMemory;
}

static int WINAPI GdipDisposeImageAttributes(void *attrs)
{
    struct gp *a = gp_of(attrs, GP_ATTR);
    if (!a)
        return InvalidParameter;
    gp_free(a);
    return Ok;
}

static int WINAPI GdipSetImageAttributesWrapMode(void *attrs, int wrap, uint32_t color, BOOL clamp)
{
    UNUSED(wrap);
    UNUSED(color);
    UNUSED(clamp);
    return gp_of(attrs, GP_ATTR) ? Ok : InvalidParameter;
}

static int WINAPI GdipSetImageAttributesColorMatrix(void *attrs, int type, BOOL enable, const void *m, const void *g,
                                                    int flags)
{
    UNUSED(type);
    UNUSED(enable);
    UNUSED(m);
    UNUSED(g);
    UNUSED(flags);
    return gp_of(attrs, GP_ATTR) ? Ok : InvalidParameter;
}

static int WINAPI GdipCreateCachedBitmap(void *image, void *graphics, void **out)
{
    if (!GFX(graphics))
        return InvalidParameter;
    return GdipCloneImage(image, out);
}

static int WINAPI GdipDrawCachedBitmap(void *graphics, void *cached, int x, int y)
{
    return GdipDrawImageI(graphics, cached, x, y);
}

static const struct w32_export gdiplus_exports[] = {
    W32_FN(GdiplusStartup), W32_FN(GdiplusShutdown), W32_FN(GdipNotificationHook), W32_FN(GdipNotificationUnhook),
    W32_FN(GdipAlloc), W32_FN(GdipFree),
    W32_FN(GdipLoadImageFromFile), W32_AS("GdipLoadImageFromFileICM", GdipLoadImageFromFile),
    W32_AS("GdipCreateBitmapFromFile", GdipLoadImageFromFile),
    W32_AS("GdipCreateBitmapFromFileICM", GdipLoadImageFromFile), W32_FN(GdipLoadImageFromStream),
    W32_AS("GdipLoadImageFromStreamICM", GdipLoadImageFromStream),
    W32_AS("GdipCreateBitmapFromStream", GdipLoadImageFromStream),
    W32_AS("GdipCreateBitmapFromStreamICM", GdipLoadImageFromStream), W32_FN(GdipCreateBitmapFromScan0),
    W32_FN(GdipCreateBitmapFromHBITMAP), W32_FN(GdipCreateHBITMAPFromBitmap), W32_FN(GdipCreateBitmapFromGraphics),
    W32_FN(GdipCloneImage), W32_FN(GdipCloneBitmapAreaI), W32_FN(GdipDisposeImage), W32_FN(GdipGetImageWidth),
    W32_FN(GdipGetImageHeight), W32_FN(GdipGetImagePixelFormat), W32_FN(GdipGetImageType), W32_FN(GdipGetImageFlags),
    W32_FN(GdipGetImageRawFormat), W32_FN(GdipImageGetFrameDimensionsCount), W32_FN(GdipImageGetFrameDimensionsList),
    W32_FN(GdipImageGetFrameCount), W32_FN(GdipImageSelectActiveFrame), W32_FN(GdipGetPropertyItemSize),
    W32_FN(GdipGetPropertyItem), W32_FN(GdipGetPropertyCount), W32_FN(GdipGetPropertyIdList),
    W32_FN(GdipBitmapLockBits), W32_FN(GdipBitmapUnlockBits), W32_FN(GdipBitmapGetPixel), W32_FN(GdipBitmapSetPixel),
    W32_FN(GdipImageRotateFlip), W32_FN(GdipGetImageThumbnail), W32_FN(GdipGetImageEncodersSize),
    W32_FN(GdipGetImageEncoders), W32_FN(GdipGetImageDecodersSize), W32_FN(GdipGetImageDecoders),
    W32_FN(GdipSaveImageToFile), W32_FN(GdipSaveImageToStream),
    W32_FN(GdipCreateFromHDC), W32_FN(GdipCreateFromHDC2), W32_FN(GdipCreateFromHWND),
    W32_AS("GdipCreateFromHWNDICM", GdipCreateFromHWND), W32_FN(GdipGetImageGraphicsContext),
    W32_FN(GdipDeleteGraphics), W32_FN(GdipGetDC), W32_FN(GdipReleaseDC), W32_FN(GdipGraphicsClear),
    W32_FN(GdipSetSmoothingMode), W32_FN(GdipGetSmoothingMode), W32_FN(GdipSetInterpolationMode),
    W32_FN(GdipGetInterpolationMode), W32_FN(GdipSetPixelOffsetMode), W32_FN(GdipGetPixelOffsetMode),
    W32_FN(GdipSetCompositingMode), W32_FN(GdipGetCompositingMode), W32_FN(GdipSetCompositingQuality),
    W32_FN(GdipGetCompositingQuality), W32_FN(GdipSetPageUnit), W32_FN(GdipGetPageUnit),
    W32_FN(GdipSetTextRenderingHint), W32_FN(GdipGetTextRenderingHint), W32_FN(GdipSetTextContrast),
    W32_FN(GdipGetTextContrast), W32_FN(GdipResetWorldTransform), W32_FN(GdipSetWorldTransform),
    W32_FN(GdipGetWorldTransform), W32_FN(GdipCreateMatrix), W32_FN(GdipDeleteMatrix), W32_FN(GdipSetClipRectI),
    W32_FN(GdipResetClip), W32_FN(GdipGetClipBoundsI), W32_AS("GdipGetVisibleClipBoundsI", GdipGetClipBoundsI),
    W32_FN(GdipSaveGraphics), W32_FN(GdipRestoreGraphics),
    W32_FN(GdipFillRectangleI), W32_FN(GdipFillRectanglesI), W32_FN(GdipFillPolygonI), W32_FN(GdipFillPolygon2I),
    W32_FN(GdipFillEllipseI), W32_FN(GdipDrawLineI), W32_FN(GdipDrawLinesI), W32_FN(GdipDrawPolygonI),
    W32_FN(GdipDrawRectangleI), W32_FN(GdipDrawRectanglesI), W32_FN(GdipDrawEllipseI), W32_FN(GdipDrawImageI),
    W32_FN(GdipDrawImageRectI), W32_FN(GdipDrawImageRectRectI), W32_FN(GdipDrawImagePointRectI),
    W32_FN(GdipCreateSolidFill), W32_FN(GdipCreateLineBrushI), W32_FN(GdipCreateLineBrushFromRectI),
    W32_FN(GdipSetSolidFillColor), W32_FN(GdipGetSolidFillColor), W32_FN(GdipCloneBrush), W32_FN(GdipDeleteBrush),
    W32_FN(GdipGetBrushType), W32_FN(GdipDeletePen), W32_FN(GdipClonePen), W32_FN(GdipSetPenColor),
    W32_FN(GdipGetPenColor), W32_FN(GdipSetPenDashStyle), W32_FN(GdipSetPenBrushFill),
    W32_AS("GdipSetPenLineJoin", pen_shape), W32_AS("GdipSetPenStartCap", pen_shape),
    W32_AS("GdipSetPenEndCap", pen_shape), W32_AS("GdipSetPenDashCap197819", pen_shape),
    W32_AS("GdipSetPenMode", pen_shape), W32_AS("GdipSetPenLineCap197819", pen_caps),
    W32_FN(GdipCreateFontFamilyFromName), W32_FN(GdipGetGenericFontFamilySansSerif),
    W32_FN(GdipGetGenericFontFamilySerif), W32_FN(GdipGetGenericFontFamilyMonospace), W32_FN(GdipCloneFontFamily),
    W32_FN(GdipDeleteFontFamily), W32_FN(GdipGetFamilyName), W32_FN(GdipCreateFontFromLogfontW),
    W32_FN(GdipCreateFontFromLogfontA), W32_FN(GdipCreateFontFromDC), W32_FN(GdipCloneFont), W32_FN(GdipDeleteFont),
    W32_FN(GdipGetFontStyle), W32_FN(GdipGetFamily), W32_FN(GdipGetLogFontW), W32_FN(GdipCreateStringFormat),
    W32_FN(GdipStringFormatGetGenericDefault), W32_FN(GdipStringFormatGetGenericTypographic),
    W32_FN(GdipCloneStringFormat), W32_FN(GdipDeleteStringFormat), W32_FN(GdipSetStringFormatAlign),
    W32_FN(GdipSetStringFormatLineAlign), W32_FN(GdipSetStringFormatFlags), W32_FN(GdipSetStringFormatTrimming),
    W32_FN(GdipSetStringFormatHotkeyPrefix), W32_FN(GdipGetStringFormatAlign), W32_FN(GdipGetStringFormatLineAlign),
    W32_FN(GdipCreateImageAttributes), W32_FN(GdipDisposeImageAttributes), W32_FN(GdipSetImageAttributesWrapMode),
    W32_FN(GdipSetImageAttributesColorMatrix), W32_FN(GdipCreateCachedBitmap),
    W32_AS("GdipDeleteCachedBitmap", GdipDisposeImage), W32_FN(GdipDrawCachedBitmap),
    W32_FN(GdipCreateRegion), W32_FN(GdipCreateRegionRectI), W32_FN(GdipDeleteRegion), W32_FN(GdipGetRegionBoundsI),
    W32_FN(GdipIsInfiniteRegion), W32_FN(GdipGetClip), W32_FN(GdipSetClipRegion), W32_FN(GdipGetRegionHRgn),
    W32_FN(GdipCreatePath), W32_FN(GdipDeletePath), W32_FN(GdipResetPath), W32_FN(GdipStartPathFigure),
    W32_FN(GdipClosePathFigure), W32_FN(GdipAddPathRectangleI), W32_FN(GdipAddPathLineI), W32_FN(GdipAddPathPolygonI),
    W32_FN(GdipWindingModeOutline), W32_FN(GdipGetPointCount), W32_FN(GdipFillPath), W32_FN(GdipDrawPath),
    W32_FN(GdipCreateBitmapFromGdiDib), W32_FN(GdipCreateHatchBrush), W32_FN(GdipSetPropertyItem),
    W32_FN(GdipGetStringFormatFlags), W32_FN(GdipSetStringFormatMeasurableCharacterRanges),
};

W32_EXPORTS(gdiplus_module, "gdiplus.dll", gdiplus_exports);
#endif
