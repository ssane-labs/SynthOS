#include <synth/block.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "io.h"

#define QUEUE_SIZE 16
#define PAGE 4096u
#define BUFFER_PAGES 16
#define TIMEOUT_MS 5000

enum {
    REG_CAP = 0x00,
    REG_CC = 0x14,
    REG_CSTS = 0x1c,
    REG_AQA = 0x24,
    REG_ASQ = 0x28,
    REG_ACQ = 0x30,
};

struct queue {
    uint32_t *sq;
    uint32_t *cq;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t phase;
    uint16_t id;
};

struct nvme {
    struct blockdev dev;
    volatile uint8_t *regs;
    uint32_t stride;
    struct queue admin;
    struct queue io;
    uint8_t *buffer;
    uint64_t *prp_list;
    uint16_t cid;
};

static struct nvme controllers[2];
static int count;

static uint32_t rd(struct nvme *n, uint32_t reg)
{
    return *(volatile uint32_t *)(n->regs + reg);
}

static void wr(struct nvme *n, uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(n->regs + reg) = v;
}

static void doorbell(struct nvme *n, uint16_t qid, bool completion, uint32_t value)
{
    wr(n, 0x1000 + (2u * qid + (completion ? 1u : 0u)) * n->stride, value);
}

static int submit(struct nvme *n, struct queue *q, uint32_t *cmd)
{
    cmd[0] |= (uint32_t)(++n->cid) << 16;
    memcpy(q->sq + q->sq_tail * 16, cmd, 64);
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_SIZE);
    doorbell(n, q->id, false, q->sq_tail);
    uint64_t end = hal_ticks_ms() + TIMEOUT_MS;
    volatile uint32_t *entry = q->cq + q->cq_head * 4;
    while (((entry[3] >> 16) & 1) != q->phase)
        if (hal_ticks_ms() > end)
            return E_IO;
    uint32_t status = entry[3] >> 17;
    if (++q->cq_head == QUEUE_SIZE) {
        q->cq_head = 0;
        q->phase ^= 1;
    }
    doorbell(n, q->id, true, q->cq_head);
    return status & 0x7ff ? E_IO : 0;
}

static void set_prps(struct nvme *n, uint32_t *cmd, uint32_t bytes)
{
    uint32_t base = vmm_phys(n->buffer);
    cmd[6] = base;
    uint32_t pages = (bytes + PAGE - 1) / PAGE;
    if (pages == 2) {
        cmd[8] = base + PAGE;
    } else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++)
            n->prp_list[i - 1] = base + i * PAGE;
        cmd[8] = vmm_phys(n->prp_list);
    }
}

static int rw(struct nvme *n, uint8_t opcode, uint32_t lba, uint32_t sectors)
{
    uint32_t cmd[16] = {opcode, 1};
    set_prps(n, cmd, sectors * BLOCK_SIZE);
    cmd[10] = lba;
    cmd[12] = sectors - 1;
    return submit(n, &n->io, cmd);
}

static int nvme_read(struct blockdev *dev, uint32_t lba, uint32_t total, void *buf)
{
    struct nvme *n = dev->priv;
    uint8_t *out = buf;
    while (total) {
        uint32_t k = MIN(total, BUFFER_PAGES * PAGE / BLOCK_SIZE);
        int err = rw(n, 0x02, lba, k);
        if (err)
            return err;
        memcpy(out, n->buffer, k * BLOCK_SIZE);
        out += k * BLOCK_SIZE;
        lba += k;
        total -= k;
    }
    return 0;
}

static int nvme_write(struct blockdev *dev, uint32_t lba, uint32_t total, const void *buf)
{
    struct nvme *n = dev->priv;
    const uint8_t *in = buf;
    while (total) {
        uint32_t k = MIN(total, BUFFER_PAGES * PAGE / BLOCK_SIZE);
        memcpy(n->buffer, in, k * BLOCK_SIZE);
        int err = rw(n, 0x01, lba, k);
        if (err)
            return err;
        in += k * BLOCK_SIZE;
        lba += k;
        total -= k;
    }
    uint32_t flush[16] = {0x00, 1};
    submit(n, &n->io, flush);
    return 0;
}

static bool wait_ready(struct nvme *n, bool ready)
{
    uint64_t end = hal_ticks_ms() + TIMEOUT_MS;
    while ((rd(n, REG_CSTS) & 1) != (ready ? 1u : 0u))
        if (hal_ticks_ms() > end || rd(n, REG_CSTS) & 2)
            return false;
    return true;
}

static bool alloc_queue(struct queue *q, uint16_t id)
{
    uint8_t *mem = kmalloc_pages(2 * PAGE);
    if (!mem)
        return false;
    memset(mem, 0, 2 * PAGE);
    q->sq = (uint32_t *)mem;
    q->cq = (uint32_t *)(mem + PAGE);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->id = id;
    return true;
}

static bool setup(struct nvme *n)
{
    uint32_t cap_hi = rd(n, REG_CAP + 4);
    n->stride = 4u << (cap_hi & 0xf);
    wr(n, REG_CC, rd(n, REG_CC) & ~1u);
    if (!wait_ready(n, false) || !alloc_queue(&n->admin, 0) || !alloc_queue(&n->io, 1))
        return false;
    n->buffer = kmalloc_pages(BUFFER_PAGES * PAGE);
    n->prp_list = kmalloc_pages(PAGE);
    if (!n->buffer || !n->prp_list)
        return false;
    wr(n, REG_AQA, (QUEUE_SIZE - 1) | (QUEUE_SIZE - 1) << 16);
    wr(n, REG_ASQ, vmm_phys(n->admin.sq));
    wr(n, REG_ASQ + 4, 0);
    wr(n, REG_ACQ, vmm_phys(n->admin.cq));
    wr(n, REG_ACQ + 4, 0);
    wr(n, REG_CC, 1u | 6u << 16 | 4u << 20);
    if (!wait_ready(n, true))
        return false;

    uint32_t identify_ns[16] = {0x06, 1};
    set_prps(n, identify_ns, PAGE);
    identify_ns[10] = 0;
    if (submit(n, &n->admin, identify_ns) != 0)
        return false;
    uint64_t size;
    memcpy(&size, n->buffer, 8);
    uint8_t format = n->buffer[26] & 0x0f;
    uint32_t lbaf;
    memcpy(&lbaf, n->buffer + 128 + format * 4, 4);
    if (((lbaf >> 16) & 0xff) != 9 || !size)
        return false;

    uint32_t identify_ctrl[16] = {0x06, 0};
    set_prps(n, identify_ctrl, PAGE);
    identify_ctrl[10] = 1;
    if (submit(n, &n->admin, identify_ctrl) == 0) {
        memcpy(n->dev.model, n->buffer + 24, 40);
        n->dev.model[40] = 0;
        for (int i = 39; i >= 0 && n->dev.model[i] == ' '; i--)
            n->dev.model[i] = 0;
    }

    uint32_t create_cq[16] = {0x05};
    create_cq[6] = vmm_phys(n->io.cq);
    create_cq[10] = 1u | (QUEUE_SIZE - 1u) << 16;
    create_cq[11] = 1;
    uint32_t create_sq[16] = {0x01};
    create_sq[6] = vmm_phys(n->io.sq);
    create_sq[10] = 1u | (QUEUE_SIZE - 1u) << 16;
    create_sq[11] = 1u | 1u << 16;
    if (submit(n, &n->admin, create_cq) != 0 || submit(n, &n->admin, create_sq) != 0)
        return false;
    n->dev.sectors = size > 0xffffffffu ? 0xffffffffu : (uint32_t)size;
    return true;
}

static void probe(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (d->class_code != 0x01 || d->subclass != 0x08 || d->prog_if != 0x02 || count == (int)ARRAY_SIZE(controllers))
        return;
    uint64_t base;
    if (!pci_bar_base(d, 0x10, &base)) {
        klog("nvme: controller registers are out of reach");
        return;
    }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);
    struct nvme *n = &controllers[count];
    n->regs = vmm_map_phys64(base, 0x2000);
    if (!n->regs || !setup(n)) {
        klog("nvme: controller did not initialize");
        return;
    }
    ksnprintf(n->dev.name, sizeof(n->dev.name), "nvme%d", count);
    n->dev.read = nvme_read;
    n->dev.write = nvme_write;
    n->dev.priv = n;
    block_register(&n->dev);
    count++;
}

void nvme_init(void)
{
    pci_foreach_storage(probe, NULL);
}
