#include <synth/font.h>
#include <synth/hal.h>
#include <synth/string.h>

#include "io.h"

#define VGA_COLS 80
#define VGA_ROWS 25

static volatile uint16_t *const vram = (volatile uint16_t *)0xb8000;

static void crtc(uint8_t reg, uint8_t value)
{
    outb(0x3d4, reg);
    outb(0x3d5, value);
}

static void attribute(uint8_t reg, uint8_t value)
{
    inb(0x3da);
    outb(0x3c0, reg);
    outb(0x3c0, value);
}

static void vga_present(const struct cell *cells, int cols, int y0, int y1)
{
    for (int y = y0; y < y1 && y < VGA_ROWS; y++)
        for (int x = 0; x < cols && x < VGA_COLS; x++) {
            const struct cell *c = &cells[y * cols + x];
            vram[y * VGA_COLS + x] = (uint16_t)(c->attr << 8 | c->ch);
        }
}

static void vga_cursor(int x, int y, bool visible)
{
    uint16_t pos = (uint16_t)(y * VGA_COLS + x);
    crtc(0x0f, (uint8_t)(pos & 0xff));
    crtc(0x0e, (uint8_t)(pos >> 8));
    outb(0x3d4, 0x0a);
    uint8_t start = inb(0x3d5);
    crtc(0x0a, visible ? (start & ~0x20) : (start | 0x20));
}

static void vga_palette(const uint32_t rgb[16])
{
    for (int i = 0; i < 16; i++) {
        outb(0x3c8, (uint8_t)i);
        outb(0x3c9, (uint8_t)(((rgb[i] >> 16) & 0xff) >> 2));
        outb(0x3c9, (uint8_t)(((rgb[i] >> 8) & 0xff) >> 2));
        outb(0x3c9, (uint8_t)((rgb[i] & 0xff) >> 2));
    }
}

static void vga_cursor_shape(enum cursor_shape shape)
{
    static const uint8_t ranges[3][2] = {{0, 15}, {13, 14}, {0, 1}};
    crtc(0x0a, ranges[shape][0]);
    crtc(0x0b, ranges[shape][1]);
}

static struct display vga = {
    .name = "VGA text",
    .cols = VGA_COLS,
    .rows = VGA_ROWS,
    .caps = DISPLAY_GRID | DISPLAY_PALETTE | DISPLAY_CURSOR_SHAPE,
    .present = vga_present,
    .cursor = vga_cursor,
    .palette = vga_palette,
    .cursor_shape = vga_cursor_shape,
};

static void seq(uint8_t reg, uint8_t value)
{
    outb(0x3c4, reg);
    outb(0x3c5, value);
}

static void graphics(uint8_t reg, uint8_t value)
{
    outb(0x3ce, reg);
    outb(0x3cf, value);
}

void vga_load_font(int codepage)
{
    const uint8_t(*font)[16] = font_for_codepage(codepage);
    volatile uint8_t *plane = (volatile uint8_t *)0xa0000;
    seq(0x02, 0x04);
    seq(0x04, 0x07);
    graphics(0x04, 0x02);
    graphics(0x05, 0x00);
    graphics(0x06, 0x00);
    for (int ch = 0; ch < 256; ch++)
        for (int row = 0; row < 32; row++)
            plane[ch * 32 + row] = row < 16 ? font[ch][row] : 0;
    seq(0x02, 0x03);
    seq(0x04, 0x03);
    graphics(0x04, 0x00);
    graphics(0x05, 0x10);
    graphics(0x06, 0x0e);
}

void vga_init(void)
{
    vga_load_font(437);
    for (uint8_t i = 0; i < 16; i++)
        attribute(i, i);
    inb(0x3da);
    outb(0x3c0, 0x10);
    uint8_t mode = inb(0x3c1);
    attribute(0x10, mode & ~0x08);
    inb(0x3da);
    outb(0x3c0, 0x20);
}

struct display *vga_display(void)
{
    return &vga;
}
