#include <synth/hal.h>

#include "io.h"

#define PIT_FREQUENCY 1193182u

static volatile uint64_t ticks;
static uint32_t tick_hz = 1000;

static void pit_tick(struct regs *r)
{
    UNUSED(r);
    ticks++;
}

void pit_init(uint32_t hz)
{
    tick_hz = hz;
    uint32_t divisor = PIT_FREQUENCY / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xff));
    outb(0x40, (uint8_t)(divisor >> 8));
    irq_install(0, pit_tick);
}

uint64_t hal_ticks_ms(void)
{
    uint64_t t;
    cpu_cli();
    t = ticks;
    cpu_sti();
    return t * 1000u / tick_hz;
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
