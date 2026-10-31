#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/vmm.h>

#include "audio.h"
#include "io.h"

/* Intel High Definition Audio: the sound controller of nearly every PC since 2005, with a codec on its link (Realtek
   ALC..., Conexant, IDT, Cirrus, VIA). The codec's widgets are walked for analog outputs (speakers, headphones,
   line out) and the path from a converter (DAC) to each; amplifiers on the way are unmuted and the external
   amplifiers (EAPD, which Realtek codecs need for the speakers) switched on. Sound plays from a cyclic buffer the
   stream's DMA engine walks; what has been played is cleared, so a late writer gives silence, not an echo.
   Plugged-in headphones take over from the speakers. */

#define PERIODS 16
#define PERIOD_BYTES 4096
#define RING_BYTES (PERIODS * PERIOD_BYTES)
#define LEAD_BYTES 1024
#define MAX_NODES 128
#define MAX_CONN 16
#define MAX_PATH 6
#define MAX_OUTS 6
#define STREAM_TAG 1

enum {
    GCAP = 0x00,
    GCTL = 0x08,
    STATESTS = 0x0e,
    INTCTL = 0x20,
    CORBLBASE = 0x40,
    CORBUBASE = 0x44,
    CORBWP = 0x48,
    CORBRP = 0x4a,
    CORBCTL = 0x4c,
    CORBSIZE = 0x4e,
    RIRBLBASE = 0x50,
    RIRBUBASE = 0x54,
    RIRBWP = 0x58,
    RINTCNT = 0x5a,
    RIRBCTL = 0x5c,
    RIRBSTS = 0x5d,
    RIRBSIZE = 0x5e,
    ICOI = 0x60,
    ICII = 0x64,
    ICIS = 0x68,
    SD_BASE = 0x80,
    SD_CTL = 0x00,
    SD_STS = 0x03,
    SD_LPIB = 0x04,
    SD_CBL = 0x08,
    SD_LVI = 0x0c,
    SD_FMT = 0x12,
    SD_BDPL = 0x18,
    SD_BDPU = 0x1c,
};

enum { W_DAC = 0, W_ADC = 1, W_MIXER = 2, W_SELECTOR = 3, W_PIN = 4 };
enum { DEV_LINE_OUT = 0, DEV_SPEAKER = 1, DEV_HP = 2 };

struct node {
    uint32_t caps, pincaps, config, outamp, inamp;
    uint8_t conn[MAX_CONN];
    int nconn;
};

struct output {
    int pin, device;
    bool jack;
    int path[MAX_PATH], sel[MAX_PATH], len;
};

struct bdl_entry {
    uint64_t addr;
    uint32_t length, ioc;
};

static struct {
    bool probed, ready, running, immediate;
    volatile uint8_t *mmio, *sd;
    uint32_t *corb;
    volatile uint64_t *rirb;
    unsigned rirb_rp;
    int codec, afg, rate;
    uint16_t format;
    struct node nodes[MAX_NODES];
    struct output outs[MAX_OUTS];
    int nouts;
    uint8_t *ring;
    struct bdl_entry *bdl;
    uint32_t write, last;
    int32_t queued;
    uint64_t jack_checked;
    bool headphones;
    char name[48];
} hd;

static uint8_t r8(unsigned off) { return *(volatile uint8_t *)(hd.mmio + off); }
static uint16_t r16(unsigned off) { return *(volatile uint16_t *)(hd.mmio + off); }
static uint32_t r32(unsigned off) { return *(volatile uint32_t *)(hd.mmio + off); }
static void w8(unsigned off, uint8_t v) { *(volatile uint8_t *)(hd.mmio + off) = v; }
static void w16(unsigned off, uint16_t v) { *(volatile uint16_t *)(hd.mmio + off) = v; }
static void w32(unsigned off, uint32_t v) { *(volatile uint32_t *)(hd.mmio + off) = v; }

static bool wait_for(unsigned off, uint32_t mask, uint32_t want, int width, int ms)
{
    uint64_t end = hal_ticks_ms() + (uint64_t)ms;
    do {
        uint32_t v = width == 8 ? r8(off) : width == 16 ? r16(off) : r32(off);
        if ((v & mask) == want)
            return true;
    } while (hal_ticks_ms() <= end);
    return false;
}

static bool command_immediate(uint32_t cmd, uint32_t *resp)
{
    if (!wait_for(ICIS, 1, 0, 16, 10))
        return false;
    w16(ICIS, 2);
    w32(ICOI, cmd);
    w16(ICIS, 1);
    if (!wait_for(ICIS, 3, 2, 16, 10))
        return false;
    *resp = r32(ICII);
    return true;
}

static bool command(uint32_t cmd, uint32_t *resp)
{
    if (hd.immediate)
        return command_immediate(cmd, resp);
    unsigned wp = (r16(CORBWP) + 1) & 0xff;
    hd.corb[wp] = cmd;
    w16(CORBWP, (uint16_t)wp);
    uint64_t end = hal_ticks_ms() + 20;
    while (hal_ticks_ms() <= end) {
        if ((r16(RIRBWP) & 0xff) == hd.rirb_rp)
            continue;
        hd.rirb_rp = (hd.rirb_rp + 1) & 0xff;
        uint64_t e = hd.rirb[hd.rirb_rp];
        w8(RIRBSTS, 0x05);
        if (e >> 32 & 0x10)
            continue;
        *resp = (uint32_t)e;
        return true;
    }
    return false;
}

static uint32_t verb(int nid, uint32_t v, uint32_t payload)
{
    uint32_t cmd = (uint32_t)hd.codec << 28 | (uint32_t)nid << 20 | (v > 0xf ? v << 8 | (payload & 0xff) : v << 16 | (payload & 0xffff));
    uint32_t resp = 0;
    if (!command(cmd, &resp) && !hd.immediate) {
        hd.immediate = true;
        klog("hda: command ring does not answer, using immediate commands");
        command(cmd, &resp);
    }
    return resp;
}

static uint32_t param(int nid, uint8_t p)
{
    return verb(nid, 0xf00, p);
}

static bool reset_controller(void)
{
    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    w32(GCTL, r32(GCTL) & ~1u);
    if (!wait_for(GCTL, 1, 0, 32, 100))
        return false;
    hal_sleep_ms(1);
    w32(GCTL, r32(GCTL) | 1u);
    if (!wait_for(GCTL, 1, 1, 32, 100))
        return false;
    for (int i = 0; i < 20 && !(r16(STATESTS) & 0x7fff); i++)
        hal_sleep_ms(1);
    return (r16(STATESTS) & 0x7fff) != 0;
}

static bool setup_rings(void)
{
    uint8_t *page = kmalloc_pages(4096);
    if (!page)
        return false;
    memset(page, 0, 4096);
    hd.corb = (uint32_t *)page;
    hd.rirb = (volatile uint64_t *)(page + 2048);
    uint32_t phys = vmm_phys(page);
    w32(INTCTL, 0);
    w8(CORBSIZE, 0x02);
    w32(CORBLBASE, phys);
    w32(CORBUBASE, 0);
    w16(CORBRP, 0x8000);
    wait_for(CORBRP, 0x8000, 0x8000, 16, 10);
    w16(CORBRP, 0);
    wait_for(CORBRP, 0x8000, 0, 16, 10);
    w16(CORBWP, 0);
    w8(RIRBSIZE, 0x02);
    w32(RIRBLBASE, phys + 2048);
    w32(RIRBUBASE, 0);
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 1);
    hd.rirb_rp = 0;
    w8(CORBCTL, 0x02);
    w8(RIRBCTL, 0x03);
    return true;
}

static int widget_type(int nid)
{
    return (int)(hd.nodes[nid].caps >> 20 & 0xf);
}

static void read_connections(int nid)
{
    struct node *n = &hd.nodes[nid];
    uint32_t len = param(nid, 0x0e);
    bool wide = len & 0x80;
    int count = (int)(len & 0x7f), per = wide ? 2 : 4, bits = wide ? 16 : 8;
    int prev = 0;
    for (int i = 0; i < count && n->nconn < MAX_CONN; i += per) {
        uint32_t r = verb(nid, 0xf02, (uint32_t)i);
        for (int k = 0; k < per && i + k < count && n->nconn < MAX_CONN; k++) {
            uint32_t e = r >> (k * bits) & ((1u << bits) - 1);
            bool range = e & (1u << (bits - 1));
            int id = (int)(e & ((1u << (bits - 1)) - 1));
            if (range && prev)
                for (int x = prev + 1; x <= id && n->nconn < MAX_CONN; x++)
                    n->conn[n->nconn++] = (uint8_t)x;
            else if (id < MAX_NODES)
                n->conn[n->nconn++] = (uint8_t)id;
            prev = id;
        }
    }
}

/* Depth-first from a pin to a DAC; the path is stored pin first. */
static bool find_dac(struct output *o, int nid, int depth)
{
    if (nid <= 0 || nid >= MAX_NODES || depth >= MAX_PATH)
        return false;
    for (int i = 0; i < depth; i++)
        if (o->path[i] == nid)
            return false;
    o->path[depth] = nid;
    o->len = depth + 1;
    int type = widget_type(nid);
    if (type == W_DAC)
        return !(hd.nodes[nid].caps & (1u << 9));
    if (depth > 0 && type != W_MIXER && type != W_SELECTOR)
        return false;
    for (int i = 0; i < hd.nodes[nid].nconn; i++) {
        o->sel[depth] = i;
        if (find_dac(o, hd.nodes[nid].conn[i], depth + 1))
            return true;
    }
    return false;
}

static uint32_t amp_gain(uint32_t caps)
{
    return caps & 0x7f;
}

static void power_up(int nid)
{
    verb(nid, 0x705, 0);
}

static void program_path(const struct output *o)
{
    for (int i = 0; i < o->len; i++) {
        int nid = o->path[i];
        struct node *n = &hd.nodes[nid];
        int type = widget_type(nid);
        power_up(nid);
        bool last = i == o->len - 1;
        if (!last && n->nconn > 1 && type != W_MIXER)
            verb(nid, 0x701, (uint32_t)o->sel[i]);
        if (!last && (n->caps & 2) && (type == W_MIXER || type == W_SELECTOR || type == W_PIN))
            verb(nid, 0x3, 0x7000u | (uint32_t)o->sel[i] << 8 | amp_gain(n->inamp));
        if (n->caps & 4)
            verb(nid, 0x3, 0xb000u | amp_gain(n->outamp));
        if (type == W_DAC) {
            verb(nid, 0x706, STREAM_TAG << 4);
            verb(nid, 0x2, hd.format);
        }
    }
}

static void set_pin(const struct output *o, bool on)
{
    struct node *n = &hd.nodes[o->pin];
    verb(o->pin, 0x707, on ? 0x40u | (o->device == DEV_HP ? 0x80u : 0) : 0);
    if (n->pincaps & (1u << 16))
        verb(o->pin, 0x70c, on ? 0x02 : 0);
}

static bool jack_present(const struct output *o)
{
    if (!(hd.nodes[o->pin].pincaps & (1u << 2)))
        return false;
    verb(o->pin, 0x709, 0);
    return (verb(o->pin, 0xf09, 0) >> 31) != 0;
}

/* Headphones in: speakers off; out: speakers back on. */
static void check_jacks(void)
{
    bool phones = false;
    for (int i = 0; i < hd.nouts; i++)
        if (hd.outs[i].device == DEV_HP && hd.outs[i].jack && jack_present(&hd.outs[i]))
            phones = true;
    if (phones == hd.headphones && hd.jack_checked)
        return;
    hd.headphones = phones;
    for (int i = 0; i < hd.nouts; i++)
        if (hd.outs[i].device == DEV_SPEAKER)
            set_pin(&hd.outs[i], !phones);
}

static bool pick_outputs(bool any)
{
    hd.nouts = 0;
    for (int nid = 1; nid < MAX_NODES && hd.nouts < MAX_OUTS; nid++) {
        struct node *n = &hd.nodes[nid];
        if (widget_type(nid) != W_PIN || !(n->pincaps & (1u << 4)) || (n->pincaps & (1u << 7)) ||
            (n->pincaps & (1u << 24)) || (n->caps & (1u << 9)))
            continue;
        int connectivity = (int)(n->config >> 30), device = (int)(n->config >> 20 & 0xf);
        if (!any && (connectivity == 1 || device > DEV_HP))
            continue;
        struct output o = {nid, any ? DEV_LINE_OUT : device, connectivity != 2, {0}, {0}, 0};
        if (find_dac(&o, nid, 0))
            hd.outs[hd.nouts++] = o;
    }
    return hd.nouts > 0;
}

static bool setup_codec(int codec)
{
    hd.codec = codec;
    uint32_t sub = param(0, 0x04);
    int start = (int)(sub >> 16 & 0xff), count = (int)(sub & 0xff);
    hd.afg = 0;
    for (int nid = start; nid < start + count && !hd.afg; nid++)
        if ((param(nid, 0x05) & 0xff) == 1)
            hd.afg = nid;
    if (!hd.afg)
        return false;
    verb(hd.afg, 0x7ff, 0);
    hal_sleep_ms(10);
    power_up(hd.afg);
    uint32_t vendor = param(0, 0x00);
    uint32_t afg_out = param(hd.afg, 0x12), afg_in = param(hd.afg, 0x0d), rates = param(hd.afg, 0x0a);
    memset(hd.nodes, 0, sizeof(hd.nodes));
    sub = param(hd.afg, 0x04);
    start = (int)(sub >> 16 & 0xff);
    count = (int)(sub & 0xff);
    for (int nid = start; nid < start + count && nid < MAX_NODES; nid++) {
        struct node *n = &hd.nodes[nid];
        n->caps = param(nid, 0x09);
        bool own = n->caps & (1u << 3);
        n->outamp = own ? param(nid, 0x12) : afg_out;
        n->inamp = own ? param(nid, 0x0d) : afg_in;
        if (widget_type(nid) == W_PIN) {
            n->pincaps = param(nid, 0x0c);
            n->config = verb(nid, 0xf1c, 0);
        }
        if (n->caps & (1u << 8))
            read_connections(nid);
    }
    if (!pick_outputs(false) && !pick_outputs(true))
        return false;
    hd.rate = hd.rate == 44100 && (rates & (1u << 5)) ? 44100 : 48000;
    hd.format = hd.rate == 44100 ? 0x4011 : 0x0011;
    for (int i = 0; i < hd.nouts; i++) {
        program_path(&hd.outs[i]);
        set_pin(&hd.outs[i], true);
    }
    hd.jack_checked = 0;
    check_jacks();
    hd.jack_checked = hal_ticks_ms();
    const char *maker = (vendor >> 16) == 0x10ec ? "Realtek" : (vendor >> 16) == 0x14f1 ? "Conexant"
                      : (vendor >> 16) == 0x111d ? "IDT" : (vendor >> 16) == 0x1106 ? "VIA"
                      : (vendor >> 16) == 0x1013 ? "Cirrus" : (vendor >> 16) == 0x1af4 ? "QEMU" : NULL;
    if (maker && (vendor >> 16) == 0x10ec)
        ksnprintf(hd.name, sizeof(hd.name), "Realtek ALC%x", (unsigned)(vendor & 0xffff));
    else if (maker)
        ksnprintf(hd.name, sizeof(hd.name), "%s HD Audio", maker);
    else
        ksnprintf(hd.name, sizeof(hd.name), "HD Audio (%04x:%04x)", (unsigned)(vendor >> 16), (unsigned)(vendor & 0xffff));
    klog("hda: codec %d %08x (%s), %d output%s, %d Hz", codec, (unsigned)vendor, hd.name, hd.nouts,
         hd.nouts == 1 ? "" : "s", hd.rate);
    for (int i = 0; i < hd.nouts; i++)
        klog("hda:   %s pin %d via %d nodes to DAC %d",
             hd.outs[i].device == DEV_SPEAKER ? "speaker" : hd.outs[i].device == DEV_HP ? "headphones" : "line out",
             hd.outs[i].pin, hd.outs[i].len, hd.outs[i].path[hd.outs[i].len - 1]);
    return true;
}

static bool setup_stream(void)
{
    unsigned iss = r16(GCAP) >> 8 & 0xf, oss = r16(GCAP) >> 12 & 0xf;
    if (!oss)
        return false;
    hd.sd = hd.mmio + SD_BASE + iss * 0x20;
    unsigned sd = SD_BASE + iss * 0x20;
    w8(sd + SD_CTL, 1);
    wait_for(sd + SD_CTL, 1, 1, 8, 10);
    w8(sd + SD_CTL, 0);
    wait_for(sd + SD_CTL, 1, 0, 8, 10);
    hd.ring = kmalloc_pages(RING_BYTES);
    hd.bdl = kmalloc_pages(4096);
    if (!hd.ring || !hd.bdl)
        return false;
    memset(hd.ring, 0, RING_BYTES);
    for (int i = 0; i < PERIODS; i++)
        hd.bdl[i] = (struct bdl_entry){vmm_phys(hd.ring + i * PERIOD_BYTES), PERIOD_BYTES, 0};
    w32(sd + SD_CBL, RING_BYTES);
    w16(sd + SD_LVI, PERIODS - 1);
    w16(sd + SD_FMT, hd.format);
    w32(sd + SD_BDPL, vmm_phys(hd.bdl));
    w32(sd + SD_BDPU, 0);
    w8(sd + SD_CTL + 2, STREAM_TAG << 4);
    w8(sd + SD_STS, 0x1c);
    return true;
}

struct candidate {
    struct pci_device dev[4];
    int count;
};

static void collect(const struct pci_device *d, void *ctx)
{
    struct candidate *c = ctx;
    bool hda = d->class_code == 0x04 && d->subclass == 0x03;
    bool dsp = d->class_code == 0x04 && d->subclass == 0x01 && (d->vendor == 0x8086 || d->vendor == 0x1022) &&
               !(pci_read(d, 0x10) & 1);
    if ((hda || dsp) && c->count < (int)ARRAY_SIZE(c->dev))
        c->dev[c->count++] = *d;
}

static bool try_controller(const struct pci_device *d, int rate)
{
    uint64_t base;
    if (!pci_bar_base(d, 0x10, &base))
        return false;
    pci_write(d, 0x04, (pci_read(d, 0x04) | 0x06) & ~0x400u);
    if (d->vendor == 0x8086)
        pci_write(d, 0x44, pci_read(d, 0x44) & ~7u);
    hd.mmio = vmm_map_phys64(base, 0x4000);
    hd.immediate = false;
    if (!hd.mmio || !reset_controller() || !setup_rings())
        return false;
    uint16_t codecs = r16(STATESTS) & 0x7fff;
    hd.rate = rate;
    for (int c = 0; c < 15; c++)
        if ((codecs & (1u << c)) && setup_codec(c) && setup_stream()) {
            klog("hda: controller %04x:%04x at %02x:%02x.%u", d->vendor, d->device, d->bus, d->dev, d->fn);
            return true;
        }
    return false;
}

bool hda_probe(int rate)
{
    if (hd.probed)
        return hd.ready;
    hd.probed = true;
    struct candidate c = {.count = 0};
    pci_foreach(collect, &c);
    for (int i = 0; i < c.count && !hd.ready; i++)
        hd.ready = try_controller(&c.dev[i], rate);
    return hd.ready;
}

static void stop(void)
{
    unsigned sd = (unsigned)(hd.sd - hd.mmio);
    w8(sd + SD_CTL, r8(sd + SD_CTL) & ~2u);
    hd.running = false;
}

int hda_open(int rate)
{
    UNUSED(rate);
    if (hd.running)
        stop();
    memset(hd.ring, 0, RING_BYTES);
    hd.write = hd.last = 0;
    hd.queued = 0;
    return hd.rate;
}

/* Bytes the DMA engine has played since the last look are cleared and no longer count as queued. */
static void advance(void)
{
    if (!hd.running)
        return;
    uint32_t pos = *(volatile uint32_t *)(hd.sd + SD_LPIB) % RING_BYTES & ~3u;
    uint32_t played = (pos + RING_BYTES - hd.last) % RING_BYTES;
    for (uint32_t done = 0; done < played;) {
        uint32_t at = (hd.last + done) % RING_BYTES, n = MIN(played - done, RING_BYTES - at);
        memset(hd.ring + at, 0, n);
        done += n;
    }
    hd.last = pos;
    hd.queued -= (int32_t)played;
    if (hd.queued < 0) {
        hd.write = (pos + LEAD_BYTES) % RING_BYTES;
        hd.queued = LEAD_BYTES;
    }
}

int hda_space(void)
{
    advance();
    if (hal_ticks_ms() - hd.jack_checked > 500) {
        check_jacks();
        hd.jack_checked = hal_ticks_ms();
    }
    return MAX(0, (RING_BYTES - PERIOD_BYTES - hd.queued) / 4);
}

int hda_write(const int16_t *frames, int count)
{
    int n = MIN(count, hda_space());
    uint32_t bytes = (uint32_t)n * 4;
    const uint8_t *src = (const uint8_t *)frames;
    for (uint32_t done = 0; done < bytes;) {
        uint32_t at = (hd.write + done) % RING_BYTES, k = MIN(bytes - done, RING_BYTES - at);
        memcpy(hd.ring + at, src + done, k);
        done += k;
    }
    hd.write = (hd.write + bytes) % RING_BYTES;
    hd.queued += (int32_t)bytes;
    if (!hd.running && n > 0) {
        unsigned sd = (unsigned)(hd.sd - hd.mmio);
        w8(sd + SD_STS, 0x1c);
        w8(sd + SD_CTL, r8(sd + SD_CTL) | 2u);
        hd.running = true;
    }
    return n;
}

void hda_close(void)
{
    if (hd.running)
        stop();
    memset(hd.ring, 0, RING_BYTES);
}

const char *hda_name(void)
{
    return hd.ready ? hd.name : NULL;
}
