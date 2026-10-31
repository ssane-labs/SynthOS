#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

/* Intel Volume Management Device. With "Intel RST", "RAID" or "VMD" chosen in the firmware, NVMe drives (and on
   some boards the SATA controller) sit behind the VMD on a PCI segment of their own: its configuration space is
   the VMD's BAR0 (1 MiB per bus), their registers live in its memory window (BAR2). The segment is set up from
   scratch here: bus numbers for the root ports, addresses for the controllers behind them. Interrupts would come
   through the VMD's own MSI-X table; the storage drivers poll instead. */

#define MAX_BUSES 16
#define VMD_CFGBAR 0x10
#define VMD_MEMBAR1 0x18
#define VMD_VMCAP 0x40
#define VMD_VMCONFIG 0x44

static const uint16_t vmd_ids[] = {0x201d, 0x28c0, 0x467f, 0x4c3d, 0x7d0b, 0x9a0b, 0xa77f, 0xad0b, 0xb60b, 0xb06f};
/* Laptop and desktop VMDs may start their buses at 128 or 224 (VMCONFIG says which). */
static const uint16_t restricted_ids[] = {0x467f, 0x4c3d, 0x7d0b, 0x9a0b, 0xa77f, 0xad0b, 0xb60b, 0xb06f};

struct vmd {
    volatile uint8_t *cfg;
    unsigned first_bus, next_bus;
    uint64_t mem, mem_end;
};

struct found {
    struct pci_device dev[16];
    int count;
};

static struct found found;
static bool scanned;

static bool listed(const uint16_t *ids, size_t n, uint16_t id)
{
    for (size_t i = 0; i < n; i++)
        if (ids[i] == id)
            return true;
    return false;
}

bool pci_is_vmd(const struct pci_device *d)
{
    return d->vendor == 0x8086 && d->class_code == 0x01 && d->subclass == 0x04 &&
           listed(vmd_ids, ARRAY_SIZE(vmd_ids), d->device);
}

static uint64_t bar_size(const struct pci_device *d, uint8_t offset, bool wide)
{
    uint32_t low = pci_read(d, offset), high = wide ? pci_read(d, (uint8_t)(offset + 4)) : 0;
    pci_write(d, offset, 0xffffffffu);
    if (wide)
        pci_write(d, (uint8_t)(offset + 4), 0xffffffffu);
    uint64_t mask = (pci_read(d, offset) & ~0xfu) | (wide ? (uint64_t)pci_read(d, (uint8_t)(offset + 4)) << 32 : 0xffffffff00000000ull);
    pci_write(d, offset, low);
    if (wide)
        pci_write(d, (uint8_t)(offset + 4), high);
    return mask == 0xfffffffffffffff0ull || !(mask & 0xfffffff0u) ? 0 : ~mask + 1;
}

/* Gives every memory BAR of a device an address in the VMD window. */
static void place_bars(struct vmd *v, const struct pci_device *d, int bars)
{
    pci_write(d, 0x04, pci_read(d, 0x04) & ~0x7u);
    for (int i = 0; i < bars; i++) {
        uint8_t off = (uint8_t)(0x10 + i * 4);
        uint32_t bar = pci_read(d, off);
        if (bar & 1)
            continue;
        bool wide = (bar & 6) == 4;
        uint64_t size = bar_size(d, off, wide);
        if (size) {
            uint64_t at = (v->mem + size - 1) & ~(size - 1);
            if (at + size > v->mem_end) {
                klog("vmd: no room for %02x:%02x.%u BAR%d", d->bus, d->dev, d->fn, i);
            } else {
                pci_write(d, off, (uint32_t)at | (bar & 0xfu));
                if (wide)
                    pci_write(d, (uint8_t)(off + 4), (uint32_t)(at >> 32));
                v->mem = at + size;
            }
        }
        if (wide)
            i++;
    }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);
}

static struct pci_device at(struct vmd *v, unsigned bus, uint8_t dev, uint8_t fn)
{
    struct pci_device d = {(uint8_t)bus, dev, fn, 0, 0, 0, 0, 0,
                           v->cfg + ((size_t)(bus - v->first_bus) << 20 | (size_t)dev << 15 | (size_t)fn << 12)};
    return d;
}

static void scan_bus(struct vmd *v, unsigned bus, int depth)
{
    for (uint8_t dev = 0; dev < 32; dev++)
        for (uint8_t fn = 0; fn < 8; fn++) {
            struct pci_device d = at(v, bus, dev, fn);
            uint32_t id = pci_read(&d, 0);
            if ((id & 0xffff) == 0xffff || id == 0) {
                if (fn == 0)
                    break;
                continue;
            }
            d.vendor = (uint16_t)id;
            d.device = (uint16_t)(id >> 16);
            uint32_t cls = pci_read(&d, 8);
            d.class_code = (uint8_t)(cls >> 24);
            d.subclass = (uint8_t)(cls >> 16);
            d.prog_if = (uint8_t)(cls >> 8);
            uint8_t header = (uint8_t)(pci_read(&d, 0x0c) >> 16);
            if ((header & 0x7f) == 1 && depth < 3 && v->next_bus + 1 < v->first_bus + MAX_BUSES) {
                unsigned secondary = ++v->next_bus;
                pci_write(&d, 0x18, (pci_read(&d, 0x18) & 0xff000000u) | (0xffu << 16) | secondary << 8 | bus);
                uint64_t start = v->mem = (v->mem + 0xfffff) & ~0xfffffull;
                place_bars(v, &d, 2);
                scan_bus(v, secondary, depth + 1);
                pci_write(&d, 0x18, (pci_read(&d, 0x18) & 0xff00ffffu) | v->next_bus << 16);
                uint64_t end = v->mem = (v->mem + 0xfffff) & ~0xfffffull;
                if (end == start) {
                    pci_write(&d, 0x20, 0x0000fff0u);
                    pci_write(&d, 0x24, 0x0000fff0u);
                } else if (end <= 0x100000000ull) {
                    pci_write(&d, 0x20, (uint32_t)((start >> 16) & 0xfff0) | (uint32_t)(((end - 1) >> 16) & 0xfff0) << 16);
                    pci_write(&d, 0x24, 0x0000fff0u);
                } else {
                    pci_write(&d, 0x20, 0x0000fff0u);
                    pci_write(&d, 0x24, (uint32_t)((start >> 16) & 0xfff0) | 1u | ((uint32_t)(((end - 1) >> 16) & 0xfff0) | 1u) << 16);
                    pci_write(&d, 0x28, (uint32_t)(start >> 32));
                    pci_write(&d, 0x2c, (uint32_t)((end - 1) >> 32));
                }
                pci_write(&d, 0x04, pci_read(&d, 0x04) | 0x06);
            } else if ((header & 0x7f) == 0) {
                place_bars(v, &d, 6);
                if (found.count < (int)ARRAY_SIZE(found.dev) && d.class_code == 0x01) {
                    found.dev[found.count++] = d;
                    klog("vmd: %04x:%04x (class %02x%02x) on bus %u", d.vendor, d.device, d.class_code, d.subclass,
                         bus);
                }
            }
            if (fn == 0 && !(header & 0x80))
                break;
        }
}

static void setup_vmd(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (!pci_is_vmd(d))
        return;
    uint32_t cmd = pci_read(d, 0x04);
    pci_write(d, 0x04, cmd & ~0x7u);
    uint64_t window = bar_size(d, VMD_MEMBAR1, (pci_read(d, VMD_MEMBAR1) & 6) == 4);
    pci_write(d, 0x04, cmd | 0x06);
    uint64_t cfg, mem;
    if (!window || !pci_bar_base(d, VMD_CFGBAR, &cfg) || !pci_bar_base(d, VMD_MEMBAR1, &mem)) {
        klog("vmd: %02x:%02x.%u has no windows", d->bus, d->dev, d->fn);
        return;
    }
    struct vmd v = {0};
    if (listed(restricted_ids, ARRAY_SIZE(restricted_ids), d->device) && (pci_read(d, VMD_VMCAP) & 1)) {
        unsigned sel = (pci_read(d, VMD_VMCONFIG) >> 8) & 3;
        v.first_bus = sel == 1 ? 128 : sel == 2 ? 224 : 0;
    }
    v.next_bus = v.first_bus;
    v.cfg = vmm_map_phys64(cfg, (size_t)MAX_BUSES << 20);
    v.mem = mem;
    v.mem_end = mem + window;
    if (!v.cfg || v.mem_end <= v.mem)
        return;
    klog("vmd: %04x at %02x:%02x.%u, buses from %u", d->device, d->bus, d->dev, d->fn, v.first_bus);
    scan_bus(&v, v.first_bus, 0);
}

/* Storage controllers: those on the ordinary PCI buses, then those behind a VMD. */
void pci_foreach_storage(void (*fn)(const struct pci_device *d, void *ctx), void *ctx)
{
    pci_foreach(fn, ctx);
    if (!scanned) {
        scanned = true;
        pci_foreach(setup_vmd, NULL);
    }
    for (int i = 0; i < found.count; i++)
        fn(&found.dev[i], ctx);
}
