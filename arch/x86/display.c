#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define MAX_OUTPUTS 4
#define DISPI_INDEX 0x1ce
#define DISPI_DATA 0x1cf
#define DISPI_ENABLED 0x01
#define DISPI_LFB 0x40
#define DISPI_NOCLEAR 0x80
#define MAX_MAP (16u << 20)

enum dispi_reg {
    D_ID,
    D_XRES,
    D_YRES,
    D_BPP,
    D_ENABLE,
    D_BANK,
    D_VIRT_W,
    D_VIRT_H,
    D_X_OFF,
    D_Y_OFF,
    D_VRAM,
};

enum kind {
    K_VBE,
    K_GOP,
    K_BOCHS,
};

struct output {
    enum kind kind;
    char name[40];
    struct pci_device pci;
    volatile uint16_t *regs;
    uint32_t fb_phys, vram;
    uint8_t *fb_virt;
    bool primary, active;
    uint16_t boot_w, boot_h;
    struct framebuffer fb;
};

static struct output outputs[MAX_OUTPUTS];
static int output_count;
static bool probed;

static const struct display_mode common[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800},  {1280, 1024},
    {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
    {2560, 1440}, {2560, 1600},
};

static uint16_t dispi_read(struct output *o, int reg)
{
    if (o->regs)
        return o->regs[reg];
    outw(DISPI_INDEX, (uint16_t)reg);
    return inw(DISPI_DATA);
}

static void dispi_write(struct output *o, int reg, uint16_t value)
{
    if (o->regs) {
        o->regs[reg] = value;
        return;
    }
    outw(DISPI_INDEX, (uint16_t)reg);
    outw(DISPI_DATA, value);
}

static bool is_uefi(void)
{
    return (x86_boot_info()->flags & BOOT_UEFI) != 0;
}

static void found_pci(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    bool bochs = d->vendor == 0x1234 && d->device == 0x1111;
    bool vbox = d->vendor == 0x80ee && d->device == 0xbeef;
    if ((!bochs && !vbox) || d->class_code != 0x03 || output_count >= MAX_OUTPUTS)
        return;
    struct output o = {.kind = K_BOCHS, .pci = *d};
    uint32_t bar0 = pci_read(d, 0x10), bar2 = pci_read(d, 0x18);

    o.fb_phys = bar0 & ~0xfu;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x2);
    o.primary = d->subclass == 0x00;
    if (bochs && !(bar2 & 1) && (bar2 & ~0xfu)) {
        uint8_t *mmio = vmm_map_phys(bar2 & ~0xfu, 4096);
        if (mmio)
            o.regs = (volatile uint16_t *)(mmio + 0x500);
    }
    if (!o.regs && !o.primary)
        return;
    uint16_t id = dispi_read(&o, D_ID);

    if (id < 0xb0c0 || id > 0xb0cf)
        return;
    o.vram = (uint32_t)dispi_read(&o, D_VRAM) * 65536u;
    if (!o.vram)
        o.vram = 16u << 20;
    o.boot_w = dispi_read(&o, D_XRES);
    o.boot_h = dispi_read(&o, D_YRES);
    ksnprintf(o.name, sizeof(o.name), "%s %s", vbox ? "VirtualBox" : "QEMU", o.primary ? "VGA" : "display");
    if (o.primary) {
        memmove(&outputs[1], &outputs[0], sizeof(outputs[0]) * (size_t)output_count);
        outputs[0] = o;
    } else {
        outputs[output_count] = o;
    }
    output_count++;
}

static void probe(void)
{
    if (probed)
        return;
    probed = true;
#ifdef CONFIG_SERIAL_CONSOLE
    return;
#else
    pci_foreach(found_pci, NULL);
    if (output_count && outputs[0].primary)
        return;
    struct output o = {0};
    if (is_uefi()) {
        if (!uefi_framebuffer())
            return;
        o.kind = K_GOP;
        str_copy(o.name, "UEFI framebuffer", sizeof(o.name));
    } else {
        o.kind = K_VBE;
        str_copy(o.name, "VESA BIOS display", sizeof(o.name));
    }
    o.primary = true;
    memmove(&outputs[1], &outputs[0], sizeof(outputs[0]) * (size_t)output_count);
    outputs[0] = o;
    output_count++;
#endif
}

int hal_screen_count(void)
{
    probe();
    return output_count;
}

const char *hal_screen_name(int index)
{
    probe();
    return index >= 0 && index < output_count ? outputs[index].name : "";
}

static uint32_t max_pixels(struct output *o)
{
    return MIN(o->vram, MAX_MAP) / 4;
}

int hal_screen_modes(int index, struct display_mode *out, int max)
{
    probe();
    if (index < 0 || index >= output_count)
        return 0;
    struct output *o = &outputs[index];
    if (o->kind == K_VBE)
        return vbe_modes(out, max);
    if (o->kind == K_GOP) {
        const struct framebuffer *fb = uefi_framebuffer();
        if (!fb || max < 1)
            return 0;
        out[0] = (struct display_mode){(uint16_t)fb->width, (uint16_t)fb->height};
        return 1;
    }
    int n = 0;
    for (size_t i = 0; i < ARRAY_SIZE(common) && n < max; i++)
        if ((uint32_t)common[i].width * common[i].height <= max_pixels(o))
            out[n++] = common[i];
    return n;
}

static bool bochs_set(struct output *o, int w, int h, struct framebuffer *fb)
{
    if (w < 320 || h < 200 || w > 4096 || h > 4096 || (uint32_t)w * (uint32_t)h > max_pixels(o))
        return false;
    if (!o->fb_virt) {
        o->fb_virt = vmm_map_phys(o->fb_phys, MIN(o->vram, MAX_MAP));
        if (!o->fb_virt)
            return false;
    }
    dispi_write(o, D_ENABLE, 0);
    dispi_write(o, D_XRES, (uint16_t)w);
    dispi_write(o, D_YRES, (uint16_t)h);
    dispi_write(o, D_BPP, 32);
    dispi_write(o, D_VIRT_W, (uint16_t)w);
    dispi_write(o, D_VIRT_H, (uint16_t)h);
    dispi_write(o, D_X_OFF, 0);
    dispi_write(o, D_Y_OFF, 0);
    dispi_write(o, D_ENABLE, DISPI_ENABLED | DISPI_LFB);
    if (o->regs) {
        volatile uint8_t *vga = (volatile uint8_t *)o->regs - 0x500 + 0x400;
        (void)vga[0x3da - 0x3c0];
        vga[0] = 0x20;
    }
    if (dispi_read(o, D_XRES) != w || dispi_read(o, D_YRES) != h)
        return false;
    *fb = (struct framebuffer){
        .base = o->fb_virt,
        .width = (uint32_t)w,
        .height = (uint32_t)h,
        .pitch = (uint32_t)w * 4,
        .bpp = 32,
        .red_shift = 16,
        .green_shift = 8,
        .blue_shift = 0,
    };
    klog("display: %s %dx%d", o->name, w, h);
    return true;
}

bool hal_screen_open(int index, int width, int height, struct framebuffer *fb)
{
    probe();
    if (index < 0 || index >= output_count)
        return false;
    struct output *o = &outputs[index];
    bool ok = false;
    switch (o->kind) {
    case K_VBE: ok = vbe_enter(width, height, fb); break;
    case K_GOP:
        if (uefi_framebuffer()) {
            *fb = *uefi_framebuffer();
            ok = true;
        }
        break;
    case K_BOCHS: {
        int w = width, h = height;
        if (!w || !bochs_set(o, w, h, fb)) {
            bool uefi_boot = is_uefi() && o->primary && o->boot_w >= 640;
            w = uefi_boot ? o->boot_w : 1024;
            h = uefi_boot ? o->boot_h : 768;
            ok = bochs_set(o, w, h, fb);
        } else {
            ok = true;
        }
        break;
    }
    }
    o->active = ok;
    if (ok)
        o->fb = *fb;
    return ok;
}

void hal_screen_close(int index)
{
    if (index < 0 || index >= output_count || !outputs[index].active)
        return;
    struct output *o = &outputs[index];
    o->active = false;
    if (o->kind == K_GOP) {
        fbcon_invalidate();
        return;
    }
    if (o->kind == K_VBE) {
        vbe_leave();
        return;
    }
    if (o->primary && is_uefi()) {
        struct framebuffer fb;
        bochs_set(o, o->boot_w, o->boot_h, &fb);
        fbcon_invalidate();
        return;
    }
    dispi_write(o, D_ENABLE, 0);
    if (o->primary)
        vbe_leave();
}

bool hal_graphics_enter(struct framebuffer *fb)
{
    return hal_screen_open(0, 0, 0, fb);
}

void hal_graphics_leave(void)
{
    for (int i = output_count - 1; i >= 0; i--)
        hal_screen_close(i);
}
