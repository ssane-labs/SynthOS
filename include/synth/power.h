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
/* closing the lid locks the screen and turns it off */
bool power_lid_locks(void);
/* in text mode, where no desktop takes them: does what a power key asks; false for any other key */
bool power_key_text(int key);

#endif
