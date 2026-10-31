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

static inline uint32_t cpu_irq_save(void)
{
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return (uint32_t)flags;
}

static inline void cpu_irq_restore(uint32_t flags)
{
    if (flags & 0x200)
        cpu_sti();
}

static inline bool cpu_irqs_enabled(void)
{
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    return flags & 0x200;
}

/* Waits for the next interrupt when that is allowed: a device that raises one on completion wakes us at once,
   instead of the CPU spinning on its registers (each read is a trap in a virtual machine). */
static inline void cpu_wait(bool device_irq)
{
    if (device_irq && cpu_irqs_enabled())
        __asm__ volatile("hlt");
}

static inline void cpu_barrier(void)
{
#ifdef __x86_64__
    __asm__ volatile("lock; orl $0, (%%rsp)" : : : "memory");
#else
    __asm__ volatile("lock; orl $0, (%%esp)" : : : "memory");
#endif
}

static inline uint64_t cpu_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

#ifdef __x86_64__
/* the frame the 64-bit interrupt stubs build; the 32-bit names alias the full registers */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    union { uint64_t rbp, ebp; };
    union { uint64_t rdi, edi; };
    union { uint64_t rsi, esi; };
    union { uint64_t rdx, edx; };
    union { uint64_t rcx, ecx; };
    union { uint64_t rbx, ebx; };
    union { uint64_t rax, eax; };
    uint64_t vector, error;
    union { uint64_t rip, eip; };
    uint64_t cs;
    union { uint64_t rflags, eflags; };
    union { uint64_t rsp, esp; };
    uint64_t ss;
};
#else
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t vector, error;
    uint32_t eip, cs, eflags;
};
#endif

#ifdef __x86_64__
/* the 64-bit kernel runs at its physical address + KERNEL_OFFSET; the ramdisk pack waits where entry.asm put it */
#define KERNEL_OFFSET 0xb0000000u
extern uint32_t boot_pack;
#define PACK_LOCATION boot_pack
#else
#define KERNEL_OFFSET 0u
#define PACK_LOCATION 0x400000u
#endif

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
    uint32_t system_lba;
    uint32_t system_sectors;
    uint32_t id[2];
};

#define BOOT_UEFI 0x1u
#define BOOT_SYSTEM 0x2u

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
    bool pae;
    bool pat;
    bool long_mode;
    bool emulated;
    bool apic;
    uint32_t family;
};

#define MAX_CPUS 16

void cpu_ap_init(int index);
void *vmm_map_uncached(uint64_t phys, size_t size);

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
    volatile uint8_t *ecam;
};

uint32_t pci_read(const struct pci_device *d, uint8_t offset);
void pci_write(const struct pci_device *d, uint8_t offset, uint32_t value);
void pci_foreach(void (*fn)(const struct pci_device *d, void *ctx), void *ctx);
void pci_foreach_storage(void (*fn)(const struct pci_device *d, void *ctx), void *ctx);
bool pci_is_vmd(const struct pci_device *d);
bool pci_relocate_bar64(const struct pci_device *d, uint8_t offset, uint32_t *out);
bool pci_bar_base(const struct pci_device *d, uint8_t offset, uint64_t *out);
void ahci_init(void);
void ahci_flush_all(void);
struct display_mode;
bool vbe_enter(int width, int height, struct framebuffer *fb);
int vbe_modes(struct display_mode *out, int max);
void vbe_leave(void);
const struct framebuffer *uefi_framebuffer(void);
void nvme_init(void);
void e1000_init(void);
void realtek_init(void);
void irq_install(int irq, irq_handler handler);
void pit_init(uint32_t hz);
void keyboard_init(void);
int keyboard_read(void);
int keyboard_modifiers(void);
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
