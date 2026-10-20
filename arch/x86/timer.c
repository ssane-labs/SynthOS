#include <synth/hal.h>

#include "io.h"
#include "usb.h"

#define PIT_FREQUENCY 1193182u

static volatile uint64_t ticks;
static uint32_t tick_hz = 1000;

static void (*hooks[4])(void);
static int hook_count;

void timer_hook(void (*fn)(void))
{
    if (hook_count < (int)ARRAY_SIZE(hooks))
        hooks[hook_count++] = fn;
}

static uint32_t *prof_buf;
static volatile uint32_t prof_len, prof_cap;
static bool prof_callers;
extern const uint8_t __text_end[];

void hal_profile_callers(bool on)
{
    prof_callers = on;
}

/* For a sample in kernel code: the first return address on the interrupted stack, which names the caller of a
   leaf function such as memcpy. */
static uint32_t caller_of(struct regs *r)
{
    uint32_t end = (uint32_t)__text_end;
    if (r->eip < 0x100000u || r->eip >= end)
        return r->eip;
    const uint32_t *sp = &r->eflags + 1;
    for (int i = 0; i < 24; i++)
        if (sp[i] >= 0x100000u && sp[i] < end)
            return sp[i];
    return r->eip;
}

void hal_profile_start(uint32_t *buf, uint32_t cap)
{
    prof_len = 0;
    prof_cap = cap;
    prof_buf = buf;
}

uint32_t hal_profile_stop(void)
{
    prof_buf = NULL;
    return prof_len;
}

static void pit_tick(struct regs *r)
{
    if (prof_buf && prof_len < prof_cap)
        prof_buf[prof_len++] = prof_callers ? caller_of(r) : r->eip;
    ticks++;
    if (ticks * 1000u / tick_hz % 2 == 0)
        for (int i = 0; i < hook_count; i++)
            hooks[i]();
}

static void pit_divisor(uint32_t divisor)
{
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xff));
    outb(0x40, (uint8_t)(divisor >> 8));
}

void pit_init(uint32_t hz)
{
    tick_hz = hz;
    pit_divisor(PIT_FREQUENCY / hz);
    irq_install(0, pit_tick);
}

void pit_bios_mode(bool bios)
{
    pit_divisor(bios ? 0 : PIT_FREQUENCY / tick_hz);
}

void pit_advance_ms(uint32_t ms)
{
    ticks += (uint64_t)ms * tick_hz / 1000u;
}

uint64_t hal_ticks_ms(void)
{
    const volatile uint32_t *half = (const volatile uint32_t *)&ticks;
    uint32_t hi, lo;
    do {
        hi = half[1];
        lo = half[0];
    } while (hi != half[1]);
    uint64_t t = (uint64_t)hi << 32 | lo;
    return tick_hz == 1000 ? t : t * 1000u / tick_hz;
}

void hal_sleep_ms(uint32_t ms)
{
    uint64_t end = hal_ticks_ms() + ms;
    while (hal_ticks_ms() < end)
        cpu_halt();
}

void hal_beep(uint32_t hz, uint32_t ms)
{
    if (hz < 20)
        hz = 20;
    uint32_t divisor = PIT_FREQUENCY / hz;
    outb(0x43, 0xb6);
    outb(0x42, (uint8_t)(divisor & 0xff));
    outb(0x42, (uint8_t)(divisor >> 8));
    outb(0x61, inb(0x61) | 3);
    hal_sleep_ms(ms);
    outb(0x61, inb(0x61) & 0xfc);
}

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd(uint8_t v, bool binary)
{
    return binary ? v : (uint8_t)((v >> 4) * 10 + (v & 0x0f));
}

void hal_rtc(struct rtc_time *t)
{
    while (cmos(0x0a) & 0x80)
        ;
    uint8_t status = cmos(0x0b);
    bool binary = status & 0x04;
    uint8_t hour = cmos(0x04);
    bool pm = !(status & 0x02) && (hour & 0x80);
    t->second = bcd(cmos(0x00), binary);
    t->minute = bcd(cmos(0x02), binary);
    t->hour = bcd(hour & 0x7f, binary);
    t->day = bcd(cmos(0x07), binary);
    t->month = bcd(cmos(0x08), binary);
    t->year = (uint16_t)(2000 + bcd(cmos(0x09), binary));
    if (pm)
        t->hour = (uint8_t)((t->hour + 12) % 24);
}

uint16_t pit_counter(void)
{
    outb(0x43, 0x00);
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(lo | hi << 8);
}
