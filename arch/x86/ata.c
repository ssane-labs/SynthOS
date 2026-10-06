#include <synth/block.h>
#include <synth/error.h>
#include <synth/hal.h>
#include <synth/string.h>

#include "io.h"

#define ATA_TIMEOUT_MS 2000

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

struct ata_drive {
    uint16_t io;
    uint16_t ctrl;
    uint8_t slave;
    struct blockdev dev;
};

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

static int ata_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_drive *d = dev->priv;
    uint16_t *p = buf;
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
    return true;
}

void ata_init(void)
{
    static const char *const names[] = {"hda", "hdb", "hdc", "hdd"};
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
