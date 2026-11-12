#include <synth/command.h>
#include <synth/config.h>
#include <synth/fmt.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/time.h>

#include "internal.h"

/* Focus: for a number of minutes notifications stay quiet, the Dock hides, the menu bar counts down and the apps
   named to block do not open. When the time is up everything is as before and a notification says so. */

static uint32_t focus_end, focus_length;
static char saved_dnd[8], saved_dock[16], blocked[160];

static void current(const char *name, char *buf, size_t cap)
{
    const struct setting *st = setting_find(name);
    buf[0] = 0;
    if (st)
        st->get(buf, cap);
}

int focus_minutes_left(void)
{
    uint32_t now = time_now();
    return focus_end && now < focus_end ? (int)((focus_end - now + 59) / 60) : 0;
}

static void focus_stop(bool done)
{
    if (!focus_end)
        return;
    focus_end = 0;
    blocked[0] = 0;
    setting_apply(shell_current(), "dnd", saved_dnd[0] ? saved_dnd : "off");
    setting_apply(shell_current(), "dockmode", saved_dock[0] ? saved_dock : "show");
    chrome_changed();
    if (done) {
        char text[96];
        ksnprintf(text, sizeof(text), "%u minutes of focus. Time for a break.", (unsigned)(focus_length / 60));
        gui_notify("Focus session done", text);
    }
}

void focus_start(int minutes, const char *block)
{
    if (!focus_end) {
        current("dnd", saved_dnd, sizeof(saved_dnd));
        current("dockmode", saved_dock, sizeof(saved_dock));
    }
    focus_length = (uint32_t)minutes * 60;
    focus_end = time_now() + focus_length;
    str_copy(blocked, block ? block : "", sizeof(blocked));
    setting_apply(shell_current(), "dnd", "on");
    if (strcmp(saved_dock, "off"))
        setting_apply(shell_current(), "dockmode", "autohide");
    chrome_changed();
}

void focus_tick(void)
{
    if (focus_end && time_now() >= focus_end)
        focus_stop(true);
}

/* true when the app has to wait for the end of the session */
bool focus_blocks(const char *app)
{
    if (!focus_minutes_left() || !blocked[0] || !app)
        return false;
    size_t n = strlen(app);
    for (const char *p = blocked; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !str_ncasecmp(p, app, n))
            return true;
        p += len + (e ? 1 : 0);
    }
    return false;
}

COMMAND(focus, NULL, "system", "focus [<minutes> [block <app>,<app>...] | stop]",
        "a focus session: quiet notifications, a hidden Dock, a countdown in the menu bar and blocked apps")
{
    if (argc == 1) {
        int left = focus_minutes_left();
        if (!left)
            sh_puts(sh, "no focus session; start one with focus 25\n");
        else
            sh_printf(sh, "Focus: %d min left%s%s\n", left, blocked[0] ? ", blocked: " : "", blocked);
        return 0;
    }
    if (!strcmp(argv[1], "stop")) {
        focus_stop(false);
        return 0;
    }
    long minutes = str_to_long(argv[1], NULL, 10);
    if (minutes < 1 || minutes > 600)
        return sh_error(sh, "focus", "say how many minutes, 1 to 600");
    char list[160] = "";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "block"))
            continue;
        if (list[0])
            str_append(list, ",", sizeof(list));
        str_append(list, argv[i], sizeof(list));
    }
    focus_start((int)minutes, list);
    sh_printf(sh, "Focus for %ld minutes%s%s\n", minutes, list[0] ? "; blocked: " : "", list);
    return 0;
}
