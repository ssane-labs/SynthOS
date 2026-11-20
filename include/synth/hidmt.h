/* HID multi-touch devices (Windows Precision Touchpads and HID touch screens, over I2C or USB): their report
   descriptor says where the fingers are in an input report; the reports become frames for the gesture engine
   (synth/touch.h). 64-bit kernel only. */
#ifndef SYNTH_HIDMT_H
#define SYNTH_HIDMT_H

#include <synth/touch.h>

struct hid_field {
    uint16_t offset; /* bits after the report ID */
    uint8_t size;
    bool present;
};

/* a value the device must be given in a feature report (the input mode, the surface and button switches) */
struct hidmt_setting {
    uint8_t report;
    struct hid_field field;
    uint8_t value;
};

struct hidmt {
    uint8_t report;     /* the touch input report's ID, 0 without IDs */
    uint16_t bytes;     /* its length after the ID */
    bool direct;        /* a touch screen rather than a touchpad */
    int fingers;        /* contacts one report carries */
    struct {
        struct hid_field tip, confidence, id, x, y;
    } f[TOUCH_MAX];
    struct hid_field count, button;
    int min_x, min_y;
    struct hidmt_setting settings[3];
    int setting_count;
    uint16_t feature_bytes[3]; /* each setting's report length after the ID */
    /* a frame spread over several reports (hybrid mode): what came so far and how many contacts it has */
    struct touch_frame pending;
    int expected, received;
    struct touch_pad pad;
};

/* reads a report descriptor; false when it has no multi-touch collection */
bool hidmt_parse(struct hidmt *m, const uint8_t *desc, size_t length);
/* one input report (with its ID first when the device has IDs); other reports are left alone */
void hidmt_input(struct hidmt *m, const uint8_t *report, size_t length, uint64_t now_ms);
/* the i-th feature report that turns multi-touch on (ID first, then its fields); 0 when there is no such report */
int hidmt_setting_report(const struct hidmt *m, int i, uint8_t *buf, int cap);

#endif
