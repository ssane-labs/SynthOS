/* The power button and lid settings, and what the keys do in text mode (the desktop handles them itself). */
#include <synth/command.h>
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
static const char *const lid_names[] = {"lock", "sleep", "nothing"};
static enum lid_action lid = LID_LOCK;

enum power_action power_button_action(void)
{
    return button;
}

enum lid_action power_lid_action(void)
{
    return lid;
}

int power_sleep_text(void)
{
    char why[96];
    if (hal_can_suspend(why, sizeof(why))) {
        kprintf("Sleeping (S3)...\n");
        con_flush();
        int r = hal_suspend();
        if (r == 0) {
            con_redraw();
            kprintf("Awake.\n");
            return 0;
        }
        kprintf("The machine did not go to sleep (%s).\n", err_str(r));
    } else {
        kprintf("No S3 sleep: %s.\n", why);
    }
    /* the fallback: nothing runs but the interrupts until a key (or the power button, or the lid opening) */
    kprintf("Idle until a key is pressed...\n");
    con_flush();
    while (hal_key_read(1000) == KEY_NONE)
        ;
    return E_NOSYS;
}

bool power_key_text(int key)
{
    switch (key) {
    case KEY_POWER:
        if (button == POWER_NOTHING)
            return true;
        if (button == POWER_SLEEP) {
            power_sleep_text();
            return true;
        }
        /* nobody to ask on a text console: the button shuts down, as on a server */
        kprintf("\nPower button: shutting down\n");
        con_flush();
        hal_poweroff();
        return true;
    case KEY_SLEEP:
        power_sleep_text();
        return true;
    case KEY_LID_CLOSE:
        if (lid == LID_SLEEP)
            power_sleep_text();
        return true;
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
    str_copy(buf, lid_names[lid], cap);
}

static int set_lid(struct shell *sh, const char *value)
{
    UNUSED(sh);
    for (int i = 0; i < 3; i++)
        if (!strcmp(value, lid_names[i])) {
            lid = (enum lid_action)i;
            return 0;
        }
    return E_INVAL;
}

static const struct setting s_button = {"powerbutton", "ask|shutdown|sleep|nothing",
                                        "what pressing the power button does", get_button, set_button};
static const struct setting s_lid = {"lidclose", "lock|sleep|nothing",
                                     "closing the lid locks the screen and turns it off, or sleeps", get_lid, set_lid};

COMMAND(suspend, "", "system", "suspend [-n]", "sleep (S3, suspend to RAM) until a key, the power button or the lid; "
                                                "-n only says whether the machine can")
{
    if (argc > 1 && !strcmp(argv[1], "-n")) {
        char why[96];
        if (hal_can_suspend(why, sizeof(why))) {
            kprintf("S3 sleep is available\n");
            return 0;
        }
        sh_error(sh, "suspend", "%s", why);
        return 1;
    }
    if (argc > 1)
        return command_usage(sh, command_find("suspend"));
    return power_sleep_text() == 0 ? 0 : 1;
}
SETTING(s_button);
SETTING(s_lid);
