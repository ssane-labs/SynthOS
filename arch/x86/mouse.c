#include <synth/hal.h>

#include "io.h"

#define QUEUE_SIZE 64

static volatile struct pointer_event queue[QUEUE_SIZE];
static volatile unsigned head, tail;
static uint8_t packet[4];
static int packet_len;
static int packet_size = 3;
static bool present;

static bool wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2))
            return true;
    return false;
}

static bool wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1)
            return true;
    return false;
}

static void controller(uint8_t cmd)
{
    wait_write();
    outb(0x64, cmd);
}

static int mouse_command(uint8_t cmd)
{
    controller(0xd4);
    wait_write();
    outb(0x60, cmd);
    if (!wait_read())
        return -1;
    return inb(0x60);
}

static void set_rate(uint8_t rate)
{
    mouse_command(0xf3);
    mouse_command(rate);
}

static void mouse_irq(struct regs *r)
{
    UNUSED(r);
    i8042_drain();
}

void mouse_byte(uint8_t data)
{
    if (packet_len == 0 && !(data & 0x08))
        return;
    packet[packet_len++] = data;
    if (packet_len < packet_size)
        return;
    packet_len = 0;
    if (packet[0] & 0xc0)
        return;
    struct pointer_event ev;
    ev.dx = packet[1] - ((packet[0] << 4) & 0x100);
    ev.dy = -(packet[2] - ((packet[0] << 3) & 0x100));
    ev.buttons = packet[0] & 0x07;
    ev.wheel = packet_size == 4 ? (int8_t)(packet[3] << 4) >> 4 : 0;
    unsigned last = tail - 1;
    if (tail != head && queue[last % QUEUE_SIZE].buttons == ev.buttons && !ev.wheel && !queue[last % QUEUE_SIZE].wheel) {
        queue[last % QUEUE_SIZE].dx += ev.dx;
        queue[last % QUEUE_SIZE].dy += ev.dy;
        return;
    }
    if (tail - head < QUEUE_SIZE)
        queue[tail++ % QUEUE_SIZE] = ev;
}

void mouse_init(void)
{
    cpu_cli();
    while (inb(0x64) & 1)
        inb(0x60);
    controller(0xa8);
    controller(0x20);
    uint8_t config = wait_read() ? inb(0x60) : 0;
    config = (uint8_t)((config | 0x02 | 0x01) & ~0x20);
    controller(0x60);
    wait_write();
    outb(0x60, config);
    present = mouse_command(0xf6) == 0xfa;
    if (present) {
        set_rate(200);
        set_rate(100);
        set_rate(80);
        if (mouse_command(0xf2) == 0xfa && wait_read() && inb(0x60) == 3)
            packet_size = 4;
        set_rate(60);
        mouse_command(0xf4);
        irq_install(12, mouse_irq);
    }
    while (inb(0x64) & 1)
        inb(0x60);
    cpu_sti();
}

bool hal_pointer_read(struct pointer_event *ev)
{
    if (head == tail)
        return false;
    cpu_cli();
    *ev = queue[head++ % QUEUE_SIZE];
    cpu_sti();
    return true;
}

bool hal_pointer_present(void)
{
    return present;
}
