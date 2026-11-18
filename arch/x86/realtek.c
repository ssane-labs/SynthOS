#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/net.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define MAX_NICS 2
#define RX8139_SIZE 8192
#define RING 64
#define BUFFER 2048

struct rtl8139 {
    struct netdev dev;
    uint16_t io;
    uint8_t *rx;
    uint8_t *tx[4];
    uint32_t rx_offset;
    int tx_next;
};

struct desc {
    uint32_t opts1, opts2, addr_lo, addr_hi;
};

/* The gigabit chips are reached through their memory BAR when the firmware gave them one: on many newer boards the
   bridges have no I/O window and the I/O BAR is left empty. */
struct rtl8169 {
    struct netdev dev;
    uint16_t io;
    volatile uint8_t *mmio;
    uint32_t xid;
    bool rtl8125, pll;
    volatile struct desc *rx, *tx;
    uint8_t *rx_buf, *tx_buf;
    int rx_next, tx_next;
};

static struct rtl8139 small[MAX_NICS];
static struct rtl8169 big[MAX_NICS];
static int small_count, big_count;

static uint8_t r8(struct rtl8169 *n, uint16_t reg)
{
    return n->mmio ? n->mmio[reg] : inb((uint16_t)(n->io + reg));
}

static uint32_t r32(struct rtl8169 *n, uint16_t reg)
{
    return n->mmio ? *(volatile uint32_t *)(n->mmio + reg) : inl((uint16_t)(n->io + reg));
}

static void w8(struct rtl8169 *n, uint16_t reg, uint8_t v)
{
    if (n->mmio)
        n->mmio[reg] = v;
    else
        outb((uint16_t)(n->io + reg), v);
}

static void w16(struct rtl8169 *n, uint16_t reg, uint16_t v)
{
    if (n->mmio)
        *(volatile uint16_t *)(n->mmio + reg) = v;
    else
        outw((uint16_t)(n->io + reg), v);
}

static void w32(struct rtl8169 *n, uint16_t reg, uint32_t v)
{
    if (n->mmio)
        *(volatile uint32_t *)(n->mmio + reg) = v;
    else
        outl((uint16_t)(n->io + reg), v);
}

/* PHY registers: through PHYAR on the older chips, through the PHY's OCP window (0xa400 + 2 * reg) from the
   RTL8168G on, the RTL8125 included. */
static void phy_write(struct rtl8169 *n, int reg, uint16_t value)
{
    bool ocp = n->xid >= 0x4c0;
    if (ocp)
        w32(n, 0xb8, 0x80000000u | (uint32_t)(0xa400 + reg * 2) << 15 | value);
    else
        w32(n, 0x60, 0x80000000u | (uint32_t)(reg & 0x1f) << 16 | value);
    for (int i = 0; i < 100; i++) {
        hal_sleep_ms(1);
        if (!(r32(n, ocp ? 0xb8 : 0x60) & 0x80000000u))
            break;
    }
}

static int rtl8139_send(struct netdev *dev, const void *frame, size_t length)
{
    struct rtl8139 *n = dev->priv;
    if (length > 1792)
        return E_RANGE;
    uint16_t tsd = (uint16_t)(n->io + 0x10 + n->tx_next * 4);
    uint64_t end = hal_ticks_ms() + 50;
    while (!(inl(tsd) & (1u << 13)))
        if (hal_ticks_ms() > end)
            return E_BUSY;
    memcpy(n->tx[n->tx_next], frame, length);
    outl((uint16_t)(n->io + 0x20 + n->tx_next * 4), vmm_phys(n->tx[n->tx_next]));
    outl(tsd, (uint32_t)length);
    n->tx_next = (n->tx_next + 1) % 4;
    return 0;
}

static int rtl8139_receive(struct netdev *dev, void *frame, size_t cap)
{
    struct rtl8139 *n = dev->priv;
    if (inb((uint16_t)(n->io + 0x37)) & 1)
        return 0;
    uint8_t *p = n->rx + n->rx_offset;
    uint16_t status = (uint16_t)(p[0] | p[1] << 8), length = (uint16_t)(p[2] | p[3] << 8);
    int result = -1;
    if ((status & 1) && length >= 4 && (size_t)(length - 4) <= cap) {
        memcpy(frame, p + 4, (size_t)(length - 4));
        result = length - 4;
    }
    n->rx_offset = (n->rx_offset + length + 4 + 3) & ~3u;
    n->rx_offset %= RX8139_SIZE;
    outw((uint16_t)(n->io + 0x38), (uint16_t)(n->rx_offset - 16));
    return result;
}

static bool rtl8139_link(struct netdev *dev)
{
    struct rtl8139 *n = dev->priv;
    return !(inb((uint16_t)(n->io + 0x58)) & 4);
}

static bool rtl8139_setup(struct rtl8139 *n)
{
    outb((uint16_t)(n->io + 0x52), 0);
    outb((uint16_t)(n->io + 0x37), 0x10);
    for (int i = 0; i < 1000 && (inb((uint16_t)(n->io + 0x37)) & 0x10); i++)
        hal_sleep_ms(1);
    n->rx = kmalloc_pages(RX8139_SIZE + 16 + 1536 + 4096);
    for (int i = 0; i < 4; i++)
        n->tx[i] = kmalloc_pages(4096);
    if (!n->rx || !n->tx[0] || !n->tx[1] || !n->tx[2] || !n->tx[3])
        return false;
    for (int i = 0; i < 6; i++)
        n->dev.mac[i] = inb((uint16_t)(n->io + i));
    outl((uint16_t)(n->io + 0x30), vmm_phys(n->rx));
    outw((uint16_t)(n->io + 0x3c), 0);
    outl((uint16_t)(n->io + 0x08), 0xffffffffu);
    outl((uint16_t)(n->io + 0x0c), 0xffffffffu);
    outl((uint16_t)(n->io + 0x44), 0x0000008eu | 7u << 13 | 7u << 8);
    outl((uint16_t)(n->io + 0x40), 0x03000600u);
    outb((uint16_t)(n->io + 0x37), 0x0c);
    n->rx_offset = 0;
    return true;
}

static int rtl8169_send(struct netdev *dev, const void *frame, size_t length)
{
    struct rtl8169 *n = dev->priv;
    if (length > BUFFER)
        return E_RANGE;
    volatile struct desc *d = &n->tx[n->tx_next];
    uint64_t end = hal_ticks_ms() + 50;
    while (d->opts1 & (1u << 31))
        if (hal_ticks_ms() > end)
            return E_BUSY;
    uint8_t *buf = n->tx_buf + n->tx_next * BUFFER;
    memcpy(buf, frame, length);
    d->addr_lo = vmm_phys(buf);
    d->addr_hi = 0;
    d->opts2 = 0;
    cpu_barrier();
    d->opts1 = (1u << 31) | (1u << 29) | (1u << 28) | (n->tx_next == RING - 1 ? 1u << 30 : 0) | (uint32_t)length;
    n->tx_next = (n->tx_next + 1) % RING;
    cpu_barrier();
    if (n->rtl8125)
        w16(n, 0x90, 1);
    else
        w8(n, 0x38, 0x40);
    return 0;
}

static void rearm(struct rtl8169 *n, int i)
{
    volatile struct desc *d = &n->rx[i];
    d->addr_lo = vmm_phys(n->rx_buf + i * BUFFER);
    d->addr_hi = 0;
    d->opts2 = 0;
    cpu_barrier();
    d->opts1 = (1u << 31) | (i == RING - 1 ? 1u << 30 : 0) | BUFFER;
}

static int rtl8169_receive(struct netdev *dev, void *frame, size_t cap)
{
    struct rtl8169 *n = dev->priv;
    volatile struct desc *d = &n->rx[n->rx_next];
    uint32_t opts = d->opts1;
    if (opts & (1u << 31))
        return 0;
    int result = -1;
    size_t length = opts & 0x3fff;
    if ((opts & (3u << 28)) == (3u << 28) && !(opts & (1u << 21)) && length >= 4 && length - 4 <= cap) {
        memcpy(frame, n->rx_buf + n->rx_next * BUFFER, length - 4);
        result = (int)(length - 4);
    }
    rearm(n, n->rx_next);
    n->rx_next = (n->rx_next + 1) % RING;
    return result;
}

static bool rtl8169_link(struct netdev *dev)
{
    struct rtl8169 *n = dev->priv;
    return r8(n, 0x6c) & 2;
}

static bool rtl8169_setup(struct rtl8169 *n)
{
    n->xid = r32(n, 0x40) >> 20 & 0x7cf;
    w8(n, 0x37, 0x10);
    for (int i = 0; i < 1000 && (r8(n, 0x37) & 0x10); i++)
        hal_sleep_ms(1);
    for (int i = 0; i < 6; i++)
        n->dev.mac[i] = r8(n, (uint16_t)i);
    n->rx = kmalloc_pages(4096);
    n->tx = kmalloc_pages(4096);
    n->rx_buf = kmalloc_pages(RING * BUFFER);
    n->tx_buf = kmalloc_pages(RING * BUFFER);
    if (!n->rx || !n->tx || !n->rx_buf || !n->tx_buf)
        return false;
    memset((void *)n->tx, 0, 4096);
    for (int i = 0; i < RING; i++)
        rearm(n, i);
    n->tx[RING - 1].opts1 = 1u << 30;
    w8(n, 0x50, 0xc0);
    /* The PHY may still be powered down by the system that ran before (Windows turns it off on shutdown): power
       the PLL up, wake the PHY and let it negotiate the link again. */
    if (n->pll)
        w8(n, 0x6f, r8(n, 0x6f) | (n->xid >= 0x4c0 ? 0xc0 : 0x80));
    phy_write(n, 0x1f, 0);
    phy_write(n, 0x00, 0x1200);
    if (n->rtl8125) {
        w32(n, 0x38, 0);
        w32(n, 0x3c, 0xffffffffu);
    }
    w16(n, 0xe0, 0x0008);
    w16(n, 0xda, 1536);
    w8(n, 0xec, 0x3b);
    w32(n, 0x20, vmm_phys((const void *)n->tx));
    w32(n, 0x24, 0);
    w32(n, 0xe4, vmm_phys((const void *)n->rx));
    w32(n, 0xe8, 0);
    w8(n, 0x37, 0x0c);
    w32(n, 0x08, 0xffffffffu);
    w32(n, 0x0c, 0xffffffffu);
    w32(n, 0x44, n->rtl8125 ? (8u << 27 | 7u << 8 | 0x0e) : 0x0000e70eu);
    w32(n, 0x40, 0x03000700u);
    if (!n->rtl8125)
        w16(n, 0x3c, 0);
    w8(n, 0x50, 0x00);
    return true;
}

static uint16_t io_bar(const struct pci_device *d)
{
    for (int i = 0; i < 6; i++) {
        uint32_t bar = pci_read(d, (uint8_t)(0x10 + i * 4));
        if (bar & 1)
            return (uint16_t)(bar & ~3u);
    }
    return 0;
}

static const uint16_t gigabit[] = {0x8161, 0x8162, 0x8168, 0x8169, 0x8167, 0x8136, 0x8125, 0x3000, 0x2502,
                                   0x2600, 0x4300, 0xc107};

bool realtek_supports(uint16_t vendor, uint16_t device)
{
    if (vendor == 0x10ec && (device == 0x8139 || device == 0x8138))
        return true;
    if (vendor != 0x10ec && !(vendor == 0x1186 && device == 0x4300) && !(vendor == 0x1259 && device == 0xc107))
        return false;
    for (size_t i = 0; i < ARRAY_SIZE(gigabit); i++)
        if (gigabit[i] == device)
            return true;
    return false;
}

static void found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    bool realtek = d->vendor == 0x10ec || (d->vendor == 0x1186 && d->device == 0x4300) ||
                   (d->vendor == 0x1259 && d->device == 0xc107);
    if (!realtek || d->class_code != 0x02)
        return;
    uint16_t io = io_bar(d);
    if (d->vendor == 0x10ec && (d->device == 0x8139 || d->device == 0x8138)) {
        if (small_count == MAX_NICS || !io)
            return;
        pci_write(d, 0x04, pci_read(d, 0x04) | 0x05);
        struct rtl8139 *n = &small[small_count];
        n->io = io;
        if (!rtl8139_setup(n))
            return;
        n->dev.driver = "rtl8139";
        n->dev.send = rtl8139_send;
        n->dev.receive = rtl8139_receive;
        n->dev.link = rtl8139_link;
        n->dev.priv = n;
        small_count++;
        net_register(&n->dev);
        return;
    }
    if (!realtek_supports(d->vendor, d->device) || big_count == MAX_NICS)
        return;
    struct rtl8169 *n = &big[big_count];
    memset(n, 0, sizeof(*n));
    n->io = io;
    n->rtl8125 = d->device == 0x8125 || d->device == 0x3000;
    n->pll = d->device != 0x8169 && d->device != 0x8167 && d->device != 0x8136 && d->vendor == 0x10ec;
    uint64_t base = 0;
    for (uint8_t bar = 0x18; bar >= 0x14 && !n->mmio; bar = (uint8_t)(bar - 4)) {
        uint32_t low = pci_read(d, bar);
        if (!(low & 1) && pci_bar_base(d, bar, &base))
            n->mmio = vmm_map_phys64(base, 0x1000);
    }
    if (!n->mmio && !io) {
        klog("realtek: %04x has neither memory nor I/O registers", d->device);
        return;
    }
    pci_write(d, 0x04, pci_read(d, 0x04) | (n->mmio ? 0x06u : 0u) | (io ? 0x05u : 0u));
    if (!rtl8169_setup(n)) {
        klog("realtek: %04x did not initialize", d->device);
        return;
    }
    klog("realtek: %04x (chip %03x) through %s", d->device, (unsigned)n->xid, n->mmio ? "memory" : "I/O ports");
    n->dev.driver = d->device == 0x8136 ? "rtl8101" : n->rtl8125 ? "rtl8125" : "rtl8169";
    n->dev.send = rtl8169_send;
    n->dev.receive = rtl8169_receive;
    n->dev.link = rtl8169_link;
    n->dev.priv = n;
    big_count++;
    net_register(&n->dev);
}

void realtek_init(void)
{
    pci_foreach(found, NULL);
}
