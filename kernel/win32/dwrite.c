#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/string.h>
#include <synth/vfs.h>

#include "win64.h"

#define S_OK 0u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000eu
#define E_NOT_FOUND 0x80070490u
#define A uint32_t
#define MAX_FACES 96

#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                                      \
    {(uint8_t)(d1), (uint8_t)((d1) >> 8), (uint8_t)((d1) >> 16), (uint8_t)((d1) >> 24), (uint8_t)(d2),               \
     (uint8_t)((d2) >> 8), (uint8_t)(d3), (uint8_t)((d3) >> 8), a, b, c, d, e, f, g, h}

static const uint8_t iid_unknown[16] = GUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const uint8_t iid_factory[16] = GUID(0xb859ee5a, 0xd838, 0x4b5b, 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48);
static const uint8_t iid_collection[16] =
    GUID(0xa84cee02, 0x3eea, 0x4eee, 0xa8, 0x27, 0x87, 0xc1, 0xa0, 0x2a, 0x0f, 0xcc);
static const uint8_t iid_family[16] = GUID(0xda20d8ef, 0x812a, 0x4c43, 0x98, 0x02, 0x62, 0xec, 0x4a, 0xbd, 0x7a, 0xdd);
static const uint8_t iid_list[16] = GUID(0x1a0d8438, 0x1d97, 0x4ec1, 0xae, 0xf9, 0xa2, 0xfb, 0x86, 0xed, 0x6a, 0xcb);
static const uint8_t iid_font[16] = GUID(0xacd16696, 0x8c14, 0x4f5d, 0x87, 0x7e, 0xfe, 0x3f, 0xc1, 0xd3, 0x27, 0x37);
static const uint8_t iid_face[16] = GUID(0x5f49804d, 0x7024, 0x4d43, 0xbf, 0xa9, 0xd2, 0x59, 0x84, 0xf5, 0x38, 0x49);
static const uint8_t iid_file[16] = GUID(0x739d886a, 0xcef5, 0x47dc, 0x87, 0x69, 0x1a, 0x8b, 0x41, 0xbe, 0xbb, 0xb0);
static const uint8_t iid_loader[16] = GUID(0x727cad4e, 0xd6af, 0x4c9e, 0x8a, 0x08, 0xd6, 0x95, 0xb1, 0x1c, 0xaa, 0x49);
static const uint8_t iid_local_loader[16] =
    GUID(0xb2d9f3ec, 0xc9fe, 0x4a11, 0xa2, 0xec, 0xd8, 0x62, 0x08, 0xf7, 0xc0, 0xa2);
static const uint8_t iid_stream[16] = GUID(0x6d4865fe, 0x0ab8, 0x4d91, 0x8f, 0x62, 0x5d, 0xd6, 0xbe, 0x34, 0xa3, 0xe0);
static const uint8_t iid_analysis[16] = GUID(0x7d97dbf7, 0xe085, 0x42d4, 0x81, 0xe3, 0x6a, 0x88, 0x3b, 0xde, 0xd1, 0x18);
static const uint8_t iid_strings[16] = GUID(0x08256209, 0x099a, 0x4b34, 0xb8, 0x6d, 0xc2, 0x2b, 0x11, 0x0e, 0x77, 0x71);
static const uint8_t iid_gdi_interop[16] =
    GUID(0x1edd9491, 0x9853, 0x4299, 0x89, 0x8f, 0x64, 0x32, 0x98, 0x3b, 0x6f, 0x3a);

struct face {
    char path[PATH_MAX_LEN];
    char family[64], style[32];
    const uint8_t *data;
    uint32_t size;
    uint16_t weight, units_per_em, glyphs, metrics_count;
    uint8_t font_style, mono, serif;
    int16_t ascent, descent, line_gap, cap_height, x_height, underline_pos, underline_size, strike_pos, strike_size;
    uint32_t cmap, hmtx, loca, glyf;
    bool long_loca;
    int family_index;
};

static struct face *faces;
static int face_count, family_count;
static int family_first[MAX_FACES];
static bool scanned;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t table(const struct face *f, const char *tag, uint32_t *len)
{
    uint16_t n = be16(f->data + 4);
    for (uint16_t i = 0; i < n && 12u + i * 16u + 16 <= f->size; i++) {
        const uint8_t *r = f->data + 12 + i * 16;
        if (!memcmp(r, tag, 4)) {
            uint32_t off = be32(r + 8), size = be32(r + 12);
            if (off + size > f->size)
                return 0;
            if (len)
                *len = size;
            return off;
        }
    }
    return 0;
}

static void read_name(const struct face *f, uint16_t id, char *out, size_t cap)
{
    uint32_t len, off = table(f, "name", &len);
    out[0] = 0;
    if (!off || len < 6)
        return;
    const uint8_t *t = f->data + off;
    uint16_t count = be16(t + 2), strings = be16(t + 4);
    int best = -1, best_score = 0;
    for (uint16_t i = 0; i < count && 6u + i * 12u + 12 <= len; i++) {
        const uint8_t *r = t + 6 + i * 12;
        if (be16(r + 6) != id)
            continue;
        int score = be16(r) == 3 ? (be16(r + 4) == 0x409 ? 3 : 2) : be16(r) == 1 ? 1 : 0;
        if (score > best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best < 0)
        return;
    const uint8_t *r = t + 6 + best * 12;
    uint16_t slen = be16(r + 8), soff = be16(r + 10);
    if ((uint32_t)strings + soff + slen > len)
        return;
    const uint8_t *s = t + strings + soff;
    size_t n = 0;
    if (be16(r) == 3)
        for (uint32_t k = 0; k + 1 < slen && n + 1 < cap; k += 2)
            out[n++] = s[k] ? '?' : (char)s[k + 1];
    else
        for (uint16_t k = 0; k < slen && n + 1 < cap; k++)
            out[n++] = (char)s[k];
    out[n] = 0;
}

static bool parse_face(struct face *f)
{
    if (f->size < 12 || (be32(f->data) != 0x00010000u && memcmp(f->data, "true", 4) && memcmp(f->data, "OTTO", 4)))
        return false;
    uint32_t len, head = table(f, "head", &len), hhea = table(f, "hhea", NULL), maxp = table(f, "maxp", NULL);
    if (!head || !hhea || !maxp)
        return false;
    f->units_per_em = be16(f->data + head + 18);
    f->long_loca = be16(f->data + head + 50) != 0;
    f->glyphs = be16(f->data + maxp + 4);
    f->metrics_count = be16(f->data + hhea + 34);
    f->ascent = (int16_t)be16(f->data + hhea + 4);
    f->descent = (int16_t)-(int16_t)be16(f->data + hhea + 6);
    f->line_gap = (int16_t)be16(f->data + hhea + 8);
    f->weight = 400;
    f->x_height = (int16_t)(f->units_per_em / 2);
    f->cap_height = (int16_t)(f->units_per_em * 7 / 10);
    f->strike_size = f->underline_size = (int16_t)(f->units_per_em / 14);
    f->strike_pos = (int16_t)(f->units_per_em * 3 / 10);
    f->underline_pos = (int16_t)-(f->units_per_em / 10);
    uint32_t os2 = table(f, "OS/2", &len);
    if (os2 && len >= 78) {
        const uint8_t *o = f->data + os2;
        f->weight = be16(o + 4);
        f->strike_size = (int16_t)be16(o + 26);
        f->strike_pos = (int16_t)be16(o + 28);
        f->font_style = (be16(o + 62) & 1) ? 2 : 0;
        f->ascent = (int16_t)be16(o + 74);
        f->descent = (int16_t)be16(o + 76);
        if (be16(o) >= 2 && len >= 90) {
            f->x_height = (int16_t)be16(o + 86);
            f->cap_height = (int16_t)be16(o + 88);
        }
    }
    uint32_t post = table(f, "post", &len);
    if (post && len >= 12) {
        f->underline_pos = (int16_t)be16(f->data + post + 8);
        f->underline_size = (int16_t)be16(f->data + post + 10);
        f->mono = be32(f->data + post + 12) != 0;
    }
    f->hmtx = table(f, "hmtx", NULL);
    f->loca = table(f, "loca", NULL);
    f->glyf = table(f, "glyf", NULL);
    uint32_t cmap = table(f, "cmap", &len);
    if (cmap) {
        const uint8_t *c = f->data + cmap;
        uint16_t n = be16(c + 2);
        uint32_t best = 0;
        int best_score = 0;
        for (uint16_t i = 0; i < n && 4u + i * 8u + 8 <= len; i++) {
            uint16_t platform = be16(c + 4 + i * 8), encoding = be16(c + 6 + i * 8);
            uint32_t off = be32(c + 8 + i * 8);
            uint16_t format = off + 2 <= len ? be16(c + off) : 0;
            int score = format == 12 ? 3 : format == 4 && (platform == 3 || platform == 0) ? 2 : 0;
            if (platform == 3 && encoding == 0)
                score = format == 4 ? 1 : 0;
            if (score > best_score) {
                best_score = score;
                best = cmap + off;
            }
        }
        f->cmap = best;
    }
    read_name(f, 16, f->family, sizeof(f->family));
    if (!f->family[0])
        read_name(f, 1, f->family, sizeof(f->family));
    read_name(f, 2, f->style, sizeof(f->style));
    if (str_casecmp(f->style, "italic") == 0 || strstr(f->style, "Italic"))
        f->font_style = 2;
    else if (strstr(f->style, "Oblique"))
        f->font_style = 1;
    f->serif = strstr(f->family, "Serif") && !strstr(f->family, "Sans");
    if (strstr(f->family, "Mono"))
        f->mono = 1;
    return f->family[0] && f->cmap && f->hmtx;
}

static void scan_dir(const char *dir)
{
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (face_count < MAX_FACES && vfs_readdir(&d, &ent) == 0) {
        const char *dot = strrchr(ent.name, '.');
        if (!dot || (str_casecmp(dot, ".ttf") && str_casecmp(dot, ".otf")))
            continue;
        struct face *f = &faces[face_count];
        memset(f, 0, sizeof(*f));
        ksnprintf(f->path, sizeof(f->path), "%s/%s", dir, ent.name);
        char *data = NULL;
        size_t size = 0;
        if (vfs_read_all(f->path, &data, &size) != 0)
            continue;
        f->data = (const uint8_t *)data;
        f->size = (uint32_t)size;
        if (parse_face(f))
            face_count++;
        else
            kfree(data);
    }
    vfs_closedir(&d);
}

static void scan_fonts(void)
{
    if (scanned)
        return;
    scanned = true;
    if (!faces && !(faces = kzalloc(MAX_FACES * sizeof(*faces))))
        return;
    scan_dir("/home/fonts");
    scan_dir("/system/fonts");
    for (int i = 0; i < face_count; i++) {
        faces[i].family_index = -1;
        for (int k = 0; k < family_count; k++)
            if (!str_casecmp(faces[family_first[k]].family, faces[i].family))
                faces[i].family_index = k;
        if (faces[i].family_index < 0) {
            faces[i].family_index = family_count;
            family_first[family_count++] = i;
        }
    }
}

static int alias_family(const char *name)
{
    static const char *const sans[] = {"segoe ui", "arial", "helvetica", "tahoma", "verdana", "calibri",
                                       "microsoft sans serif", "sans", "sans-serif", "system-ui"};
    static const char *const serif[] = {"times new roman", "times", "georgia", "cambria", "serif"};
    static const char *const mono[] = {"courier new", "courier", "consolas", "lucida console", "monospace"};
    int want = -1;
    for (size_t i = 0; i < ARRAY_SIZE(sans); i++)
        want = !str_casecmp(name, sans[i]) ? 0 : want;
    for (size_t i = 0; i < ARRAY_SIZE(serif); i++)
        want = !str_casecmp(name, serif[i]) ? 1 : want;
    for (size_t i = 0; i < ARRAY_SIZE(mono); i++)
        want = !str_casecmp(name, mono[i]) ? 2 : want;
    if (want < 0)
        return -1;
    int fallback = -1;
    for (int k = 0; k < family_count; k++) {
        const struct face *f = &faces[family_first[k]];
        int kind = f->mono ? 2 : f->serif ? 1 : 0;
        if (kind == want)
            return k;
        if (fallback < 0 && kind == 0)
            fallback = k;
    }
    return fallback >= 0 ? fallback : family_count ? 0 : -1;
}

static uint16_t glyph_index(const struct face *f, uint32_t cp)
{
    const uint8_t *c = f->data + f->cmap;
    uint16_t format = be16(c);
    if (format == 12) {
        uint32_t groups = be32(c + 12);
        for (uint32_t lo = 0, hi = groups; lo < hi;) {
            uint32_t mid = (lo + hi) / 2;
            const uint8_t *g = c + 16 + mid * 12;
            if (cp < be32(g))
                hi = mid;
            else if (cp > be32(g + 4))
                lo = mid + 1;
            else
                return (uint16_t)(be32(g + 8) + cp - be32(g));
        }
        return 0;
    }
    if (format != 4 || cp > 0xffff)
        return 0;
    uint16_t segs = be16(c + 6) / 2;
    const uint8_t *ends = c + 14, *starts = ends + segs * 2 + 2, *deltas = starts + segs * 2, *ranges = deltas + segs * 2;
    for (uint16_t i = 0; i < segs; i++) {
        if (cp > be16(ends + i * 2))
            continue;
        uint16_t start = be16(starts + i * 2);
        if (cp < start)
            return 0;
        uint16_t range = be16(ranges + i * 2), delta = be16(deltas + i * 2);
        if (!range)
            return (uint16_t)(cp + delta);
        const uint8_t *p = ranges + i * 2 + range + (cp - start) * 2;
        if (p + 2 > f->data + f->size)
            return 0;
        uint16_t g = be16(p);
        return g ? (uint16_t)(g + delta) : 0;
    }
    return 0;
}

static const uint8_t *glyph_data(const struct face *f, uint16_t g, uint32_t *len)
{
    if (!f->loca || !f->glyf || g >= f->glyphs)
        return NULL;
    uint32_t start = f->long_loca ? be32(f->data + f->loca + g * 4) : be16(f->data + f->loca + g * 2) * 2u;
    uint32_t end = f->long_loca ? be32(f->data + f->loca + g * 4 + 4) : be16(f->data + f->loca + g * 2 + 2) * 2u;
    if (end <= start || f->glyf + end > f->size || end - start < 10)
        return NULL;
    *len = end - start;
    return f->data + f->glyf + start;
}

static void glyph_box(const struct face *f, uint16_t g, int16_t box[4])
{
    uint32_t len;
    const uint8_t *h = glyph_data(f, g, &len);
    memset(box, 0, 4 * sizeof(int16_t));
    if (!h)
        return;
    for (int i = 0; i < 4; i++)
        box[i] = (int16_t)be16(h + 2 + i * 2);
}

static int simple_points(const uint8_t *h, uint32_t len, struct dw_point *pts, int cap, int dx, int dy)
{
    int contours = (int16_t)be16(h);
    uint32_t pos = 10 + (uint32_t)contours * 2;
    if (pos + 2 > len)
        return 0;
    int n = be16(h + pos - 2) + 1;
    pos += 2 + be16(h + pos);
    if (n > cap)
        return 0;
    for (int i = 0; i < n; i++) {
        if (pos >= len)
            return 0;
        uint8_t flag = h[pos++];
        int repeat = 0;
        if (flag & 8) {
            if (pos >= len)
                return 0;
            repeat = h[pos++];
        }
        for (; repeat >= 0 && i < n; repeat--, i++) {
            pts[i].on = flag;
            pts[i].end = 0;
        }
        i--;
    }
    for (int axis = 0; axis < 2; axis++) {
        int32_t v = 0;
        uint8_t is_short = axis ? 4 : 2, same = axis ? 32 : 16;
        for (int i = 0; i < n; i++) {
            uint8_t flag = pts[i].on;
            if (flag & is_short) {
                if (pos >= len)
                    return 0;
                v += flag & same ? h[pos] : -h[pos];
                pos++;
            } else if (!(flag & same)) {
                if (pos + 2 > len)
                    return 0;
                v += (int16_t)be16(h + pos);
                pos += 2;
            }
            if (axis)
                pts[i].y = v + dy;
            else
                pts[i].x = v + dx;
        }
    }
    for (int i = 0; i < n; i++)
        pts[i].on &= 1;
    for (int c = 0; c < contours; c++) {
        int e = be16(h + 10 + c * 2);
        if (e < n)
            pts[e].end = 1;
    }
    pts[n - 1].end = 1;
    return n;
}

static int glyph_points(const struct face *f, uint16_t g, struct dw_point *pts, int cap, int dx, int dy, int depth)
{
    uint32_t len;
    const uint8_t *h = glyph_data(f, g, &len);
    if (!h)
        return 0;
    if ((int16_t)be16(h) >= 0)
        return simple_points(h, len, pts, cap, dx, dy);
    if (depth > 4)
        return 0;
    int total = 0;
    uint32_t pos = 10;
    for (uint16_t flags = 0x20; flags & 0x20;) {
        if (pos + 4 > len)
            break;
        flags = be16(h + pos);
        uint16_t child = be16(h + pos + 2);
        pos += 4;
        int x = 0, y = 0;
        if (flags & 1) {
            if (pos + 4 > len)
                break;
            x = (int16_t)be16(h + pos);
            y = (int16_t)be16(h + pos + 2);
            pos += 4;
        } else {
            if (pos + 2 > len)
                break;
            x = (int8_t)h[pos];
            y = (int8_t)h[pos + 1];
            pos += 2;
        }
        pos += flags & 8 ? 2 : flags & 0x40 ? 4 : flags & 0x80 ? 8 : 0;
        if (!(flags & 2))
            x = y = 0;
        total += glyph_points(f, child, pts + total, cap - total, dx + x, dy + y, depth + 1);
    }
    return total;
}

static void glyph_metrics(const struct face *f, uint16_t g, int32_t *out)
{
    uint16_t m = MIN(g, (uint16_t)(f->metrics_count - 1));
    uint16_t advance = be16(f->data + f->hmtx + m * 4);
    int16_t lsb = g < f->metrics_count ? (int16_t)be16(f->data + f->hmtx + g * 4 + 2)
                                       : (int16_t)be16(f->data + f->hmtx + f->metrics_count * 4 + (g - f->metrics_count) * 2);
    int16_t box[4];
    glyph_box(f, g, box);
    int32_t height = f->ascent + f->descent;
    out[0] = lsb;
    out[1] = advance;
    out[2] = (int32_t)advance - lsb - (box[2] - box[0]);
    out[3] = f->ascent - box[3];
    out[4] = (uint32_t)height;
    out[5] = height - out[3] - (box[3] - box[1]);
    out[6] = f->ascent;
}

int dwrite_glyph_outline(int face, uint16_t glyph, struct dw_point *pts, int cap, int *advance, int *units)
{
    const struct face *f = &faces[face];
    int32_t m[7];
    glyph_metrics(f, glyph, m);
    *advance = m[1];
    *units = f->units_per_em ? f->units_per_em : 2048;
    return glyph_points(f, glyph, pts, cap, 0, 0, 0);
}

static NORETURN void missing_method(const char *iface, int slot)
{
    ksnprintf(w32->message, sizeof(w32->message), "DirectWrite %s method %d is not supported yet", iface, slot);
    w32_exit(0xc0000139u);
}

#define TRAP(iface, n)                                                                                             \
    static void WINAPI trap_##iface##_##n(void) { missing_method(#iface, n); }
#define TRAPS(iface, a, b, c, d) TRAP(iface, a) TRAP(iface, b) TRAP(iface, c) TRAP(iface, d)

struct obj {
    const void *const *vtbl;
    LONG refs;
    const uint8_t *iid, *iid2;
    int index;
    void *extra;
    void *proxy;
};

static uint32_t WINAPI query_interface(struct obj *self, const uint8_t *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (!memcmp(iid, iid_unknown, 16) || !memcmp(iid, self->iid, 16) || (self->iid2 && !memcmp(iid, self->iid2, 16))) {
        __atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED);
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static uint32_t WINAPI add_ref(struct obj *self)
{
    return (uint32_t)__atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED);
}

static uint32_t WINAPI release(struct obj *self)
{
    LONG n = __atomic_sub_fetch(&self->refs, 1, __ATOMIC_RELAXED);
    if (n == 0) {
        w32_free(self->extra);
        w32_free(self->proxy);
        w32_free(self);
    }
    return (uint32_t)n;
}

static struct obj *com_new(const void *const *vtbl, const uint8_t *iid, const uint8_t *iid2, int index)
{
    struct obj *o = w32_zalloc(sizeof(*o));
    if (o)
        *o = (struct obj){vtbl, 1, iid, iid2, index, NULL, NULL};
    return o;
}

static uint32_t give(void **out, struct obj *o)
{
    *out = o;
    return o ? S_OK : E_OUTOFMEMORY;
}

static uint32_t WINAPI ok2(void *self, A a)
{
    UNUSED(self);
    UNUSED(a);
    return S_OK;
}

static uint32_t WINAPI none2(void *self, void **out)
{
    UNUSED(self);
    if (out)
        *out = NULL;
    return E_NOTIMPL;
}

static uint32_t WINAPI none3(void *self, A a, void **out)
{
    UNUSED(a);
    return none2(self, out);
}

static uint32_t WINAPI none4(void *self, A a, A b, void **out)
{
    UNUSED(b);
    return none3(self, a, out);
}

static uint32_t WINAPI none5(void *self, A a, A b, A c, void **out)
{
    UNUSED(c);
    return none4(self, a, b, out);
}

static uint32_t WINAPI none7(void *self, A a, A b, A c, A d, A e, void **out)
{
    UNUSED(d);
    UNUSED(e);
    return none5(self, a, b, c, out);
}

static uint32_t WINAPI none9(void *self, A a, A b, A c, A d, A e, A f, A g, void **out)
{
    UNUSED(f);
    UNUSED(g);
    return none7(self, a, b, c, d, e, out);
}

static uint32_t WINAPI none10(void *self, A a, A b, A c, A d, A e, A f, A g, A h, void **out)
{
    UNUSED(h);
    return none9(self, a, b, c, d, e, f, g, out);
}

/* IDWriteLocalizedStrings: index = face, extra = which name (family/style) */

static const char *strings_text(struct obj *s)
{
    return s->extra ? (const char *)s->extra : faces[s->index].family;
}

static UINT WINAPI strings_count(struct obj *s)
{
    UNUSED(s);
    return 1;
}

static uint32_t WINAPI strings_find(struct obj *s, const WCHAR *locale, UINT *index, BOOL *exists)
{
    UNUSED(s);
    UNUSED(locale);
    *index = 0;
    *exists = TRUE;
    return S_OK;
}

static uint32_t WINAPI strings_locale_length(struct obj *s, UINT index, UINT *len)
{
    UNUSED(s);
    if (index)
        return E_INVALIDARG;
    *len = 5;
    return S_OK;
}

static uint32_t put_wide(const char *text, WCHAR *out, UINT size)
{
    UINT n = (UINT)strlen(text);
    if (size <= n)
        return 0x8007007au;
    for (UINT i = 0; i <= n; i++)
        out[i] = (uint8_t)text[i];
    return S_OK;
}

static uint32_t WINAPI strings_locale(struct obj *s, UINT index, WCHAR *out, UINT size)
{
    UNUSED(s);
    return index ? E_INVALIDARG : put_wide("en-us", out, size);
}

static uint32_t WINAPI strings_length(struct obj *s, UINT index, UINT *len)
{
    if (index)
        return E_INVALIDARG;
    *len = (UINT)strlen(strings_text(s));
    return S_OK;
}

static uint32_t WINAPI strings_get(struct obj *s, UINT index, WCHAR *out, UINT size)
{
    return index ? E_INVALIDARG : put_wide(strings_text(s), out, size);
}

TRAPS(strings, 9, 10, 11, 12)

static const void *const strings_vtbl[] = {
    query_interface, add_ref, release, strings_count, strings_find, strings_locale_length, strings_locale,
    strings_length, strings_get,
     trap_strings_9, trap_strings_10, trap_strings_11, trap_strings_12,
};

static struct obj *new_strings(int face, const char *text)
{
    struct obj *s = com_new(strings_vtbl, iid_strings, NULL, face);
    if (s && text && (s->extra = w32_alloc(strlen(text) + 1)))
        str_copy(s->extra, text, strlen(text) + 1);
    return s;
}

/* Font files, loader and stream: index = face */

static const void *const fstream_vtbl[];
static const void *const loader_vtbl[];

static uint32_t WINAPI fstream_read(struct obj *s, const void **start, uint64_t offset, uint64_t size, void **context)
{
    const struct face *f = &faces[s->index];
    *context = NULL;
    if (offset > f->size || size > f->size - offset) {
        *start = NULL;
        return 0x80070026u;
    }
    *start = f->data + offset;
    return S_OK;
}

static void WINAPI fstream_release_fragment(struct obj *s, void *context)
{
    UNUSED(s);
    UNUSED(context);
}

static uint32_t WINAPI fstream_size(struct obj *s, uint64_t *size)
{
    *size = faces[s->index].size;
    return S_OK;
}

static uint32_t WINAPI fstream_time(struct obj *s, uint64_t *time)
{
    UNUSED(s);
    *time = 0x01d6a0c4f0000000ull;
    return S_OK;
}

TRAPS(fstream, 7, 8, 9, 10)

static const void *const fstream_vtbl[] = {
    query_interface, add_ref, release, fstream_read, fstream_release_fragment, fstream_size, fstream_time,
     trap_fstream_7, trap_fstream_8, trap_fstream_9, trap_fstream_10,
};

static int key_face(const void *key, UINT size)
{
    int index = size == sizeof(int) ? *(const int *)key : -1;
    return index >= 0 && index < face_count ? index : -1;
}

static uint32_t WINAPI loader_stream(struct obj *l, const void *key, UINT size, void **out)
{
    UNUSED(l);
    int index = key_face(key, size);
    if (index < 0) {
        *out = NULL;
        return E_INVALIDARG;
    }
    return give(out, com_new(fstream_vtbl, iid_stream, NULL, index));
}

static uint32_t WINAPI loader_path_length(struct obj *l, const void *key, UINT size, UINT *len)
{
    UNUSED(l);
    int index = key_face(key, size);
    if (index < 0)
        return E_INVALIDARG;
    char win[PATH_MAX_LEN];
    w32_path_to_win(faces[index].path, win, sizeof(win));
    *len = (UINT)strlen(win);
    return S_OK;
}

static uint32_t WINAPI loader_path(struct obj *l, const void *key, UINT size, WCHAR *out, UINT cap)
{
    UNUSED(l);
    int index = key_face(key, size);
    if (index < 0)
        return E_INVALIDARG;
    char win[PATH_MAX_LEN];
    w32_path_to_win(faces[index].path, win, sizeof(win));
    return put_wide(win, out, cap);
}

static uint32_t WINAPI loader_time(struct obj *l, const void *key, UINT size, uint64_t *time)
{
    UNUSED(l);
    UNUSED(key);
    UNUSED(size);
    *time = 0x01d6a0c4f0000000ull;
    return S_OK;
}

TRAPS(loader, 7, 8, 9, 10)

static const void *const loader_vtbl[] = {
    query_interface, add_ref, release, loader_stream, loader_path_length, loader_path, loader_time,
     trap_loader_7, trap_loader_8, trap_loader_9, trap_loader_10,
};

static struct obj local_loader = {loader_vtbl, 1 << 30, iid_local_loader, iid_loader, 0, NULL, NULL};

static uint32_t WINAPI file_key(struct obj *f, const void **key, UINT *size)
{
    *key = &f->index;
    *size = sizeof(f->index);
    return S_OK;
}

static uint32_t WINAPI file_loader(struct obj *f, void **out)
{
    UNUSED(f);
    *out = &local_loader;
    return S_OK;
}

static uint32_t WINAPI file_analyze(struct obj *f, BOOL *supported, int *file_type, int *face_type, UINT *count)
{
    *supported = TRUE;
    *file_type = memcmp(faces[f->index].data, "OTTO", 4) ? 2 : 1;
    if (face_type)
        *face_type = memcmp(faces[f->index].data, "OTTO", 4) ? 1 : 0;
    *count = 1;
    return S_OK;
}

TRAPS(file, 6, 7, 8, 9)

static const void *const file_vtbl[] = {
    query_interface, add_ref, release, file_key, file_loader, file_analyze,
    trap_file_6, trap_file_7, trap_file_8, trap_file_9,
};

/* IDWriteFontFace: index = face */

static int WINAPI face_type(struct obj *o)
{
    return memcmp(faces[o->index].data, "OTTO", 4) ? 1 : 0;
}

static uint32_t WINAPI face_files(struct obj *o, UINT *count, void **files)
{
    if (files) {
        if (*count < 1)
            return E_INVALIDARG;
        files[0] = com_new(file_vtbl, iid_file, NULL, o->index);
    }
    *count = 1;
    return S_OK;
}

static UINT WINAPI zero1(void *o)
{
    UNUSED(o);
    return 0;
}

static void WINAPI face_metrics(struct obj *o, uint16_t *m)
{
    const struct face *f = &faces[o->index];
    uint16_t v[10] = {f->units_per_em,
                      (uint16_t)f->ascent,
                      (uint16_t)f->descent,
                      (uint16_t)f->line_gap,
                      (uint16_t)f->cap_height,
                      (uint16_t)f->x_height,
                      (uint16_t)f->underline_pos,
                      (uint16_t)f->underline_size,
                      (uint16_t)f->strike_pos,
                      (uint16_t)f->strike_size};
    memcpy(m, v, sizeof(v));
}

static uint16_t WINAPI face_glyph_count(struct obj *o)
{
    return faces[o->index].glyphs;
}

static uint32_t WINAPI face_design_metrics(struct obj *o, const uint16_t *glyphs, UINT count, int32_t *out, BOOL sideways)
{
    UNUSED(sideways);
    for (UINT i = 0; i < count; i++)
        glyph_metrics(&faces[o->index], glyphs[i], out + i * 7);
    return S_OK;
}

static uint32_t WINAPI face_indices(struct obj *o, const uint32_t *codepoints, UINT count, uint16_t *out)
{
    for (UINT i = 0; i < count; i++)
        out[i] = glyph_index(&faces[o->index], codepoints[i]);
    return S_OK;
}

static uint32_t WINAPI face_table(struct obj *o, uint32_t tag, const void **data, UINT *size, void **context,
                                  BOOL *exists)
{
    char name[4] = {(char)tag, (char)(tag >> 8), (char)(tag >> 16), (char)(tag >> 24)};
    uint32_t len = 0, off = table(&faces[o->index], name, &len);
    *context = NULL;
    *exists = off != 0;
    *data = off ? faces[o->index].data + off : NULL;
    *size = off ? len : 0;
    return S_OK;
}

static void WINAPI face_release_table(struct obj *o, void *context)
{
    UNUSED(o);
    UNUSED(context);
}

static uint32_t WINAPI face_rendering_mode(struct obj *o, float em, float ppd, int measuring, void *params, int *mode)
{
    UNUSED(o);
    UNUSED(em);
    UNUSED(ppd);
    UNUSED(measuring);
    UNUSED(params);
    *mode = 5;
    return S_OK;
}

static uint32_t WINAPI face_gdi_metrics(struct obj *o, float em, float ppd, const void *transform, uint16_t *m)
{
    UNUSED(em);
    UNUSED(ppd);
    UNUSED(transform);
    face_metrics(o, m);
    return S_OK;
}

static uint32_t WINAPI face_gdi_glyph_metrics(struct obj *o, float em, float ppd, const void *transform, BOOL natural,
                                              const uint16_t *glyphs, UINT count, int32_t *out, BOOL sideways)
{
    UNUSED(em);
    UNUSED(ppd);
    UNUSED(transform);
    UNUSED(natural);
    return face_design_metrics(o, glyphs, count, out, sideways);
}

static uint32_t WINAPI face_outline(struct obj *o, uint32_t em, const uint16_t *glyphs, const uint32_t *advances,
                                    const void *offsets, UINT count, BOOL sideways, BOOL rtl, void *sink)
{
    UNUSED(sideways);
    return dwrite_outline(o->index, em, glyphs, advances, offsets, count, rtl, sink);
}

TRAPS(face, 18, 19, 20, 21)

static const void *const face_vtbl[] = {
    query_interface, add_ref,      release,           face_type,   face_files,         zero1,
    zero1,           zero1,        face_metrics,      face_glyph_count, face_design_metrics, face_indices,
    face_table,      face_release_table, face_outline,     face_rendering_mode, face_gdi_metrics, face_gdi_glyph_metrics,
    trap_face_18,    trap_face_19, trap_face_20,      trap_face_21,
};

/* IDWriteFont: index = face */

static struct obj *new_family(int family);

static uint32_t WINAPI font_family(struct obj *o, void **out)
{
    return give(out, new_family(faces[o->index].family_index));
}

static int WINAPI font_weight(struct obj *o)
{
    return faces[o->index].weight;
}

static int WINAPI font_stretch(struct obj *o)
{
    UNUSED(o);
    return 5;
}

static int WINAPI font_style(struct obj *o)
{
    return faces[o->index].font_style;
}

static BOOL WINAPI font_symbol(struct obj *o)
{
    UNUSED(o);
    return FALSE;
}

static uint32_t WINAPI font_face_names(struct obj *o, void **out)
{
    return give(out, new_strings(o->index, faces[o->index].style[0] ? faces[o->index].style : "Regular"));
}

static uint32_t WINAPI font_info_strings(struct obj *o, int id, void **out, BOOL *exists)
{
    *exists = id == 1 || id == 2 || id == 3;
    if (!*exists) {
        *out = NULL;
        return S_OK;
    }
    const char *text = id == 1 ? "Copyright of the font authors" : id == 2 ? "1.0" : faces[o->index].family;
    return give(out, new_strings(o->index, text));
}

static void WINAPI font_metrics(struct obj *o, uint16_t *m)
{
    face_metrics(o, m);
}

static uint32_t WINAPI font_has_char(struct obj *o, uint32_t cp, BOOL *exists)
{
    *exists = glyph_index(&faces[o->index], cp) != 0;
    return S_OK;
}

static uint32_t WINAPI font_create_face(struct obj *o, void **out)
{
    return give(out, com_new(face_vtbl, iid_face, NULL, o->index));
}

TRAPS(font, 14, 15, 16, 17)

static const void *const font_vtbl[] = {
    query_interface, add_ref,        release,           font_family,   font_weight, font_stretch,
    font_style,      font_symbol,    font_face_names,   font_info_strings, zero1,   font_metrics,
    font_has_char,   font_create_face,
     trap_font_14, trap_font_15, trap_font_16, trap_font_17,
};

/* IDWriteFontFamily / IDWriteFontList: index = family */

static struct obj *new_collection(void);

static uint32_t WINAPI family_collection(struct obj *o, void **out)
{
    UNUSED(o);
    return give(out, new_collection());
}

static UINT WINAPI family_font_count(struct obj *o)
{
    UINT n = 0;
    for (int i = 0; i < face_count; i++)
        n += faces[i].family_index == o->index;
    return n;
}

static int family_face(int family, UINT n)
{
    for (int i = 0; i < face_count; i++)
        if (faces[i].family_index == family && n-- == 0)
            return i;
    return -1;
}

static uint32_t WINAPI family_font(struct obj *o, UINT n, void **out)
{
    int face = family_face(o->index, n);
    if (face < 0) {
        *out = NULL;
        return E_INVALIDARG;
    }
    return give(out, com_new(font_vtbl, iid_font, NULL, face));
}

static uint32_t WINAPI family_names(struct obj *o, void **out)
{
    return give(out, new_strings(family_first[o->index], NULL));
}

static uint32_t WINAPI family_match(struct obj *o, int weight, int stretch, int style, void **out)
{
    UNUSED(stretch);
    int best = -1, best_score = 1 << 30;
    for (int i = 0; i < face_count; i++) {
        if (faces[i].family_index != o->index)
            continue;
        int score = (faces[i].weight > weight ? faces[i].weight - weight : weight - faces[i].weight) + (faces[i].font_style != style ? 10000 : 0);
        if (score < best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best < 0) {
        *out = NULL;
        return E_NOT_FOUND;
    }
    return give(out, com_new(font_vtbl, iid_font, NULL, best));
}

static uint32_t WINAPI family_matching_list(struct obj *o, int weight, int stretch, int style, void **out)
{
    UNUSED(weight);
    UNUSED(stretch);
    UNUSED(style);
    return give(out, new_family(o->index));
}

TRAPS(family, 9, 10, 11, 12)

static const void *const family_vtbl[] = {
    query_interface, add_ref, release, family_collection, family_font_count, family_font, family_names,
    family_match,    family_matching_list,
     trap_family_9, trap_family_10, trap_family_11, trap_family_12,
};

static struct obj *new_family(int family)
{
    return com_new(family_vtbl, iid_family, iid_list, family);
}

/* IDWriteFontCollection: extra = struct families for collections built by the program, else all families */

struct families {
    int count;
    int family[MAX_FACES];
};

static UINT WINAPI collection_count(struct obj *o)
{
    const struct families *c = o->extra;
    return (UINT)(c ? c->count : family_count);
}

static uint32_t WINAPI collection_family(struct obj *o, UINT index, void **out)
{
    const struct families *c = o->extra;
    if (index >= collection_count(o)) {
        *out = NULL;
        return E_INVALIDARG;
    }
    return give(out, new_family(c ? c->family[index] : (int)index));
}

static uint32_t WINAPI collection_find(struct obj *o, const WCHAR *name, UINT *index, BOOL *exists)
{
    const struct families *c = o->extra;
    char n[64];
    size_t k = 0;
    for (; name && name[k] && k < sizeof(n) - 1; k++)
        n[k] = name[k] < 0x80 ? (char)name[k] : '?';
    n[k] = 0;
    for (UINT i = 0; i < collection_count(o); i++)
        if (!str_casecmp(faces[family_first[c ? c->family[i] : (int)i]].family, n)) {
            *index = i;
            *exists = TRUE;
            return S_OK;
        }
    int alias = c ? -1 : alias_family(n);
    *exists = alias >= 0;
    *index = alias >= 0 ? (UINT)alias : 0xffffffffu;
    return S_OK;
}

static uint32_t WINAPI collection_from_face(struct obj *o, struct obj *face, void **out)
{
    UNUSED(o);
    if (!face || face->vtbl != face_vtbl) {
        *out = NULL;
        return E_INVALIDARG;
    }
    return give(out, com_new(font_vtbl, iid_font, NULL, face->index));
}

TRAPS(collection, 7, 8, 9, 10)

static const void *const collection_vtbl[] = {
    query_interface, add_ref, release, collection_count, collection_family, collection_find, collection_from_face,
     trap_collection_7, trap_collection_8, trap_collection_9, trap_collection_10,
};

static struct obj *new_collection(void)
{
    return com_new(collection_vtbl, iid_collection, NULL, 0);
}

/* IDWriteGdiInterop: fonts by LOGFONT and back */

#define DWRITE_E_NOFONT 0x88985002u

struct logfont {
    LONG height, width, escapement, orientation, weight;
    uint8_t italic, underline, strike_out, charset, out_precision, clip_precision, quality, pitch_and_family;
    WCHAR face[32];
};

static int find_family(const WCHAR *name)
{
    char n[64];
    size_t k = 0;
    for (; name[k] && k < sizeof(n) - 1; k++)
        n[k] = name[k] < 0x80 ? (char)name[k] : '?';
    n[k] = 0;
    for (int i = 0; i < family_count; i++)
        if (!str_casecmp(faces[family_first[i]].family, n))
            return i;
    return alias_family(n);
}

static uint32_t WINAPI gdi_from_logfont(void *self, const struct logfont *lf, void **out)
{
    UNUSED(self);
    scan_fonts();
    int family = lf ? find_family(lf->face) : -1;
    if (family < 0) {
        *out = NULL;
        return DWRITE_E_NOFONT;
    }
    struct obj f = {family_vtbl, 1, iid_family, iid_list, family, NULL, NULL};
    return family_match(&f, lf->weight ? (int)lf->weight : 400, 5, lf->italic ? 2 : 0, out);
}

static void to_logfont(int face, struct logfont *lf)
{
    memset(lf, 0, sizeof(*lf));
    lf->weight = faces[face].weight;
    lf->italic = faces[face].font_style != 0;
    lf->charset = 1;
    lf->out_precision = 7;
    for (size_t i = 0; i < ARRAY_SIZE(lf->face) - 1 && faces[face].family[i]; i++)
        lf->face[i] = (uint8_t)faces[face].family[i];
}

static uint32_t WINAPI gdi_font_logfont(void *self, struct obj *font, struct logfont *lf, BOOL *system)
{
    UNUSED(self);
    if (!font || !lf)
        return E_INVALIDARG;
    to_logfont(font->index, lf);
    if (system)
        *system = TRUE;
    return S_OK;
}

static uint32_t WINAPI gdi_face_logfont(void *self, struct obj *face, struct logfont *lf)
{
    return gdi_font_logfont(self, face, lf, NULL);
}

TRAPS(gdi, 8, 9, 10, 11)

static const void *const gdi_vtbl[] = {
    query_interface, add_ref,  release,
    gdi_from_logfont,       /* CreateFontFromLOGFONT */
    gdi_font_logfont,       /* ConvertFontToLOGFONT */
    gdi_face_logfont,       /* ConvertFontFaceToLOGFONT */
    none3,                  /* CreateFontFaceFromHdc */
    none5,                  /* CreateBitmapRenderTarget */
    trap_gdi_8, trap_gdi_9, trap_gdi_10, trap_gdi_11,
};

static uint32_t WINAPI gdi_interop(void *self, void **out)
{
    UNUSED(self);
    return give(out, com_new(gdi_vtbl, iid_gdi_interop, NULL, 0));
}

/* IDWriteFactory */

static uint32_t WINAPI system_collection(void *self, void **out, BOOL check)
{
    UNUSED(self);
    UNUSED(check);
    scan_fonts();
    return give(out, new_collection());
}

static uint32_t WINAPI font_file_reference(void *self, const WCHAR *path, const void *time, void **out)
{
    UNUSED(self);
    UNUSED(time);
    char *p = w32_narrow(path);
    char unix_path[PATH_MAX_LEN];
    int found = -1;
    if (p && w32_path(p, unix_path, sizeof(unix_path)) == 0) {
        scan_fonts();
        for (int i = 0; i < face_count && found < 0; i++)
            if (!str_casecmp(faces[i].path, unix_path))
                found = i;
    }
    w32_free(p);
    if (found < 0) {
        *out = NULL;
        return 0x80070002u;
    }
    return give(out, com_new(file_vtbl, iid_file, NULL, found));
}

static uint32_t WINAPI create_font_face(void *self, int type, UINT count, struct obj **files, UINT index, int sim,
                                        void **out)
{
    UNUSED(self);
    UNUSED(type);
    UNUSED(index);
    UNUSED(sim);
    if (!count || !files || !files[0] || files[0]->vtbl != file_vtbl) {
        *out = NULL;
        return E_INVALIDARG;
    }
    return give(out, com_new(face_vtbl, iid_face, NULL, files[0]->index));
}

/* Faces handed over as streams by the program (Skia's in-memory fonts) join the table once. */
static int memory_face(const uint8_t *data, uint32_t size)
{
    for (int i = 0; i < face_count; i++)
        if (faces[i].size == size && !memcmp(faces[i].data, data, size))
            return i;
    if (face_count >= MAX_FACES)
        return -1;
    uint8_t *copy = kmalloc(size);
    if (!copy)
        return -1;
    memcpy(copy, data, size);
    struct face *f = &faces[face_count];
    memset(f, 0, sizeof(*f));
    f->data = copy;
    f->size = size;
    if (!parse_face(f)) {
        kfree(copy);
        return -1;
    }
    f->family_index = -1;
    for (int k = 0; k < family_count; k++)
        if (!str_casecmp(faces[family_first[k]].family, f->family))
            f->family_index = k;
    if (f->family_index < 0) {
        f->family_index = family_count;
        family_first[family_count++] = face_count;
    }
    return face_count++;
}

typedef uint32_t(WINAPI *stream_from_key_fn)(void *, const void *, UINT, void **);
typedef uint32_t(WINAPI *read_fragment_fn)(void *, const void **, uint64_t, uint64_t, void **);
typedef void(WINAPI *release_fragment_fn)(void *, void *);
typedef uint32_t(WINAPI *file_size_fn)(void *, uint64_t *);
typedef uint32_t(WINAPI *com_release_fn)(void *);

static uint32_t WINAPI custom_file_reference(void *self, const void *key, UINT size, void *loader, void **out)
{
    UNUSED(self);
    *out = NULL;
    if (!loader)
        return E_INVALIDARG;
    if (loader == &local_loader) {
        int index = key_face(key, size);
        return index < 0 ? E_INVALIDARG : give(out, com_new(file_vtbl, iid_file, NULL, index));
    }
    void **lv = *(void ***)loader, *stream = NULL;
    uint32_t hr = ((stream_from_key_fn)lv[3])(loader, key, size, &stream);
    if (hr || !stream)
        return hr ? hr : E_INVALIDARG;
    void **sv = *(void ***)stream;
    uint64_t bytes = 0;
    const void *start = NULL;
    void *context = NULL;
    int index = -1;
    if (!((file_size_fn)sv[5])(stream, &bytes) && bytes && bytes < 64u * 1024 * 1024 &&
        !((read_fragment_fn)sv[3])(stream, &start, 0, bytes, &context) && start) {
        index = memory_face(start, (uint32_t)bytes);
        ((release_fragment_fn)sv[4])(stream, context);
    }
    ((com_release_fn)sv[2])(stream);
    return index < 0 ? 0x88985002u : give(out, com_new(file_vtbl, iid_file, NULL, index));
}

/* IDWriteGlyphRunAnalysis: extra = struct analysis */

struct analysis {
    int32_t rect[4];
    uint8_t *bits;
};

static uint32_t WINAPI analysis_release(struct obj *self)
{
    struct analysis *a = self->extra;
    if (__atomic_load_n(&self->refs, __ATOMIC_RELAXED) == 1 && a)
        w32_free(a->bits);
    return release(self);
}

static uint32_t WINAPI analysis_bounds(struct obj *self, int type, int32_t *rect)
{
    UNUSED(type);
    memcpy(rect, ((struct analysis *)self->extra)->rect, 4 * sizeof(int32_t));
    return S_OK;
}

static uint32_t WINAPI analysis_texture(struct obj *self, int type, const int32_t *want, uint8_t *buf, UINT size)
{

    const struct analysis *a = self->extra;
    int w = want[2] - want[0], h = want[3] - want[1], bpp = type ? 3 : 1;
    if (w < 0 || h < 0 || (UINT)(w * h * bpp) > size)
        return 0x8007007au;
    int aw = a->rect[2] - a->rect[0];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = want[0] + x - a->rect[0], sy = want[1] + y - a->rect[1];
            uint8_t v = 0;
            if (a->bits && sx >= 0 && sy >= 0 && sx < aw && sy < a->rect[3] - a->rect[1])
                v = a->bits[sy * aw + sx];
            if (!type)
                v = v >= 128 ? 255 : 0;
            for (int k = 0; k < bpp; k++)
                buf[(y * w + x) * bpp + k] = v;
        }
    return S_OK;
}

static uint32_t WINAPI analysis_blend(struct obj *self, void *params, uint32_t *gamma, uint32_t *contrast,
                                      uint32_t *level)
{
    UNUSED(self);
    UNUSED(params);
    *gamma = 0x3fe66666u;
    *contrast = 0;
    *level = 0;
    return S_OK;
}

static const void *const analysis_vtbl[] = {
    query_interface, add_ref, analysis_release, analysis_bounds, analysis_texture, analysis_blend,
};

struct glyph_run {
    struct obj *face;
    uint32_t em;
    UINT count;
    const uint16_t *glyphs;
    const uint32_t *advances, *offsets;
    BOOL sideways;
    UINT bidi;
};

static uint32_t WINAPI glyph_run_analysis(void *self, const struct glyph_run *run, uint32_t ppd,
                                          const uint32_t *matrix, int mode, int measuring, uint32_t x, uint32_t y,
                                          void **out)
{
    UNUSED(self);
    UNUSED(mode);
    UNUSED(measuring);
    *out = NULL;
    if (!run || !run->face || run->face->vtbl != face_vtbl)
        return E_INVALIDARG;
    struct obj *o = com_new(analysis_vtbl, iid_analysis, NULL, 0);
    struct analysis *a = w32_zalloc(sizeof(*a));
    if (!o || !a) {
        w32_free(o);
        w32_free(a);
        return E_OUTOFMEMORY;
    }
    o->extra = a;
    uint32_t hr = dwrite_raster(run->face->index, run->em, ppd, matrix, x, y, run->glyphs, run->advances,
                                run->offsets, run->count, a->rect, &a->bits);
    if (hr) {
        analysis_release(o);
        return hr;
    }
    return give(out, o);
}

typedef uint32_t(WINAPI *enum_from_key_fn)(void *, void *, const void *, UINT, void **);
typedef uint32_t(WINAPI *move_next_fn)(void *, BOOL *);
typedef uint32_t(WINAPI *current_file_fn)(void *, void **);
typedef uint32_t(WINAPI *file_key_fn)(void *, const void **, UINT *);
typedef uint32_t(WINAPI *file_loader_fn)(void *, void **);

static int file_face(struct obj *file)
{
    if (file->vtbl == file_vtbl)
        return file->index;
    void **fv = *(void ***)file, *loader = NULL, *ours = NULL;
    const void *key = NULL;
    UINT size = 0;
    if (((file_key_fn)fv[3])(file, &key, &size) || ((file_loader_fn)fv[4])(file, &loader) || !loader)
        return -1;
    int index = -1;
    if (!custom_file_reference(NULL, key, size, loader, &ours) && ours) {
        index = ((struct obj *)ours)->index;
        release(ours);
    }
    ((com_release_fn)(*(void ***)loader)[2])(loader);
    return index;
}

static uint32_t WINAPI custom_collection(void *self, void *loader, const void *key, UINT size, void **out)
{
    *out = NULL;
    if (!loader)
        return E_INVALIDARG;
    void *files = NULL;
    uint32_t hr = ((enum_from_key_fn)(*(void ***)loader)[3])(loader, self, key, size, &files);
    if (hr || !files)
        return hr ? hr : E_INVALIDARG;
    struct obj *o = new_collection();
    struct families *c = w32_zalloc(sizeof(*c));
    if (!o || !c) {
        w32_free(o);
        w32_free(c);
        ((com_release_fn)(*(void ***)files)[2])(files);
        return E_OUTOFMEMORY;
    }
    o->extra = c;
    void **ev = *(void ***)files;
    BOOL more = FALSE;
    while (!((move_next_fn)ev[3])(files, &more) && more) {
        void *file = NULL;
        if (((current_file_fn)ev[4])(files, &file) || !file)
            continue;
        int face = file_face(file);
        ((com_release_fn)(*(void ***)file)[2])(file);
        if (face < 0)
            continue;
        int family = faces[face].family_index, k = 0;
        while (k < c->count && c->family[k] != family)
            k++;
        if (k == c->count && c->count < MAX_FACES)
            c->family[c->count++] = family;
    }
    ((com_release_fn)ev[2])(files);
    return give(out, o);
}

TRAPS(factory, 24, 25, 26, 27)

static const void *const factory_vtbl[] = {
    query_interface, add_ref,     release,
    system_collection,      /* GetSystemFontCollection */
    custom_collection,      /* CreateCustomFontCollection */
    ok2,                    /* RegisterFontCollectionLoader */
    ok2,                    /* UnregisterFontCollectionLoader */
    font_file_reference,    /* CreateFontFileReference */
    custom_file_reference,  /* CreateCustomFontFileReference */
    create_font_face,       /* CreateFontFace */
    none2,                  /* CreateRenderingParams */
    none3,                  /* CreateMonitorRenderingParams */
    none7,                  /* CreateCustomRenderingParams */
    ok2,                    /* RegisterFontFileLoader */
    ok2,                    /* UnregisterFontFileLoader */
    none9,                  /* CreateTextFormat */
    none2,                  /* CreateTypography */
    gdi_interop,            /* GetGdiInterop */
    none7,                  /* CreateTextLayout */
    none10,                 /* CreateGdiCompatibleTextLayout */
    none3,                  /* CreateEllipsisTrimmingSign */
    none2,                  /* CreateTextAnalyzer */
    none5,                  /* CreateNumberSubstitution */
    glyph_run_analysis,     /* CreateGlyphRunAnalysis */
    trap_factory_24, trap_factory_25, trap_factory_26, trap_factory_27,
};

static uint32_t WINAPI DWriteCreateFactory(int type, const uint8_t *iid, void **out)
{
    UNUSED(type);
    scan_fonts();
    struct obj *f = com_new(factory_vtbl, iid_factory, NULL, 0);
    if (!f)
        return E_OUTOFMEMORY;
    uint32_t hr = query_interface(f, iid, out);
    release(f);
    return hr;
}

static const struct w32_export dwrite_exports[] = {
    W32_FN(DWriteCreateFactory),
};

W32_EXPORTS(dwrite_module, "dwrite.dll", dwrite_exports);

/* 64-bit programs get a proxy per object: its first quadword is a vtable of 64-bit stubs that translate the
   arguments for the methods above (one letter each: i integer, p pointer, f float, q 64-bit integer, o/O returned
   object/pointer, N our object, L/R the program's collection/file loader, A array of files, F returned files,
   g glyph run). Objects of the program get 32-bit wrappers that call them back in 64-bit mode. */

struct method64 {
    const void *fn;
    const char *spec;
};

struct proxy {
    uint64_t vtbl;
    uint64_t inner;
};

#define QI_AR_RL {query_interface, "po"}, {add_ref, ""}, {release, ""}
#define TRAPS64 {NULL, NULL}, {NULL, NULL}, {NULL, NULL}, {NULL, NULL}

static const struct method64 strings_m64[] = {
    QI_AR_RL, {strings_count, ""}, {strings_find, "ppp"}, {strings_locale_length, "ip"}, {strings_locale, "ipi"},
    {strings_length, "ip"}, {strings_get, "ipi"}, TRAPS64,
};

static const struct method64 fstream_m64[] = {
    QI_AR_RL, {fstream_read, "OqqO"}, {fstream_release_fragment, "i"}, {fstream_size, "p"}, {fstream_time, "p"},
    TRAPS64,
};

static const struct method64 loader_m64[] = {
    QI_AR_RL, {loader_stream, "pio"}, {loader_path_length, "pip"}, {loader_path, "pipi"}, {loader_time, "pip"},
    TRAPS64,
};

static const struct method64 file_m64[] = {
    QI_AR_RL, {file_key, "Op"}, {file_loader, "o"}, {file_analyze, "pppp"}, TRAPS64,
};

static const struct method64 face_m64[] = {
    QI_AR_RL, {face_type, ""}, {face_files, "pF"}, {zero1, ""}, {zero1, ""}, {zero1, ""}, {face_metrics, "p"},
    {face_glyph_count, ""}, {face_design_metrics, "pipi"}, {face_indices, "pip"}, {face_table, "iOpOp"},
    {face_release_table, "i"}, {face_outline, NULL}, {face_rendering_mode, "ffiip"}, {face_gdi_metrics, "ffpp"},
    {face_gdi_glyph_metrics, "ffpipipi"}, TRAPS64,
};

static const struct method64 font_m64[] = {
    QI_AR_RL, {font_family, "o"}, {font_weight, ""}, {font_stretch, ""}, {font_style, ""}, {font_symbol, ""},
    {font_face_names, "o"}, {font_info_strings, "iop"}, {zero1, ""}, {font_metrics, "p"}, {font_has_char, "ip"},
    {font_create_face, "o"}, TRAPS64,
};

static const struct method64 family_m64[] = {
    QI_AR_RL, {family_collection, "o"}, {family_font_count, ""}, {family_font, "io"}, {family_names, "o"},
    {family_match, "iiio"}, {family_matching_list, "iiio"}, TRAPS64,
};

static const struct method64 collection_m64[] = {
    QI_AR_RL, {collection_count, ""}, {collection_family, "io"}, {collection_find, "ppp"},
    {collection_from_face, "No"}, TRAPS64,
};

static const struct method64 factory_m64[] = {
    QI_AR_RL,
    {system_collection, "oi"},
    {custom_collection, "Lpio"},
    {ok2, "i"},
    {ok2, "i"},
    {font_file_reference, "ppo"},
    {custom_file_reference, "piRo"},
    {create_font_face, "iiAiio"},
    {none2, "o"},
    {none3, "io"},
    {none7, "iiiiio"},
    {ok2, "i"},
    {ok2, "i"},
    {none9, "iiiiiiio"},
    {none2, "o"},
    {gdi_interop, "o"},
    {none7, "iiiiio"},
    {none10, "iiiiiiiio"},
    {none3, "io"},
    {none2, "o"},
    {none5, "iiio"},
    {glyph_run_analysis, "gfpiiffo"},
    TRAPS64,
};

static const struct method64 gdi_m64[] = {
    QI_AR_RL, {gdi_from_logfont, "po"}, {gdi_font_logfont, "Npp"}, {gdi_face_logfont, "Np"}, {none3, "io"},
    {none5, "iiio"}, TRAPS64,
};

static const struct method64 analysis_m64[] = {
    {query_interface, "po"}, {add_ref, ""}, {analysis_release, ""}, {analysis_bounds, "ip"},
    {analysis_texture, "ippi"}, {analysis_blend, "ippp"}, TRAPS64,
};

static const struct {
    const void *const *vtbl;
    const struct method64 *methods;
    int count;
    const char *name;
} kinds64[] = {
    {strings_vtbl, strings_m64, ARRAY_SIZE(strings_m64), "strings"},
    {fstream_vtbl, fstream_m64, ARRAY_SIZE(fstream_m64), "stream"},
    {loader_vtbl, loader_m64, ARRAY_SIZE(loader_m64), "loader"},
    {file_vtbl, file_m64, ARRAY_SIZE(file_m64), "file"},
    {face_vtbl, face_m64, ARRAY_SIZE(face_m64), "face"},
    {font_vtbl, font_m64, ARRAY_SIZE(font_m64), "font"},
    {family_vtbl, family_m64, ARRAY_SIZE(family_m64), "family"},
    {collection_vtbl, collection_m64, ARRAY_SIZE(collection_m64), "collection"},
    {factory_vtbl, factory_m64, ARRAY_SIZE(factory_m64), "factory"},
    {analysis_vtbl, analysis_m64, ARRAY_SIZE(analysis_m64), "analysis"},
    {gdi_vtbl, gdi_m64, ARRAY_SIZE(gdi_m64), "gdi interop"},
};

static W32_STATE uint64_t *vt64[ARRAY_SIZE(kinds64)];
static W32_STATE struct proxy *local_proxy;

static uint64_t h_method(uint64_t *a, const uint64_t *f, void *t);

static uint64_t vtable64(int kind)
{
    if (!vt64[kind] && (vt64[kind] = w32_alloc(sizeof(uint64_t) * (size_t)kinds64[kind].count)))
        for (int i = 0; i < kinds64[kind].count; i++)
            vt64[kind][i] = (uintptr_t)w64_target_stub(h_method, (void *)&kinds64[kind].methods[i]);
    return (uintptr_t)vt64[kind];
}

static uint64_t wrap64(struct obj *o)
{
    if (!o)
        return 0;
    struct proxy **slot = o == &local_loader ? &local_proxy : (struct proxy **)&o->proxy;
    if (!*slot) {
        int kind = 0;
        while (kind < (int)ARRAY_SIZE(kinds64) - 1 && kinds64[kind].vtbl != o->vtbl)
            kind++;
        uint64_t vtbl = vtable64(kind);
        if (!vtbl || !(*slot = w32_alloc(sizeof(struct proxy))))
            return 0;
        **slot = (struct proxy){vtbl, (uintptr_t)o};
    }
    return (uintptr_t)*slot;
}

static struct obj *unwrap64(uint64_t v)
{
    const struct proxy *p = v && !(v >> 32) ? (const struct proxy *)(uintptr_t)v : NULL;
    for (size_t k = 0; p && k < ARRAY_SIZE(kinds64); k++)
        if (vt64[k] && p->vtbl == (uintptr_t)vt64[k])
            return (struct obj *)(uintptr_t)p->inner;
    return NULL;
}

/* the program's own objects, called through 32-bit vtables */
struct rev {
    const void *const *vtbl;
    uint64_t target, context;
};

static uint64_t rev_call(struct rev *r, int slot, int argc, uint64_t *args)
{
    args[0] = r->target;
    uint64_t vtbl;
    memcpy(&vtbl, w64_ptr(r->target), 8);
    uint64_t fn;
    memcpy(&fn, (uint8_t *)w64_ptr(vtbl) + slot * 8, 8);
    return w64_call(fn, argc, args);
}

static void *rev_new(uint64_t target, const void *const *vtbl);

static uint32_t WINAPI rev_query(struct rev *r, const uint8_t *iid, void **out)
{
    UNUSED(r);
    UNUSED(iid);
    *out = NULL;
    return E_NOINTERFACE;
}

static uint32_t WINAPI rev_add_ref(struct rev *r)
{
    uint64_t args[1];
    return (uint32_t)rev_call(r, 1, 1, args);
}

static uint32_t WINAPI rev_release(struct rev *r)
{
    uint64_t args[1];
    uint32_t n = (uint32_t)rev_call(r, 2, 1, args);
    w32_free(r);
    return n;
}

static const void *const rev_file_vtbl[];
static const void *const rev_file_loader_vtbl[];
static const void *const rev_stream_vtbl[];
static const void *const rev_enum_vtbl[];

static void *ours_or_rev(uint64_t v, const void *const *vtbl)
{
    struct obj *o = unwrap64(v);
    return o ? (void *)o : v ? rev_new(v, vtbl) : NULL;
}

static uint32_t WINAPI rev_enum_from_key(struct rev *r, struct obj *factory, const void *key, UINT size, void **out)
{
    uint64_t made = 0, args[5] = {0, wrap64(factory), (uintptr_t)key, size, (uintptr_t)&made};
    uint32_t hr = (uint32_t)rev_call(r, 3, 5, args);
    *out = made ? rev_new(made, rev_enum_vtbl) : NULL;
    return hr;
}

static uint32_t WINAPI rev_move_next(struct rev *r, BOOL *more)
{
    uint64_t args[2] = {0, (uintptr_t)more};
    return (uint32_t)rev_call(r, 3, 2, args);
}

static uint32_t WINAPI rev_current_file(struct rev *r, void **out)
{
    uint64_t file = 0, args[2] = {0, (uintptr_t)&file};
    uint32_t hr = (uint32_t)rev_call(r, 4, 2, args);
    *out = ours_or_rev(file, rev_file_vtbl);
    return hr;
}

static uint32_t WINAPI rev_file_key(struct rev *r, const void **key, UINT *size)
{
    uint64_t k = 0, args[3] = {0, (uintptr_t)&k, (uintptr_t)size};
    uint32_t hr = (uint32_t)rev_call(r, 3, 3, args);
    *key = w64_ptr(k);
    return hr;
}

static uint32_t WINAPI rev_file_loader(struct rev *r, void **out)
{
    uint64_t loader = 0, args[2] = {0, (uintptr_t)&loader};
    uint32_t hr = (uint32_t)rev_call(r, 4, 2, args);
    *out = ours_or_rev(loader, rev_file_loader_vtbl);
    return hr;
}

static uint32_t WINAPI rev_stream_from_key(struct rev *r, const void *key, UINT size, void **out)
{
    uint64_t stream = 0, args[4] = {0, (uintptr_t)key, size, (uintptr_t)&stream};
    uint32_t hr = (uint32_t)rev_call(r, 3, 4, args);
    *out = ours_or_rev(stream, rev_stream_vtbl);
    return hr;
}

static uint32_t WINAPI rev_read(struct rev *r, const void **start, uint64_t offset, uint64_t size, void **context)
{
    uint64_t at = 0, args[5] = {0, (uintptr_t)&at, offset, size, (uintptr_t)&r->context};
    uint32_t hr = (uint32_t)rev_call(r, 3, 5, args);
    *start = w64_ptr(at);
    *context = r;
    return hr;
}

static void WINAPI rev_release_fragment(struct rev *r, void *context)
{
    UNUSED(context);
    uint64_t args[2] = {0, r->context};
    rev_call(r, 4, 2, args);
}

static uint32_t WINAPI rev_size(struct rev *r, uint64_t *size)
{
    uint64_t args[2] = {0, (uintptr_t)size};
    return (uint32_t)rev_call(r, 5, 2, args);
}

static const void *const rev_loader_vtbl[] = {rev_query, rev_add_ref, rev_release, rev_enum_from_key};
static const void *const rev_enum_vtbl[] = {rev_query, rev_add_ref, rev_release, rev_move_next, rev_current_file};
static const void *const rev_file_vtbl[] = {rev_query, rev_add_ref, rev_release, rev_file_key, rev_file_loader};
static const void *const rev_file_loader_vtbl[] = {rev_query, rev_add_ref, rev_release, rev_stream_from_key};
static const void *const rev_stream_vtbl[] = {rev_query, rev_add_ref, rev_release, rev_read, rev_release_fragment,
                                              rev_size};

static void *rev_new(uint64_t target, const void *const *vtbl)
{
    struct rev *r = w32_zalloc(sizeof(*r));
    if (r)
        *r = (struct rev){vtbl, target, 0};
    return r;
}

static uint64_t h_method(uint64_t *a, const uint64_t *f, void *t)
{
    const struct method64 *m = t;
    const struct proxy *self = w64_ptr(a[0]);
    for (size_t k = 0; !m->fn && k < ARRAY_SIZE(kinds64); k++)
        if (m >= kinds64[k].methods && m < kinds64[k].methods + kinds64[k].count)
            missing_method(kinds64[k].name, (int)(m - kinds64[k].methods));
    if (!m->spec)
        return E_NOTIMPL;
    uint32_t args[20], out[12] = {0}, files[8] = {0};
    void *made[4] = {NULL};
    struct glyph_run run;
    int n = 0, nmade = 0;
    args[n++] = (uint32_t)self->inner;
    for (int i = 1; m->spec[i - 1] && i < 12; i++) {
        uint8_t *p = a[i] ? w64_ptr(a[i]) : NULL;
        switch (m->spec[i - 1]) {
        case 'i':
            args[n++] = (uint32_t)a[i];
            break;
        case 'f':
            args[n++] = (uint32_t)(i < 4 ? f[i] : a[i]);
            break;
        case 'q':
            args[n++] = (uint32_t)a[i];
            args[n++] = (uint32_t)(a[i] >> 32);
            break;
        case 'o':
        case 'O':
            args[n++] = p ? (uint32_t)(uintptr_t)&out[i] : 0;
            break;
        case 'F':
            args[n++] = p ? (uint32_t)(uintptr_t)files : 0;
            break;
        case 'N':
            args[n++] = (uint32_t)(uintptr_t)unwrap64(a[i]);
            break;
        case 'L':
        case 'R': {
            struct obj *o = unwrap64(a[i]);
            const void *const *vtbl = m->spec[i - 1] == 'L' ? rev_loader_vtbl : rev_file_loader_vtbl;
            void *r = o || !a[i] ? NULL : rev_new(a[i], vtbl);
            if (r)
                made[nmade++] = r;
            args[n++] = (uint32_t)(uintptr_t)(o ? (void *)o : r);
            break;
        }
        case 'A': {
            uint32_t count = MIN((uint32_t)a[i - 1], 8u);
            for (uint32_t k = 0; p && k < count; k++) {
                uint64_t v;
                memcpy(&v, p + k * 8, 8);
                files[k] = (uint32_t)(uintptr_t)ours_or_rev(v, rev_file_vtbl);
                if (files[k] && !unwrap64(v) && nmade < 4)
                    made[nmade++] = (void *)(uintptr_t)files[k];
            }
            args[n++] = p ? (uint32_t)(uintptr_t)files : 0;
            break;
        }
        case 'g': {
            uint64_t face, glyphs, advances, offsets;
            memcpy(&face, p, 8);
            memcpy(&glyphs, p + 16, 8);
            memcpy(&advances, p + 24, 8);
            memcpy(&offsets, p + 32, 8);
            run = (struct glyph_run){unwrap64(face), 0, 0, w64_ptr(glyphs), w64_ptr(advances), w64_ptr(offsets),
                                     0, 0};
            memcpy(&run.em, p + 8, 4);
            memcpy(&run.count, p + 12, 4);
            memcpy(&run.sideways, p + 40, 4);
            memcpy(&run.bidi, p + 44, 4);
            args[n++] = (uint32_t)(uintptr_t)&run;
            break;
        }
        default:
            args[n++] = (uint32_t)(uintptr_t)p;
        }
    }
    uint32_t r = (uint32_t)w64_call32((void *)m->fn, args, n);
    for (int i = 1; m->spec[i - 1] && i < 12; i++) {
        uint8_t *p = a[i] ? w64_ptr(a[i]) : NULL;
        uint64_t v;
        if (!p)
            continue;
        if (m->spec[i - 1] == 'o' || m->spec[i - 1] == 'O') {
            v = m->spec[i - 1] == 'o' ? wrap64((struct obj *)(uintptr_t)out[i]) : out[i];
            memcpy(p, &v, 8);
        } else if (m->spec[i - 1] == 'F') {
            uint32_t count = MIN(*(const UINT *)w64_ptr(a[i - 1]), 8u);
            for (uint32_t k = 0; k < count; k++) {
                v = wrap64((struct obj *)(uintptr_t)files[k]);
                memcpy(p + k * 8, &v, 8);
            }
        }
    }
    for (int k = 0; k < nmade; k++)
        w32_free(made[k]);
    return r;
}

static uint64_t h_create_factory(uint64_t *a, const uint64_t *f, void *t)
{
    UNUSED(f);
    UNUSED(t);
    uint32_t made = 0;
    uint32_t hr = DWriteCreateFactory((int)a[0], w64_ptr(a[1]), (void **)&made);
    uint64_t v = wrap64((struct obj *)(uintptr_t)made);
    if (a[2])
        memcpy(w64_ptr(a[2]), &v, 8);
    return hr;
}

static const struct w64_export dwrite64_exports[] = {
    W64_FN("DWriteCreateFactory", h_create_factory),
};

W64_EXPORTS(dwrite64_module, "dwrite.dll", dwrite64_exports);
