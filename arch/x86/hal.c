#include <synth/block.h>
#include <synth/console.h>
#include <synth/log.h>
#include <synth/hal.h>
#include <synth/keys.h>
#include <synth/string.h>

#include "io.h"

#define BOOT_MAGIC 0x5359424fu
#define KERNEL_MAGIC 0x4b4e5953u
#define DISK_DATA_LBA 8192u

extern const char synth_ramdisk_start[];
extern const char synth_ramdisk_end[];
extern char kernel_end[];

static const struct x86_bootinfo *bootinfo;
static struct display *display;
static char machine[49];

const struct x86_bootinfo *x86_boot_info(void)
{
    return bootinfo;
}

struct display *hal_display(void)
{
    return display;
}

int hal_key_read(int timeout_ms)
{
    uint64_t end = timeout_ms > 0 ? hal_ticks_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        int key = keyboard_read();
        if (key == KEY_NONE)
            key = serial_key();
        if (key != KEY_NONE)
            return key;
        if (timeout_ms == 0 || (timeout_ms > 0 && hal_ticks_ms() >= end))
            return KEY_NONE;
        cpu_halt();
    }
}

uint32_t hal_entropy(void)
{
    static uint64_t state;
    uint64_t noise = cpu_info()->tsc ? cpu_rdtsc() : hal_ticks_ms() * 2654435761u + pit_counter();
    if (!state)
        state = noise ^ 0x9e3779b97f4a7c15ull;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    state += noise;
    return (uint32_t)(state >> 16);
}

void hal_set_codepage(int codepage)
{
    UNUSED(codepage);
}

static bool is_boot_disk(struct blockdev *dev)
{
    static uint8_t sector[512];
    if (block_read(dev, 0, 1, sector) != 0 || sector[510] != 0x55 || sector[511] != 0xaa)
        return false;
    bool marker = false;
    for (int i = 0; i + 8 <= 446 && !marker; i++)
        marker = memcmp(sector + i, "SynthOS:", 8) == 0;
    if (!marker || block_read(dev, bootinfo->kernel_lba, 1, sector) != 0)
        return false;
    uint32_t magic;
    memcpy(&magic, sector + 8, 4);
    return magic == KERNEL_MAGIC;
}

void hal_fs_init(void)
{
    ata_init();
    for (struct blockdev *dev = block_list(); dev; dev = dev->next) {
        if (dev->parent || !bootinfo->kernel_lba || !is_boot_disk(dev))
            continue;
        if (bootinfo->kernel_lba + bootinfo->kernel_sectors > DISK_DATA_LBA) {
            klog("disk: kernel image overlaps the data area, persistence disabled");
            break;
        }
        if (dev->sectors > DISK_DATA_LBA + 256)
            block_add_partition(dev, "data0", DISK_DATA_LBA, dev->sectors - DISK_DATA_LBA);
        break;
    }
}

const char *hal_arch(void)
{
    uint32_t family = cpu_info()->family;
    return family >= 6 ? "x86 (i686)" : family == 5 ? "x86 (i586)" : "x86 (i486)";
}

static void cpuid(uint32_t leaf, uint32_t out[4])
{
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3]) : "a"(leaf), "c"(0));
}

const char *hal_machine(void)
{
    if (!machine[0] && !cpu_info()->cpuid)
        str_copy(machine, "IBM PC compatible (486)", sizeof(machine));
    if (!machine[0]) {
        uint32_t r[4];
        cpuid(0x80000000u, r);
        if (r[0] >= 0x80000004u) {
            for (uint32_t i = 0; i < 3; i++) {
                cpuid(0x80000002u + i, r);
                memcpy(machine + i * 16, r, 16);
            }
            machine[48] = 0;
            char *s = machine;
            while (*s == ' ')
                s++;
            memmove(machine, s, strlen(s) + 1);
        } else {
            str_copy(machine, "IBM PC compatible", sizeof(machine));
        }
    }
    return machine;
}

void hal_reboot(void)
{
    cpu_cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
    outb(0x64, 0xfe);
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int $3" : : "m"(null_idt));
    for (;;)
        __asm__ volatile("hlt");
}

void hal_poweroff(void)
{
    outw(0x604, 0x2000);
    outw(0xb004, 0x2000);
    outw(0x4004, 0x3400);
    con_reset();
    con_clear();
    con_role(ROLE_ACCENT);
    kprintf("\n  It is now safe to turn off your computer.\n");
    con_flush();
    cpu_cli();
    for (;;)
        __asm__ volatile("hlt");
}

static void setup_heap(struct boot_info *bi)
{
    uintptr_t start = ALIGN_UP((uintptr_t)kernel_end, 4096);
    uint64_t best_end = 0, total = 0;
    const struct e820_entry *map = (const struct e820_entry *)bootinfo->mmap_addr;
    for (uint32_t i = 0; i < bootinfo->mmap_count; i++) {
        if (map[i].type != 1)
            continue;
        total += map[i].length;
        uint64_t base = map[i].base, end = base + map[i].length;
        if (base <= start && end > start && end > best_end)
            best_end = end;
    }
    if (best_end == 0)
        best_end = start + (16u << 20);
    if (best_end > 0xf0000000ull)
        best_end = 0xf0000000ull;
    bi->heap = paging_init((uint32_t)start, (uint32_t)best_end, &bi->heap_size);
    if (!bi->heap) {
        bi->heap = (void *)start;
        bi->heap_size = (size_t)(best_end - start);
    }
    bi->mem_total = (size_t)MIN(total, 0xffffffffull);
}

void x86_main(uint32_t magic, const struct x86_bootinfo *info)
{
    static struct boot_info bi;
    static const struct x86_bootinfo fallback = {BOOT_MAGIC, 0x80, 0, 0, 0, 0};
    bootinfo = magic == BOOT_MAGIC ? info : &fallback;

    cpu_init();
    pit_init(1000);
    keyboard_init();
    serial_init();
#ifdef CONFIG_SERIAL_CONSOLE
    display = serial_display();
#else
    vga_init();
    display = vga_display();
#endif
    cpu_sti();

    setup_heap(&bi);
    bi.initrd = synth_ramdisk_start;
    bi.initrd_size = (size_t)(synth_ramdisk_end - synth_ramdisk_start);
#ifdef CONFIG_CMDLINE
    bi.cmdline = CONFIG_CMDLINE;
#else
    bi.cmdline = "";
#endif
    kmain(&bi);
}
