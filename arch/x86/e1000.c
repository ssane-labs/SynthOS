#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/net.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define RX_COUNT 64
#define TX_COUNT 32
#define BUFFER 2048
#define MAX_NICS 2

enum {
    CTRL = 0x0000,
    STATUS = 0x0008,
    EERD = 0x0014,
    ICR = 0x00c0,
    IMC = 0x00d8,
    RCTL = 0x0100,
    TCTL = 0x0400,
    TIPG = 0x0410,
    RDBAL = 0x2800,
    RDBAH = 0x2804,
    RDLEN = 0x2808,
    RDH = 0x2810,
    RDT = 0x2818,
    TDBAL = 0x3800,
    TDBAH = 0x3804,
    TDLEN = 0x3808,
    TDH = 0x3810,
    TDT = 0x3818,
    MTA = 0x5200,
    RAL = 0x5400,
    RAH = 0x5404,
};

struct rx_desc {
    uint64_t addr;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    struct netdev dev;
    volatile uint8_t *regs;
    volatile struct rx_desc *rx;
    volatile struct tx_desc *tx;
    uint8_t *rx_buf, *tx_buf;
    int rx_next, tx_next;
    bool new_eeprom;
};

static struct e1000 nics[MAX_NICS];
static int nic_count;

static uint32_t rd(struct e1000 *n, uint32_t reg)
{
    return *(volatile uint32_t *)(n->regs + reg);
}

static void wr(struct e1000 *n, uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(n->regs + reg) = v;
}

static int e1000_send(struct netdev *dev, const void *frame, size_t length)
{
    struct e1000 *n = dev->priv;
    if (length > BUFFER)
        return E_RANGE;
    volatile struct tx_desc *d = &n->tx[n->tx_next];
    uint64_t end = hal_ticks_ms() + 50;
    while (d->cmd && !(d->status & 1))
        if (hal_ticks_ms() > end)
            return E_BUSY;
    memcpy(n->tx_buf + n->tx_next * BUFFER, frame, length);
    d->addr = vmm_phys(n->tx_buf + n->tx_next * BUFFER);
    d->length = (uint16_t)length;
    d->status = 0;
    d->cmd = 0x0b;
    n->tx_next = (n->tx_next + 1) % TX_COUNT;
    cpu_barrier();
    wr(n, TDT, (uint32_t)n->tx_next);
    return 0;
}

static int e1000_receive(struct netdev *dev, void *frame, size_t cap)
{
    struct e1000 *n = dev->priv;
    volatile struct rx_desc *d = &n->rx[n->rx_next];
    if (!(d->status & 1))
        return 0;
    int length = 0;
    if ((d->status & 2) && !d->errors && d->length <= cap) {
        length = d->length;
        memcpy(frame, n->rx_buf + n->rx_next * BUFFER, (size_t)length);
    }
    d->status = 0;
    int done = n->rx_next;
    n->rx_next = (n->rx_next + 1) % RX_COUNT;
    cpu_barrier();
    wr(n, RDT, (uint32_t)done);
    return length ? length : -1;
}

static bool e1000_link(struct netdev *dev)
{
    struct e1000 *n = dev->priv;
    return rd(n, STATUS) & 2;
}

static uint16_t eeprom(struct e1000 *n, uint8_t addr)
{
    wr(n, EERD, n->new_eeprom ? (1u | (uint32_t)addr << 2) : (1u | (uint32_t)addr << 8));
    uint32_t done = n->new_eeprom ? 2u : 0x10u;
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(n, EERD);
        if (v & done)
            return (uint16_t)(v >> 16);
    }
    return 0;
}

static bool setup(struct e1000 *n)
{
    wr(n, IMC, 0xffffffffu);
    wr(n, CTRL, rd(n, CTRL) | (1u << 26));
    hal_sleep_ms(10);
    for (int i = 0; i < 1000 && (rd(n, CTRL) & (1u << 26)); i++)
        hal_sleep_ms(1);
    wr(n, IMC, 0xffffffffu);
    rd(n, ICR);
    wr(n, CTRL, (rd(n, CTRL) | (1u << 6) | (1u << 5)) & ~((1u << 3) | (1u << 31) | (1u << 7)));

    uint32_t lo = rd(n, RAL), hi = rd(n, RAH);
    if (!(hi & 0x80000000u) || (!lo && !(hi & 0xffff))) {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom(n, (uint8_t)i);
            n->dev.mac[i * 2] = (uint8_t)w;
            n->dev.mac[i * 2 + 1] = (uint8_t)(w >> 8);
        }
        lo = (uint32_t)n->dev.mac[0] | (uint32_t)n->dev.mac[1] << 8 | (uint32_t)n->dev.mac[2] << 16 |
             (uint32_t)n->dev.mac[3] << 24;
        hi = (uint32_t)n->dev.mac[4] | (uint32_t)n->dev.mac[5] << 8;
        wr(n, RAL, lo);
        wr(n, RAH, hi | 0x80000000u);
    } else {
        for (int i = 0; i < 4; i++)
            n->dev.mac[i] = (uint8_t)(lo >> (8 * i));
        n->dev.mac[4] = (uint8_t)hi;
        n->dev.mac[5] = (uint8_t)(hi >> 8);
    }
    for (int i = 0; i < 128; i++)
        wr(n, MTA + (uint32_t)i * 4, 0);

    n->rx = kmalloc_pages(4096);
    n->tx = kmalloc_pages(4096);
    n->rx_buf = kmalloc_pages(RX_COUNT * BUFFER);
    n->tx_buf = kmalloc_pages(TX_COUNT * BUFFER);
    if (!n->rx || !n->tx || !n->rx_buf || !n->tx_buf)
        return false;
    memset((void *)n->rx, 0, 4096);
    memset((void *)n->tx, 0, 4096);
    for (int i = 0; i < RX_COUNT; i++)
        n->rx[i].addr = vmm_phys(n->rx_buf + i * BUFFER);
    wr(n, RDBAL, vmm_phys((const void *)n->rx));
    wr(n, RDBAH, 0);
    wr(n, RDLEN, RX_COUNT * sizeof(struct rx_desc));
    wr(n, RDH, 0);
    wr(n, RDT, RX_COUNT - 1);
    wr(n, RCTL, (1u << 1) | (1u << 15) | (1u << 26));
    wr(n, TDBAL, vmm_phys((const void *)n->tx));
    wr(n, TDBAH, 0);
    wr(n, TDLEN, TX_COUNT * sizeof(struct tx_desc));
    wr(n, TDH, 0);
    wr(n, TDT, 0);
    wr(n, TIPG, 0x0060200a);
    wr(n, TCTL, (1u << 1) | (1u << 3) | (0x0fu << 4) | (0x3fu << 12));
    return true;
}

static bool supported(uint16_t id, bool *new_eeprom)
{
    static const uint16_t old[] = {0x100e, 0x100f, 0x1004, 0x1011, 0x1010, 0x1012, 0x1015, 0x1016, 0x1017,
                                   0x101e, 0x1026, 0x1027, 0x1028, 0x1075, 0x1076, 0x1077, 0x1078, 0x1079,
                                   0x107a, 0x107b, 0x107c, 0x108a, 0x1099, 0x10b5};
    static const uint16_t fresh[] = {0x10d3, 0x10f6, 0x150c, 0x10ea, 0x10eb, 0x10ef, 0x10f0, 0x1502, 0x1503,
                                     0x153a, 0x153b, 0x155a, 0x1559, 0x15a0, 0x15a1, 0x15a2, 0x15a3, 0x156f,
                                     0x1570, 0x15b7, 0x15b8, 0x15b9, 0x15bb, 0x15bc, 0x15bd, 0x15be, 0x15d6,
                                     0x15d7, 0x15d8, 0x15e3, 0x0d4e, 0x0d4f, 0x0d4c, 0x0d4d, 0x0d53, 0x0d55,
                                     0x15f9, 0x15fa, 0x15fb, 0x15fc, 0x1a1c, 0x1a1d, 0x1a1e, 0x1a1f};
    for (size_t i = 0; i < ARRAY_SIZE(old); i++)
        if (old[i] == id) {
            *new_eeprom = false;
            return true;
        }
    for (size_t i = 0; i < ARRAY_SIZE(fresh); i++)
        if (fresh[i] == id) {
            *new_eeprom = true;
            return true;
        }
    return false;
}

static void found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    bool new_eeprom;
    if (d->vendor != 0x8086 || d->class_code != 0x02 || nic_count == MAX_NICS || !supported(d->device, &new_eeprom))
        return;
    uint32_t bar0 = pci_read(d, 0x10), base = bar0 & ~0xfu;
    if ((bar0 & 6) == 4 && pci_read(d, 0x14) && !pci_relocate_bar64(d, 0x10, &base)) {
        klog("e1000: registers above 4 GB are out of reach");
        return;
    }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);
    struct e1000 *n = &nics[nic_count];
    n->regs = vmm_map_phys(base, 0x20000);
    n->new_eeprom = new_eeprom;
    if (!n->regs || !setup(n)) {
        klog("e1000: device %04x did not initialize", d->device);
        return;
    }
    n->dev.driver = new_eeprom ? "e1000e" : "e1000";
    n->dev.send = e1000_send;
    n->dev.receive = e1000_receive;
    n->dev.link = e1000_link;
    n->dev.priv = n;
    nic_count++;
    net_register(&n->dev);
}

void e1000_init(void)
{
    pci_foreach(found, NULL);
}
