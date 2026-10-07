#ifndef SYNTH_X86_IO_H
#define SYNTH_X86_IO_H

#include <synth/types.h>

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline void cpu_cli(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static inline void cpu_sti(void)
{
    __asm__ volatile("sti" ::: "memory");
}

static inline void cpu_halt(void)
{
    __asm__ volatile("sti; hlt" ::: "memory");
}

static inline uint64_t cpu_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t vector, error;
    uint32_t eip, cs, eflags;
};

struct x86_bootinfo {
    uint32_t magic;
    uint32_t drive;
    uint32_t mmap_addr;
    uint32_t mmap_count;
    uint32_t kernel_sectors;
    uint32_t kernel_lba;
    uint32_t flags;
    uint32_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint8_t fb_shift[4];
};

#define BOOT_UEFI 0x1u

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} __attribute__((packed));

typedef void (*irq_handler)(struct regs *r);

struct cpu_info {
    bool cpuid;
    bool tsc;
    bool fpu;
    bool sse;
    uint32_t family;
};

void cpu_init(void);
const struct cpu_info *cpu_info(void);
uint16_t pit_counter(void);
void pit_bios_mode(bool bios);
void pit_advance_ms(uint32_t ms);
void pic_program(uint8_t master, uint8_t slave);

struct bios_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint16_t ds, es;
    uint32_t eflags;
    uint32_t intno;
    uint32_t ticks_before, ticks_after;
};

#define BIOS_SCRATCH 0x1f40u
#define BIOS_BOUNCE 0x20000u
#define BIOS_BOUNCE_SIZE 0x8000u

void bios_init(void);
bool bios_available(void);
bool bios_call(uint8_t intno, struct bios_regs *r);
void biosdisk_init(uint8_t drive);

struct framebuffer;

struct pci_device {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
};

uint32_t pci_read(const struct pci_device *d, uint8_t offset);
void pci_write(const struct pci_device *d, uint8_t offset, uint32_t value);
void pci_foreach(void (*fn)(const struct pci_device *d, void *ctx), void *ctx);
bool pci_relocate_bar64(const struct pci_device *d, uint8_t offset, uint32_t *out);
void ahci_init(void);
bool vbe_enter(struct framebuffer *fb);
void vbe_leave(void);
void nvme_init(void);
void irq_install(int irq, irq_handler handler);
void pit_init(uint32_t hz);
void keyboard_init(void);
int keyboard_read(void);
void i8042_drain(void);
void mouse_init(void);
void mouse_byte(uint8_t data);
void serial_init(void);
bool serial_ready(void);
int serial_getc(void);
void serial_write(const char *s, size_t n);
void vga_init(void);
void vga_load_font(int codepage);
struct display *vga_display(void);
struct display *serial_display(void);
int serial_key(void);
const struct x86_bootinfo *x86_boot_info(void);
void ata_init(void);
void *paging_init(uint32_t table_start, uint32_t table_limit, uint32_t start, uint32_t end, size_t *size);

#endif
