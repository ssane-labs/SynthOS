/* gdiplus.dll calls that take REAL (float) arguments: rounded to pixels (scales to 16.16) and passed to gdiplus.c */
#include <synth/config.h>
#if CONFIG_GUI
#include <synth/string.h>

#include "gdiplus.h"

typedef float REAL;

enum { Ok, GenericError, InvalidParameter, OutOfMemory };

#define ONE 65536.0f

static int R(REAL v)
{
    return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

static int FX(REAL v)
{
    return (int)(v * ONE);
}

/* PointF and RectF arrays as integer points */
static int *ints(const REAL *v, int n)
{
    int *out = n > 0 ? kmalloc(sizeof(int) * (size_t)n) : NULL;
    for (int i = 0; out && i < n; i++)
        out[i] = R(v[i]);
    return out;
}

static int WINAPI GdipDrawLine(void *g, void *pen, REAL x0, REAL y0, REAL x1, REAL y1)
{
    return w32_gp_draw_lines(g, pen, (const int[]){R(x0), R(y0), R(x1), R(y1)}, 2, false);
}

static int lines(void *g, void *pen, const REAL *pts, int n, bool closed)
{
    int *p = ints(pts, n * 2);
    if (!p)
        return n < 2 ? InvalidParameter : OutOfMemory;
    int r = w32_gp_draw_lines(g, pen, p, n, closed);
    kfree(p);
    return r;
}

static int WINAPI GdipDrawLines(void *g, void *pen, const REAL *pts, int n)
{
    return lines(g, pen, pts, n, false);
}

static int WINAPI GdipDrawPolygon(void *g, void *pen, const REAL *pts, int n)
{
    return lines(g, pen, pts, n, true);
}

static int WINAPI GdipFillPolygon(void *g, void *brush, const REAL *pts, int n, int mode)
{
    UNUSED(mode);
    int *p = ints(pts, n * 2);
    if (!p)
        return n < 3 ? InvalidParameter : OutOfMemory;
    int r = w32_gp_fill_poly(g, brush, p, n);
    kfree(p);
    return r;
}

static int WINAPI GdipFillPolygon2(void *g, void *brush, const REAL *pts, int n)
{
    return GdipFillPolygon(g, brush, pts, n, 0);
}

static int WINAPI GdipDrawRectangle(void *g, void *pen, REAL x, REAL y, REAL w, REAL h)
{
    return w32_gp_draw_rect(g, pen, R(x), R(y), R(w), R(h));
}

static int WINAPI GdipFillRectangle(void *g, void *brush, REAL x, REAL y, REAL w, REAL h)
{
    return w32_gp_fill_rect(g, brush, R(x), R(y), R(x + w) - R(x), R(y + h) - R(y));
}

static int WINAPI GdipFillRectangles(void *g, void *brush, const REAL *rects, int n)
{
    for (int i = 0; i < n; i++) {
        int r = GdipFillRectangle(g, brush, rects[i * 4], rects[i * 4 + 1], rects[i * 4 + 2], rects[i * 4 + 3]);
        if (r != Ok)
            return r;
    }
    return Ok;
}

static int WINAPI GdipFillEllipse(void *g, void *brush, REAL x, REAL y, REAL w, REAL h)
{
    return w32_gp_fill_ellipse(g, brush, R(x), R(y), R(w), R(h));
}

static int WINAPI GdipDrawEllipse(void *g, void *pen, REAL x, REAL y, REAL w, REAL h)
{
    return w32_gp_draw_ellipse(g, pen, R(x), R(y), R(w), R(h));
}

static int WINAPI GdipDrawImage(void *g, void *image, REAL x, REAL y)
{
    int w, h;
    if (!w32_gp_image_size(image, &w, &h))
        return InvalidParameter;
    return w32_gp_draw_image(g, image, R(x), R(y), w, h, 0, 0, w, h);
}

static int WINAPI GdipDrawImageRect(void *g, void *image, REAL x, REAL y, REAL dw, REAL dh)
{
    int w, h;
    if (!w32_gp_image_size(image, &w, &h))
        return InvalidParameter;
    return w32_gp_draw_image(g, image, R(x), R(y), R(x + dw) - R(x), R(y + dh) - R(y), 0, 0, w, h);
}

static int WINAPI GdipDrawImageRectRect(void *g, void *image, REAL dx, REAL dy, REAL dw, REAL dh, REAL sx, REAL sy,
                                        REAL sw, REAL sh, int unit, void *attrs, void *cb, void *data)
{
    UNUSED(unit);
    UNUSED(attrs);
    UNUSED(cb);
    UNUSED(data);
    return w32_gp_draw_image(g, image, R(dx), R(dy), R(dx + dw) - R(dx), R(dy + dh) - R(dy), R(sx), R(sy), R(sw),
                             R(sh));
}

static int WINAPI GdipDrawImagePointRect(void *g, void *image, REAL x, REAL y, REAL sx, REAL sy, REAL sw, REAL sh,
                                         int unit)
{
    UNUSED(unit);
    return w32_gp_draw_image(g, image, R(x), R(y), R(sw), R(sh), R(sx), R(sy), R(sw), R(sh));
}

static int WINAPI GdipTranslateWorldTransform(void *g, REAL dx, REAL dy, int order)
{
    return w32_gp_transform(g, R(dx), R(dy), 65536, 65536, order);
}

static int WINAPI GdipScaleWorldTransform(void *g, REAL sx, REAL sy, int order)
{
    return w32_gp_transform(g, 0, 0, FX(sx), FX(sy), order);
}

/* turns are not drawn: only the half turn, which is a scale by -1 */
static int WINAPI GdipRotateWorldTransform(void *g, REAL angle, int order)
{
    int a = R(angle) % 360;
    return a == 180 || a == -180 ? w32_gp_transform(g, 0, 0, -65536, -65536, order) : w32_gp_transform(g, 0, 0, 65536,
                                                                                                         65536, order);
}

static int WINAPI GdipSetClipRect(void *g, REAL x, REAL y, REAL w, REAL h, int mode)
{
    return w32_gp_clip(g, R(x), R(y), R(x + w) - R(x), R(y + h) - R(y), mode);
}

static int bounds_out(int r, const int *v, REAL *rect)
{
    if (r == Ok)
        for (int i = 0; i < 4; i++)
            rect[i] = (REAL)v[i];
    return r;
}

static int WINAPI GdipGetClipBounds(void *g, REAL *rect)
{
    int v[4];
    return rect ? bounds_out(w32_gp_clip_bounds(g, v), v, rect) : InvalidParameter;
}

static int WINAPI GdipGetRegionBounds(void *region, void *g, REAL *rect)
{
    UNUSED(g);
    int v[4];
    return rect ? bounds_out(w32_gp_region_get(region, v), v, rect) : InvalidParameter;
}

static int WINAPI GdipCreateRegionRect(const REAL *r, void **out)
{
    return r ? w32_gp_region_rect(R(r[0]), R(r[1]), R(r[2]), R(r[3]), out) : InvalidParameter;
}

static int WINAPI GdipAddPathRectangle(void *path, REAL x, REAL y, REAL w, REAL h)
{
    int x0 = R(x), y0 = R(y), x1 = R(x + w), y1 = R(y + h);
    return w32_gp_path_add(path, (const int[]){x0, y0, x1, y0, x1, y1, x0, y1}, 4, true);
}

static int WINAPI GdipAddPathEllipse(void *path, REAL x, REAL y, REAL w, REAL h)
{
    return w32_gp_path_ellipse(path, (int)x, (int)y, (int)w, (int)h);
}

static int WINAPI GdipAddPathArc(void *path, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    return w32_gp_path_arc(path, R(x), R(y), R(w), R(h), (int)(start * 16), (int)(sweep * 16));
}

static int WINAPI GdipAddPathArcI(void *path, int x, int y, int w, int h, REAL start, REAL sweep)
{
    return w32_gp_path_arc(path, x, y, w, h, (int)(start * 16), (int)(sweep * 16));
}

static int WINAPI GdipDrawArcI(void *graphics, void *pen, int x, int y, int w, int h, REAL start, REAL sweep)
{
    int pts[2 * 66];
    return w32_gp_draw_lines(graphics, pen, pts, w32_gp_arc_points(x, y, w, h, (int)(start * 16), (int)(sweep * 16), pts, 66), false);
}

static int WINAPI GdipDrawArc(void *graphics, void *pen, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep)
{
    return GdipDrawArcI(graphics, pen, R(x), R(y), R(w), R(h), start, sweep);
}

static int WINAPI GdipAddPathBezier(void *path, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
    int pts[34], c[8] = {R(x1), R(y1), R(x2), R(y2), R(x3), R(y3), R(x4), R(y4)};
    return w32_gp_path_add(path, pts, w32_gp_bezier_points(c, pts), false);
}

static int WINAPI GdipDrawBezier(void *graphics, void *pen, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
    int pts[34], c[8] = {R(x1), R(y1), R(x2), R(y2), R(x3), R(y3), R(x4), R(y4)};
    return w32_gp_draw_lines(graphics, pen, pts, w32_gp_bezier_points(c, pts), false);
}

static int WINAPI GdipAddPathLine(void *path, REAL x0, REAL y0, REAL x1, REAL y1)
{
    return w32_gp_path_add(path, (const int[]){R(x0), R(y0), R(x1), R(y1)}, 2, false);
}

static int WINAPI GdipAddPathPolygon(void *path, const REAL *pts, int n)
{
    int *p = ints(pts, n * 2);
    if (!p)
        return n < 1 ? InvalidParameter : OutOfMemory;
    int r = w32_gp_path_add(path, p, n, true);
    kfree(p);
    return r;
}

static int WINAPI GdipCreateLineBrush(const REAL *p1, const REAL *p2, uint32_t c1, uint32_t c2, int wrap, void **out)
{
    UNUSED(wrap);
    if (!p1 || !p2)
        return InvalidParameter;
    return w32_gp_gradient(R(p1[0]), R(p1[1]), R(p2[0]), R(p2[1]), c1, c2, out);
}

static int WINAPI GdipCreateLineBrushFromRect(const REAL *r, uint32_t c1, uint32_t c2, int mode, int wrap, void **out)
{
    UNUSED(wrap);
    if (!r)
        return InvalidParameter;
    int v[4] = {R(r[0]), R(r[1]), R(r[2]), R(r[3])};
    return w32_gp_gradient_rect(v, c1, c2, mode, out);
}

static int WINAPI GdipCreatePen1(uint32_t color, REAL width, int unit, void **out)
{
    UNUSED(unit);
    return w32_gp_pen(color, R(width), NULL, out);
}

static int WINAPI GdipCreatePen2(void *brush, REAL width, int unit, void **out)
{
    UNUSED(unit);
    return brush ? w32_gp_pen(0, R(width), brush, out) : InvalidParameter;
}

static int WINAPI GdipSetPenWidth(void *pen, REAL width)
{
    return w32_gp_pen_width(pen, R(width), NULL);
}

static int WINAPI GdipGetPenWidth(void *pen, REAL *width)
{
    int w;
    int r = width ? w32_gp_pen_width(pen, 0, &w) : InvalidParameter;
    if (r == Ok)
        *width = (REAL)w;
    return r;
}

static int WINAPI GdipGetImageHorizontalResolution(void *image, REAL *res)
{
    int dpi = w32_gp_image_res(image);
    if (dpi < 0 || !res)
        return InvalidParameter;
    *res = (REAL)dpi;
    return Ok;
}

static int WINAPI GdipBitmapSetResolution(void *image, REAL x, REAL y)
{
    return w32_gp_image_res(image) >= 0 && x > 0 && y > 0 ? Ok : InvalidParameter;
}

static int WINAPI GdipGetImageDimension(void *image, REAL *w, REAL *h)
{
    int iw, ih;
    if (!w || !h || !w32_gp_image_size(image, &iw, &ih))
        return InvalidParameter;
    *w = (REAL)iw;
    *h = (REAL)ih;
    return Ok;
}

static int WINAPI GdipGetImageBounds(void *image, REAL *rect, int *unit)
{
    int iw, ih;
    if (!rect || !unit || !w32_gp_image_size(image, &iw, &ih))
        return InvalidParameter;
    rect[0] = rect[1] = 0;
    rect[2] = (REAL)iw;
    rect[3] = (REAL)ih;
    *unit = 2;
    return Ok;
}

/* UnitWorld/Display/Pixel are pixels, UnitPoint 1/72 inch at 96 dpi, UnitInch, UnitDocument 1/300, UnitMillimeter */
static REAL to_px(REAL size, int unit)
{
    switch (unit) {
    case 3: return size * 96.0f / 72.0f;
    case 4: return size * 96.0f;
    case 5: return size * 96.0f / 300.0f;
    case 6: return size * 96.0f / 25.4f;
    default: return size;
    }
}

static int WINAPI GdipCreateFont(void *family, REAL size, int style, int unit, void **out)
{
    return w32_gp_font(family, R(to_px(size, unit)), style, out);
}

static int WINAPI GdipGetFontSize(void *font, REAL *size)
{
    int px = w32_gp_font_px(font);
    if (px < 0 || !size)
        return InvalidParameter;
    *size = (REAL)px;
    return Ok;
}

static int WINAPI GdipGetFontHeight(void *font, void *g, REAL *height)
{
    UNUSED(g);
    int h = w32_gp_font_height(font);
    if (h < 0 || !height)
        return InvalidParameter;
    *height = (REAL)h;
    return Ok;
}

static int WINAPI GdipGetFontHeightGivenDPI(void *font, REAL dpi, REAL *height)
{
    UNUSED(dpi);
    return GdipGetFontHeight(font, NULL, height);
}

static int WINAPI GdipDrawString(void *g, const WCHAR *text, int len, void *font, const REAL *layout, void *format,
                                 void *brush)
{
    if (!layout)
        return InvalidParameter;
    int box[4] = {R(layout[0]), R(layout[1]), R(layout[2]), R(layout[3])};
    return w32_gp_draw_text(g, text, len, font, box, format, brush);
}

static int WINAPI GdipMeasureString(void *g, const WCHAR *text, int len, void *font, const REAL *layout, void *format,
                                    REAL *box, int *chars, int *lines)
{
    UNUSED(g);
    UNUSED(format);
    int w, h, lh = w32_gp_font_height(font);
    if (!layout || !box)
        return InvalidParameter;
    int r = w32_gp_measure(text, len, font, R(layout[2]), &w, &h);
    if (r != Ok)
        return r;
    box[0] = layout[0];
    box[1] = layout[1];
    box[2] = (REAL)w;
    box[3] = (REAL)h;
    if (chars)
        *chars = len < 0 ? (int)w32_wcslen(text) : len;
    if (lines)
        *lines = lh > 0 ? h / lh : 1;
    return Ok;
}

static int WINAPI GdipMeasureCharacterRanges(void *g, const WCHAR *text, int len, void *font, const REAL *layout,
                                             void *format, int count, void **regions)
{
    UNUSED(g);
    int(*ranges)[2];
    int n = w32_gp_ranges(format, &ranges), h = w32_gp_font_height(font);
    if (n < 0 || h < 0 || !layout || !regions || count < n)
        return InvalidParameter;
    for (int i = 0; i < n; i++) {
        int x, w, r = w32_gp_text_span(text, len, font, ranges[i][0], ranges[i][1], &x, &w);
        if (r == Ok)
            r = w32_gp_region_set(regions[i], R(layout[0]) + x, R(layout[1]), w, h, false);
        if (r != Ok)
            return r;
    }
    return Ok;
}

/* matrices hold scale and offset; rotation and shear are dropped */
static int WINAPI GdipCreateMatrix2(REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy, void **out)
{
    UNUSED(m12);
    UNUSED(m21);
    return w32_gp_matrix_new(FX(m11), FX(m22), R(dx), R(dy), out);
}

static int WINAPI GdipSetMatrixElements(void *m, REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy)
{
    UNUSED(m12);
    UNUSED(m21);
    return w32_gp_matrix_set(m, (const int[]){FX(m11), FX(m22), R(dx), R(dy)});
}

static int WINAPI GdipGetMatrixElements(void *m, REAL *e)
{
    int v[4];
    int r = e ? w32_gp_matrix_get(m, v) : InvalidParameter;
    if (r == Ok) {
        e[0] = (REAL)v[0] / ONE;
        e[1] = e[2] = 0;
        e[3] = (REAL)v[1] / ONE;
        e[4] = (REAL)v[2];
        e[5] = (REAL)v[3];
    }
    return r;
}

static int WINAPI GdipTranslateMatrix(void *m, REAL dx, REAL dy, int order)
{
    int v[4];
    int r = w32_gp_matrix_get(m, v);
    if (r != Ok)
        return r;
    if (order == 1) {
        v[2] += R(dx);
        v[3] += R(dy);
    } else {
        v[2] += R(dx * (REAL)v[0] / ONE);
        v[3] += R(dy * (REAL)v[1] / ONE);
    }
    return w32_gp_matrix_set(m, v);
}

static int WINAPI GdipScaleMatrix(void *m, REAL sx, REAL sy, int order)
{
    int v[4];
    int r = w32_gp_matrix_get(m, v);
    if (r != Ok)
        return r;
    v[0] = (int)((REAL)v[0] * sx);
    v[1] = (int)((REAL)v[1] * sy);
    if (order == 1) {
        v[2] = R((REAL)v[2] * sx);
        v[3] = R((REAL)v[3] * sy);
    }
    return w32_gp_matrix_set(m, v);
}

static int WINAPI GdipRotateMatrix(void *m, REAL angle, int order)
{
    int a = R(angle) % 360;
    return a == 180 || a == -180 ? GdipScaleMatrix(m, -1, -1, order) : GdipScaleMatrix(m, 1, 1, order);
}

static int WINAPI GdipInvertMatrix(void *m)
{
    int v[4];
    int r = w32_gp_matrix_get(m, v);
    if (r != Ok)
        return r;
    if (!v[0] || !v[1])
        return InvalidParameter;
    REAL sx = (REAL)v[0] / ONE, sy = (REAL)v[1] / ONE;
    return w32_gp_matrix_set(m, (const int[]){FX(1 / sx), FX(1 / sy), R(-(REAL)v[2] / sx), R(-(REAL)v[3] / sy)});
}

static int WINAPI GdipTransformMatrixPoints(void *m, REAL *pts, int n)
{
    int v[4];
    int r = pts ? w32_gp_matrix_get(m, v) : InvalidParameter;
    for (int i = 0; r == Ok && i < n; i++) {
        pts[i * 2] = pts[i * 2] * (REAL)v[0] / ONE + (REAL)v[2];
        pts[i * 2 + 1] = pts[i * 2 + 1] * (REAL)v[1] / ONE + (REAL)v[3];
    }
    return r;
}

static int WINAPI GdipMultiplyWorldTransform(void *g, void *m, int order)
{
    int v[4];
    int r = w32_gp_matrix_get(m, v);
    return r == Ok ? w32_gp_transform(g, v[2], v[3], v[0], v[1], order) : r;
}

static const struct w32_export gdiplus_fp_exports[] = {
    W32_FN(GdipDrawLine), W32_FN(GdipDrawLines), W32_FN(GdipDrawPolygon), W32_FN(GdipFillPolygon),
    W32_FN(GdipFillPolygon2), W32_FN(GdipDrawRectangle), W32_FN(GdipFillRectangle), W32_FN(GdipFillRectangles),
    W32_FN(GdipFillEllipse), W32_FN(GdipDrawEllipse), W32_FN(GdipDrawImage), W32_FN(GdipDrawImageRect),
    W32_FN(GdipDrawImageRectRect), W32_FN(GdipDrawImagePointRect), W32_FN(GdipTranslateWorldTransform),
    W32_FN(GdipScaleWorldTransform), W32_FN(GdipRotateWorldTransform), W32_FN(GdipSetClipRect),
    W32_FN(GdipGetClipBounds), W32_AS("GdipGetVisibleClipBounds", GdipGetClipBounds), W32_FN(GdipGetRegionBounds),
    W32_FN(GdipCreateRegionRect), W32_FN(GdipAddPathRectangle), W32_FN(GdipAddPathLine), W32_FN(GdipAddPathEllipse), W32_FN(GdipAddPathPolygon),
    W32_FN(GdipAddPathArc), W32_FN(GdipAddPathArcI), W32_FN(GdipDrawArcI), W32_FN(GdipDrawArc), W32_FN(GdipAddPathBezier),
    W32_FN(GdipDrawBezier),
    W32_FN(GdipCreateLineBrush), W32_FN(GdipCreateLineBrushFromRect), W32_FN(GdipCreatePen1), W32_FN(GdipCreatePen2),
    W32_FN(GdipSetPenWidth), W32_FN(GdipGetPenWidth), W32_FN(GdipGetImageHorizontalResolution),
    W32_AS("GdipGetImageVerticalResolution", GdipGetImageHorizontalResolution), W32_FN(GdipBitmapSetResolution),
    W32_FN(GdipGetImageDimension), W32_FN(GdipGetImageBounds), W32_FN(GdipCreateFont), W32_FN(GdipGetFontSize),
    W32_FN(GdipGetFontHeight), W32_FN(GdipGetFontHeightGivenDPI), W32_FN(GdipDrawString), W32_FN(GdipMeasureString),
    W32_FN(GdipMeasureCharacterRanges), W32_FN(GdipCreateMatrix2), W32_FN(GdipSetMatrixElements),
    W32_FN(GdipGetMatrixElements), W32_FN(GdipTranslateMatrix), W32_FN(GdipScaleMatrix), W32_FN(GdipRotateMatrix),
    W32_FN(GdipInvertMatrix), W32_FN(GdipTransformMatrixPoints), W32_FN(GdipMultiplyWorldTransform),
};

W32_EXPORTS(gdiplus_fp_module, "gdiplus.dll", gdiplus_fp_exports);
#endif
