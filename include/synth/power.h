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
/* Low Power Mode (config lowpower off|on|battery): the desktop draws at most 30 frames a second and the screen is a
   quarter dimmer */
bool power_low(void);

/* the batteries together: percent of a full charge, -1 without a battery */
int power_battery_percent(bool *charging, int *minutes);

enum power_note {
    NOTE_NONE,
    NOTE_LOW,      /* 10 % and 5 % left */
    NOTE_CRITICAL, /* the firmware's critical level: a minute to plug in */
    NOTE_SHUTDOWN, /* that minute is over */
};
/* what the battery asks for (looked at every few seconds at most), and the words to tell the user */
enum power_note power_battery_check(char *text, size_t cap);
/* text mode's battery watch, from the console's wait for keys */
void power_text_tick(void);

/* text mode's sleep: S3 when the machine can, else the console waits, idle, for a key; 0 after an S3 sleep */
int power_sleep_text(void);
/* in text mode, where no desktop takes them: does what a power key asks; false for any other key */
bool power_key_text(int key);

#endif
