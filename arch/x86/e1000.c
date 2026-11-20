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
    CTRL_EXT = 0x0018,
    MDIC = 0x0020,
    FEXTNVM3 = 0x003c,
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
    /* the queue registers of the 82575 and later (igb, igc) */
    Q_RDBAL = 0xc000,
    Q_RDBAH = 0xc004,
    Q_RDLEN = 0xc008,
    Q_SRRCTL = 0xc00c,
    Q_RDH = 0xc010,
    Q_RDT = 0xc018,
    Q_RXDCTL = 0xc028,
    Q_TDBAL = 0xe000,
    Q_TDBAH = 0xe004,
    Q_TDLEN = 0xe008,
    Q_TDH = 0xe010,
    Q_TDT = 0xe018,
    Q_TXDCTL = 0xe028,
    IGC_IMC = 0x150c,
    EIMC = 0x1528,
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

/* The 82575 and later write a packet back over the buffer address ("advanced" descriptors, which the I225/I226
   insist on): the length and status move to the second half. */
struct adv_rx_desc {
    uint64_t addr, wb;
} __attribute__((packed));

struct adv_tx_desc {
    uint64_t addr;
    uint32_t cmd_type_len, olinfo_status;
} __attribute__((packed));

enum kind { E1000, E1000E, PCH, IGB, IGC };

struct e1000 {
    struct netdev dev;
    volatile uint8_t *regs;
    enum kind kind;
    uint32_t rdt, tdt;
    volatile struct rx_desc *rx;
    volatile struct tx_desc *tx;
    uint8_t *rx_buf, *tx_buf;
    int rx_next, tx_next, rx_unposted;
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

/* the 32-bit kernel (old PCs, which do not have these cards) leaves them out to stay small */
static bool advanced(const struct e1000 *n)
{
#ifdef __x86_64__
    return n->kind == IGB || n->kind == IGC;
#else
    UNUSED(n);
    return false;
#endif
}

static int adv_send(struct e1000 *n, const void *frame, size_t length)
{
    volatile struct adv_tx_desc *d = (volatile struct adv_tx_desc *)n->tx + n->tx_next;
    uint64_t end = hal_ticks_ms() + 50;
    while (d->cmd_type_len && !(d->olinfo_status & 1))
        if (hal_ticks_ms() > end)
            return E_BUSY;
    memcpy(n->tx_buf + n->tx_next * BUFFER, frame, length);
    d->addr = vmm_phys(n->tx_buf + n->tx_next * BUFFER);
    d->olinfo_status = (uint32_t)length << 14;
    cpu_barrier();
    /* DEXT, RS, IFCS, EOP, data descriptor */
    d->cmd_type_len = (1u << 29) | (1u << 27) | (1u << 25) | (1u << 24) | (3u << 20) | (uint32_t)length;
    n->tx_next = (n->tx_next + 1) % TX_COUNT;
    cpu_barrier();
    wr(n, n->tdt, (uint32_t)n->tx_next);
    return 0;
}

static int adv_receive(struct e1000 *n, void *frame, size_t cap)
{
    volatile struct adv_rx_desc *d = (volatile struct adv_rx_desc *)n->rx + n->rx_next;
    uint64_t wb = d->wb;
    uint32_t status = (uint32_t)wb;
    if (!(status & 1))
        return 0;
    size_t length = (size_t)(wb >> 32 & 0xffff);
    int result = -1;
    if ((status & 2) && !(status & 0xfff00000u) && length <= cap) {
        memcpy(frame, n->rx_buf + n->rx_next * BUFFER, length);
        result = (int)length;
    }
    int done = n->rx_next;
    d->addr = vmm_phys(n->rx_buf + done * BUFFER);
    d->wb = 0;
    n->rx_next = (n->rx_next + 1) % RX_COUNT;
    if (++n->rx_unposted >= 16 || !(((volatile struct adv_rx_desc *)n->rx)[n->rx_next].wb & 1)) {
        cpu_barrier();
        wr(n, n->rdt, (uint32_t)done);
        n->rx_unposted = 0;
    }
    return result ? result : -1;
}

static int e1000_send(struct netdev *dev, const void *frame, size_t length)
{
    struct e1000 *n = dev->priv;
    if (length > BUFFER)
        return E_RANGE;
    if (advanced(n))
        return adv_send(n, frame, length);
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
    if (advanced(n))
        return adv_receive(n, frame, cap);
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
    /* Every register write is a trap into the hypervisor in a VM: the card hears about free buffers in batches,
       and at once when the ring has been drained. */
    if (++n->rx_unposted >= 16 || !(n->rx[n->rx_next].status & 1)) {
        cpu_barrier();
        wr(n, RDT, (uint32_t)done);
        n->rx_unposted = 0;
    }
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

static void mdic_wait(struct e1000 *n)
{
    for (int i = 0; i < 100 && !(rd(n, MDIC) & (1u << 28)); i++)
        hal_sleep_ms(1);
}

/* The PHY of the I217, I218 and I219 is shared with the management engine and may still be asleep from the
   system that ran before (Windows puts it into its low-power SMBus mode on shutdown, "ULP"): ask the engine to
   let go, power-cycle the PHY with LANPHYPC and wake it if it was powered down. Linux does the same. */
static void pch_wake_phy(struct e1000 *n)
{
    enum { H2ME = 0x5b50, FWSM = 0x5b54 };
    uint32_t fwsm = rd(n, FWSM);
    if (fwsm & 0x8000) {
        wr(n, H2ME, (rd(n, H2ME) & ~0x800u) | 0x1000u);
        for (int i = 0; i < 30 && (rd(n, FWSM) & 0x400u); i++)
            hal_sleep_ms(10);
        wr(n, H2ME, rd(n, H2ME) & ~0x1000u);
    }
    if (fwsm & 0x40) {
        wr(n, FEXTNVM3, (rd(n, FEXTNVM3) & ~0x0c000000u) | 0x08000000u);
        uint32_t ctrl = rd(n, CTRL);
        wr(n, CTRL, (ctrl | 0x00010000u) & ~0x00020000u);
        rd(n, STATUS);
        hal_sleep_ms(1);
        wr(n, CTRL, ctrl & ~0x00030000u);
        rd(n, STATUS);
        for (int i = 0; i < 20 && !(rd(n, CTRL_EXT) & 4); i++)
            hal_sleep_ms(5);
        hal_sleep_ms(30);
    }
    wr(n, CTRL_EXT, rd(n, CTRL_EXT) & ~0x800u);
}

static void pch_phy_on(struct e1000 *n)
{
    enum { EXTCNF_CTRL = 0x0f00 };
    wr(n, EXTCNF_CTRL, rd(n, EXTCNF_CTRL) | 0x20);
    for (int i = 0; i < 100 && !(rd(n, EXTCNF_CTRL) & 0x20); i++)
        hal_sleep_ms(1);
    wr(n, MDIC, 0u << 16 | 2u << 21 | 2u << 26);
    mdic_wait(n);
    uint32_t v = rd(n, MDIC);
    if ((v & (1u << 28)) && !(v & (1u << 30)) && (v & 0x0c00)) {
        wr(n, MDIC, ((v & 0xffff & ~0x0c00u) | 0x1200u) | 0u << 16 | 2u << 21 | 1u << 26);
        mdic_wait(n);
    }
    wr(n, EXTCNF_CTRL, rd(n, EXTCNF_CTRL) & ~0x20u);
}

/* wakes the PHY (clears power-down and isolate) and lets it negotiate the link again */
static void phy_restart(struct e1000 *n, uint32_t phy)
{
    wr(n, MDIC, 0u << 16 | phy << 21 | 2u << 26);
    mdic_wait(n);
    uint32_t v = rd(n, MDIC);
    if (!(v & (1u << 28)) || (v & (1u << 30)))
        return;
    wr(n, MDIC, ((v & 0xffff & ~0x0c00u) | 0x1200u) | 0u << 16 | phy << 21 | 1u << 26);
    mdic_wait(n);
}

static void reset(struct e1000 *n)
{
    bool igc = n->kind == IGC;
    wr(n, IMC, 0xffffffffu);
    if (advanced(n)) {
        wr(n, IGC_IMC, 0xffffffffu);
        wr(n, EIMC, 0xffffffffu);
    }
    if (n->kind == PCH)
        pch_wake_phy(n);
    uint32_t bit = igc ? 1u << 29 : 1u << 26;
    wr(n, CTRL, rd(n, CTRL) | bit);
    hal_sleep_ms(igc ? 20 : 10);
    for (int i = 0; i < 1000 && (rd(n, CTRL) & bit); i++)
        hal_sleep_ms(1);
    hal_sleep_ms(advanced(n) ? 20 : 0);
    wr(n, IMC, 0xffffffffu);
    if (advanced(n)) {
        wr(n, IGC_IMC, 0xffffffffu);
        wr(n, EIMC, 0xffffffffu);
    }
    rd(n, ICR);
    wr(n, CTRL, (rd(n, CTRL) | (1u << 6) | (igc ? 0 : 1u << 5)) & ~((1u << 3) | (1u << 31) | (1u << 7)));
    if (n->kind != E1000)
        wr(n, CTRL_EXT, rd(n, CTRL_EXT) | (1u << 28));
    if (n->kind == PCH)
        pch_phy_on(n);
    else if (advanced(n))
        phy_restart(n, 1);
}

static void read_mac(struct e1000 *n)
{
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
}

static bool enable_queue(struct e1000 *n, uint32_t reg, uint32_t thresholds)
{
    wr(n, reg, thresholds | (1u << 25));
    for (int i = 0; i < 100; i++) {
        if (rd(n, reg) & (1u << 25))
            return true;
        hal_sleep_ms(1);
    }
    return false;
}

static bool setup(struct e1000 *n)
{
    reset(n);
    read_mac(n);
    for (int i = 0; i < 128; i++)
        wr(n, MTA + (uint32_t)i * 4, 0);

    if (!n->rx) { /* the first time; after a sleep the same rings are set up again */
        n->rx = kmalloc_pages(4096);
        n->tx = kmalloc_pages(4096);
        n->rx_buf = kmalloc_pages(RX_COUNT * BUFFER);
        n->tx_buf = kmalloc_pages(TX_COUNT * BUFFER);
    }
    if (!n->rx || !n->tx || !n->rx_buf || !n->tx_buf)
        return false;
    n->rx_next = n->tx_next = n->rx_unposted = 0;
    memset((void *)n->rx, 0, 4096);
    memset((void *)n->tx, 0, 4096);
    uint32_t rctl = (1u << 1) | (1u << 4) | (1u << 15) | (1u << 26);
    uint32_t tctl = (1u << 1) | (1u << 3) | (0x0fu << 4) | (0x3fu << 12);
    if (advanced(n)) {
        n->rdt = Q_RDT;
        n->tdt = Q_TDT;
        for (int i = 0; i < RX_COUNT; i++)
            ((volatile struct adv_rx_desc *)n->rx)[i].addr = vmm_phys(n->rx_buf + i * BUFFER);
        wr(n, Q_RDBAL, vmm_phys((const void *)n->rx));
        wr(n, Q_RDBAH, 0);
        wr(n, Q_RDLEN, RX_COUNT * sizeof(struct adv_rx_desc));
        wr(n, Q_SRRCTL, (BUFFER / 1024) | (1u << 25) | (1u << 31));
        wr(n, Q_RDH, 0);
        wr(n, Q_RDT, 0);
        if (!enable_queue(n, Q_RXDCTL, 8u | 8u << 8 | 4u << 16))
            klog("e1000: the receive queue did not start");
        wr(n, Q_RDT, RX_COUNT - 1);
        wr(n, RCTL, rctl);
        wr(n, Q_TDBAL, vmm_phys((const void *)n->tx));
        wr(n, Q_TDBAH, 0);
        wr(n, Q_TDLEN, TX_COUNT * sizeof(struct adv_tx_desc));
        wr(n, Q_TDH, 0);
        wr(n, Q_TDT, 0);
        wr(n, TCTL, tctl);
        if (!enable_queue(n, Q_TXDCTL, 8u | 1u << 8 | 1u << 16))
            klog("e1000: the transmit queue did not start");
        return true;
    }
    n->rdt = RDT;
    n->tdt = TDT;
    for (int i = 0; i < RX_COUNT; i++)
        n->rx[i].addr = vmm_phys(n->rx_buf + i * BUFFER);
    wr(n, RDBAL, vmm_phys((const void *)n->rx));
    wr(n, RDBAH, 0);
    wr(n, RDLEN, RX_COUNT * sizeof(struct rx_desc));
    wr(n, RDH, 0);
    wr(n, RDT, RX_COUNT - 1);
    wr(n, RCTL, rctl);
    wr(n, TDBAL, vmm_phys((const void *)n->tx));
    wr(n, TDBAH, 0);
    wr(n, TDLEN, TX_COUNT * sizeof(struct tx_desc));
    wr(n, TDH, 0);
    wr(n, TDT, 0);
    wr(n, TIPG, 0x0060200a);
    wr(n, TCTL, tctl);
    return true;
}

static const uint16_t ids_e1000[] = {0x100e, 0x100f, 0x1004, 0x1011, 0x1010, 0x1012, 0x1015, 0x1016, 0x1017,
                                     0x101e, 0x1026, 0x1027, 0x1028, 0x1075, 0x1076, 0x1077, 0x1078, 0x1079,
                                     0x107a, 0x107b, 0x107c, 0x108a, 0x1099, 0x10b5};
/* 82571-82574, 82583, 80003, ICH8-ICH10 and the 82577-82579 */
static const uint16_t ids_e1000e[] = {0x10d3, 0x10f6, 0x150c, 0x105e, 0x105f, 0x1060, 0x10a4, 0x10a5, 0x10bc,
                                      0x10d9, 0x10da, 0x107d, 0x107e, 0x107f, 0x10b9, 0x108b, 0x108c, 0x109a,
                                      0x1096, 0x1098, 0x10ba, 0x10bb, 0x1049, 0x104a, 0x104b, 0x104c, 0x104d,
                                      0x10c4, 0x10c5, 0x10c0, 0x10c2, 0x10c3, 0x10bd, 0x294c, 0x10e5, 0x10f5,
                                      0x10bf, 0x10cb, 0x10cc, 0x10cd, 0x10ce, 0x10de, 0x10df, 0x10ea, 0x10eb,
                                      0x10ef, 0x10f0, 0x1502, 0x1503};
/* I217, I218 and every I219 so far */
static const uint16_t ids_pch[] = {0x153a, 0x153b, 0x155a, 0x1559, 0x15a0, 0x15a1, 0x15a2, 0x15a3, 0x156f, 0x1570,
                                   0x15b7, 0x15b8, 0x15b9, 0x15bb, 0x15bc, 0x15bd, 0x15be, 0x15d6, 0x15d7, 0x15d8,
                                   0x15e3, 0x0d4e, 0x0d4f, 0x0d4c, 0x0d4d, 0x0d53, 0x0d55, 0x15f9, 0x15fa, 0x15fb,
                                   0x15fc, 0x15f4, 0x15f5, 0x1a1c, 0x1a1d, 0x1a1e, 0x1a1f, 0x0dc5, 0x0dc6, 0x0dc7,
                                   0x0dc8, 0x550a, 0x550b, 0x550c, 0x550d, 0x550e, 0x550f, 0x5510, 0x5511, 0x57a0,
                                   0x57a1, 0x57b3, 0x57b4, 0x57b5, 0x57b6, 0x57b7, 0x57b8, 0x57b9, 0x57ba};
#ifdef __x86_64__
/* 82575, 82576, 82580, I350, I354, I210, I211 */
static const uint16_t ids_igb[] = {0x10a7, 0x10a9, 0x10d6, 0x10c9, 0x10e6, 0x10e7, 0x10e8, 0x1526, 0x150a, 0x1518,
                                   0x150d, 0x150e, 0x150f, 0x1510, 0x1511, 0x1516, 0x1527, 0x1521, 0x1522, 0x1523,
                                   0x1524, 0x1f40, 0x1f41, 0x1f45, 0x1533, 0x1534, 0x1535, 0x1536, 0x1537, 0x1538,
                                   0x157b, 0x157c, 0x1539};
/* I225, I226 and the I221 */
static const uint16_t ids_igc[] = {0x15f2, 0x15f3, 0x15f7, 0x15f8, 0x15fd, 0x0d9f, 0x125b, 0x125c, 0x125d, 0x125f,
                                   0x3100, 0x3101, 0x3102, 0x5502, 0x5503};
#endif

static bool in(const uint16_t *ids, size_t count, uint16_t id)
{
    for (size_t i = 0; i < count; i++)
        if (ids[i] == id)
            return true;
    return false;
}

#define IN(ids, id) in(ids, ARRAY_SIZE(ids), id)

static bool supported(uint16_t id, enum kind *kind)
{
    *kind = IN(ids_e1000, id) ? E1000 : IN(ids_e1000e, id) ? E1000E : IN(ids_pch, id) ? PCH : (enum kind)-1;
#ifdef __x86_64__
    if (*kind == (enum kind)-1)
        *kind = IN(ids_igb, id) ? IGB : IN(ids_igc, id) ? IGC : (enum kind)-1;
#endif
    return *kind != (enum kind)-1;
}

bool e1000_supports(uint16_t device)
{
    enum kind kind;
    return supported(device, &kind);
}

/* after a sleep in S3 the card has been reset: the same set-up again, on the rings it had */
static bool e1000_resume(void)
{
    bool ok = true;
    for (int i = 0; i < nic_count; i++)
        ok &= setup(&nics[i]);
    return ok;
}

static void found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    enum kind kind;
    if (d->vendor != 0x8086 || d->class_code != 0x02 || nic_count == MAX_NICS || !supported(d->device, &kind))
        return;
    uint64_t base;
    if (!pci_bar_base(d, 0x10, &base)) {
        klog("e1000: registers above 4 GB are out of reach");
        return;
    }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);
    struct e1000 *n = &nics[nic_count];
    n->regs = vmm_map_phys64(base, 0x20000);
    n->kind = kind;
    n->new_eeprom = kind != E1000;
    if (!n->regs || !setup(n)) {
        klog("e1000: device %04x did not initialize", d->device);
        return;
    }
    static const char *const names[] = {"e1000", "e1000e", "e1000e", "igb", "igc"};
    n->dev.driver = names[kind];
    n->dev.send = e1000_send;
    n->dev.receive = e1000_receive;
    n->dev.link = e1000_link;
    n->dev.priv = n;
    nic_count++;
    net_register(&n->dev);
    resume_register("e1000", e1000_resume);
}

void e1000_init(void)
{
    pci_foreach(found, NULL);
}
