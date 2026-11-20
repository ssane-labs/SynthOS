/* virtio-input (virtio 1.x over PCI): the paravirtual mouse, tablet and multi-touch screen of QEMU and crosvm.
   Events come as Linux evdev triples on one queue, which the timer polls; a multi-touch device's contacts (slots)
   go through the gesture engine (kernel/core/touch.c) as a touch screen. 64-bit kernel only. */

#ifdef __x86_64__

#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/touch.h>
#include <synth/vmm.h>

#include "io.h"
#include "usb.h"

#define QSIZE 64
#define MAX_DEVICES 4

enum {
    EV_SYN = 0,
    EV_KEY = 1,
    EV_REL = 2,
    EV_ABS = 3,
    REL_X = 0,
    REL_Y = 1,
    REL_WHEEL = 8,
    ABS_X = 0,
    ABS_Y = 1,
    ABS_MT_SLOT = 0x2f,
    ABS_MT_POSITION_X = 0x35,
    ABS_MT_POSITION_Y = 0x36,
    ABS_MT_TRACKING_ID = 0x39,
    BTN_LEFT = 0x110,
    BTN_RIGHT = 0x111,
    BTN_MIDDLE = 0x112,
    BTN_TOUCH = 0x14a,
    CFG_ID_NAME = 0x01,
    CFG_ABS_INFO = 0x12,
};

struct desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct event {
    uint16_t type, code;
    int32_t value;
};

struct slot {
    int id; /* -1: no finger */
    int x, y;
};

struct vinput {
    volatile uint8_t *common, *device, *notify_base;
    uint32_t notify_mul;
    struct desc *desc;
    volatile uint16_t *avail, *used, *notify;
    uint16_t seen, next_avail;
    struct event *events;
    char name[40];
    bool multitouch, absolute;
    int max_x, max_y;
    /* what has come since the last SYN_REPORT */
    int dx, dy, wheel, x, y;
    unsigned buttons;
    int slot;
    struct slot slots[TOUCH_MAX];
    struct touch_pad pad;
};

static struct vinput *inputs[MAX_DEVICES];
static int input_count;

#define C8(o) (*(volatile uint8_t *)(v->common + (o)))
#define C16(o) (*(volatile uint16_t *)(v->common + (o)))
#define C32(o) (*(volatile uint32_t *)(v->common + (o)))
#define C64(o) (*(volatile uint64_t *)(v->common + (o)))

static volatile uint8_t *map_cap(const struct pci_device *d, uint8_t bar, uint32_t offset, uint32_t length)
{
    uint64_t base;
    if (bar > 5 || !pci_bar_base(d, (uint8_t)(0x10 + bar * 4), &base))
        return NULL;
    return vmm_map_phys64(base + offset, MAX(length, 4096u));
}

/* the size of a configuration answer (select, subsel); the answer itself at device + 8 */
static uint8_t config(struct vinput *v, uint8_t select, uint8_t subsel)
{
    v->device[0] = select;
    v->device[1] = subsel;
    return v->device[2];
}

static int abs_max(struct vinput *v, uint8_t axis)
{
    if (config(v, CFG_ABS_INFO, axis) < 8)
        return -1;
    return (int)*(volatile uint32_t *)(v->device + 12);
}

static void pointer(int dx, int dy, int wheel, unsigned buttons)
{
    mouse_report(dx, dy, wheel, buttons);
}

static void absolute(int x, int y, unsigned buttons)
{
    mouse_absolute(x, y, 0, buttons);
}

static void report(struct vinput *v)
{
    if (v->multitouch) {
        struct touch_frame f = {.buttons = v->buttons & 1};
        for (int i = 0; i < TOUCH_MAX; i++)
            if (v->slots[i].id >= 0)
                f.c[f.count++] = (struct touch_contact){v->slots[i].id, v->slots[i].x, v->slots[i].y};
        touch_frame(&v->pad, &f, hal_ticks_ms());
    } else if (v->absolute) {
        if (v->max_x > 0 && v->max_y > 0)
            mouse_absolute((int)((int64_t)v->x * 65535 / v->max_x), (int)((int64_t)v->y * 65535 / v->max_y),
                           v->wheel, v->buttons);
    } else {
        mouse_report(v->dx, v->dy, v->wheel, v->buttons);
    }
    v->dx = v->dy = v->wheel = 0;
}

static void event(struct vinput *v, const struct event *e)
{
    switch (e->type) {
    case EV_SYN:
        if (e->code == 0)
            report(v);
        break;
    case EV_KEY: {
        unsigned bit = e->code == BTN_LEFT || (e->code == BTN_TOUCH && !v->multitouch) ? 1u
                       : e->code == BTN_RIGHT                                         ? 2u
                       : e->code == BTN_MIDDLE                                        ? 4u
                                                                                      : 0;
        v->buttons = e->value ? v->buttons | bit : v->buttons & ~bit;
        break;
    }
    case EV_REL:
        if (e->code == REL_X)
            v->dx += e->value;
        else if (e->code == REL_Y)
            v->dy += e->value;
        else if (e->code == REL_WHEEL)
            v->wheel -= e->value;
        break;
    case EV_ABS:
        if (e->code == ABS_X)
            v->x = e->value;
        else if (e->code == ABS_Y)
            v->y = e->value;
        else if (e->code == ABS_MT_SLOT)
            v->slot = e->value >= 0 && e->value < TOUCH_MAX ? e->value : -1;
        else if (v->slot >= 0 && e->code == ABS_MT_TRACKING_ID)
            v->slots[v->slot].id = e->value < 0 ? -1 : e->value;
        else if (v->slot >= 0 && e->code == ABS_MT_POSITION_X)
            v->slots[v->slot].x = e->value;
        else if (v->slot >= 0 && e->code == ABS_MT_POSITION_Y)
            v->slots[v->slot].y = e->value;
        break;
    }
}

static void poll(void)
{
    for (int k = 0; k < input_count; k++) {
        struct vinput *v = inputs[k];
        uint16_t done = v->used[1];
        if (done == v->seen)
            continue;
        while (v->seen != done) {
            const volatile uint32_t *elem = (const volatile uint32_t *)(v->used + 2) + 2 * (v->seen % QSIZE);
            uint32_t id = elem[0] % QSIZE;
            event(v, &v->events[id]);
            v->avail[2 + v->next_avail % QSIZE] = (uint16_t)id;
            v->next_avail++;
            v->seen++;
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        v->avail[1] = v->next_avail;
        *v->notify = 0;
    }
}

static bool start(struct vinput *v, const struct pci_device *d)
{
    uint8_t cap = (uint8_t)(pci_read(d, 0x34) & 0xfc);
    for (int guard = 0; cap && guard < 32; guard++) {
        uint32_t w0 = pci_read(d, cap);
        if ((w0 & 0xff) == 0x09) {
            uint8_t type = (uint8_t)(w0 >> 24), bar = (uint8_t)(pci_read(d, (uint8_t)(cap + 4)) & 0xff);
            uint32_t off = pci_read(d, (uint8_t)(cap + 8)), len = pci_read(d, (uint8_t)(cap + 12));
            if (type == 1)
                v->common = map_cap(d, bar, off, len);
            else if (type == 2) {
                v->notify_base = map_cap(d, bar, off, len);
                v->notify_mul = pci_read(d, (uint8_t)(cap + 16));
            } else if (type == 4)
                v->device = map_cap(d, bar, off, len);
        }
        cap = (uint8_t)((w0 >> 8) & 0xfc);
    }
    if (!v->common || !v->notify_base || !v->device)
        return false;
    C8(20) = 0;
    for (int i = 0; i < 100000 && C8(20); i++)
        ;
    C8(20) = 1;
    C8(20) |= 2;
    C32(8) = 0;
    C32(12) = 0;
    C32(8) = 1;
    C32(12) = 1; /* VIRTIO_F_VERSION_1 */
    C8(20) |= 8;
    if (!(C8(20) & 8))
        return false;
    C16(22) = 0; /* the event queue */
    if (C16(24) < QSIZE)
        return false;
    C16(24) = QSIZE;
    uint8_t *mem = kmalloc_pages(4096);
    v->events = kmalloc_pages(4096);
    if (!mem || !v->events)
        return false;
    memset(mem, 0, 4096);
    v->desc = (struct desc *)mem;
    v->avail = (volatile uint16_t *)(mem + 1024);
    v->used = (volatile uint16_t *)(mem + 2048);
    for (int i = 0; i < QSIZE; i++) {
        v->desc[i] = (struct desc){vmm_phys(&v->events[i]), sizeof(struct event), 2, 0}; /* written by the device */
        v->avail[2 + i] = (uint16_t)i;
    }
    v->next_avail = QSIZE;
    v->avail[1] = QSIZE;
    C64(32) = vmm_phys(v->desc);
    C64(40) = vmm_phys((const void *)v->avail);
    C64(48) = vmm_phys((const void *)v->used);
    v->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)C16(30) * v->notify_mul);
    C16(28) = 1;
    C8(20) |= 4;
    *v->notify = 0;
    return true;
}

static void found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (d->vendor != 0x1af4 || d->device != 0x1052 || input_count == MAX_DEVICES)
        return;
    struct vinput *v = kzalloc(sizeof(*v));
    if (!v)
        return;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x6);
    if (!start(v, d)) {
        klog("virtio-input: %02x:%02x.%u did not start", d->bus, d->dev, d->fn);
        kfree(v);
        return;
    }
    uint8_t len = config(v, CFG_ID_NAME, 0);
    for (int i = 0; i < len && i < (int)sizeof(v->name) - 1; i++)
        v->name[i] = (char)v->device[8 + i];
    int mt_x = abs_max(v, ABS_MT_POSITION_X), mt_y = abs_max(v, ABS_MT_POSITION_Y);
    v->multitouch = mt_x > 0 && mt_y > 0;
    v->max_x = v->multitouch ? mt_x : abs_max(v, ABS_X);
    v->max_y = v->multitouch ? mt_y : abs_max(v, ABS_Y);
    v->absolute = v->max_x > 0 && v->max_y > 0;
    for (int i = 0; i < TOUCH_MAX; i++)
        v->slots[i].id = -1;
    v->pad = (struct touch_pad){.max_x = v->max_x, .max_y = v->max_y, .direct = true,
                                .out = {pointer, absolute, keyboard_inject}};
    klog("virtio-input: %s (%s)", v->name, v->multitouch ? "multi-touch screen" : v->absolute ? "tablet" : "mouse");
    if (v->multitouch)
        touch_add(v->name, true);
    inputs[input_count++] = v;
    if (input_count == 1)
        timer_hook(poll);
}

void virtio_input_init(void)
{
    pci_foreach(found, NULL);
}

#else

void virtio_input_init(void) {}

#endif
