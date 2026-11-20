/* ACPI: soft-off (S5) and reset through the FADT, and the events of a running machine. The firmware's AML
   (kernel/acpi/aml.c) is loaded from the DSDT and SSDTs; the SCI then reports the power and sleep buttons (fixed
   events, or devices that Notify), the lid, and the general-purpose events whose _Lxx/_Exx methods (or the embedded
   controller's _Qxx queries) say what happened. The interrupt only records and masks what fired; the methods run
   later from acpi_service, outside the interrupt, which turns the results into keys (KEY_POWER, KEY_LID_CLOSE). */
#include "io.h"
#include <synth/aml.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/keys.h>
#include <synth/log.h>
#include <synth/vmm.h>
#include <synth/string.h>

struct gas {
    uint8_t space, width, offset, size;
    uint64_t addr;
} __attribute__((packed));

static uint32_t pm1a, pm1b, smi_cmd;
static uint8_t acpi_on, slp_a, slp_b, reset_value;
static struct gas reset_reg;
static bool have_s5, have_reset, parsed;
static volatile uint8_t *reset_mmio;

/* events */
#define PM1_PWRBTN (1u << 8)
#define PM1_SLPBTN (1u << 9)
#define FADT_PWR_BUTTON (1u << 4)
#define FADT_SLP_BUTTON (1u << 5)
#define FADT_HW_REDUCED (1u << 20)
#define MAX_GPE 256
#define MAX_SSDT 32

static uint32_t pm1a_evt, pm1b_evt, fadt_flags;
static uint8_t pm1_len;
static uint16_t sci_irq;
static uint32_t gpe_blk[2];
static uint8_t gpe_len[2], gpe1_base;
static uint64_t ssdt[MAX_SSDT];
static int nssdt;
static uint64_t dsdt_phys, facs_phys;
static uint8_t s3_a, s3_b;

static bool events_on;
static uint16_t pm1_enabled;
static uint8_t gpe_enabled[MAX_GPE / 8], gpe_edge[MAX_GPE / 8];
static volatile uint8_t gpe_pending[MAX_GPE / 8];
static volatile uint32_t fixed_pending;
static uint16_t gpe_storm[MAX_GPE];
static uint64_t storm_since, last_poll;
static int busy;

static struct aml_node *lid, *power_button, *sleep_button, *ec_node;
static bool lid_dirty, power_note, sleep_note;
static int lid_open = -1;
static uint32_t battery_changes;
static int ec_gpe = -1;
static uint16_t ec_data, ec_cmd;
static uint32_t sci_count;

#define KEYS 8
static int keys[KEYS];
static unsigned key_head, key_tail;

static const uint8_t *map(uint64_t phys, size_t size)
{
    return phys ? vmm_map_phys64(phys, size) : NULL;
}

static uint64_t find_rsdp(void)
{
    const struct x86_bootinfo *bi = x86_boot_info();
    if (bi->flags & BOOT_ACPI)
        return (uint64_t)bi->acpi_rsdp_high << 32 | bi->acpi_rsdp;
    if (bi->flags & BOOT_UEFI)
        return 0;
    const uint8_t *low = vmm_map_phys64(0, 0x100000);
    if (!low)
        return 0;
    uint32_t ebda = (uint32_t)(*(const uint16_t *)(low + 0x40e)) << 4;
    uint32_t ranges[2][2] = {{ebda, ebda + 1024}, {0xe0000, 0x100000}};
    for (int r = 0; r < 2; r++) {
        if (r == 0 && ebda < 0x80000)
            continue;
        for (uint32_t a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16) {
            if (memcmp(low + a, "RSD PTR ", 8))
                continue;
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++)
                sum += low[a + i];
            if (!sum)
                return a;
        }
    }
    return 0;
}

static const uint8_t *table(uint64_t phys)
{
    const uint8_t *h = map(phys, 36);
    if (!h)
        return NULL;
    uint32_t len = *(const uint32_t *)(h + 4);
    if (len < 36 || len > (16u << 20))
        return NULL;
    return map(phys, len);
}

/* the FADT; the SSDTs the root table lists go to ssdt[] */
static const uint8_t *find_fadt(uint64_t rsdp_phys)
{
    const uint8_t *rsdp = map(rsdp_phys, 36);
    if (!rsdp)
        return NULL;
    uint64_t root = *(const uint32_t *)(rsdp + 16);
    int entry = 4;
    if (rsdp[15] >= 2 && *(const uint64_t *)(rsdp + 24)) {
        uint64_t x = *(const uint64_t *)(rsdp + 24);
        if (sizeof(void *) == 8 || !(x >> 32)) {
            root = x;
            entry = 8;
        }
    }
    const uint8_t *rt = table(root);
    if (!rt)
        return NULL;
    uint32_t len = *(const uint32_t *)(rt + 4);
    uint64_t fadt = 0;
    nssdt = 0;
    for (uint32_t off = 36; off + entry <= len; off += entry) {
        uint64_t p = entry == 8 ? *(const uint64_t *)(rt + off) : *(const uint32_t *)(rt + off);
        const uint8_t *h = map(p, 36);
        if (h && !memcmp(h, "FACP", 4) && !fadt)
            fadt = p;
        else if (h && !memcmp(h, "SSDT", 4) && nssdt < MAX_SSDT)
            ssdt[nssdt++] = p;
    }
    return fadt ? table(fadt) : NULL;
}

static int aml_byte(const uint8_t *p, uint8_t *out)
{
    if (*p == 0x0a) {
        *out = p[1];
        return 2;
    }
    if (*p == 0x00 || *p == 0x01) {
        *out = *p;
        return 1;
    }
    if (*p == 0xff) {
        *out = 0xff;
        return 1;
    }
    return 0;
}

static void find_s5(const uint8_t *dsdt)
{
    uint32_t len = *(const uint32_t *)(dsdt + 4);
    for (uint32_t i = 36; i + 16 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) || dsdt[i + 4] != 0x12)
            continue;
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\')))
            continue;
        const uint8_t *p = dsdt + i + 5;
        p += ((*p & 0xc0) >> 6) + 2; /* PkgLength, NumElements */
        int n = aml_byte(p, &slp_a);
        if (!n)
            return;
        p += n;
        if (!aml_byte(p, &slp_b))
            slp_b = slp_a;
        have_s5 = true;
        return;
    }
}

/* an I/O port block from the 32-bit field, or from the extended one when that is an I/O address */
static uint32_t port_block(const uint8_t *f, uint32_t len, uint32_t legacy, uint32_t ext)
{
    uint32_t port = *(const uint32_t *)(f + legacy);
    if (!port && len >= ext + 12 && f[ext] == 1)
        port = (uint32_t)*(const uint64_t *)(f + ext + 4);
    return port <= 0xffff ? port : 0;
}

void acpi_init(void)
{
    if (parsed)
        return;
    uint64_t rsdp = find_rsdp();
    const uint8_t *f = rsdp ? find_fadt(rsdp) : NULL;
    if (!f)
        return;
    parsed = true;
    uint32_t len = *(const uint32_t *)(f + 4);
    pm1a = *(const uint32_t *)(f + 64);
    pm1b = *(const uint32_t *)(f + 68);
    smi_cmd = *(const uint32_t *)(f + 48);
    acpi_on = f[52];
    sci_irq = *(const uint16_t *)(f + 46);
    pm1a_evt = port_block(f, len, 56, 148);
    pm1b_evt = port_block(f, len, 60, 160);
    pm1_len = len > 88 ? f[88] : 0;
    gpe_blk[0] = port_block(f, len, 80, 220);
    gpe_blk[1] = port_block(f, len, 84, 232);
    gpe_len[0] = len > 92 ? f[92] : 0;
    gpe_len[1] = len > 93 ? f[93] : 0;
    gpe1_base = len > 94 ? f[94] : 0;
    fadt_flags = len >= 116 ? *(const uint32_t *)(f + 112) : 0;
    uint64_t dsdt = *(const uint32_t *)(f + 40);
    if (len >= 148 && *(const uint64_t *)(f + 140) && (sizeof(void *) == 8 || !(*(const uint64_t *)(f + 140) >> 32)))
        dsdt = *(const uint64_t *)(f + 140);
    if (len >= 129 && (fadt_flags & (1u << 10))) {
        memcpy(&reset_reg, f + 116, sizeof(reset_reg));
        reset_value = f[128];
        if (reset_reg.space == 0)
            reset_mmio = (volatile uint8_t *)map(reset_reg.addr, 1);
        have_reset = reset_reg.addr && (reset_reg.space == 1 || reset_mmio);
    }
    facs_phys = *(const uint32_t *)(f + 36);
    if (!facs_phys && len >= 140 && (sizeof(void *) == 8 || !(*(const uint64_t *)(f + 132) >> 32)))
        facs_phys = *(const uint64_t *)(f + 132);
    const uint8_t *d = dsdt ? table(dsdt) : NULL;
    if (d && !memcmp(d, "DSDT", 4)) {
        dsdt_phys = dsdt;
        find_s5(d);
    }
}

/* ---- what AML reads and writes ---- */

struct page {
    uint64_t phys;
    volatile uint8_t *virt;
};
static struct page pages[48];
static unsigned page_next;

/* the mapping of a physical page, kept: the 32-bit kernel never takes a mapping back */
static volatile uint8_t *page_of(uint64_t phys)
{
    uint64_t base = phys & ~4095ull;
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
        if (pages[i].virt && pages[i].phys == base)
            return pages[i].virt + (phys - base);
    volatile uint8_t *v = vmm_map_uncached(base, 4096);
    if (!v)
        return NULL;
    struct page *p = &pages[page_next++ % (sizeof(pages) / sizeof(pages[0]))];
    p->phys = base;
    p->virt = v;
    return v + (phys - base);
}

static uint64_t mem_read(uint64_t addr, int bits)
{
    if ((addr & 4095) + (unsigned)bits / 8 > 4096) {
        uint64_t v = 0;
        for (int i = 0; i < bits / 8; i++)
            v |= mem_read(addr + (unsigned)i, 8) << (8 * i);
        return v;
    }
    volatile uint8_t *p = page_of(addr);
    if (!p)
        return 0;
    switch (bits) {
    case 8:
        return *p;
    case 16:
        return *(volatile uint16_t *)p;
    case 32:
        return *(volatile uint32_t *)p;
    default:
        return *(volatile uint32_t *)p | (uint64_t)*(volatile uint32_t *)(p + 4) << 32;
    }
}

static void mem_write(uint64_t addr, int bits, uint64_t v)
{
    if ((addr & 4095) + (unsigned)bits / 8 > 4096) {
        for (int i = 0; i < bits / 8; i++)
            mem_write(addr + (unsigned)i, 8, v >> (8 * i));
        return;
    }
    volatile uint8_t *p = page_of(addr);
    if (!p)
        return;
    switch (bits) {
    case 8:
        *p = (uint8_t)v;
        break;
    case 16:
        *(volatile uint16_t *)p = (uint16_t)v;
        break;
    case 32:
        *(volatile uint32_t *)p = (uint32_t)v;
        break;
    default:
        *(volatile uint32_t *)p = (uint32_t)v;
        *(volatile uint32_t *)(p + 4) = (uint32_t)(v >> 32);
        break;
    }
}

static uint64_t io_read(uint16_t port, int bits)
{
    switch (bits) {
    case 8:
        return inb(port);
    case 16:
        return inw(port);
    case 32:
        return inl(port);
    default:
        return inl(port) | (uint64_t)inl((uint16_t)(port + 4)) << 32;
    }
}

static void io_write(uint16_t port, int bits, uint64_t v)
{
    switch (bits) {
    case 8:
        outb(port, (uint8_t)v);
        break;
    case 16:
        outw(port, (uint16_t)v);
        break;
    case 32:
        outl(port, (uint32_t)v);
        break;
    default:
        outl(port, (uint32_t)v);
        outl((uint16_t)(port + 4), (uint32_t)(v >> 32));
        break;
    }
}

/* configuration space through ports 0xcf8/0xcfc: the first 256 bytes of each function */
static uint32_t pci_addr(uint64_t a)
{
    return 0x80000000u | (uint32_t)(a >> 32 & 0xff) << 16 | (uint32_t)(a >> 24 & 0x1f) << 11 |
           (uint32_t)(a >> 16 & 7) << 8 | (uint32_t)(a & 0xfc);
}

static uint64_t pci_cfg_read(uint64_t a, int bits)
{
    if ((a & 0xffff) > 0xff)
        return 0;
    if (bits == 64)
        return pci_cfg_read(a, 32) | pci_cfg_read(a + 4, 32) << 32;
    uint32_t flags = cpu_irq_save();
    outl(0xcf8, pci_addr(a));
    uint64_t v = io_read((uint16_t)(0xcfc + (a & 3)), bits);
    cpu_irq_restore(flags);
    return v;
}

static void pci_cfg_write(uint64_t a, int bits, uint64_t v)
{
    if ((a & 0xffff) > 0xff)
        return;
    if (bits == 64) {
        pci_cfg_write(a, 32, v);
        pci_cfg_write(a + 4, 32, v >> 32);
        return;
    }
    uint32_t flags = cpu_irq_save();
    outl(0xcf8, pci_addr(a));
    io_write((uint16_t)(0xcfc + (a & 3)), bits, v);
    cpu_irq_restore(flags);
}

/* ---- the embedded controller (ports from its _CRS; commands from the ACPI specification, 12.3) ---- */

#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_SCI 0x20

static bool ec_wait(uint8_t mask, uint8_t want)
{
    for (int i = 0; i < 200000; i++) {
        if ((inb(ec_cmd) & mask) == want)
            return true;
        io_wait();
    }
    return false;
}

static bool ec_read(uint8_t addr, uint8_t *v)
{
    if (!ec_wait(EC_IBF, 0))
        return false;
    outb(ec_cmd, 0x80);
    if (!ec_wait(EC_IBF, 0))
        return false;
    outb(ec_data, addr);
    if (!ec_wait(EC_OBF, EC_OBF))
        return false;
    *v = inb(ec_data);
    return true;
}

static void ec_write(uint8_t addr, uint8_t v)
{
    if (!ec_wait(EC_IBF, 0))
        return;
    outb(ec_cmd, 0x81);
    if (!ec_wait(EC_IBF, 0))
        return;
    outb(ec_data, addr);
    if (!ec_wait(EC_IBF, 0))
        return;
    outb(ec_data, v);
    ec_wait(EC_IBF, 0);
}

static uint64_t aml_read(int space, uint64_t addr, int bits)
{
    switch (space) {
    case AML_SPACE_MEMORY:
        return mem_read(addr, bits);
    case AML_SPACE_IO:
        return addr <= 0xffff ? io_read((uint16_t)addr, bits) : 0;
    case AML_SPACE_PCI:
        return pci_cfg_read(addr, bits);
    case AML_SPACE_EC: {
        uint64_t v = 0;
        for (int i = 0; ec_cmd && i < bits / 8; i++) {
            uint8_t b = 0;
            ec_read((uint8_t)(addr + (unsigned)i), &b);
            v |= (uint64_t)b << (8 * i);
        }
        return v;
    }
    case AML_SPACE_CMOS:
        outb(0x70, (uint8_t)(addr & 0x7f));
        return inb(0x71);
    default:
        return 0;
    }
}

static void aml_write(int space, uint64_t addr, int bits, uint64_t v)
{
    switch (space) {
    case AML_SPACE_MEMORY:
        mem_write(addr, bits, v);
        break;
    case AML_SPACE_IO:
        if (addr <= 0xffff)
            io_write((uint16_t)addr, bits, v);
        break;
    case AML_SPACE_PCI:
        pci_cfg_write(addr, bits, v);
        break;
    case AML_SPACE_EC:
        for (int i = 0; ec_cmd && i < bits / 8; i++)
            ec_write((uint8_t)(addr + (unsigned)i), (uint8_t)(v >> (8 * i)));
        break;
    case AML_SPACE_CMOS:
        outb(0x70, (uint8_t)(addr & 0x7f));
        outb(0x71, (uint8_t)v);
        break;
    default:
        break;
    }
}

static void stall(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++)
        io_wait();
}

static uint64_t timer_100ns(void)
{
    return hal_ticks_ms() * 10000;
}

static bool is_battery(struct aml_node *n)
{
    return aml_hid_is(n, "PNP0C0A") || aml_hid_is(n, "ACPI0003");
}

static void notify(struct aml_node *n, uint64_t value)
{
    char path[64];
    aml_path(n, path, sizeof(path));
    klog("acpi: notify %s 0x%x", path, (unsigned)value);
    if (n == lid)
        lid_dirty = true;
    else if (n == power_button && value == 0x80)
        power_note = true;
    else if (n == sleep_button && value == 0x80)
        sleep_note = true;
    else if (is_battery(n))
        battery_changes++;
}

/* ---- GPEs and fixed events ---- */

/* the port of GPE register byte i (status, or enable when en) */
static uint16_t gpe_port(int i, bool en)
{
    int block = i >= gpe_len[0] / 2;
    int at = block ? i - gpe_len[0] / 2 : i;
    return (uint16_t)(gpe_blk[block] + (uint32_t)(en ? gpe_len[block] / 2 : 0) + (uint32_t)at);
}

static int gpe_bytes(void)
{
    return MIN(gpe_len[0] / 2 + (gpe_blk[1] ? gpe_len[1] / 2 : 0), MAX_GPE / 8);
}

/* the number GPE register byte i, bit b stands for */
static int gpe_number(int i, int b)
{
    if (i < gpe_len[0] / 2)
        return i * 8 + b;
    return gpe1_base + (i - gpe_len[0] / 2) * 8 + b;
}

static uint16_t pm1_read(uint32_t base, int off)
{
    return base ? inw((uint16_t)(base + (uint32_t)off)) : 0;
}

static void pm1_write(uint32_t base, int off, uint16_t v)
{
    if (base)
        outw((uint16_t)(base + (uint32_t)off), v);
}

/* what the SCI does: note the fixed events and GPEs that fired and mask those GPEs until their methods have run */
static void sci_check(void)
{
    if (!events_on)
        return;
    uint16_t sts = pm1_read(pm1a_evt, 0) | pm1_read(pm1b_evt, 0);
    uint16_t fired = sts & pm1_enabled;
    if (fired) {
        pm1_write(pm1a_evt, 0, fired);
        pm1_write(pm1b_evt, 0, fired);
        fixed_pending |= fired;
    }
    for (int i = 0; i < gpe_bytes(); i++) {
        uint8_t active = inb(gpe_port(i, false)) & gpe_enabled[i];
        if (!active)
            continue;
        outb(gpe_port(i, true), (uint8_t)(gpe_enabled[i] & ~gpe_pending[i] & ~active));
        if (active & gpe_edge[i])
            outb(gpe_port(i, false), active & gpe_edge[i]);
        gpe_pending[i] |= active;
    }
}

static void sci_irq_handler(struct regs *r)
{
    UNUSED(r);
    sci_count++;
    sci_check();
}

static void push_key(int key)
{
    if (key_tail - key_head < KEYS)
        keys[key_tail++ % KEYS] = key;
}

static void ec_queries(void)
{
    for (int n = 0; n < 32 && (inb(ec_cmd) & EC_SCI); n++) {
        if (!ec_wait(EC_IBF, 0))
            return;
        outb(ec_cmd, 0x84);
        if (!ec_wait(EC_OBF, EC_OBF))
            return;
        uint8_t q = inb(ec_data);
        if (!q)
            return;
        char name[5] = {'_', 'Q', "0123456789ABCDEF"[q >> 4], "0123456789ABCDEF"[q & 15], 0};
        if (aml_find(ec_node, name))
            aml_eval(ec_node, name, NULL, 0, NULL);
        else
            klog("acpi: embedded controller query %s has no method", name);
    }
}

static void run_gpe(int number, bool edge)
{
    if (number == ec_gpe && ec_node) {
        ec_queries();
        return;
    }
    char name[16];
    ksnprintf(name, sizeof(name), "\\_GPE._%c%02X", edge ? 'E' : 'L', (unsigned)number);
    aml_eval(NULL, name, NULL, 0, NULL);
}

static void update_lid(void)
{
    uint64_t v;
    if (!lid || !aml_eval_int(lid, "_LID", &v))
        return;
    int open = v != 0;
    if (open != lid_open) {
        bool first = lid_open < 0;
        lid_open = open;
        klog("acpi: lid %s", open ? "open" : "closed");
        if (!first)
            push_key(open ? KEY_LID_OPEN : KEY_LID_CLOSE);
    }
}

/* the methods for what the SCI noted, then the keys that came of it */
int acpi_service(void)
{
    if (!events_on || hal_in_interrupt() || __atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
        return KEY_NONE;
    uint64_t now = hal_ticks_ms();
    if (now - last_poll >= 200) {
        /* a lost or unroutable SCI (one above IRQ 15) still gets its events, a little later */
        last_poll = now;
        uint32_t flags = cpu_irq_save();
        sci_check();
        cpu_irq_restore(flags);
    }
    if (now - storm_since >= 1000) {
        storm_since = now;
        memset(gpe_storm, 0, sizeof(gpe_storm));
    }
    uint32_t fixed = __atomic_exchange_n(&fixed_pending, 0, __ATOMIC_ACQ_REL);
    if (fixed & PM1_PWRBTN) {
        klog("acpi: power button");
        push_key(KEY_POWER);
    }
    if (fixed & PM1_SLPBTN) {
        klog("acpi: sleep button");
        push_key(KEY_SLEEP);
    }
    for (int i = 0; i < gpe_bytes(); i++) {
        uint8_t bits = __atomic_exchange_n(&gpe_pending[i], 0, __ATOMIC_ACQ_REL);
        for (int b = 0; bits && b < 8; b++) {
            if (!(bits >> b & 1))
                continue;
            int number = gpe_number(i, b);
            bool edge = gpe_edge[i] >> b & 1;
            run_gpe(number, edge);
            if (!edge)
                outb(gpe_port(i, false), (uint8_t)(1u << b));
            if (number < MAX_GPE && ++gpe_storm[number] > 200) {
                /* a level event that never goes away would keep the machine in its method */
                gpe_enabled[i] &= (uint8_t)~(1u << b);
                klog("acpi: GPE %02X fires without end, turned off", (unsigned)number);
            }
        }
        if (bits) {
            uint32_t flags = cpu_irq_save();
            outb(gpe_port(i, true), (uint8_t)(gpe_enabled[i] & ~gpe_pending[i]));
            cpu_irq_restore(flags);
        }
    }
    if (power_note) {
        power_note = false;
        klog("acpi: power button");
        push_key(KEY_POWER);
    }
    if (sleep_note) {
        sleep_note = false;
        klog("acpi: sleep button");
        push_key(KEY_SLEEP);
    }
    if (lid_dirty) {
        lid_dirty = false;
        update_lid();
    }
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return key_head != key_tail ? keys[key_head++ % KEYS] : KEY_NONE;
}

/* the embedded controller: its two ports from _CRS, its GPE from _GPE */
static void ec_setup(void)
{
    struct aml_node *n = aml_find_device("PNP0C09", NULL);
    struct aml_obj *crs;
    if (!n || !aml_eval(n, "_CRS", NULL, 0, &crs))
        return;
    uint32_t len;
    uint16_t ports[2] = {0, 0};
    int found = 0;
    /* the resource template: small items, an I/O descriptor (0x47) or a fixed one (0x4b) for each port */
    uint32_t count = 0;
    const uint8_t *bytes = aml_buffer(crs, &count);
    for (uint32_t i = 0; bytes && i < count && found < 2;) {
        uint8_t tag = bytes[i];
        if (tag & 0x80) {
            if (i + 3 > count)
                break;
            len = (uint32_t)bytes[i + 1] | (uint32_t)bytes[i + 2] << 8;
            i += 3 + (uint32_t)len;
            continue;
        }
        uint32_t size = tag & 7, type = tag >> 3 & 15;
        if (i + 1 + size > count || type == 15)
            break;
        if (type == 8 && size >= 7)
            ports[found++] = (uint16_t)(bytes[i + 2] | bytes[i + 3] << 8);
        else if (type == 9 && size >= 3)
            ports[found++] = (uint16_t)(bytes[i + 1] | bytes[i + 2] << 8);
        i += 1 + size;
    }
    aml_put(crs);
    if (found < 2)
        return;
    ec_node = n;
    ec_data = ports[0];
    ec_cmd = ports[1];
    uint64_t gpe;
    if (aml_eval_int(n, "_GPE", &gpe) && gpe < MAX_GPE)
        ec_gpe = (int)gpe;
    aml_region_ready(AML_SPACE_EC);
}

/* the GPEs that have a method (and the embedded controller's) are enabled; level ones unless they say _E */
static int gpe_setup(void)
{
    int count = 0;
    for (int i = 0; i < gpe_bytes(); i++) {
        outb(gpe_port(i, true), 0);
        outb(gpe_port(i, false), 0xff);
        for (int b = 0; b < 8; b++) {
            int number = gpe_number(i, b);
            char name[16];
            bool on = false;
            ksnprintf(name, sizeof(name), "\\_GPE._L%02X", (unsigned)number);
            if (aml_find(NULL, name)) {
                on = true;
            } else {
                name[7] = 'E';
                if (aml_find(NULL, name)) {
                    on = true;
                    gpe_edge[i] |= (uint8_t)(1u << b);
                }
            }
            if (number == ec_gpe) {
                on = true;
                gpe_edge[i] |= (uint8_t)(1u << b);
            }
            if (on) {
                gpe_enabled[i] |= (uint8_t)(1u << b);
                count++;
            }
        }
        outb(gpe_port(i, true), gpe_enabled[i]);
    }
    return count;
}

static void load_tables(void)
{
    const uint8_t *d = dsdt_phys ? table(dsdt_phys) : NULL;
    if (!d)
        return;
    aml_load(d, *(const uint32_t *)(d + 4));
    for (int i = 0; i < nssdt; i++) {
        const uint8_t *s = table(ssdt[i]);
        if (s)
            aml_load(s, *(const uint32_t *)(s + 4));
    }
}

static char summary[96];

const char *hal_power_init(void)
{
    acpi_init();
    if (!parsed || !dsdt_phys)
        return NULL;
    if (fadt_flags & FADT_HW_REDUCED) {
        str_copy(summary, "ACPI (hardware-reduced: no button or lid events yet)", sizeof(summary));
        return summary;
    }
    /* ACPI mode: the firmware hands the events to the SCI */
    if (smi_cmd && acpi_on && pm1a && !(inw((uint16_t)pm1a) & 1)) {
        outb((uint16_t)smi_cmd, acpi_on);
        for (int i = 0; i < 300 && !(inw((uint16_t)pm1a) & 1); i++)
            hal_sleep_ms(10);
    }
    if (pm1a_evt) {
        pm1_write(pm1a_evt, pm1_len / 2, 0);
        pm1_write(pm1b_evt, pm1_len / 2, 0);
        pm1_write(pm1a_evt, 0, 0xffff);
        pm1_write(pm1b_evt, 0, 0xffff);
    }
    aml_os.read = aml_read;
    aml_os.write = aml_write;
    aml_os.sleep_ms = hal_sleep_ms;
    aml_os.stall_us = stall;
    aml_os.timer = timer_100ns;
    aml_os.notify = notify;
    load_tables();
    aml_init_devices();
    ec_setup();
    lid = aml_find_device("PNP0C0D", NULL);
    power_button = aml_find_device("PNP0C0C", NULL);
    sleep_button = aml_find_device("PNP0C0E", NULL);
    int gpes = gpe_setup();
    if (!(fadt_flags & FADT_PWR_BUTTON))
        pm1_enabled |= PM1_PWRBTN;
    if (!(fadt_flags & FADT_SLP_BUTTON))
        pm1_enabled |= PM1_SLPBTN;
    pm1_write(pm1a_evt, pm1_len / 2, pm1_enabled);
    pm1_write(pm1b_evt, pm1_len / 2, pm1_enabled);
    events_on = true;
    if (sci_irq && sci_irq < 16 && sci_irq != 2) {
        /* the SCI is level-triggered: the line's bit in the ELCR */
        uint16_t port = sci_irq < 8 ? 0x4d0 : 0x4d1;
        outb(port, (uint8_t)(inb(port) | 1u << (sci_irq & 7)));
        irq_install(sci_irq, sci_irq_handler);
    }
    update_lid();
    ksnprintf(summary, sizeof(summary), "ACPI, SCI on IRQ %u, %d GPE%s%s%s%s", (unsigned)sci_irq, gpes,
              gpes == 1 ? "" : "s", (pm1_enabled & PM1_PWRBTN) || power_button ? ", power button" : "",
              lid ? ", lid" : "", ec_node ? ", embedded controller" : "");
    return summary;
}

/* ---- sleep in S3: \_S3 gives the sleep types, the FACS holds where the processor starts again ---- */

static volatile uint8_t *facs(void)
{
    volatile uint8_t *t = facs_phys ? (volatile uint8_t *)map(facs_phys, 64) : NULL;
    return t && !memcmp((const void *)t, "FACS", 4) ? t : NULL;
}

bool acpi_s3_ready(char *why, size_t cap)
{
    if (!events_on || !pm1a) {
        ksnprintf(why, cap, "ACPI is not on (or the machine is hardware-reduced)");
        return false;
    }
    struct aml_obj *pkg = NULL;
    uint64_t a = 0, b = 0;
    bool ok = aml_eval(aml_root(), "\\_S3", NULL, 0, &pkg) && pkg && aml_type_of(pkg) == AML_PKG &&
              aml_pkg_int(pkg, 0, &a);
    if (ok && !aml_pkg_int(pkg, 1, &b))
        b = a;
    if (pkg)
        aml_put(pkg);
    if (!ok) {
        ksnprintf(why, cap, "the firmware offers no S3");
        return false;
    }
    if (!facs()) {
        ksnprintf(why, cap, "there is no FACS for the waking vector");
        return false;
    }
    s3_a = (uint8_t)(a & 7);
    s3_b = (uint8_t)(b & 7);
    return true;
}

static void clear_events(void)
{
    pm1_write(pm1a_evt, 0, 0xffff);
    pm1_write(pm1b_evt, 0, 0xffff);
    for (int i = 0; i < gpe_bytes(); i++)
        outb(gpe_port(i, false), 0xff);
}

bool acpi_s3_prepare(uint32_t vector)
{
    volatile uint8_t *t = facs();
    if (!t)
        return false;
    *(volatile uint32_t *)(t + 12) = vector;
    if (*(volatile uint32_t *)(t + 4) >= 32)
        *(volatile uint64_t *)(t + 24) = 0;
    struct aml_obj *arg = aml_int(3);
    if (aml_find(aml_root(), "\\_PTS"))
        aml_eval(aml_root(), "\\_PTS", &arg, 1, NULL);
    aml_put(arg);
    clear_events();
    return true;
}

/* the last step: the machine sleeps while the registers are written, or does not and this returns */
void acpi_s3_enter(void)
{
    __asm__ volatile("wbinvd" ::: "memory");
    clear_events();
    uint16_t a = (uint16_t)((inw((uint16_t)pm1a) & 0x203) | s3_a << 10);
    outw((uint16_t)pm1a, a);
    if (pm1b)
        outw((uint16_t)pm1b, (uint16_t)((inw((uint16_t)pm1b) & 0x203) | s3_b << 10));
    outw((uint16_t)pm1a, (uint16_t)(a | 1u << 13));
    if (pm1b)
        outw((uint16_t)pm1b, (uint16_t)((inw((uint16_t)pm1b) & 0x203) | s3_b << 10 | 1u << 13));
    for (int i = 0; i < 1000000 && !(pm1_read(pm1a_evt, 0) & 0x8000); i++)
        io_wait();
}

/* after the wake (or a failed try): ACPI mode, the enabled events, the SCI's trigger, then \_WAK */
void acpi_s3_resume(void)
{
    if (smi_cmd && acpi_on && !(inw((uint16_t)pm1a) & 1)) {
        outb((uint16_t)smi_cmd, acpi_on);
        for (int i = 0; i < 300000 && !(inw((uint16_t)pm1a) & 1); i++)
            io_wait();
    }
    clear_events();
    pm1_write(pm1a_evt, pm1_len / 2, pm1_enabled);
    pm1_write(pm1b_evt, pm1_len / 2, pm1_enabled);
    for (int i = 0; i < gpe_bytes(); i++)
        outb(gpe_port(i, true), (uint8_t)(gpe_enabled[i] & ~gpe_pending[i]));
    if (sci_irq && sci_irq < 16 && sci_irq != 2) {
        uint16_t port = sci_irq < 8 ? 0x4d0 : 0x4d1;
        outb(port, (uint8_t)(inb(port) | 1u << (sci_irq & 7)));
    }
    struct aml_obj *arg = aml_int(3);
    if (aml_find(aml_root(), "\\_WAK"))
        aml_eval(aml_root(), "\\_WAK", &arg, 1, NULL);
    aml_put(arg);
    lid_dirty = true;
}

int hal_lid_state(void)
{
    return lid ? lid_open : -1;
}

void hal_acpi_info(char *buf, size_t cap)
{
    ksnprintf(buf, cap,
              "SCI: IRQ %u, %u interrupts\nPM1 events: 0x%x at 0x%x\nGPE0: %u bytes at 0x%x, GPE1: %u bytes at 0x%x\n"
              "embedded controller: %s (data 0x%x, command 0x%x, GPE %d)\nlid: %s\nbattery notifications: %u\n"
              "AML errors: %u%s%s\n",
              (unsigned)sci_irq, (unsigned)sci_count, (unsigned)pm1_enabled, (unsigned)pm1a_evt,
              (unsigned)gpe_len[0], (unsigned)gpe_blk[0], (unsigned)gpe_len[1], (unsigned)gpe_blk[1],
              ec_node ? "yes" : "no", (unsigned)ec_data, (unsigned)ec_cmd, ec_gpe,
              lid_open < 0 ? "none" : lid_open ? "open" : "closed", (unsigned)battery_changes,
              (unsigned)aml_errors, aml_errors ? ", last: " : "", aml_errors ? aml_last_error : "");
}

void acpi_poweroff(void)
{
    if (!have_s5 || !pm1a)
        return;
    if (smi_cmd && acpi_on && !(inw((uint16_t)pm1a) & 1)) {
        outb((uint16_t)smi_cmd, acpi_on);
        for (int i = 0; i < 300000 && !(inw((uint16_t)pm1a) & 1); i++)
            io_wait();
    }
    outw((uint16_t)pm1a, (uint16_t)((inw((uint16_t)pm1a) & 0x203) | (slp_a & 7) << 10 | 1u << 13));
    if (pm1b)
        outw((uint16_t)pm1b, (uint16_t)((inw((uint16_t)pm1b) & 0x203) | (slp_b & 7) << 10 | 1u << 13));
    for (int i = 0; i < 1000000; i++)
        io_wait();
}

void acpi_reset(void)
{
    if (have_reset) {
        if (reset_reg.space == 1) {
            outb((uint16_t)reset_reg.addr, reset_value);
        } else {
            *reset_mmio = reset_value;
        }
        for (int i = 0; i < 100000; i++)
            io_wait();
    }
    outb(0xcf9, 0x02);
    io_wait();
    outb(0xcf9, 0x06);
    for (int i = 0; i < 100000; i++)
        io_wait();
}
