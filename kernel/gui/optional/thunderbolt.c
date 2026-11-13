/* Thunderbolt, USB4 and docks. With Thunderbolt 3/4 and USB4 the firmware builds the PCIe tunnels, so what
 * reaches the system is PCIe hot-plug: a port whose slot says something was plugged in, and new devices
 * behind it. This module finds the host routers, shows the hot-plug ports and what hangs off them, watches the
 * slots, and when a dock arrives gives its new devices addresses inside the port's window and starts the ones
 * SynthOS has drivers for (USB controllers: the dock's USB ports, keyboard, disks, network adapters). */
#include <synth/command.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/module.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "../../../arch/x86/io.h"
#include "../internal.h"

#define MAX_DEVS 96
#define WATCH_MS 1000

struct dev {
    struct pci_device d;
    uint8_t header;
    uint8_t pcie; /* offset of the PCI Express capability, 0 if none */
    uint8_t secondary, subordinate;
    bool hotplug;
};

static struct dev devs[MAX_DEVS];
static int ndevs;

static uint16_t cfg16(const struct pci_device *d, uint8_t off)
{
    return (uint16_t)(pci_read(d, off & 0xfc) >> ((off & 2) * 8));
}

static uint8_t find_cap(const struct pci_device *d, uint8_t id)
{
    if (!(cfg16(d, 0x06) & 0x10))
        return 0;
    uint8_t at = (uint8_t)(pci_read(d, 0x34) & 0xfc);
    for (int guard = 0; at && guard < 48; guard++) {
        uint32_t v = pci_read(d, at);
        if ((v & 0xff) == id)
            return at;
        at = (uint8_t)(v >> 8 & 0xfc);
    }
    return 0;
}

static void collect(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (ndevs >= MAX_DEVS)
        return;
    struct dev *x = &devs[ndevs++];
    memset(x, 0, sizeof(*x));
    x->d = *d;
    x->header = (uint8_t)(pci_read(d, 0x0c) >> 16 & 0x7f);
    x->pcie = find_cap(d, 0x10);
    if (x->header == 1) {
        uint32_t buses = pci_read(d, 0x18);
        x->secondary = (uint8_t)(buses >> 8);
        x->subordinate = (uint8_t)(buses >> 16);
    }
    if (x->pcie && (cfg16(d, x->pcie + 2) & 0x100))
        x->hotplug = pci_read(d, x->pcie + 0x14) & 0x40;
}

static void scan(void)
{
    ndevs = 0;
    pci_foreach(collect, NULL);
}

/* ---- naming things ---- */

static const struct {
    uint16_t vendor, device;
    const char *name;
} routers[] = {
    {0x8086, 0x1513, "Intel Light Ridge (Thunderbolt 1)"},
    {0x8086, 0x1547, "Intel Cactus Ridge (Thunderbolt 1)"},
    {0x8086, 0x156a, "Intel Falcon Ridge (Thunderbolt 2)"},
    {0x8086, 0x156c, "Intel Falcon Ridge (Thunderbolt 2)"},
    {0x8086, 0x1575, "Intel Alpine Ridge (Thunderbolt 3)"},
    {0x8086, 0x1577, "Intel Alpine Ridge (Thunderbolt 3)"},
    {0x8086, 0x15bf, "Intel Alpine Ridge LP (Thunderbolt 3)"},
    {0x8086, 0x15d2, "Intel Alpine Ridge C (Thunderbolt 3)"},
    {0x8086, 0x15d9, "Intel Alpine Ridge C (Thunderbolt 3)"},
    {0x8086, 0x15e8, "Intel Titan Ridge (Thunderbolt 3)"},
    {0x8086, 0x15eb, "Intel Titan Ridge (Thunderbolt 3)"},
    {0x8086, 0x8a0d, "Intel Ice Lake (Thunderbolt 3)"},
    {0x8086, 0x8a17, "Intel Ice Lake (Thunderbolt 3)"},
    {0x8086, 0x9a1b, "Intel Tiger Lake (Thunderbolt 4)"},
    {0x8086, 0x9a1d, "Intel Tiger Lake (Thunderbolt 4)"},
    {0x8086, 0x1137, "Intel Maple Ridge (Thunderbolt 4)"},
    {0x8086, 0x463e, "Intel Alder Lake (Thunderbolt 4)"},
    {0x8086, 0x466d, "Intel Alder Lake (Thunderbolt 4)"},
    {0x8086, 0xa73e, "Intel Raptor Lake (Thunderbolt 4)"},
    {0x8086, 0xa76d, "Intel Raptor Lake (Thunderbolt 4)"},
    {0x8086, 0x7eb2, "Intel Meteor Lake (Thunderbolt 4)"},
    {0x8086, 0x7ec2, "Intel Meteor Lake (Thunderbolt 4)"},
    {0x8086, 0x7ec3, "Intel Meteor Lake (Thunderbolt 4)"},
    {0x8086, 0x5781, "Intel Barlow Ridge (Thunderbolt 5)"},
    {0x8086, 0x5784, "Intel Barlow Ridge (Thunderbolt 5)"},
    {0x1022, 0x162e, "AMD USB4 host router"},
    {0x1022, 0x162f, "AMD USB4 host router"},
    {0x1022, 0x1668, "AMD USB4 host router"},
    {0x1022, 0x1669, "AMD USB4 host router"},
};

static const char *router_name(const struct pci_device *d)
{
    for (size_t i = 0; i < ARRAY_SIZE(routers); i++)
        if (routers[i].vendor == d->vendor && routers[i].device == d->device)
            return routers[i].name;
    if (d->class_code == 0x0c && d->subclass == 0x03 && d->prog_if == 0x40)
        return "USB4 host router";
    return NULL;
}

static const char *kind(const struct pci_device *d)
{
    static const struct {
        uint8_t cls, sub, prog;
        const char *name;
    } kinds[] = {
        {0x0c, 0x03, 0x30, "USB 3 controller (xHCI)"}, {0x0c, 0x03, 0x20, "USB 2 controller (EHCI)"},
        {0x0c, 0x03, 0x40, "USB4 host router"},         {0x02, 0x00, 0xff, "Ethernet adapter"},
        {0x03, 0xff, 0xff, "Graphics"},                 {0x01, 0x08, 0xff, "NVMe storage"},
        {0x01, 0x06, 0xff, "SATA controller"},          {0x04, 0x03, 0xff, "Audio"},
        {0x06, 0x04, 0xff, "PCIe bridge"},              {0x0c, 0x0a, 0xff, "Thunderbolt controller"},
        {0x02, 0x80, 0xff, "Network adapter"},          {0x08, 0x80, 0xff, "System peripheral"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(kinds); i++)
        if (kinds[i].cls == d->class_code && (kinds[i].sub == 0xff || kinds[i].sub == d->subclass) &&
            (kinds[i].prog == 0xff || kinds[i].prog == d->prog_if))
            return kinds[i].name;
    return "Device";
}

static const char *link_speed(int s)
{
    static const char *const speeds[] = {"?", "2.5 GT/s", "5 GT/s", "8 GT/s", "16 GT/s", "32 GT/s", "64 GT/s"};
    return s >= 0 && s < (int)ARRAY_SIZE(speeds) ? speeds[s] : "?";
}

/* the hot-plug port a bus sits behind, or NULL */
static struct dev *port_of(uint8_t bus)
{
    struct dev *best = NULL;
    for (int i = 0; i < ndevs; i++) {
        struct dev *p = &devs[i];
        if (p->hotplug && p->secondary && bus >= p->secondary && bus <= p->subordinate &&
            (!best || p->secondary > best->secondary))
            best = p;
    }
    return best;
}

/* ---- giving a hot-plugged device its addresses ---- */

static struct dev *bridge_of(uint8_t bus)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].header == 1 && devs[i].secondary == bus)
            return &devs[i];
    return NULL;
}

/* Memory BARs of a device the firmware never saw are zero. Put them in the parent port's memory window (or its
 * prefetchable one), after whatever is already there. */
static int assign_bars(struct dev *x, char *why, size_t cap)
{
    struct dev *br = bridge_of(x->d.bus);
    if (!br)
        return 0;
    uint32_t win = pci_read(&br->d, 0x20);
    uint32_t base = (win & 0xfff0) << 16, limit = (win & 0xfff00000u) | 0xfffff;
    uint32_t pwin = pci_read(&br->d, 0x24);
    uint32_t pbase = (pwin & 0xfff0) << 16, plimit = (pwin & 0xfff00000u) | 0xfffff;
    if (pci_read(&br->d, 0x28) || pci_read(&br->d, 0x2c))
        pbase = 1, plimit = 0; /* a 64-bit window above 4 GB: leave it */
    /* the space the other devices behind the port already use */
    uint32_t used = base, pused = pbase;
    for (int i = 0; i < ndevs; i++) {
        struct dev *o = &devs[i];
        if (o == x || o->d.bus < br->secondary || o->d.bus > br->subordinate)
            continue;
        for (int b = 0; b < (o->header == 0 ? 6 : 2); b++) {
            uint32_t v = pci_read(&o->d, (uint8_t)(0x10 + b * 4));
            if (v & 1)
                continue;
            uint32_t at = v & ~0xfu;
            if (at >= base && at < limit)
                used = MAX(used, at + 0x1000);
            if (at >= pbase && at < plimit)
                pused = MAX(pused, at + 0x1000);
            if ((v & 6) == 4)
                b++;
        }
    }
    int assigned = 0;
    uint32_t cmd = pci_read(&x->d, 0x04);
    pci_write(&x->d, 0x04, cmd & ~0x7u);
    for (int b = 0; b < (x->header == 0 ? 6 : 2); b++) {
        uint8_t off = (uint8_t)(0x10 + b * 4);
        uint32_t v = pci_read(&x->d, off);
        bool wide = (v & 6) == 4;
        if (v & 1) {
            continue;
        }
        pci_write(&x->d, off, 0xffffffffu);
        uint32_t mask = pci_read(&x->d, off) & ~0xfu;
        pci_write(&x->d, off, v);
        if (!mask) {
            if (wide)
                b++;
            continue;
        }
        if (v & ~0xfu) { /* already placed */
            if (wide)
                b++;
            continue;
        }
        uint32_t size = ~mask + 1;
        bool pref = v & 8;
        uint32_t *next = pref && pbase < plimit ? &pused : &used;
        uint32_t top = next == &pused ? plimit : limit;
        uint32_t at = ALIGN_UP(*next, size);
        if (base >= limit || at + size - 1 > top || at < *next) {
            ksnprintf(why, cap, "the port's address window is too small");
            pci_write(&x->d, 0x04, cmd);
            return -1;
        }
        pci_write(&x->d, off, at | (v & 0xf));
        if (wide)
            pci_write(&x->d, (uint8_t)(off + 4), 0);
        *next = at + size;
        assigned++;
        if (wide)
            b++;
    }
    pci_write(&x->d, 0x04, cmd | 0x06);
    return assigned;
}

/* ---- watching the slots ---- */

#define MAX_SEEN 96
static uint32_t seen[MAX_SEEN]; /* bus:dev.fn of the devices behind hot-plug ports, as last seen */
static int nseen;
static bool watching;

static uint32_t key_of(const struct pci_device *d)
{
    return (uint32_t)d->bus << 16 | (uint32_t)d->dev << 8 | d->fn;
}

/* Clears the slots' change bits, and powers a slot that has something in it but no power yet (with native
 * hot-plug the system, not the firmware, switches the slot on). Returns how many slots were switched on. */
static int ack_slots(void)
{
    int powered = 0;
    for (int i = 0; i < ndevs; i++) {
        struct dev *p = &devs[i];
        if (!p->hotplug)
            continue;
        uint32_t caps = pci_read(&p->d, p->pcie + 0x14);
        uint32_t ctl = pci_read(&p->d, p->pcie + 0x18);
        uint16_t control = (uint16_t)ctl, status = (uint16_t)(ctl >> 16);
        if (status & 0x0109) /* write-1-to-clear: attention button, presence and link changes */
            pci_write(&p->d, p->pcie + 0x18, control | (uint32_t)(status & 0x0109) << 16);
        if ((caps & 0x2) && (status & 0x40) && (control & 0x400)) {
            control = (uint16_t)((control & ~0x400u & ~0x300u) | 0x100u); /* power on, power light on */
            pci_write(&p->d, p->pcie + 0x18, control);
            klog("thunderbolt: slot %u powered on", (unsigned)(caps >> 19));
            powered++;
        }
    }
    return powered;
}

/* Brings what is behind the hot-plug ports up to date; out (may be NULL) gets a line per change. */
static int refresh(struct shell *sh, bool notify)
{
    scan();
    if (ack_slots()) {
        hal_sleep_ms(300); /* the link trains, then the device answers */
        scan();
    }
    uint32_t now[MAX_SEEN];
    int nnow = 0, events = 0;
    for (int i = 0; i < ndevs; i++) {
        struct dev *x = &devs[i];
        struct dev *port = port_of(x->d.bus);
        if (!port || nnow >= MAX_SEEN)
            continue;
        now[nnow++] = key_of(&x->d);
        /* the firmware (or an earlier pass) turns on a device it set up; one that is still off arrived later */
        if (pci_read(&x->d, 0x04) & 0x6)
            continue;
        events++;
        char why[64] = "", msg[160];
        int bars = assign_bars(x, why, sizeof(why));
        const char *started = "";
        if (bars >= 0 && x->d.class_code == 0x0c && x->d.subclass == 0x03 && x->d.prog_if == 0x30)
            started = xhci_attach(&x->d) ? ", its USB ports are on" : ", but its USB controller did not start";
        if (bars < 0)
            ksnprintf(msg, sizeof(msg), "%s at %02x:%02x.%u could not be set up: %s", kind(&x->d), x->d.bus, x->d.dev,
                      x->d.fn, why);
        else
            ksnprintf(msg, sizeof(msg), "%s at %02x:%02x.%u%s", kind(&x->d), x->d.bus, x->d.dev, x->d.fn, started);
        klog("thunderbolt: connected: %s", msg);
        if (sh)
            sh_printf(sh, "connected: %s\n", msg);
        if (notify)
            gui_notify("Device Connected", msg);
    }
    for (int i = 0; watching && i < nseen; i++) {
        bool still = false;
        for (int j = 0; j < nnow && !still; j++)
            still = now[j] == seen[i];
        if (still)
            continue;
        events++;
        char msg[96];
        ksnprintf(msg, sizeof(msg), "the device at %02x:%02x.%u was unplugged", (unsigned)(seen[i] >> 16),
                  (unsigned)(seen[i] >> 8 & 0xff), (unsigned)(seen[i] & 0xff));
        klog("thunderbolt: %s", msg);
        if (sh)
            sh_printf(sh, "disconnected: %s\n", msg);
        if (notify)
            gui_notify("Device Disconnected", msg);
    }
    memcpy(seen, now, sizeof(uint32_t) * (size_t)nnow);
    nseen = nnow;
    watching = true;
    return events;
}

static void tick(void)
{
    static uint64_t due;
    uint64_t t = hal_ticks_ms();
    if (t < due)
        return;
    due = t + WATCH_MS;
    bool any = false;
    if (!watching) {
        refresh(NULL, false);
        return;
    }
    for (int i = 0; i < ndevs && !any; i++)
        any = devs[i].hotplug && (cfg16(&devs[i].d, devs[i].pcie + 0x1a) & 0x0108);
    /* some ports report nothing: look at the buses behind them every few seconds anyway */
    static int count;
    if (any || ++count % 5 == 0)
        refresh(NULL, true);
}

/* ---- the report ---- */

static void print_usb(struct shell *sh)
{
    struct usb_info usb[32];
    int n = hal_usb_devices(usb, 32);
    int hubs = 0;
    for (int i = 0; i < n; i++)
        hubs += !strcmp(usb[i].driver, "hub");
    sh_printf(sh, "\nUSB (%d device%s%s):\n", n, n == 1 ? "" : "s", hubs ? ", a hub or dock is connected" : "");
    if (!n)
        sh_printf(sh, "  nothing connected\n");
    static const char *const speeds[] = {"", "12 Mb/s", "1.5 Mb/s", "480 Mb/s", "5 Gb/s", "10 Gb/s"};
    for (int i = 0; i < n; i++)
        sh_printf(sh, "  %*s%s  [%04x:%04x %s, %s]\n", usb[i].depth * 2, "", usb[i].name, usb[i].vendor,
                  usb[i].product, usb[i].speed >= 0 && usb[i].speed < 6 ? speeds[usb[i].speed] : "?",
                  usb[i].driver[0] ? usb[i].driver : "no driver");
}

static void report(struct shell *sh)
{
    scan();
    int routers_found = 0;
    for (int i = 0; i < ndevs; i++) {
        const char *name = router_name(&devs[i].d);
        if (!name)
            continue;
        if (!routers_found++)
            sh_printf(sh, "Thunderbolt / USB4 host routers:\n");
        sh_printf(sh, "  %02x:%02x.%u  %s [%04x:%04x]\n", devs[i].d.bus, devs[i].d.dev, devs[i].d.fn, name,
                  devs[i].d.vendor, devs[i].d.device);
    }
    if (!routers_found)
        sh_printf(sh, "No Thunderbolt or USB4 host router: docks work over USB, or over PCIe hot-plug ports.\n");
    int ports = 0;
    for (int i = 0; i < ndevs; i++) {
        struct dev *p = &devs[i];
        if (!p->hotplug)
            continue;
        if (!ports++)
            sh_printf(sh, "\nHot-plug ports:\n");
        uint16_t status = cfg16(&p->d, p->pcie + 0x1a), link = cfg16(&p->d, p->pcie + 0x12);
        uint32_t slot = pci_read(&p->d, p->pcie + 0x14) >> 19;
        bool present = status & 0x40, up = link & 0x2000;
        for (int j = 0; j < ndevs && !present; j++)
            present = port_of(devs[j].d.bus) == p;
        sh_printf(sh, "  %02x:%02x.%u  slot %u: %s", p->d.bus, p->d.dev, p->d.fn, slot,
                  present ? "connected" : "empty");
        if (up)
            sh_printf(sh, ", link %s x%d", link_speed(link & 0xf), link >> 4 & 0x3f);
        sh_printf(sh, ", buses %02x-%02x\n", p->secondary, p->subordinate);
        for (int j = 0; j < ndevs; j++)
            if (port_of(devs[j].d.bus) == p)
                sh_printf(sh, "      %02x:%02x.%u  %s [%04x:%04x]\n", devs[j].d.bus, devs[j].d.dev, devs[j].d.fn,
                          kind(&devs[j].d), devs[j].d.vendor, devs[j].d.device);
    }
    if (!ports)
        sh_printf(sh, "\nNo PCIe hot-plug ports.\n");
    print_usb(sh);
}

COMMAND(thunderbolt, "docks", "system", "thunderbolt [rescan]",
        "Thunderbolt and USB4 controllers, hot-plug ports and docks; rescan: set up newly connected devices")
{
    if (argc > 1 && !strcmp(argv[1], "rescan")) {
        int n = refresh(sh, false);
        if (!n)
            sh_printf(sh, "nothing new behind the hot-plug ports\n");
        return 0;
    }
    if (argc > 1)
        return command_usage(sh, command_find("thunderbolt"));
    report(sh);
    return 0;
}

static const struct command *const commands[] = {&cmddef_thunderbolt};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(NULL, tick, &ext);
