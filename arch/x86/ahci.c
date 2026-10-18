#include <synth/block.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define MAX_PORTS 8
#define BUFFER_SECTORS 256
#define TIMEOUT_MS 3000

enum {
    HBA_CAP = 0x00,
    HBA_GHC = 0x04,
    HBA_IS = 0x08,
    HBA_PI = 0x0c,
    PORT_BASE = 0x100,
    PORT_SIZE = 0x80,
    P_CLB = 0x00,
    P_CLBU = 0x04,
    P_FB = 0x08,
    P_FBU = 0x0c,
    P_IS = 0x10,
    P_IE = 0x14,
    P_CMD = 0x18,
    P_TFD = 0x20,
    P_SIG = 0x24,
    P_SSTS = 0x28,
    P_SERR = 0x30,
    P_CI = 0x38,
};

enum {
    CMD_ST = 1u << 0,
    CMD_FRE = 1u << 4,
    CMD_FR = 1u << 14,
    CMD_CR = 1u << 15,
    IS_TFES = 1u << 30,
    TFD_BUSY = 0x88,
};

struct cmd_header {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
};

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prdt_entry prdt[1];
};

struct ahci_port {
    struct blockdev dev;
    volatile uint8_t *regs;
    struct cmd_header *list;
    struct cmd_table *table;
    uint8_t *buffer;
    volatile uint8_t *hba;
    bool dirty;
    uint64_t flushed;
};

static struct ahci_port ports[MAX_PORTS];
static int port_count;
static bool use_irq;

static void ahci_irq(struct regs *r)
{
    UNUSED(r);
    for (int i = 0; i < port_count; i++) {
        volatile uint8_t *regs = ports[i].regs;
        *(volatile uint32_t *)(regs + P_IS) = *(volatile uint32_t *)(regs + P_IS) & ~IS_TFES;
        *(volatile uint32_t *)(ports[i].hba + HBA_IS) = *(volatile uint32_t *)(ports[i].hba + HBA_IS);
    }
}

static uint32_t rd(struct ahci_port *p, uint32_t reg)
{
    return *(volatile uint32_t *)(p->regs + reg);
}

static void wr(struct ahci_port *p, uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(p->regs + reg) = v;
}

static bool wait_clear(struct ahci_port *p, uint32_t reg, uint32_t mask)
{
    uint64_t end = hal_ticks_ms() + TIMEOUT_MS;
    while (rd(p, reg) & mask)
        if (hal_ticks_ms() > end)
            return false;
    return true;
}

static int command(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count, bool write)
{
    if (!wait_clear(p, P_TFD, TFD_BUSY))
        return E_IO;
    struct cmd_header *h = &p->list[0];
    memset(h, 0, sizeof(*h));
    h->flags = (uint16_t)(5 | (write ? 1u << 6 : 0));
    h->prdt_length = 1;
    h->ctba = vmm_phys(p->table);
    struct cmd_table *t = p->table;
    memset(t, 0, sizeof(*t));
    t->prdt[0].dba = vmm_phys(p->buffer);
    t->prdt[0].dbc = (count ? count * BLOCK_SIZE : 512) - 1;
    uint8_t *fis = t->cfis;
    fis[0] = 0x27;
    fis[1] = 0x80;
    fis[2] = cmd;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 1u << 6;
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
    wr(p, P_IS, 0xffffffffu);
    wr(p, P_CI, 1);
    uint64_t end = hal_ticks_ms() + TIMEOUT_MS;
    while (rd(p, P_CI) & 1) {
        if ((rd(p, P_IS) & IS_TFES) || hal_ticks_ms() > end)
            return E_IO;
        cpu_wait(use_irq);
    }
    return (rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & 1) ? E_IO : 0;
}

static int ahci_read(struct blockdev *dev, uint32_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = dev->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, (uint32_t)BUFFER_SECTORS);
        int err = command(p, 0x25, lba, n, false);
        if (err)
            return err;
        memcpy(out, p->buffer, n * BLOCK_SIZE);
        out += n * BLOCK_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(struct blockdev *dev, uint32_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *p = dev->priv;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = MIN(count, (uint32_t)BUFFER_SECTORS);
        memcpy(p->buffer, in, n * BLOCK_SIZE);
        int err = command(p, 0x35, lba, n, true);
        if (err)
            return err;
        in += n * BLOCK_SIZE;
        lba += n;
        count -= n;
    }
    /* A cache flush after every write makes a virtual machine's host sync its disk each time: once a second is
       enough, and ahci_flush_all runs before the power goes off. */
    p->dirty = true;
    if (hal_ticks_ms() - p->flushed >= 1000) {
        command(p, 0xea, 0, 0, false);
        p->dirty = false;
        p->flushed = hal_ticks_ms();
    }
    return 0;
}

void ahci_flush_all(void)
{
    for (int i = 0; i < port_count; i++)
        if (ports[i].dirty) {
            command(&ports[i], 0xea, 0, 0, false);
            ports[i].dirty = false;
        }
}

static bool start_port(struct ahci_port *p)
{
    wr(p, P_CMD, rd(p, P_CMD) & ~CMD_ST);
    if (!wait_clear(p, P_CMD, CMD_CR))
        return false;
    wr(p, P_CMD, rd(p, P_CMD) & ~CMD_FRE);
    if (!wait_clear(p, P_CMD, CMD_FR))
        return false;
    uint8_t *mem = kmalloc_pages(4096 + BUFFER_SECTORS * BLOCK_SIZE);
    if (!mem)
        return false;
    memset(mem, 0, 4096);
    p->list = (struct cmd_header *)mem;
    p->table = (struct cmd_table *)(mem + 2048);
    p->buffer = mem + 4096;
    wr(p, P_CLB, vmm_phys(p->list));
    wr(p, P_CLBU, 0);
    wr(p, P_FB, vmm_phys(mem + 1024));
    wr(p, P_FBU, 0);
    wr(p, P_SERR, 0xffffffffu);
    wr(p, P_IS, 0xffffffffu);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
    return true;
}

static void model_string(const uint16_t *id, char *out)
{
    for (int i = 0; i < 20; i++) {
        out[i * 2] = (char)(id[27 + i] >> 8);
        out[i * 2 + 1] = (char)id[27 + i];
    }
    out[40] = 0;
    for (int i = 39; i >= 0 && out[i] == ' '; i--)
        out[i] = 0;
}

static void probe_port(volatile uint8_t *hba, int index)
{
    if (port_count == MAX_PORTS)
        return;
    struct ahci_port *p = &ports[port_count];
    p->regs = hba + PORT_BASE + (uint32_t)index * PORT_SIZE;
    p->hba = hba;
    uint32_t ssts = rd(p, P_SSTS);
    if ((ssts & 0x0f) != 3 || ((ssts >> 8) & 0x0f) != 1 || rd(p, P_SIG) != 0x00000101u)
        return;
    if (!start_port(p) || command(p, 0xec, 0, 0, false) != 0)
        return;
    const uint16_t *id = (const uint16_t *)p->buffer;
    uint64_t sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32;
    if (!sectors)
        sectors = (uint32_t)id[60] | (uint32_t)id[61] << 16;
    p->dev.sectors = sectors > 0xffffffffu ? 0xffffffffu : (uint32_t)sectors;
    model_string(id, p->dev.model);
    ksnprintf(p->dev.name, sizeof(p->dev.name), "sata%d", port_count);
    p->dev.read = ahci_read;
    p->dev.write = ahci_write;
    p->dev.priv = p;
    block_register(&p->dev);
    port_count++;
}

static void probe_controller(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (d->class_code != 0x01 || d->subclass != 0x06 || d->prog_if != 0x01)
        return;
    uint32_t bar = pci_read(d, 0x24) & ~0xfu;
    if (!bar)
        return;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);
    volatile uint8_t *hba = vmm_map_phys(bar, 0x1100);
    if (!hba)
        return;
    *(volatile uint32_t *)(hba + HBA_GHC) |= 1u << 31;
    uint32_t implemented = *(volatile uint32_t *)(hba + HBA_PI);
    int first = port_count;
    for (int i = 0; i < 32; i++)
        if (implemented & (1u << i))
            probe_port(hba, i);
    uint8_t line = (uint8_t)pci_read(d, 0x3c);
    if (port_count > first && line > 1 && line < 16 && line != 12 && !use_irq) {
        irq_install(line, ahci_irq);
        for (int i = first; i < port_count; i++)
            wr(&ports[i], P_IE, IS_TFES | 0x0f);
        *(volatile uint32_t *)(hba + HBA_IS) = 0xffffffffu;
        *(volatile uint32_t *)(hba + HBA_GHC) |= 1u << 1;
        use_irq = true;
    }
}

void ahci_init(void)
{
    pci_foreach(probe_controller, NULL);
}
