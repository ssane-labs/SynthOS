#include <synth/console.h>
#include <synth/hal.h>
#include <synth/input.h>

#define QUEUE_SIZE 64

static int queue[QUEUE_SIZE];
static unsigned head, tail;
static bool break_mode;
static volatile bool break_flag;
static volatile bool info_flag;
static void (*pump)(int timeout_ms);

void input_set_pump(void (*fn)(int timeout_ms))
{
    pump = fn;
}

static bool push(int key)
{
    if (key == KEY_RESIZE && !pump) {
        struct display *d = hal_display();
        con_resize(d->cols, d->rows);
    }
    if (break_mode && key == KEY_CTRL('t')) {
        info_flag = true;
        return false;
    }
    if (break_mode && key == KEY_CTRL('c')) {
        break_flag = true;
        return false;
    }
    if (tail - head >= QUEUE_SIZE)
        return false;
    queue[tail++ % QUEUE_SIZE] = key;
    return true;
}

void input_inject(int key)
{
    push(key);
}

bool input_poll(void)
{
    if (tail != head)
        return true;
    if (pump) {
        if (!hal_in_interrupt())
            pump(0);
        return tail != head;
    }
    for (;;) {
        int key = hal_key_read(0);
        if (key == KEY_NONE)
            return tail != head;
        push(key);
        if (tail != head)
            return true;
    }
}

int input_getkey(int timeout_ms)
{
    con_flush();
    uint64_t deadline = timeout_ms > 0 ? hal_ticks_ms() + (uint64_t)timeout_ms : 0;
    while (tail == head) {
        if (break_mode && break_flag)
            return KEY_NONE;
        int wait = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = hal_ticks_ms();
            if (now >= deadline)
                return KEY_NONE;
            wait = (int)(deadline - now);
        }
        if (pump) {
            pump(break_mode && wait < 0 ? 50 : wait);
            if (timeout_ms == 0 && tail == head)
                return KEY_NONE;
            continue;
        }
        int key = hal_key_read(break_mode && wait < 0 ? 50 : wait);
        if (key == KEY_NONE) {
            if (timeout_ms == 0)
                return KEY_NONE;
            continue;
        }
        push(key);
    }
    return queue[head++ % QUEUE_SIZE];
}

void input_unget(int key)
{
    if (tail - head < QUEUE_SIZE)
        queue[--head % QUEUE_SIZE] = key;
}

void input_set_break(bool enabled)
{
    break_mode = enabled;
    break_flag = false;
}

bool input_info_take(void)
{
    bool was = info_flag;
    info_flag = false;
    return was;
}

bool input_break_pending(void)
{
    return break_flag;
}

void input_clear_break(void)
{
    break_flag = false;
}
