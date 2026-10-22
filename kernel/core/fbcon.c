#include <synth/codepage.h>
#include <synth/fbcon.h>
#include <synth/font.h>
#include <synth/config.h>
#include <synth/heap.h>
#include <synth/string.h>

static struct framebuffer fb;
static struct display display;
static struct cell shown[CONFIG_MAX_COLS * CONFIG_MAX_ROWS];
static uint32_t colors[16];
static int scale = 1;
static int cursor_x = -1, cursor_y = -1;
static bool cursor_on;
static enum cursor_shape shape = CURSOR_UNDERLINE;

static uint32_t pixel(uint32_t rgb)
{
    uint32_t r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
    if (fb.bpp == 16)
        return (r >> 3) << fb.red_shift | (g >> 2) << fb.green_shift | (b >> 3) << fb.blue_shift;
    return r << fb.red_shift | g << fb.green_shift | b << fb.blue_shift;
}

static void put_row(uint8_t *line, int x0, uint8_t bits, uint32_t fg, uint32_t bg)
{
    for (int x = 0; x < FONT_WIDTH; x++) {
        uint32_t c = bits & (0x80 >> x) ? fg : bg;
        for (int s = 0; s < scale; s++) {
            int px = x0 + x * scale + s;
            if (fb.bpp == 32)
                ((uint32_t *)line)[px] = c;
            else if (fb.bpp == 24) {
                line[px * 3] = (uint8_t)c;
                line[px * 3 + 1] = (uint8_t)(c >> 8);
                line[px * 3 + 2] = (uint8_t)(c >> 16);
            } else {
                ((uint16_t *)line)[px] = (uint16_t)c;
            }
        }
    }
}

static void draw_cell(int cx, int cy, struct cell c, bool with_cursor)
{
    const uint8_t(*font)[16] = font_for_codepage(codepage_current());
    uint32_t fg = colors[c.attr & 0x0f], bg = colors[c.attr >> 4 & 0x0f];
    int x0 = cx * FONT_WIDTH * scale;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font[c.ch][row];
        if (with_cursor && (shape == CURSOR_BLOCK || (shape == CURSOR_UNDERLINE && row >= 13 && row <= 14) ||
                            (shape == CURSOR_BAR && row >= 0)))
            bits = shape == CURSOR_BAR ? (uint8_t)(bits | 0xc0) : (uint8_t)~bits;
        for (int s = 0; s < scale; s++) {
            uint8_t *line = fb.base + (size_t)((cy * FONT_HEIGHT + row) * scale + s) * fb.pitch;
            put_row(line, x0, bits, fg, bg);
        }
    }
}

static void fb_present(const struct cell *cells, int cols, int y0, int y1)
{
    for (int y = y0; y < y1 && y < display.rows; y++)
        for (int x = 0; x < cols && x < display.cols; x++) {
            struct cell c = cells[y * cols + x];
            struct cell *old = &shown[y * display.cols + x];
            if (old->ch == c.ch && old->attr == c.attr)
                continue;
            *old = c;
            draw_cell(x, y, c, cursor_on && x == cursor_x && y == cursor_y);
        }
}

static void fb_cursor(int x, int y, bool visible)
{
    if (cursor_x >= 0 && cursor_x < display.cols && cursor_y >= 0 && cursor_y < display.rows)
        draw_cell(cursor_x, cursor_y, shown[cursor_y * display.cols + cursor_x], false);
    cursor_x = x;
    cursor_y = y;
    cursor_on = visible;
    if (visible && x >= 0 && x < display.cols && y >= 0 && y < display.rows)
        draw_cell(x, y, shown[y * display.cols + x], true);
}

static void clear_margins(void)
{
    uint32_t used_w = (uint32_t)(display.cols * FONT_WIDTH * scale), used_h = (uint32_t)(display.rows * FONT_HEIGHT * scale);
    uint32_t bpp = fb.bpp / 8;
    for (uint32_t y = 0; y < fb.height; y++) {
        uint8_t *line = fb.base + (size_t)y * fb.pitch;
        for (uint32_t x = y < used_h ? used_w : 0; x < fb.width; x++)
            for (uint32_t b = 0; b < bpp; b++)
                line[x * bpp + b] = (uint8_t)(colors[0] >> (b * 8));
    }
}

static void redraw_all(void)
{
    clear_margins();
    for (int y = 0; y < display.rows; y++)
        for (int x = 0; x < display.cols; x++)
            draw_cell(x, y, shown[y * display.cols + x], cursor_on && x == cursor_x && y == cursor_y);
}

static void fb_palette(const uint32_t rgb[16])
{
    for (int i = 0; i < 16; i++)
        colors[i] = pixel(rgb[i]);
    if (display.cols)
        redraw_all();
}

static void fb_cursor_shape(enum cursor_shape s)
{
    shape = s;
    fb_cursor(cursor_x, cursor_y, cursor_on);
}

struct display *fbcon_create(const struct framebuffer *info)
{
    fb = *info;
    if (!fb.base || (fb.bpp != 32 && fb.bpp != 24 && fb.bpp != 16))
        return NULL;
    scale = fb.width >= 1600 && fb.height >= 1000 ? 2 : 1;
    display = (struct display){
        .name = "framebuffer",
        .cols = (int)MIN(fb.width / (FONT_WIDTH * (uint32_t)scale), (uint32_t)CONFIG_MAX_COLS),
        .rows = (int)MIN(fb.height / (FONT_HEIGHT * (uint32_t)scale), (uint32_t)CONFIG_MAX_ROWS),
        .caps = DISPLAY_GRID | DISPLAY_PALETTE | DISPLAY_CURSOR_SHAPE,
        .present = fb_present,
        .cursor = fb_cursor,
        .palette = fb_palette,
        .cursor_shape = fb_cursor_shape,
    };
    for (int i = 0; i < display.cols * display.rows; i++)
        shown[i] = (struct cell){' ', 0x07};
    for (int i = 0; i < 16; i++)
        colors[i] = pixel(i == 7 ? 0xaaaaaa : 0);
    redraw_all();
    return &display;
}

void fbcon_invalidate(void)
{
    for (int i = 0; i < display.cols * display.rows; i++)
        shown[i] = (struct cell){0xff, 0xff};
    clear_margins();
}

void fbcon_codepage_changed(void)
{
    if (display.cols)
        redraw_all();
}
