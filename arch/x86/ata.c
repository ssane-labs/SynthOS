#include <synth/block.h>
#include <synth/error.h>
#include <synth/hal.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define ATA_TIMEOUT_MS 2000
#define DMA_SECTORS 128

enum {
    REG_DATA = 0,
    REG_ERROR = 1,
    REG_COUNT = 2,
    REG_LBA0 = 3,
    REG_LBA1 = 4,
    REG_LBA2 = 5,
    REG_DRIVE = 6,
    REG_STATUS = 7,
};

enum {
    ST_ERR = 0x01,
    ST_DRQ = 0x08,
    ST_DF = 0x20,
    ST_BSY = 0x80,
};

struct prd {
    uint32_t addr;
    uint16_t bytes;
    uint16_t flags;
};

struct ata_drive {
    uint16_t io;
    uint16_t ctrl;
    uint8_t slave;
    bool dma;
    uint16_t bm;
    struct prd *prd;
    uint8_t *buffer;
    struct blockdev dev;
};

static uint16_t bm_base;

static struct ata_drive drives[4] = {
    {.io = 0x1f0, .ctrl = 0x3f6, .slave = 0},
    {.io = 0x1f0, .ctrl = 0x3f6, .slave = 1},
    {.io = 0x170, .ctrl = 0x376, .slave = 0},
    {.io = 0x170, .ctrl = 0x376, .slave = 1},
};

static void delay(struct ata_drive *d)
{
    for (int i = 0; i < 4; i++)
        inb(d->ctrl);
}

static int wait_ready(struct ata_drive *d, bool need_drq)
{
    uint64_t end = hal_ticks_ms() + ATA_TIMEOUT_MS;
    for (;;) {
        uint8_t st = inb(d->io + REG_STATUS);
        if (!(st & ST_BSY)) {
            if (st & (ST_ERR | ST_DF))
                return E_IO;
            if (!need_drq || (st & ST_DRQ))
                return 0;
        }
        if (hal_ticks_ms() > end)
            return E_IO;
    }
}

static void select_lba(struct ata_drive *d, uint32_t lba, uint8_t count)
{
    outb(d->io + REG_DRIVE, (uint8_t)(0xe0 | (d->slave << 4) | ((lba >> 24) & 0x0f)));
    delay(d);
    outb(d->io + REG_COUNT, count);
    outb(d->io + REG_LBA0, (uint8_t)lba);
    outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
    outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
}

static int dma_transfer(struct ata_drive *d, uint32_t lba, uint32_t n, bool write)
{
    uint32_t bytes = n * BLOCK_SIZE, phys = vmm_phys(d->buffer);
    int k = 0;
    while (bytes) {
        uint32_t chunk = MIN(bytes, 0x10000 - (phys & 0xffff));
        d->prd[k++] = (struct prd){phys, (uint16_t)chunk, 0};
        phys += chunk;
        bytes -= chunk;
    }
    d->prd[k - 1].flags = 0x8000;
    int err = wait_ready(d, false);
    if (err)
        return err;
    outb(d->bm, 0);
    outl(d->bm + 4, vmm_phys(d->prd));
    outb(d->bm + 2, inb(d->bm + 2) | 0x06);
    outb(d->bm, write ? 0x00 : 0x08);
    select_lba(d, lba, (uint8_t)n);
    outb(d->io + REG_STATUS, write ? 0xca : 0xc8);
    outb(d->bm, write ? 0x01 : 0x09);
    uint64_t end = hal_ticks_ms() + ATA_TIMEOUT_MS;
    for (;;) {
        uint8_t bs = inb(d->bm + 2), st = inb(d->io + REG_STATUS);
        if (!(bs & 0x01) && !(st & ST_BSY))
            break;
        if ((bs & 0x02) || hal_ticks_ms() > end) {
            outb(d->bm, 0);
            return E_IO;
        }
    }
    outb(d->bm, 0);
    uint8_t bs = inb(d->bm + 2), st = inb(d->io + REG_STATUS);
    outb(d->bm + 2, bs | 0x06);
    return (bs & 0x02) || (st & (ST_ERR | ST_DF)) ? E_IO : 0;
}

static int ata_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_drive *d = dev->priv;
    uint16_t *p = buf;
    while (count && d->dma) {
        uint32_t n = MIN(count, (uint32_t)DMA_SECTORS);
        if (dma_transfer(d, lba, n, false)) {
            d->dma = false;
            break;
        }
        memcpy(p, d->buffer, n * BLOCK_SIZE);
        p += n * BLOCK_SIZE / 2;
        lba += n;
        count -= n;
    }
    while (count) {
        uint8_t n = (uint8_t)MIN(count, 128u);
        int err = wait_ready(d, false);
        if (err)
            return err;
        select_lba(d, lba, n);
        outb(d->io + REG_STATUS, 0x20);
        for (uint8_t s = 0; s < n; s++) {
            delay(d);
            if ((err = wait_ready(d, true)) != 0)
                return err;
            for (int i = 0; i < 256; i++)
                *p++ = inw(d->io + REG_DATA);
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    struct ata_drive *d = dev->priv;
    const uint16_t *p = buf;
    while (count && d->dma) {
        uint32_t n = MIN(count, (uint32_t)DMA_SECTORS);
        memcpy(d->buffer, p, n * BLOCK_SIZE);
        if (dma_transfer(d, lba, n, true)) {
            d->dma = false;
            break;
        }
        p += n * BLOCK_SIZE / 2;
        lba += n;
        count -= n;
    }
    while (count) {
        uint8_t n = (uint8_t)MIN(count, 128u);
        int err = wait_ready(d, false);
        if (err)
            return err;
        select_lba(d, lba, n);
        outb(d->io + REG_STATUS, 0x30);
        for (uint8_t s = 0; s < n; s++) {
            delay(d);
            if ((err = wait_ready(d, true)) != 0)
                return err;
            for (int i = 0; i < 256; i++)
                outw(d->io + REG_DATA, *p++);
        }
        outb(d->io + REG_STATUS, 0xe7);
        if ((err = wait_ready(d, false)) != 0)
            return err;
        lba += n;
        count -= n;
    }
    return 0;
}

static bool identify(struct ata_drive *d)
{
    if (inb(d->io + REG_STATUS) == 0xff)
        return false;
    outb(d->ctrl, 0x02);
    outb(d->io + REG_DRIVE, (uint8_t)(0xa0 | (d->slave << 4)));
    delay(d);
    outb(d->io + REG_COUNT, 0);
    outb(d->io + REG_LBA0, 0);
    outb(d->io + REG_LBA1, 0);
    outb(d->io + REG_LBA2, 0);
    outb(d->io + REG_STATUS, 0xec);
    delay(d);
    if (inb(d->io + REG_STATUS) == 0)
        return false;
    uint64_t end = hal_ticks_ms() + ATA_TIMEOUT_MS;
    while (inb(d->io + REG_STATUS) & ST_BSY)
        if (hal_ticks_ms() > end)
            return false;
    if (inb(d->io + REG_LBA1) || inb(d->io + REG_LBA2))
        return false;
    if (wait_ready(d, true) != 0)
        return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(d->io + REG_DATA);
    uint32_t sectors = (uint32_t)id[60] | (uint32_t)id[61] << 16;
    if (!sectors)
        return false;
    for (int i = 0; i < 20; i++) {
        d->dev.model[i * 2] = (char)(id[27 + i] >> 8);
        d->dev.model[i * 2 + 1] = (char)id[27 + i];
    }
    d->dev.model[40] = 0;
    str_trim(d->dev.model);
    d->dev.sectors = sectors;
    if (bm_base && (id[49] & (1u << 8)) && (d->io == 0x1f0 || d->io == 0x170)) {
        uint8_t *mem = kmalloc_pages(4096 + DMA_SECTORS * BLOCK_SIZE);
        if (mem) {
            d->prd = (struct prd *)mem;
            d->buffer = mem + 4096;
            d->bm = (uint16_t)(bm_base + (d->io == 0x170 ? 8 : 0));
            d->dma = true;
        }
    }
    return true;
}

static void find_busmaster(const struct pci_device *p, void *ctx)
{
    UNUSED(ctx);
    if (p->class_code != 0x01 || p->subclass != 0x01 || bm_base || !(p->prog_if & 0x80))
        return;
    uint32_t bar = pci_read(p, 0x20);
    if (!(bar & 1) || !(bar & ~3u))
        return;
    pci_write(p, 0x04, pci_read(p, 0x04) | 0x05);
    bm_base = (uint16_t)(bar & ~3u);
}

void ata_init(void)
{
    static const char *const names[] = {"hda", "hdb", "hdc", "hdd"};
    pci_foreach(find_busmaster, NULL);
    for (int i = 0; i < 4; i++) {
        struct ata_drive *d = &drives[i];
        if (!identify(d))
            continue;
        str_copy(d->dev.name, names[i], sizeof(d->dev.name));
        d->dev.read = ata_read;
        d->dev.write = ata_write;
        d->dev.priv = d;
        block_register(&d->dev);
    }
}
