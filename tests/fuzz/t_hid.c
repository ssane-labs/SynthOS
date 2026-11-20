/* the HID multi-touch parser: a report descriptor (its length in the first two bytes), then input reports of 16
   bytes, which go through the gesture engine */
#include <synth/hidmt.h>

#include "targets.h"

static void pointer(int dx, int dy, int wheel, unsigned buttons)
{
    (void)dx, (void)dy, (void)wheel, (void)buttons;
}

static void absolute(int x, int y, unsigned buttons)
{
    (void)x, (void)y, (void)buttons;
}

static void key(int k)
{
    (void)k;
}

void fuzz_hid(const uint8_t *d, size_t n)
{
    static struct hidmt m;
    if (n < 2)
        return;
    size_t len = (size_t)(d[0] | d[1] << 8);
    if (len > n - 2)
        len = n - 2;
    if (!hidmt_parse(&m, d + 2, len))
        return;
    uint8_t buf[64];
    for (int i = 0; hidmt_setting_report(&m, i, buf, sizeof(buf)) > 0 && i < 8; i++)
        ;
    m.pad.out = (struct touch_out){pointer, absolute, key};
    uint64_t t = 0;
    for (size_t at = 2 + len; at < n; at += 16, t += 7)
        hidmt_input(&m, d + at, n - at < 16 ? n - at : 16, t);
}
