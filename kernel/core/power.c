/* The power button and lid settings, and what the keys do in text mode (the desktop handles them itself). */
#include <synth/command.h>
#include <synth/console.h>
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/keys.h>
#include <synth/log.h>
#include <synth/power.h>
#include <synth/shell.h>
#include <synth/string.h>

static const char *const button_names[] = {"ask", "shutdown", "sleep", "nothing"};
static enum power_action button = POWER_ASK;
static const char *const low_names[] = {"off", "on", "battery"};
static int low_mode; /* index into low_names */

bool power_low(void)
{
    if (low_mode == 1)
        return true;
    return low_mode == 2 && hal_ac_online() == 0;
}

int power_battery_percent(bool *charging, int *minutes)
{
    struct battery_info b[2];
    int n = hal_batteries(b, 2);
    uint64_t now = 0, full = 0;
    *charging = false;
    *minutes = -1;
    for (int i = 0; i < n; i++) {
        now += b[i].now;
        full += b[i].full;
        *charging |= b[i].charging;
        if (b[i].minutes >= 0)
            *minutes = MAX(*minutes, 0) + b[i].minutes;
    }
    if (!n)
        return -1;
    if (!full)
        return b[0].percent;
    return (int)MIN((uint64_t)100, now * 100 / full);
}

enum power_note power_battery_check(char *text, size_t cap)
{
    static uint64_t last, deadline;
    static int warned = 100; /* the last level warned about while discharging */
    uint64_t t = hal_ticks_ms();
    if (t - last < 5000)
        return NOTE_NONE;
    last = t;
    struct battery_info b[2];
    int n = hal_batteries(b, 2);
    bool charging, critical = false, discharging = false;
    int minutes, pct = power_battery_percent(&charging, &minutes);
    for (int i = 0; i < n; i++) {
        critical |= b[i].critical || (b[i].low && b[i].now <= b[i].low);
        discharging |= b[i].discharging;
    }
    if (pct < 0 || !discharging || hal_ac_online() == 1) {
        warned = 100;
        deadline = 0;
        return NOTE_NONE;
    }
    if (critical || pct <= 3) {
        if (!deadline) {
            deadline = t + 60000;
            ksnprintf(text, cap, "The battery is almost empty (%d%%). Plug in the computer: it shuts down in a minute.",
                      pct);
            return NOTE_CRITICAL;
        }
        if (t >= deadline) {
            str_copy(text, "The battery is empty: shutting down.", cap);
            return NOTE_SHUTDOWN;
        }
        return NOTE_NONE;
    }
    deadline = 0;
    int level = pct <= 5 ? 5 : pct <= 10 ? 10 : 100;
    if (level >= warned)
        return NOTE_NONE;
    warned = level;
    if (minutes >= 0)
        ksnprintf(text, cap, "%d%% of the battery is left (about %d min). Plug in the computer soon.", pct, minutes);
    else
        ksnprintf(text, cap, "%d%% of the battery is left. Plug in the computer soon.", pct);
    return NOTE_LOW;
}

void power_text_tick(void)
{
    char text[128];
    switch (power_battery_check(text, sizeof(text))) {
    case NOTE_LOW:
    case NOTE_CRITICAL:
        kprintf("\nBattery: %s\n", text);
        break;
    case NOTE_SHUTDOWN:
        kprintf("\nBattery: %s\n", text);
        con_flush();
        hal_poweroff();
        break;
    default:
        break;
    }
}

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
static void get_low(char *buf, size_t cap)
{
    str_copy(buf, low_names[low_mode], cap);
}

static int set_low(struct shell *sh, const char *value)
{
    UNUSED(sh);
    for (int i = 0; i < 3; i++)
        if (!strcmp(value, low_names[i])) {
            low_mode = i;
            return 0;
        }
    return E_INVAL;
}

static const struct setting s_low = {"lowpower", "off|on|battery",
                                     "Low Power Mode: fewer frames and a dimmer screen (battery: only on battery)",
                                     get_low, set_low};
SETTING(s_low);
SETTING(s_button);
SETTING(s_lid);

static void watt_hours(char *buf, size_t cap, uint32_t mwh)
{
    ksnprintf(buf, cap, "%u.%u Wh", (unsigned)(mwh / 1000), (unsigned)(mwh % 1000 / 100));
}

COMMAND(battery, NULL, "system", "battery", "the charge of a laptop's batteries and whether it is plugged in")
{
    UNUSED(argc), UNUSED(argv);
    struct battery_info b[2];
    int n = hal_batteries(b, 2);
    for (int i = 0; i < n; i++) {
        sh_printf(sh, "%s%s%s%s: ", b[i].name, b[i].model[0] ? " (" : "", b[i].model, b[i].model[0] ? ")" : "");
        if (b[i].percent >= 0)
            sh_printf(sh, "%d%%, ", b[i].percent);
        sh_puts(sh, b[i].charging ? "charging" : b[i].discharging ? "discharging" : "not charging");
        if (b[i].minutes >= 0)
            sh_printf(sh, ", %d h %02d min %s", b[i].minutes / 60, b[i].minutes % 60,
                      b[i].charging ? "to full" : "left");
        sh_puts(sh, b[i].critical ? ", CRITICAL\n" : "\n");
        char now[24], full[24], design[24];
        watt_hours(now, sizeof(now), b[i].now);
        watt_hours(full, sizeof(full), b[i].full);
        watt_hours(design, sizeof(design), b[i].design);
        sh_printf(sh, "  %s of %s", now, full);
        if (b[i].design)
            sh_printf(sh, " (new: %s, health %u%%)", design, (unsigned)((uint64_t)b[i].full * 100 / b[i].design));
        if (b[i].rate)
            sh_printf(sh, ", %u.%u W", (unsigned)(b[i].rate / 1000), (unsigned)(b[i].rate % 1000 / 100));
        if (b[i].cycles)
            sh_printf(sh, ", %u cycles", (unsigned)b[i].cycles);
        sh_puts(sh, "\n");
    }
    if (!n)
        sh_puts(sh, "No battery\n");
    int ac = hal_ac_online();
    if (ac >= 0)
        sh_printf(sh, "AC adapter: %s\n", ac ? "plugged in" : "not plugged in");
    sh_printf(sh, "Low Power Mode: %s (%s)\n", power_low() ? "on" : "off", low_names[low_mode]);
    return 0;
}
