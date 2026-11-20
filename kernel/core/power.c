/* The power button and lid settings, and what the keys do in text mode (the desktop handles them itself). */
#include <synth/console.h>
#include <synth/error.h>
#include <synth/hal.h>
#include <synth/keys.h>
#include <synth/log.h>
#include <synth/power.h>
#include <synth/shell.h>
#include <synth/string.h>

static const char *const button_names[] = {"ask", "shutdown", "sleep", "nothing"};
static enum power_action button = POWER_ASK;
static bool lid_lock = true;

enum power_action power_button_action(void)
{
    return button;
}

bool power_lid_locks(void)
{
    return lid_lock;
}

bool power_key_text(int key)
{
    switch (key) {
    case KEY_POWER:
        if (button == POWER_NOTHING)
            return true;
        if (button == POWER_SLEEP) {
            kprintf("\nPower button: sleep is not available yet\n");
            return true;
        }
        /* nobody to ask on a text console: the button shuts down, as on a server */
        kprintf("\nPower button: shutting down\n");
        con_flush();
        hal_poweroff();
        return true;
    case KEY_SLEEP:
        kprintf("\nSleep button: sleep is not available yet\n");
        return true;
    case KEY_LID_CLOSE:
    case KEY_LID_OPEN:
        return true;
    default:
        return false;
    }
}

static void get_button(char *buf, size_t cap)
{
    str_copy(buf, button_names[button], cap);
}

static int set_button(struct shell *sh, const char *value)
{
    UNUSED(sh);
    for (int i = 0; i < 4; i++)
        if (!strcmp(value, button_names[i])) {
            button = (enum power_action)i;
            return 0;
        }
    return E_INVAL;
}

static void get_lid(char *buf, size_t cap)
{
    str_copy(buf, lid_lock ? "lock" : "nothing", cap);
}

static int set_lid(struct shell *sh, const char *value)
{
    UNUSED(sh);
    if (strcmp(value, "lock") && strcmp(value, "nothing"))
        return E_INVAL;
    lid_lock = !strcmp(value, "lock");
    return 0;
}

static const struct setting s_button = {"powerbutton", "ask|shutdown|sleep|nothing",
                                        "what pressing the power button does", get_button, set_button};
static const struct setting s_lid = {"lidclose", "lock|nothing",
                                     "closing the lid locks the screen and turns it off", get_lid, set_lid};
SETTING(s_button);
SETTING(s_lid);
