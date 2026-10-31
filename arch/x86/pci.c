#include <synth/types.h>
#include <synth/string.h>

#include <synth/log.h>
#include <synth/vmm.h>

#include "io.h"

static uint32_t address(const struct pci_device *d, uint8_t offset)
{
    return 0x80000000u | (uint32_t)d->bus << 16 | (uint32_t)d->dev << 11 | (uint32_t)d->fn << 8 | (offset & 0xfc);
}

uint32_t pci_read(const struct pci_device *d, uint8_t offset)
{
    if (d->ecam)
        return *(volatile uint32_t *)(d->ecam + (offset & 0xfc));
    outl(0xcf8, address(d, offset));
    return inl(0xcfc);
}

void pci_write(const struct pci_device *d, uint8_t offset, uint32_t value)
{
    if (d->ecam) {
        *(volatile uint32_t *)(d->ecam + (offset & 0xfc)) = value;
        return;
    }
    outl(0xcf8, address(d, offset));
    outl(0xcfc, value);
}

void pci_foreach(void (*fn)(const struct pci_device *d, void *ctx), void *ctx)
{
    for (unsigned bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            for (uint8_t f = 0; f < 8; f++) {
                struct pci_device d = {(uint8_t)bus, dev, f, 0, 0, 0, 0, 0, NULL};
                uint32_t id = pci_read(&d, 0);
                if ((id & 0xffff) == 0xffff) {
                    if (f == 0)
                        break;
                    continue;
                }
                d.vendor = (uint16_t)id;
                d.device = (uint16_t)(id >> 16);
                uint32_t cls = pci_read(&d, 8);
                d.class_code = (uint8_t)(cls >> 24);
                d.subclass = (uint8_t)(cls >> 16);
                d.prog_if = (uint8_t)(cls >> 8);
                fn(&d, ctx);
                if (f == 0 && !(pci_read(&d, 0x0c) >> 16 & 0x80))
                    break;
            }
}

struct range_set {
    uint32_t base[96];
    uint32_t end[96];
    int count;
};

static void add_range(struct range_set *set, uint64_t base, uint64_t end)
{
    if (set->count < (int)ARRAY_SIZE(set->base) && base < 0x100000000ull && end > base) {
        set->base[set->count] = (uint32_t)base;
        set->end[set->count] = (uint32_t)MIN(end, 0xffffffffull);
        set->count++;
    }
}

static void collect_bars(const struct pci_device *d, void *ctx)
{
    struct range_set *set = ctx;
    uint8_t header = (uint8_t)(pci_read(d, 0x0c) >> 16 & 0x7f);
    int bars = header == 0 ? 6 : header == 1 ? 2 : 0;
    for (int i = 0; i < bars; i++) {
        uint32_t bar = pci_read(d, (uint8_t)(0x10 + i * 4));
        if (bar & 1)
            continue;
        bool wide = (bar & 6) == 4;
        uint32_t high = wide ? pci_read(d, (uint8_t)(0x14 + i * 4)) : 0;
        if (!high && (bar & ~0xfu))
            add_range(set, bar & ~0xfu, (uint64_t)(bar & ~0xfu) + 0x100000);
        if (wide)
            i++;
    }
}

bool pci_relocate_bar64(const struct pci_device *d, uint8_t offset, uint32_t *out)
{
    if (d->bus != 0 || d->ecam)
        return false;
    uint32_t cmd = pci_read(d, 0x04);
    pci_write(d, 0x04, cmd & ~0x6u);
    uint32_t low = pci_read(d, offset), high = pci_read(d, (uint8_t)(offset + 4));
    pci_write(d, offset, 0xffffffffu);
    pci_write(d, (uint8_t)(offset + 4), 0xffffffffu);
    uint32_t mask = pci_read(d, offset) & ~0xfu;
    pci_write(d, offset, low);
    pci_write(d, (uint8_t)(offset + 4), high);
    uint32_t size = ~mask + 1;
    if (!mask || size > 0x1000000u) {
        pci_write(d, 0x04, cmd);
        return false;
    }

    static struct range_set used;
    used.count = 0;
    const struct x86_bootinfo *bi = x86_boot_info();
    const struct e820_entry *map = (const struct e820_entry *)(uintptr_t)bi->mmap_addr;
    uint64_t ram_top = 0;
    for (uint32_t i = 0; i < bi->mmap_count; i++) {
        add_range(&used, map[i].base, map[i].base + map[i].length);
        if (map[i].base < 0x100000000ull)
            ram_top = MAX(ram_top, map[i].base + map[i].length);
    }
    if (bi->fb_addr)
        add_range(&used, bi->fb_addr, (uint64_t)bi->fb_addr + (uint64_t)bi->fb_pitch * bi->fb_height);
    add_range(&used, 0xfec00000u, 0x100000000ull);
    pci_foreach(collect_bars, &used);

    uint32_t floor = (uint32_t)MAX(ram_top, 0x80000000ull);
    for (uint64_t at = (0xfec00000u - size) & mask; at >= floor; at -= size) {
        bool clash = false;
        for (int i = 0; i < used.count && !clash; i++)
            clash = at < used.end[i] && at + size > used.base[i];
        if (clash)
            continue;
        pci_write(d, offset, (uint32_t)at | (low & 0xfu));
        pci_write(d, (uint8_t)(offset + 4), 0);
        pci_write(d, 0x04, cmd | 0x06);
        *out = (uint32_t)at;
        return true;
    }
    pci_write(d, 0x04, cmd);
    return false;
}

bool pci_bar_base(const struct pci_device *d, uint8_t offset, uint64_t *out)
{
    uint32_t low = pci_read(d, offset), high = (low & 6) == 4 ? pci_read(d, (uint8_t)(offset + 4)) : 0;
    uint32_t base = low & ~0xfu;
    if (high && !vmm_pae()) {
        if (!pci_relocate_bar64(d, offset, &base))
            return false;
        high = 0;
    }
    *out = (uint64_t)high << 32 | base;
    if (high)
        klog("pci: %02x:%02x.%u registers at %08x%08x (above 4 GB)", d->bus, d->dev, d->fn, (unsigned)high,
             (unsigned)base);
    return *out != 0;
}
