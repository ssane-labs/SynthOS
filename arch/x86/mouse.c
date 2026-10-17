#include <synth/hal.h>

#include "io.h"
#include "usb.h"

#define QUEUE_SIZE 64

static volatile struct pointer_event queue[QUEUE_SIZE];
static volatile unsigned head, tail;
static uint8_t packet[4];
static int packet_len;
static int packet_size = 3;
static bool present;
static volatile bool usb_mouse;
static void (*hook)(struct pointer_event *ev);

void hal_pointer_hook(void (*fn)(struct pointer_event *ev))
{
    uint32_t flags = cpu_irq_save();
    hook = fn;
    cpu_irq_restore(flags);
}

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

static void queue_event(struct pointer_event *ev)
{
    if (hook)
        hook(ev);
    unsigned last = tail - 1;
    volatile struct pointer_event *prev = &queue[last % QUEUE_SIZE];
    if (tail != head && prev->buttons == ev->buttons && !ev->wheel && !prev->wheel &&
        prev->absolute == ev->absolute) {
        prev->dx += ev->dx;
        prev->dy += ev->dy;
        prev->x = ev->x;
        prev->y = ev->y;
        prev->placed = ev->placed;
        return;
    }
    if (tail - head < QUEUE_SIZE)
        queue[tail++ % QUEUE_SIZE] = *ev;
}

void mouse_report(int dx, int dy, int wheel, unsigned buttons)
{
    usb_mouse = true;
    struct pointer_event ev = {.dx = dx, .dy = dy, .wheel = wheel, .buttons = buttons};
    uint32_t flags = cpu_irq_save();
    queue_event(&ev);
    cpu_irq_restore(flags);
}

void mouse_absolute(int x, int y, int wheel, unsigned buttons)
{
    usb_mouse = true;
    struct pointer_event ev = {.wheel = wheel, .buttons = buttons, .absolute = true, .x = x, .y = y};
    uint32_t flags = cpu_irq_save();
    queue_event(&ev);
    cpu_irq_restore(flags);
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
    struct pointer_event ev = {0};
    ev.dx = packet[1] - ((packet[0] << 4) & 0x100);
    ev.dy = -(packet[2] - ((packet[0] << 3) & 0x100));
    ev.buttons = packet[0] & 0x07;
    ev.wheel = packet_size == 4 ? (int8_t)(packet[3] << 4) >> 4 : 0;
    queue_event(&ev);
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
    uint32_t flags = cpu_irq_save();
    *ev = queue[head++ % QUEUE_SIZE];
    cpu_irq_restore(flags);
    return true;
}

bool keyboard_pending(void);

bool hal_input_pending(void)
{
    return head != tail || keyboard_pending();
}

bool hal_pointer_present(void)
{
    return present || usb_mouse;
}
