/* Icons and cursors with pixels: resources (RT_GROUP_ICON/RT_ICON, PNG or DIB), CreateIconIndirect, DrawIconEx.
   System icons (LoadIcon(NULL, IDI_*)) stay the small 0x30000|id handles, which draw nothing. */
#include <synth/config.h>
#if CONFIG_GUI
#include <synth/picture.h>
#include <synth/string.h>

#include "w32gui.h"

#define ICON_MAGIC 0x4e4f4349u

struct icon {
    uint32_t magic;
    int w, h, hx, hy;
    bool cursor;
    uint32_t px[]; /* straight ARGB, top-down */
};

static struct icon *icon_of(HANDLE h)
{
    struct icon *i = h;
    return w32_heap_owns(h, sizeof(*i)) && i->magic == ICON_MAGIC ? i : NULL;
}

static struct icon *icon_new(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
        return NULL;
    struct icon *i = w32_zalloc(sizeof(*i) + (size_t)w * (size_t)h * 4);
    if (i) {
        i->magic = ICON_MAGIC;
        i->w = w;
        i->h = h;
    }
    return i;
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

/* an area-averaged copy at another size (premultiplied while summing, so edges keep their colour) */
static struct icon *icon_scaled(const struct icon *s, int w, int h)
{
    struct icon *d = icon_new(w, h);
    if (!d)
        return NULL;
    d->hx = s->hx * w / s->w;
    d->hy = s->hy * h / s->h;
    d->cursor = s->cursor;
    for (int y = 0; y < h; y++) {
        int y0 = y * s->h / h, y1 = MAX((y + 1) * s->h / h, y0 + 1);
        for (int x = 0; x < w; x++) {
            int x0 = x * s->w / w, x1 = MAX((x + 1) * s->w / w, x0 + 1);
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, n++) {
                    uint32_t p = s->px[yy * s->w + xx], pa = p >> 24;
                    a += pa;
                    r += (p >> 16 & 0xff) * pa;
                    g += (p >> 8 & 0xff) * pa;
                    b += (p & 0xff) * pa;
                }
            d->px[y * w + x] = a ? (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | b / a : 0;
        }
    }
    return d;
}

static struct icon *icon_sized(struct icon *i, int cx, int cy)
{
    if (!i || ((!cx || cx == i->w) && (!cy || cy == i->h)))
        return i;
    struct icon *s = icon_scaled(i, cx ? cx : i->w, cy ? cy : i->h);
    w32_free(i);
    return s;
}

/* one RT_ICON/RT_CURSOR image: PNG, or a DIB of twice the height (colours, then the AND mask) */
static struct icon *icon_decode(const uint8_t *res, uint32_t size, bool icon)
{
    int hx = 0, hy = 0;
    if (!icon) {
        if (size < 4)
            return NULL;
        hx = (int)rd16(res);
        hy = (int)rd16(res + 2);
        res += 4;
        size -= 4;
    }
    struct icon *i = NULL;
    if (size >= 8 && res[0] == 0x89 && res[1] == 'P' && res[2] == 'N' && res[3] == 'G') {
        struct picture p;
        if (png_decode(res, size, &p) < 0)
            return NULL;
        i = icon_new(p.w, p.h);
        if (i)
            memcpy(i->px, p.px, (size_t)p.w * (size_t)p.h * 4);
        picture_free(&p);
    } else {
        if (size < 40 || rd32(res) < 40 || rd32(res) > size)
            return NULL;
        int w = (int)rd32(res + 4), h = (int)rd32(res + 8) / 2, bpp = (int)rd16(res + 14);
        uint32_t used = rd32(res + 32), colors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32))
            return NULL;
        const uint8_t *pal = res + rd32(res), *xor_bits = pal + colors * 4;
        uint32_t xs = ((uint32_t)w * (uint32_t)bpp + 31) / 32 * 4, as = ((uint32_t)w + 31) / 32 * 4;
        const uint8_t *and_bits = xor_bits + xs * (uint32_t)h;
        if ((uint32_t)(and_bits - res) > size)
            return NULL;
        bool has_mask = (uint32_t)(and_bits - res) + as * (uint32_t)h <= size, alpha = false;
        if (bpp == 32)
            for (int n = 0; n < w * h && !alpha; n++)
                alpha = xor_bits[(uint32_t)(n / w) * xs + (uint32_t)(n % w) * 4 + 3] != 0;
        if (!(i = icon_new(w, h)))
            return NULL;
        for (int y = 0; y < h; y++) {
            const uint8_t *row = xor_bits + (uint32_t)(h - 1 - y) * xs, *mrow = and_bits + (uint32_t)(h - 1 - y) * as;
            for (int x = 0; x < w; x++) {
                uint32_t c, a = 0xff;
                switch (bpp) {
                case 32: c = rd32(row + x * 4) & 0xffffff; a = alpha ? row[x * 4 + 3] : 0xff; break;
                case 24: c = (uint32_t)row[x * 3] | (uint32_t)row[x * 3 + 1] << 8 | (uint32_t)row[x * 3 + 2] << 16; break;
                case 8: c = row[x]; break;
                case 4: c = (row[x / 2] >> (x & 1 ? 0 : 4)) & 15; break;
                default: c = (row[x / 8] >> (7 - x % 8)) & 1; break;
                }
                if (bpp <= 8)
                    c = c < colors ? rd32(pal + c * 4) & 0xffffff : 0;
                if (!alpha && has_mask && (mrow[x / 8] >> (7 - x % 8) & 1))
                    a = 0;
                i->px[y * w + x] = a << 24 | c;
            }
        }
    }
    if (i) {
        i->cursor = !icon;
        i->hx = icon ? i->w / 2 : hx;
        i->hy = icon ? i->h / 2 : hy;
    }
    return i;
}

/* the entry of an icon/cursor group directory nearest the wanted size (deeper colours win ties) */
static int pick_entry(const uint8_t *dir, uint32_t size, bool icon, int want)
{
    int n = size >= 6 ? (int)rd16(dir + 4) : 0, best = -1, best_score = 0;
    for (int k = 0; k < n && 6 + (uint32_t)(k + 1) * 14 <= size; k++) {
        const uint8_t *e = dir + 6 + k * 14;
        int w = icon ? (e[0] ? e[0] : 256) : (int)rd16(e), bpp = (int)rd16(e + 6);
        int score = (w >= want ? (w - want) * 2 : (want - w) * 5) * 64 - bpp;
        if (best < 0 || score < best_score) {
            best = k;
            best_score = score;
        }
    }
    return best;
}

static const void *res_name(const void *name, bool wide, char **owned)
{
    *owned = NULL;
    if ((uintptr_t)name < 0x10000 || !wide)
        return name;
    *owned = w32_narrow(name);
    return *owned;
}

static struct icon *icon_resource(HANDLE inst, const void *name, bool wide, bool icon, int cx, int cy)
{
    char *owned;
    const void *n = res_name(name, wide, &owned);
    uint32_t size = 0;
    const uint8_t *dir = n ? w32_find_resource(inst, (const void *)(uintptr_t)(icon ? 14 : 12), n, &size) : NULL;
    w32_free(owned);
    int k = dir ? pick_entry(dir, size, icon, cx ? cx : 32) : -1;
    if (k < 0)
        return NULL;
    uint32_t id = rd16(dir + 6 + k * 14 + 12), len = 0;
    const uint8_t *res = w32_find_resource(inst, (const void *)(uintptr_t)(icon ? 3 : 1), (const void *)(uintptr_t)id, &len);
    return res ? icon_sized(icon_decode(res, len, icon), cx, cy) : NULL;
}

/* LoadImage/LoadIcon of IMAGE_ICON (and program cursors); NULL when the module has no such resource */
HANDLE w32_icon_load(HANDLE inst, const void *name, bool wide, bool icon, int cx, int cy)
{
    return inst ? icon_resource(inst, name, wide, icon, cx, cy) : NULL;
}

/* paints an icon into width x height straight-ARGB pixels (an image list cell) */
bool w32_icon_render(HANDLE h, uint32_t *out, int w, int hgt)
{
    struct icon *i = icon_of(h), *s = i && (i->w != w || i->h != hgt) ? icon_scaled(i, w, hgt) : i;
    if (!s)
        return false;
    memcpy(out, s->px, (size_t)w * (size_t)hgt * 4);
    if (s != i)
        w32_free(s);
    return true;
}

HANDLE w32_icon_from_argb(const uint32_t *px, int w, int h, int stride)
{
    struct icon *i = icon_new(w, h);
    if (i) {
        for (int y = 0; y < h; y++)
            memcpy(i->px + y * w, px + (size_t)y * (size_t)stride, (size_t)w * 4);
        i->hx = w / 2;
        i->hy = h / 2;
    }
    return i;
}

bool w32_icon_size(HANDLE h, int *w, int *hgt)
{
    struct icon *i = icon_of(h);
    if (!i)
        return false;
    *w = i->w;
    *hgt = i->h;
    return true;
}

/* draws into a canvas at (x, y) clipped to clip; DI_IMAGE alone draws without transparency, DI_MASK alone the mask */
void w32_icon_paint(HANDLE h, struct canvas *c, struct rect clip, int x, int y, int w, int hgt, UINT how)
{
    struct icon *i = icon_of(h);
    if (!i || w <= 0 || hgt <= 0)
        return;
    struct icon *s = i->w != w || i->h != hgt ? icon_scaled(i, w, hgt) : i;
    if (!s)
        return;
    for (int yy = 0; yy < hgt; yy++) {
        int ty = y + yy;
        if (ty < clip.y || ty >= clip.y + clip.h || ty < 0 || ty >= c->h)
            continue;
        for (int xx = 0; xx < w; xx++) {
            int tx = x + xx;
            if (tx < clip.x || tx >= clip.x + clip.w || tx < 0 || tx >= c->w)
                continue;
            uint32_t *o = c->px + ty * c->stride + tx, p = s->px[yy * w + xx], a = p >> 24, u = *o;
            if ((how & 3) == 1) {
                *o = a ? 0xff000000u : 0xffffffffu;
                continue;
            }
            if ((how & 3) == 2)
                a = 0xff;
            if (!a)
                continue;
            uint32_t r = ((p >> 16 & 0xff) * a + (u >> 16 & 0xff) * (255 - a)) / 255,
                     g = ((p >> 8 & 0xff) * a + (u >> 8 & 0xff) * (255 - a)) / 255,
                     b = ((p & 0xff) * a + (u & 0xff) * (255 - a)) / 255;
            *o = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
    if (s != i)
        w32_free(s);
}

static BOOL WINAPI DrawIconEx(HANDLE dc, int x, int y, HANDLE h, int cx, int cy, UINT step, HANDLE brush, UINT flags)
{
    UNUSED(step);
    struct icon *i = icon_of(h);
    if (!h)
        return FALSE;
    int w = cx ? cx : (flags & 8) || !i ? 32 : i->w, hgt = cy ? cy : (flags & 8) || !i ? 32 : i->h;
    if (brush) {
        RECT r = {x, y, x + w, y + hgt};
        w32_fill_brush(dc, &r, brush);
    }
    int ox, oy;
    struct rect clip;
    struct canvas *c = i ? w32_dc_target(dc, &ox, &oy, &clip) : NULL;
    if (!c)
        return TRUE;
    w32_icon_paint(h, c, clip, ox + x, oy + y, w, hgt, flags ? flags : 3);
    w32_dc_damage(dc, (struct rect){ox + x, oy + y, w, hgt});
    return TRUE;
}

static BOOL WINAPI DrawIcon(HANDLE dc, int x, int y, HANDLE h)
{
    return DrawIconEx(dc, x, y, h, 0, 0, 0, NULL, 3 | 8);
}

static HANDLE copy_icon(HANDLE h, int cx, int cy)
{
    struct icon *i = icon_of(h);
    if (!i)
        return h;
    struct icon *c = icon_new(i->w, i->h);
    if (c) {
        memcpy(c, i, sizeof(*i) + (size_t)i->w * (size_t)i->h * 4);
        c = icon_sized(c, cx, cy);
    }
    return c;
}

static HANDLE WINAPI CopyIcon(HANDLE h)
{
    return copy_icon(h, 0, 0);
}

HANDLE w32_icon_copy(HANDLE h, int cx, int cy)
{
    return copy_icon(h, cx, cy);
}

static BOOL WINAPI DestroyIcon(HANDLE h)
{
    struct icon *i = icon_of(h);
    if (i) {
        i->magic = 0;
        w32_free(i);
    }
    return TRUE;
}

/* ICONINFO: fIcon, xHotspot, yHotspot, hbmMask, hbmColor (handles 8 bytes apart for 64-bit programs) */
static BOOL WINAPI GetIconInfo(HANDLE h, uint8_t *info)
{
    struct icon *i = icon_of(h);
    if (!info || !h)
        return FALSE;
    int w = i ? i->w : 32, hgt = i ? i->h : 32;
    uint32_t *mask = w32_zalloc((size_t)w * (size_t)hgt * 4);
    if (!mask)
        return FALSE;
    for (int n = 0; n < w * hgt; n++)
        mask[n] = i && i->px[n] >> 24 ? 0 : 0xffffff;
    HANDLE m = w32_bitmap_argb(w, hgt, mask, w);
    /* the colour bitmap is black where the icon is clear, so SRCAND mask + SRCINVERT colour draws it */
    for (int n = 0; n < w * hgt; n++)
        mask[n] = i && i->px[n] >> 24 ? i->px[n] : 0;
    HANDLE color = w32_bitmap_argb(w, hgt, mask, w);
    w32_free(mask);
    uint32_t head[3] = {i ? !i->cursor : 1, i ? (uint32_t)i->hx : 16, i ? (uint32_t)i->hy : 16};
    memcpy(info, head, sizeof(head));
    uint64_t hm = (uintptr_t)m, hc = (uintptr_t)color;
    if (w32->win64) {
        memcpy(info + 16, &hm, 8);
        memcpy(info + 24, &hc, 8);
    } else {
        memcpy(info + 12, &hm, 4);
        memcpy(info + 16, &hc, 4);
    }
    return TRUE;
}

/* colour bitmap with its alpha (or the mask where it has none); without one, a mask of twice the height: AND then XOR */
static HANDLE WINAPI CreateIconIndirect(const uint8_t *info)
{
    if (!info)
        return NULL;
    uint32_t head[3];
    memcpy(head, info, sizeof(head));
    uint64_t hm = 0, hc = 0;
    if (w32->win64) {
        memcpy(&hm, info + 16, 8);
        memcpy(&hc, info + 24, 8);
    } else {
        memcpy(&hm, info + 12, 4);
        memcpy(&hc, info + 16, 4);
    }
    HANDLE mask = (HANDLE)(uintptr_t)hm, color = (HANDLE)(uintptr_t)hc;
    int w, h, mw = 0, mh = 0;
    bool has_mask = w32_bitmap_size(mask, &mw, &mh);
    if (!w32_bitmap_size(color ? color : mask, &w, &h))
        return NULL;
    if (!color)
        h /= 2;
    struct icon *i = icon_new(w, h);
    if (!i)
        return NULL;
    bool partial = false, any = false;
    uint32_t p, m;
    for (int y = 0; color && y < h; y++)
        for (int x = 0; x < w; x++)
            if (w32_bitmap_pixel(color, x, y, &p)) {
                any |= (p >> 24) != 0;
                partial |= (p >> 24) != 0xff;
            }
    bool alpha = any && partial;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t a = 0xff;
            if (color) {
                if (!w32_bitmap_pixel(color, x, y, &p))
                    p = 0;
                if (alpha)
                    a = p >> 24;
                else if (has_mask && w32_bitmap_pixel(mask, x, y, &m) && (m & 0xffffff))
                    a = 0;
            } else {
                bool clear = w32_bitmap_pixel(mask, x, y, &m) && (m & 0xffffff);
                p = w32_bitmap_pixel(mask, x, y + h, &m) && (m & 0xffffff) ? 0xffffff : 0;
                a = clear ? 0 : 0xff;
            }
            i->px[y * w + x] = a << 24 | (p & 0xffffff);
        }
    i->cursor = !head[0];
    i->hx = head[0] ? w / 2 : (int)head[1];
    i->hy = head[0] ? h / 2 : (int)head[2];
    return i;
}

static HANDLE WINAPI CreateIconFromResourceEx(const uint8_t *bits, DWORD size, BOOL icon, DWORD version, int cx, int cy,
                                              UINT flags)
{
    UNUSED(version);
    if (!bits || size < 8)
        return NULL;
    if ((flags & 0x40) && !cx)
        cx = cy = 32;
    return icon_sized(icon_decode(bits, size, icon), cx, cy);
}

static HANDLE WINAPI CreateIconFromResource(const uint8_t *bits, DWORD size, BOOL icon, DWORD version)
{
    return CreateIconFromResourceEx(bits, size, icon, version, 0, 0, 0);
}

static int WINAPI LookupIconIdFromDirectoryEx(const uint8_t *dir, BOOL icon, int cx, int cy, UINT flags)
{
    UNUSED(cy);
    UNUSED(flags);
    int k = dir ? pick_entry(dir, 6 + rd16(dir + 4) * 14u, icon, cx ? cx : 32) : -1;
    return k < 0 ? 0 : (int)rd16(dir + 6 + k * 14 + 12);
}

static int WINAPI LookupIconIdFromDirectory(const uint8_t *dir, BOOL icon)
{
    return LookupIconIdFromDirectoryEx(dir, icon, 0, 0, 0);
}

/* CreateIcon: an AND mask and XOR bits, top-down, rows padded to 16 bits */
static HANDLE WINAPI CreateIcon(HANDLE inst, int w, int h, BYTE planes, BYTE bpp, const uint8_t *and_bits,
                                const uint8_t *xor_bits)
{
    UNUSED(inst);
    UNUSED(planes);
    struct icon *i = icon_new(w, h);
    if (!i)
        return NULL;
    int as = (w + 15) / 16 * 2, xs = (w * bpp + 15) / 16 * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool clear = and_bits && (and_bits[y * as + x / 8] >> (7 - x % 8) & 1);
            uint32_t c = 0;
            if (xor_bits && bpp == 32)
                c = rd32(xor_bits + y * xs + x * 4) & 0xffffff;
            else if (xor_bits && bpp == 24)
                c = rd32(xor_bits + y * xs + x * 3) & 0xffffff;
            else if (xor_bits && bpp == 1)
                c = (xor_bits[y * xs + x / 8] >> (7 - x % 8) & 1) ? 0xffffff : 0;
            i->px[y * w + x] = (clear ? 0 : 0xff000000u) | c;
        }
    i->hx = w / 2;
    i->hy = h / 2;
    return i;
}

static const struct w32_export icon_exports[] = {
    W32_FN(DrawIconEx),
    W32_FN(DrawIcon),
    W32_FN(CopyIcon),
    W32_FN(DestroyIcon),
    W32_FN(GetIconInfo),
    W32_FN(CreateIconIndirect),
    W32_FN(CreateIconFromResourceEx),
    W32_FN(CreateIconFromResource),
    W32_FN(LookupIconIdFromDirectoryEx),
    W32_FN(LookupIconIdFromDirectory),
    W32_FN(CreateIcon),
};

W32_EXPORTS(icon_module, "user32.dll", icon_exports);
#endif
