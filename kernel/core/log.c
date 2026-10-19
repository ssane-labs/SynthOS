#include <synth/config.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/log.h>
#include <synth/shell.h>
#include <synth/string.h>

static char ring[CONFIG_LOG_SIZE];
static size_t ring_len;
static bool to_serial;

static void ring_append(const char *s, size_t n)
{
    if (n >= sizeof(ring)) {
        s += n - sizeof(ring) + 1;
        n = sizeof(ring) - 1;
    }
    if (ring_len + n >= sizeof(ring)) {
        size_t drop = ring_len + n - sizeof(ring) + 1;
        const char *nl = memchr(ring + drop, '\n', ring_len - drop);
        if (nl)
            drop = (size_t)(nl - ring) + 1;
        memmove(ring, ring + drop, ring_len - drop);
        ring_len -= drop;
    }
    memcpy(ring + ring_len, s, n);
    ring_len += n;
}

void klog(const char *fmt, ...)
{
    char line[256];
    uint64_t ms = hal_ticks_ms();
    int n = ksnprintf(line, sizeof(line), "[%5u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    n += kvsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    size_t len = strnlen(line, sizeof(line) - 2);
    line[len++] = '\n';
    ring_append(line, len);
    if (to_serial)
        hal_debug_write(line, len);
}

/* config logserial on: the kernel log also goes to the first serial port, to follow the desktop from outside. */
static void get_logserial(char *buf, size_t cap)
{
    str_copy(buf, to_serial ? "on" : "off", cap);
}

static int set_logserial(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "on") && strcmp(value, "off"))
        return E_INVAL;
    to_serial = !strcmp(value, "on");
    return 0;
}

static const struct setting s_logserial = {"logserial", "on|off", "copy the kernel log to the serial port",
                                           get_logserial, set_logserial};
SETTING(s_logserial);

size_t log_snapshot(char *buf, size_t cap)
{
    size_t n = MIN(ring_len, cap ? cap - 1 : 0);
    memcpy(buf, ring + ring_len - n, n);
    if (cap)
        buf[n] = 0;
    return n;
}

void panic(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    klog("PANIC: %s", msg);
    con_set_attr(ATTR(COLOR_WHITE, COLOR_RED));
    kprintf("\n *** SynthOS kernel panic *** \n");
    con_reset();
    kprintf(" %s\n System halted. Press any key to reboot.\n", msg);
    con_flush();
    while (hal_key_read(-1) == 0)
        ;
    hal_reboot();
}
