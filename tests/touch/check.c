/* The HID multi-touch parser and the gesture engine on the host (`make test-touch`): a Precision Touchpad report
   descriptor laid out as in Microsoft's sample (five fingers, a mouse collection and the configuration features)
   and a touch screen in hybrid mode (two fingers a report), then taps, scrolling and swipes as input reports. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <synth/hidmt.h>
#include <synth/keys.h>

/* what touch.c calls in the kernel */
void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

struct shell;
void sh_puts(struct shell *sh, const char *s)
{
    (void)sh;
    fputs(s, stdout);
}

int sh_printf(struct shell *sh, const char *fmt, ...)
{
    (void)sh;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

#define FINGER                                                                                                       \
    0x09, 0x22, 0xa1, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x09, 0x47, 0x81, 0x02, 0x09, 0x42, 0x81, \
        0x02, 0x95, 0x06, 0x81, 0x03, 0x75, 0x08, 0x95, 0x01, 0x25, 0x0f, 0x09, 0x51, 0x81, 0x02, 0x05, 0x01, 0x75,  \
        0x10, 0x26, 0xb0, 0x04, 0x09, 0x30, 0x81, 0x02, 0x26, 0x20, 0x03, 0x09, 0x31, 0x81, 0x02, 0x05, 0x0d, 0xc0

static const uint8_t touchpad[] = {
    /* the configuration first, so that its finger collections come before the touch ones */
    0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75,
    0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0, 0x09, 0x22, 0xa1, 0x00, 0x85, 0x04, 0x09, 0x57, 0x09, 0x58, 0x75, 0x01, 0x95,
    0x02, 0x25, 0x01, 0xb1, 0x02, 0x95, 0x06, 0xb1, 0x03, 0xc0, 0xc0,
    /* the touch pad: five fingers, scan time, contact count, the button */
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, FINGER, FINGER, FINGER, FINGER, FINGER, 0x55, 0x0c, 0x66, 0x01,
    0x10, 0x47, 0xff, 0xff, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x09, 0x56, 0x81, 0x02,
    0x09, 0x54, 0x25, 0x7f, 0x75, 0x08, 0x81, 0x02, 0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81,
    0x02, 0x95, 0x07, 0x81, 0x03, 0xc0,
    /* the mouse the pad is before Windows turns multi-touch on */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15,
    0x00, 0x25, 0x01, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
};

#define SCREEN_FINGER                                                                                                \
    0x09, 0x22, 0xa1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, \
        0x03, 0x09, 0x51, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x26, 0xff, 0x0f, 0x75, 0x10,  \
        0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0x05, 0x0d, 0xc0

static const uint8_t screen[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x05, SCREEN_FINGER, SCREEN_FINGER, 0x09, 0x54, 0x25, 0x0a, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x02, 0xc0, 0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x06, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0a,
    0x75, 0x08, 0x95, 0x01, 0xb1, 0x02, 0xc0,
};

static int failures;
static int clicks[8], wheel, last_key, presses, releases;
static unsigned held;

#define CHECK(what, cond)                                                                                            \
    do {                                                                                                             \
        if (cond)                                                                                                    \
            printf("ok   %s\n", what);                                                                               \
        else {                                                                                                       \
            printf("FAIL %s (line %d)\n", what, __LINE__);                                                           \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static void pointer(int dx, int dy, int w, unsigned buttons)
{
    (void)dx;
    (void)dy;
    wheel += w;
    for (int b = 0; b < 3; b++)
        if ((buttons & ~held) >> b & 1)
            clicks[b]++;
    held = buttons;
}

static void absolute(int x, int y, unsigned buttons)
{
    (void)x;
    (void)y;
    if (buttons & ~held & 1)
        presses++;
    if (held & ~buttons & 1)
        releases++;
    held = buttons;
}

static void key(int k)
{
    last_key = k;
}

/* a touchpad report: the fingers (x, y; id is the index), all in the first report */
static void pad_report(struct hidmt *m, int n, const int (*xy)[2], int button, uint64_t t)
{
    uint8_t r[35] = {1};
    for (int i = 0; i < n; i++) {
        uint8_t *f = r + 1 + 6 * i;
        f[0] = 3; /* confident, touching */
        f[1] = (uint8_t)i;
        f[2] = (uint8_t)xy[i][0], f[3] = (uint8_t)(xy[i][0] >> 8);
        f[4] = (uint8_t)xy[i][1], f[5] = (uint8_t)(xy[i][1] >> 8);
    }
    r[33] = (uint8_t)n;
    r[34] = (uint8_t)button;
    hidmt_input(m, r, sizeof(r), t);
}

static void screen_report(struct hidmt *m, int count, int first, int n, const int (*xy)[2], uint64_t t)
{
    uint8_t r[15] = {5};
    for (int i = 0; i < n; i++) {
        uint8_t *f = r + 1 + 6 * i;
        f[0] = 1;
        f[1] = (uint8_t)(first + i);
        f[2] = (uint8_t)xy[first + i][0], f[3] = (uint8_t)(xy[first + i][0] >> 8);
        f[4] = (uint8_t)xy[first + i][1], f[5] = (uint8_t)(xy[first + i][1] >> 8);
    }
    r[13] = (uint8_t)count;
    hidmt_input(m, r, sizeof(r), t);
}

int main(void)
{
    static struct hidmt m;
    CHECK("touchpad descriptor", hidmt_parse(&m, touchpad, sizeof(touchpad)));
    CHECK("five fingers in report 1", m.fingers == 5 && m.report == 1 && !m.direct);
    CHECK("pad size", m.pad.max_x == 1200 && m.pad.max_y == 800);
    CHECK("report length", m.bytes == 34);
    uint8_t buf[16];
    int n = hidmt_setting_report(&m, 0, buf, sizeof(buf));
    CHECK("input mode 3", n == 2 && buf[0] == 3 && buf[1] == 3);
    n = hidmt_setting_report(&m, 1, buf, sizeof(buf));
    CHECK("surface and button switches", n == 2 && buf[0] == 4 && buf[1] == 3);
    CHECK("two setting reports", hidmt_setting_report(&m, 2, buf, sizeof(buf)) == 0);
    m.pad.out = (struct touch_out){pointer, absolute, key};

    int one[1][2] = {{600, 400}};
    pad_report(&m, 1, one, 0, 1000);
    pad_report(&m, 0, one, 0, 1080);
    CHECK("one-finger tap is a left click", clicks[0] == 1 && held == 0);
    int two[2][2] = {{500, 400}, {700, 400}};
    pad_report(&m, 2, two, 0, 2000);
    pad_report(&m, 0, two, 0, 2100);
    CHECK("two-finger tap is a right click", clicks[1] == 1 && held == 0);
    pad_report(&m, 2, two, 0, 3000);
    for (int i = 1; i <= 10; i++) {
        int moved[2][2] = {{500, 400 + 40 * i}, {700, 400 + 40 * i}};
        pad_report(&m, 2, moved, 0, 3000 + 20 * (uint64_t)i);
    }
    pad_report(&m, 0, two, 0, 3300);
    CHECK("two fingers scroll 12 steps", wheel == 12);
    CHECK("no click after scrolling", clicks[1] == 1);
    pad_report(&m, 1, one, 1, 4000);
    pad_report(&m, 0, one, 0, 4100);
    CHECK("the pad's button clicks", clicks[0] == 2 && held == 0);
    int three[3][2] = {{300, 400}, {500, 400}, {700, 400}};
    pad_report(&m, 3, three, 0, 5000);
    for (int i = 1; i <= 10; i++) {
        int moved[3][2] = {{300 + 30 * i, 400}, {500 + 30 * i, 400}, {700 + 30 * i, 400}};
        pad_report(&m, 3, moved, 0, 5000 + 20 * (uint64_t)i);
    }
    pad_report(&m, 0, three, 0, 5300);
    CHECK("three fingers swiped right", last_key == KEY_SWIPE_RIGHT);
    CHECK("no tap after the swipe", clicks[2] == 0);
    pad_report(&m, 3, three, 0, 6000);
    pad_report(&m, 0, three, 0, 6100);
    CHECK("three-finger tap is a middle click", clicks[2] == 1);
    uint8_t mouse[4] = {2, 1, 5, 5};
    hidmt_input(&m, mouse, sizeof(mouse), 7000);
    CHECK("mouse reports are left alone", clicks[0] == 2 && held == 0);

    static struct hidmt s;
    CHECK("touch screen descriptor", hidmt_parse(&s, screen, sizeof(screen)));
    CHECK("two fingers a report, hybrid", s.fingers == 2 && s.direct && s.count.present && s.report == 5);
    n = hidmt_setting_report(&s, 0, buf, sizeof(buf));
    CHECK("input mode 2 for a screen", n == 2 && buf[0] == 6 && buf[1] == 2);
    s.pad.out = (struct touch_out){pointer, absolute, key};
    held = 0;
    int touch[1][2] = {{2048, 2048}};
    screen_report(&s, 1, 0, 1, touch, 1000);
    CHECK("a touch presses", presses == 1 && held == 1);
    screen_report(&s, 0, 0, 0, touch, 1050);
    CHECK("lifting releases", releases == 1 && held == 0);
    /* three fingers over two reports each frame, moving up */
    last_key = 0;
    for (int i = 0; i <= 10; i++) {
        int pts[3][2] = {{1000, 3000 - 150 * i}, {2000, 3000 - 150 * i}, {3000, 3000 - 150 * i}};
        screen_report(&s, 3, 0, 2, pts, 2000 + 20 * (uint64_t)i);
        if (i == 0)
            CHECK("half a frame is not reported", s.pad.fingers == 0);
        screen_report(&s, 0, 2, 1, pts, 2000 + 20 * (uint64_t)i);
    }
    CHECK("frames over two reports have three fingers", s.pad.fingers == 3);
    screen_report(&s, 0, 0, 0, touch, 2300);
    CHECK("three fingers swiped up", last_key == KEY_SWIPE_UP);
    CHECK("all fingers lifted", s.pad.fingers == 0);
    printf("touch tests: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
