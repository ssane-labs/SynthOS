/* Sleep in S3, suspend to RAM. Everything but the memory loses power: the firmware starts the boot processor again
   in real mode at the waking vector of the FACS, which is the processors' start-up code (smpboot.asm) pointed at
   acpi_wake_entry. That returns from acpi_sleep_lowlevel a second time, on the stack it slept on; then the processor,
   the interrupt controllers, the timer, PCI configuration, the drivers' devices and the other processors are set up
   again, as at boot. A device whose driver cannot do that (resume_register with NULL) keeps the machine out of S3. */
#include <synth/arch_win32.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/time.h>

#include "io.h"

#define MAX_RESUME 24
#define MAX_PCI 96

struct resume {
    const char *name;
    bool (*fn)(void);
};

static struct resume resumes[MAX_RESUME];
static int resume_count;

void resume_register(const char *name, bool (*fn)(void))
{
    for (int i = 0; i < resume_count; i++)
        if (!strcmp(resumes[i].name, name)) {
            if (!fn)
                resumes[i].fn = NULL;
            return;
        }
    if (resume_count < MAX_RESUME)
        resumes[resume_count++] = (struct resume){name, fn};
}

/* ---- the processor's state across the sleep: callee-saved registers and flags on the stack, the stack pointer
   here; acpi_sleep_lowlevel(enter) returns 0 when enter returned (no sleep), 1 when the machine woke */

uintptr_t sleep_saved_sp __attribute__((used));
int acpi_sleep_lowlevel(void (*enter)(void));
void acpi_wake_entry(int index);

#ifdef __x86_64__
__asm__(".text\n"
        ".globl acpi_sleep_lowlevel\n"
        "acpi_sleep_lowlevel:\n"
        "    push %rbp\n    push %rbx\n    push %r12\n    push %r13\n    push %r14\n    push %r15\n    pushfq\n"
        "    mov %rsp, sleep_saved_sp(%rip)\n"
        "    call *%rdi\n"
        "    popfq\n    pop %r15\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n    pop %rbp\n"
        "    xor %eax, %eax\n    ret\n"
        ".globl acpi_wake_entry\n"
        "acpi_wake_entry:\n"
        "    mov sleep_saved_sp(%rip), %rsp\n"
        "    popfq\n    pop %r15\n    pop %r14\n    pop %r13\n    pop %r12\n    pop %rbx\n    pop %rbp\n"
        "    mov $1, %eax\n    ret\n");
#else
__asm__(".text\n"
        ".globl acpi_sleep_lowlevel\n"
        "acpi_sleep_lowlevel:\n"
        "    push %ebp\n    push %ebx\n    push %esi\n    push %edi\n    pushfl\n"
        "    mov %esp, sleep_saved_sp\n"
        "    mov 24(%esp), %eax\n"
        "    call *%eax\n"
        "    popfl\n    pop %edi\n    pop %esi\n    pop %ebx\n    pop %ebp\n"
        "    xor %eax, %eax\n    ret\n"
        ".globl acpi_wake_entry\n"
        "acpi_wake_entry:\n"
        "    mov sleep_saved_sp, %esp\n"
        "    popfl\n    pop %edi\n    pop %esi\n    pop %ebx\n    pop %ebp\n"
        "    mov $1, %eax\n    ret\n");
#endif

/* ---- PCI configuration: the firmware gives the devices back without their addresses (BARs) and with memory and
   bus-master access off; what the kernel set up is written back, bridges first (they come first on the bus) */

struct pci_saved {
    struct pci_device d;
    uint32_t header[16];
};

static struct pci_saved *pci_saved;
static int pci_count;

static void save_one(const struct pci_device *d, void *ctx)
{
    UNUSED(ctx);
    if (pci_count >= MAX_PCI)
        return;
    struct pci_saved *s = &pci_saved[pci_count++];
    s->d = *d;
    for (int i = 0; i < 16; i++)
        s->header[i] = pci_read(d, (uint8_t)(i * 4));
}

static bool pci_save(void)
{
    if (!pci_saved && !(pci_saved = kmalloc(sizeof(*pci_saved) * MAX_PCI)))
        return false;
    pci_count = 0;
    pci_foreach(save_one, NULL);
    return true;
}

static void pci_restore(void)
{
    for (int i = 0; i < pci_count; i++) {
        struct pci_saved *s = &pci_saved[i];
        if (pci_read(&s->d, 0) != s->header[0])
            continue;
        /* from the end down, the command register (offset 4) last, as Linux does; only what changed */
        for (int r = 15; r >= 1; r--)
            if (r != 2 && pci_read(&s->d, (uint8_t)(r * 4)) != s->header[r])
                pci_write(&s->d, (uint8_t)(r * 4), s->header[r]);
    }
}

/* ---- the legacy interrupt controllers */

static uint8_t pic_mask[2], elcr[2];

static void pic_save(void)
{
    pic_mask[0] = inb(0x21);
    pic_mask[1] = inb(0xa1);
    elcr[0] = inb(0x4d0);
    elcr[1] = inb(0x4d1);
}

static void pic_restore(void)
{
    pic_program(0x20, 0x28);
    outb(0x21, pic_mask[0]);
    outb(0xa1, pic_mask[1]);
    outb(0x4d0, elcr[0]);
    outb(0x4d1, elcr[1]);
}

/* ---- the model-specific registers the kernel sets after boot (64-bit programs' syscall) */

#ifdef __x86_64__
static const uint32_t msrs[] = {0xc0000080u, 0xc0000081u, 0xc0000082u, 0xc0000084u};
static uint64_t msr_values[ARRAY_SIZE(msrs)];

static void msr_save(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(msrs); i++) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msrs[i]));
        msr_values[i] = (uint64_t)hi << 32 | lo;
    }
}

static void msr_restore(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(msrs); i++)
        __asm__ volatile("wrmsr" : : "c"(msrs[i]), "a"((uint32_t)msr_values[i]), "d"((uint32_t)(msr_values[i] >> 32)));
}
#else
static void msr_save(void) {}
static void msr_restore(void) {}
#endif

bool hal_can_suspend(char *why, size_t cap)
{
    if (!acpi_s3_ready(why, cap) || !display_can_sleep(why, cap))
        return false;
    for (int i = 0; i < resume_count; i++)
        if (!resumes[i].fn) {
            ksnprintf(why, cap, "%s cannot be set up again after sleep", resumes[i].name);
            return false;
        }
    return true;
}

static void *wake_stack[1];

int hal_suspend(void)
{
    char why[96];
    if (!hal_can_suspend(why, sizeof(why))) {
        klog("sleep: no S3: %s", why);
        return E_NOSYS;
    }
    static uint8_t *fpu; /* from the heap, as the 32-bit kernel's image has no room */
    if (!wake_stack[0]) {
        uint8_t *s = kmalloc(8192 + ARCH_FPU_AREA + 64);
        if (!s)
            return E_NOMEM;
        wake_stack[0] = s + 8192;
        fpu = (uint8_t *)(((uintptr_t)s + 8192 + 63) & ~(uintptr_t)63);
    }
    if (!pci_save())
        return E_NOMEM;
    uint32_t page = smp_trampoline(acpi_wake_entry, wake_stack, 0, 1);
    if (!page || !acpi_s3_prepare(page))
        return E_IO;
    struct rtc_time before, after;
    hal_rtc(&before);
    klog("sleep: S3, waking vector %05x", page);
    uint32_t flags = hal_irq_save();
    pic_save();
    msr_save();
    arch_fpu_save(fpu);
    int woke = acpi_sleep_lowlevel(acpi_s3_enter);
    if (woke) {
        cpu_resume();
        msr_restore();
    }
    arch_fpu_restore(fpu);
    if (woke) {
        pic_restore();
        pit_resume();
        serial_init();
        pci_restore();
        mouse_init();
        display_resume();
    }
    acpi_s3_resume();
    if (!woke) {
        hal_irq_restore(flags);
        klog("sleep: the machine did not go to sleep");
        return E_IO;
    }
    hal_rtc(&after);
    uint32_t slept = time_from_rtc(&after) - time_from_rtc(&before);
    if (slept < 30u * 24 * 3600)
        pit_advance_ms(slept * 1000u);
    hal_irq_restore(flags);
    smp_resume();
    for (int i = 0; i < resume_count; i++)
        if (resumes[i].fn && !resumes[i].fn())
            klog("sleep: %s did not come back", resumes[i].name);
    klog("sleep: awake after %u s", (unsigned)slept);
    return 0;
}
