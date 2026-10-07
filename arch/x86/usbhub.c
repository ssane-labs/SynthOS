#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "usb.h"

#define MAX_HUB_PORTS 15
#define HUB_CLASS_IN (USB_DIR_IN | USB_REQ_CLASS)
#define PORT_CLASS_IN (USB_DIR_IN | USB_REQ_CLASS | USB_REQ_OTHER)
#define PORT_CLASS_OUT (USB_REQ_CLASS | USB_REQ_OTHER)

enum {
    FEATURE_PORT_RESET = 4,
    FEATURE_PORT_POWER = 8,
};

struct hub {
    struct usb_device *dev;
    volatile bool changed;
    uint8_t power_delay;
    struct usb_device *children[MAX_HUB_PORTS + 1];
};

static const uint8_t change_features[8] = {16, 17, 18, 19, 20, 29, 25, 26};

static bool hub_match(const struct usb_device *dev, const struct usb_interface *intf)
{
    UNUSED(dev);
    return intf->cls == 9;
}

int usb_hub_prepare(struct usb_device *dev)
{
    uint8_t desc[16];
    bool super = dev->speed == USB_SUPER;
    int err = usb_control(dev, HUB_CLASS_IN, USB_GET_DESCRIPTOR, super ? 0x2a00 : 0x2900, 0, desc, 9);
    if (err)
        return err;
    dev->is_hub = true;
    dev->hub_ports = MIN(desc[2], MAX_HUB_PORTS);
    dev->tt_think = (uint8_t)((desc[3] >> 5) & 3);
    return 0;
}

static struct hub *hub_of(struct usb_device *dev)
{
    for (int i = 0; i < dev->interface_count; i++)
        if (dev->bound[i] == &usb_hub_driver || (!dev->bound[i] && dev->priv[i] && dev->interfaces[i].cls == 9))
            return dev->priv[i];
    return NULL;
}

static void hub_status(struct usb_device *dev, void *ctx, const uint8_t *data, int length)
{
    UNUSED(dev);
    struct hub *h = ctx;
    for (int i = 0; i < length; i++)
        if (data[i]) {
            h->changed = true;
            usb_port_changed();
            return;
        }
}

static int hub_attach(struct usb_device *dev, int index)
{
    if (!dev->is_hub && usb_hub_prepare(dev))
        return E_IO;
    uint8_t desc[16];
    bool super = dev->speed == USB_SUPER;
    if (usb_control(dev, HUB_CLASS_IN, USB_GET_DESCRIPTOR, super ? 0x2a00 : 0x2900, 0, desc, 9))
        return E_IO;
    struct hub *h = kzalloc(sizeof(*h));
    if (!h)
        return E_NOMEM;
    h->dev = dev;
    h->power_delay = desc[5];
    dev->priv[index] = h;
    if (super)
        usb_control(dev, USB_REQ_CLASS, 12, (uint16_t)dev->depth, 0, NULL, 0);
    for (int port = 1; port <= dev->hub_ports; port++)
        usb_control(dev, PORT_CLASS_OUT, USB_SET_FEATURE, FEATURE_PORT_POWER, (uint16_t)port, NULL, 0);
    hal_sleep_ms(h->power_delay * 2u + 100u);
    const struct usb_endpoint *e = usb_find_endpoint(&dev->interfaces[index], 3, true);
    if (e)
        dev->host->ops->interrupt(dev, e->address, (uint16_t)MIN(e->max_packet, 8), hub_status, h);
    h->changed = true;
    usb_port_changed();
    return 0;
}

static void hub_detach(struct usb_device *dev, int index)
{
    kfree(dev->priv[index]);
    dev->priv[index] = NULL;
}

static bool port_status(struct usb_device *dev, int port, uint16_t *status, uint16_t *change)
{
    uint8_t st[4];
    if (usb_control(dev, PORT_CLASS_IN, USB_GET_STATUS, 0, (uint16_t)port, st, 4))
        return false;
    *status = (uint16_t)(st[0] | st[1] << 8);
    *change = (uint16_t)(st[2] | st[3] << 8);
    return true;
}

static void clear_changes(struct usb_device *dev, int port, uint16_t change)
{
    for (int bit = 0; bit < 8; bit++)
        if (change & (1u << bit))
            usb_control(dev, PORT_CLASS_OUT, USB_CLEAR_FEATURE, change_features[bit], (uint16_t)port, NULL, 0);
}

static int reset_port(struct usb_device *dev, int port)
{
    uint16_t status = 0, change = 0;
    if (usb_control(dev, PORT_CLASS_OUT, USB_SET_FEATURE, FEATURE_PORT_RESET, (uint16_t)port, NULL, 0))
        return E_IO;
    for (int i = 0; i < 50; i++) {
        hal_sleep_ms(10);
        if (!port_status(dev, port, &status, &change))
            return E_IO;
        if ((change & 0x10) || !(status & 0x10))
            break;
    }
    clear_changes(dev, port, change);
    if (!port_status(dev, port, &status, &change) || !(status & 2))
        return E_IO;
    hal_sleep_ms(10);
    if (dev->speed == USB_SUPER)
        return USB_SUPER;
    return status & 0x200 ? USB_LOW : status & 0x400 ? USB_HIGH : USB_FULL;
}

void usb_hub_service(struct usb_device *dev)
{
    struct hub *h = hub_of(dev);
    if (!h || !h->changed || dev->gone)
        return;
    h->changed = false;
    for (int port = 1; port <= dev->hub_ports; port++) {
        uint16_t status, change;
        if (!port_status(dev, port, &status, &change))
            continue;
        clear_changes(dev, port, change);
        bool connected = status & 1;
        if (h->children[port] && (!connected || (change & 1))) {
            usb_detach(h->children[port]);
            h->children[port] = NULL;
        }
        if (!connected || h->children[port])
            continue;
        hal_sleep_ms(50);
        int speed = reset_port(dev, port);
        if (speed > 0)
            h->children[port] = usb_attach(dev->host, dev, port, dev->root_port, speed);
    }
}

const struct usb_driver usb_hub_driver = {
    .name = "hub",
    .match = hub_match,
    .attach = hub_attach,
    .detach = hub_detach,
};
