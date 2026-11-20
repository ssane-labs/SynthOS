/* HID over I2C: the touchpads (and touch screens) of most laptops since 2013. ACPI lists them as PNP0C50/ACPI0C50
   devices: an I2cSerialBus resource in _CRS names the controller, the address and the speed, and _DSM function 1
   (GUID 3cdff6f7-4267-4555-ad05-b30a3d8938de) gives the register of the HID descriptor. The controllers are
   Synopsys DesignWare I2C blocks (Intel LPSS on PCI, AMD and older Intel ones from ACPI with Memory32Fixed), run as
   master by polling. The device's interrupt line is a GPIO, which would need a driver for each platform's pin
   controller: the input register is read every few milliseconds instead, a device without a report answering with
   a length of 0. The reports go to the HID multi-touch parser (kernel/core/hidmt.c). 64-bit kernel only. */

#ifdef __x86_64__

#include <synth/aml.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/hidmt.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"
#include "usb.h"

#define MAX_DEVICES 2
#define DESC_MAX 4096
#define INPUT_MAX 256
#define POLL_MS 4

enum {
    DW_CON = 0x00,
    DW_TAR = 0x04,
    DW_DATA_CMD = 0x10,
    DW_SS_HCNT = 0x14,
    DW_SS_LCNT = 0x18,
    DW_FS_HCNT = 0x1c,
    DW_FS_LCNT = 0x20,
    DW_INTR_MASK = 0x30,
    DW_RAW_INTR = 0x34,
    DW_CLR_INTR = 0x40,
    DW_CLR_TX_ABRT = 0x54,
    DW_ENABLE = 0x6c,
    DW_STATUS = 0x70,
    DW_TXFLR = 0x74,
    DW_RXFLR = 0x78,
    DW_SDA_HOLD = 0x7c,
    DW_ENABLE_STATUS = 0x9c,
    DW_COMP_PARAM = 0xf4,
    DW_COMP_TYPE = 0xfc,
    CMD_READ = 0x100,
    CMD_STOP = 0x200,
    CMD_RESTART = 0x400,
    INTR_TX_ABRT = 0x40,
    STATUS_MASTER_ACTIVITY = 0x20,
    LPSS_PRIV = 0x200,
};

struct bus {
    volatile uint8_t *regs;
    int tx_depth, rx_depth;
    bool fast, busy;
    uint16_t hcnt, lcnt, hold;
    uint64_t phys;
    bool lpss;
};

struct device {
    char name[48];
    struct bus *bus;
    uint16_t addr, desc_reg;
    uint16_t input_reg, max_input, command_reg, data_reg, report_reg, report_len;
    struct hidmt mt;
    uint8_t in[INPUT_MAX];
    int length; /* of a read, 0 between reads */
    int pushed, got;
    uint64_t next;
};

static struct bus buses[MAX_DEVICES];
static int bus_count;
static struct device *devices[MAX_DEVICES];
static int device_count;
static volatile bool paused; /* while the buses are set up again after sleep */

#define R32(b, o) (*(volatile uint32_t *)((b)->regs + (o)))

/* ---- the DesignWare controller ---- */

static bool disable(struct bus *b)
{
    R32(b, DW_ENABLE) = 0;
    for (int i = 0; i < 100000; i++) /* a few bus clocks; also called from the timer interrupt */
        if (!(R32(b, DW_ENABLE_STATUS) & 1))
            return true;
    return false;
}

static void configure(struct bus *b)
{
    disable(b);
    R32(b, DW_CON) = 0x01 | (b->fast ? 2u : 1u) << 1 | 0x20 | 0x40; /* master, speed, restarts, no slave */
    R32(b, b->fast ? DW_FS_HCNT : DW_SS_HCNT) = b->hcnt;
    R32(b, b->fast ? DW_FS_LCNT : DW_SS_LCNT) = b->lcnt;
    if (b->hold)
        R32(b, DW_SDA_HOLD) = b->hold;
    R32(b, DW_INTR_MASK) = 0;
    (void)R32(b, DW_CLR_INTR);
}

/* the target for the next transfer; the controller takes a new one only while disabled */
static bool target(struct bus *b, uint16_t addr)
{
    if (!disable(b))
        return false;
    R32(b, DW_TAR) = addr & 0x3ff;
    (void)R32(b, DW_CLR_INTR);
    R32(b, DW_ENABLE) = 1;
    return true;
}

static bool aborted(struct bus *b)
{
    if (!(R32(b, DW_RAW_INTR) & INTR_TX_ABRT))
        return false;
    (void)R32(b, DW_CLR_TX_ABRT);
    return true;
}

/* the command for byte i of a transfer that writes w bytes and then reads r */
static uint32_t command(const uint8_t *out, int w, int r, int i)
{
    uint32_t c = i < w ? out[i] : CMD_READ;
    if (i == w && w && r)
        c |= CMD_RESTART;
    if (i == w + r - 1)
        c |= CMD_STOP;
    return c;
}

/* writes w bytes, then reads r with a repeated start; false when the device did not answer */
static bool transfer(struct bus *b, uint16_t addr, const uint8_t *out, int w, uint8_t *in, int r)
{
    if (!target(b, addr))
        return false;
    int total = w + r, sent = 0, got = 0;
    uint64_t deadline = hal_ticks_ms() + 50 + (uint64_t)total / 8;
    while (sent < total || got < r) {
        if (aborted(b))
            return false;
        while (sent < total && (int)R32(b, DW_TXFLR) < b->tx_depth &&
               (sent < w || sent - w - got < b->rx_depth)) {
            R32(b, DW_DATA_CMD) = command(out, w, r, sent);
            sent++;
        }
        while (got < r && R32(b, DW_RXFLR))
            in[got++] = (uint8_t)R32(b, DW_DATA_CMD);
        if (hal_ticks_ms() > deadline)
            return false;
    }
    while (R32(b, DW_STATUS) & STATUS_MASTER_ACTIVITY)
        if (hal_ticks_ms() > deadline || aborted(b))
            return false;
    return !aborted(b);
}

static bool bus_start(struct bus *b)
{
    if (b->lpss) { /* out of reset, the DMA remap address, the clock on */
        volatile uint8_t *priv = b->regs + LPSS_PRIV;
        if ((*(volatile uint32_t *)(priv + 4) & 7) != 7) {
            *(volatile uint32_t *)(priv + 4) = 0;
            *(volatile uint32_t *)(priv + 4) = 7;
        }
        *(volatile uint32_t *)(priv + 0x40) = (uint32_t)b->phys;
        *(volatile uint32_t *)(priv + 0x44) = (uint32_t)(b->phys >> 32);
        *(volatile uint32_t *)priv |= 1;
    }
    if (R32(b, DW_COMP_TYPE) != 0x44570140)
        return false;
    uint32_t p = R32(b, DW_COMP_PARAM);
    b->tx_depth = p ? (int)((p >> 16) & 0xff) + 1 : 16;
    b->rx_depth = p ? (int)((p >> 8) & 0xff) + 1 : 16;
    configure(b);
    return true;
}

struct pci_find {
    uint8_t dev, fn;
    struct pci_device found;
    bool ok;
};

static void pci_match(const struct pci_device *d, void *ctx)
{
    struct pci_find *f = ctx;
    if (!f->ok && d->bus == 0 && d->dev == f->dev && d->fn == f->fn) {
        f->found = *d;
        f->ok = true;
    }
}

/* a Memory32Fixed descriptor (0x86) of a resource template */
static uint64_t fixed_memory(const uint8_t *t, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        if (!(t[i] & 0x80)) {
            if ((t[i] >> 3 & 15) == 15)
                break;
            i += 1 + (t[i] & 7u);
            continue;
        }
        if (i + 3 > n)
            break;
        uint32_t len = (uint32_t)t[i + 1] | (uint32_t)t[i + 2] << 8;
        if (t[i] == 0x86 && len >= 9 && i + 12 <= n)
            return (uint64_t)t[i + 4] | (uint64_t)t[i + 5] << 8 | (uint64_t)t[i + 6] << 16 | (uint64_t)t[i + 7] << 24;
        i += 3 + len;
    }
    return 0;
}

/* the controller ACPI names: an LPSS function on PCI (by _ADR) or registers from its _CRS; NULL with why */
static struct bus *bus_for(struct aml_node *ctl, bool fast, const char **why)
{
    struct bus *b = &buses[bus_count];
    if (bus_count == MAX_DEVICES) {
        *why = "too many controllers";
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    uint64_t adr;
    if (aml_eval_int(ctl, "_ADR", &adr)) {
        struct pci_find f = {(uint8_t)(adr >> 16), (uint8_t)(adr & 7), {0}, false};
        pci_foreach(pci_match, &f);
        if (!f.ok || f.found.vendor == 0xffff) {
            *why = "no controller on PCI";
            return NULL;
        }
        uint8_t cap = (uint8_t)(pci_read(&f.found, 0x34) & 0xfc);
        for (int guard = 0; cap && guard < 32; guard++) { /* power state D0 */
            uint32_t w0 = pci_read(&f.found, cap);
            if ((w0 & 0xff) == 0x01)
                pci_write(&f.found, (uint8_t)(cap + 4), pci_read(&f.found, (uint8_t)(cap + 4)) & ~3u);
            cap = (uint8_t)((w0 >> 8) & 0xfc);
        }
        pci_write(&f.found, 0x04, pci_read(&f.found, 0x04) | 0x6);
        if (!pci_bar_base(&f.found, 0x10, &b->phys)) {
            *why = "the controller has no registers";
            return NULL;
        }
        b->lpss = f.found.vendor == 0x8086;
    } else {
        struct aml_obj *crs;
        if (aml_eval(ctl, "_CRS", NULL, 0, &crs)) {
            uint32_t n = 0;
            const uint8_t *t = aml_buffer(crs, &n);
            b->phys = t ? fixed_memory(t, n) : 0;
            aml_put(crs);
        }
    }
    if (!b->phys) {
        *why = "no controller";
        return NULL;
    }
    for (int i = 0; i < bus_count; i++)
        if (buses[i].phys == b->phys)
            return &buses[i];
    b->regs = vmm_map_phys64(b->phys, 4096);
    b->fast = fast;
    /* the timing from ACPI (high and low counts, SDA hold), else from a 216 MHz clock: slower on slower clocks */
    struct aml_obj *t;
    uint64_t v[3] = {0, 0, 0};
    if (aml_eval(ctl, fast ? "FMCN" : "SSCN", NULL, 0, &t)) {
        for (uint32_t i = 0; i < 3; i++)
            aml_pkg_int(t, i, &v[i]);
        aml_put(t);
    }
    b->hcnt = (uint16_t)(v[0] ? v[0] : fast ? 173 : 950);
    b->lcnt = (uint16_t)(v[1] ? v[1] : fast ? 367 : 1124);
    b->hold = (uint16_t)(v[2] ? v[2] : 65);
    if (!b->regs || !bus_start(b)) {
        *why = "not a DesignWare I2C controller";
        return NULL;
    }
    bus_count++;
    return b;
}

/* ---- HID over I2C ---- */

static bool reg_read(struct device *d, uint16_t reg, uint8_t *in, int n)
{
    uint8_t r[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
    return transfer(d->bus, d->addr, r, 2, in, n);
}

/* a command (SET_POWER 8, RESET 1, SET_REPORT 3) with its argument byte, and data for the data register */
static bool hid_command(struct device *d, uint8_t opcode, uint8_t arg, const uint8_t *data, int n)
{
    uint8_t out[80];
    int at = 0;
    out[at++] = (uint8_t)d->command_reg;
    out[at++] = (uint8_t)(d->command_reg >> 8);
    out[at++] = arg;
    out[at++] = opcode;
    if (data) {
        if (n + 8 > (int)sizeof(out))
            return false;
        out[at++] = (uint8_t)d->data_reg;
        out[at++] = (uint8_t)(d->data_reg >> 8);
        out[at++] = (uint8_t)(n + 2);
        out[at++] = (uint8_t)((n + 2) >> 8);
        memcpy(out + at, data, (size_t)n);
        at += n;
    }
    return transfer(d->bus, d->addr, out, at, NULL, 0);
}

/* power on, the multi-touch mode features (Set_Report, Feature, with the report ID in the command up to 14) */
static bool hid_start(struct device *d)
{
    if (!hid_command(d, 8, 0, NULL, 0))
        return false;
    hal_delay_ms(1);
    uint8_t buf[64];
    for (int i = 0, n; (n = hidmt_setting_report(&d->mt, i, buf, sizeof(buf))) > 0; i++)
        if (buf[0] < 15)
            hid_command(d, 3, (uint8_t)(0x30 | buf[0]), buf, n);
    d->length = 0;
    d->next = hal_ticks_ms();
    return true;
}

static void touch_pointer(int dx, int dy, int wheel, unsigned buttons)
{
    mouse_report(dx, dy, wheel, buttons);
}

static void touch_absolute(int x, int y, unsigned buttons)
{
    mouse_absolute(x, y, 0, buttons);
}

static const char *hid_attach(struct device *d)
{
    uint8_t desc[30];
    if (!reg_read(d, d->desc_reg, desc, sizeof(desc)))
        return "does not answer";
    if ((desc[0] | desc[1] << 8) != 30 || desc[2] != 0 || desc[3] != 1)
        return "has no HID descriptor";
#define W(o) (uint16_t)(desc[o] | desc[(o) + 1] << 8)
    d->report_len = W(4);
    d->report_reg = W(6);
    d->input_reg = W(8);
    d->max_input = MIN(W(10), (uint16_t)INPUT_MAX);
    d->command_reg = W(16);
    d->data_reg = W(18);
    uint16_t vendor = W(20), product = W(22);
#undef W
    size_t at = strlen(d->name);
    ksnprintf(d->name + at, sizeof(d->name) - at, " (%04x:%04x)", vendor, product);
    hid_command(d, 8, 0, NULL, 0);
    hid_command(d, 1, 0, NULL, 0); /* reset; the device then has a report of length 0 waiting */
    hal_delay_ms(100);
    reg_read(d, d->input_reg, d->in, 2);
    if (!d->report_len || d->report_len > DESC_MAX)
        return "has no report descriptor";
    uint8_t *report = kmalloc(d->report_len);
    if (!report)
        return "no memory";
    bool ok = reg_read(d, d->report_reg, report, d->report_len) && hidmt_parse(&d->mt, report, d->report_len);
    kfree(report);
    if (!ok)
        return "is not a touchpad or touch screen";
    d->mt.pad.out = (struct touch_out){touch_pointer, touch_absolute, keyboard_inject};
    if (!hid_start(d))
        return "does not power on";
    return NULL;
}

/* every tick: one device's input register read a FIFO's worth at a time, without waiting in the interrupt */
static void poll(void)
{
    uint64_t now = hal_ticks_ms();
    for (int k = 0; k < device_count && !paused; k++) {
        struct device *d = devices[k];
        struct bus *b = d->bus;
        if (!d->length) {
            if (b->busy || now < d->next)
                continue;
            if (!target(b, d->addr))
                continue;
            b->busy = true;
            d->length = d->max_input;
            d->pushed = d->got = 0;
        }
        if (aborted(b)) { /* nobody home: try again later */
            b->busy = false;
            d->length = 0;
            d->next = now + 100;
            continue;
        }
        while (d->pushed < d->length && (int)R32(b, DW_TXFLR) < b->tx_depth && d->pushed - d->got < b->rx_depth) {
            R32(b, DW_DATA_CMD) = CMD_READ | (d->pushed == d->length - 1 ? CMD_STOP : 0);
            d->pushed++;
        }
        while (d->got < d->length && R32(b, DW_RXFLR))
            d->in[d->got++] = (uint8_t)R32(b, DW_DATA_CMD);
        if (d->got < d->length)
            continue;
        b->busy = false;
        d->length = 0;
        int n = d->in[0] | d->in[1] << 8;
        if (n > 2 && n <= d->max_input) {
            hidmt_input(&d->mt, d->in + 2, (size_t)(n - 2), now);
            d->next = now;
        } else {
            d->next = now + POLL_MS;
        }
    }
}

static bool i2c_hid_resume(void)
{
    paused = true;
    for (int i = 0; i < bus_count; i++) {
        buses[i].busy = false;
        bus_start(&buses[i]);
    }
    for (int i = 0; i < device_count; i++)
        hid_start(devices[i]);
    paused = false;
    return true;
}

/* the I2cSerialBus descriptor (0x8e, type 1) of a resource template: address, speed and the controller's path */
static bool serial_bus(const uint8_t *t, uint32_t n, uint16_t *addr, uint32_t *speed, char *path, size_t cap)
{
    for (uint32_t i = 0; i < n;) {
        if (!(t[i] & 0x80)) {
            if ((t[i] >> 3 & 15) == 15)
                break;
            i += 1 + (t[i] & 7u);
            continue;
        }
        if (i + 3 > n)
            break;
        uint32_t len = (uint32_t)t[i + 1] | (uint32_t)t[i + 2] << 8, end = i + 3 + len;
        if (end > n)
            break;
        uint32_t data = i + 12 < end ? (uint32_t)t[i + 10] | (uint32_t)t[i + 11] << 8 : 0;
        if (t[i] == 0x8e && t[i + 5] == 1 && data >= 6 && i + 12 + data <= end) {
            *speed = (uint32_t)t[i + 12] | (uint32_t)t[i + 13] << 8 | (uint32_t)t[i + 14] << 16 |
                     (uint32_t)t[i + 15] << 24;
            *addr = (uint16_t)(t[i + 16] | t[i + 17] << 8);
            size_t k = 0;
            for (uint32_t s = i + 12 + data; s < end && t[s] && k + 1 < cap; s++)
                path[k++] = (char)t[s];
            path[k] = 0;
            return k > 0;
        }
        i = end;
    }
    return false;
}

/* _DSM(GUID, 1, 1, {}): the register of the HID descriptor */
static bool descriptor_register(struct aml_node *dev, uint16_t *reg)
{
    static const uint8_t guid[16] = {0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45,
                                     0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde};
    struct aml_obj *args[4] = {aml_buf(guid, sizeof(guid)), aml_int(1), aml_int(1), aml_pkg(0)};
    struct aml_obj *ret = NULL;
    bool ok = args[0] && args[1] && args[2] && args[3] && aml_eval(dev, "_DSM", args, 4, &ret);
    uint64_t v = 0;
    ok = ok && aml_obj_int(ret, &v);
    aml_put(ret);
    for (int i = 0; i < 4; i++)
        aml_put(args[i]);
    *reg = (uint16_t)v;
    return ok;
}

static void probe(struct aml_node *n)
{
    char path[48], ctl_path[48];
    aml_path(n, path, sizeof(path));
    struct aml_obj *crs;
    uint16_t addr = 0, reg = 0;
    uint32_t speed = 0;
    bool found = false;
    if (aml_eval(n, "_CRS", NULL, 0, &crs)) {
        uint32_t len = 0;
        const uint8_t *t = aml_buffer(crs, &len);
        found = t && serial_bus(t, len, &addr, &speed, ctl_path, sizeof(ctl_path));
        aml_put(crs);
    }
    if (!found) {
        klog("i2c-hid: %s has no I2C bus", path);
        return;
    }
    if (!descriptor_register(n, &reg)) {
        klog("i2c-hid: %s has no descriptor register (_DSM)", path);
        return;
    }
    klog("i2c-hid: %s at 0x%02x on %s (%u kHz), descriptor at 0x%04x", path, addr, ctl_path, speed / 1000, reg);
    struct aml_node *ctl = aml_find(aml_root(), ctl_path);
    const char *why = "no such controller";
    struct bus *b = ctl ? bus_for(ctl, speed >= 400000, &why) : NULL;
    if (!b) {
        klog("i2c-hid: %s: %s", ctl_path, why);
        return;
    }
    struct device *d = kzalloc(sizeof(*d));
    if (!d)
        return;
    d->bus = b;
    d->addr = addr;
    d->desc_reg = reg;
    const char *base = strrchr(path, '.');
    ksnprintf(d->name, sizeof(d->name), "%s", base ? base + 1 : path);
    why = hid_attach(d);
    if (why) {
        klog("i2c-hid: %s %s", d->name, why);
        kfree(d);
        return;
    }
    klog("i2c-hid: %s, %s with %d fingers", d->name, d->mt.direct ? "touch screen" : "touchpad", d->mt.fingers);
    touch_add(d->name, d->mt.direct);
    devices[device_count++] = d;
    if (device_count == 1) {
        timer_hook(poll);
        resume_register("i2c-hid", i2c_hid_resume);
    }
}

void i2c_hid_init(void)
{
    static const char *const ids[] = {"PNP0C50", "ACPI0C50"};
    for (size_t i = 0; i < ARRAY_SIZE(ids); i++)
        for (struct aml_node *n = aml_find_device(ids[i], NULL); n && device_count < MAX_DEVICES;
             n = aml_find_device(ids[i], n))
            probe(n);
}

#else

void i2c_hid_init(void) {}

#endif
