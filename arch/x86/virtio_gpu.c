/* virtio-gpu: the paravirtual display of QEMU, KVM, crosvm and the clouds (virtio 1.x over PCI).
   The screen is a resource in guest memory that the host shows; any size can be set, every display (scanout) of
   the device is its own output, and what the window manager redraws is sent over with a transfer and a flush. */

#ifdef __x86_64__

#include <synth/fbcon.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"
#include "virtio_gpu.h"

#define QSIZE 64
#define SLOTS (QSIZE / 2 - 1) /* two descriptors each; the last two carry a backing list and its reply */
#define SLOT_BYTES 512           /* a request and its reply: 1 KB, never across a page */

enum {
    CMD_GET_DISPLAY_INFO = 0x0100,
    CMD_RESOURCE_CREATE_2D,
    CMD_RESOURCE_UNREF,
    CMD_SET_SCANOUT,
    CMD_RESOURCE_FLUSH,
    CMD_TRANSFER_TO_HOST_2D,
    CMD_RESOURCE_ATTACH_BACKING,
    CMD_RESOURCE_DETACH_BACKING,
    CMD_UPDATE_CURSOR = 0x0300,
    CMD_MOVE_CURSOR,
    RESP_OK_NODATA = 0x1100,
    RESP_OK_DISPLAY_INFO,
    FORMAT_B8G8R8A8 = 1,
    FORMAT_B8G8R8X8 = 2,
};

struct hdr {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t ctx;
    uint8_t ring, pad[3];
};

struct vrect {
    uint32_t x, y, w, h;
};

struct desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};

struct vq {
    struct desc *desc;
    volatile uint16_t *avail; /* flags, idx, ring[] */
    volatile uint16_t *used;  /* flags, idx, then {id u32, len u32}[] */
    uint16_t size, next_avail, seen;
    volatile uint16_t *notify;
    uint8_t *slots; /* SLOTS request + reply buffers of SLOT_BYTES each */
};

struct vgpu {
    struct pci_device pci;
    volatile uint8_t *common, *device;
    volatile uint8_t *notify_base;
    uint32_t notify_mul;
    struct vq ctrl;
    /* the cursor queue: one device-readable buffer per command, no answers */
    struct desc *cdesc;
    volatile uint16_t *cavail, *cused, *cnotify;
    uint16_t csize, cnext;
    uint8_t *cbuf;
    uint32_t cursor_res;
    uint32_t *cursor_px; /* 64 x 64 */
    uint32_t scanouts;
    struct vrect info[16];
    uint32_t next_resource;
};

static struct vgpu gpus[2];
static int gpu_count;

#define C8(o) (*(volatile uint8_t *)(g->common + (o)))
#define C16(o) (*(volatile uint16_t *)(g->common + (o)))
#define C32(o) (*(volatile uint32_t *)(g->common + (o)))
#define C64(o) (*(volatile uint64_t *)(g->common + (o)))

static volatile uint8_t *map_cap(const struct pci_device *d, uint8_t bar, uint32_t offset, uint32_t length)
{
    uint64_t base;
    if (bar > 5 || !pci_bar_base(d, (uint8_t)(0x10 + bar * 4), &base))
        return NULL;
    uint8_t *p = vmm_map_phys64(base + offset, MAX(length, 4096u));
    return p;
}

static bool queue_init(struct vgpu *g, int index, struct vq *q)
{
    C16(22) = (uint16_t)index;
    uint16_t size = C16(24);
    if (!size)
        return false;
    if (size < QSIZE)
        return false;
    q->size = QSIZE;
    C16(24) = q->size;
    uint8_t *mem = kmalloc_pages(4096);
    q->slots = kmalloc_pages(ALIGN_UP(SLOTS * SLOT_BYTES * 2, 4096));
    if (!mem || !q->slots)
        return false;
    q->desc = (struct desc *)mem;
    q->avail = (volatile uint16_t *)(mem + 1024);
    q->used = (volatile uint16_t *)(mem + 2048);
    C64(32) = vmm_phys(q->desc);
    C64(40) = vmm_phys((const void *)q->avail);
    C64(48) = vmm_phys((const void *)q->used);
    q->notify = (volatile uint16_t *)(g->notify_base + (uint32_t)C16(30) * g->notify_mul);
    C16(28) = 1;
    return true;
}

/* Puts a request (read by the device) and its reply (written by it) on the queue; does not wait. */
static int submit(struct vgpu *g, const void *req, uint32_t req_len, const void *extra, uint32_t extra_len,
                  uint32_t reply_len, uint8_t **reply)
{
    struct vq *q = &g->ctrl;
    uint64_t until = hal_ticks_ms() + 500;
    while ((uint16_t)(q->next_avail - q->used[1]) >= SLOTS) /* full: wait for the oldest */
        if (hal_ticks_ms() > until)
            return -1;
    int slot = q->next_avail % SLOTS;
    uint8_t *rq = q->slots + slot * SLOT_BYTES * 2, *rp = rq + SLOT_BYTES;
    memcpy(rq, req, MIN(req_len, (uint32_t)SLOT_BYTES));
    memset(rp, 0, SLOT_BYTES);
    uint16_t d0 = (uint16_t)(slot * 2), d1 = (uint16_t)(d0 + 1);
    if (extra) { /* request, the list after it, reply: the list and the reply take the spare descriptors */
        uint16_t d2 = (uint16_t)(QSIZE - 2), d3 = (uint16_t)(QSIZE - 1);
        q->desc[d0] = (struct desc){vmm_phys(rq), req_len, 1, d2};
        q->desc[d2] = (struct desc){vmm_phys(extra), extra_len, 1, d3};
        q->desc[d3] = (struct desc){vmm_phys(rp), reply_len, 2, 0};
    } else {
        q->desc[d0] = (struct desc){vmm_phys(rq), req_len, 1, d1};
        q->desc[d1] = (struct desc){vmm_phys(rp), reply_len, 2, 0};
    }
    q->avail[2 + q->next_avail % q->size] = d0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->next_avail++;
    q->avail[1] = q->next_avail;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify = 0;
    if (reply)
        *reply = rp;
    return slot;
}

/* the cursor queue: smaller than the control queue on most hosts */
static bool cursor_queue_init(struct vgpu *g)
{
    C16(22) = 1;
    uint16_t size = C16(24);
    if (!size)
        return false;
    g->csize = size < 16 ? size : 16;
    C16(24) = g->csize;
    uint8_t *mem = kmalloc_pages(4096);
    g->cbuf = kmalloc_pages(4096);
    if (!mem || !g->cbuf)
        return false;
    g->cdesc = (struct desc *)mem;
    g->cavail = (volatile uint16_t *)(mem + 1024);
    g->cused = (volatile uint16_t *)(mem + 2048);
    C64(32) = vmm_phys(g->cdesc);
    C64(40) = vmm_phys((const void *)g->cavail);
    C64(48) = vmm_phys((const void *)g->cused);
    g->cnotify = (volatile uint16_t *)(g->notify_base + (uint32_t)C16(30) * g->notify_mul);
    C16(28) = 1;
    return true;
}

static bool cursor_submit(struct vgpu *g, const void *cmd, uint32_t len)
{
    if (!g->csize)
        return false;
    uint64_t until = hal_ticks_ms() + 100;
    while ((uint16_t)(g->cnext - g->cused[1]) >= g->csize)
        if (hal_ticks_ms() > until)
            return false;
    uint16_t slot = (uint16_t)(g->cnext % g->csize);
    uint8_t *buf = g->cbuf + slot * 128;
    memcpy(buf, cmd, MIN(len, 128u));
    g->cdesc[slot] = (struct desc){vmm_phys(buf), len, 0, 0};
    g->cavail[2 + g->cnext % g->csize] = slot;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g->cnext++;
    g->cavail[1] = g->cnext;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *g->cnotify = 1;
    return true;
}

static bool wait_all(struct vgpu *g)
{
    struct vq *q = &g->ctrl;
    uint64_t until = hal_ticks_ms() + 1000;
    while (q->used[1] != q->next_avail)
        if (hal_ticks_ms() > until)
            return false;
    return true;
}

/* A request and its answer, waited for. */
static bool call(struct vgpu *g, const void *req, uint32_t len, const void *extra, uint32_t extra_len,
                 void *answer, uint32_t answer_len, uint32_t want)
{
    uint8_t *rp;
    if (!wait_all(g) || submit(g, req, len, extra, extra_len, MAX(answer_len, (uint32_t)sizeof(struct hdr)), &rp) < 0 ||
        !wait_all(g))
        return false;
    if (answer)
        memcpy(answer, rp, answer_len);
    return ((struct hdr *)rp)->type == want;
}

static void display_info(struct vgpu *g)
{
    struct hdr h = {.type = CMD_GET_DISPLAY_INFO};
    static uint8_t answer[24 + 16 * 24];
    if (!call(g, &h, sizeof(h), NULL, 0, answer, sizeof(answer), RESP_OK_DISPLAY_INFO))
        return;
    for (int i = 0; i < 16; i++) {
        const uint32_t *p = (const uint32_t *)(answer + 24 + i * 24);
        if (p[4]) {
            g->info[i] = (struct vrect){p[0], p[1], p[2], p[3]};
            g->scanouts = (uint32_t)i + 1;
        }
    }
}

bool virtio_gpu_probe(const struct pci_device *d, int *scanouts, uint16_t *w, uint16_t *h)
{
    if (gpu_count >= (int)ARRAY_SIZE(gpus) || d->vendor != 0x1af4 || d->device != 0x1050)
        return false;
    struct vgpu *g = &gpus[gpu_count];
    memset(g, 0, sizeof(*g));
    g->pci = *d;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x6);
    uint8_t cap = (uint8_t)(pci_read(d, 0x34) & 0xfc);
    for (int guard = 0; cap && guard < 32; guard++) {
        uint32_t w0 = pci_read(d, cap);
        if ((w0 & 0xff) == 0x09) {
            uint8_t type = (uint8_t)(w0 >> 24), bar = (uint8_t)(pci_read(d, (uint8_t)(cap + 4)) & 0xff);
            uint32_t off = pci_read(d, (uint8_t)(cap + 8)), len = pci_read(d, (uint8_t)(cap + 12));
            if (type == 1)
                g->common = map_cap(d, bar, off, len);
            else if (type == 2) {
                g->notify_base = map_cap(d, bar, off, len);
                g->notify_mul = pci_read(d, (uint8_t)(cap + 16));
            } else if (type == 4)
                g->device = map_cap(d, bar, off, len);
        }
        cap = (uint8_t)((w0 >> 8) & 0xfc);
    }
    if (!g->common || !g->notify_base)
        return false;
    C8(20) = 0; /* reset */
    for (int i = 0; i < 100000 && C8(20); i++)
        ;
    C8(20) = 1;
    C8(20) |= 2;
    C32(8) = 0;
    C32(12) = 0;
    C32(8) = 1;
    C32(12) = 1; /* VIRTIO_F_VERSION_1 */
    C8(20) |= 8;
    if (!(C8(20) & 8) || !queue_init(g, 0, &g->ctrl)) {
        C8(20) = 0x80;
        return false;
    }
    if (!cursor_queue_init(g)) /* the pointer is then drawn on the screen itself */
        g->csize = 0;
    C8(20) |= 4;
    display_info(g);
    if (!g->scanouts) {
        g->scanouts = 1;
        g->info[0] = (struct vrect){0, 0, 1024, 768};
    }
    g->next_resource = 1;
    *scanouts = (int)g->scanouts;
    *w = (uint16_t)g->info[0].w;
    *h = (uint16_t)g->info[0].h;
    klog("display: virtio-gpu with %u display%s, %ux%u", (unsigned)g->scanouts, g->scanouts > 1 ? "s" : "",
         (unsigned)g->info[0].w, (unsigned)g->info[0].h);
    return gpu_count++, true;
}

int virtio_gpu_last(void)
{
    return gpu_count - 1;
}

void virtio_gpu_preferred(int gpu, int scanout, uint16_t *w, uint16_t *h)
{
    struct vgpu *g = &gpus[gpu];
    *w = (uint16_t)g->info[scanout].w;
    *h = (uint16_t)g->info[scanout].h;
}

/* A new resource of w x h in guest memory, attached and shown on the scanout; the old one goes. */
bool virtio_gpu_set(int gpu, int scanout, int w, int h, struct virtio_screen *vs, struct framebuffer *fb)
{
    struct vgpu *g = &gpus[gpu];
    size_t bytes = (size_t)w * (size_t)h * 4;
    uint8_t *pixels = kmalloc_pages(ALIGN_UP(bytes, 4096));
    struct {
        uint64_t addr;
        uint32_t len, pad;
    } *ents = kmalloc_pages(4096);
    if (!pixels || !ents) {
        kfree_pages(pixels);
        kfree_pages(ents);
        return false;
    }
    int n = 0;
    for (size_t off = 0; off < bytes; off += 4096) { /* the pages, joined where they follow each other */
        uint64_t p = vmm_phys(pixels + off);
        uint32_t len = (uint32_t)MIN((size_t)4096, bytes - off);
        if (n && ents[n - 1].addr + ents[n - 1].len == p)
            ents[n - 1].len += len;
        else if (n < 4096 / 16)
            ents[n++] = (typeof(*ents)){p, len, 0};
        else
            goto fail;
    }
    uint32_t id = g->next_resource++;
    struct {
        struct hdr h;
        uint32_t id, format, w, h2;
    } create = {{.type = CMD_RESOURCE_CREATE_2D}, id, FORMAT_B8G8R8X8, (uint32_t)w, (uint32_t)h};
    struct {
        struct hdr h;
        uint32_t id, count;
    } attach = {{.type = CMD_RESOURCE_ATTACH_BACKING}, id, (uint32_t)n};
    struct {
        struct hdr h;
        struct vrect r;
        uint32_t scanout, id;
    } set = {{.type = CMD_SET_SCANOUT}, {0, 0, (uint32_t)w, (uint32_t)h}, (uint32_t)scanout, id};
    if (!call(g, &create, sizeof(create), NULL, 0, NULL, 0, RESP_OK_NODATA) ||
        !call(g, &attach, sizeof(attach), ents, (uint32_t)n * 16, NULL, 0, RESP_OK_NODATA) ||
        !call(g, &set, sizeof(set), NULL, 0, NULL, 0, RESP_OK_NODATA))
        goto fail;
    if (vs->resource) { /* the previous picture of this display */
        struct {
            struct hdr h;
            uint32_t id, pad;
        } unref = {{.type = CMD_RESOURCE_UNREF}, vs->resource, 0};
        call(g, &unref, sizeof(unref), NULL, 0, NULL, 0, RESP_OK_NODATA);
        kfree_pages(vs->pixels);
    }
    kfree_pages(ents);
    *vs = (struct virtio_screen){gpu, scanout, id, pixels, w, h};
    *fb = (struct framebuffer){.base = pixels, .width = (uint32_t)w, .height = (uint32_t)h, .pitch = (uint32_t)w * 4,
                               .bpp = 32, .red_shift = 16, .green_shift = 8, .blue_shift = 0};
    memset(pixels, 0, bytes);
    virtio_gpu_present(vs, 0, 0, w, h);
    klog("display: virtio-gpu display %d at %dx%d", scanout + 1, w, h);
    return true;
fail:
    kfree_pages(pixels);
    kfree_pages(ents);
    return false;
}

/* Sends what changed in the rectangle to the host and shows it; may be called with interrupts off. */
void virtio_gpu_present(struct virtio_screen *vs, int x, int y, int w, int h)
{
    if (!vs->resource)
        return;
    x = MAX(x, 0), y = MAX(y, 0);
    w = MIN(w, vs->w - x), h = MIN(h, vs->h - y);
    if (w <= 0 || h <= 0)
        return;
    struct vgpu *g = &gpus[vs->gpu];
    uint32_t flags = hal_irq_save();
    struct vrect r = {(uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
    struct {
        struct hdr h;
        struct vrect r;
        uint64_t offset;
        uint32_t id, pad;
    } transfer = {{.type = CMD_TRANSFER_TO_HOST_2D}, r, (uint64_t)y * (uint64_t)vs->w * 4 + (uint64_t)x * 4, vs->resource, 0};
    struct {
        struct hdr h;
        struct vrect r;
        uint32_t id, pad;
    } flush = {{.type = CMD_RESOURCE_FLUSH}, r, vs->resource, 0};
    if (submit(g, &transfer, sizeof(transfer), NULL, 0, sizeof(struct hdr), NULL) >= 0)
        submit(g, &flush, sizeof(flush), NULL, 0, sizeof(struct hdr), NULL);
    hal_irq_restore(flags);
}

#endif

#ifdef __x86_64__
struct cursor_cmd {
    struct hdr h;
    uint32_t scanout, x, y, pad;
    uint32_t id, hot_x, hot_y, pad2;
};

/* The pointer's picture (w, h up to 64; 0xAARRGGBB, not premultiplied) and where it is, shown by the host over the
   screen: no pixels of the screen change when it moves. */
bool virtio_gpu_cursor_image(int gpu, int scanout, const uint32_t *argb, int w, int h, int hot_x, int hot_y, int x, int y)
{
    struct vgpu *g = &gpus[gpu];
    if (gpu < 0 || gpu >= gpu_count || !g->csize || w > 64 || h > 64)
        return false;
    uint32_t flags = hal_irq_save();
    bool ok = true;
    if (!g->cursor_res) {
        g->cursor_px = kmalloc_pages(64 * 64 * 4);
        struct {
            uint64_t addr;
            uint32_t len, pad;
        } *ents = kmalloc_pages(4096);
        ok = g->cursor_px && ents;
        int n = 0;
        for (int off = 0; ok && off < 64 * 64 * 4; off += 4096) {
            uint64_t p = vmm_phys((uint8_t *)g->cursor_px + off);
            if (n && ents[n - 1].addr + ents[n - 1].len == p)
                ents[n - 1].len += 4096;
            else
                ents[n++] = (typeof(*ents)){p, 4096, 0};
        }
        uint32_t id = g->next_resource++;
        struct {
            struct hdr h;
            uint32_t id, format, w, h2;
        } create = {{.type = CMD_RESOURCE_CREATE_2D}, id, FORMAT_B8G8R8A8, 64, 64};
        struct {
            struct hdr h;
            uint32_t id, count;
        } attach = {{.type = CMD_RESOURCE_ATTACH_BACKING}, id, (uint32_t)n};
        ok = ok && call(g, &create, sizeof(create), NULL, 0, NULL, 0, RESP_OK_NODATA) &&
             call(g, &attach, sizeof(attach), ents, (uint32_t)n * 16, NULL, 0, RESP_OK_NODATA);
        kfree_pages(ents);
        if (ok)
            g->cursor_res = id;
    }
    if (ok) {
        memset(g->cursor_px, 0, 64 * 64 * 4);
        for (int j = 0; j < h; j++)
            memcpy(g->cursor_px + j * 64, argb + j * w, (size_t)w * 4);
        struct {
            struct hdr h;
            struct vrect r;
            uint64_t offset;
            uint32_t id, pad;
        } transfer = {{.type = CMD_TRANSFER_TO_HOST_2D}, {0, 0, 64, 64}, 0, g->cursor_res, 0};
        ok = call(g, &transfer, sizeof(transfer), NULL, 0, NULL, 0, RESP_OK_NODATA);
    }
    if (ok) {
        struct cursor_cmd c = {{.type = CMD_UPDATE_CURSOR}, (uint32_t)scanout, (uint32_t)x, (uint32_t)y, 0,
                               g->cursor_res, (uint32_t)hot_x, (uint32_t)hot_y, 0};
        ok = cursor_submit(g, &c, sizeof(c));
    }
    hal_irq_restore(flags);
    return ok;
}

/* moves the pointer, or hides it (show false) */
bool virtio_gpu_cursor_move(int gpu, int scanout, int x, int y, bool show)
{
    struct vgpu *g = &gpus[gpu];
    if (gpu < 0 || gpu >= gpu_count || !g->cursor_res)
        return false;
    uint32_t flags = hal_irq_save();
    struct cursor_cmd c = {{.type = show ? CMD_MOVE_CURSOR : CMD_UPDATE_CURSOR}, (uint32_t)scanout,
                           (uint32_t)(x < 0 ? 0 : x), (uint32_t)(y < 0 ? 0 : y), 0, 0, 0, 0, 0};
    if (show)
        c.id = g->cursor_res;
    bool ok = cursor_submit(g, &c, sizeof(c));
    hal_irq_restore(flags);
    return ok;
}
#endif
