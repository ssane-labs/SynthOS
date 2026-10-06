#include <synth/block.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/string.h>

#include "io.h"

#define CHUNK_SECTORS (BIOS_BOUNCE_SIZE / BLOCK_SIZE)

struct biosdisk {
    struct blockdev dev;
    uint8_t drive;
    bool lba;
    uint32_t spt;
    uint32_t heads;
};

static struct biosdisk disk;

static bool transfer_lba(struct biosdisk *d, uint32_t lba, uint32_t count, bool write)
{
    uint8_t *dap = (uint8_t *)BIOS_SCRATCH;
    memset(dap, 0, 16);
    dap[0] = 16;
    dap[2] = (uint8_t)count;
    dap[6] = (uint8_t)((BIOS_BOUNCE >> 4) & 0xff);
    dap[7] = (uint8_t)(BIOS_BOUNCE >> 12);
    memcpy(dap + 8, &lba, 4);
    struct bios_regs r = {0};
    r.eax = write ? 0x4300 : 0x4200;
    r.edx = d->drive;
    r.esi = BIOS_SCRATCH;
    return bios_call(0x13, &r);
}

static bool transfer_chs(struct biosdisk *d, uint32_t lba, uint32_t count, bool write)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t l = lba + i;
        uint32_t sector = l % d->spt + 1, head = l / d->spt % d->heads, cyl = l / d->spt / d->heads;
        if (cyl > 1023)
            return false;
        struct bios_regs r = {0};
        r.eax = (write ? 0x0300u : 0x0200u) | 1u;
        r.ecx = (cyl & 0xff) << 8 | (cyl >> 2 & 0xc0) | sector;
        r.edx = head << 8 | d->drive;
        r.es = (uint16_t)((BIOS_BOUNCE + i * BLOCK_SIZE) >> 4);
        r.ebx = 0;
        if (!bios_call(0x13, &r))
            return false;
    }
    return true;
}

static bool transfer(struct biosdisk *d, uint32_t lba, uint32_t count, bool write)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (d->lba ? transfer_lba(d, lba, count, write) : transfer_chs(d, lba, count, write))
            return true;
        struct bios_regs r = {.edx = d->drive};
        bios_call(0x13, &r);
    }
    return false;
}

static int bd_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    struct biosdisk *d = dev->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, CHUNK_SECTORS);
        if (!transfer(d, lba, n, false))
            return E_IO;
        memcpy(out, (void *)BIOS_BOUNCE, n * BLOCK_SIZE);
        out += n * BLOCK_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int bd_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    struct biosdisk *d = dev->priv;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = MIN(count, CHUNK_SECTORS);
        memcpy((void *)BIOS_BOUNCE, in, n * BLOCK_SIZE);
        if (!transfer(d, lba, n, true))
            return E_IO;
        in += n * BLOCK_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

void biosdisk_init(uint8_t drive)
{
    if (!bios_available() || disk.dev.read)
        return;
    struct biosdisk *d = &disk;
    d->drive = drive;
    struct bios_regs r = {.eax = 0x4100, .ebx = 0x55aa, .edx = drive};
    d->lba = bios_call(0x13, &r) && (r.ebx & 0xffff) == 0xaa55 && (r.ecx & 1);
    if (d->lba) {
        uint8_t *params = (uint8_t *)BIOS_SCRATCH;
        memset(params, 0, 0x1e);
        params[0] = 0x1e;
        struct bios_regs p = {.eax = 0x4800, .edx = drive, .esi = BIOS_SCRATCH};
        uint64_t total = 0;
        if (bios_call(0x13, &p))
            memcpy(&total, params + 16, 8);
        d->dev.sectors = total > 0xffffffffu ? 0xffffffffu : (uint32_t)total;
    }
    struct bios_regs g = {.eax = 0x0800, .edx = drive};
    if (bios_call(0x13, &g)) {
        d->spt = g.ecx & 0x3f;
        d->heads = ((g.edx >> 8) & 0xff) + 1;
        uint32_t cyls = ((g.ecx >> 8) & 0xff) | ((g.ecx & 0xc0) << 2);
        if (!d->dev.sectors)
            d->dev.sectors = (cyls + 1) * d->heads * d->spt;
    }
    if (!d->dev.sectors || (!d->lba && !d->spt))
        return;
    ksnprintf(d->dev.name, sizeof(d->dev.name), "bios%u", (unsigned)(drive & 0x7f));
    ksnprintf(d->dev.model, sizeof(d->dev.model), "BIOS disk 0x%02x (%s)", drive, d->lba ? "LBA" : "CHS");
    d->dev.read = bd_read;
    d->dev.write = bd_write;
    d->dev.priv = d;
    block_register(&d->dev);
}
