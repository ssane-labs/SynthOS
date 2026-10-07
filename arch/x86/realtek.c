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

struct rtl8169 {
    struct netdev dev;
    uint16_t io;
    volatile struct desc *rx, *tx;
    uint8_t *rx_buf, *tx_buf;
    int rx_next, tx_next;
};

static struct rtl8139 small[MAX_NICS];
static struct rtl8169 big[MAX_NICS];
static int small_count, big_count;

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
    outb((uint16_t)(n->io + 0x38), 0x40);
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
    return inb((uint16_t)(n->io + 0x6c)) & 2;
}

static bool rtl8169_setup(struct rtl8169 *n)
{
    outb((uint16_t)(n->io + 0x37), 0x10);
    for (int i = 0; i < 1000 && (inb((uint16_t)(n->io + 0x37)) & 0x10); i++)
        hal_sleep_ms(1);
    for (int i = 0; i < 6; i++)
        n->dev.mac[i] = inb((uint16_t)(n->io + i));
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
    outb((uint16_t)(n->io + 0x50), 0xc0);
    outw((uint16_t)(n->io + 0xe0), 0x0008);
    outw((uint16_t)(n->io + 0xda), 1536);
    outb((uint16_t)(n->io + 0xec), 0x3b);
    outl((uint16_t)(n->io + 0x20), vmm_phys((const void *)n->tx));
    outl((uint16_t)(n->io + 0x24), 0);
    outl((uint16_t)(n->io + 0xe4), vmm_phys((const void *)n->rx));
    outl((uint16_t)(n->io + 0xe8), 0);
    outb((uint16_t)(n->io + 0x37), 0x0c);
    outl((uint16_t)(n->io + 0x44), 0x0000e70eu);
    outl((uint16_t)(n->io + 0x40), 0x03000700u);
    outw((uint16_t)(n->io + 0x3c), 0);
    outb((uint16_t)(n->io + 0x50), 0x00);
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

static void found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    bool realtek = d->vendor == 0x10ec || (d->vendor == 0x1186 && d->device == 0x4300) ||
                   (d->vendor == 0x1259 && d->device == 0xc107);
    if (!realtek || d->class_code != 0x02)
        return;
    uint16_t io = io_bar(d);
    if (!io)
        return;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x05);
    if (d->vendor == 0x10ec && (d->device == 0x8139 || d->device == 0x8138)) {
        if (small_count == MAX_NICS)
            return;
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
    static const uint16_t gigabit[] = {0x8161, 0x8168, 0x8169, 0x8167, 0x8136, 0x8125, 0x2502, 0x2600, 0x4300,
                                       0xc107};
    bool known = false;
    for (size_t i = 0; i < ARRAY_SIZE(gigabit); i++)
        known |= gigabit[i] == d->device;
    if (!known || big_count == MAX_NICS)
        return;
    struct rtl8169 *n = &big[big_count];
    n->io = io;
    if (!rtl8169_setup(n))
        return;
    n->dev.driver = d->device == 0x8136 ? "rtl8101" : "rtl8169";
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
