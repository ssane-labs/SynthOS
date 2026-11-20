#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"
#include "virtio_gpu.h"

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
    K_VIRTIO,
};

struct output {
    enum kind kind;
    char name[40];
    struct pci_device pci;
    volatile uint16_t *regs;
    uint32_t fb_phys, vram;
    uint8_t *fb_virt;
    bool primary, active;
    int more;
    uint16_t boot_w, boot_h;
    struct framebuffer fb;
#ifdef __x86_64__
    struct virtio_screen vs;
#endif
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

#ifndef CONFIG_SERIAL_CONSOLE
static void add_output(const struct output *o)
{
    if (o->primary) {
        memmove(&outputs[1], &outputs[0], sizeof(outputs[0]) * (size_t)output_count);
        outputs[0] = *o;
    } else {
        outputs[output_count] = *o;
    }
    output_count++;
}

static void found_pci(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
#ifdef __x86_64__
    int scanouts;
    uint16_t pw, ph;
    if (d->vendor == 0x1af4 && d->device == 0x1050 && output_count < MAX_OUTPUTS && virtio_gpu_probe(d, &scanouts, &pw, &ph)) {
        for (int i = 0; i < scanouts && output_count < MAX_OUTPUTS; i++) {
            struct output o = {.kind = K_VIRTIO, .pci = *d, .primary = i == 0 && d->subclass == 0x00};
            o.vs.gpu = virtio_gpu_last();
            o.vs.scanout = i;
            virtio_gpu_preferred(o.vs.gpu, i, &o.boot_w, &o.boot_h);
            o.vram = 64u << 20;
            ksnprintf(o.name, sizeof(o.name), scanouts > 1 ? "Virtio GPU display %d" : "Virtio GPU", i + 1);
            add_output(&o);
        }
        return;
    }
#endif
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
    add_output(&o);
}
#endif

static void probe(void)
{
    if (probed)
        return;
    probed = true;
#ifdef CONFIG_SERIAL_CONSOLE
    return;
#else
    if (!hal_safe_mode())
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
    for (int i = 0; is_uefi() && i < 3 && output_count < MAX_OUTPUTS; i++)
        if (uefi_more_framebuffer(i)) {
            struct output m = {.kind = K_GOP, .more = i + 1};
            ksnprintf(m.name, sizeof(m.name), "UEFI display %d", i + 2);
            outputs[output_count++] = m;
        }
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
        const struct framebuffer *fb = o->more ? uefi_more_framebuffer(o->more - 1) : uefi_framebuffer();
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
        o->fb_virt = vmm_map_framebuffer(o->fb_phys, MIN(o->vram, MAX_MAP));
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
    case K_GOP: {
        const struct framebuffer *g = o->more ? uefi_more_framebuffer(o->more - 1) : uefi_framebuffer();
        if (g) {
            *fb = *g;
            ok = true;
        }
        break;
    }
    case K_VIRTIO: {
#ifdef __x86_64__
        int w = width ? width : o->boot_w >= 640 ? o->boot_w : 1024, h = height ? height : o->boot_h >= 480 ? o->boot_h : 768;
        ok = w >= 320 && h >= 200 && w <= 4096 && h <= 4096 && virtio_gpu_set(o->vs.gpu, o->vs.scanout, w, h, &o->vs, fb);
        if (!ok && (width || height))
            ok = virtio_gpu_set(o->vs.gpu, o->vs.scanout, 1024, 768, &o->vs, fb);
#endif
        break;
    }
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
        if (!o->more)
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

/* Shows what was drawn into a display's framebuffer, where the display needs telling (virtio-gpu) */
void hal_screen_present(const struct framebuffer *fb, int x, int y, int w, int h)
{
#ifdef __x86_64__
    for (int i = 0; i < output_count; i++)
        if (outputs[i].kind == K_VIRTIO && outputs[i].active && outputs[i].vs.pixels == fb->base)
            virtio_gpu_present(&outputs[i].vs, x, y, w, h);
#else
    UNUSED(fb), UNUSED(x), UNUSED(y), UNUSED(w), UNUSED(h);
#endif
}

bool hal_screen_show_host(int gpu, uint32_t id, int w, int h)
{
#ifdef __x86_64__
    for (int i = 0; i < output_count; i++)
        if (outputs[i].kind == K_VIRTIO && outputs[i].active && outputs[i].vs.gpu == gpu && outputs[i].vs.resource) {
            virtio_gpu_scanout_3d(&outputs[i].vs, id, w, h);
            return true;
        }
#else
    UNUSED(gpu), UNUSED(id), UNUSED(w), UNUSED(h);
#endif
    return false;
}

bool hal_cursor_image(const struct framebuffer *fb, const uint32_t *argb, int w, int h, int hot_x, int hot_y, int x,
                      int y)
{
#ifdef __x86_64__
    for (int i = 0; i < output_count; i++)
        if (outputs[i].kind == K_VIRTIO && outputs[i].active && outputs[i].vs.pixels == fb->base)
            return virtio_gpu_cursor_image(outputs[i].vs.gpu, outputs[i].vs.scanout, argb, w, h, hot_x, hot_y, x, y);
#else
    UNUSED(fb), UNUSED(argb), UNUSED(w), UNUSED(h), UNUSED(hot_x), UNUSED(hot_y), UNUSED(x), UNUSED(y);
#endif
    return false;
}

bool hal_cursor_move(const struct framebuffer *fb, int x, int y, bool show)
{
#ifdef __x86_64__
    for (int i = 0; i < output_count; i++)
        if (outputs[i].kind == K_VIRTIO && outputs[i].active && outputs[i].vs.pixels == fb->base)
            return virtio_gpu_cursor_move(outputs[i].vs.gpu, outputs[i].vs.scanout, x, y, show);
#else
    UNUSED(fb), UNUSED(x), UNUSED(y), UNUSED(show);
#endif
    return false;
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

/* ---- sleep in S3: the displays lose their mode; those this driver sets itself are set again */

bool display_can_sleep(char *why, size_t cap)
{
    probe();
    for (int i = 0; i < output_count; i++)
        if (outputs[i].kind == K_GOP || outputs[i].kind == K_VIRTIO) {
            ksnprintf(why, cap, "the %s cannot be set up again after sleep", outputs[i].name);
            return false;
        }
    return true;
}

void display_resume(void)
{
    bool any = false;
    for (int i = 0; i < output_count; i++) {
        struct output *o = &outputs[i];
        struct framebuffer fb;
        if (!o->active)
            continue;
        any |= o->primary;
        if (o->kind == K_BOCHS && !bochs_set(o, (int)o->fb.width, (int)o->fb.height, &fb))
            klog("display: %s did not come back", o->name);
        if (o->kind == K_VBE && !vbe_enter((int)o->fb.width, (int)o->fb.height, &fb))
            klog("display: %s did not come back", o->name);
    }
    if (any || !output_count)
        return;
    /* the text console: on the firmware's framebuffer (UEFI) or in VGA text mode */
    if (is_uefi() && outputs[0].kind == K_BOCHS) {
        struct framebuffer fb;
        bochs_set(&outputs[0], outputs[0].boot_w, outputs[0].boot_h, &fb);
        fbcon_invalidate();
    } else if (!is_uefi()) {
        vbe_leave();
    }
}

/* The backlight of a laptop's built-in panel on Intel graphics: the PWM the firmware set up is left running at its
   frequency and only its duty cycle changes. The registers moved over the generations: GM45 and Pineview keep period
   and duty in one register of the display engine (0x61254), Bay Trail and Cherry Trail in the same one 0x180000
   further on, Ironlake to Ivy Bridge the period in the PCH (0xc8254) and the duty in the CPU (0x48254), Haswell to
   Kaby Lake both halves in 0xc8254, Broxton and Cannon Point onwards in two registers (0xc8254, 0xc8258). Without
   Intel graphics, or when the firmware left the PWM off, ACPI's _BCM sets the level (acpi.c). */
static uint64_t intel_base;
static uint16_t intel_id;
static bool intel_probed;
static struct {
    uint32_t page;
    volatile uint32_t *map;
} intel_pages[4];

static void find_intel(const struct pci_device *d, void *ctx)
{
    (void)ctx;
    uint64_t base;
    if (intel_base || d->vendor != 0x8086 || d->class_code != 0x03 || !pci_bar_base(d, 0x10, &base) || !base)
        return;
    intel_base = base;
    intel_id = d->device;
}

static volatile uint32_t *intel_reg(uint32_t offset)
{
    uint32_t page = offset & ~0xfffu;
    for (size_t i = 0; i < ARRAY_SIZE(intel_pages); i++) {
        if (!intel_pages[i].map) {
            intel_pages[i].page = page;
            intel_pages[i].map = vmm_map_uncached(intel_base + page, 0x1000);
        }
        if (intel_pages[i].page == page)
            return intel_pages[i].map ? intel_pages[i].map + (offset & 0xfff) / 4 : NULL;
    }
    return NULL;
}

/* period and duty in the halves of one register (GM45, Bay Trail, Haswell to Kaby Lake) */
static bool halves(uint32_t offset, int percent, int shift)
{
    volatile uint32_t *r = intel_reg(offset);
    if (!r)
        return false;
    uint32_t v = *r, max = v >> (16 + shift);
    if (!max || (shift && (v & 0x10000))) /* (legacy mode also goes through PCI configuration space) */
        return false;
    uint32_t duty = max * (uint32_t)percent / 100;
    *r = (v & ~(0xffffu >> shift << shift)) | duty << shift;
    return true;
}

static bool intel_backlight(int percent)
{
    if (!intel_probed) {
        intel_probed = true;
        pci_foreach(find_intel, NULL);
    }
    if (!intel_base)
        return false;
    uint16_t id = intel_id;
    if (id == 0x2a02 || id == 0x2a12 || id == 0x2a42) /* GM965, GME965, GM45 */
        return halves(0x61254, percent, 0);
    if (id == 0xa001 || id == 0xa011) /* Pineview: the duty in bits 15..1 */
        return halves(0x61254, percent, 1);
    if ((id & 0xfffc) == 0x0f30 || (id & 0xfffc) == 0x22b0) /* Bay Trail, Cherry Trail */
        return halves(0x180000 + 0x61254, percent, 0);
    volatile uint32_t *pch1 = intel_reg(0xc8250), *pch2 = intel_reg(0xc8254), *duty_reg = intel_reg(0xc8258);
    if (!pch1 || !pch2 || !duty_reg || !(*pch1 & 0x80000000u))
        return false;
    uint32_t ctl2 = *pch2, max = ctl2 >> 16;
    if (id == 0x0046 || (id & 0xff00) == 0x0100) { /* Ironlake, Sandy Bridge, Ivy Bridge */
        volatile uint32_t *cpu = intel_reg(0x48254);
        if (!max || !cpu)
            return false;
        *cpu = (*cpu & 0xffff0000u) | (max * (uint32_t)percent / 100);
        return true;
    }
    uint32_t duty = *duty_reg;
    if (max && (ctl2 & 0xffff) <= max && !duty) {
        *pch2 = (max << 16) | (max * (uint32_t)percent / 100);
        return true;
    }
    if (ctl2 && duty <= ctl2) {
        *duty_reg = (uint32_t)((uint64_t)ctl2 * (uint32_t)percent / 100);
        return true;
    }
    return false;
}

bool hal_backlight(int percent)
{
    percent = percent <= 0 ? 0 : percent < 5 ? 5 : percent > 100 ? 100 : percent;
    return intel_backlight(percent) || acpi_backlight(percent);
}
