/* What the power button, the sleep button and the lid of a laptop do (settings "powerbutton" and "lidclose"). */
#ifndef SYNTH_POWER_H
#define SYNTH_POWER_H

#include <synth/types.h>

enum power_action {
    POWER_ASK,
    POWER_SHUTDOWN,
    POWER_SLEEP,
    POWER_NOTHING,
};

enum power_action power_button_action(void);
enum lid_action {
    LID_LOCK, /* lock the screen and turn it off */
    LID_SLEEP,
    LID_NOTHING,
};

enum lid_action power_lid_action(void);
/* text mode's sleep: S3 when the machine can, else the console waits, idle, for a key; 0 after an S3 sleep */
int power_sleep_text(void);
/* in text mode, where no desktop takes them: does what a power key asks; false for any other key */
bool power_key_text(int key);

#endif
