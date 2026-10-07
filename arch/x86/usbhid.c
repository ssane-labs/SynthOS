#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

#include "io.h"
#include "usb.h"

#define REPEAT_DELAY 500
#define REPEAT_RATE 33
#define EXT 0x80

enum {
    HID_SET_IDLE = 0x0a,
    HID_SET_PROTOCOL = 0x0b,
};

struct field {
    uint16_t offset;
    uint8_t size;
    bool present, is_signed;
    int32_t max;
};

struct hid {
    struct usb_device *dev;
    bool keyboard;
    uint8_t last[8];
    uint8_t report_id;
    bool absolute;
    struct field x, y, wheel, buttons;
    uint8_t button_count;
};

static const uint8_t scancodes[0x66] = {
    [0x04] = 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    [0x1e] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    [0x28] = 0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c, 0x0d, 0x1a, 0x1b, 0x2b, 0x2b, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,
    0x3a,
    [0x3a] = 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    [0x46] = EXT | 0x37, 0x46, 0,
    [0x49] = EXT | 0x52, EXT | 0x47, EXT | 0x49, EXT | 0x53, EXT | 0x4f, EXT | 0x51, EXT | 0x4d, EXT | 0x4b,
    EXT | 0x50, EXT | 0x48,
    [0x53] = 0x45, EXT | 0x35, 0x37, 0x4a, 0x4e, EXT | 0x1c, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49,
    0x52, 0x53, 0x56, EXT | 0x5d,
};

static const uint8_t modifier_codes[8] = {0x1d, 0x2a, 0x38, EXT | 0x5b, EXT | 0x1d, 0x36, EXT | 0x38, EXT | 0x5c};

static struct hid *repeat_owner;
static uint8_t repeat_code;
static uint32_t repeat_at;

static void send(uint8_t code, bool release)
{
    if (!code)
        return;
    if (code & EXT)
        keyboard_scancode(0xe0);
    keyboard_scancode((uint8_t)((code & 0x7f) | (release ? 0x80 : 0)));
}

static bool contains(const uint8_t *keys, uint8_t usage)
{
    for (int i = 2; i < 8; i++)
        if (keys[i] == usage)
            return true;
    return false;
}

static void keyboard_report(struct hid *h, const uint8_t *r)
{
    if (r[2] == 1)
        return;
    uint8_t changed = (uint8_t)(r[0] ^ h->last[0]);
    for (int bit = 0; bit < 8; bit++)
        if (changed & (1u << bit))
            send(modifier_codes[bit], !(r[0] & (1u << bit)));
    for (int i = 2; i < 8; i++) {
        uint8_t u = h->last[i];
        if (u && u < sizeof(scancodes) && !contains(r, u)) {
            send(scancodes[u], true);
            if (repeat_owner == h && repeat_code == scancodes[u])
                repeat_owner = NULL;
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u && u < sizeof(scancodes) && !contains(h->last, u) && scancodes[u]) {
            send(scancodes[u], false);
            repeat_owner = h;
            repeat_code = scancodes[u];
            repeat_at = usb_now() + REPEAT_DELAY;
        }
    }
    memcpy(h->last, r, 8);
}

static void repeat_tick(void)
{
    if (!repeat_owner || (int32_t)(usb_now() - repeat_at) < 0)
        return;
    send(repeat_code, false);
    repeat_at = usb_now() + REPEAT_RATE;
}

static int32_t extract(const uint8_t *data, int length, const struct field *f)
{
    uint32_t v = 0;
    for (int i = 0; i < f->size; i++) {
        int bit = f->offset + i;
        if (bit / 8 < length && (data[bit / 8] >> (bit % 8) & 1))
            v |= 1u << i;
    }
    if (f->is_signed && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;
    return (int32_t)v;
}

static void pointer_report(struct hid *h, const uint8_t *data, int length)
{
    if (h->report_id) {
        if (length < 1 || data[0] != h->report_id)
            return;
        data++;
        length--;
    }
    unsigned buttons = h->buttons.present ? (unsigned)extract(data, length, &h->buttons) & 7u : 0;
    int x = h->x.present ? extract(data, length, &h->x) : 0;
    int y = h->y.present ? extract(data, length, &h->y) : 0;
    int wheel = h->wheel.present ? -extract(data, length, &h->wheel) : 0;
    if (h->absolute) {
        int sx = h->x.max > 0 ? (int)((int64_t)x * 65535 / h->x.max) : 0;
        int sy = h->y.max > 0 ? (int)((int64_t)y * 65535 / h->y.max) : 0;
        mouse_absolute(sx, sy, wheel, buttons);
    } else {
        mouse_report(x, y, wheel, buttons);
    }
}

static void hid_complete(struct usb_device *dev, void *ctx, const uint8_t *data, int length)
{
    UNUSED(dev);
    struct hid *h = ctx;
    if (h->keyboard) {
        if (length >= 8)
            keyboard_report(h, data);
    } else {
        pointer_report(h, data, length);
    }
}

struct parser {
    uint16_t page;
    int32_t min, max;
    uint32_t size, count;
    uint8_t id;
    uint32_t usages[16];
    int usage_count;
    uint32_t usage_min, usage_max;
    uint32_t bits;
};

static uint32_t item_value(const uint8_t *p, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_signed(const uint8_t *p, int size)
{
    uint32_t v = item_value(p, size);
    if (size == 1)
        return (int8_t)v;
    if (size == 2)
        return (int16_t)v;
    return (int32_t)v;
}

static void set_field(struct hid *h, struct field *f, const struct parser *ps, uint32_t offset)
{
    if (f->present)
        return;
    f->present = true;
    f->offset = (uint16_t)offset;
    f->size = (uint8_t)MIN(ps->size, 32u);
    f->is_signed = ps->min < 0;
    f->max = ps->max;
    h->report_id = ps->id;
}

static void input_item(struct hid *h, struct parser *ps, uint32_t flags)
{
    if (h->x.present && ps->id != h->report_id) {
        ps->bits += ps->size * ps->count;
        return;
    }
    for (uint32_t i = 0; i < ps->count; i++) {
        uint32_t usage = (int)i < ps->usage_count       ? ps->usages[i]
                         : ps->usage_count                ? ps->usages[ps->usage_count - 1]
                         : ps->usage_min + i <= ps->usage_max ? ps->usage_min + i
                                                            : 0;
        uint32_t offset = ps->bits + i * ps->size;
        if (flags & 1)
            continue;
        if (ps->page == 9 && !h->buttons.present) {
            struct parser one = *ps;
            one.size = MIN(ps->count, 8u);
            one.min = 0;
            set_field(h, &h->buttons, &one, offset);
            h->button_count = (uint8_t)one.size;
        } else if (ps->page == 1 && usage == 0x30) {
            set_field(h, &h->x, ps, offset);
            h->absolute = !(flags & 4);
        } else if (ps->page == 1 && usage == 0x31) {
            set_field(h, &h->y, ps, offset);
        } else if (ps->page == 1 && usage == 0x38) {
            set_field(h, &h->wheel, ps, offset);
        }
    }
    ps->bits += ps->size * ps->count;
}

static bool parse_report(struct hid *h, const uint8_t *p, int length)
{
    struct parser ps = {0};
    for (int at = 0; at < length;) {
        uint8_t b = p[at];
        if (b == 0xfe) {
            at += 3 + (at + 1 < length ? p[at + 1] : 0);
            continue;
        }
        int size = (b & 3) == 3 ? 4 : (b & 3);
        if (at + 1 + size > length)
            break;
        const uint8_t *v = p + at + 1;
        int type = (b >> 2) & 3, tag = b >> 4;
        if (type == 0) {
            if (tag == 8)
                input_item(h, &ps, item_value(v, size));
            ps.usage_count = 0;
            ps.usage_min = ps.usage_max = 0;
        } else if (type == 1) {
            switch (tag) {
            case 0: ps.page = (uint16_t)item_value(v, size); break;
            case 1: ps.min = item_signed(v, size); break;
            case 2: ps.max = ps.min < 0 ? item_signed(v, size) : (int32_t)item_value(v, size); break;
            case 7: ps.size = item_value(v, size); break;
            case 8:
                if (ps.id != (uint8_t)item_value(v, size))
                    ps.bits = 0;
                ps.id = (uint8_t)item_value(v, size);
                break;
            case 9: ps.count = item_value(v, size); break;
            }
        } else if (type == 2) {
            uint32_t u = item_value(v, size);
            if (size == 4)
                u &= 0xffff;
            if (tag == 0 && ps.usage_count < 16)
                ps.usages[ps.usage_count++] = u;
            else if (tag == 1)
                ps.usage_min = u;
            else if (tag == 2)
                ps.usage_max = u;
        }
        at += 1 + size;
    }
    return h->x.present && h->y.present;
}

static void boot_mouse(struct hid *h)
{
    h->report_id = 0;
    h->absolute = false;
    h->buttons = (struct field){.offset = 0, .size = 3, .present = true};
    h->x = (struct field){.offset = 8, .size = 8, .present = true, .is_signed = true, .max = 127};
    h->y = (struct field){.offset = 16, .size = 8, .present = true, .is_signed = true, .max = 127};
    h->wheel = (struct field){0};
}

static bool hid_match(const struct usb_device *dev, const struct usb_interface *intf)
{
    UNUSED(dev);
    return intf->cls == 3 && usb_find_endpoint(intf, 3, true);
}

static int hid_attach(struct usb_device *dev, int index)
{
    static bool ticking;
    const struct usb_interface *intf = &dev->interfaces[index];
    const struct usb_endpoint *e = usb_find_endpoint(intf, 3, true);
    struct hid *h = kzalloc(sizeof(*h));
    if (!h)
        return E_NOMEM;
    h->dev = dev;
    h->keyboard = intf->protocol == 1;
    if (h->keyboard) {
        if (intf->subclass == 1)
            usb_control(dev, USB_REQ_CLASS | USB_REQ_INTERFACE, HID_SET_PROTOCOL, 0, intf->number, NULL, 0);
        usb_control(dev, USB_REQ_CLASS | USB_REQ_INTERFACE, HID_SET_IDLE, 0, intf->number, NULL, 0);
    } else {
        bool parsed = false;
        uint16_t length = intf->report_length;
        uint8_t *desc = length && length <= 1024 ? kmalloc(length) : NULL;
        if (desc && usb_control(dev, USB_DIR_IN | USB_REQ_INTERFACE, USB_GET_DESCRIPTOR, 0x2200, intf->number, desc,
                                length) == 0)
            parsed = parse_report(h, desc, length);
        kfree(desc);
        if (!parsed) {
            if (intf->subclass != 1 || intf->protocol != 2) {
                kfree(h);
                return E_NOSYS;
            }
            usb_control(dev, USB_REQ_CLASS | USB_REQ_INTERFACE, HID_SET_PROTOCOL, 0, intf->number, NULL, 0);
            boot_mouse(h);
        }
        usb_control(dev, USB_REQ_CLASS | USB_REQ_INTERFACE, HID_SET_IDLE, 0, intf->number, NULL, 0);
    }
    dev->priv[index] = h;
    if (!ticking) {
        ticking = true;
        usb_poll_register(repeat_tick);
    }
    uint16_t length = (uint16_t)MIN(MAX(e->max_packet & 0x7ff, 8), 64);
    int err = dev->host->ops->interrupt(dev, e->address, length, hid_complete, h);
    if (err) {
        dev->priv[index] = NULL;
        kfree(h);
    }
    return err;
}

static void hid_detach(struct usb_device *dev, int index)
{
    struct hid *h = dev->priv[index];
    if (!h)
        return;
    uint32_t flags = cpu_irq_save();
    if (repeat_owner == h)
        repeat_owner = NULL;
    cpu_irq_restore(flags);
    if (h->keyboard)
        for (int bit = 0; bit < 8; bit++)
            if (h->last[0] & (1u << bit))
                send(modifier_codes[bit], true);
    dev->priv[index] = NULL;
    kfree(h);
}

const struct usb_driver usb_hid_driver = {
    .name = "hid",
    .match = hid_match,
    .attach = hid_attach,
    .detach = hid_detach,
};
