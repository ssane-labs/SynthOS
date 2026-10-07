#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"
#include "usb.h"

#define RING_TRBS 256
#define MAX_CONTROLLERS 4
#define MAX_PORTS 64
#define COMMAND_TIMEOUT 3000
#define PAGE 4096u

enum {
    TRB_NORMAL = 1,
    TRB_SETUP = 2,
    TRB_DATA = 3,
    TRB_STATUS = 4,
    TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9,
    TRB_DISABLE_SLOT = 10,
    TRB_ADDRESS = 11,
    TRB_CONFIGURE = 12,
    TRB_EVALUATE = 13,
    TRB_RESET_EP = 14,
    TRB_STOP_EP = 15,
    TRB_SET_DEQUEUE = 16,
    TRB_TRANSFER_EVENT = 32,
    TRB_COMMAND_EVENT = 33,
    TRB_PORT_EVENT = 34,
};

enum {
    TRB_CYCLE = 1u << 0,
    TRB_TC = 1u << 1,
    TRB_ISP = 1u << 2,
    TRB_CHAIN = 1u << 4,
    TRB_IOC = 1u << 5,
    TRB_IDT = 1u << 6,
};

enum {
    CC_SUCCESS = 1,
    CC_STALL = 6,
    CC_SHORT = 13,
};

enum {
    OP_USBCMD = 0x00,
    OP_USBSTS = 0x04,
    OP_PAGESIZE = 0x08,
    OP_CRCR = 0x18,
    OP_DCBAAP = 0x30,
    OP_CONFIG = 0x38,
};

#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PP (1u << 9)
#define PORT_PRC (1u << 21)
#define PORT_WPR (1u << 31)
#define PORT_KEEP 0x4e00ffe9u
#define PORT_CHANGES 0x00fe0000u

struct ring {
    volatile uint32_t *trb;
    uint32_t phys;
    int enqueue;
    uint32_t cycle;
};

struct endpoint {
    struct ring ring;
    volatile bool done;
    volatile uint8_t code;
    volatile uint32_t residue;
    volatile uint32_t wait;
    uint8_t *buffer;
    uint16_t length;
    usb_complete complete;
    void *ctx;
    bool periodic;
};

struct xdev {
    int slot;
    uint8_t *out;
    uint8_t *in;
    uint8_t *dma;
    struct endpoint *ep[32];
    struct usb_device *dev;
};

struct xhci {
    struct usb_host host;
    char name[24];
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *doorbell;
    int ports, slots;
    uint32_t csz;
    uint64_t *dcbaa;
    struct ring command;
    volatile uint32_t *events;
    uint32_t events_phys;
    int event_index;
    uint32_t event_cycle;
    volatile bool command_done;
    volatile uint8_t command_code, command_slot;
    volatile uint32_t command_wait;
    struct xdev *devs[256];
    uint8_t protocol[MAX_PORTS + 1];
    struct usb_device *root[MAX_PORTS + 1];
    volatile uint32_t changed[(MAX_PORTS + 32) / 32];
    bool polling;
};

static struct xhci *controllers[MAX_CONTROLLERS];
static int controller_count;

static uint32_t rd(volatile uint8_t *base, uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

static void wr(volatile uint8_t *base, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(base + reg) = value;
}

static void wr64(volatile uint8_t *base, uint32_t reg, uint32_t low)
{
    wr(base, reg, low);
    wr(base, reg + 4, 0);
}

static void *dma_page(void)
{
    void *p = kmalloc_pages(PAGE);
    if (p)
        memset(p, 0, PAGE);
    return p;
}

static bool ring_init(struct ring *r)
{
    r->trb = dma_page();
    if (!r->trb)
        return false;
    r->phys = vmm_phys((const void *)r->trb);
    r->enqueue = 0;
    r->cycle = 1;
    volatile uint32_t *link = r->trb + (RING_TRBS - 1) * 4;
    link[0] = r->phys;
    link[3] = TRB_LINK << 10 | TRB_TC;
    return true;
}

static uint32_t ring_push(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status, uint32_t control)
{
    volatile uint32_t *t = r->trb + r->enqueue * 4;
    uint32_t phys = r->phys + (uint32_t)r->enqueue * 16;
    t[0] = p0;
    t[1] = p1;
    t[2] = status;
    cpu_barrier();
    t[3] = (control & ~TRB_CYCLE) | r->cycle;
    if (++r->enqueue == RING_TRBS - 1) {
        volatile uint32_t *link = r->trb + r->enqueue * 4;
        link[3] = TRB_LINK << 10 | TRB_TC | (control & TRB_CHAIN) | r->cycle;
        r->enqueue = 0;
        r->cycle ^= 1;
    }
    return phys;
}

static void ring_free(struct ring *r)
{
    if (r->trb)
        kfree_pages((void *)r->trb);
    r->trb = NULL;
}

static void endpoint_done(struct xhci *x, struct endpoint *ep, struct xdev *d, int dci, uint8_t code, uint32_t residue)
{
    if (!ep->periodic) {
        ep->code = code;
        ep->residue = residue;
        ep->done = true;
        return;
    }
    if (code != CC_SUCCESS && code != CC_SHORT)
        return;
    int length = (int)ep->length - (int)MIN(residue, ep->length);
    if (ep->complete && d->dev && !d->dev->gone)
        ep->complete(d->dev, ep->ctx, ep->buffer, length);
    ep->wait = ring_push(&ep->ring, vmm_phys(ep->buffer), 0, ep->length, TRB_NORMAL << 10 | TRB_IOC | TRB_ISP);
    x->doorbell[d->slot] = (uint32_t)dci;
}

static void handle_event(struct xhci *x, volatile uint32_t *e)
{
    uint32_t type = (e[3] >> 10) & 0x3f;
    uint8_t code = (uint8_t)(e[2] >> 24);
    if (type == TRB_COMMAND_EVENT) {
        if (e[0] == x->command_wait) {
            x->command_code = code;
            x->command_slot = (uint8_t)(e[3] >> 24);
            x->command_done = true;
        }
    } else if (type == TRB_TRANSFER_EVENT) {
        int slot = (int)(e[3] >> 24), dci = (int)(e[3] >> 16) & 0x1f;
        struct xdev *d = x->devs[slot];
        struct endpoint *ep = d ? d->ep[dci] : NULL;
        if (ep && (e[0] == ep->wait || (code != CC_SUCCESS)))
            endpoint_done(x, ep, d, dci, code, e[2] & 0xffffff);
    } else if (type == TRB_PORT_EVENT) {
        uint32_t port = e[0] >> 24;
        if (port && port <= MAX_PORTS) {
            x->changed[port / 32] |= 1u << (port % 32);
            usb_port_changed();
        }
    }
}

static void process(struct xhci *x)
{
    bool any = false;
    for (;;) {
        volatile uint32_t *e = x->events + x->event_index * 4;
        if ((e[3] & TRB_CYCLE) != x->event_cycle)
            break;
        handle_event(x, e);
        if (++x->event_index == RING_TRBS) {
            x->event_index = 0;
            x->event_cycle ^= 1;
        }
        any = true;
    }
    if (any)
        wr64(x->rt, 0x38, (x->events_phys + (uint32_t)x->event_index * 16) | 8);
}

static void poll(struct xhci *x)
{
    uint32_t flags = cpu_irq_save();
    if (!x->polling) {
        x->polling = true;
        process(x);
        x->polling = false;
    }
    cpu_irq_restore(flags);
}

static void poll_all(void)
{
    for (int i = 0; i < controller_count; i++)
        poll(controllers[i]);
}

static bool wait_flag(struct xhci *x, volatile bool *flag, uint32_t timeout_ms)
{
    uint64_t end = hal_ticks_ms() + timeout_ms;
    while (!*flag) {
        poll(x);
        if (*flag)
            break;
        if (hal_ticks_ms() > end)
            return false;
    }
    return true;
}

static int command(struct xhci *x, uint32_t p0, uint32_t control, int *slot)
{
    x->command_done = false;
    x->command_wait = ring_push(&x->command, p0, 0, 0, control);
    x->doorbell[0] = 0;
    if (!wait_flag(x, &x->command_done, COMMAND_TIMEOUT)) {
        klog("%s: command %u timed out", x->name, (control >> 10) & 0x3f);
        return E_TIMEDOUT;
    }
    if (slot)
        *slot = x->command_slot;
    return x->command_code == CC_SUCCESS ? 0 : E_IO;
}

static volatile uint32_t *context(struct xhci *x, uint8_t *base, int index)
{
    return (volatile uint32_t *)(base + (uint32_t)index * x->csz);
}

static struct endpoint *endpoint_new(void)
{
    struct endpoint *ep = kzalloc(sizeof(*ep));
    if (ep && !ring_init(&ep->ring)) {
        kfree(ep);
        return NULL;
    }
    return ep;
}

static void endpoint_free(struct endpoint *ep)
{
    if (!ep)
        return;
    ring_free(&ep->ring);
    if (ep->buffer)
        kfree_pages(ep->buffer);
    kfree(ep);
}

static void recover(struct xhci *x, struct xdev *d, int dci)
{
    struct endpoint *ep = d->ep[dci];
    uint32_t state = context(x, d->out, dci)[0] & 7;
    if (state == 2)
        command(x, 0, TRB_RESET_EP << 10 | (uint32_t)dci << 16 | (uint32_t)d->slot << 24, NULL);
    else if (state == 1)
        command(x, 0, TRB_STOP_EP << 10 | (uint32_t)dci << 16 | (uint32_t)d->slot << 24, NULL);
    uint32_t deq = (ep->ring.phys + (uint32_t)ep->ring.enqueue * 16) | ep->ring.cycle;
    command(x, deq, TRB_SET_DEQUEUE << 10 | (uint32_t)dci << 16 | (uint32_t)d->slot << 24, NULL);
}

static int wait_endpoint(struct xhci *x, struct xdev *d, int dci, uint32_t timeout_ms)
{
    struct endpoint *ep = d->ep[dci];
    if (!wait_flag(x, &ep->done, timeout_ms)) {
        recover(x, d, dci);
        return E_TIMEDOUT;
    }
    if (ep->code == CC_SUCCESS || ep->code == CC_SHORT)
        return 0;
    recover(x, d, dci);
    return ep->code == CC_STALL ? E_PIPE : E_IO;
}

static int control_raw(struct xhci *x, struct xdev *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                       uint16_t length)
{
    struct endpoint *ep = d->ep[1];
    bool in = type & USB_DIR_IN;
    uint32_t transfer = length ? (in ? 3u : 2u) : 0u;
    ep->done = false;
    ring_push(&ep->ring, type | (uint32_t)request << 8 | (uint32_t)value << 16, index | (uint32_t)length << 16, 8,
              TRB_SETUP << 10 | TRB_IDT | transfer << 16);
    if (length)
        ring_push(&ep->ring, vmm_phys(d->dma), 0, length, TRB_DATA << 10 | (in ? 1u << 16 : 0));
    ep->wait = ring_push(&ep->ring, 0, 0, 0, TRB_STATUS << 10 | TRB_IOC | (length && in ? 0 : 1u << 16));
    x->doorbell[d->slot] = 1;
    return wait_endpoint(x, d, 1, 2000);
}

static int xhci_control(struct usb_device *dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                        void *data, uint16_t length)
{
    struct xhci *x = dev->host->priv;
    struct xdev *d = dev->hc;
    if (!d || dev->gone)
        return E_NODEV;
    if (length > PAGE)
        return E_INVAL;
    if (!(type & USB_DIR_IN) && length)
        memcpy(d->dma, data, length);
    int err = control_raw(x, d, type, request, value, index, length);
    if (!err && (type & USB_DIR_IN) && length)
        memcpy(data, d->dma, length);
    return err;
}

static uint16_t default_packet(int speed)
{
    return speed == USB_LOW ? 8 : speed == USB_SUPER ? 512 : 64;
}

static void free_xdev(struct xhci *x, struct xdev *d)
{
    uint32_t flags = cpu_irq_save();
    if (d->slot)
        x->devs[d->slot] = NULL;
    cpu_irq_restore(flags);
    for (int i = 0; i < 32; i++)
        endpoint_free(d->ep[i]);
    if (d->out)
        kfree_pages(d->out);
    if (d->in)
        kfree_pages(d->in);
    if (d->dma)
        kfree_pages(d->dma);
    kfree(d);
}

static int xhci_address(struct usb_device *dev)
{
    struct xhci *x = dev->host->priv;
    int slot = 0;
    if (command(x, 0, TRB_ENABLE_SLOT << 10, &slot) || slot <= 0 || slot > x->slots)
        return E_IO;
    struct xdev *d = kzalloc(sizeof(*d));
    if (!d)
        return E_NOMEM;
    d->out = dma_page();
    d->in = dma_page();
    d->dma = dma_page();
    d->ep[1] = endpoint_new();
    if (!d->out || !d->in || !d->dma || !d->ep[1]) {
        free_xdev(x, d);
        command(x, 0, TRB_DISABLE_SLOT << 10 | (uint32_t)slot << 24, NULL);
        return E_NOMEM;
    }
    d->dev = dev;
    x->dcbaa[slot] = vmm_phys(d->out);

    int tt_slot = 0, tt_port = 0;
    if (dev->speed == USB_LOW || dev->speed == USB_FULL)
        for (struct usb_device *child = dev, *p = dev->parent; p; child = p, p = p->parent)
            if (p->speed == USB_HIGH) {
                tt_slot = p->slot;
                tt_port = child->port;
                break;
            }
    uint16_t packet = default_packet(dev->speed);
    context(x, d->in, 0)[1] = 3;
    volatile uint32_t *sc = context(x, d->in, 1);
    sc[0] = dev->route | (uint32_t)dev->speed << 20 | 1u << 27;
    sc[1] = (uint32_t)dev->root_port << 16;
    sc[2] = (uint32_t)tt_slot | (uint32_t)tt_port << 8;
    volatile uint32_t *ep0 = context(x, d->in, 2);
    ep0[1] = 3u << 1 | 4u << 3 | (uint32_t)packet << 16;
    ep0[2] = d->ep[1]->ring.phys | 1;
    ep0[4] = 8;
    int err = command(x, vmm_phys(d->in), TRB_ADDRESS << 10 | (uint32_t)slot << 24, NULL);
    d->slot = slot;
    uint32_t flags = cpu_irq_save();
    x->devs[slot] = d;
    cpu_irq_restore(flags);
    if (err) {
        free_xdev(x, d);
        command(x, 0, TRB_DISABLE_SLOT << 10 | (uint32_t)slot << 24, NULL);
        x->dcbaa[slot] = 0;
        return err;
    }
    dev->slot = slot;
    dev->hc = d;
    hal_sleep_ms(2);

    uint8_t head[8];
    err = xhci_control(dev, USB_DIR_IN, USB_GET_DESCRIPTOR, 0x0100, 0, head, 8);
    if (err)
        return err;
    uint16_t actual = dev->speed == USB_SUPER ? (uint16_t)(1u << (head[7] & 15)) : head[7];
    dev->max_packet0 = (uint8_t)MIN(actual, 255);
    if (actual && actual != packet) {
        memset(d->in, 0, PAGE);
        context(x, d->in, 0)[1] = 2;
        volatile uint32_t *e = context(x, d->in, 2);
        e[1] = 3u << 1 | 4u << 3 | (uint32_t)actual << 16;
        e[2] = (d->ep[1]->ring.phys + (uint32_t)d->ep[1]->ring.enqueue * 16) | d->ep[1]->ring.cycle;
        e[4] = 8;
        command(x, vmm_phys(d->in), TRB_EVALUATE << 10 | (uint32_t)slot << 24, NULL);
    }
    return 0;
}

static int log2_floor(unsigned v)
{
    int n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

static int xhci_configure(struct usb_device *dev)
{
    struct xhci *x = dev->host->priv;
    struct xdev *d = dev->hc;
    memset(d->in, 0, PAGE);
    uint32_t add = 1;
    int last = 1;
    for (int i = 0; i < dev->interface_count; i++) {
        const struct usb_interface *intf = &dev->interfaces[i];
        for (int k = 0; k < intf->endpoint_count; k++) {
            const struct usb_endpoint *e = &intf->endpoints[k];
            uint32_t kind = e->attributes & 3;
            if (kind != 2 && kind != 3)
                continue;
            bool in = e->address & USB_DIR_IN;
            int dci = (e->address & 15) * 2 + (in ? 1 : 0);
            if (dci < 2 || dci > 31)
                continue;
            if (!d->ep[dci] && !(d->ep[dci] = endpoint_new()))
                return E_NOMEM;
            uint32_t interval = 0;
            if (kind == 3) {
                if (dev->speed == USB_HIGH || dev->speed == USB_SUPER)
                    interval = (uint32_t)MAX(e->interval, 1) - 1;
                else
                    interval = (uint32_t)MIN(MAX(log2_floor((unsigned)MAX(e->interval, 1) * 8), 3), 10);
            }
            uint32_t type = kind + (in ? 4u : 0u);
            uint32_t packet = e->max_packet & 0x7ff;
            volatile uint32_t *c = context(x, d->in, dci + 1);
            c[0] = interval << 16;
            c[1] = 3u << 1 | type << 3 | (uint32_t)e->burst << 8 | packet << 16;
            c[2] = d->ep[dci]->ring.phys | 1;
            c[4] = (kind == 3 ? packet : 3072u) | (kind == 3 ? packet * (e->burst + 1u) : 0u) << 16;
            add |= 1u << dci;
            last = MAX(last, dci);
        }
    }
    context(x, d->in, 0)[1] = add;
    volatile uint32_t *sc = context(x, d->in, 1), *out = context(x, d->out, 0);
    for (int i = 0; i < 4; i++)
        sc[i] = out[i];
    sc[3] = 0;
    sc[0] = (sc[0] & ~(0x1fu << 27)) | (uint32_t)last << 27;
    if (dev->is_hub) {
        sc[0] |= 1u << 26;
        sc[1] = (sc[1] & 0x00ffffffu) | (uint32_t)dev->hub_ports << 24;
        if (dev->speed == USB_HIGH)
            sc[2] = (sc[2] & ~(3u << 16)) | (uint32_t)dev->tt_think << 16;
    }
    return command(x, vmm_phys(d->in), TRB_CONFIGURE << 10 | (uint32_t)d->slot << 24, NULL);
}

static int endpoint_index(uint8_t endpoint)
{
    return (endpoint & 15) * 2 + ((endpoint & USB_DIR_IN) ? 1 : 0);
}

static int xhci_transfer(struct usb_device *dev, uint8_t endpoint, void *dma, size_t length, uint32_t timeout_ms)
{
    struct xhci *x = dev->host->priv;
    struct xdev *d = dev->hc;
    int dci = endpoint_index(endpoint);
    if (!d || dev->gone)
        return E_NODEV;
    struct endpoint *ep = d->ep[dci];
    if (!ep)
        return E_INVAL;
    uint32_t phys = length ? vmm_phys(dma) : 0, left = (uint32_t)length;
    uint32_t packet = 512;
    for (int i = 0; i < dev->interface_count; i++)
        for (int k = 0; k < dev->interfaces[i].endpoint_count; k++)
            if (dev->interfaces[i].endpoints[k].address == endpoint)
                packet = MAX(dev->interfaces[i].endpoints[k].max_packet & 0x7ffu, 8u);
    ep->done = false;
    do {
        uint32_t chunk = MIN(left, 0x10000u - (phys & 0xffffu));
        left -= chunk;
        uint32_t packets = MIN((left + packet - 1) / packet, 31u);
        uint32_t control = TRB_NORMAL << 10 | TRB_ISP | (left ? TRB_CHAIN : TRB_IOC);
        uint32_t at = ring_push(&ep->ring, phys, 0, chunk | packets << 17, control);
        if (!left)
            ep->wait = at;
        phys += chunk;
    } while (left);
    x->doorbell[d->slot] = (uint32_t)dci;
    int err = wait_endpoint(x, d, dci, timeout_ms);
    if (err)
        return err;
    return (int)(length - MIN(ep->residue, (uint32_t)length));
}

static int xhci_interrupt(struct usb_device *dev, uint8_t endpoint, uint16_t length, usb_complete done, void *ctx)
{
    struct xhci *x = dev->host->priv;
    struct xdev *d = dev->hc;
    int dci = endpoint_index(endpoint);
    struct endpoint *ep = d ? d->ep[dci] : NULL;
    if (!ep || length > PAGE)
        return E_INVAL;
    if (!ep->buffer && !(ep->buffer = dma_page()))
        return E_NOMEM;
    uint32_t flags = cpu_irq_save();
    ep->length = length;
    ep->complete = done;
    ep->ctx = ctx;
    ep->periodic = true;
    ep->wait = ring_push(&ep->ring, vmm_phys(ep->buffer), 0, length, TRB_NORMAL << 10 | TRB_IOC | TRB_ISP);
    x->doorbell[d->slot] = (uint32_t)dci;
    cpu_irq_restore(flags);
    return 0;
}

static int xhci_clear_halt(struct usb_device *dev, uint8_t endpoint)
{
    return xhci_control(dev, USB_REQ_ENDPOINT, USB_CLEAR_FEATURE, 0, endpoint, NULL, 0);
}

static void xhci_release(struct usb_device *dev)
{
    struct xhci *x = dev->host->priv;
    struct xdev *d = dev->hc;
    if (!d)
        return;
    int slot = d->slot;
    free_xdev(x, d);
    command(x, 0, TRB_DISABLE_SLOT << 10 | (uint32_t)slot << 24, NULL);
    x->dcbaa[slot] = 0;
    dev->hc = NULL;
}

static volatile uint32_t *portsc(struct xhci *x, int port)
{
    return (volatile uint32_t *)(x->op + 0x400 + 0x10 * (uint32_t)(port - 1));
}

static int port_reset(struct xhci *x, int port)
{
    volatile uint32_t *sc = portsc(x, port);
    if (!(*sc & PORT_CCS))
        return E_NODEV;
    if (x->protocol[port] == 3) {
        uint64_t end = hal_ticks_ms() + 300;
        while (!(*sc & PORT_PED) && hal_ticks_ms() < end)
            hal_sleep_ms(5);
        if (!(*sc & PORT_PED))
            *sc = (*sc & PORT_KEEP) | PORT_WPR;
    } else {
        *sc = (*sc & PORT_KEEP) | PORT_PR;
    }
    uint64_t end = hal_ticks_ms() + 500;
    while (hal_ticks_ms() < end && ((*sc & PORT_PR) || !(*sc & PORT_PED)))
        hal_sleep_ms(2);
    *sc = (*sc & PORT_KEEP) | PORT_CHANGES;
    if (!(*sc & PORT_PED))
        return E_IO;
    hal_sleep_ms(20);
    int speed = (int)(*sc >> 10) & 0xf;
    return speed ? speed : USB_FULL;
}

static void port_check(struct xhci *x, int port)
{
    volatile uint32_t *sc = portsc(x, port);
    uint32_t v = *sc;
    *sc = (v & PORT_KEEP) | (v & PORT_CHANGES);
    bool connected = v & PORT_CCS;
    if (x->root[port] && (!connected || (v & (1u << 17)))) {
        usb_detach(x->root[port]);
        x->root[port] = NULL;
    }
    if (!connected || x->root[port])
        return;
    hal_sleep_ms(50);
    int speed = port_reset(x, port);
    if (speed < 0)
        return;
    x->root[port] = usb_attach(&x->host, NULL, port, port, speed);
}

static void xhci_service(struct usb_host *host)
{
    struct xhci *x = host->priv;
    for (int port = 1; port <= x->ports; port++) {
        uint32_t bit = 1u << (port % 32);
        if (!(x->changed[port / 32] & bit))
            continue;
        uint32_t flags = cpu_irq_save();
        x->changed[port / 32] &= ~bit;
        cpu_irq_restore(flags);
        port_check(x, port);
    }
}

static const struct usb_host_ops xhci_ops = {
    .address = xhci_address,
    .control = xhci_control,
    .configure = xhci_configure,
    .transfer = xhci_transfer,
    .interrupt = xhci_interrupt,
    .clear_halt = xhci_clear_halt,
    .release = xhci_release,
    .service = xhci_service,
};

static void take_ownership(struct xhci *x)
{
    uint32_t offset = (rd(x->cap, 0x10) >> 16) << 2;
    for (int guard = 0; offset && guard < 64; guard++) {
        uint32_t v = rd(x->cap, offset);
        uint8_t id = (uint8_t)v;
        if (id == 1) {
            wr(x->cap, offset, v | 1u << 24);
            uint64_t end = hal_ticks_ms() + 1000;
            while ((rd(x->cap, offset) & (1u << 16)) && hal_ticks_ms() < end)
                hal_sleep_ms(1);
            wr(x->cap, offset, (rd(x->cap, offset) & ~(1u << 16)) | 1u << 24);
            uint32_t ctl = rd(x->cap, offset + 4);
            wr(x->cap, offset + 4, (ctl & ~0x0000e011u) | 0xe0000000u);
        } else if (id == 2) {
            uint8_t major = (uint8_t)(v >> 24);
            uint32_t ports = rd(x->cap, offset + 8);
            int first = (int)(ports & 0xff), count = (int)(ports >> 8) & 0xff;
            for (int p = first; p < first + count && p <= MAX_PORTS; p++)
                if (p > 0)
                    x->protocol[p] = major;
        }
        uint32_t next = (v >> 8) & 0xff;
        offset = next ? offset + next * 4 : 0;
    }
}

static bool wait_status(struct xhci *x, uint32_t mask, uint32_t want, uint32_t timeout_ms)
{
    uint64_t end = hal_ticks_ms() + timeout_ms;
    while ((rd(x->op, OP_USBSTS) & mask) != want)
        if (hal_ticks_ms() > end)
            return false;
    return true;
}

static bool start(struct xhci *x)
{
    take_ownership(x);
    wr(x->op, OP_USBCMD, rd(x->op, OP_USBCMD) & ~1u);
    if (!wait_status(x, 1, 1, 100))
        return false;
    wr(x->op, OP_USBCMD, 2);
    uint64_t end = hal_ticks_ms() + 1000;
    while ((rd(x->op, OP_USBCMD) & 2) && hal_ticks_ms() < end)
        ;
    hal_sleep_ms(1);
    if ((rd(x->op, OP_USBCMD) & 2) || !wait_status(x, 1u << 11, 0, 1000))
        return false;

    uint32_t hcs1 = rd(x->cap, 0x04), hcs2 = rd(x->cap, 0x08), hcc1 = rd(x->cap, 0x10);
    x->slots = (int)MIN(hcs1 & 0xff, 255u);
    x->ports = (int)MIN((hcs1 >> 24) & 0xff, (uint32_t)MAX_PORTS);
    x->csz = hcc1 & 4 ? 64 : 32;
    wr(x->op, OP_CONFIG, (uint32_t)x->slots);

    x->dcbaa = dma_page();
    if (!x->dcbaa || !ring_init(&x->command))
        return false;
    uint32_t scratch = ((hcs2 >> 27) & 0x1f) | ((hcs2 >> 21) & 0x1f) << 5;
    if (scratch) {
        uint64_t *array = dma_page();
        if (!array || scratch > 512)
            return false;
        for (uint32_t i = 0; i < scratch; i++) {
            void *page = dma_page();
            if (!page)
                return false;
            array[i] = vmm_phys(page);
        }
        x->dcbaa[0] = vmm_phys(array);
    }
    wr64(x->op, OP_DCBAAP, vmm_phys(x->dcbaa));
    wr64(x->op, OP_CRCR, x->command.phys | 1);

    x->events = dma_page();
    uint32_t *erst = dma_page();
    if (!x->events || !erst)
        return false;
    x->events_phys = vmm_phys((const void *)x->events);
    x->event_cycle = 1;
    erst[0] = x->events_phys;
    erst[2] = RING_TRBS;
    wr(x->rt, 0x28, 1);
    wr64(x->rt, 0x38, x->events_phys);
    wr64(x->rt, 0x30, vmm_phys(erst));
    wr(x->rt, 0x20, 1);
    wr(x->op, OP_USBCMD, 1);
    if (!wait_status(x, 1, 0, 1000))
        return false;
    if (hcc1 & 8)
        for (int port = 1; port <= x->ports; port++)
            *portsc(x, port) = (*portsc(x, port) & PORT_KEEP) | PORT_PP;
    return true;
}

static void found_pci(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (d->class_code != 0x0c || d->subclass != 0x03 || d->prog_if != 0x30 || controller_count >= MAX_CONTROLLERS)
        return;
    uint64_t base;
    if (!pci_bar_base(d, 0x10, &base)) {
        klog("xhci: registers above 4 GB are out of reach");
        return;
    }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);
    if (d->vendor == 0x8086) {
        static const uint16_t switchable[] = {0x1e31, 0x8c31, 0x9c31, 0x9cb1, 0x8cb1, 0x8d31, 0xa12f, 0xa2af};
        for (size_t i = 0; i < ARRAY_SIZE(switchable); i++)
            if (d->device == switchable[i]) {
                pci_write(d, 0xd8, pci_read(d, 0xdc));
                pci_write(d, 0xd0, pci_read(d, 0xd4));
            }
    }
    struct xhci *x = kzalloc(sizeof(*x));
    if (!x)
        return;
    x->cap = vmm_map_phys64(base, 0x1000);
    if (!x->cap) {
        kfree(x);
        return;
    }
    uint32_t dboff = rd(x->cap, 0x14) & ~3u, rtsoff = rd(x->cap, 0x18) & ~0x1fu;
    uint32_t size = MAX(MAX(dboff + 256 * 4, rtsoff + 0x40), 0x480u + 0x10u * MAX_PORTS);
    x->cap = vmm_map_phys64(base, ALIGN_UP(size, PAGE));
    if (!x->cap) {
        kfree(x);
        return;
    }
    x->op = x->cap + (rd(x->cap, 0) & 0xff);
    x->rt = x->cap + rtsoff;
    x->doorbell = (volatile uint32_t *)(x->cap + dboff);
    ksnprintf(x->name, sizeof(x->name), "xhci%d", controller_count);
    x->host = (struct usb_host){.ops = &xhci_ops, .name = x->name, .priv = x};
    for (int p = 0; p <= MAX_PORTS; p++)
        x->protocol[p] = 2;
    if (!start(x)) {
        klog("%s: controller did not start", x->name);
        return;
    }
    klog("%s: %d ports, %d slots", x->name, x->ports, x->slots);
    controllers[controller_count++] = x;
    usb_host_register(&x->host);
    for (int port = 1; port <= x->ports; port++)
        x->changed[port / 32] |= 1u << (port % 32);
}

void xhci_init(void)
{
    pci_foreach(found_pci, NULL);
    if (!controller_count)
        return;
    usb_poll_register(poll_all);
    hal_sleep_ms(100);
    usb_port_changed();
}
