#include <synth/fbcon.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define MAX_WIDTH 1280
#define MAX_HEIGHT 1024

struct mode_info {
    uint16_t attributes;
    uint8_t window_a, window_b;
    uint16_t granularity, window_size, segment_a, segment_b;
    uint32_t window_function;
    uint16_t pitch;
    uint16_t width, height;
    uint8_t char_w, char_h, planes, bpp, banks, memory_model, bank_size, image_pages, reserved0;
    uint8_t red_mask, red_position, green_mask, green_position, blue_mask, blue_position;
    uint8_t reserved_mask, reserved_position, direct_color;
    uint32_t framebuffer;
    uint32_t offscreen;
    uint16_t offscreen_size;
    uint16_t linear_pitch;
} __attribute__((packed));

static uint8_t *mapped_base;
static uint32_t mapped_phys;

static bool query_mode(uint16_t mode, struct mode_info *out)
{
    struct bios_regs r = {.eax = 0x4f01, .ecx = mode, .es = (uint16_t)(BIOS_BOUNCE >> 4), .edi = 0};
    if (!bios_call(0x10, &r) || (r.eax & 0xffff) != 0x004f)
        return false;
    memcpy(out, (void *)BIOS_BOUNCE, sizeof(*out));
    return true;
}

static int score(const struct mode_info *m)
{
    if ((m->attributes & 0x91) != 0x91 || m->memory_model != 6 || !m->framebuffer)
        return -1;
    if (m->bpp != 32 && m->bpp != 24 && m->bpp != 16)
        return -1;
    if (m->width > MAX_WIDTH || m->height > MAX_HEIGHT || m->width < 640 || m->height < 480)
        return -1;
    int depth = m->bpp == 32 ? 3 : m->bpp == 24 ? 2 : 1;
    int size = m->width == 1024 && m->height == 768 ? 4 : m->width * m->height / 300000;
    return depth * 16 + size;
}

bool vbe_enter(struct framebuffer *fb)
{
    bios_init();
    if (!bios_available())
        return false;
    uint8_t *info = (uint8_t *)BIOS_BOUNCE;
    memset(info, 0, 512);
    memcpy(info, "VBE2", 4);
    struct bios_regs r = {.eax = 0x4f00, .es = (uint16_t)(BIOS_BOUNCE >> 4), .edi = 0};
    if (!bios_call(0x10, &r) || (r.eax & 0xffff) != 0x004f || memcmp(info, "VESA", 4) != 0)
        return false;
    uint16_t list_off, list_seg;
    memcpy(&list_off, info + 14, 2);
    memcpy(&list_seg, info + 16, 2);
    uint16_t modes[128];
    int count = 0;
    const uint16_t *list = (const uint16_t *)(uintptr_t)((uint32_t)list_seg * 16 + list_off);
    while (count < (int)ARRAY_SIZE(modes) && list[count] != 0xffff)
        count++;
    memcpy(modes, list, (size_t)count * 2);

    int best = -1, best_score = -1;
    struct mode_info chosen = {0};
    for (int i = 0; i < count; i++) {
        struct mode_info m;
        if (!query_mode(modes[i], &m))
            continue;
        int s = score(&m);
        if (s > best_score) {
            best_score = s;
            best = modes[i];
            chosen = m;
        }
    }
    if (best < 0)
        return false;
    struct bios_regs set = {.eax = 0x4f02, .ebx = (uint32_t)best | 0x4000};
    if (!bios_call(0x10, &set) || (set.eax & 0xffff) != 0x004f)
        return false;
    uint32_t pitch = chosen.linear_pitch ? chosen.linear_pitch : chosen.pitch;
    if (mapped_phys != chosen.framebuffer) {
        mapped_base = vmm_map_phys(chosen.framebuffer, (size_t)pitch * chosen.height);
        mapped_phys = chosen.framebuffer;
    }
    if (!mapped_base) {
        vbe_leave();
        return false;
    }
    *fb = (struct framebuffer){
        .base = mapped_base,
        .width = chosen.width,
        .height = chosen.height,
        .pitch = pitch,
        .bpp = chosen.bpp,
        .red_shift = chosen.red_position,
        .green_shift = chosen.green_position,
        .blue_shift = chosen.blue_position,
    };
    klog("vbe: mode %03x %ux%ux%u", (unsigned)best, chosen.width, chosen.height, chosen.bpp);
    return true;
}

void vbe_leave(void)
{
    struct bios_regs r = {.eax = 0x0003};
    bios_call(0x10, &r);
    vga_init();
}
