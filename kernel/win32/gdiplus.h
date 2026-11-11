#ifndef W32_GDIPLUS_H
#define W32_GDIPLUS_H

#include "w32gui.h"

enum { GP_IMAGE = 1, GP_GRAPHICS, GP_BRUSH, GP_PEN, GP_FONT, GP_FAMILY, GP_FORMAT, GP_MATRIX, GP_ATTR, GP_REGION, GP_PATH };

typedef struct {
    uint32_t Data1;
    uint16_t Data2, Data3;
    uint8_t Data4[8];
} GUID;


int w32_gp_clone_area(int x, int y, int w, int h, uint32_t format, void *image, void **out);
bool w32_gp_image_size(void *image, int *w, int *h);
int w32_gp_fill_rect(void *graphics, void *brush, int x, int y, int w, int h);
int w32_gp_fill_poly(void *graphics, void *brush, const int *pts, int n);
int w32_gp_fill_ellipse(void *graphics, void *brush, int x, int y, int w, int h);
int w32_gp_draw_lines(void *graphics, void *pen, const int *pts, int n, bool closed);
int w32_gp_draw_rect(void *graphics, void *pen, int x, int y, int w, int h);
int w32_gp_draw_ellipse(void *graphics, void *pen, int x, int y, int w, int h);
int w32_gp_draw_image(void *graphics, void *image, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh);
int w32_gp_transform(void *graphics, int tx, int ty, int sx, int sy, int order);
int w32_gp_matrix_new(int sx, int sy, int tx, int ty, void **out);
int w32_gp_matrix_get(void *matrix, int *v);
int w32_gp_matrix_set(void *matrix, const int *v);
int w32_gp_clip(void *graphics, int x, int y, int w, int h, int mode);
int w32_gp_gradient(int x1, int y1, int x2, int y2, uint32_t c1, uint32_t c2, void **out);
int w32_gp_gradient_rect(const int *r, uint32_t c1, uint32_t c2, int mode, void **out);
int w32_gp_pen(uint32_t color, int width, void *brush, void **out);
int w32_gp_pen_width(void *pen, int width, int *get);
int w32_gp_font(void *family, int px, int style, void **out);
int w32_gp_font_px(void *font);
int w32_gp_measure(const WCHAR *text, int len, void *font, int width, int *w, int *h);
int w32_gp_draw_text(void *graphics, const WCHAR *text, int len, void *font, const int *box, void *format,
                     void *brush);
int w32_gp_region_rect(int x, int y, int w, int h, void **out);
int w32_gp_region_set(void *region, int x, int y, int w, int h, bool infinite);
int w32_gp_region_get(void *region, int *rect);
int w32_gp_path_add(void *path, const int *pts, int n, bool close);
int w32_gp_ranges(void *format, int (**ranges)[2]);
int w32_gp_text_span(const WCHAR *text, int len, void *font, int first, int count, int *x, int *w);
int w32_gp_font_height(void *font);
int w32_gp_clip_bounds(void *graphics, int *rect);
int w32_gp_image_res(void *image);
int w32_gp_path_ellipse(void *path, int x, int y, int w, int h);
int w32_gp_path_arc(void *path, int x, int y, int w, int h, int start, int sweep);
int w32_gp_arc_points(int x, int y, int w, int h, int start, int sweep, int *pts, int max);
int w32_gp_bezier_points(const int *c, int *pts);

#endif
