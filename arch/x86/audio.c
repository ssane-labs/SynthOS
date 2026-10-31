#include <synth/error.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "audio.h"
#include "io.h"

/* Sound output: Intel HD Audio (hda.c) where there is one, else Intel AC'97 (ICH, older PCs and VMs) with 16-bit
   stereo PCM through a ring of 32 DMA buffers. */

#define BUFFERS 32
#define BUFFER_FRAMES 512
#define NAM_RESET 0x00
#define NAM_MASTER 0x02
#define NAM_PCM 0x18
#define NAM_EXT_ID 0x28
#define NAM_EXT_CTRL 0x2a
#define NAM_FRONT_RATE 0x2c
#define PO_BDBAR 0x10
#define PO_CIV 0x14
#define PO_LVI 0x15
#define PO_SR 0x16
#define PO_CR 0x1b
#define GLOB_CNT 0x2c
#define GLOB_STA 0x30

struct bd {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
};

static struct {
    bool probed, ready, running;
    uint16_t nam, nabm;
    int rate;
    struct bd *list;
    int16_t *data;
    int fill, fill_frames;
} ac;

static void found(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (ac.nam || d->class_code != 0x04 || d->subclass != 0x01)
        return;
    uint32_t bar0 = pci_read(d, 0x10), bar1 = pci_read(d, 0x14);
    if (!(bar0 & 1) || !(bar1 & 1))
        return;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x05);
    ac.nam = (uint16_t)(bar0 & ~3u);
    ac.nabm = (uint16_t)(bar1 & ~3u);
}

static bool probe(int rate)
{
    ac.probed = true;
    pci_foreach(found, NULL);
    if (!ac.nam)
        return false;
    outl(ac.nabm + GLOB_CNT, 0x02);
    for (int i = 0; i < 100 && !(inl(ac.nabm + GLOB_STA) & 0x100); i++)
        hal_sleep_ms(1);
    outw(ac.nam + NAM_RESET, 1);
    hal_sleep_ms(2);
    outw(ac.nam + NAM_MASTER, 0x0000);
    outw(ac.nam + NAM_PCM, 0x0808);
    ac.rate = 48000;
    if (inw(ac.nam + NAM_EXT_ID) & 1) {
        outw(ac.nam + NAM_EXT_CTRL, (uint16_t)(inw(ac.nam + NAM_EXT_CTRL) | 1));
        outw(ac.nam + NAM_FRONT_RATE, (uint16_t)rate);
        ac.rate = inw(ac.nam + NAM_FRONT_RATE);
        if (ac.rate < 8000)
            ac.rate = 48000;
    }
    ac.list = kmalloc_pages(4096);
    ac.data = kmalloc_pages(BUFFERS * BUFFER_FRAMES * 4);
    if (!ac.list || !ac.data)
        return false;
    memset(ac.data, 0, BUFFERS * BUFFER_FRAMES * 4);
    for (int i = 0; i < BUFFERS; i++)
        ac.list[i] = (struct bd){vmm_phys(ac.data + i * BUFFER_FRAMES * 2), BUFFER_FRAMES * 2, 0};
    outb(ac.nabm + PO_CR, 0x02);
    hal_sleep_ms(1);
    outb(ac.nabm + PO_CR, 0);
    outl(ac.nabm + PO_BDBAR, vmm_phys(ac.list));
    ac.ready = true;
    klog("audio: AC'97 at io %x/%x, %d Hz", ac.nam, ac.nabm, ac.rate);
    return true;
}

static bool use_hda(int rate)
{
    static bool tried, found;
    if (!tried) {
        tried = true;
        found = hda_probe(rate);
    }
    return found;
}

int hal_audio_open(int rate)
{
    if (use_hda(rate))
        return hda_open(rate);
    if (!ac.probed && !probe(rate))
        return E_NODEV;
    if (!ac.ready)
        return E_NODEV;
    ac.running = false;
    ac.fill_frames = 0;
    return ac.rate;
}

static int civ(void)
{
    return inb(ac.nabm + PO_CIV) % BUFFERS;
}

static int queued(void)
{
    if (!ac.running)
        return 0;
    if (inw(ac.nabm + PO_SR) & 1)
        return 0;
    int lvi = inb(ac.nabm + PO_LVI) % BUFFERS;
    return (lvi - civ() + BUFFERS) % BUFFERS + 1;
}

int hal_audio_space(void)
{
    if (use_hda(44100))
        return hda_space();
    if (!ac.ready)
        return 0;
    int q = queued();
    return MAX(0, (BUFFERS - 2 - q) * BUFFER_FRAMES - ac.fill_frames);
}

static void submit(void)
{
    outw(ac.nabm + PO_SR, 0x1c);
    outb(ac.nabm + PO_LVI, (uint8_t)ac.fill);
    ac.fill = (ac.fill + 1) % BUFFERS;
    if (!ac.running) {
        outb(ac.nabm + PO_CR, 0x01);
        ac.running = true;
    }
}

static int next_buffer(void)
{
    if (!ac.running)
        return civ();
    if (inw(ac.nabm + PO_SR) & 1)
        return (civ() + 1) % BUFFERS;
    return ac.fill;
}

int hal_audio_write(const int16_t *frames, int count)
{
    if (use_hda(44100))
        return hda_write(frames, count);
    if (!ac.ready)
        return E_NODEV;
    int done = 0;
    while (done < count) {
        if (!ac.fill_frames) {
            if (ac.running && !(inw(ac.nabm + PO_SR) & 1) && queued() >= BUFFERS - 2)
                break;
            ac.fill = next_buffer();
        }
        int n = MIN(count - done, BUFFER_FRAMES - ac.fill_frames);
        memcpy(ac.data + (ac.fill * BUFFER_FRAMES + ac.fill_frames) * 2, frames + done * 2, (size_t)n * 4);
        ac.fill_frames += n;
        done += n;
        if (ac.fill_frames == BUFFER_FRAMES) {
            ac.fill_frames = 0;
            submit();
        }
    }
    return done;
}

void hal_audio_close(void)
{
    if (use_hda(44100)) {
        hda_close();
        return;
    }
    if (!ac.ready)
        return;
    outb(ac.nabm + PO_CR, 0);
    ac.running = false;
    ac.fill_frames = 0;
}

const char *hal_audio_name(void)
{
    if (use_hda(44100))
        return hda_name();
    if (!ac.probed)
        probe(44100);
    return ac.ready ? "Intel AC'97" : NULL;
}
