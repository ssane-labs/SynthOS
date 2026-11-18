/* ACPI power control: soft-off (S5) and reset through the FADT. */
#include "io.h"
#include <synth/vmm.h>
#include <synth/string.h>

struct gas {
    uint8_t space, width, offset, size;
    uint64_t addr;
} __attribute__((packed));

static uint32_t pm1a, pm1b, smi_cmd;
static uint8_t acpi_on, slp_a, slp_b, reset_value;
static struct gas reset_reg;
static bool have_s5, have_reset;
static volatile uint8_t *reset_mmio;

static const uint8_t *map(uint64_t phys, size_t size)
{
    return phys ? vmm_map_phys64(phys, size) : NULL;
}

static uint64_t find_rsdp(void)
{
    const struct x86_bootinfo *bi = x86_boot_info();
    if (bi->flags & BOOT_ACPI)
        return (uint64_t)bi->acpi_rsdp_high << 32 | bi->acpi_rsdp;
    if (bi->flags & BOOT_UEFI)
        return 0;
    const uint8_t *low = vmm_map_phys64(0, 0x100000);
    if (!low)
        return 0;
    uint32_t ebda = (uint32_t)(*(const uint16_t *)(low + 0x40e)) << 4;
    uint32_t ranges[2][2] = {{ebda, ebda + 1024}, {0xe0000, 0x100000}};
    for (int r = 0; r < 2; r++) {
        if (r == 0 && ebda < 0x80000)
            continue;
        for (uint32_t a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16) {
            if (memcmp(low + a, "RSD PTR ", 8))
                continue;
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++)
                sum += low[a + i];
            if (!sum)
                return a;
        }
    }
    return 0;
}

static const uint8_t *table(uint64_t phys)
{
    const uint8_t *h = map(phys, 36);
    if (!h)
        return NULL;
    uint32_t len = *(const uint32_t *)(h + 4);
    if (len < 36 || len > (16u << 20))
        return NULL;
    return map(phys, len);
}

static const uint8_t *find_fadt(uint64_t rsdp_phys)
{
    const uint8_t *rsdp = map(rsdp_phys, 36);
    if (!rsdp)
        return NULL;
    uint64_t root = *(const uint32_t *)(rsdp + 16);
    int entry = 4;
    if (rsdp[15] >= 2 && *(const uint64_t *)(rsdp + 24)) {
        uint64_t x = *(const uint64_t *)(rsdp + 24);
        if (sizeof(void *) == 8 || !(x >> 32)) {
            root = x;
            entry = 8;
        }
    }
    const uint8_t *rt = table(root);
    if (!rt)
        return NULL;
    uint32_t len = *(const uint32_t *)(rt + 4);
    for (uint32_t off = 36; off + entry <= len; off += entry) {
        uint64_t p = entry == 8 ? *(const uint64_t *)(rt + off) : *(const uint32_t *)(rt + off);
        const uint8_t *h = map(p, 36);
        if (h && !memcmp(h, "FACP", 4))
            return table(p);
    }
    return NULL;
}

static int aml_int(const uint8_t *p, uint8_t *out)
{
    if (*p == 0x0a) {
        *out = p[1];
        return 2;
    }
    if (*p == 0x00 || *p == 0x01) {
        *out = *p;
        return 1;
    }
    if (*p == 0xff) {
        *out = 0xff;
        return 1;
    }
    return 0;
}

static void find_s5(const uint8_t *dsdt)
{
    uint32_t len = *(const uint32_t *)(dsdt + 4);
    for (uint32_t i = 36; i + 16 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) || dsdt[i + 4] != 0x12)
            continue;
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\')))
            continue;
        const uint8_t *p = dsdt + i + 5;
        p += ((*p & 0xc0) >> 6) + 2; /* PkgLength, NumElements */
        int n = aml_int(p, &slp_a);
        if (!n)
            return;
        p += n;
        if (!aml_int(p, &slp_b))
            slp_b = slp_a;
        have_s5 = true;
        return;
    }
}

void acpi_init(void)
{
    uint64_t rsdp = find_rsdp();
    const uint8_t *f = rsdp ? find_fadt(rsdp) : NULL;
    if (!f)
        return;
    uint32_t len = *(const uint32_t *)(f + 4);
    pm1a = *(const uint32_t *)(f + 64);
    pm1b = *(const uint32_t *)(f + 68);
    smi_cmd = *(const uint32_t *)(f + 48);
    acpi_on = f[52];
    uint64_t dsdt = *(const uint32_t *)(f + 40);
    if (len >= 148 && *(const uint64_t *)(f + 140) && (sizeof(void *) == 8 || !(*(const uint64_t *)(f + 140) >> 32)))
        dsdt = *(const uint64_t *)(f + 140);
    if (len >= 129 && (*(const uint32_t *)(f + 112) & (1u << 10))) {
        memcpy(&reset_reg, f + 116, sizeof(reset_reg));
        reset_value = f[128];
        if (reset_reg.space == 0)
            reset_mmio = (volatile uint8_t *)map(reset_reg.addr, 1);
        have_reset = reset_reg.addr && (reset_reg.space == 1 || reset_mmio);
    }
    const uint8_t *d = dsdt ? table(dsdt) : NULL;
    if (d && !memcmp(d, "DSDT", 4))
        find_s5(d);
}

void acpi_poweroff(void)
{
    if (!have_s5 || !pm1a)
        return;
    if (smi_cmd && acpi_on && !(inw((uint16_t)pm1a) & 1)) {
        outb((uint16_t)smi_cmd, acpi_on);
        for (int i = 0; i < 300000 && !(inw((uint16_t)pm1a) & 1); i++)
            io_wait();
    }
    outw((uint16_t)pm1a, (uint16_t)((inw((uint16_t)pm1a) & 0x203) | (slp_a & 7) << 10 | 1u << 13));
    if (pm1b)
        outw((uint16_t)pm1b, (uint16_t)((inw((uint16_t)pm1b) & 0x203) | (slp_b & 7) << 10 | 1u << 13));
    for (int i = 0; i < 1000000; i++)
        io_wait();
}

void acpi_reset(void)
{
    if (have_reset) {
        if (reset_reg.space == 1) {
            outb((uint16_t)reset_reg.addr, reset_value);
        } else {
            *reset_mmio = reset_value;
        }
        for (int i = 0; i < 100000; i++)
            io_wait();
    }
    outb(0xcf9, 0x02);
    io_wait();
    outb(0xcf9, 0x06);
    for (int i = 0; i < 100000; i++)
        io_wait();
}
