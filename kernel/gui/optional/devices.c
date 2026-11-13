#include <synth/block.h>
#include <synth/command.h>
#include <synth/hal.h>
#include <synth/module.h>
#include <synth/net.h>

#include "../../../arch/x86/io.h"
#include "systool.h"

/* Device Manager: the computer's devices by kind — the PCI functions with the driver SynthOS has for each (or that
   it has none), the disks, the USB devices with their drivers, the displays, keyboard and mouse, the processors.
   A device without a driver is a problem and shows first in its group. */

#define MAX_DEVS 96

enum dev_state { DEV_OK, DEV_BASIC, DEV_NONE, DEV_MISSING };

enum dev_group {
    G_DISPLAY,
    G_DISK,
    G_STORAGE,
    G_NETWORK,
    G_SOUND,
    G_USB,
    G_USBDEV,
    G_INPUT,
    G_CPU,
    G_SYSTEM,
    G_OTHER,
    G_COUNT,
};

static const char *const group_names[G_COUNT] = {
    "Display adapters", "Disk drives", "Storage controllers", "Network adapters", "Sound",       "USB controllers",
    "USB devices",      "Keyboards and mice", "Processors", "System devices",      "Other devices"};
static const enum symbol_id group_syms[G_COUNT] = {SYM_DISPLAY, SYM_DISK, SYM_CHIP,  SYM_NETWORK, SYM_VOLUME, SYM_USB,
                                                   SYM_USB,     SYM_KEYBOARD, SYM_CHIP, SYM_GEAR, SYM_INFO};

struct device {
    enum dev_group group;
    enum dev_state state;
    char name[64], driver[24], where[32], ids[24];
    uint32_t irq;
};

static struct device devs[MAX_DEVS];
static int dev_count;

static const char *vendor_name(uint16_t v)
{
    static const struct {
        uint16_t id;
        const char *name;
    } vendors[] = {{0x8086, "Intel"},  {0x1022, "AMD"},     {0x1002, "AMD"},      {0x10de, "NVIDIA"},
                   {0x10ec, "Realtek"}, {0x1af4, "VirtIO"}, {0x1b36, "Red Hat"}, {0x1234, "QEMU"},
                   {0x15ad, "VMware"}, {0x80ee, "VirtualBox"}, {0x14e4, "Broadcom"}, {0x168c, "Qualcomm Atheros"},
                   {0x144d, "Samsung"}, {0x15b7, "SanDisk"}, {0x1c5c, "SK hynix"}, {0x1987, "Phison"},
                   {0x1b21, "ASMedia"}, {0x1912, "Renesas"}, {0x17cb, "Qualcomm"}, {0x14c3, "MediaTek"}};
    for (size_t i = 0; i < ARRAY_SIZE(vendors); i++)
        if (vendors[i].id == v)
            return vendors[i].name;
    return NULL;
}

static bool nic_driver(const char *prefix)
{
    for (int i = 0;; i++) {
        struct net_iface *ifc = net_interface(i);
        if (!ifc)
            return false;
        if (ifc->dev && ifc->dev->driver && !strncmp(ifc->dev->driver, prefix, strlen(prefix)))
            return true;
    }
}

static struct device *add(enum dev_group g, enum dev_state st, const char *name, const char *driver)
{
    if (dev_count >= MAX_DEVS)
        return NULL;
    struct device *d = &devs[dev_count++];
    memset(d, 0, sizeof(*d));
    d->group = g;
    d->state = st;
    str_copy(d->name, name, sizeof(d->name));
    str_copy(d->driver, driver ? driver : "", sizeof(d->driver));
    return d;
}

/* what a PCI function is, and which driver of SynthOS (if any) takes it */
static void pci_found(const struct pci_device *p, void *ctx)
{
    UNUSED(ctx);
    uint8_t c = p->class_code, s = p->subclass, i = p->prog_if;
    enum dev_group g = G_OTHER;
    enum dev_state st = DEV_NONE;
    const char *what = "Unknown device", *driver = NULL;
    if (c == 0x03) {
        g = G_DISPLAY;
        what = s == 0 ? "VGA display adapter" : "Display controller";
        bool own = (p->vendor == 0x1234 && p->device == 0x1111) || (p->vendor == 0x80ee && p->device == 0xbeef);
        st = own ? DEV_OK : DEV_BASIC;
        driver = own ? "bochs-display" : "framebuffer";
    } else if (c == 0x01) {
        g = G_STORAGE;
        if (s == 0x01) {
            what = "IDE controller";
            driver = "ata";
        } else if (s == 0x06) {
            what = "SATA AHCI controller";
            driver = "ahci";
        } else if (s == 0x08) {
            what = "NVMe controller";
            driver = "nvme";
        } else if (s == 0x04 && p->vendor == 0x8086) {
            what = "Intel Volume Management Device";
            driver = "vmd";
        } else {
            what = s == 0x00 ? "SCSI controller" : s == 0x07 ? "SAS controller" : "Storage controller";
        }
        st = driver ? DEV_OK : DEV_NONE;
    } else if (c == 0x02) {
        g = G_NETWORK;
        what = s == 0x80 ? "Wireless network adapter" : "Ethernet adapter";
        if (p->vendor == 0x8086 && nic_driver("e1000"))
            driver = "e1000";
        else if (p->vendor == 0x10ec && (nic_driver("rtl") || nic_driver("8139")))
            driver = "realtek";
        st = driver ? DEV_OK : DEV_NONE;
    } else if (c == 0x0d) {
        g = G_NETWORK;
        what = "Wireless controller";
    } else if (c == 0x04) {
        g = G_SOUND;
        what = s == 0x03 ? "High Definition Audio" : s == 0x01 ? "Audio device" : "Multimedia device";
        if (s == 0x03 || (s == 0x01 && (p->vendor == 0x8086 || p->vendor == 0x1022))) {
            driver = "hda";
            st = DEV_OK;
        }
    } else if (c == 0x0c && s == 0x03) {
        g = G_USB;
        static const char *const kinds[] = {"USB 1.1 UHCI controller", "USB 1.1 OHCI controller",
                                            "USB 2.0 EHCI controller", "USB 3 xHCI controller"};
        what = i <= 0x30 && i % 0x10 == 0 ? kinds[i / 0x10] : "USB controller";
        if (i == 0x30) {
            driver = "xhci";
            st = DEV_OK;
        }
    } else if (c == 0x0c && s == 0x05) {
        g = G_SYSTEM;
        what = "SMBus controller";
        st = DEV_OK;
        driver = "(not needed)";
    } else if (c == 0x06) {
        g = G_SYSTEM;
        static const char *const bridges[] = {"Host bridge", "ISA bridge", "EISA bridge", "MCA bridge",
                                              "PCI bridge",  "PCMCIA bridge", "NuBus bridge", "CardBus bridge"};
        what = s < ARRAY_SIZE(bridges) ? bridges[s] : "Bridge";
        st = DEV_OK;
        driver = "pci";
    } else if (c == 0x08 || c == 0x11 || c == 0x10 || c == 0x13) {
        g = G_SYSTEM;
        what = c == 0x08 ? "System peripheral" : c == 0x10 ? "Encryption controller" : "Signal processing controller";
        st = c == 0x08 ? DEV_OK : DEV_NONE;
        driver = c == 0x08 ? "(not needed)" : NULL;
    } else if (c == 0x0c && s == 0x80) {
        g = G_SYSTEM;
        what = "Serial bus controller";
    } else if (c == 0x07) {
        g = G_SYSTEM;
        what = "Communication controller";
    }
    char name[64];
    const char *v = vendor_name(p->vendor);
    ksnprintf(name, sizeof(name), "%s%s%s", v ? v : "", v ? " " : "", what);
    struct device *d = add(g, st, name, driver);
    if (d) {
        ksnprintf(d->where, sizeof(d->where), "PCI %02x:%02x.%x", p->bus, p->dev, p->fn);
        ksnprintf(d->ids, sizeof(d->ids), "%04x:%04x", p->vendor, p->device);
        d->irq = pci_read(p, 0x3c) & 0xff;
    }
}

static void collect(void)
{
    dev_count = 0;
    pci_foreach(pci_found, NULL);
    for (struct blockdev *b = block_list(); b; b = b->next) {
        if (b->parent)
            continue;
        char name[64], size[16];
        fmt_size(size, sizeof(size), (uint64_t)b->sectors * BLOCK_SIZE);
        ksnprintf(name, sizeof(name), "%s (%s)", b->model[0] ? b->model : "Disk", size);
        struct device *d = add(G_DISK, DEV_OK, name, "disk");
        if (d)
            str_copy(d->where, b->name, sizeof(d->where));
    }
    static struct usb_info usb[32];
    int n = hal_usb_devices(usb, 32);
    for (int k = 0; k < n; k++) {
        struct device *d = add(G_USBDEV, usb[k].driver[0] ? DEV_OK : DEV_NONE, usb[k].name[0] ? usb[k].name : "USB device",
                               usb[k].driver[0] ? usb[k].driver : NULL);
        if (d) {
            ksnprintf(d->where, sizeof(d->where), "%s port %d", usb[k].host, usb[k].port);
            ksnprintf(d->ids, sizeof(d->ids), "%04x:%04x", usb[k].vendor, usb[k].product);
        }
    }
    for (int k = 0; k < wm_screen_count(); k++) {
        struct rect r = wm_screen_rect(k);
        char name[64];
        ksnprintf(name, sizeof(name), "Display %d (%dx%d)", k + 1, r.w, r.h);
        add(G_DISPLAY, DEV_OK, name, "monitor");
    }
    add(G_INPUT, DEV_OK, "Keyboard", "keyboard");
    add(G_INPUT, DEV_OK, "Mouse", "mouse");
    for (int k = 0; k < hal_cpu_count(); k++) {
        char name[32];
        ksnprintf(name, sizeof(name), "Processor %d", k);
        add(G_CPU, DEV_OK, name, "cpu");
    }
}

static const char *state_text(enum dev_state s)
{
    static const char *const t[] = {"Working", "Basic driver (no acceleration)", "No driver", "Missing"};
    return t[s];
}

static int problems(void)
{
    int p = 0;
    for (int i = 0; i < dev_count; i++)
        p += devs[i].state == DEV_NONE;
    return p;
}

/* ---- the window ---- */

struct devwin {
    struct ui_state st;
    int selected, scroll;
    bool only_problems;
};

static struct window *open_win;

/* the rows: group headers and devices, problems first in each group */
static int rows(int *out, int max, bool only_problems)
{
    int n = 0;
    for (int g = 0; g < G_COUNT; g++) {
        int head = n;
        if (n < max)
            out[n++] = -1 - g;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < dev_count && n < max; i++)
                if (devs[i].group == (enum dev_group)g && (devs[i].state == DEV_NONE) == !pass &&
                    (!only_problems || devs[i].state == DEV_NONE))
                    out[n++] = i;
        if (n == head + 1)
            n = head; /* an empty group */
    }
    return n;
}

static void frame(struct ui *ui, void *data)
{
    struct devwin *w = data;
    int W = ui->win->surface.w, H = ui->win->surface.h;
    static const struct tool_item items[] = {{SYM_LIST, "All Devices", "Devices"}, {SYM_INFO, "Problems", NULL}};
    int page = w->only_problems;
    if (tool_sidebar(ui, H, items, 2, &page)) {
        w->only_problems = page;
        w->selected = -1;
        w->scroll = 0;
    }
    char sub[48];
    int bad = problems();
    ksnprintf(sub, sizeof(sub), bad ? "%d devices, %d without a driver" : "%d devices, all working", dev_count, bad);
    struct rect a = tool_page(ui, W, H, w->only_problems ? "Problems" : "All Devices", sub);
    if (ui_symbol_button(ui, (struct rect){W - 76, 12, 28, 28}, SYM_RESTART, false))
        collect();
    int list_w = a.w * 3 / 5, x = a.x + 12, y = a.y + 8, row_h = 28;
    int order[MAX_DEVS + G_COUNT], n = rows(order, ARRAY_SIZE(order), w->only_problems);
    int visible = (a.h - 16) / row_h;
    if (ui->e && ui->e->type == EV_WHEEL && rect_contains((struct rect){x, y, list_w, a.h}, ui->e->x, ui->e->y))
        w->scroll = CLAMP(w->scroll + ui->e->wheel * 3, 0, MAX(n - visible, 0));
    if (!n)
        ui_label(ui, (struct rect){x, y + 40, list_w, 20}, "Every device has a driver", &font_sans, style.tertiary,
                 ALIGN_CENTER);
    for (int k = w->scroll; k < n && k - w->scroll < visible; k++) {
        struct rect r = {x, y + (k - w->scroll) * row_h, list_w - 12, row_h};
        if (order[k] < 0) {
            int g = -1 - order[k];
            if (ui->c)
                ui_settings_icon(ui->c, r.x + 4, r.y + 5, 18, group_syms[g], style.accent);
            ui_label(ui, (struct rect){r.x + 30, r.y, r.w - 30, r.h}, group_names[g], &font_bold, style.text,
                     ALIGN_LEFT);
            continue;
        }
        const struct device *d = &devs[order[k]];
        bool sel = w->selected == order[k];
        if (ui->c && sel)
            gfx_round(ui->c, r, 6, style.selection);
        if (ui->c && d->state != DEV_OK)
            gfx_circle(ui->c, r.x + 38, r.y + r.h / 2, 4, d->state == DEV_NONE ? style.error : style.warning);
        ui_label(ui, (struct rect){r.x + 50, r.y, r.w - 56, r.h}, d->name, &font_sans, ui_row_text(sel), ALIGN_LEFT);
        if (ui_hover(ui, r) && ui->e && ui->e->type == EV_MOUSE_DOWN)
            w->selected = order[k];
    }
    struct rect info = {a.x + list_w + 8, a.y + 16, a.w - list_w - 24, 7 * 30 + 20};
    if (w->selected < 0 || w->selected >= dev_count) {
        ui_label(ui, (struct rect){info.x, info.y, info.w, 30}, "Select a device", &font_sans, style.tertiary,
                 ALIGN_CENTER);
        return;
    }
    const struct device *d = &devs[w->selected];
    if (ui->c) {
        gfx_round(ui->c, info, 12, style.group);
        ui_wrap_text(ui->c, (struct rect){info.x + 14, info.y + 10, info.w - 28, 40}, &font_bold, d->name, style.text);
    }
    char irq[12];
    ksnprintf(irq, sizeof(irq), d->irq && d->irq != 0xff ? "%u" : "-", d->irq);
    const char *keys[] = {"Kind", "Status", "Driver", "Location", "IDs", "Interrupt"};
    const char *vals[] = {group_names[d->group], state_text(d->state), d->driver[0] ? d->driver : "none",
                          d->where[0] ? d->where : "-", d->ids[0] ? d->ids : "-", irq};
    for (int k = 0; k < 6; k++) {
        struct rect r = {info.x + 14, info.y + 50 + k * 28, info.w - 28, 28};
        ui_label(ui, r, keys[k], &font_small, style.secondary, ALIGN_LEFT);
        ui_label(ui, r, vals[k], &font_small, k == 1 && d->state != DEV_OK ? style.error : style.text, ALIGN_RIGHT);
    }
    if (d->state == DEV_NONE && ui->c)
        ui_wrap_text(ui->c, (struct rect){info.x, info.y + info.h + 12, info.w, 80}, &font_small,
                     "SynthOS has no driver for this device yet, so it is not used. Windows programs cannot use it "
                     "either.",
                     style.secondary);
}

static void paint(struct window *win, struct canvas *c)
{
    struct devwin *w = win->data;
    ui_run(win, &w->st, c, NULL, frame, w);
}

static void event(struct window *win, struct gui_event *e)
{
    struct devwin *w = win->data;
    ui_run(win, &w->st, NULL, e, frame, w);
}

static void destroy(struct window *win)
{
    kfree(win->data);
    if (open_win == win)
        open_win = NULL;
}

static const struct window_ops ops = {paint, event, NULL, destroy};

static void launch(const char *arg)
{
    UNUSED(arg);
    if (open_win && wm_window_alive(open_win)) {
        wm_show(open_win);
        return;
    }
    struct devwin *w = kzalloc(sizeof(*w));
    if (!w)
        return;
    w->selected = -1;
    collect();
    open_win = tool_open("Device Manager", ICON_SYSINFO, 860, 560, &ops, w);
    if (!open_win)
        kfree(w);
}

static const struct gui_app devices_app = {.name = "Device Manager",
                                           .description = "The computer's devices, their drivers and problems",
                                           .icon = ICON_SYSINFO,
                                           .launch = launch};

COMMAND(lsdev, "devmgmt", "system", "lsdev [problems]", "list the devices by kind with their drivers")
{
    bool only = argc > 1 && !strcmp(argv[1], "problems");
    collect();
    int order[MAX_DEVS + G_COUNT], n = rows(order, ARRAY_SIZE(order), only);
    for (int k = 0; k < n; k++) {
        if (order[k] < 0) {
            sh_printf(sh, "%s\n", group_names[-1 - order[k]]);
            continue;
        }
        const struct device *d = &devs[order[k]];
        sh_printf(sh, "  %-40s %-14s %s%s%s\n", d->name, d->driver[0] ? d->driver : "-", d->where,
                  d->state == DEV_NONE ? "  [no driver]" : "", d->state == DEV_BASIC ? "  [basic driver]" : "");
    }
    sh_printf(sh, "%d devices, %d without a driver\n", dev_count, problems());
    return 0;
}

static const struct command *const commands[] = {&cmddef_lsdev};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(&devices_app, NULL, &ext);
