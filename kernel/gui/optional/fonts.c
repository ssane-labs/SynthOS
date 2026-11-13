#include <synth/command.h>
#include <synth/error.h>
#include <synth/module.h>
#include <synth/path.h>
#include <synth/users.h>
#include <synth/vfs.h>
#include <synth/winpath.h>

#include "systool.h"

/* Fonts: TrueType and OpenType files (.ttf, .otf, .ttc) - installed for everyone into C:\Windows\Fonts (an
   administrator) or for one person into their AppData, previewed and removed. The outlines are drawn here: TrueType
   quadratic contours (simple and composite glyphs) and OpenType CFF Type 2 charstrings (cubic, with subroutines,
   flex and CID fonts), flattened to lines and filled with exact horizontal coverage on four sub-scanlines. With
   ClearType smoothing on, the glyph is drawn at three times the width and each colour channel takes its own third
   of the pixel through a five-tap filter, as LCD subpixel rendering does. Integer arithmetic only (26.6 pixels). */

#define SYSTEM_FONTS "C:\\Windows\\Fonts"
#define USER_FONTS "C:\\Users\\%s\\AppData\\Local\\Microsoft\\Windows\\Fonts"
#define MAX_FONTS 96
#define MAX_SEGS 6000

/* ---- the font file ---- */

struct font {
    const uint8_t *d;
    size_t size;
    uint32_t cmap, glyf, loca, hmtx, cff;
    int upem, ascent, descent, nglyphs, nhmetrics;
    bool long_loca;
    /* CFF */
    uint32_t charstrings, gsubrs, subrs, fdarray, fdselect;
    int cff_bias_g, cff_bias_l;
    bool cid;
    char family[64], style[48];
};

static uint32_t be16(const uint8_t *p)
{
    return (uint32_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool in(const struct font *f, uint32_t off, uint32_t n)
{
    return off <= f->size && n <= f->size - off;
}

static uint32_t table(const struct font *f, uint32_t base, const char *tag)
{
    if (!in(f, base, 12))
        return 0;
    uint32_t n = be16(f->d + base + 4);
    for (uint32_t i = 0; i < n && in(f, base + 12 + 16 * i, 16); i++) {
        const uint8_t *e = f->d + base + 12 + 16 * i;
        if (!memcmp(e, tag, 4) && in(f, be32(e + 8), be32(e + 12)))
            return be32(e + 8);
    }
    return 0;
}

/* ---- CFF: INDEX and DICT ---- */

/* the offset and length of item i of the INDEX at *at; *at moves past the INDEX when next is set */
static bool cff_item(const struct font *f, uint32_t at, uint32_t i, uint32_t *off, uint32_t *len)
{
    if (!in(f, at, 3))
        return false;
    uint32_t count = be16(f->d + at), osz = f->d[at + 2];
    if (i >= count || osz < 1 || osz > 4 || !in(f, at + 3, (count + 1) * osz))
        return false;
    const uint8_t *o = f->d + at + 3;
    uint32_t a = 0, b = 0;
    for (uint32_t k = 0; k < osz; k++) {
        a = a << 8 | o[i * osz + k];
        b = b << 8 | o[(i + 1) * osz + k];
    }
    uint32_t data = at + 3 + (count + 1) * osz - 1;
    if (b < a || !in(f, data + a, b - a))
        return false;
    *off = data + a;
    *len = b - a;
    return true;
}

static uint32_t cff_count(const struct font *f, uint32_t at)
{
    return in(f, at, 2) ? be16(f->d + at) : 0;
}

static uint32_t cff_skip(const struct font *f, uint32_t at)
{
    uint32_t count = cff_count(f, at);
    if (!count)
        return at + 2;
    uint32_t off, len;
    return cff_item(f, at, count - 1, &off, &len) ? off + len : (uint32_t)f->size;
}

/* operands before operator op (12 x is 1200 + x) in a DICT */
static int dict_find(const struct font *f, uint32_t at, uint32_t len, int op, int32_t *vals, int max)
{
    int32_t stack[48];
    int n = 0;
    for (uint32_t p = at; p < at + len && in(f, p, 1);) {
        uint8_t b = f->d[p];
        if (b <= 21) {
            int o = b;
            p++;
            if (b == 12 && in(f, p, 1))
                o = 1200 + f->d[p++];
            if (o == op) {
                for (int i = 0; i < n && i < max; i++)
                    vals[i] = stack[i];
                return n;
            }
            n = 0;
            continue;
        }
        int32_t v = 0;
        if (b == 28 && in(f, p, 3))
            v = (int16_t)be16(f->d + p + 1), p += 3;
        else if (b == 29 && in(f, p, 5))
            v = (int32_t)be32(f->d + p + 1), p += 5;
        else if (b == 30) { /* a real number: skipped */
            p++;
            while (in(f, p, 1) && (f->d[p] & 0x0f) != 0x0f && (f->d[p] >> 4) != 0x0f)
                p++;
            p++;
        } else if (b >= 32 && b <= 246)
            v = b - 139, p++;
        else if (b >= 247 && b <= 250 && in(f, p, 2))
            v = (b - 247) * 256 + f->d[p + 1] + 108, p += 2;
        else if (b >= 251 && b <= 254 && in(f, p, 2))
            v = -(b - 251) * 256 - f->d[p + 1] - 108, p += 2;
        else
            p++;
        if (n < 48)
            stack[n++] = v;
    }
    return -1;
}

static int bias(uint32_t count)
{
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

static bool cff_open(struct font *f, uint32_t cff)
{
    if (!in(f, cff, 4))
        return false;
    f->cff = cff;
    uint32_t names = cff + f->d[cff + 2], top = cff_skip(f, names), strings = cff_skip(f, top);
    f->gsubrs = cff_skip(f, strings);
    f->cff_bias_g = bias(cff_count(f, f->gsubrs));
    uint32_t off, len;
    if (!cff_item(f, top, 0, &off, &len))
        return false;
    int32_t v[4];
    if (dict_find(f, off, len, 17, v, 1) != 1)
        return false;
    f->charstrings = cff + (uint32_t)v[0];
    f->nglyphs = (int)cff_count(f, f->charstrings);
    if (dict_find(f, off, len, 1236, v, 1) == 1 && dict_find(f, off, len, 1237, v + 1, 1) == 1) {
        f->cid = true;
        f->fdarray = cff + (uint32_t)v[0];
        f->fdselect = cff + (uint32_t)v[1];
    } else if (dict_find(f, off, len, 18, v, 2) == 2) {
        uint32_t priv = cff + (uint32_t)v[1];
        int32_t s;
        if (dict_find(f, priv, (uint32_t)v[0], 19, &s, 1) == 1) {
            f->subrs = priv + (uint32_t)s;
            f->cff_bias_l = bias(cff_count(f, f->subrs));
        }
    }
    return true;
}

/* a CID font: the local subroutines of the font dict the glyph belongs to */
static void cid_subrs(const struct font *f, int gid, uint32_t *subrs, int *b)
{
    *subrs = 0;
    *b = 0;
    int fd = 0;
    uint32_t s = f->fdselect;
    if (!in(f, s, 1))
        return;
    if (f->d[s] == 0 && in(f, s + 1 + (uint32_t)gid, 1)) {
        fd = f->d[s + 1 + gid];
    } else if (f->d[s] == 3 && in(f, s + 1, 2)) {
        uint32_t n = be16(f->d + s + 1);
        for (uint32_t i = 0; i < n && in(f, s + 3 + i * 3, 5); i++) {
            uint32_t first = be16(f->d + s + 3 + i * 3), next = be16(f->d + s + 6 + i * 3);
            if ((uint32_t)gid >= first && (uint32_t)gid < next) {
                fd = f->d[s + 5 + i * 3];
                break;
            }
        }
    }
    uint32_t off, len;
    int32_t v[2], sub;
    if (!cff_item(f, f->fdarray, (uint32_t)fd, &off, &len) || dict_find(f, off, len, 18, v, 2) != 2)
        return;
    uint32_t priv = f->cff + (uint32_t)v[1];
    if (dict_find(f, priv, (uint32_t)v[0], 19, &sub, 1) == 1) {
        *subrs = priv + (uint32_t)sub;
        *b = bias(cff_count(f, *subrs));
    }
}

static void name_of(struct font *f, uint32_t name)
{
    if (!in(f, name, 6))
        return;
    uint32_t count = be16(f->d + name + 2), strings = name + be16(f->d + name + 4);
    for (int want = 0; want < 2; want++) {
        int id = want ? 2 : 1; /* family, then style */
        char *out = want ? f->style : f->family;
        size_t cap = want ? sizeof(f->style) : sizeof(f->family);
        int best = 0;
        for (uint32_t i = 0; i < count && in(f, name + 6 + i * 12, 12); i++) {
            const uint8_t *r = f->d + name + 6 + i * 12;
            uint32_t platform = be16(r), lang = be16(r + 4), nid = be16(r + 6), len = be16(r + 8);
            uint32_t at = strings + be16(r + 10);
            if ((int)nid != id || !in(f, at, len))
                continue;
            int score = platform == 3 ? (lang == 0x409 ? 3 : 2) : platform == 1 ? 1 : 0;
            if (score <= best)
                continue;
            best = score;
            size_t o = 0;
            if (platform == 3 || platform == 0) {
                for (uint32_t k = 0; k + 1 < len && o + 1 < cap; k += 2) {
                    uint32_t c = be16(f->d + at + k);
                    out[o++] = c < 0x80 ? (char)c : '?';
                }
            } else {
                for (uint32_t k = 0; k < len && o + 1 < cap; k++)
                    out[o++] = f->d[at + k] < 0x80 ? (char)f->d[at + k] : '?';
            }
            out[o] = 0;
        }
    }
}

static bool font_open(struct font *f, const uint8_t *d, size_t size)
{
    memset(f, 0, sizeof(*f));
    f->d = d;
    f->size = size;
    uint32_t base = 0;
    if (size >= 16 && !memcmp(d, "ttcf", 4)) /* a collection: its first font */
        base = be32(d + 12);
    if (!in(f, base, 12))
        return false;
    uint32_t head = table(f, base, "head"), hhea = table(f, base, "hhea"), maxp = table(f, base, "maxp");
    if (!head || !hhea || !in(f, head, 54) || !in(f, hhea, 36))
        return false;
    f->upem = (int)be16(d + head + 18);
    f->long_loca = be16(d + head + 50) != 0;
    f->ascent = (int16_t)be16(d + hhea + 4);
    f->descent = (int16_t)be16(d + hhea + 6);
    f->nhmetrics = (int)be16(d + hhea + 34);
    f->nglyphs = maxp && in(f, maxp, 6) ? (int)be16(d + maxp + 4) : 0;
    f->hmtx = table(f, base, "hmtx");
    f->cmap = table(f, base, "cmap");
    f->glyf = table(f, base, "glyf");
    f->loca = table(f, base, "loca");
    uint32_t cff = table(f, base, "CFF ");
    if (f->upem < 16 || f->upem > 16384 || (!f->glyf && !(cff && cff_open(f, cff))))
        return false;
    name_of(f, table(f, base, "name"));
    if (!f->family[0])
        str_copy(f->family, "Unnamed font", sizeof(f->family));
    return true;
}

static int glyph_index(const struct font *f, uint32_t cp)
{
    uint32_t c = f->cmap;
    if (!c || !in(f, c, 4))
        return 0;
    uint32_t n = be16(f->d + c + 2), best = 0;
    int best_fmt = 0;
    for (uint32_t i = 0; i < n && in(f, c + 4 + i * 8, 8); i++) {
        const uint8_t *r = f->d + c + 4 + i * 8;
        uint32_t platform = be16(r), encoding = be16(r + 2), sub = c + be32(r + 4);
        if (!in(f, sub, 2))
            continue;
        uint32_t fmt = be16(f->d + sub);
        if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
            if ((fmt == 12 && best_fmt != 12) || (fmt == 4 && !best_fmt))
                best = sub, best_fmt = (int)fmt;
    }
    if (best_fmt == 12 && in(f, best, 16)) {
        uint32_t groups = be32(f->d + best + 12);
        for (uint32_t g = 0; g < groups && in(f, best + 16 + g * 12, 12); g++) {
            const uint8_t *r = f->d + best + 16 + g * 12;
            if (cp >= be32(r) && cp <= be32(r + 4))
                return (int)(be32(r + 8) + cp - be32(r));
        }
    } else if (best_fmt == 4 && cp < 0x10000 && in(f, best, 14)) {
        uint32_t segs = be16(f->d + best + 6) / 2, ends = best + 14, starts = ends + segs * 2 + 2;
        uint32_t deltas = starts + segs * 2, ranges = deltas + segs * 2;
        if (!in(f, ranges, segs * 2))
            return 0;
        for (uint32_t s = 0; s < segs; s++) {
            if (cp > be16(f->d + ends + s * 2))
                continue;
            uint32_t start = be16(f->d + starts + s * 2);
            if (cp < start)
                return 0;
            uint32_t delta = be16(f->d + deltas + s * 2), ro = be16(f->d + ranges + s * 2);
            if (!ro)
                return (int)((cp + delta) & 0xffff);
            uint32_t at = ranges + s * 2 + ro + (cp - start) * 2;
            uint32_t g = in(f, at, 2) ? be16(f->d + at) : 0;
            return g ? (int)((g + delta) & 0xffff) : 0;
        }
    }
    return 0;
}

static int advance(const struct font *f, int gid)
{
    if (!f->hmtx || f->nhmetrics < 1)
        return f->upem / 2;
    int i = gid < f->nhmetrics ? gid : f->nhmetrics - 1;
    return in(f, f->hmtx + (uint32_t)i * 4, 2) ? (int)be16(f->d + f->hmtx + (uint32_t)i * 4) : f->upem / 2;
}

/* ---- outlines to line segments, in 26.6 pixels ---- */

struct seg {
    int32_t x0, y0, x1, y1;
};

struct path {
    struct seg *segs;
    int n;
    int64_t scale_x, scale_y; /* 26.6 pixels per 65536 font units */
    int32_t ox, oy;           /* the origin, 26.6 */
    int32_t cx, cy, sx, sy;   /* current and contour start, 26.6 */
    bool open;
};

static int32_t px_x(const struct path *p, int32_t fu16)
{
    return p->ox + (int32_t)(((int64_t)fu16 * p->scale_x) >> 32);
}

static int32_t px_y(const struct path *p, int32_t fu16)
{
    return p->oy - (int32_t)(((int64_t)fu16 * p->scale_y) >> 32);
}

static void line(struct path *p, int32_t x, int32_t y)
{
    if (p->n < MAX_SEGS && (y != p->cy || x != p->cx))
        p->segs[p->n++] = (struct seg){p->cx, p->cy, x, y};
    p->cx = x;
    p->cy = y;
}

static void close_contour(struct path *p)
{
    if (p->open)
        line(p, p->sx, p->sy);
    p->open = false;
}

/* font units in 16.16 */
static void move_to(struct path *p, int32_t x, int32_t y)
{
    close_contour(p);
    p->cx = p->sx = px_x(p, x);
    p->cy = p->sy = px_y(p, y);
    p->open = true;
}

static void line_to(struct path *p, int32_t x, int32_t y)
{
    line(p, px_x(p, x), px_y(p, y));
}

static void quad_to(struct path *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    int64_t ax = p->cx, ay = p->cy, bx = px_x(p, x1), by = px_y(p, y1), cx = px_x(p, x2), cy = px_y(p, y2);
    const int N = 8;
    for (int i = 1; i <= N; i++) {
        int64_t t = i, u = N - i;
        line(p, (int32_t)((u * u * ax + 2 * u * t * bx + t * t * cx) / (N * N)),
             (int32_t)((u * u * ay + 2 * u * t * by + t * t * cy) / (N * N)));
    }
}

static void cubic_to(struct path *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3)
{
    int64_t ax = p->cx, ay = p->cy, bx = px_x(p, x1), by = px_y(p, y1), cx = px_x(p, x2), cy = px_y(p, y2);
    int64_t dx = px_x(p, x3), dy = px_y(p, y3);
    const int N = 12;
    for (int i = 1; i <= N; i++) {
        int64_t t = i, u = N - i;
        line(p, (int32_t)((u * u * u * ax + 3 * u * u * t * bx + 3 * u * t * t * cx + t * t * t * dx) / (N * N * N)),
             (int32_t)((u * u * u * ay + 3 * u * u * t * by + 3 * u * t * t * cy + t * t * t * dy) / (N * N * N)));
    }
}

/* TrueType: one glyph (dx, dy in font units: a component's offset) */
static void tt_glyph(struct path *p, const struct font *f, int gid, int32_t dx, int32_t dy, int depth)
{
    if (gid < 0 || gid >= f->nglyphs || depth > 6 || !f->loca)
        return;
    uint32_t a, b;
    if (f->long_loca) {
        if (!in(f, f->loca + (uint32_t)gid * 4, 8))
            return;
        a = be32(f->d + f->loca + gid * 4), b = be32(f->d + f->loca + gid * 4 + 4);
    } else {
        if (!in(f, f->loca + (uint32_t)gid * 2, 4))
            return;
        a = be16(f->d + f->loca + gid * 2) * 2, b = be16(f->d + f->loca + gid * 2 + 2) * 2;
    }
    if (b <= a || !in(f, f->glyf + a, b - a) || b - a < 10)
        return;
    const uint8_t *g = f->d + f->glyf + a, *end = f->d + f->glyf + b;
    int contours = (int16_t)be16(g);
    if (contours < 0) { /* composite */
        const uint8_t *c = g + 10;
        for (;;) {
            if (c + 4 > end)
                return;
            uint32_t flags = be16(c), sub = be16(c + 2);
            c += 4;
            int32_t ox, oy;
            if (flags & 1) {
                if (c + 4 > end)
                    return;
                ox = (int16_t)be16(c), oy = (int16_t)be16(c + 2), c += 4;
            } else {
                if (c + 2 > end)
                    return;
                ox = (int8_t)c[0], oy = (int8_t)c[1], c += 2;
            }
            c += flags & 8 ? 2 : flags & 0x40 ? 4 : flags & 0x80 ? 8 : 0; /* scaled components drawn unscaled */
            if (!(flags & 2))
                ox = oy = 0; /* point-matched placement: not supported */
            tt_glyph(p, f, (int)sub, dx + ox, dy + oy, depth + 1);
            if (!(flags & 0x20))
                return;
        }
    }
    if (contours == 0)
        return;
    const uint8_t *ends = g + 10;
    if (ends + contours * 2 + 2 > end)
        return;
    int npts = (int)be16(ends + (contours - 1) * 2) + 1;
    uint32_t ilen = be16(ends + contours * 2);
    const uint8_t *fl = ends + contours * 2 + 2 + ilen;
    if (npts > 4096 || fl > end)
        return;
    uint8_t *flags = kmalloc((size_t)npts);
    int32_t *xs = kmalloc(sizeof(int32_t) * (size_t)npts * 2), *ys = xs ? xs + npts : NULL;
    if (!flags || !xs) {
        kfree(flags);
        kfree(xs);
        return;
    }
    const uint8_t *q = fl;
    for (int i = 0; i < npts && q < end;) {
        uint8_t fv = *q++;
        int rep = 1;
        if ((fv & 8) && q < end)
            rep += *q++;
        while (rep-- && i < npts)
            flags[i++] = fv;
    }
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fv = flags[i];
        if (fv & 2)
            v += q < end ? ((fv & 16) ? *q : -(int32_t)*q) : 0, q++;
        else if (!(fv & 16))
            v += q + 1 < end ? (int16_t)be16(q) : 0, q += 2;
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fv = flags[i];
        if (fv & 4)
            v += q < end ? ((fv & 32) ? *q : -(int32_t)*q) : 0, q++;
        else if (!(fv & 32))
            v += q + 1 < end ? (int16_t)be16(q) : 0, q += 2;
        ys[i] = v;
    }
    int start = 0;
    for (int c = 0; c < contours; c++) {
        int last = (int)be16(ends + c * 2);
        if (last < start || last >= npts)
            break;
        int n = last - start + 1;
        /* begin at an on-curve point, or between two off-curve ones */
        int first = -1;
        for (int k = 0; k < n && first < 0; k++)
            if (flags[start + k] & 1)
                first = k;
        int32_t fx, fy;
        if (first >= 0) {
            fx = xs[start + first], fy = ys[start + first];
        } else {
            first = 0;
            fx = (xs[start] + xs[last]) / 2, fy = (ys[start] + ys[last]) / 2;
        }
        move_to(p, (fx + dx) << 16, (fy + dy) << 16);
        bool have_ctrl = false;
        int32_t qx = 0, qy = 0;
        for (int k = 1; k <= n; k++) {
            int i = start + (first + k) % n;
            int32_t x = xs[i] + dx, y = ys[i] + dy;
            if (flags[i] & 1) {
                if (have_ctrl)
                    quad_to(p, qx << 16, qy << 16, x << 16, y << 16);
                else
                    line_to(p, x << 16, y << 16);
                have_ctrl = false;
            } else {
                if (have_ctrl)
                    quad_to(p, qx << 16, qy << 16, ((qx + x) / 2) << 16, ((qy + y) / 2) << 16);
                qx = x, qy = y;
                have_ctrl = true;
            }
        }
        if (have_ctrl)
            quad_to(p, qx << 16, qy << 16, (fx + dx) << 16, (fy + dy) << 16);
        close_contour(p);
        start = last + 1;
    }
    kfree(flags);
    kfree(xs);
}

/* CFF Type 2 charstrings; values are 16.16 */
struct t2 {
    struct path *p;
    const struct font *f;
    uint32_t subrs;
    int lbias;
    int32_t st[48];
    int n, stems, depth;
    int32_t x, y;
    bool width_done, ended;
};

static void t2_move(struct t2 *t, int32_t dx, int32_t dy)
{
    t->x += dx, t->y += dy;
    move_to(t->p, t->x, t->y);
}

static void t2_line(struct t2 *t, int32_t dx, int32_t dy)
{
    t->x += dx, t->y += dy;
    line_to(t->p, t->x, t->y);
}

static void t2_curve(struct t2 *t, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t g)
{
    int32_t x1 = t->x + a, y1 = t->y + b, x2 = x1 + c, y2 = y1 + d;
    t->x = x2 + e, t->y = y2 + g;
    cubic_to(t->p, x1, y1, x2, y2, t->x, t->y);
}

/* the optional width before the first stack-clearing operator */
static int t2_args(struct t2 *t, bool even)
{
    int skip = 0;
    if (!t->width_done) {
        if ((t->n % 2 == 1) == even)
            skip = 1;
        t->width_done = true;
    }
    return skip;
}

static void t2_run(struct t2 *t, uint32_t at, uint32_t len)
{
    const struct font *f = t->f;
    if (++t->depth > 10)
        return;
    for (uint32_t p = at; p < at + len && in(f, p, 1) && !t->ended;) {
        uint8_t b = f->d[p++];
        if (b >= 32 || b == 28) {
            int32_t v;
            if (b == 28) {
                v = in(f, p, 2) ? (int16_t)be16(f->d + p) : 0;
                p += 2;
                v *= 65536;
            } else if (b <= 246) {
                v = (b - 139) * 65536;
            } else if (b <= 250) {
                v = ((b - 247) * 256 + (in(f, p, 1) ? f->d[p] : 0) + 108) * 65536;
                p++;
            } else if (b <= 254) {
                v = (-(b - 251) * 256 - (in(f, p, 1) ? f->d[p] : 0) - 108) * 65536;
                p++;
            } else {
                v = in(f, p, 4) ? (int32_t)be32(f->d + p) : 0;
                p += 4;
            }
            if (t->n < 48)
                t->st[t->n++] = v;
            continue;
        }
        int32_t *s = t->st;
        int n = t->n, i = 0;
        switch (b) {
        case 1: case 3: case 18: case 23: /* stems */
            i = t2_args(t, true);
            t->stems += (n - i) / 2;
            t->n = 0;
            break;
        case 19: case 20: /* hintmask, cntrmask: an implicit vstem first */
            i = t2_args(t, true);
            t->stems += (n - i) / 2;
            p += (uint32_t)(t->stems + 7) / 8;
            t->n = 0;
            break;
        case 21:
            i = t2_args(t, true);
            if (n - i >= 2)
                t2_move(t, s[i], s[i + 1]);
            t->n = 0;
            break;
        case 22:
            i = t2_args(t, false);
            if (n - i >= 1)
                t2_move(t, s[i], 0);
            t->n = 0;
            break;
        case 4:
            i = t2_args(t, false);
            if (n - i >= 1)
                t2_move(t, 0, s[i]);
            t->n = 0;
            break;
        case 5:
            for (; i + 1 < n; i += 2)
                t2_line(t, s[i], s[i + 1]);
            t->n = 0;
            break;
        case 6: case 7: /* alternating horizontal and vertical lines */
            for (bool h = b == 6; i < n; i++, h = !h)
                t2_line(t, h ? s[i] : 0, h ? 0 : s[i]);
            t->n = 0;
            break;
        case 8:
            for (; i + 5 < n; i += 6)
                t2_curve(t, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            t->n = 0;
            break;
        case 24: /* rcurveline */
            for (; i + 5 < n - 2; i += 6)
                t2_curve(t, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            if (i + 1 < n)
                t2_line(t, s[i], s[i + 1]);
            t->n = 0;
            break;
        case 25: /* rlinecurve */
            for (; i + 1 < n - 6; i += 2)
                t2_line(t, s[i], s[i + 1]);
            if (i + 5 < n)
                t2_curve(t, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            t->n = 0;
            break;
        case 26: { /* vvcurveto */
            int32_t dx1 = 0;
            if (n % 2)
                dx1 = s[i++];
            for (; i + 3 < n; i += 4, dx1 = 0)
                t2_curve(t, dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
            t->n = 0;
            break;
        }
        case 27: { /* hhcurveto */
            int32_t dy1 = 0;
            if (n % 2)
                dy1 = s[i++];
            for (; i + 3 < n; i += 4, dy1 = 0)
                t2_curve(t, s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0);
            t->n = 0;
            break;
        }
        case 30: case 31: { /* vhcurveto, hvcurveto */
            bool h = b == 31;
            for (; i + 3 < n; i += 4, h = !h) {
                int32_t last = (n - i == 5) ? s[i + 4] : 0;
                if (h)
                    t2_curve(t, s[i], 0, s[i + 1], s[i + 2], last, s[i + 3]);
                else
                    t2_curve(t, 0, s[i], s[i + 1], s[i + 2], s[i + 3], last);
            }
            t->n = 0;
            break;
        }
        case 10: case 29: { /* callsubr, callgsubr */
            if (!n)
                break;
            int idx = (s[--t->n] >> 16) + (b == 10 ? t->lbias : f->cff_bias_g);
            uint32_t off, sl;
            if (idx >= 0 && cff_item(f, b == 10 ? t->subrs : f->gsubrs, (uint32_t)idx, &off, &sl))
                t2_run(t, off, sl);
            break;
        }
        case 11:
            t->depth--;
            return;
        case 14:
            t2_args(t, true);
            close_contour(t->p);
            t->ended = true;
            break;
        case 12: {
            uint8_t e = in(f, p, 1) ? f->d[p] : 0;
            p++;
            int32_t y0 = t->y;
            if (e == 35 && n >= 12) { /* flex */
                t2_curve(t, s[0], s[1], s[2], s[3], s[4], s[5]);
                t2_curve(t, s[6], s[7], s[8], s[9], s[10], s[11]);
            } else if (e == 34 && n >= 7) { /* hflex */
                t2_curve(t, s[0], 0, s[1], s[2], s[3], 0);
                t2_curve(t, s[4], 0, s[5], y0 - t->y, s[6], 0);
            } else if (e == 36 && n >= 9) { /* hflex1 */
                t2_curve(t, s[0], s[1], s[2], s[3], s[4], 0);
                t2_curve(t, s[5], 0, s[6], s[7], s[8], y0 - (t->y + s[7]));
            } else if (e == 37 && n >= 11) { /* flex1 */
                int32_t x0 = t->x, dx = 0, dy = 0;
                for (int k = 0; k < 10; k += 2)
                    dx += s[k], dy += s[k + 1];
                t2_curve(t, s[0], s[1], s[2], s[3], s[4], s[5]);
                bool horiz = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy);
                int32_t lx = horiz ? s[10] : x0 - (t->x + s[6] + s[8]);
                int32_t ly = horiz ? y0 - (t->y + s[7] + s[9]) : s[10];
                t2_curve(t, s[6], s[7], s[8], s[9], lx, ly);
            }
            t->n = 0;
            break;
        }
        default:
            t->n = 0;
            break;
        }
    }
    t->depth--;
}

static void cff_glyph(struct path *p, const struct font *f, int gid)
{
    uint32_t off, len;
    if (!cff_item(f, f->charstrings, (uint32_t)gid, &off, &len))
        return;
    struct t2 *t = kzalloc(sizeof(*t));
    if (!t)
        return;
    t->p = p;
    t->f = f;
    t->subrs = f->subrs;
    t->lbias = f->cff_bias_l;
    if (f->cid)
        cid_subrs(f, gid, &t->subrs, &t->lbias);
    t2_run(t, off, len);
    close_contour(p);
    kfree(t);
}

/* ---- filling ---- */

struct cross {
    int32_t x;
    int dir;
};

/* coverage 0..255 of a w x h box from the segments; xscale 3 for subpixel */
static uint8_t *fill(const struct seg *segs, int n, int w, int h)
{
    uint8_t *cov = kzalloc((size_t)w * (size_t)h);
    uint16_t *acc = kmalloc(sizeof(uint16_t) * (size_t)w);
    struct cross *xs = kmalloc(sizeof(struct cross) * (size_t)(n + 1));
    if (!cov || !acc || !xs) {
        kfree(acc);
        kfree(xs);
        return cov;
    }
    for (int row = 0; row < h; row++) {
        memset(acc, 0, sizeof(uint16_t) * (size_t)w);
        for (int sub = 0; sub < 4; sub++) {
            int32_t y = row * 64 + sub * 16 + 8;
            int nx = 0;
            for (int i = 0; i < n; i++) {
                const struct seg *s = &segs[i];
                int32_t lo = MIN(s->y0, s->y1), hi = MAX(s->y0, s->y1);
                if (y < lo || y >= hi)
                    continue;
                int32_t x = s->x0 + (int32_t)((int64_t)(y - s->y0) * (s->x1 - s->x0) / (s->y1 - s->y0));
                xs[nx++] = (struct cross){x, s->y1 > s->y0 ? 1 : -1};
            }
            for (int i = 1; i < nx; i++) /* by x */
                for (int j = i; j > 0 && xs[j].x < xs[j - 1].x; j--) {
                    struct cross tmp = xs[j];
                    xs[j] = xs[j - 1];
                    xs[j - 1] = tmp;
                }
            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += xs[i].dir;
                if (!wind)
                    continue;
                int32_t a = MAX(xs[i].x, 0), b = MIN(xs[i + 1].x, w * 64);
                for (int32_t x = a; x < b;) { /* each pixel gets the length of span inside it */
                    int px = x >> 6;
                    int32_t next = MIN((px + 1) * 64, b);
                    acc[px] += (uint16_t)(next - x);
                    x = next;
                }
            }
        }
        for (int x = 0; x < w; x++)
            cov[row * w + x] = (uint8_t)MIN(255, acc[x]);
    }
    kfree(acc);
    kfree(xs);
    return cov;
}

/* ---- drawing text ---- */

static uint32_t mix(uint32_t bg, uint32_t fg, int r, int g, int b)
{
    int br = (int)(bg >> 16 & 255), bgc = (int)(bg >> 8 & 255), bb = (int)(bg & 255);
    int fr = (int)(fg >> 16 & 255), fgc = (int)(fg >> 8 & 255), fb = (int)(fg & 255);
    return 0xff000000u | (uint32_t)(br + (fr - br) * r / 255) << 16 | (uint32_t)(bgc + (fgc - bgc) * g / 255) << 8 |
           (uint32_t)(bb + (fb - bb) * b / 255);
}

/* draws text with its baseline at (x, y); returns the width in pixels. Without a canvas only measures. */
static int draw_text(struct canvas *c, const struct font *f, const char *text, int x, int y, int px, uint32_t color,
                     bool subpixel)
{
    struct path p = {0};
    p.segs = kmalloc(sizeof(struct seg) * MAX_SEGS);
    if (!p.segs)
        return 0;
    int xs = subpixel ? 3 : 1;
    p.scale_x = ((int64_t)px * 64 * xs << 32) / ((int64_t)f->upem << 16);
    p.scale_y = ((int64_t)px * 64 << 32) / ((int64_t)f->upem << 16);
    int asc = (int)((int64_t)f->ascent * px / f->upem) + 2, desc = (int)((int64_t)-f->descent * px / f->upem) + 2;
    int64_t pen = 0; /* 26.6 */
    for (const char *s = text; *s;) {
        int cp = (int)utf8_next(&s);
        int gid = glyph_index(f, (uint32_t)cp);
        int adv = (int)((int64_t)advance(f, gid) * px * 64 / f->upem);
        if (c && cp != ' ') {
            p.n = 0;
            int gw = (adv >> 6) + px + 4, gh = asc + desc;
            int left = px / 4 + 2;
            p.ox = left * 64 * xs + (int32_t)((pen & 63) * xs);
            p.oy = asc * 64;
            if (f->glyf)
                tt_glyph(&p, f, gid, 0, 0, 0);
            else
                cff_glyph(&p, f, gid);
            uint8_t *cov = p.n ? fill(p.segs, p.n, gw * xs, gh) : NULL;
            int gx = x + (int)(pen >> 6) - left, gy = y - asc;
            for (int row = 0; cov && row < gh; row++) {
                int ty = gy + row;
                if (ty < c->clip.y || ty >= c->clip.y + c->clip.h)
                    continue;
                for (int col = 0; col < gw; col++) {
                    int tx = gx + col;
                    if (tx < c->clip.x || tx >= c->clip.x + c->clip.w)
                        continue;
                    const uint8_t *r = cov + row * gw * xs;
                    int cr, cg, cb;
                    if (subpixel) { /* five-tap filter over the thirds: red left, green centre, blue right */
                        int ch[3];
                        for (int k = 0; k < 3; k++) {
                            int at = col * 3 + k, sum = 0;
                            static const int taps[5] = {1, 2, 3, 2, 1};
                            for (int d = -2; d <= 2; d++) {
                                int q = at + d;
                                sum += taps[d + 2] * (q >= 0 && q < gw * 3 ? r[q] : 0);
                            }
                            ch[k] = sum / 9;
                        }
                        cr = ch[0], cg = ch[1], cb = ch[2];
                    } else {
                        cr = cg = cb = r[col];
                    }
                    if (cr | cg | cb) {
                        uint32_t *dst = &c->px[ty * c->stride + tx];
                        *dst = mix(*dst, color, cr, cg, cb);
                    }
                }
            }
            kfree(cov);
        }
        pen += adv;
    }
    kfree(p.segs);
    return (int)(pen >> 6);
}

/* ---- installed fonts ---- */

struct entry {
    char path[PATH_MAX_LEN];
    char family[64], style[48];
    bool user;
};

static struct entry *fonts_list;
static int fonts_count;

static bool font_dir(bool user, char *out, size_t cap)
{
    char win[PATH_MAX_LEN];
    if (user)
        ksnprintf(win, sizeof(win), USER_FONTS, user_current()->name);
    else
        str_copy(win, SYSTEM_FONTS, sizeof(win));
    return winpath_to_vfs("/", win, out, cap) == 0;
}

static bool font_ext(const char *name)
{
    const char *e = path_ext(name);
    return e && (!str_casecmp(e, ".ttf") || !str_casecmp(e, ".otf") || !str_casecmp(e, ".ttc"));
}

static bool describe(const char *path, char *family, size_t fcap, char *style, size_t scap)
{
    char *data;
    size_t size;
    if (vfs_read_all(path, &data, &size) != 0)
        return false;
    struct font *f = kmalloc(sizeof(*f));
    bool ok = f && font_open(f, (const uint8_t *)data, size);
    if (ok) {
        str_copy(family, f->family, fcap);
        str_copy(style, f->style[0] ? f->style : "Regular", scap);
    }
    kfree(f);
    kfree(data);
    return ok;
}

static void scan_dir(const char *dir, bool user)
{
    struct vdir d;
    if (vfs_opendir(dir, &d) != 0)
        return;
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == 0 && fonts_count < MAX_FONTS) {
        if (!font_ext(ent.name))
            continue;
        struct entry *e = &fonts_list[fonts_count];
        path_join(dir, ent.name, e->path, sizeof(e->path));
        e->user = user;
        if (describe(e->path, e->family, sizeof(e->family), e->style, sizeof(e->style)))
            fonts_count++;
    }
    vfs_closedir(&d);
}

static void rescan(void)
{
    if (!fonts_list && !(fonts_list = kzalloc(sizeof(struct entry) * MAX_FONTS)))
        return;
    fonts_count = 0;
    char dir[PATH_MAX_LEN];
    scan_dir("/system/fonts", false);
    if (font_dir(false, dir, sizeof(dir)))
        scan_dir(dir, false);
    if (font_dir(true, dir, sizeof(dir)))
        scan_dir(dir, true);
}

/* installs for everyone when an administrator asks, else for this person; *where gets the new path */
static int install(const char *file, char *where, size_t cap)
{
    char family[64], style[48], dir[PATH_MAX_LEN];
    if (!describe(file, family, sizeof(family), style, sizeof(style)))
        return E_INVAL;
    bool user = !user_current()->admin;
    if (!font_dir(user, dir, sizeof(dir)))
        return E_NOENT;
    vfs_mkdirs(dir);
    path_join(dir, path_basename(file), where, cap);
    if (!strcmp(where, file))
        return E_EXIST;
    return vfs_copy(file, where);
}

static int uninstall(const char *name, char *removed, size_t cap)
{
    rescan();
    for (int i = 0; i < fonts_count; i++) {
        const struct entry *e = &fonts_list[i];
        char full[128];
        ksnprintf(full, sizeof(full), "%s %s", e->family, e->style);
        if (str_casecmp(e->family, name) && str_casecmp(full, name) && str_casecmp(path_basename(e->path), name))
            continue;
        if (!strncmp(e->path, "/system/", 8))
            return E_PERM;
        str_copy(removed, full, cap);
        return vfs_unlink(e->path);
    }
    return E_NOENT;
}

/* ---- the app ---- */

struct fonts_app {
    struct ui_state st;
    int selected, scroll;
    bool subpixel;
    struct ui_text sample;
    char opened[PATH_MAX_LEN]; /* a font file opened from Files */
    char status[160];
    char *data; /* the font shown */
    size_t size;
    struct font font;
    bool font_ok;
    char shown[PATH_MAX_LEN];
};

static struct window *open_win;

static void show(struct fonts_app *a, const char *path)
{
    if (!strcmp(a->shown, path))
        return;
    kfree(a->data);
    a->data = NULL;
    a->font_ok = false;
    str_copy(a->shown, path, sizeof(a->shown));
    if (vfs_read_all(path, &a->data, &a->size) == 0)
        a->font_ok = font_open(&a->font, (const uint8_t *)a->data, a->size);
}

static void frame(struct ui *ui, void *data)
{
    struct fonts_app *a = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    ui_sidebar_panel(ui, TOOL_SIDE, H, style.window);
    ui_sidebar_header(ui, (struct rect){22, 46, TOOL_SIDE - 32, 20}, "Fonts");
    int y = 68;
    if (a->opened[0]) {
        if (ui_sidebar_item(ui, (struct rect){14, y, TOOL_SIDE - 26, TOOL_ROW - 2}, SYM_DOC, path_basename(a->opened),
                            a->selected == -1))
            a->selected = -1;
        y += TOOL_ROW;
    }
    for (int i = a->scroll; i < fonts_count && y + TOOL_ROW < H - 8; i++, y += TOOL_ROW) {
        char label[120];
        ksnprintf(label, sizeof(label), "%s %s", fonts_list[i].family, fonts_list[i].style);
        if (ui_sidebar_item(ui, (struct rect){14, y, TOOL_SIDE - 26, TOOL_ROW - 2}, SYM_DOC, label,
                            a->selected == i))
            a->selected = i;
    }
    if (ui->e && ui->e->type == EV_WHEEL && ui->e->x < TOOL_SIDE)
        a->scroll = CLAMP(a->scroll + ui->e->wheel, 0, MAX(fonts_count - 4, 0));
    const char *path = a->selected == -1 ? a->opened : a->selected < fonts_count ? fonts_list[a->selected].path : "";
    if (path[0])
        show(a, path);
    char title[128] = "Fonts";
    if (a->font_ok)
        ksnprintf(title, sizeof(title), "%s %s", a->font.family, a->font.style[0] ? a->font.style : "");
    char sub[64];
    ksnprintf(sub, sizeof(sub), fonts_count == 1 ? "%d font" : "%d fonts", fonts_count);
    struct rect r = tool_page(ui, W, H, title, sub);
    int x = r.x + 20, w = r.w - 40;
    y = r.y + 12;
    static const char *const modes[2] = {"Standard", "ClearType"};
    int mode = a->subpixel;
    ui_segmented(ui, (struct rect){x, y, 200, 26}, modes, 2, &mode);
    a->subpixel = mode == 1;
    ui_textbox_hint(ui, (struct rect){x + 212, y, w - 212 - (a->selected == -1 ? 112 : 112), 26}, &a->sample,
                    "Type a sample");
    struct rect btn = {x + w - 100, y, 100, 26};
    bool installed = false;
    for (int i = 0; a->selected == -1 && i < fonts_count; i++)
        installed |= !strcmp(fonts_list[i].path, a->opened);
    if (a->selected == -1 && !installed) {
        if (ui_button_primary(ui, btn, "Install")) {
            char where[PATH_MAX_LEN];
            int err = install(a->opened, where, sizeof(where));
            ksnprintf(a->status, sizeof(a->status), err ? "Could not install: %s" : "Installed into %s",
                      err ? err_str(err) : where);
            rescan();
        }
    } else if (a->selected >= 0 && a->selected < fonts_count && strncmp(path, "/system/", 8) &&
               ui_button(ui, btn, "Remove")) {
        int err = vfs_unlink(path);
        ksnprintf(a->status, sizeof(a->status), err ? "Could not remove: %s" : "Removed", err ? err_str(err) : "");
        a->selected = 0;
        a->shown[0] = 0;
        rescan();
    }
    y += 40;
    if (!a->font_ok) {
        ui_label(ui, (struct rect){x, y + 40, w, 20}, path[0] ? "This file is not a font that can be shown" :
                                                                "No fonts", &font_sans, style.tertiary, ALIGN_CENTER);
    } else if (ui->c) {
        struct canvas *c = ui->c;
        struct rect saved = c->clip;
        c->clip = (struct rect){r.x, r.y + 44, r.w, r.h - 70};
        const char *sample = a->sample.buf[0] ? a->sample.buf : "The quick brown fox jumps over the lazy dog";
        static const int sizes[] = {12, 16, 24, 36, 54};
        for (size_t i = 0; i < ARRAY_SIZE(sizes) && y < H - 40; i++) {
            int s = sizes[i];
            char label[8];
            ksnprintf(label, sizeof(label), "%d", s);
            ui_label(ui, (struct rect){x, y, 30, 18}, label, &font_small, style.tertiary, ALIGN_LEFT);
            draw_text(c, &a->font, sample, x + 34, y + s, s, style.text, a->subpixel);
            y += s + s / 2 + 6;
        }
        draw_text(c, &a->font, "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789", x + 34, y + 20, 18,
                  style.secondary, a->subpixel);
        c->clip = saved;
    }
    ui_label(ui, (struct rect){x, H - 26, w, 20}, a->status, &font_small, style.secondary, ALIGN_LEFT);
}

static void paint(struct window *win, struct canvas *c)
{
    struct fonts_app *a = win->data;
    ui_run(win, &a->st, c, NULL, frame, a);
}

static void event(struct window *win, struct gui_event *e)
{
    struct fonts_app *a = win->data;
    ui_run(win, &a->st, NULL, e, frame, a);
}

static void destroy(struct window *win)
{
    struct fonts_app *a = win->data;
    kfree(a->data);
    kfree(a);
    if (open_win == win)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    if (open_win && wm_window_alive(open_win))
        wm_destroy(open_win);
    struct fonts_app *a = kzalloc(sizeof(*a));
    if (!a)
        return;
    rescan();
    a->subpixel = true;
    if (arg && arg[0]) {
        str_copy(a->opened, arg, sizeof(a->opened));
        a->selected = -1;
    }
    open_win = tool_open("Fonts", ICON_TEXT, 880, 600, &ops, a);
    if (!open_win)
        kfree(a);
}

static const struct gui_app fonts_app = {"Fonts", "Install, preview and remove TrueType and OpenType fonts", ICON_TEXT,
                                         launch, ".ttf;.otf;.ttc", NULL};

/* ---- the command ---- */

COMMAND(fonts, "fontview", "system",
        "fonts [list] | fonts info|install <file> | fonts remove <name> | fonts render <file> <text> [size]",
        "list, install, remove and draw TrueType/OpenType fonts")
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "list"))) {
        rescan();
        for (int i = 0; i < fonts_count; i++)
            sh_printf(sh, "%-28s %-14s %s%s\n", fonts_list[i].family, fonts_list[i].style, fonts_list[i].path,
                      fonts_list[i].user ? " (yours)" : "");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "remove")) {
        char removed[128];
        int err = uninstall(argv[2], removed, sizeof(removed));
        if (err == E_PERM)
            return sh_error(sh, "fonts", "%s is part of the system", argv[2]);
        if (err)
            return sh_error(sh, "fonts", "%s: %s", argv[2], err_str(err));
        sh_printf(sh, "Removed %s\n", removed);
        return 0;
    }
    if (argc < 3)
        return command_usage(sh, command_find("fonts"));
    char path[PATH_MAX_LEN];
    if (sh_resolve(sh, argv[2], path, sizeof(path)) != 0 || !vfs_exists(path))
        return sh_fs_error(sh, "fonts", argv[2], E_NOENT);
    if (!strcmp(argv[1], "install")) {
        char where[PATH_MAX_LEN];
        int err = install(path, where, sizeof(where));
        if (err == E_INVAL)
            return sh_error(sh, "fonts", "%s is not a TrueType or OpenType font", argv[2]);
        if (err)
            return sh_error(sh, "fonts", "%s", err_str(err));
        sh_printf(sh, "Installed into %s\n", where);
        return 0;
    }
    char *data;
    size_t size;
    struct font *f = kmalloc(sizeof(*f));
    if (!f || vfs_read_all(path, &data, &size) != 0) {
        kfree(f);
        return sh_fs_error(sh, "fonts", argv[2], E_IO);
    }
    int rc = 0;
    if (!font_open(f, (const uint8_t *)data, size)) {
        rc = sh_error(sh, "fonts", "%s is not a TrueType or OpenType font", argv[2]);
    } else if (!strcmp(argv[1], "info")) {
        sh_printf(sh, "family: %s\nstyle: %s\noutlines: %s\nglyphs: %d\nunits per em: %d\n", f->family, f->style,
                  f->glyf ? "TrueType" : f->cid ? "CFF (CID)" : "CFF", f->nglyphs, f->upem);
    } else if (!strcmp(argv[1], "render") && argc >= 4) {
        /* the text drawn small, as characters: a check that the outlines come out */
        int px = argc > 4 ? (int)str_to_long(argv[4], NULL, 10) : 16;
        px = CLAMP(px, 6, 48);
        int w = draw_text(NULL, f, argv[3], 0, 0, px, 0, false) + px, h = px + px / 2;
        struct canvas c = {kzalloc(sizeof(uint32_t) * (size_t)w * (size_t)h), w, h, w, {0, 0, w, h}};
        if (c.px) {
            for (int i = 0; i < w * h; i++)
                c.px[i] = 0xff000000u;
            draw_text(&c, f, argv[3], 0, px, px, 0xffffffffu, false);
            int ink = 0;
            for (int yy = 0; yy < h; yy++) {
                char row[256];
                int n = 0;
                for (int xx = 0; xx < w && n < 255; xx++) {
                    int v = (int)(c.px[yy * w + xx] & 255);
                    row[n++] = v > 170 ? '#' : v > 85 ? '+' : v > 20 ? '.' : ' ';
                    ink += v > 20;
                }
                while (n && row[n - 1] == ' ')
                    n--;
                row[n] = 0;
                if (n)
                    sh_printf(sh, "%s\n", row);
            }
            sh_printf(sh, "%d pixels inked\n", ink);
            kfree(c.px);
        }
    } else {
        rc = command_usage(sh, command_find("fonts"));
    }
    kfree(f);
    kfree(data);
    return rc;
}

static const struct command *const commands[] = {&cmddef_fonts};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&fonts_app, NULL, &ext);
