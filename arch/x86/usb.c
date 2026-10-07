#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>

#include "io.h"
#include "usb.h"

#define MAX_HOSTS 4
#define MAX_DEVICES 32
#define MAX_POLLERS 4
#define TICK_MS 2

static struct usb_host *hosts[MAX_HOSTS];
static int host_count;
static struct usb_device *devices[MAX_DEVICES];
static void (*pollers[MAX_POLLERS])(void);
static int poller_count;
static volatile bool pending;
static volatile uint32_t now_ms;
static bool servicing;

static const struct usb_driver *const drivers[] = {&usb_hub_driver, &usb_hid_driver, &usb_storage_driver};

static void tick(void)
{
    now_ms += TICK_MS;
    for (int i = 0; i < poller_count; i++)
        pollers[i]();
}

uint32_t usb_now(void)
{
    return now_ms;
}

void usb_poll_register(void (*poll)(void))
{
    if (poller_count == MAX_POLLERS)
        return;
    pollers[poller_count++] = poll;
    if (poller_count == 1)
        timer_hook(tick);
}

void usb_host_register(struct usb_host *host)
{
    if (host_count < MAX_HOSTS)
        hosts[host_count++] = host;
}

void usb_port_changed(void)
{
    pending = true;
}

int usb_control(struct usb_device *dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data,
                uint16_t length)
{
    if (dev->gone)
        return E_NODEV;
    return dev->host->ops->control(dev, type, request, value, index, data, length);
}

const struct usb_endpoint *usb_find_endpoint(const struct usb_interface *intf, uint8_t type, bool in)
{
    for (int i = 0; i < intf->endpoint_count; i++) {
        const struct usb_endpoint *e = &intf->endpoints[i];
        if ((e->attributes & 3) == type && !!(e->address & USB_DIR_IN) == in)
            return e;
    }
    return NULL;
}

static void parse_config(struct usb_device *dev, const uint8_t *p, int length)
{
    struct usb_interface *intf = NULL;
    struct usb_endpoint *last = NULL;
    for (int at = 0; at + 2 <= length;) {
        int len = p[at], type = p[at + 1];
        if (len < 2 || at + len > length)
            break;
        const uint8_t *d = p + at;
        if (type == 4 && len >= 9) {
            intf = NULL;
            last = NULL;
            if (d[3] == 0 && dev->interface_count < USB_MAX_INTERFACES) {
                intf = &dev->interfaces[dev->interface_count++];
                memset(intf, 0, sizeof(*intf));
                intf->number = d[2];
                intf->cls = d[5];
                intf->subclass = d[6];
                intf->protocol = d[7];
            }
        } else if (type == 5 && len >= 7 && intf && intf->endpoint_count < USB_MAX_ENDPOINTS) {
            last = &intf->endpoints[intf->endpoint_count++];
            last->address = d[2];
            last->attributes = d[3];
            last->max_packet = (uint16_t)(d[4] | d[5] << 8);
            last->interval = d[6];
            last->burst = 0;
        } else if (type == 0x30 && len >= 6 && last) {
            last->burst = d[2];
        } else if (type == 0x21 && len >= 9 && intf) {
            intf->report_length = (uint16_t)(d[7] | d[8] << 8);
        }
        at += len;
    }
}

static void read_string(struct usb_device *dev, uint8_t index, char *out, size_t cap)
{
    uint8_t buf[128];
    if (!index || usb_control(dev, USB_DIR_IN, USB_GET_DESCRIPTOR, (uint16_t)(0x0300 | index), 0x0409, buf, 2) ||
        buf[0] < 4)
        return;
    uint8_t len = (uint8_t)MIN(buf[0], sizeof(buf));
    if (usb_control(dev, USB_DIR_IN, USB_GET_DESCRIPTOR, (uint16_t)(0x0300 | index), 0x0409, buf, len))
        return;
    size_t n = 0;
    for (int i = 2; i + 1 < len && n + 1 < cap; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | buf[i + 1] << 8);
        out[n++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

static const char *class_name(const struct usb_device *dev)
{
    for (int i = 0; i < dev->interface_count; i++) {
        const struct usb_interface *f = &dev->interfaces[i];
        if (f->cls == 3 && f->protocol == 1)
            return "USB keyboard";
        if (f->cls == 3 && f->protocol == 2)
            return "USB mouse";
        if (f->cls == 3)
            return "USB input device";
        if (f->cls == 8)
            return "USB storage";
        if (f->cls == 9)
            return "USB hub";
    }
    return "USB device";
}

static int add_device(struct usb_device *dev)
{
    for (int i = 0; i < MAX_DEVICES; i++)
        if (!devices[i]) {
            devices[i] = dev;
            return 0;
        }
    return E_NOMEM;
}

static void remove_device(struct usb_device *dev)
{
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i] == dev)
            devices[i] = NULL;
}

static int enumerate(struct usb_device *dev)
{
    int err = dev->host->ops->address(dev);
    if (err)
        return err;
    uint8_t desc[18];
    if ((err = usb_control(dev, USB_DIR_IN, USB_GET_DESCRIPTOR, 0x0100, 0, desc, 18)))
        return err;
    dev->vendor = (uint16_t)(desc[8] | desc[9] << 8);
    dev->product = (uint16_t)(desc[10] | desc[11] << 8);
    dev->cls = desc[4];
    uint8_t head[9];
    if ((err = usb_control(dev, USB_DIR_IN, USB_GET_DESCRIPTOR, 0x0200, 0, head, 9)))
        return err;
    uint16_t total = (uint16_t)MIN(head[2] | head[3] << 8, 1024);
    uint8_t *config = kmalloc(total);
    if (!config)
        return E_NOMEM;
    if ((err = usb_control(dev, USB_DIR_IN, USB_GET_DESCRIPTOR, 0x0200, 0, config, total))) {
        kfree(config);
        return err;
    }
    parse_config(dev, config, total);
    uint8_t value = config[5];
    kfree(config);

    read_string(dev, desc[15], dev->name, sizeof(dev->name));
    if (!dev->name[0])
        str_copy(dev->name, class_name(dev), sizeof(dev->name));
    if (dev->cls == 9 || (dev->interface_count && dev->interfaces[0].cls == 9))
        usb_hub_prepare(dev);
    if ((err = dev->host->ops->configure(dev)))
        return err;
    if ((err = usb_control(dev, 0, USB_SET_CONFIGURATION, value, 0, NULL, 0)))
        return err;
    return 0;
}

struct usb_device *usb_attach(struct usb_host *host, struct usb_device *parent, int port, int root_port, int speed)
{
    struct usb_device *dev = kzalloc(sizeof(*dev));
    if (!dev)
        return NULL;
    dev->host = host;
    dev->parent = parent;
    dev->port = port;
    dev->root_port = root_port;
    dev->speed = speed;
    if (parent) {
        dev->depth = parent->depth + 1;
        dev->route = parent->route | (uint32_t)MIN(port, 15) << (4 * parent->depth);
    }
    int err = add_device(dev);
    if (!err)
        err = enumerate(dev);
    if (err) {
        klog("usb: device on %s port %d failed: %s", host->name, port, err_str(err));
        remove_device(dev);
        host->ops->release(dev);
        kfree(dev);
        return NULL;
    }
    for (int i = 0; i < dev->interface_count; i++)
        for (size_t k = 0; k < ARRAY_SIZE(drivers); k++)
            if (drivers[k]->match(dev, &dev->interfaces[i])) {
                if (drivers[k]->attach(dev, i) == 0)
                    dev->bound[i] = drivers[k];
                break;
            }
    klog("usb: %s (%04x:%04x) on %s port %d", dev->name, dev->vendor, dev->product, host->name, port);
    return dev;
}

void usb_detach(struct usb_device *dev)
{
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i] && devices[i]->parent == dev)
            usb_detach(devices[i]);
    dev->gone = true;
    for (int i = 0; i < dev->interface_count; i++)
        if (dev->bound[i] && dev->bound[i]->detach)
            dev->bound[i]->detach(dev, i);
    klog("usb: %s disconnected", dev->name);
    remove_device(dev);
    dev->host->ops->release(dev);
    kfree(dev);
}

void usb_service(void)
{
    if (!pending || servicing || hal_in_interrupt())
        return;
    servicing = true;
    for (int pass = 0; pass < 8 && pending; pass++) {
        pending = false;
        for (int i = 0; i < host_count; i++)
            hosts[i]->ops->service(hosts[i]);
        for (int i = 0; i < MAX_DEVICES; i++)
            if (devices[i] && devices[i]->is_hub)
                usb_hub_service(devices[i]);
    }
    servicing = false;
}

void usb_init(void)
{
    xhci_init();
    usb_service();
}

int hal_usb_devices(struct usb_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_DEVICES && n < max; i++) {
        struct usb_device *d = devices[i];
        if (!d)
            continue;
        struct usb_info *u = &out[n++];
        memset(u, 0, sizeof(*u));
        str_copy(u->name, d->name, sizeof(u->name));
        str_copy(u->host, d->host->name, sizeof(u->host));
        for (int k = 0; k < d->interface_count; k++)
            if (d->bound[k]) {
                str_copy(u->driver, d->bound[k]->name, sizeof(u->driver));
                break;
            }
        u->vendor = d->vendor;
        u->product = d->product;
        u->speed = d->speed;
        u->port = d->port;
        u->depth = d->depth;
    }
    return n;
}
