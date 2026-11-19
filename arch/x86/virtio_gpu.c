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
    CMD_GET_CAPSET_INFO,
    CMD_GET_CAPSET,
    CMD_CTX_CREATE = 0x0200,
    CMD_CTX_DESTROY,
    CMD_CTX_ATTACH_RESOURCE,
    CMD_CTX_DETACH_RESOURCE,
    CMD_RESOURCE_CREATE_3D,
    CMD_TRANSFER_TO_HOST_3D,
    CMD_TRANSFER_FROM_HOST_3D,
    CMD_SUBMIT_3D,
    CMD_UPDATE_CURSOR = 0x0300,
    CMD_MOVE_CURSOR,
    RESP_OK_NODATA = 0x1100,
    RESP_OK_DISPLAY_INFO,
    RESP_OK_CAPSET_INFO,
    RESP_OK_CAPSET,
    FLAG_FENCE = 1,
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
    /* 3D: the host renders (virgl); capability sets; the last fence asked for; contexts made */
    bool virgl;
    uint32_t capsets;
    uint64_t fence;
    uint32_t next_ctx;
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
    uint64_t until = hal_ticks_ms() + 5000; /* a fenced 3D request waits for the host's rendering */
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
    C32(0) = 0;
    uint32_t offered = C32(4);
    C32(8) = 0;
    C32(12) = offered & 1; /* VIRTIO_GPU_F_VIRGL: 3D commands rendered by the host */
    C32(8) = 1;
    C32(12) = 1; /* VIRTIO_F_VERSION_1 */
    g->virgl = offered & 1;
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
    g->next_ctx = 1;
    if (g->virgl && g->device)
        g->capsets = *(volatile uint32_t *)(g->device + 12);
    *scanouts = (int)g->scanouts;
    *w = (uint16_t)g->info[0].w;
    *h = (uint16_t)g->info[0].h;
    klog("display: virtio-gpu with %u display%s, %ux%u%s", (unsigned)g->scanouts, g->scanouts > 1 ? "s" : "",
         (unsigned)g->info[0].w, (unsigned)g->info[0].h, g->virgl ? ", 3D (virgl)" : "");
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
    *vs = (struct virtio_screen){gpu, scanout, id, pixels, w, h, 0};
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

#ifdef __x86_64__
/* ---- 3D: contexts that the host renders for (virgl), resources in its memory, command streams, fences */

#include <synth/command.h>
#include <synth/shell.h>

struct backing_ent {
    uint64_t addr;
    uint32_t len, pad;
};

static struct vgpu *gpu3d(int gpu)
{
    return gpu >= 0 && gpu < gpu_count && gpus[gpu].virgl ? &gpus[gpu] : NULL;
}

bool virtio_gpu_has_3d(int gpu)
{
    return gpu3d(gpu) != NULL;
}

/* A request that carries a fence: the device answers once the host has done everything submitted before it. */
static bool fenced(struct vgpu *g, struct hdr *h, uint32_t len, const void *extra, uint32_t extra_len)
{
    h->flags |= FLAG_FENCE;
    h->fence = ++g->fence;
    return call(g, h, len, extra, extra_len, NULL, 0, RESP_OK_NODATA);
}

/* the capability sets the host offers: id, highest version, size */
int virtio_gpu_capsets(int gpu, uint32_t (*out)[3], int max)
{
    struct vgpu *g = gpu3d(gpu);
    int n = 0;
    for (uint32_t i = 0; g && i < g->capsets && n < max; i++) {
        struct {
            struct hdr h;
            uint32_t index, pad;
        } q = {{.type = CMD_GET_CAPSET_INFO}, i, 0};
        uint32_t a[10];
        if (call(g, &q, sizeof(q), NULL, 0, a, sizeof(a), RESP_OK_CAPSET_INFO))
            out[n][0] = a[6], out[n][1] = a[7], out[n][2] = a[8], n++;
    }
    return n;
}

/* capability set id, version 1 into buf (at most a reply slot); asking also makes virglrenderer work out the limits it
   checks commands against, such as the largest texture buffer */
bool virtio_gpu_capset(int gpu, uint32_t id, void *buf, uint32_t size)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g)
        return false;
    struct {
        struct hdr h;
        uint32_t id, version;
    } q = {{.type = CMD_GET_CAPSET}, id, 1};
    uint8_t a[SLOT_BYTES];
    if (!call(g, &q, sizeof(q), NULL, 0, a, sizeof(a), RESP_OK_CAPSET))
        return false;
    if (buf)
        memcpy(buf, a + sizeof(struct hdr), MIN(size, (uint32_t)(sizeof(a) - sizeof(struct hdr))));
    return true;
}

/* a rendering context on the host (0 when it cannot be made) */
uint32_t virtio_gpu_ctx_create(int gpu, const char *name)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g)
        return 0;
    struct {
        struct hdr h;
        uint32_t nlen, init;
        char name[64];
    } c = {{.type = CMD_CTX_CREATE, .ctx = g->next_ctx}, 0, 0, {0}};
    str_copy(c.name, name, sizeof(c.name));
    c.nlen = (uint32_t)strlen(c.name);
    if (!call(g, &c, sizeof(c), NULL, 0, NULL, 0, RESP_OK_NODATA))
        return 0;
    return g->next_ctx++;
}

void virtio_gpu_ctx_destroy(int gpu, uint32_t ctx)
{
    struct vgpu *g = gpu3d(gpu);
    struct hdr h = {.type = CMD_CTX_DESTROY, .ctx = ctx};
    if (g && ctx)
        call(g, &h, sizeof(h), NULL, 0, NULL, 0, RESP_OK_NODATA);
}

/* Pages of guest memory behind a resource, joined where they follow each other; NULL when they do not fit one
   page of entries. */
static bool attach_backing(struct vgpu *g, uint32_t id, void *mem, size_t bytes)
{
    struct backing_ent *ents = kmalloc_pages(4096);
    if (!ents)
        return false;
    int n = 0;
    bool ok = true;
    for (size_t off = 0; ok && off < bytes; off += 4096) {
        uint64_t p = vmm_phys((uint8_t *)mem + off);
        uint32_t len = (uint32_t)MIN((size_t)4096, bytes - off);
        if (n && ents[n - 1].addr + ents[n - 1].len == p)
            ents[n - 1].len += len;
        else if (n < 4096 / 16)
            ents[n++] = (struct backing_ent){p, len, 0};
        else
            ok = false;
    }
    struct {
        struct hdr h;
        uint32_t id, count;
    } attach = {{.type = CMD_RESOURCE_ATTACH_BACKING}, id, (uint32_t)n};
    ok = ok && call(g, &attach, sizeof(attach), ents, (uint32_t)n * 16, NULL, 0, RESP_OK_NODATA);
    kfree_pages(ents);
    return ok;
}

/* A resource on the host, used by the context: a buffer (target 0) or a texture, with guest memory behind it when
   backing is not NULL (for transfers). Returns its id, or 0. */
uint32_t virtio_gpu_resource_3d(int gpu, uint32_t ctx, const struct virtio_res3d *r, void *backing, size_t bytes)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g)
        return 0;
    uint32_t id = g->next_resource++;
    struct {
        struct hdr h;
        uint32_t id, target, format, bind, w, h2, depth, array, last_level, samples, flags, pad;
    } c = {{.type = CMD_RESOURCE_CREATE_3D, .ctx = ctx}, id, r->target, r->format, r->bind, r->w, r->h ? r->h : 1,
           r->depth ? r->depth : 1, r->array ? r->array : 1, r->levels ? r->levels - 1 : 0, r->samples, r->flags, 0};
    struct {
        struct hdr h;
        uint32_t id, pad;
    } a = {{.type = CMD_CTX_ATTACH_RESOURCE, .ctx = ctx}, id, 0};
    if (!call(g, &c, sizeof(c), NULL, 0, NULL, 0, RESP_OK_NODATA) ||
        (backing && !attach_backing(g, id, backing, bytes)) || !call(g, &a, sizeof(a), NULL, 0, NULL, 0, RESP_OK_NODATA))
        return 0;
    return id;
}

/* Lets context ctx use resource id too (one made for another context). */
bool virtio_gpu_ctx_attach(int gpu, uint32_t ctx, uint32_t id)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g || !id)
        return false;
    struct {
        struct hdr h;
        uint32_t id, pad;
    } a = {{.type = CMD_CTX_ATTACH_RESOURCE, .ctx = ctx}, id, 0};
    return call(g, &a, sizeof(a), NULL, 0, NULL, 0, RESP_OK_NODATA);
}

void virtio_gpu_resource_unref(int gpu, uint32_t ctx, uint32_t id)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g || !id)
        return;
    struct {
        struct hdr h;
        uint32_t id, pad;
    } d = {{.type = CMD_CTX_DETACH_RESOURCE, .ctx = ctx}, id, 0}, u = {{.type = CMD_RESOURCE_UNREF}, id, 0};
    call(g, &d, sizeof(d), NULL, 0, NULL, 0, RESP_OK_NODATA);
    call(g, &u, sizeof(u), NULL, 0, NULL, 0, RESP_OK_NODATA);
}

/* Copies a box of a resource between the host and its guest memory (to_host true: guest to host), and waits until
   the host has done it and everything before it. */
bool virtio_gpu_transfer_3d(int gpu, uint32_t ctx, uint32_t id, bool to_host, uint32_t x, uint32_t y, uint32_t w,
                            uint32_t h, uint32_t stride)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g)
        return false;
    struct {
        struct hdr h;
        uint32_t box[6];
        uint64_t offset;
        uint32_t id, level, stride, layer_stride;
    } t = {{.type = to_host ? CMD_TRANSFER_TO_HOST_3D : CMD_TRANSFER_FROM_HOST_3D, .ctx = ctx},
           {x, y, 0, w, h, 1},
           (uint64_t)y * stride + (uint64_t)x * 4,
           id,
           0,
           stride,
           0};
    return fenced(g, &t.h, sizeof(t), NULL, 0);
}

/* A box of one level of a resource between the host and its guest memory, which holds it at offset with rows stride
   bytes apart and layers (or depth slices) layer_stride apart; with wait, returns once the host has done it. */
bool virtio_gpu_transfer_box(int gpu, uint32_t ctx, uint32_t id, bool to_host, uint32_t level, const uint32_t box[6],
                             uint64_t offset, uint32_t stride, uint32_t layer_stride, bool wait)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g)
        return false;
    struct {
        struct hdr h;
        uint32_t box[6];
        uint64_t offset;
        uint32_t id, level, stride, layer_stride;
    } t = {{.type = to_host ? CMD_TRANSFER_TO_HOST_3D : CMD_TRANSFER_FROM_HOST_3D, .ctx = ctx},
           {box[0], box[1], box[2], box[3], box[4], box[5]},
           offset,
           id,
           level,
           stride,
           layer_stride};
    if (wait)
        return fenced(g, &t.h, sizeof(t), NULL, 0);
    return call(g, &t, sizeof(t), NULL, 0, NULL, 0, RESP_OK_NODATA);
}

/* Sends a command stream (virgl's protocol) to the context, from memory in one page; with wait, returns once the
   host has run it. */
void virtio_gpu_scanout_3d(struct virtio_screen *vs, uint32_t id, int w, int h)
{
    struct vgpu *g = &gpus[vs->gpu];
    uint32_t res = id ? id : vs->resource;
    struct vrect r = {0, 0, (uint32_t)(id ? w : vs->w), (uint32_t)(id ? h : vs->h)};
    struct {
        struct hdr h;
        struct vrect r;
        uint32_t scanout, id;
    } set = {{.type = CMD_SET_SCANOUT}, r, (uint32_t)vs->scanout, res};
    struct {
        struct hdr h;
        struct vrect r;
        uint32_t id, pad;
    } flush = {{.type = CMD_RESOURCE_FLUSH}, r, res, 0};
    uint32_t flags = hal_irq_save();
    if (vs->shown != id)
        submit(g, &set, sizeof(set), NULL, 0, sizeof(struct hdr), NULL);
    vs->shown = id;
    hal_irq_restore(flags);
    if (id) {
        flags = hal_irq_save();
        submit(g, &flush, sizeof(flush), NULL, 0, sizeof(struct hdr), NULL);
        hal_irq_restore(flags);
    } else {
        virtio_gpu_present(vs, 0, 0, vs->w, vs->h);
    }
}

bool virtio_gpu_submit_3d(int gpu, uint32_t ctx, const uint32_t *cmd, uint32_t dwords, bool wait)
{
    struct vgpu *g = gpu3d(gpu);
    if (!g || !dwords || dwords > 1024)
        return false;
    struct {
        struct hdr h;
        uint32_t size, pad;
    } s = {{.type = CMD_SUBMIT_3D, .ctx = ctx}, dwords * 4, 0};
    if (wait)
        return fenced(g, &s.h, sizeof(s), cmd, dwords * 4);
    return call(g, &s, sizeof(s), cmd, dwords * 4, NULL, 0, RESP_OK_NODATA);
}

/* ---- a check of the host's rendering: clear a texture, draw a triangle, read the pixels back */

enum {
    VCMD_CREATE_OBJECT = 1, VCMD_BIND_OBJECT = 2, VCMD_SET_VIEWPORT = 4, VCMD_SET_FRAMEBUFFER = 5,
    VCMD_SET_VERTEX_BUFFERS = 6, VCMD_CLEAR = 7, VCMD_DRAW_VBO = 8, VCMD_INLINE_WRITE = 9, VCMD_BIND_SHADER = 31,
    VOBJ_BLEND = 1, VOBJ_RASTERIZER = 2, VOBJ_DSA = 3, VOBJ_SHADER = 4, VOBJ_VERTEX_ELEMENTS = 5, VOBJ_SURFACE = 8,
    VFMT_B8G8R8A8_UNORM = 1, VFMT_R32G32_FLOAT = 29, VFMT_R32G32B32A32_FLOAT = 31, VFMT_R8_UNORM = 64,
    VBIND_RENDER_TARGET = 1 << 1, VBIND_SAMPLER_VIEW = 1 << 3, VBIND_VERTEX_BUFFER = 1 << 4,
};

struct vstream {
    uint32_t *p;
    uint32_t n, cap;
};

static void vput(struct vstream *v, uint32_t w)
{
    if (v->n < v->cap)
        v->p[v->n] = w;
    v->n++;
}

static void vcmd(struct vstream *v, uint32_t cmd, uint32_t obj, uint32_t len)
{
    vput(v, cmd | obj << 8 | len << 16);
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* a TGSI shader in text, as virgl takes it */
static void vshader(struct vstream *v, uint32_t handle, uint32_t type, const char *text)
{
    uint32_t bytes = (uint32_t)strlen(text) + 1, words = (bytes + 3) / 4;
    vcmd(v, VCMD_CREATE_OBJECT, VOBJ_SHADER, 5 + words);
    vput(v, handle), vput(v, type), vput(v, bytes), vput(v, 300), vput(v, 0);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i * 4 + k < bytes; k++)
            w |= (uint32_t)(uint8_t)text[i * 4 + k] << (8 * k);
        vput(v, w);
    }
    vcmd(v, VCMD_BIND_SHADER, 0, 2);
    vput(v, handle), vput(v, type);
}

/* a system without a screen (serial console) has not set the device up: the check does it */
static void probe_found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    int scanouts;
    uint16_t w, h;
    virtio_gpu_probe(d, &scanouts, &w, &h);
}

/* the first virtio-gpu that renders 3D, set up if nothing has done that yet; -1 when there is none */
int virtio_gpu_find_3d(void)
{
    if (!gpu_count)
        pci_foreach(probe_found, NULL);
    for (int i = 0; i < gpu_count; i++)
        if (gpus[i].virgl)
            return i;
    return -1;
}

COMMAND(virgl, NULL, "system", "virgl", "check 3D rendering by the host's graphics card (virtio-gpu with virgl)")
{
    UNUSED(argc), UNUSED(argv);
    if (!gpu_count)
        pci_foreach(probe_found, NULL);
    int gpu = -1;
    for (int i = 0; i < gpu_count; i++)
        if (gpus[i].virgl)
            gpu = i;
    if (gpu < 0)
        return sh_error(sh, "virgl", "no virtio-gpu with 3D (QEMU: -device virtio-vga-gl -display sdl,gl=on)");
    uint32_t caps[8][3];
    int ncaps = virtio_gpu_capsets(gpu, caps, 8);
    for (int i = 0; i < ncaps; i++)
        sh_printf(sh, "capset %u version %u, %u bytes\n", caps[i][0], caps[i][1], caps[i][2]);
    const int W = 64, H = 64;
    uint32_t *pixels = kmalloc_pages(W * H * 4);
    uint32_t *cmd = kmalloc_pages(4096);
    uint32_t ctx = pixels && cmd ? virtio_gpu_ctx_create(gpu, "synthos-check") : 0;
    if (!ctx) {
        kfree_pages(pixels), kfree_pages(cmd);
        return sh_error(sh, "virgl", "no 3D context");
    }
    struct virtio_res3d rt = {.target = 2, .format = VFMT_B8G8R8A8_UNORM, .bind = VBIND_RENDER_TARGET | VBIND_SAMPLER_VIEW,
                              .w = (uint32_t)W, .h = (uint32_t)H};
    struct virtio_res3d vb = {.target = 0, .format = VFMT_R8_UNORM, .bind = VBIND_VERTEX_BUFFER, .w = 72};
    uint32_t rt_id = virtio_gpu_resource_3d(gpu, ctx, &rt, pixels, (size_t)W * H * 4);
    uint32_t vb_id = rt_id ? virtio_gpu_resource_3d(gpu, ctx, &vb, NULL, 0) : 0;
    int rc = 0;
    if (!vb_id) {
        rc = sh_error(sh, "virgl", "the host made no resources");
        goto out;
    }
    struct vstream v = {cmd, 0, 1024};
    /* the render target, cleared to blue */
    vcmd(&v, VCMD_CREATE_OBJECT, VOBJ_SURFACE, 5);
    vput(&v, 1), vput(&v, rt_id), vput(&v, VFMT_B8G8R8A8_UNORM), vput(&v, 0), vput(&v, 0);
    vcmd(&v, VCMD_SET_FRAMEBUFFER, 0, 3);
    vput(&v, 1), vput(&v, 0), vput(&v, 1);
    vcmd(&v, VCMD_CLEAR, 0, 8);
    vput(&v, 4), vput(&v, fbits(0)), vput(&v, fbits(0)), vput(&v, fbits(1)), vput(&v, fbits(1));
    vput(&v, 0), vput(&v, 0x3ff00000), vput(&v, 0); /* depth 1.0 as a double, stencil 0 */
    memset(pixels, 0, (size_t)W * H * 4);
    if (!virtio_gpu_submit_3d(gpu, ctx, cmd, v.n, true) ||
        !virtio_gpu_transfer_3d(gpu, ctx, rt_id, false, 0, 0, (uint32_t)W, (uint32_t)H, (uint32_t)W * 4)) {
        rc = sh_error(sh, "virgl", "the clear did not finish");
        goto out;
    }
    uint32_t cleared = pixels[5 * W + 5];
    /* a red triangle in the middle */
    v.n = 0;
    vcmd(&v, VCMD_CREATE_OBJECT, VOBJ_BLEND, 11);
    vput(&v, 2), vput(&v, 0), vput(&v, 0);
    for (int i = 0; i < 8; i++)
        vput(&v, 0xfu << 27);
    vcmd(&v, VCMD_BIND_OBJECT, VOBJ_BLEND, 1), vput(&v, 2);
    vcmd(&v, VCMD_CREATE_OBJECT, VOBJ_RASTERIZER, 9);
    vput(&v, 3), vput(&v, 1u << 1 | 1u << 29), vput(&v, fbits(1)), vput(&v, 0), vput(&v, 0), vput(&v, fbits(1));
    vput(&v, 0), vput(&v, 0), vput(&v, 0);
    vcmd(&v, VCMD_BIND_OBJECT, VOBJ_RASTERIZER, 1), vput(&v, 3);
    vcmd(&v, VCMD_CREATE_OBJECT, VOBJ_DSA, 5);
    vput(&v, 4), vput(&v, 0), vput(&v, 0), vput(&v, 0), vput(&v, 0);
    vcmd(&v, VCMD_BIND_OBJECT, VOBJ_DSA, 1), vput(&v, 4);
    vshader(&v, 5, 0,
            "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n"
            "  0: MOV OUT[0], IN[0]\n  1: MOV OUT[1], IN[1]\n  2: END\n");
    vshader(&v, 6, 1,
            "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: MOV OUT[0], IN[0]\n  1: END\n");
    vcmd(&v, VCMD_CREATE_OBJECT, VOBJ_VERTEX_ELEMENTS, 9);
    vput(&v, 7);
    vput(&v, 0), vput(&v, 0), vput(&v, 0), vput(&v, VFMT_R32G32_FLOAT);
    vput(&v, 8), vput(&v, 0), vput(&v, 0), vput(&v, VFMT_R32G32B32A32_FLOAT);
    vcmd(&v, VCMD_BIND_OBJECT, VOBJ_VERTEX_ELEMENTS, 1), vput(&v, 7);
    static const float tri[3][6] = {{-0.5f, -0.5f, 1, 0, 0, 1}, {0.5f, -0.5f, 1, 0, 0, 1}, {0, 0.5f, 1, 0, 0, 1}};
    vcmd(&v, VCMD_INLINE_WRITE, 0, 11 + 18);
    vput(&v, vb_id), vput(&v, 0), vput(&v, 0), vput(&v, 0), vput(&v, 0);
    vput(&v, 0), vput(&v, 0), vput(&v, 0), vput(&v, 72), vput(&v, 1), vput(&v, 1);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 6; k++)
            vput(&v, fbits(tri[i][k]));
    vcmd(&v, VCMD_SET_VERTEX_BUFFERS, 0, 3);
    vput(&v, 24), vput(&v, 0), vput(&v, vb_id);
    vcmd(&v, VCMD_SET_VIEWPORT, 0, 7);
    vput(&v, 0);
    vput(&v, fbits(W / 2.0f)), vput(&v, fbits(H / 2.0f)), vput(&v, fbits(0.5f));
    vput(&v, fbits(W / 2.0f)), vput(&v, fbits(H / 2.0f)), vput(&v, fbits(0.5f));
    vcmd(&v, VCMD_DRAW_VBO, 0, 12);
    vput(&v, 0), vput(&v, 3), vput(&v, 4), vput(&v, 0), vput(&v, 1), vput(&v, 0);
    vput(&v, 0), vput(&v, 0), vput(&v, 0), vput(&v, 0), vput(&v, 2), vput(&v, 0);
    if (v.n > v.cap || !virtio_gpu_submit_3d(gpu, ctx, cmd, v.n, true) ||
        !virtio_gpu_transfer_3d(gpu, ctx, rt_id, false, 0, 0, (uint32_t)W, (uint32_t)H, (uint32_t)W * 4)) {
        rc = sh_error(sh, "virgl", "the draw did not finish");
        goto out;
    }
    sh_printf(sh, "virgl clear=%08x middle=%08x corner=%08x fences=%u\n", cleared, pixels[(H / 2) * W + W / 2],
              pixels[2 * W + 2], (unsigned)gpus[gpu].fence);
out:
    virtio_gpu_resource_unref(gpu, ctx, vb_id);
    virtio_gpu_resource_unref(gpu, ctx, rt_id);
    virtio_gpu_ctx_destroy(gpu, ctx);
    kfree_pages(pixels);
    kfree_pages(cmd);
    return rc;
}
#endif
