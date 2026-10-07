#include <synth/block.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "usb.h"

#define BUFFER_SIZE 0x10000u
#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u
#define TIMEOUT_MS 10000

struct storage {
    struct blockdev dev;
    struct usb_device *usb;
    uint8_t in, out, interface;
    uint8_t *buffer;
    uint8_t *wrapper;
    uint32_t tag;
    bool gone;
};

static int count;

static bool storage_match(const struct usb_device *dev, const struct usb_interface *intf)
{
    UNUSED(dev);
    return intf->cls == 8 && (intf->subclass == 6 || intf->subclass == 5 || intf->subclass == 2) &&
           intf->protocol == 0x50;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int transfer(struct storage *s, uint8_t endpoint, void *data, size_t length)
{
    const struct usb_host_ops *ops = s->usb->host->ops;
    int n = ops->transfer(s->usb, endpoint, data, length, TIMEOUT_MS);
    if (n == E_PIPE)
        ops->clear_halt(s->usb, endpoint);
    return n;
}

static void reset_recovery(struct storage *s)
{
    usb_control(s->usb, USB_REQ_CLASS | USB_REQ_INTERFACE, 0xff, 0, s->interface, NULL, 0);
    s->usb->host->ops->clear_halt(s->usb, s->in);
    s->usb->host->ops->clear_halt(s->usb, s->out);
}

static int scsi(struct storage *s, const uint8_t *cdb, uint8_t cdb_length, uint32_t length, bool in)
{
    if (s->gone || s->usb->gone)
        return E_NODEV;
    uint8_t *cbw = s->wrapper;
    memset(cbw, 0, 31);
    uint32_t tag = ++s->tag;
    memcpy(cbw, &(uint32_t){CBW_SIGNATURE}, 4);
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &length, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = cdb_length;
    memcpy(cbw + 15, cdb, cdb_length);
    int n = transfer(s, s->out, cbw, 31);
    if (n < 0) {
        reset_recovery(s);
        return n;
    }
    if (length) {
        n = transfer(s, in ? s->in : s->out, s->buffer, length);
        if (n < 0 && n != E_PIPE) {
            reset_recovery(s);
            return n;
        }
    }
    uint8_t *csw = s->wrapper + 64;
    n = transfer(s, s->in, csw, 13);
    if (n == E_PIPE)
        n = transfer(s, s->in, csw, 13);
    uint32_t signature, csw_tag;
    memcpy(&signature, csw, 4);
    memcpy(&csw_tag, csw + 4, 4);
    if (n < 13 || signature != CSW_SIGNATURE || csw_tag != tag || csw[12] == 2) {
        reset_recovery(s);
        return E_IO;
    }
    return csw[12] == 0 ? 0 : E_IO;
}

static void request_sense(struct storage *s)
{
    uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    scsi(s, cdb, 6, 18, true);
}

static int rw(struct storage *s, bool write, uint32_t lba, uint32_t sectors)
{
    uint8_t cdb[10] = {write ? 0x2a : 0x28};
    put_be32(cdb + 2, lba);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)sectors;
    int err = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = scsi(s, cdb, 10, sectors * BLOCK_SIZE, !write);
        if (err != E_IO)
            break;
        request_sense(s);
    }
    return err;
}

static int storage_read(struct blockdev *dev, uint32_t lba, uint32_t total, void *buf)
{
    struct storage *s = dev->priv;
    uint8_t *out = buf;
    while (total) {
        uint32_t k = MIN(total, BUFFER_SIZE / BLOCK_SIZE);
        int err = rw(s, false, lba, k);
        if (err)
            return err;
        memcpy(out, s->buffer, k * BLOCK_SIZE);
        out += k * BLOCK_SIZE;
        lba += k;
        total -= k;
    }
    return 0;
}

static int storage_write(struct blockdev *dev, uint32_t lba, uint32_t total, const void *buf)
{
    struct storage *s = dev->priv;
    const uint8_t *in = buf;
    while (total) {
        uint32_t k = MIN(total, BUFFER_SIZE / BLOCK_SIZE);
        memcpy(s->buffer, in, k * BLOCK_SIZE);
        int err = rw(s, true, lba, k);
        if (err)
            return err;
        in += k * BLOCK_SIZE;
        lba += k;
        total -= k;
    }
    return 0;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
}

static int storage_attach(struct usb_device *dev, int index)
{
    const struct usb_interface *intf = &dev->interfaces[index];
    const struct usb_endpoint *in = usb_find_endpoint(intf, 2, true), *out = usb_find_endpoint(intf, 2, false);
    if (!in || !out)
        return E_NOSYS;
    struct storage *s = kzalloc(sizeof(*s));
    if (!s)
        return E_NOMEM;
    s->usb = dev;
    s->in = in->address;
    s->out = out->address;
    s->interface = intf->number;
    s->buffer = kmalloc_pages(BUFFER_SIZE);
    s->wrapper = kmalloc_pages(4096);
    if (!s->buffer || !s->wrapper)
        goto fail;

    uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    if (scsi(s, inquiry, 6, 36, true) == 0) {
        char vendor[9], product[17];
        memcpy(vendor, s->buffer + 8, 8);
        vendor[8] = 0;
        memcpy(product, s->buffer + 16, 16);
        product[16] = 0;
        trim(vendor);
        trim(product);
        ksnprintf(s->dev.model, sizeof(s->dev.model), "%s %s", vendor, product);
    }
    bool ready = false;
    for (int i = 0; i < 40 && !ready; i++) {
        uint8_t tur[6] = {0};
        ready = scsi(s, tur, 6, 0, false) == 0;
        if (!ready) {
            request_sense(s);
            hal_sleep_ms(100);
        }
    }
    uint8_t capacity[10] = {0x25};
    if (!ready || scsi(s, capacity, 10, 8, true) != 0)
        goto fail;
    uint32_t last = get_be32(s->buffer), block = get_be32(s->buffer + 4);
    if (block != BLOCK_SIZE || !last) {
        klog("usb storage: unsupported block size %u", (unsigned)block);
        goto fail;
    }
    s->dev.sectors = last == 0xffffffffu ? last : last + 1;
    ksnprintf(s->dev.name, sizeof(s->dev.name), "usb%d", count++);
    s->dev.read = storage_read;
    s->dev.write = storage_write;
    s->dev.priv = s;
    dev->priv[index] = s;
    block_register(&s->dev);
    char size[16];
    fmt_size(size, sizeof(size), (uint64_t)s->dev.sectors * BLOCK_SIZE);
    klog("usb storage: %s %s (%s)", s->dev.name, s->dev.model, size);
    return 0;
fail:
    if (s->buffer)
        kfree_pages(s->buffer);
    if (s->wrapper)
        kfree_pages(s->wrapper);
    kfree(s);
    return E_IO;
}

static void storage_detach(struct usb_device *dev, int index)
{
    struct storage *s = dev->priv[index];
    if (s) {
        s->gone = true;
        block_unregister(&s->dev);
    }
    dev->priv[index] = NULL;
}

const struct usb_driver usb_storage_driver = {
    .name = "storage",
    .match = storage_match,
    .attach = storage_attach,
    .detach = storage_detach,
};
