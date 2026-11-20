/* Touchpads and touch screens: frames of finger contacts from any driver (HID multi-touch over I2C or USB,
   virtio-input) become pointer motion, clicks, scrolling and three-finger swipes. 64-bit kernel only. */
#ifndef SYNTH_TOUCH_H
#define SYNTH_TOUCH_H

#include <synth/types.h>

#define TOUCH_MAX 5

struct touch_contact {
    int id;
    int x, y; /* device units, 0..max */
};

struct touch_frame {
    int count;        /* fingers on the surface */
    struct touch_contact c[TOUCH_MAX];
    unsigned buttons; /* the pad's own click (button 1) */
};

/* what a pad turns into */
struct touch_out {
    void (*pointer)(int dx, int dy, int wheel, unsigned buttons);
    void (*absolute)(int x, int y, unsigned buttons); /* 0..65535, touch screens */
    void (*key)(int key);                             /* KEY_SWIPE_* */
};

struct touch_pad {
    int max_x, max_y;
    bool direct; /* a touch screen: one finger points where it touches */
    struct touch_out out;
    /* the engine's own state */
    int fingers, most;
    struct touch_contact last[TOUCH_MAX];
    int last_count;
    uint64_t began;
    int64_t travel;
    int64_t acc_x, acc_y, scroll, swipe_x, swipe_y;
    bool swiped, clicked, moved;
    unsigned buttons, sent;
};

void touch_frame(struct touch_pad *pad, const struct touch_frame *f, uint64_t now_ms);
/* the gestures recognised so far, for the log and tests: taps, scroll steps, swipes */
void touch_stats(char *buf, size_t cap);
/* a driver names its pad for the touchpad command (the name stays valid) */
void touch_add(const char *name, bool direct);

#endif
