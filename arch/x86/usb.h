#ifndef SYNTH_X86_USB_H
#define SYNTH_X86_USB_H

#include <synth/types.h>

enum usb_speed {
    USB_FULL = 1,
    USB_LOW = 2,
    USB_HIGH = 3,
    USB_SUPER = 4,
};

#define USB_DIR_IN 0x80
#define USB_REQ_CLASS 0x20
#define USB_REQ_INTERFACE 0x01
#define USB_REQ_ENDPOINT 0x02
#define USB_REQ_OTHER 0x03

#define USB_GET_STATUS 0
#define USB_CLEAR_FEATURE 1
#define USB_SET_FEATURE 3
#define USB_GET_DESCRIPTOR 6
#define USB_SET_CONFIGURATION 9

#define USB_MAX_ENDPOINTS 8
#define USB_MAX_INTERFACES 4

struct usb_endpoint {
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet;
    uint8_t interval;
    uint8_t burst;
};

struct usb_interface {
    uint8_t number, cls, subclass, protocol;
    uint16_t report_length;
    int endpoint_count;
    struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
};

struct usb_host;

struct usb_device {
    struct usb_host *host;
    struct usb_device *parent;
    int port;
    int root_port;
    uint32_t route;
    int depth;
    int speed;
    int slot;
    void *hc;
    uint16_t vendor, product;
    uint8_t cls;
    uint8_t max_packet0;
    char name[48];
    bool gone;
    bool is_hub;
    int hub_ports;
    uint8_t tt_think;
    int interface_count;
    struct usb_interface interfaces[USB_MAX_INTERFACES];
    const struct usb_driver *bound[USB_MAX_INTERFACES];
    void *priv[USB_MAX_INTERFACES];
};

typedef void (*usb_complete)(struct usb_device *dev, void *ctx, const uint8_t *data, int length);

struct usb_host_ops {
    int (*address)(struct usb_device *dev);
    int (*control)(struct usb_device *dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data,
                   uint16_t length);
    int (*configure)(struct usb_device *dev);
    int (*transfer)(struct usb_device *dev, uint8_t endpoint, void *dma, size_t length, uint32_t timeout_ms);
    int (*interrupt)(struct usb_device *dev, uint8_t endpoint, uint16_t length, usb_complete done, void *ctx);
    int (*clear_halt)(struct usb_device *dev, uint8_t endpoint);
    void (*release)(struct usb_device *dev);
    void (*service)(struct usb_host *host);
};

struct usb_host {
    const struct usb_host_ops *ops;
    const char *name;
    void *priv;
};

struct usb_driver {
    const char *name;
    bool (*match)(const struct usb_device *dev, const struct usb_interface *intf);
    int (*attach)(struct usb_device *dev, int interface);
    void (*detach)(struct usb_device *dev, int interface);
};

struct usb_device *usb_attach(struct usb_host *host, struct usb_device *parent, int port, int root_port, int speed);
void usb_detach(struct usb_device *dev);
int usb_control(struct usb_device *dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data,
                uint16_t length);
const struct usb_endpoint *usb_find_endpoint(const struct usb_interface *intf, uint8_t type, bool in);
void usb_port_changed(void);
void usb_poll_register(void (*poll)(void));
void usb_host_register(struct usb_host *host);

extern const struct usb_driver usb_hub_driver, usb_hid_driver, usb_storage_driver;

void xhci_init(void);
void usb_init(void);
void usb_service(void);
void usb_hub_service(struct usb_device *hub);
int usb_hub_prepare(struct usb_device *dev);
uint32_t usb_now(void);
void keyboard_scancode(uint8_t code);
void mouse_report(int dx, int dy, int wheel, unsigned buttons);
void mouse_absolute(int x, int y, int wheel, unsigned buttons);
void timer_hook(void (*fn)(void));

#endif
